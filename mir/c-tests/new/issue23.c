#include <stdio.h>
#include <string.h>
#include <math.h>
int main (void) {
  char buf[1000], n;
  n = sprintf (buf, "%lf", NAN);
  n += sprintf (&buf[n], ";%lf", INFINITY);
  n += sprintf (&buf[n], ";%lf", HUGE_VAL);
  n += sprintf (&buf[n], ";%f", HUGE_VALF);
  n += sprintf (&buf[n], ";%Lf", HUGE_VALL);
#ifdef _WIN32
  return strcmp (buf, "-nan(ind);inf;inf;inf;inf") != 0;
#else
  return strcmp (buf, "nan;inf;inf;inf;inf") != 0;
#endif
}
