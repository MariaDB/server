/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2023, MariaDB Corporation.

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

#ifdef UNIV_DEBUG
/* Flag to control TRX_RSEG_N_SLOTS behavior debugging. */
uint	trx_rseg_n_slots_debug = 0;

void rw_trx_hash_t::validate_element(trx_t *trx)
{
  ut_ad(!trx->read_only || !trx->rsegs.m_redo.rseg);
  ut_ad(!trx->is_autocommit_non_locking());
  ut_d(bool acquire_trx_mutex= !trx->mutex_is_owner());
  ut_d(if (acquire_trx_mutex) trx->mutex_lock());
  switch (trx->state) {
  case TRX_STATE_NOT_STARTED:
  case TRX_STATE_ABORTED:
    ut_error;
  case TRX_STATE_PREPARED:
  case TRX_STATE_PREPARED_RECOVERED:
  case TRX_STATE_COMMITTED_IN_MEMORY:
    ut_ad(!trx->is_autocommit_non_locking());
    break;
  case TRX_STATE_ACTIVE:
    if (!trx->is_autocommit_non_locking())
      break;
    ut_ad(!trx->is_recovered);
    ut_ad(trx->read_only);
    ut_ad(trx->mysql_thd);
  }
  ut_d(if (acquire_trx_mutex) trx->mutex_unlock());
}
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

/** Initialize the transaction system when creating the database. */
dberr_t trx_sys_create_sys_pages(mtr_t *mtr)
{
  mtr->x_lock_space(fil_system.sys_space);
  static_assert(TRX_SYS_SPACE == 0, "compatibility");

  /* Create the trx sys file block in a new allocated file segment */
  dberr_t err;
  buf_block_t *block= fseg_create(fil_system.sys_space,
                                  TRX_SYS + TRX_SYS_FSEG_HEADER, mtr, &err);
  if (UNIV_UNLIKELY(!block))
    return err;
  ut_a(block->page.id() == page_id_t(0, TRX_SYS_PAGE_NO));

  mtr->write<2>(*block, FIL_PAGE_TYPE + block->page.frame,
                FIL_PAGE_TYPE_TRX_SYS);

  /* Reset the rollback segment slots.  Old versions of InnoDB
  (before MySQL 5.5) define TRX_SYS_N_RSEGS as 256 and expect
  that the whole array is initialized. */
  static_assert(256 >= TRX_SYS_N_RSEGS, "");
  static_assert(TRX_SYS + TRX_SYS_RSEGS + 256 * TRX_SYS_RSEG_SLOT_SIZE <=
                UNIV_PAGE_SIZE_MIN - FIL_PAGE_DATA_END, "");
  mtr->write<4>(*block, TRX_SYS + TRX_SYS_RSEGS + TRX_SYS_RSEG_PAGE_NO +
                block->page.frame, FSP_FIRST_RSEG_PAGE_NO);
  mtr->memset(block, TRX_SYS + TRX_SYS_RSEGS + TRX_SYS_RSEG_SLOT_SIZE,
              255 * TRX_SYS_RSEG_SLOT_SIZE, 0xff);

  buf_block_t *r= trx_rseg_header_create(fil_system.sys_space, 0, 0,
                                         mtr, &err);
  if (UNIV_UNLIKELY(!r))
    return err;
  ut_a(r->page.id() == page_id_t(0, FSP_FIRST_RSEG_PAGE_NO));

  return trx_lists_init_at_db_start();
}

void trx_sys_t::create()
{
  ut_ad(this == &trx_sys);
  ut_ad(!is_initialised());
  m_initialised= true;
  trx_list.create();
  rw_trx_hash.init();
  for (auto &rseg : temp_rsegs)
    rseg.init(nullptr, FIL_NULL);
  for (auto &rseg : rseg_array)
    rseg.init(nullptr, FIL_NULL);
}

