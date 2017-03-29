/* Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved. 

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include "filesort_utils.h"
#include "sql_const.h"
#include "sql_sort.h"
#include "table.h"
#include "my_sys.h"


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
    ( num_buffers * num_keys_per_buffer * log(1.0 + num_keys_per_buffer) +
      last_n_elems * log(1.0 + last_n_elems) )
    / TIME_FOR_COMPARE_ROWID;
  
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

uchar **Filesort_buffer::alloc_sort_buffer(uint num_records, uint record_length)
{
  ulong sort_buff_sz;

  DBUG_ENTER("alloc_sort_buffer");

  DBUG_EXECUTE_IF("alloc_sort_buffer_fail",
                  DBUG_SET("+d,simulate_out_of_memory"););

  if (m_idx_array.is_null())
  {
    sort_buff_sz= num_records * (record_length + sizeof(uchar*));
    set_if_bigger(sort_buff_sz, record_length * MERGEBUFF2); 
    uchar **sort_keys=
      (uchar**) my_malloc(sort_buff_sz, MYF(MY_THREAD_SPECIFIC));
    m_idx_array= Idx_array(sort_keys, num_records);
    m_record_length= record_length;
    uchar **start_of_data= m_idx_array.array() + m_idx_array.size();
    m_start_of_data= reinterpret_cast<uchar*>(start_of_data);
  }
  else
  {
    DBUG_ASSERT(num_records == m_idx_array.size());
    DBUG_ASSERT(record_length == m_record_length);
  }
  DBUG_RETURN(m_idx_array.array());
}


void Filesort_buffer::free_sort_buffer()
{
  my_free(m_idx_array.array());
  m_idx_array= Idx_array();
  m_record_length= 0;
  m_start_of_data= NULL;
}


void Filesort_buffer::sort_buffer(const Sort_param *param, uint count)
{
  size_t size= param->sort_length;
  if (count <= 1 || size == 0)
    return;
  uchar **keys= get_sort_keys();
  uchar **buffer= NULL;
  if (radixsort_is_appliccable(count, param->sort_length) &&
      (buffer= (uchar**) my_malloc(count*sizeof(char*),
                                   MYF(MY_THREAD_SPECIFIC))))
  {
    radixsort_for_str_ptr(keys, count, param->sort_length, buffer);
    my_free(buffer);
    return;
  }
  
  my_qsort2(keys, count, sizeof(uchar*), get_ptr_compare(size), &size);
}
