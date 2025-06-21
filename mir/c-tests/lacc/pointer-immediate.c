#include <assert.h>
#include <stdio.h>

typedef float vec_t[3];

typedef struct {
  vec_t loc;
} point_t;

static int offsets[]
  = {(unsigned long) &((point_t *) 0)->loc[0], (unsigned long) &((point_t *) 0)->loc[2],
     (unsigned long) &((vec_t *) 0)[1]};

int main (void) {
  int *a = (((int *) 0x10) - 2);
  int *b = (((int *) 0x10) + 2);

  assert (a == (int *) 0x08);
  assert (b == (int *) 0x18);

  return printf ("%d, %d, %d\n", offsets[0], offsets[1], offsets[2]);
}
