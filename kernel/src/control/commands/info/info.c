#include <control/commands/info/info.h>

#include <asm/unaligned.h>

#include <control/sessions/session.h>
#include <control/sessions/session_manager.h>

#define MAPPLE_INFO_SUPPORTED_COMMANDS                                        \
	((1ULL << MAPPLE_COMMAND_ECHO) | (1ULL << MAPPLE_COMMAND_PSLIST) |    \
	 (1ULL << MAPPLE_COMMAND_INFO))

int mapple_handle_info(struct mapple_command_context *context)
{
	struct mapple_info_response response = { 0 };
	int ret;

	put_unaligned_be64(context->session->id, &response.session_id);
	put_unaligned_be32(MAPPLE_MAX_SESSIONS, &response.max_sessions);
	put_unaligned_be64(MAPPLE_INFO_SUPPORTED_COMMANDS, &response.supported_commands);

	ret = mapple_command_write_response_header(context, MAPPLE_COMMAND_INFO, 0, MAPPLE_STATUS_OK,
						   sizeof(response));
	if (ret)
		return ret;
	return mapple_command_write(context, &response, sizeof(response));
}
