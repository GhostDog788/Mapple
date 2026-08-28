#include <control/commands/pslist/context.h>

#include <asm/unaligned.h>
#include <linux/cgroup.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/fs_struct.h>
#include <linux/ipc_namespace.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/nsproxy.h>
#include <linux/pid_namespace.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/time_namespace.h>
#include <linux/user_namespace.h>
#include <linux/utsname.h>

#include <net/net_namespace.h>

struct mapple_pslist_captured_context {
	struct mapple_pslist_context_record wire;
	struct cgroup_namespace *cgroup_ns;
	char *cwd;
	char *root;
	char *cgroup_host_path;
	char *cgroup_namespace_path;
	u32 cwd_length;
	u32 root_length;
	u32 cgroup_host_path_length;
	u32 cgroup_namespace_path_length;
};

static void mapple_pslist_encode_record(struct mapple_pslist_stream_record *record,
					u32 size, u16 type, u64 group)
{
	put_unaligned_be32(size, &record->record_size);
	put_unaligned_be16(type, &record->record_type);
	put_unaligned_be64(group, &record->group);
}

static int mapple_pslist_capture_path(const struct path *path, char **result,
				       u32 *length)
{
	char *start;
	char *buffer;

	buffer = kmalloc(MAPPLE_PSLIST_CONTEXT_PATH_MAX_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	start = d_path(path, buffer, MAPPLE_PSLIST_CONTEXT_PATH_MAX_SIZE);
	if (IS_ERR(start)) {
		kfree(buffer);
		return PTR_ERR(start);
	}
	*length = strlen(start);
	memmove(buffer, start, *length);
	*result = buffer;
	return 0;
}

static void mapple_pslist_capture_filesystem(
	struct mapple_pslist_captured_context *captured, struct task_struct *task)
{
	struct mapple_pslist_context_record *wire = &captured->wire;
	struct path cwd;
	struct path root;
	bool available = false;
	int ret;

	task_lock(task);
	if (task->fs) {
		get_fs_pwd(task->fs, &cwd);
		get_fs_root(task->fs, &root);
		available = true;
	}
	task_unlock(task);
	if (!available) {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE, &wire->cwd_status);
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE, &wire->root_status);
		put_unaligned_be32(ENOENT, &wire->cwd_error);
		put_unaligned_be32(ENOENT, &wire->root_error);
		return;
	}
	ret = mapple_pslist_capture_path(&cwd, &captured->cwd,
					 &captured->cwd_length);
	path_put(&cwd);
	if (ret < 0) {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE, &wire->cwd_status);
		put_unaligned_be32(-ret, &wire->cwd_error);
	} else {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE, &wire->cwd_status);
		put_unaligned_be32(captured->cwd_length, &wire->cwd_length);
	}
	ret = mapple_pslist_capture_path(&root, &captured->root,
					 &captured->root_length);
	path_put(&root);
	if (ret) {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE, &wire->root_status);
		put_unaligned_be32(-ret, &wire->root_error);
	} else {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE, &wire->root_status);
		put_unaligned_be32(captured->root_length, &wire->root_length);
	}
}

static void mapple_pslist_capture_namespaces(
	struct mapple_pslist_captured_context *captured, struct task_struct *task)
{
	struct mapple_pslist_context_record *wire = &captured->wire;
	const struct cred *cred;
	struct nsproxy *nsproxy;
	struct pid_namespace *pid_ns;
	u32 available = 0;

