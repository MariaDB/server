/* Copyright (c) 2025, Kristian Nielsen.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/*
  Code for reading engine-implemented binlog from the mysqlbinlog client
  program.
*/

#include <cinttypes>
#include <algorithm>
#include <vector>
#include "client_priv.h"
#include "mysqlbinlog-engine.h"
#include "my_compr_int.h"
#include "my_dir.h"


const char *INNODB_BINLOG_MAGIC= "\xfe\xfe\x0d\x01";
static constexpr uint32_t INNODB_BINLOG_FILE_VERS_MAJOR= 1;

static uint32_t binlog_page_size;

/*
  Some code here is copied from storage/innobase/handler/innodb_binlog.cc and
  storage/innodb_binlog/fsp/fsp_binlog.cc and modified for use in the
  mysqlbinlog command-line client.

  Normally it is desirable to share code rather than copy/modify it, but
  special considerations apply here:

   - Normally, it is desirable to share the code so that modifications to the
     logic are automatically kept in sync between the two use cases. However
     in the case of the binlog, non-backwards compatible changes are highly
     undesirable, and having a separate reader implementation in mysqlbinlog
     is actually useful to detect any unintended or non-desirable changes to
     the format that prevent old code from reading it. The binlog files
     should remain readable to old mysqlbinlog versions if at all possible,
     as well as to any other 3rd-party readers.

   - The main purpose of the code inside InnoDB is to very efficiently allow
     reading of binlog data concurrently with active writing threads and
     concurrently with page fifo asynchroneous flushing. In contrast, the
     purpose of the mysqlclient code is to have a simple stand-alone command
     line reader of the files. These two use cases are sufficiently
     different, and the code frameworks used for storage/innobase/ and
     client/ likewise sufficiently different, that code-sharing seems more
     troublesome than beneficial here.
*/

static constexpr uint32_t BINLOG_PAGE_SIZE_MAX= 65536;
#define BINLOG_PAGE_DATA 0
#define BINLOG_PAGE_CHECKSUM 4
#define BINLOG_PAGE_DATA_END BINLOG_PAGE_CHECKSUM

#define BINLOG_NAME_BASE "binlog-"
#define BINLOG_NAME_EXT ".ibb"

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
static constexpr uint32_t FSP_BINLOG_FLAG_BIT_CONT= 7;
static constexpr uint32_t FSP_BINLOG_FLAG_CONT= (1 << FSP_BINLOG_FLAG_BIT_CONT);
static constexpr uint32_t FSP_BINLOG_FLAG_BIT_LAST= 6;
static constexpr uint32_t FSP_BINLOG_FLAG_LAST= (1 << FSP_BINLOG_FLAG_BIT_LAST);
static constexpr uint32_t FSP_BINLOG_TYPE_MASK=
  ~(FSP_BINLOG_FLAG_CONT | FSP_BINLOG_FLAG_LAST);
static constexpr uint64_t ALLOWED_NESTED_RECORDS=
  /* GTID STATE at start of page can occur in the middle of other record. */
  ((uint64_t)1 << FSP_BINLOG_TYPE_GTID_STATE) |
  /* DUMMY data at tablespace end can occur in the middle of other record. */
  ((uint64_t)1 << FSP_BINLOG_TYPE_DUMMY)
  ;


static char binlog_dir[FN_REFLEN + 1];


