/*****************************************************************************

Copyright (c) 2024, Kristian Nielsen

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
@file fsp/fsp_binlog.cc
InnoDB implementation of binlog.
*******************************************************/

#include "ut0bitop.h"
#include "fsp0fsp.h"
#include "buf0flu.h"
#include "trx0trx.h"
#include "fsp_binlog.h"
#include "innodb_binlog.h"

#include "rpl_gtid_base.h"
#include "log.h"


static buf_block_t *binlog_cur_block;

/*
  How often (in terms of bytes written) to dump a (differential) binlog state
  at the start of the page, to speed up finding the initial GTID position for
  a connecting slave.

  This value must be used over the setting innodb_binlog_state_interval,
  because after a restart the latest binlog file will be using the value of the
  setting prior to the restart; the new value of the setting (if different)
  will be used for newly created binlog files.
*/
uint64_t current_binlog_state_interval;

/*
  Mutex protecting active_binlog_file_no and active_binlog_space.
*/
mysql_mutex_t active_binlog_mutex;
pthread_cond_t active_binlog_cond;

/* The currently being written binlog tablespace. */
std::atomic<uint64_t> active_binlog_file_no;
fil_space_t* active_binlog_space;

/*
  The first binlog tablespace that is still open.
  This can be equal to active_binlog_file_no, if the tablespace prior to the
  active one has been fully flushed out to disk and closed.
  Or it can be one less, if the prior tablespace is still being written out and
  closed.
*/
uint64_t first_open_binlog_file_no;

/*
  The most recent created and open tablespace.
  This can be equal to active_binlog_file_no+1, if the next tablespace to be
  used has already been pre-allocated and opened.
  Or it can be the same as active_binlog_file_no, if the pre-allocation of the
  next tablespace is still pending.
*/
uint64_t last_created_binlog_file_no;
fil_space_t *last_created_binlog_space;

/*
  Point at which it is guaranteed that all data has been written out to the
  binlog file (on the OS level; not necessarily fsync()'ed yet).

  Stores the most recent two values, each corresponding to active_binlog_file_no&1.
*/
/* ToDo: maintain this offset value as up to where data has been written out to the OS. Needs to be binary-searched in current binlog file at server restart; which is also a reason why it might not be a multiple of the page size. */
std::atomic<uint64_t> binlog_cur_written_offset[2];
/*
  Offset of last valid byte of data in most recent 2 binlog files.
  A value of ~0 means that file is not opened as a tablespace (and data is
  valid until the end of the file).
*/
std::atomic<uint64_t> binlog_cur_end_offset[2];


/*
  Initialize the InnoDB implementation of binlog.
  Note that we do not create or open any binlog tablespaces here.
  This is only done if InnoDB binlog is enabled on the server level.
*/
void
fsp_binlog_init()
{
  mysql_mutex_init(fsp_active_binlog_mutex_key, &active_binlog_mutex, nullptr);
  pthread_cond_init(&active_binlog_cond, nullptr);
}


void
fsp_binlog_shutdown()
{
  pthread_cond_destroy(&active_binlog_cond);
  mysql_mutex_destroy(&active_binlog_mutex);
}


/** Write out all pages, flush, and close/detach a binlog tablespace.
@param[in] file_no	 Index of the binlog tablespace
@return DB_SUCCESS or error code */
dberr_t
fsp_binlog_tablespace_close(uint64_t file_no)
{
  mtr_t mtr;
  dberr_t res;

  uint32_t space_id= SRV_SPACE_ID_BINLOG0 + (file_no & 1);
  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_get_by_id(space_id);
  mysql_mutex_unlock(&fil_system.mutex);
  if (!space) {
    res= DB_ERROR;
    goto end;
  }

  /*
    Write out any remaining pages in the buffer pool to the binlog tablespace.
    Then flush the file to disk, and close the old tablespace.

    ToDo: Will this turn into a busy-wait if some of the pages are still latched
    in this tablespace, maybe because even though the tablespace has been
    written full, the mtr that's ending in the next tablespace may still be
    active? This will need fixing, no busy-wait should be done here.
  */

  /*
    Take and release an exclusive latch on the last page in the tablespace to
    be closed. We might be signalled that the tablespace is done while the mtr
    completing the tablespace write is still active; the exclusive latch will
    ensure we wait for any last mtr to commit before we close the tablespace.
  */
  mtr.start();
  buf_page_get_gen(page_id_t{space_id, space->size - 1}, 0, RW_X_LATCH, nullptr,
                   BUF_GET, &mtr, &res);
  mtr.commit();

  while (buf_flush_list_space(space))
    ;
  // ToDo: Also, buf_flush_list_space() seems to use io_capacity, but that's not appropriate here perhaps
  os_aio_wait_until_no_pending_writes(false);
  space->flush<false>();
  fil_space_free(space_id, false);
  res= DB_SUCCESS;
end:
  return res;
}


