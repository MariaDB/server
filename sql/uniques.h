/* Copyright (c) 2016 MariaDB corporation

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

#ifndef UNIQUE_INCLUDED
#define UNIQUE_INCLUDED

#include "filesort.h"

/*
   Unique -- class for unique (removing of duplicates).
   Puts all values to the TREE. If the tree becomes too big,
   it's dumped to the file. User can request sorted values, or
   just iterate through them. In the last case tree merging is performed in
   memory simultaneously with iteration, so it should be ~2-3x faster.
 */

class Unique :public Sql_alloc
{
  DYNAMIC_ARRAY file_ptrs;
  ulong max_elements;   /* Total number of elements that will be stored in-memory */
  size_t max_in_memory_size;
  IO_CACHE file;
  TREE tree;
 /* Number of elements filtered out due to min_dupl_count when storing results
    to table. See Unique::get */
  ulong filtered_out_elems;
  uint size;

  uint full_size;   /* Size of element + space needed to store the number of
                       duplicates found for the element. */
  uint min_dupl_count;   /* Minimum number of occurences of element required for
                            it to be written to record_pointers.
                            always 0 for unions, > 0 for intersections */
  bool with_counters;
  /*
    size in bytes used for storing keys in the Unique tree
  */
  size_t memory_used;

  bool merge(TABLE *table, uchar *buff, size_t size, bool without_last_merge);
  bool flush();

  // return the amount of unused memory in the Unique tree
  size_t space_left()
  {
    DBUG_ASSERT(max_in_memory_size >= memory_used);
    return max_in_memory_size - memory_used;
  }

  // Check if the Unique tree is full or not
  bool is_full(size_t record_size)
  {
    if (!tree.elements_in_tree)  // Atleast insert one element in the tree
      return false;
    return record_size > space_left();
  }

public:
  ulong elements;
  SORT_INFO sort;
  Unique(qsort_cmp2 comp_func, void *comp_func_fixed_arg,
         uint size_arg, size_t max_in_memory_size_arg,
         uint min_dupl_count_arg= 0);
  virtual ~Unique();
  ulong elements_in_tree() { return tree.elements_in_tree; }

  /*
    @brief
      Add a record to the Unique tree
    @param
      ptr                      key value
      size                     length of the key
  */

  inline bool unique_add(void *ptr, uint size_arg)
  {
    DBUG_ENTER("unique_add");
    DBUG_PRINT("info", ("tree %u - %lu", tree.elements_in_tree, max_elements));
    TREE_ELEMENT *res;
    size_t rec_size= size_arg + sizeof(TREE_ELEMENT) + tree.size_of_element;

    if (!(tree.flag & TREE_ONLY_DUPS) && is_full(rec_size) && flush())
      DBUG_RETURN(1);
    uint count= tree.elements_in_tree;
    res= tree_insert(&tree, ptr, size_arg, tree.custom_arg);
    if (tree.elements_in_tree != count)
    {
      /*
        increment memory used only when a unique element is inserted
        in the tree
      */
      memory_used+= rec_size;
    }
    DBUG_RETURN(!res);
  }

  bool is_in_memory() { return (my_b_tell(&file) == 0); }
  void close_for_expansion() { tree.flag= TREE_ONLY_DUPS; }

  bool get(TABLE *table);
  
  /* Cost of searching for an element in the tree */
  inline static double get_search_cost(ulonglong tree_elems,
                                       double compare_factor)
  {
    return log((double) tree_elems) / (compare_factor * M_LN2);
  }  

  static double get_use_cost(uint *buffer, size_t nkeys, uint key_size,
                             size_t max_in_memory_size, double compare_factor,
                             bool intersect_fl, bool *in_memory);
  inline static int get_cost_calc_buff_size(size_t nkeys, uint key_size,
                                            size_t max_in_memory_size)
  {
    size_t max_elems_in_tree=
      max_in_memory_size / ALIGN_SIZE(sizeof(TREE_ELEMENT)+key_size);

    if (max_elems_in_tree == 0)
      max_elems_in_tree= 1;
    return (int) (sizeof(uint)*(1 + nkeys/max_elems_in_tree));
  }

  void reset();
  bool walk(TABLE *table, tree_walk_action action, void *walk_action_arg);

  uint get_size() const { return size; }
  uint get_full_size() const { return full_size; }
  size_t get_max_in_memory_size() const { return max_in_memory_size; }
  bool is_count_stored() { return with_counters; }
  IO_CACHE *get_file ()  { return &file; }
  virtual uchar *get_packed_rec_ptr()
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  virtual Sort_keys *get_keys()
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  virtual SORT_FIELD *get_sortorder()
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  virtual bool setup(THD *thd, Item_sum *item, uint non_const_args,
                     uint arg_count, bool exclude_nulls)
  {
    return false;
  }

  virtual bool setup(THD *thd, Field *field)
  {
    return false;
  }

  virtual int compare_packed_keys(uchar *a, uchar *b)
  {
    DBUG_ASSERT(0);
    return 0;
  }

  virtual uint get_length(uchar *ptr, bool exclude_nulls)
  {
    return size;
  }
  virtual int write_record_to_file(uchar *key);

  // returns TRUE if the unique tree stores packed values
  virtual bool is_packed() { return false; }

  friend int unique_write_to_file(uchar* key, element_count count, Unique *unique);
  friend int unique_write_to_ptrs(uchar* key, element_count count, Unique *unique);

  friend int unique_write_to_file_with_count(uchar* key, element_count count,
                                             Unique *unique);
  friend int unique_intersect_write_to_ptrs(uchar* key, element_count count,
				            Unique *unique);
};


class Unique_packed : public Unique
{
  /*
    Packed record ptr for a record of the table, the packed value in this
    record is added to the unique tree
  */
  uchar* packed_rec_ptr;

  /*
    Array of SORT_FIELD structure storing the information about the key parts
    in the sort key of the Unique tree
    @see Unique::setup()
  */
  SORT_FIELD *sortorder;

  /*
    Structure storing information about usage of keys
  */
  Sort_keys *sort_keys;

  public:
  Unique_packed(qsort_cmp2 comp_func, void *comp_func_fixed_arg,
                uint size_arg, size_t max_in_memory_size_arg,
                uint min_dupl_count_arg);

  ~Unique_packed();
  bool is_packed() { return true; }
  uchar *get_packed_rec_ptr() { return packed_rec_ptr; }
  Sort_keys *get_keys() { return sort_keys; }
  SORT_FIELD *get_sortorder() { return sortorder; }
  bool setup(THD *thd, Item_sum *item, uint non_const_args,
             uint arg_count, bool exclude_nulls);
  bool setup(THD *thd, Field *field);
  int compare_packed_keys(uchar *a, uchar *b);
  int write_record_to_file(uchar *key);
  static void store_packed_length(uchar *p, uint sz)
  {
    int4store(p, sz - size_of_length_field);
  }

  // returns the length of the key along with the length bytes for the key
  static uint read_packed_length(uchar *p)
  {
    return size_of_length_field + uint4korr(p);
  }

  static const uint size_of_length_field= 4;
};




#endif /* UNIQUE_INCLUDED */
