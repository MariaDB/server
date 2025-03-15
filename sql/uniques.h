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

#include "mariadb.h"
#include "field.h"
#include "filesort.h"
#include "my_tree.h"
#include "sql_string.h"


/*

  Keys_descriptor class storing information about the keys that would be
  inserted in the Unique tree. This is an abstract class which is
  extended by other class to support descriptors for keys with fixed and
  variable size.
*/

class Keys_descriptor : public Sql_alloc
{
protected:

  /* maximum possible size of any key, in bytes */
  uint max_length;
  enum attributes
  {
    FIXED_SIZED_KEYS= 0,
    VARIABLE_SIZED_KEYS
  } keys_type;

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
  virtual ~Keys_descriptor() {};
  virtual uint get_length_of_key(uchar *ptr) = 0;
  bool is_variable_sized() { return keys_type == VARIABLE_SIZED_KEYS; }
  virtual int compare_keys(const uchar *a, const uchar *b) const = 0;

  // Fill structures like sort_keys, sortorder
  virtual bool setup_for_item(THD *thd, Item_sum *item,
                              uint non_const_args, uint arg_count);
  virtual bool setup_for_field(THD *thd, Field *field);

  uint get_max_key_length() const { return max_length; };
  Sort_keys *get_keys() { return sort_keys; }
  SORT_FIELD *get_sortorder() { return sortorder; }

  /*
    Create a keys record according to this descriptor's storage format.

    The data is taken from the SORT_FIELD. The SORT_FIELD's values (underlying
    table::record[0] must be set before calling this function.

    The record is then to be used within a Unique object to store the result.
  */
  virtual uchar* create_keys_record(uchar *original_record) = 0;
  virtual bool init(THD *thd, uint count);
protected:
  bool setup_for_item_impl(THD *thd, Item_sum *item,
                           uint non_const_args, uint arg_count,
                           bool is_mem_comparable);
  bool setup_for_field_impl(THD *thd, Field *field, bool is_mem_comparable);
};


/*
  Keys_descriptor for fixed size keys with single key part
*/

class Fixed_size_keys_descriptor : public Keys_descriptor
{
public:
  Fixed_size_keys_descriptor(uint length);
  virtual ~Fixed_size_keys_descriptor() {}
  uint get_length_of_key(uchar *ptr) override { return max_length; }
  int compare_keys(const uchar *a, const uchar *b) const override;
  uchar* create_keys_record(uchar *original_record) override
  {
    return original_record;
  }
};


/*
  Keys_descriptor for fixed size mem-comparable keys with single key part
*/
class Fixed_size_keys_mem_comparable: public Fixed_size_keys_descriptor
{
public:
  Fixed_size_keys_mem_comparable(uint length)
    :Fixed_size_keys_descriptor(length) {}
  ~Fixed_size_keys_mem_comparable() {}
  int compare_keys(const uchar *a, const uchar *b) const override;
};


/*
  Keys_descriptor for fixed size keys for rowid comparison
*/
class Fixed_size_keys_for_rowids: public Fixed_size_keys_descriptor
{
private:
  handler *file;

public:
  Fixed_size_keys_for_rowids(handler *file_arg)
    :Fixed_size_keys_descriptor(file_arg->ref_length), file(file_arg)
  {}
  ~Fixed_size_keys_for_rowids() {}
  int compare_keys(const uchar *a, const uchar *b) const override;
};


/*
  Keys_descriptor for fixed size keys where a key part can be NULL
  Used currently in JSON_ARRAYAGG
*/

class Fixed_size_keys_descriptor_with_nulls : public Fixed_size_keys_descriptor
{
public:
  Fixed_size_keys_descriptor_with_nulls(uint length)
    : Fixed_size_keys_descriptor(length) {}
  ~Fixed_size_keys_descriptor_with_nulls() {}
  int compare_keys(const uchar *a, const uchar *b) const override;
};


/*
  Keys_descriptor for fixed size keys in group_concat
*/
class Fixed_size_keys_for_gconcat : public Fixed_size_keys_descriptor
{
public:
  Fixed_size_keys_for_gconcat(uint length)
    : Fixed_size_keys_descriptor(length) {}
  ~Fixed_size_keys_for_gconcat() {}
  int compare_keys(const uchar *a, const uchar *b) const override;
};


/*
  Base class for the descriptor for variable size keys
*/
class Variable_size_keys_descriptor : public Keys_descriptor
{
protected:
  uchar *rec_buf;
  String tmp_buffer;
public:
  Variable_size_keys_descriptor(uint length);
  ~Variable_size_keys_descriptor();
  uint get_length_of_key(uchar *ptr) override
  {
    return read_packed_length(ptr);
  }

  // All need to be moved to some new class
  // returns the length of the key along with the length bytes for the key
  static uint read_packed_length(const uchar *p)
  {
    return SIZE_OF_LENGTH_FIELD + uint4korr(p);
  }
  static void store_packed_length(uchar *p, uint sz)
  {
    int4store(p, sz - SIZE_OF_LENGTH_FIELD);
  }
  static const uint SIZE_OF_LENGTH_FIELD= 4;
  int compare_keys(const uchar *a, const uchar *b) const override;
  uchar* create_keys_record(uchar *orig_record) override;
  bool init(THD *thd, uint count) override;
};


