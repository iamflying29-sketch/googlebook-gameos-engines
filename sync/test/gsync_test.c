/* GameOS Sync test suite. It checks every behavior that Linux's ntsync selftests check (written fresh for GameOS
   from the list of behaviors), plus waits across processes, a cross-process stress test and benchmarks.
   Usage: gsync_test [bench]. Exit code 0 means every check passed. Every wait in the tests has a deadline. */

#define _GNU_SOURCE
#include "../gsync.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("  FAIL %s:%d: %s\n", __func__, __LINE__, #cond); } } while (0)

static struct gsync_device *dev;
static int dev_fd;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t in_ms(unsigned ms) { return now_ns() + (uint64_t)ms * 1000000ull; }

static int wait_any(const gsync_handle *o, uint32_t n, uint32_t owner, uint64_t timeout, uint32_t *idx)
{
    return gsync_wait_any(dev, o, n, timeout, 0, owner, 0, idx);
}

static int wait_all(const gsync_handle *o, uint32_t n, uint32_t owner, uint64_t timeout, uint32_t *idx)
{
    return gsync_wait_all(dev, o, n, timeout, 0, owner, 0, idx);
}

static void test_semaphore_state(void)
{
    gsync_handle s;
    uint32_t count, max, prev, idx;
    CHECK(gsync_create_sem(dev, 3, 2, &s) == -EINVAL);
    CHECK(gsync_create_sem(dev, 2, 2, &s) == 0);
    CHECK(gsync_sem_release(dev, s, 1, &prev) == -EOVERFLOW);
    CHECK(gsync_sem_read(dev, s, &count, &max) == 0 && count == 2 && max == 2);
    CHECK(wait_any(&s, 1, 123, now_ns(), &idx) == 0 && idx == 0);
    CHECK(gsync_sem_read(dev, s, &count, NULL) == 0 && count == 1);
    CHECK(wait_any(&s, 1, 123, now_ns(), &idx) == 0);
    CHECK(wait_any(&s, 1, 123, now_ns(), &idx) == -ETIMEDOUT);
    CHECK(gsync_sem_release(dev, s, 2, &prev) == 0 && prev == 0);
    CHECK(gsync_sem_release(dev, s, 1, &prev) == -EOVERFLOW);
    CHECK(gsync_sem_release(dev, s, ~0u, &prev) == -EOVERFLOW);
    CHECK(gsync_sem_read(dev, s, &count, NULL) == 0 && count == 2);
    CHECK(gsync_close(dev, s) == 0);
    CHECK(gsync_close(dev, s) == -EINVAL);
    CHECK(gsync_sem_release(dev, s, 1, &prev) == -EINVAL);
}

static void test_mutex_state(void)
{
    gsync_handle m;
    uint32_t owner, count, prev, idx;
    CHECK(gsync_create_mutex(dev, 123, 0, &m) == -EINVAL);
    CHECK(gsync_create_mutex(dev, 0, 1, &m) == -EINVAL);
    CHECK(gsync_create_mutex(dev, 123, 2, &m) == 0);
    CHECK(wait_any(&m, 1, 123, now_ns(), &idx) == 0);
    CHECK(gsync_mutex_read(dev, m, &owner, &count) == 0 && owner == 123 && count == 3);
    CHECK(wait_any(&m, 1, 456, now_ns(), &idx) == -ETIMEDOUT);
    CHECK(gsync_mutex_unlock(dev, m, 0, &prev) == -EINVAL);
    CHECK(gsync_mutex_unlock(dev, m, 456, &prev) == -EPERM);
    CHECK(gsync_mutex_unlock(dev, m, 123, &prev) == 0 && prev == 3);
    CHECK(gsync_mutex_unlock(dev, m, 123, &prev) == 0 && prev == 2);
    CHECK(gsync_mutex_unlock(dev, m, 123, &prev) == 0 && prev == 1);
    CHECK(gsync_mutex_read(dev, m, &owner, &count) == 0 && owner == 0 && count == 0);
    CHECK(gsync_mutex_unlock(dev, m, 123, &prev) == -EPERM);
    CHECK(wait_any(&m, 1, 456, now_ns(), &idx) == 0);
    CHECK(gsync_mutex_read(dev, m, &owner, &count) == 0 && owner == 456 && count == 1);
    CHECK(gsync_mutex_kill(dev, m, 0) == -EINVAL);
    CHECK(gsync_mutex_kill(dev, m, 123) == -EPERM);
    CHECK(gsync_mutex_kill(dev, m, 456) == 0);
    CHECK(gsync_mutex_read(dev, m, &owner, &count) == -EOWNERDEAD && owner == 0 && count == 0);
    CHECK(gsync_mutex_read(dev, m, &owner, &count) == -EOWNERDEAD);
    CHECK(wait_any(&m, 1, 123, now_ns(), &idx) == -EOWNERDEAD && idx == 0);
    CHECK(gsync_mutex_read(dev, m, &owner, &count) == 0 && owner == 123 && count == 1);
    CHECK(gsync_close(dev, m) == 0);
    /* At the recursion limit even the owner cannot acquire it again. */
    CHECK(gsync_create_mutex(dev, 123, ~0u, &m) == 0);
    CHECK(wait_any(&m, 1, 123, now_ns(), &idx) == -ETIMEDOUT);
    CHECK(gsync_close(dev, m) == 0);
}

