#include <commands/pslist/environment.h>

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <commands/pslist/json.h>

static __u16 mapple_pslist_environment_read_u16(const void *record, size_t offset)
{
	__u16 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohs(value);
}

static __u32 mapple_pslist_environment_read_u32(const void *record, size_t offset)
{
	__u32 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohl(value);
}

static __u64 mapple_pslist_environment_read_u64(const void *record, size_t offset)
{
	__u64 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return be64toh(value);
}

static int mapple_pslist_environment_status_valid(__u16 status)
{
	return status >= MAPPLE_PSLIST_ENVIRONMENT_AVAILABLE &&
	       status <= MAPPLE_PSLIST_ENVIRONMENT_RACED;
}

static __u32 mapple_pslist_environment_count_entries(const unsigned char *bytes,
						       __u32 length)
{
	__u32 count = 0;
	__u32 index;

	for (index = 0; index < length; ++index)
		if (!bytes[index])
			++count;
	if (length && bytes[length - 1])
		++count;
	return count;
}

int mapple_pslist_decode_environment_record(
	const void *record, size_t size, struct mapple_pslist_environment *environment)
{
	const struct mapple_pslist_environment_record *wire = record;
	const unsigned char *bytes = (const unsigned char *)record + sizeof(*wire);
	__u64 object_id;

	if (size < sizeof(*wire) || environment->has_record)
		return EPROTO;
	object_id = mapple_pslist_environment_read_u64(
		wire, offsetof(struct mapple_pslist_environment_record, object_id));
	if (object_id != environment->object_id)
		return EPROTO;
	environment->data_status = mapple_pslist_environment_read_u16(
		wire, offsetof(struct mapple_pslist_environment_record, status));
	environment->flags = mapple_pslist_environment_read_u16(
		wire, offsetof(struct mapple_pslist_environment_record, flags));
	environment->error = (__s32)mapple_pslist_environment_read_u32(
		wire, offsetof(struct mapple_pslist_environment_record, error));
	environment->byte_length = mapple_pslist_environment_read_u32(
		wire, offsetof(struct mapple_pslist_environment_record, byte_length));
	environment->entry_count = mapple_pslist_environment_read_u32(
		wire, offsetof(struct mapple_pslist_environment_record, entry_count));
	if (wire->reserved ||
	    !mapple_pslist_environment_status_valid(environment->data_status) ||
	    (environment->flags & ~MAPPLE_PSLIST_ENVIRONMENT_KNOWN_FLAGS) ||
	    environment->byte_length > MAPPLE_PSLIST_ENVIRONMENT_MAX_SIZE ||
	    sizeof(*wire) + (__u64)environment->byte_length != size)
		return EPROTO;
	if (environment->data_status == MAPPLE_PSLIST_ENVIRONMENT_AVAILABLE) {
		if (!environment->byte_length || environment->error ||
		    !!(environment->flags & MAPPLE_PSLIST_ENVIRONMENT_FLAG_TERMINATED) !=
			    !bytes[environment->byte_length - 1] ||
		    mapple_pslist_environment_count_entries(
			    bytes, environment->byte_length) != environment->entry_count)
			return EPROTO;
	} else if (environment->data_status == MAPPLE_PSLIST_ENVIRONMENT_EMPTY) {
		if (environment->byte_length || environment->entry_count ||
		    environment->error ||
		    environment->flags != MAPPLE_PSLIST_ENVIRONMENT_FLAG_TERMINATED)
			return EPROTO;
	} else if (environment->byte_length || environment->entry_count ||
		   environment->flags || environment->error <= 0) {
		return EPROTO;
	}
	if (environment->byte_length) {
		environment->bytes = malloc(environment->byte_length);
		if (!environment->bytes)
			return ENOMEM;
		memcpy(environment->bytes, bytes, environment->byte_length);
	}
	environment->has_record = 1;
	return 0;
}

void mapple_pslist_free_environment(struct mapple_pslist_environment *environment)
{
	free(environment->bytes);
	memset(environment, 0, sizeof(*environment));
}

