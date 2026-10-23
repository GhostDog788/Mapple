#include <control/commands/pslist/elf/parser.h>

#include <asm/unaligned.h>
#include <linux/elf.h>
#include <linux/slab.h>

#define MAPPLE_ELF_EI_ABI_VERSION 8
#define MAPPLE_ELF_SHN_XINDEX 0xffff
#define MAPPLE_ELF_DT_RUNPATH 29
#define MAPPLE_ELF_STV_DEFAULT 0
#define MAPPLE_ELF_STV_PROTECTED 3
#define MAPPLE_ELF_NT_GNU_BUILD_ID 3

struct mapple_pslist_elf_section {
	u32 type;
	u32 link;
	u64 offset;
	u64 size;
	u64 entry_size;
};

static int mapple_pslist_elf_read(struct file *file, u64 offset, void *buffer,
				   size_t size, u64 file_size)
{
	loff_t position;
	ssize_t read;

	if (offset > file_size || size > file_size - offset || offset > S64_MAX)
		return -ENOEXEC;
	position = offset;
	read = kernel_read(file, buffer, size, &position);
	if (read < 0)
		return read;
	return read == size ? 0 : -ENOEXEC;
}

static u16 mapple_pslist_elf_u16(const struct mapple_pslist_elf_capture *capture,
				  const void *value)
{
	return capture->data_encoding == ELFDATA2LSB ? get_unaligned_le16(value) :
							 get_unaligned_be16(value);
}

static u32 mapple_pslist_elf_u32(const struct mapple_pslist_elf_capture *capture,
				  const void *value)
{
	return capture->data_encoding == ELFDATA2LSB ? get_unaligned_le32(value) :
							 get_unaligned_be32(value);
}

static u64 mapple_pslist_elf_u64(const struct mapple_pslist_elf_capture *capture,
				  const void *value)
{
	return capture->data_encoding == ELFDATA2LSB ? get_unaligned_le64(value) :
							 get_unaligned_be64(value);
}

static int mapple_pslist_elf_grow(void **entries, u32 *capacity, u32 count,
				   size_t entry_size, u32 limit)
{
	u32 new_capacity;
	void *new_entries;

	if (count < *capacity)
		return 0;
	if (count >= limit)
		return -E2BIG;
	new_capacity = *capacity ? *capacity * 2 : 32;
	if (new_capacity < *capacity || new_capacity > limit)
		new_capacity = limit;
	new_entries = krealloc_array(*entries, new_capacity, entry_size, GFP_KERNEL);
	if (!new_entries)
		return -ENOMEM;
	*entries = new_entries;
	*capacity = new_capacity;
	return 0;
}

static int mapple_pslist_elf_add_string(struct mapple_pslist_elf_capture *capture,
					 u16 kind, const u8 *bytes, size_t length)
{
	struct mapple_pslist_elf_string *string;
	int ret;

	if (!length || length > MAPPLE_PSLIST_ELF_STRING_MAX_SIZE)
		return -E2BIG;
	ret = mapple_pslist_elf_grow((void **)&capture->strings,
				     &capture->string_capacity, capture->string_count,
				     sizeof(*capture->strings),
				     MAPPLE_PSLIST_ELF_MAX_DYNAMIC_STRINGS);
	if (ret)
		return ret;
	string = &capture->strings[capture->string_count];
	memset(string, 0, sizeof(*string));
	string->bytes = kmemdup(bytes, length, GFP_KERNEL);
	if (!string->bytes)
		return -ENOMEM;
	string->kind = kind;
	string->length = length;
	++capture->string_count;
	return 0;
}

static int mapple_pslist_elf_add_table_string(
	struct mapple_pslist_elf_capture *capture, u16 kind, const u8 *table,
	size_t table_size, u64 offset)
{
	const u8 *end;
	size_t available;
	size_t length;

	if (offset >= table_size)
		return -ENOEXEC;
	available = table_size - offset;
	end = memchr(table + offset, '\0', available);
	if (!end)
		return -ENOEXEC;
	length = end - (table + offset);
	if (!length)
		return -ENOEXEC;
	return mapple_pslist_elf_add_string(capture, kind, table + offset, length);
}

