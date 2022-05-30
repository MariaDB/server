/*****************************************************************************

Copyright (c) 2014, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2022, MariaDB Corporation.

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
@file lock/lock0prdt.cc
The transaction lock system

Created 9/7/2013 Jimmy Yang
*******************************************************/

#define LOCK_MODULE_IMPLEMENTATION

#include "lock0lock.h"
#include "lock0priv.h"
#include "lock0prdt.h"
#include "dict0mem.h"
#include "que0que.h"

/*********************************************************************//**
Get a minimum bounding box from a Predicate
@return	the minimum bounding box */
UNIV_INLINE
rtr_mbr_t*
prdt_get_mbr_from_prdt(
/*===================*/
	const lock_prdt_t*	prdt)	/*!< in: the lock predicate */
{
	rtr_mbr_t*	mbr_loc = reinterpret_cast<rtr_mbr_t*>(prdt->data);

	return(mbr_loc);
}

/*********************************************************************//**
Get a predicate from a lock
@return	the predicate */
lock_prdt_t*
lock_get_prdt_from_lock(
/*====================*/
	const lock_t*	lock)	/*!< in: the lock */
{
	lock_prdt_t*	prdt = reinterpret_cast<lock_prdt_t*>(
				&((reinterpret_cast<byte*>(
					const_cast<lock_t*>(&lock[1])))[
						UNIV_WORD_SIZE]));

	return(prdt);
}

/*********************************************************************//**
Get a minimum bounding box directly from a lock
@return	the minimum bounding box*/
UNIV_INLINE
rtr_mbr_t*
lock_prdt_get_mbr_from_lock(
/*========================*/
	const lock_t*	lock)	/*!< in: the lock */
{
	ut_ad(lock->type_mode & LOCK_PREDICATE);

	lock_prdt_t*	prdt = lock_get_prdt_from_lock(lock);

	rtr_mbr_t*	mbr_loc = prdt_get_mbr_from_prdt(prdt);

	return(mbr_loc);
}

/*********************************************************************//**
Append a predicate to the lock */
void
lock_prdt_set_prdt(
/*===============*/
	lock_t*			lock,	/*!< in: lock */
	const lock_prdt_t*	prdt)	/*!< in: Predicate */
{
	ut_ad(lock->type_mode & LOCK_PREDICATE);

	memcpy(&(((byte*) &lock[1])[UNIV_WORD_SIZE]), prdt, sizeof *prdt);
}


/** Check whether two predicate locks are compatible with each other
@param[in]	prdt1	first predicate lock
@param[in]	prdt2	second predicate lock
@param[in]	op	predicate comparison operator
@return	true if consistent */
static
bool
lock_prdt_consistent(
	lock_prdt_t*	prdt1,
	lock_prdt_t*	prdt2,
	ulint		op)
{
	bool		ret = false;
	rtr_mbr_t*	mbr1 = prdt_get_mbr_from_prdt(prdt1);
	rtr_mbr_t*	mbr2 = prdt_get_mbr_from_prdt(prdt2);
	ulint		action;

	if (op) {
		action = op;
	} else {
		if (prdt2->op != 0 && (prdt1->op != prdt2->op)) {
			return(false);
		}

		action = prdt1->op;
	}

	switch (action) {
	case PAGE_CUR_CONTAIN:
		ret = MBR_CONTAIN_CMP(mbr1, mbr2);
		break;
	case PAGE_CUR_DISJOINT:
		ret = MBR_DISJOINT_CMP(mbr1, mbr2);
		break;
	case PAGE_CUR_MBR_EQUAL:
		ret = MBR_EQUAL_CMP(mbr1, mbr2);
		break;
	case PAGE_CUR_INTERSECT:
		ret = MBR_INTERSECT_CMP(mbr1, mbr2);
		break;
	case PAGE_CUR_WITHIN:
		ret = MBR_WITHIN_CMP(mbr1, mbr2);
		break;
	default:
		ib::error() << "invalid operator " << action;
		ut_error;
	}

	return(ret);
}

