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
  Encode a key into a particular format. The format depends whether
  the key is of fixed size or variable size.

  @notes
    Currently this encoding is only done for variable size keys
*/

class Key_encoder
{
protected:
  /*
    Packed record ptr for a record of the table, the packed value in this
    record is added to the unique tree
  */
  uchar* rec_ptr;

  String tmp_buffer;
public:
  virtual ~Key_encoder();
  virtual uchar* make_record(Sort_keys *keys, bool exclude_nulls) = 0;
  bool init(uint length);
  uchar *get_rec_ptr() { return rec_ptr; }
};


class Key_encoder_for_variable_size_key : public Key_encoder
{
public:
  Key_encoder_for_variable_size_key()
  {
    rec_ptr= NULL;
  }
  virtual ~Key_encoder_for_variable_size_key() {}
  uchar* make_record(Sort_keys *keys, bool exclude_nulls) override;
};


class Key_encoder_for_group_concat : public Key_encoder_for_variable_size_key
{
public:
  Key_encoder_for_group_concat() : Key_encoder_for_variable_size_key(){}
  ~Key_encoder_for_group_concat() {}
  uchar* make_record(Sort_keys *keys, bool exclude_nulls) override;
};


/*
  Descriptor class storing information about the keys that would be
  inserted in the Unique tree. This is an abstract class which is
  extended by other class to support descriptors for keys with fixed and
  variable size.
*/

class Descriptor : public Sql_alloc
{
protected:

  /* maximum possible size of any key, in bytes */
  uint max_length;
  enum attributes
  {
    FIXED_SIZED_KEYS= 0,
    VARIABLE_SIZED_KEYS
  };

  /*
    Storing information about the attributes for the keys
    Each bit for the flag points to an attribute
    Currently only 2 bits are used, so the remaining bit can be used
    in the future if some extension is required for descriptors
  */
  uint flags;
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
  virtual ~Descriptor() {};
  virtual uint get_length_of_key(uchar *ptr) = 0;
  bool is_variable_sized()
  {
    return flags & (1 << VARIABLE_SIZED_KEYS);
  }
  virtual int compare_keys(uchar *a, uchar *b) = 0;

  // Fill structures like sort_keys, sortorder
  virtual bool setup_for_item(THD *thd, Item_sum *item,
                              uint non_const_args, uint arg_count)
  { return false; }
  virtual bool setup_for_field(THD *thd, Field *field) { return false; }

  inline Sort_keys *get_keys() { return sort_keys; }
  inline SORT_FIELD *get_sortorder() { return sortorder; }

  virtual bool is_single_arg() = 0;
  virtual bool init(THD *thd, uint count);
};


/*
  Descriptor for fixed size keys with single key part
*/

class Fixed_size_keys_descriptor : public Descriptor
{
public:
  Fixed_size_keys_descriptor(uint length);
  virtual ~Fixed_size_keys_descriptor() {}
  uint get_length_of_key(uchar *ptr) override { return max_length; }
  bool setup_for_field(THD *thd, Field *field);
  bool setup_for_item(THD *thd, Item_sum *item,
                      uint non_const_args, uint arg_count);
  int compare_keys(uchar *a, uchar *b) override;
  virtual bool is_single_arg() override { return true; }
};


/*
  Descriptor for fixed size mem-comparable keys with single key part
*/
class Fixed_size_keys_mem_comparable: public Fixed_size_keys_descriptor
{
public:
  Fixed_size_keys_mem_comparable(uint length)
    :Fixed_size_keys_descriptor(length){}
  ~Fixed_size_keys_mem_comparable() {}
  int compare_keys(uchar *a, uchar *b) override;
};


/*
  Descriptor for fixed size keys for rowid comparison

  Used by index merge and delete queries
*/
class Fixed_size_keys_for_rowids: public Fixed_size_keys_descriptor
{
private:
  handler *file;

public:
  Fixed_size_keys_for_rowids(handler *file_arg)
    :Fixed_size_keys_descriptor(file_arg->ref_length)
  {
    file= file_arg;
  }
  ~Fixed_size_keys_for_rowids() {}
  int compare_keys(uchar *a, uchar *b) override;
};


/*
  Descriptor for fixed size keys where a key part can be NULL
  Used currently in JSON_ARRAYAGG
*/