	cred = get_task_cred(task);
	put_unaligned_be32(cred->user_ns->ns.inum, &wire->user_namespace);
	put_cred(cred);
	available |= MAPPLE_PSLIST_NAMESPACE_USER;
	pid_ns = task_active_pid_ns(task);
	put_unaligned_be32(pid_ns->ns.inum, &wire->pid_namespace_active);
	available |= MAPPLE_PSLIST_NAMESPACE_PID_ACTIVE;
	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy) {
		put_unaligned_be32(nsproxy->uts_ns->ns.inum,
				   &wire->uts_namespace);
		put_unaligned_be32(nsproxy->ipc_ns->ns.inum,
				   &wire->ipc_namespace);
		put_unaligned_be32(nsproxy->pid_ns_for_children->ns.inum,
				   &wire->pid_namespace_children);
		put_unaligned_be32(nsproxy->net_ns->ns.inum,
				   &wire->network_namespace);
		put_unaligned_be32(nsproxy->cgroup_ns->ns.inum,
				   &wire->cgroup_namespace);
		put_unaligned_be32(nsproxy->time_ns->ns.inum,
				   &wire->time_namespace);
		put_unaligned_be32(nsproxy->time_ns_for_children->ns.inum,
				   &wire->time_namespace_children);
		captured->cgroup_ns = nsproxy->cgroup_ns;
		get_cgroup_ns(captured->cgroup_ns);
		available |= MAPPLE_PSLIST_NAMESPACE_UTS | MAPPLE_PSLIST_NAMESPACE_IPC |
			     MAPPLE_PSLIST_NAMESPACE_PID_CHILDREN |
			     MAPPLE_PSLIST_NAMESPACE_NETWORK |
			     MAPPLE_PSLIST_NAMESPACE_CGROUP | MAPPLE_PSLIST_NAMESPACE_TIME |
			     MAPPLE_PSLIST_NAMESPACE_TIME_CHILDREN;
	}
	task_unlock(task);
	put_unaligned_be32(available, &wire->namespace_available_mask);
	put_unaligned_be32(MAPPLE_PSLIST_NAMESPACE_MOUNT,
			   &wire->namespace_unsupported_mask);
}

static struct cgroup *mapple_pslist_get_default_cgroup(struct task_struct *task)
{
	struct cgroup *cgroup;

	rcu_read_lock();
	for (;;) {
		cgroup = task_dfl_cgroup(task);
		if (cgroup_tryget(cgroup))
			break;
		cpu_relax();
	}
	rcu_read_unlock();
	return cgroup;
}

static void mapple_pslist_capture_cgroup_path(
	struct cgroup *cgroup, struct cgroup_namespace *cgroup_ns, bool namespaced,
	char **result,
	u32 *length, __u16 *wire_status, __u32 *wire_error, __u32 *wire_length)
{
	char *buffer;
	int ret;

	buffer = kmalloc(MAPPLE_PSLIST_CONTEXT_PATH_MAX_SIZE, GFP_KERNEL);
	if (!buffer) {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE, wire_status);
		put_unaligned_be32(ENOMEM, wire_error);
		return;
	}
	ret = namespaced ? cgroup_path_ns(cgroup, buffer,
					  MAPPLE_PSLIST_CONTEXT_PATH_MAX_SIZE, cgroup_ns) :
			   cgroup_path(cgroup, buffer,
				       MAPPLE_PSLIST_CONTEXT_PATH_MAX_SIZE);
	if (ret < 0) {
		kfree(buffer);
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE, wire_status);
		put_unaligned_be32(-ret, wire_error);
		return;
	}
	*length = strlen(buffer);
	*result = buffer;
	put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE, wire_status);
	put_unaligned_be32(*length, wire_length);
}

static void mapple_pslist_capture_cgroup(
	struct mapple_pslist_captured_context *captured, struct task_struct *task)
{
	struct mapple_pslist_context_record *wire = &captured->wire;
	struct cgroup *cgroup;

