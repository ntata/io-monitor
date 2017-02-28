
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