class chunk_reader_mysqlbinlog {
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
    uchar chunk_type;
    /* When set, read will skip the current chunk, if any. */
    bool skip_current;
    /* Set while we are in the middle of reading a record. */
    bool in_record;
  } s;

  /* Length of the currently open file, valid if cur_file_handle != -1. */
  uint64_t cur_file_length;
  /* Buffer for reading a page from a binlog file. */
  uchar *page_buffer;
  /* Open file handle to tablespace file_no, or -1. */
  File cur_file_handle;
  /*
    Flag used to skip the rest of any partial chunk we might be starting in
    the middle of.
  */
  bool skipping_partial;
  /* If the s.file_no / s.page_no is loaded in the page buffer. */
  bool page_loaded;

  chunk_reader_mysqlbinlog();
  void set_page_buf(uchar *in_page_buf) { page_buffer= in_page_buf; }
  ~chunk_reader_mysqlbinlog();

  /* Current type, or FSP_BINLOG_TYPE_FILLER if between records. */
  uchar cur_type() { return (uchar)(s.chunk_type & FSP_BINLOG_TYPE_MASK); }
  bool cur_is_cont() { return (s.chunk_type & FSP_BINLOG_FLAG_CONT) != 0; }
  bool end_of_record() { return !s.in_record; }
  bool is_end_of_page() noexcept
  {
    return s.in_page_offset >= binlog_page_size - (BINLOG_PAGE_DATA_END + 3);
  }
  bool is_end_of_file() noexcept
  {
    return current_pos() + (BINLOG_PAGE_DATA_END + 3) >= cur_file_length;
  }
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
  int read_data(uchar *buffer, int max_len, bool multipage);
  /* Read the file header of current file_no. */
  int parse_file_header();

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
  uint64_t current_pos() {
    return (s.page_no * binlog_page_size) + s.in_page_offset;
  }
  void set_fd(File fd);
};


class oob_reader_mysqlbinlog {
  enum oob_states {
    /* The initial state, about to visit the node for the first time. */
    ST_initial,
    /* State of leaf node while traversing the prior trees in the forest. */
    ST_traversing_prior_trees,
    /* State of non-leaf node while traversing its left sub-tree. */
    ST_traversing_left_child,
    /* State of non-leaf node while traversing its right sub-tree. */
    ST_traversing_right_child,
    /* State of node while reading out its data. */
    ST_self
  };

  /*
    Stack entry for one node currently taking part in post-order traversal.
    We maintain a stack of pending nodes during the traversal, as the traversal
    happens in a state machine rather than by recursion.
  */
  struct stack_entry {
    /* Saved position after reading header. */
    chunk_reader_mysqlbinlog::saved_position saved_pos;
    /* The location of this node's OOB record. */
    uint64_t file_no;
    uint64_t offset;
    /* Right child, to be traversed after left child. */
    uint64_t right_file_no;
    uint64_t right_offset;
    /* Offset of real data in this node, after header. */
    uint32_t header_len;
    /* Amount of data read into rd_buf, and amount used to parse header. */
    uint32_t rd_buf_len;
    uint32_t rd_buf_sofar;
    /* Current state in post-order traversal state machine. */
    enum oob_states state;
    /* Buffer for reading header. */
    uchar rd_buf[5*COMPR_INT_MAX64];
    /*
      True when the node is reached using only left child pointers, false
      otherwise. Used to identify the left-most leaf in a tree which points to
      a prior tree that must be traversed first.
    */
    bool is_leftmost;
  };
  std::vector<stack_entry>stack;

  /* State machine current state. */
  enum oob_states state;

public:
  oob_reader_mysqlbinlog();
  ~oob_reader_mysqlbinlog();

  void start_traversal(uint64_t file_no, uint64_t offset);
  bool oob_traversal_done() { return stack.empty(); }
 int read_data(chunk_reader_mysqlbinlog *chunk_rd, uchar *buf, int max_len);

private:
  void push_state(enum oob_states state, uint64_t file_no, uint64_t offset,
                  bool is_leftmost);
};


class binlog_reader_innodb : public handler_binlog_reader {
  enum reader_states {
    ST_read_next_event_group, ST_read_oob_data, ST_read_commit_record
  };

  chunk_reader_mysqlbinlog chunk_rd;
  oob_reader_mysqlbinlog oob_reader;
  chunk_reader_mysqlbinlog::saved_position saved_commit_pos;

  /* Out-of-band data to read after commit record, if any. */
  uint64_t oob_count;
  uint64_t oob_last_file_no;
  uint64_t oob_last_offset;
  /* Any secondary out-of-band data to be also read. */
  uint64_t oob_count2;
  uint64_t oob_last_file_no2;
  uint64_t oob_last_offset2;
  /*
    The starting file_no. We stop once we've read the last record in this file
    (which may span into the next file).
  */
  uint64_t start_file_no;
  /* Buffer to hold a page read directly from the binlog file. */
  uchar *page_buf;
  /* Keep track of pending bytes in the rd_buf. */
  uint32_t rd_buf_len;
  uint32_t rd_buf_sofar;
  /* State for state machine reading chunks one by one. */
  enum reader_states state;

