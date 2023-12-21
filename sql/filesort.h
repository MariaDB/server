/* Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef FILESORT_INCLUDED
#define FILESORT_INCLUDED

#include "my_base.h"                            /* ha_rows */
#include "sql_alloc.h"
#include "filesort_utils.h"

class SQL_SELECT;
class THD;
struct TABLE;
class Filesort_tracker;
struct SORT_FIELD;
struct SORT_FIELD_ATTR;
typedef struct st_order ORDER;
class JOIN;
class Addon_fields;
class Sort_keys;


/**
  Sorting related info.
  To be extended by another WL to include complete filesort implementation.
*/
class Filesort: public Sql_alloc
{
public:
  /** List of expressions to order the table by */
  ORDER *order;
  /** Number of records to return */
  ha_rows limit;
  /** ORDER BY list with some precalculated info for filesort */
  SORT_FIELD *sortorder;
  /* Used with ROWNUM. Contains the number of rows filesort has found so far */
  ha_rows *accepted_rows;
  /** select to use for getting records */
  SQL_SELECT *select;

  /** TRUE <=> free select on destruction */
  bool own_select;
  /** TRUE means we are using Priority Queue for order by with limit. */
  bool using_pq;
  /* 
    TRUE means sort operation must produce table rowids. 
    FALSE means that it also has an option of producing {sort_key, addon_fields}
          pairs.

    Usually initialized with value of join_tab->keep_current_rowid to allow for
    a call to table->file->position() using these table rowids.
  */
  bool sort_positions;
  /*
    TRUE means all the fields of table of whose bitmap read_set is set
         need to be read while reading records in the sort buffer.
    FALSE otherwise
  */
  bool set_all_read_bits;

  Filesort_tracker *tracker;
  Sort_keys *sort_keys;

  /* Unpack temp table columns to base table columns*/
  void (*unpack)(TABLE *);

  Filesort(ORDER *order_arg, ha_rows limit_arg, bool sort_positions_arg,
           SQL_SELECT *select_arg):
    order(order_arg),
    limit(limit_arg),
    sortorder(NULL),
    accepted_rows(0),
    select(select_arg),
    own_select(false), 
    using_pq(false),
    sort_positions(sort_positions_arg),
    set_all_read_bits(false),
    sort_keys(NULL),
    unpack(NULL)
  {
    DBUG_ASSERT(order);
  };

  ~Filesort() { cleanup(); }
  /* Prepare ORDER BY list for sorting. */
  Sort_keys* make_sortorder(THD *thd, JOIN *join, table_map first_table_bit);

private:
  void cleanup();
};


class SORT_INFO
{
  /// Buffer for sorting keys.
  Filesort_buffer filesort_buffer;

public:
  SORT_INFO()
    :addon_fields(NULL), record_pointers(0),
     sort_keys(NULL),
     sorted_result_in_fsbuf(FALSE)
  {
    buffpek.str= 0;
    my_b_clear(&io_cache);
  }

  ~SORT_INFO();

  void free_data()
  {
    close_cached_file(&io_cache);
    free_addon_buff();
    my_free(record_pointers);
    my_free(buffpek.str);
    my_free(addon_fields);
    free_sort_buffer();
  }

  void reset()
  {
    free_data();
    record_pointers= 0;
    buffpek.str= 0;
    addon_fields= 0;
    sorted_result_in_fsbuf= false;
  }

  void free_addon_buff();

  IO_CACHE  io_cache;           /* If sorted through filesort */
  LEX_STRING buffpek;           /* Buffer for buffpek structures */
  Addon_fields *addon_fields;   /* Addon field descriptors */
  uchar     *record_pointers;    /* If sorted in memory */
  Sort_keys *sort_keys;         /* Sort key descriptors*/

  /**
    If the entire result of filesort fits in memory, we skip the merge phase.
    We may leave the result in filesort_buffer
    (indicated by sorted_result_in_fsbuf), or we may strip away
    the sort keys, and copy the sorted result into a new buffer.
    @see save_index()
   */
  bool      sorted_result_in_fsbuf;

  /*
    How many rows in final result.
    Also how many rows in record_pointers, if used
  */
  ha_rows   return_rows;
  ha_rows   m_examined_rows;    /* How many rows read. Already in thd */
  ha_rows   found_rows;         /* How many rows was accepted */

  /** Sort filesort_buffer */
  void sort_buffer(Sort_param *param, uint count)
  { filesort_buffer.sort_buffer(param, count); }

  uchar **get_sort_keys()
  { return filesort_buffer.get_sort_keys(); }

  uchar *get_sorted_record(uint ix)
  { return filesort_buffer.get_sorted_record(ix); }

  uchar *alloc_sort_buffer(uint num_records, uint record_length)
  { return filesort_buffer.alloc_sort_buffer(num_records, record_length); }

  void free_sort_buffer()
  { filesort_buffer.free_sort_buffer(); }

  bool isfull() const
  { return filesort_buffer.isfull(); }
  void init_record_pointers()
  { filesort_buffer.init_record_pointers(); }
  void init_next_record_pointer()
  { filesort_buffer.init_next_record_pointer(); }
  uchar *get_next_record_pointer()
  { return filesort_buffer.get_next_record_pointer(); }
  void adjust_next_record_pointer(uint val)
  { filesort_buffer.adjust_next_record_pointer(val); }

  Bounds_checked_array<uchar> get_raw_buf()
  { return filesort_buffer.get_raw_buf(); }

  size_t sort_buffer_size() const
  { return filesort_buffer.sort_buffer_size(); }

  bool is_allocated() const
  { return filesort_buffer.is_allocated(); }
  void set_sort_length(uint val)
  { filesort_buffer.set_sort_length(val); }
  uint get_sort_length() const
  { return filesort_buffer.get_sort_length(); }

  bool has_filesort_result_in_memory() const
  {
    return record_pointers || sorted_result_in_fsbuf;
  }

  /// Are we using "addon fields"?
  bool using_addon_fields() const
  {
    return addon_fields != NULL;
  }

  /// Are we using "packed addon fields"?
  bool using_packed_addons();

  /**
    Copies (unpacks) values appended to sorted fields from a buffer back to
    their regular positions specified by the Field::ptr pointers.
    @param buff            Buffer which to unpack the value from
  */
  template<bool Packed_addon_fields>
  inline void unpack_addon_fields(uchar *buff);

  bool using_packed_sortkeys();

  friend SORT_INFO *filesort(THD *thd, TABLE *table, Filesort *filesort,
                             Filesort_tracker* tracker, JOIN *join,
                             table_map first_table_bit);
};

SORT_INFO *filesort(THD *thd, TABLE *table, Filesort *filesort,
                    Filesort_tracker* tracker, JOIN *join=NULL,
                    table_map first_table_bit=0);

bool filesort_use_addons(TABLE *table, uint sortlength,
                         uint *length, uint *fields, uint *null_fields,
                         uint *m_packable_length);

void change_double_for_sort(double nr,uchar *to);
void store_length(uchar *to, uint length, uint pack_length);
void
reverse_key(uchar *to, const SORT_FIELD_ATTR *sort_field);

#endif /* FILESORT_INCLUDED */
