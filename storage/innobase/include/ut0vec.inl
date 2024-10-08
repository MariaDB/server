/*****************************************************************************

Copyright (c) 2006, 2014, Oracle and/or its affiliates. All Rights Reserved.

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

/*******************************************************************//**
@file include/ut0vec.ic
A vector of pointers to data items

Created 4/6/2006 Osku Salerma
************************************************************************/

#define	IB_VEC_OFFSET(v, i)	(vec->sizeof_value * i)

/********************************************************************
The default ib_vector_t heap malloc. Uses mem_heap_alloc(). */
UNIV_INLINE
void*
ib_heap_malloc(
/*===========*/
	ib_alloc_t*	allocator,	/* in: allocator */
	ulint		size)		/* in: size in bytes */
{
	mem_heap_t*	heap = (mem_heap_t*) allocator->arg;

	return(mem_heap_alloc(heap, size));
}

/********************************************************************
The default ib_vector_t heap free. Does nothing. */
UNIV_INLINE
void
ib_heap_free(
/*=========*/
	ib_alloc_t*	allocator UNIV_UNUSED,	/* in: allocator */
	void*		ptr UNIV_UNUSED)	/* in: size in bytes */
{
	/* We can't free individual elements. */
}

/********************************************************************
The default ib_vector_t heap resize. Since we can't resize the heap
we have to copy the elements from the old ptr to the new ptr.
We always assume new_size >= old_size, so the buffer won't overflow.
Uses mem_heap_alloc(). */
UNIV_INLINE
void*
ib_heap_resize(
/*===========*/
	ib_alloc_t*	allocator,	/* in: allocator */
	void*		old_ptr,	/* in: pointer to memory */
	ulint		old_size,	/* in: old size in bytes */
	ulint		new_size)	/* in: new size in bytes */
{
	void*		new_ptr;
	mem_heap_t*	heap = (mem_heap_t*) allocator->arg;

	ut_a(new_size >= old_size);
	new_ptr = mem_heap_alloc(heap, new_size);
	memcpy(new_ptr, old_ptr, old_size);

	return(new_ptr);
}

/********************************************************************
Create a heap allocator that uses the passed in heap. */
UNIV_INLINE
ib_alloc_t*
ib_heap_allocator_create(
/*=====================*/
	mem_heap_t*	heap)		/* in: heap to use */
{
	ib_alloc_t*	heap_alloc;

	heap_alloc = (ib_alloc_t*) mem_heap_alloc(heap, sizeof(*heap_alloc));

	heap_alloc->arg = heap;
	heap_alloc->mem_release = ib_heap_free;
	heap_alloc->mem_malloc = ib_heap_malloc;
	heap_alloc->mem_resize = ib_heap_resize;

	return(heap_alloc);
}

/********************************************************************
Free a heap allocator. */
UNIV_INLINE
void
ib_heap_allocator_free(
/*===================*/
	ib_alloc_t*	ib_ut_alloc)	/* in: alloc instace to free */
{
	mem_heap_free((mem_heap_t*) ib_ut_alloc->arg);
}

/********************************************************************
Get number of elements in vector. */
UNIV_INLINE
ulint
ib_vector_size(
/*===========*/
					/* out: number of elements in vector*/
	const ib_vector_t*	vec)	/* in: vector */
{
	return(vec->used);
}

/****************************************************************//**
Get n'th element. */
UNIV_INLINE
void*
ib_vector_get(
/*==========*/
	ib_vector_t*	vec,	/*!< in: vector */
	ulint		n)	/*!< in: element index to get */
{
	ut_a(n < vec->used);

	return((byte*) vec->data + IB_VEC_OFFSET(vec, n));
}

/********************************************************************
Const version of the get n'th element.
@return n'th element */
UNIV_INLINE
const void*
ib_vector_get_const(
/*================*/
	const ib_vector_t*	vec,	/* in: vector */
	ulint			n)	/* in: element index to get */
{
	ut_a(n < vec->used);

	return((byte*) vec->data + IB_VEC_OFFSET(vec, n));
}
/****************************************************************//**
Get last element. The vector must not be empty.
@return last element */
UNIV_INLINE
void*
ib_vector_get_last(
/*===============*/
	ib_vector_t*	vec)	/*!< in: vector */
{
	ut_a(vec->used > 0);

	return((byte*) ib_vector_get(vec, vec->used - 1));
}

