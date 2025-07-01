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
@file include/fsp_binlog.h
InnoDB implementation of binlog.
*******************************************************/

#ifndef fsp_binlog_h
#define fsp_binlog_h

#include <utility>
#include <atomic>

#include "lf.h"

#include "univ.i"
#include "mtr0mtr.h"


struct chunk_data_base;
struct binlog_header_data;

/* 4-byte "magic" identifying InnoDB binlog file (little endian). */
static constexpr uint32_t IBB_MAGIC= 0x010dfefe;
static constexpr uint32_t IBB_FILE_VERS_MAJOR= 1;
static constexpr uint32_t IBB_FILE_VERS_MINOR= 0;

/*
  The size of the header page that is stored in the first page of a file.
  This is the smallest page size that can be used in a backwards compatible
  way. Having a fixed-size small header page means we can get the real page
  size of the file from the header page, but still be able to checksum the
  header page without relying on unchecked page size field to compute the
  checksum.

  (The remainder of the header page is just unused or could potentially
  later be used for other data as needed).
*/
static constexpr uint32_t IBB_HEADER_PAGE_SIZE= 512;
static constexpr uint32_t IBB_PAGE_SIZE_MIN= IBB_HEADER_PAGE_SIZE;
static constexpr uint32_t IBB_PAGE_SIZE_MAX= 65536;

/** Store crc32 checksum at the end of the page */
#define BINLOG_PAGE_CHECKSUM 4

#define BINLOG_PAGE_DATA 0
#define BINLOG_PAGE_DATA_END BINLOG_PAGE_CHECKSUM


enum fsp_binlog_chunk_types {
  /* Zero means no data, effectively EOF. */
  FSP_BINLOG_TYPE_EMPTY= 0,
  /* A binlogged committed event group. */
  FSP_BINLOG_TYPE_COMMIT= 1,
  /* A binlog GTID state record. */
  FSP_BINLOG_TYPE_GTID_STATE= 2,
  /* Out-of-band event group data. */
  FSP_BINLOG_TYPE_OOB_DATA= 3,
  /* Dummy record, use to fill remainder of page (eg. FLUSH BINARY LOGS). */
  FSP_BINLOG_TYPE_DUMMY= 4,
  /* Must be one more than the last type. */
  FSP_BINLOG_TYPE_END,

  /* Padding data at end of page. */
  FSP_BINLOG_TYPE_FILLER= 0xff
};

/*
  Bit set on the chunk type for a continuation chunk, when data needs to be
  split across pages.
*/
static constexpr uint32_t FSP_BINLOG_FLAG_BIT_CONT= 7;
static constexpr uint32_t FSP_BINLOG_FLAG_CONT= (1 << FSP_BINLOG_FLAG_BIT_CONT);
/*
  Bit set on the chunk type for the last chunk (no continuation chunks
  follow)
*/
static constexpr uint32_t FSP_BINLOG_FLAG_BIT_LAST= 6;
static constexpr uint32_t FSP_BINLOG_FLAG_LAST= (1 << FSP_BINLOG_FLAG_BIT_LAST);
static constexpr uint32_t FSP_BINLOG_TYPE_MASK=
  ~(FSP_BINLOG_FLAG_CONT | FSP_BINLOG_FLAG_LAST);

/*
  These are the chunk types that are allowed to occur in the middle of
  another record.
*/
static constexpr uint64_t ALLOWED_NESTED_RECORDS=
  /* GTID STATE at start of page can occur in the middle of other record. */
  ((uint64_t)1 << FSP_BINLOG_TYPE_GTID_STATE) |
  /* DUMMY data at tablespace end can occur in the middle of other record. */
  ((uint64_t)1 << FSP_BINLOG_TYPE_DUMMY)
  ;
/* Ensure that all types fit in the ALLOWED_NESTED_RECORDS bitmask. */
static_assert(FSP_BINLOG_TYPE_END <= 8*sizeof(ALLOWED_NESTED_RECORDS),
              "Binlog types must be <64 to fit "
              "in ALLOWED_NESTED_RECORDS bitmask");


