/* GameOS Sync: Windows NT synchronization in shared memory with classic futexes. See gsync.h.

   Design (written for GameOS from the documented behavior of Linux's ntsync driver and of Windows' dispatcher):
   - One shared region per device: a header, a table of object slots and a table of waiter records. Every
     cross-reference is an index, never a pointer, because each process maps the region at its own address.
   - Each object has its own lock. A device-wide wait-all lock serializes wait-all operations; an object that has
     wait-all waiters (all_hint != 0) is locked through the device lock and marked dev_locked, so a wait-all can
     inspect and consume several objects atomically. Lock order: recovery lock, wait-all lock, object locks,
     allocation lock. Nobody waits for a lock while holding an object lock.
   - Each waiting thread owns a waiter record with one futex word, "signaled", and one queue entry per object.
     A signaler hands an object to a waiter by changing "signaled" from waiting to the object's index with a
     compare-and-swap; only the winner may consume the object and wake the thread. A waiter sleeps on its own word,
     so waiting on many objects needs only one classic futex.
   - signaled is -1 while the waiter is awake and still waiting, -2 once it has announced that it sleeps, and -3 if
     recovery found the thread dead. A signaler calls FUTEX_WAKE only if it replaced -2, so an uncontended wait or
     signal never enters the kernel.

   Crash safety (Android's memory killer can kill a process at any instruction):
   - A lock word holds the holder's thread id (plus a "sleepers" bit), set by the same atomic operation that takes
     the lock, so a held lock always names its holder.
   - Before changing an object, the holder writes a journal into the object: the old state, and for a handoff the
     waiter and index. Recovery rolls an unfinished change back, or completes a handoff the waiter already saw.
   - Queue entries carry a sequence number while queued, so recovery rebuilds every queue in its original order from
     the waiter records of live threads, and recomputes all_hint and dev_locked.
   - gsync_recover() stops the world by taking every lock (adopting locks whose holders are dead), repairs, and
     releases. A thread stuck on a lock for 200 ms checks the holder and runs recovery if it is dead; the wineserver
     also runs it whenever a process of the prefix dies.

   Copyright (C) 2026 Thomas Liviakis
   This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
   General Public License as published by the Free Software Foundation; either version 2.1 of the License, or
   (at your option) any later version. */

#define _GNU_SOURCE
#include "gsync.h"

#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>

#define GS_MAGIC 0x43595347u /* "GSYC" */
#define GS_VERSION 3u
#define GS_ENTRIES (GSYNC_MAX_WAIT_OBJECTS + 1) /* the objects plus the alert */
#define GS_SLOT_BITS 20u
#define GS_SLOT_MASK ((1u << GS_SLOT_BITS) - 1)
#define GS_GEN_MASK ((1u << (32 - GS_SLOT_BITS)) - 1)

enum { GS_FREE = 0, GS_SEM = 1, GS_MUTEX = 2, GS_EVENT = 3 };
#define GS_CLOSED 0x100u

#define GS_WAITING -1  /* awake, not yet given an object */
#define GS_SLEEPING -2 /* announced that it sleeps on the futex */
#define GS_DEAD -3     /* recovery found the thread dead; nobody may hand it anything */

/* A satisfied waiter's word holds a handoff ticket and the object index: (ticket << 7) | index. The ticket lets
   recovery tell exactly which handoff a waiter received. */
#define SIG_INDEX_BITS 7u
#define SIG_INDEX(v) ((uint32_t)(v) & ((1u << SIG_INDEX_BITS) - 1))
#define SIG_VALUE(tag, index) ((int32_t)((((tag) & 0xffffffu) << SIG_INDEX_BITS) | (index)))

#define LOCK_SLEEPERS 0x80000000u
#define LOCK_TID_MASK 0x7fffffffu
#define LOCK_CHECK_MS 200

enum { J_NONE = 0, J_OP = 1, J_CLAIM = 2 };

#ifdef GSYNC_FAULT_INJECTION
int gsync_fault_at;
int gsync_fault_count;
static void fault_hit(int line)
{
    char msg[48];
    int n = 0, v = line;
    const char *p = "fault at gsync.c line ";
    while (*p) msg[n++] = *p++;
    char d[8];
    int k = 0;
    do d[k++] = '0' + v % 10; while ((v /= 10) && k < 8);
    while (k) msg[n++] = d[--k];
    msg[n++] = '\n';
    if (write(2, msg, n) < 0) { }
    syscall(SYS_kill, getpid(), SIGKILL);
}
#define FAULT() do { if (++gsync_fault_count == gsync_fault_at) fault_hit(__LINE__); } while (0)
#else
#define FAULT() do { } while (0)
#endif

union gs_state {
    struct { uint32_t count, max; } sem;
    struct { uint32_t count, owner, ownerdead; } mutex;
    struct { uint32_t manual, signaled; } event;
    uint32_t raw[3];
};

struct gs_header {
    uint32_t magic, version;
    uint64_t size;
    uint32_t max_objects, max_waiters;
    uint32_t objects_offset, waiters_offset;
    _Atomic uint32_t recover_lock;  /* thread id of the recovering thread, 0 if none */
    _Atomic uint32_t wait_all_lock; /* the device lock */
    _Atomic uint32_t alloc_lock;    /* protects the free lists and the bump counters */
    _Atomic uint32_t queue_seq;
    _Atomic uint32_t claim_seq;     /* source of handoff tickets */
    _Atomic uint32_t recoveries;
    uint32_t obj_free_head, obj_used;
    uint32_t waiter_free_head, waiter_used;
};

struct gs_obj {
    _Atomic uint32_t lock;
    _Atomic uint32_t type;        /* GS_FREE, GS_SEM, GS_MUTEX or GS_EVENT, plus GS_CLOSED */
    _Atomic uint32_t gen;
    _Atomic uint32_t refs;        /* one for the open handle, one for each operation or wait using it */
    _Atomic uint32_t all_hint;    /* number of wait-all waiters queued on this object */
    _Atomic uint32_t dev_locked;  /* set while a holder of the device lock owns this object */
    uint32_t free_next;
    union gs_state u;
    uint32_t any_head, any_tail;  /* queue entry ids, 0 = none; protected by the object lock */
    uint32_t all_head, all_tail;  /* protected by the device lock */
    /* Journal of the change in progress, protected like the state. */
    uint32_t j_kind, j_waiter, j_index, j_pulse, j_tag;
    union gs_state j_old;
};

struct gs_entry {
    uint32_t prev, next, obj, index;
    uint32_t seq; /* nonzero while queued: the order in which it was queued */
};

struct gs_waiter {
    _Atomic int32_t signaled;
    uint32_t owner, all, ownerdead, count, tid, free_next, in_use;
    struct gs_entry e[GS_ENTRIES];
};

_Static_assert(sizeof(struct gs_obj) <= 128, "object slot must fit 128 bytes");

struct gsync_device {
    uint8_t *base;
    size_t size;
    int fd;
    struct gs_header *hdr;
    struct gs_obj *objs;
    struct gs_waiter *waiters;
    pthread_key_t key;
};

/* ---------------------------------------------------------------- small helpers */

static __thread uint32_t tls_tid;

