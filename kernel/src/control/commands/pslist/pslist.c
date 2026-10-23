#include <control/commands/pslist/pslist.h>

#include <asm/unaligned.h>
#include <linux/err.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/rcupdate.h>
#include <linux/threads.h>

#include <control/commands/pslist/elf/objects.h>
#include <control/commands/pslist/files/descriptors.h>
#include <control/commands/pslist/memory/environment.h>
#include <control/commands/pslist/memory/identity.h>
#include <control/commands/pslist/memory/maps.h>
#include <control/commands/pslist/snapshot/state.h>
#include <control/commands/pslist/snapshot/stream.h>
#include <control/commands/pslist/task/context.h>
#include <control/commands/pslist/task/resources.h>
#include <control/commands/pslist/task/security.h>

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

static int mapple_pslist_process_security(struct mapple_pslist_snapshot *snapshot,
					  struct task_struct *task, pid_t pid)
{
	struct mapple_pslist_writer *writer = &snapshot->writer;
	u64 object_id;
	int ret;

	if (!(snapshot->effective_groups & MAPPLE_PSLIST_GROUP_SECURITY))
		return 0;
	object_id = mapple_pslist_next_object_id(snapshot, MAPPLE_PSLIST_GROUP_INDEX_SECURITY);
	ret = mapple_pslist_write_link(writer, pid, object_id, MAPPLE_PSLIST_OBJECT_TASK,
				       MAPPLE_PSLIST_GROUP_SECURITY);
	if (!ret)
		ret = mapple_pslist_write_security_object(writer, task,
							  &snapshot->security_metadata, object_id);
	return ret;
}

static int mapple_pslist_process_context(struct mapple_pslist_snapshot *snapshot,
					 struct task_struct *task, pid_t pid)
{
	struct mapple_pslist_writer *writer = &snapshot->writer;
	u64 object_id;
	int ret;

	if (!(snapshot->effective_groups & MAPPLE_PSLIST_GROUP_CONTEXT))
		return 0;
	object_id = mapple_pslist_next_object_id(snapshot, MAPPLE_PSLIST_GROUP_INDEX_CONTEXT);
	ret = mapple_pslist_write_link(writer, pid, object_id, MAPPLE_PSLIST_OBJECT_TASK,
				       MAPPLE_PSLIST_GROUP_CONTEXT);
	if (!ret)
		ret = mapple_pslist_write_context_object(writer, task, object_id);
	return ret;
}

static int mapple_pslist_process_resources(struct mapple_pslist_snapshot *snapshot,
					   struct task_struct *task, pid_t pid)
{
	struct mapple_pslist_writer *writer = &snapshot->writer;
	u64 object_id;
	int ret;

	if (!(snapshot->effective_groups & MAPPLE_PSLIST_GROUP_RESOURCES))
		return 0;
	object_id = mapple_pslist_next_object_id(snapshot, MAPPLE_PSLIST_GROUP_INDEX_RESOURCES);
	ret = mapple_pslist_write_link(writer, pid, object_id, MAPPLE_PSLIST_OBJECT_TASK,
				       MAPPLE_PSLIST_GROUP_RESOURCES);
	if (!ret)
		ret = mapple_pslist_write_resources_object(writer, task, object_id);
	return ret;
}

static int mapple_pslist_process_mm(struct mapple_pslist_snapshot *snapshot,
				    struct task_struct *task, pid_t pid)
{
	struct mapple_pslist_snapshot_object *object;
	struct mapple_pslist_writer *writer = &snapshot->writer;
	struct mapple_pslist_elf_registry *elf_registry = NULL;
	u64 identity_object_id = 0;
	u64 environment_object_id = 0;
	u64 memory_maps_object_id = 0;
	u64 groups;
	bool is_new = false;
	int ret;

