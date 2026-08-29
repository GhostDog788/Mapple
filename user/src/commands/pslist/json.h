#ifndef COMMAND_PSLIST_JSON_H
#define COMMAND_PSLIST_JSON_H

#include <stddef.h>

int mapple_pslist_print_json_bytes(const unsigned char *bytes, size_t length);
int mapple_pslist_print_json_indent(unsigned int width);

#endif /* COMMAND_PSLIST_JSON_H */
