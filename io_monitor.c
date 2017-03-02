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
#include <limits.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <utime.h>
#include <time.h>
#include <errno.h>
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
#include <sys/time.h>
#include <arpa/inet.h>
#include "ops.h"
#include "domains.h"
#include "domains_names.h"
#include "mq.h"


// to build:
// gcc -shared -fPIC io_monitor.c -o io_monitor.so -ldl
//
// to use:
// LD_PRELOAD=io_monitor.so ; my-process-to-monitor
//
// Also need socket server on other side of IPC to receive
// the metrics


// TODO and enhancements
// - implement missing intercept calls (FILE_SPACE, PROCESSES, etc.)
// - find a better name/grouping for MISC
// - should there be a sampling mechanism? (capturing everything on a busy
//     server process can generate a lot of data)
// - implement missing functions for opening/creating files
//     http://man7.org/linux/man-pages/man2/open.2.html


#define DECL_VARS() \
struct timeval start_time, end_time; \
int error_code = 0;

#define GET_START_TIME() \
gettimeofday(&start_time, NULL);

#define GET_END_TIME() \
gettimeofday(&end_time, NULL);

#define TIME_BEFORE() \
&start_time

#define TIME_AFTER() \
&end_time



// environment variables that we respond to
static const char* ENV_FACILITY_ID = "FACILITY_ID";
static const char* ENV_MESSAGE_QUEUE_PATH = "MESSAGE_QUEUE_PATH";
static const char* ENV_START_ON_OPEN = "START_ON_OPEN";
static const char* ENV_MONITOR_DOMAINS = "MONITOR_DOMAINS";
static const char* ENV_START_ON_ELAPSED = "START_ON_ELAPSED";

static const int SOCKET_PORT = 8001;
static const int DOMAIN_UNSPECIFIED = -1;
static const int FD_NONE = -1;
static const int MQ_KEY_NONE = -1;
static int failed_socket_connections = 0;
static int failed_ipc_sends = 0;
static const ssize_t ZERO_BYTES = 0L;
static const char* message_queue_path = NULL;
static const int message_project_id = 'm';
static key_t message_queue_key = -1;
static int message_queue_id = -1;
static unsigned int domain_bit_flags = 0;



// set up bit flags for each domain
static unsigned int BIT_LINKS = (1 << LINKS);
static unsigned int BIT_XATTRS = (1 << XATTRS);
static unsigned int BIT_DIRS = (1 << DIRS);
static unsigned int BIT_FILE_SYSTEMS = (1 << FILE_SYSTEMS);
static unsigned int BIT_FILE_DESCRIPTORS = (1 << FILE_DESCRIPTORS);
static unsigned int BIT_SYNCS = (1 << SYNCS);
static unsigned int BIT_SOCKETS = (1 << SOCKETS);
static unsigned int BIT_SEEKS = (1 << SEEKS);
static unsigned int BIT_FILE_SPACE = (1 << FILE_SPACE);
static unsigned int BIT_PROCESSES = (1 << PROCESSES);
static unsigned int BIT_FILE_METADATA = (1 << FILE_METADATA);
static unsigned int BIT_FILE_WRITE = (1 << FILE_WRITE);
static unsigned int BIT_FILE_READ = (1 << FILE_READ);
static unsigned int BIT_FILE_OPEN_CLOSE = (1 << FILE_OPEN_CLOSE);
static unsigned int BIT_MISC = (1 << MISC);
static unsigned int BIT_DIR_METADATA = (1 << DIR_METADATA);


// a debugging aid that we can easily turn off/on
#ifdef NDEBUG
#define PUTS(s)
#else
#define PUTS(s) \
puts(s);
#endif

//***********  initialization  ***********
void initialize_monitor();
unsigned int domain_list_to_bit_mask(const char* domain_list);

//***********  IPC mechanisms  ***********
int send_tcp_socket(struct monitor_record_t* monitor_record);
int send_msg_queue(struct monitor_record_t* monitor_record);

//***********  monitoring mechanism  ***********
void record(DOMAIN_TYPE dom_type,
            OP_TYPE op_type,
            int fd,
            const char* s1,
            const char* s2,
            struct timeval* start_time,
            struct timeval* end_time,
            int error_code,
            ssize_t bytes_transferred);

//***********  file io  ************
// open
typedef int (*orig_open_f_type)(const char* pathname, int flags, ...);
typedef int (*orig_open64_f_type)(const char* pathname, int flags, ...);
typedef FILE* (*orig_fopen_f_type)(const char* path, const char* mode);
typedef int (*orig_creat_f_type)(const char* path, mode_t mode);
typedef int (*orig_creat64_f_type)(const char* path, mode_t mode);

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

// allocate
typedef int (*orig_posix_fallocate_f_type)(int fd, off_t offset, off_t len);
typedef int (*orig_fallocate_f_type)(int fd, int mode, off_t offset, off_t len);

// truncate
typedef int (*orig_truncate_f_type)(const char* path, off_t length);
typedef int (*orig_ftruncate_f_type)(int fd, off_t length);


