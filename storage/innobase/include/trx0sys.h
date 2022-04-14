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
/** Display the binlog offset info if it is present in the undo pages. */
void trx_sys_print_mysql_binlog_offset();

/** Create the rollback segments.
@return	whether the creation succeeded */
bool trx_sys_create_rsegs();

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

class rw_trx_hash_t
{
  /** number of payload elements in array[] */
  static constexpr size_t n_cells= 64;
  /** Hash cell chain */
  struct hash_chain
  {
    /** pointer to the first transaction */
    trx_t *first;

    /** Append a transaction. */
    void append(trx_t *trx)
    {
      ut_d(validate_element(trx));
      ut_ad(!trx->rw_trx_hash);
      trx_t **prev= &first;
      while (*prev)
        prev= &(*prev)->rw_trx_hash;
      *prev= trx;
    }

    /** Remove a transaction. */
    void remove(trx_t *trx)
    {
      ut_d(validate_element(trx));
      trx_t **prev= &first;
      while (*prev != trx)
        prev= &(*prev)->rw_trx_hash;
      *prev= trx->rw_trx_hash;
      trx->rw_trx_hash= nullptr;
    }
  };

  /** Number of array[] elements per page_hash_latch.
  Must be one less than a power of 2. */
  static constexpr size_t ELEMENTS_PER_LATCH= 64 / sizeof(void*) - 1;
  static constexpr size_t EMPTY_SLOTS_PER_LATCH=
    ((CPU_LEVEL1_DCACHE_LINESIZE / 64) - 1) * (64 / sizeof(void*));

  /** @return raw array index converted to padded index */
  static constexpr ulint pad(ulint h)
  {
    return 1 + h + (h / ELEMENTS_PER_LATCH) * (1 + EMPTY_SLOTS_PER_LATCH);
  }

  static constexpr size_t n_cells_padded= 1 + n_cells +
    (n_cells / ELEMENTS_PER_LATCH) * (1 + EMPTY_SLOTS_PER_LATCH);
  static constexpr size_t n_cells_padded_aligned=
    (n_cells_padded + CPU_LEVEL1_DCACHE_LINESIZE - 1) &
    ~(CPU_LEVEL1_DCACHE_LINESIZE - 1);

  /** the hash table */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE)
  hash_chain array[n_cells_padded_aligned];

  ulint calc_hash(trx_id_t id) const { return calc_hash(ulint(id), n_cells); }

  /** @return the hash value before any ELEMENTS_PER_LATCH padding */
  static ulint hash(ulint fold, ulint n) { return ut_hash_ulint(fold, n); }

  /** @return the index of an array element */
  static ulint calc_hash(ulint fold, ulint n_cells)
  {
    return pad(hash(fold, n_cells));
  }

  /** @return the latch covering a hash table chain */
  static page_hash_latch &lock_get(hash_chain &chain)
  {
    static_assert(!((ELEMENTS_PER_LATCH + 1) & ELEMENTS_PER_LATCH),
                  "must be one less than a power of 2");
    const size_t addr= reinterpret_cast<size_t>(&chain);
    ut_ad(addr & (ELEMENTS_PER_LATCH * sizeof chain));
    return *reinterpret_cast<page_hash_latch*>
      (addr & ~(ELEMENTS_PER_LATCH * sizeof chain));
  }

  /** Get a hash table slot. */
  hash_chain &cell_get(trx_id_t id) { return array[calc_hash(id)]; }

#ifdef NO_ELISION
  static constexpr bool empty(const page_hash_latch*) { return false; }
#else
  /** @return whether the cache line is empty */
  TRANSACTIONAL_TARGET bool empty(const page_hash_latch *lk) const;
#endif

