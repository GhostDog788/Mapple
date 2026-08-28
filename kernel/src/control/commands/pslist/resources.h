#ifndef MAPPLE_COMMAND_PSLIST_RESOURCES_H
#define MAPPLE_COMMAND_PSLIST_RESOURCES_H

#include <linux/sched.h>

#include <control/commands/commands.h>

int mapple_pslist_write_resources_object(struct mapple_command_context *context,
					  struct task_struct *task, u64 object_id,
					  u32 *object_count);

#endif /* MAPPLE_COMMAND_PSLIST_RESOURCES_H */
