#include <control/commands/pslist/environment.h>

#include <asm/unaligned.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>

struct mapple_pslist_captured_environment {
	struct mapple_pslist_environment_record wire;
	char *bytes;
	u32 length;
};

static void mapple_pslist_encode_record(struct mapple_pslist_stream_record *record,
					u32 size, u16 type, u64 group)
{
	put_unaligned_be32(size, &record->record_size);
	put_unaligned_be16(type, &record->record_type);
	put_unaligned_be64(group, &record->group);
}

static bool mapple_pslist_task_uses_mm(struct task_struct *task,
				       struct mm_struct *mm)
{
	bool matches;

	task_lock(task);
	matches = task->mm == mm;
	task_unlock(task);
	return matches;
}

static void mapple_pslist_encode_environment_status(
	struct mapple_pslist_environment_record *wire, u16 status, u32 error)
{
	put_unaligned_be16(status, &wire->status);
	put_unaligned_be32(error, &wire->error);
}

static int mapple_pslist_capture_environment(
	struct mapple_pslist_captured_environment *captured,
	struct task_struct *task, struct mm_struct *mm)
{
	unsigned long start;
	unsigned long end;
	unsigned long length;
	u32 entry_count = 0;
	u32 flags = 0;
	unsigned long index;
	int copied;

	spin_lock(&mm->arg_lock);
	start = mm->env_start;
	end = mm->env_end;
	spin_unlock(&mm->arg_lock);
	if (end < start) {
		mapple_pslist_encode_environment_status(
			&captured->wire, MAPPLE_PSLIST_ENVIRONMENT_UNAVAILABLE, EINVAL);
		return 0;
	}
	if (end == start) {
		put_unaligned_be16(MAPPLE_PSLIST_ENVIRONMENT_EMPTY,
				   &captured->wire.status);
		put_unaligned_be16(MAPPLE_PSLIST_ENVIRONMENT_FLAG_TERMINATED,
				   &captured->wire.flags);
		return 0;
	}
	length = end - start;
	if (length > MAPPLE_PSLIST_ENVIRONMENT_MAX_SIZE) {
		mapple_pslist_encode_environment_status(
			&captured->wire, MAPPLE_PSLIST_ENVIRONMENT_TOO_LARGE, E2BIG);
		return 0;
	}
	captured->bytes = kvmalloc(length, GFP_KERNEL);
	if (!captured->bytes)
		return -ENOMEM;
	if (!mapple_pslist_task_uses_mm(task, mm)) {
		mapple_pslist_encode_environment_status(
			&captured->wire, MAPPLE_PSLIST_ENVIRONMENT_RACED, ESRCH);
		goto discard;
	}
	copied = access_process_vm(task, start, captured->bytes, length, FOLL_FORCE);
	if (!mapple_pslist_task_uses_mm(task, mm)) {
		mapple_pslist_encode_environment_status(
			&captured->wire, MAPPLE_PSLIST_ENVIRONMENT_RACED, ESRCH);
		goto discard;
	}
	if (copied != (int)length) {
		mapple_pslist_encode_environment_status(
			&captured->wire, MAPPLE_PSLIST_ENVIRONMENT_UNAVAILABLE, EFAULT);
		goto discard;
	}
	for (index = 0; index < length; ++index)
		if (!captured->bytes[index])
			++entry_count;
	if (captured->bytes[length - 1])
		++entry_count;
	else
		flags |= MAPPLE_PSLIST_ENVIRONMENT_FLAG_TERMINATED;
	captured->length = length;
	put_unaligned_be16(MAPPLE_PSLIST_ENVIRONMENT_AVAILABLE, &captured->wire.status);
	put_unaligned_be16(flags, &captured->wire.flags);
	put_unaligned_be32(captured->length, &captured->wire.byte_length);
	put_unaligned_be32(entry_count, &captured->wire.entry_count);
	return 0;

discard:
	kvfree(captured->bytes);
	captured->bytes = NULL;
	return 0;
}

static int mapple_pslist_write_object_begin(struct mapple_command_context *context,
					     u64 object_id)
{
	struct mapple_pslist_object_begin begin = { 0 };

	mapple_pslist_encode_record(&begin.record, sizeof(begin),
				    MAPPLE_PSLIST_STREAM_OBJECT_BEGIN,
				    MAPPLE_PSLIST_GROUP_ENVIRONMENT);
	put_unaligned_be64(object_id, &begin.object_id);
	put_unaligned_be16(MAPPLE_PSLIST_OBJECT_MM, &begin.object_type);
	return mapple_command_write(context, &begin, sizeof(begin));
}

static int mapple_pslist_write_object_end(struct mapple_command_context *context,
					   u64 object_id, u16 status)
{
	struct mapple_pslist_object_end end = { 0 };

	mapple_pslist_encode_record(&end.record, sizeof(end),
				    MAPPLE_PSLIST_STREAM_OBJECT_END,
				    MAPPLE_PSLIST_GROUP_ENVIRONMENT);
	put_unaligned_be64(object_id, &end.object_id);
	put_unaligned_be16(MAPPLE_PSLIST_OBJECT_MM, &end.object_type);
	put_unaligned_be16(status, &end.status);
	put_unaligned_be32(1, &end.record_count);
	return mapple_command_write(context, &end, sizeof(end));
}

int mapple_pslist_write_environment_object(struct mapple_command_context *context,
					    struct task_struct *task,
					    struct mm_struct *mm, u64 object_id,
					    u32 *object_count, bool *partial)
{
	struct mapple_pslist_captured_environment captured = { 0 };
	u16 object_status = MAPPLE_PSLIST_OBJECT_COMPLETE;
	u16 environment_status;
	int ret;

	put_unaligned_be64(object_id, &captured.wire.object_id);
	ret = mapple_pslist_capture_environment(&captured, task, mm);
	if (ret)
		return ret;
	environment_status = get_unaligned_be16(&captured.wire.status);
	if (environment_status != MAPPLE_PSLIST_ENVIRONMENT_AVAILABLE &&
	    environment_status != MAPPLE_PSLIST_ENVIRONMENT_EMPTY) {
		object_status = MAPPLE_PSLIST_OBJECT_PARTIAL;
		*partial = true;
	}
	mapple_pslist_encode_record(&captured.wire.record,
				    sizeof(captured.wire) + captured.length,
				    MAPPLE_PSLIST_STREAM_ENVIRONMENT,
				    MAPPLE_PSLIST_GROUP_ENVIRONMENT);
	ret = mapple_pslist_write_object_begin(context, object_id);
	if (ret)
		goto release;
	++*object_count;
	ret = mapple_command_write(context, &captured.wire, sizeof(captured.wire));
	if (!ret)
		ret = mapple_command_write(context, captured.bytes, captured.length);
	if (!ret)
		ret = mapple_pslist_write_object_end(context, object_id, object_status);

release:
	kvfree(captured.bytes);
	return ret;
}