static uint32_t gs_tid(void)
{
    if (!tls_tid) tls_tid = (uint32_t)syscall(SYS_gettid);
    return tls_tid;
}

static int tid_alive(uint32_t tid)
{
    if (!tid) return 0;
    if (syscall(SYS_kill, (pid_t)tid, 0) == 0) return 1;
    return errno == EPERM;
}

static inline void cpu_relax(void)
{
    __builtin_ia32_pause();
}

/* Shared futexes: the waiter and the signaler may live in different processes. */
static long futex_wait_abs(void *addr, uint32_t val, const struct timespec *abs, int realtime)
{
    int op = FUTEX_WAIT_BITSET | (realtime ? FUTEX_CLOCK_REALTIME : 0);
    return syscall(SYS_futex, addr, op, val, abs, NULL, FUTEX_BITSET_MATCH_ANY);
}

static long futex_wait_rel(void *addr, uint32_t val, const struct timespec *rel)
{
    return syscall(SYS_futex, addr, FUTEX_WAIT, val, rel, NULL, 0);
}

static void futex_wake(void *addr, int n)
{
    syscall(SYS_futex, addr, FUTEX_WAKE, n, NULL, NULL, 0);
}

int gsync_recover(struct gsync_device *dev);

enum { LK_OBJ, LK_ALL, LK_ALLOC };
struct gs_obj;
static void repair(struct gsync_device *dev, int kind, struct gs_obj *o);

/* Takes over a lock whose holder is dead. Returns 1 if this thread now holds it. */
static int try_adopt(_Atomic uint32_t *l, uint32_t v)
{
    uint32_t tid = v & LOCK_TID_MASK;
    if (!v || tid == gs_tid() || tid_alive(tid)) return 0;
    return atomic_compare_exchange_strong(l, &v, gs_tid() | LOCK_SLEEPERS);
}

/* A lock whose word holds the holder's thread id, plus LOCK_SLEEPERS when someone sleeps on it. If the holder dies,
   a waiter adopts the lock after LOCK_CHECK_MS and repairs the data it protects before using it. */
static void ll_lock(struct gsync_device *dev, _Atomic uint32_t *l, int kind, struct gs_obj *o)
{
    uint32_t self = gs_tid(), v = 0;
    if (atomic_compare_exchange_strong_explicit(l, &v, self, memory_order_acquire, memory_order_relaxed)) return;
    for (int i = 0; i < 100; i++) {
        cpu_relax();
        v = 0;
        if (atomic_load_explicit(l, memory_order_relaxed) == 0 &&
            atomic_compare_exchange_weak_explicit(l, &v, self, memory_order_acquire, memory_order_relaxed))
            return;
    }
    for (;;) {
        v = atomic_load_explicit(l, memory_order_relaxed);
        if (v == 0) {
            /* Others may still sleep on the word, so keep the sleepers bit when taking it. */
            if (atomic_compare_exchange_weak_explicit(l, &v, self | LOCK_SLEEPERS, memory_order_acquire,
                                                      memory_order_relaxed))
                return;
            continue;
        }
        if (!(v & LOCK_SLEEPERS) &&
            !atomic_compare_exchange_weak_explicit(l, &v, v | LOCK_SLEEPERS, memory_order_relaxed, memory_order_relaxed))
            continue;
        struct timespec rel = { 0, LOCK_CHECK_MS * 1000000L };
        long r = futex_wait_rel((void *)l, v | LOCK_SLEEPERS, &rel);
        if (r == -1 && errno == ETIMEDOUT && try_adopt(l, atomic_load(l))) {
            repair(dev, kind, o);
            return;
        }
    }
}

static void ll_unlock(_Atomic uint32_t *l)
{
    if (atomic_exchange_explicit(l, 0, memory_order_release) & LOCK_SLEEPERS) futex_wake((void *)l, 1);
}

static inline uint32_t waiter_index(struct gsync_device *dev, struct gs_waiter *w)
{
    return (uint32_t)(w - dev->waiters);
}

static inline struct gs_waiter *entry_waiter(struct gsync_device *dev, uint32_t id)
{
    return &dev->waiters[(id - 1) / GS_ENTRIES];
}

static inline struct gs_entry *entry_ptr(struct gsync_device *dev, uint32_t id)
{
    return &dev->waiters[(id - 1) / GS_ENTRIES].e[(id - 1) % GS_ENTRIES];
}

static inline uint32_t entry_id(struct gsync_device *dev, struct gs_waiter *w, uint32_t i)
{
    return waiter_index(dev, w) * GS_ENTRIES + i + 1;
}

static void list_link(struct gsync_device *dev, uint32_t *head, uint32_t *tail, uint32_t id)
{
    struct gs_entry *e = entry_ptr(dev, id);
    e->next = 0;
    e->prev = *tail;
    FAULT();
    if (*tail) entry_ptr(dev, *tail)->next = id;
    else *head = id;
    FAULT();
    *tail = id;
}

static void list_append(struct gsync_device *dev, uint32_t *head, uint32_t *tail, uint32_t id)
{
    entry_ptr(dev, id)->seq = atomic_fetch_add(&dev->hdr->queue_seq, 1) + 1;
    list_link(dev, head, tail, id);
}

static void list_remove(struct gsync_device *dev, uint32_t *head, uint32_t *tail, uint32_t id)
{
    struct gs_entry *e = entry_ptr(dev, id);
    if (e->prev) entry_ptr(dev, e->prev)->next = e->next;
    else *head = e->next;
    FAULT();
    if (e->next) entry_ptr(dev, e->next)->prev = e->prev;
    else *tail = e->prev;
    e->prev = e->next = 0;
    e->seq = 0;
}

/* ---------------------------------------------------------------- object references */

static void slot_free(struct gsync_device *dev, struct gs_obj *o)
{
    uint32_t slot = (uint32_t)(o - dev->objs) + 1;
    ll_lock(dev, &dev->hdr->alloc_lock, LK_ALLOC, NULL);
    atomic_store(&o->type, GS_FREE);
    FAULT();
    o->free_next = dev->hdr->obj_free_head;
    dev->hdr->obj_free_head = slot;
    ll_unlock(&dev->hdr->alloc_lock);
}

static struct gs_obj *obj_get(struct gsync_device *dev, gsync_handle h)
{
    uint32_t slot = h & GS_SLOT_MASK, gen = h >> GS_SLOT_BITS;
    if (!slot || slot > dev->hdr->max_objects) return NULL;
    struct gs_obj *o = &dev->objs[slot - 1];
    uint32_t r = atomic_load(&o->refs);
    do {
        if (!r) return NULL;
    } while (!atomic_compare_exchange_weak(&o->refs, &r, r + 1));
    uint32_t t = atomic_load(&o->type);
    if (atomic_load(&o->gen) != gen || t == GS_FREE || (t & GS_CLOSED)) {
        /* A stale handle reached a reused slot: give the reference back. */
        if (atomic_fetch_sub(&o->refs, 1) == 1) slot_free(dev, o);
        return NULL;
    }
    return o;
}

static void obj_put(struct gsync_device *dev, struct gs_obj *o)
{
    if (atomic_fetch_sub(&o->refs, 1) == 1) slot_free(dev, o);
}

