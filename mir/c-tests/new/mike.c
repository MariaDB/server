#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

int main () {
  uint8_t *buf;
  // buf = 1 == 0 ? (uint8_t*)0 : (uint8_t*)malloc(16);    // OK
  buf = 1 == 0 ? 0 : (uint8_t *) malloc (16);  // CRASH
  buf[0] = 123;
  printf ("buf[0]: %d\n", buf[0]);

  return 0;
}
