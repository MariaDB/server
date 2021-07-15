#include "stdio.h"
#include "my_sys.h"

#include "ring_buffer.hpp"


int main() {
  int fd;

  RingBuffer rg;


  IO_CACHE cache;


  fd = my_open("input.txt",O_CREAT | O_RDWR,MYF(MY_WME));
  init_io_cache(&cache, fd, 4096, SEQ_READ_APPEND, 0,0, MYF(MY_WME));


  end_io_cache(&cache);
  my_close(cache.file, MYF(MY_WME));
}