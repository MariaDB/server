/*****************************************************************************

Copyright (c) 1997, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2013, 2018, MariaDB Corporation.

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
@file log/log0recv.cc
Recovery

Created 9/20/1997 Heikki Tuuri
*******************************************************/

#include "ha_prototypes.h"

#include <vector>
#include <map>
#include <string>
#include <my_service_manager.h>

#include "log0recv.h"

#ifdef HAVE_MY_AES_H
#include <my_aes.h>
#endif

#include "log0crypt.h"
#include "mem0mem.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "mtr0mtr.h"
#include "mtr0log.h"
#include "page0cur.h"
#include "page0zip.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "ibuf0ibuf.h"
#include "trx0undo.h"
#include "trx0rec.h"
#include "fil0fil.h"
#include "fsp0sysspace.h"
#include "ut0new.h"
#include "row0trunc.h"
#include "buf0rea.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0roll.h"
#include "row0merge.h"

/** Log records are stored in the hash table in chunks at most of this size;
this must be less than srv_page_size as it is stored in the buffer pool */
#define RECV_DATA_BLOCK_SIZE	(MEM_MAX_ALLOC_IN_BUF - sizeof(recv_data_t))

/** Read-ahead area in applying log records to file pages */
#define RECV_READ_AHEAD_AREA	32

/** The recovery system */
recv_sys_t*	recv_sys;
/** TRUE when applying redo log records during crash recovery; FALSE
otherwise.  Note that this is FALSE while a background thread is
rolling back incomplete transactions. */
volatile bool	recv_recovery_on;

/** TRUE when recv_init_crash_recovery() has been called. */
bool	recv_needed_recovery;
#ifdef UNIV_DEBUG
/** TRUE if writing to the redo log (mtr_commit) is forbidden.
Protected by log_sys.mutex. */
bool	recv_no_log_write = false;
#endif /* UNIV_DEBUG */

/** TRUE if buf_page_is_corrupted() should check if the log sequence
number (FIL_PAGE_LSN) is in the future.  Initially FALSE, and set by
recv_recovery_from_checkpoint_start(). */
bool	recv_lsn_checks_on;

/** If the following is TRUE, the buffer pool file pages must be invalidated
after recovery and no ibuf operations are allowed; this becomes TRUE if
the log record hash table becomes too full, and log records must be merged
to file pages already before the recovery is finished: in this case no
ibuf operations are allowed, as they could modify the pages read in the
buffer pool before the pages have been recovered to the up-to-date state.

TRUE means that recovery is running and no operations on the log files
are allowed yet: the variable name is misleading. */
bool	recv_no_ibuf_operations;

/** The type of the previous parsed redo log record */
static mlog_id_t	recv_previous_parsed_rec_type;
/** The offset of the previous parsed redo log record */
static ulint	recv_previous_parsed_rec_offset;
/** The 'multi' flag of the previous parsed redo log record */
static ulint	recv_previous_parsed_rec_is_multi;

/** This many frames must be left free in the buffer pool when we scan
the log and store the scanned log records in the buffer pool: we will
use these free frames to read in pages when we start applying the
log records to the database.
This is the default value. If the actual size of the buffer pool is
larger than 10 MB we'll set this value to 512. */
ulint	recv_n_pool_free_frames;

/** The maximum lsn we see for a page during the recovery process. If this
is bigger than the lsn we are able to scan up to, that is an indication that
the recovery failed and the database may be corrupt. */
static lsn_t	recv_max_page_lsn;

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t	trx_rollback_clean_thread_key;
mysql_pfs_key_t	recv_writer_thread_key;
#endif /* UNIV_PFS_THREAD */

/** Is recv_writer_thread active? */
bool	recv_writer_thread_active;

#ifndef	DBUG_OFF
/** Return string name of the redo log record type.
@param[in]	type	record log record enum
@return string name of record log record */
static const char* get_mlog_string(mlog_id_t type);
#endif /* !DBUG_OFF */

/** Tablespace item during recovery */
struct file_name_t {
	/** Tablespace file name (MLOG_FILE_NAME) */
	std::string	name;
	/** Tablespace object (NULL if not valid or not found) */
	fil_space_t*	space;

	/** Tablespace status. */
	enum fil_status {
		/** Normal tablespace */
		NORMAL,
		/** Deleted tablespace */
		DELETED,
		/** Missing tablespace */
		MISSING
	};

	/** Status of the tablespace */
	fil_status	status;

	/** Constructor */
	file_name_t(std::string name_, bool deleted) :
		name(name_), space(NULL), status(deleted ? DELETED: NORMAL) {}
};

/** Map of dirty tablespaces during recovery */
typedef std::map<
	ulint,
	file_name_t,
	std::less<ulint>,
	ut_allocator<std::pair<const ulint, file_name_t> > >	recv_spaces_t;

static recv_spaces_t	recv_spaces;

/** States of recv_addr_t */
enum recv_addr_state {
	/** not yet processed */
	RECV_NOT_PROCESSED,
	/** page is being read */
	RECV_BEING_READ,
	/** log records are being applied on the page */
	RECV_BEING_PROCESSED,
	/** log records have been applied on the page */
	RECV_PROCESSED,
	/** log records have been discarded because the tablespace
	does not exist */
	RECV_DISCARDED
};

/** Hashed page file address struct */
struct recv_addr_t{
	/** recovery state of the page */
	recv_addr_state	state;
	/** tablespace identifier */
	unsigned	space:32;
	/** page number */
	unsigned	page_no:32;
	/** list of log records for this page */
	UT_LIST_BASE_NODE_T(recv_t) rec_list;
	/** hash node in the hash bucket chain */
	hash_node_t	addr_hash;
};

/** Report optimized DDL operation (without redo log),
corresponding to MLOG_INDEX_LOAD.
@param[in]	space_id	tablespace identifier
*/
void (*log_optimized_ddl_op)(ulint space_id);

/** Report backup-unfriendly TRUNCATE operation (with separate log file),
corresponding to MLOG_TRUNCATE. */
void (*log_truncate)();

/** Report an operation to create, delete, or rename a file during backup.
@param[in]	space_id	tablespace identifier
@param[in]	flags		tablespace flags (NULL if not create)
@param[in]	name		file name (not NUL-terminated)
@param[in]	len		length of name, in bytes
@param[in]	new_name	new file name (NULL if not rename)
@param[in]	new_len		length of new_name, in bytes (0 if NULL) */
void (*log_file_op)(ulint space_id, const byte* flags,
		    const byte* name, ulint len,
		    const byte* new_name, ulint new_len);

/** Process a MLOG_CREATE2 record that indicates that a tablespace
is being shrunk in size.
@param[in]	space_id	tablespace identifier
@param[in]	pages		trimmed size of the file, in pages
@param[in]	lsn		log sequence number of the operation */
static void recv_addr_trim(ulint space_id, unsigned pages, lsn_t lsn)
{
	DBUG_ENTER("recv_addr_trim");
	DBUG_LOG("ib_log",
		 "discarding log beyond end of tablespace "
		 << page_id_t(space_id, pages) << " before LSN " << lsn);
	ut_ad(mutex_own(&recv_sys->mutex));
	for (ulint i = recv_sys->addr_hash->n_cells; i--; ) {
		hash_cell_t* const cell = hash_get_nth_cell(
			recv_sys->addr_hash, i);
		for (recv_addr_t* addr = static_cast<recv_addr_t*>(cell->node),
			     *prev = NULL, *next;
		     addr;
		     prev = addr, addr = next) {
			next = static_cast<recv_addr_t*>(addr->addr_hash);

			if (addr->space != space_id || addr->page_no < pages) {
				continue;
			}

			for (recv_t* recv = UT_LIST_GET_FIRST(addr->rec_list);
			     recv; ) {
				recv_t* n = UT_LIST_GET_NEXT(rec_list, recv);
				if (recv->start_lsn < lsn) {
					DBUG_PRINT("ib_log",
						   ("Discarding %s for"
						    " page %u:%u at " LSN_PF,
						    get_mlog_string(
							    recv->type),
						    addr->space, addr->page_no,
						    recv->start_lsn));
					UT_LIST_REMOVE(addr->rec_list, recv);
				}
				recv = n;
			}

			if (UT_LIST_GET_LEN(addr->rec_list)) {
				DBUG_PRINT("ib_log",
					   ("preserving " ULINTPF
					    " records for page %u:%u",
					    UT_LIST_GET_LEN(addr->rec_list),
					    addr->space, addr->page_no));
			} else {
				ut_ad(recv_sys->n_addrs);
				--recv_sys->n_addrs;
				if (addr == cell->node) {
					cell->node = next;
				} else {
					prev->addr_hash = next;
				}
			}
		}
	}
	if (fil_space_t* space = fil_space_get(space_id)) {
		ut_ad(UT_LIST_GET_LEN(space->chain) == 1);
		fil_node_t* file = UT_LIST_GET_FIRST(space->chain);
		ut_ad(file->is_open());
		os_file_truncate(file->name, file->handle,
				 os_offset_t(pages) << srv_page_size_shift,
				 true);
	}
	DBUG_VOID_RETURN;
}

/** Process a file name from a MLOG_FILE_* record.
@param[in,out]	name		file name
@param[in]	len		length of the file name
@param[in]	space_id	the tablespace ID
@param[in]	deleted		whether this is a MLOG_FILE_DELETE record */
static
void
fil_name_process(
	char*	name,
	ulint	len,
	ulint	space_id,
	bool	deleted)
{
	if (srv_operation == SRV_OPERATION_BACKUP) {
		return;
	}

	ut_ad(srv_operation == SRV_OPERATION_NORMAL
	      || srv_operation == SRV_OPERATION_RESTORE
	      || srv_operation == SRV_OPERATION_RESTORE_EXPORT);

	/* We will also insert space=NULL into the map, so that
	further checks can ensure that a MLOG_FILE_NAME record was
	scanned before applying any page records for the space_id. */

	os_normalize_path(name);
	file_name_t	fname(std::string(name, len - 1), deleted);
	std::pair<recv_spaces_t::iterator,bool> p = recv_spaces.insert(
		std::make_pair(space_id, fname));
	ut_ad(p.first->first == space_id);

	file_name_t&	f = p.first->second;

	if (deleted) {
		/* Got MLOG_FILE_DELETE */

		if (!p.second && f.status != file_name_t::DELETED) {
			f.status = file_name_t::DELETED;
			if (f.space != NULL) {
				fil_space_free(space_id, false);
				f.space = NULL;
			}
		}

		ut_ad(f.space == NULL);
	} else if (p.second // the first MLOG_FILE_NAME or MLOG_FILE_RENAME2
		   || f.name != fname.name) {
		fil_space_t*	space;

		/* Check if the tablespace file exists and contains
		the space_id. If not, ignore the file after displaying
		a note. Abort if there are multiple files with the
		same space_id. */
		switch (fil_ibd_load(space_id, name, space)) {
		case FIL_LOAD_OK:
			ut_ad(space != NULL);

			if (f.space == NULL || f.space == space) {
				f.name = fname.name;
				f.space = space;
				f.status = file_name_t::NORMAL;
			} else {
				ib::error() << "Tablespace " << space_id
					<< " has been found in two places: '"
					<< f.name << "' and '" << name << "'."
					" You must delete one of them.";
				recv_sys->found_corrupt_fs = true;
			}
			break;

		case FIL_LOAD_ID_CHANGED:
			ut_ad(space == NULL);
			break;

		case FIL_LOAD_NOT_FOUND:
			/* No matching tablespace was found; maybe it
			was renamed, and we will find a subsequent
			MLOG_FILE_* record. */
			ut_ad(space == NULL);

			if (srv_force_recovery) {
				/* Without innodb_force_recovery,
				missing tablespaces will only be
				reported in
				recv_init_crash_recovery_spaces().
				Enable some more diagnostics when
				forcing recovery. */

				ib::info()
					<< "At LSN: " << recv_sys->recovered_lsn
					<< ": unable to open file " << name
					<< " for tablespace " << space_id;
			}
			break;

		case FIL_LOAD_INVALID:
			ut_ad(space == NULL);
			if (srv_force_recovery == 0) {
				ib::warn() << "We do not continue the crash"
					" recovery, because the table may"
					" become corrupt if we cannot apply"
					" the log records in the InnoDB log to"
					" it. To fix the problem and start"
					" mysqld:";
				ib::info() << "1) If there is a permission"
					" problem in the file and mysqld"
					" cannot open the file, you should"
					" modify the permissions.";
				ib::info() << "2) If the tablespace is not"
					" needed, or you can restore an older"
					" version from a backup, then you can"
					" remove the .ibd file, and use"
					" --innodb_force_recovery=1 to force"
					" startup without this file.";
				ib::info() << "3) If the file system or the"
					" disk is broken, and you cannot"
					" remove the .ibd file, you can set"
					" --innodb_force_recovery.";
				recv_sys->found_corrupt_fs = true;
				break;
			}

			ib::info() << "innodb_force_recovery was set to "
				<< srv_force_recovery << ". Continuing crash"
				" recovery even though we cannot access the"
				" files for tablespace " << space_id << ".";
			break;
		}
	}
}

/** Parse or process a MLOG_FILE_* record.
@param[in]	ptr		redo log record
@param[in]	end		end of the redo log buffer
@param[in]	space_id	the tablespace ID
@param[in]	first_page_no	first page number in the file
@param[in]	type		MLOG_FILE_NAME or MLOG_FILE_DELETE
or MLOG_FILE_CREATE2 or MLOG_FILE_RENAME2
@param[in]	apply		whether to apply the record
@return pointer to next redo log record
@retval NULL if this log record was truncated */
static
byte*
fil_name_parse(
	byte*		ptr,
	const byte*	end,
	ulint		space_id,
	ulint		first_page_no,
	mlog_id_t	type,
	bool		apply)
{
	if (type == MLOG_FILE_CREATE2) {
		if (end < ptr + 4) {
			return(NULL);
		}
		ptr += 4;
	}

	if (end < ptr + 2) {
		return(NULL);
	}

	ulint	len = mach_read_from_2(ptr);
	ptr += 2;
	if (end < ptr + len) {
		return(NULL);
	}

	/* MLOG_FILE_* records should only be written for
	user-created tablespaces. The name must be long enough
	and end in .ibd. */
	bool corrupt = is_predefined_tablespace(space_id)
		|| len < sizeof "/a.ibd\0"
		|| (!first_page_no != !memcmp(ptr + len - 5, DOT_IBD, 5))
		|| memchr(ptr, OS_PATH_SEPARATOR, len) == NULL;

	byte*	end_ptr	= ptr + len;

	switch (type) {
	default:
		ut_ad(0); // the caller checked this
	case MLOG_FILE_NAME:
		if (corrupt) {
			ib::error() << "MLOG_FILE_NAME incorrect:" << ptr;
			recv_sys->found_corrupt_log = true;
			break;
		}

		fil_name_process(
			reinterpret_cast<char*>(ptr), len, space_id, false);
		break;
	case MLOG_FILE_DELETE:
		if (corrupt) {
			ib::error() << "MLOG_FILE_DELETE incorrect:" << ptr;
			recv_sys->found_corrupt_log = true;
			break;
		}

		fil_name_process(
			reinterpret_cast<char*>(ptr), len, space_id, true);
		/* fall through */
	case MLOG_FILE_CREATE2:
		if (first_page_no) {
			ut_ad(first_page_no
			      == SRV_UNDO_TABLESPACE_SIZE_IN_PAGES);
			ut_a(srv_is_undo_tablespace(space_id));
			compile_time_assert(
				UT_ARR_SIZE(recv_sys->truncated_undo_spaces)
				== TRX_SYS_MAX_UNDO_SPACES);
			recv_sys_t::trunc& t = recv_sys->truncated_undo_spaces[
				space_id - srv_undo_space_id_start];
			t.lsn = recv_sys->recovered_lsn;
			t.pages = uint32_t(first_page_no);
		} else if (log_file_op) {
			log_file_op(space_id,
				    type == MLOG_FILE_CREATE2 ? ptr - 4 : NULL,
				    ptr, len, NULL, 0);
		}
		break;
	case MLOG_FILE_RENAME2:
		if (corrupt) {
			ib::error() << "MLOG_FILE_RENAME2 incorrect:" << ptr;
			recv_sys->found_corrupt_log = true;
		}

		/* The new name follows the old name. */
		byte*	new_name = end_ptr + 2;
		if (end < new_name) {
			return(NULL);
		}

		ulint	new_len = mach_read_from_2(end_ptr);

		if (end < end_ptr + 2 + new_len) {
			return(NULL);
		}

		end_ptr += 2 + new_len;

		corrupt = corrupt
			|| new_len < sizeof "/a.ibd\0"
			|| memcmp(new_name + new_len - 5, DOT_IBD, 5) != 0
			|| !memchr(new_name, OS_PATH_SEPARATOR, new_len);

		if (corrupt) {
			ib::error() << "MLOG_FILE_RENAME2 new_name incorrect:" << ptr
				    << " new_name: " << new_name;
			recv_sys->found_corrupt_log = true;
			break;
		}

		fil_name_process(
			reinterpret_cast<char*>(ptr), len,
			space_id, false);
		fil_name_process(
			reinterpret_cast<char*>(new_name), new_len,
			space_id, false);

		if (log_file_op) {
			log_file_op(space_id, NULL,
				    ptr, len, new_name, new_len);
		}

		if (!apply) {
			break;
		}
		if (!fil_op_replay_rename(
			    space_id, first_page_no,
			    reinterpret_cast<const char*>(ptr),
			    reinterpret_cast<const char*>(new_name))) {
			recv_sys->found_corrupt_fs = true;
		}
	}

	return(end_ptr);
}

