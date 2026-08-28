#ifndef COMMAND_PSLIST_MEMORY_MAPS_H
#define COMMAND_PSLIST_MEMORY_MAPS_H

#include <stddef.h>

#include <commands/pslist/model.h>

int mapple_pslist_decode_memory_maps_metadata(
	const void *record, size_t size, struct mapple_pslist_memory_maps *memory_maps);
int mapple_pslist_decode_memory_map(
	const void *record, size_t size, struct mapple_pslist_memory_maps *memory_maps);
void mapple_pslist_free_memory_maps(struct mapple_pslist_memory_maps *memory_maps);
int mapple_pslist_print_json_memory_maps(
	const struct mapple_pslist_memory_maps *memory_maps, int indent);

#endif /* COMMAND_PSLIST_MEMORY_MAPS_H */
