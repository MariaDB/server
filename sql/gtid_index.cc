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

#include "gtid_index.h"
#include "sql_const.h"
#include "log.h"


static const uchar GTID_INDEX_MAGIC[4]= {
  254, 254, 12, 1,
};

Gtid_index_writer *Gtid_index_writer::hot_index_list= nullptr;
/* gtid_index_mutex is inited in MYSQL_LOG::init_pthread_objects(). */
mysql_mutex_t Gtid_index_writer::gtid_index_mutex;


Gtid_index_writer::Gtid_index_writer(const char *filename, uint32 offset,
                                     rpl_binlog_state_base *binlog_state,
                                     uint32 opt_page_size,
                                     my_off_t opt_span_min)
  : offset_min_threshold(opt_span_min),
    nodes(nullptr), previous_offset(0),
    max_level(0), index_file(-1),
    error_state(false), file_header_written(false), in_hot_index_list(false)
{
  uint32 count;
  rpl_gtid *gtid_list;
  page_size= opt_page_size;
  pending_state.init();

  if (alloc_level_if_missing(0))
  {
    give_error("Out of memory allocating node list");
    return;
  }

  /*
    Lock the index mutex at this point just before we create the new index
    file on disk. From this point on, and until the index is fully written,
    the reader will find us in the "hot index" list and will be able to read
    from the index while it's still being constructed.
  */
  lock_gtid_index();

  build_index_filename(filename);
  int create_flags= O_WRONLY|O_TRUNC|O_BINARY|O_EXCL;
  index_file= mysql_file_create(key_file_gtid_index, index_file_name,
                                CREATE_MODE, create_flags, MYF(0));
  if (index_file < 0 && my_errno == EEXIST)
  {
    /*
      It shouldn't happen that an old GTID index file remains, as we remove
      them as part of RESET MASTER and PURGE BINARY LOGS. But if it happens
      due to some external file copy of the user or something, delete any old
      GTID index file first.
    */
    sql_print_information("Old GTID index file found '%s', deleting",
                          index_file_name);
    my_errno= 0;
    mysql_file_delete(key_file_gtid_index, index_file_name, MYF(0));
    index_file= mysql_file_create(key_file_gtid_index, index_file_name,
                                  CREATE_MODE, create_flags, MYF(0));
  }
  if (index_file < 0)
  {
    give_error("Failed to open new index file for writing");
    goto err;
  }

  /*
    Write out an initial index record, i.e. corresponding to the GTID_LIST
    event / binlog state at the start of the binlog file.
  */
  count= binlog_state->count_nolock();
  gtid_list= gtid_list_buffer(count);
  if (count > 0)
  {
    if (!gtid_list)
      goto err;
    binlog_state->get_gtid_list_nolock(gtid_list, count);
  }
  write_record(offset, gtid_list, count);

  insert_in_hot_index();

err:
  unlock_gtid_index();
}


Gtid_index_writer::~Gtid_index_writer()
{
  if (in_hot_index_list)
  {
    lock_gtid_index();
    close();
    unlock_gtid_index();
  }

  if (index_file > 0)
  {
    /*
      Should have been closed by call to Gtid_index_writer::close().
      We can at least avoid leaking file descriptor.
    */
    mysql_file_close(index_file, MYF(0));
  }

  if (nodes)
  {
    for (uint32 i= 0; i <= max_level; ++i)
      delete nodes[i];
    my_free(nodes);
  }

  /*
    state.free() is not needed here, will be called from rpl_binlog_state_base
    destructor.
  */
}


void
Gtid_index_writer::gtid_index_init()
{
  mysql_mutex_init(key_gtid_index_lock, &gtid_index_mutex, MY_MUTEX_INIT_SLOW);
}

void
Gtid_index_writer::gtid_index_cleanup()
{
  mysql_mutex_destroy(&gtid_index_mutex);
}


const Gtid_index_writer *
Gtid_index_writer::find_hot_index(const char *file_name)
{
  mysql_mutex_assert_owner(&gtid_index_mutex);

  for (const Gtid_index_writer *p= hot_index_list; p; p= p->next_hot_index)
  {
    if (0 == strcmp(file_name, p->index_file_name))
      return p;
  }
  return nullptr;
}

void
Gtid_index_writer::insert_in_hot_index()
{
  mysql_mutex_assert_owner(&gtid_index_mutex);

  next_hot_index= hot_index_list;
  hot_index_list= this;
  in_hot_index_list= true;
}


void
Gtid_index_writer::remove_from_hot_index()
{
  mysql_mutex_assert_owner(&gtid_index_mutex);

  Gtid_index_writer **next_ptr_ptr= &hot_index_list;
  for (;;)
  {
    Gtid_index_writer *p= *next_ptr_ptr;
    if (!p)
      break;
    if (p == this)
    {
      *next_ptr_ptr= p->next_hot_index;
      break;
    }
    next_ptr_ptr= &p->next_hot_index;
  }
  next_hot_index= nullptr;
  in_hot_index_list= false;
}

