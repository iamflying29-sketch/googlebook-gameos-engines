/* GameOS Sync fault-injection test. A victim process runs a workload touching every path and is killed (SIGKILL) at
   fault point k, for every k, while a live helper process keeps using the same objects. After each kill, the test
   requires that the helper never hangs, that the shared state is fully consistent, and that every object still
   works. Build with -DGSYNC_FAULT_INJECTION. Usage: gsync_fault [max_points] [stride]. */

#define _GNU_SOURCE
#include "../gsync.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern int gsync_fault_at, gsync_fault_count;
int gsync_check(struct gsync_device *dev);

struct shared {
    gsync_handle sem, mtx, autoev, manev, alert;
    int victim_points;
};

static int dev_fd;
static struct shared *sh;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t in_ms(unsigned ms) { return now_ns() + (uint64_t)ms * 1000000ull; }

/* The victim's workload: fast paths, slow paths with sleeps and handoffs, wait-all, alerts, pulses, kills. */
static void workload(struct gsync_device *d, uint32_t owner, int rounds)
{
    uint32_t idx;
    gsync_handle any2[2] = { sh->sem, sh->mtx }, all3[3] = { sh->sem, sh->mtx, sh->manev };
    for (int r = 0; r < rounds; r++) {
        if (gsync_wait_any(d, any2, 2, in_ms(30), 0, owner, 0, &idx) == 0) {
            if (idx == 0) gsync_sem_release(d, sh->sem, 1, NULL);
            else gsync_mutex_unlock(d, sh->mtx, owner, NULL);
        }
        if (gsync_wait_any(d, &sh->mtx, 1, in_ms(30), 0, owner, sh->alert, &idx) == 0 && idx == 0)
            gsync_mutex_unlock(d, sh->mtx, owner, NULL);
        gsync_event_set(d, sh->autoev, NULL);
        gsync_wait_any(d, &sh->autoev, 1, in_ms(20), 0, owner, 0, &idx);
        if (gsync_wait_all(d, all3, 3, in_ms(30), 0, owner, sh->alert, &idx) == 0 && idx == 0) {
            gsync_sem_release(d, sh->sem, 1, NULL);
            gsync_mutex_unlock(d, sh->mtx, owner, NULL);
        }
        gsync_event_pulse(d, sh->manev, NULL);
        gsync_event_set(d, sh->manev, NULL);
        gsync_sem_release(d, sh->sem, 1, NULL);
        gsync_wait_any(d, &sh->sem, 1, in_ms(10), 0, owner, 0, &idx);
        if (gsync_wait_any(d, &sh->mtx, 1, in_ms(30), 0, owner, 0, &idx) == 0 && idx == 0) gsync_mutex_kill(d, sh->mtx, owner);
        gsync_event_set(d, sh->alert, NULL);
        gsync_event_reset(d, sh->alert, NULL);
    }
}

/* The helper keeps working on the same objects and must never hang. */
static int helper(void)
{
    struct gsync_device *d = gsync_device_open(dev_fd);
    if (!d) return 2;
    uint32_t idx, owner = 4242;
    char log[4096];
    int logn = 0;
    for (int r = 0; r < 40; r++) {
        uint64_t t0 = now_ns();
        int ret = gsync_wait_any(d, &sh->mtx, 1, in_ms(50), 0, owner, 0, &idx);
        int ur = 1;
        if (ret == 0 || ret == -EOWNERDEAD) ur = gsync_mutex_unlock(d, sh->mtx, owner, NULL);
        if (logn < 3900) logn += snprintf(log + logn, sizeof log - logn, "[%d wait %d %lums unlock %d] ", r, ret,
                                          (unsigned long)((now_ns() - t0) / 1000000), ur);
        if (gsync_wait_any(d, &sh->autoev, 1, in_ms(20), 0, owner, 0, &idx) == 0) gsync_event_set(d, sh->autoev, NULL);
        gsync_sem_release(d, sh->sem, 1, NULL);
        gsync_wait_any(d, &sh->sem, 1, in_ms(20), 0, owner, 0, &idx);
        gsync_event_set(d, sh->manev, NULL);
    }
    uint32_t mo = 0, mc = 0;
    gsync_mutex_read(d, sh->mtx, &mo, &mc);
    if (mo == owner) printf("    helper ends owning the mutex (count %u); its waits: %s\n", mc, log);
    return 0;
}

static void make_objects(struct gsync_device *dev)
{
    gsync_create_sem(dev, 1, 1000, &sh->sem);
    gsync_create_mutex(dev, 0, 0, &sh->mtx);
    gsync_create_event(dev, 0, 0, &sh->autoev);
    gsync_create_event(dev, 1, 1, &sh->manev);
    gsync_create_event(dev, 0, 0, &sh->alert);
}

static void close_objects(struct gsync_device *dev)
{
    gsync_close(dev, sh->sem);
    gsync_close(dev, sh->mtx);
    gsync_close(dev, sh->autoev);
    gsync_close(dev, sh->manev);
    gsync_close(dev, sh->alert);
}

