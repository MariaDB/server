

/* -*- mode: c -*-
 * $Id: matrix.gcc,v 1.6 2001/03/31 15:52:48 doug Exp $
 * http://www.bagley.org/~doug/shootout/
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SIZE 30

int **mkmatrix (int rows, int cols) {
  int i, j, count = 1;
  int **m = (int **) malloc (rows * sizeof (int *));
  for (i = 0; i < rows; i++) {
    m[i] = (int *) malloc (cols * sizeof (int));
    for (j = 0; j < cols; j++) {
      m[i][j] = count++;
    }
  }
  return (m);
}

void zeromatrix (int rows, int cols, int **m) {
  int i, j;
  for (i = 0; i < rows; i++)
    for (j = 0; j < cols; j++) m[i][j] = 0;
}

void freematrix (int rows, int **m) {
  while (--rows > -1) {
    free (m[rows]);
  }
  free (m);
}

int **mmult (int rows, int cols, int **m1, int **m2, int **m3) {
  int i, j, k, val;
  for (i = 0; i < rows; i++) {
    for (j = 0; j < cols; j++) {
      val = 0;
      for (k = 0; k < cols; k++) {
        val += m1[i][k] * m2[k][j];
      }
      m3[i][j] = val;
    }
  }
  return (m3);
}

int main (int argc, char *argv[]) {
  int i, n = ((argc == 2) ? atoi (argv[1]) : 1);

  int **m1 = mkmatrix (SIZE, SIZE);
  int **m2 = mkmatrix (SIZE, SIZE);
  int **mm = mkmatrix (SIZE, SIZE);

  for (i = 0; i < n; i++) {
    mm = mmult (SIZE, SIZE, m1, m2, mm);
  }
  printf ("%d %d %d %d\n", mm[0][0], mm[2][3], mm[3][2], mm[4][4]);

  freematrix (SIZE, m1);
  freematrix (SIZE, m2);
  freematrix (SIZE, mm);
  return (0);
}
