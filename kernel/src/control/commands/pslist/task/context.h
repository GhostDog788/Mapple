#ifndef MAPPLE_COMMAND_PSLIST_TASK_CONTEXT_H
#define MAPPLE_COMMAND_PSLIST_TASK_CONTEXT_H

#include <linux/sched.h>

#include <control/commands/pslist/snapshot/stream.h>

int mapple_pslist_write_context_object(struct mapple_pslist_writer *writer,
				       struct task_struct *task, u64 object_id);

#endif /* MAPPLE_COMMAND_PSLIST_TASK_CONTEXT_H */
