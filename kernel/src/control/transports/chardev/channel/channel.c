#include <control/transports/chardev/channel/channel.h>

#include <linux/completion.h>
#include <linux/kref.h>
#include <linux/slab.h>

#include <control/transports/chardev/channel/pipe.h>

#define CHANNEL_FIFO_SIZE (64U * 1024U)

struct chardev_channel {
	struct kref refcount;
	struct channel_pipe user_to_stream;
	struct channel_pipe stream_to_user;
	struct completion session_closed;
};

static void channel_release(struct kref *refcount)
{
	struct chardev_channel *channel = container_of(refcount, struct chardev_channel, refcount);

	channel_pipe_cleanup(&channel->stream_to_user);
	channel_pipe_cleanup(&channel->user_to_stream);
	kfree(channel);
}

struct chardev_channel *channel_create(void)
{
	struct chardev_channel *channel;
	int ret;

	channel = kzalloc(sizeof(*channel), GFP_KERNEL);
	if (!channel)
		return NULL;
	ret = channel_pipe_init(&channel->user_to_stream, CHANNEL_FIFO_SIZE);
	if (ret)
		goto free_channel;
	ret = channel_pipe_init(&channel->stream_to_user, CHANNEL_FIFO_SIZE);
	if (ret)
		goto cleanup_user_to_stream;

	kref_init(&channel->refcount);
	init_completion(&channel->session_closed);
	return channel;

cleanup_user_to_stream:
	channel_pipe_cleanup(&channel->user_to_stream);
free_channel:
	kfree(channel);
	return NULL;
}

void channel_get(struct chardev_channel *channel) { kref_get(&channel->refcount); }

void channel_put(struct chardev_channel *channel) { kref_put(&channel->refcount, channel_release); }

ssize_t channel_stream_read(struct chardev_channel *channel, void *buffer, size_t size)
{
	return channel_pipe_read(&channel->user_to_stream, buffer, size);
}

ssize_t channel_stream_write(struct chardev_channel *channel, const void *buffer, size_t size)
{
	return channel_pipe_write(&channel->stream_to_user, buffer, size);
}

ssize_t channel_user_read(struct chardev_channel *channel, char __user *buffer, size_t size, bool nonblock)
{
	return channel_pipe_user_read(&channel->stream_to_user, buffer, size, nonblock);
}

ssize_t channel_user_write(struct chardev_channel *channel, const char __user *buffer, size_t size,
							   bool nonblock)
{
	return channel_pipe_user_write(&channel->user_to_stream, buffer, size, nonblock);
}

__poll_t channel_poll(struct chardev_channel *channel, struct file *file, struct poll_table_struct *wait)
{
	return channel_pipe_poll_read(&channel->stream_to_user, file, wait) |
		   channel_pipe_poll_write(&channel->user_to_stream, file, wait);
}

void channel_shutdown(struct chardev_channel *channel)
{
	channel_pipe_shutdown(&channel->user_to_stream);
	channel_pipe_shutdown(&channel->stream_to_user);
}

void channel_session_closed(struct chardev_channel *channel) { complete_all(&channel->session_closed); }

void channel_wait_closed(struct chardev_channel *channel) { wait_for_completion(&channel->session_closed); }