// unique identifier to know originator of metrics. defaults to 'u' (unspecified)
static char facility[5];
static const char* start_on_open = NULL;
static int socket_fd = -1;
static int paused = 0;
static have_elapsed_threshold = 0;
static double elapsed_threshold = 0.0;


// open/close
static orig_open_f_type orig_open = NULL;
static orig_open64_f_type orig_open64 = NULL;
static orig_fopen_f_type orig_fopen = NULL;
static orig_fopen_f_type orig_fopen64 = NULL;
static orig_creat_f_type orig_creat = NULL;
static orig_creat64_f_type orig_creat64 = NULL;
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

// allocate
static orig_posix_fallocate_f_type orig_posix_fallocate = NULL;
static orig_fallocate_f_type orig_fallocate = NULL;

// truncate
static orig_truncate_f_type orig_truncate = NULL;
static orig_ftruncate_f_type orig_ftruncate = NULL;


void load_library_functions();

#define CHECK_LOADED_FNS() \
if (NULL == orig_open) initialize_monitor();
   

//*****************************************************************************

__attribute__((constructor)) void init() {
   PUTS("init");
   DECL_VARS()
   GET_START_TIME()
   CHECK_LOADED_FNS();
   /* retrieve actual command that was called */
   char cmdline[PATH_MAX];
   sprintf(cmdline, "/proc/%d/cmdline", getpid());
   int fd = orig_open(cmdline, O_RDONLY);
   int len = orig_read(fd, cmdline, PATH_MAX);
   if (len) {
     len--;
     while (--len) {
       if (!cmdline[len])
	 cmdline[len] = ' ';
     }
   } else {
     sprintf(cmdline, "could not determine path");
   }
   orig_close(fd);
   /* here retrieve actual path */

   GET_END_TIME();

   
   record(START_STOP, START, 0, cmdline, NULL,
          TIME_BEFORE(), TIME_AFTER(), 0, ZERO_BYTES);
   
}

//*****************************************************************************

__attribute__((destructor))  void fini() {
   PUTS("fini")
   DECL_VARS()
   GET_START_TIME()
   CHECK_LOADED_FNS();
   /* collect CPU usage, brk/heap size metrics from /proc */

   GET_END_TIME();

   
   record(START_STOP, STOP, 0, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), 0, ZERO_BYTES);
   //TODO: let collector know that we're done?
}

//*****************************************************************************

// we only need to load the original library functions once
void load_library_functions() {
   if (NULL != orig_open) {
      return;
   }

   start_on_open = getenv(ENV_START_ON_OPEN);
   const char* start_on_elapsed = getenv(ENV_START_ON_ELAPSED);
   if (start_on_open != NULL) {
      paused = 1;
   } else if (start_on_elapsed != NULL) {
      double elapsed_value = atof(start_on_elapsed);
      if (elapsed_value > 0.1) {
         elapsed_threshold = elapsed_value;
         have_elapsed_threshold = 1;
         paused = 1;
      }
   }

   // open/close
   orig_open = (orig_open_f_type)dlsym(RTLD_NEXT,"open");
   orig_open64 = (orig_open64_f_type)dlsym(RTLD_NEXT,"open64");
   orig_fopen = (orig_fopen_f_type)dlsym(RTLD_NEXT,"fopen");
   orig_fopen64 = (orig_fopen_f_type)dlsym(RTLD_NEXT,"fopen64");
   orig_creat = (orig_creat_f_type)dlsym(RTLD_NEXT,"creat");
   orig_creat64 = (orig_creat64_f_type)dlsym(RTLD_NEXT,"creat64");
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

   // allocate
   orig_posix_fallocate = (orig_posix_fallocate_f_type)dlsym(RTLD_NEXT,"posix_fallocate");
   orig_fallocate = (orig_fallocate_f_type)dlsym(RTLD_NEXT,"fallocate");

   // truncate
   orig_truncate = (orig_truncate_f_type)dlsym(RTLD_NEXT,"truncate");
   orig_ftruncate = (orig_ftruncate_f_type)dlsym(RTLD_NEXT,"ftruncate");
}

//*****************************************************************************

unsigned int domain_list_to_bit_mask(const char* domain_list)
{
   unsigned int bit_mask = 0;
   char* token;
   char* domain_list_copy = strdup(domain_list);
   char* rest = domain_list_copy;
   int i;
   
   while ((token = strtok_r(rest, ",", &rest))) {
     for (i = 0; i != END_DOMAINS; ++i) {
       if (!strcmp(token, domains_names[i])) {
	   bit_mask |= (1 << i);
	   break;
	 }
     }
   }
   free(domain_list_copy);

   return bit_mask;
}

//*****************************************************************************

