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

#include <type_traits>
#include "fsp0fsp.h"
#include "buf0flu.h"
#include "trx0trx.h"
#include "fsp_binlog.h"
#include "innodb_binlog.h"

#include "my_bit.h"
#include "rpl_gtid_base.h"
#include "log.h"


/**
  The page size used for binlog pages.

  For now, we just use a 16k page size. It could be changed later to be
  configurable, changing the page size of the binlog is much easier than for
  normal InnoDB tablespaces, as we could simply flush out the current file and
  create the next file with a different page size, just need to put the page
  size somewhere in the file header.

  On the other hand, the page size does not seem to be very significant for
  performance or anything. All data can be split across to the next page, and
  everything is written in sequence through the kernel buffer cache which is
  then free to flush it to disk in whatever chunk sizes it wants.
*/
uint32_t ibb_page_size_shift= 14;
ulong ibb_page_size= (1 << ibb_page_size_shift);


/**
  How often (in terms of pages written) to dump a (differential) binlog state
  at the start of the page, to speed up finding the initial GTID position for
  a connecting slave.

  This value must be used over the setting innodb_binlog_state_interval,
  because after a restart the latest binlog file will be using the value of the
  setting prior to the restart; the new value of the setting (if different)
  will be used for newly created binlog files. The value refers to the file
  of active_binlog_file_no.
*/
uint64_t current_binlog_state_interval;

/**
  Mutex protecting active_binlog_file_no.
*/
mysql_mutex_t active_binlog_mutex;
pthread_cond_t active_binlog_cond;
/** Mutex protecting binlog_cur_durable_offset[] and ibb_pending_lsn_fifo. */
mysql_mutex_t binlog_durable_mutex;
mysql_cond_t binlog_durable_cond;

/** The currently being written binlog tablespace. */
std::atomic<uint64_t> active_binlog_file_no;

/**
  The first binlog tablespace that is still open.
  This can be equal to active_binlog_file_no, if the tablespace prior to the
  active one has been fully flushed out to disk and closed.
  Or it can be one less, if the prior tablespace is still being written out and
  closed.
*/
uint64_t first_open_binlog_file_no;

/**
  The most recent created and open tablespace.
  This can be equal to active_binlog_file_no+1, if the next tablespace to be
  used has already been pre-allocated and opened.
  Or it can be the same as active_binlog_file_no, if the pre-allocation of the
  next tablespace is still pending.
*/
uint64_t last_created_binlog_file_no;

/**
  Point at which it is guaranteed that all data has been written out to the
  binlog file (on the OS level; not necessarily fsync()'ed yet).

  Stores the most recent four values, each corresponding to
  active_binlog_file_no&4. This is so that it can be always valid for both
  active and active-1 (active-2 is always durable, as we make the entire binlog
  file N durable before pre-allocating N+2). Just before active moves to the
  next file_no, we can set the value for active+1, leaving active and active-1
  still valid. (Only 3 entries are needed, but we use four to be able to use
  bit-wise and instead of modulo-3).
*/
std::atomic<uint64_t> binlog_cur_durable_offset[4];
/**
  Offset of last valid byte of data in most recent 4 binlog files.
  A value of ~0 means that file is not opened as a tablespace (and data is
  valid until the end of the file).
*/
std::atomic<uint64_t> binlog_cur_end_offset[4];

fsp_binlog_page_fifo *binlog_page_fifo;

/** Object to keep track of outstanding oob references in binlog files. */
ibb_file_oob_refs ibb_file_hash;


fsp_binlog_page_entry *
fsp_binlog_page_fifo::get_entry(uint64_t file_no, uint64_t page_no,
                                uint32_t latch, bool completed, bool clean)
{
  mysql_mutex_assert_owner(&m_mutex);
  ut_a(file_no == first_file_no || file_no == first_file_no + 1);
  page_list *pl= &fifos[file_no & 1];
  ut_ad(pl->first_page_no + pl->used_entries == page_no);
  if (UNIV_UNLIKELY(pl->used_entries == pl->allocated_entries))
  {
    size_t new_allocated_entries= 2*pl->allocated_entries;
    size_t new_size= new_allocated_entries * sizeof(*pl->entries);
    fsp_binlog_page_entry **new_entries=
      (fsp_binlog_page_entry **)ut_realloc(pl->entries, new_size);
    if (!new_entries)
      return nullptr;
    /* Copy any wrapped-around elements into not-wrapped new locations. */
    if (pl->first_entry + pl->used_entries > pl->allocated_entries)
    {
      size_t wrapped_entries=
        pl->first_entry + pl->used_entries - pl->allocated_entries;
      ut_ad(new_allocated_entries >= pl->allocated_entries + wrapped_entries);
      memcpy(new_entries + pl->allocated_entries, new_entries,
             wrapped_entries * sizeof(*new_entries));
    }
    pl->entries= new_entries;
    pl->allocated_entries= new_allocated_entries;
  }
  fsp_binlog_page_entry *&e_loc= pl->entry_at(pl->used_entries);
  ++pl->used_entries;
  if (UNIV_LIKELY(freelist != nullptr))
  {
    e_loc= (fsp_binlog_page_entry *)freelist;
    freelist= (byte *)*(uintptr_t *)freelist;
    --free_buffers;
  }
  else
  {
    byte *mem=
      static_cast<byte*>(ut_malloc(sizeof(*e_loc) + ibb_page_size,
                                   mem_key_binlog));
    if (!mem)
      return nullptr;
    e_loc= (fsp_binlog_page_entry *)mem;
  }
  e_loc->latched= latch;
  e_loc->last_page= (page_no + 1 == size_in_pages(file_no));
  e_loc->complete= completed;
  e_loc->flushed_clean= clean;
  e_loc->pending_flush= false;
  return e_loc;
}


void
fsp_binlog_page_fifo::release_entry(uint64_t file_no, uint64_t page_no)
{
  ut_a(file_no == first_file_no || file_no == first_file_no + 1);
  page_list *pl= &fifos[file_no & 1];
  ut_a(page_no == pl->first_page_no);
  fsp_binlog_page_entry *e= pl->entries[pl->first_entry];
  ut_ad(pl->used_entries > 0);
  --pl->used_entries;
  ++pl->first_entry;
  ++pl->first_page_no;
  if (UNIV_UNLIKELY(pl->first_entry == pl->allocated_entries))
    pl->first_entry= 0;
  /*
    Put the page buffer on the freelist. Unless we already have too much on the
    freelist; then put it on a temporary list so it can be freed later, outside
    of holding the mutex.
  */
  if (UNIV_LIKELY(free_buffers * MAX_FREE_BUFFERS_FRAC <=
                  innodb_binlog_size_in_pages))
  {
    *(uintptr_t *)e= (uintptr_t)freelist;
    freelist= (byte *)e;
    ++free_buffers;
  }
  else
  {
    *(uintptr_t *)e= (uintptr_t)to_free_list;
    to_free_list= (byte *)e;
  }
}


void
fsp_binlog_page_fifo::unlock_with_delayed_free()
{
  mysql_mutex_assert_owner(&m_mutex);
  byte *to_free= to_free_list;
  to_free_list= nullptr;
  mysql_mutex_unlock(&m_mutex);
  if (UNIV_UNLIKELY(to_free != nullptr))
  {
    do
    {
      byte *next= (byte *)*(uintptr_t *)to_free;
      ut_free(to_free);
      to_free= next;
    } while (to_free);
  }
}


fsp_binlog_page_entry *
fsp_binlog_page_fifo::create_page(uint64_t file_no, uint32_t page_no)
{
  mysql_mutex_lock(&m_mutex);
  ut_ad(first_file_no != ~(uint64_t)0);
  ut_a(file_no == first_file_no || file_no == first_file_no + 1);
  /* Can only allocate pages consecutively. */
  ut_a(page_no == fifos[file_no & 1].first_page_no +
       fifos[file_no & 1].used_entries);
  fsp_binlog_page_entry *e= get_entry(file_no, page_no, 1, false, false);
  ut_a(e);
  mysql_mutex_unlock(&m_mutex);
  memset(e->page_buf(), 0, ibb_page_size);

  return e;
}


