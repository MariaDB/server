/*****************************************************************************

Copyright (c) 2014, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

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
@file sync/sync0debug.cc
Debug checks for latches.

Created 2012-08-21 Sunny Bains
*******************************************************/

#include "sync0debug.h"
#include "sync0sync.h"
#include "sync0arr.h"
#include "srv0start.h"
#include "lock0lock.h"

#include <vector>
#include <algorithm>
#include <map>

#ifdef UNIV_DEBUG

my_bool		srv_sync_debug;

/** The global mutex which protects debug info lists of all rw-locks.
To modify the debug info list of an rw-lock, this mutex has to be
acquired in addition to the mutex protecting the lock. */
static mysql_mutex_t rw_lock_debug_mutex;

/** The latch held by a thread */
struct Latched
{
  Latched(const rw_lock_t *latch, latch_level_t level)
    : latch(latch), level(level) {}

  bool operator==(const Latched &o) const
  { return latch == o.latch && level == o.level; }

  /** The latch instance */
  const rw_lock_t *latch= nullptr;
  /** The latch level. For buffer blocks we can pass a separate latch
  level to check against, see buf_block_dbg_add_level() */
  latch_level_t level= SYNC_UNKNOWN;
};

/** RW-lock rank names */
static const char *const level_names[SYNC_LEVEL_MAX + 1]= {
  "SYNC_UNKNOWN",
  "RW_LOCK_SX",
  "RW_LOCK_X_WAIT",
  "RW_LOCK_S",
  "RW_LOCK_X",
  "RW_LOCK_NOT_LOCKED",
  "SYNC_SEARCH_SYS",
  "SYNC_TRX_SYS_HEADER",
  "SYNC_IBUF_BITMAP",
  "SYNC_IBUF_TREE_NODE",
  "SYNC_IBUF_TREE_NODE_NEW",
  "SYNC_IBUF_INDEX_TREE",
  "SYNC_FSP_PAGE",
  "SYNC_FSP",
  "SYNC_EXTERN_STORAGE",
  "SYNC_TRX_UNDO_PAGE",
  "SYNC_RSEG_HEADER",
  "SYNC_RSEG_HEADER_NEW",
  "SYNC_PURGE_LATCH",
  "SYNC_TREE_NODE",
  "SYNC_TREE_NODE_FROM_HASH",
  "SYNC_TREE_NODE_NEW",
  "SYNC_INDEX_TREE",
  "SYNC_IBUF_HEADER",
  "SYNC_DICT_HEADER",
  "SYNC_DICT_OPERATION",
  "SYNC_TRX_I_S_RWLOCK",
  "SYNC_LEVEL_VARYING",
  "SYNC_NO_ORDER_CHECK"
};

/** Thread specific latches. This is ordered on level in descending order. */
typedef std::vector<Latched, ut_allocator<Latched> > Latches;

/** The deadlock detector. */
struct LatchDebug {
	/** Comparator for the ThreadMap. */
	struct os_thread_id_less
		: public std::binary_function<
		  os_thread_id_t,
		  os_thread_id_t,
		  bool>
	{
		/** @return true if lhs < rhs */
		bool operator()(
			const os_thread_id_t& lhs,
			const os_thread_id_t& rhs) const
			UNIV_NOTHROW
		{
			return(os_thread_pf(lhs) < os_thread_pf(rhs));
		}
	};

	/** For tracking a thread's latches. */
	typedef std::map<
		os_thread_id_t,
		Latches*,
		os_thread_id_less,
		ut_allocator<std::pair<const os_thread_id_t, Latches*> > >
		ThreadMap;

	void init() { mysql_mutex_init(0, &m_mutex, nullptr); }
	void close() { mysql_mutex_destroy(&m_mutex); m_threads.clear(); }

	/** Create a new instance if one doesn't exist else return
	the existing one.
	@param[in]	add		add an empty entry if one is not
					found (default no)
	@return	pointer to a thread's acquired latches. */
	Latches* thread_latches(bool add = false)
		UNIV_NOTHROW;

