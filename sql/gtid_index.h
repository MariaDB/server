/*
   Copyright (c) 2023 Kristian Nielsen <knielsen@knielsen-hq.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/

#ifndef GTID_INDEX_H
#define GTID_INDEX_H

#include "my_global.h"
#include "mysqld.h"
#include "mariadb.h"
#include "rpl_gtid.h"

/*
  This implements an on-disk index for each binlog file to speed up access to
  the binlog at a specific offset or GTID position. This is primarily used when
  a slave connects to the master, but also by user calling BINLOG_GTID_POS().

  A connecting slave first scans the binlog files to find the last one with an
  initial GTID_LIST event that lies before the starting GTID position. Then a
  sequential scan of the binlog file is done until the requested GTID position
  is found.

  The binlog index conceptually extends this using index records corresponding
  to different offset within one binlog file. Each record functions as if it
  was the initial GTID_LIST event of a new binlog file, allowing the
  sequential scan to start from the corresponding position. By having
  sufficiently many index records, the scan will be fast.

  The code that adds one record to the index is in two parts, a "sync" path
  and an "async" path. The "sync" path in process_gtid_check_batch() does the
  minimum amount of work which needs to run as part of transaction commit. The
  actual writing of the index, in async_update(), can then be done as a
  background task, minimizing the performance impact on transaction processing.
  The "sync" and "async" paths each run single threaded, but can execute in
  parallel with each other.

  The index file is written incrementally together with the binlog file.
  However there is no fsync()'s of the index file needed while writing. A
  partially written index left by a crashing server will be re-written during
  binlog recovery. A reader is allowed to use the index as it is begin written
  (for the "hot" binlog file); such access is protected by mutex.

  In case of lost or corrupt index, fallback to full sequential scan is done
  (so performance will be affected but not correct functionality).

  The index file is structured like a B+-tree. The index is append-only, so
  also resembles a log-structured merge-tree, but with no merging of levels
  needed as it covers a single fixed-size binlog file. This makes the building
  of the tree relatively simple.

  Keys in the tree consist of a GTID state (corresponding to a GTID_LIST
  event) and the associated binlog file offset. All keys (except the first key
  in each level of the tree) are delta-compressed to save space, holding only
  the (domain_id, server_id) pairs that differ from the previous record.

  The file is page-based. The first page contains the leftmost leaf node, and
  the root node is at the end of the file. An incompletely written index file
  can be detected by the last page in the file not being a root node page.
  Nodes in the B+-tree usually fit in one page, but a node can be split across
  multiple pages if GTID states are very large.

  Page format:

  The first page contains an extra file header:

    Offset  Size  Description
       0      4   MAGIC header identifying the file as a binlog index
       4      1   Major version number. A new major version of the file format
                  is not readable by older server versions.
       5      1   Minor version number. Formats differing only in minor version
                  are backwards compatible and can be read by older servers.
       6      2   Padding/unused.
       8      4   Page size.

  Each page additionally contains this header:

    Offset  Size  Description
       0      1   Flags
       1      3   Padding/unused

  The last 4 bytes of each page is a 32-bit CRC.

  An interior node is a sequence of
    <child ptr> <key> <child ptr> <key> ... <key> <child ptr>
  while a leaf node has only keys.

  A child pointer is stored as 4 byte integer. The first page is 1, so that
  0 can be used to denote "not present".

  Format of a key:

    Offset  Size  Description
      0       4   Number of GTIDs in the key, plus 1. Or 0 for EOF.
      4       4   Binlog file offset
      8       4   Domain_id of first GTID
     12       4   Server_id of first GTID
     16       8   Seq_no of first GTID
     ...          and so on for each GTID in the key.

  A node typically fits in one page. But if the GTID state is very big (or
  the page size very small), multiple pages may be used. When a node is split,
  it can be split after a child pointer or before or after a GTID, but not
  elsewhere.

Here is an example GTID index with page_size=64 containing 3 records:
  Offset  GTID state
  0x11d   [empty]
  0x20e   [0-1-1]
  0x2ad   [0-1-2]
The example contains 3 nodes, each stored in a single page. Two leaf nodes and
one interior root node.

Page 1 (leaf node page with file header):
  fe fe 0c 01              "magic" identifying the file as a binlog GTID index
  01 00                    Major version 1, minor version 0
  00 00                    Padding / currently unused
  40 00 00 00              Page size (64 bytes in this example)
  05                       Flag PAGE_FLAG_IS_LEAF | PAGE_FLAG_LAST (single-page leaf node)
  00 00 00                 Padding / current unused
Key 1:
  01 00 00 00              <GTID_count + 1> = 1 (entry has zero GTIDs in it)
  1d 01 00 00              Binlog file offset = 0x11d
                           [Empty GTID state at the very start of the binlog]
Key 2:
  02 00 00 00              GTID_count = 1
  0e 02 00 00              Binlog file offset = 0x20e
  00 00 00 00 01 00 00 00
  01 00 00 00 00 00 00 00  GTID 0-1-1

  00 00 00 00 00           Zero denotes end-of-node
  00 00 00 00 00 00 00     (Unused space in the page)
  0e 4f ac 43              Checksum / CRC

Page 2 (leaf node):
  05                       Flag PAGE_FLAG_IS_LEAF | PAGE_FLAG_LAST (single-page leaf node)
  00 00 00                 Unused
Key 1:
  02 00 00 00              GTID_count = 1
  ad 02 00 00              Binlog file offset = 0x2ad
  00 00 00 00 01 00 00 00
  02 00 00 00 00 00 00 00  GTID 0-1-2

  00 00 00 00              End-of-node
  00 00 00 00 00 00 00 00
  00 00 00 00 00 00 00 00
  00 00 00 00 00 00 00 00
  00 00 00 00              (Unused space in the page)
  0c 4e c2 b9              CRC

Page 3 (root node):
  0c                       PAGE_FLAG_ROOT | PAGE_FLAG_LAST (interior root node)
  00 00 00                 Unused
Child pointer:
  01 00 00 00              Pointer to page 1
Key for next child page:
  02 00 00 00              GTID_count = 1
  ad 02 00 00              Binlog offset = 0x2ad
  00 00 00 00 01 00 00 00
  02 00 00 00 00 00 00 00  GTID 0-1-2
Child pointer:
  02 00 00 00              Pointer to page 2

  00 00 0 000              Zero denotes end-of-node
  00 00 00 00 00 00 00 00
  00 00 00 00 00 00 00 00
  00 00 00 00              (Unused)
  8155 a3c7                CRC

  Below is an example of the logical B-Tree structure of a larger GTID index
  with a total of 12 keys.

  We use S0, S1, ..., S11 to denote a key, which consists of a GTID state (as
  seen in @@binlog_gtid_state and GTID_LIST_EVENT) and the associated binlog
  file offset. D1, D2, ..., D11 denote the same keys, but delta-compressed, so
  that D1 stores only those GTIDs that are not the same as in S0.

  Pages are denoted by P1, P2, ..., P8. In the example, P1, P2, P3, P5, and P6
  are leaf pages, the rest are interior node pages. P8 is the root node (the
  root is always the last page in the index).

  The contents of each page is listed in square brackets [...]. So P1[S0 D1 D2]
  is a leaf page with 3 keys, and P7[P5 <D10+D11> P6] is an interior node page
  with one key <D10+D11> and two child-page pointers to P5 and P6. The
  notation <D10+D11> denotes the delta-compression of key S11 relative to S9;
  all GTIDs in S11 that are not present in S9. In the code, this is computed
  by combining D10 and D11, hence the use of the notation "D10+D11" instead of
  the equivalent "S11-S9".

  Here is the example B-Tree. It has 3 levels, with the leaf nodes at the top:

    P1[S0 D1 D2]   P2[D3 D4 D5]   P3[D6 D7 D8]   P5[D9 D10]   P6[D11]
          P4[P1 <S3> P2 <D4+D5+D6> P3]            P7[P5 <D10+D11> P6]
                                     P8[P4 <S9> P7]

  To find eg. S4, we start from the root P8. S4<S9, so we follow the left child
  pointer to P4. S4>S3 and S4<S6 (S6=(S3+D4+D5+D6)), so we follow the child
  pointer to leaf page P2.

  The index is written completely append-only; this is possible since keys are
  always inserted in-order, at the end of the index. One page is kept in-memory
  at each level of the B-Tree; when a new key no longer fits the page, it is
  written out to disk and a new in-memory page is allocated for it.

  Here are the operations that occur while writing the index file from the
  above example. The left column is each key added to the index as the
  corresponding GTID is written into the binlog file (<EOF> is when the index
  is closed at binlog rotation). The right column are the operations performed,
  as follows:
    alloc(p)      Allocate page p
    add_key(p,k)  Insert the key k into the page p
    add_ptr(p,q)  Insert a pointer to child page q in parent page p
    write(p)      Write out page p to disk at the end of the index file.

  GTID STATE   OPERATIONS
    S0       alloc(P1) add_key(P1,S0)
    D1         add_key(P1,D1)
    D2         add_key(P1,D2)
    D3       write(P1) alloc(P4) add_ptr(P4,P1)
             alloc(P2) add_key(P2,D3) add_key(P4,S3)
    D4         add_key(P2,D4)
    D5         add_key(P2,D5)
    D6       write(P2) add_ptr(P4,P2)
             alloc(P3) add_key(P3,D6) add_key(P4,D4+D5+D6)
    D7         add_key(P3,D7)
    D8         add_key(P3,D8)
    D9       write(P3) add_ptr(P4,P3)
             alloc(P5) add_key(P5,D9)
             write(P4) alloc(P8) add_ptr(P8,P4) alloc(P7) add_key(P8,S9)
    D10        add_key(P5,D10)
    D11      write(P5) add_ptr(P7,P5)
             alloc(P6) add_key(P6,D11) add_key(P7,D10+D11)
    <EOF>    write(P6) add_ptr(P7,P6)
             write(P7) add_ptr(P8,P7)
             write(P8)

  After adding each record to the index, there is exactly one partial page
  allocated in-memory for each level present in the B-Tree; new pages being
  allocated as old pages fill up and are written to disk.
*/


