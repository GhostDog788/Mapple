#ifndef MAPPLE_TRANSPORT_H
#define MAPPLE_TRANSPORT_H

enum mapple_transport_kind {
	MAPPLE_TRANSPORT_CHARDEV,
	MAPPLE_TRANSPORT_TCP,
};

const char *mapple_transport_name(enum mapple_transport_kind transport);

#endif /* MAPPLE_TRANSPORT_H */
