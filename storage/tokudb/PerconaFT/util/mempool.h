/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#pragma once

/* a memory pool is a contiguous region of memory that supports single
   allocations from the pool.  these allocated regions are never recycled.
   when the memory pool no longer has free space, the allocated chunks
   must be relocated by the application to a new memory pool. */

#include <stddef.h>

struct mempool;

  // TODO 4050 Hide mempool struct internals from callers

struct mempool {
    void *base;           /* the base address of the memory */
    size_t free_offset;      /* the offset of the memory pool free space */
    size_t size;             /* the size of the memory */
    size_t frag_size;        /* the size of the fragmented memory */
};

/* This is a constructor to be used when the memory for the mempool struct has been
 * allocated by the caller, but no memory has yet been allocatd for the data.
 */
void toku_mempool_zero(struct mempool *mp);

/* initialize the memory pool with the base address and size of a
   contiguous chunk of memory */
void toku_mempool_init(struct mempool *mp, void *base, size_t free_offset, size_t size);

/* allocate memory and construct mempool
 */
void toku_mempool_construct(struct mempool *mp, size_t data_size);

/* treat mempool as if it has just been created; ignore any frag and start allocating from beginning again.
 */
void toku_mempool_reset(struct mempool *mp);

/* reallocate memory for construct mempool
 */
void toku_mempool_realloc_larger(struct mempool *mp, size_t data_size);

/* destroy the memory pool */
void toku_mempool_destroy(struct mempool *mp);

/* get the base address of the memory pool */
void *toku_mempool_get_base(const struct mempool *mp);

/* get the a pointer that is offset bytes in front of base of the memory pool */
void *toku_mempool_get_pointer_from_base_and_offset(const struct mempool *mp, size_t offset);

/* get the offset from base of a pointer */
size_t toku_mempool_get_offset_from_pointer_and_base(const struct mempool *mp, const void* p);

/* get the a pointer of the first free byte (if any) */
void* toku_mempool_get_next_free_ptr(const struct mempool *mp);

/* get the limit of valid offsets.  (anything later was not allocated) */
size_t toku_mempool_get_offset_limit(const struct mempool *mp);

/* get the size of the memory pool */
size_t toku_mempool_get_size(const struct mempool *mp);

/* get the amount of fragmented (wasted) space in the memory pool */
size_t toku_mempool_get_frag_size(const struct mempool *mp);

/* get the amount of space that is holding useful data */
size_t toku_mempool_get_used_size(const struct mempool *mp);

/* get the amount of space that is available for new data */
size_t toku_mempool_get_free_size(const struct mempool *mp);

/* get the amount of space that has been allocated for use (wasted or not) */
size_t toku_mempool_get_allocated_size(const struct mempool *mp);

/* allocate a chunk of memory from the memory pool */
void *toku_mempool_malloc(struct mempool *mp, size_t size);

/* free a previously allocated chunk of memory.  the free only updates
   a count of the amount of free space in the memory pool.  the memory
   pool does not keep track of the locations of the free chunks */
void toku_mempool_mfree(struct mempool *mp, void *vp, size_t size);

/* verify that a memory range is contained within a mempool */
static inline int toku_mempool_inrange(struct mempool *mp, void *vp, size_t size) {
    return (mp->base <= vp) && ((char *)vp + size <= (char *)mp->base + mp->size);
}

/* get memory footprint */
size_t toku_mempool_footprint(struct mempool *mp);

void toku_mempool_clone(const struct mempool* orig_mp, struct mempool* new_mp);
