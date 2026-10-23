#include <control/commands/pslist/files/descriptors.h>
#include <control/commands/pslist/files/network.h>

#include <asm/unaligned.h>
#include <linux/anon_inodes.h>
#include <linux/bitmap.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/limits.h>
#include <linux/magic.h>
#include <linux/net.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/uidgid.h>
#include <linux/uio.h>
#include <linux/user_namespace.h>

#define MAPPLE_PSLIST_FD_SNAPSHOT_RETRIES 3
#define MAPPLE_PSLIST_FDINFO_INITIAL_SIZE PAGE_SIZE

struct mapple_pslist_fd_reference {
	struct file *file;
	u32 fd;
	u32 descriptor_flags;
	u32 open_flags;
};

struct mapple_pslist_fd_snapshot {
	struct mapple_pslist_fd_reference *entries;
	size_t count;
	u16 status;
};

struct mapple_pslist_captured_fd {
	struct mapple_pslist_fd_record generic;
	struct mapple_pslist_fd_details_record details;
	struct mapple_pslist_captured_socket network;
	char *path;
	char *fdinfo;
	u32 path_length;
	u32 fdinfo_length;
	bool socket;
};

static void mapple_pslist_encode_timespec(struct mapple_pslist_timespec *wire,
					  const struct timespec64 *value)
{
	put_unaligned_be64((u64)value->tv_sec, &wire->seconds);
	put_unaligned_be32(value->tv_nsec, &wire->nanoseconds);
}

static void mapple_pslist_release_snapshot(struct mapple_pslist_fd_snapshot *snapshot)
{
	size_t index;

	for (index = 0; index < snapshot->count; ++index)
		fput(snapshot->entries[index].file);
	kvfree(snapshot->entries);
}

static int mapple_pslist_capture_snapshot_once(struct task_struct *task,
					       struct files_struct *expected_files,
					       struct mapple_pslist_fd_snapshot *snapshot,
					       bool accept_partial, bool *retry)
{
	struct mapple_pslist_fd_reference *entries = NULL;
	struct files_struct *files;
	struct fdtable *table;
	size_t capacity;
	size_t count = 0;
	unsigned int fd;

	*retry = false;
	task_lock(task);
	if (task->files != expected_files) {
		task_unlock(task);
		snapshot->status = MAPPLE_PSLIST_OBJECT_RACED;
		return 0;
	}
	spin_lock(&expected_files->file_lock);
	table = files_fdtable(expected_files);
	capacity = bitmap_weight(table->open_fds, table->max_fds);
	spin_unlock(&expected_files->file_lock);
	task_unlock(task);

	if (capacity) {
		entries = kvmalloc_array(capacity, sizeof(*entries), GFP_KERNEL);
		if (!entries)
			return -ENOMEM;
	}

	task_lock(task);
	files = task->files;
	if (files != expected_files) {
		task_unlock(task);
		kvfree(entries);
		*retry = true;
		return 0;
	}
	spin_lock(&files->file_lock);
	table = files_fdtable(files);
	for (fd = 0; fd < table->max_fds; ++fd) {
		struct file *file = files_lookup_fd_locked(files, fd);

		if (!file)
			continue;
		if (count == capacity) {
			*retry = true;
			break;
		}
		get_file(file);
		entries[count].file = file;
		entries[count].fd = fd;
		entries[count].descriptor_flags = close_on_exec(fd, table) ?
			MAPPLE_PSLIST_FD_DESCRIPTOR_CLOEXEC : 0;
		entries[count].open_flags = READ_ONCE(file->f_flags);
		++count;
	}
	spin_unlock(&files->file_lock);
	task_unlock(task);

	if (*retry && !accept_partial) {
		while (count)
			fput(entries[--count].file);
		kvfree(entries);
		return 0;
	}
	snapshot->entries = entries;
	snapshot->count = count;
	snapshot->status = *retry ? MAPPLE_PSLIST_OBJECT_PARTIAL :
					    MAPPLE_PSLIST_OBJECT_COMPLETE;
	return 0;
}

static int mapple_pslist_capture_snapshot(struct task_struct *task,
					  struct files_struct *expected_files,
					  struct mapple_pslist_fd_snapshot *snapshot)
{
	bool retry;
	int attempt;
	int ret;

