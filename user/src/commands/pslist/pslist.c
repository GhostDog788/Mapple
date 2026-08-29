#include <commands/pslist/pslist.h>

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <commands.h>
#include <commands/pslist/context.h>
#include <commands/pslist/elf.h>
#include <commands/pslist/environment.h>
#include <commands/pslist/fd.h>
#include <commands/pslist/json.h>
#include <commands/pslist/mapped_files.h>
#include <commands/pslist/memory_maps.h>
#include <commands/pslist/model.h>
#include <commands/pslist/resources.h>
#include <commands/pslist/security.h>
#include <communication/command.h>
#include <communication/stream.h>

#define MAPPLE_PSLIST_INITIAL_CAPACITY 64U
#define MAPPLE_PSLIST_RECORD_MAX_SIZE (MAPPLE_PSLIST_COMMAND_LINE_MAX_SIZE + MAPPLE_PSLIST_FDINFO_MAX_SIZE)

struct mapple_pslist_group_name {
	const char *name;
	__u64 value;
};

static const struct mapple_pslist_group_name mapple_pslist_group_names[] = {
	{"core", 0},
	{"identity", MAPPLE_PSLIST_GROUP_IDENTITY},
	{"environment", MAPPLE_PSLIST_GROUP_ENVIRONMENT},
	{"security", MAPPLE_PSLIST_GROUP_SECURITY},
	{"context", MAPPLE_PSLIST_GROUP_CONTEXT},
	{"resources", MAPPLE_PSLIST_GROUP_RESOURCES},
	{"fds", MAPPLE_PSLIST_GROUP_FDS},
	{"fds-details", MAPPLE_PSLIST_GROUP_FDS_DETAILS},
	{"network", MAPPLE_PSLIST_GROUP_NETWORK},
	{"memory-maps", MAPPLE_PSLIST_GROUP_MEMORY_MAPS},
	{"elf", MAPPLE_PSLIST_GROUP_ELF},
};

static __u16 mapple_pslist_read_u16(const void *record, size_t offset)
{
	__u16 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohs(value);
}

static __u32 mapple_pslist_read_u32(const void *record, size_t offset)
{
	__u32 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohl(value);
}

static __u64 mapple_pslist_read_u64(const void *record, size_t offset)
{
	__u64 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return be64toh(value);
}

static const char *mapple_pslist_state_name(__u8 state)
{
	switch (state) {
	case 'R':
		return "running";
	case 'S':
		return "sleeping";
	case 'D':
		return "disk sleep";
	case 'T':
		return "stopped";
	case 't':
		return "tracing stop";
	case 'X':
		return "dead";
	case 'Z':
		return "zombie";
	case 'P':
		return "parked";
	case 'I':
		return "idle";
	default:
		return NULL;
	}
}

static const char *mapple_pslist_snapshot_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_SNAPSHOT_COMPLETE:
		return "complete";
	case MAPPLE_PSLIST_SNAPSHOT_PARTIAL:
		return "partial";
	default:
		return NULL;
	}
}

static const char *mapple_pslist_object_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_OBJECT_COMPLETE:
		return "complete";
	case MAPPLE_PSLIST_OBJECT_PARTIAL:
		return "partial";
	case MAPPLE_PSLIST_OBJECT_UNAVAILABLE:
		return "unavailable";
	case MAPPLE_PSLIST_OBJECT_RACED:
		return "raced";
	case MAPPLE_PSLIST_OBJECT_TOO_LARGE:
		return "too_large";
	default:
		return NULL;
	}
}

static const char *mapple_pslist_command_line_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_COMMAND_LINE_AVAILABLE:
		return "available";
	case MAPPLE_PSLIST_COMMAND_LINE_EMPTY:
		return "empty";
	case MAPPLE_PSLIST_COMMAND_LINE_UNAVAILABLE:
		return "unavailable";
	case MAPPLE_PSLIST_COMMAND_LINE_TOO_LARGE:
		return "too_large";
	default:
		return NULL;
	}
}

static const char *mapple_pslist_executable_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_EXECUTABLE_AVAILABLE:
		return "available";
	case MAPPLE_PSLIST_EXECUTABLE_NONE:
		return "none";
	case MAPPLE_PSLIST_EXECUTABLE_UNAVAILABLE:
		return "unavailable";
	default:
		return NULL;
	}
}

int mapple_pslist_groups_from_names(const char *const *names, size_t count, __u64 *groups)
{
	size_t name_index;
	size_t group_index;
	__u64 result = 0;

	if ((!names && count) || !groups)
		return EINVAL;
	for (name_index = 0; name_index < count; ++name_index) {
		if (!names[name_index])
			return EINVAL;
		for (group_index = 0;
			 group_index < sizeof(mapple_pslist_group_names) / sizeof(mapple_pslist_group_names[0]);
			 ++group_index) {
			if (!strcmp(names[name_index], mapple_pslist_group_names[group_index].name))
				break;
		}
		if (group_index == sizeof(mapple_pslist_group_names) / sizeof(mapple_pslist_group_names[0]))
			return EINVAL;
		if (mapple_pslist_group_names[group_index].value &&
			!(mapple_pslist_group_names[group_index].value & MAPPLE_PSLIST_SUPPORTED_GROUPS))
			return EOPNOTSUPP;
		result |= mapple_pslist_group_names[group_index].value;
	}
	*groups = result;
	return 0;
}

static int mapple_pslist_grow(void **entries, size_t *capacity, size_t count, size_t entry_size)
{
	void *new_entries;
	size_t new_capacity;

	if (count < *capacity)
		return 0;
	new_capacity = *capacity ? *capacity * 2 : MAPPLE_PSLIST_INITIAL_CAPACITY;
	if (new_capacity < *capacity || new_capacity > SIZE_MAX / entry_size)
		return EOVERFLOW;
	new_entries = realloc(*entries, new_capacity * entry_size);
	if (!new_entries)
		return ENOMEM;
	*entries = new_entries;
	*capacity = new_capacity;
	return 0;
}

static struct mapple_pslist_task *mapple_pslist_find_task(struct mapple_pslist_snapshot *snapshot, __u32 pid)
{
	size_t index;

	for (index = 0; index < snapshot->task_count; ++index)
		if (snapshot->tasks[index].pid == pid)
			return &snapshot->tasks[index];
	return NULL;
}

static struct mapple_pslist_identity *mapple_pslist_find_identity(struct mapple_pslist_snapshot *snapshot,
																  __u64 object_id)
{
	size_t index;

	for (index = 0; index < snapshot->identity_count; ++index)
		if (snapshot->identities[index].object_id == object_id)
			return &snapshot->identities[index];
	return NULL;
}

static struct mapple_pslist_environment *
mapple_pslist_find_environment(struct mapple_pslist_snapshot *snapshot, __u64 object_id)
{
	size_t index;

	for (index = 0; index < snapshot->environment_count; ++index)
		if (snapshot->environments[index].object_id == object_id)
			return &snapshot->environments[index];
	return NULL;
}

static struct mapple_pslist_security *mapple_pslist_find_security(struct mapple_pslist_snapshot *snapshot,
																  __u64 object_id)
{
	size_t index;

	for (index = 0; index < snapshot->security_count; ++index)
		if (snapshot->security[index].object_id == object_id)
			return &snapshot->security[index];
	return NULL;
}

static struct mapple_pslist_context *mapple_pslist_find_context(struct mapple_pslist_snapshot *snapshot,
																__u64 object_id)
{
	size_t index;

	for (index = 0; index < snapshot->context_count; ++index)
		if (snapshot->contexts[index].object_id == object_id)
			return &snapshot->contexts[index];
	return NULL;
}

static struct mapple_pslist_resources *mapple_pslist_find_resources(struct mapple_pslist_snapshot *snapshot,
																	__u64 object_id)
{
	size_t index;

	for (index = 0; index < snapshot->resources_count; ++index)
		if (snapshot->resources[index].object_id == object_id)
			return &snapshot->resources[index];
	return NULL;
}

static struct mapple_pslist_fd_table *mapple_pslist_find_fd_table(struct mapple_pslist_snapshot *snapshot,
																  __u64 object_id)
{
	size_t index;

	for (index = 0; index < snapshot->fd_table_count; ++index)
		if (snapshot->fd_tables[index].object_id == object_id)
			return &snapshot->fd_tables[index];
	return NULL;
}

static struct mapple_pslist_network_table *
mapple_pslist_find_network_table(struct mapple_pslist_snapshot *snapshot, __u64 object_id)
{
	size_t index;

	for (index = 0; index < snapshot->network_table_count; ++index)
		if (snapshot->network_tables[index].object_id == object_id)
			return &snapshot->network_tables[index];
	return NULL;
}

