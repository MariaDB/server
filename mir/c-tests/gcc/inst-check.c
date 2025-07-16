#include <stdarg.h>

extern void exit (int);
f(m)
{
  int i,s=0;
  for(i=0;i<m;i++)
    s+=i;
  return s;
}

main()
{
  exit (0);
}
