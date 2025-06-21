#include <assert.h>

typedef struct Foo {
  int a;
  int b;
  int c;
} Foo;

int main () {
  Foo foo1 = {
    .a = 1,
    .b = 2,
    .c = 3,
  };
  assert (foo1.a == 1);
  assert (foo1.b == 2);
  assert (foo1.c == 3);

  Foo foo2 = {
    .b = 2,
    .a = 1,
    .c = 3,
  };
  assert (foo2.a == 1);
  assert (foo2.b == 2);
  assert (foo2.c == 3);
  return 0;
}