static int obj_new(struct gsync_device *dev, uint32_t type, const union gs_state *init, gsync_handle *h)
{
    struct gs_header *hdr = dev->hdr;
    uint32_t slot;
    ll_lock(dev, &hdr->alloc_lock, LK_ALLOC, NULL);
    if (hdr->obj_free_head) {
        slot = hdr->obj_free_head;
        hdr->obj_free_head = dev->objs[slot - 1].free_next;
    } else if (hdr->obj_used < hdr->max_objects) {
        slot = hdr->obj_used + 1;
    } else {
        ll_unlock(&hdr->alloc_lock);
        return -ENOSPC;
    }
    struct gs_obj *o = &dev->objs[slot - 1];
    uint32_t gen = (atomic_load(&o->gen) + 1) & GS_GEN_MASK;
    atomic_store(&o->gen, gen);
    o->any_head = o->any_tail = o->all_head = o->all_tail = 0;
    o->j_kind = J_NONE;
    o->j_pulse = 0;
    atomic_store(&o->all_hint, 0);
    atomic_store(&o->dev_locked, 0);
    o->u = *init;
    atomic_store(&o->refs, 1);
    atomic_store(&o->type, type); /* the slot counts as used from here on */
    if (slot > hdr->obj_used) hdr->obj_used = slot;
    ll_unlock(&hdr->alloc_lock);
    *h = (gen << GS_SLOT_BITS) | slot;
    return 0;
}

/* ---------------------------------------------------------------- object locking */

static void obj_lock(struct gsync_device *dev, struct gs_obj *o)
{
    for (;;) {
        ll_lock(dev, &o->lock, LK_OBJ, o);
        if (!atomic_load_explicit(&o->dev_locked, memory_order_relaxed)) return;
        ll_unlock(&o->lock);
        /* A holder of the device lock owns this object; wait for it to finish. */
        ll_lock(dev, &dev->hdr->wait_all_lock, LK_ALL, NULL);
        ll_unlock(&dev->hdr->wait_all_lock);
    }
}

static void obj_unlock(struct gs_obj *o)
{
    ll_unlock(&o->lock);
}

static void dev_lock_obj(struct gsync_device *dev, struct gs_obj *o)
{
    ll_lock(dev, &o->lock, LK_OBJ, o);
    atomic_store_explicit(&o->dev_locked, 1, memory_order_relaxed);
    ll_unlock(&o->lock);
}

static void dev_unlock_obj(struct gsync_device *dev, struct gs_obj *o)
{
    ll_lock(dev, &o->lock, LK_OBJ, o);
    atomic_store_explicit(&o->dev_locked, 0, memory_order_relaxed);
    ll_unlock(&o->lock);
}

/* Locks an object for a state change. Returns 1 if the device lock was taken because wait-all waiters exist. */
static int lock_obj(struct gsync_device *dev, struct gs_obj *o)
{
    obj_lock(dev, o);
    if (!atomic_load(&o->all_hint)) return 0;
    obj_unlock(o);
    ll_lock(dev, &dev->hdr->wait_all_lock, LK_ALL, NULL);
    dev_lock_obj(dev, o);
    return 1;
}

static void unlock_obj(struct gsync_device *dev, struct gs_obj *o, int all)
{
    if (all) {
        dev_unlock_obj(dev, o);
        ll_unlock(&dev->hdr->wait_all_lock);
    } else {
        obj_unlock(o);
    }
}

/* ---------------------------------------------------------------- object state */

static inline uint32_t obj_type(struct gs_obj *o)
{
    return atomic_load_explicit(&o->type, memory_order_relaxed) & ~GS_CLOSED;
}

static int is_signaled(struct gs_obj *o, uint32_t owner)
{
    switch (obj_type(o)) {
    case GS_SEM: return o->u.sem.count != 0;
    case GS_MUTEX:
        if (o->u.mutex.owner && o->u.mutex.owner != owner) return 0;
        return o->u.mutex.count < UINT32_MAX;
    case GS_EVENT: return o->u.event.signaled != 0;
    }
    return 0;
}

/* Gives the object to a waiter with the given owner. Sets *ownerdead for an abandoned mutex. */
static void consume(struct gs_obj *o, uint32_t owner, uint32_t *ownerdead)
{
    switch (obj_type(o)) {
    case GS_SEM: o->u.sem.count--; break;
    case GS_MUTEX:
        if (o->u.mutex.ownerdead) {
            o->u.mutex.ownerdead = 0;
            *ownerdead = 1;
        }
        FAULT();
        o->u.mutex.count++;
        o->u.mutex.owner = owner;
        break;
    case GS_EVENT:
        if (!o->u.event.manual) o->u.event.signaled = 0;
        break;
    }
}

static void journal_op(struct gs_obj *o)
{
    o->j_old = o->u;
    atomic_signal_fence(memory_order_seq_cst);
    o->j_kind = J_OP;
    atomic_thread_fence(memory_order_release);
}

static uint32_t next_tag(struct gsync_device *dev)
{
    return atomic_fetch_add_explicit(&dev->hdr->claim_seq, 1, memory_order_relaxed) & 0xffffffu;
}

static void journal_claim(struct gsync_device *dev, struct gs_obj *o, struct gs_waiter *w, uint32_t index, uint32_t tag)
{
    o->j_old = o->u;
    o->j_waiter = waiter_index(dev, w) + 1;
    o->j_index = index;
    o->j_tag = tag;
    atomic_signal_fence(memory_order_seq_cst);
    o->j_kind = J_CLAIM;
    atomic_thread_fence(memory_order_release);
}

static void journal_done(struct gs_obj *o)
{
    atomic_thread_fence(memory_order_release);
    o->j_kind = J_NONE;
}

/* Claims waiter w with a ticketed value. Returns the previous value (negative) on success, or 0 if the waiter was
   already satisfied or is dead. */
static int32_t claim(struct gs_waiter *w, int32_t value)
{
    int32_t v = atomic_load(&w->signaled);
    while (v == GS_WAITING || v == GS_SLEEPING) {
        if (atomic_compare_exchange_weak(&w->signaled, &v, value)) return v;
    }
    return 0;
}

static void wake_if_sleeping(struct gs_waiter *w, int32_t prev)
{
    if (prev == GS_SLEEPING) futex_wake((void *)&w->signaled, 1);
}

/* Hands a signaled object to its wait-any waiters in queue order. Caller holds the object's lock. */
static void try_wake_any(struct gsync_device *dev, struct gs_obj *o)
{
    uint32_t type = obj_type(o);
    for (uint32_t id = o->any_head; id; id = entry_ptr(dev, id)->next) {
        struct gs_waiter *w = entry_waiter(dev, id);
        if (!is_signaled(o, w->owner)) {
            if (type == GS_MUTEX) continue; /* owned by someone else: a later waiter may be the owner */
            break;
        }
        uint32_t index = entry_ptr(dev, id)->index, tag = next_tag(dev);
        journal_claim(dev, o, w, index, tag);
        FAULT();
        int32_t prev = claim(w, SIG_VALUE(tag, index));
        FAULT();
        if (prev) consume(o, w->owner, &w->ownerdead);
        FAULT();
        if (prev) wake_if_sleeping(w, prev);
        journal_done(o);
    }
}

