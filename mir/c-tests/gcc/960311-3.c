extern void exit (int);
#include <stdio.h>

#ifdef DEBUG
#define abort() printf ("error, line %d\n", __LINE__)
#endif

int count;

void a1() { ++count; }

void
b (unsigned long data)
{
  if (data & 0x80000000) a1();
  data <<= 1;

  if (data & 0x80000000) a1();
  data <<= 1;

  if (data & 0x80000000) a1();
}

main ()
{
  count = 0;
  b (0);
  if (count != 0)
    abort ();

  count = 0;
  b (0x80000000);
  if (count != 1)
    abort ();

  count = 0;
  b (0x40000000);
  if (count != 1)
    abort ();

  count = 0;
  b (0x20000000);
  if (count != 1)
    abort ();

  count = 0;
  b (0xc0000000);
  if (count != 2)
    abort ();

  count = 0;
  b (0xa0000000);
  if (count != 2)
    abort ();

  count = 0;
  b (0x60000000);
  if (count != 2)
    abort ();

  count = 0;
  b (0xe0000000);
  if (count != 3)
    abort ();

#ifdef DEBUG
  printf ("Done.\n");
#endif
  exit (0);
}
