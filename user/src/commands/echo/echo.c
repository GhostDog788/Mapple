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

static int run_sized_echo(int session_fd)
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

	if (ntohs(response.command) != MAPPLE_COMMAND_ECHO || ntohl(response.flags) ||
		be64toh(response.payload_size) != ECHO_PAYLOAD_SIZE)
		return EPROTO;
	ret = read_exact(session_fd, echoed, ECHO_PAYLOAD_SIZE);
	if (ret)
		return ret;
	if (memcmp(payload, echoed, ECHO_PAYLOAD_SIZE) != 0)
		return EBADMSG;
	return 0;
}

static int send_stream_chunk(int session_fd, const void *data, __u32 size)
{
	__u32 wire_size = htonl(size);
	int ret;

	ret = write_all(session_fd, &wire_size, sizeof(wire_size));
	if (ret || !size)
		return ret;
	return write_all(session_fd, data, size);
}

static int receive_stream_chunk(int session_fd, const void *expected, __u32 expected_size)
{
	char received[ECHO_PAYLOAD_SIZE];
	__u32 wire_size;
	__u32 size;
	int ret;

	ret = read_exact(session_fd, &wire_size, sizeof(wire_size));
	if (ret)
		return ret;
	size = ntohl(wire_size);
	if (size != expected_size || size > sizeof(received))
		return EPROTO;
	ret = read_exact(session_fd, received, size);
	if (ret)
		return ret;
	if (size && memcmp(expected, received, size) != 0)
		return EBADMSG;
	return 0;
}

static int run_stream_echo(int session_fd)
{
	static const char first_chunk[] = "Hello ";
	static const char second_chunk[] = "server!";
	struct mapple_wire_header response;
	int ret;

	ret = send_command_header(session_fd, MAPPLE_COMMAND_ECHO, MAPPLE_HEADER_FLAG_STREAM, 0);
	if (ret)
		return ret;
	ret = send_stream_chunk(session_fd, first_chunk, sizeof(first_chunk) - 1);
	if (ret)
		return ret;
	ret = send_stream_chunk(session_fd, second_chunk, sizeof(second_chunk) - 1);
	if (ret)
		return ret;
	ret = send_stream_chunk(session_fd, NULL, 0);
	if (ret)
		return ret;

	ret = receive_response_header(session_fd, &response);
	if (ret)
		return ret;
	if (ntohs(response.command) != MAPPLE_COMMAND_ECHO ||
		ntohl(response.flags) != MAPPLE_HEADER_FLAG_STREAM || be64toh(response.payload_size))
		return EPROTO;
	ret = receive_stream_chunk(session_fd, first_chunk, sizeof(first_chunk) - 1);
	if (ret)
		return ret;
	ret = receive_stream_chunk(session_fd, second_chunk, sizeof(second_chunk) - 1);
	if (ret)
		return ret;
	return receive_stream_chunk(session_fd, NULL, 0);
}

int run_echo(int session_fd)
{
	int ret;

	ret = run_sized_echo(session_fd);
	if (ret)
		return ret;
	return run_stream_echo(session_fd);
}
