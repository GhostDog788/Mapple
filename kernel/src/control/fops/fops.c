#include <control/fops/fops.h>

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>

#include <shared.h>

#include <control/session.h>

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

static dev_t mapple_devt;
static struct cdev mapple_cdev;
static struct class *mapple_class;

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
		ret = wait_event_interruptible(context->rx_data_wait,
			mapple_stream_is_shutdown(stream) || !kfifo_is_empty(&context->rx_fifo));
		if (ret)
			return ret;
	}
}

static ssize_t mapple_chardev_stream_write(struct mapple_stream *stream, const void *buffer,
						   size_t size)
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
		ret = wait_event_interruptible(context->tx_space_wait,
			mapple_stream_is_shutdown(stream) || !kfifo_is_full(&context->tx_fifo));
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
	struct mapple_chardev_session *context =
		container_of(session, struct mapple_chardev_session, session);

	kfifo_free(&context->tx_fifo);
	kfifo_free(&context->rx_fifo);
	kfree(context);
}

static int mapple_open(struct inode *, struct file *file)
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
	mapple_session_init(&context->session, MAPPLE_TRANSPORT_CHARDEV,
				    &mapple_chardev_stream_ops, mapple_chardev_session_release);

	ret = mapple_session_register(&context->session);
	if (ret)
		goto put_session;
	ret = mapple_session_start_parser(&context->session);
	if (ret) {
		mapple_session_unregister(&context->session);
		goto put_session;
	}

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

static ssize_t mapple_read(struct file *file, char __user *buffer, size_t size, loff_t *)
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

	ret = kfifo_to_user(&context->tx_fifo, buffer,
			    min_t(size_t, size, MAPPLE_FIFO_SIZE), &copied);
	if (!ret) {
		wake_up_interruptible(&context->tx_space_wait);
		ret = copied;
	}
unlock:
	mutex_unlock(&context->read_lock);
	return ret;
}

static ssize_t mapple_write(struct file *file, const char __user *buffer, size_t size,
				    loff_t *)
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

	ret = kfifo_from_user(&context->rx_fifo, buffer,
			      min_t(size_t, size, MAPPLE_FIFO_SIZE), &copied);
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
	if (!mapple_stream_is_shutdown(&context->session.stream) &&
	    !kfifo_is_full(&context->rx_fifo))
		mask |= EPOLLOUT | EPOLLWRNORM;
	if (mapple_stream_is_shutdown(&context->session.stream))
		mask |= EPOLLHUP | EPOLLERR;
	return mask;
}

static int mapple_release(struct inode *, struct file *file)
{
	struct mapple_chardev_session *context = file->private_data;

	mapple_session_shutdown(&context->session);
	mapple_session_wait(&context->session);
	mapple_session_put(&context->session); /* file reference */
	file->private_data = NULL;
	return 0;
}

static const struct file_operations mapple_fops = {
	.owner = THIS_MODULE,
	.open = mapple_open,
	.read = mapple_read,
	.write = mapple_write,
	.poll = mapple_poll,
	.release = mapple_release,
	.llseek = no_llseek,
};

static struct class *mapple_class_create(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	return class_create(MAPPLE_CLASS_NAME);
#else
	return class_create(THIS_MODULE, MAPPLE_CLASS_NAME);
#endif
}

int mapple_chardev_init(void)
{
	struct device *device;
	int ret;

	ret = alloc_chrdev_region(&mapple_devt, 0, 1, MAPPLE_DEVICE_NAME);
	if (ret)
		return ret;
	cdev_init(&mapple_cdev, &mapple_fops);
	mapple_cdev.owner = THIS_MODULE;
	ret = cdev_add(&mapple_cdev, mapple_devt, 1);
	if (ret)
		goto unregister_region;
	mapple_class = mapple_class_create();
	if (IS_ERR(mapple_class)) {
		ret = PTR_ERR(mapple_class);
		goto delete_cdev;
	}
	device = device_create(mapple_class, NULL, mapple_devt, NULL, MAPPLE_DEVICE_NAME);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		goto destroy_class;
	}

	pr_info("mapple: registered /dev/%s major=%u minor=%u\n", MAPPLE_DEVICE_NAME,
		MAJOR(mapple_devt), MINOR(mapple_devt));
	return 0;

destroy_class:
	class_destroy(mapple_class);
delete_cdev:
	cdev_del(&mapple_cdev);
unregister_region:
	unregister_chrdev_region(mapple_devt, 1);
	return ret;
}

void mapple_chardev_exit(void)
{
	device_destroy(mapple_class, mapple_devt);
	class_destroy(mapple_class);
	cdev_del(&mapple_cdev);
	unregister_chrdev_region(mapple_devt, 1);
	pr_info("mapple: unregistered /dev/%s\n", MAPPLE_DEVICE_NAME);
}
