/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2021, MariaDB Corporation.

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
@file include/lock0types.h
The transaction lock system global types

Created 5/7/1996 Heikki Tuuri
*******************************************************/

#include "dict0types.h"
#include "buf0types.h"
#include "ut0lst.h"

#ifndef lock0types_h
#define lock0types_h

#define lock_t ib_lock_t

struct lock_t;
struct lock_table_t;

/* Basic lock modes */
enum lock_mode {
	LOCK_IS = 0,	/* intention shared */
	LOCK_IX,	/* intention exclusive */
	LOCK_S,		/* shared */
	LOCK_X,		/* exclusive */
	LOCK_AUTO_INC,	/* locks the auto-inc counter of a table
			in an exclusive mode */
	LOCK_NONE,	/* this is used elsewhere to note consistent read */
	LOCK_NUM = LOCK_NONE, /* number of lock modes */
	LOCK_NONE_UNSET = 7
};

/** A table lock */
struct lock_table_t {
	dict_table_t*	table;		/*!< database table in dictionary
					cache */
	UT_LIST_NODE_T(ib_lock_t)
			locks;		/*!< list of locks on the same
					table */
	/** Print the table lock into the given output stream
	@param[in,out]	out	the output stream
	@return the given output stream. */
	std::ostream& print(std::ostream& out) const;
};

/** Record lock for a page */
struct lock_rec_t {
	/** page identifier */
	page_id_t	page_id;
	ib_uint32_t	n_bits;		/*!< number of bits in the lock
					bitmap; NOTE: the lock bitmap is
					placed immediately after the
					lock struct */

	/** Print the record lock into the given output stream
	@param[in,out]	out	the output stream
	@return the given output stream. */
	std::ostream& print(std::ostream& out) const;
};

/** Print the record lock into the given output stream
@param[in,out]	out	the output stream
@return the given output stream. */
inline std::ostream &lock_rec_t::print(std::ostream &out) const
{
  out << "[lock_rec_t: space=" << page_id.space()
      << ", page_no=" << page_id.page_no()
      << ", n_bits=" << n_bits << "]";
  return out;
}

inline
std::ostream&
operator<<(std::ostream& out, const lock_rec_t& lock)
{
	return(lock.print(out));
}

#define LOCK_MODE_MASK	0x7	/*!< mask used to extract mode from the
				type_mode field in a lock */
/** Lock types */
/* @{ */
/** table lock (record lock if the flag is not set) */
#define LOCK_TABLE	8U

#define LOCK_WAIT	256U	/*!< Waiting lock flag; when set, it
				means that the lock has not yet been
				granted, it is just waiting for its
				turn in the wait queue */
/* Precise modes */
#define LOCK_ORDINARY	0	/*!< this flag denotes an ordinary
				next-key lock in contrast to LOCK_GAP
				or LOCK_REC_NOT_GAP */
#define LOCK_GAP	512U	/*!< when this bit is set, it means that the
				lock holds only on the gap before the record;
				for instance, an x-lock on the gap does not
				give permission to modify the record on which
				the bit is set; locks of this type are created
				when records are removed from the index chain
				of records */
#define LOCK_REC_NOT_GAP 1024U	/*!< this bit means that the lock is only on
				the index record and does NOT block inserts
				to the gap before the index record; this is
				used in the case when we retrieve a record
				with a unique key, and is also used in
				locking plain SELECTs (not part of UPDATE
				or DELETE) when the user has set the READ
				COMMITTED isolation level */
#define LOCK_INSERT_INTENTION 2048U/*!< this bit is set when we place a waiting
				gap type record lock request in order to let
				an insert of an index record to wait until
				there are no conflicting locks by other
				transactions on the gap; note that this flag
				remains set when the waiting lock is granted,
				or if the lock is inherited to a neighboring
				record */
#define LOCK_PREDICATE	8192U	/*!< Predicate lock */
#define LOCK_PRDT_PAGE	16384U	/*!< Page lock */


