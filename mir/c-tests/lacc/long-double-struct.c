int printf (const char *, ...);

struct S1 {
  long double a;
} foo = {23.689896L};

struct S2 {
  long b;
  long double a;
  char c;
} bar = {2, 123.235, 'a'};

struct S1 s1_ret (long double d) {
  struct S1 r = {0};
  r.a = d + 2.3L;
#ifdef _WIN32 /* MSVC runtime wrongly print long double */
  printf ("s1_ret: %f, %f\n", (double) d, (double) r.a);
#else
  printf ("s1_ret: %Lf, %Lf\n", d, r.a);
#endif
  return r;
}

int s1_arg (struct S1 u) {
  long double d = u.a;
  struct S1 v;
  v = s1_ret (d + 54.789);
#ifdef _WIN32 /* MSVC runtime wrongly print long double */
  return printf ("s1_arg: %f, %f\n", (double) u.a, (double) v.a);
#else
  return printf ("s1_arg: %Lf, %Lf\n", u.a, v.a);
#endif
}

struct S2 s2_ret (long double d) {
  struct S2 r = {0};
  r.b = 32;
  r.c = 'b';
  r.a = d + 3;
#ifdef _WIN32 /* MSVC runtime wrongly print long double */
  printf ("s2_ret: %f, (%f, %ld, %d)\n", (double) d, (double) r.a, r.b, r.c);
#else
  printf ("s2_ret: %Lf, (%Lf, %ld, %d)\n", d, r.a, r.b, r.c);
#endif
  return r;
}

int s2_arg (struct S2 u) {
  long double d = u.a;
  struct S2 v;
  v = s2_ret (d + 235.80);
#ifdef _WIN32 /* MSVC runtime wrongly print long double */
  return printf ("s2_arg: (%f, %ld, %d), (%f, %ld, %d)\n", (double) u.a, u.b, u.c, (double) v.a,
                 v.b, v.c);
#else
  return printf ("s2_arg: (%Lf, %ld, %d), (%Lf, %ld, %d)\n", u.a, u.b, u.c, v.a, v.b, v.c);
#endif
}

int main (void) { return s1_arg (foo) + s2_arg (bar); }
