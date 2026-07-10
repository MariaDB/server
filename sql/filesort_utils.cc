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
#include "optimizer_defaults.h"

PSI_memory_key key_memory_Filesort_buffer_sort_keys;

const LEX_CSTRING filesort_names[]=
{
  { STRING_WITH_LEN("priority_queue with addon fields")},
  { STRING_WITH_LEN("priority_queue with row lookup")},
  { STRING_WITH_LEN("merge_sort with addon fields")},
  { STRING_WITH_LEN("merge_sort with row lookup)")},
  { STRING_WITH_LEN("Error while computing filesort cost")}
};

/*
 Different ways to do sorting:
 Merge Sort -> Without addon Fields, with fixed length
 Merge Sort -> Without addon Fields, with dynamic length
 Merge Sort -> With addon Fields, with fixed length
 Merge Sort -> With addon Fields, with dynamic length

 Priority queue -> Without addon fields
 Priority queue -> With addon fields

 With PQ (Priority queue) we could have a simple key (memcmp) or a
 complex key (double & varchar for example). This cost difference
 is currently not considered.
*/


/**
  Compute the cost of running qsort over a set of rows.
  @param num_rows           How many rows will be sorted.
  @param with_addon_fields  Set to true if the sorted rows include the whole
                            row (with addon fields) or just the keys themselves.

  @retval
    Cost of the operation.
*/

double get_qsort_sort_cost(ha_rows num_rows, bool with_addon_fields)
{
  const double row_copy_cost= with_addon_fields ? DEFAULT_ROW_COPY_COST :
                                                  DEFAULT_KEY_COPY_COST;
  const double key_cmp_cost= DEFAULT_KEY_COMPARE_COST;
  const double qsort_constant_factor= QSORT_SORT_SLOWNESS_CORRECTION_FACTOR *
                                      (row_copy_cost + key_cmp_cost);

  return qsort_constant_factor * num_rows * log2(1.0 + num_rows);
}


/**
  Compute the cost of sorting num_rows and only retrieving queue_size rows.
  @param num_rows           How many rows will be sorted.
  @param queue_size         How many rows will be returned by the priority
                            queue.
  @param with_addon_fields  Set to true if the sorted rows include the whole
                            row (with addon fields) or just the keys themselves.

  @retval
    Cost of the operation.
*/

double get_pq_sort_cost(size_t num_rows, size_t queue_size,
                        bool with_addon_fields)
{
  const double row_copy_cost= with_addon_fields ? DEFAULT_ROW_COPY_COST :
                                                  DEFAULT_KEY_COPY_COST;
  const double key_cmp_cost= DEFAULT_KEY_COMPARE_COST;
  /* 2 -> 1 insert, 1 pop from the queue*/
  const double pq_sort_constant_factor= PQ_SORT_SLOWNESS_CORRECTION_FACTOR *
                                        2.0 * (row_copy_cost + key_cmp_cost);

  return pq_sort_constant_factor * num_rows * log2(1.0 + queue_size);
}


/**
  Compute the cost of merging "num_buffers" sorted buffers using a priority
  queue.

  See comments for get_merge_buffers_cost().
*/

