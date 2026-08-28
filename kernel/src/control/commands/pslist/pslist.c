#include <control/commands/pslist/pslist.h>

#include <asm/unaligned.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
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
#include <linux/timekeeping.h>
#include <linux/threads.h>

#include <control/commands/pslist/fd.h>

#define MAPPLE_PSLIST_OBJECT_HASH_BITS 8

enum mapple_pslist_group_index {
	MAPPLE_PSLIST_GROUP_INDEX_IDENTITY,
	MAPPLE_PSLIST_GROUP_INDEX_FDS,
	MAPPLE_PSLIST_GROUP_INDEX_NETWORK,
	MAPPLE_PSLIST_GROUP_INDEX_COUNT,
};

struct mapple_pslist_group {
	u64 next_object_id;
};

struct mapple_pslist_snapshot_object {
	struct hlist_node node;
	const void *key;
	u64 group_ids[MAPPLE_PSLIST_GROUP_INDEX_COUNT];
	u16 type;
	struct mm_struct *mm;
};

struct mapple_pslist_snapshot {
	DECLARE_HASHTABLE(objects, MAPPLE_PSLIST_OBJECT_HASH_BITS);
	u64 requested_groups;
	u64 effective_groups;
	struct mapple_pslist_group groups[MAPPLE_PSLIST_GROUP_INDEX_COUNT];
	u32 task_count;
	u32 object_count;
	bool partial;
};

struct mapple_pslist_captured_identity {
	struct mapple_pslist_identity_record wire;
	char *command_line;
	char *executable_path;
	u32 command_line_length;
	u32 executable_path_length;
};

static void mapple_pslist_encode_record(struct mapple_pslist_stream_record *record,
					u32 size, u16 type, u64 group)
{
	put_unaligned_be32(size, &record->record_size);
	put_unaligned_be16(type, &record->record_type);
	put_unaligned_be64(group, &record->group);
}

