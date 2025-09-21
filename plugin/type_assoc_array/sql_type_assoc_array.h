/*
   Copyright (c) 2025, Rakuten Securities
   Copyright (c) 2025, MariaDB plc

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/
#ifndef SQL_TYPE_ASSOC_ARRAY_INCLUDED
#define SQL_TYPE_ASSOC_ARRAY_INCLUDED

#include "sql_type_composite.h"
#include "field_composite.h"
#include "item_composite.h"
#include "sp_type_def.h"


class Item_field_packable;
class Assoc_array_data;


/*
  Special handler associative arrays
*/
class Type_handler_assoc_array: public Type_handler_composite
{
public:
  static const Type_handler_assoc_array *singleton();
  static bool check_key_expression_type(Item *key);
  static bool check_functor_args(List<Item> *args, const Qualified_ident &name);
  static bool check_subscript_expression(const Type_handler *formal_th,
                                         Item *key);
public:
  bool has_methods() const override { return true; }
  bool has_functors() const override { return true; }
  bool is_complex() const override
  {
    /*
      Have an assoc array variable generate an sp_instr_destruct_variable
      instruction in the end of the DECLARE/BEGIN/END block declaring
      the variable.
    */
    return true;
  }
  const Type_collection *type_collection() const override;
  const Type_handler *type_handler_for_comparison() const override
  {
    return singleton();
  }
  bool Spvar_definition_with_complex_data_types(Spvar_definition *def)
                                                       const override;
  bool Column_definition_set_attributes(THD *thd,
                                        Column_definition *def,
                                        const Lex_field_type_st &attr,
                                        column_definition_type_t type)
                                                       const override;

  bool sp_variable_declarations_finalize(THD *thd,
                                         LEX *lex, int nvars,
                                         const Column_definition &def)
                                                        const override;

  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp) const
    override
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }

protected:
  static bool lex_ident_col_eq(Lex_ident_column *a, Lex_ident_column *b)
  {
    return a->streq(*b);
  }

  bool sp_check_assoc_array_args(const sp_type_def &def, List<Item> &args) const
  {
    List<Lex_ident_column> names;

    List_iterator<Item> it(args);
    for (Item *item= it++; item; item= it++)
    {
      /*
        Make sure all value have keys:
          assoc_array_type('key1'=>'val1', 'key2'=>'val2') -- correct
          assoc_array_type('val1'        , 'val2'        ) -- wrong
      */
      if (unlikely(!item->is_explicit_name()))
      {
        my_error(ER_NEED_NAMED_ASSOCIATION, MYF(0), def.get_name().str);
        return true;
      }

      /*
        Make sure keys are unique in:
          assoc_array_type('key1'=>'val1', 'key2'=>'val2')
      */
      if (unlikely(names.add_unique(&item->name, lex_ident_col_eq)))
      {
        my_error(ER_DUP_UNKNOWN_IN_INDEX, MYF(0), item->name.str);
        return true;
      }
    }

    return false;
  }

public:
  /*
    SELECT 1 INTO spvar(arg);
    SELECT 1 INTO spvar(arg).field_name;
  */
  my_var *make_outvar_lvalue_functor(THD *thd, const Lex_ident_sys_st &name,
                                     Item *arg,
                                     const Lex_ident_sys &opt_field,
                                     sp_head *sphead,
                                     const sp_rcontext_addr &addr,
                                     bool validate_only) const override;
  // assoc_array_var:= assoc_array_type('key1'=>'val1', 'key2'=>'val2')
  Item *make_typedef_constructor_item(THD *thd, const sp_type_def &def,
                                      List<Item> *args) const override;

  Item_cache *Item_get_cache(THD *thd, const Item *item) const override
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }
  bool set_comparator_func(THD *thd, Arg_comparator *cmp) const override
  {
    MY_ASSERT_UNREACHABLE();
    return true;
  }
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const override
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }
  in_vector *make_in_vector(THD *thd, const Item_func_in *f, uint nargs) const
    override
  {
    MY_ASSERT_UNREACHABLE();
    return nullptr;
  }
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const
    override
  {
    MY_ASSERT_UNREACHABLE();
    return false;
  }

  String *print_item_value(THD *thd, Item *item, String *str) const override;

  virtual Item_splocal *create_item_functor(THD *thd,
                                            const Lex_ident_sys &varname,
                                            const sp_rcontext_addr &addr,
                                            List<Item> *args,
                                            const Lex_ident_sys &member,
                                            const Lex_ident_cli_st &name_cli)
                                                          const override;
  sp_instr *create_instr_set_assign_functor(THD *thd, LEX *lex,
                                            const Qualified_ident &ident,
                                            const sp_rcontext_addr &addr,
                                            List<Item> *params,
                                            const Lex_ident_sys_st &field_name,
                                            Item *item,
                                            const LEX_CSTRING &expr_str)
                                                         const override;