/*********************************************************************//**
Checks if a predicate lock request for a new lock has to wait for
another lock.
@return	true if new lock has to wait for lock2 to be released */
bool
lock_prdt_has_to_wait(
/*==================*/
	const trx_t*	trx,	/*!< in: trx of new lock */
	unsigned	type_mode,/*!< in: precise mode of the new lock
				to set: LOCK_S or LOCK_X, possibly
				ORed to LOCK_PREDICATE or LOCK_PRDT_PAGE,
				LOCK_INSERT_INTENTION */
	lock_prdt_t*	prdt,	/*!< in: lock predicate to check */
	const lock_t*	lock2)	/*!< in: another record lock; NOTE that
				it is assumed that this has a lock bit
				set on the same record as in the new
				lock we are setting */
{
	lock_prdt_t*	cur_prdt = lock_get_prdt_from_lock(lock2);

	ut_ad(trx && lock2);
	ut_ad((lock2->type_mode & LOCK_PREDICATE && type_mode & LOCK_PREDICATE)
	      || (lock2->type_mode & LOCK_PRDT_PAGE
		  && type_mode & LOCK_PRDT_PAGE));

	ut_ad(type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE));

	if (trx != lock2->trx
	    && !lock_mode_compatible(static_cast<lock_mode>(
			             LOCK_MODE_MASK & type_mode),
				     lock2->mode())) {

		/* If it is a page lock, then return true (conflict) */
		if (type_mode & LOCK_PRDT_PAGE) {
			ut_ad(lock2->type_mode & LOCK_PRDT_PAGE);

			return(true);
		}

		/* Predicate lock does not conflicts with non-predicate lock */
		if (!(lock2->type_mode & LOCK_PREDICATE)) {
			return(FALSE);
		}

		ut_ad(lock2->type_mode & LOCK_PREDICATE);

		if (!(type_mode & LOCK_INSERT_INTENTION)) {
			/* PREDICATE locks without LOCK_INSERT_INTENTION flag
			do not need to wait for anything. This is because
			different users can have conflicting lock types
			on predicates. */

			return(FALSE);
		}

		if (lock2->type_mode & LOCK_INSERT_INTENTION) {

			/* No lock request needs to wait for an insert
			intention lock to be removed. This makes it similar
			to GAP lock, that allows conflicting insert intention
			locks */
			return(FALSE);
		}

		if (!lock_prdt_consistent(cur_prdt, prdt, 0)) {
			return(false);
		}

		return(TRUE);
	}

	return(FALSE);
}

/*********************************************************************//**
Checks if a transaction has a GRANTED stronger or equal predicate lock
on the page
@return	lock or NULL */
UNIV_INLINE
lock_t*
lock_prdt_has_lock(
/*===============*/
	ulint			precise_mode,	/*!< in: LOCK_S or LOCK_X */
	hash_cell_t&		cell,		/*!< hash table cell of id */
	const page_id_t		id,		/*!< in: page identifier */
	lock_prdt_t*		prdt,		/*!< in: The predicate to be
						attached to the new lock */
	const trx_t*		trx)		/*!< in: transaction */
{
	ut_ad((precise_mode & LOCK_MODE_MASK) == LOCK_S
	      || (precise_mode & LOCK_MODE_MASK) == LOCK_X);
	ut_ad(!(precise_mode & LOCK_INSERT_INTENTION));

	for (lock_t*lock= lock_sys_t::get_first(cell, id, PRDT_HEAPNO);
	     lock;
	     lock = lock_rec_get_next(PRDT_HEAPNO, lock)) {
		ut_ad(lock->type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE));

		if (lock->trx == trx
		    && !(lock->type_mode & (LOCK_INSERT_INTENTION | LOCK_WAIT))
		    && lock_mode_stronger_or_eq(
			    lock->mode(),
			    static_cast<lock_mode>(
				    precise_mode & LOCK_MODE_MASK))) {
			if (lock->type_mode & LOCK_PRDT_PAGE) {
				return(lock);
			}

			lock_prdt_t*	cur_prdt = lock_get_prdt_from_lock(
							lock);

			/* if the lock predicate operator is the same
			as the one to look, and prdicate test is successful,
			then we find a lock */
			if (cur_prdt->op == prdt->op
			    && lock_prdt_consistent(cur_prdt, prdt, 0)) {

				return(lock);
			}
		}
	}

	return(NULL);
}

