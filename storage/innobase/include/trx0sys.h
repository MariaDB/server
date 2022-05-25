/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/trx0sys.h
Transaction system

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#pragma once
#include "buf0buf.h"
#include "fil0fil.h"
#include "trx0rseg.h"
#include "mem0mem.h"
#include "mtr0mtr.h"
#include "ut0byte.h"
#include "ut0lst.h"
#include "read0types.h"
#include "page0types.h"
#include "trx0trx.h"
#include "ilist.h"
#include "my_cpu.h"

#ifdef UNIV_PFS_MUTEX
extern mysql_pfs_key_t trx_sys_mutex_key;
#endif

/** Checks if a page address is the trx sys header page.
@param[in]	page_id	page id
@return true if trx sys header page */
inline bool trx_sys_hdr_page(const page_id_t page_id)
{
  return page_id == page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO);
}

/*****************************************************************//**
Creates and initializes the transaction system at the database creation. */
void
trx_sys_create_sys_pages(void);
/*==========================*/
/** Find an available rollback segment.
@param[in]	sys_header
@return an unallocated rollback segment slot in the TRX_SYS header
@retval ULINT_UNDEFINED if not found */
ulint
trx_sys_rseg_find_free(const buf_block_t* sys_header);
/** Request the TRX_SYS page.
@param[in]	rw	whether to lock the page for writing
@return the TRX_SYS page
@retval	NULL	if the page cannot be read */
inline buf_block_t *trx_sysf_get(mtr_t* mtr, bool rw= true)
{
  return buf_page_get(page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO),
                      0, rw ? RW_X_LATCH : RW_S_LATCH, mtr);
}

#ifdef UNIV_DEBUG
/* Flag to control TRX_RSEG_N_SLOTS behavior debugging. */
extern uint			trx_rseg_n_slots_debug;
#endif

/** Write DB_TRX_ID.
@param[out]	db_trx_id	the DB_TRX_ID field to be written to
@param[in]	id		transaction ID */
UNIV_INLINE
void
trx_write_trx_id(byte* db_trx_id, trx_id_t id)
{
	compile_time_assert(DATA_TRX_ID_LEN == 6);
	mach_write_to_6(db_trx_id, id);
}

/** Read a transaction identifier.
@return id */
inline
trx_id_t
trx_read_trx_id(const byte* ptr)
{
	compile_time_assert(DATA_TRX_ID_LEN == 6);
	return(mach_read_from_6(ptr));
}

#ifdef UNIV_DEBUG
/** Check that the DB_TRX_ID in a record is valid.
@param[in]	db_trx_id	the DB_TRX_ID column to validate
@param[in]	trx_id		the id of the ALTER TABLE transaction */
inline bool trx_id_check(const void* db_trx_id, trx_id_t trx_id)
{
	trx_id_t id = trx_read_trx_id(static_cast<const byte*>(db_trx_id));
	ut_ad(id == 0 || id > trx_id);
	return true;
}
#endif

/*****************************************************************//**
Updates the offset information about the end of the MySQL binlog entry
which corresponds to the transaction just being committed. In a MySQL
replication slave updates the latest master binlog position up to which
replication has proceeded. */
void
trx_sys_update_mysql_binlog_offset(
/*===============================*/
	const char*	file_name,/*!< in: MySQL log file name */
	int64_t		offset,	/*!< in: position in that log file */
	buf_block_t*	sys_header, /*!< in,out: trx sys header */
	mtr_t*		mtr);	/*!< in,out: mini-transaction */
/** Display the MySQL binlog offset info if it is present in the trx
system header. */
void
trx_sys_print_mysql_binlog_offset();

/** Create the rollback segments.
@return	whether the creation succeeded */
bool
trx_sys_create_rsegs();

/** The automatically created system rollback segment has this id */
#define TRX_SYS_SYSTEM_RSEG_ID	0

/** The offset of the transaction system header on the page */
#define	TRX_SYS		FSEG_PAGE_DATA

/** Transaction system header */
/*------------------------------------------------------------- @{ */
/** In old versions of InnoDB, this persisted the value of
trx_sys.get_max_trx_id(). Starting with MariaDB 10.3.5,
the field TRX_RSEG_MAX_TRX_ID in rollback segment header pages
and the fields TRX_UNDO_TRX_ID, TRX_UNDO_TRX_NO in undo log pages
are used instead. The field only exists for the purpose of upgrading
from older MySQL or MariaDB versions. */
#define	TRX_SYS_TRX_ID_STORE	0
#define TRX_SYS_FSEG_HEADER	8	/*!< segment header for the
					tablespace segment the trx
					system is created into */
#define	TRX_SYS_RSEGS		(8 + FSEG_HEADER_SIZE)
					/*!< the start of the array of
					rollback segment specification
					slots */

/* Rollback segment specification slot offsets */

/** the tablespace ID of an undo log header; starting with
MySQL/InnoDB 5.1.7, this is FIL_NULL if the slot is unused */
#define	TRX_SYS_RSEG_SPACE	0
/** the page number of an undo log header, or FIL_NULL if unused */
#define	TRX_SYS_RSEG_PAGE_NO	4
/** Size of a rollback segment specification slot */
#define TRX_SYS_RSEG_SLOT_SIZE	8