size_t trx_sys_t::history_size()
{
  ut_ad(is_initialised());
  size_t size= 0;
  for (auto &rseg : rseg_array)
  {
    rseg.latch.rd_lock(SRW_LOCK_CALL);
    size+= rseg.history_size;
  }
  for (auto &rseg : rseg_array)
    rseg.latch.rd_unlock();
  return size;
}

bool trx_sys_t::history_exceeds(size_t threshold)
{
  ut_ad(is_initialised());
  size_t size= 0;
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

TPOOL_SUPPRESS_TSAN size_t trx_sys_t::history_size_approx() const
{
  ut_ad(is_initialised());
  size_t size= 0;
  for (auto &rseg : rseg_array)
    size+= rseg.history_size;
  return size;
}

my_bool trx_sys_t::find_same_or_older_callback(void *el, void *i) noexcept
{
  auto element= static_cast<rw_trx_hash_element_t *>(el);
  auto id= static_cast<trx_id_t*>(i);
  return element->id <= *id;
}


bool trx_sys_t::find_same_or_older_low(trx_t *trx, trx_id_t id) noexcept
{
  return rw_trx_hash.iterate(trx, find_same_or_older_callback, &id);
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
    ut_ad(!space->is_temporary());
    ut_ad(!space->is_being_imported());
    if (buf_block_t *sys_header= trx_sysf_get(&mtr))
    {
      ulint rseg_id= trx_sys_rseg_find_free(sys_header);
      dberr_t err;
      if (buf_block_t *rblock= rseg_id == ULINT_UNDEFINED
          ? nullptr : trx_rseg_header_create(space, rseg_id, 0, &mtr, &err))
      {
        rseg= &trx_sys.rseg_array[rseg_id];
        rseg->destroy();
        rseg->init(space, rblock->page.id().page_no());
        ut_ad(rseg->is_persistent());
        mtr.write<4,mtr_t::MAYBE_NOP>
          (*sys_header, TRX_SYS + TRX_SYS_RSEGS + TRX_SYS_RSEG_SPACE +
           rseg_id * TRX_SYS_RSEG_SLOT_SIZE + sys_header->page.frame,
           space_id);
        mtr.write<4,mtr_t::MAYBE_NOP>
          (*sys_header, TRX_SYS + TRX_SYS_RSEGS + TRX_SYS_RSEG_PAGE_NO +
           rseg_id * TRX_SYS_RSEG_SLOT_SIZE + sys_header->page.frame,
           rseg->page_no);
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
		if (space > (srv_undo_space_id_start
			     + srv_undo_tablespaces_active - 1)) {
			srv_undo_tablespaces_active++;
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
	for (auto& rseg : temp_rsegs) rseg.destroy();
	for (auto& rseg : rseg_array) rseg.destroy();

	ut_a(trx_list.empty());
	trx_list.close();
	m_initialised = false;
}

/** @return total number of active (non-prepared) transactions */
size_t trx_sys_t::any_active_transactions(size_t *prepared)
{
  size_t total_trx= 0, prepared_trx= 0;

  trx_sys.trx_list.for_each([&](const trx_t &trx) {
    switch (trx.state) {
    case TRX_STATE_NOT_STARTED:
    case TRX_STATE_ABORTED:
      break;
    case TRX_STATE_ACTIVE:
      if (!trx.id)
        break;
      /* fall through */
    case TRX_STATE_COMMITTED_IN_MEMORY:
      total_trx++;
      break;
    case TRX_STATE_PREPARED:
    case TRX_STATE_PREPARED_RECOVERED:
      prepared_trx++;
    }
  });

  if (prepared)
    *prepared= prepared_trx;

  return total_trx;
}

#ifndef EMBEDDED_LIBRARY
/** @return true if any active (non-prepared) transactions is recovered */
bool trx_sys_t::any_active_transaction_recovered()
{
  return trx_sys.trx_list.find_first([&](trx_t &trx)
  {
    if (trx.state != TRX_STATE_ACTIVE)
      return false;

    bool found= false;
    trx.mutex_lock();
    found= trx.is_recovered;
    trx.mutex_unlock();
    return found;
  });
}
#endif
