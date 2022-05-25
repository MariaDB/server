/*****************************************************************************

Copyright (c) 1996, 2022, Oracle and/or its affiliates.
Copyright (c) 2014, 2022, MariaDB Corporation.

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
@file lock/lock0lock.cc
The transaction lock system

Created 5/7/1996 Heikki Tuuri
*******************************************************/

#define LOCK_MODULE_IMPLEMENTATION

#include "univ.i"

#include <mysql/service_thd_error_context.h>
#include <mysql/service_thd_wait.h>
#include <sql_class.h>

#include "lock0lock.h"
#include "lock0priv.h"
#include "dict0mem.h"
#include "trx0purge.h"
#include "trx0sys.h"
#include "ut0vec.h"
#include "btr0cur.h"
#include "row0sel.h"
#include "row0mysql.h"
#include "row0vers.h"
#include "pars0pars.h"
#include "srv0mon.h"

#include <set>

#ifdef WITH_WSREP
#include <mysql/service_wsrep.h>
#include <debug_sync.h>
#endif /* WITH_WSREP */

/** The value of innodb_deadlock_detect */
my_bool innodb_deadlock_detect;
/** The value of innodb_deadlock_report */
ulong innodb_deadlock_report;

#ifdef HAVE_REPLICATION
extern "C" void thd_rpl_deadlock_check(MYSQL_THD thd, MYSQL_THD other_thd);
extern "C" int thd_need_wait_reports(const MYSQL_THD thd);
extern "C" int thd_need_ordering_with(const MYSQL_THD thd, const MYSQL_THD other_thd);
#endif

/** Functor for accessing the embedded node within a table lock. */
struct TableLockGetNode
{
  ut_list_node<lock_t> &operator()(lock_t &elem)
  { return(elem.un_member.tab_lock.locks); }
};

/** Create the hash table.
@param n  the lower bound of n_cells */
void lock_sys_t::hash_table::create(ulint n)
{
  n_cells= ut_find_prime(n);
  const size_t size= pad(n_cells) * sizeof *array;
  void* v= aligned_malloc(size, CPU_LEVEL1_DCACHE_LINESIZE);
  memset(v, 0, size);
  array= static_cast<hash_cell_t*>(v);
}

/** Resize the hash table.
@param n  the lower bound of n_cells */
void lock_sys_t::hash_table::resize(ulint n)
{
  ut_ad(lock_sys.is_writer());
  ulint new_n_cells= ut_find_prime(n);
  const size_t size= pad(new_n_cells) * sizeof *array;
  void* v= aligned_malloc(size, CPU_LEVEL1_DCACHE_LINESIZE);
  memset(v, 0, size);
  hash_cell_t *new_array= static_cast<hash_cell_t*>(v);

  for (auto i= pad(n_cells); i--; )
  {
    if (lock_t *lock= static_cast<lock_t*>(array[i].node))
    {
      /* all hash_latch must vacated */
      ut_ad(i % (ELEMENTS_PER_LATCH + LATCH) >= LATCH);
      do
      {
        ut_ad(!lock->is_table());
        hash_cell_t *c= calc_hash(lock->un_member.rec_lock.page_id.fold(),
                                  new_n_cells) + new_array;
        lock_t *next= lock->hash;
        lock->hash= nullptr;
        if (!c->node)
          c->node= lock;
        else if (!lock->is_waiting())
        {
          lock->hash= static_cast<lock_t*>(c->node);
          c->node= lock;
        }
        else
        {
          lock_t *next= static_cast<lock_t*>(c->node);
          while (next->hash)
            next= next->hash;
          next->hash= lock;
        }
        lock= next;
      }
      while (lock);
    }
  }

  aligned_free(array);
  array= new_array;
  n_cells= new_n_cells;
}

#ifdef SUX_LOCK_GENERIC
void lock_sys_t::hash_latch::wait()
{
  pthread_mutex_lock(&lock_sys.hash_mutex);
  while (!write_trylock())
    pthread_cond_wait(&lock_sys.hash_cond, &lock_sys.hash_mutex);
  pthread_mutex_unlock(&lock_sys.hash_mutex);
}

void lock_sys_t::hash_latch::release()
{
  pthread_mutex_lock(&lock_sys.hash_mutex);
  write_unlock();
  pthread_cond_signal(&lock_sys.hash_cond);
  pthread_mutex_unlock(&lock_sys.hash_mutex);
}
#endif

#ifdef UNIV_DEBUG
/** Assert that a lock shard is exclusively latched by this thread */
void lock_sys_t::assert_locked(const lock_t &lock) const
{
  ut_ad(this == &lock_sys);
  if (is_writer())
    return;
  if (lock.is_table())
    assert_locked(*lock.un_member.tab_lock.table);
  else
    lock_sys.hash_get(lock.type_mode).
      assert_locked(lock.un_member.rec_lock.page_id);
}

/** Assert that a table lock shard is exclusively latched by this thread */
void lock_sys_t::assert_locked(const dict_table_t &table) const
{
  ut_ad(!table.is_temporary());
  if (is_writer())
    return;
  ut_ad(readers);
  ut_ad(table.lock_mutex_is_owner());
}

/** Assert that hash cell for page is exclusively latched by this thread */
void lock_sys_t::hash_table::assert_locked(const page_id_t id) const
{
  if (lock_sys.is_writer())
    return;
  ut_ad(lock_sys.readers);
  ut_ad(latch(cell_get(id.fold()))->is_locked());
}

/** Assert that a hash table cell is exclusively latched (by some thread) */
void lock_sys_t::assert_locked(const hash_cell_t &cell) const
{
  if (is_writer())
    return;
  ut_ad(lock_sys.readers);
  ut_ad(hash_table::latch(const_cast<hash_cell_t*>(&cell))->is_locked());
}
#endif

LockGuard::LockGuard(lock_sys_t::hash_table &hash, page_id_t id)
{
  const auto id_fold= id.fold();
  lock_sys.rd_lock(SRW_LOCK_CALL);
  cell_= hash.cell_get(id_fold);
  hash.latch(cell_)->acquire();
}

LockMultiGuard::LockMultiGuard(lock_sys_t::hash_table &hash,
                               const page_id_t id1, const page_id_t id2)
{
  ut_ad(id1.space() == id2.space());
  const auto id1_fold= id1.fold(), id2_fold= id2.fold();
  lock_sys.rd_lock(SRW_LOCK_CALL);
  cell1_= hash.cell_get(id1_fold);
  cell2_= hash.cell_get(id2_fold);

  auto latch1= hash.latch(cell1_), latch2= hash.latch(cell2_);
  if (latch1 > latch2)
    std::swap(latch1, latch2);
  latch1->acquire();
  if (latch1 != latch2)
    latch2->acquire();
}

LockMultiGuard::~LockMultiGuard()
{
  auto latch1= lock_sys_t::hash_table::latch(cell1_),
    latch2= lock_sys_t::hash_table::latch(cell2_);
  latch1->release();
  if (latch1 != latch2)
    latch2->release();
  /* Must be last, to avoid a race with lock_sys_t::hash_table::resize() */
  lock_sys.rd_unlock();
}

TRANSACTIONAL_TARGET
TMLockGuard::TMLockGuard(lock_sys_t::hash_table &hash, page_id_t id)
{
  const auto id_fold= id.fold();
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
  if (xbegin())
  {
    if (lock_sys.latch.is_write_locked())
      xabort();
    cell_= hash.cell_get(id_fold);
    if (hash.latch(cell_)->is_locked())
      xabort();
    elided= true;
    return;
  }
  elided= false;
#endif
  lock_sys.rd_lock(SRW_LOCK_CALL);
  cell_= hash.cell_get(id_fold);
  hash.latch(cell_)->acquire();
}

/** Pretty-print a table lock.
@param[in,out]	file	output stream
@param[in]	lock	table lock */
static void lock_table_print(FILE* file, const lock_t* lock);

/** Pretty-print a record lock.
@param[in,out]	file	output stream
@param[in]	lock	record lock
@param[in,out]	mtr	mini-transaction for accessing the record */
static void lock_rec_print(FILE* file, const lock_t* lock, mtr_t& mtr);

namespace Deadlock
{
  /** Whether to_check may be nonempty */
  static Atomic_relaxed<bool> to_be_checked;
  /** Transactions to check for deadlock. Protected by lock_sys.wait_mutex. */
  static std::set<trx_t*> to_check;

  MY_ATTRIBUTE((nonnull, warn_unused_result))
  /** Check if a lock request results in a deadlock.
  Resolve a deadlock by choosing a transaction that will be rolled back.
  @param trx    transaction requesting a lock
  @return whether trx must report DB_DEADLOCK */
  static bool check_and_resolve(trx_t *trx);

  /** Quickly detect a deadlock using Brent's cycle detection algorithm.
  @param trx     transaction that is waiting for another transaction
  @return a transaction that is part of a cycle
  @retval nullptr if no cycle was found */
  inline trx_t *find_cycle(trx_t *trx)
  {
    mysql_mutex_assert_owner(&lock_sys.wait_mutex);
    trx_t *tortoise= trx, *hare= trx;
    for (unsigned power= 1, l= 1; (hare= hare->lock.wait_trx) != nullptr; l++)
    {
      if (tortoise == hare)
      {
        ut_ad(l > 1);
        lock_sys.deadlocks++;
        /* Note: Normally, trx should be part of any deadlock cycle
        that is found. However, if innodb_deadlock_detect=OFF had been
        in effect in the past, it is possible that trx will be waiting
        for a transaction that participates in a pre-existing deadlock
        cycle. In that case, our victim will not be trx. */
        return hare;
      }
      if (l == power)
      {
        /* The maximum concurrent number of TRX_STATE_ACTIVE transactions
        is TRX_RSEG_N_SLOTS * 128, or innodb_page_size / 16 * 128
        (default: 131,072, maximum: 524,288).
        Our maximum possible number of iterations should be twice that. */
        power<<= 1;
        l= 0;
        tortoise= hare;
      }
    }
    return nullptr;
  }
};

#ifdef UNIV_DEBUG
/** Validate the transactional locks. */
static void lock_validate();

/** Validate the record lock queues on a page.
@param block    buffer pool block
@param latched  whether the tablespace latch may be held
@return true if ok */
static bool lock_rec_validate_page(const buf_block_t *block, bool latched)
  MY_ATTRIBUTE((nonnull, warn_unused_result));
#endif /* UNIV_DEBUG */

/* The lock system */
lock_sys_t lock_sys;

/** Only created if !srv_read_only_mode. Protected by lock_sys.latch. */
static FILE *lock_latest_err_file;

/*********************************************************************//**
Reports that a transaction id is insensible, i.e., in the future. */
ATTRIBUTE_COLD
void
lock_report_trx_id_insanity(
/*========================*/
	trx_id_t	trx_id,		/*!< in: trx id */
	const rec_t*	rec,		/*!< in: user record */
	dict_index_t*	index,		/*!< in: index */
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec, index) */
	trx_id_t	max_trx_id)	/*!< in: trx_sys.get_max_trx_id() */
{
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!rec_is_metadata(rec, *index));

	ib::error()
		<< "Transaction id " << ib::hex(trx_id)
		<< " associated with record" << rec_offsets_print(rec, offsets)
		<< " in index " << index->name
		<< " of table " << index->table->name
		<< " is greater than the global counter " << max_trx_id
		<< "! The table is corrupted.";
}

/*********************************************************************//**
Checks that a transaction id is sensible, i.e., not in the future.
@return true if ok */
bool
lock_check_trx_id_sanity(
/*=====================*/
	trx_id_t	trx_id,		/*!< in: trx id */
	const rec_t*	rec,		/*!< in: user record */
	dict_index_t*	index,		/*!< in: index */
	const rec_offs*	offsets)	/*!< in: rec_get_offsets(rec, index) */
{
  ut_ad(rec_offs_validate(rec, index, offsets));
  ut_ad(!rec_is_metadata(rec, *index));

  trx_id_t max_trx_id= trx_sys.get_max_trx_id();
  ut_ad(max_trx_id || srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN);

  if (UNIV_LIKELY(max_trx_id != 0) && UNIV_UNLIKELY(trx_id >= max_trx_id))
  {
    lock_report_trx_id_insanity(trx_id, rec, index, offsets, max_trx_id);
    return false;
  }
  return true;
}


/**
  Creates the lock system at database start.

  @param[in] n_cells number of slots in lock hash table
*/
void lock_sys_t::create(ulint n_cells)
{
  ut_ad(this == &lock_sys);
  ut_ad(!is_initialised());

  m_initialised= true;

  latch.SRW_LOCK_INIT(lock_latch_key);
#ifdef __aarch64__
  mysql_mutex_init(lock_wait_mutex_key, &wait_mutex, MY_MUTEX_INIT_FAST);
#else
  mysql_mutex_init(lock_wait_mutex_key, &wait_mutex, nullptr);
#endif
#ifdef SUX_LOCK_GENERIC
  pthread_mutex_init(&hash_mutex, nullptr);
  pthread_cond_init(&hash_cond, nullptr);
#endif

  rec_hash.create(n_cells);
  prdt_hash.create(n_cells);
  prdt_page_hash.create(n_cells);

  if (!srv_read_only_mode)
  {
    lock_latest_err_file= os_file_create_tmpfile();
    ut_a(lock_latest_err_file);
  }
}

#ifdef UNIV_PFS_RWLOCK
/** Acquire exclusive lock_sys.latch */
void lock_sys_t::wr_lock(const char *file, unsigned line)
{
  mysql_mutex_assert_not_owner(&wait_mutex);
  latch.wr_lock(file, line);
  ut_ad(!writer.exchange(pthread_self(), std::memory_order_relaxed));
}
/** Release exclusive lock_sys.latch */
void lock_sys_t::wr_unlock()
{
  ut_ad(writer.exchange(0, std::memory_order_relaxed) ==
        pthread_self());
  latch.wr_unlock();
}

/** Acquire shared lock_sys.latch */
void lock_sys_t::rd_lock(const char *file, unsigned line)
{
  mysql_mutex_assert_not_owner(&wait_mutex);
  latch.rd_lock(file, line);
  ut_ad(!writer.load(std::memory_order_relaxed));
  ut_d(readers.fetch_add(1, std::memory_order_relaxed));
}

/** Release shared lock_sys.latch */
void lock_sys_t::rd_unlock()
{
  ut_ad(!writer.load(std::memory_order_relaxed));
  ut_ad(readers.fetch_sub(1, std::memory_order_relaxed));
  latch.rd_unlock();
}
#endif

/**
  Resize the lock hash table.

  @param[in] n_cells number of slots in lock hash table
*/
void lock_sys_t::resize(ulint n_cells)
{
  ut_ad(this == &lock_sys);
  /* Buffer pool resizing is rarely initiated by the user, and this
  would exceed the maximum size of a memory transaction. */
  LockMutexGuard g{SRW_LOCK_CALL};
  rec_hash.resize(n_cells);
  prdt_hash.resize(n_cells);
  prdt_page_hash.resize(n_cells);
}

/** Closes the lock system at database shutdown. */
void lock_sys_t::close()
{
  ut_ad(this == &lock_sys);

  if (!m_initialised)
    return;

  if (lock_latest_err_file)
  {
    my_fclose(lock_latest_err_file, MYF(MY_WME));
    lock_latest_err_file= nullptr;
  }

  rec_hash.free();
  prdt_hash.free();
  prdt_page_hash.free();
#ifdef SUX_LOCK_GENERIC
  pthread_mutex_destroy(&hash_mutex);
  pthread_cond_destroy(&hash_cond);
#endif

  latch.destroy();
  mysql_mutex_destroy(&wait_mutex);

  Deadlock::to_check.clear();
  Deadlock::to_be_checked= false;

  m_initialised= false;
}

#ifdef WITH_WSREP
# ifdef UNIV_DEBUG
/** Check if both conflicting lock transaction and other transaction
requesting record lock are brute force (BF). If they are check is
this BF-BF wait correct and if not report BF wait and assert.

@param[in]	lock_rec	other waiting record lock
@param[in]	trx		trx requesting conflicting record lock
*/
static void wsrep_assert_no_bf_bf_wait(const lock_t *lock, const trx_t *trx)
{
	ut_ad(!lock->is_table());
	lock_sys.assert_locked(*lock);
	trx_t* lock_trx= lock->trx;

	/* Note that we are holding lock_sys.latch, thus we should
	not acquire THD::LOCK_thd_data mutex below to avoid latching
	order violation. */

	if (!trx->is_wsrep() || !lock_trx->is_wsrep())
		return;
	if (UNIV_LIKELY(!wsrep_thd_is_BF(trx->mysql_thd, FALSE))
	    || UNIV_LIKELY(!wsrep_thd_is_BF(lock_trx->mysql_thd, FALSE)))
		return;

	ut_ad(trx->state == TRX_STATE_ACTIVE);

	switch (lock_trx->state) {
	case TRX_STATE_COMMITTED_IN_MEMORY:
		/* The state change is only protected by trx_t::mutex,
		which we are not even holding here. */
	case TRX_STATE_PREPARED:
		/* Wait for lock->trx to complete the commit
		(or XA ROLLBACK) and to release the lock. */
		return;
	case TRX_STATE_ACTIVE:
		break;
	default:
		ut_ad("invalid state" == 0);
	}

	/* If BF - BF order is honored, i.e. trx already holding
	record lock should be ordered before this new lock request
	we can keep trx waiting for the lock. If conflicting
	transaction is already aborting or rolling back for replaying
	we can also let new transaction waiting. */
	if (wsrep_thd_order_before(lock_trx->mysql_thd, trx->mysql_thd)
	    || wsrep_thd_is_aborting(lock_trx->mysql_thd)) {
		return;
	}

	mtr_t mtr;

	ib::error() << "Conflicting lock on table: "
		    << lock->index->table->name
		    << " index: "
		    << lock->index->name()
		    << " that has lock ";
	lock_rec_print(stderr, lock, mtr);

	ib::error() << "WSREP state: ";

	wsrep_report_bf_lock_wait(trx->mysql_thd,
				  trx->id);
	wsrep_report_bf_lock_wait(lock_trx->mysql_thd,
				  lock_trx->id);
	/* BF-BF wait is a bug */
	ut_error;
}
# endif /* UNIV_DEBUG */

/** check if lock timeout was for priority thread,
as a side effect trigger lock monitor
@param trx    transaction owning the lock
@return false for regular lock timeout */
ATTRIBUTE_NOINLINE static bool wsrep_is_BF_lock_timeout(const trx_t &trx)
{
  ut_ad(trx.is_wsrep());

  if (trx.error_state == DB_DEADLOCK || !srv_monitor_timer ||
      !wsrep_thd_is_BF(trx.mysql_thd, false))
    return false;

  ib::info() << "WSREP: BF lock wait long for trx:" << ib::hex(trx.id)
             << " query: " << wsrep_thd_query(trx.mysql_thd);
  return true;
}
#endif /* WITH_WSREP */

/*********************************************************************//**
Checks if a lock request for a new lock has to wait for request lock2.
@return TRUE if new lock has to wait for lock2 to be removed */
UNIV_INLINE
bool
lock_rec_has_to_wait(
/*=================*/
	bool		for_locking,
				/*!< in is called locking or releasing */
	const trx_t*	trx,	/*!< in: trx of new lock */
	unsigned	type_mode,/*!< in: precise mode of the new lock
				to set: LOCK_S or LOCK_X, possibly
				ORed to LOCK_GAP or LOCK_REC_NOT_GAP,
				LOCK_INSERT_INTENTION */
	const lock_t*	lock2,	/*!< in: another record lock; NOTE that
				it is assumed that this has a lock bit
				set on the same record as in the new
				lock we are setting */
	bool		lock_is_on_supremum)
				/*!< in: TRUE if we are setting the
				lock on the 'supremum' record of an
				index page: we know then that the lock
				request is really for a 'gap' type lock */
{
	ut_ad(trx);
	ut_ad(!lock2->is_table());
	ut_d(lock_sys.hash_get(type_mode).assert_locked(
		     lock2->un_member.rec_lock.page_id));

	if (trx == lock2->trx
	    || lock_mode_compatible(
		       static_cast<lock_mode>(LOCK_MODE_MASK & type_mode),
		       lock2->mode())) {
		return false;
	}

	/* We have somewhat complex rules when gap type record locks
	cause waits */

	if ((lock_is_on_supremum || (type_mode & LOCK_GAP))
	    && !(type_mode & LOCK_INSERT_INTENTION)) {

		/* Gap type locks without LOCK_INSERT_INTENTION flag
		do not need to wait for anything. This is because
		different users can have conflicting lock types
		on gaps. */

		return false;
	}

	if (!(type_mode & LOCK_INSERT_INTENTION) && lock2->is_gap()) {

		/* Record lock (LOCK_ORDINARY or LOCK_REC_NOT_GAP
		does not need to wait for a gap type lock */

		return false;
	}

	if ((type_mode & LOCK_GAP) && lock2->is_record_not_gap()) {

		/* Lock on gap does not need to wait for
		a LOCK_REC_NOT_GAP type lock */

		return false;
	}

	if (lock2->is_insert_intention()) {
		/* No lock request needs to wait for an insert
		intention lock to be removed. This is ok since our
		rules allow conflicting locks on gaps. This eliminates
		a spurious deadlock caused by a next-key lock waiting
		for an insert intention lock; when the insert
		intention lock was granted, the insert deadlocked on
		the waiting next-key lock.

		Also, insert intention locks do not disturb each
		other. */

		return false;
	}

#ifdef HAVE_REPLICATION
	if ((type_mode & LOCK_GAP || lock2->is_gap())
	    && !thd_need_ordering_with(trx->mysql_thd, lock2->trx->mysql_thd)) {
		/* If the upper server layer has already decided on the
		commit order between the transaction requesting the
		lock and the transaction owning the lock, we do not
		need to wait for gap locks. Such ordeering by the upper
		server layer happens in parallel replication, where the
		commit order is fixed to match the original order on the
		master.

		Such gap locks are mainly needed to get serialisability
		between transactions so that they will be binlogged in
		the correct order so that statement-based replication
		will give the correct results. Since the right order
		was already determined on the master, we do not need
		to enforce it again here.

		Skipping the locks is not essential for correctness,
		since in case of deadlock we will just kill the later
		transaction and retry it. But it can save some
		unnecessary rollbacks and retries. */

		return false;
	}
#endif /* HAVE_REPLICATION */

#ifdef WITH_WSREP
		/* New lock request from a transaction is using unique key
		scan and this transaction is a wsrep high priority transaction
		(brute force). If conflicting transaction is also wsrep high
		priority transaction we should avoid lock conflict because
		ordering of these transactions is already decided and
		conflicting transaction will be later replayed. */
		if (trx->is_wsrep_UK_scan()
		    && wsrep_thd_is_BF(lock2->trx->mysql_thd, false)) {
			return false;
		}

		/* We very well can let bf to wait normally as other
		BF will be replayed in case of conflict. For debug
		builds we will do additional sanity checks to catch
		unsupported bf wait if any. */
		ut_d(wsrep_assert_no_bf_bf_wait(lock2, trx));
#endif /* WITH_WSREP */

	return true;
}

/*********************************************************************//**
Checks if a lock request lock1 has to wait for request lock2.
@return TRUE if lock1 has to wait for lock2 to be removed */
bool
lock_has_to_wait(
/*=============*/
	const lock_t*	lock1,	/*!< in: waiting lock */
	const lock_t*	lock2)	/*!< in: another lock; NOTE that it is
				assumed that this has a lock bit set
				on the same record as in lock1 if the
				locks are record locks */
{
	ut_ad(lock1 && lock2);

	if (lock1->trx == lock2->trx
	    || lock_mode_compatible(lock1->mode(), lock2->mode())) {
		return false;
	}

	if (lock1->is_table()) {
		return true;
	}

	ut_ad(!lock2->is_table());

	if (lock1->type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE)) {
		return lock_prdt_has_to_wait(lock1->trx, lock1->type_mode,
					     lock_get_prdt_from_lock(lock1),
					     lock2);
	}

	return lock_rec_has_to_wait(
		false, lock1->trx, lock1->type_mode, lock2,
		lock_rec_get_nth_bit(lock1, PAGE_HEAP_NO_SUPREMUM));
}

/*============== RECORD LOCK BASIC FUNCTIONS ============================*/

/**********************************************************************//**
Looks for a set bit in a record lock bitmap. Returns ULINT_UNDEFINED,
if none found.
@return bit index == heap number of the record, or ULINT_UNDEFINED if
none found */
ulint
lock_rec_find_set_bit(
/*==================*/
	const lock_t*	lock)	/*!< in: record lock with at least one bit set */
{
	for (ulint i = 0; i < lock_rec_get_n_bits(lock); ++i) {

		if (lock_rec_get_nth_bit(lock, i)) {

			return(i);
		}
	}

	return(ULINT_UNDEFINED);
}

/*********************************************************************//**
Resets the record lock bitmap to zero. NOTE: does not touch the wait_lock
pointer in the transaction! This function is used in lock object creation
and resetting. */
static
void
lock_rec_bitmap_reset(
/*==================*/
	lock_t*	lock)	/*!< in: record lock */
{
	ulint	n_bytes;

	ut_ad(!lock->is_table());

	/* Reset to zero the bitmap which resides immediately after the lock
	struct */

	n_bytes = lock_rec_get_n_bits(lock) / 8;

	ut_ad((lock_rec_get_n_bits(lock) % 8) == 0);

	memset(reinterpret_cast<void*>(&lock[1]), 0, n_bytes);
}

/*********************************************************************//**
Copies a record lock to heap.
@return copy of lock */
static
lock_t*
lock_rec_copy(
/*==========*/
	const lock_t*	lock,	/*!< in: record lock */
	mem_heap_t*	heap)	/*!< in: memory heap */
{
	ulint	size;

	ut_ad(!lock->is_table());

	size = sizeof(lock_t) + lock_rec_get_n_bits(lock) / 8;

	return(static_cast<lock_t*>(mem_heap_dup(heap, lock, size)));
}

/*********************************************************************//**
Gets the previous record lock set on a record.
@return previous lock on the same record, NULL if none exists */
const lock_t*
lock_rec_get_prev(
/*==============*/
	const lock_t*	in_lock,/*!< in: record lock */
	ulint		heap_no)/*!< in: heap number of the record */
{
  ut_ad(!in_lock->is_table());
  const page_id_t id{in_lock->un_member.rec_lock.page_id};
  hash_cell_t *cell= lock_sys.hash_get(in_lock->type_mode).cell_get(id.fold());

  for (lock_t *lock= lock_sys_t::get_first(*cell, id); lock != in_lock;
       lock= lock_rec_get_next_on_page(lock))
    if (lock_rec_get_nth_bit(lock, heap_no))
      return lock;

  return nullptr;
}

/*============= FUNCTIONS FOR ANALYZING RECORD LOCK QUEUE ================*/

