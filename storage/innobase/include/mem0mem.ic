/*****************************************************************************

Copyright (c) 1994, 2014, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/********************************************************************//**
@file include/mem0mem.ic
The memory management

Created 6/8/1994 Heikki Tuuri
*************************************************************************/

#include "ut0new.h"

#ifdef UNIV_DEBUG
# define mem_heap_create_block(heap, n, type, file_name, line)		\
	mem_heap_create_block_func(heap, n, file_name, line, type)
# define mem_heap_create_at(N, file_name, line)				\
	mem_heap_create_func(N, file_name, line, MEM_HEAP_DYNAMIC)
#else /* UNIV_DEBUG */
# define mem_heap_create_block(heap, n, type, file_name, line)		\
	mem_heap_create_block_func(heap, n, type)
# define mem_heap_create_at(N, file_name, line)				\
	mem_heap_create_func(N, MEM_HEAP_DYNAMIC)
#endif /* UNIV_DEBUG */
/***************************************************************//**
Creates a memory heap block where data can be allocated.
@return own: memory heap block, NULL if did not succeed (only possible
for MEM_HEAP_BTR_SEARCH type heaps) */
mem_block_t*
mem_heap_create_block_func(
/*=======================*/
	mem_heap_t*	heap,	/*!< in: memory heap or NULL if first block
				should be created */
	ulint		n,	/*!< in: number of bytes needed for user data */
#ifdef UNIV_DEBUG
	const char*	file_name,/*!< in: file name where created */
	unsigned	line,	/*!< in: line where created */
#endif /* UNIV_DEBUG */
	ulint		type);	/*!< in: type of heap: MEM_HEAP_DYNAMIC or
				MEM_HEAP_BUFFER */

/******************************************************************//**
Frees a block from a memory heap. */
void
mem_heap_block_free(
/*================*/
	mem_heap_t*	heap,	/*!< in: heap */
	mem_block_t*	block);	/*!< in: block to free */

/******************************************************************//**
Frees the free_block field from a memory heap. */
void
mem_heap_free_block_free(
/*=====================*/
	mem_heap_t*	heap);	/*!< in: heap */

/***************************************************************//**
Adds a new block to a memory heap.
@param[in]	heap	memory heap
@param[in]	n	number of bytes needed
@return created block, NULL if did not succeed (only possible for
MEM_HEAP_BTR_SEARCH type heaps) */
mem_block_t*
mem_heap_add_block(
	mem_heap_t*	heap,
	ulint		n);

UNIV_INLINE
void
mem_block_set_len(mem_block_t* block, ulint len)
{
	ut_ad(len > 0);

	block->len = len;
}

UNIV_INLINE
ulint
mem_block_get_len(mem_block_t* block)
{
	return(block->len);
}

UNIV_INLINE
void
mem_block_set_type(mem_block_t* block, ulint type)
{
	ut_ad((type == MEM_HEAP_DYNAMIC) || (type == MEM_HEAP_BUFFER)
	      || (type == MEM_HEAP_BUFFER + MEM_HEAP_BTR_SEARCH));

	block->type = type;
}

UNIV_INLINE
ulint
mem_block_get_type(mem_block_t* block)
{
	return(block->type);
}

UNIV_INLINE
void
mem_block_set_free(mem_block_t* block, ulint free)
{
	ut_ad(free > 0);
	ut_ad(free <= mem_block_get_len(block));

	block->free = free;
}

UNIV_INLINE
ulint
mem_block_get_free(mem_block_t* block)
{
	return(block->free);
}

UNIV_INLINE
void
mem_block_set_start(mem_block_t* block, ulint start)
{
	ut_ad(start > 0);

	block->start = start;
}

UNIV_INLINE
ulint
mem_block_get_start(mem_block_t* block)
{
	return(block->start);
}

/** Allocates and zero-fills n bytes of memory from a memory heap.
@param[in]	heap	memory heap
@param[in]	n	number of bytes; if the heap is allowed to grow into
the buffer pool, this must be <= MEM_MAX_ALLOC_IN_BUF
@return allocated, zero-filled storage */
UNIV_INLINE
void*
mem_heap_zalloc(
	mem_heap_t*	heap,
	ulint		n)
{
	ut_ad(heap);
	ut_ad(!(heap->type & MEM_HEAP_BTR_SEARCH));
	return(memset(mem_heap_alloc(heap, n), 0, n));
}

