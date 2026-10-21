# GameOS Sync

GameOS Sync gives Wine the Windows synchronization behavior of Linux's ntsync driver (semaphores, mutexes, events,
wait-any, wait-all, alerts and abandoned mutexes) on systems that have no `/dev/ntsync`, such as the app sandbox on
my Googlebook. It runs entirely in user space: the objects live in one shared memory region per Wine prefix, and
threads sleep on classic futexes, which Android allows.

Each waiting thread has its own wait record in shared memory, the way the Windows kernel itself works. That gives a
real wait queue, so PulseEvent and wait-all behave exactly as on Windows, which Proton's fsync admits it cannot
guarantee. Operations that do not have to sleep make no system call at all.

## Measured on my Googlebook (inside an app, 2026-10-21)

| Operation | GameOS Sync | esync style |
|---|---|---|
| Uncontended acquire plus release | 61.5 ns | 259.6 ns |
| Wake-up round trip between two threads | 3.28 µs | 4.12 µs |

The test suite checks every behavior that the ntsync selftests check, plus waits and stress across processes:
153 checks, all passing on the device.

## Crash safety

Android can kill a game process at any instruction, even while it holds one of GameOS Sync's internal locks. So:

- Every lock word holds its holder's thread id, written by the same atomic operation that takes the lock.
- Before changing an object, the holder writes a small journal. Every handoff to a waiting thread carries a unique
  ticket, so recovery knows exactly which handoffs a waiter received.
- A thread that finds a lock held by a dead thread takes it over after 200 ms and repairs only what that lock
  protects. The wineserver also cleans up after every process that dies.

The fault-injection test (`test/gsync_fault.c`, built with `-DGSYNC_FAULT_INJECTION`) kills a process at every
point inside every critical section while another process keeps working on the same objects. On my Googlebook:
88 of 88 kill points, with 0 hangs, 0 inconsistent states and 0 broken objects.

## Building and testing

```bash
gcc -O2 -Wall -Wextra -std=gnu11 -pthread -o gsync_test test/gsync_test.c gsync.c
./gsync_test bench
gcc -O2 -Wall -Wextra -std=gnu11 -pthread -DGSYNC_FAULT_INJECTION -o gsync_fault test/gsync_fault.c gsync.c
./gsync_fault
```

## Status

Working: every ntsync behavior across threads and processes, with crash recovery. Still to do: the Wine 11 patch
that uses GameOS Sync in place of `/dev/ntsync`, and measurements with real games.

License: LGPL-2.1-or-later, so it can be linked into Wine.