	cgroup = mapple_pslist_get_default_cgroup(task);
	if (!cgroup) {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_RACED, &wire->cgroup_status);
		put_unaligned_be32(ESRCH, &wire->cgroup_error);
		return;
	}
	put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE, &wire->cgroup_status);
	put_unaligned_be64(cgroup_id(cgroup), &wire->cgroup_id);
	put_unaligned_be64(cgroup_ino(cgroup), &wire->cgroup_inode);
	put_unaligned_be32(cgroup->root->hierarchy_id, &wire->cgroup_hierarchy_id);
	mapple_pslist_capture_cgroup_path(
		cgroup, NULL, false, &captured->cgroup_host_path,
		&captured->cgroup_host_path_length, &wire->cgroup_host_path_status,
		&wire->cgroup_host_path_error, &wire->cgroup_host_path_length);
	if (captured->cgroup_ns)
		mapple_pslist_capture_cgroup_path(
			cgroup, captured->cgroup_ns, true,
			&captured->cgroup_namespace_path,
			&captured->cgroup_namespace_path_length,
			&wire->cgroup_namespace_path_status,
			&wire->cgroup_namespace_path_error,
			&wire->cgroup_namespace_path_length);
	else {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE,
				   &wire->cgroup_namespace_path_status);
		put_unaligned_be32(ESRCH, &wire->cgroup_namespace_path_error);
	}
	cgroup_put(cgroup);
}

static int mapple_pslist_write_object_begin(struct mapple_command_context *context,
					     u64 object_id)
{
	struct mapple_pslist_object_begin begin = { 0 };

	mapple_pslist_encode_record(&begin.record, sizeof(begin),
				    MAPPLE_PSLIST_STREAM_OBJECT_BEGIN,
				    MAPPLE_PSLIST_GROUP_CONTEXT);
	put_unaligned_be64(object_id, &begin.object_id);
	put_unaligned_be16(MAPPLE_PSLIST_OBJECT_TASK, &begin.object_type);
	return mapple_command_write(context, &begin, sizeof(begin));
}

static int mapple_pslist_write_object_end(struct mapple_command_context *context,
					   u64 object_id)
{
	struct mapple_pslist_object_end end = { 0 };

	mapple_pslist_encode_record(&end.record, sizeof(end),
				    MAPPLE_PSLIST_STREAM_OBJECT_END,
				    MAPPLE_PSLIST_GROUP_CONTEXT);
	put_unaligned_be64(object_id, &end.object_id);
	put_unaligned_be16(MAPPLE_PSLIST_OBJECT_TASK, &end.object_type);
	put_unaligned_be16(MAPPLE_PSLIST_OBJECT_COMPLETE, &end.status);
	put_unaligned_be32(1, &end.record_count);
	return mapple_command_write(context, &end, sizeof(end));
}

int mapple_pslist_write_context_object(struct mapple_command_context *context,
					struct task_struct *task, u64 object_id,
					u32 *object_count)
{
	struct mapple_pslist_captured_context captured = { 0 };
	u64 variable_size;
	int ret;

	put_unaligned_be64(object_id, &captured.wire.object_id);
	mapple_pslist_capture_filesystem(&captured, task);
	mapple_pslist_capture_namespaces(&captured, task);
	mapple_pslist_capture_cgroup(&captured, task);
	variable_size = (u64)captured.cwd_length + captured.root_length +
			captured.cgroup_host_path_length +
			captured.cgroup_namespace_path_length;
	mapple_pslist_encode_record(&captured.wire.record,
				    sizeof(captured.wire) + variable_size,
				    MAPPLE_PSLIST_STREAM_CONTEXT,
				    MAPPLE_PSLIST_GROUP_CONTEXT);
	ret = mapple_pslist_write_object_begin(context, object_id);
	if (ret)
		goto release;
	++*object_count;
	ret = mapple_command_write(context, &captured.wire, sizeof(captured.wire));
	if (!ret)
		ret = mapple_command_write(context, captured.cwd, captured.cwd_length);
	if (!ret)
		ret = mapple_command_write(context, captured.root, captured.root_length);
	if (!ret)
		ret = mapple_command_write(context, captured.cgroup_host_path,
					   captured.cgroup_host_path_length);
	if (!ret)
		ret = mapple_command_write(context, captured.cgroup_namespace_path,
					   captured.cgroup_namespace_path_length);
	if (!ret)
		ret = mapple_pslist_write_object_end(context, object_id);

release:
	if (captured.cgroup_ns)
		put_cgroup_ns(captured.cgroup_ns);
	kfree(captured.cwd);
	kfree(captured.root);
	kfree(captured.cgroup_host_path);
	kfree(captured.cgroup_namespace_path);
	return ret;
}
