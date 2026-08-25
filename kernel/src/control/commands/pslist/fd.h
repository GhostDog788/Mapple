#ifndef MAPPLE_COMMAND_PSLIST_FD_H
#define MAPPLE_COMMAND_PSLIST_FD_H

#include <linux/sched.h>

#include <control/commands/commands.h>

int mapple_pslist_write_fd_table(struct mapple_command_context *context,
				 struct task_struct *task);

#endif /* MAPPLE_COMMAND_PSLIST_FD_H */
