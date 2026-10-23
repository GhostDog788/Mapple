#include <control/commands/pslist/elf/objects.h>
#include <control/commands/pslist/elf/parser.h>

#include <asm/unaligned.h>
#include <linux/elf.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/time64.h>

struct mapple_pslist_elf_file {
	struct hlist_node hash_node;
	struct list_head order_node;
	struct inode *inode;
	struct file *file;
	u64 object_id;
};

static bool mapple_pslist_elf_stat_changed(const struct kstat *before,
					    const struct kstat *after)
{
	return before->dev != after->dev || before->ino != after->ino ||
	       before->size != after->size ||
	       !timespec64_equal(&before->mtime, &after->mtime) ||
	       !timespec64_equal(&before->ctime, &after->ctime);
}

static int mapple_pslist_elf_capture_file(struct mapple_pslist_elf_file *entry,
					  struct mapple_pslist_elf_capture *capture)
{
	struct kstat before;
	struct kstat after;
	int ret;

	ret = vfs_getattr_nosec(&entry->file->f_path, &before, STATX_BASIC_STATS,
				AT_STATX_DONT_SYNC);
	if (ret)
		goto unavailable;
	if (before.size < 0) {
		ret = -ENOEXEC;
		goto unavailable;
	}
	capture->file_size = before.size;
	put_unaligned_be32(MAJOR(before.dev), &capture->metadata.file_device_major);
	put_unaligned_be32(MINOR(before.dev), &capture->metadata.file_device_minor);
	put_unaligned_be32(before.mode, &capture->metadata.file_mode);
	put_unaligned_be64(before.ino, &capture->metadata.file_inode);
	put_unaligned_be64(before.size, &capture->metadata.file_size);
	put_unaligned_be64((u64)timespec64_to_ns(&before.mtime), &capture->metadata.file_mtime_ns);
	put_unaligned_be64((u64)timespec64_to_ns(&before.ctime), &capture->metadata.file_ctime_ns);
	put_unaligned_be64(file_inode(entry->file)->i_sb->s_magic,
			   &capture->metadata.file_filesystem_magic);
	ret = mapple_pslist_elf_parse(entry->file, capture);
	if (ret == -ENOMEM)
		return ret;
	if (ret)
		goto capture_error;
	ret = vfs_getattr_nosec(&entry->file->f_path, &after, STATX_BASIC_STATS,
				AT_STATX_DONT_SYNC);
	if (ret)
		goto unavailable;
	if (mapple_pslist_elf_stat_changed(&before, &after)) {
		capture->status = MAPPLE_PSLIST_OBJECT_RACED;
		capture->error = EAGAIN;
		return 0;
	}
	capture->status = MAPPLE_PSLIST_OBJECT_COMPLETE;
	return 0;

capture_error:
	capture->status = ret == -E2BIG ? MAPPLE_PSLIST_OBJECT_TOO_LARGE :
					  MAPPLE_PSLIST_OBJECT_UNAVAILABLE;
	capture->error = -ret;
	return 0;

unavailable:
	if (ret == -ENOMEM)
		return ret;
	capture->status = MAPPLE_PSLIST_OBJECT_UNAVAILABLE;
	capture->error = -ret;
	return 0;
}

void mapple_pslist_elf_registry_init(struct mapple_pslist_elf_registry *registry)
{
	hash_init(registry->files);
	INIT_LIST_HEAD(&registry->order);
	registry->next_object_id = 1;
}

void mapple_pslist_elf_registry_release(struct mapple_pslist_elf_registry *registry)
{
	struct mapple_pslist_elf_file *entry;
	struct mapple_pslist_elf_file *temporary;

	list_for_each_entry_safe(entry, temporary, &registry->order, order_node) {
		hash_del(&entry->hash_node);
		list_del(&entry->order_node);
		fput(entry->file);
		kfree(entry);
	}
}

int mapple_pslist_elf_observe_file(struct mapple_pslist_elf_registry *registry,
				    struct file *file, u64 *object_id)
{
	struct mapple_pslist_elf_file *entry;
	struct inode *inode = file_inode(file);
	loff_t position = 0;
	u8 magic[SELFMAG];
	ssize_t read;

	*object_id = 0;
	if (!S_ISREG(inode->i_mode))
		return 0;
	hash_for_each_possible(registry->files, entry, hash_node,
			       (unsigned long)inode) {
		if (entry->inode == inode) {
			*object_id = entry->object_id;
			return 0;
		}
	}
	read = kernel_read(file, magic, sizeof(magic), &position);
	if (read < 0)
		return read;
	if (read != sizeof(magic) || memcmp(magic, ELFMAG, SELFMAG))
		return 0;
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	get_file(file);
	entry->file = file;
	entry->inode = inode;
	entry->object_id = registry->next_object_id++;
	hash_add(registry->files, &entry->hash_node, (unsigned long)inode);
	list_add_tail(&entry->order_node, &registry->order);
	*object_id = entry->object_id;
	return 0;
}