static struct mapple_pslist_memory_maps *
mapple_pslist_find_memory_maps(struct mapple_pslist_snapshot *snapshot, __u64 object_id)
{
	size_t index;

	for (index = 0; index < snapshot->memory_maps_count; ++index)
		if (snapshot->memory_maps[index].object_id == object_id)
			return &snapshot->memory_maps[index];
	return NULL;
}

static struct mapple_pslist_elf *mapple_pslist_find_elf(struct mapple_pslist_snapshot *snapshot,
														__u64 object_id)
{
	size_t index;

	for (index = 0; index < snapshot->elf_count; ++index)
		if (snapshot->elfs[index].object_id == object_id)
			return &snapshot->elfs[index];
	return NULL;
}

static int mapple_pslist_add_task(struct mapple_pslist_snapshot *snapshot,
								  const struct mapple_pslist_task_record *wire)
{
	struct mapple_pslist_task *task;
	int ret;

	ret = mapple_pslist_grow((void **)&snapshot->tasks, &snapshot->task_capacity, snapshot->task_count,
							 sizeof(*snapshot->tasks));
	if (ret)
		return ret;
	task = &snapshot->tasks[snapshot->task_count];
	memset(task, 0, sizeof(*task));
	task->type = mapple_pslist_read_u16(wire, offsetof(struct mapple_pslist_task_record, task_type));
	task->pid = mapple_pslist_read_u32(&wire->task, offsetof(struct mapple_pslist_task_data, pid));
	task->tgid = mapple_pslist_read_u32(&wire->task, offsetof(struct mapple_pslist_task_data, tgid));
	task->ppid = mapple_pslist_read_u32(&wire->task, offsetof(struct mapple_pslist_task_data, ppid));
	task->start_boottime_ns =
		mapple_pslist_read_u64(&wire->task, offsetof(struct mapple_pslist_task_data, start_boottime_ns));
	task->state = wire->task.state;
	memcpy(task->comm, wire->task.comm, sizeof(task->comm));
	if (!task->pid || !task->tgid || wire->reserved || wire->task.reserved[0] || wire->task.reserved[1] ||
		wire->task.reserved[2] || task->type < MAPPLE_PSLIST_TASK_THREAD ||
		task->type > MAPPLE_PSLIST_TASK_KTHREAD || !mapple_pslist_state_name(task->state) ||
		!memchr(task->comm, '\0', sizeof(task->comm)) || mapple_pslist_find_task(snapshot, task->pid))
		return EPROTO;
	if ((task->type == MAPPLE_PSLIST_TASK_PROCESS && task->pid != task->tgid) ||
		(task->type == MAPPLE_PSLIST_TASK_THREAD && task->pid == task->tgid))
		return EPROTO;
	++snapshot->task_count;
	return 0;
}

static int mapple_pslist_add_identity(struct mapple_pslist_snapshot *snapshot, __u64 object_id)
{
	struct mapple_pslist_identity *identity;
	int ret;

	if (mapple_pslist_find_identity(snapshot, object_id))
		return EPROTO;
	ret = mapple_pslist_grow((void **)&snapshot->identities, &snapshot->identity_capacity,
							 snapshot->identity_count, sizeof(*snapshot->identities));
	if (ret)
		return ret;
	identity = &snapshot->identities[snapshot->identity_count++];
	memset(identity, 0, sizeof(*identity));
	identity->object_id = object_id;
	return 0;
}

static int mapple_pslist_add_environment(struct mapple_pslist_snapshot *snapshot, __u64 object_id)
{
	struct mapple_pslist_environment *environment;
	int ret;

	if (mapple_pslist_find_environment(snapshot, object_id))
		return EPROTO;
	ret = mapple_pslist_grow((void **)&snapshot->environments, &snapshot->environment_capacity,
							 snapshot->environment_count, sizeof(*snapshot->environments));
	if (ret)
		return ret;
	environment = &snapshot->environments[snapshot->environment_count++];
	memset(environment, 0, sizeof(*environment));
	environment->object_id = object_id;
	return 0;
}

static int mapple_pslist_add_security(struct mapple_pslist_snapshot *snapshot, __u64 object_id)
{
	struct mapple_pslist_security *security;
	int ret;

	if (mapple_pslist_find_security(snapshot, object_id))
		return EPROTO;
	ret = mapple_pslist_grow((void **)&snapshot->security, &snapshot->security_capacity,
							 snapshot->security_count, sizeof(*snapshot->security));
	if (ret)
		return ret;
	security = &snapshot->security[snapshot->security_count++];
	memset(security, 0, sizeof(*security));
	security->object_id = object_id;
	return 0;
}

static int mapple_pslist_add_context(struct mapple_pslist_snapshot *snapshot, __u64 object_id)
{
	struct mapple_pslist_context *context;
	int ret;

	if (mapple_pslist_find_context(snapshot, object_id))
		return EPROTO;
	ret = mapple_pslist_grow((void **)&snapshot->contexts, &snapshot->context_capacity,
							 snapshot->context_count, sizeof(*snapshot->contexts));
	if (ret)
		return ret;
	context = &snapshot->contexts[snapshot->context_count++];
	memset(context, 0, sizeof(*context));
	context->object_id = object_id;
	return 0;
}

static int mapple_pslist_add_resources(struct mapple_pslist_snapshot *snapshot, __u64 object_id)
{
	struct mapple_pslist_resources *resources;
	int ret;

	if (mapple_pslist_find_resources(snapshot, object_id))
		return EPROTO;
	ret = mapple_pslist_grow((void **)&snapshot->resources, &snapshot->resources_capacity,
							 snapshot->resources_count, sizeof(*snapshot->resources));
	if (ret)
		return ret;
	resources = &snapshot->resources[snapshot->resources_count++];
	memset(resources, 0, sizeof(*resources));
	resources->object_id = object_id;
	return 0;
}

static int mapple_pslist_add_fd_table(struct mapple_pslist_snapshot *snapshot, __u64 object_id)
{
	struct mapple_pslist_fd_table *table;
	int ret;

	if (mapple_pslist_find_fd_table(snapshot, object_id))
		return 0;
	ret = mapple_pslist_grow((void **)&snapshot->fd_tables, &snapshot->fd_table_capacity,
							 snapshot->fd_table_count, sizeof(*snapshot->fd_tables));
	if (ret)
		return ret;
	table = &snapshot->fd_tables[snapshot->fd_table_count++];
	memset(table, 0, sizeof(*table));
	table->object_id = object_id;
	return 0;
}

static int mapple_pslist_add_network_table(struct mapple_pslist_snapshot *snapshot, __u64 object_id)
{
	struct mapple_pslist_network_table *table;
	int ret;

	if (mapple_pslist_find_network_table(snapshot, object_id))
		return EPROTO;
	ret = mapple_pslist_grow((void **)&snapshot->network_tables, &snapshot->network_table_capacity,
							 snapshot->network_table_count, sizeof(*snapshot->network_tables));
	if (ret)
		return ret;
	table = &snapshot->network_tables[snapshot->network_table_count++];
	memset(table, 0, sizeof(*table));
	table->object_id = object_id;
	return 0;
}

static int mapple_pslist_add_memory_maps(struct mapple_pslist_snapshot *snapshot, __u64 object_id)
{
	struct mapple_pslist_memory_maps *memory_maps;
	int ret;

	if (mapple_pslist_find_memory_maps(snapshot, object_id))
		return EPROTO;
	ret = mapple_pslist_grow((void **)&snapshot->memory_maps, &snapshot->memory_maps_capacity,
							 snapshot->memory_maps_count, sizeof(*snapshot->memory_maps));
	if (ret)
		return ret;
	memory_maps = &snapshot->memory_maps[snapshot->memory_maps_count++];
	memset(memory_maps, 0, sizeof(*memory_maps));
	memory_maps->object_id = object_id;
	return 0;
}

static int mapple_pslist_add_elf(struct mapple_pslist_snapshot *snapshot, __u64 object_id)
{
	struct mapple_pslist_elf *elf;
	int ret;

	if (object_id != snapshot->elf_count + 1 || mapple_pslist_find_elf(snapshot, object_id))
		return EPROTO;
	ret = mapple_pslist_grow((void **)&snapshot->elfs, &snapshot->elf_capacity, snapshot->elf_count,
							 sizeof(*snapshot->elfs));
	if (ret)
		return ret;
	elf = &snapshot->elfs[snapshot->elf_count++];
	memset(elf, 0, sizeof(*elf));
	elf->object_id = object_id;
	return 0;
}