	/** Check that all the latches already owned by a thread have a lower
	level than limit.
	@param[in]	latches		the thread's existing (acquired) latches
	@param[in]	limit		to check against
	@return latched if there is one with a level <= limit . */
	const Latched* less(
		const Latches*	latches,
		latch_level_t	limit) const
		UNIV_NOTHROW;

	/** Checks if the level value exists in the thread's acquired latches.
	@param[in]	latches		the thread's existing (acquired) latches
	@param[in]	level		to lookup
	@return	latch if found or 0 */
	const rw_lock_t* find(
		const Latches*	Latches,
		latch_level_t	level) const
		UNIV_NOTHROW;

	/**
	Checks if the level value exists in the thread's acquired latches.
	@param[in]	level		to lookup
	@return	latch if found or 0 */
	const rw_lock_t* find(latch_level_t level)
		UNIV_NOTHROW;

	/** Report error and abort.
	@param[in]	latches		thread's existing latches
	@param[in]	latched		The existing latch causing the
					invariant to fail
	@param[in]	level		The new level request that breaks
					the order */
	void crash(
		const Latches*	latches,
		const Latched*	latched,
		latch_level_t	level) const
		UNIV_NOTHROW;

	/** Do a basic ordering check.
	@param[in]	latches		thread's existing latches
	@param[in]	requested_level	Level requested by latch
	@param[in]	level		declared ulint so that we can
					do level - 1. The level of the
					latch that the thread is trying
					to acquire
	@return true if passes, else crash with error message. */
	inline bool basic_check(
		const Latches*	latches,
		latch_level_t	requested_level,
		lint		level) const
		UNIV_NOTHROW;

	/** Adds a latch and its level in the thread level array. Allocates
	the memory for the array if called for the first time for this
	OS thread.  Makes the checks against other latch levels stored
	in the array for this thread.

	@param[in]	latch	latch that the thread wants to acqire.
	@param[in]	level	latch level to check against */
	void lock_validate(
		const rw_lock_t*	latch,
		latch_level_t	level)
		UNIV_NOTHROW
	{
		if (latch->level != SYNC_LEVEL_VARYING) {
			ut_ad(level != SYNC_LEVEL_VARYING);

			Latches*	latches = check_order(latch, level);

			ut_a(latches->empty()
			     || level == SYNC_LEVEL_VARYING
			     || level == SYNC_NO_ORDER_CHECK
			     || latches->back().level
			     == SYNC_NO_ORDER_CHECK
			     || latches->back().latch->level
			     == SYNC_LEVEL_VARYING
			     || latches->back().level >= level);
		}
	}

	/** Adds a latch and its level in the thread level array. Allocates
	the memory for the array if called for the first time for this
	OS thread.  Makes the checks against other latch levels stored
	in the array for this thread.

	@param[in]	latch	latch that the thread wants to acqire.
	@param[in]	level	latch level to check against */
	void lock_granted(
		const rw_lock_t*	latch,
		latch_level_t	level)
		UNIV_NOTHROW
	{
		if (latch->level != SYNC_LEVEL_VARYING) {
			thread_latches(true)->push_back(Latched(latch, level));
		}
	}

	/** For recursive X rw-locks.
	@param[in]	latch		The RW-Lock to relock  */
	void relock(const rw_lock_t* latch)
		UNIV_NOTHROW
	{
		latch_level_t	level = latch->level;

		if (level != SYNC_LEVEL_VARYING) {

			Latches*	latches = thread_latches(true);

			Latches::iterator	it = std::find(
				latches->begin(), latches->end(),
				Latched(latch, level));

			ut_a(latches->empty()
			     || level == SYNC_LEVEL_VARYING
			     || level == SYNC_NO_ORDER_CHECK
			     || latches->back().latch->level
			     == SYNC_LEVEL_VARYING
			     || latches->back().latch->level
			     == SYNC_NO_ORDER_CHECK
			     || latches->back().level >= level
			     || it != latches->end());

			if (it == latches->end()) {
				latches->push_back(Latched(latch, level));
			} else {
				latches->insert(it, Latched(latch, level));
			}
		}
	}

