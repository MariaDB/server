#include <stdlib.h>
#include <assert.h>

typedef struct Residue {
  short (*residue_books)[8];
} Residue;

int main () {
  Residue r;
  Residue* pr = &r;
  void* p = malloc (sizeof (pr->residue_books[0]) * 10);
  pr->residue_books = (short (*)[8]) p;
  for (int i = 0; i < 10; ++i)
    for (int j = 0; j < 8; ++j) pr->residue_books[i][j] = 0xffff;
  assert (p == pr->residue_books);
  free (pr->residue_books);
  return 0;
}