static int mapple_pslist_decode_snapshot_begin(struct mapple_pslist_snapshot *snapshot,
											   const struct mapple_pslist_snapshot_begin *wire)
{
	snapshot->requested_groups =
		mapple_pslist_read_u64(wire, offsetof(struct mapple_pslist_snapshot_begin, requested_groups));
	snapshot->effective_groups =
		mapple_pslist_read_u64(wire, offsetof(struct mapple_pslist_snapshot_begin, effective_groups));
	snapshot->supported_groups =
		mapple_pslist_read_u64(wire, offsetof(struct mapple_pslist_snapshot_begin, supported_groups));
	snapshot->start_boottime_ns =
		mapple_pslist_read_u64(wire, offsetof(struct mapple_pslist_snapshot_begin, start_boottime_ns));
	if ((snapshot->requested_groups & ~snapshot->effective_groups) ||
		(snapshot->effective_groups & ~snapshot->supported_groups) ||
		snapshot->supported_groups != MAPPLE_PSLIST_SUPPORTED_GROUPS)
		return EPROTO;
	return 0;
}

static int mapple_pslist_decode_link(struct mapple_pslist_snapshot *snapshot,
									 const struct mapple_pslist_task_object_link *wire, __u64 group)
{
	struct mapple_pslist_task *task;
	__u64 object_id;
	__u32 pid;
	__u16 object_type;
	__u64 *slot;

	pid = mapple_pslist_read_u32(wire, offsetof(struct mapple_pslist_task_object_link, pid));
	object_id = mapple_pslist_read_u64(wire, offsetof(struct mapple_pslist_task_object_link, object_id));
	object_type = mapple_pslist_read_u16(wire, offsetof(struct mapple_pslist_task_object_link, object_type));
	task = mapple_pslist_find_task(snapshot, pid);
	if (!task || !object_id || wire->reserved)
		return EPROTO;
	switch (group) {
	case MAPPLE_PSLIST_GROUP_IDENTITY:
		if (object_type != MAPPLE_PSLIST_OBJECT_MM)
			return EPROTO;
		slot = &task->group_object_ids[MAPPLE_PSLIST_OUTPUT_IDENTITY];
		break;
	case MAPPLE_PSLIST_GROUP_ENVIRONMENT:
		if (object_type != MAPPLE_PSLIST_OBJECT_MM)
			return EPROTO;
		slot = &task->group_object_ids[MAPPLE_PSLIST_OUTPUT_ENVIRONMENT];
		break;
	case MAPPLE_PSLIST_GROUP_SECURITY:
		if (object_type != MAPPLE_PSLIST_OBJECT_TASK)
			return EPROTO;
		slot = &task->group_object_ids[MAPPLE_PSLIST_OUTPUT_SECURITY];
		break;
	case MAPPLE_PSLIST_GROUP_CONTEXT:
		if (object_type != MAPPLE_PSLIST_OBJECT_TASK)
			return EPROTO;
		slot = &task->group_object_ids[MAPPLE_PSLIST_OUTPUT_CONTEXT];
		break;
	case MAPPLE_PSLIST_GROUP_RESOURCES:
		if (object_type != MAPPLE_PSLIST_OBJECT_TASK)
			return EPROTO;
		slot = &task->group_object_ids[MAPPLE_PSLIST_OUTPUT_RESOURCES];
		break;
	case MAPPLE_PSLIST_GROUP_FDS:
		if (object_type != MAPPLE_PSLIST_OBJECT_FILES)
			return EPROTO;
		slot = &task->group_object_ids[MAPPLE_PSLIST_OUTPUT_FDS];
		break;
	case MAPPLE_PSLIST_GROUP_NETWORK:
		if (object_type != MAPPLE_PSLIST_OBJECT_FILES)
			return EPROTO;
		slot = &task->group_object_ids[MAPPLE_PSLIST_OUTPUT_NETWORK];
		break;
	case MAPPLE_PSLIST_GROUP_MEMORY_MAPS:
		if (object_type != MAPPLE_PSLIST_OBJECT_MM)
			return EPROTO;
		slot = &task->group_object_ids[MAPPLE_PSLIST_OUTPUT_MEMORY_MAPS];
		break;
	default:
		return EPROTO;
	}
	if (*slot)
		return EPROTO;
	*slot = object_id;
	return 0;
}

static int mapple_pslist_decode_object_begin(struct mapple_pslist_snapshot *snapshot,
											 const struct mapple_pslist_object_begin *wire, __u64 group)
{
	__u64 object_id = mapple_pslist_read_u64(wire, offsetof(struct mapple_pslist_object_begin, object_id));
	__u16 object_type =
		mapple_pslist_read_u16(wire, offsetof(struct mapple_pslist_object_begin, object_type));

	if (!object_id || wire->reserved || wire->reserved2)
		return EPROTO;
	if (group == MAPPLE_PSLIST_GROUP_IDENTITY && object_type == MAPPLE_PSLIST_OBJECT_MM)
		return mapple_pslist_add_identity(snapshot, object_id);
	if (group == MAPPLE_PSLIST_GROUP_ENVIRONMENT && object_type == MAPPLE_PSLIST_OBJECT_MM)
		return mapple_pslist_add_environment(snapshot, object_id);
	if (group == MAPPLE_PSLIST_GROUP_SECURITY && object_type == MAPPLE_PSLIST_OBJECT_TASK)
		return mapple_pslist_add_security(snapshot, object_id);
	if (group == MAPPLE_PSLIST_GROUP_CONTEXT && object_type == MAPPLE_PSLIST_OBJECT_TASK)
		return mapple_pslist_add_context(snapshot, object_id);
	if (group == MAPPLE_PSLIST_GROUP_RESOURCES && object_type == MAPPLE_PSLIST_OBJECT_TASK)
		return mapple_pslist_add_resources(snapshot, object_id);
	if (group == MAPPLE_PSLIST_GROUP_FDS && object_type == MAPPLE_PSLIST_OBJECT_FILES)
		return mapple_pslist_add_fd_table(snapshot, object_id);
	if (group == MAPPLE_PSLIST_GROUP_NETWORK && object_type == MAPPLE_PSLIST_OBJECT_FILES)
		return mapple_pslist_add_network_table(snapshot, object_id);
	if (group == MAPPLE_PSLIST_GROUP_MEMORY_MAPS && object_type == MAPPLE_PSLIST_OBJECT_MM)
		return mapple_pslist_add_memory_maps(snapshot, object_id);
	if (group == MAPPLE_PSLIST_GROUP_ELF && object_type == MAPPLE_PSLIST_OBJECT_ELF_FILE)
		return mapple_pslist_add_elf(snapshot, object_id);
	return EPROTO;
}

static int mapple_pslist_decode_identity(struct mapple_pslist_snapshot *snapshot,
										 const struct mapple_pslist_identity_record *wire, size_t size)
{
	struct mapple_pslist_identity *identity;
	const struct mapple_pslist_process_data *data = &wire->identity;
	const unsigned char *bytes = (const unsigned char *)wire + sizeof(*wire);
	__u64 object_id;
	__u64 variable_size;

	object_id = mapple_pslist_read_u64(wire, offsetof(struct mapple_pslist_identity_record, object_id));
	identity = mapple_pslist_find_identity(snapshot, object_id);
	if (!identity || identity->has_record)
		return EPROTO;
	identity->command_line_status =
		mapple_pslist_read_u16(data, offsetof(struct mapple_pslist_process_data, command_line_status));
	identity->executable_status =
		mapple_pslist_read_u16(data, offsetof(struct mapple_pslist_process_data, executable_status));
	identity->executable_flags =
		mapple_pslist_read_u32(data, offsetof(struct mapple_pslist_process_data, executable_flags));
	identity->command_line_length =
		mapple_pslist_read_u32(data, offsetof(struct mapple_pslist_process_data, command_line_length));
	identity->executable_path_length =
		mapple_pslist_read_u32(data, offsetof(struct mapple_pslist_process_data, executable_path_length));
	identity->executable_device_major =
		mapple_pslist_read_u32(data, offsetof(struct mapple_pslist_process_data, executable_device_major));
	identity->executable_device_minor =
		mapple_pslist_read_u32(data, offsetof(struct mapple_pslist_process_data, executable_device_minor));
	identity->executable_mode =
		mapple_pslist_read_u32(data, offsetof(struct mapple_pslist_process_data, executable_mode));
	identity->executable_inode =
		mapple_pslist_read_u64(data, offsetof(struct mapple_pslist_process_data, executable_inode));
	identity->executable_size =
		mapple_pslist_read_u64(data, offsetof(struct mapple_pslist_process_data, executable_size));
	identity->executable_mtime_ns =
		(__s64)mapple_pslist_read_u64(data, offsetof(struct mapple_pslist_process_data, executable_mtime_ns));
	identity->executable_ctime_ns =
		(__s64)mapple_pslist_read_u64(data, offsetof(struct mapple_pslist_process_data, executable_ctime_ns));
	identity->executable_filesystem_magic = mapple_pslist_read_u64(
		data, offsetof(struct mapple_pslist_process_data, executable_filesystem_magic));
	variable_size = (__u64)identity->command_line_length + identity->executable_path_length;
	if (!mapple_pslist_command_line_status_name(identity->command_line_status) ||
		!mapple_pslist_executable_status_name(identity->executable_status) ||
		(identity->executable_flags & ~MAPPLE_PSLIST_EXECUTABLE_KNOWN_FLAGS) ||
		identity->command_line_length > MAPPLE_PSLIST_COMMAND_LINE_MAX_SIZE ||
		sizeof(*wire) + variable_size != size)
		return EPROTO;
	if ((identity->command_line_status == MAPPLE_PSLIST_COMMAND_LINE_AVAILABLE) !=
		!!identity->command_line_length)
		return EPROTO;
	if (identity->executable_status == MAPPLE_PSLIST_EXECUTABLE_AVAILABLE) {
		if (!!(identity->executable_flags & MAPPLE_PSLIST_EXECUTABLE_FLAG_PATH_UNAVAILABLE) ==
			!!identity->executable_path_length)
			return EPROTO;
	} else if (identity->executable_flags || identity->executable_path_length ||
			   identity->executable_device_major || identity->executable_device_minor ||
			   identity->executable_mode || identity->executable_inode || identity->executable_size ||
			   identity->executable_mtime_ns || identity->executable_ctime_ns ||
			   identity->executable_filesystem_magic) {
		return EPROTO;
	}
	if ((identity->executable_flags & MAPPLE_PSLIST_EXECUTABLE_FLAG_METADATA_UNAVAILABLE) &&
		(identity->executable_device_major || identity->executable_device_minor ||
		 identity->executable_mode || identity->executable_inode || identity->executable_size ||
		 identity->executable_mtime_ns || identity->executable_ctime_ns ||
		 identity->executable_filesystem_magic))
		return EPROTO;
	if (identity->command_line_length) {
		identity->command_line = malloc(identity->command_line_length);
		if (!identity->command_line)
			return ENOMEM;
		memcpy(identity->command_line, bytes, identity->command_line_length);
		bytes += identity->command_line_length;
	}
	if (identity->executable_path_length) {
		identity->executable_path = malloc(identity->executable_path_length);
		if (!identity->executable_path)
			return ENOMEM;
		memcpy(identity->executable_path, bytes, identity->executable_path_length);
	}
	identity->has_record = 1;
	return 0;
}

