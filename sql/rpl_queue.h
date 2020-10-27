#ifndef RPL_QUEUE_H
#define RPL_QUEUE_H
#include "queue.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#ifndef EVENT_LEN_OFFSET
#define EVENT_LEN_OFFSET 9
#endif
class slave_queue_element
{
  public:
  uchar *event, *tail;
  bool malloced;
  /*
   Control flags will only in put when this event is start of new transaction
  */
  uchar flags;
  //event_length + flags(1 byte)
  uint total_length;
  slave_queue_element(uchar* ptr, uchar* buffer_start, uchar* buffer_end)
  {
    //READ the EVENT_LENGTH;
    uchar len[4];
    ulong size= buffer_end - buffer_start;
    ulong ptr_numeric= ptr - buffer_start;
    ulong ev_length_start= (ptr_numeric + EVENT_LEN_OFFSET) %size;
    ulong ev_length_end= (ptr_numeric + EVENT_LEN_OFFSET + 4) %size;
    //EVENT_LEN_OFFSET is in continues memory
    if( ev_length_start< ev_length_end)
    {
      total_length= uint4korr(buffer_start + ev_length_start);
    }
    else
    {
      ulong remainder= ev_length_end;
      memcpy(len, ptr+ ev_length_start, 4 - remainder);
      memcpy(len+4-remainder, buffer_start, remainder);
      total_length= uint4korr(len);
    }
   //Event in continues memory chunk.
    if (ptr_numeric <  (ptr_numeric+total_length) % size)
    {
      event= ptr;
      malloced= false;
      tail= event+ total_length;
    }
    else
    {
       //malloc and memcpy to continues memory chunk
      malloced= true;
      //QTODO
      event= (uchar *)my_malloc(0, total_length, MYF(MY_WME));
      ulong remainder= (ptr_numeric + total_length) % size;
      memcpy(event, ptr, total_length - remainder);
      memcpy(event+total_length - remainder, buffer_start, remainder);
      tail= buffer_start + remainder;
    }
  }
  slave_queue_element(uchar *ev)
  {
    malloced= false;
    total_length= uint4korr(ev+EVENT_LEN_OFFSET);
    event= ev;
  }
  //We need to wrap arround in case we overstoot buffer end.
  uchar* write(uchar *ptr, uchar *buffer_start, uchar * buffer_end)
  {
    uint64 t_len= MY_MIN(total_length, buffer_end - ptr);
    memcpy(ptr, event, t_len);
    if (t_len < total_length)
    {
      memcpy(ptr, event, t_len);
      memcpy(buffer_start, event + t_len, total_length - t_len);
      return buffer_start + total_length - t_len;
    }
    //NO wrapping needed
    else
    {
      memcpy(ptr, event, total_length);
      return ptr + total_length;
    }
  }
  ~slave_queue_element()
  {
    if (malloced)
      free(event);
  }

  //QTODO
  //Control flags
  uchar NEW_TRANSACTION= 1;
  uchar RELAY_LOGGED= 2;
  uchar COMMITTED= 4;
  // Reserved for use by the circular queue
  // UNUSED_SPACE = 0xFF
};
typedef circular_buffer_queue<slave_queue_element> r_queue;
#endif  //RPL_QUEUE_H
