int a1 = 10, b1 = 20;
int a2 = 0x7fffffff, b2 = 1;
unsigned ua1 = 10, ub1 = 20;
unsigned ua2 = 0xffffffff, ub2 = 1;
long la1 = 10, lb1 = 20;
long la2 = 0x7fffffffffffffff, lb2 = 1;
unsigned long lua1 = 10, lub1 = 20;
unsigned long lua2 = 0xffffffffffffffff, lub2 = 1;
extern void abort (void);
int main (void) {
#if __has_builtin(__builtin_add_overflow)
  int res;
  unsigned ures;
  long lres;
  unsigned long lures;
  if (__builtin_add_overflow (a1, b1, &res)) abort ();
  if (!__builtin_add_overflow (a2, b2, &res)) abort ();
  if (__builtin_add_overflow (ua1, ub1, &ures)) abort ();
  if (!__builtin_add_overflow (ua2, ub2, &ures)) abort ();
  if (__builtin_add_overflow (la1, lb1, &lres)) abort ();
  if (!__builtin_add_overflow (la2, lb2, &lres)) abort ();
  if (__builtin_add_overflow (lua1, lub1, &lures)) abort ();
  if (!__builtin_add_overflow (lua2, lub2, &lures)) abort ();
#endif
  return 0;
}
