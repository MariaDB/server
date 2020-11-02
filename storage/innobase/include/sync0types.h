/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
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

/**************************************************//**
@file include/sync0types.h
Global types for sync

Created 9/5/1995 Heikki Tuuri
*******************************************************/

#pragma once
#include "my_atomic_wrapper.h"
#include <vector>

#include "ut0new.h"
#undef rw_lock_t
struct rw_lock_t;

/*
		LATCHING ORDER WITHIN THE DATABASE
		==================================

The mutex or latch in the central memory object, for instance, a rollback
segment object, must be acquired before acquiring the latch or latches to
the corresponding file data structure. In the latching order below, these
file page object latches are placed immediately below the corresponding
central memory object latch or mutex.

Synchronization object			Notes
----------------------			-----

Dictionary mutex			If we have a pointer to a dictionary
|					object, e.g., a table, it can be
|					accessed without reserving the
|					dictionary mutex. We must have a
|					reservation, a memoryfix, to the
|					appropriate table object in this case,
|					and the table must be explicitly
|					released later.
V
Dictionary header
|
V
Secondary index tree latch		The tree latch protects also all
|					the B-tree non-leaf pages. These
V					can be read with the page only
Secondary index non-leaf		bufferfixed to save CPU time,
|					no s-latch is needed on the page.
|					Modification of a page requires an
|					x-latch on the page, however. If a
|					thread owns an x-latch to the tree,
|					it is allowed to latch non-leaf pages
|					even after it has acquired the fsp
|					latch.
V
Secondary index leaf			The latch on the secondary index leaf
|					can be kept while accessing the
|					clustered index, to save CPU time.
V
Clustered index tree latch		To increase concurrency, the tree
|					latch is usually released when the
|					leaf page latch has been acquired.
V
Clustered index non-leaf
|
V
Clustered index leaf
|
V
Transaction system header
|
V
Rollback segment mutex			The rollback segment mutex must be
|					reserved, if, e.g., a new page must
|					be added to an undo log. The rollback
|					segment and the undo logs in its
|					history list can be seen as an
|					analogue of a B-tree, and the latches
|					reserved similarly, using a version of
|					lock-coupling. If an undo log must be
|					extended by a page when inserting an
|					undo log record, this corresponds to
|					a pessimistic insert in a B-tree.
V
Rollback segment header
|
V
Purge system latch
|
V
Undo log pages				If a thread owns the trx undo mutex,
|					or for a log in the history list, the
|					rseg mutex, it is allowed to latch
|					undo log pages in any order, and even
|					after it has acquired the fsp latch.
|					If a thread does not have the
|					appropriate mutex, it is allowed to
|					latch only a single undo log page in
|					a mini-transaction.
V
File space management latch		If a mini-transaction must allocate
|					several file pages, it can do that,
|					because it keeps the x-latch to the
|					file space management in its memo.
V
File system pages
|
V
lock_sys.wait_mutex			Mutex protecting lock timeout data
|
V
lock_sys.mutex				Mutex protecting lock_sys_t
|
V
trx_sys.mutex				Mutex protecting trx_sys.trx_list
|
V
Threads mutex				Background thread scheduling mutex
|
V
query_thr_mutex				Mutex protecting query threads
|
V
trx_mutex				Mutex protecting trx_t fields
|
V
Search system mutex
|
V
Buffer pool mutex
|
V
Log mutex
|
Any other latch
|
V
Memory pool mutex */

/** Latching order levels. If you modify these, you have to also update
LatchDebug internals in sync0debug.cc */

enum latch_level_t {
	SYNC_UNKNOWN = 0,

	RW_LOCK_SX,
	RW_LOCK_X_WAIT,
	RW_LOCK_S,
	RW_LOCK_X,
	RW_LOCK_NOT_LOCKED,

	SYNC_SEARCH_SYS,

	SYNC_TRX_SYS_HEADER,