/** Clean up after recv_sys_init() */
void
recv_sys_close()
{
	if (recv_sys != NULL) {
		recv_sys->dblwr.pages.clear();

		if (recv_sys->addr_hash != NULL) {
			hash_table_free(recv_sys->addr_hash);
		}

		if (recv_sys->heap != NULL) {
			mem_heap_free(recv_sys->heap);
		}

		if (recv_sys->flush_start != NULL) {
			os_event_destroy(recv_sys->flush_start);
		}

		if (recv_sys->flush_end != NULL) {
			os_event_destroy(recv_sys->flush_end);
		}

		if (recv_sys->buf != NULL) {
			ut_free_dodump(recv_sys->buf, recv_sys->buf_size);
		}

		ut_ad(!recv_writer_thread_active);
		mutex_free(&recv_sys->writer_mutex);

		mutex_free(&recv_sys->mutex);

		ut_free(recv_sys);
		recv_sys = NULL;
	}

	recv_spaces.clear();
}

/************************************************************
Reset the state of the recovery system variables. */
void
recv_sys_var_init(void)
/*===================*/
{
	recv_recovery_on = false;
	recv_needed_recovery = false;
	recv_lsn_checks_on = false;
	recv_no_ibuf_operations = false;
	recv_previous_parsed_rec_type = MLOG_SINGLE_REC_FLAG;
	recv_previous_parsed_rec_offset	= 0;
	recv_previous_parsed_rec_is_multi = 0;
	recv_n_pool_free_frames	= 256;
	recv_max_page_lsn = 0;
}

/******************************************************************//**
recv_writer thread tasked with flushing dirty pages from the buffer
pools.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(recv_writer_thread)(
/*===============================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	my_thread_init();
	ut_ad(!srv_read_only_mode);

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(recv_writer_thread_key);
#endif /* UNIV_PFS_THREAD */

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "recv_writer thread running, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */

	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {

		/* Wait till we get a signal to clean the LRU list.
		Bounded by max wait time of 100ms. */
		int64_t      sig_count = os_event_reset(buf_flush_event);
		os_event_wait_time_low(buf_flush_event, 100000, sig_count);

		mutex_enter(&recv_sys->writer_mutex);

		if (!recv_recovery_on) {
			mutex_exit(&recv_sys->writer_mutex);
			break;
		}

		/* Flush pages from end of LRU if required */
		os_event_reset(recv_sys->flush_end);
		recv_sys->flush_type = BUF_FLUSH_LRU;
		os_event_set(recv_sys->flush_start);
		os_event_wait(recv_sys->flush_end);

		mutex_exit(&recv_sys->writer_mutex);
	}

	recv_writer_thread_active = false;

	my_thread_end();
	/* We count the number of threads in os_thread_exit().
	A created thread should always use that to exit and not
	use return() to exit. */
	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

/** Initialize the redo log recovery subsystem. */
void
recv_sys_init()
{
	ut_ad(recv_sys == NULL);

	recv_sys = static_cast<recv_sys_t*>(ut_zalloc_nokey(sizeof(*recv_sys)));

	mutex_create(LATCH_ID_RECV_SYS, &recv_sys->mutex);
	mutex_create(LATCH_ID_RECV_WRITER, &recv_sys->writer_mutex);

	recv_sys->heap = mem_heap_create_typed(256, MEM_HEAP_FOR_RECV_SYS);

	if (!srv_read_only_mode) {
		recv_sys->flush_start = os_event_create(0);
		recv_sys->flush_end = os_event_create(0);
	}

	ulint size = buf_pool_get_curr_size();
	/* Set appropriate value of recv_n_pool_free_frames. */
	if (size >= 10 << 20) {
		/* Buffer pool of size greater than 10 MB. */
		recv_n_pool_free_frames = 512;
	}

	recv_sys->buf = static_cast<byte*>(
		ut_malloc_dontdump(RECV_PARSING_BUF_SIZE));
	recv_sys->buf_size = RECV_PARSING_BUF_SIZE;

	recv_sys->addr_hash = hash_create(size / 512);
	recv_sys->progress_time = ut_time();
	recv_max_page_lsn = 0;

	/* Call the constructor for recv_sys_t::dblwr member */
	new (&recv_sys->dblwr) recv_dblwr_t();
}

/** Empty a fully processed hash table. */
static
void
recv_sys_empty_hash()
{
	ut_ad(mutex_own(&(recv_sys->mutex)));
	ut_a(recv_sys->n_addrs == 0);

	hash_table_free(recv_sys->addr_hash);
	mem_heap_empty(recv_sys->heap);

	recv_sys->addr_hash = hash_create(buf_pool_get_curr_size() / 512);
}

/********************************************************//**
Frees the recovery system. */
void
recv_sys_debug_free(void)
/*=====================*/
{
	mutex_enter(&(recv_sys->mutex));

	hash_table_free(recv_sys->addr_hash);
	mem_heap_free(recv_sys->heap);
	ut_free_dodump(recv_sys->buf, recv_sys->buf_size);

	recv_sys->buf_size = 0;
	recv_sys->buf = NULL;
	recv_sys->heap = NULL;
	recv_sys->addr_hash = NULL;

	/* wake page cleaner up to progress */
	if (!srv_read_only_mode) {
		ut_ad(!recv_recovery_on);
		ut_ad(!recv_writer_thread_active);
		os_event_reset(buf_flush_event);
		os_event_set(recv_sys->flush_start);
	}

	mutex_exit(&(recv_sys->mutex));
}

/** Read a log segment to log_sys.buf.
@param[in,out]	start_lsn	in: read area start,
out: the last read valid lsn
@param[in]	end_lsn		read area end
@return	whether no invalid blocks (e.g checksum mismatch) were found */
bool log_t::files::read_log_seg(lsn_t* start_lsn, lsn_t end_lsn)
{
	ulint	len;
	bool success = true;
	ut_ad(log_sys.mutex.is_owned());
	ut_ad(!(*start_lsn % OS_FILE_LOG_BLOCK_SIZE));
	ut_ad(!(end_lsn % OS_FILE_LOG_BLOCK_SIZE));
	byte* buf = log_sys.buf;
loop:
	lsn_t source_offset = calc_lsn_offset(*start_lsn);

	ut_a(end_lsn - *start_lsn <= ULINT_MAX);
	len = (ulint) (end_lsn - *start_lsn);

	ut_ad(len != 0);

	const bool at_eof = (source_offset % file_size) + len > file_size;
	if (at_eof) {
		/* If the above condition is true then len (which is ulint)
		is > the expression below, so the typecast is ok */
		len = ulint(file_size - (source_offset % file_size));
	}

	log_sys.n_log_ios++;

	MONITOR_INC(MONITOR_LOG_IO);

	ut_a((source_offset >> srv_page_size_shift) <= ULINT_MAX);

	const ulint	page_no = ulint(source_offset >> srv_page_size_shift);

	fil_io(IORequestLogRead, true,
	       page_id_t(SRV_LOG_SPACE_FIRST_ID, page_no),
	       univ_page_size,
	       ulint(source_offset & (srv_page_size - 1)),
	       len, buf, NULL);

	for (ulint l = 0; l < len; l += OS_FILE_LOG_BLOCK_SIZE,
		     buf += OS_FILE_LOG_BLOCK_SIZE,
		     (*start_lsn) += OS_FILE_LOG_BLOCK_SIZE) {
		const ulint block_number = log_block_get_hdr_no(buf);

		if (block_number != log_block_convert_lsn_to_no(*start_lsn)) {
			/* Garbage or an incompletely written log block.
			We will not report any error, because this can
			happen when InnoDB was killed while it was
			writing redo log. We simply treat this as an
			abrupt end of the redo log. */
			end_lsn = *start_lsn;
			break;
		}

		if (innodb_log_checksums || is_encrypted()) {
			ulint crc = log_block_calc_checksum_crc32(buf);
			ulint cksum = log_block_get_checksum(buf);

			DBUG_EXECUTE_IF("log_intermittent_checksum_mismatch", {
					 static int block_counter;
					 if (block_counter++ == 0) {
						 cksum = crc + 1;
					 }
			 });

			if (crc != cksum) {
				ib::error() << "Invalid log block checksum."
					    << " block: " << block_number
					    << " checkpoint no: "
					    << log_block_get_checkpoint_no(buf)
					    << " expected: " << crc
					    << " found: " << cksum;
				end_lsn = *start_lsn;
				success = false;
				break;
			}

			if (is_encrypted()) {
				log_crypt(buf, *start_lsn,
					  OS_FILE_LOG_BLOCK_SIZE, true);
			}
		}

		ulint dl = log_block_get_data_len(buf);
		if (dl < LOG_BLOCK_HDR_SIZE
		    || (dl > OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE
			&& dl != OS_FILE_LOG_BLOCK_SIZE)) {
			recv_sys->found_corrupt_log = true;
			end_lsn = *start_lsn;
			break;
		}
	}

	if (recv_sys->report(ut_time())) {
		ib::info() << "Read redo log up to LSN=" << *start_lsn;
		service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
			"Read redo log up to LSN=" LSN_PF,
			*start_lsn);
	}

	if (*start_lsn != end_lsn) {
		goto loop;
	}

	return(success);
}



/********************************************************//**
Copies a log segment from the most up-to-date log group to the other log
groups, so that they all contain the latest log data. Also writes the info
about the latest checkpoint to the groups, and inits the fields in the group
memory structs to up-to-date values. */
static
void
recv_synchronize_groups()
{
	const lsn_t recovered_lsn = recv_sys->recovered_lsn;

	/* Read the last recovered log block to the recovery system buffer:
	the block is always incomplete */

	lsn_t start_lsn = ut_uint64_align_down(recovered_lsn,
					       OS_FILE_LOG_BLOCK_SIZE);
	log_sys.log.read_log_seg(&start_lsn,
				 start_lsn + OS_FILE_LOG_BLOCK_SIZE);
	log_sys.log.set_fields(recovered_lsn);

	/* Copy the checkpoint info to the log; remember that we have
	incremented checkpoint_no by one, and the info will not be written
	over the max checkpoint info, thus making the preservation of max
	checkpoint info on disk certain */

	if (!srv_read_only_mode) {
		log_write_checkpoint_info(true, 0);
		log_mutex_enter();
	}
}

/** Check the consistency of a log header block.
@param[in]	log header block
@return true if ok */
static
bool
recv_check_log_header_checksum(
	const byte*	buf)
{
	return(log_block_get_checksum(buf)
	       == log_block_calc_checksum_crc32(buf));
}

/** Find the latest checkpoint in the format-0 log header.
@param[out]	max_field	LOG_CHECKPOINT_1 or LOG_CHECKPOINT_2
@return error code or DB_SUCCESS */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
recv_find_max_checkpoint_0(ulint* max_field)
{
	ib_uint64_t	max_no = 0;
	ib_uint64_t	checkpoint_no;
	byte*		buf	= log_sys.checkpoint_buf;

	ut_ad(log_sys.log.format == 0);

	/** Offset of the first checkpoint checksum */
	static const uint CHECKSUM_1 = 288;
	/** Offset of the second checkpoint checksum */
	static const uint CHECKSUM_2 = CHECKSUM_1 + 4;
	/** Most significant bits of the checkpoint offset */
	static const uint OFFSET_HIGH32 = CHECKSUM_2 + 12;
	/** Least significant bits of the checkpoint offset */
	static const uint OFFSET_LOW32 = 16;

	bool found = false;

	for (ulint field = LOG_CHECKPOINT_1; field <= LOG_CHECKPOINT_2;
	     field += LOG_CHECKPOINT_2 - LOG_CHECKPOINT_1) {
		log_header_read(field);

		if (static_cast<uint32_t>(ut_fold_binary(buf, CHECKSUM_1))
		    != mach_read_from_4(buf + CHECKSUM_1)
		    || static_cast<uint32_t>(
			    ut_fold_binary(buf + LOG_CHECKPOINT_LSN,
					   CHECKSUM_2 - LOG_CHECKPOINT_LSN))
		    != mach_read_from_4(buf + CHECKSUM_2)) {
			DBUG_LOG("ib_log",
				 "invalid pre-10.2.2 checkpoint " << field);
			continue;
		}

		checkpoint_no = mach_read_from_8(
			buf + LOG_CHECKPOINT_NO);

		if (!log_crypt_101_read_checkpoint(buf)) {
			ib::error() << "Decrypting checkpoint failed";
			continue;
		}

		DBUG_PRINT("ib_log",
			   ("checkpoint " UINT64PF " at " LSN_PF " found",
			    checkpoint_no,
			    mach_read_from_8(buf + LOG_CHECKPOINT_LSN)));

		if (checkpoint_no >= max_no) {
			found = true;
			*max_field = field;
			max_no = checkpoint_no;

			log_sys.log.lsn = mach_read_from_8(
				buf + LOG_CHECKPOINT_LSN);
			log_sys.log.lsn_offset = static_cast<ib_uint64_t>(
				mach_read_from_4(buf + OFFSET_HIGH32)) << 32
				| mach_read_from_4(buf + OFFSET_LOW32);
		}
	}

	if (found) {
		return(DB_SUCCESS);
	}

	ib::error() << "Upgrade after a crash is not supported."
		" This redo log was created before MariaDB 10.2.2,"
		" and we did not find a valid checkpoint."
		" Please follow the instructions at"
		" https://mariadb.com/kb/en/library/upgrading/";
	return(DB_ERROR);
}

/** Determine if a pre-MySQL 5.7.9/MariaDB 10.2.2 redo log is clean.
@param[in]	lsn	checkpoint LSN
@param[in]	crypt	whether the log might be encrypted
@return error code
@retval	DB_SUCCESS	if the redo log is clean
@retval DB_ERROR	if the redo log is corrupted or dirty */
static dberr_t recv_log_format_0_recover(lsn_t lsn, bool crypt)
{
	log_mutex_enter();
	const lsn_t	source_offset = log_sys.log.calc_lsn_offset(lsn);
	log_mutex_exit();
	const ulint	page_no = ulint(source_offset >> srv_page_size_shift);
	byte*		buf = log_sys.buf;

	static const char* NO_UPGRADE_RECOVERY_MSG =
		"Upgrade after a crash is not supported."
		" This redo log was created before MariaDB 10.2.2";

	fil_io(IORequestLogRead, true,
	       page_id_t(SRV_LOG_SPACE_FIRST_ID, page_no),
	       univ_page_size,
	       ulint((source_offset & ~(OS_FILE_LOG_BLOCK_SIZE - 1))
		     & (srv_page_size - 1)),
	       OS_FILE_LOG_BLOCK_SIZE, buf, NULL);

	if (log_block_calc_checksum_format_0(buf)
	    != log_block_get_checksum(buf)
	    && !log_crypt_101_read_block(buf)) {
		ib::error() << NO_UPGRADE_RECOVERY_MSG
			<< ", and it appears corrupted.";
		return(DB_CORRUPTION);
	}

	if (log_block_get_data_len(buf)
	    == (source_offset & (OS_FILE_LOG_BLOCK_SIZE - 1))) {
	} else if (crypt) {
		ib::error() << "Cannot decrypt log for upgrading."
			" The encrypted log was created"
			" before MariaDB 10.2.2.";
		return DB_ERROR;
	} else {
		ib::error() << NO_UPGRADE_RECOVERY_MSG << ".";
		return(DB_ERROR);
	}

	/* Mark the redo log for upgrading. */
	srv_log_file_size = 0;
	recv_sys->parse_start_lsn = recv_sys->recovered_lsn
		= recv_sys->scanned_lsn
		= recv_sys->mlog_checkpoint_lsn = lsn;
	log_sys.last_checkpoint_lsn = log_sys.next_checkpoint_lsn
		= log_sys.lsn = log_sys.write_lsn
		= log_sys.current_flush_lsn = log_sys.flushed_to_disk_lsn
		= lsn;
	log_sys.next_checkpoint_no = 0;
	return(DB_SUCCESS);
}