class Fixed_size_keys_descriptor_with_nulls : public Fixed_size_keys_descriptor
{
public:
  Fixed_size_keys_descriptor_with_nulls(uint length)
    : Fixed_size_keys_descriptor(length) {}
  ~Fixed_size_keys_descriptor_with_nulls() {}
  int compare_keys(uchar *a, uchar *b) override;
};


/*
  Descriptor for fixed size keys in group_concat
*/
class Fixed_size_keys_for_group_concat : public Fixed_size_keys_descriptor
{
public:
  Fixed_size_keys_for_group_concat(uint length)
    : Fixed_size_keys_descriptor(length) {}
  ~Fixed_size_keys_for_group_concat() {}
  int compare_keys(uchar *a, uchar *b) override;
};


/*
  Descriptor for fixed size keys with multiple key parts
*/

class Fixed_size_composite_keys_descriptor : public Fixed_size_keys_descriptor
{
public:
  Fixed_size_composite_keys_descriptor(uint length)
    : Fixed_size_keys_descriptor(length) {}
  ~Fixed_size_composite_keys_descriptor() {}
  int compare_keys(uchar *a, uchar *b) override;
  bool is_single_arg() override { return false; }
};


/*
  Base class for the descriptor for variable size keys
*/

class Variable_size_keys_descriptor : public Descriptor
{
public:
  Variable_size_keys_descriptor(uint length);
  virtual ~Variable_size_keys_descriptor() {}
  uint get_length_of_key(uchar *ptr) override
  {
    return read_packed_length(ptr);
  }
  int compare_keys(uchar *a, uchar *b) override { return 0; }
  bool is_single_arg() override { return false; }

  virtual bool setup_for_item(THD *thd, Item_sum *item,
                              uint non_const_args, uint arg_count) override;
  virtual bool setup_for_field(THD *thd, Field *field) override;

  // All need to be moved to some new class
  // returns the length of the key along with the length bytes for the key
  static uint read_packed_length(uchar *p)
  {
    return size_of_length_field + uint4korr(p);
  }
  static void store_packed_length(uchar *p, uint sz)
  {
    int4store(p, sz - size_of_length_field);
  }
  static const uint size_of_length_field= 4;
};


/*
  Descriptor for variable size keys with only one component

  Used by EITS, JSON_ARRAYAGG.
  COUNT(DISTINCT col) AND GROUP_CONCAT(DISTINCT col) are also allowed
  that the number of arguments with DISTINCT is 1.

  Comparisons happens only for packable fields with a collation specific
  function
*/

class Variable_size_keys_simple : public Variable_size_keys_descriptor
{
public:
  Variable_size_keys_simple(uint length)
    :Variable_size_keys_descriptor(length){}
  ~Variable_size_keys_simple() {}
  int compare_keys(uchar *a, uchar *b) override;
  bool is_single_arg() override { return true; }
};


/*
  Descriptor for variable sized keys with multiple key parts for
  COUNT(DISTINCT col1, col2 ....)

  Comparison happens in the following way:
  a) comparison for fixed fields happen with memcmp
  b) comparison for packable fields happen with collation specific comparison
     function
*/

class Variable_size_composite_key_desc : public Variable_size_keys_descriptor
{
public:
  Variable_size_composite_key_desc(uint length)
    : Variable_size_keys_descriptor(length){}
  ~Variable_size_composite_key_desc() {}
  int compare_keys(uchar *a, uchar *b) override;
};



/*
  Descriptor for variable sized keys with multiple key parts
  for GROUP_CONCAT

  Comparison happens in the following way:
  a) comparison for fixed fields happen with original values,
     so Field::cmp is used
  b) comparison for packable fields happen with collation specific comparison
     function
*/

class Variable_size_composite_key_desc_for_gconcat :
                                         public Variable_size_keys_descriptor
{
public:
  Variable_size_composite_key_desc_for_gconcat(uint length)
    : Variable_size_keys_descriptor(length) {}
  ~Variable_size_composite_key_desc_for_gconcat() {}
  int compare_keys(uchar *a, uchar *b) override;
  bool setup_for_item(THD *thd, Item_sum *item,
                      uint non_const_args, uint arg_count) override;
};


