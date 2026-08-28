#include <communication/request.h>

#include <endian.h>
#include <netinet/in.h>

void make_header(struct mapple_wire_header *header, uint16_t command, uint32_t flags,
		 uint64_t payload_size)
{
	header->magic = htonl(MAPPLE_PROTOCOL_MAGIC);
	header->version = htons(MAPPLE_PROTOCOL_VERSION);
	header->command = htons(command);
	header->flags = htonl(flags);
	header->status = htonl(MAPPLE_STATUS_OK);
	header->payload_size = htobe64(payload_size);
}
