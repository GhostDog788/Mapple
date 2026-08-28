#ifndef COMMAND_PSLIST_H
#define COMMAND_PSLIST_H

#include <stddef.h>

#include <linux/types.h>

int run_pslist(int session_fd);
int run_pslist_groups(int session_fd, const char *const *group_names, size_t group_count);
int mapple_pslist_groups_from_names(const char *const *names, size_t count, __u64 *groups);

#endif /* COMMAND_PSLIST_H */
