/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All rights reserved.
Copyright (c) 2009, Percona Inc.
Copyright (c) 2013, 2023, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted
by Percona Inc.. Those modifications are
gratefully acknowledged and are described briefly in the InnoDB
documentation. The contributions by Percona Inc. are incorporated with
their permission, and subject to the conditions contained in the file
COPYING.Percona.

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

/********************************************************************//**
@file srv/srv0start.cc
Starts the InnoDB database server

Created 2/16/1996 Heikki Tuuri
*************************************************************************/

#include "my_global.h"

#include "mysqld.h"
#include "mysql/psi/mysql_stage.h"
#include "mysql/psi/psi.h"

#include "row0ftsort.h"
#include "ut0mem.h"
#include "mem0mem.h"
#include "data0data.h"
#include "data0type.h"
#include "dict0dict.h"
#include "buf0buf.h"
#include "buf0dblwr.h"
#include "buf0dump.h"
#include "os0file.h"
#include "fil0fil.h"
#include "fil0crypt.h"
#include "fsp0fsp.h"
#include "rem0rec.h"
#include "mtr0mtr.h"
#include "log0crypt.h"
#include "log0recv.h"
#include "page0page.h"
#include "page0cur.h"
#include "trx0trx.h"
#include "trx0sys.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "rem0rec.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "mysql/service_wsrep.h" /* wsrep_recovery */
#include "trx0rseg.h"
#include "buf0flu.h"
#include "buf0rea.h"
#include "dict0boot.h"
#include "dict0load.h"
#include "dict0stats_bg.h"
#include "que0que.h"
#include "lock0lock.h"
#include "trx0roll.h"
#include "trx0purge.h"
#include "lock0lock.h"
#include "pars0pars.h"
#include "btr0sea.h"
#include "rem0cmp.h"
#include "dict0crea.h"
#include "row0ins.h"
#include "row0sel.h"
#include "row0upd.h"
#include "row0row.h"
#include "row0mysql.h"
#include "btr0pcur.h"
#include "ibuf0ibuf.h"
#include "innodb_binlog.h"
#include "zlib.h"
#include "log.h"

/** Log sequence number at shutdown */
lsn_t	srv_shutdown_lsn;

/** TRUE if a raw partition is in use */
ibool	srv_start_raw_disk_in_use;

/** UNDO tablespaces starts with space id. */
uint32_t srv_undo_space_id_start;

/** TRUE if the server is being started, before rolling back any
incomplete transactions */
bool	srv_startup_is_before_trx_rollback_phase;
/** TRUE if the server is being started */
bool	srv_is_being_started;
/** TRUE if the server was successfully started */
bool	srv_was_started;
/** whether srv_start() has been called */
static bool		srv_start_has_been_called;

/** Whether any undo log records can be generated */
bool	srv_undo_sources;

/** innodb_encrypt_log */
my_bool srv_encrypt_log;

#ifdef UNIV_DEBUG
/** InnoDB system tablespace to set during recovery */
uint	srv_sys_space_size_debug;
/** whether redo log file have been created at startup */
bool	srv_log_file_created;
#endif /* UNIV_DEBUG */

/** whether some background threads that create redo log have been started */
static bool srv_started_redo;

/** At a shutdown this value climbs from SRV_SHUTDOWN_NONE to
SRV_SHUTDOWN_CLEANUP and then to SRV_SHUTDOWN_LAST_PHASE, and so on */
enum srv_shutdown_t	srv_shutdown_state = SRV_SHUTDOWN_NONE;

/** Name of srv_monitor_file */
static char*	srv_monitor_file_name;
std::unique_ptr<tpool::timer> srv_master_timer;

/** */
#define SRV_MAX_N_PENDING_SYNC_IOS	100

#ifdef UNIV_PFS_THREAD
/* Keys to register InnoDB threads with performance schema */
mysql_pfs_key_t	thread_pool_thread_key;
#endif /* UNIV_PFS_THREAD */

#ifdef HAVE_PSI_STAGE_INTERFACE
/** Array of all InnoDB stage events for monitoring activities via
performance schema. */
static PSI_stage_info*	srv_stages[] =
{
	&srv_stage_alter_table_end,
	&srv_stage_alter_table_insert,
	&srv_stage_alter_table_log_index,
	&srv_stage_alter_table_log_table,
	&srv_stage_alter_table_merge_sort,
	&srv_stage_alter_table_read_pk_internal_sort,
	&srv_stage_buffer_pool_load,
};
#endif /* HAVE_PSI_STAGE_INTERFACE */

/** Delete any garbage log files */
static void delete_log_files()
{
  for (size_t i= 1; i < 102; i++)
    delete_log_file(std::to_string(i).c_str());
}

/** Creates log file.
@param create_new_db   whether the database is being initialized
@param lsn             log sequence number
@param logfile0        name of the log file
@return DB_SUCCESS or error code */
static dberr_t create_log_file(bool create_new_db, lsn_t lsn)
{
	ut_ad(!srv_read_only_mode);

	/* We will retain ib_logfile0 until we have written a new logically
	empty log as ib_logfile101 and atomically renamed it to
	ib_logfile0 in log_t::resize_rename(). */
	delete_log_files();

	ut_ad(!os_aio_pending_reads());
	ut_d(mysql_mutex_lock(&buf_pool.flush_list_mutex));
	ut_ad(!buf_pool.get_oldest_modification(0));
	ut_d(mysql_mutex_unlock(&buf_pool.flush_list_mutex));
	/* os_aio_pending_writes() may hold here if some
	write_io_callback() did not release the slot yet.  However,
	the page write itself must have completed, because the
	buf_pool.flush_list is empty. In debug builds, we wait for
	this to happen, hoping to get a hung process if this
	assumption does not hold. */
	ut_d(os_aio_wait_until_no_pending_writes(false));

	log_sys.latch.wr_lock(SRW_LOCK_CALL);
	log_sys.set_capacity();

	std::string logfile0{get_log_file_path("ib_logfile101")};
	bool ret;
	os_file_t file{
          os_file_create_func(logfile0.c_str(),
                              OS_FILE_CREATE,
                              OS_LOG_FILE, false, &ret)
        };

	if (!ret) {
		sql_print_error("InnoDB: Cannot create %.*s",
				int(logfile0.size()), logfile0.data());
err_exit:
		log_sys.latch.wr_unlock();
		return DB_ERROR;
	}

	ret = os_file_set_size(logfile0.c_str(), file, srv_log_file_size);
	if (!ret) {
		ib::error() << "Cannot set log file " << logfile0
			    << " size to " << ib::bytes_iec{srv_log_file_size};
close_and_exit:
		os_file_close_func(file);
		goto err_exit;
	}

	log_sys.set_latest_format(srv_encrypt_log);
	if (!log_sys.attach(file, srv_log_file_size)) {
		goto close_and_exit;
	}

	mysql_mutex_lock(&recv_sys.mutex);
	const bool all_opened = fil_system.sys_space->open(create_new_db);
	mysql_mutex_unlock(&recv_sys.mutex);

	if (!all_opened) {
		goto err_exit;
	}

	/* Create a log checkpoint. */
	if (log_sys.is_encrypted() && !log_crypt_init()) {
		goto err_exit;
	}
	ut_d(recv_no_log_write = false);
	log_sys.create(lsn);

	ut_ad(srv_startup_is_before_trx_rollback_phase);
	if (create_new_db) {
		srv_startup_is_before_trx_rollback_phase = false;
	}

	/* Enable log_checkpoint() in buf_flush_page_cleaner().
	If we are upgrading or resizing the log at startup, we must not
	write any FILE_MODIFY or FILE_CHECKPOINT records to the old log file
	because the ib_logfile0 could be in an old format that we can only
	check for emptiness, or the write could lead to an overflow.

	At this point, we are not holding recv_sys.mutex, but there are no
	pending page reads, and buf_pool.flush_list is empty. Therefore,
	we can clear the flag without risking any race condition with
	buf_page_t::read_complete(). */
	recv_sys.recovery_on = false;
	log_sys.latch.wr_unlock();

	log_make_checkpoint();
	log_buffer_flush_to_disk();

	return DB_SUCCESS;
}

/** Rename the redo log file after resizing.
@return whether an error occurred */
bool log_t::resize_rename() noexcept
{
  std::string old_name{get_log_file_path("ib_logfile101")};
  std::string new_name{get_log_file_path()};

  if (IF_WIN(MoveFileEx(old_name.c_str(), new_name.c_str(),
                        MOVEFILE_REPLACE_EXISTING),
             !rename(old_name.c_str(), new_name.c_str())))
    return false;

  sql_print_error("InnoDB: Failed to rename log from %.*s to %.*s (error %d)",
                  int(old_name.size()), old_name.data(),
                  int(new_name.size()), new_name.data(),
                  IF_WIN(int(GetLastError()), errno));
  return true;
}