static int mapple_pslist_decode_object_end(struct mapple_pslist_snapshot *snapshot,
										   const struct mapple_pslist_object_end *wire, __u64 group)
{
	struct mapple_pslist_identity *identity;
	struct mapple_pslist_environment *environment;
	struct mapple_pslist_security *security;
	struct mapple_pslist_context *context;
	struct mapple_pslist_resources *resources;
	struct mapple_pslist_fd_table *table;
	struct mapple_pslist_network_table *network_table;
	struct mapple_pslist_memory_maps *memory_maps;
	struct mapple_pslist_elf *elf;
	__u64 object_id = mapple_pslist_read_u64(wire, offsetof(struct mapple_pslist_object_end, object_id));
	__u16 status = mapple_pslist_read_u16(wire, offsetof(struct mapple_pslist_object_end, status));
	__u32 count = mapple_pslist_read_u32(wire, offsetof(struct mapple_pslist_object_end, record_count));

	if (!mapple_pslist_object_status_name(status) || wire->reserved)
		return EPROTO;
	if (group == MAPPLE_PSLIST_GROUP_IDENTITY) {
		identity = mapple_pslist_find_identity(snapshot, object_id);
		if (!identity || !identity->has_record || identity->status || count != 1)
			return EPROTO;
		identity->status = status;
		return 0;
	}
	if (group == MAPPLE_PSLIST_GROUP_ENVIRONMENT) {
		environment = mapple_pslist_find_environment(snapshot, object_id);
		if (!environment || !environment->has_record || environment->status || count != 1)
			return EPROTO;
		environment->status = status;
		return 0;
	}
	if (group == MAPPLE_PSLIST_GROUP_SECURITY) {
		security = mapple_pslist_find_security(snapshot, object_id);
		if (!security || !security->has_record || security->status || count != 1)
			return EPROTO;
		security->status = status;
		return 0;
	}
	if (group == MAPPLE_PSLIST_GROUP_CONTEXT) {
		context = mapple_pslist_find_context(snapshot, object_id);
		if (!context || !context->has_record || context->status || count != 1)
			return EPROTO;
		context->status = status;
		return 0;
	}
	if (group == MAPPLE_PSLIST_GROUP_RESOURCES) {
		resources = mapple_pslist_find_resources(snapshot, object_id);
		if (!resources || !resources->has_record || resources->status || count != 1)
			return EPROTO;
		resources->status = status;
		return 0;
	}
	if (group == MAPPLE_PSLIST_GROUP_FDS) {
		size_t index;
		size_t details_count = 0;

		table = mapple_pslist_find_fd_table(snapshot, object_id);
		if (!table)
			return EPROTO;
		if (table->has_fds || count != table->count)
			return EPROTO;
		table->has_fds = 1;
		table->fds_status = status;
		if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_FDS_DETAILS) {
			for (index = 0; index < table->count; ++index)
				details_count += table->entries[index].details_available;
			if (details_count != table->count)
				return EPROTO;
			table->has_details = 1;
		}
	} else if (group == MAPPLE_PSLIST_GROUP_NETWORK) {
		network_table = mapple_pslist_find_network_table(snapshot, object_id);
		if (!network_table || network_table->has_records || count != network_table->count)
			return EPROTO;
		network_table->has_records = 1;
		network_table->status = status;
	} else if (group == MAPPLE_PSLIST_GROUP_MEMORY_MAPS) {
		memory_maps = mapple_pslist_find_memory_maps(snapshot, object_id);
		if (!memory_maps || !memory_maps->has_metadata || memory_maps->status)
			return EPROTO;
		if (status == MAPPLE_PSLIST_OBJECT_COMPLETE) {
			if (memory_maps->mapping_count > memory_maps->mapping_limit ||
				memory_maps->count != memory_maps->mapping_count || count != memory_maps->count + 1)
				return EPROTO;
		} else {
			if (memory_maps->count || count != 1)
				return EPROTO;
			if (status == MAPPLE_PSLIST_OBJECT_TOO_LARGE &&
				memory_maps->mapping_count <= memory_maps->mapping_limit)
				return EPROTO;
		}
		memory_maps->status = status;
	} else if (group == MAPPLE_PSLIST_GROUP_ELF) {
		elf = mapple_pslist_find_elf(snapshot, object_id);
		if (!elf || mapple_pslist_read_u16(wire, offsetof(struct mapple_pslist_object_end, object_type)) !=
						MAPPLE_PSLIST_OBJECT_ELF_FILE)
			return EPROTO;
		return mapple_pslist_finish_elf(
			elf, status, mapple_pslist_read_u32(wire, offsetof(struct mapple_pslist_object_end, error_code)),
			count);
	} else
		return EPROTO;
	return 0;
}

static int mapple_pslist_decode_snapshot_end(struct mapple_pslist_snapshot *snapshot,
											 const struct mapple_pslist_snapshot_end *wire,
											 size_t object_count)
{
	__u32 task_count;
	__u32 wire_object_count;

	snapshot->status = mapple_pslist_read_u16(wire, offsetof(struct mapple_pslist_snapshot_end, status));
	task_count = mapple_pslist_read_u32(wire, offsetof(struct mapple_pslist_snapshot_end, task_count));
	wire_object_count =
		mapple_pslist_read_u32(wire, offsetof(struct mapple_pslist_snapshot_end, object_count));
	snapshot->end_boottime_ns =
		mapple_pslist_read_u64(wire, offsetof(struct mapple_pslist_snapshot_end, end_boottime_ns));
	if (!mapple_pslist_snapshot_status_name(snapshot->status) || wire->reserved || wire->reserved2 ||
		task_count != snapshot->task_count || wire_object_count != object_count ||
		snapshot->end_boottime_ns < snapshot->start_boottime_ns)
		return EPROTO;
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_ELF) {
		size_t map_index;
		size_t object_index;
		unsigned char *referenced;

		referenced = calloc(snapshot->elf_count, 1);
		if (snapshot->elf_count && !referenced)
			return ENOMEM;
		for (object_index = 0; object_index < snapshot->memory_maps_count; ++object_index) {
			const struct mapple_pslist_memory_maps *maps = &snapshot->memory_maps[object_index];

			for (map_index = 0; map_index < maps->count; ++map_index) {
				__u64 id = maps->mappings[map_index].elf_object_id;

				if (!id)
					continue;
				if (id > snapshot->elf_count || !snapshot->elfs[id - 1].status) {
					free(referenced);
					return EPROTO;
				}
				referenced[id - 1] = 1;
			}
		}
		for (object_index = 0; object_index < snapshot->elf_count; ++object_index)
			if (!referenced[object_index]) {
				free(referenced);
				return EPROTO;
			}
		free(referenced);
	} else {
		size_t map_index;
		size_t object_index;

		if (snapshot->elf_count)
			return EPROTO;
		for (object_index = 0; object_index < snapshot->memory_maps_count; ++object_index)
			for (map_index = 0; map_index < snapshot->memory_maps[object_index].count; ++map_index)
				if (snapshot->memory_maps[object_index].mappings[map_index].elf_object_id)
					return EPROTO;
	}
	return 0;
}

