/*
 * shared.h - ABI shared between the kernel module and the usermode client.
 *
 * This is the ONE header both sides compile. Keep it free of anything only
 * one side can include: pick the right ioctl/type headers per side via
 * __KERNEL__, and use only fixed-layout types in anything that crosses the
 * user/kernel boundary.
 */
#ifndef SHARED_H
#define SHARED_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

/* Auto-created device node: /dev/mapple (see device_create in kernel). */
#define mapple_DEVICE_NAME "mapple"
#define mapple_CLASS_NAME  "mapple"
#define mapple_DEVICE_PATH "/dev/" mapple_DEVICE_NAME

/* Largest string the driver will store, including the trailing NUL. The
 * whole buffer crosses the boundary by value, so both sides agree on size.
 */
#define mapple_MSG_MAX 256

struct mapple_msg {
	char data[mapple_MSG_MAX];
};

/* Documentation/userspace-api/ioctl/ioctl-number.rst */
#define mapple_IOC_MAGIC 'K'
#define IOCTL_SET_MSG _IOW(mapple_IOC_MAGIC, 0, struct mapple_msg)
#define IOCTL_GET_MSG _IOR(mapple_IOC_MAGIC, 1, struct mapple_msg)

#endif /* SHARED_H */
