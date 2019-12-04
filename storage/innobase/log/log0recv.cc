/*****************************************************************************

Copyright (c) 1997, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2013, 2019, MariaDB Corporation.

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
@file log/log0recv.cc
Recovery

Created 9/20/1997 Heikki Tuuri
*******************************************************/

#include "univ.i"

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
#include "buf0rea.h"
#include "srv0srv.h"
#include "srv0start.h"

/** Log records are stored in the hash table in chunks at most of this size;
this must be less than srv_page_size as it is stored in the buffer pool */
#define RECV_DATA_BLOCK_SIZE	(MEM_MAX_ALLOC_IN_BUF - sizeof(recv_t::data_t) - REDZONE_SIZE)

/** Read-ahead area in applying log records to file pages */
#define RECV_READ_AHEAD_AREA	32U

/** The recovery system */
recv_sys_t	recv_sys;
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
mysql_pfs_key_t	recv_writer_thread_key;
#endif /* UNIV_PFS_THREAD */

/** Is recv_writer_thread active? */
bool	recv_writer_thread_active;


/** Stored physiological log record with byte-oriented start/end LSN */
struct recv_t : public log_rec_t
{
  /**
    Constructor.
    @param len       total length of the redo log record body, in bytes
    @param type      redo log record chunk
    @param start_lsn start LSN of the mini-transaction
    @param end_lsn   end LSN of the mini-transaction
  */
  recv_t(uint32_t len, mlog_id_t type, lsn_t start_lsn, lsn_t end_lsn) :
    log_rec_t(end_lsn), len(len), type(type), start_lsn(start_lsn), data(NULL)
  {}

  /** log record body length in bytes */
  const uint32_t len;
  /** log record type */
  const mlog_id_t type;
  /** start LSN of the mini-transaction (not necessarily of this record) */
  const lsn_t start_lsn;
  /** log record */
  struct data_t
  {
    /** pointer to the next chunk, or NULL for the last chunk.  The
    log record data (at most RECV_DATA_BLOCK_SIZE bytes per chunk)
    is stored immediately after this field. */
    data_t *next= NULL;

    data_t() {}
    /**
      Constructor.
      @param chunk   redo log record chunk
      @param len     length of the chunk, in bytes
    */
    data_t(const void* chunk, size_t len)
    {
      memcpy(reinterpret_cast<void*>(this + 1), chunk, len);
    }

    /**
      Append a log snippet.
      @param d  log snippet
    */
    void append(data_t *d) { ut_ad(!next); ut_ad(!d->next); next= d; }
  }* data;
};


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

	/** FSP_SIZE of tablespace */
	ulint		size;

	/** the log sequence number of the last observed MLOG_INDEX_LOAD
	record for the tablespace */
	lsn_t		enable_lsn;

	/** Constructor */
	file_name_t(std::string name_, bool deleted) :
		name(name_), space(NULL), status(deleted ? DELETED: NORMAL),
		size(0), enable_lsn(0) {}

	/** Report a MLOG_INDEX_LOAD operation, meaning that
	mlog_init for any earlier LSN must be skipped.
	@param lsn	log sequence number of the MLOG_INDEX_LOAD */
	void mlog_index_load(lsn_t lsn)
	{
		if (enable_lsn < lsn) enable_lsn = lsn;
	}
};

/** Map of dirty tablespaces during recovery */
typedef std::map<
	ulint,
	file_name_t,
	std::less<ulint>,
	ut_allocator<std::pair<const ulint, file_name_t> > >	recv_spaces_t;

static recv_spaces_t	recv_spaces;

/** Report optimized DDL operation (without redo log),
corresponding to MLOG_INDEX_LOAD.
@param[in]	space_id	tablespace identifier
*/
void (*log_optimized_ddl_op)(ulint space_id);

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

/** Information about initializing page contents during redo log processing */
class mlog_init_t
{
public:
	/** A page initialization operation that was parsed from
	the redo log */
	struct init {
		/** log sequence number of the page initialization */
		lsn_t lsn;
		/** Whether btr_page_create() avoided a read of the page.

		At the end of the last recovery batch, mark_ibuf_exist()
		will mark pages for which this flag is set. */
		bool created;
	};

private:
	typedef std::map<const page_id_t, init,
			 std::less<const page_id_t>,
			 ut_allocator<std::pair<const page_id_t, init> > >
		map;
	/** Map of page initialization operations.
	FIXME: Merge this to recv_sys.pages! */
	map inits;
public:
	/** Record that a page will be initialized by the redo log.
	@param[in]	page_id		page identifier
	@param[in]	lsn		log sequence number */
	void add(const page_id_t page_id, lsn_t lsn)
	{
		ut_ad(mutex_own(&recv_sys.mutex));
		const init init = { lsn, false };
		std::pair<map::iterator, bool> p = inits.insert(
			map::value_type(page_id, init));
		ut_ad(!p.first->second.created);
		if (!p.second && p.first->second.lsn < init.lsn) {
			p.first->second = init;
		}
	}

	/** Get the last stored lsn of the page id and its respective
	init/load operation.
	@param[in]	page_id	page id
	@param[in,out]	init	initialize log or load log
	@return the latest page initialization;
	not valid after releasing recv_sys.mutex. */
	init& last(page_id_t page_id)
	{
		ut_ad(mutex_own(&recv_sys.mutex));
		return inits.find(page_id)->second;
	}

	/** At the end of each recovery batch, reset the 'created' flags. */
	void reset()
	{
		ut_ad(mutex_own(&recv_sys.mutex));
		ut_ad(recv_no_ibuf_operations);
		for (map::value_type& i : inits) {
			i.second.created = false;
		}
	}

	/** On the last recovery batch, mark whether there exist
	buffered changes for the pages that were initialized
	by buf_page_create() and still reside in the buffer pool.
	@param[in,out]	mtr	dummy mini-transaction */
	void mark_ibuf_exist(mtr_t& mtr)
	{
		ut_ad(mutex_own(&recv_sys.mutex));
		ut_ad(!recv_no_ibuf_operations);
		mtr.start();

		for (const map::value_type& i : inits) {
			if (!i.second.created) {
				continue;
			}
			if (buf_block_t* block = buf_page_get_gen(
				    i.first, 0, RW_X_LATCH, NULL,
				    BUF_GET_IF_IN_POOL, __FILE__, __LINE__,
				    &mtr)) {
				mutex_exit(&recv_sys.mutex);
				block->page.ibuf_exist = ibuf_page_exists(
					block->page);
				mtr.commit();
				mtr.start();
				mutex_enter(&recv_sys.mutex);
			}
		}

		mtr.commit();
	}

	/** Clear the data structure */
	void clear() { inits.clear(); }
};

static mlog_init_t mlog_init;

