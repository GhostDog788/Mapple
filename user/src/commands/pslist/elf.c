#include <commands/pslist/elf.h>

#include <arpa/inet.h>
#include <elf.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <commands/pslist/json.h>

static __u16 mapple_pslist_elf_read_u16(const void *record, size_t offset)
{
	__u16 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohs(value);
}

static __u32 mapple_pslist_elf_read_u32(const void *record, size_t offset)
{
	__u32 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return ntohl(value);
}

static __u64 mapple_pslist_elf_read_u64(const void *record, size_t offset)
{
	__u64 value;

	memcpy(&value, (const unsigned char *)record + offset, sizeof(value));
	return be64toh(value);
}

static int mapple_pslist_elf_grow(void **entries, size_t *capacity, size_t count, size_t entry_size,
								  size_t limit)
{
	size_t new_capacity;
	void *new_entries;

	if (count < *capacity)
		return 0;
	if (count >= limit)
		return EOVERFLOW;
	new_capacity = *capacity ? *capacity * 2 : 32;
	if (new_capacity < *capacity || new_capacity > limit)
		new_capacity = limit;
	new_entries = realloc(*entries, new_capacity * entry_size);
	if (!new_entries)
		return ENOMEM;
	*entries = new_entries;
	*capacity = new_capacity;
	return 0;
}

int mapple_pslist_decode_elf_metadata(const void *record, size_t size, struct mapple_pslist_elf *elf)
{
	const struct mapple_pslist_elf_metadata_record *wire = record;
	__u64 object_id;

	if (size != sizeof(*wire) || elf->has_metadata)
		return EPROTO;
	object_id =
		mapple_pslist_elf_read_u64(wire, offsetof(struct mapple_pslist_elf_metadata_record, object_id));
	if (object_id != elf->object_id)
		return EPROTO;
	elf->elf_class = wire->elf_class;
	elf->data_encoding = wire->data_encoding;
	elf->os_abi = wire->os_abi;
	elf->abi_version = wire->abi_version;
#define READ_U16(field)                                                                                      \
	elf->field = mapple_pslist_elf_read_u16(wire, offsetof(struct mapple_pslist_elf_metadata_record, field))
#define READ_U32(field)                                                                                      \
	elf->field = mapple_pslist_elf_read_u32(wire, offsetof(struct mapple_pslist_elf_metadata_record, field))
#define READ_U64(field)                                                                                      \
	elf->field = mapple_pslist_elf_read_u64(wire, offsetof(struct mapple_pslist_elf_metadata_record, field))
	READ_U16(object_type);
	READ_U16(machine);
	READ_U32(elf_version);
	READ_U32(flags);
	READ_U32(program_header_count);
	READ_U32(section_header_count);
	READ_U32(string_record_count);
	READ_U32(symbol_record_count);
	READ_U32(import_count);
	READ_U32(export_count);
	READ_U64(entry_address);
	READ_U64(program_header_offset);
	READ_U64(section_header_offset);
	READ_U16(program_header_entry_size);
	READ_U16(section_header_entry_size);
	READ_U16(section_name_index);
	READ_U32(file_device_major);
	READ_U32(file_device_minor);
	READ_U32(file_mode);
	READ_U64(file_inode);
	READ_U64(file_size);
	elf->file_mtime_ns = (__s64)mapple_pslist_elf_read_u64(
		wire, offsetof(struct mapple_pslist_elf_metadata_record, file_mtime_ns));
	elf->file_ctime_ns = (__s64)mapple_pslist_elf_read_u64(
		wire, offsetof(struct mapple_pslist_elf_metadata_record, file_ctime_ns));
	READ_U64(file_filesystem_magic);
#undef READ_U64
#undef READ_U32
#undef READ_U16
	if (wire->reserved || wire->reserved2 ||
		(elf->elf_class && elf->elf_class != ELFCLASS32 && elf->elf_class != ELFCLASS64) ||
		(elf->data_encoding && elf->data_encoding != ELFDATA2LSB && elf->data_encoding != ELFDATA2MSB) ||
		elf->program_header_count > MAPPLE_PSLIST_ELF_MAX_PROGRAM_HEADERS ||
		elf->section_header_count > MAPPLE_PSLIST_ELF_MAX_SECTION_HEADERS ||
		elf->string_record_count > MAPPLE_PSLIST_ELF_MAX_DYNAMIC_STRINGS ||
		elf->symbol_record_count > MAPPLE_PSLIST_ELF_MAX_DYNAMIC_SYMBOLS ||
		elf->import_count > elf->symbol_record_count || elf->export_count > elf->symbol_record_count ||
		(__u64)elf->import_count + elf->export_count != elf->symbol_record_count)
		return EPROTO;
	elf->has_metadata = 1;
	return 0;
}

