#include <control/transports/chardev/channel.h>

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/wait.h>

#define CHANNEL_FIFO_SIZE (64U * 1024U)

struct chardev_channel {
	struct kref refcount;
	struct kfifo rx_fifo;
	struct kfifo tx_fifo;
	struct mutex read_lock;
	struct mutex write_lock;
	wait_queue_head_t rx_data_wait;
	wait_queue_head_t rx_space_wait;
	wait_queue_head_t tx_data_wait;
	wait_queue_head_t tx_space_wait;
	atomic_t shut_down;
	struct completion session_closed;
};

static void channel_release(struct kref *refcount)
{
	struct chardev_channel *channel = container_of(refcount, struct chardev_channel, refcount);

	kfifo_free(&channel->tx_fifo);
	kfifo_free(&channel->rx_fifo);
	kfree(channel);
}

struct chardev_channel *channel_create(void)
{
	struct chardev_channel *channel;
	int ret;

	channel = kzalloc(sizeof(*channel), GFP_KERNEL);
	if (!channel)
		return NULL;
	ret = kfifo_alloc(&channel->rx_fifo, CHANNEL_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		goto free_channel;
	ret = kfifo_alloc(&channel->tx_fifo, CHANNEL_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		goto free_rx;

	kref_init(&channel->refcount);
	mutex_init(&channel->read_lock);
	mutex_init(&channel->write_lock);
	init_waitqueue_head(&channel->rx_data_wait);
	init_waitqueue_head(&channel->rx_space_wait);
	init_waitqueue_head(&channel->tx_data_wait);
	init_waitqueue_head(&channel->tx_space_wait);
	atomic_set(&channel->shut_down, 0);
	init_completion(&channel->session_closed);
	return channel;

free_rx:
	kfifo_free(&channel->rx_fifo);
free_channel:
	kfree(channel);
	return NULL;
}

void channel_get(struct chardev_channel *channel) { kref_get(&channel->refcount); }

void channel_put(struct chardev_channel *channel) { kref_put(&channel->refcount, channel_release); }

static bool channel_is_shutdown(const struct chardev_channel *channel)
{
	return atomic_read(&channel->shut_down) != 0;
}

ssize_t channel_stream_read(struct chardev_channel *channel, void *buffer, size_t size)
{
	unsigned int copied;
	int ret;

	for (;;) {
		copied = kfifo_out(&channel->rx_fifo, buffer, size);
		if (copied) {
			wake_up_interruptible(&channel->rx_space_wait);
			return copied;
		}
		if (channel_is_shutdown(channel))
			return 0;
		ret = wait_event_interruptible(channel->rx_data_wait,
									   channel_is_shutdown(channel) || !kfifo_is_empty(&channel->rx_fifo));
		if (ret)
			return ret;
	}
}

ssize_t channel_stream_write(struct chardev_channel *channel, const void *buffer, size_t size)
{
	unsigned int copied;
	int ret;

	for (;;) {
		if (channel_is_shutdown(channel))
			return -EPIPE;
		copied = kfifo_in(&channel->tx_fifo, buffer, size);
		if (copied) {
			wake_up_interruptible(&channel->tx_data_wait);
			return copied;
		}
		ret = wait_event_interruptible(channel->tx_space_wait,
									   channel_is_shutdown(channel) || !kfifo_is_full(&channel->tx_fifo));
		if (ret)
			return ret;
	}
}

ssize_t channel_user_read(struct chardev_channel *channel, char __user *buffer, size_t size, bool nonblock)
{
	unsigned int copied = 0;
	int ret;

	if (!size)
		return 0;
	ret = mutex_lock_interruptible(&channel->read_lock);
	if (ret)
		return ret;
	while (kfifo_is_empty(&channel->tx_fifo)) {
		if (channel_is_shutdown(channel)) {
			ret = 0;
			goto unlock;
		}
		if (nonblock) {
			ret = -EAGAIN;
			goto unlock;
		}
		ret = wait_event_interruptible(channel->tx_data_wait,
									   channel_is_shutdown(channel) || !kfifo_is_empty(&channel->tx_fifo));
		if (ret)
			goto unlock;
	}

	ret = kfifo_to_user(&channel->tx_fifo, buffer, min_t(size_t, size, CHANNEL_FIFO_SIZE), &copied);
	if (!ret) {
		wake_up_interruptible(&channel->tx_space_wait);
		ret = copied;
	}
unlock:
	mutex_unlock(&channel->read_lock);
	return ret;
}

ssize_t channel_user_write(struct chardev_channel *channel, const char __user *buffer, size_t size,
						   bool nonblock)
{
	unsigned int copied = 0;
	int ret;

	if (!size)
		return 0;
	ret = mutex_lock_interruptible(&channel->write_lock);
	if (ret)
		return ret;
	while (kfifo_is_full(&channel->rx_fifo)) {
		if (channel_is_shutdown(channel)) {
			ret = -EPIPE;
			goto unlock;
		}
		if (nonblock) {
			ret = -EAGAIN;
			goto unlock;
		}
		ret = wait_event_interruptible(channel->rx_space_wait,
									   channel_is_shutdown(channel) || !kfifo_is_full(&channel->rx_fifo));
		if (ret)
			goto unlock;
	}
	if (channel_is_shutdown(channel)) {
		ret = -EPIPE;
		goto unlock;
	}

	ret = kfifo_from_user(&channel->rx_fifo, buffer, min_t(size_t, size, CHANNEL_FIFO_SIZE), &copied);
	if (!ret) {
		wake_up_interruptible(&channel->rx_data_wait);
		ret = copied;
	}
unlock:
	mutex_unlock(&channel->write_lock);
	return ret;
}

__poll_t channel_poll(struct chardev_channel *channel, struct file *file, struct poll_table_struct *wait)
{
	__poll_t mask = 0;

	poll_wait(file, &channel->tx_data_wait, wait);
	poll_wait(file, &channel->rx_space_wait, wait);
	if (!kfifo_is_empty(&channel->tx_fifo))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (!channel_is_shutdown(channel) && !kfifo_is_full(&channel->rx_fifo))
		mask |= EPOLLOUT | EPOLLWRNORM;
	if (channel_is_shutdown(channel))
		mask |= EPOLLHUP | EPOLLERR;
	return mask;
}

void channel_shutdown(struct chardev_channel *channel)
{
	if (atomic_cmpxchg(&channel->shut_down, 0, 1) != 0)
		return;
	wake_up_interruptible_all(&channel->rx_data_wait);
	wake_up_interruptible_all(&channel->rx_space_wait);
	wake_up_interruptible_all(&channel->tx_data_wait);
	wake_up_interruptible_all(&channel->tx_space_wait);
}

void channel_session_closed(struct chardev_channel *channel) { complete_all(&channel->session_closed); }

void channel_wait_closed(struct chardev_channel *channel) { wait_for_completion(&channel->session_closed); }
