/**
 *
 * Copyright (C) TidesDB
 *
 * Original Author: Alex Gaetano Padula
 *
 * Licensed under the Mozilla Public License, v. 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.mozilla.org/en-US/MPL/2.0/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __ALLOC_H__
#define __ALLOC_H__

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/**
 * tidesdb_malloc_fn
 * function pointer type for malloc-like allocation
 * @param size number of bytes to allocate
 * @return pointer to allocated memory or NULL on failure
 */
typedef void *(*tidesdb_malloc_fn)(size_t size);

/**
 * tidesdb_calloc_fn
 * function pointer type for calloc-like allocation
 * @param count number of elements to allocate
 * @param size size of each element in bytes
 * @return pointer to zero-initialized memory or NULL on failure
 */
typedef void *(*tidesdb_calloc_fn)(size_t count, size_t size);

/**
 * tidesdb_realloc_fn
 * function pointer type for realloc-like reallocation
 * @param ptr pointer to previously allocated memory (or NULL)
 * @param size new size in bytes
 * @return pointer to reallocated memory or NULL on failure
 */
typedef void *(*tidesdb_realloc_fn)(void *ptr, size_t size);

/**
 * tidesdb_free_fn
 * function pointer type for free-like deallocation
 * @param ptr pointer to memory to free (may be NULL)
 */
typedef void (*tidesdb_free_fn)(void *ptr);

/**
 * tidesdb_allocator_t
 * holds the allocator function pointers
 * @param malloc_fn malloc function pointer
 * @param calloc_fn calloc function pointer
 * @param realloc_fn realloc function pointer
 * @param free_fn free function pointer
 */
typedef struct tidesdb_allocator_t
{
    tidesdb_malloc_fn malloc_fn;
    tidesdb_calloc_fn calloc_fn;
    tidesdb_realloc_fn realloc_fn;
    tidesdb_free_fn free_fn;
} tidesdb_allocator_t;

extern tidesdb_allocator_t tidesdb_allocator;
extern _Atomic(int) tidesdb_initialized;

/**
 * tidesdb_init
 * initializes TidesDB with optional custom memory allocation functions
 * must be called exactly once before any other TidesDB function
 * pass NULL for any function to use the default system allocator
 *
 * @param malloc_fn custom malloc function (or NULL for system malloc)
 * @param calloc_fn custom calloc function (or NULL for system calloc)
 * @param realloc_fn custom realloc function (or NULL for system realloc)
 * @param free_fn custom free function (or NULL for system free)
 * @return 0 on success, -1 if already initialized
 */
int tidesdb_init(tidesdb_malloc_fn malloc_fn, tidesdb_calloc_fn calloc_fn,
                 tidesdb_realloc_fn realloc_fn, tidesdb_free_fn free_fn);

/**
 * tidesdb_finalize
 * finalizes TidesDB and resets the allocator
 * should be called after all TidesDB operations are complete
 * after calling this, tidesdb_init() can be called again
 */
void tidesdb_finalize(void);

/**
 * tidesdb_ensure_initialized
 * internal function to auto-initialize with system allocator if not initialized
 * called automatically by TidesDB methods
 */
void tidesdb_ensure_initialized(void);

/* allocation macros that use the configured allocator */
#define tdb_malloc(size)        (tidesdb_allocator.malloc_fn(size))
#define tdb_calloc(count, size) (tidesdb_allocator.calloc_fn((count), (size)))
#define tdb_realloc(ptr, size)  (tidesdb_allocator.realloc_fn((ptr), (size)))
#define tdb_free(ptr)           (tidesdb_allocator.free_fn(ptr))

/**
 * override standard allocation functions.
 * this allows existing code using malloc/calloc/realloc/free to automatically
 */
#undef malloc
#undef calloc
#undef realloc
#undef free
#define malloc(size)        tdb_malloc(size)
#define calloc(count, size) tdb_calloc((count), (size))
#define realloc(ptr, size)  tdb_realloc((ptr), (size))
#define free(ptr)           tdb_free(ptr)

/**
 * tdb_strdup
 * custom allocator-aware string duplication
 * uses malloc (which is redirected to tdb_malloc above) so that the
 * returned pointer can safely be freed via the custom allocator's free
 * @param s the string to duplicate
 * @return newly allocated copy of s, or NULL on failure
 */
static inline char *tdb_strdup(const char *s)
{
    if (!s) return NULL;
    const size_t len = strlen(s) + 1;
    char *dup = (char *)malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

#endif /* __ALLOC_H__ */
