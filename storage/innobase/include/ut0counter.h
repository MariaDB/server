/*****************************************************************************

Copyright (c) 2012, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/ut0counter.h

Counter utility class

Created 2012/04/12 by Sunny Bains
*******************************************************/

#ifndef ut0counter_h
#define ut0counter_h

#include <my_rdtsc.h>
#include "univ.i"
#include "os0thread.h"

/** CPU cache line size */
#ifdef CPU_LEVEL1_DCACHE_LINESIZE
# define CACHE_LINE_SIZE	CPU_LEVEL1_DCACHE_LINESIZE
#else
# error CPU_LEVEL1_DCACHE_LINESIZE is undefined
#endif /* CPU_LEVEL1_DCACHE_LINESIZE */

/** Default number of slots to use in ib_counter_t */
#define IB_N_SLOTS		64

/** Get the offset into the counter array. */
template <typename Type, int N>
struct generic_indexer_t {
        /** @return offset within m_counter */
        static size_t offset(size_t index) UNIV_NOTHROW
	{
                return(((index % N) + 1) * (CACHE_LINE_SIZE / sizeof(Type)));
        }
};

/** Use the result of my_timer_cycles(), which mainly uses RDTSC for cycles,
to index into the counter array. See the comments for my_timer_cycles() */
template <typename Type=ulint, int N=1>
struct counter_indexer_t : public generic_indexer_t<Type, N> {
	/** @return result from RDTSC or similar functions. */
	static size_t get_rnd_index() UNIV_NOTHROW
	{
		size_t	c = static_cast<size_t>(my_timer_cycles());

		if (c != 0) {
			return(c);
		} else {
			/* We may go here if my_timer_cycles() returns 0,
			so we have to have the plan B for the counter. */
#if !defined(_WIN32)
			return(size_t(os_thread_get_curr_id()));
#else
			LARGE_INTEGER cnt;
			QueryPerformanceCounter(&cnt);

			return(static_cast<size_t>(cnt.QuadPart));
#endif /* !_WIN32 */
		}
	}

	/** @return a random offset to the array */
	static size_t get_rnd_offset() UNIV_NOTHROW
	{
		return(generic_indexer_t<Type, N>::offset(get_rnd_index()));
	}
};

#define	default_indexer_t	counter_indexer_t

/** Atomic counter */
struct MY_ALIGNED(CACHE_LINE_SIZE) ib_counter_t
{
	ib_counter_t() : m_counter(0) {}

	/** Increment the counter by 1. */
	void inc() UNIV_NOTHROW { add(1); }

	/** Add to the counter.
	@param[in]	n	amount to be added */
	void add(uint64_t n) UNIV_NOTHROW { my_atomic_add64(&m_counter, n); }

	/* @return total value - not 100% accurate, since it is not atomic. */
	operator uint64_t() const UNIV_NOTHROW {
		return(my_atomic_load64_explicit(&m_counter,
						 MY_MEMORY_ORDER_RELAXED));
	}

private:
	uint64_t		m_counter;
};

#endif /* ut0counter_h */