  /* Used to read the header of the commit record. */
  uchar rd_buf[5*COMPR_INT_MAX64];
private:
  int read_data(uchar *buf, uint32_t len);

public:
  binlog_reader_innodb();
  virtual ~binlog_reader_innodb();
  virtual int read_binlog_data(uchar *buf, uint32_t len) final;
  virtual bool data_available() final;
  virtual bool wait_available(THD *thd, const struct timespec *abstime) final;
  virtual int init_gtid_pos(slave_connection_state *pos,
                            rpl_binlog_state_base *state) final;
  virtual int init_legacy_pos(const char *filename, ulonglong offset) final;
  virtual void enable_single_file() final;
  bool is_valid() { return page_buf != nullptr; }
  bool init_from_fd_pos(File fd, ulonglong start_position);
};


static int
read_page_mysqlbinlog(File fd, uchar *buf, uint32_t page_no) noexcept
{
  size_t read= my_pread(fd, buf, binlog_page_size,
                        (my_off_t)page_no * binlog_page_size, MYF(0));
  int res= 1;
  if (likely(read == binlog_page_size))
  {
    const uint32_t payload= (uint32_t)binlog_page_size - BINLOG_PAGE_CHECKSUM;
    uint32_t crc32= uint4korr(buf + payload);
    /* Allow a completely zero (empty) page as well. */
    if (unlikely(crc32 != my_crc32c(0, buf, payload)) &&
        (buf[0] != 0 || 0 != memcmp(buf, buf+1, binlog_page_size - 1)))
    {
      res= -1;
      my_errno= EIO;
    }
  }
  else if (read == (size_t)-1)
    res= -1;
  else
    res= 0;

  return res;
}


chunk_reader_mysqlbinlog::chunk_reader_mysqlbinlog()
  : s { 0, 0, 0, 0, 0, FSP_BINLOG_TYPE_FILLER, false, false },
    cur_file_length(0),
    cur_file_handle((File)-1),
    skipping_partial(false), page_loaded(false)
{
}


chunk_reader_mysqlbinlog::~chunk_reader_mysqlbinlog()
{
  if (cur_file_handle >= (File)0)
    my_close(cur_file_handle, MYF(0));
}


int
chunk_reader_mysqlbinlog::read_error_corruption(uint64_t file_no, uint64_t page_no,
                                                const char *msg)
{
  error("Corrupt InnoDB binlog found on page %" PRIu64 " in binlog number "
        "%" PRIu64 ": %s", page_no, file_no, msg);
  return -1;
}


