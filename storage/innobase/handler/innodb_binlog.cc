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
@file handler/innodb_binlog.cc
InnoDB implementation of binlog.
*******************************************************/

#include "ut0compr_int.h"
#include "innodb_binlog.h"
#include "mtr0log.h"
#include "fsp0fsp.h"
#include "trx0trx.h"
#include "log0log.h"
#include "small_vector.h"

#include "rpl_gtid_base.h"
#include "handler.h"
#include "log.h"


static int innodb_binlog_inited= 0;

uint32_t innodb_binlog_size_in_pages;
const char *innodb_binlog_directory;

/* Current write position in active binlog file. */
uint32_t binlog_cur_page_no;
uint32_t binlog_cur_page_offset;

/*
  Server setting for how often to dump a (differential) binlog state at the
  start of the page, to speed up finding the initial GTID position, read-only.
*/
ulonglong innodb_binlog_state_interval;

/*
  Differential binlog state in the currently active binlog tablespace, relative
  to the state at the start.
*/
rpl_binlog_state_base binlog_diff_state;

static std::thread binlog_prealloc_thr_obj;
static bool prealloc_thread_end= false;

/*
  Mutex around purge operations, including earliest_binlog_file_no and
  total_binlog_used_size.
*/
mysql_mutex_t purge_binlog_mutex;

/* The earliest binlog tablespace file. Used in binlog purge. */
static uint64_t earliest_binlog_file_no;

/*
  The total space in use by binlog tablespace files. Maintained in-memory to
  not have to stat(2) every file for every new binlog tablespace allocated in
  case of --max-binlog-total-size.

  Initialized at server startup (and in RESET MASTER), and updated as binlog
  files are pre-allocated and purged.
*/
size_t total_binlog_used_size;

static bool purge_warning_given= false;


#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t binlog_prealloc_thread_key;
#endif


/* Structure holding context for out-of-band chunks of binlogged event group. */
struct binlog_oob_context {
  /*
    Structure used to encapsulate the data to be binlogged in an out-of-band
    chunk, for use by fsp_binlog_write_rec().
  */
  struct chunk_data_oob : public chunk_data_base {
    /*
      Need room for 5 numbers:
        node index
        left child file_no
        left child offset
        right child file_no
        right child offset
    */
    static constexpr uint32_t max_buffer= 5*COMPR_INT_MAX64;
    uint64_t sofar;
    uint64_t main_len;
    byte *main_data;
    uint32_t header_len;
    byte header_buf[max_buffer];

    chunk_data_oob(uint64_t idx,
                   uint64_t left_file_no, uint64_t left_offset,
                   uint64_t right_file_no, uint64_t right_offset,
                   byte *data, size_t data_len);
    virtual ~chunk_data_oob() {};
    virtual std::pair<uint32_t, bool> copy_data(byte *p, uint32_t max_len) final;
  };

  bool binlog_node(uint32_t node, uint64_t new_idx,
                   uint32_t left_node, uint32_t right_node,
                   chunk_data_oob *oob_data);

  uint64_t first_node_file_no;
  uint64_t first_node_offset;
  uint32_t node_list_len;
  uint32_t node_list_alloc_len;
  /*
    The node_list contains the root of each tree in the forest of perfect
    binary trees.
  */
#ifdef _MSC_VER
/* Flexible array member is not standard C++, disable compiler warning. */
#pragma warning(disable : 4200)
#endif
  struct node_info {
    uint64_t file_no;
    uint64_t offset;
    uint64_t node_index;
    uint32_t height;
  } node_list [];
};


/*
  A class for doing the post-order traversal of the forest of perfect binary
  trees that make up the out-of-band data for a commit record.
*/
class innodb_binlog_oob_reader {
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
    binlog_chunk_reader::saved_position saved_pos;
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
    byte rd_buf[5*COMPR_INT_MAX64];
    /*
      True when the node is reached using only left child pointers, false
      otherwise. Used to identify the left-most leaf in a tree which points to
      a prior tree that must be traversed first.
    */
    bool is_leftmost;
  };
  small_vector<stack_entry, 8>stack;

  /* State machine current state. */
  enum oob_states state;

public:
  innodb_binlog_oob_reader();
  ~innodb_binlog_oob_reader();

  void start_traversal(uint64_t file_no, uint64_t offset);
  bool oob_traversal_done() { return stack.empty(); }
 int read_data(binlog_chunk_reader *chunk_rd, uchar *buf, int max_len);

private:
  void push_state(enum oob_states state, uint64_t file_no, uint64_t offset,
                  bool is_leftmost);
};


class ha_innodb_binlog_reader : public handler_binlog_reader {
  enum reader_states {
    ST_read_next_event_group, ST_read_oob_data, ST_read_commit_record
  };

  binlog_chunk_reader chunk_rd;
  innodb_binlog_oob_reader oob_reader;
  binlog_chunk_reader::saved_position saved_commit_pos;

  /* Buffer to hold a page read directly from the binlog file. */
  uchar *page_buf;
  /* Out-of-band data to read after commit record, if any. */
  uint64_t oob_count;
  uint64_t oob_last_file_no;
  uint64_t oob_last_offset;
  /* Keep track of pending bytes in the rd_buf. */
  uint32_t rd_buf_len;
  uint32_t rd_buf_sofar;
  /* State for state machine reading chunks one by one. */
  enum reader_states state;

  /* Used to read the header of the commit record. */
  byte rd_buf[5*COMPR_INT_MAX64];
private:
  int read_from_file(uint64_t end_offset, uchar *buf, uint32_t len);
  int read_from_page(uchar *page_ptr, uint64_t end_offset,
                     uchar *buf, uint32_t len);
  int read_data(uchar *buf, uint32_t len);

public:
  ha_innodb_binlog_reader(uint64_t file_no= 0, uint64_t offset= 0);
  ~ha_innodb_binlog_reader();
  virtual int read_binlog_data(uchar *buf, uint32_t len) final;
  virtual bool data_available() final;
  virtual int init_gtid_pos(slave_connection_state *pos,
                            rpl_binlog_state_base *state) final;
  virtual int init_legacy_pos(const char *filename, ulonglong offset) final;
  virtual void get_filename(char name[FN_REFLEN], uint64_t file_no) final;
};


struct chunk_data_cache : public chunk_data_base {
  IO_CACHE *cache;
  size_t main_remain;
  size_t gtid_remain;
  uint32_t header_remain;
  uint32_t header_sofar;
  byte header_buf[5*COMPR_INT_MAX64];

  chunk_data_cache(IO_CACHE *cache_arg,
                   handler_binlog_event_group_info *binlog_info)
  : cache(cache_arg),
    main_remain(binlog_info->gtid_offset - binlog_info->out_of_band_offset),
    header_sofar(0)
  {
    size_t end_offset= my_b_tell(cache);
    ut_ad(end_offset > binlog_info->out_of_band_offset);
    ut_ad(binlog_info->gtid_offset >= binlog_info->out_of_band_offset);
    ut_ad(end_offset >= binlog_info->gtid_offset);
    gtid_remain= end_offset - binlog_info->gtid_offset;

    binlog_oob_context *c= (binlog_oob_context *)binlog_info->engine_ptr;
    unsigned char *p;
    if (c && c->node_list_len)
    {
      /*
        Link to the out-of-band data. First store the number of nodes; then
        store 2 x 2 numbers of file_no/offset for the first and last node.
      */
      uint32_t last= c->node_list_len-1;
      uint64_t num_nodes= c->node_list[last].node_index + 1;
      p= compr_int_write(header_buf, num_nodes);
      p= compr_int_write(p, c->first_node_file_no);
      p= compr_int_write(p, c->first_node_offset);
      p= compr_int_write(p, c->node_list[last].file_no);
      p= compr_int_write(p, c->node_list[last].offset);
    }
    else
    {
      /*
        No out-of-band data, marked with a single 0 count for nodes and no
        first/last links.
      */
      p= compr_int_write(header_buf, 0);
    }
    header_remain= (uint32_t)(p - header_buf);
    ut_ad((size_t)(p - header_buf) <= sizeof(header_buf));

    if (cache->pos_in_file > binlog_info->out_of_band_offset) {
      /*
        ToDo: A limitation in mysys IO_CACHE. If I change (reinit_io_cache())
        the cache from WRITE_CACHE to READ_CACHE without seeking out of the
        current buffer, then the cache will not be flushed to disk (which is
        good for small cache that fits completely in buffer). But then if I
        later my_b_seek() or reinit_io_cache() it again and seek out of the
        current buffer, the buffered data will not be flushed to the file
        because the cache is now a READ_CACHE! The result is that the end of the
        cache will be lost if the cache doesn't fit in memory.

        So for now, have to do this somewhat in-elegant conditional flush
        myself.
      */
      flush_io_cache(cache);
    }

    /* Start with the GTID event, which is put at the end of the IO_CACHE. */
    my_bool res= reinit_io_cache(cache, READ_CACHE, binlog_info->gtid_offset, 0, 0);
    ut_a(!res /* ToDo: Error handling. */);
  }
  ~chunk_data_cache() { }

  virtual std::pair<uint32_t, bool> copy_data(byte *p, uint32_t max_len) final
  {
    uint32_t size= 0;
    /* Write header data, if any still available. */
    if (header_remain > 0)
    {
      size= header_remain > max_len ? max_len : (uint32_t)header_remain;
      memcpy(p, header_buf + header_sofar, size);
      header_remain-= size;
      header_sofar+= size;
      max_len-= size;
      if (UNIV_UNLIKELY(max_len == 0))
      {
        ut_ad(gtid_remain + main_remain > 0);
        return {size, false};
      }
    }

    /* Write GTID data, if any still available. */
    ut_ad(header_remain == 0);
    if (gtid_remain > 0)
    {
      uint32_t size2= gtid_remain > max_len ? max_len : (uint32_t)gtid_remain;
      int res2= my_b_read(cache, p + size, size2);
      ut_a(!res2 /* ToDo: Error handling */);
      gtid_remain-= size2;
      if (gtid_remain == 0)
        my_b_seek(cache, 0);    /* Move to read the rest of the events. */
      max_len-= size2;
      size+= size2;
      if (max_len == 0)
        return {size, gtid_remain + main_remain == 0};
    }

    /* Write remaining data. */
    ut_ad(gtid_remain == 0);
    if (main_remain == 0)
    {
      /*
        This means that only GTID data is present, eg. when the main data was
        already binlogged out-of-band.
      */
      ut_ad(size > 0);
      return {size, true};
    }
    uint32_t size2= main_remain > max_len ? max_len : (uint32_t)main_remain;
    int res2= my_b_read(cache, p + size, size2);
    ut_a(!res2 /* ToDo: Error handling */);
    ut_ad(main_remain >= size2);
    main_remain-= size2;
    return {size + size2, main_remain == 0};
  }
};


class gtid_search {
public:
  /*
    Note that this enum is set up to be compatible with int results -1/0/1 for
    error/not found/fount from read_gtid_state_from_page().
  */
  enum Read_Result {
    READ_ENOENT= -2,
    READ_ERROR= -1,
    READ_NOT_FOUND= 0,
    READ_FOUND= 1
  };
  gtid_search();
  ~gtid_search();
  enum Read_Result read_gtid_state_file_no(rpl_binlog_state_base *state,
                                           uint64_t file_no, uint32_t page_no,
                                           uint64_t *out_file_end,
                                           uint32_t *out_diff_state_interval);
  int find_gtid_pos(slave_connection_state *pos,
                    rpl_binlog_state_base *out_state, uint64_t *out_file_no,
                    uint64_t *out_offset);
private:
  uint64_t cur_open_file_no;
  uint64_t cur_open_file_length;
  File cur_open_file;
};


struct found_binlogs {
  uint64_t last_file_no, prev_file_no, earliest_file_no;
  size_t last_size, prev_size, total_size;
  int found_binlogs;
};


/*
  This structure holds the state needed during InnoDB recovery for recovering
  binlog tablespace files.
*/
class binlog_recovery {
public:
  struct found_binlogs scan_result;
  byte *page_buf;
  const char *binlog_dir;
  /*
    The current file number being recovered.
    This starts out as the most recent existing non-empty binlog that has a
    starting LSN no bigger than the recovery starting LSN. This should always be
    one of the two most recent binlog files found at startup.
  */
  uint64_t cur_file_no;
  /* The physical length of cur_file_no file. */
  uint64_t cur_phys_size;
  /*
    The starting LSN (as stored in the header of the binlog tablespace file).
    No redo prior to this LSN should be applied to this file.
  */
  lsn_t start_file_lsn;
  /* Open file for cur_file_no, or -1 if not open. */
  File cur_file_fh;
  /* The sofar position of redo in cur_file_no (end point of previous redo). */
  uint32_t cur_page_no;
  uint32_t cur_page_offset;

  /* The path to cur_file_no. */
  char full_path[OS_FILE_MAX_PATH];