/*********************************************************************//**
Checks if a transaction has a GRANTED explicit lock on rec stronger or equal
to precise_mode.
@return lock or NULL */
UNIV_INLINE
lock_t*
lock_rec_has_expl(
/*==============*/
	ulint			precise_mode,/*!< in: LOCK_S or LOCK_X
					possibly ORed to LOCK_GAP or
					LOCK_REC_NOT_GAP, for a
					supremum record we regard this
					always a gap type request */
	const hash_cell_t&	cell,	/*!< in: lock hash table cell */
	const page_id_t		id,	/*!< in: page identifier */
	ulint			heap_no,/*!< in: heap number of the record */
	const trx_t*		trx)	/*!< in: transaction */
{
  ut_ad((precise_mode & LOCK_MODE_MASK) == LOCK_S
	|| (precise_mode & LOCK_MODE_MASK) == LOCK_X);
  ut_ad(!(precise_mode & LOCK_INSERT_INTENTION));

  for (lock_t *lock= lock_sys_t::get_first(cell, id, heap_no); lock;
       lock= lock_rec_get_next(heap_no, lock))
    if (lock->trx == trx &&
	!(lock->type_mode & (LOCK_WAIT | LOCK_INSERT_INTENTION)) &&
	(!((LOCK_REC_NOT_GAP | LOCK_GAP) & lock->type_mode) ||
	 heap_no == PAGE_HEAP_NO_SUPREMUM ||
	 ((LOCK_REC_NOT_GAP | LOCK_GAP) & precise_mode & lock->type_mode)) &&
	lock_mode_stronger_or_eq(lock->mode(), static_cast<lock_mode>
				 (precise_mode & LOCK_MODE_MASK)))
      return lock;

  return nullptr;
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Checks if some other transaction has a lock request in the queue.
@return lock or NULL */
static
lock_t*
lock_rec_other_has_expl_req(
/*========================*/
	lock_mode		mode,	/*!< in: LOCK_S or LOCK_X */
	const hash_cell_t&	cell,	/*!< in: lock hash table cell */
	const page_id_t		id,	/*!< in: page identifier */
	bool			wait,	/*!< in: whether also waiting locks
					are taken into account */
	ulint			heap_no,/*!< in: heap number of the record */
	const trx_t*		trx)	/*!< in: transaction, or NULL if
					requests by all transactions
					are taken into account */
{
	ut_ad(mode == LOCK_X || mode == LOCK_S);

	/* Only GAP lock can be on SUPREMUM, and we are not looking for
	GAP lock */
	if (heap_no == PAGE_HEAP_NO_SUPREMUM) {
		return(NULL);
	}

	for (lock_t* lock = lock_sys_t::get_first(cell, id, heap_no);
	     lock; lock = lock_rec_get_next(heap_no, lock)) {
		if (lock->trx != trx
		    && !lock->is_gap()
		    && (!lock->is_waiting() || wait)
		    && lock_mode_stronger_or_eq(lock->mode(), mode)) {

			return(lock);
		}
	}

	return(NULL);
}
#endif /* UNIV_DEBUG */

#ifdef WITH_WSREP
void lock_wait_wsrep_kill(trx_t *bf_trx, ulong thd_id, trx_id_t trx_id);

/** Kill the holders of conflicting locks.
@param trx   brute-force applier transaction running in the current thread */
ATTRIBUTE_COLD ATTRIBUTE_NOINLINE
static void lock_wait_wsrep(trx_t *trx)
{
  DBUG_ASSERT(wsrep_on(trx->mysql_thd));
  if (!wsrep_thd_is_BF(trx->mysql_thd, false))
    return;

  std::set<trx_t*> victims;

  lock_sys.wr_lock(SRW_LOCK_CALL);
  mysql_mutex_lock(&lock_sys.wait_mutex);

  const lock_t *wait_lock= trx->lock.wait_lock;
  if (!wait_lock)
  {
func_exit:
    lock_sys.wr_unlock();
    mysql_mutex_unlock(&lock_sys.wait_mutex);
    return;
  }

  if (wait_lock->is_table())
  {
    dict_table_t *table= wait_lock->un_member.tab_lock.table;
    for (lock_t *lock= UT_LIST_GET_FIRST(table->locks); lock;
         lock= UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock))
      /* if victim has also BF status, but has earlier seqno, we have to wait */
      if (lock->trx != trx &&
          !(wsrep_thd_is_BF(lock->trx->mysql_thd, false) &&
            wsrep_thd_order_before(lock->trx->mysql_thd, trx->mysql_thd)))
      {
        victims.emplace(lock->trx);
      }
  }
  else
  {
    const page_id_t id{wait_lock->un_member.rec_lock.page_id};
    hash_cell_t &cell= *(wait_lock->type_mode & LOCK_PREDICATE
                         ? lock_sys.prdt_hash : lock_sys.rec_hash).cell_get
      (id.fold());
    if (lock_t *lock= lock_sys_t::get_first(cell, id))
    {
      const ulint heap_no= lock_rec_find_set_bit(wait_lock);
      if (!lock_rec_get_nth_bit(lock, heap_no))
        lock= lock_rec_get_next(heap_no, lock);
      do
        /* if victim has also BF status, but has earlier seqno, we have to wait */
        if (lock->trx != trx &&
            !(wsrep_thd_is_BF(lock->trx->mysql_thd, false) &&
              wsrep_thd_order_before(lock->trx->mysql_thd, trx->mysql_thd)))
        {
          victims.emplace(lock->trx);
        }
      while ((lock= lock_rec_get_next(heap_no, lock)));
    }
  }

  if (victims.empty())
    goto func_exit;

  std::vector<std::pair<ulong,trx_id_t>> victim_id;
  for (trx_t *v : victims)
    victim_id.emplace_back(std::pair<ulong,trx_id_t>
                           {thd_get_thread_id(v->mysql_thd), v->id});

  DBUG_EXECUTE_IF("sync.before_wsrep_thd_abort",
                  {
                    const char act[]=
                      "now SIGNAL sync.before_wsrep_thd_abort_reached "
                      "WAIT_FOR signal.before_wsrep_thd_abort";
                    DBUG_ASSERT(!debug_sync_set_action(trx->mysql_thd,
                                                       STRING_WITH_LEN(act)));
                  };);

  lock_sys.wr_unlock();
  mysql_mutex_unlock(&lock_sys.wait_mutex);

  for (const auto &v : victim_id)
    lock_wait_wsrep_kill(trx, v.first, v.second);
}
#endif /* WITH_WSREP */

/*********************************************************************//**
Checks if some other transaction has a conflicting explicit lock request
in the queue, so that we have to wait.
@param[in] mode LOCK_S or LOCK_X, possibly ORed to LOCK_GAP or LOC_REC_NOT_GAP,
LOCK_INSERT_INTENTION
@param[in] cell lock hash table cell
@param[in] id page identifier
@param[in] heap_no heap number of the record
@param[in] trx our transaction
@return conflicting lock and the flag which indicated if conflicting locks
which wait for the current transaction were ignored */
static lock_t *lock_rec_other_has_conflicting(unsigned mode,
                                              const hash_cell_t &cell,
                                              const page_id_t id,
                                              ulint heap_no, const trx_t *trx)
{
	bool	is_supremum = (heap_no == PAGE_HEAP_NO_SUPREMUM);

	for (lock_t* lock = lock_sys_t::get_first(cell, id, heap_no);
	     lock; lock = lock_rec_get_next(heap_no, lock)) {
		if (lock_rec_has_to_wait(true, trx, mode, lock, is_supremum)) {
			return(lock);
		}
	}

	return(NULL);
}

/*********************************************************************//**
Checks if some transaction has an implicit x-lock on a record in a secondary
index.
@return transaction id of the transaction which has the x-lock, or 0;
NOTE that this function can return false positives but never false
negatives. The caller must confirm all positive results by calling
trx_is_active(). */
static
trx_t*
lock_sec_rec_some_has_impl(
/*=======================*/
	trx_t*		caller_trx,/*!<in/out: trx of current thread */
	const rec_t*	rec,	/*!< in: user record */
	dict_index_t*	index,	/*!< in: secondary index */
	const rec_offs*	offsets)/*!< in: rec_get_offsets(rec, index) */
{
  lock_sys.assert_unlocked();
  ut_ad(!dict_index_is_clust(index));
  ut_ad(page_rec_is_user_rec(rec));
  ut_ad(rec_offs_validate(rec, index, offsets));
  ut_ad(!rec_is_metadata(rec, *index));

  const trx_id_t max_trx_id= page_get_max_trx_id(page_align(rec));

  if ((caller_trx->id > max_trx_id &&
       !trx_sys.find_same_or_older(caller_trx, max_trx_id)) ||
      !lock_check_trx_id_sanity(max_trx_id, rec, index, offsets))
    return nullptr;

  /* In this case it is possible that some transaction has an implicit
  x-lock. We have to look in the clustered index. */
  return row_vers_impl_x_locked(caller_trx, rec, index, offsets);
}

/*********************************************************************//**
Return the number of table locks for a transaction.
The caller must be holding lock_sys.latch. */
ulint
lock_number_of_tables_locked(
/*=========================*/
	const trx_lock_t*	trx_lock)	/*!< in: transaction locks */
{
	const lock_t*	lock;
	ulint		n_tables = 0;

	lock_sys.assert_locked();

	for (lock = UT_LIST_GET_FIRST(trx_lock->trx_locks);
	     lock != NULL;
	     lock = UT_LIST_GET_NEXT(trx_locks, lock)) {

		if (lock->is_table()) {
			n_tables++;
		}
	}

	return(n_tables);
}

/*============== RECORD LOCK CREATION AND QUEUE MANAGEMENT =============*/

/** Reset the wait status of a lock.
@param[in,out]	lock	lock that was possibly being waited for */
static void lock_reset_lock_and_trx_wait(lock_t *lock)
{
  lock_sys.assert_locked(*lock);
  mysql_mutex_assert_owner(&lock_sys.wait_mutex);
  trx_t *trx= lock->trx;
  ut_ad(lock->is_waiting());
  ut_ad(!trx->lock.wait_lock || trx->lock.wait_lock == lock);
  if (trx_t *wait_trx= trx->lock.wait_trx)
    Deadlock::to_check.erase(wait_trx);
  trx->lock.wait_lock= nullptr;
  trx->lock.wait_trx= nullptr;
  lock->type_mode&= ~LOCK_WAIT;
}

#ifdef UNIV_DEBUG
/** Check transaction state */
static void check_trx_state(const trx_t *trx)
{
  ut_ad(!trx->auto_commit || trx->will_lock);
  const auto state= trx->state;
  ut_ad(state == TRX_STATE_ACTIVE ||
        state == TRX_STATE_PREPARED_RECOVERED ||
        state == TRX_STATE_PREPARED ||
        state == TRX_STATE_COMMITTED_IN_MEMORY);
}
#endif

/** Create a new record lock and inserts it to the lock queue,
without checking for deadlocks or conflicts.
@param[in]	c_lock		conflicting lock
@param[in]	type_mode	lock mode and wait flag
@param[in]	page_id		index page number
@param[in]	page		R-tree index page, or NULL
@param[in]	heap_no		record heap number in the index page
@param[in]	index		the index tree
@param[in,out]	trx		transaction
@param[in]	holds_trx_mutex	whether the caller holds trx->mutex
@return created lock */
lock_t*
lock_rec_create_low(
	lock_t*		c_lock,
	unsigned	type_mode,
	const page_id_t	page_id,
	const page_t*	page,
	ulint		heap_no,
	dict_index_t*	index,
	trx_t*		trx,
	bool		holds_trx_mutex)
{
	lock_t*		lock;
	ulint		n_bytes;

	ut_d(lock_sys.hash_get(type_mode).assert_locked(page_id));
	ut_ad(xtest() || holds_trx_mutex == trx->mutex_is_owner());
	ut_ad(dict_index_is_clust(index) || !dict_index_is_online_ddl(index));
	ut_ad(!(type_mode & LOCK_TABLE));
	ut_ad(trx->state != TRX_STATE_NOT_STARTED);
	ut_ad(!trx->is_autocommit_non_locking());

	/* If rec is the supremum record, then we reset the gap and
	LOCK_REC_NOT_GAP bits, as all locks on the supremum are
	automatically of the gap type */

	if (UNIV_UNLIKELY(heap_no == PAGE_HEAP_NO_SUPREMUM)) {
		ut_ad(!(type_mode & LOCK_REC_NOT_GAP));
		type_mode = type_mode & ~(LOCK_GAP | LOCK_REC_NOT_GAP);
	}

	if (UNIV_LIKELY(!(type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE)))) {
		n_bytes = (page_dir_get_n_heap(page) + 7) / 8;
	} else {
		ut_ad(heap_no == PRDT_HEAPNO);

		/* The lock is always on PAGE_HEAP_NO_INFIMUM (0), so
		we only need 1 bit (which round up to 1 byte) for
		lock bit setting */
		n_bytes = 1;

		if (type_mode & LOCK_PREDICATE) {
			ulint	tmp = UNIV_WORD_SIZE - 1;

			/* We will attach predicate structure after lock.
			Make sure the memory is aligned on 8 bytes,
			the mem_heap_alloc will align it with
			MEM_SPACE_NEEDED anyway. */
			n_bytes = (n_bytes + sizeof(lock_prdt_t) + tmp) & ~tmp;
			ut_ad(n_bytes == sizeof(lock_prdt_t) + UNIV_WORD_SIZE);
		}
	}

	if (!holds_trx_mutex) {
		trx->mutex_lock();
	}
	ut_ad(trx->mutex_is_owner());
	ut_ad(trx->state != TRX_STATE_NOT_STARTED);

	if (trx->lock.rec_cached >= UT_ARR_SIZE(trx->lock.rec_pool)
	    || sizeof *lock + n_bytes > sizeof *trx->lock.rec_pool) {
		lock = static_cast<lock_t*>(
			mem_heap_alloc(trx->lock.lock_heap,
				       sizeof *lock + n_bytes));
	} else {
		lock = &trx->lock.rec_pool[trx->lock.rec_cached++].lock;
	}

	lock->trx = trx;
	lock->type_mode = type_mode;
	lock->index = index;
	lock->un_member.rec_lock.page_id = page_id;

	if (UNIV_LIKELY(!(type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE)))) {
		lock->un_member.rec_lock.n_bits = uint32_t(n_bytes * 8);
	} else {
		/* Predicate lock always on INFIMUM (0) */
		lock->un_member.rec_lock.n_bits = 8;
 	}
	lock_rec_bitmap_reset(lock);
	lock_rec_set_nth_bit(lock, heap_no);
	index->table->n_rec_locks++;
	ut_ad(index->table->get_ref_count() || !index->table->can_be_evicted);

	const auto lock_hash = &lock_sys.hash_get(type_mode);
	lock_hash->cell_get(page_id.fold())->append(*lock, &lock_t::hash);

	if (type_mode & LOCK_WAIT) {
		if (trx->lock.wait_trx) {
			ut_ad(!c_lock || trx->lock.wait_trx == c_lock->trx);
			ut_ad(trx->lock.wait_lock);
			ut_ad((*trx->lock.wait_lock).trx == trx);
		} else {
			ut_ad(c_lock);
			trx->lock.wait_trx = c_lock->trx;
			ut_ad(!trx->lock.wait_lock);
		}
		trx->lock.wait_lock = lock;
	}
	UT_LIST_ADD_LAST(trx->lock.trx_locks, lock);
	if (!holds_trx_mutex) {
		trx->mutex_unlock();
	}
	MONITOR_INC(MONITOR_RECLOCK_CREATED);
	MONITOR_INC(MONITOR_NUM_RECLOCK);

	return lock;
}

/** Enqueue a waiting request for a lock which cannot be granted immediately.
Check for deadlocks.
@param[in]	type_mode	the requested lock mode (LOCK_S or LOCK_X)
				possibly ORed with LOCK_GAP or
				LOCK_REC_NOT_GAP, ORed with
				LOCK_INSERT_INTENTION if this
				waiting lock request is set
				when performing an insert of
				an index record
@param[in]	id		page identifier
@param[in]	page		leaf page in the index
@param[in]	heap_no		record heap number in the block
@param[in]	index		index tree
@param[in,out]	thr		query thread
@param[in]	prdt		minimum bounding box (spatial index)
@retval	DB_LOCK_WAIT		if the waiting lock was enqueued
@retval	DB_DEADLOCK		if this transaction was chosen as the victim */
dberr_t
lock_rec_enqueue_waiting(
	lock_t*			c_lock,
	unsigned		type_mode,
	const page_id_t		id,
	const page_t*		page,
	ulint			heap_no,
	dict_index_t*		index,
	que_thr_t*		thr,
	lock_prdt_t*		prdt)
{
	ut_d(lock_sys.hash_get(type_mode).assert_locked(id));
	ut_ad(!srv_read_only_mode);
	ut_ad(dict_index_is_clust(index) || !dict_index_is_online_ddl(index));

	trx_t* trx = thr_get_trx(thr);
	ut_ad(xtest() || trx->mutex_is_owner());
	ut_ad(!trx->dict_operation_lock_mode);

	if (trx->mysql_thd && thd_lock_wait_timeout(trx->mysql_thd) == 0) {
		trx->error_state = DB_LOCK_WAIT_TIMEOUT;
		return DB_LOCK_WAIT_TIMEOUT;
	}

	/* Enqueue the lock request that will wait to be granted, note that
	we already own the trx mutex. */
	lock_t* lock = lock_rec_create_low(
		c_lock,
		type_mode | LOCK_WAIT, id, page, heap_no, index, trx, true);

	if (prdt && type_mode & LOCK_PREDICATE) {
		lock_prdt_set_prdt(lock, prdt);
	}

	trx->lock.wait_thr = thr;
	trx->lock.clear_deadlock_victim();

	DBUG_LOG("ib_lock", "trx " << ib::hex(trx->id)
		 << " waits for lock in index " << index->name
		 << " of table " << index->table->name);

	MONITOR_INC(MONITOR_LOCKREC_WAIT);

	return DB_LOCK_WAIT;
}

/*********************************************************************//**
Looks for a suitable type record lock struct by the same trx on the same page.
This can be used to save space when a new record lock should be set on a page:
no new struct is needed, if a suitable old is found.
@return lock or NULL */
static inline
lock_t*
lock_rec_find_similar_on_page(
	ulint           type_mode,      /*!< in: lock type_mode field */
	ulint           heap_no,        /*!< in: heap number of the record */
	lock_t*         lock,           /*!< in: lock_sys.get_first() */
	const trx_t*    trx)            /*!< in: transaction */
{
	lock_sys.rec_hash.assert_locked(lock->un_member.rec_lock.page_id);

	for (/* No op */;
	     lock != NULL;
	     lock = lock_rec_get_next_on_page(lock)) {

		if (lock->trx == trx
		    && lock->type_mode == type_mode
		    && lock_rec_get_n_bits(lock) > heap_no) {

			return(lock);
		}
	}

	return(NULL);
}

/*********************************************************************//**
Adds a record lock request in the record queue. The request is normally
added as the last in the queue, but if there are no waiting lock requests
on the record, and the request to be added is not a waiting request, we
can reuse a suitable record lock object already existing on the same page,
just setting the appropriate bit in its bitmap. This is a low-level function
which does NOT check for deadlocks or lock compatibility!
@param[in] type_mode lock mode, wait, gap etc. flags
@param[in,out] cell first hash table cell
@param[in] id page identifier
@param[in] page buffer block containing the record
@param[in] heap_no heap number of the record
@param[in] index index of record
@param[in,out] trx transaction
@param[in] caller_owns_trx_mutex TRUE if caller owns the transaction mutex */
TRANSACTIONAL_TARGET
static void lock_rec_add_to_queue(unsigned type_mode, hash_cell_t &cell,
                                  const page_id_t id, const page_t *page,
                                  ulint heap_no, dict_index_t *index,
                                  trx_t *trx, bool caller_owns_trx_mutex)
{
	ut_d(lock_sys.hash_get(type_mode).assert_locked(id));
	ut_ad(xtest() || caller_owns_trx_mutex == trx->mutex_is_owner());
	ut_ad(index->is_primary()
	      || dict_index_get_online_status(index) != ONLINE_INDEX_CREATION);
	ut_ad(!(type_mode & LOCK_TABLE));
#ifdef UNIV_DEBUG
	switch (type_mode & LOCK_MODE_MASK) {
	case LOCK_X:
	case LOCK_S:
		break;
	default:
		ut_error;
	}

	if (!(type_mode & (LOCK_WAIT | LOCK_GAP))) {
		lock_mode	mode = (type_mode & LOCK_MODE_MASK) == LOCK_S
			? LOCK_X
			: LOCK_S;
		const lock_t*	other_lock
			= lock_rec_other_has_expl_req(
				mode, cell, id, false, heap_no, trx);
#ifdef WITH_WSREP
		if (UNIV_LIKELY_NULL(other_lock) && trx->is_wsrep()) {
			/* Only BF transaction may be granted lock
			before other conflicting lock request. */
			if (!wsrep_thd_is_BF(trx->mysql_thd, FALSE)
			    && !wsrep_thd_is_BF(other_lock->trx->mysql_thd, FALSE)) {
				/* If it is not BF, this case is a bug. */
				wsrep_report_bf_lock_wait(trx->mysql_thd, trx->id);
				wsrep_report_bf_lock_wait(other_lock->trx->mysql_thd, other_lock->trx->id);
				ut_error;
			}
		} else
#endif /* WITH_WSREP */
		ut_ad(!other_lock);
	}
#endif /* UNIV_DEBUG */

	/* If rec is the supremum record, then we can reset the gap bit, as
	all locks on the supremum are automatically of the gap type, and we
	try to avoid unnecessary memory consumption of a new record lock
	struct for a gap type lock */

	if (heap_no == PAGE_HEAP_NO_SUPREMUM) {
		ut_ad(!(type_mode & LOCK_REC_NOT_GAP));

		/* There should never be LOCK_REC_NOT_GAP on a supremum
		record, but let us play safe */

		type_mode &= ~(LOCK_GAP | LOCK_REC_NOT_GAP);
	}

	if (type_mode & LOCK_WAIT) {
		goto create;
	} else if (lock_t *first_lock = lock_sys_t::get_first(cell, id)) {
		for (lock_t* lock = first_lock;;) {
			if (lock->is_waiting()
			    && lock_rec_get_nth_bit(lock, heap_no)) {
				goto create;
			}
			if (!(lock = lock_rec_get_next_on_page(lock))) {
				break;
			}
		}

		/* Look for a similar record lock on the same page:
		if one is found and there are no waiting lock requests,
		we can just set the bit */
		if (lock_t* lock = lock_rec_find_similar_on_page(
			    type_mode, heap_no, first_lock, trx)) {
			trx_t* lock_trx = lock->trx;
			if (caller_owns_trx_mutex) {
				trx->mutex_unlock();
			}
			{
				TMTrxGuard tg{*lock_trx};
				lock_rec_set_nth_bit(lock, heap_no);
			}

			if (caller_owns_trx_mutex) {
				trx->mutex_lock();
			}
			return;
		}
	}

create:
	/* Note: We will not pass any conflicting lock to lock_rec_create(),
	because we should be moving an existing waiting lock request. */
	ut_ad(!(type_mode & LOCK_WAIT) || trx->lock.wait_trx);

	lock_rec_create_low(nullptr,
			    type_mode, id, page, heap_no, index, trx,
			    caller_owns_trx_mutex);
}

/*********************************************************************//**
Tries to lock the specified record in the mode requested. If not immediately
possible, enqueues a waiting lock request. This is a low-level function
which does NOT look at implicit locks! Checks lock compatibility within
explicit locks. This function sets a normal next-key lock, or in the case
of a page supremum record, a gap type lock.
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, DB_LOCK_WAIT, or DB_DEADLOCK */
static
dberr_t
lock_rec_lock(
/*==========*/
	bool			impl,	/*!< in: if true, no lock is set
					if no wait is necessary: we
					assume that the caller will
					set an implicit lock */
	unsigned		mode,	/*!< in: lock mode: LOCK_X or
					LOCK_S possibly ORed to either
					LOCK_GAP or LOCK_REC_NOT_GAP */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	ulint			heap_no,/*!< in: heap number of record */
	dict_index_t*		index,	/*!< in: index of record */
	que_thr_t*		thr)	/*!< in: query thread */
{
  trx_t *trx= thr_get_trx(thr);

  ut_ad(!srv_read_only_mode);
  ut_ad(((LOCK_MODE_MASK | LOCK_TABLE) & mode) == LOCK_S ||
        ((LOCK_MODE_MASK | LOCK_TABLE) & mode) == LOCK_X);
  ut_ad(~mode & (LOCK_GAP | LOCK_REC_NOT_GAP));
  ut_ad(dict_index_is_clust(index) || !dict_index_is_online_ddl(index));
  DBUG_EXECUTE_IF("innodb_report_deadlock", return DB_DEADLOCK;);

  ut_ad((LOCK_MODE_MASK & mode) != LOCK_S ||
        lock_table_has(trx, index->table, LOCK_IS));
  ut_ad((LOCK_MODE_MASK & mode) != LOCK_X ||
         lock_table_has(trx, index->table, LOCK_IX));

  if (lock_table_has(trx, index->table,
                     static_cast<lock_mode>(LOCK_MODE_MASK & mode)))
    return DB_SUCCESS;

  /* During CREATE TABLE, we will write to newly created FTS_*_CONFIG
  on which no lock has been created yet. */
  ut_ad(!trx->dict_operation_lock_mode ||
        (strstr(index->table->name.m_name, "/FTS_") &&
         strstr(index->table->name.m_name, "_CONFIG") + sizeof("_CONFIG") ==
         index->table->name.m_name + strlen(index->table->name.m_name) + 1));
  MONITOR_ATOMIC_INC(MONITOR_NUM_RECLOCK_REQ);
  const page_id_t id{block->page.id()};
  LockGuard g{lock_sys.rec_hash, id};

  if (lock_t *lock= lock_sys_t::get_first(g.cell(), id))
  {
    dberr_t err= DB_SUCCESS;
    trx->mutex_lock();
    if (lock_rec_get_next_on_page(lock) ||
        lock->trx != trx ||
        lock->type_mode != mode ||
        lock_rec_get_n_bits(lock) <= heap_no)
    {
      /* Do nothing if the trx already has a strong enough lock on rec */
      if (!lock_rec_has_expl(mode, g.cell(), id, heap_no, trx))
      {
        if (lock_t *c_lock= lock_rec_other_has_conflicting(mode, g.cell(), id,
                                                           heap_no, trx))
          /*
            If another transaction has a non-gap conflicting
            request in the queue, as this transaction does not
            have a lock strong enough already granted on the
            record, we have to wait.
          */
          err= lock_rec_enqueue_waiting(c_lock, mode, id, block->page.frame,
                                        heap_no, index, thr, nullptr);
        else if (!impl)
        {
          /* Set the requested lock on the record. */
          lock_rec_add_to_queue(mode, g.cell(), id, block->page.frame, heap_no,
                                index, trx, true);
          err= DB_SUCCESS_LOCKED_REC;
        }
      }
    }
    else if (!impl)
    {
      /*
        If the nth bit of the record lock is already set then we do not set
        a new lock bit, otherwise we do set
      */
      if (!lock_rec_get_nth_bit(lock, heap_no))
      {
        lock_rec_set_nth_bit(lock, heap_no);
        err= DB_SUCCESS_LOCKED_REC;
      }
    }
    trx->mutex_unlock();
    return err;
  }

  /* Simplified and faster path for the most common cases */
  if (!impl)
    lock_rec_create_low(nullptr, mode, id, block->page.frame, heap_no, index,
                        trx, false);

  return DB_SUCCESS_LOCKED_REC;
}

