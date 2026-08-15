#include <control/commands/command_registry.h>

#include <commands.h>

#include <control/commands/echo/echo.h>

static const mapple_command_handler_t mapple_command_handlers[MAPPLE_COMMAND_COUNT] = {
	[MAPPLE_COMMAND_ECHO] = mapple_handle_echo,
};

mapple_command_handler_t mapple_command_find_handler(u16 command)
{
	if (command >= MAPPLE_COMMAND_COUNT)
		return NULL;

	return mapple_command_handlers[command];
}
