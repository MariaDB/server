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


static const uchar GTID_INDEX_MAGIC[8]= {
  'M', 'D', 'B', 'B', 'L', 'I', 'D', 'X'
};

Gtid_index_writer::Gtid_index_writer(const char *filename, my_off_t offset,
                                     rpl_binlog_state *binlog_state)
  : nodes(nullptr), gtid_buffer(nullptr), previous_offset(0),
    gtid_buffer_alloc(0),
    max_level(0), pending_gtid_count(0), index_file(-1),
    error_state(false), file_header_written(false)
{
  page_size= 4096;   /* ToDo: init from config */
  pending_state.init();

  char *p= strmake(index_file_name, filename, sizeof(index_file_name)-1);
  size_t remain= sizeof(index_file_name) - (p - index_file_name);
  p= strmake(p, ".idx", remain-1);
  index_file= mysql_file_create(key_file_gtid_index, index_file_name,
                                CREATE_MODE, O_RDWR|O_TRUNC|O_BINARY,
                                MYF(MY_WME));
  if (index_file < 0)
  {
    give_error("Failed to open new index file for writing");
    return;
  }

  if (alloc_level_if_missing(0))
  {
    give_error("Out of memory allocating node list");
    return;
  }

  /*
    Write out an initial index record, i.e. corresponding to the GTID_LIST
    event / binlog state at the start of the binlog file.
  */
  uint32 count= binlog_state->count();
  rpl_gtid *gtid_list= gtid_list_buffer(count);
  if (count > 0)
  {
    if (!gtid_list)
      return;
    binlog_state->get_gtid_list(gtid_list, count);
  }
  write_record(offset, gtid_list, count);
}


Gtid_index_writer::~Gtid_index_writer()
{
  if (index_file > 0)
  {
    /*
      Should have been closed by call to Gtid_index_writer::close().
      We can at least avoid leaking file descriptor.
    */
    mysql_file_close(index_file, MYF(0));
  }

  my_free(gtid_buffer);
  if (nodes)
  {
    for (uint32 i= 0; i <= max_level; ++i)
      delete nodes[i];
    my_free(nodes);
  }

  /*
    state.free() is not needed here, will be called from rpl_binlog_state
    destructor.
  */
}


void
Gtid_index_writer::process_gtid(my_off_t offset, const rpl_gtid *gtid)
{
  ++pending_gtid_count;
  if (unlikely(pending_state.update_nolock(gtid, false)))
  {
    give_error("Out of memory processing GTID for binlog GTID index");
    return;
  }
  /*
    Sparse index; we record only selected GTIDs, and scan the binlog forward
    from there to find the exact spot.
  */
  if (offset - previous_offset < offset_max_threshold &&
      (offset - previous_offset < offset_min_threshold ||
       pending_gtid_count < gtid_threshold))
    return;

  uint32 count= pending_state.count();
  DBUG_ASSERT(count > 0 /* Since we just updated with a GTID. */);
  rpl_gtid *gtid_list= (rpl_gtid *)
    my_malloc(PSI_INSTRUMENT_ME, count*sizeof(*gtid_list), MYF(0));
  if (unlikely(!gtid_list))
  {
    give_error("Out of memory allocating GTID list for binlog GTID index");
    return;
  }
  if (unlikely(pending_state.get_gtid_list(gtid_list, count)))
  {
    /* Shouldn't happen as we allocated the list with the correct length. */
    DBUG_ASSERT(false);
    give_error("Internal error allocating GTID list for binlog GTID index");
    return;
  }
  pending_state.reset();
  previous_offset= offset;
  pending_gtid_count= 0;
  // ToDo: Enter the async path, send a requenst to the binlog background thread.
  // For now, direct call.
  // Also, for recovery, we would still have direct call.
  write_record(offset, gtid_list, count);
  my_free(gtid_list);
}


void
Gtid_index_writer::close(my_off_t offset)
{
  // ToDo: Enter the async path, send a requenst to the binlog background thread.

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

    if (mysql_file_sync(index_file, MYF(MY_WME)))
      give_error("Error syncing index file to disk");
    else
    {
      // ToDo: something with binlog checkpoints or something to mark that now this index file need no longer be crash recovered. Or alternatively, crash recovery at startup could scan binlog index files (even if no crashed main binlog), and just recover any that are not with a clean root page at the end (PAGE_FLAG_ROOT | PAGE_FLAG_LAST).
    }
  }

  // ToDo: Do this from outside instead in the async path.
  delete this;
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

  my_off_t node_pos= mysql_file_tell(index_file, MYF(0));

  for (Node_page *p= n->first_page; p ; p= p->next)
  {
    if (unlikely(is_root))
      *(p->flag_ptr) |= PAGE_FLAG_ROOT;
    if (likely(!p->next))
      *(p->flag_ptr) |= PAGE_FLAG_LAST;
    if (mysql_file_write(index_file, p->page, page_size, MYF(MY_WME|MY_NABP)))
    {
      give_error("Error writing index page");
      return 0;
    }
  }

  DBUG_ASSERT(node_pos % page_size == 0);
  /* Page numbers are +1 just so that zero can denote invalid page pointer. */
  return 1 + (node_pos / page_size);
}


