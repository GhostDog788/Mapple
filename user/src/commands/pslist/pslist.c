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
#include <communication/command.h>
#include <communication/stream.h>

#define MAPPLE_PSLIST_INITIAL_CAPACITY 64U
#define MAPPLE_PSLIST_IO_CHUNK_SIZE 4096U

struct mapple_pslist_task {
	__u32 pid;
	__u32 tgid;
	__u32 ppid;
	__u64 start_boottime_ns;
	__u8 state;
	char comm[MAPPLE_PSLIST_COMM_SIZE];
};

struct mapple_pslist_process {
	__u16 command_line_status;
	__u16 executable_status;
	__u32 executable_flags;
	__u32 executable_device_major;
	__u32 executable_device_minor;
	__u32 executable_mode;
	unsigned char *command_line;
	__u32 command_line_length;
	unsigned char *executable_path;
	__u32 executable_path_length;
	__u64 executable_inode;
	__u64 executable_size;
	__s64 executable_mtime_ns;
	__s64 executable_ctime_ns;
	__u64 executable_filesystem_magic;
};

union mapple_pslist_entry_data {
	struct mapple_pslist_process process;
};

struct mapple_pslist_entry {
	__u16 type;
	struct mapple_pslist_task task;
	union mapple_pslist_entry_data data;
};

struct mapple_pslist_snapshot {
	struct mapple_pslist_entry *entries;
	size_t count;
	size_t capacity;
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

static int mapple_pslist_grow_snapshot(struct mapple_pslist_snapshot *snapshot)
{
	size_t new_capacity;
	struct mapple_pslist_entry *new_entries;

	if (snapshot->count < snapshot->capacity)
		return 0;
	if (!snapshot->capacity)
		new_capacity = MAPPLE_PSLIST_INITIAL_CAPACITY;
	else {
		if (snapshot->capacity > SIZE_MAX / 2)
			return EOVERFLOW;
		new_capacity = snapshot->capacity * 2;
	}
	if (new_capacity > SIZE_MAX / sizeof(*snapshot->entries))
		return EOVERFLOW;
	new_entries = realloc(snapshot->entries, new_capacity * sizeof(*snapshot->entries));
	if (!new_entries)
		return ENOMEM;
	snapshot->entries = new_entries;
	snapshot->capacity = new_capacity;
	return 0;
}

static int mapple_pslist_add_entry(struct mapple_pslist_snapshot *snapshot,
				   struct mapple_pslist_entry *entry)
{
	int ret;

