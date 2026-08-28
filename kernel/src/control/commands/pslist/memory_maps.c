#include <control/commands/pslist/memory_maps.h>

#include <asm/unaligned.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hugetlb.h>
#include <linux/kdev_t.h>
#include <linux/mm.h>
#include <linux/pkeys.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/time64.h>

#define MAPPLE_PSLIST_MEMORY_MAP_CAPTURE_RETRIES 3

struct mapple_pslist_memory_map_reference {
	struct mapple_pslist_memory_map_record wire;
	struct file *file;
	char name[MAPPLE_PSLIST_MEMORY_MAP_NAME_MAX_SIZE];
	u32 name_length;
};

struct mapple_pslist_memory_maps_snapshot {
	struct mapple_pslist_memory_map_reference *entries;
	u32 count;
	u16 status;
};

struct mapple_pslist_captured_memory_map {
	struct mapple_pslist_memory_map_record wire;
	char *path;
	u32 path_length;
};

static void mapple_pslist_encode_record(struct mapple_pslist_stream_record *record,
					u32 size, u16 type, u64 group)
{
	put_unaligned_be32(size, &record->record_size);
	put_unaligned_be16(type, &record->record_type);
	put_unaligned_be64(group, &record->group);
}

static void mapple_pslist_release_snapshot(
	struct mapple_pslist_memory_maps_snapshot *snapshot)
{
	u32 index;

	if (snapshot->entries)
		for (index = 0; index < snapshot->count; ++index)
			if (snapshot->entries[index].file)
				fput(snapshot->entries[index].file);
	kvfree(snapshot->entries);
}

static void mapple_pslist_capture_name(
	struct mapple_pslist_memory_map_reference *entry, const char *name,
	u16 kind)
{
	size_t length;

	put_unaligned_be16(kind, &entry->wire.name_kind);
	length = strnlen(name, MAPPLE_PSLIST_MEMORY_MAP_NAME_MAX_SIZE);
	if (length == MAPPLE_PSLIST_MEMORY_MAP_NAME_MAX_SIZE) {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_TOO_LARGE,
				   &entry->wire.name_status);
		put_unaligned_be32(E2BIG, &entry->wire.name_error);
		return;
	}
	memcpy(entry->name, name, length);
	entry->name_length = length;
	put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE, &entry->wire.name_status);
	put_unaligned_be32(entry->name_length, &entry->wire.name_length);
}

static void mapple_pslist_capture_mapping_name(
	struct mapple_pslist_memory_map_reference *entry,
	struct vm_area_struct *vma)
{
	const char *special_name;

#ifdef CONFIG_ANON_VMA_NAME
	if (vma->anon_name) {
		mapple_pslist_capture_name(
			entry, vma->anon_name->name,
			vma->vm_file ? MAPPLE_PSLIST_MEMORY_MAP_NAME_SHARED_ANONYMOUS :
				       MAPPLE_PSLIST_MEMORY_MAP_NAME_ANONYMOUS);
		return;
	}
#endif
	if (!vma->vm_file && vma->vm_ops && vma->vm_ops->name) {
		special_name = vma->vm_ops->name(vma);
		if (special_name) {
			mapple_pslist_capture_name(
				entry, special_name, MAPPLE_PSLIST_MEMORY_MAP_NAME_SPECIAL);
			return;
		}
	}
	if (!vma->vm_file && vma_is_initial_heap(vma))
		put_unaligned_be16(MAPPLE_PSLIST_MEMORY_MAP_NAME_HEAP,
				   &entry->wire.name_kind);
	else if (!vma->vm_file && vma_is_initial_stack(vma))
		put_unaligned_be16(MAPPLE_PSLIST_MEMORY_MAP_NAME_STACK,
				   &entry->wire.name_kind);
}

static void mapple_pslist_capture_mapping(
	struct mapple_pslist_memory_map_reference *entry,
	struct vm_area_struct *vma, u32 index)
{
	u16 mapping_type;

	if (vma->vm_file) {
		mapping_type = MAPPLE_PSLIST_MEMORY_MAPPING_FILE;
		get_file(vma->vm_file);
		entry->file = vma->vm_file;
	} else if (vma_is_anonymous(vma)) {
		mapping_type = MAPPLE_PSLIST_MEMORY_MAPPING_ANONYMOUS;
	} else {
		mapping_type = MAPPLE_PSLIST_MEMORY_MAPPING_SPECIAL;
	}
	put_unaligned_be32(index + 1, &entry->wire.mapping_index);
	put_unaligned_be16(mapping_type, &entry->wire.mapping_type);
	put_unaligned_be32(vma_pkey(vma), &entry->wire.protection_key);
	put_unaligned_be64(vma->vm_start, &entry->wire.start_address);
	put_unaligned_be64(vma->vm_end, &entry->wire.end_address);
	put_unaligned_be64(vma->vm_flags, &entry->wire.vm_flags);
	put_unaligned_be64(vma->vm_pgoff, &entry->wire.page_offset);
	put_unaligned_be64(vma_kernel_pagesize(vma), &entry->wire.mapping_page_size);
	mapple_pslist_capture_mapping_name(entry, vma);
}