fsp_binlog_page_entry *
fsp_binlog_page_fifo::get_page(uint64_t file_no, uint32_t page_no)
{
  fsp_binlog_page_entry *res= nullptr;
  page_list *pl;

  mysql_mutex_lock(&m_mutex);

  ut_ad(first_file_no != ~(uint64_t)0);
  ut_a(file_no <= first_file_no + 1);
  if (file_no < first_file_no)
    goto end;
  pl= &fifos[file_no & 1];
  if (page_no >= pl->first_page_no &&
      page_no < pl->first_page_no + pl->used_entries)
  {
    res= pl->entry_at(page_no - pl->first_page_no);
    ++res->latched;
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
  if (--page->latched == 0 && (page->complete || page->pending_flush))
    pthread_cond_broadcast(&m_cond);  /* Page ready to be flushed to disk */
  mysql_mutex_unlock(&m_mutex);
}


/**
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

  /*
    Check against having two pending last-in-binlog-file pages to release.
    But allow to have the same page released twice in a single mtr (this can
    happen when 2-phase commit puts an XID/XA complete record just in front
    of the commit record).
  */
  fsp_binlog_page_entry *old_page= mtr->get_binlog_page();
  ut_ad(!(old_page != nullptr && old_page != page));
  if (UNIV_UNLIKELY(old_page != nullptr))
  {
    if (UNIV_UNLIKELY(old_page != page))
      sql_print_error("InnoDB: Internal inconsistency with mini-transaction "
                      "that spans more than two binlog files. Recovery may "
                      "be affected until the next checkpoint.");
    release_page(old_page);
  }
  mtr->set_binlog_page(page);
}


/* Check if there are any (complete) non-flushed pages in a tablespace. */
bool
fsp_binlog_page_fifo::has_unflushed(uint64_t file_no)
{
  mysql_mutex_assert_owner(&m_mutex);
  if (UNIV_UNLIKELY(file_no < first_file_no))
    return false;
  if (UNIV_UNLIKELY(file_no > first_file_no + 1))
    return false;
  const page_list *pl= &fifos[file_no & 1];
  if (pl->used_entries == 0)
    return false;
  if (!pl->entries[pl->first_entry]->complete)
    return false;
  ut_ad(!pl->entries[pl->first_entry]->flushed_clean
        /* Clean and complete page should have been freed */);
  return true;
}


/**
  Flush (write to disk) the first unflushed page in a file.
  Returns true when the last page has been flushed.

  Must be called with m_mutex held.

  If called with force=true, will flush even any final, incomplete page.
  Otherwise such page will not be written out. Any final, incomplete page
  is left in the FIFO in any case.
*/
void
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
      return;
    /* Guard against simultaneous RESET MASTER. */
    if (file_no > first_file_no + 1)
      return;

    if (!flushing)
    {
      pl= &fifos[file_no & 1];
      if (pl->used_entries == 0)
        return;
      e= pl->entries[pl->first_entry];
      if (e->latched == 0)
        break;
      if (force)
        e->pending_flush= true;
    }
    my_cond_wait(&m_cond, &m_mutex.m_mutex);
  }
  flushing= true;
  uint32_t page_no= pl->first_page_no;
  bool is_complete= e->complete;
  ut_ad(is_complete || pl->used_entries == 1);
  if (is_complete || (force && !e->flushed_clean))
  {
    /*
      Careful here! We are going to release the mutex while flushing the page
      to disk. At this point, another thread might come in and add more data
      to the page in parallel, if e->complete is not set!

      So here we set flushed_clean _before_ releasing the mutex. Then any
      other thread that in parallel latches the page and tries to update it in
      parallel will either increment e->latched, or set e->flushed_clean back
      to false (or both). This allows us to detect a parallel update and retry
      the write in that case.
    */
  retry:
    if (!is_complete)
      e->flushed_clean= true;
    e->pending_flush= false;
    /* Release the mutex, then free excess page buffers while not holding it. */
    unlock_with_delayed_free();
    File fh= get_fh(file_no);
    ut_a(pl->fh >= (File)0);
    size_t res= crc32_pwrite_page(fh, e->page_buf(), page_no, MYF(MY_WME));
    ut_a(res == ibb_page_size);
    mysql_mutex_lock(&m_mutex);
    if (UNIV_UNLIKELY(e->latched) ||
        (!is_complete && UNIV_UNLIKELY(!e->flushed_clean)))
    {
      flushing= false;
      pthread_cond_broadcast(&m_cond);
      for (;;)
      {
        ut_ad(file_no < first_file_no ||
              pl->first_page_no >= page_no);
        ut_ad(file_no < first_file_no ||
              pl->first_page_no > page_no ||
              pl->entries[pl->first_entry] == e);
        if (!flushing)
        {
          if (file_no < first_file_no ||
              pl->first_page_no != page_no ||
              pl->entries[pl->first_entry] != e)
          {
            /* Someone else flushed the page for us. */
            return;
          }
          /* Guard against simultaneous RESET MASTER. */
          if (file_no > first_file_no + 1)
            return;
          if (e->latched == 0)
            break;
          if (force)
            e->pending_flush= true;
        }
        my_cond_wait(&m_cond, &m_mutex.m_mutex);
      }
      flushing= true;
      if (!is_complete)
      {
        /*
          The page was not complete, a writer may have added data. Need to redo
          the flush.
        */
        is_complete= e->complete;
        goto retry;
      }
      /*
        The page was complete, but was latched while we were flushing (by a
        reader). No need to flush again, just needed to wait until the latch
        was released before we can continue to free the page.
      */
    }
  }
  /*
    We marked the FIFO as flushing, page could not have disappeared despite
    releasing the mutex during the I/O.
  */
  ut_ad(flushing);
  ut_ad(pl->used_entries >= 1);
  if (is_complete)
    release_entry(file_no, page_no);
  flushing= false;
  pthread_cond_broadcast(&m_cond);
}


void
fsp_binlog_page_fifo::flush_up_to(uint64_t file_no, uint32_t page_no)
{
  mysql_mutex_lock(&m_mutex);
  for (;;)
  {
    const page_list *pl= &fifos[file_no & 1];
    if (file_no < first_file_no ||
        (file_no == first_file_no && pl->first_page_no > page_no))
      break;
    /* Guard against simultaneous RESET MASTER. */
    if (file_no > first_file_no + 1)
      break;
    /*
      The flush is complete if there are no pages left, or if there is just
      one incomplete page left that is fully flushed so far.
    */
    if (pl->used_entries == 0 ||
        (pl->used_entries == 1 && !pl->entries[pl->first_entry]->complete &&
         pl->entries[pl->first_entry]->flushed_clean))
      break;
    uint64_t file_no_to_flush= file_no;
    /* Flush the prior file to completion first. */
    if (file_no == first_file_no + 1 && fifos[(file_no - 1) & 1].used_entries)
    {
      file_no_to_flush= file_no - 1;
      pl= &fifos[file_no_to_flush & 1];
      ut_ad(pl->entries[pl->first_entry]->complete);
    }
    flush_one_page(file_no_to_flush, true);
  }
  /* Will release the mutex and free any excess page buffers. */
  unlock_with_delayed_free();
}


