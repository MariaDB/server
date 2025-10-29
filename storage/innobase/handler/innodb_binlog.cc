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

/*
  Need MYSQL_SERVER defined to be able to use THD_ENTER_COND from sql_class.h
  to make my_cond_wait() killable.
*/
#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_class.h"

#include "innodb_binlog.h"
#include "mtr0log.h"
#include "fsp0fsp.h"
#include "trx0trx.h"
#include "log0log.h"
#include "small_vector.h"

#include "mysys_err.h"
#include "my_compr_int.h"
#include "rpl_gtid_base.h"
#include "handler_binlog_reader.h"
#include "log.h"


class ibb_xid_hash;


static int innodb_binlog_inited= 0;

pending_lsn_fifo ibb_pending_lsn_fifo;
uint32_t innodb_binlog_size_in_pages;
const char *innodb_binlog_directory;

/** Current write position in active binlog file. */
uint32_t binlog_cur_page_no;
uint32_t binlog_cur_page_offset;

/**
  Server setting for how often to dump a (differential) binlog state at the
  start of the page, to speed up finding the initial GTID position, read-only.
*/
ulonglong innodb_binlog_state_interval;

/**
  Differential binlog state in the currently active binlog tablespace, relative
  to the state at the start.
*/
rpl_binlog_state_base binlog_diff_state;

static std::thread binlog_prealloc_thr_obj;
static bool prealloc_thread_end= false;

/**
  Mutex around purge operations, including earliest_binlog_file_no and
  total_binlog_used_size.
*/
mysql_mutex_t purge_binlog_mutex;

/** The earliest binlog tablespace file. Used in binlog purge. */
static uint64_t earliest_binlog_file_no;

/**
  The total space in use by binlog tablespace files. Maintained in-memory to
  not have to stat(2) every file for every new binlog tablespace allocated in
  case of --max-binlog-total-size.

  Initialized at server startup (and in RESET MASTER), and updated as binlog
  files are pre-allocated and purged.
*/
size_t total_binlog_used_size;

static bool purge_warning_given= false;

/** References to pending XA PREPARED transactions in the binlog. */
static ibb_xid_hash *ibb_xa_xid_hash;

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t binlog_prealloc_thread_key;
#endif


/**
   Structure holding context for out-of-band chunks of binlogged event group.
*/
struct binlog_oob_context {
  struct savepoint;
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
    const byte *main_data;
    uint32_t header_len;
    byte header_buf[max_buffer];

    chunk_data_oob(uint64_t idx,
                   uint64_t left_file_no, uint64_t left_offset,
                   uint64_t right_file_no, uint64_t right_offset,
                   const byte *data, size_t data_len);
    virtual ~chunk_data_oob() {};
    virtual std::pair<uint32_t, bool> copy_data(byte *p, uint32_t max_len) final;
  };

  bool binlog_node(uint32_t node, uint64_t new_idx,
                   uint32_t left_node, uint32_t right_node,
                   chunk_data_oob *oob_data, LF_PINS *pins, mtr_t *mtr);
  bool create_stmt_start_point();
  savepoint *create_savepoint();
  void rollback_to_savepoint(savepoint *savepoint);
  void rollback_to_stmt_start();

  /*
    Pending binlog write for the ibb_pending_lsn_fifo.
    pending_file_no is ~0 when no write is pending.
  */
  uint64_t pending_file_no;
  uint64_t pending_offset;
  lsn_t pending_lsn;

  uint64_t first_node_file_no;
  uint64_t first_node_offset;
  LF_PINS *lf_pins;
  savepoint *stmt_start_point;
  savepoint *savepoint_stack;
  /*
    The secondary pointer is for when server layer binlogs both a
    non-transactional and a transactional oob stream.
  */
  binlog_oob_context *secondary_ctx;
  uint32_t node_list_len;
  uint32_t node_list_alloc_len;
  /*
    Set if we incremented refcount in first_node_file_no, so we need to
    decrement again at commit record write or reset/rollback.
  */
  bool pending_refcount;
  /* Set when the transaction is sealed after writing an XA PREPARE record. */
  bool is_xa_prepared;
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

  /* Saved oob state for implementing ROLLBACK TO SAVEPOINT. */
  struct savepoint {
    /* Maintain a stack of pending savepoints. */
    savepoint *next;
    uint32_t node_list_len;
    uint32_t alloc_len;
    struct node_info node_list[];
  };
};


/**
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

  /* Out-of-band data to read after commit record, if any. */
  uint64_t oob_count;
  uint64_t oob_last_file_no;
  uint64_t oob_last_offset;
  /* Any secondary out-of-band data to be also read. */
  uint64_t oob_count2;
  uint64_t oob_last_file_no2;
  uint64_t oob_last_offset2;
  /*
    Originally requested starting file_no, from init_gtid_pos() or
    init_legacy_pos(). Or ~0 if none.
  */
  uint64_t requested_file_no;
  /* Buffer to hold a page read directly from the binlog file. */
  uchar *page_buf;
  /* Keep track of pending bytes in the rd_buf. */
  uint32_t rd_buf_len;
  uint32_t rd_buf_sofar;
  /* State for state machine reading chunks one by one. */
  enum reader_states state;

  /* Used to read the header of the commit record. */
  byte rd_buf[5*COMPR_INT_MAX64];
private:
  int read_data(uchar *buf, uint32_t len);

public:
  ha_innodb_binlog_reader(bool wait_durable, uint64_t file_no= 0,
                          uint64_t offset= 0);
  ~ha_innodb_binlog_reader();
  virtual int read_binlog_data(uchar *buf, uint32_t len) final;
  virtual bool data_available() final;
  virtual bool wait_available(THD *thd, const struct timespec *abstime) final;
  virtual int init_gtid_pos(THD *thd, slave_connection_state *pos,
                            rpl_binlog_state_base *state) final;
  virtual int init_legacy_pos(THD *thd, const char *filename,
                              ulonglong offset) final;
  virtual void enable_single_file() final;
  void seek_internal(uint64_t file_no, uint64_t offset);
};


/**
  Class that keeps track of the oob references etc. for each
  XA PREPAREd XID.
*/
class ibb_xid_hash {
public:
  struct xid_elem {
    XID xid;
    uint64_t refcnt_file_no;
    uint64_t oob_num_nodes;
    uint64_t oob_first_file_no;
    uint64_t oob_first_offset;
    uint64_t oob_last_file_no;
    uint64_t oob_last_offset;
  };
  HASH xid_hash;
  mysql_mutex_t xid_mutex;

  ibb_xid_hash();
  ~ibb_xid_hash();
  bool add_xid(const XID *xid, const binlog_oob_context *c);
  xid_elem *grab_xid(const XID *xid);
  template <typename F> bool run_on_xid(const XID *xid, F callback);
};


struct chunk_data_cache : public chunk_data_base {
  IO_CACHE *cache;
  binlog_oob_context *oob_ctx;
  my_off_t main_start;
  size_t main_remain;
  size_t gtid_remain;
  uint32_t header_remain;
  uint32_t header_sofar;
  byte header_buf[10*COMPR_INT_MAX64];

  chunk_data_cache(IO_CACHE *cache_arg,
                   handler_binlog_event_group_info *binlog_info)
  : cache(cache_arg),
    main_start(binlog_info->out_of_band_offset),
    main_remain((size_t)(binlog_info->gtid_offset -
                         binlog_info->out_of_band_offset)),
    header_sofar(0)
  {
    size_t end_offset= (size_t)my_b_tell(cache);
    ut_ad(end_offset > binlog_info->out_of_band_offset);
    ut_ad(binlog_info->gtid_offset >= binlog_info->out_of_band_offset);
    ut_ad(end_offset >= binlog_info->gtid_offset);
    gtid_remain= end_offset - (size_t)binlog_info->gtid_offset;

    binlog_oob_context *c=
      static_cast<binlog_oob_context *>(binlog_info->engine_ptr);
    unsigned char *p= header_buf;
    ut_ad(c);
    oob_ctx= c;
    if (UNIV_UNLIKELY(!c))
      ;
    else if (UNIV_UNLIKELY(binlog_info->xa_xid != nullptr) &&
             !binlog_info->internal_xa)
    {
      /*
        For explicit user XA COMMIT, the commit record must point to the
        OOB data previously saved in XA PREPARE.
      */
      bool err= ibb_xa_xid_hash->run_on_xid(binlog_info->xa_xid,
        [&p](const ibb_xid_hash::xid_elem *elem) -> bool {
          if (UNIV_LIKELY(elem->oob_num_nodes > 0))
          {
            p= compr_int_write(p, elem->oob_num_nodes);
            p= compr_int_write(p, elem->oob_first_file_no);
            p= compr_int_write(p, elem->oob_first_offset);
            p= compr_int_write(p, elem->oob_last_file_no);
            p= compr_int_write(p, elem->oob_last_offset);
            p= compr_int_write(p, 0);
          }
          else
            p= compr_int_write(p, 0);
          return false;
        });
      /*
        The XID must always be found, else we have a serious
        inconsistency between the server layer and binlog state.
        In case of inconsistency, better crash than leave a corrupt
        binlog.
      */
      ut_a(!err);
      ut_ad(binlog_info->engine_ptr2 == nullptr);
    }
    else if (c->node_list_len)
    {
      /*
        Link to the out-of-band data. First store the number of nodes; then
        store 2 x 2 numbers of file_no/offset for the first and last node.

        There's a special case when we have to link two times out-of-band data,
        due to mixing non-transactional and transactional stuff. In that case,
        the non-transactional goes first. In the common case where there is no
        dual oob references, we just store a single 0 count.
      */
      binlog_oob_context *c2= c->secondary_ctx=
        static_cast<binlog_oob_context *>(binlog_info->engine_ptr2);
      if (UNIV_UNLIKELY(c2 != nullptr) && c2->node_list_len)
      {
        uint32_t last2= c2->node_list_len-1;
        uint64_t num_nodes2= c2->node_list[last2].node_index + 1;
        p= compr_int_write(p, num_nodes2);
        p= compr_int_write(p, c2->first_node_file_no);
        p= compr_int_write(p, c2->first_node_offset);
        p= compr_int_write(p, c2->node_list[last2].file_no);
        p= compr_int_write(p, c2->node_list[last2].offset);
      }

      uint32_t last= c->node_list_len-1;
      uint64_t num_nodes= c->node_list[last].node_index + 1;
      p= compr_int_write(p, num_nodes);
      p= compr_int_write(p, c->first_node_file_no);
      p= compr_int_write(p, c->first_node_offset);
      p= compr_int_write(p, c->node_list[last].file_no);
      p= compr_int_write(p, c->node_list[last].offset);
      if (UNIV_LIKELY(c2 == nullptr) || c2->node_list_len == 0)
        p= compr_int_write(p, 0);
    }
    else
    {
      /*
        No out-of-band data, marked with a single 0 count for nodes and no
        first/last links.
      */
      p= compr_int_write(p, 0);
    }
    header_remain= (uint32_t)(p - header_buf);
    ut_ad((size_t)(p - header_buf) <= sizeof(header_buf));

    ut_ad (cache->pos_in_file <= binlog_info->out_of_band_offset);

    if (UNIV_UNLIKELY(binlog_info->internal_xa))
    {
      /*
        Insert the XID for the internal 2-phase commit in the xid_hash,
        incrementing the reference count. This will ensure we hold on to
        the commit record until ibb_binlog_unlog() is called, at which point
        the other participating storage engine(s) have durably committed.
      */
      bool err= ibb_xa_xid_hash->add_xid(binlog_info->xa_xid, c);
      ut_a(!err);
    }

    /* Start with the GTID event, which is put at the end of the IO_CACHE. */
    my_bool res= reinit_io_cache(cache, READ_CACHE, binlog_info->gtid_offset, 0, 0);
    ut_a(!res /* ToDo: Error handling. */);
  }
  ~chunk_data_cache() { }