/*
  Open an existing tablespace. The filehandle fh is taken over by the tablespace
  (or closed in case of error).
*/
fil_space_t *
fsp_binlog_open(const char *file_name, pfs_os_file_t fh,
                uint64_t file_no, size_t file_size, bool open_empty)
{
  const uint32_t page_size= (uint32_t)srv_page_size;
  const uint32_t page_size_shift= srv_page_size_shift;

  os_offset_t binlog_size= innodb_binlog_size_in_pages << srv_page_size_shift;
  if (open_empty && file_size < binlog_size) {
    /*
      A crash may have left a partially pre-allocated file. If so, extend it
      to the required size.
      Note that this may also extend a previously pre-allocated file to the new
      binlog configured size, if the configuration changed during server
      restart.
    */
    if (!os_file_set_size(file_name, fh, binlog_size, false)) {
      sql_print_warning("Failed to change the size of InnoDB binlog file '%s' "
                        "from %zu to %zu bytes (error code: %d)", file_name,
                        file_size, (size_t)binlog_size, errno);
    } else {
      file_size= (size_t)binlog_size;
    }
  }
  if (file_size < 2*page_size)
  {
    sql_print_warning("InnoDB binlog file number %llu is too short (%zu bytes), "
                      "should be at least %u bytes",
                      file_no, file_size, 2*page_size);
    os_file_close(fh);
    return nullptr;
  }

  uint32_t space_id= SRV_SPACE_ID_BINLOG0 + (file_no & 1);

  if (!open_empty) {
    page_t *page_buf= static_cast<byte*>(aligned_malloc(page_size, page_size));
    if (!page_buf) {
      os_file_close(fh);
      return nullptr;
    }

    dberr_t err= os_file_read(IORequestRead, fh, page_buf, 0, page_size, nullptr);
    if (err != DB_SUCCESS) {
      sql_print_warning("Unable to read first page of file '%s'", file_name);
      aligned_free(page_buf);
      os_file_close(fh);
      return nullptr;
    }

    /* ToDo: Maybe use leaner page format for binlog tablespace? */
    uint32_t id1= mach_read_from_4(FIL_PAGE_SPACE_ID + page_buf);
    if (id1 != space_id) {
      sql_print_warning("Binlog file %s has inconsistent tablespace id %u "
                        "(expected %u)", file_name, id1, space_id);
      aligned_free(page_buf);
      os_file_close(fh);
      return nullptr;
    }
    // ToDo: should we here check buf_page_is_corrupted() ?

    aligned_free(page_buf);
  }

  uint32_t fsp_flags=
    FSP_FLAGS_FCRC32_MASK_MARKER | FSP_FLAGS_FCRC32_PAGE_SSIZE();
  /* ToDo: Enryption. */
  fil_encryption_t mode= FIL_ENCRYPTION_OFF;
  fil_space_crypt_t* crypt_data= nullptr;
  fil_space_t *space;

  mysql_mutex_lock(&fil_system.mutex);
  if (!(space= fil_space_t::create(space_id, fsp_flags, false, crypt_data,
                                   mode, true))) {
    mysql_mutex_unlock(&fil_system.mutex);
    os_file_close(fh);
    return nullptr;
  }

  space->add(file_name, fh, (uint32_t)(file_size >> page_size_shift),
             false, true);

  first_open_binlog_file_no= file_no;
  if (last_created_binlog_file_no == ~(uint64_t)0 ||
      file_no > last_created_binlog_file_no) {
    last_created_binlog_file_no= file_no;
    last_created_binlog_space= space;
  }

  mysql_mutex_unlock(&fil_system.mutex);
  return space;
}


