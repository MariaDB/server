/*****************************************************************************

Copyright (c) 2014, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2021, MariaDB Corporation.

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

/**************************************************//**
@file ut/ut0new.h
Instrumented memory allocator.

Created May 26, 2014 Vasil Dimov
*******************************************************/

/** Dynamic memory allocation within InnoDB guidelines.
All dynamic (heap) memory allocations (malloc(3), strdup(3), etc, "new",
various std:: containers that allocate memory internally), that are done
within InnoDB are instrumented. This means that InnoDB uses a custom set
of functions for allocating memory, rather than calling e.g. "new" directly.

Here follows a cheat sheet on what InnoDB functions to use whenever a
standard one would have been used.

Creating new objects with "new":
--------------------------------
Standard:
  new expression
  or
  new(std::nothrow) expression
InnoDB, default instrumentation:
  UT_NEW_NOKEY(expression)
InnoDB, custom instrumentation, preferred:
  UT_NEW(expression, key)

Destroying objects, created with "new":
---------------------------------------
Standard:
  delete ptr
InnoDB:
  UT_DELETE(ptr)

Creating new arrays with "new[]":
---------------------------------
Standard:
  new type[num]
  or
  new(std::nothrow) type[num]
InnoDB, default instrumentation:
  UT_NEW_ARRAY_NOKEY(type, num)
InnoDB, custom instrumentation, preferred:
  UT_NEW_ARRAY(type, num, key)

Destroying arrays, created with "new[]":
----------------------------------------
Standard:
  delete[] ptr
InnoDB:
  UT_DELETE_ARRAY(ptr)

Declaring a type with a std:: container, e.g. std::vector:
----------------------------------------------------------
Standard:
  std::vector<t>
InnoDB:
  std::vector<t, ut_allocator<t> >

Declaring objects of some std:: type:
-------------------------------------
Standard:
  std::vector<t> v
InnoDB, default instrumentation:
  std::vector<t, ut_allocator<t> > v
InnoDB, custom instrumentation, preferred:
  std::vector<t, ut_allocator<t> > v(ut_allocator<t>(key))

Raw block allocation (as usual in C++, consider whether using "new" would
not be more appropriate):
-------------------------------------------------------------------------
Standard:
  malloc(num)
InnoDB, default instrumentation:
  ut_malloc_nokey(num)
InnoDB, custom instrumentation, preferred:
  ut_malloc(num, key)

Raw block resize:
-----------------
Standard:
  realloc(ptr, new_size)
InnoDB:
  ut_realloc(ptr, new_size)

Raw block deallocation:
-----------------------
Standard:
  free(ptr)
InnoDB:
  ut_free(ptr)

Note: the expression passed to UT_NEW() or UT_NEW_NOKEY() must always end
with (), thus:
Standard:
  new int
InnoDB:
  UT_NEW_NOKEY(int())
*/

#ifndef ut0new_h
#define ut0new_h

#include <limits> /* std::numeric_limits */
#include <thread>

#include <stddef.h>
#include <stdlib.h> /* malloc() */
#include <string.h> /* strlen(), strrchr(), strncmp() */

#include <my_sys.h> /* my_large_free/malloc() */

#include "my_global.h" /* needed for headers from mysql/psi/ */

#include "mysql/psi/mysql_memory.h" /* PSI_MEMORY_CALL() */

#include "mysql/psi/psi_memory.h" /* PSI_memory_key, PSI_memory_info */

#include "ut0ut.h" /* ut_strcmp_functor */

#define	OUT_OF_MEMORY_MSG \
	"Check if you should increase the swap file or ulimits of your" \
	" operating system. Note that on most 32-bit computers the process" \
	" memory space is limited to 2 GB or 4 GB."

/** The total amount of memory currently allocated from the operating
system with allocate_large() */
extern Atomic_counter<ulint> os_total_large_mem_allocated;

/** Maximum number of retries to allocate memory. */
extern const size_t	alloc_max_retries;

constexpr uint32_t INVALID_AUTOEVENT_IDX = 0xFFFFFFFFU;

