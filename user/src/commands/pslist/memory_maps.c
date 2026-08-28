#include <commands/pslist/memory_maps.h>

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

static __u16 mapple_pslist_memory_maps_read_u16(const void *record, size_t offset)
{
	__u16 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohs(value);
}

static __u32 mapple_pslist_memory_maps_read_u32(const void *record, size_t offset)
{
	__u32 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohl(value);
}

static __u64 mapple_pslist_memory_maps_read_u64(const void *record, size_t offset)
{
	__u64 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return be64toh(value);
}

static int mapple_pslist_memory_maps_data_status_valid(__u16 status)
{
	return status >= MAPPLE_PSLIST_DATA_AVAILABLE &&
	       status <= MAPPLE_PSLIST_DATA_UNSUPPORTED;
}

int mapple_pslist_decode_memory_maps_metadata(
	const void *record, size_t size, struct mapple_pslist_memory_maps *memory_maps)
{
	const struct mapple_pslist_memory_maps_metadata_record *wire = record;
	__u64 object_id;

	if (size != sizeof(*wire) || memory_maps->has_metadata)
		return EPROTO;
	object_id = mapple_pslist_memory_maps_read_u64(
		wire, offsetof(struct mapple_pslist_memory_maps_metadata_record, object_id));
	if (object_id != memory_maps->object_id)
		return EPROTO;
	memory_maps->page_size_bytes = mapple_pslist_memory_maps_read_u32(
		wire, offsetof(struct mapple_pslist_memory_maps_metadata_record,
			       page_size_bytes));
	memory_maps->mapping_count = mapple_pslist_memory_maps_read_u32(
		wire, offsetof(struct mapple_pslist_memory_maps_metadata_record,
			       mapping_count));
	memory_maps->mapping_limit = mapple_pslist_memory_maps_read_u32(
		wire, offsetof(struct mapple_pslist_memory_maps_metadata_record,
			       mapping_limit));
	if (wire->reserved || !memory_maps->page_size_bytes ||
	    (memory_maps->page_size_bytes & (memory_maps->page_size_bytes - 1)) ||
	    memory_maps->mapping_limit != MAPPLE_PSLIST_MEMORY_MAP_MAX_COUNT)
		return EPROTO;
	memory_maps->has_metadata = 1;
	return 0;
}

static int mapple_pslist_memory_maps_grow(struct mapple_pslist_memory_maps *memory_maps)
{
	struct mapple_pslist_memory_map *mappings;
	size_t capacity;

	if (memory_maps->count < memory_maps->capacity)
		return 0;
	capacity = memory_maps->capacity ? memory_maps->capacity * 2 : 64;
	if (capacity < memory_maps->capacity ||
	    capacity > MAPPLE_PSLIST_MEMORY_MAP_MAX_COUNT)
		return EOVERFLOW;
	mappings = realloc(memory_maps->mappings, capacity * sizeof(*mappings));
	if (!mappings)
		return ENOMEM;
	memory_maps->mappings = mappings;
	memory_maps->capacity = capacity;
	return 0;
}

static int mapple_pslist_memory_map_copy_bytes(unsigned char **destination,
						const unsigned char **source,
						__u32 length)
{
	if (!length)
		return 0;
	*destination = malloc(length);
	if (!*destination)
		return ENOMEM;
	memcpy(*destination, *source, length);
	*source += length;
	return 0;
}

static int mapple_pslist_memory_map_name_valid(
	const struct mapple_pslist_memory_map *mapping)
{
	switch (mapping->name_kind) {
	case MAPPLE_PSLIST_MEMORY_MAP_NAME_NONE:
	case MAPPLE_PSLIST_MEMORY_MAP_NAME_HEAP:
	case MAPPLE_PSLIST_MEMORY_MAP_NAME_STACK:
		return !mapping->name_status && !mapping->name_error &&
		       !mapping->name_length;
	case MAPPLE_PSLIST_MEMORY_MAP_NAME_ANONYMOUS:
	case MAPPLE_PSLIST_MEMORY_MAP_NAME_SHARED_ANONYMOUS:
	case MAPPLE_PSLIST_MEMORY_MAP_NAME_SPECIAL:
		if (!mapple_pslist_memory_maps_data_status_valid(mapping->name_status))
			return 0;
		if (mapping->name_status == MAPPLE_PSLIST_DATA_AVAILABLE)
			return mapping->name_length && !mapping->name_error;
		return !mapping->name_length && mapping->name_error > 0;
	default:
		return 0;
	}
}

