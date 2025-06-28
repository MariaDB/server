/* -*- mode: c -*-
 * $Id: except.gcc,v 1.1 2001/01/07 04:31:15 doug Exp $
 * http://www.bagley.org/~doug/shootout/
 */

#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>

int HI = 0, LO = 0;

static jmp_buf Hi_exception;
static jmp_buf Lo_exception;

void blowup (int n) {
  if (n & 1) {
    longjmp (Lo_exception, 1);
  } else {
    longjmp (Hi_exception, 1);
  }
}

void lo_function (volatile int n) {
  if (setjmp (Lo_exception) != 0) {
    LO++;
  } else {
    blowup (n);
  }
}

void hi_function (volatile int n) {
  if (setjmp (Hi_exception) != 0) {
    HI++;
  } else {
    lo_function (n);
  }
}

void some_function (int n) { hi_function (n); }

int main (int argc, char *argv[]) {
  int volatile N = ((argc == 2) ? atoi (argv[1]) : 1);

  while (N) {
    some_function (N--);
  }
  printf ("Exceptions: HI=%d / LO=%d\n", HI, LO);
  return (0);
}