/* Tries to satisfy wait-all waiter w. Caller holds the device lock; locked_obj (if any) is already dev-locked. */
static void try_wake_all(struct gsync_device *dev, struct gs_waiter *w, struct gs_obj *locked_obj)
{
    uint32_t n = w->count;
    if (!n) return; /* only the alert can satisfy a wait-all on no objects */
    struct gs_obj *objs[GSYNC_MAX_WAIT_OBJECTS];
    for (uint32_t i = 0; i < n; i++) {
        objs[i] = &dev->objs[w->e[i].obj - 1];
        if (objs[i] != locked_obj) dev_lock_obj(dev, objs[i]);
    }
    int can = 1;
    for (uint32_t i = 0; i < n && can; i++)
        if (!is_signaled(objs[i], w->owner)) can = 0;
    if (can) {
        uint32_t tag = next_tag(dev);
        for (uint32_t i = 0; i < n; i++) journal_claim(dev, objs[i], w, 0, tag);
        FAULT();
        int32_t prev = claim(w, SIG_VALUE(tag, 0));
        FAULT();
        for (uint32_t i = 0; i < n; i++) {
            if (prev) consume(objs[i], w->owner, &w->ownerdead);
            FAULT();
        }
        if (prev) wake_if_sleeping(w, prev);
        for (uint32_t i = 0; i < n; i++) journal_done(objs[i]);
    }
    for (uint32_t i = 0; i < n; i++)
        if (objs[i] != locked_obj) dev_unlock_obj(dev, objs[i]);
}

static void try_wake_all_obj(struct gsync_device *dev, struct gs_obj *o)
{
    for (uint32_t id = o->all_head; id; id = entry_ptr(dev, id)->next)
        try_wake_all(dev, entry_waiter(dev, id), o);
}

/* After a state change that may signal o: wait-all waiters first, then wait-any waiters. */
static void wake_after_change(struct gsync_device *dev, struct gs_obj *o, int all)
{
    if (all) try_wake_all_obj(dev, o);
    try_wake_any(dev, o);
}

/* ---------------------------------------------------------------- device */

static void waiter_release(struct gsync_device *dev, uint32_t idx)
{
    struct gs_header *hdr = dev->hdr;
    struct gs_waiter *w = &dev->waiters[idx - 1];
    ll_lock(dev, &hdr->alloc_lock, LK_ALLOC, NULL);
    w->in_use = 0;
    FAULT();
    w->free_next = hdr->waiter_free_head;
    hdr->waiter_free_head = idx;
    ll_unlock(&hdr->alloc_lock);
}

static __thread struct gsync_device *tls_dev;
static __thread uint32_t tls_waiter;

/* A forked child must not reuse its parent's thread id or wait record: both belong to the parent's thread. */
static void after_fork_child(void)
{
    tls_tid = 0;
    tls_waiter = 0;
    tls_dev = NULL;
}

static pthread_once_t atfork_once = PTHREAD_ONCE_INIT;

static void register_atfork(void)
{
    pthread_atfork(NULL, NULL, after_fork_child);
}

static void waiter_key_destructor(void *arg)
{
    if (tls_dev && tls_waiter) waiter_release(tls_dev, tls_waiter);
    tls_waiter = 0;
    tls_dev = NULL;
    (void)arg;
}

static struct gs_waiter *waiter_self(struct gsync_device *dev)
{
    if (tls_dev == dev && tls_waiter) {
        struct gs_waiter *w = &dev->waiters[tls_waiter - 1];
        if (w->in_use && w->tid == gs_tid()) return w;
    }
    struct gs_header *hdr = dev->hdr;
    uint32_t idx = 0;
    ll_lock(dev, &hdr->alloc_lock, LK_ALLOC, NULL);
    if (hdr->waiter_free_head) {
        idx = hdr->waiter_free_head;
        hdr->waiter_free_head = dev->waiters[idx - 1].free_next;
    } else if (hdr->waiter_used < hdr->max_waiters) {
        idx = ++hdr->waiter_used;
    }
    if (idx) {
        struct gs_waiter *w = &dev->waiters[idx - 1];
        for (uint32_t i = 0; i < GS_ENTRIES; i++) w->e[i].seq = 0;
        w->tid = gs_tid();
        w->in_use = 1;
    }
    ll_unlock(&hdr->alloc_lock);
    if (!idx) return NULL;
    tls_dev = dev;
    tls_waiter = idx;
    pthread_setspecific(dev->key, dev);
    return &dev->waiters[idx - 1];
}

static size_t region_size(uint32_t max_objects, uint32_t max_waiters, uint32_t *obj_off, uint32_t *wait_off)
{
    size_t page = 4096;
    size_t o = (sizeof(struct gs_header) + 127) & ~(size_t)127;
    size_t w = (o + (size_t)max_objects * sizeof(struct gs_obj) + 63) & ~(size_t)63;
    size_t end = w + (size_t)max_waiters * sizeof(struct gs_waiter);
    *obj_off = (uint32_t)o;
    *wait_off = (uint32_t)w;
    return (end + page - 1) & ~(page - 1);
}

static struct gsync_device *map_device(int fd, size_t size)
{
    pthread_once(&atfork_once, register_atfork);
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) return NULL;
    struct gsync_device *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        munmap(base, size);
        return NULL;
    }
    dev->base = base;
    dev->size = size;
    dev->fd = fd;
    dev->hdr = base;
    if (pthread_key_create(&dev->key, waiter_key_destructor)) {
        munmap(base, size);
        free(dev);
        return NULL;
    }
    return dev;
}

static void bind_tables(struct gsync_device *dev)
{
    dev->objs = (struct gs_obj *)(dev->base + dev->hdr->objects_offset);
    dev->waiters = (struct gs_waiter *)(dev->base + dev->hdr->waiters_offset);
}

struct gsync_device *gsync_device_create(uint32_t max_objects, uint32_t max_waiters, int *fd_out)
{
    if (!max_objects || max_objects > GS_SLOT_MASK || !max_waiters || max_waiters > 65536) {
        errno = EINVAL;
        return NULL;
    }
    uint32_t obj_off, wait_off;
    size_t size = region_size(max_objects, max_waiters, &obj_off, &wait_off);
    int fd = memfd_create("gameos-gsync", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return NULL;
    }
    struct gsync_device *dev = map_device(fd, size);
    if (!dev) {
        close(fd);
        return NULL;
    }
    struct gs_header *hdr = dev->hdr;
    hdr->size = size;
    hdr->max_objects = max_objects;
    hdr->max_waiters = max_waiters;
    hdr->objects_offset = obj_off;
    hdr->waiters_offset = wait_off;
    hdr->version = GS_VERSION;
    atomic_thread_fence(memory_order_release);
    hdr->magic = GS_MAGIC;
    bind_tables(dev);
    if (fd_out) *fd_out = fd;
    return dev;
}

struct gsync_device *gsync_device_open(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return NULL;
    if ((size_t)st.st_size < sizeof(struct gs_header)) {
        errno = EINVAL;
        return NULL;
    }
    struct gsync_device *dev = map_device(fd, (size_t)st.st_size);
    if (!dev) return NULL;
    if (dev->hdr->magic != GS_MAGIC || dev->hdr->version != GS_VERSION || dev->hdr->size != (uint64_t)st.st_size) {
        gsync_device_close(dev);
        errno = EINVAL;
        return NULL;
    }
    bind_tables(dev);
    return dev;
}

