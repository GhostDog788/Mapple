#ifndef CHARDEV_CHANNEL_H
#define CHARDEV_CHANNEL_H

#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/types.h>

struct chardev_channel;

struct chardev_channel *channel_create(void);
void channel_get(struct chardev_channel *channel);
void channel_put(struct chardev_channel *channel);
ssize_t channel_stream_read(struct chardev_channel *channel, void *buffer, size_t size);
ssize_t channel_stream_write(struct chardev_channel *channel, const void *buffer, size_t size);
ssize_t channel_user_read(struct chardev_channel *channel, char __user *buffer, size_t size, bool nonblock);
ssize_t channel_user_write(struct chardev_channel *channel, const char __user *buffer, size_t size,
						   bool nonblock);
__poll_t channel_poll(struct chardev_channel *channel, struct file *file, struct poll_table_struct *wait);
void channel_shutdown(struct chardev_channel *channel);
void channel_session_closed(struct chardev_channel *channel);
void channel_wait_closed(struct chardev_channel *channel);

#endif /* CHARDEV_CHANNEL_H */
