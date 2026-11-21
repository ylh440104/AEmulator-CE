#include "qemu/osdep.h"
#include "qemu.h"
#include "guest_qemupipe.h"
#include <sys/stat.h>
#include <fcntl.h>

static int pipe_path(const char *path)
{
    return path && strcmp(path, "/dev/qemu_pipe") == 0;
}

int guest_qemupipe_open(const char *path)
{
    char tmpl[] = "/tmp/qemu_pipe_XXXXXX";
    int fd;

    if (!pipe_path(path)) {
        return -1;
    }
    fd = mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    unlink(tmpl);
    return fd;
}

int guest_qemupipe_is(const char *path)
{
    return pipe_path(path);
}

int guest_qemupipe_write(int fd, const void *buf, size_t n)
{
    (void)fd;
    (void)buf;
    return (int)n;
}
