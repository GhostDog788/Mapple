#ifndef MAPPLE_TCP_SERVER_INTERNAL_H
#define MAPPLE_TCP_SERVER_INTERNAL_H

#include <linux/net.h>
#include <linux/sched.h>

#include <control/sessions/session.h>

struct mapple_tcp_session {
	struct mapple_session session;
	struct socket *socket;
};

struct mapple_tcp_server {
	struct socket *listen_socket;
	struct task_struct *listener_task;
};

struct mapple_session *mapple_session_from_stream(struct mapple_stream *stream);

struct mapple_tcp_session *mapple_tcp_from_session(struct mapple_session *session);

struct mapple_tcp_session *mapple_tcp_from_stream(struct mapple_stream *stream);

#endif /* MAPPLE_TCP_SERVER_INTERNAL_H */
