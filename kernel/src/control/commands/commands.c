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
	header->flags = get_unaligned_be32(&wire->flags);
	header->status = get_unaligned_be32(&wire->status);
	header->payload_size = get_unaligned_be64(&wire->payload_size);
}

static void mapple_encode_header(struct mapple_wire_header *wire, u16 command, u32 flags, u32 status,
				  u64 payload_size)
{
	put_unaligned_be32(MAPPLE_PROTOCOL_MAGIC, &wire->magic);
	put_unaligned_be16(MAPPLE_PROTOCOL_VERSION, &wire->version);
	put_unaligned_be16(command, &wire->command);
	put_unaligned_be32(flags, &wire->flags);
	put_unaligned_be32(status, &wire->status);
	put_unaligned_be64(payload_size, &wire->payload_size);
}

static bool mapple_header_is_streaming(const struct mapple_decoded_header *header)
{
	return header->flags & MAPPLE_HEADER_FLAG_STREAM;
}

static bool mapple_header_is_valid(const struct mapple_decoded_header *header)
{
	if (header->magic != MAPPLE_PROTOCOL_MAGIC || header->version != MAPPLE_PROTOCOL_VERSION)
		return false;
	if (header->status != MAPPLE_STATUS_OK)
		return false;
	if (header->flags & ~MAPPLE_HEADER_KNOWN_FLAGS)
		return false;
	if (mapple_header_is_streaming(header) && header->payload_size)
		return false;
	return true;
}

int mapple_command_read(struct mapple_command_context *context, void *buffer, size_t size)
{
	int ret;

	if (!context->payload_streaming && (u64)size > context->remaining_payload)
		return -EMSGSIZE;
	ret = mapple_stream_read_exact(&context->session->stream, buffer, size);
	if (ret)
		return ret;
	if (!context->payload_streaming)
		context->remaining_payload -= size;
	return 0;
}

int mapple_command_write_response_header(struct mapple_command_context *context, u16 command, u32 flags,
										 u32 status, u64 response_size)
{
	struct mapple_wire_header wire;
	int ret;

	if (context->response_started)
		return -EPROTO;
	if (flags & ~MAPPLE_HEADER_KNOWN_FLAGS)
		return -EINVAL;
	if ((flags & MAPPLE_HEADER_FLAG_STREAM) && response_size)
		return -EINVAL;
	if (status != MAPPLE_STATUS_OK && (flags || response_size))
		return -EINVAL;
	mapple_encode_header(&wire, command, flags, status, response_size);
	ret = mapple_stream_write_all(&context->session->stream, &wire, sizeof(wire));
	if (ret)
		return ret;
	context->response_started = true;
	context->response_streaming = flags & MAPPLE_HEADER_FLAG_STREAM;
	if (!context->response_streaming) {
		context->response_size = response_size;
		context->remaining_response = response_size;
	}
	return 0;
}

int mapple_command_write(struct mapple_command_context *context, const void *buffer, size_t size)
{
	int ret;

	if (!context->response_started)
		return -EPROTO;
	if (!context->response_streaming && (u64)size > context->remaining_response)
		return -EMSGSIZE;
	ret = mapple_stream_write_all(&context->session->stream, buffer, size);
	if (ret)
		return ret;
	if (!context->response_streaming)
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

		if (!mapple_header_is_valid(&context.header))
			return -EPROTO;
		context.payload_streaming = mapple_header_is_streaming(&context.header);
		if (!context.payload_streaming) {
			context.payload_size = context.header.payload_size;
			context.remaining_payload = context.header.payload_size;
		}
		handler = mapple_command_find_handler(context.header.command);
		if (!handler)
			return -EOPNOTSUPP;

		ret = handler(&context);
		if (ret)
			return ret;
		if ((!context.payload_streaming && context.remaining_payload) ||
			!context.response_started ||
			(!context.response_streaming && context.remaining_response))
			return -EPROTO;
	}
}
