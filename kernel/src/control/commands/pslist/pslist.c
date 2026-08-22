#include <control/commands/pslist/pslist.h>

#include <asm/unaligned.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/threads.h>

struct mapple_pslist_captured_record {
	struct mapple_pslist_process_record process;
	struct mapple_pslist_task_record task;
	char *command_line;
	u32 command_line_length;
	bool has_process;
};

static void mapple_pslist_encode_header(struct mapple_pslist_record_header *header, u32 record_size,
					u16 record_type)
{
	put_unaligned_be32(record_size, &header->record_size);
	put_unaligned_be16(record_type, &header->record_type);
}

static struct task_struct *mapple_pslist_next_task(unsigned int *next_tid)
{
	struct task_struct *task = NULL;
	struct pid *pid;
	pid_t nr;

	rcu_read_lock();
	while (*next_tid <= PID_MAX_LIMIT) {
		pid = find_ge_pid(*next_tid, &init_pid_ns);
		if (!pid)
			break;
		nr = pid_nr_ns(pid, &init_pid_ns);
		*next_tid = nr + 1;
		task = pid_task(pid, PIDTYPE_PID);
		if (task) {
			get_task_struct(task);
			break;
		}
	}
	rcu_read_unlock();
	return task;
}

static int mapple_pslist_capture_command_line(struct task_struct *task, char **buffer_out,
						      u32 *length_out)
{
	struct mm_struct *mm;
	unsigned long arg_start;
	unsigned long arg_end;
	unsigned long length;
	char *buffer;
	int copied;

	*buffer_out = NULL;
	*length_out = 0;
	mm = get_task_mm(task);
	if (!mm)
		return 0;
	spin_lock(&mm->arg_lock);
	arg_start = mm->arg_start;
	arg_end = mm->arg_end;
	spin_unlock(&mm->arg_lock);
	if (arg_end <= arg_start) {
		mmput(mm);
		return 0;
	}
	length = arg_end - arg_start;
	if (length > INT_MAX) {
		mmput(mm);
		return -EOVERFLOW;
	}
	buffer = kvmalloc(length, GFP_KERNEL);
	if (!buffer) {
		mmput(mm);
		return -ENOMEM;
	}
	copied = access_process_vm(task, arg_start, buffer, length, FOLL_FORCE);
	mmput(mm);
	if (copied < 0) {
		kvfree(buffer);
		return copied;
	}
	if (copied != (int)length) {
		kvfree(buffer);
		return 0;
	}
	*buffer_out = buffer;
	*length_out = length;
	return 0;
}

static int mapple_pslist_capture_record(struct mapple_pslist_captured_record *record,
					struct task_struct *task)
{
	pid_t tgid;
	pid_t tid;
	u32 record_size;
	int ret;

	memset(record, 0, sizeof(*record));
	tid = task_pid_nr_ns(task, &init_pid_ns);
	tgid = task_tgid_nr_ns(task, &init_pid_ns);
	if (!tid || !tgid)
		return -ESRCH;

	mapple_pslist_encode_header(&record->task.header, sizeof(record->task),
				    MAPPLE_PSLIST_RECORD_TASK);
	put_unaligned_be32(tid, &record->task.tid);
	put_unaligned_be32(tgid, &record->task.tgid);
	record->task.state = task_state_to_char(task);
	get_task_comm(record->task.comm, task);
	if (tid != tgid)
		return 0;

	ret = mapple_pslist_capture_command_line(task, &record->command_line,
						  &record->command_line_length);
	if (ret)
		return ret;
	record_size = sizeof(record->process) + record->command_line_length;
	mapple_pslist_encode_header(&record->process.header, record_size,
				    MAPPLE_PSLIST_RECORD_PROCESS);
	put_unaligned_be32(tgid, &record->process.tgid);
	put_unaligned_be32(task_ppid_nr_ns(task, &init_pid_ns), &record->process.ppid);
	put_unaligned_be32(record->command_line_length, &record->process.command_line_length);
	record->has_process = true;
	return 0;
}

static int mapple_pslist_write_records(struct mapple_command_context *context,
				       const struct mapple_pslist_captured_record *record)
{
	int ret;

	if (record->has_process) {
		ret = mapple_command_write(context, &record->process, sizeof(record->process));
		if (ret)
			return ret;
		ret = mapple_command_write(context, record->command_line,
					   record->command_line_length);
		if (ret)
			return ret;
	}
	return mapple_command_write(context, &record->task, sizeof(record->task));
}

int mapple_handle_pslist(struct mapple_command_context *context)
{
	const struct mapple_pslist_record_header terminator = { 0 };
	struct mapple_pslist_captured_record record;
	struct task_struct *task;
	unsigned int next_tid = 1;
	int ret;

	BUILD_BUG_ON(MAPPLE_PSLIST_COMM_SIZE != TASK_COMM_LEN);
	if (context->payload_streaming || context->remaining_payload)
		return -EMSGSIZE;
	ret = mapple_command_write_response_header(context, MAPPLE_COMMAND_PSLIST,
						   MAPPLE_HEADER_FLAG_STREAM, 0);
	if (ret)
		return ret;

	while ((task = mapple_pslist_next_task(&next_tid))) {
		ret = mapple_pslist_capture_record(&record, task);
		put_task_struct(task);
		if (ret == -ESRCH)
			continue;
		if (ret)
			return ret;
		ret = mapple_pslist_write_records(context, &record);
		kvfree(record.command_line);
		if (ret)
			return ret;
	}
	return mapple_command_write(context, &terminator, sizeof(terminator));
}
