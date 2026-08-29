#include <commands/pslist/mapped_files.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <commands/pslist/json.h>

static int mapple_pslist_mapped_file_bytes_equal(const unsigned char *left, const unsigned char *right,
												 __u32 length)
{
	return !length || !memcmp(left, right, length);
}

static int mapple_pslist_mapped_files_equal(const struct mapple_pslist_memory_map *left,
											const struct mapple_pslist_memory_map *right)
{
	return left->elf_object_id == right->elf_object_id && left->path_status == right->path_status &&
		   left->file_metadata_status == right->file_metadata_status &&
		   left->path_error == right->path_error && left->file_metadata_error == right->file_metadata_error &&
		   left->file_flags == right->file_flags && left->path_length == right->path_length &&
		   left->file_device_major == right->file_device_major &&
		   left->file_device_minor == right->file_device_minor && left->file_mode == right->file_mode &&
		   left->file_inode == right->file_inode && left->file_size == right->file_size &&
		   left->file_mtime_ns == right->file_mtime_ns && left->file_ctime_ns == right->file_ctime_ns &&
		   left->file_filesystem_magic == right->file_filesystem_magic &&
		   mapple_pslist_mapped_file_bytes_equal(left->path, right->path, left->path_length);
}

static int mapple_pslist_grow_mapped_files(struct mapple_pslist_snapshot *snapshot)
{
	struct mapple_pslist_mapped_file *files;
	size_t capacity;

	if (snapshot->mapped_file_count < snapshot->mapped_file_capacity)
		return 0;
	capacity = snapshot->mapped_file_capacity ? snapshot->mapped_file_capacity * 2 : 64;
	if (capacity < snapshot->mapped_file_capacity)
		return EOVERFLOW;
	files = realloc(snapshot->mapped_files, capacity * sizeof(*files));
	if (!files)
		return ENOMEM;
	snapshot->mapped_files = files;
	snapshot->mapped_file_capacity = capacity;
	return 0;
}

static int mapple_pslist_add_elf_mapped_file(struct mapple_pslist_snapshot *snapshot, __u64 elf_id,
											 __u64 file_id)
{
	struct mapple_pslist_elf *elf;
	__u64 *ids;
	size_t capacity;

	if (!elf_id)
		return 0;
	if (elf_id > snapshot->elf_count || snapshot->elfs[elf_id - 1].object_id != elf_id)
		return EPROTO;
	elf = &snapshot->elfs[elf_id - 1];
	if (elf->mapped_file_count < elf->mapped_file_capacity) {
		elf->mapped_file_ids[elf->mapped_file_count++] = file_id;
		return 0;
	}
	capacity = elf->mapped_file_capacity ? elf->mapped_file_capacity * 2 : 4;
	if (capacity < elf->mapped_file_capacity)
		return EOVERFLOW;
	ids = realloc(elf->mapped_file_ids, capacity * sizeof(*ids));
	if (!ids)
		return ENOMEM;
	elf->mapped_file_ids = ids;
	elf->mapped_file_capacity = capacity;
	elf->mapped_file_ids[elf->mapped_file_count++] = file_id;
	return 0;
}

static int mapple_pslist_add_mapped_file(struct mapple_pslist_snapshot *snapshot,
										 struct mapple_pslist_memory_map *mapping)
{
	struct mapple_pslist_mapped_file *file;
	size_t index;
	int ret;

	for (index = 0; index < snapshot->mapped_file_count; ++index) {
		file = &snapshot->mapped_files[index];
		if (!mapple_pslist_mapped_files_equal(file->mapping, mapping))
			continue;
		mapping->mapped_file_object_id = file->object_id;
		return 0;
	}
	ret = mapple_pslist_grow_mapped_files(snapshot);
	if (ret)
		return ret;
	file = &snapshot->mapped_files[snapshot->mapped_file_count];
	file->object_id = snapshot->mapped_file_count + 1;
	file->mapping = mapping;
	mapping->mapped_file_object_id = file->object_id;
	++snapshot->mapped_file_count;
	return mapple_pslist_add_elf_mapped_file(snapshot, mapping->elf_object_id, file->object_id);
}

