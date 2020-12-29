#ifndef RPL_QUEUE_H
#define RPL_QUEUE_H
#include "queue.h"
#include <cmath>
#include <cstddef>
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
   Not used 
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
    //NO wrapping needed //QTODO
    else
    {
      memcpy(ptr, event, total_length);
      return ptr + total_length;
    }
  }
  ~slave_queue_element()
  {
    if (malloced)
      my_free(event);
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
/*
  Second implementation of queue.h element
  Features:- 
    Storage in continues memory
    Storage of objects
    No event length needs to be stored we will be using create_log_event_or_get_size
class slave_queue_element_2
{
  public:
  uchar *event, *tail;
  bool malloced;
  uint total_length;

  uint get_length(Log_event *ev)
  {
    uint32 size= 0;
    create_log_event_or_get_size(NULL, 0, NULL, ev->get_type_code(), NULL, &size);
    return size;
  }
  slave_queue_element_2(uchar* ptr, uchar* buffer_start, uchar* buffer_end)
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
  slave_queue_element_2(uchar *ev)
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
    //NO wrapping needed //QTODO
    else
    {
      memcpy(ptr, event, total_length);
      return ptr + total_length;
    }
  }
  ~slave_queue_element_2()
  {
    if (malloced)
      my_free(event);
  }

  //QTODO
  //Control flags
  uchar NEW_TRANSACTION= 1;
  uchar RELAY_LOGGED= 2;
  uchar COMMITTED= 4;
  // Reserved for use by the circular queue
  // UNUSED_SPACE = 0xFF
};

*/


//TODO we are same method as base class , but this is for benchmark only
//should be modifies later
class circular_buffer_queue_events :public circular_buffer_queue<slave_queue_element>
{
  public:
  //Since we need continues memory space we can have some 
  //gap in the end , This will keep track of gaps
  //It should be updated as we cycle through the whole
  //buffer again and again
  //It should be treated as a read only
  void *logical_buffer_end;

  int init(ulong buffer_size)
  {
    circular_buffer_queue::init(buffer_size);
    logical_buffer_end= buffer_end;
    return 0;
  }
  /*
   * Enqueue is divided in 2 parts
   * enqueue_1 and enqueue_2
   * enqueue_1 will return the old pointer with the guarantee that size N can be written
   * without overriding the tail pointer (it will be continues)
   * enqueue_2 will update the head pointer
   *
   * It is assumed that there is only on producer  !important
   */

  //enqueue_1
  //enqueue part one --> get the size / or wait till we have size
  //head pointer will not be updated
  //Working :- we will return the head pointer with guarantee that allocated size
  //will not override the tail pointer
  
  public:
  void * enqueue_1(uint32 size)
  {
    mysql_mutex_lock(&free_queue);
    //We need continues block of memory
    //Case 1 buffer_end - head < size
    // [---T-----H--] 
    // Solution
    // H = 0 and see if T - H > size , update the logical_buffer_end
    // case 2
    // [---H--------T----]
    // nothing to worry , just free size comparison should be enough
    // Case 1
    if (head > tail || head ==tail)// queue is empty for second cond
    {
      if ((buffer_end - head) > size)
      {
        do_wait(size);
      }
      else
      {
        logical_buffer_end= head;
        //No need to grab mutex while updating head
        //since there is onlt one producer
        head= buffer;
        do_wait(size);
      }

    }
    else
    {
      do_wait(size);
    }
    mysql_mutex_unlock(&free_queue);
    return head;
  };
  
  void enqueue_2(uint32 size)
  {
    //We will never we out of bound of buffer_end 
    //enqueue_1 will take care of it.
    head+= size;
  }
  
  void* dequeue_1(uint32 size)
  {
    if (used_buffer() > 0 )
    {
      void *old_tail= NULL; 
      lock_mutex();
      if (tail < logical_buffer_end)
      {
        old_tail= tail;
        tail+= size;
      }
      else
      {
        //tail will never be > then logical_buffer_end
        assert(tail == logical_buffer_end);
        tail= buffer;
        old_tail= tail;
        tail+= size;
      }
      unlock_mutex();
      return old_tail;
    }
    return NULL;
  }

};
#endif  //RPL_QUEUE_H
