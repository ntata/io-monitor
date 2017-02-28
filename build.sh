#!/bin/sh
cat ops.h | ./enum_to_strings.sh ops_names >ops_names.h
cat domains.h | ./enum_to_strings.sh domains_names >domains_names.h

gcc -shared -fPIC io_monitor.c -o io_monitor.so -ldl
gcc mq_listener.c -g -o mq_listener