static int mapple_pslist_elf_string_kind_valid(__u16 kind)
{
	return kind >= MAPPLE_PSLIST_ELF_STRING_INTERPRETER && kind <= MAPPLE_PSLIST_ELF_STRING_BUILD_ID;
}

int mapple_pslist_decode_elf_string(const void *record, size_t size, struct mapple_pslist_elf *elf)
{
	const struct mapple_pslist_elf_string_record *wire = record;
	const unsigned char *bytes = (const unsigned char *)record + sizeof(*wire);
	struct mapple_pslist_elf_string *string;
	__u64 object_id;
	__u32 index;
	__u32 length;
	__u16 kind;
	int ret;

	if (size < sizeof(*wire) || !elf->has_metadata || elf->string_count >= elf->string_record_count)
		return EPROTO;
	object_id = mapple_pslist_elf_read_u64(wire, offsetof(struct mapple_pslist_elf_string_record, object_id));
	index = mapple_pslist_elf_read_u32(wire, offsetof(struct mapple_pslist_elf_string_record, string_index));
	kind = mapple_pslist_elf_read_u16(wire, offsetof(struct mapple_pslist_elf_string_record, string_kind));
	length = mapple_pslist_elf_read_u32(wire, offsetof(struct mapple_pslist_elf_string_record, byte_length));
	if (object_id != elf->object_id || index != elf->string_count + 1 ||
		!mapple_pslist_elf_string_kind_valid(kind) || wire->reserved || wire->reserved2 || !length ||
		sizeof(*wire) + (__u64)length != size ||
		length > (kind == MAPPLE_PSLIST_ELF_STRING_BUILD_ID ? MAPPLE_PSLIST_ELF_BUILD_ID_MAX_SIZE
															: MAPPLE_PSLIST_ELF_STRING_MAX_SIZE) ||
		(kind != MAPPLE_PSLIST_ELF_STRING_BUILD_ID && memchr(bytes, '\0', length)))
		return EPROTO;
	ret = mapple_pslist_elf_grow((void **)&elf->strings, &elf->string_capacity, elf->string_count,
								 sizeof(*elf->strings), MAPPLE_PSLIST_ELF_MAX_DYNAMIC_STRINGS);
	if (ret)
		return ret;
	string = &elf->strings[elf->string_count];
	memset(string, 0, sizeof(*string));
	string->bytes = malloc(length);
	if (!string->bytes)
		return ENOMEM;
	memcpy(string->bytes, bytes, length);
	string->kind = kind;
	string->length = length;
	++elf->string_count;
	return 0;
}