  virtual std::pair<uint32_t, bool> copy_data(byte *p, uint32_t max_len) final
  {
    uint32_t size= 0;

    if (UNIV_LIKELY(oob_ctx != nullptr) && oob_ctx->pending_refcount)
    {
      ibb_file_hash.oob_ref_dec(oob_ctx->first_node_file_no, oob_ctx->lf_pins);
      oob_ctx->pending_refcount= false;
      if (UNIV_UNLIKELY(oob_ctx->secondary_ctx != nullptr) &&
          oob_ctx->secondary_ctx->pending_refcount)
      {
        ibb_file_hash.oob_ref_dec(oob_ctx->secondary_ctx->first_node_file_no,
                                  oob_ctx->secondary_ctx->lf_pins);
        oob_ctx->secondary_ctx->pending_refcount= false;
      }
    }

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
        my_b_seek(cache, main_start); /* Move to read the rest of the events. */
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


template<uint32_t bufsize_>
struct chunk_data_from_buf : public chunk_data_base {
  static constexpr uint32_t bufsize= bufsize_;

  uint32_t data_remain;
  uint32_t data_sofar;
  byte buffer[bufsize];

  chunk_data_from_buf() : data_sofar(0)
  {
    /* data_remain must be initialized in derived class constructor. */
  }

  virtual std::pair<uint32_t, bool> copy_data(byte *p, uint32_t max_len) final
  {
    if (UNIV_UNLIKELY(data_remain <= 0))
      return {0, true};
    uint32_t size= data_remain > max_len ? max_len : data_remain;
    memcpy(p, buffer + data_sofar, size);
    data_remain-= size;
    data_sofar+= size;
    return {size, data_remain == 0};
  }
  ~chunk_data_from_buf() { }
};


/**
  Record data for the XA prepare record.

  Size needed for the record data:
    1 byte type/flag.
    1 byte engine count.
    4 bytes formatID
    1 byte gtrid length
    1 byte bqual length
    128 bytes (max) gtrid and bqual strings.
*/
struct chunk_data_xa_prepare :
  public chunk_data_from_buf<1 + 1 + 4 + 1 + 1 + 128> {

  chunk_data_xa_prepare(const XID *xid, uchar engine_count)
  {
    /* ToDo: Need the correct data here, like the oob references. To be done when we start doing the XA crash recovery. */
    buffer[0]= 42 /* ToDo */;
    buffer[1]= engine_count;
    int4store(&buffer[2], xid->formatID);
    ut_a(xid->gtrid_length >= 0 && xid->gtrid_length <= 64);
    buffer[6]= (uchar)xid->gtrid_length;
    ut_a(xid->bqual_length >= 0 && xid->bqual_length <= 64);
    buffer[7]= (uchar)xid->bqual_length;
    memcpy(&buffer[8], &xid->data[0], xid->gtrid_length + xid->bqual_length);
    data_remain=
      static_cast<uint32_t>(8 + xid->gtrid_length + xid->bqual_length);
  }
  ~chunk_data_xa_prepare() { }
};


/**
  Record data for the XA COMMIT or XA ROLLBACK record.

  Size needed for the record data:
    1 byte type/flag.
    4 bytes formatID
    1 byte gtrid length
    1 byte bqual length
    128 bytes (max) gtrid and bqual strings.
*/
struct chunk_data_xa_complete :
  public chunk_data_from_buf<1 + 4 + 1 + 1 + 128> {

  chunk_data_xa_complete(const XID *xid, bool is_commit)
  {
    buffer[0]= (is_commit ? IBB_FL_XA_TYPE_COMMIT : IBB_FL_XA_TYPE_ROLLBACK);
    int4store(&buffer[1], xid->formatID);
    ut_a(xid->gtrid_length >= 0 && xid->gtrid_length <= 64);
    buffer[5]= (uchar)xid->gtrid_length;
    ut_a(xid->bqual_length >= 0 && xid->bqual_length <= 64);
    buffer[6]= (uchar)xid->bqual_length;
    memcpy(&buffer[7], &xid->data[0], xid->gtrid_length + xid->bqual_length);
    data_remain=
      static_cast<uint32_t>(7 + xid->gtrid_length + xid->bqual_length);
  }
  ~chunk_data_xa_complete() { }
};


class gtid_search {
public:
  gtid_search();
  ~gtid_search();
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
  int num_found;
  /* Default constructor to silence compiler warnings -Wuninitialized. */
  found_binlogs()= default;
};


/**
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
  /*
    The LSN of the previously applied redo record. Used to ignore duplicate
    redo records passed from the InnoDB recovery layer, eg. in multi-batch
    recovery. Also prev_size, prev_page_no, prev_offset, prev_space_id.
  */
  lsn_t prev_lsn;
  size_t prev_size;

  /* Open file for cur_file_no, or -1 if not open. */
  File cur_file_fh;
  /* The sofar position of redo in cur_file_no (end point of previous redo). */
  uint32_t cur_page_no;
  uint32_t cur_page_offset;

  uint32_t prev_page_no;
  uint16_t prev_offset;
  bool prev_space_id;

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
  bool update_page_from_record(uint16_t offset,
                               const byte *buf, size_t size) noexcept;
};


static binlog_recovery recover_obj;


static void innodb_binlog_prealloc_thread();
static int scan_for_binlogs(const char *binlog_dir, found_binlogs *binlog_files,
                            bool error_if_missing) noexcept;
static int innodb_binlog_discover();
static bool binlog_state_recover();
static void innodb_binlog_autopurge(uint64_t first_open_file_no, LF_PINS *pins);


/**
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
get_binlog_header(const char *binlog_path, byte *page_buf,
                  lsn_t &out_lsn, bool &out_empty) noexcept
{
  binlog_header_data header;

  out_empty= true;
  out_lsn= 0;

  File fh= my_open(binlog_path, O_RDONLY | O_BINARY, MYF(0));
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
  uint32_t crc32= uint4korr(page_buf + payload);
  if (UNIV_UNLIKELY(crc32 != my_crc32c(0, page_buf, payload)))
    return 0;

  fsp_binlog_extract_header_page(page_buf, &header);
  if (header.is_invalid)
    return 0;
  if (!header.is_empty)
  {
    out_empty= false;
    out_lsn= header.start_lsn;
  }
  return 1;
}


int
binlog_recovery::get_header(uint64_t file_no, lsn_t &out_lsn, bool &out_empty)
  noexcept
{
  char full_path[OS_FILE_MAX_PATH];
  binlog_name_make(full_path, file_no, binlog_dir);
  return get_binlog_header(full_path, page_buf, out_lsn, out_empty);
}


bool binlog_recovery::init_recovery(bool space_id, uint32_t page_no,
                                    uint16_t offset,
                                    lsn_t start_lsn, lsn_t end_lsn,
                                    const byte *buf, size_t size) noexcept
{
  /* Start by initializing resource pointers so we are safe to releaes(). */
  cur_file_fh= (File)-1;
  if (!(page_buf= static_cast<byte *>
        (ut_malloc(ibb_page_size, mem_key_binlog))))
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
  int num_binlogs= scan_result.num_found;
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
    {
      uint64_t start_file_no= file_no2;
      /*
        Only one binlog file found.

        This first recovery record may apply to the previous file (which has
        then presumably been purged since the last checkpoint). Or it may
        apply to this file, or only to the following file. The case where it
        is not this file needs a bit of care.

        If the recovery record lsn is less than the lsn in this file, we know
        that it must apply to the previous file, and we can start from this
        file.

        If the recovery record lsn is equal or greater, then it can apply to
        the previous file if it is part of a mini-transaction that spans into
        this file. Or it can apply to the following file. If it applies to the
        following file it must have page_no=0 and offset=0, since that file is
        missing and will be recovered from scratch. Conversely, if the record
        has page_no=0 and offset=0, it cannot apply to the previous file, as
        we keep mini-transactions smaller than one binlog file.
      */
      if (space_id != (file_no2 & 1) && start_lsn >= lsn2 &&
          page_no == 0 && offset == 0)
        ++start_file_no;
      return init_recovery_from(start_file_no, lsn2, page_no, offset,
                                start_lsn, buf, size);
    }

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
    {
      /*
        As above for the case where only one file is found, we need to
        carefully distinguish the case where the recovery record applies to
        file_no1-1 or file_no1+1; when start_lsn >= lsn1, the record can
        apply to file_no1+1 only if it is for page_no==0 and offset==0.
      */
      if (space_id != (file_no1 & 1) && start_lsn >= lsn1 &&
          page_no == 0 && offset == 0)
        return init_recovery_from(file_no2, lsn1, page_no, offset,
                                start_lsn, buf, size);
      else
        return init_recovery_from(file_no1, lsn1, page_no, offset,
                                start_lsn, buf, size);
    }
    else if (space_id == (file_no2 & 1) && start_lsn >= lsn2)
    {
      /* The record must apply to file_no2. */
      return init_recovery_from(file_no2, lsn2,
                                page_no, offset, start_lsn, buf, size);
    }
    else
    {
      /*
        The record cannot apply to file_no2, as either the space_id differs
        or the lsn is too early. Start from file_no1.
      */
      return init_recovery_from(file_no1, lsn1,
                                page_no, offset, start_lsn, buf, size);
    }
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
  prev_lsn= lsn;
  prev_space_id= file_no & 1;
  prev_page_no= page_no;
  prev_offset= offset;
  prev_size= size;
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
      skipping_partial_page= false;
      return update_page_from_record(offset, buf, size);
    }
  }
  return false;
}


/**
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
  prev_lsn= (lsn_t)0;
  prev_space_id= 0;
  prev_page_no= 0;
  prev_offset= 0;
  prev_size= 0;
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
         scan_result.num_found >= 1 && i <= scan_result.last_file_no;
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
  cur_file_fh= my_open(full_path, O_RDWR | O_BINARY, MYF(0));
  if (cur_file_fh < (File)0)
  {
    /*
      If we are on page 0 and the binlog file does not exist, then we should
      create it (and recover its content).
      Otherwise, it is an error, we cannot recover it as we are missing the
      start of it.
    */
    if (my_errno != ENOENT ||
        cur_page_no != 0 ||
        (cur_file_fh= my_open(full_path, O_RDWR | O_CREAT | O_TRUNC |
                              O_BINARY, MYF(0))) < (File)0)
    {
      my_error(EE_FILENOTFOUND, MYF(MY_WME), full_path, my_errno);
      return true;
    }
  }
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
  int res= crc32_pread_page(cur_file_fh, page_buf, 0, MYF(0));
  if (res <= 0)
  {
    sql_print_warning("InnoDB: Could not read last binlog file during recovery");
    return;
  }
  binlog_header_data header;
  fsp_binlog_extract_header_page(page_buf, &header);

  if (header.is_invalid)
  {
    sql_print_warning("InnoDB: Invalid header page in last binlog file "
                      "during recovery");
    return;
  }
  if (header.is_empty)
  {
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
  if (cur_page_offset && flush_page())
    return true;
  if (close_file())
    return true;
  ++cur_file_no;
  cur_page_no= 0;
  return false;
}