	/** Iterate over a thread's latches.
	@param[in]	functor		The callback
	@return true if the functor returns true. */
	bool for_each(const sync_check_functor_t& functor)
		UNIV_NOTHROW
	{
		if (const Latches* latches = thread_latches()) {
			Latches::const_iterator	end = latches->end();
			for (Latches::const_iterator it = latches->begin();
			     it != end; ++it) {

				if (functor(it->level)) {
					return(true);
				}
			}
		}

		return(false);
	}

	/** Removes a latch from the thread level array if it is found there.
	@param[in]	latch		The latch that was released
	@return true if found in the array; it is not an error if the latch is
	not found, as we presently are not able to determine the level for
	every latch reservation the program does */
	void unlock(const rw_lock_t* latch) UNIV_NOTHROW;

  /** Get the level name
  @param[in]	level		The level ID to lookup
  @return level name */
  static const char *get_level_name(latch_level_t level)
  {
    static_assert(array_elements(level_names) == SYNC_LEVEL_MAX + 1, "size");
    ut_ad(level <= SYNC_LEVEL_MAX);
    return level_names[level];
  }

private:
	/** Adds a latch and its level in the thread level array. Allocates
	the memory for the array if called first time for this OS thread.
	Makes the checks against other latch levels stored in the array
	for this thread.

	@param[in]	latch	 pointer to a mutex or an rw-lock
	@param[in]	level	level in the latching order
	@return the thread's latches */
	Latches* check_order(
		const rw_lock_t*	latch,
		latch_level_t	level)
		UNIV_NOTHROW;

	/** Print the latches acquired by a thread
	@param[in]	latches		Latches acquired by a thread */
	void print_latches(const Latches* latches) const
		UNIV_NOTHROW;

	/** Mutex protecting m_threads */
	mysql_mutex_t		m_mutex;

	/** Thread specific data */
	ThreadMap		m_threads;
};

static LatchDebug latch_debug;

/** Print the latches acquired by a thread
@param[in]	latches		Latches acquired by a thread */
void
LatchDebug::print_latches(const Latches* latches) const
	UNIV_NOTHROW
{
	ib::error() << "Latches already owned by this thread: ";

	Latches::const_iterator	end = latches->end();

	for (Latches::const_iterator it = latches->begin();
	     it != end;
	     ++it) {

		ib::error()
			<< sync_latch_get_name(it->latch->get_id())
			<< " -> "
			<< it->level << " "
			<< "(" << get_level_name(it->level) << ")";
	}
}

/** Report error and abort
@param[in]	latches		thread's existing latches
@param[in]	latched		The existing latch causing the invariant to fail
@param[in]	level		The new level request that breaks the order */
void
LatchDebug::crash(
	const Latches*	latches,
	const Latched*	latched,
	latch_level_t	level) const
	UNIV_NOTHROW
{
	ib::error()
		<< "Thread " << os_thread_pf(os_thread_get_curr_id())
		<< " already owns a latch "
		<< sync_latch_get_name(latched->latch->m_id) << " at level"
		<< " " << latched->level << " ("
		<< get_level_name(latched->level)
		<< " ), which is at a lower/same level than the"
		<< " requested latch: "
		<< level << " (" << get_level_name(level) << "). "
		<< latched->latch->to_string();

	print_latches(latches);

	ut_error;
}

/** Check that all the latches already owned by a thread have a lower
level than limit.
@param[in]	latches		the thread's existing (acquired) latches
@param[in]	limit		to check against
@return latched info if there is one with a level <= limit . */
const Latched*
LatchDebug::less(
	const Latches*	latches,
	latch_level_t	limit) const
	UNIV_NOTHROW
{
	Latches::const_iterator	end = latches->end();

	for (Latches::const_iterator it = latches->begin(); it != end; ++it) {

		if (it->level <= limit) {
			return(&(*it));
		}
	}

	return(NULL);
}

/** Do a basic ordering check.
@param[in]	latches		thread's existing latches
@param[in]	requested_level	Level requested by latch
@param[in]	in_level	declared ulint so that we can do level - 1.
				The level of the latch that the thread is
				trying to acquire
@return true if passes, else crash with error message. */
inline bool
LatchDebug::basic_check(
	const Latches*	latches,
	latch_level_t	requested_level,
	lint		in_level) const
	UNIV_NOTHROW
{
	latch_level_t	level = latch_level_t(in_level);

	ut_ad(level < SYNC_LEVEL_MAX);

	const Latched*	latched = less(latches, level);

	if (latched != NULL) {
		crash(latches, latched, requested_level);
		return(false);
	}

	return(true);
}