class Gtid_index_base
{
public:
  /* +4 for ".idx" prefix. */
  static constexpr size_t GTID_INDEX_FILENAME_MAX_SIZE= FN_REFLEN+4;

protected:
  enum enum_page_flags {
    /* Set for a leaf node page, cleared for an interior node page. */
    PAGE_FLAG_IS_LEAF= 1,
    /* This is a continuation page. */
    PAGE_FLAG_IS_CONT= 2,
    /* No continuation page follows (the last page in a group). */
    PAGE_FLAG_LAST= 4,
    /*
      Flag set to mark the root node. (The root node is normally the last page
      in the index file, but having an explicit flag allows us to detect a
      partially written index file with the root node missing.
    */
    PAGE_FLAG_ROOT= 8,
  };

  /*
    Minor version increment represents a backwards-compatible format (can be
    read by any server version that knows the format of the major version).
    Major version increment means a server should not attempt to read from the
    index.
  */
  static constexpr uchar GTID_INDEX_VERSION_MAJOR= 1;
  static constexpr uchar GTID_INDEX_VERSION_MINOR= 0;
  static constexpr size_t GTID_INDEX_FILE_HEADER_SIZE= 12;
  static constexpr size_t GTID_INDEX_PAGE_HEADER_SIZE= 4;
  static constexpr size_t CHECKSUM_LEN= 4;

#ifdef _MSC_VER
/*
  Flexible array member is part of C99, but it is not standard in C++.
  All the compilers and platforms we support do support it, though.
  Just we need to disable on Windows a warning about using a non-standard
  C++ extension.
*/
#pragma warning(disable : 4200)
#endif
  struct Node_page
  {
    Node_page *next;
    /* Pointer to allow to update the "flags" byte at page writeout. */
    uchar *flag_ptr;
    /* Flexible array member; will be allocated to opt_gtid_index_page_size. */
    uchar page[];
  };