/** Read the tablespace ID of a rollback segment slot.
@param[in]	sys_header	TRX_SYS page
@param[in]	rseg_id		rollback segment identifier
@return	undo tablespace id */
inline
uint32_t
trx_sysf_rseg_get_space(const buf_block_t* sys_header, ulint rseg_id)
{
	ut_ad(rseg_id < TRX_SYS_N_RSEGS);
	return mach_read_from_4(TRX_SYS + TRX_SYS_RSEGS + TRX_SYS_RSEG_SPACE
				+ rseg_id * TRX_SYS_RSEG_SLOT_SIZE
				+ sys_header->page.frame);
}

/** Read the page number of a rollback segment slot.
@param[in]	sys_header	TRX_SYS page
@param[in]	rseg_id		rollback segment identifier
@return	undo page number */
inline uint32_t
trx_sysf_rseg_get_page_no(const buf_block_t *sys_header, ulint rseg_id)
{
  ut_ad(rseg_id < TRX_SYS_N_RSEGS);
  return mach_read_from_4(TRX_SYS + TRX_SYS_RSEGS + TRX_SYS_RSEG_PAGE_NO +
			  rseg_id * TRX_SYS_RSEG_SLOT_SIZE +
			  sys_header->page.frame);
}

/** Maximum length of MySQL binlog file name, in bytes.
(Used before MariaDB 10.3.5.) */
#define TRX_SYS_MYSQL_LOG_NAME_LEN	512
/** Contents of TRX_SYS_MYSQL_LOG_MAGIC_N_FLD */
#define TRX_SYS_MYSQL_LOG_MAGIC_N	873422344

#if UNIV_PAGE_SIZE_MIN < 4096
# error "UNIV_PAGE_SIZE_MIN < 4096"
#endif
/** The offset of the MySQL binlog offset info in the trx system header */
#define TRX_SYS_MYSQL_LOG_INFO		(srv_page_size - 1000)
#define	TRX_SYS_MYSQL_LOG_MAGIC_N_FLD	0	/*!< magic number which is
						TRX_SYS_MYSQL_LOG_MAGIC_N
						if we have valid data in the
						MySQL binlog info */
#define TRX_SYS_MYSQL_LOG_OFFSET	4	/*!< the 64-bit offset
						within that file */
#define TRX_SYS_MYSQL_LOG_NAME		12	/*!< MySQL log file name */

/** Memory map TRX_SYS_PAGE_NO = 5 when srv_page_size = 4096

0...37 FIL_HEADER
38...45 TRX_SYS_TRX_ID_STORE
46...55 TRX_SYS_FSEG_HEADER (FSEG_HEADER_SIZE == 10)
56      TRX_SYS_RSEGS
  56...59  TRX_SYS_RSEG_SPACE       for slot 0
  60...63  TRX_SYS_RSEG_PAGE_NO     for slot 0
  64...67  TRX_SYS_RSEG_SPACE       for slot 1
  68...71  TRX_SYS_RSEG_PAGE_NO     for slot 1
....
 594..597  TRX_SYS_RSEG_SPACE       for slot 72
 598..601  TRX_SYS_RSEG_PAGE_NO     for slot 72
...
  ...1063  TRX_SYS_RSEG_PAGE_NO     for slot 126

(srv_page_size-3500 WSREP ::: FAIL would overwrite undo tablespace
space_id, page_no pairs :::)
596 TRX_SYS_WSREP_XID_INFO             TRX_SYS_WSREP_XID_MAGIC_N_FLD
600 TRX_SYS_WSREP_XID_FORMAT
604 TRX_SYS_WSREP_XID_GTRID_LEN
608 TRX_SYS_WSREP_XID_BQUAL_LEN
612 TRX_SYS_WSREP_XID_DATA   (len = 128)
739 TRX_SYS_WSREP_XID_DATA_END

FIXED WSREP XID info offsets for 4k page size 10.0.32-galera
(srv_page_size-2500)
1596 TRX_SYS_WSREP_XID_INFO             TRX_SYS_WSREP_XID_MAGIC_N_FLD
1600 TRX_SYS_WSREP_XID_FORMAT
1604 TRX_SYS_WSREP_XID_GTRID_LEN
1608 TRX_SYS_WSREP_XID_BQUAL_LEN
1612 TRX_SYS_WSREP_XID_DATA   (len = 128)
1739 TRX_SYS_WSREP_XID_DATA_END

(srv_page_size - 2000 MYSQL MASTER LOG)
2096   TRX_SYS_MYSQL_MASTER_LOG_INFO   TRX_SYS_MYSQL_LOG_MAGIC_N_FLD
2100   TRX_SYS_MYSQL_LOG_OFFSET_HIGH
2104   TRX_SYS_MYSQL_LOG_OFFSET_LOW
2108   TRX_SYS_MYSQL_LOG_NAME

(srv_page_size - 1000 MYSQL LOG)
3096   TRX_SYS_MYSQL_LOG_INFO          TRX_SYS_MYSQL_LOG_MAGIC_N_FLD
3100   TRX_SYS_MYSQL_LOG_OFFSET_HIGH
3104   TRX_SYS_MYSQL_LOG_OFFSET_LOW
3108   TRX_SYS_MYSQL_LOG_NAME

(srv_page_size - 200 DOUBLEWRITE)
3896   TRX_SYS_DOUBLEWRITE		TRX_SYS_DOUBLEWRITE_FSEG
3906         TRX_SYS_DOUBLEWRITE_MAGIC
3910         TRX_SYS_DOUBLEWRITE_BLOCK1
3914         TRX_SYS_DOUBLEWRITE_BLOCK2
3918         TRX_SYS_DOUBLEWRITE_REPEAT
3930         TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N

(srv_page_size - 8, TAILER)
4088..4096	FIL_TAILER

*/
#ifdef WITH_WSREP
/** The offset to WSREP XID headers (used before MariaDB 10.3.5) */
#define TRX_SYS_WSREP_XID_INFO std::max(srv_page_size - 3500, 1596UL)
#define TRX_SYS_WSREP_XID_MAGIC_N_FLD 0
#define TRX_SYS_WSREP_XID_MAGIC_N 0x77737265