public:
  inline void destroy();

  bool empty() { return !for_each_until([](const trx_t&){return true;}); }

  size_t size()
  {
    size_t count= 0;
    for_each([&count](const trx_t&){count++;});
    return count;
  }

  page_hash_latch &lock_get(trx_id_t id) { return lock_get(cell_get(id)); }

  void insert(trx_t *trx)
  {
    hash_chain &chain= cell_get(trx->id);
    auto &lk= lock_get(chain);
    lk.lock();
    chain.append(trx);
    lk.unlock();
  }

  void erase(trx_t *trx)
  {
    hash_chain &chain= cell_get(trx->id);
    auto &lk= lock_get(chain);
    lk.lock();
    chain.remove(trx);
    lk.unlock();
  }

  template <typename Callable> void for_each(Callable &&callback)
  {
    const ulint n= pad(n_cells);
    ulint i= 0;
    do
    {
      const ulint k= i + (ELEMENTS_PER_LATCH + 1);
      auto lk= reinterpret_cast<page_hash_latch*>(&array[i]);
      if (!empty(lk))
      {
        lk->lock_shared();
        while (++i < k)
        {
          for (trx_t* trx= array[i].first; trx; trx= trx->rw_trx_hash)
          {
            ut_d(validate_element(trx));
            callback(*trx);
          }
        }
        lk->unlock_shared();
      }
      i= k;
    }
    while (i < n);
  }

  template <typename Callable> bool for_each_until(Callable &&callback)
  {
    const ulint n= pad(n_cells);
    ulint i= 0;
    do
    {
      const ulint k= i + (ELEMENTS_PER_LATCH + 1);
      auto lk= reinterpret_cast<page_hash_latch*>(&array[i]);
      if (!empty(lk))
      {
        lk->lock_shared();
        while (++i < k)
        {
          for (trx_t* trx= array[i].first; trx; trx= trx->rw_trx_hash)
          {
            ut_d(validate_element(trx));
            if (callback(*trx))
            {
              lk->unlock_shared();
              return true;
            }
          }
        }
        lk->unlock_shared();
      }
      i= k;
    }
    while (i < n);
    return false;
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
#endif

  /**
    Find a transaction.

    Only ACTIVE or PREPARED trx objects may participate in hash. Nevertheless
    the transaction may get committed before this method returns.

    With do_ref_count == false the caller may dereference returned trx pointer
    only if lock_sys.latch was acquired before calling find().

    With do_ref_count == true caller may dereference trx even if it is not
    holding lock_sys.latch. Caller is responsible for calling
    trx->release_reference() when it is done playing with trx.

    @return transaction corresponding to trx_id
    @retval nullptr if not found
  */

  trx_t *find(trx_id_t trx_id, bool do_ref_count)
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

    trx_t *trx= nullptr;
    hash_chain &chain= cell_get(trx_id);
    auto &lk= lock_get(chain);
    lk.lock_shared();
    for (trx= chain.first; trx; trx= trx->rw_trx_hash)
    {
      if (trx->id == trx_id)
      {
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
        break;
      }
    }
    lk.unlock_shared();
    return trx;
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
    Returns the minimum trx id in rw trx list.

    This is the smallest id for which the trx can possibly be active. (But, you
    must look at the trx->state to find out if the minimum trx id transaction
    itself is active, or already committed.)

    @return the minimum trx id, or m_max_trx_id if the trx list is empty
  */

  trx_id_t get_min_trx_id()
  {
    trx_id_t id= get_max_trx_id();
    rw_trx_hash.for_each([&id](const trx_t &trx)
    {
      /* We don't care about read-only transactions here. */
      if (trx.id < id && trx.rsegs.m_redo.rseg)
        id= trx.id;
    });
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
  inline void assign_new_trx_no(trx_t &trx);


  /**
    Takes MVCC snapshot.

    To reduce malloc probablility we reserve rw_trx_hash.size() + 32 elements
    in ids.

    For details about get_rw_trx_hash_version() != get_max_trx_id() spin
    @sa register_rw() and @sa assign_new_trx_no().

    We rely on get_rw_trx_hash_version() to issue ACQUIRE memory barrier so
    that loading of m_rw_trx_hash_version happens before accessing rw_trx_hash.

    @param[out]    ids        array to store registered transaction identifiers
    @param[out]    max_trx_id variable to store m_max_trx_id value
    @param[out]    mix_trx_no variable to store min(no) value
  */

  void snapshot_ids(trx_ids_t *ids, trx_id_t *max_trx_id, trx_id_t *min_trx_no)
  {
    trx_id_t max_id;
    while ((max_id= get_rw_trx_hash_version()) != get_max_trx_id())
      ut_delay(1);
    trx_id_t min_no= max_id;
    ids->clear();
    ids->reserve(rw_trx_hash.size() + 32);
    rw_trx_hash.for_each([max_id,&min_no,&ids](const trx_t &trx)
    {
      if (trx.id < max_id)
      {
        ids->push_back(trx.id);
        if (trx.no < min_no)
          min_no= trx.no;
      }
    });
    *max_trx_id= max_id;
    *min_trx_no= min_no;
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


  bool is_registered(trx_id_t id) { return id && find(id, false); }


  trx_t *find(trx_id_t id, bool do_ref_count= true)
  {
    return rw_trx_hash.find(id, do_ref_count);
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
