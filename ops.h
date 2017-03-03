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
   ALLOCATE,       // 33  (FILE_SPACE)
   TRUNCATE,       // 34  (FILE_SPACE)
   OPENDIR,        // 35  (DIR_METADATA)
   CLOSEDIR,       // 36  (DIR_METADATA)
   READDIR,        // 37  (DIR_METADATA)
   SEEKDIR,        // 38  (DIR_METADATA)
   TELLDIR,        // 39  (DIR_METADATA)
   DIRFD,          // 40  (DIR_METADATA)
   REWINDDIR,      // 41  (DIR_METADATA)
   SCANDIR,        // 42  (DIR_METADATA)
   CONNECT,        // 43  (SOCKETS)
   ACCEPT,         // 44  (SOCKETS)
   LISTEN,         // 45  (SOCKETS)
   BIND,           // 46  (SOCKETS)
   
   // operations listed below are NOT directly associated with
   // C functions
   START,          // Start execution of program
   STOP,           // Stop execution of program
   HTTP_REQ_SEND,  // Send an HTTP request. s1 will contain verb and URL
   HTTP_REQ_RECV,  // Get an HTTP request.  s1 will contain verb and URL
   HTTP_RESP_SEND, // Send an HTTP response. error code field will contain response code
   HTTP_RESP_RECV, // Receive HTTP response
   HTTP_RESP_FINI_SEND, // Sent final byte of HTTP response
   HTTP_RESP_FINI_RECV, // Receive final byte of HTTP response
   
   END_OPS         // keep this one as last
} OP_TYPE;
