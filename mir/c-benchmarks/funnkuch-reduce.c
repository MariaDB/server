

/* The Computer Language Benchmarks Game
 * https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
 *
 * contributed by Ledrug Katz
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* this depends highly on the platform.  It might be faster to use
   char type on 32-bit systems; it might be faster to use unsigned. */

typedef int elem;

elem s[16], t[16];

int maxflips = 0;
int max_n;
int odd = 0;
int checksum = 0;

int flip()
{
   register int i;
   register elem *x, *y, c;

   for (x = t, y = s, i = max_n; i--; )
      *x++ = *y++;
   i = 1;
   do {
      for (x = t, y = t + t[0]; x < y; )
         c = *x, *x++ = *y, *y-- = c;
      i++;
   } while (t[t[0]]);
   return i;
}

static inline void rotate(int n)
{
   elem c;
   register int i;
   c = s[0];
   for (i = 1; i <= n; i++) s[i-1] = s[i];
   s[n] = c;
}

/* Tompkin-Paige iterative perm generation */
void tk(int n)
{
   int i = 0, f;
   elem c[16] = {0};

   while (i < n) {
      rotate(i);
      if (c[i] >= i) {
         c[i++] = 0;
         continue;
      }

      c[i]++;
      i = 1;
      odd = ~odd;
      if (*s) {
         f = s[s[0]] ? flip() : 1;
         if (f > maxflips) maxflips = f;
         checksum += odd ? -f : f;
      }
   }
}

int main(int argc, char **v)
{
   int i;

   if (argc < 2) {
      fprintf(stderr, "usage: %s number\n", v[0]);
      exit(1);
   }

   max_n = atoi(v[1]);
   if (max_n < 3 || max_n > 15) {
      fprintf(stderr, "range: must be 3 <= n <= 12\n");
      exit(1);
   }

   for (i = 0; i < max_n; i++) s[i] = i;
   tk(max_n);

   printf("%d\nPfannkuchen(%d) = %d\n", checksum, max_n, maxflips);

   return 0;
}
