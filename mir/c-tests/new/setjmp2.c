extern void printf (const char *str, ...);
extern void exit (int);
#include <setjmp.h>

static jmp_buf env;

static void foo (void) {
  longjmp (env, 1);
  exit (1);
}
static void (*foop) (void) = foo;

static void bar (void) { (*foop) (); }
static void (*barp) (void) = bar;
static int (*setjmp2) (jmp_buf) = setjmp;

int main (void) {
  int i = 42;
  if (setjmp2 (env)) {
    return i != 42;
  }
  (*barp) ();
  return 1;
}