void
Gtid_index_writer::process_gtid(uint32 offset, const rpl_gtid *gtid)
{
  rpl_gtid *gtid_list;
  uint32 gtid_count;

  if (process_gtid_check_batch(offset, gtid, &gtid_list, &gtid_count))
    return;    // Error

  if (gtid_list)
    async_update(offset, gtid_list, gtid_count);
}


int
Gtid_index_writer::process_gtid_check_batch(uint32 offset, const rpl_gtid *gtid,
                                            rpl_gtid **out_gtid_list,
                                            uint32 *out_gtid_count)
{
  uint32 count;
  rpl_gtid *gtid_list;

  mysql_mutex_assert_not_owner(&gtid_index_mutex);

  if (unlikely(pending_state.update_nolock(gtid)))
  {
    give_error("Out of memory processing GTID for binlog GTID index");
    return 1;
  }
  /*
    Sparse index; we record only selected GTIDs, and scan the binlog forward
    from there to find the exact spot.
  */
  if (offset - previous_offset < offset_min_threshold)
  {
    *out_gtid_list= nullptr;
    *out_gtid_count= 0;
    return 0;
  }

  count= pending_state.count_nolock();
  DBUG_ASSERT(count > 0 /* Since we just updated with a GTID. */);
  gtid_list= (rpl_gtid *)
    my_malloc(key_memory_binlog_gtid_index, count*sizeof(*gtid_list), MYF(0));
  if (unlikely(!gtid_list))
  {
    give_error("Out of memory allocating GTID list for binlog GTID index");
    return 1;
  }
  if (unlikely(pending_state.get_gtid_list_nolock(gtid_list, count)))
  {
    /* Shouldn't happen as we allocated the list with the correct length. */
    DBUG_ASSERT(false);
    give_error("Internal error allocating GTID list for binlog GTID index");
    my_free(gtid_list);
    return 1;
  }
  pending_state.reset_nolock();
  previous_offset= offset;
  *out_gtid_list= gtid_list;
  *out_gtid_count= count;
  return 0;
}


int
Gtid_index_writer::async_update(uint32 event_offset,
                                rpl_gtid *gtid_list,
                                uint32 gtid_count)
{
  lock_gtid_index();
  int res= write_record(event_offset, gtid_list, gtid_count);
  unlock_gtid_index();
  my_free(gtid_list);
  return res;
}


void
Gtid_index_writer::close()
{
  lock_gtid_index();
  if (!error_state)
  {

    /*
      Write out the remaining pending pages, and insert the final child pointer
      in interior nodes.
    */
    for (uint32 level= 0; ; ++level)
    {
      uint32 node_ptr= write_current_node(level, level==max_level);
      nodes[level]->reset();
      if (!node_ptr || level >= max_level)
        break;
      add_child_ptr(level+1, node_ptr);
    }
  }
  remove_from_hot_index();
  unlock_gtid_index();

  if (!error_state)
  {
    if (mysql_file_sync(index_file, MYF(0)))
      give_error("Error syncing index file to disk");
  }

  mysql_file_close(index_file, MYF(0));
  index_file= (File)-1;
}


Gtid_index_base::Index_node_base::Index_node_base()
  : first_page(nullptr), current_page(nullptr), current_ptr(nullptr)
{
}


Gtid_index_base::Index_node_base::~Index_node_base()
{
  free_pages();
}


void
Gtid_index_base::Index_node_base::free_pages()
{
  for (Node_page *p= first_page; p; )
  {
    Node_page *q= p->next;
    my_free(p);
    p= q;
  }
}


void
Gtid_index_base::Index_node_base::reset()
{
  free_pages();
  first_page= current_page= nullptr;
}


Gtid_index_base::Gtid_index_base()
  : gtid_buffer(nullptr), gtid_buffer_alloc(0)
{
}


Gtid_index_base::~Gtid_index_base()
{
  if (gtid_buffer_alloc > 0)
    my_free(gtid_buffer);
}


void
Gtid_index_base::make_gtid_index_file_name(char *out_name, size_t bufsize,
                                           const char *base_filename)
{
  char *p= strmake(out_name, base_filename, bufsize-1);
  size_t remain= bufsize - (p - out_name);
  strmake(p, ".idx", remain-1);
}


void
Gtid_index_base::build_index_filename(const char *filename)
{
  make_gtid_index_file_name(index_file_name, sizeof(index_file_name), filename);
}


rpl_gtid *
Gtid_index_base::gtid_list_buffer(uint32 count)
{
  if (gtid_buffer_alloc >= count)
    return gtid_buffer;
  rpl_gtid *new_buffer= (rpl_gtid *)
    my_malloc(key_memory_binlog_gtid_index, count*sizeof(*new_buffer), MYF(0));
  if (!new_buffer)
  {
    give_error("Out of memory allocating buffer for GTID list");
    return NULL;
  }
  my_free(gtid_buffer);
  gtid_buffer= new_buffer;
  gtid_buffer_alloc= count;
  return new_buffer;
}


Gtid_index_writer::Index_node::Index_node(uint32 level_)
  : num_records(0), level(level_), force_spill_page(false)
{
  state.init();
}


Gtid_index_writer::Index_node::~Index_node()
{
  free_pages();
}