/*********************************************************************//**
Checks if some other transaction has a conflicting predicate
lock request in the queue, so that we have to wait.
@return	lock or NULL */
static
lock_t*
lock_prdt_other_has_conflicting(
/*============================*/
	unsigned		mode,	/*!< in: LOCK_S or LOCK_X,
					possibly ORed to LOCK_PREDICATE or
					LOCK_PRDT_PAGE, LOCK_INSERT_INTENTION */
	const hash_cell_t&	cell,	/*!< in: hash table cell */
	const page_id_t		id,	/*!< in: page identifier */
	lock_prdt_t*		prdt,    /*!< in: Predicates (currently)
					the Minimum Bounding Rectangle)
					the new lock will be on */
	const trx_t*		trx)	/*!< in: our transaction */
{
	for (lock_t* lock = lock_sys_t::get_first(cell, id, PRDT_HEAPNO);
	     lock != NULL;
	     lock = lock_rec_get_next(PRDT_HEAPNO, lock)) {

		if (lock->trx == trx) {
			continue;
		}

		if (lock_prdt_has_to_wait(trx, mode, prdt, lock)) {
			return(lock);
		}
	}

	return(NULL);
}

/*********************************************************************//**
Reset the Minimum Bounding Rectangle (to a large area) */
static
void
lock_prdt_enlarge_mbr(
/*==================*/
	const lock_t*	lock,	/*!< in/out: lock to modify */
	rtr_mbr_t*	mbr)    /*!< in: Minimum Bounding Rectangle */
{
	rtr_mbr_t*	cur_mbr = lock_prdt_get_mbr_from_lock(lock);

	if (cur_mbr->xmin > mbr->xmin) {
		cur_mbr->xmin = mbr->xmin;
	}

	if (cur_mbr->ymin > mbr->ymin) {
		cur_mbr->ymin = mbr->ymin;
	}

	if (cur_mbr->xmax < mbr->xmax) {
		cur_mbr->xmax = mbr->xmax;
	}

	if (cur_mbr->ymax < mbr->ymax) {
		cur_mbr->ymax = mbr->ymax;
	}
}

/*********************************************************************//**
Reset the predicates to a "covering" (larger) predicates */
static
void
lock_prdt_enlarge_prdt(
/*===================*/
	lock_t*		lock,	/*!< in/out: lock to modify */
	lock_prdt_t*	prdt)	/*!< in: predicate */
{
	rtr_mbr_t*	mbr = prdt_get_mbr_from_prdt(prdt);

	lock_prdt_enlarge_mbr(lock, mbr);
}

/*********************************************************************//**
Check two predicates' MBRs are the same
@return	true if they are the same */
static
bool
lock_prdt_is_same(
/*==============*/
	lock_prdt_t*	prdt1,		/*!< in: MBR with the lock */
	lock_prdt_t*	prdt2)		/*!< in: MBR with the lock */
{
	rtr_mbr_t*	mbr1 = prdt_get_mbr_from_prdt(prdt1);
	rtr_mbr_t*	mbr2 = prdt_get_mbr_from_prdt(prdt2);

	if (prdt1->op == prdt2->op && MBR_EQUAL_CMP(mbr1, mbr2)) {
		return(true);
	}

	return(false);
}