void
fsp_binlog_page_fifo::do_fdatasync(uint64_t file_no)
{
  File fh;
  mysql_mutex_lock(&m_mutex);
  for (;;)
  {
    if (file_no < first_file_no)
      break;   /* Old files are already fully synced. */
    /* Guard against simultaneous RESET MASTER. */
    if (file_no > first_file_no + 1)
      break;
    fh= fifos[file_no & 1].fh;
    if (fh <= (File)-1)
      break;
    if (flushing)
    {
      while (flushing)
        my_cond_wait(&m_cond, &m_mutex.m_mutex);
      continue;  /* Loop again to recheck state, as we released the mutex */
    }
    flushing= true;
    mysql_mutex_unlock(&m_mutex);
    int res= my_sync(fh, MYF(MY_WME));
    ut_a(!res);
    mysql_mutex_lock(&m_mutex);
    flushing= false;
    pthread_cond_broadcast(&m_cond);
    break;
  }
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

/**
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
         fifos[first_file_no & 1].used_entries == 0));
  ut_a(first_file_no == ~(uint64_t)0 ||
       file_no == first_file_no + 1 ||
       file_no == first_file_no + 2 ||
       (init_page != ~(uint32_t)0 && file_no + 1 == first_file_no &&
         fifos[first_file_no & 1].used_entries == 0));
  page_list *pl= &fifos[file_no & 1];
  if (first_file_no == ~(uint64_t)0)
  {
    first_file_no= file_no;
  }
  else if (UNIV_UNLIKELY(file_no + 1 == first_file_no))
    first_file_no= file_no;
  else if (file_no == first_file_no + 2)
  {
    /* All pages in (N-2) must be flushed before doing (N). */
    ut_a(pl->used_entries == 0);
    if (UNIV_UNLIKELY(pl->fh != (File)-1))
    {
      ut_ad(false /* Should have been done as part of tablespace close. */);
      my_close(pl->fh, MYF(0));
    }
    first_file_no= file_no - 1;
  }

  pl->fh= (File)-1;
  pl->size_in_pages= size_in_pages;
  ut_ad(pl->used_entries == 0);
  ut_ad(pl->first_entry == 0);
  if (UNIV_UNLIKELY(init_page != ~(uint32_t)0))
  {
    pl->first_page_no= init_page;
    if (partial_page)
    {
      fsp_binlog_page_entry *e= get_entry(file_no, init_page, 0, false, true);
      ut_a(e);
      memcpy(e->page_buf(), partial_page, ibb_page_size);
    }
  }
  else
    pl->first_page_no= 0;
  pthread_cond_broadcast(&m_cond);
  mysql_mutex_unlock(&m_mutex);
}


void
fsp_binlog_page_fifo::release_tablespace(uint64_t file_no)
{
  mysql_mutex_lock(&m_mutex);
  page_list *pl= &fifos[file_no & 1];
  ut_a(file_no == first_file_no);
  ut_a(pl->used_entries == 0 ||
       /* Allow a final, incomplete-but-fully-flushed page in the fifo. */
       (!pl->entries[pl->first_entry]->complete &&
        pl->entries[pl->first_entry]->flushed_clean &&
        pl->used_entries == 1 &&
        fifos[(file_no + 1) & 1].used_entries == 0));
  if (pl->fh != (File)-1)
  {
    while (flushing)
      my_cond_wait(&m_cond, &m_mutex.m_mutex);
    flushing= true;
    File fh= pl->fh;
    mysql_mutex_unlock(&m_mutex);
    int res= my_sync(fh, MYF(MY_WME));
    ut_a(!res);
    mysql_mutex_lock(&m_mutex);
    free_page_list(file_no);
    flushing= false;
    pthread_cond_broadcast(&m_cond);
  }
  first_file_no= file_no + 1;
  mysql_mutex_unlock(&m_mutex);
}


fsp_binlog_page_fifo::fsp_binlog_page_fifo()
  : first_file_no(~(uint64_t)0), free_buffers(0), freelist(nullptr),
    to_free_list(nullptr), flushing(false),
    flush_thread_started(false), flush_thread_end(false)
{
  for (unsigned i= 0; i < 2; ++i)
  {
    fifos[i].allocated_entries= 64;
    fifos[i].entries=
      (fsp_binlog_page_entry **)ut_malloc(fifos[i].allocated_entries *
                                          sizeof(fsp_binlog_page_entry *),
                                          mem_key_binlog);
    ut_a(fifos[i].entries);
    fifos[i].used_entries= 0;
    fifos[i].first_entry= 0;
    fifos[i].first_page_no= 0;
    fifos[i].size_in_pages= 0;
    fifos[i].fh= (File)-1;
  }
  mysql_mutex_init(fsp_page_fifo_mutex_key, &m_mutex, nullptr);
  pthread_cond_init(&m_cond, nullptr);
}


void
fsp_binlog_page_fifo::free_page_list(uint64_t file_no)
{
  page_list *pl= &fifos[file_no & 1];
  if (pl->fh != (File)-1)
    my_close(pl->fh, MYF(0));
  while (pl->used_entries > 0)
  {
    memset(pl->entries[pl->first_entry]->page_buf(), 0, ibb_page_size);
    release_entry(file_no, pl->first_page_no);
  }
  /* We hold on to the pl->entries array and reuse for next tablespace. */
  pl->used_entries= 0;
  pl->first_entry= 0;
  pl->first_page_no= 0;
  pl->size_in_pages= 0;
  pl->fh= (File)-1;
}


void
fsp_binlog_page_fifo::reset()
{
  ut_ad(!flushing);
  if (first_file_no != ~(uint64_t)0)
  {
    for (uint32_t i= 0; i < 2; ++i)
      free_page_list(first_file_no + i);
  }
  first_file_no= ~(uint64_t)0;
  /* Release page buffers in the freelist. */
  while (freelist)
  {
    byte *q= (byte *)*(uintptr_t *)freelist;
    ut_free(freelist);
    freelist= q;
  }
  free_buffers= 0;
  while (to_free_list)
  {
    byte *q= (byte *)*(uintptr_t *)to_free_list;
    ut_free(to_free_list);
    to_free_list= q;
  }
}


fsp_binlog_page_fifo::~fsp_binlog_page_fifo()
{
  ut_ad(!flushing);
  reset();
  for (uint32_t i= 0; i < 2; ++i)
    ut_free(fifos[i].entries);
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
  pthread_cond_broadcast(&m_cond);
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
  pthread_cond_broadcast(&m_cond);

  while (!flush_thread_end)
  {
    /*
      Flush pages one by one as long as there are more pages pending.
      Once all have been flushed, wait for more pages to become pending.
      Don't try to force flush a final page that is not yet completely
      filled with data.
    */
    uint64_t file_no= first_file_no;
    if (first_file_no != ~(uint64_t)0)
    {
      if (has_unflushed(file_no))
      {
        flush_one_page(file_no, false);
        continue;  // Check again for more pages available to flush
      }
      else if (has_unflushed(file_no + 1))
      {
        flush_one_page(file_no + 1, false);
        continue;
      }
    }
    if (!flush_thread_end)
      my_cond_wait(&m_cond, &m_mutex.m_mutex);
  }

  flush_thread_started= false;
  pthread_cond_broadcast(&m_cond);
  mysql_mutex_unlock(&m_mutex);
}


size_t
crc32_pwrite_page(File fd, byte *buf, uint32_t page_no, myf MyFlags) noexcept
{
  const uint32_t payload= (uint32_t)ibb_page_size - BINLOG_PAGE_CHECKSUM;
  int4store(buf + payload, my_crc32c(0, buf, payload));
  return my_pwrite(fd, (const uchar *)buf, ibb_page_size,
                   (my_off_t)page_no << ibb_page_size_shift, MyFlags);
}


/**
  Read a page, with CRC check.
  Returns:

   -1  error
    0  EOF
    1  Ok
*/
int
crc32_pread_page(File fd, byte *buf, uint32_t page_no, myf MyFlags) noexcept
{
  size_t read= my_pread(fd, buf, ibb_page_size,
                        (my_off_t)page_no << ibb_page_size_shift, MyFlags);
  int res= 1;
  if (UNIV_LIKELY(read == ibb_page_size))
  {
    const uint32_t payload= (uint32_t)ibb_page_size - BINLOG_PAGE_CHECKSUM;
    uint32_t crc32= uint4korr(buf + payload);
    /* Allow a completely zero (empty) page as well. */
    if (UNIV_UNLIKELY(crc32 != my_crc32c(0, buf, payload)) &&
        (buf[0] != 0 || 0 != memcmp(buf, buf+1, ibb_page_size - 1)))
    {
      res= -1;
      my_errno= EIO;
      if (MyFlags & MY_WME)
      {
        sql_print_error("InnoDB: Page corruption in binlog tablespace file "
                        "page number %u (invalid crc32 checksum 0x%08X)",
                        page_no, crc32);
        my_error(ER_BINLOG_READ_EVENT_CHECKSUM_FAILURE, MYF(0));
      }
    }
  }
  else if (read == (size_t)-1)
  {
    if (MyFlags & MY_WME)
      my_error(ER_READING_BINLOG_FILE, MYF(0), page_no, my_errno);
    res= -1;
  }
  else
    res= 0;

  return res;
}