/** XID field: formatID, gtrid_len, bqual_len, xid_data */
#define TRX_SYS_WSREP_XID_LEN        (4 + 4 + 4 + XIDDATASIZE)
#define TRX_SYS_WSREP_XID_FORMAT     4
#define TRX_SYS_WSREP_XID_GTRID_LEN  8
#define TRX_SYS_WSREP_XID_BQUAL_LEN 12
#define TRX_SYS_WSREP_XID_DATA      16
#endif /* WITH_WSREP*/

/** Doublewrite buffer */
/* @{ */
/** The offset of the doublewrite buffer header on the trx system header page */
#define TRX_SYS_DOUBLEWRITE		(srv_page_size - 200)
/*-------------------------------------------------------------*/
#define TRX_SYS_DOUBLEWRITE_FSEG	0	/*!< fseg header of the fseg
						containing the doublewrite
						buffer */
#define TRX_SYS_DOUBLEWRITE_MAGIC	FSEG_HEADER_SIZE
						/*!< 4-byte magic number which
						shows if we already have
						created the doublewrite
						buffer */
#define TRX_SYS_DOUBLEWRITE_BLOCK1	(4 + FSEG_HEADER_SIZE)
						/*!< page number of the
						first page in the first
						sequence of 64
						(= FSP_EXTENT_SIZE) consecutive
						pages in the doublewrite
						buffer */
#define TRX_SYS_DOUBLEWRITE_BLOCK2	(8 + FSEG_HEADER_SIZE)
						/*!< page number of the
						first page in the second
						sequence of 64 consecutive
						pages in the doublewrite
						buffer */
#define TRX_SYS_DOUBLEWRITE_REPEAT	12	/*!< we repeat
						TRX_SYS_DOUBLEWRITE_MAGIC,
						TRX_SYS_DOUBLEWRITE_BLOCK1,
						TRX_SYS_DOUBLEWRITE_BLOCK2
						so that if the trx sys
						header is half-written
						to disk, we still may
						be able to recover the
						information */
/** If this is not yet set to TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N,
we must reset the doublewrite buffer, because starting from 4.1.x the
space id of a data page is stored into
FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID. */
#define TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED (24 + FSEG_HEADER_SIZE)

/*-------------------------------------------------------------*/
/** Contents of TRX_SYS_DOUBLEWRITE_MAGIC */
constexpr uint32_t TRX_SYS_DOUBLEWRITE_MAGIC_N= 536853855;
/** Contents of TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED */
constexpr uint32_t TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N= 1783657386;
/* @} */

trx_t* current_trx();

struct rw_trx_hash_element_t
{
  rw_trx_hash_element_t()
  {
    memset(reinterpret_cast<void*>(this), 0, sizeof *this);
    mutex.init();
  }


  ~rw_trx_hash_element_t() { mutex.destroy(); }


  trx_id_t id; /* lf_hash_init() relies on this to be first in the struct */

  /**
    Transaction serialization number.

    Assigned shortly before the transaction is moved to COMMITTED_IN_MEMORY
    state. Initially set to TRX_ID_MAX.
  */
  Atomic_counter<trx_id_t> no;
  trx_t *trx;
  srw_mutex mutex;
};


/**
  Wrapper around LF_HASH to store set of in memory read-write transactions.
*/

class rw_trx_hash_t
{
  LF_HASH hash;


  template <typename T>
  using walk_action= my_bool(rw_trx_hash_element_t *element, T *action);


  /**
    Constructor callback for lock-free allocator.

    Object is just allocated and is not yet accessible via rw_trx_hash by
    concurrent threads. Object can be reused multiple times before it is freed.
    Every time object is being reused initializer() callback is called.
  */