static int mapple_pslist_memory_map_file_valid(
	const struct mapple_pslist_memory_map *mapping)
{
	if (mapping->mapping_type != MAPPLE_PSLIST_MEMORY_MAPPING_FILE)
		return !mapping->path_status && !mapping->file_metadata_status &&
		       !mapping->path_error && !mapping->file_metadata_error &&
		       !mapping->file_flags && !mapping->path_length &&
		       !mapping->file_device_major && !mapping->file_device_minor &&
		       !mapping->file_mode && !mapping->file_inode &&
		       !mapping->file_size &&
		       !mapping->file_mtime_ns && !mapping->file_ctime_ns &&
		       !mapping->file_filesystem_magic;
	if (!mapple_pslist_memory_maps_data_status_valid(mapping->path_status) ||
	    !mapple_pslist_memory_maps_data_status_valid(
		    mapping->file_metadata_status) ||
	    (mapping->file_flags & ~MAPPLE_PSLIST_MEMORY_MAP_KNOWN_FILE_FLAGS))
		return 0;
	if (mapping->path_status == MAPPLE_PSLIST_DATA_AVAILABLE) {
		if (!mapping->path_length || mapping->path_error)
			return 0;
	} else if (mapping->path_length || mapping->path_error <= 0) {
		return 0;
	}
	if (mapping->file_metadata_status == MAPPLE_PSLIST_DATA_AVAILABLE)
		return !mapping->file_metadata_error;
	return mapping->file_metadata_error > 0 && !mapping->file_device_major &&
	       !mapping->file_device_minor && !mapping->file_mode &&
	       !mapping->file_inode && !mapping->file_size &&
	       !mapping->file_mtime_ns && !mapping->file_ctime_ns &&
	       !mapping->file_filesystem_magic;
}

