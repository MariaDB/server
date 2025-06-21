#include <assert.h>
#include <stdio.h>

static float fz = 0.0f, fltnan = 0.0f / 0.0f;
static double dz = 0.0, dblnan = 0.0 / 0.0;
static long double ldz = 0.0l, ldblnan = 0.0l / 0.0l;

int main (int argc, char** argv) {
  int a = (fz == fltnan);
  a += (fz != fltnan);
  a += (fz < fltnan);
  a += (fz <= fltnan);
  a += (fz > fltnan);
  a += (fz >= fltnan);
  a += (dz == dblnan);
  a += (dz != dblnan);
  a += (dz < dblnan);
  a += (dz <= dblnan);
  a += (dz > dblnan);
  a += (dz >= dblnan);
  a += (ldz == ldblnan);
  a += (ldz != ldblnan);
  a += (ldz < ldblnan);
  a += (ldz <= ldblnan);
  a += (ldz > ldblnan);
  a += (ldz >= ldblnan);
  printf ("%s!\n", a == 3 ? "OK" : "FAIL");
  return 0;
}
