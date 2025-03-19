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


/*
  How often (in terms of pages written) to dump a (differential) binlog state
  at the start of the page, to speed up finding the initial GTID position for
  a connecting slave.

  This value must be used over the setting innodb_binlog_state_interval,
  because after a restart the latest binlog file will be using the value of the
  setting prior to the restart; the new value of the setting (if different)
  will be used for newly created binlog files.
*/
uint32_t current_binlog_state_interval;

/*
  Mutex protecting active_binlog_file_no.
*/
mysql_mutex_t active_binlog_mutex;
pthread_cond_t active_binlog_cond;

/* The currently being written binlog tablespace. */
std::atomic<uint64_t> active_binlog_file_no;

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

fsp_binlog_page_fifo *binlog_page_fifo;


fsp_binlog_page_entry *
fsp_binlog_page_fifo::create_page(uint64_t file_no, uint32_t page_no)
{
  mysql_mutex_lock(&m_mutex);
  ut_ad(first_file_no != ~(uint64_t)0);
  ut_a(file_no == first_file_no || file_no == first_file_no + 1);

  page_list *pl= &fifos[file_no & 1];
  fsp_binlog_page_entry **next_ptr_ptr= &pl->first_page;
  uint32_t entry_page_no= pl->first_page_no;
  /* Can only add a page at the end of the list. */
  while (*next_ptr_ptr)
  {
    next_ptr_ptr= &((*next_ptr_ptr)->next);
    ++entry_page_no;
  }
  ut_a(page_no == entry_page_no);
  fsp_binlog_page_entry *e= (fsp_binlog_page_entry *)ut_malloc(sizeof(*e), mem_key_binlog);
  ut_a(e);
  e->next= nullptr;
  e->page_buf= static_cast<byte*>(aligned_malloc(srv_page_size, srv_page_size));
  ut_a(e->page_buf);
  memset(e->page_buf, 0, srv_page_size);
  e->file_no= file_no;
  e->page_no= page_no;
  e->last_page= (page_no + 1 == size_in_pages(file_no));
  e->latched= 1;
  e->complete= false;
  e->flushed_clean= false;
  *next_ptr_ptr= e;

  mysql_mutex_unlock(&m_mutex);
  return e;
}


fsp_binlog_page_entry *
fsp_binlog_page_fifo::get_page(uint64_t file_no, uint32_t page_no)
{
  fsp_binlog_page_entry *res= nullptr;
  page_list *pl;
  fsp_binlog_page_entry *p;
  uint32_t entry_page_no;

  mysql_mutex_lock(&m_mutex);
  ut_ad(first_file_no != ~(uint64_t)0);
  ut_a(file_no <= first_file_no + 1);
  if (file_no < first_file_no)
    goto end;
  pl= &fifos[file_no & 1];
  p= pl->first_page;
  entry_page_no= pl->first_page_no;
  if (!p || page_no < entry_page_no)
    goto end;
  while (p)
  {
    if (page_no == entry_page_no)
    {
      /* Found the page. */
      ut_ad(p->file_no == file_no);
      ut_ad(p->page_no == page_no);
      ++p->latched;
      res= p;
      break;
    }
    p= p->next;
    ++entry_page_no;
  }

end:
  mysql_mutex_unlock(&m_mutex);
  return res;
}


void
fsp_binlog_page_fifo::release_page(fsp_binlog_page_entry *page)
{
  mysql_mutex_lock(&m_mutex);
  ut_a(page->latched > 0);
  if (--page->latched == 0)
    pthread_cond_signal(&m_cond);
  mysql_mutex_unlock(&m_mutex);
}


