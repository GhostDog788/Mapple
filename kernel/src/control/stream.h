#ifndef MAPPLE_STREAM_H
#define MAPPLE_STREAM_H

#include <linux/atomic.h>
#include <linux/stddef.h>
#include <linux/types.h>

struct mapple_stream;

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
void mapple_stream_shutdown(struct mapple_stream *stream);

#endif /* MAPPLE_STREAM_H */
