/* { dg-xfail-if "Can not call system libm.a with -msoft-float" { powerpc-*-aix* rs6000-*-aix* } { "-msoft-float" } { "" } } */
#include <math.h>
extern void exit (int);

main()
{
  volatile double a;
  double c;
  a = 32.0;
  c = pow(a, 1.0/3.0);
  if (c + 0.1 > 3.174802
      && c - 0.1 < 3.174802)
    exit (0);
  else
    abort ();
}
