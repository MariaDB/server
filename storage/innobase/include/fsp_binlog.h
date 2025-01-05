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

#include "univ.i"
#include "mtr0mtr.h"


struct fil_space_t;
struct chunk_data_base;


enum fsp_binlog_chunk_types {
  /* Zero means no data, effectively EOF. */
  FSP_BINLOG_TYPE_EMPTY= 0,
  /* A binlogged committed event group. */
  FSP_BINLOG_TYPE_COMMIT= 1,
  /* A binlog GTID state record. */
  FSP_BINLOG_TYPE_GTID_STATE= 2,
  /* Out-of-band event group data. */
  FSP_BINLOG_TYPE_OOB_DATA= 3,
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
  ((uint64_t)1 << FSP_BINLOG_TYPE_GTID_STATE)
  ;
/* Ensure that all types fit in the ALLOWED_NESTED_RECORDS bitmask. */
static_assert(FSP_BINLOG_TYPE_END <= 8*sizeof(ALLOWED_NESTED_RECORDS));


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

  /* The mtr is started iff cur_block is non-NULL. */
  mtr_t mtr;
  /*
    After fetch_current_page(), this points into either cur_block or
    page_buffer as appropriate.
  */
  byte *page_ptr;
  /* Valid after fetch_current_page(), if page found in buffer pool. */
  buf_block_t *cur_block;
  /* Buffer for reading a page directly from a tablespace file. */
  byte *page_buffer;
  /* Amount of data in file, valid after fetch_current_page(). */
  uint64_t cur_end_offset;
  /* Length of the currently open file, valid if cur_file_handle != -1. */
  uint64_t cur_file_length;
  /* Open file handle to tablespace file_no, or -1. */
  File cur_file_handle;
  /*
    Flag used to skip the rest of any partial chunk we might be starting in
    the middle of.
  */
  bool skipping_partial;

  binlog_chunk_reader();
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
};


extern uint64_t current_binlog_state_interval;
extern mysql_mutex_t active_binlog_mutex;
extern pthread_cond_t active_binlog_cond;
extern std::atomic<uint64_t> active_binlog_file_no;
extern fil_space_t* active_binlog_space;
extern uint64_t first_open_binlog_file_no;
extern uint64_t last_created_binlog_file_no;
extern fil_space_t *last_created_binlog_space;
extern std::atomic<uint64_t> binlog_cur_written_offset[2];
extern std::atomic<uint64_t> binlog_cur_end_offset[2];

extern void fsp_binlog_init();
extern dberr_t fsp_binlog_tablespace_close(uint64_t file_no);
extern fil_space_t *fsp_binlog_open(const char *file_name, pfs_os_file_t fh,
                                    uint64_t file_no, size_t file_size,
                                    bool open_empty);
extern dberr_t fsp_binlog_tablespace_create(uint64_t file_no,
                                            fil_space_t **new_space);
extern std::pair<uint64_t, uint64_t> fsp_binlog_write_chunk(
  struct chunk_data_base *chunk_data, mtr_t *mtr, byte chunk_type);

#endif /* fsp_binlog_h */
