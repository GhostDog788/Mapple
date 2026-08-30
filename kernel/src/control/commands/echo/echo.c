#include <control/commands/echo/echo.h>

#include <asm/unaligned.h>
#include <linux/minmax.h>
#include <linux/slab.h>

#define MAPPLE_ECHO_CHUNK_SIZE 4096U

static int mapple_echo_sized(struct mapple_command_context *context, u8 *buffer)
{
	int ret;

	ret = mapple_command_write_response_header(context, MAPPLE_COMMAND_ECHO, 0, MAPPLE_STATUS_OK,
						   context->payload_size);
	if (ret)
		return ret;

	while (context->remaining_payload) {
		size_t chunk = min_t(u64, context->remaining_payload, MAPPLE_ECHO_CHUNK_SIZE);

		ret = mapple_command_read(context, buffer, chunk);
		if (ret)
			return ret;
		ret = mapple_command_write(context, buffer, chunk);
		if (ret)
			return ret;
	}
	return 0;
}

static int mapple_echo_stream(struct mapple_command_context *context, u8 *buffer)
{
	__u32 wire_chunk_size;
	u32 chunk_size;
	int ret;

	ret = mapple_command_write_response_header(context, MAPPLE_COMMAND_ECHO,
						   MAPPLE_HEADER_FLAG_STREAM, MAPPLE_STATUS_OK, 0);
	if (ret)
		return ret;
	for (;;) {
		ret = mapple_command_read(context, &wire_chunk_size, sizeof(wire_chunk_size));
		if (ret)
			return ret;
		chunk_size = get_unaligned_be32(&wire_chunk_size);
		ret = mapple_command_write(context, &wire_chunk_size, sizeof(wire_chunk_size));
		if (ret || !chunk_size)
			return ret;

		while (chunk_size) {
			size_t size = min_t(u32, chunk_size, MAPPLE_ECHO_CHUNK_SIZE);

			ret = mapple_command_read(context, buffer, size);
			if (ret)
				return ret;
			ret = mapple_command_write(context, buffer, size);
			if (ret)
				return ret;
			chunk_size -= size;
		}
	}
}

int mapple_handle_echo(struct mapple_command_context *context)
{
	u8 *buffer;
	int ret;

	buffer = kmalloc(MAPPLE_ECHO_CHUNK_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	if (context->payload_streaming)
		ret = mapple_echo_stream(context, buffer);
	else
		ret = mapple_echo_sized(context, buffer);

	kfree(buffer);
	return ret;
}
