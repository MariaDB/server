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
#include "sql_type.h"
#include "item.h"
#include "sql_type_assoc_array.h"
#include "field.h"
#include "sql_class.h"
#include "item.h"
#include "sql_select.h" // Virtual_tmp_table
#include "sp_rcontext.h"
#include "sp_head.h"
#include "sp_type_def.h"


/*
  Support class to rollback the change list when append_to_log is called.
  The following query for example:
    INSERT INTO t1 VALUES (first_names(TRIM(nick || ' ')));

  will create a new item during fix_fields.

  The raii in the class suffix denotes that this class will
  automatically rollback the change list upon destruction.
*/
class Item_change_list_savepoint_raii : public Item_change_list_savepoint
{
private:
  THD *m_thd;
public:
  Item_change_list_savepoint_raii(THD *thd)
    : Item_change_list_savepoint(thd)
    , m_thd(thd)
  { }
  ~Item_change_list_savepoint_raii()
  {
    rollback(m_thd);
  }
};


class Item_field_packable
{
protected:
  Binary_string m_buffer;
public:
  Item_field_packable()
  {}

  Item_field_packable(char *pack_buffer, uint length)
  : m_buffer(pack_buffer, length)
  {}

  virtual ~Item_field_packable()= default;
  virtual void unpack() const = 0;
  virtual bool pack() = 0;

  uchar *ptr() const
  {
    return reinterpret_cast<uchar *>(&m_buffer[0]);
  }
  uint buffer_length() const
  {
    return m_buffer.alloced_length();
  }
};


class Item_field_packable_scalar: public Item_field,
                                  public Item_field_packable
{
public:
  Item_field_packable_scalar(THD *thd, Field *field)
    :Item_field(thd, field), Item_field_packable()
  {
    thd->free_list= next;
  }
  Item_field_packable_scalar(THD *thd, Field *field,
                             char *pack_buffer, uint pack_length)
    :Item_field(thd, field), Item_field_packable(pack_buffer, pack_length)
  {
    thd->free_list= next;
  }
  ~Item_field_packable_scalar()= default;

  static Item_field_packable_scalar *create(THD *thd, Field *field)
  {
    auto item_size= sizeof(Item_field_packable_scalar);
    uint length= item_size +
                    field->packed_col_length(field->ptr,
                                             field->value_length()) + 1;
    auto temp= (char *)my_malloc(PSI_NOT_INSTRUMENTED,
                          length,
                          MYF(MY_ZEROFILL | MY_WME));
    if (!temp)
      return nullptr;
    
    return ::new (temp) Item_field_packable_scalar(thd,
                                                   field,
                                                   temp + item_size,
                                                   length - item_size);
  }

  void unpack() const override
  {
    field->unpack(field->ptr, ptr(), ptr() + buffer_length());
  }

  bool pack() override
  {
    auto length= field->packed_col_length(field->ptr,
                                          field->value_length()) + 1;
    if (unlikely(m_buffer.realloc(length)))
      return true;

    field->pack(ptr(), field->ptr);
    return false;
  }

  double val_real() override
  {
    unpack();
    return Item_field::val_real();
  }
  longlong val_int() override
  {
    unpack();
    return Item_field::val_int();
  }
  bool val_bool() override
  {
    unpack();
    return Item_field::val_bool();
  }
  my_decimal *val_decimal(my_decimal *dec) override
  {
    unpack();
    return Item_field::val_decimal(dec);
  }
  String *val_str(String *str) override
  {
    unpack();
    return Item_field::val_str(str);
  }
  void save_result(Field *to) override
  {
    unpack();
    Item_field::save_result(to);
  }
  double val_result() override
  {
    unpack();
    return Item_field::val_result();
  }
  longlong val_int_result() override
  {
    unpack();
    return Item_field::val_int_result();
  }
  bool val_native(THD *thd, Native *to) override
  {
    unpack();
    return Item_field::val_native(thd, to);
  }
  bool val_native_result(THD *thd, Native *to) override
  {
    unpack();
    return Item_field::val_native_result(thd, to);
  }
  String *str_result(String* tmp) override
  {
    unpack();
    return Item_field::str_result(tmp);
  }
  my_decimal *val_decimal_result(my_decimal *dec) override
  {
    unpack();
    return Item_field::val_decimal_result(dec);
  }
  bool val_bool_result() override
  {
    unpack();
    return Item_field::val_bool_result();
  }
  bool is_null_result() override
  {
    unpack();
    return Item_field::is_null_result();
  }
  bool send(Protocol *protocol, st_value *buffer) override
  {
    unpack();
    return Item_field::send(protocol, buffer);
  }
  int save_in_field(Field *field,bool no_conversions) override
  {
    unpack();
    return Item_field::save_in_field(field, no_conversions);
  }
};


class Item_field_packable_row: public Item_field_row,
                               public Item_field_packable
{
public:
  Item_field_packable_row(THD *thd, Field *field)
    :Item_field_row(thd, field), Item_field_packable()
  {
    thd->free_list= next;
  }
  ~Item_field_packable_row()
  {
    if (args)
    {
      for (uint i= 0; i < arg_count; i++)
      {
        if (args[i])
        {
          Item_field_packable *item_elem=
            dynamic_cast<Item_field_packable *>(args[i]);
          DBUG_ASSERT(item_elem);
          item_elem->~Item_field_packable();

          my_free(args[i]);
        }
      }
      my_free(args);
    }
  }

  bool alloc_arguments(uint count)
  {
    if ((args= (Item **) my_malloc(PSI_NOT_INSTRUMENTED,
                                   sizeof(Item *) * count,
                                   MYF(MY_ZEROFILL | MY_WME))) == NULL)
    {
      arg_count= 0;
      return true;
    }
    return false;
  }

  bool
  add_array_of_item_field(THD *thd, const Virtual_tmp_table &vtable)
  {
    DBUG_ASSERT(vtable.s->fields);
    DBUG_ASSERT(!arg_count);

    if (alloc_arguments(vtable.s->fields))
      return true;

    for (arg_count= 0; arg_count < vtable.s->fields; arg_count++)
    {
      auto field= vtable.field[arg_count];
      if (!(args[arg_count]= Item_field_packable_scalar::create(thd, field)))
        return true;
    }
    return false;
  }

  Item *do_get_copy(THD *thd) const override
  {
    DBUG_ASSERT(0);
    return get_item_copy<Item_field_packable_row>(thd, this);
  }

  void unpack() const override
  {
    for (uint i= 0; i < arg_count; i++)
    {
      Item_field_packable *item_elem=
        dynamic_cast<Item_field_packable *>(args[i]);
      if (item_elem)
        item_elem->unpack();
    }
  }

  bool pack() override
  {
    for (uint i= 0; i < arg_count; i++)
    {
      Item_field_packable *item_elem=
        dynamic_cast<Item_field_packable *>(args[i]);
      DBUG_ASSERT(item_elem);
      if (unlikely(item_elem->pack()))
        return true;
    }

    return false;
  }
};


