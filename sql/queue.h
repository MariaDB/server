#ifndef QUEUE_H
#define QUEUE_H

#include "my_global.h"
#include "my_base.h"
#include "my_pthread.h"
#include "my_sys.h"
#include "mysql/psi/mysql_thread.h"
#include <cstring>

#define UNUSED_SPACE 0xFF


/*
  We will use N-1 to check whether buffer is full or not.
  Comments
  #  Free Space
  *  Filled Space
  H  Head
  T  Tail
*/
template <typename Element_type>
class circular_buffer_queue
{
 public:
  uchar *buffer, *buffer_end;
  //Total no of events currently in queue
  ulong events;
  ulong buffer_size;
  mysql_mutex_t lock_queue;
  mysql_mutex_t free_queue;
  mysql_cond_t free_cond;
  uchar *head, *tail;
  ulong free_size()
  {
    if (head > tail)
      return buffer_size - (head-tail)-1;
    if (tail > head)
      return tail-head-1;
    return buffer_size - 1;
  }
  ulong used_buffer()
  {
    return buffer_size - free_size() -1;
  }
  circular_buffer_queue(){};

  int init(ulong buffer_size)
  {
    if (!(buffer= (uchar*)my_malloc(PSI_INSTRUMENT_ME, buffer_size,
                               MYF(MY_THREAD_SPECIFIC|MY_WME))))
      return 1;
    this->buffer_size= buffer_size;
    buffer_end= buffer + buffer_size;
    head= tail= buffer;
    mysql_mutex_init(0, &lock_queue, MY_MUTEX_INIT_SLOW);
    mysql_mutex_init(0, &free_queue, MY_MUTEX_INIT_SLOW);
    mysql_cond_init(0, &free_cond, 0);
    return 0;
  }

  void destroy()
  {
    my_free(buffer);
    mysql_mutex_destroy(&lock_queue);
    mysql_mutex_destroy(&free_queue);
    mysql_cond_destroy(&free_cond);
  }

  /*
     We want to write in continues memory.
   */
  int enqueue(Element_type *elem)
  {
    uint32 length= elem->total_length;
    if (free_size() < length)
      return 1;
    mysql_mutex_lock(&lock_queue);
    head= elem->write(head, buffer, buffer_end);
    mysql_mutex_unlock(&lock_queue);
    return 0;
  };

  Element_type *dequeue()
  {
    if (used_buffer() > 0)
    {
      mysql_mutex_lock(&lock_queue);
      Element_type *el= new Element_type(tail, buffer, buffer_end);
      //We are not going to unlock mutex till we get explicit call of
      //unlock_mutex by caller thread (that means sql thread has copied data
      //into its buffer)
      tail= el->tail;
      return el;
    }
    return NULL;
  }

  int waited_enqueue(Element_type *elem)
  {
    mysql_mutex_lock(&free_queue);
    while(free_size() < elem->total_length)
      mysql_cond_wait(&free_cond, &free_queue);
    mysql_mutex_unlock(&free_queue);
    return enqueue(elem);
  }

  void lock_mutex()
  {
    mysql_mutex_lock(&lock_queue);
  }
  void unlock_mutex()
  {
    mysql_mutex_unlock(&lock_queue);
    mysql_cond_broadcast(&free_cond);
  }

  void do_wait(uint32 size)
  {
    while(free_size() < size)
      mysql_cond_wait(&free_cond, &free_queue);
  }
};


#endif    /* QUEUE_H  */