/** Create a new instance if one doesn't exist else return the existing one.
@param[in]	add		add an empty entry if one is not found
				(default no)
@return	pointer to a thread's acquired latches. */
Latches*
LatchDebug::thread_latches(bool add)
	UNIV_NOTHROW
{
	mysql_mutex_lock(&m_mutex);

	os_thread_id_t		thread_id = os_thread_get_curr_id();
	ThreadMap::iterator	lb = m_threads.lower_bound(thread_id);

	if (lb != m_threads.end()
	    && !(m_threads.key_comp()(thread_id, lb->first))) {

		Latches*	latches = lb->second;

		mysql_mutex_unlock(&m_mutex);

		return(latches);

	} else if (!add) {
		mysql_mutex_unlock(&m_mutex);
		return(NULL);
	} else {
		typedef ThreadMap::value_type value_type;

		Latches*	latches = UT_NEW_NOKEY(Latches());

		ut_a(latches != NULL);

		latches->reserve(32);

		m_threads.insert(lb, value_type(thread_id, latches));
		mysql_mutex_unlock(&m_mutex);

		return(latches);
	}
}

/** Checks if the level value exists in the thread's acquired latches.
@param[in]	levels		the thread's existing (acquired) latches
@param[in]	level		to lookup
@return	latch if found or 0 */
const rw_lock_t*
LatchDebug::find(
	const Latches*	latches,
	latch_level_t	level) const UNIV_NOTHROW
{
  if (latches)
    for (const Latched &l : *latches)
      if (l.level == level)
        return l.latch;
  return nullptr;
}

/** Checks if the level value exists in the thread's acquired latches.
@param[in]	 level		The level to lookup
@return	latch if found or NULL */
const rw_lock_t*
LatchDebug::find(latch_level_t level)
	UNIV_NOTHROW
{
	return(find(thread_latches(), level));
}