/** Keys for registering allocations with performance schema.
Pointers to these variables are supplied to PFS code via the pfs_info[]
array and the PFS code initializes them via PSI_MEMORY_CALL(register_memory)().
mem_key_other and mem_key_std are special in the following way (see also
ut_allocator::get_mem_key()):
* If the caller has not provided a key and the file name of the caller is
  unknown, then mem_key_std will be used. This happens only when called from
  within std::* containers.
* If the caller has not provided a key and the file name of the caller is
  known, but is not amongst the predefined names (see ut_new_boot()) then
  mem_key_other will be used. Generally this should not happen and if it
  happens then that means that the list of predefined names must be extended.
Keep this list alphabetically sorted. */
extern PSI_memory_key	mem_key_ahi;
extern PSI_memory_key	mem_key_buf_buf_pool;
extern PSI_memory_key	mem_key_dict_stats_bg_recalc_pool_t;
extern PSI_memory_key	mem_key_dict_stats_index_map_t;
extern PSI_memory_key	mem_key_dict_stats_n_diff_on_level;
extern PSI_memory_key	mem_key_other;
extern PSI_memory_key	mem_key_row_log_buf;
extern PSI_memory_key	mem_key_row_merge_sort;
extern PSI_memory_key	mem_key_std;

/** Setup the internal objects needed for UT_NEW() to operate.
This must be called before the first call to UT_NEW(). */
void
ut_new_boot();

#ifdef UNIV_PFS_MEMORY

/**
Retrieve a memory key (registered with PFS),
given AUTOEVENT_IDX of the caller

@param[in] autoevent_idx - AUTOEVENT_IDX value of the caller
@return registered memory key or PSI_NOT_INSTRUMENTED */
PSI_memory_key ut_new_get_key_by_file(uint32_t autoevent_idx);

#endif /* UNIV_PFS_MEMORY */

/** A structure that holds the necessary data for performance schema
accounting. An object of this type is put in front of each allocated block
of memory when allocation is done by ut_allocator::allocate(). This is
because the data is needed even when freeing the memory. Users of
ut_allocator::allocate_large() are responsible for maintaining this
themselves. */
struct ut_new_pfx_t {

#ifdef UNIV_PFS_MEMORY

	/** Performance schema key. Assigned to a name at startup via
	PSI_MEMORY_CALL(register_memory)() and later used for accounting
	allocations and deallocations with
	PSI_MEMORY_CALL(memory_alloc)(key, size, owner) and
	PSI_MEMORY_CALL(memory_free)(key, size, owner). */
	PSI_memory_key	m_key;

        /**
          Thread owner.
          Instrumented thread that owns the allocated memory.
          This state is used by the performance schema to maintain
          per thread statistics,
          when memory is given from thread A to thread B.
        */
        struct PSI_thread *m_owner;

#endif /* UNIV_PFS_MEMORY */

	/** Size of the allocated block in bytes, including this prepended
	aux structure (for ut_allocator::allocate()). For example if InnoDB
	code requests to allocate 100 bytes, and sizeof(ut_new_pfx_t) is 16,
	then 116 bytes are allocated in total and m_size will be 116.
	ut_allocator::allocate_large() does not prepend this struct to the
	allocated block and its users are responsible for maintaining it
	and passing it later to ut_allocator::deallocate_large(). */
	size_t		m_size;
#if SIZEOF_VOIDP == 4
	/** Pad the header size to a multiple of 64 bits on 32-bit systems,
	so that the payload will be aligned to 64 bits. */
	size_t		pad;
#endif
};

#if defined(DBUG_OFF) && defined(HAVE_MADVISE) && defined(MADV_DODUMP)
static inline void ut_dontdump(void *ptr, size_t m_size, bool dontdump)
{
	ut_a(ptr != NULL);

	if (dontdump && madvise(ptr, m_size, MADV_DONTDUMP)) {
		ib::warn() << "Failed to set memory to " DONTDUMP_STR ": "
			   << strerror(errno)
			   << " ptr " << ptr
			   << " size " << m_size;
	}
}

static inline void ut_dodump(void* ptr, size_t m_size)
{
	if (ptr && madvise(ptr, m_size, MADV_DODUMP)) {
		ib::warn() << "Failed to set memory to " DODUMP_STR ": "
			   << strerror(errno)
			   << " ptr " << ptr
			   << " size " << m_size;
	}
}
#else
static inline void ut_dontdump(void *, size_t, bool) {}
static inline void ut_dodump(void*, size_t) {}
#endif