/** Determine if a redo log from MariaDB 10.4 is clean.
@return	error code
@retval	DB_SUCCESS	if the redo log is clean
@retval	DB_CORRUPTION	if the redo log is corrupted
@retval	DB_ERROR	if the redo log is not empty */
static dberr_t recv_log_recover_10_4()
{
	ut_ad(!log_sys.is_encrypted());
	const lsn_t	lsn = log_sys.log.lsn;
	log_mutex_enter();
	const lsn_t	source_offset = log_sys.log.calc_lsn_offset(lsn);
	log_mutex_exit();
	const ulint	page_no
		= (ulint) (source_offset / univ_page_size.physical());
	byte*		buf = log_sys.buf;

	fil_io(IORequestLogRead, true,
	       page_id_t(SRV_LOG_SPACE_FIRST_ID, page_no),
	       univ_page_size,
	       (ulint) ((source_offset & ~(OS_FILE_LOG_BLOCK_SIZE - 1))
			% univ_page_size.physical()),
	       OS_FILE_LOG_BLOCK_SIZE, buf, NULL);

	if (log_block_calc_checksum(buf) != log_block_get_checksum(buf)) {
		return DB_CORRUPTION;
	}

	/* On a clean shutdown, the redo log will be logically empty
	after the checkpoint lsn. */

	if (log_block_get_data_len(buf)
	    != (source_offset & (OS_FILE_LOG_BLOCK_SIZE - 1))) {
		return DB_ERROR;
	}

	/* Mark the redo log for downgrading. */
	srv_log_file_size = 0;
	recv_sys->parse_start_lsn = recv_sys->recovered_lsn
		= recv_sys->scanned_lsn
		= recv_sys->mlog_checkpoint_lsn = lsn;
	log_sys.last_checkpoint_lsn = log_sys.next_checkpoint_lsn
		= log_sys.lsn = log_sys.write_lsn
		= log_sys.current_flush_lsn = log_sys.flushed_to_disk_lsn
		= lsn;
	log_sys.next_checkpoint_no = 0;
	return DB_SUCCESS;
}

/** Find the latest checkpoint in the log header.
@param[out]	max_field	LOG_CHECKPOINT_1 or LOG_CHECKPOINT_2
@return error code or DB_SUCCESS */
dberr_t
recv_find_max_checkpoint(ulint* max_field)
{
	ib_uint64_t	max_no;
	ib_uint64_t	checkpoint_no;
	ulint		field;
	byte*		buf;

	max_no = 0;
	*max_field = 0;

	buf = log_sys.checkpoint_buf;

	log_header_read(0);
	/* Check the header page checksum. There was no
	checksum in the first redo log format (version 0). */
	log_sys.log.format = mach_read_from_4(buf + LOG_HEADER_FORMAT);
	log_sys.log.subformat = log_sys.log.format != LOG_HEADER_FORMAT_3_23
		? mach_read_from_4(buf + LOG_HEADER_SUBFORMAT)
		: 0;
	if (log_sys.log.format != LOG_HEADER_FORMAT_3_23
	    && !recv_check_log_header_checksum(buf)) {
		ib::error() << "Invalid redo log header checksum.";
		return(DB_CORRUPTION);
	}

	char creator[LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR + 1];

	memcpy(creator, buf + LOG_HEADER_CREATOR, sizeof creator);
	/* Ensure that the string is NUL-terminated. */
	creator[LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR] = 0;

	switch (log_sys.log.format) {
	case LOG_HEADER_FORMAT_3_23:
		return(recv_find_max_checkpoint_0(max_field));
	case LOG_HEADER_FORMAT_10_2:
	case LOG_HEADER_FORMAT_10_2 | LOG_HEADER_FORMAT_ENCRYPTED:
	case LOG_HEADER_FORMAT_CURRENT:
	case LOG_HEADER_FORMAT_CURRENT | LOG_HEADER_FORMAT_ENCRYPTED:
	case LOG_HEADER_FORMAT_10_4:
		/* We can only parse the unencrypted LOG_HEADER_FORMAT_10_4.
		The encrypted format uses a larger redo log block trailer. */
		break;
	default:
		ib::error() << "Unsupported redo log format."
			" The redo log was created with " << creator << ".";
		return(DB_ERROR);
	}

	for (field = LOG_CHECKPOINT_1; field <= LOG_CHECKPOINT_2;
	     field += LOG_CHECKPOINT_2 - LOG_CHECKPOINT_1) {

		log_header_read(field);

		const ulint crc32 = log_block_calc_checksum_crc32(buf);
		const ulint cksum = log_block_get_checksum(buf);

		if (crc32 != cksum) {
			DBUG_PRINT("ib_log",
				   ("invalid checkpoint,"
				    " at " ULINTPF
				    ", checksum " ULINTPFx
				    " expected " ULINTPFx,
				    field, cksum, crc32));
			continue;
		}

		if (log_sys.is_encrypted()
		    && !log_crypt_read_checkpoint_buf(buf)) {
			ib::error() << "Reading checkpoint"
				" encryption info failed.";
			continue;
		}

		checkpoint_no = mach_read_from_8(
			buf + LOG_CHECKPOINT_NO);

		DBUG_PRINT("ib_log",
			   ("checkpoint " UINT64PF " at " LSN_PF " found",
			    checkpoint_no, mach_read_from_8(
				    buf + LOG_CHECKPOINT_LSN)));

		if (checkpoint_no >= max_no) {
			*max_field = field;
			max_no = checkpoint_no;
			log_sys.log.lsn = mach_read_from_8(
				buf + LOG_CHECKPOINT_LSN);
			log_sys.log.lsn_offset = mach_read_from_8(
				buf + LOG_CHECKPOINT_OFFSET);
			log_sys.next_checkpoint_no = checkpoint_no;
		}
	}

	if (*max_field == 0) {
		/* Before 10.2.2, we could get here during database
		initialization if we created an ib_logfile0 file that
		was filled with zeroes, and were killed. After
		10.2.2, we would reject such a file already earlier,
		when checking the file header. */
		ib::error() << "No valid checkpoint found"
			" (corrupted redo log)."
			" You can try --innodb-force-recovery=6"
			" as a last resort.";
		return(DB_ERROR);
	}

	if (log_sys.log.format == LOG_HEADER_FORMAT_10_4) {
		dberr_t err = recv_log_recover_10_4();
		if (err != DB_SUCCESS) {
			ib::error()
				<< "Downgrade after a crash is not supported."
				" The redo log was created with " << creator
				<< (err == DB_ERROR
				    ? "." : ", and it appears corrupted.");
		}
		return err;
	}

	return DB_SUCCESS;
}

/** Try to parse a single log record body and also applies it if
specified.
@param[in]	type		redo log entry type
@param[in]	ptr		redo log record body
@param[in]	end_ptr		end of buffer
@param[in]	space_id	tablespace identifier
@param[in]	page_no		page number
@param[in]	apply		whether to apply the record
@param[in,out]	block		buffer block, or NULL if
a page log record should not be applied
or if it is a MLOG_FILE_ operation
@param[in,out]	mtr		mini-transaction, or NULL if
a page log record should not be applied
@return log record end, NULL if not a complete record */
static
byte*
recv_parse_or_apply_log_rec_body(
	mlog_id_t	type,
	byte*		ptr,
	byte*		end_ptr,
	ulint		space_id,
	ulint		page_no,
	bool		apply,
	buf_block_t*	block,
	mtr_t*		mtr)
{
	ut_ad(!block == !mtr);
	ut_ad(!apply || recv_sys->mlog_checkpoint_lsn != 0);

	switch (type) {
	case MLOG_FILE_NAME:
	case MLOG_FILE_DELETE:
	case MLOG_FILE_CREATE2:
	case MLOG_FILE_RENAME2:
		ut_ad(block == NULL);
		/* Collect the file names when parsing the log,
		before applying any log records. */
		return(fil_name_parse(ptr, end_ptr, space_id, page_no, type,
				      apply));
	case MLOG_INDEX_LOAD:
		if (end_ptr < ptr + 8) {
			return(NULL);
		}
		return(ptr + 8);
	case MLOG_TRUNCATE:
		if (log_truncate) {
			ut_ad(srv_operation != SRV_OPERATION_NORMAL);
			log_truncate();
			recv_sys->found_corrupt_fs = true;
			return NULL;
		}
		return(truncate_t::parse_redo_entry(ptr, end_ptr, space_id));

	default:
		break;
	}

	dict_index_t*	index	= NULL;
	page_t*		page;
	page_zip_des_t*	page_zip;
#ifdef UNIV_DEBUG
	ulint		page_type;
#endif /* UNIV_DEBUG */

	if (block) {
		/* Applying a page log record. */
		ut_ad(apply);
		page = block->frame;
		page_zip = buf_block_get_page_zip(block);
		ut_d(page_type = fil_page_get_type(page));
	} else if (apply
		   && !is_predefined_tablespace(space_id)
		   && recv_spaces.find(space_id) == recv_spaces.end()) {
		if (recv_sys->recovered_lsn < recv_sys->mlog_checkpoint_lsn) {
			/* We have not seen all records between the
			checkpoint and MLOG_CHECKPOINT. There should be
			a MLOG_FILE_DELETE for this tablespace later. */
			recv_spaces.insert(
				std::make_pair(space_id,
					       file_name_t("", false)));
			goto parse_log;
		}

		ib::error() << "Missing MLOG_FILE_NAME or MLOG_FILE_DELETE"
			" for redo log record " << type << " (page "
			    << space_id << ":" << page_no << ") at "
			    << recv_sys->recovered_lsn << ".";
		recv_sys->found_corrupt_log = true;
		return(NULL);
	} else {
parse_log:
		/* Parsing a page log record. */
		page = NULL;
		page_zip = NULL;
		ut_d(page_type = FIL_PAGE_TYPE_ALLOCATED);
	}

	const byte*	old_ptr = ptr;

	switch (type) {
#ifdef UNIV_LOG_LSN_DEBUG
	case MLOG_LSN:
		/* The LSN is checked in recv_parse_log_rec(). */
		break;
#endif /* UNIV_LOG_LSN_DEBUG */
	case MLOG_1BYTE: case MLOG_2BYTES: case MLOG_4BYTES: case MLOG_8BYTES:
#ifdef UNIV_DEBUG
		if (page && page_type == FIL_PAGE_TYPE_ALLOCATED
		    && end_ptr >= ptr + 2) {
			/* It is OK to set FIL_PAGE_TYPE and certain
			list node fields on an empty page.  Any other
			write is not OK. */

			/* NOTE: There may be bogus assertion failures for
			dict_hdr_create(), trx_rseg_header_create(),
			trx_sys_create_doublewrite_buf(), and
			trx_sysf_create().
			These are only called during database creation. */
			ulint	offs = mach_read_from_2(ptr);

			switch (type) {
			default:
				ut_error;
			case MLOG_2BYTES:
				/* Note that this can fail when the
				redo log been written with something
				older than InnoDB Plugin 1.0.4. */
				ut_ad(offs == FIL_PAGE_TYPE
				      || srv_is_undo_tablespace(space_id)
				      || offs == IBUF_TREE_SEG_HEADER
				      + IBUF_HEADER + FSEG_HDR_OFFSET
				      || offs == PAGE_BTR_IBUF_FREE_LIST
				      + PAGE_HEADER + FIL_ADDR_BYTE
				      || offs == PAGE_BTR_IBUF_FREE_LIST
				      + PAGE_HEADER + FIL_ADDR_BYTE
				      + FIL_ADDR_SIZE
				      || offs == PAGE_BTR_SEG_LEAF
				      + PAGE_HEADER + FSEG_HDR_OFFSET
				      || offs == PAGE_BTR_SEG_TOP
				      + PAGE_HEADER + FSEG_HDR_OFFSET
				      || offs == PAGE_BTR_IBUF_FREE_LIST_NODE
				      + PAGE_HEADER + FIL_ADDR_BYTE
				      + 0 /*FLST_PREV*/
				      || offs == PAGE_BTR_IBUF_FREE_LIST_NODE
				      + PAGE_HEADER + FIL_ADDR_BYTE
				      + FIL_ADDR_SIZE /*FLST_NEXT*/);
				break;
			case MLOG_4BYTES:
				/* Note that this can fail when the
				redo log been written with something
				older than InnoDB Plugin 1.0.4. */
				ut_ad(0
				      /* fil_crypt_rotate_page() writes this */
				      || offs == FIL_PAGE_SPACE_ID
				      || srv_is_undo_tablespace(space_id)
				      || offs == IBUF_TREE_SEG_HEADER
				      + IBUF_HEADER + FSEG_HDR_SPACE
				      || offs == IBUF_TREE_SEG_HEADER
				      + IBUF_HEADER + FSEG_HDR_PAGE_NO
				      || offs == PAGE_BTR_IBUF_FREE_LIST
				      + PAGE_HEADER/* flst_init */
				      || offs == PAGE_BTR_IBUF_FREE_LIST
				      + PAGE_HEADER + FIL_ADDR_PAGE
				      || offs == PAGE_BTR_IBUF_FREE_LIST
				      + PAGE_HEADER + FIL_ADDR_PAGE
				      + FIL_ADDR_SIZE
				      || offs == PAGE_BTR_SEG_LEAF
				      + PAGE_HEADER + FSEG_HDR_PAGE_NO
				      || offs == PAGE_BTR_SEG_LEAF
				      + PAGE_HEADER + FSEG_HDR_SPACE
				      || offs == PAGE_BTR_SEG_TOP
				      + PAGE_HEADER + FSEG_HDR_PAGE_NO
				      || offs == PAGE_BTR_SEG_TOP
				      + PAGE_HEADER + FSEG_HDR_SPACE
				      || offs == PAGE_BTR_IBUF_FREE_LIST_NODE
				      + PAGE_HEADER + FIL_ADDR_PAGE
				      + 0 /*FLST_PREV*/
				      || offs == PAGE_BTR_IBUF_FREE_LIST_NODE
				      + PAGE_HEADER + FIL_ADDR_PAGE
				      + FIL_ADDR_SIZE /*FLST_NEXT*/);
				break;
			}
		}
#endif /* UNIV_DEBUG */
		ptr = mlog_parse_nbytes(type, ptr, end_ptr, page, page_zip);
		if (ptr != NULL && page != NULL
		    && page_no == 0 && type == MLOG_4BYTES) {
			ulint	offs = mach_read_from_2(old_ptr);
			switch (offs) {
				fil_space_t*	space;
				ulint		val;
			default:
				break;
			case FSP_HEADER_OFFSET + FSP_SPACE_FLAGS:
			case FSP_HEADER_OFFSET + FSP_SIZE:
			case FSP_HEADER_OFFSET + FSP_FREE_LIMIT:
			case FSP_HEADER_OFFSET + FSP_FREE + FLST_LEN:
				space = fil_space_get(space_id);
				ut_a(space != NULL);
				val = mach_read_from_4(page + offs);

				switch (offs) {
				case FSP_HEADER_OFFSET + FSP_SPACE_FLAGS:
					space->flags = val;
					break;
				case FSP_HEADER_OFFSET + FSP_SIZE:
					space->size_in_header = val;
					break;
				case FSP_HEADER_OFFSET + FSP_FREE_LIMIT:
					space->free_limit = val;
					break;
				case FSP_HEADER_OFFSET + FSP_FREE + FLST_LEN:
					space->free_len = val;
					ut_ad(val == flst_get_len(
						      page + offs));
					break;
				}
			}
		}
		break;
	case MLOG_REC_INSERT: case MLOG_COMP_REC_INSERT:
		ut_ad(!page || fil_page_type_is_index(page_type));

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type == MLOG_COMP_REC_INSERT,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = page_cur_parse_insert_rec(FALSE, ptr, end_ptr,
							block, index, mtr);
		}
		break;
	case MLOG_REC_CLUST_DELETE_MARK: case MLOG_COMP_REC_CLUST_DELETE_MARK:
		ut_ad(!page || fil_page_type_is_index(page_type));

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type == MLOG_COMP_REC_CLUST_DELETE_MARK,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = btr_cur_parse_del_mark_set_clust_rec(
				ptr, end_ptr, page, page_zip, index);
		}
		break;
	case MLOG_REC_SEC_DELETE_MARK:
		ut_ad(!page || fil_page_type_is_index(page_type));
		ptr = btr_cur_parse_del_mark_set_sec_rec(ptr, end_ptr,
							 page, page_zip);
		break;
	case MLOG_REC_UPDATE_IN_PLACE: case MLOG_COMP_REC_UPDATE_IN_PLACE:
		ut_ad(!page || fil_page_type_is_index(page_type));

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type == MLOG_COMP_REC_UPDATE_IN_PLACE,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = btr_cur_parse_update_in_place(ptr, end_ptr, page,
							    page_zip, index);
		}
		break;
	case MLOG_LIST_END_DELETE: case MLOG_COMP_LIST_END_DELETE:
	case MLOG_LIST_START_DELETE: case MLOG_COMP_LIST_START_DELETE:
		ut_ad(!page || fil_page_type_is_index(page_type));

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type == MLOG_COMP_LIST_END_DELETE
				     || type == MLOG_COMP_LIST_START_DELETE,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = page_parse_delete_rec_list(type, ptr, end_ptr,
							 block, index, mtr);
		}
		break;
	case MLOG_LIST_END_COPY_CREATED: case MLOG_COMP_LIST_END_COPY_CREATED:
		ut_ad(!page || fil_page_type_is_index(page_type));

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type == MLOG_COMP_LIST_END_COPY_CREATED,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = page_parse_copy_rec_list_to_created_page(
				ptr, end_ptr, block, index, mtr);
		}
		break;
	case MLOG_PAGE_REORGANIZE:
	case MLOG_COMP_PAGE_REORGANIZE:
	case MLOG_ZIP_PAGE_REORGANIZE:
		ut_ad(!page || fil_page_type_is_index(page_type));

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type != MLOG_PAGE_REORGANIZE,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = btr_parse_page_reorganize(
				ptr, end_ptr, index,
				type == MLOG_ZIP_PAGE_REORGANIZE,
				block, mtr);
		}
		break;
	case MLOG_PAGE_CREATE: case MLOG_COMP_PAGE_CREATE:
		/* Allow anything in page_type when creating a page. */
		ut_a(!page_zip);
		page_parse_create(block, type == MLOG_COMP_PAGE_CREATE, false);
		break;
	case MLOG_PAGE_CREATE_RTREE: case MLOG_COMP_PAGE_CREATE_RTREE:
		page_parse_create(block, type == MLOG_COMP_PAGE_CREATE_RTREE,
				  true);
		break;
	case MLOG_UNDO_INSERT:
		ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);
		ptr = trx_undo_parse_add_undo_rec(ptr, end_ptr, page);
		break;
	case MLOG_UNDO_ERASE_END:
		if (page) {
			ut_ad(page_type == FIL_PAGE_UNDO_LOG);
			trx_undo_erase_page_end(page);
		}
		break;
	case MLOG_UNDO_INIT:
		/* Allow anything in page_type when creating a page. */
		ptr = trx_undo_parse_page_init(ptr, end_ptr, page);
		break;
	case MLOG_UNDO_HDR_REUSE:
		ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);
		ptr = trx_undo_parse_page_header_reuse(ptr, end_ptr, page);
		break;
	case MLOG_UNDO_HDR_CREATE:
		ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);
		ptr = trx_undo_parse_page_header(ptr, end_ptr, page, mtr);
		break;
	case MLOG_REC_MIN_MARK: case MLOG_COMP_REC_MIN_MARK:
		ut_ad(!page || fil_page_type_is_index(page_type));
		/* On a compressed page, MLOG_COMP_REC_MIN_MARK
		will be followed by MLOG_COMP_REC_DELETE
		or MLOG_ZIP_WRITE_HEADER(FIL_PAGE_PREV, FIL_NULL)
		in the same mini-transaction. */
		ut_a(type == MLOG_COMP_REC_MIN_MARK || !page_zip);
		ptr = btr_parse_set_min_rec_mark(
			ptr, end_ptr, type == MLOG_COMP_REC_MIN_MARK,
			page, mtr);
		break;
	case MLOG_REC_DELETE: case MLOG_COMP_REC_DELETE:
		ut_ad(!page || fil_page_type_is_index(page_type));

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type == MLOG_COMP_REC_DELETE,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = page_cur_parse_delete_rec(ptr, end_ptr,
							block, index, mtr);
		}
		break;
	case MLOG_IBUF_BITMAP_INIT:
		/* Allow anything in page_type when creating a page. */
		ptr = ibuf_parse_bitmap_init(ptr, end_ptr, block, mtr);
		break;
	case MLOG_INIT_FILE_PAGE2:
		/* Allow anything in page_type when creating a page. */
		ptr = fsp_parse_init_file_page(ptr, end_ptr, block);
		break;
	case MLOG_WRITE_STRING:
		ptr = mlog_parse_string(ptr, end_ptr, page, page_zip);
		break;
	case MLOG_ZIP_WRITE_NODE_PTR:
		ut_ad(!page || fil_page_type_is_index(page_type));
		ptr = page_zip_parse_write_node_ptr(ptr, end_ptr,
						    page, page_zip);
		break;
	case MLOG_ZIP_WRITE_BLOB_PTR:
		ut_ad(!page || fil_page_type_is_index(page_type));
		ptr = page_zip_parse_write_blob_ptr(ptr, end_ptr,
						    page, page_zip);
		break;
	case MLOG_ZIP_WRITE_HEADER:
		ut_ad(!page || fil_page_type_is_index(page_type));
		ptr = page_zip_parse_write_header(ptr, end_ptr,
						  page, page_zip);
		break;
	case MLOG_ZIP_PAGE_COMPRESS:
		/* Allow anything in page_type when creating a page. */
		ptr = page_zip_parse_compress(ptr, end_ptr,
					      page, page_zip);
		break;
	case MLOG_ZIP_PAGE_COMPRESS_NO_DATA:
		if (NULL != (ptr = mlog_parse_index(
				ptr, end_ptr, TRUE, &index))) {

			ut_a(!page || ((ibool)!!page_is_comp(page)
				== dict_table_is_comp(index->table)));
			ptr = page_zip_parse_compress_no_data(
				ptr, end_ptr, page, page_zip, index);
		}
		break;
	case MLOG_ZIP_WRITE_TRX_ID:
		/* This must be a clustered index leaf page. */
		ut_ad(!page || page_type == FIL_PAGE_INDEX);
		ptr = page_zip_parse_write_trx_id(ptr, end_ptr,
						  page, page_zip);
		break;
	case MLOG_FILE_WRITE_CRYPT_DATA:
		dberr_t err;
		ptr = const_cast<byte*>(fil_parse_write_crypt_data(ptr, end_ptr, &err));

		if (err != DB_SUCCESS) {
			recv_sys->found_corrupt_log = TRUE;
		}
		break;
	default:
		ptr = NULL;
		ib::error() << "Incorrect log record type "
			<< ib::hex(unsigned(type));

		recv_sys->found_corrupt_log = true;
	}

	if (index) {
		dict_table_t*	table = index->table;

		dict_mem_index_free(index);
		dict_mem_table_free(table);
	}

	return(ptr);
}