/**
Adds a latch and its level in the thread level array. Allocates the memory
for the array if called first time for this OS thread. Makes the checks
against other latch levels stored in the array for this thread.
@param[in]	latch	pointer to a mutex or an rw-lock
@param[in]	level	level in the latching order
@return the thread's latches */
Latches*
LatchDebug::check_order(
	const rw_lock_t*	latch,
	latch_level_t	level)
	UNIV_NOTHROW
{
	ut_ad(latch->level != SYNC_LEVEL_VARYING);

	Latches*	latches = thread_latches(true);

	/* NOTE that there is a problem with _NODE and _LEAF levels: if the
	B-tree height changes, then a leaf can change to an internal node
	or the other way around. We do not know at present if this can cause
	unnecessary assertion failures below. */

	switch (level) {
#ifdef SAFE_MUTEX
		extern mysql_mutex_t ibuf_mutex;
		extern mysql_mutex_t ibuf_pessimistic_insert_mutex;
		extern mysql_mutex_t ibuf_bitmap_mutex;
#endif /* SAFE_MUTEX */
	case SYNC_NO_ORDER_CHECK:
	case SYNC_EXTERN_STORAGE:
	case SYNC_TREE_NODE_FROM_HASH:
		/* Do no order checking */
		break;

	case SYNC_TRX_SYS_HEADER:

		if (srv_is_being_started) {
			/* This is violated during trx_sys_create_rsegs()
			when creating additional rollback segments when
			upgrading in srv_start(). */
			break;
		}

		/* Fall through */

	case SYNC_SEARCH_SYS:
	case SYNC_PURGE_LATCH:
	case SYNC_DICT_OPERATION:
	case SYNC_DICT_HEADER:
	case SYNC_TRX_I_S_RWLOCK:
		basic_check(latches, level, level);
		break;

	case SYNC_IBUF_BITMAP:

		/* Either the thread must own the master mutex to all
		the bitmap pages, or it is allowed to latch only ONE
		bitmap page. */
		basic_check(latches, level, SYNC_IBUF_BITMAP - 1);
#ifdef SAFE_MUTEX
		if (!srv_is_being_started
		    && !mysql_mutex_is_owner(&ibuf_bitmap_mutex)) {
			/* This is violated during trx_sys_create_rsegs()
			when creating additional rollback segments during
			upgrade. */
			basic_check(latches, level, SYNC_IBUF_BITMAP);
		}
#endif /* SAFE_MUTEX */
		break;

	case SYNC_FSP_PAGE:
		ut_a(find(latches, SYNC_FSP) != 0);
		break;

	case SYNC_FSP:

		ut_a(find(latches, SYNC_FSP) != 0
		     || basic_check(latches, level, SYNC_FSP));
		break;

	case SYNC_TRX_UNDO_PAGE:

		/* Purge is allowed to read in as many UNDO pages as it likes.
		The purge thread can read the UNDO pages without any covering
		mutex. */

		ut_a(basic_check(latches, level, level - 1));
		break;

	case SYNC_RSEG_HEADER:
		break;

	case SYNC_RSEG_HEADER_NEW:

		ut_a(find(latches, SYNC_FSP_PAGE) != 0);
		break;

	case SYNC_TREE_NODE:

		ut_a(find(latches, SYNC_FSP) == &fil_system.temp_space->latch
		     || find(latches, SYNC_INDEX_TREE)
		     || find(latches, SYNC_DICT_OPERATION)
		     || basic_check(latches, level, SYNC_TREE_NODE - 1));
		break;

	case SYNC_TREE_NODE_NEW:

		ut_a(find(latches, SYNC_FSP_PAGE) != 0);
		break;

	case SYNC_INDEX_TREE:

		basic_check(latches, level, SYNC_TREE_NODE - 1);
		break;

	case SYNC_IBUF_TREE_NODE:

		ut_a(find(latches, SYNC_IBUF_INDEX_TREE) != 0
		     || basic_check(latches, level, SYNC_IBUF_TREE_NODE - 1));
		break;

	case SYNC_IBUF_TREE_NODE_NEW:

		/* ibuf_add_free_page() allocates new pages for the change
		buffer while only holding the tablespace x-latch. These
		pre-allocated new pages may only be used while holding
		ibuf_mutex, in btr_page_alloc_for_ibuf(). */
#ifdef SAFE_MUTEX
		ut_a(mysql_mutex_is_owner(&ibuf_mutex)
		     || find(latches, SYNC_FSP) != 0);
#endif
		break;

	case SYNC_IBUF_INDEX_TREE:

		if (find(latches, SYNC_FSP) != 0) {
			basic_check(latches, level, level - 1);
		} else {
			basic_check(latches, level, SYNC_IBUF_TREE_NODE - 1);
		}
		break;

	case SYNC_IBUF_HEADER:

		basic_check(latches, level, SYNC_FSP - 1);
		mysql_mutex_assert_not_owner(&ibuf_mutex);
		mysql_mutex_assert_not_owner(&ibuf_pessimistic_insert_mutex);
		break;

	case SYNC_UNKNOWN:
	case SYNC_LEVEL_VARYING:
	case RW_LOCK_X:
	case RW_LOCK_X_WAIT:
	case RW_LOCK_S:
	case RW_LOCK_SX:
	case RW_LOCK_NOT_LOCKED:
		/* These levels should never be set for a latch. */
		ut_error;
		break;
	}

	return(latches);
}

