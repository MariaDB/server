/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2021, MariaDB Corporation.

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

#ifndef trx0sys_h
#define trx0sys_h

#include "buf0buf.h"
#include "fil0fil.h"
#include "trx0types.h"
#include "mem0mem.h"
#include "mtr0mtr.h"
#include "ut0byte.h"
#include "ut0lst.h"
#include "read0types.h"
#include "page0types.h"
#include "ut0mutex.h"
#include "trx0trx.h"
#ifdef WITH_WSREP
#include "trx0xa.h"
#endif /* WITH_WSREP */

typedef UT_LIST_BASE_NODE_T(trx_t) trx_ut_list_t;

/** Checks if a page address is the trx sys header page.
@param[in]	page_id	page id
@return true if trx sys header page */
inline bool trx_sys_hdr_page(const page_id_t& page_id)
{
	return(page_id.space() == TRX_SYS_SPACE
	       && page_id.page_no() == TRX_SYS_PAGE_NO);
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
  buf_block_t* block = buf_page_get(page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO),
				    0, rw ? RW_X_LATCH : RW_S_LATCH, mtr);
  ut_d(if (block) buf_block_dbg_add_level(block, SYNC_TRX_SYS_HEADER);)
  return block;
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
/*------------------------------------------------------------- @} */

/** The number of rollback segments; rollback segment id must fit in
the 7 bits reserved for it in DB_ROLL_PTR. */
#define	TRX_SYS_N_RSEGS			128
/** Maximum number of undo tablespaces (not counting the system tablespace) */
#define TRX_SYS_MAX_UNDO_SPACES		(TRX_SYS_N_RSEGS - 1)

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
				+ sys_header->frame);
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
			  sys_header->frame);
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

/** Size of the doublewrite block in pages */
#define TRX_SYS_DOUBLEWRITE_BLOCK_SIZE	FSP_EXTENT_SIZE
/* @} */

trx_t* current_trx();

struct rw_trx_hash_element_t
{
  rw_trx_hash_element_t(): trx(0)
  {
    mutex_create(LATCH_ID_RW_TRX_HASH_ELEMENT, &mutex);
  }


  ~rw_trx_hash_element_t()
  {
    mutex_free(&mutex);
  }


  trx_id_t id; /* lf_hash_init() relies on this to be first in the struct */
  Atomic_counter<trx_id_t> no;
  trx_t *trx;
  ib_mutex_t mutex;
};


/**
  Wrapper around LF_HASH to store set of in memory read-write transactions.
*/

class rw_trx_hash_t
{
  LF_HASH hash;


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


  struct eliminate_duplicates_arg
  {
    trx_ids_t ids;
    my_hash_walk_action action;
    void *argument;
    eliminate_duplicates_arg(size_t size, my_hash_walk_action act, void* arg):
      action(act), argument(arg) { ids.reserve(size); }
  };


  static my_bool eliminate_duplicates(rw_trx_hash_element_t *element,
                                      eliminate_duplicates_arg *arg)
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
    mutex_enter(&trx->mutex);
    ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE) ||
          trx_state_eq(trx, TRX_STATE_COMMITTED_IN_MEMORY) ||
          trx_state_eq(trx, TRX_STATE_PREPARED_RECOVERED) ||
          trx_state_eq(trx, TRX_STATE_PREPARED));
    mutex_exit(&trx->mutex);
  }


  struct debug_iterator_arg
  {
    my_hash_walk_action action;
    void *argument;
  };


  static my_bool debug_iterator(rw_trx_hash_element_t *element,
                                debug_iterator_arg *arg)
  {
    mutex_enter(&element->mutex);
    if (element->trx)
      validate_element(element->trx);
    mutex_exit(&element->mutex);
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
    only if lock_sys.mutex was acquired before calling find().

    With do_ref_count == true caller may dereference trx even if it is not
    holding lock_sys.mutex. Caller is responsible for calling
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
      mutex_enter(&element->mutex);
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
          trx_mutex_enter(trx);
          const trx_state_t state= trx->state;
          trx_mutex_exit(trx);
          if (state == TRX_STATE_COMMITTED_IN_MEMORY)
            trx= NULL;
          else
            trx->reference();
        }
      }
      mutex_exit(&element->mutex);
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
    mutex_enter(&trx->rw_trx_hash_element->mutex);
    trx->rw_trx_hash_element->trx= 0;
    mutex_exit(&trx->rw_trx_hash_element->mutex);
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

      mutex_enter(&element->mutex);
      if (trx_t trx= element->trx)
      {
        // trx is protected against commit in this branch
      }
      mutex_exit(&element->mutex);

    May miss concurrently inserted transactions.

    @return
      @retval 0 iteration completed successfully
      @retval 1 iteration was interrupted (action returned 1)
  */

  int iterate(trx_t *caller_trx, my_hash_walk_action action, void *argument)
  {
    LF_PINS *pins= caller_trx ? get_pins(caller_trx) : lf_hash_get_pins(&hash);
    ut_a(pins);
#ifdef UNIV_DEBUG
    debug_iterator_arg debug_arg= { action, argument };
    action= reinterpret_cast<my_hash_walk_action>(debug_iterator);
    argument= &debug_arg;
#endif
    int res= lf_hash_iterate(&hash, pins, action, argument);
    if (!caller_trx)
      lf_hash_put_pins(pins);
    return res;
  }


  int iterate(my_hash_walk_action action, void *argument)
  {
    return iterate(current_trx(), action, argument);
  }


  /**
    Iterates the hash and eliminates duplicate elements.

    @sa iterate()
  */

  int iterate_no_dups(trx_t *caller_trx, my_hash_walk_action action,
                      void *argument)
  {
    eliminate_duplicates_arg arg(size() + 32, action, argument);
    return iterate(caller_trx, reinterpret_cast<my_hash_walk_action>
                   (eliminate_duplicates), &arg);
  }


  int iterate_no_dups(my_hash_walk_action action, void *argument)
  {
    return iterate_no_dups(current_trx(), action, argument);
  }
};