uint32
Gtid_index_writer::write_current_node(uint32 level, bool is_root)
{
  Index_node *n= nodes[level];

  uint32 node_pos= (uint32)mysql_file_tell(index_file, MYF(0));

  for (Node_page *p= n->first_page; p ; p= p->next)
  {
    if (unlikely(is_root))
      *(p->flag_ptr) |= PAGE_FLAG_ROOT;
    if (likely(!p->next))
      *(p->flag_ptr) |= PAGE_FLAG_LAST;
    int4store(p->page + page_size - CHECKSUM_LEN,
              my_checksum(0, p->page, page_size - CHECKSUM_LEN));
    if (mysql_file_write(index_file, p->page, page_size, MYF(MY_NABP)))
    {
      give_error("Error writing index page");
      return 0;
    }
  }

  DBUG_ASSERT(node_pos % page_size == 0);
  /* Page numbers are +1 just so that zero can denote invalid page pointer. */
  return 1 + (node_pos / (uint32)page_size);
}


void
Gtid_index_writer::Index_node::reset()
{
  Index_node_base::reset();
  state.reset_nolock();
  num_records= 0;
  force_spill_page= false;
}


/*
  Make sure there is requested space in the current page, by allocating a
  new spill page if necessary.
*/
int
Gtid_index_writer::reserve_space(Index_node *n, size_t bytes)
{
  DBUG_ASSERT(bytes <= page_size);
  if (likely(n->current_page) &&
      likely(n->current_ptr - n->current_page->page + bytes <=
             (page_size - CHECKSUM_LEN)))
    return 0;
  /* Not enough room, allocate a spill page. */
  Node_page *page= alloc_page();
  n->force_spill_page= false;
  if (!page)
    return 1;
  n->current_ptr=
    init_header(page, n->level==0, !n->current_page);
  if (n->current_page)
    n->current_page->next= page;
  else
    n->first_page= page;
  n->current_page= page;
  return 0;
}


int
Gtid_index_writer::do_write_record(uint32 level,
                                   uint32 event_offset,
                                   const rpl_gtid *gtid_list,
                                   uint32 gtid_count)
{
  DBUG_ASSERT(level <= max_level);
  Index_node *n= nodes[level];
  if (reserve_space(n, 8))
    return 1;
  /* Store the count as +1, so that 0 can mean "no more records". */
  int4store(n->current_ptr, gtid_count+1);
  int4store(n->current_ptr+4, event_offset);
  n->current_ptr+= 8;
  for (uint32 i= 0; i < gtid_count; ++i)
  {
    if (reserve_space(n, 16))
      return 1;
    int4store(n->current_ptr, gtid_list[i].domain_id);
    int4store(n->current_ptr+4, gtid_list[i].server_id);
    int8store(n->current_ptr+8, gtid_list[i].seq_no);
    n->current_ptr+= 16;
  }

  ++n->num_records;
  return 0;
}


/*
  Add a child pointer to the current node on LEVEL.
  The first page has node_ptr=1 just so that a zero node_ptr can be used as
  a no/invalid value (effectively node_ptr points to the end of the target
  page, in unit of pages).

  Adding a child pointer shouldn't spill to a new page, code must make sure that
  there is always room for the final child pointer in current non-leaf node.
*/
int
Gtid_index_writer::add_child_ptr(uint32 level, my_off_t node_offset)
{
  DBUG_ASSERT(level <= max_level);
  DBUG_ASSERT(node_offset > 0);
  Index_node *n= nodes[level];
  if (reserve_space(n, 4))
    return 1;
  DBUG_ASSERT(n->current_page);
  DBUG_ASSERT((size_t)(n->current_ptr - n->current_page->page + 4) <=
              page_size - CHECKSUM_LEN);

  int4store(n->current_ptr, node_offset);
  n->current_ptr+= 4;
  return 0;
}


/*
  Write one index record to the GTID index, flushing nodes and allocating
  new nodes as necessary.
*/
int
Gtid_index_writer::write_record(uint32 event_offset,
                                const rpl_gtid *gtid_list,
                                uint32 gtid_count)
{
  if (error_state)
    return 1;  /* Avoid continuing on a possibly corrupt state. */

  uint32 level= 0;
  /*
    The most frequent case is when there is room in the current page for the
    current position to be written, in which case we exit early in the first
    iteration of the following loop.

    In the general case, we move up through the path to the root, writing
    lower-level node page to disk and adding child pointers in higher-level
    nodes, until we reach a node that has room. This final node may be a
    freshly allocated new root node in the few times when the height of the
    tree increases.
  */
  for (;;)
  {
    Index_node *n= nodes[level];
    if (update_gtid_state(&n->state, gtid_list, gtid_count))
      return give_error("Out of memory updating the local GTID state");

    if (check_room(level, gtid_count))
    {
      /* There is room in the node, just add the index record. */
      return do_write_record(level, event_offset, gtid_list, gtid_count);
    }

    /*
      This node is full:
       - First, write out this node to disk.
       - Add a child pointer in the parent node (allocating one if needed).
       - On level 0, allocate a new leaf node and add the index record there.
       - On levels >0, skip the last index record when the node gets full
         (B+-Tree has (k-1) keys for k child pointers).
       - Loop to the parent node to add an index record there.
    */
    uint32 node_ptr= write_current_node(level, false);
    if (!node_ptr)
      return 1;
    if (alloc_level_if_missing(level+1) ||
        add_child_ptr(level+1, node_ptr))
      return 1;
    uint32 new_count= n->state.count_nolock();
    rpl_gtid *new_gtid_list= gtid_list_buffer(new_count);
    if (new_count > 0 && !new_gtid_list)
      return 1;
    if (n->state.get_gtid_list_nolock(new_gtid_list, new_count))
      return give_error("Internal error processing GTID state");
    n->reset();
    if (level == 0)
    {
      if (do_write_record(level, event_offset, new_gtid_list, new_count))
        return 1;
    }
    else
    {
      /*
        Allocate a page for the node. This is mostly to help the reader of hot
        index to not see NULL pointers, and we will need the page later anyway
        to put at least one child pointer to the level below.
      */
      if (reserve_space(n, 4))
        return 1;
    }
    gtid_list= new_gtid_list;
    gtid_count= new_count;
    ++level;
  }
  // NotReached.
}