int
crc32_pread_page(pfs_os_file_t fh, byte *buf, uint32_t page_no, myf MyFlags)
  noexcept
{
  const uint32_t page_size= (uint32_t)ibb_page_size;
  ulint bytes_read= 0;
  dberr_t err= os_file_read(IORequestRead, fh, buf,
                            (os_offset_t)page_no << ibb_page_size_shift,
                            page_size, &bytes_read);
  if (UNIV_UNLIKELY(err != DB_SUCCESS))
    return -1;
  else if (UNIV_UNLIKELY(bytes_read < page_size))
    return 0;

  const uint32_t payload= (uint32_t)ibb_page_size - BINLOG_PAGE_CHECKSUM;
  uint32_t crc32= uint4korr(buf + payload);
  /* Allow a completely zero (empty) page as well. */
  if (UNIV_UNLIKELY(crc32 != my_crc32c(0, buf, payload)) &&
      (buf[0] != 0 || 0 != memcmp(buf, buf+1, ibb_page_size - 1)))
  {
    my_errno= EIO;
    if (MyFlags & MY_WME)
      sql_print_error("InnoDB: Page corruption in binlog tablespace file "
                      "page number %u (invalid crc32 checksum 0x%08X)",
                      page_no, crc32);
    return -1;
  }
  return 1;
}


/**
  Need specific constructor/initializer for struct ibb_tblspc_entry stored in
  the ibb_file_hash. This is a work-around for C++ abstractions that makes it
  non-standard behaviour to memcpy() std::atomic objects.
*/
static void
ibb_file_hash_constructor(uchar *arg)
{
  new(arg + LF_HASH_OVERHEAD) ibb_tblspc_entry();
}


static void
ibb_file_hash_destructor(uchar *arg)
{
  ibb_tblspc_entry *e=
    reinterpret_cast<ibb_tblspc_entry *>(arg + LF_HASH_OVERHEAD);
  e->~ibb_tblspc_entry();
}