static void test_event_state(void)
{
    gsync_handle e;
    uint32_t manual, sig, prev, idx;
    CHECK(gsync_create_event(dev, 1, 0, &e) == 0);
    CHECK(gsync_event_set(dev, e, &prev) == 0 && prev == 0);
    CHECK(gsync_event_set(dev, e, &prev) == 0 && prev == 1);
    CHECK(wait_any(&e, 1, 123, now_ns(), &idx) == 0);
    CHECK(gsync_event_read(dev, e, &manual, &sig) == 0 && manual == 1 && sig == 1);
    CHECK(gsync_event_reset(dev, e, &prev) == 0 && prev == 1);
    CHECK(gsync_event_reset(dev, e, &prev) == 0 && prev == 0);
    CHECK(wait_any(&e, 1, 123, now_ns(), &idx) == -ETIMEDOUT);
    CHECK(gsync_event_pulse(dev, e, &prev) == 0 && prev == 0);
    CHECK(gsync_event_read(dev, e, NULL, &sig) == 0 && sig == 0);
    CHECK(gsync_close(dev, e) == 0);

    CHECK(gsync_create_event(dev, 0, 1, &e) == 0);
    CHECK(wait_any(&e, 1, 123, now_ns(), &idx) == 0);
    CHECK(gsync_event_read(dev, e, &manual, &sig) == 0 && manual == 0 && sig == 0);
    CHECK(wait_any(&e, 1, 123, now_ns(), &idx) == -ETIMEDOUT);
    CHECK(gsync_event_set(dev, e, &prev) == 0 && prev == 0);
    CHECK(gsync_event_pulse(dev, e, &prev) == 0 && prev == 1);
    CHECK(gsync_event_read(dev, e, NULL, &sig) == 0 && sig == 0);
    CHECK(gsync_close(dev, e) == 0);
}

