#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <commands/test.h>
#include <communication/transport.h>

int test(int (*open_transport)(int *), enum mapple_command_id command)
{
	int session_fd;
	int ret;

	ret = open_transport(&session_fd);
	if (ret)
		return ret;
	ret = test_command(session_fd, command);
	close(session_fd);
	return ret;
}

int main(void)
{
	int tcp_ret;
	int chardev_ret;

	signal(SIGPIPE, SIG_IGN);
	tcp_ret = test(open_tcp, MAPPLE_COMMAND_ECHO);
	chardev_ret = test(open_chardev, MAPPLE_COMMAND_ECHO);
	if (tcp_ret)
		fprintf(stderr, "TCP command failed: %s\n", strerror(tcp_ret));
	else
		printf("TCP command verified\n");
	if (chardev_ret)
		fprintf(stderr, "chardev command failed: %s\n", strerror(chardev_ret));
	else
		printf("chardev command verified\n");
	return tcp_ret || chardev_ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
