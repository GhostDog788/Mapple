#ifndef COMMAND_PSLIST_ENVIRONMENT_H
#define COMMAND_PSLIST_ENVIRONMENT_H

#include <stddef.h>

#include <commands/pslist/model.h>

int mapple_pslist_decode_environment_record(
	const void *record, size_t size, struct mapple_pslist_environment *environment);
void mapple_pslist_free_environment(struct mapple_pslist_environment *environment);
int mapple_pslist_print_json_environment(
	const struct mapple_pslist_environment *environment, int indent);

#endif /* COMMAND_PSLIST_ENVIRONMENT_H */
