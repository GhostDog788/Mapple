#ifndef STREAM_HELPERS_H
#define STREAM_HELPERS_H

#include <stddef.h>

int read_exact(int fd, void *buffer, size_t size);
int write_all(int fd, const void *buffer, size_t size);

#endif /* STREAM_HELPERS_H */
