#include <control/transports/chardev/channel/pipe.h>

#include <linux/errno.h>
#include <linux/minmax.h>

static bool channel_pipe_is_shutdown(const struct channel_pipe *pipe)
{
	return atomic_read(&pipe->shut_down) != 0;
}

int channel_pipe_init(struct channel_pipe *pipe, unsigned int size)
{
	int ret;

	ret = kfifo_alloc(&pipe->fifo, size, GFP_KERNEL);
	if (ret)
		return ret;
	mutex_init(&pipe->read_lock);
	mutex_init(&pipe->write_lock);
	init_waitqueue_head(&pipe->data_wait);
	init_waitqueue_head(&pipe->space_wait);
	atomic_set(&pipe->shut_down, 0);
	return 0;
}

void channel_pipe_cleanup(struct channel_pipe *pipe) { kfifo_free(&pipe->fifo); }

ssize_t channel_pipe_read(struct channel_pipe *pipe, void *buffer, size_t size)
{
	unsigned int copied;
	int ret;

	if (!size)
		return 0;
	ret = mutex_lock_interruptible(&pipe->read_lock);
	if (ret)
		return ret;
	for (;;) {
		copied = kfifo_out(&pipe->fifo, buffer, size);
		if (copied) {
			wake_up_interruptible(&pipe->space_wait);
			ret = copied;
			break;
		}
		if (channel_pipe_is_shutdown(pipe)) {
			ret = 0;
			break;
		}
		ret = wait_event_interruptible(pipe->data_wait,
									   channel_pipe_is_shutdown(pipe) || !kfifo_is_empty(&pipe->fifo));
		if (ret)
			break;
	}
	mutex_unlock(&pipe->read_lock);
	return ret;
}

ssize_t channel_pipe_write(struct channel_pipe *pipe, const void *buffer, size_t size)
{
	unsigned int copied;
	int ret;

	if (!size)
		return 0;
	ret = mutex_lock_interruptible(&pipe->write_lock);
	if (ret)
		return ret;
	for (;;) {
		if (channel_pipe_is_shutdown(pipe)) {
			ret = -EPIPE;
			break;
		}
		copied = kfifo_in(&pipe->fifo, buffer, size);
		if (copied) {
			wake_up_interruptible(&pipe->data_wait);
			ret = copied;
			break;
		}
		ret = wait_event_interruptible(pipe->space_wait,
									   channel_pipe_is_shutdown(pipe) || !kfifo_is_full(&pipe->fifo));
		if (ret)
			break;
	}
	mutex_unlock(&pipe->write_lock);
	return ret;
}

ssize_t channel_pipe_user_read(struct channel_pipe *pipe, char __user *buffer, size_t size, bool nonblock)
{
	unsigned int copied = 0;
	int ret;

	if (!size)
		return 0;
	ret = mutex_lock_interruptible(&pipe->read_lock);
	if (ret)
		return ret;
	while (kfifo_is_empty(&pipe->fifo)) {
		if (channel_pipe_is_shutdown(pipe)) {
			ret = 0;
			goto unlock;
		}
		if (nonblock) {
			ret = -EAGAIN;
			goto unlock;
		}
		ret = wait_event_interruptible(pipe->data_wait,
									   channel_pipe_is_shutdown(pipe) || !kfifo_is_empty(&pipe->fifo));
		if (ret)
			goto unlock;
	}

	ret = kfifo_to_user(&pipe->fifo, buffer, min_t(size_t, size, kfifo_size(&pipe->fifo)), &copied);
	if (!ret) {
		wake_up_interruptible(&pipe->space_wait);
		ret = copied;
	}
unlock:
	mutex_unlock(&pipe->read_lock);
	return ret;
}

ssize_t channel_pipe_user_write(struct channel_pipe *pipe, const char __user *buffer, size_t size,
								bool nonblock)
{
	unsigned int copied = 0;
	int ret;

	if (!size)
		return 0;
	ret = mutex_lock_interruptible(&pipe->write_lock);
	if (ret)
		return ret;
	while (kfifo_is_full(&pipe->fifo)) {
		if (channel_pipe_is_shutdown(pipe)) {
			ret = -EPIPE;
			goto unlock;
		}
		if (nonblock) {
			ret = -EAGAIN;
			goto unlock;
		}
		ret = wait_event_interruptible(pipe->space_wait,
									   channel_pipe_is_shutdown(pipe) || !kfifo_is_full(&pipe->fifo));
		if (ret)
			goto unlock;
	}
	if (channel_pipe_is_shutdown(pipe)) {
		ret = -EPIPE;
		goto unlock;
	}

	ret = kfifo_from_user(&pipe->fifo, buffer, min_t(size_t, size, kfifo_size(&pipe->fifo)), &copied);
	if (!ret) {
		wake_up_interruptible(&pipe->data_wait);
		ret = copied;
	}
unlock:
	mutex_unlock(&pipe->write_lock);
	return ret;
}

__poll_t channel_pipe_poll_read(struct channel_pipe *pipe, struct file *file, struct poll_table_struct *wait)
{
	__poll_t mask = 0;

	poll_wait(file, &pipe->data_wait, wait);
	if (!kfifo_is_empty(&pipe->fifo))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (channel_pipe_is_shutdown(pipe))
		mask |= EPOLLHUP | EPOLLERR;
	return mask;
}

__poll_t channel_pipe_poll_write(struct channel_pipe *pipe, struct file *file, struct poll_table_struct *wait)
{
	__poll_t mask = 0;

	poll_wait(file, &pipe->space_wait, wait);
	if (!channel_pipe_is_shutdown(pipe) && !kfifo_is_full(&pipe->fifo))
		mask |= EPOLLOUT | EPOLLWRNORM;
	if (channel_pipe_is_shutdown(pipe))
		mask |= EPOLLHUP | EPOLLERR;
	return mask;
}

void channel_pipe_shutdown(struct channel_pipe *pipe)
{
	if (atomic_cmpxchg(&pipe->shut_down, 0, 1) != 0)
		return;
	wake_up_interruptible_all(&pipe->data_wait);
	wake_up_interruptible_all(&pipe->space_wait);
}