/** Create an undo tablespace file
@param[in] name	 file name
@return DB_SUCCESS or error code */
static dberr_t srv_undo_tablespace_create(const char* name)
{
	pfs_os_file_t	fh;
	bool		ret;
	dberr_t		err = DB_SUCCESS;

	os_file_create_subdirs_if_needed(name);

	fh = os_file_create(
		innodb_data_file_key,
		name,
		srv_read_only_mode ? OS_FILE_OPEN : OS_FILE_CREATE,
		OS_DATA_FILE, srv_read_only_mode, &ret);

	if (!ret) {
		if (os_file_get_last_error(false) != OS_FILE_ALREADY_EXISTS
#ifdef _AIX
			/* AIX 5.1 after security patch ML7 may have
			errno set to 0 here, which causes our function
			to return 100; work around that AIX problem */
		    && os_file_get_last_error(false) != 100
#endif
		) {
			ib::error() << "Can't create UNDO tablespace "
				<< name;
		}
		err = DB_ERROR;
	} else if (srv_read_only_mode) {
		ib::info() << name << " opened in read-only mode";
	} else {
		/* We created the data file and now write it full of zeros */

		ib::info() << "Data file " << name << " did not exist: new to"
			" be created";

		ib::info() << "Setting file " << name << " size to "
			<< ib::bytes_iec{SRV_UNDO_TABLESPACE_SIZE_IN_PAGES
					 << srv_page_size_shift};

		ib::info() << "Database physically writes the file full: "
			<< "wait...";

		if (!os_file_set_size(name, fh, os_offset_t
				      {SRV_UNDO_TABLESPACE_SIZE_IN_PAGES}
				      << srv_page_size_shift)) {
			ib::error() << "Unable to allocate " << name;
			err = DB_ERROR;
		}

		os_file_close(fh);
	}

	return(err);
}

inline dberr_t trx_sys_t::reset_page(mtr_t *mtr)
{
  dberr_t err= DB_SUCCESS;
  buf_block_t *sys_header= buf_page_get_gen(
    page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO), 0, RW_X_LATCH, nullptr,
    BUF_GET, mtr, &err);

  if (!sys_header) return err;

  if (mach_read_from_4(sys_header->page.frame + TRX_SYS +
                       TRX_SYS_FSEG_HEADER) != TRX_SYS_SPACE)
    return DB_CORRUPTION;

  /* Store the TRX_SYS_FSEG_HEADER page, offset */
  char fseg_addr[6];
  memcpy(fseg_addr,
         sys_header->page.frame + TRX_SYS + TRX_SYS_FSEG_HEADER + 4, 6);

  const bool dblwr_enabled=
    mach_read_from_4(TRX_SYS_DOUBLEWRITE_MAGIC + TRX_SYS_DOUBLEWRITE +
                     sys_header->page.frame)
    == TRX_SYS_DOUBLEWRITE_MAGIC_N;

  char doublewrite[TRX_SYS_DOUBLEWRITE_BLOCK2 + 4];
  memcpy(doublewrite, TRX_SYS_DOUBLEWRITE + sys_header->page.frame,
         sizeof doublewrite);

  fsp_init_file_page(fil_system.sys_space, sys_header, mtr);

  mtr->write<2>(*sys_header, FIL_PAGE_TYPE + sys_header->page.frame,
                FIL_PAGE_TYPE_TRX_SYS);

  DBUG_EXECUTE_IF("sys_fseg_header_fail",
                  {
                    fseg_addr[4]= 0;
                    fseg_addr[5]= 0;
                  });

  /** Write the TRX_SYS_FSEG_HEADER only if it is not zero-filled */
  if (!memcmp(fseg_addr + 4, "00", 2))
    mtr->memcpy(*sys_header,
                sys_header->page.frame + TRX_SYS + TRX_SYS_FSEG_HEADER + 4,
                fseg_addr, 6);
  mtr->write<4>(*sys_header,
                TRX_SYS + TRX_SYS_RSEGS + TRX_SYS_RSEG_PAGE_NO +
                sys_header->page.frame, FSP_FIRST_RSEG_PAGE_NO);
  mtr->memset(sys_header,
             TRX_SYS + TRX_SYS_RSEGS + TRX_SYS_RSEG_SLOT_SIZE,
	     254 * TRX_SYS_RSEG_SLOT_SIZE, 0xff);

  static_assert(TRX_SYS_RSEG_SLOT_SIZE == 8, "");

  if (dblwr_enabled)
  {
    mtr->memcpy(
      *sys_header, sys_header->page.frame + TRX_SYS_DOUBLEWRITE,
      doublewrite, sizeof doublewrite);
    mtr->memmove(
      *sys_header,
      TRX_SYS_DOUBLEWRITE + FSEG_HEADER_SIZE + TRX_SYS_DOUBLEWRITE_REPEAT,
      TRX_SYS_DOUBLEWRITE + FSEG_HEADER_SIZE, 12);
    memcpy(
      sys_header->page.frame + TRX_SYS_DOUBLEWRITE
      + FSEG_HEADER_SIZE + TRX_SYS_DOUBLEWRITE_REPEAT,
      sys_header->page.frame + TRX_SYS_DOUBLEWRITE + FSEG_HEADER_SIZE, 12);
    mtr->write<4>(
      *sys_header,
      TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED +
      sys_header->page.frame,
      TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N);
  }

  return DB_SUCCESS;
}

/** Delete the old undo tablespaces present in the undo log directory */
static dberr_t srv_undo_delete_old_tablespaces()
{
  /* Delete the old undo tablespaces*/
  for (uint32_t i= 0; i < srv_undo_tablespaces_open; ++i)
    fil_close_tablespace(srv_undo_space_id_start + i);

  DBUG_EXECUTE_IF("after_deleting_old_undo_abort", return DB_ERROR;);

  /* Do checkpoint to get rid of old undo log tablespaces redo logs */
  log_make_checkpoint();

  DBUG_EXECUTE_IF("after_deleting_old_undo_success", return DB_ERROR;);

  return DB_SUCCESS;
}

/** Recreate the undo log tablespaces */
ATTRIBUTE_COLD static dberr_t srv_undo_tablespaces_reinit()
{
  mtr_t mtr;
  dberr_t err;
  buf_block_t *first_rseg_hdr;
  uint32_t latest_space_id;

  mtr.start();

  buf_block_t *dict_hdr= buf_page_get_gen(
    page_id_t(DICT_HDR_SPACE, DICT_HDR_PAGE_NO), 0, RW_X_LATCH,
    nullptr, BUF_GET, &mtr, &err);

  if (!dict_hdr)
    goto func_exit;

  /* Assign the new space id for the first undo tablespace */
  latest_space_id= mach_read_from_4(
    DICT_HDR + DICT_HDR_MAX_SPACE_ID + dict_hdr->page.frame);

  if (latest_space_id + srv_undo_tablespaces > SRV_SPACE_ID_UPPER_BOUND)
  {
    err= DB_ERROR;
    sql_print_error("InnoDB: Running out of tablespace id");
    goto func_exit;
  }

  first_rseg_hdr=
    buf_page_get_gen(trx_sys.rseg_array[0].page_id(), 0, RW_X_LATCH,
                     nullptr, BUF_GET, &mtr, &err);
  if (!first_rseg_hdr)
    goto func_exit;

  if (UNIV_UNLIKELY(mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT +
                                     first_rseg_hdr->page.frame)))
    trx_rseg_format_upgrade(first_rseg_hdr, &mtr);

  mtr.write<8,mtr_t::MAYBE_NOP>(*first_rseg_hdr,
                                TRX_RSEG + TRX_RSEG_MAX_TRX_ID +
                                first_rseg_hdr->page.frame,
                                trx_sys.get_max_trx_id() - 1);

  /* Reset TRX_SYS page */
  err= trx_sys.reset_page(&mtr);

  if (err)
    goto func_exit;

  if (srv_undo_tablespaces_open == 0)
  {
    /* Free the system rollback segment */
    for (ulint i= 1; i < TRX_SYS_N_RSEGS; i++)
    {
      trx_rseg_t *rseg= &trx_sys.rseg_array[i];
      if (rseg->space != fil_system.sys_space)
        continue;
      buf_block_t *block= buf_page_get_gen(
        rseg->page_id(), 0, RW_X_LATCH, nullptr, BUF_GET, &mtr);
      if (!block) break;
      while (!fseg_free_step(block, TRX_RSEG + TRX_RSEG_FSEG_HEADER, &mtr));
    }
  }

  for (ulint rseg_id= 1; rseg_id < TRX_SYS_N_RSEGS; rseg_id++)
  {
    trx_rseg_t *rseg= &trx_sys.rseg_array[rseg_id];
    rseg->destroy();
    rseg->init(nullptr, FIL_NULL);
  }

  if (*trx_sys.recovered_binlog_filename
#ifdef WITH_WSREP
      || !trx_sys.recovered_wsrep_xid.is_null()
#endif /* WITH_WSREP */
     )
  {
    /* Update binlog offset, binlog file name & wsrep xid in
    system tablespace rollback segment */
    if (*trx_sys.recovered_binlog_filename)
    {
      ut_d(const size_t len = strlen(trx_sys.recovered_binlog_filename) + 1);
      ut_ad(len > 1);
      ut_ad(len <= TRX_RSEG_BINLOG_NAME_LEN);
      trx_rseg_update_binlog_offset(
        first_rseg_hdr, trx_sys.recovered_binlog_filename,
        trx_sys.recovered_binlog_offset, &mtr);
    }

#ifdef WITH_WSREP
    if (!trx_sys.recovered_wsrep_xid.is_null())
      trx_rseg_update_wsrep_checkpoint(
        first_rseg_hdr, &trx_sys.recovered_wsrep_xid, &mtr);
#endif /* WITH_WSREP */
  }

  dict_hdr->page.fix();

  mtr.commit();

  DBUG_EXECUTE_IF("after_rseg_reset_abort",
                  log_write_up_to(mtr.commit_lsn(), true);
                  dict_hdr->page.unfix();
                  return DB_ERROR;);

  sql_print_information(
    "InnoDB: Reinitializing innodb_undo_tablespaces= %u from %u",
    srv_undo_tablespaces, srv_undo_tablespaces_open);

  /* Delete the old undo tablespaces */
  err= srv_undo_delete_old_tablespaces();
  if (err)
  {
    dict_hdr->page.unfix();
    return err;
  }

  mtr.start();

  dict_hdr->page.lock.x_lock();
  mtr.memo_push(dict_hdr, MTR_MEMO_PAGE_X_FIX);

  if (srv_undo_tablespaces == 0)
  {
    srv_undo_space_id_start= 0;
    srv_undo_tablespaces_open= 0;
    goto func_exit;
  }

  srv_undo_space_id_start= latest_space_id;
  if (fil_assign_new_space_id(&srv_undo_space_id_start))
    mtr.write<4>(*dict_hdr, DICT_HDR + DICT_HDR_MAX_SPACE_ID
                 + dict_hdr->page.frame, srv_undo_space_id_start);

  /* Re-create the new undo tablespaces */
  err= srv_undo_tablespaces_init(true, &mtr);