/** Create a binlog tablespace file
@param[in]  file_no	 Index of the binlog tablespace
@param[out] new_space	 The newly created tablespace
@return DB_SUCCESS or error code */
dberr_t fsp_binlog_tablespace_create(uint64_t file_no, fil_space_t **new_space)
{
	pfs_os_file_t	fh;
	bool		ret;

        *new_space= nullptr;
	uint32_t size= innodb_binlog_size_in_pages;
	if(srv_read_only_mode)
		return DB_ERROR;

        char name[OS_FILE_MAX_PATH];
        binlog_name_make(name, file_no);

	os_file_create_subdirs_if_needed(name);

	/* ToDo: Do we need here an mtr.log_file_op(FILE_CREATE) like in fil_ibd_create(()? */
	fh = os_file_create(
		innodb_data_file_key, name,
		OS_FILE_CREATE, OS_DATA_FILE, srv_read_only_mode, &ret);

	if (!ret) {
		os_file_close(fh);
		return DB_ERROR;
	}

	/* ToDo: Enryption? */
	fil_encryption_t mode= FIL_ENCRYPTION_OFF;
	fil_space_crypt_t* crypt_data= nullptr;

	/* We created the binlog file and now write it full of zeros */
	if (!os_file_set_size(name, fh,
			      os_offset_t{size} << srv_page_size_shift)) {
		sql_print_error("Unable to allocate file %s", name);
		os_file_close(fh);
		os_file_delete(innodb_data_file_key, name);
		return DB_ERROR;
	}

	mysql_mutex_lock(&fil_system.mutex);
        /* ToDo: Need to ensure file (N-2) is no longer active before creating (N). */
	uint32_t space_id= SRV_SPACE_ID_BINLOG0 + (file_no & 1);
	if (!(*new_space= fil_space_t::create(space_id,
                                                ( FSP_FLAGS_FCRC32_MASK_MARKER |
						  FSP_FLAGS_FCRC32_PAGE_SSIZE()),
						false, crypt_data,
						mode, true))) {
		mysql_mutex_unlock(&fil_system.mutex);
		os_file_close(fh);
		os_file_delete(innodb_data_file_key, name);
		return DB_ERROR;
	}

	fil_node_t* node = (*new_space)->add(name, fh, size, false, true);
	node->find_metadata();
	mysql_mutex_unlock(&fil_system.mutex);

	return DB_SUCCESS;
}


