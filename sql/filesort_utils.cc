/* Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved. 
   Copyright (c) 2012, 2020, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "mariadb.h"
#include "filesort_utils.h"
#include "sql_const.h"
#include "sql_sort.h"
#include "table.h"


PSI_memory_key key_memory_Filesort_buffer_sort_keys;

namespace {
/**
  A local helper function. See comments for get_merge_buffers_cost().
 */
double get_merge_cost(ha_rows num_elements, ha_rows num_buffers, uint elem_size)
{
  return 
    2.0 * ((double) num_elements * elem_size) / IO_SIZE
    + (double) num_elements * log((double) num_buffers) /
      (TIME_FOR_COMPARE_ROWID * M_LN2);
}
}

/**
  This is a simplified, and faster version of @see get_merge_many_buffs_cost().
  We calculate the cost of merging buffers, by simulating the actions
  of @see merge_many_buff. For explanations of formulas below,
  see comments for get_merge_buffers_cost().
  TODO: Use this function for Unique::get_use_cost().
*/
double get_merge_many_buffs_cost_fast(ha_rows num_rows,
                                      ha_rows num_keys_per_buffer,
                                      uint    elem_size)
{
  ha_rows num_buffers= num_rows / num_keys_per_buffer;
  ha_rows last_n_elems= num_rows % num_keys_per_buffer;
  double total_cost;

  // Calculate CPU cost of sorting buffers.
  total_cost=
    ((num_buffers * num_keys_per_buffer * log(1.0 + num_keys_per_buffer) +
      last_n_elems * log(1.0 + last_n_elems)) /
     TIME_FOR_COMPARE_ROWID);
  
  // Simulate behavior of merge_many_buff().
  while (num_buffers >= MERGEBUFF2)
  {
    // Calculate # of calls to merge_buffers().
    const ha_rows loop_limit= num_buffers - MERGEBUFF*3/2;
    const ha_rows num_merge_calls= 1 + loop_limit/MERGEBUFF;
    const ha_rows num_remaining_buffs=
      num_buffers - num_merge_calls * MERGEBUFF;

    // Cost of merge sort 'num_merge_calls'.
    total_cost+=
      num_merge_calls *
      get_merge_cost(num_keys_per_buffer * MERGEBUFF, MERGEBUFF, elem_size);

    // # of records in remaining buffers.
    last_n_elems+= num_remaining_buffs * num_keys_per_buffer;

    // Cost of merge sort of remaining buffers.
    total_cost+=
      get_merge_cost(last_n_elems, 1 + num_remaining_buffs, elem_size);

    num_buffers= num_merge_calls;
    num_keys_per_buffer*= MERGEBUFF;
  }

  // Simulate final merge_buff call.
  last_n_elems+= num_keys_per_buffer * num_buffers;
  total_cost+= get_merge_cost(last_n_elems, 1 + num_buffers, elem_size);
  return total_cost;
}

/*
  alloc_sort_buffer()

  Allocate buffer for sorting keys.
  Try to reuse old buffer if possible.

  @return
    0   Error
    #   Pointer to allocated buffer
*/

uchar *Filesort_buffer::alloc_sort_buffer(uint num_records,
                                          uint record_length)
{
  size_t buff_size;
  DBUG_ENTER("alloc_sort_buffer");
  DBUG_EXECUTE_IF("alloc_sort_buffer_fail",
                  DBUG_SET("+d,simulate_out_of_memory"););

  buff_size= ALIGN_SIZE(num_records * (record_length + sizeof(uchar*)));

  if (m_rawmem)
  {
    /*
      Reuse old buffer if exists and is large enough
      Note that we don't make the buffer smaller, as we want to be
      prepared for next subquery iteration.
    */
    if (buff_size > m_size_in_bytes)
    {
      /*
        Better to free and alloc than realloc as we don't have to remember
        the old values
      */
      my_free(m_rawmem);
      if (!(m_rawmem= (uchar*) my_malloc(key_memory_Filesort_buffer_sort_keys,
                                         buff_size, MYF(MY_THREAD_SPECIFIC))))
      {
        m_size_in_bytes= 0;
        DBUG_RETURN(0);
      }
    }
  }
  else
  {
    if (!(m_rawmem= (uchar*) my_malloc(key_memory_Filesort_buffer_sort_keys,
                                       buff_size, MYF(MY_THREAD_SPECIFIC))))
    {
      m_size_in_bytes= 0;
      DBUG_RETURN(0);
    }

  }

  m_size_in_bytes= buff_size;
  m_record_pointers= reinterpret_cast<uchar**>(m_rawmem) +
                     ((m_size_in_bytes / sizeof(uchar*)) - 1);
  m_num_records= num_records;
  m_record_length= record_length;
  m_idx= 0;
  DBUG_RETURN(m_rawmem);
}


void Filesort_buffer::free_sort_buffer()
{
  my_free(m_rawmem);
  *this= Filesort_buffer();
}


void Filesort_buffer::sort_buffer(const Sort_param *param, uint count)
{
  size_t size= param->sort_length;
  m_sort_keys= get_sort_keys();

  if (count <= 1 || size == 0)
    return;

  // don't reverse for PQ, it is already done
  if (!param->using_pq)
    reverse_record_pointers();

  uchar **buffer= NULL;
  if (!param->using_packed_sortkeys() &&
      radixsort_is_appliccable(count, param->sort_length) &&
      (buffer= (uchar**) my_malloc(PSI_INSTRUMENT_ME, count*sizeof(char*),
                                   MYF(MY_THREAD_SPECIFIC))))
  {
    radixsort_for_str_ptr(m_sort_keys, count, param->sort_length, buffer);
    my_free(buffer);
    return;
  }

  my_qsort2(m_sort_keys, count, sizeof(uchar*),
            param->get_compare_function(),
            param->get_compare_argument(&size));
}