void initialize_monitor() {
   // establish facility id
   memset(facility, 0, sizeof(facility));
   const char* facility_id = getenv(ENV_FACILITY_ID);
   if (facility_id != NULL) {
      strncpy(facility, facility_id, 4);
   } else {
      facility[0] = 'u';  // unspecified
   }

   message_queue_path = getenv(ENV_MESSAGE_QUEUE_PATH);

   const char* monitor_domain_list = getenv(ENV_MONITOR_DOMAINS);
   if (monitor_domain_list != NULL) {
      if (!strcmp(monitor_domain_list, "ALL")) {
         domain_bit_flags = -1; // turn all of them on
      } else {
         domain_bit_flags = domain_list_to_bit_mask(monitor_domain_list);
      }
   } else {
      // by default, don't record anything
      domain_bit_flags = 0;
   }

   load_library_functions();
}

//*****************************************************************************

int send_msg_queue(struct monitor_record_t* monitor_record)
{
   MONITOR_MESSAGE monitor_message;
   if (message_queue_key == MQ_KEY_NONE) {
      if (message_queue_path != NULL) {
         message_queue_key = ftok(message_queue_path, message_project_id);
         if (message_queue_key != -1) {
            message_queue_id = msgget(message_queue_key, 0600 | IPC_CREAT);
         }
      }
   }

   if (message_queue_id == MQ_KEY_NONE) {
      PUTS("no message queue available")
      return -1;
   }

   memset(&monitor_message, 0, sizeof(MONITOR_MESSAGE));
   monitor_message.message_type = 1L;
   memcpy(&monitor_message.monitor_record, monitor_record, sizeof (*monitor_record));

   return msgsnd(message_queue_id,
                 &monitor_message,
                 sizeof(*monitor_record),
                 IPC_NOWAIT);
}

//*****************************************************************************

int send_tcp_socket(struct monitor_record_t* monitor_record)
{
   int rc;
   int record_length;
   int sockfd;
   int port;
   char msg_size_header[10];
   struct sockaddr_in server;

   // set up a 10 byte header that includes the size (in bytes)
   // of our payload since sockets don't include any built-in
   // message boundaries
   record_length = sizeof(*monitor_record);
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
            rc = write(sockfd, monitor_record, record_length);
            if (record_length == rc) {
               rc = 0;
            } else {
               //printf("expected to write %d bytes, actual=%d\n",
               //       record_length, rc);
               rc = -1;
            }
         } else {
            //printf("header expected to write %d bytes, actual=%d\n", 10, rc);
            rc = -1;
         }
         //shutdown(sockfd, SHUT_RDWR);
      } else {
         // we're not able to reach our IPC peer. however, we're just a
         // thin shim used to intercept C library calls. we can't cause
         // the real process/application to crash or malfunction. the
         // show must go on...
         failed_socket_connections++;
         rc = -1;
      }
      close(sockfd);
      socket_fd = FD_NONE;
   }

   return rc;
}

//*****************************************************************************

#define RECORD_FIELD(f) record_output. f = f
#define RECORD_FIELD_S(f) if (f) {strncpy(record_output.f, f, sizeof(record_output.f)); \
    record_output.f[sizeof(record_output.f)-1] = 0; }

void record(DOMAIN_TYPE dom_type,
            OP_TYPE op_type,
            int fd,
            const char* s1,
            const char* s2,
            struct timeval* start_time,
            struct timeval* end_time,
            int error_code,
            ssize_t bytes_transferred)
{
   struct monitor_record_t record_output;
   unsigned long timestamp;
   int rc_ipc;
   int record_length;
   pid_t pid;
   double elapsed_time;

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
   if ((fd > -1) && (fd < 3) && dom_type != START_STOP) {
      return;
   }

   
   // if we're not monitoring this domain we just ignore
   const unsigned int domain_bit_flag = 1 << dom_type;
   if (0 == (domain_bit_flags & domain_bit_flag)) {
      PUTS("ignoring domain")
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

      if (paused && (start_on_open != NULL) &&
          (strstr(s1, start_on_open) != NULL)) {
         PUTS("starting on open")
         paused = 0;
      }
   }

   // sec to msec
   elapsed_time = (end_time->tv_sec - start_time->tv_sec) * 1000.0;
   // usec to ms
   elapsed_time += (end_time->tv_usec - start_time->tv_usec) / 1000.0;

   if (paused && have_elapsed_threshold && (elapsed_time > elapsed_threshold)) {
      PUTS("starting on elapsed")
      paused = 0;
   }

   if (paused) {
      return;
   }

   timestamp = (unsigned long)time(NULL);
   pid = getpid();

   bzero(&record_output, sizeof(record_output));

   RECORD_FIELD_S(facility);
   RECORD_FIELD(timestamp);
   RECORD_FIELD(elapsed_time);
   RECORD_FIELD(pid);
   RECORD_FIELD(dom_type);
   RECORD_FIELD(op_type);
   RECORD_FIELD(error_code);
   RECORD_FIELD(fd);
   RECORD_FIELD(bytes_transferred);
   RECORD_FIELD_S(s1);
   RECORD_FIELD_S(s2);
   
   if (message_queue_path != NULL) {
      rc_ipc = send_msg_queue(&record_output);
   } else {
      rc_ipc = send_tcp_socket(&record_output);
   }

   if (rc_ipc != 0) {
      PUTS("io_monitor.c ipc send failed")
      failed_ipc_sends++;
   }
}