bool
Gtid_index_writer::check_room(uint32 level, uint32 gtid_count)
{
  Index_node *n= nodes[level];
  /* There's always room in an empty (to-be-allocated) page. */
  if (!n->current_page || n->num_records == 0)
    return true;
  /*
    Make sure we use at least 1/2 a page of room after the initial record,
    setting a flag to allocate a spill page later if needed.
  */
  size_t avail= page_size - CHECKSUM_LEN - (n->current_ptr - n->current_page->page);
  if (n->num_records==1 && avail < page_size/2)
  {
    n->force_spill_page= true;
    return true;
  }
  if (n->force_spill_page)
    return true;
  size_t needed= 8 + 16*gtid_count;
  /* Non-leaf pages need extra 4 bytes for a child pointer. */
  if (level > 0)
    needed+= 4;
  return needed <= avail;
}


int
Gtid_index_writer::alloc_level_if_missing(uint32 level)
{
  if (likely(nodes))
  {
    if (likely(max_level >= level))
      return 0;
    DBUG_ASSERT(level == max_level+1);  // Alloc one at a time
  }

  Index_node *node= new Index_node(level);
  if (!node)
    return give_error("Out of memory allocating new node");
  Index_node **new_nodes= (Index_node **)
    my_realloc(key_memory_binlog_gtid_index, nodes, (level+1)*sizeof(*nodes),
               MYF(MY_ALLOW_ZERO_PTR|MY_ZEROFILL));
  if (!new_nodes)
  {
    delete node;
    return give_error("Out of memory allocating larger node list");
  }
  new_nodes[level]= node;
  nodes= new_nodes;
  max_level= level;
  return 0;
}


/*
  Initialize the start of a data page.
  This is at the start of a page, except for the very first page where it
  comes after the global file header.
  Format:
    0    flags.
    1-3  unused padding/reserved.

  The argument FIRST denotes if this is the first page (if false it is a
  continuation page).
*/
uchar *
Gtid_index_writer::init_header(Node_page *page, bool is_leaf, bool is_first)
{
  uchar *p= page->page;
  bool is_file_header= !file_header_written;

  if (unlikely(is_file_header))
  {
    memcpy(p, GTID_INDEX_MAGIC, sizeof(GTID_INDEX_MAGIC));
    p+= sizeof(GTID_INDEX_MAGIC);
    *p++= GTID_INDEX_VERSION_MAJOR;
    *p++= GTID_INDEX_VERSION_MINOR;
    /* Flags/padding currently unused. */
    *p++= 0;
    *p++= 0;
    int4store(p, page_size);
    p+= 4;
    DBUG_ASSERT(p == page->page + GTID_INDEX_FILE_HEADER_SIZE);
    file_header_written= true;
  }

  uchar flags= 0;
  if (is_leaf)
    flags|= PAGE_FLAG_IS_LEAF;
  if (unlikely(!is_first))
    flags|= PAGE_FLAG_IS_CONT;
  page->flag_ptr= p;
  *p++= flags;
  /* Padding/reserved. */
  p+= 3;
  DBUG_ASSERT(p == page->page +
              (is_file_header ? GTID_INDEX_FILE_HEADER_SIZE : 0) +
              GTID_INDEX_PAGE_HEADER_SIZE);
  DBUG_ASSERT((size_t)(p - page->page) < page_size - CHECKSUM_LEN);
  return p;
}


int
Gtid_index_base::update_gtid_state(rpl_binlog_state_base *state,
                                   const rpl_gtid *gtid_list, uint32 gtid_count)
{
  for (uint32 i= 0; i < gtid_count; ++i)
    if (state->update_nolock(&gtid_list[i]))
      return 1;
  return 0;
}


Gtid_index_base::Node_page *Gtid_index_base::alloc_page()
{
  Node_page *new_node= (Node_page *)
    my_malloc(key_memory_binlog_gtid_index,
              sizeof(Node_page) + page_size,
              MYF(MY_ZEROFILL));
  if (!new_node)
    give_error("Out of memory for allocating index page");
  return new_node;
}


