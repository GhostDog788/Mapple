#include <control/fops/fops.h>

#include <linux/fs.h>
#include <linux/module.h>

#include <control/fops/ioctl/ioctl.h>

const struct file_operations mapple_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = mapple_ioctl,
};
