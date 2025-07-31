int a1 = 10, b1 = -20;
int a2 = 0x7fffffff, b2 = -1;
unsigned ua1 = 20, ub1 = 10;
unsigned ua2 = 1, ub2 = 0xffffffff;
long la1 = 10, lb1 = -20;
long la2 = 0x7fffffffffffffff, lb2 = -1;
unsigned long lua1 = 20, lub1 = 10;
unsigned long lua2 = 1, lub2 = 0xffffffffffffffff;
extern void abort (void);
int main (void) {
#if __has_builtin(__builtin_sub_overflow)
  int res;
  unsigned ures;
  long lres;
  unsigned long lures;
  if (__builtin_sub_overflow (a1, b1, &res)) abort ();
  if (!__builtin_sub_overflow (a2, b2, &res)) abort ();
  if (__builtin_sub_overflow (ua1, ub1, &ures)) abort ();
  if (!__builtin_sub_overflow (ua2, ub2, &ures)) abort ();
  if (__builtin_sub_overflow (la1, lb1, &lres)) abort ();
  if (!__builtin_sub_overflow (la2, lb2, &lres)) abort ();
  if (__builtin_sub_overflow (lua1, lub1, &lures)) abort ();
  if (!__builtin_sub_overflow (lua2, lub2, &lures)) abort ();
#endif
  return 0;
}
