/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef MIR_ALLOC_H
#define MIR_ALLOC_H

#include <assert.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MIR_alloc {
  void *(*malloc) (size_t, void *);
  void *(*calloc) (size_t, size_t, void *);
  void *(*realloc) (void *, size_t, size_t, void *);
  void (*free) (void *, void *);
  void *user_data;
} *MIR_alloc_t;

static inline void *MIR_malloc (MIR_alloc_t alloc, size_t size) {
  assert (alloc != NULL);
  return alloc->malloc (size, alloc->user_data);
}

static inline void *MIR_calloc (MIR_alloc_t alloc, size_t num, size_t size) {
  assert (alloc != NULL);
  return alloc->calloc (num, size, alloc->user_data);
}

static inline void *MIR_realloc (MIR_alloc_t alloc, void *ptr, size_t old_size, size_t new_size) {
  assert (alloc != NULL);
  return alloc->realloc (ptr, old_size, new_size, alloc->user_data);
}

static inline void MIR_free (MIR_alloc_t alloc, void *ptr) {
  assert (alloc != NULL);
  alloc->free (ptr, alloc->user_data);
}

#ifdef __cplusplus
}
#endif

#endif /* #ifndef MIR_ALLOC_H */
