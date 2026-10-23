#ifndef MAPPLE_COMMAND_PSLIST_FILES_DESCRIPTORS_H
#define MAPPLE_COMMAND_PSLIST_FILES_DESCRIPTORS_H

#include <linux/fdtable.h>
#include <linux/sched.h>
#include <linux/types.h>

#include <control/commands/pslist/snapshot/stream.h>

int mapple_pslist_write_files_objects(struct mapple_pslist_writer *writer, struct task_struct *task,
				      struct files_struct *expected_files, u64 fds_object_id,
				      u64 network_object_id, u64 groups);

#endif /* MAPPLE_COMMAND_PSLIST_FILES_DESCRIPTORS_H */