	ret = mapple_pslist_grow_snapshot(snapshot);
	if (ret)
		return ret;
	snapshot->entries[snapshot->count++] = *entry;
	if (entry->type == MAPPLE_PSLIST_RECORD_PROCESS) {
		entry->data.process.command_line = NULL;
		entry->data.process.executable_path = NULL;
	}
	return 0;
}

static void mapple_pslist_free_snapshot(struct mapple_pslist_snapshot *snapshot)
{
	size_t index;

	for (index = 0; index < snapshot->count; ++index) {
		if (snapshot->entries[index].type != MAPPLE_PSLIST_RECORD_PROCESS)
			continue;
		free(snapshot->entries[index].data.process.command_line);
		free(snapshot->entries[index].data.process.executable_path);
	}
	free(snapshot->entries);
}

static int mapple_pslist_validate_process(const struct mapple_pslist_process *process,
					  __u64 minimum_size, __u32 record_size)
{
	if (process->command_line_status < MAPPLE_PSLIST_COMMAND_LINE_AVAILABLE ||
	    process->command_line_status > MAPPLE_PSLIST_COMMAND_LINE_TOO_LARGE ||
	    process->executable_status < MAPPLE_PSLIST_EXECUTABLE_AVAILABLE ||
	    process->executable_status > MAPPLE_PSLIST_EXECUTABLE_UNAVAILABLE ||
	    process->command_line_length > MAPPLE_PSLIST_COMMAND_LINE_MAX_SIZE ||
	    (process->executable_flags & ~MAPPLE_PSLIST_EXECUTABLE_KNOWN_FLAGS) ||
	    minimum_size > record_size)
		return EPROTO;
	if (process->command_line_status == MAPPLE_PSLIST_COMMAND_LINE_AVAILABLE) {
		if (!process->command_line_length)
			return EPROTO;
	} else if (process->command_line_length) {
		return EPROTO;
	}
	if (process->executable_status == MAPPLE_PSLIST_EXECUTABLE_AVAILABLE) {
		if (!!(process->executable_flags & MAPPLE_PSLIST_EXECUTABLE_FLAG_PATH_UNAVAILABLE) ==
		    !!process->executable_path_length)
			return EPROTO;
	} else if (process->executable_flags || process->executable_path_length ||
		   process->executable_device_major || process->executable_device_minor ||
		   process->executable_mode || process->executable_inode ||
		   process->executable_size || process->executable_mtime_ns ||
		   process->executable_ctime_ns || process->executable_filesystem_magic) {
		return EPROTO;
	}
	if (process->executable_flags & MAPPLE_PSLIST_EXECUTABLE_FLAG_METADATA_UNAVAILABLE &&
	    (process->executable_device_major || process->executable_device_minor ||
	     process->executable_mode || process->executable_inode || process->executable_size ||
	     process->executable_mtime_ns || process->executable_ctime_ns ||
	     process->executable_filesystem_magic))
		return EPROTO;
	return 0;
}

static int mapple_pslist_decode_task(const struct mapple_pslist_record_header *header,
				     struct mapple_pslist_task *task)
{
	const struct mapple_pslist_task_data *wire = &header->task;

	task->pid = mapple_pslist_read_u32(wire, offsetof(struct mapple_pslist_task_data, pid));
	task->tgid = mapple_pslist_read_u32(wire, offsetof(struct mapple_pslist_task_data, tgid));
	task->ppid = mapple_pslist_read_u32(wire, offsetof(struct mapple_pslist_task_data, ppid));
	task->start_boottime_ns = mapple_pslist_read_u64(
		wire, offsetof(struct mapple_pslist_task_data, start_boottime_ns));
	task->state = wire->state;
	memcpy(task->comm, wire->comm, sizeof(task->comm));
	if (!task->pid || !task->tgid || !mapple_pslist_state_name(task->state) ||
	    wire->reserved[0] || wire->reserved[1] || wire->reserved[2] ||
	    !memchr(task->comm, '\0', sizeof(task->comm)))
		return EPROTO;
	return 0;
}

static int mapple_pslist_read_process(int session_fd, __u32 record_size,
				      struct mapple_pslist_entry *entry)
{
	struct mapple_pslist_process_data wire;
	struct mapple_pslist_process *process = &entry->data.process;
	__u64 minimum_size;
	size_t extension_size;
	int ret;

