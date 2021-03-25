/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2021, MariaDB Corporation.

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
@file trx/trx0trx.cc
The transaction

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0trx.h"

#ifdef WITH_WSREP
#include <mysql/service_wsrep.h>
#endif

#include <mysql/service_thd_error_context.h>

#include "btr0sea.h"
#include "lock0lock.h"
#include "log0log.h"
#include "os0proc.h"
#include "que0que.h"
#include "srv0mon.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0purge.h"
#include "trx0rec.h"
#include "trx0roll.h"
#include "trx0rseg.h"
#include "trx0undo.h"
#include "trx0xa.h"
#include "ut0pool.h"
#include "ut0vec.h"

#include <set>
#include <new>

/** The bit pattern corresponding to TRX_ID_MAX */
const byte trx_id_max_bytes[8] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

/** The bit pattern corresponding to max timestamp */
const byte timestamp_max_bytes[7] = {
	0x7f, 0xff, 0xff, 0xff, 0x0f, 0x42, 0x3f
};


static const ulint MAX_DETAILED_ERROR_LEN = 256;

/** Set of table_id */
typedef std::set<
	table_id_t,
	std::less<table_id_t>,
	ut_allocator<table_id_t> >	table_id_set;

/*************************************************************//**
Set detailed error message for the transaction. */
void
trx_set_detailed_error(
/*===================*/
	trx_t*		trx,	/*!< in: transaction struct */
	const char*	msg)	/*!< in: detailed error message */
{
	strncpy(trx->detailed_error, msg, MAX_DETAILED_ERROR_LEN - 1);
	trx->detailed_error[MAX_DETAILED_ERROR_LEN - 1] = '\0';
}

/*************************************************************//**
Set detailed error message for the transaction from a file. Note that the
file is rewinded before reading from it. */
void
trx_set_detailed_error_from_file(
/*=============================*/
	trx_t*	trx,	/*!< in: transaction struct */
	FILE*	file)	/*!< in: file to read message from */
{
	os_file_read_string(file, trx->detailed_error, MAX_DETAILED_ERROR_LEN);
}

/********************************************************************//**
Initialize transaction object.
@param trx trx to initialize */
static
void
trx_init(
/*=====*/
	trx_t*	trx)
{
	trx->no = TRX_ID_MAX;

	trx->state = TRX_STATE_NOT_STARTED;

	trx->is_recovered = false;

	trx->op_info = "";

	trx->active_commit_ordered = false;

	trx->isolation_level = TRX_ISO_REPEATABLE_READ;

	trx->check_foreigns = true;

	trx->check_unique_secondary = true;

	trx->lock.n_rec_locks = 0;

	trx->dict_operation = TRX_DICT_OP_NONE;

	trx->table_id = 0;

	trx->error_state = DB_SUCCESS;

	trx->error_key_num = ULINT_UNDEFINED;

	trx->undo_no = 0;

	trx->rsegs.m_redo.rseg = NULL;

	trx->rsegs.m_noredo.rseg = NULL;

	trx->read_only = false;

	trx->auto_commit = false;

	trx->will_lock = 0;

	trx->ddl = false;

	trx->internal = false;

	ut_d(trx->start_file = 0);

	ut_d(trx->start_line = 0);

	trx->magic_n = TRX_MAGIC_N;

	trx->lock.que_state = TRX_QUE_RUNNING;

	trx->last_sql_stat_start.least_undo_no = 0;

	ut_ad(!trx->read_view.is_open());

	trx->lock.rec_cached = 0;

	trx->lock.table_cached = 0;
#ifdef WITH_WSREP
	ut_ad(!trx->wsrep);
	ut_ad(!trx->wsrep_event);
	ut_ad(!trx->wsrep_UK_scan);
#endif /* WITH_WSREP */

	ut_ad(trx->get_flush_observer() == NULL);
}

/** For managing the life-cycle of the trx_t instance that we get
from the pool. */
struct TrxFactory {

	/** Initializes a transaction object. It must be explicitly started
	with trx_start_if_not_started() before using it. The default isolation
	level is TRX_ISO_REPEATABLE_READ.
	@param trx Transaction instance to initialise */
	static void init(trx_t* trx)
	{
		/* Explicitly call the constructor of the already
		allocated object. trx_t objects are allocated by
		ut_zalloc_nokey() in Pool::Pool() which would not call
		the constructors of the trx_t members. */
		new(&trx->mod_tables) trx_mod_tables_t();

		new(&trx->lock.table_locks) lock_list();

		new(&trx->read_view) ReadView();

		trx->rw_trx_hash_pins = 0;
		trx_init(trx);

		trx->dict_operation_lock_mode = 0;

		trx->xid = UT_NEW_NOKEY(xid_t());

		trx->detailed_error = reinterpret_cast<char*>(
			ut_zalloc_nokey(MAX_DETAILED_ERROR_LEN));

		trx->lock.lock_heap = mem_heap_create_typed(
			1024, MEM_HEAP_FOR_LOCK_HEAP);

		lock_trx_lock_list_init(&trx->lock.trx_locks);

		UT_LIST_INIT(trx->lock.evicted_tables,
			     &dict_table_t::table_LRU);

		UT_LIST_INIT(
			trx->trx_savepoints,
			&trx_named_savept_t::trx_savepoints);

		mutex_create(LATCH_ID_TRX, &trx->mutex);
	}

	/** Release resources held by the transaction object.
	@param trx the transaction for which to release resources */
	static void destroy(trx_t* trx)
	{
#ifdef __SANITIZE_ADDRESS__
		/* Unpoison the memory for AddressSanitizer */
		MEM_MAKE_ADDRESSABLE(trx, sizeof *trx);
#else
		/* Declare the contents as initialized for Valgrind;
		we checked this in trx_t::free(). */
		MEM_MAKE_DEFINED(trx, sizeof *trx);
#endif

		ut_a(trx->magic_n == TRX_MAGIC_N);
		ut_ad(!trx->mysql_thd);

		ut_a(trx->lock.wait_lock == NULL);
		ut_a(trx->lock.wait_thr == NULL);
		ut_a(trx->dict_operation_lock_mode == 0);

		if (trx->lock.lock_heap != NULL) {
			mem_heap_free(trx->lock.lock_heap);
			trx->lock.lock_heap = NULL;
		}

		ut_a(UT_LIST_GET_LEN(trx->lock.trx_locks) == 0);
		ut_ad(UT_LIST_GET_LEN(trx->lock.evicted_tables) == 0);

		UT_DELETE(trx->xid);
		ut_free(trx->detailed_error);

		mutex_free(&trx->mutex);

		trx->mod_tables.~trx_mod_tables_t();

		ut_ad(!trx->read_view.is_open());

		trx->lock.table_locks.~lock_list();

		trx->read_view.~ReadView();
	}
};

/** The lock strategy for TrxPool */
struct TrxPoolLock {
	TrxPoolLock() { }

	/** Create the mutex */
	void create()
	{
		mutex_create(LATCH_ID_TRX_POOL, &m_mutex);
	}

	/** Acquire the mutex */
	void enter() { mutex_enter(&m_mutex); }

	/** Release the mutex */
	void exit() { mutex_exit(&m_mutex); }

	/** Free the mutex */
	void destroy() { mutex_free(&m_mutex); }

	/** Mutex to use */
	ib_mutex_t	m_mutex;
};

/** The lock strategy for the TrxPoolManager */
struct TrxPoolManagerLock {
	TrxPoolManagerLock() { }

	/** Create the mutex */
	void create()
	{
		mutex_create(LATCH_ID_TRX_POOL_MANAGER, &m_mutex);
	}

	/** Acquire the mutex */
	void enter() { mutex_enter(&m_mutex); }

	/** Release the mutex */
	void exit() { mutex_exit(&m_mutex); }

	/** Free the mutex */
	void destroy() { mutex_free(&m_mutex); }

	/** Mutex to use */
	ib_mutex_t	m_mutex;
};

/** Use explicit mutexes for the trx_t pool and its manager. */
typedef Pool<trx_t, TrxFactory, TrxPoolLock> trx_pool_t;
typedef PoolManager<trx_pool_t, TrxPoolManagerLock > trx_pools_t;

/** The trx_t pool manager */
static trx_pools_t* trx_pools;

/** Size of on trx_t pool in bytes. */
static const ulint MAX_TRX_BLOCK_SIZE = 1024 * 1024 * 4;

/** Create the trx_t pool */
void
trx_pool_init()
{
	trx_pools = UT_NEW_NOKEY(trx_pools_t(MAX_TRX_BLOCK_SIZE));

	ut_a(trx_pools != 0);
}

/** Destroy the trx_t pool */
void
trx_pool_close()
{
	UT_DELETE(trx_pools);

	trx_pools = 0;
}