/*
  Release a page that is part of an mtr, except that if this is the last page
  of a binlog tablespace, then delay release until mtr commit.

  This is used to make sure that a tablespace is not closed until any mtr that
  modified it has been committed and the modification redo logged. This way, a
  closed tablespace never needs recovery and at most the two most recent binlog
  tablespaces need to be considered during recovery.
*/
void
fsp_binlog_page_fifo::release_page_mtr(fsp_binlog_page_entry *page, mtr_t *mtr)
{
  if (!page->last_page)
    return release_page(page);

  fsp_binlog_page_entry *old_page= mtr->get_binlog_page();
  ut_ad(!old_page);
  if (UNIV_UNLIKELY(old_page != nullptr))
  {
    sql_print_error("InnoDB: Internal inconsistency with mini-transaction that "
                    "spans more than two binlog files. Recovery may be "
                    "affected until the next checkpoint.");
    release_page(old_page);
  }
  mtr->set_binlog_page(page);
}


/*
  Flush (write to disk) the first unflushed page in a file.
  Returns true when the last page has been flushed.

  Must be called with m_mutex held.

  If called with force=true, will flush even any final, incomplete page.
  Otherwise such page will not be written out. Any final, incomplete page
  is left in the FIFO in any case.
*/
bool
fsp_binlog_page_fifo::flush_one_page(uint64_t file_no, bool force)
{
  page_list *pl;
  fsp_binlog_page_entry *e;

  mysql_mutex_assert_owner(&m_mutex);
  /*
    Wait for the FIFO to be not flushing from another thread, and for the
    first page to not be latched.
  */
  for (;;)
  {
    /*
      Let's make page not present not an error, to allow races where someone else
      flushed the page ahead of us.
    */
    if (file_no < first_file_no)
      return true;
    ut_a(file_no <= first_file_no + 1);

    if (!flushing)
    {
      pl= &fifos[file_no & 1];
      e= pl->first_page;
      if (!e)
        return true;
      if (e->latched == 0)
        break;
    }
    my_cond_wait(&m_cond, &m_mutex.m_mutex);
  }
  flushing= true;
  uint32_t page_no= pl->first_page_no;
  mysql_mutex_unlock(&m_mutex);
  ut_ad(e->complete || !e->next);
  if (e->complete || (force && !e->flushed_clean))
  {
    File fh= get_fh(file_no);
    ut_a(pl->fh >= (File)0);
    size_t res= my_pwrite(fh, e->page_buf, srv_page_size,
                          (uint64_t)page_no << srv_page_size_shift,
                          MYF(MY_WME));
    ut_a(res == srv_page_size);
    e->flushed_clean= true;
  }
  mysql_mutex_lock(&m_mutex);
  /*
    We marked the FIFO as flushing, page could not have disappeared despite
    releasing the mutex during the I/O.
  */
  ut_ad(flushing);
  bool done= (e->next == nullptr);
  if (e->complete)
  {
    pl->first_page= e->next;
    pl->first_page_no= page_no + 1;
    aligned_free(e->page_buf);
    ut_free(e);
  }
  else
    done= true;  /* Cannot flush past final incomplete page. */

  flushing= false;
  pthread_cond_signal(&m_cond);
  return done;
}


void
fsp_binlog_page_fifo::flush_up_to(uint64_t file_no, uint32_t page_no)
{
  mysql_mutex_lock(&m_mutex);
  for (;;)
  {
    if (file_no < first_file_no ||
        (file_no == first_file_no && fifos[file_no & 1].first_page_no > page_no))
      break;
    /* Guard against simultaneous RESET MASTER. */
    if (file_no > first_file_no + 1)
      break;
    uint64_t file_no_to_flush= file_no;
    /* Flush the prior file to completion first. */
    if (file_no == first_file_no + 1 && fifos[(file_no - 1) & 1].first_page)
      file_no_to_flush= file_no - 1;
    bool done= flush_one_page(file_no_to_flush, true);
    if (done && file_no == file_no_to_flush)
      break;
  }
  mysql_mutex_unlock(&m_mutex);
}


