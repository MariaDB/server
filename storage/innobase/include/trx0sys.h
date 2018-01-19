/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/trx0sys.h
Transaction system

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#ifndef trx0sys_h
#define trx0sys_h

#include "univ.i"

#include "buf0buf.h"
#include "fil0fil.h"
#include "trx0types.h"
#include "mem0mem.h"
#include "mtr0mtr.h"
#include "ut0byte.h"
#include "mem0mem.h"
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
UNIV_INLINE
bool
trx_sys_hdr_page(
	const page_id_t&	page_id);

/** Initialize the transaction system main-memory data structures. */
void trx_sys_init_at_db_start();

/*****************************************************************//**
Creates and initializes the transaction system at the database creation. */
void
trx_sys_create_sys_pages(void);
/*==========================*/
/** @return an unallocated rollback segment slot in the TRX_SYS header
@retval ULINT_UNDEFINED if not found */
ulint
trx_sysf_rseg_find_free(mtr_t* mtr);
/**********************************************************************//**
Gets a pointer to the transaction system file copy and x-locks its page.
@return pointer to system file copy, page x-locked */
UNIV_INLINE
trx_sysf_t*
trx_sysf_get(
/*=========*/
	mtr_t*	mtr);	/*!< in: mtr */
/*****************************************************************//**
Gets the space of the nth rollback segment slot in the trx system
file copy.
@return space id */
UNIV_INLINE
ulint
trx_sysf_rseg_get_space(
/*====================*/
	trx_sysf_t*	sys_header,	/*!< in: trx sys file copy */
	ulint		i,		/*!< in: slot index == rseg id */
	mtr_t*		mtr);		/*!< in: mtr */
/*****************************************************************//**
Gets the page number of the nth rollback segment slot in the trx system
file copy.
@return page number, FIL_NULL if slot unused */
UNIV_INLINE
ulint
trx_sysf_rseg_get_page_no(
/*======================*/
	trx_sysf_t*	sys_header,	/*!< in: trx sys file copy */
	ulint		i,		/*!< in: slot index == rseg id */
	mtr_t*		mtr);		/*!< in: mtr */
/*****************************************************************//**
Sets the space id of the nth rollback segment slot in the trx system
file copy. */
UNIV_INLINE
void
trx_sysf_rseg_set_space(
/*====================*/
	trx_sysf_t*	sys_header,	/*!< in: trx sys file copy */
	ulint		i,		/*!< in: slot index == rseg id */
	ulint		space,		/*!< in: space id */
	mtr_t*		mtr);		/*!< in: mtr */
/*****************************************************************//**
Sets the page number of the nth rollback segment slot in the trx system
file copy. */
UNIV_INLINE
void
trx_sysf_rseg_set_page_no(
/*======================*/
	trx_sysf_t*	sys_header,	/*!< in: trx sys file copy */
	ulint		i,		/*!< in: slot index == rseg id */
	ulint		page_no,	/*!< in: page number, FIL_NULL if
					the slot is reset to unused */
	mtr_t*		mtr);		/*!< in: mtr */

#ifdef UNIV_DEBUG
/* Flag to control TRX_RSEG_N_SLOTS behavior debugging. */
extern uint			trx_rseg_n_slots_debug;
#endif

/*****************************************************************//**
Writes a trx id to an index page. In case that the id size changes in
some future version, this function should be used instead of
mach_write_... */
UNIV_INLINE
void
trx_write_trx_id(
/*=============*/
	byte*		ptr,	/*!< in: pointer to memory where written */
	trx_id_t	id);	/*!< in: id */

/** Read a transaction identifier.
@return id */
inline
trx_id_t
trx_read_trx_id(const byte* ptr)
{
#if DATA_TRX_ID_LEN != 6
# error "DATA_TRX_ID_LEN != 6"
#endif
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
        trx_sysf_t*     sys_header, /*!< in: trx sys header */
	mtr_t*		mtr);	/*!< in: mtr */
/** Display the MySQL binlog offset info if it is present in the trx
system header. */
void
trx_sys_print_mysql_binlog_offset();
#ifdef WITH_WSREP

