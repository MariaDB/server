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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef FILESORT_INCLUDED
#define FILESORT_INCLUDED

#include "my_base.h"                            /* ha_rows */
#include "filesort_utils.h"

class SQL_SELECT;
class THD;
struct TABLE;
struct SORT_FIELD;
class Filesort_tracker;

class SORT_INFO
{
  /// Buffer for sorting keys.
  Filesort_buffer filesort_buffer;

public:
  SORT_INFO()
    :addon_field(0), record_pointers(0)
  {
    buffpek.str= 0;
    my_b_clear(&io_cache);
  }

  ~SORT_INFO();

  void free_data()
  {
    close_cached_file(&io_cache);
    my_free(record_pointers);
    my_free(buffpek.str);
    my_free(addon_field);
  }

  void reset()
  {
    free_data();
    record_pointers= 0;
    buffpek.str= 0;
    addon_field= 0;
  }


  IO_CACHE  io_cache;           /* If sorted through filesort */
  LEX_STRING buffpek;           /* Buffer for buffpek structures */
  LEX_STRING addon_buf;         /* Pointer to a buffer if sorted with fields */
  struct st_sort_addon_field *addon_field;     /* Pointer to the fields info */
  /* To unpack back */
  void    (*unpack)(struct st_sort_addon_field *, uchar *, uchar *);
  uchar     *record_pointers;    /* If sorted in memory */
  /*
    How many rows in final result.
    Also how many rows in record_pointers, if used
  */
  ha_rows   return_rows;
  ha_rows   examined_rows;	/* How many rows read */
  ha_rows   found_rows;         /* How many rows was accepted */

  /** Sort filesort_buffer */
  void sort_buffer(Sort_param *param, uint count)
  { filesort_buffer.sort_buffer(param, count); }

  /**
     Accessors for Filesort_buffer (which @c).
  */
  uchar *get_record_buffer(uint idx)
  { return filesort_buffer.get_record_buffer(idx); }

  uchar **get_sort_keys()
  { return filesort_buffer.get_sort_keys(); }

  uchar **alloc_sort_buffer(uint num_records, uint record_length)
  { return filesort_buffer.alloc_sort_buffer(num_records, record_length); }

  void free_sort_buffer()
  { filesort_buffer.free_sort_buffer(); }

  void init_record_pointers()
  { filesort_buffer.init_record_pointers(); }

  size_t sort_buffer_size() const
  { return filesort_buffer.sort_buffer_size(); }

  friend SORT_INFO *filesort(THD *thd, TABLE *table, SORT_FIELD *sortorder,
                                 uint s_length,
                                 SQL_SELECT *select, ha_rows max_rows,
                                 bool sort_positions,
                                 Filesort_tracker* tracker);
};


SORT_INFO *filesort(THD *thd, TABLE *table, SORT_FIELD *sortorder,
                        uint s_length,
                        SQL_SELECT *select, ha_rows max_rows,
                        bool sort_positions,
                        Filesort_tracker* tracker);

void change_double_for_sort(double nr,uchar *to);

#endif /* FILESORT_INCLUDED */
