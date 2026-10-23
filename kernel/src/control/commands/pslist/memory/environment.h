#ifndef MAPPLE_COMMAND_PSLIST_MEMORY_ENVIRONMENT_H
#define MAPPLE_COMMAND_PSLIST_MEMORY_ENVIRONMENT_H

#include <linux/mm_types.h>
#include <linux/sched.h>

#include <control/commands/pslist/snapshot/stream.h>

int mapple_pslist_write_environment_object(struct mapple_pslist_writer *writer,
					   struct task_struct *task, struct mm_struct *mm,
					   u64 object_id);

#endif /* MAPPLE_COMMAND_PSLIST_MEMORY_ENVIRONMENT_H */
