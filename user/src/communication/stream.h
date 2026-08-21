#ifndef MAPPLE_USER_STREAM_H
#define MAPPLE_USER_STREAM_H

#include <stddef.h>

int read_exact(int fd, void *buffer, size_t size);
int write_all(int fd, const void *buffer, size_t size);

#endif /* MAPPLE_USER_STREAM_H */