/** Allocator class for allocating memory from inside std::* containers.
@tparam	T		type of allocated object
@tparam oom_fatal	whether to commit suicide when running out of memory */
template <class T, bool oom_fatal = true>
class ut_allocator {
public:
	typedef T*		pointer;
	typedef const T*	const_pointer;
	typedef T&		reference;
	typedef const T&	const_reference;
	typedef T		value_type;
	typedef size_t		size_type;
	typedef ptrdiff_t	difference_type;

#ifdef UNIV_PFS_MEMORY
	/** Default constructor. */
	explicit
	ut_allocator(PSI_memory_key key = PSI_NOT_INSTRUMENTED)
		: m_key(key)
	{
	}
#else
	ut_allocator() {}
	ut_allocator(PSI_memory_key) {}
#endif /* UNIV_PFS_MEMORY */

	/** Constructor from allocator of another type. */
	template <class U>
	ut_allocator(const ut_allocator<U>&
#ifdef UNIV_PFS_MEMORY
		     other
#endif
		     )
	{
#ifdef UNIV_PFS_MEMORY
		const PSI_memory_key other_key = other.get_mem_key();

		m_key = (other_key != mem_key_std)
			? other_key
			: PSI_NOT_INSTRUMENTED;
#endif /* UNIV_PFS_MEMORY */
	}

	/** Return the maximum number of objects that can be allocated by
	this allocator. */
	size_type
	max_size() const
	{
		const size_type	s_max = std::numeric_limits<size_type>::max();

#ifdef UNIV_PFS_MEMORY
		return((s_max - sizeof(ut_new_pfx_t)) / sizeof(T));
#else
		return(s_max / sizeof(T));
#endif /* UNIV_PFS_MEMORY */
	}

	pointer allocate(size_type n) { return allocate(n, NULL, INVALID_AUTOEVENT_IDX); }

	/** Allocate a chunk of memory that can hold 'n_elements' objects of
	type 'T' and trace the allocation.
	If the allocation fails this method may throw an exception. This
	is mandated by the standard and if it returns NULL instead, then
	STL containers that use it (e.g. std::vector) may get confused.
	After successfull allocation the returned pointer must be passed
	to ut_allocator::deallocate() when no longer needed.
	@param[in]	n_elements	number of elements
	@param[in]	set_to_zero	if true, then the returned memory is
	initialized with 0x0 bytes.
	@param[in]	throw_on_error	if true, raize exception if too big
	@return pointer to the allocated memory */
	pointer
	allocate(
		size_type	n_elements,
		const_pointer,
		uint32_t
#ifdef UNIV_PFS_MEMORY
		autoevent_idx /* AUTOEVENT_IDX of the caller */
#endif
		,
		bool		set_to_zero = false,
		bool		throw_on_error = true)
	{
		if (n_elements == 0) {
			return(NULL);
		}

		if (n_elements > max_size()) {
			if (throw_on_error) {
				throw(std::bad_alloc());
			} else {
				return(NULL);
			}
		}

		void*	ptr;
		size_t	total_bytes = n_elements * sizeof(T);

#ifdef UNIV_PFS_MEMORY
		/* The header size must not ruin the 64-bit alignment
		on 32-bit systems. Some allocated structures use
		64-bit fields. */
		ut_ad((sizeof(ut_new_pfx_t) & 7) == 0);
		total_bytes += sizeof(ut_new_pfx_t);
#endif /* UNIV_PFS_MEMORY */

		for (size_t retries = 1; ; retries++) {

			if (set_to_zero) {
				ptr = calloc(1, total_bytes);
			} else {
				ptr = malloc(total_bytes);
			}

			if (ptr != NULL || retries >= alloc_max_retries) {
				break;
			}

			std::this_thread::sleep_for(std::chrono::seconds(1));
		}

		if (ptr == NULL) {
			ib::fatal_or_error(oom_fatal)
				<< "Cannot allocate " << total_bytes
				<< " bytes of memory after "
				<< alloc_max_retries << " retries over "
				<< alloc_max_retries << " seconds. OS error: "
				<< strerror(errno) << " (" << errno << "). "
				<< OUT_OF_MEMORY_MSG;
			if (throw_on_error) {
				throw(std::bad_alloc());
			} else {
				return(NULL);
			}
		}

#ifdef UNIV_PFS_MEMORY
		ut_new_pfx_t*	pfx = static_cast<ut_new_pfx_t*>(ptr);

		allocate_trace(total_bytes, autoevent_idx, pfx);

		return(reinterpret_cast<pointer>(pfx + 1));
#else
		return(reinterpret_cast<pointer>(ptr));
#endif /* UNIV_PFS_MEMORY */
	}