int mapple_pslist_decode_memory_map(
	const void *record, size_t size, struct mapple_pslist_memory_maps *memory_maps)
{
	const struct mapple_pslist_memory_map_record *wire = record;
	const unsigned char *bytes = (const unsigned char *)record + sizeof(*wire);
	struct mapple_pslist_memory_map *mapping;
	__u64 object_id;
	__u64 variable_size;
	int ret;

	if (size < sizeof(*wire) || !memory_maps->has_metadata ||
	    memory_maps->mapping_count > memory_maps->mapping_limit ||
	    memory_maps->count >= memory_maps->mapping_count)
		return EPROTO;
	object_id = mapple_pslist_memory_maps_read_u64(
		wire, offsetof(struct mapple_pslist_memory_map_record, object_id));
	if (object_id != memory_maps->object_id)
		return EPROTO;
	ret = mapple_pslist_memory_maps_grow(memory_maps);
	if (ret)
		return ret;
	mapping = &memory_maps->mappings[memory_maps->count];
	memset(mapping, 0, sizeof(*mapping));
#define MAP_U16(field)                                                            \
	mapping->field = mapple_pslist_memory_maps_read_u16(                         \
		wire, offsetof(struct mapple_pslist_memory_map_record, field))
#define MAP_U32(field)                                                            \
	mapping->field = mapple_pslist_memory_maps_read_u32(                         \
		wire, offsetof(struct mapple_pslist_memory_map_record, field))
#define MAP_U64(field)                                                            \
	mapping->field = mapple_pslist_memory_maps_read_u64(                         \
		wire, offsetof(struct mapple_pslist_memory_map_record, field))
	MAP_U32(mapping_index);
	MAP_U16(mapping_type);
	MAP_U16(name_kind);
	MAP_U16(name_status);
	MAP_U16(path_status);
	MAP_U16(file_metadata_status);
	mapping->name_error = (__s32)mapple_pslist_memory_maps_read_u32(
		wire, offsetof(struct mapple_pslist_memory_map_record, name_error));
	mapping->path_error = (__s32)mapple_pslist_memory_maps_read_u32(
		wire, offsetof(struct mapple_pslist_memory_map_record, path_error));
	mapping->file_metadata_error = (__s32)mapple_pslist_memory_maps_read_u32(
		wire, offsetof(struct mapple_pslist_memory_map_record,
			       file_metadata_error));
	MAP_U32(file_flags);
	MAP_U32(name_length);
	MAP_U32(path_length);
	MAP_U32(file_device_major);
	MAP_U32(file_device_minor);
	MAP_U32(file_mode);
	MAP_U32(protection_key);
	MAP_U64(start_address);
	MAP_U64(end_address);
	MAP_U64(vm_flags);
	MAP_U64(page_offset);
	MAP_U64(mapping_page_size);
	MAP_U64(file_inode);
	MAP_U64(file_size);
	mapping->file_mtime_ns = (__s64)mapple_pslist_memory_maps_read_u64(
		wire, offsetof(struct mapple_pslist_memory_map_record, file_mtime_ns));
	mapping->file_ctime_ns = (__s64)mapple_pslist_memory_maps_read_u64(
		wire, offsetof(struct mapple_pslist_memory_map_record, file_ctime_ns));
	MAP_U64(file_filesystem_magic);
#undef MAP_U64
#undef MAP_U32
#undef MAP_U16
	variable_size = (__u64)mapping->name_length + mapping->path_length;
	if (wire->reserved || sizeof(*wire) + variable_size != size ||
	    mapping->mapping_index != memory_maps->count + 1 ||
	    mapping->mapping_type < MAPPLE_PSLIST_MEMORY_MAPPING_ANONYMOUS ||
	    mapping->mapping_type > MAPPLE_PSLIST_MEMORY_MAPPING_SPECIAL ||
	    !mapping->mapping_page_size ||
	    (mapping->mapping_page_size & (mapping->mapping_page_size - 1)) ||
	    mapping->start_address >= mapping->end_address ||
	    mapping->name_length > MAPPLE_PSLIST_MEMORY_MAP_NAME_MAX_SIZE ||
	    mapping->path_length > MAPPLE_PSLIST_MEMORY_MAP_PATH_MAX_SIZE ||
	    !mapple_pslist_memory_map_name_valid(mapping) ||
	    !mapple_pslist_memory_map_file_valid(mapping))
		return EPROTO;
	if (mapping->mapping_type == MAPPLE_PSLIST_MEMORY_MAPPING_FILE &&
	    mapping->name_kind != MAPPLE_PSLIST_MEMORY_MAP_NAME_NONE &&
	    mapping->name_kind != MAPPLE_PSLIST_MEMORY_MAP_NAME_SHARED_ANONYMOUS)
		return EPROTO;
	bytes = (const unsigned char *)record + sizeof(*wire);
	ret = mapple_pslist_memory_map_copy_bytes(&mapping->name, &bytes,
						  mapping->name_length);
	if (!ret)
		ret = mapple_pslist_memory_map_copy_bytes(&mapping->path, &bytes,
						       mapping->path_length);
	if (ret) {
		free(mapping->name);
		free(mapping->path);
		memset(mapping, 0, sizeof(*mapping));
		return ret;
	}
	++memory_maps->count;
	return 0;
}

void mapple_pslist_free_memory_maps(struct mapple_pslist_memory_maps *memory_maps)
{
	size_t index;

	for (index = 0; index < memory_maps->count; ++index) {
		free(memory_maps->mappings[index].name);
		free(memory_maps->mappings[index].path);
	}
	free(memory_maps->mappings);
	memset(memory_maps, 0, sizeof(*memory_maps));
}

static const char *mapple_pslist_memory_maps_object_status_name(__u16 status)
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

static const char *mapple_pslist_memory_maps_data_status_name(__u16 status)
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

static const char *mapple_pslist_memory_mapping_type_name(__u16 type)
{
	switch (type) {
	case MAPPLE_PSLIST_MEMORY_MAPPING_ANONYMOUS: return "anonymous";
	case MAPPLE_PSLIST_MEMORY_MAPPING_FILE: return "file";
	case MAPPLE_PSLIST_MEMORY_MAPPING_SPECIAL: return "special";
	default: return NULL;
	}
}