static int mapple_pslist_capture_snapshot(
	struct mm_struct *mm, struct mapple_pslist_memory_maps_snapshot *snapshot)
{
	struct mapple_pslist_memory_map_reference *entries;
	struct vm_area_struct *vma;
	unsigned int attempt;
	u32 capacity;
	u32 count;

	for (attempt = 0; attempt < MAPPLE_PSLIST_MEMORY_MAP_CAPTURE_RETRIES;
	     ++attempt) {
		mmap_read_lock(mm);
		capacity = mm->map_count;
		mmap_read_unlock(mm);
		if (capacity > MAPPLE_PSLIST_MEMORY_MAP_MAX_COUNT) {
			snapshot->count = capacity;
			snapshot->status = MAPPLE_PSLIST_OBJECT_TOO_LARGE;
			return 0;
		}
		entries = capacity ?
			kvmalloc_array(capacity, sizeof(*entries), GFP_KERNEL | __GFP_ZERO) :
			NULL;
		if (capacity && !entries)
			return -ENOMEM;
		mmap_read_lock(mm);
		if (mm->map_count > capacity) {
			mmap_read_unlock(mm);
			kvfree(entries);
			continue;
		}
		count = 0;
		{
			VMA_ITERATOR(iterator, mm, 0);

			for_each_vma(iterator, vma) {
				mapple_pslist_capture_mapping(&entries[count], vma, count);
				++count;
			}
		}
		mmap_read_unlock(mm);
		snapshot->entries = entries;
		snapshot->count = count;
		snapshot->status = MAPPLE_PSLIST_OBJECT_COMPLETE;
		return 0;
	}
	snapshot->status = MAPPLE_PSLIST_OBJECT_RACED;
	return 0;
}

static void mapple_pslist_capture_path(
	struct mapple_pslist_captured_memory_map *captured, struct file *file)
{
	char *path;
	char *buffer;
	int error;

	buffer = kvmalloc(MAPPLE_PSLIST_MEMORY_MAP_PATH_MAX_SIZE, GFP_KERNEL);
	if (!buffer) {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE,
				   &captured->wire.path_status);
		put_unaligned_be32(ENOMEM, &captured->wire.path_error);
		return;
	}
	path = file_path(file, buffer, MAPPLE_PSLIST_MEMORY_MAP_PATH_MAX_SIZE);
	if (IS_ERR(path)) {
		error = -PTR_ERR(path);
		kvfree(buffer);
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE,
				   &captured->wire.path_status);
		put_unaligned_be32(error, &captured->wire.path_error);
		return;
	}
	captured->path_length = strlen(path);
	memmove(buffer, path, captured->path_length);
	captured->path = buffer;
	put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE, &captured->wire.path_status);
	put_unaligned_be32(captured->path_length, &captured->wire.path_length);
}

static void mapple_pslist_capture_file_metadata(
	struct mapple_pslist_captured_memory_map *captured, struct file *file)
{
	struct kstat stat;
	u32 flags = 0;
	int ret;

	if (d_unlinked(file->f_path.dentry))
		flags |= MAPPLE_PSLIST_MEMORY_MAP_FILE_DELETED;
	put_unaligned_be32(flags, &captured->wire.file_flags);
	ret = vfs_getattr_nosec(&file->f_path, &stat, STATX_BASIC_STATS,
				 AT_STATX_DONT_SYNC);
	if (ret) {
		put_unaligned_be16(MAPPLE_PSLIST_DATA_UNAVAILABLE,
				   &captured->wire.file_metadata_status);
		put_unaligned_be32(-ret, &captured->wire.file_metadata_error);
		return;
	}
	put_unaligned_be16(MAPPLE_PSLIST_DATA_AVAILABLE,
			   &captured->wire.file_metadata_status);
	put_unaligned_be32(MAJOR(stat.dev), &captured->wire.file_device_major);
	put_unaligned_be32(MINOR(stat.dev), &captured->wire.file_device_minor);
	put_unaligned_be32(stat.mode, &captured->wire.file_mode);
	put_unaligned_be64(stat.ino, &captured->wire.file_inode);
	put_unaligned_be64(stat.size, &captured->wire.file_size);
	put_unaligned_be64((u64)timespec64_to_ns(&stat.mtime),
			   &captured->wire.file_mtime_ns);
	put_unaligned_be64((u64)timespec64_to_ns(&stat.ctime),
			   &captured->wire.file_ctime_ns);
	put_unaligned_be64(file_inode(file)->i_sb->s_magic,
			   &captured->wire.file_filesystem_magic);
}

