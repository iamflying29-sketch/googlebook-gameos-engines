/* GameOS Sync: Windows NT synchronization objects (semaphores, mutexes, events) with wait-any and wait-all, in
   shared memory between processes, using only classic futexes. It provides the same behavior that Linux's ntsync
   driver gives Wine, for systems without /dev/ntsync such as Android apps.

   Copyright (C) 2026 Thomas Liviakis
   This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
   General Public License as published by the Free Software Foundation; either version 2.1 of the License, or
   (at your option) any later version. */

#ifndef GAMEOS_GSYNC_H
#define GAMEOS_GSYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An object handle: a slot number plus a generation, so a stale handle never reaches a reused slot. 0 is invalid. */
typedef uint32_t gsync_handle;

/* One device per Wine prefix (per wineserver), shared by all its processes through one memfd. */
struct gsync_device;

#define GSYNC_MAX_WAIT_OBJECTS 64            /* ntsync and Windows: MAXIMUM_WAIT_OBJECTS */
#define GSYNC_TIMEOUT_INFINITE UINT64_MAX
#define GSYNC_WAIT_REALTIME 0x1              /* the absolute timeout is CLOCK_REALTIME, otherwise CLOCK_MONOTONIC */

/* Device: create a new shared region (returns its memfd through *fd) or map an existing one received from the
   creator. Returns NULL with errno set on failure. */
struct gsync_device *gsync_device_create(uint32_t max_objects, uint32_t max_waiters, int *fd);
struct gsync_device *gsync_device_open(int fd);
void gsync_device_close(struct gsync_device *dev);

/* Object creation and closing. Every function returns 0 on success or a negative errno value. */
int gsync_create_sem(struct gsync_device *dev, uint32_t count, uint32_t max, gsync_handle *out);
int gsync_create_mutex(struct gsync_device *dev, uint32_t owner, uint32_t count, gsync_handle *out);
int gsync_create_event(struct gsync_device *dev, int manual, int signaled, gsync_handle *out);
int gsync_close(struct gsync_device *dev, gsync_handle h);

/* Signal operations. Each reports the previous state through the optional out argument, as ntsync does. */
int gsync_sem_release(struct gsync_device *dev, gsync_handle h, uint32_t count, uint32_t *prev_count);
int gsync_mutex_unlock(struct gsync_device *dev, gsync_handle h, uint32_t owner, uint32_t *prev_count);
int gsync_mutex_kill(struct gsync_device *dev, gsync_handle h, uint32_t owner);
int gsync_event_set(struct gsync_device *dev, gsync_handle h, uint32_t *prev_signaled);
int gsync_event_reset(struct gsync_device *dev, gsync_handle h, uint32_t *prev_signaled);
int gsync_event_pulse(struct gsync_device *dev, gsync_handle h, uint32_t *prev_signaled);

/* Read operations. gsync_mutex_read returns -EOWNERDEAD while the mutex is abandoned. */
int gsync_sem_read(struct gsync_device *dev, gsync_handle h, uint32_t *count, uint32_t *max);
int gsync_mutex_read(struct gsync_device *dev, gsync_handle h, uint32_t *owner, uint32_t *count);
int gsync_event_read(struct gsync_device *dev, gsync_handle h, uint32_t *manual, uint32_t *signaled);

/* Waits. timeout is an absolute time in nanoseconds on the chosen clock, or GSYNC_TIMEOUT_INFINITE. owner must be
   nonzero (Wine passes the Windows thread id). alert is an optional event handle (0 for none) that wakes the wait
   with index == count. On success *index is the satisfying object's index (wait-all: 0). Returns 0, -ETIMEDOUT,
   -EOWNERDEAD (an abandoned mutex was acquired; *index is valid) or -EINVAL. */
int gsync_wait_any(struct gsync_device *dev, const gsync_handle *objs, uint32_t count, uint64_t timeout,
                   uint32_t flags, uint32_t owner, gsync_handle alert, uint32_t *index);
int gsync_wait_all(struct gsync_device *dev, const gsync_handle *objs, uint32_t count, uint64_t timeout,
                   uint32_t flags, uint32_t owner, gsync_handle alert, uint32_t *index);

/* Cleans up after dead threads and processes: adopts and repairs any internal lock a dead thread still holds, and
   frees dead threads' wait records. The wineserver calls this when a process of the prefix dies; threads stuck on a
   lock held by a dead thread also repair it themselves after 200 ms. Returns the number of wait records freed. */
int gsync_recover(struct gsync_device *dev);

#ifdef __cplusplus
}
#endif

#endif