//*****************************************************************************

int open(const char* pathname, int flags, ...)
{
   CHECK_LOADED_FNS()
   PUTS("open")
   DECL_VARS()
   GET_START_TIME()
   const int fd = orig_open(pathname, flags);
   GET_END_TIME()

   if (fd == -1) {
      error_code = errno;
   }

   char* real_path = realpath(pathname, NULL);
   record(FILE_OPEN_CLOSE, OPEN, fd, real_path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);
   free(real_path);

   return fd;
}

//*****************************************************************************

int open64(const char* pathname, int flags, ...)
{
   CHECK_LOADED_FNS()
   PUTS("open64")
   DECL_VARS()
   GET_START_TIME()
   const int fd = orig_open64(pathname, flags);
   GET_END_TIME()

   if (fd == -1) {
      error_code = errno;
   }

   char* real_path = realpath(pathname, NULL);
   record(FILE_OPEN_CLOSE, OPEN, fd, real_path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);
   free(real_path);

   return fd;
}

//*****************************************************************************

int creat(const char* pathname, mode_t mode)
{
   CHECK_LOADED_FNS()
   PUTS("creat")
   DECL_VARS()
   GET_START_TIME()
   const int fd = orig_creat(pathname, mode);
   GET_END_TIME()

   if (fd == -1) {
      error_code = errno;
   }

   char* real_path = realpath(pathname, NULL);
   record(FILE_OPEN_CLOSE, OPEN, fd, real_path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);
   free(real_path);

   return fd;
}

//*****************************************************************************

int creat64(const char* pathname, mode_t mode)
{
   CHECK_LOADED_FNS()
   PUTS("creat64")
   DECL_VARS()
   GET_START_TIME()
   const int fd = orig_creat64(pathname, mode);
   GET_END_TIME()

   if (fd == -1) {
      error_code = errno;
   }

   char* real_path = realpath(pathname, NULL);
   record(FILE_OPEN_CLOSE, OPEN, fd, real_path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);
   free(real_path);

   return fd;
}

//*****************************************************************************

int close(int fd)
{
   CHECK_LOADED_FNS()
   PUTS("close")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_close(fd);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_OPEN_CLOSE, CLOSE, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int fclose(FILE* fp)
{
   CHECK_LOADED_FNS()
   PUTS("fclose")
   DECL_VARS()
   GET_START_TIME()
   const int fd = fileno(fp);
   const int rc = orig_fclose(fp);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_OPEN_CLOSE, CLOSE, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

ssize_t write(int fd, const void* buf, size_t count)
{
   CHECK_LOADED_FNS()
   PUTS("write")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t bytes_written = orig_write(fd, buf, count);
   GET_END_TIME()

   if (bytes_written < 0) {
      error_code = errno;
   }

   record(FILE_WRITE, WRITE, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, bytes_written);

   return bytes_written;
}

//*****************************************************************************

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset)
{
   CHECK_LOADED_FNS()
   PUTS("pwrite")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t bytes_written = orig_pwrite(fd, buf, count, offset);
   GET_END_TIME()

   if (bytes_written < 0) {
      error_code = errno;
   }

   record(FILE_WRITE, WRITE, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, bytes_written);

   return bytes_written;
}

//*****************************************************************************

ssize_t writev(int fd, const struct iovec* iov, int iovcnt)
{
   CHECK_LOADED_FNS()
   PUTS("writev")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t bytes_written = orig_writev(fd, iov, iovcnt);
   GET_END_TIME()

   if (bytes_written < 0) {
      error_code = errno;
   }

   record(FILE_WRITE, WRITE, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, bytes_written);

   return bytes_written;
}

//*****************************************************************************

ssize_t pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset)
{
   CHECK_LOADED_FNS()
   PUTS("pwritev")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t bytes_written = orig_pwritev(fd, iov, iovcnt, offset);
   GET_END_TIME()

   if (bytes_written < 0) {
      error_code = errno;
   }

   record(FILE_WRITE, WRITE, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, bytes_written);

   return bytes_written;
}

//*****************************************************************************

int fprintf(FILE* stream, const char* format, ...)
{
   CHECK_LOADED_FNS()
   PUTS("fprintf")
   DECL_VARS()
   GET_START_TIME()
   va_list args;
   va_start(args, format);
   const ssize_t bytes_written = orig_vfprintf(stream, format, args);
   va_end(args);
   GET_END_TIME()

   ssize_t record_bytes_written;

   if (bytes_written >= 0) {
      error_code = 0;
      record_bytes_written = bytes_written;
   } else {
      error_code = -1;
      record_bytes_written = 0;
   }

   record(FILE_WRITE, WRITE, fileno(stream), NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, record_bytes_written);

   return bytes_written;
}

//*****************************************************************************

