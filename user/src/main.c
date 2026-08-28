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

static int test_pslist(int (*open_transport)(int *), const char *const *groups,
		       size_t group_count)
{
	int session_fd;
	int ret;

	ret = open_transport(&session_fd);
	if (ret)
		return ret;
	ret = test_pslist_command(session_fd, groups, group_count);
	close(session_fd);
	return ret;
}

int main(int argc, char **argv)
{
	int tcp_ret;
	int chardev_ret;

	signal(SIGPIPE, SIG_IGN);
	if (argc == 1) {
		tcp_ret = test(open_tcp, MAPPLE_COMMAND_PSLIST);
		chardev_ret = 0; //test(open_chardev, MAPPLE_COMMAND_PSLIST);
	} else {
		tcp_ret = test_pslist(open_tcp, (const char *const *)&argv[1], argc - 1);
		chardev_ret = test_pslist(open_chardev, (const char *const *)&argv[1], argc - 1);
	}
	if (tcp_ret)
		fprintf(stderr, "TCP command failed: %s\n", strerror(tcp_ret));
	else
		fprintf(stderr, "TCP command verified\n");
	if (chardev_ret)
		fprintf(stderr, "chardev command failed: %s\n", strerror(chardev_ret));
	else
		fprintf(stderr, "chardev command verified\n");
	return tcp_ret || chardev_ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
