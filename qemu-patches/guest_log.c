#include "qemu/osdep.h"
#include "qemu.h"
#include "user-internals.h"
#include "fd-trans.h"
#include "guest_log.h"

#define LOG_MAX 4096

int guest_log_on(void)
{
    const char *e = getenv("GUEST_ANDROID_LOG");
    return e && *e && *e != '0';
}

static void log_record(const unsigned char *p, size_t length)
{
    if (length < 3 || p[0] < 2 || p[0] > 7) return;
    const unsigned char *zero = memchr(p + 1, 0, length - 1);
    if (!zero) return;
    size_t tag_size = zero - p - 1;
    if (!tag_size || tag_size > 128) return;
    for (size_t i = 1; i <= tag_size; i++)
        if (p[i] < 32 || p[i] > 126) return;
    const unsigned char *text = zero + 1;
    size_t size = length - (text - p);
    while (size && (text[size - 1] == 0 || text[size - 1] == 10)) size--;
    static const char levels[] = "??VDIWEF";
    char output[LOG_MAX + 224];
    int used = snprintf(output, sizeof output, "[android pid=%d] %c/%.*s: ",
                        (int)getpid(), levels[p[0]], (int)tag_size, p + 1);
    if (used < 0 || (size_t)used >= sizeof output - 1) return;
    size_t room = sizeof output - used - 2;
    if (size > room) size = room;
    memcpy(output + used, text, size);
    output[used + size] = '\n';
    (void)!write(STDERR_FILENO, output, used + size + 1);
}

static abi_long android_log_write(void *buf, size_t length)
{
    if (guest_log_on()) log_record(buf, length);
    return (abi_long)length;
}

static TargetFdTrans android_log_trans = { .target_to_host_data = android_log_write };

static int is_android_log(const char *path)
{
    return path && (!strcmp(path, "/dev/log/main") || !strcmp(path, "/dev/log/system") ||
                    !strcmp(path, "/dev/log/radio") || !strcmp(path, "/dev/stdout"));
}

int guest_log_try_open(const char *path, int *fd)
{
    if (!is_android_log(path)) return 0;
    *fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    return 1;
}

void guest_log_probe_path(const char *path, abi_long fd)
{
    (void)path;
    (void)fd;
}

void guest_log_opened(const char *path, abi_long fd)
{
    if (fd >= 0 && is_android_log(path)) fd_trans_register((int)fd, &android_log_trans);
}

int guest_log_fd(abi_long fd)
{
    return guest_log_on() && fd_trans_target_to_host_data((int)fd) == android_log_write;
}

void guest_log_vec(const struct iovec *vec, int n)
{
    unsigned char record[LOG_MAX];
    size_t size = 0;
    for (int i = 0; i < n; i++) {
        size_t len = vec[i].iov_len;
        if (len > sizeof record - size) len = sizeof record - size;
        if (len) memcpy(record + size, vec[i].iov_base, len);
        size += len;
        if (size == sizeof record) break;
    }
    log_record(record, size);
}