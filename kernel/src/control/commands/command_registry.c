#include <control/commands/command_registry.h>

#include <commands.h>

#include <control/commands/echo/echo.h>

mapple_command_handler_t mapple_command_find_handler(u16 command)
{
	switch (command) {
	case MAPPLE_COMMAND_ECHO:
		return mapple_handle_echo;
	default:
		return NULL;
	}
}
