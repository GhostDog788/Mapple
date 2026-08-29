#ifndef TEST_COMMANDS_H
#define TEST_COMMANDS_H

#include <stddef.h>

#include <commands.h>

int test_command(int session_fd, enum mapple_command_id command);
int test_pslist_command(int session_fd, const char *const *groups, size_t group_count);

#endif /* TEST_COMMANDS_H */