func_exit:
  mtr.commit();

  DBUG_EXECUTE_IF("after_reinit_undo_abort",
                  log_write_up_to(mtr.commit_lsn(), true);
                  err= DB_ERROR;);

  if (err == DB_SUCCESS)
  {
    /* Usually, recovery must work no matter when
    log_checkpoints are triggered. This is a special case,
    because this code is executed as part of InnoDB startup.
    Backup requires that the server has been started up,
    backup should never observe the log records that
    were written in mtr and also srv_undo_tablespaces_init()
    initializes the undo tablespace start id based on page0
    content before reading the redo log */
    log_make_checkpoint();

    DBUG_EXECUTE_IF("after_reinit_undo_success", err= DB_ERROR;);
    srv_undo_tablespaces_active= srv_undo_tablespaces;
  }
  return err;
}

/** Reinitialize the undo tablespaces when there is no undo log
left to purge/rollback and validate the number of undo opened
undo tablespace and user given undo tablespace
@return DB_SUCCESS if it is valid */
static dberr_t srv_undo_tablespaces_reinitialize()
{

  /* Re-create the undo tablespaces if it has no undo logs
  left to purge/rollback */
  if (srv_undo_tablespaces != srv_undo_tablespaces_open &&
      trx_sys.is_undo_empty())
    return srv_undo_tablespaces_reinit();

  /* If the user says that there are fewer than what we find we
  tolerate that discrepancy but not the inverse. Because there could
  be unused undo tablespaces for future use. */

  if (srv_undo_tablespaces != srv_undo_tablespaces_open)
  {
    sql_print_warning("InnoDB: Cannot change innodb_undo_tablespaces=%u "
                      "because previous shutdown was not with "
                      "innodb_fast_shutdown=0", srv_undo_tablespaces);
    srv_undo_tablespaces= srv_undo_tablespaces_open;
  }
  else if (srv_undo_tablespaces_open > 0)
    sql_print_information("InnoDB: Opened " UINT32PF " undo tablespaces",
                          srv_undo_tablespaces_open);

  return DB_SUCCESS;
}

/** @return the number of active undo tablespaces (except system tablespace) */
static uint32_t trx_rseg_get_n_undo_tablespaces()
{
  std::set<uint32_t> space_ids;
  mtr_t mtr;
  mtr.start();

  if (const buf_block_t *sys_header=
      recv_sys.recover({TRX_SYS_SPACE, TRX_SYS_PAGE_NO}, &mtr, nullptr))
    for (ulint rseg_id= 0; rseg_id < TRX_SYS_N_RSEGS; rseg_id++)
      if (trx_sysf_rseg_get_page_no(sys_header, rseg_id) != FIL_NULL)
	if (uint32_t space= trx_sysf_rseg_get_space(sys_header, rseg_id))
	  space_ids.insert(space);
  mtr.commit();
  return static_cast<uint32_t>(space_ids.size());
}

/** Open an undo tablespace.
@param[in]	create	whether undo tablespaces are being created
@param[in]	name	tablespace file name
@param[in]	i	undo tablespace count
@return undo tablespace identifier
@retval 0   if file doesn't exist
@retval ~0U if page0 is corrupted */
static uint32_t srv_undo_tablespace_open(bool create, const char* name,
                                         uint32_t i)
{
  bool success;
  uint32_t space_id= 0;
  uint32_t fsp_flags= 0;

  if (create)
  {
    space_id= srv_undo_space_id_start + i;
    switch (srv_checksum_algorithm) {
    case SRV_CHECKSUM_ALGORITHM_FULL_CRC32:
    case SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32:
      fsp_flags= FSP_FLAGS_FCRC32_MASK_MARKER | FSP_FLAGS_FCRC32_PAGE_SSIZE();
      break;
    default:
      fsp_flags= FSP_FLAGS_PAGE_SSIZE();
    }
  }

  pfs_os_file_t fh= os_file_create(innodb_data_file_key, name,
                                   OS_FILE_OPEN_SILENT,
                                   OS_DATA_FILE,
                                   srv_read_only_mode, &success);

  if (!success)
    return 0;

  ulint n_retries = 5;
  os_offset_t size= os_file_get_size(fh);
  ut_a(size != os_offset_t(-1));
  page_t *apage= nullptr;
  const page_t *page= nullptr;

  if (!create)
  {
    apage= static_cast<byte*>(aligned_malloc(srv_page_size, srv_page_size));
undo_retry:
    if (os_file_read(IORequestRead, fh, apage, 0, srv_page_size, nullptr) !=
        DB_SUCCESS)
    {
err_exit:
      if (n_retries && srv_operation == SRV_OPERATION_BACKUP)
      {
        sql_print_information("InnoDB: Retrying to read undo "
                              "tablespace %s", name);
        n_retries--;
        goto undo_retry;
      }
      sql_print_error("InnoDB: Unable to read first page of file %s", name);
      aligned_free(apage);
      return ~0U;
    }

    page= apage;
    DBUG_EXECUTE_IF("undo_space_read_fail", goto err_exit;);

    uint32_t id= mach_read_from_4(FIL_PAGE_SPACE_ID + page);
    if (id == 0 || id >= SRV_SPACE_ID_UPPER_BOUND ||
        memcmp_aligned<2>(FIL_PAGE_SPACE_ID + page,
                          FSP_HEADER_OFFSET + FSP_SPACE_ID + page, 4))
    {
      sql_print_error("InnoDB: Inconsistent tablespace ID in file %s", name);
      goto err_exit;
    }

    space_id= id;
    fsp_flags= mach_read_from_4(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + page);

    if (buf_page_is_corrupted(false, page, fsp_flags))
    {
      page= recv_sys.dblwr.find_page(page_id_t{space_id, 0}, LSN_MAX);
      if (!page)
      {
        /* If the first page of the given user tablespace is not there
        in the doublewrite buffer, then the recovery is going to fail
        now. Report error only when doublewrite buffer is not empty */
        sql_print_error("InnoDB: Corrupted page "
                        "[page id: space=" UINT32PF ", page number=0]"
                        " of datafile '%s' could not be found"
                        " in the doublewrite buffer", space_id, name);
        goto err_exit;
      }

      fsp_flags= mach_read_from_4(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + page);
    }
  }

  /* Load the tablespace into InnoDB's internal data structures. */

  /* We set the biggest space id to the undo tablespace
  because InnoDB hasn't opened any other tablespace apart
  from the system tablespace. */

  fil_set_max_space_id_if_bigger(space_id);

  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_t::create(space_id, fsp_flags, false, nullptr,
                                          FIL_ENCRYPTION_DEFAULT, true);
  ut_ad(space);
  fil_node_t *file= space->add(name, fh, 0, false, true);

  if (create)
  {
    space->set_sizes(SRV_UNDO_TABLESPACE_SIZE_IN_PAGES);
    space->size= file->size= uint32_t(size >> srv_page_size_shift);
  }
  else if (!file->read_page0(page, true))
  {
    os_file_close(file->handle);
    file->handle= OS_FILE_CLOSED;
    ut_a(fil_system.n_open > 0);
    fil_system.n_open--;
  }

  mysql_mutex_unlock(&fil_system.mutex);
  aligned_free(apage);
  return space_id;
}

/** Check if undo tablespaces and redo log files exist before creating a
new system tablespace
@retval DB_SUCCESS  if all undo and redo logs are not found
@retval DB_ERROR    if any undo and redo logs are found */
static
dberr_t
srv_check_undo_redo_logs_exists()
{
	bool		ret;
	os_file_t	fh;
	char	name[OS_FILE_MAX_PATH];

	/* Check if any undo tablespaces exist */
	for (ulint i = 1; i <= srv_undo_tablespaces; ++i) {

		snprintf(name, sizeof name, "%s/undo%03zu", srv_undo_dir, i);

		fh = os_file_create_func(
			name,
			OS_FILE_OPEN_RETRY_SILENT,
			OS_DATA_FILE,
			srv_read_only_mode,
			&ret);

		if (ret) {
			os_file_close_func(fh);
			ib::error()
				<< "undo tablespace '" << name << "' exists."
				" Creating system tablespace with existing undo"
				" tablespaces is not supported. Please delete"
				" all undo tablespaces before creating new"
				" system tablespace.";
			return(DB_ERROR);
		}
	}

	/* Check if redo log file exists */
	auto logfilename = get_log_file_path();

	fh = os_file_create_func(logfilename.c_str(),
				 OS_FILE_OPEN_RETRY_SILENT,
				 OS_LOG_FILE,
				 srv_read_only_mode, &ret);

	if (ret) {
		os_file_close_func(fh);
		ib::error() << "redo log file '" << logfilename
			    << "' exists. Creating system tablespace with"
			       " existing redo log file is not recommended."
			       " Please delete redo log file before"
			       " creating new system tablespace.";
		return DB_ERROR;
	}

	return(DB_SUCCESS);
}

