#include "shared.h"

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

static dev_t mapple_devt;
static struct cdev mapple_cdev;
static struct class *mapple_class;

/* The stored string and the lock that guards it against concurrent ioctls. */
static DEFINE_MUTEX(mapple_lock);
static char mapple_msg[mapple_MSG_MAX];

static long mapple_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	struct mapple_msg *buf;
	long ret = 0;

	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	switch (cmd) {
	case IOCTL_SET_MSG:
		if (copy_from_user(buf, uarg, sizeof(*buf))) {
			ret = -EFAULT;
			break;
		}
		/* Never trust user input to be terminated. */
		buf->data[mapple_MSG_MAX - 1] = '\0';
		mutex_lock(&mapple_lock);
		memcpy(mapple_msg, buf->data, mapple_MSG_MAX);
		mutex_unlock(&mapple_lock);
		pr_info("mapple: stored \"%s\"\n", buf->data);
		break;
	case IOCTL_GET_MSG:
		mutex_lock(&mapple_lock);
		memcpy(buf->data, mapple_msg, mapple_MSG_MAX);
		mutex_unlock(&mapple_lock);
		buf->data[mapple_MSG_MAX - 1] = '\0';
		if (copy_to_user(uarg, buf, sizeof(*buf)))
			ret = -EFAULT;
		break;
	default:
		ret = -ENOTTY;
	}

	kfree(buf);
	return ret;
}

static const struct file_operations mapple_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = mapple_ioctl,
};

static struct class *mapple_class_create(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	return class_create(mapple_CLASS_NAME);
#else
	return class_create(THIS_MODULE, mapple_CLASS_NAME);
#endif
}

static int __init mapple_init(void)
{
	struct device *device;
	int ret;

	ret = alloc_chrdev_region(&mapple_devt, 0, 1, mapple_DEVICE_NAME);
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

	device = device_create(mapple_class, NULL, mapple_devt, NULL,
			       mapple_DEVICE_NAME);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		goto destroy_class;
	}

	pr_info("mapple: registered /dev/%s major=%u minor=%u\n",
		mapple_DEVICE_NAME, MAJOR(mapple_devt), MINOR(mapple_devt));
	return 0;

destroy_class:
	class_destroy(mapple_class);
delete_cdev:
	cdev_del(&mapple_cdev);
unregister_region:
	unregister_chrdev_region(mapple_devt, 1);
	return ret;
}

static void __exit mapple_exit(void)
{
	device_destroy(mapple_class, mapple_devt);
	class_destroy(mapple_class);
	cdev_del(&mapple_cdev);
	unregister_chrdev_region(mapple_devt, 1);
	pr_info("mapple: unregistered /dev/%s\n", mapple_DEVICE_NAME);
}

module_init(mapple_init);
module_exit(mapple_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Stores and retrieves a single string via ioctl");
