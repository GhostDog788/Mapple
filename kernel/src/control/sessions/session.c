#include <control/sessions/session.h>

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/printk.h>
#include <linux/sched/task.h>

#include <control/commands/commands.h>

struct mapple_session_manager mapple_sessions;

const char *mapple_transport_name(enum mapple_transport_kind transport)
{
	return transport == MAPPLE_TRANSPORT_TCP ? "tcp" : "chardev";
}

void mapple_session_manager_init(void)
{
	mutex_init(&mapple_sessions.lock);
	INIT_LIST_HEAD(&mapple_sessions.sessions);
	mapple_sessions.active_count = 0;
	mapple_sessions.next_id = 0;
	mapple_sessions.stopping = false;
}

void mapple_session_manager_begin_shutdown(void)
{
	mutex_lock(&mapple_sessions.lock);
	mapple_sessions.stopping = true;
	mutex_unlock(&mapple_sessions.lock);
}

u32 mapple_session_manager_active_count(void)
{
	u32 count;

	mutex_lock(&mapple_sessions.lock);
	count = mapple_sessions.active_count;
	mutex_unlock(&mapple_sessions.lock);
	return count;
}

bool mapple_session_manager_is_empty(void)
{
	bool empty;

	mutex_lock(&mapple_sessions.lock);
	empty = list_empty(&mapple_sessions.sessions);
	mutex_unlock(&mapple_sessions.lock);
	return empty;
}

size_t mapple_session_manager_snapshot(struct mapple_session **sessions, size_t capacity)
{
	struct mapple_session *session;
	size_t count = 0;

	mutex_lock(&mapple_sessions.lock);
	list_for_each_entry(session, &mapple_sessions.sessions, manager_node) {
		if (count == capacity)
			break;
		mapple_session_get(session);
		sessions[count++] = session;
	}
	mutex_unlock(&mapple_sessions.lock);
	return count;
}

static void mapple_session_reap(struct work_struct *work);

void mapple_session_init(struct mapple_session *session, enum mapple_transport_kind transport,
				 const struct mapple_stream_ops *stream_ops,
				 void (*release)(struct mapple_session *session))
{
	kref_init(&session->refcount);
	INIT_LIST_HEAD(&session->manager_node);
	session->id = 0;
	session->transport = transport;
	session->state = MAPPLE_SESSION_STARTING;
	session->registered = false;
	session->listed = false;
	mapple_stream_init(&session->stream, stream_ops);
	session->parser_task = NULL;
	mutex_init(&session->join_lock);
	session->parser_joined = false;
	INIT_WORK(&session->reap_work, mapple_session_reap);
	session->release = release;
}

void mapple_session_get(struct mapple_session *session)
{
	kref_get(&session->refcount);
}

static void mapple_session_release_ref(struct kref *refcount)
{
	struct mapple_session *session = container_of(refcount, struct mapple_session, refcount);

	if (session->parser_task)
		put_task_struct(session->parser_task);
	session->release(session);
}

void mapple_session_put(struct mapple_session *session)
{
	kref_put(&session->refcount, mapple_session_release_ref);
}

int mapple_session_register(struct mapple_session *session)
{
	u32 active;

	mutex_lock(&mapple_sessions.lock);
	if (mapple_sessions.stopping) {
		mutex_unlock(&mapple_sessions.lock);
		pr_debug("mapple: rejected %s session while stopping\n",
			 mapple_transport_name(session->transport));
		return -ESHUTDOWN;
	}
	if (mapple_sessions.active_count >= MAPPLE_MAX_SESSIONS) {
		active = mapple_sessions.active_count;
		mutex_unlock(&mapple_sessions.lock);
		pr_debug("mapple: rejected %s session at capacity (%u/%u)\n",
			 mapple_transport_name(session->transport), active, MAPPLE_MAX_SESSIONS);
		return -EBUSY;
	}

	session->id = ++mapple_sessions.next_id;
	session->registered = true;
	session->listed = true;
	session->state = MAPPLE_SESSION_STARTING;
	mapple_session_get(session); /* registry reference */
	list_add_tail(&session->manager_node, &mapple_sessions.sessions);
	active = ++mapple_sessions.active_count;
	mutex_unlock(&mapple_sessions.lock);

	pr_debug("mapple: session %llu created transport=%s active=%u\n",
		  session->id, mapple_transport_name(session->transport), active);
	return 0;
}

void mapple_session_unregister(struct mapple_session *session)
{
	u32 active;
	bool removed = false;
	bool unlist = false;

	mutex_lock(&mapple_sessions.lock);
	if (session->registered) {
		session->registered = false;
		session->state = MAPPLE_SESSION_CLOSED;
		active = --mapple_sessions.active_count;
		removed = true;
		/* A failed start has no task for the asynchronous reaper to join. */
		if (!session->parser_task) {
			list_del_init(&session->manager_node);
			session->listed = false;
			unlist = true;
		}
	}
	mutex_unlock(&mapple_sessions.lock);

	if (!removed)
		return;
	pr_debug("mapple: session %llu removed transport=%s active=%u\n",
		  session->id, mapple_transport_name(session->transport), active);
	if (unlist)
		mapple_session_put(session); /* registry reference */
}

static void mapple_session_join_parser(struct mapple_session *session)
{
	mutex_lock(&session->join_lock);
	if (!session->parser_joined && session->parser_task) {
		kthread_stop(session->parser_task);
		session->parser_joined = true;
	}
	mutex_unlock(&session->join_lock);
}

static void mapple_session_reap(struct work_struct *work)
{
	struct mapple_session *session = container_of(work, struct mapple_session, reap_work);
	bool unlist = false;

	mapple_session_join_parser(session);
	mutex_lock(&mapple_sessions.lock);
	if (session->listed) {
		list_del_init(&session->manager_node);
		session->listed = false;
		unlist = true;
	}
	mutex_unlock(&mapple_sessions.lock);
	if (unlist)
		mapple_session_put(session); /* registry reference */
	mapple_session_put(session); /* parser reference; may free session */
}

static int mapple_session_parser_main(void *data)
{
	struct mapple_session *session = data;
	int ret;

	mutex_lock(&mapple_sessions.lock);
	if (session->registered)
		session->state = MAPPLE_SESSION_RUNNING;
	mutex_unlock(&mapple_sessions.lock);

	ret = mapple_protocol_run(session);
	if (ret && ret != -ECONNRESET && ret != -EPIPE)
		pr_debug("mapple: session %llu protocol ended: %d\n", session->id, ret);

	mutex_lock(&mapple_sessions.lock);
	if (session->registered)
		session->state = MAPPLE_SESSION_STOPPING;
	mutex_unlock(&mapple_sessions.lock);
	mapple_stream_shutdown(&session->stream);
	mapple_session_unregister(session);
	schedule_work(&session->reap_work);
	return 0;
}

int mapple_session_start_parser(struct mapple_session *session)
{
	struct task_struct *task;

	mapple_session_get(session); /* parser reference */
	task = kthread_create(mapple_session_parser_main, session, "mapple/%llu", session->id);
	if (IS_ERR(task)) {
		mapple_session_put(session);
		return PTR_ERR(task);
	}
	session->parser_task = task;
	get_task_struct(task);
	wake_up_process(task);
	return 0;
}

void mapple_session_shutdown(struct mapple_session *session)
{
	mapple_stream_shutdown(&session->stream);
}

void mapple_session_wait(struct mapple_session *session)
{
	if (!session->parser_task)
		return;
	mapple_session_join_parser(session);
	flush_work(&session->reap_work);
}
