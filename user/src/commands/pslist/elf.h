#ifndef COMMAND_PSLIST_ELF_H
#define COMMAND_PSLIST_ELF_H

#include <stddef.h>

#include <commands/pslist/model.h>

int mapple_pslist_decode_elf_metadata(const void *record, size_t size,
				       struct mapple_pslist_elf *elf);
int mapple_pslist_decode_elf_string(const void *record, size_t size,
				     struct mapple_pslist_elf *elf);
int mapple_pslist_decode_elf_symbol(const void *record, size_t size,
				     struct mapple_pslist_elf *elf);
int mapple_pslist_finish_elf(struct mapple_pslist_elf *elf, __u16 status,
			     __u32 error, __u32 record_count);
void mapple_pslist_free_elf(struct mapple_pslist_elf *elf);
int mapple_pslist_build_elf_symbol_names(struct mapple_pslist_snapshot *snapshot);
int mapple_pslist_print_json_elf_symbol_names(const struct mapple_pslist_snapshot *snapshot);
int mapple_pslist_print_json_elf(const struct mapple_pslist_elf *elf, int indent);

#endif /* COMMAND_PSLIST_ELF_H */