#if (LOCK_WAIT|LOCK_GAP|LOCK_REC_NOT_GAP|LOCK_INSERT_INTENTION|LOCK_PREDICATE|LOCK_PRDT_PAGE)&LOCK_MODE_MASK
# error
#endif
#if (LOCK_WAIT|LOCK_GAP|LOCK_REC_NOT_GAP|LOCK_INSERT_INTENTION|LOCK_PREDICATE|LOCK_PRDT_PAGE)&LOCK_TYPE_MASK
# error
#endif
/* @} */

/**
Checks if the `mode` is LOCK_S or LOCK_X (possibly ORed with LOCK_WAIT or
LOCK_REC) which means the lock is a
Next Key Lock, a.k.a. LOCK_ORDINARY, as opposed to Predicate Lock,
GAP lock, Insert Intention or Record Lock.
@param  mode  A mode and flags, of a lock.
@return true if the only bits set in `mode` are LOCK_S or LOCK_X and optionally
LOCK_WAIT or LOCK_REC */
static inline bool lock_mode_is_next_key_lock(ulint mode)
{
  static_assert(LOCK_ORDINARY == 0, "LOCK_ORDINARY must be 0 (no flags)");
  ut_ad((mode & LOCK_TABLE) == 0);
  mode&= ~LOCK_WAIT;
  ut_ad((mode & LOCK_WAIT) == 0);
  ut_ad(((mode & ~(LOCK_MODE_MASK)) == LOCK_ORDINARY) ==
        (mode == LOCK_S || mode == LOCK_X));
  return (mode & ~(LOCK_MODE_MASK)) == LOCK_ORDINARY;
}

/** Lock struct; protected by lock_sys.latch */
struct ib_lock_t
{
  /** the owner of the lock */
  trx_t *trx;
  /** other locks of the transaction; protected by
  lock_sys.is_writer() and trx->mutex_is_owner(); @see trx_lock_t::trx_locks */
  UT_LIST_NODE_T(ib_lock_t) trx_locks;

	dict_index_t*	index;		/*!< index for a record lock */

	ib_lock_t*	hash;		/*!< hash chain node for a record
					lock. The link node in a singly linked
					list, used during hashing. */

	/** time(NULL) of the lock request creation.
	Used for computing wait_time and diagnostics only.
	Note: bogus durations may be reported
	when the system time is adjusted! */
	time_t		requested_time;
	/** Cumulated wait time in seconds.
	Note: may be bogus when the system time is adjusted! */
	ulint		wait_time;

	union {
		lock_table_t	tab_lock;/*!< table lock */
		lock_rec_t	rec_lock;/*!< record lock */
	} un_member;			/*!< lock details */

	ib_uint32_t	type_mode;	/*!< lock type, mode, LOCK_GAP or
					LOCK_REC_NOT_GAP,
					LOCK_INSERT_INTENTION,
					wait flag, ORed */

	bool is_waiting() const
	{
		return(type_mode & LOCK_WAIT);
	}

	bool is_gap() const
	{
		return(type_mode & LOCK_GAP);
	}

	bool is_record_not_gap() const
	{
		return(type_mode & LOCK_REC_NOT_GAP);
	}

	/** @return true if the lock is a Next Key Lock */
	bool is_next_key_lock() const
	{
		return !(type_mode & LOCK_TABLE) &&
		       lock_mode_is_next_key_lock(type_mode);
	}

	bool is_insert_intention() const
	{
		return(type_mode & LOCK_INSERT_INTENTION);
	}

	bool is_table() const { return type_mode & LOCK_TABLE; }

	enum lock_mode mode() const
	{
		return(static_cast<enum lock_mode>(type_mode & LOCK_MODE_MASK));
	}

        bool is_rec_granted_exclusive_not_gap() const
        {
          return (type_mode & (LOCK_MODE_MASK | LOCK_GAP)) == LOCK_X;
        }

	/** Print the lock object into the given output stream.
	@param[in,out]	out	the output stream
	@return the given output stream. */
	std::ostream& print(std::ostream& out) const;

	const char* type_string() const
	{ return is_table() ? "LOCK_TABLE" : "LOCK_REC"; }
};

typedef UT_LIST_BASE_NODE_T(ib_lock_t) trx_lock_list_t;

#endif /* lock0types_h */