int Gtid_index_writer::give_error(const char *msg)
{
  if (!error_state)
  {
    sql_print_information("Error during binlog GTID index creation, will "
                          "fallback to slower sequential binlog scan. "
                          "Error is: %s", msg);
    error_state= true;
  }
  return 1;
}


Gtid_index_reader::Gtid_index_reader()
  : n(nullptr), index_file(-1),
    file_open(false), index_valid(false), has_root_node(false),
    version_major(0), version_minor(0)
{
  current_state.init();
  compare_state.init();
}


Gtid_index_reader::~Gtid_index_reader()
{
  if (file_open)
    mysql_file_close(index_file, MYF(0));
}


int
Gtid_index_reader::search_offset(uint32 in_offset,
                                 uint32 *out_offset, uint32 *out_gtid_count)
{
  in_search_offset= in_offset;
  search_cmp_function= &Gtid_index_reader::search_cmp_offset;

  return do_index_search(out_offset, out_gtid_count);
}

int
Gtid_index_reader::search_gtid_pos(slave_connection_state *in_gtid_pos,
                                   uint32 *out_offset, uint32 *out_gtid_count)
{
  in_search_gtid_pos= in_gtid_pos;
  search_cmp_function= &Gtid_index_reader::search_cmp_gtid_pos;

  int res= do_index_search(out_offset, out_gtid_count);
  /* Let's not leave a dangling pointer to the caller's memory. */
  in_search_gtid_pos= nullptr;

  return res;
}

rpl_gtid *
Gtid_index_reader::search_gtid_list()
{
  return gtid_buffer;
}


int
Gtid_index_reader::search_cmp_offset(uint32 offset,
                                     rpl_binlog_state_base *state)
{
  if (offset <= in_search_offset)
    return 0;
  else
    return -1;
}


int
Gtid_index_reader::search_cmp_gtid_pos(uint32 offset,
                                       rpl_binlog_state_base *state)
{
  if (state->is_before_pos(in_search_gtid_pos))
    return 0;
  else
    return -1;
}


int
Gtid_index_reader::next_page()
{
  if (!read_page->next)
    return 1;
  read_page= read_page->next;
  read_ptr= read_page->flag_ptr + 4;
  return 0;
}


int
Gtid_index_reader::find_bytes(uint32 num_bytes)
{
  if ((my_ptrdiff_t)(read_ptr - read_page->page + num_bytes) <=
      (my_ptrdiff_t)(page_size - CHECKSUM_LEN))
    return 0;
  return next_page();
}


int
Gtid_index_reader::get_child_ptr(uint32 *out_child_ptr)
{
  if (find_bytes(4))
      return give_error("Corrupt index, short index node");
  *out_child_ptr= (uint32)uint4korr(read_ptr);
  read_ptr+= 4;
  return 0;
}


/*
  Read the start of an index record (count of GTIDs in the differential state
  and offset).
  Returns:
     0  ok
     1  EOF, no more data in this node
*/
int
Gtid_index_reader::get_offset_count(uint32 *out_offset, uint32 *out_gtid_count)
{
  if (find_bytes(8))
    return 1;
  uint32 gtid_count= uint4korr(read_ptr);
  if (gtid_count == 0)
  {
    /* 0 means invalid/no record (we store N+1 for N GTIDs in record). */
    return 1;
  }
  *out_gtid_count= gtid_count - 1;
  *out_offset= uint4korr(read_ptr + 4);
  read_ptr+= 8;
  return 0;
}


int
Gtid_index_reader::get_gtid_list(rpl_gtid *out_gtid_list, uint32 count)
{
  for (uint32 i= 0; i < count; ++i)
  {
    if (find_bytes(16))
      return give_error("Corrupt index, short index node");
    out_gtid_list[i].domain_id= uint4korr(read_ptr);
    out_gtid_list[i].server_id= uint4korr(read_ptr + 4);
    out_gtid_list[i].seq_no= uint8korr(read_ptr + 8);
    read_ptr+= 16;
  }
  return 0;
}


int
Gtid_index_reader::open_index_file(const char *binlog_filename)
{
  close_index_file();
  build_index_filename(binlog_filename);
  if ((index_file= mysql_file_open(key_file_gtid_index, index_file_name,
                                   O_RDONLY|O_BINARY, MYF(0))) < 0)
    return 1;    // No error for missing index (eg. upgrade)

  file_open= true;
  if (read_file_header())
    return 1;

  return 0;
}

void
Gtid_index_reader::close_index_file()
{
  if (!file_open)
    return;
  mysql_file_close(index_file, MYF(0));
  file_open= false;
  index_valid= false;
}


int
Gtid_index_reader::do_index_search(uint32 *out_offset, uint32 *out_gtid_count)
{
  /* In cold index, we require a complete index with a valid root node. */
  if (!has_root_node)
    return -1;

  return do_index_search_root(out_offset, out_gtid_count);
}


