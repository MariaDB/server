#include <assert.h>

typedef struct Empty {
} Empty;
typedef struct Foo {
  Empty e;
  int x;
} Foo;
static Empty empty;

Foo getfoo (int x) { return (Foo){.e = empty, .x = x}; }
Foo getfoo2 (int x) { return (Foo){.x = x, .e = empty}; }

int main () {
  Foo foo = getfoo (2);
  assert (foo.x == 2);
  Foo foo2 = getfoo2 (42);
  assert (foo2.x == 42);
  return 0;
}
