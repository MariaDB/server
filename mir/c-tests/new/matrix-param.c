#include <stdio.h>
static int mult (int m1[3][3], int m2[3][3]) {
  int el, res[3][3];

  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) {
      el = 0;
      for (int k = 0; k < 3; k++) el += m1[i][k] * m2[k][j];
      res[i][j] = el;
      printf ("(%d,%d) = %d\n", i, j, el);
    }
  el = 0;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) el += res[i][j];
  return el;
}

int main (void) {
  int m1[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
  int m2[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
  printf ("result = %d\n", mult (m1, m2));
  return 0;
}
