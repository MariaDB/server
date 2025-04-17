#ifndef SQL_TYPE_ASSOC_ARRAY_INCLUDED
#define SQL_TYPE_ASSOC_ARRAY_INCLUDED

#include "sql_type_composite.h"

#include "field_composite.h"
#include "item_composite.h"


class Field_assoc_array final :public Field_composite
{
protected:
  MEM_ROOT m_mem_root;
  TREE m_tree;

  TABLE *m_table;
  Row_definition_list *m_def;

  Field *m_element_field;
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

  Field *create_element_field(THD *thd);
  bool init_element_field(THD *thd);

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

protected:
  Item_field *create_element(THD *thd);
  bool copy_and_convert_key(THD *thd, const String *key, String &key_copy) const;
  CHARSET_INFO *key_charset() const;

  bool insert_element(String &&key, Item_field *element);
};


class Item_assoc_array_base: public Item_composite_base
{
public:
  const char *composite_name() const override
  { return "associative array"; }
};


/**
  Item_field for the associative array data type
*/
class Item_field_assoc_array: public Item_field, public Item_assoc_array_base
{
public:
  Row_definition_list *m_def;
  Item_field_assoc_array(THD *thd, Field *field)
   :Item_field(thd, field),
    m_def(NULL)
  { }

  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_field_assoc_array>(thd, this); }

  const Type_handler *type_handler() const override
  { return &type_handler_assoc_array; }

  bool set_array_def(THD *thd, Row_definition_list *def);

  uint cols_for_elements() const override;

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
  Field_composite *get_composite_field() const
  {
    return dynamic_cast<Field_composite *>(field);
  }
};


class Item_assoc_array: public Item_composite
{
public:
  Item_assoc_array(THD *thd)
   :Item_composite(thd)
  { }

  Item_assoc_array(THD *thd, List<Item> &list)
   :Item_composite(thd, list)
   { }

  const char *composite_name() const override
  { return "associative array"; }
  enum Type type() const override { return ASSOC_ARRAY_ITEM; }
  const Type_handler *type_handler() const override
  {
    return &type_handler_assoc_array;
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


class Item_splocal_assoc_array_element :public Item_splocal,
                                        public Item_assoc_array_base
{
protected:
  Item *m_key;
  bool set_value(THD *thd, sp_rcontext *ctx, Item **it) override;
public:
  Item_splocal_assoc_array_element(THD *thd,
                         const Sp_rcontext_handler *rh,
                         const LEX_CSTRING *sp_var_name,
                         Item *m_key, uint sp_var_idx,
                         const Type_handler *handler,
                         uint pos_in_q= 0, uint len_in_q= 0)
   :Item_splocal(thd, rh, sp_var_name, sp_var_idx, handler, pos_in_q, len_in_q),
    m_key(m_key)
  { }
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
  public Item_assoc_array_base
{
public:
  Item *m_key;
  Item_field *m_element_item;
  Item_splocal_assoc_array_element_field(THD *thd,
                         const Sp_rcontext_handler *rh,
                         const LEX_CSTRING *sp_var_name,
                         Item *key, const LEX_CSTRING *sp_field_name,
                         uint sp_var_idx,
                         const Type_handler *handler,
                         uint pos_in_q= 0, uint len_in_q= 0)
   :Item_splocal_row_field_by_name(thd, rh, sp_var_name, sp_field_name,
                                   sp_var_idx, handler, pos_in_q, len_in_q),
    m_key(key),
    m_element_item(NULL)
  { }
  bool fix_fields(THD *thd, Item **) override;
  Item *this_item() override;
  const Item *this_item() const override;
  Item **this_item_addr(THD *thd, Item **) override;
  bool append_for_log(THD *thd, String *str) override;
  void print(String *str, enum_query_type query_type) override;

  Item_composite_base *get_composite_variable(sp_rcontext *ctx) const;
};


#endif /* SQL_TYPE_ASSOC_ARRAY_INCLUDED */