static void
ibb_file_hash_initializer(LF_HASH *hash, void *dst, const void *src)
{
  const ibb_tblspc_entry *src_e= static_cast<const ibb_tblspc_entry *>(src);
  ibb_tblspc_entry *dst_e=
    const_cast<ibb_tblspc_entry *>(static_cast<const ibb_tblspc_entry *>(dst));
  dst_e->file_no= src_e->file_no;
  dst_e->oob_refs.store(src_e->oob_refs.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
  dst_e->xa_refs.store(src_e->xa_refs.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
  dst_e->oob_ref_file_no.store(src_e->oob_ref_file_no.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
  dst_e->xa_ref_file_no.store(src_e->xa_ref_file_no.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
}


void
ibb_file_oob_refs::init() noexcept
{
  lf_hash_init(&hash, sizeof(ibb_tblspc_entry), LF_HASH_UNIQUE,
               offsetof(ibb_tblspc_entry, file_no),
               sizeof(ibb_tblspc_entry::file_no), nullptr, nullptr);
  hash.alloc.constructor= ibb_file_hash_constructor;
  hash.alloc.destructor= ibb_file_hash_destructor;
  hash.initializer= ibb_file_hash_initializer;
  earliest_oob_ref= ~(uint64_t)0;
  earliest_xa_ref= ~(uint64_t)0;
}


void
ibb_file_oob_refs::destroy() noexcept
{
  lf_hash_destroy(&hash);
}


void
ibb_file_oob_refs::remove(uint64_t file_no, LF_PINS *pins)
{
  lf_hash_delete(&hash, pins, &file_no, sizeof(file_no));
}


void
ibb_file_oob_refs::remove_up_to(uint64_t file_no, LF_PINS *pins)
{
  for (;;)
  {
    int res= lf_hash_delete(&hash, pins, &file_no, sizeof(file_no));
    if (res || file_no == 0)
      break;
    --file_no;
  }
}


uint64_t
ibb_file_oob_refs::oob_ref_inc(uint64_t file_no, LF_PINS *pins, bool do_xa)
{
  ibb_tblspc_entry *e= static_cast<ibb_tblspc_entry *>
    (lf_hash_search(&hash, pins, &file_no, sizeof(file_no)));
  if (!e)
    return ~(uint64_t)0;
  uint64_t refcnt= e->oob_refs.fetch_add(1, std::memory_order_acquire);
  if (UNIV_UNLIKELY(do_xa))
    refcnt= e->xa_refs.fetch_add(1, std::memory_order_acquire);
  lf_hash_search_unpin(pins);
  return refcnt + 1;
}


uint64_t
ibb_file_oob_refs::oob_ref_dec(uint64_t file_no, LF_PINS *pins, bool do_xa)
{
  ibb_tblspc_entry *e= static_cast<ibb_tblspc_entry *>
    (lf_hash_search(&hash, pins, &file_no, sizeof(file_no)));
  if (!e)
    return ~(uint64_t)0;
  uint64_t oob_refcnt= e->oob_refs.fetch_sub(1, std::memory_order_acquire) - 1;
  uint64_t ret_refcnt= oob_refcnt;
  if (UNIV_UNLIKELY(do_xa))
  {
    mysql_mutex_assert_owner(&ibb_xa_xid_hash->xid_mutex);
    ret_refcnt= e->xa_refs.fetch_sub(1, std::memory_order_acquire) - 1;
  }
  lf_hash_search_unpin(pins);
  ut_ad(oob_refcnt != (uint64_t)0 - 1);

  if (oob_refcnt == 0)
    do_zero_refcnt_action(file_no, pins, false);
  return ret_refcnt;
}


void
ibb_file_oob_refs::do_zero_refcnt_action(uint64_t file_no, LF_PINS *pins,
                                         bool active_moving)
{
  for (;;)
  {
    ibb_tblspc_entry *e= static_cast<ibb_tblspc_entry *>
      (lf_hash_search(&hash, pins, &file_no, sizeof(file_no)));
    if (!e)
      return;
    uint64_t refcnt= e->oob_refs.load(std::memory_order_acquire);
    lf_hash_search_unpin(pins);
    if (refcnt > 0)
      return;
    /*
      Reference count reached zero. Check if this was the earliest_oob_ref
      that reached zero, and if so move it to the next file. Repeat this
      for consecutive refcount-is-zero file_no, in case N+1 reaches zero
      before N does.

      As records are written into the active binlog file, the refcount can
      reach zero temporarily and then go up again, so do not move the
      earliest_oob_ref ahead yet.

      As the active is about to move to the next file, we check again, and
      this time move the earliest_oob_ref if the refcount on the (previously)
      active binlog file ended up at zero.
    */
    uint64_t active= active_binlog_file_no.load(std::memory_order_acquire);
    ut_ad(file_no <= active + active_moving);
    if (file_no >= active + active_moving)
      return;
    bool ok;
    do
    {
      uint64_t read_file_no= earliest_oob_ref.load(std::memory_order_relaxed);
      if (read_file_no != file_no)
        break;
      ok= earliest_oob_ref.compare_exchange_weak(read_file_no, file_no + 1,
                                                 std::memory_order_relaxed);
    } while (!ok);
    /* Handle any following file_no that may have dropped to zero earlier. */
    ++file_no;
  }
}


bool
ibb_file_oob_refs::update_refs(uint64_t file_no, LF_PINS *pins,
                               uint64_t oob_ref, uint64_t xa_ref)
{
  ibb_tblspc_entry *e= static_cast<ibb_tblspc_entry *>
    (lf_hash_search(&hash, pins, &file_no, sizeof(file_no)));
  if (!e)
    return false;
  e->oob_ref_file_no.store(oob_ref, std::memory_order_relaxed);
  e->xa_ref_file_no.store(xa_ref, std::memory_order_relaxed);
  lf_hash_search_unpin(pins);
  return true;
}


/*
  This is called when an xa/xid refcount goes from 1->0 or 0->1, to update
  the value of ibb_file_hash.earliest_xa_ref if necessary.
*/
void
ibb_file_oob_refs::update_earliest_xa_ref(uint64_t ref_file_no, LF_PINS *pins)
{
  mysql_mutex_assert_owner(&ibb_xa_xid_hash->xid_mutex);
  uint64_t file_no1= earliest_xa_ref.load(std::memory_order_relaxed);
  if (file_no1 < ref_file_no)
  {
    /* Current is before the updated one, no change possible for now. */
    return;
  }
  uint64_t file_no2= active_binlog_file_no.load(std::memory_order_acquire);
  uint64_t file_no= ref_file_no;
  for (;;)
  {
    if (file_no > file_no2)
    {
      /* No active XA anymore. */
      file_no= ~(uint64_t)0;
      break;
    }
    ibb_tblspc_entry *e= static_cast<ibb_tblspc_entry *>
      (lf_hash_search(&hash, pins, &file_no, sizeof(file_no)));
    if (!e)
    {
      ++file_no;
      continue;
    }
    uint64_t refcnt= e->xa_refs.load(std::memory_order_acquire);
    lf_hash_search_unpin(pins);
    if (refcnt > 0)
      break;
    ++file_no;
  }
  earliest_xa_ref.store(file_no, std::memory_order_relaxed);
}


/**
  Look up the earliest file with OOB references from a given file_no.
  Insert a new entry into the file hash (reading the file header from disk)
  if not there already.
*/
bool
ibb_file_oob_refs::get_oob_ref_file_no(uint64_t file_no, LF_PINS *pins,
                                       uint64_t *out_oob_ref_file_no)
{
  ibb_tblspc_entry *e= static_cast<ibb_tblspc_entry *>
    (lf_hash_search(&hash, pins, &file_no, sizeof(file_no)));
  if (e)
  {
    *out_oob_ref_file_no= e->oob_ref_file_no.load(std::memory_order_relaxed);
    lf_hash_search_unpin(pins);
    return false;
  }

  *out_oob_ref_file_no= ~(uint64_t)0;
  byte *page_buf= static_cast<byte *>(ut_malloc(ibb_page_size, mem_key_binlog));
  if (!page_buf)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), ibb_page_size);
    return true;
  }
  char filename[OS_FILE_MAX_PATH];
  binlog_name_make(filename, file_no);
  File fh= my_open(filename, O_RDONLY | O_BINARY, MYF(0));
  if (fh < (File)0)
  {
    my_error(ER_ERROR_ON_READ, MYF(0), filename, my_errno);
    ut_free(page_buf);
    return true;
  }
  int res= crc32_pread_page(fh, page_buf, 0, MYF(0));
  my_close(fh, MYF(0));
  if (res <= 0)
  {
    ut_free(page_buf);
    my_error(ER_ERROR_ON_READ, MYF(0), filename, my_errno);
    return true;
  }
  binlog_header_data header;
  fsp_binlog_extract_header_page(page_buf, &header);
  ut_free(page_buf);
  if (header.is_invalid || header.is_empty)
  {
    my_error(ER_FILE_CORRUPT, MYF(0), filename);
    return true;
  }
  *out_oob_ref_file_no= header.oob_ref_file_no;
  if (ibb_record_in_file_hash(file_no, header.oob_ref_file_no,
                              header.xa_ref_file_no, pins))
    return true;

  return false;
}


/*
  Check if a file_no contains oob data that is needed by an active
  (ie. not committed) transaction. This is seen simply as having refcount
  greater than 0.
*/
bool
ibb_file_oob_refs::get_oob_ref_in_use(uint64_t file_no, LF_PINS *pins)
{
  ibb_tblspc_entry *e= static_cast<ibb_tblspc_entry *>
    (lf_hash_search(&hash, pins, &file_no, sizeof(file_no)));
  if (!e)
    return false;

  uint64_t refcnt= e->oob_refs.load(std::memory_order_relaxed);
  lf_hash_search_unpin(pins);
  return refcnt > 0;
}


/*
  Check if there are any of the in-use binlog files that have refcount > 0
  (meaning any references to oob data from active transactions).
  Any such references must prevent a RESET MASTER, as otherwise they could
  be committed with OOB references pointing to garbage data.
*/
bool
ibb_file_oob_refs::check_any_oob_ref_in_use(uint64_t start_file_no,
                                            uint64_t end_file_no,
                                            LF_PINS *lf_pins)
{
  if (unlikely(start_file_no == ~(uint64_t)0)
      || unlikely(end_file_no == ~(uint64_t)0))
    return false;

  for (uint64_t file_no= start_file_no; file_no <= end_file_no; ++file_no)
  {
    if (get_oob_ref_in_use(file_no, lf_pins))
      return true;
  }
  return false;
}


bool
ibb_record_in_file_hash(uint64_t file_no, uint64_t oob_ref, uint64_t xa_ref,
                         LF_PINS *in_pins)
{
  bool err= false;
  LF_PINS *pins= in_pins ? in_pins : lf_hash_get_pins(&ibb_file_hash.hash);
  if (!pins)
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return true;
  }
  ibb_tblspc_entry entry;
  entry.file_no= file_no;
  entry.oob_refs.store(0, std::memory_order_relaxed);
  entry.xa_refs.store(0, std::memory_order_relaxed);
  entry.oob_ref_file_no.store(oob_ref, std::memory_order_relaxed);
  entry.xa_ref_file_no.store(xa_ref, std::memory_order_relaxed);
  int res= lf_hash_insert(&ibb_file_hash.hash, pins, &entry);
  if (res)
  {
    ut_ad(res < 0 /* Should not get unique violation, never insert twice */);
    sql_print_error("InnoDB: Could not initialize in-memory structure for "
                    "binlog tablespace file number %" PRIu64 ", %s", file_no,
                    (res < 0 ? "out of memory" : "internal error"));
    err= true;
  }
  if (!in_pins)
    lf_hash_put_pins(pins);
  return err;
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
fsp_binlog_extract_header_page(const byte *page_buf,
                              binlog_header_data *out_header_data) noexcept
{
  uint32_t magic= uint4korr(page_buf);
  uint32_t vers_major= uint4korr(page_buf + 8);
  const uint32_t payload= IBB_HEADER_PAGE_SIZE - BINLOG_PAGE_CHECKSUM;
  uint32_t crc32= uint4korr(page_buf + payload);
  out_header_data->is_empty= false;
  out_header_data->is_invalid= false;
  if (crc32 != my_crc32c(0, page_buf, payload) ||
      magic != IBB_MAGIC || vers_major > IBB_FILE_VERS_MAJOR)
  {
    if (page_buf[0] == 0 &&
        0 == memcmp(page_buf, page_buf+1, IBB_HEADER_PAGE_SIZE - 1))
      out_header_data->is_empty= true;
    else
      out_header_data->is_invalid= true;
    return;
  }
  out_header_data->page_size_shift= uint4korr(page_buf + 4);
  out_header_data->vers_major= vers_major;
  out_header_data->vers_minor= uint4korr(page_buf + 12);
  out_header_data->file_no= uint8korr(page_buf + 16);
  out_header_data-> page_count= uint8korr(page_buf + 24);
  out_header_data-> start_lsn= uint8korr(page_buf + 32);
  out_header_data-> diff_state_interval= uint8korr(page_buf + 40);
  out_header_data->oob_ref_file_no= uint8korr(page_buf + 48);
  out_header_data->xa_ref_file_no= uint8korr(page_buf + 56);
}


void
fsp_log_binlog_write(mtr_t *mtr, fsp_binlog_page_entry *page,
                     uint64_t file_no, uint32_t page_no,
                     uint32_t page_offset, uint32_t len)
{
  ut_ad(page->latched);
  if (page_offset + len >= ibb_page_size - BINLOG_PAGE_DATA_END)
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
  page_id_t page_id(LOG_BINLOG_ID_0 | static_cast<uint32_t>(file_no & 1),
                    page_no);
  mtr->write_binlog(page_id, (uint16_t)page_offset,
                    page_offset + &page->page_buf()[0], len);
}


void
fsp_log_header_page(mtr_t *mtr, fsp_binlog_page_entry *page, uint64_t file_no,
                    uint32_t len)
  noexcept
{
  page->complete= true;
  page_id_t page_id(LOG_BINLOG_ID_0 | static_cast<uint32_t>(file_no & 1), 0);
  mtr->write_binlog(page_id, 0, &page->page_buf()[0], len);
}


/**
  Initialize the InnoDB implementation of binlog.
  Note that we do not create or open any binlog tablespaces here.
  This is only done if InnoDB binlog is enabled on the server level.
*/
dberr_t
fsp_binlog_init()
{
  mysql_mutex_init(fsp_active_binlog_mutex_key, &active_binlog_mutex, nullptr);
  pthread_cond_init(&active_binlog_cond, nullptr);
  mysql_mutex_init(fsp_binlog_durable_mutex_key, &binlog_durable_mutex, nullptr);
  mysql_cond_init(fsp_binlog_durable_cond_key, &binlog_durable_cond, nullptr);
  mysql_mutex_record_order(&binlog_durable_mutex, &active_binlog_mutex);

  ibb_file_hash.init();
  binlog_page_fifo= new fsp_binlog_page_fifo();
  if (UNIV_UNLIKELY(!binlog_page_fifo))
  {
    sql_print_error("InnoDB: Could not allocate memory for the page fifo, "
                    "cannot proceed");
    return DB_OUT_OF_MEMORY;
  }
  binlog_page_fifo->start_flush_thread();
  return DB_SUCCESS;
}


void
fsp_binlog_shutdown()
{
  binlog_page_fifo->stop_flush_thread();
  delete binlog_page_fifo;
  ibb_file_hash.destroy();
  mysql_cond_destroy(&binlog_durable_cond);
  mysql_mutex_destroy(&binlog_durable_mutex);
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
  uint32_t size=
    binlog_page_fifo->size_in_pages(file_no) << ibb_page_size_shift;
  /* release_tablespace() will fdatasync() the file first. */
  binlog_page_fifo->release_tablespace(file_no);
  /*
    Durably sync the redo log. This simplifies things a bit, as then we know
    that we will not need to discard any data from an old binlog file during
    recovery, at most from the latest two existing files.
  */
  log_buffer_flush_to_disk(true);
  uint64_t end_offset=
    binlog_cur_end_offset[file_no & 3].load(std::memory_order_relaxed);
  binlog_cur_end_offset[file_no & 3].store(size, std::memory_order_relaxed);
  /*
    Wait for the last record in the file to be marked durably synced to the
    (redo) log. We already ensured that the record is durable with the above
    call to log_buffer_flush_to_disk(); this way, we ensure that the update
    of binlog_cur_durable_offset[] happens correctly through the
    ibb_pending_lsn_fifo, so that the current durable position will be
    consistent with a recorded LSN, and a reader will not see EOF in the
    middle of a record.
  */
  uint64_t dur_offset=
    binlog_cur_durable_offset[file_no & 3].load(std:: memory_order_relaxed);
  if (dur_offset < end_offset)
    ibb_wait_durable_offset(file_no, end_offset);

  return DB_SUCCESS;
}


/**
  Open an existing tablespace. The filehandle fh is taken over by the tablespace
  (or closed in case of error).
*/
bool
fsp_binlog_open(const char *file_name, pfs_os_file_t fh,
                uint64_t file_no, size_t file_size,
                uint32_t init_page, byte *partial_page)
{
  const uint32_t page_size= (uint32_t)ibb_page_size;
  const uint32_t page_size_shift= ibb_page_size_shift;

  os_offset_t binlog_size= innodb_binlog_size_in_pages << ibb_page_size_shift;
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
dberr_t fsp_binlog_tablespace_create(uint64_t file_no, uint32_t size_in_pages,
                                     LF_PINS *pins)
{
	pfs_os_file_t	fh;
	bool		ret;

	if(srv_read_only_mode)
		return DB_ERROR;

        char name[OS_FILE_MAX_PATH];
        binlog_name_make(name, file_no);

	os_file_create_subdirs_if_needed(name);

try_again:
	fh = os_file_create(
		innodb_data_file_key, name,
		OS_FILE_CREATE, OS_DATA_FILE, srv_read_only_mode, &ret);

	if (!ret) {
		os_file_close(fh);
		return DB_ERROR;
	}

	/* We created the binlog file and now write it full of zeros */
	if (!os_file_set_size(name, fh,
			      os_offset_t{size_in_pages} << ibb_page_size_shift)
            ) {
		char buf[MYSYS_STRERROR_SIZE];
		ulong wait_sec= MY_WAIT_FOR_USER_TO_FIX_PANIC;
		DBUG_EXECUTE_IF("ib_alloc_file_disk_full",
				wait_sec= 2;);
		my_strerror(buf, sizeof(buf), errno);
		sql_print_error("InnoDB: Unable to allocate file %s: \"%s\". "
				"Waiting %lu seconds before trying again...",
				name, buf, wait_sec);
		os_file_close(fh);
		os_file_delete(innodb_data_file_key, name);
		my_sleep(wait_sec * 1000000);
		goto try_again;
	}

        /*
          Enter an initial entry in the hash for this binlog tablespace file.
          It will be later updated with the appropriate values when the file
          first gets used and the header page is written.
        */
        ibb_record_in_file_hash(file_no, ~(uint64_t)0,~(uint64_t)0, pins);

        binlog_page_fifo->create_tablespace(file_no, size_in_pages);
        os_file_close(fh);

	return DB_SUCCESS;
}


/**
  Write out a binlog record.
  Split into chucks that each fit on a page.
  The data for the record is provided by a class derived from chunk_data_base.

  As a special case, a record write of type FSP_BINLOG_TYPE_FILLER does not
  write any record, but moves to the next tablespace and writes the initial
  GTID state record, used for FLUSH BINARY LOGS.

  Returns a pair of {file_no, offset} marking the start of the record written.
*/
std::pair<uint64_t, uint64_t>
fsp_binlog_write_rec(chunk_data_base *chunk_data, mtr_t *mtr, byte chunk_type,
                     LF_PINS *pins)
{
  uint32_t page_size= (uint32_t)ibb_page_size;
  uint32_t page_size_shift= ibb_page_size_shift;
  const uint32_t page_end= page_size - BINLOG_PAGE_DATA_END;
  uint32_t page_no= binlog_cur_page_no;
  uint32_t page_offset= binlog_cur_page_offset;
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
    if (page_offset == BINLOG_PAGE_DATA) {
      ut_ad(!block);
      uint32_t file_size_in_pages= binlog_page_fifo->size_in_pages(file_no);
      if (UNIV_UNLIKELY(page_no >= file_size_in_pages)) {
        /*
          Signal to the pre-allocation thread that this tablespace has been
          written full, so that it can be closed and a new one pre-allocated
          in its place. Then wait for a new tablespace to be pre-allocated that
          we can use.

          The normal case is that the next tablespace is already pre-allocated
          and available; binlog tablespace N is active while (N+1) is being
          pre-allocated. Only under extreme I/O pressure should we need to
          stall here.
        */
        ut_ad(!pending_prev_end_offset);
        pending_prev_end_offset= page_no << page_size_shift;
        mysql_mutex_lock(&active_binlog_mutex);
        while (last_created_binlog_file_no <= file_no) {
          my_cond_wait(&active_binlog_cond, &active_binlog_mutex.m_mutex);
        }

        ++file_no;
        file_size_in_pages= binlog_page_fifo->size_in_pages(file_no);
        binlog_cur_durable_offset[file_no & 3].store(0, std::memory_order_relaxed);
        binlog_cur_end_offset[file_no & 3].store(0, std::memory_order_relaxed);
        pthread_cond_signal(&active_binlog_cond);
        mysql_mutex_unlock(&active_binlog_mutex);
        binlog_cur_page_no= page_no= 0;
        current_binlog_state_interval=
          (uint64_t)(innodb_binlog_state_interval >> page_size_shift);
      }

      /* Write the header page at the start of a binlog tablespace file. */
      if (page_no == 0)
      {
        /* Active is moving to next file, so check if oob refcount of previous
           file is zero.
        */
        if (UNIV_LIKELY(file_no > 0))
          ibb_file_hash.do_zero_refcnt_action(file_no - 1, pins, true);

        lsn_t start_lsn= log_get_lsn();
        bool err= ibb_write_header_page(mtr, file_no, file_size_in_pages,
                                        start_lsn,
                                        current_binlog_state_interval, pins);
        ut_a(!err);
        page_no= 1;
      }

      /* Must be a power of two. */
      ut_ad(current_binlog_state_interval == 0 ||
            current_binlog_state_interval ==
            (uint64_t)1 << (63 - my_nlz(current_binlog_state_interval)));

      if (page_no == 1 ||
          0 == (page_no & (current_binlog_state_interval - 1))) {
        if (page_no == 1) {
          bool err;
          rpl_binlog_state_base *binlog_state= &binlog_full_state;
          binlog_diff_state.reset_nolock();
          if (UNIV_UNLIKELY(file_no == 0 && page_no == 1) &&
              (binlog_full_state.count_nolock() == 1))
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
            binlog_full_state.get_gtid_list_nolock(&singleton_gtid, 1);
            if (singleton_gtid.seq_no == 1)
              binlog_state= &binlog_diff_state;  // Conveniently empty
          }
          err= binlog_gtid_state(binlog_state, mtr, block, page_no,
                                 page_offset, file_no);
          ut_a(!err);
        } else {
          bool err= binlog_gtid_state(&binlog_diff_state, mtr, block, page_no,
                                      page_offset, file_no);
          ut_a(!err);
        }
        if (UNIV_UNLIKELY(!block))
        {
          /*
            This happens in the special case where binlog_gtid_state() exactly
            ends on the page boundary.
            We must create the next page then to write our chunk into, _except_
            for the special chunk_type FSP_BINLOG_TYPE_FILLER, which writes
            noting except the gtid state record! In this case we must exit the
            loop so we don't leave an empty page created.
          */
          if (UNIV_UNLIKELY(chunk_type == FSP_BINLOG_TYPE_FILLER))
            break;
          block= binlog_page_fifo->create_page(file_no, page_no);
        }
        ut_ad(block);
      } else
        block= binlog_page_fifo->create_page(file_no, page_no);
    } else {
      block= binlog_page_fifo->get_page(file_no, page_no);
    }

    ut_ad(page_offset < page_end);
    uint32_t page_remain= page_end - page_offset;
    byte *ptr= page_offset + &block->page_buf()[0];
    if (page_remain < 4) {
      /* Pad the remaining few bytes, and move to next page. */
      if (UNIV_LIKELY(page_remain > 0))
      {
        memset(ptr, FSP_BINLOG_TYPE_FILLER,  page_remain);
        fsp_log_binlog_write(mtr, block, file_no, page_no, page_offset,
                             page_remain);
      }
      binlog_page_fifo->release_page_mtr(block, mtr);
      block= nullptr;
      ++page_no;
      page_offset= BINLOG_PAGE_DATA;
      DBUG_EXECUTE_IF("pause_binlog_write_after_release_page",
                      my_sleep(200000););
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

    fsp_log_binlog_write(mtr, block, file_no, page_no, page_offset, size + 3);
    cont_flag= FSP_BINLOG_FLAG_CONT;
    if (page_remain == 0) {
      binlog_page_fifo->release_page_mtr(block, mtr);
      block= nullptr;
      page_offset= BINLOG_PAGE_DATA;
      ++page_no;
      DBUG_EXECUTE_IF("pause_binlog_write_after_release_page",
                      if (!size_last.second)
                        my_sleep(200000);
                      );
    } else {
      page_offset+= size+3;
    }
    if (size_last.second)
      break;
    ut_ad(!block);
    if (UNIV_UNLIKELY(block != nullptr))
    {
      /*
        Defensive coding, just to not leave a page latch which would hang the
        entire server hard. This code should not be reachable.
      */
      binlog_page_fifo->release_page_mtr(block, mtr);
      block= nullptr;
    }
  }
  if (block)
    binlog_page_fifo->release_page_mtr(block, mtr);
  binlog_cur_page_no= page_no;
  binlog_cur_page_offset= page_offset;
  binlog_cur_end_offset[file_no & 3].store
    (((uint64_t)page_no << page_size_shift) + page_offset,
     std::memory_order_relaxed);
  if (UNIV_UNLIKELY(pending_prev_end_offset != 0))
  {
    mysql_mutex_lock(&binlog_durable_mutex);
    mysql_mutex_lock(&active_binlog_mutex);
    binlog_cur_end_offset[(file_no-1) & 3].store(pending_prev_end_offset,
                                                 std::memory_order_relaxed);
    active_binlog_file_no.store(file_no, std::memory_order_release);
    pthread_cond_signal(&active_binlog_cond);
    mysql_mutex_unlock(&active_binlog_mutex);
    mysql_mutex_unlock(&binlog_durable_mutex);
  }
  return {start_file_no, start_offset};
}


/**
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
  mtr_t mtr{nullptr};

  mysql_mutex_lock(&purge_binlog_mutex);

  binlog_page_fifo->lock_wait_for_idle();
  File fh= binlog_page_fifo->get_fh(file_no);
  if (fh == (File)-1)
  {
    binlog_page_fifo->unlock();
    mysql_mutex_unlock(&purge_binlog_mutex);
    return true;
  }

  if (my_chsize(fh, ((uint64_t)page_no + 1) << ibb_page_size_shift, 0,
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

  LF_PINS *lf_pins= lf_hash_get_pins(&ibb_file_hash.hash);
  ut_a(lf_pins);
  uint32_t page_offset= binlog_cur_page_offset;
  if (page_offset > BINLOG_PAGE_DATA ||
      page_offset < ibb_page_size - BINLOG_PAGE_DATA_END)
  {
  /*
    If we are not precisely the end of a page, fill up that page with a dummy
    record. Otherwise the zeros at the end of the page would be detected as
    end-of-file of the entire binlog.
  */
    mtr.start();
    fsp_binlog_write_rec(&dummy_data, &mtr, FSP_BINLOG_TYPE_DUMMY, lf_pins);
    mtr.commit();
  }

  if (page_no + 1 < binlog_page_fifo->size_in_pages(file_no))
  {
    binlog_page_fifo->truncate_file_size(file_no, page_no + 1);
    size_t reclaimed= (binlog_page_fifo->size_in_pages(file_no) - (page_no + 1))
      << ibb_page_size_shift;
    if (UNIV_LIKELY(total_binlog_used_size >= reclaimed))
      total_binlog_used_size-= reclaimed;
    else
      ut_ad(0);
  }

  /* Flush out all pages in the (now filled-up) tablespace. */
  binlog_page_fifo->flush_up_to(file_no, page_no);

  /*
    Load the binlog GTID state from the server layer (in case it changed
    due to FLUSH BINARY LOGS DELETE_DOMAIN_ID).
  */
  load_global_binlog_state(&binlog_full_state);

  mysql_mutex_unlock(&purge_binlog_mutex);

  /*
    Now get a new GTID state record written to the next binlog tablespace.
    This ensures that the new state (in case of DELETE_DOMAIN_ID) will be
    persisted across a server restart.
  */
  mtr.start();
  fsp_binlog_write_rec(&dummy_data, &mtr, FSP_BINLOG_TYPE_FILLER, lf_pins);
  mtr.commit();
  lf_hash_put_pins(lf_pins);
  log_buffer_flush_to_disk(srv_flush_log_at_trx_commit & 1);
  ibb_pending_lsn_fifo.add_to_fifo(mtr.commit_lsn(), file_no+1,
    binlog_cur_end_offset[(file_no + 1) & 3].load(std::memory_order_relaxed));

  return false;
}