protected:
  Item *create_item_method_func(THD *thd,
                                const Lex_ident_sys &ca,
                                const Lex_ident_sys &cb,
                                List<Item> *args,
                                const Lex_ident_cli_st &query_fragment)
                                                                 const;
  Item *create_item_method_proc(THD *thd,
                                const Lex_ident_sys &ca,
                                const Lex_ident_sys &cb,
                                List<Item> *args,
                                const Lex_ident_cli_st &query_fragment)
                                                                 const;
public:
  virtual
  Item *create_item_method(THD *thd,
                           object_method_type_t type,
                           const Lex_ident_sys &ca,
                           const Lex_ident_sys &cb,
                           List<Item> *args,
                           const Lex_ident_cli_st &query_fragment)
                                                   const override
  {
    return type == object_method_type_t::PROCEDURE ?
           create_item_method_proc(thd, ca, cb, args, query_fragment) :
           create_item_method_func(thd, ca, cb, args, query_fragment);
  }

  LEX_CSTRING key_to_lex_cstring(THD *thd, const sp_rcontext_addr &var,
                                 Item **key, String *buffer) const override;

  bool get_item_index(THD *thd,
                      const Item_field *item,
                      const LEX_CSTRING& name,
                      uint& idx) const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  Item_field *get_item(THD *thd,
                       const Item_field *item,
                       const LEX_CSTRING& name) const override;
  Item_field *get_or_create_item(THD *thd,
                                 Item_field *item,
                                 const LEX_CSTRING& name) const override;

  Item_field *prepare_for_set(Item_field *item) const override;
  bool finalize_for_set(Item_field *item) const override;
};


class Field_assoc_array final :public Field_composite
{
protected:
  TREE m_tree;

  Virtual_tmp_table *m_table;
  Row_definition_list *m_def;

  Item_field_packable *m_item_pack;
  Item *m_item;

  // A helper method
  Assoc_array_data *assoc_tree_search(String *key) const
  {
    return reinterpret_cast<Assoc_array_data *>(tree_search((TREE*) &m_tree,
                                                            key,
                                                            get_key_field()));
  }

public:
  Field_assoc_array(uchar *ptr_arg,
                    const LEX_CSTRING *field_name_arg);
  ~Field_assoc_array();

  void set_array_def(Row_definition_list *def)
  {
    DBUG_ASSERT(def);
    DBUG_ASSERT(def->elements == 2);
    m_def= def;
  }

  const Row_definition_list *get_array_def() const
  {
    return m_def;
  }

  const Spvar_definition *get_key_def() const
  {
    return m_def->head();
  }

  Item_field *make_item_field_spvar(THD *thd,
                                    const Spvar_definition &def) override;

  bool sp_prepare_and_store_item(THD *thd, Item **value) override;

  uint rows() const override;
  bool get_key(String *key, bool is_first) override;
  bool get_next_key(const String *curr_key, String *next_key) override;
  bool get_prior_key(const String *curr_key, String *prior_key) override;
  Item_field *element_by_key(THD *thd, String *key) override;
  Item_field *element_by_key(THD *thd, String *key) const override;
  Item **element_addr_by_key(THD *thd, String *key) override;
  bool delete_all_elements() override;
  bool delete_element_by_key(String *key) override;
  void expr_event_handler(THD *thd, expr_event_t event) override
  {
    if ((bool) (event & expr_event_t::DESTRUCT_ANY))
    {
      delete_all_elements();
      set_null();
      return;
    }
    DBUG_ASSERT(0);
  }

  Item *get_element_item() const override { return m_item; }
  Field *get_key_field() const;
  Field *get_element_field() const;

protected:
  bool copy_and_convert_key(const String &key, String *key_copy) const;
  bool unpack_key(const Binary_string &key, String *key_dst) const;
  bool create_fields(THD *thd);

  /*
    Initialize the element base Field and Item_field for the
    associative array.
  */
  bool init_element_base(THD *thd);

  bool init_key_def(THD *thd, Spvar_definition *key_def) const;

  /*
    Create a packable item field for the associative array element field
    and return it.

    @param thd        - Current thread
    @param field      - The element field
  */
  Item_field_packable *create_packable(THD *thd, Field *field);

  bool insert_element(THD *thd, Assoc_array_data *data, bool warn_on_dup_key);
  bool get_next_or_prior_key(const String *curr_key,
                             String *new_key,
                             bool is_next);
};


