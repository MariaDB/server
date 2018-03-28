/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2018, MariaDB Corporation.

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
@file trx/trx0sys.cc
Transaction system

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "ha_prototypes.h"

#include "mysqld.h"
#include "trx0sys.h"
#include "sql_error.h"

#include "fsp0fsp.h"
#include "mtr0log.h"
#include "mtr0log.h"
#include "trx0trx.h"
#include "trx0rseg.h"
#include "trx0undo.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0purge.h"
#include "log0log.h"
#include "log0recv.h"
#include "os0file.h"
#include "fsp0sysspace.h"

#include <mysql/service_wsrep.h>

/** The transaction system */
trx_sys_t		trx_sys;

/** Check whether transaction id is valid.
@param[in]	id              transaction id to check
@param[in]      name            table name */
void
ReadView::check_trx_id_sanity(
	trx_id_t		id,
	const table_name_t&	name)
{
	if (id >= trx_sys.get_max_trx_id()) {

		ib::warn() << "A transaction id"
			   << " in a record of table "
			   << name
			   << " is newer than the"
			   << " system-wide maximum.";
		ut_ad(0);
		THD *thd = current_thd;
		if (thd != NULL) {
			char    table_name[MAX_FULL_NAME_LEN + 1];

			innobase_format_name(
				table_name, sizeof(table_name),
				name.m_name);

			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_SIGNAL_WARN,
					    "InnoDB: Transaction id"
					    " in a record of table"
					    " %s is newer than system-wide"
					    " maximum.", table_name);
		}
	}
}

#ifdef UNIV_DEBUG
/* Flag to control TRX_RSEG_N_SLOTS behavior debugging. */
uint	trx_rseg_n_slots_debug = 0;
#endif

/** Display the MySQL binlog offset info if it is present in the trx
system header. */
void
trx_sys_print_mysql_binlog_offset()
{
	if (!*trx_sys.recovered_binlog_filename) {
		return;
	}

	ib::info() << "Last binlog file '"
		<< trx_sys.recovered_binlog_filename
		<< "', position "
		<< trx_sys.recovered_binlog_offset;
}

/** Find an available rollback segment.
@param[in]	sys_header
@return an unallocated rollback segment slot in the TRX_SYS header
@retval ULINT_UNDEFINED if not found */
ulint
trx_sys_rseg_find_free(const buf_block_t* sys_header)
{
	for (ulint rseg_id = 0; rseg_id < TRX_SYS_N_RSEGS; rseg_id++) {
		if (trx_sysf_rseg_get_page_no(sys_header, rseg_id)
		    == FIL_NULL) {
			return rseg_id;
		}
	}

	return(ULINT_UNDEFINED);
}

/** Count the number of initialized persistent rollback segment slots. */
static
void
trx_sysf_get_n_rseg_slots()
{
	mtr_t		mtr;
	mtr.start();

	srv_available_undo_logs = 0;
	if (const buf_block_t* sys_header = trx_sysf_get(&mtr, false)) {
		for (ulint rseg_id = 0; rseg_id < TRX_SYS_N_RSEGS; rseg_id++) {
			srv_available_undo_logs
				+= trx_sysf_rseg_get_page_no(sys_header,
							     rseg_id)
				!= FIL_NULL;
		}
	}

	mtr.commit();
}