binlog_chunk_reader::binlog_chunk_reader(std::atomic<uint64_t> *limit_offset_)
  : s { 0, ~(uint64_t)0, 0, 0, 0, 0, FSP_BINLOG_TYPE_FILLER, false, false },
    stop_file_no(~(uint64_t)0), page_ptr(0), cur_block(0), page_buffer(nullptr),
    limit_offset(limit_offset_), cur_file_handle((File)-1),
    skipping_partial(false)
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


/**
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
    uint64_t offset= (s.page_no << ibb_page_size_shift) | s.in_page_offset;
    uint64_t active= active2;
    uint64_t end_offset=
      limit_offset[s.file_no & 3].load(std::memory_order_acquire);
    /*
      Can be different from end_offset if limit_offset is the
      binlog_cur_durable_offset.
    */
    uint64_t real_end_offset=
      binlog_cur_end_offset[s.file_no & 3].load(std::memory_order_acquire);
    if (s.file_no > active || UNIV_UNLIKELY(active == ~(uint64_t)0)
        || UNIV_UNLIKELY(s.file_no > stop_file_no))
    {
      ut_ad(s.page_no == 1 || s.file_no > stop_file_no);
      ut_ad(s.in_page_offset == 0 || s.file_no > stop_file_no);
      /*
        Allow a reader that reached the very end of the active binlog file to
        have moved ahead early to the start of the coming binlog file.
      */
      return CHUNK_READER_EOF;
    }

    if (s.file_no + 1 >= active) {
      /* Check if we should read from the buffer pool or from the file. */
      if (end_offset != ~(uint64_t)0 && offset < end_offset)
        block= binlog_page_fifo->get_page(s.file_no, s.page_no);
      active2= active_binlog_file_no.load(std::memory_order_acquire);
      if (UNIV_UNLIKELY(active2 != active)) {
        /*
          The active binlog file changed while we were processing; we might
          have gotten invalid end_offset or a buffer pool page from a wrong
          tablespace. So just try again.
        */
        if (block)
          binlog_page_fifo->release_page(block);
        continue;
      }
      cur_end_offset= end_offset;
      if (offset >= end_offset) {
        ut_ad(!block);
        if (s.file_no == active) {
          /* Reached end of the currently active binlog file -> EOF. */
          return CHUNK_READER_EOF;
        }
        ut_ad(s.file_no + 1 == active);
        if (offset < real_end_offset)
        {
          /*
            Reached durable limit of active-1 _and_ not at the end of the
            file where we should move to the next one.
          */
          return CHUNK_READER_EOF;
        }
      }
      if (block) {
        cur_block= block;
        page_ptr= block->page_buf();
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
    if (s.file_no + 1 >= active)
      cur_end_offset= end_offset;
    else
      cur_end_offset= cur_file_length;

    if (offset >= cur_file_length) {
      /* End of this file, move to the next one. */
  goto_next_file:
      if (UNIV_UNLIKELY(s.file_no >= stop_file_no))
        return CHUNK_READER_EOF;
      if (cur_file_handle >= (File)0)
      {
        my_close(cur_file_handle, MYF(0));
        cur_file_handle= (File)-1;
        cur_file_length= ~(uint64_t)0;
      }
      ++s.file_no;
      s.page_no= 1;  /* Skip the header page. */
      continue;
    }

    int res= crc32_pread_page(cur_file_handle, page_buffer, s.page_no,
                              MYF(MY_WME));
    if (res < 0)
      return CHUNK_READER_ERROR;
    if (res == 0)
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
      if (s.in_record && s.file_no <= stop_file_no)
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
    /*
      This code gives warning "comparison of unsigned expression in ‘< 0’ is
      always false" when BINLOG_PAGE_DATA is 0.

      So use a static assert for now; if it ever triggers, replace it with this
      code:

       if (s.in_page_offset < BINLOG_PAGE_DATA)
         s.in_page_offset= BINLOG_PAGE_DATA;
    */
    if (0)
      static_assert(BINLOG_PAGE_DATA == 0,
                    "Replace static_assert with code from above comment");

    /* Check for end-of-file. */
    if ((s.page_no << ibb_page_size_shift) + s.in_page_offset >= cur_end_offset)
      return sofar;

    if (s.in_page_offset >= ibb_page_size - (BINLOG_PAGE_DATA_END + 3) ||
             page_ptr[s.in_page_offset] == FSP_BINLOG_TYPE_FILLER)
    {
      ut_ad(s.in_page_offset >= ibb_page_size - BINLOG_PAGE_DATA_END ||
            page_ptr[s.in_page_offset] == FSP_BINLOG_TYPE_FILLER);
      goto go_next_page;
    }

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
    s.rec_start_file_no= s.file_no;
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

  if (s.in_page_offset >= ibb_page_size - (BINLOG_PAGE_DATA_END + 3) &&
      (s.page_no << ibb_page_size_shift) + s.in_page_offset < cur_end_offset)
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
        (s.page_no << ibb_page_size_shift) >= cur_file_length)
    {
      /* Move to the next file. */
      my_close(cur_file_handle, MYF(0));
      cur_file_handle= (File)-1;
      cur_file_length= ~(uint64_t)0;
      ++s.file_no;
      s.page_no= 1;  /* Skip the header page. */
    }
  }

  if (sofar > 0 && (!multipage || !s.in_record))
    return sofar;

  goto read_more_data;
}