static void test_wait_any(void)
{
    gsync_handle o[2], many[65];
    uint32_t idx, count, owner;
    CHECK(gsync_create_sem(dev, 2, 3, &o[0]) == 0);
    CHECK(gsync_create_mutex(dev, 0, 0, &o[1]) == 0);
    CHECK(wait_any(o, 2, 123, now_ns(), &idx) == 0 && idx == 0);
    CHECK(wait_any(o, 2, 123, now_ns(), &idx) == 0 && idx == 0);
    CHECK(wait_any(o, 2, 123, now_ns(), &idx) == 0 && idx == 1); /* semaphore empty, mutex wins */
    CHECK(gsync_mutex_read(dev, o[1], &owner, &count) == 0 && owner == 123 && count == 1);
    CHECK(wait_any(o, 2, 456, now_ns(), &idx) == -ETIMEDOUT);
    CHECK(gsync_mutex_kill(dev, o[1], 123) == 0);
    CHECK(wait_any(o, 2, 456, now_ns(), &idx) == -EOWNERDEAD && idx == 1);
    /* The same object twice: lowest index, one unit consumed. */
    gsync_handle dup[2] = { o[0], o[0] };
    CHECK(gsync_sem_release(dev, o[0], 1, NULL) == 0);
    CHECK(wait_any(dup, 2, 123, now_ns(), &idx) == 0 && idx == 0);
    CHECK(gsync_sem_read(dev, o[0], &count, NULL) == 0 && count == 0);
    /* Limits and argument checks. */
    CHECK(wait_any(o, 0, 123, now_ns(), &idx) == -ETIMEDOUT);
    for (int i = 0; i < 65; i++) many[i] = o[0];
    CHECK(wait_any(many, 64, 123, now_ns(), &idx) == -ETIMEDOUT);
    CHECK(wait_any(many, 65, 123, now_ns(), &idx) == -EINVAL);
    CHECK(wait_any(o, 2, 0, now_ns(), &idx) == -EINVAL);
    CHECK(gsync_wait_any(dev, o, 2, now_ns(), 0x2, 123, 0, &idx) == -EINVAL);
    gsync_handle bad = o[0] + (1u << 20); /* wrong generation */
    CHECK(wait_any(&bad, 1, 123, now_ns(), &idx) == -EINVAL);
    CHECK(gsync_close(dev, o[0]) == 0 && gsync_close(dev, o[1]) == 0);
}

static void test_wait_all(void)
{
    gsync_handle o[4];
    uint32_t idx, count, owner, sig;
    CHECK(gsync_create_sem(dev, 1, 3, &o[0]) == 0);
    CHECK(gsync_create_mutex(dev, 0, 0, &o[1]) == 0);
    CHECK(gsync_create_event(dev, 1, 1, &o[2]) == 0);
    CHECK(gsync_create_event(dev, 0, 1, &o[3]) == 0);
    CHECK(wait_all(o, 4, 123, now_ns(), &idx) == 0 && idx == 0);
    CHECK(gsync_sem_read(dev, o[0], &count, NULL) == 0 && count == 0);
    CHECK(gsync_mutex_read(dev, o[1], &owner, &count) == 0 && owner == 123 && count == 1);
    CHECK(gsync_event_read(dev, o[2], NULL, &sig) == 0 && sig == 1);
    CHECK(gsync_event_read(dev, o[3], NULL, &sig) == 0 && sig == 0);
    /* Not all signaled: nothing is consumed. */
    CHECK(gsync_event_set(dev, o[3], NULL) == 0);
    CHECK(wait_all(o, 4, 123, now_ns(), &idx) == -ETIMEDOUT);
    CHECK(gsync_mutex_read(dev, o[1], &owner, &count) == 0 && count == 1);
    CHECK(gsync_event_read(dev, o[3], NULL, &sig) == 0 && sig == 1);
    /* Recursive mutex inside a wait-all, then abandoned mutex reported with everything acquired. */
    CHECK(gsync_sem_release(dev, o[0], 1, NULL) == 0);
    CHECK(wait_all(o, 4, 123, now_ns(), &idx) == 0);
    CHECK(gsync_mutex_read(dev, o[1], &owner, &count) == 0 && count == 2);
    CHECK(gsync_mutex_kill(dev, o[1], 123) == 0);
    CHECK(gsync_sem_release(dev, o[0], 1, NULL) == 0);
    CHECK(gsync_event_set(dev, o[3], NULL) == 0);
    CHECK(wait_all(o, 4, 456, now_ns(), &idx) == -EOWNERDEAD && idx == 0);
    CHECK(gsync_sem_read(dev, o[0], &count, NULL) == 0 && count == 0);
    CHECK(gsync_mutex_read(dev, o[1], &owner, &count) == 0 && owner == 456 && count == 1);
    /* Duplicates are invalid in a wait-all. */
    gsync_handle dup[2] = { o[0], o[0] };
    CHECK(wait_all(dup, 2, 123, now_ns(), &idx) == -EINVAL);
    for (int i = 0; i < 4; i++) CHECK(gsync_close(dev, o[i]) == 0);
}