  bool inited;
  /*
    Flag set in case of severe error and --innodb-force_recovery to completely
    skip any binlog recovery.
  */
  bool skip_recovery;
  /*
    Special case, if we start from completely empty (no non-empty binlog files).
    This should recover into an empty binlog state.
  */
  bool start_empty;
  /*
    Special case: The last two files are empty. Then we ignore the last empty
    file and use the 2 previous files instead. The ignored file is deleted only
    after successful recovery, to try to avoid destroying data in case of
    recovery problems.
  */
  bool ignore_last;
  /*
    Mark the case where the first binlog tablespace file we need to consider for
    recovery has file LSN that is later than the first redo record; in this case
    we need to skip records until the first one that applies to this file.
  */
  bool skipping_early_lsn;
  /*
    Skip any initial records until the start of a page. We are guaranteed that
    any page that needs to be recovered will have recovery data for the whole
    page, and this way we never need to read-modify-write pages during recovery.
  */
  bool skipping_partial_page;

  bool init_recovery(bool space_id, uint32_t page_no, uint16_t offset,
                     lsn_t start_lsn, lsn_t lsn,
                     const byte *buf, size_t size) noexcept;
  bool apply_redo(bool space_id, uint32_t page_no, uint16_t offset,
                  lsn_t start_lsn, lsn_t lsn,
                  const byte *buf, size_t size) noexcept;
  int get_header(uint64_t file_no, lsn_t &out_lsn, bool &out_empty) noexcept;
  bool init_recovery_from(uint64_t file_no, lsn_t file_lsn, uint32_t page_no,
                           uint16_t offset, lsn_t lsn,
                           const byte *buf, size_t size) noexcept;
  void init_recovery_empty() noexcept;
  void init_recovery_skip_all() noexcept;
  void end_actions(bool recovery_successful) noexcept;
  void release() noexcept;
  bool open_cur_file() noexcept;
  bool flush_page() noexcept;
  void zero_out_cur_file();
  bool close_file() noexcept;
  bool next_file() noexcept;
  bool next_page() noexcept;
  void update_page_from_record(uint16_t offset,
                               const byte *buf, size_t size) noexcept;
};


static binlog_recovery recover_obj;


static void innodb_binlog_prealloc_thread();
static int scan_for_binlogs(const char *binlog_dir, found_binlogs *binlog_files,
                            bool error_if_missing) noexcept;
static int innodb_binlog_discover();
static bool binlog_state_recover();
static void innodb_binlog_autopurge(uint64_t first_open_file_no);
static int read_gtid_state_from_page(rpl_binlog_state_base *state,
                                     const byte *page, uint32_t page_no,
                                     binlog_header_data *out_header_data);


/*
  Read the header of a binlog tablespace file identified by file_no.
  Sets the out_empty false if the file is empty or has checksum error (or
  is missing).
  Else sets out_empty true and sets out_lsn from the header.

  Returns:
   -1  error
    0  File is missing (ENOENT) or has bad checksum on first page.
    1  File found (but may be empty according to out_empty).
*/
int
binlog_recovery::get_header(uint64_t file_no, lsn_t &out_lsn, bool &out_empty)
  noexcept
{
  char full_path[OS_FILE_MAX_PATH];
  rpl_binlog_state_base dummy_state;
  binlog_header_data header;

  out_empty= true;
  out_lsn= 0;

  binlog_name_make(full_path, file_no, binlog_dir);
  File fh= my_open(full_path, O_RDONLY | O_BINARY, MYF(0));
  if (fh < (File)0)
    return (my_errno == ENOENT ? 0 : -1);
  size_t read= my_pread(fh, page_buf, ibb_page_size, 0, MYF(0));
  my_close(fh, MYF(0));
  if (UNIV_UNLIKELY(read == (size_t)-1))
    return -1;
  if (read == 0)
    return 0;
  /*
    If the crc32 does not match, the page was not written properly, so treat
    it as an empty file.
  */
  const uint32_t payload= (uint32_t)ibb_page_size - BINLOG_PAGE_CHECKSUM;
  uint32_t crc32= mach_read_from_4(page_buf + payload);
  if (UNIV_UNLIKELY(crc32 != my_crc32c(0, page_buf, payload)))
    return 0;

  dummy_state.init();
  int res= read_gtid_state_from_page(&dummy_state, page_buf, 0, &header);
  if (res <= 0)
    return res;
  if (!header.is_empty)
  {
    out_empty= false;
    out_lsn= header.start_lsn;
  }
  return 1;
}


bool binlog_recovery::init_recovery(bool space_id, uint32_t page_no,
                                    uint16_t offset,
                                    lsn_t start_lsn, lsn_t end_lsn,
                                    const byte *buf, size_t size) noexcept
{
  /* Start by initializing resource pointers so we are safe to releaes(). */
  cur_file_fh= (File)-1;
  if (!(page_buf= (byte *)ut_malloc(ibb_page_size, mem_key_binlog)))
  {
    my_error(ER_OUTOFMEMORY, MYF(MY_WME), ibb_page_size);
    return true;
  }
  memset(page_buf, 0, ibb_page_size);
  inited= true;
  /*
    ToDo: It would be good to find a way to not duplicate this logic for
    where the binlog tablespace filess are stored with the code in
    innodb_binlog_init(). But it's a bit awkward, because InnoDB recovery
    runs during plugin init, so not even available for the server to call
    into until after recovery is done.
  */
  binlog_dir= opt_binlog_directory;
  if (!binlog_dir || !binlog_dir[0])
    binlog_dir= ".";
  if (scan_for_binlogs(binlog_dir, &scan_result, true) <= 0)
    return true;

  /*
    Here we find the two most recent, non-empty binlogs to do recovery on.
    Before we allocate binlog tablespace file N+2, we flush and fsync file N
    to disk. This ensures that we only ever need to apply redo records to the
    two most recent files during recovery.

    A special case however arises if the two most recent binlog files are
    both completely empty. Then we do not have any LSN to match against to
    know if a redo record applies to one of these two files, or to an earlier
    file with same value of bit 0 of the file_no. In this case, we ignore the
    most recent file (deleting it later after successful recovery), and
    consider instead the two prior files, the first of which is guaranteed to
    have durably saved a starting LSN to use.

    Hence the loop, which can only ever have one or two iterations.

    A further special case is if there are fewer than two (or three if last
    two are empty) files. If there are no files, or only empty files, then the
    server must have stopped just after RESET MASTER (or just after
    initializing the binlogs at first startup), and we should just start the
    binlogs from scratch.
  */
  ignore_last= false;
  uint64_t file_no2= scan_result.last_file_no;
  uint64_t file_no1= scan_result.prev_file_no;
  int num_binlogs= scan_result.found_binlogs;
  for (;;)
  {
    lsn_t lsn1= 0, lsn2= 0;
    bool is_empty1= true, is_empty2= true;
    int res2= get_header(file_no2, lsn2, is_empty2);

    if (num_binlogs == 0 ||
        (num_binlogs == 1 && is_empty2))
    {
      init_recovery_empty();
      return false;
    }
    if (num_binlogs == 1)
      return init_recovery_from(file_no2 + (space_id != (file_no2 & 1)), lsn2,
                                page_no, offset, start_lsn, buf, size);

    int res1= get_header(file_no1, lsn1, is_empty1);

    if (res2 < 0 && !srv_force_recovery)
    {
      sql_print_error("InnoDB: I/O error reading binlog file number %" PRIu64,
                      file_no2);
      return true;
    }
    if (res1 < 0 && !srv_force_recovery)
    {
      sql_print_error("InnoDB: I/O error reading binlog file number %" PRIu64,
                      file_no1);
      return true;
    }
    if (is_empty1 && is_empty2)
    {
      if (!ignore_last)
      {
        ignore_last= true;
        if (file_no2 > scan_result.earliest_file_no)
        {
          --file_no2;
          if (file_no1 > scan_result.earliest_file_no)
            --file_no1;
          else
            --num_binlogs;
        }
        else
          --num_binlogs;
        continue;
      }
      if (srv_force_recovery)
      {
        /*
          If the last 3 files are empty, we cannot get an LSN to know which
          records apply to each file. This should not happen unless there is
          damage to the file system. If force recovery is requested, we must
          simply do no recovery at all on the binlog files.
        */
        sql_print_warning("InnoDB: Binlog tablespace file recovery is not "
                          "possible. Recovery is skipped due to "
                          "--innodb-force-recovery");
        init_recovery_skip_all();
        return false;
      }
      sql_print_error("InnoDB: Last 3 binlog tablespace files are all empty. "
                      "Recovery is not possible");
      return true;
    }
    if (is_empty2)
      lsn2= lsn1;
    if (space_id == (file_no2 & 1) && start_lsn >= lsn1)
    {
      if (start_lsn < lsn2 && !srv_force_recovery)
      {
        sql_print_error("InnoDB: inconsistent space_id %d for lsn=%" LSN_PF,
                        (int)space_id, start_lsn);
        return true;
      }
      return init_recovery_from(file_no2, lsn2,
                                page_no, offset, start_lsn, buf, size);
    }
    else
      return init_recovery_from(file_no1, lsn1,
                                page_no, offset, start_lsn, buf, size);
    /* NotReached. */
  }
}


bool
binlog_recovery::init_recovery_from(uint64_t file_no, lsn_t file_lsn,
                                    uint32_t page_no, uint16_t offset,
                                    lsn_t lsn, const byte *buf, size_t size)
  noexcept
{
  cur_file_no= file_no;
  cur_phys_size= 0;
  start_file_lsn= file_lsn;
  cur_page_no= page_no;
  cur_page_offset= 0;
  skip_recovery= false;
  start_empty= false;
  skipping_partial_page= true;
  if (lsn < start_file_lsn)
    skipping_early_lsn= true;
  else
  {
    skipping_early_lsn= false;
    if (offset <= BINLOG_PAGE_DATA)
    {
      update_page_from_record(offset, buf, size);
      skipping_partial_page= false;
    }
  }
  return false;
}


/*
  Initialize recovery from the state where there are no binlog files, or only
  completely empty binlog files. In this case we have no file LSN to compare
  redo records against.

  This can only happen if we crash immediately after RESET MASTER (or fresh
  server installation) as an initial file header is durably written to disk
  before binlogging new data. Therefore we should skip _all_ redo records and
  recover into a completely empty state.
*/
void
binlog_recovery::init_recovery_empty() noexcept
{
  cur_file_no= 0;
  cur_phys_size= 0;
  start_file_lsn= (lsn_t)0;
  cur_page_no= 0;
  cur_page_offset= 0;
  skip_recovery= false;
  start_empty= true;
  ignore_last= false;
  skipping_early_lsn= false;
  skipping_partial_page= true;
}


void
binlog_recovery::init_recovery_skip_all() noexcept
{
  skip_recovery= true;
}


void
binlog_recovery::end_actions(bool recovery_successful) noexcept
{
  char full_path[OS_FILE_MAX_PATH];
  if (recovery_successful && !skip_recovery)
  {
    if (!start_empty)
    {
      if (cur_page_offset)
        flush_page();
      if (cur_file_fh > (File)-1)
        zero_out_cur_file();
      close_file();
      ++cur_file_no;
    }

    /*
      Delete any binlog tablespace files following the last recovered file.
      These files could be pre-allocated but never used files, or they could be
      files that were written with data that was eventually not recovered due
      to --innodb-flush-log-at-trx-commit=0|2.
    */
    for (uint64_t i= cur_file_no;
         scan_result.found_binlogs >= 1 && i <= scan_result.last_file_no;
         ++i)
    {
      binlog_name_make(full_path, i, binlog_dir);
      if (my_delete(full_path, MYF(MY_WME)))
        sql_print_warning("InnoDB: Could not delete empty file '%s' ("
                          "error: %d)", full_path, my_errno);
    }
  }
  release();
}


void
binlog_recovery::release() noexcept
{
  if (cur_file_fh >= (File)0)
  {
    my_close(cur_file_fh, MYF(0));
    cur_file_fh= (File)-1;
  }
  ut_free(page_buf);
  page_buf= nullptr;
  inited= false;
}


bool
binlog_recovery::open_cur_file() noexcept
{
  if (cur_file_fh >= (File)0)
    my_close(cur_file_fh, MYF(0));
  binlog_name_make(full_path, cur_file_no, binlog_dir);
  cur_file_fh= my_open(full_path, O_RDWR | O_BINARY, MYF(MY_WME));
  if (cur_file_fh < (File)0)
    return true;
  cur_phys_size= (uint64_t)my_seek(cur_file_fh, 0, MY_SEEK_END, MYF(0));
  return false;
}


bool
binlog_recovery::flush_page() noexcept
{
  if (cur_file_fh < (File)0 &&
      open_cur_file())
    return true;
  size_t res=
    crc32_pwrite_page(cur_file_fh, page_buf, cur_page_no, MYF(MY_WME));
  if (res != ibb_page_size)
    return true;
  cur_page_offset= 0;
  memset(page_buf, 0, ibb_page_size);
  return false;
}


