#ifndef MAPPLE_KERNEL_COMMANDS_H
#define MAPPLE_KERNEL_COMMANDS_H

#include <linux/types.h>

#include <commands.h>

#include <control/sessions/session.h>

struct mapple_decoded_header {
	u32 magic;
	u16 version;
	u16 command;
	u32 flags;
	u32 status;
	u64 payload_size;
};

struct mapple_command_context {
	struct mapple_session *session;
	struct mapple_decoded_header header;
	bool payload_streaming;
	u64 payload_size;
	u64 remaining_payload;
	bool response_started;
	bool response_streaming;
	u64 response_size;
	u64 remaining_response;
};

typedef int (*mapple_command_handler_t)(struct mapple_command_context *context);

int mapple_protocol_run(struct mapple_session *session);

int mapple_command_read(struct mapple_command_context *context, void *buffer, size_t size);
int mapple_command_write_response_header(struct mapple_command_context *context, u16 command, u32 flags,
						 u32 status, u64 response_size);
int mapple_command_write(struct mapple_command_context *context, const void *buffer, size_t size);

#endif /* MAPPLE_KERNEL_COMMANDS_H */