/*********************************************************************//**
Calculates the fold value of a page file address: used in inserting or
searching for a log record in the hash table.
@return folded value */
UNIV_INLINE
ulint
recv_fold(
/*======*/
	ulint	space,	/*!< in: space */
	ulint	page_no)/*!< in: page number */
{
	return(ut_fold_ulint_pair(space, page_no));
}

/*********************************************************************//**
Calculates the hash value of a page file address: used in inserting or
searching for a log record in the hash table.
@return folded value */
UNIV_INLINE
ulint
recv_hash(
/*======*/
	ulint	space,	/*!< in: space */
	ulint	page_no)/*!< in: page number */
{
	return(hash_calc_hash(recv_fold(space, page_no), recv_sys->addr_hash));
}

/*********************************************************************//**
Gets the hashed file address struct for a page.
@return file address struct, NULL if not found from the hash table */
static
recv_addr_t*
recv_get_fil_addr_struct(
/*=====================*/
	ulint	space,	/*!< in: space id */
	ulint	page_no)/*!< in: page number */
{
	recv_addr_t*	recv_addr;

	for (recv_addr = static_cast<recv_addr_t*>(
			HASH_GET_FIRST(recv_sys->addr_hash,
				       recv_hash(space, page_no)));
	     recv_addr != 0;
	     recv_addr = static_cast<recv_addr_t*>(
		     HASH_GET_NEXT(addr_hash, recv_addr))) {

		if (recv_addr->space == space
		    && recv_addr->page_no == page_no) {

			return(recv_addr);
		}
	}

	return(NULL);
}

/*******************************************************************//**
Adds a new log record to the hash table of log records. */
static
void
recv_add_to_hash_table(
/*===================*/
	mlog_id_t	type,		/*!< in: log record type */
	ulint		space,		/*!< in: space id */
	ulint		page_no,	/*!< in: page number */
	byte*		body,		/*!< in: log record body */
	byte*		rec_end,	/*!< in: log record end */
	lsn_t		start_lsn,	/*!< in: start lsn of the mtr */
	lsn_t		end_lsn)	/*!< in: end lsn of the mtr */
{
	recv_t*		recv;
	ulint		len;
	recv_data_t*	recv_data;
	recv_data_t**	prev_field;
	recv_addr_t*	recv_addr;

	ut_ad(type != MLOG_FILE_DELETE);
	ut_ad(type != MLOG_FILE_CREATE2);
	ut_ad(type != MLOG_FILE_RENAME2);
	ut_ad(type != MLOG_FILE_NAME);
	ut_ad(type != MLOG_DUMMY_RECORD);
	ut_ad(type != MLOG_CHECKPOINT);
	ut_ad(type != MLOG_INDEX_LOAD);
	ut_ad(type != MLOG_TRUNCATE);

	len = ulint(rec_end - body);

	recv = static_cast<recv_t*>(
		mem_heap_alloc(recv_sys->heap, sizeof(recv_t)));

	recv->type = type;
	recv->len = ulint(rec_end - body);
	recv->start_lsn = start_lsn;
	recv->end_lsn = end_lsn;

	recv_addr = recv_get_fil_addr_struct(space, page_no);

	if (recv_addr == NULL) {
		recv_addr = static_cast<recv_addr_t*>(
			mem_heap_alloc(recv_sys->heap, sizeof(recv_addr_t)));

		recv_addr->space = space;
		recv_addr->page_no = page_no;
		recv_addr->state = RECV_NOT_PROCESSED;

		UT_LIST_INIT(recv_addr->rec_list, &recv_t::rec_list);

		HASH_INSERT(recv_addr_t, addr_hash, recv_sys->addr_hash,
			    recv_fold(space, page_no), recv_addr);
		recv_sys->n_addrs++;
#if 0
		fprintf(stderr, "Inserting log rec for space %lu, page %lu\n",
			space, page_no);
#endif
	}

	UT_LIST_ADD_LAST(recv_addr->rec_list, recv);

	prev_field = &(recv->data);

	/* Store the log record body in chunks of less than srv_page_size:
	recv_sys->heap grows into the buffer pool, and bigger chunks could not
	be allocated */

	while (rec_end > body) {

		len = ulint(rec_end - body);

		if (len > RECV_DATA_BLOCK_SIZE) {
			len = RECV_DATA_BLOCK_SIZE;
		}

		recv_data = static_cast<recv_data_t*>(
			mem_heap_alloc(recv_sys->heap,
				       sizeof(recv_data_t) + len));

		*prev_field = recv_data;

		memcpy(recv_data + 1, body, len);

		prev_field = &(recv_data->next);

		body += len;
	}

	*prev_field = NULL;
}

/*********************************************************************//**
Copies the log record body from recv to buf. */
static
void
recv_data_copy_to_buf(
/*==================*/
	byte*	buf,	/*!< in: buffer of length at least recv->len */
	recv_t*	recv)	/*!< in: log record */
{
	recv_data_t*	recv_data;
	ulint		part_len;
	ulint		len;

	len = recv->len;
	recv_data = recv->data;

	while (len > 0) {
		if (len > RECV_DATA_BLOCK_SIZE) {
			part_len = RECV_DATA_BLOCK_SIZE;
		} else {
			part_len = len;
		}

		ut_memcpy(buf, ((byte*) recv_data) + sizeof(recv_data_t),
			  part_len);
		buf += part_len;
		len -= part_len;

		recv_data = recv_data->next;
	}
}

/** Apply the hashed log records to the page, if the page lsn is less than the
lsn of a log record.
@param just_read_in	whether the page recently arrived to the I/O handler
@param block		the page in the buffer pool */
void
recv_recover_page(bool just_read_in, buf_block_t* block)
{
	page_t*		page;
	page_zip_des_t*	page_zip;
	recv_addr_t*	recv_addr;
	recv_t*		recv;
	byte*		buf;
	lsn_t		start_lsn;
	lsn_t		end_lsn;
	lsn_t		page_lsn;
	lsn_t		page_newest_lsn;
	ibool		modification_to_page;
	mtr_t		mtr;

	mutex_enter(&(recv_sys->mutex));

	if (recv_sys->apply_log_recs == FALSE) {

		/* Log records should not be applied now */

		mutex_exit(&(recv_sys->mutex));

		return;
	}

	recv_addr = recv_get_fil_addr_struct(block->page.id.space(),
					     block->page.id.page_no());

	if ((recv_addr == NULL)
	    || (recv_addr->state == RECV_BEING_PROCESSED)
	    || (recv_addr->state == RECV_PROCESSED)) {
		ut_ad(recv_addr == NULL || recv_needed_recovery);

		mutex_exit(&(recv_sys->mutex));

		return;
	}

	ut_ad(recv_needed_recovery);

	if (UNIV_UNLIKELY(srv_print_verbose_log == 2)) {
		fprintf(stderr, "Applying log to page %u:%u\n",
			recv_addr->space, recv_addr->page_no);
	}

	DBUG_LOG("ib_log", "Applying log to page " << block->page.id);

	recv_addr->state = RECV_BEING_PROCESSED;

	mutex_exit(&(recv_sys->mutex));

	mtr_start(&mtr);
	mtr_set_log_mode(&mtr, MTR_LOG_NONE);

	page = block->frame;
	page_zip = buf_block_get_page_zip(block);

	if (just_read_in) {
		/* Move the ownership of the x-latch on the page to
		this OS thread, so that we can acquire a second
		x-latch on it.  This is needed for the operations to
		the page to pass the debug checks. */

		rw_lock_x_lock_move_ownership(&block->lock);
	}

	ibool	success = buf_page_get_known_nowait(
		RW_X_LATCH, block, BUF_KEEP_OLD,
		__FILE__, __LINE__, &mtr);
	ut_a(success);

	buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);

	/* Read the newest modification lsn from the page */
	page_lsn = mach_read_from_8(page + FIL_PAGE_LSN);

	/* It may be that the page has been modified in the buffer
	pool: read the newest modification lsn there */

	page_newest_lsn = buf_page_get_newest_modification(&block->page);

	if (page_newest_lsn) {

		page_lsn = page_newest_lsn;
	}

	modification_to_page = FALSE;
	start_lsn = end_lsn = 0;

	recv = UT_LIST_GET_FIRST(recv_addr->rec_list);
	fil_space_t* space = fil_space_acquire(block->page.id.space());

	while (recv) {
		end_lsn = recv->end_lsn;

		ut_ad(end_lsn <= log_sys.log.scanned_lsn);

		if (recv->len > RECV_DATA_BLOCK_SIZE) {
			/* We have to copy the record body to a separate
			buffer */

			buf = static_cast<byte*>(ut_malloc_nokey(recv->len));

			recv_data_copy_to_buf(buf, recv);
		} else {
			buf = ((byte*)(recv->data)) + sizeof(recv_data_t);
		}

		/* If per-table tablespace was truncated and there exist REDO
		records before truncate that are to be applied as part of
		recovery (checkpoint didn't happen since truncate was done)
		skip such records using lsn check as they may not stand valid
		post truncate.
		LSN at start of truncate is recorded and any redo record
		with LSN less than recorded LSN is skipped.
		Note: We can't skip complete recv_addr as same page may have
		valid REDO records post truncate those needs to be applied. */

		/* Ignore applying the redo logs for tablespace that is
		truncated. Post recovery there is fixup action that will
		restore the tablespace back to normal state.
		Applying redo at this stage can result in error given that
		redo will have action recorded on page before tablespace
		was re-inited and that would lead to an error while applying
		such action. */
		if (recv->start_lsn >= page_lsn
		    && !srv_is_tablespace_truncated(space->id)
		    && !(srv_was_tablespace_truncated(space)
			 && recv->start_lsn
			 < truncate_t::get_truncated_tablespace_init_lsn(
				 space->id))) {

			lsn_t	end_lsn;

			if (!modification_to_page) {

				modification_to_page = TRUE;
				start_lsn = recv->start_lsn;
			}

			if (UNIV_UNLIKELY(srv_print_verbose_log == 2)) {
				fprintf(stderr, "apply " LSN_PF ":"
					" %d len " ULINTPF " page %u:%u\n",
					recv->start_lsn, recv->type, recv->len,
					recv_addr->space, recv_addr->page_no);
			}

			DBUG_LOG("ib_log", "apply " << recv->start_lsn << ": "
				 << get_mlog_string(recv->type)
				 << " len " << recv->len
				 << " page " << block->page.id);

			recv_parse_or_apply_log_rec_body(
				recv->type, buf, buf + recv->len,
				block->page.id.space(),
				block->page.id.page_no(),
				true, block, &mtr);

			end_lsn = recv->start_lsn + recv->len;
			mach_write_to_8(FIL_PAGE_LSN + page, end_lsn);
			mach_write_to_8(srv_page_size
					- FIL_PAGE_END_LSN_OLD_CHKSUM
					+ page, end_lsn);

			if (page_zip) {
				mach_write_to_8(FIL_PAGE_LSN
						+ page_zip->data, end_lsn);
			}
		}

		if (recv->len > RECV_DATA_BLOCK_SIZE) {
			ut_free(buf);
		}

		recv = UT_LIST_GET_NEXT(rec_list, recv);
	}

	space->release();