	/** Free a memory allocated by allocate() and trace the deallocation.
	@param[in,out]	ptr		pointer to memory to free */
	void deallocate(pointer ptr, size_type n_elements = 0)
	{
#ifdef UNIV_PFS_MEMORY
		if (ptr == NULL) {
			return;
		}

		ut_new_pfx_t*	pfx = reinterpret_cast<ut_new_pfx_t*>(ptr) - 1;

		deallocate_trace(pfx);

		free(pfx);
#else
		free(ptr);
#endif /* UNIV_PFS_MEMORY */
	}

	/** Create an object of type 'T' using the value 'val' over the
	memory pointed by 'p'. */
	void
	construct(
		pointer		p,
		const T&	val)
	{
		new(p) T(val);
	}

	/** Destroy an object pointed by 'p'. */
	void
	destroy(
		pointer	p)
	{
		p->~T();
	}

	/** Return the address of an object. */
	pointer
	address(
		reference	x) const
	{
		return(&x);
	}

	/** Return the address of a const object. */
	const_pointer
	address(
		const_reference	x) const
	{
		return(&x);
	}

	template <class U>
	struct rebind {
		typedef ut_allocator<U>	other;
	};

	/* The following are custom methods, not required by the standard. */

#ifdef UNIV_PFS_MEMORY

	/** realloc(3)-like method.
	The passed in ptr must have been returned by allocate() and the
	pointer returned by this method must be passed to deallocate() when
	no longer needed.
	@param[in,out]	ptr		old pointer to reallocate
	@param[in]	n_elements	new number of elements to allocate
	@param[in]	file		file name of the caller
	@return newly allocated memory */
	pointer
	reallocate(
		void*		ptr,
		size_type	n_elements,
		uint32_t	autoevent_idx)
	{
		if (n_elements == 0) {
			deallocate(static_cast<pointer>(ptr));
			return(NULL);
		}

		if (ptr == NULL) {
			return(allocate(n_elements, NULL, autoevent_idx, false, false));
		}

		if (n_elements > max_size()) {
			return(NULL);
		}

		ut_new_pfx_t*	pfx_old;
		ut_new_pfx_t*	pfx_new;
		size_t		total_bytes;

		pfx_old = reinterpret_cast<ut_new_pfx_t*>(ptr) - 1;

		total_bytes = n_elements * sizeof(T) + sizeof(ut_new_pfx_t);

		for (size_t retries = 1; ; retries++) {

			pfx_new = static_cast<ut_new_pfx_t*>(
				realloc(pfx_old, total_bytes));

			if (pfx_new != NULL || retries >= alloc_max_retries) {
				break;
			}

			std::this_thread::sleep_for(std::chrono::seconds(1));
		}

		if (pfx_new == NULL) {
			ib::fatal_or_error(oom_fatal)
				<< "Cannot reallocate " << total_bytes
				<< " bytes of memory after "
				<< alloc_max_retries << " retries over "
				<< alloc_max_retries << " seconds. OS error: "
				<< strerror(errno) << " (" << errno << "). "
				<< OUT_OF_MEMORY_MSG;
			return(NULL);
		}

		/* pfx_new still contains the description of the old block
		that was presumably freed by realloc(). */
		deallocate_trace(pfx_new);

		/* pfx_new is set here to describe the new block. */
		allocate_trace(total_bytes, autoevent_idx, pfx_new);

		return(reinterpret_cast<pointer>(pfx_new + 1));
	}

	/** Allocate, trace the allocation and construct 'n_elements' objects
	of type 'T'. If the allocation fails or if some of the constructors
	throws an exception, then this method will return NULL. It does not
	throw exceptions. After successfull completion the returned pointer
	must be passed to delete_array() when no longer needed.
	@param[in]	n_elements	number of elements to allocate
	@param[in]	file		file name of the caller
	@return pointer to the first allocated object or NULL */
	pointer
	new_array(
		size_type	n_elements,
		uint32_t autoevent_idx
		)
	{
		T*	p = allocate(n_elements, NULL, autoevent_idx, false, false);

		if (p == NULL) {
			return(NULL);
		}

		T*		first = p;
		size_type	i;

		try {
			for (i = 0; i < n_elements; i++) {
				new(p) T;
				++p;
			}
		} catch (...) {
			for (size_type j = 0; j < i; j++) {
				--p;
				p->~T();
			}

			deallocate(first);

			throw;
		}

		return(first);
	}