/** @return an allocated transaction */
trx_t *trx_create()
{
	trx_t*	trx = trx_pools->get();

#ifdef __SANITIZE_ADDRESS__
	/* Unpoison the memory for AddressSanitizer.
	It may have been poisoned in trx_t::free().*/
	MEM_MAKE_ADDRESSABLE(trx, sizeof *trx);
#else
	/* Declare the memory initialized for Valgrind.
	The trx_t that are released to the pool are
	actually initialized; we checked that by
	MEM_CHECK_DEFINED() in trx_t::free(). */
	MEM_MAKE_DEFINED(trx, sizeof *trx);
#endif

	trx->assert_freed();

	mem_heap_t*	heap;
	ib_alloc_t*	alloc;

	/* We just got trx from pool, it should be non locking */
	ut_ad(trx->will_lock == 0);
	ut_ad(!trx->rw_trx_hash_pins);

	DBUG_LOG("trx", "Create: " << trx);

	heap = mem_heap_create(sizeof(ib_vector_t) + sizeof(void*) * 8);

	alloc = ib_heap_allocator_create(heap);

	trx->autoinc_locks = ib_vector_create(alloc, sizeof(void**), 4);

	ut_ad(trx->mod_tables.empty());
	ut_ad(trx->lock.n_rec_locks == 0);
	ut_ad(trx->lock.table_cached == 0);
	ut_ad(trx->lock.rec_cached == 0);
	ut_ad(UT_LIST_GET_LEN(trx->lock.evicted_tables) == 0);

#ifdef WITH_WSREP
	trx->wsrep_event= NULL;
	ut_ad(!trx->wsrep_UK_scan);
#endif /* WITH_WSREP */

	trx_sys.register_trx(trx);

	return(trx);
}

/** Free the memory to trx_pools */
void trx_t::free()
{
  MEM_CHECK_DEFINED(this, sizeof *this);

  ut_ad(!n_mysql_tables_in_use);
  ut_ad(!mysql_n_tables_locked);
  ut_ad(!internal);
  ut_ad(!declared_to_be_inside_innodb);
  ut_ad(!will_lock);
  ut_ad(error_state == DB_SUCCESS);
  ut_ad(magic_n == TRX_MAGIC_N);
  ut_ad(!read_only);
  ut_ad(!lock.wait_lock);

  dict_operation= TRX_DICT_OP_NONE;
  trx_sys.deregister_trx(this);
  assert_freed();
  trx_sys.rw_trx_hash.put_pins(this);

  mysql_thd= NULL;
  mysql_log_file_name= NULL;

  // FIXME: We need to avoid this heap free/alloc for each commit.
  if (autoinc_locks)
  {
    ut_ad(ib_vector_is_empty(autoinc_locks));
    /* We allocated a dedicated heap for the vector. */
    ib_vector_free(autoinc_locks);
    autoinc_locks= NULL;
  }

  mod_tables.clear();

  MEM_NOACCESS(&n_ref, sizeof n_ref);
  /* do not poison mutex */
  MEM_NOACCESS(&id, sizeof id);
  MEM_NOACCESS(&no, sizeof no);
  MEM_NOACCESS(&state, sizeof state);
  MEM_NOACCESS(&is_recovered, sizeof is_recovered);
#ifdef WITH_WSREP
  MEM_NOACCESS(&wsrep, sizeof wsrep);
#endif
  MEM_NOACCESS(&read_view, sizeof read_view);
  MEM_NOACCESS(&trx_list, sizeof trx_list);
  MEM_NOACCESS(&lock, sizeof lock);
  MEM_NOACCESS(&op_info, sizeof op_info);
  MEM_NOACCESS(&isolation_level, sizeof isolation_level);
  MEM_NOACCESS(&check_foreigns, sizeof check_foreigns);
  MEM_NOACCESS(&is_registered, sizeof is_registered);
  MEM_NOACCESS(&active_commit_ordered, sizeof active_commit_ordered);
  MEM_NOACCESS(&check_unique_secondary, sizeof check_unique_secondary);
  MEM_NOACCESS(&flush_log_later, sizeof flush_log_later);
  MEM_NOACCESS(&must_flush_log_later, sizeof must_flush_log_later);
  MEM_NOACCESS(&duplicates, sizeof duplicates);
  MEM_NOACCESS(&dict_operation, sizeof dict_operation);
  MEM_NOACCESS(&declared_to_be_inside_innodb, sizeof declared_to_be_inside_innodb);
  MEM_NOACCESS(&n_tickets_to_enter_innodb, sizeof n_tickets_to_enter_innodb);
  MEM_NOACCESS(&dict_operation_lock_mode, sizeof dict_operation_lock_mode);
  MEM_NOACCESS(&start_time, sizeof start_time);
  MEM_NOACCESS(&start_time_micro, sizeof start_time_micro);
  MEM_NOACCESS(&commit_lsn, sizeof commit_lsn);
  MEM_NOACCESS(&table_id, sizeof table_id);
  MEM_NOACCESS(&mysql_thd, sizeof mysql_thd);
  MEM_NOACCESS(&mysql_log_file_name, sizeof mysql_log_file_name);
  MEM_NOACCESS(&mysql_log_offset, sizeof mysql_log_offset);
  MEM_NOACCESS(&n_mysql_tables_in_use, sizeof n_mysql_tables_in_use);
  MEM_NOACCESS(&mysql_n_tables_locked, sizeof mysql_n_tables_locked);
  MEM_NOACCESS(&error_state, sizeof error_state);
  MEM_NOACCESS(&error_info, sizeof error_info);
  MEM_NOACCESS(&error_key_num, sizeof error_key_num);
  MEM_NOACCESS(&graph, sizeof graph);
  MEM_NOACCESS(&trx_savepoints, sizeof trx_savepoints);
  MEM_NOACCESS(&undo_no, sizeof undo_no);
  MEM_NOACCESS(&last_sql_stat_start, sizeof last_sql_stat_start);
  MEM_NOACCESS(&rsegs, sizeof rsegs);
  MEM_NOACCESS(&roll_limit, sizeof roll_limit);
  MEM_NOACCESS(&in_rollback, sizeof in_rollback);
  MEM_NOACCESS(&pages_undone, sizeof pages_undone);
  MEM_NOACCESS(&n_autoinc_rows, sizeof n_autoinc_rows);
  MEM_NOACCESS(&autoinc_locks, sizeof autoinc_locks);
  MEM_NOACCESS(&read_only, sizeof read_only);
  MEM_NOACCESS(&auto_commit, sizeof auto_commit);
  MEM_NOACCESS(&will_lock, sizeof will_lock);
  MEM_NOACCESS(&fts_trx, sizeof fts_trx);
  MEM_NOACCESS(&fts_next_doc_id, sizeof fts_next_doc_id);
  MEM_NOACCESS(&flush_tables, sizeof flush_tables);
  MEM_NOACCESS(&ddl, sizeof ddl);
  MEM_NOACCESS(&internal, sizeof internal);
#ifdef UNIV_DEBUG
  MEM_NOACCESS(&start_line, sizeof start_line);
  MEM_NOACCESS(&start_file, sizeof start_file);
#endif /* UNIV_DEBUG */
  MEM_NOACCESS(&xid, sizeof xid);
  MEM_NOACCESS(&mod_tables, sizeof mod_tables);
  MEM_NOACCESS(&detailed_error, sizeof detailed_error);
  MEM_NOACCESS(&flush_observer, sizeof flush_observer);
#ifdef WITH_WSREP
  MEM_NOACCESS(&wsrep_event, sizeof wsrep_event);
  ut_ad(!wsrep_UK_scan);
  MEM_NOACCESS(&wsrep_UK_scan, sizeof wsrep_UK_scan);
#endif /* WITH_WSREP */
  MEM_NOACCESS(&magic_n, sizeof magic_n);
  trx_pools->mem_free(this);
}

/** Transition to committed state, to release implicit locks. */
inline void trx_t::commit_state()
{
  ut_ad(state == TRX_STATE_PREPARED
	|| state == TRX_STATE_PREPARED_RECOVERED
	|| state == TRX_STATE_ACTIVE);
  /* This makes the transaction committed in memory and makes its
  changes to data visible to other transactions. NOTE that there is a
  small discrepancy from the strict formal visibility rules here: a
  user of the database can see modifications made by another
  transaction T even before the necessary redo log segment has been
  flushed to the disk. If the database happens to crash before the
  flush, the user has seen modifications from T which will never be a
  committed transaction. However, any transaction T2 which sees the
  modifications of the committing transaction T, and which also itself
  makes modifications to the database, will get an lsn larger than the
  committing transaction T. In the case where the log flush fails, and
  T never gets committed, also T2 will never get committed. */
  trx_mutex_enter(this);
  state= TRX_STATE_COMMITTED_IN_MEMORY;
  trx_mutex_exit(this);
  ut_ad(id || !is_referenced());
}

/** Release any explicit locks of a committing transaction. */
inline void trx_t::release_locks()
{
  DBUG_ASSERT(state == TRX_STATE_COMMITTED_IN_MEMORY);
  DBUG_ASSERT(!is_referenced());

  if (UT_LIST_GET_LEN(lock.trx_locks))
  {
    lock_release(this);
    lock.n_rec_locks = 0;
    ut_ad(UT_LIST_GET_LEN(lock.trx_locks) == 0);
    ut_ad(ib_vector_is_empty(autoinc_locks));
    mem_heap_empty(lock.lock_heap);
  }

  lock.table_locks.clear();
}

/** At shutdown, frees a transaction object. */
void
trx_free_at_shutdown(trx_t *trx)
{
	ut_ad(trx->is_recovered);
	ut_a(trx_state_eq(trx, TRX_STATE_PREPARED)
	     || trx_state_eq(trx, TRX_STATE_PREPARED_RECOVERED)
	     || (trx_state_eq(trx, TRX_STATE_ACTIVE)
		 && (!srv_was_started || is_mariabackup_restore_or_export()
		     || srv_read_only_mode
		     || srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO
		     || (!srv_is_being_started
		         && !srv_undo_sources && srv_fast_shutdown))));
	ut_a(trx->magic_n == TRX_MAGIC_N);

	trx->commit_state();
	trx->release_locks();
	trx_undo_free_at_shutdown(trx);

	ut_a(!trx->read_only);

	DBUG_LOG("trx", "Free prepared: " << trx);
	trx->state = TRX_STATE_NOT_STARTED;
	ut_ad(!UT_LIST_GET_LEN(trx->lock.trx_locks));
	trx->id = 0;
	trx->free();
}