void
fsp_binlog_page_fifo::do_fdatasync(uint64_t file_no)
{
  File fh;
  mysql_mutex_lock(&m_mutex);
  if (file_no < first_file_no)
    goto done;   /* Old files are already fully synced. */
  ut_a(file_no == first_file_no || file_no == first_file_no + 1);
  fh= fifos[file_no & 1].fh;
  if (fh != (File)-1)
  {
    while (flushing)
      my_cond_wait(&m_cond, &m_mutex.m_mutex);
    flushing= true;
    mysql_mutex_unlock(&m_mutex);
    int res= my_sync(fh, MYF(MY_WME));
    ut_a(!res);
    mysql_mutex_lock(&m_mutex);
    flushing= false;
    pthread_cond_signal(&m_cond);
  }
done:
  mysql_mutex_unlock(&m_mutex);
}


File
fsp_binlog_page_fifo::get_fh(uint64_t file_no)
{
  File fh= fifos[file_no & 1].fh;
  if (fh == (File)-1)
  {
    char filename[OS_FILE_MAX_PATH];
    binlog_name_make(filename, file_no);
    fifos[file_no & 1].fh= fh= my_open(filename, O_RDWR | O_BINARY, MYF(MY_WME));
  }
  return fh;
}

/*
  If init_page is not ~(uint32_t)0, then it is the page to continue writing
  when re-opening existing binlog at server startup.

  If in addition, partial_page is non-NULL, it is an (aligned) page buffer
  containing the partial data of page init_page.

  If init_page is set but partial_page is not, then init_page is the first,
  empty page in the tablespace to create and start writing to.
*/
void
fsp_binlog_page_fifo::create_tablespace(uint64_t file_no,
                                        uint32_t size_in_pages,
                                        uint32_t init_page,
                                        byte *partial_page)
{
  mysql_mutex_lock(&m_mutex);
  ut_ad(init_page == ~(uint32_t)0 ||
        first_file_no == ~(uint64_t)0 ||
        /* At server startup allow opening N empty and (N-1) partial. */
        (init_page != ~(uint32_t)0 && file_no + 1 == first_file_no &&
         !fifos[first_file_no & 1].first_page));
  ut_a(first_file_no == ~(uint64_t)0 ||
       file_no == first_file_no + 1 ||
       file_no == first_file_no + 2 ||
       (init_page != ~(uint32_t)0 && file_no + 1 == first_file_no &&
         !fifos[first_file_no & 1].first_page));
  if (first_file_no == ~(uint64_t)0)
  {
    first_file_no= file_no;
  }
  else if (UNIV_UNLIKELY(file_no + 1 == first_file_no))
    first_file_no= file_no;
  else if (file_no == first_file_no + 2)
  {
    /* All pages in (N-2) must be flushed before doing (N). */
    ut_a(!fifos[file_no & 1].first_page);
    if (fifos[file_no & 1].fh != (File)-1)
      my_close(fifos[file_no & 1].fh, MYF(0));
    first_file_no= file_no - 1;
  }

  if (init_page != ~(uint32_t)0)
  {
    if (partial_page)
    {
      fsp_binlog_page_entry *e=
        (fsp_binlog_page_entry *)ut_malloc(sizeof(*e), mem_key_binlog);
      ut_a(e);
      e->next= nullptr;
      e->page_buf=
        static_cast<byte*>(aligned_malloc(srv_page_size, srv_page_size));
      ut_a(e->page_buf);
      memcpy(e->page_buf, partial_page, srv_page_size);
      e->file_no= file_no;
      e->page_no= init_page;
      e->last_page= (init_page + 1 == size_in_pages);
      e->latched= 0;
      e->complete= false;
      e->flushed_clean= true;
      fifos[file_no & 1].first_page= e;
    }
    else
      fifos[file_no & 1].first_page= nullptr;
    fifos[file_no & 1].first_page_no= init_page;
  }
  else
  {
    fifos[file_no & 1].first_page= nullptr;
    fifos[file_no & 1].first_page_no= 0;
  }
  fifos[file_no & 1].fh= (File)-1;
  fifos[file_no & 1].size_in_pages= size_in_pages;
  mysql_mutex_unlock(&m_mutex);
}


