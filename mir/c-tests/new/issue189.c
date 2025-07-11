#include <assert.h>

typedef struct Boo {
  float value;
} Boo;
typedef struct Foo {
  Boo boos[4];
} Foo;
typedef struct {
  Boo v[4];
} Boo_arr4;

int main () {
  Foo foo;
  (*(Boo_arr4*) foo.boos) = (Boo_arr4){{(Boo){.value = 1.0f}}};
  assert (foo.boos[0].value == 1.0f);
  assert (foo.boos[1].value == 0.0f);
  return 0;
}