#ifdef UNIV_ZIP_DEBUG
	if (fil_page_index_page_check(page)) {
		page_zip_des_t*	page_zip = buf_block_get_page_zip(block);

		ut_a(!page_zip
		     || page_zip_validate_low(page_zip, page, NULL, FALSE));
	}
#endif /* UNIV_ZIP_DEBUG */

	if (modification_to_page) {
		ut_a(block);

		log_flush_order_mutex_enter();
		buf_flush_recv_note_modification(block, start_lsn, end_lsn);
		log_flush_order_mutex_exit();
	}

	/* Make sure that committing mtr does not change the modification
	lsn values of page */

	mtr.discard_modifications();

	mtr_commit(&mtr);

	ib_time_t time = ut_time();

	mutex_enter(&recv_sys->mutex);

	if (recv_max_page_lsn < page_lsn) {
		recv_max_page_lsn = page_lsn;
	}

	recv_addr->state = RECV_PROCESSED;

	ut_a(recv_sys->n_addrs > 0);
	if (ulint n = --recv_sys->n_addrs) {
		if (recv_sys->report(time)) {
			ib::info() << "To recover: " << n << " pages from log";
			service_manager_extend_timeout(
				INNODB_EXTEND_TIMEOUT_INTERVAL, "To recover: " ULINTPF " pages from log", n);
		}
	}

	mutex_exit(&recv_sys->mutex);
}

/** Reads in pages which have hashed log records, from an area around a given
page number.
@param[in]	page_id	page id
@return number of pages found */
static
ulint
recv_read_in_area(
	const page_id_t&	page_id)
{
	recv_addr_t* recv_addr;
	ulint	page_nos[RECV_READ_AHEAD_AREA];
	ulint	low_limit;
	ulint	n;

	low_limit = page_id.page_no()
		- (page_id.page_no() % RECV_READ_AHEAD_AREA);

	n = 0;

	for (ulint page_no = low_limit;
	     page_no < low_limit + RECV_READ_AHEAD_AREA;
	     page_no++) {

		recv_addr = recv_get_fil_addr_struct(page_id.space(), page_no);

		const page_id_t	cur_page_id(page_id.space(), page_no);

		if (recv_addr && !buf_page_peek(cur_page_id)) {

			mutex_enter(&(recv_sys->mutex));

			if (recv_addr->state == RECV_NOT_PROCESSED) {
				recv_addr->state = RECV_BEING_READ;

				page_nos[n] = page_no;

				n++;
			}

			mutex_exit(&(recv_sys->mutex));
		}
	}

	buf_read_recv_pages(FALSE, page_id.space(), page_nos, n);
	return(n);
}

/** Apply the hash table of stored log records to persistent data pages.
@param[in]	last_batch	whether the change buffer merge will be
				performed as part of the operation */
void
recv_apply_hashed_log_recs(bool last_batch)
{
	ut_ad(srv_operation == SRV_OPERATION_NORMAL
	      || srv_operation == SRV_OPERATION_RESTORE
	      || srv_operation == SRV_OPERATION_RESTORE_EXPORT);

	mutex_enter(&recv_sys->mutex);

	while (recv_sys->apply_batch_on) {
		bool abort = recv_sys->found_corrupt_log;
		mutex_exit(&recv_sys->mutex);

		if (abort) {
			return;
		}

		os_thread_sleep(500000);
		mutex_enter(&recv_sys->mutex);
	}

	ut_ad(!last_batch == log_mutex_own());

	recv_no_ibuf_operations = !last_batch
		|| srv_operation == SRV_OPERATION_RESTORE
		|| srv_operation == SRV_OPERATION_RESTORE_EXPORT;

	ut_d(recv_no_log_write = recv_no_ibuf_operations);

	if (ulint n = recv_sys->n_addrs) {
		const char* msg = last_batch
			? "Starting final batch to recover "
			: "Starting a batch to recover ";
		ib::info() << msg << n << " pages from redo log.";
		sd_notifyf(0, "STATUS=%s" ULINTPF " pages from redo log",
			   msg, n);
	}
	recv_sys->apply_log_recs = TRUE;
	recv_sys->apply_batch_on = TRUE;

	for (ulint id = srv_undo_tablespaces_open; id--; ) {
		recv_sys_t::trunc& t = recv_sys->truncated_undo_spaces[id];
		if (t.lsn) {
			recv_addr_trim(id + srv_undo_space_id_start, t.pages,
				       t.lsn);
		}
	}

	for (ulint i = 0; i < hash_get_n_cells(recv_sys->addr_hash); i++) {
		for (recv_addr_t* recv_addr = static_cast<recv_addr_t*>(
			     HASH_GET_FIRST(recv_sys->addr_hash, i));
		     recv_addr;
		     recv_addr = static_cast<recv_addr_t*>(
				HASH_GET_NEXT(addr_hash, recv_addr))) {

			if (srv_is_tablespace_truncated(recv_addr->space)) {
				/* Avoid applying REDO log for the tablespace
				that is schedule for TRUNCATE. */
				ut_a(recv_sys->n_addrs);
				recv_addr->state = RECV_DISCARDED;
				recv_sys->n_addrs--;
				continue;
			}

			if (recv_addr->state == RECV_DISCARDED) {
				ut_a(recv_sys->n_addrs);
				recv_sys->n_addrs--;
				continue;
			}

			const page_id_t		page_id(recv_addr->space,
							recv_addr->page_no);
			bool			found;
			const page_size_t&	page_size
				= fil_space_get_page_size(recv_addr->space,
							  &found);

			ut_ad(found);

			if (recv_addr->state == RECV_NOT_PROCESSED) {
				mutex_exit(&recv_sys->mutex);

				if (buf_page_peek(page_id)) {
					mtr_t	mtr;
					mtr.start();

					buf_block_t* block = buf_page_get(
						page_id, page_size,
						RW_X_LATCH, &mtr);

					buf_block_dbg_add_level(
						block, SYNC_NO_ORDER_CHECK);

					recv_recover_page(FALSE, block);
					mtr.commit();
				} else {
					recv_read_in_area(page_id);
				}

				mutex_enter(&recv_sys->mutex);
			}
		}
	}

	/* Wait until all the pages have been processed */

	while (recv_sys->n_addrs != 0) {
		bool abort = recv_sys->found_corrupt_log;

		mutex_exit(&(recv_sys->mutex));

		if (abort) {
			return;
		}

		os_thread_sleep(500000);

		mutex_enter(&(recv_sys->mutex));
	}

	if (!last_batch) {
		/* Flush all the file pages to disk and invalidate them in
		the buffer pool */

		mutex_exit(&(recv_sys->mutex));
		log_mutex_exit();

		/* Stop the recv_writer thread from issuing any LRU
		flush batches. */
		mutex_enter(&recv_sys->writer_mutex);

		/* Wait for any currently run batch to end. */
		buf_flush_wait_LRU_batch_end();

		os_event_reset(recv_sys->flush_end);
		recv_sys->flush_type = BUF_FLUSH_LIST;
		os_event_set(recv_sys->flush_start);
		os_event_wait(recv_sys->flush_end);

		buf_pool_invalidate();

		/* Allow batches from recv_writer thread. */
		mutex_exit(&recv_sys->writer_mutex);

		log_mutex_enter();
		mutex_enter(&(recv_sys->mutex));
	}

	recv_sys->apply_log_recs = FALSE;
	recv_sys->apply_batch_on = FALSE;

	recv_sys_empty_hash();

	mutex_exit(&recv_sys->mutex);
}

/** Tries to parse a single log record.
@param[out]	type		log record type
@param[in]	ptr		pointer to a buffer
@param[in]	end_ptr		end of the buffer
@param[out]	space_id	tablespace identifier
@param[out]	page_no		page number
@param[in]	apply		whether to apply MLOG_FILE_* records
@param[out]	body		start of log record body
@return length of the record, or 0 if the record was not complete */
static
ulint
recv_parse_log_rec(
	mlog_id_t*	type,
	byte*		ptr,
	byte*		end_ptr,
	ulint*		space,
	ulint*		page_no,
	bool		apply,
	byte**		body)
{
	byte*	new_ptr;

	*body = NULL;

	UNIV_MEM_INVALID(type, sizeof *type);
	UNIV_MEM_INVALID(space, sizeof *space);
	UNIV_MEM_INVALID(page_no, sizeof *page_no);
	UNIV_MEM_INVALID(body, sizeof *body);

	if (ptr == end_ptr) {

		return(0);
	}

	switch (*ptr) {
#ifdef UNIV_LOG_LSN_DEBUG
	case MLOG_LSN | MLOG_SINGLE_REC_FLAG:
	case MLOG_LSN:
		new_ptr = mlog_parse_initial_log_record(
			ptr, end_ptr, type, space, page_no);
		if (new_ptr != NULL) {
			const lsn_t	lsn = static_cast<lsn_t>(
				*space) << 32 | *page_no;
			ut_a(lsn == recv_sys->recovered_lsn);
		}

		*type = MLOG_LSN;
		return(new_ptr - ptr);
#endif /* UNIV_LOG_LSN_DEBUG */
	case MLOG_MULTI_REC_END:
	case MLOG_DUMMY_RECORD:
		*type = static_cast<mlog_id_t>(*ptr);
		return(1);
	case MLOG_CHECKPOINT:
		if (end_ptr < ptr + SIZE_OF_MLOG_CHECKPOINT) {
			return(0);
		}
		*type = static_cast<mlog_id_t>(*ptr);
		return(SIZE_OF_MLOG_CHECKPOINT);
	case MLOG_MULTI_REC_END | MLOG_SINGLE_REC_FLAG:
	case MLOG_DUMMY_RECORD | MLOG_SINGLE_REC_FLAG:
	case MLOG_CHECKPOINT | MLOG_SINGLE_REC_FLAG:
		ib::error() << "Incorrect log record type "
			<< ib::hex(unsigned(*ptr));
		recv_sys->found_corrupt_log = true;
		return(0);
	}

	new_ptr = mlog_parse_initial_log_record(ptr, end_ptr, type, space,
						page_no);
	*body = new_ptr;

	if (UNIV_UNLIKELY(!new_ptr)) {

		return(0);
	}

	const byte*	old_ptr = new_ptr;
	new_ptr = recv_parse_or_apply_log_rec_body(
		*type, new_ptr, end_ptr, *space, *page_no, apply, NULL, NULL);

	if (UNIV_UNLIKELY(new_ptr == NULL)) {
		return(0);
	}

	if (*page_no == 0 && *type == MLOG_4BYTES
	    && mach_read_from_2(old_ptr) == FSP_HEADER_OFFSET + FSP_SIZE) {
		old_ptr += 2;
		fil_space_set_recv_size(*space,
					mach_parse_compressed(&old_ptr,
							      end_ptr));
	}

	return ulint(new_ptr - ptr);
}

/*******************************************************//**
Calculates the new value for lsn when more data is added to the log. */
static
lsn_t
recv_calc_lsn_on_data_add(
/*======================*/
	lsn_t		lsn,	/*!< in: old lsn */
	ib_uint64_t	len)	/*!< in: this many bytes of data is
				added, log block headers not included */
{
	ulint		frag_len;
	ib_uint64_t	lsn_len;

	frag_len = (lsn % OS_FILE_LOG_BLOCK_SIZE) - LOG_BLOCK_HDR_SIZE;
	ut_ad(frag_len < OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE
	      - LOG_BLOCK_TRL_SIZE);
	lsn_len = len;
	lsn_len += (lsn_len + frag_len)
		/ (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE
		   - LOG_BLOCK_TRL_SIZE)
		* (LOG_BLOCK_HDR_SIZE + LOG_BLOCK_TRL_SIZE);

	return(lsn + lsn_len);
}

/** Prints diagnostic info of corrupt log.
@param[in]	ptr	pointer to corrupt log record
@param[in]	type	type of the log record (could be garbage)
@param[in]	space	tablespace ID (could be garbage)
@param[in]	page_no	page number (could be garbage)
@return whether processing should continue */
static
bool
recv_report_corrupt_log(
	const byte*	ptr,
	int		type,
	ulint		space,
	ulint		page_no)
{
	ib::error() <<
		"############### CORRUPT LOG RECORD FOUND ##################";

	const ulint ptr_offset = ulint(ptr - recv_sys->buf);

	ib::info() << "Log record type " << type << ", page " << space << ":"
		<< page_no << ". Log parsing proceeded successfully up to "
		<< recv_sys->recovered_lsn << ". Previous log record type "
		<< recv_previous_parsed_rec_type << ", is multi "
		<< recv_previous_parsed_rec_is_multi << " Recv offset "
		<< ptr_offset << ", prev "
		<< recv_previous_parsed_rec_offset;

	ut_ad(ptr <= recv_sys->buf + recv_sys->len);

	const ulint	limit	= 100;
	const ulint	prev_offset = std::min(recv_previous_parsed_rec_offset,
					       ptr_offset);
	const ulint	before = std::min(prev_offset, limit);
	const ulint	after = std::min(recv_sys->len - ptr_offset, limit);

	ib::info() << "Hex dump starting " << before << " bytes before and"
		" ending " << after << " bytes after the corrupted record:";

	const byte* start = recv_sys->buf + prev_offset - before;

	ut_print_buf(stderr, start, ulint(ptr - start) + after);
	putc('\n', stderr);

	if (!srv_force_recovery) {
		ib::info() << "Set innodb_force_recovery to ignore this error.";
		return(false);
	}

	ib::warn() << "The log file may have been corrupt and it is possible"
		" that the log scan did not proceed far enough in recovery!"
		" Please run CHECK TABLE on your InnoDB tables to check"
		" that they are ok! If mysqld crashes after this recovery; "
		<< FORCE_RECOVERY_MSG;
	return(true);
}

