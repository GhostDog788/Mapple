#ifndef MAPPLE_SESSION_H
#define MAPPLE_SESSION_H

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <control/stream.h>

#define MAPPLE_MAX_SESSIONS 8U

enum mapple_transport_kind {
	MAPPLE_TRANSPORT_CHARDEV,
	MAPPLE_TRANSPORT_TCP,
};

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
	enum mapple_transport_kind transport;
	enum mapple_session_state state;
	bool registered;
	bool listed;
	struct mapple_stream stream;
	struct task_struct *parser_task;
	struct mutex join_lock;
	bool parser_joined;
	struct work_struct reap_work;
	void (*release)(struct mapple_session *session);
};

struct mapple_session_manager {
	struct mutex lock;
	struct list_head sessions;
	u32 active_count;
	u64 next_id;
	bool stopping;
};

extern struct mapple_session_manager mapple_sessions;

void mapple_session_manager_init(void);
void mapple_session_manager_begin_shutdown(void);
u32 mapple_session_manager_active_count(void);
bool mapple_session_manager_is_empty(void);
size_t mapple_session_manager_snapshot(struct mapple_session **sessions, size_t capacity);

void mapple_session_init(struct mapple_session *session, enum mapple_transport_kind transport,
				 const struct mapple_stream_ops *stream_ops,
				 void (*release)(struct mapple_session *session));
void mapple_session_get(struct mapple_session *session);
void mapple_session_put(struct mapple_session *session);
int mapple_session_register(struct mapple_session *session);
void mapple_session_unregister(struct mapple_session *session);
int mapple_session_start_parser(struct mapple_session *session);
void mapple_session_shutdown(struct mapple_session *session);
void mapple_session_wait(struct mapple_session *session);
const char *mapple_transport_name(enum mapple_transport_kind transport);

#endif /* MAPPLE_SESSION_H */
