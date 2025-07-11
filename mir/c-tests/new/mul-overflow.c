void printf (const char *, ...);

int a = 0xffff, b = 0xffff, c = 8;
unsigned ua = 0x10000, ub = 0x10000, uc = 8;
long la = 0xffffffff, lb = 0xffffffff, lc = 8;
unsigned long ula = 0x100000000, ulb = 0x100000000, ulc = 8;

int main (void) {
  int r;
  unsigned ur;
  long lr;
  unsigned long ulr;
  if (!__builtin_mul_overflow (a, b, &r)) return 42;
  if (!__builtin_mul_overflow (ua, ub, &ur)) return 43;
  if (!__builtin_mul_overflow (la, lb, &lr)) return 44;
  if (!__builtin_mul_overflow (ula, ulb, &ulr)) return 45;
  if (__builtin_mul_overflow (c, c, &r)) return 46;
  if (__builtin_mul_overflow (uc, uc, &ur)) return 47;
  if (__builtin_mul_overflow (lc, lc, &lr)) return 48;
  if (__builtin_mul_overflow (ulc, ulc, &ulr)) return 49;
  printf ("%d,%u,%ld,%lu\n", r, ur, lr, ulr);
  return 0;
}
