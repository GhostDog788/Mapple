#include <control/commands/pslist/snapshot/stream.h>

#include <asm/unaligned.h>
#include <linux/pid_namespace.h>
#include <linux/timekeeping.h>

void mapple_pslist_encode_record(struct mapple_pslist_stream_record *record, u32 size, u16 type,
				 u64 group)
{
	put_unaligned_be32(size, &record->record_size);
	put_unaligned_be16(type, &record->record_type);
	put_unaligned_be64(group, &record->group);
}

int mapple_pslist_write(struct mapple_pslist_writer *writer, const void *data, size_t size)
{
	return mapple_command_write(writer->command, data, size);
}

int mapple_pslist_write_error(struct mapple_command_context *context, u32 error_code)
{
	struct mapple_pslist_error_response response = { 0 };
	int ret;

	put_unaligned_be32(error_code, &response.error_code);
	put_unaligned_be64(MAPPLE_PSLIST_SUPPORTED_GROUPS, &response.supported_groups);
	ret = mapple_command_write_response_header(context, MAPPLE_COMMAND_PSLIST, 0,
						   MAPPLE_STATUS_OK, sizeof(response));
	if (ret)
		return ret;
	return mapple_command_write(context, &response, sizeof(response));
}

int mapple_pslist_write_snapshot_begin(struct mapple_pslist_writer *writer, u64 requested_groups,
				       u64 effective_groups)
{
	struct mapple_pslist_snapshot_begin begin = { 0 };
	int ret;

	ret = mapple_command_write_response_header(writer->command, MAPPLE_COMMAND_PSLIST,
						   MAPPLE_HEADER_FLAG_STREAM, MAPPLE_STATUS_OK, 0);
	if (ret)
		return ret;

	mapple_pslist_encode_record(&begin.record, sizeof(begin),
				    MAPPLE_PSLIST_STREAM_SNAPSHOT_BEGIN, 0);
	put_unaligned_be64(requested_groups, &begin.requested_groups);
	put_unaligned_be64(effective_groups, &begin.effective_groups);
	put_unaligned_be64(MAPPLE_PSLIST_SUPPORTED_GROUPS, &begin.supported_groups);
	put_unaligned_be64(ktime_get_boottime_ns(), &begin.start_boottime_ns);
	return mapple_pslist_write(writer, &begin, sizeof(begin));
}

int mapple_pslist_write_snapshot_end(struct mapple_pslist_writer *writer)
{
	struct mapple_pslist_snapshot_end end = { 0 };

	mapple_pslist_encode_record(&end.record, sizeof(end), MAPPLE_PSLIST_STREAM_SNAPSHOT_END, 0);
	put_unaligned_be16(writer->partial ? MAPPLE_PSLIST_SNAPSHOT_PARTIAL :
					     MAPPLE_PSLIST_SNAPSHOT_COMPLETE,
			   &end.status);
	put_unaligned_be32(writer->task_count, &end.task_count);
	put_unaligned_be32(writer->object_count, &end.object_count);
	put_unaligned_be64(ktime_get_boottime_ns(), &end.end_boottime_ns);
	return mapple_pslist_write(writer, &end, sizeof(end));
}

static int mapple_pslist_task_type(struct task_struct *task, pid_t pid, pid_t tgid)
{
	if (task->flags & PF_KTHREAD)
		return MAPPLE_PSLIST_TASK_KTHREAD;
	if (pid == tgid)
		return MAPPLE_PSLIST_TASK_PROCESS;
	return MAPPLE_PSLIST_TASK_THREAD;
}

int mapple_pslist_write_task(struct mapple_pslist_writer *writer, struct task_struct *task,
			     pid_t pid, pid_t tgid)
{
	struct mapple_pslist_task_record record = { 0 };
	u16 task_type;
	int ret;

	task_type = mapple_pslist_task_type(task, pid, tgid);
	mapple_pslist_encode_record(&record.record, sizeof(record), MAPPLE_PSLIST_STREAM_TASK, 0);
	put_unaligned_be16(task_type, &record.task_type);
	put_unaligned_be32(pid, &record.task.pid);
	put_unaligned_be32(tgid, &record.task.tgid);
	put_unaligned_be32(task_ppid_nr_ns(task, &init_pid_ns), &record.task.ppid);
	put_unaligned_be64(task->start_boottime, &record.task.start_boottime_ns);
	record.task.state = task_state_to_char(task);
	get_task_comm(record.task.comm, task);
	ret = mapple_pslist_write(writer, &record, sizeof(record));
	if (!ret)
		++writer->task_count;
	return ret;
}

int mapple_pslist_write_link(struct mapple_pslist_writer *writer, pid_t pid, u64 object_id,
			     u16 object_type, u64 group)
{
	struct mapple_pslist_task_object_link link = { 0 };

	mapple_pslist_encode_record(&link.record, sizeof(link),
				    MAPPLE_PSLIST_STREAM_TASK_OBJECT_LINK, group);
	put_unaligned_be32(pid, &link.pid);
	put_unaligned_be64(object_id, &link.object_id);
	put_unaligned_be16(object_type, &link.object_type);
	return mapple_pslist_write(writer, &link, sizeof(link));
}

int mapple_pslist_write_object_begin(struct mapple_pslist_writer *writer, u64 object_id,
				     u16 object_type, u64 group)
{
	struct mapple_pslist_object_begin begin = { 0 };
	int ret;

	mapple_pslist_encode_record(&begin.record, sizeof(begin), MAPPLE_PSLIST_STREAM_OBJECT_BEGIN,
				    group);
	put_unaligned_be64(object_id, &begin.object_id);
	put_unaligned_be16(object_type, &begin.object_type);
	ret = mapple_pslist_write(writer, &begin, sizeof(begin));
	if (!ret)
		++writer->object_count;
	return ret;
}

int mapple_pslist_write_object_end(struct mapple_pslist_writer *writer, u64 object_id,
				   u16 object_type, u64 group, u16 status, u32 error,
				   u32 record_count)
{
	struct mapple_pslist_object_end end = { 0 };

	mapple_pslist_encode_record(&end.record, sizeof(end), MAPPLE_PSLIST_STREAM_OBJECT_END,
				    group);
	put_unaligned_be64(object_id, &end.object_id);
	put_unaligned_be16(object_type, &end.object_type);
	put_unaligned_be16(status, &end.status);
	put_unaligned_be32(error, &end.error_code);
	put_unaligned_be32(record_count, &end.record_count);
	return mapple_pslist_write(writer, &end, sizeof(end));
}
