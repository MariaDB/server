#include "stdio.h"
#include "my_sys.h"

#include "ring_buffer.hpp"


int main() {
  uchar buff[20];

  RingBuffer bf((char*)"input.txt", 4096);

  bf.read(buff, 10);
  bf.write((uchar*)"123", 3);
  bf.read(buff+10, 10);
}