int
binlog_chunk_reader::find_offset_in_page(uint32_t off)
{
  if (!page_ptr)
  {
    enum chunk_reader_status res= fetch_current_page();
    if (res == CHUNK_READER_EOF)
      return 0;
    else if (res == CHUNK_READER_ERROR)
      return -1;
  }

  /*
    Skip ahead in the page until we come to the first chunk boundary that
    is at or later than the requested offset.
  */
  s.in_page_offset= 0;
  s.chunk_len= 0;
  s.chunk_read_offset= 0;
  s.chunk_type= FSP_BINLOG_TYPE_FILLER;
  s.skip_current= 0;
  s.in_record= 0;
  while (s.in_page_offset < off &&
         s.in_page_offset < cur_end_offset &&
         s.in_page_offset < ibb_page_size)
  {
    byte type= page_ptr[s.in_page_offset];
    if (type == 0 || type == FSP_BINLOG_TYPE_FILLER)
      break;
    uint32_t chunk_len= page_ptr[s.in_page_offset + 1] |
      ((uint32_t)page_ptr[s.in_page_offset + 2] << 8);
    s.in_page_offset+= std::min(3 + chunk_len, (uint32_t)ibb_page_size);
  }
  return 0;
}


/**
  Read the header page of the current binlog file_no.
  Returns:
    1 Header page found and returned.
    0 EOF, no header page found (ie. file is empty / nothing is durable yet).
   -1 Error.
*/
int
binlog_chunk_reader::get_file_header(binlog_header_data *out_header)
{
  seek(current_file_no(), 0);
  enum chunk_reader_status res= fetch_current_page();
  if (UNIV_UNLIKELY(res != CHUNK_READER_FOUND))
    return res == CHUNK_READER_EOF ? 0 : -1;
  fsp_binlog_extract_header_page(page_ptr, out_header);
  if (out_header->is_invalid || out_header->is_empty)
    return -1;
  return 1;
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
    file_no, ~(uint64_t)0, (uint32_t)(offset >> ibb_page_size_shift),
    (uint32_t)(offset & (ibb_page_size - 1)),
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
      the page from the file in this case on next read, as data might be added
      to the page.
    */
    page_ptr= nullptr;
  }
}


