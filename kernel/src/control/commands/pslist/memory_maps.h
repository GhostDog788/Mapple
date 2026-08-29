#ifndef MAPPLE_COMMAND_PSLIST_MEMORY_MAPS_H
#define MAPPLE_COMMAND_PSLIST_MEMORY_MAPS_H

#include <linux/mm_types.h>

#include <control/commands/commands.h>
#include <control/commands/pslist/elf.h>

int mapple_pslist_write_memory_maps_object(struct mapple_command_context *context,
					    struct mm_struct *mm, u64 object_id,
					    struct mapple_pslist_elf_registry *elf,
					    u32 *object_count, bool *partial);

#endif /* MAPPLE_COMMAND_PSLIST_MEMORY_MAPS_H */
