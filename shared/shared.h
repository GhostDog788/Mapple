/*
 * shared.h - ABI shared between the kernel module and the usermode client.
 *
 * This is the ONE header both sides compile. Keep it free of anything only
 * one side can include, and use only fixed-layout types in anything that
 * crosses the user/kernel boundary.
 */
#ifndef SHARED_H
#define SHARED_H

/* Auto-created device node: /dev/mapple (see device_create in kernel). */
#define MAPPLE_DEVICE_NAME "mapple"
#define MAPPLE_CLASS_NAME  "mapple"
#define MAPPLE_DEVICE_PATH "/dev/" MAPPLE_DEVICE_NAME

#define MAPPLE_TCP_PORT 4444

#endif /* SHARED_H */
