#ifndef MAPPLE_TRANSPORT_H
#define MAPPLE_TRANSPORT_H

#include <control/stream.h>

struct mapple_transport {
	const char *name;
	const struct mapple_stream_ops *stream_ops;
	void (*release_backend)(void *backend);
};

#endif /* MAPPLE_TRANSPORT_H */
