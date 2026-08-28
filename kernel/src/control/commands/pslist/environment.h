#ifndef MAPPLE_COMMAND_PSLIST_ENVIRONMENT_H
#define MAPPLE_COMMAND_PSLIST_ENVIRONMENT_H

#include <linux/mm_types.h>
#include <linux/sched.h>

#include <control/commands/commands.h>

int mapple_pslist_write_environment_object(struct mapple_command_context *context,
					    struct task_struct *task,
					    struct mm_struct *mm, u64 object_id,
					    u32 *object_count, bool *partial);

#endif /* MAPPLE_COMMAND_PSLIST_ENVIRONMENT_H */
