//
// Copyright (c) 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied.
// See the License for the specific language governing permissions and
// limitations under the License.


// This source is meant to be compiled as a shared library (.so) on Linux
// and used with the LD_PRELOAD mechanism as a mechanism to intercept
// C library calls. The primary use would be to understand lower level
// details as part of a process or application.
//
// Although many C library functions are intercepted, the ideas is
// that this library is just a very simple pass-through where only
// metrics are captured. Because this code is in the data path, the
// code has to have small footprint and be very efficient. As part of
// this objective, the metrics captured are all passed into the
// "record" function and then passed over-the-wall to some other
// process that will store or analyze the metrics.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <dirent.h>
#include <utime.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <sys/uio.h>
#include <sys/mount.h>
#include <sys/stat.h>


// to build:
// gcc -shared -fPIC io_monitor.c -o io_monitor.so -ldl
//
// to use:
// LD_LIBRARY=io_monitor.so ; my-process-to-monitor


// TODO and enhancements
// - allow to be started in 'paused' mode (eliminate startup noise; e.g., python)
//     maybe use x seconds time delay
// - change (or at least support) some other IPC mechanism other than TCP sockets
// - consider whether errors should be reported
// - consider whether to add more fields in metrics payload:
//      -- elapsed time (nanoseconds or microseconds)
//      -- number bytes
// - consider adding a way to filter here (inclusive or exclusive)
// - implement missing intercept calls (FILE_SPACE, PROCESSES, etc.)
// - find a better name/grouping for MISC
// - should there be a sampling mechanism (capturing everything on a busy
//     server process can generate a lot of data)
// - implement missing functions for opening/creating files
//     http://man7.org/linux/man-pages/man2/open.2.html
// - add number of bytes transferred (read/write)


static const int SOCKET_PORT = 8001;
static const int DOMAIN_UNSPECIFIED = -1;
static const int FD_NONE = -1;
static const char* FACILITY_ID = "FACILITY_ID";
static int failed_socket_connections = 0;

// set up some categories to group metrics
typedef enum {
   LINKS,             // 0  (link, unlink)
   XATTRS,            // 1  (getxattr, setxattr, listxattr)
   DIRS,              // 2  (mkdir, rmdir, chdir)
   FILE_SYSTEMS,      // 3  (mount, umount)
   FILE_DESCRIPTORS,  // 4  (dup, fcntl)
   SYNCS,             // 5  (sync, flush)
   SOCKETS,           // 6  (socket, accept, bind, connect, send, recv)
   SEEKS,             // 7  (fseek, lseek)
   FILE_SPACE,        // 8  (fallocate, ftruncate)
   PROCESSES,         // 9  (fork, exec, kill, exit)
   FILE_METADATA,     // 10  (stat, access, chmod, chown)
   FILE_WRITE,        // 11  (write)
   FILE_READ,         // 12  (read)
   FILE_OPEN_CLOSE,   // 13  (open, close)
   MISC,              // 14  (miscellaneous; rename, flock, mknod, chroot, etc.)
   DIR_METADATA       // 15  (opendir, readdir, seekdir, etc.)
} DOMAIN_TYPE;

// define the operations that we're tracking. please note that in some
// cases several C library calls all get identified the same (e.g., open,
// open64, fopen are all recorded as OPEN).
typedef enum {
   OPEN,           // 0  (FILE_OPEN_CLOSE)
   CLOSE,          // 1  (FILE_OPEN_CLOSE)
   WRITE,          // 2  (FILE_WRITE)
   READ,           // 3  (FILE_READ)
   SYNC,           // 4  (SYNCS)
   SEEK,           // 5  (SEEKS)
   RENAME,         // 6  (MISC)
   LINK,           // 7  (LINKS)
   UNLINK,         // 8  (LINKS)
   FCNTL,          // 9  (FILE_DESCRIPTORS)
   DUP,            // 10  (FILE_DESCRIPTORS)
   STAT,           // 11  (FILE_METADATA)
   ACCESS,         // 12  (FILE_METADATA)
   CHMOD,          // 13  (FILE_METADATA)
   CHOWN,          // 14  (FILE_METADATA)
   FLOCK,          // 15  (MISC)
   READLINK,       // 16  (LINKS)
   UTIME,          // 17  (FILE_METADATA)
   GETXATTR,       // 18  (XATTRS)
   SETXATTR,       // 19  (XATTRS)
   LISTXATTR,      // 20  (XATTRS)
   REMOVEXATTR,    // 21  (XATTRS)
   MOUNT,          // 22  (FILE_SYSTEMS)
   UMOUNT,         // 23  (FILE_SYSTEMS)
   FORK,           // 24  (PROCESSES)
   MKNOD,          // 25  (MISC)
   MKDIR,          // 26  (DIRS)
   RMDIR,          // 27  (DIRS)
   CHDIR,          // 28  (DIRS)
   CHROOT,         // 29  (MISC)
   SOCKET,         // 30  (SOCKETS)
   _IO_NEW_FOPEN,  // 31  (FILE_OPEN_CLOSE)
   FLUSH,          // 32  (SYNCS)
   FALLOCATE,      // 33  (FILE_SPACE)
   FTRUNCATE,      // 34  (FILE_SPACE)
   OPENDIR,        // 35  (DIR_METADATA)
   CLOSEDIR,       // 36  (DIR_METADATA)
   READDIR,        // 37  (DIR_METADATA)
   SEEKDIR,        // 38  (DIR_METADATA)
   TELLDIR,        // 39  (DIR_METADATA)
   DIRFD,          // 40  (DIR_METADATA)
   REWINDDIR,      // 41  (DIR_METADATA)
   SCANDIR         // 42  (DIR_METADATA)
} OP_TYPE;


// a debugging aid that we can easily turn off/on
#define PUTS(s)
//puts(s);

//***********  initialization  ***********
void initialize_monitor();