void
binlog_recovery::zero_out_cur_file()
{
  if (cur_file_fh < (File)0)
    return;

  /* Recover the original size from the current file. */
  size_t read= crc32_pread_page(cur_file_fh, page_buf, 0, MYF(0));
  if (read != (size_t)ibb_page_size)
  {
    sql_print_warning("InnoDB: Could not read last binlog file during recovery");
    return;
  }
  binlog_header_data header;
  rpl_binlog_state_base dummy_state;
  dummy_state.init();
  int res= read_gtid_state_from_page(&dummy_state, page_buf, 0, &header);
  if (res <= 0)
  {
    if (res < 0)
      sql_print_warning("InnoDB: Could not read last binlog file during recovery");
    else
      sql_print_warning("InnoDB: Empty binlog file header found during recovery");
    ut_ad(0);
    return;
  }

  /* Fill up or truncate the file to its original size. */
  if (my_chsize(cur_file_fh, (my_off_t)header.page_count << ibb_page_size_shift,
                0, MYF(0)))
    sql_print_warning("InnoDB: Could not change the size of last binlog file "
                      "during recovery (error: %d)", my_errno);
  for (uint32_t i= cur_page_no + 1; i < header.page_count; ++i)
  {
    if (my_pread(cur_file_fh, page_buf, ibb_page_size,
                 (my_off_t)i << ibb_page_size_shift, MYF(0)) <
        (size_t)ibb_page_size)
      break;
    /* Check if page already zeroed out. */
    if (page_buf[0] == 0 && !memcmp(page_buf, page_buf+1, ibb_page_size - 1))
      continue;
    memset(page_buf, 0, ibb_page_size);
    if (my_pwrite(cur_file_fh, page_buf, ibb_page_size,
                  (uint64_t)i << ibb_page_size_shift, MYF(MY_WME)) <
        (size_t)ibb_page_size)
    {
      sql_print_warning("InnoDB: Error writing to last binlog file during "
                        "recovery (error code: %d)", my_errno);
      break;
    }
  }
}


bool
binlog_recovery::close_file() noexcept
{
  if (cur_file_fh >= (File)0)
  {
    if (my_sync(cur_file_fh, MYF(MY_WME)))
      return true;
    my_close(cur_file_fh, (File)0);
    cur_file_fh= (File)-1;
    cur_phys_size= 0;
  }
  return false;
}


bool
binlog_recovery::next_file() noexcept
{
  if (flush_page())
    return true;
  if (close_file())
    return true;
  ++cur_file_no;
  cur_page_no= 0;
  cur_page_offset= 0;
  return false;
}


bool
binlog_recovery::next_page() noexcept
{
  if (flush_page())
    return true;
  ++cur_page_no;
  return false;
}


bool
binlog_recovery::apply_redo(bool space_id, uint32_t page_no, uint16_t offset,
                            lsn_t start_lsn, lsn_t end_lsn,
                            const byte *buf, size_t size) noexcept
{
  if (UNIV_UNLIKELY(skip_recovery) || start_empty)
    return false;

  if (skipping_partial_page)
  {
    if (offset > BINLOG_PAGE_DATA)
      return false;
    skipping_partial_page= false;
  }

  if (skipping_early_lsn)
  {
    if (start_lsn < start_file_lsn || space_id != (cur_file_no & 1))
      return false;  /* Skip record for earlier file that's already durable. */
    /* Now reset the current page to match the real starting point. */
    cur_page_no= page_no;
  }

  if (UNIV_UNLIKELY(start_lsn < start_file_lsn))
  {
    ut_a(!skipping_early_lsn /* Was handled in condition above */);
    if (!srv_force_recovery)
    {
      sql_print_error("InnoDB: Unexpected LSN " LSN_PF " during recovery, "
                      "expected at least " LSN_PF, start_lsn, start_file_lsn);
      return true;
    }
    sql_print_warning("InnoDB: Ignoring unexpected LSN " LSN_PF " during "
                      "recovery, ", start_lsn);
    return false;
  }
  skipping_early_lsn= false;

  /* Test for moving to the next file. */
  if (space_id != (cur_file_no & 1))
  {
    /* Check that we recovered all of this file. */
    if ( ( (cur_page_offset > BINLOG_PAGE_DATA &&
            cur_page_offset < ibb_page_size - BINLOG_PAGE_DATA_END) ||
           cur_page_no + (cur_page_offset > BINLOG_PAGE_DATA) <
           cur_phys_size >> ibb_page_size_shift) &&
         !srv_force_recovery)
    {
      sql_print_error("InnoDB: Missing recovery record at end of file_no=%"
                      PRIu64 ", LSN " LSN_PF, cur_file_no, start_lsn);
      return true;
    }

    /* Check that we recover from the start of the next file. */
    if ((page_no > 0 || offset > BINLOG_PAGE_DATA) && !srv_force_recovery)
    {
      sql_print_error("InnoDB: Missing recovery record at start of file_no=%"
                      PRIu64 ", LSN " LSN_PF, cur_file_no+1, start_lsn);
      return true;
    }

    if (next_file())
      return true;
  }
  /* Test for moving to the next page. */
  else if (page_no != cur_page_no)
  {
    if (cur_page_offset < ibb_page_size - BINLOG_PAGE_DATA_END &&
        !srv_force_recovery)
    {
      sql_print_error("InnoDB: Missing recovery record in file_no=%"
                      PRIu64 ", page_no=%u, LSN " LSN_PF,
                      cur_file_no, cur_page_no, start_lsn);
      return true;
    }

    if ((page_no != cur_page_no + 1 || offset > BINLOG_PAGE_DATA) &&
        !srv_force_recovery)
    {
      sql_print_error("InnoDB: Missing recovery record in file_no=%"
                      PRIu64 ", page_no=%u, LSN " LSN_PF,
                      cur_file_no, cur_page_no + 1, start_lsn);
      return true;
    }

    if (next_page())
      return true;
  }
  /* Test no gaps in offset. */
  else if (offset != cur_page_offset &&
           offset > BINLOG_PAGE_DATA &&
           !srv_force_recovery)
  {
      sql_print_error("InnoDB: Missing recovery record in file_no=%"
                      PRIu64 ", page_no=%u, LSN " LSN_PF,
                      cur_file_no, cur_page_no, start_lsn);
      return true;
  }

  if (offset + size >= ibb_page_size)
    return !srv_force_recovery;

  update_page_from_record(offset, buf, size);
  return false;
}


void
binlog_recovery::update_page_from_record(uint16_t offset,
                                         const byte *buf, size_t size) noexcept
{
  memcpy(page_buf + offset, buf, size);
  cur_page_offset= offset + (uint32_t)size;
}


/*
  Check if this is an InnoDB binlog file name.
  Return the index/file_no if so.
*/
static bool
is_binlog_name(const char *name, uint64_t *out_idx)
{
  const size_t base_len= sizeof(BINLOG_NAME_BASE) - 1;  // Length without '\0' terminator
  const size_t ext_len= sizeof(BINLOG_NAME_EXT) - 1;

  if (0 != strncmp(name, BINLOG_NAME_BASE, base_len))
    return false;
  size_t name_len= strlen(name);
  if (name_len < base_len + 1 + ext_len)
    return false;
  const char *ext_start= name + (name_len - ext_len);
  if (0 != strcmp(ext_start, BINLOG_NAME_EXT))
    return false;
  if (!std::isdigit((unsigned char)(name[base_len])))
    return false;
  char *conv_end= nullptr;
  unsigned long long idx= std::strtoull(name + base_len, &conv_end, 10);
  if (idx == ULLONG_MAX || conv_end != ext_start)
    return false;

  *out_idx= (uint64_t)idx;
  return true;
}


void
innodb_binlog_startup_init()
{
  fsp_binlog_init();
  mysql_mutex_init(fsp_purge_binlog_mutex_key, &purge_binlog_mutex, nullptr);
  binlog_diff_state.init();
  innodb_binlog_inited= 1;
}


static void
innodb_binlog_init_state()
{
  first_open_binlog_file_no= ~(uint64_t)0;
  binlog_cur_end_offset[0].store(~(uint64_t)0, std::memory_order_relaxed);
  binlog_cur_end_offset[1].store(~(uint64_t)0, std::memory_order_relaxed);
  last_created_binlog_file_no= ~(uint64_t)0;
  earliest_binlog_file_no= ~(uint64_t)0;
  total_binlog_used_size= 0;
  active_binlog_file_no.store(~(uint64_t)0, std::memory_order_release);
  binlog_cur_page_no= 0;
  binlog_cur_page_offset= BINLOG_PAGE_DATA;
  current_binlog_state_interval=
    (uint32_t)(innodb_binlog_state_interval >> ibb_page_size_shift);
  ut_a(innodb_binlog_state_interval ==
       (current_binlog_state_interval << ibb_page_size_shift));
}


/* Start the thread that pre-allocates new binlog files. */
static void
start_binlog_prealloc_thread()
{
  prealloc_thread_end= false;
  binlog_prealloc_thr_obj= std::thread{innodb_binlog_prealloc_thread};

  mysql_mutex_lock(&active_binlog_mutex);
  while (last_created_binlog_file_no == ~(uint64_t)0) {
    /* Wait for the first binlog file to be available. */
    my_cond_wait(&active_binlog_cond, &active_binlog_mutex.m_mutex);
  }
  mysql_mutex_unlock(&active_binlog_mutex);
}


/*
  Write the initial header record to the file and durably sync it to disk in
  the binlog tablespace file and in the redo log.

  This is to ensure recovery can work correctly. This way, recovery will
  always find a non-empty file with an initial lsn to start recovery from.
  Except in the case where we crash right here; in this case recovery will
  find no binlog files at all and will know to recover to the empty state
  with no binlog files present.
*/
static void
binlog_sync_initial()
{
  chunk_data_flush dummy_data;
  mtr_t mtr;
  mtr.start();
  fsp_binlog_write_rec(&dummy_data, &mtr, FSP_BINLOG_TYPE_FILLER);
  mtr.commit();
  log_buffer_flush_to_disk(true);
  binlog_page_fifo->flush_up_to(0, 0);
}


/*
  Open the InnoDB binlog implementation.
  This is called from server binlog layer if the user configured the binlog to
  use the innodb implementation (with --binlog-storage-engine=innodb).
*/
bool
innodb_binlog_init(size_t binlog_size, const char *directory)
{
  uint64_t pages= binlog_size >> ibb_page_size_shift;
  if (UNIV_LIKELY(pages > (uint64_t)UINT32_MAX)) {
    pages= UINT32_MAX;
    sql_print_warning("Requested max_binlog_size is larger than the maximum "
                      "InnoDB tablespace size, truncated to %llu",
                      (pages << ibb_page_size_shift));
  } else if (pages < 2) {  /* Minimum one data page and one index page. */
    pages= 2;
    sql_print_warning("Requested max_binlog_size is smaller than the minimum "
                      "size supported by InnoDB, truncated to %llu",
                      (pages << ibb_page_size_shift));
  }
  innodb_binlog_size_in_pages= (uint32_t)pages;

  if (!directory || !directory[0])
    directory= ".";
  else if (strlen(directory) + BINLOG_NAME_MAX_LEN > OS_FILE_MAX_PATH)
  {
    sql_print_error("Specified binlog directory path '%s' is too long",
                    directory);
    return true;
  }
  innodb_binlog_directory= directory;

  innodb_binlog_init_state();
  innodb_binlog_inited= 2;

  /* Find any existing binlog files and continue writing in them. */
  int res= innodb_binlog_discover();
  if (res < 0)
  {
    /* Need to think more on the error handling if the binlog cannot be opened. We may need to abort starting the server, at least for some errors? And/or in some cases maybe force ignore any existing unusable files and continue with a new binlog (but then maybe innodb_binlog_discover() should return 0 and print warnings in the error log?). */
    return true;
  }
  if (res > 0)
  {
    /* We are continuing from existing binlogs. Recover the binlog state. */
    if (binlog_state_recover())
      return true;
  }

  start_binlog_prealloc_thread();
  binlog_sync_initial();

  return false;
}


/* Compute the (so far) last and last-but-one binlog files found. */
static void
process_binlog_name(found_binlogs *bls, uint64_t idx, size_t size)
{
  if (bls->found_binlogs == 0 ||
      idx > bls->last_file_no) {
    if (bls->found_binlogs >= 1 && idx == bls->last_file_no + 1) {
      bls->prev_file_no= bls->last_file_no;
      bls->prev_size= bls->last_size;
      bls->found_binlogs= 2;
    } else {
      bls->found_binlogs= 1;
    }
    bls->last_file_no= idx;
    bls->last_size= size;
  } else if (bls->found_binlogs == 1 && idx + 1 == bls->last_file_no) {
    bls->found_binlogs= 2;
    bls->prev_file_no= idx;
    bls->prev_size= size;
  }

  if (bls->found_binlogs == 0)
  {
    bls->earliest_file_no= idx;
    bls->total_size= size;
  }
  else
  {
    if (idx < bls->earliest_file_no)
      bls->earliest_file_no= idx;
    bls->total_size+= size;
  }
}


