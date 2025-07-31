/* -*- mode: c -*-
 * $Id: sieve.gcc,v 1.7 2001/05/06 04:37:45 doug Exp $
 * http://www.bagley.org/~doug/shootout/
 */

#include <stdio.h>
#include <stdlib.h>

int main (int argc, char *argv[]) {
  int NUM = ((argc == 2) ? atoi (argv[1]) : 1);
  static char flags[8192 + 1];
  long i, k;
  int count = 0;

  while (NUM--) {
    count = 0;
    for (i = 2; i <= 8192; i++) {
      flags[i] = 1;
    }
    for (i = 2; i <= 8192; i++) {
      if (flags[i]) {
        // remove all multiples of prime: i
        for (k = i + i; k <= 8192; k += i) {
          flags[k] = 0;
        }
        count++;
      }
    }
  }
  printf ("Count: %d\n", count);
  return (0);
}
