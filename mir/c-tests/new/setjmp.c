#include <setjmp.h>

extern void printf (const char *str, ...);
extern void exit (int);
static jmp_buf env;

static void foo (void) {
  longjmp (env, 1);
  exit (1);
}
static void (*foop) (void) = foo;

static void bar (void) { (*foop) (); }
static void (*barp) (void) = bar;

int main (void) {
  int i = 42;
  if (setjmp (env)) {
    return i != 42;
  }
  (*barp) ();
  return 1;
}