/**
  Disconnect a prepared transaction from MySQL
  @param[in,out] trx transaction
*/
void trx_disconnect_prepared(trx_t *trx)
{
  ut_ad(trx_state_eq(trx, TRX_STATE_PREPARED));
  ut_ad(trx->mysql_thd);
  trx->read_view.close();
  trx->is_recovered= true;
  trx->mysql_thd= NULL;
  /* todo/fixme: suggest to do it at innodb prepare */
  trx->will_lock= 0;
}

/****************************************************************//**
Resurrect the table locks for a resurrected transaction. */
static
void
trx_resurrect_table_locks(
/*======================*/
	trx_t*			trx,	/*!< in/out: transaction */
	const trx_undo_t*	undo)	/*!< in: undo log */
{
	mtr_t			mtr;
	page_t*			undo_page;
	trx_undo_rec_t*		undo_rec;
	table_id_set		tables;

	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE) ||
	      trx_state_eq(trx, TRX_STATE_PREPARED));
	ut_ad(undo->rseg == trx->rsegs.m_redo.rseg);

	if (undo->empty()) {
		return;
	}

	mtr_start(&mtr);

	/* trx_rseg_mem_create() may have acquired an X-latch on this
	page, so we cannot acquire an S-latch. */
	undo_page = trx_undo_page_get(
		page_id_t(trx->rsegs.m_redo.rseg->space->id,
			  undo->top_page_no), &mtr);

	undo_rec = undo_page + undo->top_offset;

	do {
		ulint		type;
		undo_no_t	undo_no;
		table_id_t	table_id;
		ulint		cmpl_info;
		bool		updated_extern;

		page_t*		undo_rec_page = page_align(undo_rec);

		if (undo_rec_page != undo_page) {
			mtr.release_page(undo_page, MTR_MEMO_PAGE_X_FIX);
			undo_page = undo_rec_page;
		}

		trx_undo_rec_get_pars(
			undo_rec, &type, &cmpl_info,
			&updated_extern, &undo_no, &table_id);
		tables.insert(table_id);

		undo_rec = trx_undo_get_prev_rec(
			undo_rec, undo->hdr_page_no,
			undo->hdr_offset, false, &mtr);
	} while (undo_rec);

	mtr_commit(&mtr);

	for (table_id_set::const_iterator i = tables.begin();
	     i != tables.end(); i++) {
		if (dict_table_t* table = dict_table_open_on_id(
			    *i, FALSE, DICT_TABLE_OP_LOAD_TABLESPACE)) {
			if (!table->is_readable()) {
				mutex_enter(&dict_sys.mutex);
				dict_table_close(table, TRUE, FALSE);
				dict_sys.remove(table);
				mutex_exit(&dict_sys.mutex);
				continue;
			}

			if (trx->state == TRX_STATE_PREPARED) {
				trx->mod_tables.insert(
					trx_mod_tables_t::value_type(table,
								     0));
			}
			lock_table_ix_resurrect(table, trx);

			DBUG_LOG("ib_trx",
				 "resurrect " << ib::hex(trx->id)
				 << " IX lock on " << table->name);

			dict_table_close(table, FALSE, FALSE);
		}
	}
}


/**
  Resurrect the transactions that were doing inserts/updates the time of the
  crash, they need to be undone.
*/

static void trx_resurrect(trx_undo_t *undo, trx_rseg_t *rseg,
                          time_t start_time, ulonglong start_time_micro,
                          uint64_t *rows_to_undo,
                          bool is_old_insert)
{
  trx_state_t state;
  /*
    This is single-threaded startup code, we do not need the
    protection of trx->mutex or trx_sys.mutex here.
  */
  switch (undo->state)
  {
  case TRX_UNDO_ACTIVE:
    state= TRX_STATE_ACTIVE;
    break;
  case TRX_UNDO_PREPARED:
    /*
      Prepared transactions are left in the prepared state
      waiting for a commit or abort decision from MySQL
    */
    ib::info() << "Transaction " << undo->trx_id
               << " was in the XA prepared state.";

    state= TRX_STATE_PREPARED;
    break;
  default:
    if (is_old_insert && srv_force_recovery < SRV_FORCE_NO_TRX_UNDO)
      trx_undo_commit_cleanup(undo, false);
    return;
  }

  trx_t *trx= trx_create();
  trx->state= state;
  ut_d(trx->start_file= __FILE__);
  ut_d(trx->start_line= __LINE__);
  ut_ad(trx->no == TRX_ID_MAX);

  if (is_old_insert)
    trx->rsegs.m_redo.old_insert= undo;
  else
    trx->rsegs.m_redo.undo= undo;

  trx->undo_no= undo->top_undo_no + 1;
  trx->rsegs.m_redo.rseg= rseg;
  /*
    For transactions with active data will not have rseg size = 1
    or will not qualify for purge limit criteria. So it is safe to increment
    this trx_ref_count w/o mutex protection.
  */
  ++trx->rsegs.m_redo.rseg->trx_ref_count;
  *trx->xid= undo->xid;
  trx->id= undo->trx_id;
  trx->is_recovered= true;
  trx->start_time= start_time;
  trx->start_time_micro= start_time_micro;

  if (undo->dict_operation)
  {
    trx_set_dict_operation(trx, TRX_DICT_OP_TABLE);
    if (!trx->table_id)
      trx->table_id= undo->table_id;
  }

  trx_sys.rw_trx_hash.insert(trx);
  trx_sys.rw_trx_hash.put_pins(trx);
  trx_resurrect_table_locks(trx, undo);
  if (trx_state_eq(trx, TRX_STATE_ACTIVE))
    *rows_to_undo+= trx->undo_no;
}


/** Initialize (resurrect) transactions at startup. */
void
trx_lists_init_at_db_start()
{
	ut_a(srv_is_being_started);
	ut_ad(!srv_was_started);

	if (srv_operation == SRV_OPERATION_RESTORE) {
		/* mariabackup --prepare only deals with
		the redo log and the data files, not with
		transactions or the data dictionary. */
		trx_rseg_array_init();
		return;
	}

	if (srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN) {
		return;
	}

	purge_sys.create();
	trx_rseg_array_init();

	/* Look from the rollback segments if there exist undo logs for
	transactions. */
	const time_t	start_time	= time(NULL);
	const ulonglong	start_time_micro= microsecond_interval_timer();
	uint64_t	rows_to_undo	= 0;

	for (ulint i = 0; i < TRX_SYS_N_RSEGS; ++i) {
		trx_undo_t*	undo;
		trx_rseg_t*	rseg = trx_sys.rseg_array[i];

		/* Some rollback segment may be unavailable,
		especially if the server was previously run with a
		non-default value of innodb_undo_logs. */
		if (rseg == NULL) {
			continue;
		}

		/* Resurrect transactions that were doing inserts
		using the old separate insert_undo log. */
		undo = UT_LIST_GET_FIRST(rseg->old_insert_list);
		while (undo) {
			trx_undo_t* next = UT_LIST_GET_NEXT(undo_list, undo);
			trx_resurrect(undo, rseg, start_time, start_time_micro,
				      &rows_to_undo, true);
			undo = next;
		}

		/* Ressurrect other transactions. */
		for (undo = UT_LIST_GET_FIRST(rseg->undo_list);
		     undo != NULL;
		     undo = UT_LIST_GET_NEXT(undo_list, undo)) {
			trx_t *trx = trx_sys.find(0, undo->trx_id, false);
			if (!trx) {
				trx_resurrect(undo, rseg, start_time,
					      start_time_micro,
					      &rows_to_undo, false);
			} else {
				ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE) ||
				      trx_state_eq(trx, TRX_STATE_PREPARED));
				ut_ad(trx->start_time == start_time);
				ut_ad(trx->is_recovered);
				ut_ad(trx->rsegs.m_redo.rseg == rseg);
				ut_ad(trx->rsegs.m_redo.rseg->trx_ref_count);

				trx->rsegs.m_redo.undo = undo;
				if (undo->top_undo_no >= trx->undo_no) {
					if (trx_state_eq(trx,
							 TRX_STATE_ACTIVE)) {
						rows_to_undo -= trx->undo_no;
						rows_to_undo +=
							undo->top_undo_no + 1;
					}

					trx->undo_no = undo->top_undo_no + 1;
				}
				trx_resurrect_table_locks(trx, undo);
			}
		}
	}

	if (trx_sys.rw_trx_hash.size()) {

		ib::info() << trx_sys.rw_trx_hash.size()
			<< " transaction(s) which must be rolled back or"
			" cleaned up in total " << rows_to_undo
			<< " row operations to undo";

		ib::info() << "Trx id counter is " << trx_sys.get_max_trx_id();
	}
	trx_sys.clone_oldest_view();
}

