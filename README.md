# io-monitor

## Overview

This is a thin shim intended to be used with **LD_PRELOAD** in order
to capture/intercept many C library calls for the purpose of
getting real-time metrics without having to change any existing
source code. The idea is that relevant metrics should be
captured very efficiently and then handed off to another process
to handle.

## IPC Mechanism

The preferred IPC mechanism is **Unix SysV message queues**. In order
to make use of the captured metrics, you must set the environment
variable **MESSAGE_QUEUE_PATH** to an existing file where user has
permissions for writing.

## Identifying Metrics

Each captured metric has an **operation type** to identify the kind
of operation that generated the metric. In some cases, the specified
operation is a family of functions grouped by functionality.
For example, the 'OPEN' operation on files can be one of the
following functions: open, open64, creat, creat64, fopen, fopen64.

Every operation type belongs to a **domain**. The domain is simply
a grouping mechanism to treat a certain logically related set
of operations as one (e.g., to enable/disable monitoring).

## Operations

| Operation     | Domain           | Functions |
| ---------     | ------           | --------- |
| CLOSEDIR      | DIR_METADATA     | closedir |
| DIRFD         | DIR_METADATA     | dirfd |
| OPENDIR       | DIR_METADATA     | fdopendir, opendir |
| READDIR       | DIR_METADATA     | readdir, readdir_r |
| REWINDDIR     | DIR_METADATA     | rewinddir |
| SCANDIR       | DIR_METADATA     | NOT-IMPLEMENTED |
| SEEKDIR       | DIR_METADATA     | seekdir |
| TELLDIR       | DIR_METADATA     | telldir |
| CHDIR         | DIRS             | NOT-IMPLEMENTED |
| MKDIR         | DIRS             | NOT-IMPLEMENTED |
| RMDIR         | DIRS             | NOT-IMPLEMENTED |
| DUP           | FILE_DESCRIPTORS | NOT-IMPLEMENTED |
| FCNTL         | FILE_DESCRIPTORS | NOT-IMPLEMENTED |
| ACCESS        | FILE_METADATA    | access, faccessat |
| CHMOD         | FILE_METADATA    | chmod, fchmod, fchmodat |
| CHOWN         | FILE_METADATA    | chown, fchown, fchownat, lchown |
| STAT          | FILE_METADATA    | fstat, lstat, stat |
| UTIME         | FILE_METADATA    | utime |
| CLOSE         | FILE_OPEN_CLOSE  | close, fclose |
| OPEN          | FILE_OPEN_CLOSE  | open, open64, creat, creat64, fopen, fopen64 |
| _IO_NEW_FOPEN | FILE_OPEN_CLOSE  | _IO_new_fopen |
| READ          | FILE_READ        | read, pread, readv, preadv, fread, fscanf, vfscanf |
| ALLOCATE      | FILE_SPACE       | posix_fallocate, fallocate |
| TRUNCATE      | FILE_SPACE       | truncate, ftruncate |
| MOUNT         | FILE_SYSTEMS     | mount |
| UMOUNT        | FILE_SYSTEMS     | umount, umount2 |
| WRITE         | FILE_WRITE       | write, pwrite, writev, pwritev, fprintf, vfprintf, fwrite |
| LINK          | LINKS            | NOT-IMPLEMENTED |
| READLINK      | LINKS            | NOT-IMPLEMENTED |
| UNLINK        | LINKS            | NOT-IMPLEMENTED |
| CHROOT        | MISC             | NOT-IMPLEMENTED |
| FLOCK         | MISC             | NOT-IMPLEMENTED |
| MKNOD         | MISC             | NOT-IMPLEMENTED |
| RENAME        | MISC             | NOT-IMPLEMENTED |
| EXEC          | PROCESSES        | NOT-IMPLEMENTED |
| FORK          | PROCESSES        | NOT-IMPLEMENTED |
| KILL          | PROCESSES        | NOT-IMPLEMENTED |
| SEEK          | SEEKS            | NOT-IMPLEMENTED |
| SOCKET        | SOCKETS          | NOT-IMPLEMENTED |
| START         | START_STOP       | startup of a process (no corresponding function call) |
| STOP          | START_STOP       | end of a process (no corresponding function call) |
| FLUSH         | SYNCS            | fflush |
| SYNC          | SYNCS            | fsync, fdatasync, sync, syncfs |
| GETXATTR      | XATTRS           | getxattr, lgetxattr, fgetxattr |
| LISTXATTR     | XATTRS           | listxattr, llistxattr, flistxattr |
| REMOVEXATTR   | XATTRS           | removexattr, fremovexattr, lremovexattr |
| SETXATTR      | XATTRS           | setxattr, lsetxattr, fsetxattr |


