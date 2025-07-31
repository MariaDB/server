#include <stdio.h>
typedef unsigned long uint64_t;
struct S1 {
  volatile signed f0 : 26;
  volatile uint64_t f1;
  unsigned : 0;
  unsigned f2 : 12;
  volatile signed f3 : 1;
};

static struct S1 g_90 = {72, 18446744073709551615UL, 46, -0}; /* VOLATILE GLOBAL g_90 */
int main (void) {
  printf ("%d,%lu,%d,%d(size=%d)\n", g_90.f0, g_90.f1, g_90.f2, g_90.f3, sizeof (struct S1));
  return 0;
}