static int mapple_pslist_read_snapshot(int session_fd, struct mapple_pslist_snapshot *snapshot)
{
	struct mapple_pslist_stream_record header;
	unsigned char *record = NULL;
	size_t object_count = 0;
	int began = 0;
	int ended = 0;
	int ret;

	while (!ended) {
		__u32 record_size;
		__u16 record_type;
		__u64 group;

		ret = read_exact(session_fd, &header, sizeof(header));
		if (ret)
			return ret;
		record_size =
			mapple_pslist_read_u32(&header, offsetof(struct mapple_pslist_stream_record, record_size));
		record_type =
			mapple_pslist_read_u16(&header, offsetof(struct mapple_pslist_stream_record, record_type));
		group = mapple_pslist_read_u64(&header, offsetof(struct mapple_pslist_stream_record, group));
		if (header.reserved || record_size < sizeof(header) || record_size > MAPPLE_PSLIST_RECORD_MAX_SIZE ||
			(group && (group & (group - 1))) || (group & ~MAPPLE_PSLIST_KNOWN_GROUPS))
			return EPROTO;
		record = malloc(record_size);
		if (!record)
			return ENOMEM;
		memcpy(record, &header, sizeof(header));
		ret = read_exact(session_fd, record + sizeof(header), record_size - sizeof(header));
		if (ret)
			goto free_record;
		switch (record_type) {
		case MAPPLE_PSLIST_STREAM_SNAPSHOT_BEGIN:
			if (began || group || record_size != sizeof(struct mapple_pslist_snapshot_begin)) {
				ret = EPROTO;
				break;
			}
			ret = mapple_pslist_decode_snapshot_begin(snapshot, (const void *)record);
			began = !ret;
			break;
		case MAPPLE_PSLIST_STREAM_TASK:
			if (!began || group || record_size != sizeof(struct mapple_pslist_task_record)) {
				ret = EPROTO;
				break;
			}
			ret = mapple_pslist_add_task(snapshot, (const void *)record);
			break;
		case MAPPLE_PSLIST_STREAM_TASK_OBJECT_LINK:
			if (!began || !group || record_size != sizeof(struct mapple_pslist_task_object_link)) {
				ret = EPROTO;
				break;
			}
			ret = mapple_pslist_decode_link(snapshot, (const void *)record, group);
			break;
		case MAPPLE_PSLIST_STREAM_OBJECT_BEGIN:
			if (!began || !group || record_size != sizeof(struct mapple_pslist_object_begin)) {
				ret = EPROTO;
				break;
			}
			ret = mapple_pslist_decode_object_begin(snapshot, (const void *)record, group);
			if (!ret)
				++object_count;
			break;
		case MAPPLE_PSLIST_STREAM_IDENTITY:
			if (!began || group != MAPPLE_PSLIST_GROUP_IDENTITY) {
				ret = EPROTO;
				break;
			}
			ret = mapple_pslist_decode_identity(snapshot, (const void *)record, record_size);
			break;
		case MAPPLE_PSLIST_STREAM_ENVIRONMENT: {
			__u64 id =
				mapple_pslist_read_u64(record, offsetof(struct mapple_pslist_environment_record, object_id));
			struct mapple_pslist_environment *environment = mapple_pslist_find_environment(snapshot, id);

			ret = !environment || group != MAPPLE_PSLIST_GROUP_ENVIRONMENT
					  ? EPROTO
					  : mapple_pslist_decode_environment_record(record, record_size, environment);
			break;
		}
		case MAPPLE_PSLIST_STREAM_SECURITY: {
			__u64 id =
				mapple_pslist_read_u64(record, offsetof(struct mapple_pslist_security_record, object_id));
			struct mapple_pslist_security *security = mapple_pslist_find_security(snapshot, id);

			ret = !security || group != MAPPLE_PSLIST_GROUP_SECURITY
					  ? EPROTO
					  : mapple_pslist_decode_security_record(record, record_size, security);
			break;
		}
		case MAPPLE_PSLIST_STREAM_CONTEXT: {
			__u64 id =
				mapple_pslist_read_u64(record, offsetof(struct mapple_pslist_context_record, object_id));
			struct mapple_pslist_context *context = mapple_pslist_find_context(snapshot, id);

			ret = !context || group != MAPPLE_PSLIST_GROUP_CONTEXT
					  ? EPROTO
					  : mapple_pslist_decode_context_record(record, record_size, context);
			break;
		}
		case MAPPLE_PSLIST_STREAM_RESOURCES: {
			__u64 id =
				mapple_pslist_read_u64(record, offsetof(struct mapple_pslist_resources_record, object_id));
			struct mapple_pslist_resources *resources = mapple_pslist_find_resources(snapshot, id);

			ret = !resources || group != MAPPLE_PSLIST_GROUP_RESOURCES
					  ? EPROTO
					  : mapple_pslist_decode_resources_record(record, record_size, resources);
			break;
		}
		case MAPPLE_PSLIST_STREAM_FD: {
			__u64 id = mapple_pslist_read_u64(record, offsetof(struct mapple_pslist_fd_record, object_id));
			struct mapple_pslist_fd_table *table = mapple_pslist_find_fd_table(snapshot, id);
			ret = !table || group != MAPPLE_PSLIST_GROUP_FDS
					  ? EPROTO
					  : mapple_pslist_decode_fd_record(record, record_size, table);
			break;
		}
		case MAPPLE_PSLIST_STREAM_FD_DETAILS: {
			__u64 id =
				mapple_pslist_read_u64(record, offsetof(struct mapple_pslist_fd_details_record, object_id));
			struct mapple_pslist_fd_table *table = mapple_pslist_find_fd_table(snapshot, id);
			ret = !table || group != MAPPLE_PSLIST_GROUP_FDS_DETAILS
					  ? EPROTO
					  : mapple_pslist_decode_fd_details_record(record, record_size, table);
			break;
		}
		case MAPPLE_PSLIST_STREAM_NETWORK: {
			__u64 id =
				mapple_pslist_read_u64(record, offsetof(struct mapple_pslist_network_record, object_id));
			struct mapple_pslist_network_table *table = mapple_pslist_find_network_table(snapshot, id);
			ret = !table || group != MAPPLE_PSLIST_GROUP_NETWORK
					  ? EPROTO
					  : mapple_pslist_decode_network_record(record, record_size, table);
			break;
		}
		case MAPPLE_PSLIST_STREAM_MEMORY_MAPS_METADATA: {
			__u64 id = mapple_pslist_read_u64(
				record, offsetof(struct mapple_pslist_memory_maps_metadata_record, object_id));
			struct mapple_pslist_memory_maps *memory_maps = mapple_pslist_find_memory_maps(snapshot, id);

			ret = !memory_maps || group != MAPPLE_PSLIST_GROUP_MEMORY_MAPS
					  ? EPROTO
					  : mapple_pslist_decode_memory_maps_metadata(record, record_size, memory_maps);
			break;
		}
		case MAPPLE_PSLIST_STREAM_MEMORY_MAP: {
			__u64 id =
				mapple_pslist_read_u64(record, offsetof(struct mapple_pslist_memory_map_record, object_id));
			struct mapple_pslist_memory_maps *memory_maps = mapple_pslist_find_memory_maps(snapshot, id);

			ret = !memory_maps || group != MAPPLE_PSLIST_GROUP_MEMORY_MAPS
					  ? EPROTO
					  : mapple_pslist_decode_memory_map(record, record_size, memory_maps);
			break;
		}
		case MAPPLE_PSLIST_STREAM_ELF_METADATA: {
			__u64 id =
				mapple_pslist_read_u64(record, offsetof(struct mapple_pslist_elf_metadata_record, object_id));
			struct mapple_pslist_elf *elf = mapple_pslist_find_elf(snapshot, id);

			ret = !elf || group != MAPPLE_PSLIST_GROUP_ELF
					  ? EPROTO
					  : mapple_pslist_decode_elf_metadata(record, record_size, elf);
			break;
		}
		case MAPPLE_PSLIST_STREAM_ELF_STRING: {
			__u64 id =
				mapple_pslist_read_u64(record, offsetof(struct mapple_pslist_elf_string_record, object_id));
			struct mapple_pslist_elf *elf = mapple_pslist_find_elf(snapshot, id);

			ret = !elf || group != MAPPLE_PSLIST_GROUP_ELF
					  ? EPROTO
					  : mapple_pslist_decode_elf_string(record, record_size, elf);
			break;
		}
		case MAPPLE_PSLIST_STREAM_ELF_SYMBOL: {
			__u64 id =
				mapple_pslist_read_u64(record, offsetof(struct mapple_pslist_elf_symbol_record, object_id));
			struct mapple_pslist_elf *elf = mapple_pslist_find_elf(snapshot, id);

			ret = !elf || group != MAPPLE_PSLIST_GROUP_ELF
					  ? EPROTO
					  : mapple_pslist_decode_elf_symbol(record, record_size, elf);
			break;
		}
		case MAPPLE_PSLIST_STREAM_OBJECT_END:
			if (!began || !group || record_size != sizeof(struct mapple_pslist_object_end)) {
				ret = EPROTO;
				break;
			}
			ret = mapple_pslist_decode_object_end(snapshot, (const void *)record, group);
			break;
		case MAPPLE_PSLIST_STREAM_SNAPSHOT_END:
			if (!began || group || record_size != sizeof(struct mapple_pslist_snapshot_end)) {
				ret = EPROTO;
				break;
			}
			ret = mapple_pslist_decode_snapshot_end(snapshot, (const void *)record, object_count);
			ended = !ret;
			break;
		default:
			ret = began ? 0 : EPROTO;
			break;
		}
	free_record:
		free(record);
		record = NULL;
		if (ret)
			return ret;
	}
	return 0;
}