/*
   Unique -- An abstract class for unique (removing duplicates).
*/

class Unique : public Sql_alloc {

protected:

  /*
    Storing all meta-data information of the expressions whose value are
    being added to the Unique tree
  */
  Descriptor *m_descriptor;
public:

  virtual void reset() = 0;
  virtual bool unique_add(void *ptr) = 0;
  virtual ~Unique() {};

  virtual void close_for_expansion() = 0;

  virtual bool get(TABLE *table) = 0;
  virtual bool walk(TABLE *table, tree_walk_action action,
                    void *walk_action_arg)= 0;

  virtual SORT_INFO *get_sort() = 0;

  virtual ulong get_n_elements() = 0;
  virtual size_t get_max_in_memory_size() const = 0;
  virtual bool is_in_memory() = 0;

  virtual ulong elements_in_tree() = 0;
  Descriptor *get_descriptor() { return m_descriptor; }
};


/*
   Unique_impl -- class for unique (removing of duplicates).
   Puts all values to the TREE. If the tree becomes too big,
   it's dumped to the file. User can request sorted values, or
   just iterate through them. In the last case tree merging is performed in
   memory simultaneously with iteration, so it should be ~2-3x faster.
*/

class Unique_impl : public Unique {
  DYNAMIC_ARRAY file_ptrs;
  /* Total number of elements that will be stored in-memory */
  ulong max_elements;
  size_t max_in_memory_size;
  IO_CACHE file;
  TREE tree;
 /* Number of elements filtered out due to min_dupl_count when storing results
    to table. See Unique::get */
  ulong filtered_out_elems;
  uint size;

  uint full_size;   /* Size of element + space needed to store the number of
                       duplicates found for the element. */
  uint min_dupl_count;   /* Minimum number of occurrences of element required for
                            it to be written to record_pointers.
                            always 0 for unions, > 0 for intersections */
  bool with_counters;

  // size in bytes used for storing keys in the Unique tree
  size_t memory_used;
  ulong elements;
  SORT_INFO sort;

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

  /*
    @brief
      Add a record to the Unique tree
    @param
      ptr                      key value
      size                     length of the key
    @retval
      TRUE                     ERROE
      FALSE                    key successfully inserted in the Unique tree
  */

  bool unique_add(void *ptr, uint size_arg)
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

public:

  /*
    @brief
      Returns the number of elements in the unique instance

    @details
      If all the elements fit in the memory, then this returns all the
      distinct elements.
  */
  ulong get_n_elements() override
  {
    return is_in_memory() ? elements_in_tree() : elements;
  }

  SORT_INFO *get_sort() override { return &sort; }

  Unique_impl(qsort_cmp2 comp_func, void *comp_func_fixed_arg,
         uint size_arg, size_t max_in_memory_size_arg,
         uint min_dupl_count_arg, Descriptor *desc);
  ~Unique_impl();
  ulong elements_in_tree() { return tree.elements_in_tree; }

  bool unique_add(void *ptr) override
  {
    return unique_add(ptr, m_descriptor->get_length_of_key((uchar*)ptr));
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

  void reset() override;
  bool walk(TABLE *table, tree_walk_action action, void *walk_action_arg);

  uint get_size() const { return size; }
  uint get_full_size() const { return full_size; }
  size_t get_max_in_memory_size() const { return max_in_memory_size; }
  bool is_count_stored() { return with_counters; }
  IO_CACHE *get_file ()  { return &file; }
  int write_record_to_file(uchar *key);

  // returns TRUE if the unique tree stores packed values
  bool is_variable_sized() { return m_descriptor->is_variable_sized(); }

  // returns TRUE if the key to be inserted has only one component
  bool is_single_arg() { return m_descriptor->is_single_arg(); }
  Descriptor* get_descriptor() { return m_descriptor; }

  friend int unique_write_to_file(uchar* key, element_count count, Unique_impl *unique);
  friend int unique_write_to_ptrs(uchar* key, element_count count, Unique_impl *unique);

  friend int unique_write_to_file_with_count(uchar* key, element_count count,
                                             Unique_impl *unique);
  friend int unique_intersect_write_to_ptrs(uchar* key, element_count count,
                                            Unique_impl *unique);
};

#endif /* UNIQUE_INCLUDED */