bool
binlog_recovery::next_page() noexcept
{
  if (cur_page_offset && flush_page())
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

  /*
    In a multi-batch recovery, InnoDB recovery redo parser will sometimes
    pass the same record(s) twice to the binlog recovery.

    The binlog recovery code wants to do consistency checks that records are
    processed in strict order, so we handle this special case by detecting
    and ignoring duplicate records.

    A duplicate record is determined by being in the same mtr (identified by
    end_lsn); and having page_no/offset either earlier in the same space_id,
    or later in a different space_id. Using the property that an mtr is always
    smaller than the binlog maximum file size.
  */
  if (end_lsn == prev_lsn &&
      ( ( space_id == prev_space_id &&
          ( ((uint64_t)page_no << 32 | offset) <=
            ((uint64_t)prev_page_no << 32 | prev_offset) ) ) ||
        ( space_id != prev_space_id &&
          ( ((uint64_t)page_no << 32 | offset) >
            ((uint64_t)prev_page_no << 32 | prev_offset) ) ) ) )
    return false;
  prev_lsn= end_lsn;
  prev_space_id= space_id;
  prev_page_no= page_no;
  prev_offset= offset;
  prev_size= size;

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
    if (cur_page_offset > BINLOG_PAGE_DATA &&
        cur_page_offset < ibb_page_size - BINLOG_PAGE_DATA_END &&
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

  return update_page_from_record(offset, buf, size);
}


bool
binlog_recovery::update_page_from_record(uint16_t offset,
                                         const byte *buf, size_t size) noexcept
{
  memcpy(page_buf + offset, buf, size);
  if (cur_page_no == 0 && offset == 0)
  {
    binlog_header_data header;
    /*
      This recovery record is for the file header page.
      This record is special, it covers only the used part of the header page.
      The reaminder of the page must be set to zeroes.
      Additionally, there is an extra CRC corresponding to a minimum
      page size of IBB_PAGE_SIZE_MIN, in anticipation for future configurable
      page size.
    */
    memset(page_buf + size, 0, ibb_page_size - (size + BINLOG_PAGE_DATA_END));
    cur_page_offset= (uint32_t)ibb_page_size - BINLOG_PAGE_DATA_END;
    uint32_t payload= IBB_HEADER_PAGE_SIZE - BINLOG_PAGE_CHECKSUM;
    int4store(page_buf + payload, my_crc32c(0, page_buf, payload));
    fsp_binlog_extract_header_page(page_buf, &header);
    if (header.is_invalid)
    {
      sql_print_error("InnoDB: Corrupt or invalid file header found during "
                      "recovery of file number %" PRIu64, cur_file_no);
      return !srv_force_recovery;
    }
    if (header.is_empty)
    {
      sql_print_error("InnoDB: Empty file header found during "
                      "recovery of file number %" PRIu64, cur_file_no);
      return !srv_force_recovery;
    }
    if (header.file_no != cur_file_no)
    {
      sql_print_error("InnoDB: Inconsistency in file header during recovery. "
                      "The header in file number %" PRIu64 " is for file "
                      "number %" PRIu64, cur_file_no, header.file_no);
      return !srv_force_recovery;
    }

    return false;
  }

  cur_page_offset= offset + (uint32_t)size;
  return false;
}


/**
  Check if this is an InnoDB binlog file name.
  Return the index/file_no if so.
*/
bool
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


dberr_t
innodb_binlog_startup_init()
{
  dberr_t err= fsp_binlog_init();
  if (err != DB_SUCCESS)
    return err;
  mysql_mutex_init(fsp_purge_binlog_mutex_key, &purge_binlog_mutex, nullptr);
  binlog_diff_state.init();
  ibb_xa_xid_hash= new ibb_xid_hash();
  if (UNIV_UNLIKELY(!ibb_xa_xid_hash))
  {
    sql_print_error("InnoDB: Could not allocate memory for the internal "
                    "XID hash, cannot proceed");
    return DB_OUT_OF_MEMORY;
  }

  innodb_binlog_inited= 1;
  return DB_SUCCESS;
}


static void
innodb_binlog_init_state()
{
  first_open_binlog_file_no= ~(uint64_t)0;
  for (uint32_t i= 0; i < 4; ++i)
  {
    binlog_cur_end_offset[i].store(~(uint64_t)0, std::memory_order_relaxed);
    binlog_cur_durable_offset[i].store(~(uint64_t)0, std::memory_order_relaxed);
  }
  last_created_binlog_file_no= ~(uint64_t)0;
  earliest_binlog_file_no= ~(uint64_t)0;
  total_binlog_used_size= 0;
  active_binlog_file_no.store(~(uint64_t)0, std::memory_order_release);
  ibb_file_hash.earliest_oob_ref.store(0, std::memory_order_relaxed);
  binlog_cur_page_no= 0;
  binlog_cur_page_offset= BINLOG_PAGE_DATA;
  current_binlog_state_interval=
    (uint64_t)(innodb_binlog_state_interval >> ibb_page_size_shift);
  ut_a(innodb_binlog_state_interval ==
       (current_binlog_state_interval << ibb_page_size_shift));
}


/** Start the thread that pre-allocates new binlog files. */
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


/**
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
  mtr_t mtr{nullptr};
  LF_PINS *lf_pins= lf_hash_get_pins(&ibb_file_hash.hash);
  ut_a(lf_pins);
  mtr.start();
  fsp_binlog_write_rec(&dummy_data, &mtr, FSP_BINLOG_TYPE_FILLER, lf_pins);
  uint64_t file_no= active_binlog_file_no.load(std::memory_order_relaxed);
  mtr.commit();
  lf_hash_put_pins(lf_pins);
  log_buffer_flush_to_disk(true);
  binlog_page_fifo->flush_up_to(0, 0);
  binlog_page_fifo->do_fdatasync(0);
  ibb_pending_lsn_fifo.add_to_fifo(mtr.commit_lsn(), file_no,
    binlog_cur_end_offset[file_no & 3].load(std::memory_order_relaxed));
}


void
ibb_set_max_size(size_t binlog_size)
{
  uint64_t pages= binlog_size >> ibb_page_size_shift;
  if (UNIV_LIKELY(pages > (uint64_t)UINT32_MAX)) {
    pages= UINT32_MAX;
    sql_print_warning("Requested max_binlog_size is larger than the maximum "
                      "InnoDB tablespace size, truncated to " UINT64PF,
                      (pages << ibb_page_size_shift));
  } else if (pages < 4) {
    pages= 4;
    sql_print_warning("Requested max_binlog_size is smaller than the minimum "
                      "size supported by InnoDB, truncated to " UINT64PF,
                      (pages << ibb_page_size_shift));
  }
  innodb_binlog_size_in_pages= (uint32_t)pages;
}


/**
  Open the InnoDB binlog implementation.
  This is called from server binlog layer if the user configured the binlog to
  use the innodb implementation (with --binlog-storage-engine=innodb).
*/
bool
innodb_binlog_init(size_t binlog_size, const char *directory)
{
  ibb_set_max_size(binlog_size);
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
    /* ToDo: Need to think more on the error handling if the binlog cannot be opened. We may need to abort starting the server, at least for some errors? And/or in some cases maybe force ignore any existing unusable files and continue with a new binlog (but then maybe innodb_binlog_discover() should return 0 and print warnings in the error log?). */
    return true;
  }
  if (res > 0)
  {
    /* We are continuing from existing binlogs. Recover the binlog state. */
    if (binlog_state_recover())
      return true;
  }

  start_binlog_prealloc_thread();

  if (res <= 0)
  {
    /*
      We are creating binlogs anew from scratch.
      Write and fsync the initial file-header, so that recovery will know where
      to start in case of a crash.
    */
    binlog_sync_initial();
  }

  return false;
}


/** Compute the (so far) last and last-but-one binlog files found. */
static void
process_binlog_name(found_binlogs *bls, uint64_t idx, size_t size)
{
  if (bls->num_found == 0)
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

  if (bls->num_found == 0 ||
      idx > bls->last_file_no) {
    if (bls->num_found >= 1 && idx == bls->last_file_no + 1) {
      bls->prev_file_no= bls->last_file_no;
      bls->prev_size= bls->last_size;
      bls->num_found= 2;
    } else {
      bls->num_found= 1;
    }
    bls->last_file_no= idx;
    bls->last_size= size;
  } else if (bls->num_found == 1 && idx + 1 == bls->last_file_no) {
    bls->num_found= 2;
    bls->prev_file_no= idx;
    bls->prev_size= size;
  }
}


/**
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

  binlog_files->num_found= 0;
  size_t num_entries= dir->number_of_files;
  fileinfo *entries= dir->dir_entry;
  for (size_t i= 0; i < num_entries; ++i) {
    const char *name= entries[i].name;
    uint64_t idx;
    if (!is_binlog_name(name, &idx))
      continue;
    process_binlog_name(binlog_files, idx, (size_t)entries[i].mystat->st_size);
  }
  my_dirend(dir);

  return 1;  /* Success */
}


static bool
binlog_page_empty(const byte *page)
{
  return page[BINLOG_PAGE_DATA] == 0;
}


/**
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
                   uint32_t *out_page_no, uint32_t *out_pos_in_page,
                   binlog_header_data *out_header_data)
{
  const uint32_t page_size= (uint32_t)ibb_page_size;
  const uint32_t page_size_shift= (uint32_t)ibb_page_size_shift;
  const uint32_t idx= file_no & 3;
  char file_name[OS_FILE_MAX_PATH];
  uint32_t p_0, p_1, p_2, last_nonempty;
  byte *p, *page_end;
  bool ret;

  *out_page_no= 0;
  *out_pos_in_page= BINLOG_PAGE_DATA;
  out_header_data->diff_state_interval= 0;
  out_header_data->is_invalid= true;

  binlog_name_make(file_name, file_no);
  pfs_os_file_t fh= os_file_create(innodb_data_file_key, file_name,
                                   OS_FILE_OPEN, OS_DATA_FILE,
                                   srv_read_only_mode, &ret);
  if (!ret) {
    sql_print_warning("InnoDB: Unable to open file '%s'", file_name);
    return -1;
  }

  int res= crc32_pread_page(fh, page_buf, 0, MYF(MY_WME));
  if (res <= 0) {
    os_file_close(fh);
    return -1;
  }
  fsp_binlog_extract_header_page(page_buf, out_header_data);
  if (out_header_data->is_invalid)
  {
    sql_print_error("InnoDB: Invalid or corrupt file header in file "
                    "'%s'", file_name);
    return -1;
  }
  if (out_header_data->is_empty) {
    ret=
      fsp_binlog_open(file_name, fh, file_no, file_size, ~(uint32_t)0, nullptr);
    binlog_cur_durable_offset[idx].store(0, std::memory_order_relaxed);
    binlog_cur_end_offset[idx].store(0, std::memory_order_relaxed);
    return (ret ? -1 : 0);
  }
  if (out_header_data->file_no != file_no)
  {
    sql_print_error("InnoDB: Inconsistent file header in file '%s', "
                    "wrong file_no %" PRIu64, file_name,
                    out_header_data->file_no);
    return -1;
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
    res= crc32_pread_page(fh, page_buf, p_1, MYF(MY_WME));
    if (res <= 0) {
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
  res= crc32_pread_page(fh, page_buf, last_nonempty, MYF(MY_WME));
  if (res <= 0) {
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

  /*
    Normalize the position, so that we store (page_no+1, BINLOG_PAGE_DATA)
    and not (page_no, page_size - BINLOG_PAGE_DATA_END).
  */
  byte *partial_page;
  if (p == page_end)
  {
    *out_page_no= p_0;
    *out_pos_in_page= BINLOG_PAGE_DATA;
    partial_page= nullptr;
  }
  else
  {
    *out_page_no= p_0 - 1;
    *out_pos_in_page= (uint32_t)(p - page_buf);
    partial_page= page_buf;
  }

  ret= fsp_binlog_open(file_name, fh, file_no, file_size,
                       *out_page_no, partial_page);
  uint64_t pos= (*out_page_no << page_size_shift) | *out_pos_in_page;
  binlog_cur_durable_offset[idx].store(pos, std::memory_order_relaxed);
  binlog_cur_end_offset[idx].store(pos, std::memory_order_relaxed);
  return ret ? -1 : 1;
}


static void
binlog_discover_init(uint64_t file_no, uint64_t interval)
{
  active_binlog_file_no.store(file_no, std::memory_order_release);
  ibb_file_hash.earliest_oob_ref.store(file_no, std::memory_order_relaxed);
  current_binlog_state_interval= interval;
  ibb_pending_lsn_fifo.init(file_no);
}


/**
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
  struct found_binlogs binlog_files;
  binlog_header_data header;

  int res= scan_for_binlogs(innodb_binlog_directory, &binlog_files, false);
  if (res <= 0)
  {
    if (res == 0)
      ibb_pending_lsn_fifo.init(0);
    return res;
  }

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
  if (binlog_files.num_found >= 1) {
    earliest_binlog_file_no= binlog_files.earliest_file_no;
    total_binlog_used_size= binlog_files.total_size;

    res= find_pos_in_binlog(binlog_files.last_file_no,
                            binlog_files.last_size,
                            page_buf.get(), &page_no, &pos_in_page,
                            &header);
    if (res < 0) {
      file_no= binlog_files.last_file_no;
      if (ibb_record_in_file_hash(file_no, ~(uint64_t)0, ~(uint64_t)0))
        return -1;
      binlog_discover_init(file_no, innodb_binlog_state_interval);
      sql_print_warning("Binlog number " UINT64PF " could no be opened. "
                        "Starting a new binlog file from number " UINT64PF,
                        binlog_files.last_file_no, (file_no + 1));
      return 0;
    }

    if (res > 0) {
      /* Found start position in the last binlog file. */
      file_no= binlog_files.last_file_no;
      if (ibb_record_in_file_hash(file_no, header.oob_ref_file_no,
                                  header.xa_ref_file_no))
        return -1;
      binlog_discover_init(file_no, header.diff_state_interval);
      binlog_cur_page_no= page_no;
      binlog_cur_page_offset= pos_in_page;
      sql_print_information("InnoDB: Continuing binlog number %" PRIu64
                            " from position %" PRIu64 ".", file_no,
                            (((uint64_t)page_no << page_size_shift)
                             | pos_in_page));
      return binlog_files.num_found;
    }

    /* res == 0, the last binlog is empty. */
    if (binlog_files.num_found >= 2) {
      /* The last binlog is empty, try the previous one. */
      res= find_pos_in_binlog(binlog_files.prev_file_no,
                              binlog_files.prev_size,
                              page_buf.get(),
                              &prev_page_no, &prev_pos_in_page,
                              &header);
      if (res < 0) {
        file_no= binlog_files.last_file_no;
        if (ibb_record_in_file_hash(file_no, ~(uint64_t)0, ~(uint64_t)0))
          return -1;
        binlog_discover_init(file_no, innodb_binlog_state_interval);
        binlog_cur_page_no= page_no;
        binlog_cur_page_offset= pos_in_page;
        sql_print_warning("Binlog number " UINT64PF " could not be opened, "
                          "starting from binlog number " UINT64PF " instead",
                          binlog_files.prev_file_no, file_no);
        return 1;
      }
      file_no= binlog_files.prev_file_no;
      if (ibb_record_in_file_hash(file_no, header.oob_ref_file_no,
                                  header.xa_ref_file_no))
        return -1;
      binlog_discover_init(file_no, header.diff_state_interval);
      binlog_cur_page_no= prev_page_no;
      binlog_cur_page_offset= prev_pos_in_page;
      sql_print_information("InnoDB: Continuing binlog number %" PRIu64
                            " from position %" PRIu64 ".", file_no,
                            (((uint64_t)prev_page_no << page_size_shift) |
                             prev_pos_in_page));
      return binlog_files.num_found;
    }

    /* Just one empty binlog file found. */
    file_no= binlog_files.last_file_no;
    if (ibb_record_in_file_hash(file_no, ~(uint64_t)0, ~(uint64_t)0))
      return -1;
    binlog_discover_init(file_no, innodb_binlog_state_interval);
    binlog_cur_page_no= page_no;
    binlog_cur_page_offset= pos_in_page;
    sql_print_information("InnoDB: Continuing binlog number %" PRIu64 " from "
                          "position %u.", file_no, BINLOG_PAGE_DATA);
    return binlog_files.num_found;
  }

  /* No binlog files found, start from scratch. */
  file_no= 0;
  earliest_binlog_file_no= 0;
  ibb_file_hash.earliest_oob_ref.store(0, std::memory_order_relaxed);
  total_binlog_used_size= 0;
  ibb_pending_lsn_fifo.init(0);
  current_binlog_state_interval= innodb_binlog_state_interval;
  sql_print_information("InnoDB: Starting a new binlog from file number %"
                        PRIu64 ".", file_no);
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
    delete ibb_xa_xid_hash;
    binlog_diff_state.free();
    fsp_binlog_shutdown();
    mysql_mutex_destroy(&purge_binlog_mutex);
  }
}