	SYNC_IBUF_BITMAP,
	SYNC_IBUF_TREE_NODE,
	SYNC_IBUF_TREE_NODE_NEW,
	SYNC_IBUF_INDEX_TREE,

	SYNC_FSP_PAGE,
	SYNC_FSP,
	SYNC_EXTERN_STORAGE,
	SYNC_TRX_UNDO_PAGE,
	SYNC_RSEG_HEADER,
	SYNC_RSEG_HEADER_NEW,
	SYNC_PURGE_LATCH,
	SYNC_TREE_NODE,
	SYNC_TREE_NODE_FROM_HASH,
	SYNC_TREE_NODE_NEW,
	SYNC_INDEX_TREE,

	SYNC_IBUF_HEADER,
	SYNC_DICT_HEADER,

	SYNC_DICT_OPERATION,

	SYNC_TRX_I_S_RWLOCK,

	/** Level is varying. Only used with buffer pool page locks, which
	do not have a fixed level, but instead have their level set after
	the page is locked; see e.g.  ibuf_bitmap_get_map_page(). */

	SYNC_LEVEL_VARYING,

	/** This can be used to suppress order checking. */
	SYNC_NO_ORDER_CHECK,

	/** Maximum level value */
	SYNC_LEVEL_MAX = SYNC_NO_ORDER_CHECK
};

/** Each latch has an ID. This id is used for creating the latch and to look
up its meta-data. See sync0debug.cc. */
enum latch_id_t {
	LATCH_ID_NONE = 0,
	LATCH_ID_BTR_SEARCH,
	LATCH_ID_BUF_BLOCK_LOCK,
	LATCH_ID_BUF_BLOCK_DEBUG,
	LATCH_ID_DICT_OPERATION,
	LATCH_ID_FIL_SPACE,
	LATCH_ID_TRX_I_S_CACHE,
	LATCH_ID_TRX_PURGE,
	LATCH_ID_IBUF_INDEX_TREE,
	LATCH_ID_INDEX_TREE,
	LATCH_ID_DICT_TABLE_STATS,
	LATCH_ID_MAX = LATCH_ID_DICT_TABLE_STATS
};

