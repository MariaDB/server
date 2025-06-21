/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef MIR_VARR_H
#define MIR_VARR_H

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "mir-alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(VARR_ENABLE_CHECKING) && !defined(NDEBUG)
#define VARR_ENABLE_CHECKING
#endif

#ifndef VARR_ENABLE_CHECKING
#define VARR_ASSERT(EXPR, OP, T) ((void) (EXPR))

#else
static inline void mir_varr_assert_fail (const char *op, const char *var) {
  fprintf (stderr, "wrong %s for %s", op, var);
  assert (0);
}

#define VARR_ASSERT(EXPR, OP, T) (void) ((EXPR) ? 0 : (mir_varr_assert_fail (OP, #T), 0))

#endif

#ifdef __GNUC__
#define MIR_VARR_UNUSED __attribute__ ((unused))
#define MIR_VARR_NO_RETURN __attribute__ ((noreturn))
#else
#define MIR_VARR_UNUSED
#define MIR_VARR_NO_RETURN
#endif

static inline void MIR_VARR_NO_RETURN mir_varr_error (const char *message) {
#ifdef MIR_VARR_ERROR
  MIR_VARR_ERROR (message);
  assert (0);
#else
  fprintf (stderr, "%s\n", message);
#endif
  exit (1);
}

/*---------------- Typed variable length arrays -----------------------------*/
#define VARR_CONCAT2(A, B) A##B
#define VARR_CONCAT3(A, B, C) A##B##C
#define VARR(T) VARR_CONCAT2 (VARR_, T)
#define VARR_OP(T, OP) VARR_CONCAT3 (VARR_, T, OP)
#define VARR_OP_DEF(T, OP) MIR_VARR_UNUSED VARR_OP (T, OP)

#define VARR_T(T)           \
  typedef struct VARR (T) { \
    size_t els_num;         \
    size_t size;            \
    T *varr;                \
    MIR_alloc_t alloc;      \
  } VARR (T)

#define VARR_DEFAULT_SIZE 64

