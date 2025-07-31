/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include <stdlib.h>
#include "mir-alloc.h"

#ifdef __GNUC__
#define ALLOC_UNUSED __attribute__ ((unused))
#else
#define ALLOC_UNUSED
#endif

static void *default_malloc (size_t size, void *user_data ALLOC_UNUSED) {
  return malloc (size);
}

static void *default_calloc (size_t num, size_t size, void *user_data ALLOC_UNUSED) {
  return calloc (num, size);
}

static void *default_realloc (void *ptr, size_t old_size ALLOC_UNUSED, size_t new_size, void *user_data ALLOC_UNUSED) {
  return realloc (ptr, new_size);
}

static void default_free (void *ptr, void *user_data ALLOC_UNUSED) {
  free (ptr);
}

static struct MIR_alloc default_alloc = {
  .malloc = default_malloc,
  .calloc = default_calloc,
  .realloc = default_realloc,
  .free = default_free,
  .user_data = NULL
};