/** Assign a persistent rollback segment in a round-robin fashion,
evenly distributed between 0 and innodb_undo_logs-1
@return	persistent rollback segment
@retval	NULL	if innodb_read_only */
static trx_rseg_t* trx_assign_rseg_low()
{
	if (srv_read_only_mode) {
		ut_ad(srv_undo_logs == ULONG_UNDEFINED);
		return(NULL);
	}

	/* The first slot is always assigned to the system tablespace. */
	ut_ad(trx_sys.rseg_array[0]->space == fil_system.sys_space);

	/* Choose a rollback segment evenly distributed between 0 and
	innodb_undo_logs-1 in a round-robin fashion, skipping those
	undo tablespaces that are scheduled for truncation. */
	static Atomic_counter<unsigned>	rseg_slot;
	ulong	slot = ulong{rseg_slot++} % srv_undo_logs;
	trx_rseg_t*	rseg;

#ifdef UNIV_DEBUG
	ulint	start_scan_slot = slot;
	bool	look_for_rollover = false;
#endif /* UNIV_DEBUG */

	bool	allocated = false;

	do {
		for (;;) {
			rseg = trx_sys.rseg_array[slot];

#ifdef UNIV_DEBUG
			/* Ensure that we are not revisiting the same
			slot that we have already inspected. */
			if (look_for_rollover) {
				ut_ad(start_scan_slot != slot);
			}
			look_for_rollover = true;
#endif /* UNIV_DEBUG */

			slot = (slot + 1) % srv_undo_logs;

			if (rseg == NULL) {
				continue;
			}

			ut_ad(rseg->is_persistent());

			if (rseg->space != fil_system.sys_space) {
				if (rseg->skip_allocation
				    || !srv_undo_tablespaces) {
					continue;
				}
			} else if (trx_rseg_t* next
				   = trx_sys.rseg_array[slot]) {
				if (next->space != fil_system.sys_space
				    && srv_undo_tablespaces > 0) {
					/** If dedicated
					innodb_undo_tablespaces have
					been configured, try to use them
					instead of the system tablespace. */
					continue;
				}
			}

			break;
		}

		/* By now we have only selected the rseg but not marked it
		allocated. By marking it allocated we are ensuring that it will
		never be selected for UNDO truncate purge. */
		mutex_enter(&rseg->mutex);
		if (!rseg->skip_allocation) {
			rseg->trx_ref_count++;
			allocated = true;
		}
		mutex_exit(&rseg->mutex);
	} while (!allocated);

	ut_ad(rseg->trx_ref_count > 0);
	ut_ad(rseg->is_persistent());
	return(rseg);
}

/** Set the innodb_log_optimize_ddl page flush observer
@param[in,out]	space	tablespace
@param[in,out]	stage	performance_schema accounting */
void trx_t::set_flush_observer(fil_space_t* space, ut_stage_alter_t* stage)
{
	flush_observer = UT_NEW_NOKEY(FlushObserver(space, this, stage));
}

/** Remove the flush observer */
void trx_t::remove_flush_observer()
{
	UT_DELETE(flush_observer);
	flush_observer = NULL;
}

/** Assign a rollback segment for modifying temporary tables.
@return the assigned rollback segment */
trx_rseg_t*
trx_t::assign_temp_rseg()
{
	ut_ad(!rsegs.m_noredo.rseg);
	ut_ad(!trx_is_autocommit_non_locking(this));
	compile_time_assert(ut_is_2pow(TRX_SYS_N_RSEGS));

	/* Choose a temporary rollback segment between 0 and 127
	in a round-robin fashion. */
	static Atomic_counter<unsigned> rseg_slot;
	trx_rseg_t*	rseg = trx_sys.temp_rsegs[
		rseg_slot++ & (TRX_SYS_N_RSEGS - 1)];
	ut_ad(!rseg->is_persistent());
	rsegs.m_noredo.rseg = rseg;

	if (id == 0) {
		trx_sys.register_rw(this);
	}

	ut_ad(!rseg->is_persistent());
	return(rseg);
}

/****************************************************************//**
Starts a transaction. */
static
void
trx_start_low(
/*==========*/
	trx_t*	trx,		/*!< in: transaction */
	bool	read_write)	/*!< in: true if read-write transaction */
{
	ut_ad(!trx->in_rollback);
	ut_ad(!trx->is_recovered);
	ut_ad(trx->start_line != 0);
	ut_ad(trx->start_file != 0);
	ut_ad(trx->roll_limit == 0);
	ut_ad(trx->error_state == DB_SUCCESS);
	ut_ad(trx->rsegs.m_redo.rseg == NULL);
	ut_ad(trx->rsegs.m_noredo.rseg == NULL);
	ut_ad(trx_state_eq(trx, TRX_STATE_NOT_STARTED));
	ut_ad(UT_LIST_GET_LEN(trx->lock.trx_locks) == 0);

	/* Check whether it is an AUTOCOMMIT SELECT */
	trx->auto_commit = thd_trx_is_auto_commit(trx->mysql_thd);

	trx->read_only = srv_read_only_mode
		|| (!trx->ddl && !trx->internal
		    && thd_trx_is_read_only(trx->mysql_thd));

	if (!trx->auto_commit) {
		++trx->will_lock;
	} else if (trx->will_lock == 0) {
		trx->read_only = true;
	}

#ifdef WITH_WSREP
	trx->xid->null();
#endif /* WITH_WSREP */

	/* The initial value for trx->no: TRX_ID_MAX is used in
	read_view_open_now: */

	trx->no = TRX_ID_MAX;

	ut_a(ib_vector_is_empty(trx->autoinc_locks));
	ut_a(trx->lock.table_locks.empty());

	/* No other thread can access this trx object through rw_trx_hash, thus
	we don't need trx_sys.mutex protection for that purpose. Still this
	trx can be found through trx_sys.trx_list, which means state
	change must be protected by e.g. trx->mutex.

	For now we update it without mutex protection, because original code
	did it this way. It has to be reviewed and fixed properly. */
	trx->state = TRX_STATE_ACTIVE;

	/* By default all transactions are in the read-only list unless they
	are non-locking auto-commit read only transactions or background
	(internal) transactions. Note: Transactions marked explicitly as
	read only can write to temporary tables, we put those on the RO
	list too. */

	if (!trx->read_only
	    && (trx->mysql_thd == 0 || read_write || trx->ddl)) {

		/* Temporary rseg is assigned only if the transaction
		updates a temporary table */
		trx->rsegs.m_redo.rseg = trx_assign_rseg_low();
		ut_ad(trx->rsegs.m_redo.rseg != 0
		      || srv_read_only_mode
		      || srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO);

		trx_sys.register_rw(trx);
	} else {
		if (!trx_is_autocommit_non_locking(trx)) {

			/* If this is a read-only transaction that is writing
			to a temporary table then it needs a transaction id
			to write to the temporary table. */

			if (read_write) {
				ut_ad(!srv_read_only_mode);
				trx_sys.register_rw(trx);
			}
		} else {
			ut_ad(!read_write);
		}
	}

	trx->start_time = time(NULL);
	trx->start_time_micro = trx->mysql_thd
		? thd_query_start_micro(trx->mysql_thd)
		: microsecond_interval_timer();

	ut_a(trx->error_state == DB_SUCCESS);

	MONITOR_INC(MONITOR_TRX_ACTIVE);
}

/** Set the serialisation number for a persistent committed transaction.
@param[in,out]	trx	committed transaction with persistent changes */
static
void
trx_serialise(trx_t* trx)
{
	trx_rseg_t *rseg = trx->rsegs.m_redo.rseg;
	ut_ad(rseg);
	ut_ad(mutex_own(&rseg->mutex));

	if (rseg->last_page_no == FIL_NULL) {
		mutex_enter(&purge_sys.pq_mutex);
	}

	trx_sys.assign_new_trx_no(trx);

	/* If the rollback segment is not empty then the
	new trx_t::no can't be less than any trx_t::no
	already in the rollback segment. User threads only
	produce events when a rollback segment is empty. */
	if (rseg->last_page_no == FIL_NULL) {
		purge_sys.purge_queue.push(TrxUndoRsegs(trx->no, *rseg));
		mutex_exit(&purge_sys.pq_mutex);
	}
}

/****************************************************************//**
Assign the transaction its history serialisation number and write the
update UNDO log record to the assigned rollback segment. */
static
void
trx_write_serialisation_history(
/*============================*/
	trx_t*		trx,	/*!< in/out: transaction */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	/* Change the undo log segment states from TRX_UNDO_ACTIVE to some
	other state: these modifications to the file data structure define
	the transaction as committed in the file based domain, at the
	serialization point of the log sequence number lsn obtained below. */

	/* We have to hold the rseg mutex because update log headers have
	to be put to the history list in the (serialisation) order of the
	UNDO trx number. This is required for the purge in-memory data
	structures too. */

	if (trx_undo_t* undo = trx->rsegs.m_noredo.undo) {
		/* Undo log for temporary tables is discarded at transaction
		commit. There is no purge for temporary tables, and also no
		MVCC, because they are private to a session. */

		mtr_t	temp_mtr;
		temp_mtr.start();
		temp_mtr.set_log_mode(MTR_LOG_NO_REDO);

		mutex_enter(&trx->rsegs.m_noredo.rseg->mutex);
		trx_undo_set_state_at_finish(undo, &temp_mtr);
		mutex_exit(&trx->rsegs.m_noredo.rseg->mutex);
		temp_mtr.commit();
	}

	trx_rseg_t*	rseg = trx->rsegs.m_redo.rseg;
	if (!rseg) {
		ut_ad(!trx->rsegs.m_redo.undo);
		ut_ad(!trx->rsegs.m_redo.old_insert);
		return;
	}

	trx_undo_t*& undo = trx->rsegs.m_redo.undo;
	trx_undo_t*& old_insert = trx->rsegs.m_redo.old_insert;

	if (!undo && !old_insert) {
		return;
	}

	ut_ad(!trx->read_only);
	ut_ad(!undo || undo->rseg == rseg);
	ut_ad(!old_insert || old_insert->rseg == rseg);
	mutex_enter(&rseg->mutex);

	/* Assign the transaction serialisation number and add any
	undo log to the purge queue. */
	trx_serialise(trx);

	if (UNIV_LIKELY_NULL(old_insert)) {
		UT_LIST_REMOVE(rseg->old_insert_list, old_insert);
		trx_purge_add_undo_to_history(trx, old_insert, mtr);
	}
	if (undo) {
		UT_LIST_REMOVE(rseg->undo_list, undo);
		trx_purge_add_undo_to_history(trx, undo, mtr);
	}

	mutex_exit(&rseg->mutex);

	MONITOR_INC(MONITOR_TRX_COMMIT_UNDO);

	trx->mysql_log_file_name = NULL;
}