/** Allocates n bytes of memory from a memory heap.
@param[in]	heap	memory heap
@param[in]	n	number of bytes; if the heap is allowed to grow into
the buffer pool, this must be <= MEM_MAX_ALLOC_IN_BUF
@return allocated storage, NULL if did not succeed (only possible for
MEM_HEAP_BTR_SEARCH type heaps) */
UNIV_INLINE
void*
mem_heap_alloc(
	mem_heap_t*	heap,
	ulint		n)
{
	mem_block_t*	block;
	byte*		buf;
	ulint		free;

	block = UT_LIST_GET_LAST(heap->base);

	n += REDZONE_SIZE;

	ut_ad(!(block->type & MEM_HEAP_BUFFER) || (n <= MEM_MAX_ALLOC_IN_BUF));

	/* Check if there is enough space in block. If not, create a new
	block to the heap */

	if (mem_block_get_len(block)
	    < mem_block_get_free(block) + MEM_SPACE_NEEDED(n)) {

		block = mem_heap_add_block(heap, n);

		if (block == NULL) {

			return(NULL);
		}
	}

	free = mem_block_get_free(block);

	buf = (byte*) block + free;

	mem_block_set_free(block, free + MEM_SPACE_NEEDED(n));

	buf = buf + REDZONE_SIZE;
	MEM_MAKE_ADDRESSABLE(buf, n - REDZONE_SIZE);
	return(buf);
}

/** Returns a pointer to the heap top.
@param[in]	heap	memory heap
@return pointer to the heap top */
UNIV_INLINE
byte*
mem_heap_get_heap_top(
	mem_heap_t*	heap)
{
	mem_block_t*	block;
	byte*		buf;

	block = UT_LIST_GET_LAST(heap->base);

	buf = (byte*) block + mem_block_get_free(block);

	return(buf);
}

/** Frees the space in a memory heap exceeding the pointer given.
The pointer must have been acquired from mem_heap_get_heap_top.
The first memory block of the heap is not freed.
@param[in]	heap		heap from which to free
@param[in]	old_top		pointer to old top of heap */
UNIV_INLINE
void
mem_heap_free_heap_top(
	mem_heap_t*	heap,
	byte*		old_top)
{
	mem_block_t*	block;
	mem_block_t*	prev_block;

	ut_d(mem_heap_validate(heap));

	block = UT_LIST_GET_LAST(heap->base);

	while (block != NULL) {
		if (((byte*) block + mem_block_get_free(block) >= old_top)
		    && ((byte*) block <= old_top)) {
			/* Found the right block */

			break;
		}

		/* Store prev_block value before freeing the current block
		(the current block will be erased in freeing) */

		prev_block = UT_LIST_GET_PREV(list, block);

		mem_heap_block_free(heap, block);

		block = prev_block;
	}

	ut_ad(block);

	/* Set the free field of block */
	mem_block_set_free(block,
			   ulint(old_top - reinterpret_cast<byte*>(block)));

	ut_ad(mem_block_get_start(block) <= mem_block_get_free(block));
	MEM_NOACCESS(old_top, (byte*) block + block->len - old_top);

	/* If free == start, we may free the block if it is not the first
	one */

	if ((heap != block) && (mem_block_get_free(block)
				== mem_block_get_start(block))) {
		mem_heap_block_free(heap, block);
	}
}

/** Empties a memory heap.
The first memory block of the heap is not freed.
@param[in]	heap	heap to empty */
UNIV_INLINE
void
mem_heap_empty(
	mem_heap_t*	heap)
{
	mem_heap_free_heap_top(heap, (byte*) heap + mem_block_get_start(heap));

	if (heap->free_block) {
		mem_heap_free_block_free(heap);
	}
}

/** Returns a pointer to the topmost element in a memory heap.
The size of the element must be given.
@param[in]	heap	memory heap
@param[in]	n	size of the topmost element
@return pointer to the topmost element */
UNIV_INLINE
void*
mem_heap_get_top(
	mem_heap_t*	heap,
	ulint		n)
{
	mem_block_t*	block;
	byte*		buf;

	block = UT_LIST_GET_LAST(heap->base);

	buf = (byte*) block + mem_block_get_free(block) - MEM_SPACE_NEEDED(n);

	return((void*) buf);
}

