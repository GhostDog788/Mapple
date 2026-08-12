#include "shared.h"

#include <fcntl.h>  /* open */
#include <stdio.h>  /* printf, perror */
#include <stdlib.h> /* EXIT_* */
#include <string.h> /* strncpy, memset */
#include <unistd.h> /* close */

int main(void)
{
	struct mapple_msg msg;
	int fd;

	fd = open(mapple_DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		perror("open " mapple_DEVICE_PATH);
		return EXIT_FAILURE;
	}

	/* Store the string in the driver. */
	memset(&msg, 0, sizeof(msg));
	strncpy(msg.data, "hello world", sizeof(msg.data) - 1);
	if (ioctl(fd, IOCTL_SET_MSG, &msg) < 0) {
		perror("ioctl IOCTL_SET_MSG");
		close(fd);
		return EXIT_FAILURE;
	}
	printf("stored:    \"%s\"\n", msg.data);

	/* Read it back out of the driver. */
	memset(&msg, 0, sizeof(msg));
	if (ioctl(fd, IOCTL_GET_MSG, &msg) < 0) {
		perror("ioctl IOCTL_GET_MSG");
		close(fd);
		return EXIT_FAILURE;
	}
	printf("read back: \"%s\"\n", msg.data);

	close(fd);
	return EXIT_SUCCESS;
}