static dberr_t srv_all_undo_tablespaces_open(bool create_new_undo,
                                             uint32_t n_undo)
{
  /* Open all the undo tablespaces that are currently in use. If we
  fail to open any of these it is a fatal error. The tablespace ids
  should be contiguous. It is a fatal error because they are required
  for recovery and are referenced by the UNDO logs (a.k.a RBS). */

  uint32_t prev_id= create_new_undo ? srv_undo_space_id_start - 1 : 0;

  for (uint32_t i= 0; i < n_undo; ++i)
  {
    char name[OS_FILE_MAX_PATH];
    snprintf(name, sizeof name, "%s/undo%03u", srv_undo_dir, i + 1);
    uint32_t space_id= srv_undo_tablespace_open(create_new_undo, name, i);
    switch (space_id) {
    case ~0U:
      return DB_CORRUPTION;
    case 0:
      if (!create_new_undo)
        goto unused_undo;
      sql_print_error("InnoDB: Unable to open create tablespace '%s'.", name);
      return DB_ERROR;
    default:
      /* Should be no gaps in undo tablespace ids. */
      ut_a(!i || prev_id + 1 == space_id);
    }

    prev_id= space_id;

    /* Note the first undo tablespace id in case of
    no active undo tablespace. */
    if (0 == srv_undo_tablespaces_open++)
      srv_undo_space_id_start= space_id;
  }

  /* Open any extra unused undo tablespaces. These must be contiguous.
  We stop at the first failure. These are undo tablespaces that are
  not in use and therefore not required by recovery. We only check
  that there are no gaps. */
unused_undo:
  for (uint32_t i= prev_id + 1; i < srv_undo_space_id_start + TRX_SYS_N_RSEGS;
       ++i)
  {
     char name[OS_FILE_MAX_PATH];
     snprintf(name, sizeof name, "%s/undo%03u", srv_undo_dir, i);
     uint32_t space_id= srv_undo_tablespace_open(false, name, i);
     if (!space_id || space_id == ~0U)
       break;
     if (0 == srv_undo_tablespaces_open++)
       srv_undo_space_id_start= space_id;
  }

  return DB_SUCCESS;
}

/** Open the configured number of dedicated undo tablespaces.
@param[in]	create_new_undo	whether the undo tablespaces has to be created
@param[in,out]	mtr		mini-transaction
@return DB_SUCCESS or error code */
dberr_t srv_undo_tablespaces_init(bool create_new_undo, mtr_t *mtr)
{
  srv_undo_tablespaces_open= 0;

  ut_ad(!create_new_undo || mtr);
  ut_a(srv_undo_tablespaces <= TRX_SYS_N_RSEGS);
  ut_a(!create_new_undo || srv_operation <= SRV_OPERATION_EXPORT_RESTORED);

  if (srv_undo_tablespaces == 1)
    srv_undo_tablespaces= 0;

  /* Create the undo spaces only if we are creating a new
  instance. We don't allow creating of new undo tablespaces
  in an existing instance (yet). */
  if (create_new_undo)
  {
    DBUG_EXECUTE_IF("innodb_undo_upgrade", srv_undo_space_id_start= 3;);

    for (ulint i= 0; i < srv_undo_tablespaces; ++i)
    {
      char name[OS_FILE_MAX_PATH];
      snprintf(name, sizeof name, "%s/undo%03zu", srv_undo_dir, i + 1);
      if (dberr_t err= srv_undo_tablespace_create(name))
      {
	ib::error() << "Could not create undo tablespace '" << name << "'.";
	return err;
      }
    }
  }

  /* Get the tablespace ids of all the undo segments excluding
  the system tablespace (0). If we are creating a new instance then
  we build the undo_tablespace_ids ourselves since they don't
  already exist. */
  srv_undo_tablespaces_active= srv_undo_tablespaces;

  uint32_t n_undo= (create_new_undo || srv_operation == SRV_OPERATION_BACKUP ||
                    srv_operation == SRV_OPERATION_RESTORE_DELTA)
    ? srv_undo_tablespaces : TRX_SYS_N_RSEGS;

  mysql_mutex_lock(&recv_sys.mutex);
  dberr_t err= srv_all_undo_tablespaces_open(create_new_undo, n_undo);
  mysql_mutex_unlock(&recv_sys.mutex);

  /* Initialize srv_undo_space_id_start=0 when there are no
  dedicated undo tablespaces. */
  if (srv_undo_tablespaces_open == 0)
    srv_undo_space_id_start= 0;

  if (create_new_undo)
    for (uint32_t i= 0; err == DB_SUCCESS && i < srv_undo_tablespaces; ++i)
      err= fsp_header_init(fil_space_get(srv_undo_space_id_start + i),
                           SRV_UNDO_TABLESPACE_SIZE_IN_PAGES, mtr);

  return err;
}

/** Create the temporary file tablespace.
@param[in]	create_new_db	whether we are creating a new database
@return DB_SUCCESS or error code. */
static
dberr_t
srv_open_tmp_tablespace(bool create_new_db)
{
	ulint	sum_of_new_sizes;

	/* Will try to remove if there is existing file left-over by last
	unclean shutdown */
	srv_tmp_space.set_sanity_check_status(true);
	srv_tmp_space.delete_files();
	srv_tmp_space.set_ignore_read_only(true);

	bool	create_new_temp_space;

	srv_tmp_space.set_space_id(SRV_TMP_SPACE_ID);

	dberr_t	err = srv_tmp_space.check_file_spec(
		&create_new_temp_space, 12 * 1024 * 1024);

	if (err == DB_FAIL) {
		ib::error() << "The innodb_temporary"
			" data file must be writable!";
		err = DB_ERROR;
	} else if (err != DB_SUCCESS) {
		ib::error() << "Could not create the shared innodb_temporary.";
	} else if ((err = srv_tmp_space.open_or_create(
			    true, create_new_db, &sum_of_new_sizes))
		   != DB_SUCCESS) {
		ib::error() << "Unable to create the shared innodb_temporary";
	} else if (fil_system.temp_space->open(true)) {
		/* Initialize the header page */
		mtr_t mtr;
		mtr.start();
		mtr.set_log_mode(MTR_LOG_NO_REDO);
		err = fsp_header_init(fil_system.temp_space,
				      srv_tmp_space.get_sum_of_sizes(),
				      &mtr);
		mtr.commit();
		if (err == DB_SUCCESS) {
			err = trx_temp_rseg_create(&mtr);
		}
	} else {
		/* This file was just opened in the code above! */
		ib::error() << "The innodb_temporary"
			" data file cannot be re-opened"
			" after check_file_spec() succeeded!";
		err = DB_ERROR;
	}

	return(err);
}

/** Shutdown background threads, except the page cleaner.
@param init_abort set to true when InnoDB startup aborted */
static void srv_shutdown_threads(bool init_abort= false)
{
	ut_ad(!srv_undo_sources);
	srv_master_timer.reset();
	/* In case of InnoDB start up aborted, Don't change
	the srv_shutdown_state. Because innodb_shutdown()
	does call innodb_preshutdown() which changes the
	srv_shutdown_state back to SRV_SHUTDOWN_INITIATED */
	if (!init_abort) {
		srv_shutdown_state = SRV_SHUTDOWN_EXIT_THREADS;
	}

	if (purge_sys.enabled()) {
		srv_purge_shutdown();
	}

	if (srv_n_fil_crypt_threads) {
		fil_crypt_set_thread_cnt(0);
	}
}


/** Shut down background threads that can generate undo log. */
static void srv_shutdown_bg_undo_sources()
{
  srv_shutdown_state= SRV_SHUTDOWN_INITIATED;

  if (srv_undo_sources)
  {
    ut_ad(!srv_read_only_mode);
    fts_optimize_shutdown();
    dict_stats_shutdown();
    srv_undo_sources= false;
  }
}

#ifdef UNIV_DEBUG
# define srv_init_abort(_db_err)	\
	srv_init_abort_low(create_new_db, __FILE__, __LINE__, _db_err)
#else
# define srv_init_abort(_db_err)	\
	srv_init_abort_low(create_new_db, _db_err)
#endif /* UNIV_DEBUG */

