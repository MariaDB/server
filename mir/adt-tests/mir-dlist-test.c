#include "mir-dlist.h"

typedef struct elem *elem_t;

DEF_DLIST_LINK (elem_t);

struct elem {
  int v;
  DLIST_LINK (elem_t) link;
};

DEF_DLIST (elem_t, link);

int main (void) {
  int status;
  DLIST (elem_t) list;
  struct elem e1, e2, e3, e4;

  e1.v = 1;
  e2.v = 2;
  e3.v = 3;
  e4.v = 4;
  DLIST_INIT (elem_t, list);
  status = DLIST_LENGTH (elem_t, list) == 0;
  status &= DLIST_HEAD (elem_t, list) == NULL && DLIST_TAIL (elem_t, list) == NULL;

  DLIST_APPEND (elem_t, list, &e3);
  DLIST_APPEND (elem_t, list, &e4);
  DLIST_PREPEND (elem_t, list, &e2);
  DLIST_PREPEND (elem_t, list, &e1);
  status &= DLIST_LENGTH (elem_t, list) == 4;
  status &= DLIST_HEAD (elem_t, list) == &e1 && DLIST_TAIL (elem_t, list) == &e4;
  status &= DLIST_NEXT (elem_t, &e1) == &e2 && DLIST_PREV (elem_t, &e4) == &e3;
  status &= DLIST_EL (elem_t, list, 0) == &e1 && DLIST_EL (elem_t, list, 3) == &e4;
  status &= DLIST_EL (elem_t, list, -4) == &e1 && DLIST_EL (elem_t, list, -1) == &e4;

  DLIST_REMOVE (elem_t, list, &e1);
  DLIST_REMOVE (elem_t, list, &e3);
  status &= DLIST_LENGTH (elem_t, list) == 2;
  status &= DLIST_HEAD (elem_t, list) == &e2 && DLIST_TAIL (elem_t, list) == &e4;
  status &= DLIST_NEXT (elem_t, &e2) == &e4 && DLIST_PREV (elem_t, &e4) == &e2;

  DLIST_INSERT_BEFORE (elem_t, list, &e2, &e1);
  DLIST_INSERT_AFTER (elem_t, list, &e2, &e3);
  status &= DLIST_LENGTH (elem_t, list) == 4;
  status &= DLIST_HEAD (elem_t, list) == &e1 && DLIST_TAIL (elem_t, list) == &e4;
  status &= DLIST_NEXT (elem_t, &e1) == &e2 && DLIST_PREV (elem_t, &e4) == &e3;

  fprintf (stderr, status ? "DLIST OK\n" : "DLIST FAILURE!\n");
  return !status;
}
