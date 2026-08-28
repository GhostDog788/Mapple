#ifndef COMMAND_PSLIST_RESOURCES_H
#define COMMAND_PSLIST_RESOURCES_H

#include <stddef.h>

#include <commands/pslist/model.h>

int mapple_pslist_decode_resources_record(const void *record, size_t size,
					  struct mapple_pslist_resources *resources);
int mapple_pslist_print_json_resources(const struct mapple_pslist_resources *resources,
					int indent);

#endif /* COMMAND_PSLIST_RESOURCES_H */