	/** Destroy, deallocate and trace the deallocation of an array created
	by new_array().
	@param[in,out]	ptr	pointer to the first object in the array */
	void
	delete_array(
		T*	ptr)
	{
		if (ptr == NULL) {
			return;
		}

		const size_type	n_elements = n_elements_allocated(ptr);

		T*		p = ptr + n_elements - 1;

		for (size_type i = 0; i < n_elements; i++) {
			p->~T();
			--p;
		}

		deallocate(ptr);
	}

#endif /* UNIV_PFS_MEMORY */

	/** Allocate a large chunk of memory that can hold 'n_elements'
	objects of type 'T' and trace the allocation.
	@param[in]	n_elements	number of elements
	@param[in]	dontdump	if true, advise the OS is not to core
	dump this memory.
	@param[out]	pfx		storage for the description of the
	allocated memory. The caller must provide space for this one and keep
	it until the memory is no longer needed and then pass it to
	deallocate_large().
	@return pointer to the allocated memory or NULL */
	pointer
	allocate_large(
		size_type	n_elements,
		ut_new_pfx_t*	pfx,
		bool		dontdump = false)
	{
		if (n_elements == 0 || n_elements > max_size()) {
			return(NULL);
		}

		ulint	n_bytes = n_elements * sizeof(T);

		pointer	ptr = reinterpret_cast<pointer>(
			my_large_malloc(&n_bytes, MYF(0)));

		if (ptr == NULL) {
			return NULL;
		}

		ut_dontdump(ptr, n_bytes, dontdump);

		if (pfx != NULL) {
#ifdef UNIV_PFS_MEMORY
			allocate_trace(n_bytes, 0, pfx);
#endif /* UNIV_PFS_MEMORY */
			pfx->m_size = n_bytes;
		}

		os_total_large_mem_allocated += n_bytes;

		return(ptr);
	}

	pointer
	allocate_large_dontdump(
		size_type	n_elements,
		ut_new_pfx_t*	pfx)
	{
		return allocate_large(n_elements, pfx, true);
	}
	/** Free a memory allocated by allocate_large() and trace the
	deallocation.
	@param[in,out]	ptr	pointer to memory to free
	@param[in]	pfx	descriptor of the memory, as returned by
	allocate_large(). */
	void
	deallocate_large(
		pointer			ptr,
		const ut_new_pfx_t*	pfx)
	{
		size_t size = pfx->m_size;
#ifdef UNIV_PFS_MEMORY
		if (pfx) {
			deallocate_trace(pfx);
		}
#endif /* UNIV_PFS_MEMORY */
		os_total_large_mem_allocated -= size;

		my_large_free(ptr, size);
	}

	void
	deallocate_large_dodump(
		pointer			ptr,
		const ut_new_pfx_t*	pfx)
	{
		ut_dodump(ptr, pfx->m_size);
		deallocate_large(ptr, pfx);
	}

#ifdef UNIV_PFS_MEMORY
	/** Get the performance schema key to use for tracing allocations.
	@param[in]	file	file name of the caller or NULL if unknown
	@return performance schema key */
	PSI_memory_key
	get_mem_key(
		uint32_t autoevent_idx = INVALID_AUTOEVENT_IDX) const
	{
		if (m_key != PSI_NOT_INSTRUMENTED) {
			return(m_key);
		}

		if (autoevent_idx == INVALID_AUTOEVENT_IDX) {
			return(mem_key_std);
		}
		const PSI_memory_key	key = ut_new_get_key_by_file(autoevent_idx);

		if (key != PSI_NOT_INSTRUMENTED) {
			return(key);
		}

		return(mem_key_other);
	}

private:

	/** Retrieve the size of a memory block allocated by new_array().
	@param[in]	ptr	pointer returned by new_array().
	@return size of memory block */
	size_type
	n_elements_allocated(
		const_pointer	ptr)
	{
		const ut_new_pfx_t*	pfx
			= reinterpret_cast<const ut_new_pfx_t*>(ptr) - 1;

		const size_type		user_bytes
			= pfx->m_size - sizeof(ut_new_pfx_t);

		ut_ad(user_bytes % sizeof(T) == 0);

		return(user_bytes / sizeof(T));
	}