/********************************************************************
Finalize a transaction containing updates for a FTS table. */
static
void
trx_finalize_for_fts_table(
/*=======================*/
	fts_trx_table_t*	ftt)	    /* in: FTS trx table */
{
	fts_t*		  fts = ftt->table->fts;
	fts_doc_ids_t*	  doc_ids = ftt->added_doc_ids;

	ut_a(fts->add_wq);

	mem_heap_t* heap = static_cast<mem_heap_t*>(doc_ids->self_heap->arg);

	ib_wqueue_add(fts->add_wq, doc_ids, heap);

	/* fts_trx_table_t no longer owns the list. */
	ftt->added_doc_ids = NULL;
}

/******************************************************************//**
Finalize a transaction containing updates to FTS tables. */
static
void
trx_finalize_for_fts(
/*=================*/
	trx_t*	trx,		/*!< in/out: transaction */
	bool	is_commit)	/*!< in: true if the transaction was
				committed, false if it was rolled back. */
{
	if (is_commit) {
		const ib_rbt_node_t*	node;
		ib_rbt_t*		tables;
		fts_savepoint_t*	savepoint;

		savepoint = static_cast<fts_savepoint_t*>(
			ib_vector_last(trx->fts_trx->savepoints));

		tables = savepoint->tables;

		for (node = rbt_first(tables);
		     node;
		     node = rbt_next(tables, node)) {
			fts_trx_table_t**	ftt;

			ftt = rbt_value(fts_trx_table_t*, node);

			if ((*ftt)->added_doc_ids) {
				trx_finalize_for_fts_table(*ftt);
			}
		}
	}

	fts_trx_free(trx->fts_trx);
	trx->fts_trx = NULL;
}

/**********************************************************************//**
If required, flushes the log to disk based on the value of
innodb_flush_log_at_trx_commit. */
static
void
trx_flush_log_if_needed_low(
/*========================*/
	lsn_t	lsn)	/*!< in: lsn up to which logs are to be
			flushed. */
{
	bool	flush = srv_file_flush_method != SRV_NOSYNC;

	switch (srv_flush_log_at_trx_commit) {
	case 3:
	case 2:
		/* Write the log but do not flush it to disk */
		flush = false;
		/* fall through */
	case 1:
		/* Write the log and optionally flush it to disk */
		log_write_up_to(lsn, flush);
		return;
	case 0:
		/* Do nothing */
		return;
	}

	ut_error;
}

/**********************************************************************//**
If required, flushes the log to disk based on the value of
innodb_flush_log_at_trx_commit. */
static
void
trx_flush_log_if_needed(
/*====================*/
	lsn_t	lsn,	/*!< in: lsn up to which logs are to be
			flushed. */
	trx_t*	trx)	/*!< in/out: transaction */
{
	trx->op_info = "flushing log";
	trx_flush_log_if_needed_low(lsn);
	trx->op_info = "";
}

/**********************************************************************//**
For each table that has been modified by the given transaction: update
its dict_table_t::update_time with the current timestamp. Clear the list
of the modified tables at the end. */
static
void
trx_update_mod_tables_timestamp(
/*============================*/
	trx_t*	trx)	/*!< in: transaction */
{
	/* consider using trx->start_time if calling time() is too
	expensive here */
	const time_t now = time(NULL);

	trx_mod_tables_t::const_iterator	end = trx->mod_tables.end();

	for (trx_mod_tables_t::const_iterator it = trx->mod_tables.begin();
	     it != end;
	     ++it) {

		/* This could be executed by multiple threads concurrently
		on the same table object. This is fine because time_t is
		word size or less. And _purely_ _theoretically_, even if
		time_t write is not atomic, likely the value of 'now' is
		the same in all threads and even if it is not, getting a
		"garbage" in table->update_time is justified because
		protecting it with a latch here would be too performance
		intrusive. */
		dict_table_t* table = it->first;
		table->update_time = now;
	}

	trx->mod_tables.clear();
}

/** Evict a table definition due to the rollback of ALTER TABLE.
@param[in]	table_id	table identifier */
void trx_t::evict_table(table_id_t table_id)
{
	ut_ad(in_rollback);

	dict_table_t* table = dict_table_open_on_id(
		table_id, true, DICT_TABLE_OP_OPEN_ONLY_IF_CACHED);
	if (!table) {
		return;
	}

	if (!table->release()) {
		/* This must be a DDL operation that is being rolled
		back in an active connection. */
		ut_a(table->get_ref_count() == 1);
		ut_ad(!is_recovered);
		ut_ad(mysql_thd);
		return;
	}

	/* This table should only be locked by this transaction, if at all. */
	ut_ad(UT_LIST_GET_LEN(table->locks) <= 1);
	const bool locked = UT_LIST_GET_LEN(table->locks);
	ut_ad(!locked || UT_LIST_GET_FIRST(table->locks)->trx == this);
	dict_sys.remove(table, true, locked);
	if (locked) {
		UT_LIST_ADD_FIRST(lock.evicted_tables, table);
	}
}

/** Mark a transaction committed in the main memory data structures. */
inline void trx_t::commit_in_memory(const mtr_t *mtr)
{
  must_flush_log_later= false;
  read_view.close();

  if (trx_is_autocommit_non_locking(this))
  {
    ut_ad(id == 0);
    ut_ad(read_only);
    ut_a(!is_recovered);
    ut_ad(!rsegs.m_redo.rseg);

    /* Note: We are asserting without holding the lock mutex. But
    that is OK because this transaction is not waiting and cannot
    be rolled back and no new locks can (or should) be added
    because it is flagged as a non-locking read-only transaction. */
    ut_a(UT_LIST_GET_LEN(lock.trx_locks) == 0);

    /* This state change is not protected by any mutex, therefore
    there is an inherent race here around state transition during
    printouts. We ignore this race for the sake of efficiency.
    However, the trx_sys_t::mutex will protect the trx_t instance
    and it cannot be removed from the trx_list and freed
    without first acquiring the trx_sys_t::mutex. */
    ut_ad(trx_state_eq(this, TRX_STATE_ACTIVE));

    MONITOR_INC(MONITOR_TRX_NL_RO_COMMIT);

    DBUG_LOG("trx", "Autocommit in memory: " << this);
    state= TRX_STATE_NOT_STARTED;
  }
  else
  {
#ifdef UNIV_DEBUG
    if (!UT_LIST_GET_LEN(lock.trx_locks))
      for (auto l : lock.table_locks)
        ut_ad(!l);
#endif /* UNIV_DEBUG */
    commit_state();

    if (id)
    {
      trx_sys.deregister_rw(this);

      /* Wait for any implicit-to-explicit lock conversions to cease,
      so that there will be no race condition in lock_release(). */
      while (UNIV_UNLIKELY(is_referenced()))
        ut_delay(srv_spin_wait_delay);
    }
    else
      ut_ad(read_only || !rsegs.m_redo.rseg);

    if (read_only || !rsegs.m_redo.rseg)
    {
      MONITOR_INC(MONITOR_TRX_RO_COMMIT);
    }
    else
    {
      trx_update_mod_tables_timestamp(this);
      MONITOR_INC(MONITOR_TRX_RW_COMMIT);
      is_recovered= false;
    }

    release_locks();
    id= 0;
    DEBUG_SYNC_C("after_trx_committed_in_memory");

    while (dict_table_t *table= UT_LIST_GET_FIRST(lock.evicted_tables))
    {
      UT_LIST_REMOVE(lock.evicted_tables, table);
      dict_mem_table_free(table);
    }
  }

  ut_ad(!rsegs.m_redo.undo);
  ut_ad(UT_LIST_GET_LEN(lock.evicted_tables) == 0);

  if (trx_rseg_t *rseg= rsegs.m_redo.rseg)
  {
    mutex_enter(&rseg->mutex);
    ut_ad(rseg->trx_ref_count > 0);
    --rseg->trx_ref_count;
    mutex_exit(&rseg->mutex);

    if (trx_undo_t *&insert= rsegs.m_redo.old_insert)
    {
      ut_ad(insert->rseg == rseg);
      trx_undo_commit_cleanup(insert, false);
      insert= nullptr;
    }
  }

  ut_ad(!rsegs.m_redo.old_insert);

  if (mtr)
  {
    if (trx_undo_t *&undo= rsegs.m_noredo.undo)
    {
      ut_ad(undo->rseg == rsegs.m_noredo.rseg);
      trx_undo_commit_cleanup(undo, true);
      undo= nullptr;
    }

    /* NOTE that we could possibly make a group commit more efficient
    here: call os_thread_yield here to allow also other trxs to come
    to commit! */

    /*-------------------------------------*/

    /* Depending on the my.cnf options, we may now write the log
    buffer to the log files, making the transaction durable if the OS
    does not crash. We may also flush the log files to disk, making
    the transaction durable also at an OS crash or a power outage.

    The idea in InnoDB's group commit is that a group of transactions
    gather behind a trx doing a physical disk write to log files, and
    when that physical write has been completed, one of those
    transactions does a write which commits the whole group. Note that
    this group commit will only bring benefit if there are > 2 users
    in the database. Then at least 2 users can gather behind one doing
    the physical log write to disk.

    If we are calling trx_t::commit() under prepare_commit_mutex, we
    will delay possible log write and flush to a separate function
    trx_commit_complete_for_mysql(), which is only called when the
    thread has released the mutex. This is to make the group commit
    algorithm to work. Otherwise, the prepare_commit mutex would
    serialize all commits and prevent a group of transactions from
    gathering. */

    commit_lsn= mtr->commit_lsn();
    if (!commit_lsn)
      /* Nothing to be done. */;
    else if (flush_log_later)
      /* Do nothing yet */
      must_flush_log_later= true;
    else if (srv_flush_log_at_trx_commit)
      trx_flush_log_if_needed(commit_lsn, this);

    /* Tell server some activity has happened, since the trx does
    changes something. Background utility threads like master thread,
    purge thread or page_cleaner thread might have some work to do. */
    srv_active_wake_master_thread();
  }

  ut_ad(!rsegs.m_noredo.undo);

  /* Free all savepoints, starting from the first. */
  trx_named_savept_t *savep= UT_LIST_GET_FIRST(trx_savepoints);

  trx_roll_savepoints_free(this, savep);

  if (fts_trx)
    trx_finalize_for_fts(this, undo_no != 0);

#ifdef WITH_WSREP
  /* Serialization history has been written and the transaction is
  committed in memory, which makes this commit ordered. Release commit
  order critical section. */
  if (wsrep)
  {
    wsrep= false;
    wsrep_commit_ordered(mysql_thd);
  }
  lock.was_chosen_as_wsrep_victim= false;
#endif /* WITH_WSREP */
  trx_mutex_enter(this);
  dict_operation= TRX_DICT_OP_NONE;

  DBUG_LOG("trx", "Commit in memory: " << this);
  state= TRX_STATE_NOT_STARTED;

  assert_freed();
  trx_init(this);
  trx_mutex_exit(this);

  ut_a(error_state == DB_SUCCESS);
  if (!srv_read_only_mode)
    srv_wake_purge_thread_if_not_active();
}

