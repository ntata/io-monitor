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


// mq_listener.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdbool.h>
#include "domains.h"
#include "ops.h"
#include "ops_names.h"
#include "domains_names.h"
#include "mq.h"

struct monitor_record_t *dump;
int num_entries;

#define MAX_COLUMNS 4096
struct column {
  int pid;
  int ppid;
  char command[PATH_MAX];
  char exe_fn[PATH_MAX];
  
  /* positioning of graphical representation */
  int mar_top;
  int mar_left;
  int height;
  bool primary;
};
#define MAIN_COL_SEP 30
#define COL_WIDTH 10
int num_columns = 0;
int num_pri_columns = 0;
struct column columns[MAX_COLUMNS];
int find_col_by_pid(int pid)
{
  for (int i = 0 ; i!= num_columns ; ++i) {
    if (columns[i].pid == pid) {
      return i;
    }
  }
  return -1;
}
int top_pos = 200;
/* find names of primary columns */
void find_columns()
{
  for (int i = 0 ; i!= num_entries ; ++i) {
    top_pos++;
    if (dump[i].op_type == START) {
      if (num_columns == MAX_COLUMNS)
	break;
      bool pri_col = true;
      bool invisible = false;
      if (!strstr(dump[i].s1, "python")) {
	pri_col = false; /* if its not written in Python, it's not a primary column */
      }
      if (strstr(dump[i].s1, "sh")) {
	invisible = true;
	/* just skip sh processes for now */
      }
      columns[num_columns].pid = dump[i].pid;
      columns[num_columns].ppid = atoi(dump[i].s2);
      strcpy(columns[num_columns].command,dump[i].s1);
      columns[num_columns].mar_top = top_pos;
      columns[num_columns].mar_left =
	pri_col? MAIN_COL_SEP/2 + MAIN_COL_SEP * num_pri_columns
	: columns[find_col_by_pid(columns[num_columns].ppid)].mar_left
	+ COL_WIDTH * 2 / 3;
      columns[num_columns].exe_fn[0]=0;
      char *path_copy = strdup(dump[i].s1);
      char *tmp = path_copy;
      while (*tmp) {
	if (*tmp == ' ') *tmp = '/';
	tmp++;
      }
      tmp = path_copy;
      char *token;
      while ((token = strtok_r(tmp, "/", &tmp))) {
	if (strstr(token, "swift")) {
	  strcpy(columns[num_columns].exe_fn, token);
	  break;
	}
      }
      free(path_copy);
      if (!pri_col && -1 == find_col_by_pid(columns[num_columns].ppid)) {
	continue;
      }
      columns[num_columns].height = 0;
      if (invisible)
	columns[num_columns].height = 5;
      columns[num_columns].primary = pri_col;
      num_columns++;
      if (pri_col)
	num_pri_columns++;
    } else if (dump[i].op_type == STOP) {
      int cid = find_col_by_pid(dump[i].pid);
      columns[cid].height = top_pos - columns[cid].mar_top;

      if (columns[cid].height < 10) {
	top_pos += 10 - columns[cid].height;
	columns[cid].height = 10;
      }
    }
  }
  top_pos+=20;
  for (int i = 0 ; i != num_columns ; ++i) {
    if (columns[i].height < 5) {
      columns[i].height = top_pos - columns[i].mar_top;
    }


  }
}

void dump_columns()
{
  FILE * htm = fopen("html/out.htm", "w");
  fprintf(htm,"<head><title>Report</title>");
  fprintf(htm,"<style>"
	  ".dl {"
	  "border-left: solid black 1px;"
	  "position: absolute}\n "
	  ".tp {z-index: 3;"
	  "position: absolute;"
	  "background: white;"
	  "border: solid black 1px;"
	  "}\n"
	  ".tp .tooltip {"
	  " visibility: hidden;"
	  " position: fixed;"
	  " right: 0;"
	  " width: 500px;"
	  " height: 150px;"
	  " bottom: 0;"
	  " background: pink }\n"
	  " p {margin: 0.3em}\n"
	  ".tp:hover {background:yellow}\n"
	  "</style></head>"
	  );
  fprintf(htm,"<body>");
  fprintf(htm,"<div id=\"hintbox\" style=\""
	  "position: fixed;"
	  "z-index:80; opacity: 0.8;"
	  "background:gray;"
	  "height: 60px; padding: 0.0em;"
	  "left: 0; right:0; bottom:0;"
	  "\"> hover over element to get info </div>");
  for (int i = 0 ; i != num_columns ; ++i) {
    fprintf(htm,
	  "<div class=\"tp\" style=\""
	  "width: %dpx;"
	  "height: %dpx;"
	    "top: %dpx;"
	    "left: %dpx"
	    "\""
	    "onmouseover=\""
	    "document.getElementById('hintbox').innerHTML ="
	    "document.getElementById('h%d').innerHTML;"
	    
	    "\""
	    ">&nbsp; <div id=\"h%d\" class=\"tooltip\">"
	    "<p>Command: %s</p>"
	    "<p>PID: %d, PPID: %d</p>"
	    "</div> </div>\n",
	    COL_WIDTH,
	    columns[i].height,
	    columns[i].mar_top,
	    columns[i].mar_left,
	    i,i,
	    columns[i].command,
	    columns[i].pid,
	    columns[i].ppid
	  );
    if (columns[i].primary)
    fprintf(htm, "<div style=\""
	    "top:%dpx;"
	    "left:%dpx;"
	    "position:absolute;"
	    "font-family:fixed;"
	    "height: 10px;"
	    "font-height: 10px;"
	    "width:200px;"
	    "text-align:left;"
	    "transform: rotate(-90deg);"
	    "\">%s</div>",
	    columns[i].mar_top-110,
	    columns[i].mar_left-9*COL_WIDTH,
	    columns[i].exe_fn+6
	    );
    if (columns[i].primary) {
      fprintf(htm,"<div class=\"dl\" style=\""
	      "width: 4px;"
	      "height: %dpx;"
	      "top: 0px;"
	      "left: %dpx;"
	      "\">&nbsp;</div>\n",
	      top_pos+100,
	      columns[i].mar_left + COL_WIDTH/2);	      
    }
  }
  fprintf(htm,"</body>");
  fclose(htm);
}

int main (int argc, char** argv)
{
  int fd = open("dump1.d", O_RDONLY);
  int offs = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_CUR);
  dump = mmap(0, offs, PROT_READ, MAP_SHARED, fd, 0);
  num_entries = offs / sizeof(struct monitor_record_t);

  find_columns();
  dump_columns();

  
  munmap(dump, offs);
  close(fd);
}