int vfprintf(FILE* stream, const char* format, va_list ap)
{
   CHECK_LOADED_FNS()
   PUTS("vfprintf")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t bytes_written = orig_vfprintf(stream, format, ap);
   GET_END_TIME()

   ssize_t record_bytes_written;

   if (bytes_written >= 0) {
      error_code = 0;
      record_bytes_written = bytes_written;
   } else {
      error_code = -1;
      record_bytes_written = 0;
   }

   record(FILE_WRITE, WRITE, fileno(stream), NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, record_bytes_written);

   return bytes_written;
}

//*****************************************************************************

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream)
{
   CHECK_LOADED_FNS()
   PUTS("fwrite")
   DECL_VARS()
   GET_START_TIME()
   const size_t rc = orig_fwrite(ptr, size, nmemb, stream);
   GET_END_TIME()

   if (rc < nmemb) {
      error_code = 1;
   }

   // our recording of 0 bytes here is not accurate, however we don't
   // have an easy way of knowing how many bytes were converted.
   record(FILE_WRITE, WRITE, fileno(stream), NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, rc * nmemb);

   return rc;
}

//*****************************************************************************

ssize_t read(int fd, void* buf, size_t count)
{
   CHECK_LOADED_FNS()
   PUTS("read")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t bytes_read = orig_read(fd, buf, count);
   GET_END_TIME()

   if (bytes_read < 0) {
      error_code = errno;
   }

   record(FILE_READ, READ, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, bytes_read);

   return bytes_read;
}

//*****************************************************************************

ssize_t pread(int fd, void* buf, size_t count, off_t offset)
{
   CHECK_LOADED_FNS()
   PUTS("pread")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t bytes_read = orig_pread(fd, buf, count, offset);
   GET_END_TIME()

   if (bytes_read < 0) {
      error_code = errno;
   }

   record(FILE_READ, READ, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, bytes_read);

   return bytes_read;
}

//*****************************************************************************

ssize_t readv(int fd, const struct iovec* iov, int iovcnt)
{
   CHECK_LOADED_FNS()
   PUTS("readv")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t bytes_read = orig_readv(fd, iov, iovcnt);
   GET_END_TIME()

   if (bytes_read < 0) {
      error_code = errno;
   }

   record(FILE_READ, READ, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, bytes_read);

   return bytes_read;
}

//*****************************************************************************

ssize_t preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset)
{
   CHECK_LOADED_FNS()
   PUTS("preadv")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t bytes_read = orig_preadv(fd, iov, iovcnt, offset);
   GET_END_TIME()

   if (bytes_read < 0) {
      error_code = errno;
   }

   record(FILE_READ, READ, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, bytes_read);

   return bytes_read;
}

//*****************************************************************************

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream)
{
   CHECK_LOADED_FNS()
   PUTS("fread")
   DECL_VARS()
   GET_START_TIME()
   const size_t items_read = orig_fread(ptr, size, nmemb, stream);
   GET_END_TIME()

   if (items_read < nmemb) {
      if (ferror(stream)) {
         error_code = 1;
      }
   }

   record(FILE_READ, READ, fileno(stream), NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, items_read * size);

   return items_read;
}

//*****************************************************************************

