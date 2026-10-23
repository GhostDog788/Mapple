#ifndef MAPPLE_COMMAND_PSLIST_MEMORY_MAPS_H
#define MAPPLE_COMMAND_PSLIST_MEMORY_MAPS_H

#include <linux/mm_types.h>

#include <control/commands/pslist/elf/objects.h>
#include <control/commands/pslist/snapshot/stream.h>

int mapple_pslist_write_memory_maps_object(struct mapple_pslist_writer *writer,
					   struct mm_struct *mm, u64 object_id,
					   struct mapple_pslist_elf_registry *elf);

#endif /* MAPPLE_COMMAND_PSLIST_MEMORY_MAPS_H */