static void mapple_pslist_free_snapshot(struct mapple_pslist_snapshot *snapshot)
{
	size_t index;

	for (index = 0; index < snapshot->identity_count; ++index) {
		free(snapshot->identities[index].command_line);
		free(snapshot->identities[index].executable_path);
	}
	for (index = 0; index < snapshot->environment_count; ++index)
		mapple_pslist_free_environment(&snapshot->environments[index]);
	for (index = 0; index < snapshot->security_count; ++index)
		mapple_pslist_free_security(&snapshot->security[index]);
	for (index = 0; index < snapshot->context_count; ++index)
		mapple_pslist_free_context(&snapshot->contexts[index]);
	for (index = 0; index < snapshot->fd_table_count; ++index)
		mapple_pslist_free_fd_table(&snapshot->fd_tables[index]);
	for (index = 0; index < snapshot->network_table_count; ++index)
		mapple_pslist_free_network_table(&snapshot->network_tables[index]);
	for (index = 0; index < snapshot->memory_maps_count; ++index)
		mapple_pslist_free_memory_maps(&snapshot->memory_maps[index]);
	for (index = 0; index < snapshot->elf_count; ++index)
		mapple_pslist_free_elf(&snapshot->elfs[index]);
	free(snapshot->mapped_files);
	free(snapshot->tasks);
	free(snapshot->identities);
	free(snapshot->environments);
	free(snapshot->security);
	free(snapshot->contexts);
	free(snapshot->resources);
	free(snapshot->fd_tables);
	free(snapshot->network_tables);
	free(snapshot->memory_maps);
	free(snapshot->elfs);
	memset(snapshot, 0, sizeof(*snapshot));
}

static int mapple_pslist_compare_tasks(const void *left, const void *right)
{
	const struct mapple_pslist_task *first = left;
	const struct mapple_pslist_task *second = right;
	int first_kernel = first->type == MAPPLE_PSLIST_TASK_KTHREAD;
	int second_kernel = second->type == MAPPLE_PSLIST_TASK_KTHREAD;

	if (first_kernel != second_kernel)
		return first_kernel ? 1 : -1;
	if (!first_kernel && first->tgid != second->tgid)
		return first->tgid < second->tgid ? -1 : 1;
	if (first->type != second->type) {
		if (first->type == MAPPLE_PSLIST_TASK_PROCESS)
			return -1;
		if (second->type == MAPPLE_PSLIST_TASK_PROCESS)
			return 1;
	}
	return first->pid < second->pid ? -1 : first->pid > second->pid;
}

static int mapple_pslist_print_group_names(__u64 groups)
{
	size_t index;
	int first = 1;

	if (putchar('[') == EOF)
		return EIO;
	for (index = 0; index < sizeof(mapple_pslist_group_names) / sizeof(mapple_pslist_group_names[0]);
		 ++index) {
		if (!(groups & mapple_pslist_group_names[index].value))
			continue;
		if (printf("%s\"%s\"", first ? "" : ", ", mapple_pslist_group_names[index].name) < 0)
			return EIO;
		first = 0;
	}
	return putchar(']') == EOF ? EIO : 0;
}

static int mapple_pslist_print_task(const struct mapple_pslist_task *task, __u64 groups, int indent)
{
	const char *state = mapple_pslist_state_name(task->state);
	int first_group = 1;
	int ret;

	if (!state)
		return EPROTO;
	ret = mapple_pslist_print_json_indent(indent);
	if (ret)
		return ret;
	if (printf("{\"pid\": %u, \"tgid\": %u, \"ppid\": %u, "
			   "\"start_boottime_ns\": %" PRIu64 ", \"state\": \"%s\", \"comm\": ",
			   task->pid, task->tgid, task->ppid, (uint64_t)task->start_boottime_ns, state) < 0)
		return EIO;
	ret = mapple_pslist_print_json_bytes((const unsigned char *)task->comm,
										 strnlen(task->comm, sizeof(task->comm)));
	if (ret)
		return ret;
	if (fputs(", \"groups\": {", stdout) == EOF)
		return EIO;
#define PRINT_TASK_GROUP(mask, name, index)                                                                  \
	do {                                                                                                     \
		if ((groups) & (mask)) {                                                                             \
			if (printf("%s\"%s\": %" PRIu64, first_group ? "" : ", ", (name),                                \
					   (uint64_t)task->group_object_ids[(index)]) < 0)                                       \
				return EIO;                                                                                  \
			first_group = 0;                                                                                 \
		}                                                                                                    \
	} while (0)
	PRINT_TASK_GROUP(MAPPLE_PSLIST_GROUP_IDENTITY, "identity", MAPPLE_PSLIST_OUTPUT_IDENTITY);
	PRINT_TASK_GROUP(MAPPLE_PSLIST_GROUP_ENVIRONMENT, "environment", MAPPLE_PSLIST_OUTPUT_ENVIRONMENT);
	PRINT_TASK_GROUP(MAPPLE_PSLIST_GROUP_SECURITY, "security", MAPPLE_PSLIST_OUTPUT_SECURITY);
	PRINT_TASK_GROUP(MAPPLE_PSLIST_GROUP_CONTEXT, "context", MAPPLE_PSLIST_OUTPUT_CONTEXT);
	PRINT_TASK_GROUP(MAPPLE_PSLIST_GROUP_RESOURCES, "resources", MAPPLE_PSLIST_OUTPUT_RESOURCES);
	PRINT_TASK_GROUP(MAPPLE_PSLIST_GROUP_FDS, "fds", MAPPLE_PSLIST_OUTPUT_FDS);
	PRINT_TASK_GROUP(MAPPLE_PSLIST_GROUP_NETWORK, "network", MAPPLE_PSLIST_OUTPUT_NETWORK);
	PRINT_TASK_GROUP(MAPPLE_PSLIST_GROUP_MEMORY_MAPS, "memory_maps", MAPPLE_PSLIST_OUTPUT_MEMORY_MAPS);
#undef PRINT_TASK_GROUP
	return fputs("}}", stdout) == EOF ? EIO : 0;
}

static int mapple_pslist_print_argv(const struct mapple_pslist_identity *identity)
{
	size_t offset = 0;
	int first = 1;
	int ret;

	if (putchar('[') == EOF)
		return EIO;
	while (offset < identity->command_line_length) {
		size_t end = offset;
		while (end < identity->command_line_length && identity->command_line[end])
			++end;
		if (!first && fputs(", ", stdout) == EOF)
			return EIO;
		ret = mapple_pslist_print_json_bytes(identity->command_line + offset, end - offset);
		if (ret)
			return ret;
		first = 0;
		offset = end < identity->command_line_length ? end + 1 : end;
	}
	return putchar(']') == EOF ? EIO : 0;
}