/*
  Invalidate the position of a Rewritable_query_parameter object
  in the query string. We use this when we want to rewrite nested rqps
  which is the case for the associative array methods or element accessors.
*/
static void invalidate_rqp(const Item *item, void *arg)
{
  DBUG_ASSERT(item);
  if (arg && static_cast<Item *>(arg) == item)
    return;

  Rewritable_query_parameter *parg;
  if ((parg=
        dynamic_cast<Rewritable_query_parameter *>(const_cast<Item *>(item))))
    parg->pos_in_query= 0;
}


class Item_method_base
{
protected:
  uint m_var_idx;
  Lex_ident_sys m_var_name;
  const Sp_rcontext_handler *m_rcontext_handler;
  THD *m_thd;
public:
  Item_method_base(THD *thd)
    :m_var_idx(0),
     m_rcontext_handler(nullptr),
     m_thd(thd)
  { }
  virtual ~Item_method_base() = default;

  virtual bool init_method(const Lex_ident_sys &item_name,
                           const Lex_ident_cli_st &query_fragment)= 0;
  sp_rcontext *get_rcontext(sp_rcontext *local_ctx) const
  {
    return m_rcontext_handler->get_rcontext(local_ctx);
  }
  Item_field *get_variable(sp_rcontext *ctx= nullptr) const
  {
    if (!ctx)
      ctx= m_thd->spcont;
    return get_rcontext(ctx)->get_variable(m_var_idx);
  }
  Item_composite_base *get_composite_field() const
  {
    Item_field *item= get_variable(m_thd->spcont);
    DBUG_ASSERT(item);
    return dynamic_cast<Item_composite_base *>(item);
  }
};


template <typename T>
class Item_method_func :public T,
                        public Rewritable_query_parameter,
                        public Item_method_base
{

public:
  Item_method_func(THD *thd, Item *arg)
    :T(thd, arg),
     Rewritable_query_parameter(),
     Item_method_base(thd)
  { }
  Item_method_func(THD *thd)
    :T(thd),
     Rewritable_query_parameter(),
     Item_method_base(thd)
  { }
  virtual ~Item_method_func() = default;

  bool init_method(const Lex_ident_sys &item_name,
                   const Lex_ident_cli_st &query_fragment) override
  {
    DBUG_ASSERT(!item_name.is_null());
    sp_variable *spvar= nullptr;
    m_var_name= item_name;

    spvar= m_thd->lex->find_variable(&m_var_name, &m_rcontext_handler);
    DBUG_ASSERT(spvar);

    m_var_idx= spvar->offset;

    T::traverse_cond(invalidate_rqp, nullptr, Item::traverse_order::PREFIX);

    pos_in_query= query_fragment.pos() - m_thd->lex->sphead->m_tmp_query;
    len_in_query= query_fragment.length;

    return false;
  }


  Rewritable_query_parameter *get_rewritable_query_parameter() override
  { return this; }


  bool append_value_for_log(THD *thd, String *str)
  {
    StringBuffer<STRING_BUFFER_USUAL_SIZE> str_value_holder(&my_charset_latin1);
    Item *item= T::this_item();
    String *str_value= item->type_handler()->print_item_value(thd, item,
                                                              &str_value_holder);
    return (str_value ?
            str->append(*str_value) :
            str->append(NULL_clex_str));
  }


  bool append_for_log(THD *thd, String *str) override
  {
    Item_change_list_savepoint_raii savepoint(thd);
    if (T::fix_fields_if_needed(thd, NULL))
      return true;

    if (limit_clause_param)
      return str->append_ulonglong(T::val_uint());

    Item_field *item=get_variable(thd->spcont);
    DBUG_ASSERT(item);

    CHARSET_INFO *cs= thd->variables.character_set_client;
    StringBuffer<NAME_CHAR_LEN> tmp(cs);

    return tmp.append(T::name, &my_charset_utf8mb3_bin) ||
           str->append(STRING_WITH_LEN("NAME_CONST(")) ||
           append_query_string(cs, str, tmp.ptr(), tmp.length(), false) ||
           str->append(',') ||
           append_value_for_log(thd, str) || str->append(')');
  }


  void print(String *str, enum_query_type query_type) override
  {
    if (str->append(m_var_name) ||
        str->append('@'))
      return;
    str->qs_append(m_var_idx);
    if (str->append('.'))
      return;
    T::print(str, query_type);
  }
};


typedef Item_method_func<Item_bool_func> Item_bool_method;
typedef Item_method_func<Item_long_func> Item_long_method;
typedef Item_method_func<Item_handled_func> Item_handled_method;


class Func_handler_assoc_array_first:
        public Item_handled_func::Handler_str
{
public:
  const Type_handler *
      return_type_handler(const Item_handled_func *item) const override
  {
    return &type_handler_string;
  }


  bool fix_length_and_dec(Item_handled_func *item) const override
  {
    auto var_field=
      dynamic_cast<Field_assoc_array *>(get_composite_field(item));
    DBUG_ASSERT(var_field);

    item->collation.collation= var_field->key_charset();

    return false;
  }


  static Field_composite *get_composite_field(Item *item)
  {
    auto method= dynamic_cast<Item_method_base *>(item);
    DBUG_ASSERT(method);

    auto method_var= method->get_variable();
    DBUG_ASSERT(method_var);

    return dynamic_cast<Field_composite *>(method_var->field);
  }


  virtual String *val_str(Item_handled_func *item, String *tmp) const override
  {
    auto var_field= get_composite_field(item);
    DBUG_ASSERT(var_field);

    if ((item->null_value= var_field->get_key(tmp, true)))
      return NULL;

    return tmp;
  }
};


class Func_handler_assoc_array_last:
        public Func_handler_assoc_array_first
{
  String *val_str(Item_handled_func *item, String *tmp) const override
  {
    auto var_field= get_composite_field(item);
    DBUG_ASSERT(var_field);

    if ((item->null_value= var_field->get_key(tmp, false)))
      return NULL;

    return tmp;
  }
};


class Func_handler_assoc_array_next:
        public Func_handler_assoc_array_first
{
  String *val_str(Item_handled_func *item, String *tmp) const override
  {
    DBUG_ASSERT(item->fixed());

    auto var_field= get_composite_field(item);
    DBUG_ASSERT(var_field);

    auto curr_key= item->arguments()[0]->val_str();
    if ((item->null_value= !curr_key ||
                           var_field->get_next_key(curr_key, tmp)))
      return NULL;

    return tmp;
  }
};


class Func_handler_assoc_array_prior:
        public Func_handler_assoc_array_first
{
  String *val_str(Item_handled_func *item, String *tmp) const override
  {
    DBUG_ASSERT(item->fixed());

    auto var_field= get_composite_field(item);
    DBUG_ASSERT(var_field);

    auto curr_key= item->arguments()[0]->val_str();
    if ((item->null_value= !curr_key ||
                            var_field->get_prior_key(curr_key, tmp)))
      return NULL;

    return tmp;
  }
};