static u64 mapple_pslist_effective_groups(u64 requested)
{
	if (requested & (MAPPLE_PSLIST_GROUP_FDS_DETAILS | MAPPLE_PSLIST_GROUP_NETWORK))
		requested |= MAPPLE_PSLIST_GROUP_FDS;
	return requested;
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

static unsigned long mapple_pslist_object_hash(const void *key, u16 type)
{
	return (unsigned long)key ^ type;
}

static struct mapple_pslist_snapshot_object *
mapple_pslist_find_object(struct mapple_pslist_snapshot *snapshot, const void *key, u16 type)
{
	struct mapple_pslist_snapshot_object *object;
	unsigned long hash = mapple_pslist_object_hash(key, type);

	hash_for_each_possible(snapshot->objects, object, node, hash) {
		if (object->key == key && object->type == type)
			return object;
	}
	return NULL;
}

static int mapple_pslist_add_object(struct mapple_pslist_snapshot *snapshot, const void *key,
				    u16 type, struct mm_struct *mm,
				    struct mapple_pslist_snapshot_object **result)
{
	struct mapple_pslist_snapshot_object *object;

	object = kzalloc(sizeof(*object), GFP_KERNEL);
	if (!object)
		return -ENOMEM;
	object->key = key;
	object->type = type;
	object->mm = mm;
	hash_add(snapshot->objects, &object->node,
		 mapple_pslist_object_hash(key, type));
	*result = object;
	return 0;
}

static u64 mapple_pslist_group_object_id(struct mapple_pslist_snapshot *snapshot,
					 struct mapple_pslist_snapshot_object *object,
					 enum mapple_pslist_group_index group_index)
{
	u64 *object_id = &object->group_ids[group_index];

	if (!*object_id)
		*object_id = snapshot->groups[group_index].next_object_id++;
	return *object_id;
}

static void mapple_pslist_release_snapshot(struct mapple_pslist_snapshot *snapshot)
{
	struct mapple_pslist_snapshot_object *object;
	struct hlist_node *temporary;
	unsigned int bucket;

	hash_for_each_safe(snapshot->objects, bucket, temporary, object, node) {
		hash_del(&object->node);
		if (object->mm)
			mmput(object->mm);
		kfree(object);
	}
}

static int mapple_pslist_write_error(struct mapple_command_context *context, u32 error_code)
{
	struct mapple_pslist_error_response response = { 0 };
	int ret;

	put_unaligned_be32(error_code, &response.error_code);
	put_unaligned_be64(MAPPLE_PSLIST_SUPPORTED_GROUPS, &response.supported_groups);
	ret = mapple_command_write_response_header(context, MAPPLE_COMMAND_PSLIST, 0,
					   sizeof(response));
	if (ret)
		return ret;
	return mapple_command_write(context, &response, sizeof(response));
}

static int mapple_pslist_write_snapshot_begin(struct mapple_command_context *context,
					       struct mapple_pslist_snapshot *snapshot)
{
	struct mapple_pslist_snapshot_begin begin = { 0 };

	mapple_pslist_encode_record(&begin.record, sizeof(begin),
				    MAPPLE_PSLIST_STREAM_SNAPSHOT_BEGIN, 0);
	put_unaligned_be64(snapshot->requested_groups, &begin.requested_groups);
	put_unaligned_be64(snapshot->effective_groups, &begin.effective_groups);
	put_unaligned_be64(MAPPLE_PSLIST_SUPPORTED_GROUPS, &begin.supported_groups);
	put_unaligned_be64(ktime_get_boottime_ns(), &begin.start_boottime_ns);
	return mapple_command_write(context, &begin, sizeof(begin));
}

static int mapple_pslist_write_snapshot_end(struct mapple_command_context *context,
					     const struct mapple_pslist_snapshot *snapshot)
{
	struct mapple_pslist_snapshot_end end = { 0 };

	mapple_pslist_encode_record(&end.record, sizeof(end),
				    MAPPLE_PSLIST_STREAM_SNAPSHOT_END, 0);
	put_unaligned_be16(snapshot->partial ? MAPPLE_PSLIST_SNAPSHOT_PARTIAL :
					       MAPPLE_PSLIST_SNAPSHOT_COMPLETE,
			   &end.status);
	put_unaligned_be32(snapshot->task_count, &end.task_count);
	put_unaligned_be32(snapshot->object_count, &end.object_count);
	put_unaligned_be64(ktime_get_boottime_ns(), &end.end_boottime_ns);
	return mapple_command_write(context, &end, sizeof(end));
}

static int mapple_pslist_task_type(struct task_struct *task, pid_t pid, pid_t tgid)
{
	if (task->flags & PF_KTHREAD)
		return MAPPLE_PSLIST_TASK_KTHREAD;
	if (pid == tgid)
		return MAPPLE_PSLIST_TASK_PROCESS;
	return MAPPLE_PSLIST_TASK_THREAD;
}

static int mapple_pslist_write_task(struct mapple_command_context *context,
				    struct task_struct *task, pid_t pid, pid_t tgid)
{
	struct mapple_pslist_task_record record = { 0 };
	u16 task_type;

	task_type = mapple_pslist_task_type(task, pid, tgid);
	mapple_pslist_encode_record(&record.record, sizeof(record),
				    MAPPLE_PSLIST_STREAM_TASK, 0);
	put_unaligned_be16(task_type, &record.task_type);
	put_unaligned_be32(pid, &record.task.pid);
	put_unaligned_be32(tgid, &record.task.tgid);
	put_unaligned_be32(task_ppid_nr_ns(task, &init_pid_ns), &record.task.ppid);
	put_unaligned_be64(task->start_boottime, &record.task.start_boottime_ns);
	record.task.state = task_state_to_char(task);
	get_task_comm(record.task.comm, task);
	return mapple_command_write(context, &record, sizeof(record));
}

static int mapple_pslist_write_link(struct mapple_command_context *context, pid_t pid,
				    u64 object_id, u16 object_type, u64 group)
{
	struct mapple_pslist_task_object_link link = { 0 };

	mapple_pslist_encode_record(&link.record, sizeof(link),
				    MAPPLE_PSLIST_STREAM_TASK_OBJECT_LINK, group);
	put_unaligned_be32(pid, &link.pid);
	put_unaligned_be64(object_id, &link.object_id);
	put_unaligned_be16(object_type, &link.object_type);
	return mapple_command_write(context, &link, sizeof(link));
}

static int mapple_pslist_write_object_begin(struct mapple_command_context *context,
					    u64 object_id, u16 object_type, u64 group)
{
	struct mapple_pslist_object_begin begin = { 0 };

	mapple_pslist_encode_record(&begin.record, sizeof(begin),
				    MAPPLE_PSLIST_STREAM_OBJECT_BEGIN, group);
	put_unaligned_be64(object_id, &begin.object_id);
	put_unaligned_be16(object_type, &begin.object_type);
	return mapple_command_write(context, &begin, sizeof(begin));
}

static int mapple_pslist_write_object_end(struct mapple_command_context *context,
					  u64 object_id, u16 object_type, u64 group,
					  u16 status, u32 count)
{
	struct mapple_pslist_object_end end = { 0 };

	mapple_pslist_encode_record(&end.record, sizeof(end),
				    MAPPLE_PSLIST_STREAM_OBJECT_END, group);
	put_unaligned_be64(object_id, &end.object_id);
	put_unaligned_be16(object_type, &end.object_type);
	put_unaligned_be16(status, &end.status);
	put_unaligned_be32(count, &end.record_count);
	return mapple_command_write(context, &end, sizeof(end));
}

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

static int mapple_pslist_write_identity_object(struct mapple_command_context *context,
					       struct task_struct *task, struct mm_struct *mm,
					       u64 object_id, u32 *object_count,
					       bool *partial)
{
	struct mapple_pslist_captured_identity captured = { 0 };
	u16 status = MAPPLE_PSLIST_OBJECT_COMPLETE;
	int ret;

	ret = mapple_pslist_write_object_begin(context, object_id, MAPPLE_PSLIST_OBJECT_MM,
					       MAPPLE_PSLIST_GROUP_IDENTITY);
	if (ret)
		return ret;
	++*object_count;
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
		*partial = true;
	}
	mapple_pslist_encode_record(&captured.wire.record,
				    sizeof(captured.wire) + captured.command_line_length +
				    captured.executable_path_length,
				    MAPPLE_PSLIST_STREAM_IDENTITY,
				    MAPPLE_PSLIST_GROUP_IDENTITY);
	ret = mapple_command_write(context, &captured.wire, sizeof(captured.wire));
	if (!ret)
		ret = mapple_command_write(context, captured.command_line,
					   captured.command_line_length);
	if (!ret)
		ret = mapple_command_write(context, captured.executable_path,
					   captured.executable_path_length);
	if (!ret)
		ret = mapple_pslist_write_object_end(context, object_id,
					     MAPPLE_PSLIST_OBJECT_MM,
					     MAPPLE_PSLIST_GROUP_IDENTITY, status, 1);

release:
	kvfree(captured.command_line);
	kvfree(captured.executable_path);
	return ret;
}