/**
  Background thread to close old binlog tablespaces and pre-allocate new ones.
*/
static void
innodb_binlog_prealloc_thread()
{
  my_thread_init();
#ifdef UNIV_PFS_THREAD
  pfs_register_thread(binlog_prealloc_thread_key);
#endif
  LF_PINS *lf_pins= lf_hash_get_pins(&ibb_file_hash.hash);
  ut_a(lf_pins);

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
      dberr_t res2= fsp_binlog_tablespace_create(last_created, size_in_pages,
                                                 lf_pins);
      if (earliest_binlog_file_no == ~(uint64_t)0)
        earliest_binlog_file_no= last_created;
      total_binlog_used_size+= (size_in_pages << ibb_page_size_shift);

      innodb_binlog_autopurge(first_open, lf_pins);
      mysql_mutex_unlock(&purge_binlog_mutex);

      mysql_mutex_lock(&active_binlog_mutex);
       /*
         ToDo: Error handling.
         For example, disk full, while tricky to handle well, should not crash
         the server at least.
       */
      ut_a(res2 == DB_SUCCESS);
      last_created_binlog_file_no= last_created;

      /* If we created the initial tablespace file, make it the active one. */
      ut_ad(active < ~(uint64_t)0 || last_created == 0);
      if (active == ~(uint64_t)0) {
        binlog_cur_end_offset[last_created & 3].
          store(0, std::memory_order_release);
        binlog_cur_durable_offset[last_created & 3]
          .store(0, std::memory_order_release);
        active_binlog_file_no.store(last_created, std::memory_order_relaxed);
        ibb_file_hash.earliest_oob_ref.store(last_created,
                                             std::memory_order_relaxed);
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
      continue;  /* Re-start loop after releasing/reacquiring mutex. */
    }

    /* Exit thread at server shutdown. */
    if (prealloc_thread_end)
      break;
    my_cond_wait(&active_binlog_cond, &active_binlog_mutex.m_mutex);

  }
  mysql_mutex_unlock(&active_binlog_mutex);

  lf_hash_put_pins(lf_pins);
  my_thread_end();

#ifdef UNIV_PFS_THREAD
  pfs_delete_thread();
#endif
}


bool
ibb_write_header_page(mtr_t *mtr, uint64_t file_no, uint64_t file_size_in_pages,
                      lsn_t start_lsn, uint64_t gtid_state_interval_in_pages,
                      LF_PINS *pins)
{
  fsp_binlog_page_entry *block;
  uint32_t used_bytes;

  block= binlog_page_fifo->create_page(file_no, 0);
  ut_a(block /* ToDo: error handling? */);
  byte *ptr= &block->page_buf()[0];
  uint64_t oob_ref_file_no=
    ibb_file_hash.earliest_oob_ref.load(std::memory_order_relaxed);
  uint64_t xa_ref_file_no=
    ibb_file_hash.earliest_xa_ref.load(std::memory_order_relaxed);
  ibb_file_hash.update_refs(file_no, pins, oob_ref_file_no, xa_ref_file_no);

  int4store(ptr, IBB_MAGIC);
  int4store(ptr + 4, ibb_page_size_shift);
  int4store(ptr + 8, IBB_FILE_VERS_MAJOR);
  int4store(ptr + 12, IBB_FILE_VERS_MINOR);
  int8store(ptr + 16, file_no);
  int8store(ptr + 24, file_size_in_pages);
  int8store(ptr + 32, start_lsn);
  int8store(ptr + 40, gtid_state_interval_in_pages);
  int8store(ptr + 48, oob_ref_file_no);
  int8store(ptr + 56, xa_ref_file_no);
  used_bytes= IBB_BINLOG_HEADER_SIZE;
  ut_ad(ibb_page_size >= IBB_HEADER_PAGE_SIZE);
  memset(ptr + used_bytes, 0, ibb_page_size - (used_bytes + BINLOG_PAGE_CHECKSUM));
  /*
    For future expansion with configurable page size:
    Write a CRC32 at the end of the minimal page size. This way, the header
    page can be read and checksummed without knowing the page size used in
    the file, and then the actual page size can be obtained from the header
    page.
  */
  const uint32_t payload= IBB_HEADER_PAGE_SIZE - BINLOG_PAGE_CHECKSUM;
  int4store(ptr + payload, my_crc32c(0, ptr, payload));

  fsp_log_header_page(mtr, block, file_no, used_bytes);
  binlog_page_fifo->release_page_mtr(block, mtr);

  return false;  // No error
}


