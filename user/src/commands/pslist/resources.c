#define _GNU_SOURCE

#include <commands/pslist/resources.h>

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <commands/pslist/json.h>

static __u16 mapple_pslist_resources_read_u16(const void *record, size_t offset)
{
	__u16 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohs(value);
}

static __u32 mapple_pslist_resources_read_u32(const void *record, size_t offset)
{
	__u32 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohl(value);
}

static __u64 mapple_pslist_resources_read_u64(const void *record, size_t offset)
{
	__u64 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return be64toh(value);
}

static int mapple_pslist_resources_status_valid(__u16 status)
{
	return status >= MAPPLE_PSLIST_DATA_AVAILABLE &&
	       status <= MAPPLE_PSLIST_DATA_UNSUPPORTED;
}

static int mapple_pslist_resources_memory_empty(
	const struct mapple_pslist_resources *resources)
{
	return !resources->memory_virtual_pages &&
	       !resources->memory_peak_virtual_pages && !resources->memory_locked_pages &&
	       !resources->memory_data_pages && !resources->memory_stack_pages &&
	       !resources->memory_executable_pages && !resources->memory_resident_pages &&
	       !resources->memory_peak_resident_pages &&
	       !resources->memory_anonymous_pages && !resources->memory_file_pages &&
	       !resources->memory_shared_pages && !resources->memory_swap_entries;
}

static int mapple_pslist_resources_io_empty(
	const struct mapple_pslist_resources *resources)
{
	return !resources->io_read_characters && !resources->io_write_characters &&
	       !resources->io_read_syscalls && !resources->io_write_syscalls &&
	       !resources->io_storage_read_bytes && !resources->io_storage_write_bytes &&
	       !resources->io_cancelled_write_bytes;
}

int mapple_pslist_decode_resources_record(const void *record, size_t size,
					  struct mapple_pslist_resources *resources)
{
	const struct mapple_pslist_resources_record *wire = record;
	__u64 object_id;

	if (size != sizeof(*wire) || resources->has_record)
		return EPROTO;
	object_id = mapple_pslist_resources_read_u64(
		wire, offsetof(struct mapple_pslist_resources_record, object_id));
	if (object_id != resources->object_id)
		return EPROTO;
#define RESOURCES_U16(field)                                                      \
	resources->field = mapple_pslist_resources_read_u16(                        \
		wire, offsetof(struct mapple_pslist_resources_record, field))
#define RESOURCES_U32(field)                                                      \
	resources->field = mapple_pslist_resources_read_u32(                        \
		wire, offsetof(struct mapple_pslist_resources_record, field))
#define RESOURCES_U64(field)                                                      \
	resources->field = mapple_pslist_resources_read_u64(                        \
		wire, offsetof(struct mapple_pslist_resources_record, field))
	RESOURCES_U16(memory_status);
	RESOURCES_U16(io_status);
	RESOURCES_U16(network_status);
	RESOURCES_U16(memory_reason);
	resources->memory_error = (__s32)mapple_pslist_resources_read_u32(
		wire, offsetof(struct mapple_pslist_resources_record, memory_error));
	resources->io_error = (__s32)mapple_pslist_resources_read_u32(
		wire, offsetof(struct mapple_pslist_resources_record, io_error));
	resources->network_error = (__s32)mapple_pslist_resources_read_u32(
		wire, offsetof(struct mapple_pslist_resources_record, network_error));
	RESOURCES_U32(current_cpu);
	RESOURCES_U32(scheduling_policy);
	resources->priority = (__s32)mapple_pslist_resources_read_u32(
		wire, offsetof(struct mapple_pslist_resources_record, priority));
	resources->nice = (__s32)mapple_pslist_resources_read_u32(
		wire, offsetof(struct mapple_pslist_resources_record, nice));
	RESOURCES_U32(realtime_priority);
	RESOURCES_U32(page_size_bytes);
	RESOURCES_U64(cpu_user_time_ns);
	RESOURCES_U64(cpu_system_time_ns);
	RESOURCES_U64(minor_faults);
	RESOURCES_U64(major_faults);
	RESOURCES_U64(voluntary_context_switches);
	RESOURCES_U64(involuntary_context_switches);
	RESOURCES_U64(io_read_characters);
	RESOURCES_U64(io_write_characters);
	RESOURCES_U64(io_read_syscalls);
	RESOURCES_U64(io_write_syscalls);
	RESOURCES_U64(io_storage_read_bytes);
	RESOURCES_U64(io_storage_write_bytes);
	RESOURCES_U64(io_cancelled_write_bytes);
	RESOURCES_U64(memory_virtual_pages);
	RESOURCES_U64(memory_peak_virtual_pages);
	RESOURCES_U64(memory_locked_pages);
	RESOURCES_U64(memory_data_pages);
	RESOURCES_U64(memory_stack_pages);
	RESOURCES_U64(memory_executable_pages);
	RESOURCES_U64(memory_resident_pages);
	RESOURCES_U64(memory_peak_resident_pages);
	RESOURCES_U64(memory_anonymous_pages);
	RESOURCES_U64(memory_file_pages);
	RESOURCES_U64(memory_shared_pages);
	RESOURCES_U64(memory_swap_entries);
#undef RESOURCES_U64
#undef RESOURCES_U32
#undef RESOURCES_U16
	if (!mapple_pslist_resources_status_valid(resources->memory_status) ||
	    !mapple_pslist_resources_status_valid(resources->io_status) ||
	    !mapple_pslist_resources_status_valid(resources->network_status) ||
	    resources->priority < 0 || resources->priority > 139 ||
	    resources->nice < -20 || resources->nice > 19 ||
	    resources->realtime_priority > 99 || !resources->page_size_bytes ||
	    (resources->page_size_bytes & (resources->page_size_bytes - 1)))
		return EPROTO;
	if (resources->memory_status == MAPPLE_PSLIST_DATA_AVAILABLE) {
		if (resources->memory_error ||
		    resources->memory_reason != MAPPLE_PSLIST_RESOURCES_MEMORY_REASON_NONE)
			return EPROTO;
	} else if (resources->memory_error ||
		   resources->memory_reason != MAPPLE_PSLIST_RESOURCES_MEMORY_REASON_NO_MM ||
		   !mapple_pslist_resources_memory_empty(resources)) {
		return EPROTO;
	}
	if (resources->io_status == MAPPLE_PSLIST_DATA_AVAILABLE) {
		if (resources->io_error)
			return EPROTO;
	} else if (resources->io_error || !mapple_pslist_resources_io_empty(resources)) {
		return EPROTO;
	}
	if (resources->network_status != MAPPLE_PSLIST_DATA_UNSUPPORTED ||
	    resources->network_error)
		return EPROTO;
	resources->has_record = 1;
	return 0;
}