static const char *mapple_pslist_environment_data_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_ENVIRONMENT_AVAILABLE: return "available";
	case MAPPLE_PSLIST_ENVIRONMENT_EMPTY: return "empty";
	case MAPPLE_PSLIST_ENVIRONMENT_UNAVAILABLE: return "unavailable";
	case MAPPLE_PSLIST_ENVIRONMENT_TOO_LARGE: return "too_large";
	case MAPPLE_PSLIST_ENVIRONMENT_RACED: return "raced";
	default: return NULL;
	}
}

static const char *mapple_pslist_environment_object_status_name(__u16 status)
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

static int mapple_pslist_environment_print_error(__s32 error)
{
	if (!error)
		return fputs("null", stdout) == EOF ? EIO : 0;
	return printf("{\"code\": %d, \"message\": ", error) < 0 ||
	       mapple_pslist_print_json_bytes((const unsigned char *)strerror(error),
					      strlen(strerror(error))) ||
	       putchar('}') == EOF ? EIO : 0;
}

static int mapple_pslist_environment_print_entry(const unsigned char *entry,
						   size_t length)
{
	const unsigned char *separator = memchr(entry, '=', length);
	int ret;

	if (!separator) {
		if (fputs("{\"parse_status\": \"missing_equals\", \"raw\": ", stdout) == EOF)
			return EIO;
		ret = mapple_pslist_print_json_bytes(entry, length);
		return ret || putchar('}') == EOF ? (ret ? ret : EIO) : 0;
	}
	if (fputs("{\"name\": ", stdout) == EOF)
		return EIO;
	ret = mapple_pslist_print_json_bytes(entry, separator - entry);
	if (ret || fputs(", \"value\": ", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_json_bytes(separator + 1,
					     length - (separator - entry) - 1);
	return ret || putchar('}') == EOF ? (ret ? ret : EIO) : 0;
}

int mapple_pslist_print_json_environment(
	const struct mapple_pslist_environment *environment, int indent)
{
	const char *status = mapple_pslist_environment_object_status_name(
		environment->status);
	const char *data_status = mapple_pslist_environment_data_status_name(
		environment->data_status);
	__u32 entry_index;
	size_t start = 0;
	size_t end;
	int ret;

	if (!status || !data_status ||
	    printf("{\"collection_status\": \"%s\",\n", status) < 0)
		return EPROTO;
	ret = mapple_pslist_print_json_indent(indent + 2);
	if (ret || printf("\"block\": {\"collection_status\": \"%s\", "
			  "\"byte_length\": %u, \"entry_count\": %u, "
			  "\"terminated\": %s, \"error\": ", data_status,
			  environment->byte_length, environment->entry_count,
			  environment->flags & MAPPLE_PSLIST_ENVIRONMENT_FLAG_TERMINATED ?
				  "true" : "false") < 0)
		return ret ? ret : EIO;
	ret = mapple_pslist_environment_print_error(environment->error);
	if (ret || fputs(", \"entries\": [", stdout) == EOF)
		return ret ? ret : EIO;
	for (entry_index = 0; entry_index < environment->entry_count; ++entry_index) {
		end = start;
		while (end < environment->byte_length && environment->bytes[end])
			++end;
		if (fputs(entry_index ? ",\n" : "\n", stdout) == EOF)
			return EIO;
		ret = mapple_pslist_print_json_indent(indent + 6);
		if (ret)
			return ret;
		ret = mapple_pslist_environment_print_entry(
			environment->bytes + start, end - start);
		if (ret)
			return ret;
		start = end < environment->byte_length ? end + 1 : end;
	}
	if (environment->entry_count) {
		if (putchar('\n') == EOF)
			return EIO;
		ret = mapple_pslist_print_json_indent(indent + 4);
		if (ret)
			return ret;
	}
	if (fputs("]}\n", stdout) == EOF)
		return EIO;
	ret = mapple_pslist_print_json_indent(indent);
	return ret || putchar('}') == EOF ? (ret ? ret : EIO) : 0;
}
