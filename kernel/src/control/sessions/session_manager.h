#ifndef MAPPLE_SESSION_MANAGER_H
#define MAPPLE_SESSION_MANAGER_H

#include <control/sessions/session.h>

#define MAPPLE_MAX_SESSIONS 8U

void mapple_session_manager_init(void);
void mapple_session_manager_shutdown(void);
int mapple_session_manager_register(struct mapple_session *session, const struct mapple_transport *transport);
void mapple_session_manager_unregister(struct mapple_session *session);
void mapple_session_manager_set_state(struct mapple_session *session, enum mapple_session_state state);
void mapple_session_manager_unlist(struct mapple_session *session);

#endif /* MAPPLE_SESSION_MANAGER_H */
