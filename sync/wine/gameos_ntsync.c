/* GameOS: the ntsync interface on top of GameOS Sync, for Wine on systems without /dev/ntsync (Android apps).
   Wine's in-process sync code keeps calling the ntsync interface; when a GameOS Sync device is active, these
   functions answer with the same arguments, results and errno values, and object "fds" are GameOS Sync handles.

   Copyright (C) 2026 Thomas Liviakis
   This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
   General Public License as published by the Free Software Foundation; either version 2.1 of the License, or
   (at your option) any later version. */

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ntsync_tmp.h"
#include "gsync.h"
#include "gameos_ntsync.h"

static struct gsync_device *gdev;
static int is_server;

int gameos_gsync_active(void)
{
    return gdev != NULL;
}

int gameos_gsync_server_create(void)
{
    const char *env = getenv( "GAMEOS_GSYNC" );
    int fd = -1;
    if (!env || env[0] != '1') return -1;
    /* Sparse memfd: only pages that objects and waiters touch use memory. */
    gdev = gsync_device_create( 1u << 18, 16384, &fd );
    if (!gdev) return -1;
    is_server = 1;
    return fd;
}

int gameos_gsync_client_open(int fd)
{
    if (gdev) return 1;
    gdev = gsync_device_open( fd );
    return gdev != NULL;
}

static int fail(int ret)
{
    errno = -ret;
    return -1;
}

int gameos_ntsync_ioctl(int fd, unsigned long request, void *arg)
{
    gsync_handle h = (gsync_handle)fd, out = 0;
    int ret;

    if (!gdev) return ioctl( fd, request, arg );

    switch (request)
    {
    case NTSYNC_IOC_CREATE_SEM:
    {
        struct ntsync_sem_args *a = arg;
        if ((ret = gsync_create_sem( gdev, a->count, a->max, &out ))) return fail( ret );
        return (int)out;
    }
    case NTSYNC_IOC_CREATE_MUTEX:
    {
        struct ntsync_mutex_args *a = arg;
        if ((ret = gsync_create_mutex( gdev, a->owner, a->count, &out ))) return fail( ret );
        return (int)out;
    }
    case NTSYNC_IOC_CREATE_EVENT:
    {
        struct ntsync_event_args *a = arg;
        if ((ret = gsync_create_event( gdev, a->manual, a->signaled, &out ))) return fail( ret );
        return (int)out;
    }
    case NTSYNC_IOC_SEM_RELEASE:
    {
        __u32 *count = arg, prev = 0;
        if ((ret = gsync_sem_release( gdev, h, *count, &prev ))) return fail( ret );
        *count = prev;
        return 0;
    }
    case NTSYNC_IOC_MUTEX_UNLOCK:
    {
        struct ntsync_mutex_args *a = arg;
        __u32 prev = 0;
        if ((ret = gsync_mutex_unlock( gdev, h, a->owner, &prev ))) return fail( ret );
        a->count = prev;
        return 0;
    }
    case NTSYNC_IOC_MUTEX_KILL:
        if ((ret = gsync_mutex_kill( gdev, h, *(__u32 *)arg ))) return fail( ret );
        return 0;
    case NTSYNC_IOC_EVENT_SET:
        if ((ret = gsync_event_set( gdev, h, (__u32 *)arg ))) return fail( ret );
        return 0;
    case NTSYNC_IOC_EVENT_RESET:
        if ((ret = gsync_event_reset( gdev, h, (__u32 *)arg ))) return fail( ret );
        return 0;
    case NTSYNC_IOC_EVENT_PULSE:
        if ((ret = gsync_event_pulse( gdev, h, (__u32 *)arg ))) return fail( ret );
        return 0;
    case NTSYNC_IOC_SEM_READ:
    {
        struct ntsync_sem_args *a = arg;
        if ((ret = gsync_sem_read( gdev, h, &a->count, &a->max ))) return fail( ret );
        return 0;
    }
    case NTSYNC_IOC_MUTEX_READ:
    {
        struct ntsync_mutex_args *a = arg;
        if ((ret = gsync_mutex_read( gdev, h, &a->owner, &a->count ))) return fail( ret );
        return 0;
    }
    case NTSYNC_IOC_EVENT_READ:
    {
        struct ntsync_event_args *a = arg;
        if ((ret = gsync_event_read( gdev, h, &a->manual, &a->signaled ))) return fail( ret );
        return 0;
    }
    case NTSYNC_IOC_WAIT_ANY:
    case NTSYNC_IOC_WAIT_ALL:
    {
        struct ntsync_wait_args *a = arg;
        gsync_handle objs[GSYNC_MAX_WAIT_OBJECTS];
        const __u32 *fds = (const __u32 *)(uintptr_t)a->objs;
        __u32 index = ~0u;
        if (a->count > GSYNC_MAX_WAIT_OBJECTS || a->pad) return fail( -EINVAL );
        for (__u32 i = 0; i < a->count; i++) objs[i] = fds[i];
        if (request == NTSYNC_IOC_WAIT_ANY)
            ret = gsync_wait_any( gdev, objs, a->count, a->timeout, a->flags, a->owner, a->alert, &index );
        else
            ret = gsync_wait_all( gdev, objs, a->count, a->timeout, a->flags, a->owner, a->alert, &index );
        if (ret == 0 || ret == -EOWNERDEAD) a->index = index;
        if (ret) return fail( ret );
        return 0;
    }
    }
    return fail( -ENOTTY );
}

int gameos_ntsync_close(int fd)
{
    if (!gdev) return close( fd );
    if (is_server) gsync_close( gdev, (gsync_handle)fd );
    return 0;
}

void gameos_gsync_recover(void)
{
    if (gdev) gsync_recover( gdev );
}