//***********  monitoring mechanism  ***********
void record(DOMAIN_TYPE dom_type,
            OP_TYPE op_type,
            int fd,
            const char* s1,
            const char* s2);

//***********  file io  ************
// open
typedef int (*orig_open_f_type)(const char* pathname, int flags);
typedef int (*orig_open64_f_type)(const char* pathname, int flags);
typedef FILE* (*orig_fopen_f_type)(const char* path, const char* mode);

// close
typedef int (*orig_close_f_type)(int fd);
typedef int (*orig_fclose_f_type)(FILE* fp);

// write
typedef ssize_t (*orig_write_f_type)(int fd, const void* buf, size_t count);
typedef ssize_t (*orig_pwrite_f_type)(int fd, const void* buf, size_t count, off_t offset);
typedef ssize_t (*orig_writev_f_type)(int fd, const struct iovec* iov, int iovcnt);
typedef ssize_t (*orig_pwritev_f_type)(int fd, const struct iovec* iov, int iovcnt,
                off_t offset);
typedef int (*orig_fprintf_f_type)(FILE* stream, const char* format, ...);
typedef int (*orig_vfprintf_f_type)(FILE* stream, const char* format, va_list ap);
typedef size_t (*orig_fwrite_f_type)(const void* ptr, size_t size, size_t nmemb, FILE* stream);

// read
typedef ssize_t (*orig_read_f_type)(int fd, void* buf, size_t count);
typedef ssize_t (*orig_pread_f_type)(int fd, void* buf, size_t count, off_t offset);
typedef ssize_t (*orig_readv_f_type)(int fd, const struct iovec* iov, int iovcnt);
typedef ssize_t (*orig_preadv_f_type)(int fd, const struct iovec* iov, int iovcnt,
               off_t offset);
typedef size_t (*orig_fread_f_type)(void* ptr, size_t size, size_t nmemb, FILE* stream);
typedef int (*orig_fscanf_f_type)(FILE* stream, const char* format, ...);
typedef int (*orig_vfscanf_f_type)(FILE* stream, const char* format, va_list ap);

// sync
typedef int (*orig_fsync_f_type)(int fd);
typedef int (*orig_fdatasync_f_type)(int fd);
typedef void (*orig_sync_f_type)(void);
typedef int (*orig_syncfs_f_type)(int fd);
typedef int (*orig_fflush_f_type)(FILE* fp);


//***********  xattrs  *************
// setxattr
typedef int (*orig_setxattr_f_type)(const char* path,
                                    const char* name,
                                    const void* value,
                                    size_t size,
                                    int flags);
typedef int (*orig_lsetxattr_f_type)(const char* path,
                                     const char* name,
                                     const void* value,
                                     size_t size,
                                     int flags);
typedef int (*orig_fsetxattr_f_type)(int fd,
                                     const char* name,
                                     const void* value,
                                     size_t size,
                                     int flags);

// getxattr
typedef ssize_t (*orig_getxattr_f_type)(const char* path,
                                        const char* name,
                                        void* value,
                                        size_t size);
typedef ssize_t (*orig_lgetxattr_f_type)(const char* path,
                                         const char* name,
                                         void* value,
                                         size_t size);
typedef ssize_t (*orig_fgetxattr_f_type)(int fd,
                                         const char* name,
                                         void* value,
                                         size_t size);

// listxattr
typedef ssize_t (*orig_listxattr_f_type)(const char* path,
                                         char* list,
                                         size_t size);
typedef ssize_t (*orig_llistxattr_f_type)(const char* path,
                                          char* list,
                                          size_t size);
typedef ssize_t (*orig_flistxattr_f_type)(int fd,
                                          char* list,
                                          size_t size);

// removexattr
typedef int (*orig_removexattr_f_type)(const char* path, const char* name);
typedef int (*orig_lremovexattr_f_type)(const char* path, const char* name);
typedef int (*orig_fremovexattr_f_type)(int fd, const char* name);

// mount
typedef int (*orig_mount_f_type)(const char* source, const char* target,
                 const char* filesystemtype, unsigned long mountflags,
                 const void* data);
typedef int (*orig_umount_f_type)(const char* target);
typedef int (*orig_umount2_f_type)(const char* target, int flags);

// directory metadata
typedef DIR* (*orig_opendir_f_type)(const char* name);
typedef DIR* (*orig_fdopendir_f_type)(int fd);
typedef int (*orig_closedir_f_type)(DIR* dirp);
typedef struct dirent* (*orig_readdir_f_type)(DIR* dirp);
typedef int (*orig_readdir_r_f_type)(DIR* dirp,
                                     struct dirent* entry,
                                     struct dirent** result);
typedef int (*orig_dirfd_f_type)(DIR* dirp);
typedef void (*orig_rewinddir_f_type)(DIR* dirp);
typedef void (*orig_seekdir_f_type)(DIR* dirp, long loc);
typedef long (*orig_telldir_f_type)(DIR* dirp);
// leaving out scandir for now due to high complexity

// file metadata
typedef int (*orig_fstat_f_type)(int fd, struct stat* buf);
typedef int (*orig_lstat_f_type)(const char* path, struct stat* buf);
typedef int (*orig_stat_f_type)(const char* path, struct stat* buf);

typedef int (*orig_access_f_type)(const char* path, int amode);
typedef int (*orig_faccessat_f_type)(int fd, const char* path, int mode, int flag);

typedef int (*orig_chmod_f_type)(const char* path, mode_t mode);
typedef int (*orig_fchmod_f_type)(int fd, mode_t mode);
typedef int (*orig_fchmodat_f_type)(int fd, const char* path, mode_t mode, int flag);

