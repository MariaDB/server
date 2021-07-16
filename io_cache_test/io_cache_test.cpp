#include "stdio.h"
#include "my_sys.h"

#include "ring_buffer.hpp"


int main() {
  uchar buff[20];
  int fd;
  fd = my_open("input.txt",O_CREAT | O_RDWR,MYF(MY_WME));

  RingBuffer bf(fd, 4096);

  bf.read(buff, 10);
  bf.write((uchar*)"123", 3);
  bf.read(buff+10, 10);

  my_close(fd, MYF(MY_WME));
}