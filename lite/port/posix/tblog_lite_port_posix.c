/* tblog-lite host port for Linux/POSIX. Tested (lite/tests). */
#define _POSIX_C_SOURCE 200809L
#include "tblog_lite_port_posix.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

static int out_fd_ = 1;
static volatile char lock_;  /* spinlock between threads */
static sigset_t saved_mask_; /* owned by the lock holder */

uint32_t tblog_lite_posix_enter(void) {
    sigset_t all, old;
    sigfillset(&all);
    /* Signals first: a handler must never run on a thread holding the lock
     * (it would spin forever waiting for itself). */
    pthread_sigmask(SIG_BLOCK, &all, &old);
    while (__atomic_test_and_set(&lock_, __ATOMIC_ACQUIRE)) {
    }
    saved_mask_ = old;
    return 0;
}

void tblog_lite_posix_exit(uint32_t state) {
    sigset_t old = saved_mask_;
    (void)state;
    __atomic_clear(&lock_, __ATOMIC_RELEASE);
    pthread_sigmask(SIG_SETMASK, &old, 0);
}

uint32_t tblog_lite_posix_ms(void) {
    static struct timespec t0;
    static int have_t0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!have_t0) { /* benign race: worst case two threads both set t0 */
        t0 = t;
        have_t0 = 1;
    }
    return (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}

static void fd_write(const char *data, size_t len) {
    while (len) {
        ssize_t n = write(out_fd_, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        data += n;
        len -= (size_t)n;
    }
}

const tblog_lite_port *tblog_lite_port_posix(int fd) {
    static const tblog_lite_port port = {fd_write, tblog_lite_posix_ms, tblog_lite_posix_enter,
                                         tblog_lite_posix_exit, 0};
    out_fd_ = fd;
    return &port;
}
