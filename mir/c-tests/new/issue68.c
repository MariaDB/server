#include <stdio.h>
int foo (int c0, int c1, int c2, int c3, int c4, int c5, int c6) { return c0 + c1 + c6; }
int (*f) (int c0, int c1, int c2, int c3, int c4, int c5, int c6) = foo;
int main (void) {
  printf ("%d\n", f (1, 3, 4, 5, 6, 7, 8));
  return 0;
}
