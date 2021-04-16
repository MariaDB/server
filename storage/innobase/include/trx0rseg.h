/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

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

#ifndef trx0rseg_h
#define trx0rseg_h

#include "trx0sys.h"
#include "fut0lst.h"

/** Gets a rollback segment header.
@param[in]	space		space where placed
@param[in]	page_no		page number of the header
@param[in,out]	mtr		mini-transaction
@return rollback segment header, page x-latched */
UNIV_INLINE
buf_block_t*
trx_rsegf_get(fil_space_t* space, uint32_t page_no, mtr_t* mtr);

/** Gets a newly created rollback segment header.
@param[in]	space		space where placed
@param[in]	page_no		page number of the header
@param[in,out]	mtr		mini-transaction
@return rollback segment header, page x-latched */
UNIV_INLINE
buf_block_t*
trx_rsegf_get_new(
	ulint			space,
	uint32_t		page_no,
	mtr_t*			mtr);

/** Create a rollback segment header.
@param[in,out]	space		system, undo, or temporary tablespace
@param[in]	rseg_id		rollback segment identifier
@param[in,out]	sys_header	the TRX_SYS page (NULL for temporary rseg)
@param[in,out]	mtr		mini-transaction
@return the created rollback segment
@retval	NULL	on failure */
buf_block_t*
trx_rseg_header_create(
	fil_space_t*	space,
	ulint		rseg_id,
	buf_block_t*	sys_header,
	mtr_t*		mtr);

/** Initialize the rollback segments in memory at database startup. */
void
trx_rseg_array_init();

/** Free a rollback segment in memory. */
void
trx_rseg_mem_free(trx_rseg_t* rseg);

/** Create a persistent rollback segment.
@param[in]	space_id	system or undo tablespace id
@return pointer to new rollback segment
@retval	NULL	on failure */
trx_rseg_t*
trx_rseg_create(ulint space_id)
	MY_ATTRIBUTE((warn_unused_result));

/** Create the temporary rollback segments. */
void
trx_temp_rseg_create();

/* Number of undo log slots in a rollback segment file copy */
#define TRX_RSEG_N_SLOTS	(srv_page_size / 16)

/* Maximum number of transactions supported by a single rollback segment */
#define TRX_RSEG_MAX_N_TRXS	(TRX_RSEG_N_SLOTS / 2)

/** The rollback segment memory object */
struct trx_rseg_t {
	/*--------------------------------------------------------*/
	/** rollback segment id == the index of its slot in the trx
	system file copy */
	ulint				id;

	/** mutex protecting the fields in this struct except id,space,page_no
	which are constant */
	RsegMutex			mutex;

	/** space where the rollback segment header is placed */
	fil_space_t*			space;

	/** page number of the rollback segment header */
	uint32_t			page_no;

	/** current size in pages */
	uint32_t			curr_size;

	/*--------------------------------------------------------*/
	/* Fields for undo logs */
	/** List of undo logs */
	UT_LIST_BASE_NODE_T(trx_undo_t)	undo_list;

	/** List of undo log segments cached for fast reuse */
	UT_LIST_BASE_NODE_T(trx_undo_t)	undo_cached;

	/** List of recovered old insert_undo logs of incomplete
	transactions (to roll back or XA COMMIT & purge) */
	UT_LIST_BASE_NODE_T(trx_undo_t) old_insert_list;

	/*--------------------------------------------------------*/

	/** Page number of the last not yet purged log header in the history
	list; FIL_NULL if all list purged */
	uint32_t			last_page_no;

	/** Byte offset of the last not yet purged log header */
	uint16_t			last_offset;

	/** trx_t::no * 2 + old_insert of the last not yet purged log */
	trx_id_t			last_commit;

	/** Whether the log segment needs purge */
	bool				needs_purge;

	/** Reference counter to track rseg allocated transactions. */
	ulint				trx_ref_count;

	/** If true, then skip allocating this rseg as it reside in
	UNDO-tablespace marked for truncate. */
	bool				skip_allocation;

	/** @return the commit ID of the last committed transaction */
	trx_id_t last_trx_no() const { return last_commit >> 1; }

	void set_last_trx_no(trx_id_t trx_no, bool is_update)
	{
		last_commit = trx_no << 1 | trx_id_t(is_update);
	}

	/** @return whether the rollback segment is persistent */
	bool is_persistent() const
	{
		ut_ad(space == fil_system.temp_space
		      || space == fil_system.sys_space
		      || (srv_undo_space_id_start > 0
			  && space->id >= srv_undo_space_id_start
			  && space->id <= srv_undo_space_id_start
			  + TRX_SYS_MAX_UNDO_SPACES));
		ut_ad(space == fil_system.temp_space
		      || space == fil_system.sys_space
		      || (srv_undo_space_id_start > 0
			  && space->id >= srv_undo_space_id_start
			  && space->id <= srv_undo_space_id_start
			  + srv_undo_tablespaces_open)
		      || !srv_was_started);
		return(space->id != SRV_TMP_SPACE_ID);
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
                          n * TRX_RSEG_SLOT_SIZE + rseg_header->frame);
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

#include "trx0rseg.ic"

#endif
