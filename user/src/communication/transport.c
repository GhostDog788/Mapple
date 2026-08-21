#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <communication/transport.h>
#include <shared.h>

int open_tcp(int *fd_out)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = htons(MAPPLE_TCP_PORT),
	};
	int ret;
	int fd;

	ret = inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
	if (ret != 1)
		return ret == 0 ? EINVAL : errno;
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return errno;
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		ret = errno;

		close(fd);
		return ret;
	}
	*fd_out = fd;
	return 0;
}

int open_chardev(int *fd_out)
{
	int fd = open(MAPPLE_DEVICE_PATH, O_RDWR);

	if (fd < 0)
		return errno;
	*fd_out = fd;
	return 0;
}
