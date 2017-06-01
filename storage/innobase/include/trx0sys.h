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

// Forward declaration
class MVCC;
class ReadView;

/** The transaction system */
extern trx_sys_t*	trx_sys;

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
Creates the trx_sys instance and initializes purge_queue and mutex. */
void
trx_sys_create(void);
/*================*/
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
/*****************************************************************//**
Allocates a new transaction id.
@return new, allocated trx id */
UNIV_INLINE
trx_id_t
trx_sys_get_new_trx_id();
/*===================*/
/*****************************************************************//**
Determines the maximum transaction id.
@return maximum currently allocated trx id; will be stale after the
next call to trx_sys_get_new_trx_id() */
UNIV_INLINE
trx_id_t
trx_sys_get_max_trx_id(void);
/*========================*/

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
/*****************************************************************//**
Reads a trx id from an index page. In case that the id size changes in
some future version, this function should be used instead of
mach_read_...
@return id */
UNIV_INLINE
trx_id_t
trx_read_trx_id(
/*============*/
	const byte*	ptr);	/*!< in: pointer to memory from where to read */
/****************************************************************//**
Looks for the trx instance with the given id in the rw trx_list.
@return	the trx handle or NULL if not found */
UNIV_INLINE
trx_t*
trx_get_rw_trx_by_id(
/*=================*/
	trx_id_t	trx_id);/*!< in: trx id to search for */
/****************************************************************//**
Returns the minimum trx id in rw trx list. This is the smallest id for which
the trx can possibly be active. (But, you must look at the trx->state to
find out if the minimum trx id transaction itself is active, or already
committed.)
@return the minimum trx id, or trx_sys->max_trx_id if the trx list is empty */
UNIV_INLINE
trx_id_t
trx_rw_min_trx_id(void);
/*===================*/
/****************************************************************//**
Checks if a rw transaction with the given id is active.
@return transaction instance if active, or NULL */
UNIV_INLINE
trx_t*
trx_rw_is_active_low(
/*=================*/
	trx_id_t	trx_id,		/*!< in: trx id of the transaction */
	ibool*		corrupt);	/*!< in: NULL or pointer to a flag
					that will be set if corrupt */
/****************************************************************//**
Checks if a rw transaction with the given id is active. If the caller is
not holding trx_sys->mutex, the transaction may already have been
committed.
@return transaction instance if active, or NULL; */
UNIV_INLINE
trx_t*
trx_rw_is_active(
/*=============*/
	trx_id_t	trx_id,		/*!< in: trx id of the transaction */
	ibool*		corrupt,	/*!< in: NULL or pointer to a flag
					that will be set if corrupt */
	bool		do_ref_count);	/*!< in: if true then increment the
					trx_t::n_ref_count */
#if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
/***********************************************************//**
Assert that a transaction has been recovered.
@return TRUE */
UNIV_INLINE
ibool
trx_assert_recovered(
/*=================*/
	trx_id_t	trx_id)		/*!< in: transaction identifier */
	MY_ATTRIBUTE((warn_unused_result));
#endif /* UNIV_DEBUG || UNIV_BLOB_LIGHT_DEBUG */
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
	ulint		field,	/*!< in: offset of the MySQL log info field in
				the trx sys header */
        trx_sysf_t*     sys_header, /*!< in: trx sys header */
	mtr_t*		mtr);	/*!< in: mtr */
/*****************************************************************//**
Prints to stderr the MySQL binlog offset info in the trx system header if
the magic number shows it valid. */
void
trx_sys_print_mysql_binlog_offset(void);
/*===================================*/
#ifdef WITH_WSREP
/** Update WSREP checkpoint XID in sys header. */
void
trx_sys_update_wsrep_checkpoint(
/*============================*/
	const XID*	xid,		/*!< in: WSREP XID */
	trx_sysf_t*	sys_header,	/*!< in: sys_header */
	mtr_t*		mtr);		/*!< in: mtr */

void
/** Read WSREP checkpoint XID from sys header. */
trx_sys_read_wsrep_checkpoint(
/*==========================*/
	XID* xid); /*!< out: WSREP XID */
#endif /* WITH_WSREP */

/*****************************************************************//**
Shutdown/Close the transaction system. */
void
trx_sys_close(void);
/*===============*/
/** Create the rollback segments.
@return	whether the creation succeeded */
bool
trx_sys_create_rsegs();
/*****************************************************************//**
Get the number of transaction in the system, independent of their state.
@return count of transactions in trx_sys_t::trx_list */
UNIV_INLINE
ulint
trx_sys_get_n_rw_trx(void);
/*======================*/