/*********************************************************************//**
Looks for a similar predicate lock struct by the same trx on the same page.
This can be used to save space when a new record lock should be set on a page:
no new struct is needed, if a suitable old one is found.
@return	lock or NULL */
static
lock_t*
lock_prdt_find_on_page(
/*===================*/
	unsigned		type_mode,	/*!< in: lock type_mode field */
	const buf_block_t*	block,		/*!< in: buffer block */
	lock_prdt_t*		prdt,		/*!< in: MBR with the lock */
	const trx_t*		trx)		/*!< in: transaction */
{
	const page_id_t id{block->page.id()};
	hash_cell_t& cell = *lock_sys.hash_get(type_mode).cell_get(id.fold());

	for (lock_t *lock = lock_sys_t::get_first(cell, id);
	     lock != NULL;
	     lock = lock_rec_get_next_on_page(lock)) {

		if (lock->trx == trx
		    && lock->type_mode == type_mode) {
			if (lock->type_mode & LOCK_PRDT_PAGE) {
				return(lock);
			}

			ut_ad(lock->type_mode & LOCK_PREDICATE);

			if (lock_prdt_is_same(lock_get_prdt_from_lock(lock),
					      prdt)) {
				return(lock);
			}
		}
	}

	return(NULL);
}

/*********************************************************************//**
Adds a predicate lock request in the predicate lock queue.
@return	lock where the bit was set */
static
lock_t*
lock_prdt_add_to_queue(
/*===================*/
	unsigned		type_mode,/*!< in: lock mode, wait, predicate
					etc. flags */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	dict_index_t*		index,	/*!< in: index of record */
	trx_t*			trx,	/*!< in/out: transaction */
	lock_prdt_t*		prdt,	/*!< in: Minimum Bounding Rectangle
					the new lock will be on */
	bool			caller_owns_trx_mutex)
					/*!< in: TRUE if caller owns the
					transaction mutex */
{
	ut_ad(caller_owns_trx_mutex == trx->mutex_is_owner());
	ut_ad(index->is_spatial());
	ut_ad(!dict_index_is_online_ddl(index));
	ut_ad(type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE));

#ifdef UNIV_DEBUG
	switch (type_mode & LOCK_MODE_MASK) {
	case LOCK_X:
	case LOCK_S:
		break;
	default:
		ut_error;
	}
#endif /* UNIV_DEBUG */

	/* Try to extend a similar non-waiting lock on the same page */
	if (!(type_mode & LOCK_WAIT)) {
		const page_id_t id{block->page.id()};
		hash_cell_t& cell = *lock_sys.hash_get(type_mode).
			cell_get(id.fold());

		for (lock_t* lock = lock_sys_t::get_first(cell, id);
		     lock; lock = lock_rec_get_next_on_page(lock)) {
			if (lock->is_waiting()
			    && lock->type_mode
			    & (LOCK_PREDICATE | LOCK_PRDT_PAGE)
			    && lock_rec_get_nth_bit(lock, PRDT_HEAPNO)) {
				goto create;
			}
		}

		if (lock_t* lock = lock_prdt_find_on_page(type_mode, block,
							  prdt, trx)) {
			if (lock->type_mode & LOCK_PREDICATE) {
				lock_prdt_enlarge_prdt(lock, prdt);
			}

			return lock;
		}
	}