/** Innobase start-up aborted. Perform cleanup actions.
@param[in]	create_new_db	TRUE if new db is  being created
@param[in]	file		File name
@param[in]	line		Line number
@param[in]	err		Reason for aborting InnoDB startup
@return DB_SUCCESS or error code. */
MY_ATTRIBUTE((warn_unused_result, nonnull))
static
dberr_t
srv_init_abort_low(
	bool		create_new_db,
#ifdef UNIV_DEBUG
	const char*	file,
	unsigned	line,
#endif /* UNIV_DEBUG */
	dberr_t		err)
{
	ut_ad(srv_is_being_started);

	if (create_new_db) {
		ib::error() << "Database creation was aborted"
#ifdef UNIV_DEBUG
			" at " << innobase_basename(file) << "[" << line << "]"
#endif /* UNIV_DEBUG */
			" with error " << err << ". You may need"
			" to delete the ibdata1 file before trying to start"
			" up again.";
	} else if (srv_operation == SRV_OPERATION_NORMAL) {
		ib::error() << "Plugin initialization aborted"
#ifdef UNIV_DEBUG
			" at " << innobase_basename(file) << "[" << line << "]"
#endif /* UNIV_DEBUG */
			" with error " << err;
	}

	srv_shutdown_bg_undo_sources();
	srv_shutdown_threads(true);
	return(err);
}

/** Prepare to delete the redo log file. Flush the dirty pages from all the
buffer pools.  Flush the redo log buffer to the redo log file.
@return lsn upto which data pages have been flushed. */
ATTRIBUTE_COLD static lsn_t srv_prepare_to_delete_redo_log_file() noexcept
{
  DBUG_ENTER("srv_prepare_to_delete_redo_log_file");

  ut_ad(recv_sys.recovery_on);

  /* Clean the buffer pool. */
  buf_flush_sync();

  DBUG_EXECUTE_IF("innodb_log_abort_1", DBUG_RETURN(0););
  DBUG_PRINT("ib_log", ("After innodb_log_abort_1"));

  log_sys.latch.wr_lock(SRW_LOCK_CALL);
  const bool latest_format{log_sys.is_latest()};
  lsn_t flushed_lsn{log_sys.get_flushed_lsn(std::memory_order_relaxed)};

  if (latest_format && !(log_sys.file_size & 4095) &&
      flushed_lsn != log_sys.next_checkpoint_lsn +
      (log_sys.is_encrypted()
       ? SIZE_OF_FILE_CHECKPOINT + 8
       : SIZE_OF_FILE_CHECKPOINT))
  {
#ifdef HAVE_PMEM
    if (!log_sys.is_opened())
      log_sys.buf_size= unsigned(std::min<uint64_t>(log_sys.capacity(),
                                                    log_sys.buf_size_max));
#endif
    fil_names_clear(flushed_lsn);
    flushed_lsn= log_sys.get_lsn();
  }

  {
    const char *msg;
    if (!latest_format)
    {
      msg= "Upgrading redo log: ";
same_size:
      ib::info() << msg << ib::bytes_iec(srv_log_file_size)
                 << "; LSN=" << flushed_lsn;
    }
    else if (srv_log_file_size == log_sys.file_size)
    {
      msg= srv_encrypt_log
        ? "Encrypting redo log: " : "Removing redo log encryption: ";
      goto same_size;
    }
    else
    {
      if (srv_encrypt_log == (my_bool)log_sys.is_encrypted())
        msg= srv_encrypt_log ? "Resizing encrypted" : "Resizing";
      else
        msg= srv_encrypt_log
          ? "Encrypting and resizing"
          : "Removing encryption and resizing";

      ib::info() << msg << " redo log from "
                 << ib::bytes_iec{log_sys.file_size} << " to "
                 << ib::bytes_iec{srv_log_file_size}
                 << "; LSN=" << flushed_lsn;
    }
  }

  log_sys.latch.wr_unlock();

  if (latest_format)
    log_write_up_to(flushed_lsn, false);

  ut_ad(flushed_lsn == log_get_lsn());
  ut_ad(!os_aio_pending_reads());
  ut_d(mysql_mutex_lock(&buf_pool.flush_list_mutex));
  ut_ad(!buf_pool.get_oldest_modification(0));
  ut_d(mysql_mutex_unlock(&buf_pool.flush_list_mutex));
  ut_d(os_aio_wait_until_no_pending_writes(false));

  DBUG_RETURN(flushed_lsn);
}

/** Upgrade the redo log to the latest format, or change its size
or encryption, before starting to write any log records. */
ATTRIBUTE_COLD static dberr_t srv_log_rebuild()
{
  /* Prepare to delete the old redo log file */
  const lsn_t lsn{srv_prepare_to_delete_redo_log_file()};

  DBUG_EXECUTE_IF("innodb_log_abort_1", return DB_ERROR;);
  /* Prohibit redo log writes from any other threads until creating a
  log checkpoint at the end of create_log_file(). */
  ut_d(recv_no_log_write= true);
  ut_ad(!os_aio_pending_reads());
  ut_d(mysql_mutex_lock(&buf_pool.flush_list_mutex));
  ut_ad(!buf_pool.get_oldest_modification(0));
  ut_d(mysql_mutex_unlock(&buf_pool.flush_list_mutex));
  /* os_aio_pending_writes() may hold here if some write_io_callback()
  did not release the slot yet.  However, the page write itself must
  have completed, because the buf_pool.flush_list is empty. In debug
  builds, we wait for this to happen, hoping to get a hung process if
  this assumption does not hold. */
  ut_d(os_aio_wait_until_no_pending_writes(false));

  /* Close the redo log file, so that we can replace it */
  log_sys.close_file();

  DBUG_EXECUTE_IF("innodb_log_abort_5", return DB_ERROR;);

  dberr_t err= create_log_file(false, lsn);

  if (err == DB_SUCCESS && log_sys.resize_rename())
    err = DB_ERROR;

  return err;
}

/** Rebuild the redo log if needed. */
static dberr_t srv_log_rebuild_if_needed()
{
  if (srv_force_recovery == SRV_FORCE_NO_LOG_REDO)
    /* Completely ignore the redo log. */
    return DB_SUCCESS;
  if (srv_read_only_mode)
    /* Leave the redo log alone. */
    return DB_SUCCESS;

  if (log_sys.file_size == srv_log_file_size &&
      log_sys.format ==
      (srv_encrypt_log ? log_t::FORMAT_ENC_11 : log_t::FORMAT_10_8))
  {
    /* No need to add or remove encryption, upgrade, or resize. */
    delete_log_files();
    return DB_SUCCESS;
  }

  return srv_log_rebuild();
}

ut_d(bool ibuf_upgrade_was_needed;)

ATTRIBUTE_COLD static dberr_t ibuf_log_rebuild_if_needed()
{
  ut_d(ibuf_upgrade_was_needed= true;)
  mysql_mutex_lock(&recv_sys.mutex);
  recv_sys.apply(true);
  mysql_mutex_unlock(&recv_sys.mutex);

  if (recv_sys.is_corrupt_log() || recv_sys.is_corrupt_fs())
    return DB_CORRUPTION;

  dberr_t err= srv_log_rebuild_if_needed();
  recv_sys.debug_free();
  return err;
}

static tpool::task_group rollback_all_recovered_group(1);
static tpool::task rollback_all_recovered_task(trx_rollback_all_recovered,
					       nullptr,
					       &rollback_all_recovered_group);

inline lsn_t log_t::init_lsn() noexcept
{
  latch.wr_lock(SRW_LOCK_CALL);
  ut_ad(!write_lsn_offset);
  write_lsn_offset= 0;
  const lsn_t lsn{base_lsn.load(std::memory_order_relaxed)};
  flushed_to_disk_lsn.store(lsn, std::memory_order_relaxed);
  write_lsn= lsn;
  latch.wr_unlock();
  return lsn;
}

