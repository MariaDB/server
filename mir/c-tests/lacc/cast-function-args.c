int printf (const char *, ...);

int foo (unsigned long long a, unsigned char b, float f, double d) {
  return printf ("%llu, %u, %f, %f\n", a, b, f, d);
}

int main (void) {
  int i = -3;
  float f = 3.14f;
  double d = 2.71;
  return foo (i, i, d, f);
}
