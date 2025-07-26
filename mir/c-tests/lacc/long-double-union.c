int printf (const char *, ...);

union U1 {
  long double a;
} foo = {23.689896L};

union U2 {
  long double a;
  long b;
} bar = {123.235};

union U1 u1_ret (long double d) {
  union U1 r = {0};
  r.a = d + 2.3L;
#ifdef _WIN32 /* MSVC runtime wrongly print long double */
  printf ("u1_ret: %f, %f\n", (double) d, (double) r.a);
#else
  printf ("u1_ret: %Lf, %Lf\n", d, r.a);
#endif
  return r;
}

int u1_arg (union U1 u) {
  long double d = u.a;
  union U1 v;
  v = u1_ret (d + 54.789);
#ifdef _WIN32 /* MSVC runtime wrongly print long double */
  return printf ("u1_arg: %f, %f\n", (double) u.a, (double) v.a);
#else
  return printf ("u1_arg: %Lf, %Lf\n", u.a, v.a);
#endif
}

union U2 u2_ret (long double d) {
  union U2 r = {0};
  r.a = d + 3;
#ifdef _WIN32 /* MSVC runtime wrongly print long double */
  printf ("u2_ret: %f, %f\n", (double) d, (double) r.a);
#else
  printf ("u2_ret: %Lf, %Lf\n", d, r.a);
#endif
  return r;
}

int u2_arg (union U2 u) {
  long double d = u.a;
  union U2 v;
  v = u2_ret (d + 235.80);
#ifdef _WIN32 /* MSVC runtime wrongly print long double */
  return printf ("u2_arg: %f, %f\n", (double) u.a, (double) v.a);
#else
  return printf ("u2_arg: %Lf, %Lf\n", u.a, v.a);
#endif
}

int main (void) { return u1_arg (foo) + u2_arg (bar); }