static int mapple_pslist_print_identity(const struct mapple_pslist_identity *identity, int indent)
{
	const char *argv_status = mapple_pslist_command_line_status_name(identity->command_line_status);
	const char *executable_status = mapple_pslist_executable_status_name(identity->executable_status);
	const char *metadata_status;
	const char *path_status;
	const char *status = mapple_pslist_object_status_name(identity->status);
	int ret;

	if (!status || !argv_status || !executable_status)
		return EPROTO;
	if (identity->executable_status == MAPPLE_PSLIST_EXECUTABLE_AVAILABLE) {
		path_status = identity->executable_flags & MAPPLE_PSLIST_EXECUTABLE_FLAG_PATH_UNAVAILABLE
						  ? "unavailable"
						  : "available";
		metadata_status = identity->executable_flags & MAPPLE_PSLIST_EXECUTABLE_FLAG_METADATA_UNAVAILABLE
							  ? "unavailable"
							  : "available";
	} else {
		path_status = "not_applicable";
		metadata_status = "not_applicable";
	}
	if (printf("{\n%*s\"status\": \"%s\",\n%*s\"argv_status\": \"%s\","
			   "\n%*s\"argv\": ",
			   indent + 2, "", status, indent + 2, "", argv_status, indent + 2, "") < 0)
		return EIO;
	if (identity->command_line_status == MAPPLE_PSLIST_COMMAND_LINE_AVAILABLE)
		ret = mapple_pslist_print_argv(identity);
	else if (identity->command_line_status == MAPPLE_PSLIST_COMMAND_LINE_EMPTY)
		ret = fputs("[]", stdout) == EOF ? EIO : 0;
	else
		ret = fputs("null", stdout) == EOF ? EIO : 0;
	if (!ret && printf(",\n%*s\"executable\": {\n%*s\"status\": \"%s\","
					   "\n%*s\"path_status\": \"%s\",\n%*s\"path\": ",
					   indent + 2, "", indent + 4, "", executable_status, indent + 4, "", path_status,
					   indent + 4, "") < 0)
		ret = EIO;
	if (!ret && identity->executable_path)
		ret = mapple_pslist_print_json_bytes(identity->executable_path, identity->executable_path_length);
	else if (!ret && fputs("null", stdout) == EOF)
		ret = EIO;
	if (!ret && printf(",\n%*s\"deleted\": %s,\n%*s\"metadata_status\": \"%s\","
					   "\n%*s\"device_major\": ",
					   indent + 4, "",
					   identity->executable_flags & MAPPLE_PSLIST_EXECUTABLE_FLAG_DELETED ? "true" : "false",
					   indent + 4, "", metadata_status, indent + 4, "") < 0)
		ret = EIO;
	if (!ret && identity->executable_status == MAPPLE_PSLIST_EXECUTABLE_AVAILABLE &&
		!(identity->executable_flags & MAPPLE_PSLIST_EXECUTABLE_FLAG_METADATA_UNAVAILABLE)) {
		if (printf("%u,\n%*s\"device_minor\": %u,\n%*s\"inode\": %" PRIu64
				   ",\n%*s\"mode\": %u,\n%*s\"size\": %" PRIu64 ",\n%*s\"mtime_ns\": %" PRId64
				   ",\n%*s\"ctime_ns\": %" PRId64 ",\n%*s\"filesystem_magic\": %" PRIu64,
				   identity->executable_device_major, indent + 4, "", identity->executable_device_minor,
				   indent + 4, "", (uint64_t)identity->executable_inode, indent + 4, "",
				   identity->executable_mode, indent + 4, "", (uint64_t)identity->executable_size, indent + 4,
				   "", (int64_t)identity->executable_mtime_ns, indent + 4, "",
				   (int64_t)identity->executable_ctime_ns, indent + 4, "",
				   (uint64_t)identity->executable_filesystem_magic) < 0)
			ret = EIO;
	} else if (!ret && fputs("null", stdout) == EOF) {
		ret = EIO;
	} else if (!ret && printf(",\n%*s\"device_minor\": null,\n%*s\"inode\": null,"
							  "\n%*s\"mode\": null,\n%*s\"size\": null,"
							  "\n%*s\"mtime_ns\": null,\n%*s\"ctime_ns\": null,"
							  "\n%*s\"filesystem_magic\": null",
							  indent + 4, "", indent + 4, "", indent + 4, "", indent + 4, "", indent + 4, "",
							  indent + 4, "", indent + 4, "") < 0) {
		ret = EIO;
	}
	if (!ret && printf("\n%*s}\n%*s}", indent + 2, "", indent, "") < 0)
		ret = EIO;
	return ret;
}

static int mapple_pslist_print_tasks(const struct mapple_pslist_snapshot *snapshot)
{
	size_t group_end;
	size_t index = 0;
	int first;
	int ret;

	if (fputs("  \"tasks\": {\n    \"processes\": [", stdout) == EOF)
		return EIO;
	first = 1;
	while (index < snapshot->task_count && snapshot->tasks[index].type != MAPPLE_PSLIST_TASK_KTHREAD) {
		group_end = index + 1;
		while (group_end < snapshot->task_count &&
			   snapshot->tasks[group_end].type != MAPPLE_PSLIST_TASK_KTHREAD &&
			   snapshot->tasks[group_end].tgid == snapshot->tasks[index].tgid)
			++group_end;
		if (snapshot->tasks[index].type == MAPPLE_PSLIST_TASK_PROCESS) {
			size_t thread;

			if (fputs(first ? "\n      {\"process\": " : ",\n      {\"process\": ", stdout) == EOF)
				return EIO;
			ret = mapple_pslist_print_task(&snapshot->tasks[index], snapshot->effective_groups, 0);
			if (ret || fputs(", \"threads\": [", stdout) == EOF)
				return ret ? ret : EIO;
			for (thread = index + 1; thread < group_end; ++thread) {
				if (fputs(thread == index + 1 ? "\n" : ",\n", stdout) == EOF)
					return EIO;
				ret = mapple_pslist_print_task(&snapshot->tasks[thread], snapshot->effective_groups, 8);
				if (ret)
					return ret;
			}
			if (group_end > index + 1 && fputs("\n      ", stdout) == EOF)
				return EIO;
			if (fputs("]}", stdout) == EOF)
				return EIO;
			first = 0;
		}
		index = group_end;
	}
	if (!first && fputs("\n    ", stdout) == EOF)
		return EIO;
	if (fputs("],\n    \"unmatched_threads\": [", stdout) == EOF)
		return EIO;
	first = 1;
	index = 0;
	while (index < snapshot->task_count && snapshot->tasks[index].type != MAPPLE_PSLIST_TASK_KTHREAD) {
		group_end = index + 1;
		while (group_end < snapshot->task_count &&
			   snapshot->tasks[group_end].type != MAPPLE_PSLIST_TASK_KTHREAD &&
			   snapshot->tasks[group_end].tgid == snapshot->tasks[index].tgid)
			++group_end;
		if (snapshot->tasks[index].type != MAPPLE_PSLIST_TASK_PROCESS) {
			for (; index < group_end; ++index) {
				if (fputs(first ? "\n" : ",\n", stdout) == EOF)
					return EIO;
				ret = mapple_pslist_print_task(&snapshot->tasks[index], snapshot->effective_groups, 6);
				if (ret)
					return ret;
				first = 0;
			}
		} else {
			index = group_end;
		}
	}
	if (!first && fputs("\n    ", stdout) == EOF)
		return EIO;
	if (fputs("],\n    \"kthreads\": [", stdout) == EOF)
		return EIO;
	first = 1;
	for (; index < snapshot->task_count; ++index) {
		if (snapshot->tasks[index].type != MAPPLE_PSLIST_TASK_KTHREAD)
			continue;
		if (fputs(first ? "\n" : ",\n", stdout) == EOF)
			return EIO;
		ret = mapple_pslist_print_task(&snapshot->tasks[index], snapshot->effective_groups, 6);
		if (ret)
			return ret;
		first = 0;
	}
	if (!first && fputs("\n    ", stdout) == EOF)
		return EIO;
	return fputs("]\n  },\n", stdout) == EOF ? EIO : 0;
}