int mapple_pslist_decode_elf_symbol(const void *record, size_t size, struct mapple_pslist_elf *elf)
{
	const struct mapple_pslist_elf_symbol_record *wire = record;
	const unsigned char *name = (const unsigned char *)record + sizeof(*wire);
	struct mapple_pslist_elf_symbol *symbol;
	__u64 object_id;
	__u32 index;
	__u32 name_length;
	__u16 kind;
	int ret;

	if (size < sizeof(*wire) || !elf->has_metadata || elf->symbol_count >= elf->symbol_record_count)
		return EPROTO;
	object_id = mapple_pslist_elf_read_u64(wire, offsetof(struct mapple_pslist_elf_symbol_record, object_id));
	index = mapple_pslist_elf_read_u32(wire, offsetof(struct mapple_pslist_elf_symbol_record, symbol_index));
	kind = mapple_pslist_elf_read_u16(wire, offsetof(struct mapple_pslist_elf_symbol_record, symbol_kind));
	name_length =
		mapple_pslist_elf_read_u32(wire, offsetof(struct mapple_pslist_elf_symbol_record, name_length));
	if (object_id != elf->object_id || index != elf->symbol_count + 1 ||
		(kind != MAPPLE_PSLIST_ELF_SYMBOL_IMPORT && kind != MAPPLE_PSLIST_ELF_SYMBOL_EXPORT) ||
		(wire->binding != STB_GLOBAL && wire->binding != STB_WEAK && wire->binding != 10) ||
		wire->visibility > STV_PROTECTED || wire->reserved || wire->reserved2 || !name_length ||
		name_length > MAPPLE_PSLIST_ELF_STRING_MAX_SIZE || sizeof(*wire) + (__u64)name_length != size ||
		memchr(name, '\0', name_length))
		return EPROTO;
	ret = mapple_pslist_elf_grow((void **)&elf->symbols, &elf->symbol_capacity, elf->symbol_count,
								 sizeof(*elf->symbols), MAPPLE_PSLIST_ELF_MAX_DYNAMIC_SYMBOLS);
	if (ret)
		return ret;
	symbol = &elf->symbols[elf->symbol_count];
	memset(symbol, 0, sizeof(*symbol));
	symbol->name = malloc(name_length);
	if (!symbol->name)
		return ENOMEM;
	memcpy(symbol->name, name, name_length);
	symbol->kind = kind;
	symbol->binding = wire->binding;
	symbol->type = wire->symbol_type;
	symbol->visibility = wire->visibility;
	symbol->section_index =
		mapple_pslist_elf_read_u16(wire, offsetof(struct mapple_pslist_elf_symbol_record, section_index));
	symbol->name_length = name_length;
	symbol->value = mapple_pslist_elf_read_u64(wire, offsetof(struct mapple_pslist_elf_symbol_record, value));
	symbol->size = mapple_pslist_elf_read_u64(wire, offsetof(struct mapple_pslist_elf_symbol_record, size));
	if ((kind == MAPPLE_PSLIST_ELF_SYMBOL_IMPORT && symbol->section_index) ||
		(kind == MAPPLE_PSLIST_ELF_SYMBOL_EXPORT && !symbol->section_index)) {
		free(symbol->name);
		memset(symbol, 0, sizeof(*symbol));
		return EPROTO;
	}
	++elf->symbol_count;
	return 0;
}

int mapple_pslist_finish_elf(struct mapple_pslist_elf *elf, __u16 status, __u32 error, __u32 record_count)
{
	if (!elf->has_metadata || elf->status || record_count != 1 + elf->string_count + elf->symbol_count)
		return EPROTO;
	if (status == MAPPLE_PSLIST_OBJECT_COMPLETE) {
		if (error || !elf->elf_class || !elf->data_encoding ||
			elf->string_count != elf->string_record_count || elf->symbol_count != elf->symbol_record_count)
			return EPROTO;
	} else if (!error || elf->string_count || elf->symbol_count || elf->string_record_count ||
			   elf->symbol_record_count || elf->import_count || elf->export_count) {
		return EPROTO;
	}
	elf->status = status;
	elf->error = error;
	return 0;
}

void mapple_pslist_free_elf(struct mapple_pslist_elf *elf)
{
	size_t index;

	for (index = 0; index < elf->string_count; ++index)
		free(elf->strings[index].bytes);
	for (index = 0; index < elf->symbol_count; ++index)
		free(elf->symbols[index].name);
	free(elf->strings);
	free(elf->symbols);
	free(elf->mapped_file_ids);
	memset(elf, 0, sizeof(*elf));
}

static const char *mapple_pslist_elf_status_name(__u16 status)
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

static const char *mapple_pslist_elf_class_name(__u8 value)
{
	switch (value) {
	case ELFCLASS32:
		return "ELF32";
	case ELFCLASS64:
		return "ELF64";
	default:
		return "unknown";
	}
}

static const char *mapple_pslist_elf_data_name(__u8 value)
{
	switch (value) {
	case ELFDATA2LSB:
		return "little_endian";
	case ELFDATA2MSB:
		return "big_endian";
	default:
		return "unknown";
	}
}

static const char *mapple_pslist_elf_object_type_name(__u16 value)
{
	switch (value) {
	case ET_NONE:
		return "none";
	case ET_REL:
		return "relocatable";
	case ET_EXEC:
		return "executable";
	case ET_DYN:
		return "shared_object";
	case ET_CORE:
		return "core";
	default:
		return "processor_or_os_specific";
	}
}

static const char *mapple_pslist_elf_machine_name(__u16 value)
{
	switch (value) {
	case EM_386:
		return "x86";
	case EM_X86_64:
		return "x86_64";
	case EM_ARM:
		return "arm";
	case EM_AARCH64:
		return "aarch64";
	case EM_RISCV:
		return "riscv";
	default:
		return "unknown";
	}
}