/*
  Write out a binlog record.
  Split into chucks that each fit on a page.
  The data for the record is provided by a class derived from chunk_data_base.

  As a special case, a record write of type FSP_BINLOG_TYPE_FILLER does not
  write any record, but moves to the next tablespace and writes the initial
  GTID state record, used for FLUSH BINARY LOGS.
*/
std::pair<uint64_t, uint64_t>
fsp_binlog_write_rec(chunk_data_base *chunk_data, mtr_t *mtr, byte chunk_type)
{
  uint32_t page_size= (uint32_t)srv_page_size;
  uint32_t page_size_shift= srv_page_size_shift;
  fil_space_t *space= active_binlog_space;
  const uint32_t page_end= page_size - FIL_PAGE_DATA_END;
  uint32_t page_no= binlog_cur_page_no;
  uint32_t page_offset= binlog_cur_page_offset;
  /* ToDo: What is the lifetime of what's pointed to by binlog_cur_block, is there some locking needed around it or something? */
  buf_block_t *block= binlog_cur_block;
  uint64_t file_no= active_binlog_file_no.load(std::memory_order_relaxed);
  uint64_t pending_prev_end_offset= 0;
  uint64_t start_file_no= 0;
  uint64_t start_offset= 0;

  /*
    Write out the event data in chunks of whatever size will fit in the current
    page, until all data has been written.
  */
  byte cont_flag= 0;
  for (;;) {
    if (page_offset == FIL_PAGE_DATA) {
      if (UNIV_UNLIKELY(page_no >= space->size)) {
        /*
          Signal to the pre-allocation thread that this tablespace has been
          written full, so that it can be closed and a new one pre-allocated
          in its place. Then wait for a new tablespace to be pre-allocated that
          we can use.

          The normal case is that the next tablespace is already pre-allocated
          and available; binlog tablespace N is active while (N+1) is being
          pre-allocated. Only under extreme I/O pressure should be need to
          stall here.

          ToDo: Handle recovery. Idea: write the current LSN at the start of
          the binlog tablespace when we create it. At recovery, we should open
          the (at most) 2 most recent binlog tablespaces. Whenever we have a
          redo record, skip it if its LSN is smaller than the one stored in the
          tablespace corresponding to its space_id. This way, it should be safe
          to re-use tablespace ids between just two, SRV_SPACE_ID_BINLOG0 and
          SRV_SPACE_ID_BINLOG1.
        */
        pending_prev_end_offset= page_no << page_size_shift;
        mysql_mutex_lock(&active_binlog_mutex);
        /* ToDo: Make this wait killable?. */
        /* ToDo2: Handle not stalling infinitely if the new tablespace cannot be created due to eg. I/O error. Or should we in this case loop and repeatedly retry the create? */
        while (last_created_binlog_file_no <= file_no) {
          my_cond_wait(&active_binlog_cond, &active_binlog_mutex.m_mutex);
        }

        // ToDo: assert that a single write doesn't span more than two binlog files.
        ++file_no;
        binlog_cur_written_offset[file_no & 1].store(0, std::memory_order_relaxed);
        binlog_cur_end_offset[file_no & 1].store(0, std::memory_order_relaxed);
        active_binlog_file_no.store(file_no, std::memory_order_release);
        active_binlog_space= space= last_created_binlog_space;
        pthread_cond_signal(&active_binlog_cond);
        mysql_mutex_unlock(&active_binlog_mutex);
        binlog_cur_page_no= page_no= 0;
        /* ToDo: Here we must use the value from the file, if this file was pre-allocated before a server restart where the value of innodb_binlog_state_interval changed. Maybe just make innodb_binlog_state_interval dynamic and make the prealloc thread (and discover code at startup) supply the correct value to use for each file. */
        current_binlog_state_interval= innodb_binlog_state_interval;
      }

      /* Must be a power of two and larger than page size. */
      ut_ad(current_binlog_state_interval == 0 ||
            current_binlog_state_interval > page_size);
      ut_ad(current_binlog_state_interval == 0 ||
            current_binlog_state_interval ==
            (uint64_t)1 << (63 - nlz(current_binlog_state_interval)));

      if (0 == (page_no &
                ((current_binlog_state_interval >> page_size_shift) - 1))) {
        if (page_no == 0) {
          rpl_binlog_state_base full_state;
          bool err;
          full_state.init();
          err= load_global_binlog_state(&full_state);
          ut_a(!err /* ToDo error handling */);
          if (UNIV_UNLIKELY(file_no == 0 && page_no == 0) &&
              (full_state.count_nolock() == 1))
          {
            /*
              The gtid state written here includes the GTID for the event group
              currently being written. This is precise when the event group
              data begins before this point. If the event group happens to
              start exactly on a binlog file boundary, it just means we will
              have to read slightly more binlog data to find the starting point
              of that GTID.

              But there is an annoying case if this is the very first binlog
              file created (no migration from legacy binlog). If we start the
              binlog with some GTID 0-1-1 and write the state "0-1-1" at the
              start of the first file, then we will be unable to start
              replicating from the GTID position "0-1-1", corresponding to the
              *second* event group in the binlog. Because there will be no
              slightly earlier point to start reading from!

              So we put a slightly awkward special case here to handle that: If
              at the start of the first file we have a singleton gtid state
              with seq_no=1, D-S-1, then it must be the very first GTID in the
              entire binlog, so we write an *empty* gtid state that will always
              allow to start replicating from the very start of the binlog.

              (If the user would explicitly set the seq_no of the very first
              GTID in the binlog greater than 1, then starting from that GTID
              position will still not be possible).
            */
            rpl_gtid singleton_gtid;
            full_state.get_gtid_list_nolock(&singleton_gtid, 1);
            if (singleton_gtid.seq_no == 1)
            {
              full_state.reset_nolock();
            }
          }
          err= binlog_gtid_state(&full_state, mtr, block, page_no,
                                 page_offset, space);
          ut_a(!err /* ToDo error handling */);
          ut_ad(block);
          full_state.free();
          binlog_diff_state.reset_nolock();
        } else {
          bool err= binlog_gtid_state(&binlog_diff_state, mtr, block, page_no,
                                      page_offset, space);
          ut_a(!err /* ToDo error handling */);
        }
      } else
        block= fsp_page_create(space, page_no, mtr);
    } else {
      dberr_t err;
      /* ToDo: Is RW_SX_LATCH appropriate here? */
      block= buf_page_get_gen(page_id_t{space->id, page_no},
                              0, RW_SX_LATCH, block,
                              BUF_GET, mtr, &err);
      ut_a(err == DB_SUCCESS);
    }

    ut_ad(page_offset < page_end);
    uint32_t page_remain= page_end - page_offset;
    byte *ptr= page_offset + block->page.frame;
    /* ToDo: Do this check at the end instead, to save one buf_page_get_gen()? */
    if (page_remain < 4) {
      /* Pad the remaining few bytes, and move to next page. */
      mtr->memset(block, page_offset, page_remain, FSP_BINLOG_TYPE_FILLER);
      block= nullptr;
      ++page_no;
      page_offset= FIL_PAGE_DATA;
      continue;
    }

    if (UNIV_UNLIKELY(chunk_type == FSP_BINLOG_TYPE_FILLER))
    {
      /*
        Used for FLUSH BINARY LOGS, to move to the next tablespace and write
        the initial GTID state record without writing any actual event data.
      */
      break;
    }

    if (start_offset == 0)
    {
      start_file_no= file_no;
      start_offset= (page_no << page_size_shift) + page_offset;
    }
    page_remain-= 3;    /* Type byte and 2-byte length. */
    std::pair<uint32_t, bool> size_last=
      chunk_data->copy_data(ptr+3, page_remain);
    uint32_t size= size_last.first;
    ut_ad(size_last.second || size == page_remain);
    ut_ad(size <= page_remain);
    page_remain-= size;
    byte last_flag= size_last.second ? FSP_BINLOG_FLAG_LAST : 0;
    ptr[0]= chunk_type | cont_flag | last_flag;
    ptr[1]= size & 0xff;
    ptr[2]= (byte)(size >> 8);
    ut_ad(size <= 0xffff);

    mtr->memcpy(*block, page_offset, size+3);
    cont_flag= FSP_BINLOG_FLAG_CONT;
    if (page_remain == 0) {
      block= nullptr;
      page_offset= FIL_PAGE_DATA;
      ++page_no;
    } else {
      page_offset+= size+3;
    }
    if (size_last.second)
      break;
  }
  binlog_cur_block= block;
  binlog_cur_page_no= page_no;
  binlog_cur_page_offset= page_offset;
  if (UNIV_UNLIKELY(pending_prev_end_offset != 0))
    binlog_cur_end_offset[(file_no-1) & 1].store(pending_prev_end_offset,
                                                 std::memory_order_relaxed);
  binlog_cur_end_offset[file_no & 1].store((page_no << page_size_shift) + page_offset,
                                           std::memory_order_relaxed);
  return {start_file_no, start_offset};
}