int
Gtid_index_reader::do_index_search_root(uint32 *out_offset,
                                        uint32 *out_gtid_count)
{
  current_state.reset_nolock();
  compare_state.reset_nolock();
  /*
    These states will be initialized to the full state stored at the start of
    the root node and then incrementally updated.
  */
  bool current_state_updated= false;

  if (read_root_node())
    return -1;
  for (;;)
  {
    if (*n->first_page->flag_ptr & PAGE_FLAG_IS_LEAF)
      break;

    if (compare_state.load_nolock(&current_state))
    {
      give_error("Out of memory allocating GTID list");
      return -1;
    }
    uint32 child_ptr;
    if (get_child_ptr(&child_ptr))
      return -1;

    /* Scan over the keys in the node to find the child pointer to follow */
    for (;;)
    {
      uint32 offset, gtid_count;
      int res= get_offset_count(&offset, &gtid_count);
      if (res == 1)   // EOF?
      {
        /* Follow the right-most child pointer. */
        if (read_node(child_ptr))
          return -1;
        break;
      }
      rpl_gtid *gtid_list= gtid_list_buffer(gtid_count);
      uint32 child2_ptr;
      if ((gtid_count > 0 && !gtid_list) ||
          get_gtid_list(gtid_list, gtid_count) ||
          get_child_ptr(&child2_ptr))
        return -1;
      if (update_gtid_state(&compare_state, gtid_list, gtid_count))
        return -1;
      int cmp= (this->*search_cmp_function)(offset, &compare_state);
      if (cmp < 0)
      {
        /* Follow the left child of this key. */
        if (read_node(child_ptr))
          return -1;
        break;
      }
      /* Continue to scan the next key. */
      update_gtid_state(&current_state, gtid_list, gtid_count);
      current_state_updated= true;
      current_offset= offset;
      child_ptr= child2_ptr;
    }
  }
  return do_index_search_leaf(current_state_updated,
                              out_offset, out_gtid_count);
}

int Gtid_index_reader::do_index_search_leaf(bool current_state_updated,
                                            uint32 *out_offset,
                                            uint32 *out_gtid_count)
{
  uint32 offset, gtid_count;
  int res= get_offset_count(&offset, &gtid_count);
  if (res == 1)
  {
    DBUG_ASSERT(0);
    give_error("Corrupt index; empty leaf node");
    return -1;
  }
  rpl_gtid *gtid_list= gtid_list_buffer(gtid_count);
  if ((gtid_count > 0 && !gtid_list) ||
      get_gtid_list(gtid_list, gtid_count))
    return -1;
  /*
    The first key is ignored (already included in the current state), unless
    it is the very first state in the index.
  */
  if (!current_state_updated)
    update_gtid_state(&current_state, gtid_list, gtid_count);
  current_offset= offset;
  if (compare_state.load_nolock(&current_state))
  {
    give_error("Out of memory allocating GTID state");
    return -1;
  }
  int cmp= (this->*search_cmp_function)(offset, &compare_state);
  if (cmp < 0)
    return 0;  // Search position is before start of index.

  /* Scan over the keys in the leaf node. */
  for (;;)
  {
    uint32 offset, gtid_count;
    int res= get_offset_count(&offset, &gtid_count);
    if (res == 1)  // EOF?
    {
      /* Reached end of leaf, last key is the one searched for. */
      break;
    }
    gtid_list= gtid_list_buffer(gtid_count);
    if ((gtid_count > 0 && !gtid_list) ||
        get_gtid_list(gtid_list, gtid_count))
      return -1;
    if (update_gtid_state(&compare_state, gtid_list, gtid_count))
      return -1;
    cmp= (this->*search_cmp_function)(offset, &compare_state);
    if (cmp < 0)
    {
      /* Next key is larger, so current state is the one searched for. */
      break;
    }
    update_gtid_state(&current_state, gtid_list, gtid_count);
    current_offset= offset;
  }

  *out_offset= current_offset;
  *out_gtid_count= current_state.count_nolock();
  /* Save the result in the shared gtid list buffer. */
  if ((!(gtid_list= gtid_list_buffer(*out_gtid_count)) && *out_gtid_count > 0) ||
      current_state.get_gtid_list_nolock(gtid_list, *out_gtid_count))
    return -1;

  return 1;
}