  struct Index_node_base
  {
    Node_page *first_page;
    Node_page *current_page;
    /* The current_ptr is only valid if current_page != 0. */
    uchar *current_ptr;

    Index_node_base();
    ~Index_node_base();
    void free_pages();
    void reset();
  };

public:
  static void make_gtid_index_file_name(char *out_name, size_t bufsize,
                                        const char *base_filename);

protected:
  int update_gtid_state(rpl_binlog_state_base *state,
                        const rpl_gtid *gtid_list, uint32 gtid_count);
  Node_page *alloc_page();
  rpl_gtid *gtid_list_buffer(uint32 count);
  void build_index_filename(const char *filename);
  virtual int give_error(const char *msg) = 0;

  /*
    A buffer to hold a gtid_list temporarily.
    Increased as needed to hold largest needed list.
  */
  rpl_gtid *gtid_buffer;
  uint32 gtid_buffer_alloc;
  size_t page_size;
public:
  char index_file_name[GTID_INDEX_FILENAME_MAX_SIZE];

protected:
  Gtid_index_base();
  virtual ~Gtid_index_base();
};


class Gtid_index_writer : public Gtid_index_base
{
private:
  const my_off_t offset_min_threshold;

  struct Index_node : public Index_node_base
  {
    rpl_binlog_state_base state;
    uint32 num_records;
    uint32 level;
    bool force_spill_page;

