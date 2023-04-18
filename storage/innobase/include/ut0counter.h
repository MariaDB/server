/*****************************************************************************

Copyright (c) 2012, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

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

#include "univ.i"
#include "my_rdtsc.h"

/** Use the result of my_timer_cycles(), which mainly uses RDTSC for cycles
as a random value. See the comments for my_timer_cycles() */
/** @return result from RDTSC or similar functions. */
static inline size_t
get_rnd_value()
{
	size_t c = static_cast<size_t>(my_timer_cycles());

	if (c != 0) {
		return c;
	}

	/* We may go here if my_timer_cycles() returns 0,
	so we have to have the plan B for the counter. */
#if !defined(_WIN32)
	return (size_t)pthread_self();
#else
	LARGE_INTEGER cnt;
	QueryPerformanceCounter(&cnt);

	return static_cast<size_t>(cnt.QuadPart);
#endif /* !_WIN32 */
}

/** Atomic which occupies whole CPU cache line.
Note: We rely on the default constructor of std::atomic and
do not explicitly initialize the contents. This works for us,
because ib_counter_t is only intended for usage with global
memory that is allocated from the .bss and thus guaranteed to
be zero-initialized by the run-time environment.
@see srv_stats */
template <typename Type>
struct ib_atomic_counter_element_t {
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) Atomic_relaxed<Type> value;
};

template <typename Type>
struct ib_counter_element_t {
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) Type value;
};


/** Class for using fuzzy counters. The counter is multi-instance relaxed atomic
so the results are not guaranteed to be 100% accurate but close
enough. */
template <typename Type,
          template <typename T> class Element = ib_atomic_counter_element_t,
          int N = 128 >
struct ib_counter_t {
	/** Increment the counter by 1. */
	void inc() { add(1); }
	ib_counter_t& operator++() { inc(); return *this; }

	/** Increment the counter by 1.
	@param[in]	index	a reasonably thread-unique identifier */
	void inc(size_t index) { add(index, 1); }

	/** Add to the counter.
	@param[in]	n	amount to be added */
	void add(Type n) { add(get_rnd_value(), n); }

	/** Add to the counter.
	@param[in]	index	a reasonably thread-unique identifier
	@param[in]	n	amount to be added */
	TPOOL_SUPPRESS_TSAN void add(size_t index, Type n) {
		index = index % N;

		ut_ad(index < UT_ARR_SIZE(m_counter));

		m_counter[index].value += n;
	}

	/* @return total value - not 100% accurate, since it is relaxed atomic*/
	operator Type() const {
		Type	total = 0;

		for (const auto &counter : m_counter) {
			total += counter.value;
		}

		return(total);
	}

private:
	static_assert(sizeof(Element<Type>) == CPU_LEVEL1_DCACHE_LINESIZE, "");
	/** Array of counter elements */
	alignas(CPU_LEVEL1_DCACHE_LINESIZE) Element<Type> m_counter[N];
};

#endif /* ut0counter_h */