void gsync_device_close(struct gsync_device *dev)
{
    if (!dev) return;
    if (tls_dev == dev) waiter_key_destructor(NULL);
    pthread_key_delete(dev->key);
    munmap(dev->base, dev->size);
    free(dev);
}

/* ---------------------------------------------------------------- creation and closing */

int gsync_create_sem(struct gsync_device *dev, uint32_t count, uint32_t max, gsync_handle *out)
{
    if (count > max) return -EINVAL;
    union gs_state s = { .sem = { count, max } };
    return obj_new(dev, GS_SEM, &s, out);
}

int gsync_create_mutex(struct gsync_device *dev, uint32_t owner, uint32_t count, gsync_handle *out)
{
    if (!owner != !count) return -EINVAL;
    union gs_state s = { .mutex = { count, owner, 0 } };
    return obj_new(dev, GS_MUTEX, &s, out);
}

int gsync_create_event(struct gsync_device *dev, int manual, int signaled, gsync_handle *out)
{
    union gs_state s = { .event = { manual ? 1u : 0u, signaled ? 1u : 0u } };
    return obj_new(dev, GS_EVENT, &s, out);
}

int gsync_close(struct gsync_device *dev, gsync_handle h)
{
    struct gs_obj *o = obj_get(dev, h);
    if (!o) return -EINVAL;
    uint32_t t = atomic_load(&o->type);
    int closed_now = 0;
    while (!(t & GS_CLOSED)) {
        if (atomic_compare_exchange_weak(&o->type, &t, t | GS_CLOSED)) {
            closed_now = 1;
            break;
        }
    }
    obj_put(dev, o);                /* the reference taken by obj_get */
    if (!closed_now) return -EINVAL;
    obj_put(dev, o);                /* the handle's own reference; waiters keep theirs */
    return 0;
}

/* ---------------------------------------------------------------- signal operations */

static struct gs_obj *get_typed(struct gsync_device *dev, gsync_handle h, uint32_t type)
{
    struct gs_obj *o = obj_get(dev, h);
    if (o && obj_type(o) != type) {
        obj_put(dev, o);
        return NULL;
    }
    return o;
}

int gsync_sem_release(struct gsync_device *dev, gsync_handle h, uint32_t count, uint32_t *prev_count)
{
    struct gs_obj *o = get_typed(dev, h, GS_SEM);
    if (!o) return -EINVAL;
    int ret = 0;
    int all = lock_obj(dev, o);
    uint32_t prev = o->u.sem.count;
    if (prev + count < prev || prev + count > o->u.sem.max) {
        ret = -EOVERFLOW;
    } else {
        journal_op(o);
        FAULT();
        o->u.sem.count = prev + count;
        journal_done(o);
        wake_after_change(dev, o, all);
    }
    unlock_obj(dev, o, all);
    obj_put(dev, o);
    if (!ret && prev_count) *prev_count = prev;
    return ret;
}

int gsync_mutex_unlock(struct gsync_device *dev, gsync_handle h, uint32_t owner, uint32_t *prev_count)
{
    if (!owner) return -EINVAL;
    struct gs_obj *o = get_typed(dev, h, GS_MUTEX);
    if (!o) return -EINVAL;
    int ret = 0;
    int all = lock_obj(dev, o);
    uint32_t prev = o->u.mutex.count;
    if (o->u.mutex.owner != owner) {
        ret = -EPERM;
    } else {
        journal_op(o);
        o->u.mutex.count--;
        FAULT();
        if (!o->u.mutex.count) o->u.mutex.owner = 0;
        journal_done(o);
        if (!o->u.mutex.count) wake_after_change(dev, o, all);
    }
    unlock_obj(dev, o, all);
    obj_put(dev, o);
    if (!ret && prev_count) *prev_count = prev;
    return ret;
}

int gsync_mutex_kill(struct gsync_device *dev, gsync_handle h, uint32_t owner)
{
    if (!owner) return -EINVAL;
    struct gs_obj *o = get_typed(dev, h, GS_MUTEX);
    if (!o) return -EINVAL;
    int ret = 0;
    int all = lock_obj(dev, o);
    if (o->u.mutex.owner != owner) {
        ret = -EPERM;
    } else {
        journal_op(o);
        o->u.mutex.ownerdead = 1;
        FAULT();
        o->u.mutex.owner = 0;
        o->u.mutex.count = 0;
        journal_done(o);
        wake_after_change(dev, o, all);
    }
    unlock_obj(dev, o, all);
    obj_put(dev, o);
    return ret;
}

static int event_change(struct gsync_device *dev, gsync_handle h, int pulse, uint32_t *prev_signaled)
{
    struct gs_obj *o = get_typed(dev, h, GS_EVENT);
    if (!o) return -EINVAL;
    int all = lock_obj(dev, o);
    uint32_t prev = o->u.event.signaled;
    journal_op(o);
    o->j_pulse = pulse; /* if the signaler dies during the wakes, recovery still clears a pulsed event */
    FAULT();
    o->u.event.signaled = 1;
    journal_done(o);
    wake_after_change(dev, o, all);
    if (pulse) o->u.event.signaled = 0; /* exactly the waiters queued at this moment were woken */
    o->j_pulse = 0;
    unlock_obj(dev, o, all);
    obj_put(dev, o);
    if (prev_signaled) *prev_signaled = prev;
    return 0;
}

int gsync_event_set(struct gsync_device *dev, gsync_handle h, uint32_t *prev_signaled)
{
    return event_change(dev, h, 0, prev_signaled);
}

int gsync_event_pulse(struct gsync_device *dev, gsync_handle h, uint32_t *prev_signaled)
{
    return event_change(dev, h, 1, prev_signaled);
}

int gsync_event_reset(struct gsync_device *dev, gsync_handle h, uint32_t *prev_signaled)
{
    struct gs_obj *o = get_typed(dev, h, GS_EVENT);
    if (!o) return -EINVAL;
    int all = lock_obj(dev, o);
    uint32_t prev = o->u.event.signaled;
    o->u.event.signaled = 0; /* a single store: nothing to journal */
    unlock_obj(dev, o, all);
    obj_put(dev, o);
    if (prev_signaled) *prev_signaled = prev;
    return 0;
}

/* ---------------------------------------------------------------- read operations */

int gsync_sem_read(struct gsync_device *dev, gsync_handle h, uint32_t *count, uint32_t *max)
{
    struct gs_obj *o = get_typed(dev, h, GS_SEM);
    if (!o) return -EINVAL;
    obj_lock(dev, o);
    if (count) *count = o->u.sem.count;
    if (max) *max = o->u.sem.max;
    obj_unlock(o);
    obj_put(dev, o);
    return 0;
}

int gsync_mutex_read(struct gsync_device *dev, gsync_handle h, uint32_t *owner, uint32_t *count)
{
    struct gs_obj *o = get_typed(dev, h, GS_MUTEX);
    if (!o) return -EINVAL;
    obj_lock(dev, o);
    uint32_t dead = o->u.mutex.ownerdead;
    if (owner) *owner = o->u.mutex.owner;
    if (count) *count = o->u.mutex.count;
    obj_unlock(o);
    obj_put(dev, o);
    return dead ? -EOWNERDEAD : 0;
}

