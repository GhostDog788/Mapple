#ifndef MAPPLE_SESSION_MANAGER_H
#define MAPPLE_SESSION_MANAGER_H

#include <control/sessions/session.h>

#define MAPPLE_MAX_SESSIONS 8U

void mapple_session_manager_init(void);
void mapple_session_manager_shutdown(void);
/* Takes ownership of stream_backend, including when registration fails. */
int mapple_session_manager_register(const struct mapple_transport *transport, void *stream_backend);
void mapple_session_manager_unregister(struct mapple_session *session);

#endif /* MAPPLE_SESSION_MANAGER_H */