  static void rw_trx_hash_constructor(uchar *arg)
  {
    new(arg + LF_HASH_OVERHEAD) rw_trx_hash_element_t();
  }


  /**
    Destructor callback for lock-free allocator.

    Object is about to be freed and is not accessible via rw_trx_hash by
    concurrent threads.
  */

  static void rw_trx_hash_destructor(uchar *arg)
  {
    reinterpret_cast<rw_trx_hash_element_t*>
      (arg + LF_HASH_OVERHEAD)->~rw_trx_hash_element_t();
  }


  /**
    Destructor callback for lock-free allocator.

    This destructor is used at shutdown. It frees remaining transaction
    objects.

    XA PREPARED transactions may remain if they haven't been committed or
    rolled back. ACTIVE transactions may remain if startup was interrupted or
    server is running in read-only mode or for certain srv_force_recovery
    levels.
  */

  static void rw_trx_hash_shutdown_destructor(uchar *arg)
  {
    rw_trx_hash_element_t *element=
      reinterpret_cast<rw_trx_hash_element_t*>(arg + LF_HASH_OVERHEAD);
    if (trx_t *trx= element->trx)
    {
      ut_ad(trx_state_eq(trx, TRX_STATE_PREPARED) ||
            trx_state_eq(trx, TRX_STATE_PREPARED_RECOVERED) ||
            (trx_state_eq(trx, TRX_STATE_ACTIVE) &&
             (!srv_was_started ||
              srv_read_only_mode ||
              srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO)));
      trx_free_at_shutdown(trx);
    }
    element->~rw_trx_hash_element_t();
  }


  /**
    Initializer callback for lock-free hash.

    Object is not yet accessible via rw_trx_hash by concurrent threads, but is
    about to become such. Object id can be changed only by this callback and
    remains the same until all pins to this object are released.

    Object trx can be changed to 0 by erase() under object mutex protection,
    which indicates it is about to be removed from lock-free hash and become
    not accessible by concurrent threads.
  */

  static void rw_trx_hash_initializer(LF_HASH *,
                                      rw_trx_hash_element_t *element,
                                      trx_t *trx)
  {
    ut_ad(element->trx == 0);
    element->trx= trx;
    element->id= trx->id;
    element->no= TRX_ID_MAX;
    trx->rw_trx_hash_element= element;
  }


  /**
    Gets LF_HASH pins.

    Pins are used to protect object from being destroyed or reused. They are
    normally stored in trx object for quick access. If caller doesn't have trx
    available, we try to get it using currnet_trx(). If caller doesn't have trx
    at all, temporary pins are allocated.
  */

  LF_PINS *get_pins(trx_t *trx)
  {
    if (!trx->rw_trx_hash_pins)
    {
      trx->rw_trx_hash_pins= lf_hash_get_pins(&hash);
      ut_a(trx->rw_trx_hash_pins);
    }
    return trx->rw_trx_hash_pins;
  }


  template <typename T> struct eliminate_duplicates_arg
  {
    trx_ids_t ids;
    walk_action<T> *action;
    T *argument;
    eliminate_duplicates_arg(size_t size, walk_action<T> *act, T *arg):
      action(act), argument(arg) { ids.reserve(size); }
  };


  template <typename T>
  static my_bool eliminate_duplicates(rw_trx_hash_element_t *element,
                                      eliminate_duplicates_arg<T> *arg)
  {
    for (trx_ids_t::iterator it= arg->ids.begin(); it != arg->ids.end(); it++)
    {
      if (*it == element->id)
        return 0;
    }
    arg->ids.push_back(element->id);
    return arg->action(element, arg->argument);
  }


#ifdef UNIV_DEBUG
  static void validate_element(trx_t *trx)
  {
    ut_ad(!trx->read_only || !trx->rsegs.m_redo.rseg);
    ut_ad(!trx->is_autocommit_non_locking());
    /* trx->state can be anything except TRX_STATE_NOT_STARTED */
    ut_d(trx->mutex_lock());
    ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE) ||
          trx_state_eq(trx, TRX_STATE_COMMITTED_IN_MEMORY) ||
          trx_state_eq(trx, TRX_STATE_PREPARED_RECOVERED) ||
          trx_state_eq(trx, TRX_STATE_PREPARED));
    ut_d(trx->mutex_unlock());
  }


  template <typename T> struct debug_iterator_arg
  {
    walk_action<T> *action;
    T *argument;
  };


  template <typename T>
  static my_bool debug_iterator(rw_trx_hash_element_t *element,
                                debug_iterator_arg<T> *arg)
  {
    element->mutex.wr_lock();
    if (element->trx)
      validate_element(element->trx);
    element->mutex.wr_unlock();
    return arg->action(element, arg->argument);
  }
#endif


