#ifndef MAPPLE_COMMAND_PSLIST_ELF_PARSER_H
#define MAPPLE_COMMAND_PSLIST_ELF_PARSER_H

#include <linux/fs.h>

#include <commands.h>

struct mapple_pslist_elf_section;

struct mapple_pslist_elf_string {
	u16 kind;
	u32 length;
	u8 *bytes;
};

struct mapple_pslist_elf_symbol {
	u16 kind;
	u8 binding;
	u8 type;
	u8 visibility;
	u16 section_index;
	u32 name_length;
	u8 *name;
	u64 value;
	u64 size;
};

struct mapple_pslist_elf_capture {
	struct mapple_pslist_elf_metadata_record metadata;
	struct mapple_pslist_elf_string *strings;
	struct mapple_pslist_elf_symbol *symbols;
	struct mapple_pslist_elf_section *sections;
	u32 string_count;
	u32 string_capacity;
	u32 symbol_count;
	u32 symbol_capacity;
	u32 import_count;
	u32 export_count;
	u32 section_count;
	u8 elf_class;
	u8 data_encoding;
	u16 status;
	u32 error;
	u64 file_size;
};

int mapple_pslist_elf_parse(struct file *file, struct mapple_pslist_elf_capture *capture);
void mapple_pslist_elf_discard_details(struct mapple_pslist_elf_capture *capture);
void mapple_pslist_elf_release_capture(struct mapple_pslist_elf_capture *capture);

#endif
