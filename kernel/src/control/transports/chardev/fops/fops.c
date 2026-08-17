#include <control/transports/chardev/fops/fops.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include <control/sessions/session_manager.h>

#define MAPPLE_FIFO_SIZE (64U * 1024U)

struct mapple_chardev_session {
	struct mapple_session session;
	struct kfifo rx_fifo;
	struct kfifo tx_fifo;
	struct mutex read_lock;
	struct mutex write_lock;
	wait_queue_head_t rx_data_wait;
	wait_queue_head_t rx_space_wait;
	wait_queue_head_t tx_data_wait;
	wait_queue_head_t tx_space_wait;
};

static struct mapple_chardev_session *mapple_chardev_from_stream(struct mapple_stream *stream)
{
	struct mapple_session *session = container_of(stream, struct mapple_session, stream);

	return container_of(session, struct mapple_chardev_session, session);
}

static ssize_t mapple_chardev_stream_read(struct mapple_stream *stream, void *buffer, size_t size)
{
	struct mapple_chardev_session *context = mapple_chardev_from_stream(stream);
	unsigned int copied;
	int ret;

	for (;;) {
		copied = kfifo_out(&context->rx_fifo, buffer, size);
		if (copied) {
			wake_up_interruptible(&context->rx_space_wait);
			return copied;
		}
		if (mapple_stream_is_shutdown(stream))
			return 0;
		ret = wait_event_interruptible(context->rx_data_wait, mapple_stream_is_shutdown(stream) ||
																  !kfifo_is_empty(&context->rx_fifo));
		if (ret)
			return ret;
	}
}

static ssize_t mapple_chardev_stream_write(struct mapple_stream *stream, const void *buffer, size_t size)
{
	struct mapple_chardev_session *context = mapple_chardev_from_stream(stream);
	unsigned int copied;
	int ret;

	for (;;) {
		if (mapple_stream_is_shutdown(stream))
			return -EPIPE;
		copied = kfifo_in(&context->tx_fifo, buffer, size);
		if (copied) {
			wake_up_interruptible(&context->tx_data_wait);
			return copied;
		}
		ret = wait_event_interruptible(context->tx_space_wait, mapple_stream_is_shutdown(stream) ||
																   !kfifo_is_full(&context->tx_fifo));
		if (ret)
			return ret;
	}
}

static void mapple_chardev_stream_shutdown(struct mapple_stream *stream)
{
	struct mapple_chardev_session *context = mapple_chardev_from_stream(stream);

	wake_up_interruptible_all(&context->rx_data_wait);
	wake_up_interruptible_all(&context->rx_space_wait);
	wake_up_interruptible_all(&context->tx_data_wait);
	wake_up_interruptible_all(&context->tx_space_wait);
}

static const struct mapple_stream_ops mapple_chardev_stream_ops = {
	.read_some = mapple_chardev_stream_read,
	.write_some = mapple_chardev_stream_write,
	.shutdown = mapple_chardev_stream_shutdown,
};

static void mapple_chardev_session_release(struct mapple_session *session)
{
	struct mapple_chardev_session *context = container_of(session, struct mapple_chardev_session, session);

	kfifo_free(&context->tx_fifo);
	kfifo_free(&context->rx_fifo);
	kfree(context);
}

static const struct mapple_transport mapple_chardev_transport = {
	.name = "chardev",
	.stream_ops = &mapple_chardev_stream_ops,
	.release = mapple_chardev_session_release,
};