/* Item_funcs for associative array methods */

class Item_func_assoc_array_first :public Item_handled_method
{
public:
  Item_func_assoc_array_first(THD *thd)
    :Item_handled_method(thd) {}
  bool check_arguments() const override
  {
    return false;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("first") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    static Func_handler_assoc_array_first ha_str_key;
    set_func_handler(&ha_str_key);
    return m_func_handler->fix_length_and_dec(this);
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_assoc_array_first>(thd, this); }
};


class Item_func_assoc_array_last :public Item_handled_method
{
public:
  Item_func_assoc_array_last(THD *thd)
    :Item_handled_method(thd) {}
  bool check_arguments() const override
  {
    return false;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("last") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_assoc_array_last>(thd, this); }
};


class Item_func_assoc_array_next :public Item_handled_method
{
public:
  Item_func_assoc_array_next(THD *thd, Item *curr_key)
    :Item_handled_method(thd, curr_key) {}
  bool check_arguments() const override
  {
    return false;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("next") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_assoc_array_next>(thd, this); }
};


class Item_func_assoc_array_prior :public Item_handled_method
{
public:
  Item_func_assoc_array_prior(THD *thd, Item *curr_key)
    :Item_handled_method(thd, curr_key) {}
  bool check_arguments() const override
  {
    return false;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("prior") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_assoc_array_prior>(thd, this); }
};


class Item_func_assoc_array_count :public Item_long_method
{
  bool check_arguments() const override
  { return arg_count != 0; }
public:
  Item_func_assoc_array_count(THD *thd)
    :Item_long_method(thd) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("count") };
    return name;
  }
  longlong val_int() override
  {
    DBUG_ASSERT(fixed());

    auto *array= get_composite_field();
    DBUG_ASSERT(array);

    return array->rows();
  }
  bool fix_length_and_dec(THD *thd) override
  {
    decimals= 0;
    max_length= 10;
    set_maybe_null();
    return FALSE;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_assoc_array_count>(thd, this); }
};


class Item_func_assoc_array_exists :public Item_bool_method
{
  bool check_arguments() const override
  { return arg_count != 1; }

public:
  Item_func_assoc_array_exists(THD *thd, Item *key)
    :Item_bool_method(thd, key) {}
  bool val_bool() override
  {
    DBUG_ASSERT(fixed());

    if (args[0]->null_value)
      return false;

    auto *array= get_composite_field();
    DBUG_ASSERT(array);

    return array->element_by_key(current_thd, args[0]->val_str()) != NULL;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("exists") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    decimals= 0;
    max_length= 1;
    set_maybe_null();
    return FALSE;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_assoc_array_exists>(thd, this); }
};


class Item_func_assoc_array_delete :public Item_bool_method
{
  bool check_arguments() const override { return arg_count > 1; }

public:
  Item_func_assoc_array_delete(THD *thd)
    :Item_bool_method(thd)
  {}
  Item_func_assoc_array_delete(THD *thd, Item *key)
    :Item_bool_method(thd, key)
  {}
  bool val_bool() override
  {
    DBUG_ASSERT(fixed());

    Item_field *item= get_variable(m_thd->spcont);
    DBUG_ASSERT(item);

    auto field= dynamic_cast<Field_composite *>(item->
      field_for_view_update()->field);
    if (arg_count == 0)
      return field->delete_all_elements();
    else if (arg_count == 1)
      return field->delete_element_by_key(args[0]->val_str());

    return false;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("delete") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    decimals= 0;
    max_length= 1;
    set_maybe_null();
    return FALSE;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_assoc_array_delete>(thd, this); }
};


bool Item_func_assoc_array_last::fix_length_and_dec(THD *thd)
{
  static Func_handler_assoc_array_last ha_str_key;
  set_func_handler(&ha_str_key);
  return m_func_handler->fix_length_and_dec(this);
}


bool Item_func_assoc_array_next::fix_length_and_dec(THD *thd)
{
  static Func_handler_assoc_array_next ha_str_key;
  set_func_handler(&ha_str_key);
  return m_func_handler->fix_length_and_dec(this);
}


bool Item_func_assoc_array_prior::fix_length_and_dec(THD *thd)
{
  static Func_handler_assoc_array_prior ha_str_key;
  set_func_handler(&ha_str_key);
  return m_func_handler->fix_length_and_dec(this);
}


static
Item_method_base *sp_get_assoc_array_key(THD *thd,
                                         List<Item> *args,
                                         bool is_first)
{
  if (args)
  {
    my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0), is_first ? "FIRST" : "LAST",
             "", 0, args->elements);
    return NULL;
  }

  return is_first ?
    (Item_method_base *)new (thd->mem_root) Item_func_assoc_array_first(thd) :
    (Item_method_base *)new (thd->mem_root) Item_func_assoc_array_last(thd);
}


static
Item_method_base *sp_get_assoc_array_next_or_prior(THD *thd,
                                                   List<Item> *args,
                                                   bool is_next)
{
  if (!args || args->elements != 1)
  {
    my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0), is_next ? "NEXT" : "PRIOR",
             "", 1, args ? args->elements : 0);
    return NULL;
  }

  Item_args args_item(thd, *args);
  return is_next ? (Item_method_base *)
                      new (thd->mem_root)
                        Item_func_assoc_array_next(thd,
                                                   args_item.arguments()[0]) :
                   (Item_method_base *)
                      new (thd->mem_root)
                        Item_func_assoc_array_prior(thd,
                                                   args_item.arguments()[0]);
}


static
Item_method_base *sp_get_assoc_array_count(THD *thd, List<Item> *args)
{
  if (args)
  {
    my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0), "COUNT",
             "", 0, args->elements);
    return NULL;
  }

  return new (thd->mem_root) Item_func_assoc_array_count(thd);
}


static
Item_method_base *sp_get_assoc_array_exists(THD *thd, List<Item> *args)
{
  if (!args || args->elements != 1)
  {
    my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0), "EXISTS",
             "", 1, args ? args->elements : 0);
    return NULL;
  }

  Item_args args_item(thd, *args);
  return new (thd->mem_root)
            Item_func_assoc_array_exists(thd, args_item.arguments()[0]);
}


static
Item_method_base *sp_get_assoc_array_delete(THD *thd,
                                            List<Item> *args)
{
  if (args)
  {
    if (args->elements != 1)
    {
      my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0), "DELETE", 
               "", 1, args->elements);
      return NULL;
    }

    Item_args args_item(thd, *args);
    return new (thd->mem_root)
      Item_func_assoc_array_delete(thd, args_item.arguments()[0]);
  }
  else
    return new (thd->mem_root) Item_func_assoc_array_delete(thd);
}


