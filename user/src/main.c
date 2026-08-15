#include "communication/helpers/command.h"
#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <commands.h>
#include <communication/helpers/request.h>
#include <communication/helpers/stream.h>
#include <communication/helpers/transport.h>
#include <shared.h>

#define ECHO_MESSAGE "Hello server!"
#define ECHO_PAYLOAD_SIZE (sizeof(ECHO_MESSAGE) - 1)

static int verify_echo(int fd)
{
	struct mapple_wire_header response;
	const char payload[] = ECHO_MESSAGE;
	char echoed[ECHO_PAYLOAD_SIZE];
	int ret;

	ret = send_command(fd, MAPPLE_COMMAND_ECHO, payload, ECHO_PAYLOAD_SIZE);
	if (ret)
		return ret;
	ret = receive_response_header(fd, &response);
	if (ret)
		return ret;

	if (ntohs(response.command) != MAPPLE_COMMAND_ECHO || be64toh(response.payload_size) != ECHO_PAYLOAD_SIZE)
		return EPROTO;
	ret = read_exact(fd, echoed, ECHO_PAYLOAD_SIZE);
	if (ret)
		return ret;
	if (memcmp(payload, echoed, ECHO_PAYLOAD_SIZE) != 0)
		return EBADMSG;
	return 0;
}

static bool run_transport(const char *name, int (*open_transport)(int *))
{
	int fd;
	int ret;

	ret = open_transport(&fd);
	if (ret) {
		fprintf(stderr, "%s: open failed: %s\n", name, strerror(ret));
		return false;
	}
	ret = verify_echo(fd);
	if (ret) {
		close(fd);
		fprintf(stderr, "%s: ECHO failed: %s\n", name, strerror(ret));
		return false;
	}
	close(fd);
	printf("%s: ECHO verified (%zu bytes)\n", name, ECHO_PAYLOAD_SIZE);
	return true;
}

int main(void)
{
	bool tcp_ok;
	bool chardev_ok;

	signal(SIGPIPE, SIG_IGN);
	tcp_ok = run_transport("TCP 127.0.0.1:4444", open_tcp);
	chardev_ok = run_transport("chardev /dev/mapple", open_chardev);
	return tcp_ok && chardev_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
