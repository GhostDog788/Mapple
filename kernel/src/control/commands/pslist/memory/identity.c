#include <control/commands/pslist/memory/identity.h>

#include <asm/unaligned.h>
#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/time64.h>

struct mapple_pslist_captured_identity {
	struct mapple_pslist_identity_record wire;
	char *command_line;
	char *executable_path;
	u32 command_line_length;
	u32 executable_path_length;
};

static bool mapple_pslist_task_uses_mm(struct task_struct *task, struct mm_struct *mm)
{
	bool matches;

	task_lock(task);
	matches = task->mm == mm;
	task_unlock(task);
	return matches;
}

static int mapple_pslist_capture_command_line(struct mapple_pslist_captured_identity *captured,
					       struct task_struct *task, struct mm_struct *mm)
{
	struct mapple_pslist_process_data *identity = &captured->wire.identity;
	unsigned long arg_start;
	unsigned long arg_end;
	unsigned long length;
	char *buffer;
	int copied;
	u16 status;

	spin_lock(&mm->arg_lock);
	arg_start = mm->arg_start;
	arg_end = mm->arg_end;
	spin_unlock(&mm->arg_lock);
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
	if (!mapple_pslist_task_uses_mm(task, mm)) {
		kvfree(buffer);
		status = MAPPLE_PSLIST_COMMAND_LINE_UNAVAILABLE;
		goto encode;
	}
	copied = access_process_vm(task, arg_start, buffer, length, FOLL_FORCE);
	if (copied != (int)length || !mapple_pslist_task_uses_mm(task, mm)) {
		kvfree(buffer);
		status = MAPPLE_PSLIST_COMMAND_LINE_UNAVAILABLE;
		goto encode;
	}
	captured->command_line = buffer;
	captured->command_line_length = length;
	status = MAPPLE_PSLIST_COMMAND_LINE_AVAILABLE;

encode:
	put_unaligned_be16(status, &identity->command_line_status);
	put_unaligned_be32(captured->command_line_length, &identity->command_line_length);
	return 0;
}

static struct file *mapple_pslist_get_executable(struct mm_struct *mm, u16 *status)
{
	struct file *executable;
	bool reference_failed = false;

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

static int mapple_pslist_capture_executable_path(struct mapple_pslist_captured_identity *captured,
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
	captured->executable_path = buffer;
	captured->executable_path_length = length;
	return 0;
}

static void mapple_pslist_capture_executable_metadata(struct mapple_pslist_process_data *identity,
						       struct file *executable, u32 *flags)
{
	struct kstat stat;
	int ret;

	ret = vfs_getattr_nosec(&executable->f_path, &stat, STATX_BASIC_STATS,
				 AT_STATX_DONT_SYNC);
	if (ret) {
		*flags |= MAPPLE_PSLIST_EXECUTABLE_FLAG_METADATA_UNAVAILABLE;
		return;
	}
	put_unaligned_be32(MAJOR(stat.dev), &identity->executable_device_major);
	put_unaligned_be32(MINOR(stat.dev), &identity->executable_device_minor);
	put_unaligned_be32(stat.mode, &identity->executable_mode);
	put_unaligned_be64(stat.ino, &identity->executable_inode);
	put_unaligned_be64(stat.size, &identity->executable_size);
	put_unaligned_be64((u64)timespec64_to_ns(&stat.mtime), &identity->executable_mtime_ns);
	put_unaligned_be64((u64)timespec64_to_ns(&stat.ctime), &identity->executable_ctime_ns);
	put_unaligned_be64(file_inode(executable)->i_sb->s_magic,
			   &identity->executable_filesystem_magic);
}

static int mapple_pslist_capture_executable(struct mapple_pslist_captured_identity *captured,
					     struct mm_struct *mm)
{
	struct mapple_pslist_process_data *identity = &captured->wire.identity;
	struct file *executable;
	u16 executable_status;
	u32 executable_flags = 0;
	int ret;

	executable = mapple_pslist_get_executable(mm, &executable_status);
	put_unaligned_be16(executable_status, &identity->executable_status);
	if (executable) {
		if (d_unlinked(executable->f_path.dentry))
			executable_flags |= MAPPLE_PSLIST_EXECUTABLE_FLAG_DELETED;
		ret = mapple_pslist_capture_executable_path(captured, executable,
						      &executable_flags);
		if (ret) {
			fput(executable);
			return ret;
		}
		mapple_pslist_capture_executable_metadata(identity, executable,
						   &executable_flags);
		fput(executable);
	}
	put_unaligned_be32(executable_flags, &identity->executable_flags);
	put_unaligned_be32(captured->executable_path_length,
			   &identity->executable_path_length);
	return 0;
}

int mapple_pslist_write_identity_object(struct mapple_pslist_writer *writer,
					struct task_struct *task, struct mm_struct *mm,
					u64 object_id)
{
	struct mapple_pslist_captured_identity captured = { 0 };
	u16 status = MAPPLE_PSLIST_OBJECT_COMPLETE;
	int ret;

	ret = mapple_pslist_write_object_begin(writer, object_id, MAPPLE_PSLIST_OBJECT_MM,
					       MAPPLE_PSLIST_GROUP_IDENTITY);
	if (ret)
		return ret;
	put_unaligned_be64(object_id, &captured.wire.object_id);
	ret = mapple_pslist_capture_command_line(&captured, task, mm);
	if (!ret)
		ret = mapple_pslist_capture_executable(&captured, mm);
	if (ret)
		goto release;
	if (get_unaligned_be16(&captured.wire.identity.command_line_status) ==
		    MAPPLE_PSLIST_COMMAND_LINE_UNAVAILABLE ||
	    get_unaligned_be16(&captured.wire.identity.executable_status) ==
		    MAPPLE_PSLIST_EXECUTABLE_UNAVAILABLE) {
		status = MAPPLE_PSLIST_OBJECT_PARTIAL;
		writer->partial = true;
	}
	mapple_pslist_encode_record(&captured.wire.record,
				    sizeof(captured.wire) + captured.command_line_length +
					    captured.executable_path_length,
				    MAPPLE_PSLIST_STREAM_IDENTITY, MAPPLE_PSLIST_GROUP_IDENTITY);
	ret = mapple_pslist_write(writer, &captured.wire, sizeof(captured.wire));
	if (!ret)
		ret = mapple_pslist_write(writer, captured.command_line,
					  captured.command_line_length);
	if (!ret)
		ret = mapple_pslist_write(writer, captured.executable_path,
					  captured.executable_path_length);
	if (!ret)
		ret = mapple_pslist_write_object_end(writer, object_id, MAPPLE_PSLIST_OBJECT_MM,
						     MAPPLE_PSLIST_GROUP_IDENTITY, status, 0, 1);

release:
	kvfree(captured.command_line);
	kvfree(captured.executable_path);
	return ret;
}