/** Commit the transaction in a mini-transaction.
@param mtr  mini-transaction (if there are any persistent modifications) */
void trx_t::commit_low(mtr_t *mtr)
{
  assert_trx_nonlocking_or_in_list(this);
  ut_ad(!trx_state_eq(this, TRX_STATE_COMMITTED_IN_MEMORY));
  ut_ad(!mtr || mtr->is_active());
  ut_d(bool aborted = in_rollback && error_state == DB_DEADLOCK);
  ut_ad(!mtr == (aborted || !has_logged_or_recovered()));
  ut_ad(!mtr || !aborted);

  /* undo_no is non-zero if we're doing the final commit. */
  if (fts_trx && undo_no)
  {
    ut_a(!trx_is_autocommit_non_locking(this));
    dberr_t error= fts_commit(this);
    /* FTS-FIXME: Temporarily tolerate DB_DUPLICATE_KEY instead of
    dying. This is a possible scenario if there is a crash between
    insert to DELETED table committing and transaction committing. The
    fix would be able to return error from this function */
    ut_a(error == DB_SUCCESS || error == DB_DUPLICATE_KEY);
  }

#ifndef DBUG_OFF
  const bool debug_sync= mysql_thd && has_logged_persistent();
#endif

  if (mtr)
  {
    trx_write_serialisation_history(this, mtr);

    /* The following call commits the mini-transaction, making the
    whole transaction committed in the file-based world, at this log
    sequence number. The transaction becomes 'durable' when we write
    the log to disk, but in the logical sense the commit in the
    file-based data structures (undo logs etc.) happens here.

    NOTE that transaction numbers, which are assigned only to
    transactions with an update undo log, do not necessarily come in
    exactly the same order as commit lsn's, if the transactions have
    different rollback segments. To get exactly the same order we
    should hold the kernel mutex up to this point, adding to the
    contention of the kernel mutex. However, if a transaction T2 is
    able to see modifications made by a transaction T1, T2 will always
    get a bigger transaction number and a bigger commit lsn than T1. */

    mtr->commit();
  }
#ifndef DBUG_OFF
  if (debug_sync)
    DEBUG_SYNC_C("before_trx_state_committed_in_memory");
#endif

  commit_in_memory(mtr);
}


void trx_t::commit()
{
  mtr_t *mtr= nullptr;
  mtr_t local_mtr;

  if (has_logged_or_recovered())
  {
    mtr= &local_mtr;
    local_mtr.start();
  }
  commit_low(mtr);
}

/****************************************************************//**
Prepares a transaction for commit/rollback. */
void
trx_commit_or_rollback_prepare(
/*===========================*/
	trx_t*	trx)		/*!< in/out: transaction */
{
	/* We are reading trx->state without holding trx_sys.mutex
	here, because the commit or rollback should be invoked for a
	running (or recovered prepared) transaction that is associated
	with the current thread. */

	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		trx_start_low(trx, true);
		/* fall through */

	case TRX_STATE_ACTIVE:
	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
		/* If the trx is in a lock wait state, moves the waiting
		query thread to the suspended state */

		if (trx->lock.que_state == TRX_QUE_LOCK_WAIT) {

			ut_a(trx->lock.wait_thr != NULL);
			trx->lock.wait_thr->state = QUE_THR_SUSPENDED;
			trx->lock.wait_thr = NULL;

			trx->lock.que_state = TRX_QUE_RUNNING;
		}

		ut_a(trx->lock.n_active_thrs == 1);
		return;

	case TRX_STATE_COMMITTED_IN_MEMORY:
		break;
	}

	ut_error;
}

/*********************************************************************//**
Creates a commit command node struct.
@return own: commit node struct */
commit_node_t*
trx_commit_node_create(
/*===================*/
	mem_heap_t*	heap)	/*!< in: mem heap where created */
{
	commit_node_t*	node;

	node = static_cast<commit_node_t*>(mem_heap_alloc(heap, sizeof(*node)));
	node->common.type  = QUE_NODE_COMMIT;
	node->state = COMMIT_NODE_SEND;

	return(node);
}

/***********************************************************//**
Performs an execution step for a commit type node in a query graph.
@return query thread to run next, or NULL */
que_thr_t*
trx_commit_step(
/*============*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	commit_node_t*	node;

	node = static_cast<commit_node_t*>(thr->run_node);

	ut_ad(que_node_get_type(node) == QUE_NODE_COMMIT);

	if (thr->prev_node == que_node_get_parent(node)) {
		node->state = COMMIT_NODE_SEND;
	}

	if (node->state == COMMIT_NODE_SEND) {
		trx_t*	trx;

		node->state = COMMIT_NODE_WAIT;

		trx = thr_get_trx(thr);

		ut_a(trx->lock.wait_thr == NULL);
		ut_a(trx->lock.que_state != TRX_QUE_LOCK_WAIT);

		trx_commit_or_rollback_prepare(trx);

		trx->lock.que_state = TRX_QUE_COMMITTING;
		trx->commit();
		ut_ad(trx->lock.wait_thr == NULL);
		trx->lock.que_state = TRX_QUE_RUNNING;

		thr = NULL;
	} else {
		ut_ad(node->state == COMMIT_NODE_WAIT);

		node->state = COMMIT_NODE_SEND;

		thr->run_node = que_node_get_parent(node);
	}

	return(thr);
}

/**********************************************************************//**
Does the transaction commit for MySQL.
@return DB_SUCCESS or error number */
dberr_t
trx_commit_for_mysql(
/*=================*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	/* Because we do not do the commit by sending an Innobase
	sig to the transaction, we must here make sure that trx has been
	started. */

	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		ut_d(trx->start_file = __FILE__);
		ut_d(trx->start_line = __LINE__);

		trx_start_low(trx, true);
		/* fall through */
	case TRX_STATE_ACTIVE:
	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
		trx->op_info = "committing";
		trx->commit();
		MONITOR_DEC(MONITOR_TRX_ACTIVE);
		trx->op_info = "";
		return(DB_SUCCESS);
	case TRX_STATE_COMMITTED_IN_MEMORY:
		break;
	}
	ut_error;
	return(DB_CORRUPTION);
}

