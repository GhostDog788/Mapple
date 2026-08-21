#include <control/transports/chardev/stream_ops.h>

#include <control/transports/chardev/channel/channel.h>

static ssize_t mapple_chardev_stream_read(struct mapple_stream *stream, void *buffer, size_t size)
{
	return channel_stream_read(stream->backend, buffer, size);
}

static ssize_t mapple_chardev_stream_write(struct mapple_stream *stream, const void *buffer, size_t size)
{
	return channel_stream_write(stream->backend, buffer, size);
}

static void mapple_chardev_stream_shutdown(struct mapple_stream *stream)
{
	channel_shutdown(stream->backend);
}

const struct mapple_stream_ops mapple_chardev_stream_ops = {
	.read_some = mapple_chardev_stream_read,
	.write_some = mapple_chardev_stream_write,
	.shutdown = mapple_chardev_stream_shutdown,
};
