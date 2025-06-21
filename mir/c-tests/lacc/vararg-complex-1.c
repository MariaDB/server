#include <stdarg.h>

/* Complex enough to be passed on stack. */
struct object {
  char u;
  long a, b, c, d;
  short v;
};

/* Take n integer arguments, except that the third is of type struct object. */
struct object sum (int n, struct object obj, ...) {
  int value = 0, i;
  va_list args;

  obj.u = 'g';
  obj.a = 12;

  va_start (args, obj);
  for (i = 0; i < n; ++i) {
    if (i == 2) {
      struct object u = va_arg (args, struct object);
      obj.c = u.u + u.a + u.b + u.c + u.d + u.v;
    } else {
      value += va_arg (args, int);
    }
  }
  va_end (args);

  obj.d = value + obj.d;
  return obj;
}

int main () {
  struct object obj = {'a', 1, 2, 3, 4, 5};
  struct object arg = {'b', 6, 5, 8, 9, 3};
  obj = sum (7, obj, 10, 8, arg, 42, 2, 3, 4);
  return (obj.u + obj.a + obj.b + obj.c + obj.d + obj.v) != 324;
}
