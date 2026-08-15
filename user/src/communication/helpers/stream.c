#include <communication/helpers/stream.h>

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

int write_all(int fd, const void *buffer, size_t size)
{
	const uint8_t *bytes = buffer;
	size_t done = 0;

	while (done < size) {
		ssize_t ret = write(fd, bytes + done, size - done);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (ret == 0) {
			return EPIPE;
		}
		done += (size_t)ret;
	}
	return 0;
}

int read_exact(int fd, void *buffer, size_t size)
{
	uint8_t *bytes = buffer;
	size_t done = 0;

	while (done < size) {
		ssize_t ret = read(fd, bytes + done, size - done);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (ret == 0) {
			return ECONNRESET;
		}
		done += (size_t)ret;
	}
	return 0;
}