static int mapple_pslist_process_mm(struct mapple_command_context *context,
				    struct mapple_pslist_snapshot *snapshot,
				    struct task_struct *task, pid_t pid)
{
	struct mapple_pslist_snapshot_object *object;
	struct mm_struct *mm;
	u64 object_id;
	bool is_new = false;
	int ret;

	if (!(snapshot->effective_groups & MAPPLE_PSLIST_GROUP_IDENTITY))
		return 0;
	mm = get_task_mm(task);
	if (!mm)
		return 0;
	object = mapple_pslist_find_object(snapshot, mm, MAPPLE_PSLIST_OBJECT_MM);
	if (!object) {
		ret = mapple_pslist_add_object(snapshot, mm, MAPPLE_PSLIST_OBJECT_MM, mm,
					       &object);
		if (ret) {
			mmput(mm);
			return ret;
		}
		is_new = true;
	} else {
		mmput(mm);
	}
	object_id = mapple_pslist_group_object_id(snapshot, object,
						  MAPPLE_PSLIST_GROUP_INDEX_IDENTITY);
	ret = mapple_pslist_write_link(context, pid, object_id,
				       MAPPLE_PSLIST_OBJECT_MM,
				       MAPPLE_PSLIST_GROUP_IDENTITY);
	if (!ret && is_new)
		ret = mapple_pslist_write_identity_object(context, task, object->mm,
						  object_id, &snapshot->object_count,
						  &snapshot->partial);
	return ret;
}

static struct files_struct *mapple_pslist_task_files(struct task_struct *task)
{
	struct files_struct *files;

	task_lock(task);
	files = task->files;
	task_unlock(task);
	return files;
}

static int mapple_pslist_write_files_links(struct mapple_command_context *context,
					   pid_t pid, u64 fds_object_id,
					   u64 network_object_id, u64 groups)
{
	int ret;

	ret = mapple_pslist_write_link(context, pid, fds_object_id,
				       MAPPLE_PSLIST_OBJECT_FILES,
				       MAPPLE_PSLIST_GROUP_FDS);
	if (!ret && (groups & MAPPLE_PSLIST_GROUP_NETWORK))
		ret = mapple_pslist_write_link(context, pid, network_object_id,
					       MAPPLE_PSLIST_OBJECT_FILES,
					       MAPPLE_PSLIST_GROUP_NETWORK);
	return ret;
}