/**********************************************************************//**
If required, flushes the log to disk if we called trx_commit_for_mysql()
with trx->flush_log_later == TRUE. */
void
trx_commit_complete_for_mysql(
/*==========================*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	if (trx->id != 0
	    || !trx->must_flush_log_later
	    || (srv_flush_log_at_trx_commit == 1 && trx->active_commit_ordered)) {

		return;
	}

	trx_flush_log_if_needed(trx->commit_lsn, trx);

	trx->must_flush_log_later = false;
}

/**********************************************************************//**
Marks the latest SQL statement ended. */
void
trx_mark_sql_stat_end(
/*==================*/
	trx_t*	trx)	/*!< in: trx handle */
{
	ut_a(trx);

	switch (trx->state) {
	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
	case TRX_STATE_COMMITTED_IN_MEMORY:
		break;
	case TRX_STATE_NOT_STARTED:
		trx->undo_no = 0;
		/* fall through */
	case TRX_STATE_ACTIVE:
		trx->last_sql_stat_start.least_undo_no = trx->undo_no;

		if (trx->fts_trx != NULL) {
			fts_savepoint_laststmt_refresh(trx);
		}

		return;
	}

	ut_error;
}

/**********************************************************************//**
Prints info about a transaction. */
void
trx_print_low(
/*==========*/
	FILE*		f,
			/*!< in: output stream */
	const trx_t*	trx,
			/*!< in: transaction */
	ulint		max_query_len,
			/*!< in: max query length to print,
			or 0 to use the default max length */
	ulint		n_rec_locks,
			/*!< in: lock_number_of_rows_locked(&trx->lock) */
	ulint		n_trx_locks,
			/*!< in: length of trx->lock.trx_locks */
	ulint		heap_size)
			/*!< in: mem_heap_get_size(trx->lock.lock_heap) */
{
	ibool		newline;
	const char*	op_info;

	fprintf(f, "TRANSACTION " TRX_ID_FMT, trx_get_id_for_print(trx));

	/* trx->state cannot change from or to NOT_STARTED while we
	are holding the trx_sys.mutex. It may change from ACTIVE to
	PREPARED or COMMITTED. */
	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		fputs(", not started", f);
		goto state_ok;
	case TRX_STATE_ACTIVE:
		fprintf(f, ", ACTIVE %lu sec",
			(ulong) difftime(time(NULL), trx->start_time));
		goto state_ok;
	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
		fprintf(f, ", ACTIVE (PREPARED) %lu sec",
			(ulong) difftime(time(NULL), trx->start_time));
		goto state_ok;
	case TRX_STATE_COMMITTED_IN_MEMORY:
		fputs(", COMMITTED IN MEMORY", f);
		goto state_ok;
	}
	fprintf(f, ", state %lu", (ulong) trx->state);
	ut_ad(0);
state_ok:

	/* prevent a race condition */
	op_info = trx->op_info;

	if (*op_info) {
		putc(' ', f);
		fputs(op_info, f);
	}

	if (trx->is_recovered) {
		fputs(" recovered trx", f);
	}

	if (trx->declared_to_be_inside_innodb) {
		fprintf(f, ", thread declared inside InnoDB %lu",
			(ulong) trx->n_tickets_to_enter_innodb);
	}

	putc('\n', f);

	if (trx->n_mysql_tables_in_use > 0 || trx->mysql_n_tables_locked > 0) {
		fprintf(f, "mysql tables in use %lu, locked %lu\n",
			(ulong) trx->n_mysql_tables_in_use,
			(ulong) trx->mysql_n_tables_locked);
	}

	newline = TRUE;

	/* trx->lock.que_state of an ACTIVE transaction may change
	while we are not holding trx->mutex. We perform a dirty read
	for performance reasons. */

	switch (trx->lock.que_state) {
	case TRX_QUE_RUNNING:
		newline = FALSE; break;
	case TRX_QUE_LOCK_WAIT:
		fputs("LOCK WAIT ", f); break;
	case TRX_QUE_ROLLING_BACK:
		fputs("ROLLING BACK ", f); break;
	case TRX_QUE_COMMITTING:
		fputs("COMMITTING ", f); break;
	default:
		fprintf(f, "que state %lu ", (ulong) trx->lock.que_state);
	}

	if (n_trx_locks > 0 || heap_size > 400) {
		newline = TRUE;

		fprintf(f, "%lu lock struct(s), heap size %lu,"
			" %lu row lock(s)",
			(ulong) n_trx_locks,
			(ulong) heap_size,
			(ulong) n_rec_locks);
	}

	if (trx->undo_no != 0) {
		newline = TRUE;
		fprintf(f, ", undo log entries " TRX_ID_FMT, trx->undo_no);
	}

	if (newline) {
		putc('\n', f);
	}

	if (trx->state != TRX_STATE_NOT_STARTED && trx->mysql_thd != NULL) {
		innobase_mysql_print_thd(
			f, trx->mysql_thd, static_cast<uint>(max_query_len));
	}
}

/**********************************************************************//**
Prints info about a transaction.
The caller must hold lock_sys.mutex.
When possible, use trx_print() instead. */
void
trx_print_latched(
/*==============*/
	FILE*		f,		/*!< in: output stream */
	const trx_t*	trx,		/*!< in: transaction */
	ulint		max_query_len)	/*!< in: max query length to print,
					or 0 to use the default max length */
{
	ut_ad(lock_mutex_own());

	trx_print_low(f, trx, max_query_len,
		      lock_number_of_rows_locked(&trx->lock),
		      UT_LIST_GET_LEN(trx->lock.trx_locks),
		      mem_heap_get_size(trx->lock.lock_heap));
}

/**********************************************************************//**
Prints info about a transaction.
Acquires and releases lock_sys.mutex. */
void
trx_print(
/*======*/
	FILE*		f,		/*!< in: output stream */
	const trx_t*	trx,		/*!< in: transaction */
	ulint		max_query_len)	/*!< in: max query length to print,
					or 0 to use the default max length */
{
	ulint	n_rec_locks;
	ulint	n_trx_locks;
	ulint	heap_size;

	lock_mutex_enter();
	n_rec_locks = lock_number_of_rows_locked(&trx->lock);
	n_trx_locks = UT_LIST_GET_LEN(trx->lock.trx_locks);
	heap_size = mem_heap_get_size(trx->lock.lock_heap);
	lock_mutex_exit();

	trx_print_low(f, trx, max_query_len,
		      n_rec_locks, n_trx_locks, heap_size);
}

/*******************************************************************//**
Compares the "weight" (or size) of two transactions. Transactions that
have edited non-transactional tables are considered heavier than ones
that have not.
@return TRUE if weight(a) >= weight(b) */
bool
trx_weight_ge(
/*==========*/
	const trx_t*	a,	/*!< in: transaction to be compared */
	const trx_t*	b)	/*!< in: transaction to be compared */
{
	ibool	a_notrans_edit;
	ibool	b_notrans_edit;

	/* If mysql_thd is NULL for a transaction we assume that it has
	not edited non-transactional tables. */

	a_notrans_edit = a->mysql_thd != NULL
		&& thd_has_edited_nontrans_tables(a->mysql_thd);

	b_notrans_edit = b->mysql_thd != NULL
		&& thd_has_edited_nontrans_tables(b->mysql_thd);

	if (a_notrans_edit != b_notrans_edit) {

		return(a_notrans_edit);
	}

	/* Either both had edited non-transactional tables or both had
	not, we fall back to comparing the number of altered/locked
	rows. */

	return(TRX_WEIGHT(a) >= TRX_WEIGHT(b));
}

/** Prepare a transaction.
@return	log sequence number that makes the XA PREPARE durable
@retval	0	if no changes needed to be made durable */
static
lsn_t
trx_prepare_low(trx_t* trx)
{
	ut_ad(!trx->rsegs.m_redo.old_insert);
	ut_ad(!trx->is_recovered);

	mtr_t	mtr;

	if (trx_undo_t* undo = trx->rsegs.m_noredo.undo) {
		ut_ad(undo->rseg == trx->rsegs.m_noredo.rseg);

		mtr.start();
		mtr.set_log_mode(MTR_LOG_NO_REDO);

		mutex_enter(&undo->rseg->mutex);
		trx_undo_set_state_at_prepare(trx, undo, false, &mtr);
		mutex_exit(&undo->rseg->mutex);

		mtr.commit();
	}

	trx_undo_t* undo = trx->rsegs.m_redo.undo;

	if (!undo) {
		/* There were no changes to persistent tables. */
		return(0);
	}

	trx_rseg_t*	rseg = trx->rsegs.m_redo.rseg;
	ut_ad(undo->rseg == rseg);

	mtr.start();

	/* Change the undo log segment states from TRX_UNDO_ACTIVE to
	TRX_UNDO_PREPARED: these modifications to the file data
	structure define the transaction as prepared in the file-based
	world, at the serialization point of lsn. */

	mutex_enter(&rseg->mutex);
	trx_undo_set_state_at_prepare(trx, undo, false, &mtr);
	mutex_exit(&rseg->mutex);

	/* Make the XA PREPARE durable. */
	mtr.commit();
	ut_ad(mtr.commit_lsn() > 0);
	return(mtr.commit_lsn());
}

/****************************************************************//**
Prepares a transaction. */
static
void
trx_prepare(
/*========*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	/* Only fresh user transactions can be prepared.
	Recovered transactions cannot. */
	ut_a(!trx->is_recovered);

	lsn_t	lsn = trx_prepare_low(trx);

	DBUG_EXECUTE_IF("ib_trx_crash_during_xa_prepare_step", DBUG_SUICIDE(););

	ut_a(trx->state == TRX_STATE_ACTIVE);
	trx_mutex_enter(trx);
	trx->state = TRX_STATE_PREPARED;
	trx_mutex_exit(trx);

	if (lsn) {
		/* Depending on the my.cnf options, we may now write the log
		buffer to the log files, making the prepared state of the
		transaction durable if the OS does not crash. We may also
		flush the log files to disk, making the prepared state of the
		transaction durable also at an OS crash or a power outage.

		The idea in InnoDB's group prepare is that a group of
		transactions gather behind a trx doing a physical disk write
		to log files, and when that physical write has been completed,
		one of those transactions does a write which prepares the whole
		group. Note that this group prepare will only bring benefit if
		there are > 2 users in the database. Then at least 2 users can
		gather behind one doing the physical log write to disk.

		We must not be holding any mutexes or latches here. */

		trx_flush_log_if_needed(lsn, trx);
	}
}