/* After recovery every object must still behave correctly. */
static int objects_work(struct gsync_device *dev, uint32_t victim_owner)
{
    uint32_t idx, prev;
    uint32_t mo = 0, mc = 0;
    int mr = gsync_mutex_read(dev, sh->mtx, &mo, &mc);
    int kr = gsync_mutex_kill(dev, sh->mtx, victim_owner); /* what the wineserver does for a dead thread's mutexes */
    int r = gsync_wait_any(dev, &sh->mtx, 1, in_ms(500), 0, 7, 0, &idx);
    if (r != 0 && r != -EOWNERDEAD) {
        printf("    mutex before: read %d owner %u count %u; kill(%u) %d; wait %d\n", mr, mo, mc, victim_owner, kr, r);
        return 1;
    }
    if (gsync_mutex_unlock(dev, sh->mtx, 7, NULL)) return 2;
    if (gsync_sem_release(dev, sh->sem, 1, &prev)) return 3;
    if (gsync_wait_any(dev, &sh->sem, 1, in_ms(500), 0, 7, 0, &idx)) return 4;
    if (gsync_event_set(dev, sh->autoev, NULL)) return 5;
    if (gsync_wait_any(dev, &sh->autoev, 1, in_ms(500), 0, 7, 0, &idx)) return 6;
    gsync_handle all2[2] = { sh->sem, sh->manev };
    gsync_sem_release(dev, sh->sem, 1, NULL); /* a dead victim may legitimately keep the unit it took */
    gsync_event_set(dev, sh->manev, NULL);
    if (gsync_wait_all(dev, all2, 2, in_ms(500), 0, 7, 0, &idx)) return 7;
    gsync_sem_release(dev, sh->sem, 1, NULL);
    return 0;
}

static pid_t spawn(int fault_at, uint32_t owner, int rounds)
{
    pid_t p = fork();
    if (p == 0) {
        struct gsync_device *d = gsync_device_open(dev_fd);
        if (!d) _exit(2);
        gsync_fault_count = 0;
        gsync_fault_at = fault_at;
        workload(d, owner, rounds);
        if (!fault_at) sh->victim_points = gsync_fault_count;
        _exit(0);
    }
    return p;
}

/* Waits for a child with a deadline; a child still running at the deadline counts as a hang. */
static int reap(pid_t p, int seconds, int *status)
{
    uint64_t end = in_ms((unsigned)seconds * 1000);
    while (now_ns() < end) {
        pid_t r = waitpid(p, status, WNOHANG);
        if (r == p) return 0;
        usleep(2000);
    }
    kill(p, SIGKILL);
    waitpid(p, status, 0);
    return -1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int max_points = argc > 1 ? atoi(argv[1]) : 1000000;
    int stride = argc > 2 ? atoi(argv[2]) : 1;
    int first = argc > 3 ? atoi(argv[3]) : 1;
    struct gsync_device *dev = gsync_device_create(1 << 16, 1024, &dev_fd);
    sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (!dev || sh == MAP_FAILED) return 1;
    const int rounds = 3;

    make_objects(dev);
    int st;
    pid_t v = spawn(0, 900, rounds);
    if (reap(v, 30, &st) || !WIFEXITED(st) || WEXITSTATUS(st)) {
        printf("FAIL calibration run\n");
        return 1;
    }
    int points = sh->victim_points;
    if (points > max_points) points = max_points;
    printf("fault points in one workload run: %d (testing up to %d, stride %d)\n", sh->victim_points, points, stride);
    close_objects(dev);

    int hangs = 0, inconsistent = 0, broken = 0, killed = 0, tested = 0;
    for (int k = first; k <= points; k += stride) {
        make_objects(dev);
        uint32_t owner = 900 + (uint32_t)k;
        pid_t h = fork();
        if (h == 0) _exit(helper());
        v = spawn(k, owner, rounds);
        if (reap(v, 20, &st)) hangs++;
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL) killed++;
        gsync_recover(dev); /* what the wineserver does when a process dies */
        int hst;
        int hung = reap(h, 20, &hst);
        if (hung || !WIFEXITED(hst) || WEXITSTATUS(hst)) {
            hangs++;
            printf("  point %d: helper %s (exited %d, code %d, signal %d)\n", k, hung ? "HUNG" : "failed",
                   WIFEXITED(hst), WIFEXITED(hst) ? WEXITSTATUS(hst) : -1, WIFSIGNALED(hst) ? WTERMSIG(hst) : 0);
        }
        gsync_recover(dev); /* free the helper's records too, as after any process exit */
        int problems = gsync_check(dev);
        if (problems) {
            inconsistent++;
            printf("  point %d: %d consistency problems\n", k, problems);
        }
        int w = objects_work(dev, owner);
        if (w) {
            broken++;
            printf("  point %d: objects broken (step %d)\n", k, w);
        }
        close_objects(dev);
        tested++;
    }
    printf("fault injection: %d kill points tested, %d victims killed mid-work, %d hangs, %d inconsistent, %d broken\n",
           tested, killed, hangs, inconsistent, broken);
    return (hangs || inconsistent || broken) ? 1 : 0;
}
