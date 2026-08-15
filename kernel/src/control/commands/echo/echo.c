#include <control/commands/echo/echo.h>

#include <linux/minmax.h>
#include <linux/slab.h>

#define MAPPLE_ECHO_CHUNK_SIZE 4096U

int mapple_handle_echo(struct mapple_command_context *context)
{
	u8 *buffer;
	int ret;

	buffer = kmalloc(MAPPLE_ECHO_CHUNK_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	ret = mapple_command_write_response_header(context, MAPPLE_COMMAND_ECHO,
						   context->original_payload_size);
	if (ret)
		goto free_buffer;

	while (context->remaining_payload) {
		size_t chunk = min_t(u64, context->remaining_payload, MAPPLE_ECHO_CHUNK_SIZE);

		ret = mapple_command_read(context, buffer, chunk);
		if (ret)
			goto free_buffer;
		ret = mapple_command_write(context, buffer, chunk);
		if (ret)
			goto free_buffer;
	}

free_buffer:
	kfree(buffer);
	return ret;
}