static int mapple_pslist_process_files(struct mapple_command_context *context,
				       struct mapple_pslist_snapshot *snapshot,
				       struct task_struct *task, pid_t pid)
{
	struct mapple_pslist_snapshot_object *object;
	struct files_struct *files;
	u64 network_object_id = 0;
	u64 fds_object_id;
	bool is_new = false;
	int ret;

	if (!(snapshot->effective_groups & MAPPLE_PSLIST_GROUP_FDS))
		return 0;
	files = mapple_pslist_task_files(task);
	if (!files)
		return 0;
	object = mapple_pslist_find_object(snapshot, files, MAPPLE_PSLIST_OBJECT_FILES);
	if (!object) {
		ret = mapple_pslist_add_object(snapshot, files, MAPPLE_PSLIST_OBJECT_FILES,
					       NULL, &object);
		if (ret)
			return ret;
		is_new = true;
	}
	fds_object_id = mapple_pslist_group_object_id(snapshot, object,
						     MAPPLE_PSLIST_GROUP_INDEX_FDS);
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_NETWORK)
		network_object_id = mapple_pslist_group_object_id(
			snapshot, object, MAPPLE_PSLIST_GROUP_INDEX_NETWORK);
	ret = mapple_pslist_write_files_links(context, pid, fds_object_id,
					      network_object_id, snapshot->effective_groups);
	if (!ret && is_new)
		ret = mapple_pslist_write_files_objects(context, task, files,
						 fds_object_id, network_object_id,
						 snapshot->effective_groups,
						 &snapshot->object_count,
						 &snapshot->partial);
	return ret;
}

static int mapple_pslist_process_task(struct mapple_command_context *context,
				      struct mapple_pslist_snapshot *snapshot,
				      struct task_struct *task)
{
	pid_t tgid;
	pid_t pid;
	int ret;

	pid = task_pid_nr_ns(task, &init_pid_ns);
	tgid = task_tgid_nr_ns(task, &init_pid_ns);
	if (!pid || !tgid)
		return -ESRCH;
	ret = mapple_pslist_write_task(context, task, pid, tgid);
	if (ret)
		return ret;
	++snapshot->task_count;
	ret = mapple_pslist_process_mm(context, snapshot, task, pid);
	if (!ret)
		ret = mapple_pslist_process_files(context, snapshot, task, pid);
	return ret;
}

int mapple_handle_pslist(struct mapple_command_context *context)
{
	struct mapple_pslist_snapshot *snapshot;
	struct mapple_pslist_request request;
	struct task_struct *task;
	unsigned int next_tid = 1;
	unsigned int group_index;
	u64 requested_groups;
	int ret;

	BUILD_BUG_ON(MAPPLE_PSLIST_COMM_SIZE != TASK_COMM_LEN);
	if (context->payload_streaming ||
	    context->remaining_payload != sizeof(request))
		return -EMSGSIZE;
	ret = mapple_command_read(context, &request, sizeof(request));
	if (ret)
		return ret;
	requested_groups = get_unaligned_be64(&request.groups);
	if (requested_groups & ~MAPPLE_PSLIST_KNOWN_GROUPS)
		return mapple_pslist_write_error(context, EINVAL);
	if (requested_groups & ~MAPPLE_PSLIST_SUPPORTED_GROUPS)
		return mapple_pslist_write_error(context, EOPNOTSUPP);
	snapshot = kzalloc(sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;
	for (group_index = 0; group_index < MAPPLE_PSLIST_GROUP_INDEX_COUNT;
	     ++group_index)
		snapshot->groups[group_index].next_object_id = 1;
	snapshot->requested_groups = requested_groups;
	snapshot->effective_groups = mapple_pslist_effective_groups(requested_groups);
	hash_init(snapshot->objects);
	ret = mapple_command_write_response_header(context, MAPPLE_COMMAND_PSLIST,
					   MAPPLE_HEADER_FLAG_STREAM, 0);
	if (ret)
		goto free_snapshot;
	ret = mapple_pslist_write_snapshot_begin(context, snapshot);
	while (!ret && (task = mapple_pslist_next_task(&next_tid))) {
		ret = mapple_pslist_process_task(context, snapshot, task);
		if (ret == -ESRCH) {
			snapshot->partial = true;
			ret = 0;
		}
		put_task_struct(task);
	}
	if (!ret)
		ret = mapple_pslist_write_snapshot_end(context, snapshot);
	mapple_pslist_release_snapshot(snapshot);
free_snapshot:
	kfree(snapshot);
	return ret;
}
