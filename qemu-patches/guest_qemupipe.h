#ifndef GUEST_QEMUPIPE_H
#define GUEST_QEMUPIPE_H
#include <stddef.h>
int guest_qemupipe_open(const char *path);
int guest_qemupipe_is(const char *path);
int guest_qemupipe_write(int fd, const void *buf, size_t n);
#endif
