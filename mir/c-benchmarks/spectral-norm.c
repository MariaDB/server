/* The Computer Language Benchmarks Game
 * https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
 *
 * Contributed by Mr Ledrug
 *
 * Algorithm lifted from Intel Fortran #2 code by Steve Decker et al.
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static inline int A(int i, int j) {
   return ((i+j) * (i+j+1) / 2 + i + 1);
}

double dot(double * v, double * u, int n) {
   int i;
   double sum = 0;
   for (i = 0; i < n; i++)
      sum += v[i] * u[i];
   return sum;
}

void mult_Av(double * v, double * out, const int n) {
   int i, j;
   double sum;
   //#   pragma omp parallel for private(sum,j)
   for (i = 0; i < n; i++) {
      for (sum = j = 0; j < n; j++)
         sum += v[j] / A(i,j);
      out[i] = sum;
   }
}

void mult_Atv(double * v, double * out, const int n) {
   int i, j;
   double sum;
   //#   pragma omp parallel for private(sum,j)
   for (i = 0; i < n; i++) {
      for (sum = j = 0; j < n; j++)
         sum += v[j] / A(j,i);
      out[i] = sum;
   }
}

double *tmp;
void mult_AtAv(double *v, double *out, const int n) {
   mult_Av(v, tmp, n);
   mult_Atv(tmp, out, n);
}

int main(int argc, char**argv) {
   int n = atoi(argv[1]);
   if (n <= 0) n = 2000;

   double *u, *v;
   u = malloc(n * sizeof(double));
   v = malloc(n * sizeof(double));
   tmp = malloc(n * sizeof(double));

   int i;
   for (i = 0; i < n; i++) u[i] = 1;
   for (i = 0; i < 10; i++) {
      mult_AtAv(u, v, n);
      mult_AtAv(v, u, n);
   }

   printf("%.9f\n", sqrt(dot(u,v, n) / dot(v,v,n)));

   return 0;
}
