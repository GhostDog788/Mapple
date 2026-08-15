#include <control/transports/tcp_server/internal.h>

#include <linux/container_of.h>

struct mapple_session *mapple_session_from_stream(struct mapple_stream *stream)
{
	return container_of(stream, struct mapple_session, stream);
}

struct mapple_tcp_session *mapple_tcp_from_session(struct mapple_session *session)
{
	return container_of(session, struct mapple_tcp_session, session);
}

struct mapple_tcp_session *mapple_tcp_from_stream(struct mapple_stream *stream)
{
	struct mapple_session *session = mapple_session_from_stream(stream);

	return mapple_tcp_from_session(session);
}