/* ---------------------------------------------------------------- threaded tests */

struct waiter_args {
    gsync_handle objs[4];
    uint32_t count, owner, all;
    gsync_handle alert;
    uint64_t timeout;
    int ret;
    uint32_t index;
    _Atomic int done;
};

static void *waiter_thread(void *p)
{
    struct waiter_args *a = p;
    if (a->all) a->ret = gsync_wait_all(dev, a->objs, a->count, a->timeout, 0, a->owner, a->alert, &a->index);
    else a->ret = gsync_wait_any(dev, a->objs, a->count, a->timeout, 0, a->owner, a->alert, &a->index);
    atomic_store(&a->done, 1);
    return NULL;
}

static pthread_t start_waiter(struct waiter_args *a)
{
    pthread_t t;
    atomic_store(&a->done, 0);
    a->index = ~0u;
    pthread_create(&t, NULL, waiter_thread, a);
    usleep(20000); /* let it reach its sleep */
    return t;
}

static int still_waiting(struct waiter_args *a)
{
    usleep(20000);
    return !atomic_load(&a->done);
}

static void test_wake_any(void)
{
    struct waiter_args a = { .count = 2, .owner = 456, .timeout = 0 };
    uint32_t count, owner, sig;
    gsync_create_sem(dev, 0, 3, &a.objs[0]);
    gsync_create_mutex(dev, 123, 2, &a.objs[1]);
    a.timeout = in_ms(3000);
    pthread_t t = start_waiter(&a);
    CHECK(still_waiting(&a));
    CHECK(gsync_sem_release(dev, a.objs[0], 1, NULL) == 0);
    pthread_join(t, NULL);
    CHECK(a.ret == 0 && a.index == 0);
    CHECK(gsync_sem_read(dev, a.objs[0], &count, NULL) == 0 && count == 0);

    a.timeout = in_ms(3000);
    t = start_waiter(&a);
    CHECK(gsync_mutex_unlock(dev, a.objs[1], 123, NULL) == 0);
    CHECK(still_waiting(&a)); /* still owned once */
    CHECK(gsync_mutex_unlock(dev, a.objs[1], 123, NULL) == 0);
    pthread_join(t, NULL);
    CHECK(a.ret == 0 && a.index == 1);
    CHECK(gsync_mutex_read(dev, a.objs[1], &owner, &count) == 0 && owner == 456 && count == 1);
    gsync_close(dev, a.objs[0]);
    gsync_close(dev, a.objs[1]);

    /* Events: auto set, manual set, and pulse. */
    struct waiter_args e = { .count = 1, .owner = 123 };
    gsync_create_event(dev, 0, 0, &e.objs[0]);
    e.timeout = in_ms(3000);
    t = start_waiter(&e);
    CHECK(still_waiting(&e));
    CHECK(gsync_event_set(dev, e.objs[0], NULL) == 0);
    pthread_join(t, NULL);
    CHECK(e.ret == 0 && e.index == 0);
    CHECK(gsync_event_read(dev, e.objs[0], NULL, &sig) == 0 && sig == 0);
    e.timeout = in_ms(3000);
    t = start_waiter(&e);
    CHECK(gsync_event_pulse(dev, e.objs[0], NULL) == 0);
    pthread_join(t, NULL);
    CHECK(e.ret == 0 && e.index == 0);
    CHECK(gsync_event_read(dev, e.objs[0], NULL, &sig) == 0 && sig == 0);
    gsync_close(dev, e.objs[0]);

    gsync_create_event(dev, 1, 0, &e.objs[0]);
    struct waiter_args e2 = e;
    e.timeout = e2.timeout = in_ms(3000);
    pthread_t t1 = start_waiter(&e), t2 = start_waiter(&e2);
    CHECK(gsync_event_pulse(dev, e.objs[0], NULL) == 0); /* a manual pulse wakes every current waiter */
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    CHECK(e.ret == 0 && e2.ret == 0);
    CHECK(gsync_event_read(dev, e.objs[0], NULL, &sig) == 0 && sig == 0);

    /* Closing the object during a wait: the waiter keeps it alive and simply times out. */
    e.timeout = in_ms(200);
    t = start_waiter(&e);
    CHECK(gsync_close(dev, e.objs[0]) == 0);
    pthread_join(t, NULL);
    CHECK(e.ret == -ETIMEDOUT);
}

