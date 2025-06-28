int printf (const char *, ...);

int main (void) {
  double c1 = -15.3L;
  long double c2 = 22.5f;

  long double a = (long double) c1 + c2, b = c1 - c2, c = c1 * c2, d = c1 / c2;
#ifdef _WIN32 /* MSVC runtime wrongly print long double */
  return printf ("(%f, %f), %f, %f, %f, %f\n", c1, (double) c2, (double) a, (double) b, (double) c,
                 (double) d);
#else
  return printf ("(%f, %Lf), %Lf, %Lf, %Lf, %Lf\n", c1, c2, a, b, c, d);
#endif
}
