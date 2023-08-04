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


#ifdef __GLIBC__
struct libc_version_t
{
  int major;
  int minor;
};

#include <gnu/libc-version.h>


static libc_version_t get_libc_version() {
  libc_version_t ver;
  sscanf(gnu_get_libc_version(), "%d.%d", &ver.major, &ver.minor);

  return ver;
}
static const libc_version_t libc_version= get_libc_version();
#endif

/** Use the result of my_timer_cycles(), which mainly uses RDTSC for cycles
as a random value. See the comments for my_timer_cycles() */
/** @return result from RDTSC or similar functions. */
static inline size_t
get_rnd_value()
{
#if defined(__GLIBC__) && defined(__linux)
#if __GLIBC__ < 2 || __GLIBC_MINOR__ < 35
#if !defined(HAVE_POWER8) && !defined(__x86_64__) && !defined(__riscv)
        // Build-time glibc is older, but the runtime is fresh enough
        if (libc_version.major >= 2 && libc_version.minor >= 35)
#endif
#endif
        return sched_getcpu();
#endif
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

static const auto ncpus= std::thread::hardware_concurrency();

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

	/** Add to the counter.
	@param[in]	n	amount to be added */
	void add(Type n) { add(get_rnd_value(), n); }

private:
	/** Add to the counter.
	@param[in]	index	a reasonably thread-unique identifier
	@param[in]	n	amount to be added */
	TPOOL_SUPPRESS_TSAN void add(size_t index, Type n) {
		index = index % ncpus;

		m_counter[index].value += n;
	}
public:

	/* @return total value - not 100% accurate, since it is relaxed atomic*/
	operator Type() const {
		Type	total = 0;

		for (size_t i = 0; i < ncpus; i++) {
			total += m_counter[i].value;
		}

		return(total);
	}

private:
	static_assert(sizeof(Element<Type>) == CPU_LEVEL1_DCACHE_LINESIZE, "");
	/** Array of counter elements */
	alignas(CPU_LEVEL1_DCACHE_LINESIZE) Element<Type> m_counter[N];
};

#endif /* ut0counter_h */
