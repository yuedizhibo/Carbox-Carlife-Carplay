#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <time.h>

#define I2C_SLAVE_IOCTL 0x0703UL
#define MFI_ADDR 0x10UL
#define TRACKED_FD_CAP 4096

static unsigned char tracked[TRACKED_FD_CAP];

static int (*next_ioctl(void))(int, unsigned long, ...) {
    static int (*fn)(int, unsigned long, ...);
    if (!fn) fn = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    return fn;
}

static ssize_t (*next_read(void))(int, void *, size_t) {
    static ssize_t (*fn)(int, void *, size_t);
    if (!fn) fn = (ssize_t (*)(int, void *, size_t))dlsym(RTLD_NEXT, "read");
    return fn;
}

static int (*next_close(void))(int) {
    static int (*fn)(int);
    if (!fn) fn = (int (*)(int))dlsym(RTLD_NEXT, "close");
    return fn;
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    unsigned long arg = va_arg(ap, unsigned long);
    va_end(ap);
    int rc = next_ioctl()(fd, request, arg);
    if (rc >= 0 && request == I2C_SLAVE_IOCTL && arg == MFI_ADDR && fd >= 0 && fd < TRACKED_FD_CAP) {
        tracked[fd] = 1;
    }
    return rc;
}

ssize_t read(int fd, void *buf, size_t count) {
    if (fd >= 0 && fd < TRACKED_FD_CAP && tracked[fd]) {
        const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };
        nanosleep(&delay, NULL);
    }
    return next_read()(fd, buf, count);
}

int close(int fd) {
    if (fd >= 0 && fd < TRACKED_FD_CAP) tracked[fd] = 0;
    return next_close()(fd);
}

__attribute__((constructor)) static void announce(void) {
    static const char message[] = "MFI_READ_DELAY_EVAL_ACTIVE\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
}
