#ifndef SQL_CIRCULAR_QUEUE_INCLUDED
#define SQL_CIRCULAR_QUEUE_INCLUDED
#include <atomic>

#define READ_ONE_EVENT 0
#define READ_ONE_TRANSACTION -1


/*
  In memory lock free circular queue
*/

class circular_queue
{
public:
  int init(MEM_ROOT *mem_root, size_t length);
  void * read(int64 length);
  size_t write(const void* data, size_t length);
  int reset_queue();
  int delete_queue();
private:
  uchar *buffer;
  uchar *buffer_end;
  uint next_gtid_event_len_pos(uint64 &start_position);
  char pad1[CPU_LEVEL1_DCACHE_LINESIZE];
  std::atomic <uint64> read_ptr_cached;
  char pad2[CPU_LEVEL1_DCACHE_LINESIZE];
  std::atomic <uint64> read_ptr_flush;
  char pad3[CPU_LEVEL1_DCACHE_LINESIZE];
  std::atomic <uint64> write_ptr_cached;
  char pad4[CPU_LEVEL1_DCACHE_LINESIZE];
  std::atomic <uint64> write_ptr;
  char pad5[CPU_LEVEL1_DCACHE_LINESIZE];
};


#endif /* SQL_CIRCULAR_QUEUE_INCLUDED */
