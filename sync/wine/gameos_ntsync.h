/* GameOS: the ntsync interface on top of GameOS Sync, for Wine on systems without /dev/ntsync.
   Copyright (C) 2026 Thomas Liviakis. LGPL-2.1-or-later. */
#ifndef GAMEOS_NTSYNC_H
#define GAMEOS_NTSYNC_H

/* Server: creates a GameOS Sync device if GAMEOS_GSYNC=1 is set. Returns its memfd, or -1. */
int gameos_gsync_server_create(void);
/* Client: attaches to the inproc device received from the server if it is a GameOS Sync region. */
int gameos_gsync_client_open(int fd);
/* Nonzero when GameOS Sync is in use in this process. */
int gameos_gsync_active(void);
/* Same contract as ioctl() on ntsync object and device fds: returns >= 0 or -1 with errno set. */
int gameos_ntsync_ioctl(int fd, unsigned long request, void *arg);
/* Closes an object: the server destroys it, a client does nothing (the server owns object lifetime). */
int gameos_ntsync_close(int fd);
/* Server: cleans up after dead threads and processes. */
void gameos_gsync_recover(void);

#endif