/** Parse log records from a buffer and optionally store them to a
hash table to wait merging to file pages.
@param[in]	checkpoint_lsn	the LSN of the latest checkpoint
@param[in]	store		whether to store page operations
@param[in]	apply		whether to apply the records
@return whether MLOG_CHECKPOINT record was seen the first time,
or corruption was noticed */
bool recv_parse_log_recs(lsn_t checkpoint_lsn, store_t store, bool apply)
{
	byte*		ptr;
	byte*		end_ptr;
	bool		single_rec;
	ulint		len;
	lsn_t		new_recovered_lsn;
	lsn_t		old_lsn;
	mlog_id_t	type;
	ulint		space;
	ulint		page_no;
	byte*		body;

	ut_ad(log_mutex_own());
	ut_ad(recv_sys->parse_start_lsn != 0);
loop:
	ptr = recv_sys->buf + recv_sys->recovered_offset;

	end_ptr = recv_sys->buf + recv_sys->len;

	if (ptr == end_ptr) {

		return(false);
	}

	switch (*ptr) {
	case MLOG_CHECKPOINT:
#ifdef UNIV_LOG_LSN_DEBUG
	case MLOG_LSN:
#endif /* UNIV_LOG_LSN_DEBUG */
	case MLOG_DUMMY_RECORD:
		single_rec = true;
		break;
	default:
		single_rec = !!(*ptr & MLOG_SINGLE_REC_FLAG);
	}

	if (single_rec) {
		/* The mtr did not modify multiple pages */

		old_lsn = recv_sys->recovered_lsn;

		/* Try to parse a log record, fetching its type, space id,
		page no, and a pointer to the body of the log record */

		len = recv_parse_log_rec(&type, ptr, end_ptr, &space,
					 &page_no, apply, &body);

		if (recv_sys->found_corrupt_log) {
			recv_report_corrupt_log(ptr, type, space, page_no);
			return(true);
		}

		if (recv_sys->found_corrupt_fs) {
			return(true);
		}

		if (len == 0) {
			return(false);
		}

		new_recovered_lsn = recv_calc_lsn_on_data_add(old_lsn, len);

		if (new_recovered_lsn > recv_sys->scanned_lsn) {
			/* The log record filled a log block, and we require
			that also the next log block should have been scanned
			in */

			return(false);
		}

		recv_previous_parsed_rec_type = type;
		recv_previous_parsed_rec_offset = recv_sys->recovered_offset;
		recv_previous_parsed_rec_is_multi = 0;

		recv_sys->recovered_offset += len;
		recv_sys->recovered_lsn = new_recovered_lsn;

		switch (type) {
			lsn_t	lsn;
		case MLOG_DUMMY_RECORD:
			/* Do nothing */
			break;
		case MLOG_CHECKPOINT:
			compile_time_assert(SIZE_OF_MLOG_CHECKPOINT == 1 + 8);
			lsn = mach_read_from_8(ptr + 1);

			if (UNIV_UNLIKELY(srv_print_verbose_log == 2)) {
				fprintf(stderr,
					"MLOG_CHECKPOINT(" LSN_PF ") %s at "
					LSN_PF "\n", lsn,
					lsn != checkpoint_lsn ? "ignored"
					: recv_sys->mlog_checkpoint_lsn
					? "reread" : "read",
					recv_sys->recovered_lsn);
			}

			DBUG_PRINT("ib_log",
				   ("MLOG_CHECKPOINT(" LSN_PF ") %s at "
				    LSN_PF,
				    lsn,
				    lsn != checkpoint_lsn ? "ignored"
				    : recv_sys->mlog_checkpoint_lsn
				    ? "reread" : "read",
				    recv_sys->recovered_lsn));

			if (lsn == checkpoint_lsn) {
				if (recv_sys->mlog_checkpoint_lsn) {
					/* At recv_reset_logs() we may
					write a duplicate MLOG_CHECKPOINT
					for the same checkpoint LSN. Thus
					recv_sys->mlog_checkpoint_lsn
					can differ from the current LSN. */
					ut_ad(recv_sys->mlog_checkpoint_lsn
					      <= recv_sys->recovered_lsn);
					break;
				}
				recv_sys->mlog_checkpoint_lsn
					= recv_sys->recovered_lsn;
				return(true);
			}
			break;
#ifdef UNIV_LOG_LSN_DEBUG
		case MLOG_LSN:
			/* Do not add these records to the hash table.
			The page number and space id fields are misused
			for something else. */
			break;
#endif /* UNIV_LOG_LSN_DEBUG */
		default:
			switch (store) {
			case STORE_NO:
				break;
			case STORE_IF_EXISTS:
				if (fil_space_get_flags(space)
				    == ULINT_UNDEFINED) {
					break;
				}
				/* fall through */
			case STORE_YES:
				recv_add_to_hash_table(
					type, space, page_no, body,
					ptr + len, old_lsn,
					recv_sys->recovered_lsn);
			}
			/* fall through */
		case MLOG_INDEX_LOAD:
			if (type == MLOG_INDEX_LOAD) {
				if (log_optimized_ddl_op) {
					log_optimized_ddl_op(space);
				}
			}
			/* fall through */
		case MLOG_FILE_NAME:
		case MLOG_FILE_DELETE:
		case MLOG_FILE_CREATE2:
		case MLOG_FILE_RENAME2:
		case MLOG_TRUNCATE:
			/* These were already handled by
			recv_parse_log_rec() and
			recv_parse_or_apply_log_rec_body(). */
			DBUG_PRINT("ib_log",
				("scan " LSN_PF ": log rec %s"
				" len " ULINTPF
				" page " ULINTPF ":" ULINTPF,
				old_lsn, get_mlog_string(type),
				len, space, page_no));
		}
	} else {
		/* Check that all the records associated with the single mtr
		are included within the buffer */

		ulint	total_len	= 0;
		ulint	n_recs		= 0;
		bool	only_mlog_file	= true;
		ulint	mlog_rec_len	= 0;

		for (;;) {
			len = recv_parse_log_rec(
				&type, ptr, end_ptr, &space, &page_no,
				false, &body);

			if (recv_sys->found_corrupt_log
			    || type == MLOG_CHECKPOINT
			    || (ptr != end_ptr
				&& (*ptr & MLOG_SINGLE_REC_FLAG))) {
				recv_sys->found_corrupt_log = true;
				recv_report_corrupt_log(
					ptr, type, space, page_no);
				return(true);
			}

			if (recv_sys->found_corrupt_fs) {
				return(true);
			}

			if (len == 0) {
				return(false);
			}

			recv_previous_parsed_rec_type = type;
			recv_previous_parsed_rec_offset
				= recv_sys->recovered_offset + total_len;
			recv_previous_parsed_rec_is_multi = 1;

			/* MLOG_FILE_NAME redo log records doesn't make changes
			to persistent data. If only MLOG_FILE_NAME redo
			log record exists then reset the parsing buffer pointer
			by changing recovered_lsn and recovered_offset. */
			if (type != MLOG_FILE_NAME && only_mlog_file == true) {
				only_mlog_file = false;
			}

			if (only_mlog_file) {
				new_recovered_lsn = recv_calc_lsn_on_data_add(
					recv_sys->recovered_lsn, len);
				mlog_rec_len += len;
				recv_sys->recovered_offset += len;
				recv_sys->recovered_lsn = new_recovered_lsn;
			}

			total_len += len;
			n_recs++;

			ptr += len;

			if (type == MLOG_MULTI_REC_END) {
				DBUG_PRINT("ib_log",
					   ("scan " LSN_PF
					    ": multi-log end"
					    " total_len " ULINTPF
					    " n=" ULINTPF,
					    recv_sys->recovered_lsn,
					    total_len, n_recs));
				total_len -= mlog_rec_len;
				break;
			}

			DBUG_PRINT("ib_log",
				   ("scan " LSN_PF ": multi-log rec %s"
				    " len " ULINTPF
				    " page " ULINTPF ":" ULINTPF,
				    recv_sys->recovered_lsn,
				    get_mlog_string(type), len, space, page_no));
		}

		new_recovered_lsn = recv_calc_lsn_on_data_add(
			recv_sys->recovered_lsn, total_len);

		if (new_recovered_lsn > recv_sys->scanned_lsn) {
			/* The log record filled a log block, and we require
			that also the next log block should have been scanned
			in */

			return(false);
		}

		/* Add all the records to the hash table */

		ptr = recv_sys->buf + recv_sys->recovered_offset;

		for (;;) {
			old_lsn = recv_sys->recovered_lsn;
			/* This will apply MLOG_FILE_ records. We
			had to skip them in the first scan, because we
			did not know if the mini-transaction was
			completely recovered (until MLOG_MULTI_REC_END). */
			len = recv_parse_log_rec(
				&type, ptr, end_ptr, &space, &page_no,
				apply, &body);

			if (recv_sys->found_corrupt_log
			    && !recv_report_corrupt_log(
				    ptr, type, space, page_no)) {
				return(true);
			}

			if (recv_sys->found_corrupt_fs) {
				return(true);
			}

			ut_a(len != 0);
			ut_a(!(*ptr & MLOG_SINGLE_REC_FLAG));

			recv_sys->recovered_offset += len;
			recv_sys->recovered_lsn
				= recv_calc_lsn_on_data_add(old_lsn, len);

			switch (type) {
			case MLOG_MULTI_REC_END:
				/* Found the end mark for the records */
				goto loop;
#ifdef UNIV_LOG_LSN_DEBUG
			case MLOG_LSN:
				/* Do not add these records to the hash table.
				The page number and space id fields are misused
				for something else. */
				break;
#endif /* UNIV_LOG_LSN_DEBUG */
			case MLOG_INDEX_LOAD:
				/* Mariabackup FIXME: Report an error
				when encountering MLOG_INDEX_LOAD on
				--prepare or already on --backup. */
				ut_a(srv_operation == SRV_OPERATION_NORMAL);
				break;
			case MLOG_FILE_NAME:
			case MLOG_FILE_DELETE:
			case MLOG_FILE_CREATE2:
			case MLOG_FILE_RENAME2:
			case MLOG_TRUNCATE:
				/* These were already handled by
				recv_parse_log_rec() and
				recv_parse_or_apply_log_rec_body(). */
				break;
			default:
				switch (store) {
				case STORE_NO:
					break;
				case STORE_IF_EXISTS:
					if (fil_space_get_flags(space)
					    == ULINT_UNDEFINED) {
						break;
					}
					/* fall through */
				case STORE_YES:
					recv_add_to_hash_table(
						type, space, page_no,
						body, ptr + len,
						old_lsn,
						new_recovered_lsn);
				}
			}

			ptr += len;
		}
	}

	goto loop;
}

/** Adds data from a new log block to the parsing buffer of recv_sys if
recv_sys->parse_start_lsn is non-zero.
@param[in]	log_block	log block to add
@param[in]	scanned_lsn	lsn of how far we were able to find
				data in this log block
@return true if more data added */
bool recv_sys_add_to_parsing_buf(const byte* log_block, lsn_t scanned_lsn)
{
	ulint	more_len;
	ulint	data_len;
	ulint	start_offset;
	ulint	end_offset;

	ut_ad(scanned_lsn >= recv_sys->scanned_lsn);

	if (!recv_sys->parse_start_lsn) {
		/* Cannot start parsing yet because no start point for
		it found */

		return(false);
	}

	data_len = log_block_get_data_len(log_block);

	if (recv_sys->parse_start_lsn >= scanned_lsn) {

		return(false);

	} else if (recv_sys->scanned_lsn >= scanned_lsn) {

		return(false);

	} else if (recv_sys->parse_start_lsn > recv_sys->scanned_lsn) {
		more_len = (ulint) (scanned_lsn - recv_sys->parse_start_lsn);
	} else {
		more_len = (ulint) (scanned_lsn - recv_sys->scanned_lsn);
	}

	if (more_len == 0) {

		return(false);
	}

	ut_ad(data_len >= more_len);

	start_offset = data_len - more_len;

	if (start_offset < LOG_BLOCK_HDR_SIZE) {
		start_offset = LOG_BLOCK_HDR_SIZE;
	}

	end_offset = data_len;

	if (end_offset > OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) {
		end_offset = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE;
	}

	ut_ad(start_offset <= end_offset);

	if (start_offset < end_offset) {
		ut_memcpy(recv_sys->buf + recv_sys->len,
			  log_block + start_offset, end_offset - start_offset);

		recv_sys->len += end_offset - start_offset;

		ut_a(recv_sys->len <= RECV_PARSING_BUF_SIZE);
	}

	return(true);
}

/** Moves the parsing buffer data left to the buffer start. */
void recv_sys_justify_left_parsing_buf()
{
	ut_memmove(recv_sys->buf, recv_sys->buf + recv_sys->recovered_offset,
		   recv_sys->len - recv_sys->recovered_offset);

	recv_sys->len -= recv_sys->recovered_offset;

	recv_sys->recovered_offset = 0;
}

