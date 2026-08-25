#ifndef COMMAND_PSLIST_FD_H
#define COMMAND_PSLIST_FD_H

#include <commands/pslist/model.h>

int mapple_pslist_read_fd_table(int session_fd, struct mapple_pslist_fd_table *table);
void mapple_pslist_free_fd_table(struct mapple_pslist_fd_table *table);
int mapple_pslist_print_json_fd_table(const struct mapple_pslist_fd_table *table,
				       int indent);

#endif /* COMMAND_PSLIST_FD_H */