create:
	/* Note: We will not pass any conflicting lock to lock_rec_create(),
	because we should be moving an existing waiting lock request. */
	ut_ad(!(type_mode & LOCK_WAIT) || trx->lock.wait_trx);

	lock_t* lock = lock_rec_create(nullptr,
				       type_mode, block, PRDT_HEAPNO, index,
				       trx, caller_owns_trx_mutex);

	if (lock->type_mode & LOCK_PREDICATE) {
		lock_prdt_set_prdt(lock, prdt);
	}

	return lock;
}

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate insert of
a predicate record.
@return	DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_prdt_insert_check_and_lock(
/*============================*/
	const rec_t*	rec,	/*!< in: record after which to insert */
	buf_block_t*	block,	/*!< in/out: buffer block of rec */
	dict_index_t*	index,	/*!< in: index */
	que_thr_t*	thr,	/*!< in: query thread */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	lock_prdt_t*	prdt)	/*!< in: Predicates with Minimum Bound
				Rectangle */
{
  ut_ad(block->page.frame == page_align(rec));
  ut_ad(!index->table->is_temporary());
  ut_ad(index->is_spatial());

  trx_t *trx= thr_get_trx(thr);
  const page_id_t id{block->page.id()};
  dberr_t err= DB_SUCCESS;

  {
    LockGuard g{lock_sys.prdt_hash, id};
    /* Because this code is invoked for a running transaction by
    the thread that is serving the transaction, it is not necessary
    to hold trx->mutex here. */
    ut_ad(lock_table_has(trx, index->table, LOCK_IX));

    /* Only need to check locks on prdt_hash */
    if (ut_d(lock_t *lock=) lock_sys_t::get_first(g.cell(), id, PRDT_HEAPNO))
    {
      ut_ad(lock->type_mode & LOCK_PREDICATE);

      /* If another transaction has an explicit lock request which locks
      the predicate, waiting or granted, on the successor, the insert
      has to wait.

      Similar to GAP lock, we do not consider lock from inserts conflicts
      with each other */

      const ulint mode= LOCK_X | LOCK_PREDICATE | LOCK_INSERT_INTENTION;
      lock_t *c_lock= lock_prdt_other_has_conflicting(mode, g.cell(), id,
                                                      prdt, trx);

      if (c_lock)
      {
        rtr_mbr_t *mbr= prdt_get_mbr_from_prdt(prdt);
        trx->mutex_lock();
        /* Allocate MBR on the lock heap */
        lock_init_prdt_from_mbr(prdt, mbr, 0, trx->lock.lock_heap);
        err= lock_rec_enqueue_waiting(c_lock, mode, id, block->page.frame,
                                      PRDT_HEAPNO, index, thr, prdt);
        trx->mutex_unlock();
      }
    }
  }

  if (err == DB_SUCCESS)
    /* Update the page max trx id field */
    page_update_max_trx_id(block, buf_block_get_page_zip(block), trx->id, mtr);

  return err;
}

/**************************************************************//**
Check whether any predicate lock in parent needs to propagate to
child page after split. */
void
lock_prdt_update_parent(
/*====================*/
        buf_block_t*    left_block,	/*!< in/out: page to be split */
        buf_block_t*    right_block,	/*!< in/out: the new half page */
        lock_prdt_t*	left_prdt,	/*!< in: MBR on the old page */
        lock_prdt_t*	right_prdt,	/*!< in: MBR on the new page */
	const page_id_t	page_id)	/*!< in: parent page */
{
	auto fold= page_id.fold();
	LockMutexGuard g{SRW_LOCK_CALL};
	hash_cell_t& cell = *lock_sys.prdt_hash.cell_get(fold);

	/* Get all locks in parent */
	for (lock_t *lock = lock_sys_t::get_first(cell, page_id);
	     lock;
	     lock = lock_rec_get_next_on_page(lock)) {
		lock_prdt_t*	lock_prdt;
		ulint		op = PAGE_CUR_DISJOINT;

		ut_ad(lock);

		if (!(lock->type_mode & LOCK_PREDICATE)
		    || (lock->type_mode & LOCK_MODE_MASK) == LOCK_X) {
			continue;
		}

		lock_prdt = lock_get_prdt_from_lock(lock);

		/* Check each lock in parent to see if it intersects with
		left or right child */
		if (!lock_prdt_consistent(lock_prdt, left_prdt, op)
		    && !lock_prdt_find_on_page(lock->type_mode, left_block,
					       lock_prdt, lock->trx)) {
			lock_prdt_add_to_queue(lock->type_mode,
					       left_block, lock->index,
					       lock->trx, lock_prdt,
					       false);
		}

		if (!lock_prdt_consistent(lock_prdt, right_prdt, op)
		    && !lock_prdt_find_on_page(lock->type_mode, right_block,
					       lock_prdt, lock->trx)) {
			lock_prdt_add_to_queue(lock->type_mode, right_block,
					       lock->index, lock->trx,
					       lock_prdt, false);
		}
	}
}