__attribute__((noinline))
static ssize_t
serialize_gtid_state(rpl_binlog_state_base *state, byte *buf, size_t buf_size)
  noexcept
{
  unsigned char *p= (unsigned char *)buf;
  /*
    1 uint64_t for the number of entries in the state stored.
    1 uint64_t for the XA references file_no.
    2 uint32_t + 1 uint64_t for at least one GTID.
  */
  ut_ad(buf_size >= 2*COMPR_INT_MAX32 + 3*COMPR_INT_MAX64);
  p= compr_int_write(p, state->count_nolock());
  uint64_t xa_ref_file_no=
    ibb_file_hash.earliest_xa_ref.load(std::memory_order_relaxed);
  /* Write 1 +file_no, so that 0 (1 + ~0) means "no reference". */
  p= compr_int_write(p, xa_ref_file_no + 1);
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
                  uint32_t &page_offset, uint64_t file_no)
{
  /*
    Use a small, efficient stack-allocated buffer by default, falling back to
    malloc() if needed for large GTID state.
  */
  byte small_buf[192];
  byte *buf, *alloced_buf;
  uint32_t block_page_no= ~(uint32_t)0;
  block= nullptr;

  ssize_t used_bytes= serialize_gtid_state(state, small_buf, sizeof(small_buf));
  if (used_bytes >= 0)
  {
    buf= small_buf;
    alloced_buf= nullptr;
  }
  else
  {
    size_t buf_size= 2*COMPR_INT_MAX64 +
      state->count_nolock() * (2*COMPR_INT_MAX32 + COMPR_INT_MAX64);
    alloced_buf= static_cast<byte *>(ut_malloc(buf_size, mem_key_binlog));
    if (UNIV_UNLIKELY(!alloced_buf))
      return true;
    buf= alloced_buf;
    used_bytes= serialize_gtid_state(state, buf, buf_size);
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
  /* Page 0 is reserved for the header page. */
  ut_ad(page_no != 0);

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
      byte *ptr= page_offset + &block->page_buf()[0];
      uint32_t chunk= (uint32_t)used_bytes;
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
      fsp_log_binlog_write(mtr, block, file_no, block_page_no, page_offset,
                           (uint32)(chunk+3));
      page_offset+= chunk + 3;
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


/**
  Read a binlog state record. The passed in STATE object is updated with the
  state read.

  Returns:
    1  State record found
    0  No state record found
    -1 Error
*/
static int
read_gtid_state(binlog_chunk_reader *chunk_reader,
                rpl_binlog_state_base *state,
                uint64_t *out_xa_ref_file_no) noexcept
{
  byte buf[256];
  static_assert(sizeof(buf) >= 2*COMPR_INT_MAX64 + 6*COMPR_INT_MAX64,
                "buf must hold at least 2 GTIDs");
  int res= chunk_reader->read_data(buf, sizeof(buf), true);
  if (UNIV_UNLIKELY(res < 0))
    return -1;
  if (res == 0 || chunk_reader->cur_type() != FSP_BINLOG_TYPE_GTID_STATE)
    return 0;
  const byte *p= buf;
  const byte *p_end= buf + res;

  /* Read the number of GTIDs in the gtid state record. */
  std::pair<uint64_t, const unsigned char *> v_and_p= compr_int_read(buf);
  p= v_and_p.second;
  if (UNIV_UNLIKELY(p > p_end))
    return -1;
  uint64_t num_gtid= v_and_p.first;
  /*
    Read the earliest file_no containing pending XA if any.
    Note that unsigned underflow means 0 - 1 becomes ~0, as required.
  */
  v_and_p= compr_int_read(p);
  p= v_and_p.second;
  if (UNIV_UNLIKELY(p > p_end))
    return -1;
  *out_xa_ref_file_no= v_and_p.first - 1;

  /* Read each GTID one by one and add into the state. */
  for (uint64_t count= num_gtid; count > 0; --count)
  {
    ptrdiff_t remain= p_end - p;
    /* Read more data as needed to ensure we have read a full GTID. */
    if (UNIV_UNLIKELY(!chunk_reader->end_of_record()) &&
        UNIV_UNLIKELY(remain < 3*COMPR_INT_MAX64))
    {
      memmove(buf, p, remain);
      res= chunk_reader->read_data(buf + remain, (int)(sizeof(buf) - remain),
                                   true);
      if (UNIV_UNLIKELY(res < 0))
        return -1;
      p= buf;
      p_end= p + remain + res;
      remain+= res;
    }
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


/**
  Recover the GTID binlog state at startup.
  Read the full binlog state at the start of the current binlog file, as well
  as the last differential binlog state on top, if any. Then scan from there to
  the end to obtain the exact current GTID binlog state.

  Return false if ok, true if error.
*/
static bool
binlog_state_recover()
{
  rpl_binlog_state_base state;
  state.init();
  uint64_t active= active_binlog_file_no.load(std::memory_order_relaxed);
  uint64_t diff_state_interval= current_binlog_state_interval;
  uint32_t page_no= 1;
  uint64_t xa_ref_file_no;

  binlog_chunk_reader chunk_reader(binlog_cur_end_offset);
  byte *page_buf=
    static_cast<byte *>(ut_malloc(ibb_page_size, mem_key_binlog));
  if (!page_buf)
    return true;
  chunk_reader.set_page_buf(page_buf);
  chunk_reader.seek(active, page_no << ibb_page_size_shift);
  int res= read_gtid_state(&chunk_reader, &state, &xa_ref_file_no);
  if (res < 0)
  {
    ut_free(page_buf);
    return true;
  }
  if (diff_state_interval == 0)
  {
    sql_print_warning("Invalid differential binlog state interval " UINT64PF
                      " found in binlog file, ignoring", diff_state_interval);
  }
  else
  {
    page_no= (uint32_t)(binlog_cur_page_no -
                        (binlog_cur_page_no % diff_state_interval));
    while (page_no > 1)
    {
      chunk_reader.seek(active, page_no << ibb_page_size_shift);
      res= read_gtid_state(&chunk_reader, &state, &xa_ref_file_no);
      if (res > 0)
        break;
      page_no-= (uint32_t)diff_state_interval;
    }
  }
  ut_free(page_buf);

  ha_innodb_binlog_reader reader(false, active,
                                 page_no << ibb_page_size_shift);
  return binlog_recover_gtid_state(&state, &reader);
}


/** Allocate a context for out-of-band binlogging. */
static binlog_oob_context *
alloc_oob_context(uint32 list_length= 10)
{
  size_t needed= sizeof(binlog_oob_context) +
    list_length * sizeof(binlog_oob_context::node_info);
  binlog_oob_context *c=
    static_cast<binlog_oob_context *>(ut_malloc(needed, mem_key_binlog));
  if (c)
  {
    if (!(c->lf_pins= lf_hash_get_pins(&ibb_file_hash.hash)))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      ut_free(c);
      return nullptr;
    }
    c->stmt_start_point= nullptr;
    c->savepoint_stack= nullptr;
    c->pending_file_no= ~(uint64_t)0;
    c->node_list_alloc_len= list_length;
    c->node_list_len= 0;
    c->secondary_ctx= nullptr;
    c->pending_refcount= false;
    c->is_xa_prepared= false;
  }
  else
    my_error(ER_OUTOFMEMORY, MYF(0), needed);

  return c;
}


static void
innodb_binlog_write_cache(IO_CACHE *cache,
                       handler_binlog_event_group_info *binlog_info, mtr_t *mtr)
{
  binlog_oob_context *c=
    static_cast<binlog_oob_context *>(binlog_info->engine_ptr);
  if (!c)
    binlog_info->engine_ptr= c= alloc_oob_context();
  ut_a(c);

  if (unlikely(binlog_info->xa_xid))
  {
    /*
      Write an XID commit record just before the main commit record.
      The XID commit record just contains the XID, and is used by binlog XA
      crash recovery to ensure than the other storage engine(s) that are part
      of the transaciton commit or rollback consistently with the binlog
      engine.
    */
    chunk_data_xa_complete chunk_data2(binlog_info->xa_xid, true);
    fsp_binlog_write_rec(&chunk_data2, mtr, FSP_BINLOG_TYPE_XA_COMPLETE,
                         c->lf_pins);
  }

  chunk_data_cache chunk_data(cache, binlog_info);

  fsp_binlog_write_rec(&chunk_data, mtr, FSP_BINLOG_TYPE_COMMIT, c->lf_pins);
  uint64_t file_no= active_binlog_file_no.load(std::memory_order_relaxed);
  c->pending_file_no= file_no;
  c->pending_offset=
    binlog_cur_end_offset[file_no & 3].load(std::memory_order_relaxed);
}


static inline void
reset_oob_context(binlog_oob_context *c)
{
  if (c->stmt_start_point)
    c->stmt_start_point->node_list_len= 0;
  while (c->savepoint_stack != nullptr)
  {
    binlog_oob_context::savepoint *next_savepoint= c->savepoint_stack->next;
    ut_free(c->savepoint_stack);
    c->savepoint_stack= next_savepoint;
  }
  c->pending_file_no= ~(uint64_t)0;
  if (c->pending_refcount)
  {
    ibb_file_hash.oob_ref_dec(c->first_node_file_no, c->lf_pins);
    c->pending_refcount= false;
  }
  c->node_list_len= 0;
  c->secondary_ctx= nullptr;
  c->is_xa_prepared= false;
}


static inline void
free_oob_context(binlog_oob_context *c)
{
  ut_ad(!c->pending_refcount /* Should not have pending until free */);
  reset_oob_context(c);  /* Defensive programming, should be redundant */
  ut_free(c->stmt_start_point);
  lf_hash_put_pins(c->lf_pins);
  ut_free(c);
}


static binlog_oob_context *
ensure_oob_context(void **engine_data, uint32_t needed_len)
{
  binlog_oob_context *c= static_cast<binlog_oob_context *>(*engine_data);
  if (c->node_list_alloc_len >= needed_len)
    return c;
  if (needed_len < c->node_list_alloc_len + 10)
    needed_len= c->node_list_alloc_len + 10;
  binlog_oob_context *new_c= alloc_oob_context(needed_len);
  if (UNIV_UNLIKELY(!new_c))
    return nullptr;
  ut_ad(c->node_list_len <= c->node_list_alloc_len);
  memcpy(new_c, c, sizeof(binlog_oob_context) +
         c->node_list_len*sizeof(binlog_oob_context::node_info));
  new_c->node_list_alloc_len= needed_len;
  *engine_data= new_c;
  ut_free(c);
  return new_c;
}


/**
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
innodb_binlog_oob_ordered(THD *thd, const unsigned char *data, size_t data_len,
                          void **engine_data, void **stm_start_data,
                          void **savepoint_data)
{
  binlog_oob_context *c= static_cast<binlog_oob_context *>(*engine_data);
  if (!c)
    *engine_data= c= alloc_oob_context();
  if (UNIV_UNLIKELY(!c))
    return true;
  if (UNIV_UNLIKELY(c->is_xa_prepared))
  {
    my_error(ER_XAER_RMFAIL, MYF(0), "IDLE");
    return true;
  }

  if (stm_start_data)
  {
    if (c->create_stmt_start_point())
      return true;
    *stm_start_data= nullptr;  /* We do not need to store any data there. */
    if (data_len == 0 && !savepoint_data)
      return false;
  }
  if (savepoint_data)
  {
    binlog_oob_context::savepoint *sv= c->create_savepoint();
    if (!sv)
      return true;
    *((binlog_oob_context::savepoint **)savepoint_data)= sv;
    if (data_len == 0)
      return false;
  }
  ut_ad(data_len > 0);

  mtr_t mtr{thd_to_trx(thd)};
  uint32_t i= c->node_list_len;
  uint64_t new_idx= i==0 ? 0 : c->node_list[i-1].node_index + 1;
  if (i >= 2 && c->node_list[i-2].height == c->node_list[i-1].height)
  {
    /* Case 1: Replace two trees with a tree rooted in a new node. */
    binlog_oob_context::chunk_data_oob oob_data
      (new_idx,
       c->node_list[i-2].file_no, c->node_list[i-2].offset,
       c->node_list[i-1].file_no, c->node_list[i-1].offset,
       static_cast<const byte *>(data), data_len);
    if (c->binlog_node(i-2, new_idx, i-2, i-1, &oob_data, c->lf_pins, &mtr))
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
       static_cast<const byte *>(data), data_len);
    if (c->binlog_node(i, new_idx, i-1, i-1, &oob_data, c->lf_pins, &mtr))
      return true;
    c->node_list_len= i + 1;
  }
  else
  {
    /* Special case i==0, like case 2 but no prior node to link to. */
    binlog_oob_context::chunk_data_oob oob_data
      (new_idx, 0, 0, 0, 0, static_cast<const byte *>(data), data_len);
    if (c->binlog_node(i, new_idx, ~(uint32_t)0, ~(uint32_t)0, &oob_data,
                       c->lf_pins, &mtr))
      return true;
    c->first_node_file_no= c->node_list[i].file_no;
    c->first_node_offset= c->node_list[i].offset;
    c->node_list_len= 1;
    c->pending_refcount=
      !ibb_file_hash.oob_ref_inc(c->first_node_file_no, c->lf_pins);
  }

  uint64_t file_no= active_binlog_file_no.load(std::memory_order_relaxed);
  c->pending_file_no= file_no;
  c->pending_offset=
    binlog_cur_end_offset[file_no & 3].load(std::memory_order_relaxed);
  innodb_binlog_post_commit(&mtr, c);
  return false;
}


bool
innodb_binlog_oob(THD *thd, const unsigned char *data, size_t data_len,
                  void **engine_data)
{
  binlog_oob_context *c= static_cast<binlog_oob_context *>(*engine_data);
  if (UNIV_LIKELY(c != nullptr))
    ibb_pending_lsn_fifo.record_commit(c);
  return false;
}


/**
  Binlog a new out-of-band tree node and put it at position `node` in the list
  of trees. A leaf node is denoted by left and right child being identical (and
  in this case they point to the root of the prior tree).
*/
bool
binlog_oob_context::binlog_node(uint32_t node, uint64_t new_idx,
                                uint32_t left_node, uint32_t right_node,
                                chunk_data_oob *oob_data, LF_PINS *pins,
                                mtr_t *mtr)
{
  uint32_t new_height=
    left_node == right_node ? 1 : 1 + node_list[left_node].height;
  mtr->start();
  std::pair<uint64_t, uint64_t> new_file_no_offset=
    fsp_binlog_write_rec(oob_data, mtr, FSP_BINLOG_TYPE_OOB_DATA, pins);
  mtr->commit();
  node_list[node].file_no= new_file_no_offset.first;
  node_list[node].offset= new_file_no_offset.second;
  node_list[node].node_index= new_idx;
  node_list[node].height= new_height;
  return false;  // ToDo: Error handling?
}


binlog_oob_context::chunk_data_oob::chunk_data_oob(uint64_t idx,
        uint64_t left_file_no, uint64_t left_offset,
        uint64_t right_file_no, uint64_t right_offset,
        const byte *data, size_t data_len)
  : sofar(0), main_len(data_len), main_data(data)
{
  ut_ad(data_len > 0);
  byte *p= &header_buf[0];
  p= compr_int_write(p, idx);
  p= compr_int_write(p, left_file_no);
  p= compr_int_write(p, left_offset);
  p= compr_int_write(p, right_file_no);
  p= compr_int_write(p, right_offset);
  ut_ad((uint32_t)(p - &header_buf[0]) <= max_buffer);
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


bool
binlog_oob_context::create_stmt_start_point()
{
  if (!stmt_start_point || node_list_len > stmt_start_point->alloc_len)
  {
    ut_free(stmt_start_point);
    size_t size= sizeof(savepoint) + node_list_len * sizeof(node_info);
    stmt_start_point=
      static_cast<savepoint *>(ut_malloc(size, mem_key_binlog));
    if (!stmt_start_point)
    {
      my_error(ER_OUTOFMEMORY, MYF(0), size);
      return true;
    }
    stmt_start_point->alloc_len= node_list_len;
  }
  stmt_start_point->node_list_len= node_list_len;
  memcpy(stmt_start_point->node_list, node_list,
         node_list_len * sizeof(node_info));
  return false;
}


binlog_oob_context::savepoint *
binlog_oob_context::create_savepoint()
{
  size_t size= sizeof(savepoint) + node_list_len * sizeof(node_info);
  savepoint *s= static_cast<savepoint *>(ut_malloc(size, mem_key_binlog));
  if (!s)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), size);
    return nullptr;
  }
  s->next= savepoint_stack;
  s->node_list_len= node_list_len;
  memcpy(s->node_list, node_list, node_list_len * sizeof(node_info));
  savepoint_stack= s;
  return s;
}


void
binlog_oob_context::rollback_to_savepoint(savepoint *savepoint)
{
  ut_a(node_list_alloc_len >= savepoint->node_list_len);
  node_list_len= savepoint->node_list_len;
  memcpy(node_list, savepoint->node_list,
         savepoint->node_list_len * sizeof(node_info));

  /* Remove any later savepoints from the stack. */
  for (;;)
  {
    struct savepoint *s= savepoint_stack;
    ut_ad(s != nullptr /* Should always find the savepoint on the stack. */);
    if (UNIV_UNLIKELY(!s))
      break;
    if (s == savepoint)
      break;
    savepoint_stack= s->next;
    ut_free(s);
  }
}


void
binlog_oob_context::rollback_to_stmt_start()
{
  ut_a(node_list_alloc_len >= stmt_start_point->node_list_len);
  node_list_len= stmt_start_point->node_list_len;
  memcpy(node_list, stmt_start_point->node_list,
         stmt_start_point->node_list_len * sizeof(node_info));
}


void
ibb_savepoint_rollback(THD *thd, void **engine_data,
                       void **stmt_start_data, void **savepoint_data)
{
  binlog_oob_context *c= static_cast<binlog_oob_context *>(*engine_data);
  ut_a(c != nullptr);

  if (stmt_start_data)
  {
    ut_ad(savepoint_data == nullptr);
    c->rollback_to_stmt_start();
  }

  if (savepoint_data)
  {
    ut_ad(stmt_start_data == nullptr);
    binlog_oob_context::savepoint *savepoint=
      (binlog_oob_context::savepoint *)*savepoint_data;
    c->rollback_to_savepoint(savepoint);
  }
}


void
innodb_reset_oob(void **engine_data)
{
  binlog_oob_context *c= static_cast<binlog_oob_context *>(*engine_data);
  if (c)
    reset_oob_context(c);
}