int gsync_event_read(struct gsync_device *dev, gsync_handle h, uint32_t *manual, uint32_t *signaled)
{
    struct gs_obj *o = get_typed(dev, h, GS_EVENT);
    if (!o) return -EINVAL;
    obj_lock(dev, o);
    if (manual) *manual = o->u.event.manual;
    if (signaled) *signaled = o->u.event.signaled;
    obj_unlock(o);
    obj_put(dev, o);
    return 0;
}

/* ---------------------------------------------------------------- waits */

/* Takes references on every object and the alert. Returns 0 or -EINVAL (no references kept). */
static int get_wait_objs(struct gsync_device *dev, const gsync_handle *handles, uint32_t count, gsync_handle alert,
                         struct gs_obj **objs, struct gs_obj **alert_obj)
{
    uint32_t got = 0;
    *alert_obj = NULL;
    for (; got < count; got++) {
        objs[got] = obj_get(dev, handles[got]);
        if (!objs[got]) goto fail;
    }
    if (alert) {
        *alert_obj = get_typed(dev, alert, GS_EVENT);
        if (!*alert_obj) goto fail;
    }
    return 0;
fail:
    while (got--) obj_put(dev, objs[got]);
    return -EINVAL;
}

static void put_wait_objs(struct gsync_device *dev, struct gs_obj **objs, uint32_t count, struct gs_obj *alert_obj)
{
    for (uint32_t i = 0; i < count; i++) obj_put(dev, objs[i]);
    if (alert_obj) obj_put(dev, alert_obj);
}

static int validate_wait(uint32_t count, uint32_t flags, uint32_t owner)
{
    if (!owner || count > GSYNC_MAX_WAIT_OBJECTS || (flags & ~GSYNC_WAIT_REALTIME)) return -EINVAL;
    return 0;
}

/* Sleeps until w->signaled holds an index or the deadline passes. */
static void sleep_on(struct gs_waiter *w, uint64_t timeout, uint32_t flags)
{
    struct timespec ts, *tp = NULL;
    if (timeout != GSYNC_TIMEOUT_INFINITE) {
        ts.tv_sec = (time_t)(timeout / 1000000000ull);
        ts.tv_nsec = (long)(timeout % 1000000000ull);
        tp = &ts;
    }
    for (;;) {
        int32_t v = atomic_load(&w->signaled);
        if (v >= 0) return;
        if (v == GS_WAITING) {
            int32_t expect = GS_WAITING;
            if (!atomic_compare_exchange_strong(&w->signaled, &expect, GS_SLEEPING)) continue;
        }
        long r = futex_wait_abs((void *)&w->signaled, (uint32_t)GS_SLEEPING, tp, flags & GSYNC_WAIT_REALTIME);
        if (r == -1 && errno == ETIMEDOUT) return;
    }
}

static inline struct gs_obj *wait_obj(struct gs_obj **objs, uint32_t count, struct gs_obj *alert_obj, uint32_t i)
{
    return i < count ? objs[i] : alert_obj;
}

int gsync_wait_any(struct gsync_device *dev, const gsync_handle *handles, uint32_t count, uint64_t timeout,
                   uint32_t flags, uint32_t owner, gsync_handle alert, uint32_t *index)
{
    int ret = validate_wait(count, flags, owner);
    if (ret) return ret;
    struct gs_obj *objs[GSYNC_MAX_WAIT_OBJECTS], *alert_obj;
    if (get_wait_objs(dev, handles, count, alert, objs, &alert_obj)) return -EINVAL;
    uint32_t total = count + (alert_obj ? 1 : 0);

    /* Fast path: take the first signaled object in index order, the alert last. No system call. */
    for (uint32_t i = 0; i < total; i++) {
        struct gs_obj *o = wait_obj(objs, count, alert_obj, i);
        int all = lock_obj(dev, o);
        if (is_signaled(o, owner)) {
            uint32_t dead = 0;
            journal_op(o); /* if this thread dies here, recovery rolls the take back */
            consume(o, owner, &dead);
            journal_done(o);
            unlock_obj(dev, o, all);
            put_wait_objs(dev, objs, count, alert_obj);
            *index = i;
            return dead ? -EOWNERDEAD : 0;
        }
        unlock_obj(dev, o, all);
    }

    struct gs_waiter *w = waiter_self(dev);
    if (!w) {
        put_wait_objs(dev, objs, count, alert_obj);
        return -ENOMEM;
    }
    atomic_store(&w->signaled, GS_WAITING);
    w->owner = owner;
    w->all = 0;
    w->ownerdead = 0;
    w->count = count;

    /* Queue on every object, then give each object in index order a chance to satisfy the wait. */
    for (uint32_t i = 0; i < total; i++) {
        struct gs_obj *o = wait_obj(objs, count, alert_obj, i);
        w->e[i].obj = (uint32_t)(o - dev->objs) + 1;
        w->e[i].index = i;
        int all = lock_obj(dev, o);
        list_append(dev, &o->any_head, &o->any_tail, entry_id(dev, w, i));
        unlock_obj(dev, o, all);
    }
    for (uint32_t i = 0; i < total && atomic_load(&w->signaled) < 0; i++) {
        struct gs_obj *o = wait_obj(objs, count, alert_obj, i);
        int all = lock_obj(dev, o);
        try_wake_any(dev, o);
        unlock_obj(dev, o, all);
    }

    sleep_on(w, timeout, flags);

    for (uint32_t i = 0; i < total; i++) {
        struct gs_obj *o = wait_obj(objs, count, alert_obj, i);
        int all = lock_obj(dev, o);
        list_remove(dev, &o->any_head, &o->any_tail, entry_id(dev, w, i));
        unlock_obj(dev, o, all);
    }

    int32_t s = atomic_load(&w->signaled);
    if (s >= 0) {
        *index = SIG_INDEX(s);
        ret = w->ownerdead ? -EOWNERDEAD : 0;
    } else {
        ret = -ETIMEDOUT;
    }
    put_wait_objs(dev, objs, count, alert_obj);
    return ret;
}