static int mapple_pslist_write_object_begin(struct mapple_command_context *context,
					     u64 object_id)
{
	struct mapple_pslist_object_begin begin = { 0 };

	mapple_pslist_encode_record(&begin.record, sizeof(begin),
				    MAPPLE_PSLIST_STREAM_OBJECT_BEGIN,
				    MAPPLE_PSLIST_GROUP_MEMORY_MAPS);
	put_unaligned_be64(object_id, &begin.object_id);
	put_unaligned_be16(MAPPLE_PSLIST_OBJECT_MM, &begin.object_type);
	return mapple_command_write(context, &begin, sizeof(begin));
}

static int mapple_pslist_write_metadata(struct mapple_command_context *context,
					 u64 object_id, u32 mapping_count)
{
	struct mapple_pslist_memory_maps_metadata_record metadata = { 0 };

	mapple_pslist_encode_record(&metadata.record, sizeof(metadata),
				    MAPPLE_PSLIST_STREAM_MEMORY_MAPS_METADATA,
				    MAPPLE_PSLIST_GROUP_MEMORY_MAPS);
	put_unaligned_be64(object_id, &metadata.object_id);
	put_unaligned_be32(PAGE_SIZE, &metadata.page_size_bytes);
	put_unaligned_be32(mapping_count, &metadata.mapping_count);
	put_unaligned_be32(MAPPLE_PSLIST_MEMORY_MAP_MAX_COUNT,
			   &metadata.mapping_limit);
	return mapple_command_write(context, &metadata, sizeof(metadata));
}

static int mapple_pslist_write_mapping(
	struct mapple_command_context *context,
	struct mapple_pslist_memory_map_reference *reference, u64 object_id)
{
	struct mapple_pslist_captured_memory_map captured = { 0 };
	int ret;

	memcpy(&captured.wire, &reference->wire, sizeof(captured.wire));
	put_unaligned_be64(object_id, &captured.wire.object_id);
	if (reference->file) {
		mapple_pslist_capture_path(&captured, reference->file);
		mapple_pslist_capture_file_metadata(&captured, reference->file);
	}
	mapple_pslist_encode_record(&captured.wire.record,
				    sizeof(captured.wire) + reference->name_length +
					    captured.path_length,
				    MAPPLE_PSLIST_STREAM_MEMORY_MAP,
				    MAPPLE_PSLIST_GROUP_MEMORY_MAPS);
	ret = mapple_command_write(context, &captured.wire, sizeof(captured.wire));
	if (!ret)
		ret = mapple_command_write(context, reference->name,
					   reference->name_length);
	if (!ret)
		ret = mapple_command_write(context, captured.path,
					   captured.path_length);
	kvfree(captured.path);
	return ret;
}

static int mapple_pslist_write_object_end(struct mapple_command_context *context,
					   u64 object_id, u16 status,
					   u32 record_count)
{
	struct mapple_pslist_object_end end = { 0 };

	mapple_pslist_encode_record(&end.record, sizeof(end),
				    MAPPLE_PSLIST_STREAM_OBJECT_END,
				    MAPPLE_PSLIST_GROUP_MEMORY_MAPS);
	put_unaligned_be64(object_id, &end.object_id);
	put_unaligned_be16(MAPPLE_PSLIST_OBJECT_MM, &end.object_type);
	put_unaligned_be16(status, &end.status);
	put_unaligned_be32(record_count, &end.record_count);
	return mapple_command_write(context, &end, sizeof(end));
}

int mapple_pslist_write_memory_maps_object(struct mapple_command_context *context,
					    struct mm_struct *mm, u64 object_id,
					    u32 *object_count, bool *partial)
{
	struct mapple_pslist_memory_maps_snapshot snapshot = { 0 };
	u32 index;
	int ret;

	ret = mapple_pslist_capture_snapshot(mm, &snapshot);
	if (ret)
		return ret;
	if (snapshot.status != MAPPLE_PSLIST_OBJECT_COMPLETE)
		*partial = true;
	ret = mapple_pslist_write_object_begin(context, object_id);
	if (ret)
		goto release;
	++*object_count;
	ret = mapple_pslist_write_metadata(context, object_id, snapshot.count);
	if (!ret && snapshot.status == MAPPLE_PSLIST_OBJECT_COMPLETE)
		for (index = 0; !ret && index < snapshot.count; ++index)
			ret = mapple_pslist_write_mapping(
				context, &snapshot.entries[index], object_id);
	if (!ret)
		ret = mapple_pslist_write_object_end(
			context, object_id, snapshot.status,
			1 + (snapshot.status == MAPPLE_PSLIST_OBJECT_COMPLETE ?
				     snapshot.count : 0));

release:
	mapple_pslist_release_snapshot(&snapshot);
	return ret;
}