/*
  Scan the binlog directory for binlog files.
  Returns:
    1 Success
    0 Binlog directory not found
   -1 Other error
*/
static int
scan_for_binlogs(const char *binlog_dir, found_binlogs *binlog_files,
                 bool error_if_missing) noexcept
{
  MY_DIR *dir= my_dir(binlog_dir, MYF(MY_WANT_STAT));
  if (!dir)
  {
    if (my_errno != ENOENT || error_if_missing)
      sql_print_error("Could not read the binlog directory '%s', error code %d",
                      binlog_dir, my_errno);
    return (my_errno == ENOENT ? 0 : -1);
  }

  binlog_files->found_binlogs= 0;
  size_t num_entries= dir->number_of_files;
  fileinfo *entries= dir->dir_entry;
  for (size_t i= 0; i < num_entries; ++i) {
    const char *name= entries[i].name;
    uint64_t idx;
    if (!is_binlog_name(name, &idx))
      continue;
    process_binlog_name(binlog_files, idx, entries[i].mystat->st_size);
  }
  my_dirend(dir);

  return 1;  /* Success */
}


static bool
binlog_page_empty(const byte *page)
{
  /* ToDo: Here we also need to see if there is a full state record at the start of the file. If not, we have to delete the file and ignore it, it is an incomplete file. Or can we rely on the innodb crash recovery to make file creation atomic and we will never see a partially pre-allocated file? Also if the gtid state is larger than mtr max size (if there is such max?), or if we crash in the middle of pre-allocation? */
  return page[BINLOG_PAGE_DATA] == 0;
}


/*
  Find the last written position in the binlog file.
  Do a binary search through the pages to find the last non-empty page, then
  scan the page to find the place to start writing new binlog data.

  Returns:
     1 position found, output in *out_space, *out_page_no and *out_pos_in_page.
     0 binlog file is empty.
    -1 error.
*/

static int
find_pos_in_binlog(uint64_t file_no, size_t file_size, byte *page_buf,
                   uint32_t *out_page_no, uint32_t *out_pos_in_page)
{
  const uint32_t page_size= (uint32_t)ibb_page_size;
  const uint32_t page_size_shift= (uint32_t)ibb_page_size_shift;
  const uint32_t idx= file_no & 1;
  char file_name[OS_FILE_MAX_PATH];
  uint32_t p_0, p_1, p_2, last_nonempty;
  dberr_t err;
  byte *p, *page_end;
  bool ret;

  *out_page_no= 0;
  *out_pos_in_page= BINLOG_PAGE_DATA;

  binlog_name_make(file_name, file_no);
  pfs_os_file_t fh= os_file_create(innodb_data_file_key, file_name,
                                   OS_FILE_OPEN, OS_DATA_FILE,
                                   srv_read_only_mode, &ret);
  if (!ret) {
    sql_print_warning("Unable to open file '%s'", file_name);
    return -1;
  }

  err= os_file_read(IORequestRead, fh, page_buf, 0, page_size, nullptr);
  if (err != DB_SUCCESS) {
    os_file_close(fh);
    return -1;
  }
  if (binlog_page_empty(page_buf)) {
    ret=
      fsp_binlog_open(file_name, fh, file_no, file_size, ~(uint32_t)0, nullptr);
    binlog_cur_written_offset[idx].store(0, std::memory_order_relaxed);
    binlog_cur_end_offset[idx].store(0, std::memory_order_relaxed);
    return (ret ? -1 : 0);
  }
  last_nonempty= 0;

  /*
    During the binary search, p_0-1 is the largest page number that is know to
    be non-empty. And p_2 is the first page that is known to be empty.
  */
  p_0= 1;
  p_2= (uint32_t)(file_size / page_size);
  for (;;) {
    if (p_0 == p_2)
      break;
    ut_ad(p_0 < p_2);
    p_1= (p_0 + p_2) / 2;
    err= os_file_read(IORequestRead, fh, page_buf, p_1 << page_size_shift,
                      page_size, nullptr);
    if (err != DB_SUCCESS) {
      os_file_close(fh);
      return -1;
    }
    if (binlog_page_empty(page_buf)) {
      p_2= p_1;
    } else {
      p_0= p_1 + 1;
      last_nonempty= p_1;
    }
  }
  /* At this point, p_0 == p_2 is the first empty page. */
  ut_ad(p_0 >= 1);

  /*
    This sometimes does an extra read, but as this is only during startup it
    does not matter.
  */
  err= os_file_read(IORequestRead, fh, page_buf,
                    last_nonempty << page_size_shift, page_size, nullptr);
  if (err != DB_SUCCESS) {
    os_file_close(fh);
    return -1;
  }

  /* Now scan the last page to find the position in it to continue. */
  p= &page_buf[BINLOG_PAGE_DATA];
  page_end= &page_buf[page_size - BINLOG_PAGE_DATA_END];
  while (*p && p < page_end) {
    if (*p == FSP_BINLOG_TYPE_FILLER) {
      p= page_end;
      break;
    }
    p += 3 + (((uint32_t)p[2] << 8) | ((uint32_t)p[1] & 0xff));
    // ToDo: How to handle page corruption?
    ut_a(p <= page_end);
  }

  *out_page_no= p_0 - 1;
  *out_pos_in_page= (uint32_t)(p - page_buf);

  if (*out_pos_in_page >= page_size - BINLOG_PAGE_DATA_END)
    ret= fsp_binlog_open(file_name, fh, file_no, file_size, p_0, nullptr);
  else
    ret= fsp_binlog_open(file_name, fh, file_no, file_size, p_0 - 1, page_buf);
  uint64_t pos= (*out_page_no << page_size_shift) | *out_pos_in_page;
  binlog_cur_written_offset[idx].store(pos, std::memory_order_relaxed);
  binlog_cur_end_offset[idx].store(pos, std::memory_order_relaxed);
  return ret ? -1 : 1;
}


/*
  Returns:
    -1     error
     0     No binlogs found
     1     Just one binlog file found
     2     Found two (or more) existing binlog files
*/
static int
innodb_binlog_discover()
{
  uint64_t file_no;
  const uint32_t page_size= (uint32_t)ibb_page_size;
  const uint32_t page_size_shift= (uint32_t)ibb_page_size_shift;
  struct found_binlogs UNINIT_VAR(binlog_files);

  int res= scan_for_binlogs(innodb_binlog_directory, &binlog_files, false);
  if (res <= 0)
    return res;

  /*
    Now, if we found any binlog files, locate the point in one of them where
    binlogging stopped, and where we should continue writing new binlog data.
  */
  uint32_t page_no, prev_page_no, pos_in_page, prev_pos_in_page;
  std::unique_ptr<byte, void (*)(void *)>
    page_buf(static_cast<byte*>(aligned_malloc(page_size, page_size)),
             &aligned_free);
  if (!page_buf)
    return -1;
  if (binlog_files.found_binlogs >= 1) {
    earliest_binlog_file_no= binlog_files.earliest_file_no;
    total_binlog_used_size= binlog_files.total_size;

    res= find_pos_in_binlog(binlog_files.last_file_no,
                            binlog_files.last_size,
                            page_buf.get(), &page_no, &pos_in_page);
    if (res < 0) {
      file_no= binlog_files.last_file_no;
      active_binlog_file_no.store(file_no, std::memory_order_release);
      sql_print_warning("Binlog number %llu could no be opened. Starting a new "
                        "binlog file from number %llu",
                        binlog_files.last_file_no, (file_no + 1));
      return 0;
    }

    if (res > 0) {
      /* Found start position in the last binlog file. */
      file_no= binlog_files.last_file_no;
      active_binlog_file_no.store(file_no, std::memory_order_release);
      binlog_cur_page_no= page_no;
      binlog_cur_page_offset= pos_in_page;
      ib::info() << "Continuing binlog number " << file_no << " from position "
                 << (((uint64_t)page_no << page_size_shift) | pos_in_page)
                 << ".";
      return binlog_files.found_binlogs;
    }

    /* res == 0, the last binlog is empty. */
    if (binlog_files.found_binlogs >= 2) {
      /* The last binlog is empty, try the previous one. */
      res= find_pos_in_binlog(binlog_files.prev_file_no,
                              binlog_files.prev_size,
                              page_buf.get(),
                              &prev_page_no, &prev_pos_in_page);
      if (res < 0) {
        file_no= binlog_files.last_file_no;
        active_binlog_file_no.store(file_no, std::memory_order_release);
        binlog_cur_page_no= page_no;
        binlog_cur_page_offset= pos_in_page;
        sql_print_warning("Binlog number %llu could not be opened, starting "
                          "from binlog number %llu instead",
                          binlog_files.prev_file_no, file_no);
        return 1;
      }
      file_no= binlog_files.prev_file_no;
      active_binlog_file_no.store(file_no, std::memory_order_release);
      binlog_cur_page_no= prev_page_no;
      binlog_cur_page_offset= prev_pos_in_page;
      ib::info() << "Continuing binlog number " << file_no << " from position "
                 << (((uint64_t)prev_page_no << page_size_shift) |
                     prev_pos_in_page)
                 << ".";
      return binlog_files.found_binlogs;
    }

    /* Just one empty binlog file found. */
    file_no= binlog_files.last_file_no;
    active_binlog_file_no.store(file_no, std::memory_order_release);
    binlog_cur_page_no= page_no;
    binlog_cur_page_offset= pos_in_page;
    ib::info() << "Continuing binlog number " << file_no << " from position "
               << BINLOG_PAGE_DATA << ".";
    return binlog_files.found_binlogs;
  }

  /* No binlog files found, start from scratch. */
  file_no= 0;
  earliest_binlog_file_no= 0;
  total_binlog_used_size= 0;
  ib::info() << "Starting a new binlog from file number " << file_no << ".";
  return 0;
}


void innodb_binlog_close(bool shutdown)
{
  if (innodb_binlog_inited >= 2)
  {
    if (binlog_prealloc_thr_obj.joinable()) {
      mysql_mutex_lock(&active_binlog_mutex);
      prealloc_thread_end= true;
      pthread_cond_signal(&active_binlog_cond);
      mysql_mutex_unlock(&active_binlog_mutex);
      binlog_prealloc_thr_obj.join();
    }

    uint64_t file_no= first_open_binlog_file_no;
    if (file_no != ~(uint64_t)0) {
      if (file_no <= last_created_binlog_file_no) {
        fsp_binlog_tablespace_close(file_no);
        if (file_no + 1 <= last_created_binlog_file_no) {
          fsp_binlog_tablespace_close(file_no + 1);
        }
      }
    }
  }

  if (shutdown && innodb_binlog_inited >= 1)
  {
    binlog_diff_state.free();
    mysql_mutex_destroy(&purge_binlog_mutex);
    fsp_binlog_shutdown();
  }
}


/*
  Background thread to close old binlog tablespaces and pre-allocate new ones.
*/
static void
innodb_binlog_prealloc_thread()
{
  my_thread_init();
#ifdef UNIV_PFS_THREAD
  pfs_register_thread(binlog_prealloc_thread_key);
#endif

  mysql_mutex_lock(&active_binlog_mutex);
  while (1)
  {
    uint64_t active= active_binlog_file_no.load(std::memory_order_relaxed);
    uint64_t first_open= first_open_binlog_file_no;

    /* Pre-allocate the next tablespace (if not done already). */
    uint64_t last_created= last_created_binlog_file_no;
    if (last_created <= active && last_created <= first_open) {
      ut_ad(last_created == active);
      ut_ad(last_created == first_open || first_open == ~(uint64_t)0);
      /*
        Note: `last_created` is initialized to ~0, so incrementing it here
        makes us start from binlog file 0.
      */
      ++last_created;
      mysql_mutex_unlock(&active_binlog_mutex);

      mysql_mutex_lock(&purge_binlog_mutex);
      uint32_t size_in_pages=  innodb_binlog_size_in_pages;
      dberr_t res2= fsp_binlog_tablespace_create(last_created, size_in_pages);
      if (earliest_binlog_file_no == ~(uint64_t)0)
        earliest_binlog_file_no= last_created;
      total_binlog_used_size+= (size_in_pages << ibb_page_size_shift);

      innodb_binlog_autopurge(first_open);
      mysql_mutex_unlock(&purge_binlog_mutex);

      mysql_mutex_lock(&active_binlog_mutex);
      ut_a(res2 == DB_SUCCESS /* ToDo: Error handling. */);
      last_created_binlog_file_no= last_created;

      /* If we created the initial tablespace file, make it the active one. */
      ut_ad(active < ~(uint64_t)0 || last_created == 0);
      if (active == ~(uint64_t)0) {
        active_binlog_file_no.store(last_created, std::memory_order_relaxed);
      }
      if (first_open == ~(uint64_t)0)
        first_open_binlog_file_no= first_open= last_created;

      pthread_cond_signal(&active_binlog_cond);
      continue;  /* Re-start loop after releasing/reacquiring mutex. */
    }

    /*
      Flush out to disk and close any binlog tablespace that has been
      completely written.
    */
    if (first_open < active) {
      ut_ad(first_open == active - 1);
      mysql_mutex_unlock(&active_binlog_mutex);
      fsp_binlog_tablespace_close(active - 1);
      mysql_mutex_lock(&active_binlog_mutex);
      first_open_binlog_file_no= first_open + 1;
      binlog_cur_end_offset[first_open & 1].store(~(uint64_t)0,
                                                  std::memory_order_relaxed);
      continue;  /* Re-start loop after releasing/reacquiring mutex. */
    }

    /* Exit thread at server shutdown. */
    if (prealloc_thread_end)
      break;
    my_cond_wait(&active_binlog_cond, &active_binlog_mutex.m_mutex);

  }
  mysql_mutex_unlock(&active_binlog_mutex);

  my_thread_end();

#ifdef UNIV_PFS_THREAD
  pfs_delete_thread();
#endif
}