int gsync_wait_all(struct gsync_device *dev, const gsync_handle *handles, uint32_t count, uint64_t timeout,
                   uint32_t flags, uint32_t owner, gsync_handle alert, uint32_t *index)
{
    int ret = validate_wait(count, flags, owner);
    if (ret) return ret;
    /* The same object twice, or the alert among the objects, is invalid. */
    for (uint32_t i = 0; i < count; i++) {
        if (alert && (handles[i] & GS_SLOT_MASK) == (alert & GS_SLOT_MASK)) return -EINVAL;
        for (uint32_t j = 0; j < i; j++)
            if ((handles[i] & GS_SLOT_MASK) == (handles[j] & GS_SLOT_MASK)) return -EINVAL;
    }
    struct gs_obj *objs[GSYNC_MAX_WAIT_OBJECTS], *alert_obj;
    if (get_wait_objs(dev, handles, count, alert, objs, &alert_obj)) return -EINVAL;
    struct gs_waiter *w = waiter_self(dev);
    if (!w) {
        put_wait_objs(dev, objs, count, alert_obj);
        return -ENOMEM;
    }
    struct gs_header *hdr = dev->hdr;
    atomic_store(&w->signaled, GS_WAITING);
    w->owner = owner;
    w->all = 1;
    w->ownerdead = 0;
    w->count = count;

    ll_lock(dev, &hdr->wait_all_lock, LK_ALL, NULL);
    for (uint32_t i = 0; i < count; i++) {
        w->e[i].obj = (uint32_t)(objs[i] - dev->objs) + 1;
        w->e[i].index = i;
        list_append(dev, &objs[i]->all_head, &objs[i]->all_tail, entry_id(dev, w, i));
        atomic_fetch_add(&objs[i]->all_hint, 1);
    }
    if (alert_obj) {
        w->e[count].obj = (uint32_t)(alert_obj - dev->objs) + 1;
        w->e[count].index = count;
        obj_lock(dev, alert_obj);
        list_append(dev, &alert_obj->any_head, &alert_obj->any_tail, entry_id(dev, w, count));
        obj_unlock(alert_obj);
    }
    try_wake_all(dev, w, NULL);
    ll_unlock(&hdr->wait_all_lock);

    /* The alert is checked only after the objects, as in Windows and ntsync. */
    if (alert_obj && atomic_load(&w->signaled) < 0) {
        int all = lock_obj(dev, alert_obj);
        try_wake_any(dev, alert_obj);
        unlock_obj(dev, alert_obj, all);
    }

    sleep_on(w, timeout, flags);

    ll_lock(dev, &hdr->wait_all_lock, LK_ALL, NULL);
    for (uint32_t i = 0; i < count; i++) {
        list_remove(dev, &objs[i]->all_head, &objs[i]->all_tail, entry_id(dev, w, i));
        atomic_fetch_sub(&objs[i]->all_hint, 1);
    }
    ll_unlock(&hdr->wait_all_lock);
    if (alert_obj) {
        int all = lock_obj(dev, alert_obj);
        list_remove(dev, &alert_obj->any_head, &alert_obj->any_tail, entry_id(dev, w, count));
        unlock_obj(dev, alert_obj, all);
    }

    int32_t s = atomic_load(&w->signaled);
    if (s >= 0 && SIG_INDEX(s) == 0 && count) {
        *index = 0;
        ret = w->ownerdead ? -EOWNERDEAD : 0;
    } else if (alert_obj && s >= 0 && SIG_INDEX(s) == count) {
        *index = count;
        ret = 0;
    } else {
        ret = -ETIMEDOUT;
    }
    put_wait_objs(dev, objs, count, alert_obj);
    return ret;
}

/* ---------------------------------------------------------------- recovery */

/* Applies an unfinished journal: roll a change back, or complete a handoff that the waiter already received. */
static void journal_fix(struct gsync_device *dev, struct gs_obj *o)
{
#ifdef GSYNC_FAULT_INJECTION
    if (o->j_kind && getenv("GSYNC_TRACE")) {
        struct gs_waiter *tw = o->j_waiter ? &dev->waiters[o->j_waiter - 1] : NULL;
        fprintf(stderr, "journal_fix by %u: slot %u kind %u waiter %u (tid %u signaled %d) index %u; state %u/%u/%u old %u/%u/%u\n",
                gs_tid(), (unsigned)(o - dev->objs) + 1, o->j_kind, o->j_waiter, tw ? tw->tid : 0,
                tw ? (int)atomic_load(&tw->signaled) : 99, o->j_index, o->u.raw[0], o->u.raw[1], o->u.raw[2],
                o->j_old.raw[0], o->j_old.raw[1], o->j_old.raw[2]);
    }
#endif
    if (o->j_kind == J_OP) {
        o->u = o->j_old;
    } else if (o->j_kind == J_CLAIM) {
        o->u = o->j_old;
        struct gs_waiter *w = o->j_waiter ? &dev->waiters[o->j_waiter - 1] : NULL;
        if (w && atomic_load(&w->signaled) == SIG_VALUE(o->j_tag, o->j_index)) {
            consume(o, w->owner, &w->ownerdead);
            futex_wake((void *)&w->signaled, 1);
        }
    }
    o->j_kind = J_NONE;
    if (o->j_pulse) {
        o->u.event.signaled = 0;
        o->j_pulse = 0;
    }
}

struct seq_id {
    uint32_t seq, id;
};