static int mapple_open(struct inode *_, struct file *file)
{
	struct mapple_chardev_session *context;
	int ret;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;
	ret = kfifo_alloc(&context->rx_fifo, MAPPLE_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		goto free_context;
	ret = kfifo_alloc(&context->tx_fifo, MAPPLE_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		goto free_rx;

	mutex_init(&context->read_lock);
	mutex_init(&context->write_lock);
	init_waitqueue_head(&context->rx_data_wait);
	init_waitqueue_head(&context->rx_space_wait);
	init_waitqueue_head(&context->tx_data_wait);
	init_waitqueue_head(&context->tx_space_wait);
	ret = mapple_session_manager_register(&context->session, &mapple_chardev_transport);
	if (ret)
		goto put_session;

	file->private_data = context;
	return 0;

put_session:
	mapple_session_put(&context->session);
	return ret;
free_rx:
	kfifo_free(&context->rx_fifo);
free_context:
	kfree(context);
	return ret;
}

static ssize_t mapple_read(struct file *file, char __user *buffer, size_t size, loff_t *_)
{
	struct mapple_chardev_session *context = file->private_data;
	unsigned int copied = 0;
	int ret;

	if (!size)
		return 0;
	ret = mutex_lock_interruptible(&context->read_lock);
	if (ret)
		return ret;
	while (kfifo_is_empty(&context->tx_fifo)) {
		if (mapple_stream_is_shutdown(&context->session.stream)) {
			ret = 0;
			goto unlock;
		}
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto unlock;
		}
		ret = wait_event_interruptible(context->tx_data_wait,
									   mapple_stream_is_shutdown(&context->session.stream) ||
										   !kfifo_is_empty(&context->tx_fifo));
		if (ret)
			goto unlock;
	}

	ret = kfifo_to_user(&context->tx_fifo, buffer, min_t(size_t, size, MAPPLE_FIFO_SIZE), &copied);
	if (!ret) {
		wake_up_interruptible(&context->tx_space_wait);
		ret = copied;
	}
unlock:
	mutex_unlock(&context->read_lock);
	return ret;
}

static ssize_t mapple_write(struct file *file, const char __user *buffer, size_t size, loff_t *_)
{
	struct mapple_chardev_session *context = file->private_data;
	unsigned int copied = 0;
	int ret;

	if (!size)
		return 0;
	ret = mutex_lock_interruptible(&context->write_lock);
	if (ret)
		return ret;
	while (kfifo_is_full(&context->rx_fifo)) {
		if (mapple_stream_is_shutdown(&context->session.stream)) {
			ret = -EPIPE;
			goto unlock;
		}
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto unlock;
		}
		ret = wait_event_interruptible(context->rx_space_wait,
									   mapple_stream_is_shutdown(&context->session.stream) ||
										   !kfifo_is_full(&context->rx_fifo));
		if (ret)
			goto unlock;
	}
	if (mapple_stream_is_shutdown(&context->session.stream)) {
		ret = -EPIPE;
		goto unlock;
	}

	ret = kfifo_from_user(&context->rx_fifo, buffer, min_t(size_t, size, MAPPLE_FIFO_SIZE), &copied);
	if (!ret) {
		wake_up_interruptible(&context->rx_data_wait);
		ret = copied;
	}
unlock:
	mutex_unlock(&context->write_lock);
	return ret;
}

static __poll_t mapple_poll(struct file *file, struct poll_table_struct *wait)
{
	struct mapple_chardev_session *context = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &context->tx_data_wait, wait);
	poll_wait(file, &context->rx_space_wait, wait);
	if (!kfifo_is_empty(&context->tx_fifo))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (!mapple_stream_is_shutdown(&context->session.stream) && !kfifo_is_full(&context->rx_fifo))
		mask |= EPOLLOUT | EPOLLWRNORM;
	if (mapple_stream_is_shutdown(&context->session.stream))
		mask |= EPOLLHUP | EPOLLERR;
	return mask;
}

static int mapple_release(struct inode *_, struct file *file)
{
	struct mapple_chardev_session *context = file->private_data;

	mapple_session_shutdown(&context->session);
	mapple_session_wait(&context->session);
	mapple_session_put(&context->session); /* file reference */
	file->private_data = NULL;
	return 0;
}

const struct file_operations mapple_fops = {
	.owner = THIS_MODULE,
	.open = mapple_open,
	.read = mapple_read,
	.write = mapple_write,
	.poll = mapple_poll,
	.release = mapple_release,
	.llseek = no_llseek,
};
