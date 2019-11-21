/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2019, MariaDB Corporation.

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
#include "ut0lst.h"

#ifndef lock0types_h
#define lock0types_h

#define lock_t ib_lock_t

struct lock_t;
struct lock_sys_t;
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
	LOCK_NONE_UNSET = 255
};

/** Convert the given enum value into string.
@param[in]	mode	the lock mode
@return human readable string of the given enum value */
inline
const char* lock_mode_string(enum lock_mode mode)
{
	switch (mode) {
	case LOCK_IS:
		return("LOCK_IS");
	case LOCK_IX:
		return("LOCK_IX");
	case LOCK_S:
		return("LOCK_S");
	case LOCK_X:
		return("LOCK_X");
	case LOCK_AUTO_INC:
		return("LOCK_AUTO_INC");
	case LOCK_NONE:
		return("LOCK_NONE");
	case LOCK_NONE_UNSET:
		return("LOCK_NONE_UNSET");
	default:
		ut_error;
	}
}

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
	ib_uint32_t	space;		/*!< space id */
	ib_uint32_t	page_no;	/*!< page number */
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
inline
std::ostream& lock_rec_t::print(std::ostream& out) const
{
	out << "[lock_rec_t: space=" << space << ", page_no=" << page_no
		<< ", n_bits=" << n_bits << "]";
	return(out);
}

inline
std::ostream&
operator<<(std::ostream& out, const lock_rec_t& lock)
{
	return(lock.print(out));
}

#define LOCK_MODE_MASK	0xFUL	/*!< mask used to extract mode from the
				type_mode field in a lock */
/** Lock types */
/* @{ */
#define LOCK_TABLE	16U	/*!< table lock */
#define	LOCK_REC	32U	/*!< record lock */
#define LOCK_TYPE_MASK	0xF0UL	/*!< mask used to extract lock type from the
				type_mode field in a lock */
#if LOCK_MODE_MASK & LOCK_TYPE_MASK
# error "LOCK_MODE_MASK & LOCK_TYPE_MASK"
#endif

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

/** Lock struct; protected by lock_sys->mutex */
struct ib_lock_t
{
	trx_t*		trx;		/*!< transaction owning the
					lock */
	UT_LIST_NODE_T(ib_lock_t)
			trx_locks;	/*!< list of the locks of the
					transaction */

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

	/** Determine if the lock object is a record lock.
	@return true if record lock, false otherwise. */
	bool is_record_lock() const
	{
		return(type() == LOCK_REC);
	}

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

	bool is_insert_intention() const
	{
		return(type_mode & LOCK_INSERT_INTENTION);
	}

	ulint type() const {
		return(type_mode & LOCK_TYPE_MASK);
	}

	enum lock_mode mode() const
	{
		return(static_cast<enum lock_mode>(type_mode & LOCK_MODE_MASK));
	}

	/** Print the lock object into the given output stream.
	@param[in,out]	out	the output stream
	@return the given output stream. */
	std::ostream& print(std::ostream& out) const;

	/** Convert the member 'type_mode' into a human readable string.
	@return human readable string */
	std::string type_mode_string() const;

	const char* type_string() const
	{
		switch (type_mode & LOCK_TYPE_MASK) {
		case LOCK_REC:
			return("LOCK_REC");
		case LOCK_TABLE:
			return("LOCK_TABLE");
		default:
			ut_error;
		}
	}
};

typedef UT_LIST_BASE_NODE_T(ib_lock_t) trx_lock_list_t;

#endif /* lock0types_h */
