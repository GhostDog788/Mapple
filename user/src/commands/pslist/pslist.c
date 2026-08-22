#include <commands/pslist/pslist.h>

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <commands.h>
#include <communication/command.h>
#include <communication/stream.h>

#define MAPPLE_PSLIST_INITIAL_CAPACITY 64U
#define MAPPLE_PSLIST_IO_CHUNK_SIZE 4096U

struct mapple_pslist_process {
	__u32 tgid;
	__u32 ppid;
	unsigned char *command_line;
	__u32 command_line_length;
};

struct mapple_pslist_task {
	__u32 tid;
	__u32 tgid;
	__u8 state;
	char comm[MAPPLE_PSLIST_COMM_SIZE];
};

struct mapple_pslist_snapshot {
	struct mapple_pslist_process *processes;
	size_t process_count;
	size_t process_capacity;
	struct mapple_pslist_task *tasks;
	size_t task_count;
	size_t task_capacity;
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

static int mapple_pslist_skip(int session_fd, size_t size)
{
	unsigned char buffer[MAPPLE_PSLIST_IO_CHUNK_SIZE];
	size_t chunk_size;
	int ret;

	while (size) {
		chunk_size = size < sizeof(buffer) ? size : sizeof(buffer);
		ret = read_exact(session_fd, buffer, chunk_size);
		if (ret)
			return ret;
		size -= chunk_size;
	}
	return 0;
}

static int mapple_pslist_grow_array(void **items, size_t item_size, size_t count,
				    size_t *capacity)
{
	size_t new_capacity;
	void *new_items;

	if (count < *capacity)
		return 0;
	if (!*capacity)
		new_capacity = MAPPLE_PSLIST_INITIAL_CAPACITY;
	else {
		if (*capacity > SIZE_MAX / 2)
			return EOVERFLOW;
		new_capacity = *capacity * 2;
	}
	if (new_capacity > SIZE_MAX / item_size)
		return EOVERFLOW;
	new_items = realloc(*items, new_capacity * item_size);
	if (!new_items)
		return ENOMEM;
	*items = new_items;
	*capacity = new_capacity;
	return 0;
}

static int mapple_pslist_add_process(struct mapple_pslist_snapshot *snapshot,
				     struct mapple_pslist_process *process)
{
	int ret;