typedef int (*orig_chown_f_type)(const char* path, uid_t owner, gid_t group);
typedef int (*orig_fchown_f_type)(int fildes, uid_t owner, gid_t group);
typedef int (*orig_lchown_f_type)(const char* path, uid_t owner, gid_t group);
typedef int (*orig_fchownat_f_type)(int fd, const char* path, uid_t owner,
                                    gid_t group, int flag);

typedef int (*orig_utime_f_type)(const char* path, const struct utimbuf* times);


// unique identifier to know originator of metrics. defaults to 'u' (unspecified)
static char facility[5];
static int socket_fd = -1;

// open/close
static orig_open_f_type orig_open = NULL;
static orig_open64_f_type orig_open64 = NULL;
static orig_fopen_f_type orig_fopen = NULL;
static orig_fopen_f_type orig_fopen64 = NULL;
static orig_close_f_type orig_close = NULL;
static orig_fclose_f_type orig_fclose = NULL;

// writes
static orig_write_f_type orig_write = NULL;
static orig_pwrite_f_type orig_pwrite = NULL;
static orig_writev_f_type orig_writev = NULL;
static orig_pwritev_f_type orig_pwritev = NULL;
static orig_fprintf_f_type orig_fprintf = NULL;
static orig_vfprintf_f_type orig_vfprintf = NULL;
static orig_fwrite_f_type orig_fwrite = NULL;

// reads
static orig_read_f_type orig_read = NULL;
static orig_pread_f_type orig_pread = NULL;
static orig_readv_f_type orig_readv = NULL;
static orig_preadv_f_type orig_preadv = NULL;
static orig_fread_f_type orig_fread = NULL;
static orig_fscanf_f_type orig_fscanf = NULL;
static orig_vfscanf_f_type orig_vfscanf = NULL;

// sync/flush
static orig_fsync_f_type orig_fsync = NULL;
static orig_fdatasync_f_type orig_fdatasync = NULL;
static orig_sync_f_type orig_sync = NULL;
static orig_syncfs_f_type orig_syncfs = NULL;
static orig_fflush_f_type orig_fflush = NULL;

// xattrs
static orig_setxattr_f_type orig_setxattr = NULL;
static orig_lsetxattr_f_type orig_lsetxattr = NULL;
static orig_fsetxattr_f_type orig_fsetxattr = NULL;
static orig_getxattr_f_type orig_getxattr = NULL;
static orig_lgetxattr_f_type orig_lgetxattr = NULL;
static orig_fgetxattr_f_type orig_fgetxattr = NULL;
static orig_listxattr_f_type orig_listxattr = NULL;
static orig_llistxattr_f_type orig_llistxattr = NULL;
static orig_flistxattr_f_type orig_flistxattr = NULL;
static orig_removexattr_f_type orig_removexattr = NULL;
static orig_lremovexattr_f_type orig_lremovexattr = NULL;
static orig_fremovexattr_f_type orig_fremovexattr = NULL;

// filesystem mount/umount
static orig_mount_f_type orig_mount = NULL;
static orig_umount_f_type orig_umount = NULL;
static orig_umount2_f_type orig_umount2 = NULL;

// directory metadata
static orig_opendir_f_type orig_opendir = NULL;
static orig_fdopendir_f_type orig_fdopendir = NULL;
static orig_closedir_f_type orig_closedir = NULL;
static orig_readdir_f_type orig_readdir = NULL;
static orig_readdir_r_f_type orig_readdir_r = NULL;
static orig_dirfd_f_type orig_dirfd = NULL;
static orig_rewinddir_f_type orig_rewinddir = NULL;
static orig_seekdir_f_type orig_seekdir = NULL;
static orig_telldir_f_type orig_telldir = NULL;

// file metadata
static orig_fstat_f_type orig_fstat = NULL;
static orig_lstat_f_type orig_lstat = NULL;
static orig_stat_f_type orig_stat = NULL;
static orig_access_f_type orig_access = NULL;
static orig_faccessat_f_type orig_faccessat = NULL;
static orig_chmod_f_type orig_chmod = NULL;
static orig_fchmod_f_type orig_fchmod = NULL;
static orig_fchmodat_f_type orig_fchmodat = NULL;
static orig_chown_f_type orig_chown = NULL;
static orig_fchown_f_type orig_fchown = NULL;
static orig_lchown_f_type orig_lchown = NULL;
static orig_fchownat_f_type orig_fchownat = NULL;
static orig_utime_f_type orig_utime = NULL;



void load_library_functions();

#define CHECK_LOADED_FNS() \
if (NULL == orig_open) initialize_monitor();
   

//*****************************************************************************

__attribute__((constructor)) void init() {
   PUTS("init")
   initialize_monitor();
}

//*****************************************************************************

__attribute__((destructor))  void fini() {
   PUTS("fini")
}

//*****************************************************************************