static int mapple_pslist_elf_add_symbol(struct mapple_pslist_elf_capture *capture,
					 const u8 *name, size_t name_length,
					 u16 kind, u8 binding, u8 type,
					 u8 visibility, u16 section_index,
					 u64 value, u64 size)
{
	struct mapple_pslist_elf_symbol *symbol;
	int ret;

	if (!name_length)
		return 0;
	if (name_length > MAPPLE_PSLIST_ELF_STRING_MAX_SIZE)
		return -E2BIG;
	ret = mapple_pslist_elf_grow((void **)&capture->symbols,
				     &capture->symbol_capacity, capture->symbol_count,
				     sizeof(*capture->symbols),
				     MAPPLE_PSLIST_ELF_MAX_DYNAMIC_SYMBOLS);
	if (ret)
		return ret;
	symbol = &capture->symbols[capture->symbol_count];
	memset(symbol, 0, sizeof(*symbol));
	symbol->name = kmemdup(name, name_length, GFP_KERNEL);
	if (!symbol->name)
		return -ENOMEM;
	symbol->kind = kind;
	symbol->binding = binding;
	symbol->type = type;
	symbol->visibility = visibility;
	symbol->section_index = section_index;
	symbol->name_length = name_length;
	symbol->value = value;
	symbol->size = size;
	++capture->symbol_count;
	if (kind == MAPPLE_PSLIST_ELF_SYMBOL_IMPORT)
		++capture->import_count;
	else
		++capture->export_count;
	return 0;
}

static int mapple_pslist_elf_read_section_header(
	struct file *file, struct mapple_pslist_elf_capture *capture, u64 offset,
	u16 entry_size, struct mapple_pslist_elf_section *section, u32 *info)
{
	Elf64_Shdr header = { 0 };
	size_t minimum = capture->elf_class == ELFCLASS64 ? sizeof(Elf64_Shdr) :
							      sizeof(Elf32_Shdr);
	int ret;

	if (entry_size < minimum)
		return -ENOEXEC;
	ret = mapple_pslist_elf_read(file, offset, &header, minimum,
				     capture->file_size);
	if (ret)
		return ret;
	if (capture->elf_class == ELFCLASS64) {
		const Elf64_Shdr *wire = &header;

		section->type = mapple_pslist_elf_u32(capture, &wire->sh_type);
		section->link = mapple_pslist_elf_u32(capture, &wire->sh_link);
		section->offset = mapple_pslist_elf_u64(capture, &wire->sh_offset);
		section->size = mapple_pslist_elf_u64(capture, &wire->sh_size);
		section->entry_size = mapple_pslist_elf_u64(capture, &wire->sh_entsize);
		if (info)
			*info = mapple_pslist_elf_u32(capture, &wire->sh_info);
	} else {
		const Elf32_Shdr *wire = (const Elf32_Shdr *)&header;

		section->type = mapple_pslist_elf_u32(capture, &wire->sh_type);
		section->link = mapple_pslist_elf_u32(capture, &wire->sh_link);
		section->offset = mapple_pslist_elf_u32(capture, &wire->sh_offset);
		section->size = mapple_pslist_elf_u32(capture, &wire->sh_size);
		section->entry_size = mapple_pslist_elf_u32(capture, &wire->sh_entsize);
		if (info)
			*info = mapple_pslist_elf_u32(capture, &wire->sh_info);
	}
	if (section->type != SHT_NULL && section->type != SHT_NOBITS &&
	    (section->offset > capture->file_size ||
	     section->size > capture->file_size - section->offset))
		return -ENOEXEC;
	return 0;
}

static int mapple_pslist_elf_capture_header(struct file *file,
					     struct mapple_pslist_elf_capture *capture)
{
	Elf64_Ehdr header = { 0 };
	u32 section_info = 0;
	u64 program_offset;
	u64 section_offset;
	u32 program_count;
	u32 section_count;
	u32 section_name_index;
	u16 program_entry_size;
	u16 section_entry_size;
	u16 elf_header_size;
	size_t header_size;
	int ret;

