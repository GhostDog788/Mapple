#include <control/fops/ioctl/ioctl.h>

#include "shared.h"

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

static DEFINE_MUTEX(mapple_lock);
static char mapple_msg[mapple_MSG_MAX];

long mapple_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
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