__attribute__((noinline))
static ssize_t
serialize_gtid_state(rpl_binlog_state_base *state, byte *buf, size_t buf_size,
                     uint32_t file_size_in_pages, uint64_t file_no,
                     bool is_first_page)
{
  unsigned char *p= (unsigned char *)buf;
  /*
    1 uint64_t for the current LSN at start of binlog file.
    1 uint64_t for the file_no.
    1 uint32_t for the file size in pages.
    1 uint32_t for the innodb_binlog_state_interval in pages.
    1 uint64_t for the number of entries in the state stored.
    2 uint32_t + 1 uint64_t for at least one GTID.
  */
  ut_ad(buf_size >= 4*COMPR_INT_MAX32 + 4*COMPR_INT_MAX64);
  if (is_first_page) {
    /*
      In the first page where we put the full state, include the value of the
      setting for the interval at which differential states are binlogged, so
      we know how to search them independent of how the setting changes.

      We also include the current LSN for recovery purposes; and the file
      length and file_no, which is also useful if we have to recover the whole
      file from the redo log after a crash.
    */
    p= compr_int_write(p, log_get_lsn());
    p= compr_int_write(p, file_no);
    p= compr_int_write(p, file_size_in_pages);
    /* ToDo: Check that this current_binlog_state_interval is the correct value! */
    p= compr_int_write(p, current_binlog_state_interval);
  }
  p= compr_int_write(p, state->count_nolock());
  unsigned char * const pmax=
    p + (buf_size - (2*COMPR_INT_MAX32 + COMPR_INT_MAX64));

  if (state->iterate(
    [pmax, &p] (const rpl_gtid *gtid) {
      if (UNIV_UNLIKELY(p > pmax))
        return true;
      p= compr_int_write(p, gtid->domain_id);
      p= compr_int_write(p, gtid->server_id);
      p= compr_int_write(p, gtid->seq_no);
      return false;
    }))
    return -1;
  else
    return p - (unsigned char *)buf;
}


bool
binlog_gtid_state(rpl_binlog_state_base *state, mtr_t *mtr,
                  fsp_binlog_page_entry * &block, uint32_t &page_no,
                  uint32_t &page_offset, uint64_t file_no,
                  uint32_t file_size_in_pages)
{
  /*
    Use a small, efficient stack-allocated buffer by default, falling back to
    malloc() if needed for large GTID state.
  */
  byte small_buf[192];
  byte *buf, *alloced_buf;
  uint32_t block_page_no= ~(uint32_t)0;
  block= nullptr;

  ssize_t used_bytes= serialize_gtid_state(state, small_buf, sizeof(small_buf),
                                           file_size_in_pages, file_no,
                                           page_no==0);
  if (used_bytes >= 0)
  {
    buf= small_buf;
    alloced_buf= nullptr;
  }
  else
  {
    size_t buf_size=
      state->count_nolock() * (2*COMPR_INT_MAX32 + COMPR_INT_MAX64);
    alloced_buf= (byte *)ut_malloc(buf_size, mem_key_binlog);
    if (UNIV_UNLIKELY(!alloced_buf))
      return true;
    buf= alloced_buf;
    used_bytes= serialize_gtid_state(state, buf, buf_size, file_size_in_pages,
                                     file_no, page_no==0);
    if (UNIV_UNLIKELY(used_bytes < 0))
    {
      ut_ad(0 /* Shouldn't happen, as we allocated maximum needed size. */);
      ut_free(alloced_buf);
      return true;
    }
  }

  const uint32_t page_size= (uint32_t)ibb_page_size;
  const uint32_t page_room= page_size - (BINLOG_PAGE_DATA + BINLOG_PAGE_DATA_END);
  uint32_t needed_pages= (uint32_t)((used_bytes + page_room - 1) / page_room);

  /* For now, GTID state always at the start of a page. */
  ut_ad(page_offset == BINLOG_PAGE_DATA);

  /*
    Only write the GTID state record if there is room for actual event data
    afterwards. There is no point in using space to allow fast search to a
    point if there is no data to search for after that point.
  */
  if (page_no + needed_pages < binlog_page_fifo->size_in_pages(file_no))
  {
    byte cont_flag= 0;
    while (used_bytes > 0)
    {
      ut_ad(page_no < binlog_page_fifo->size_in_pages(file_no));
      if (block)
        binlog_page_fifo->release_page_mtr(block, mtr);
      block_page_no= page_no;
      block= binlog_page_fifo->create_page(file_no, block_page_no);
      ut_a(block /* ToDo: error handling? */);
      page_offset= BINLOG_PAGE_DATA;
      byte *ptr= page_offset + &block->page_buf[0];
      ssize_t chunk= used_bytes;
      byte last_flag= FSP_BINLOG_FLAG_LAST;
      if (chunk > page_room - 3) {
        last_flag= 0;
        chunk= page_room - 3;
        ++page_no;
      }
      ptr[0]= FSP_BINLOG_TYPE_GTID_STATE | cont_flag | last_flag;
      ptr[1] = (byte)chunk & 0xff;
      ptr[2] = (byte)(chunk >> 8);
      ut_ad(chunk <= 0xffff);
      memcpy(ptr+3, buf, chunk);
      fsp_log_binlog_write(mtr, block, page_offset, (uint32)(chunk+3));
      page_offset+= (uint32_t)(chunk+3);
      buf+= chunk;
      used_bytes-= chunk;
      cont_flag= FSP_BINLOG_FLAG_CONT;
    }

    if (page_offset == page_size - BINLOG_PAGE_DATA_END) {
      if (block)
        binlog_page_fifo->release_page_mtr(block, mtr);
      block= nullptr;
      block_page_no= ~(uint32_t)0;
      page_offset= BINLOG_PAGE_DATA;
      ++page_no;
    }
  }
  ut_free(alloced_buf);

  /* Make sure we return a page for caller to write the main event data into. */
  if (UNIV_UNLIKELY(!block)) {
    block= binlog_page_fifo->create_page(file_no, page_no);
    ut_a(block /* ToDo: error handling? */);
  }

  return false;  // No error
}


/*
  Read a binlog state record from a page in a buffer. The passed in STATE
  object is updated with the state read.

  Returns:
    1  State record found
    0  No state record found
    -1 Error
*/
static int
read_gtid_state_from_page(rpl_binlog_state_base *state, const byte *page,
                          uint32_t page_no, binlog_header_data *out_header_data)
{
  const byte *p= page + BINLOG_PAGE_DATA;
  byte t= *p;
  if (UNIV_UNLIKELY((t & FSP_BINLOG_TYPE_MASK) != FSP_BINLOG_TYPE_GTID_STATE))
  {
    out_header_data->is_empty= binlog_page_empty(page);
    return 0;
  }
  out_header_data->is_empty= false;
  /* ToDo: Handle reading a state that spans multiple pages. For now, we assume the state fits in a single page. */
  ut_a(t & FSP_BINLOG_FLAG_LAST);

  uint32_t len= ((uint32_t)p[2] << 8) | p[1];
  const byte *p_end= p + 3 + len;
  if (UNIV_UNLIKELY(p + 3 >= p_end))
    return -1;
  std::pair<uint64_t, const unsigned char *> v_and_p= compr_int_read(p + 3);
  p= v_and_p.second;
  if (page_no == 0)
  {
    /*
      The state in the first page has four extra words: The start LSN of the
      file; the file_no of the file; the file length, in pages; and the offset
      between differential binlog states logged regularly in the binlog
      tablespace.
    */
    if (UNIV_UNLIKELY(p >= p_end))
      return -1;
    out_header_data->start_lsn= (uint32_t)v_and_p.first;
    v_and_p= compr_int_read(p);
    p= v_and_p.second;
    if (UNIV_UNLIKELY(p >= p_end))
      return -1;
    out_header_data->file_no= v_and_p.first;
    v_and_p= compr_int_read(p);
    p= v_and_p.second;
    if (UNIV_UNLIKELY(p >= p_end) || UNIV_UNLIKELY(v_and_p.first >= UINT32_MAX))
      return -1;
    out_header_data->page_count= (uint32_t)v_and_p.first;
    v_and_p= compr_int_read(p);
    p= v_and_p.second;
    if (UNIV_UNLIKELY(p >= p_end) || UNIV_UNLIKELY(v_and_p.first >= UINT32_MAX))
      return -1;
    out_header_data->diff_state_interval= (uint32_t)v_and_p.first;
    v_and_p= compr_int_read(p);
    p= v_and_p.second;
  }
  else
  {
    out_header_data->start_lsn= 0;
    out_header_data->file_no= ~(uint64_t)0;
    out_header_data->page_count= 0;
    out_header_data->diff_state_interval= 0;
  }

  if (UNIV_UNLIKELY(p > p_end))
    return -1;

  for (uint64_t count= v_and_p.first; count > 0; --count)
  {
    rpl_gtid gtid;
    if (UNIV_UNLIKELY(p >= p_end))
      return -1;
    v_and_p= compr_int_read(p);
    if (UNIV_UNLIKELY(v_and_p.first > UINT32_MAX))
      return -1;
    gtid.domain_id= (uint32_t)v_and_p.first;
    p= v_and_p.second;
    if (UNIV_UNLIKELY(p >= p_end))
      return -1;
    v_and_p= compr_int_read(p);
    if (UNIV_UNLIKELY(v_and_p.first > UINT32_MAX))
      return -1;
    gtid.server_id= (uint32_t)v_and_p.first;
    p= v_and_p.second;
    if (UNIV_UNLIKELY(p >= p_end))
      return -1;
    v_and_p= compr_int_read(p);
    gtid.seq_no= v_and_p.first;
    p= v_and_p.second;
    if (UNIV_UNLIKELY(p > p_end))
      return -1;
    if (state->update_nolock(&gtid))
      return -1;
  }

  /*
    For now, we expect no more data.
    Later it could be extended, as we store (and read) the count of GTIDs.
  */
  ut_ad(p == p_end);

  return 1;
}


/*
  Read a binlog state record from a specific page in a file. The passed in
  STATE object is updated with the state read.

  Returns:
    1  State record found
    0  No state record found
    -1 Error
*/
static int
read_gtid_state(rpl_binlog_state_base *state, File file, uint32_t page_no,
                binlog_header_data *out_header_data)
{
  std::unique_ptr<byte [], void (*)(void *)> page_buf
    ((byte *)my_malloc(PSI_NOT_INSTRUMENTED, ibb_page_size, MYF(MY_WME)),
     &my_free);
  if (UNIV_UNLIKELY(!page_buf))
    return -1;

  /* ToDo: Handle encryption. */
  size_t res= crc32_pread_page(file, page_buf.get(), page_no, MYF(MY_WME));
  if (UNIV_UNLIKELY(res == (size_t)-1))
    return -1;

  return read_gtid_state_from_page(state, page_buf.get(), page_no,
                                   out_header_data);
}


/*
  Recover the GTID binlog state at startup.
  Read the full binlog state at the start of the current binlog file, as well
  as the last differential binlog state on top, if any. Then scan from there to
  the end to obtain the exact current GTID binlog state.

  Return false if ok, true if error.
*/
static bool
binlog_state_recover()
{
  binlog_header_data header_data;
  rpl_binlog_state_base state;
  state.init();
  uint32_t diff_state_interval= 0;
  uint32_t page_no= 0;
  char filename[OS_FILE_MAX_PATH];

  binlog_name_make(filename,
                   active_binlog_file_no.load(std::memory_order_relaxed));
  File file= my_open(filename, O_RDONLY | O_BINARY, MYF(MY_WME));
  if (UNIV_UNLIKELY(file < (File)0))
    return true;

  int res= read_gtid_state(&state, file, page_no, &header_data);
  if (res < 0)
  {
    my_close(file, MYF(0));
    return true;
  }
  diff_state_interval= header_data.diff_state_interval;
  if (diff_state_interval == 0)
  {
    sql_print_warning("Invalid differential binlog state interval %llu found "
                      "in binlog file, ignoring", diff_state_interval);
    current_binlog_state_interval= 0;  /* Disable in this binlog file */
  }
  else
  {
    current_binlog_state_interval= diff_state_interval;
    page_no= (uint32_t)(binlog_cur_page_no -
                        (binlog_cur_page_no % diff_state_interval));
    while (page_no > 0)
    {
      res= read_gtid_state(&state, file, page_no, &header_data);
      if (res > 0)
        break;
      page_no-= (uint32_t)diff_state_interval;
    }
  }
  my_close(file, MYF(0));

  ha_innodb_binlog_reader reader(active_binlog_file_no.load
                                   (std::memory_order_relaxed),
                                 page_no << ibb_page_size_shift);
  return binlog_recover_gtid_state(&state, &reader);
}