/*
  The object representing a binlog page that is not yet flushed to disk.
  At the end of the object is an additionally allocated byte buffer of
  size ibb_page_size, ie. the page buffer containing the data in the page.

  The LATCHED count is the number of current writers and readers of the page
  (the page cannot be flushed and freed until this drops to zero).

  The flag LAST_PAGE is set for the very last page in a tablespace file,
  used to hold this page latched until the end of a mini-transaction.

  The flag COMPLETE is set when the writer has written the last byte of the
  page (a page cannot be freed until it is complete, and will normally not be
  flushed unless required for an InnoDB log checkpoint).

  The flag FLUSHED_CLEAN is set if a (partial) page has been flushed to disk,
  and cleared again by a writer when more data is added to the page.
*/
struct fsp_binlog_page_entry {
  uint32_t latched;
  /* Flag set for the last page in a file. */
  bool last_page;
  /*
    Flag set when the page has been filled, no more data will be added and
    it is safe to write out to disk and remove from the FIFO.
  */
  bool complete;
  /*
    Flag set when the page is not yet complete, but all data added so far
    have been written out to the file. So the page should not be written
    again (until more data is added), but nor can it be removed from the
    FIFO yet.
  */
  bool flushed_clean;
  /*
    Flag set when the page is not yet complete, but nevertheless waiting to be
    flushed to disk (eg. due to InnoDB checkpointing). Used to avoid waking up
    the flush thread on every release of a last partial page in the file
    when it is not needed.
  */
  bool pending_flush;

  byte *page_buf() { return (byte *)this + sizeof(fsp_binlog_page_entry); }
};


/*
  A page FIFO, as a lower-level alternative to the buffer pool used for full
  tablespaces.

  Since binlog files are written strictly append-only, we can simply add new
  pages at the end and flush them from the beginning.

  Some attempt is made to get reasonable scalability of the page fifo (even
  though it is still protected by a global mutex that could potentially be
  contended between writers and readers). The mutex is only held shortly;
  a "latch" count in each page marks when there are active readers or writers
  preventing page flush and free. Thus readers and writers can access a page
  concurrently. File write operations/syscalls are done outside of holding the
  mutex, and a freelist is used to likewise avoid most malloc/free.
*/
class fsp_binlog_page_fifo {
public:
  /*
    Allow at most 1/N of the pages in one binlog file will be kept in-memory
    on the free list of page buffers.
  */
  static constexpr uint64_t MAX_FREE_BUFFERS_FRAC= 4;

  struct page_list {
    fsp_binlog_page_entry **entries;
    size_t allocated_entries;
    size_t used_entries;
    size_t first_entry;
    uint32_t first_page_no;
    uint32_t size_in_pages;
    File fh;

    fsp_binlog_page_entry *&entry_at(size_t idx)
    {
      idx+= first_entry;
      if (idx >= allocated_entries)
        idx-= allocated_entries;
      ut_ad(idx < allocated_entries);
      return entries[idx];
    }

  };
private:
  mysql_mutex_t m_mutex;
  pthread_cond_t m_cond;
  std::thread flush_thread_obj;

