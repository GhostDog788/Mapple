#ifndef MAPPLE_COMMAND_PSLIST_SNAPSHOT_STREAM_H
#define MAPPLE_COMMAND_PSLIST_SNAPSHOT_STREAM_H

#include <linux/sched.h>

#include <control/commands/commands.h>

struct mapple_pslist_writer {
	struct mapple_command_context *command;
	u32 task_count;
	u32 object_count;
	bool partial;
};

void mapple_pslist_encode_record(struct mapple_pslist_stream_record *record, u32 size, u16 type,
				 u64 group);
int mapple_pslist_write(struct mapple_pslist_writer *writer, const void *data, size_t size);
int mapple_pslist_write_error(struct mapple_command_context *context, u32 error_code);
int mapple_pslist_write_snapshot_begin(struct mapple_pslist_writer *writer, u64 requested_groups,
				       u64 effective_groups);
int mapple_pslist_write_snapshot_end(struct mapple_pslist_writer *writer);
int mapple_pslist_write_task(struct mapple_pslist_writer *writer, struct task_struct *task,
			     pid_t pid, pid_t tgid);
int mapple_pslist_write_link(struct mapple_pslist_writer *writer, pid_t pid, u64 object_id,
			     u16 object_type, u64 group);
int mapple_pslist_write_object_begin(struct mapple_pslist_writer *writer, u64 object_id,
				     u16 object_type, u64 group);
int mapple_pslist_write_object_end(struct mapple_pslist_writer *writer, u64 object_id,
				   u16 object_type, u64 group, u16 status, u32 error,
				   u32 record_count);

#endif
