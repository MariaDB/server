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
@file include/trx0rseg.h
Rollback segment

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#pragma once
#include "trx0types.h"
#include "fut0lst.h"
#ifdef WITH_WSREP
# include "trx0xa.h"
#endif /* WITH_WSREP */

/** Gets a rollback segment header.
@param[in]	space		space where placed
@param[in]	page_no		page number of the header
@param[in,out]	mtr		mini-transaction
@return rollback segment header, page x-latched */
UNIV_INLINE
buf_block_t*
trx_rsegf_get(fil_space_t* space, uint32_t page_no, mtr_t* mtr);

/** Create a rollback segment header.
@param[in,out]	space		system, undo, or temporary tablespace
@param[in]	rseg_id		rollback segment identifier
@param[in]	max_trx_id	new value of TRX_RSEG_MAX_TRX_ID
@param[in,out]	sys_header	the TRX_SYS page (NULL for temporary rseg)
@param[in,out]	mtr		mini-transaction
@return the created rollback segment
@retval	NULL	on failure */
buf_block_t*
trx_rseg_header_create(
	fil_space_t*	space,
	ulint		rseg_id,
	trx_id_t	max_trx_id,
	buf_block_t*	sys_header,
	mtr_t*		mtr);

/** Initialize or recover the rollback segments at startup. */
dberr_t trx_rseg_array_init();

/** Create the temporary rollback segments. */
void trx_temp_rseg_create();

/* Number of undo log slots in a rollback segment file copy */
#define TRX_RSEG_N_SLOTS	(srv_page_size / 16)

/* Maximum number of transactions supported by a single rollback segment */
#define TRX_RSEG_MAX_N_TRXS	(TRX_RSEG_N_SLOTS / 2)

/** The rollback segment memory object */
struct alignas(CPU_LEVEL1_DCACHE_LINESIZE) trx_rseg_t
{
  /** tablespace containing the rollback segment; constant after init() */
  fil_space_t *space;
  /** latch protecting everything except page_no, space */
  srw_spin_lock latch;
  /** rollback segment header page number; constant after init() */
  uint32_t page_no;
  /** length of the TRX_RSEG_HISTORY list (number of transactions) */
  uint32_t history_size;

private:
  /** Reference counter to track rseg allocated transactions,
  with SKIP and NEEDS_PURGE flags. */
  std::atomic<uint32_t> ref;

  /** Whether undo tablespace truncation is pending */
  static constexpr uint32_t SKIP= 1;
  /** Whether the log segment needs purge */
  static constexpr uint32_t NEEDS_PURGE= 2;
  /** Transaction reference count multiplier */
  static constexpr uint32_t REF= 4;

  uint32_t ref_load() const { return ref.load(std::memory_order_relaxed); }

  /** Set a bit in ref */
  template<bool needs_purge> void ref_set()
  {
    static_assert(SKIP == 1U << 0, "compatibility");
    static_assert(NEEDS_PURGE == 1U << 1, "compatibility");
#if defined __GNUC__ && (defined __i386__ || defined __x86_64__)
    if (needs_purge)
      __asm__ __volatile__("lock btsl $1, %0" : "+m" (ref));
    else
      __asm__ __volatile__("lock btsl $0, %0" : "+m" (ref));
#elif defined _MSC_VER && (defined _M_IX86 || defined _M_X64)
    _interlockedbittestandset(reinterpret_cast<volatile long*>(&ref),
                              needs_purge);
#else
    ref.fetch_or(needs_purge ? NEEDS_PURGE : SKIP, std::memory_order_relaxed);
#endif
  }
  /** Clear a bit in ref */
  template<bool needs_purge> void ref_reset()
  {
    static_assert(SKIP == 1U << 0, "compatibility");
    static_assert(NEEDS_PURGE == 1U << 1, "compatibility");
#if defined __GNUC__ && (defined __i386__ || defined __x86_64__)
    if (needs_purge)
      __asm__ __volatile__("lock btrl $1, %0" : "+m" (ref));
    else
      __asm__ __volatile__("lock btrl $0, %0" : "+m" (ref));
#elif defined _MSC_VER && (defined _M_IX86 || defined _M_X64)
    _interlockedbittestandreset(reinterpret_cast<volatile long*>(&ref),
                                needs_purge);
#else
    ref.fetch_and(needs_purge ? ~NEEDS_PURGE : ~SKIP,
                  std::memory_order_relaxed);
#endif
  }

public:

