/*****************************************************************************

Copyright (c) 2012, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2018, MariaDB Corporation.

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
@file include/ut0counter.h

Counter utility class

Created 2012/04/12 by Sunny Bains
*******************************************************/

#ifndef ut0counter_h
#define ut0counter_h

#include "os0thread.h"
#include "my_rdtsc.h"
#include "my_atomic.h"

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

/** Class for using fuzzy counters. The counter is relaxed atomic
so the results are not guaranteed to be 100% accurate but close
enough. Creates an array of counters and separates each element by the
CACHE_LINE_SIZE bytes */
template <
	typename Type,
	int N = IB_N_SLOTS,
	template<typename, int> class Indexer = default_indexer_t>
struct MY_ALIGNED(CACHE_LINE_SIZE) ib_counter_t
{
	/** Increment the counter by 1. */
	void inc() UNIV_NOTHROW { add(1); }

	/** Increment the counter by 1.
	@param[in]	index	a reasonably thread-unique identifier */
	void inc(size_t index) UNIV_NOTHROW { add(index, 1); }

	/** Add to the counter.
	@param[in]	n	amount to be added */
	void add(Type n) UNIV_NOTHROW { add(m_policy.get_rnd_offset(), n); }

	/** Add to the counter.
	@param[in]	index	a reasonably thread-unique identifier
	@param[in]	n	amount to be added */
	void add(size_t index, Type n) UNIV_NOTHROW {
		size_t	i = m_policy.offset(index);

		ut_ad(i < UT_ARR_SIZE(m_counter));

		if (sizeof(Type) == 8) {
			my_atomic_add64_explicit(
			    reinterpret_cast<int64*>(&m_counter[i]),
			    static_cast<int64>(n), MY_MEMORY_ORDER_RELAXED);
		} else if (sizeof(Type) == 4) {
			my_atomic_add32_explicit(
			    reinterpret_cast<int32*>(&m_counter[i]),
			    static_cast<int32>(n), MY_MEMORY_ORDER_RELAXED);
		}
		compile_time_assert(sizeof(Type) == 8 || sizeof(Type) == 4);
	}

	/* @return total value - not 100% accurate, since it is relaxed atomic. */
	operator Type() const UNIV_NOTHROW {
		Type	total = 0;

		for (size_t i = 0; i < N; ++i) {
			if (sizeof(Type) == 8) {
				total += static_cast<
				    Type>(my_atomic_load64_explicit(
				    reinterpret_cast<int64*>(const_cast<Type*>(
					&m_counter[m_policy.offset(i)])),
				    MY_MEMORY_ORDER_RELAXED));
			} else if (sizeof(Type) == 4) {
				total += static_cast<
				    Type>(my_atomic_load32_explicit(
				    reinterpret_cast<int32*>(const_cast<Type*>(
					&m_counter[m_policy.offset(i)])),
				    MY_MEMORY_ORDER_RELAXED));
			}
		}

		return(total);
	}

private:
	/** Indexer into the array */
	Indexer<Type, N>m_policy;

        /** Slot 0 is unused. */
	Type		m_counter[(N + 1) * (CACHE_LINE_SIZE / sizeof(Type))];
};

#endif /* ut0counter_h */
