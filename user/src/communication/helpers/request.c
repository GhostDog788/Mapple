#include <communication/helpers/request.h>
#include <netinet/in.h>

void make_header(struct mapple_wire_header *header, uint16_t command, uint64_t payload_size)
{
	header->magic = htonl(MAPPLE_PROTOCOL_MAGIC);
	header->version = htons(MAPPLE_PROTOCOL_VERSION);
	header->command = htons(command);
	header->payload_size = htobe64(payload_size);
}