void
fsp_binlog_page_fifo::release_tablespace(uint64_t file_no)
{
  mysql_mutex_lock(&m_mutex);
  ut_a(file_no == first_file_no);
  ut_a(!fifos[file_no & 1].first_page ||
       /* Allow a final, incomplete-but-fully-flushed page in the fifo. */
       (!fifos[file_no & 1].first_page->complete &&
        fifos[file_no & 1].first_page->flushed_clean &&
        !fifos[file_no & 1].first_page->next &&
        !fifos[(file_no + 1) & 1].first_page));
  if (fifos[file_no & 1].fh != (File)-1)
  {
    while (flushing)
      my_cond_wait(&m_cond, &m_mutex.m_mutex);
    flushing= true;
    File fh= fifos[file_no & 1].fh;
    mysql_mutex_unlock(&m_mutex);
    int res= my_sync(fh, MYF(MY_WME));
    ut_a(!res);
    my_close(fifos[file_no & 1].fh, MYF(0));
    mysql_mutex_lock(&m_mutex);
    flushing= false;
    pthread_cond_signal(&m_cond);
  }
  first_file_no= file_no + 1;

  fifos[file_no & 1].first_page= nullptr;
  fifos[file_no & 1].first_page_no= 0;
  fifos[file_no & 1].size_in_pages= 0;
  fifos[file_no & 1].fh= (File)-1;
  mysql_mutex_unlock(&m_mutex);
}


fsp_binlog_page_fifo::fsp_binlog_page_fifo()
  : first_file_no(~(uint64_t)0), flushing(false),
    flush_thread_started(false), flush_thread_end(false)
{
  fifos[0]= {nullptr, 0, 0, (File)-1 };
  fifos[1]= {nullptr, 0, 0, (File)-1 };
  mysql_mutex_init(fsp_page_fifo_mutex_key, &m_mutex, nullptr);
  pthread_cond_init(&m_cond, nullptr);

  // ToDo I think I need to read the first page here, or somewhere?
  // Normally I'd never want to read a page into the page fifo, but at startup, I seem to need to do so for the first page I start writing on. Though I suppose I already read that, so maybe just a way to add that page into the FIFO in the constructor?
}


void
fsp_binlog_page_fifo::reset()
{
  ut_ad(!flushing);
  for (uint32_t i= 0; i < 2; ++i)
  {
    if (fifos[i].fh != (File)-1)
      my_close(fifos[i].fh, MYF(0));
    fsp_binlog_page_entry *e= fifos[i].first_page;
    while (e)
    {
      fsp_binlog_page_entry *next= e->next;
      aligned_free(e->page_buf);
      ut_free(e);
      e= next;
    }
    fifos[i]= {nullptr, 0, 0, (File)-1 };
  }
  first_file_no= ~(uint64_t)0;
}


fsp_binlog_page_fifo::~fsp_binlog_page_fifo()
{
  ut_ad(!flushing);
  reset();
  mysql_mutex_destroy(&m_mutex);
  pthread_cond_destroy(&m_cond);
}


void
fsp_binlog_page_fifo::lock_wait_for_idle()
{
  mysql_mutex_lock(&m_mutex);
  while(flushing)
    my_cond_wait(&m_cond, &m_mutex.m_mutex);
}


void
fsp_binlog_page_fifo::start_flush_thread()
{
  flush_thread_started= false;
  flush_thread_end= false;
  flush_thread_obj= std::thread{ [this] { flush_thread_run(); } };
  mysql_mutex_lock(&m_mutex);
  while (!flush_thread_started)
    my_cond_wait(&m_cond, &m_mutex.m_mutex);
  mysql_mutex_unlock(&m_mutex);
}


