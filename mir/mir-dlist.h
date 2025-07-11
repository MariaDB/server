/* This file is part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* Typed doubly linked lists.  */

#ifndef MIR_DLIST_H

#define MIR_DLIST_H

#include <stdio.h>
#include <assert.h>

#if !defined(DLIST_ENABLE_CHECKING) && !defined(NDEBUG)
#define DLIST_ENABLE_CHECKING
#endif

#ifndef DLIST_ENABLE_CHECKING
#define DLIST_ASSERT(EXPR, OP, T) ((void) (EXPR))

#else
static inline void dlist_assert_fail (const char* op, const char* var) {
  fprintf (stderr, "wrong %s for %s", op, var);
  assert (0);
}

#define DLIST_ASSERT(EXPR, OP, T) (void) ((EXPR) ? 0 : (dlist_assert_fail (OP, #T), 0))

#endif

#ifdef __GNUC__
#define MIR_DLIST_UNUSED __attribute__ ((unused))
#else
#define MIR_DLIST_UNUSED
#endif

#define DLIST(T) DLIST_##T
#define DLIST_OP(T, OP) DLIST_##T##_##OP
#define DLIST_OP_DEF(T, OP) MIR_DLIST_UNUSED DLIST_OP (T, OP)
#define DLIST_LINK(T) DLIST_LINK_##T

#define DLIST_LINK_T(T)           \
  typedef struct DLIST_LINK (T) { \
    T prev, next;                 \
  } DLIST_LINK (T)

#define DEF_DLIST_LINK(T) DLIST_LINK_T (T);

#define DEF_DLIST_TYPE(T)    \
  typedef struct DLIST (T) { \
    T head, tail;            \
  } DLIST (T)

