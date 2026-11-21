/* Minimal C usage of tblog. Build: make examples && ./build/examples/basic [log.conf] */
#include <tblog/log.h>

#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

static void work(int id) {
    for (int i = 0; i < 1000; ++i) {
        TBLOG_INFO("worker %d step %d", id, i);
        if (i % 250 == 0) TBLOG_MDEBUG("network", "worker %d checkpoint %d", id, i);
    }
}

/* Four threads, each logging 1000 lines (POSIX threads or Win32 threads). */
#if defined(_WIN32)
static DWORD WINAPI work_thread(LPVOID arg) {
    work((int)(INT_PTR)arg);
    return 0;
}
static void run_workers(void) {
    HANDLE t[4];
    for (int i = 0; i < 4; ++i) t[i] = CreateThread(NULL, 0, work_thread, (LPVOID)(INT_PTR)i, 0, NULL);
    for (int i = 0; i < 4; ++i) {
        if (!t[i]) continue;
        WaitForSingleObject(t[i], INFINITE);
        CloseHandle(t[i]);
    }
}
#else
static void *work_thread(void *arg) {
    work((int)(long)arg);
    return NULL;
}
static void run_workers(void) {
    pthread_t t[4];
    for (long i = 0; i < 4; ++i) pthread_create(&t[i], NULL, work_thread, (void *)i);
    for (int i = 0; i < 4; ++i) pthread_join(t[i], NULL);
}
#endif

int main(int argc, char **argv) {
    if (tblog_init(argc > 1 ? argv[1] : NULL) != 0) {
        fprintf(stderr, "tblog_init failed\n");
        return 1;
    }
    TBLOG_INFO("started, pid-independent message %s", "hello");
    TBLOG_DEBUG("this is filtered at the default INFO level");

    run_workers();

    TBLOG_WARN("done, dropped=%llu", (unsigned long long)tblog_dropped_count());
    tblog_shutdown();
    return 0;
}