void
innodb_free_oob(void *engine_data)
{
  free_oob_context(static_cast<binlog_oob_context *>(engine_data));
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


/**
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


ha_innodb_binlog_reader::ha_innodb_binlog_reader(bool wait_durable,
                                                 uint64_t file_no,
                                                 uint64_t offset)
  : chunk_rd(wait_durable ?
             binlog_cur_durable_offset : binlog_cur_end_offset),
    requested_file_no(~(uint64_t)0),
    rd_buf_len(0), rd_buf_sofar(0), state(ST_read_next_event_group)
{
  page_buf= static_cast<uchar *>(ut_malloc(ibb_page_size, mem_key_binlog));
  chunk_rd.set_page_buf(page_buf);
  if (offset < ibb_page_size)
    offset= ibb_page_size;
  chunk_rd.seek(file_no, offset);
  chunk_rd.skip_partial(true);
}


ha_innodb_binlog_reader::~ha_innodb_binlog_reader()
{
  ut_free(page_buf);
}


/**
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
  int sofar= 0;

again:
  switch (state)
  {
  case ST_read_next_event_group:
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

    if (UNIV_LIKELY(len > 0) && UNIV_LIKELY(!chunk_rd.end_of_record()))
    {
      res= chunk_rd.read_data(buf, len, false);
      if (res < 0)
        return -1;
      len-= res;
      buf+= res;
      sofar+= res;
    }

    if (UNIV_LIKELY(rd_buf_sofar == rd_buf_len) && chunk_rd.end_of_record())
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
      if (UNIV_UNLIKELY(oob_count2 > 0))
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
    if (UNIV_UNLIKELY(res == 0))
    {
      ut_ad(0 /* Should have had oob_traversal_done() last time then. */);
      if (sofar == 0)
        goto again;
    }
    return sofar + res;

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


bool
ha_innodb_binlog_reader::wait_available(THD *thd,
                                        const struct timespec *abstime)
{
  bool is_timeout= false;
  lsn_t pending_sync_lsn= 0;
  bool did_enter_cond= false;
  PSI_stage_info old_stage;

  if (data_available())
    return false;

  mysql_mutex_lock(&binlog_durable_mutex);
  for (;;)
  {
    /* Process anything that has become durable since we last looked. */
    lsn_t durable_lsn= log_sys.get_flushed_lsn(std::memory_order_relaxed);
    ibb_pending_lsn_fifo.process_durable_lsn(durable_lsn);

    /* Check if there is anything more pending to be made durable. */
    if (!ibb_pending_lsn_fifo.is_empty())
    {
      pending_lsn_fifo::entry &e= ibb_pending_lsn_fifo.cur_head();
      if (durable_lsn < e.lsn)
        pending_sync_lsn= e.lsn;
    }

    /*
      Check if there is data available for us now.
      As we are holding binlog_durable_mutex, active_binlog_file_no cannot
      move during this check.
    */
    uint64_t cur= active_binlog_file_no.load(std::memory_order_relaxed);
    uint64_t durable_offset=
      binlog_cur_durable_offset[cur & 3].load(std::memory_order_relaxed);
    if (durable_offset == 0 && chunk_rd.s.file_no + 1 == cur)
    {
      /*
        If active has durable position=0, it means the current durable
        position is somewhere in active-1.
      */
      cur= chunk_rd.s.file_no;
      durable_offset=
        binlog_cur_durable_offset[cur & 3].load(std::memory_order_relaxed);
    }
    if (chunk_rd.is_before_pos(cur, durable_offset))
      break;

    if (pending_sync_lsn != 0 && ibb_pending_lsn_fifo.flushing_lsn == 0)
    {
      /*
        There is no data available for us now, but there is data that will be
        available when the InnoDB redo log has been durably flushed to disk.
        So now we will do such a sync (unless another thread is already doing
        it), so we can proceed getting more data out.
      */
      ibb_pending_lsn_fifo.flushing_lsn= pending_sync_lsn;
      mysql_mutex_unlock(&binlog_durable_mutex);
      log_write_up_to(pending_sync_lsn, true);
      mysql_mutex_lock(&binlog_durable_mutex);
      ibb_pending_lsn_fifo.flushing_lsn= pending_sync_lsn= 0;
      /* Need to loop back to repeat all checks, after releasing the mutex. */
      continue;
    }

    if (thd && thd_kill_level(thd))
      break;

    if (thd && !did_enter_cond)
    {
      THD_ENTER_COND(thd, &binlog_durable_cond, &binlog_durable_mutex,
                     &stage_master_has_sent_all_binlog_to_slave, &old_stage);
      did_enter_cond= true;
    }
    if (abstime)
    {
      int res= mysql_cond_timedwait(&binlog_durable_cond,
                                    &binlog_durable_mutex,
                                    abstime);
      if (res == ETIMEDOUT)
      {
        is_timeout= true;
        break;
      }
    }
    else
      mysql_cond_wait(&binlog_durable_cond, &binlog_durable_mutex);
  }
  /*
    If there is pending binlog data to durably sync to the redo log, but we
    did not do this sync ourselves, then signal another thread (if any) to
    wakeup and sync. This is necessary to not lose the sync wakeup signal.

    (We use wake-one rather than wake-all for signalling a pending redo log
    sync to avoid wakeup-storm).
  */
  if (pending_sync_lsn != 0)
    mysql_cond_signal(&binlog_durable_cond);

  if (did_enter_cond)
    THD_EXIT_COND(thd, &old_stage);
  else
    mysql_mutex_unlock(&binlog_durable_mutex);

  return is_timeout;
}


handler_binlog_reader *
innodb_get_binlog_reader(bool wait_durable)
{
  return new ha_innodb_binlog_reader(wait_durable);
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


/**
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
  uint64_t dummy_xa_ref;
  /*
    Dirty read, but getting a slightly stale value is no problem, we will just
    be starting to scan the binlog file at a slightly earlier position than
    necessary.
  */
  uint64_t file_no= active_binlog_file_no.load(std::memory_order_relaxed);

  std::unique_ptr<byte, void (*)(byte *)>
    page_buf(static_cast<byte*>(ut_malloc(ibb_page_size, mem_key_binlog)),
             [](byte *p) {ut_free(p);});
  if (page_buf == nullptr)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), ibb_page_size);
    return -1;
  }
  binlog_chunk_reader chunk_reader(binlog_cur_durable_offset);
  chunk_reader.set_page_buf(page_buf.get());

  /* First search backwards for the right file to start from. */
  uint64_t diff_state_page_interval= 0;
  rpl_binlog_state_base base_state, page0_diff_state, tmp_diff_state;
  base_state.init();
  for (;;)
  {
    /* Read the header page, needed to get the binlog diff state interval. */
    binlog_header_data header;
    chunk_reader.seek(file_no, 0);
    int res= chunk_reader.get_file_header(&header);
    if (UNIV_UNLIKELY(res < 0))
      return -1;
    if (UNIV_UNLIKELY(res == 0))
        goto not_found_in_file;
    diff_state_page_interval= header.diff_state_interval;

    chunk_reader.seek(file_no, ibb_page_size);
    res= read_gtid_state(&chunk_reader, &base_state, &dummy_xa_ref);
    if (UNIV_UNLIKELY(res < 0))
      return -1;
    if (res == 0)
    {
  not_found_in_file:
      if (file_no == 0)
      {
        /* Handle the special case of a completely empty binlog file. */
        out_state->reset_nolock();
        *out_file_no= file_no;
        *out_offset= ibb_page_size;
        return 1;
      }
      /* If GTID state is not (durably) available, try the previous file. */
    }
    else if (base_state.is_before_pos(pos))
      break;
    base_state.reset_nolock();
    if (file_no <= earliest_binlog_file_no)
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
  uint32_t page2= (uint32_t) (diff_state_page_interval +
                ((chunk_reader.cur_end_offset - 1) >> ibb_page_size_shift));
  /* Round to the next diff_state_page_interval after file end. */
  page2-= page2 % (uint32_t)diff_state_page_interval;
  uint32_t page1= page0 +
    ((page2 - page0) /
         (2*(uint32_t)diff_state_page_interval) *
         (uint32_t)diff_state_page_interval);
  page0_diff_state.init();
  page0_diff_state.load_nolock(&base_state);
  tmp_diff_state.init();
  while (page1 >= page0 + diff_state_page_interval && page1 > 1)
  {
    ut_ad((page1 - page0) % diff_state_page_interval == 0);
    tmp_diff_state.reset_nolock();
    tmp_diff_state.load_nolock(&base_state);
    chunk_reader.seek(file_no, page1 << ibb_page_size_shift);
    int res= read_gtid_state(&chunk_reader, &tmp_diff_state, &dummy_xa_ref);
    if (UNIV_UNLIKELY(res < 0))
      return -1;
    if (res == 0)
    {
      /*
        If the diff state record was not written here for some reason, just
        try the one just before. It will be safe, even if not always optimal,
        and this is an abnormal situation anyway.
      */
      page1= page1 - (uint32_t)diff_state_page_interval;
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
    page1= page0 +
      ((page2 - page0) /
           (2*(uint32_t)diff_state_page_interval) *
           (uint32_t)diff_state_page_interval);
  }
  ut_ad(page1 >= page0);
  out_state->load_nolock(&page0_diff_state);
  *out_file_no= file_no;
  if (page0 == 0)
    page0= 1;  /* Skip the initial file header page. */
  *out_offset= (uint64_t)page0 << ibb_page_size_shift;
  return 1;
}


int
ha_innodb_binlog_reader::init_gtid_pos(THD *thd, slave_connection_state *pos,
                                       rpl_binlog_state_base *state)
{
  gtid_search search_obj;
  uint64_t file_no;
  uint64_t offset;

  /*
    Wait for at least the initial GTID state record to become durable before
    looking for the starting GTID position.
    This is unlikely to need to wait, as it would imply that _no_ part of the
    binlog is durable at this point. But it might theoretically occur perhaps
    after a PURGE of all binlog files but the active; and failing to do the
    wait if needed might wrongly return an error that the GTID position is
    too old.
  */
  chunk_rd.seek(earliest_binlog_file_no, ibb_page_size);
  if (UNIV_UNLIKELY(wait_available(thd, nullptr)))
    return -1;

  int res= search_obj.find_gtid_pos(pos, state, &file_no, &offset);
  if (res < 0)
    return -1;
  if (res > 0)
  {
    requested_file_no= file_no;
    chunk_rd.seek(file_no, offset);
    chunk_rd.skip_partial(true);
    cur_file_no= chunk_rd.current_file_no();
    cur_file_pos= chunk_rd.current_pos();
  }
  return res;
}


int
ha_innodb_binlog_reader::init_legacy_pos(THD *thd, const char *filename,
                                         ulonglong offset)
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
  if (file_no > active_binlog_file_no.load(std::memory_order_acquire))
  {
    my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0), "SHOW BINLOG EVENTS",
             "Could not find target log");
    return -1;
  }
  requested_file_no= file_no;
  if ((uint64_t)offset >= (uint64_t)(UINT32_MAX) << ibb_page_size_shift)
  {
    my_error(ER_BINLOG_POS_INVALID, MYF(0), offset);
    return -1;
  }

  if (offset < ibb_page_size)
    offset= ibb_page_size;

  /*
    Start at the beginning of the page containing the requested position. Then
    read forwards until the requested position is reached. This way we avoid
    reading garbaga data for invalid request offset.
  */

  chunk_rd.seek(file_no,
                (uint64_t)offset & ((uint64_t)~0 << ibb_page_size_shift));
  int err=
    chunk_rd.find_offset_in_page((uint32_t)(offset & (ibb_page_size - 1)));
  chunk_rd.release(true);
  chunk_rd.skip_partial(true);

  cur_file_no= chunk_rd.current_file_no();
  cur_file_pos= chunk_rd.current_pos();
  return err;
}


void
ha_innodb_binlog_reader::enable_single_file()
{
  chunk_rd.stop_file_no= requested_file_no != ~(uint64_t)0 ?
    requested_file_no : chunk_rd.s.file_no;
}


void
ha_innodb_binlog_reader::seek_internal(uint64_t file_no, uint64_t offset)
{
  chunk_rd.seek(file_no, offset);
  chunk_rd.skip_partial(true);
  cur_file_no= chunk_rd.current_file_no();
  cur_file_pos= chunk_rd.current_pos();
}


void
ibb_wait_durable_offset(uint64_t file_no, uint64_t wait_offset)
{
  uint64_t dur_offset=
    binlog_cur_durable_offset[file_no & 3].load(std:: memory_order_relaxed);
  ha_innodb_binlog_reader reader(true, file_no, dur_offset);
  for (;;)
  {
    reader.wait_available(nullptr, nullptr);
    dur_offset=
      binlog_cur_durable_offset[file_no & 3].load(std:: memory_order_relaxed);
    if (dur_offset >= wait_offset)
      break;
    reader.seek_internal(file_no, dur_offset);
  }
}


pending_lsn_fifo::pending_lsn_fifo()
  : flushing_lsn(0), last_lsn_added(0), cur_file_no(~(uint64_t)0),
    head(0), tail(0)
{
}