void
Gtid_index_writer::Index_node::reset()
{
  Index_node_base::reset();
  state.reset();
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
      likely(n->current_ptr - n->current_page->page + bytes <= page_size))
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
  int4store(n->current_ptr, gtid_count);
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

  Adding a child pointer doesn't spill to a new page, code must make sure that
  there is always room for the final child pointer in current non-leaf node.

  ToDo: A child pointer is 8 bytes for now to preserve 8-byte alignment.
*/
void
Gtid_index_writer::add_child_ptr(uint32 level, my_off_t node_offset)
{
  DBUG_ASSERT(level <= max_level);
  DBUG_ASSERT(node_offset % page_size == 0);
  Index_node *n= nodes[level];
  DBUG_ASSERT(n->current_page);
  DBUG_ASSERT((size_t)(n->current_ptr - n->current_page->page + 8) <= page_size);

  int8store(n->current_ptr, (node_offset / page_size) + 1);
  n->current_ptr+= 8;
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
    for (uint32 i= 0; i < gtid_count; ++i)
      if (n->state.update_nolock(&gtid_list[i], false))
        /* ToDo: Something that doesn't call my_error() here? */
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
    if (alloc_level_if_missing(level+1))
      return 1;
    add_child_ptr(level+1, node_ptr);
    uint32 new_count= n->state.count();  /* ToDo faster count op? */
    rpl_gtid *new_gtid_list= gtid_list_buffer(new_count);
    if (n->state.get_gtid_list(new_gtid_list, new_count))
      return give_error("Internal error processing GTID state");
    n->reset();
    if (level == 0)
      if (do_write_record(level, event_offset, new_gtid_list, new_count))
        return 1;
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
  if (!n->current_page)
    return true;
  /*
    Make sure we use at least 1/2 a page of room after the initial record,
    setting a flag to allocate a spill page later if needed.
  */
  size_t avail= page_size - (n->current_ptr - n->current_page->page);
  if (n->num_records==1 && avail < page_size/2)
  {
    n->force_spill_page= true;
    return true;
  }
  if (n->force_spill_page)
    return true;
  size_t needed= 8 + 16*gtid_count;
  /* Non-leaf pages need extra 8 bytes for a child pointer. */
  if (level > 0)
    needed+= 8;
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
    my_realloc(PSI_INSTRUMENT_ME, nodes, (level+1)*sizeof(*nodes),
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


rpl_gtid *
Gtid_index_writer::gtid_list_buffer(uint32 count)
{
  if (gtid_buffer_alloc >= count)
    return gtid_buffer;
  rpl_gtid *new_buffer= (rpl_gtid *)
    my_malloc(PSI_INSTRUMENT_ME, count*sizeof(*new_buffer), MYF(0));
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


/*
  Initialize the start of a data page.
  This is at the start of a page, except for the very first page where it
  comes after the global file header.
  Format:
    0    flags.
    1-7  unused padding/reserved.

  The argument FIRST denotes if this is the first page (if false it is a
  continuation page).
*/
uchar *
Gtid_index_writer::init_header(Node_page *page, bool is_leaf, bool is_first)
{
  uchar *p= page->page;

  if (unlikely(!file_header_written))
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
  p+= 7;
  DBUG_ASSERT(p == page->page + GTID_INDEX_FILE_HEADER_SIZE +
              GTID_INDEX_PAGE_HEADER_SIZE);
  DBUG_ASSERT((size_t)(p - page->page) < page_size);
  return p;
}


Gtid_index_base::Node_page *Gtid_index_base::alloc_page()
{
  Node_page *new_node= (Node_page *)
    my_malloc(PSI_INSTRUMENT_ME,
              sizeof(Node_page) + page_size,
              MYF(MY_ZEROFILL|MY_WME));
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
  : index_file(-1),
    file_open(false), index_valid(false),
    version_major(0), version_minor(0)
{
}


Gtid_index_reader::~Gtid_index_reader()
{
  if (file_open)
    mysql_file_close(index_file, MYF(0));
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
Gtid_index_reader::read_file_header()
{
  index_valid= false;
  if (!file_open)
    return 1;

  /*
    Read the file header and check that it's valid and that the format is not
    too new a version for us to be able to read it.
  */
  uchar buf[GTID_INDEX_FILE_HEADER_SIZE + GTID_INDEX_PAGE_HEADER_SIZE];

  if (MY_FILEPOS_ERROR == mysql_file_seek(index_file, 0, MY_SEEK_SET, MYF(0)) ||
      mysql_file_read(index_file, buf,
                      GTID_INDEX_FILE_HEADER_SIZE + GTID_INDEX_PAGE_HEADER_SIZE,
                      MYF(MY_NABP)))
    return 1;
  if (memcmp(&buf[0], GTID_INDEX_MAGIC, sizeof(GTID_INDEX_MAGIC)))
    return 1;
  version_major= buf[8];
  version_minor= buf[9];
  /* We cannot safely read a major version we don't know about. */
  if (version_major > GTID_INDEX_VERSION_MAJOR)
    return 1;
  page_size= uint4korr(&buf[12]);

  /*
    Check that there is a valid root node at the end of the file.
    (If there is not, the index was only partially written before server crash).
  */
  uchar flags= buf[GTID_INDEX_PAGE_HEADER_SIZE];
  constexpr uchar needed_flags= PAGE_FLAG_ROOT|PAGE_FLAG_LAST;
  if ((flags & needed_flags) == needed_flags)
  {
    /* Special case: the index is a single page, which is the root node. */
  }
  else
  {
    uchar buf2[GTID_INDEX_PAGE_HEADER_SIZE];
    if (MY_FILEPOS_ERROR == mysql_file_seek(index_file, -(my_off_t)page_size,
                                            MY_SEEK_END, MYF(0)) ||
        mysql_file_read(index_file, buf2, GTID_INDEX_PAGE_HEADER_SIZE,
                        MYF(MY_NABP)))
      return 1;
    flags= buf2[0];
    if (!((flags & needed_flags) == needed_flags))
      return 1;
  }
  index_valid= true;
  return 0;
}


int
Gtid_index_reader::read_root_node()
{
  if (!index_valid)
    return 1;

  n.reset();
  /*
    Read pages one by one from the back of the file until we have a complete
    root node.
  */
  if (MY_FILEPOS_ERROR == mysql_file_seek(index_file, -(my_off_t)page_size,
                                          MY_SEEK_END, MYF(0)))
    return 1;

  for (;;)
  {
    Node_page *page= alloc_page();
    if (!page)
      return 1;
    if (mysql_file_read(index_file, page->page, page_size, MYF(MY_NABP)))
    {
      my_free(page);
      return 1;
    }
    page->next= n.first_page;
    n.first_page= page;
    uchar flags= page->page[0];
    if (unlikely(!(flags & PAGE_FLAG_ROOT)))
      return 1;                        // Corrupt index, no start of root node
    if (!(flags & PAGE_FLAG_IS_CONT))
      break;                           // Found start of root node
    if (MY_FILEPOS_ERROR == mysql_file_seek(index_file, -(my_off_t)page_size,
                                            MY_SEEK_CUR, MYF(0)))
      return 1;
  }

  return 0;
}


int
Gtid_index_reader::read_node(uint32 page_ptr)
{
  if (!index_valid || !page_ptr)
    return 1;

  if (MY_FILEPOS_ERROR == mysql_file_seek(index_file, (page_ptr-1)*page_size,
                                          MY_SEEK_SET, MYF(0)))
    return 1;

  bool file_header= (page_ptr == 1);
  n.reset();
  Node_page **next_ptr_ptr= &n.first_page;
  for (;;)
  {
    Node_page *page= alloc_page();
    if (!page)
      return 1;
    if (mysql_file_read(index_file, page->page, page_size, MYF(MY_NABP)))
    {
      my_free(page);
      return 1;
    }
    page->flag_ptr= &page->page[file_header ? GTID_INDEX_PAGE_HEADER_SIZE : 0];
    file_header= false;
    /* Insert the page at the end of the list. */
    page->next= nullptr;
    *next_ptr_ptr= page;
    next_ptr_ptr= &page->next;

    uchar flags= *(page->flag_ptr);
    if (flags & PAGE_FLAG_LAST)
      break;
  }

  return 0;
}


int Gtid_index_reader::give_error(const char *msg)
{
  sql_print_information("Error reading binlog GTID index, will "
                        "fallback to slower sequential binlog scan. "
                        "Error is: %s", msg);
  return 1;
}
