#ifndef MAPPLE_SESSION_H
#define MAPPLE_SESSION_H

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <control/stream.h>
#include <control/transports/transport.h>

enum mapple_session_state {
	MAPPLE_SESSION_STARTING,
	MAPPLE_SESSION_RUNNING,
	MAPPLE_SESSION_STOPPING,
	MAPPLE_SESSION_CLOSED,
};

struct mapple_session {
	struct kref refcount;
	struct list_head manager_node;
	u64 id;
	const struct mapple_transport *transport;
	enum mapple_session_state state;
	bool registered;
	struct mapple_stream stream;
	struct task_struct *parser_task;
	struct work_struct reap_work;
};

void mapple_session_init(struct mapple_session *session, const struct mapple_transport *transport,
						 void *stream_backend);
void mapple_session_get(struct mapple_session *session);
void mapple_session_put(struct mapple_session *session);
int mapple_session_start_parser(struct mapple_session *session);
void mapple_session_shutdown(struct mapple_session *session);

#endif /* MAPPLE_SESSION_H */