/** Start InnoDB.
@param[in]	create_new_db	whether to create a new database
@return DB_SUCCESS or error code */
dberr_t srv_start(bool create_new_db)
{
	dberr_t		err		= DB_SUCCESS;
	mtr_t		mtr;

	ut_ad(srv_operation <= SRV_OPERATION_RESTORE_EXPORT
	      || srv_operation == SRV_OPERATION_RESTORE
	      || srv_operation == SRV_OPERATION_RESTORE_EXPORT);

	if (srv_force_recovery) {
		ib::info() << "!!! innodb_force_recovery is set to "
			<< srv_force_recovery << " !!!";
		if (srv_force_recovery == SRV_FORCE_NO_LOG_REDO) {
			srv_read_only_mode = true;
		}
	}

	if (srv_read_only_mode) {
		sql_print_information("InnoDB: Started in read only mode");
		buf_dblwr.use = buf_dblwr.USE_NO;
	}

	high_level_read_only = srv_read_only_mode
		|| srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN
		|| srv_sys_space.created_new_raw();

	srv_started_redo = false;

	compile_time_assert(sizeof(ulint) == sizeof(void*));

#ifdef UNIV_DEBUG
	ib::info() << "!!!!!!!! UNIV_DEBUG switched on !!!!!!!!!";
#endif

	ib::info() << "Compressed tables use zlib " ZLIB_VERSION
#ifdef UNIV_ZIP_DEBUG
	      " with validation"
#endif /* UNIV_ZIP_DEBUG */
	      ;
#ifdef UNIV_ZIP_COPY
	ib::info() << "and extra copying";
#endif /* UNIV_ZIP_COPY */

	/* Since InnoDB does not currently clean up all its internal data
	structures in MySQL Embedded Server Library server_end(), we
	print an error message if someone tries to start up InnoDB a
	second time during the process lifetime. */

	if (srv_start_has_been_called) {
		ib::error() << "Startup called second time"
			" during the process lifetime."
			" In the MariaDB Embedded Server Library"
			" you cannot call server_init() more than"
			" once during the process lifetime.";
	}

	srv_start_has_been_called = true;

	srv_is_being_started = true;

	/* Register performance schema stages before any real work has been
	started which may need to be instrumented. */
	mysql_stage_register("innodb", srv_stages,
			     static_cast<int>(UT_ARR_SIZE(srv_stages)));

	srv_boot();

	ib::info() << my_crc32c_implementation();

	if (!srv_read_only_mode) {
		mysql_mutex_init(srv_monitor_file_mutex_key,
				 &srv_monitor_file_mutex, nullptr);
		mysql_mutex_init(srv_misc_tmpfile_mutex_key,
				 &srv_misc_tmpfile_mutex, nullptr);
	}

	if (!srv_read_only_mode) {
		if (srv_innodb_status) {

			srv_monitor_file_name = static_cast<char*>(
				ut_malloc_nokey(
					strlen(fil_path_to_mysql_datadir)
					+ 20 + sizeof "/innodb_status."));

			sprintf(srv_monitor_file_name,
				"%s/innodb_status." ULINTPF,
				fil_path_to_mysql_datadir,
				static_cast<ulint>
				(IF_WIN(GetCurrentProcessId(), getpid())));

			srv_monitor_file = my_fopen(srv_monitor_file_name,
						    O_RDWR|O_TRUNC|O_CREAT,
						    MYF(MY_WME));

			if (!srv_monitor_file) {
				ib::error() << "Unable to create "
					<< srv_monitor_file_name << ": "
					<< strerror(errno);
				if (err == DB_SUCCESS) {
					err = DB_ERROR;
				}
			}
		} else {

			srv_monitor_file_name = NULL;
			srv_monitor_file = os_file_create_tmpfile();

			if (!srv_monitor_file && err == DB_SUCCESS) {
				err = DB_ERROR;
			}
		}

		srv_misc_tmpfile = os_file_create_tmpfile();

		if (!srv_misc_tmpfile && err == DB_SUCCESS) {
			err = DB_ERROR;
		}
	}

	if (err != DB_SUCCESS) {
		return(srv_init_abort(err));
	}

	if (os_aio_init()) {
		return(srv_init_abort(DB_ERROR));
	}

	fil_system.create(srv_file_per_table ? 50000 : 5000);

	if (buf_pool.create()) {
		return(srv_init_abort(DB_ERROR));
	}

	log_sys.create();
	recv_sys.create();
	lock_sys.create(srv_lock_table_size = 5 * buf_pool.curr_size());

	srv_startup_is_before_trx_rollback_phase = true;

	if (!srv_read_only_mode) {
		buf_flush_page_cleaner_init();
		ut_ad(buf_page_cleaner_is_active);
	}

	if (innodb_encrypt_temporary_tables && !log_crypt_init()) {
		return srv_init_abort(DB_ERROR);
	}

	/* Check if undo tablespaces and redo log files exist before creating
	a new system tablespace */
	if (create_new_db) {
		err = srv_check_undo_redo_logs_exists();
		if (err != DB_SUCCESS) {
			return(srv_init_abort(DB_ERROR));
		}
		recv_sys.debug_free();
	} else {
		err = recv_recovery_read_checkpoint();
		if (err != DB_SUCCESS) {
			return srv_init_abort(err);
		}
	}

	/* Open or create the data files. */
	ulint	sum_of_new_sizes;

	err = srv_sys_space.open_or_create(
		false, create_new_db, &sum_of_new_sizes);

	switch (err) {
	case DB_SUCCESS:
		break;
	case DB_CANNOT_OPEN_FILE:
		ib::error()
			<< "Could not open or create the system tablespace. If"
			" you tried to add new data files to the system"
			" tablespace, and it failed here, you should now"
			" edit innodb_data_file_path in my.cnf back to what"
			" it was, and remove the new ibdata files InnoDB"
			" created in this failed attempt. InnoDB only wrote"
			" those files full of zeros, but did not yet use"
			" them in any way. But be careful: do not remove"
			" old data files which contain your precious data!";
		/* fall through */
	default:
		/* Other errors might be flagged by
		Datafile::validate_first_page() */
		return srv_init_abort(err);
	}

	if (create_new_db) {
		lsn_t flushed_lsn = log_sys.init_lsn();

		err = create_log_file(true, flushed_lsn);

		if (err != DB_SUCCESS) {
			for (const Datafile &file: srv_sys_space) {
				os_file_delete(innodb_data_file_key,
					       file.filepath());
			}
			return srv_init_abort(err);
		}

		srv_undo_space_id_start = 1;
	}

	/* Open data files in the system tablespace: we keep
	them open until database shutdown */
	mysql_mutex_lock(&recv_sys.mutex);
	ut_d(fil_system.sys_space->recv_size = srv_sys_space_size_debug);
	err = fil_system.sys_space->open(create_new_db)
		? DB_SUCCESS : DB_ERROR;
	mysql_mutex_unlock(&recv_sys.mutex);


	if (err == DB_SUCCESS) {
		mtr_t mtr;
		mtr.start();
		err= srv_undo_tablespaces_init(create_new_db, &mtr);
		mtr.commit();
	}
	else {
		err= DB_ERROR;
	}

	/* If the force recovery is set very high then we carry on regardless
	of all errors. Basically this is fingers crossed mode. */

	if (err != DB_SUCCESS
	    && srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN) {

		return(srv_init_abort(err));
	}

	/* Initialize objects used by dict stats gathering thread, which
	can also be used by recovery if it tries to drop some table */
	if (!srv_read_only_mode) {
		dict_stats_init();
	}

	trx_sys.create();

	if (create_new_db) {
		ut_ad(!srv_read_only_mode);

		mtr.start();
		ut_ad(fil_system.sys_space->id == 0);
		compile_time_assert(TRX_SYS_SPACE == 0);
		err = fsp_header_init(fil_system.sys_space,
				      uint32_t(sum_of_new_sizes), &mtr);
		/* Allocate dummy change buffer pages for backward
		compatibility and to prevent a downgrade. */
		if (err != DB_SUCCESS) {
		} else if (buf_block_t *b =
			   fseg_create(fil_system.sys_space, PAGE_DATA, &mtr,
				       &err)) {
			ut_ad(b->page.id()
			      == page_id_t(0, FSP_IBUF_HEADER_PAGE_NO));
			b = fseg_alloc_free_page_general(
				b->page.frame + PAGE_DATA,
				FSP_IBUF_TREE_ROOT_PAGE_NO, FSP_UP, false,
				&mtr, &mtr, &err);
			if (b) {
				ut_ad(b->page.id() == page_id_t
				      (0, FSP_IBUF_TREE_ROOT_PAGE_NO));
				mtr.set_modified(*b);
				fsp_init_file_page(fil_system.sys_space, b,
						   &mtr);
			} else {
				ut_ad(err != DB_SUCCESS);
			}
		}
		/* To maintain backward compatibility we create only
		the first rollback segment before the double write buffer.
		All the remaining rollback segments will be created later,
		after the double write buffer has been created. */
		if (err == DB_SUCCESS) {
			err = trx_sys_create_sys_pages(&mtr);
		}
		mtr.commit();

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}

		err = dict_create();

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}

		buf_flush_sync();

		ut_ad(!srv_log_file_created);
		ut_d(srv_log_file_created= true);

		if (log_sys.resize_rename()) {
			return(srv_init_abort(DB_ERROR));
		}
	} else {
		/* Suppress warnings in fil_space_t::create() for files
		that are being read before dict_boot() has recovered
		DICT_HDR_MAX_SPACE_ID. */
		fil_system.space_id_reuse_warned = true;

		/* We always try to do a recovery, even if the database had
		been shut down normally: this is the normal startup path */

		err = recv_recovery_from_checkpoint_start();
		recv_sys.close_files();

		bool must_upgrade_ibuf = false;

		switch (srv_operation) {
                case SRV_OPERATION_NORMAL:
		case SRV_OPERATION_EXPORT_RESTORED:
		case SRV_OPERATION_RESTORE_EXPORT:
			if (err != DB_SUCCESS) {
				break;
			}

			err = ibuf_upgrade_needed();

			if (UNIV_UNLIKELY(err == DB_FAIL)) {
				must_upgrade_ibuf = true;
				err = ibuf_log_rebuild_if_needed();
			}

			if (err != DB_SUCCESS) {
				break;
			}

			err = dict_boot();
			/* fall through */
		case SRV_OPERATION_RESTORE:
			if (err != DB_SUCCESS) {
				break;
			}

			srv_undo_tablespaces_active
				= trx_rseg_get_n_undo_tablespaces();
			break;
		default:
			ut_ad("wrong mariabackup mode" == 0);
		}

		if (err != DB_SUCCESS) {
			return srv_init_abort(err);
		}

		if (srv_force_recovery < SRV_FORCE_NO_LOG_REDO) {
			/* Apply the hashed log records to the
			respective file pages, for the last batch of
			recv_group_scan_log_recs().
			Since it may generate huge batch of threadpool tasks,
			for read io task group, scale down thread creation rate
			by temporarily restricting tpool concurrency.
			*/
			srv_thread_pool->set_concurrency(srv_n_read_io_threads);

			mysql_mutex_lock(&recv_sys.mutex);
			recv_sys.apply(true);
			mysql_mutex_unlock(&recv_sys.mutex);

			srv_thread_pool->set_concurrency();

			if (recv_sys.is_corrupt_log()
			    || recv_sys.is_corrupt_fs()) {
				return(srv_init_abort(DB_CORRUPTION));
			}

			if (srv_operation != SRV_OPERATION_RESTORE
			    || recv_needed_recovery) {
			}

			DBUG_PRINT("ib_log", ("apply completed"));

			if (srv_operation != SRV_OPERATION_RESTORE) {
				dict_sys.lock(SRW_LOCK_CALL);
				dict_load_sys_table(dict_sys.sys_tables);
				dict_sys.unlock();

				if (UNIV_UNLIKELY(must_upgrade_ibuf)) {
					dict_load_tablespaces(nullptr, true);
					err = ibuf_upgrade();
					if (err != DB_SUCCESS) {
						return srv_init_abort(err);
					}
				}

				dict_sys.lock(SRW_LOCK_CALL);
				dict_load_sys_table(dict_sys.sys_columns);
				dict_load_sys_table(dict_sys.sys_indexes);
				dict_load_sys_table(dict_sys.sys_fields);
				dict_sys.unlock();
				dict_sys.load_sys_tables();

				err = trx_lists_init_at_db_start();
				if (err != DB_SUCCESS) {
					return srv_init_abort(err);
				}

				if (recv_needed_recovery) {
					trx_sys_print_mysql_binlog_offset();
				}
			} else if (recv_needed_recovery) {
				err = trx_lists_init_at_db_start();
				if (err != DB_SUCCESS) {
					return srv_init_abort(err);
				}
				trx_sys_print_mysql_binlog_offset();
			}
		}

		fil_system.space_id_reuse_warned = false;

		if (srv_operation > SRV_OPERATION_EXPORT_RESTORED) {
			ut_ad(srv_operation == SRV_OPERATION_RESTORE_EXPORT
			      || srv_operation == SRV_OPERATION_RESTORE);
			return(err);
		}

		/* Upgrade or resize or rebuild the redo logs before
		generating any dirty pages, so that the old redo log
		file will not be written to. */

		err = srv_log_rebuild_if_needed();

		if (err != DB_SUCCESS) {
			return srv_init_abort(err);
		}

		recv_sys.debug_free();

		if (!srv_read_only_mode) {
			const uint32_t flags = FSP_FLAGS_PAGE_SSIZE();
			for (uint32_t id = srv_undo_space_id_start;
			     id <= srv_undo_tablespaces; id++) {
				if (fil_space_t* space = fil_space_get(id)) {
					fsp_flags_try_adjust(space, flags);
				}
			}

			if (sum_of_new_sizes > 0) {
				/* New data file(s) were added */
				mtr.start();
				mtr.x_lock_space(fil_system.sys_space);
				buf_block_t* block = buf_page_get(
					page_id_t(0, 0), 0,
					RW_SX_LATCH, &mtr);
				/* The first page of the system tablespace
				should already have been successfully
				accessed earlier during startup. */
				ut_a(block);
				ulint size = mach_read_from_4(
					FSP_HEADER_OFFSET + FSP_SIZE
					+ block->page.frame);
				ut_ad(size == fil_system.sys_space
				      ->size_in_header);
				size += sum_of_new_sizes;
				mtr.write<4>(*block,
					     FSP_HEADER_OFFSET + FSP_SIZE
					     + block->page.frame, size);
				fil_system.sys_space->size_in_header
					= uint32_t(size);
				mtr.commit();
				log_write_up_to(mtr.commit_lsn(), true);
			}
		}

#ifdef UNIV_DEBUG
		{
			mtr.start();
			buf_block_t* block = buf_page_get(page_id_t(0, 0), 0,
							  RW_S_LATCH, &mtr);
			ut_ad(mach_read_from_4(FSP_SIZE + FSP_HEADER_OFFSET
					       + block->page.frame)
			      == fil_system.sys_space->size_in_header);
			mtr.commit();
		}
#endif
		const ulint	tablespace_size_in_header
			= fil_system.sys_space->size_in_header;
		const ulint	sum_of_data_file_sizes
			= srv_sys_space.get_sum_of_sizes();
		/* Compare the system tablespace file size to what is
		stored in FSP_SIZE. In srv_sys_space.open_or_create()
		we already checked that the file sizes match the
		innodb_data_file_path specification. */
		if (srv_read_only_mode
		    || sum_of_data_file_sizes == tablespace_size_in_header) {
			/* Do not complain about the size. */
		} else if (!srv_sys_space.can_auto_extend_last_file()
			   || sum_of_data_file_sizes
			   < tablespace_size_in_header) {
			ib::error() << "Tablespace size stored in header is "
				<< tablespace_size_in_header
				<< " pages, but the sum of data file sizes is "
				<< sum_of_data_file_sizes << " pages";

			if (srv_force_recovery == 0
			    && sum_of_data_file_sizes
			    < tablespace_size_in_header) {
				ib::error() <<
					"Cannot start InnoDB. The tail of"
					" the system tablespace is"
					" missing. Have you edited"
					" innodb_data_file_path in my.cnf"
					" in an inappropriate way, removing"
					" data files from there?"
					" You can set innodb_force_recovery=1"
					" in my.cnf to force"
					" a startup if you are trying to"
					" recover a badly corrupt database.";

				return(srv_init_abort(DB_ERROR));
			}
		}
	}

	ut_ad(err == DB_SUCCESS);
	ut_a(sum_of_new_sizes != ULINT_UNDEFINED);

	/* Create the doublewrite buffer to a new tablespace */
	if (!srv_read_only_mode && srv_force_recovery < SRV_FORCE_NO_TRX_UNDO
	    && !buf_dblwr.create()) {
		return(srv_init_abort(DB_ERROR));
	}

	/* Recreate the undo tablespaces */
	if (!high_level_read_only) {
		err = srv_undo_tablespaces_reinitialize();
		if (err) {
			return srv_init_abort(err);
		}
	}

	srv_undo_tablespaces = srv_undo_tablespaces_open;

	/* Here the double write buffer has already been created and so
	any new rollback segments will be allocated after the double
	write buffer. The default segment should already exist.
	We create the new segments only if it's a new database or
	the database was shutdown cleanly. */

	/* Note: When creating the extra rollback segments during an upgrade
	we violate the latching order, even if the change buffer is empty.
	It cannot create a deadlock because we are still
	running in single threaded mode essentially. Only the IO threads
	should be running at this stage. */

	if (!trx_sys_create_rsegs()) {
		return(srv_init_abort(DB_ERROR));
	}

	if (!create_new_db) {
		ut_ad(high_level_read_only
		      || srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN);

		if (!high_level_read_only
		    && srv_sys_space.can_auto_shrink()) {
			fsp_system_tablespace_truncate(false);
			DBUG_EXECUTE_IF("crash_after_sys_truncate",
					return srv_init_abort(DB_ERROR););
		}

		/* Validate a few system page types that were left
		uninitialized before MySQL or MariaDB 5.5. */
		if (!high_level_read_only
		    && !fil_system.sys_space->full_crc32()) {
			buf_block_t*	block;
			mtr.start();
			/* Bitmap page types will be reset in
			buf_dblwr_check_block() without redo logging. */
			block = buf_page_get(
				page_id_t(0, FSP_IBUF_HEADER_PAGE_NO),
				0, RW_X_LATCH, &mtr);
			if (UNIV_UNLIKELY(!block)) {
			corrupted_old_page:
				mtr.commit();
				return srv_init_abort(DB_CORRUPTION);
			}
			fil_block_check_type(*block, FIL_PAGE_TYPE_SYS, &mtr);
			/* Already MySQL 3.23.53 initialized
			FSP_IBUF_TREE_ROOT_PAGE_NO to
			FIL_PAGE_INDEX. No need to reset that one. */
			block = buf_page_get(
				page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO),
				0, RW_X_LATCH, &mtr);
			if (UNIV_UNLIKELY(!block)) {
				goto corrupted_old_page;
			}
			fil_block_check_type(*block, FIL_PAGE_TYPE_TRX_SYS,
					     &mtr);
			block = buf_page_get(
				page_id_t(TRX_SYS_SPACE,
					  FSP_FIRST_RSEG_PAGE_NO),
				0, RW_X_LATCH, &mtr);
			if (UNIV_UNLIKELY(!block)) {
				goto corrupted_old_page;
			}
			fil_block_check_type(*block, FIL_PAGE_TYPE_SYS, &mtr);
			block = buf_page_get(
				page_id_t(TRX_SYS_SPACE, FSP_DICT_HDR_PAGE_NO),
				0, RW_X_LATCH, &mtr);
			if (UNIV_UNLIKELY(!block)) {
				goto corrupted_old_page;
			}
			fil_block_check_type(*block, FIL_PAGE_TYPE_SYS, &mtr);
			mtr.commit();
		}

		/* Roll back any recovered data dictionary
		transactions, so that the data dictionary tables will
		be free of any locks.  The data dictionary latch
		should guarantee that there is at most one data
		dictionary transaction active at a time. */
		if (!high_level_read_only
		    && srv_force_recovery <= SRV_FORCE_NO_TRX_UNDO) {
			/* If the following call is ever removed, the
			first-time ha_innobase::open() must hold (or
			acquire and release) a table lock that
			conflicts with trx_resurrect_table_locks(), to
			ensure that any recovered incomplete ALTER
			TABLE will have been rolled back. Otherwise,
			dict_table_t::instant could be cleared by
			rollback invoking
			dict_index_t::clear_instant_alter() while open
			table handles exist in client connections. */
			trx_rollback_recovered(false);
		}

		if (srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN) {
			dict_load_tablespaces();
		}

		if (srv_force_recovery < SRV_FORCE_NO_TRX_UNDO
		    && !srv_read_only_mode) {
			/* Drop partially created indexes. */
			row_merge_drop_temp_indexes();
			/* Rollback incomplete non-DDL transactions */
			trx_rollback_is_active = true;
			srv_thread_pool->submit_task(&rollback_all_recovered_task);
		}
	}

	srv_startup_is_before_trx_rollback_phase = false;

	if (!srv_read_only_mode) {
		DBUG_EXECUTE_IF("innodb_skip_monitors", goto skip_monitors;);
		/* Create the task which warns of long semaphore waits */
		srv_start_periodic_timer(srv_monitor_timer, srv_monitor_task,
					 SRV_MONITOR_INTERVAL);

#ifndef DBUG_OFF
skip_monitors:
#endif
		ut_ad(srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN
		      || !purge_sys.enabled());

		if (srv_force_recovery < SRV_FORCE_NO_BACKGROUND) {
			srv_undo_sources = true;
			/* Create the dict stats gathering task */
			dict_stats_start();
			/* Create the thread that will optimize the
			FULLTEXT search index subsystem. */
			fts_optimize_init();
		}
	}

	err = dict_sys.create_or_check_sys_tables();
	switch (err) {
	case DB_SUCCESS:
		break;
	case DB_READ_ONLY:
		if (srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO) {
			break;
		}
		ib::error() << "Cannot create system tables in read-only mode";
		/* fall through */
	default:
		return(srv_init_abort(err));
	}

        innodb_binlog_startup_init();

	if (!srv_read_only_mode
	    && srv_operation <= SRV_OPERATION_EXPORT_RESTORED) {
		/* Initialize the innodb_temporary tablespace and keep
		it open until shutdown. */
		err = srv_open_tmp_tablespace(create_new_db);

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}

		if (srv_force_recovery < SRV_FORCE_NO_BACKGROUND) {
			srv_start_periodic_timer(srv_master_timer, srv_master_callback, 1000);
		}
	}

	srv_is_being_started = false;

	if (srv_print_verbose_log) {
		sql_print_information("InnoDB: "
				      "log sequence number " LSN_PF
				      "%s"
				      "; transaction id " TRX_ID_FMT,
				      recv_sys.lsn,
				      log_sys.is_mmap()
				      ? " (memory-mapped)" : "",
				      trx_sys.get_max_trx_id());
	}

	if (!srv_read_only_mode) {
		if (create_new_db) {
			srv_buffer_pool_load_at_startup = FALSE;
		}

#ifdef WITH_WSREP
		/*
		  Create the dump/load thread only when not running with
		  --wsrep-recover.
		*/
		if (!get_wsrep_recovery()) {
#endif /* WITH_WSREP */

		/* Start buffer pool dump/load task */
		buf_load_at_startup();

#ifdef WITH_WSREP
		} else {
			ib::warn() <<
				"Skipping buffer pool dump/restore during "
				"wsrep recovery.";
		}
#endif /* WITH_WSREP */

		/* Create thread(s) that handles key rotation. This is
		needed already here as log_preflush_pool_modified_pages
		will flush dirty pages and that might need e.g.
		fil_crypt_threads_cond. */
		fil_crypt_threads_init();

		srv_started_redo = true;
	}

	return(DB_SUCCESS);
}

