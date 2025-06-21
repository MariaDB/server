#include <assert.h>
#include <stdio.h>

static float fltnan = 0.0f / 0.0f;
static double dblnan = 0.0 / 0.0;
static long double ldblnan = 0.0l / 0.0l;

int main (int argc, char** argv) {
  int a = fltnan != fltnan;
  a += !(fltnan == fltnan);
  a += !(fltnan < fltnan);
  a += !(fltnan <= fltnan);
  a += !(fltnan > fltnan);
  a += !(fltnan >= fltnan);
  a += dblnan != dblnan;
  a += !(dblnan == dblnan);
  a += !(dblnan < dblnan);
  a += !(dblnan <= dblnan);
  a += !(dblnan > dblnan);
  a += !(dblnan >= dblnan);
  a += ldblnan != ldblnan;
  a += !(ldblnan == ldblnan);
  a += !(ldblnan < ldblnan);
  a += !(ldblnan <= ldblnan);
  a += !(ldblnan > ldblnan);
  a += !(ldblnan >= ldblnan);
  printf ("%s!\n", a == 18 ? "OK" : "FAIL");
  return 0;
}