  /*
    The first_file_no is the first valid file in the fifo. The other entry in
    the fifo holds (first_file_no+1) if it is not empty.
    If first_file_no==~0, then there are no files in the fifo (initial state
    just after construction).
  */
  uint64_t first_file_no;
  page_list fifos[2];
  /*
    Free list for page objects, to avoid repeated aligned_alloc().
    Each object is allocated as a byte array of size
    sizeof(fsp_binlog_page_entry) + ibb_page_size, holding the
    fsp_binlog_page_entry object and the page buffer just after it.
    When on the freelist, instead just the first sizeof(byte *) bytes store
    a simple `next' pointer.
  */
  size_t free_buffers;
  byte *freelist;
  /* Temporary overflow of freelist, to be freed after mutex is unlocked. */
  byte *to_free_list;
  bool flushing;
  bool flush_thread_started;
  bool flush_thread_end;

private:
  fsp_binlog_page_entry *get_entry(uint64_t file_no, uint64_t page_no,
                                   uint32_t latch, bool completed, bool clean);
  void release_entry(uint64_t file_no, uint64_t page_no);

public:
  fsp_binlog_page_fifo();
  ~fsp_binlog_page_fifo();
  void reset();
  void start_flush_thread();
  void stop_flush_thread();
  void flush_thread_run();
  void lock_wait_for_idle();
  void unlock() { mysql_mutex_unlock(&m_mutex); }
  void unlock_with_delayed_free();
  void create_tablespace(uint64_t file_no, uint32_t size_in_pages,
                         uint32_t init_page= ~(uint32_t)0,
                         byte *partial_page= nullptr);
  void release_tablespace(uint64_t file_no);
  void free_page_list(uint64_t file_no);
  fsp_binlog_page_entry *create_page(uint64_t file_no, uint32_t page_no);
  fsp_binlog_page_entry *get_page(uint64_t file_no, uint32_t page_no);
  void release_page(fsp_binlog_page_entry *page);
  void release_page_mtr(fsp_binlog_page_entry *page, mtr_t *mtr);
  bool flush_one_page(uint64_t file_no, bool force);
  void flush_up_to(uint64_t file_no, uint32_t page_no);
  void do_fdatasync(uint64_t file_no);
  File get_fh(uint64_t file_no);
  uint32_t size_in_pages(uint64_t file_no) {
    return fifos[file_no & 1].size_in_pages;
  }
  void truncate_file_size(uint64_t file_no, uint32_t size_in_pages)
  {
    fifos[file_no & 1].size_in_pages= size_in_pages;
  }
};


/* Structure of an entry in the hash of binlog tablespace files. */
struct ibb_tblspc_entry {
  uint64_t file_no;
  /*
    Active transactions/oob-event-groups that start in this binlog tablespace
    file (including any user XA).
  */
  std::atomic<uint64_t>oob_refs;
  /*
    Active XA transactions whose oob start in this binlog tablespace file.
    ToDo: Note that user XA is not yet implemented.
  */
  std::atomic<uint64_t>xa_refs;
  /*
    The earliest file number that this binlog tablespace file has oob
    references into.
    (This is a conservative estimate, references may not actually exist in
    case their commit record went into a later file, or they ended up rolling
    back).
    Includes any XA oob records.
  */
  std::atomic<uint64_t>oob_ref_file_no;
  /*
    Earliest file number that we have XA references into.
    ToDo: Note that user XA is not yet implemented.
  */
  std::atomic<uint64_t>xa_ref_file_no;

  ibb_tblspc_entry()= default;
  ~ibb_tblspc_entry()= default;
};


/*
  Class keeping reference counts of oob records starting in different binlog
  tablespace files.
  Used to keep track of which files should not be purged because they contain
  oob (start) records that are still referenced by needed binlog tablespace
  files or by active transactions.
*/
class ibb_file_oob_refs {
public:
  /* Hash contains struct ibb_tblspc_entry keyed on file_no. */
  LF_HASH hash;
  /*
    Earliest file_no with start oob records that are still referenced by active
    transactions / event groups.
  */
  std::atomic<uint64_t> earliest_oob_ref;
  /*
    Same, but restricted to those oob that constitute XA transactions.
    Thus, this may be larger than earliest_oob_ref or even ~(uint64_t)0 in
    case there are no active XA.
  */
  std::atomic<uint64_t> earliest_xa_ref;

public:
  /* Init the hash empty. */
  void init() noexcept;
  void destroy() noexcept;
  /* Delete an entry from the hash. */
  void remove(uint64_t file_no, LF_PINS *pins);
  /* Delete all (consecutive) entries from file_no down. */
  void remove_up_to(uint64_t file_no, LF_PINS *pins);
  /* Update an entry when an OOB record is started/completed. */
  bool oob_ref_inc(uint64_t file_no, LF_PINS *pins);
  bool oob_ref_dec(uint64_t file_no, LF_PINS *pins);
  /* Update earliest_oob_ref when refcount drops to zero. */
  void do_zero_refcnt_action(uint64_t file_no, LF_PINS *pins,
                             bool active_moving);
  /* Update the oob and xa file_no's active at start of this file_no. */
  bool update_refs(uint64_t file_no, LF_PINS *pins,
                   uint64_t oob_ref, uint64_t xa_ref);
  /* Lookup the oob-referenced file_no from a file_no. */
  bool get_oob_ref_file_no(uint64_t file_no, LF_PINS *pins,
                           uint64_t *out_oob_ref_file_no);
};