int fscanf(FILE* stream, const char* format, ...)
{
   CHECK_LOADED_FNS()
   PUTS("fscanf")
   DECL_VARS()
   GET_START_TIME()
   va_list args;
   va_start(args, format);
   const int rc = orig_vfscanf(stream, format, args);
   va_end(args);
   GET_END_TIME()

   if (rc == EOF) {
      error_code = errno;
   }

   // our recording of 0 bytes here is not accurate, however we don't
   // have an easy way of knowing how many bytes were converted.
   record(FILE_READ, READ, fileno(stream), NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int vfscanf(FILE* stream, const char* format, va_list ap)
{
   CHECK_LOADED_FNS()
   PUTS("vfscanf")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_vfscanf(stream, format, ap);
   GET_END_TIME()

   if (rc == EOF) {
      error_code = errno;
   }

   // our recording of 0 bytes here is not accurate, however we don't
   // have an easy way of knowing how many bytes were converted.
   record(FILE_READ, READ, fileno(stream), NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int fsync(int fd)
{
   CHECK_LOADED_FNS()
   PUTS("fsync")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_fsync(fd);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(SYNCS, SYNC, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int fdatasync(int fd)
{
   CHECK_LOADED_FNS()
   PUTS("fdatasync")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_fdatasync(fd);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(SYNCS, SYNC, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

void sync()
{
   CHECK_LOADED_FNS()
   PUTS("sync")
   DECL_VARS()
   GET_START_TIME()
   sync();
   GET_END_TIME()
   record(SYNCS, SYNC, FD_NONE, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), 0, ZERO_BYTES);
}

//*****************************************************************************

int syncfs(int fd)
{
   CHECK_LOADED_FNS()
   PUTS("syncfs")
   DECL_VARS()
   GET_START_TIME()
   const int rc = syncfs(fd);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(SYNCS, SYNC, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

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
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_setxattr(path, name, value, size, flags);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(XATTRS, SETXATTR, FD_NONE, path, name,
          TIME_BEFORE(), TIME_AFTER(), error_code, size);

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
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_lsetxattr(path, name, value, size, flags);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(XATTRS, SETXATTR, FD_NONE, path, name,
          TIME_BEFORE(), TIME_AFTER(), error_code, size);

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
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_fsetxattr(fd, name, value, size, flags);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(XATTRS, SETXATTR, fd, name, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, size);

   return rc;
}

//*****************************************************************************

ssize_t getxattr(const char* path, const char* name, void* value, size_t size)
{
   CHECK_LOADED_FNS()
   PUTS("getxattr")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t bytes_read = orig_getxattr(path, name, value, size);
   GET_END_TIME()
   ssize_t recorded_bytes_read = bytes_read;

   if (bytes_read == -1) {
      error_code = errno;
      recorded_bytes_read = 0;
   }

   record(XATTRS, GETXATTR, FD_NONE, path, name,
          TIME_BEFORE(), TIME_AFTER(), error_code, recorded_bytes_read);

   return bytes_read;
}

//*****************************************************************************

ssize_t lgetxattr(const char* path, const char* name, void* value, size_t size)
{
   CHECK_LOADED_FNS()
   PUTS("lgetxattr")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t bytes_read = orig_lgetxattr(path, name, value, size);
   GET_END_TIME()

   ssize_t recorded_bytes_read = bytes_read;

   if (bytes_read == -1) {
      error_code = errno;
      recorded_bytes_read = 0;
   }

   record(XATTRS, GETXATTR, FD_NONE, path, name,
          TIME_BEFORE(), TIME_AFTER(), error_code, recorded_bytes_read);

   return bytes_read;
}

//*****************************************************************************

ssize_t fgetxattr(int fd, const char* name, void* value, size_t size)
{
   CHECK_LOADED_FNS()
   PUTS("fgetxattr")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t bytes_read = orig_fgetxattr(fd, name, value, size);
   GET_END_TIME()

   ssize_t recorded_bytes_read = bytes_read;

   if (bytes_read == -1) {
      error_code = errno;
      recorded_bytes_read = 0;
   }

   record(XATTRS, GETXATTR, fd, name, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, recorded_bytes_read);

   return bytes_read;
}

//*****************************************************************************

ssize_t listxattr(const char* path, char* list, size_t size)
{
   CHECK_LOADED_FNS()
   PUTS("listxattr")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t list_size = orig_listxattr(path, list, size);
   GET_END_TIME()

   if (list_size < 0) {
      error_code = errno;
   }

   record(XATTRS, LISTXATTR, FD_NONE, path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return list_size;
}

//*****************************************************************************

ssize_t llistxattr(const char* path, char* list, size_t size)
{
   CHECK_LOADED_FNS()
   PUTS("llistxattr")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t list_size = orig_llistxattr(path, list, size);
   GET_END_TIME()

   if (list_size < 0) {
      error_code = errno;
   }

   record(XATTRS, LISTXATTR, FD_NONE, path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return list_size;
}

//*****************************************************************************

ssize_t flistxattr(int fd, char* list, size_t size)
{
   CHECK_LOADED_FNS()
   PUTS("flistxattr")
   DECL_VARS()
   GET_START_TIME()
   const ssize_t list_size = orig_flistxattr(fd, list, size);
   GET_END_TIME()

   if (list_size < 0) {
      error_code = errno;
   }

   record(XATTRS, LISTXATTR, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return list_size;
}

//*****************************************************************************

int removexattr(const char* path, const char* name)
{
   CHECK_LOADED_FNS()
   PUTS("removexattr")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_removexattr(path, name);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(XATTRS, REMOVEXATTR, FD_NONE, path, name,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int lremovexattr(const char* path, const char* name)
{
   CHECK_LOADED_FNS()
   PUTS("lremovexattr")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_lremovexattr(path, name);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(XATTRS, REMOVEXATTR, FD_NONE, path, name,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int fremovexattr(int fd, const char* name)
{
   CHECK_LOADED_FNS()
   PUTS("fremovexattr")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_fremovexattr(fd, name);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(XATTRS, REMOVEXATTR, fd, name, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int mount(const char* source, const char* target,
          const char* filesystemtype, unsigned long mountflags,
          const void* data)
{
   CHECK_LOADED_FNS()
   PUTS("mount")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_mount(source, target, filesystemtype, mountflags, data);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_SYSTEMS, MOUNT, FD_NONE, source, target,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int umount(const char* target)
{
   CHECK_LOADED_FNS()
   PUTS("umount")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_umount(target);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_SYSTEMS, UMOUNT, FD_NONE, target, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int umount2(const char* target, int flags)
{
   CHECK_LOADED_FNS()
   PUTS("umount2")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_umount2(target, flags);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_SYSTEMS, UMOUNT, FD_NONE, target, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

FILE* fopen(const char* path, const char* mode)
{
   CHECK_LOADED_FNS()
   PUTS("fopen")
   DECL_VARS()
   GET_START_TIME()
   FILE* rc = orig_fopen(path, mode);
   GET_END_TIME()
   int fd;

   if (rc == NULL) {
      error_code = errno;
      fd = FD_NONE;
   } else {
      fd = fileno(rc);
   }

   char* real_path = realpath(path, NULL);
   record(FILE_OPEN_CLOSE, OPEN, fd, real_path, mode,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);
   free(real_path);

   return rc;
}

//*****************************************************************************

FILE* fopen64(const char* path, const char* mode)
{
   CHECK_LOADED_FNS()
   PUTS("fopen64")
   DECL_VARS()
   GET_START_TIME()
   FILE* rc = orig_fopen64(path, mode);
   GET_END_TIME()

   if (rc == NULL) {
      error_code = errno;
   }

   char* real_path = realpath(path, NULL);
   const char* record_path;
   if (real_path != NULL) {
      record_path = real_path;
   } else {
      record_path = path;
   }

   int fd = FD_NONE;
   if (rc != NULL) {
      fd = fileno(rc);
   }

   record(FILE_OPEN_CLOSE, OPEN, fd, record_path, mode,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);
   if (real_path != NULL) {
      free(real_path);
   }

   return rc;
}

//*****************************************************************************

FILE* _IO_new_fopen(const char* path, const char* mode)
{
   CHECK_LOADED_FNS()
   PUTS("_IO_new_fopen")
   DECL_VARS()
   GET_START_TIME()
   FILE* rc = orig_fopen(path, mode);
   GET_END_TIME()
   int fd;

   if (rc == NULL) {
      error_code = errno;
      fd = FD_NONE;
   } else {
      fd = fileno(rc);
   }

   char* real_path = realpath(path, NULL);
   record(FILE_OPEN_CLOSE, OPEN, fd, real_path, mode,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);
   free(real_path);

   return rc;
}

//*****************************************************************************

int fflush(FILE* fp)
{
   CHECK_LOADED_FNS()
   PUTS("fflush")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_fflush(fp);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(SYNCS, FLUSH, fileno(fp), NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

DIR* opendir(const char *name)
{
   CHECK_LOADED_FNS()
   PUTS("opendir")
   DECL_VARS()
   GET_START_TIME()
   DIR* rc = orig_opendir(name);
   GET_END_TIME()

   if (rc == NULL) {
      error_code = errno;
   }

   record(DIR_METADATA, OPENDIR, FD_NONE, name, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

DIR* fdopendir(int fd)
{
   CHECK_LOADED_FNS()
   PUTS("fdopendir")
   DECL_VARS()
   GET_START_TIME()
   DIR* rc = orig_fdopendir(fd);
   GET_END_TIME()

   if (rc == NULL) {
      error_code = errno;
   }

   record(DIR_METADATA, OPENDIR, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int closedir(DIR* dirp)
{
   CHECK_LOADED_FNS()
   PUTS("closedir")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_closedir(dirp);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(DIR_METADATA, CLOSEDIR, FD_NONE, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

struct dirent* readdir(DIR* dirp)
{
   CHECK_LOADED_FNS()
   PUTS("readdir")
   DECL_VARS()
   GET_START_TIME()
   struct dirent* rc = orig_readdir(dirp);
   GET_END_TIME()

   if (rc == NULL) {
      error_code = errno;
   }

   record(DIR_METADATA, READDIR, FD_NONE, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int readdir_r(DIR* dirp, struct dirent* entry, struct dirent** result)
{
   CHECK_LOADED_FNS()
   PUTS("readdir_r")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_readdir_r(dirp, entry, result);
   GET_END_TIME()

   record(DIR_METADATA, READDIR, FD_NONE, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), rc, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int dirfd(DIR* dirp)
{
   CHECK_LOADED_FNS()
   PUTS("dirfd")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_dirfd(dirp);
   GET_END_TIME()

   if (rc < 0) {
      error_code = errno;
   }

   record(DIR_METADATA, DIRFD, FD_NONE, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

void rewinddir(DIR* dirp)
{
   CHECK_LOADED_FNS()
   PUTS("rewinddir")
   DECL_VARS()
   GET_START_TIME()
   orig_rewinddir(dirp);
   GET_END_TIME()
   record(DIR_METADATA, REWINDDIR, FD_NONE, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), 0, ZERO_BYTES);
}

//*****************************************************************************

void seekdir(DIR* dirp, long loc)
{
   CHECK_LOADED_FNS()
   PUTS("seekdir")
   DECL_VARS()
   GET_START_TIME()
   orig_seekdir(dirp, loc);
   GET_END_TIME()
   record(DIR_METADATA, SEEKDIR, FD_NONE, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), 0, ZERO_BYTES);
}

//*****************************************************************************

long telldir(DIR* dirp)
{
   CHECK_LOADED_FNS()
   PUTS("telldir")
   DECL_VARS()
   GET_START_TIME()
   const long loc = orig_telldir(dirp);
   GET_END_TIME()

   if (loc < 0L) {
      error_code = errno;
   }

   record(DIR_METADATA, TELLDIR, FD_NONE, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return loc;
}

//*****************************************************************************

int fstat(int fildes, struct stat* buf)
{
   CHECK_LOADED_FNS()
   PUTS("fstat")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_fstat(fildes, buf);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_METADATA, STAT, fildes, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int lstat(const char* path, struct stat* buf)
{
   CHECK_LOADED_FNS()
   PUTS("lstat")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_lstat(path, buf);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_METADATA, STAT, FD_NONE, path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int stat(const char* path, struct stat* buf)
{
   CHECK_LOADED_FNS()
   PUTS("stat")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_stat(path, buf);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_METADATA, STAT, FD_NONE, path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int access(const char* path, int amode)
{
   CHECK_LOADED_FNS()
   PUTS("access")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_access(path, amode);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_METADATA, ACCESS, FD_NONE, path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int faccessat(int fd, const char* path, int mode, int flag)
{
   CHECK_LOADED_FNS()
   PUTS("faccessat")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_faccessat(fd, path, mode, flag);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_METADATA, ACCESS, fd, path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int chmod(const char* path, mode_t mode)
{
   CHECK_LOADED_FNS()
   PUTS("chmod")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_chmod(path, mode);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_METADATA, CHMOD, FD_NONE, path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int fchmod(int fildes, mode_t mode)
{
   CHECK_LOADED_FNS()
   PUTS("fchmod")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_fchmod(fildes, mode);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_METADATA, CHMOD, fildes, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int fchmodat(int fd, const char* path, mode_t mode, int flag)
{
   CHECK_LOADED_FNS()
   PUTS("fchmodat")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_fchmodat(fd, path, mode, flag);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_METADATA, CHMOD, fd, path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int chown(const char* path, uid_t owner, gid_t group)
{
   CHECK_LOADED_FNS()
   PUTS("chown")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_chown(path, owner, group);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_METADATA, CHOWN, FD_NONE, path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int fchown(int fildes, uid_t owner, gid_t group)
{  
   CHECK_LOADED_FNS()
   PUTS("fchown")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_fchown(fildes, owner, group);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_METADATA, CHOWN, fildes, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int lchown(const char* path, uid_t owner, gid_t group)
{  
   CHECK_LOADED_FNS()
   PUTS("lchown")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_lchown(path, owner, group);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_METADATA, CHOWN, FD_NONE, path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int fchownat(int fd, const char* path, uid_t owner, gid_t group, int flag)
{  
   CHECK_LOADED_FNS()
   PUTS("fchownat")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_fchownat(fd, path, owner, group, flag);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_METADATA, CHOWN, fd, path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int utime(const char* path, const struct utimbuf* times)
{
   CHECK_LOADED_FNS()
   PUTS("utime")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_utime(path, times);
   GET_END_TIME()

   if (rc != 0) {
      error_code = errno;
   }

   record(FILE_METADATA, UTIME, FD_NONE, path, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, ZERO_BYTES);

   return rc;
}

//*****************************************************************************

int posix_fallocate(int fd, off_t offset, off_t len)
{
   CHECK_LOADED_FNS()
   PUTS("posix_fallocate")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_posix_fallocate(fd, offset, len);
   GET_END_TIME()
   ssize_t bytes_written;

   // according to man page, errno is NOT set on error!
   if (rc == 0) {
      bytes_written = len;
   } else {
      bytes_written = ZERO_BYTES;
   }

   record(FILE_SPACE, ALLOCATE, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), rc, bytes_written);

   return rc;
}

//*****************************************************************************

int fallocate(int fd, int mode, off_t offset, off_t len)
{
   CHECK_LOADED_FNS()
   PUTS("ftruncate")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_fallocate(fd, mode, offset, len);
   GET_END_TIME()
   ssize_t bytes_written;

   if (rc == 0) {
      error_code = 0;
      bytes_written = len;
   } else {
      error_code = errno;
      bytes_written = ZERO_BYTES;
   }

   record(FILE_SPACE, ALLOCATE, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, bytes_written);

   return rc;
}

//*****************************************************************************

int truncate(const char* path, off_t length)
{
   CHECK_LOADED_FNS()
   PUTS("truncate")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_truncate(path, length);
   GET_END_TIME()
   ssize_t bytes_written;

   if (rc == 0) {
      error_code = 0;
      bytes_written = length;
   } else {
      error_code = errno;
      bytes_written = ZERO_BYTES;
   }

   record(FILE_SPACE, TRUNCATE, FD_NONE, path, NULL,
          TIME_BEFORE(), TIME_AFTER(), rc, bytes_written);

   return rc;
}

//*****************************************************************************

int ftruncate(int fd, off_t length)
{
   CHECK_LOADED_FNS()
   PUTS("ftruncate")
   DECL_VARS()
   GET_START_TIME()
   const int rc = orig_ftruncate(fd, length);
   GET_END_TIME() 
   ssize_t bytes_written;

   if (rc == 0) {
      error_code = 0;
      bytes_written = length;
   } else {
      error_code = errno;
      bytes_written = ZERO_BYTES;
   }

   record(FILE_SPACE, TRUNCATE, fd, NULL, NULL,
          TIME_BEFORE(), TIME_AFTER(), error_code, bytes_written);

   return rc;
}

//*****************************************************************************