/*********************************************************************//**
Checks if a waiting record lock request still has to wait in a queue.
@return lock that is causing the wait */
static
const lock_t*
lock_rec_has_to_wait_in_queue(const hash_cell_t &cell, const lock_t *wait_lock)
{
	const lock_t*	lock;
	ulint		heap_no;
	ulint		bit_mask;
	ulint		bit_offset;

	ut_ad(wait_lock->is_waiting());
	ut_ad(!wait_lock->is_table());

	heap_no = lock_rec_find_set_bit(wait_lock);

	bit_offset = heap_no / 8;
	bit_mask = static_cast<ulint>(1) << (heap_no % 8);

	for (lock = lock_sys_t::get_first(
		     cell, wait_lock->un_member.rec_lock.page_id);
	     lock != wait_lock;
	     lock = lock_rec_get_next_on_page_const(lock)) {
		const byte*	p = (const byte*) &lock[1];

		if (heap_no < lock_rec_get_n_bits(lock)
		    && (p[bit_offset] & bit_mask)
		    && lock_has_to_wait(wait_lock, lock)) {
			return(lock);
		}
	}

	return(NULL);
}

/** Note that a record lock wait started */
inline void lock_sys_t::wait_start()
{
  mysql_mutex_assert_owner(&wait_mutex);
  wait_count+= WAIT_COUNT_STEP + 1;
  /* The maximum number of concurrently waiting transactions is one less
  than the maximum number of concurrent transactions. */
  static_assert(WAIT_COUNT_STEP == UNIV_PAGE_SIZE_MAX / 16 * TRX_SYS_N_RSEGS,
                "compatibility");
}

/** Note that a record lock wait resumed */
inline
void lock_sys_t::wait_resume(THD *thd, my_hrtime_t start, my_hrtime_t now)
{
  mysql_mutex_assert_owner(&wait_mutex);
  ut_ad(get_wait_pending());
  ut_ad(get_wait_cumulative());
  wait_count--;
  if (now.val >= start.val)
  {
    const uint32_t diff_time=
      static_cast<uint32_t>((now.val - start.val) / 1000);
    wait_time+= diff_time;

    if (diff_time > wait_time_max)
      wait_time_max= diff_time;

    thd_storage_lock_wait(thd, diff_time);
  }
}

#ifdef HAVE_REPLICATION
ATTRIBUTE_NOINLINE MY_ATTRIBUTE((nonnull))
/** Report lock waits to parallel replication.
@param trx       transaction that may be waiting for a lock
@param wait_lock lock that is being waited for */
static void lock_wait_rpl_report(trx_t *trx)
{
  mysql_mutex_assert_owner(&lock_sys.wait_mutex);
  ut_ad(trx->state == TRX_STATE_ACTIVE);
  THD *const thd= trx->mysql_thd;
  ut_ad(thd);
  const lock_t *wait_lock= trx->lock.wait_lock;
  if (!wait_lock)
    return;
  ut_ad(!(wait_lock->type_mode & LOCK_AUTO_INC));
  /* This would likely be too large to attempt to use a memory transaction,
  even for wait_lock->is_table(). */
  if (!lock_sys.wr_lock_try())
  {
    mysql_mutex_unlock(&lock_sys.wait_mutex);
    lock_sys.wr_lock(SRW_LOCK_CALL);
    mysql_mutex_lock(&lock_sys.wait_mutex);
    wait_lock= trx->lock.wait_lock;
    if (!wait_lock)
    {
func_exit:
      lock_sys.wr_unlock();
      return;
    }
    ut_ad(wait_lock->is_waiting());
  }
  else if (!wait_lock->is_waiting())
    goto func_exit;
  ut_ad(!(wait_lock->type_mode & LOCK_AUTO_INC));

  if (wait_lock->is_table())
  {
    dict_table_t *table= wait_lock->un_member.tab_lock.table;
    for (lock_t *lock= UT_LIST_GET_FIRST(table->locks); lock;
         lock= UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock))
      if (!(lock->type_mode & LOCK_AUTO_INC) && lock->trx != trx)
        thd_rpl_deadlock_check(thd, lock->trx->mysql_thd);
  }
  else
  {
    const page_id_t id{wait_lock->un_member.rec_lock.page_id};
    hash_cell_t &cell= *(wait_lock->type_mode & LOCK_PREDICATE
                         ? lock_sys.prdt_hash : lock_sys.rec_hash).cell_get
      (id.fold());
    if (lock_t *lock= lock_sys_t::get_first(cell, id))
    {
      const ulint heap_no= lock_rec_find_set_bit(wait_lock);
      if (!lock_rec_get_nth_bit(lock, heap_no))
        lock= lock_rec_get_next(heap_no, lock);
      do
        if (lock->trx->mysql_thd != thd)
          thd_rpl_deadlock_check(thd, lock->trx->mysql_thd);
      while ((lock= lock_rec_get_next(heap_no, lock)));
    }
  }

  goto func_exit;
}
#endif /* HAVE_REPLICATION */

/** Wait for a lock to be released.
@retval DB_DEADLOCK if this transaction was chosen as the deadlock victim
@retval DB_INTERRUPTED if the execution was interrupted by the user
@retval DB_LOCK_WAIT_TIMEOUT if the lock wait timed out
@retval DB_SUCCESS if the lock was granted */
dberr_t lock_wait(que_thr_t *thr)
{
  trx_t *trx= thr_get_trx(thr);

  if (trx->mysql_thd)
    DEBUG_SYNC_C("lock_wait_suspend_thread_enter");

  /* InnoDB system transactions may use the global value of
  innodb_lock_wait_timeout, because trx->mysql_thd == NULL. */
  const ulong innodb_lock_wait_timeout= trx_lock_wait_timeout_get(trx);
  const my_hrtime_t suspend_time= my_hrtime_coarse();
  ut_ad(!trx->dict_operation_lock_mode);

  /* The wait_lock can be cleared by another thread in lock_grant(),
  lock_rec_cancel(), or lock_cancel_waiting_and_release(). But, a wait
  can only be initiated by the current thread which owns the transaction.

  Even if trx->lock.wait_lock were changed, the object that it used to
  point to it will remain valid memory (remain allocated from
  trx->lock.lock_heap). If trx->lock.wait_lock was set to nullptr, the
  original object could be transformed to a granted lock. On a page
  split or merge, we would change trx->lock.wait_lock to point to
  another waiting lock request object, and the old object would be
  logically discarded.

  In any case, it is safe to read the memory that wait_lock points to,
  even though we are not holding any mutex. We are only reading
  wait_lock->type_mode & (LOCK_TABLE | LOCK_AUTO_INC), which will be
  unaffected by any page split or merge operation. (Furthermore,
  table lock objects will never be cloned or moved.) */
  const lock_t *const wait_lock= trx->lock.wait_lock;

  if (!wait_lock)
  {
    /* The lock has already been released or this transaction
    was chosen as a deadlock victim: no need to wait */
    if (trx->lock.was_chosen_as_deadlock_victim.fetch_and(byte(~1)))
      trx->error_state= DB_DEADLOCK;
    else
      trx->error_state= DB_SUCCESS;

    return trx->error_state;
  }

  trx->lock.suspend_time= suspend_time;

  ut_ad(!trx->dict_operation_lock_mode);

  IF_WSREP(if (trx->is_wsrep()) lock_wait_wsrep(trx),);

  const auto type_mode= wait_lock->type_mode;
#ifdef HAVE_REPLICATION
  /* Even though lock_wait_rpl_report() has nothing to do with
  deadlock detection, it was always disabled by innodb_deadlock_detect=OFF.
  We will keep it in that way, because unfortunately
  thd_need_wait_reports() will hold even if parallel (or any) replication
  is not being used. We want to be allow the user to skip
  lock_wait_rpl_report(). */
  const bool rpl= !(type_mode & LOCK_AUTO_INC) && trx->mysql_thd &&
    innodb_deadlock_detect && thd_need_wait_reports(trx->mysql_thd);
#endif
  const bool row_lock_wait= thr->lock_state == QUE_THR_LOCK_ROW;
  timespec abstime;
  set_timespec_time_nsec(abstime, suspend_time.val * 1000);
  abstime.MY_tv_sec+= innodb_lock_wait_timeout;
  /* Dictionary transactions must wait be immune to lock wait timeouts
  for locks on data dictionary tables. Here we check only for
  SYS_TABLES, SYS_COLUMNS, SYS_INDEXES, SYS_FIELDS. Locks on further
  tables SYS_FOREIGN, SYS_FOREIGN_COLS, SYS_VIRTUAL will only be
  acquired while holding an exclusive lock on one of the 4 tables. */
  const bool no_timeout= innodb_lock_wait_timeout >= 100000000 ||
    ((type_mode & LOCK_TABLE) &&
     wait_lock->un_member.tab_lock.table->id <= DICT_FIELDS_ID);
  thd_wait_begin(trx->mysql_thd, (type_mode & LOCK_TABLE)
                 ? THD_WAIT_TABLE_LOCK : THD_WAIT_ROW_LOCK);
  dberr_t error_state= DB_SUCCESS;

  mysql_mutex_lock(&lock_sys.wait_mutex);
  if (trx->lock.wait_lock)
  {
    if (Deadlock::check_and_resolve(trx))
    {
      ut_ad(!trx->lock.wait_lock);
      error_state= DB_DEADLOCK;
      goto end_wait;
    }
  }
  else
    goto end_wait;

  if (row_lock_wait)
    lock_sys.wait_start();

#ifdef HAVE_REPLICATION
  if (rpl)
    lock_wait_rpl_report(trx);
#endif

  trx->error_state= DB_SUCCESS;

  while (trx->lock.wait_lock)
  {
    int err;

    if (no_timeout)
    {
      my_cond_wait(&trx->lock.cond, &lock_sys.wait_mutex.m_mutex);
      err= 0;
    }
    else
      err= my_cond_timedwait(&trx->lock.cond, &lock_sys.wait_mutex.m_mutex,
                             &abstime);
    error_state= trx->error_state;
    switch (error_state) {
    case DB_DEADLOCK:
    case DB_INTERRUPTED:
      break;
    default:
      ut_ad(error_state != DB_LOCK_WAIT_TIMEOUT);
      /* Dictionary transactions must ignore KILL, because they could
      be executed as part of a multi-transaction DDL operation,
      such as rollback_inplace_alter_table() or ha_innobase::delete_table(). */
      if (!trx->dict_operation && trx_is_interrupted(trx))
        /* innobase_kill_query() can only set trx->error_state=DB_INTERRUPTED
        for any transaction that is attached to a connection. */
        error_state= DB_INTERRUPTED;
      else if (!err)
        continue;
#ifdef WITH_WSREP
      else if (trx->is_wsrep() && wsrep_is_BF_lock_timeout(*trx));
#endif
      else
      {
        error_state= DB_LOCK_WAIT_TIMEOUT;
        lock_sys.timeouts++;
      }
    }
    break;
  }

  if (row_lock_wait)
    lock_sys.wait_resume(trx->mysql_thd, suspend_time, my_hrtime_coarse());

  if (lock_t *lock= trx->lock.wait_lock)
  {
    lock_sys_t::cancel<false>(trx, lock);
    lock_sys.deadlock_check();
  }

end_wait:
  mysql_mutex_unlock(&lock_sys.wait_mutex);
  thd_wait_end(trx->mysql_thd);

  trx->error_state= error_state;
  return error_state;
}


/** Resume a lock wait */
static void lock_wait_end(trx_t *trx)
{
  mysql_mutex_assert_owner(&lock_sys.wait_mutex);
  ut_ad(trx->mutex_is_owner());
  ut_d(const auto state= trx->state);
  ut_ad(state == TRX_STATE_ACTIVE || state == TRX_STATE_PREPARED);
  ut_ad(trx->lock.wait_thr);

  if (trx->lock.was_chosen_as_deadlock_victim.fetch_and(byte(~1)))
  {
    ut_ad(state == TRX_STATE_ACTIVE);
    trx->error_state= DB_DEADLOCK;
  }

  trx->lock.wait_thr= nullptr;
  pthread_cond_signal(&trx->lock.cond);
}

/** Grant a waiting lock request and release the waiting transaction. */
static void lock_grant(lock_t *lock)
{
  lock_reset_lock_and_trx_wait(lock);
  trx_t *trx= lock->trx;
  trx->mutex_lock();
  if (lock->mode() == LOCK_AUTO_INC)
  {
    dict_table_t *table= lock->un_member.tab_lock.table;
    ut_ad(!table->autoinc_trx);
    table->autoinc_trx= trx;
    ib_vector_push(trx->autoinc_locks, &lock);
  }

  DBUG_PRINT("ib_lock", ("wait for trx " TRX_ID_FMT " ends", trx->id));

  /* If we are resolving a deadlock by choosing another transaction as
  a victim, then our original transaction may not be waiting anymore */

  if (trx->lock.wait_thr)
    lock_wait_end(trx);

  trx->mutex_unlock();
}

/*************************************************************//**
Cancels a waiting record lock request and releases the waiting transaction
that requested it. NOTE: does NOT check if waiting lock requests behind this
one can now be granted! */
static void lock_rec_cancel(lock_t *lock)
{
  trx_t *trx= lock->trx;
  mysql_mutex_lock(&lock_sys.wait_mutex);
  trx->mutex_lock();

  ut_d(lock_sys.hash_get(lock->type_mode).
       assert_locked(lock->un_member.rec_lock.page_id));
  /* Reset the bit (there can be only one set bit) in the lock bitmap */
  lock_rec_reset_nth_bit(lock, lock_rec_find_set_bit(lock));

  /* Reset the wait flag and the back pointer to lock in trx */
  lock_reset_lock_and_trx_wait(lock);

  /* The following releases the trx from lock wait */
  lock_wait_end(trx);
  mysql_mutex_unlock(&lock_sys.wait_mutex);
  trx->mutex_unlock();
}

/** Remove a record lock request, waiting or granted, from the queue and
grant locks to other transactions in the queue if they now are entitled
to a lock. NOTE: all record locks contained in in_lock are removed.
@param[in,out]	in_lock		record lock
@param[in]	owns_wait_mutex	whether lock_sys.wait_mutex is held */
static void lock_rec_dequeue_from_page(lock_t *in_lock, bool owns_wait_mutex)
{
#ifdef SAFE_MUTEX
	ut_ad(owns_wait_mutex == mysql_mutex_is_owner(&lock_sys.wait_mutex));
#endif /* SAFE_MUTEX */
	ut_ad(!in_lock->is_table());

	const page_id_t page_id{in_lock->un_member.rec_lock.page_id};
	auto& lock_hash = lock_sys.hash_get(in_lock->type_mode);
	ut_ad(lock_sys.is_writer() || in_lock->trx->mutex_is_owner());

	ut_d(auto old_n_locks=)
	in_lock->index->table->n_rec_locks--;
	ut_ad(old_n_locks);

	const ulint rec_fold = page_id.fold();
	hash_cell_t &cell = *lock_hash.cell_get(rec_fold);
	lock_sys.assert_locked(cell);

	HASH_DELETE(lock_t, hash, &lock_hash, rec_fold, in_lock);
	ut_ad(lock_sys.is_writer() || in_lock->trx->mutex_is_owner());
	UT_LIST_REMOVE(in_lock->trx->lock.trx_locks, in_lock);

	MONITOR_INC(MONITOR_RECLOCK_REMOVED);
	MONITOR_DEC(MONITOR_NUM_RECLOCK);

	bool acquired = false;

	/* Check if waiting locks in the queue can now be granted:
	grant locks if there are no conflicting locks ahead. Stop at
	the first X lock that is waiting or has been granted. */

	for (lock_t* lock = lock_sys_t::get_first(cell, page_id);
	     lock != NULL;
	     lock = lock_rec_get_next_on_page(lock)) {

		if (!lock->is_waiting()) {
			continue;
		}

		if (!owns_wait_mutex) {
			mysql_mutex_lock(&lock_sys.wait_mutex);
			acquired = owns_wait_mutex = true;
		}

		ut_ad(lock->trx->lock.wait_trx);
		ut_ad(lock->trx->lock.wait_lock);

		if (const lock_t* c = lock_rec_has_to_wait_in_queue(
			    cell, lock)) {
			trx_t* c_trx = c->trx;
			lock->trx->lock.wait_trx = c_trx;
			if (c_trx->lock.wait_trx
			    && innodb_deadlock_detect
			    && Deadlock::to_check.emplace(c_trx).second) {
				Deadlock::to_be_checked = true;
			}
		} else {
			/* Grant the lock */
			ut_ad(lock->trx != in_lock->trx);
			lock_grant(lock);
		}
	}

	if (acquired) {
		mysql_mutex_unlock(&lock_sys.wait_mutex);
	}
}

/** Remove a record lock request, waiting or granted, on a discarded page
@param hash     hash table
@param in_lock  lock object */
TRANSACTIONAL_TARGET
void lock_rec_discard(lock_sys_t::hash_table &lock_hash, lock_t *in_lock)
{
  ut_ad(!in_lock->is_table());
  lock_hash.assert_locked(in_lock->un_member.rec_lock.page_id);

  HASH_DELETE(lock_t, hash, &lock_hash,
              in_lock->un_member.rec_lock.page_id.fold(), in_lock);
  ut_d(uint32_t old_locks);
  {
    trx_t *trx= in_lock->trx;
    TMTrxGuard tg{*trx};
    ut_d(old_locks=)
    in_lock->index->table->n_rec_locks--;
    UT_LIST_REMOVE(trx->lock.trx_locks, in_lock);
  }
  ut_ad(old_locks);
  MONITOR_INC(MONITOR_RECLOCK_REMOVED);
  MONITOR_DEC(MONITOR_NUM_RECLOCK);
}

/*************************************************************//**
Removes record lock objects set on an index page which is discarded. This
function does not move locks, or check for waiting locks, therefore the
lock bitmaps must already be reset when this function is called. */
static void
lock_rec_free_all_from_discard_page(page_id_t id, const hash_cell_t &cell,
                                    lock_sys_t::hash_table &lock_hash)
{
  for (lock_t *lock= lock_sys_t::get_first(cell, id); lock; )
  {
    ut_ad(&lock_hash != &lock_sys.rec_hash ||
          lock_rec_find_set_bit(lock) == ULINT_UNDEFINED);
    ut_ad(!lock->is_waiting());
    lock_t *next_lock= lock_rec_get_next_on_page(lock);
    lock_rec_discard(lock_hash, lock);
    lock= next_lock;
  }
}

/** Discard locks for an index when purging DELETE FROM SYS_INDEXES
after an aborted CREATE INDEX operation.
@param index   a stale index on which ADD INDEX operation was aborted */
ATTRIBUTE_COLD void lock_discard_for_index(const dict_index_t &index)
{
  ut_ad(!index.is_committed());
  /* This is very rarely executed code, and the size of the hash array
  would exceed the maximum size of a memory transaction. */
  LockMutexGuard g{SRW_LOCK_CALL};
  const ulint n= lock_sys.rec_hash.pad(lock_sys.rec_hash.n_cells);
  for (ulint i= 0; i < n; i++)
  {
    for (lock_t *lock= static_cast<lock_t*>(lock_sys.rec_hash.array[i].node);
         lock; )
    {
      ut_ad(!lock->is_table());
      if (lock->index == &index)
      {
        ut_ad(!lock->is_waiting());
        lock_rec_discard(lock_sys.rec_hash, lock);
        lock= static_cast<lock_t*>(lock_sys.rec_hash.array[i].node);
      }
      else
        lock= lock->hash;
    }
  }
}

/*============= RECORD LOCK MOVING AND INHERITING ===================*/

/*************************************************************//**
Resets the lock bits for a single record. Releases transactions waiting for
lock requests here. */
TRANSACTIONAL_TARGET
static
void
lock_rec_reset_and_release_wait(const hash_cell_t &cell, const page_id_t id,
                                ulint heap_no)
{
  for (lock_t *lock= lock_sys.get_first(cell, id, heap_no); lock;
       lock= lock_rec_get_next(heap_no, lock))
  {
    if (lock->is_waiting())
      lock_rec_cancel(lock);
    else
    {
      TMTrxGuard tg{*lock->trx};
      lock_rec_reset_nth_bit(lock, heap_no);
    }
  }
}

/*************************************************************//**
Makes a record to inherit the locks (except LOCK_INSERT_INTENTION type)
of another record as gap type locks, but does not reset the lock bits of
the other record. Also waiting lock requests on rec are inherited as
GRANTED gap locks. */
static
void
lock_rec_inherit_to_gap(
/*====================*/
	hash_cell_t&		heir_cell,	/*!< heir hash table cell */
	const page_id_t		heir,		/*!< in: page containing the
						record which inherits */
	const hash_cell_t&	donor_cell,	/*!< donor hash table cell */
	const page_id_t		donor,		/*!< in: page containing the
						record from which inherited;
						does NOT reset the locks on
						this record */
	const page_t*		heir_page,	/*!< in: heir page frame */
	ulint			heir_heap_no,	/*!< in: heap_no of the
						inheriting record */
	ulint			heap_no)	/*!< in: heap_no of the
						donating record */
{
	/* At READ UNCOMMITTED or READ COMMITTED isolation level,
	we do not want locks set
	by an UPDATE or a DELETE to be inherited as gap type locks. But we
	DO want S-locks/X-locks(taken for replace) set by a consistency
	constraint to be inherited also then. */

	for (lock_t* lock= lock_sys_t::get_first(donor_cell, donor, heap_no);
	     lock;
	     lock = lock_rec_get_next(heap_no, lock)) {
		trx_t* lock_trx = lock->trx;
		if (!lock->is_insert_intention()
		    && (lock_trx->isolation_level > TRX_ISO_READ_COMMITTED
			|| lock->mode() !=
			(lock_trx->duplicates ? LOCK_S : LOCK_X))) {
			lock_rec_add_to_queue(LOCK_GAP | lock->mode(),
					      heir_cell, heir, heir_page,
					      heir_heap_no,
					      lock->index, lock_trx, false);
		}
	}
}

/*************************************************************//**
Makes a record to inherit the gap locks (except LOCK_INSERT_INTENTION type)
of another record as gap type locks, but does not reset the lock bits of the
other record. Also waiting lock requests are inherited as GRANTED gap locks. */
static
void
lock_rec_inherit_to_gap_if_gap_lock(
/*================================*/
	const buf_block_t*	block,		/*!< in: buffer block */
	ulint			heir_heap_no,	/*!< in: heap_no of
						record which inherits */
	ulint			heap_no)	/*!< in: heap_no of record
						from which inherited;
						does NOT reset the locks
						on this record */
{
  const page_id_t id{block->page.id()};
  LockGuard g{lock_sys.rec_hash, id};

  for (lock_t *lock= lock_sys_t::get_first(g.cell(), id, heap_no); lock;
       lock= lock_rec_get_next(heap_no, lock))
     if (!lock->is_insert_intention() && (heap_no == PAGE_HEAP_NO_SUPREMUM ||
                                          !lock->is_record_not_gap()) &&
         !lock_table_has(lock->trx, lock->index->table, LOCK_X))
       lock_rec_add_to_queue(LOCK_GAP | lock->mode(),
                             g.cell(), id, block->page.frame,
                             heir_heap_no, lock->index, lock->trx, false);
}