static const char *mapple_pslist_resources_data_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_DATA_AVAILABLE: return "available";
	case MAPPLE_PSLIST_DATA_UNAVAILABLE: return "unavailable";
	case MAPPLE_PSLIST_DATA_TOO_LARGE: return "too_large";
	case MAPPLE_PSLIST_DATA_RACED: return "raced";
	case MAPPLE_PSLIST_DATA_UNSUPPORTED: return "unsupported";
	default: return NULL;
	}
}

static const char *mapple_pslist_resources_object_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_OBJECT_COMPLETE: return "complete";
	case MAPPLE_PSLIST_OBJECT_PARTIAL: return "partial";
	case MAPPLE_PSLIST_OBJECT_UNAVAILABLE: return "unavailable";
	case MAPPLE_PSLIST_OBJECT_RACED: return "raced";
	case MAPPLE_PSLIST_OBJECT_TOO_LARGE: return "too_large";
	default: return NULL;
	}
}

static const char *mapple_pslist_resources_memory_reason_name(__u16 reason)
{
	switch (reason) {
	case MAPPLE_PSLIST_RESOURCES_MEMORY_REASON_NONE: return NULL;
	case MAPPLE_PSLIST_RESOURCES_MEMORY_REASON_NO_MM:
		return "no_memory_descriptor";
	default: return NULL;
	}
}

static const char *mapple_pslist_scheduling_policy_name(__u32 policy)
{
	switch (policy) {
	case SCHED_OTHER: return "SCHED_OTHER";
	case SCHED_FIFO: return "SCHED_FIFO";
	case SCHED_RR: return "SCHED_RR";
	case SCHED_BATCH: return "SCHED_BATCH";
	case SCHED_IDLE: return "SCHED_IDLE";
	case SCHED_DEADLINE: return "SCHED_DEADLINE";
	default: return NULL;
	}
}

static int mapple_pslist_resources_print_error(__s32 error)
{
	if (!error)
		return fputs("null", stdout) == EOF ? EIO : 0;
	return printf("{\"code\": %d, \"message\": ", error) < 0 ||
	       mapple_pslist_print_json_bytes((const unsigned char *)strerror(error),
					      strlen(strerror(error))) ||
	       putchar('}') == EOF ? EIO : 0;
}