/** Process a record that indicates that a tablespace is
being shrunk in size.
@param page_id	first page identifier that is not in the file
@param lsn	log sequence number of the shrink operation */
inline void recv_sys_t::trim(const page_id_t page_id, lsn_t lsn)
{
	DBUG_ENTER("recv_sys_t::trim");
	DBUG_LOG("ib_log",
		 "discarding log beyond end of tablespace "
		 << page_id << " before LSN " << lsn);
	ut_ad(mutex_own(&mutex));
	for (recv_sys_t::map::iterator p = pages.lower_bound(page_id);
	     p != pages.end() && p->first.space() == page_id.space();) {
		recv_sys_t::map::iterator r = p++;
		if (r->second.log.trim(lsn)) {
			pages.erase(r);
		}
	}
	if (fil_space_t* space = fil_space_get(page_id.space())) {
		ut_ad(UT_LIST_GET_LEN(space->chain) == 1);
		fil_node_t* file = UT_LIST_GET_FIRST(space->chain);
		ut_ad(file->is_open());
		os_file_truncate(file->name, file->handle,
				 os_offset_t{page_id.page_no()}
				 << srv_page_size_shift, true);
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
fil_name_process(char* name, ulint len, ulint space_id, bool deleted)
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

				if (f.size && f.space == NULL) {
					fil_space_set_recv_size(space->id, f.size);
				}

				f.name = fname.name;
				f.space = space;
				f.status = file_name_t::NORMAL;
			} else {
				ib::error() << "Tablespace " << space_id
					<< " has been found in two places: '"
					<< f.name << "' and '" << name << "'."
					" You must delete one of them.";
				recv_sys.found_corrupt_fs = true;
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
					<< "At LSN: " << recv_sys.recovered_lsn
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
				recv_sys.found_corrupt_fs = true;
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
@param[in,out]	ptr		redo log record
@param[in]	end		end of the redo log buffer
@param[in]	space_id	the tablespace ID
@param[in]	first_page_no	first page number in the file
@param[in]	type		MLOG_FILE_NAME or MLOG_FILE_DELETE
or MLOG_FILE_CREATE2 or MLOG_FILE_RENAME2
@param[in]	apply		whether to apply the record
@return pointer to next redo log record
@retval NULL if this log record was truncated */
static
const byte*
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
		|| (!first_page_no != !memcmp(ptr + len - 5, DOT_IBD, 5));

	if (!corrupt && !memchr(ptr, OS_PATH_SEPARATOR, len)) {
		if (byte* c = static_cast<byte*>
		    (memchr(ptr, OS_PATH_SEPARATOR_ALT, len))) {
			ut_ad(c >= ptr);
			ut_ad(c < ptr + len);
			do {
				*c = OS_PATH_SEPARATOR;
			} while ((c = static_cast<byte*>
				  (memchr(ptr, OS_PATH_SEPARATOR_ALT,
					  len - ulint(c - ptr)))) != NULL);
		} else {
			corrupt = true;
		}
	}

	byte*	end_ptr	= ptr + len;

	switch (type) {
	default:
		ut_ad(0); // the caller checked this
		/* fall through */
	case MLOG_FILE_NAME:
		if (corrupt) {
			ib::error() << "MLOG_FILE_NAME incorrect:" << ptr;
			recv_sys.found_corrupt_log = true;
			break;
		}

		fil_name_process(
			reinterpret_cast<char*>(ptr), len, space_id, false);
		break;
	case MLOG_FILE_DELETE:
		if (corrupt) {
			ib::error() << "MLOG_FILE_DELETE incorrect:" << ptr;
			recv_sys.found_corrupt_log = true;
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
				UT_ARR_SIZE(recv_sys.truncated_undo_spaces)
				== TRX_SYS_MAX_UNDO_SPACES);
			recv_sys_t::trunc& t = recv_sys.truncated_undo_spaces[
				space_id - srv_undo_space_id_start];
			t.lsn = recv_sys.recovered_lsn;
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
			recv_sys.found_corrupt_log = true;
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
			|| memcmp(new_name + new_len - 5, DOT_IBD, 5) != 0;

		if (!corrupt && !memchr(new_name, OS_PATH_SEPARATOR, new_len)) {
			if (byte* c = static_cast<byte*>
			    (memchr(new_name, OS_PATH_SEPARATOR_ALT,
				    new_len))) {
				ut_ad(c >= new_name);
				ut_ad(c < new_name + new_len);
				do {
					*c = OS_PATH_SEPARATOR;
				} while ((c = static_cast<byte*>
					  (memchr(ptr, OS_PATH_SEPARATOR_ALT,
						  new_len
						  - ulint(c - new_name))))
					 != NULL);
			} else {
				corrupt = true;
			}
		}

		if (corrupt) {
			ib::error() << "MLOG_FILE_RENAME2 new_name incorrect:" << ptr
				    << " new_name: " << new_name;
			recv_sys.found_corrupt_log = true;
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
			recv_sys.found_corrupt_fs = true;
		}
	}

	return(end_ptr);
}

/** Clean up after recv_sys_t::create() */
void recv_sys_t::close()
{
	ut_ad(this == &recv_sys);
	ut_ad(!recv_writer_thread_active);

	if (is_initialised()) {
		dblwr.pages.clear();
		pages.clear();

		if (heap) {
			mem_heap_free(heap);
			heap = NULL;
		}

		if (flush_start) {
			os_event_destroy(flush_start);
		}

		if (flush_end) {
			os_event_destroy(flush_end);
		}

		if (buf) {
			ut_free_dodump(buf, buf_size);
			buf = NULL;
		}

		buf_size = 0;
		mutex_free(&writer_mutex);
		mutex_free(&mutex);
	}

	recv_spaces.clear();
	mlog_init.clear();
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
	recv_n_pool_free_frames	= 384;
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

		mutex_enter(&recv_sys.writer_mutex);

		if (!recv_recovery_is_on()) {
			mutex_exit(&recv_sys.writer_mutex);
			break;
		}

		/* Flush pages from end of LRU if required */
		os_event_reset(recv_sys.flush_end);
		recv_sys.flush_type = BUF_FLUSH_LRU;
		os_event_set(recv_sys.flush_start);
		os_event_wait(recv_sys.flush_end);

		mutex_exit(&recv_sys.writer_mutex);
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
void recv_sys_t::create()
{
	ut_ad(this == &recv_sys);
	ut_ad(!is_initialised());
	ut_ad(!flush_start);
	ut_ad(!flush_end);
	mutex_create(LATCH_ID_RECV_SYS, &mutex);
	mutex_create(LATCH_ID_RECV_WRITER, &writer_mutex);

	heap = mem_heap_create_typed(256, MEM_HEAP_FOR_RECV_SYS);

	if (!srv_read_only_mode) {
		flush_start = os_event_create(0);
		flush_end = os_event_create(0);
	}

	flush_type = BUF_FLUSH_LRU;
	apply_log_recs = false;
	apply_batch_on = false;

	ulint size = buf_pool_get_curr_size();
	/* Set appropriate value of recv_n_pool_free_frames. */
	if (size >= 10 << 20) {
		/* Buffer pool of size greater than 10 MB. */
		recv_n_pool_free_frames = 512;
	}

	buf = static_cast<byte*>(ut_malloc_dontdump(RECV_PARSING_BUF_SIZE));
	buf_size = RECV_PARSING_BUF_SIZE;
	len = 0;
	parse_start_lsn = 0;
	scanned_lsn = 0;
	scanned_checkpoint_no = 0;
	recovered_offset = 0;
	recovered_lsn = 0;
	found_corrupt_log = false;
	found_corrupt_fs = false;
	mlog_checkpoint_lsn = 0;

	progress_time = time(NULL);
	recv_max_page_lsn = 0;

	memset(truncated_undo_spaces, 0, sizeof truncated_undo_spaces);
	last_stored_lsn = 0;
}

/** Clear a fully processed set of stored redo log records. */
inline void recv_sys_t::clear()
{
	ut_ad(mutex_own(&mutex));
	pages.clear();
	mem_heap_empty(heap);
}

/** Free most recovery data structures. */
void recv_sys_t::debug_free()
{
	ut_ad(this == &recv_sys);
	ut_ad(is_initialised());
	mutex_enter(&mutex);

	pages.clear();
	mem_heap_free(heap);
	ut_free_dodump(buf, buf_size);

	buf = NULL;
	heap = NULL;

	/* wake page cleaner up to progress */
	if (!srv_read_only_mode) {
		ut_ad(!recv_recovery_is_on());
		ut_ad(!recv_writer_thread_active);
		os_event_reset(buf_flush_event);
		os_event_set(flush_start);
	}

	mutex_exit(&mutex);
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
	       0,
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
fail:
			end_lsn = *start_lsn;
			success = false;
			break;
		}

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
			goto fail;
		}

		if (is_encrypted()
		    && !log_crypt(buf, *start_lsn,
				  OS_FILE_LOG_BLOCK_SIZE,
				  LOG_DECRYPT)) {
			goto fail;
		}

		ulint dl = log_block_get_data_len(buf);
		if (dl < LOG_BLOCK_HDR_SIZE
		    || (dl != OS_FILE_LOG_BLOCK_SIZE
			&& dl > log_sys.trailer_offset())) {
			recv_sys.found_corrupt_log = true;
			goto fail;
		}
	}

	if (recv_sys.report(time(NULL))) {
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
	const lsn_t recovered_lsn = recv_sys.recovered_lsn;

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
		log_write_checkpoint_info(0);
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

			log_sys.log.set_lsn(mach_read_from_8(
				buf + LOG_CHECKPOINT_LSN));
			log_sys.log.set_lsn_offset(
				lsn_t(mach_read_from_4(buf + OFFSET_HIGH32))
				<< 32
				| mach_read_from_4(buf + OFFSET_LOW32));
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
	       0,
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
	recv_sys.parse_start_lsn = recv_sys.recovered_lsn
		= recv_sys.scanned_lsn
		= recv_sys.mlog_checkpoint_lsn = lsn;
	log_sys.last_checkpoint_lsn = log_sys.next_checkpoint_lsn
		= log_sys.lsn = log_sys.write_lsn
		= log_sys.current_flush_lsn = log_sys.flushed_to_disk_lsn
		= lsn;
	log_sys.next_checkpoint_no = 0;
	return(DB_SUCCESS);
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
	log_sys.log.subformat = log_sys.log.format != log_t::FORMAT_3_23
		? mach_read_from_4(buf + LOG_HEADER_SUBFORMAT)
		: 0;
	if (log_sys.log.format != log_t::FORMAT_3_23
	    && !recv_check_log_header_checksum(buf)) {
		ib::error() << "Invalid redo log header checksum.";
		return(DB_CORRUPTION);
	}

	char creator[LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR + 1];

	memcpy(creator, buf + LOG_HEADER_CREATOR, sizeof creator);
	/* Ensure that the string is NUL-terminated. */
	creator[LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR] = 0;

	switch (log_sys.log.format) {
	case log_t::FORMAT_3_23:
		return(recv_find_max_checkpoint_0(max_field));
	case log_t::FORMAT_10_2:
	case log_t::FORMAT_10_2 | log_t::FORMAT_ENCRYPTED:
	case log_t::FORMAT_10_3:
	case log_t::FORMAT_10_3 | log_t::FORMAT_ENCRYPTED:
	case log_t::FORMAT_10_4:
	case log_t::FORMAT_10_4 | log_t::FORMAT_ENCRYPTED:
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
			log_sys.log.set_lsn(mach_read_from_8(
				buf + LOG_CHECKPOINT_LSN));
			log_sys.log.set_lsn_offset(mach_read_from_8(
				buf + LOG_CHECKPOINT_OFFSET));
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

	return(DB_SUCCESS);
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
const byte*
recv_parse_or_apply_log_rec_body(
	mlog_id_t	type,
	const byte*	ptr,
	const byte*	end_ptr,
	ulint		space_id,
	ulint		page_no,
	bool		apply,
	buf_block_t*	block,
	mtr_t*		mtr)
{
	ut_ad(!block == !mtr);
	ut_ad(!apply || recv_sys.mlog_checkpoint_lsn);

	switch (type) {
	case MLOG_FILE_NAME:
	case MLOG_FILE_DELETE:
	case MLOG_FILE_CREATE2:
	case MLOG_FILE_RENAME2:
		ut_ad(block == NULL);
		/* Collect the file names when parsing the log,
		before applying any log records. */
		return fil_name_parse(const_cast<byte*>(ptr), end_ptr,
				      space_id, page_no, type, apply);
	case MLOG_INDEX_LOAD:
		if (end_ptr < ptr + 8) {
			return(NULL);
		}
		return(ptr + 8);
	case MLOG_TRUNCATE:
		ib::error() << "Cannot crash-upgrade from "
			"old-style TRUNCATE TABLE";
		recv_sys.found_corrupt_log = true;
		return NULL;
	default:
		break;
	}

	dict_index_t*	index	= NULL;
	page_t*		page;
	page_zip_des_t*	page_zip;
#ifdef UNIV_DEBUG
	uint16_t	page_type;
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
		if (recv_sys.recovered_lsn < recv_sys.mlog_checkpoint_lsn) {
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
			    << recv_sys.recovered_lsn << ".";
		recv_sys.found_corrupt_log = true;
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
	case MLOG_MEMSET:
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
		if (ptr && page && !page_no && type == MLOG_4BYTES) {
			switch (ulint offs = mach_read_from_2(old_ptr)) {
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
			ptr = page_cur_parse_insert_rec(false, ptr, end_ptr,
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
		ptr = trx_undo_parse_page_header(ptr, end_ptr, block, mtr);
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
			block, mtr);
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
		if (block) ibuf_bitmap_init_apply(block);
		break;
	case MLOG_INIT_FILE_PAGE2:
		/* Allow anything in page_type when creating a page. */
		if (block) fsp_apply_init_file_page(block);
		break;
	case MLOG_INIT_FREE_PAGE:
		/* The page can be zero-filled and its previous
		contents can be ignored. We do not write or apply
		this record yet. */
		break;
	case MLOG_WRITE_STRING:
		if (page_no || mach_read_from_2(ptr + 2)
		    != 11 + MY_AES_BLOCK_SIZE) {
			/* Not writing crypt_info */
		} else if (fil_space_t* space
			   = fil_space_acquire_silent(space_id)) {
			if (mach_read_from_2(ptr)
			    == FSP_HEADER_OFFSET + XDES_ARR_OFFSET + MAGIC_SZ
			    + space->physical_size() * XDES_SIZE
			    / FSP_EXTENT_SIZE
			    && (ptr[4] == CRYPT_SCHEME_UNENCRYPTED
				|| ptr[4] == CRYPT_SCHEME_1)
			    && ptr[5] == MY_AES_BLOCK_SIZE
			    && ptr[6 + MY_AES_BLOCK_SIZE + 4 + 4]
			    <= FIL_ENCRYPTION_OFF) {
				/* from fil_space_crypt_t::write_page0() */
				fil_crypt_parse(space, ptr + 4);
			}
			space->release();
		}
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
		ptr = page_zip_parse_compress(ptr, end_ptr, block);
		break;
	case MLOG_ZIP_PAGE_COMPRESS_NO_DATA:
		if (NULL != (ptr = mlog_parse_index(
				ptr, end_ptr, TRUE, &index))) {

			ut_a(!page || ((ibool)!!page_is_comp(page)
				== dict_table_is_comp(index->table)));
			if (end_ptr == ptr) {
				ptr = NULL;
				break;
			}
			if (page &&
			    !page_zip_compress(block, index, *ptr, NULL)) {
				ut_error;
			}
			ptr++;
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
		ptr = fil_parse_write_crypt_data(ptr, end_ptr, &err);

		if (err != DB_SUCCESS) {
			recv_sys.found_corrupt_log = TRUE;
		}
		break;
	default:
		ptr = NULL;
		ib::error() << "Incorrect log record type "
			<< ib::hex(unsigned(type));

		recv_sys.found_corrupt_log = true;
	}

	if (index) {
		dict_table_t*	table = index->table;

		dict_mem_index_free(index);
		dict_mem_table_free(table);
	}

	return(ptr);
}

/** Store a redo log record for applying.
@param type	record type
@param page_id	page identifier
@param body	record body
@param rec_end	end of record
@param lsn	start LSN of the mini-transaction
@param end_lsn	end LSN of the mini-transaction */
inline void recv_sys_t::add(mlog_id_t type, const page_id_t page_id,
                            const byte* body, const byte* rec_end, lsn_t lsn,
                            lsn_t end_lsn)
{
  ut_ad(type != MLOG_FILE_DELETE);
  ut_ad(type != MLOG_FILE_CREATE2);
  ut_ad(type != MLOG_FILE_RENAME2);
  ut_ad(type != MLOG_FILE_NAME);
  ut_ad(type != MLOG_DUMMY_RECORD);
  ut_ad(type != MLOG_CHECKPOINT);
  ut_ad(type != MLOG_INDEX_LOAD);
  ut_ad(type != MLOG_TRUNCATE);

  std::pair<map::iterator, bool> p= pages.insert(map::value_type
                                                 (page_id, page_recv_t()));
  page_recv_t& recs= p.first->second;
  ut_ad(p.second == recs.log.empty());

  switch (type) {
  case MLOG_INIT_FILE_PAGE2:
  case MLOG_ZIP_PAGE_COMPRESS:
  case MLOG_INIT_FREE_PAGE:
    recs.will_not_read();
    mlog_init.add(page_id, lsn);
  default:
    break;
  }

  /* Store the log record body in limited-size chunks, because the
  heap grows into the buffer pool. */
  uint32_t len= uint32_t(rec_end - body);
  const uint32_t chunk_limit= static_cast<uint32_t>(RECV_DATA_BLOCK_SIZE);

  recv_t *recv= new (mem_heap_alloc(heap, sizeof(recv_t)))
    recv_t(len, type, lsn, end_lsn);
  recs.log.append(recv);

  for (recv_t::data_t *prev= NULL;;) {
    const uint32_t l= std::min(len, chunk_limit);
    recv_t::data_t *d= new (mem_heap_alloc(heap, sizeof(recv_t::data_t) + l))
      recv_t::data_t(body, l);
    if (prev)
      prev->append(d);
    else
      recv->data= d;
    prev= d;

    body+= l;
    len-= l;
    if (!len)
      break;
  }
}

/** Trim old log records for a page
@param start_lsn oldest log sequence number to preserve
@return whether the entire log was trimmed */
inline bool page_recv_t::recs_t::trim(lsn_t start_lsn)
{
  for (log_rec_t** prev= &head;; *prev= (*prev)->next)
  {
    if (!*prev) return true;
    if ((*prev)->lsn >= start_lsn) return false;
  }
}

inline void page_recv_t::recs_t::clear()
{
  head= tail= NULL;
}


/** Ignore any earlier redo log records for this page. */
inline void page_recv_t::will_not_read()
{
  ut_ad(state == RECV_NOT_PROCESSED || state == RECV_WILL_NOT_READ);
  state= RECV_WILL_NOT_READ;
  log.clear();
}


/*********************************************************************//**
Copies the log record body from recv to buf. */
static ATTRIBUTE_COLD
void
recv_data_copy_to_buf(
/*==================*/
	byte*	buf,	/*!< in: buffer of length at least recv->len */
	const recv_t& recv)	/*!< in: log record */
{
	const recv_t::data_t* recv_data = recv.data;
	ulint len = recv.len;
	const ulint chunk_limit = static_cast<ulint>(RECV_DATA_BLOCK_SIZE);

	do {
		const ulint l = std::min(len, chunk_limit);
		memcpy(buf, reinterpret_cast<const byte*>(recv_data + 1), l);
		recv_data = recv_data->next;
		buf += l;
		len -= l;
	} while (len);
}

/** Apply the hashed log records to the page, if the page lsn is less than the
lsn of a log record.
@param[in,out]	block		buffer pool page
@param[in,out]	mtr		mini-transaction
@param[in,out]	p		recovery address
@param[in,out]	init		page initialization operation, or NULL */
static void recv_recover_page(buf_block_t* block, mtr_t& mtr,
			      const recv_sys_t::map::iterator& p,
			      mlog_init_t::init* init = NULL)
{
	page_t*		page;
	page_zip_des_t*	page_zip;

	ut_ad(mutex_own(&recv_sys.mutex));
	ut_ad(recv_sys.apply_log_recs);
	ut_ad(recv_needed_recovery);
	ut_ad(!init || init->created);
	ut_ad(!init || init->lsn);
	ut_ad(block->page.id == p->first);
	ut_ad(!p->second.is_being_processed());

	if (UNIV_UNLIKELY(srv_print_verbose_log == 2)) {
		ib::info() << "Applying log to page " << block->page.id;
	}

	DBUG_LOG("ib_log", "Applying log to page " << block->page.id);

	p->second.state = page_recv_t::RECV_BEING_PROCESSED;

	mutex_exit(&recv_sys.mutex);

	page = block->frame;
	page_zip = buf_block_get_page_zip(block);

	const lsn_t page_lsn = mach_read_from_8(page + FIL_PAGE_LSN);
	bool free_page = false;
	lsn_t start_lsn = 0, end_lsn = 0;
	ut_d(lsn_t recv_start_lsn = 0);
	const lsn_t init_lsn = init ? init->lsn : 0;
	const ulint chunk_limit = static_cast<ulint>(RECV_DATA_BLOCK_SIZE);

	for (const log_rec_t* l : p->second.log) {
		const recv_t* recv = static_cast<const recv_t*>(l);
		ut_ad(recv->start_lsn);
		ut_ad(recv->lsn);
		ut_ad(recv_start_lsn < recv->start_lsn);
		ut_d(recv_start_lsn = recv->start_lsn);
		ut_ad(end_lsn <= recv->lsn);
		end_lsn = recv->lsn;
		ut_ad(end_lsn <= log_sys.log.scanned_lsn);

		if (recv->start_lsn < page_lsn) {
			/* Ignore this record, because there are later changes
			for this page. */
			DBUG_LOG("ib_log", "apply skip "
				 << get_mlog_string(recv->type)
				 << " LSN " << recv->start_lsn << " < "
				 << page_lsn);
		} else if (recv->start_lsn < init_lsn) {
			DBUG_LOG("ib_log", "init skip "
				 << get_mlog_string(recv->type)
				 << " LSN " << recv->start_lsn << " < "
				 << init_lsn);
		} else {
			if (recv->type == MLOG_INIT_FREE_PAGE) {
				/* This does not really modify the page. */
				free_page = true;
			} else if (!start_lsn) {
				start_lsn = recv->start_lsn;
			}

			if (UNIV_UNLIKELY(srv_print_verbose_log == 2)) {
				ib::info() << "apply " << recv->start_lsn
					   << ":" << recv->type
					   << " len " << recv->len
					   << " page " << block->page.id;
			}

			DBUG_LOG("ib_log", "apply " << recv->start_lsn << ": "
				 << get_mlog_string(recv->type)
				 << " len " << recv->len
				 << " page " << block->page.id);

			byte* buf;
			const byte* recs;

			if (UNIV_UNLIKELY(recv->len > chunk_limit)) {
				/* We have to copy the record body to
				a separate buffer */
				recs = buf = static_cast<byte*>
					(ut_malloc_nokey(recv->len));
				recv_data_copy_to_buf(buf, *recv);
			} else {
				buf = NULL;
				recs = reinterpret_cast<const byte*>
					(recv->data + 1);
			}

			recv_parse_or_apply_log_rec_body(
				recv->type, recs, recs + recv->len,
				block->page.id.space(),
				block->page.id.page_no(), true, block, &mtr);

			end_lsn = recv->start_lsn + recv->len;
			mach_write_to_8(FIL_PAGE_LSN + page, end_lsn);
			mach_write_to_8(srv_page_size
					- FIL_PAGE_END_LSN_OLD_CHKSUM
					+ page, end_lsn);

			if (page_zip) {
				mach_write_to_8(FIL_PAGE_LSN + page_zip->data,
						end_lsn);
			}

			ut_free(buf);
		}
	}

#ifdef UNIV_ZIP_DEBUG
	ut_ad(!fil_page_index_page_check(page)
	      || !page_zip
	      || page_zip_validate_low(page_zip, page, NULL, FALSE));
#endif /* UNIV_ZIP_DEBUG */

	if (start_lsn) {
		log_flush_order_mutex_enter();
		buf_flush_note_modification(block, start_lsn, end_lsn, NULL);
		log_flush_order_mutex_exit();
	} else if (free_page && init) {
		/* There have been no operations than MLOG_INIT_FREE_PAGE.
		Any buffered changes must not be merged. A subsequent
		buf_page_create() from a user thread should discard
		any buffered changes. */
		init->created = false;
		ut_ad(!mtr.has_modifications());
	}

	/* Make sure that committing mtr does not change the modification
	lsn values of page */

	mtr.discard_modifications();
	mtr.commit();

	time_t now = time(NULL);

	mutex_enter(&recv_sys.mutex);

	if (recv_max_page_lsn < page_lsn) {
		recv_max_page_lsn = page_lsn;
	}

	ut_ad(p->second.is_being_processed());
	ut_ad(!recv_sys.pages.empty());
	recv_sys.pages.erase(p);

	if (recv_sys.report(now)) {
		const ulint n = recv_sys.pages.size();
		ib::info() << "To recover: " << n << " pages from log";
		service_manager_extend_timeout(
			INNODB_EXTEND_TIMEOUT_INTERVAL, "To recover: " ULINTPF " pages from log", n);
	}
}

/** Remove records for a corrupted page.
This function should called when srv_force_recovery > 0.
@param[in]	page_id	page id of the corrupted page */
void recv_recover_corrupt_page(page_id_t page_id)
{
	mutex_enter(&recv_sys.mutex);
	recv_sys.pages.erase(page_id);
	mutex_exit(&recv_sys.mutex);
}

/** Apply any buffered redo log to a page that was just read from a data file.
@param[in,out]	bpage	buffer pool page */
void recv_recover_page(buf_page_t* bpage)
{
	mtr_t mtr;
	mtr.start();
	mtr.set_log_mode(MTR_LOG_NONE);

	ut_ad(buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE);
	buf_block_t* block = reinterpret_cast<buf_block_t*>(bpage);

	/* Move the ownership of the x-latch on the page to
	this OS thread, so that we can acquire a second
	x-latch on it.  This is needed for the operations to
	the page to pass the debug checks. */
	rw_lock_x_lock_move_ownership(&block->lock);
	buf_block_buf_fix_inc(block, __FILE__, __LINE__);
	rw_lock_x_lock(&block->lock);
	mtr.memo_push(block, MTR_MEMO_PAGE_X_FIX);

	mutex_enter(&recv_sys.mutex);
	if (recv_sys.apply_log_recs) {
		recv_sys_t::map::iterator p = recv_sys.pages.find(bpage->id);
		if (p != recv_sys.pages.end()
		    && !p->second.is_being_processed()) {
			recv_recover_page(block, mtr, p);
			goto func_exit;
		}
	}

	mtr.commit();
func_exit:
	mutex_exit(&recv_sys.mutex);
	ut_ad(mtr.has_committed());
}

/** Reads in pages which have hashed log records, from an area around a given
page number.
@param[in]	page_id	page id */
static void recv_read_in_area(page_id_t page_id)
{
	ulint	page_nos[RECV_READ_AHEAD_AREA];
	compile_time_assert(ut_is_2pow(RECV_READ_AHEAD_AREA));
	page_id.set_page_no(ut_2pow_round(page_id.page_no(),
					  RECV_READ_AHEAD_AREA));
	const ulint up_limit = page_id.page_no() + RECV_READ_AHEAD_AREA;
	ulint*	p = page_nos;

	for (recv_sys_t::map::iterator i= recv_sys.pages.lower_bound(page_id);
	     i != recv_sys.pages.end()
	     && i->first.space() == page_id.space()
	     && i->first.page_no() < up_limit; i++) {
		if (i->second.state == page_recv_t::RECV_NOT_PROCESSED
		    && !buf_page_peek(i->first)) {
			i->second.state = page_recv_t::RECV_BEING_READ;
			*p++ = i->first.page_no();
		}
	}

	if (p != page_nos) {
		mutex_exit(&recv_sys.mutex);
		buf_read_recv_pages(FALSE, page_id.space(), page_nos,
				    ulint(p - page_nos));
		mutex_enter(&recv_sys.mutex);
	}
}

/** Apply recv_sys.pages to persistent data pages.
@param[in]	last_batch	whether redo log writes are possible */
void recv_apply_hashed_log_recs(bool last_batch)
{
	ut_ad(srv_operation == SRV_OPERATION_NORMAL
	      || srv_operation == SRV_OPERATION_RESTORE
	      || srv_operation == SRV_OPERATION_RESTORE_EXPORT);

	mutex_enter(&recv_sys.mutex);

	while (recv_sys.apply_batch_on) {
		bool abort = recv_sys.found_corrupt_log;
		mutex_exit(&recv_sys.mutex);

		if (abort) {
			return;
		}

		os_thread_sleep(500000);
		mutex_enter(&recv_sys.mutex);
	}

	ut_ad(!last_batch == log_mutex_own());

	recv_no_ibuf_operations = !last_batch
		|| srv_operation == SRV_OPERATION_RESTORE
		|| srv_operation == SRV_OPERATION_RESTORE_EXPORT;

	ut_d(recv_no_log_write = recv_no_ibuf_operations);

	mtr_t mtr;

	if (recv_sys.pages.empty()) {
		goto done;
	}

	if (!log_sys.log.subformat && !srv_force_recovery
	    && srv_undo_tablespaces_open) {
		ib::error() << "Recovery of separately logged"
			" TRUNCATE operations is no longer supported."
			" Set innodb_force_recovery=1"
			" if no *trunc.log files exist";
		recv_sys.found_corrupt_log = true;
		mutex_exit(&recv_sys.mutex);
		return;
	} else {
		const char* msg = last_batch
			? "Starting final batch to recover "
			: "Starting a batch to recover ";
		const ulint n = recv_sys.pages.size();
		ib::info() << msg << n << " pages from redo log.";
		sd_notifyf(0, "STATUS=%s" ULINTPF " pages from redo log",
			   msg, n);
	}

	recv_sys.apply_log_recs = true;
	recv_sys.apply_batch_on = true;

	for (ulint id = srv_undo_tablespaces_open; id--;) {
		const recv_sys_t::trunc& t= recv_sys.truncated_undo_spaces[id];
		if (t.lsn) {
			recv_sys.trim(page_id_t(id + srv_undo_space_id_start,
						t.pages), t.lsn);
		}
	}

	for (recv_sys_t::map::iterator p = recv_sys.pages.begin();
	     p != recv_sys.pages.end();) {
		const page_id_t page_id = p->first;
		page_recv_t& recs = p->second;
		ut_ad(!recs.log.empty());

		switch (recs.state) {
		case page_recv_t::RECV_BEING_READ:
		case page_recv_t::RECV_BEING_PROCESSED:
			p++;
			continue;
		case page_recv_t::RECV_NOT_PROCESSED:
		apply:
			mtr.start();
			mtr.set_log_mode(MTR_LOG_NONE);
			if (buf_block_t* block = buf_page_get_gen(
				    page_id, 0, RW_X_LATCH, NULL,
				    BUF_GET_IF_IN_POOL,
				    __FILE__, __LINE__, &mtr, NULL)) {
				buf_block_dbg_add_level(
					block, SYNC_NO_ORDER_CHECK);
				recv_recover_page(block, mtr, p);
				ut_ad(mtr.has_committed());
			} else {
				mtr.commit();
				recv_read_in_area(page_id);
			}
			break;
		case page_recv_t::RECV_WILL_NOT_READ:
			mlog_init_t::init& i = mlog_init.last(page_id);
			const lsn_t end_lsn = recs.log.last()->lsn;
			if (end_lsn < i.lsn) {
				DBUG_LOG("ib_log", "skip log for page "
					 << page_id
					 << " LSN " << end_lsn
					 << " < " << i.lsn);
ignore:
				recv_sys_t::map::iterator r = p++;
				recv_sys.pages.erase(r);
				continue;
			}

			fil_space_t* space = fil_space_acquire_for_io(
				page_id.space());
			if (!space) {
				goto ignore;
			}

			if (space->enable_lsn) {
do_read:
				space->release_for_io();
				recs.state = page_recv_t::RECV_NOT_PROCESSED;
				goto apply;
			}

			/* Determine if a tablespace could be
			for an internal table for FULLTEXT INDEX.
			For those tables, no MLOG_INDEX_LOAD record
			used to be written when redo logging was
			disabled. Hence, we cannot optimize
			away page reads when crash-upgrading
			from MariaDB versions before 10.4,
			because all the redo log records for
			initializing and modifying the page in
			the past could be older than the page
			in the data file.

			The check is too broad, causing all
			tables whose names start with FTS_ to
			skip the optimization. */
			if ((log_sys.log.format & ~log_t::FORMAT_ENCRYPTED)
			    != log_t::FORMAT_10_4
			    && strstr(space->name, "/FTS_")) {
				goto do_read;
			}

			mtr.start();
			mtr.set_log_mode(MTR_LOG_NONE);
			buf_block_t* block = buf_page_create(
				page_id, space->zip_size(), &mtr);
			p = recv_sys.pages.find(page_id);
			if (p == recv_sys.pages.end()) {
				/* The page happened to exist
				in the buffer pool, or it was
				just being read in. Before
				buf_page_get_with_no_latch()
				returned, all changes must have
				been applied to the page already. */
				mtr.commit();
			} else {
				ut_ad(&recs == &p->second);
				i.created = true;
				buf_block_dbg_add_level(
					block, SYNC_NO_ORDER_CHECK);
				mtr.x_latch_at_savepoint(0, block);
				recv_recover_page(block, mtr, p, &i);
				ut_ad(mtr.has_committed());
			}

			space->release_for_io();
		}

		p = recv_sys.pages.lower_bound(page_id);
	}

	/* Wait until all the pages have been processed */

	while (!recv_sys.pages.empty()) {
		const bool abort = recv_sys.found_corrupt_log
			|| recv_sys.found_corrupt_fs;

		if (recv_sys.found_corrupt_fs && !srv_force_recovery) {
			ib::info() << "Set innodb_force_recovery=1"
				" to ignore corrupted pages.";
		}

		mutex_exit(&(recv_sys.mutex));

		if (abort) {
			return;
		}

		os_thread_sleep(500000);

		mutex_enter(&(recv_sys.mutex));
	}

done:
	if (!last_batch) {
		/* Flush all the file pages to disk and invalidate them in
		the buffer pool */

		mutex_exit(&(recv_sys.mutex));
		log_mutex_exit();

		/* Stop the recv_writer thread from issuing any LRU
		flush batches. */
		mutex_enter(&recv_sys.writer_mutex);

		/* Wait for any currently run batch to end. */
		buf_flush_wait_LRU_batch_end();

		os_event_reset(recv_sys.flush_end);
		recv_sys.flush_type = BUF_FLUSH_LIST;
		os_event_set(recv_sys.flush_start);
		os_event_wait(recv_sys.flush_end);

		buf_pool_invalidate();

		/* Allow batches from recv_writer thread. */
		mutex_exit(&recv_sys.writer_mutex);

		log_mutex_enter();
		mutex_enter(&(recv_sys.mutex));
		mlog_init.reset();
	} else if (!recv_no_ibuf_operations) {
		/* We skipped this in buf_page_create(). */
		mlog_init.mark_ibuf_exist(mtr);
	}

	recv_sys.apply_log_recs = false;
	recv_sys.apply_batch_on = false;

	recv_sys.clear();

	mutex_exit(&recv_sys.mutex);
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
	const byte*	ptr,
	const byte*	end_ptr,
	ulint*		space,
	ulint*		page_no,
	bool		apply,
	const byte**	body)
{
	const byte*	new_ptr;

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
			ut_a(lsn == recv_sys.recovered_lsn);
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
		recv_sys.found_corrupt_log = true;
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
	    && apply
	    && mach_read_from_2(old_ptr) == FSP_HEADER_OFFSET + FSP_SIZE) {
		old_ptr += 2;

		ulint size = mach_parse_compressed(&old_ptr, end_ptr);

		recv_spaces_t::iterator it = recv_spaces.find(*space);

		ut_ad(!recv_sys.mlog_checkpoint_lsn
		      || *space == TRX_SYS_SPACE
		      || srv_is_undo_tablespace(*space)
		      || it != recv_spaces.end());

		if (it != recv_spaces.end() && !it->second.space) {
			it->second.size = size;
		}

		fil_space_set_recv_size(*space, size);
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
	unsigned frag_len = (lsn % OS_FILE_LOG_BLOCK_SIZE) - LOG_BLOCK_HDR_SIZE;
	unsigned payload_size = log_sys.payload_size();
	ut_ad(frag_len < payload_size);
	lsn_t lsn_len = len;
	lsn_len += (lsn_len + frag_len) / payload_size
		* (OS_FILE_LOG_BLOCK_SIZE - payload_size);

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

	const ulint ptr_offset = ulint(ptr - recv_sys.buf);

	ib::info() << "Log record type " << type << ", page " << space << ":"
		<< page_no << ". Log parsing proceeded successfully up to "
		<< recv_sys.recovered_lsn << ". Previous log record type "
		<< recv_previous_parsed_rec_type << ", is multi "
		<< recv_previous_parsed_rec_is_multi << " Recv offset "
		<< ptr_offset << ", prev "
		<< recv_previous_parsed_rec_offset;

	ut_ad(ptr <= recv_sys.buf + recv_sys.len);

	const ulint	limit	= 100;
	const ulint	prev_offset = std::min(recv_previous_parsed_rec_offset,
					       ptr_offset);
	const ulint	before = std::min(prev_offset, limit);
	const ulint	after = std::min(recv_sys.len - ptr_offset, limit);

	ib::info() << "Hex dump starting " << before << " bytes before and"
		" ending " << after << " bytes after the corrupted record:";

	const byte* start = recv_sys.buf + prev_offset - before;

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

/** Report a MLOG_INDEX_LOAD operation.
@param[in]	space_id	tablespace id
@param[in]	page_no		page number
@param[in]	lsn		log sequence number */
ATTRIBUTE_COLD static void
recv_mlog_index_load(ulint space_id, ulint page_no, lsn_t lsn)
{
	recv_spaces_t::iterator it = recv_spaces.find(space_id);
	if (it != recv_spaces.end()) {
		it->second.mlog_index_load(lsn);
	}

	if (log_optimized_ddl_op) {
		log_optimized_ddl_op(space_id);
	}
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
	bool		single_rec;
	ulint		len;
	lsn_t		new_recovered_lsn;
	lsn_t		old_lsn;
	mlog_id_t	type;
	ulint		space;
	ulint		page_no;
	const byte*	body;

	ut_ad(log_mutex_own());
	ut_ad(mutex_own(&recv_sys.mutex));
	ut_ad(recv_sys.parse_start_lsn != 0);
loop:
	const byte* ptr = recv_sys.buf + recv_sys.recovered_offset;
	const byte* end_ptr = recv_sys.buf + recv_sys.len;

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

		old_lsn = recv_sys.recovered_lsn;

		/* Try to parse a log record, fetching its type, space id,
		page no, and a pointer to the body of the log record */

		len = recv_parse_log_rec(&type, ptr, end_ptr, &space,
					 &page_no, apply, &body);

		if (recv_sys.found_corrupt_log) {
			recv_report_corrupt_log(ptr, type, space, page_no);
			return(true);
		}

		if (recv_sys.found_corrupt_fs) {
			return(true);
		}

		if (len == 0) {
			return(false);
		}

		new_recovered_lsn = recv_calc_lsn_on_data_add(old_lsn, len);

		if (new_recovered_lsn > recv_sys.scanned_lsn) {
			/* The log record filled a log block, and we require
			that also the next log block should have been scanned
			in */

			return(false);
		}

		recv_previous_parsed_rec_type = type;
		recv_previous_parsed_rec_offset = recv_sys.recovered_offset;
		recv_previous_parsed_rec_is_multi = 0;

		recv_sys.recovered_offset += len;
		recv_sys.recovered_lsn = new_recovered_lsn;

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
					: recv_sys.mlog_checkpoint_lsn
					? "reread" : "read",
					recv_sys.recovered_lsn);
			}

			DBUG_PRINT("ib_log",
				   ("MLOG_CHECKPOINT(" LSN_PF ") %s at "
				    LSN_PF,
				    lsn,
				    lsn != checkpoint_lsn ? "ignored"
				    : recv_sys.mlog_checkpoint_lsn
				    ? "reread" : "read",
				    recv_sys.recovered_lsn));

			if (lsn == checkpoint_lsn) {
				if (recv_sys.mlog_checkpoint_lsn) {
					ut_ad(recv_sys.mlog_checkpoint_lsn
					      <= recv_sys.recovered_lsn);
					break;
				}
				recv_sys.mlog_checkpoint_lsn
					= recv_sys.recovered_lsn;
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
				recv_sys.add(
					type, page_id_t(space, page_no), body,
					ptr + len, old_lsn,
					recv_sys.recovered_lsn);
			}
			/* fall through */
		case MLOG_INDEX_LOAD:
			if (type == MLOG_INDEX_LOAD) {
				recv_mlog_index_load(space, page_no, old_lsn);
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

			if (recv_sys.found_corrupt_log) {
corrupted_log:
				recv_report_corrupt_log(
					ptr, type, space, page_no);
				return(true);
			}

			if (ptr == end_ptr) {
			} else if (type == MLOG_CHECKPOINT
				   || (*ptr & MLOG_SINGLE_REC_FLAG)) {
				recv_sys.found_corrupt_log = true;
				goto corrupted_log;
			}

			if (recv_sys.found_corrupt_fs) {
				return(true);
			}

			if (len == 0) {
				return(false);
			}

			recv_previous_parsed_rec_type = type;
			recv_previous_parsed_rec_offset
				= recv_sys.recovered_offset + total_len;
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
					recv_sys.recovered_lsn, len);
				mlog_rec_len += len;
				recv_sys.recovered_offset += len;
				recv_sys.recovered_lsn = new_recovered_lsn;
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
					    recv_sys.recovered_lsn,
					    total_len, n_recs));
				total_len -= mlog_rec_len;
				break;
			}

			DBUG_PRINT("ib_log",
				   ("scan " LSN_PF ": multi-log rec %s"
				    " len " ULINTPF
				    " page " ULINTPF ":" ULINTPF,
				    recv_sys.recovered_lsn,
				    get_mlog_string(type), len, space, page_no));
		}

		new_recovered_lsn = recv_calc_lsn_on_data_add(
			recv_sys.recovered_lsn, total_len);

		if (new_recovered_lsn > recv_sys.scanned_lsn) {
			/* The log record filled a log block, and we require
			that also the next log block should have been scanned
			in */

			return(false);
		}

		/* Add all the records to the hash table */

		ptr = recv_sys.buf + recv_sys.recovered_offset;

		for (;;) {
			old_lsn = recv_sys.recovered_lsn;
			/* This will apply MLOG_FILE_ records. We
			had to skip them in the first scan, because we
			did not know if the mini-transaction was
			completely recovered (until MLOG_MULTI_REC_END). */
			len = recv_parse_log_rec(
				&type, ptr, end_ptr, &space, &page_no,
				apply, &body);

			if (recv_sys.found_corrupt_log
			    && !recv_report_corrupt_log(
				    ptr, type, space, page_no)) {
				return(true);
			}

			if (recv_sys.found_corrupt_fs) {
				return(true);
			}

			ut_a(len != 0);
			ut_a(!(*ptr & MLOG_SINGLE_REC_FLAG));

			recv_sys.recovered_offset += len;
			recv_sys.recovered_lsn
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
				recv_mlog_index_load(space, page_no, old_lsn);
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
					recv_sys.add(
						type,
						page_id_t(space, page_no),
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
recv_sys.parse_start_lsn is non-zero.
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

	ut_ad(scanned_lsn >= recv_sys.scanned_lsn);

	if (!recv_sys.parse_start_lsn) {
		/* Cannot start parsing yet because no start point for
		it found */

		return(false);
	}

	data_len = log_block_get_data_len(log_block);

	if (recv_sys.parse_start_lsn >= scanned_lsn) {

		return(false);

	} else if (recv_sys.scanned_lsn >= scanned_lsn) {

		return(false);

	} else if (recv_sys.parse_start_lsn > recv_sys.scanned_lsn) {
		more_len = (ulint) (scanned_lsn - recv_sys.parse_start_lsn);
	} else {
		more_len = (ulint) (scanned_lsn - recv_sys.scanned_lsn);
	}

	if (more_len == 0) {

		return(false);
	}

	ut_ad(data_len >= more_len);

	start_offset = data_len - more_len;

	if (start_offset < LOG_BLOCK_HDR_SIZE) {
		start_offset = LOG_BLOCK_HDR_SIZE;
	}

	end_offset = std::min<ulint>(data_len, log_sys.trailer_offset());

	ut_ad(start_offset <= end_offset);

	if (start_offset < end_offset) {
		memcpy(recv_sys.buf + recv_sys.len,
		       log_block + start_offset, end_offset - start_offset);

		recv_sys.len += end_offset - start_offset;

		ut_a(recv_sys.len <= RECV_PARSING_BUF_SIZE);
	}

	return(true);
}

/** Moves the parsing buffer data left to the buffer start. */
void recv_sys_justify_left_parsing_buf()
{
	memmove(recv_sys.buf, recv_sys.buf + recv_sys.recovered_offset,
		recv_sys.len - recv_sys.recovered_offset);

	recv_sys.len -= recv_sys.recovered_offset;

	recv_sys.recovered_offset = 0;
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
	bool		apply		= recv_sys.mlog_checkpoint_lsn != 0;
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

		if (scanned_lsn + data_len > recv_sys.scanned_lsn
		    && log_block_get_checkpoint_no(log_block)
		    < recv_sys.scanned_checkpoint_no
		    && (recv_sys.scanned_checkpoint_no
			- log_block_get_checkpoint_no(log_block)
			> 0x80000000UL)) {

			/* Garbage from a log buffer flush which was made
			before the most recent database recovery */
			finished = true;
			break;
		}

		if (!recv_sys.parse_start_lsn
		    && (log_block_get_first_rec_group(log_block) > 0)) {

			/* We found a point from which to start the parsing
			of log records */

			recv_sys.parse_start_lsn = scanned_lsn
				+ log_block_get_first_rec_group(log_block);
			recv_sys.scanned_lsn = recv_sys.parse_start_lsn;
			recv_sys.recovered_lsn = recv_sys.parse_start_lsn;
		}

		scanned_lsn += data_len;

		if (data_len == LOG_BLOCK_HDR_SIZE + SIZE_OF_MLOG_CHECKPOINT
		    && scanned_lsn == checkpoint_lsn + SIZE_OF_MLOG_CHECKPOINT
		    && log_block[LOG_BLOCK_HDR_SIZE] == MLOG_CHECKPOINT
		    && checkpoint_lsn == mach_read_from_8(LOG_BLOCK_HDR_SIZE
							  + 1 + log_block)) {
			/* The redo log is logically empty. */
			ut_ad(recv_sys.mlog_checkpoint_lsn == 0
			      || recv_sys.mlog_checkpoint_lsn
			      == checkpoint_lsn);
			recv_sys.mlog_checkpoint_lsn = checkpoint_lsn;
			DBUG_PRINT("ib_log", ("found empty log; LSN=" LSN_PF,
					      scanned_lsn));
			finished = true;
			break;
		}

		if (scanned_lsn > recv_sys.scanned_lsn) {
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
					<< recv_sys.scanned_lsn;
			}

			/* We were able to find more log data: add it to the
			parsing buffer if parse_start_lsn is already
			non-zero */

			DBUG_EXECUTE_IF(
				"reduce_recv_parsing_buf",
				recv_parsing_buf_size
					= (70 * 1024);
				);

			if (recv_sys.len + 4 * OS_FILE_LOG_BLOCK_SIZE
			    >= recv_parsing_buf_size) {
				ib::error() << "Log parsing buffer overflow."
					" Recovery may have failed!";

				recv_sys.found_corrupt_log = true;

				if (!srv_force_recovery) {
					ib::error()
						<< "Set innodb_force_recovery"
						" to ignore this error.";
					return(true);
				}
			} else if (!recv_sys.found_corrupt_log) {
				more_data = recv_sys_add_to_parsing_buf(
					log_block, scanned_lsn);
			}

			recv_sys.scanned_lsn = scanned_lsn;
			recv_sys.scanned_checkpoint_no
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

	mutex_enter(&recv_sys.mutex);

	if (more_data && !recv_sys.found_corrupt_log) {
		/* Try to parse more log records */

		if (recv_parse_log_recs(checkpoint_lsn,
					*store_to_hash, apply)) {
			ut_ad(recv_sys.found_corrupt_log
			      || recv_sys.found_corrupt_fs
			      || recv_sys.mlog_checkpoint_lsn
			      == recv_sys.recovered_lsn);
			finished = true;
			goto func_exit;
		}

		if (*store_to_hash != STORE_NO
		    && mem_heap_get_size(recv_sys.heap) > available_memory) {

			DBUG_PRINT("ib_log", ("Ran out of memory and last "
					      "stored lsn " LSN_PF,
					      recv_sys.recovered_lsn));

			recv_sys.last_stored_lsn = recv_sys.recovered_lsn;
			*store_to_hash = STORE_NO;
		}

		if (recv_sys.recovered_offset > recv_parsing_buf_size / 4) {
			/* Move parsing buffer data to the buffer start */

			recv_sys_justify_left_parsing_buf();
		}
	}

func_exit:
	mutex_exit(&recv_sys.mutex);
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
	DBUG_ASSERT(!last_phase || recv_sys.mlog_checkpoint_lsn > 0);

	mutex_enter(&recv_sys.mutex);
	recv_sys.len = 0;
	recv_sys.recovered_offset = 0;
	recv_sys.clear();
	srv_start_lsn = *contiguous_lsn;
	recv_sys.parse_start_lsn = *contiguous_lsn;
	recv_sys.scanned_lsn = *contiguous_lsn;
	recv_sys.recovered_lsn = *contiguous_lsn;
	recv_sys.scanned_checkpoint_no = 0;
	recv_previous_parsed_rec_type = MLOG_SINGLE_REC_FLAG;
	recv_previous_parsed_rec_offset	= 0;
	recv_previous_parsed_rec_is_multi = 0;
	ut_ad(recv_max_page_lsn == 0);
	ut_ad(last_phase || !recv_writer_thread_active);
	mutex_exit(&recv_sys.mutex);

	lsn_t	start_lsn;
	lsn_t	end_lsn;
	store_t	store_to_hash	= recv_sys.mlog_checkpoint_lsn == 0
		? STORE_NO : (last_phase ? STORE_IF_EXISTS : STORE_YES);
	ulint	available_mem	= srv_page_size
		* (buf_pool_get_n_pages()
		   - (recv_n_pool_free_frames * srv_buf_pool_instances));

	log_sys.log.scanned_lsn = end_lsn = *contiguous_lsn =
		ut_uint64_align_down(*contiguous_lsn, OS_FILE_LOG_BLOCK_SIZE);

	do {
		if (last_phase && store_to_hash == STORE_NO) {
			store_to_hash = STORE_IF_EXISTS;
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

	if (recv_sys.found_corrupt_log || recv_sys.found_corrupt_fs) {
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
		if (i->second.name.find(TEMP_TABLE_PATH_PREFIX) != std::string::npos) {
			ib::warn() << "Tablespace " << i->first << " was not"
				" found at " << i->second.name << " when"
				" restoring a (partial?) backup. All redo log"
				" for this file will be ignored!";
		}
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

	for (recv_sys_t::map::iterator p = recv_sys.pages.begin();
	     p != recv_sys.pages.end();) {
		ut_ad(!p->second.log.empty());
		const ulint space = p->first.space();
		if (is_predefined_tablespace(space)) {
next:
			p++;
			continue;
		}

		recv_spaces_t::iterator i = recv_spaces.find(space);
		ut_ad(i != recv_spaces.end());

		switch (i->second.status) {
		case file_name_t::NORMAL:
			goto next;
		case file_name_t::MISSING:
			err = recv_init_missing_space(err, i);
			i->second.status = file_name_t::DELETED;
			/* fall through */
		case file_name_t::DELETED:
			recv_sys_t::map::iterator r = p++;
			recv_sys.pages.erase(r);
			continue;
		}
		ut_ad(0);
	}

	if (err != DB_SUCCESS) {
		return(err);
	}

	/* When rescan is not needed, recv_sys.pages will contain the
	entire redo log. If rescan is needed or innodb_force_recovery
	is set, we can ignore missing tablespaces. */
	for (const recv_spaces_t::value_type& rs : recv_spaces) {
		if (rs.second.status != file_name_t::MISSING) {
			continue;
		}

		missing_tablespace = true;

		if (srv_force_recovery > 0) {
			ib::warn() << "Tablespace " << rs.first
				<<" was not found at " << rs.second.name
				<<", and innodb_force_recovery was set."
				<<" All redo log for this tablespace"
				<<" will be ignored!";
			continue;
		}

		if (!rescan) {
			ib::info() << "Tablespace " << rs.first
				<< " was not found at '"
				<< rs.second.name << "', but there"
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

	for (recv_spaces_t::value_type& rs : recv_spaces) {
		ut_ad(!is_predefined_tablespace(rs.first));
		ut_ad(rs.second.status != file_name_t::DELETED
		      || !rs.second.space);

		if (rs.second.status == file_name_t::DELETED) {
			/* The tablespace was deleted,
			so we can ignore any redo log for it. */
			flag_deleted = true;
		} else if (rs.second.space != NULL) {
			/* The tablespace was found, and there
			are some redo log records for it. */
			fil_names_dirty(rs.second.space);
			rs.second.space->enable_lsn = rs.second.enable_lsn;
		} else if (rs.second.name == "") {
			ib::error() << "Missing MLOG_FILE_NAME"
				" or MLOG_FILE_DELETE"
				" before MLOG_CHECKPOINT for tablespace "
				<< rs.first;
			recv_sys.found_corrupt_log = true;
			return(DB_CORRUPTION);
		} else {
			rs.second.status = file_name_t::MISSING;
			flag_deleted = true;
		}

		ut_ad(rs.second.status == file_name_t::DELETED
		      || rs.second.name != "");
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
#ifdef UNIV_DEBUG
	for (ulint i= 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t* buf_pool = buf_pool_from_array(i);
		buf_flush_list_mutex_enter(buf_pool);
		ut_ad(UT_LIST_GET_LEN(buf_pool->LRU) == 0);
		ut_ad(UT_LIST_GET_LEN(buf_pool->unzip_LRU) == 0);
		buf_flush_list_mutex_exit(buf_pool);
	}
#endif

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

		srv_start_lsn = recv_sys.recovered_lsn = log_sys.lsn;
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
	recv_sys.mlog_checkpoint_lsn = 0;

	ut_ad(RECV_SCAN_SIZE <= srv_log_buffer_size);

	const lsn_t	end_lsn = mach_read_from_8(
		buf + LOG_CHECKPOINT_END_LSN);

	ut_ad(recv_sys.pages.empty());
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
		recv_sys.found_corrupt_log = true;
		log_mutex_exit();
		return(DB_ERROR);
	}

	/* Look for MLOG_CHECKPOINT. */
	recv_group_scan_log_recs(checkpoint_lsn, &contiguous_lsn, false);
	/* The first scan should not have stored or applied any records. */
	ut_ad(recv_sys.pages.empty());
	ut_ad(!recv_sys.found_corrupt_fs);

	if (srv_read_only_mode && recv_needed_recovery) {
		log_mutex_exit();
		return(DB_READ_ONLY);
	}

	if (recv_sys.found_corrupt_log && !srv_force_recovery) {
		log_mutex_exit();
		ib::warn() << "Log scan aborted at LSN " << contiguous_lsn;
		return(DB_ERROR);
	}

	if (recv_sys.mlog_checkpoint_lsn == 0) {
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

		if ((recv_sys.found_corrupt_log && !srv_force_recovery)
		    || recv_sys.found_corrupt_fs) {
			log_mutex_exit();
			return(DB_ERROR);
		}
	}

	/* NOTE: we always do a 'recovery' at startup, but only if
	there is something wrong we will print a message to the
	user about recovery: */

	if (flush_lsn == checkpoint_lsn + SIZE_OF_MLOG_CHECKPOINT
	    && recv_sys.mlog_checkpoint_lsn == checkpoint_lsn) {
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

	log_sys.lsn = recv_sys.recovered_lsn;

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
					      recv_sys.last_stored_lsn));

			lsn_t recent_stored_lsn = recv_sys.last_stored_lsn;
			rescan = recv_group_scan_log_recs(
				checkpoint_lsn, &recent_stored_lsn, false);

			ut_ad(!recv_sys.found_corrupt_fs);

			missing_tablespace = false;

			err = recv_sys.found_corrupt_log
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

			if ((recv_sys.found_corrupt_log
			     && !srv_force_recovery)
			    || recv_sys.found_corrupt_fs) {
				log_mutex_exit();
				return(DB_ERROR);
			}
		}
	} else {
		ut_ad(!rescan || recv_sys.pages.empty());
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

	if (recv_sys.recovered_lsn < checkpoint_lsn) {
		log_mutex_exit();

		ib::error() << "Recovered only to lsn:"
			    << recv_sys.recovered_lsn << " checkpoint_lsn: " << checkpoint_lsn;

		return(DB_ERROR);
	}

	log_sys.next_checkpoint_lsn = checkpoint_lsn;
	log_sys.next_checkpoint_no = checkpoint_no + 1;

	recv_synchronize_groups();

	if (!recv_needed_recovery) {
		ut_a(checkpoint_lsn == recv_sys.recovered_lsn);
	} else {
		srv_start_lsn = recv_sys.recovered_lsn;
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

	mutex_enter(&recv_sys.mutex);

	recv_sys.apply_log_recs = true;

	mutex_exit(&recv_sys.mutex);

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
	mutex_enter(&recv_sys.writer_mutex);

	/* Free the resources of the recovery system */
	recv_recovery_on = false;

	/* By acquring the mutex we ensure that the recv_writer thread
	won't trigger any more LRU batches. Now wait for currently
	in progress batches to finish. */
	buf_flush_wait_LRU_batch_end();

	mutex_exit(&recv_sys.writer_mutex);

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

	recv_sys.debug_free();

	/* Free up the flush_rbt. */
	buf_flush_free_flush_rbt();
	/* Enable innodb_sync_debug checks */
	ut_d(sync_check_enable());
}

/** Find a doublewrite copy of a page.
@param[in]	space_id	tablespace identifier
@param[in]	page_no		page number
@return	page frame
@retval NULL if no page was found */
const byte*
recv_dblwr_t::find_page(ulint space_id, ulint page_no)
{
  const byte *result= NULL;
  lsn_t max_lsn= 0;

  for (const byte *page : pages)
  {
    if (page_get_page_no(page) != page_no ||
        page_get_space_id(page) != space_id)
      continue;
    const lsn_t lsn= mach_read_from_8(page + FIL_PAGE_LSN);
    if (lsn <= max_lsn)
      continue;
    max_lsn= lsn;
    result= page;
  }

  return result;
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

	case MLOG_MEMSET:
		return("MLOG_MEMSET");

	case MLOG_INIT_FREE_PAGE:
		return("MLOG_INIT_FREE_PAGE");

	case MLOG_FILE_WRITE_CRYPT_DATA:
		return("MLOG_FILE_WRITE_CRYPT_DATA");
	}
	DBUG_ASSERT(0);
	return(NULL);
}
#endif /* !DBUG_OFF */
