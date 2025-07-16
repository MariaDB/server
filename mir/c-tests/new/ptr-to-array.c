#include <stdio.h>
typedef unsigned long HARD_REG_SET[5];
long peep2_find_free_register (HARD_REG_SET *r) { return r[0][2]; }
int main (void) {
  HARD_REG_SET r = {1, 2, 3, 4, 5};
  long l = peep2_find_free_register (&r);
  printf ("%ld\n", l);
  return 0;
}