/*************************************************************//**
Moves the locks of a record to another record and resets the lock bits of
the donating record. */
TRANSACTIONAL_TARGET
static
void
lock_rec_move(
	hash_cell_t&		receiver_cell,	/*!< in: hash table cell */
	const buf_block_t&	receiver,	/*!< in: buffer block containing
						the receiving record */
	const page_id_t		receiver_id,	/*!< in: page identifier */
	const hash_cell_t&	donator_cell,	/*!< in: hash table cell */
	const page_id_t		donator_id,	/*!< in: page identifier of
						the donating record */
	ulint			receiver_heap_no,/*!< in: heap_no of the record
						which gets the locks; there
						must be no lock requests
						on it! */
	ulint			donator_heap_no)/*!< in: heap_no of the record
						which gives the locks */
{
	ut_ad(!lock_sys_t::get_first(receiver_cell,
				     receiver_id, receiver_heap_no));

	for (lock_t *lock = lock_sys_t::get_first(donator_cell, donator_id,
						  donator_heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next(donator_heap_no, lock)) {
		const auto type_mode = lock->type_mode;
		if (type_mode & LOCK_WAIT) {
			ut_ad(lock->trx->lock.wait_lock == lock);
			lock->type_mode &= ~LOCK_WAIT;
		}

		trx_t* lock_trx = lock->trx;
		lock_trx->mutex_lock();
		lock_rec_reset_nth_bit(lock, donator_heap_no);

		/* Note that we FIRST reset the bit, and then set the lock:
		the function works also if donator_id == receiver_id */

		lock_rec_add_to_queue(type_mode, receiver_cell,
				      receiver_id, receiver.page.frame,
				      receiver_heap_no,
				      lock->index, lock_trx, true);
		lock_trx->mutex_unlock();
	}

	ut_ad(!lock_sys_t::get_first(donator_cell, donator_id,
				     donator_heap_no));
}

/** Move all the granted locks to the front of the given lock list.
All the waiting locks will be at the end of the list.
@param[in,out]	lock_list	the given lock list.  */
static
void
lock_move_granted_locks_to_front(
	UT_LIST_BASE_NODE_T(lock_t)&	lock_list)
{
	lock_t*	lock;

	bool seen_waiting_lock = false;

	for (lock = UT_LIST_GET_FIRST(lock_list); lock;
	     lock = UT_LIST_GET_NEXT(trx_locks, lock)) {

		if (!seen_waiting_lock) {
			if (lock->is_waiting()) {
				seen_waiting_lock = true;
			}
			continue;
		}

		ut_ad(seen_waiting_lock);

		if (!lock->is_waiting()) {
			lock_t* prev = UT_LIST_GET_PREV(trx_locks, lock);
			ut_a(prev);
			ut_list_move_to_front(lock_list, lock);
			lock = prev;
		}
	}
}

/*************************************************************//**
Updates the lock table when we have reorganized a page. NOTE: we copy
also the locks set on the infimum of the page; the infimum may carry
locks if an update of a record is occurring on the page, and its locks
were temporarily stored on the infimum. */
TRANSACTIONAL_TARGET
void
lock_move_reorganize_page(
/*======================*/
	const buf_block_t*	block,	/*!< in: old index page, now
					reorganized */
	const buf_block_t*	oblock)	/*!< in: copy of the old, not
					reorganized page */
{
  mem_heap_t *heap;

  {
    UT_LIST_BASE_NODE_T(lock_t) old_locks;
    UT_LIST_INIT(old_locks, &lock_t::trx_locks);

    const page_id_t id{block->page.id()};
    const auto id_fold= id.fold();
    {
      TMLockGuard g{lock_sys.rec_hash, id};
      if (!lock_sys_t::get_first(g.cell(), id))
        return;
    }

    /* We will modify arbitrary trx->lock.trx_locks.
    Do not bother with a memory transaction; we are going
    to allocate memory and copy a lot of data. */
    LockMutexGuard g{SRW_LOCK_CALL};
    hash_cell_t &cell= *lock_sys.rec_hash.cell_get(id_fold);

    /* Note: Predicate locks for SPATIAL INDEX are not affected by
    page reorganize, because they do not refer to individual record
    heap numbers. */
    lock_t *lock= lock_sys_t::get_first(cell, id);

    if (!lock)
      return;

    heap= mem_heap_create(256);

    /* Copy first all the locks on the page to heap and reset the
    bitmaps in the original locks; chain the copies of the locks
    using the trx_locks field in them. */

    do
    {
      /* Make a copy of the lock */
      lock_t *old_lock= lock_rec_copy(lock, heap);

      UT_LIST_ADD_LAST(old_locks, old_lock);

      /* Reset bitmap of lock */
      lock_rec_bitmap_reset(lock);

      if (lock->is_waiting())
      {
        ut_ad(lock->trx->lock.wait_lock == lock);
        lock->type_mode&= ~LOCK_WAIT;
      }

      lock= lock_rec_get_next_on_page(lock);
    }
    while (lock);

    const ulint comp= page_is_comp(block->page.frame);
    ut_ad(comp == page_is_comp(oblock->page.frame));

    lock_move_granted_locks_to_front(old_locks);

    DBUG_EXECUTE_IF("do_lock_reverse_page_reorganize",
                    ut_list_reverse(old_locks););

    for (lock= UT_LIST_GET_FIRST(old_locks); lock;
         lock= UT_LIST_GET_NEXT(trx_locks, lock))
    {
      /* NOTE: we copy also the locks set on the infimum and
      supremum of the page; the infimum may carry locks if an
      update of a record is occurring on the page, and its locks
      were temporarily stored on the infimum */
      const rec_t *rec1= page_get_infimum_rec(block->page.frame);
      const rec_t *rec2= page_get_infimum_rec(oblock->page.frame);

      /* Set locks according to old locks */
      for (;;)
      {
        ulint old_heap_no;
        ulint new_heap_no;
        ut_d(const rec_t* const orec= rec1);
        ut_ad(page_rec_is_metadata(rec1) == page_rec_is_metadata(rec2));

        if (comp)
        {
          old_heap_no= rec_get_heap_no_new(rec2);
          new_heap_no= rec_get_heap_no_new(rec1);

          rec1= page_rec_get_next_low(rec1, TRUE);
          rec2= page_rec_get_next_low(rec2, TRUE);
        }
        else
        {
          old_heap_no= rec_get_heap_no_old(rec2);
          new_heap_no= rec_get_heap_no_old(rec1);
          ut_ad(!memcmp(rec1, rec2, rec_get_data_size_old(rec2)));

          rec1= page_rec_get_next_low(rec1, FALSE);
          rec2= page_rec_get_next_low(rec2, FALSE);
        }

        trx_t *lock_trx= lock->trx;
	lock_trx->mutex_lock();

	/* Clear the bit in old_lock. */
	if (old_heap_no < lock->un_member.rec_lock.n_bits &&
            lock_rec_reset_nth_bit(lock, old_heap_no))
        {
          ut_ad(!page_rec_is_metadata(orec));

          /* NOTE that the old lock bitmap could be too
          small for the new heap number! */
          lock_rec_add_to_queue(lock->type_mode, cell, id, block->page.frame,
                                new_heap_no, lock->index, lock_trx, true);
        }

	lock_trx->mutex_unlock();

        if (new_heap_no == PAGE_HEAP_NO_SUPREMUM)
        {
           ut_ad(old_heap_no == PAGE_HEAP_NO_SUPREMUM);
           break;
        }
      }

      ut_ad(lock_rec_find_set_bit(lock) == ULINT_UNDEFINED);
    }
  }

  mem_heap_free(heap);

#ifdef UNIV_DEBUG_LOCK_VALIDATE
  if (fil_space_t *space= fil_space_t::get(id.space()))
  {
    ut_ad(lock_rec_validate_page(block, space->is_latched()));
    space->release();
  }
#endif
}

/*************************************************************//**
Moves the explicit locks on user records to another page if a record
list end is moved to another page. */
TRANSACTIONAL_TARGET
void
lock_move_rec_list_end(
/*===================*/
	const buf_block_t*	new_block,	/*!< in: index page to move to */
	const buf_block_t*	block,		/*!< in: index page */
	const rec_t*		rec)		/*!< in: record on page: this
						is the first record moved */
{
  const ulint comp= page_rec_is_comp(rec);

  ut_ad(block->page.frame == page_align(rec));
  ut_ad(comp == page_is_comp(new_block->page.frame));

  const page_id_t id{block->page.id()};
  const page_id_t new_id{new_block->page.id()};
  {
    /* This would likely be too large for a memory transaction. */
    LockMultiGuard g{lock_sys.rec_hash, id, new_id};

    /* Note: when we move locks from record to record, waiting locks
    and possible granted gap type locks behind them are enqueued in
    the original order, because new elements are inserted to a hash
    table to the end of the hash chain, and lock_rec_add_to_queue
    does not reuse locks if there are waiters in the queue. */
    for (lock_t *lock= lock_sys_t::get_first(g.cell1(), id); lock;
         lock= lock_rec_get_next_on_page(lock))
    {
      const rec_t *rec1= rec;
      const rec_t *rec2;
      const auto type_mode= lock->type_mode;

      if (comp)
      {
        if (page_offset(rec1) == PAGE_NEW_INFIMUM)
          rec1= page_rec_get_next_low(rec1, TRUE);
        rec2= page_rec_get_next_low(new_block->page.frame + PAGE_NEW_INFIMUM,
                                    TRUE);
      }
      else
      {
        if (page_offset(rec1) == PAGE_OLD_INFIMUM)
          rec1= page_rec_get_next_low(rec1, FALSE);
        rec2= page_rec_get_next_low(new_block->page.frame + PAGE_OLD_INFIMUM,
                                    FALSE);
      }

      /* Copy lock requests on user records to new page and
      reset the lock bits on the old */
      for (;;)
      {
        ut_ad(page_rec_is_metadata(rec1) == page_rec_is_metadata(rec2));
        ut_d(const rec_t* const orec= rec1);

        ulint rec1_heap_no;
        ulint rec2_heap_no;

        if (comp)
        {
          rec1_heap_no= rec_get_heap_no_new(rec1);
          if (rec1_heap_no == PAGE_HEAP_NO_SUPREMUM)
            break;

          rec2_heap_no= rec_get_heap_no_new(rec2);
          rec1= page_rec_get_next_low(rec1, TRUE);
          rec2= page_rec_get_next_low(rec2, TRUE);
        }
        else
        {
          rec1_heap_no= rec_get_heap_no_old(rec1);

          if (rec1_heap_no == PAGE_HEAP_NO_SUPREMUM)
            break;
          rec2_heap_no= rec_get_heap_no_old(rec2);

          ut_ad(rec_get_data_size_old(rec1) == rec_get_data_size_old(rec2));
          ut_ad(!memcmp(rec1, rec2, rec_get_data_size_old(rec1)));

          rec1= page_rec_get_next_low(rec1, FALSE);
          rec2= page_rec_get_next_low(rec2, FALSE);
        }

        trx_t *lock_trx= lock->trx;
        lock_trx->mutex_lock();

        if (rec1_heap_no < lock->un_member.rec_lock.n_bits &&
            lock_rec_reset_nth_bit(lock, rec1_heap_no))
        {
          ut_ad(!page_rec_is_metadata(orec));

          if (type_mode & LOCK_WAIT)
          {
            ut_ad(lock_trx->lock.wait_lock == lock);
            lock->type_mode&= ~LOCK_WAIT;
          }

          lock_rec_add_to_queue(type_mode, g.cell2(), new_id,
                                new_block->page.frame,
                                rec2_heap_no, lock->index, lock_trx, true);
        }

        lock_trx->mutex_unlock();
      }
    }
  }

#ifdef UNIV_DEBUG_LOCK_VALIDATE
  if (fil_space_t *space= fil_space_t::get(id.space()))
  {
    const bool is_latched{space->is_latched()};
    ut_ad(lock_rec_validate_page(block, is_latched));
    ut_ad(lock_rec_validate_page(new_block, is_latched));
    space->release();
  }
#endif
}

/*************************************************************//**
Moves the explicit locks on user records to another page if a record
list start is moved to another page. */
TRANSACTIONAL_TARGET
void
lock_move_rec_list_start(
/*=====================*/
	const buf_block_t*	new_block,	/*!< in: index page to
						move to */
	const buf_block_t*	block,		/*!< in: index page */
	const rec_t*		rec,		/*!< in: record on page:
						this is the first
						record NOT copied */
	const rec_t*		old_end)	/*!< in: old
						previous-to-last
						record on new_page
						before the records
						were copied */
{
  const ulint comp= page_rec_is_comp(rec);

  ut_ad(block->page.frame == page_align(rec));
  ut_ad(comp == page_is_comp(new_block->page.frame));
  ut_ad(new_block->page.frame == page_align(old_end));
  ut_ad(!page_rec_is_metadata(rec));
  const page_id_t id{block->page.id()};
  const page_id_t new_id{new_block->page.id()};

  {
    /* This would likely be too large for a memory transaction. */
    LockMultiGuard g{lock_sys.rec_hash, id, new_id};

    for (lock_t *lock= lock_sys_t::get_first(g.cell1(), id); lock;
         lock= lock_rec_get_next_on_page(lock))
    {
      const rec_t *rec1;
      const rec_t *rec2;
      const auto type_mode= lock->type_mode;

      if (comp)
      {
        rec1= page_rec_get_next_low(block->page.frame + PAGE_NEW_INFIMUM,
                                    TRUE);
        rec2= page_rec_get_next_low(old_end, TRUE);
      }
      else
      {
        rec1= page_rec_get_next_low(block->page.frame + PAGE_OLD_INFIMUM,
                                    FALSE);
        rec2= page_rec_get_next_low(old_end, FALSE);
      }

      /* Copy lock requests on user records to new page and
      reset the lock bits on the old */

      while (rec1 != rec)
      {
        ut_ad(page_rec_is_metadata(rec1) == page_rec_is_metadata(rec2));
        ut_d(const rec_t* const prev= rec1);

        ulint rec1_heap_no;
        ulint rec2_heap_no;

        if (comp)
        {
          rec1_heap_no= rec_get_heap_no_new(rec1);
          rec2_heap_no= rec_get_heap_no_new(rec2);

          rec1= page_rec_get_next_low(rec1, TRUE);
          rec2= page_rec_get_next_low(rec2, TRUE);
        }
        else
        {
          rec1_heap_no= rec_get_heap_no_old(rec1);
          rec2_heap_no= rec_get_heap_no_old(rec2);

          ut_ad(!memcmp(rec1, rec2, rec_get_data_size_old(rec2)));

          rec1= page_rec_get_next_low(rec1, FALSE);
          rec2= page_rec_get_next_low(rec2, FALSE);
        }

        trx_t *lock_trx= lock->trx;
        lock_trx->mutex_lock();

        if (rec1_heap_no < lock->un_member.rec_lock.n_bits &&
            lock_rec_reset_nth_bit(lock, rec1_heap_no))
        {
          ut_ad(!page_rec_is_metadata(prev));

          if (type_mode & LOCK_WAIT)
          {
            ut_ad(lock_trx->lock.wait_lock == lock);
            lock->type_mode&= ~LOCK_WAIT;
          }

          lock_rec_add_to_queue(type_mode, g.cell2(), new_id,
                                new_block->page.frame,
                                rec2_heap_no, lock->index, lock_trx, true);
        }

        lock_trx->mutex_unlock();
      }

#ifdef UNIV_DEBUG
      if (page_rec_is_supremum(rec))
        for (auto i= lock_rec_get_n_bits(lock); --i > PAGE_HEAP_NO_USER_LOW; )
          ut_ad(!lock_rec_get_nth_bit(lock, i));
#endif /* UNIV_DEBUG */
    }
  }

#ifdef UNIV_DEBUG_LOCK_VALIDATE
  ut_ad(lock_rec_validate_page(block));
#endif
}

/*************************************************************//**
Moves the explicit locks on user records to another page if a record
list start is moved to another page. */
TRANSACTIONAL_TARGET
void
lock_rtr_move_rec_list(
/*===================*/
	const buf_block_t*	new_block,	/*!< in: index page to
						move to */
	const buf_block_t*	block,		/*!< in: index page */
	rtr_rec_move_t*		rec_move,       /*!< in: recording records
						moved */
	ulint			num_move)       /*!< in: num of rec to move */
{
  if (!num_move)
    return;

  const ulint comp= page_rec_is_comp(rec_move[0].old_rec);

  ut_ad(block->page.frame == page_align(rec_move[0].old_rec));
  ut_ad(new_block->page.frame == page_align(rec_move[0].new_rec));
  ut_ad(comp == page_rec_is_comp(rec_move[0].new_rec));
  const page_id_t id{block->page.id()};
  const page_id_t new_id{new_block->page.id()};

  {
    /* This would likely be too large for a memory transaction. */
    LockMultiGuard g{lock_sys.rec_hash, id, new_id};

    for (lock_t *lock= lock_sys_t::get_first(g.cell1(), id); lock;
         lock= lock_rec_get_next_on_page(lock))
    {
      const rec_t *rec1;
      const rec_t *rec2;
      const auto type_mode= lock->type_mode;

      /* Copy lock requests on user records to new page and
      reset the lock bits on the old */

      for (ulint moved= 0; moved < num_move; moved++)
      {
        ulint rec1_heap_no;
        ulint rec2_heap_no;

        rec1= rec_move[moved].old_rec;
        rec2= rec_move[moved].new_rec;
        ut_ad(!page_rec_is_metadata(rec1));
        ut_ad(!page_rec_is_metadata(rec2));

        if (comp)
        {
          rec1_heap_no= rec_get_heap_no_new(rec1);
          rec2_heap_no= rec_get_heap_no_new(rec2);
        }
        else
        {
          rec1_heap_no= rec_get_heap_no_old(rec1);
          rec2_heap_no= rec_get_heap_no_old(rec2);

          ut_ad(!memcmp(rec1, rec2, rec_get_data_size_old(rec2)));
        }

        trx_t *lock_trx= lock->trx;
        lock_trx->mutex_lock();

        if (rec1_heap_no < lock->un_member.rec_lock.n_bits &&
            lock_rec_reset_nth_bit(lock, rec1_heap_no))
        {
          if (type_mode & LOCK_WAIT)
          {
            ut_ad(lock_trx->lock.wait_lock == lock);
            lock->type_mode&= ~LOCK_WAIT;
          }

          lock_rec_add_to_queue(type_mode, g.cell2(), new_id,
                                new_block->page.frame,
                                rec2_heap_no, lock->index, lock_trx, true);

          rec_move[moved].moved= true;
        }

        lock_trx->mutex_unlock();
      }
    }
  }

#ifdef UNIV_DEBUG_LOCK_VALIDATE
  ut_ad(lock_rec_validate_page(block));
#endif
}
/*************************************************************//**
Updates the lock table when a page is split to the right. */
void
lock_update_split_right(
/*====================*/
	const buf_block_t*	right_block,	/*!< in: right page */
	const buf_block_t*	left_block)	/*!< in: left page */
{
  const ulint h= lock_get_min_heap_no(right_block);
  const page_id_t l{left_block->page.id()};
  const page_id_t r{right_block->page.id()};

  /* This would likely be too large for a memory transaction. */
  LockMultiGuard g{lock_sys.rec_hash, l, r};

  /* Move the locks on the supremum of the left page to the supremum
  of the right page */

  lock_rec_move(g.cell2(), *right_block, r, g.cell1(), l,
                PAGE_HEAP_NO_SUPREMUM, PAGE_HEAP_NO_SUPREMUM);

  /* Inherit the locks to the supremum of left page from the successor
  of the infimum on right page */
  lock_rec_inherit_to_gap(g.cell1(), l, g.cell2(), r, left_block->page.frame,
                          PAGE_HEAP_NO_SUPREMUM, h);
}

void lock_update_node_pointer(const buf_block_t *left_block,
                              const buf_block_t *right_block)
{
  const ulint h= lock_get_min_heap_no(right_block);
  const page_id_t l{left_block->page.id()};
  const page_id_t r{right_block->page.id()};
  LockMultiGuard g{lock_sys.rec_hash, l, r};

  lock_rec_inherit_to_gap(g.cell2(), r, g.cell1(), l, right_block->page.frame,
                          h, PAGE_HEAP_NO_SUPREMUM);
}

#ifdef UNIV_DEBUG
static void lock_assert_no_spatial(const page_id_t id)
{
  const auto id_fold= id.fold();
  auto cell= lock_sys.prdt_page_hash.cell_get(id_fold);
  auto latch= lock_sys_t::hash_table::latch(cell);
  latch->acquire();
  /* there should exist no page lock on the left page,
  otherwise, it will be blocked from merge */
  ut_ad(!lock_sys_t::get_first(*cell, id));
  latch->release();
  cell= lock_sys.prdt_hash.cell_get(id_fold);
  latch= lock_sys_t::hash_table::latch(cell);
  latch->acquire();
  ut_ad(!lock_sys_t::get_first(*cell, id));
  latch->release();
}
#endif

/*************************************************************//**
Updates the lock table when a page is merged to the right. */
void
lock_update_merge_right(
/*====================*/
	const buf_block_t*	right_block,	/*!< in: right page to
						which merged */
	const rec_t*		orig_succ,	/*!< in: original
						successor of infimum
						on the right page
						before merge */
	const buf_block_t*	left_block)	/*!< in: merged index
						page which will be
						discarded */
{
  ut_ad(!page_rec_is_metadata(orig_succ));

  const page_id_t l{left_block->page.id()};
  const page_id_t r{right_block->page.id()};
  /* This would likely be too large for a memory transaction. */
  LockMultiGuard g{lock_sys.rec_hash, l, r};

  /* Inherit the locks from the supremum of the left page to the
  original successor of infimum on the right page, to which the left
  page was merged */
  lock_rec_inherit_to_gap(g.cell2(), r, g.cell1(), l, right_block->page.frame,
                          page_rec_get_heap_no(orig_succ),
                          PAGE_HEAP_NO_SUPREMUM);

  /* Reset the locks on the supremum of the left page, releasing
  waiting transactions */
  lock_rec_reset_and_release_wait(g.cell1(), l, PAGE_HEAP_NO_SUPREMUM);
  lock_rec_free_all_from_discard_page(l, g.cell1(), lock_sys.rec_hash);

  ut_d(lock_assert_no_spatial(l));
}

/** Update locks when the root page is copied to another in
btr_root_raise_and_insert(). Note that we leave lock structs on the
root page, even though they do not make sense on other than leaf
pages: the reason is that in a pessimistic update the infimum record
of the root page will act as a dummy carrier of the locks of the record
to be updated. */
void lock_update_root_raise(const buf_block_t &block, const page_id_t root)
{
  const page_id_t id{block.page.id()};
  /* This would likely be too large for a memory transaction. */
  LockMultiGuard g{lock_sys.rec_hash, id, root};
  /* Move the locks on the supremum of the root to the supremum of block */
  lock_rec_move(g.cell1(), block, id, g.cell2(), root,
                PAGE_HEAP_NO_SUPREMUM, PAGE_HEAP_NO_SUPREMUM);
}

/** Update the lock table when a page is copied to another.
@param new_block  the target page
@param old        old page (not index root page) */
void lock_update_copy_and_discard(const buf_block_t &new_block, page_id_t old)
{
  const page_id_t id{new_block.page.id()};
  /* This would likely be too large for a memory transaction. */
  LockMultiGuard g{lock_sys.rec_hash, id, old};
  /* Move the locks on the supremum of the old page to the supremum of new */
  lock_rec_move(g.cell1(), new_block, id, g.cell2(), old,
                PAGE_HEAP_NO_SUPREMUM, PAGE_HEAP_NO_SUPREMUM);
  lock_rec_free_all_from_discard_page(old, g.cell2(), lock_sys.rec_hash);
}

/*************************************************************//**
Updates the lock table when a page is split to the left. */
void
lock_update_split_left(
/*===================*/
	const buf_block_t*	right_block,	/*!< in: right page */
	const buf_block_t*	left_block)	/*!< in: left page */
{
  ulint h= lock_get_min_heap_no(right_block);
  const page_id_t l{left_block->page.id()};
  const page_id_t r{right_block->page.id()};
  LockMultiGuard g{lock_sys.rec_hash, l, r};
  /* Inherit the locks to the supremum of the left page from the
  successor of the infimum on the right page */
  lock_rec_inherit_to_gap(g.cell1(), l, g.cell2(), r, left_block->page.frame,
                          PAGE_HEAP_NO_SUPREMUM, h);
}

/** Update the lock table when a page is merged to the left.
@param left      left page
@param orig_pred original predecessor of supremum on the left page before merge
@param right     merged, to-be-discarded right page */
void lock_update_merge_left(const buf_block_t& left, const rec_t *orig_pred,
                            const page_id_t right)
{
  ut_ad(left.page.frame == page_align(orig_pred));

  const page_id_t l{left.page.id()};

  /* This would likely be too large for a memory transaction. */
  LockMultiGuard g{lock_sys.rec_hash, l, right};
  const rec_t *left_next_rec= page_rec_get_next_const(orig_pred);

  if (!page_rec_is_supremum(left_next_rec))
  {
    /* Inherit the locks on the supremum of the left page to the
    first record which was moved from the right page */
    lock_rec_inherit_to_gap(g.cell1(), l, g.cell1(), l, left.page.frame,
                            page_rec_get_heap_no(left_next_rec),
                            PAGE_HEAP_NO_SUPREMUM);

    /* Reset the locks on the supremum of the left page,
    releasing waiting transactions */
    lock_rec_reset_and_release_wait(g.cell1(), l, PAGE_HEAP_NO_SUPREMUM);
  }

  /* Move the locks from the supremum of right page to the supremum
  of the left page */
  lock_rec_move(g.cell1(), left, l, g.cell2(), right,
                PAGE_HEAP_NO_SUPREMUM, PAGE_HEAP_NO_SUPREMUM);
  lock_rec_free_all_from_discard_page(right, g.cell2(), lock_sys.rec_hash);

  /* there should exist no page lock on the right page,
  otherwise, it will be blocked from merge */
  ut_d(lock_assert_no_spatial(right));
}

/*************************************************************//**
Resets the original locks on heir and replaces them with gap type locks
inherited from rec. */
void
lock_rec_reset_and_inherit_gap_locks(
/*=================================*/
	const buf_block_t&	heir_block,	/*!< in: block containing the
						record which inherits */
	const page_id_t		donor,		/*!< in: page containing the
						record from which inherited;
						does NOT reset the locks on
						this record */
	ulint			heir_heap_no,	/*!< in: heap_no of the
						inheriting record */
	ulint			heap_no)	/*!< in: heap_no of the
						donating record */
{
  const page_id_t heir{heir_block.page.id()};
  /* This is a rare operation and likely too large for a memory transaction. */
  LockMultiGuard g{lock_sys.rec_hash, heir, donor};
  lock_rec_reset_and_release_wait(g.cell1(), heir, heir_heap_no);
  lock_rec_inherit_to_gap(g.cell1(), heir, g.cell2(), donor,
                          heir_block.page.frame, heir_heap_no, heap_no);
}

/*************************************************************//**
Updates the lock table when a page is discarded. */
void
lock_update_discard(
/*================*/
	const buf_block_t*	heir_block,	/*!< in: index page
						which will inherit the locks */
	ulint			heir_heap_no,	/*!< in: heap_no of the record
						which will inherit the locks */
	const buf_block_t*	block)		/*!< in: index page
						which will be discarded */
{
	const page_t*	page = block->page.frame;
	const rec_t*	rec;
	ulint		heap_no;
	const page_id_t	heir(heir_block->page.id());
	const page_id_t	page_id(block->page.id());
	/* This would likely be too large for a memory transaction. */
	LockMultiGuard	g{lock_sys.rec_hash, heir, page_id};

	if (lock_sys_t::get_first(g.cell2(), page_id)) {
		ut_d(lock_assert_no_spatial(page_id));
		/* Inherit all the locks on the page to the record and
		reset all the locks on the page */

		if (page_is_comp(page)) {
			rec = page + PAGE_NEW_INFIMUM;

			do {
				heap_no = rec_get_heap_no_new(rec);

				lock_rec_inherit_to_gap(g.cell1(), heir,
							g.cell2(), page_id,
							heir_block->page.frame,
							heir_heap_no, heap_no);

				lock_rec_reset_and_release_wait(
					g.cell2(), page_id, heap_no);

				rec = page + rec_get_next_offs(rec, TRUE);
			} while (heap_no != PAGE_HEAP_NO_SUPREMUM);
		} else {
			rec = page + PAGE_OLD_INFIMUM;

			do {
				heap_no = rec_get_heap_no_old(rec);

				lock_rec_inherit_to_gap(g.cell1(), heir,
							g.cell2(), page_id,
							heir_block->page.frame,
							heir_heap_no, heap_no);

				lock_rec_reset_and_release_wait(
					g.cell2(), page_id, heap_no);

				rec = page + rec_get_next_offs(rec, FALSE);
			} while (heap_no != PAGE_HEAP_NO_SUPREMUM);
		}

		lock_rec_free_all_from_discard_page(page_id, g.cell2(),
						    lock_sys.rec_hash);
	} else {
		const auto fold = page_id.fold();
		auto cell = lock_sys.prdt_hash.cell_get(fold);
		auto latch = lock_sys_t::hash_table::latch(cell);
		latch->acquire();
		lock_rec_free_all_from_discard_page(page_id, *cell,
						    lock_sys.prdt_hash);
		latch->release();
		cell = lock_sys.prdt_page_hash.cell_get(fold);
		latch = lock_sys_t::hash_table::latch(cell);
		latch->acquire();
		lock_rec_free_all_from_discard_page(page_id, *cell,
						    lock_sys.prdt_page_hash);
		latch->release();
	}
}

/*************************************************************//**
Updates the lock table when a new user record is inserted. */
void
lock_update_insert(
/*===============*/
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec)	/*!< in: the inserted record */
{
	ulint	receiver_heap_no;
	ulint	donator_heap_no;

	ut_ad(block->page.frame == page_align(rec));
	ut_ad(!page_rec_is_metadata(rec));

	/* Inherit the gap-locking locks for rec, in gap mode, from the next
	record */

	if (page_rec_is_comp(rec)) {
		receiver_heap_no = rec_get_heap_no_new(rec);
		donator_heap_no = rec_get_heap_no_new(
			page_rec_get_next_low(rec, TRUE));
	} else {
		receiver_heap_no = rec_get_heap_no_old(rec);
		donator_heap_no = rec_get_heap_no_old(
			page_rec_get_next_low(rec, FALSE));
	}

	lock_rec_inherit_to_gap_if_gap_lock(
		block, receiver_heap_no, donator_heap_no);
}

/*************************************************************//**
Updates the lock table when a record is removed. */
void
lock_update_delete(
/*===============*/
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec)	/*!< in: the record to be removed */
{
	const page_t*	page = block->page.frame;
	ulint		heap_no;
	ulint		next_heap_no;

	ut_ad(page == page_align(rec));
	ut_ad(!page_rec_is_metadata(rec));

	if (page_is_comp(page)) {
		heap_no = rec_get_heap_no_new(rec);
		next_heap_no = rec_get_heap_no_new(page
						   + rec_get_next_offs(rec,
								       TRUE));
	} else {
		heap_no = rec_get_heap_no_old(rec);
		next_heap_no = rec_get_heap_no_old(page
						   + rec_get_next_offs(rec,
								       FALSE));
	}

	const page_id_t id{block->page.id()};
	LockGuard g{lock_sys.rec_hash, id};

	/* Let the next record inherit the locks from rec, in gap mode */

	lock_rec_inherit_to_gap(g.cell(), id, g.cell(), id, block->page.frame,
				next_heap_no, heap_no);

	/* Reset the lock bits on rec and release waiting transactions */
	lock_rec_reset_and_release_wait(g.cell(), id, heap_no);
}

/*********************************************************************//**
Stores on the page infimum record the explicit locks of another record.
This function is used to store the lock state of a record when it is
updated and the size of the record changes in the update. The record
is moved in such an update, perhaps to another page. The infimum record
acts as a dummy carrier record, taking care of lock releases while the
actual record is being moved. */
void
lock_rec_store_on_page_infimum(
/*===========================*/
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec)	/*!< in: record whose lock state
					is stored on the infimum
					record of the same page; lock
					bits are reset on the
					record */
{
  const ulint heap_no= page_rec_get_heap_no(rec);

  ut_ad(block->page.frame == page_align(rec));
  const page_id_t id{block->page.id()};

  LockGuard g{lock_sys.rec_hash, id};
  lock_rec_move(g.cell(), *block, id, g.cell(), id,
                PAGE_HEAP_NO_INFIMUM, heap_no);
}

/** Restore the explicit lock requests on a single record, where the
state was stored on the infimum of a page.
@param block   buffer block containing rec
@param rec     record whose lock state is restored
@param donator page (rec is not necessarily on this page)
whose infimum stored the lock state; lock bits are reset on the infimum */
void lock_rec_restore_from_page_infimum(const buf_block_t &block,
					const rec_t *rec, page_id_t donator)
{
  const ulint heap_no= page_rec_get_heap_no(rec);
  const page_id_t id{block.page.id()};
  LockMultiGuard g{lock_sys.rec_hash, id, donator};
  lock_rec_move(g.cell1(), block, id, g.cell2(), donator, heap_no,
                PAGE_HEAP_NO_INFIMUM);
}

/*========================= TABLE LOCKS ==============================*/

/**
Create a table lock, without checking for deadlocks or lock compatibility.
@param table      table on which the lock is created
@param type_mode  lock type and mode
@param trx        transaction
@param c_lock     conflicting lock
@return the created lock object */
lock_t *lock_table_create(dict_table_t *table, unsigned type_mode, trx_t *trx,
                          lock_t *c_lock)
{
	lock_t*		lock;

	lock_sys.assert_locked(*table);
	ut_ad(trx->mutex_is_owner());
	ut_ad(!trx->is_wsrep() || lock_sys.is_writer());
	ut_ad(trx->state == TRX_STATE_ACTIVE || trx->is_recovered);
	ut_ad(!trx->is_autocommit_non_locking());
	/* During CREATE TABLE, we will write to newly created FTS_*_CONFIG
	on which no lock has been created yet. */
	ut_ad(!trx->dict_operation_lock_mode
	      || (strstr(table->name.m_name, "/FTS_")
		  && strstr(table->name.m_name, "_CONFIG") + sizeof("_CONFIG")
		  == table->name.m_name + strlen(table->name.m_name) + 1));

	switch (LOCK_MODE_MASK & type_mode) {
	case LOCK_AUTO_INC:
		++table->n_waiting_or_granted_auto_inc_locks;
		/* For AUTOINC locking we reuse the lock instance only if
		there is no wait involved else we allocate the waiting lock
		from the transaction lock heap. */
		if (type_mode == LOCK_AUTO_INC) {
			lock = table->autoinc_lock;

			ut_ad(!table->autoinc_trx);
			table->autoinc_trx = trx;

			ib_vector_push(trx->autoinc_locks, &lock);
			goto allocated;
		}

		break;
	case LOCK_X:
	case LOCK_S:
		++table->n_lock_x_or_s;
		break;
	}

	lock = trx->lock.table_cached < array_elements(trx->lock.table_pool)
		? &trx->lock.table_pool[trx->lock.table_cached++]
		: static_cast<lock_t*>(
			mem_heap_alloc(trx->lock.lock_heap, sizeof *lock));

allocated:
	lock->type_mode = ib_uint32_t(type_mode | LOCK_TABLE);
	lock->trx = trx;

	lock->un_member.tab_lock.table = table;

	ut_ad(table->get_ref_count() > 0 || !table->can_be_evicted);

	UT_LIST_ADD_LAST(trx->lock.trx_locks, lock);

	ut_list_append(table->locks, lock, TableLockGetNode());

	if (type_mode & LOCK_WAIT) {
		if (trx->lock.wait_trx) {
			ut_ad(!c_lock || trx->lock.wait_trx == c_lock->trx);
			ut_ad(trx->lock.wait_lock);
			ut_ad((*trx->lock.wait_lock).trx == trx);
		} else {
			ut_ad(c_lock);
			trx->lock.wait_trx = c_lock->trx;
			ut_ad(!trx->lock.wait_lock);
		}
		trx->lock.wait_lock = lock;
	}

	lock->trx->lock.table_locks.push_back(lock);

	MONITOR_INC(MONITOR_TABLELOCK_CREATED);
	MONITOR_INC(MONITOR_NUM_TABLELOCK);

	return(lock);
}

/*************************************************************//**
Pops autoinc lock requests from the transaction's autoinc_locks. We
handle the case where there are gaps in the array and they need to
be popped off the stack. */
UNIV_INLINE
void
lock_table_pop_autoinc_locks(
/*=========================*/
	trx_t*	trx)	/*!< in/out: transaction that owns the AUTOINC locks */
{
	ut_ad(!ib_vector_is_empty(trx->autoinc_locks));

	/* Skip any gaps, gaps are NULL lock entries in the
	trx->autoinc_locks vector. */

	do {
		ib_vector_pop(trx->autoinc_locks);

		if (ib_vector_is_empty(trx->autoinc_locks)) {
			return;
		}

	} while (*(lock_t**) ib_vector_get_last(trx->autoinc_locks) == NULL);
}

/*************************************************************//**
Removes an autoinc lock request from the transaction's autoinc_locks. */
UNIV_INLINE
void
lock_table_remove_autoinc_lock(
/*===========================*/
	lock_t*	lock,	/*!< in: table lock */
	trx_t*	trx)	/*!< in/out: transaction that owns the lock */
{
	ut_ad(lock->type_mode == (LOCK_AUTO_INC | LOCK_TABLE));
	lock_sys.assert_locked(*lock->un_member.tab_lock.table);
	ut_ad(trx->mutex_is_owner());

	auto s = ib_vector_size(trx->autoinc_locks);
	ut_ad(s);

	/* With stored functions and procedures the user may drop
	a table within the same "statement". This special case has
	to be handled by deleting only those AUTOINC locks that were
	held by the table being dropped. */

	lock_t*	autoinc_lock = *static_cast<lock_t**>(
		ib_vector_get(trx->autoinc_locks, --s));

	/* This is the default fast case. */

	if (autoinc_lock == lock) {
		lock_table_pop_autoinc_locks(trx);
	} else {
		/* The last element should never be NULL */
		ut_a(autoinc_lock != NULL);

		/* Handle freeing the locks from within the stack. */

		while (s) {
			autoinc_lock = *static_cast<lock_t**>(
				ib_vector_get(trx->autoinc_locks, --s));

			if (autoinc_lock == lock) {
				void*	null_var = NULL;
				ib_vector_set(trx->autoinc_locks, s, &null_var);
				return;
			}
		}

		/* Must find the autoinc lock. */
		ut_error;
	}
}

/*************************************************************//**
Removes a table lock request from the queue and the trx list of locks;
this is a low-level function which does NOT check if waiting requests
can now be granted. */
UNIV_INLINE
const dict_table_t*
lock_table_remove_low(
/*==================*/
	lock_t*	lock)	/*!< in/out: table lock */
{
	ut_ad(lock->is_table());

	trx_t*		trx;
	dict_table_t*	table;

	ut_ad(lock->is_table());
	trx = lock->trx;
	table = lock->un_member.tab_lock.table;
	lock_sys.assert_locked(*table);
	ut_ad(trx->mutex_is_owner());

	/* Remove the table from the transaction's AUTOINC vector, if
	the lock that is being released is an AUTOINC lock. */
	switch (lock->mode()) {
	case LOCK_AUTO_INC:
		ut_ad((table->autoinc_trx == trx) == !lock->is_waiting());

		if (table->autoinc_trx == trx) {
			table->autoinc_trx = NULL;
			/* The locks must be freed in the reverse order from
			the one in which they were acquired. This is to avoid
			traversing the AUTOINC lock vector unnecessarily.

			We only store locks that were granted in the
			trx->autoinc_locks vector (see lock_table_create()
			and lock_grant()). */
			lock_table_remove_autoinc_lock(lock, trx);
		}

		ut_ad(table->n_waiting_or_granted_auto_inc_locks);
		--table->n_waiting_or_granted_auto_inc_locks;
		break;
	case LOCK_X:
	case LOCK_S:
		ut_ad(table->n_lock_x_or_s);
		--table->n_lock_x_or_s;
		break;
	default:
		break;
	}

	UT_LIST_REMOVE(trx->lock.trx_locks, lock);
	ut_list_remove(table->locks, lock, TableLockGetNode());

	MONITOR_INC(MONITOR_TABLELOCK_REMOVED);
	MONITOR_DEC(MONITOR_NUM_TABLELOCK);
	return table;
}

/*********************************************************************//**
Enqueues a waiting request for a table lock which cannot be granted
immediately. Checks for deadlocks.
@retval	DB_LOCK_WAIT	if the waiting lock was enqueued
@retval	DB_DEADLOCK	if this transaction was chosen as the victim */
static
dberr_t
lock_table_enqueue_waiting(
/*=======================*/
	unsigned	mode,	/*!< in: lock mode this transaction is
				requesting */
	dict_table_t*	table,	/*!< in/out: table */
	que_thr_t*	thr,	/*!< in: query thread */
	lock_t*		c_lock)	/*!< in: conflicting lock or NULL */
{
	lock_sys.assert_locked(*table);
	ut_ad(!srv_read_only_mode);

	trx_t* trx = thr_get_trx(thr);
	ut_ad(trx->mutex_is_owner());
	ut_ad(!trx->dict_operation_lock_mode);

#ifdef WITH_WSREP
	if (trx->is_wsrep() && trx->lock.was_chosen_as_deadlock_victim) {
		return(DB_DEADLOCK);
	}
#endif /* WITH_WSREP */

	/* Enqueue the lock request that will wait to be granted */
	lock_table_create(table, mode | LOCK_WAIT, trx, c_lock);

	trx->lock.wait_thr = thr;
	trx->lock.clear_deadlock_victim();

	MONITOR_INC(MONITOR_TABLELOCK_WAIT);
	return(DB_LOCK_WAIT);
}

/*********************************************************************//**
Checks if other transactions have an incompatible mode lock request in
the lock queue.
@return lock or NULL */
UNIV_INLINE
lock_t*
lock_table_other_has_incompatible(
/*==============================*/
	const trx_t*		trx,	/*!< in: transaction, or NULL if all
					transactions should be included */
	ulint			wait,	/*!< in: LOCK_WAIT if also
					waiting locks are taken into
					account, or 0 if not */
	const dict_table_t*	table,	/*!< in: table */
	lock_mode		mode)	/*!< in: lock mode */
{
	lock_sys.assert_locked(*table);

	static_assert(LOCK_IS == 0, "compatibility");
	static_assert(LOCK_IX == 1, "compatibility");

	if (UNIV_LIKELY(mode <= LOCK_IX && !table->n_lock_x_or_s)) {
		return(NULL);
	}

	for (lock_t* lock = UT_LIST_GET_LAST(table->locks);
	     lock;
	     lock = UT_LIST_GET_PREV(un_member.tab_lock.locks, lock)) {

		trx_t* lock_trx = lock->trx;

		if (lock_trx != trx
		    && !lock_mode_compatible(lock->mode(), mode)
		    && (wait || !lock->is_waiting())) {
			return(lock);
		}
	}

	return(NULL);
}

/** Aqcuire or enqueue a table lock */
static dberr_t lock_table_low(dict_table_t *table, lock_mode mode,
                              que_thr_t *thr, trx_t *trx)
{
  DBUG_EXECUTE_IF("innodb_table_deadlock", return DB_DEADLOCK;);
  lock_t *wait_for=
    lock_table_other_has_incompatible(trx, LOCK_WAIT, table, mode);
  dberr_t err= DB_SUCCESS;

  trx->mutex_lock();

  if (wait_for)
    err= lock_table_enqueue_waiting(mode, table, thr, wait_for);
  else
    lock_table_create(table, mode, trx, nullptr);

  trx->mutex_unlock();

  return err;
}

#ifdef WITH_WSREP
/** Aqcuire or enqueue a table lock in Galera replication mode. */
ATTRIBUTE_NOINLINE
static dberr_t lock_table_wsrep(dict_table_t *table, lock_mode mode,
                                que_thr_t *thr, trx_t *trx)
{
  LockMutexGuard g{SRW_LOCK_CALL};
  return lock_table_low(table, mode, thr, trx);
}
#endif

/** Acquire a table lock.
@param table   table to be locked
@param fktable pointer to table, in case of a FOREIGN key check
@param mode    lock mode
@param thr     SQL execution thread
@retval DB_SUCCESS    if the lock was acquired
@retval DB_DEADLOCK   if a deadlock occurred, or fktable && *fktable != table
@retval DB_LOCK_WAIT  if lock_wait() must be invoked */
dberr_t lock_table(dict_table_t *table, dict_table_t *const*fktable,
                   lock_mode mode, que_thr_t *thr)
{
  ut_ad(table);

  if (!fktable && table->is_temporary())
    return DB_SUCCESS;

  ut_ad(fktable || table->get_ref_count() || !table->can_be_evicted);

  trx_t *trx= thr_get_trx(thr);

  /* Look for equal or stronger locks the same trx already has on the
  table. No need to acquire LockMutexGuard here because only the
  thread that is executing a transaction can access trx_t::table_locks. */
  if (lock_table_has(trx, table, mode) || srv_read_only_mode)
    return DB_SUCCESS;

  if ((mode == LOCK_IX || mode == LOCK_X) &&
      !trx->read_only && !trx->rsegs.m_redo.rseg)
    trx_set_rw_mode(trx);

#ifdef WITH_WSREP
  if (trx->is_wsrep())
    return lock_table_wsrep(table, mode, thr, trx);
#endif
  lock_sys.rd_lock(SRW_LOCK_CALL);
  dberr_t err;
  if (fktable != nullptr && *fktable != table)
    err= DB_DEADLOCK;
  else
  {
    table->lock_mutex_lock();
    err= lock_table_low(table, mode, thr, trx);
    table->lock_mutex_unlock();
  }
  lock_sys.rd_unlock();

  return err;
}

/** Create a table lock object for a resurrected transaction.
@param table    table to be X-locked
@param trx      transaction
@param mode     LOCK_X or LOCK_IX */
void lock_table_resurrect(dict_table_t *table, trx_t *trx, lock_mode mode)
{
  ut_ad(trx->is_recovered);
  ut_ad(mode == LOCK_X || mode == LOCK_IX);

  if (lock_table_has(trx, table, mode))
    return;

  {
    /* This is executed at server startup while no connections
    are alowed. Do not bother with lock elision. */
    LockMutexGuard g{SRW_LOCK_CALL};
    ut_ad(!lock_table_other_has_incompatible(trx, LOCK_WAIT, table, mode));

    trx->mutex_lock();
    lock_table_create(table, mode, trx);
  }
  trx->mutex_unlock();
}

/** Find a lock that a waiting table lock request still has to wait for. */
static const lock_t *lock_table_has_to_wait_in_queue(const lock_t *wait_lock)
{
  ut_ad(wait_lock->is_waiting());
  ut_ad(wait_lock->is_table());

  dict_table_t *table= wait_lock->un_member.tab_lock.table;
  lock_sys.assert_locked(*table);

  static_assert(LOCK_IS == 0, "compatibility");
  static_assert(LOCK_IX == 1, "compatibility");

  if (UNIV_LIKELY(wait_lock->mode() <= LOCK_IX && !table->n_lock_x_or_s))
    return nullptr;

  for (const lock_t *lock= UT_LIST_GET_FIRST(table->locks); lock != wait_lock;
       lock= UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock))
    if (lock_has_to_wait(wait_lock, lock))
      return lock;

  return nullptr;
}