	groups = snapshot->effective_groups &
		 (MAPPLE_PSLIST_GROUP_IDENTITY | MAPPLE_PSLIST_GROUP_ENVIRONMENT |
		  MAPPLE_PSLIST_GROUP_MEMORY_MAPS);
	if (!groups)
		return 0;
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_ELF)
		elf_registry = &snapshot->elf_registry;
	object = mapple_pslist_snapshot_mm(snapshot, task, &is_new);
	if (IS_ERR_OR_NULL(object))
		return PTR_ERR_OR_ZERO(object);
	if (groups & MAPPLE_PSLIST_GROUP_IDENTITY)
		identity_object_id = mapple_pslist_group_object_id(
			snapshot, object, MAPPLE_PSLIST_GROUP_INDEX_IDENTITY);
	if (groups & MAPPLE_PSLIST_GROUP_ENVIRONMENT)
		environment_object_id = mapple_pslist_group_object_id(
			snapshot, object, MAPPLE_PSLIST_GROUP_INDEX_ENVIRONMENT);
	if (groups & MAPPLE_PSLIST_GROUP_MEMORY_MAPS)
		memory_maps_object_id = mapple_pslist_group_object_id(
			snapshot, object, MAPPLE_PSLIST_GROUP_INDEX_MEMORY_MAPS);
	ret = 0;
	if (groups & MAPPLE_PSLIST_GROUP_IDENTITY)
		ret = mapple_pslist_write_link(writer, pid, identity_object_id,
					       MAPPLE_PSLIST_OBJECT_MM,
					       MAPPLE_PSLIST_GROUP_IDENTITY);
	if (!ret && (groups & MAPPLE_PSLIST_GROUP_ENVIRONMENT))
		ret = mapple_pslist_write_link(writer, pid, environment_object_id,
					       MAPPLE_PSLIST_OBJECT_MM,
					       MAPPLE_PSLIST_GROUP_ENVIRONMENT);
	if (!ret && (groups & MAPPLE_PSLIST_GROUP_MEMORY_MAPS))
		ret = mapple_pslist_write_link(writer, pid, memory_maps_object_id,
					       MAPPLE_PSLIST_OBJECT_MM,
					       MAPPLE_PSLIST_GROUP_MEMORY_MAPS);
	if (!ret && is_new && (groups & MAPPLE_PSLIST_GROUP_IDENTITY))
		ret = mapple_pslist_write_identity_object(writer, task, object->mm,
							  identity_object_id);
	if (!ret && is_new && (groups & MAPPLE_PSLIST_GROUP_ENVIRONMENT))
		ret = mapple_pslist_write_environment_object(writer, task, object->mm,
							     environment_object_id);
	if (!ret && is_new && (groups & MAPPLE_PSLIST_GROUP_MEMORY_MAPS))
		ret = mapple_pslist_write_memory_maps_object(
			writer, object->mm, memory_maps_object_id, elf_registry);
	return ret;
}

static int mapple_pslist_write_files_links(struct mapple_pslist_writer *writer, pid_t pid,
					   u64 fds_object_id, u64 network_object_id, u64 groups)
{
	int ret;

	ret = mapple_pslist_write_link(writer, pid, fds_object_id, MAPPLE_PSLIST_OBJECT_FILES,
				       MAPPLE_PSLIST_GROUP_FDS);
	if (!ret && (groups & MAPPLE_PSLIST_GROUP_NETWORK))
		ret = mapple_pslist_write_link(writer, pid, network_object_id,
					       MAPPLE_PSLIST_OBJECT_FILES,
					       MAPPLE_PSLIST_GROUP_NETWORK);
	return ret;
}

static int mapple_pslist_process_files(struct mapple_pslist_snapshot *snapshot,
				       struct task_struct *task, pid_t pid)
{
	struct mapple_pslist_snapshot_object *object;
	struct mapple_pslist_writer *writer = &snapshot->writer;
	u64 network_object_id = 0;
	u64 fds_object_id;
	bool is_new = false;
	int ret;

