#include "sql_circular_queue.h"
#include "log_event.h"


inline int circular_queue::init(MEM_ROOT *mem_root, size_t length)
{
  if ((buffer= (uchar *)alloc_root(mem_root, length)))
    return 1;
  buffer_end= buffer + length;
  return 0;
}

inline uint circular_queue::next_gtid_log_event_pos(uint64 &start_position)
{
  uint64 event_length= start_position;
  DBUG_ASSERT(*(buffer+start_position+EVENT_TYPE_OFFSET) ==
                                                    GTID_LOG_EVENT);
  do
  {
    start_position+= *(buffer+start_position+EVENT_LEN_OFFSET);
  }
  while(*(buffer+start_position+EVENT_TYPE_OFFSET) != GTID_LOG_EVENT);
  return start_position - event_length;
}
void * circular_queue::read(int64 length)
{
  uint64 start_position= read_ptr_cached.load(std::memory_order_relaxed);
  if (length == READ_ONE_TRANSACTION)
  {
    uint transaction_end_pos;
    while(1)
      if (*(buffer+start_position+EVENT_TYPE_OFFSET) == GTID_LOG_EVENT)
      {
        do
          transaction_end_pos= next_gtid_log_event_pos(buffer, start_position);
        while(read_ptr_cached.compare_exchange_weak(start_position, start_position
                                                     + transaction_end_pos));
        return buffer + (start_position - transaction_end_pos);
      }
  }
  else if(length == READ_ONE_EVENT)
  {
    uint event_end_pos;
    while(1)
    {
      do
      {
        event_end_pos= *(buffer+start_position+EVENT_LEN_OFFSET);
        DBUG_ASSERT(event_end_pos <= buffer_read_end);
        if (event_end_pos == buffer_read_end)
        {
          start_position= 0;
        }
      }
      while(read_ptr_cached.compare_exchange_weak(start_position, start_position
                                                     + event_end_pos));
      return buffer + (start_position - event_end_pos);
    }
  }
   return NULL;
}