/*************************************************************//**
Removes a table lock request, waiting or granted, from the queue and grants
locks to other transactions in the queue, if they now are entitled to a
lock.
@param[in,out]	in_lock		table lock
@param[in]	owns_wait_mutex	whether lock_sys.wait_mutex is held */
static void lock_table_dequeue(lock_t *in_lock, bool owns_wait_mutex)
{
#ifdef SAFE_MUTEX
	ut_ad(owns_wait_mutex == mysql_mutex_is_owner(&lock_sys.wait_mutex));
#endif
	ut_ad(in_lock->trx->mutex_is_owner());
	lock_t*	lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, in_lock);

	const dict_table_t* table = lock_table_remove_low(in_lock);

	static_assert(LOCK_IS == 0, "compatibility");
	static_assert(LOCK_IX == 1, "compatibility");

	if (UNIV_LIKELY(in_lock->mode() <= LOCK_IX && !table->n_lock_x_or_s)) {
		return;
	}

	bool acquired = false;

	/* Check if waiting locks in the queue can now be granted: grant
	locks if there are no conflicting locks ahead. */

	for (/* No op */;
	     lock != NULL;
	     lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock)) {
		if (!lock->is_waiting()) {
			continue;
		}

		if (!owns_wait_mutex) {
			mysql_mutex_lock(&lock_sys.wait_mutex);
			acquired = owns_wait_mutex = true;
		}

		ut_ad(lock->trx->lock.wait_trx);
		ut_ad(lock->trx->lock.wait_lock);

		if (const lock_t* c = lock_table_has_to_wait_in_queue(lock)) {
			trx_t* c_trx = c->trx;
			lock->trx->lock.wait_trx = c_trx;
			if (c_trx->lock.wait_trx
			    && innodb_deadlock_detect
			    && Deadlock::to_check.emplace(c_trx).second) {
				Deadlock::to_be_checked = true;
			}
		} else {
			/* Grant the lock */
			ut_ad(in_lock->trx != lock->trx);
			in_lock->trx->mutex_unlock();
			lock_grant(lock);
			in_lock->trx->mutex_lock();
		}
	}

	if (acquired) {
		mysql_mutex_unlock(&lock_sys.wait_mutex);
	}
}


/** Sets a lock on a table based on the given mode.
@param table	table to lock
@param trx	transaction
@param mode	LOCK_X or LOCK_S
@param no_wait  whether to skip handling DB_LOCK_WAIT
@return error code */
dberr_t lock_table_for_trx(dict_table_t *table, trx_t *trx, lock_mode mode,
                           bool no_wait)
{
  mem_heap_t *heap= mem_heap_create(512);
  sel_node_t *node= sel_node_create(heap);
  que_thr_t *thr= pars_complete_graph_for_exec(node, trx, heap, nullptr);
  thr->graph->state= QUE_FORK_ACTIVE;

  thr= static_cast<que_thr_t*>
    (que_fork_get_first_thr(static_cast<que_fork_t*>
                            (que_node_get_parent(thr))));

run_again:
  thr->run_node= thr;
  thr->prev_node= thr->common.parent;
  dberr_t err= lock_table(table, nullptr, mode, thr);

  switch (err) {
  case DB_SUCCESS:
    break;
  case DB_LOCK_WAIT:
    if (no_wait)
    {
      lock_sys.cancel_lock_wait_for_trx(trx);
      break;
    }
    /* fall through */
  default:
    trx->error_state= err;
    if (row_mysql_handle_errors(&err, trx, thr, nullptr))
      goto run_again;
  }

  que_graph_free(thr->graph);
  trx->op_info= "";

  return err;
}

/** Exclusively lock the data dictionary tables.
@param trx  dictionary transaction
@return error code
@retval DB_SUCCESS on success */
dberr_t lock_sys_tables(trx_t *trx)
{
  dberr_t err;
  if (!(err= lock_table_for_trx(dict_sys.sys_tables, trx, LOCK_X)) &&
      !(err= lock_table_for_trx(dict_sys.sys_columns, trx, LOCK_X)) &&
      !(err= lock_table_for_trx(dict_sys.sys_indexes, trx, LOCK_X)) &&
      !(err= lock_table_for_trx(dict_sys.sys_fields, trx, LOCK_X)))
  {
    if (dict_sys.sys_foreign)
      err= lock_table_for_trx(dict_sys.sys_foreign, trx, LOCK_X);
    if (!err && dict_sys.sys_foreign_cols)
      err= lock_table_for_trx(dict_sys.sys_foreign_cols, trx, LOCK_X);
    if (!err && dict_sys.sys_virtual)
      err= lock_table_for_trx(dict_sys.sys_virtual, trx, LOCK_X);
  }
  return err;
}

/*=========================== LOCK RELEASE ==============================*/

