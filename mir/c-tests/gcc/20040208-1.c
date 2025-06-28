extern void exit (int);
int main () {
#ifndef _WIN32 /* long double > 64 bits */
  long double x;

  x = 0x1.0p-500L;
  x *= 0x1.0p-522L;
  if (x != 0x1.0p-1022L) abort ();
#endif
  exit (0);
}