/*
  Empty chunk data, used to pass a dummy record to fsp_binlog_write_rec()
  in fsp_binlog_flush().
*/
struct chunk_data_flush : public chunk_data_base {
  ~chunk_data_flush() { }

  virtual std::pair<uint32_t, bool> copy_data(byte *p, uint32_t max_len) final
  {
    memset(p, 0xff, max_len);
    return {max_len, true};
  }
};


/*
  Implementation of FLUSH BINARY LOGS.
  Truncate the current binlog tablespace, fill up the last page with dummy data
  (if needed), write the current GTID state to the first page in the next
  tablespace file (for DELETE_DOMAIN_ID).

  Relies on the server layer to prevent other binlog writes in parallel during
  the operation.
*/
bool
fsp_binlog_flush()
{
  uint64_t file_no= active_binlog_file_no.load(std::memory_order_relaxed);
  uint32_t space_id= SRV_SPACE_ID_BINLOG0 + (file_no & 1);
  uint32_t page_no= binlog_cur_page_no;
  fil_space_t *space= active_binlog_space;
  chunk_data_flush dummy_data;
  mtr_t mtr;

  mtr.start();
  mtr.x_lock_space(space);
  /*
    ToDo: Here, if we are already at precisely the end of a page, we need not
    fill up that page with a dummy record, we can just truncate the tablespace
    to that point. But then we need to handle an assertion m_modifications!=0
    in mtr_t::commit_shrink().
  */
  fsp_binlog_write_rec(&dummy_data, &mtr, FSP_BINLOG_TYPE_DUMMY);
  if (page_no + 1 < space->size)
  {
    mtr.trim_pages(page_id_t(space_id, page_no + 1));
    mtr.commit_shrink(*space, page_no + 1);
  }
  else
    mtr.commit();

  /* Flush out all pages in the (now filled-up) tablespace. */
  while (buf_flush_list_space(space))
    ;

  /*
    Now get a new GTID state record written to the next binlog tablespace.
    This ensures that the new state (in case of DELETE_DOMAIN_ID) will be
    persisted across a server restart.
  */
  mtr.start();
  fsp_binlog_write_rec(&dummy_data, &mtr, FSP_BINLOG_TYPE_FILLER);
  mtr.commit();

  return false;
}


binlog_chunk_reader::binlog_chunk_reader()
  : s { 0, 0,0, 0, 0, FSP_BINLOG_TYPE_FILLER, false, false },
    page_ptr(0), cur_block(0), page_buffer(nullptr),
    cur_file_handle((File)-1), skipping_partial(false)
{
  /* Nothing else. */
}


binlog_chunk_reader::~binlog_chunk_reader()
{
  release();
  if (cur_file_handle >= (File)0)
    my_close(cur_file_handle, MYF(0));
}


int
binlog_chunk_reader::read_error_corruption(uint64_t file_no, uint64_t page_no,
                                           const char *msg)
{
  sql_print_error("Corrupt binlog found on page %llu in binlog number %llu: "
                  "%s", page_no, file_no, msg);
  return -1;
}


