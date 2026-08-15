#ifndef REQUEST_HELPERS_H
#define REQUEST_HELPERS_H

#include <stdint.h>

#include <commands.h>

void make_header(struct mapple_wire_header *header, uint16_t command, uint64_t payload_size);

#endif /* REQUEST_HELPERS_H */
