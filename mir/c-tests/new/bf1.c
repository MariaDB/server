#include <stdio.h>
/* --- Struct/Union Declarations --- */
struct S0 {
  signed f0 : 25;
  signed f1 : 12;
  unsigned char f2;
  signed : 0;
  signed f3 : 7;
  volatile signed f4 : 25;
};
static struct S0 g_102 = {5332, 50, 0xADL, 4, -1877}; /* VOLATILE GLOBAL g_102 */

/* ---------------------------------------- */
int main (void) {
  printf ("%d,%d,%x,%d,%d(size=%d)\n", g_102.f0, g_102.f1, g_102.f2, g_102.f3, g_102.f4,
          sizeof (struct S0));
  return 0;
}
