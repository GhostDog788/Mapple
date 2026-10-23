#ifndef MAPPLE_COMMAND_PSLIST_MEMORY_IDENTITY_H
#define MAPPLE_COMMAND_PSLIST_MEMORY_IDENTITY_H

#include <linux/mm_types.h>
#include <linux/sched.h>

#include <control/commands/pslist/snapshot/stream.h>

int mapple_pslist_write_identity_object(struct mapple_pslist_writer *writer,
					struct task_struct *task, struct mm_struct *mm,
					u64 object_id);

#endif
