#include <stdio.h>
#include <stdint.h>
enum E1 { E1C = -1, E1D = 0x7fffffff };
enum E2 { E2C = +0, E2D = 0xffffffff };
enum E3 { E3C = -1, E3D = 0x80000000 };
enum E4 { E4C = +0, E4D = 0x100000000 };
enum E5 { E5C = INT32_MIN, E5D = 0 };
enum E6 { E6C = INT64_MIN, E6D = 0 };
int main (void) {
  printf ("E1=%d, E2=%d, E3=%d, E4=%d, E5=%d, E6=%d\n", sizeof (enum E1), sizeof (enum E2),
          sizeof (enum E3), sizeof (enum E4), sizeof (enum E5), sizeof (enum E6));
  return 0;
}