static void
innodb_binlog_write_cache(IO_CACHE *cache,
                       handler_binlog_event_group_info *binlog_info, mtr_t *mtr)
{
  chunk_data_cache chunk_data(cache, binlog_info);
  fsp_binlog_write_rec(&chunk_data, mtr, FSP_BINLOG_TYPE_COMMIT);
}


/* Allocate a context for out-of-band binlogging. */
static binlog_oob_context *
alloc_oob_context(uint32 list_length)
{
  size_t needed= sizeof(binlog_oob_context) +
    list_length * sizeof(binlog_oob_context::node_info);
  binlog_oob_context *c=
    (binlog_oob_context *) ut_malloc(needed, mem_key_binlog);
  if (c)
  {
    c->node_list_alloc_len= list_length;
    c->node_list_len= 0;
  }
  else
    my_error(ER_OUTOFMEMORY, MYF(0), needed);

  return c;
}


static inline void
free_oob_context(binlog_oob_context *c)
{
  ut_free(c);
}


static binlog_oob_context *
ensure_oob_context(void **engine_data, uint32_t needed_len)
{
  binlog_oob_context *c= (binlog_oob_context *)*engine_data;
  if (c->node_list_alloc_len >= needed_len)
    return c;
  if (needed_len < c->node_list_alloc_len + 10)
    needed_len= c->node_list_alloc_len + 10;
  binlog_oob_context *new_c= alloc_oob_context(needed_len);
  if (UNIV_UNLIKELY(!new_c))
    return nullptr;
  memcpy(new_c, c, sizeof(binlog_oob_context) +
         needed_len*sizeof(binlog_oob_context::node_info));
  new_c->node_list_alloc_len= needed_len;
  *engine_data= new_c;
  free_oob_context(c);
  return new_c;
}


/*
  Binlog an out-of-band piece of event group data.

  For large transactions, we binlog the data in pieces spread out over the
  binlog file(s), to avoid a large stall to write large amounts of data during
  transaction commit, and to avoid having to keep all of the transaction in
  memory or spill it to temporary file.

  The chunks of data are written out in a binary tree structure, to allow
  efficiently reading the transaction back in order from start to end. Note
  that the binlog is written append-only, so we cannot simply link each chunk
  to the following chunk, as the following chunk is unknown when binlogging the
  prior chunk. With a binary tree structure, the reader can do a post-order
  traversal and only need to keep log_2(N) node pointers in-memory at any time.

  A perfect binary tree of height h has 2**h - 1 nodes. At any time during a
  transaction, the out-of-band data in the binary log for that transaction
  consists of a forest (eg. a list) of perfect binary trees of strictly
  decreasing height, except that the last two trees may have the same height.
  For example, here is how it looks for a transaction where 13 nodes (0-12)
  have been binlogged out-of-band so far:

          6
       _ / \_
      2      5      9     12
     / \    / \    / \    / \
    0   1  3   4  7   8 10  11

  In addition to the shown binary tree parent->child pointers, each leaf has a
  (single) link to the root node of the prior (at the time the leaf was added)
  tree. In the example this means the following links:
    11->10, 10->9, 8->7, 7->6, 4->3, 3->2, 1->0
  This allows to fully traverse the forest of perfect binary trees starting
  from the last node (12 in the example). In the example, only 10->9 and 7->6
  will be needed, but the other links would be needed if the tree had been
  completed at earlier stages.

  As a new node is added, there are two different cases on how to maintain
  the binary tree forest structure:

    1. If the last two trees in the forest have the same height h, then those
       two trees are replaced by a single tree of height (h+1) with the new
       node as root and the two trees as left and right child. The number of
       trees in the forest thus decrease by one.

    2. Otherwise the new node is added at the end of the forest as a tree of
       height 1; in this case the forest increases by one tree.

  In both cases, we maintain the invariants that the forest consist of a list
  of perfect binary trees, and that the heights of the trees are strictly
  decreasing except that the last two trees can have the same height.

  When a transaction is committed, the commit record contains a pointer to
  the root node of the last tree in the forest. If the transaction is never
  committed (explicitly rolled back or lost due to disconnect or server
  restart or crash), then the out-of-band data is simply left in place; it
  will be ignored by readers and eventually discarded as the old binlog files
  are purged.
*/
bool
innodb_binlog_oob(THD *thd, const unsigned char *data, size_t data_len,
               void **engine_data)
{
  binlog_oob_context *c= (binlog_oob_context *)*engine_data;
  if (!c)
    *engine_data= c= alloc_oob_context(10);
  if (UNIV_UNLIKELY(!c))
    return true;

  uint32_t i= c->node_list_len;
  uint64_t new_idx= i==0 ? 0 : c->node_list[i-1].node_index + 1;
  if (i >= 2 && c->node_list[i-2].height == c->node_list[i-1].height)
  {
    /* Case 1: Replace two trees with a tree rooted in a new node. */
    binlog_oob_context::chunk_data_oob oob_data
      (new_idx,
       c->node_list[i-2].file_no, c->node_list[i-2].offset,
       c->node_list[i-1].file_no, c->node_list[i-1].offset,
       (byte *)data, data_len);
    if (c->binlog_node(i-2, new_idx, i-2, i-1, &oob_data))
      return true;
    c->node_list_len= i - 1;
  }
  else if (i > 0)
  {
    /* Case 2: Add the new node as a singleton tree. */
    c= ensure_oob_context(engine_data, i+1);
    if (!c)
      return true;
    binlog_oob_context::chunk_data_oob oob_data
      (new_idx,
       0, 0, /* NULL left child signifies a leaf */
       c->node_list[i-1].file_no, c->node_list[i-1].offset,
       (byte *)data, data_len);
    if (c->binlog_node(i, new_idx, i-1, i-1, &oob_data))
      return true;
    c->node_list_len= i + 1;
  }
  else
  {
    /* Special case i==0, like case 2 but no prior node to link to. */
    binlog_oob_context::chunk_data_oob oob_data
      (new_idx, 0, 0, 0, 0, (byte *)data, data_len);
    if (c->binlog_node(i, new_idx, ~(uint32_t)0, ~(uint32_t)0, &oob_data))
      return true;
    c->first_node_file_no= c->node_list[i].file_no;
    c->first_node_offset= c->node_list[i].offset;
    c->node_list_len= 1;
  }

  return false;
}


/*
  Binlog a new out-of-band tree node and put it at position `node` in the list
  of trees. A leaf node is denoted by left and right child being identical (and
  in this case they point to the root of the prior tree).
*/
bool
binlog_oob_context::binlog_node(uint32_t node, uint64_t new_idx,
                                uint32_t left_node, uint32_t right_node,
                                chunk_data_oob *oob_data)
{
  uint32_t new_height=
    left_node == right_node ? 1 : 1 + node_list[left_node].height;
  mtr_t mtr;
  mtr.start();
  std::pair<uint64_t, uint64_t> new_file_no_offset=
    fsp_binlog_write_rec(oob_data, &mtr, FSP_BINLOG_TYPE_OOB_DATA);
  mtr.commit();
  node_list[node].file_no= new_file_no_offset.first;
  node_list[node].offset= new_file_no_offset.second;
  node_list[node].node_index= new_idx;
  node_list[node].height= new_height;
  return false;  // ToDo: Error handling?
}


binlog_oob_context::chunk_data_oob::chunk_data_oob(uint64_t idx,
        uint64_t left_file_no, uint64_t left_offset,
        uint64_t right_file_no, uint64_t right_offset,
        byte *data, size_t data_len)
  : sofar(0), main_len(data_len), main_data(data)
{
  ut_ad(data_len > 0);
  byte *p= &header_buf[0];
  p= compr_int_write(p, idx);
  p= compr_int_write(p, left_file_no);
  p= compr_int_write(p, left_offset);
  p= compr_int_write(p, right_file_no);
  p= compr_int_write(p, right_offset);
  ut_ad(p - &header_buf[0] <= max_buffer);
  header_len= (uint32_t)(p - &header_buf[0]);
}


std::pair<uint32_t, bool>
binlog_oob_context::chunk_data_oob::copy_data(byte *p, uint32_t max_len)
{
  uint32_t size= 0;
  /* First write header data, if any left. */
  if (sofar < header_len)
  {
    size= std::min(header_len - (uint32_t)sofar, max_len);
    memcpy(p, header_buf + sofar, size);
    p+= size;
    sofar+= size;
    if (UNIV_UNLIKELY(max_len == size))
      return {size, sofar == header_len + main_len};
    max_len-= size;
  }

  /* Then write the main chunk data. */
  ut_ad(sofar >= header_len);
  ut_ad(main_len > 0);
  uint32_t size2=
    (uint32_t)std::min(header_len + main_len - sofar, (uint64_t)max_len);
  memcpy(p, main_data + (sofar - header_len), size2);
  sofar+= size2;
  return {size + size2, sofar == header_len + main_len};
}


void
innodb_free_oob(THD *thd, void *engine_data)
{
  free_oob_context((binlog_oob_context *)engine_data);
}


innodb_binlog_oob_reader::innodb_binlog_oob_reader()
{
  /* Nothing. */
}


innodb_binlog_oob_reader::~innodb_binlog_oob_reader()
{
  /* Nothing. */
}


void
innodb_binlog_oob_reader::push_state(enum oob_states state, uint64_t file_no,
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
innodb_binlog_oob_reader::start_traversal(uint64_t file_no, uint64_t offset)
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
innodb_binlog_oob_reader::read_data(binlog_chunk_reader *chunk_rd,
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
    ut_ad(0 /* Should not call when no more oob data to read. */);
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

    if (UNIV_LIKELY(len > 0) && UNIV_LIKELY(!chunk_rd->end_of_record()))
    {
      res= chunk_rd->read_data(buf, len, false);
      if (res < 0)
        return -1;
      size+= res;
    }

    if (chunk_rd->end_of_record())
    {
      /* This oob record done, pop the state. */
      ut_ad(!stack.empty());
      stack.erase(stack.end() - 1, stack.end());
    }
    return size;

  default:
    ut_ad(0);
    return -1;
  }
}


ha_innodb_binlog_reader::ha_innodb_binlog_reader(uint64_t file_no,
                                                 uint64_t offset)
  : rd_buf_len(0), rd_buf_sofar(0), state(ST_read_next_event_group)
{
  page_buf= (uchar *)ut_malloc(ibb_page_size, mem_key_binlog);
  chunk_rd.set_page_buf(page_buf);
  chunk_rd.seek(file_no, offset);
  chunk_rd.skip_partial(true);
}


ha_innodb_binlog_reader::~ha_innodb_binlog_reader()
{
  ut_free(page_buf);
}


/*
  Read data from current position in binlog.

  If the data is written to disk (visible at the OS level, even if not
  necessarily fsync()'ed to disk), we can read directly from the file.
  Otherwise, the data must still be available in the buffer pool and
  we can read it from there.

  First try a dirty read of current state; if this says the data is available
  to read from the file, this is safe to do (data cannot become un-written).

  If not, then check if the page is in the buffer pool; if not, then likewise
  we know it's safe to read from the file directly.

  Finally, do another check of the current state. This will catch the case
  where we looked for a page in binlog file N, but its tablespace id has been
  recycled, so we got a page from (N+2) instead. In this case also, we can
  then read from the real file.
*/
int ha_innodb_binlog_reader::read_binlog_data(uchar *buf, uint32_t len)
{
  int res= read_data(buf, len);
  chunk_rd.release(res == 0);
  cur_file_no= chunk_rd.current_file_no();
  cur_file_pos= chunk_rd.current_pos();
  return res;
}