	/** Trace a memory allocation.
	After the accounting, the data needed for tracing the deallocation
	later is written into 'pfx'.
	The PFS event name is picked on the following criteria:
	1. If key (!= PSI_NOT_INSTRUMENTED) has been specified when constructing
	   this ut_allocator object, then the name associated with that key will
	   be used (this is the recommended approach for new code)
	2. Otherwise, if "file" is NULL, then the name associated with
	   mem_key_std will be used
	3. Otherwise, if an entry is found by ut_new_get_key_by_file(), that
	   corresponds to "file", that will be used (see ut_new_boot())
	4. Otherwise, the name associated with mem_key_other will be used.
	@param[in]	size	number of bytes that were allocated
	@param[in]	autoevent_idx	autoevent_idx of the caller
	@param[out]	pfx	placeholder to store the info which will be
	needed when freeing the memory */
	void
	allocate_trace(
		size_t		size,
		const uint32_t autoevent_idx,
		ut_new_pfx_t*	pfx)
	{
		const PSI_memory_key	key = get_mem_key(autoevent_idx);

		pfx->m_key = PSI_MEMORY_CALL(memory_alloc)(key, size, & pfx->m_owner);
		pfx->m_size = size;
	}

	/** Trace a memory deallocation.
	@param[in]	pfx	info for the deallocation */
	void
	deallocate_trace(
		const ut_new_pfx_t*	pfx)
	{
		PSI_MEMORY_CALL(memory_free)(pfx->m_key, pfx->m_size, pfx->m_owner);
	}

	/** Performance schema key. */
	PSI_memory_key	m_key;

#endif /* UNIV_PFS_MEMORY */

private:

	/** Assignment operator, not used, thus disabled (private). */
	template <class U>
	void
	operator=(
		const ut_allocator<U>&);
};

/** Compare two allocators of the same type.
As long as the type of A1 and A2 is the same, a memory allocated by A1
could be freed by A2 even if the pfs mem key is different. */
template <typename T>
inline
bool
operator==(const ut_allocator<T>&, const ut_allocator<T>&) { return(true); }

/** Compare two allocators of the same type. */
template <typename T>
inline
bool
operator!=(
	const ut_allocator<T>&	lhs,
	const ut_allocator<T>&	rhs)
{
	return(!(lhs == rhs));
}

#ifdef UNIV_PFS_MEMORY

/*
 constexpr trickery ahead.

 Compute AUTOEVENT_IDX at compile time.
 (index in the auto_event_names array, corresponding to basename of __FILE__)

 The tricks are necessary to reduce the cost of lookup the
 PSI_memory_key for auto event.
*/

static constexpr const char* cexpr_basename_helper(const char* s, const char* last_slash)
{
  return
    *s == '\0' ? last_slash :
    *s == '/' || *s == '\\' ? cexpr_basename_helper(s + 1, s + 1) :
    cexpr_basename_helper(s + 1, last_slash);
}

static constexpr const char* cexpr_basename(const char* filename)
{
  return cexpr_basename_helper(filename, filename);
}

static constexpr bool cexpr_strequal_ignore_dot(const char* a, const char* b)
{
  return  *a == 0 || *a == '.' ? (*b == 0 || *b == '.')
    : *a == *b ? cexpr_strequal_ignore_dot(a + 1, b + 1) : false;
}

constexpr const char* const auto_event_names[] =
{
  "btr0btr",
  "btr0buf",
  "btr0bulk",
  "btr0cur",
  "btr0pcur",
  "btr0sea",
  "buf0buf",
  "buf0dblwr",
  "buf0dump",
  "buf0lru",
  "buf0rea",
  "dict0dict",
  "dict0mem",
  "dict0stats",
  "eval0eval",
  "fil0crypt",
  "fil0fil",
  "fsp0file",
  "fts0ast",
  "fts0blex",
  "fts0config",
  "fts0file",
  "fts0fts",
  "fts0opt",
  "fts0pars",
  "fts0que",
  "fts0sql",
  "fts0tlex",
  "gis0sea",
  "ha_innodb",
  "handler0alter",
  "hash0hash",
  "i_s",
  "lexyy",
  "lock0lock",
  "mem0mem",
  "os0file",
  "pars0lex",
  "rem0rec",
  "row0ftsort",
  "row0import",
  "row0log",
  "row0merge",
  "row0mysql",
  "row0sel",
  "srv0start",
  "trx0i_s",
  "trx0i_s",
  "trx0roll",
  "trx0rseg",
  "trx0seg",
  "trx0trx",
  "trx0undo",
  "ut0list",
  "ut0mem",
  "ut0new",
  "ut0pool",
  "ut0rbt",
  "ut0wqueue",
  "xtrabackup",
  nullptr
};