/****************************************************************************
  Field_assoc_array, e.g. for associative array type SP variables
****************************************************************************/

/*
  The data structure used to store the key-value pairs in the
  associative array (TREE)
*/
struct Assoc_array_data :public Sql_alloc
{
  Assoc_array_data(String &&src_key, Item_field *value)
    : value(value)
  {
    key.swap(src_key);
  }
  Assoc_array_data(const String &src_key, Item_field *value)
    : key(src_key), value(value)
  {}

  String key;
  Item_field *value;
};


static int assoc_array_tree_cmp(void *arg, const void *lhs_arg,
                         const void *rhs_arg)
{
  const Assoc_array_data *lhs= (const Assoc_array_data *)lhs_arg;
  const Assoc_array_data *rhs= (const Assoc_array_data *)rhs_arg;
  return sortcmp(&lhs->key, &rhs->key, (CHARSET_INFO*)arg);
}


static int assoc_array_tree_del(void *data_arg, TREE_FREE, void*)
{
  DBUG_ASSERT(data_arg);
  Assoc_array_data *data= (Assoc_array_data *)data_arg;

  // Explicitly set the key's buffer to NULL to deallocate
  // the memory held in it's internal buffer.
  data->key.set((const char*)NULL, 0, &my_charset_bin);

  // Call destructor of the value
  Item_field_packable *item_elem=
    dynamic_cast<Item_field_packable *>(data->value);
  if (item_elem)
  {
    item_elem->~Item_field_packable();
    my_free(data->value);
  }
  return 0;
}


Field_assoc_array::Field_assoc_array(uchar *ptr_arg,
                                     const LEX_CSTRING *field_name_arg)
  :Field_composite(ptr_arg, field_name_arg),
   m_table(nullptr),
   m_table_share(nullptr),
   m_def(nullptr),
   m_element_field(nullptr)
{
  init_alloc_root(PSI_NOT_INSTRUMENTED, &m_mem_root, 512, 0, MYF(0));

  m_table_share= (TABLE_SHARE*) alloc_root(&m_mem_root, sizeof(TABLE_SHARE));
  if (!m_table_share)
    return;

  bzero((void *)m_table_share, sizeof(TABLE_SHARE));

  m_table_share->table_cache_key= empty_clex_str;
  m_table_share->table_name= Lex_ident_table(empty_clex_str);

  init_tree(&m_tree, 0, 0,
            sizeof(Assoc_array_data), assoc_array_tree_cmp,
	          assoc_array_tree_del, NULL,
            MYF(MY_THREAD_SPECIFIC | MY_TREE_WITH_DELETE));
}


Field_assoc_array::~Field_assoc_array()
{
  if (m_table)
    m_table->alias.free();
  delete_tree(&m_tree, 0);

  free_root(&m_mem_root, MYF(0));
}


CHARSET_INFO *Field_assoc_array::key_charset() const
{
  if (!m_def || !m_def->elements)
    return &my_charset_bin;

  auto &key_def= *m_def->begin();
  return key_def.charset;
}


bool Field_assoc_array::sp_prepare_and_store_item(THD *thd, Item **value)
{
  DBUG_ENTER("Field_assoc_array::sp_prepare_and_store_item");

  if (value[0]->type() == Item::NULL_ITEM)
  {
    delete_all_elements();

    DBUG_RETURN(false);
  }

  Item *src;
  if (!(src= thd->sp_fix_func_item(value)) ||
      src->cmp_type() != ROW_RESULT ||
      src->type_handler() != &type_handler_assoc_array)
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), m_table->s->fields);
    DBUG_RETURN(true);
  }

  src->bring_value();
  auto composite= dynamic_cast<Item_composite_base *>(src);
  DBUG_ASSERT(composite);

  delete_all_elements();

  Item_field *element= nullptr;
  String src_key;
  if (!composite->get_key(&src_key, true))
  {
    do
    {
      Item **src_elem= composite->element_addr_by_key(thd, NULL, &src_key);
      if (!src_elem)
        goto error;
      
      if (m_element_field->sp_prepare_and_store_item(thd, src_elem))
        goto error;

      element= create_element(thd);
      if (!element)
        goto error;

      type_handler_assoc_array.finalize_for_set(element);

      String key_copy;
      if (copy_and_convert_key(thd, &src_key, key_copy))
        goto error;

      if (insert_element(std::move(key_copy), element))
        goto error;

      element= nullptr;

      set_notnull();
    } while (!composite->get_next_key(&src_key, &src_key));
  }

  DBUG_RETURN(false);

error:
  if (element)
    my_free(element);

  set_null();
  DBUG_RETURN(true);
}


bool Field_assoc_array::insert_element(String &&key, Item_field *element)
{
  Assoc_array_data data(std::move(key), element);

  if (unlikely(!tree_insert(&m_tree, &data, 0, (void *) key_charset())))
    return true;

  data.key.release();

  return false;
}


Item_field *Field_assoc_array::element_by_key(THD *thd, String *key)
{
  if (!key)
    return NULL;

  Item_field *item= NULL;

  String key_copy;
  if (copy_and_convert_key(thd, key, key_copy))
    return nullptr;

  Assoc_array_data *data= (Assoc_array_data *)
                          tree_search(&m_tree, &key_copy,
                          (void *)key_charset());
  if (data)
    item= data->value;

  if (!item)
  {
    // Create an element for the key if not found
    if (!(item= create_element(thd)))
      goto error;

    if (insert_element(std::move(key_copy), item))
      goto error;
    set_notnull();
  }

  return item;

error:
  if (item)
    my_free(item);

  return NULL;
}


Item_field *Field_assoc_array::element_by_key(THD *thd, String *key) const
{
  if (!key)
    return NULL;

  Item_field *item= NULL;

  String key_copy;
  if (copy_and_convert_key(thd, key, key_copy))
    return NULL;

  Assoc_array_data *data= (Assoc_array_data *)
                           tree_search((TREE *)&m_tree,
                                        &key_copy,
                                        (void *)key_charset());
  if (data)
    item= data->value;

  return item;
}


bool Field_assoc_array::copy_and_convert_key(THD *thd,
                                             const String *key,
                                             String &key_copy) const
{
  DBUG_ASSERT(key);

  uint errors;
  auto &key_def= *m_def->begin();
  if (key_def.type_handler()->field_type() == MYSQL_TYPE_VARCHAR)
  {
    if (key_copy.copy(key, key_def.charset, &errors))
      return true;

    if (key_copy.length() > key_def.length)
    {
      my_error(ER_TOO_LONG_KEY, MYF(0), key_def.length);
      return true;
    }
  }
  else
  {
    if (key_copy.copy(key, key_charset(), &errors))
      return true;

    // Convert the key to a number to perform range check
    // Follow Oracle's range for numerical keys
    char *endptr;
    int error;
    long key_long= key_charset()->strntol(key_copy.ptr(),
                                          key_copy.length(),
                                          10, &endptr, &error);

    if (error ||
        (endptr != key_copy.ptr() + key_copy.length()) ||
        key_long < INT32_MIN || key_long > INT32_MAX)
    {
      my_error(ER_WRONG_VALUE, MYF(0), "ASSOCIATIVE ARRAY KEY",
               key_copy.c_ptr());
      return true;
    }
  }

  return false;
}


