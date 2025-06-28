#include <assert.h>
static int f () { return 1; }
typedef int (*F) ();
static F pf = &f;
static F* ppf = &pf;
int main (int argc, char** argv) {
  assert (((*ppf) () == (int) 1));
  return 0;
}
