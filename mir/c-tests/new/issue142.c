#include <stdarg.h>

int printf (char *, ...);
struct car {
  char a, b, c;
  long d;
};

int foo (int n, ...) {
  struct car c1, c2;
  va_list args;

  va_start (args, n);
  c1 = va_arg (args, struct car);
  int n1 = c1.a + c1.b + c1.c;
  c2 = va_arg (args, struct car);
  int n2 = c2.a + c2.b + c2.c;
  va_end (args);
  return n1 + n2;
}

int main (void) {
  struct car c1 = {1, 2, 3};
  struct car c2 = {4, 5, 6};
  printf ("%d\n", foo (1, c2, c1));
  return 0;
}