Field *Field_assoc_array::create_element_field(THD *thd)
{
  Field *field= nullptr;
  auto &value_def= *(++m_def->begin());
  if (value_def.is_column_type_ref())
  {
    Column_definition cdef;
    if (value_def.column_type_ref()->resolve_type_ref(thd, &cdef))
      return nullptr;

    field= cdef.make_field(m_table_share, thd->mem_root, &empty_clex_str);
  }
  else
    field= value_def.make_field(m_table_share, thd->mem_root, &empty_clex_str);

  if (!field)
    return nullptr;
  
  if (!(m_table= create_virtual_tmp_table(thd, field)))
    return nullptr;

  Field_row *field_row= dynamic_cast<Field_row*>(field);
  if (field_row)
    field_row->field_name= field_name;

  return field;
}


bool Field_assoc_array::init_element_field(THD *thd)
{
  if (m_element_field)
    return false;

  if (!(m_element_field= create_element_field(thd)))
    return true;

  Field_row *field_row= dynamic_cast<Field_row*>(m_element_field);
  if (field_row)
  {
    auto &value_def= *(++m_def->begin());
    if (field_row->row_create_fields(thd, value_def))
      return true;
  }

  return false;
}


Item_field *
Field_assoc_array::make_item_field_spvar(THD *thd,
                                         const Spvar_definition &def)
{
  auto item= new (thd->mem_root) Item_field_assoc_array(thd, this);
  if (!item)
    return nullptr;

  item->set_array_def(thd, def.row_field_definitions());

  if (init_element_field(thd))
    return nullptr;

  return item;
}


Item_field *Field_assoc_array::create_element(THD *thd)
{
  DBUG_ASSERT(m_element_field);

  Item_field *item= nullptr;

  Field_row *field_row= dynamic_cast<Field_row*>(m_element_field);
  if (field_row)
  {
    auto temp= my_malloc(PSI_NOT_INSTRUMENTED,
                         sizeof(Item_field_packable_row),
                         MYF(MY_ZEROFILL | MY_WME));
    if (!temp)
      return nullptr;

    auto row_elem= ::new (temp) Item_field_packable_row(thd, field_row);
    row_elem->add_array_of_item_field(thd, *field_row->virtual_tmp_table());
    item= row_elem;
  }
  else
  {
    item= Item_field_packable_scalar::create(thd, m_element_field);
  }

  return item;
}


Item **Field_assoc_array::element_addr_by_key(THD *thd, String *key)
{
  if (!key)
    return NULL;

  String key_copy;
  if (copy_and_convert_key(thd, key, key_copy))
    return NULL;

  Assoc_array_data *data= (Assoc_array_data *)
                          tree_search(&m_tree,
                                      &key_copy,
                                      (void *)key_charset());
  if (data)
    return (Item **)&data->value;

  return NULL;
}


bool Field_assoc_array::delete_all_elements()
{
  delete_tree(&m_tree, 0);
  set_null();
  return false;
}


bool Field_assoc_array::delete_element_by_key(String *key)
{
  if (!key)
    return false; // We do not care if the key is NULL

  Assoc_array_data key_data(*key, NULL);
  (void) tree_delete(&m_tree, &key_data, 0, (void *)key_charset());
  return false;
}


uint Field_assoc_array::rows() const
{
  return m_tree.elements_in_tree;
}


bool Field_assoc_array::get_key(String *key, bool is_first)
{
  TREE_ELEMENT **last_pos;
  TREE_ELEMENT *parents[MAX_TREE_HEIGHT+1];

  Assoc_array_data *data= (Assoc_array_data *)
                          tree_search_edge(&m_tree,
                                           parents,
                                           &last_pos, is_first ?
                                           offsetof(TREE_ELEMENT, left) :
                                           offsetof(TREE_ELEMENT, right));
  if (data)
  {
    key->copy(data->key);
    return false;
  }

  return true;
}


bool Field_assoc_array::get_next_key(const String *curr_key, String *next_key)
{
  DBUG_ASSERT(next_key);

  TREE_ELEMENT **last_pos;
  TREE_ELEMENT *parents[MAX_TREE_HEIGHT+1];

  if (!curr_key)
    return true;

  Assoc_array_data key_data(*curr_key, NULL);

  Assoc_array_data *data= (Assoc_array_data *)
                          tree_search_key(&m_tree, &key_data, 
                          parents, &last_pos,
                          HA_READ_AFTER_KEY, (void *)key_charset());
  if (data)
  {
    next_key->copy(data->key);
    return false;
  }
  return true;
}


bool Field_assoc_array::get_prior_key(const String *curr_key, String *prior_key)
{
  DBUG_ASSERT(prior_key);

  TREE_ELEMENT **last_pos;
  TREE_ELEMENT *parents[MAX_TREE_HEIGHT+1];

  if (!curr_key)
    return true;

  Assoc_array_data key_data(*curr_key, NULL);

  Assoc_array_data *data= (Assoc_array_data *)
                          tree_search_key(&m_tree,
                                          &key_data, 
                                          parents,
                                          &last_pos,
                                          HA_READ_BEFORE_KEY,
                                          (void *)key_charset());
  if (data)
  {
    prior_key->copy(data->key);
    return false;
  }
  return true;
}


bool Item_field_assoc_array::set_array_def(THD *thd,
                                           Row_definition_list *def)
{
  DBUG_ASSERT(field);

  Field_assoc_array *field_assoc_array= dynamic_cast<Field_assoc_array*>(field);
  if (!field_assoc_array)
    return true;

  field_assoc_array->set_array_def(def);
  return false;
}


bool Item_assoc_array::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);
  null_value= 0;
  base_flags&= ~item_base_t::MAYBE_NULL;

  Item **arg, **arg_end;
  for (arg= args, arg_end= args + arg_count; arg != arg_end ; arg++)
  {
    if ((*arg)->fix_fields_if_needed(thd, arg))
      return TRUE;
    // we can't assign 'item' before, because fix_fields() can change arg
    Item *item= *arg;

    base_flags|= (item->base_flags & item_base_t::MAYBE_NULL);
    with_flags|= item->with_flags;
  }
  base_flags|= item_base_t::FIXED;
  return FALSE;
}


void Item_assoc_array::bring_value()
{
  for (uint i= 0; i < arg_count; i++)
    args[i]->bring_value();
}


