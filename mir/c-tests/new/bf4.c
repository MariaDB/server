int printf (const char *, ...);
struct S0 {
  short f0;
  volatile unsigned f1 : 3;
  volatile unsigned f2 : 11;
};
struct S1 {
  char f0;
  char f1;
  const volatile struct S0 f2;
};
static struct S1 g_86;
static struct S1 func_1 (void) { return g_86; }
int main (void) {
  printf ("%d,%d\n", sizeof (struct S0), sizeof (struct S1));
  return 0;
}
