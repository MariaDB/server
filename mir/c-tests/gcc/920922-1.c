extern void exit (int);
unsigned long*
f(p)unsigned long*p;
{
  unsigned long a = (*p++) >> 24;
  return p + a;
}

main ()
{
  unsigned long x = 0x80000000UL;
  if (f(&x) != &x + 0x81)
    abort();
  exit(0);
}
