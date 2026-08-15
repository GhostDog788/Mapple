#include <control/stream.h>

#include <linux/errno.h>

void mapple_stream_init(struct mapple_stream *stream, const struct mapple_stream_ops *ops)
{
	stream->ops = ops;
	atomic_set(&stream->shut_down, 0);
}

bool mapple_stream_is_shutdown(const struct mapple_stream *stream)
{
	return atomic_read(&stream->shut_down) != 0;
}

ssize_t mapple_stream_read_some(struct mapple_stream *stream, void *buffer, size_t size)
{
	if (!size)
		return 0;
	return stream->ops->read_some(stream, buffer, size);
}

ssize_t mapple_stream_write_some(struct mapple_stream *stream, const void *buffer, size_t size)
{
	if (!size)
		return 0;
	if (mapple_stream_is_shutdown(stream))
		return -EPIPE;
	return stream->ops->write_some(stream, buffer, size);
}

int mapple_stream_read_exact(struct mapple_stream *stream, void *buffer, size_t size)
{
	size_t done = 0;

	while (done < size) {
		ssize_t ret = mapple_stream_read_some(stream, (u8 *)buffer + done, size - done);

		if (ret < 0)
			return ret;
		if (!ret)
			return -ECONNRESET;
		done += ret;
	}
	return 0;
}

int mapple_stream_write_all(struct mapple_stream *stream, const void *buffer, size_t size)
{
	size_t done = 0;

	while (done < size) {
		ssize_t ret = mapple_stream_write_some(stream, (const u8 *)buffer + done,
								 size - done);

		if (ret < 0)
			return ret;
		if (!ret)
			return -EPIPE;
		done += ret;
	}
	return 0;
}

void mapple_stream_shutdown(struct mapple_stream *stream)
{
	if (atomic_cmpxchg(&stream->shut_down, 0, 1) == 0)
		stream->ops->shutdown(stream);
}
