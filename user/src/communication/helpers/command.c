#include "communication/helpers/request.h"

#include <errno.h>
#include <netinet/in.h>

#include "communication/helpers/stream.h"
#include <communication/helpers/command.h>

int send_command(int fd, uint16_t command, const void *data, size_t len)
{
	int ret;
	struct mapple_wire_header request;

	make_header(&request, command, len);

	ret = write_all(fd, &request, sizeof(request));
	if (ret)
		return ret;
	ret = write_all(fd, data, len);
	if (ret)
		return ret;
	return 0;
}

int receive_response_header(int fd, struct mapple_wire_header *header)
{
	int ret;

	ret = read_exact(fd, header, sizeof(struct mapple_wire_header));
	if (ret)
		return ret;

	if (ntohl(header->magic) != MAPPLE_PROTOCOL_MAGIC || ntohs(header->version) != MAPPLE_PROTOCOL_VERSION)
		return EPROTO;

	return 0;
}