/* Vector of pointer to object.  */
#define DEF_VARR(T)                                                                           \
  VARR_T (T);                                                                                 \
                                                                                              \
  static inline void VARR_OP_DEF (T, create) (VARR (T) * *varr, MIR_alloc_t alloc,            \
                                              size_t size) {                                  \
    VARR (T) * va;                                                                            \
    if (size == 0) size = VARR_DEFAULT_SIZE;                                                  \
    *varr = va = (VARR (T) *) MIR_malloc (alloc, sizeof (VARR (T)));                          \
    if (va == NULL) mir_varr_error ("varr: no memory");                                       \
    va->els_num = 0;                                                                          \
    va->size = size;                                                                          \
    va->varr = (T *) MIR_malloc (alloc, size * sizeof (T));                                   \
    va->alloc = alloc;                                                                        \
  }                                                                                           \
                                                                                              \
  static inline void VARR_OP_DEF (T, destroy) (VARR (T) * *varr) {                            \
    VARR (T) *va = *varr;                                                                     \
    MIR_alloc_t alloc = va->alloc;                                                            \
    VARR_ASSERT (va && va->varr, "destroy", T);                                               \
    MIR_free (alloc, va->varr);                                                               \
    MIR_free (alloc, va);                                                                     \
    *varr = NULL;                                                                             \
  }                                                                                           \
                                                                                              \
  static inline size_t VARR_OP_DEF (T, length) (const VARR (T) * varr) {                      \
    VARR_ASSERT (varr, "length", T);                                                          \
    return varr->els_num;                                                                     \
  }                                                                                           \
                                                                                              \
  static inline size_t VARR_OP_DEF (T, capacity) (const VARR (T) * varr) {                    \
    VARR_ASSERT (varr, "size", T);                                                            \
    return varr->size;                                                                        \
  }                                                                                           \
                                                                                              \
  static inline T *VARR_OP_DEF (T, addr) (const VARR (T) * varr) {                            \
    VARR_ASSERT (varr, "addr", T);                                                            \
    return &varr->varr[0];                                                                    \
  }                                                                                           \
                                                                                              \
  static inline T VARR_OP_DEF (T, last) (const VARR (T) * varr) {                             \
    VARR_ASSERT (varr && varr->varr && varr->els_num, "last", T);                             \
    return varr->varr[varr->els_num - 1];                                                     \
  }                                                                                           \
                                                                                              \
  static inline T VARR_OP_DEF (T, get) (const VARR (T) * varr, size_t ix) {                   \
    VARR_ASSERT (varr && varr->varr && ix < varr->els_num, "get", T);                         \
    return varr->varr[ix];                                                                    \
  }                                                                                           \
                                                                                              \
  static inline void VARR_OP_DEF (T, set) (const VARR (T) * varr, size_t ix, T obj) {         \
    VARR_ASSERT (varr && varr->varr && ix < varr->els_num, "set", T);                         \
    varr->varr[ix] = obj;                                                                     \
  }                                                                                           \
                                                                                              \
  static inline void VARR_OP_DEF (T, trunc) (VARR (T) * varr, size_t size) {                  \
    VARR_ASSERT (varr && varr->varr && varr->els_num >= size, "trunc", T);                    \
    varr->els_num = size;                                                                     \
  }                                                                                           \
                                                                                              \
  static inline int VARR_OP_DEF (T, expand) (VARR (T) * varr, size_t size) {                  \
    VARR_ASSERT (varr && varr->varr, "expand", T);                                            \
    MIR_alloc_t alloc = varr->alloc;                                                          \
    if (varr->size < size) {                                                                  \
      size += size / 2;                                                                       \
      varr->varr = (T *) MIR_realloc (alloc, varr->varr, sizeof (T) * varr->size,             \
                                      sizeof (T) * size);                                     \
      varr->size = size;                                                                      \
      return 1;                                                                               \
    }                                                                                         \
    return 0;                                                                                 \
  }                                                                                           \
                                                                                              \
  static inline void VARR_OP_DEF (T, tailor) (VARR (T) * varr, size_t size) {                 \
    VARR_ASSERT (varr && varr->varr, "tailor", T);                                            \
    MIR_alloc_t alloc = varr->alloc;                                                          \
    if (varr->size != size)                                                                   \
      varr->varr = (T *) MIR_realloc (alloc, varr->varr, sizeof (T) * varr->size,             \
                                      sizeof (T) * size);                                     \
    varr->els_num = varr->size = size;                                                        \
  }                                                                                           \
                                                                                              \
  static inline void VARR_OP_DEF (T, push) (VARR (T) * varr, T obj) {                         \
    T *slot;                                                                                  \
    VARR_OP (T, expand) (varr, varr->els_num + 1);                                            \
    slot = &varr->varr[varr->els_num++];                                                      \
    *slot = obj;                                                                              \
  }                                                                                           \
                                                                                              \
  static inline void VARR_OP_DEF (T, push_arr) (VARR (T) * varr, const T *objs, size_t len) { \
    size_t i;                                                                                 \
    T *slot;                                                                                  \
    VARR_OP (T, expand) (varr, varr->els_num + len);                                          \
    for (i = 0; i < len; i++) {                                                               \
      slot = &varr->varr[varr->els_num++];                                                    \
      *slot = objs[i];                                                                        \
    }                                                                                         \
  }                                                                                           \
                                                                                              \
  static inline T VARR_OP_DEF (T, pop) (VARR (T) * varr) {                                    \
    T obj;                                                                                    \
    VARR_ASSERT (varr && varr->varr && varr->els_num, "pop", T);                              \
    obj = varr->varr[--varr->els_num];                                                        \
    return obj;                                                                               \
  }

#define VARR_CREATE(T, V, A, L) (VARR_OP (T, create) (&(V), A, L))
#define VARR_DESTROY(T, V) (VARR_OP (T, destroy) (&(V)))
#define VARR_LENGTH(T, V) (VARR_OP (T, length) (V))
#define VARR_CAPACITY(T, V) (VARR_OP (T, capacity) (V))
#define VARR_ADDR(T, V) (VARR_OP (T, addr) (V))
#define VARR_LAST(T, V) (VARR_OP (T, last) (V))
#define VARR_GET(T, V, I) (VARR_OP (T, get) (V, I))
#define VARR_SET(T, V, I, O) (VARR_OP (T, set) (V, I, O))
#define VARR_TRUNC(T, V, S) (VARR_OP (T, trunc) (V, S))
#define VARR_EXPAND(T, V, S) (VARR_OP (T, expand) (V, S))
#define VARR_TAILOR(T, V, S) (VARR_OP (T, tailor) (V, S))
#define VARR_PUSH(T, V, O) (VARR_OP (T, push) (V, O))
#define VARR_PUSH_ARR(T, V, A, L) (VARR_OP (T, push_arr) (V, A, L))
#define VARR_POP(T, V) (VARR_OP (T, pop) (V))
#define VARR_FOREACH_ELEM(T, V, I, EL) \
  for ((I) = 0; (I) >= VARR_LENGTH (T, V) ? 0 : (EL = VARR_GET (T, V, I), 1); (I)++)

#ifdef __cplusplus
}
#endif

#endif /* #ifndef MIR_VARR_H */
