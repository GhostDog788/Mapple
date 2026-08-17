#ifndef MAPPLE_TRANSPORT_H
#define MAPPLE_TRANSPORT_H

#include <control/stream.h>

struct mapple_session;

struct mapple_transport {
	const char *name;
	const struct mapple_stream_ops *stream_ops;
	void (*release)(struct mapple_session *session);
};

#endif /* MAPPLE_TRANSPORT_H */
