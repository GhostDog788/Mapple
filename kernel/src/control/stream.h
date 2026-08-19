#ifndef MAPPLE_STREAM_H
#define MAPPLE_STREAM_H

#include <linux/atomic.h>
#include <linux/stddef.h>
#include <linux/types.h>

struct mapple_stream;

/**
 * struct mapple_stream_ops - operations implemented by a stream backend
 * @read_some: read available stream data, potentially blocking
 * @write_some: write stream data, potentially blocking
 * @shutdown: wake blocked operations and make future operations terminate
 *
 * Before @shutdown returns, it must arrange for blocked reads and writes to
 * make progress toward termination. mapple_stream_shutdown() invokes the
 * callback at most once.
 */
struct mapple_stream_ops {
	ssize_t (*read_some)(struct mapple_stream *stream, void *buffer, size_t size);
	ssize_t (*write_some)(struct mapple_stream *stream, const void *buffer, size_t size);
	void (*shutdown)(struct mapple_stream *stream);
};

struct mapple_stream {
	const struct mapple_stream_ops *ops;
	void *backend;
	atomic_t shut_down;
};

void mapple_stream_init(struct mapple_stream *stream, const struct mapple_stream_ops *ops, void *backend);
ssize_t mapple_stream_read_some(struct mapple_stream *stream, void *buffer, size_t size);
ssize_t mapple_stream_write_some(struct mapple_stream *stream, const void *buffer, size_t size);
int mapple_stream_read_exact(struct mapple_stream *stream, void *buffer, size_t size);
int mapple_stream_write_all(struct mapple_stream *stream, const void *buffer, size_t size);

/**
 * mapple_stream_shutdown - stop a stream exactly once
 * @stream: stream to stop
 *
 * Concurrent and repeated calls are safe. The backend shutdown callback runs
 * only for the first call.
 */
void mapple_stream_shutdown(struct mapple_stream *stream);

#endif /* MAPPLE_STREAM_H */