constexpr uint32_t cexpr_lookup_auto_event_name(const char* name, uint32_t idx = 0)
{
  return !auto_event_names[idx] ? INVALID_AUTOEVENT_IDX :
    cexpr_strequal_ignore_dot(name, auto_event_names[idx]) ? idx :
    cexpr_lookup_auto_event_name(name, idx + 1);
}

/*
 The AUTOEVENT_IDX macro.

 Note, that there is a static_assert that checks whether
 basename of the __FILE is not registered in the auto_event_names array.
 If you run into this assert, add the basename to the array.

 Weird looking lambda is used to force the evaluation at the compile time.
*/
#define AUTOEVENT_IDX []()\
{\
  constexpr auto idx = cexpr_lookup_auto_event_name(cexpr_basename(__FILE__)); \
  static_assert(idx != INVALID_AUTOEVENT_IDX, "auto_event_names contains no entry for " __FILE__);\
  return idx; \
}()


/** Allocate, trace the allocation and construct an object.
Use this macro instead of 'new' within InnoDB.
For example: instead of
	Foo*	f = new Foo(args);
use:
	Foo*	f = UT_NEW(Foo(args), mem_key_some);
Upon failure to allocate the memory, this macro may return NULL. It
will not throw exceptions. After successfull allocation the returned
pointer must be passed to UT_DELETE() when no longer needed.
@param[in]	expr	any expression that could follow "new"
@param[in]	key	performance schema memory tracing key
@return pointer to the created object or NULL */
#define UT_NEW(expr, key) \
	/* Placement new will return NULL and not attempt to construct an
	object if the passed in pointer is NULL, e.g. if allocate() has
	failed to allocate memory and has returned NULL. */ \
	::new(ut_allocator<byte>(key).allocate( \
		sizeof expr, NULL, AUTOEVENT_IDX, false, false)) expr

/** Allocate, trace the allocation and construct an object.
Use this macro instead of 'new' within InnoDB and instead of UT_NEW()
when creating a dedicated memory key is not feasible.
For example: instead of
	Foo*	f = new Foo(args);
use:
	Foo*	f = UT_NEW_NOKEY(Foo(args));
Upon failure to allocate the memory, this macro may return NULL. It
will not throw exceptions. After successfull allocation the returned
pointer must be passed to UT_DELETE() when no longer needed.
@param[in]	expr	any expression that could follow "new"
@return pointer to the created object or NULL */
#define UT_NEW_NOKEY(expr)	UT_NEW(expr, PSI_NOT_INSTRUMENTED)

/** Destroy, deallocate and trace the deallocation of an object created by
UT_NEW() or UT_NEW_NOKEY().
We can't instantiate ut_allocator without having the type of the object, thus
we redirect this to a templated function. */
#define UT_DELETE(ptr)		ut_delete(ptr)


/** Destroy and account object created by UT_NEW() or UT_NEW_NOKEY().
@param[in,out]	ptr	pointer to the object */
template <typename T>
inline
void
ut_delete(
	T*	ptr)
{
	if (ptr == NULL) {
		return;
	}

	ut_allocator<T>	allocator;

	allocator.destroy(ptr);
	allocator.deallocate(ptr);
}

/** Allocate and account 'n_elements' objects of type 'type'.
Use this macro to allocate memory within InnoDB instead of 'new[]'.
The returned pointer must be passed to UT_DELETE_ARRAY().
@param[in]	type		type of objects being created
@param[in]	n_elements	number of objects to create
@param[in]	key		performance schema memory tracing key
@return pointer to the first allocated object or NULL */
#define UT_NEW_ARRAY(type, n_elements, key) \
	ut_allocator<type>(key).new_array(n_elements, AUTOEVENT_IDX)

/** Allocate and account 'n_elements' objects of type 'type'.
Use this macro to allocate memory within InnoDB instead of 'new[]' and
instead of UT_NEW_ARRAY() when it is not feasible to create a dedicated key.
@param[in]	type		type of objects being created
@param[in]	n_elements	number of objects to create
@return pointer to the first allocated object or NULL */
#define UT_NEW_ARRAY_NOKEY(type, n_elements) \
	UT_NEW_ARRAY(type, n_elements, PSI_NOT_INSTRUMENTED)