int ha_innodb_binlog_reader::read_data(uchar *buf, uint32_t len)
{
  int res;
  const uchar *p_end;
  const uchar *p;
  std::pair<uint64_t, const unsigned char *> v_and_p;
  int size;

again:
  switch (state)
  {
  case ST_read_next_event_group:
    static_assert(sizeof(rd_buf) == 5*COMPR_INT_MAX64,
                  "rd_buf size must match code using it");
    res= chunk_rd.read_data(rd_buf, 5*COMPR_INT_MAX64, true);
    if (res <= 0)
      return res;
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
    }

    rd_buf_sofar= (uint32_t)(p - rd_buf);
    state= ST_read_commit_record;
    goto again;

  case ST_read_commit_record:
    size= 0;
    if (rd_buf_len > rd_buf_sofar)
    {
      /* Use any excess data from when the header was read. */
      size= std::min((int)(rd_buf_len - rd_buf_sofar), (int)len);
      memcpy(buf, rd_buf + rd_buf_sofar, size);
      rd_buf_sofar+= size;
      len-= size;
      buf+= size;
    }

    if (UNIV_LIKELY(len > 0) && UNIV_LIKELY(!chunk_rd.end_of_record()))
    {
      res= chunk_rd.read_data(buf, len, false);
      if (res < 0)
        return -1;
      size+= res;
    }

    if (UNIV_LIKELY(rd_buf_sofar == rd_buf_len) && chunk_rd.end_of_record())
    {
      if (oob_count == 0)
        state= ST_read_next_event_group;
      else
      {
        oob_reader.start_traversal(oob_last_file_no, oob_last_offset);
        chunk_rd.save_pos(&saved_commit_pos);
        state= ST_read_oob_data;
      }
      if (size == 0)
        goto again;
    }

    return size;

  case ST_read_oob_data:
    res= oob_reader.read_data(&chunk_rd, buf, len);
    if (res < 0)
      return -1;
    if (oob_reader.oob_traversal_done())
    {
      chunk_rd.restore_pos(&saved_commit_pos);
      state= ST_read_next_event_group;
    }
    if (UNIV_UNLIKELY(res == 0))
    {
      ut_ad(0 /* Should have had oob_traversal_done() last time then. */);
      goto again;
    }
    return res;

  default:
    ut_ad(0);
    return -1;
  }
}


bool
ha_innodb_binlog_reader::data_available()
{
  if (state != ST_read_next_event_group)
    return true;
  return chunk_rd.data_available();
}


handler_binlog_reader *
innodb_get_binlog_reader()
{
  return new ha_innodb_binlog_reader();
}


gtid_search::gtid_search()
  : cur_open_file_no(~(uint64_t)0), cur_open_file_length(0),
    cur_open_file((File)-1)
{
  /* Nothing else. */
}


gtid_search::~gtid_search()
{
  if (cur_open_file >= (File)0)
    my_close(cur_open_file, MYF(0));
}


/*
  Read a GTID state record from file_no and page_no.

  Returns:
    READ_ERROR      Error reading the file or corrupt data
    READ_ENOENT     File not found
    READ_NOT_FOUND  No GTID state record found on the page
    READ_FOUND      Record found

  ToDo: Rewrite this to use a binlog_chunk_reader.

*/
enum gtid_search::Read_Result
gtid_search::read_gtid_state_file_no(rpl_binlog_state_base *state,
                                     uint64_t file_no, uint32_t page_no,
                                     uint64_t *out_file_end,
                                     uint32_t *out_diff_state_interval)
{
  binlog_header_data header_data;
  *out_file_end= 0;
  uint64_t active2= active_binlog_file_no.load(std::memory_order_acquire);
  if (file_no > active2)
    return READ_ENOENT;

  for (;;)
  {
    uint64_t active= active2;
    uint64_t end_offset=
      binlog_cur_end_offset[file_no&1].load(std::memory_order_acquire);
    fsp_binlog_page_entry *block;

    if (file_no + 1 >= active &&
        end_offset != ~(uint64_t)0 &&
        page_no <= (end_offset >> ibb_page_size_shift))
    {
      /*
        See if the page is available in the buffer pool.
        Since we only use the low bit of file_no to determine the tablespace
        id, the buffer pool page will only be valid if the active file_no did
        not change while getting the page (otherwise it might belong to a
        later tablespace file).
      */
      block= binlog_page_fifo->get_page(file_no, page_no);
    }
    else
      block= nullptr;
    active2= active_binlog_file_no.load(std::memory_order_acquire);
    if (UNIV_UNLIKELY(active2 != active))
    {
      /* Active moved ahead while we were reading, try again. */
      if (block)
        binlog_page_fifo->release_page(block);
      continue;
    }
    if (file_no + 1 >= active)
    {
      *out_file_end= end_offset;
      /*
        Note: if end_offset is ~0, it means that the tablespace has been closed
        and needs to be read as a plain file. Then this condition will be false
        and we fall through to the file-reading code below, no need for an
        extra conditional jump here.
      */
      if (page_no > (end_offset >> ibb_page_size_shift))
      {
        ut_ad(!block);
        return READ_NOT_FOUND;
      }
    }

    if (block)
    {
      ut_ad(end_offset != ~(uint64_t)0);
      int res= read_gtid_state_from_page(state, block->page_buf, page_no,
                                         &header_data);
      *out_diff_state_interval= header_data.diff_state_interval;
      binlog_page_fifo->release_page(block);
      return (Read_Result)res;
    }
    else
    {
      if (cur_open_file_no != file_no)
      {
        if (cur_open_file >= (File)0)
        {
          my_close(cur_open_file, MYF(0));
          cur_open_file= (File)-1;
          cur_open_file_length= 0;
        }
      }
      if (cur_open_file < (File)0)
      {
        char filename[OS_FILE_MAX_PATH];
        binlog_name_make(filename, file_no);
        cur_open_file= my_open(filename, O_RDONLY | O_BINARY, MYF(0));
        if (cur_open_file < (File)0)
        {
          if (errno == ENOENT)
            return READ_ENOENT;
          my_error(ER_CANT_OPEN_FILE, MYF(0), filename, errno);
          return READ_ERROR;
        }
        MY_STAT stat_buf;
        if (my_fstat(cur_open_file, &stat_buf, MYF(0))) {
          my_error(ER_CANT_GET_STAT, MYF(0), filename, errno);
          my_close(cur_open_file, MYF(0));
          cur_open_file= (File)-1;
          return READ_ERROR;
        }
        cur_open_file_length= stat_buf.st_size;
        cur_open_file_no= file_no;
      }
      if (!*out_file_end)
        *out_file_end= cur_open_file_length;
      int res= read_gtid_state(state, cur_open_file, page_no, &header_data);
      *out_diff_state_interval= header_data.diff_state_interval;
      return (Read_Result)res;
    }
  }
}


/*
  Search for a GTID position in the binlog.
  Find a binlog file_no and an offset into the file that is guaranteed to
  be before the target position. It can be a bit earlier, that only means a
  bit more of the binlog needs to be scanned to find the real position.

  Returns:
    -1 error
     0 Position not found (has been purged)
     1 Position found
*/

int
gtid_search::find_gtid_pos(slave_connection_state *pos,
                           rpl_binlog_state_base *out_state,
                           uint64_t *out_file_no, uint64_t *out_offset)
{
  /*
    Dirty read, but getting a slightly stale value is no problem, we will just
    be starting to scan the binlog file at a slightly earlier position than
    necessary.
  */
  uint64_t file_no= active_binlog_file_no.load(std::memory_order_relaxed);

  /* First search backwards for the right file to start from. */
  uint64_t file_end= 0;
  uint32_t diff_state_page_interval= 0;
  rpl_binlog_state_base base_state, page0_diff_state, tmp_diff_state;
  base_state.init();
  for (;;)
  {
    enum Read_Result res=
      read_gtid_state_file_no(&base_state, file_no, 0, &file_end,
                              &diff_state_page_interval);
    if (res == READ_ENOENT)
      return 0;
    if (res == READ_ERROR)
      return -1;
    if (res == READ_NOT_FOUND)
    {
      if (file_no == 0)
      {
        /* Handle the special case of a completely empty binlog file. */
        out_state->reset_nolock();
        *out_file_no= file_no;
        *out_offset= 0;
        return 1;
      }
      ut_ad(0 /* Not expected to find no state, should always be written. */);
      return -1;
    }
    if (base_state.is_before_pos(pos))
      break;
    base_state.reset_nolock();
    if (file_no == 0)
      return 0;
    --file_no;
  }

  /*
    Then binary search for the last differential state record that is still
    before the searched position.

    The invariant is that page2 is known to be after the target page, and page0
    is known to be a valid position to start (but possibly earlier than needed).
  */
  uint32_t page0= 0;
  uint32_t page2= (uint32_t)
    (diff_state_page_interval + ((file_end - 1) >> ibb_page_size_shift));
  /* Round to the next diff_state_page_interval after file_end. */
  page2-= page2 % diff_state_page_interval;
  uint32_t page1= (page0 + page2) / 2;
  page0_diff_state.init();
  page0_diff_state.load_nolock(&base_state);
  tmp_diff_state.init();
  while (page1 >= page0 + diff_state_page_interval)
  {
    ut_ad((page1 - page0) % diff_state_page_interval == 0);
    tmp_diff_state.reset_nolock();
    tmp_diff_state.load_nolock(&base_state);
    uint32_t dummy;
    enum Read_Result res=
      read_gtid_state_file_no(&tmp_diff_state, file_no, page1, &file_end,
                              &dummy);
    if (res == READ_ENOENT)
      return 0;  /* File purged while we are reading from it? */
    if (res == READ_ERROR)
      return -1;
    if (res == READ_NOT_FOUND)
    {
      /*
        If the diff state record was not written here for some reason, just
        try the one just before. It will be safe, even if not always optimal,
        and this is an abnormal situation anyway.
      */
      page1= page1 - diff_state_page_interval;
      continue;
    }
    if (tmp_diff_state.is_before_pos(pos))
    {
      page0= page1;
      page0_diff_state.reset_nolock();
      page0_diff_state.load_nolock(&tmp_diff_state);
    }
    else
      page2= page1;
    page1= (page0 + page2) / 2;
  }
  ut_ad(page1 >= page0);
  out_state->load_nolock(&page0_diff_state);
  *out_file_no= file_no;
  *out_offset= (uint64_t)page0 << ibb_page_size_shift;
  return 1;
}


int
ha_innodb_binlog_reader::init_gtid_pos(slave_connection_state *pos,
                                       rpl_binlog_state_base *state)
{
  gtid_search search_obj;
  uint64_t file_no;
  uint64_t offset;
  int res= search_obj.find_gtid_pos(pos, state, &file_no, &offset);
  if (res < 0)
    return -1;
  if (res > 0)
  {
    chunk_rd.seek(file_no, offset);
    chunk_rd.skip_partial(true);
    cur_file_no= chunk_rd.current_file_no();
    cur_file_pos= chunk_rd.current_pos();
  }
  return res;
}


int
ha_innodb_binlog_reader::init_legacy_pos(const char *filename, ulonglong offset)
{
  uint64_t file_no;
  if (!filename)
  {
    mysql_mutex_lock(&purge_binlog_mutex);
    file_no= earliest_binlog_file_no;
    mysql_mutex_unlock(&purge_binlog_mutex);
  }
  else if (!is_binlog_name(filename, &file_no))
  {
    my_error(ER_UNKNOWN_TARGET_BINLOG, MYF(0));
    return -1;
  }
  if ((uint64_t)offset >= (uint64_t)(UINT32_MAX) << ibb_page_size_shift)
  {
    my_error(ER_BINLOG_POS_INVALID, MYF(0), offset);
    return -1;
  }

  /*
    ToDo: Here, we could start at the beginning of the page containing the
    requested position. Then read forwards until the requested position is
    reached. This way we avoid reading garbaga data for invalid request
    offset.
  */
  chunk_rd.seek(file_no, (uint64_t)offset);
  chunk_rd.skip_partial(true);
  cur_file_no= chunk_rd.current_file_no();
  cur_file_pos= chunk_rd.current_pos();
  return 0;
}


void
ha_innodb_binlog_reader::get_filename(char name[FN_REFLEN], uint64_t file_no)
{
  static_assert(BINLOG_NAME_MAX_LEN <= FN_REFLEN,
                "FN_REFLEN too shot to hold InnoDB binlog name");
  binlog_name_make_short(name, file_no);
}


extern "C" void binlog_get_cache(THD *, IO_CACHE **,
                                 handler_binlog_event_group_info **,
                                 const rpl_gtid **);

void
innodb_binlog_trx(trx_t *trx, mtr_t *mtr)
{
  IO_CACHE *cache;
  handler_binlog_event_group_info *binlog_info;
  const rpl_gtid *gtid;

  if (!trx->mysql_thd)
    return;
  binlog_get_cache(trx->mysql_thd, &cache, &binlog_info, &gtid);
  if (UNIV_LIKELY(binlog_info != nullptr) &&
      UNIV_LIKELY(binlog_info->gtid_offset > 0)) {
    binlog_diff_state.update_nolock(gtid);
    innodb_binlog_write_cache(cache, binlog_info, mtr);
  }
}


bool
innobase_binlog_write_direct(IO_CACHE *cache,
                             handler_binlog_event_group_info *binlog_info,
                             const rpl_gtid *gtid)
{
  mtr_t mtr;
  if (gtid)
    binlog_diff_state.update_nolock(gtid);
  mtr.start();
  innodb_binlog_write_cache(cache, binlog_info, &mtr);
  mtr.commit();
  /* ToDo: Should we sync the log here? Maybe depending on an extra bool parameter? */
  /* ToDo: Presumably innodb_binlog_write_cache() should be able to fail in some cases? Then return any such error to the caller. */
  return false;
}


