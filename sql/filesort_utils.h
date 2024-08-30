/* Copyright (c) 2010, 2012 Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef FILESORT_UTILS_INCLUDED
#define FILESORT_UTILS_INCLUDED

#include "my_global.h"
#include "my_base.h"
#include "sql_array.h"
#include "handler.h"

class Sort_param;

/**
  Calculate cost of merge sort

    @param num_rows            Total number of rows.
    @param num_keys_per_buffer Number of keys per buffer.
    @param elem_size           Size of each element.
    @param key_compare_cost    Cost to compare two keys during QSort & merge

    Calculates cost of merge sort by simulating call to merge_many_buff().

  @retval
    Computed cost of merge sort in disk seeks.

  @note
    Declared here in order to be able to unit test it,
    since library dependencies have not been sorted out yet.

    See also comments get_merge_many_buffs_cost().
*/
double get_merge_many_buffs_cost_fast(ha_rows num_rows,
                                      ha_rows num_keys_per_buffer,
                                      size_t elem_size,
                                      double compare_cost,
                                      bool with_addon_fields);



/**
  These are the current sorting algorithms we compute cost for:

  PQ_SORT_ALL_FIELDS      Sort via priority queue, with addon fields.
  PQ_SORT_ORDER_BY_FIELDS Sort via priority queue, without addon fields.

  MERGE_SORT_ALL_FIELDS      Sort via merge sort, with addon fields.
  MERGE_SORT_ORDER_BY_FIELDS Sort via merge sort, without addon fields.

  Note:
  There is the possibility to do merge-sorting with dynamic length fields.
  This is more expensive than if there are only fixed length fields,
  however we do not (yet) account for that extra cost. We can extend the
  cost computation in the future to cover that case as well.

  Effectively there are 4 possible combinations for merge sort:
    With/without addon fields
    With/without dynamic length fields.
*/

enum sort_type
{
  PQ_SORT_ALL_FIELDS= 0,
  PQ_SORT_ORDER_BY_FIELDS,
  MERGE_SORT_ALL_FIELDS,
  MERGE_SORT_ORDER_BY_FIELDS,

  NO_SORT_POSSIBLE_OUT_OF_MEM,                  /* In case of errors */
  FINAL_SORT_TYPE= NO_SORT_POSSIBLE_OUT_OF_MEM
};

struct Sort_costs
{
  Sort_costs() :
    fastest_sort(NO_SORT_POSSIBLE_OUT_OF_MEM), lowest_cost(DBL_MAX) {}

  void compute_sort_costs(Sort_param *param, ha_rows num_rows,
                          size_t memory_available,
                          bool with_addon_fields);

  /* Cache value for fastest_sort. */
  enum sort_type fastest_sort;
  /* Cache value for lowest cost. */
  double lowest_cost;
private:
  /*
    Array to hold all computed costs.
    TODO(cvicentiu) This array is only useful for debugging. If it's not
    used in debugging code, it can be removed to reduce memory usage.
  */
  double costs[FINAL_SORT_TYPE];

  void compute_pq_sort_costs(Sort_param *param, ha_rows num_rows,
                             size_t memory_available,
                             bool with_addon_fields);
  void compute_merge_sort_costs(Sort_param *param, ha_rows num_rows,
                                size_t memory_available,
                                bool with_addon_fields);
  void compute_fastest_sort();
};

/**
  A wrapper class around the buffer used by filesort().
  The sort buffer is a contiguous chunk of memory,
  containing both records to be sorted, and pointers to said records:

  <start of buffer     |  still unused  |                      end of buffer>
  | rec0 | rec1 | rec2 |  ............  |ptr to rec2|ptr to rec1|ptr to rec0|

  Records will be inserted "left-to-right". Records are not necessarily
  fixed-size, they can be packed and stored without any "gaps".

  Record pointers will be inserted "right-to-left", as a side-effect
  of inserting the actual records.

  We wrap the buffer in order to be able to do lazy initialization of the
  pointers: the buffer is often much larger than what we actually need.

  With this allocation scheme, and lazy initialization of the pointers,
  we are able to pack variable-sized records in the buffer,
  and thus possibly have space for more records than we initially estimated.

  The buffer must be kept available for multiple executions of the
  same sort operation, so we have explicit allocate and free functions,
  rather than doing alloc/free in CTOR/DTOR.
*/

class Filesort_buffer
{
public:
  Filesort_buffer() :
    m_next_rec_ptr(NULL), m_rawmem(NULL), m_record_pointers(NULL),
    m_sort_keys(NULL),
    m_num_records(0), m_record_length(0),
    m_sort_length(0),
    m_size_in_bytes(0), m_idx(0)
  {}

  /** Sort me... */
  void sort_buffer(const Sort_param *param, uint count);

  /**
    Reverses the record pointer array, to avoid recording new results for
    non-deterministic mtr tests.
  */
  void reverse_record_pointers()
  {
    if (m_idx < 2) // There is nothing to swap.
      return;
    uchar **keys= get_sort_keys();
    const longlong count= m_idx - 1;
    for (longlong ix= 0; ix <= count/2; ++ix)
    {
      uchar *tmp= keys[count - ix];
      keys[count - ix] = keys[ix];
      keys[ix]= tmp;
    }
  }