	memset(snapshot, 0, sizeof(*snapshot));
	for (attempt = 0; attempt < MAPPLE_PSLIST_FD_SNAPSHOT_RETRIES; ++attempt) {
		ret = mapple_pslist_capture_snapshot_once(
			task, expected_files, snapshot,
			attempt == MAPPLE_PSLIST_FD_SNAPSHOT_RETRIES - 1, &retry);
		if (ret || !retry)
			return ret;
	}
	return 0;
}

static bool mapple_pslist_fd_still_matches(struct task_struct *task, unsigned int fd,
					    struct file *expected)
{
	struct files_struct *files;
	struct file *current_file = NULL;
	bool matches;

	task_lock(task);
	files = task->files;
	if (files) {
		spin_lock(&files->file_lock);
		current_file = files_lookup_fd_locked(files, fd);
		spin_unlock(&files->file_lock);
	}
	matches = current_file == expected;
	task_unlock(task);
	return matches;
}

static void mapple_pslist_capture_path(struct mapple_pslist_captured_fd *captured,
				       struct file *file)
{
	char *path;
	char *buffer;
	int error;

	buffer = kvmalloc(PATH_MAX, GFP_KERNEL);
	if (!buffer) {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE,
				   &captured->generic.path_status);
		put_unaligned_be32(ENOMEM, &captured->generic.path_error);
		return;
	}
	path = file_path(file, buffer, PATH_MAX);
	if (IS_ERR(path)) {
		error = -PTR_ERR(path);
		kvfree(buffer);
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE,
				   &captured->generic.path_status);
		put_unaligned_be32(error, &captured->generic.path_error);
		return;
	}
	captured->path_length = strlen(path);
	memmove(buffer, path, captured->path_length);
	captured->path = buffer;
	put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE, &captured->generic.path_status);
	put_unaligned_be32(captured->path_length, &captured->generic.path_length);
}

static void mapple_pslist_capture_stat(struct mapple_pslist_captured_fd *captured,
				       struct file *file)
{
	struct mapple_pslist_fd_stat *wire = &captured->details.stat;
	struct kstat stat;
	int ret;

	ret = vfs_getattr_nosec(&file->f_path, &stat,
				 STATX_BASIC_STATS | STATX_BTIME | STATX_MNT_ID | STATX_DIOALIGN,
				 AT_STATX_DONT_SYNC);
	if (ret) {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE,
				   &captured->generic.identity_status);
		put_unaligned_be32(-ret, &captured->generic.identity_error);
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE,
				   &captured->details.stat_status);
		put_unaligned_be32(-ret, &captured->details.stat_error);
		return;
	}
	put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE,
			   &captured->generic.identity_status);
	put_unaligned_be32(MAJOR(stat.dev), &captured->generic.device_major);
	put_unaligned_be32(MINOR(stat.dev), &captured->generic.device_minor);
	put_unaligned_be64(stat.ino, &captured->generic.inode);
	put_unaligned_be64(stat.mnt_id, &captured->generic.mount_id);

	put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE, &captured->details.stat_status);
	put_unaligned_be32(stat.result_mask, &wire->result_mask);
	put_unaligned_be32(stat.mode, &wire->mode);
	put_unaligned_be32(stat.nlink, &wire->link_count);
	put_unaligned_be32(stat.blksize, &wire->block_size);
	put_unaligned_be32(from_kuid_munged(&init_user_ns, stat.uid), &wire->uid);
	put_unaligned_be32(from_kgid_munged(&init_user_ns, stat.gid), &wire->gid);
	put_unaligned_be32(MAJOR(stat.rdev), &wire->special_device_major);
	put_unaligned_be32(MINOR(stat.rdev), &wire->special_device_minor);
	put_unaligned_be32(stat.dio_mem_align, &wire->direct_io_memory_alignment);
	put_unaligned_be32(stat.dio_offset_align, &wire->direct_io_offset_alignment);
	put_unaligned_be64((u64)stat.size, &wire->size);
	put_unaligned_be64(stat.blocks, &wire->blocks);
	put_unaligned_be64(stat.attributes, &wire->attributes);
	put_unaligned_be64(stat.attributes_mask, &wire->attributes_mask);
	put_unaligned_be64(stat.change_cookie, &wire->change_cookie);
	put_unaligned_be64(file_inode(file)->i_sb->s_magic, &wire->filesystem_magic);
	mapple_pslist_encode_timespec(&wire->access_time, &stat.atime);
	mapple_pslist_encode_timespec(&wire->birth_time, &stat.btime);
	mapple_pslist_encode_timespec(&wire->change_time, &stat.ctime);
	mapple_pslist_encode_timespec(&wire->modification_time, &stat.mtime);
}