bool binlog_chunk_reader::data_available()
{
  if (!end_of_record())
    return true;
  uint64_t active= active_binlog_file_no.load(std::memory_order_acquire);
  if (UNIV_UNLIKELY(active == ~(uint64_t)0))
    return false;
  uint64_t end_offset;
  for (;;)
  {
    if (active > s.file_no + 1)
      return true;
    end_offset= limit_offset[s.file_no & 3].load(std::memory_order_acquire);
    uint64_t active2= active_binlog_file_no.load(std::memory_order_acquire);
    if (active2 == active)
      break;
    /* Active moved while we were checking, try again. */
    active= active2;
  }
  uint64_t offset= (s.page_no << ibb_page_size_shift) | s.in_page_offset;
  if (offset < end_offset)
    return true;

  ut_ad(s.file_no + 1 == active || s.file_no == active);
  ut_ad(offset == end_offset || (offset == ibb_page_size && end_offset == 0));
  return false;
}


bool
binlog_chunk_reader::is_before_pos(uint64_t file_no, uint64_t offset)
{
  if (s.file_no < file_no)
    return true;
  if (s.file_no > file_no)
    return false;
  uint64_t own_offset= (s.page_no << ibb_page_size_shift) | s.in_page_offset;
  if (own_offset < offset)
    return true;
  return false;
}
