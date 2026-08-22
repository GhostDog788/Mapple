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

struct mapple_pslist_record {
	struct mapple_pslist_record_header header;
	char *command_line;
	u32 command_line_length;
};

static struct task_struct *mapple_pslist_next_task(unsigned int *next_pid)
{
	struct task_struct *task = NULL;
	struct pid *pid;
	pid_t nr;

	rcu_read_lock();
	while (*next_pid <= PID_MAX_LIMIT) {
		pid = find_ge_pid(*next_pid, &init_pid_ns);
		if (!pid)
			break;
		nr = pid_nr_ns(pid, &init_pid_ns);
		*next_pid = nr + 1;
		task = pid_task(pid, PIDTYPE_TGID);
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

static int mapple_pslist_capture_record(struct mapple_pslist_record *record, struct task_struct *task)
{
	u32 record_size;
	int ret;

	memset(record, 0, sizeof(*record));
	get_task_comm(record->header.comm, task);
	ret = mapple_pslist_capture_command_line(task, &record->command_line,
						  &record->command_line_length);
	if (ret)
		return ret;
	record_size = sizeof(record->header) + record->command_line_length;
	put_unaligned_be32(record_size, &record->header.record_size);
	put_unaligned_be32(task_tgid_nr_ns(task, &init_pid_ns), &record->header.pid);
	put_unaligned_be32(task_ppid_nr_ns(task, &init_pid_ns), &record->header.ppid);
	put_unaligned_be32(record->command_line_length, &record->header.command_line_length);
	record->header.state = task_state_to_char(task);
	return 0;
}

static int mapple_pslist_write_record(struct mapple_command_context *context,
				      const struct mapple_pslist_record *record)
{
	int ret;

	ret = mapple_command_write(context, &record->header, sizeof(record->header));
	if (ret)
		return ret;
	return mapple_command_write(context, record->command_line, record->command_line_length);
}

int mapple_handle_pslist(struct mapple_command_context *context)
{
	const struct mapple_pslist_record_header terminator = { 0 };
	struct mapple_pslist_record record;
	struct task_struct *task;
	unsigned int next_pid = 1;
	int ret;

	BUILD_BUG_ON(MAPPLE_PSLIST_COMM_SIZE != TASK_COMM_LEN);
	if (context->payload_streaming || context->remaining_payload)
		return -EMSGSIZE;
	ret = mapple_command_write_response_header(context, MAPPLE_COMMAND_PSLIST,
						   MAPPLE_HEADER_FLAG_STREAM, 0);
	if (ret)
		return ret;

	while ((task = mapple_pslist_next_task(&next_pid))) {
		ret = mapple_pslist_capture_record(&record, task);
		put_task_struct(task);
		if (ret)
			return ret;
		ret = mapple_pslist_write_record(context, &record);
		kvfree(record.command_line);
		if (ret)
			return ret;
	}
	return mapple_command_write(context, &terminator, sizeof(terminator));
}