#ifdef UNIV_PFS_MUTEX
#ifdef UNIV_PFS_RWLOCK
/** Latch element.
Used for rwlocks which have PFS keys defined under UNIV_PFS_RWLOCK.
@param[in]	id		Latch id
@param[in]	level		Latch level
@param[in]	key		PFS key */
# define LATCH_ADD_RWLOCK(id, level, key)	latch_meta[LATCH_ID_ ## id] =\
	UT_NEW_NOKEY(latch_meta_t(LATCH_ID_ ## id, #id, level, #level, key))
#else
# define LATCH_ADD_RWLOCK(id, level, key)	latch_meta[LATCH_ID_ ## id] =\
	UT_NEW_NOKEY(latch_meta_t(LATCH_ID_ ## id, #id, level, #level,	     \
		     PSI_NOT_INSTRUMENTED))
#endif /* UNIV_PFS_RWLOCK */

#else
# define LATCH_ADD_RWLOCK(id, level, key)	latch_meta[LATCH_ID_ ## id] =\
	UT_NEW_NOKEY(latch_meta_t(LATCH_ID_ ## id, #id, level, #level))
#endif /* UNIV_PFS_MUTEX */

/** Default latch counter */
class LatchCounter {

public:
	/** The counts we collect for a mutex */
	struct Count {

		/** Constructor */
		Count()
			UNIV_NOTHROW
			:
			m_spins(),
			m_waits(),
			m_calls(),
			m_enabled()
		{
			/* No op */
		}

		/** Rest the values to zero */
		void reset()
			UNIV_NOTHROW
		{
			m_spins = 0;
			m_waits = 0;
			m_calls = 0;
		}

		/** Number of spins trying to acquire the latch. */
		uint32_t	m_spins;

		/** Number of waits trying to acquire the latch */
		uint32_t	m_waits;

		/** Number of times it was called */
		uint32_t	m_calls;

		/** true if enabled */
		bool		m_enabled;
	};

	/** Constructor */
	LatchCounter() { mysql_mutex_init(0, &m_mutex, nullptr); }

	/** Destructor */
	~LatchCounter()
		UNIV_NOTHROW
	{
		mysql_mutex_destroy(&m_mutex);
		for (Count *count : m_counters) UT_DELETE(count);
	}

	/** Reset all counters to zero. It is not protected by any
	mutex and we don't care about atomicity. Unless it is a
	demonstrated problem. The information collected is not
	required for the correct functioning of the server. */
	void reset()
		UNIV_NOTHROW
	{
		mysql_mutex_lock(&m_mutex);
		for (Count *count : m_counters) count->reset();
		mysql_mutex_unlock(&m_mutex);
	}

	/** @return the aggregate counter */
	Count* sum_register()
		UNIV_NOTHROW
	{
		mysql_mutex_lock(&m_mutex);

		Count*	count;

		if (m_counters.empty()) {
			count = UT_NEW_NOKEY(Count());
			m_counters.push_back(count);
		} else {
			ut_a(m_counters.size() == 1);
			count = m_counters[0];
		}

		mysql_mutex_unlock(&m_mutex);

		return(count);
	}

	/** Register a single instance counter */
	void single_register(Count* count)
		UNIV_NOTHROW
	{
		mysql_mutex_lock(&m_mutex);
		m_counters.push_back(count);
		mysql_mutex_unlock(&m_mutex);
	}

	/** Deregister a single instance counter
	@param[in]	count		The count instance to deregister */
	void single_deregister(Count* count)
		UNIV_NOTHROW
	{
		mysql_mutex_lock(&m_mutex);
		m_counters.erase(
			std::remove(
				m_counters.begin(),
				m_counters.end(), count),
			m_counters.end());
		mysql_mutex_unlock(&m_mutex);
	}

	/** Iterate over the counters */
	template<typename C> void iterate(const C& callback) UNIV_NOTHROW
	{
		mysql_mutex_lock(&m_mutex);
		for (Count *count : m_counters) callback(count);
		mysql_mutex_unlock(&m_mutex);
	}

	/** Disable the monitoring */
	void enable()
		UNIV_NOTHROW
	{
		mysql_mutex_lock(&m_mutex);
		for (Count *count : m_counters) count->m_enabled = true;
		m_active = true;
		mysql_mutex_unlock(&m_mutex);
	}

	/** Disable the monitoring */
	void disable()
		UNIV_NOTHROW
	{
		mysql_mutex_lock(&m_mutex);
		for (Count *count : m_counters) count->m_enabled = false;
		m_active = false;
		mysql_mutex_unlock(&m_mutex);
	}

	/** @return if monitoring is active */
	bool is_enabled() const { return m_active; }

	LatchCounter(const LatchCounter&) = delete;
	LatchCounter& operator=(const LatchCounter&) = delete;

	/** Mutex protecting m_counters */
	mysql_mutex_t m_mutex;

	/** Counters for the latches */
	std::vector<Count*> m_counters;

	/** if true then we collect the data */
	Atomic_relaxed<bool> m_active= false;
};

/** Latch meta data */
template <typename Counter = LatchCounter>
class LatchMeta {

public:
	typedef Counter CounterType;

#ifdef UNIV_PFS_MUTEX
	typedef	mysql_pfs_key_t	pfs_key_t;
#endif /* UNIV_PFS_MUTEX */

	/** Constructor */
	LatchMeta()
		:
		m_id(LATCH_ID_NONE),
		m_name(),
		m_level(SYNC_UNKNOWN),
		m_level_name()
#ifdef UNIV_PFS_MUTEX
		,m_pfs_key()
#endif /* UNIV_PFS_MUTEX */
	{
	}

	/** Destructor */
	~LatchMeta() { }

	/** Constructor
	@param[in]	id		Latch id
	@param[in]	name		Latch name
	@param[in]	level		Latch level
	@param[in]	level_name	Latch level text representation
	@param[in]	key		PFS key */
	LatchMeta(
		latch_id_t	id,
		const char*	name,
		latch_level_t	level,
		const char*	level_name
#ifdef UNIV_PFS_MUTEX
		,pfs_key_t	key
#endif /* UNIV_PFS_MUTEX */
	      )
		:
		m_id(id),
		m_name(name),
		m_level(level),
		m_level_name(level_name)
#ifdef UNIV_PFS_MUTEX
		,m_pfs_key(key)
#endif /* UNIV_PFS_MUTEX */
	{
		/* No op */
	}

	/* Less than operator.
	@param[in]	rhs		Instance to compare against
	@return true if this.get_id() < rhs.get_id() */
	bool operator<(const LatchMeta& rhs) const
	{
		return(get_id() < rhs.get_id());
	}

	/** @return the latch id */
	latch_id_t get_id() const
	{
		return(m_id);
	}

	/** @return the latch name */
	const char* get_name() const
	{
		return(m_name);
	}

	/** @return the latch level */
	latch_level_t get_level() const
	{
		return(m_level);
	}

	/** @return the latch level name */
	const char* get_level_name() const
	{
		return(m_level_name);
	}

#ifdef UNIV_PFS_MUTEX
	/** @return the PFS key for the latch */
	pfs_key_t get_pfs_key() const
	{
		return(m_pfs_key);
	}
#endif /* UNIV_PFS_MUTEX */

	/** @return the counter instance */
	Counter* get_counter()
	{
		return(&m_counter);
	}

private:
	/** Latch id */
	latch_id_t		m_id;

	/** Latch name */
	const char*		m_name;

	/** Latch level in the ordering */
	latch_level_t		m_level;

	/** Latch level text representation */
	const char*		m_level_name;

#ifdef UNIV_PFS_MUTEX
	/** PFS key */
	pfs_key_t		m_pfs_key;
#endif /* UNIV_PFS_MUTEX */

	/** For gathering latch statistics */
	Counter			m_counter;
};

typedef LatchMeta<LatchCounter> latch_meta_t;
typedef std::vector<latch_meta_t*, ut_allocator<latch_meta_t*> > LatchMetaData;

/** Note: This is accessed without any mutex protection. It is initialised
at startup and elements should not be added to or removed from it after
that.  See sync_latch_meta_init() */
extern LatchMetaData	latch_meta;

/** Get the latch meta-data from the latch ID
@param[in]	id		Latch ID
@return the latch meta data */
inline
latch_meta_t&
sync_latch_get_meta(latch_id_t id)
{
	ut_ad(static_cast<size_t>(id) < latch_meta.size());
	ut_ad(id == latch_meta[id]->get_id());

	return(*latch_meta[id]);
}

/** Fetch the counter for the latch
@param[in]	id		Latch ID
@return the latch counter */
inline
latch_meta_t::CounterType*
sync_latch_get_counter(latch_id_t id)
{
	latch_meta_t&	meta = sync_latch_get_meta(id);

	return(meta.get_counter());
}

/** Get the latch name from the latch ID
@param[in]	id		Latch ID
@return the name, will assert if not found */
inline
const char*
sync_latch_get_name(latch_id_t id)
{
	ut_ad(id != LATCH_ID_NONE);

	const latch_meta_t&	meta = sync_latch_get_meta(id);

	return(meta.get_name());
}

/** Get the latch ordering level
@param[in]	id		Latch id to lookup
@return the latch level */
inline
latch_level_t
sync_latch_get_level(latch_id_t id)
{
	ut_ad(id != LATCH_ID_NONE);

	const latch_meta_t&	meta = sync_latch_get_meta(id);

	return(meta.get_level());
}

#ifdef UNIV_PFS_MUTEX
/** Get the latch PFS key from the latch ID
@param[in]	id		Latch ID
@return the PFS key */
inline
mysql_pfs_key_t
sync_latch_get_pfs_key(latch_id_t id)
{
	const latch_meta_t&	meta = sync_latch_get_meta(id);

	return(meta.get_pfs_key());
}
#endif

/** Get the latch name from a sync level
@param[in]	level		Latch level to lookup
@return 0 if not found. */
const char*
sync_latch_get_name(latch_level_t level);

/** Print the filename "basename"
@return the basename */
const char*
sync_basename(const char* filename);

#ifdef UNIV_DEBUG
/** Subclass this to iterate over a thread's acquired latch levels. */
struct sync_check_functor_t {
	virtual ~sync_check_functor_t() { }
	virtual bool operator()(const latch_level_t) const = 0;
};

/** Check that no latch is being held.
@tparam	some_allowed	whether some latches are allowed to be held */
template<bool some_allowed = false>
struct sync_checker : public sync_check_functor_t
{
	/** Check the latching constraints
	@param[in]	level		The level held by the thread
	@return whether a latch violation was detected */
	bool operator()(const latch_level_t level) const override
	{
		if (some_allowed) {
			switch (level) {
			case SYNC_FSP:
			case SYNC_DICT_OPERATION:
			case SYNC_NO_ORDER_CHECK:
				return(false);
			default:
				return(true);
			}
		}

		return(true);
	}
};

/** The strict latch checker (no InnoDB latches may be held) */
typedef struct sync_checker<false> sync_check;
/** The sloppy latch checker (can hold InnoDB dictionary or SQL latches) */
typedef struct sync_checker<true> dict_sync_check;

/** Functor to check for given latching constraints. */
struct sync_allowed_latches : public sync_check_functor_t {

	/** Constructor
	@param[in]	from	first element in an array of latch_level_t
	@param[in]	to	last element in an array of latch_level_t */
	sync_allowed_latches(
		const latch_level_t*	from,
		const latch_level_t*	to)
		: begin(from), end(to) { }

	/** Checks whether the given rw_lock_t violates the latch constraint.
	This object maintains a list of allowed latch levels, and if the given
	latch belongs to a latch level that is not there in the allowed list,
	then it is a violation.

	@param[in]	latch	The latch level to check
	@return true if there is a latch violation */
	bool operator()(const latch_level_t level) const override
	{
		return(std::find(begin, end, level) == end);
	}

private:
	/** First element in an array of allowed latch levels */
	const latch_level_t* const begin;
	/** First element after the end of the array of allowed latch levels */
	const latch_level_t* const end;
};

/** Get the latch id from a latch name.
@param[in]	id	Latch name
@return LATCH_ID_NONE. */
latch_id_t
sync_latch_get_id(const char* name);

typedef ulint rw_lock_flags_t;

/* Flags to specify lock types for rw_lock_own_flagged() */
enum rw_lock_flag_t {
	RW_LOCK_FLAG_S  = 1 << 0,
	RW_LOCK_FLAG_X  = 1 << 1,
	RW_LOCK_FLAG_SX = 1 << 2
};

#endif /* UNIV_DBEUG */

/** Simple non-atomic counter aligned to CACHE_LINE_SIZE
@tparam	Type	the integer type of the counter */
template <typename Type>
struct MY_ALIGNED(CPU_LEVEL1_DCACHE_LINESIZE) simple_counter
{
	/** Increment the counter */
	Type inc() { return add(1); }
	/** Decrement the counter */
	Type dec() { return add(Type(~0)); }

	/** Add to the counter
	@param[in]	i	amount to be added
	@return	the value of the counter after adding */
	Type add(Type i) { return m_counter += i; }

	/** @return the value of the counter */
	operator Type() const { return m_counter; }

private:
	/** The counter */
	Type	m_counter;
};