/**
  Shutdown purge to make sure that there is no possibility that we call any
  plugin code (e.g., audit) inside virtual column computation.
*/
void innodb_preshutdown()
{
  static bool first_time= true;
  if (!first_time)
    return;
  first_time= false;

  if (srv_read_only_mode)
    return;
  if (!srv_fast_shutdown && srv_operation <= SRV_OPERATION_EXPORT_RESTORED)
    if (srv_force_recovery < SRV_FORCE_NO_TRX_UNDO && srv_was_started)
      while (trx_sys.any_active_transactions())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
  srv_shutdown_bg_undo_sources();
  srv_purge_shutdown();

  if (srv_n_fil_crypt_threads)
    fil_crypt_set_thread_cnt(0);
}


/** Shut down InnoDB. */
void innodb_shutdown()
{
	innodb_preshutdown();
	ut_ad(!srv_undo_sources);
	switch (srv_operation) {
	case SRV_OPERATION_BACKUP:
	case SRV_OPERATION_RESTORE_DELTA:
	case SRV_OPERATION_BACKUP_NO_DEFER:
		break;
	case SRV_OPERATION_RESTORE:
	case SRV_OPERATION_RESTORE_EXPORT:
		mysql_mutex_lock(&buf_pool.flush_list_mutex);
		srv_shutdown_state = SRV_SHUTDOWN_CLEANUP;
		while (buf_page_cleaner_is_active) {
			pthread_cond_signal(&buf_pool.do_flush_list);
			my_cond_wait(&buf_pool.done_flush_list,
				     &buf_pool.flush_list_mutex.m_mutex);
		}
		mysql_mutex_unlock(&buf_pool.flush_list_mutex);
		break;
	case SRV_OPERATION_NORMAL:
	case SRV_OPERATION_EXPORT_RESTORED:
		/* Shut down the persistent files. */
		logs_empty_and_mark_files_at_shutdown();
	}

        innodb_binlog_close();
	os_aio_free();
	fil_space_t::close_all();
	/* Exit any remaining threads. */
	ut_ad(!buf_page_cleaner_is_active);
	srv_shutdown_threads();

	if (srv_monitor_file) {
		my_fclose(srv_monitor_file, MYF(MY_WME));
		srv_monitor_file = 0;
		if (srv_monitor_file_name) {
			unlink(srv_monitor_file_name);
			ut_free(srv_monitor_file_name);
		}
	}

	if (srv_misc_tmpfile) {
		my_fclose(srv_misc_tmpfile, MYF(MY_WME));
		srv_misc_tmpfile = 0;
	}

	ut_ad(dict_sys.is_initialised() || !srv_was_started);
	ut_ad(trx_sys.is_initialised() || !srv_was_started);
	ut_ad(buf_dblwr.is_created() || !srv_was_started
	      || srv_read_only_mode
	      || srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO);
	ut_ad(lock_sys.is_initialised() || !srv_was_started);
	ut_ad(log_sys.is_initialised() || !srv_was_started);

	dict_stats_deinit();

	if (srv_started_redo) {
		ut_ad(!srv_read_only_mode);
		/* srv_shutdown_bg_undo_sources() already invoked
		fts_optimize_shutdown(); dict_stats_shutdown(); */

		fil_crypt_threads_cleanup();
	}

	/* This must be disabled before closing the buffer pool
	and closing the data dictionary.  */

#ifdef BTR_CUR_HASH_ADAPT
	if (dict_sys.is_initialised()) {
		btr_search_disable();
	}
#endif /* BTR_CUR_HASH_ADAPT */
	log_sys.close();
	purge_sys.close();
	trx_sys.close();
	buf_dblwr.close();
	lock_sys.close();
	trx_pool_close();

	if (!srv_read_only_mode) {
		mysql_mutex_destroy(&srv_monitor_file_mutex);
		mysql_mutex_destroy(&srv_misc_tmpfile_mutex);
	}

	dict_sys.close();
	btr_search_sys_free();
	srv_free();
	fil_system.close();
	pars_lexer_close();
	recv_sys.close();

	ut_ad(buf_pool.is_initialised() || !srv_was_started);
	buf_pool.close();

	srv_sys_space.shutdown();
	if (srv_tmp_space.get_sanity_check_status()) {
		if (fil_system.temp_space) {
			fil_system.temp_space->close();
		}
		srv_tmp_space.delete_files();
	}
	srv_tmp_space.shutdown();

	if (srv_stats.pages_page_compression_error)
		ib::warn() << "Page compression errors: "
			   << srv_stats.pages_page_compression_error;

	if (srv_was_started && srv_print_verbose_log) {
		ib::info() << "Shutdown completed; log sequence number "
			   << srv_shutdown_lsn
			   << "; transaction id " << trx_sys.get_max_trx_id();
	}
	srv_thread_pool_end();
	srv_started_redo = false;
	srv_was_started = false;
	srv_start_has_been_called = false;
}

/** Get the meta-data filename from the table name for a
single-table tablespace.
@param[in]	table		table object
@param[out]	filename	filename
@param[in]	max_len		filename max length */
void
srv_get_meta_data_filename(
	dict_table_t*	table,
	char*		filename,
	ulint		max_len)
{
	ulint		len;
	char*		path;

	/* Make sure the data_dir_path is set. */
	dict_get_and_save_data_dir_path(table);

	const char* data_dir_path = DICT_TF_HAS_DATA_DIR(table->flags)
		? table->data_dir_path : nullptr;
	ut_ad(!DICT_TF_HAS_DATA_DIR(table->flags) || data_dir_path);

	path = fil_make_filepath(data_dir_path, table->name, CFG,
				 data_dir_path != nullptr);
	ut_a(path);
	len = strlen(path);
	ut_a(max_len >= len);

	strcpy(filename, path);

	ut_free(path);
}
