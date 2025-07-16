#include <stdarg.h>
struct car {
  char a;
  long d;
};

o (int n, ...) {
  struct car c0, c;
  va_list args;
  va_arg (args, struct car);
  int n0;
  c = va_arg (args, struct car);
  int n2 = (args);
  return 0;
}

int main (void) { return 0; }
