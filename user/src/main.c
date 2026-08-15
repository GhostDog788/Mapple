#include "commands.h"
#include "shared.h"

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

#define ECHO_MESSAGE "Hello server!"
#define ECHO_PAYLOAD_SIZE (sizeof(ECHO_MESSAGE) - 1U)

_Static_assert(sizeof(struct mapple_wire_header) == MAPPLE_WIRE_HEADER_SIZE,
	       "mapple wire header must be 16 bytes");

static int write_all(int fd, const void *buffer, size_t size)
{
	const uint8_t *bytes = buffer;
	size_t done = 0;

	while (done < size) {
		ssize_t ret = write(fd, bytes + done, size - done);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (ret == 0) {
			errno = EPIPE;
			return -1;
		}
		done += (size_t)ret;
	}
	return 0;
}

static int read_exact(int fd, void *buffer, size_t size)
{
	uint8_t *bytes = buffer;
	size_t done = 0;

	while (done < size) {
		ssize_t ret = read(fd, bytes + done, size - done);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (ret == 0) {
			errno = ECONNRESET;
			return -1;
		}
		done += (size_t)ret;
	}
	return 0;
}

static void make_header(struct mapple_wire_header *header, uint16_t command,
			uint64_t payload_size)
{
	header->magic = htonl(MAPPLE_PROTOCOL_MAGIC);
	header->version = htons(MAPPLE_PROTOCOL_VERSION);
	header->command = htons(command);
	header->payload_size = htobe64(payload_size);
}

static int verify_echo(int fd)
{
	struct mapple_wire_header request;
	struct mapple_wire_header response;
	const char payload[] = ECHO_MESSAGE;
	char echoed[ECHO_PAYLOAD_SIZE];

	make_header(&request, MAPPLE_COMMAND_ECHO, ECHO_PAYLOAD_SIZE);

	if (write_all(fd, &request, sizeof(request)) ||
	    write_all(fd, payload, ECHO_PAYLOAD_SIZE) ||
	    read_exact(fd, &response, sizeof(response)))
		return -1;

	if (ntohl(response.magic) != MAPPLE_PROTOCOL_MAGIC ||
	    ntohs(response.version) != MAPPLE_PROTOCOL_VERSION ||
	    ntohs(response.command) != MAPPLE_COMMAND_ECHO ||
	    be64toh(response.payload_size) != ECHO_PAYLOAD_SIZE) {
		errno = EPROTO;
		return -1;
	}
	if (read_exact(fd, echoed, ECHO_PAYLOAD_SIZE))
		return -1;
	if (memcmp(payload, echoed, ECHO_PAYLOAD_SIZE) != 0) {
		errno = EBADMSG;
		return -1;
	}
	return 0;
}

static int open_tcp(void)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = htons(MAPPLE_TCP_PORT),
	};
	int fd;

	if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
		errno = EINVAL;
		return -1;
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}
	return fd;
}

static int open_chardev(void)
{
	return open(MAPPLE_DEVICE_PATH, O_RDWR);
}

static bool run_transport(const char *name, int (*open_transport)(void))
{
	int fd = open_transport();
	int saved_errno;

	if (fd < 0) {
		fprintf(stderr, "%s: open failed: %s\n", name, strerror(errno));
		return false;
	}
	if (verify_echo(fd) < 0) {
		saved_errno = errno;
		close(fd);
		fprintf(stderr, "%s: ECHO failed: %s\n", name, strerror(saved_errno));
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