static u16 mapple_pslist_fd_kind(struct file *file)
{
	umode_t mode = file_inode(file)->i_mode;

	if (sock_from_file(file))
		return MAPPLE_PSLIST_FD_SOCKET;
	if (file_inode(file)->i_sb->s_magic == ANON_INODE_FS_MAGIC)
		return MAPPLE_PSLIST_FD_ANON_INODE;
	if (S_ISREG(mode))
		return MAPPLE_PSLIST_FD_REGULAR;
	if (S_ISDIR(mode))
		return MAPPLE_PSLIST_FD_DIRECTORY;
	if (S_ISCHR(mode))
		return MAPPLE_PSLIST_FD_CHARACTER;
	if (S_ISBLK(mode))
		return MAPPLE_PSLIST_FD_BLOCK;
	if (S_ISFIFO(mode))
		return MAPPLE_PSLIST_FD_PIPE;
	if (S_ISLNK(mode))
		return MAPPLE_PSLIST_FD_SYMLINK;
	return MAPPLE_PSLIST_FD_OTHER;
}

static void mapple_pslist_capture_credentials(struct mapple_pslist_fd_details_record *wire,
					      struct file *file)
{
	const struct cred *cred = file->f_cred;
	struct fown_struct *owner = &file->f_owner;
	pid_t owner_pid;

	put_unaligned_be32(from_kuid_munged(&init_user_ns, cred->uid), &wire->credential_uid);
	put_unaligned_be32(from_kgid_munged(&init_user_ns, cred->gid), &wire->credential_gid);
	put_unaligned_be32(from_kuid_munged(&init_user_ns, cred->euid),
			   &wire->credential_effective_uid);
	put_unaligned_be32(from_kgid_munged(&init_user_ns, cred->egid),
			   &wire->credential_effective_gid);
	put_unaligned_be32(from_kuid_munged(&init_user_ns, cred->suid),
			   &wire->credential_saved_uid);
	put_unaligned_be32(from_kgid_munged(&init_user_ns, cred->sgid),
			   &wire->credential_saved_gid);
	put_unaligned_be32(from_kuid_munged(&init_user_ns, cred->fsuid),
			   &wire->credential_filesystem_uid);
	put_unaligned_be32(from_kgid_munged(&init_user_ns, cred->fsgid),
			   &wire->credential_filesystem_gid);

	read_lock_irq(&owner->lock);
	owner_pid = pid_nr_ns(owner->pid, &init_pid_ns);
	put_unaligned_be32((u32)owner_pid, &wire->async_owner_pid);
	put_unaligned_be16(owner->pid_type, &wire->async_owner_type);
	put_unaligned_be16(owner->signum, &wire->async_signal);
	put_unaligned_be32(from_kuid_munged(&init_user_ns, owner->uid),
			   &wire->async_owner_uid);
	put_unaligned_be32(from_kuid_munged(&init_user_ns, owner->euid),
			   &wire->async_owner_effective_uid);
	read_unlock_irq(&owner->lock);
}

static ssize_t mapple_pslist_read_fdinfo_file(struct file *file, void *buffer,
					       size_t size, loff_t *position)
{
	struct kvec vector = { .iov_base = buffer, .iov_len = size };
	struct iov_iter iterator;
	struct kiocb request;
	ssize_t ret;

	if (file->f_op->read != seq_read)
		return -EOPNOTSUPP;
	init_sync_kiocb(&request, file);
	request.ki_pos = *position;
	iov_iter_kvec(&iterator, ITER_DEST, &vector, 1, size);
	ret = seq_read_iter(&request, &iterator);
	if (ret > 0)
		*position = request.ki_pos;
	return ret;
}

static void mapple_pslist_capture_fdinfo(struct mapple_pslist_captured_fd *captured,
					 struct task_struct *task, unsigned int fd,
					 struct file *expected)
{
	struct file *proc_file;
	char proc_path[64];
	char *buffer = NULL;
	size_t capacity = 0;
	size_t count = 0;
	loff_t position = 0;
	ssize_t read_size;
	int error = 0;