public:
  void init()
  {
    lf_hash_init(&hash, sizeof(rw_trx_hash_element_t), LF_HASH_UNIQUE, 0,
                 sizeof(trx_id_t), 0, &my_charset_bin);
    hash.alloc.constructor= rw_trx_hash_constructor;
    hash.alloc.destructor= rw_trx_hash_destructor;
    hash.initializer=
      reinterpret_cast<lf_hash_initializer>(rw_trx_hash_initializer);
  }


  void destroy()
  {
    hash.alloc.destructor= rw_trx_hash_shutdown_destructor;
    lf_hash_destroy(&hash);
  }


  /**
    Releases LF_HASH pins.

    Must be called by thread that owns trx_t object when the latter is being
    "detached" from thread (e.g. released to the pool by trx_t::free()). Can be
    called earlier if thread is expected not to use rw_trx_hash.

    Since pins are not allowed to be transferred to another thread,
    initialisation thread calls this for recovered transactions.
  */

  void put_pins(trx_t *trx)
  {
    if (trx->rw_trx_hash_pins)
    {
      lf_hash_put_pins(trx->rw_trx_hash_pins);
      trx->rw_trx_hash_pins= 0;
    }
  }


  /**
    Finds trx object in lock-free hash with given id.

    Only ACTIVE or PREPARED trx objects may participate in hash. Nevertheless
    the transaction may get committed before this method returns.

    With do_ref_count == false the caller may dereference returned trx pointer
    only if lock_sys.latch was acquired before calling find().

    With do_ref_count == true caller may dereference trx even if it is not
    holding lock_sys.latch. Caller is responsible for calling
    trx->release_reference() when it is done playing with trx.

    Ideally this method should get caller rw_trx_hash_pins along with trx
    object as a parameter, similar to insert() and erase(). However most
    callers lose trx early in their call chains and it is not that easy to pass
    them through.

    So we take more expensive approach: get trx through current_thd()->ha_data.
    Some threads don't have trx attached to THD, and at least server
    initialisation thread, fts_optimize_thread, srv_master_thread,
    dict_stats_thread, srv_monitor_thread, btr_defragment_thread don't even
    have THD at all. For such cases we allocate pins only for duration of
    search and free them immediately.

    This has negative performance impact and should be fixed eventually (by
    passing caller_trx as a parameter). Still stream of DML is more or less Ok.

    @return
      @retval 0 not found
      @retval pointer to trx
  */

  trx_t *find(trx_t *caller_trx, trx_id_t trx_id, bool do_ref_count)
  {
    /*
      In MariaDB 10.3, purge will reset DB_TRX_ID to 0
      when the history is lost. Read/write transactions will
      always have a nonzero trx_t::id; there the value 0 is
      reserved for transactions that did not write or lock
      anything yet.

      The caller should already have handled trx_id==0 specially.
    */
    ut_ad(trx_id);
    ut_ad(!caller_trx || caller_trx->id != trx_id || !do_ref_count);

    trx_t *trx= 0;
    LF_PINS *pins= caller_trx ? get_pins(caller_trx) : lf_hash_get_pins(&hash);
    ut_a(pins);

    rw_trx_hash_element_t *element= reinterpret_cast<rw_trx_hash_element_t*>
      (lf_hash_search(&hash, pins, reinterpret_cast<const void*>(&trx_id),
                      sizeof(trx_id_t)));
    if (element)
    {
      element->mutex.wr_lock();
      lf_hash_search_unpin(pins);
      if ((trx= element->trx)) {
        DBUG_ASSERT(trx_id == trx->id);
        ut_d(validate_element(trx));
        if (do_ref_count)
        {
          /*
            We have an early state check here to avoid committer
            starvation in a wait loop for transaction references,
            when there's a stream of trx_sys.find() calls from other
            threads. The trx->state may change to COMMITTED after
            trx->mutex is released, and it will have to be rechecked
            by the caller after reacquiring the mutex.
          */
          if (trx->state == TRX_STATE_COMMITTED_IN_MEMORY)
            trx= nullptr;
          else
            trx->reference();
        }
      }
      element->mutex.wr_unlock();
    }
    if (!caller_trx)
      lf_hash_put_pins(pins);
    return trx;
  }


  /**
    Inserts trx to lock-free hash.

    Object becomes accessible via rw_trx_hash.
  */

  void insert(trx_t *trx)
  {
    ut_d(validate_element(trx));
    int res= lf_hash_insert(&hash, get_pins(trx),
                            reinterpret_cast<void*>(trx));
    ut_a(res == 0);
  }


  /**
    Removes trx from lock-free hash.

    Object becomes not accessible via rw_trx_hash. But it still can be pinned
    by concurrent find(), which is supposed to release it immediately after
    it sees object trx is 0.
  */

  void erase(trx_t *trx)
  {
    ut_d(validate_element(trx));
    trx->rw_trx_hash_element->mutex.wr_lock();
    trx->rw_trx_hash_element->trx= nullptr;
    trx->rw_trx_hash_element->mutex.wr_unlock();
    int res= lf_hash_delete(&hash, get_pins(trx),
                            reinterpret_cast<const void*>(&trx->id),
                            sizeof(trx_id_t));
    ut_a(res == 0);
  }


  /**
    Returns the number of elements in the hash.

    The number is exact only if hash is protected against concurrent
    modifications (e.g. single threaded startup or hash is protected
    by some mutex). Otherwise the number may be used as a hint only,
    because it may change even before this method returns.
  */

  uint32_t size() { return uint32_t(lf_hash_size(&hash)); }


  /**
    Iterates the hash.

    @param caller_trx  used to get/set pins
    @param action      called for every element in hash
    @param argument    opque argument passed to action

    May return the same element multiple times if hash is under contention.
    If caller doesn't like to see the same transaction multiple times, it has
    to call iterate_no_dups() instead.

    May return element with committed transaction. If caller doesn't like to
    see committed transactions, it has to skip those under element mutex:

      element->mutex.wr_lock();
      if (trx_t trx= element->trx)
      {
        // trx is protected against commit in this branch
      }
      element->mutex.wr_unlock();

    May miss concurrently inserted transactions.

    @return
      @retval 0 iteration completed successfully
      @retval 1 iteration was interrupted (action returned 1)
  */

  template <typename T>
  int iterate(trx_t *caller_trx, walk_action<T> *action, T *argument= nullptr)
  {
    LF_PINS *pins= caller_trx ? get_pins(caller_trx) : lf_hash_get_pins(&hash);
    ut_a(pins);
#ifdef UNIV_DEBUG
    debug_iterator_arg<T> debug_arg= { action, argument };
    action= reinterpret_cast<decltype(action)>(debug_iterator<T>);
    argument= reinterpret_cast<T*>(&debug_arg);
#endif
    int res= lf_hash_iterate(&hash, pins,
                             reinterpret_cast<my_hash_walk_action>(action),
                             const_cast<void*>(static_cast<const void*>
                             (argument)));
    if (!caller_trx)
      lf_hash_put_pins(pins);
    return res;
  }


  template <typename T>
  int iterate(walk_action<T> *action, T *argument= nullptr)
  {
    return iterate(current_trx(), action, argument);
  }


  /**
    Iterates the hash and eliminates duplicate elements.

    @sa iterate()
  */

  template <typename T>
  int iterate_no_dups(trx_t *caller_trx, walk_action<T> *action,
                      T *argument= nullptr)
  {
    eliminate_duplicates_arg<T> arg(size() + 32, action, argument);
    return iterate(caller_trx, eliminate_duplicates<T>, &arg);
  }


  template <typename T>
  int iterate_no_dups(walk_action<T> *action, T *argument= nullptr)
  {
    return iterate_no_dups(current_trx(), action, argument);
  }
};