/** Removes a latch from the thread level array if it is found there.
@param[in]	latch		that was released/unlocked
@param[in]	level		level of the latch
@return true if found in the array; it is not an error if the latch is
not found, as we presently are not able to determine the level for
every latch reservation the program does */
void
LatchDebug::unlock(const rw_lock_t* latch)
	UNIV_NOTHROW
{
	Latches*	latches;

	if (*latch->get_name() == '.') {

		/* Ignore diagnostic latches, starting with '.' */

	} else if ((latches = thread_latches()) != NULL) {

		Latches::reverse_iterator	rend = latches->rend();

		for (Latches::reverse_iterator it = latches->rbegin();
		     it != rend;
		     ++it) {

			if (it->latch != latch) {

				continue;
			}

			Latches::iterator	i = it.base();

			latches->erase(--i);

			/* If this thread doesn't own any more
			latches remove from the map.

			FIXME: Perhaps use the master thread
			to do purge. Or, do it from close connection.
			This could be expensive. */

			if (latches->empty()) {
				os_thread_id_t thread_id;
				thread_id = os_thread_get_curr_id();

				mysql_mutex_lock(&m_mutex);
				m_threads.erase(thread_id);
				mysql_mutex_unlock(&m_mutex);
				UT_DELETE(latches);
			}

			return;
		}

		if (latch->level != SYNC_LEVEL_VARYING) {
			ib::error()
				<< "Couldn't find latch "
				<< sync_latch_get_name(latch->get_id());

			print_latches(latches);

			/** Must find the latch. */
			ut_error;
		}
	}
}

/** Get the latch id from a latch name.
@param[in]	name	Latch name
@return latch id if found else LATCH_ID_NONE. */
latch_id_t
sync_latch_get_id(const char* name)
{
	LatchMetaData::const_iterator	end = latch_meta.end();

	/* Linear scan should be OK, this should be extremely rare. */

	for (LatchMetaData::const_iterator it = latch_meta.begin();
	     it != end;
	     ++it) {

		if (*it == NULL || (*it)->get_id() == LATCH_ID_NONE) {

			continue;

		} else if (strcmp((*it)->get_name(), name) == 0) {

			return((*it)->get_id());
		}
	}

	return(LATCH_ID_NONE);
}

/** Get the latch name from a sync level
@param[in]	level		Latch level to lookup
@return NULL if not found. */
const char*
sync_latch_get_name(latch_level_t level)
{
	LatchMetaData::const_iterator	end = latch_meta.end();

	/* Linear scan should be OK, this should be extremely rare. */

	for (LatchMetaData::const_iterator it = latch_meta.begin();
	     it != end;
	     ++it) {

		if (*it == NULL || (*it)->get_id() == LATCH_ID_NONE) {

			continue;

		} else if ((*it)->get_level() == level) {

			return((*it)->get_name());
		}
	}

	return(0);
}

/** Check if it is OK to acquire the latch. */
void sync_check_lock_validate(const rw_lock_t *latch)
{
  if (srv_sync_debug)
    latch_debug.lock_validate(latch, latch->level);
}

/** Note that the lock has been granted. */
void sync_check_lock_granted(const rw_lock_t *latch)
{
  if (srv_sync_debug)
    latch_debug.lock_granted(latch, latch->level);
}

/** Check if it is OK to acquire the latch.
@param[in]	latch	latch type
@param[in]	level	Latch level */
void sync_check_lock(const rw_lock_t *latch, latch_level_t level)
{
  ut_ad(latch->level == SYNC_LEVEL_VARYING);
  ut_ad(latch->get_id() == LATCH_ID_BUF_BLOCK_LOCK);
  if (srv_sync_debug)
  {
    latch_debug.lock_validate(latch, level);
    latch_debug.lock_granted(latch, level);
  }
}

/** Check if it is OK to re-acquire the lock.
@param[in]	latch		RW-LOCK to relock (recursive X locks) */
void sync_check_relock(const rw_lock_t *latch)
{
  if (srv_sync_debug)
    latch_debug.relock(latch);
}

/** Removes a latch from the thread level array if it is found there.
@param[in]	latch		The latch to unlock */
void sync_check_unlock(const rw_lock_t *latch)
{
  if (srv_sync_debug)
    latch_debug.unlock(latch);
}

/** Checks if the level array for the current thread contains a
mutex or rw-latch at the specified level.
@param[in]	level		to find
@return	a matching latch, or NULL if not found */
const rw_lock_t *sync_check_find(latch_level_t level)
{
  return latch_debug.find(level);
}

/** Iterate over the thread's latches.
@param[in,out]	functor		called for each element.
@return true if the functor returns true for any element */
bool sync_check_iterate(const sync_check_functor_t &functor)
{
  return latch_debug.for_each(functor);
}