	if (!mapple_pslist_fd_still_matches(task, fd, expected)) {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_RACED,
				   &captured->details.fdinfo_status);
		return;
	}
	snprintf(proc_path, sizeof(proc_path), "/proc/%u/fdinfo/%u",
		 task_pid_nr_ns(task, &init_pid_ns), fd);
	proc_file = filp_open(proc_path, O_RDONLY | O_CLOEXEC, 0);
	if (IS_ERR(proc_file)) {
		error = -PTR_ERR(proc_file);
		goto done;
	}
	capacity = MAPPLE_PSLIST_FDINFO_INITIAL_SIZE;
	buffer = kvmalloc(capacity, GFP_KERNEL);
	if (!buffer) {
		error = ENOMEM;
		goto close;
	}
	for (;;) {
		if (count == capacity) {
			size_t new_capacity;
			char *new_buffer;

			if (capacity == MAPPLE_PSLIST_FDINFO_MAX_SIZE + 1U)
				break;
			new_capacity = min_t(size_t, capacity * 2,
					     MAPPLE_PSLIST_FDINFO_MAX_SIZE + 1U);
			new_buffer = kvrealloc(buffer, capacity, new_capacity, GFP_KERNEL);
			if (!new_buffer) {
				error = ENOMEM;
				goto close;
			}
			buffer = new_buffer;
			capacity = new_capacity;
		}
		read_size = mapple_pslist_read_fdinfo_file(
			proc_file, buffer + count, capacity - count, &position);
		if (read_size < 0) {
			error = -read_size;
			goto close;
		}
		if (!read_size)
			break;
		count += read_size;
	}

close:
	filp_close(proc_file, NULL);
done:
	if (!mapple_pslist_fd_still_matches(task, fd, expected)) {
		kvfree(buffer);
		put_unaligned_be16(MAPPLE_PSLIST_DATA_RACED,
				   &captured->details.fdinfo_status);
		return;
	}
	if (error) {
		kvfree(buffer);
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE,
				   &captured->details.fdinfo_status);
		put_unaligned_be32(error, &captured->details.fdinfo_error);
		return;
	}
	if (count > MAPPLE_PSLIST_FDINFO_MAX_SIZE) {
		kvfree(buffer);
		put_unaligned_be16(MAPPLE_PSLIST_DATA_TOO_LARGE,
				   &captured->details.fdinfo_status);
		return;
	}
	captured->fdinfo = buffer;
	captured->fdinfo_length = count;
	put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE, &captured->details.fdinfo_status);
	put_unaligned_be32(count, &captured->details.fdinfo_length);
}

static void mapple_pslist_capture_fd(struct mapple_pslist_captured_fd *captured,
				     struct task_struct *task, u64 fds_object_id,
				     u64 network_object_id, u64 groups,
				     const struct mapple_pslist_fd_reference *reference)
{
	struct file *file = reference->file;

	memset(captured, 0, sizeof(*captured));
	put_unaligned_be64(fds_object_id, &captured->generic.object_id);
	put_unaligned_be32(reference->fd, &captured->generic.fd);
	put_unaligned_be16(mapple_pslist_fd_kind(file), &captured->generic.kind);
	put_unaligned_be32(reference->descriptor_flags, &captured->generic.descriptor_flags);
	put_unaligned_be32(reference->open_flags, &captured->generic.open_flags);
	mapple_pslist_capture_path(captured, file);
	mapple_pslist_capture_stat(captured, file);
	mapple_pslist_encode_record(&captured->generic.record,
				    sizeof(captured->generic) + captured->path_length,
				    MAPPLE_PSLIST_STREAM_FD, MAPPLE_PSLIST_GROUP_FDS);

	if (groups & MAPPLE_PSLIST_GROUP_FDS_DETAILS) {
		put_unaligned_be64(fds_object_id, &captured->details.object_id);
		put_unaligned_be32(reference->fd, &captured->details.fd);
		put_unaligned_be64(file->f_mode, &captured->details.file_mode);
		put_unaligned_be64((u64)file->f_pos, &captured->details.position);
		put_unaligned_be64(file->f_version, &captured->details.version);
		mapple_pslist_capture_credentials(&captured->details, file);
		mapple_pslist_capture_fdinfo(captured, task, reference->fd, file);
		mapple_pslist_encode_record(&captured->details.record,
					    sizeof(captured->details) + captured->fdinfo_length,
					    MAPPLE_PSLIST_STREAM_FD_DETAILS,
					    MAPPLE_PSLIST_GROUP_FDS_DETAILS);
	}
	if (groups & MAPPLE_PSLIST_GROUP_NETWORK)
		captured->socket = mapple_pslist_capture_socket(&captured->network, file,
								network_object_id, reference->fd);
}

