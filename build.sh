#!/bin/sh
gcc -shared -fPIC io_monitor.c -o io_monitor.so -ldl