    Index_node(uint32 level_);
    ~Index_node();
    void reset();
  };

public:
  static void gtid_index_init();
  static void gtid_index_cleanup();
protected:
  friend class Gtid_index_reader_hot;
  static void lock_gtid_index() { mysql_mutex_lock(&gtid_index_mutex); }
  static void unlock_gtid_index() { mysql_mutex_unlock(&gtid_index_mutex); }
  static const Gtid_index_writer *find_hot_index(const char *file_name);

public:
  Gtid_index_writer(const char *filename, uint32 offset,
                    rpl_binlog_state_base *binlog_state,
                    uint32 opt_page_size, my_off_t opt_span_min);
  virtual ~Gtid_index_writer();
  void process_gtid(uint32 offset, const rpl_gtid *gtid);
  int process_gtid_check_batch(uint32 offset, const rpl_gtid *gtid,
                               rpl_gtid **out_gtid_list,
                               uint32 *out_gtid_count);
  int async_update(uint32 event_offset, rpl_gtid *gtid_list, uint32 gtid_count);
  void close();

private:
  void insert_in_hot_index();
  void remove_from_hot_index();
  uint32 write_current_node(uint32 level, bool is_root);
  int reserve_space(Index_node *n, size_t bytes);
  int do_write_record(uint32 level, uint32 event_offset,
                      const rpl_gtid *gtid_list, uint32 gtid_count);
  int add_child_ptr(uint32 level, my_off_t node_offset);
  int write_record(uint32 event_offset, const rpl_gtid *gtid_list,
                   uint32 gtid_count);
  bool check_room(uint32 level, uint32 gtid_count);
  int alloc_level_if_missing(uint32 level);
  uchar *init_header(Node_page *page, bool is_leaf, bool is_first);
  int give_error(const char *msg) override;

  static mysql_mutex_t gtid_index_mutex;
  static Gtid_index_writer *hot_index_list;

  rpl_binlog_state_base pending_state;
  /* Next pointer for the hot_index_list linked list. */
  Gtid_index_writer *next_hot_index;
  /* The currently being built index nodes, from leaf[0] to root[max_level]. */
  Index_node **nodes;
  my_off_t previous_offset;
  uint32 max_level;

  File index_file;

