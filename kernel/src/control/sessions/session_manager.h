#ifndef MAPPLE_SESSION_MANAGER_H
#define MAPPLE_SESSION_MANAGER_H

#include <control/sessions/session.h>

#define MAPPLE_MAX_SESSIONS 8U

void mapple_session_manager_init(void);

/**
 * mapple_session_manager_shutdown - stop accepting sessions and drain the registry
 *
 * Prevents new registrations, shuts down every registered session, and waits
 * until the session registry is empty.
 *
 * Deferred reaper work may still be executing after this function returns.
 * The module workqueue must be drained before unloading module code. Resources
 * used by backend release callbacks must remain valid until that drain finishes.
 */
void mapple_session_manager_shutdown(void);

/**
 * mapple_session_manager_register - create and register a session
 * @transport: transport implementation for the session
 * @stream_backend: backend transferred to the new session
 *
 * Takes ownership of @stream_backend, including when registration fails.
 */
int mapple_session_manager_register(const struct mapple_transport *transport, void *stream_backend);
void mapple_session_manager_unregister(struct mapple_session *session);

#endif /* MAPPLE_SESSION_MANAGER_H */
