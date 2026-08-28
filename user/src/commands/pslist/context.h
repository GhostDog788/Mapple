#ifndef COMMAND_PSLIST_CONTEXT_H
#define COMMAND_PSLIST_CONTEXT_H

#include <stddef.h>

#include <commands/pslist/model.h>

int mapple_pslist_decode_context_record(const void *record, size_t size,
					struct mapple_pslist_context *context);
void mapple_pslist_free_context(struct mapple_pslist_context *context);
int mapple_pslist_print_json_context(const struct mapple_pslist_context *context,
				      int indent);

#endif /* COMMAND_PSLIST_CONTEXT_H */
