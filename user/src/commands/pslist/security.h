#ifndef COMMAND_PSLIST_SECURITY_H
#define COMMAND_PSLIST_SECURITY_H

#include <stddef.h>

#include <commands/pslist/model.h>

int mapple_pslist_decode_security_record(const void *record, size_t size,
					 struct mapple_pslist_security *security);
void mapple_pslist_free_security(struct mapple_pslist_security *security);
int mapple_pslist_print_json_security(const struct mapple_pslist_security *security,
				       int indent);

#endif /* COMMAND_PSLIST_SECURITY_H */
