#include <assert.h>
#include <stdio.h>

static long long foo = 43 + (long long) "What" - 1;

int main (void) {
  int a = "abc" == (void *) 0;

  assert (a == 0 && "Hello");
  assert (a == 1 || "Hello");

  if ("foo" || "bar") {
    printf ("if\n");
  }

  while ("bar") {
    printf ("while\n");
    break;
  }

  printf ("%s\n", ((char *) foo) - 42);

  return printf ("a = %d\n", a);
}
