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

  The code has a performance-critical "sync" path which is called while holding
  LOCK_log whenever a new GTID is added to a binlog file. And a less critical
  "async" path which runs in the binlog background thread and does most of the
  processing. The "sync" and "async" paths each run single threaded, but can
  execute in parallel with each other.

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
       0      8   MAGIC header identifying the file as a binlog index
       8      1   Major version number. A new major version of the file format
                  is not readable by older server versions.
       9      1   Minor version number. Formats differing only in minor version
                  are backwards compatible and can be read by older servers.
      10      2   Padding/unused.
      12      4   Page size.

  Each page additionally contains this header:

    Offset  Size  Description
       0      1   Flags
       1      7   Padding/unused

  Here is an example index file in schematic form:

       S0 D1 D2    D3 D4 D5    D6 D7 D8    D9 D10 D11
    A(S0 D1 D2) B(D3 D4 D5) C(D6 D7 D8) E(D9 D10) F(D11)
        D(A <S3> B <D4+D5+D6> C)   G(E <D10+D11> F)
                        H(D <S9> G)

  S0 is the full initial GTID state at the start of the file.
  D1-D11 are the differential GTID states in the binlog file; eg. they could
      be the individual GTIDs in the binlog file if a record is writte for
      each GTID.
  S3 is the full GTID state corresponding to D3, ie. S3=S0+D1+D2+D3.
  A(), B(), ..., H() are the nodes in the binlog index. H is the root.
  A(S0 D1 D2) is a leaf node containing records S0, D1, and D2.
  G(E <D10+D11> F) is an interior node with key <D10+D11> and child pointers to
      E and F.

  To find eg. S4, we start from the root H. S4<S9, so we follow the left child
  pointer to D. S4>S3, so we follow the child pointer to leaf node C.

  Here are the operations that occur while writing the example index file:

    S0  A(A) R(A,S0)
    D1       R(A,D1)
    D2       R(A,D2)
    D3  W(A) I(D) P(D,A) A(B) R(B,D3) R(D,S3)
    D4       R(A,D4)
    D5       R(A,D5)
    D6  W(B) P(D,B) A(C) R(C,D6) R(D,D4+D5+D6)
    D7       R(C,D7)
    D8       R(C,D8)
    D9  W(C) P(D,C) A(E) R(E,D9) W(D) I(H) P(H,D) R(H,S9)
    D10      R(E,D10)
    D11 W(E) I(G) P(G,E) A(F) R(F,S10) R(G,D10+D11)
    <EOF> W(F) P(G,F) W(G) P(H,G) W(H)

    A(x)   -> allocate leaf node x.
    R(x,k) -> insert an index record containing key k in node x.
    W(x)   -> write node x to the index file.
    I(y)   -> allocate interior node y.
    P(y,x) -> insert a child pointer to y in x.
*/


class Gtid_index_base
{
public:
  enum enum_page_flags {
    /* Cleared for a leaf node page, set for an interior node page. */
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
  static constexpr size_t GTID_INDEX_FILE_HEADER_SIZE= 16;
  static constexpr size_t GTID_INDEX_PAGE_HEADER_SIZE= 8;

  struct Node_page
  {
    Node_page *next;
    /* Pointer to allow to update the "flags" byte at page writeout. */
    uchar *flag_ptr;
    /* Will be allocated to size opt_gtid_index_page_size. */
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

  int update_gtid_state(rpl_binlog_state *state,
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
  char index_file_name[FN_REFLEN+4];  // +4 for ".idx" prefix

protected:
  Gtid_index_base();
  virtual ~Gtid_index_base();
};


class Gtid_index_writer : public Gtid_index_base
{
public:
  /* ToDo: configurable. */
  static constexpr uint32 gtid_threshold= 1; // ToDo 10;
  static constexpr my_off_t offset_min_threshold= 1; // ToDo 4096;
  static constexpr my_off_t offset_max_threshold= 65536;

  struct Index_node : public Index_node_base
  {
    rpl_binlog_state state;
    uint32 num_records;
    uint32 level;
    bool force_spill_page;

    Index_node(uint32 level_);
    ~Index_node();
    void reset();
  };

  static void gtid_index_init();
  static void gtid_index_cleanup();
  static void lock_gtid_index() { mysql_mutex_lock(&gtid_index_mutex); }
  static void unlock_gtid_index() { mysql_mutex_unlock(&gtid_index_mutex); }
  static const Gtid_index_writer *find_hot_index(const char *file_name);

  Gtid_index_writer(const char *filename, my_off_t offset,
                    rpl_binlog_state *binlog_state);
  virtual ~Gtid_index_writer();
  void insert_in_hot_index();
  void remove_from_hot_index();
  void process_gtid(my_off_t offset, const rpl_gtid *gtid);
  void close();
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

  rpl_binlog_state pending_state;
  /* Next pointer for the hot_index_list linked list. */
  Gtid_index_writer *next_hot_index;
  /* The currently being built index nodes, from leaf[0] to root[max_level]. */
  Index_node **nodes;
  my_off_t previous_offset;
  uint32 max_level;
  uint32 pending_gtid_count;

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

  int search_cmp_offset(uint32 offset, rpl_binlog_state *state);
  int search_cmp_gtid_pos(uint32 offset, rpl_binlog_state *state);
  int do_index_search(uint32 *out_offset, uint32 *out_gtid_count);
  int do_index_search_root(uint32 *out_offset, uint32 *out_gtid_count);
  int do_index_search_leaf(bool current_state_updated,
                           uint32 *out_offset, uint32 *out_gtid_count);
  int next_page();
  int find_bytes(uint32 num_bytes);
  int get_child_ptr(uint32 *out_child_ptr);
  int get_offset_count(uint32 *out_offset, uint32 *out_gtid_count);
  int get_gtid_list(rpl_gtid *out_gtid_list, uint32 count);
  int open_index_file(const char *binlog_filename);
  void close_index_file();
  int read_file_header();
  int read_root_node();
  int read_root_node_hot();
  int read_root_node_cold();
  int read_node(uint32 page_ptr);
  int read_node_hot();
  int read_node_cold(uint32 page_ptr);
  int give_error(const char *msg) override;

  rpl_binlog_state current_state;
  rpl_binlog_state compare_state;
  Index_node_base cold_node;
  /* n points to either cold node or hot node in writer. */
  Index_node_base *n;
  /* Pointer to the writer object, if we're reading a hot index. */
  const Gtid_index_writer *hot_writer;
  int (Gtid_index_reader::* search_cmp_function)(uint32, rpl_binlog_state *);
  slave_connection_state *in_search_gtid_pos;
  Node_page *read_page;
  uchar *read_ptr;
  File index_file;
  uint32 current_offset;
  uint32 in_search_offset;
  /* The level we are currently reading in the hot writer .*/
  uint32 hot_level;
  bool file_open;
  bool index_valid;
  bool has_root_node;
  uchar version_major;
  uchar version_minor;
};

#endif  /* GTID_INDEX_H */