  /** Initialize the fields that are not zero-initialized. */
  void init(fil_space_t *space, uint32_t page);
  /** Reinitialize the fields on undo tablespace truncation. */
  void reinit(uint32_t page);
  /** Clean up. */
  void destroy();

  /** Note that undo tablespace truncation was started. */
  void set_skip_allocation() { ut_ad(is_persistent()); ref_set<false>(); }
  /** Note that undo tablespace truncation was completed. */
  void clear_skip_allocation()
  {
    ut_ad(is_persistent());
#if defined DBUG_OFF
    ref_reset<false>();
#else
    ut_d(auto r=) ref.fetch_and(~SKIP, std::memory_order_relaxed);
    ut_ad(r == SKIP);
#endif
  }
  /** Note that the rollback segment requires purge. */
  void set_needs_purge() { ref_set<true>(); }
  /** Note that the rollback segment will not require purge. */
  void clear_needs_purge() { ref_reset<true>(); }
  /** @return whether the segment is marked for undo truncation */
  bool skip_allocation() const { return ref_load() & SKIP; }
  /** @return whether the segment needs purge */
  bool needs_purge() const { return ref_load() & NEEDS_PURGE; }
  /** Increment the reference count */
  void acquire()
  { ut_d(auto r=) ref.fetch_add(REF); ut_ad(!(r & SKIP)); }
  /** Increment the reference count if possible
  @retval true  if the reference count was incremented
  @retval false if skip_allocation() holds */
  bool acquire_if_available()
  {
    uint32_t r= 0;
    while (!ref.compare_exchange_weak(r, r + REF,
                                      std::memory_order_relaxed,
                                      std::memory_order_relaxed))
      if (r & SKIP)
        return false;
    return true;
  }

  /** Decrement the reference count */
  void release()
  {
    ut_d(const auto r=)
    ref.fetch_sub(REF, std::memory_order_relaxed);
    ut_ad(r >= REF);
  }
  /** @return whether references exist */
  bool is_referenced() const { return ref_load() >= REF; }

  /** current size in pages */
  uint32_t curr_size;

  /** List of undo logs (transactions) */
  UT_LIST_BASE_NODE_T(trx_undo_t) undo_list;
  /** List of undo log segments cached for fast reuse */
  UT_LIST_BASE_NODE_T(trx_undo_t) undo_cached;

  /** Last not yet purged undo log header; FIL_NULL if all purged */
  uint32_t last_page_no;

  /** trx_t::no | last_offset << 48 */
  uint64_t last_commit_and_offset;

  /** @return the commit ID of the last committed transaction */
  trx_id_t last_trx_no() const
  { return last_commit_and_offset & ((1ULL << 48) - 1); }
  /** @return header offset of the last committed transaction */
  uint16_t last_offset() const
  { return static_cast<uint16_t>(last_commit_and_offset >> 48); }

  void set_last_commit(uint16_t last_offset, trx_id_t trx_no)
  {
    last_commit_and_offset= static_cast<uint64_t>(last_offset) << 48 | trx_no;
  }

  /** @return whether the rollback segment is persistent */
  bool is_persistent() const
  {
    ut_ad(space == fil_system.temp_space || space == fil_system.sys_space ||
          (srv_undo_space_id_start > 0 &&
           space->id >= srv_undo_space_id_start &&
           space->id <= srv_undo_space_id_start + TRX_SYS_MAX_UNDO_SPACES));
    ut_ad(space == fil_system.temp_space || space == fil_system.sys_space ||
          !srv_was_started ||
          (srv_undo_space_id_start > 0 &&
           space->id >= srv_undo_space_id_start
           && space->id <= srv_undo_space_id_start +
           srv_undo_tablespaces_open));
    return space->id != SRV_TMP_SPACE_ID;
  }
};

/* Undo log segment slot in a rollback segment header */
/*-------------------------------------------------------------*/
#define	TRX_RSEG_SLOT_PAGE_NO	0	/* Page number of the header page of
					an undo log segment */
/*-------------------------------------------------------------*/
/* Slot size */
#define TRX_RSEG_SLOT_SIZE	4

/* The offset of the rollback segment header on its page */
#define	TRX_RSEG		FSEG_PAGE_DATA

/* Transaction rollback segment header */
/*-------------------------------------------------------------*/
/** 0xfffffffe = pre-MariaDB 10.3.5 format; 0=MariaDB 10.3.5 or later */
#define	TRX_RSEG_FORMAT		0
/** Number of pages in the TRX_RSEG_HISTORY list */
#define	TRX_RSEG_HISTORY_SIZE	4
/** Committed transaction logs that have not been purged yet */
#define	TRX_RSEG_HISTORY	8
#define	TRX_RSEG_FSEG_HEADER	(8 + FLST_BASE_NODE_SIZE)
					/* Header for the file segment where
					this page is placed */
