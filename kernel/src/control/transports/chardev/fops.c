#include <control/transports/chardev/fops.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/module.h>

#include <control/sessions/session_manager.h>
#include <control/transports/chardev/channel.h>
#include <control/transports/chardev/stream_ops.h>

static void mapple_chardev_release_backend(void *backend)
{
	struct chardev_channel *channel = backend;

	channel_session_closed(channel);
	channel_put(channel);
}

static const struct mapple_transport mapple_chardev_transport = {
	.name = "chardev",
	.stream_ops = &mapple_chardev_stream_ops,
	.release_backend = mapple_chardev_release_backend,
};

static int mapple_open(struct inode *_, struct file *file)
{
	struct chardev_channel *channel;
	int ret;

	channel = channel_create();
	if (!channel)
		return -ENOMEM;

	channel_get(channel); /* backend reference transferred to the manager */
	ret = mapple_session_manager_register(&mapple_chardev_transport, channel);
	if (ret) {
		channel_put(channel); /* file reference */
		return ret;
	}

	file->private_data = channel;
	return 0;
}

static ssize_t mapple_read(struct file *file, char __user *buffer, size_t size, loff_t *_)
{
	struct chardev_channel *channel = file->private_data;

	return channel_user_read(channel, buffer, size, file->f_flags & O_NONBLOCK);
}

static ssize_t mapple_write(struct file *file, const char __user *buffer, size_t size, loff_t *_)
{
	struct chardev_channel *channel = file->private_data;

	return channel_user_write(channel, buffer, size, file->f_flags & O_NONBLOCK);
}

static __poll_t mapple_poll(struct file *file, struct poll_table_struct *wait)
{
	struct chardev_channel *channel = file->private_data;

	return channel_poll(channel, file, wait);
}

static int mapple_release(struct inode *_, struct file *file)
{
	struct chardev_channel *channel = file->private_data;

	channel_shutdown(channel);
	channel_wait_closed(channel);
	channel_put(channel); /* file reference */
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
