# io-monitor

This is a thin shim intended to be used with LD_PRELOAD in order
to capture/intercept many C library calls for the purpose of
getting real-time metrics without having to change any existing
source code.

Currently the captured metrics are formatted into a comma-separated
string as follows:

facility,ts,duration,pid,domain,op-type,error code,fd,bytes transferred,s1,s2

The facility is set through an environment variable named 'FACILITY_ID'.
The facility identifier should have maximum of 4 characters.

Example:
abcd,1487975251,0.305000,28049,13,0,0,3,0,/home/paul/foo/bar.txt,wt

    facility = "abcd" (facility to pinpoint "who" generated the metrics)
    unix timestamp = "1487975251" (timestamp of operation)
    elapsed time (ms) = "0.305000" (elapsed time in ms)
    pid = "28049" (process id)
    domain = "13" (see DOMAIN_TYPE in io_monitor.c)
    operation = "0" (see OP_TYPE in io_monitor.c))
    error code = "0" (0 = success; non-zero = errno in most cases)
    file descriptor = "3" (or -1 if not applicable)
    bytes transferred = "0" (only non-zero for read/write operations)
    s1 = "/home/paul/foo/bar.txt" (optional; context-dependent)
    s2 = "wt" (optional; context-dependent)

The formatted string of metrics is then transferred via IPC to
another process. Currently this IPC mechanism is TCP sockets
on localhost. The use of TCP sockets is meant to be a temporary
solution.

In some cases, there may be a great deal of unwanted captures that
occur when process is first started. An example would be running
a Python program. When the Python interpreter starts it reads
many files as part of its initialization.

The monitor has a simple feature to prevent such undesirable
startup noise. If you set a value for the environment variable
'START_ON_OPEN', the monitor will start in 'paused' mode. It
will remain paused until a file open operation occurs with
the file path containing the specified value. 

For example, you might set START_ON_OPEN as:

    export START_ON_OPEN="hello_world.txt"

for a Python program that begins by opening the file "hello_world.txt".
This technique would prevent the normal Python initialization traffic
from being captured by the monitor.