/*************************************************************//**
Removes a granted record lock of a transaction from the queue and grants
locks to other transactions waiting in the queue if they now are entitled
to a lock. */
TRANSACTIONAL_TARGET
void
lock_rec_unlock(
/*============*/
	trx_t*			trx,	/*!< in/out: transaction that has
					set a record lock */
	const page_id_t		id,	/*!< in: page containing rec */
	const rec_t*		rec,	/*!< in: record */
	lock_mode		lock_mode)/*!< in: LOCK_S or LOCK_X */
{
	lock_t*		first_lock;
	lock_t*		lock;
	ulint		heap_no;

	ut_ad(trx);
	ut_ad(rec);
	ut_ad(!trx->lock.wait_lock);
	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE));
	ut_ad(!page_rec_is_metadata(rec));

	heap_no = page_rec_get_heap_no(rec);

	LockGuard g{lock_sys.rec_hash, id};

	first_lock = lock_sys_t::get_first(g.cell(), id, heap_no);

	/* Find the last lock with the same lock_mode and transaction
	on the record. */

	for (lock = first_lock; lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {
		if (lock->trx == trx && lock->mode() == lock_mode) {
			goto released;
		}
	}

	{
		ib::error	err;
		err << "Unlock row could not find a " << lock_mode
			<< " mode lock on the record. Current statement: ";
		size_t		stmt_len;
		if (const char* stmt = innobase_get_stmt_unsafe(
			    trx->mysql_thd, &stmt_len)) {
			err.write(stmt, stmt_len);
		}
	}

	return;

released:
	ut_a(!lock->is_waiting());
	{
		TMTrxGuard tg{*trx};
		lock_rec_reset_nth_bit(lock, heap_no);
	}

	/* Check if we can now grant waiting lock requests */

	for (lock = first_lock; lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {
		if (!lock->is_waiting()) {
			continue;
		}
		mysql_mutex_lock(&lock_sys.wait_mutex);
		ut_ad(lock->trx->lock.wait_trx);
		ut_ad(lock->trx->lock.wait_lock);

		if (const lock_t* c = lock_rec_has_to_wait_in_queue(g.cell(),
								    lock)) {
			lock->trx->lock.wait_trx = c->trx;
		} else {
			/* Grant the lock */
			ut_ad(trx != lock->trx);
			lock_grant(lock);
		}
		mysql_mutex_unlock(&lock_sys.wait_mutex);
	}
}

/** Release the explicit locks of a committing transaction,
and release possible other transactions waiting because of these locks.
@return whether the operation succeeded */
TRANSACTIONAL_TARGET static bool lock_release_try(trx_t *trx)
{
  /* At this point, trx->lock.trx_locks cannot be modified by other
  threads, because our transaction has been committed.
  See the checks and assertions in lock_rec_create_low() and
  lock_rec_add_to_queue().

  The function lock_table_create() should never be invoked on behalf
  of a transaction running in another thread. Also there, we will
  assert that the current transaction be active. */
  DBUG_ASSERT(trx->state == TRX_STATE_COMMITTED_IN_MEMORY);
  DBUG_ASSERT(!trx->is_referenced());

  bool all_released= true;
restart:
  ulint count= 1000;
  /* We will not attempt hardware lock elision (memory transaction)
  here. Both lock_rec_dequeue_from_page() and lock_table_dequeue()
  would likely lead to a memory transaction due to a system call, to
  wake up a waiting transaction. */
  lock_sys.rd_lock(SRW_LOCK_CALL);
  trx->mutex_lock();

  /* Note: Anywhere else, trx->mutex is not held while acquiring
  a lock table latch, but here we are following the opposite order.
  To avoid deadlocks, we only try to acquire the lock table latches
  but not keep waiting for them. */

  for (lock_t *lock= UT_LIST_GET_LAST(trx->lock.trx_locks); lock; )
  {
    ut_ad(lock->trx == trx);
    lock_t *prev= UT_LIST_GET_PREV(trx_locks, lock);
    if (!lock->is_table())
    {
      ut_ad(!lock->index->table->is_temporary());
      ut_ad(lock->mode() != LOCK_X ||
            lock->index->table->id >= DICT_HDR_FIRST_ID ||
            trx->dict_operation || trx->was_dict_operation);
      auto &lock_hash= lock_sys.hash_get(lock->type_mode);
      auto cell= lock_hash.cell_get(lock->un_member.rec_lock.page_id.fold());
      auto latch= lock_sys_t::hash_table::latch(cell);
      if (!latch->try_acquire())
        all_released= false;
      else
      {
        lock_rec_dequeue_from_page(lock, false);
        latch->release();
      }
    }
    else
    {
      dict_table_t *table= lock->un_member.tab_lock.table;
      ut_ad(!table->is_temporary());
      ut_ad(table->id >= DICT_HDR_FIRST_ID ||
            (lock->mode() != LOCK_IX && lock->mode() != LOCK_X) ||
            trx->dict_operation || trx->was_dict_operation);
      if (!table->lock_mutex_trylock())
        all_released= false;
      else
      {
        lock_table_dequeue(lock, false);
        table->lock_mutex_unlock();
      }
    }

    lock= all_released ? UT_LIST_GET_LAST(trx->lock.trx_locks) : prev;
    if (!--count)
      break;
  }

  lock_sys.rd_unlock();
  trx->mutex_unlock();
  if (all_released && !count)
    goto restart;
  return all_released;
}

/** Release the explicit locks of a committing transaction,
and release possible other transactions waiting because of these locks. */
void lock_release(trx_t *trx)
{
#ifdef UNIV_DEBUG
  std::set<table_id_t> to_evict;
  if (innodb_evict_tables_on_commit_debug &&
      !trx->is_recovered && !trx->dict_operation &&
      !trx->dict_operation_lock_mode)
    for (const auto& p : trx->mod_tables)
      if (!p.first->is_temporary())
        to_evict.emplace(p.first->id);
#endif
  ulint count;

  for (count= 5; count--; )
    if (lock_release_try(trx))
      goto released;

  /* Fall back to acquiring lock_sys.latch in exclusive mode */
restart:
  count= 1000;
  /* There is probably no point to try lock elision here;
  in lock_release_try() it is different. */
  lock_sys.wr_lock(SRW_LOCK_CALL);
  trx->mutex_lock();

  while (lock_t *lock= UT_LIST_GET_LAST(trx->lock.trx_locks))
  {
    ut_ad(lock->trx == trx);
    if (!lock->is_table())
    {
      ut_ad(!lock->index->table->is_temporary());
      ut_ad(lock->mode() != LOCK_X ||
            lock->index->table->id >= DICT_HDR_FIRST_ID ||
            trx->dict_operation || trx->was_dict_operation);
      lock_rec_dequeue_from_page(lock, false);
    }
    else
    {
      ut_d(dict_table_t *table= lock->un_member.tab_lock.table);
      ut_ad(!table->is_temporary());
      ut_ad(table->id >= DICT_HDR_FIRST_ID ||
            (lock->mode() != LOCK_IX && lock->mode() != LOCK_X) ||
            trx->dict_operation || trx->was_dict_operation);
      lock_table_dequeue(lock, false);
    }

    if (!--count)
      break;
  }

  lock_sys.wr_unlock();
  trx->mutex_unlock();
  if (!count)
    goto restart;

released:
  if (UNIV_UNLIKELY(Deadlock::to_be_checked))
  {
    mysql_mutex_lock(&lock_sys.wait_mutex);
    lock_sys.deadlock_check();
    mysql_mutex_unlock(&lock_sys.wait_mutex);
  }

  trx->lock.was_chosen_as_deadlock_victim= false;
  trx->lock.n_rec_locks= 0;

#ifdef UNIV_DEBUG
  if (to_evict.empty())
    return;
  dict_sys.lock(SRW_LOCK_CALL);
  LockMutexGuard g{SRW_LOCK_CALL};
  for (const table_id_t id : to_evict)
    if (dict_table_t *table= dict_sys.find_table(id))
      if (!table->get_ref_count() && !UT_LIST_GET_LEN(table->locks))
        dict_sys.remove(table, true);
  dict_sys.unlock();
#endif
}

/** Release the explicit locks of a committing transaction while
dict_sys.latch is exclusively locked,
and release possible other transactions waiting because of these locks. */
void lock_release_on_drop(trx_t *trx)
{
  ut_ad(lock_sys.is_writer());
  ut_ad(trx->mutex_is_owner());
  ut_ad(trx->dict_operation);

  while (lock_t *lock= UT_LIST_GET_LAST(trx->lock.trx_locks))
  {
    ut_ad(lock->trx == trx);
    if (!lock->is_table())
    {
      ut_ad(!lock->index->table->is_temporary());
      ut_ad(lock->mode() != LOCK_X ||
            lock->index->table->id >= DICT_HDR_FIRST_ID ||
            trx->dict_operation);
      lock_rec_dequeue_from_page(lock, false);
    }
    else
    {
      ut_d(dict_table_t *table= lock->un_member.tab_lock.table);
      ut_ad(!table->is_temporary());
      ut_ad(table->id >= DICT_HDR_FIRST_ID ||
            (lock->mode() != LOCK_IX && lock->mode() != LOCK_X) ||
            trx->dict_operation);
      lock_table_dequeue(lock, false);
    }
  }
}

/** Release non-exclusive locks on XA PREPARE,
and wake up possible other transactions waiting because of these locks.
@param trx   transaction in XA PREPARE state
@return whether all locks were released */
static bool lock_release_on_prepare_try(trx_t *trx)
{
  /* At this point, trx->lock.trx_locks can still be modified by other
  threads to convert implicit exclusive locks into explicit ones.

  The function lock_table_create() should never be invoked on behalf
  of a transaction that is running in another thread. Also there, we
  will assert that the current transaction be active. */
  DBUG_ASSERT(trx->state == TRX_STATE_PREPARED);

  bool all_released= true;
  lock_sys.rd_lock(SRW_LOCK_CALL);
  trx->mutex_lock();

  /* Note: Normally, trx->mutex is not held while acquiring
  a lock table latch, but here we are following the opposite order.
  To avoid deadlocks, we only try to acquire the lock table latches
  but not keep waiting for them. */

  for (lock_t *prev, *lock= UT_LIST_GET_LAST(trx->lock.trx_locks); lock;
       lock= prev)
  {
    ut_ad(lock->trx == trx);
    prev= UT_LIST_GET_PREV(trx_locks, lock);
    if (!lock->is_table())
    {
      ut_ad(!lock->index->table->is_temporary());
      if (lock->mode() == LOCK_X && !lock->is_gap())
        continue;
      auto &lock_hash= lock_sys.hash_get(lock->type_mode);
      auto cell= lock_hash.cell_get(lock->un_member.rec_lock.page_id.fold());
      auto latch= lock_sys_t::hash_table::latch(cell);
      if (latch->try_acquire())
      {
        lock_rec_dequeue_from_page(lock, false);
        latch->release();
      }
      else
        all_released= false;
    }
    else
    {
      dict_table_t *table= lock->un_member.tab_lock.table;
      ut_ad(!table->is_temporary());
      switch (lock->mode()) {
      case LOCK_IS:
      case LOCK_S:
        if (table->lock_mutex_trylock())
        {
          lock_table_dequeue(lock, false);
          table->lock_mutex_unlock();
        }
        else
          all_released= false;
        break;
      case LOCK_IX:
      case LOCK_X:
        ut_ad(table->id >= DICT_HDR_FIRST_ID || trx->dict_operation);
        /* fall through */
      default:
        break;
      }
    }
  }

  lock_sys.rd_unlock();
  trx->mutex_unlock();
  return all_released;
}

/** Release non-exclusive locks on XA PREPARE,
and release possible other transactions waiting because of these locks. */
void lock_release_on_prepare(trx_t *trx)
{
  for (ulint count= 5; count--; )
    if (lock_release_on_prepare_try(trx))
      return;

  LockMutexGuard g{SRW_LOCK_CALL};
  trx->mutex_lock();

  for (lock_t *prev, *lock= UT_LIST_GET_LAST(trx->lock.trx_locks); lock;
       lock= prev)
  {
    ut_ad(lock->trx == trx);
    prev= UT_LIST_GET_PREV(trx_locks, lock);
    if (!lock->is_table())
    {
      ut_ad(!lock->index->table->is_temporary());
      if (lock->mode() != LOCK_X || lock->is_gap())
        lock_rec_dequeue_from_page(lock, false);
    }
    else
    {
      ut_d(dict_table_t *table= lock->un_member.tab_lock.table);
      ut_ad(!table->is_temporary());
      switch (lock->mode()) {
      case LOCK_IS:
      case LOCK_S:
        lock_table_dequeue(lock, false);
        break;
      case LOCK_IX:
      case LOCK_X:
        ut_ad(table->id >= DICT_HDR_FIRST_ID || trx->dict_operation);
        /* fall through */
      default:
        break;
      }
    }
  }

  trx->mutex_unlock();
}

/** Release locks on a table whose creation is being rolled back */
ATTRIBUTE_COLD
void lock_release_on_rollback(trx_t *trx, dict_table_t *table)
{
  trx->mod_tables.erase(table);

  /* This is very rarely executed code, in the rare case that an
  CREATE TABLE operation is being rolled back. Theoretically,
  we might try to remove the locks in multiple memory transactions. */
  lock_sys.wr_lock(SRW_LOCK_CALL);
  trx->mutex_lock();

  for (lock_t *next, *lock= UT_LIST_GET_FIRST(table->locks); lock; lock= next)
  {
    next= UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock);
    ut_ad(lock->trx == trx);
    UT_LIST_REMOVE(trx->lock.trx_locks, lock);
    ut_list_remove(table->locks, lock, TableLockGetNode());
  }

  for (lock_t *p, *lock= UT_LIST_GET_LAST(trx->lock.trx_locks); lock; lock= p)
  {
    p= UT_LIST_GET_PREV(trx_locks, lock);
    ut_ad(lock->trx == trx);
    if (lock->is_table())
      ut_ad(lock->un_member.tab_lock.table != table);
    else if (lock->index->table == table)
      lock_rec_dequeue_from_page(lock, false);
  }

  lock_sys.wr_unlock();
  trx->mutex_unlock();
}

/*********************************************************************//**
Removes table locks of the transaction on a table to be dropped. */
static
void
lock_trx_table_locks_remove(
/*========================*/
	const lock_t*	lock_to_remove)		/*!< in: lock to remove */
{
	trx_t*		trx = lock_to_remove->trx;

	ut_ad(lock_to_remove->is_table());
	lock_sys.assert_locked(*lock_to_remove->un_member.tab_lock.table);
	ut_ad(trx->mutex_is_owner());

	for (lock_list::iterator it = trx->lock.table_locks.begin(),
             end = trx->lock.table_locks.end(); it != end; ++it) {
		const lock_t*	lock = *it;

		ut_ad(!lock || trx == lock->trx);
		ut_ad(!lock || lock->is_table());
		ut_ad(!lock || lock->un_member.tab_lock.table);

		if (lock == lock_to_remove) {
			*it = NULL;
			return;
		}
	}

	/* Lock must exist in the vector. */
	ut_error;
}

/*===================== VALIDATION AND DEBUGGING ====================*/

/** Print info of a table lock.
@param[in,out]	file	output stream
@param[in]	lock	table lock */
static
void
lock_table_print(FILE* file, const lock_t* lock)
{
	lock_sys.assert_locked();
	ut_a(lock->is_table());

	fputs("TABLE LOCK table ", file);
	ut_print_name(file, lock->trx,
		      lock->un_member.tab_lock.table->name.m_name);
	fprintf(file, " trx id " TRX_ID_FMT, lock->trx->id);

	switch (auto mode = lock->mode()) {
	case LOCK_S:
		fputs(" lock mode S", file);
		break;
	case LOCK_X:
		ut_ad(lock->trx->id != 0);
		fputs(" lock mode X", file);
		break;
	case LOCK_IS:
		fputs(" lock mode IS", file);
		break;
	case LOCK_IX:
		ut_ad(lock->trx->id != 0);
		fputs(" lock mode IX", file);
		break;
	case LOCK_AUTO_INC:
		fputs(" lock mode AUTO-INC", file);
		break;
	default:
		fprintf(file, " unknown lock mode %u", mode);
	}

	if (lock->is_waiting()) {
		fputs(" waiting", file);
	}

	putc('\n', file);
}

/** Pretty-print a record lock.
@param[in,out]	file	output stream
@param[in]	lock	record lock
@param[in,out]	mtr	mini-transaction for accessing the record */
static void lock_rec_print(FILE* file, const lock_t* lock, mtr_t& mtr)
{
	ut_ad(!lock->is_table());

	const page_id_t page_id{lock->un_member.rec_lock.page_id};
	ut_d(lock_sys.hash_get(lock->type_mode).assert_locked(page_id));

	fprintf(file, "RECORD LOCKS space id %u page no %u n bits " ULINTPF
		" index %s of table ",
		page_id.space(), page_id.page_no(),
		lock_rec_get_n_bits(lock),
		lock->index->name());
	ut_print_name(file, lock->trx, lock->index->table->name.m_name);
	fprintf(file, " trx id " TRX_ID_FMT, lock->trx->id);

	switch (lock->mode()) {
	case LOCK_S:
		fputs(" lock mode S", file);
		break;
	case LOCK_X:
		fputs(" lock_mode X", file);
		break;
	default:
		ut_error;
	}

	if (lock->is_gap()) {
		fputs(" locks gap before rec", file);
	}

	if (lock->is_record_not_gap()) {
		fputs(" locks rec but not gap", file);
	}

	if (lock->is_insert_intention()) {
		fputs(" insert intention", file);
	}

	if (lock->is_waiting()) {
		fputs(" waiting", file);
	}

	putc('\n', file);

	mem_heap_t*		heap		= NULL;
	rec_offs		offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*		offsets		= offsets_;
	rec_offs_init(offsets_);

	mtr.start();
	const buf_block_t* block = buf_page_try_get(page_id, &mtr);

	for (ulint i = 0; i < lock_rec_get_n_bits(lock); ++i) {

		if (!lock_rec_get_nth_bit(lock, i)) {
			continue;
		}

		fprintf(file, "Record lock, heap no %lu", (ulong) i);

		if (block) {
			ut_ad(page_is_leaf(block->page.frame));
			const rec_t*	rec;

			rec = page_find_rec_with_heap_no(
				buf_block_get_frame(block), i);
			ut_ad(!page_rec_is_metadata(rec));

			offsets = rec_get_offsets(
				rec, lock->index, offsets,
				lock->index->n_core_fields,
				ULINT_UNDEFINED, &heap);

			putc(' ', file);
			rec_print_new(file, rec, offsets);
		}

		putc('\n', file);
	}

	mtr.commit();

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
}

#ifdef UNIV_DEBUG
/* Print the number of lock structs from lock_print_info_summary() only
in non-production builds for performance reasons, see
http://bugs.mysql.com/36942 */
#define PRINT_NUM_OF_LOCK_STRUCTS
#endif /* UNIV_DEBUG */

#ifdef PRINT_NUM_OF_LOCK_STRUCTS
/*********************************************************************//**
Calculates the number of record lock structs in the record lock hash table.
@return number of record locks */
TRANSACTIONAL_TARGET
static ulint lock_get_n_rec_locks()
{
	ulint	n_locks	= 0;
	ulint	i;

	lock_sys.assert_locked();

	for (i = 0; i < lock_sys.rec_hash.n_cells; i++) {
		const lock_t*	lock;

		for (lock = static_cast<const lock_t*>(
			     HASH_GET_FIRST(&lock_sys.rec_hash, i));
		     lock != 0;
		     lock = static_cast<const lock_t*>(
				HASH_GET_NEXT(hash, lock))) {

			n_locks++;
		}
	}

	return(n_locks);
}
#endif /* PRINT_NUM_OF_LOCK_STRUCTS */

/*********************************************************************//**
Prints info of locks for all transactions.
@return FALSE if not able to acquire lock_sys.latch (and dislay info) */
ibool
lock_print_info_summary(
/*====================*/
	FILE*	file,	/*!< in: file where to print */
	ibool	nowait)	/*!< in: whether to wait for lock_sys.latch */
{
	/* Here, lock elision does not make sense, because
	for the output we are going to invoke system calls,
	which would interrupt a memory transaction. */
	if (!nowait) {
		lock_sys.wr_lock(SRW_LOCK_CALL);
	} else if (!lock_sys.wr_lock_try()) {
		fputs("FAIL TO OBTAIN LOCK MUTEX,"
		      " SKIP LOCK INFO PRINTING\n", file);
		return(FALSE);
	}

	if (lock_sys.deadlocks) {
		fputs("------------------------\n"
		      "LATEST DETECTED DEADLOCK\n"
		      "------------------------\n", file);

		if (!srv_read_only_mode) {
			ut_copy_file(file, lock_latest_err_file);
		}
	}

	fputs("------------\n"
	      "TRANSACTIONS\n"
	      "------------\n", file);

	fprintf(file, "Trx id counter " TRX_ID_FMT "\n",
		trx_sys.get_max_trx_id());

	fprintf(file,
		"Purge done for trx's n:o < " TRX_ID_FMT
		" undo n:o < " TRX_ID_FMT " state: %s\n"
		"History list length %u\n",
		purge_sys.tail.trx_no,
		purge_sys.tail.undo_no,
		purge_sys.enabled()
		? (purge_sys.running() ? "running"
		   : purge_sys.paused() ? "stopped" : "running but idle")
		: "disabled",
		trx_sys.history_size());

#ifdef PRINT_NUM_OF_LOCK_STRUCTS
	fprintf(file,
		"Total number of lock structs in row lock hash table %lu\n",
		(ulong) lock_get_n_rec_locks());
#endif /* PRINT_NUM_OF_LOCK_STRUCTS */
	return(TRUE);
}

/** Prints transaction lock wait and MVCC state.
@param[in,out]	file	file where to print
@param[in]	trx	transaction
@param[in]	now	current my_hrtime_coarse() */
void lock_trx_print_wait_and_mvcc_state(FILE *file, const trx_t *trx,
                                        my_hrtime_t now)
{
	fprintf(file, "---");

	trx_print_latched(file, trx, 600);
	trx->read_view.print_limits(file);

	if (const lock_t* wait_lock = trx->lock.wait_lock) {
		const my_hrtime_t suspend_time= trx->lock.suspend_time;
		fprintf(file,
			"------- TRX HAS BEEN WAITING %llu ns"
			" FOR THIS LOCK TO BE GRANTED:\n",
			now.val - suspend_time.val);

		if (!wait_lock->is_table()) {
			mtr_t mtr;
			lock_rec_print(file, wait_lock, mtr);
		} else {
			lock_table_print(file, wait_lock);
		}

		fprintf(file, "------------------\n");
	}
}

/*********************************************************************//**
Prints info of locks for a transaction. */
static
void
lock_trx_print_locks(
/*=================*/
	FILE*		file,		/*!< in/out: File to write */
	const trx_t*	trx)		/*!< in: current transaction */
{
	mtr_t mtr;
	uint32_t i= 0;
	/* Iterate over the transaction's locks. */
	lock_sys.assert_locked();
	for (lock_t *lock = UT_LIST_GET_FIRST(trx->lock.trx_locks);
	     lock != NULL;
	     lock = UT_LIST_GET_NEXT(trx_locks, lock)) {
		if (!lock->is_table()) {
			lock_rec_print(file, lock, mtr);
		} else {
			lock_table_print(file, lock);
		}

		if (++i == 10) {

			fprintf(file,
				"10 LOCKS PRINTED FOR THIS TRX:"
				" SUPPRESSING FURTHER PRINTS\n");

			break;
		}
	}
}

/** Functor to display all transactions */
struct lock_print_info
{
  lock_print_info(FILE* file, my_hrtime_t now) :
    file(file), now(now),
    purge_trx(purge_sys.query ? purge_sys.query->trx : nullptr)
  {}

  void operator()(const trx_t &trx) const
  {
    if (UNIV_UNLIKELY(&trx == purge_trx))
      return;
    lock_trx_print_wait_and_mvcc_state(file, &trx, now);

    if (trx.will_lock && srv_print_innodb_lock_monitor)
      lock_trx_print_locks(file, &trx);
  }

  FILE* const file;
  const my_hrtime_t now;
  const trx_t* const purge_trx;
};

/*********************************************************************//**
Prints info of locks for each transaction. This function will release
lock_sys.latch, which the caller must be holding in exclusive mode. */
void
lock_print_info_all_transactions(
/*=============================*/
	FILE*		file)	/*!< in/out: file where to print */
{
	fprintf(file, "LIST OF TRANSACTIONS FOR EACH SESSION:\n");

	trx_sys.trx_list.for_each(lock_print_info(file, my_hrtime_coarse()));
	lock_sys.wr_unlock();

	ut_d(lock_validate());
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Find the the lock in the trx_t::trx_lock_t::table_locks vector.
@return true if found */
static
bool
lock_trx_table_locks_find(
/*======================*/
	trx_t*		trx,		/*!< in: trx to validate */
	const lock_t*	find_lock)	/*!< in: lock to find */
{
	bool		found = false;

	ut_ad(trx->mutex_is_owner());

	for (lock_list::const_iterator it = trx->lock.table_locks.begin(),
             end = trx->lock.table_locks.end(); it != end; ++it) {

		const lock_t*	lock = *it;

		if (lock == NULL) {

			continue;

		} else if (lock == find_lock) {

			/* Can't be duplicates. */
			ut_a(!found);
			found = true;
		}

		ut_a(trx == lock->trx);
		ut_a(lock->is_table());
		ut_a(lock->un_member.tab_lock.table != NULL);
	}

	return(found);
}

/*********************************************************************//**
Validates the lock queue on a table.
@return TRUE if ok */
static
ibool
lock_table_queue_validate(
/*======================*/
	const dict_table_t*	table)	/*!< in: table */
{
	const lock_t*	lock;

	lock_sys.assert_locked(*table);

	for (lock = UT_LIST_GET_FIRST(table->locks);
	     lock != NULL;
	     lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock)) {

		/* lock->trx->state cannot change from or to NOT_STARTED
		while we are holding the lock_sys.latch. It may change
		from ACTIVE or PREPARED to PREPARED or COMMITTED. */
		lock->trx->mutex_lock();
		check_trx_state(lock->trx);

		if (lock->trx->state == TRX_STATE_COMMITTED_IN_MEMORY) {
		} else if (!lock->is_waiting()) {
			ut_a(!lock_table_other_has_incompatible(
				     lock->trx, 0, table,
				     lock->mode()));
		} else {
			ut_a(lock_table_has_to_wait_in_queue(lock));
		}

		ut_a(lock_trx_table_locks_find(lock->trx, lock));
		lock->trx->mutex_unlock();
	}

	return(TRUE);
}

/*********************************************************************//**
Validates the lock queue on a single record.
@return TRUE if ok */
static
bool
lock_rec_queue_validate(
/*====================*/
	bool			locked_lock_trx_sys,
					/*!< in: if the caller holds
					both the lock_sys.latch and
					trx_sys_t->lock. */
	const page_id_t		id,	/*!< in: page identifier */
	const rec_t*		rec,	/*!< in: record to look at */
	const dict_index_t*	index,	/*!< in: index, or NULL if not known */
	const rec_offs*		offsets)/*!< in: rec_get_offsets(rec, index) */
{
	const lock_t*	lock;
	ulint		heap_no;

	ut_a(rec);
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!page_rec_is_comp(rec) == !rec_offs_comp(offsets));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(!index || dict_index_is_clust(index)
	      || !dict_index_is_online_ddl(index));

	heap_no = page_rec_get_heap_no(rec);

	if (!locked_lock_trx_sys) {
		lock_sys.wr_lock(SRW_LOCK_CALL);
	}

	hash_cell_t &cell= *lock_sys.rec_hash.cell_get(id.fold());
	lock_sys.assert_locked(cell);

	if (!page_rec_is_user_rec(rec)) {

		for (lock = lock_sys_t::get_first(cell, id, heap_no);
		     lock != NULL;
		     lock = lock_rec_get_next_const(heap_no, lock)) {

			ut_ad(!index || lock->index == index);

			lock->trx->mutex_lock();
			ut_ad(!lock->trx->read_only
			      || !lock->trx->is_autocommit_non_locking());
			ut_ad(trx_state_eq(lock->trx,
					   TRX_STATE_COMMITTED_IN_MEMORY)
			      || !lock->is_waiting()
			      || lock_rec_has_to_wait_in_queue(cell, lock));
			lock->trx->mutex_unlock();
		}

func_exit:
		if (!locked_lock_trx_sys) {
			lock_sys.wr_unlock();
		}

		return true;
	}

	ut_ad(page_rec_is_leaf(rec));

	const trx_id_t impl_trx_id = index && index->is_primary()
		? lock_clust_rec_some_has_impl(rec, index, offsets)
		: 0;

	if (trx_t *impl_trx = impl_trx_id
	    ? trx_sys.find(current_trx(), impl_trx_id, false)
	    : 0) {
		/* impl_trx could have been committed before we
		acquire its mutex, but not thereafter. */

		impl_trx->mutex_lock();
		ut_ad(impl_trx->state != TRX_STATE_NOT_STARTED);
		if (impl_trx->state == TRX_STATE_COMMITTED_IN_MEMORY) {
		} else if (const lock_t* other_lock
			   = lock_rec_other_has_expl_req(
				   LOCK_S, cell, id, true, heap_no,
				   impl_trx)) {
			/* The impl_trx is holding an implicit lock on the
			given record 'rec'. So there cannot be another
			explicit granted lock.  Also, there can be another
			explicit waiting lock only if the impl_trx has an
			explicit granted lock. */

#ifdef WITH_WSREP
			/** Galera record locking rules:
			* If there is no other record lock to the same record, we may grant
			the lock request.
			* If there is other record lock but this requested record lock is
			compatible, we may grant the lock request.
			* If there is other record lock and it is not compatible with
			requested lock, all normal transactions must wait.
			* BF (brute force) additional exceptions :
			** If BF already holds record lock for requested record, we may
			grant new record lock even if there is conflicting record lock(s)
			waiting on a queue.
			** If conflicting transaction holds requested record lock,
			we will cancel this record lock and select conflicting transaction
			for BF abort or kill victim.
			** If conflicting transaction is waiting for requested record lock
			we will cancel this wait and select conflicting transaction
			for BF abort or kill victim.
			** There should not be two BF transactions waiting for same record lock
			*/
			if (other_lock->trx->is_wsrep() && !other_lock->is_waiting()) {
				wsrep_report_bf_lock_wait(impl_trx->mysql_thd, impl_trx->id);
				wsrep_report_bf_lock_wait(other_lock->trx->mysql_thd, other_lock->trx->id);

				if (!lock_rec_has_expl(LOCK_X | LOCK_REC_NOT_GAP,
						       cell, id, heap_no,
						       impl_trx)) {
					ib::info() << "WSREP impl BF lock conflict";
				}
			} else
#endif /* WITH_WSREP */
			{
				ut_ad(other_lock->is_waiting());
				ut_ad(lock_rec_has_expl(LOCK_X | LOCK_REC_NOT_GAP,
						        cell, id, heap_no,
							impl_trx));
			}
		}

		impl_trx->mutex_unlock();
	}

	for (lock = lock_sys_t::get_first(cell, id, heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next_const(heap_no, lock)) {
		ut_ad(!lock->trx->read_only
		      || !lock->trx->is_autocommit_non_locking());
		ut_ad(!page_rec_is_metadata(rec));

		if (index) {
			ut_a(lock->index == index);
		}

		if (lock->is_waiting()) {
			ut_a(lock->is_gap()
			     || lock_rec_has_to_wait_in_queue(cell, lock));
		} else if (!lock->is_gap()) {
			const lock_mode	mode = lock->mode() == LOCK_S
				? LOCK_X : LOCK_S;

			const lock_t*	other_lock
				= lock_rec_other_has_expl_req(
					mode, cell, id, false, heap_no,
					lock->trx);
#ifdef WITH_WSREP
			if (UNIV_UNLIKELY(other_lock && lock->trx->is_wsrep())) {
				/* Only BF transaction may be granted
				lock before other conflicting lock
				request. */
				if (!wsrep_thd_is_BF(lock->trx->mysql_thd, FALSE)
				    && !wsrep_thd_is_BF(other_lock->trx->mysql_thd, FALSE)) {
					/* If no BF, this case is a bug. */
					wsrep_report_bf_lock_wait(lock->trx->mysql_thd, lock->trx->id);
					wsrep_report_bf_lock_wait(other_lock->trx->mysql_thd, other_lock->trx->id);
					ut_error;
				}
			} else
#endif /* WITH_WSREP */
			ut_ad(!other_lock);
		}
	}

	goto func_exit;
}