int
chunk_reader_mysqlbinlog::read_data(uchar *buffer, int max_len, bool multipage)
{
  uint32_t size;
  int sofar= 0;

read_more_data:
  if (max_len == 0)
    return sofar;

  if (!page_loaded)
  {
    enum chunk_reader_status res= fetch_current_page();
    if (res == CHUNK_READER_EOF)
      return 0;
    if (res == CHUNK_READER_ERROR)
      return -1;
  }

  if (s.chunk_len == 0)
  {
    uchar type;
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
    if ((s.page_no * binlog_page_size) + s.in_page_offset >= cur_file_length)
      return sofar;

    if (s.in_page_offset >= binlog_page_size - (BINLOG_PAGE_DATA_END + 3) ||
             page_buffer[s.in_page_offset] == FSP_BINLOG_TYPE_FILLER)
    {
      DBUG_ASSERT(s.in_page_offset >= binlog_page_size - BINLOG_PAGE_DATA_END ||
            page_buffer[s.in_page_offset] == FSP_BINLOG_TYPE_FILLER);
      goto go_next_page;
    }

    type= page_buffer[s.in_page_offset];
    if (type == 0)
      return 0;

    /*
      Consistency check on the chunks. A record must consist in a sequence of
      chunks of the same type, all but the first must have the
      FSP_BINLOG_FLAG_BIT_CONT bit set, and the final one must have the
      FSP_BINLOG_FLAG_BIT_LAST bit set.
    */
    if (!s.in_record)
    {
      if (type & FSP_BINLOG_FLAG_CONT && !s.skip_current)
      {
        if (skipping_partial)
        {
          s.chunk_len= page_buffer[s.in_page_offset + 1] |
            ((uint32_t)page_buffer[s.in_page_offset + 2] << 8);
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
          s.chunk_len= page_buffer[s.in_page_offset + 1] |
            ((uint32_t)page_buffer[s.in_page_offset + 2] << 8);
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
    s.chunk_len= page_buffer[s.in_page_offset + 1] |
      ((uint32_t)page_buffer[s.in_page_offset + 2] << 8);
    s.chunk_read_offset= 0;
  }

  /* Now we have a chunk available to read data from. */
  DBUG_ASSERT(s.chunk_read_offset < s.chunk_len);
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
    memcpy(buffer, page_buffer + s.in_page_offset + 3 + s.chunk_read_offset, size);
    buffer+= size;
    s.chunk_read_offset+= size;
    max_len-= size;
    sofar+= size;
  }

  if (s.chunk_len > s.chunk_read_offset)
  {
    DBUG_ASSERT(max_len == 0 /* otherwise would have read more */);
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

  if (s.in_page_offset >= binlog_page_size - (BINLOG_PAGE_DATA_END + 3) &&
      (s.page_no * binlog_page_size) + s.in_page_offset < cur_file_length)
  {
go_next_page:
    /* End of page reached, move to the next page. */
    ++s.page_no;
    page_loaded= false;
    s.in_page_offset= 0;

    if (cur_file_handle >= (File)0 &&
        (s.page_no * binlog_page_size) >= cur_file_length)
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


oob_reader_mysqlbinlog::oob_reader_mysqlbinlog()
{
  /* Nothing. */
}


oob_reader_mysqlbinlog::~oob_reader_mysqlbinlog()
{
  /* Nothing. */
}


void
oob_reader_mysqlbinlog::push_state(enum oob_states state, uint64_t file_no,
                                     uint64_t offset, bool is_leftmost)
{
  stack_entry new_entry;
  new_entry.state= state;
  new_entry.file_no= file_no;
  new_entry.offset= offset;
  new_entry.is_leftmost= is_leftmost;
  stack.emplace_back(std::move(new_entry));
}


void
oob_reader_mysqlbinlog::start_traversal(uint64_t file_no, uint64_t offset)
{
  stack.clear();
  push_state(ST_initial, file_no, offset, true);
}


/*
  Read from out-of-band event group data.

  Does a state-machine incremental traversal of the forest of perfect binary
  trees of oob records in the event group. May read just the data available
  on one page, thus returning less than the requested number of bytes (this
  is to prefer to inspect each page only once, returning data page-by-page as
  long as reader asks for at least a full page worth of data).
*/
int
oob_reader_mysqlbinlog::read_data(chunk_reader_mysqlbinlog *chunk_rd,
                                  uchar *buf, int len)
{
  stack_entry *e;
  uint64_t chunk_idx;
  uint64_t left_file_no;
  uint64_t left_offset;
  int res;
  const uchar *p_end;
  const uchar *p;
  std::pair<uint64_t, const unsigned char *> v_and_p;
  int size;

  if (stack.empty())
  {
    DBUG_ASSERT(0 /* Should not call when no more oob data to read. */);
    return 0;
  }

again:
  e= &(stack[stack.size() - 1]);
  switch (e->state)
  {
  case ST_initial:
    chunk_rd->seek(e->file_no, e->offset);
    static_assert(sizeof(e->rd_buf) == 5*COMPR_INT_MAX64,
                  "rd_buf size must match code using it");
    res= chunk_rd->read_data(e->rd_buf, 5*COMPR_INT_MAX64, true);
    if (res < 0)
      return -1;
    if (chunk_rd->cur_type() != FSP_BINLOG_TYPE_OOB_DATA)
      return chunk_rd->read_error_corruption("Wrong chunk type");
    if (res == 0)
      return chunk_rd->read_error_corruption("Unexpected EOF, expected "
                                             "oob chunk");
    e->rd_buf_len= res;
    p_end= e->rd_buf + res;
    v_and_p= compr_int_read(e->rd_buf);
    p= v_and_p.second;
    if (p > p_end)
      return chunk_rd->read_error_corruption("Short chunk");
    chunk_idx= v_and_p.first;
    (void)chunk_idx;

    v_and_p= compr_int_read(p);
    p= v_and_p.second;
    if (p > p_end)
      return chunk_rd->read_error_corruption("Short chunk");
    left_file_no= v_and_p.first;
    v_and_p= compr_int_read(p);
    p= v_and_p.second;
    if (p > p_end)
      return chunk_rd->read_error_corruption("Short chunk");
    left_offset= v_and_p.first;

    v_and_p= compr_int_read(p);
    p= v_and_p.second;
    if (p > p_end)
      return chunk_rd->read_error_corruption("Short chunk");
    e->right_file_no= v_and_p.first;
    v_and_p= compr_int_read(p);
    p= v_and_p.second;
    if (p > p_end)
      return chunk_rd->read_error_corruption("Short chunk");
    e->right_offset= v_and_p.first;
    e->rd_buf_sofar= (uint32_t)(p - e->rd_buf);
    if (left_file_no == 0 && left_offset == 0)
    {
      /* Leaf node. */
      if (e->is_leftmost && !(e->right_file_no == 0 && e->right_offset == 0))
      {
        /* Traverse the prior tree(s) in the forst. */
        e->state= ST_traversing_prior_trees;
        chunk_rd->save_pos(&e->saved_pos);
        push_state(ST_initial, e->right_file_no, e->right_offset, true);
      }
      else
        e->state= ST_self;
    }
    else
    {
      e->state= ST_traversing_left_child;
      chunk_rd->save_pos(&e->saved_pos);
      push_state(ST_initial, left_file_no, left_offset, e->is_leftmost);
    }
    goto again;

  case ST_traversing_prior_trees:
    chunk_rd->restore_pos(&e->saved_pos);
    e->state= ST_self;
    goto again;

  case ST_traversing_left_child:
    e->state= ST_traversing_right_child;
    push_state(ST_initial, e->right_file_no, e->right_offset, false);
    goto again;

  case ST_traversing_right_child:
    chunk_rd->restore_pos(&e->saved_pos);
    e->state= ST_self;
    goto again;

  case ST_self:
    size= 0;
    if (e->rd_buf_len > e->rd_buf_sofar)
    {
      /* Use any excess data from when the header was read. */
      size= std::min((int)(e->rd_buf_len - e->rd_buf_sofar), len);
      memcpy(buf, e->rd_buf + e->rd_buf_sofar, size);
      e->rd_buf_sofar+= size;
      len-= size;
      buf+= size;
    }

    if (len > 0 && !chunk_rd->end_of_record())
    {
      res= chunk_rd->read_data(buf, len, false);
      if (res < 0)
        return -1;
      size+= res;
    }

    if (chunk_rd->end_of_record())
    {
      /* This oob record done, pop the state. */
      DBUG_ASSERT(!stack.empty());
      stack.erase(stack.end() - 1, stack.end());
    }
    return size;

  default:
    DBUG_ASSERT(0);
    return -1;
  }
}


binlog_reader_innodb::binlog_reader_innodb()
  : oob_count(0), oob_last_file_no(0), oob_last_offset(0),
    oob_count2(0), oob_last_file_no2(0), oob_last_offset2(0),
    start_file_no(~(uint64_t)0),
    rd_buf_len(0), rd_buf_sofar(0), state(ST_read_next_event_group)
{
  page_buf= (uchar *)
    my_malloc(PSI_NOT_INSTRUMENTED, BINLOG_PAGE_SIZE_MAX, MYF(MY_WME));
  chunk_rd.set_page_buf(page_buf);
}


binlog_reader_innodb::~binlog_reader_innodb()
{
  my_free(page_buf);
}


void
chunk_reader_mysqlbinlog::set_fd(File fd)
{
  if (cur_file_handle != (File)-1)
  {
    my_close(cur_file_handle, MYF(0));
    cur_file_length= ~(uint64_t)0;
    page_loaded= false;
  }
  cur_file_handle= fd;
  my_off_t old_pos= my_tell(fd, MYF(0));
  if (old_pos != (my_off_t)-1)
  {
    /* Will be ~0 if we cannot seek the file. */
    cur_file_length= my_seek(fd, 0, SEEK_END, MYF(0));
    my_seek(fd, old_pos, SEEK_SET, MYF(0));
  }
}


bool
binlog_reader_innodb::data_available()
{
  DBUG_ASSERT(0 /* Should not be used in mysqlbinlog. */);
  return true;
}


bool
binlog_reader_innodb::wait_available(THD *thd, const struct timespec *abstime)
{
  DBUG_ASSERT(0 /* Should not be used in mysqlbinlog. */);
  return true;
}


int
binlog_reader_innodb::init_gtid_pos(slave_connection_state *pos,
                                    rpl_binlog_state_base *state)
{
  DBUG_ASSERT(0 /* Should not be used in mysqlbinlog. */);
  return 1;
}


int
binlog_reader_innodb::init_legacy_pos(const char *filename, ulonglong offset)
{
  DBUG_ASSERT(0 /* Should not be used in mysqlbinlog. */);
  return 1;
}


void
binlog_reader_innodb::enable_single_file()
{
  DBUG_ASSERT(0 /* Should not be used in mysqlbinlog. */);
}


int
binlog_reader_innodb::read_binlog_data(uchar *buf, uint32_t len)
{
  int res= read_data(buf, len);
  return res;
}


bool
binlog_reader_innodb::init_from_fd_pos(File fd, ulonglong start_position)
{
  chunk_rd.set_fd(fd);
  if (chunk_rd.parse_file_header())
    return true;
  uint64_t prev_start_file_no= start_file_no;
  start_file_no= chunk_rd.s.file_no;
  if (prev_start_file_no != ~(uint64_t)0 &&
      prev_start_file_no + 1 == chunk_rd.s.file_no)
  {
    /* Continuing in the file following the previous one. */
  }
  else
  {
    if (start_position < binlog_page_size)
      start_position= binlog_page_size;
    chunk_rd.seek(chunk_rd.s.file_no, (uint64_t)start_position);
    chunk_rd.skip_partial(true);
  }
  return false;
}


int binlog_reader_innodb::read_data(uchar *buf, uint32_t len)
{
  int res;
  const uchar *p_end;
  const uchar *p;
  std::pair<uint64_t, const unsigned char *> v_and_p;
  int sofar= 0;

again:
  switch (state)
  {
  case ST_read_next_event_group:
    if (chunk_rd.s.file_no > start_file_no ||
        (chunk_rd.s.file_no == start_file_no && chunk_rd.is_end_of_file()))
    {
      /*
        We have read the entire file, return EOF.
        If the user specified to read the following file also, we may
        continue where we left in that file later.
      */
      return sofar;
    }
    static_assert(sizeof(rd_buf) == 5*COMPR_INT_MAX64,
                  "rd_buf size must match code using it");
    res= chunk_rd.read_data(rd_buf, 5*COMPR_INT_MAX64, true);
    if (res < 0)
      return res;
    if (res == 0)
      return sofar;
    if (chunk_rd.cur_type() != FSP_BINLOG_TYPE_COMMIT)
    {
      chunk_rd.skip_current();
      goto again;
    }
    /* Found the start of a commit record. */
    chunk_rd.skip_partial(false);

    /* Read the header of the commit record to see if there's any oob data. */
    rd_buf_len= res;
    p_end= rd_buf + res;
    v_and_p= compr_int_read(rd_buf);
    p= v_and_p.second;
    if (p > p_end)
      return chunk_rd.read_error_corruption("Short chunk");
    oob_count= v_and_p.first;
    oob_count2= 0;

    if (oob_count > 0)
    {
      /* Skip the pointer to first chunk. */
      v_and_p= compr_int_read(p);
      p= v_and_p.second;
      if (p > p_end)
        return chunk_rd.read_error_corruption("Short chunk");
      v_and_p= compr_int_read(p);
      p= v_and_p.second;
      if (p > p_end)
        return chunk_rd.read_error_corruption("Short chunk");

      v_and_p= compr_int_read(p);
      p= v_and_p.second;
      if (p > p_end)
        return chunk_rd.read_error_corruption("Short chunk");
      oob_last_file_no= v_and_p.first;
      v_and_p= compr_int_read(p);
      p= v_and_p.second;
      if (p > p_end)
        return chunk_rd.read_error_corruption("Short chunk");
      oob_last_offset= v_and_p.first;

      /* Check for any secondary oob data. */
      v_and_p= compr_int_read(p);
      p= v_and_p.second;
      if (p > p_end)
        return chunk_rd.read_error_corruption("Short chunk");
      oob_count2= v_and_p.first;

      if (oob_count2 > 0)
      {
        /* Skip the pointer to first chunk. */
        v_and_p= compr_int_read(p);
        p= v_and_p.second;
        if (p > p_end)
          return chunk_rd.read_error_corruption("Short chunk");
        v_and_p= compr_int_read(p);
        p= v_and_p.second;
        if (p > p_end)
          return chunk_rd.read_error_corruption("Short chunk");

        v_and_p= compr_int_read(p);
        p= v_and_p.second;
        if (p > p_end)
          return chunk_rd.read_error_corruption("Short chunk");
        oob_last_file_no2= v_and_p.first;
        v_and_p= compr_int_read(p);
        p= v_and_p.second;
        if (p > p_end)
          return chunk_rd.read_error_corruption("Short chunk");
        oob_last_offset2= v_and_p.first;
      }
    }

    rd_buf_sofar= (uint32_t)(p - rd_buf);
    state= ST_read_commit_record;
    goto again;

  case ST_read_commit_record:
    if (rd_buf_len > rd_buf_sofar)
    {
      /* Use any excess data from when the header was read. */
      int size= std::min((int)(rd_buf_len - rd_buf_sofar), (int)len);
      memcpy(buf, rd_buf + rd_buf_sofar, size);
      rd_buf_sofar+= size;
      len-= size;
      buf+= size;
      sofar+= size;
    }

    if (len > 0 && !chunk_rd.end_of_record())
    {
      res= chunk_rd.read_data(buf, len, false);
      if (res < 0)
        return -1;
      len-= res;
      buf+= res;
      sofar+= res;
    }

    if (rd_buf_sofar == rd_buf_len && chunk_rd.end_of_record())
    {
      if (oob_count == 0)
      {
        state= ST_read_next_event_group;
        if (len > 0 && !chunk_rd.is_end_of_page())
        {
          /*
            Let us try to read more data from this page. The goal is to read
            from each page only once, as long as caller passes in a buffer at
            least as big as our page size. Though commit record header that
            spans a page boundary or oob records can break this property.
          */
          goto again;
        }
      }
      else
      {
        oob_reader.start_traversal(oob_last_file_no, oob_last_offset);
        chunk_rd.save_pos(&saved_commit_pos);
        state= ST_read_oob_data;
      }
      if (sofar == 0)
        goto again;
    }

    return sofar;

  case ST_read_oob_data:
    res= oob_reader.read_data(&chunk_rd, buf, len);
    if (res < 0)
      return -1;
    if (oob_reader.oob_traversal_done())
    {
      if (oob_count2 > 0)
      {
        /* Switch over to secondary oob data. */
        oob_count= oob_count2;
        oob_count2= 0;
        oob_last_file_no= oob_last_file_no2;
        oob_last_offset= oob_last_offset2;
        oob_reader.start_traversal(oob_last_file_no, oob_last_offset);
        state= ST_read_oob_data;
      }
      else
      {
        chunk_rd.restore_pos(&saved_commit_pos);
        state= ST_read_next_event_group;
      }
    }
    if (res == 0)
    {
      DBUG_ASSERT(0 /* Should have had oob_traversal_done() last time then. */);
      if (sofar == 0)
        goto again;
    }
    return sofar + res;

  default:
    DBUG_ASSERT(0);
    return -1;
  }
}


int
chunk_reader_mysqlbinlog::parse_file_header()
{
  binlog_page_size= BINLOG_HEADER_PAGE_SIZE;  // Until we get the real page size
  if (read_page_mysqlbinlog(cur_file_handle, page_buffer, 0) <= 0)
  {
    error("Cannot read first page of InnoDB binlog file");
    return -1;
  }
  const uint32_t payload= BINLOG_HEADER_PAGE_SIZE - BINLOG_PAGE_CHECKSUM;
  uint32_t crc32= uint4korr(page_buffer + payload);
  if (crc32 != my_crc32c(0, page_buffer, payload))
  {
    error("Invalid checksum on first page, cannot read binlog file");
    return -1;
  }
  uint32_t vers_major= uint4korr(page_buffer + 8);
  if (vers_major > INNODB_BINLOG_FILE_VERS_MAJOR)
  {
    error("Unsupported version of InnoDB binlog file, cannot read");
    return -1;
  }
  binlog_page_size= 1 << uint4korr(page_buffer + 4);
  s.file_no= uint8korr(page_buffer + 16);
  return 0;
}


enum chunk_reader_mysqlbinlog::chunk_reader_status
chunk_reader_mysqlbinlog::fetch_current_page()
{
  uint64_t offset;
  page_loaded= false;
  for (;;)
  {
    if (cur_file_handle < (File)0)
    {
      char filename[FN_REFLEN + 1];
      MY_STAT stat_buf;

      snprintf(filename, FN_REFLEN,
               "%s/" BINLOG_NAME_BASE "%06" PRIu64 BINLOG_NAME_EXT,
               binlog_dir, s.file_no);
      cur_file_handle= my_open(filename, O_RDONLY | O_BINARY, MYF(MY_WME));
      if (cur_file_handle < (File)0) {
        cur_file_handle= (File)-1;
        cur_file_length= ~(uint64_t)0;
        return CHUNK_READER_ERROR;
      }
      if (my_fstat(cur_file_handle, &stat_buf, MYF(0))) {
        error("Cannot stat() file '%s', errno: %d", filename, errno);
        my_close(cur_file_handle, MYF(0));
        cur_file_handle= (File)-1;
        cur_file_length= ~(uint64_t)0;
        return CHUNK_READER_ERROR;
      }
      cur_file_length= stat_buf.st_size;
    }

    offset= (s.page_no * binlog_page_size) | s.in_page_offset;
    if (offset >= cur_file_length) {
      /* End of this file, move to the next one. */
  goto_next_file:
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
    break;
  }

  int res= read_page_mysqlbinlog(cur_file_handle, page_buffer, s.page_no);
  if (res < 0)
    return CHUNK_READER_ERROR;
  if (res == 0)
    goto goto_next_file;
  page_loaded= true;
  return CHUNK_READER_FOUND;
}


void
chunk_reader_mysqlbinlog::restore_pos(chunk_reader_mysqlbinlog::saved_position *pos)
{
  if (cur_file_handle != (File)-1 && pos->file_no != s.file_no)
  {
    /* Seek to a different file than currently open, close it. */
    my_close(cur_file_handle, MYF(0));
    cur_file_handle= (File)-1;
    cur_file_length= ~(uint64_t)0;
  }
  s= *pos;
  page_loaded= false;
}


void
chunk_reader_mysqlbinlog::seek(uint64_t file_no, uint64_t offset)
{
  saved_position pos {
    file_no, (uint32_t)(offset / binlog_page_size),
    (uint32_t)(offset % binlog_page_size),
    0, 0, FSP_BINLOG_TYPE_FILLER, false, false };
  restore_pos(&pos);
}


bool
open_engine_binlog(handler_binlog_reader *generic_reader,
                   ulonglong start_position,
                   const char *filename, IO_CACHE *opened_cache)
{
  binlog_reader_innodb *reader= (binlog_reader_innodb *)generic_reader;
  if (!reader->is_valid())
  {
    error("Out of memory allocating page buffer");
    return true;
  }
  static_assert(sizeof(binlog_dir) >= FN_REFLEN + 1,
                "dirname_part() needs up to FN_REFLEN char buffer");
  size_t dummy;
  dirname_part(binlog_dir, filename, &dummy);
  if (!strlen(binlog_dir))
    strncpy(binlog_dir, ".", sizeof(binlog_dir) - 1);
  return reader->init_from_fd_pos(dup(opened_cache->file), start_position);
}


handler_binlog_reader *
get_binlog_reader_innodb()
{
  return new binlog_reader_innodb();
}