static const char *mapple_pslist_elf_binding_name(__u8 value)
{
	switch (value) {
	case STB_GLOBAL:
		return "global";
	case STB_WEAK:
		return "weak";
	case 10:
		return "gnu_unique";
	default:
		return "unknown";
	}
}

static const char *mapple_pslist_elf_symbol_type_name(__u8 value)
{
	switch (value) {
	case STT_NOTYPE:
		return "notype";
	case STT_OBJECT:
		return "object";
	case STT_FUNC:
		return "function";
	case STT_SECTION:
		return "section";
	case STT_FILE:
		return "file";
	case STT_COMMON:
		return "common";
	case STT_TLS:
		return "tls";
	case 10:
		return "gnu_ifunc";
	default:
		return "unknown";
	}
}

static const char *mapple_pslist_elf_visibility_name(__u8 value)
{
	switch (value) {
	case STV_DEFAULT:
		return "default";
	case STV_INTERNAL:
		return "internal";
	case STV_HIDDEN:
		return "hidden";
	case STV_PROTECTED:
		return "protected";
	default:
		return "unknown";
	}
}

static int mapple_pslist_elf_print_error(__u32 error)
{
	if (!error)
		return fputs("null", stdout) == EOF ? EIO : 0;
	if (printf("{\"code\": %u, \"message\": ", error) < 0)
		return EIO;
	if (mapple_pslist_print_json_bytes((const unsigned char *)strerror(error), strlen(strerror(error))))
		return EIO;
	return putchar('}') == EOF ? EIO : 0;
}

static int mapple_pslist_elf_print_hex(const unsigned char *bytes, size_t length)
{
	size_t index;

	if (putchar('"') == EOF)
		return EIO;
	for (index = 0; index < length; ++index)
		if (printf("%02x", bytes[index]) < 0)
			return EIO;
	return putchar('"') == EOF ? EIO : 0;
}

static int mapple_pslist_elf_print_strings(const struct mapple_pslist_elf *elf, __u16 kind, int hex)
{
	size_t index;
	int first = 1;
	int ret;

	if (putchar('[') == EOF)
		return EIO;
	for (index = 0; index < elf->string_count; ++index) {
		const struct mapple_pslist_elf_string *string = &elf->strings[index];

		if (string->kind != kind)
			continue;
		if (!first && fputs(", ", stdout) == EOF)
			return EIO;
		ret = hex ? mapple_pslist_elf_print_hex(string->bytes, string->length)
				  : mapple_pslist_print_json_bytes(string->bytes, string->length);
		if (ret)
			return ret;
		first = 0;
	}
	return putchar(']') == EOF ? EIO : 0;
}

static int mapple_pslist_elf_compare_symbol_pointers(const void *left, const void *right)
{
	const struct mapple_pslist_elf_symbol *first = *(const struct mapple_pslist_elf_symbol *const *)left;
	const struct mapple_pslist_elf_symbol *second = *(const struct mapple_pslist_elf_symbol *const *)right;

	if (first->type != second->type)
		return first->type < second->type ? -1 : 1;
	if (first->binding != second->binding)
		return first->binding < second->binding ? -1 : 1;
	if (first->visibility != second->visibility)
		return first->visibility < second->visibility ? -1 : 1;
	if (first->section_index != second->section_index)
		return first->section_index < second->section_index ? -1 : 1;
	return first < second ? -1 : first > second;
}

static int mapple_pslist_elf_symbols_same_group(const struct mapple_pslist_elf_symbol *left,
												const struct mapple_pslist_elf_symbol *right)
{
	return left->type == right->type && left->binding == right->binding &&
		   left->visibility == right->visibility && left->section_index == right->section_index;
}

static int mapple_pslist_elf_print_symbol(const struct mapple_pslist_elf_symbol *symbol, __u16 kind,
										  int indent)
{
	int ret;

	ret = mapple_pslist_print_json_indent(indent);
	if (ret || fputs("{\"name\": ", stdout) == EOF)
		return ret ? ret : EIO;
	ret = mapple_pslist_print_json_bytes(symbol->name, symbol->name_length);
	if (ret)
		return ret;
	if (kind == MAPPLE_PSLIST_ELF_SYMBOL_EXPORT || symbol->value)
		if (printf(", \"value\": \"0x%" PRIx64 "\"", (uint64_t)symbol->value) < 0)
			return EIO;
	if (kind == MAPPLE_PSLIST_ELF_SYMBOL_EXPORT || symbol->size)
		if (printf(", \"size_bytes\": %" PRIu64, (uint64_t)symbol->size) < 0)
			return EIO;
	return putchar('}') == EOF ? EIO : 0;
}