/** Validate the record lock queues on a page.
@param block    buffer pool block
@param latched  whether the tablespace latch may be held
@return true if ok */
static bool lock_rec_validate_page(const buf_block_t *block, bool latched)
{
	const lock_t*	lock;
	const rec_t*	rec;
	ulint		nth_lock	= 0;
	ulint		nth_bit		= 0;
	ulint		i;
	mem_heap_t*	heap		= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;
	rec_offs_init(offsets_);

	const page_id_t id{block->page.id()};

	LockGuard g{lock_sys.rec_hash, id};
loop:
	lock = lock_sys_t::get_first(g.cell(), id);

	if (!lock) {
		goto function_exit;
	}

	DBUG_ASSERT(!block->page.is_freed());

	for (i = 0; i < nth_lock; i++) {

		lock = lock_rec_get_next_on_page_const(lock);

		if (!lock) {
			goto function_exit;
		}
	}

	ut_ad(!lock->trx->read_only
	      || !lock->trx->is_autocommit_non_locking());

	/* Only validate the record queues when this thread is not
	holding a tablespace latch. */
	if (!latched)
	for (i = nth_bit; i < lock_rec_get_n_bits(lock); i++) {
		bool locked = lock_rec_get_nth_bit(lock, i);
		if (locked || i == PAGE_HEAP_NO_SUPREMUM) {

			rec = page_find_rec_with_heap_no(block->page.frame, i);
			ut_a(rec);
			ut_ad(!locked || page_rec_is_leaf(rec));

			/* If this thread is holding the file space
			latch (fil_space_t::latch), the following
			check WILL break the latching order and may
			cause a deadlock of threads. */

			if (locked) {
				offsets = rec_get_offsets(rec, lock->index,
					offsets, lock->index->n_core_fields,
					ULINT_UNDEFINED, &heap);
				lock_rec_queue_validate(true, id, rec,
					lock->index, offsets);
			}

			nth_bit = i + 1;

			goto loop;
		}
	}

	nth_bit = 0;
	nth_lock++;

	goto loop;

function_exit:
	if (heap != NULL) {
		mem_heap_free(heap);
	}
	return(TRUE);
}

/*********************************************************************//**
Validate record locks up to a limit.
@return lock at limit or NULL if no more locks in the hash bucket */
static MY_ATTRIBUTE((warn_unused_result))
const lock_t*
lock_rec_validate(
/*==============*/
	ulint		start,		/*!< in: lock_sys.rec_hash
					bucket */
	page_id_t*	limit)		/*!< in/out: upper limit of
					(space, page_no) */
{
	lock_sys.assert_locked();

	for (const lock_t* lock = static_cast<const lock_t*>(
		     HASH_GET_FIRST(&lock_sys.rec_hash, start));
	     lock != NULL;
	     lock = static_cast<const lock_t*>(HASH_GET_NEXT(hash, lock))) {

		ut_ad(!lock->trx->read_only
		      || !lock->trx->is_autocommit_non_locking());
		ut_ad(!lock->is_table());

		page_id_t current(lock->un_member.rec_lock.page_id);

		if (current > *limit) {
			*limit = current + 1;
			return(lock);
		}
	}

	return(0);
}

/*********************************************************************//**
Validate a record lock's block */
static void lock_rec_block_validate(const page_id_t page_id)
{
	/* The lock and the block that it is referring to may be freed at
	this point. We pass BUF_GET_POSSIBLY_FREED to skip a debug check.
	If the lock exists in lock_rec_validate_page() we assert
	block->page.status != FREED. */

	buf_block_t*	block;
	mtr_t		mtr;

	/* Transactional locks should never refer to dropped
	tablespaces, because all DDL operations that would drop or
	discard or rebuild a tablespace do hold an exclusive table
	lock, which would conflict with any locks referring to the
	tablespace from other transactions. */
	if (fil_space_t* space = fil_space_t::get(page_id.space())) {
		dberr_t err = DB_SUCCESS;
		mtr_start(&mtr);

		block = buf_page_get_gen(
			page_id,
			space->zip_size(),
			RW_X_LATCH, NULL,
			BUF_GET_POSSIBLY_FREED,
			&mtr, &err);

		ut_ad(!block || block->page.is_freed()
		      || lock_rec_validate_page(block, space->is_latched()));

		mtr_commit(&mtr);

		space->release();
	}
}

static my_bool lock_validate_table_locks(rw_trx_hash_element_t *element, void*)
{
  lock_sys.assert_locked();
  element->mutex.wr_lock();
  if (element->trx)
  {
    check_trx_state(element->trx);
    for (const lock_t *lock= UT_LIST_GET_FIRST(element->trx->lock.trx_locks);
         lock != NULL;
         lock= UT_LIST_GET_NEXT(trx_locks, lock))
      if (lock->is_table())
        lock_table_queue_validate(lock->un_member.tab_lock.table);
  }
  element->mutex.wr_unlock();
  return 0;
}


/** Validate the transactional locks. */
static void lock_validate()
{
  std::set<page_id_t> pages;
  {
    LockMutexGuard g{SRW_LOCK_CALL};
    /* Validate table locks */
    trx_sys.rw_trx_hash.iterate(lock_validate_table_locks);

    for (ulint i= 0; i < lock_sys.rec_hash.n_cells; i++)
    {
      page_id_t limit{0, 0};
      while (const lock_t *lock= lock_rec_validate(i, &limit))
      {
        if (lock_rec_find_set_bit(lock) == ULINT_UNDEFINED)
          /* The lock bitmap is empty; ignore it. */
          continue;
        pages.insert(lock->un_member.rec_lock.page_id);
      }
    }
  }

  for (page_id_t page_id : pages)
    lock_rec_block_validate(page_id);
}
#endif /* UNIV_DEBUG */
/*============ RECORD LOCK CHECKS FOR ROW OPERATIONS ====================*/

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate insert of
a record. If they do, first tests if the query thread should anyway
be suspended for some reason; if not, then puts the transaction and
the query thread to the lock wait state and inserts a waiting request
for a gap x-lock to the lock queue.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
TRANSACTIONAL_TARGET
dberr_t
lock_rec_insert_check_and_lock(
/*===========================*/
	const rec_t*	rec,	/*!< in: record after which to insert */
	buf_block_t*	block,	/*!< in/out: buffer block of rec */
	dict_index_t*	index,	/*!< in: index */
	que_thr_t*	thr,	/*!< in: query thread */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	bool*		inherit)/*!< out: set to true if the new
				inserted record maybe should inherit
				LOCK_GAP type locks from the successor
				record */
{
  ut_ad(block->page.frame == page_align(rec));
  ut_ad(mtr->is_named_space(index->table->space));
  ut_ad(page_is_leaf(block->page.frame));
  ut_ad(!index->table->is_temporary());

  dberr_t err= DB_SUCCESS;
  bool inherit_in= *inherit;
  trx_t *trx= thr_get_trx(thr);
  const rec_t *next_rec= page_rec_get_next_const(rec);
  ulint heap_no= page_rec_get_heap_no(next_rec);
  const page_id_t id{block->page.id()};
  ut_ad(!rec_is_metadata(next_rec, *index));

  {
    LockGuard g{lock_sys.rec_hash, id};
    /* Because this code is invoked for a running transaction by
    the thread that is serving the transaction, it is not necessary
    to hold trx->mutex here. */

    /* When inserting a record into an index, the table must be at
    least IX-locked. When we are building an index, we would pass
    BTR_NO_LOCKING_FLAG and skip the locking altogether. */
    ut_ad(lock_table_has(trx, index->table, LOCK_IX));

    *inherit= lock_sys_t::get_first(g.cell(), id, heap_no);

    if (*inherit)
    {
      /* Spatial index does not use GAP lock protection. It uses
      "predicate lock" to protect the "range" */
      if (index->is_spatial())
        return DB_SUCCESS;

      /* If another transaction has an explicit lock request which locks
      the gap, waiting or granted, on the successor, the insert has to wait.

      An exception is the case where the lock by the another transaction
      is a gap type lock which it placed to wait for its turn to insert. We
      do not consider that kind of a lock conflicting with our insert. This
      eliminates an unnecessary deadlock which resulted when 2 transactions
      had to wait for their insert. Both had waiting gap type lock requests
      on the successor, which produced an unnecessary deadlock. */
      const unsigned type_mode= LOCK_X | LOCK_GAP | LOCK_INSERT_INTENTION;

      if (lock_t *c_lock= lock_rec_other_has_conflicting(type_mode,
                                                         g.cell(), id,
                                                         heap_no, trx))
      {
        trx->mutex_lock();
        err= lock_rec_enqueue_waiting(c_lock, type_mode, id, block->page.frame,
                                      heap_no, index, thr, nullptr);
        trx->mutex_unlock();
      }
    }
  }

  switch (err) {
  case DB_SUCCESS_LOCKED_REC:
    err = DB_SUCCESS;
    /* fall through */
  case DB_SUCCESS:
    if (!inherit_in || index->is_clust())
      break;
    /* Update the page max trx id field */
    page_update_max_trx_id(block, buf_block_get_page_zip(block), trx->id, mtr);
  default:
    /* We only care about the two return values. */
    break;
  }

#ifdef UNIV_DEBUG
  {
    mem_heap_t *heap= nullptr;
    rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
    const rec_offs *offsets;
    rec_offs_init(offsets_);

    offsets= rec_get_offsets(next_rec, index, offsets_, index->n_core_fields,
                             ULINT_UNDEFINED, &heap);

    ut_ad(lock_rec_queue_validate(false, id, next_rec, index, offsets));

    if (UNIV_LIKELY_NULL(heap))
      mem_heap_free(heap);
  }
#endif /* UNIV_DEBUG */

  return err;
}

/*********************************************************************//**
Creates an explicit record lock for a running transaction that currently only
has an implicit lock on the record. The transaction instance must have a
reference count > 0 so that it can't be committed and freed before this
function has completed. */
static
void
lock_rec_convert_impl_to_expl_for_trx(
/*==================================*/
	const page_id_t		id,	/*!< in: page identifier */
	const rec_t*		rec,	/*!< in: user record on page */
	dict_index_t*		index,	/*!< in: index of record */
	trx_t*			trx,	/*!< in/out: active transaction */
	ulint			heap_no)/*!< in: rec heap number to lock */
{
  ut_ad(trx->is_referenced());
  ut_ad(page_rec_is_leaf(rec));
  ut_ad(!rec_is_metadata(rec, *index));

  DEBUG_SYNC_C("before_lock_rec_convert_impl_to_expl_for_trx");
  {
    LockGuard g{lock_sys.rec_hash, id};
    trx->mutex_lock();
    ut_ad(!trx_state_eq(trx, TRX_STATE_NOT_STARTED));

    if (!trx_state_eq(trx, TRX_STATE_COMMITTED_IN_MEMORY) &&
        !lock_rec_has_expl(LOCK_X | LOCK_REC_NOT_GAP, g.cell(), id, heap_no,
                           trx))
      lock_rec_add_to_queue(LOCK_X | LOCK_REC_NOT_GAP, g.cell(), id,
                            page_align(rec), heap_no, index, trx, true);
  }

  trx->mutex_unlock();
  trx->release_reference();

  DEBUG_SYNC_C("after_lock_rec_convert_impl_to_expl_for_trx");
}


#ifdef UNIV_DEBUG
struct lock_rec_other_trx_holds_expl_arg
{
  const ulint heap_no;
  const hash_cell_t &cell;
  const page_id_t id;
  const trx_t &impl_trx;
};


static my_bool lock_rec_other_trx_holds_expl_callback(
  rw_trx_hash_element_t *element,
  lock_rec_other_trx_holds_expl_arg *arg)
{
  element->mutex.wr_lock();
  if (element->trx)
  {
    element->trx->mutex_lock();
    ut_ad(element->trx->state != TRX_STATE_NOT_STARTED);
    lock_t *expl_lock= element->trx->state == TRX_STATE_COMMITTED_IN_MEMORY
      ? nullptr
      : lock_rec_has_expl(LOCK_S | LOCK_REC_NOT_GAP,
                          arg->cell, arg->id, arg->heap_no, element->trx);
    /*
      An explicit lock is held by trx other than the trx holding the implicit
      lock.
    */
    ut_ad(!expl_lock || expl_lock->trx == &arg->impl_trx);
    element->trx->mutex_unlock();
  }
  element->mutex.wr_unlock();
  return 0;
}


/**
  Checks if some transaction, other than given trx_id, has an explicit
  lock on the given rec.

  FIXME: if the current transaction holds implicit lock from INSERT, a
  subsequent locking read should not convert it to explicit. See also
  MDEV-11215.

  @param      caller_trx  trx of current thread
  @param[in]  trx         trx holding implicit lock on rec
  @param[in]  rec         user record
  @param[in]  id          page identifier
*/
static void lock_rec_other_trx_holds_expl(trx_t *caller_trx, trx_t *trx,
                                          const rec_t *rec,
                                          const page_id_t id)
{
  if (trx)
  {
    ut_ad(!page_rec_is_metadata(rec));
    LockGuard g{lock_sys.rec_hash, id};
    ut_ad(trx->is_referenced());
    const trx_state_t state{trx->state};
    ut_ad(state != TRX_STATE_NOT_STARTED);
    if (state == TRX_STATE_COMMITTED_IN_MEMORY)
      /* The transaction was committed before we acquired LockGuard. */
      return;
    lock_rec_other_trx_holds_expl_arg arg=
    { page_rec_get_heap_no(rec), g.cell(), id, *trx };
    trx_sys.rw_trx_hash.iterate(caller_trx,
                                lock_rec_other_trx_holds_expl_callback, &arg);
  }
}
#endif /* UNIV_DEBUG */


/** If an implicit x-lock exists on a record, convert it to an explicit one.

Often, this is called by a transaction that is about to enter a lock wait
due to the lock conflict. Two explicit locks would be created: first the
exclusive lock on behalf of the lock-holder transaction in this function,
and then a wait request on behalf of caller_trx, in the calling function.

This may also be called by the same transaction that is already holding
an implicit exclusive lock on the record. In this case, no explicit lock
should be created.

@param[in,out]	caller_trx	current transaction
@param[in]	id		index tree leaf page identifier
@param[in]	rec		record on the leaf page
@param[in]	index		the index of the record
@param[in]	offsets		rec_get_offsets(rec,index)
@return	whether caller_trx already holds an exclusive lock on rec */
static
bool
lock_rec_convert_impl_to_expl(
	trx_t*			caller_trx,
	page_id_t		id,
	const rec_t*		rec,
	dict_index_t*		index,
	const rec_offs*		offsets)
{
	trx_t*		trx;

	lock_sys.assert_unlocked();
	ut_ad(page_rec_is_user_rec(rec));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!page_rec_is_comp(rec) == !rec_offs_comp(offsets));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(!rec_is_metadata(rec, *index));

	if (dict_index_is_clust(index)) {
		trx_id_t	trx_id;

		trx_id = lock_clust_rec_some_has_impl(rec, index, offsets);

		if (trx_id == 0) {
			return false;
		}
		if (UNIV_UNLIKELY(trx_id == caller_trx->id)) {
			return true;
		}

		trx = trx_sys.find(caller_trx, trx_id);
	} else {
		ut_ad(!dict_index_is_online_ddl(index));

		trx = lock_sec_rec_some_has_impl(caller_trx, rec, index,
						 offsets);
		if (trx == caller_trx) {
			trx->release_reference();
			return true;
		}

		ut_d(lock_rec_other_trx_holds_expl(caller_trx, trx, rec, id));
	}

	if (trx) {
		ulint	heap_no = page_rec_get_heap_no(rec);

		ut_ad(trx->is_referenced());

		/* If the transaction is still active and has no
		explicit x-lock set on the record, set one for it.
		trx cannot be committed until the ref count is zero. */

		lock_rec_convert_impl_to_expl_for_trx(
			id, rec, index, trx, heap_no);
	}

	return false;
}

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate modify (update,
delete mark, or delete unmark) of a clustered index record. If they do,
first tests if the query thread should anyway be suspended for some
reason; if not, then puts the transaction and the query thread to the
lock wait state and inserts a waiting request for a record x-lock to the
lock queue.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_clust_rec_modify_check_and_lock(
/*=================================*/
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: record which should be
					modified */
	dict_index_t*		index,	/*!< in: clustered index */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(rec, index) */
	que_thr_t*		thr)	/*!< in: query thread */
{
	dberr_t	err;
	ulint	heap_no;

	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(dict_index_is_clust(index));
	ut_ad(block->page.frame == page_align(rec));

	ut_ad(!rec_is_metadata(rec, *index));
	ut_ad(!index->table->is_temporary());

	heap_no = rec_offs_comp(offsets)
		? rec_get_heap_no_new(rec)
		: rec_get_heap_no_old(rec);

	/* If a transaction has no explicit x-lock set on the record, set one
	for it */

	if (lock_rec_convert_impl_to_expl(thr_get_trx(thr), block->page.id(),
					  rec, index, offsets)) {
		/* We already hold an implicit exclusive lock. */
		return DB_SUCCESS;
	}

	err = lock_rec_lock(true, LOCK_X | LOCK_REC_NOT_GAP,
			    block, heap_no, index, thr);

	ut_ad(lock_rec_queue_validate(false, block->page.id(),
				      rec, index, offsets));

	if (err == DB_SUCCESS_LOCKED_REC) {
		err = DB_SUCCESS;
	}

	return(err);
}

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate modify (delete
mark or delete unmark) of a secondary index record.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_sec_rec_modify_check_and_lock(
/*===============================*/
	ulint		flags,	/*!< in: if BTR_NO_LOCKING_FLAG
				bit is set, does nothing */
	buf_block_t*	block,	/*!< in/out: buffer block of rec */
	const rec_t*	rec,	/*!< in: record which should be
				modified; NOTE: as this is a secondary
				index, we always have to modify the
				clustered index record first: see the
				comment below */
	dict_index_t*	index,	/*!< in: secondary index */
	que_thr_t*	thr,	/*!< in: query thread
				(can be NULL if BTR_NO_LOCKING_FLAG) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	dberr_t	err;
	ulint	heap_no;

	ut_ad(!dict_index_is_clust(index));
	ut_ad(!dict_index_is_online_ddl(index) || (flags & BTR_CREATE_FLAG));
	ut_ad(block->page.frame == page_align(rec));
	ut_ad(mtr->is_named_space(index->table->space));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(!rec_is_metadata(rec, *index));

	if (flags & BTR_NO_LOCKING_FLAG) {

		return(DB_SUCCESS);
	}
	ut_ad(!index->table->is_temporary());

	heap_no = page_rec_get_heap_no(rec);

#ifdef WITH_WSREP
	trx_t *trx= thr_get_trx(thr);
	/* If transaction scanning an unique secondary key is wsrep
	high priority thread (brute force) this scanning may involve
	GAP-locking in the index. As this locking happens also when
	applying replication events in high priority applier threads,
	there is a probability for lock conflicts between two wsrep
	high priority threads. To avoid this GAP-locking we mark that
	this transaction is using unique key scan here. */
	if (trx->is_wsrep() && wsrep_thd_is_BF(trx->mysql_thd, false))
		trx->wsrep = 3;
#endif /* WITH_WSREP */

	/* Another transaction cannot have an implicit lock on the record,
	because when we come here, we already have modified the clustered
	index record, and this would not have been possible if another active
	transaction had modified this secondary index record. */

	err = lock_rec_lock(true, LOCK_X | LOCK_REC_NOT_GAP,
			    block, heap_no, index, thr);

#ifdef WITH_WSREP
	if (trx->wsrep == 3) trx->wsrep = 1;
#endif /* WITH_WSREP */

#ifdef UNIV_DEBUG
	{
		mem_heap_t*	heap		= NULL;
		rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
		const rec_offs*	offsets;
		rec_offs_init(offsets_);

		offsets = rec_get_offsets(rec, index, offsets_,
					  index->n_core_fields,
					  ULINT_UNDEFINED, &heap);

		ut_ad(lock_rec_queue_validate(
			      false, block->page.id(), rec, index, offsets));

		if (heap != NULL) {
			mem_heap_free(heap);
		}
	}
#endif /* UNIV_DEBUG */

	if (err == DB_SUCCESS || err == DB_SUCCESS_LOCKED_REC) {
		/* Update the page max trx id field */
		/* It might not be necessary to do this if
		err == DB_SUCCESS (no new lock created),
		but it should not cost too much performance. */
		page_update_max_trx_id(block,
				       buf_block_get_page_zip(block),
				       thr_get_trx(thr)->id, mtr);
		err = DB_SUCCESS;
	}

	return(err);
}