void
pending_lsn_fifo::init(uint64_t start_file_no)
{
  mysql_mutex_lock(&binlog_durable_mutex);
  ut_ad(cur_file_no == ~(uint64_t)0);
  cur_file_no= start_file_no;
  mysql_mutex_unlock(&binlog_durable_mutex);
}


void
pending_lsn_fifo::reset()
{
  mysql_mutex_lock(&binlog_durable_mutex);
  cur_file_no= ~(uint64_t)0;
  mysql_mutex_unlock(&binlog_durable_mutex);
}


bool
pending_lsn_fifo::process_durable_lsn(lsn_t lsn)
{
  mysql_mutex_assert_owner(&binlog_durable_mutex);
  ut_ad(cur_file_no != ~(uint64_t)0);

  entry *got= nullptr;
  for (;;)
  {
    if (is_empty())
      break;
    entry &e= cur_tail();
    if (lsn < e.lsn)
      break;
    got= &e;
    drop_tail();
  }
  if (got)
  {
    uint64_t active= active_binlog_file_no.load(std::memory_order_relaxed);
    DBUG_EXECUTE_IF("block_binlog_durable", active= got->file_no + 2;);
    if (got->file_no + 1 >= active)
    {
      /*
        We must never set the durable offset back to a prior value.
        This should be assured by never adding a smaller lsn into the fifo than
        any prior lsn added, and checked by this assertion.
      */
      ut_ad(binlog_cur_durable_offset[got->file_no & 3].
            load(std::memory_order_relaxed) <= got->offset);
      binlog_cur_durable_offset[got->file_no & 3].store
        (got->offset, std::memory_order_relaxed);
    }
    /*
      If we moved the durable point to the next file_no, mark the prior
      file_no as now fully durable.
      Since we only ever have at most two binlog tablespaces open, and since
      we make file_no=N fully durable (by calling into this function) before
      pre-allocating N+2, we can only ever move ahead one file_no at a time
      here.
    */
    if (cur_file_no != got->file_no)
    {
      ut_ad(got->file_no == cur_file_no + 1);
      binlog_cur_durable_offset[cur_file_no & 3].store(
        binlog_cur_end_offset[cur_file_no & 3].load(std::memory_order_relaxed),
        std::memory_order_relaxed);
      cur_file_no= got->file_no;
    }
    mysql_cond_broadcast(&binlog_durable_cond);
    return true;
  }
  return false;
}


/**
  After a binlog commit, put the LSN and the corresponding binlog position
  into the ibb_pending_lsn_fifo. We do this here (rather than immediately in
  innodb_binlog_post_commit()), so that we can delay it until we are no longer
  holding more critical locks that could block other writers. As we will be
  contending with readers here on binlog_durable_mutex.
*/
void
pending_lsn_fifo::record_commit(binlog_oob_context *c)
{
  uint64_t pending_file_no= c->pending_file_no;
  if (pending_file_no == ~(uint64_t)0)
    return;
  c->pending_file_no= ~(uint64_t)0;
  lsn_t pending_lsn= c->pending_lsn;
  uint64_t pending_offset= c->pending_offset;
  add_to_fifo(pending_lsn, pending_file_no, pending_offset);
}


void
pending_lsn_fifo::add_to_fifo(uint64_t lsn, uint64_t file_no, uint64_t offset)
{
  mysql_mutex_lock(&binlog_durable_mutex);
  /*
    The record_commit() operation is done outside of critical locks for
    scalabitily, so can occur out-of-order. So only insert the new entry if
    it is newer than any previously inserted.
  */
  ut_ad(is_empty() || cur_head().lsn == last_lsn_added);
  if (lsn > last_lsn_added)
  {
    if (is_full())
    {
      /*
        When the fifo is full, we just overwrite the head with a newer LSN.
        This way, whenever _some_ LSN gets synced durably to disk, we will
        always be able to make some progress and clear some fifo entries. And
        when this latest LSN gets eventually synced, any overwritten entry
        will progress as well.
      */
    }
    else
    {
      /*
        Insert a new head.
        Note that we make the fifo size a power-of-two (2 <<fixed_size_log2).
        So if we wrap around uint32_t here, the outcome is still valid.
      */
      new_head();
    }
    entry &h= cur_head();
    h.file_no= file_no;
    h.offset= offset;
    h.lsn= lsn;
    last_lsn_added= lsn;
    /* Make an immediate check in case the LSN is already durable. */
    bool signalled=
      process_durable_lsn(log_sys.get_flushed_lsn(std::memory_order_relaxed));
    if (!signalled && flushing_lsn == 0)
    {
      /*
        If process_durable_lsn() did not find any new data become durable, it
        does not broadcast a wakeup signal. But since we inserted a new entry
        in the fifo, we still want to signal _one_ other thread to potentially
        wake up and start a redo log sync to make the new entry durable, unless
        a thread is already doing such redo log sync.
      */
      mysql_cond_signal(&binlog_durable_cond);
    }
  }
  mysql_mutex_unlock(&binlog_durable_mutex);
}


static const uchar *get_xid_hash_key(const void *p, size_t *out_len, my_bool)
{
  const XID *xid= &(reinterpret_cast<const ibb_xid_hash::xid_elem *>(p)->xid);
  *out_len= xid->key_length();
  return xid->key();
}


ibb_xid_hash::ibb_xid_hash()
{
  mysql_mutex_init(ibb_xid_hash_mutex_key, &xid_mutex, nullptr);
  my_hash_init(mem_key_binlog, &xid_hash, &my_charset_bin, 32, 0,
               sizeof(XID), get_xid_hash_key, nullptr, MYF(HASH_UNIQUE));
}


ibb_xid_hash::~ibb_xid_hash()
{
  for (uint32 i= 0; i < xid_hash.records; ++i)
    my_free(my_hash_element(&xid_hash, i));
  my_hash_free(&xid_hash);
  mysql_mutex_destroy(&xid_mutex);
}


bool
ibb_xid_hash::add_xid(const XID *xid, const binlog_oob_context *c)
{
  xid_elem *e=
    (xid_elem *)my_malloc(mem_key_binlog, sizeof(xid_elem), MYF(MY_WME));
  if (!e)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int)sizeof(xid_elem));
    return true;
  }
  e->xid.set(xid);
  uint64_t refcnt_file_no;
  if (UNIV_LIKELY(c->node_list_len > 0))
  {
    uint32_t last= c->node_list_len-1;
    e->oob_num_nodes= c->node_list[last].node_index + 1;
    e->oob_first_file_no= c->first_node_file_no;
    e->oob_first_offset= c->first_node_offset;
    e->oob_last_file_no= c->node_list[last].file_no;
    e->oob_last_offset= c->node_list[last].offset;
    refcnt_file_no= e->oob_first_file_no;
  }
  else
  {
    e->oob_num_nodes= 0;
    e->oob_first_file_no= 0;
    e->oob_first_offset= 0;
    e->oob_last_file_no= 0;
    e->oob_last_offset= 0;
    /*
      Empty XA transaction, but we still need to ensure the prepare record
      is kept until the (empty) transactions gets XA COMMMIT'ted.
    */
    refcnt_file_no= active_binlog_file_no.load(std::memory_order_acquire);
  }
  e->refcnt_file_no= refcnt_file_no;
  mysql_mutex_lock(&xid_mutex);
  if (my_hash_insert(&xid_hash, (uchar *)e))
  {
    mysql_mutex_unlock(&xid_mutex);
    my_free(e);
    return true;
  }
  mysql_mutex_unlock(&xid_mutex);
  ibb_file_hash.oob_ref_inc(refcnt_file_no, c->lf_pins, true);
  return false;
}


template <typename F> bool
ibb_xid_hash::run_on_xid(const XID *xid, F callback)
{
  size_t key_len= 0;
  const uchar *key_ptr= get_xid_hash_key(xid, &key_len, 1);
  bool err;

  mysql_mutex_lock(&xid_mutex);
  uchar *rec= my_hash_search(&xid_hash, key_ptr, key_len);
  if (UNIV_LIKELY(rec != nullptr))
  {
    err= callback(reinterpret_cast<xid_elem *>(rec));
  }
  else
    err= true;
  mysql_mutex_unlock(&xid_mutex);
  return err;
}


/*
  Look up an XID in the internal XID hash.
  Remove the entry found (if any) and return it.
*/
ibb_xid_hash::xid_elem *
ibb_xid_hash::grab_xid(const XID *xid)
{
  xid_elem *e= nullptr;
  size_t key_len= 0;
  const uchar *key_ptr= get_xid_hash_key(xid, &key_len, 1);
  mysql_mutex_lock(&xid_mutex);
  uchar *rec= my_hash_search(&xid_hash, key_ptr, key_len);
  if (UNIV_LIKELY(rec != nullptr))
  {
    e= reinterpret_cast<xid_elem *>(rec);
    my_hash_delete(&xid_hash, rec);
  }
  mysql_mutex_unlock(&xid_mutex);
  return e;
}


void
ibb_get_filename(char name[FN_REFLEN], uint64_t file_no)
{
  static_assert(BINLOG_NAME_MAX_LEN <= FN_REFLEN,
                "FN_REFLEN too shot to hold InnoDB binlog name");
  binlog_name_make_short(name, file_no);
}


extern "C" void binlog_get_cache(THD *, uint64_t, uint64_t, IO_CACHE **,
                                 handler_binlog_event_group_info **,
                                 const rpl_gtid **);

binlog_oob_context *
innodb_binlog_trx(trx_t *trx, mtr_t *mtr)
{
  IO_CACHE *cache;
  handler_binlog_event_group_info *binlog_info;
  const rpl_gtid *gtid;
  uint64_t file_no, pos;

  if (!trx->mysql_thd)
    return nullptr;
  innodb_binlog_status(&file_no, &pos);
  binlog_get_cache(trx->mysql_thd, file_no, pos, &cache, &binlog_info, &gtid);
  if (UNIV_LIKELY(binlog_info != nullptr) &&
      UNIV_LIKELY(binlog_info->gtid_offset > 0)) {
    binlog_diff_state.update_nolock(gtid);
    innodb_binlog_write_cache(cache, binlog_info, mtr);
    return static_cast<binlog_oob_context *>(binlog_info->engine_ptr);
  }
  return nullptr;
}


void
innodb_binlog_post_commit(mtr_t *mtr, binlog_oob_context *c)
{
  if (c)
  {
    c->pending_lsn= mtr->commit_lsn();
    ut_ad(c->pending_lsn != 0);
  }
}


/*
  Function to record the write of a record to the binlog, when done outside
  of a normal binlog commit, eg. XA PREPARE or XA ROLLBACK.
*/
static void
innodb_binlog_post_write_rec(mtr_t *mtr, binlog_oob_context *c)
{
  uint64_t file_no= active_binlog_file_no.load(std::memory_order_relaxed);
  c->pending_file_no= file_no;
  c->pending_offset=
    binlog_cur_end_offset[file_no & 3].load(std::memory_order_relaxed);
  innodb_binlog_post_commit(mtr, c);
}


bool
innobase_binlog_write_direct_ordered(IO_CACHE *cache,
                             handler_binlog_event_group_info *binlog_info,
                             const rpl_gtid *gtid)
{
  mtr_t mtr{nullptr};
  ut_ad(binlog_info->engine_ptr2 == nullptr);
  if (gtid)
    binlog_diff_state.update_nolock(gtid);
  innodb_binlog_status(&binlog_info->out_file_no, &binlog_info->out_offset);
  mtr.start();
  innodb_binlog_write_cache(cache, binlog_info, &mtr);
  mtr.commit();
  innodb_binlog_post_commit(&mtr, static_cast<binlog_oob_context *>
                            (binlog_info->engine_ptr));
  return false;
}


bool
innobase_binlog_write_direct(IO_CACHE *cache,
                             handler_binlog_event_group_info *binlog_info,
                             const rpl_gtid *gtid)
{
  ut_ad(binlog_info->engine_ptr2 == nullptr);
  binlog_oob_context *c=
    static_cast<binlog_oob_context *>(binlog_info->engine_ptr);
  if (UNIV_LIKELY(c != nullptr))
  {
    if (srv_flush_log_at_trx_commit & 1)
      log_write_up_to(c->pending_lsn, true);
    ibb_pending_lsn_fifo.record_commit(c);
  }
  return false;
}


void
ibb_group_commit(THD *thd, handler_binlog_event_group_info *binlog_info)
{
  binlog_oob_context *c=
    static_cast<binlog_oob_context *>(binlog_info->engine_ptr);
  if (UNIV_LIKELY(c != nullptr))
  {
    if (srv_flush_log_at_trx_commit & 1 && c->pending_lsn)
    {
      /*
        Sync the InnoDB redo log durably to disk here for the entire group
        commit, so that it will be available for all binlog readers.
      */
      log_write_up_to(c->pending_lsn, true);
    }
    ibb_pending_lsn_fifo.record_commit(c);
  }
}


