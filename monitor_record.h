#ifndef __MONITOR_RECORD_H
#define __MONITOR_RECORD_H
#include <linux/limits.h>
#define STR_LEN 256

struct monitor_record_t {
  char facility[STR_LEN];
  int timestamp;
  float elapsed_time;
  int pid;

  int dom_type;
  int op_type;

  int error_code;
  int fd;
  size_t bytes_transferred;
  char s1[PATH_MAX];
  char s2[STR_LEN];
};

#endif
