#ifndef GUEST_PMEM_H
#define GUEST_PMEM_H
#include "cpu.h"
int guest_pmem_ioctl(int fd, unsigned long req, abi_ulong arg, abi_long *ret);
#endif