/*
  Read the file header and check that it's valid and that the format is not
  too new a version for us to be able to read it.
*/
int
Gtid_index_reader::read_file_header()
{
  if (!file_open)
    return 1;

  uchar buf[GTID_INDEX_FILE_HEADER_SIZE + GTID_INDEX_PAGE_HEADER_SIZE];

  if (MY_FILEPOS_ERROR == mysql_file_seek(index_file, 0, MY_SEEK_SET, MYF(0)) ||
      mysql_file_read(index_file, buf,
                      GTID_INDEX_FILE_HEADER_SIZE + GTID_INDEX_PAGE_HEADER_SIZE,
                      MYF(MY_NABP)))
    return give_error("Error reading page from index file");
  if (memcmp(&buf[0], GTID_INDEX_MAGIC, sizeof(GTID_INDEX_MAGIC)))
    return give_error("Corrupt index file, magic not found in header");
  version_major= buf[4];
  version_minor= buf[5];
  /* We cannot safely read a major version we don't know about. */
  if (version_major > GTID_INDEX_VERSION_MAJOR)
    return give_error("Incompatible index file, version too high");
  page_size= uint4korr(&buf[8]);

  /* Verify checksum integrity of page_size and major/minor version. */
  uint32 crc= my_checksum(0, buf, sizeof(buf));
  uchar *buf3= (uchar *)
    my_malloc(key_memory_binlog_gtid_index, page_size - sizeof(buf), MYF(0));
  if (!buf3)
    return give_error("Error allocating memory for index page");
  int res= 0;
  if (mysql_file_read(index_file, buf3, page_size - sizeof(buf), MYF(MY_NABP)))
    res= give_error("Error reading page from index file");
  else
  {
    crc= my_checksum(crc, buf3, page_size - sizeof(buf) - CHECKSUM_LEN);
    if (crc != uint4korr(buf3 + page_size - sizeof(buf) - CHECKSUM_LEN))
      res= give_error("Corrupt page, invalid checksum");
  }
  my_free(buf3);
  if (res)
    return res;

  /*
    Check that there is a valid root node at the end of the file.
    If there is not, the index may be a "hot index" that is currently being
    constructed. Or it was only partially written before server crash and not
    recovered for some reason.
  */
  uchar flags= buf[GTID_INDEX_PAGE_HEADER_SIZE];
  constexpr uchar needed_flags= PAGE_FLAG_ROOT|PAGE_FLAG_LAST;
  if ((flags & needed_flags) == needed_flags)
  {
    /* Special case: the index is a single page, which is the root node. */
    has_root_node= true;
  }
  else
  {
    uchar buf2[GTID_INDEX_PAGE_HEADER_SIZE];
    if (MY_FILEPOS_ERROR == mysql_file_seek(index_file, -(int32)page_size,
                                            MY_SEEK_END, MYF(0)) ||
        mysql_file_read(index_file, buf2, GTID_INDEX_PAGE_HEADER_SIZE,
                        MYF(MY_NABP)))
      return give_error("Error reading root page from index file");
    flags= buf2[0];
    has_root_node= ((flags & needed_flags) == needed_flags);
    /* No need to verify checksum here, will be done by read_root_node(). */
  }
  index_valid= true;
  return 0;
}


int
Gtid_index_reader::verify_checksum(Gtid_index_base::Node_page *page)
{
  uint32 calc_checksum= my_checksum(0, page->page, page_size - CHECKSUM_LEN);
  uint32 read_checksum= uint4korr(page->page + page_size - CHECKSUM_LEN);
  if (calc_checksum != read_checksum)
    return give_error("Corrupt page, invalid checksum");
  return 0;
}


Gtid_index_base::Node_page *
Gtid_index_reader::alloc_and_read_page()
{
  Node_page *page= alloc_page();
  if (!page)
  {
    give_error("Error allocating memory for index page");
    return nullptr;
  }
  if (mysql_file_read(index_file, page->page, page_size, MYF(MY_NABP)))
  {
    my_free(page);
    give_error("Error reading page from index file");
    return nullptr;
  }
  if (verify_checksum(page))
  {
    my_free(page);
    return nullptr;
  }
  return page;
}


int
Gtid_index_reader::read_root_node()
{
  if (!index_valid || !has_root_node)
    return 1;

  cold_node.reset();
  n= &cold_node;
  /*
    Read pages one by one from the back of the file until we have a complete
    root node.
  */
  if (MY_FILEPOS_ERROR == mysql_file_seek(index_file, -(int32)page_size,
                                          MY_SEEK_END, MYF(0)))
    return give_error("Error seeking index file");

  for (;;)
  {
    Node_page *page= alloc_and_read_page();
    if (!page)
      return 1;
    if (mysql_file_tell(index_file, MYF(0)) == page_size)
      page->flag_ptr= &page->page[GTID_INDEX_FILE_HEADER_SIZE];
    else
      page->flag_ptr= &page->page[0];
    page->next= n->first_page;
    n->first_page= page;
    uchar flags= *page->flag_ptr;
    if (unlikely(!(flags & PAGE_FLAG_ROOT)))
      return give_error("Corrupt or truncated index, no root node found");
    if (!(flags & PAGE_FLAG_IS_CONT))
      break;                           // Found start of root node
    if (MY_FILEPOS_ERROR == mysql_file_seek(index_file, -(int32)(2*page_size),
                                            MY_SEEK_CUR, MYF(0)))
      return give_error("Error seeking index file for multi-page root node");
  }

  read_page= n->first_page;
  read_ptr= read_page->flag_ptr + GTID_INDEX_PAGE_HEADER_SIZE;
  return 0;
}


int
Gtid_index_reader::read_node(uint32 page_ptr)
{
  DBUG_ASSERT(page_ptr != 0 /* No zero child pointers in on-disk pages. */);
  if (!index_valid || !page_ptr)
    return 1;
  return read_node_cold(page_ptr);
}