## Domains

| Domain           | Description                      | Operations |
| ------           | -----------                      | ---------- |
| DIR_METADATA     | directory metadata operations    | CLOSEDIR, DIRFD, OPENDIR, READDIR, REWINDDIR, SCANDIR, SEEKDIR, TELLDIR |
| DIRS             | directory operations             | CHDIR, MKDIR, RMDIR |
| FILE_DESCRIPTORS | file descriptor manipulations    | DUP, FCNTL |
| FILE_METADATA    | file metadata operations         | ACCESS, CHMOD, CHOWN, STAT, UTIME |
| FILE_WRITE       | file write operations            | WRITE |
| FILE_READ        | file read operations             | READ |
| FILE_OPEN_CLOSE  | file open/close operations       | CLOSE, OPEN, _IO_NEW_FOPEN |
| FILE_SYSTEMS     | file system operations           | MOUNT, UMOUNT |
| FILE_SPACE       | file space adjustment operations | ALLOCATE, TRUNCATE |
| HTTP             | HTTP network operations          | TBD: http verb events |
| LINKS            | hard and soft link operations    | LINK, READLINK, UNLINK |
| MISC             | misc. operations                 | CHROOT, FLOCK, MKNOD, RENAME |
| NETWORKING       | networking operations            | TBD: accept, listen, connect, etc. |
| PROCESSES        | process operations               | EXEC, FORK, KILL |
| SEEKS            | file seek operations             | SEEK |
| SOCKETS          | socket operations                | NOT-IMPLEMENTED |
| START_STOP       | begin and end of processes       | START, STOP |
| SYNCS            | file sync/flush operations       | FLUSH, SYNC |
| XATTRS           | extended attribute operations    | GETXATTR, LISTXATTR, REMOVEXATTR, SETXATTR |

## Environment Variables

| Variable           | Required? | Description |
| ------             | --------- | ----------- |
| FACILITY_ID        | N         | Identifies the component. defaults to 'u' |
| MESSAGE_QUEUE_PATH | Y         | File path of existing file associated with SysV message queue |
| MONITOR_DOMAINS    | Y         | list of comma-separated domains to monitor or 'ALL' |
| START_ON_OPEN      | N         | starts paused, resumes on open of specified file |
| START_ON_ELAPSED   | N         | starts paused, resumes on elapsed time crossing specified threshold |


## START_ON_OPEN

START_ON_OPEN causes io monitor to start in paused mode. It will remain paused until a
file is opened whose path contains (substring) the value specified by this variable. 

In some cases, there may be a great deal of unwanted captures that occur when process
is first started. An example would be running a Python program. When the Python
interpreter starts it reads many files as part of its initialization.

The START_ON_OPEN feature can prevent such undesirable startup noise. If you set a
value for the environment variable 'START_ON_OPEN', the monitor will start in 'paused'
mode. It will remain paused until a file open operation occurs with the file path
containing the specified value. 

For example, you might set START_ON_OPEN as:

    export START_ON_OPEN="hello_world.txt"

for a Python program that begins by opening the file "hello_world.txt". This technique
would prevent the normal Python initialization traffic from being captured by the monitor.

## Metrics

| Metric            | Description |
| ------            | ----------- |
| facility          | identifier of component that generated the metrics |
| ts                | unix timestamp of when operation occurred |
| duration          | elapsed time of operation in milliseconds |
| pid               | process id where metrics were collected |
| domain            | domain grouping for the operation |
| op-type           | type of operation |
| error code        | integer error code. 0 = success; non-zero = errno in most cases |
| fd                | file descriptor associated with operation, or -1 if N/A |
| bytes transferred | number of bytes transferred for read/write operations |
| arg1              | context dependent |
| arg2              | context dependent |