/****************************************************************//**
Set the n'th element. */
UNIV_INLINE
void
ib_vector_set(
/*==========*/
	ib_vector_t*	vec,	/*!< in/out: vector */
	ulint		n,	/*!< in: element index to set */
	void*		elem)	/*!< in: data element */
{
	void*		slot;

	ut_a(n < vec->used);

	slot = ((byte*) vec->data + IB_VEC_OFFSET(vec, n));
	memcpy(slot, elem, vec->sizeof_value);
}

/********************************************************************
Reset the vector size to 0 elements. */
UNIV_INLINE
void
ib_vector_reset(
/*============*/
					/* out: void */
	ib_vector_t*	vec)		/* in: vector */
{
	vec->used = 0;
}

/********************************************************************
Get the last element of the vector. */
UNIV_INLINE
void*
ib_vector_last(
/*===========*/
					/* out: void */
	ib_vector_t*	vec)		/* in: vector */
{
	ut_a(ib_vector_size(vec) > 0);

	return(ib_vector_get(vec, ib_vector_size(vec) - 1));
}

/********************************************************************
Get the last element of the vector. */
UNIV_INLINE
const void*
ib_vector_last_const(
/*=================*/
					/* out: void */
	const ib_vector_t*	vec)	/* in: vector */
{
	ut_a(ib_vector_size(vec) > 0);

	return(ib_vector_get_const(vec, ib_vector_size(vec) - 1));
}

/****************************************************************//**
Remove the last element from the vector.
@return last vector element */
UNIV_INLINE
void*
ib_vector_pop(
/*==========*/
				/* out: pointer to element */
	ib_vector_t*	vec)	/* in: vector */
{
	void*		elem;

	ut_a(vec->used > 0);

	elem = ib_vector_last(vec);
	--vec->used;

	return(elem);
}

/********************************************************************
Append an element to the vector, if elem != NULL then copy the data
from elem.*/
UNIV_INLINE
void*
ib_vector_push(
/*===========*/
				/* out: pointer to the "new" element */
	ib_vector_t*	vec,	/* in: vector */
	const void*	elem)	/* in: element to add (can be NULL) */
{
	void*		last;

	if (vec->used >= vec->total) {
		ib_vector_resize(vec);
	}

	last = (byte*) vec->data + IB_VEC_OFFSET(vec, vec->used);

#ifdef UNIV_DEBUG
	memset(last, 0, vec->sizeof_value);
#endif

	if (elem) {
		memcpy(last, elem, vec->sizeof_value);
	}

	++vec->used;

	return(last);
}

/*******************************************************************//**
Remove an element to the vector
@return pointer to the "removed" element */
UNIV_INLINE
void*
ib_vector_remove(
/*=============*/
	ib_vector_t*	vec,	/*!< in: vector */
	const void*	elem)	/*!< in: value to remove */
{
	void*		current = NULL;
	void*		next;
	ulint		i;
	ulint		old_used_count = vec->used;

	for (i = 0; i < vec->used; i++) {
		current = ib_vector_get(vec, i);

		if (*(void**) current == elem) {
			if (i == vec->used - 1) {
				return(ib_vector_pop(vec));
			}

			next = ib_vector_get(vec, i + 1);
			memmove(current, next, vec->sizeof_value
			        * (vec->used - i - 1));
			--vec->used;
			break;
		}
	}

	return((old_used_count != vec->used) ? current : NULL);
}

/********************************************************************
Destroy the vector. Make sure the vector owns the allocator, e.g.,
the heap in the the heap allocator. */
UNIV_INLINE
void
ib_vector_free(
/*===========*/
	ib_vector_t*	vec)		/* in, own: vector */
{
	/* Currently we only support one type of allocator - heap,
	when the heap is freed all the elements are freed too. */

	/* Only the heap allocator uses the arg field. */
	ut_ad(vec->allocator->arg != NULL);

	mem_heap_free((mem_heap_t*) vec->allocator->arg);
}

/********************************************************************
Test whether a vector is empty or not.
@return TRUE if empty */
UNIV_INLINE
ibool
ib_vector_is_empty(
/*===============*/
	const ib_vector_t*	vec)	/*!< in: vector */
{
	return(ib_vector_size(vec) == 0);
}