/** XA PREPARE a transaction.
@param[in,out]	trx	transaction to prepare */
void trx_prepare_for_mysql(trx_t* trx)
{
	trx_start_if_not_started_xa(trx, false);

	trx->op_info = "preparing";

	trx_prepare(trx);

	trx->op_info = "";
}


struct trx_recover_for_mysql_callback_arg
{
  XID *xid_list;
  uint len;
  uint count;
};


static my_bool trx_recover_for_mysql_callback(rw_trx_hash_element_t *element,
  trx_recover_for_mysql_callback_arg *arg)
{
  DBUG_ASSERT(arg->len > 0);
  mutex_enter(&element->mutex);
  if (trx_t *trx= element->trx)
  {
    /*
      The state of a read-write transaction can only change from ACTIVE to
      PREPARED while we are holding the element->mutex. But since it is
      executed at startup no state change should occur.
    */
    if (trx_state_eq(trx, TRX_STATE_PREPARED))
    {
      ut_ad(trx->is_recovered);
      ut_ad(trx->id);
      if (arg->count == 0)
        ib::info() << "Starting recovery for XA transactions...";
      XID& xid= arg->xid_list[arg->count];
      if (arg->count++ < arg->len)
      {
        trx->state= TRX_STATE_PREPARED_RECOVERED;
        ib::info() << "Transaction " << trx->id
                   << " in prepared state after recovery";
        ib::info() << "Transaction contains changes to " << trx->undo_no
                   << " rows";
        xid= *trx->xid;
      }
    }
  }
  mutex_exit(&element->mutex);
  /* Do not terminate upon reaching arg->len; count all transactions */
  return false;
}


static my_bool trx_recover_reset_callback(rw_trx_hash_element_t *element,
  void*)
{
  mutex_enter(&element->mutex);
  if (trx_t *trx= element->trx)
  {
    if (trx_state_eq(trx, TRX_STATE_PREPARED_RECOVERED))
      trx->state= TRX_STATE_PREPARED;
  }
  mutex_exit(&element->mutex);
  return false;
}


/**
  Find prepared transaction objects for recovery.

  @param[out]  xid_list  prepared transactions
  @param[in]   len       number of slots in xid_list

  @return number of prepared transactions stored in xid_list
*/

int trx_recover_for_mysql(XID *xid_list, uint len)
{
  trx_recover_for_mysql_callback_arg arg= { xid_list, len, 0 };

  ut_ad(xid_list);
  ut_ad(len);

  /* Fill xid_list with PREPARED transactions. */
  trx_sys.rw_trx_hash.iterate_no_dups(reinterpret_cast<my_hash_walk_action>
                                      (trx_recover_for_mysql_callback), &arg);
  if (arg.count)
  {
    ib::info() << arg.count
        << " transactions in prepared state after recovery";
    /* After returning the full list, reset the state, because
    init_server_components() wants to recover the collection of
    transactions twice, by first calling tc_log->open() and then
    ha_recover() directly. */
    if (arg.count <= len)
      trx_sys.rw_trx_hash.iterate(reinterpret_cast<my_hash_walk_action>
                                  (trx_recover_reset_callback), NULL);
  }
  return int(std::min(arg.count, len));
}


struct trx_get_trx_by_xid_callback_arg
{
  const XID *xid;
  trx_t *trx;
};


static my_bool trx_get_trx_by_xid_callback(rw_trx_hash_element_t *element,
  trx_get_trx_by_xid_callback_arg *arg)
{
  my_bool found= 0;
  mutex_enter(&element->mutex);
  if (trx_t *trx= element->trx)
  {
    trx_mutex_enter(trx);
    if (trx->is_recovered &&
	(trx_state_eq(trx, TRX_STATE_PREPARED) ||
	 trx_state_eq(trx, TRX_STATE_PREPARED_RECOVERED)) &&
        arg->xid->eq(reinterpret_cast<XID*>(trx->xid)))
    {
#ifdef WITH_WSREP
      /* The commit of a prepared recovered Galera
      transaction needs a valid trx->xid for
      invoking trx_sys_update_wsrep_checkpoint(). */
      if (!wsrep_is_wsrep_xid(trx->xid))
#endif /* WITH_WSREP */
      /* Invalidate the XID, so that subsequent calls will not find it. */
      trx->xid->null();
      arg->trx= trx;
      found= 1;
    }
    trx_mutex_exit(trx);
  }
  mutex_exit(&element->mutex);
  return found;
}

/** Look up an X/Open distributed transaction in XA PREPARE state.
@param[in]	xid	X/Open XA transaction identifier
@return	transaction on match (the trx_t::xid will be invalidated);
note that the trx may have been committed before the caller acquires
trx_t::mutex
@retval	NULL if no match */
trx_t* trx_get_trx_by_xid(const XID* xid)
{
  trx_get_trx_by_xid_callback_arg arg= { xid, 0 };

  if (xid)
    trx_sys.rw_trx_hash.iterate(reinterpret_cast<my_hash_walk_action>
                                (trx_get_trx_by_xid_callback), &arg);
  return arg.trx;
}


/*************************************************************//**
Starts the transaction if it is not yet started. */
void
trx_start_if_not_started_xa_low(
/*============================*/
	trx_t*	trx,		/*!< in/out: transaction */
	bool	read_write)	/*!< in: true if read write transaction */
{
	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		trx_start_low(trx, read_write);
		return;

	case TRX_STATE_ACTIVE:
		if (trx->id == 0 && read_write) {
			/* If the transaction is tagged as read-only then
			it can only write to temp tables and for such
			transactions we don't want to move them to the
			trx_sys_t::rw_trx_hash. */
			if (!trx->read_only) {
				trx_set_rw_mode(trx);
			}
		}
		return;
	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
	case TRX_STATE_COMMITTED_IN_MEMORY:
		break;
	}

	ut_error;
}

/*************************************************************//**
Starts the transaction if it is not yet started. */
void
trx_start_if_not_started_low(
/*==========================*/
	trx_t*	trx,		/*!< in: transaction */
	bool	read_write)	/*!< in: true if read write transaction */
{
	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		trx_start_low(trx, read_write);
		return;

	case TRX_STATE_ACTIVE:
		if (read_write && trx->id == 0 && !trx->read_only) {
			trx_set_rw_mode(trx);
		}
		return;

	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
	case TRX_STATE_COMMITTED_IN_MEMORY:
		break;
	}

	ut_error;
}

/*************************************************************//**
Starts a transaction for internal processing. */
void
trx_start_internal_low(
/*===================*/
	trx_t*	trx)		/*!< in/out: transaction */
{
	/* Ensure it is not flagged as an auto-commit-non-locking
	transaction. */

	trx->will_lock = 1;

	trx->internal = true;

	trx_start_low(trx, true);
}

/** Starts a read-only transaction for internal processing.
@param[in,out] trx	transaction to be started */
void
trx_start_internal_read_only_low(
	trx_t*	trx)
{
	/* Ensure it is not flagged as an auto-commit-non-locking
	transaction. */

	trx->will_lock = 1;

	trx->internal = true;

	trx_start_low(trx, false);
}

/*************************************************************//**
Starts the transaction for a DDL operation. */
void
trx_start_for_ddl_low(
/*==================*/
	trx_t*		trx,	/*!< in/out: transaction */
	trx_dict_op_t	op)	/*!< in: dictionary operation type */
{
	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		/* Flag this transaction as a dictionary operation, so that
		the data dictionary will be locked in crash recovery. */

		trx_set_dict_operation(trx, op);

		/* Ensure it is not flagged as an auto-commit-non-locking
		transation. */
		trx->will_lock = 1;

		trx->ddl= true;

		trx_start_internal_low(trx);
		return;

	case TRX_STATE_ACTIVE:
	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
	case TRX_STATE_COMMITTED_IN_MEMORY:
		break;
	}

	ut_error;
}

/*************************************************************//**
Set the transaction as a read-write transaction if it is not already
tagged as such. Read-only transactions that are writing to temporary
tables are assigned an ID and a rollback segment but are not added
to the trx read-write list because their updates should not be visible
to other transactions and therefore their changes can be ignored by
by MVCC. */
void
trx_set_rw_mode(
/*============*/
	trx_t*		trx)		/*!< in/out: transaction that is RW */
{
	ut_ad(trx->rsegs.m_redo.rseg == 0);
	ut_ad(!trx_is_autocommit_non_locking(trx));
	ut_ad(!trx->read_only);
	ut_ad(trx->id == 0);

	if (high_level_read_only) {
		return;
	}

	/* Function is promoting existing trx from ro mode to rw mode.
	In this process it has acquired trx_sys.mutex as it plan to
	move trx from ro list to rw list. If in future, some other thread
	looks at this trx object while it is being promoted then ensure
	that both threads are synced by acquring trx->mutex to avoid decision
	based on in-consistent view formed during promotion. */

	trx->rsegs.m_redo.rseg = trx_assign_rseg_low();
	ut_ad(trx->rsegs.m_redo.rseg != 0);

	trx_sys.register_rw(trx);

	/* So that we can see our own changes. */
	if (trx->read_view.is_open()) {
		trx->read_view.set_creator_trx_id(trx->id);
	}
}
