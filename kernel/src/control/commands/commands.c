#include <control/commands/commands.h>

#include <asm/unaligned.h>
#include <linux/errno.h>
#include <linux/string.h>

#include <control/commands/command_registry.h>
#include <control/sessions/session.h>

static void mapple_decode_header(const struct mapple_wire_header *wire, struct mapple_decoded_header *header)
{
	header->magic = get_unaligned_be32(&wire->magic);
	header->version = get_unaligned_be16(&wire->version);
	header->command = get_unaligned_be16(&wire->command);
	header->payload_size = get_unaligned_be64(&wire->payload_size);
}

static void mapple_encode_header(struct mapple_wire_header *wire, u16 command, u64 payload_size)
{
	put_unaligned_be32(MAPPLE_PROTOCOL_MAGIC, &wire->magic);
	put_unaligned_be16(MAPPLE_PROTOCOL_VERSION, &wire->version);
	put_unaligned_be16(command, &wire->command);
	put_unaligned_be64(payload_size, &wire->payload_size);
}

int mapple_command_read(struct mapple_command_context *context, void *buffer, size_t size)
{
	int ret;

	if ((u64)size > context->remaining_payload)
		return -EMSGSIZE;
	ret = mapple_stream_read_exact(&context->session->stream, buffer, size);
	if (ret)
		return ret;
	context->remaining_payload -= size;
	return 0;
}

int mapple_command_write_response_header(struct mapple_command_context *context, u16 command,
										 u64 payload_size)
{
	struct mapple_wire_header wire;
	int ret;

	if (context->response_started)
		return -EPROTO;
	mapple_encode_header(&wire, command, payload_size);
	ret = mapple_stream_write_all(&context->session->stream, &wire, sizeof(wire));
	if (ret)
		return ret;
	context->response_started = true;
	context->remaining_response = payload_size;
	return 0;
}

int mapple_command_write(struct mapple_command_context *context, const void *buffer, size_t size)
{
	int ret;

	if (!context->response_started || (u64)size > context->remaining_response)
		return -EMSGSIZE;
	ret = mapple_stream_write_all(&context->session->stream, buffer, size);
	if (ret)
		return ret;
	context->remaining_response -= size;
	return 0;
}

int mapple_protocol_run(struct mapple_session *session)
{
	struct mapple_wire_header wire;
	struct mapple_command_context context;
	mapple_command_handler_t handler;
	int ret;

	for (;;) {
		ret = mapple_stream_read_exact(&session->stream, &wire, sizeof(wire));
		if (ret)
			return ret;

		memset(&context, 0, sizeof(context));
		context.session = session;
		mapple_decode_header(&wire, &context.header);
		context.original_payload_size = context.header.payload_size;
		context.remaining_payload = context.header.payload_size;

		if (context.header.magic != MAPPLE_PROTOCOL_MAGIC ||
			context.header.version != MAPPLE_PROTOCOL_VERSION)
			return -EPROTO;
		handler = mapple_command_find_handler(context.header.command);
		if (!handler)
			return -EOPNOTSUPP;

		ret = handler(&context);
		if (ret)
			return ret;
		if (context.remaining_payload || !context.response_started || context.remaining_response)
			return -EPROTO;
	}
}