/*********************************************************************
Check if there are any active (non-prepared) transactions.
@return total number of active transactions or 0 if none */
ulint
trx_sys_any_active_transactions(void);
/*=================================*/

/**
Add the transaction to the RW transaction set
@param trx		transaction instance to add */
UNIV_INLINE
void
trx_sys_rw_trx_add(trx_t* trx);

#ifdef UNIV_DEBUG
/*************************************************************//**
Validate the trx_sys_t::rw_trx_list.
@return true if the list is valid */
bool
trx_sys_validate_trx_list();
/*========================*/
#endif /* UNIV_DEBUG */

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
#define TRX_SYS_MYSQL_LOG_OFFSET_HIGH	4	/*!< high 4 bytes of the offset
						within that file */
#define TRX_SYS_MYSQL_LOG_OFFSET_LOW	8	/*!< low 4 bytes of the offset
						within that file */
#define TRX_SYS_MYSQL_LOG_NAME		12	/*!< MySQL log file name */

#ifdef WITH_WSREP
/* The offset to WSREP XID headers */
#define TRX_SYS_WSREP_XID_INFO (UNIV_PAGE_SIZE - 3500)
#define TRX_SYS_WSREP_XID_MAGIC_N_FLD 0
#define TRX_SYS_WSREP_XID_MAGIC_N 0x77737265

/* XID field: formatID, gtrid_len, bqual_len, xid_data */
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
/* @} */

/** The transaction system central memory data structure. */
struct trx_sys_t {

	TrxSysMutex	mutex;		/*!< mutex protecting most fields in
					this structure except when noted
					otherwise */

	MVCC*		mvcc;		/*!< Multi version concurrency control
					manager */
	volatile trx_id_t
			max_trx_id;	/*!< The smallest number not yet
					assigned as a transaction id or
					transaction number. This is declared
					volatile because it can be accessed
					without holding any mutex during
					AC-NL-RO view creation. */
	trx_ut_list_t	serialisation_list;
					/*!< Ordered on trx_t::no of all the
					currenrtly active RW transactions */
#ifdef UNIV_DEBUG
	trx_id_t	rw_max_trx_id;	/*!< Max trx id of read-write
					transactions which exist or existed */
#endif /* UNIV_DEBUG */

	/** Avoid false sharing */
	const char	pad1[CACHE_LINE_SIZE];
	trx_ut_list_t	rw_trx_list;	/*!< List of active and committed in
					memory read-write transactions, sorted
					on trx id, biggest first. Recovered
					transactions are always on this list. */

	/** Avoid false sharing */
	const char	pad2[CACHE_LINE_SIZE];
	trx_ut_list_t	mysql_trx_list;	/*!< List of transactions created
					for MySQL. All user transactions are
					on mysql_trx_list. The rw_trx_list
					can contain system transactions and
					recovered transactions that will not
					be in the mysql_trx_list.
					mysql_trx_list may additionally contain
					transactions that have not yet been
					started in InnoDB. */

	trx_ids_t	rw_trx_ids;	/*!< Array of Read write transaction IDs
					for MVCC snapshot. A ReadView would take
					a snapshot of these transactions whose
					changes are not visible to it. We should
					remove transactions from the list before
					committing in memory and releasing locks
					to ensure right order of removal and
					consistent snapshot. */

	/** Avoid false sharing */
	const char	pad3[CACHE_LINE_SIZE];
	/** Temporary rollback segments */
	trx_rseg_t*	temp_rsegs[TRX_SYS_N_RSEGS];
	/** Avoid false sharing */
	const char	pad4[CACHE_LINE_SIZE];

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

	TrxIdSet	rw_trx_set;	/*!< Mapping from transaction id
					to transaction instance */

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
};

/** When a trx id which is zero modulo this number (which must be a power of
two) is assigned, the field TRX_SYS_TRX_ID_STORE on the transaction system
page is updated */
#define TRX_SYS_TRX_ID_WRITE_MARGIN	((trx_id_t) 256)

/** Test if trx_sys->mutex is owned. */
#define trx_sys_mutex_own() (trx_sys->mutex.is_owned())

/** Acquire the trx_sys->mutex. */
#define trx_sys_mutex_enter() do {			\
	mutex_enter(&trx_sys->mutex);			\
} while (0)

/** Release the trx_sys->mutex. */
#define trx_sys_mutex_exit() do {			\
	trx_sys->mutex.exit();				\
} while (0)

#include "trx0sys.ic"

#endif