// we only need to load the original library functions once
void load_library_functions() {
   if (NULL != orig_open) {
      return;
   }

   // open/close
   orig_open = (orig_open_f_type)dlsym(RTLD_NEXT,"open");
   orig_open64 = (orig_open64_f_type)dlsym(RTLD_NEXT,"open64");
   orig_fopen = (orig_fopen_f_type)dlsym(RTLD_NEXT,"fopen");
   orig_fopen64 = (orig_fopen_f_type)dlsym(RTLD_NEXT,"fopen64");
   orig_close = (orig_close_f_type)dlsym(RTLD_NEXT,"close");
   orig_fclose = (orig_fclose_f_type)dlsym(RTLD_NEXT,"fclose");

   // write
   orig_write = (orig_write_f_type)dlsym(RTLD_NEXT,"write");
   orig_pwrite = (orig_pwrite_f_type)dlsym(RTLD_NEXT,"pwrite");
   orig_writev = (orig_writev_f_type)dlsym(RTLD_NEXT,"writev");
   orig_pwritev = (orig_pwritev_f_type)dlsym(RTLD_NEXT,"pwritev");
   orig_fprintf = (orig_fprintf_f_type)dlsym(RTLD_NEXT,"fprintf");
   orig_vfprintf = (orig_vfprintf_f_type)dlsym(RTLD_NEXT,"vfprintf");
   orig_fwrite = (orig_fwrite_f_type)dlsym(RTLD_NEXT,"fwrite");

   // read
   orig_read = (orig_read_f_type)dlsym(RTLD_NEXT,"read");
   orig_pread = (orig_pread_f_type)dlsym(RTLD_NEXT,"pread");
   orig_readv = (orig_readv_f_type)dlsym(RTLD_NEXT,"readv");
   orig_preadv = (orig_preadv_f_type)dlsym(RTLD_NEXT,"preadv");
   orig_fread = (orig_fread_f_type)dlsym(RTLD_NEXT,"fread");
   orig_fscanf = (orig_fscanf_f_type)dlsym(RTLD_NEXT,"fscanf");
   orig_vfscanf = (orig_vfscanf_f_type)dlsym(RTLD_NEXT,"vfscanf");

   // sync
   orig_fsync = (orig_fsync_f_type)dlsym(RTLD_NEXT,"fsync");
   orig_fdatasync = (orig_fdatasync_f_type)dlsym(RTLD_NEXT,"fdatasync");
   orig_fflush = (orig_fflush_f_type)dlsym(RTLD_NEXT,"fflush");
   orig_sync = (orig_sync_f_type)dlsym(RTLD_NEXT,"sync");
   orig_syncfs = (orig_syncfs_f_type)dlsym(RTLD_NEXT,"syncfs");

   // xattrs
   orig_setxattr = (orig_setxattr_f_type)dlsym(RTLD_NEXT,"setxattr");
   orig_lsetxattr = (orig_lsetxattr_f_type)dlsym(RTLD_NEXT,"lsetxattr");
   orig_fsetxattr = (orig_fsetxattr_f_type)dlsym(RTLD_NEXT,"fsetxattr");
   orig_getxattr = (orig_getxattr_f_type)dlsym(RTLD_NEXT,"getxattr");
   orig_lgetxattr = (orig_lgetxattr_f_type)dlsym(RTLD_NEXT,"lgetxattr");
   orig_fgetxattr = (orig_fgetxattr_f_type)dlsym(RTLD_NEXT,"fgetxattr");
   orig_listxattr = (orig_listxattr_f_type)dlsym(RTLD_NEXT,"listxattr");
   orig_llistxattr = (orig_llistxattr_f_type)dlsym(RTLD_NEXT,"llistxattr");
   orig_flistxattr = (orig_flistxattr_f_type)dlsym(RTLD_NEXT,"flistxattr");
   orig_removexattr = (orig_removexattr_f_type)dlsym(RTLD_NEXT,"removexattr");
   orig_lremovexattr = (orig_lremovexattr_f_type)dlsym(RTLD_NEXT,"lremovexattr");
   orig_fremovexattr = (orig_fremovexattr_f_type)dlsym(RTLD_NEXT,"fremovexattr");

   // mount/umount
   orig_mount = (orig_mount_f_type)dlsym(RTLD_NEXT,"mount");
   orig_umount = (orig_umount_f_type)dlsym(RTLD_NEXT,"umount");
   orig_umount2 = (orig_umount2_f_type)dlsym(RTLD_NEXT,"umount2");

   // directory metadata
   orig_opendir = (orig_opendir_f_type)dlsym(RTLD_NEXT,"opendir");
   orig_fdopendir = (orig_fdopendir_f_type)dlsym(RTLD_NEXT,"fdopendir");
   orig_closedir = (orig_closedir_f_type)dlsym(RTLD_NEXT,"closedir");
   orig_readdir = (orig_readdir_f_type)dlsym(RTLD_NEXT,"readdir");
   orig_readdir_r = (orig_readdir_r_f_type)dlsym(RTLD_NEXT,"readdir_r");
   orig_dirfd = (orig_dirfd_f_type)dlsym(RTLD_NEXT,"dirfd");
   orig_rewinddir = (orig_rewinddir_f_type)dlsym(RTLD_NEXT,"rewinddir");
   orig_seekdir = (orig_seekdir_f_type)dlsym(RTLD_NEXT,"seekdir");
   orig_telldir = (orig_telldir_f_type)dlsym(RTLD_NEXT,"telldir");

   // file metadata
   orig_fstat = (orig_fstat_f_type)dlsym(RTLD_NEXT,"fstat");
   orig_lstat = (orig_lstat_f_type)dlsym(RTLD_NEXT,"lstat");
   orig_stat = (orig_stat_f_type)dlsym(RTLD_NEXT,"stat");
   orig_access = (orig_access_f_type)dlsym(RTLD_NEXT,"access");
   orig_faccessat = (orig_faccessat_f_type)dlsym(RTLD_NEXT,"faccessat");
   orig_chmod = (orig_chmod_f_type)dlsym(RTLD_NEXT,"chmod");
   orig_fchmod = (orig_fchmod_f_type)dlsym(RTLD_NEXT,"fchmod");
   orig_fchmodat = (orig_fchmodat_f_type)dlsym(RTLD_NEXT,"fchmodat");
   orig_chown = (orig_chown_f_type)dlsym(RTLD_NEXT,"chown");
   orig_fchown = (orig_fchown_f_type)dlsym(RTLD_NEXT,"fchown");
   orig_lchown = (orig_lchown_f_type)dlsym(RTLD_NEXT,"lchown");
   orig_fchownat = (orig_fchownat_f_type)dlsym(RTLD_NEXT,"fchownat");
   orig_utime = (orig_utime_f_type)dlsym(RTLD_NEXT,"utime");
}

//*****************************************************************************

void initialize_monitor() {
   memset(facility, 0, sizeof(facility));
   const char* facility_id = getenv(FACILITY_ID);
   if (facility_id != NULL) {
      strncpy(facility, facility_id, 4);
   } else {
      facility[0] = 'u';  // unspecified
   }
   load_library_functions();
}