/**
  Item_field for the associative array data type
*/
class Item_field_assoc_array: public Item_field, public Item_composite_base
{
public:
  Item_field_assoc_array(THD *thd, Field *field)
   :Item_field(thd, field)
  { }

  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_field_assoc_array>(thd, this); }

  const Type_handler *type_handler() const override
  { return Type_handler_assoc_array::singleton(); }

  bool set_array_def(THD *thd, Row_definition_list *def);

  uint rows() const override
  {
    return get_composite_field()->rows();
  }
  bool get_key(String *key, bool is_first) override
  {
    return get_composite_field()->get_key(key, is_first);
  }
  bool get_next_key(const String *curr_key, String *next_key) override
  {
    return get_composite_field()->get_next_key(curr_key, next_key);
  }
  Item *element_by_key(THD *thd, String *key) override
  {
    return ((const Field_composite *)get_composite_field())->
      element_by_key(thd, key);
  }
  Item **element_addr_by_key(THD *thd, Item **ref, String *key) override
  {
    return get_composite_field()->element_addr_by_key(thd, key);
  }
  Field_composite *get_composite_field() const override
  {
    return dynamic_cast<Field_composite *>(field);
  }
};


class Item_assoc_array: public Item_composite
{
protected:
  Lex_ident_column m_name;

public:
  Item_assoc_array(THD *thd, const Lex_ident_column &name)
   :Item_composite(thd),
    m_name(name)
  { }

  Item_assoc_array(THD *thd, const Lex_ident_column &name, List<Item> &list)
   :Item_composite(thd, list),
    m_name(name)
   { }

  const Type_handler *type_handler() const override
  {
    return Type_handler_assoc_array::singleton();
  }

  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override
  {
    return NULL;
  }

  uint rows() const override;
  bool get_key(String *key, bool is_first) override;
  bool get_next_key(const String *curr_key, String *next_key) override;
  Item *element_by_key(THD *thd, String *key) override;
  Item **element_addr_by_key(THD *thd, Item **addr_arg, String *key) override;

  bool fix_fields(THD *thd, Item **ref) override;
  void bring_value() override;
  void print(String *str, enum_query_type query_type) override;

  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_assoc_array>(thd, this); }
  Item *do_build_clone(THD *thd) const override;
};


class Item_splocal_assoc_array_base :public Item_composite_base
{
protected:
  Item *m_key;
  /*
    In expressions:
      - assoc_array(key_expr)
      - assoc_array(key_expr).field
    execute "fix_fields_if_needed" for the key_expr and check if it's
    compatible with the "INDEX_BY key_type" clause of the assoc array
    definition.
    @param thd        - Current thd
    @param array_addr - The run-time address of the assoc array variable.
  */
  bool fix_key(THD *thd, const sp_rcontext_addr &array_addr);
  bool is_element_exists(THD *thd,
                         const Field_composite *field,
                         const LEX_CSTRING &name) const;
public:
  Item_splocal_assoc_array_base(Item *key);
};


class Item_splocal_assoc_array_element :public Item_splocal,
                                        public Item_splocal_assoc_array_base
{
protected:
  bool set_value(THD *thd, sp_rcontext *ctx, Item **it) override;
public:
  Item_splocal_assoc_array_element(THD *thd,
                                   const sp_rcontext_addr &addr,
                                   const Lex_ident_sys &sp_var_name,
                                   Item *key, const Type_handler *handler,
                                   uint pos_in_q= 0, uint len_in_q= 0);
  bool fix_fields(THD *thd, Item **) override;
  Item *this_item() override;
  const Item *this_item() const override;
  Item **this_item_addr(THD *thd, Item **) override;
  bool append_for_log(THD *thd, String *str) override;
  void print(String *str, enum_query_type query_type) override;

  Item *do_get_copy(THD *) const override { return nullptr; }
  Item *do_build_clone(THD *thd) const override { return nullptr; }

  Item_composite_base *get_composite_variable(sp_rcontext *ctx) const;
};


class Item_splocal_assoc_array_element_field :
  public Item_splocal_row_field_by_name,
  public Item_splocal_assoc_array_base
{
protected:
  Item_field *m_element_item;
public:
  Item_splocal_assoc_array_element_field(THD *thd,
                                        const sp_rcontext_addr &addr,
                                        const Lex_ident_sys &sp_var_name,
                                        Item *key,
                                        const Lex_ident_sys &sp_field_name,
                                        const Type_handler *handler,
                                        uint pos_in_q= 0, uint len_in_q= 0);
  bool fix_fields(THD *thd, Item **) override;
  Item *this_item() override;
  const Item *this_item() const override;
  Item **this_item_addr(THD *thd, Item **) override;
  bool append_for_log(THD *thd, String *str) override;
  void print(String *str, enum_query_type query_type) override;

  Item_composite_base *get_composite_variable(sp_rcontext *ctx) const;
};


#endif /* SQL_TYPE_ASSOC_ARRAY_INCLUDED */