/*
  Obtain the data on the page currently pointed to by the chunk reader. The
  page is either latched in the buffer pool (starting an mtr), or read from
  the file into the page buffer.

  The code does a dirty read of active_binlog_file_no to determine if the page
  is known to be available to read from the file, or if it should be looked up
  in the buffer pool. After making the decision, another dirty read is done to
  protect against the race where the active tablespace changes in the middle,
  and if so the operation is re-tried. This is necessary since the binlog files
  N and N-2 use the same tablespace id, so we must ensure we do not mistake a
  page from N as belonging to N-2.
*/
enum binlog_chunk_reader::chunk_reader_status
binlog_chunk_reader::fetch_current_page()
{
  ut_ad(!cur_block /* Must have no active mtr */);
  uint64_t active2= active_binlog_file_no.load(std::memory_order_acquire);
  for (;;) {
    buf_block_t *block= nullptr;
    bool mtr_started= false;
    uint64_t offset= (s.page_no << srv_page_size_shift) | s.in_page_offset;
    uint64_t active= active2;
    uint64_t end_offset=
      binlog_cur_end_offset[s.file_no&1].load(std::memory_order_acquire);
    ut_ad(s.file_no <= active);

    if (s.file_no + 1 >= active) {
      /* Check if we should read from the buffer pool or from the file. */
      if (end_offset != ~(uint64_t)0 && offset < end_offset) {
        mtr.start();
        mtr_started= true;
        /*
          ToDo: Should we keep track of the last block read and use it as a
          hint? Will be mainly useful when reading the partially written active
          page at the current end of the active binlog, which might be a common
          case.
        */
        buf_block_t *hint_block= nullptr;
        uint32_t space_id= SRV_SPACE_ID_BINLOG0 + (s.file_no & 1);
        dberr_t err= DB_SUCCESS;
        block= buf_page_get_gen(page_id_t{space_id, s.page_no}, 0,
                                RW_S_LATCH, hint_block, BUF_GET_IF_IN_POOL,
                                &mtr, &err);
        if (err != DB_SUCCESS) {
          mtr.commit();
          return CHUNK_READER_ERROR;
        }
      }
      active2= active_binlog_file_no.load(std::memory_order_acquire);
      if (UNIV_UNLIKELY(active2 != active)) {
        /*
          The active binlog file changed while we were processing; we might
          have gotten invalid end_offset or a buffer pool page from a wrong
          tablespace. So just try again.
        */
        if (mtr_started)
          mtr.commit();
        continue;
      }
      cur_end_offset= end_offset;
      if (offset >= end_offset) {
        ut_ad(!mtr_started);
        if (s.file_no == active) {
          /* Reached end of the currently active binlog file -> EOF. */
          return CHUNK_READER_EOF;
        }
      }
      if (block) {
        cur_block= block;
        ut_ad(mtr_started);
        page_ptr= block->page.frame;
        return CHUNK_READER_FOUND;
      } else {
        /* Not in buffer pool, just read it from the file. */
        if (mtr_started)
          mtr.commit();
        /* Fall through to read from file. */
      }
    }

    /* Tablespace is not open, just read from the file. */
    if (cur_file_handle < (File)0)
    {
      char filename[OS_FILE_MAX_PATH];
      MY_STAT stat_buf;

      binlog_name_make(filename, s.file_no);
      cur_file_handle= my_open(filename, O_RDONLY | O_BINARY, MYF(MY_WME));
      if (UNIV_UNLIKELY(cur_file_handle < (File)0)) {
        cur_file_handle= (File)-1;
        cur_file_length= ~(uint64_t)0;
        return CHUNK_READER_ERROR;
      }
      if (my_fstat(cur_file_handle, &stat_buf, MYF(0))) {
        my_error(ER_CANT_GET_STAT, MYF(0), filename, errno);
        my_close(cur_file_handle, MYF(0));
        cur_file_handle= (File)-1;
        cur_file_length= ~(uint64_t)0;
        return CHUNK_READER_ERROR;
      }
      cur_file_length= stat_buf.st_size;
    }
    if (s.file_no == active)
      cur_end_offset= end_offset;
    else
      cur_end_offset= cur_file_length;

    if (offset >= cur_file_length) {
      /* End of this file, move to the next one. */
      /*
        ToDo: Should also obey binlog_cur_written_offset[], once we start
        actually maintaining that, to save unnecessary buffer pool
        lookup.
      */
  goto_next_file:
      if (cur_file_handle >= (File)0)
      {
        my_close(cur_file_handle, MYF(0));
        cur_file_handle= (File)-1;
        cur_file_length= ~(uint64_t)0;
      }
      ++s.file_no;
      s.page_no= 0;
      continue;
    }

    size_t res= my_pread(cur_file_handle, page_buffer, srv_page_size,
                         s.page_no << srv_page_size_shift, MYF(MY_WME));
    if (res == (size_t)-1)
      return CHUNK_READER_ERROR;
    if (res == 0 && my_errno == HA_ERR_FILE_TOO_SHORT)
      goto goto_next_file;
    page_ptr= page_buffer;
    return CHUNK_READER_FOUND;
  }
  /* NOTREACHED */
}