//*****************************************************************************

void record(DOMAIN_TYPE dom_type,
            OP_TYPE op_type,
            int fd,
            const char* s1,
            const char* s2) {

   char msg_size_header[10];
   char record_output[256];
   unsigned long timestamp;
   int sockfd;
   int port;
   int rc;
   struct sockaddr_in server;
   int record_length;
   pid_t pid;

   // have we already tried to connect to our peer and failed?
   if (failed_socket_connections > 0) {
      // don't bother proceeding since we've already been unable to reach
      // the other side of our IPC. repeated failures just adds more
      // latency.
      return;
   }

   // exclude things that we should not be capturing
   // since we're using sockets, we're also intercepting
   // our own socket calls. if we're asked to record
   // any of our own work we just ignore it.
   if ((socket_fd != FD_NONE) && (fd == socket_fd)) {
      return;
   }

   // ignore reporting on stdin, stdout, stderr
   if ((fd > -1) && (fd < 3)) {
      return;
   }

   if (op_type == OPEN) {
      if (!strcmp(s1, ".")) {
         // ignore open of current directory
         return;
      }

      if (!strcmp(s1, "..")) {
         // ignore open of parent directory
         return;
      }
   }

   timestamp = (unsigned long)time(NULL);
   pid = getpid();

   bzero(record_output, sizeof(record_output));
   if (NULL != s1) {
      if (NULL != s2) {
         snprintf(record_output,
                  sizeof(record_output),
                  "%s,%ld,%d,%d,%d,%d,%s,%s",
                  facility, timestamp, pid, dom_type, op_type, fd, s1, s2);
      } else {
         snprintf(record_output,
                  sizeof(record_output),
                  "%s,%ld,%d,%d,%d,%d,%s",
                  facility, timestamp, pid, dom_type, op_type, fd, s1);
      }
   } else {
      snprintf(record_output,
               sizeof(record_output),
               "%s,%ld,%d,%d,%d,%d",
               facility, timestamp, pid, dom_type, op_type, fd);
   }

   // set up a 10 byte header that includes the size (in bytes)
   // of our payload since sockets don't include any built-in
   // message boundaries
   record_length = strlen(record_output);
   memset(msg_size_header, 0, 10);
   snprintf(msg_size_header, 10, "%d", record_length);

   // we're using TCP sockets here to throw the record over the wall
   // to another process on same machine. TCP sockets is probably
   // not the best IPC mechanism for this purpose, but this is
   // handy to get started.
   sockfd = socket(AF_INET, SOCK_STREAM, 0);
   if (sockfd > 0) {
      port = SOCKET_PORT;

      // we DO NOT want to send anything remotely.
      // we are in the middle of the data path, so we need to
      // keep any induced latency to absolute minimum.
      server.sin_addr.s_addr = inet_addr("127.0.0.1");
      server.sin_family = AF_INET;
      server.sin_port = htons(port);
      socket_fd = sockfd;

      rc = connect(sockfd,
                   (struct sockaddr*) &server,
                   sizeof(server));
      if (rc == 0) {
         int one = 1;
         int send_buffer_size = 256;
         setsockopt(sockfd, SOL_TCP, TCP_NODELAY, &one, sizeof(one));
         setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF,
                    &send_buffer_size, sizeof(send_buffer_size));
         rc = write(sockfd, msg_size_header, 10);
         if (10 == rc) {
            rc = write(sockfd, record_output, record_length);
            if (record_length == rc) {
               read(sockfd, record_output, sizeof(record_output)-1);
            } else {
               //printf("expected to write %d bytes, actual=%d\n",
               //       record_length, rc);
            }
         } else {
            //printf("header expected to write %d bytes, actual=%d\n", 10, rc);
         }
         //shutdown(sockfd, SHUT_RDWR);
      } else {
         // we're not able to reach our IPC peer. however, we're just a
         // thin shim used to intercept C library calls. we can't cause
         // the real process/application to crash or malfunction. the
         // show must go on...
         failed_socket_connections++;
      }
      close(sockfd);
      socket_fd = FD_NONE;
   }
}

//*****************************************************************************

int open(const char* pathname, int flags)
{
   CHECK_LOADED_FNS()
   PUTS("open")
   int fd = orig_open(pathname, flags);
   if (fd > -1) {
      record(FILE_OPEN_CLOSE, OPEN, fd, pathname, NULL);
   }
   return fd;
}

//*****************************************************************************

int open64(const char* pathname, int flags)
{
   CHECK_LOADED_FNS()
   PUTS("open64")
   int fd = orig_open64(pathname, flags);
   if (fd > -1) { 
      record(FILE_OPEN_CLOSE, OPEN, fd, pathname, NULL);
   }
   return fd;
}

//*****************************************************************************