#define DEF_DLIST_CODE(T, LINK)                                                                    \
                                                                                                   \
  static inline void DLIST_OP_DEF (T, init) (DLIST (T) * list) { list->head = list->tail = NULL; } \
                                                                                                   \
  static inline T DLIST_OP_DEF (T, head) (DLIST (T) * list) { return list->head; }                 \
                                                                                                   \
  static inline T DLIST_OP_DEF (T, tail) (DLIST (T) * list) { return list->tail; }                 \
                                                                                                   \
  static inline T DLIST_OP_DEF (T, prev) (T elem) { return elem->LINK.prev; }                      \
  static inline T DLIST_OP_DEF (T, next) (T elem) { return elem->LINK.next; }                      \
                                                                                                   \
  static inline T DLIST_OP_DEF (T, el) (DLIST (T) * list, int n) {                                 \
    T e;                                                                                           \
                                                                                                   \
    if (n >= 0) {                                                                                  \
      for (e = list->head; e != NULL && n != 0; e = e->LINK.next, n--)                             \
        ;                                                                                          \
    } else {                                                                                       \
      for (e = list->tail; e != NULL && n != -1; e = e->LINK.prev, n++)                            \
        ;                                                                                          \
    }                                                                                              \
    return e;                                                                                      \
  }                                                                                                \
                                                                                                   \
  static inline void DLIST_OP_DEF (T, prepend) (DLIST (T) * list, T elem) {                        \
    DLIST_ASSERT (list&& elem, "prepend", T);                                                      \
    if (list->head == NULL) {                                                                      \
      DLIST_ASSERT (list->tail == NULL, "prepend", T);                                             \
      list->tail = elem;                                                                           \
    } else {                                                                                       \
      DLIST_ASSERT (list->head->LINK.prev == NULL, "prepend", T);                                  \
      list->head->LINK.prev = elem;                                                                \
    }                                                                                              \
    elem->LINK.prev = NULL;                                                                        \
    elem->LINK.next = list->head;                                                                  \
    list->head = elem;                                                                             \
  }                                                                                                \
                                                                                                   \
  static inline void DLIST_OP_DEF (T, append) (DLIST (T) * list, T elem) {                         \
    DLIST_ASSERT (list&& elem, "append", T);                                                       \
    if (list->tail == NULL) {                                                                      \
      DLIST_ASSERT (list->head == NULL, "append", T);                                              \
      list->head = elem;                                                                           \
    } else {                                                                                       \
      DLIST_ASSERT (list->tail->LINK.next == NULL, "append", T);                                   \
      list->tail->LINK.next = elem;                                                                \
    }                                                                                              \
    elem->LINK.next = NULL;                                                                        \
    elem->LINK.prev = list->tail;                                                                  \
    list->tail = elem;                                                                             \
  }                                                                                                \
                                                                                                   \
  static inline void DLIST_OP_DEF (T, insert_before) (DLIST (T) * list, T before, T elem) {        \
    DLIST_ASSERT (list&& before&& elem && list->tail, "insert_before", T);                         \
    if (before->LINK.prev == NULL) {                                                               \
      DLIST_ASSERT (list->head == before, "insert_before", T);                                     \
      before->LINK.prev = elem;                                                                    \
      elem->LINK.next = before;                                                                    \
      elem->LINK.prev = NULL;                                                                      \
      list->head = elem;                                                                           \
    } else {                                                                                       \
      DLIST_ASSERT (list->head, "insert_before", T);                                               \
      before->LINK.prev->LINK.next = elem;                                                         \
      elem->LINK.prev = before->LINK.prev;                                                         \
      before->LINK.prev = elem;                                                                    \
      elem->LINK.next = before;                                                                    \
    }                                                                                              \
  }                                                                                                \
                                                                                                   \
  static inline void DLIST_OP_DEF (T, insert_after) (DLIST (T) * list, T after, T elem) {          \
    DLIST_ASSERT (list&& after&& elem && list->head, "insert_after", T);                           \
    if (after->LINK.next == NULL) {                                                                \
      DLIST_ASSERT (list->tail == after, "insert_after", T);                                       \
      after->LINK.next = elem;                                                                     \
      elem->LINK.prev = after;                                                                     \
      elem->LINK.next = NULL;                                                                      \
      list->tail = elem;                                                                           \
    } else {                                                                                       \
      DLIST_ASSERT (list->tail, "insert_after", T);                                                \
      after->LINK.next->LINK.prev = elem;                                                          \
      elem->LINK.next = after->LINK.next;                                                          \
      after->LINK.next = elem;                                                                     \
      elem->LINK.prev = after;                                                                     \
    }                                                                                              \
  }                                                                                                \
                                                                                                   \
  static inline void DLIST_OP_DEF (T, remove) (DLIST (T) * list, T elem) {                         \
    DLIST_ASSERT (list&& elem, "remove", T);                                                       \
    if (elem->LINK.prev != NULL) {                                                                 \
      elem->LINK.prev->LINK.next = elem->LINK.next;                                                \
    } else {                                                                                       \
      DLIST_ASSERT (list->head == elem, "remove", T);                                              \
      list->head = elem->LINK.next;                                                                \
    }                                                                                              \
    if (elem->LINK.next != NULL) {                                                                 \
      elem->LINK.next->LINK.prev = elem->LINK.prev;                                                \
    } else {                                                                                       \
      DLIST_ASSERT (list->tail == elem, "remove", T);                                              \
      list->tail = elem->LINK.prev;                                                                \
    }                                                                                              \
    elem->LINK.prev = elem->LINK.next = NULL;                                                      \
  }                                                                                                \
                                                                                                   \
  static inline size_t DLIST_OP_DEF (T, length) (DLIST (T) * list) {                               \
    size_t len = 0;                                                                                \
    T curr;                                                                                        \
                                                                                                   \
    for (curr = list->head; curr != NULL; curr = curr->LINK.next) len++;                           \
    return len;                                                                                    \
  }

#define DEF_DLIST(T, LINK) \
  DEF_DLIST_TYPE (T);      \
  DEF_DLIST_CODE (T, LINK)

#define DLIST_INIT(T, L) (DLIST_OP (T, init) (&(L)))
#define DLIST_HEAD(T, L) (DLIST_OP (T, head) (&(L)))
#define DLIST_TAIL(T, L) (DLIST_OP (T, tail) (&(L)))
#define DLIST_PREV(T, E) (DLIST_OP (T, prev) (E))
#define DLIST_NEXT(T, E) (DLIST_OP (T, next) (E))
#define DLIST_EL(T, L, N) (DLIST_OP (T, el) (&(L), N))
#define DLIST_PREPEND(T, L, E) (DLIST_OP (T, prepend) (&(L), (E)))
#define DLIST_APPEND(T, L, E) (DLIST_OP (T, append) (&(L), (E)))
#define DLIST_INSERT_BEFORE(T, L, B, E) (DLIST_OP (T, insert_before) (&(L), (B), (E)))
#define DLIST_INSERT_AFTER(T, L, A, E) (DLIST_OP (T, insert_after) (&(L), (A), (E)))
#define DLIST_REMOVE(T, L, E) (DLIST_OP (T, remove) (&(L), (E)))
#define DLIST_LENGTH(T, L) (DLIST_OP (T, length) (&(L)))

#endif /* #ifndef MIR_DLIST_H */
