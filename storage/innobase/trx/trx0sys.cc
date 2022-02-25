/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
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
@file trx/trx0sys.cc
Transaction system

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0sys.h"
#include "mysqld.h"
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

/** The transaction system */
trx_sys_t		trx_sys;

/** Check whether transaction id is valid.
@param[in]	id              transaction id to check
@param[in]      name            table name */
void
ReadViewBase::check_trx_id_sanity(
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

	ut_ad(mtr);

	/* Note that below we first reserve the file space x-latch, and
	then enter the kernel: we must do it in this order to conform
	to the latching order rules. */

	mtr->x_lock_space(fil_system.sys_space);
	compile_time_assert(TRX_SYS_SPACE == 0);

	/* Create the trx sys file block in a new allocated file segment */
	block = fseg_create(fil_system.sys_space,
			    TRX_SYS + TRX_SYS_FSEG_HEADER,
			    mtr);

	ut_a(block->page.id() == page_id_t(0, TRX_SYS_PAGE_NO));

	mtr->write<2>(*block, FIL_PAGE_TYPE + block->page.frame,
		      FIL_PAGE_TYPE_TRX_SYS);

	ut_ad(!mach_read_from_4(block->page.frame
				+ TRX_SYS_DOUBLEWRITE
				+ TRX_SYS_DOUBLEWRITE_MAGIC));

	/* Reset the rollback segment slots.  Old versions of InnoDB
	(before MySQL 5.5) define TRX_SYS_N_RSEGS as 256 and expect
	that the whole array is initialized. */
	compile_time_assert(256 >= TRX_SYS_N_RSEGS);
	compile_time_assert(TRX_SYS + TRX_SYS_RSEGS
			    + 256 * TRX_SYS_RSEG_SLOT_SIZE
			    <= UNIV_PAGE_SIZE_MIN - FIL_PAGE_DATA_END);
	mtr->memset(block, TRX_SYS + TRX_SYS_RSEGS,
		    256 * TRX_SYS_RSEG_SLOT_SIZE, 0xff);
	/* Initialize all of the page.  This part used to be uninitialized. */
	mtr->memset(block, TRX_SYS + TRX_SYS_RSEGS
		    + 256 * TRX_SYS_RSEG_SLOT_SIZE,
		    srv_page_size
		    - (FIL_PAGE_DATA_END + TRX_SYS + TRX_SYS_RSEGS
		       + 256 * TRX_SYS_RSEG_SLOT_SIZE),
		    0);

	/* Create the first rollback segment in the SYSTEM tablespace */
	slot_no = trx_sys_rseg_find_free(block);
	buf_block_t* rblock = trx_rseg_header_create(fil_system.sys_space,
						     slot_no, 0, block, mtr);

	ut_a(slot_no == TRX_SYS_SYSTEM_RSEG_ID);
	ut_a(rblock->page.id() == page_id_t(0, FSP_FIRST_RSEG_PAGE_NO));
}

void trx_sys_t::create()
{
  ut_ad(this == &trx_sys);
  ut_ad(!is_initialised());
  m_initialised= true;
  trx_list.create();
  rw_trx_hash.init();
}

uint32_t trx_sys_t::history_size()
{
  ut_ad(is_initialised());
  uint32_t size= 0;
  for (auto &rseg : rseg_array)
  {
    rseg.latch.rd_lock(SRW_LOCK_CALL);
    size+= rseg.history_size;
  }
  for (auto &rseg : rseg_array)
    rseg.latch.rd_unlock();
  return size;
}

bool trx_sys_t::history_exceeds(uint32_t threshold)
{
  ut_ad(is_initialised());
  uint32_t size= 0;
  bool exceeds= false;
  size_t i;
  for (i= 0; i < array_elements(rseg_array); i++)
  {
    rseg_array[i].latch.rd_lock(SRW_LOCK_CALL);
    size+= rseg_array[i].history_size;
    if (size > threshold)
    {
      exceeds= true;
      i++;
      break;
    }
  }
  while (i)
    rseg_array[--i].latch.rd_unlock();
  return exceeds;
}

TPOOL_SUPPRESS_TSAN bool trx_sys_t::history_exists()
{
  ut_ad(is_initialised());
  for (auto &rseg : rseg_array)
    if (rseg.history_size)
      return true;
  return false;
}

TPOOL_SUPPRESS_TSAN uint32_t trx_sys_t::history_size_approx() const
{
  ut_ad(is_initialised());
  uint32_t size= 0;
  for (auto &rseg : rseg_array)
    size+= rseg.history_size;
  return size;
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

/** Create a persistent rollback segment.
@param space_id   system or undo tablespace id
@return pointer to new rollback segment
@retval nullptr  on failure */
static trx_rseg_t *trx_rseg_create(uint32_t space_id)
{
  trx_rseg_t *rseg= nullptr;
  mtr_t mtr;

  mtr.start();

  if (fil_space_t *space= mtr.x_lock_space(space_id))
  {
    ut_ad(space->purpose == FIL_TYPE_TABLESPACE);
    if (buf_block_t *sys_header= trx_sysf_get(&mtr))
    {
      ulint rseg_id= trx_sys_rseg_find_free(sys_header);
      if (buf_block_t *rblock= rseg_id == ULINT_UNDEFINED
          ? nullptr : trx_rseg_header_create(space, rseg_id, 0, sys_header,
                                             &mtr))
      {
        ut_ad(trx_sysf_rseg_get_space(sys_header, rseg_id) == space_id);
        rseg= &trx_sys.rseg_array[rseg_id];
        rseg->init(space, rblock->page.id().page_no());
        ut_ad(rseg->is_persistent());
      }
    }
  }

  mtr.commit();
  return rseg;
}

/** Create the rollback segments.
@return	whether the creation succeeded */
bool trx_sys_create_rsegs()
{
	/* srv_available_undo_logs reflects the number of persistent
	rollback segments that have been initialized in the
	transaction system header page. */
	ut_ad(srv_undo_tablespaces <= TRX_SYS_MAX_UNDO_SPACES);

	if (high_level_read_only) {
		srv_available_undo_logs = 0;
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

	for (uint32_t i = 0; srv_available_undo_logs < TRX_SYS_N_RSEGS;
	     i++, srv_available_undo_logs++) {
		/* Tablespace 0 is the system tablespace.
		Dedicated undo log tablespaces start from 1. */
		uint32_t space = srv_undo_tablespaces > 0
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

	ut_ad(srv_available_undo_logs == TRX_SYS_N_RSEGS);

	ib::info info;
	info << srv_available_undo_logs;
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

	for (ulint i = 0; i < array_elements(temp_rsegs); ++i) {
		temp_rsegs[i].destroy();
	}
	for (ulint i = 0; i < array_elements(rseg_array); ++i) {
		rseg_array[i].destroy();
	}

	ut_a(trx_list.empty());
	trx_list.close();
	m_initialised = false;
}

/** @return total number of active (non-prepared) transactions */
ulint trx_sys_t::any_active_transactions()
{
  uint32_t total_trx= 0;

  trx_sys.trx_list.for_each([&total_trx](const trx_t &trx) {
    if (trx.state == TRX_STATE_COMMITTED_IN_MEMORY ||
        (trx.state == TRX_STATE_ACTIVE && trx.id))
      total_trx++;
  });

  return total_trx;
}
