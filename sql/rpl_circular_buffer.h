#ifndef RPL_CIRCULAR_BUFFER_INCLUDED
#define RPL_CIRCULAR_BUFFER_INCLUDED

#include <mutex>
class rpl_circular_buffer
{
public:
  enum buffer_granularity
  {
    ONE_EVENT = 1,
    ONE_TRANSACTION,
  };
  int init(MEM_ROOT *mem_root, uint64 size);
  uint64 buffer_size(){ return size;};
  uint64 empty_space();
  uint64 end_unused_space();
  uchar* read(buffer_granularity read_type);
  /*
   It will always be written on continues memory, So suppose if we reach near
   buffer end and we dont have enough space for next event/transaction then we
   will write from starting (if we have already flushed the data)
   In stort our granularity will be either full transaction or full event.
  */
  uint64 write(uchar* data, buffer_granularity write_type);
  /*
   Move the flush pointer to given ptr
   */
  void flush(uchar *ptr){flush_head= ptr;};
  bool is_full()
  {
    if(write_head == read_head && elements)
      return TRUE;
    return FALSE;
  }
private:
  uchar* buffer;
  uchar* buffer_end;
  uint64 size;
  uint elements;
  /*
   Some time we can have empty space in end if transaction/event is big to fit
   in continues space.
  */
  uchar* buffer_usable_ptr;
  // Actual free space
  uint64 usable_free_space;
  uchar* write_head;
  uchar* read_head;
  uchar* flush_head;
  std::mutex read_lock;
  std::mutex write_lock;
};



#endif //RPL_CIRCULAR_BUFFER_INCLUDED