bool
ibb_write_xa_prepare_ordered(THD *thd,
                             handler_binlog_event_group_info *binlog_info,
                             uchar engine_count)
{
  mtr_t mtr{nullptr};
  binlog_oob_context *c=
    static_cast<binlog_oob_context *>(binlog_info->engine_ptr);
  // ToDo: Here need also the oob ref.
  chunk_data_xa_prepare chunk_data(binlog_info->xa_xid, engine_count);
  mtr.start();
  fsp_binlog_write_rec(&chunk_data, &mtr, FSP_BINLOG_TYPE_XA_PREPARE,
                       c->lf_pins);
  mtr.commit();
  innodb_binlog_post_write_rec(&mtr, c);

  return false;
}


bool
ibb_write_xa_prepare(THD *thd,
                     handler_binlog_event_group_info *binlog_info,
                     uchar engine_count)
{
  binlog_oob_context *c=
    static_cast<binlog_oob_context *>(binlog_info->engine_ptr);
  ut_ad(binlog_info->xa_xid != nullptr);
  if (ibb_xa_xid_hash->add_xid(binlog_info->xa_xid, c))
    return true;

  /*
    Sync the redo log to ensure that the prepare record is durably written to
    disk. This is necessary before returning OK to the client, to be sure we
    can recover the binlog part of the XA transaction in case of crash.
  */
  if (srv_flush_log_at_trx_commit > 0)
    log_write_up_to(c->pending_lsn, (srv_flush_log_at_trx_commit & 1));
  ibb_pending_lsn_fifo.record_commit(c);

  return false;
}


bool
ibb_xa_rollback_ordered(THD *thd, const XID *xid, void **engine_data)
{
  binlog_oob_context *c=
    static_cast<binlog_oob_context *>(*engine_data);
  if (UNIV_UNLIKELY(c == nullptr))
    *engine_data= c= alloc_oob_context();

  /*
    Write ROLLBACK record to the binlog.
    This will be used during recovery to know that the XID is no longer active,
    allowing purge of the associated binlogs.
  */
  chunk_data_xa_complete chunk_data(xid, false);
  mtr_t mtr{nullptr};
  mtr.start();
  fsp_binlog_write_rec(&chunk_data, &mtr, FSP_BINLOG_TYPE_XA_COMPLETE,
                       c->lf_pins);
  mtr.commit();
  innodb_binlog_post_write_rec(&mtr, c);

  return false;
}


bool
ibb_xa_rollback(THD *thd, const XID *xid, void **engine_data)
{
  binlog_oob_context *c=
    static_cast<binlog_oob_context *>(*engine_data);

  /*
    Keep the reference count here, as we need the rollback record to be
    available for recovery until all engines have durably rolled back.
    Decrement will happen after that, in ibb_binlog_unlog().
  */

  /*
    Durably write the rollback record to disk. This way, when we return the
    "ok" packet to the client, we are sure that crash recovery will make the
    XID rollback in engines if needed.
  */
  ut_ad(c->pending_lsn > 0);
  if (srv_flush_log_at_trx_commit > 0)
    log_write_up_to(c->pending_lsn, (srv_flush_log_at_trx_commit & 1));

  ibb_pending_lsn_fifo.record_commit(c);
  c->pending_lsn= 0;
  return false;
}


void
ibb_binlog_unlog(const XID *xid, void **engine_data)
{
  binlog_oob_context *c=
    static_cast<binlog_oob_context *>(*engine_data);
  if (UNIV_UNLIKELY(c == nullptr))
    *engine_data= c= alloc_oob_context();
  ibb_xid_hash::xid_elem *elem= ibb_xa_xid_hash->grab_xid(xid);
  if (elem)
  {
    ibb_file_hash.oob_ref_dec(elem->refcnt_file_no, c->lf_pins, true);
    my_free(elem);
  }
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
innodb_binlog_status(uint64_t *out_file_no, uint64_t *out_pos)
{
  static_assert(BINLOG_NAME_MAX_LEN <= FN_REFLEN,
                "FN_REFLEN too shot to hold InnoDB binlog name");
  uint64_t file_no= active_binlog_file_no.load(std::memory_order_relaxed);
  uint32_t page_no= binlog_cur_page_no;
  uint32_t in_page_offset= binlog_cur_page_offset;
  *out_file_no= file_no;
  *out_pos= ((uint64_t)page_no << ibb_page_size_shift) | in_page_offset;
}


bool
innodb_binlog_get_init_state(rpl_binlog_state_base *out_state)
{
  binlog_chunk_reader chunk_reader(binlog_cur_end_offset);
  bool err= false;
  uint64_t dummy_xa_ref;

  byte *page_buf= static_cast<byte *>(ut_malloc(ibb_page_size, mem_key_binlog));
  if (!page_buf)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), ibb_page_size);
    return true;
  }
  chunk_reader.set_page_buf(page_buf);

  mysql_mutex_lock(&purge_binlog_mutex);
  chunk_reader.seek(earliest_binlog_file_no, ibb_page_size);
  int res= read_gtid_state(&chunk_reader, out_state, &dummy_xa_ref);
  mysql_mutex_unlock(&purge_binlog_mutex);
  if (res != 1)
    err= true;
  ut_free(page_buf);
  return err;

}


bool
innodb_reset_binlogs()
{
  bool err= false;
  LF_PINS *lf_pins= lf_hash_get_pins(&ibb_file_hash.hash);
  ut_a(lf_pins);
  ut_a(innodb_binlog_inited >= 2);

  uint64_t active= active_binlog_file_no.load(std::memory_order_relaxed);
  if (ibb_file_hash.check_any_oob_ref_in_use(earliest_binlog_file_no,
                                             active, lf_pins))
  {
    my_error(ER_BINLOG_IN_USE_TRX, MYF(0));
    return true;
  }

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
  ibb_pending_lsn_fifo.reset();

  ibb_file_hash.remove_up_to(last_created_binlog_file_no, lf_pins);

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
      /*
        Just as defensive coding, also remove any entry from the file hash
        with this file_no. We would expect to have already deleted everything
        in remove_up_to() above.
      */
      ibb_file_hash.remove(file_no, lf_pins);
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
  ibb_pending_lsn_fifo.init(0);
  binlog_page_fifo->unlock_with_delayed_free();
  start_binlog_prealloc_thread();
  binlog_sync_initial();

  lf_hash_put_pins(lf_pins);
  return err;
}


/*
  Given a limit_file_no that is still needed by a slave (dump thread).
  The dump thread will need to read any oob records references from event
  groups in that file_no, so it will then also need to read from any earlier
  file_no referenced from limit_file_no.

  This function handles this dependency, by reading the header page (or
  getting from the ibb_file_hash if available) to get any earlier file_no
  containing such references.
*/
static bool
purge_adjust_limit_file_no(handler_binlog_purge_info *purge_info, LF_PINS *pins)
{
  uint64_t limit_file_no= purge_info->limit_file_no;
  if (limit_file_no == ~(uint64_t)0)
    return false;

  uint64_t referenced_file_no;
  if (ibb_file_hash.get_oob_ref_file_no(limit_file_no, pins,
                                        &referenced_file_no))
  {
    if (referenced_file_no < limit_file_no)
      purge_info->limit_file_no= referenced_file_no;
    else
      ut_ad(referenced_file_no == limit_file_no ||
            referenced_file_no == ~(uint64_t)0);
    return false;
  }

  byte *page_buf= static_cast<byte *>(ut_malloc(ibb_page_size, mem_key_binlog));
  if (!page_buf)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), ibb_page_size);
    return true;
  }
  char filename[OS_FILE_MAX_PATH];
  binlog_name_make(filename, limit_file_no);
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
    my_error(ER_ERROR_ON_READ, MYF(0), filename, my_errno);
    return true;
  }
  if (header.oob_ref_file_no < limit_file_no)
    purge_info->limit_file_no= header.oob_ref_file_no;
  else
    ut_ad(header.oob_ref_file_no == limit_file_no ||
          header.oob_ref_file_no == ~(uint64_t)0);
  ibb_record_in_file_hash(limit_file_no, header.oob_ref_file_no,
                          header.xa_ref_file_no, pins);
  return false;
}


/**
  The low-level function handling binlog purge.

  How much to purge is determined by:

  1. Lowest file_no that should not be purged. This is determined as the
  minimum of:
    1a. active_binlog_file_no
    1b. first_open_binlog_file_no
    1c. Any file_no in use by an active dump thread
    1d. Any file_no containing oob data referenced by file_no from (1c)
    1e. Any file_no containing oob data referenced by an active transaction.
    1f. User specified file_no (from PURGE BINARY LOGS TO, if any).

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
innodb_binlog_purge_low(handler_binlog_purge_info *purge_info,
                        uint64_t limit_name_file_no, LF_PINS *lf_pins,
                        uint64_t *out_file_no)
  noexcept
{
  uint64_t limit_file_no= purge_info->limit_file_no;
  bool by_date= purge_info->purge_by_date;
  bool by_size= purge_info->purge_by_size;
  bool by_name= purge_info->purge_by_name;
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
                          "binlog file '%s' (errno: %d)", filename, my_errno);
      continue;
    }

    if (by_date && stat_buf.st_mtime < purge_info->limit_date)
      want_purge= true;
    if (by_size && loc_total_size > purge_info->limit_size)
      want_purge= true;
    if (by_name && file_no < limit_name_file_no)
      want_purge= true;
    if (!want_purge ||
        file_no >= limit_file_no ||
        ibb_file_hash.get_oob_ref_in_use(file_no, lf_pins))
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
      loc_total_size-= (size_t)stat_buf.st_size;

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

    ibb_file_hash.remove(file_no, lf_pins);
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
                          "file '%s' (errno: %d)", filename, my_errno);
        continue;
      }
    }
  }
  total_binlog_used_size= loc_total_size;
  *out_file_no= file_no;
  return (want_purge ? 1 : 0);
}


static void
innodb_binlog_autopurge(uint64_t first_open_file_no, LF_PINS *pins)
{
  handler_binlog_purge_info purge_info;
#ifdef HAVE_REPLICATION
  extern bool ha_binlog_purge_info(handler_binlog_purge_info *out_info);
  bool can_purge= ha_binlog_purge_info(&purge_info);
#else
  bool can_purge= false;
  memset(&purge_info, 0, sizeof(purge_info));  /* Silence compiler warnings. */
#endif
  if (!can_purge ||
      !(purge_info.purge_by_size || purge_info.purge_by_date))
    return;

  if (purge_adjust_limit_file_no(&purge_info, pins))
    return;

  /* Don't purge any actively open tablespace files. */
  uint64_t orig_limit_file_no= purge_info.limit_file_no;
  if (purge_info.limit_file_no == ~(uint64_t)0 ||
      purge_info.limit_file_no > first_open_file_no)
    purge_info.limit_file_no= first_open_file_no;
  uint64_t active= active_binlog_file_no.load(std::memory_order_relaxed);
  if (purge_info.limit_file_no > active)
    purge_info.limit_file_no= active;
  purge_info.purge_by_name= false;

  uint64_t file_no;
  int res= innodb_binlog_purge_low(&purge_info, 0, pins, &file_no);
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
      else if (orig_limit_file_no == file_no)
        sql_print_information("InnoDB: Binlog file %s could not be purged "
                              "because it is in use by a binlog dump thread "
                              "(connected slave)", filename);
      else if (purge_info.limit_file_no == file_no)
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

  LF_PINS *lf_pins= lf_hash_get_pins(&ibb_file_hash.hash);
  ut_a(lf_pins);
  if (purge_adjust_limit_file_no(purge_info, lf_pins))
  {
    lf_hash_put_pins(lf_pins);
    return LOG_INFO_IO;
  }

  uint64_t orig_limit_file_no= purge_info->limit_file_no;
  purge_info->limit_file_no= std::min(orig_limit_file_no, limit_file_no);

  mysql_mutex_lock(&purge_binlog_mutex);
  uint64_t file_no;
  int res= innodb_binlog_purge_low(purge_info, to_file_no, lf_pins, &file_no);
  mysql_mutex_unlock(&purge_binlog_mutex);
  lf_hash_put_pins(lf_pins);

  if (res == 1)
  {
    static_assert(sizeof(purge_info->nonpurge_filename) >= BINLOG_NAME_MAX_LEN,
                  "No room to return filename");
    binlog_name_make_short(purge_info->nonpurge_filename, file_no);
    if (!purge_info->nonpurge_reason)
    {
      if (limit_file_no == file_no)
        purge_info->nonpurge_reason= "the binlog file is in active use";
      else if (orig_limit_file_no == file_no)
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
