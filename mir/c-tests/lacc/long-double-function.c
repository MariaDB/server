#include <stdarg.h>

int printf (const char *, ...);

struct S1 {
  long l;
  long double ld;
};

struct S1 pack (long l, long double ld) {
  struct S1 s = {0}, *r;
  s.l = l;
  s.ld = ld;
  r = &s;
  return *r;
}

long double sum (long double g, long double f) { return g + f; }

long double diff (int n, ...) {
  int i;
  long double ld = 0;
  va_list args;
  va_start (args, n);
  for (i = 0; i < n; ++i) {
    ld = ld - va_arg (args, long double);
  }
  va_end (args);
  return ld;
}

int main (void) {
  struct S1 s1;
  double c1 = -15.3;
  long double c2 = 22.5f, c3;

  c2 = sum (c1, c2);
  s1 = pack (42L, c2);
  c3 = diff (2, c2, s1.ld, 2.89L);

#ifdef _WIN32 /* MSVC runtime wrongly print long double */
  return printf ("%f, {%ld, %f}, %f\n", (double) c2, s1.l, (double) s1.ld, (double) c3);
#else
  return printf ("%Lf, {%ld, %Lf}, %Lf\n", c2, s1.l, s1.ld, c3);
#endif
}