	if (record_size < sizeof(struct mapple_pslist_record_header) + sizeof(wire))
		return EPROTO;
	ret = read_exact(session_fd, &wire, sizeof(wire));
	if (ret)
		return ret;
	process->command_line_status = mapple_pslist_read_u16(
		&wire, offsetof(struct mapple_pslist_process_data, command_line_status));
	process->executable_status = mapple_pslist_read_u16(
		&wire, offsetof(struct mapple_pslist_process_data, executable_status));
	process->executable_flags = mapple_pslist_read_u32(
		&wire, offsetof(struct mapple_pslist_process_data, executable_flags));
	process->command_line_length = mapple_pslist_read_u32(
		&wire, offsetof(struct mapple_pslist_process_data, command_line_length));
	process->executable_path_length = mapple_pslist_read_u32(
		&wire, offsetof(struct mapple_pslist_process_data, executable_path_length));
	process->executable_device_major = mapple_pslist_read_u32(
		&wire, offsetof(struct mapple_pslist_process_data, executable_device_major));
	process->executable_device_minor = mapple_pslist_read_u32(
		&wire, offsetof(struct mapple_pslist_process_data, executable_device_minor));
	process->executable_mode = mapple_pslist_read_u32(
		&wire, offsetof(struct mapple_pslist_process_data, executable_mode));
	process->executable_inode = mapple_pslist_read_u64(
		&wire, offsetof(struct mapple_pslist_process_data, executable_inode));
	process->executable_size = mapple_pslist_read_u64(
		&wire, offsetof(struct mapple_pslist_process_data, executable_size));
	process->executable_mtime_ns = (__s64)mapple_pslist_read_u64(
		&wire, offsetof(struct mapple_pslist_process_data, executable_mtime_ns));
	process->executable_ctime_ns = (__s64)mapple_pslist_read_u64(
		&wire, offsetof(struct mapple_pslist_process_data, executable_ctime_ns));
	process->executable_filesystem_magic = mapple_pslist_read_u64(
		&wire, offsetof(struct mapple_pslist_process_data, executable_filesystem_magic));
	minimum_size = sizeof(struct mapple_pslist_record_header) + sizeof(wire) +
		       (__u64)process->command_line_length + process->executable_path_length;
	ret = mapple_pslist_validate_process(process, minimum_size, record_size);
	if (ret)
		return ret;
	if (process->command_line_length) {
		process->command_line = malloc(process->command_line_length);
		if (!process->command_line)
			return ENOMEM;
		ret = read_exact(session_fd, process->command_line, process->command_line_length);
		if (ret)
			goto free_process;
	}
	if (process->executable_path_length) {
		process->executable_path = malloc(process->executable_path_length);
		if (!process->executable_path) {
			ret = ENOMEM;
			goto free_process;
		}
		ret = read_exact(session_fd, process->executable_path,
				 process->executable_path_length);
		if (ret)
			goto free_process;
	}
	extension_size = record_size - (size_t)minimum_size;
	ret = mapple_pslist_skip(session_fd, extension_size);
	if (ret)
		goto free_process;
	return 0;

free_process:
	free(process->command_line);
	free(process->executable_path);
	process->command_line = NULL;
	process->executable_path = NULL;
	return ret;
}

static int mapple_pslist_read_snapshot(int session_fd, struct mapple_pslist_snapshot *snapshot)
{
	const struct mapple_pslist_record_header terminator = { 0 };
	struct mapple_pslist_record_header header;
	struct mapple_pslist_entry entry;
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
		memset(&entry, 0, sizeof(entry));
		entry.type = record_type;
		ret = mapple_pslist_decode_task(&header, &entry.task);
		if (ret)
			return ret;

		switch (record_type) {
		case MAPPLE_PSLIST_RECORD_THREAD:
			if (entry.task.pid == entry.task.tgid)
				return EPROTO;
			ret = mapple_pslist_skip(session_fd, record_size - sizeof(header));
			break;
		case MAPPLE_PSLIST_RECORD_PROCESS:
			if (entry.task.pid != entry.task.tgid)
				return EPROTO;
			ret = mapple_pslist_read_process(session_fd, record_size, &entry);
			break;
		case MAPPLE_PSLIST_RECORD_KTHREAD:
			ret = mapple_pslist_skip(session_fd, record_size - sizeof(header));
			break;
		case 0:
			return EPROTO;
		default:
			ret = mapple_pslist_skip(session_fd, record_size - sizeof(header));
			if (ret)
				return ret;
			continue;
		}
		if (ret)
			return ret;
		ret = mapple_pslist_add_entry(snapshot, &entry);
		if (ret) {
			if (entry.type == MAPPLE_PSLIST_RECORD_PROCESS) {
				free(entry.data.process.command_line);
				free(entry.data.process.executable_path);
			}
			return ret;
		}
	}
}

static int mapple_pslist_compare_entries_by_pid(const void *left, const void *right)
{
	const struct mapple_pslist_entry *first = left;
	const struct mapple_pslist_entry *second = right;

	return first->task.pid < second->task.pid ? -1 : first->task.pid > second->task.pid;
}