class thread_safe_trx_ilist_t
{
public:
  void create() { mysql_mutex_init(trx_sys_mutex_key, &mutex, nullptr); }
  void close() { mysql_mutex_destroy(&mutex); }

  bool empty() const
  {
    mysql_mutex_lock(&mutex);
    auto result= trx_list.empty();
    mysql_mutex_unlock(&mutex);
    return result;
  }

  void push_front(trx_t &trx)
  {
    mysql_mutex_lock(&mutex);
    trx_list.push_front(trx);
    mysql_mutex_unlock(&mutex);
  }

  void remove(trx_t &trx)
  {
    mysql_mutex_lock(&mutex);
    trx_list.remove(trx);
    mysql_mutex_unlock(&mutex);
  }

  template <typename Callable> void for_each(Callable &&callback) const
  {
    mysql_mutex_lock(&mutex);
    for (const auto &trx : trx_list)
      callback(trx);
    mysql_mutex_unlock(&mutex);
  }

  template <typename Callable> void for_each(Callable &&callback)
  {
    mysql_mutex_lock(&mutex);
    for (auto &trx : trx_list)
      callback(trx);
    mysql_mutex_unlock(&mutex);
  }

  void freeze() const { mysql_mutex_lock(&mutex); }
  void unfreeze() const { mysql_mutex_unlock(&mutex); }

private:
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) mutable mysql_mutex_t mutex;
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) ilist<trx_t> trx_list;
};

/** The transaction system central memory data structure. */
class trx_sys_t
{
  /**
    The smallest number not yet assigned as a transaction id or transaction
    number. Accessed and updated with atomic operations.
  */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) Atomic_counter<trx_id_t> m_max_trx_id;


  /**
    Solves race conditions between register_rw() and snapshot_ids() as well as
    race condition between assign_new_trx_no() and snapshot_ids().

    @sa register_rw()
    @sa assign_new_trx_no()
    @sa snapshot_ids()
  */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE)
  std::atomic<trx_id_t> m_rw_trx_hash_version;


  bool m_initialised;

public:
  /** List of all transactions. */
  thread_safe_trx_ilist_t trx_list;

  /** Temporary rollback segments */
  trx_rseg_t temp_rsegs[TRX_SYS_N_RSEGS];

  /** Persistent rollback segments; space==nullptr if slot not in use */
  trx_rseg_t rseg_array[TRX_SYS_N_RSEGS];

  /**
    Lock-free hash of in memory read-write transactions.
    Works faster when it is on it's own cache line (tested).
  */

  alignas(CPU_LEVEL1_DCACHE_LINESIZE) rw_trx_hash_t rw_trx_hash;


