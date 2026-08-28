#include <commands/test.h>

#include <errno.h>

#include <commands/echo/echo.h>
#include <commands/pslist/pslist.h>

typedef int (*command_test_t)(int session_fd);

static const command_test_t command_tests[MAPPLE_COMMAND_COUNT] = {
	[MAPPLE_COMMAND_ECHO] = run_echo,
	[MAPPLE_COMMAND_PSLIST] = run_pslist,
};

int test_command(int session_fd, enum mapple_command_id command)
{
	if (command < 0 || command >= MAPPLE_COMMAND_COUNT)
		return EINVAL;
	if (!command_tests[command])
		return EOPNOTSUPP;
	return command_tests[command](session_fd);
}

int test_pslist_command(int session_fd, const char *const *groups, size_t group_count)
{
	return run_pslist_groups(session_fd, groups, group_count);
}
