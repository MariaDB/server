/*****************************************************************************

Copyright (c) 2012, 2015, Oracle and/or its affiliates. All Rights Reserved.
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

/******************************************************************//**
@file include/ut0mutex.h
Policy based mutexes.

Created 2012-03-24 Sunny Bains.
***********************************************************************/

#pragma once
#ifndef UNIV_INNOCHECKSUM
#include "sync0policy.h"
#include "ib0mutex.h"

/** Create a typedef using the MutexType<PolicyType>
@param[in]	M		Mutex type
@param[in[	P		Policy type
@param[in]	T		The resulting typedef alias */
#define UT_MUTEX_TYPE(M, P, T) typedef PolicyMutex<M<P> > T;

# ifdef __linux__
UT_MUTEX_TYPE(TTASFutexMutex, GenericPolicy, FutexMutex);
# endif /* __linux__ */

UT_MUTEX_TYPE(TTASMutex, GenericPolicy, SpinMutex);
UT_MUTEX_TYPE(OSTrackMutex, GenericPolicy, SysMutex);
UT_MUTEX_TYPE(TTASEventMutex, GenericPolicy, SyncArrayMutex);

#ifdef MUTEX_FUTEX
/** The default mutex type. */
typedef FutexMutex ib_mutex_t;
#define MUTEX_TYPE	"Uses futexes"
#elif defined(MUTEX_SYS)
typedef SysMutex ib_mutex_t;
#define MUTEX_TYPE	"Uses system mutexes"
#elif defined(MUTEX_EVENT)
typedef SyncArrayMutex ib_mutex_t;
#define MUTEX_TYPE	"Uses event mutexes"
#else
#error "ib_mutex_t type is unknown"
#endif /* MUTEX_FUTEX */

extern uint	srv_spin_wait_delay;
extern ulong	srv_n_spin_wait_rounds;

#define mutex_create(I, M)		mutex_init((M), (I),		\
						   __FILE__, __LINE__)

#define mutex_enter_loc(M,file,line)	(M)->enter(			\
					uint32_t(srv_n_spin_wait_rounds), \
					uint32_t(srv_spin_wait_delay),	\
					file, line)
#define mutex_enter(M)			mutex_enter_loc(M, __FILE__, __LINE__)

#define mutex_enter_nospin(M)		(M)->enter(			\
					0,				\
					0,				\
					__FILE__, uint32_t(__LINE__))

#define mutex_enter_nowait(M)		(M)->trylock(__FILE__,		\
						     uint32_t(__LINE__))

#define mutex_exit(M)			(M)->exit()

#define mutex_free(M)			mutex_destroy(M)

#ifdef UNIV_DEBUG
/**
Checks that the mutex has been initialized. */
#define mutex_validate(M)		(M)->validate()

/**
Checks that the current thread owns the mutex. Works only
in the debug version. */
#define mutex_own(M)			(M)->is_owned()
#else
#define mutex_own(M)			/* No op */
#define mutex_validate(M)		/* No op */
#endif /* UNIV_DEBUG */

/** Iterate over the mutex meta data */
class MutexMonitor {
public:
	/** Constructor */
	MutexMonitor() { }

	/** Destructor */
	~MutexMonitor() { }

	/** Enable the mutex monitoring */
	void enable();

	/** Disable the mutex monitoring */
	void disable();

	/** Reset the mutex monitoring values */
	void reset();

	/** Invoke the callback for each active mutex collection
	@param[in,out]	callback	Functor to call
	@return false if callback returned false */
	template<typename Callback>
	bool iterate(Callback& callback) const
		UNIV_NOTHROW
	{
		LatchMetaData::iterator	end = latch_meta.end();

		for (LatchMetaData::iterator it = latch_meta.begin();
		     it != end;
		     ++it) {

			/* Some of the slots will be null in non-debug mode */

			if (latch_meta_t* l= *it) {
				if (!callback(*l)) {
					return false;
				}
			}
		}

		return(true);
	}
};

/** Defined in sync0sync.cc */
extern MutexMonitor	mutex_monitor;

/**
Creates, or rather, initializes a mutex object in a specified memory
location (which must be appropriately aligned). The mutex is initialized
in the reset state. Explicit freeing of the mutex with mutex_free is
necessary only if the memory block containing it is freed.
Add the mutex instance to the global mutex list.
@param[in,out]	mutex		mutex to initialise
@param[in]	id		The mutex ID (Latch ID)
@param[in]	filename	Filename from where it was called
@param[in]	line		Line number in filename from where called */
template <typename Mutex>
void mutex_init(
	Mutex*		mutex,
	latch_id_t	id,
	const char*	file_name,
	uint32_t	line)
{
	new(mutex) Mutex();

	mutex->init(id, file_name, line);
}

/**
Removes a mutex instance from the mutex list. The mutex is checked to
be in the reset state.
@param[in,out]	 mutex		mutex instance to destroy */
template <typename Mutex>
void mutex_destroy(
	Mutex*		mutex)
{
	mutex->destroy();
}

#endif /* UNIV_INNOCHECKSUM */