#ifdef WITH_WSREP
  /** Latest recovered XID during startup */
  XID recovered_wsrep_xid;
#endif
  /** Latest recovered binlog offset */
  uint64_t recovered_binlog_offset;
  /** Latest recovered binlog file name */
  char recovered_binlog_filename[TRX_SYS_MYSQL_LOG_NAME_LEN];
  /** FIL_PAGE_LSN of the page with the latest recovered binlog metadata */
  lsn_t recovered_binlog_lsn;


  /**
    Constructor.

    Some members may require late initialisation, thus we just mark object as
    uninitialised. Real initialisation happens in create().
  */

  trx_sys_t(): m_initialised(false) {}


  /**
    @return TRX_RSEG_HISTORY length (number of committed transactions to purge)
  */
  uint32_t history_size();


  /**
    Check whether history_size() exceeds a specified number.
    @param threshold   number of committed transactions
    @return whether TRX_RSEG_HISTORY length exceeds the threshold
  */
  bool history_exceeds(uint32_t threshold);


  /**
    @return approximate history_size(), without latch protection
  */
  TPOOL_SUPPRESS_TSAN uint32_t history_size_approx() const;


  /**
    @return whether history_size() is nonzero (with some race condition)
  */
  TPOOL_SUPPRESS_TSAN bool history_exists();


  /**
    Determine if the specified transaction or any older one might be active.

    @param caller_trx  used to get/set pins
    @param id          transaction identifier
    @return whether any transaction not newer than id might be active
  */

  bool find_same_or_older(trx_t *caller_trx, trx_id_t id)
  {
    return rw_trx_hash.iterate(caller_trx, find_same_or_older_callback, &id);
  }


  /**
    Determines the maximum transaction id.

    @return maximum currently allocated trx id; will be stale after the
            next call to trx_sys.get_new_trx_id()
  */

  trx_id_t get_max_trx_id()
  {
    return m_max_trx_id;
  }


  /**
    Allocates a new transaction id.
    @return new, allocated trx id
  */

  trx_id_t get_new_trx_id()
  {
    trx_id_t id= get_new_trx_id_no_refresh();
    refresh_rw_trx_hash_version();
    return id;
  }


  /**
    Allocates and assigns new transaction serialisation number.

    There's a gap between m_max_trx_id increment and transaction serialisation
    number becoming visible through rw_trx_hash. While we're in this gap
    concurrent thread may come and do MVCC snapshot without seeing allocated
    but not yet assigned serialisation number. Then at some point purge thread
    may clone this view. As a result it won't see newly allocated serialisation
    number and may remove "unnecessary" history data of this transaction from
    rollback segments.

    m_rw_trx_hash_version is intended to solve this problem. MVCC snapshot has
    to wait until m_max_trx_id == m_rw_trx_hash_version, which effectively
    means that all transaction serialisation numbers up to m_max_trx_id are
    available through rw_trx_hash.

    We rely on refresh_rw_trx_hash_version() to issue RELEASE memory barrier so
    that m_rw_trx_hash_version increment happens after
    trx->rw_trx_hash_element->no becomes visible through rw_trx_hash.

    @param trx transaction
  */
  void assign_new_trx_no(trx_t *trx)
  {
    trx->rw_trx_hash_element->no= get_new_trx_id_no_refresh();
    refresh_rw_trx_hash_version();
  }


  /**
    Takes MVCC snapshot.

    To reduce malloc probablility we reserve rw_trx_hash.size() + 32 elements
    in ids.

    For details about get_rw_trx_hash_version() != get_max_trx_id() spin
    @sa register_rw() and @sa assign_new_trx_no().

    We rely on get_rw_trx_hash_version() to issue ACQUIRE memory barrier so
    that loading of m_rw_trx_hash_version happens before accessing rw_trx_hash.

    To optimise snapshot creation rw_trx_hash.iterate() is being used instead
    of rw_trx_hash.iterate_no_dups(). It means that some transaction
    identifiers may appear multiple times in ids.

    @param[in,out] caller_trx used to get access to rw_trx_hash_pins
    @param[out]    ids        array to store registered transaction identifiers
    @param[out]    max_trx_id variable to store m_max_trx_id value
    @param[out]    mix_trx_no variable to store min(no) value
  */

  void snapshot_ids(trx_t *caller_trx, trx_ids_t *ids, trx_id_t *max_trx_id,
                    trx_id_t *min_trx_no)
  {
    snapshot_ids_arg arg(ids);

    while ((arg.m_id= get_rw_trx_hash_version()) != get_max_trx_id())
      ut_delay(1);
    arg.m_no= arg.m_id;

    ids->clear();
    ids->reserve(rw_trx_hash.size() + 32);
    rw_trx_hash.iterate(caller_trx, copy_one_id, &arg);

    *max_trx_id= arg.m_id;
    *min_trx_no= arg.m_no;
  }


  /** Initialiser for m_max_trx_id and m_rw_trx_hash_version. */
  void init_max_trx_id(trx_id_t value)
  {
    m_max_trx_id= value;
    m_rw_trx_hash_version.store(value, std::memory_order_relaxed);
  }


  bool is_initialised() const { return m_initialised; }


  /** Initialise the transaction subsystem. */
  void create();

  /** Close the transaction subsystem on shutdown. */
  void close();

  /** @return total number of active (non-prepared) transactions */
  ulint any_active_transactions();


  /**
    Determine the rollback segment identifier.

    @param rseg        rollback segment
    @param persistent  whether the rollback segment is persistent
    @return the rollback segment identifier
  */
  unsigned rseg_id(const trx_rseg_t *rseg, bool persistent) const
  {
    const trx_rseg_t *array= persistent ? rseg_array : temp_rsegs;
    ut_ad(rseg >= array);
    ut_ad(rseg < &array[TRX_SYS_N_RSEGS]);
    return static_cast<unsigned>(rseg - array);
  }


  /**
    Registers read-write transaction.

    Transaction becomes visible to MVCC.

    There's a gap between m_max_trx_id increment and transaction becoming
    visible through rw_trx_hash. While we're in this gap concurrent thread may
    come and do MVCC snapshot. As a result concurrent read view will be able to
    observe records owned by this transaction even before it was committed.

    m_rw_trx_hash_version is intended to solve this problem. MVCC snapshot has
    to wait until m_max_trx_id == m_rw_trx_hash_version, which effectively
    means that all transactions up to m_max_trx_id are available through
    rw_trx_hash.

    We rely on refresh_rw_trx_hash_version() to issue RELEASE memory barrier so
    that m_rw_trx_hash_version increment happens after transaction becomes
    visible through rw_trx_hash.
  */

  void register_rw(trx_t *trx)
  {
    trx->id= get_new_trx_id_no_refresh();
    rw_trx_hash.insert(trx);
    refresh_rw_trx_hash_version();
  }


  /**
    Deregisters read-write transaction.

    Transaction is removed from rw_trx_hash, which releases all implicit locks.
    MVCC snapshot won't see this transaction anymore.
  */

  void deregister_rw(trx_t *trx)
  {
    rw_trx_hash.erase(trx);
  }


  bool is_registered(trx_t *caller_trx, trx_id_t id)
  {
    return id && find(caller_trx, id, false);
  }


  trx_t *find(trx_t *caller_trx, trx_id_t id, bool do_ref_count= true)
  {
    return rw_trx_hash.find(caller_trx, id, do_ref_count);
  }


  /**
    Registers transaction in trx_sys.

    @param trx transaction
  */
  void register_trx(trx_t *trx)
  {
    trx_list.push_front(*trx);
  }


  /**
    Deregisters transaction in trx_sys.

    @param trx transaction
  */
  void deregister_trx(trx_t *trx)
  {
    trx_list.remove(*trx);
  }


  /**
    Clones the oldest view and stores it in view.

    No need to call ReadView::close(). The caller owns the view that is passed
    in. This function is called by purge thread to determine whether it should
    purge the delete marked record or not.
  */
  void clone_oldest_view(ReadViewBase *view) const;


  /** @return the number of active views */
  size_t view_count() const
  {
    size_t count= 0;

    trx_list.for_each([&count](const trx_t &trx) {
      if (trx.read_view.is_open())
        ++count;
    });

    return count;
  }

