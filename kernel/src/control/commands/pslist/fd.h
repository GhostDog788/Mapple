#ifndef MAPPLE_COMMAND_PSLIST_FD_H
#define MAPPLE_COMMAND_PSLIST_FD_H

#include <linux/fdtable.h>
#include <linux/sched.h>
#include <linux/types.h>

#include <control/commands/commands.h>

int mapple_pslist_write_files_objects(struct mapple_command_context *context,
				      struct task_struct *task,
				      struct files_struct *expected_files, u64 object_id,
				      u64 groups, u32 *object_count, bool *partial);

#endif /* MAPPLE_COMMAND_PSLIST_FD_H */
