#include <communication/command.h>

#include <endian.h>
#include <errno.h>
#include <netinet/in.h>

#include <communication/request.h>
#include <communication/stream.h>

int send_command_header(int fd, uint16_t command, uint32_t flags, uint64_t payload_size)
{
	struct mapple_wire_header request;

	make_header(&request, command, flags, payload_size);
	return write_all(fd, &request, sizeof(request));
}

int send_command(int fd, uint16_t command, const void *data, size_t len)
{
	int ret;

	ret = send_command_header(fd, command, 0, len);
	if (ret)
		return ret;
	ret = write_all(fd, data, len);
	if (ret)
		return ret;
	return 0;
}

int receive_response_header(int fd, struct mapple_wire_header *header)
{
	uint64_t payload_size;
	uint32_t flags;
	uint32_t status;
	int ret;

	ret = read_exact(fd, header, sizeof(struct mapple_wire_header));
	if (ret)
		return ret;

	if (ntohl(header->magic) != MAPPLE_PROTOCOL_MAGIC || ntohs(header->version) != MAPPLE_PROTOCOL_VERSION)
		return EPROTO;
	status = ntohl(header->status);
	if (status != MAPPLE_STATUS_OK)
		return EPROTO;
	flags = ntohl(header->flags);
	payload_size = be64toh(header->payload_size);
	if (flags & ~MAPPLE_HEADER_KNOWN_FLAGS)
		return EPROTO;
	if ((flags & MAPPLE_HEADER_FLAG_STREAM) && payload_size)
		return EPROTO;

	return 0;
}