static
double get_merge_cost(ha_rows num_elements, ha_rows num_buffers,
                      size_t elem_size, double compare_cost,
                      double disk_read_cost)
{
  /* 2 -> 1 read + 1 write */
  const double io_cost= (2.0 * (num_elements * elem_size +
                                DISK_CHUNK_SIZE - 1) /
                         DISK_CHUNK_SIZE) * disk_read_cost;
  /* 2 -> 1 insert, 1 pop for the priority queue used to merge the buffers. */
  const double cpu_cost= (2.0 * num_elements * log2(1.0 + num_buffers) *
                          compare_cost) * PQ_SORT_SLOWNESS_CORRECTION_FACTOR;
  return io_cost + cpu_cost;
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
                                      size_t elem_size,
                                      double key_compare_cost,
                                      double disk_read_cost,
                                      bool with_addon_fields)
{
  DBUG_ASSERT(num_keys_per_buffer != 0);

  ha_rows num_buffers= num_rows / num_keys_per_buffer;
  ha_rows last_n_elems= num_rows % num_keys_per_buffer;
  double total_cost;
  double full_buffer_sort_cost;

  /* Calculate cost for sorting all merge buffers + the last one. */
  full_buffer_sort_cost= get_qsort_sort_cost(num_keys_per_buffer,
                                             with_addon_fields);
  total_cost= (num_buffers * full_buffer_sort_cost +
               get_qsort_sort_cost(last_n_elems, with_addon_fields));

  if (num_buffers >= MERGEBUFF2)
    total_cost+= TMPFILE_CREATE_COST * 2;       // We are creating 2 files.

  /* Simulate behavior of merge_many_buff(). */
  while (num_buffers >= MERGEBUFF2)
  {
    /* Calculate # of calls to merge_buffers(). */
    const ha_rows loop_limit= num_buffers - MERGEBUFF * 3 / 2;
    const ha_rows num_merge_calls= 1 + loop_limit / MERGEBUFF;
    const ha_rows num_remaining_buffs=
      num_buffers - num_merge_calls * MERGEBUFF;

    /* Cost of merge sort 'num_merge_calls'. */
    total_cost+=
      num_merge_calls *
      get_merge_cost(num_keys_per_buffer * MERGEBUFF, MERGEBUFF, elem_size,
                     key_compare_cost, disk_read_cost);

    // # of records in remaining buffers.
    last_n_elems+= num_remaining_buffs * num_keys_per_buffer;

    // Cost of merge sort of remaining buffers.
    total_cost+=
      get_merge_cost(last_n_elems, 1 + num_remaining_buffs, elem_size,
                     key_compare_cost, disk_read_cost);

    num_buffers= num_merge_calls;
    num_keys_per_buffer*= MERGEBUFF;
  }

  // Simulate final merge_buff call.
  last_n_elems+= num_keys_per_buffer * num_buffers;
  total_cost+= get_merge_cost(last_n_elems, 1 + num_buffers, elem_size,
                              key_compare_cost, disk_read_cost);
  return total_cost;
}


void Sort_costs::compute_fastest_sort()
{
  lowest_cost= DBL_MAX;
  uint min_idx= NO_SORT_POSSIBLE_OUT_OF_MEM;
  for (uint i= 0; i < FINAL_SORT_TYPE; i++)
  {
    if (lowest_cost > costs[i])
    {
      min_idx= i;
      lowest_cost= costs[i];
    }
  }
  fastest_sort= static_cast<enum sort_type>(min_idx);
}


/*
  Calculate cost of using priority queue for filesort.
  There are two options: using addon fields or not
*/

void Sort_costs::compute_pq_sort_costs(Sort_param *param, ha_rows num_rows,
                                       size_t memory_available,
                                       bool with_addon_fields)
{
  /*
    Implementation detail of PQ. To be able to keep a PQ of size N we need
    N+1 elements allocated so we can use the last element as "swap" space
    for the "insert" operation.
    TODO(cvicentiu): This should be left as an implementation detail inside
    the PQ, not have the optimizer take it into account.
  */
  size_t queue_size= param->limit_rows + 1;
  size_t row_length, num_available_keys;

  costs[PQ_SORT_ALL_FIELDS]= DBL_MAX;
  costs[PQ_SORT_ORDER_BY_FIELDS]= DBL_MAX;

  /*
    We can't use priority queue if there's no limit or the limit is
    too big.
  */
  if (param->limit_rows == HA_POS_ERROR ||
      param->limit_rows >= UINT_MAX - 2)
    return;

  /* Calculate cost without addon keys (probably using less memory) */
  row_length=         param->sort_length + param->ref_length + sizeof(char*);
  num_available_keys= memory_available / row_length;

  if (queue_size < num_available_keys)
  {
    handler *file= param->sort_form->file;
    costs[PQ_SORT_ORDER_BY_FIELDS]=
      get_pq_sort_cost(num_rows, queue_size, false) +
      file->cost(file->ha_rnd_pos_call_time(MY_MIN(queue_size - 1, num_rows)));
  }

  /* Calculate cost with addon fields */
  if (with_addon_fields)
  {
    row_length=         param->rec_length + sizeof(char *);
    num_available_keys= memory_available / row_length;

    if (queue_size < num_available_keys)
      costs[PQ_SORT_ALL_FIELDS]= get_pq_sort_cost(num_rows, queue_size, true);
  }
}