int
binlog_chunk_reader::read_data(byte *buffer, int max_len, bool multipage)
{
  uint32_t size;
  int sofar= 0;

read_more_data:
  if (max_len == 0)
    return sofar;

  if (!page_ptr)
  {
    enum chunk_reader_status res= fetch_current_page();
    if (res == CHUNK_READER_EOF)
    {
      if (s.in_record)
        return read_error_corruption(s.file_no, s.page_no, "binlog tablespace "
                                     "truncated in the middle of record");
      else
        return 0;
    }
    else if (res == CHUNK_READER_ERROR)
      return -1;
  }

  if (s.chunk_len == 0)
  {
    byte type;
    if (s.in_page_offset < FIL_PAGE_DATA)
      s.in_page_offset= FIL_PAGE_DATA;
    else if (s.in_page_offset >= srv_page_size - (FIL_PAGE_DATA_END + 3) ||
             page_ptr[s.in_page_offset] == FSP_BINLOG_TYPE_FILLER)
    {
      ut_ad(s.in_page_offset >= srv_page_size - FIL_PAGE_DATA_END ||
            page_ptr[s.in_page_offset] == FSP_BINLOG_TYPE_FILLER);
      goto go_next_page;
    }

    /* Check for end-of-file. */
    if (cur_end_offset == ~(uint64_t)0 ||
        (s.page_no << srv_page_size_shift) + s.in_page_offset >= cur_end_offset)
      return sofar;

    type= page_ptr[s.in_page_offset];
    if (type == 0)
    {
      ut_ad(0 /* Should have detected end-of-file on cur_end_offset. */);
      return 0;
    }

    /*
      Consistency check on the chunks. A record must consist in a sequence of
      chunks of the same type, all but the first must have the
      FSP_BINLOG_FLAG_BIT_CONT bit set, and the final one must have the
      FSP_BINLOG_FLAG_BIT_LAST bit set.
    */
    if (!s.in_record)
    {
      if (UNIV_UNLIKELY(type & FSP_BINLOG_FLAG_CONT) && !s.skip_current)
      {
        if (skipping_partial)
        {
          s.chunk_len= page_ptr[s.in_page_offset + 1] |
            ((uint32_t)page_ptr[s.in_page_offset + 2] << 8);
          s.skip_current= true;
          goto skip_chunk;
        }
        else
          return read_error_corruption(s.file_no, s.page_no, "Binlog record "
                                       "starts with continuation chunk");
      }
    }
    else
    {
      if ((type ^ s.chunk_type) & FSP_BINLOG_TYPE_MASK)
      {
        /*
          As a special case, we must allow a GTID state to appear in the
          middle of a record.
        */
        if (((uint64_t)1 << (type & FSP_BINLOG_TYPE_MASK)) &
            ALLOWED_NESTED_RECORDS)
        {
          s.chunk_len= page_ptr[s.in_page_offset + 1] |
            ((uint32_t)page_ptr[s.in_page_offset + 2] << 8);
          goto skip_chunk;
        }
        /* Chunk type changed in the middle. */
        return read_error_corruption(s.file_no, s.page_no, "Binlog record missing "
                                     "end chunk");
      }
      if (!(type & FSP_BINLOG_FLAG_CONT))
      {
        /* START chunk without END chunk. */
        return read_error_corruption(s.file_no, s.page_no, "Binlog record missing "
                                     "end chunk");
      }
    }

    s.skip_current= false;
    s.chunk_type= type;
    s.in_record= true;
    s.chunk_len= page_ptr[s.in_page_offset + 1] |
      ((uint32_t)page_ptr[s.in_page_offset + 2] << 8);
    s.chunk_read_offset= 0;
  }

  /* Now we have a chunk available to read data from. */
  ut_ad(s.chunk_read_offset < s.chunk_len);
  if (s.skip_current &&
      (s.chunk_read_offset > 0 || (s.chunk_type & FSP_BINLOG_FLAG_CONT)))
  {
    /*
      Skip initial continuation chunks.
      Used to be able to start reading potentially in the middle of a record,
      ie. at a GTID state point.
    */
    s.chunk_read_offset= s.chunk_len;
  }
  else
  {
    size= std::min((uint32_t)max_len, s.chunk_len - s.chunk_read_offset);
    memcpy(buffer, page_ptr + s.in_page_offset + 3 + s.chunk_read_offset, size);
    buffer+= size;
    s.chunk_read_offset+= size;
    max_len-= size;
    sofar+= size;
  }

  if (s.chunk_len > s.chunk_read_offset)
  {
    ut_ad(max_len == 0 /* otherwise would have read more */);
    return sofar;
  }

  /* We have read all of the chunk. Move to next chunk or end of the record. */
skip_chunk:
  s.in_page_offset+= 3 + s.chunk_len;
  s.chunk_len= 0;
  s.chunk_read_offset= 0;

  if (s.chunk_type & FSP_BINLOG_FLAG_LAST)
  {
    s.in_record= false;  /* End of record. */
    s.skip_current= false;
  }

  if (s.in_page_offset >= srv_page_size - (FIL_PAGE_DATA_END + 3))
  {
go_next_page:
    /* End of page reached, move to the next page. */
    ++s.page_no;
    page_ptr= nullptr;
    if (cur_block)
    {
      mtr.commit();
      cur_block= nullptr;
    }
    s.in_page_offset= 0;

    if (cur_file_handle >= (File)0 &&
        (s.page_no << srv_page_size_shift) >= cur_file_length)
    {
      /* Move to the next file. */
      /*
        ToDo: Should we have similar logic for moving to the next file when
        we reach the end of the last page using the buffer pool?

        Then we save re-fetching the page later. But then we need to keep
        track of the file length also when reading from the buffer pool.
      */
      my_close(cur_file_handle, MYF(0));
      cur_file_handle= (File)-1;
      cur_file_length= ~(uint64_t)0;
      ++s.file_no;
      s.page_no= 0;
    }
  }

  if (sofar > 0 && (!multipage || !s.in_record))
    return sofar;

  goto read_more_data;
}


