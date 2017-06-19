/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
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
#include "read0read.h"
#include "fsp0sysspace.h"

#include <mysql/service_wsrep.h>

/** The transaction system */
trx_sys_t*		trx_sys;

/** Check whether transaction id is valid.
@param[in]	id              transaction id to check
@param[in]      name            table name */
void
ReadView::check_trx_id_sanity(
	trx_id_t		id,
	const table_name_t&	name)
{
	if (id >= trx_sys->max_trx_id) {

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

/*****************************************************************//**
Writes the value of max_trx_id to the file based trx system header. */
void
trx_sys_flush_max_trx_id(void)
/*==========================*/
{
	mtr_t		mtr;
	trx_sysf_t*	sys_header;

#ifndef WITH_WSREP
	/* wsrep_fake_trx_id  violates this assert
	Copied from trx_sys_get_new_trx_id
	*/
	ut_ad(trx_sys_mutex_own());
#endif /* WITH_WSREP */

	if (!srv_read_only_mode) {
		mtr_start(&mtr);

		sys_header = trx_sysf_get(&mtr);

		mlog_write_ull(
			sys_header + TRX_SYS_TRX_ID_STORE,
			trx_sys->max_trx_id, &mtr);

		mtr_commit(&mtr);
	}
}

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
	mtr_t*		mtr)	/*!< in: mtr */
{
	DBUG_PRINT("InnoDB",("trx_mysql_binlog_offset: %lld", (longlong) offset));

	if (ut_strlen(file_name) >= TRX_SYS_MYSQL_LOG_NAME_LEN) {

		/* We cannot fit the name to the 512 bytes we have reserved */

		return;
	}

	if (mach_read_from_4(sys_header + field
			     + TRX_SYS_MYSQL_LOG_MAGIC_N_FLD)
	    != TRX_SYS_MYSQL_LOG_MAGIC_N) {

		mlog_write_ulint(sys_header + field
				 + TRX_SYS_MYSQL_LOG_MAGIC_N_FLD,
				 TRX_SYS_MYSQL_LOG_MAGIC_N,
				 MLOG_4BYTES, mtr);
	}

	if (0 != strcmp((char*) (sys_header + field + TRX_SYS_MYSQL_LOG_NAME),
			file_name)) {

		mlog_write_string(sys_header + field
				  + TRX_SYS_MYSQL_LOG_NAME,
				  (byte*) file_name, 1 + ut_strlen(file_name),
				  mtr);
	}

	if (mach_read_from_4(sys_header + field
			     + TRX_SYS_MYSQL_LOG_OFFSET_HIGH) > 0
	    || (offset >> 32) > 0) {

		mlog_write_ulint(sys_header + field
				 + TRX_SYS_MYSQL_LOG_OFFSET_HIGH,
				 (ulint)(offset >> 32),
				 MLOG_4BYTES, mtr);
	}

	mlog_write_ulint(sys_header + field
			 + TRX_SYS_MYSQL_LOG_OFFSET_LOW,
			 (ulint)(offset & 0xFFFFFFFFUL),
			 MLOG_4BYTES, mtr);
}

/*****************************************************************//**
Stores the MySQL binlog offset info in the trx system header if
the magic number shows it valid, and print the info to stderr */
void
trx_sys_print_mysql_binlog_offset(void)
/*===================================*/
{
	trx_sysf_t*	sys_header;
	mtr_t		mtr;
	ulint		trx_sys_mysql_bin_log_pos_high;
	ulint		trx_sys_mysql_bin_log_pos_low;

	mtr_start(&mtr);

	sys_header = trx_sysf_get(&mtr);

	if (mach_read_from_4(sys_header + TRX_SYS_MYSQL_LOG_INFO
			     + TRX_SYS_MYSQL_LOG_MAGIC_N_FLD)
	    != TRX_SYS_MYSQL_LOG_MAGIC_N) {

		mtr_commit(&mtr);

		return;
	}

	trx_sys_mysql_bin_log_pos_high = mach_read_from_4(
		sys_header + TRX_SYS_MYSQL_LOG_INFO
		+ TRX_SYS_MYSQL_LOG_OFFSET_HIGH);
	trx_sys_mysql_bin_log_pos_low = mach_read_from_4(
		sys_header + TRX_SYS_MYSQL_LOG_INFO
		+ TRX_SYS_MYSQL_LOG_OFFSET_LOW);

	fprintf(stderr,
		"InnoDB: Last MySQL binlog file position " ULINTPF " " ULINTPF
		", file name %s\n",
		trx_sys_mysql_bin_log_pos_high, trx_sys_mysql_bin_log_pos_low,
		sys_header + TRX_SYS_MYSQL_LOG_INFO
		+ TRX_SYS_MYSQL_LOG_NAME);

	mtr_commit(&mtr);
}

#ifdef WITH_WSREP

#ifdef UNIV_DEBUG
static long long trx_sys_cur_xid_seqno = -1;
static unsigned char trx_sys_cur_xid_uuid[16];

long long read_wsrep_xid_seqno(const XID* xid)
{
	long long seqno;
	memcpy(&seqno, xid->data + 24, sizeof(long long));
	return seqno;
}

void read_wsrep_xid_uuid(const XID* xid, unsigned char* buf)
{
	memcpy(buf, xid->data + 8, 16);
}

#endif /* UNIV_DEBUG */

void
trx_sys_update_wsrep_checkpoint(
/*============================*/
	const XID*	xid,		/*!< in: transaction XID */
	trx_sysf_t*	sys_header,	/*!< in: sys_header */
	mtr_t*		mtr)		/*!< in: mtr */
{
#ifdef UNIV_DEBUG
	{
		/* Check that seqno is monotonically increasing */
		unsigned char xid_uuid[16];
		long long xid_seqno = read_wsrep_xid_seqno(xid);
		read_wsrep_xid_uuid(xid, xid_uuid);
		if (!memcmp(xid_uuid, trx_sys_cur_xid_uuid, 16))
		{
			/*
			This check is a protection against the initial seqno (-1)
			assigned in read_wsrep_xid_uuid(), which, if not checked,
			would cause the following assertion to fail.
			*/
			if (xid_seqno > -1 )
			{
				ut_ad(xid_seqno > trx_sys_cur_xid_seqno);
			}
		} else {
			memcpy(trx_sys_cur_xid_uuid, xid_uuid, 16);
		}
		trx_sys_cur_xid_seqno = xid_seqno;
	}
#endif /* UNIV_DEBUG */

		ut_ad(xid && mtr);
		ut_a(xid->formatID == -1 || wsrep_is_wsrep_xid(xid));

		if (mach_read_from_4(sys_header + TRX_SYS_WSREP_XID_INFO
				+ TRX_SYS_WSREP_XID_MAGIC_N_FLD)
			!= TRX_SYS_WSREP_XID_MAGIC_N) {
			mlog_write_ulint(sys_header + TRX_SYS_WSREP_XID_INFO
				+ TRX_SYS_WSREP_XID_MAGIC_N_FLD,
				TRX_SYS_WSREP_XID_MAGIC_N,
				MLOG_4BYTES, mtr);
		}

		mlog_write_ulint(sys_header + TRX_SYS_WSREP_XID_INFO
			+ TRX_SYS_WSREP_XID_FORMAT,
			(int)xid->formatID,
			MLOG_4BYTES, mtr);
		mlog_write_ulint(sys_header + TRX_SYS_WSREP_XID_INFO
			+ TRX_SYS_WSREP_XID_GTRID_LEN,
			(int)xid->gtrid_length,
			MLOG_4BYTES, mtr);
		mlog_write_ulint(sys_header + TRX_SYS_WSREP_XID_INFO
			+ TRX_SYS_WSREP_XID_BQUAL_LEN,
			(int)xid->bqual_length,
			MLOG_4BYTES, mtr);
		mlog_write_string(sys_header + TRX_SYS_WSREP_XID_INFO
			+ TRX_SYS_WSREP_XID_DATA,
			(const unsigned char*) xid->data,
			XIDDATASIZE, mtr);

}

void
trx_sys_read_wsrep_checkpoint(
/*==========================*/
	XID* xid)
{
	trx_sysf_t*	sys_header;
	mtr_t		mtr;
	ulint		magic;

	ut_ad(xid);

	mtr_start(&mtr);

	sys_header = trx_sysf_get(&mtr);

	if ((magic = mach_read_from_4(sys_header + TRX_SYS_WSREP_XID_INFO
					+ TRX_SYS_WSREP_XID_MAGIC_N_FLD))
	    != TRX_SYS_WSREP_XID_MAGIC_N) {
		memset(xid, 0, sizeof(*xid));
		long long seqno= -1;
		memcpy(xid->data + 24, &seqno, sizeof(long long));
		xid->formatID = -1;
		trx_sys_update_wsrep_checkpoint(xid, sys_header, &mtr);
		mtr_commit(&mtr);
		return;
	}

	xid->formatID = (int)mach_read_from_4(
			sys_header
			+ TRX_SYS_WSREP_XID_INFO + TRX_SYS_WSREP_XID_FORMAT);
	xid->gtrid_length = (int)mach_read_from_4(
			sys_header
			+ TRX_SYS_WSREP_XID_INFO + TRX_SYS_WSREP_XID_GTRID_LEN);
	xid->bqual_length = (int)mach_read_from_4(
			sys_header
			+ TRX_SYS_WSREP_XID_INFO + TRX_SYS_WSREP_XID_BQUAL_LEN);
	ut_memcpy(xid->data,
		  sys_header + TRX_SYS_WSREP_XID_INFO + TRX_SYS_WSREP_XID_DATA,
		  XIDDATASIZE);

	mtr_commit(&mtr);
}

#endif /* WITH_WSREP */

/** @return an unallocated rollback segment slot in the TRX_SYS header
@retval ULINT_UNDEFINED if not found */
ulint
trx_sysf_rseg_find_free(mtr_t* mtr)
{
	trx_sysf_t*	sys_header = trx_sysf_get(mtr);

	for (ulint i = 0; i < TRX_SYS_N_RSEGS; i++) {
		if (trx_sysf_rseg_get_page_no(sys_header, i, mtr)
		    == FIL_NULL) {
			return(i);
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

	trx_sysf_t*	sys_header	= trx_sysf_get(&mtr);
	srv_available_undo_logs = 0;

	for (ulint i = 0; i < TRX_SYS_N_RSEGS; i++) {
		srv_available_undo_logs
			+= trx_sysf_rseg_get_page_no(sys_header, i, &mtr)
			!= FIL_NULL;
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
	trx_sysf_t*	sys_header;
	ulint		slot_no;
	buf_block_t*	block;
	page_t*		page;
	ulint		page_no;
	byte*		ptr;

	ut_ad(mtr);

	/* Note that below we first reserve the file space x-latch, and
	then enter the kernel: we must do it in this order to conform
	to the latching order rules. */

	mtr_x_lock_space(TRX_SYS_SPACE, mtr);

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

	sys_header = trx_sysf_get(mtr);

	/* Start counting transaction ids from number 1 up */
	mach_write_to_8(sys_header + TRX_SYS_TRX_ID_STORE, 1);

	/* Reset the rollback segment slots.  Old versions of InnoDB
	(before MySQL 5.5) define TRX_SYS_N_RSEGS as 256 and expect
	that the whole array is initialized. */
	ptr = TRX_SYS_RSEGS + sys_header;
	compile_time_assert(256 >= TRX_SYS_N_RSEGS);
	memset(ptr, 0xff, 256 * TRX_SYS_RSEG_SLOT_SIZE);
	ptr += 256 * TRX_SYS_RSEG_SLOT_SIZE;
	ut_a(ptr <= page + (UNIV_PAGE_SIZE - FIL_PAGE_DATA_END));

	/* Initialize all of the page.  This part used to be uninitialized. */
	memset(ptr, 0, UNIV_PAGE_SIZE - FIL_PAGE_DATA_END + page - ptr);

	mlog_log_string(sys_header, UNIV_PAGE_SIZE - FIL_PAGE_DATA_END
			+ page - sys_header, mtr);

	/* Create the first rollback segment in the SYSTEM tablespace */
	slot_no = trx_sysf_rseg_find_free(mtr);
	page_no = trx_rseg_header_create(TRX_SYS_SPACE,
					 ULINT_MAX, slot_no, mtr);

	ut_a(slot_no == TRX_SYS_SYSTEM_RSEG_ID);
	ut_a(page_no == FSP_FIRST_RSEG_PAGE_NO);
}

/** Initialize the transaction system main-memory data structures. */
void
trx_sys_init_at_db_start()
{
	trx_sysf_t*	sys_header;
	ib_uint64_t	rows_to_undo	= 0;
	const char*	unit		= "";

	/* VERY important: after the database is started, max_trx_id value is
	divisible by TRX_SYS_TRX_ID_WRITE_MARGIN, and the 'if' in
	trx_sys_get_new_trx_id will evaluate to TRUE when the function
	is first time called, and the value for trx id will be written
	to the disk-based header! Thus trx id values will not overlap when
	the database is repeatedly started! */

	mtr_t	mtr;
	mtr.start();

	sys_header = trx_sysf_get(&mtr);

	trx_sys->max_trx_id = 2 * TRX_SYS_TRX_ID_WRITE_MARGIN
		+ ut_uint64_align_up(mach_read_from_8(sys_header
						   + TRX_SYS_TRX_ID_STORE),
				     TRX_SYS_TRX_ID_WRITE_MARGIN);

	mtr.commit();
	ut_d(trx_sys->rw_max_trx_id = trx_sys->max_trx_id);

	trx_dummy_sess = sess_open();

	trx_lists_init_at_db_start();

	/* This mutex is not strictly required, it is here only to satisfy
	the debug code (assertions). We are still running in single threaded
	bootstrap mode. */

	trx_sys_mutex_enter();

	if (UT_LIST_GET_LEN(trx_sys->rw_trx_list) > 0) {
		const trx_t*	trx;

		for (trx = UT_LIST_GET_FIRST(trx_sys->rw_trx_list);
		     trx != NULL;
		     trx = UT_LIST_GET_NEXT(trx_list, trx)) {

			ut_ad(trx->is_recovered);
			assert_trx_in_rw_list(trx);

			if (trx_state_eq(trx, TRX_STATE_ACTIVE)) {
				rows_to_undo += trx->undo_no;
			}
		}

		if (rows_to_undo > 1000000000) {
			unit = "M";
			rows_to_undo = rows_to_undo / 1000000;
		}

		ib::info() << UT_LIST_GET_LEN(trx_sys->rw_trx_list)
			<< " transaction(s) which must be rolled back or"
			" cleaned up in total " << rows_to_undo << unit
			<< " row operations to undo";

		ib::info() << "Trx id counter is " << trx_sys->max_trx_id;
	}

	trx_sys_mutex_exit();

	trx_sys->mvcc->clone_oldest_view(&purge_sys->view);
}

/*****************************************************************//**
Creates the trx_sys instance and initializes purge_queue and mutex. */
void
trx_sys_create(void)
/*================*/
{
	ut_ad(trx_sys == NULL);

	trx_sys = static_cast<trx_sys_t*>(ut_zalloc_nokey(sizeof(*trx_sys)));

	mutex_create(LATCH_ID_TRX_SYS, &trx_sys->mutex);

	UT_LIST_INIT(trx_sys->serialisation_list, &trx_t::no_list);
	UT_LIST_INIT(trx_sys->rw_trx_list, &trx_t::trx_list);
	UT_LIST_INIT(trx_sys->mysql_trx_list, &trx_t::mysql_trx_list);

	trx_sys->mvcc = UT_NEW_NOKEY(MVCC(1024));

	new(&trx_sys->rw_trx_ids) trx_ids_t(ut_allocator<trx_id_t>(
			mem_key_trx_sys_t_rw_trx_ids));

	new(&trx_sys->rw_trx_set) TrxIdSet();
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

/*********************************************************************
Shutdown/Close the transaction system. */
void
trx_sys_close(void)
/*===============*/
{
	ut_ad(trx_sys != NULL);
	ut_ad(srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS);

	if (ulint size = trx_sys->mvcc->size()) {
		ib::error() << "All read views were not closed before"
			" shutdown: " << size << " read views open";
	}

	if (trx_dummy_sess) {
		sess_close(trx_dummy_sess);
		trx_dummy_sess = NULL;
	}

	/* Only prepared transactions may be left in the system. Free them. */
	ut_a(UT_LIST_GET_LEN(trx_sys->rw_trx_list) == trx_sys->n_prepared_trx
	     || !srv_was_started
	     || srv_read_only_mode
	     || srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO);

	for (trx_t* trx = UT_LIST_GET_FIRST(trx_sys->rw_trx_list);
	     trx != NULL;
	     trx = UT_LIST_GET_FIRST(trx_sys->rw_trx_list)) {

		trx_free_prepared(trx);

		UT_LIST_REMOVE(trx_sys->rw_trx_list, trx);
	}

	/* There can't be any active transactions. */

	for (ulint i = 0; i < TRX_SYS_N_RSEGS; ++i) {
		if (trx_rseg_t* rseg = trx_sys->rseg_array[i]) {
			trx_rseg_mem_free(rseg);
		}

		if (trx_rseg_t* rseg = trx_sys->temp_rsegs[i]) {
			trx_rseg_mem_free(rseg);
		}
	}

	UT_DELETE(trx_sys->mvcc);

	ut_a(UT_LIST_GET_LEN(trx_sys->rw_trx_list) == 0);
	ut_a(UT_LIST_GET_LEN(trx_sys->mysql_trx_list) == 0);
	ut_a(UT_LIST_GET_LEN(trx_sys->serialisation_list) == 0);

	/* We used placement new to create this mutex. Call the destructor. */
	mutex_free(&trx_sys->mutex);

	trx_sys->rw_trx_ids.~trx_ids_t();

	trx_sys->rw_trx_set.~TrxIdSet();

	ut_free(trx_sys);

	trx_sys = NULL;
}

/*********************************************************************
Check if there are any active (non-prepared) transactions.
@return total number of active transactions or 0 if none */
ulint
trx_sys_any_active_transactions(void)
/*=================================*/
{
	ulint	total_trx = 0;

	trx_sys_mutex_enter();

	total_trx = UT_LIST_GET_LEN(trx_sys->rw_trx_list)
		  + UT_LIST_GET_LEN(trx_sys->mysql_trx_list);

	ut_a(total_trx >= trx_sys->n_prepared_trx);
	total_trx -= trx_sys->n_prepared_trx;

	trx_sys_mutex_exit();

	return(total_trx);
}

#ifdef UNIV_DEBUG
/*************************************************************//**
Validate the trx_ut_list_t.
@return true if valid. */
static
bool
trx_sys_validate_trx_list_low(
/*===========================*/
	trx_ut_list_t*	trx_list)	/*!< in: &trx_sys->rw_trx_list */
{
	const trx_t*	trx;
	const trx_t*	prev_trx = NULL;

	ut_ad(trx_sys_mutex_own());

	ut_ad(trx_list == &trx_sys->rw_trx_list);

	for (trx = UT_LIST_GET_FIRST(*trx_list);
	     trx != NULL;
	     prev_trx = trx, trx = UT_LIST_GET_NEXT(trx_list, prev_trx)) {

		check_trx_state(trx);
		ut_a(prev_trx == NULL || prev_trx->id > trx->id);
	}

	return(true);
}

/*************************************************************//**
Validate the trx_sys_t::rw_trx_list.
@return true if the list is valid. */
bool
trx_sys_validate_trx_list()
/*=======================*/
{
	ut_ad(trx_sys_mutex_own());

	ut_a(trx_sys_validate_trx_list_low(&trx_sys->rw_trx_list));

	return(true);
}
#endif /* UNIV_DEBUG */