static const char *mapple_pslist_memory_map_name_kind_name(__u16 kind)
{
	switch (kind) {
	case MAPPLE_PSLIST_MEMORY_MAP_NAME_NONE: return "none";
	case MAPPLE_PSLIST_MEMORY_MAP_NAME_HEAP: return "heap";
	case MAPPLE_PSLIST_MEMORY_MAP_NAME_STACK: return "stack";
	case MAPPLE_PSLIST_MEMORY_MAP_NAME_ANONYMOUS: return "anonymous";
	case MAPPLE_PSLIST_MEMORY_MAP_NAME_SHARED_ANONYMOUS:
		return "shared_anonymous";
	case MAPPLE_PSLIST_MEMORY_MAP_NAME_SPECIAL: return "special";
	default: return NULL;
	}
}

static int mapple_pslist_memory_maps_print_error(__s32 error)
{
	if (!error)
		return fputs("null", stdout) == EOF ? EIO : 0;
	return printf("{\"code\": %d, \"message\": ", error) < 0 ||
	       mapple_pslist_print_json_bytes((const unsigned char *)strerror(error),
					      strlen(strerror(error))) ||
	       putchar('}') == EOF ? EIO : 0;
}

static int mapple_pslist_print_vm_flags(__u64 flags)
{
	static const struct {
		__u64 mask;
		const char *name;
	} names[] = {
		{ MAPPLE_PSLIST_VM_READ, "read" },
		{ MAPPLE_PSLIST_VM_WRITE, "write" },
		{ MAPPLE_PSLIST_VM_EXEC, "execute" },
		{ MAPPLE_PSLIST_VM_SHARED, "shared" },
		{ MAPPLE_PSLIST_VM_MAYREAD, "may_read" },
		{ MAPPLE_PSLIST_VM_MAYWRITE, "may_write" },
		{ MAPPLE_PSLIST_VM_MAYEXEC, "may_execute" },
		{ MAPPLE_PSLIST_VM_MAYSHARE, "may_share" },
		{ MAPPLE_PSLIST_VM_GROWSDOWN, "grows_down" },
		{ MAPPLE_PSLIST_VM_UFFD_MISSING, "userfaultfd_missing" },
		{ MAPPLE_PSLIST_VM_PFNMAP, "pfn_map" },
		{ MAPPLE_PSLIST_VM_UFFD_WP, "userfaultfd_write_protect" },
		{ MAPPLE_PSLIST_VM_LOCKED, "locked" },
		{ MAPPLE_PSLIST_VM_IO, "io" },
		{ MAPPLE_PSLIST_VM_SEQ_READ, "sequential_read" },
		{ MAPPLE_PSLIST_VM_RAND_READ, "random_read" },
		{ MAPPLE_PSLIST_VM_DONTCOPY, "do_not_copy" },
		{ MAPPLE_PSLIST_VM_DONTEXPAND, "do_not_expand" },
		{ MAPPLE_PSLIST_VM_LOCKONFAULT, "lock_on_fault" },
		{ MAPPLE_PSLIST_VM_ACCOUNT, "accounted" },
		{ MAPPLE_PSLIST_VM_NORESERVE, "no_reserve" },
		{ MAPPLE_PSLIST_VM_HUGETLB, "hugetlb" },
		{ MAPPLE_PSLIST_VM_SYNC, "synchronous" },
		{ MAPPLE_PSLIST_VM_WIPEONFORK, "wipe_on_fork" },
		{ MAPPLE_PSLIST_VM_DONTDUMP, "do_not_dump" },
		{ MAPPLE_PSLIST_VM_SOFTDIRTY, "soft_dirty" },
		{ MAPPLE_PSLIST_VM_MIXEDMAP, "mixed_map" },
		{ MAPPLE_PSLIST_VM_HUGEPAGE, "hugepage_advice" },
		{ MAPPLE_PSLIST_VM_NOHUGEPAGE, "no_hugepage_advice" },
		{ MAPPLE_PSLIST_VM_MERGEABLE, "mergeable" },
	};
	size_t index;
	int first = 1;

	if (printf("{\"mask\": \"0x%016" PRIx64 "\", \"names\": [",
		   (uint64_t)flags) < 0)
		return EIO;
	for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
		if (!(flags & names[index].mask))
			continue;
		if (printf("%s\"%s\"", first ? "" : ", ", names[index].name) < 0)
			return EIO;
		first = 0;
	}
	return printf("], \"unknown_mask\": \"0x%016" PRIx64 "\"}",
		      (uint64_t)(flags & ~MAPPLE_PSLIST_VM_KNOWN_FLAGS)) < 0 ? EIO : 0;
}