/** Update WSREP XID info in sys_header of TRX_SYS_PAGE_NO = 5.
@param[in]	xid		Transaction XID
@param[in,out]	sys_header	sys_header
@param[in]	mtr		minitransaction */
UNIV_INTERN
void
trx_sys_update_wsrep_checkpoint(
	const XID*	xid,
	trx_sysf_t*	sys_header,
	mtr_t*		mtr);

/** Read WSREP checkpoint XID from sys header.
@param[out]	xid	WSREP XID
@return	whether the checkpoint was present */
UNIV_INTERN
bool
trx_sys_read_wsrep_checkpoint(XID* xid);
#endif /* WITH_WSREP */

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
#define	TRX_SYS_TRX_ID_STORE	0	/*!< the maximum trx id or trx
					number modulo
					TRX_SYS_TRX_ID_UPDATE_MARGIN
					written to a file page by any
					transaction; the assignment of
					transaction ids continues from
					this number rounded up by
					TRX_SYS_TRX_ID_UPDATE_MARGIN
					plus
					TRX_SYS_TRX_ID_UPDATE_MARGIN
					when the database is
					started */
#define TRX_SYS_FSEG_HEADER	8	/*!< segment header for the
					tablespace segment the trx
					system is created into */
#define	TRX_SYS_RSEGS		(8 + FSEG_HEADER_SIZE)
					/*!< the start of the array of
					rollback segment specification
					slots */
/*------------------------------------------------------------- @} */

/* Max number of rollback segments: the number of segment specification slots
in the transaction system array; rollback segment id must fit in one (signed)
byte, therefore 128; each slot is currently 8 bytes in size. If you want
to raise the level to 256 then you will need to fix some assertions that
impose the 7 bit restriction. e.g., mach_write_to_3() */
#define	TRX_SYS_N_RSEGS			128
/** Maximum number of undo tablespaces (not counting the system tablespace) */
#define TRX_SYS_MAX_UNDO_SPACES		(TRX_SYS_N_RSEGS - 1)

/** Maximum length of MySQL binlog file name, in bytes. */
#define TRX_SYS_MYSQL_LOG_NAME_LEN	512
/** Contents of TRX_SYS_MYSQL_LOG_MAGIC_N_FLD */
#define TRX_SYS_MYSQL_LOG_MAGIC_N	873422344

#if UNIV_PAGE_SIZE_MIN < 4096
# error "UNIV_PAGE_SIZE_MIN < 4096"
#endif
/** The offset of the MySQL binlog offset info in the trx system header */
#define TRX_SYS_MYSQL_LOG_INFO		(UNIV_PAGE_SIZE - 1000)
#define	TRX_SYS_MYSQL_LOG_MAGIC_N_FLD	0	/*!< magic number which is
						TRX_SYS_MYSQL_LOG_MAGIC_N
						if we have valid data in the
						MySQL binlog info */
#define TRX_SYS_MYSQL_LOG_OFFSET	4	/*!< the 64-bit offset
						within that file */
#define TRX_SYS_MYSQL_LOG_NAME		12	/*!< MySQL log file name */