int mapple_pslist_build_mapped_files(struct mapple_pslist_snapshot *snapshot)
{
	size_t object_index;
	size_t mapping_index;
	int ret;

	for (object_index = 0; object_index < snapshot->memory_maps_count; ++object_index) {
		struct mapple_pslist_memory_maps *maps = &snapshot->memory_maps[object_index];

		for (mapping_index = 0; mapping_index < maps->count; ++mapping_index) {
			struct mapple_pslist_memory_map *mapping = &maps->mappings[mapping_index];

			if (mapping->mapping_type != MAPPLE_PSLIST_MEMORY_MAPPING_FILE)
				continue;
			ret = mapple_pslist_add_mapped_file(snapshot, mapping);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static const char *mapple_pslist_mapped_file_status_name(__u16 status)
{
	switch (status) {
	case MAPPLE_PSLIST_DATA_AVAILABLE:
		return "available";
	case MAPPLE_PSLIST_DATA_UNAVAILABLE:
		return "unavailable";
	case MAPPLE_PSLIST_DATA_TOO_LARGE:
		return "too_large";
	case MAPPLE_PSLIST_DATA_RACED:
		return "raced";
	case MAPPLE_PSLIST_DATA_UNSUPPORTED:
		return "unsupported";
	default:
		return NULL;
	}
}

static int mapple_pslist_print_mapped_file_error(__s32 error)
{
	if (!error)
		return fputs("null", stdout) == EOF ? EIO : 0;
	if (printf("{\"code\": %d, \"message\": ", error) < 0)
		return EIO;
	if (mapple_pslist_print_json_bytes((const unsigned char *)strerror(error), strlen(strerror(error))))
		return EIO;
	return putchar('}') == EOF ? EIO : 0;
}

static int mapple_pslist_print_mapped_file(const struct mapple_pslist_mapped_file *file)
{
	const struct mapple_pslist_memory_map *mapping = file->mapping;
	const char *path_status = mapple_pslist_mapped_file_status_name(mapping->path_status);
	const char *metadata_status = mapple_pslist_mapped_file_status_name(mapping->file_metadata_status);
	int ret;

	if (!path_status || !metadata_status)
		return EPROTO;
	if (printf("{\"deleted\": %s, \"path\": {\"collection_status\": \"%s\", "
			   "\"value\": ",
			   mapping->file_flags & MAPPLE_PSLIST_MEMORY_MAP_FILE_DELETED ? "true" : "false",
			   path_status) < 0)
		return EIO;
	ret = mapping->path ? mapple_pslist_print_json_bytes(mapping->path, mapping->path_length)
						: (fputs("null", stdout) == EOF ? EIO : 0);
	if (ret || fputs(", \"error\": ", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_mapped_file_error(mapping->path_error);
	if (ret || printf("}, \"identity\": {\"collection_status\": \"%s\", "
					  "\"error\": ",
					  metadata_status) < 0)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_mapped_file_error(mapping->file_metadata_error);
	if (ret)
		return ret;
	if (mapping->file_metadata_status == MAPPLE_PSLIST_DATA_AVAILABLE &&
		printf(", \"device\": {\"major\": %u, \"minor\": %u}, "
			   "\"inode\": \"%" PRIu64 "\", \"mode\": %u, \"size\": \"%" PRIu64 "\", \"mtime_ns\": \"%" PRId64 "\""
			   ", \"ctime_ns\": \"%" PRId64 "\", \"filesystem_magic\": \"0x%016" PRIx64 "\"",
			   mapping->file_device_major, mapping->file_device_minor, (uint64_t)mapping->file_inode,
			   mapping->file_mode, (uint64_t)mapping->file_size, (int64_t)mapping->file_mtime_ns,
			   (int64_t)mapping->file_ctime_ns, (uint64_t)mapping->file_filesystem_magic) < 0)
		return EIO;
	if (fputs("}", stdout) == EOF)
		return EIO;
	if (mapping->elf_object_id && printf(", \"elf\": %" PRIu64, (uint64_t)mapping->elf_object_id) < 0)
		return EIO;
	return putchar('}') == EOF ? EIO : 0;
}

int mapple_pslist_print_json_mapped_files(const struct mapple_pslist_snapshot *snapshot, int indent)
{
	size_t index;
	int ret;

	if (putchar('{') == EOF)
		return EIO;
	for (index = 0; index < snapshot->mapped_file_count; ++index) {
		if (printf("%s\n%*s\"%" PRIu64 "\": ", index ? "," : "", indent + 2, "",
				   (uint64_t)snapshot->mapped_files[index].object_id) < 0)
			return EIO;
		ret = mapple_pslist_print_mapped_file(&snapshot->mapped_files[index]);
		if (ret)
			return ret;
	}
	if (snapshot->mapped_file_count && printf("\n%*s", indent, "") < 0)
		return EIO;
	return putchar('}') == EOF ? EIO : 0;
}