	ret = mapple_pslist_grow_array((void **)&snapshot->processes, sizeof(*snapshot->processes),
				       snapshot->process_count, &snapshot->process_capacity);
	if (ret)
		return ret;
	snapshot->processes[snapshot->process_count++] = *process;
	process->command_line = NULL;
	return 0;
}

static int mapple_pslist_add_task(struct mapple_pslist_snapshot *snapshot,
				  const struct mapple_pslist_task *task)
{
	int ret;

	ret = mapple_pslist_grow_array((void **)&snapshot->tasks, sizeof(*snapshot->tasks),
				       snapshot->task_count, &snapshot->task_capacity);
	if (ret)
		return ret;
	snapshot->tasks[snapshot->task_count++] = *task;
	return 0;
}

static void mapple_pslist_free_snapshot(struct mapple_pslist_snapshot *snapshot)
{
	size_t index;

	for (index = 0; index < snapshot->process_count; ++index)
		free(snapshot->processes[index].command_line);
	free(snapshot->processes);
	free(snapshot->tasks);
}

static int mapple_pslist_read_process(int session_fd,
				      const struct mapple_pslist_record_header *header,
				      __u32 record_size, struct mapple_pslist_snapshot *snapshot)
{
	struct mapple_pslist_process_record wire;
	struct mapple_pslist_process process = { 0 };
	__u64 minimum_size;
	size_t extension_size;
	int ret;

	if (record_size < sizeof(wire))
		return EPROTO;
	memcpy(&wire.header, header, sizeof(*header));
	ret = read_exact(session_fd, (unsigned char *)&wire + sizeof(wire.header),
			 sizeof(wire) - sizeof(wire.header));
	if (ret)
		return ret;
	process.tgid = mapple_pslist_read_u32(
		&wire, offsetof(struct mapple_pslist_process_record, tgid));
	process.ppid = mapple_pslist_read_u32(
		&wire, offsetof(struct mapple_pslist_process_record, ppid));
	process.command_line_length = mapple_pslist_read_u32(
		&wire, offsetof(struct mapple_pslist_process_record, command_line_length));
	minimum_size = sizeof(wire) + (__u64)process.command_line_length;
	if (!process.tgid || minimum_size > record_size)
		return EPROTO;
	if (process.command_line_length) {
		process.command_line = malloc(process.command_line_length);
		if (!process.command_line)
			return ENOMEM;
		ret = read_exact(session_fd, process.command_line, process.command_line_length);
		if (ret)
			goto free_command_line;
	}
	extension_size = record_size - (size_t)minimum_size;
	ret = mapple_pslist_skip(session_fd, extension_size);
	if (ret)
		goto free_command_line;
	ret = mapple_pslist_add_process(snapshot, &process);
	if (ret)
		goto free_command_line;
	return 0;

free_command_line:
	free(process.command_line);
	return ret;
}

static int mapple_pslist_read_task(int session_fd, const struct mapple_pslist_record_header *header,
				   __u32 record_size, struct mapple_pslist_snapshot *snapshot)
{
	struct mapple_pslist_task_record wire;
	struct mapple_pslist_task task;
	int ret;

	if (record_size < sizeof(wire))
		return EPROTO;
	memcpy(&wire.header, header, sizeof(*header));
	ret = read_exact(session_fd, (unsigned char *)&wire + sizeof(wire.header),
			 sizeof(wire) - sizeof(wire.header));
	if (ret)
		return ret;
	task.tid = mapple_pslist_read_u32(
		&wire, offsetof(struct mapple_pslist_task_record, tid));
	task.tgid = mapple_pslist_read_u32(
		&wire, offsetof(struct mapple_pslist_task_record, tgid));
	task.state = wire.state;
	memcpy(task.comm, wire.comm, sizeof(task.comm));
	if (!task.tid || !task.tgid || wire.reserved[0] || wire.reserved[1] || wire.reserved[2] ||
	    !memchr(task.comm, '\0', sizeof(task.comm)))
		return EPROTO;
	ret = mapple_pslist_skip(session_fd, record_size - sizeof(wire));
	if (ret)
		return ret;
	return mapple_pslist_add_task(snapshot, &task);
}

static int mapple_pslist_read_snapshot(int session_fd, struct mapple_pslist_snapshot *snapshot)
{
	const struct mapple_pslist_record_header terminator = { 0 };
	struct mapple_pslist_record_header header;
	__u32 record_size;
	__u16 record_type;
	int ret;

	for (;;) {
		ret = read_exact(session_fd, &header, sizeof(header));
		if (ret)
			return ret;
		record_size = mapple_pslist_read_u32(
			&header, offsetof(struct mapple_pslist_record_header, record_size));
		record_type = mapple_pslist_read_u16(
			&header, offsetof(struct mapple_pslist_record_header, record_type));
		if (!record_size)
			return memcmp(&header, &terminator, sizeof(header)) ? EPROTO : 0;
		if (record_size < sizeof(header) ||
		    mapple_pslist_read_u16(
			    &header, offsetof(struct mapple_pslist_record_header, reserved)))
			return EPROTO;

		switch (record_type) {
		case MAPPLE_PSLIST_RECORD_TASK:
			ret = mapple_pslist_read_task(session_fd, &header, record_size, snapshot);
			break;
		case MAPPLE_PSLIST_RECORD_PROCESS:
			ret = mapple_pslist_read_process(session_fd, &header, record_size, snapshot);
			break;
		case 0:
			return EPROTO;
		default:
			ret = mapple_pslist_skip(session_fd, record_size - sizeof(header));
			break;
		}
		if (ret)
			return ret;
	}
}

static int mapple_pslist_compare_processes(const void *left, const void *right)
{
	const struct mapple_pslist_process *first = left;
	const struct mapple_pslist_process *second = right;

	return first->tgid < second->tgid ? -1 : first->tgid > second->tgid;
}

static int mapple_pslist_compare_task_ids(const void *left, const void *right)
{
	const struct mapple_pslist_task *first = left;
	const struct mapple_pslist_task *second = right;

	return first->tid < second->tid ? -1 : first->tid > second->tid;
}

static int mapple_pslist_compare_task_groups(const void *left, const void *right)
{
	const struct mapple_pslist_task *first = left;
	const struct mapple_pslist_task *second = right;

	if (first->tgid != second->tgid)
		return first->tgid < second->tgid ? -1 : 1;
	return first->tid < second->tid ? -1 : first->tid > second->tid;
}

static int mapple_pslist_sort_snapshot(struct mapple_pslist_snapshot *snapshot)
{
	size_t index;

	if (snapshot->process_count > 1)
		qsort(snapshot->processes, snapshot->process_count, sizeof(*snapshot->processes),
		      mapple_pslist_compare_processes);
	for (index = 1; index < snapshot->process_count; ++index) {
		if (snapshot->processes[index - 1].tgid == snapshot->processes[index].tgid)
			return EPROTO;
	}
	if (snapshot->task_count > 1)
		qsort(snapshot->tasks, snapshot->task_count, sizeof(*snapshot->tasks),
		      mapple_pslist_compare_task_ids);
	for (index = 1; index < snapshot->task_count; ++index) {
		if (snapshot->tasks[index - 1].tid == snapshot->tasks[index].tid)
			return EPROTO;
	}
	if (snapshot->task_count > 1)
		qsort(snapshot->tasks, snapshot->task_count, sizeof(*snapshot->tasks),
		      mapple_pslist_compare_task_groups);
	return 0;
}

static int mapple_pslist_print_json_bytes(const unsigned char *bytes, size_t length)
{
	size_t index;
	int ret;

	if (putchar('"') == EOF)
		return EIO;
	for (index = 0; index < length; ++index) {
		switch (bytes[index]) {
		case '"':
			ret = fputs("\\\"", stdout);
			break;
		case '\\':
			ret = fputs("\\\\", stdout);
			break;
		case '\b':
			ret = fputs("\\b", stdout);
			break;
		case '\f':
			ret = fputs("\\f", stdout);
			break;
		case '\n':
			ret = fputs("\\n", stdout);
			break;
		case '\r':
			ret = fputs("\\r", stdout);
			break;
		case '\t':
			ret = fputs("\\t", stdout);
			break;
		default:
			if (bytes[index] >= 0x20 && bytes[index] < 0x7f)
				ret = putchar(bytes[index]);
			else
				ret = printf("\\u%04x", bytes[index]);
			break;
		}
		if (ret == EOF || ret < 0)
			return EIO;
	}
	return putchar('"') == EOF ? EIO : 0;
}

static int mapple_pslist_print_json_command_line(const unsigned char *command_line, __u32 length)
{
	size_t argument_end;
	size_t offset = 0;
	int ret;

	if (putchar('[') == EOF)
		return EIO;
	while (offset < length) {
		argument_end = offset;
		while (argument_end < length && command_line[argument_end])
			++argument_end;
		if (offset && putchar(',') == EOF)
			return EIO;
		ret = mapple_pslist_print_json_bytes(command_line + offset, argument_end - offset);
		if (ret)
			return ret;
		if (argument_end == length)
			break;
		offset = argument_end + 1;
	}
	return putchar(']') == EOF ? EIO : 0;
}

static int mapple_pslist_print_json_task(const struct mapple_pslist_task *task)
{
	int ret;

	if (printf("        {\"tid\": %u, \"state\": ", task->tid) < 0)
		return EIO;
	ret = mapple_pslist_print_json_bytes(&task->state, sizeof(task->state));
	if (ret)
		return ret;
	if (fputs(", \"comm\": ", stdout) == EOF)
		return EIO;
	ret = mapple_pslist_print_json_bytes((const unsigned char *)task->comm,
					     strnlen(task->comm, sizeof(task->comm)));
	if (ret)
		return ret;
	return putchar('}') == EOF ? EIO : 0;
}

static int mapple_pslist_print_json_group(__u32 tgid,
					  const struct mapple_pslist_process *process,
					  const struct mapple_pslist_task *tasks,
					  size_t task_count)
{
	const struct mapple_pslist_task *leader = NULL;
	int first_thread = 1;
	size_t index;
	int ret;

	for (index = 0; index < task_count; ++index) {
		if (tasks[index].tid == tgid) {
			leader = &tasks[index];
			break;
		}
	}
	if (printf("    {\n      \"pid\": %u,\n      \"ppid\": ", tgid) < 0)
		return EIO;
	if (process) {
		if (printf("%u", process->ppid) < 0)
			return EIO;
	} else if (fputs("null", stdout) == EOF) {
		return EIO;
	}
	if (fputs(",\n      \"state\": ", stdout) == EOF)
		return EIO;
	if (leader) {
		ret = mapple_pslist_print_json_bytes(&leader->state, sizeof(leader->state));
		if (ret)
			return ret;
	} else if (fputs("null", stdout) == EOF) {
		return EIO;
	}
	if (fputs(",\n      \"comm\": ", stdout) == EOF)
		return EIO;
	if (leader) {
		ret = mapple_pslist_print_json_bytes(
			(const unsigned char *)leader->comm,
			strnlen(leader->comm, sizeof(leader->comm)));
		if (ret)
			return ret;
	} else if (fputs("null", stdout) == EOF) {
		return EIO;
	}
	if (fputs(",\n      \"command_line\": ", stdout) == EOF)
		return EIO;
	if (process) {
		ret = mapple_pslist_print_json_command_line(process->command_line,
							    process->command_line_length);
		if (ret)
			return ret;
	} else if (fputs("null", stdout) == EOF) {
		return EIO;
	}
	if (fputs(",\n      \"threads\": [", stdout) == EOF)
		return EIO;
	for (index = 0; index < task_count; ++index) {
		if (&tasks[index] == leader)
			continue;
		if (fputs(first_thread ? "\n" : ",\n", stdout) == EOF)
			return EIO;
		ret = mapple_pslist_print_json_task(&tasks[index]);
		if (ret)
			return ret;
		first_thread = 0;
	}
	if (!first_thread && fputs("\n      ", stdout) == EOF)
		return EIO;
	return fputs("]\n    }", stdout) == EOF ? EIO : 0;
}

static int mapple_pslist_print_snapshot(const struct mapple_pslist_snapshot *snapshot)
{
	const struct mapple_pslist_process *process;
	size_t process_index = 0;
	size_t task_index = 0;
	size_t task_end;
	__u32 process_tgid;
	__u32 task_tgid;
	__u32 tgid;
	int first_process = 1;
	int ret;

	if (fputs("{\n  \"processes\": [", stdout) == EOF)
		return EIO;
	while (process_index < snapshot->process_count || task_index < snapshot->task_count) {
		process_tgid = process_index < snapshot->process_count ?
			snapshot->processes[process_index].tgid : UINT32_MAX;
		task_tgid = task_index < snapshot->task_count ?
			snapshot->tasks[task_index].tgid : UINT32_MAX;
		tgid = process_tgid < task_tgid ? process_tgid : task_tgid;
		process = process_tgid == tgid ? &snapshot->processes[process_index] : NULL;
		task_end = task_index;
		while (task_end < snapshot->task_count && snapshot->tasks[task_end].tgid == tgid)
			++task_end;
		if (fputs(first_process ? "\n" : ",\n", stdout) == EOF)
			return EIO;
		ret = mapple_pslist_print_json_group(
			tgid, process, task_end == task_index ? NULL : &snapshot->tasks[task_index],
			task_end - task_index);
		if (ret)
			return ret;
		first_process = 0;
		if (process)
			++process_index;
		task_index = task_end;
	}
	if (!first_process && fputs("\n  ", stdout) == EOF)
		return EIO;
	return fputs("]\n}\n", stdout) == EOF ? EIO : 0;
}

int run_pslist(int session_fd)
{
	struct mapple_pslist_snapshot snapshot = { 0 };
	struct mapple_wire_header response;
	int ret;

	ret = send_command(session_fd, MAPPLE_COMMAND_PSLIST, NULL, 0);
	if (ret)
		return ret;
	ret = receive_response_header(session_fd, &response);
	if (ret)
		return ret;
	if (ntohs(response.command) != MAPPLE_COMMAND_PSLIST ||
	    ntohl(response.flags) != MAPPLE_HEADER_FLAG_STREAM || be64toh(response.payload_size))
		return EPROTO;

	ret = mapple_pslist_read_snapshot(session_fd, &snapshot);
	if (!ret)
		ret = mapple_pslist_sort_snapshot(&snapshot);
	if (!ret)
		ret = mapple_pslist_print_snapshot(&snapshot);
	mapple_pslist_free_snapshot(&snapshot);
	return ret;
}