class binlog_chunk_reader {
public:
  enum chunk_reader_status {
    CHUNK_READER_ERROR= -1,
    CHUNK_READER_EOF= 0,
    CHUNK_READER_FOUND= 1
  };

  /*
    Current state, can be obtained from save_pos() and later passed to
    restore_pos().
  */
  struct saved_position {
    /* Current position file. */
    uint64_t file_no;
    /* Current position page. */
    uint32_t page_no;
    /* Start of current chunk inside page. */
    uint32_t in_page_offset;
    /*
      The length of the current chunk, once the chunk type has been read.
      If 0, it means the chunk type (and length) has not yet been read.
    */
    uint32_t chunk_len;
    /* The read position inside the current chunk. */
    uint32_t chunk_read_offset;
    byte chunk_type;
    /* When set, read will skip the current chunk, if any. */
    bool skip_current;
    /* Set while we are in the middle of reading a record. */
    bool in_record;
  } s;

  /* Amount of data in file, valid after fetch_current_page(). */
  uint64_t cur_end_offset;
  /* Length of the currently open file, valid if cur_file_handle != -1. */
  uint64_t cur_file_length;
  /*
    After fetch_current_page(), this points into either cur_block or
    page_buffer as appropriate.
  */
  byte *page_ptr;
  /* Valid after fetch_current_page(), if page found in buffer pool. */
  fsp_binlog_page_entry *cur_block;
  /* Buffer for reading a page directly from a tablespace file. */
  byte *page_buffer;
  /*
    Points to either binlog_cur_durable_offset, for readers that should not
    see binlog data until it has become durable on disk; or
    binlog_cur_end_offset otherwise.
  */
  std::atomic<uint64_t> * const limit_offset;
  /* Open file handle to tablespace file_no, or -1. */
  File cur_file_handle;
  /*
    Flag used to skip the rest of any partial chunk we might be starting in
    the middle of.
  */
  bool skipping_partial;

  binlog_chunk_reader(std::atomic<uint64_t> *limit_offset_);
  void set_page_buf(byte *in_page_buf) { page_buffer= in_page_buf; }
  ~binlog_chunk_reader();

  /* Current type, or FSP_BINLOG_TYPE_FILLER if between records. */
  byte cur_type() { return (byte)(s.chunk_type & FSP_BINLOG_TYPE_MASK); }
  bool cur_is_cont() { return (s.chunk_type & FSP_BINLOG_FLAG_CONT) != 0; }
  bool end_of_record() { return !s.in_record; }
  static int read_error_corruption(uint64_t file_no, uint64_t page_no,
                                   const char *msg);
  int read_error_corruption(const char *msg)
  {
    return read_error_corruption(s.file_no, s.page_no, msg);
  }
  enum chunk_reader_status fetch_current_page();
  /*
    Try to read max_len bytes from a record into buffer.

    If multipage is true, will move across pages to read following
    continuation chunks, if any, to try and read max_len total bytes. Only if
    the record ends before max_len bytes is less amount of bytes returned.

    If multipage is false, will read as much is available on one page (up to
    max of max_len), and then return.

    Returns number of bytes read, or -1 for error.
    Returns 0 if the chunk_reader is pointing to start of a chunk at the end
    of the current binlog (ie. end-of-file).
  */
  int read_data(byte *buffer, int max_len, bool multipage);
  /* Read the file header of current file_no. */
  int get_file_header(binlog_header_data *out_header);