static int cmp_seq_id(const void *a, const void *b)
{
    const struct seq_id *x = a, *y = b;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* Collects the queued entries of live waiters that belong on an object's any-list (all = 0) or all-list (all = 1);
   o == NULL collects all-list entries for every object. Entries of dead waiters are unqueued (seq cleared). */
static size_t collect_entries(struct gsync_device *dev, uint32_t slot, int all, struct seq_id *out)
{
    size_t n = 0;
    for (uint32_t i = 0; i < dev->hdr->waiter_used; i++) {
        struct gs_waiter *w = &dev->waiters[i];
        if (!w->in_use) continue;
        int alive = w->in_use == 1 && tid_alive(w->tid);
        for (uint32_t k = 0; k < GS_ENTRIES; k++) {
            struct gs_entry *e = &w->e[k];
            if (!e->seq || (slot && e->obj != slot)) continue;
            int in_all = w->all && e->index < w->count;
            if (in_all != all) continue;
            if (!alive) {
                e->seq = 0;
                continue;
            }
            out[n].seq = e->seq;
            out[n].id = i * GS_ENTRIES + k + 1;
            n++;
        }
    }
    qsort(out, n, sizeof(*out), cmp_seq_id);
    return n;
}

/* The caller adopted o's lock from a dead holder: fix the journal and rebuild the any-list. */
static void repair_obj(struct gsync_device *dev, struct gs_obj *o)
{
    uint32_t slot = (uint32_t)(o - dev->objs) + 1;
    journal_fix(dev, o);
    size_t cap = (size_t)dev->hdr->waiter_used * GS_ENTRIES + 1;
    struct seq_id *list = malloc(cap * sizeof(*list));
    if (!list) return;
    size_t n = collect_entries(dev, slot, 0, list);
    o->any_head = o->any_tail = 0;
    for (size_t j = 0; j < n; j++) list_link(dev, &o->any_head, &o->any_tail, list[j].id);
    free(list);
}

/* The caller adopted the device lock from a dead holder: fix dev-locked objects and rebuild all-lists. */
static void repair_all(struct gsync_device *dev)
{
    struct gs_header *hdr = dev->hdr;
    size_t cap = (size_t)hdr->waiter_used * GS_ENTRIES + 1;
    struct seq_id *list = malloc(cap * sizeof(*list));
    if (!list) return;
    size_t n = collect_entries(dev, 0, 1, list);
    for (uint32_t i = 0; i < hdr->obj_used; i++) {
        struct gs_obj *o = &dev->objs[i];
        if (obj_type(o) == GS_FREE) continue;
        ll_lock(dev, &o->lock, LK_OBJ, o);
        if (atomic_load(&o->dev_locked)) journal_fix(dev, o); /* changed by the dead holder of the device lock */
        atomic_store(&o->dev_locked, 0);
        o->all_head = o->all_tail = 0;
        uint32_t hint = 0;
        for (size_t j = 0; j < n; j++) {
            if (entry_ptr(dev, list[j].id)->obj != i + 1) continue;
            list_link(dev, &o->all_head, &o->all_tail, list[j].id);
            hint++;
        }
        atomic_store(&o->all_hint, hint);
        ll_unlock(&o->lock);
    }
    free(list);
}

/* The caller adopted the allocation lock from a dead holder: rebuild both free lists. */
static void repair_alloc(struct gsync_device *dev)
{
    struct gs_header *hdr = dev->hdr;
    uint32_t used = 0;
    for (uint32_t i = 0; i < hdr->max_objects; i++)
        if (atomic_load(&dev->objs[i].gen)) used = i + 1;
    hdr->obj_used = used;
    hdr->obj_free_head = 0;
    for (uint32_t i = used; i-- > 0;) {
        struct gs_obj *o = &dev->objs[i];
        if (atomic_load(&o->type) == GS_FREE) {
            o->free_next = hdr->obj_free_head;
            hdr->obj_free_head = i + 1;
        }
    }
    hdr->waiter_free_head = 0;
    for (uint32_t i = hdr->waiter_used; i-- > 0;) {
        struct gs_waiter *w = &dev->waiters[i];
        if (!w->in_use) {
            w->free_next = hdr->waiter_free_head;
            hdr->waiter_free_head = i + 1;
        }
    }
}

static void repair(struct gsync_device *dev, int kind, struct gs_obj *o)
{
    if (kind == LK_OBJ) repair_obj(dev, o);
    else if (kind == LK_ALL) repair_all(dev);
    else repair_alloc(dev);
    atomic_fetch_add(&dev->hdr->recoveries, 1);
}

/* Cleans up after dead threads: adopts and repairs any lock a dead thread still holds, then unqueues and frees the
   wait records of dead threads. The wineserver calls this when a process of the prefix dies. Safe to call any time
   from a thread that holds no GameOS Sync lock. Returns the number of dead wait records freed. */
int gsync_recover(struct gsync_device *dev)
{
    struct gs_header *hdr = dev->hdr;
    if (try_adopt(&hdr->wait_all_lock, atomic_load(&hdr->wait_all_lock))) {
        repair(dev, LK_ALL, NULL);
        ll_unlock(&hdr->wait_all_lock);
    }
    for (uint32_t i = 0; i < hdr->obj_used; i++) {
        struct gs_obj *o = &dev->objs[i];
        if (try_adopt(&o->lock, atomic_load(&o->lock))) {
            repair(dev, LK_OBJ, o);
            ll_unlock(&o->lock);
        }
    }
    if (try_adopt(&hdr->alloc_lock, atomic_load(&hdr->alloc_lock))) {
        repair(dev, LK_ALLOC, NULL);
        ll_unlock(&hdr->alloc_lock);
    }
    int freed = 0;
    for (uint32_t i = 0; i < hdr->waiter_used; i++) {
        struct gs_waiter *w = &dev->waiters[i];
        if (w->in_use != 1 || tid_alive(w->tid)) continue;
        /* Nobody may hand a dead thread anything from now on. */
        int32_t v = atomic_load(&w->signaled);
        while ((v == GS_WAITING || v == GS_SLEEPING) && !atomic_compare_exchange_weak(&w->signaled, &v, GS_DEAD)) {
        }
        for (uint32_t k = 0; k < GS_ENTRIES; k++) {
            struct gs_entry *e = &w->e[k];
            if (!e->seq || !e->obj || e->obj > hdr->obj_used) continue;
            struct gs_obj *o = &dev->objs[e->obj - 1];
            uint32_t id = i * GS_ENTRIES + k + 1;
            if (w->all && e->index < w->count) {
                ll_lock(dev, &hdr->wait_all_lock, LK_ALL, NULL);
                if (e->seq) {
                    list_remove(dev, &o->all_head, &o->all_tail, id);
                    atomic_fetch_sub(&o->all_hint, 1);
                }
                ll_unlock(&hdr->wait_all_lock);
            } else {
                int all = lock_obj(dev, o);
                if (e->seq) list_remove(dev, &o->any_head, &o->any_tail, id);
                unlock_obj(dev, o, all);
            }
        }
        waiter_release(dev, i + 1);
        freed++;
    }
    return freed;
}

/* Consistency check for tests: returns the number of problems found (0 = consistent). Call when quiescent. */
#include <stdio.h>
#define PROBLEM_UNUSED_GUARD 1
#define PROBLEM(...) do { problems++; if (verbose) fprintf(stderr, "gsync_check: " __VA_ARGS__); } while (0)
int gsync_check(struct gsync_device *dev)
{
    struct gs_header *hdr = dev->hdr;
    int problems = 0, verbose = getenv("GSYNC_CHECK_VERBOSE") != NULL;
    if (atomic_load(&hdr->wait_all_lock) || atomic_load(&hdr->alloc_lock) || atomic_load(&hdr->recover_lock))
        PROBLEM("device lock 0x%x alloc lock 0x%x still held\n", atomic_load(&hdr->wait_all_lock), atomic_load(&hdr->alloc_lock));
    for (uint32_t i = 0; i < hdr->obj_used; i++) {
        struct gs_obj *o = &dev->objs[i];
        if (atomic_load(&o->lock) || o->j_kind || o->j_pulse || atomic_load(&o->dev_locked))
            PROBLEM("slot %u lock 0x%x journal %u pulse %u dev_locked %u\n", i + 1, atomic_load(&o->lock), o->j_kind,
                    o->j_pulse, atomic_load(&o->dev_locked));
        uint32_t hint = 0;
        for (int which = 0; which < 2; which++) {
            uint32_t head = which ? o->all_head : o->any_head, tail = which ? o->all_tail : o->any_tail, prev = 0;
            uint32_t id = head, steps = 0;
            for (; id && steps < 1000000; id = entry_ptr(dev, id)->next, steps++) {
                struct gs_entry *e = entry_ptr(dev, id);
                if (e->prev != prev || e->obj != i + 1 || !e->seq || entry_waiter(dev, id)->in_use != 1)
                    PROBLEM("slot %u %s-list entry %u bad (prev %u/%u obj %u seq %u in_use %u)\n", i + 1,
                            which ? "all" : "any", id, e->prev, prev, e->obj, e->seq, entry_waiter(dev, id)->in_use);
                prev = id;
                if (which) hint++;
            }
            if (prev != tail) PROBLEM("slot %u %s-list tail %u, walked to %u\n", i + 1, which ? "all" : "any", tail, prev);
        }
        if (hint != atomic_load(&o->all_hint)) PROBLEM("slot %u all_hint %u but %u entries\n", i + 1, atomic_load(&o->all_hint), hint);
        if (obj_type(o) == GS_SEM && o->u.sem.count > o->u.sem.max) PROBLEM("slot %u semaphore over max\n", i + 1);
        if (obj_type(o) == GS_MUTEX && (!o->u.mutex.owner != !o->u.mutex.count))
            PROBLEM("slot %u mutex owner %u count %u\n", i + 1, o->u.mutex.owner, o->u.mutex.count);
    }
    return problems;
}