/** The transaction system central memory data structure. */
class trx_sys_t
{
  /**
    The smallest number not yet assigned as a transaction id or transaction
    number. Accessed and updated with atomic operations.
  */
  MY_ALIGNED(CACHE_LINE_SIZE) Atomic_counter<trx_id_t> m_max_trx_id;


  /**
    Solves race conditions between register_rw() and snapshot_ids() as well as
    race condition between assign_new_trx_no() and snapshot_ids().

    @sa register_rw()
    @sa assign_new_trx_no()
    @sa snapshot_ids()
  */
  MY_ALIGNED(CACHE_LINE_SIZE) std::atomic<trx_id_t> m_rw_trx_hash_version;


  bool m_initialised;

public:
  /**
    TRX_RSEG_HISTORY list length (number of committed transactions to purge)
  */
  MY_ALIGNED(CACHE_LINE_SIZE) Atomic_counter<uint32_t> rseg_history_len;

  /** Mutex protecting trx_list. */
  MY_ALIGNED(CACHE_LINE_SIZE) mutable TrxSysMutex mutex;

  /** List of all transactions. */
  MY_ALIGNED(CACHE_LINE_SIZE) trx_ut_list_t trx_list;

	MY_ALIGNED(CACHE_LINE_SIZE)
	/** Temporary rollback segments */
	trx_rseg_t*	temp_rsegs[TRX_SYS_N_RSEGS];

	MY_ALIGNED(CACHE_LINE_SIZE)
	trx_rseg_t*	rseg_array[TRX_SYS_N_RSEGS];
					/*!< Pointer array to rollback
					segments; NULL if slot not in use;
					created and destroyed in
					single-threaded mode; not protected
					by any mutex, because it is read-only
					during multi-threaded operation */

  /**
    Lock-free hash of in memory read-write transactions.
    Works faster when it is on it's own cache line (tested).
  */

  MY_ALIGNED(CACHE_LINE_SIZE) rw_trx_hash_t rw_trx_hash;


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
    Returns the minimum trx id in rw trx list.

    This is the smallest id for which the trx can possibly be active. (But, you
    must look at the trx->state to find out if the minimum trx id transaction
    itself is active, or already committed.)

    @return the minimum trx id, or m_max_trx_id if the trx list is empty
  */

  trx_id_t get_min_trx_id()
  {
    trx_id_t id= get_max_trx_id();
    rw_trx_hash.iterate(reinterpret_cast<my_hash_walk_action>
                        (get_min_trx_id_callback), &id);
    return id;
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
    trx->no= get_new_trx_id_no_refresh();
    trx->rw_trx_hash_element->no= trx->no;
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
    @param[out]    mix_trx_no variable to store min(trx->no) value
  */

  void snapshot_ids(trx_t *caller_trx, trx_ids_t *ids, trx_id_t *max_trx_id,
                    trx_id_t *min_trx_no)
  {
    ut_ad(!mutex_own(&mutex));
    snapshot_ids_arg arg(ids);

    while ((arg.m_id= get_rw_trx_hash_version()) != get_max_trx_id())
      ut_delay(1);
    arg.m_no= arg.m_id;

    ids->clear();
    ids->reserve(rw_trx_hash.size() + 32);
    rw_trx_hash.iterate(caller_trx,
                        reinterpret_cast<my_hash_walk_action>(copy_one_id),
                        &arg);

    *max_trx_id= arg.m_id;
    *min_trx_no= arg.m_no;
  }


  /** Initialiser for m_max_trx_id and m_rw_trx_hash_version. */
  void init_max_trx_id(trx_id_t value)
  {
    m_max_trx_id= value;
    m_rw_trx_hash_version.store(value, std::memory_order_relaxed);
  }


  bool is_initialised() { return m_initialised; }


  /** Initialise the transaction subsystem. */
  void create();

  /** Close the transaction subsystem on shutdown. */
  void close();

  /** @return total number of active (non-prepared) transactions */
  ulint any_active_transactions();


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
    mutex_enter(&mutex);
    UT_LIST_ADD_FIRST(trx_list, trx);
    mutex_exit(&mutex);
  }


  /**
    Deregisters transaction in trx_sys.

    @param trx transaction
  */
  void deregister_trx(trx_t *trx)
  {
    mutex_enter(&mutex);
    UT_LIST_REMOVE(trx_list, trx);
    mutex_exit(&mutex);
  }


  /**
    Clones the oldest view and stores it in view.

    No need to call ReadView::close(). The caller owns the view that is passed
    in. This function is called by purge thread to determine whether it should
    purge the delete marked record or not.
  */
  void clone_oldest_view();


  /** @return the number of active views */
  size_t view_count() const
  {
    size_t count= 0;

    mutex_enter(&mutex);
    for (const trx_t *trx= UT_LIST_GET_FIRST(trx_list); trx;
         trx= UT_LIST_GET_NEXT(trx_list, trx))
    {
      if (trx->read_view.get_state() == READ_VIEW_STATE_OPEN)
        ++count;
    }
    mutex_exit(&mutex);
    return count;
  }

private:
  static my_bool get_min_trx_id_callback(rw_trx_hash_element_t *element,
                                         trx_id_t *id)
  {
    if (element->id < *id)
    {
      mutex_enter(&element->mutex);
      /* We don't care about read-only transactions here. */
      if (element->trx && element->trx->rsegs.m_redo.rseg)
        *id= element->id;
      mutex_exit(&element->mutex);
    }
    return 0;
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

#endif
