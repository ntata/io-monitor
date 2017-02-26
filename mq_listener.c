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


static const int MESSAGE_QUEUE_PROJECT_ID = 'm';

typedef struct _MONITOR_MESSAGE
{
   long message_type;
   char monitor_record[256];
} MONITOR_MESSAGE;

//*****************************************************************************

int main(int argc, char* argv[]) {
   const char* message_queue_path;
   int message_queue_key;
   int message_queue_id;
   int rc;
   ssize_t message_size_received;
   MONITOR_MESSAGE monitor_message;

   if (argc < 2) {
      printf("error: missing arguments\n");
      printf("usage: %s <msg-queue-path>\n", argv[0]);
      exit(1);
   }

   message_queue_path = argv[1];

   message_queue_key = ftok(message_queue_path, MESSAGE_QUEUE_PROJECT_ID);
   if (message_queue_key == -1) {
      printf("error: unable to obtain key for message queue path '%s'\n",
             message_queue_path);
      printf("errno: %d\n", errno);
      exit(1);
   }

   message_queue_id = msgget(message_queue_key, (0664 | IPC_CREAT));
   if (message_queue_id == -1) {
      printf("error: unable to obtain id for message queue path '%s'\n",
             message_queue_path);
      printf("errno: %d\n", errno);
      exit(1);
   }

   while (1) {
      memset(&monitor_message, 0, sizeof(MONITOR_MESSAGE));
      message_size_received = msgrcv(message_queue_id,
                                     &monitor_message,   // void* ptr
                                     256,   // size_t nbytes
                                     0,   // long type
                                     0);  // int flag
      if (message_size_received > 0) {
         printf("%s\n", monitor_message.monitor_record);
      } else {
         printf("rc = %zu\n", message_size_received);
         printf("errno = %d\n", errno);
      }
   }

   return 0;
}

//*****************************************************************************