/*****************************************************************//**
Creates the file page for the transaction system. This function is called only
at the database creation, before trx_sys_init. */
static
void
trx_sysf_create(
/*============*/
	mtr_t*	mtr)	/*!< in: mtr */
{
	ulint		slot_no;
	buf_block_t*	block;
	page_t*		page;
	ulint		page_no;
	byte*		ptr;

	ut_ad(mtr);

	/* Note that below we first reserve the file space x-latch, and
	then enter the kernel: we must do it in this order to conform
	to the latching order rules. */

	mtr_x_lock(&fil_system.sys_space->latch, mtr);

	/* Create the trx sys file block in a new allocated file segment */
	block = fseg_create(TRX_SYS_SPACE, 0, TRX_SYS + TRX_SYS_FSEG_HEADER,
			    mtr);
	buf_block_dbg_add_level(block, SYNC_TRX_SYS_HEADER);

	ut_a(block->page.id.page_no() == TRX_SYS_PAGE_NO);

	page = buf_block_get_frame(block);

	mlog_write_ulint(page + FIL_PAGE_TYPE, FIL_PAGE_TYPE_TRX_SYS,
			 MLOG_2BYTES, mtr);

	/* Reset the doublewrite buffer magic number to zero so that we
	know that the doublewrite buffer has not yet been created (this
	suppresses a Valgrind warning) */

	mlog_write_ulint(page + TRX_SYS_DOUBLEWRITE
			 + TRX_SYS_DOUBLEWRITE_MAGIC, 0, MLOG_4BYTES, mtr);

	/* Reset the rollback segment slots.  Old versions of InnoDB
	(before MySQL 5.5) define TRX_SYS_N_RSEGS as 256 and expect
	that the whole array is initialized. */
	ptr = TRX_SYS + TRX_SYS_RSEGS + page;
	compile_time_assert(256 >= TRX_SYS_N_RSEGS);
	memset(ptr, 0xff, 256 * TRX_SYS_RSEG_SLOT_SIZE);
	ptr += 256 * TRX_SYS_RSEG_SLOT_SIZE;
	ut_a(ptr <= page + (UNIV_PAGE_SIZE - FIL_PAGE_DATA_END));

	/* Initialize all of the page.  This part used to be uninitialized. */
	memset(ptr, 0, UNIV_PAGE_SIZE - FIL_PAGE_DATA_END + page - ptr);

	mlog_log_string(TRX_SYS + page, UNIV_PAGE_SIZE - FIL_PAGE_DATA_END
			- TRX_SYS, mtr);

	/* Create the first rollback segment in the SYSTEM tablespace */
	slot_no = trx_sys_rseg_find_free(block);
	page_no = trx_rseg_header_create(TRX_SYS_SPACE, slot_no, block, mtr);

	ut_a(slot_no == TRX_SYS_SYSTEM_RSEG_ID);
	ut_a(page_no == FSP_FIRST_RSEG_PAGE_NO);
}

/** Create the instance */
void
trx_sys_t::create()
{
	ut_ad(this == &trx_sys);
	ut_ad(!is_initialised());
	m_initialised = true;
	mutex_create(LATCH_ID_TRX_SYS, &mutex);
	UT_LIST_INIT(mysql_trx_list, &trx_t::mysql_trx_list);
	UT_LIST_INIT(m_views, &ReadView::m_view_list);
	my_atomic_store32(&rseg_history_len, 0);

	rw_trx_hash.init();
}

/*****************************************************************//**
Creates and initializes the transaction system at the database creation. */
void
trx_sys_create_sys_pages(void)
/*==========================*/
{
	mtr_t	mtr;

	mtr_start(&mtr);

	trx_sysf_create(&mtr);

	mtr_commit(&mtr);
}