/**************************************************************//**
Update predicate lock when page splits */
static
void
lock_prdt_update_split_low(
/*=======================*/
	buf_block_t*	new_block,	/*!< in/out: the new half page */
	lock_prdt_t*	prdt,		/*!< in: MBR on the old page */
	lock_prdt_t*	new_prdt,	/*!< in: MBR on the new page */
	const page_id_t	id,		/*!< in: page number */
	unsigned	type_mode)	/*!< in: LOCK_PREDICATE or
					LOCK_PRDT_PAGE */
{
	hash_cell_t& cell = *lock_sys.hash_get(type_mode).cell_get(id.fold());

	for (lock_t* lock = lock_sys_t::get_first(cell, id);
	     lock;
	     lock = lock_rec_get_next_on_page(lock)) {
		/* First dealing with Page Lock */
		if (lock->type_mode & LOCK_PRDT_PAGE) {
			/* Duplicate the lock to new page */
			lock_prdt_add_to_queue(lock->type_mode,
					       new_block,
					       lock->index,
					       lock->trx, nullptr, false);
			continue;
		}

		/* Now dealing with Predicate Lock */
		lock_prdt_t*	lock_prdt;
		ulint		op = PAGE_CUR_DISJOINT;

		ut_ad(lock->type_mode & LOCK_PREDICATE);

		/* No need to duplicate waiting X locks */
		if ((lock->type_mode & LOCK_MODE_MASK) == LOCK_X) {
			continue;
		}

		lock_prdt = lock_get_prdt_from_lock(lock);

		if (!lock_prdt_consistent(lock_prdt, new_prdt, op)) {
			/* Move the lock to new page */
			lock_prdt_add_to_queue(lock->type_mode, new_block,
					       lock->index, lock->trx,
					       lock_prdt, false);
		}
	}
}

/**************************************************************//**
Update predicate lock when page splits */
void
lock_prdt_update_split(
/*===================*/
	buf_block_t*	new_block,	/*!< in/out: the new half page */
	lock_prdt_t*	prdt,		/*!< in: MBR on the old page */
	lock_prdt_t*	new_prdt,	/*!< in: MBR on the new page */
	const page_id_t	page_id)	/*!< in: page number */
{
	LockMutexGuard g{SRW_LOCK_CALL};
	lock_prdt_update_split_low(new_block, prdt, new_prdt,
				   page_id, LOCK_PREDICATE);

	lock_prdt_update_split_low(new_block, NULL, NULL,
				   page_id, LOCK_PRDT_PAGE);
}

/*********************************************************************//**
Initiate a Predicate Lock from a MBR */
void
lock_init_prdt_from_mbr(
/*====================*/
	lock_prdt_t*	prdt,	/*!< in/out: predicate to initialized */
	rtr_mbr_t*	mbr,	/*!< in: Minimum Bounding Rectangle */
	ulint		mode,	/*!< in: Search mode */
	mem_heap_t*	heap)	/*!< in: heap for allocating memory */
{
	memset(prdt, 0, sizeof(*prdt));

	if (heap != NULL) {
		prdt->data = mem_heap_dup(heap, mbr, sizeof *mbr);
	} else {
		prdt->data = static_cast<void*>(mbr);
	}

	prdt->op = static_cast<uint16>(mode);
}

