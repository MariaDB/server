#ifdef _WIN32 /* too long bitfield */
int main (void) { return 68; }
#else
int printf (const char *, ...);

struct A {
  long a : 40;
} a1 = {187134098732};

static int test_a (void) {
  long l = a1.a;
  return printf ("%ld\n", l);
}

struct B {
  struct A a;
  short b : 4;
} b1 = {3, -2};

static int test_b (void) {
  int foo = b1.b;
  return printf ("%ld, %d\n", (long) b1.a.a, foo);
}

struct C {
  short a : 3;
  char b : 5;
  char c : 3;
} c1 = {3};

static int test_c (void) { return printf ("{%d, %d, %d}\n", c1.a, c1.b, c1.c); }

struct D {
  unsigned long a : 32;
  unsigned long : 4;
  unsigned long b : 20;
} d1 = {-1, -1}, d2 = {0, 1241}, d3 = {0};

static int test_d (struct D d) { return printf ("{%u, %u}\n", d.a, d.b); }

int main (void) {
  printf ("sizeof(struct A) = %lu\n", sizeof (struct A));
  printf ("sizeof(struct B) = %lu\n", sizeof (struct B));
  printf ("sizeof(struct C) = %lu\n", sizeof (struct C));
  return test_a () + test_b () + test_c () + test_d (d1) + test_d (d2) + test_d (d3);
};
#endif