void Item_assoc_array::print(String *str, enum_query_type query_type)
{
  str->append(m_name, current_thd->variables.character_set_client);
  str->append('(');
  for (uint i= 0; i < arg_count; i++)
  {
    if (i)
      str->append(',');

    str->append('\'');
    str->append(args[i]->name.str, args[i]->name.length);
    str->append(STRING_WITH_LEN("'=>"));
    args[i]->print(str, query_type);
  }
  str->append(')');
}


Item *Item_assoc_array::do_build_clone(THD *thd) const
{
  Item **copy_args= static_cast<Item **>
    (alloc_root(thd->mem_root, sizeof(Item *) * arg_count));
  if (unlikely(!copy_args))
    return 0;
  for (uint i= 0; i < arg_count; i++)
  {
    Item *arg_clone= args[i]->build_clone(thd);
    if (!arg_clone)
      return 0;
    copy_args[i]= arg_clone;
  }
  Item_assoc_array *copy= (Item_assoc_array *) get_copy(thd);
  if (unlikely(!copy))
    return 0;
  copy->args= copy_args;
  return copy;
}


uint Item_assoc_array::rows() const
{
  return arg_count;
}


bool Item_assoc_array::get_key(String *key, bool is_first)
{
  DBUG_ASSERT(key);

  uint current_arg;

  if (arg_count == 0)
    return true;

  if (is_first)
    current_arg= 0;
  else
    current_arg= arg_count - 1;

  key->set(args[current_arg]->name.str,
           args[current_arg]->name.length,
           &my_charset_bin);
  return false;
}


bool Item_assoc_array::get_next_key(const String *curr_key, String *next_key)
{
  DBUG_ASSERT(curr_key);
  DBUG_ASSERT(next_key);

  /*
    The code below is pretty slow, but a constructor is a one time operation
  */
  for (uint i= 0; i < arg_count; i++)
  {
    if (args[i]->name.length == curr_key->length() &&
        !memcmp(args[i]->name.str, curr_key->ptr(), curr_key->length()))
    {
      if (i == arg_count - 1)
        return true;
      next_key->set(args[i + 1]->name.str,
                    args[i + 1]->name.length,
                    &my_charset_bin);
      return false;
    }
  }

  return true;
}


Item *Item_assoc_array::element_by_key(THD *thd, String *key)
{
  DBUG_ASSERT(key);

  /*
    See the comment in get_next_key() about the performance
  */
  for (uint i= 0; i < arg_count; i++)
  {
    if (args[i]->name.length == key->length() &&
        !memcmp(args[i]->name.str, key->ptr(), key->length()))
      return args[i];
  }

  return NULL;
}


Item **Item_assoc_array::element_addr_by_key(THD *thd,
                                             Item **addr_arg,
                                             String *key)
{
  /*
    See the comment in get_next_key() about the performance
  */
  for (uint i= 0; i < arg_count; i++)
  {
    if (args[i]->name.length == key->length() &&
        !memcmp(args[i]->name.str, key->ptr(), key->length()))
      return &args[i];
  }

  return NULL;
}


Item_splocal_assoc_array_base::Item_splocal_assoc_array_base(Item *key)
  :m_key(key)
{
  DBUG_ASSERT(m_key);
  m_key->traverse_cond(invalidate_rqp, nullptr, Item::traverse_order::PREFIX);
}


Item_composite_base *
Item_splocal_assoc_array_element::get_composite_variable(sp_rcontext *ctx) const
{
  return dynamic_cast<Item_composite_base *>(get_variable(ctx));
}


Item_splocal_assoc_array_element::Item_splocal_assoc_array_element(THD *thd,
                         const sp_rcontext_addr &addr,
                         const Lex_ident_sys &sp_var_name,
                         Item *key, const Type_handler *handler,
                         uint pos_in_q, uint len_in_q)
   :Item_splocal(thd, addr.rcontext_handler(), &sp_var_name,
                  addr.offset(), handler, pos_in_q, len_in_q),
    Item_splocal_assoc_array_base(key)
{
}


bool Item_splocal_assoc_array_element::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);

  if (m_key->fix_fields_if_needed(thd, &m_key))
    return true;

  if (m_key->null_value)
  {
    my_error(ER_NULL_FOR_ASSOC_ARRAY_INDEX, MYF(0),
             m_name.str);
    return true;
  }

  Item *item= get_composite_variable(thd->spcont)->
    element_by_key(thd, m_key->val_str());
  if (!item)
  {
    my_error(ER_ASSOC_ARRAY_ELEM_NOT_FOUND, MYF(0),
             m_key->val_str()->c_ptr());
    return true;
  }

  set_handler(item->type_handler());
  return fix_fields_from_item(thd, ref, item);
}


Item *
Item_splocal_assoc_array_element::this_item()
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(m_key->fixed());
  return get_composite_variable(m_thd->spcont)->
            element_by_key(m_thd, m_key->val_str());
}


const Item *
Item_splocal_assoc_array_element::this_item() const
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(m_key->fixed());
  return get_composite_variable(m_thd->spcont)->
            element_by_key(m_thd, m_key->val_str());
}


Item **
Item_splocal_assoc_array_element::this_item_addr(THD *thd, Item **ref)
{
  DBUG_ASSERT(m_sp == thd->spcont->m_sp);
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(m_key->fixed());
  return get_composite_variable(thd->spcont)->
           element_addr_by_key(m_thd, ref, m_key->val_str());
}


void Item_splocal_assoc_array_element::print(String *str, enum_query_type type)
{
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();
  str->append(prefix);
  str->append(&m_name);
  str->append('[');
  m_key->print(str, type);
  str->append(']');
  str->append('@');
  str->qs_append(m_var_idx);
  str->append('[');
  m_key->print(str, type);
  str->append(']');
}


bool Item_splocal_assoc_array_element::set_value(THD *thd,
                                                 sp_rcontext *ctx,
                                                 Item **it)
{
  LEX_CSTRING key;
  if (type_handler_assoc_array.key_to_lex_cstring(thd, &m_key, name, key))
    return true;

  return get_rcontext(ctx)->set_variable_composite_by_name(thd, m_var_idx, key,
                                                            it);
}


Item_splocal_assoc_array_element_field::
Item_splocal_assoc_array_element_field(THD *thd,
                                       const sp_rcontext_addr &addr,
                                       const Lex_ident_sys &sp_var_name,
                                       Item *key,
                                       const Lex_ident_sys &sp_field_name,
                                       const Type_handler *handler,
                                       uint pos_in_q, uint len_in_q)
  :Item_splocal_row_field_by_name(thd, addr.rcontext_handler(),
                                  &sp_var_name, &sp_field_name,
                                  addr.offset(), handler, pos_in_q, len_in_q),
  Item_splocal_assoc_array_base(key),
  m_element_item(NULL)
{
}


Item_composite_base *Item_splocal_assoc_array_element_field::
get_composite_variable(sp_rcontext *ctx) const
{
  return dynamic_cast<Item_composite_base *>(get_variable(ctx));
}


