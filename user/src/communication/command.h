#ifndef MAPPLE_USER_COMMAND_H
#define MAPPLE_USER_COMMAND_H

#include <commands.h>

#include <stddef.h>
#include <stdint.h>

int send_command(int fd, uint16_t command, const void *data, size_t len);
int receive_response_header(int fd, struct mapple_wire_header *header);

#endif /* MAPPLE_USER_COMMAND_H */
