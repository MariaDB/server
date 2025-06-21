void printf (const char *fmt, ...);
void abort (void);
#if defined(_WIN32) || !defined(SIEVE_BENCH)
/* limitation for alloca */
#define SieveSize 8190
#define Expected 1027
#else
#define SieveSize 819000
#define Expected 65333
#endif
#define N_ITER 1000
int sieve (int n) {
  long i, k, count, iter, prime;
  char flags[SieveSize];

  for (iter = 0; iter < n; iter++) {
    count = 0;
    for (i = 0; i < SieveSize; i++) flags[i] = 1;
    for (i = 2; i < SieveSize; i++)
      if (flags[i]) {
        prime = i + 1;
        for (k = i + prime; k < SieveSize; k += prime) flags[k] = 0;
        count++;
      }
  }
  return count;
}
int main (void) {
  int n = sieve (N_ITER);

  printf ("%d iterations of sieve for %d: result = %d\n", N_ITER, SieveSize, n);
  if (n != Expected) abort ();
  return 0;
}
