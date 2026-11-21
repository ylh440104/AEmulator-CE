#ifndef GUEST_LOG_H
#define GUEST_LOG_H

#include "cpu.h"

int guest_log_on(void);
int guest_log_try_open(const char *path, int *fd);
void guest_log_opened(const char *path, abi_long fd);
void guest_log_probe_path(const char *path, abi_long fd);
int guest_log_fd(abi_long fd);
void guest_log_vec(const struct iovec *vec, int n);

#endif