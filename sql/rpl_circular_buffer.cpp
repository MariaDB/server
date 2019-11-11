#include "rpl_circular_buffer.h"


int rpl_circular_buffer::init(MEM_ROOT* mem_root, uint64 size)
{
  if ((buffer= (uchar* )alloc_root(mem_root, size)))
    return 1;
  buffer_end= buffer + size - 1;
  usable_free_space= size;
  buffer_usable_ptr= buffer_end;
  write_head= read_head= flush_head= buffer;
  elements= 0;
  return 0;
}

uint64 rpl_circular_buffer::empty_space()
{
  if (write_head == read_head && !elements)
    return 0;
  /*
   Empty space for this case
   S= Buffer_start
   F= Flush Pointer
   W= write pointer
   E= buffer end
   U= Buffer usable ptr
   S--F----W-----E
   Empty space= E - W + F -S

   S--W-- F --- U--E
   Empty space = F-W
  */
  if (write_head > flush_head)
    return (buffer_end - write_head) + (flush_head - buffer);
  else
    return flush_head - write_head;

}

uint64 rpl_circular_buffer::write(uchar *data, buffer_granularity write_type)
{
  //Will take very less time because there is only one write thread.
  //write_lock.lock()
  // Look for usable_free_space if it is more than transaction size then we will
  // write other wise we will wait untill we have enough space.
  //TODO FIND transaction size.
  uint64 trans_size= 100;
  /*
    TS = Transaction size/EVENT Size

    if this case
    S--F----W----E
    if E-W > TS
      then write data and W+= TS

    if E-W < TS
      buffer_usable_ptr= write_head
      write_head= 0
    It will become the second case


    if this case
    S--W----F--U--E
    if TS < F -W
      write
      W+= TS
    else
      give error that buffer is full
    and write into file

  */
  if (empty_space() <  trans_size)
    return 0;
  if (write_head > flush_head)
  {
    if (buffer_end - write_head > trans_size)
    {
      //MEMORY_ORDER_RELEASE should be there for lock free
      memcpy(write_head, data, trans_size);
      write_head+= trans_size;
      elements++;
      return trans_size;
    }
    else
    {
      write_head= buffer;
      if (empty_space() <  trans_size)
        return 0;
    }
  }
  if ((write_head < flush_head) && ((flush_head - write_head) > trans_size))
  {
    //MEMORY_ORDER_RELEASE should be there for lock free
    memcpy(write_head, data, trans_size);
    write_head+= trans_size;
    elements++;
    return trans_size;
  }
  return 0;
}

uchar* rpl_circular_buffer::read(buffer_granularity granularity)
{
  uint64 trans_size= 100;
  uchar* return_addr= NULL;
  /*
   Read from queue
   lock the read mutex
    R < W
   S--R---W--E

   R+= TS/ES
   unlock the mutex
   return the old read ptr

   R > W
   S--W---R--U--E
   R += TS/ES
   if R == U
     R= 0
   unlock the mutex
   return the old ptr
  */
  read_lock.lock();
  return_addr= read_head;
  if (read_head < write_head)
  {
    read_head+= trans_size;
    read_lock.unlock();
    return return_addr;
  }
  else
  {
    read_head+= trans_size;
    if (read_head == buffer_usable_ptr)
      read_head= 0;
    return return_addr;
  }
}