/*********************************************************************//**
Like lock_clust_rec_read_check_and_lock(), but reads a
secondary index record.
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_sec_rec_read_check_and_lock(
/*=============================*/
	ulint			flags,	/*!< in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/*!< in: secondary index */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(rec, index) */
	lock_mode		mode,	/*!< in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	unsigned		gap_mode,/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr)	/*!< in: query thread */
{
	dberr_t	err;

	ut_ad(!dict_index_is_clust(index));
	ut_ad(!dict_index_is_online_ddl(index));
	ut_ad(block->page.frame == page_align(rec));
	ut_ad(page_rec_is_user_rec(rec) || page_rec_is_supremum(rec));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(mode == LOCK_X || mode == LOCK_S);

	if ((flags & BTR_NO_LOCKING_FLAG)
	    || srv_read_only_mode
	    || index->table->is_temporary()) {

		return(DB_SUCCESS);
	}

	const page_id_t id{block->page.id()};

	ut_ad(!rec_is_metadata(rec, *index));

	trx_t *trx = thr_get_trx(thr);
	if (!page_rec_is_supremum(rec)
	    && !lock_table_has(trx, index->table, LOCK_X)
	    && lock_rec_convert_impl_to_expl(thr_get_trx(thr), id, rec,
					     index, offsets)
	    && gap_mode == LOCK_REC_NOT_GAP) {
		/* We already hold an implicit exclusive lock. */
		return DB_SUCCESS;
	}

#ifdef WITH_WSREP
	/* If transaction scanning an unique secondary key is wsrep
	high priority thread (brute force) this scanning may involve
	GAP-locking in the index. As this locking happens also when
	applying replication events in high priority applier threads,
	there is a probability for lock conflicts between two wsrep
	high priority threads. To avoid this GAP-locking we mark that
	this transaction is using unique key scan here. */
	if (trx->is_wsrep() && wsrep_thd_is_BF(trx->mysql_thd, false))
		trx->wsrep = 3;
#endif /* WITH_WSREP */

	err = lock_rec_lock(false, gap_mode | mode,
			    block, page_rec_get_heap_no(rec), index, thr);

#ifdef WITH_WSREP
	if (trx->wsrep == 3) trx->wsrep = 1;
#endif /* WITH_WSREP */

	ut_ad(lock_rec_queue_validate(false, id, rec, index, offsets));

	return(err);
}

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate read, or passing
over by a read cursor, of a clustered index record. If they do, first tests
if the query thread should anyway be suspended for some reason; if not, then
puts the transaction and the query thread to the lock wait state and inserts a
waiting request for a record lock to the lock queue. Sets the requested mode
lock on the record.
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_clust_rec_read_check_and_lock(
/*===============================*/
	ulint			flags,	/*!< in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/*!< in: clustered index */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(rec, index) */
	lock_mode		mode,	/*!< in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	unsigned		gap_mode,/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr)	/*!< in: query thread */
{
	ut_ad(dict_index_is_clust(index));
	ut_ad(block->page.frame == page_align(rec));
	ut_ad(page_rec_is_user_rec(rec) || page_rec_is_supremum(rec));
	ut_ad(gap_mode == LOCK_ORDINARY || gap_mode == LOCK_GAP
	      || gap_mode == LOCK_REC_NOT_GAP);
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(!rec_is_metadata(rec, *index));

	if ((flags & BTR_NO_LOCKING_FLAG)
	    || srv_read_only_mode
	    || index->table->is_temporary()) {

		return(DB_SUCCESS);
	}

	const page_id_t id{block->page.id()};

	ulint heap_no = page_rec_get_heap_no(rec);

	trx_t *trx = thr_get_trx(thr);
	if (!lock_table_has(trx, index->table, LOCK_X)
	    && heap_no != PAGE_HEAP_NO_SUPREMUM
	    && lock_rec_convert_impl_to_expl(trx, id, rec, index, offsets)
	    && gap_mode == LOCK_REC_NOT_GAP) {
		/* We already hold an implicit exclusive lock. */
		return DB_SUCCESS;
	}

	dberr_t err = lock_rec_lock(false, gap_mode | mode,
				    block, heap_no, index, thr);

	ut_ad(lock_rec_queue_validate(false, id, rec, index, offsets));

	DEBUG_SYNC_C("after_lock_clust_rec_read_check_and_lock");

	return(err);
}
/*********************************************************************//**
Checks if locks of other transactions prevent an immediate read, or passing
over by a read cursor, of a clustered index record. If they do, first tests
if the query thread should anyway be suspended for some reason; if not, then
puts the transaction and the query thread to the lock wait state and inserts a
waiting request for a record lock to the lock queue. Sets the requested mode
lock on the record. This is an alternative version of
lock_clust_rec_read_check_and_lock() that does not require the parameter
"offsets".
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_clust_rec_read_check_and_lock_alt(
/*===================================*/
	ulint			flags,	/*!< in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/*!< in: clustered index */
	lock_mode		mode,	/*!< in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	unsigned		gap_mode,/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr)	/*!< in: query thread */
{
	mem_heap_t*	tmp_heap	= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;
	dberr_t		err;
	rec_offs_init(offsets_);

	ut_ad(page_rec_is_leaf(rec));
	offsets = rec_get_offsets(rec, index, offsets, index->n_core_fields,
				  ULINT_UNDEFINED, &tmp_heap);
	err = lock_clust_rec_read_check_and_lock(flags, block, rec, index,
						 offsets, mode, gap_mode, thr);
	if (tmp_heap) {
		mem_heap_free(tmp_heap);
	}

	if (err == DB_SUCCESS_LOCKED_REC) {
		err = DB_SUCCESS;
	}

	return(err);
}

/*******************************************************************//**
Check if a transaction holds any autoinc locks.
@return TRUE if the transaction holds any AUTOINC locks. */
static
ibool
lock_trx_holds_autoinc_locks(
/*=========================*/
	const trx_t*	trx)		/*!< in: transaction */
{
	ut_a(trx->autoinc_locks != NULL);

	return(!ib_vector_is_empty(trx->autoinc_locks));
}

/** Release all AUTO_INCREMENT locks of the transaction. */
static void lock_release_autoinc_locks(trx_t *trx)
{
  {
    LockMutexGuard g{SRW_LOCK_CALL};
    mysql_mutex_lock(&lock_sys.wait_mutex);
    trx->mutex_lock();
    auto autoinc_locks= trx->autoinc_locks;
    ut_a(autoinc_locks);

    /* We release the locks in the reverse order. This is to avoid
    searching the vector for the element to delete at the lower level.
    See (lock_table_remove_low()) for details. */
    while (ulint size= ib_vector_size(autoinc_locks))
    {
      lock_t *lock= *static_cast<lock_t**>
        (ib_vector_get(autoinc_locks, size - 1));
      ut_ad(lock->type_mode == (LOCK_AUTO_INC | LOCK_TABLE));
      lock_table_dequeue(lock, true);
      lock_trx_table_locks_remove(lock);
    }
  }
  mysql_mutex_unlock(&lock_sys.wait_mutex);
  trx->mutex_unlock();
}

/** Cancel a waiting lock request and release possibly waiting transactions */
static void lock_cancel_waiting_and_release(lock_t *lock)
{
  lock_sys.assert_locked(*lock);
  mysql_mutex_assert_owner(&lock_sys.wait_mutex);
  trx_t *trx= lock->trx;
  trx->mutex_lock();
  ut_ad(trx->state == TRX_STATE_ACTIVE);

  if (!lock->is_table())
    lock_rec_dequeue_from_page(lock, true);
  else
  {
    if (lock->type_mode == (LOCK_AUTO_INC | LOCK_TABLE))
    {
      ut_ad(trx->autoinc_locks);
      ib_vector_remove(trx->autoinc_locks, lock);
    }
    lock_table_dequeue(lock, true);
    /* Remove the lock from table lock vector too. */
    lock_trx_table_locks_remove(lock);
  }

  /* Reset the wait flag and the back pointer to lock in trx. */
  lock_reset_lock_and_trx_wait(lock);

  lock_wait_end(trx);
  trx->mutex_unlock();
}

void lock_sys_t::cancel_lock_wait_for_trx(trx_t *trx)
{
  lock_sys.wr_lock(SRW_LOCK_CALL);
  mysql_mutex_lock(&lock_sys.wait_mutex);
  if (lock_t *lock= trx->lock.wait_lock)
  {
    /* check if victim is still waiting */
    if (lock->is_waiting())
      lock_cancel_waiting_and_release(lock);
  }
  lock_sys.wr_unlock();
  mysql_mutex_unlock(&lock_sys.wait_mutex);
}

/** Cancel a waiting lock request.
@tparam check_victim  whether to check for DB_DEADLOCK
@param lock           waiting lock request
@param trx            active transaction
@retval DB_SUCCESS    if no lock existed
@retval DB_DEADLOCK   if trx->lock.was_chosen_as_deadlock_victim was set
@retval DB_LOCK_WAIT  if the lock was canceled */
template<bool check_victim>
dberr_t lock_sys_t::cancel(trx_t *trx, lock_t *lock)
{
  mysql_mutex_assert_owner(&lock_sys.wait_mutex);
  ut_ad(trx->lock.wait_lock == lock);
  ut_ad(trx->state == TRX_STATE_ACTIVE);
  dberr_t err= DB_SUCCESS;
  /* This would be too large for a memory transaction, except in the
  DB_DEADLOCK case, which was already tested in lock_trx_handle_wait(). */
  if (lock->is_table())
  {
    if (!lock_sys.rd_lock_try())
    {
      mysql_mutex_unlock(&lock_sys.wait_mutex);
      lock_sys.rd_lock(SRW_LOCK_CALL);
      mysql_mutex_lock(&lock_sys.wait_mutex);
      lock= trx->lock.wait_lock;
      if (!lock);
      else if (check_victim && trx->lock.was_chosen_as_deadlock_victim)
        err= DB_DEADLOCK;
      else
        goto resolve_table_lock;
    }
    else
    {
resolve_table_lock:
      dict_table_t *table= lock->un_member.tab_lock.table;
      if (!table->lock_mutex_trylock())
      {
        /* The correct latching order is:
        lock_sys.latch, table->lock_mutex_lock(), lock_sys.wait_mutex.
        Thus, we must release lock_sys.wait_mutex for a blocking wait. */
        mysql_mutex_unlock(&lock_sys.wait_mutex);
        table->lock_mutex_lock();
        mysql_mutex_lock(&lock_sys.wait_mutex);
        lock= trx->lock.wait_lock;
        if (!lock)
          goto retreat;
        else if (check_victim && trx->lock.was_chosen_as_deadlock_victim)
        {
          err= DB_DEADLOCK;
          goto retreat;
        }
      }
      if (lock->is_waiting())
        lock_cancel_waiting_and_release(lock);
      /* Even if lock->is_waiting() did not hold above, we must return
      DB_LOCK_WAIT, or otherwise optimistic parallel replication could
      occasionally hang. Potentially affected tests:
      rpl.rpl_parallel_optimistic
      rpl.rpl_parallel_optimistic_nobinlog
      rpl.rpl_parallel_optimistic_xa_lsu_off */
      err= DB_LOCK_WAIT;
retreat:
      table->lock_mutex_unlock();
    }
    lock_sys.rd_unlock();
  }
  else
  {
    /* To prevent the record lock from being moved between pages
    during a page split or merge, we must hold exclusive lock_sys.latch. */
    if (!lock_sys.wr_lock_try())
    {
      mysql_mutex_unlock(&lock_sys.wait_mutex);
      lock_sys.wr_lock(SRW_LOCK_CALL);
      mysql_mutex_lock(&lock_sys.wait_mutex);
      lock= trx->lock.wait_lock;
      if (!lock);
      else if (check_victim && trx->lock.was_chosen_as_deadlock_victim)
        err= DB_DEADLOCK;
      else
        goto resolve_record_lock;
    }
    else
    {
resolve_record_lock:
      if (lock->is_waiting())
        lock_cancel_waiting_and_release(lock);
      /* Even if lock->is_waiting() did not hold above, we must return
      DB_LOCK_WAIT, or otherwise optimistic parallel replication could
      occasionally hang. Potentially affected tests:
      rpl.rpl_parallel_optimistic
      rpl.rpl_parallel_optimistic_nobinlog
      rpl.rpl_parallel_optimistic_xa_lsu_off */
      err= DB_LOCK_WAIT;
    }
    lock_sys.wr_unlock();
  }

  return err;
}

/** Cancel a waiting lock request (if any) when killing a transaction */
void lock_sys_t::cancel(trx_t *trx)
{
  mysql_mutex_lock(&lock_sys.wait_mutex);
  if (lock_t *lock= trx->lock.wait_lock)
  {
    /* Dictionary transactions must be immune to KILL, because they
    may be executed as part of a multi-transaction DDL operation, such
    as rollback_inplace_alter_table() or ha_innobase::delete_table(). */
    if (!trx->dict_operation)
    {
      trx->error_state= DB_INTERRUPTED;
      cancel<false>(trx, lock);
    }
  }
  lock_sys.deadlock_check();
  mysql_mutex_unlock(&lock_sys.wait_mutex);
}

/*********************************************************************//**
Unlocks AUTO_INC type locks that were possibly reserved by a trx. This
function should be called at the the end of an SQL statement, by the
connection thread that owns the transaction (trx->mysql_thd). */
void
lock_unlock_table_autoinc(
/*======================*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	lock_sys.assert_unlocked();
	ut_ad(!trx->mutex_is_owner());
	ut_ad(!trx->lock.wait_lock);

	/* This can be invoked on NOT_STARTED, ACTIVE, PREPARED,
	but not COMMITTED transactions. */

	ut_ad(trx_state_eq(trx, TRX_STATE_NOT_STARTED)
	      || !trx_state_eq(trx, TRX_STATE_COMMITTED_IN_MEMORY));

	/* This function is invoked for a running transaction by the
	thread that is serving the transaction. Therefore it is not
	necessary to hold trx->mutex here. */

	if (lock_trx_holds_autoinc_locks(trx)) {
		lock_release_autoinc_locks(trx);
	}
}

/** Handle a pending lock wait (DB_LOCK_WAIT) in a semi-consistent read
while holding a clustered index leaf page latch.
@param trx           transaction that is or was waiting for a lock
@retval DB_SUCCESS   if the lock was granted
@retval DB_DEADLOCK  if the transaction must be aborted due to a deadlock
@retval DB_LOCK_WAIT if a lock wait would be necessary; the pending
                     lock request was released */
dberr_t lock_trx_handle_wait(trx_t *trx)
{
  if (trx->lock.was_chosen_as_deadlock_victim)
    return DB_DEADLOCK;
  if (!trx->lock.wait_lock)
    return DB_SUCCESS;
  dberr_t err= DB_SUCCESS;
  mysql_mutex_lock(&lock_sys.wait_mutex);
  if (trx->lock.was_chosen_as_deadlock_victim)
    err= DB_DEADLOCK;
  else if (lock_t *wait_lock= trx->lock.wait_lock)
    err= lock_sys_t::cancel<true>(trx, wait_lock);
  lock_sys.deadlock_check();
  mysql_mutex_unlock(&lock_sys.wait_mutex);
  return err;
}

#ifdef UNIV_DEBUG
/**
  Do an exhaustive check for any locks (table or rec) against the table.

  @param[in]  table  check if there are any locks held on records in this table
                     or on the table itself
*/

static my_bool lock_table_locks_lookup(rw_trx_hash_element_t *element,
                                       const dict_table_t *table)
{
  lock_sys.assert_locked();
  element->mutex.wr_lock();
  if (element->trx)
  {
    element->trx->mutex_lock();
    check_trx_state(element->trx);
    if (element->trx->state != TRX_STATE_COMMITTED_IN_MEMORY)
    {
      for (const lock_t *lock= UT_LIST_GET_FIRST(element->trx->lock.trx_locks);
           lock != NULL;
           lock= UT_LIST_GET_NEXT(trx_locks, lock))
      {
        ut_ad(lock->trx == element->trx);
        if (!lock->is_table())
        {
          ut_ad(lock->index->online_status != ONLINE_INDEX_CREATION ||
                lock->index->is_primary());
          ut_ad(lock->index->table != table);
        }
        else
          ut_ad(lock->un_member.tab_lock.table != table);
      }
    }
    element->trx->mutex_unlock();
  }
  element->mutex.wr_unlock();
  return 0;
}
#endif /* UNIV_DEBUG */

/** Check if there are any locks on a table.
@return true if table has either table or record locks. */
TRANSACTIONAL_TARGET
bool lock_table_has_locks(dict_table_t *table)
{
  if (table->n_rec_locks)
    return true;
  ulint len;
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
  if (xbegin())
  {
    if (table->lock_mutex_is_locked())
      xabort();
    len= UT_LIST_GET_LEN(table->locks);
    xend();
  }
  else
#endif
  {
    table->lock_mutex_lock();
    len= UT_LIST_GET_LEN(table->locks);
    table->lock_mutex_unlock();
  }
  if (len)
    return true;
#ifdef UNIV_DEBUG
  {
    LockMutexGuard g{SRW_LOCK_CALL};
    trx_sys.rw_trx_hash.iterate(lock_table_locks_lookup,
                                const_cast<const dict_table_t*>(table));
  }
#endif /* UNIV_DEBUG */
  return false;
}

/*******************************************************************//**
Initialise the table lock list. */
void
lock_table_lock_list_init(
/*======================*/
	table_lock_list_t*	lock_list)	/*!< List to initialise */
{
	UT_LIST_INIT(*lock_list, &lock_table_t::locks);
}

#ifdef UNIV_DEBUG
/*******************************************************************//**
Check if the transaction holds any locks on the sys tables
or its records.
@return the strongest lock found on any sys table or 0 for none */
const lock_t*
lock_trx_has_sys_table_locks(
/*=========================*/
	const trx_t*	trx)	/*!< in: transaction to check */
{
	const lock_t*	strongest_lock = 0;
	lock_mode	strongest = LOCK_NONE;

	LockMutexGuard g{SRW_LOCK_CALL};

	const lock_list::const_iterator end = trx->lock.table_locks.end();
	lock_list::const_iterator it = trx->lock.table_locks.begin();

	/* Find a valid mode. Note: ib_vector_size() can be 0. */

	for (/* No op */; it != end; ++it) {
		const lock_t*	lock = *it;

		if (lock != NULL
		    && dict_is_sys_table(lock->un_member.tab_lock.table->id)) {

			strongest = lock->mode();
			ut_ad(strongest != LOCK_NONE);
			strongest_lock = lock;
			break;
		}
	}

	if (strongest == LOCK_NONE) {
		return(NULL);
	}

	for (/* No op */; it != end; ++it) {
		const lock_t*	lock = *it;

		if (lock == NULL) {
			continue;
		}

		ut_ad(trx == lock->trx);
		ut_ad(lock->is_table());
		ut_ad(lock->un_member.tab_lock.table);

		lock_mode mode = lock->mode();

		if (dict_is_sys_table(lock->un_member.tab_lock.table->id)
		    && lock_mode_stronger_or_eq(mode, strongest)) {

			strongest = mode;
			strongest_lock = lock;
		}
	}

	return(strongest_lock);
}

/** Check if the transaction holds an explicit exclusive lock on a record.
@param[in]	trx	transaction
@param[in]	table	table
@param[in]	id	leaf page identifier
@param[in]	heap_no	heap number identifying the record
@return whether an explicit X-lock is held */
bool lock_trx_has_expl_x_lock(const trx_t &trx, const dict_table_t &table,
                              page_id_t id, ulint heap_no)
{
  ut_ad(heap_no > PAGE_HEAP_NO_SUPREMUM);
  ut_ad(lock_table_has(&trx, &table, LOCK_IX));
  if (!lock_table_has(&trx, &table, LOCK_X))
  {
    LockGuard g{lock_sys.rec_hash, id};
    ut_ad(lock_rec_has_expl(LOCK_X | LOCK_REC_NOT_GAP,
                            g.cell(), id, heap_no, &trx));
  }
  return true;
}
#endif /* UNIV_DEBUG */

namespace Deadlock
{
  /** rewind(3) the file used for storing the latest detected deadlock and
  print a heading message to stderr if printing of all deadlocks to stderr
  is enabled. */
  static void start_print()
  {
    lock_sys.assert_locked();

    rewind(lock_latest_err_file);
    ut_print_timestamp(lock_latest_err_file);

    if (srv_print_all_deadlocks)
      ib::info() << "Transactions deadlock detected,"
                    " dumping detailed information.";
  }

  /** Print a message to the deadlock file and possibly to stderr.
  @param msg message to print */
  static void print(const char *msg)
  {
    fputs(msg, lock_latest_err_file);
    if (srv_print_all_deadlocks)
      ib::info() << msg;
  }

  /** Print transaction data to the deadlock file and possibly to stderr.
  @param trx transaction */
  static void print(const trx_t &trx)
  {
    lock_sys.assert_locked();

    ulint n_rec_locks= trx.lock.n_rec_locks;
    ulint n_trx_locks= UT_LIST_GET_LEN(trx.lock.trx_locks);
    ulint heap_size= mem_heap_get_size(trx.lock.lock_heap);

    trx_print_low(lock_latest_err_file, &trx, 3000,
                  n_rec_locks, n_trx_locks, heap_size);

    if (srv_print_all_deadlocks)
      trx_print_low(stderr, &trx, 3000, n_rec_locks, n_trx_locks, heap_size);
  }

  /** Print lock data to the deadlock file and possibly to stderr.
  @param lock record or table type lock */
  static void print(const lock_t &lock)
  {
    lock_sys.assert_locked();

    if (!lock.is_table())
    {
      mtr_t mtr;
      lock_rec_print(lock_latest_err_file, &lock, mtr);

      if (srv_print_all_deadlocks)
        lock_rec_print(stderr, &lock, mtr);
    }
    else
    {
      lock_table_print(lock_latest_err_file, &lock);

      if (srv_print_all_deadlocks)
        lock_table_print(stderr, &lock);
    }
  }

  ATTRIBUTE_COLD
  /** Report a deadlock (cycle in the waits-for graph).
  @param trx        transaction waiting for a lock in this thread
  @param current_trx whether trx belongs to the current thread
  @return the transaction to be rolled back (unless one was committed already)
  @return nullptr if no deadlock */
  static trx_t *report(trx_t *const trx, bool current_trx)
  {
    mysql_mutex_assert_owner(&lock_sys.wait_mutex);
    ut_ad(xtest() || lock_sys.is_writer() == !current_trx);

    /* Normally, trx should be a direct part of the deadlock
    cycle. However, if innodb_deadlock_detect had been OFF in the
    past, or if current_trx=false, trx may be waiting for a lock that
    is held by a participant of a pre-existing deadlock, without being
    part of the deadlock itself. That is, the path to the deadlock may be
    P-shaped instead of O-shaped, with trx being at the foot of the P.

    We will process the entire path leading to a cycle, and we will
    choose the victim (to be aborted) among the cycle. */

    static const char rollback_msg[]= "*** WE ROLL BACK TRANSACTION (%u)\n";
    char buf[9 + sizeof rollback_msg];

    /* If current_trx=true, trx is owned by this thread, and we can
    safely invoke these without holding trx->mutex or lock_sys.latch.
    If current_trx=false, a concurrent commit is protected by both
    lock_sys.latch and lock_sys.wait_mutex. */
    const undo_no_t trx_weight= TRX_WEIGHT(trx) |
      (trx->mysql_thd &&
#ifdef WITH_WSREP
       (thd_has_edited_nontrans_tables(trx->mysql_thd) ||
        (trx->is_wsrep() && wsrep_thd_is_BF(trx->mysql_thd, false)))
#else
       thd_has_edited_nontrans_tables(trx->mysql_thd)
#endif /* WITH_WSREP */
       ? 1ULL << 63 : 0);

    trx_t *victim= nullptr;
    undo_no_t victim_weight= ~0ULL;
    unsigned victim_pos= 0, trx_pos= 0;

    /* Here, lock elision does not make sense, because
    for the output we are going to invoke system calls,
    which would interrupt a memory transaction. */
    if (current_trx && !lock_sys.wr_lock_try())
    {
      mysql_mutex_unlock(&lock_sys.wait_mutex);
      lock_sys.wr_lock(SRW_LOCK_CALL);
      mysql_mutex_lock(&lock_sys.wait_mutex);
    }

    {
      unsigned l= 0;
      /* Now that we are holding lock_sys.wait_mutex again, check
      whether a cycle still exists. */
      trx_t *cycle= find_cycle(trx);
      if (!cycle)
        goto func_exit; /* One of the transactions was already aborted. */
      for (trx_t *next= cycle;;)
      {
        next= next->lock.wait_trx;
        const undo_no_t next_weight= TRX_WEIGHT(next) |
          (next->mysql_thd &&
#ifdef WITH_WSREP
           (thd_has_edited_nontrans_tables(next->mysql_thd) ||
            (next->is_wsrep() && wsrep_thd_is_BF(next->mysql_thd, false)))
#else
           thd_has_edited_nontrans_tables(next->mysql_thd)
#endif /* WITH_WSREP */
           ? 1ULL << 63 : 0);
        if (next_weight < victim_weight)
        {
          victim_weight= next_weight;
          victim= next;
          victim_pos= l;
        }
        if (next == victim)
          trx_pos= l;
        if (next == cycle)
          break;
      }

      if (trx_pos && trx_weight == victim_weight)
      {
        victim= trx;
        victim_pos= trx_pos;
      }

      /* Finally, display the deadlock */
      switch (const auto r= static_cast<enum report>(innodb_deadlock_report)) {
      case REPORT_OFF:
        break;
      case REPORT_BASIC:
      case REPORT_FULL:
        start_print();
        l= 0;

        for (trx_t *next= cycle;;)
        {
          next= next->lock.wait_trx;
          ut_ad(next);
          ut_ad(next->state == TRX_STATE_ACTIVE);
          const lock_t *wait_lock= next->lock.wait_lock;
          ut_ad(wait_lock);
          snprintf(buf, sizeof buf, "\n*** (%u) TRANSACTION:\n", ++l);
          print(buf);
          print(*next);
          print("*** WAITING FOR THIS LOCK TO BE GRANTED:\n");
          print(*wait_lock);
          if (r == REPORT_BASIC);
          else if (wait_lock->is_table())
          {
            if (const lock_t *lock=
                UT_LIST_GET_FIRST(wait_lock->un_member.tab_lock.table->locks))
            {
              ut_ad(!lock->is_waiting());
              print("*** CONFLICTING WITH:\n");
              do
                print(*lock);
              while ((lock= UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock)) &&
                     !lock->is_waiting());
            }
            else
              ut_ad("no conflicting table lock found" == 0);
          }
          else
          {
            const page_id_t id{wait_lock->un_member.rec_lock.page_id};
            hash_cell_t &cell= *(wait_lock->type_mode & LOCK_PREDICATE
                                 ? lock_sys.prdt_hash : lock_sys.rec_hash).
              cell_get(id.fold());
            if (const lock_t *lock= lock_sys_t::get_first(cell, id))
            {
              const ulint heap_no= lock_rec_find_set_bit(wait_lock);
              if (!lock_rec_get_nth_bit(lock, heap_no))
                lock= lock_rec_get_next_const(heap_no, lock);
              ut_ad(!lock->is_waiting());
              print("*** CONFLICTING WITH:\n");
              do
                print(*lock);
              while ((lock= lock_rec_get_next_const(heap_no, lock)) &&
                     !lock->is_waiting());
            }
            else
              ut_ad("no conflicting record lock found" == 0);
          }
          if (next == cycle)
            break;
        }
        snprintf(buf, sizeof buf, rollback_msg, victim_pos);
        print(buf);
      }

      ut_ad(victim->state == TRX_STATE_ACTIVE);

      victim->lock.was_chosen_as_deadlock_victim= true;
      lock_cancel_waiting_and_release(victim->lock.wait_lock);
#ifdef WITH_WSREP
      if (victim->is_wsrep() && wsrep_thd_is_SR(victim->mysql_thd))
        wsrep_handle_SR_rollback(trx->mysql_thd, victim->mysql_thd);
#endif
    }

func_exit:
    if (current_trx)
      lock_sys.wr_unlock();
    return victim;
  }
}

/** Check if a lock request results in a deadlock.
Resolve a deadlock by choosing a transaction that will be rolled back.
@param trx    transaction requesting a lock
@return whether trx must report DB_DEADLOCK */
static bool Deadlock::check_and_resolve(trx_t *trx)
{
  mysql_mutex_assert_owner(&lock_sys.wait_mutex);

  ut_ad(!trx->mutex_is_owner());
  ut_ad(trx->state == TRX_STATE_ACTIVE);
  ut_ad(!srv_read_only_mode);

  if (!innodb_deadlock_detect)
    return false;

  if (UNIV_LIKELY_NULL(find_cycle(trx)) && report(trx, true) == trx)
    return true;

  if (UNIV_LIKELY(!trx->lock.was_chosen_as_deadlock_victim))
    return false;

  if (lock_t *wait_lock= trx->lock.wait_lock)
    lock_sys_t::cancel<false>(trx, wait_lock);

  lock_sys.deadlock_check();
  return true;
}

/** Check for deadlocks while holding only lock_sys.wait_mutex. */
TRANSACTIONAL_TARGET
void lock_sys_t::deadlock_check()
{
  ut_ad(!is_writer());
  mysql_mutex_assert_owner(&wait_mutex);
  bool acquired= false;
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
  bool elided= false;
#endif

  if (Deadlock::to_be_checked)
  {
    for (;;)
    {
      auto i= Deadlock::to_check.begin();
      if (i == Deadlock::to_check.end())
        break;
      if (acquired);
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
      else if (xbegin())
      {
        if (latch.is_locked_or_waiting())
          xabort();
        acquired= elided= true;
      }
#endif
      else
      {
        acquired= wr_lock_try();
        if (!acquired)
        {
          acquired= true;
          mysql_mutex_unlock(&wait_mutex);
          lock_sys.wr_lock(SRW_LOCK_CALL);
          mysql_mutex_lock(&wait_mutex);
          continue;
        }
      }
      trx_t *trx= *i;
      Deadlock::to_check.erase(i);
      if (Deadlock::find_cycle(trx))
        Deadlock::report(trx, false);
    }
    Deadlock::to_be_checked= false;
  }
  ut_ad(Deadlock::to_check.empty());
#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
  if (elided)
    return;
#endif
  if (acquired)
    wr_unlock();
}

/** Update the locks when a page is split and merged to two pages,
in defragmentation. */
void lock_update_split_and_merge(
	const buf_block_t* left_block,	/*!< in: left page to which merged */
	const rec_t* orig_pred,		/*!< in: original predecessor of
					supremum on the left page before merge*/
	const buf_block_t* right_block)	/*!< in: right page from which merged */
{
  ut_ad(page_is_leaf(left_block->page.frame));
  ut_ad(page_is_leaf(right_block->page.frame));
  ut_ad(page_align(orig_pred) == left_block->page.frame);

  const page_id_t l{left_block->page.id()};
  const page_id_t r{right_block->page.id()};

  /* This would likely be too large for a memory transaction. */
  LockMultiGuard g{lock_sys.rec_hash, l, r};
  const rec_t *left_next_rec= page_rec_get_next_const(orig_pred);
  ut_ad(!page_rec_is_metadata(left_next_rec));

  /* Inherit the locks on the supremum of the left page to the
  first record which was moved from the right page */
  lock_rec_inherit_to_gap(g.cell1(), l, g.cell1(), l, left_block->page.frame,
                          page_rec_get_heap_no(left_next_rec),
                          PAGE_HEAP_NO_SUPREMUM);

  /* Reset the locks on the supremum of the left page,
  releasing waiting transactions */
  lock_rec_reset_and_release_wait(g.cell1(), l, PAGE_HEAP_NO_SUPREMUM);

  /* Inherit the locks to the supremum of the left page from the
  successor of the infimum on the right page */
  lock_rec_inherit_to_gap(g.cell1(), l, g.cell2(), r, left_block->page.frame,
                          PAGE_HEAP_NO_SUPREMUM,
                          lock_get_min_heap_no(right_block));
}