static int mapple_pslist_elf_write_string(struct mapple_pslist_writer *writer, u64 object_id,
					  u32 index, const struct mapple_pslist_elf_string *string)
{
	struct mapple_pslist_elf_string_record record = { 0 };
	int ret;

	mapple_pslist_encode_record(&record.record, sizeof(record) + string->length,
				    MAPPLE_PSLIST_STREAM_ELF_STRING, MAPPLE_PSLIST_GROUP_ELF);
	put_unaligned_be64(object_id, &record.object_id);
	put_unaligned_be32(index + 1, &record.string_index);
	put_unaligned_be16(string->kind, &record.string_kind);
	put_unaligned_be32(string->length, &record.byte_length);
	ret = mapple_pslist_write(writer, &record, sizeof(record));
	if (!ret)
		ret = mapple_pslist_write(writer, string->bytes, string->length);
	return ret;
}

static int mapple_pslist_elf_write_symbol(struct mapple_pslist_writer *writer, u64 object_id,
					  u32 index, const struct mapple_pslist_elf_symbol *symbol)
{
	struct mapple_pslist_elf_symbol_record record = { 0 };
	int ret;

	mapple_pslist_encode_record(&record.record, sizeof(record) + symbol->name_length,
				    MAPPLE_PSLIST_STREAM_ELF_SYMBOL, MAPPLE_PSLIST_GROUP_ELF);
	put_unaligned_be64(object_id, &record.object_id);
	put_unaligned_be32(index + 1, &record.symbol_index);
	put_unaligned_be16(symbol->kind, &record.symbol_kind);
	record.binding = symbol->binding;
	record.symbol_type = symbol->type;
	record.visibility = symbol->visibility;
	put_unaligned_be16(symbol->section_index, &record.section_index);
	put_unaligned_be32(symbol->name_length, &record.name_length);
	put_unaligned_be64(symbol->value, &record.value);
	put_unaligned_be64(symbol->size, &record.size);
	ret = mapple_pslist_write(writer, &record, sizeof(record));
	if (!ret)
		ret = mapple_pslist_write(writer, symbol->name, symbol->name_length);
	return ret;
}

static int mapple_pslist_write_elf_object(struct mapple_pslist_writer *writer,
					  struct mapple_pslist_elf_file *entry)
{
	struct mapple_pslist_elf_capture capture = { 0 };
	u32 index;
	int ret;

	put_unaligned_be64(entry->object_id, &capture.metadata.object_id);
	ret = mapple_pslist_elf_capture_file(entry, &capture);
	if (ret)
		goto release;
	if (capture.status != MAPPLE_PSLIST_OBJECT_COMPLETE) {
		writer->partial = true;
		mapple_pslist_elf_discard_details(&capture);
	}
	put_unaligned_be32(capture.string_count, &capture.metadata.string_record_count);
	put_unaligned_be32(capture.symbol_count, &capture.metadata.symbol_record_count);
	put_unaligned_be32(capture.import_count, &capture.metadata.import_count);
	put_unaligned_be32(capture.export_count, &capture.metadata.export_count);
	mapple_pslist_encode_record(&capture.metadata.record, sizeof(capture.metadata),
				    MAPPLE_PSLIST_STREAM_ELF_METADATA, MAPPLE_PSLIST_GROUP_ELF);
	ret = mapple_pslist_write_object_begin(
		writer, entry->object_id, MAPPLE_PSLIST_OBJECT_ELF_FILE, MAPPLE_PSLIST_GROUP_ELF);
	if (ret)
		goto release;
	ret = mapple_pslist_write(writer, &capture.metadata, sizeof(capture.metadata));
	for (index = 0; !ret && index < capture.string_count; ++index)
		ret = mapple_pslist_elf_write_string(writer, entry->object_id, index,
						     &capture.strings[index]);
	for (index = 0; !ret && index < capture.symbol_count; ++index)
		ret = mapple_pslist_elf_write_symbol(writer, entry->object_id, index,
						     &capture.symbols[index]);
	if (!ret)
		ret = mapple_pslist_write_object_end(
			writer, entry->object_id, MAPPLE_PSLIST_OBJECT_ELF_FILE,
			MAPPLE_PSLIST_GROUP_ELF, capture.status, capture.error,
			1 + capture.string_count + capture.symbol_count);

release:
	mapple_pslist_elf_release_capture(&capture);
	return ret;
}

int mapple_pslist_write_elf_objects(struct mapple_pslist_writer *writer,
				    struct mapple_pslist_elf_registry *registry)
{
	struct mapple_pslist_elf_file *entry;
	int ret = 0;

	list_for_each_entry(entry, &registry->order, order_node) {
		ret = mapple_pslist_write_elf_object(writer, entry);
		if (ret)
			break;
	}
	return ret;
}