/** Destroy, deallocate and trace the deallocation of an array created by
UT_NEW_ARRAY() or UT_NEW_ARRAY_NOKEY().
We can't instantiate ut_allocator without having the type of the object, thus
we redirect this to a templated function. */
#define UT_DELETE_ARRAY(ptr)	ut_delete_array(ptr)

/** Destroy and account objects created by UT_NEW_ARRAY() or
UT_NEW_ARRAY_NOKEY().
@param[in,out]	ptr	pointer to the first object in the array */
template <typename T>
inline
void
ut_delete_array(
	T*	ptr)
{
	ut_allocator<T>().delete_array(ptr);
}

#define ut_malloc(n_bytes, key)		static_cast<void*>( \
	ut_allocator<byte>(key).allocate( \
		n_bytes, NULL, AUTOEVENT_IDX, false, false))

#define ut_malloc_dontdump(n_bytes, key) static_cast<void*>( \
	ut_allocator<byte>(key).allocate_large( \
		n_bytes, NULL, true))

#define ut_zalloc(n_bytes, key)		static_cast<void*>( \
	ut_allocator<byte>(key).allocate( \
		n_bytes, NULL, AUTOEVENT_IDX, true, false))

#define ut_malloc_nokey(n_bytes)	static_cast<void*>( \
	ut_allocator<byte>(PSI_NOT_INSTRUMENTED).allocate( \
		n_bytes, NULL, AUTOEVENT_IDX, false, false))

#define ut_zalloc_nokey(n_bytes)	static_cast<void*>( \
	ut_allocator<byte>(PSI_NOT_INSTRUMENTED).allocate( \
		n_bytes, NULL, AUTOEVENT_IDX, true, false))

#define ut_zalloc_nokey_nofatal(n_bytes)	static_cast<void*>( \
	ut_allocator<byte, false>(PSI_NOT_INSTRUMENTED).allocate( \
		n_bytes, NULL, AUTOEVENT_IDX, true, false))

#define ut_realloc(ptr, n_bytes)	static_cast<void*>( \
	ut_allocator<byte>(PSI_NOT_INSTRUMENTED).reallocate( \
		ptr, n_bytes, AUTOEVENT_IDX))

#define ut_free(ptr)	ut_allocator<byte>(PSI_NOT_INSTRUMENTED).deallocate( \
	reinterpret_cast<byte*>(ptr))

#else /* UNIV_PFS_MEMORY */

/* Fallbacks when memory tracing is disabled at compile time. */

#define UT_NEW(expr, key)		::new(std::nothrow) expr
#define UT_NEW_NOKEY(expr)		::new(std::nothrow) expr
#define UT_DELETE(ptr)			::delete ptr

#define UT_NEW_ARRAY(type, n_elements, key) \
	::new(std::nothrow) type[n_elements]

#define UT_NEW_ARRAY_NOKEY(type, n_elements) \
	::new(std::nothrow) type[n_elements]

#define UT_DELETE_ARRAY(ptr)		::delete[] ptr

#define ut_malloc(n_bytes, key)		::malloc(n_bytes)

#define ut_zalloc(n_bytes, key)		::calloc(1, n_bytes)

#define ut_malloc_nokey(n_bytes)	::malloc(n_bytes)

static inline void *ut_malloc_dontdump(size_t n_bytes, ...)
{
	void *ptr = my_large_malloc(&n_bytes, MYF(0));

	ut_dontdump(ptr, n_bytes, true);

	if (ptr) {
		os_total_large_mem_allocated += n_bytes;
	}
	return ptr;
}

#define ut_zalloc_nokey(n_bytes)	::calloc(1, n_bytes)

#define ut_zalloc_nokey_nofatal(n_bytes)	::calloc(1, n_bytes)

#define ut_realloc(ptr, n_bytes)	::realloc(ptr, n_bytes)

#define ut_free(ptr)			::free(ptr)

#endif /* UNIV_PFS_MEMORY */

static inline void ut_free_dodump(void *ptr, size_t size)
{
	ut_dodump(ptr, size);
	os_total_large_mem_allocated -= size;
	my_large_free(ptr, size);
}

#endif /* ut0new_h */