int
Gtid_index_reader::read_node_cold(uint32 page_ptr)
{
  if (MY_FILEPOS_ERROR == mysql_file_seek(index_file, (page_ptr-1)*page_size,
                                          MY_SEEK_SET, MYF(0)))
    return give_error("Error seeking index file");

  bool file_header= (page_ptr == 1);
  cold_node.reset();
  n= &cold_node;
  Node_page **next_ptr_ptr= &n->first_page;
  for (;;)
  {
    Node_page *page= alloc_and_read_page();
    if (!page)
      return 1;
    page->flag_ptr= &page->page[file_header ? GTID_INDEX_FILE_HEADER_SIZE : 0];
    file_header= false;
    /* Insert the page at the end of the list. */
    page->next= nullptr;
    *next_ptr_ptr= page;
    next_ptr_ptr= &page->next;

    uchar flags= *(page->flag_ptr);
    if (flags & PAGE_FLAG_LAST)
      break;
  }

  read_page= n->first_page;
  read_ptr= read_page->flag_ptr + GTID_INDEX_PAGE_HEADER_SIZE;
  return 0;
}


int Gtid_index_reader::give_error(const char *msg)
{
  sql_print_information("Error reading binlog GTID index, will "
                        "fallback to slower sequential binlog scan. "
                        "Error is: %s", msg);
  return 1;
}


Gtid_index_reader_hot::Gtid_index_reader_hot()
  : hot_writer(nullptr)
{
}


int
Gtid_index_reader_hot::get_child_ptr(uint32 *out_child_ptr)
{
  if (find_bytes(4))
  {
    /*
      If reading hot index, EOF or zero child ptr means the child pointer has
      not yet been written. A zero out_child_ptr makes read_node() read the
      hot node for the child.
    */
    if (hot_writer)
    {
      *out_child_ptr= 0;
      return 0;
    }
    return give_error("Corrupt index, short index node");
  }
  *out_child_ptr= (uint32)uint4korr(read_ptr);
  read_ptr+= 4;
  return 0;
}


int
Gtid_index_reader_hot::do_index_search(uint32 *out_offset,
                                       uint32 *out_gtid_count)
{
  /* Check for a "hot" index. */
  Gtid_index_writer::lock_gtid_index();
  hot_writer= Gtid_index_writer::find_hot_index(index_file_name);
  if (!hot_writer)
  {
    Gtid_index_writer::unlock_gtid_index();
    /*
      Check the index file header (and index end) again, in case it was
      hot when open_index_file() was called, but became cold in the meantime.
    */
    if (!has_root_node && Gtid_index_reader::read_file_header())
      return -1;
  }

  int res= do_index_search_root(out_offset, out_gtid_count);

  if (hot_writer)
  {
    hot_writer= nullptr;
    Gtid_index_writer::unlock_gtid_index();
  }
  return res;
}


int
Gtid_index_reader_hot::read_file_header()
{
  if (!file_open)
    return 1;

  Gtid_index_writer::lock_gtid_index();
  hot_writer= Gtid_index_writer::find_hot_index(index_file_name);
  if (!hot_writer)
    Gtid_index_writer::unlock_gtid_index();

  int res;
  if (hot_writer && hot_writer->max_level == 0)
  {
    /*
      No pages from the hot index have been written to disk, there's just a
      single incomplete node at level 0.
      We have to read the file header from the in-memory page.
    */
    uchar *p= hot_writer->nodes[0]->first_page->page;
    page_size= uint4korr(p + 8);
    has_root_node= false;
    index_valid= true;
    res= 0;
  }
  else
    res= Gtid_index_reader::read_file_header();

  if (hot_writer)
  {
    hot_writer= nullptr;
    Gtid_index_writer::unlock_gtid_index();
  }
  return res;
}


int
Gtid_index_reader_hot::read_root_node()
{
  if (!index_valid)
    return 1;

  if (hot_writer)
  {
    hot_level= hot_writer->max_level;
    return read_node_hot();
  }
  if (has_root_node)
  {
    return Gtid_index_reader::read_root_node();
  }
  return 1;
}


int
Gtid_index_reader_hot::read_node(uint32 page_ptr)
{
  if (!index_valid || (!page_ptr && !hot_writer))
    return 1;

  if (hot_writer)
  {
    if (!page_ptr)
    {
      /*
        The "hot" index is only partially written. Not yet written child pages
        are indicated by zero child pointers. Such child pages are found from
        the list of active nodes in the writer.
      */
      if (hot_level <= 0)
      {
        DBUG_ASSERT(0 /* Should be no child pointer to follow on leaf page. */);
        return give_error("Corrupt hot index (child pointer on leaf page");
      }
      DBUG_ASSERT(n == hot_writer->nodes[hot_level]);
      --hot_level;
      return read_node_hot();
    }

    /*
      We started searching the "hot" index, but now we've reached a "cold"
      part of the index that's already fully written. So leave the "hot index"
      mode and continue reading pages from the on-disk index from here.
    */
    hot_writer= nullptr;
    Gtid_index_writer::unlock_gtid_index();
  }

  return read_node_cold(page_ptr);
}


int
Gtid_index_reader_hot::read_node_hot()
{
  if (hot_writer->error_state)
    return give_error("Cannot access hot index");
  n= hot_writer->nodes[hot_level];
  read_page= n->first_page;
  /* The writer should allocate pages for all nodes. */
  DBUG_ASSERT(read_page != nullptr);
  if (!read_page)
    return give_error("Page not available in hot index");
  read_ptr= read_page->flag_ptr + GTID_INDEX_PAGE_HEADER_SIZE;
  return 0;
}