/*
  Calculate cost of using qsort optional merge sort for resolving filesort.
  There are two options: using addon fields or not
*/

void Sort_costs::compute_merge_sort_costs(Sort_param *param,
                                          ha_rows num_rows,
                                          size_t memory_available,
                                          bool with_addon_fields)
{
  size_t row_length= param->sort_length + param->ref_length + sizeof(char *);
  size_t num_available_keys= memory_available / row_length;

  costs[MERGE_SORT_ALL_FIELDS]= DBL_MAX;
  costs[MERGE_SORT_ORDER_BY_FIELDS]= DBL_MAX;

  if (num_available_keys)
  {
    handler *file= param->sort_form->file;
    costs[MERGE_SORT_ORDER_BY_FIELDS]=
      get_merge_many_buffs_cost_fast(num_rows, num_available_keys,
                                     row_length, DEFAULT_KEY_COMPARE_COST,
                                     default_optimizer_costs.disk_read_cost,
                                     false) +
      file->cost(file->ha_rnd_pos_call_time(MY_MIN(param->limit_rows, num_rows)));
  }
  if (with_addon_fields)
  {
    /* Compute cost of merge sort *if* we strip addon fields. */
    row_length= param->rec_length + sizeof(char *);
    num_available_keys= memory_available / row_length;

    if (num_available_keys)
      costs[MERGE_SORT_ALL_FIELDS]=
        get_merge_many_buffs_cost_fast(num_rows, num_available_keys,
                                       row_length, DEFAULT_KEY_COMPARE_COST,
                                       DISK_READ_COST_THD(thd),
                                       true);
  }

  /*
     TODO(cvicentiu) we do not handle dynamic length fields yet.
     The code should decide here if the format is FIXED length or DYNAMIC
     and fill in the appropriate costs.
  */
}

void Sort_costs::compute_sort_costs(Sort_param *param, ha_rows num_rows,
                                    size_t memory_available,
                                    bool with_addon_fields)
{
  compute_pq_sort_costs(param, num_rows, memory_available,
                        with_addon_fields);
  compute_merge_sort_costs(param, num_rows, memory_available,
                           with_addon_fields);
  compute_fastest_sort();
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
      radixsort_is_applicable(count, param->sort_length) &&
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


static
size_t get_sort_length(THD *thd, Item *item)
{
  SORT_FIELD_ATTR sort_attr;
  sort_attr.type= (item->type_handler()->is_packable() ?
                   SORT_FIELD_ATTR::VARIABLE_SIZE :
                   SORT_FIELD_ATTR::FIXED_SIZE);
  item->type_handler()->sort_length(thd, item, &sort_attr);

  return sort_attr.length + (item->maybe_null() ? 1 : 0);
}


/**
   Calculate the cost of doing a filesort

   @param table           Table to sort
   @param Order_by        Fields to sort
   @param rows_to_read    Number of rows to be sorted
   @param limit_rows      Number of rows in result (when using limit)
   @param used_sort_type  Set to the sort algorithm used

   @result cost of sorting
*/


double cost_of_filesort(TABLE *table, ORDER *order_by, ha_rows rows_to_read,
                        ha_rows limit_rows, enum sort_type *used_sort_type)
{
  THD *thd= table->in_use;
  Sort_costs costs;
  Sort_param param;
  size_t memory_available= (size_t) thd->variables.sortbuff_size;
  uint sort_len= 0;
  uint addon_field_length, num_addon_fields, num_nullable_fields;
  uint packable_length;
  bool with_addon_fields;

  for (ORDER *ptr= order_by; ptr ; ptr= ptr->next)
  {
    size_t length= get_sort_length(thd, *ptr->item);
    set_if_smaller(length, thd->variables.max_sort_length);
    sort_len+= (uint) length;
  }

  with_addon_fields=
    filesort_use_addons(table, sort_len, &addon_field_length,
                        &num_addon_fields, &num_nullable_fields,
                        &packable_length);

  /* Fill in the Sort_param structure so we can compute the sort costs */
  param.setup_lengths_and_limit(table, sort_len, addon_field_length,
                                limit_rows);

  costs.compute_sort_costs(&param, rows_to_read, memory_available,
                           with_addon_fields);

  *used_sort_type= costs.fastest_sort;
  return costs.lowest_cost;
}