/*********************************************************************//**
Acquire a predicate lock on a block
@return	DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_prdt_lock(
/*===========*/
	buf_block_t*	block,	/*!< in/out: buffer block of rec */
	lock_prdt_t*	prdt,	/*!< in: Predicate for the lock */
	dict_index_t*	index,	/*!< in: secondary index */
	lock_mode	mode,	/*!< in: mode of the lock which
				the read cursor should set on
				records: LOCK_S or LOCK_X; the
				latter is possible in
				SELECT FOR UPDATE */
	unsigned	type_mode,
				/*!< in: LOCK_PREDICATE or LOCK_PRDT_PAGE */
	que_thr_t*	thr)	/*!< in: query thread
				(can be NULL if BTR_NO_LOCKING_FLAG) */
{
	trx_t*		trx = thr_get_trx(thr);
	dberr_t		err = DB_SUCCESS;
	lock_rec_req_status	status = LOCK_REC_SUCCESS;

	if (trx->read_only || index->table->is_temporary()) {
		return(DB_SUCCESS);
	}

	ut_ad(!dict_index_is_clust(index));
	ut_ad(!dict_index_is_online_ddl(index));
	ut_ad(type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE));

	auto& hash = lock_sys.prdt_hash_get(type_mode != LOCK_PREDICATE);
	const page_id_t id{block->page.id()};

	/* Another transaction cannot have an implicit lock on the record,
	because when we come here, we already have modified the clustered
	index record, and this would not have been possible if another active
	transaction had modified this secondary index record. */

	LockGuard g{hash, id};

	const unsigned	prdt_mode = type_mode | mode;
	lock_t*		lock = lock_sys_t::get_first(g.cell(), id);

	if (lock == NULL) {
		lock = lock_rec_create(
			NULL,
			prdt_mode, block, PRDT_HEAPNO,
			index, trx, FALSE);

		status = LOCK_REC_SUCCESS_CREATED;
	} else {
		if (lock_rec_get_next_on_page(lock)
		    || lock->trx != trx
		    || lock->type_mode != prdt_mode
		    || lock_rec_get_n_bits(lock) == 0
		    || ((type_mode & LOCK_PREDICATE)
		        && (!lock_prdt_consistent(
				lock_get_prdt_from_lock(lock), prdt, 0)))) {
			trx->mutex_lock();

			lock = lock_prdt_has_lock(
				mode, g.cell(), id, prdt, trx);

			if (lock) {
			} else if (lock_t* wait_for
				   = lock_prdt_other_has_conflicting(
					   prdt_mode, g.cell(), id, prdt,
					   trx)) {
				err = lock_rec_enqueue_waiting(
					wait_for, prdt_mode, id,
					block->page.frame, PRDT_HEAPNO,
					index, thr, prdt);
			} else {
				lock_prdt_add_to_queue(
					prdt_mode, block, index, trx,
					prdt, true);
			}

			trx->mutex_unlock();
		} else {
			if (!lock_rec_get_nth_bit(lock, PRDT_HEAPNO)) {
				lock_rec_set_nth_bit(lock, PRDT_HEAPNO);
				status = LOCK_REC_SUCCESS_CREATED;
			}
		}
	}

	if (status == LOCK_REC_SUCCESS_CREATED && type_mode == LOCK_PREDICATE) {
		/* Append the predicate in the lock record */
		lock_prdt_set_prdt(lock, prdt);
	}

	return(err);
}

/*********************************************************************//**
Acquire a "Page" lock on a block
@return	DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_place_prdt_page_lock(
	const page_id_t	page_id,	/*!< in: page identifier */
	dict_index_t*	index,		/*!< in: secondary index */
	que_thr_t*	thr)		/*!< in: query thread */
{
	ut_ad(thr != NULL);
	ut_ad(!high_level_read_only);

	ut_ad(index->is_spatial());
	ut_ad(!dict_index_is_online_ddl(index));

	/* Another transaction cannot have an implicit lock on the record,
	because when we come here, we already have modified the clustered
	index record, and this would not have been possible if another active
	transaction had modified this secondary index record. */

	LockGuard g{lock_sys.prdt_page_hash, page_id};

	const lock_t*	lock = lock_sys_t::get_first(g.cell(), page_id);
	const ulint	mode = LOCK_S | LOCK_PRDT_PAGE;
	trx_t*		trx = thr_get_trx(thr);

	if (lock != NULL) {
		/* Find a matching record lock owned by this transaction. */

		while (lock != NULL && lock->trx != trx) {
			lock = lock_rec_get_next_on_page_const(lock);
		}

		ut_ad(lock == NULL || lock->type_mode == mode);
		ut_ad(lock == NULL || lock_rec_get_n_bits(lock) != 0);
	}

	if (lock == NULL) {
		lock = lock_rec_create_low(
			NULL,
			mode, page_id, NULL, PRDT_HEAPNO,
			index, trx, FALSE);

#ifdef PRDT_DIAG
		printf("GIS_DIAGNOSTIC: page lock %d\n", (int) page_no);
#endif /* PRDT_DIAG */
	}

	return(DB_SUCCESS);
}

