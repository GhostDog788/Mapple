#include <control/commands/pslist/pslist.h>

#include <asm/unaligned.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/threads.h>

struct mapple_pslist_captured_record {
	struct mapple_pslist_process_record process;
	struct mapple_pslist_task_record task;
	char *command_line;
	char *executable_path;
	u32 command_line_length;
	u32 executable_path_length;
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

static struct file *mapple_pslist_get_executable(struct task_struct *task, u16 *status)
{
	struct file *executable = NULL;
	struct mm_struct *mm;
	bool reference_failed = false;

	if (task->flags & PF_KTHREAD) {
		*status = MAPPLE_PSLIST_EXECUTABLE_KERNEL_THREAD;
		return NULL;
	}
	mm = get_task_mm(task);
	if (!mm) {
		*status = MAPPLE_PSLIST_EXECUTABLE_UNAVAILABLE;
		return NULL;
	}
	rcu_read_lock();
	executable = rcu_dereference(mm->exe_file);
	if (executable && !get_file_rcu(executable)) {
		executable = NULL;
		reference_failed = true;
	}
	rcu_read_unlock();
	mmput(mm);
	if (executable)
		*status = MAPPLE_PSLIST_EXECUTABLE_AVAILABLE;
	else
		*status = reference_failed ? MAPPLE_PSLIST_EXECUTABLE_UNAVAILABLE :
					     MAPPLE_PSLIST_EXECUTABLE_NONE;
	return executable;
}

static void mapple_pslist_capture_executable_path(struct mapple_pslist_captured_record *record,
						  struct file *executable, u32 *flags)
{
	char *path;
	char *buffer;
	size_t length;

	buffer = kvmalloc(PATH_MAX, GFP_KERNEL);
	if (!buffer)
		goto unavailable;
	path = file_path(executable, buffer, PATH_MAX);
	if (IS_ERR(path)) {
		kvfree(buffer);
		goto unavailable;
	}
	length = strlen(path);
	memmove(buffer, path, length);
	record->executable_path = buffer;
	record->executable_path_length = length;
	return;

unavailable:
	*flags |= MAPPLE_PSLIST_EXECUTABLE_FLAG_PATH_UNAVAILABLE;
}

static void mapple_pslist_capture_executable_metadata(
	struct mapple_pslist_process_record *process, struct file *executable, u32 *flags)
{
	struct kstat stat;
	int ret;

	ret = vfs_getattr_nosec(&executable->f_path, &stat, STATX_BASIC_STATS,
				 AT_STATX_DONT_SYNC);
	if (ret) {
		*flags |= MAPPLE_PSLIST_EXECUTABLE_FLAG_METADATA_UNAVAILABLE;
		return;
	}
	put_unaligned_be32(MAJOR(stat.dev), &process->executable_device_major);
	put_unaligned_be32(MINOR(stat.dev), &process->executable_device_minor);
	put_unaligned_be32(stat.mode, &process->executable_mode);
	put_unaligned_be64(stat.ino, &process->executable_inode);
	put_unaligned_be64(stat.size, &process->executable_size);
	put_unaligned_be64((u64)timespec64_to_ns(&stat.mtime), &process->executable_mtime_ns);
	put_unaligned_be64((u64)timespec64_to_ns(&stat.ctime), &process->executable_ctime_ns);
	put_unaligned_be64(file_inode(executable)->i_sb->s_magic,
			   &process->executable_filesystem_magic);
}

static void mapple_pslist_capture_executable(struct mapple_pslist_captured_record *record,
					     struct task_struct *task)
{
	struct mapple_pslist_process_record *process = &record->process;
	struct file *executable;
	u16 executable_status;
	u32 executable_flags = 0;

	executable = mapple_pslist_get_executable(task, &executable_status);
	put_unaligned_be16(executable_status, &process->executable_status);
	if (executable) {
		if (d_unlinked(executable->f_path.dentry))
			executable_flags |= MAPPLE_PSLIST_EXECUTABLE_FLAG_DELETED;
		mapple_pslist_capture_executable_path(record, executable, &executable_flags);
		mapple_pslist_capture_executable_metadata(process, executable, &executable_flags);
		fput(executable);
	}
	put_unaligned_be32(executable_flags, &process->executable_flags);
	put_unaligned_be32(record->executable_path_length, &process->executable_path_length);
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
	put_unaligned_be32(tgid, &record->process.tgid);
	put_unaligned_be32(task_ppid_nr_ns(task, &init_pid_ns), &record->process.ppid);
	put_unaligned_be64(task->start_boottime, &record->process.start_boottime_ns);
	put_unaligned_be32(record->command_line_length, &record->process.command_line_length);
	mapple_pslist_capture_executable(record, task);
	record_size = sizeof(record->process) + record->command_line_length +
		      record->executable_path_length;
	mapple_pslist_encode_header(&record->process.header, record_size,
				    MAPPLE_PSLIST_RECORD_PROCESS);
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
		ret = mapple_command_write(context, record->executable_path,
					   record->executable_path_length);
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
		kvfree(record.executable_path);
		if (ret)
			return ret;
	}
	return mapple_command_write(context, &terminator, sizeof(terminator));
}