static int mapple_pslist_elf_print_symbol_group(const struct mapple_pslist_elf_symbol *const *symbols,
												size_t count, __u16 kind, int indent)
{
	const struct mapple_pslist_elf_symbol *first = symbols[0];
	const char *binding = mapple_pslist_elf_binding_name(first->binding);
	const char *type = mapple_pslist_elf_symbol_type_name(first->type);
	const char *visibility = mapple_pslist_elf_visibility_name(first->visibility);
	size_t index;
	int ret;

	ret = mapple_pslist_print_json_indent(indent);
	if (ret)
		return ret;
	if (!binding || !type || !visibility)
		return EPROTO;
	if (printf("{\"type\": \"%s\", \"binding\": \"%s\", "
			   "\"visibility\": \"%s\"",
			   type, binding, visibility) < 0)
		return EIO;
	if (kind == MAPPLE_PSLIST_ELF_SYMBOL_EXPORT &&
		printf(", \"section_index\": %u", first->section_index) < 0)
		return EIO;
	if (fputs(", \"symbols\": [\n", stdout) == EOF)
		return EIO;
	for (index = 0; index < count; ++index) {
		ret = mapple_pslist_elf_print_symbol(symbols[index], kind, indent + 2);
		if (ret || fputs(index + 1 == count ? "\n" : ",\n", stdout) == EOF)
			return ret ? ret : EIO;
	}
	ret = mapple_pslist_print_json_indent(indent);
	if (ret)
		return ret;
	return fputs("]}", stdout) == EOF ? EIO : 0;
}

static int mapple_pslist_elf_print_symbols(const struct mapple_pslist_elf *elf, __u16 kind, int indent)
{
	const struct mapple_pslist_elf_symbol **symbols;
	size_t count = 0;
	size_t group_start;
	size_t index;
	int ret = 0;

	for (index = 0; index < elf->symbol_count; ++index)
		if (elf->symbols[index].kind == kind)
			++count;
	if (!count)
		return fputs("{\"groups\": []}", stdout) == EOF ? EIO : 0;
	symbols = malloc(count * sizeof(*symbols));
	if (!symbols)
		return ENOMEM;
	count = 0;
	for (index = 0; index < elf->symbol_count; ++index)
		if (elf->symbols[index].kind == kind)
			symbols[count++] = &elf->symbols[index];
	qsort(symbols, count, sizeof(*symbols), mapple_pslist_elf_compare_symbol_pointers);
	if (fputs("{\"groups\": [\n", stdout) == EOF) {
		ret = EIO;
		goto free_symbols;
	}
	for (group_start = 0; group_start < count;) {
		size_t group_end = group_start + 1;

		while (group_end < count &&
			   mapple_pslist_elf_symbols_same_group(symbols[group_start], symbols[group_end]))
			++group_end;
		ret = mapple_pslist_elf_print_symbol_group(&symbols[group_start], group_end - group_start, kind,
												   indent + 2);
		if (ret || fputs(group_end == count ? "\n" : ",\n", stdout) == EOF) {
			if (!ret)
				ret = EIO;
			goto free_symbols;
		}
		group_start = group_end;
	}
	ret = mapple_pslist_print_json_indent(indent);
	if (!ret && fputs("]}", stdout) == EOF)
		ret = EIO;

free_symbols:
	free(symbols);
	return ret;
}

static int mapple_pslist_elf_print_mapped_files(const struct mapple_pslist_elf *elf)
{
	size_t index;

	if (putchar('[') == EOF)
		return EIO;
	for (index = 0; index < elf->mapped_file_count; ++index)
		if (printf("%s%" PRIu64, index ? ", " : "", (uint64_t)elf->mapped_file_ids[index]) < 0)
			return EIO;
	return putchar(']') == EOF ? EIO : 0;
}