/** Acquires the debug mutex. We cannot use the mutex defined in sync0sync,
because the debug mutex is also acquired in sync0arr while holding the OS
mutex protecting the sync array, and the ordinary mutex_enter might
recursively call routines in sync0arr, leading to a deadlock on the OS
mutex. */
void
rw_lock_debug_mutex_enter()
{
  mysql_mutex_lock(&rw_lock_debug_mutex);
}

/** Releases the debug mutex. */
void
rw_lock_debug_mutex_exit()
{
  mysql_mutex_unlock(&rw_lock_debug_mutex);
}
#endif /* UNIV_DEBUG */

/* Meta data for all the InnoDB latches. If the latch is not in recorded
here then it will be be considered for deadlock checks.  */
LatchMetaData	latch_meta;

/** Load the latch meta data. */
static
void
sync_latch_meta_init()
	UNIV_NOTHROW
{
	latch_meta.resize(LATCH_ID_MAX + 1);

	/* The latches should be ordered on latch_id_t. So that we can
	index directly into the vector to update and fetch meta-data. */

	// Add the RW locks
	LATCH_ADD_RWLOCK(BTR_SEARCH, SYNC_SEARCH_SYS, btr_search_latch_key);

	LATCH_ADD_RWLOCK(BUF_BLOCK_LOCK, SYNC_LEVEL_VARYING,
			 buf_block_lock_key);

#ifdef UNIV_DEBUG
	LATCH_ADD_RWLOCK(BUF_BLOCK_DEBUG, SYNC_LEVEL_VARYING,
			 buf_block_debug_latch_key);
#endif /* UNIV_DEBUG */

	LATCH_ADD_RWLOCK(DICT_OPERATION, SYNC_DICT_OPERATION,
			 dict_operation_lock_key);

	LATCH_ADD_RWLOCK(FIL_SPACE, SYNC_FSP, fil_space_latch_key);

	LATCH_ADD_RWLOCK(TRX_I_S_CACHE, SYNC_TRX_I_S_RWLOCK,
			 trx_i_s_cache_lock_key);

	LATCH_ADD_RWLOCK(TRX_PURGE, SYNC_PURGE_LATCH, trx_purge_latch_key);

	LATCH_ADD_RWLOCK(IBUF_INDEX_TREE, SYNC_IBUF_INDEX_TREE,
			 index_tree_rw_lock_key);

	LATCH_ADD_RWLOCK(INDEX_TREE, SYNC_INDEX_TREE, index_tree_rw_lock_key);

	latch_id_t	id = LATCH_ID_NONE;

	/* The array should be ordered on latch ID.We need to
	index directly into it from the mutex policy to update
	the counters and access the meta-data. */

	for (LatchMetaData::iterator it = latch_meta.begin();
	     it != latch_meta.end();
	     ++it) {

		const latch_meta_t*	meta = *it;


		/* Skip blank entries */
		if (meta == NULL || meta->get_id() == LATCH_ID_NONE) {
			continue;
		}

		ut_a(id < meta->get_id());

		id = meta->get_id();
	}
}

/** Destroy the latch meta data */
static
void
sync_latch_meta_destroy()
{
	for (LatchMetaData::iterator it = latch_meta.begin();
	     it != latch_meta.end();
	     ++it) {

		UT_DELETE(*it);
	}

	latch_meta.clear();
}

/** Initializes the synchronization data structures. */
void
sync_check_init()
{
	sync_latch_meta_init();

	/* create the mutex to protect rw_lock list. */

	mysql_mutex_init(rw_lock_list_mutex_key, &rw_lock_list_mutex, nullptr);

	ut_d(mysql_mutex_init(0, &rw_lock_debug_mutex, nullptr);)
	ut_d(latch_debug.init());

	sync_array_init();
}

/** Free the InnoDB synchronization data structures. */
void sync_check_close()
{
  ut_d(mysql_mutex_destroy(&rw_lock_debug_mutex));
  ut_d(latch_debug.close());
  mysql_mutex_destroy(&rw_lock_list_mutex);
  sync_array_close();
  sync_latch_meta_destroy();
}