  /* Save current position, and restore it later. */
  void save_pos(saved_position *out_pos) { *out_pos= s; }
  void restore_pos(saved_position *pos);
  void seek(uint64_t file_no, uint64_t offset);

  /*
    Make next read_data() skip any data from the current chunk (if any), and
    start reading data only from the beginning of the next chunk. */
  void skip_current() { if (s.in_record) s.skip_current= true; }
  /*
    Used initially, after seeking potentially into the middle of a (commit)
    record, to skip any continuation chunks until we reach the start of the
    first real record.
  */
  void skip_partial(bool skip) { skipping_partial= skip; }
  /* Release any buffer pool page latch. */
  void release(bool release_file_page= false);
  bool data_available();
  bool is_before_pos(uint64_t file_no, uint64_t offset);
  uint64_t current_file_no() { return s.file_no; }
  uint64_t current_pos() {
    return (s.page_no << srv_page_size_shift) + s.in_page_offset;
  }
};


extern uint32_t ibb_page_size_shift;
extern ulong ibb_page_size;
/* The state interval (in pages) used for active_binlog_file_no. */
extern uint64_t current_binlog_state_interval;
extern mysql_mutex_t active_binlog_mutex;
extern pthread_cond_t active_binlog_cond;
extern mysql_mutex_t binlog_durable_mutex;
extern mysql_cond_t binlog_durable_cond;
extern std::atomic<uint64_t> active_binlog_file_no;
extern uint64_t first_open_binlog_file_no;
extern uint64_t last_created_binlog_file_no;
extern std::atomic<uint64_t> binlog_cur_durable_offset[4];
extern std::atomic<uint64_t> binlog_cur_end_offset[4];
extern fsp_binlog_page_fifo *binlog_page_fifo;

extern ibb_file_oob_refs ibb_file_hash;


static inline void
fsp_binlog_release(fsp_binlog_page_entry *page)
{
  binlog_page_fifo->release_page(page);
}

extern size_t crc32_pwrite_page(File fd, byte *buf, uint32_t page_no,
                                myf MyFlags) noexcept;
extern int crc32_pread_page(File fd, byte *buf, uint32_t page_no,
                            myf MyFlags) noexcept;
extern int crc32_pread_page(pfs_os_file_t fh, byte *buf, uint32_t page_no,
                            myf MyFlags) noexcept;
extern bool ibb_record_in_file_hash(uint64_t file_no, uint64_t oob_ref,
                                    uint64_t xa_ref, LF_PINS *in_pins=nullptr);
extern void binlog_write_up_to_now() noexcept;
extern void fsp_binlog_extract_header_page(const byte *page_buf,
                                           binlog_header_data *out_header_data)
  noexcept;
extern void fsp_log_binlog_write(mtr_t *mtr, fsp_binlog_page_entry *page,
                                 uint64_t file_no, uint32_t page_no,
                                 uint32_t page_offset, uint32_t len);
extern void fsp_log_header_page(mtr_t *mtr, fsp_binlog_page_entry *page,
                                uint64_t file_no, uint32_t len) noexcept;
extern void fsp_binlog_init();
extern void fsp_binlog_shutdown();
extern dberr_t fsp_binlog_tablespace_close(uint64_t file_no);
extern bool fsp_binlog_open(const char *file_name, pfs_os_file_t fh,
                            uint64_t file_no, size_t file_size,
                            uint32_t init_page, byte *partial_page);
extern dberr_t fsp_binlog_tablespace_create(uint64_t file_no,
                                            uint32_t size_in_pages,
                                            LF_PINS *pins);
extern std::pair<uint64_t, uint64_t> fsp_binlog_write_rec(
  struct chunk_data_base *chunk_data, mtr_t *mtr, byte chunk_type,
  LF_PINS *pins);
extern bool fsp_binlog_flush();

#endif /* fsp_binlog_h */
