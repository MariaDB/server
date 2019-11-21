#ifndef ITEM_ROW_INCLUDED
#define ITEM_ROW_INCLUDED

/*
   Copyright (c) 2002, 2013, Oracle and/or its affiliates.

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

/**
  Row items used for comparing rows and IN operations on rows:

  @verbatim
  (a, b, c) > (10, 10, 30)
  (a, b, c) = (select c, d, e, from t1 where x=12)
  (a, b, c) IN ((1,2,2), (3,4,5), (6,7,8)
  (a, b, c) IN (select c, d, e, from t1)
  @endverbatim
*/


/**
   Item which stores (x,y,...) and ROW(x,y,...).
   Note that this can be recursive: ((x,y),(z,t)) is a ROW of ROWs.
*/
class Item_row: public Item,
                private Item_args,
                private Used_tables_and_const_cache
{
  table_map not_null_tables_cache;
  /**
    If elements are made only of constants, of which one or more are
    NULL. For example, this item is (1,2,NULL), or ( (1,NULL), (2,3) ).
  */
  bool with_null;
public:
  Item_row(THD *thd, List<Item> &list):
  Item(thd), Item_args(thd, list), not_null_tables_cache(0), with_null(0)
  { }
  Item_row(THD *thd, Item_row *row):
    Item(thd), Item_args(thd, static_cast<Item_args*>(row)), Used_tables_and_const_cache(),
    not_null_tables_cache(0), with_null(0)
  { }

  enum Type type() const { return ROW_ITEM; };
  void illegal_method_call(const char *);
  bool is_null() { return null_value; }
  void make_field(THD *thd, Send_field *)
  {
    illegal_method_call((const char*)"make_field");
  };
  double val_real()
  {
    illegal_method_call((const char*)"val");
    return 0;
  };
  longlong val_int()
  {
    illegal_method_call((const char*)"val_int");
    return 0;
  };
  String *val_str(String *)
  {
    illegal_method_call((const char*)"val_str");
    return 0;
  };
  my_decimal *val_decimal(my_decimal *)
  {
    illegal_method_call((const char*)"val_decimal");
    return 0;
  };
  bool fix_fields(THD *thd, Item **ref);
  void fix_after_pullout(st_select_lex *new_parent, Item **ref, bool merge);
  void cleanup();
  void split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
                      List<Item> &fields, uint flags);
  table_map used_tables() const { return used_tables_cache; };
  bool const_item() const { return const_item_cache; };
  enum Item_result result_type() const { return ROW_RESULT; }
  Item_result cmp_type() const { return ROW_RESULT; }
  enum_field_types field_type() const
  {
    return MYSQL_TYPE_NULL;
  }
  void update_used_tables()
  {
    used_tables_and_const_cache_init();
    used_tables_and_const_cache_update_and_join(arg_count, args);
  }
  table_map not_null_tables() const { return not_null_tables_cache; }
  virtual void print(String *str, enum_query_type query_type);

  bool walk(Item_processor processor, bool walk_subquery, void *arg)
  {
    if (walk_args(processor, walk_subquery, arg))
      return true;
    return (this->*processor)(arg);
  }
  Item *transform(THD *thd, Item_transformer transformer, uchar *arg);
  bool eval_not_null_tables(void *opt_arg);

  uint cols() { return arg_count; }
  Item* element_index(uint i) { return args[i]; }
  Item** addr(uint i) { return args + i; }
  bool check_cols(uint c);
  bool null_inside() { return with_null; };
  void bring_value();

  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
  {
    Item_args::propagate_equal_fields(thd, Context_identity(), cond);
    return this;
  }

  bool excl_dep_on_table(table_map tab_map)
  {
    return Item_args::excl_dep_on_table(tab_map);
  }

  bool excl_dep_on_grouping_fields(st_select_lex *sel)
  {
    return Item_args::excl_dep_on_grouping_fields(sel);
  }

  bool check_vcol_func_processor(void *arg) {return FALSE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_row>(thd, mem_root, this); }
  Item *build_clone(THD *thd, MEM_ROOT *mem_root);
};

#endif /* ITEM_ROW_INCLUDED */
