#include <control/commands/pslist/pslist.h>

#include <control/commands/pslist/fd.h>

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
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/threads.h>

struct mapple_pslist_mm_source {
	struct task_struct *task;
	struct mm_struct *mm;
};

struct mapple_pslist_captured_record {
	struct mapple_pslist_record_header header;
	struct mapple_pslist_process_data process;
	char *command_line;
	char *executable_path;
	u32 command_line_length;
	u32 executable_path_length;
	u16 type;
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

/* find_lock_task_mm() provides this operation but is not exported to modules. */
static void mapple_pslist_get_mm_source(struct task_struct *leader,
					struct mapple_pslist_mm_source *source)
{
	struct task_struct *task;

	source->task = NULL;
	source->mm = NULL;
	rcu_read_lock();
	for_each_thread(leader, task) {
		task_lock(task);
		if (!(task->flags & PF_KTHREAD) && task->mm) {
			get_task_struct(task);
			mmget(task->mm);
			source->task = task;
			source->mm = task->mm;
			task_unlock(task);
			break;
		}
		task_unlock(task);
	}
	rcu_read_unlock();
}

static void mapple_pslist_put_mm_source(struct mapple_pslist_mm_source *source)
{
	if (!source->mm)
		return;
	mmput(source->mm);
	put_task_struct(source->task);
}

static bool mapple_pslist_source_uses_mm(const struct mapple_pslist_mm_source *source)
{
	bool matches;

	task_lock(source->task);
	matches = source->task->mm == source->mm;
	task_unlock(source->task);
	return matches;
}

static int mapple_pslist_capture_command_line(struct mapple_pslist_captured_record *record,
					       const struct mapple_pslist_mm_source *source)
{
	struct mapple_pslist_process_data *process = &record->process;
	unsigned long arg_start;
	unsigned long arg_end;
	unsigned long length;
	char *buffer;
	int copied;
	u16 status;

	if (!source->mm) {
		status = MAPPLE_PSLIST_COMMAND_LINE_UNAVAILABLE;
		goto encode;
	}
	spin_lock(&source->mm->arg_lock);
	arg_start = source->mm->arg_start;
	arg_end = source->mm->arg_end;
	spin_unlock(&source->mm->arg_lock);
	if (!arg_end || arg_end < arg_start) {
		status = MAPPLE_PSLIST_COMMAND_LINE_UNAVAILABLE;
		goto encode;
	}
	if (arg_end == arg_start) {
		status = MAPPLE_PSLIST_COMMAND_LINE_EMPTY;
		goto encode;
	}
	length = arg_end - arg_start;
	if (length > MAPPLE_PSLIST_COMMAND_LINE_MAX_SIZE) {
		status = MAPPLE_PSLIST_COMMAND_LINE_TOO_LARGE;
		goto encode;
	}
	buffer = kvmalloc(length, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	if (!mapple_pslist_source_uses_mm(source)) {
		kvfree(buffer);
		status = MAPPLE_PSLIST_COMMAND_LINE_UNAVAILABLE;
		goto encode;
	}
	copied = access_process_vm(source->task, arg_start, buffer, length, FOLL_FORCE);
	if (copied != (int)length || !mapple_pslist_source_uses_mm(source)) {
		kvfree(buffer);
		status = MAPPLE_PSLIST_COMMAND_LINE_UNAVAILABLE;
		goto encode;
	}
	record->command_line = buffer;
	record->command_line_length = length;
	status = MAPPLE_PSLIST_COMMAND_LINE_AVAILABLE;

encode:
	put_unaligned_be16(status, &process->command_line_status);
	put_unaligned_be32(record->command_line_length, &process->command_line_length);
	return 0;
}

static struct file *mapple_pslist_get_executable(struct mm_struct *mm, u16 *status)
{
	struct file *executable;
	bool reference_failed = false;

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
	if (executable)
		*status = MAPPLE_PSLIST_EXECUTABLE_AVAILABLE;
	else
		*status = reference_failed ? MAPPLE_PSLIST_EXECUTABLE_UNAVAILABLE :
					     MAPPLE_PSLIST_EXECUTABLE_NONE;
	return executable;
}

static int mapple_pslist_capture_executable_path(struct mapple_pslist_captured_record *record,
						  struct file *executable, u32 *flags)
{
	char *path;
	char *buffer;
	size_t length;

	buffer = kvmalloc(PATH_MAX, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	path = file_path(executable, buffer, PATH_MAX);
	if (IS_ERR(path)) {
		kvfree(buffer);
		*flags |= MAPPLE_PSLIST_EXECUTABLE_FLAG_PATH_UNAVAILABLE;
		return 0;
	}
	length = strlen(path);
	memmove(buffer, path, length);
	record->executable_path = buffer;
	record->executable_path_length = length;
	return 0;
}

static void mapple_pslist_capture_executable_metadata(
	struct mapple_pslist_process_data *process, struct file *executable, u32 *flags)
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

static int mapple_pslist_capture_executable(struct mapple_pslist_captured_record *record,
					     struct mm_struct *mm)
{
	struct mapple_pslist_process_data *process = &record->process;
	struct file *executable;
	u16 executable_status;
	u32 executable_flags = 0;
	int ret;

	executable = mapple_pslist_get_executable(mm, &executable_status);
	put_unaligned_be16(executable_status, &process->executable_status);
	if (executable) {
		if (d_unlinked(executable->f_path.dentry))
			executable_flags |= MAPPLE_PSLIST_EXECUTABLE_FLAG_DELETED;
		ret = mapple_pslist_capture_executable_path(record, executable,
							      &executable_flags);
		if (ret) {
			fput(executable);
			return ret;
		}
		mapple_pslist_capture_executable_metadata(process, executable,
						    &executable_flags);
		fput(executable);
	}
	put_unaligned_be32(executable_flags, &process->executable_flags);
	put_unaligned_be32(record->executable_path_length, &process->executable_path_length);
	return 0;
}

static void mapple_pslist_capture_task_data(struct mapple_pslist_captured_record *record,
					    struct task_struct *task, pid_t pid, pid_t tgid)
{
	struct mapple_pslist_task_data *data = &record->header.task;

	put_unaligned_be32(pid, &data->pid);
	put_unaligned_be32(tgid, &data->tgid);
	put_unaligned_be32(task_ppid_nr_ns(task, &init_pid_ns), &data->ppid);
	put_unaligned_be64(task->start_boottime, &data->start_boottime_ns);
	data->state = task_state_to_char(task);
	get_task_comm(data->comm, task);
}

static int mapple_pslist_capture_process(struct mapple_pslist_captured_record *record,
					  struct task_struct *task)
{
	struct mapple_pslist_mm_source source;
	u32 record_size;
	int ret;

	mapple_pslist_get_mm_source(task, &source);
	ret = mapple_pslist_capture_command_line(record, &source);
	if (!ret)
		ret = mapple_pslist_capture_executable(record, source.mm);
	mapple_pslist_put_mm_source(&source);
	if (ret)
		return ret;
	record_size = sizeof(record->header) + sizeof(record->process) +
		      record->command_line_length +
		      record->executable_path_length;
	mapple_pslist_encode_header(&record->header, record_size, record->type);
	return 0;
}

static int mapple_pslist_capture_record(struct mapple_pslist_captured_record *record,
					struct task_struct *task)
{
	pid_t tgid;
	pid_t pid;

	memset(record, 0, sizeof(*record));
	pid = task_pid_nr_ns(task, &init_pid_ns);
	tgid = task_tgid_nr_ns(task, &init_pid_ns);
	if (!pid || !tgid)
		return -ESRCH;
	if (task->flags & PF_KTHREAD)
		record->type = MAPPLE_PSLIST_RECORD_KTHREAD;
	else if (pid == tgid)
		record->type = MAPPLE_PSLIST_RECORD_PROCESS;
	else
		record->type = MAPPLE_PSLIST_RECORD_THREAD;
	mapple_pslist_capture_task_data(record, task, pid, tgid);
	if (record->type == MAPPLE_PSLIST_RECORD_PROCESS)
		return mapple_pslist_capture_process(record, task);
	mapple_pslist_encode_header(&record->header, sizeof(record->header), record->type);
	return 0;
}

static void mapple_pslist_release_record(struct mapple_pslist_captured_record *record)
{
	kvfree(record->command_line);
	kvfree(record->executable_path);
}

static int mapple_pslist_write_record(struct mapple_command_context *context,
				       const struct mapple_pslist_captured_record *record)
{
	int ret;

	ret = mapple_command_write(context, &record->header, sizeof(record->header));
	if (ret || record->type != MAPPLE_PSLIST_RECORD_PROCESS)
		return ret;
	ret = mapple_command_write(context, &record->process, sizeof(record->process));
	if (ret)
		return ret;
	ret = mapple_command_write(context, record->command_line,
				   record->command_line_length);
	if (ret)
		return ret;
	return mapple_command_write(context, record->executable_path,
				    record->executable_path_length);
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
		if (ret == -ESRCH) {
			put_task_struct(task);
			continue;
		}
		if (!ret)
			ret = mapple_pslist_write_record(context, &record);
		if (!ret && record.type == MAPPLE_PSLIST_RECORD_PROCESS)
			ret = mapple_pslist_write_fd_table(context, task);
		put_task_struct(task);
		mapple_pslist_release_record(&record);
		if (ret)
			return ret;
	}
	return mapple_command_write(context, &terminator, sizeof(terminator));
}
