#ifndef MAPPLE_IOCTL_H
#define MAPPLE_IOCTL_H

#include <linux/fs.h>

long mapple_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#endif /* MAPPLE_IOCTL_H */
