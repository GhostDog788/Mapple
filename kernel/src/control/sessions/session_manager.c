#include <control/sessions/session_manager.h>

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wait.h>

struct mapple_session_manager {
	struct mutex lock;
	struct list_head sessions;
	u32 active_count;
	u64 next_id;
	bool stopping;
	wait_queue_head_t empty_wait;
};

static struct mapple_session_manager mapple_sessions;

static u32 mapple_session_manager_active_count(void)
{
	u32 count;

	mutex_lock(&mapple_sessions.lock);
	count = mapple_sessions.active_count;
	mutex_unlock(&mapple_sessions.lock);
	return count;
}

static bool mapple_session_manager_is_empty(void)
{
	bool empty;

	mutex_lock(&mapple_sessions.lock);
	empty = list_empty(&mapple_sessions.sessions);
	mutex_unlock(&mapple_sessions.lock);
	return empty;
}

static size_t mapple_session_manager_snapshot(struct mapple_session **sessions, size_t capacity)
{
	struct mapple_session *session;
	size_t count = 0;

	mutex_lock(&mapple_sessions.lock);
	list_for_each_entry(session, &mapple_sessions.sessions, manager_node)
	{
		if (count == capacity)
			break;
		mapple_session_get(session);
		sessions[count++] = session;
	}
	mutex_unlock(&mapple_sessions.lock);
	return count;
}

void mapple_session_manager_init(void)
{
	mutex_init(&mapple_sessions.lock);
	INIT_LIST_HEAD(&mapple_sessions.sessions);
	mapple_sessions.active_count = 0;
	mapple_sessions.next_id = 0;
	mapple_sessions.stopping = false;
	init_waitqueue_head(&mapple_sessions.empty_wait);
}

void mapple_session_manager_shutdown(void)
{
	struct mapple_session *sessions[MAPPLE_MAX_SESSIONS];
	size_t count;
	size_t index;

	mutex_lock(&mapple_sessions.lock);
	mapple_sessions.stopping = true;
	mutex_unlock(&mapple_sessions.lock);

	count = mapple_session_manager_snapshot(sessions, ARRAY_SIZE(sessions));
	for (index = 0; index < count; ++index)
		mapple_session_shutdown(sessions[index]);
	for (index = 0; index < count; ++index)
		mapple_session_put(sessions[index]);

	wait_event(mapple_sessions.empty_wait, mapple_session_manager_is_empty());
	WARN_ON(mapple_session_manager_active_count() != 0);
	WARN_ON(!mapple_session_manager_is_empty());
}

int mapple_session_manager_register(const struct mapple_transport *transport, void *stream_backend)
{
	struct mapple_session *session;
	u32 active;
	int ret;

	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session) {
		transport->release_backend(stream_backend);
		return -ENOMEM;
	}
	mapple_session_init(session, transport, stream_backend);

	mutex_lock(&mapple_sessions.lock);
	if (mapple_sessions.stopping) {
		mutex_unlock(&mapple_sessions.lock);
		pr_debug("mapple: rejected %s session while stopping\n", session->transport->name);
		ret = -ESHUTDOWN;
		goto put_session;
	}
	if (mapple_sessions.active_count >= MAPPLE_MAX_SESSIONS) {
		active = mapple_sessions.active_count;
		mutex_unlock(&mapple_sessions.lock);
		pr_debug("mapple: rejected %s session at capacity (%u/%u)\n", session->transport->name, active,
				 MAPPLE_MAX_SESSIONS);
		ret = -EBUSY;
		goto put_session;
	}

	session->id = ++mapple_sessions.next_id;
	session->registered = true;
	session->state = MAPPLE_SESSION_STARTING;
	mapple_session_get(session); /* registry reference */
	list_add_tail(&session->manager_node, &mapple_sessions.sessions);
	active = ++mapple_sessions.active_count;
	mutex_unlock(&mapple_sessions.lock);

	pr_debug("mapple: session %llu created transport=%s active=%u\n", session->id, session->transport->name,
			 active);

	ret = mapple_session_start_parser(session);
	if (ret)
		mapple_session_manager_unregister(session);

put_session:
	mapple_session_put(session); /* creator reference */
	return ret;
}

void mapple_session_manager_unregister(struct mapple_session *session)
{
	u32 active;
	bool removed = false;

	mutex_lock(&mapple_sessions.lock);
	if (session->registered) {
		session->registered = false;
		session->state = MAPPLE_SESSION_CLOSED;
		list_del_init(&session->manager_node);
		active = --mapple_sessions.active_count;
		removed = true;
	}
	mutex_unlock(&mapple_sessions.lock);

	if (!removed)
		return;
	pr_debug("mapple: session %llu removed transport=%s active=%u\n", session->id, session->transport->name,
			 active);
	if (!active)
		wake_up_all(&mapple_sessions.empty_wait);
	mapple_session_put(session); /* registry reference */
}

void mapple_session_manager_set_state(struct mapple_session *session, enum mapple_session_state state)
{
	mutex_lock(&mapple_sessions.lock);
	if (session->registered)
		session->state = state;
	mutex_unlock(&mapple_sessions.lock);
}