static int mapple_pslist_compare_entries_for_output(const void *left, const void *right)
{
	const struct mapple_pslist_entry *first = left;
	const struct mapple_pslist_entry *second = right;
	int first_is_kthread = first->type == MAPPLE_PSLIST_RECORD_KTHREAD;
	int second_is_kthread = second->type == MAPPLE_PSLIST_RECORD_KTHREAD;

	if (first_is_kthread != second_is_kthread)
		return first_is_kthread ? 1 : -1;
	if (first_is_kthread)
		return mapple_pslist_compare_entries_by_pid(left, right);
	if (first->task.tgid != second->task.tgid)
		return first->task.tgid < second->task.tgid ? -1 : 1;
	if (first->type != second->type) {
		if (first->type == MAPPLE_PSLIST_RECORD_PROCESS)
			return -1;
		if (second->type == MAPPLE_PSLIST_RECORD_PROCESS)
			return 1;
	}
	return mapple_pslist_compare_entries_by_pid(left, right);
}

static int mapple_pslist_sort_snapshot(struct mapple_pslist_snapshot *snapshot)
{
	size_t index;

	if (snapshot->count > 1)
		qsort(snapshot->entries, snapshot->count, sizeof(*snapshot->entries),
		      mapple_pslist_compare_entries_by_pid);
	for (index = 1; index < snapshot->count; ++index) {
		if (snapshot->entries[index - 1].task.pid == snapshot->entries[index].task.pid)
			return EPROTO;
	}
	if (snapshot->count > 1)
		qsort(snapshot->entries, snapshot->count, sizeof(*snapshot->entries),
		      mapple_pslist_compare_entries_for_output);
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

static int mapple_pslist_print_json_argv(const unsigned char *command_line, __u32 length)
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

static int mapple_pslist_print_json_executable(const struct mapple_pslist_process *process)
{
	const char *metadata_status;
	const char *path_status;
	const char *status;
	int ret;

	status = mapple_pslist_executable_status_name(process->executable_status);
	if (!status)
		return EPROTO;
	if (process->executable_status == MAPPLE_PSLIST_EXECUTABLE_AVAILABLE) {
		path_status = process->executable_flags &
				      MAPPLE_PSLIST_EXECUTABLE_FLAG_PATH_UNAVAILABLE ?
				      "unavailable" : "available";
		metadata_status = process->executable_flags &
					  MAPPLE_PSLIST_EXECUTABLE_FLAG_METADATA_UNAVAILABLE ?
					  "unavailable" : "available";
	} else {
		path_status = "not_applicable";
		metadata_status = "not_applicable";
	}
	if (printf("{\n        \"status\": \"%s\",\n        \"path_status\": \"%s\","
		   "\n        \"path\": ", status, path_status) < 0)
		return EIO;
	if (process->executable_path) {
		ret = mapple_pslist_print_json_bytes(process->executable_path,
						     process->executable_path_length);
		if (ret)
			return ret;
	} else if (fputs("null", stdout) == EOF) {
		return EIO;
	}
	if (printf(",\n        \"deleted\": %s,\n        \"metadata_status\": \"%s\","
		   "\n        \"device_major\": ",
		   process->executable_flags & MAPPLE_PSLIST_EXECUTABLE_FLAG_DELETED ?
			   "true" : "false",
		   metadata_status) < 0)
		return EIO;
	if (process->executable_status == MAPPLE_PSLIST_EXECUTABLE_AVAILABLE &&
	    !(process->executable_flags & MAPPLE_PSLIST_EXECUTABLE_FLAG_METADATA_UNAVAILABLE)) {
		if (printf("%u,\n        \"device_minor\": %u,"
			   "\n        \"inode\": %" PRIu64 ","
			   "\n        \"mode\": %u,"
			   "\n        \"size\": %" PRIu64 ","
			   "\n        \"mtime_ns\": %" PRId64 ","
			   "\n        \"ctime_ns\": %" PRId64 ","
			   "\n        \"filesystem_magic\": %" PRIu64 "\n      }",
			   process->executable_device_major, process->executable_device_minor,
			   (uint64_t)process->executable_inode, process->executable_mode,
			   (uint64_t)process->executable_size,
			   (int64_t)process->executable_mtime_ns,
			   (int64_t)process->executable_ctime_ns,
			   (uint64_t)process->executable_filesystem_magic) < 0)
			return EIO;
	} else if (fputs("null,\n        \"device_minor\": null,"
			 "\n        \"inode\": null,\n        \"mode\": null,"
			 "\n        \"size\": null,\n        \"mtime_ns\": null,"
			 "\n        \"ctime_ns\": null,"
			 "\n        \"filesystem_magic\": null\n      }",
			 stdout) == EOF) {
		return EIO;
	}
	return 0;
}

static int mapple_pslist_print_json_state(__u8 state)
{
	const char *name = mapple_pslist_state_name(state);

	if (!name)
		return EPROTO;
	return printf("\"%s\"", name) < 0 ? EIO : 0;
}

static int mapple_pslist_print_json_task_fields(const struct mapple_pslist_task *task)
{
	int ret;

	if (printf("\"pid\": %u, \"tgid\": %u, \"ppid\": %u, "
		   "\"start_boottime_ns\": %" PRIu64 ", \"state\": ",
		   task->pid, task->tgid, task->ppid,
		   (uint64_t)task->start_boottime_ns) < 0)
		return EIO;
	ret = mapple_pslist_print_json_state(task->state);
	if (ret)
		return ret;
	if (fputs(", \"comm\": ", stdout) == EOF)
		return EIO;
	return mapple_pslist_print_json_bytes((const unsigned char *)task->comm,
					      strnlen(task->comm, sizeof(task->comm)));
}

static int mapple_pslist_print_json_task(const struct mapple_pslist_task *task,
					  const char *indent)
{
	int ret;

	if (printf("%s{", indent) < 0)
		return EIO;
	ret = mapple_pslist_print_json_task_fields(task);
	if (ret)
		return ret;
	return putchar('}') == EOF ? EIO : 0;
}

static int mapple_pslist_print_json_process(const struct mapple_pslist_entry *entry,
					     const struct mapple_pslist_entry *threads,
					     size_t thread_count)
{
	const struct mapple_pslist_process *process = &entry->data.process;
	const char *command_line_status;
	size_t index;
	int ret;

	command_line_status = mapple_pslist_command_line_status_name(process->command_line_status);
	if (!command_line_status)
		return EPROTO;
	if (fputs("    {\n      ", stdout) == EOF)
		return EIO;
	ret = mapple_pslist_print_json_task_fields(&entry->task);
	if (ret)
		return ret;
	if (printf(",\n      \"argv_status\": \"%s\",\n      \"argv\": ",
		   command_line_status) < 0)
		return EIO;
	if (process->command_line_status == MAPPLE_PSLIST_COMMAND_LINE_AVAILABLE) {
		ret = mapple_pslist_print_json_argv(process->command_line,
						     process->command_line_length);
		if (ret)
			return ret;
	} else if (process->command_line_status == MAPPLE_PSLIST_COMMAND_LINE_EMPTY) {
		if (fputs("[]", stdout) == EOF)
			return EIO;
	} else if (fputs("null", stdout) == EOF) {
		return EIO;
	}
	if (fputs(",\n      \"executable\": ", stdout) == EOF)
		return EIO;
	ret = mapple_pslist_print_json_executable(process);
	if (ret)
		return ret;
	if (fputs(",\n      \"threads\": [", stdout) == EOF)
		return EIO;
	for (index = 0; index < thread_count; ++index) {
		if (fputs(index ? ",\n" : "\n", stdout) == EOF)
			return EIO;
		ret = mapple_pslist_print_json_task(&threads[index].task, "        ");
		if (ret)
			return ret;
	}
	if (thread_count && fputs("\n      ", stdout) == EOF)
		return EIO;
	return fputs("]\n    }", stdout) == EOF ? EIO : 0;
}

static int mapple_pslist_print_processes(const struct mapple_pslist_snapshot *snapshot)
{
	size_t group_end;
	size_t index = 0;
	size_t process_count = 0;
	int ret;

	if (fputs("{\n  \"processes\": [", stdout) == EOF)
		return EIO;
	while (index < snapshot->count &&
	       snapshot->entries[index].type != MAPPLE_PSLIST_RECORD_KTHREAD) {
		group_end = index + 1;
		while (group_end < snapshot->count &&
		       snapshot->entries[group_end].type != MAPPLE_PSLIST_RECORD_KTHREAD &&
		       snapshot->entries[group_end].task.tgid == snapshot->entries[index].task.tgid)
			++group_end;
		if (snapshot->entries[index].type == MAPPLE_PSLIST_RECORD_PROCESS) {
			if (fputs(process_count ? ",\n" : "\n", stdout) == EOF)
				return EIO;
			ret = mapple_pslist_print_json_process(
				&snapshot->entries[index], &snapshot->entries[index + 1],
				group_end - index - 1);
			if (ret)
				return ret;
			++process_count;
		}
		index = group_end;
	}
	if (process_count && fputs("\n  ", stdout) == EOF)
		return EIO;
	return fputs("],\n  \"unmatched_threads\": [", stdout) == EOF ? EIO : 0;
}

static int mapple_pslist_print_unmatched_threads(const struct mapple_pslist_snapshot *snapshot)
{
	size_t group_end;
	int first = 1;
	size_t index = 0;
	size_t thread_index;
	int ret;

	while (index < snapshot->count &&
	       snapshot->entries[index].type != MAPPLE_PSLIST_RECORD_KTHREAD) {
		group_end = index + 1;
		while (group_end < snapshot->count &&
		       snapshot->entries[group_end].type != MAPPLE_PSLIST_RECORD_KTHREAD &&
		       snapshot->entries[group_end].task.tgid == snapshot->entries[index].task.tgid)
			++group_end;
		if (snapshot->entries[index].type != MAPPLE_PSLIST_RECORD_PROCESS) {
			for (thread_index = index; thread_index < group_end; ++thread_index) {
				if (fputs(first ? "\n" : ",\n", stdout) == EOF)
					return EIO;
				ret = mapple_pslist_print_json_task(
					&snapshot->entries[thread_index].task, "    ");
				if (ret)
					return ret;
				first = 0;
			}
		}
		index = group_end;
	}
	if (!first && fputs("\n  ", stdout) == EOF)
		return EIO;
	return fputs("],\n  \"kthreads\": [", stdout) == EOF ? EIO : 0;
}

static int mapple_pslist_print_kthreads(const struct mapple_pslist_snapshot *snapshot)
{
	int first = 1;
	size_t index;
	int ret;

	for (index = 0; index < snapshot->count; ++index) {
		if (snapshot->entries[index].type != MAPPLE_PSLIST_RECORD_KTHREAD)
			continue;
		if (fputs(first ? "\n" : ",\n", stdout) == EOF)
			return EIO;
		ret = mapple_pslist_print_json_task(&snapshot->entries[index].task, "    ");
		if (ret)
			return ret;
		first = 0;
	}
	if (!first && fputs("\n  ", stdout) == EOF)
		return EIO;
	return fputs("]\n}\n", stdout) == EOF ? EIO : 0;
}

static int mapple_pslist_print_snapshot(const struct mapple_pslist_snapshot *snapshot)
{
	int ret;

	ret = mapple_pslist_print_processes(snapshot);
	if (!ret)
		ret = mapple_pslist_print_unmatched_threads(snapshot);
	if (!ret)
		ret = mapple_pslist_print_kthreads(snapshot);
	return ret;
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
	    ntohl(response.flags) != MAPPLE_HEADER_FLAG_STREAM ||
	    be64toh(response.payload_size))
		return EPROTO;
	ret = mapple_pslist_read_snapshot(session_fd, &snapshot);
	if (!ret)
		ret = mapple_pslist_sort_snapshot(&snapshot);
	if (!ret)
		ret = mapple_pslist_print_snapshot(&snapshot);
	mapple_pslist_free_snapshot(&snapshot);
	return ret;
}