static void mapple_pslist_release_captured_fd(struct mapple_pslist_captured_fd *captured)
{
	kvfree(captured->path);
	kvfree(captured->fdinfo);
}

static int mapple_pslist_write_captured_fd(struct mapple_pslist_writer *writer,
					   const struct mapple_pslist_captured_fd *captured,
					   u64 groups)
{
	int ret;

	ret = mapple_pslist_write(writer, &captured->generic, sizeof(captured->generic));
	if (!ret)
		ret = mapple_pslist_write(writer, captured->path, captured->path_length);
	if (ret)
		return ret;
	if (groups & MAPPLE_PSLIST_GROUP_FDS_DETAILS) {
		ret = mapple_pslist_write(writer, &captured->details, sizeof(captured->details));
		if (!ret)
			ret = mapple_pslist_write(writer, captured->fdinfo,
						  captured->fdinfo_length);
		if (ret)
			return ret;
	}
	if (!(groups & MAPPLE_PSLIST_GROUP_NETWORK) || !captured->socket)
		return 0;
	return mapple_pslist_write_socket(writer, &captured->network);
}

int mapple_pslist_write_files_objects(struct mapple_pslist_writer *writer, struct task_struct *task,
				      struct files_struct *expected_files, u64 fds_object_id,
				      u64 network_object_id, u64 groups)
{
	struct mapple_pslist_fd_snapshot snapshot;
	struct mapple_pslist_captured_fd *captured;
	u32 network_count = 0;
	size_t index;
	int ret;

	ret = mapple_pslist_capture_snapshot(task, expected_files, &snapshot);
	if (ret)
		return ret;
	if (snapshot.status != MAPPLE_PSLIST_OBJECT_COMPLETE)
		writer->partial = true;
	ret = mapple_pslist_write_object_begin(writer, fds_object_id, MAPPLE_PSLIST_OBJECT_FILES,
					       MAPPLE_PSLIST_GROUP_FDS);
	if (ret)
		goto release_snapshot;
	if (groups & MAPPLE_PSLIST_GROUP_NETWORK) {
		ret = mapple_pslist_write_object_begin(writer, network_object_id,
						       MAPPLE_PSLIST_OBJECT_FILES,
						       MAPPLE_PSLIST_GROUP_NETWORK);
		if (ret)
			goto release_snapshot;
	}
	captured = kmalloc(sizeof(*captured), GFP_KERNEL);
	if (!captured) {
		ret = -ENOMEM;
		goto release_snapshot;
	}
	for (index = 0; index < snapshot.count; ++index) {
		mapple_pslist_capture_fd(captured, task, fds_object_id, network_object_id, groups,
					 &snapshot.entries[index]);
		ret = mapple_pslist_write_captured_fd(writer, captured, groups);
		if (captured->socket)
			++network_count;
		mapple_pslist_release_captured_fd(captured);
		if (ret)
			goto free_captured;
	}
	ret = mapple_pslist_write_object_end(writer, fds_object_id, MAPPLE_PSLIST_OBJECT_FILES,
					     MAPPLE_PSLIST_GROUP_FDS, snapshot.status, 0,
					     snapshot.count);
	if (!ret && (groups & MAPPLE_PSLIST_GROUP_NETWORK))
		ret = mapple_pslist_write_object_end(writer, network_object_id,
						     MAPPLE_PSLIST_OBJECT_FILES,
						     MAPPLE_PSLIST_GROUP_NETWORK, snapshot.status,
						     0, network_count);

free_captured:
	kfree(captured);
release_snapshot:
	mapple_pslist_release_snapshot(&snapshot);
	return ret;
}