	ret = mapple_pslist_elf_read(file, 0, &header.e_ident, EI_NIDENT,
				     capture->file_size);
	if (ret)
		return ret;
	if (memcmp(header.e_ident, ELFMAG, SELFMAG) ||
	    (header.e_ident[EI_CLASS] != ELFCLASS32 &&
	     header.e_ident[EI_CLASS] != ELFCLASS64) ||
	    (header.e_ident[EI_DATA] != ELFDATA2LSB &&
	     header.e_ident[EI_DATA] != ELFDATA2MSB) ||
	    header.e_ident[EI_VERSION] != EV_CURRENT)
		return -ENOEXEC;
	capture->elf_class = header.e_ident[EI_CLASS];
	capture->data_encoding = header.e_ident[EI_DATA];
	header_size = capture->elf_class == ELFCLASS64 ? sizeof(Elf64_Ehdr) :
							     sizeof(Elf32_Ehdr);
	ret = mapple_pslist_elf_read(file, 0, &header, header_size,
				     capture->file_size);
	if (ret)
		return ret;
	if (capture->elf_class == ELFCLASS64) {
		const Elf64_Ehdr *wire = &header;

		program_offset = mapple_pslist_elf_u64(capture, &wire->e_phoff);
		section_offset = mapple_pslist_elf_u64(capture, &wire->e_shoff);
		program_entry_size = mapple_pslist_elf_u16(capture, &wire->e_phentsize);
		elf_header_size = mapple_pslist_elf_u16(capture, &wire->e_ehsize);
		program_count = mapple_pslist_elf_u16(capture, &wire->e_phnum);
		section_entry_size = mapple_pslist_elf_u16(capture, &wire->e_shentsize);
		section_count = mapple_pslist_elf_u16(capture, &wire->e_shnum);
		section_name_index = mapple_pslist_elf_u16(capture, &wire->e_shstrndx);
		put_unaligned_be16(mapple_pslist_elf_u16(capture, &wire->e_type),
				   &capture->metadata.object_type);
		put_unaligned_be16(mapple_pslist_elf_u16(capture, &wire->e_machine),
				   &capture->metadata.machine);
		put_unaligned_be32(mapple_pslist_elf_u32(capture, &wire->e_version),
				   &capture->metadata.elf_version);
		put_unaligned_be32(mapple_pslist_elf_u32(capture, &wire->e_flags),
				   &capture->metadata.flags);
		put_unaligned_be64(mapple_pslist_elf_u64(capture, &wire->e_entry),
				   &capture->metadata.entry_address);
	} else {
		const Elf32_Ehdr *wire = (const Elf32_Ehdr *)&header;

		program_offset = mapple_pslist_elf_u32(capture, &wire->e_phoff);
		section_offset = mapple_pslist_elf_u32(capture, &wire->e_shoff);
		program_entry_size = mapple_pslist_elf_u16(capture, &wire->e_phentsize);
		elf_header_size = mapple_pslist_elf_u16(capture, &wire->e_ehsize);
		program_count = mapple_pslist_elf_u16(capture, &wire->e_phnum);
		section_entry_size = mapple_pslist_elf_u16(capture, &wire->e_shentsize);
		section_count = mapple_pslist_elf_u16(capture, &wire->e_shnum);
		section_name_index = mapple_pslist_elf_u16(capture, &wire->e_shstrndx);
		put_unaligned_be16(mapple_pslist_elf_u16(capture, &wire->e_type),
				   &capture->metadata.object_type);
		put_unaligned_be16(mapple_pslist_elf_u16(capture, &wire->e_machine),
				   &capture->metadata.machine);
		put_unaligned_be32(mapple_pslist_elf_u32(capture, &wire->e_version),
				   &capture->metadata.elf_version);
		put_unaligned_be32(mapple_pslist_elf_u32(capture, &wire->e_flags),
				   &capture->metadata.flags);
		put_unaligned_be64(mapple_pslist_elf_u32(capture, &wire->e_entry),
				   &capture->metadata.entry_address);
	}
	if (elf_header_size < header_size)
		return -ENOEXEC;
	if (section_offset && (!section_entry_size ||
	    section_entry_size < (capture->elf_class == ELFCLASS64 ?
				    sizeof(Elf64_Shdr) : sizeof(Elf32_Shdr))))
		return -ENOEXEC;
	if (section_offset && (!section_count || program_count == PN_XNUM ||
			       section_name_index == MAPPLE_ELF_SHN_XINDEX)) {
		struct mapple_pslist_elf_section first;

		ret = mapple_pslist_elf_read_section_header(
			file, capture, section_offset, section_entry_size, &first,
			&section_info);
		if (ret)
			return ret;
		if (!section_count) {
			if (first.size > U32_MAX)
				return -E2BIG;
			section_count = first.size;
		}
		if (program_count == PN_XNUM)
			program_count = section_info;
		if (section_name_index == MAPPLE_ELF_SHN_XINDEX)
			section_name_index = first.link;
	}
	if (program_count > MAPPLE_PSLIST_ELF_MAX_PROGRAM_HEADERS ||
	    section_count > MAPPLE_PSLIST_ELF_MAX_SECTION_HEADERS)
		return -E2BIG;
	if (program_count &&
	    (!program_offset || program_entry_size <
		(capture->elf_class == ELFCLASS64 ? sizeof(Elf64_Phdr) :
						       sizeof(Elf32_Phdr))))
		return -ENOEXEC;
	if (program_count &&
	    (program_offset > capture->file_size ||
	     (u64)program_count * program_entry_size >
		capture->file_size - program_offset))
		return -ENOEXEC;
	if (section_count &&
	    (!section_offset || section_offset > capture->file_size ||
	     (u64)section_count * section_entry_size >
		capture->file_size - section_offset))
		return -ENOEXEC;
	if (section_name_index != SHN_UNDEF && section_name_index >= section_count)
		return -ENOEXEC;
	capture->metadata.elf_class = capture->elf_class;
	capture->metadata.data_encoding = capture->data_encoding;
	capture->metadata.os_abi = header.e_ident[EI_OSABI];
	capture->metadata.abi_version = header.e_ident[MAPPLE_ELF_EI_ABI_VERSION];
	put_unaligned_be32(program_count, &capture->metadata.program_header_count);
	put_unaligned_be32(section_count, &capture->metadata.section_header_count);
	put_unaligned_be64(program_offset, &capture->metadata.program_header_offset);
	put_unaligned_be64(section_offset, &capture->metadata.section_header_offset);
	put_unaligned_be16(program_entry_size,
			   &capture->metadata.program_header_entry_size);
	put_unaligned_be16(section_entry_size,
			   &capture->metadata.section_header_entry_size);
	put_unaligned_be16(section_name_index, &capture->metadata.section_name_index);
	capture->section_count = section_count;
	if (section_count) {
		u32 index;

		capture->sections = kvmalloc_array(section_count,
						    sizeof(*capture->sections), GFP_KERNEL);
		if (!capture->sections)
			return -ENOMEM;
		for (index = 0; index < section_count; ++index) {
			ret = mapple_pslist_elf_read_section_header(
				file, capture,
				section_offset + (u64)index * section_entry_size,
				section_entry_size, &capture->sections[index], NULL);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int mapple_pslist_elf_capture_interpreter(
	struct file *file, struct mapple_pslist_elf_capture *capture)
{
	Elf64_Phdr header;
	u64 table_offset = get_unaligned_be64(&capture->metadata.program_header_offset);
	u32 count = get_unaligned_be32(&capture->metadata.program_header_count);
	u16 entry_size = get_unaligned_be16(&capture->metadata.program_header_entry_size);
	u32 index;

	for (index = 0; index < count; ++index) {
		u64 offset;
		u64 size;
		u32 type;
		size_t minimum = capture->elf_class == ELFCLASS64 ? sizeof(Elf64_Phdr) :
								      sizeof(Elf32_Phdr);
		u8 *bytes;
		int ret;

		ret = mapple_pslist_elf_read(file,
			table_offset + (u64)index * entry_size, &header, minimum,
			capture->file_size);
		if (ret)
			return ret;
		if (capture->elf_class == ELFCLASS64) {
			const Elf64_Phdr *wire = &header;

			type = mapple_pslist_elf_u32(capture, &wire->p_type);
			offset = mapple_pslist_elf_u64(capture, &wire->p_offset);
			size = mapple_pslist_elf_u64(capture, &wire->p_filesz);
		} else {
			const Elf32_Phdr *wire = (const Elf32_Phdr *)&header;

			type = mapple_pslist_elf_u32(capture, &wire->p_type);
			offset = mapple_pslist_elf_u32(capture, &wire->p_offset);
			size = mapple_pslist_elf_u32(capture, &wire->p_filesz);
		}
		if (type != PT_INTERP)
			continue;
		if (size < 2 || size > MAPPLE_PSLIST_ELF_STRING_MAX_SIZE + 1)
			return -E2BIG;
		bytes = kmalloc(size, GFP_KERNEL);
		if (!bytes)
			return -ENOMEM;
		ret = mapple_pslist_elf_read(file, offset, bytes, size,
					     capture->file_size);
		if (!ret && bytes[size - 1])
			ret = -ENOEXEC;
		if (!ret)
			ret = mapple_pslist_elf_add_string(
				capture, MAPPLE_PSLIST_ELF_STRING_INTERPRETER,
				bytes, size - 1);
		kfree(bytes);
		return ret;
	}
	return 0;
}

static int mapple_pslist_elf_read_section(struct file *file,
					   struct mapple_pslist_elf_capture *capture,
					   const struct mapple_pslist_elf_section *section,
					   u8 **bytes)
{
	int ret;

	if (section->size > MAPPLE_PSLIST_ELF_MAX_TABLE_SIZE)
		return -E2BIG;
	if (!section->size) {
		*bytes = NULL;
		return 0;
	}
	*bytes = kvmalloc(section->size, GFP_KERNEL);
	if (!*bytes)
		return -ENOMEM;
	ret = mapple_pslist_elf_read(file, section->offset, *bytes,
				     section->size, capture->file_size);
	if (ret) {
		kvfree(*bytes);
		*bytes = NULL;
	}
	return ret;
}

static int mapple_pslist_elf_capture_dynamic(
	struct file *file, struct mapple_pslist_elf_capture *capture,
	const struct mapple_pslist_elf_section *section)
{
	const struct mapple_pslist_elf_section *strings;
	u8 *dynamic_bytes = NULL;
	u8 *string_bytes = NULL;
	u64 entry_size = capture->elf_class == ELFCLASS64 ? sizeof(Elf64_Dyn) :
							       sizeof(Elf32_Dyn);
	u64 count;
	u64 index;
	int ret;

	if (section->link >= capture->section_count)
		return -ENOEXEC;
	strings = &capture->sections[section->link];
	if (strings->type != SHT_STRTAB ||
	    (section->entry_size && section->entry_size != entry_size) ||
	    section->size % entry_size)
		return -ENOEXEC;
	ret = mapple_pslist_elf_read_section(file, capture, strings, &string_bytes);
	if (ret)
		return ret;
	ret = mapple_pslist_elf_read_section(file, capture, section, &dynamic_bytes);
	if (ret)
		goto release;
	count = section->size / entry_size;
	for (index = 0; index < count; ++index) {
		s64 tag;
		u64 value;
		u16 kind = 0;

		if (capture->elf_class == ELFCLASS64) {
			const Elf64_Dyn *entry = (const Elf64_Dyn *)(dynamic_bytes +
								 index * entry_size);

			tag = mapple_pslist_elf_u64(capture, &entry->d_tag);
			value = mapple_pslist_elf_u64(capture, &entry->d_un.d_val);
		} else {
			const Elf32_Dyn *entry = (const Elf32_Dyn *)(dynamic_bytes +
								 index * entry_size);

			tag = (s32)mapple_pslist_elf_u32(capture, &entry->d_tag);
			value = mapple_pslist_elf_u32(capture, &entry->d_un.d_val);
		}
		if (tag == DT_NULL)
			break;
		switch (tag) {
		case DT_NEEDED: kind = MAPPLE_PSLIST_ELF_STRING_NEEDED; break;
		case DT_SONAME: kind = MAPPLE_PSLIST_ELF_STRING_SONAME; break;
		case DT_RPATH: kind = MAPPLE_PSLIST_ELF_STRING_RPATH; break;
		case MAPPLE_ELF_DT_RUNPATH:
			kind = MAPPLE_PSLIST_ELF_STRING_RUNPATH;
			break;
		default: break;
		}
		if (!kind)
			continue;
		ret = mapple_pslist_elf_add_table_string(
			capture, kind, string_bytes, strings->size, value);
		if (ret)
			break;
	}

release:
	kvfree(dynamic_bytes);
	kvfree(string_bytes);
	return ret;
}

static int mapple_pslist_elf_capture_symbols(
	struct file *file, struct mapple_pslist_elf_capture *capture,
	const struct mapple_pslist_elf_section *section)
{
	const struct mapple_pslist_elf_section *strings;
	u64 entry_size = capture->elf_class == ELFCLASS64 ? sizeof(Elf64_Sym) :
							       sizeof(Elf32_Sym);
	u8 *symbol_bytes = NULL;
	u8 *string_bytes = NULL;
	u64 count;
	u64 index;
	int ret;

	if (section->link >= capture->section_count)
		return -ENOEXEC;
	strings = &capture->sections[section->link];
	if (strings->type != SHT_STRTAB ||
	    (section->entry_size && section->entry_size != entry_size) ||
	    section->size % entry_size)
		return -ENOEXEC;
	count = section->size / entry_size;
	if (count > MAPPLE_PSLIST_ELF_MAX_DYNAMIC_SYMBOLS)
		return -E2BIG;
	ret = mapple_pslist_elf_read_section(file, capture, strings, &string_bytes);
	if (ret)
		return ret;
	ret = mapple_pslist_elf_read_section(file, capture, section, &symbol_bytes);
	if (ret)
		goto release;
	for (index = 0; index < count; ++index) {
		u32 name_offset;
		u16 section_index;
		u8 info;
		u8 visibility;
		u8 binding;
		u8 type;
		u16 kind;
		u64 value;
		u64 size;
		const u8 *end;
		size_t name_length;

		if (capture->elf_class == ELFCLASS64) {
			const Elf64_Sym *symbol = (const Elf64_Sym *)(symbol_bytes +
								 index * entry_size);

			name_offset = mapple_pslist_elf_u32(capture, &symbol->st_name);
			section_index = mapple_pslist_elf_u16(capture, &symbol->st_shndx);
			info = symbol->st_info;
			visibility = symbol->st_other & 0x3;
			value = mapple_pslist_elf_u64(capture, &symbol->st_value);
			size = mapple_pslist_elf_u64(capture, &symbol->st_size);
		} else {
			const Elf32_Sym *symbol = (const Elf32_Sym *)(symbol_bytes +
								 index * entry_size);

			name_offset = mapple_pslist_elf_u32(capture, &symbol->st_name);
			section_index = mapple_pslist_elf_u16(capture, &symbol->st_shndx);
			info = symbol->st_info;
			visibility = symbol->st_other & 0x3;
			value = mapple_pslist_elf_u32(capture, &symbol->st_value);
			size = mapple_pslist_elf_u32(capture, &symbol->st_size);
		}
		binding = info >> 4;
		type = info & 0xf;
		if (binding != STB_GLOBAL && binding != STB_WEAK && binding != 10)
			continue;
		if (section_index == SHN_UNDEF) {
			kind = MAPPLE_PSLIST_ELF_SYMBOL_IMPORT;
		} else if (visibility == MAPPLE_ELF_STV_DEFAULT ||
			   visibility == MAPPLE_ELF_STV_PROTECTED) {
			kind = MAPPLE_PSLIST_ELF_SYMBOL_EXPORT;
		} else {
			continue;
		}
		if (name_offset >= strings->size) {
			ret = -ENOEXEC;
			break;
		}
		end = memchr(string_bytes + name_offset, '\0',
			     strings->size - name_offset);
		if (!end) {
			ret = -ENOEXEC;
			break;
		}
		name_length = end - (string_bytes + name_offset);
		ret = mapple_pslist_elf_add_symbol(
			capture, string_bytes + name_offset, name_length, kind,
			binding, type, visibility, section_index, value, size);
		if (ret)
			break;
	}

release:
	kvfree(symbol_bytes);
	kvfree(string_bytes);
	return ret;
}

static int mapple_pslist_elf_capture_build_id(
	struct file *file, struct mapple_pslist_elf_capture *capture,
	const struct mapple_pslist_elf_section *section)
{
	u8 *bytes = NULL;
	u64 offset = 0;
	int ret = 0;

	ret = mapple_pslist_elf_read_section(file, capture, section, &bytes);
	if (ret)
		return ret;
	while (offset + sizeof(Elf32_Nhdr) <= section->size) {
		const Elf32_Nhdr *note = (const Elf32_Nhdr *)(bytes + offset);
		u32 name_size = mapple_pslist_elf_u32(capture, &note->n_namesz);
		u32 description_size = mapple_pslist_elf_u32(capture, &note->n_descsz);
		u32 type = mapple_pslist_elf_u32(capture, &note->n_type);
		u64 name_offset = offset + sizeof(*note);
		u64 description_offset = ALIGN(name_offset + name_size, 4);
		u64 next = ALIGN(description_offset + description_size, 4);

		if (name_offset > section->size || name_size > section->size - name_offset ||
		    description_offset > section->size ||
		    description_size > section->size - description_offset ||
		    next > section->size) {
			ret = -ENOEXEC;
			break;
		}
		if (type == MAPPLE_ELF_NT_GNU_BUILD_ID && name_size == 4 &&
		    !memcmp(bytes + name_offset, "GNU", 4)) {
			if (!description_size ||
			    description_size > MAPPLE_PSLIST_ELF_BUILD_ID_MAX_SIZE)
				ret = -E2BIG;
			else
				ret = mapple_pslist_elf_add_string(
					capture, MAPPLE_PSLIST_ELF_STRING_BUILD_ID,
					bytes + description_offset, description_size);
			break;
		}
		offset = next;
	}
	kvfree(bytes);
	return ret;
}

static int mapple_pslist_elf_capture_sections(struct file *file,
					       struct mapple_pslist_elf_capture *capture)
{
	bool has_build_id = false;
	u32 index;
	int ret;

	for (index = 0; index < capture->section_count; ++index) {
		const struct mapple_pslist_elf_section *section =
			&capture->sections[index];

		if (section->type == SHT_DYNAMIC) {
			ret = mapple_pslist_elf_capture_dynamic(file, capture, section);
			if (ret)
				return ret;
		} else if (section->type == SHT_DYNSYM) {
			ret = mapple_pslist_elf_capture_symbols(file, capture, section);
			if (ret)
				return ret;
		} else if (section->type == SHT_NOTE && !has_build_id) {
			u32 before = capture->string_count;

			ret = mapple_pslist_elf_capture_build_id(file, capture, section);
			if (ret)
				return ret;
			has_build_id = capture->string_count != before;
		}
	}
	return 0;
}

int mapple_pslist_elf_parse(struct file *file, struct mapple_pslist_elf_capture *capture)
{
	int ret;

	ret = mapple_pslist_elf_capture_header(file, capture);
	if (!ret)
		ret = mapple_pslist_elf_capture_interpreter(file, capture);
	if (!ret)
		ret = mapple_pslist_elf_capture_sections(file, capture);
	return ret;
}

void mapple_pslist_elf_discard_details(struct mapple_pslist_elf_capture *capture)
{
	u32 index;

	for (index = 0; index < capture->string_count; ++index)
		kfree(capture->strings[index].bytes);
	for (index = 0; index < capture->symbol_count; ++index)
		kfree(capture->symbols[index].name);
	kfree(capture->strings);
	kfree(capture->symbols);
	capture->strings = NULL;
	capture->symbols = NULL;
	capture->string_count = 0;
	capture->string_capacity = 0;
	capture->symbol_count = 0;
	capture->symbol_capacity = 0;
	capture->import_count = 0;
	capture->export_count = 0;
}

void mapple_pslist_elf_release_capture(struct mapple_pslist_elf_capture *capture)
{
	mapple_pslist_elf_discard_details(capture);
	kvfree(capture->sections);
}
