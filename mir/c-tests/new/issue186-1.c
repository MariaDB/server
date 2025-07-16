#include <assert.h>
#include <stdio.h>

static float fltnan = 0.0f / 0.0f;
static double dblnan = 0.0 / 0.0;
static long double ldblnan = 0.0l / 0.0l;

int main (int argc, char** argv) {
  assert (fltnan != fltnan);
  assert (!(fltnan == fltnan));
  assert (!(fltnan < fltnan));
  assert (!(fltnan <= fltnan));
  assert (!(fltnan > fltnan));
  assert (!(fltnan >= fltnan));
  assert (dblnan != dblnan);
  assert (!(dblnan == dblnan));
  assert (!(dblnan < dblnan));
  assert (!(dblnan <= dblnan));
  assert (!(dblnan > dblnan));
  assert (!(dblnan >= dblnan));
  assert (ldblnan != ldblnan);
  assert (!(ldblnan == ldblnan));
  assert (!(ldblnan < ldblnan));
  assert (!(ldblnan <= ldblnan));
  assert (!(ldblnan > ldblnan));
  assert (!(ldblnan >= ldblnan));
  printf ("OK!\n");
  return 0;
}