void
fsp_binlog_page_fifo::stop_flush_thread()
{
  if (!flush_thread_started)
    return;
  mysql_mutex_lock(&m_mutex);
  flush_thread_end= true;
  pthread_cond_signal(&m_cond);
  while (flush_thread_started)
    my_cond_wait(&m_cond, &m_mutex.m_mutex);
  mysql_mutex_unlock(&m_mutex);
  flush_thread_obj.join();
}


void
fsp_binlog_page_fifo::flush_thread_run()
{
  mysql_mutex_lock(&m_mutex);
  flush_thread_started= true;
  pthread_cond_signal(&m_cond);

  while (!flush_thread_end)
  {
    /*
      Flush pages one by one as long as there are more pages pending.
      Once all have been flushed, wait for more pages to become pending.
      Don't try to force flush a final page that is not yet completely
      filled with data.
    */
    uint64_t file_no= first_file_no;
    bool all_flushed= true;
    if (first_file_no != ~(uint64_t)0)
    {
      all_flushed= flush_one_page(file_no, false);
      /*
        flush_one_page() can release the m_mutex temporarily, so do an
        extra check against first_file_no to guard against a RESET MASTER
        running in parallel.
      */
      if (all_flushed && file_no <= first_file_no)
        all_flushed= flush_one_page(file_no + 1, false);
    }
    if (all_flushed)
      my_cond_wait(&m_cond, &m_mutex.m_mutex);
  }

  flush_thread_started= false;
  pthread_cond_signal(&m_cond);
  mysql_mutex_unlock(&m_mutex);
}

void
binlog_write_up_to_now() noexcept
{
  fsp_binlog_page_fifo *fifo= binlog_page_fifo;
  if (!fifo)
    return;    /* Startup eg. */

  uint64_t active= active_binlog_file_no.load(std::memory_order_relaxed);
  uint64_t active2;
  uint32_t page_no;
  do
  {
    active2= active;
    /* ToDo: What kind of locking or std::memory_order is needed for page_no? */
    page_no= binlog_cur_page_no;
    active= active_binlog_file_no.load(std::memory_order_relaxed);
  } while (UNIV_UNLIKELY(active != active2));

  if (active != ~(uint64_t)0)
  {
    fifo->flush_up_to(active, page_no);
    fifo->do_fdatasync(active);
  }
}


void
fsp_log_binlog_write(mtr_t *mtr, fsp_binlog_page_entry *page,
                     uint32_t page_offset, uint32_t len)
{
  uint64_t file_no= page->file_no;
  uint32_t page_no= page->page_no;
  if (page_offset + len >= srv_page_size - FIL_PAGE_DATA_END)
    page->complete= true;
  if (page->flushed_clean)
  {
    /*
      If the page with partial data has been written to the file system, then
      redo log all the data on the page, to be sure we can still recover the
      entire page reliably even if the latest checkpoint is after that partial
      write.
    */
    len= page_offset + len;
    page_offset= 0;
    page->flushed_clean= false;
  }
  mtr->write_binlog((file_no & 1), page_no, (uint16_t)page_offset,
                    page_offset + &page->page_buf[0], len);
}

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
  binlog_page_fifo= new fsp_binlog_page_fifo();
  binlog_page_fifo->start_flush_thread();
}


void
fsp_binlog_shutdown()
{
  binlog_page_fifo->stop_flush_thread();
  delete binlog_page_fifo;
  pthread_cond_destroy(&active_binlog_cond);
  mysql_mutex_destroy(&active_binlog_mutex);
}


/** Write out all pages, flush, and close/detach a binlog tablespace.
@param[in] file_no	 Index of the binlog tablespace
@return DB_SUCCESS or error code */
dberr_t
fsp_binlog_tablespace_close(uint64_t file_no)
{
  binlog_page_fifo->flush_up_to(file_no, ~(uint32_t)0);
  /* release_tablespace() will fdatasync() the file first. */
  binlog_page_fifo->release_tablespace(file_no);
  /*
    Durably sync the redo log. This simplifies things a bit, as then we know
    that we will not need to discard any data from an old binlog file during
    recovery, at most from the latest two existing files.
  */
  log_buffer_flush_to_disk(true);
  return DB_SUCCESS;
}