private:
  static my_bool find_same_or_older_callback(rw_trx_hash_element_t *element,
                                             trx_id_t *id)
  {
    return element->id <= *id;
  }


  struct snapshot_ids_arg
  {
    snapshot_ids_arg(trx_ids_t *ids): m_ids(ids) {}
    trx_ids_t *m_ids;
    trx_id_t m_id;
    trx_id_t m_no;
  };


  static my_bool copy_one_id(rw_trx_hash_element_t *element,
                             snapshot_ids_arg *arg)
  {
    if (element->id < arg->m_id)
    {
      trx_id_t no= element->no;
      arg->m_ids->push_back(element->id);
      if (no < arg->m_no)
        arg->m_no= no;
    }
    return 0;
  }


  /** Getter for m_rw_trx_hash_version, must issue ACQUIRE memory barrier. */
  trx_id_t get_rw_trx_hash_version()
  {
    return m_rw_trx_hash_version.load(std::memory_order_acquire);
  }


  /** Increments m_rw_trx_hash_version, must issue RELEASE memory barrier. */
  void refresh_rw_trx_hash_version()
  {
    m_rw_trx_hash_version.fetch_add(1, std::memory_order_release);
  }


  /**
    Allocates new transaction id without refreshing rw_trx_hash version.

    This method is extracted for exclusive use by register_rw() and
    assign_new_trx_no() where new id must be allocated atomically with
    payload of these methods from MVCC snapshot point of view.

    @sa get_new_trx_id()
    @sa assign_new_trx_no()

    @return new transaction id
  */

  trx_id_t get_new_trx_id_no_refresh()
  {
    return m_max_trx_id++;
  }
};


/** The transaction system */
extern trx_sys_t trx_sys;
