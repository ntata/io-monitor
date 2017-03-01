
ifndef NDEBUG
CFLAGS = -g
else
CFLAGS = -O2 -DNDEBUG
endif

headers = ops.h domains.h ops_names.h domains_names.h

all: mq_listener io_monitor.so

ops_names.h: ops.h
	cat ops.h | ./enum_to_strings.sh ops_names >ops_names.h

domains_names.h: domains.h
	cat domains.h | ./enum_to_strings.sh domains_names >domains_names.h

io_monitor.so: io_monitor.c $(headers)
	gcc $(CFLAGS) -shared -fPIC io_monitor.c -o io_monitor.so -ldl


mq_listener: mq_listener.c $(headers)
	gcc $(CFLAGS) mq_listener.c -o mq_listener

clean:
	rm -f mq_listener
	rm -f io_monitor.so
	rm -f domains_names.h
	rm -f ops_names.h