/** Memory map TRX_SYS_PAGE_NO = 5 when UNIV_PAGE_SIZE = 4096

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

(UNIV_PAGE_SIZE-3500 WSREP ::: FAIL would overwrite undo tablespace
space_id, page_no pairs :::)
596 TRX_SYS_WSREP_XID_INFO             TRX_SYS_WSREP_XID_MAGIC_N_FLD
600 TRX_SYS_WSREP_XID_FORMAT
604 TRX_SYS_WSREP_XID_GTRID_LEN
608 TRX_SYS_WSREP_XID_BQUAL_LEN
612 TRX_SYS_WSREP_XID_DATA   (len = 128)
739 TRX_SYS_WSREP_XID_DATA_END

FIXED WSREP XID info offsets for 4k page size 10.0.32-galera
(UNIV_PAGE_SIZE-2500)
1596 TRX_SYS_WSREP_XID_INFO             TRX_SYS_WSREP_XID_MAGIC_N_FLD
1600 TRX_SYS_WSREP_XID_FORMAT
1604 TRX_SYS_WSREP_XID_GTRID_LEN
1608 TRX_SYS_WSREP_XID_BQUAL_LEN
1612 TRX_SYS_WSREP_XID_DATA   (len = 128)
1739 TRX_SYS_WSREP_XID_DATA_END

(UNIV_PAGE_SIZE - 2000 MYSQL MASTER LOG)
2096   TRX_SYS_MYSQL_MASTER_LOG_INFO   TRX_SYS_MYSQL_LOG_MAGIC_N_FLD
2100   TRX_SYS_MYSQL_LOG_OFFSET_HIGH
2104   TRX_SYS_MYSQL_LOG_OFFSET_LOW
2108   TRX_SYS_MYSQL_LOG_NAME

(UNIV_PAGE_SIZE - 1000 MYSQL LOG)
3096   TRX_SYS_MYSQL_LOG_INFO          TRX_SYS_MYSQL_LOG_MAGIC_N_FLD
3100   TRX_SYS_MYSQL_LOG_OFFSET_HIGH
3104   TRX_SYS_MYSQL_LOG_OFFSET_LOW
3108   TRX_SYS_MYSQL_LOG_NAME

(UNIV_PAGE_SIZE - 200 DOUBLEWRITE)
3896   TRX_SYS_DOUBLEWRITE		TRX_SYS_DOUBLEWRITE_FSEG
3906         TRX_SYS_DOUBLEWRITE_MAGIC
3910         TRX_SYS_DOUBLEWRITE_BLOCK1
3914         TRX_SYS_DOUBLEWRITE_BLOCK2
3918         TRX_SYS_DOUBLEWRITE_REPEAT
3930         TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N

(UNIV_PAGE_SIZE - 8, TAILER)
4088..4096	FIL_TAILER

*/
#ifdef WITH_WSREP
/** The offset to WSREP XID headers */
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
#define TRX_SYS_DOUBLEWRITE		(UNIV_PAGE_SIZE - 200)
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
#define TRX_SYS_DOUBLEWRITE_MAGIC_N	536853855
/** Contents of TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED */
#define TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N 1783657386

/** Size of the doublewrite block in pages */
#define TRX_SYS_DOUBLEWRITE_BLOCK_SIZE	FSP_EXTENT_SIZE

