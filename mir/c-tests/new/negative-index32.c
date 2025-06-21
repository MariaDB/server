#include <stdio.h>
int a[3] = {1, 2, 3};
int n = 3;
int main (void) {
  int *s = a + 3, sum = 0;
  for (int i = -3; i < 0; i++) sum += s[i];
  printf ("sum=%d\n", sum);
  return 0;
}