/*****************************************************************//**
Frees the topmost element in a memory heap. The size of the element must be
given. */
UNIV_INLINE
void
mem_heap_free_top(
/*==============*/
	mem_heap_t*	heap,	/*!< in: memory heap */
	ulint		n)	/*!< in: size of the topmost element */
{
	mem_block_t*	block;

	n += REDZONE_SIZE;

	block = UT_LIST_GET_LAST(heap->base);

	/* Subtract the free field of block */
	mem_block_set_free(block, mem_block_get_free(block)
			   - MEM_SPACE_NEEDED(n));

	/* If free == start, we may free the block if it is not the first
	one */

	if ((heap != block) && (mem_block_get_free(block)
				== mem_block_get_start(block))) {
		mem_heap_block_free(heap, block);
	} else {
		MEM_NOACCESS((byte*) block + mem_block_get_free(block), n);
	}
}

/** Creates a memory heap.
NOTE: Use the corresponding macros instead of this function.
A single user buffer of 'size' will fit in the block.
0 creates a default size block.
@param[in]	size		Desired start block size.
@param[in]	file_name	File name where created
@param[in]	line		Line where created
@param[in]	type		Heap type
@return own: memory heap, NULL if did not succeed (only possible for
MEM_HEAP_BTR_SEARCH type heaps) */
UNIV_INLINE
mem_heap_t*
mem_heap_create_func(
	ulint		size,
#ifdef UNIV_DEBUG
	const char*	file_name,
	unsigned	line,
#endif /* UNIV_DEBUG */
	ulint		type)
{
	mem_block_t*   block;

	if (!size) {
		size = MEM_BLOCK_START_SIZE;
	}

	block = mem_heap_create_block(NULL, size, type, file_name, line);

	if (block == NULL) {

		return(NULL);
	}

	/* The first block should not be in buffer pool,
	because it might be relocated to resize buffer pool. */
	ut_ad(block->buf_block == NULL);

	UT_LIST_INIT(block->base, &mem_block_t::list);

	/* Add the created block itself as the first block in the list */
	UT_LIST_ADD_FIRST(block->base, block);

	return(block);
}

/** Frees the space occupied by a memory heap.
NOTE: Use the corresponding macro instead of this function.
@param[in]	heap	Heap to be freed */
UNIV_INLINE
void
mem_heap_free(
	mem_heap_t*	heap)
{
	mem_block_t*	block;
	mem_block_t*	prev_block;

	block = UT_LIST_GET_LAST(heap->base);

	if (heap->free_block) {
		mem_heap_free_block_free(heap);
	}

	while (block != NULL) {
		/* Store the contents of info before freeing current block
		(it is erased in freeing) */

		prev_block = UT_LIST_GET_PREV(list, block);

		mem_heap_block_free(heap, block);

		block = prev_block;
	}
}

/*****************************************************************//**
Returns the space in bytes occupied by a memory heap. */
UNIV_INLINE
ulint
mem_heap_get_size(
/*==============*/
	mem_heap_t*	heap)	/*!< in: heap */
{
	ulint size = heap->total_size;

	if (heap->free_block) {
		size += srv_page_size;
	}

	return(size);
}

/**********************************************************************//**
Duplicates a NUL-terminated string.
@return own: a copy of the string, must be deallocated with ut_free */
UNIV_INLINE
char*
mem_strdup(
/*=======*/
	const char*	str)	/*!< in: string to be copied */
{
	ulint	len = strlen(str) + 1;
	return(static_cast<char*>(memcpy(ut_malloc_nokey(len), str, len)));
}

/**********************************************************************//**
Makes a NUL-terminated copy of a nonterminated string.
@return own: a copy of the string, must be deallocated with ut_free */
UNIV_INLINE
char*
mem_strdupl(
/*========*/
	const char*	str,	/*!< in: string to be copied */
	ulint		len)	/*!< in: length of str, in bytes */
{
	char*	s = static_cast<char*>(ut_malloc_nokey(len + 1));
	s[len] = 0;
	return(static_cast<char*>(memcpy(s, str, len)));
}
