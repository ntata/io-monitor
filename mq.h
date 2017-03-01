#ifndef __MQ_H
#define __MQ_H
#include "monitor_record.h"


typedef struct _MONITOR_MESSAGE
{
   long message_type;
   struct monitor_record_t monitor_record;
} MONITOR_MESSAGE;

#endif //__MQ_H
