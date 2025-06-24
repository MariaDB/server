/*****************************************************************************

Copyright (c) 1996, 2014, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/trx0types.h
Transaction system global type definitions

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#pragma once
#include "univ.i"
#include "ut0new.h"

#include <vector>

/** printf(3) format used for printing DB_TRX_ID and other system fields */
#define TRX_ID_FMT	IB_ID_FMT

/** maximum length that a formatted trx_t::id could take, not including
the terminating NUL character. */
static const ulint TRX_ID_MAX_LEN = 17;

/** Space id of the transaction system page (the system tablespace) */
static constexpr uint32_t TRX_SYS_SPACE= 0;

/** Page number of the transaction system page */
#define TRX_SYS_PAGE_NO		FSP_TRX_SYS_PAGE_NO

/** Random value to check for corruption of trx_t */
static const ulint TRX_MAGIC_N = 91118598;

constexpr uint innodb_purge_threads_MAX= 32;
constexpr uint innodb_purge_batch_size_MAX= 5000;

/** Transaction states (trx_t::state) */
enum trx_state_t {
	TRX_STATE_NOT_STARTED,
	/** The transaction was aborted (rolled back) due to an error */
	TRX_STATE_ABORTED,

	TRX_STATE_ACTIVE,
	/** XA PREPARE has been executed; only XA COMMIT or XA ROLLBACK
	are possible */
	TRX_STATE_PREPARED,
	/** XA PREPARE transaction that was returned to ha_recover() */
	TRX_STATE_PREPARED_RECOVERED,
        /** The transaction has been committed (or completely rolled back) */
	TRX_STATE_COMMITTED_IN_MEMORY
};

/** Transaction bulk insert operation @see trx_t::bulk_insert */
enum trx_bulk_insert {
    TRX_NO_BULK,
    /** bulk insert is being executed during DML */
    TRX_DML_BULK,
    /** bulk insert is being executed in copy_data_between_tables() */
    TRX_DDL_BULK
};

/** Memory objects */
/* @{ */
/** Transaction */
struct trx_t;
/** The locks and state of an active transaction */
struct trx_lock_t;
/** Rollback segment */
struct trx_rseg_t;
/** Transaction undo log */
struct trx_undo_t;
/** Rollback command node in a query graph */
struct roll_node_t;
/** Commit command node in a query graph */
struct commit_node_t;
/* @} */

/** Row identifier (DB_ROW_ID, DATA_ROW_ID) */
typedef ib_id_t	row_id_t;
/** Transaction identifier (DB_TRX_ID, DATA_TRX_ID) */
typedef ib_id_t	trx_id_t;
/** Rollback pointer (DB_ROLL_PTR, DATA_ROLL_PTR) */
typedef ib_id_t	roll_ptr_t;
/** Undo number */
typedef ib_id_t	undo_no_t;

/** File objects */
/* @{ */
/** Undo segment header */
typedef byte	trx_usegf_t;
/** Undo log header */
typedef byte	trx_ulogf_t;
/** Undo log page header */
typedef byte	trx_upagef_t;

/** Undo log record */
typedef	byte	trx_undo_rec_t;

/* @} */

/** Info required to purge a record */
struct trx_purge_rec_t
{
  /** Undo log record, or nullptr (roll_ptr!=0 if the log can be skipped) */
  const trx_undo_rec_t *undo_rec;
  /** File pointer to undo_rec */
  roll_ptr_t roll_ptr;
};

typedef std::vector<trx_id_t, ut_allocator<trx_id_t> >	trx_ids_t;

/** Number of std::unordered_map hash buckets expected to be needed
for table IDs in a purge batch. GNU libstdc++ would default to 1 and
enlarge and rehash on demand. */
static constexpr size_t TRX_PURGE_TABLE_BUCKETS= 128;

/** The number of rollback segments; rollback segment id must fit in
the 7 bits reserved for it in DB_ROLL_PTR. */
static constexpr unsigned TRX_SYS_N_RSEGS= 128;
/** Maximum number of undo tablespaces (not counting the system tablespace) */
static constexpr unsigned TRX_SYS_MAX_UNDO_SPACES= TRX_SYS_N_RSEGS - 1;
