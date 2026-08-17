#include <control/sessions/session.h>

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/printk.h>
#include <linux/sched/task.h>
#include <linux/slab.h>

#include <control/commands/commands.h>
#include <control/sessions/session_manager.h>

static void mapple_session_reap(struct work_struct *work);

void mapple_session_init(struct mapple_session *session, const struct mapple_transport *transport,
						 void *stream_backend)
{
	kref_init(&session->refcount);
	INIT_LIST_HEAD(&session->manager_node);
	session->id = 0;
	session->transport = transport;
	session->state = MAPPLE_SESSION_STARTING;
	session->registered = false;
	session->listed = false;
	mapple_stream_init(&session->stream, transport->stream_ops, stream_backend);
	session->parser_task = NULL;
	mutex_init(&session->join_lock);
	session->parser_joined = false;
	INIT_WORK(&session->reap_work, mapple_session_reap);
}

void mapple_session_get(struct mapple_session *session) { kref_get(&session->refcount); }

static void mapple_session_release_ref(struct kref *refcount)
{
	struct mapple_session *session = container_of(refcount, struct mapple_session, refcount);

	if (session->parser_task)
		put_task_struct(session->parser_task);
	session->transport->release_backend(session->stream.backend);
	kfree(session);
}

void mapple_session_put(struct mapple_session *session)
{
	kref_put(&session->refcount, mapple_session_release_ref);
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

	mapple_session_join_parser(session);
	mapple_session_manager_unlist(session);
	mapple_session_put(session); /* parser reference; may free session */
}

static int mapple_session_parser_main(void *data)
{
	struct mapple_session *session = data;
	int ret;

	mapple_session_manager_set_state(session, MAPPLE_SESSION_RUNNING);

	ret = mapple_protocol_run(session);
	if (ret && ret != -ECONNRESET && ret != -EPIPE)
		pr_debug("mapple: session %llu protocol ended: %d\n", session->id, ret);

	mapple_session_manager_set_state(session, MAPPLE_SESSION_STOPPING);
	mapple_stream_shutdown(&session->stream);
	mapple_session_manager_unregister(session);
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

void mapple_session_shutdown(struct mapple_session *session) { mapple_stream_shutdown(&session->stream); }

void mapple_session_wait(struct mapple_session *session)
{
	if (!session->parser_task)
		return;
	mapple_session_join_parser(session);
	flush_work(&session->reap_work);
}