  /*
    This is set if we encounter an error (such as out-of-memory or I/O error).
    Then we will no longer do any updates to the index, to prevent leaving a
    corrupt index. This is not fatal; the partial index will work up to where
    it got the error, and the code can fall-back to sequential scan of the
    binlog.
  */
  bool error_state;
  /* Flag to help put the file header at the start of the very first page. */
  bool file_header_written;
  /* Flag set while this object is visible in the "hot index" list. */
  bool in_hot_index_list;
};


class Gtid_index_reader : public Gtid_index_base
{
public:
  Gtid_index_reader();
  virtual ~Gtid_index_reader();

  int open_index_file(const char *binlog_filename);
  void close_index_file();
  /*
    The search functions take either a binlog offset or GTID position to search
    for. They return:
      0   for "not found" (searched position is earlier than start of index).
      1   for "found"
     -1   for error.
    When found, the returned position is the last position in the index that
    lies at or before the searched position. The offset of the returned
    position is written to *out_offset. The number of GTIDs in the returned
    GTID state is written to *out_gtid_count; the list of found GTIDs can be
    accessed with search_gtid_list() and is valid only until next search or
    freeing of the Gtid_index_reader object.
  */
  int search_offset(uint32 in_offset, uint32 *out_offset,
                    uint32 *out_gtid_count);
  int search_gtid_pos(slave_connection_state *in_gtid_pos, uint32 *out_offset,
                      uint32 *out_gtid_count);
  rpl_gtid *search_gtid_list();

protected:
  int search_cmp_offset(uint32 offset, rpl_binlog_state_base *state);
  int search_cmp_gtid_pos(uint32 offset, rpl_binlog_state_base *state);
  virtual int do_index_search(uint32 *out_offset, uint32 *out_gtid_count);
  int do_index_search_root(uint32 *out_offset, uint32 *out_gtid_count);
  int do_index_search_leaf(bool current_state_updated,
                           uint32 *out_offset, uint32 *out_gtid_count);
  int next_page();
  int find_bytes(uint32 num_bytes);
  virtual int get_child_ptr(uint32 *out_child_ptr);
  int get_offset_count(uint32 *out_offset, uint32 *out_gtid_count);
  int get_gtid_list(rpl_gtid *out_gtid_list, uint32 count);
  virtual int read_file_header();
  int verify_checksum(Node_page *page);
  Node_page *alloc_and_read_page();
  virtual int read_root_node();
  virtual int read_node(uint32 page_ptr);
  int read_node_cold(uint32 page_ptr);
  int give_error(const char *msg) override;

  rpl_binlog_state_base current_state;
  rpl_binlog_state_base compare_state;
  Index_node_base cold_node;
  /* n points to either cold node or hot node in writer. */
  Index_node_base *n;
  int (Gtid_index_reader::* search_cmp_function)(uint32, rpl_binlog_state_base *);
  slave_connection_state *in_search_gtid_pos;
  Node_page *read_page;
  uchar *read_ptr;
  File index_file;
  uint32 current_offset;
  uint32 in_search_offset;
  bool file_open;
  bool index_valid;
  bool has_root_node;
  uchar version_major;
  uchar version_minor;
};


/*
   Sub-class of Gtid_index_reader that can additionally access in-memory "hot"
   pages of the index, which are partially filled pages of the current binlog
   file, not yet written to disk.
*/
class Gtid_index_reader_hot : public Gtid_index_reader
{
public:
  Gtid_index_reader_hot();
  virtual ~Gtid_index_reader_hot() { }

private:
  int do_index_search(uint32 *out_offset, uint32 *out_gtid_count) override;
  int get_child_ptr(uint32 *out_child_ptr) override;
  int read_file_header() override;
  int read_root_node() override;
  int read_node(uint32 page_ptr) override;
  int read_node_hot();

  /* Pointer to the writer object, if we're reading a hot index. */
  const Gtid_index_writer *hot_writer;
  /* The level we are currently reading in the hot writer .*/
  uint32 hot_level;
};

#endif  /* GTID_INDEX_H */