int mapple_pslist_print_json_resources(const struct mapple_pslist_resources *resources,
					int indent)
{
	const char *status = mapple_pslist_resources_object_status_name(resources->status);
	const char *memory_status = mapple_pslist_resources_data_status_name(
		resources->memory_status);
	const char *io_status = mapple_pslist_resources_data_status_name(
		resources->io_status);
	const char *network_status = mapple_pslist_resources_data_status_name(
		resources->network_status);
	const char *policy = mapple_pslist_scheduling_policy_name(
		resources->scheduling_policy);
	const char *memory_reason = mapple_pslist_resources_memory_reason_name(
		resources->memory_reason);
	int ret;

	if (!status || !memory_status || !io_status || !network_status || !policy)
		return EPROTO;
	if (printf("{\"collection_status\": \"%s\",\n", status) < 0)
		return EIO;
	ret = mapple_pslist_print_json_indent(indent + 2);
	if (ret || printf("\"cpu\": {\"user_time_ns\": \"%" PRIu64 "\""
			  ", \"system_time_ns\": \"%" PRIu64 "\""
			  ", \"faults\": {\"minor\": \"%" PRIu64 "\""
			  ", \"major\": \"%" PRIu64 "\""
			  "}, \"context_switches\": {\"voluntary\": \"%" PRIu64 "\""
			  ", \"involuntary\": \"%" PRIu64 "\""
			  "}, \"scheduler\": {\"current_cpu\": %u, "
			  "\"policy\": \"%s\", \"priority\": %d, \"nice\": %d, "
			  "\"realtime_priority\": %u}},\n",
			  (uint64_t)resources->cpu_user_time_ns,
			  (uint64_t)resources->cpu_system_time_ns,
			  (uint64_t)resources->minor_faults,
			  (uint64_t)resources->major_faults,
			  (uint64_t)resources->voluntary_context_switches,
			  (uint64_t)resources->involuntary_context_switches,
			  resources->current_cpu, policy, resources->priority,
			  resources->nice, resources->realtime_priority) < 0)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_json_indent(indent + 2);
	if (ret || printf("\"memory\": {\"collection_status\": \"%s\", "
			  "\"error\": ", memory_status) < 0)
		return ret ? ret : EIO;
	ret = mapple_pslist_resources_print_error(resources->memory_error);
	if (ret || printf(", \"reason\": %s%s%s, \"page_size_bytes\": %u, \"pages\": {"
			  "\"virtual\": \"%" PRIu64 "\", \"peak_virtual\": \"%" PRIu64 "\""
			  ", \"locked\": \"%" PRIu64 "\", \"data\": \"%" PRIu64 "\""
			  ", \"stack\": \"%" PRIu64 "\", \"executable\": \"%" PRIu64 "\""
			  ", \"resident\": \"%" PRIu64 "\", \"peak_resident\": \"%" PRIu64 "\""
			  ", \"anonymous\": \"%" PRIu64 "\", \"file\": \"%" PRIu64 "\""
			  ", \"shared\": \"%" PRIu64 "\", \"swap_entries\": \"%" PRIu64 "\""
			  "}},\n", memory_reason ? "\"" : "",
			  memory_reason ? memory_reason : "null",
			  memory_reason ? "\"" : "", resources->page_size_bytes,
			  (uint64_t)resources->memory_virtual_pages,
			  (uint64_t)resources->memory_peak_virtual_pages,
			  (uint64_t)resources->memory_locked_pages,
			  (uint64_t)resources->memory_data_pages,
			  (uint64_t)resources->memory_stack_pages,
			  (uint64_t)resources->memory_executable_pages,
			  (uint64_t)resources->memory_resident_pages,
			  (uint64_t)resources->memory_peak_resident_pages,
			  (uint64_t)resources->memory_anonymous_pages,
			  (uint64_t)resources->memory_file_pages,
			  (uint64_t)resources->memory_shared_pages,
			  (uint64_t)resources->memory_swap_entries) < 0)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_json_indent(indent + 2);
	if (ret || printf("\"io\": {\"collection_status\": \"%s\", \"error\": ",
			  io_status) < 0)
		return ret ? ret : EIO;
	ret = mapple_pslist_resources_print_error(resources->io_error);
	if (ret || printf(", \"characters\": {\"read\": \"%" PRIu64 "\""
			  ", \"written\": \"%" PRIu64 "\""
			  "}, \"syscalls\": {\"read\": \"%" PRIu64 "\""
			  ", \"write\": \"%" PRIu64 "\""
			  "}, \"storage_bytes\": {\"read\": \"%" PRIu64 "\""
			  ", \"written\": \"%" PRIu64 "\""
			  ", \"cancelled_write\": \"%" PRIu64 "\"}},\n",
			  (uint64_t)resources->io_read_characters,
			  (uint64_t)resources->io_write_characters,
			  (uint64_t)resources->io_read_syscalls,
			  (uint64_t)resources->io_write_syscalls,
			  (uint64_t)resources->io_storage_read_bytes,
			  (uint64_t)resources->io_storage_write_bytes,
			  (uint64_t)resources->io_cancelled_write_bytes) < 0)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_json_indent(indent + 2);
	if (ret || printf("\"network\": {\"collection_status\": \"%s\", "
			  "\"reason\": \"fresh_snapshot_has_no_per_task_network_accounting\", "
			  "\"error\": ", network_status) < 0)
		return ret ? ret : EIO;
	ret = mapple_pslist_resources_print_error(resources->network_error);
	if (ret || fputs("}\n", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_json_indent(indent);
	return ret || putchar('}') == EOF ? (ret ? ret : EIO) : 0;
}
