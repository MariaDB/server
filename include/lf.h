/* Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef INCLUDE_LF_INCLUDED
#define INCLUDE_LF_INCLUDED

#include <my_atomic.h>

C_MODE_START

/*
  wait-free dynamic array, see lf_dynarray.c

  4 levels of 256 elements each mean 4311810304 elements in an array - it
  should be enough for a while
*/
#define LF_DYNARRAY_LEVEL_LENGTH 256
#define LF_DYNARRAY_LEVELS       4

typedef struct {
  void * volatile level[LF_DYNARRAY_LEVELS];
  uint size_of_element;
} LF_DYNARRAY;

typedef int (*lf_dynarray_func)(void *, void *);

void lf_dynarray_init(LF_DYNARRAY *array, uint element_size);
void lf_dynarray_destroy(LF_DYNARRAY *array);

void *lf_dynarray_value(LF_DYNARRAY *array, uint idx);
void *lf_dynarray_lvalue(LF_DYNARRAY *array, uint idx);
int lf_dynarray_iterate(LF_DYNARRAY *array, lf_dynarray_func func, void *arg);

/*
  pin manager for memory allocator, lf_alloc-pin.c
*/

#define LF_PINBOX_PINS 4
#define LF_PURGATORY_SIZE 100

typedef void lf_pinbox_free_func(void *, void *, void*);

typedef struct {
  LF_DYNARRAY pinarray;
  lf_pinbox_free_func *free_func;
  void *free_func_arg;
  uint free_ptr_offset;
  uint32 volatile pinstack_top_ver;         /* this is a versioned pointer */
  uint32 volatile pins_in_array;            /* number of elements in array */
} LF_PINBOX;

typedef struct {
  void * volatile pin[LF_PINBOX_PINS];
  LF_PINBOX *pinbox;
  void  *purgatory;
  uint32 purgatory_count;
  uint32 volatile link;
  /* avoid false sharing */
  char pad[CPU_LEVEL1_DCACHE_LINESIZE];
} LF_PINS;

/* compile-time assert to make sure we have enough pins.  */
#define lf_pin(PINS, PIN, ADDR)                                \
  do {                                                          \
    compile_time_assert(PIN < LF_PINBOX_PINS);                  \
    my_atomic_storeptr(&(PINS)->pin[PIN], (ADDR));              \
  } while(0)

#define lf_unpin(PINS, PIN)        lf_pin(PINS, PIN, NULL)
#define lf_assert_pin(PINS, PIN)   assert((PINS)->pin[PIN] != 0)
#define lf_assert_unpin(PINS, PIN) assert((PINS)->pin[PIN] == 0)

void lf_pinbox_init(LF_PINBOX *pinbox, uint free_ptr_offset,
                    lf_pinbox_free_func *free_func, void * free_func_arg);
void lf_pinbox_destroy(LF_PINBOX *pinbox);

LF_PINS *lf_pinbox_get_pins(LF_PINBOX *pinbox);
void lf_pinbox_put_pins(LF_PINS *pins);
void lf_pinbox_free(LF_PINS *pins, void *addr);

/*
  memory allocator, lf_alloc-pin.c
*/

typedef struct st_lf_allocator {
  LF_PINBOX pinbox;
  uchar * volatile top;
  uint element_size;
  uint32 volatile mallocs;
  void (*constructor)(uchar *); /* called, when an object is malloc()'ed */
  void (*destructor)(uchar *);  /* called, when an object is free()'d    */
} LF_ALLOCATOR;

void lf_alloc_init(LF_ALLOCATOR *allocator, uint size, uint free_ptr_offset);
void lf_alloc_destroy(LF_ALLOCATOR *allocator);
uint lf_alloc_pool_count(LF_ALLOCATOR *allocator);
/*
  shortcut macros to access underlying pinbox functions from an LF_ALLOCATOR
  see lf_pinbox_get_pins() and lf_pinbox_put_pins()
*/
#define lf_alloc_free(PINS, PTR)       lf_pinbox_free((PINS), (PTR))
#define lf_alloc_get_pins(A)           lf_pinbox_get_pins(&(A)->pinbox)
#define lf_alloc_put_pins(PINS)        lf_pinbox_put_pins(PINS)
#define lf_alloc_direct_free(ALLOC, ADDR) \
  do {                                    \
    if ((ALLOC)->destructor)              \
      (ALLOC)->destructor((uchar*) ADDR); \
    my_free(ADDR);                        \
  } while(0)

void *lf_alloc_new(LF_PINS *pins);

C_MODE_END

/*
  extendible hash, lf_hash.cc
*/
#include <hash.h>

C_MODE_START

typedef struct st_lf_hash LF_HASH;
typedef void (*lf_hash_initializer)(LF_HASH *hash, void *dst, const void *src);

#define LF_HASH_UNIQUE 1

/* lf_hash overhead per element (that is, sizeof(LF_SLIST) */
extern const int LF_HASH_OVERHEAD;

struct st_lf_hash {
  LF_DYNARRAY array;                    /* hash itself */
  LF_ALLOCATOR alloc;                   /* allocator for elements */
  my_hash_get_key get_key;              /* see HASH */
  lf_hash_initializer initializer;      /* called when an element is inserted */
  my_hash_function hash_function;       /* see HASH */
  CHARSET_INFO *charset;                /* see HASH */
  uint key_offset, key_length;          /* see HASH */
  uint element_size;                    /* size of memcpy'ed area on insert */
  uint flags;                           /* LF_HASH_UNIQUE, etc */
  int32 volatile size;                  /* size of array */
  int32 volatile count;                 /* number of elements in the hash */
};

void lf_hash_init(LF_HASH *hash, uint element_size, uint flags,
                  uint key_offset, uint key_length, my_hash_get_key get_key,
                  CHARSET_INFO *charset);
void lf_hash_destroy(LF_HASH *hash);
int lf_hash_insert(LF_HASH *hash, LF_PINS *pins, const void *data);
void *lf_hash_search(LF_HASH *hash, LF_PINS *pins, const void *key, uint keylen);
void *lf_hash_search_using_hash_value(LF_HASH *hash, LF_PINS *pins,
                                      my_hash_value_type hash_value,
                                      const void *key, uint keylen);
int lf_hash_delete(LF_HASH *hash, LF_PINS *pins, const void *key, uint keylen);
int lf_hash_iterate(LF_HASH *hash, LF_PINS *pins,
                    my_hash_walk_action action, void *argument);
#define lf_hash_size(hash) \
  my_atomic_load32_explicit(&(hash)->count, MY_MEMORY_ORDER_RELAXED)
/*
  shortcut macros to access underlying pinbox functions from an LF_HASH
  see lf_pinbox_get_pins() and lf_pinbox_put_pins()
*/
#define lf_hash_get_pins(HASH)       lf_alloc_get_pins(&(HASH)->alloc)
#define lf_hash_put_pins(PINS)       lf_pinbox_put_pins(PINS)
#define lf_hash_search_unpin(PINS)   lf_unpin((PINS), 2)
/*
  cleanup
*/

C_MODE_END

#endif