void
binlog_chunk_reader::restore_pos(binlog_chunk_reader::saved_position *pos)
{
  if (page_ptr &&
      !(pos->file_no == s.file_no && pos->page_no == s.page_no))
  {
    /* Seek to a different page, release any current page. */
    if (cur_block)
    {
      mtr.commit();
      cur_block= nullptr;
    }
    page_ptr= nullptr;
  }
  if (cur_file_handle != (File)-1 && pos->file_no != s.file_no)
  {
    /* Seek to a different file than currently open, close it. */
    my_close(cur_file_handle, MYF(0));
    cur_file_handle= (File)-1;
    cur_file_length= ~(uint64_t)0;
  }
  s= *pos;
}


void
binlog_chunk_reader::seek(uint64_t file_no, uint64_t offset)
{
  saved_position pos {
    file_no, (uint32_t)(offset >> srv_page_size_shift),
    (uint32_t)(offset & (srv_page_size - 1)),
    0, 0, FSP_BINLOG_TYPE_FILLER, false, false };
  restore_pos(&pos);
}


void binlog_chunk_reader::release(bool release_file_page)
{
  if (cur_block)
  {
    mtr.commit();
    cur_block= nullptr;
    page_ptr= nullptr;
  }
  else if (release_file_page)
  {
    /*
      For when we reach EOF while reading from the file. We need to re-read
      the page from the file (or buffer pool) in this case on next read, as
      data might be added to the page.
    */
    page_ptr= nullptr;
  }
}


bool binlog_chunk_reader::data_available()
{
  if (!end_of_record())
    return true;
  uint64_t active= active_binlog_file_no.load(std::memory_order_acquire);
  if (active != s.file_no)
  {
    ut_ad(active > s.file_no);
    return true;
  }
  uint64_t end_offset=
    binlog_cur_end_offset[s.file_no&1].load(std::memory_order_acquire);
  uint64_t active2= active_binlog_file_no.load(std::memory_order_acquire);
  if (active2 != active)
    return true;  // Active moved while we were checking
  if (end_offset == ~(uint64_t)0)
    return false;  // Nothing in this binlog file yet
  uint64_t offset= (s.page_no << srv_page_size_shift) | s.in_page_offset;
  if (offset < end_offset)
    return true;

  ut_ad(s.file_no == active2);
  ut_ad(offset == end_offset);
  return false;
}
