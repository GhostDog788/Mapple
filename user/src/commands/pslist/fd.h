#ifndef COMMAND_PSLIST_FD_H
#define COMMAND_PSLIST_FD_H

#include <stddef.h>

#include <commands/pslist/model.h>

int mapple_pslist_decode_fd_record(const void *record, size_t size,
				   struct mapple_pslist_fd_table *table);
int mapple_pslist_decode_fd_details_record(const void *record, size_t size,
					   struct mapple_pslist_fd_table *table);
int mapple_pslist_decode_network_record(const void *record, size_t size,
					struct mapple_pslist_network_table *table);
void mapple_pslist_free_fd_table(struct mapple_pslist_fd_table *table);
void mapple_pslist_free_network_table(struct mapple_pslist_network_table *table);
int mapple_pslist_print_json_fd_table(const struct mapple_pslist_fd_table *table,
				       int indent);
int mapple_pslist_print_json_network_table(
	const struct mapple_pslist_network_table *table, int indent);

#endif /* COMMAND_PSLIST_FD_H */