static void test_wake_all(void)
{
    struct waiter_args a = { .count = 4, .owner = 123, .all = 1 };
    uint32_t count, owner, sig, idx;
    gsync_create_sem(dev, 0, 3, &a.objs[0]);
    gsync_create_mutex(dev, 456, 1, &a.objs[1]);
    gsync_create_event(dev, 1, 0, &a.objs[2]);
    gsync_create_event(dev, 0, 0, &a.objs[3]);
    a.timeout = in_ms(3000);
    pthread_t t = start_waiter(&a);
    CHECK(gsync_sem_release(dev, a.objs[0], 1, NULL) == 0);
    CHECK(still_waiting(&a));
    /* Another thread may take the semaphore while the wait-all is not satisfiable. */
    CHECK(wait_any(&a.objs[0], 1, 789, now_ns(), &idx) == 0);
    CHECK(gsync_sem_release(dev, a.objs[0], 1, NULL) == 0);
    CHECK(gsync_mutex_unlock(dev, a.objs[1], 456, NULL) == 0);
    CHECK(gsync_event_set(dev, a.objs[2], NULL) == 0);
    CHECK(still_waiting(&a));
    CHECK(gsync_event_set(dev, a.objs[3], NULL) == 0);
    pthread_join(t, NULL);
    CHECK(a.ret == 0 && a.index == 0);
    CHECK(gsync_sem_read(dev, a.objs[0], &count, NULL) == 0 && count == 0);
    CHECK(gsync_mutex_read(dev, a.objs[1], &owner, &count) == 0 && owner == 123 && count == 1);
    CHECK(gsync_event_read(dev, a.objs[2], NULL, &sig) == 0 && sig == 1);
    CHECK(gsync_event_read(dev, a.objs[3], NULL, &sig) == 0 && sig == 0);
    for (int i = 0; i < 4; i++) gsync_close(dev, a.objs[i]);
}

static void test_alerts(void)
{
    gsync_handle alert, sem;
    uint32_t idx, sig;
    gsync_create_event(dev, 0, 1, &alert);
    gsync_create_sem(dev, 1, 2, &sem);
    /* Only an alert. */
    CHECK(gsync_wait_any(dev, NULL, 0, now_ns(), 0, 123, alert, &idx) == 0 && idx == 0);
    CHECK(gsync_wait_any(dev, NULL, 0, now_ns(), 0, 123, alert, &idx) == -ETIMEDOUT); /* auto alert consumed */
    /* Objects win over the alert. */
    gsync_event_set(dev, alert, NULL);
    CHECK(gsync_wait_any(dev, &sem, 1, now_ns(), 0, 123, alert, &idx) == 0 && idx == 0);
    CHECK(gsync_wait_any(dev, &sem, 1, now_ns(), 0, 123, alert, &idx) == 0 && idx == 1);
    /* Wake through the alert. */
    struct waiter_args a = { .count = 1, .owner = 123, .alert = alert };
    a.objs[0] = sem;
    a.timeout = in_ms(3000);
    pthread_t t = start_waiter(&a);
    CHECK(gsync_event_set(dev, alert, NULL) == 0);
    pthread_join(t, NULL);
    CHECK(a.ret == 0 && a.index == 1);
    CHECK(gsync_event_read(dev, alert, NULL, &sig) == 0 && sig == 0);
    /* Wait-all with an alert. */
    gsync_handle objs[2];
    gsync_create_sem(dev, 0, 2, &objs[0]);
    gsync_create_sem(dev, 1, 2, &objs[1]);
    struct waiter_args b = { .count = 2, .owner = 123, .all = 1, .alert = alert };
    b.objs[0] = objs[0];
    b.objs[1] = objs[1];
    b.timeout = in_ms(3000);
    t = start_waiter(&b);
    CHECK(gsync_event_set(dev, alert, NULL) == 0);
    pthread_join(t, NULL);
    CHECK(b.ret == 0 && b.index == 2);
    gsync_sem_release(dev, objs[0], 1, NULL);
    gsync_event_set(dev, alert, NULL);
    CHECK(gsync_wait_all(dev, objs, 2, now_ns(), 0, 123, alert, &idx) == 0 && idx == 0);
    gsync_close(dev, alert);
    gsync_close(dev, sem);
    gsync_close(dev, objs[0]);
    gsync_close(dev, objs[1]);
}

