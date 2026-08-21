#ifndef MAPPLE_USER_REQUEST_H
#define MAPPLE_USER_REQUEST_H

#include <stdint.h>

#include <commands.h>

void make_header(struct mapple_wire_header *header, uint16_t command, uint64_t payload_size);

#endif /* MAPPLE_USER_REQUEST_H */