/*
  Open an existing tablespace. The filehandle fh is taken over by the tablespace
  (or closed in case of error).
*/
bool
fsp_binlog_open(const char *file_name, pfs_os_file_t fh,
                uint64_t file_no, size_t file_size,
                uint32_t init_page, byte *partial_page)
{
  const uint32_t page_size= (uint32_t)srv_page_size;
  const uint32_t page_size_shift= srv_page_size_shift;

  os_offset_t binlog_size= innodb_binlog_size_in_pages << srv_page_size_shift;
  if (init_page == ~(uint32_t)0 && file_size < binlog_size) {
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
    return true;
  }

  binlog_page_fifo->create_tablespace(file_no,
                                      (uint32_t)(file_size >> page_size_shift),
                                      init_page, partial_page);
  os_file_close(fh);
  first_open_binlog_file_no= file_no;
  if (last_created_binlog_file_no == ~(uint64_t)0 ||
      file_no > last_created_binlog_file_no) {
    last_created_binlog_file_no= file_no;
  }
  return false;
}


/** Create a binlog tablespace file
@param[in]  file_no	 Index of the binlog tablespace
@return DB_SUCCESS or error code */
dberr_t fsp_binlog_tablespace_create(uint64_t file_no, uint32_t size_in_pages)
{
	pfs_os_file_t	fh;
	bool		ret;

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

	/* We created the binlog file and now write it full of zeros */
	if (!os_file_set_size(name, fh,
			      os_offset_t{size_in_pages} << srv_page_size_shift)
            ) {
		sql_print_error("InnoDB: Unable to allocate file %s", name);
		os_file_close(fh);
		os_file_delete(innodb_data_file_key, name);
		return DB_ERROR;
	}

        binlog_page_fifo->create_tablespace(file_no, size_in_pages);
        os_file_close(fh);

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
  const uint32_t page_end= page_size - FIL_PAGE_DATA_END;
  uint32_t page_no= binlog_cur_page_no;
  uint32_t page_offset= binlog_cur_page_offset;
  /* ToDo: What is the lifetime of what's pointed to by binlog_cur_block, is there some locking needed around it or something? */
  fsp_binlog_page_entry *block= nullptr;
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
      uint32_t file_size_in_pages= binlog_page_fifo->size_in_pages(file_no);
      if (UNIV_UNLIKELY(page_no >= file_size_in_pages)) {
        /*
          Signal to the pre-allocation thread that this tablespace has been
          written full, so that it can be closed and a new one pre-allocated
          in its place. Then wait for a new tablespace to be pre-allocated that
          we can use.

          The normal case is that the next tablespace is already pre-allocated
          and available; binlog tablespace N is active while (N+1) is being
          pre-allocated. Only under extreme I/O pressure should be need to
          stall here.
        */
        ut_ad(!pending_prev_end_offset);
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
        pthread_cond_signal(&active_binlog_cond);
        mysql_mutex_unlock(&active_binlog_mutex);
        binlog_cur_page_no= page_no= 0;
        /* ToDo: Here we must use the value from the file, if this file was pre-allocated before a server restart where the value of innodb_binlog_state_interval changed. Maybe just make innodb_binlog_state_interval dynamic and make the prealloc thread (and discover code at startup) supply the correct value to use for each file. */
        current_binlog_state_interval=
          (uint32_t)(innodb_binlog_state_interval >> page_size_shift);
      }

      /* Must be a power of two. */
      ut_ad(current_binlog_state_interval == 0 ||
            current_binlog_state_interval ==
            (uint64_t)1 << (63 - nlz(current_binlog_state_interval)));

      if (0 == (page_no & (current_binlog_state_interval - 1))) {
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
                                 page_offset, file_no, file_size_in_pages);
          ut_a(!err /* ToDo error handling */);
          ut_ad(block);
          full_state.free();
          binlog_diff_state.reset_nolock();
        } else {
          bool err= binlog_gtid_state(&binlog_diff_state, mtr, block, page_no,
                                      page_offset, file_no, file_size_in_pages);
          ut_a(!err /* ToDo error handling */);
        }
      } else
        block= binlog_page_fifo->create_page(file_no, page_no);
    } else {
      block= binlog_page_fifo->get_page(file_no, page_no);
    }

    ut_ad(page_offset < page_end);
    uint32_t page_remain= page_end - page_offset;
    byte *ptr= page_offset + &block->page_buf[0];
    /* ToDo: Do this check at the end instead, to save one buf_page_get_gen()? */
    if (page_remain < 4) {
      /* Pad the remaining few bytes, and move to next page. */
      if (UNIV_LIKELY(page_remain > 0))
      {
        memset(ptr, FSP_BINLOG_TYPE_FILLER,  page_remain);
        fsp_log_binlog_write(mtr, block, page_offset, page_remain);
      }
      binlog_page_fifo->release_page_mtr(block, mtr);
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

    fsp_log_binlog_write(mtr, block, page_offset, size + 3);
    cont_flag= FSP_BINLOG_FLAG_CONT;
    if (page_remain == 0) {
      binlog_page_fifo->release_page_mtr(block, mtr);
      block= nullptr;
      page_offset= FIL_PAGE_DATA;
      ++page_no;
    } else {
      page_offset+= size+3;
    }
    if (size_last.second)
      break;
  }
  if (block)
    binlog_page_fifo->release_page_mtr(block, mtr);
  binlog_cur_page_no= page_no;
  binlog_cur_page_offset= page_offset;
  if (UNIV_UNLIKELY(pending_prev_end_offset != 0))
  {
    mysql_mutex_lock(&active_binlog_mutex);
    binlog_cur_end_offset[(file_no-1) & 1].store(pending_prev_end_offset,
                                                 std::memory_order_relaxed);
    active_binlog_file_no.store(file_no, std::memory_order_release);
    pthread_cond_signal(&active_binlog_cond);
    mysql_mutex_unlock(&active_binlog_mutex);
  }
  binlog_cur_end_offset[file_no & 1].store(((uint64_t)page_no << page_size_shift) + page_offset,
                                           std::memory_order_relaxed);
  return {start_file_no, start_offset};
}


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
  uint32_t page_no= binlog_cur_page_no;
  chunk_data_flush dummy_data;
  mtr_t mtr;

  mysql_mutex_lock(&purge_binlog_mutex);

  binlog_page_fifo->lock_wait_for_idle();
  File fh= binlog_page_fifo->get_fh(file_no);
  if (fh == (File)-1)
  {
    binlog_page_fifo->unlock();
    mysql_mutex_unlock(&purge_binlog_mutex);
    return true;
  }

  if (my_chsize(fh, ((uint64_t)page_no + 1) << srv_page_size_shift, 0,
                MYF(MY_WME)))
  {
    binlog_page_fifo->unlock();
    mysql_mutex_unlock(&purge_binlog_mutex);
    return true;
  }
  /*
    Sync the truncate to disk. This way, if we crash after this we are sure the
    truncate has been effected so we do not put the filler record in what is
    then the middle of the file. If we crash before the truncate is durable, we
    just come up as if the flush has never happened. If we crash with the
    truncate durable but without the filler record, that is not a problem, the
    binlog file will just be shorter.
  */
  my_sync(fh, MYF(0));
  binlog_page_fifo->unlock();

  uint32_t page_offset= binlog_cur_page_offset;
  if (page_offset > FIL_PAGE_DATA ||
      page_offset < srv_page_size - FIL_PAGE_DATA_END)
  {
  /*
    If we are not precisely the end of a page, fill up that page with a dummy
    record. Otherwise the zeros at the end of the page would be detected as
    end-of-file of the entire binlog.
  */
    mtr.start();
    fsp_binlog_write_rec(&dummy_data, &mtr, FSP_BINLOG_TYPE_DUMMY);
    mtr.commit();
  }

  if (page_no + 1 < binlog_page_fifo->size_in_pages(file_no))
  {
    binlog_page_fifo->truncate_file_size(file_no, page_no + 1);
    size_t reclaimed= (binlog_page_fifo->size_in_pages(file_no) - (page_no + 1))
      << srv_page_size_shift;
    if (UNIV_LIKELY(total_binlog_used_size >= reclaimed))
      total_binlog_used_size-= reclaimed;
    else
      ut_ad(0);
  }

  /* Flush out all pages in the (now filled-up) tablespace. */
  binlog_page_fifo->flush_up_to(file_no, page_no);

  mysql_mutex_unlock(&purge_binlog_mutex);

  /*
    Now get a new GTID state record written to the next binlog tablespace.
    This ensures that the new state (in case of DELETE_DOMAIN_ID) will be
    persisted across a server restart.
  */
  mtr.start();
  fsp_binlog_write_rec(&dummy_data, &mtr, FSP_BINLOG_TYPE_FILLER);
  mtr.commit();
  log_buffer_flush_to_disk(srv_flush_log_at_trx_commit & 1);

  return false;
}