static int mapple_pslist_print_objects(const struct mapple_pslist_snapshot *snapshot)
{
	size_t index;
	int first = 1;
	int ret;

	if (fputs("  \"objects\": {", stdout) == EOF)
		return EIO;
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_IDENTITY) {
		if (fputs("\n    \"identity\": {", stdout) == EOF)
			return EIO;
		for (index = 0; index < snapshot->identity_count; ++index) {
			if (printf("%s\n      \"%" PRIu64 "\": ", index ? "," : "",
					   (uint64_t)snapshot->identities[index].object_id) < 0)
				return EIO;
			ret = mapple_pslist_print_identity(&snapshot->identities[index], 6);
			if (ret)
				return ret;
		}
		if (snapshot->identity_count && fputs("\n    ", stdout) == EOF)
			return EIO;
		if (putchar('}') == EOF)
			return EIO;
		first = 0;
	}
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_ENVIRONMENT) {
		if (fputs(first ? "\n    \"environment\": {" : ",\n    \"environment\": {", stdout) == EOF)
			return EIO;
		for (index = 0; index < snapshot->environment_count; ++index) {
			if (printf("%s\n      \"%" PRIu64 "\": ", index ? "," : "",
					   (uint64_t)snapshot->environments[index].object_id) < 0)
				return EIO;
			ret = mapple_pslist_print_json_environment(&snapshot->environments[index], 6);
			if (ret)
				return ret;
		}
		if (snapshot->environment_count && fputs("\n    ", stdout) == EOF)
			return EIO;
		if (putchar('}') == EOF)
			return EIO;
		first = 0;
	}
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_SECURITY) {
		if (fputs(first ? "\n    \"security\": {" : ",\n    \"security\": {", stdout) == EOF)
			return EIO;
		for (index = 0; index < snapshot->security_count; ++index) {
			if (printf("%s\n      \"%" PRIu64 "\": ", index ? "," : "",
					   (uint64_t)snapshot->security[index].object_id) < 0)
				return EIO;
			ret = mapple_pslist_print_json_security(&snapshot->security[index], 6);
			if (ret)
				return ret;
		}
		if (snapshot->security_count && fputs("\n    ", stdout) == EOF)
			return EIO;
		if (putchar('}') == EOF)
			return EIO;
		first = 0;
	}
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_CONTEXT) {
		if (fputs(first ? "\n    \"context\": {" : ",\n    \"context\": {", stdout) == EOF)
			return EIO;
		for (index = 0; index < snapshot->context_count; ++index) {
			if (printf("%s\n      \"%" PRIu64 "\": ", index ? "," : "",
					   (uint64_t)snapshot->contexts[index].object_id) < 0)
				return EIO;
			ret = mapple_pslist_print_json_context(&snapshot->contexts[index], 6);
			if (ret)
				return ret;
		}
		if (snapshot->context_count && fputs("\n    ", stdout) == EOF)
			return EIO;
		if (putchar('}') == EOF)
			return EIO;
		first = 0;
	}
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_RESOURCES) {
		if (fputs(first ? "\n    \"resources\": {" : ",\n    \"resources\": {", stdout) == EOF)
			return EIO;
		for (index = 0; index < snapshot->resources_count; ++index) {
			if (printf("%s\n      \"%" PRIu64 "\": ", index ? "," : "",
					   (uint64_t)snapshot->resources[index].object_id) < 0)
				return EIO;
			ret = mapple_pslist_print_json_resources(&snapshot->resources[index], 6);
			if (ret)
				return ret;
		}
		if (snapshot->resources_count && fputs("\n    ", stdout) == EOF)
			return EIO;
		if (putchar('}') == EOF)
			return EIO;
		first = 0;
	}
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_FDS) {
		if (fputs(first ? "\n    \"fds\": {" : ",\n    \"fds\": {", stdout) == EOF)
			return EIO;
		for (index = 0; index < snapshot->fd_table_count; ++index) {
			if (printf("%s\n      \"%" PRIu64 "\": ", index ? "," : "",
					   (uint64_t)snapshot->fd_tables[index].object_id) < 0)
				return EIO;
			ret = mapple_pslist_print_json_fd_table(&snapshot->fd_tables[index], 6);
			if (ret)
				return ret;
		}
		if (snapshot->fd_table_count && fputs("\n    ", stdout) == EOF)
			return EIO;
		if (putchar('}') == EOF)
			return EIO;
		first = 0;
	}
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_NETWORK) {
		if (fputs(first ? "\n    \"network\": {" : ",\n    \"network\": {", stdout) == EOF)
			return EIO;
		for (index = 0; index < snapshot->network_table_count; ++index) {
			if (printf("%s\n      \"%" PRIu64 "\": ", index ? "," : "",
					   (uint64_t)snapshot->network_tables[index].object_id) < 0)
				return EIO;
			ret = mapple_pslist_print_json_network_table(&snapshot->network_tables[index], 6);
			if (ret)
				return ret;
		}
		if (snapshot->network_table_count && fputs("\n    ", stdout) == EOF)
			return EIO;
		if (putchar('}') == EOF)
			return EIO;
		first = 0;
	}
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_MEMORY_MAPS) {
		if (fputs(first ? "\n    \"mapped_files\": " : ",\n    \"mapped_files\": ", stdout) == EOF)
			return EIO;
		ret = mapple_pslist_print_json_mapped_files(snapshot, 4);
		if (ret)
			return ret;
		first = 0;
		if (fputs(first ? "\n    \"memory_maps\": {" : ",\n    \"memory_maps\": {", stdout) == EOF)
			return EIO;
		for (index = 0; index < snapshot->memory_maps_count; ++index) {
			if (printf("%s\n      \"%" PRIu64 "\": ", index ? "," : "",
					   (uint64_t)snapshot->memory_maps[index].object_id) < 0)
				return EIO;
			ret = mapple_pslist_print_json_memory_maps(&snapshot->memory_maps[index], 6);
			if (ret)
				return ret;
		}
		if (snapshot->memory_maps_count && fputs("\n    ", stdout) == EOF)
			return EIO;
		if (putchar('}') == EOF)
			return EIO;
		first = 0;
	}
	if (snapshot->effective_groups & MAPPLE_PSLIST_GROUP_ELF) {
		if (fputs(first ? "\n    \"elf\": {" : ",\n    \"elf\": {", stdout) == EOF)
			return EIO;
		for (index = 0; index < snapshot->elf_count; ++index) {
			if (printf("%s\n      \"%" PRIu64 "\": ", index ? "," : "",
					   (uint64_t)snapshot->elfs[index].object_id) < 0)
				return EIO;
			ret = mapple_pslist_print_json_elf(&snapshot->elfs[index], 6);
			if (ret)
				return ret;
		}
		if (snapshot->elf_count && fputs("\n    ", stdout) == EOF)
			return EIO;
		if (putchar('}') == EOF)
			return EIO;
		first = 0;
	}
	if (!first && putchar('\n') == EOF)
		return EIO;
	return fputs("  }\n", stdout) == EOF ? EIO : 0;
}

static int mapple_pslist_print_snapshot(struct mapple_pslist_snapshot *snapshot)
{
	const char *status = mapple_pslist_snapshot_status_name(snapshot->status);
	int ret;

	if (!status)
		return EPROTO;
	qsort(snapshot->tasks, snapshot->task_count, sizeof(*snapshot->tasks), mapple_pslist_compare_tasks);
	if (printf("{\n  \"snapshot\": {\"status\": \"%s\", \"start_boottime_ns\": %" PRIu64
			   ", \"end_boottime_ns\": %" PRIu64 ", \"requested_groups\": ",
			   status, (uint64_t)snapshot->start_boottime_ns, (uint64_t)snapshot->end_boottime_ns) < 0)
		return EIO;
	ret = mapple_pslist_print_group_names(snapshot->requested_groups);
	if (!ret && fputs(", \"effective_groups\": ", stdout) == EOF)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_print_group_names(snapshot->effective_groups);
	if (!ret && fputs("},\n", stdout) == EOF)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_print_tasks(snapshot);
	if (!ret)
		ret = mapple_pslist_print_objects(snapshot);
	if (!ret && fputs("}\n", stdout) == EOF)
		ret = EIO;
	return ret;
}

int run_pslist_groups(int session_fd, const char *const *group_names, size_t group_count)
{
	struct mapple_pslist_snapshot snapshot = {0};
	struct mapple_pslist_error_response error;
	struct mapple_pslist_request request;
	struct mapple_wire_header response;
	__u64 groups;
	__u64 payload_size;
	__u32 flags;
	int ret;

	ret = mapple_pslist_groups_from_names(group_names, group_count, &groups);
	if (ret)
		return ret;
	request.groups = htobe64(groups);
	ret = send_command(session_fd, MAPPLE_COMMAND_PSLIST, &request, sizeof(request));
	if (ret)
		return ret;
	ret = receive_response_header(session_fd, &response);
	if (ret)
		return ret;
	if (ntohs(response.command) != MAPPLE_COMMAND_PSLIST)
		return EPROTO;
	flags = ntohl(response.flags);
	payload_size = be64toh(response.payload_size);
	if (!(flags & MAPPLE_HEADER_FLAG_STREAM)) {
		if (flags || payload_size != sizeof(error))
			return EPROTO;
		ret = read_exact(session_fd, &error, sizeof(error));
		if (ret)
			return ret;
		if (error.reserved || be64toh(error.supported_groups) != MAPPLE_PSLIST_SUPPORTED_GROUPS)
			return EPROTO;
		ret = ntohl(error.error_code);
		return ret ? ret : EPROTO;
	}
	if (flags != MAPPLE_HEADER_FLAG_STREAM || payload_size)
		return EPROTO;
	ret = mapple_pslist_read_snapshot(session_fd, &snapshot);
	if (!ret && snapshot.requested_groups != groups)
		ret = EPROTO;
	if (!ret)
		ret = mapple_pslist_build_mapped_files(&snapshot);
	if (!ret)
		ret = mapple_pslist_print_snapshot(&snapshot);
	mapple_pslist_free_snapshot(&snapshot);
	return ret;
}

int run_pslist(int session_fd)
{
	static const char *const default_groups[] = {
		"identity", "environment", "security", "context",	  "resources",
		"fds",		"fds-details", "network",  "memory-maps", "elf",
	};

	return run_pslist_groups(session_fd, default_groups, sizeof(default_groups) / sizeof(default_groups[0]));
}