#define TRX_RSEG_UNDO_SLOTS	(8 + FLST_BASE_NODE_SIZE + FSEG_HEADER_SIZE)
					/* Undo log segment slots */
/** Maximum transaction ID (valid only if TRX_RSEG_FORMAT is 0) */
#define TRX_RSEG_MAX_TRX_ID	(TRX_RSEG_UNDO_SLOTS + TRX_RSEG_N_SLOTS	\
				 * TRX_RSEG_SLOT_SIZE)

/** 8 bytes offset within the binlog file */
#define TRX_RSEG_BINLOG_OFFSET		TRX_RSEG_MAX_TRX_ID + 8
/** MySQL log file name, 512 bytes, including terminating NUL
(valid only if TRX_RSEG_FORMAT is 0).
If no binlog information is present, the first byte is NUL. */
#define TRX_RSEG_BINLOG_NAME		TRX_RSEG_MAX_TRX_ID + 16
/** Maximum length of binlog file name, including terminating NUL, in bytes */
#define TRX_RSEG_BINLOG_NAME_LEN	512

#ifdef WITH_WSREP
/** The offset to WSREP XID headers */
#define	TRX_RSEG_WSREP_XID_INFO		TRX_RSEG_MAX_TRX_ID + 16 + 512

/** WSREP XID format (1 if present and valid, 0 if not present) */
#define TRX_RSEG_WSREP_XID_FORMAT	TRX_RSEG_WSREP_XID_INFO
/** WSREP XID GTRID length */
#define TRX_RSEG_WSREP_XID_GTRID_LEN	TRX_RSEG_WSREP_XID_INFO + 4
/** WSREP XID bqual length */
#define TRX_RSEG_WSREP_XID_BQUAL_LEN	TRX_RSEG_WSREP_XID_INFO + 8
/** WSREP XID data (XIDDATASIZE bytes) */
#define TRX_RSEG_WSREP_XID_DATA		TRX_RSEG_WSREP_XID_INFO + 12
#endif /* WITH_WSREP*/

/*-------------------------------------------------------------*/

/** Read the page number of an undo log slot.
@param[in]      rseg_header     rollback segment header
@param[in]      n               slot number */
inline uint32_t trx_rsegf_get_nth_undo(const buf_block_t *rseg_header, ulint n)
{
  ut_ad(n < TRX_RSEG_N_SLOTS);
  return mach_read_from_4(TRX_RSEG + TRX_RSEG_UNDO_SLOTS +
                          n * TRX_RSEG_SLOT_SIZE + rseg_header->page.frame);
}

#ifdef WITH_WSREP
/** Update the WSREP XID information in rollback segment header.
@param[in,out]	rseg_header	rollback segment header
@param[in]	xid		WSREP XID
@param[in,out]	mtr		mini-transaction */
void
trx_rseg_update_wsrep_checkpoint(
	buf_block_t*	rseg_header,
	const XID*	xid,
	mtr_t*		mtr);

/** Update WSREP checkpoint XID in first rollback segment header
as part of wsrep_set_SE_checkpoint() when it is guaranteed that there
are no wsrep transactions committing.
If the UUID part of the WSREP XID does not match to the UUIDs of XIDs already
stored into rollback segments, the WSREP XID in all the remaining rollback
segments will be reset.
@param[in]	xid		WSREP XID */
void trx_rseg_update_wsrep_checkpoint(const XID* xid);

/** Recover the latest WSREP checkpoint XID.
@param[out]	xid	WSREP XID
@return	whether the WSREP XID was found */
bool trx_rseg_read_wsrep_checkpoint(XID& xid);
#endif /* WITH_WSREP */

/** Upgrade a rollback segment header page to MariaDB 10.3 format.
@param[in,out]	rseg_header	rollback segment header page
@param[in,out]	mtr		mini-transaction */
void trx_rseg_format_upgrade(buf_block_t *rseg_header, mtr_t *mtr);

/** Update the offset information about the end of the binlog entry
which corresponds to the transaction just being committed.
In a replication slave, this updates the master binlog position
up to which replication has proceeded.
@param[in,out]	rseg_header	rollback segment header
@param[in]	trx		committing transaction
@param[in,out]	mtr		mini-transaction */
void trx_rseg_update_binlog_offset(buf_block_t *rseg_header, const trx_t *trx,
                                   mtr_t *mtr);

#include "trx0rseg.inl"