int close(int fd)
{
   CHECK_LOADED_FNS()
   PUTS("close")
   int rc = orig_close(fd);
   if (0 == rc) {
      record(FILE_OPEN_CLOSE, CLOSE, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int fclose(FILE* fp)
{
   CHECK_LOADED_FNS()
   PUTS("fclose")
   int fd = fileno(fp);
   int rc = orig_fclose(fp);
   if (rc == 0) {
      record(FILE_OPEN_CLOSE, CLOSE, fd, NULL, NULL); 
   }
   return rc;
}

//*****************************************************************************

ssize_t write(int fd, const void* buf, size_t count)
{
   CHECK_LOADED_FNS()
   PUTS("write")
   ssize_t rc = orig_write(fd, buf, count);
   if (rc > 0) {
      record(FILE_WRITE, WRITE, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset)
{
   CHECK_LOADED_FNS()
   PUTS("pwrite")
   ssize_t rc = orig_pwrite(fd, buf, count, offset);
   if (rc > 0) {
      record(FILE_WRITE, WRITE, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

ssize_t writev(int fd, const struct iovec* iov, int iovcnt)
{
   CHECK_LOADED_FNS()
   PUTS("writev")
   ssize_t rc = orig_writev(fd, iov, iovcnt);
   if (rc > 0) {
      record(FILE_WRITE, WRITE, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

ssize_t pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset)
{
   CHECK_LOADED_FNS()
   PUTS("pwritev")
   ssize_t rc = orig_pwritev(fd, iov, iovcnt, offset);
   if (rc > 0) {
      record(FILE_WRITE, WRITE, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int fprintf(FILE* stream, const char* format, ...)
{
   CHECK_LOADED_FNS()
   PUTS("fprintf")
   va_list args;
   va_start(args, format);
   int rc = orig_vfprintf(stream, format, args);
   va_end(args);
   if (rc > 0) {
      record(FILE_WRITE, WRITE, fileno(stream), NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int vfprintf(FILE* stream, const char* format, va_list ap)
{
   CHECK_LOADED_FNS()
   PUTS("vfprintf")
   int rc = orig_vfprintf(stream, format, ap);
   if (rc > 0) {
      record(FILE_WRITE, WRITE, fileno(stream), NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream)
{
   CHECK_LOADED_FNS()
   PUTS("fwrite")
   size_t rc = orig_fwrite(ptr, size, nmemb, stream);
   if (rc > 0) {
      record(FILE_WRITE, WRITE, fileno(stream), NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

ssize_t read(int fd, void* buf, size_t count)
{
   CHECK_LOADED_FNS()
   PUTS("read")
   ssize_t rc = orig_read(fd, buf, count);
   if (rc > 0) {
      record(FILE_READ, READ, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

ssize_t pread(int fd, void* buf, size_t count, off_t offset)
{
   CHECK_LOADED_FNS()
   PUTS("pread")
   ssize_t rc = orig_pread(fd, buf, count, offset);
   if (rc > 0) {
      record(FILE_READ, READ, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

ssize_t readv(int fd, const struct iovec* iov, int iovcnt)
{
   CHECK_LOADED_FNS()
   PUTS("readv")
   ssize_t rc = orig_readv(fd, iov, iovcnt);
   if (rc > 0) {
      record(FILE_READ, READ, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

ssize_t preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset)
{
   CHECK_LOADED_FNS()
   PUTS("preadv")
   ssize_t rc = orig_preadv(fd, iov, iovcnt, offset);
   if (rc > 0) {
      record(FILE_READ, READ, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream)
{
   CHECK_LOADED_FNS()
   PUTS("fread")
   size_t rc = orig_fread(ptr, size, nmemb, stream);
   if (rc > 0) {
      record(FILE_READ, READ, fileno(stream), NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int fscanf(FILE* stream, const char* format, ...)
{
   CHECK_LOADED_FNS()
   PUTS("fscanf")
   va_list args;
   va_start(args, format);
   int rc = orig_vfscanf(stream, format, args);
   va_end(args);
   if (rc > 0) {
      record(FILE_READ, READ, fileno(stream), NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int vfscanf(FILE* stream, const char* format, va_list ap)
{
   CHECK_LOADED_FNS()
   PUTS("vfscanf")
   int rc = orig_vfscanf(stream, format, ap);
   if (rc > 0) {
      record(FILE_READ, READ, fileno(stream), NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int fsync(int fd)
{
   CHECK_LOADED_FNS()
   PUTS("fsync")
   int rc = orig_fsync(fd);
   if (0 == rc) {
      record(SYNCS, SYNC, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int fdatasync(int fd)
{
   CHECK_LOADED_FNS()
   PUTS("fdatasync")
   int rc = orig_fdatasync(fd);
   if (0 == rc) {
      record(SYNCS, SYNC, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

void sync()
{
   CHECK_LOADED_FNS()
   PUTS("sync")
   sync();
   record(SYNCS, SYNC, FD_NONE, NULL, NULL);
}

//*****************************************************************************

int syncfs(int fd)
{
   CHECK_LOADED_FNS()
   PUTS("syncfs")
   int rc = syncfs(fd);
   if (rc == 0) {
      record(SYNCS, SYNC, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int setxattr(const char* path,
             const char* name,
             const void* value,
             size_t size,
             int flags)
{
   CHECK_LOADED_FNS()
   PUTS("setxattr")
   int rc = orig_setxattr(path, name, value, size, flags);
   if (0 == rc) {
      record(XATTRS, SETXATTR, FD_NONE, path, name);
   }
   return rc;
}

//*****************************************************************************

int lsetxattr(const char* path,
              const char* name,
              const void* value,
              size_t size,
              int flags)
{
   CHECK_LOADED_FNS()
   PUTS("lsetxattr")
   int rc = orig_lsetxattr(path, name, value, size, flags);
   if (0 == rc) {
      record(XATTRS, SETXATTR, FD_NONE, path, name);
   }
   return rc;
}

//*****************************************************************************

int fsetxattr(int fd,
              const char* name,
              const void* value,
              size_t size,
              int flags)
{
   CHECK_LOADED_FNS()
   PUTS("fsetxattr")
   int rc = orig_fsetxattr(fd, name, value, size, flags);
   if (0 == rc) {
      record(XATTRS, SETXATTR, fd, name, NULL);
   }
   return rc;
}

//*****************************************************************************

ssize_t getxattr(const char* path, const char* name, void* value, size_t size)
{
   CHECK_LOADED_FNS()
   PUTS("getxattr")
   ssize_t rc = orig_getxattr(path, name, value, size);
   if (rc > 0) {
      record(XATTRS, GETXATTR, FD_NONE, path, name);
   }
   return rc;
}

//*****************************************************************************

ssize_t lgetxattr(const char* path, const char* name, void* value, size_t size)
{
   CHECK_LOADED_FNS()
   PUTS("lgetxattr")
   ssize_t rc = orig_lgetxattr(path, name, value, size);
   if (rc > 0) {
      record(XATTRS, GETXATTR, FD_NONE, path, name);
   }
   return rc;
}

//*****************************************************************************

ssize_t fgetxattr(int fd, const char* name, void* value, size_t size)
{
   CHECK_LOADED_FNS()
   PUTS("fgetxattr")
   ssize_t rc = orig_fgetxattr(fd, name, value, size);
   if (rc > 0) {
      record(XATTRS, GETXATTR, fd, name, NULL);
   }
   return rc;
}

//*****************************************************************************

ssize_t listxattr(const char* path, char* list, size_t size)
{
   CHECK_LOADED_FNS()
   PUTS("listxattr")
   ssize_t rc = orig_listxattr(path, list, size);
   if (rc > 0) {
      record(XATTRS, LISTXATTR, FD_NONE, path, NULL);
   }
   return rc;
}

//*****************************************************************************

ssize_t llistxattr(const char* path, char* list, size_t size)
{
   CHECK_LOADED_FNS()
   PUTS("llistxattr")
   ssize_t rc = orig_llistxattr(path, list, size);
   if (rc > 0) {
      record(XATTRS, LISTXATTR, FD_NONE, path, NULL);
   }
   return rc;
}

//*****************************************************************************

ssize_t flistxattr(int fd, char* list, size_t size)
{
   CHECK_LOADED_FNS()
   PUTS("flistxattr")
   ssize_t rc = orig_flistxattr(fd, list, size);
   if (rc > 0) {
      record(XATTRS, LISTXATTR, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int removexattr(const char* path, const char* name)
{
   CHECK_LOADED_FNS()
   PUTS("removexattr")
   int rc = orig_removexattr(path, name);
   if (0 == rc) {
      record(XATTRS, REMOVEXATTR, FD_NONE, path, name);
   }
   return rc;
}

//*****************************************************************************

int lremovexattr(const char* path, const char* name)
{
   CHECK_LOADED_FNS()
   PUTS("lremovexattr")
   int rc = orig_lremovexattr(path, name);
   if (0 == rc) {
      record(XATTRS, REMOVEXATTR, FD_NONE, path, name);
   }
   return rc;
}

//*****************************************************************************

int fremovexattr(int fd, const char* name)
{
   CHECK_LOADED_FNS()
   PUTS("fremovexattr")
   int rc = orig_fremovexattr(fd, name);
   if (0 == rc) {
      record(XATTRS, REMOVEXATTR, fd, name, NULL);
   }
   return rc;
}

//*****************************************************************************

int mount(const char* source, const char* target,
          const char* filesystemtype, unsigned long mountflags,
          const void* data)
{
   CHECK_LOADED_FNS()
   PUTS("mount")
   int rc = orig_mount(source, target, filesystemtype, mountflags, data);
   if (0 == rc) {
      record(FILE_SYSTEMS, MOUNT, FD_NONE, source, target);
   }
   return rc;
}

//*****************************************************************************

int umount(const char* target)
{
   CHECK_LOADED_FNS()
   PUTS("umount")
   int rc = orig_umount(target);
   if (0 == rc) {
      record(FILE_SYSTEMS, UMOUNT, FD_NONE, target, NULL); 
   }
   return rc;
}

//*****************************************************************************

int umount2(const char* target, int flags)
{
   CHECK_LOADED_FNS()
   PUTS("umount2")
   int rc = orig_umount2(target, flags);
   if (0 == rc) {
      record(FILE_SYSTEMS, UMOUNT, FD_NONE, target, NULL);
   }
   return rc;
}

//*****************************************************************************

FILE* fopen(const char* path, const char* mode)
{
   CHECK_LOADED_FNS()
   PUTS("fopen")
   FILE* rc = orig_fopen(path, mode);
   if (0 != rc) {
      record(FILE_OPEN_CLOSE, OPEN, fileno(rc), path, mode);
   }
   return rc;
}

//*****************************************************************************

FILE* fopen64(const char* path, const char* mode)
{
   CHECK_LOADED_FNS()
   PUTS("fopen64")
   FILE* rc = orig_fopen64(path, mode);
   if (0 != rc) {
      record(FILE_OPEN_CLOSE, OPEN, fileno(rc), path, mode);
   }
   return rc;
}

//*****************************************************************************

FILE* _IO_new_fopen(const char* path, const char* mode)
{
   CHECK_LOADED_FNS()
   PUTS("_IO_new_fopen")
   FILE* rc = orig_fopen(path, mode);
   if (0 != rc) {
      record(FILE_OPEN_CLOSE, OPEN, fileno(rc), path, mode);
   }
   return rc;
}

//*****************************************************************************

int fflush(FILE* fp)
{
   CHECK_LOADED_FNS()
   PUTS("fflush")
   int rc = orig_fflush(fp);
   if (rc == 0) {
      record(SYNCS, FLUSH, fileno(fp), NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

DIR* opendir(const char *name)
{
   CHECK_LOADED_FNS()
   PUTS("opendir")
   DIR* rc = orig_opendir(name);
   if (rc != NULL) {
      record(DIR_METADATA, OPENDIR, FD_NONE, name, NULL);
   }
   return rc;
}

//*****************************************************************************

DIR* fdopendir(int fd)
{
   CHECK_LOADED_FNS()
   PUTS("fdopendir")
   DIR* rc = orig_fdopendir(fd);
   if (rc != NULL) {
      record(DIR_METADATA, OPENDIR, fd, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int closedir(DIR* dirp)
{
   CHECK_LOADED_FNS()
   PUTS("closedir")
   int rc = orig_closedir(dirp);
   if (rc == 0) {
      record(DIR_METADATA, CLOSEDIR, FD_NONE, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

struct dirent* readdir(DIR* dirp)
{
   CHECK_LOADED_FNS()
   PUTS("readdir")
   struct dirent* rc = orig_readdir(dirp);
   if (rc != NULL) {
      record(DIR_METADATA, READDIR, FD_NONE, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int readdir_r(DIR* dirp, struct dirent* entry, struct dirent** result)
{
   CHECK_LOADED_FNS()
   PUTS("readdir_r")
   int rc = orig_readdir_r(dirp, entry, result);
   if (rc == 0) {
      record(DIR_METADATA, READDIR, FD_NONE, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int dirfd(DIR* dirp)
{
   CHECK_LOADED_FNS()
   PUTS("dirfd")
   int rc = orig_dirfd(dirp);
   if (rc > -1) {
      record(DIR_METADATA, DIRFD, FD_NONE, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

void rewinddir(DIR* dirp)
{
   CHECK_LOADED_FNS()
   PUTS("rewinddir")
   orig_rewinddir(dirp);
   record(DIR_METADATA, REWINDDIR, FD_NONE, NULL, NULL);
}

//*****************************************************************************

void seekdir(DIR* dirp, long loc)
{
   CHECK_LOADED_FNS()
   PUTS("seekdir")
   orig_seekdir(dirp, loc);
   record(DIR_METADATA, SEEKDIR, FD_NONE, NULL, NULL);
}

//*****************************************************************************

long telldir(DIR* dirp)
{
   CHECK_LOADED_FNS()
   PUTS("telldir")
   long rc = orig_telldir(dirp);
   if (rc > -1L) {
      record(DIR_METADATA, TELLDIR, FD_NONE, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int fstat(int fildes, struct stat* buf)
{
   CHECK_LOADED_FNS()
   PUTS("fstat")
   int rc = orig_fstat(fildes, buf);
   if (rc == 0) {
      record(FILE_METADATA, STAT, fildes, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int lstat(const char* path, struct stat* buf)
{
   CHECK_LOADED_FNS()
   PUTS("lstat")
   int rc = orig_lstat(path, buf);
   if (rc == 0) {
      record(FILE_METADATA, STAT, FD_NONE, path, NULL);
   }
   return rc;
}

//*****************************************************************************

int stat(const char* path, struct stat* buf)
{
   CHECK_LOADED_FNS()
   PUTS("stat")
   int rc = orig_stat(path, buf);
   if (rc == 0) {
      record(FILE_METADATA, STAT, FD_NONE, path, NULL);
   }
   return rc;
}

//*****************************************************************************

int access(const char* path, int amode)
{
   CHECK_LOADED_FNS()
   PUTS("access")
   int rc = orig_access(path, amode);
   if (rc == 0) {
      record(FILE_METADATA, ACCESS, FD_NONE, path, NULL);
   }
   return rc;
}

//*****************************************************************************

int faccessat(int fd, const char* path, int mode, int flag)
{
   CHECK_LOADED_FNS()
   PUTS("faccessat")
   int rc = orig_faccessat(fd, path, mode, flag);
   if (rc == 0) {
      record(FILE_METADATA, ACCESS, fd, path, NULL);
   }
   return rc;
}

//*****************************************************************************

int chmod(const char* path, mode_t mode)
{
   CHECK_LOADED_FNS()
   PUTS("chmod")
   int rc = orig_chmod(path, mode);
   if (rc == 0) {
      record(FILE_METADATA, CHMOD, FD_NONE, path, NULL);
   }
   return rc;
}

//*****************************************************************************

int fchmod(int fildes, mode_t mode)
{
   CHECK_LOADED_FNS()
   PUTS("fchmod")
   int rc = orig_fchmod(fildes, mode);
   if (rc == 0) {
      record(FILE_METADATA, CHMOD, fildes, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int fchmodat(int fd, const char* path, mode_t mode, int flag)
{
   CHECK_LOADED_FNS()
   PUTS("fchmodat")
   int rc = orig_fchmodat(fd, path, mode, flag);
   if (rc == 0) {
      record(FILE_METADATA, CHMOD, fd, path, NULL);
   }
   return rc;
}

//*****************************************************************************

int chown(const char* path, uid_t owner, gid_t group)
{
   CHECK_LOADED_FNS()
   PUTS("chown")
   int rc = orig_chown(path, owner, group);
   if (rc == 0) {
      record(FILE_METADATA, CHOWN, FD_NONE, path, NULL);
   }
   return rc;
}

//*****************************************************************************

int fchown(int fildes, uid_t owner, gid_t group)
{  
   CHECK_LOADED_FNS()
   PUTS("fchown")
   int rc = orig_fchown(fildes, owner, group);
   if (rc == 0) {
      record(FILE_METADATA, CHOWN, fildes, NULL, NULL);
   }
   return rc;
}

//*****************************************************************************

int lchown(const char* path, uid_t owner, gid_t group)
{  
   CHECK_LOADED_FNS()
   PUTS("lchown")
   int rc = orig_lchown(path, owner, group);
   if (rc == 0) {
      record(FILE_METADATA, CHOWN, FD_NONE, path, NULL);
   }
   return rc;
}

//*****************************************************************************

int fchownat(int fd, const char* path, uid_t owner, gid_t group, int flag)
{  
   CHECK_LOADED_FNS()
   PUTS("fchownat")
   int rc = orig_fchownat(fd, path, owner, group, flag);
   if (rc == 0) {
      record(FILE_METADATA, CHOWN, fd, path, NULL);
   }
   return rc;
}

//*****************************************************************************

int utime(const char* path, const struct utimbuf* times)
{
   CHECK_LOADED_FNS()
   PUTS("utime")
   int rc = orig_utime(path, times);
   if (rc == 0) {
      record(FILE_METADATA, UTIME, FD_NONE, path, NULL);
   }
   return rc;
}

//*****************************************************************************