  /**
    Initializes all the record pointers.
  */
  void init_record_pointers()
  {
    init_next_record_pointer();
    while (m_idx < m_num_records)
      (void) get_next_record_pointer();
    reverse_record_pointers();
  }

  /**
    Prepares the buffer for the next batch of records to process.
   */
  void init_next_record_pointer()
  {
    m_idx= 0;
    m_next_rec_ptr= m_rawmem;
    m_sort_keys= NULL;
  }

  /**
    @returns the number of bytes currently in use for data.
   */
  size_t space_used_for_data() const
  {
    return m_next_rec_ptr ? m_next_rec_ptr - m_rawmem : 0;
  }

  /**
    @returns the number of bytes left in the buffer.
  */
  size_t spaceleft() const
  {
    DBUG_ASSERT(m_next_rec_ptr >= m_rawmem);
    const size_t spaceused=
      (m_next_rec_ptr - m_rawmem) +
      (static_cast<size_t>(m_idx) * sizeof(uchar*));
    return m_size_in_bytes - spaceused;
  }

  /**
    Is the buffer full?
  */
  bool isfull() const
  {
    if (m_idx < m_num_records)
      return false;
    return spaceleft() < (m_record_length + sizeof(uchar*));
  }

  /**
    Where should the next record be stored?
   */
  uchar *get_next_record_pointer()
  {
    uchar *retval= m_next_rec_ptr;
    // Save the return value in the record pointer array.
    m_record_pointers[-m_idx]= m_next_rec_ptr;
    // Prepare for the subsequent request.
    m_idx++;
    m_next_rec_ptr+= m_record_length;
    return retval;
  }

  /**
    Adjusts for actual record length. get_next_record_pointer() above was
    pessimistic, and assumed that the record could not be packed.
   */
  void adjust_next_record_pointer(uint val)
  {
    m_next_rec_ptr-= (m_record_length - val);
  }

  /// Returns total size: pointer array + record buffers.
  size_t sort_buffer_size() const
  {
    return m_size_in_bytes;
  }

  bool is_allocated() const
  {
    return m_rawmem;
  }

  /**
    Allocates the buffer, but does *not* initialize pointers.
    Total size = (num_records * record_length) + (num_records * sizeof(pointer))
                  space for records               space for pointer to records
    Caller is responsible for raising an error if allocation fails.

    @param num_records   Number of records.
    @param record_length (maximum) size of each record.
    @returns Pointer to allocated area, or NULL in case of out-of-memory.
  */
  uchar *alloc_sort_buffer(uint num_records, uint record_length);

  /// Frees the buffer.
  void free_sort_buffer();

  void reset()
  {
    m_rawmem= NULL;
  }
  /**
    Used to access the "right-to-left" array of record pointers as an ordinary
    "left-to-right" array, so that we can pass it directly on to std::sort().
  */
  uchar **get_sort_keys()
  {
    if (m_idx == 0)
      return NULL;
    return &m_record_pointers[1 - m_idx];
  }

  /**
    Gets sorted record number ix. @see get_sort_keys()
    Only valid after buffer has been sorted!
  */
  uchar *get_sorted_record(uint ix)
  {
    return m_sort_keys[ix];
  }

  /**
    @returns The entire buffer, as a character array.
    This is for reusing the memory for merge buffers.
   */
  Bounds_checked_array<uchar> get_raw_buf()
  {
    return Bounds_checked_array<uchar>(m_rawmem, m_size_in_bytes);
  }

  /**
    We need an assignment operator, see filesort().
    This happens to have the same semantics as the one that would be
    generated by the compiler.
    Note that this is a shallow copy. We have two objects sharing the same
    array.
  */
  Filesort_buffer &operator=(const Filesort_buffer &rhs) = default;

  uint get_sort_length() const { return m_sort_length; }
  void set_sort_length(uint val) { m_sort_length= val; }

private:
  uchar  *m_next_rec_ptr;    /// The next record will be inserted here.
  uchar  *m_rawmem;          /// The raw memory buffer.
  uchar **m_record_pointers; /// The "right-to-left" array of record pointers.
  uchar **m_sort_keys;       /// Caches the value of get_sort_keys()
  uint    m_num_records;     /// Saved value from alloc_sort_buffer()
  uint    m_record_length;   /// Saved value from alloc_sort_buffer()
  uint    m_sort_length;     /// The length of the sort key.
  size_t  m_size_in_bytes;   /// Size of raw buffer, in bytes.

  /**
    This is the index in the "right-to-left" array of the next record to
    be inserted into the buffer. It is signed, because we use it in signed
    expressions like:
        m_record_pointers[-m_idx];
    It is longlong rather than int, to ensure that it covers UINT_MAX32
    without any casting/warning.
  */
  longlong m_idx;
};

/* Names for sort_type */
extern const LEX_CSTRING filesort_names[];

double cost_of_filesort(TABLE *table, ORDER *order_by, ha_rows rows_to_read,
                        ha_rows limit_rows, enum sort_type *used_sort_type);

double get_qsort_sort_cost(ha_rows num_rows, bool with_addon_fields);
int compare_packed_sort_keys(void *sort_keys, unsigned char **a,
                             unsigned char **b);
qsort2_cmp get_packed_keys_compare_ptr();
#endif  // FILESORT_UTILS_INCLUDED