	if (!(snapshot->effective_groups & MAPPLE_PSLIST_GROUP_FDS))
		return 0;
	object = mapple_pslist_snapshot_files(snapshot, task, &is_new);
	if (IS_ERR_OR_NULL(object))
		return PTR_ERR_OR_ZERO(object);
	fds_object_id =
		mapple_pslist_group_object_id(snapshot, object, MAPPLE_PSLIST_GROUP_INDEX_FDS);
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_NETWORK)
		network_object_id = mapple_pslist_group_object_id(
			snapshot, object, MAPPLE_PSLIST_GROUP_INDEX_NETWORK);
	ret = mapple_pslist_write_files_links(writer, pid, fds_object_id, network_object_id,
					      snapshot->effective_groups);
	if (!ret && is_new)
		ret = mapple_pslist_write_files_objects(writer, task, object->files, fds_object_id,
							network_object_id,
							snapshot->effective_groups);
	return ret;
}

static int mapple_pslist_process_task(struct mapple_pslist_snapshot *snapshot,
				      struct task_struct *task)
{
	pid_t tgid;
	pid_t pid;
	int ret;

	pid = task_pid_nr_ns(task, &init_pid_ns);
	tgid = task_tgid_nr_ns(task, &init_pid_ns);
	if (!pid || !tgid)
		return -ESRCH;
	ret = mapple_pslist_write_task(&snapshot->writer, task, pid, tgid);
	if (ret)
		return ret;
	ret = mapple_pslist_process_security(snapshot, task, pid);
	if (!ret)
		ret = mapple_pslist_process_context(snapshot, task, pid);
	if (!ret)
		ret = mapple_pslist_process_resources(snapshot, task, pid);
	if (!ret)
		ret = mapple_pslist_process_mm(snapshot, task, pid);
	if (!ret)
		ret = mapple_pslist_process_files(snapshot, task, pid);
	return ret;
}

static int mapple_pslist_collect_tasks(struct mapple_pslist_snapshot *snapshot)
{
	unsigned int next_tid = 1;
	struct task_struct *task;
	int ret;

	while ((task = mapple_pslist_next_task(&next_tid))) {
		ret = mapple_pslist_process_task(snapshot, task);
		put_task_struct(task);
		if (ret == -ESRCH)
			snapshot->writer.partial = true;
		else if (ret)
			return ret;
	}
	return 0;
}

int mapple_handle_pslist(struct mapple_command_context *context)
{
	struct mapple_pslist_snapshot *snapshot;
	struct mapple_pslist_request request;
	u64 requested_groups;
	int ret;

	BUILD_BUG_ON(MAPPLE_PSLIST_COMM_SIZE != TASK_COMM_LEN);
	if (context->payload_streaming || context->remaining_payload != sizeof(request))
		return -EMSGSIZE;
	ret = mapple_command_read(context, &request, sizeof(request));
	if (ret)
		return ret;
	requested_groups = get_unaligned_be64(&request.groups);
	if (requested_groups & ~MAPPLE_PSLIST_KNOWN_GROUPS)
		return mapple_pslist_write_error(context, EINVAL);
	if (requested_groups & ~MAPPLE_PSLIST_SUPPORTED_GROUPS)
		return mapple_pslist_write_error(context, EOPNOTSUPP);

	snapshot = mapple_pslist_snapshot_create(context, requested_groups);
	if (!snapshot)
		return -ENOMEM;
	ret = mapple_pslist_write_snapshot_begin(&snapshot->writer, snapshot->requested_groups,
						 snapshot->effective_groups);
	if (!ret)
		ret = mapple_pslist_collect_tasks(snapshot);
	if (!ret && (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_ELF))
		ret = mapple_pslist_write_elf_objects(&snapshot->writer, &snapshot->elf_registry);
	if (!ret)
		ret = mapple_pslist_write_snapshot_end(&snapshot->writer);
	mapple_pslist_snapshot_release(snapshot);
	return ret;
}