static int mapple_pslist_print_mapping_name(
	const struct mapple_pslist_memory_map *mapping)
{
	const char *kind = mapple_pslist_memory_map_name_kind_name(mapping->name_kind);
	const char *status;
	const char *derived = NULL;
	int ret;

	if (!kind)
		return EPROTO;
	if (mapping->name_kind == MAPPLE_PSLIST_MEMORY_MAP_NAME_HEAP)
		derived = "[heap]";
	else if (mapping->name_kind == MAPPLE_PSLIST_MEMORY_MAP_NAME_STACK)
		derived = "[stack]";
	if (printf("{\"kind\": \"%s\", \"collection_status\": ", kind) < 0)
		return EIO;
	if (!mapping->name_status) {
		if (fputs("null, \"value\": ", stdout) == EOF)
			return EIO;
	} else {
		status = mapple_pslist_memory_maps_data_status_name(mapping->name_status);
		if (!status || printf("\"%s\", \"value\": ", status) < 0)
			return EPROTO;
	}
	if (mapping->name) {
		ret = mapple_pslist_print_json_bytes(mapping->name, mapping->name_length);
	} else if (derived) {
		ret = mapple_pslist_print_json_bytes((const unsigned char *)derived,
						     strlen(derived));
	} else {
		ret = fputs("null", stdout) == EOF ? EIO : 0;
	}
	if (ret || fputs(", \"error\": ", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_memory_maps_print_error(mapping->name_error);
	return ret || putchar('}') == EOF ? (ret ? ret : EIO) : 0;
}

static int mapple_pslist_print_mapped_file(
	const struct mapple_pslist_memory_map *mapping)
{
	const char *path_status = mapple_pslist_memory_maps_data_status_name(
		mapping->path_status);
	const char *metadata_status = mapple_pslist_memory_maps_data_status_name(
		mapping->file_metadata_status);
	int ret;

	if (mapping->mapping_type != MAPPLE_PSLIST_MEMORY_MAPPING_FILE)
		return fputs("null", stdout) == EOF ? EIO : 0;
	if (!path_status || !metadata_status ||
	    printf("{\"deleted\": %s, \"path\": {\"collection_status\": \"%s\", "
		   "\"value\": ",
		   mapping->file_flags & MAPPLE_PSLIST_MEMORY_MAP_FILE_DELETED ?
			   "true" : "false", path_status) < 0)
		return EPROTO;
	ret = mapping->path ? mapple_pslist_print_json_bytes(mapping->path,
						     mapping->path_length) :
		     (fputs("null", stdout) == EOF ? EIO : 0);
	if (ret || fputs(", \"error\": ", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_memory_maps_print_error(mapping->path_error);
	if (ret || printf("}, \"identity\": {\"collection_status\": \"%s\", "
			  "\"error\": ", metadata_status) < 0)
		return ret ? ret : EIO;
	ret = mapple_pslist_memory_maps_print_error(mapping->file_metadata_error);
	if (ret)
		return ret;
	if (mapping->file_metadata_status == MAPPLE_PSLIST_DATA_AVAILABLE) {
		if (printf(", \"device\": {\"major\": %u, \"minor\": %u}, "
			   "\"inode\": %" PRIu64 ", \"mode\": %u, \"size\": %" PRIu64
			   ", \"mtime_ns\": %" PRId64 ", \"ctime_ns\": %" PRId64
			   ", \"filesystem_magic\": \"0x%016" PRIx64 "\"",
			   mapping->file_device_major, mapping->file_device_minor,
			   (uint64_t)mapping->file_inode,
			   mapping->file_mode,
			   (uint64_t)mapping->file_size,
			   (int64_t)mapping->file_mtime_ns,
			   (int64_t)mapping->file_ctime_ns,
			   (uint64_t)mapping->file_filesystem_magic) < 0)
			return EIO;
	}
	return fputs("}}", stdout) == EOF ? EIO : 0;
}

static int mapple_pslist_print_memory_mapping(
	const struct mapple_pslist_memory_map *mapping, __u32 page_size, int indent)
{
	const char *type = mapple_pslist_memory_mapping_type_name(mapping->mapping_type);
	__u64 byte_offset = 0;
	int offset_overflow;
	int ret;

	if (!type)
		return EPROTO;
	offset_overflow = mapping->page_offset > UINT64_MAX / page_size;
	if (!offset_overflow)
		byte_offset = mapping->page_offset * page_size;
	ret = mapple_pslist_print_json_indent(indent);
	if (ret || printf("{\"index\": %u, \"type\": \"%s\", "
			  "\"address\": {\"start\": \"0x%016" PRIx64
			  "\", \"end\": \"0x%016" PRIx64
			  "\", \"size_bytes\": %" PRIu64 "}, "
			  "\"permissions\": {\"read\": %s, \"write\": %s, "
			  "\"execute\": %s, \"sharing\": \"%s\"}, \"vm_flags\": ",
			  mapping->mapping_index, type,
			  (uint64_t)mapping->start_address,
			  (uint64_t)mapping->end_address,
			  (uint64_t)(mapping->end_address - mapping->start_address),
			  mapping->vm_flags & MAPPLE_PSLIST_VM_READ ? "true" : "false",
			  mapping->vm_flags & MAPPLE_PSLIST_VM_WRITE ? "true" : "false",
			  mapping->vm_flags & MAPPLE_PSLIST_VM_EXEC ? "true" : "false",
			  mapping->vm_flags & MAPPLE_PSLIST_VM_SHARED ? "shared" :
								 "private") < 0)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_vm_flags(mapping->vm_flags);
	if (ret || printf(", \"page_offset\": %" PRIu64 ", \"byte_offset\": ",
			  (uint64_t)mapping->page_offset) < 0)
		return ret ? ret : EIO;
	if (offset_overflow) {
		if (fputs("null", stdout) == EOF)
			return EIO;
	} else if (printf("%" PRIu64, (uint64_t)byte_offset) < 0) {
		return EIO;
	}
	if (printf(", \"mapping_page_size_bytes\": %" PRIu64
		   ", \"protection_key\": %u, \"name\": ",
		   (uint64_t)mapping->mapping_page_size,
		   mapping->protection_key) < 0)
		return EIO;
	ret = mapple_pslist_print_mapping_name(mapping);
	if (ret || fputs(", \"file\": ", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_mapped_file(mapping);
	return ret || putchar('}') == EOF ? (ret ? ret : EIO) : 0;
}

int mapple_pslist_print_json_memory_maps(
	const struct mapple_pslist_memory_maps *memory_maps, int indent)
{
	const char *status = mapple_pslist_memory_maps_object_status_name(
		memory_maps->status);
	size_t index;
	int ret;

	if (!status || printf("{\"collection_status\": \"%s\", "
			      "\"page_size_bytes\": %u, \"mapping_count\": %u, "
			      "\"mapping_limit\": %u, \"mappings\": [",
			      status, memory_maps->page_size_bytes,
			      memory_maps->mapping_count, memory_maps->mapping_limit) < 0)
		return EPROTO;
	for (index = 0; index < memory_maps->count; ++index) {
		if (fputs(index ? ",\n" : "\n", stdout) == EOF)
			return EIO;
		ret = mapple_pslist_print_memory_mapping(
			&memory_maps->mappings[index], memory_maps->page_size_bytes,
			indent + 4);
		if (ret)
			return ret;
	}
	if (memory_maps->count) {
		if (putchar('\n') == EOF)
			return EIO;
		ret = mapple_pslist_print_json_indent(indent + 2);
		if (ret)
			return ret;
	}
	return fputs("]}", stdout) == EOF ? EIO : 0;
}