bool
innodb_find_binlogs(uint64_t *out_first, uint64_t *out_last)
{
  mysql_mutex_lock(&active_binlog_mutex);
  *out_last= last_created_binlog_file_no;
  mysql_mutex_unlock(&active_binlog_mutex);
  mysql_mutex_lock(&purge_binlog_mutex);
  *out_first= earliest_binlog_file_no;
  mysql_mutex_unlock(&purge_binlog_mutex);
  if (*out_first == ~(uint64_t)0 || *out_last == ~(uint64_t)0)
  {
    ut_ad(0 /* Impossible, we wait at startup for binlog to be created. */);
    return true;
  }
  return false;
}


void
innodb_binlog_status(char out_filename[FN_REFLEN], ulonglong *out_pos)
{
  static_assert(BINLOG_NAME_MAX_LEN <= FN_REFLEN,
                "FN_REFLEN too shot to hold InnoDB binlog name");
  uint64_t file_no= active_binlog_file_no.load(std::memory_order_relaxed);
  uint32_t page_no= binlog_cur_page_no;
  uint32_t in_page_offset= binlog_cur_page_offset;
  binlog_name_make_short(out_filename, file_no);
  *out_pos= ((ulonglong)page_no << ibb_page_size_shift) | in_page_offset;
}


bool
innodb_binlog_get_init_state(rpl_binlog_state_base *out_state)
{
  gtid_search search_obj;
  uint64_t dummy_file_end;
  uint32_t dummy_diff_state_interval;
  bool err= false;

  mysql_mutex_lock(&purge_binlog_mutex);
  uint64_t file_no= earliest_binlog_file_no;
  enum gtid_search::Read_Result res=
    search_obj.read_gtid_state_file_no(out_state, file_no, 0, &dummy_file_end,
                                       &dummy_diff_state_interval);
  mysql_mutex_unlock(&purge_binlog_mutex);
  if (res != gtid_search::READ_FOUND)
    err= true;
  return err;

}


bool
innodb_reset_binlogs()
{
  bool err= false;

  ut_a(innodb_binlog_inited >= 2);

  /* Close existing binlog tablespaces and stop the pre-alloc thread. */
  innodb_binlog_close(false);

  /*
    Durably flush the redo log to disk. This is mostly to simplify
    conceptually (RESET MASTER is not performance critical). This way, we will
    never see a state where recovery stops at an LSN prior to the RESET
    MASTER, so we do not have any question around truncating the binlog to a
    point before the RESET MASTER.
  */
 log_buffer_flush_to_disk(true);

  /* Prevent any flushing activity while resetting. */
  binlog_page_fifo->lock_wait_for_idle();
  binlog_page_fifo->reset();

  /* Delete all binlog files in the directory. */
  MY_DIR *dir= my_dir(innodb_binlog_directory, MYF(MY_WME));
  if (!dir)
  {
    sql_print_error("Could not read the binlog directory '%s', error code %d",
                    innodb_binlog_directory, my_errno);
    err= true;
  }
  else
  {
    size_t num_entries= dir->number_of_files;
    fileinfo *entries= dir->dir_entry;
    for (size_t i= 0; i < num_entries; ++i) {
      const char *name= entries[i].name;
      uint64_t file_no;
      if (!is_binlog_name(name, &file_no))
        continue;
      char full_path[OS_FILE_MAX_PATH];
      binlog_name_make(full_path, file_no);
      if (my_delete(full_path, MYF(MY_WME)))
        err= true;
    }
    my_dirend(dir);
  }
  /*
    If we get an error deleting any of the existing files, we report the error
    back up. But we still try to initialize an empty binlog state, better than
    leaving a non-functional binlog with corrupt internal state.
  */

  /* Re-initialize empty binlog state and start the pre-alloc thread. */
  innodb_binlog_init_state();
  binlog_page_fifo->unlock();
  start_binlog_prealloc_thread();
  binlog_sync_initial();

  return err;
}


/*
  The low-level function handling binlog purge.

  How much to purge is determined by:

  1. Lowest file_no that should not be purged. This is determined as the
  minimum of:
    1a. active_binlog_file_no
    1b. first_open_binlog_file_no
    1c. Any file_no in use by an active dump thread
    1d. Any file_no containing oob data referenced by file_no from (1c)
    1e. User specified file_no (from PURGE BINARY LOGS TO, if any).
    1f. (ToDo): Any file_no that was still active at the last checkpoint.

  2. Unix timestamp specifying the minimal value that should not be purged,
  optional (used by PURGE BINARY LOGS BEFORE and --binlog-expire-log-seconds).

  3. Maximum total size of binlogs, optional (from --max-binlog-total-size).

  Sets out_file_no to the earliest binlog file not purged.
  Additionally returns:

     0  Purged all files as requested.
     1  Some files were not purged due to being currently in-use (by binlog
        writing or active dump threads).
*/
static int
innodb_binlog_purge_low(uint64_t limit_file_no,
                        bool by_date, time_t limit_date,
                        bool by_size, ulonglong limit_size,
                        bool by_name, uint64_t limit_name_file_no,
                        uint64_t *out_file_no)
{
  uint64_t active= active_binlog_file_no.load(std::memory_order_relaxed);
  bool need_active_flush= (active <= limit_file_no + 2);
  ut_ad(by_date || by_size || by_name);
  ut_a(limit_file_no <= active);
  ut_a(limit_file_no <= first_open_binlog_file_no);

  mysql_mutex_assert_owner(&purge_binlog_mutex);
  size_t loc_total_size= total_binlog_used_size;
  uint64_t file_no;
  bool want_purge;

  for (file_no= earliest_binlog_file_no; ; ++file_no)
  {
    want_purge= false;

    char filename[OS_FILE_MAX_PATH];
    binlog_name_make(filename, file_no);
    MY_STAT stat_buf;
    if (!my_stat(filename, &stat_buf, MYF(0)))
    {
      if (my_errno == ENOENT)
        sql_print_information("InnoDB: File already gone when purging binlog "
                              "file '%s'", filename);
      else
        sql_print_warning("InnoDB: Failed to stat() when trying to purge "
                          "binlog file '%' (errno: %d)", filename, my_errno);
      continue;
    }

    if (by_date && stat_buf.st_mtime < limit_date)
      want_purge= true;
    if (by_size && loc_total_size > limit_size)
      want_purge= true;
    if (by_name && file_no < limit_name_file_no)
      want_purge= true;
    if (file_no >= limit_file_no || !want_purge)
      break;
    earliest_binlog_file_no= file_no + 1;
    if (loc_total_size < (size_t)stat_buf.st_size)
    {
      /*
        Somehow we miscounted size, files changed from outside server or
        possibly bug. We will handle not underflowing the total. If this
        assertion becomes a problem for testing, it can just be removed.
      */
      ut_ad(0);
    }
    else
      loc_total_size-= stat_buf.st_size;

    /*
      Make sure that we always leave at least one binlog file durably non-empty,
      by fsync()'ing the first page of the active file before deleting file
      (active-2). This way, recovery will always have at least one file header
      from which to determine the LSN at which to start applying redo records.
    */
    if (file_no + 2 >= active && need_active_flush)
    {
      binlog_page_fifo->flush_up_to(active, 0);
      need_active_flush= false;
    }

    if (my_delete(filename, MYF(0)))
    {
      if (my_errno == ENOENT)
      {
        /*
          File already gone, just ignore the error.
          (This should be somewhat unusual to happen as stat() succeeded).
        */
      }
      else
      {
        sql_print_warning("InnoDB: Delete failed while trying to purge binlog "
                          "file '%s' (errno: %d)", filename, my_error);
        continue;
      }
    }
  }
  total_binlog_used_size= loc_total_size;
  *out_file_no= file_no;
  return (want_purge ? 1 : 0);
}


static void
innodb_binlog_autopurge(uint64_t first_open_file_no)
{
  handler_binlog_purge_info UNINIT_VAR(purge_info);
#ifdef HAVE_REPLICATION
  extern bool ha_binlog_purge_info(handler_binlog_purge_info *out_info);
  bool can_purge= ha_binlog_purge_info(&purge_info);
#else
  bool can_purge= false;
#endif
  if (!can_purge ||
      !(purge_info.purge_by_size || purge_info.purge_by_date))
    return;

  /*
    ToDo: Here, we need to move back the purge_info.limit_file_no to the
    earliest file containing any oob data referenced from the supplied
    purge_info.limit_file_no.
  */

  /* Don't purge any actively open tablespace files. */
  uint64_t limit_file_no= purge_info.limit_file_no;
  if (limit_file_no == ~(uint64_t)0 || limit_file_no > first_open_file_no)
    limit_file_no= first_open_file_no;
  uint64_t active= active_binlog_file_no.load(std::memory_order_relaxed);
  if (limit_file_no > active)
    limit_file_no= active;

  uint64_t file_no;
  int res=
    innodb_binlog_purge_low(limit_file_no,
                            purge_info.purge_by_date, purge_info.limit_date,
                            purge_info.purge_by_size, purge_info.limit_size,
                            false, 0,
                            &file_no);
  if (res)
  {
    if (!purge_warning_given)
    {
      char filename[BINLOG_NAME_MAX_LEN];
      binlog_name_make_short(filename, file_no);
      if (purge_info.nonpurge_reason)
        sql_print_information("InnoDB: Binlog file %s could not be purged "
                              "because %s",
                              filename, purge_info.nonpurge_reason);
      else if (purge_info.limit_file_no == file_no)
        sql_print_information("InnoDB: Binlog file %s could not be purged "
                              "because it is in use by a binlog dump thread "
                              "(connected slave)", filename);
      else if (limit_file_no == file_no)
        sql_print_information("InnoDB: Binlog file %s could not be purged "
                              "because it is in active use", filename);
      else
        sql_print_information("InnoDB: Binlog file %s could not be purged "
                              "because it might still be needed", filename);
      purge_warning_given= true;
    }
  }
  else
    purge_warning_given= false;
}


int
innodb_binlog_purge(handler_binlog_purge_info *purge_info)
{
  /*
    Let us check that we do not get an attempt to purge by file, date, and/or
    size at the same time.
    (If we do, it is not necesarily a problem, but this cannot happen in
    current server code).
  */
  ut_ad(1 == (!!purge_info->purge_by_name +
              !!purge_info->purge_by_date +
              !!purge_info->purge_by_size));

  if (!purge_info->purge_by_name && !purge_info->purge_by_date &&
      !purge_info->purge_by_size)
    return 0;

  mysql_mutex_lock(&active_binlog_mutex);
  uint64_t limit_file_no=
    std::min(active_binlog_file_no.load(std::memory_order_relaxed),
             first_open_binlog_file_no);
  uint64_t last_created= last_created_binlog_file_no;
  mysql_mutex_unlock(&active_binlog_mutex);

  uint64_t to_file_no= ~(uint64_t)0;
  if (purge_info->purge_by_name)
  {
    if (!is_binlog_name(purge_info->limit_name, &to_file_no) ||
        to_file_no > last_created)
      return LOG_INFO_EOF;
  }

  mysql_mutex_lock(&purge_binlog_mutex);
  uint64_t file_no;
  int res= innodb_binlog_purge_low(
        std::min(purge_info->limit_file_no, limit_file_no),
        purge_info->purge_by_date, purge_info->limit_date,
        purge_info->purge_by_size, purge_info->limit_size,
                                   purge_info->purge_by_name, to_file_no,
        &file_no);
  mysql_mutex_unlock(&purge_binlog_mutex);
  if (res == 1)
  {
    static_assert(sizeof(purge_info->nonpurge_filename) >= BINLOG_NAME_MAX_LEN,
                  "No room to return filename");
    binlog_name_make_short(purge_info->nonpurge_filename, file_no);
    if (!purge_info->nonpurge_reason)
    {
      if (limit_file_no == file_no)
        purge_info->nonpurge_reason= "the binlog file is in active use";
      else if (purge_info->limit_file_no == file_no)
        purge_info->nonpurge_reason= "it is in use by a binlog dump thread "
          "(connected slave)";
    }
    res= LOG_INFO_IN_USE;
  }
  else
    purge_warning_given= false;

  return res;
}


bool
binlog_recover_write_data(bool space_id, uint32_t page_no,
                          uint16_t offset,
                          lsn_t start_lsn, lsn_t lsn,
                          const byte *buf, size_t size) noexcept
{
  if (!recover_obj.inited)
    return recover_obj.init_recovery(space_id, page_no, offset, start_lsn, lsn,
                                     buf, size);
  return recover_obj.apply_redo(space_id, page_no, offset, start_lsn, lsn,
                                     buf, size);
}


void
binlog_recover_end(lsn_t lsn) noexcept
{
  if (recover_obj.inited)
    recover_obj.end_actions(true);
}
