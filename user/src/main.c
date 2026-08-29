#include <errno.h>
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

static void print_usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage: %s [--transport tcp|chardev] [GROUP ...]\n"
		"\n"
		"Request one PSList snapshot over one transport. TCP is the default.\n"
		"Groups: core identity environment security context resources fds "
		"fds-details network memory-maps elf\n",
		program);
}

static int parse_transport(const char *name, int (**open_transport)(int *),
			   const char **transport_name)
{
	if (!strcmp(name, "tcp")) {
		*open_transport = open_tcp;
		*transport_name = "TCP";
		return 0;
	}
	if (!strcmp(name, "chardev")) {
		*open_transport = open_chardev;
		*transport_name = "chardev";
		return 0;
	}
	return EINVAL;
}

int main(int argc, char **argv)
{
	int (*open_transport)(int *) = open_tcp;
	const char *transport_name = "TCP";
	int first_group = 1;
	int ret;

	signal(SIGPIPE, SIG_IGN);
	while (first_group < argc) {
		const char *argument = argv[first_group];
		const char *value;

		if (!strcmp(argument, "--")) {
			++first_group;
			break;
		}
		if (!strcmp(argument, "--help") || !strcmp(argument, "-h")) {
			print_usage(stdout, argv[0]);
			return EXIT_SUCCESS;
		}
		if (!strcmp(argument, "--transport")) {
			if (++first_group == argc) {
				fprintf(stderr, "--transport requires tcp or chardev\n");
				print_usage(stderr, argv[0]);
				return EXIT_FAILURE;
			}
			value = argv[first_group];
		} else if (!strncmp(argument, "--transport=", sizeof("--transport=") - 1)) {
			value = argument + sizeof("--transport=") - 1;
		} else if (argument[0] == '-') {
			fprintf(stderr, "unknown option: %s\n", argument);
			print_usage(stderr, argv[0]);
			return EXIT_FAILURE;
		} else {
			break;
		}
		ret = parse_transport(value, &open_transport, &transport_name);
		if (ret) {
			fprintf(stderr, "unknown transport: %s\n", value);
			print_usage(stderr, argv[0]);
			return EXIT_FAILURE;
		}
		++first_group;
	}
	if (first_group == argc)
		ret = test(open_transport, MAPPLE_COMMAND_PSLIST);
	else
		ret = test_pslist(open_transport, (const char *const *)&argv[first_group],
				  (size_t)(argc - first_group));
	if (ret)
		fprintf(stderr, "%s command failed: %s\n", transport_name, strerror(ret));
	else
		fprintf(stderr, "%s command verified\n", transport_name);
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
