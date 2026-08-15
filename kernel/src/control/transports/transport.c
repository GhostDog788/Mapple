#include <control/transports/transport.h>

const char *mapple_transport_name(enum mapple_transport_kind transport)
{
	return transport == MAPPLE_TRANSPORT_TCP ? "tcp" : "chardev";
}