binlog_chunk_reader::binlog_chunk_reader()
  : s { 0, 0, 0, 0, 0, FSP_BINLOG_TYPE_FILLER, false, false },
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
  sql_print_error("InnoDB: Corrupt binlog found on page %llu in binlog number "
                  "%llu: %s", page_no, file_no, msg);
  return -1;
}


/*
  Obtain the data on the page currently pointed to by the chunk reader. The
  page is either latched in the page fifo, or read from the file into the page
  buffer.

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
  ut_ad(!cur_block /* Must have no active page latch */);
  uint64_t active2= active_binlog_file_no.load(std::memory_order_acquire);
  for (;;) {
    fsp_binlog_page_entry *block= nullptr;
    uint64_t offset= (s.page_no << srv_page_size_shift) | s.in_page_offset;
    uint64_t active= active2;
    uint64_t end_offset=
      binlog_cur_end_offset[s.file_no&1].load(std::memory_order_acquire);
    if (s.file_no > active)
    {
      ut_ad(s.page_no == 0);
      ut_ad(s.in_page_offset == 0);
      /*
        Allow a reader that reached the very end of the active binlog file to
        have moved ahead early to the start of the coming binlog file.
      */
      return CHUNK_READER_EOF;
    }

    if (s.file_no + 1 >= active) {
      /* Check if we should read from the buffer pool or from the file. */
      if (end_offset != ~(uint64_t)0 && offset < end_offset) {
        /*
          ToDo: Should we keep track of the last block read and use it as a
          hint? Will be mainly useful when reading the partially written active
          page at the current end of the active binlog, which might be a common
          case.
        */
        block= binlog_page_fifo->get_page(s.file_no, s.page_no);
      }
      active2= active_binlog_file_no.load(std::memory_order_acquire);
      if (UNIV_UNLIKELY(active2 != active)) {
        /*
          The active binlog file changed while we were processing; we might
          have gotten invalid end_offset or a buffer pool page from a wrong
          tablespace. So just try again.
        */
        continue;
      }
      cur_end_offset= end_offset;
      if (offset >= end_offset) {
        if (s.file_no == active) {
          /* Reached end of the currently active binlog file -> EOF. */
          return CHUNK_READER_EOF;
        }
      }
      if (block) {
        cur_block= block;
        page_ptr= block->page_buf;
        return CHUNK_READER_FOUND;
      } else {
        /* Not in buffer pool, just read it from the file. */
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
      binlog_page_fifo->release_page(cur_block);
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
      binlog_page_fifo->release_page(cur_block);
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
    binlog_page_fifo->release_page(cur_block);
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
    ut_ad(active > s.file_no || (s.page_no == 0 && s.in_page_offset == 0));
    return active > s.file_no;
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