/** Check whether there are R-tree Page lock on a page
@param[in]	trx	trx to test the lock
@param[in]	page_id	page identifier
@return	true if there is none */
bool lock_test_prdt_page_lock(const trx_t *trx, const page_id_t page_id)
{
  LockGuard g{lock_sys.prdt_page_hash, page_id};
  lock_t *lock= lock_sys_t::get_first(g.cell(), page_id);
  return !lock || trx == lock->trx;
}

/*************************************************************//**
Moves the locks of a page to another page and resets the lock bits of
the donating records. */
void
lock_prdt_rec_move(
/*===============*/
	const buf_block_t*	receiver,	/*!< in: buffer block containing
						the receiving record */
	const page_id_t		donator)	/*!< in: target page */
{
	LockMultiGuard g{lock_sys.prdt_hash, receiver->page.id(), donator};

	for (lock_t *lock = lock_sys_t::get_first(g.cell2(), donator,
						  PRDT_HEAPNO);
	     lock;
	     lock = lock_rec_get_next(PRDT_HEAPNO, lock)) {

		const auto type_mode = lock->type_mode;
		lock_prdt_t*	lock_prdt = lock_get_prdt_from_lock(lock);

		lock_rec_reset_nth_bit(lock, PRDT_HEAPNO);
		if (type_mode & LOCK_WAIT) {
			ut_ad(lock->trx->lock.wait_lock == lock);
			lock->type_mode &= ~LOCK_WAIT;
		}
		lock_prdt_add_to_queue(
			type_mode, receiver, lock->index, lock->trx,
			lock_prdt, false);
	}
}

/** Remove locks on a discarded SPATIAL INDEX page.
@param id   page to be discarded
@param page whether to discard also from lock_sys.prdt_hash */
void lock_sys_t::prdt_page_free_from_discard(const page_id_t id, bool all)
{
  const auto id_fold= id.fold();
  rd_lock(SRW_LOCK_CALL);
  auto cell= prdt_page_hash.cell_get(id_fold);
  auto latch= hash_table::latch(cell);
  latch->acquire();

  for (lock_t *lock= get_first(*cell, id), *next; lock; lock= next)
  {
    next= lock_rec_get_next_on_page(lock);
    lock_rec_discard(prdt_page_hash, lock);
  }

  if (all)
  {
    latch->release();
    cell= prdt_hash.cell_get(id_fold);
    latch= hash_table::latch(cell);
    latch->acquire();
    for (lock_t *lock= get_first(*cell, id), *next; lock; lock= next)
    {
      next= lock_rec_get_next_on_page(lock);
      lock_rec_discard(prdt_hash, lock);
    }
  }

  latch->release();
  cell= rec_hash.cell_get(id_fold);
  latch= hash_table::latch(cell);
  latch->acquire();

  for (lock_t *lock= get_first(*cell, id), *next; lock; lock= next)
  {
    next= lock_rec_get_next_on_page(lock);
    lock_rec_discard(rec_hash, lock);
  }

  latch->release();
  /* Must be last, to avoid a race with lock_sys_t::hash_table::resize() */
  rd_unlock();
}
