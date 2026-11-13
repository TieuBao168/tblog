/* Minimal C usage of tblog. Build: make examples && ./build/examples/basic [log.conf] */
#include <tblog/log.h>

#include <pthread.h>
#include <stdio.h>

static void *work(void *arg) {
    int id = (int)(long)arg;
    for (int i = 0; i < 1000; ++i) {
        TBLOG_INFO("worker %d step %d", id, i);
        if (i % 250 == 0) TBLOG_MDEBUG("network", "worker %d checkpoint %d", id, i);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (tblog_init(argc > 1 ? argv[1] : NULL) != 0) {
        fprintf(stderr, "tblog_init failed\n");
        return 1;
    }
    TBLOG_INFO("started, pid-independent message %s", "hello");
    TBLOG_DEBUG("this is filtered at the default INFO level");

    pthread_t t[4];
    for (long i = 0; i < 4; ++i) pthread_create(&t[i], NULL, work, (void *)i);
    for (int i = 0; i < 4; ++i) pthread_join(t[i], NULL);

    TBLOG_WARN("done, dropped=%llu", (unsigned long long)tblog_dropped_count());
    tblog_shutdown();
    return 0;
}
