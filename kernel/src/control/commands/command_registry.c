#include <control/commands/command_registry.h>

#include <commands.h>

#include <control/commands/echo/echo.h>
#include <control/commands/info/info.h>
#include <control/commands/pslist/pslist.h>
#include <control/commands/getfile/getfile.h>

static const mapple_command_handler_t mapple_command_handlers[MAPPLE_COMMAND_COUNT] = {
	[MAPPLE_COMMAND_ECHO] = mapple_handle_echo,
	[MAPPLE_COMMAND_PSLIST] = mapple_handle_pslist,
	[MAPPLE_COMMAND_INFO] = mapple_handle_info,
	[MAPPLE_COMMAND_GETFILE] = mapple_handle_getfile,
};

mapple_command_handler_t mapple_command_find_handler(u16 command)
{
	if (command >= MAPPLE_COMMAND_COUNT)
		return NULL;

	return mapple_command_handlers[command];
}