bool Item_splocal_assoc_array_element_field::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);

  if (m_key->fix_fields_if_needed(thd, &m_key))
    return true;

  Item *element_item_base= get_composite_variable(thd->spcont)->
                            element_by_key(thd, m_key->val_str());
  if (!element_item_base ||
      !(m_element_item= element_item_base->field_for_view_update()))
  {
    my_error(ER_ASSOC_ARRAY_ELEM_NOT_FOUND, MYF(0),
             m_key->val_str()->c_ptr());
    return true;
  }

  auto element_handler= m_element_item->type_handler()->to_composite();
  if (!element_handler ||
      element_handler->get_item_index(thd,
                                      m_element_item,
                                      m_field_name,
                                      m_field_idx))
  {
    my_error(ER_BAD_FIELD_ERROR, MYF(0),
             m_key->val_str()->c_ptr(), thd_where(thd));
    return true;
  }

  Item *item= m_element_item->element_index(m_field_idx);
  set_handler(item->type_handler());
  return fix_fields_from_item(thd, ref, item);
}


Item *
Item_splocal_assoc_array_element_field::this_item()
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);
  DBUG_ASSERT(fixed());

  return m_element_item->element_index(m_field_idx);
}


const Item *
Item_splocal_assoc_array_element_field::this_item() const
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);
  DBUG_ASSERT(fixed());

  return m_element_item->element_index(m_field_idx);
}


Item **
Item_splocal_assoc_array_element_field::this_item_addr(THD *thd, Item **)
{
  DBUG_ASSERT(m_sp == thd->spcont->m_sp);
  DBUG_ASSERT(fixed());

  return m_element_item->addr(m_field_idx);
}


void Item_splocal_assoc_array_element_field::print(String *str,
                                                   enum_query_type type)
{
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();
  str->append(prefix);
  str->append(&m_name);
  str->append('[');
  m_key->print(str, type);
  str->append(']');
  str->append('.');
  str->append(&m_field_name);
  str->append('@');
  str->qs_append(m_var_idx);
  str->append('[');
  m_key->print(str, type);
  str->append(']');
  str->append('.');
  str->qs_append(m_field_idx);
}


bool Item_splocal_assoc_array_element::append_for_log(THD *thd, String *str)
{
  Item_change_list_savepoint_raii savepoint(thd);

  if (fix_fields_if_needed(thd, NULL))
    return true;

  if (limit_clause_param)
    return str->append_ulonglong(val_uint());

  CHARSET_INFO *cs= thd->variables.character_set_client;
  StringBuffer<NAME_CHAR_LEN> tmp(cs);

  return tmp.append(name, &my_charset_utf8mb3_bin) ||
         str->append(STRING_WITH_LEN("NAME_CONST(")) ||
         append_query_string(cs, str, tmp.ptr(), tmp.length(), false) ||
         str->append(',') ||
         append_value_for_log(thd, str) || str->append(')');
}


bool Item_splocal_assoc_array_element_field::append_for_log(THD *thd,
                                                            String *str)
{
  Item_change_list_savepoint_raii savepoint(thd);

  if (fix_fields_if_needed(thd, NULL))
    return true;

  if (limit_clause_param)
    return str->append_ulonglong(val_uint());

  CHARSET_INFO *cs= thd->variables.character_set_client;
  StringBuffer<NAME_CHAR_LEN> tmp(cs);

  return tmp.append(name, &my_charset_utf8mb3_bin) ||
         str->append(STRING_WITH_LEN("NAME_CONST(")) ||
         append_query_string(cs, str, tmp.ptr(), tmp.length(), false) ||
         str->append(',') ||
         append_value_for_log(thd, str) || str->append(')');
}


class my_var_sp_assoc_array_element: public my_var_sp
{
  Item *m_key;
public:
  my_var_sp_assoc_array_element(const Lex_ident_sys_st &varname, Item *key,
                                const sp_rcontext_addr &addr, sp_head *s)
   :my_var_sp(varname, addr,
              &type_handler_assoc_array, s),
    m_key(key)
  { }

  bool check_assignability(THD *thd, const List<Item> &select_list,
                           bool *assign_as_row) const override
  {
    Item_field *item= thd->get_variable(*this);
    const Field_assoc_array *field=
      dynamic_cast<const Field_assoc_array*>(item->field);
    DBUG_ASSERT(field);
    DBUG_ASSERT(field->get_array_def());
    DBUG_ASSERT(field->get_array_def()->elements == 2);
    const Row_definition_list *def= field->get_array_def();
    List_iterator<Spvar_definition> it(*const_cast<Row_definition_list*>(def));
    it++; // Skip the INDEX BY definition
    const Spvar_definition &table_of= *(it++); // The TABLE OF definition
    /*
      Check select_list compatibility depending on whether the assoc element
      is a ROW or a scalar data type.
    */
    return (*assign_as_row= table_of.row_field_definitions() != nullptr) ?
           table_of.row_field_definitions()->elements != select_list.elements :
           select_list.elements != 1;
  }

  bool set(THD *thd, Item *item) override
  {
    LEX_CSTRING key;
    if (type_handler_assoc_array.key_to_lex_cstring(thd, &m_key, name, key))
      return true;

    return get_rcontext(thd->spcont)->
              set_variable_composite_by_name(thd, offset(), key, &item);
  }
  bool set_row(THD *thd, List<Item> &select_list) override
  {
    Item_row *item_row= new (thd->mem_root) Item_row(thd, select_list);
    return set(thd, item_row);
  }
};


/*
  Special handler associative arrays
*/
class Type_handler_assoc_array: public Type_handler_composite
{
public:
  bool has_methods() const override { return true; }
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
  const Type_handler *type_handler_for_comparison() const override;
  bool Spvar_definition_with_complex_data_types(Spvar_definition *def)
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
        my_error(ER_NEED_NAMED_ASSOCIATION, MYF(0), def.get_name());
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
  // SELECT 1 INTO spvar(arg);
  my_var *make_outvar_lvalue_function(THD *thd, const Lex_ident_sys_st &name,
                                      Item *arg, sp_head *sphead,
                                      const sp_rcontext_addr &addr,
                                      bool validate_only) const override
  {
    if (validate_only)
      return nullptr; // e.g. EXPLAIN SELECT .. INTO spvar_assoc_array('key');
    return new (thd->mem_root) my_var_sp_assoc_array_element(name, arg, addr,
                                                             sphead);
  }
  // assoc_array_var:= assoc_array_type('key1'=>'val1', 'key2'=>'val2')
  Item *make_typedef_constructor_item(THD *thd, const sp_type_def &def,
                                      List<Item> *args) const override
  {
    if (unlikely(args == NULL))
      return new (thd->mem_root) Item_assoc_array(thd, def.get_name());

    if (unlikely(sp_check_assoc_array_args(def, *args)))
      return nullptr;

    return new (thd->mem_root) Item_assoc_array(thd, def.get_name(), *args);
  }

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

