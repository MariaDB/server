#include <stdio.h>
struct S {
  int filler;
  struct {
    const char* str;
  };
};
void f (struct S a) { printf ("%s\n", a.str); }
int main (void) {
  f ((struct S){.str = "foo"});
  return 0;
}
