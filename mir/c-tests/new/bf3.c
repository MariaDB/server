#include <stdio.h>
struct S1 {
  unsigned f0 : 20;
  volatile unsigned f1 : 5;
  unsigned f2 : 25;
  volatile unsigned f3 : 14;
  unsigned f4 : 20;
  volatile signed char f5;
  volatile signed : 0;
  unsigned f6 : 15;
  unsigned f7 : 2;
  signed f8 : 21;
};
static volatile struct S1 g = {666, 3, 3554, 83, 783, 0x19L, 3, 1, -291};
int main (void) {
  printf ("%d,%d,%d, %d,%d,%x, %d,%d,%d\n", g.f0, g.f1, g.f2, g.f3, g.f4, g.f5, g.f6, g.f7, g.f8);
  return 0;
}
