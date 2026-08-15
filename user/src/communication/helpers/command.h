#ifndef COMMAND_HELPERS_H
#define COMMAND_HELPERS_H

#include <commands.h>

#include <stddef.h>
#include <stdint.h>

int send_command(int fd, uint16_t command, const void *data, size_t len);
int receive_response_header(int fd, struct mapple_wire_header *header);

#endif /* COMMAND_HELPERS_H */