/** Create the rollback segments.
@return	whether the creation succeeded */
bool
trx_sys_create_rsegs()
{
	/* srv_available_undo_logs reflects the number of persistent
	rollback segments that have been initialized in the
	transaction system header page.

	srv_undo_logs determines how many of the
	srv_available_undo_logs rollback segments may be used for
	logging new transactions. */
	ut_ad(srv_undo_tablespaces <= TRX_SYS_MAX_UNDO_SPACES);
	ut_ad(srv_undo_logs <= TRX_SYS_N_RSEGS);

	if (srv_read_only_mode) {
		srv_undo_logs = srv_available_undo_logs = ULONG_UNDEFINED;
		return(true);
	}

	/* This is executed in single-threaded mode therefore it is not
	necessary to use the same mtr in trx_rseg_create(). n_used cannot
	change while the function is executing. */
	trx_sysf_get_n_rseg_slots();

	ut_ad(srv_available_undo_logs <= TRX_SYS_N_RSEGS);

	/* The first persistent rollback segment is always initialized
	in the system tablespace. */
	ut_a(srv_available_undo_logs > 0);

	if (srv_force_recovery) {
		/* Do not create additional rollback segments if
		innodb_force_recovery has been set. */
		if (srv_undo_logs > srv_available_undo_logs) {
			srv_undo_logs = srv_available_undo_logs;
		}
	} else {
		for (ulint i = 0; srv_available_undo_logs < srv_undo_logs;
		     i++, srv_available_undo_logs++) {
			/* Tablespace 0 is the system tablespace.
			Dedicated undo log tablespaces start from 1. */
			ulint space = srv_undo_tablespaces > 0
				? (i % srv_undo_tablespaces)
				+ srv_undo_space_id_start
				: TRX_SYS_SPACE;

			if (!trx_rseg_create(space)) {
				ib::error() << "Unable to allocate the"
					" requested innodb_undo_logs";
				return(false);
			}

			/* Increase the number of active undo
			tablespace in case new rollback segment
			assigned to new undo tablespace. */
			if (space > srv_undo_tablespaces_active) {
				srv_undo_tablespaces_active++;

				ut_ad(srv_undo_tablespaces_active == space);
			}
		}
	}

	ut_ad(srv_undo_logs <= srv_available_undo_logs);

	ib::info info;
	info << srv_undo_logs << " out of " << srv_available_undo_logs;
	if (srv_undo_tablespaces_active) {
		info << " rollback segments in " << srv_undo_tablespaces_active
		<< " undo tablespaces are active.";
	} else {
		info << " rollback segments are active.";
	}

	return(true);
}

/** Close the transaction system on shutdown */
void
trx_sys_t::close()
{
	ut_ad(srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS);
	if (!is_initialised()) {
		return;
	}

	if (size_t size = view_count()) {
		ib::error() << "All read views were not closed before"
			" shutdown: " << size << " read views open";
	}

	rw_trx_hash.destroy();

	/* There can't be any active transactions. */

	for (ulint i = 0; i < TRX_SYS_N_RSEGS; ++i) {
		if (trx_rseg_t* rseg = rseg_array[i]) {
			trx_rseg_mem_free(rseg);
		}

		if (trx_rseg_t* rseg = temp_rsegs[i]) {
			trx_rseg_mem_free(rseg);
		}
	}

	ut_a(UT_LIST_GET_LEN(mysql_trx_list) == 0);
	ut_ad(UT_LIST_GET_LEN(m_views) == 0);
	mutex_free(&mutex);
	m_initialised = false;
}


static my_bool active_count_callback(rw_trx_hash_element_t *element,
                                     uint32_t *count)
{
  mutex_enter(&element->mutex);
  if (trx_t *trx= element->trx)
  {
    mutex_enter(&trx->mutex);
    if (trx_state_eq(trx, TRX_STATE_ACTIVE))
      ++*count;
    mutex_exit(&trx->mutex);
  }
  mutex_exit(&element->mutex);
  return 0;
}


/** @return total number of active (non-prepared) transactions */
ulint trx_sys_t::any_active_transactions()
{
	uint32_t total_trx = 0;

	trx_sys.rw_trx_hash.iterate_no_dups(
				reinterpret_cast<my_hash_walk_action>
				(active_count_callback), &total_trx);

	mutex_enter(&mutex);
	for (trx_t* trx = UT_LIST_GET_FIRST(trx_sys.mysql_trx_list);
	     trx != NULL;
	     trx = UT_LIST_GET_NEXT(mysql_trx_list, trx)) {
		if (trx->state != TRX_STATE_NOT_STARTED && !trx->id) {
			total_trx++;
		}
	}
	mutex_exit(&mutex);

	return(total_trx);
}
