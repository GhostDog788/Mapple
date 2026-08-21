#include <commands/echo/echo.h>

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <string.h>

#include <commands.h>
#include <communication/command.h>
#include <communication/stream.h>

#define ECHO_MESSAGE "Hello server!"
#define ECHO_PAYLOAD_SIZE (sizeof(ECHO_MESSAGE) - 1)

int run_echo(int session_fd)
{
	struct mapple_wire_header response;
	const char payload[] = ECHO_MESSAGE;
	char echoed[ECHO_PAYLOAD_SIZE];
	int ret;

	ret = send_command(session_fd, MAPPLE_COMMAND_ECHO, payload, ECHO_PAYLOAD_SIZE);
	if (ret)
		return ret;
	ret = receive_response_header(session_fd, &response);
	if (ret)
		return ret;

	if (ntohs(response.command) != MAPPLE_COMMAND_ECHO || be64toh(response.payload_size) != ECHO_PAYLOAD_SIZE)
		return EPROTO;
	ret = read_exact(session_fd, echoed, ECHO_PAYLOAD_SIZE);
	if (ret)
		return ret;
	if (memcmp(payload, echoed, ECHO_PAYLOAD_SIZE) != 0)
		return EBADMSG;
	return 0;
}