static gsync_handle stress_mutex;
static long stress_counter;
static void *stress_thread(void *p)
{
    uint32_t owner = (uint32_t)(uintptr_t)p, idx;
    for (int i = 0; i < 10000; i++) {
        if (gsync_wait_any(dev, &stress_mutex, 1, in_ms(5000), 0, owner, 0, &idx)) return (void *)1;
        stress_counter++;
        if (gsync_mutex_unlock(dev, stress_mutex, owner, NULL)) return (void *)1;
    }
    return NULL;
}

static void test_stress_threads(void)
{
    pthread_t t[4];
    stress_counter = 0;
    gsync_create_mutex(dev, 0, 0, &stress_mutex);
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, stress_thread, (void *)(uintptr_t)(100 + i));
    int bad = 0;
    for (int i = 0; i < 4; i++) {
        void *r;
        pthread_join(t[i], &r);
        bad |= r != NULL;
    }
    CHECK(!bad && stress_counter == 40000);
    gsync_close(dev, stress_mutex);
}

/* Two processes, two threads each, one mutex and one counter in shared memory. */
static void test_stress_processes(void)
{
    gsync_handle m;
    gsync_create_mutex(dev, 0, 0, &m);
    long *counter = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *counter = 0;
    pid_t kids[2];
    for (int k = 0; k < 2; k++) {
        kids[k] = fork();
        if (kids[k] == 0) {
            struct gsync_device *d = gsync_device_open(dev_fd);
            if (!d) _exit(2);
            int bad = 0;
            for (int i = 0; i < 20000 && !bad; i++) {
                uint32_t owner = 1000 + (uint32_t)k, idx;
                if (gsync_wait_any(d, &m, 1, in_ms(5000), 0, owner, 0, &idx)) bad = 1;
                (*counter)++;
                if (gsync_mutex_unlock(d, m, owner, NULL)) bad = 1;
            }
            _exit(bad);
        }
    }
    int ok = 1;
    for (int k = 0; k < 2; k++) {
        int st;
        waitpid(kids[k], &st, 0);
        ok &= WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }
    CHECK(ok && *counter == 40000);
    /* Cross-process wake: the child sleeps on an event that the parent sets. */
    gsync_handle ev;
    gsync_create_event(dev, 0, 0, &ev);
    pid_t c = fork();
    if (c == 0) {
        struct gsync_device *d = gsync_device_open(dev_fd);
        uint32_t idx;
        _exit(d && gsync_wait_any(d, &ev, 1, in_ms(3000), 0, 77, 0, &idx) == 0 ? 0 : 1);
    }
    usleep(50000);
    gsync_event_set(dev, ev, NULL);
    int st;
    waitpid(c, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    gsync_close(dev, ev);
    gsync_close(dev, m);
    munmap(counter, 4096);
}

/* ---------------------------------------------------------------- benchmarks */

#define ROUNDS 100000
static gsync_handle ping, pong;
static int efd_ping, efd_pong;

static void *pong_gsync(void *p)
{
    uint32_t idx;
    (void)p;
    for (int i = 0; i < ROUNDS; i++) {
        gsync_wait_any(dev, &ping, 1, GSYNC_TIMEOUT_INFINITE, 0, 2, 0, &idx);
        gsync_event_set(dev, pong, NULL);
    }
    return NULL;
}

static void *pong_eventfd(void *p)
{
    uint64_t v;
    (void)p;
    for (int i = 0; i < ROUNDS; i++) {
        struct pollfd pf = { efd_ping, POLLIN, 0 };
        ppoll(&pf, 1, NULL, NULL);
        if (read(efd_ping, &v, 8) != 8) break;
        v = 1;
        if (write(efd_pong, &v, 8) != 8) break;
    }
    return NULL;
}

static void benchmarks(void)
{
    uint32_t idx;
    gsync_handle m;
    gsync_create_mutex(dev, 0, 0, &m);
    uint64_t t0 = now_ns();
    for (int i = 0; i < 1000000; i++) {
        gsync_wait_any(dev, &m, 1, GSYNC_TIMEOUT_INFINITE, 0, 5, 0, &idx);
        gsync_mutex_unlock(dev, m, 5, NULL);
    }
    double unc = (double)(now_ns() - t0) / 1e6;
    printf("bench uncontended mutex acquire plus release: %.1f ns per pair (1000000 pairs)\n", unc);
    gsync_close(dev, m);
    /* The same operation done the esync way: a semaphore eventfd, read to acquire and write to release. */
    int sfd = eventfd(1, EFD_SEMAPHORE | EFD_NONBLOCK);
    t0 = now_ns();
    for (int i = 0; i < 1000000; i++) {
        uint64_t v;
        if (read(sfd, &v, 8) != 8) break;
        v = 1;
        if (write(sfd, &v, 8) != 8) break;
    }
    printf("bench uncontended esync-style acquire plus release (eventfd read plus write): %.1f ns per pair\n",
           (double)(now_ns() - t0) / 1e6);
    close(sfd);

    gsync_create_event(dev, 0, 0, &ping);
    gsync_create_event(dev, 0, 0, &pong);
    pthread_t t;
    pthread_create(&t, NULL, pong_gsync, NULL);
    t0 = now_ns();
    for (int i = 0; i < ROUNDS; i++) {
        gsync_event_set(dev, ping, NULL);
        gsync_wait_any(dev, &pong, 1, GSYNC_TIMEOUT_INFINITE, 0, 1, 0, &idx);
    }
    double g = (double)(now_ns() - t0) / ROUNDS / 1000.0;
    pthread_join(t, NULL);
    gsync_close(dev, ping);
    gsync_close(dev, pong);

    efd_ping = eventfd(0, 0);
    efd_pong = eventfd(0, 0);
    pthread_create(&t, NULL, pong_eventfd, NULL);
    t0 = now_ns();
    for (int i = 0; i < ROUNDS; i++) {
        uint64_t v = 1;
        if (write(efd_ping, &v, 8) != 8) break;
        struct pollfd pf = { efd_pong, POLLIN, 0 };
        ppoll(&pf, 1, NULL, NULL);
        if (read(efd_pong, &v, 8) != 8) break;
    }
    double e = (double)(now_ns() - t0) / ROUNDS / 1000.0;
    pthread_join(t, NULL);
    close(efd_ping);
    close(efd_pong);
    printf("bench event ping-pong round trip between two threads: gsync %.2f us, eventfd (esync style) %.2f us\n", g, e);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    dev = gsync_device_create(4096, 256, &dev_fd);
    if (!dev) {
        printf("FAIL device create errno %d\n", errno);
        return 1;
    }
    test_semaphore_state();
    test_mutex_state();
    test_event_state();
    test_wait_any();
    test_wait_all();
    test_wake_any();
    test_wake_all();
    test_alerts();
    test_stress_threads();
    test_stress_processes();
    printf("gsync tests: %d checks, %d failures\n", checks, failures);
    if (argc > 1 && !strcmp(argv[1], "bench")) benchmarks();
    gsync_device_close(dev);
    return failures ? 1 : 0;
}
