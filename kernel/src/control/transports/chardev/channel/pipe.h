#ifndef MAPPLE_CHARDEV_CHANNEL_PIPE_H
#define MAPPLE_CHARDEV_CHANNEL_PIPE_H

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/types.h>
#include <linux/wait.h>

struct channel_pipe {
	struct kfifo fifo;
	struct mutex read_lock;
	struct mutex write_lock;
	wait_queue_head_t data_wait;
	wait_queue_head_t space_wait;
	atomic_t shut_down;
};

int channel_pipe_init(struct channel_pipe *pipe, unsigned int size);
void channel_pipe_cleanup(struct channel_pipe *pipe);
ssize_t channel_pipe_read(struct channel_pipe *pipe, void *buffer, size_t size);
ssize_t channel_pipe_write(struct channel_pipe *pipe, const void *buffer, size_t size);
ssize_t channel_pipe_user_read(struct channel_pipe *pipe, char __user *buffer, size_t size, bool nonblock);
ssize_t channel_pipe_user_write(struct channel_pipe *pipe, const char __user *buffer, size_t size,
								bool nonblock);
__poll_t channel_pipe_poll_read(struct channel_pipe *pipe, struct file *file, struct poll_table_struct *wait);
__poll_t channel_pipe_poll_write(struct channel_pipe *pipe, struct file *file, struct poll_table_struct *wait);
void channel_pipe_shutdown(struct channel_pipe *pipe);

#endif /* MAPPLE_CHARDEV_CHANNEL_PIPE_H */