/*
  Keys_descriptor for variable sized keys for GROUP_CONCAT

  This class is purposefully coded for group_concat.
  GROUP_CONCAT(DISTINCT <cols>) uses the Unique object to keep a record of all
  distinct values and also creates it's val_str value from the same Unique
  object.

  This means that if we are to store mem-comparable versions of keys, we are
  unable to use the same record to output val_str. This is why for GROUP_CONCAT
  we store the original version of the key.

  To preserve backwards compatibility, we specifically force the use of
  Field::cmp function, by marking SORT_FIELD's "is_mem_comparable" field to
  false, so that the output when calling Unique::walk is still sorted.
*/
class Variable_size_key_desc_for_gconcat : public Variable_size_keys_descriptor
{
public:
  Variable_size_key_desc_for_gconcat(uint length);
  ~Variable_size_key_desc_for_gconcat() {}
  /* This crates a "packed key", not a "packed sort key". */
  uchar* create_keys_record(uchar *orig_record) override;
  bool setup_for_item(THD *thd, Item_sum *item,
                      uint non_const_args,
                      uint arg_count) override;
  bool setup_for_field(THD *thd, Field *field) override;
};

/*
   Unique -- class for unique (removing of duplicates).
   Puts all values to the TREE. If the tree becomes too big,
   it's dumped to the file. User can request sorted values, or
   just iterate through them. In the last case tree merging is performed in
   memory simultaneously with iteration, so it should be ~2-3x faster.
*/
class Unique : public Sql_alloc
{
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

  const uint full_size;   /* Size of element + space needed to store the number of
                             duplicates found for the element. */
  const uint min_dupl_count; /* Minimum number of occurrences of element
                                required for it to be written to
                                record_pointers.
                                always 0 for unions, > 0 for intersections */
  const bool with_counters;

  // size in bytes used for storing keys in the Unique tree
  size_t memory_used;
  ulong elements;
  SORT_INFO sort;

  /*
    Storing all meta-data information of the expressions whose value are
    being added to the Unique tree
  */
  Keys_descriptor *keys_descriptor;

  bool merge(TABLE *table, uchar *buff, size_t size, bool without_last_merge);
  bool flush();

  // return the amount of unused memory in the Unique tree
  size_t space_left() const
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

  bool unique_add(void *ptr, uint key_size)
  {
    DBUG_ENTER("unique_add");
    DBUG_PRINT("info", ("tree %u - %lu", tree.elements_in_tree, max_elements));
    TREE_ELEMENT *res;
    size_t rec_size= key_size + sizeof(TREE_ELEMENT) + tree.size_of_element;

    if (!(tree.flag & TREE_ONLY_DUPS) && is_full(rec_size) && flush())
      DBUG_RETURN(1);
    uint count= tree.elements_in_tree;
    res= tree_insert(&tree, ptr, key_size, tree.custom_arg);
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
  ulong get_n_elements()
  {
    return is_in_memory() ? elements_in_tree() : elements;
  }

  SORT_INFO *get_sort() { return &sort; }

  Unique(Keys_descriptor *desc,
         size_t max_in_memory_size_arg,
         uint min_dupl_count_arg);
  ~Unique();
  ulong elements_in_tree() { return tree.elements_in_tree; }

  bool unique_add(uchar *ptr)
  {
    uchar *rec_ptr= keys_descriptor->create_keys_record(ptr);
    DBUG_ASSERT(rec_ptr);
    DBUG_ASSERT(keys_descriptor->get_length_of_key(rec_ptr) <= size);
    return unique_add(rec_ptr, keys_descriptor->get_length_of_key(rec_ptr));
  }

  bool is_in_memory() { return (my_b_tell(&file) == 0); }
  void close_for_expansion() { tree.flag= TREE_ONLY_DUPS; }

  bool get(TABLE *table);

  /* Cost of searching for an element in the tree */
  inline static double get_search_cost(ulonglong tree_elems,
                                       double compare_factor)
  {
    return log((double) tree_elems) * compare_factor / M_LN2;
  }

  static double get_use_cost(THD *thd, uint *buffer, size_t nkeys, uint key_size,
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
  bool is_count_stored() { return with_counters; }
  int write_record_to_file(uchar *key);
  size_t get_max_in_memory_size() const { return max_in_memory_size; }


  IO_CACHE *get_file()  { return &file; }
  // returns TRUE if the unique tree stores packed values
  bool is_variable_sized() { return keys_descriptor->is_variable_sized(); }

  int compare_keys(const uchar *a, const uchar *b) const
  { return keys_descriptor->compare_keys(a, b); }
  SORT_FIELD *get_sortorder() { return keys_descriptor->get_sortorder(); }


  bool setup_for_field(THD *thd, Field *field)
  { return keys_descriptor->setup_for_field(thd, field); }

  bool setup_for_item(THD *thd, Item_sum *item,
                      uint non_const_args, uint arg_count)
  { return keys_descriptor->setup_for_item(thd, item, non_const_args, arg_count); }

private:
  static int unique_write_to_file(void *key_, element_count count,
                                  void *unique_);
  static int unique_write_to_file_with_count(void* key_, element_count count,
                                             void *unique_);
  static int unique_write_to_ptrs(void* key_, element_count count,
                                  void *unique_);
  static int unique_intersect_write_to_ptrs(void* key_, element_count count,
                                            void *unique_);
  static int unique_compare_keys(void *arg, const void *key1, const void *key2);
};

#endif /* UNIQUE_INCLUDED */