int mapple_pslist_print_json_elf(const struct mapple_pslist_elf *elf, int indent)
{
	const char *status = mapple_pslist_elf_status_name(elf->status);
	int ret;

	if (!status || printf("{\n%*s\"collection_status\": \"%s\",\n%*s\"error\": ", indent + 2, "", status,
						  indent + 2, "") < 0)
		return EPROTO;
	ret = mapple_pslist_elf_print_error(elf->error);
	if (ret ||
		printf(",\n%*s\"header\": {\"class\": \"%s\", "
			   "\"data_encoding\": \"%s\", \"os_abi\": %u, "
			   "\"abi_version\": %u, \"object_type\": \"%s\", "
			   "\"object_type_value\": %u, \"machine\": \"%s\", "
			   "\"machine_value\": %u, \"version\": %u, "
			   "\"flags\": \"0x%08x\", \"entry_address\": \"0x%016" PRIx64
			   "\", \"program_headers\": {\"offset\": %" PRIu64 ", \"entry_size\": %u, \"count\": %u}, "
			   "\"section_headers\": {\"offset\": %" PRIu64 ", \"entry_size\": %u, \"count\": %u, "
			   "\"name_index\": %u}},\n",
			   indent + 2, "", mapple_pslist_elf_class_name(elf->elf_class),
			   mapple_pslist_elf_data_name(elf->data_encoding), elf->os_abi, elf->abi_version,
			   mapple_pslist_elf_object_type_name(elf->object_type), elf->object_type,
			   mapple_pslist_elf_machine_name(elf->machine), elf->machine, elf->elf_version, elf->flags,
			   (uint64_t)elf->entry_address, (uint64_t)elf->program_header_offset,
			   elf->program_header_entry_size, elf->program_header_count,
			   (uint64_t)elf->section_header_offset, elf->section_header_entry_size,
			   elf->section_header_count, elf->section_name_index) < 0)
		return ret ? ret : EIO;
	if (printf("%*s\"file_identity\": {\"device\": {\"major\": %u, "
			   "\"minor\": %u}, \"inode\": %" PRIu64 ", \"mode\": %u, "
			   "\"size\": %" PRIu64 ", \"mtime_ns\": %" PRId64 ", \"ctime_ns\": %" PRId64
			   ", \"filesystem_magic\": "
			   "\"0x%016" PRIx64 "\"},\n%*s\"mapped_files\": ",
			   indent + 2, "", elf->file_device_major, elf->file_device_minor, (uint64_t)elf->file_inode,
			   elf->file_mode, (uint64_t)elf->file_size, (int64_t)elf->file_mtime_ns,
			   (int64_t)elf->file_ctime_ns, (uint64_t)elf->file_filesystem_magic, indent + 2, "") < 0)
		return EIO;
	ret = mapple_pslist_elf_print_mapped_files(elf);
	if (ret || printf(",\n%*s\"interpreter\": ", indent + 2, "") < 0)
		return ret ? ret : EIO;
	ret = mapple_pslist_elf_print_strings(elf, MAPPLE_PSLIST_ELF_STRING_INTERPRETER, 0);
#define PRINT_STRINGS(name, kind, hex)                                                                       \
	do {                                                                                                     \
		if (!ret && printf(",\n%*s\"%s\": ", indent + 2, "", (name)) < 0)                                    \
			ret = EIO;                                                                                       \
		if (!ret)                                                                                            \
			ret = mapple_pslist_elf_print_strings(elf, (kind), (hex));                                       \
	} while (0)
	PRINT_STRINGS("needed", MAPPLE_PSLIST_ELF_STRING_NEEDED, 0);
	PRINT_STRINGS("soname", MAPPLE_PSLIST_ELF_STRING_SONAME, 0);
	PRINT_STRINGS("rpath", MAPPLE_PSLIST_ELF_STRING_RPATH, 0);
	PRINT_STRINGS("runpath", MAPPLE_PSLIST_ELF_STRING_RUNPATH, 0);
	PRINT_STRINGS("build_ids", MAPPLE_PSLIST_ELF_STRING_BUILD_ID, 1);
#undef PRINT_STRINGS
	if (!ret && printf(",\n%*s\"imports\": ", indent + 2, "") < 0)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_elf_print_symbols(elf, MAPPLE_PSLIST_ELF_SYMBOL_IMPORT, indent + 2);
	if (!ret && printf(",\n%*s\"exports\": ", indent + 2, "") < 0)
		ret = EIO;
	if (!ret)
		ret = mapple_pslist_elf_print_symbols(elf, MAPPLE_PSLIST_ELF_SYMBOL_EXPORT, indent + 2);
	if (!ret && printf("\n%*s}", indent, "") < 0)
		ret = EIO;
	return ret;
}