  virtual
  Item *create_item_method(THD *thd,
                           const Lex_ident_sys &ca,
                           const Lex_ident_sys &cb,
                           List<Item> *args,
                           const Lex_ident_cli_st &query_fragment)
                                                   const override;

  bool key_to_lex_cstring(THD *thd,
                          Item **key,
                          const LEX_CSTRING& name,
                          LEX_CSTRING& out_key) const override;

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

  void prepare_for_set(Item_field *item) const override;
  bool finalize_for_set(Item_field *item) const override;
};


class Type_collection_assoc_array: public Type_collection
{
public:
  bool init(Type_handler_data *data) override
  {
    return false;
  }
  const Type_handler *aggregate_for_result(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    return NULL;
  }
  const Type_handler *aggregate_for_comparison(const Type_handler *a,
                                               const Type_handler *b)
                                               const override
  {
    return NULL;
  }
  const Type_handler *aggregate_for_min_max(const Type_handler *a,
                                            const Type_handler *b)
                                            const override
  {
    return NULL;
  }
  const Type_handler *aggregate_for_num_op(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    return NULL;
  }
};


static Type_collection_assoc_array type_collection_assoc_array;


const Type_collection *Type_handler_assoc_array::type_collection() const
{
  return &type_collection_assoc_array;
}


const Type_handler *Type_handler_assoc_array::type_handler_for_comparison() const
{
  return &type_handler_assoc_array;
}


bool Type_handler_assoc_array::Spvar_definition_with_complex_data_types(
                                                 Spvar_definition *def) const
{
  /*
    No needs to check the "TABLE OF" and "INDEX BY" data types.
    An assoc array variable in any cases use memory resources which need
    to be freed when a routine execution leaves the DECLARE/BEGIN/END block
    declaring this variable.
  */
  return true;
}


Field *Type_handler_assoc_array::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  DBUG_ASSERT(attr->length == 0);
  DBUG_ASSERT(f_maybe_null(attr->pack_flag));
  return new (mem_root) Field_assoc_array(rec.ptr(), name);
}


String *Type_handler_assoc_array::
          print_item_value(THD *thd, Item *item, String *str) const
{
  DBUG_ASSERT(item->type_handler() == this);

  CHARSET_INFO *cs= thd->variables.character_set_client;
  StringBuffer<STRING_BUFFER_USUAL_SIZE> val(cs);

  /*
    Only `IS NULL` or `IS NOT NULL` operations are supported on
    an associative array.
  */
  if (item->is_null())
    str->append(NULL_clex_str);
  else
    str->append_longlong(1);

  return str;
}


Item *Type_handler_assoc_array::create_item_method(THD *thd,
                                                   const Lex_ident_sys &a,
                                                   const Lex_ident_sys &b,
                                                   List<Item> *args,
                                                   const Lex_ident_cli_st
                                                     &query_fragment) const
{
  Item_method_base *item= nullptr;
  if (b.length == 5)
  {
    if (Lex_ident_column(b).streq("COUNT"_Lex_ident_column))
      item= sp_get_assoc_array_count(thd, args);
    else if (Lex_ident_column(b).streq("FIRST"_Lex_ident_column))
      item= sp_get_assoc_array_key(thd, args, true);
    else if (Lex_ident_column(b).streq("PRIOR"_Lex_ident_column))
      item= sp_get_assoc_array_next_or_prior(thd, args, false);
  }
  else if (b.length == 4)
  {
    if (Lex_ident_column(b).streq("LAST"_Lex_ident_column))
      item= sp_get_assoc_array_key(thd, args, false);
    else if (Lex_ident_column(b).streq("NEXT"_Lex_ident_column))
      item= sp_get_assoc_array_next_or_prior(thd, args, true);
  }
  else if (b.length == 6)
  {
    if (Lex_ident_column(b).streq("EXISTS"_Lex_ident_column))
      item= sp_get_assoc_array_exists(thd, args);
    else if (Lex_ident_column(b).streq("DELETE"_Lex_ident_column))
      item= sp_get_assoc_array_delete(thd, args);
  }

  if (!item)
  {
    my_error(ER_BAD_FIELD_ERROR, MYF(0), a.str, b.str);
    return nullptr;
  }

  if (item->init_method(a, query_fragment))
    return nullptr;

  return dynamic_cast<Item *>(item);
}


bool Type_handler_assoc_array::key_to_lex_cstring(THD *thd,
                                                  Item **key,
                                                  const LEX_CSTRING& name,
                                                  LEX_CSTRING& out_key) const
{
  DBUG_ASSERT(key);
  DBUG_ASSERT(*key);

  if ((*key)->fix_fields_if_needed(thd, key))
    return true;

  if ((*key)->null_value)
  {
    my_error(ER_NULL_FOR_ASSOC_ARRAY_INDEX,
             MYF(0),
             name.str ? name.str : "unknown");
    return true;
  }

  auto str= (*key)->val_str();
  if (!str)
    return true;

  out_key= str->to_lex_cstring();
  return false;
}


Item_field *Type_handler_assoc_array::get_item(THD *thd,
                                               const Item_field *item,
                                               const LEX_CSTRING& name) const
{
  DBUG_ASSERT(item);

  auto item_assoc= dynamic_cast<const Item_field_assoc_array *>(item);
  if (!item_assoc)
    return nullptr;

  const Field_composite *field= item_assoc->get_composite_field();
  if (!field)
    return nullptr;

  String key(name.str, name.length, &my_charset_bin);
  auto elem= field->element_by_key(thd, &key);
  if (!elem)
  {
    my_error(ER_ASSOC_ARRAY_ELEM_NOT_FOUND, MYF(0),
             name.str);
    return nullptr;
  }

  return elem;
}


Item_field *
Type_handler_assoc_array::get_or_create_item(THD *thd,
                                             Item_field *item,
                                             const LEX_CSTRING& name) const
{
  DBUG_ASSERT(item);

  auto item_assoc= dynamic_cast<Item_field_assoc_array *>(item);
  if (!item_assoc)
    return nullptr;

  Field_composite *field= item_assoc->get_composite_field();
  if (!field)
    return nullptr;

  String key(name.str, name.length, &my_charset_bin);
  return field->element_by_key(thd, &key);
}


void Type_handler_assoc_array::prepare_for_set(Item_field *item) const
{
  Item_field_packable *item_elem= dynamic_cast<Item_field_packable *>(item);
  if (!item_elem)
    return;

  item_elem->unpack();
}


bool Type_handler_assoc_array::finalize_for_set(Item_field *item) const
{
  Item_field_packable *item_elem= dynamic_cast<Item_field_packable *>(item);
  if (!item_elem)
    return false;

  return item_elem->pack();
}


static const Named_type_handler<Type_handler_assoc_array>
               type_handler_assoc_array_internal("associative_array");

const Type_handler_composite &type_handler_assoc_array=
  type_handler_assoc_array_internal;


/************************************************************************/