/** Scan redo log from a buffer and stores new log data to the parsing buffer.
Parse and hash the log records if new data found.
Apply log records automatically when the hash table becomes full.
@return true if not able to scan any more in this log group */
static
bool
recv_scan_log_recs(
/*===============*/
	ulint		available_memory,/*!< in: we let the hash table of recs
					to grow to this size, at the maximum */
	store_t*	store_to_hash,	/*!< in,out: whether the records should be
					stored to the hash table; this is reset
					if just debug checking is needed, or
					when the available_memory runs out */
	const byte*	log_block,	/*!< in: log segment */
	lsn_t		checkpoint_lsn,	/*!< in: latest checkpoint LSN */
	lsn_t		start_lsn,	/*!< in: buffer start LSN */
	lsn_t		end_lsn,	/*!< in: buffer end LSN */
	lsn_t*		contiguous_lsn,	/*!< in/out: it is known that all log
					groups contain contiguous log data up
					to this lsn */
	lsn_t*		group_scanned_lsn)/*!< out: scanning succeeded up to
					this lsn */
{
	lsn_t		scanned_lsn	= start_lsn;
	bool		finished	= false;
	ulint		data_len;
	bool		more_data	= false;
	bool		apply		= recv_sys->mlog_checkpoint_lsn != 0;
	ulint		recv_parsing_buf_size = RECV_PARSING_BUF_SIZE;

	ut_ad(start_lsn % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad(end_lsn % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad(end_lsn >= start_lsn + OS_FILE_LOG_BLOCK_SIZE);

	const byte* const	log_end = log_block
		+ ulint(end_lsn - start_lsn);

	do {
		ut_ad(!finished);

		if (log_block_get_flush_bit(log_block)) {
			/* This block was a start of a log flush operation:
			we know that the previous flush operation must have
			been completed for all log groups before this block
			can have been flushed to any of the groups. Therefore,
			we know that log data is contiguous up to scanned_lsn
			in all non-corrupt log groups. */

			if (scanned_lsn > *contiguous_lsn) {
				*contiguous_lsn = scanned_lsn;
			}
		}

		data_len = log_block_get_data_len(log_block);

		if (scanned_lsn + data_len > recv_sys->scanned_lsn
		    && log_block_get_checkpoint_no(log_block)
		    < recv_sys->scanned_checkpoint_no
		    && (recv_sys->scanned_checkpoint_no
			- log_block_get_checkpoint_no(log_block)
			> 0x80000000UL)) {

			/* Garbage from a log buffer flush which was made
			before the most recent database recovery */
			finished = true;
			break;
		}

		if (!recv_sys->parse_start_lsn
		    && (log_block_get_first_rec_group(log_block) > 0)) {

			/* We found a point from which to start the parsing
			of log records */

			recv_sys->parse_start_lsn = scanned_lsn
				+ log_block_get_first_rec_group(log_block);
			recv_sys->scanned_lsn = recv_sys->parse_start_lsn;
			recv_sys->recovered_lsn = recv_sys->parse_start_lsn;
		}

		scanned_lsn += data_len;

		if (data_len == LOG_BLOCK_HDR_SIZE + SIZE_OF_MLOG_CHECKPOINT
		    && scanned_lsn == checkpoint_lsn + SIZE_OF_MLOG_CHECKPOINT
		    && log_block[LOG_BLOCK_HDR_SIZE] == MLOG_CHECKPOINT
		    && checkpoint_lsn == mach_read_from_8(LOG_BLOCK_HDR_SIZE
							  + 1 + log_block)) {
			/* The redo log is logically empty. */
			ut_ad(recv_sys->mlog_checkpoint_lsn == 0
			      || recv_sys->mlog_checkpoint_lsn
			      == checkpoint_lsn);
			recv_sys->mlog_checkpoint_lsn = checkpoint_lsn;
			DBUG_PRINT("ib_log", ("found empty log; LSN=" LSN_PF,
					      scanned_lsn));
			finished = true;
			break;
		}

		if (scanned_lsn > recv_sys->scanned_lsn) {
			ut_ad(!srv_log_files_created);
			if (!recv_needed_recovery) {
				recv_needed_recovery = true;

				if (srv_read_only_mode) {
					ib::warn() << "innodb_read_only"
						" prevents crash recovery";
					return(true);
				}

				ib::info() << "Starting crash recovery from"
					" checkpoint LSN="
					<< recv_sys->scanned_lsn;
			}

			/* We were able to find more log data: add it to the
			parsing buffer if parse_start_lsn is already
			non-zero */

			DBUG_EXECUTE_IF(
				"reduce_recv_parsing_buf",
				recv_parsing_buf_size
					= (70 * 1024);
				);

			if (recv_sys->len + 4 * OS_FILE_LOG_BLOCK_SIZE
			    >= recv_parsing_buf_size) {
				ib::error() << "Log parsing buffer overflow."
					" Recovery may have failed!";

				recv_sys->found_corrupt_log = true;

				if (!srv_force_recovery) {
					ib::error()
						<< "Set innodb_force_recovery"
						" to ignore this error.";
					return(true);
				}
			} else if (!recv_sys->found_corrupt_log) {
				more_data = recv_sys_add_to_parsing_buf(
					log_block, scanned_lsn);
			}

			recv_sys->scanned_lsn = scanned_lsn;
			recv_sys->scanned_checkpoint_no
				= log_block_get_checkpoint_no(log_block);
		}

		if (data_len < OS_FILE_LOG_BLOCK_SIZE) {
			/* Log data for this group ends here */
			finished = true;
			break;
		} else {
			log_block += OS_FILE_LOG_BLOCK_SIZE;
		}
	} while (log_block < log_end);

	*group_scanned_lsn = scanned_lsn;

	if (more_data && !recv_sys->found_corrupt_log) {
		/* Try to parse more log records */

		if (recv_parse_log_recs(checkpoint_lsn,
					*store_to_hash, apply)) {
			ut_ad(recv_sys->found_corrupt_log
			      || recv_sys->found_corrupt_fs
			      || recv_sys->mlog_checkpoint_lsn
			      == recv_sys->recovered_lsn);
			return(true);
		}

		if (*store_to_hash != STORE_NO
		    && mem_heap_get_size(recv_sys->heap) > available_memory) {

			DBUG_PRINT("ib_log", ("Ran out of memory and last "
					      "stored lsn " LSN_PF,
					      recv_sys->recovered_lsn));

			recv_sys->last_stored_lsn = recv_sys->recovered_lsn;
			*store_to_hash = STORE_NO;
		}

		if (recv_sys->recovered_offset > recv_parsing_buf_size / 4) {
			/* Move parsing buffer data to the buffer start */

			recv_sys_justify_left_parsing_buf();
		}
	}

	return(finished);
}

/** Scans log from a buffer and stores new log data to the parsing buffer.
Parses and hashes the log records if new data found.
@param[in]	checkpoint_lsn		latest checkpoint log sequence number
@param[in,out]	contiguous_lsn		log sequence number
until which all redo log has been scanned
@param[in]	last_phase		whether changes
can be applied to the tablespaces
@return whether rescan is needed (not everything was stored) */
static
bool
recv_group_scan_log_recs(
	lsn_t		checkpoint_lsn,
	lsn_t*		contiguous_lsn,
	bool		last_phase)
{
	DBUG_ENTER("recv_group_scan_log_recs");
	DBUG_ASSERT(!last_phase || recv_sys->mlog_checkpoint_lsn > 0);

	mutex_enter(&recv_sys->mutex);
	recv_sys->len = 0;
	recv_sys->recovered_offset = 0;
	recv_sys->n_addrs = 0;
	recv_sys_empty_hash();
	srv_start_lsn = *contiguous_lsn;
	recv_sys->parse_start_lsn = *contiguous_lsn;
	recv_sys->scanned_lsn = *contiguous_lsn;
	recv_sys->recovered_lsn = *contiguous_lsn;
	recv_sys->scanned_checkpoint_no = 0;
	recv_previous_parsed_rec_type = MLOG_SINGLE_REC_FLAG;
	recv_previous_parsed_rec_offset	= 0;
	recv_previous_parsed_rec_is_multi = 0;
	ut_ad(recv_max_page_lsn == 0);
	ut_ad(last_phase || !recv_writer_thread_active);
	mutex_exit(&recv_sys->mutex);

	lsn_t	start_lsn;
	lsn_t	end_lsn;
	store_t	store_to_hash	= recv_sys->mlog_checkpoint_lsn == 0
		? STORE_NO : (last_phase ? STORE_IF_EXISTS : STORE_YES);
	ulint	available_mem	= srv_page_size
		* (buf_pool_get_n_pages()
		   - (recv_n_pool_free_frames * srv_buf_pool_instances));

	log_sys.log.scanned_lsn = end_lsn = *contiguous_lsn =
		ut_uint64_align_down(*contiguous_lsn, OS_FILE_LOG_BLOCK_SIZE);

	do {
		if (last_phase && store_to_hash == STORE_NO) {
			store_to_hash = STORE_IF_EXISTS;
			/* We must not allow change buffer
			merge here, because it would generate
			redo log records before we have
			finished the redo log scan. */
			recv_apply_hashed_log_recs(false);
		}

		start_lsn = ut_uint64_align_down(end_lsn,
						 OS_FILE_LOG_BLOCK_SIZE);
		end_lsn = start_lsn;
		log_sys.log.read_log_seg(&end_lsn, start_lsn + RECV_SCAN_SIZE);
	} while (end_lsn != start_lsn
		 && !recv_scan_log_recs(
			 available_mem, &store_to_hash, log_sys.buf,
			 checkpoint_lsn,
			 start_lsn, end_lsn,
			 contiguous_lsn, &log_sys.log.scanned_lsn));

	if (recv_sys->found_corrupt_log || recv_sys->found_corrupt_fs) {
		DBUG_RETURN(false);
	}

	DBUG_PRINT("ib_log", ("%s " LSN_PF " completed",
			      last_phase ? "rescan" : "scan",
			      log_sys.log.scanned_lsn));

	DBUG_RETURN(store_to_hash == STORE_NO);
}

/** Report a missing tablespace for which page-redo log exists.
@param[in]	err	previous error code
@param[in]	i	tablespace descriptor
@return new error code */
static
dberr_t
recv_init_missing_space(dberr_t err, const recv_spaces_t::const_iterator& i)
{
	if (srv_operation == SRV_OPERATION_RESTORE
	    || srv_operation == SRV_OPERATION_RESTORE_EXPORT) {
		ib::warn() << "Tablespace " << i->first << " was not"
			" found at " << i->second.name << " when"
			" restoring a (partial?) backup. All redo log"
			" for this file will be ignored!";
		return(err);
	}

	if (srv_force_recovery == 0) {
		ib::error() << "Tablespace " << i->first << " was not"
			" found at " << i->second.name << ".";

		if (err == DB_SUCCESS) {
			ib::error() << "Set innodb_force_recovery=1 to"
				" ignore this and to permanently lose"
				" all changes to the tablespace.";
			err = DB_TABLESPACE_NOT_FOUND;
		}
	} else {
		ib::warn() << "Tablespace " << i->first << " was not"
			" found at " << i->second.name << ", and"
			" innodb_force_recovery was set. All redo log"
			" for this tablespace will be ignored!";
	}

	return(err);
}

/** Report the missing tablespace and discard the redo logs for the deleted
tablespace.
@param[in]	rescan			rescan of redo logs is needed
					if hash table ran out of memory
@param[out]	missing_tablespace	missing tablespace exists or not
@return error code or DB_SUCCESS. */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
recv_validate_tablespace(bool rescan, bool& missing_tablespace)
{
	dberr_t err = DB_SUCCESS;

	for (ulint h = 0; h < hash_get_n_cells(recv_sys->addr_hash); h++) {
		for (recv_addr_t* recv_addr = static_cast<recv_addr_t*>(
			     HASH_GET_FIRST(recv_sys->addr_hash, h));
		     recv_addr != 0;
		     recv_addr = static_cast<recv_addr_t*>(
			     HASH_GET_NEXT(addr_hash, recv_addr))) {

			const ulint space = recv_addr->space;

			if (is_predefined_tablespace(space)) {
				continue;
			}

			recv_spaces_t::iterator i = recv_spaces.find(space);
			ut_ad(i != recv_spaces.end());

			switch (i->second.status) {
			case file_name_t::MISSING:
				err = recv_init_missing_space(err, i);
				i->second.status = file_name_t::DELETED;
				/* fall through */
			case file_name_t::DELETED:
				recv_addr->state = RECV_DISCARDED;
				/* fall through */
			case file_name_t::NORMAL:
				continue;
			}
			ut_ad(0);
		}
	}

	if (err != DB_SUCCESS) {
		return(err);
	}

	/* When rescan is not needed then recv_sys->addr_hash will have
	all space id belongs to redo log. If rescan is needed and
	innodb_force_recovery > 0 then InnoDB can ignore missing tablespace. */
	for (recv_spaces_t::iterator i = recv_spaces.begin();
	     i != recv_spaces.end(); i++) {

		if (i->second.status != file_name_t::MISSING) {
			continue;
		}

		missing_tablespace = true;

		if (srv_force_recovery > 0) {
			ib::warn() << "Tablespace " << i->first
				<<" was not found at " << i->second.name
				<<", and innodb_force_recovery was set."
				<<" All redo log for this tablespace"
				<<" will be ignored!";
			continue;
		}

		if (!rescan) {
			ib::info() << "Tablespace " << i->first
				<< " was not found at '"
				<< i->second.name << "', but there"
				<<" were no modifications either.";
		}
	}

	if (!rescan || srv_force_recovery > 0) {
		missing_tablespace = false;
	}

	return DB_SUCCESS;
}

/** Check if all tablespaces were found for crash recovery.
@param[in]	rescan			rescan of redo logs is needed
@param[out]	missing_tablespace	missing table exists
@return error code or DB_SUCCESS */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
recv_init_crash_recovery_spaces(bool rescan, bool& missing_tablespace)
{
	bool		flag_deleted	= false;

	ut_ad(!srv_read_only_mode);
	ut_ad(recv_needed_recovery);

	for (recv_spaces_t::iterator i = recv_spaces.begin();
	     i != recv_spaces.end(); i++) {
		ut_ad(!is_predefined_tablespace(i->first));
		ut_ad(i->second.status != file_name_t::DELETED || !i->second.space);

		if (i->second.status == file_name_t::DELETED) {
			/* The tablespace was deleted,
			so we can ignore any redo log for it. */
			flag_deleted = true;
		} else if (i->second.space != NULL) {
			/* The tablespace was found, and there
			are some redo log records for it. */
			fil_names_dirty(i->second.space);
		} else if (i->second.name == "") {
			ib::error() << "Missing MLOG_FILE_NAME"
				" or MLOG_FILE_DELETE"
				" before MLOG_CHECKPOINT for tablespace "
				<< i->first;
			recv_sys->found_corrupt_log = true;
			return(DB_CORRUPTION);
		} else {
			i->second.status = file_name_t::MISSING;
			flag_deleted = true;
		}

		ut_ad(i->second.status == file_name_t::DELETED || i->second.name != "");
	}

	if (flag_deleted) {
		return recv_validate_tablespace(rescan, missing_tablespace);
	}

	return DB_SUCCESS;
}

/** Start recovering from a redo log checkpoint.
@see recv_recovery_from_checkpoint_finish
@param[in]	flush_lsn	FIL_PAGE_FILE_FLUSH_LSN
of first system tablespace page
@return error code or DB_SUCCESS */
dberr_t
recv_recovery_from_checkpoint_start(lsn_t flush_lsn)
{
	ulint		max_cp_field;
	lsn_t		checkpoint_lsn;
	bool		rescan;
	ib_uint64_t	checkpoint_no;
	lsn_t		contiguous_lsn;
	byte*		buf;
	dberr_t		err = DB_SUCCESS;

	ut_ad(srv_operation == SRV_OPERATION_NORMAL
	      || srv_operation == SRV_OPERATION_RESTORE
	      || srv_operation == SRV_OPERATION_RESTORE_EXPORT);

	/* Initialize red-black tree for fast insertions into the
	flush_list during recovery process. */
	buf_flush_init_flush_rbt();

	if (srv_force_recovery >= SRV_FORCE_NO_LOG_REDO) {

		ib::info() << "innodb_force_recovery=6 skips redo log apply";

		return(DB_SUCCESS);
	}

	recv_recovery_on = true;

	log_mutex_enter();

	err = recv_find_max_checkpoint(&max_cp_field);

	if (err != DB_SUCCESS) {

		srv_start_lsn = recv_sys->recovered_lsn = log_sys.lsn;
		log_mutex_exit();
		return(err);
	}

	log_header_read(max_cp_field);

	buf = log_sys.checkpoint_buf;

	checkpoint_lsn = mach_read_from_8(buf + LOG_CHECKPOINT_LSN);
	checkpoint_no = mach_read_from_8(buf + LOG_CHECKPOINT_NO);

	/* Start reading the log from the checkpoint lsn. The variable
	contiguous_lsn contains an lsn up to which the log is known to
	be contiguously written. */
	recv_sys->mlog_checkpoint_lsn = 0;

	ut_ad(RECV_SCAN_SIZE <= srv_log_buffer_size);

	const lsn_t	end_lsn = mach_read_from_8(
		buf + LOG_CHECKPOINT_END_LSN);

	ut_ad(recv_sys->n_addrs == 0);
	contiguous_lsn = checkpoint_lsn;
	switch (log_sys.log.format) {
	case 0:
		log_mutex_exit();
		return recv_log_format_0_recover(checkpoint_lsn,
						 buf[20 + 32 * 9] == 2);
	default:
		if (end_lsn == 0) {
			break;
		}
		if (end_lsn >= checkpoint_lsn) {
			contiguous_lsn = end_lsn;
			break;
		}
		recv_sys->found_corrupt_log = true;
		log_mutex_exit();
		return(DB_ERROR);
	}

	/* Look for MLOG_CHECKPOINT. */
	recv_group_scan_log_recs(checkpoint_lsn, &contiguous_lsn, false);
	/* The first scan should not have stored or applied any records. */
	ut_ad(recv_sys->n_addrs == 0);
	ut_ad(!recv_sys->found_corrupt_fs);

	if (srv_read_only_mode && recv_needed_recovery) {
		log_mutex_exit();
		return(DB_READ_ONLY);
	}

	if (recv_sys->found_corrupt_log && !srv_force_recovery) {
		log_mutex_exit();
		ib::warn() << "Log scan aborted at LSN " << contiguous_lsn;
		return(DB_ERROR);
	}

	if (recv_sys->mlog_checkpoint_lsn == 0) {
		lsn_t scan_lsn = log_sys.log.scanned_lsn;
		if (!srv_read_only_mode && scan_lsn != checkpoint_lsn) {
			log_mutex_exit();
			ib::error err;
			err << "Missing MLOG_CHECKPOINT";
			if (end_lsn) {
				err << " at " << end_lsn;
			}
			err << " between the checkpoint " << checkpoint_lsn
			    << " and the end " << scan_lsn << ".";
			return(DB_ERROR);
		}

		log_sys.log.scanned_lsn = checkpoint_lsn;
		rescan = false;
	} else {
		contiguous_lsn = checkpoint_lsn;
		rescan = recv_group_scan_log_recs(
			checkpoint_lsn, &contiguous_lsn, false);

		if ((recv_sys->found_corrupt_log && !srv_force_recovery)
		    || recv_sys->found_corrupt_fs) {
			log_mutex_exit();
			return(DB_ERROR);
		}
	}

	/* NOTE: we always do a 'recovery' at startup, but only if
	there is something wrong we will print a message to the
	user about recovery: */

	if (flush_lsn == checkpoint_lsn + SIZE_OF_MLOG_CHECKPOINT
	    && recv_sys->mlog_checkpoint_lsn == checkpoint_lsn) {
		/* The redo log is logically empty. */
	} else if (checkpoint_lsn != flush_lsn) {
		ut_ad(!srv_log_files_created);

		if (checkpoint_lsn + SIZE_OF_MLOG_CHECKPOINT < flush_lsn) {
			ib::warn() << "Are you sure you are using the"
				" right ib_logfiles to start up the database?"
				" Log sequence number in the ib_logfiles is "
				<< checkpoint_lsn << ", less than the"
				" log sequence number in the first system"
				" tablespace file header, " << flush_lsn << ".";
		}

		if (!recv_needed_recovery) {

			ib::info() << "The log sequence number " << flush_lsn
				<< " in the system tablespace does not match"
				" the log sequence number " << checkpoint_lsn
				<< " in the ib_logfiles!";

			if (srv_read_only_mode) {
				ib::error() << "innodb_read_only"
					" prevents crash recovery";
				log_mutex_exit();
				return(DB_READ_ONLY);
			}

			recv_needed_recovery = true;
		}
	}

	log_sys.lsn = recv_sys->recovered_lsn;

	if (recv_needed_recovery) {
		bool missing_tablespace = false;

		err = recv_init_crash_recovery_spaces(
			rescan, missing_tablespace);

		if (err != DB_SUCCESS) {
			log_mutex_exit();
			return(err);
		}

		/* If there is any missing tablespace and rescan is needed
		then there is a possiblity that hash table will not contain
		all space ids redo logs. Rescan the remaining unstored
		redo logs for the validation of missing tablespace. */
		ut_ad(rescan || !missing_tablespace);

		while (missing_tablespace) {
			DBUG_PRINT("ib_log", ("Rescan of redo log to validate "
					      "the missing tablespace. Scan "
					      "from last stored LSN " LSN_PF,
					      recv_sys->last_stored_lsn));

			lsn_t recent_stored_lsn = recv_sys->last_stored_lsn;
			rescan = recv_group_scan_log_recs(
				checkpoint_lsn, &recent_stored_lsn, false);

			ut_ad(!recv_sys->found_corrupt_fs);

			missing_tablespace = false;

			err = recv_sys->found_corrupt_log
				? DB_ERROR
				: recv_validate_tablespace(
					rescan, missing_tablespace);

			if (err != DB_SUCCESS) {
				log_mutex_exit();
				return err;
			}

			rescan = true;
		}

		if (srv_operation == SRV_OPERATION_NORMAL) {
			buf_dblwr_process();
		}

		ut_ad(srv_force_recovery <= SRV_FORCE_NO_UNDO_LOG_SCAN);

		/* Spawn the background thread to flush dirty pages
		from the buffer pools. */
		recv_writer_thread_active = true;
		os_thread_create(recv_writer_thread, 0, 0);

		if (rescan) {
			contiguous_lsn = checkpoint_lsn;

			recv_group_scan_log_recs(
				checkpoint_lsn, &contiguous_lsn, true);

			if ((recv_sys->found_corrupt_log
			     && !srv_force_recovery)
			    || recv_sys->found_corrupt_fs) {
				log_mutex_exit();
				return(DB_ERROR);
			}
		}
	} else {
		ut_ad(!rescan || recv_sys->n_addrs == 0);
	}

	if (log_sys.log.scanned_lsn < checkpoint_lsn
	    || log_sys.log.scanned_lsn < recv_max_page_lsn) {

		ib::error() << "We scanned the log up to "
			<< log_sys.log.scanned_lsn
			<< ". A checkpoint was at " << checkpoint_lsn << " and"
			" the maximum LSN on a database page was "
			<< recv_max_page_lsn << ". It is possible that the"
			" database is now corrupt!";
	}

	if (recv_sys->recovered_lsn < checkpoint_lsn) {
		log_mutex_exit();

		ib::error() << "Recovered only to lsn:"
			    << recv_sys->recovered_lsn << " checkpoint_lsn: " << checkpoint_lsn;

		return(DB_ERROR);
	}

	log_sys.next_checkpoint_lsn = checkpoint_lsn;
	log_sys.next_checkpoint_no = checkpoint_no + 1;

	recv_synchronize_groups();

	if (!recv_needed_recovery) {
		ut_a(checkpoint_lsn == recv_sys->recovered_lsn);
	} else {
		srv_start_lsn = recv_sys->recovered_lsn;
	}

	log_sys.buf_free = ulong(log_sys.lsn % OS_FILE_LOG_BLOCK_SIZE);
	log_sys.buf_next_to_write = log_sys.buf_free;
	log_sys.write_lsn = log_sys.lsn;

	log_sys.last_checkpoint_lsn = checkpoint_lsn;

	if (!srv_read_only_mode && srv_operation == SRV_OPERATION_NORMAL) {
		/* Write a MLOG_CHECKPOINT marker as the first thing,
		before generating any other redo log. This ensures
		that subsequent crash recovery will be possible even
		if the server were killed soon after this. */
		fil_names_clear(log_sys.last_checkpoint_lsn, true);
	}

	MONITOR_SET(MONITOR_LSN_CHECKPOINT_AGE,
		    log_sys.lsn - log_sys.last_checkpoint_lsn);

	log_sys.next_checkpoint_no = ++checkpoint_no;

	mutex_enter(&recv_sys->mutex);

	recv_sys->apply_log_recs = TRUE;

	mutex_exit(&recv_sys->mutex);

	log_mutex_exit();

	recv_lsn_checks_on = true;

	/* The database is now ready to start almost normal processing of user
	transactions: transaction rollbacks and the application of the log
	records in the hash table can be run in background. */

	return(DB_SUCCESS);
}

/** Complete recovery from a checkpoint. */
void
recv_recovery_from_checkpoint_finish(void)
{
	/* Make sure that the recv_writer thread is done. This is
	required because it grabs various mutexes and we want to
	ensure that when we enable sync_order_checks there is no
	mutex currently held by any thread. */
	mutex_enter(&recv_sys->writer_mutex);

	/* Free the resources of the recovery system */
	recv_recovery_on = false;

	/* By acquring the mutex we ensure that the recv_writer thread
	won't trigger any more LRU batches. Now wait for currently
	in progress batches to finish. */
	buf_flush_wait_LRU_batch_end();

	mutex_exit(&recv_sys->writer_mutex);

	ulint count = 0;
	while (recv_writer_thread_active) {
		++count;
		os_thread_sleep(100000);
		if (srv_print_verbose_log && count > 600) {
			ib::info() << "Waiting for recv_writer to"
				" finish flushing of buffer pool";
			count = 0;
		}
	}

	recv_sys_debug_free();

	/* Free up the flush_rbt. */
	buf_flush_free_flush_rbt();
}

/********************************************************//**
Initiates the rollback of active transactions. */
void
recv_recovery_rollback_active(void)
/*===============================*/
{
	ut_ad(!recv_writer_thread_active);

	/* Switch latching order checks on in sync0debug.cc, if
	--innodb-sync-debug=true (default) */
	ut_d(sync_check_enable());

	/* We can't start any (DDL) transactions if UNDO logging
	has been disabled, additionally disable ROLLBACK of recovered
	user transactions. */
	if (srv_force_recovery < SRV_FORCE_NO_TRX_UNDO
	    && !srv_read_only_mode) {

		/* Drop partially created indexes. */
		row_merge_drop_temp_indexes();
		/* Drop garbage tables. */
		row_mysql_drop_garbage_tables();

		/* Drop any auxiliary tables that were not dropped when the
		parent table was dropped. This can happen if the parent table
		was dropped but the server crashed before the auxiliary tables
		were dropped. */
		fts_drop_orphaned_tables();

		/* Rollback the uncommitted transactions which have no user
		session */

		trx_rollback_is_active = true;
		os_thread_create(trx_rollback_all_recovered, 0, 0);
	}
}

/******************************************************//**
Resets the logs. The contents of log files will be lost! */
void
recv_reset_logs(
/*============*/
	lsn_t		lsn)		/*!< in: reset to this lsn
					rounded up to be divisible by
					OS_FILE_LOG_BLOCK_SIZE, after
					which we add
					LOG_BLOCK_HDR_SIZE */
{
	ut_ad(log_mutex_own());

	log_sys.lsn = ut_uint64_align_up(lsn, OS_FILE_LOG_BLOCK_SIZE);

	log_sys.log.lsn = log_sys.lsn;
	log_sys.log.lsn_offset = LOG_FILE_HDR_SIZE;

	log_sys.buf_next_to_write = 0;
	log_sys.write_lsn = log_sys.lsn;

	log_sys.next_checkpoint_no = 0;
	log_sys.last_checkpoint_lsn = 0;

	memset(log_sys.buf, 0, srv_log_buffer_size);
	log_block_init(log_sys.buf, log_sys.lsn);
	log_block_set_first_rec_group(log_sys.buf, LOG_BLOCK_HDR_SIZE);

	log_sys.buf_free = LOG_BLOCK_HDR_SIZE;
	log_sys.lsn += LOG_BLOCK_HDR_SIZE;

	MONITOR_SET(MONITOR_LSN_CHECKPOINT_AGE,
		    (log_sys.lsn - log_sys.last_checkpoint_lsn));

	log_mutex_exit();

	/* Reset the checkpoint fields in logs */

	log_make_checkpoint_at(LSN_MAX, TRUE);

	log_mutex_enter();
}

/** Find a doublewrite copy of a page.
@param[in]	space_id	tablespace identifier
@param[in]	page_no		page number
@return	page frame
@retval NULL if no page was found */

const byte*
recv_dblwr_t::find_page(ulint space_id, ulint page_no)
{
	typedef std::vector<const byte*, ut_allocator<const byte*> >
		matches_t;

	matches_t	matches;
	const byte*	result = 0;

	for (list::iterator i = pages.begin(); i != pages.end(); ++i) {
		if (page_get_space_id(*i) == space_id
		    && page_get_page_no(*i) == page_no) {
			matches.push_back(*i);
		}
	}

	if (matches.size() == 1) {
		result = matches[0];
	} else if (matches.size() > 1) {

		lsn_t max_lsn	= 0;
		lsn_t page_lsn	= 0;

		for (matches_t::iterator i = matches.begin();
		     i != matches.end();
		     ++i) {

			page_lsn = mach_read_from_8(*i + FIL_PAGE_LSN);

			if (page_lsn > max_lsn) {
				max_lsn = page_lsn;
				result = *i;
			}
		}
	}

	return(result);
}

#ifndef DBUG_OFF
/** Return string name of the redo log record type.
@param[in]	type	record log record enum
@return string name of record log record */
static const char* get_mlog_string(mlog_id_t type)
{
	switch (type) {
	case MLOG_SINGLE_REC_FLAG:
		return("MLOG_SINGLE_REC_FLAG");

	case MLOG_1BYTE:
		return("MLOG_1BYTE");

	case MLOG_2BYTES:
		return("MLOG_2BYTES");

	case MLOG_4BYTES:
		return("MLOG_4BYTES");

	case MLOG_8BYTES:
		return("MLOG_8BYTES");

	case MLOG_REC_INSERT:
		return("MLOG_REC_INSERT");

	case MLOG_REC_CLUST_DELETE_MARK:
		return("MLOG_REC_CLUST_DELETE_MARK");

	case MLOG_REC_SEC_DELETE_MARK:
		return("MLOG_REC_SEC_DELETE_MARK");

	case MLOG_REC_UPDATE_IN_PLACE:
		return("MLOG_REC_UPDATE_IN_PLACE");

	case MLOG_REC_DELETE:
		return("MLOG_REC_DELETE");

	case MLOG_LIST_END_DELETE:
		return("MLOG_LIST_END_DELETE");

	case MLOG_LIST_START_DELETE:
		return("MLOG_LIST_START_DELETE");

	case MLOG_LIST_END_COPY_CREATED:
		return("MLOG_LIST_END_COPY_CREATED");

	case MLOG_PAGE_REORGANIZE:
		return("MLOG_PAGE_REORGANIZE");

	case MLOG_PAGE_CREATE:
		return("MLOG_PAGE_CREATE");

	case MLOG_UNDO_INSERT:
		return("MLOG_UNDO_INSERT");

	case MLOG_UNDO_ERASE_END:
		return("MLOG_UNDO_ERASE_END");

	case MLOG_UNDO_INIT:
		return("MLOG_UNDO_INIT");

	case MLOG_UNDO_HDR_REUSE:
		return("MLOG_UNDO_HDR_REUSE");

	case MLOG_UNDO_HDR_CREATE:
		return("MLOG_UNDO_HDR_CREATE");

	case MLOG_REC_MIN_MARK:
		return("MLOG_REC_MIN_MARK");

	case MLOG_IBUF_BITMAP_INIT:
		return("MLOG_IBUF_BITMAP_INIT");

#ifdef UNIV_LOG_LSN_DEBUG
	case MLOG_LSN:
		return("MLOG_LSN");
#endif /* UNIV_LOG_LSN_DEBUG */

	case MLOG_WRITE_STRING:
		return("MLOG_WRITE_STRING");

	case MLOG_MULTI_REC_END:
		return("MLOG_MULTI_REC_END");

	case MLOG_DUMMY_RECORD:
		return("MLOG_DUMMY_RECORD");

	case MLOG_FILE_DELETE:
		return("MLOG_FILE_DELETE");

	case MLOG_COMP_REC_MIN_MARK:
		return("MLOG_COMP_REC_MIN_MARK");

	case MLOG_COMP_PAGE_CREATE:
		return("MLOG_COMP_PAGE_CREATE");

	case MLOG_COMP_REC_INSERT:
		return("MLOG_COMP_REC_INSERT");

	case MLOG_COMP_REC_CLUST_DELETE_MARK:
		return("MLOG_COMP_REC_CLUST_DELETE_MARK");

	case MLOG_COMP_REC_UPDATE_IN_PLACE:
		return("MLOG_COMP_REC_UPDATE_IN_PLACE");

	case MLOG_COMP_REC_DELETE:
		return("MLOG_COMP_REC_DELETE");

	case MLOG_COMP_LIST_END_DELETE:
		return("MLOG_COMP_LIST_END_DELETE");

	case MLOG_COMP_LIST_START_DELETE:
		return("MLOG_COMP_LIST_START_DELETE");

	case MLOG_COMP_LIST_END_COPY_CREATED:
		return("MLOG_COMP_LIST_END_COPY_CREATED");

	case MLOG_COMP_PAGE_REORGANIZE:
		return("MLOG_COMP_PAGE_REORGANIZE");

	case MLOG_FILE_CREATE2:
		return("MLOG_FILE_CREATE2");

	case MLOG_ZIP_WRITE_NODE_PTR:
		return("MLOG_ZIP_WRITE_NODE_PTR");

	case MLOG_ZIP_WRITE_BLOB_PTR:
		return("MLOG_ZIP_WRITE_BLOB_PTR");

	case MLOG_ZIP_WRITE_HEADER:
		return("MLOG_ZIP_WRITE_HEADER");

	case MLOG_ZIP_PAGE_COMPRESS:
		return("MLOG_ZIP_PAGE_COMPRESS");

	case MLOG_ZIP_PAGE_COMPRESS_NO_DATA:
		return("MLOG_ZIP_PAGE_COMPRESS_NO_DATA");

	case MLOG_ZIP_PAGE_REORGANIZE:
		return("MLOG_ZIP_PAGE_REORGANIZE");

	case MLOG_ZIP_WRITE_TRX_ID:
		return("MLOG_ZIP_WRITE_TRX_ID");

	case MLOG_FILE_RENAME2:
		return("MLOG_FILE_RENAME2");

	case MLOG_FILE_NAME:
		return("MLOG_FILE_NAME");

	case MLOG_CHECKPOINT:
		return("MLOG_CHECKPOINT");

	case MLOG_PAGE_CREATE_RTREE:
		return("MLOG_PAGE_CREATE_RTREE");

	case MLOG_COMP_PAGE_CREATE_RTREE:
		return("MLOG_COMP_PAGE_CREATE_RTREE");

	case MLOG_INIT_FILE_PAGE2:
		return("MLOG_INIT_FILE_PAGE2");

	case MLOG_INDEX_LOAD:
		return("MLOG_INDEX_LOAD");

	case MLOG_TRUNCATE:
		return("MLOG_TRUNCATE");

	case MLOG_FILE_WRITE_CRYPT_DATA:
		return("MLOG_FILE_WRITE_CRYPT_DATA");
	}
	DBUG_ASSERT(0);
	return(NULL);
}
#endif /* !DBUG_OFF */
