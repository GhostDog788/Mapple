#ifndef MAPPLE_COMMAND_PSLIST_FILES_NETWORK_H
#define MAPPLE_COMMAND_PSLIST_FILES_NETWORK_H

#include <linux/fs.h>
#include <linux/un.h>

#include <control/commands/pslist/snapshot/stream.h>

struct mapple_pslist_captured_socket {
	struct mapple_pslist_network_record wire;
	u8 local_address[sizeof_field(struct sockaddr_un, sun_path)];
	u8 peer_address[sizeof_field(struct sockaddr_un, sun_path)];
	u32 local_length;
	u32 peer_length;
};

bool mapple_pslist_capture_socket(struct mapple_pslist_captured_socket *captured, struct file *file,
				  u64 object_id, u32 fd);
int mapple_pslist_write_socket(struct mapple_pslist_writer *writer,
			       const struct mapple_pslist_captured_socket *captured);

#endif
