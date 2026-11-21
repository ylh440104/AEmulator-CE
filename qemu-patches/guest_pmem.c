#include "qemu/osdep.h"
#include "qemu.h"
#include "guest_pmem.h"
#include <sys/stat.h>

/* pmem ioctls of this era: _IOW('P', nr, unsigned int) with no size bits decoded
 * by the driver, so the request is just the type/code pair. */
#define PMEM_IOC(nr) ((unsigned long)(('P' << 8) | (nr)))

int guest_pmem_ioctl(int fd, unsigned long req, abi_ulong arg, abi_long *ret)
{
    /* this image's pmem header uses a lowercase 'p' for the type byte */
    if (((req >> 8) & 0xff) != 'p' && ((req >> 8) & 0xff) != 'P') {
        return 0;
    }
    switch ((req >> 0) & 0xff) {
    case 3: /* PMEM_GET_SIZE */
    case 7: { /* PMEM_GET_TOTAL_SIZE */
        struct stat st;
        unsigned int sz = 8u * 1024u * 1024u;
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            sz = (unsigned int)st.st_size;
        }
        if (arg && put_user_u32(sz, arg)) {
            *ret = -TARGET_EFAULT;
            return 1;
        }
        *ret = 0;
        return 1;
    }
    case 5: { /* PMEM_ALLOCATE */
        unsigned int want = 0;
        if (arg && get_user_u32(want, arg) == 0 && want > 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && (unsigned long)st.st_size < want) {
                ftruncate(fd, (off_t)want);
            }
        }
        *ret = 0;
        return 1;
    }
    case 1: /* PMEM_GET_PHYS */
    case 2: /* PMEM_MAP */
    case 4: /* PMEM_UNMAP */
    case 6: /* PMEM_CONNECT */
    case 8: /* PMEM_CACHE_FLUSH */
    case 9: /* PMEM_SET_CACHED */
    case 10: /* PMEM_SET_UNCACHED */
        *ret = 0;
        return 1;
    }
    *ret = 0;
    return 1;
}