/** When a trx id which is zero modulo this number (which must be a power of
two) is assigned, the field TRX_SYS_TRX_ID_STORE on the transaction system
page is updated */
#define TRX_SYS_TRX_ID_WRITE_MARGIN	((trx_id_t) 256)
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
    ut_ad(!trx_is_autocommit_non_locking(trx));
    mutex_enter(&trx->mutex);
    ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE) ||
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
    "detached" from thread (e.g. released to the pool by trx_free()). Can be
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
    only if lock_sys->mutex was acquired before calling find().

    With do_ref_count == true caller may dereference trx even if it is not
    holding lock_sys->mutex. Caller is responsible for calling
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

  trx_t *find(trx_t *caller_trx, trx_id_t trx_id, bool do_ref_count= false)
  {
    /*
      In MariaDB 10.3, purge will reset DB_TRX_ID to 0
      when the history is lost. Read/write transactions will
      always have a nonzero trx_t::id; there the value 0 is
      reserved for transactions that did not write or lock
      anything yet.
    */
    if (!trx_id)
      return NULL;

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
      if ((trx= element->trx))
      {
        if (do_ref_count)
          trx->reference();
        ut_d(validate_element(trx));
      }
      mutex_exit(&element->mutex);
    }
    if (!caller_trx)
      lf_hash_put_pins(pins);
    return trx;
  }


  trx_t *find(trx_id_t trx_id, bool do_ref_count= false)
  {
    return find(current_trx(), trx_id, do_ref_count);
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

  int32_t size()
  {
    return my_atomic_load32_explicit(&hash.count, MY_MEMORY_ORDER_RELAXED);
  }


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
struct trx_sys_t {
private:
  /**
    The smallest number not yet assigned as a transaction id or transaction
    number. Accessed and updated with atomic operations.
  */

  MY_ALIGNED(CACHE_LINE_SIZE) trx_id_t m_max_trx_id;

  bool m_initialised;

public:
	MY_ALIGNED(CACHE_LINE_SIZE)
	TrxSysMutex	mutex;		/*!< mutex protecting most fields in
					this structure except when noted
					otherwise */

	MY_ALIGNED(CACHE_LINE_SIZE)
	MVCC		mvcc;		/*!< Multi version concurrency control
					manager */
	trx_ut_list_t	serialisation_list;
					/*!< Ordered on trx_t::no of all the
					currenrtly active RW transactions */

	MY_ALIGNED(CACHE_LINE_SIZE)
	trx_ut_list_t	mysql_trx_list;	/*!< List of transactions created
					for MySQL. All user transactions are
					on mysql_trx_list. The rw_trx_hash
					can contain system transactions and
					recovered transactions that will not
					be in the mysql_trx_list.
					mysql_trx_list may additionally contain
					transactions that have not yet been
					started in InnoDB. */

	MY_ALIGNED(CACHE_LINE_SIZE)
	trx_ids_t	rw_trx_ids;	/*!< Array of Read write transaction IDs
					for MVCC snapshot. A ReadView would take
					a snapshot of these transactions whose
					changes are not visible to it. We should
					remove transactions from the list before
					committing in memory and releasing locks
					to ensure right order of removal and
					consistent snapshot. */

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
	ulint		rseg_history_len;
					/*!< Length of the TRX_RSEG_HISTORY
					list (update undo logs for committed
					transactions), protected by
					rseg->mutex */


  /**
    Lock-free hash of in memory read-write transactions.
    Works faster when it is on it's own cache line (tested).
  */

  MY_ALIGNED(CACHE_LINE_SIZE) rw_trx_hash_t rw_trx_hash;

	ulint		n_prepared_trx;	/*!< Number of transactions currently
					in the XA PREPARED state */

	ulint		n_prepared_recovered_trx; /*!< Number of transactions
					currently in XA PREPARED state that are
					also recovered. Such transactions cannot
					be added during runtime. They can only
					occur after recovery if mysqld crashed
					while there were XA PREPARED
					transactions. We disable query cache
					if such transactions exist. */

  /**
    Constructor.

    We only initialise rw_trx_ids here as it is impossible to postpone it's
    initialisation to create().
  */

  trx_sys_t(): m_initialised(false),
    rw_trx_ids(ut_allocator<trx_id_t>(mem_key_trx_sys_t_rw_trx_ids))
  {}


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

  trx_id_t get_max_trx_id(void)
  {
    return static_cast<trx_id_t>
           (my_atomic_load64_explicit(reinterpret_cast<int64*>(&m_max_trx_id),
                                      MY_MEMORY_ORDER_RELAXED));
  }


  /**
    Allocates a new transaction id.

    VERY important: after the database is started, m_max_trx_id value is
    divisible by TRX_SYS_TRX_ID_WRITE_MARGIN, and the following if
    will evaluate to TRUE when this function is first time called,
    and the value for trx id will be written to disk-based header!
    Thus trx id values will not overlap when the database is
    repeatedly started!

    @return new, allocated trx id
  */

  trx_id_t get_new_trx_id()
  {
    ut_ad(mutex_own(&mutex));
    trx_id_t id= static_cast<trx_id_t>(my_atomic_add64_explicit(
      reinterpret_cast<int64*>(&m_max_trx_id), 1, MY_MEMORY_ORDER_RELAXED));

    if (UNIV_UNLIKELY(!(id % TRX_SYS_TRX_ID_WRITE_MARGIN)))
      flush_max_trx_id();
    return(id);
  }


  void init_max_trx_id(trx_id_t value)
  {
    m_max_trx_id= value;
  }


  bool is_initialised() { return m_initialised; }


  /** Create the instance */
  void create();

  /** Close the transaction system on shutdown */
  void close();

  /** @return total number of active (non-prepared) transactions */
  ulint any_active_transactions();


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


  /**
    Writes the value of m_max_trx_id to the file based trx system header.
  */

  void flush_max_trx_id();
};


/** The transaction system */
extern trx_sys_t trx_sys;

#include "trx0sys.ic"

#endif
