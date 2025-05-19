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
#define MYSQL_SERVER
#include "my_global.h"
#include "mysql/plugin_data_type.h"
#include "sql_type.h"
#include "item.h"
#include "sql_type_assoc_array.h"
#include "field.h"
#include "sql_class.h"
#include "item.h"
#include "sql_select.h" // Virtual_tmp_table
#include "sp_rcontext.h"
#include "sp_head.h"
#include "sp_instr.h"
#include "sp_type_def.h"


/*
  A helper class: a buffer to pass to val_str() to get key values.
*/
class StringBufferKey: public StringBuffer<STRING_BUFFER_USUAL_SIZE>
{
public:
  using StringBuffer::StringBuffer;
};

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
  Binary_string *m_buffer;
  uint m_offset;
  Item_field_packable *m_assign;
public:
  Item_field_packable()
  : m_buffer(nullptr)
  , m_offset(0)
  , m_assign(nullptr)
  {}

  void set_assign(Item_field_packable *assign)
  {
    m_assign= assign;
  }

  /*
    Get the assignment pair for LHS during assignment.
    We need the pair during self-assignment to ensure that we pack and unpack
    using the correct buffer.
  */
  Item_field_packable *get_assign() const
  {
    m_assign->set_buffer(m_buffer);
    m_assign->set_offset(m_offset);
    return m_assign;
  }

  void set_buffer(Binary_string *buffer)
  {
    DBUG_ASSERT(buffer);
    DBUG_ASSERT(buffer->get_thread_specific());
    m_buffer= buffer;
  }

  void set_offset(uint offset)
  {
    m_offset= offset;
  }

  virtual ~Item_field_packable()= default;
  virtual const uchar *unpack() const = 0;
  virtual bool pack() = 0;

  uchar *ptr() const
  {
    return m_buffer->ptr() ? 
      reinterpret_cast<uchar *>(&(*m_buffer)[0]) + m_offset:
      nullptr;
  }
  uint buffer_length() const
  {
    return m_buffer->alloced_length() - m_offset;
  }
};


class Item_field_packable_scalar: public Item_field,
                                  public Item_field_packable
{
public:
  Item_field_packable_scalar(THD *thd, Field *field)
    :Item_field(thd, field)
  {
  }
  ~Item_field_packable_scalar()= default;

  const uchar *unpack() const override
  {
    DBUG_ASSERT(field);
    DBUG_ASSERT(m_buffer);

    if (!ptr() || *ptr())
    {
      field->set_null();
      return !ptr() ? nullptr : ptr() + 1;
    }

    field->set_notnull();
    return field->unpack(field->ptr, ptr() + 1,
                         ptr() + buffer_length());
  }

  bool pack() override
  {
    DBUG_ASSERT(field);
    DBUG_ASSERT(m_buffer);

    if (field->is_null())
    {
      if (unlikely(m_buffer->realloc(1)))
        return true;
      *ptr()= true;
      return false;
    }
  
    uint length= field->packed_col_length();
    if (unlikely(m_buffer->realloc(length + 1)))
      return true;

    *ptr()= false;
    uchar *start= ptr() + 1;
#ifndef DBUG_OFF
    StringBuffer<64> type;
    field->sql_type(type);
    const uchar *pend=
#endif
    field->pack(start, field->ptr);
    DBUG_ASSERT((uint) (pend - start) == length);
    DBUG_EXECUTE_IF("assoc_array_pack",
                    push_warning_printf(current_thd,
                      Sql_condition::WARN_LEVEL_NOTE,
                      ER_YES, "pack=%u plen=%u ; mdlen=%u flen=%u ; `%s` %s",
                      (uint) (pend - start), length,
                      field->max_data_length(),
                      field->field_length,
                      field->field_name.str,
                      ErrConvString(&type).ptr()););
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
    :Item_field_row(thd, field)
  {
  }
  ~Item_field_packable_row()= default;

  bool
  add_array_of_item_field(THD *thd)
  {
    DBUG_ASSERT(field);
    DBUG_ASSERT(field->virtual_tmp_table());
  
    const Virtual_tmp_table &vtable= *field->virtual_tmp_table();

    DBUG_ASSERT(vtable.s->fields);
    DBUG_ASSERT(!arg_count);

    if (alloc_arguments(thd, vtable.s->fields))
      return true;

    for (arg_count= 0; arg_count < vtable.s->fields; arg_count++)
    {
      auto field= vtable.field[arg_count];
      if (!(args[arg_count]= new (thd->mem_root) Item_field(thd, field)))
        return true;
    }
    return false;
  }

  Item *do_get_copy(THD *thd) const override
  {
    DBUG_ASSERT(0);
    return get_item_copy<Item_field_packable_row>(thd, this);
  }

  static uint packed_col_length(Field *field)
  {
    DBUG_ASSERT(field);
    DBUG_ASSERT(field->virtual_tmp_table());
  
    const Virtual_tmp_table &vtable= *field->virtual_tmp_table();
    uint length= 0;
    for (uint i= 0; i < vtable.s->fields; i++)
    {
      auto field= vtable.field[i];
      if (!field->is_null())
        length+= field->packed_col_length();
    }

    return vtable.s->null_bytes + length + 1;
  }

  const uchar* unpack() const override
  {
    DBUG_ASSERT(field);
    DBUG_ASSERT(field->virtual_tmp_table());
    DBUG_ASSERT(m_buffer);

    const Virtual_tmp_table &vtable= *field->virtual_tmp_table();

    if (!ptr() || *ptr())
    {
      field->set_null();
      for (uint i= 0; i < arg_count; i++)
      {
        auto field= vtable.field[i];
        field->set_null();
      }
      return nullptr;
    }

    /*
      The layout of the buffer for ROW elements is:
      null flag for the ROW
      null bytes for the ROW fields
      packed data for field 0
      packed data for field 1
      ...
      packed data for field n

      Fields where the null flag is set are not packed.
    */
    ptr()[0] ? field->set_null() : field->set_notnull();

    // Copy the null bytes
    memcpy(vtable.null_flags, ptr() + 1, vtable.s->null_bytes);
    uint offset= vtable.s->null_bytes + 1;

    for (uint i= 0; i < arg_count; i++)
    {
      auto field= vtable.field[i];
      DBUG_ASSERT(field);

      if (!field->is_null())
      {
        auto end= field->unpack(field->ptr, ptr() + offset,
                                ptr() + buffer_length());
        if (unlikely(!end))
          return nullptr;
        offset= (uint) (end - ptr());
      }
    }

    DBUG_ASSERT(offset <= buffer_length());
    return ptr() + offset;
  }

  bool pack() override
  {
    DBUG_ASSERT(field);
    DBUG_ASSERT(field->virtual_tmp_table());
    DBUG_ASSERT(m_buffer);
  
    const Virtual_tmp_table &vtable= *field->virtual_tmp_table();

    uint length= packed_col_length(field); 
    if (unlikely(m_buffer->realloc(length)))
      return true;
  
    *ptr()= field->is_null();
    if (field->is_null())
      return false;
  
    // Copy the null bytes
    memcpy(ptr() + 1, vtable.null_flags, vtable.s->null_bytes);
    
    uint offset= 1 + vtable.s->null_bytes;
    for (uint i= 0; i < arg_count; i++)
    {
      auto field= vtable.field[i];
      if (!field->is_null())
      {
        auto end= field->pack(ptr() + offset, field->ptr);
        if (unlikely(!end))
          return true;
        
        offset= (uint) (end - ptr());
      }
    }

    DBUG_ASSERT(offset <= length);

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
    len_in_query= (uint) query_fragment.length;

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
    str->append_ulonglong(m_var_idx);
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

    item->collation.collation= var_field->get_key_field()->charset();

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

    StringBufferKey buffer;
    auto curr_key= item->arguments()[0]->val_str(&buffer);
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

    StringBufferKey buffer;
    auto curr_key= item->arguments()[0]->val_str(&buffer);
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
    DBUG_ASSERT(!null_value);

    auto *array= get_composite_field();
    DBUG_ASSERT(array);

    return array->rows();
  }
  bool fix_length_and_dec(THD *thd) override
  {
    decimals= 0;
    max_length= 10;
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
    DBUG_ASSERT(!null_value);
    StringBufferKey buffer;
    String *str= args[0]->val_str(&buffer);
    if (!str)
      return false;

    auto *array= get_composite_field();
    DBUG_ASSERT(array);

    return array->element_by_key(current_thd, str) != NULL;
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
    {
      StringBufferKey buffer;
      return field->delete_element_by_key(args[0]->val_str(&buffer));
    }

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
class Assoc_array_data :public Sql_alloc
{
  void set_thread_specific()
  {
    m_key.set_thread_specific();
    m_value.set_thread_specific();
  }

public:
  Assoc_array_data()
  {
    set_thread_specific();
  }

  void release()
  {
    m_key.release();
    m_value.release();
    set_thread_specific();
  }

  String m_key;
  Binary_string m_value;
};


static int assoc_array_tree_cmp(void *arg, const void *lhs_arg,
                         const void *rhs_arg)
{
  const Assoc_array_data *lhs= (const Assoc_array_data *)lhs_arg;
  const Assoc_array_data *rhs= (const Assoc_array_data *)rhs_arg;

  auto field= reinterpret_cast<Field *>(arg);
  DBUG_ASSERT(field);

  if (field->type() == MYSQL_TYPE_VARCHAR)
    return sortcmp(&lhs->m_key, &rhs->m_key, field->charset());

  return field->cmp(reinterpret_cast<const uchar*>(lhs->m_key.ptr()),
                    reinterpret_cast<const uchar*>(rhs->m_key.ptr()));
}


static int assoc_array_tree_del(void *data_arg, TREE_FREE, void*)
{
  DBUG_ASSERT(data_arg);
  Assoc_array_data *data= (Assoc_array_data *)data_arg;

  // Explicitly set the key's buffer to NULL to deallocate
  // the memory held in it's internal buffer.
  data->m_key.set((const char*)NULL, 0, &my_charset_bin);
  data->m_value.set((const char*)NULL, 0);
  return 0;
}


Field_assoc_array::Field_assoc_array(uchar *ptr_arg,
                                     const LEX_CSTRING *field_name_arg)
  :Field_composite(ptr_arg, field_name_arg),
   m_table(nullptr),
   m_def(nullptr)
{
  init_tree(&m_tree, 0, 0,
            sizeof(Assoc_array_data), assoc_array_tree_cmp,
            assoc_array_tree_del, NULL,
            MYF(MY_THREAD_SPECIFIC | MY_TREE_WITH_DELETE));

  // Make sure that we cannot insert elements with duplicate keys
  m_tree.flag|= TREE_NO_DUPS;
}


Field_assoc_array::~Field_assoc_array()
{
  delete_tree(&m_tree, 0);

  delete m_table;
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
      src->type_handler() != Type_handler_assoc_array::singleton())
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), m_table->s->fields);
    DBUG_RETURN(true);
  }

  auto item_field_src= src->field_for_view_update();
  if (item_field_src && item_field_src->field == this)
    DBUG_RETURN(false); // Self assignment, let's not do anything.

  src->bring_value();
  auto composite= dynamic_cast<Item_composite_base *>(src);
  DBUG_ASSERT(composite);

  delete_all_elements();

  String src_key;
  if (!composite->get_key(&src_key, true))
  {
    do
    {
      Item **src_elem= composite->element_addr_by_key(thd, NULL, &src_key);
      if (!src_elem)
        goto error;
      
      if (get_element_field()->sp_prepare_and_store_item(thd, src_elem))
        goto error;

      Assoc_array_data data;
      m_item_pack->set_buffer(&data.m_value);
      m_item_pack->pack();

      if (copy_and_convert_key(src_key, &data.m_key))
        goto error;

      if (insert_element(thd, &data, true))
        goto error;

      set_notnull();
    } while (!composite->get_next_key(&src_key, &src_key));
  }

  DBUG_RETURN(false);

error:
  set_null();
  DBUG_RETURN(true);
}


bool Field_assoc_array::insert_element(THD *thd, Assoc_array_data *data,
                                       bool warn_on_dup_key)
{
  DBUG_ASSERT(data->m_key.get_thread_specific());
  DBUG_ASSERT(data->m_value.get_thread_specific());

  if (unlikely(!tree_insert(&m_tree, data, 0, get_key_field())))
  {
    if (warn_on_dup_key && !thd->is_error())
      push_warning_printf(thd,Sql_condition::WARN_LEVEL_WARN,
                          ER_DUP_UNKNOWN_IN_INDEX,
                          ER_THD(thd, ER_DUP_UNKNOWN_IN_INDEX),
                          ErrConvString(data->m_key.ptr(),
                                        data->m_key.length(),
                                        get_key_field()->charset()).ptr());
    return thd->is_error(); // We want to return false on duplicate key
  }

  data->release();

  DBUG_ASSERT(data->m_key.get_thread_specific());
  DBUG_ASSERT(data->m_value.get_thread_specific());

  return false;
}


Item_field *Field_assoc_array::element_by_key(THD *thd, String *key)
{
  if (!key)
    return NULL;

  Assoc_array_data data;
  if (copy_and_convert_key(*key, &data.m_key))
    return nullptr;

  bool is_inserted= false;
  Assoc_array_data *tree_data= assoc_tree_search(&data.m_key);
  if (!tree_data)
  {
    /*
      copy_and_convert_key() alloced key->length()*mbmaxlen bytes for the
      longest possible result of the character set conversion. Let's shrink
      the buffer to the actual length().
    */
    data.m_key.shrink(data.m_key.length());

    // Create an element for the key if not found
    if (insert_element(thd, &data, false))
      return nullptr;
    set_notnull();

    is_inserted= true;

    // "data" is now released. Copy/convert the key again.
    if (copy_and_convert_key(*key, &data.m_key))
      return nullptr;
    tree_data= assoc_tree_search(&data.m_key);
    DBUG_ASSERT(tree_data);
  }

  m_item_pack->set_buffer(&tree_data->m_value);

  if (!is_inserted)
    m_item_pack->unpack();

  return dynamic_cast<Item_field *>(m_item_pack);
}


Item_field *Field_assoc_array::element_by_key(THD *thd, String *key) const
{
  if (!key)
    return NULL;

  String key_copy;
  if (copy_and_convert_key(*key, &key_copy))
    return NULL;

  Assoc_array_data *data= assoc_tree_search(&key_copy);
  if (!data)
    return NULL;

  m_item_pack->set_buffer(&data->m_value);
  m_item_pack->unpack();

  return dynamic_cast<Item_field *>(m_item_pack);
}


static bool convert_charset_with_error(CHARSET_INFO *tocs, String *to,
                                       const String &from,
                                       const char *op,
                                       size_t nchars)
{
  String_copier copier;
  const char *pos;

  if (to->copy(tocs, from.charset(), from.ptr(), from.length(),
               nchars, &copier))
    return true; // EOM

  if (unlikely(pos= copier.well_formed_error_pos()))
  {
    ErrConvString err(pos, from.length() - (pos - from.ptr()),
                      &my_charset_bin);
    my_error(ER_INVALID_CHARACTER_STRING, MYF(0),
             from.charset() == &my_charset_bin ?
             tocs->cs_name.str : from.charset()->cs_name.str,
             err.ptr());
    return true;
  }

  if (unlikely(pos= copier.cannot_convert_error_pos()))
  {
    char buf[16];
    int mblen= from.charset()->charlen(pos, from.end());
    DBUG_ASSERT(mblen > 0 && mblen * 2 + 1 <= (int) sizeof(buf));
    octet2hex(buf, (uchar*)pos, mblen);
    my_error(ER_CANNOT_CONVERT_CHARACTER, MYF(0),
             from.charset()->cs_name.str, buf,
             tocs->cs_name.str);
    return true;
  }

  if (copier.source_end_pos() < from.end())
  {
    my_error(ER_WRONG_STRING_LENGTH, MYF(0),
             ErrConvString(&from).ptr(), op, (int) nchars);
    return true;
  }
  return false;
}


bool Field_assoc_array::copy_and_convert_key(const String &key,
                                             String *key_copy) const
{
  if (get_key_field()->type_handler()->field_type() == MYSQL_TYPE_VARCHAR)
  {
    if (convert_charset_with_error(get_key_field()->charset(), key_copy, key,
                                   "INDEX BY", get_key_field()->char_length()))
      return true;
  }
  else
  {
    uint errors;

    if (unlikely(key_copy->copy(&key, &my_charset_numeric, &errors)))
      return true;
    /*
      Use the non-prepared key_def with the original type handlers.
    */
    auto &key_def= *m_def->begin();
    auto type_handler= dynamic_cast<const Type_handler_general_purpose_int*>
                                                    (key_def.type_handler());
    DBUG_ASSERT(type_handler);

    /*
      Convert the key to a number to perform range check
    */
    char *endptr;
    int error;

    longlong key_ll;
    ulonglong key_ull;

    bool is_unsigned= type_handler->is_unsigned();
    auto cs= get_key_field()->charset();

    key_ull= cs->strntoull10rnd(key_copy->ptr(), key_copy->length(),
                                is_unsigned, &endptr, &error);
    key_ll= (longlong) key_ull;

    if (error || (endptr != key_copy->end()))
    {
      my_error(ER_WRONG_VALUE, MYF(0), "ASSOCIATIVE ARRAY KEY",
               ErrConvString(key_copy).ptr());
      return true;
    }

    if (is_unsigned)
    {
      if (key_ull > type_handler->type_limits_int()->max_unsigned())
        error= 1;
    }
    else
    {
      if (key_ll < type_handler->type_limits_int()->min_signed() ||
          key_ll > type_handler->type_limits_int()->max_signed())
        error= 1;
    }

    if (error)
    {
      my_error(ER_WRONG_VALUE, MYF(0), "ASSOCIATIVE ARRAY KEY",
               ErrConvString(key_copy).ptr());
      return true;
    }

    key_copy->length(0);
    if (unlikely(key_copy->alloc(8)))
      return true;
    
    key_copy->q_append_int64(key_ll);
  }

  return false;
}


bool Field_assoc_array::unpack_key(const Binary_string &key,
                                   String *key_dst) const
{
  auto &key_def= *m_def->begin();
  if (key_def.type_handler()->field_type() == MYSQL_TYPE_VARCHAR)
  {
    if (key_dst->copy(key.ptr(), key.length(), get_key_field()->charset()))
      return true;
  }
  else
  {
    auto type_handler= dynamic_cast<const Type_handler_general_purpose_int*>
                                                    (key_def.type_handler());
    const bool is_unsigned= type_handler->is_unsigned();
    /*
      Reset the string length before appending
    */
    key_dst->length(0);

    if (unlikely(key_dst->alloc(type_handler->
                                  type_limits_int()->char_length())))
      return true;
    if (is_unsigned)
    {
      auto key_val= uint8korr(key.ptr());
      key_dst->qs_append(key_val);
    }
    else
    {
      auto key_val= sint8korr(key.ptr());
      key_dst->qs_append_int64(key_val);
    }

    key_dst->set_charset(&my_charset_numeric);
  }

  return false;
}


#ifndef DBUG_OFF
static void dbug_print_defs(THD *thd, const char *prefix,
                            const Spvar_definition &key,
                            const Spvar_definition &val)
{
  DBUG_EXECUTE_IF("assoc_array",
                  push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                    ER_YES, "%skey: len=%-4u cs=%s", prefix,
                    (uint) key.length, key.charset->coll_name.str););
  DBUG_EXECUTE_IF("assoc_array",
                  push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                    ER_YES, "%sval: len=%-4u cs=%s", prefix,
                    (uint) val.length,
                    val.charset->coll_name.str););

  if (Row_definition_list *row= val.row_field_definitions())
  {
    uint i= 0;
    List_iterator<Spvar_definition> it(*row);
    for (Spvar_definition *def= it++; def; def= it++, i++)
    {
      DBUG_EXECUTE_IF("assoc_array",
                  push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                    ER_YES, "%s[%u]: len=%-4u cs=%s", prefix, i,
                    (uint) def->length,
                    def->charset->coll_name.str););
    }
  }
}
#endif // DBUG_OFF


Field *Field_assoc_array::get_key_field() const
{
  DBUG_ASSERT(m_table);
  return m_table->field[0];
}


Field *Field_assoc_array::get_element_field() const
{
  DBUG_ASSERT(m_table);
  return m_table->field[1];
}


/*
  We willl create 3 subfields in the associative array:
   1. The key field
   2. The value field
   3. The value assign field
*/
bool Field_assoc_array::create_fields(THD *thd)
{
  List<Spvar_definition> field_list;

  Spvar_definition key_def;
  if (init_key_def(thd, &key_def))
    return true;
  field_list.push_back(&key_def);

  /*
    Initialize the value definition
  */
  auto &value_def= *(++m_def->begin());
  Spvar_definition value_rdef; // A resolved definition, for %ROWTYPE

  if (value_def.is_column_type_ref())
  {
    if (value_def.column_type_ref()->resolve_type_ref(thd, &value_rdef) ||
        field_list.push_back(&value_rdef))
      return true;
  }
  else
  {
    if (field_list.push_back(&value_def))
      return true;
  }

  // Create another copy of the value field definition for assignment
  field_list.push_back(field_list.elem(1));

  DBUG_EXECUTE_IF("assoc_array",
                  dbug_print_defs(thd, "create_fields: ",
                                  key_def, value_def););

  /*
    Create the fields
  */
  if (!(m_table= create_virtual_tmp_table(thd, field_list)))
    return true;

  /*
    Assign the array's field name to it's element field. We want
    any error messages that uses the field_name to use the array's
    name.
  */
  for (uint i= 1; i <= 2; i++)
  {
    DBUG_ASSERT(m_table->field[i]);
    m_table->field[i]->field_name= field_name;
  }

  return false;
}


bool Field_assoc_array::init_key_def(THD *thd, Spvar_definition *key_def) const
{
  DBUG_ASSERT(key_def);

  *key_def= *m_def->begin();

  if (key_def->type_handler()->field_type() != MYSQL_TYPE_VARCHAR)
  {
    DBUG_ASSERT(dynamic_cast<const Type_handler_general_purpose_int*>
                                        (key_def->type_handler()));

    if (key_def->type_handler()->is_unsigned())
      key_def->set_handler(&type_handler_ulonglong);
    else
      key_def->set_handler(&type_handler_slonglong);
  }

  /*
    Now call sp_prepare_create_field().
    - for integer types it'll set the key_def.pack_flag to be valid
      so that signedness can be determined
    - for varchar it'll evaluate charset and set max octet length according
      to the charset mbmaxlen and the character length
  */
  {
    /*
      Disallow VARCHAR->TEXT conversion for the INDEX BY field.
      Let's have warnings always escalated to errors during
      sp_prepare_create_field().
    */
    Abort_on_warning_instant_set frame_abort_on_warning(thd, true);
    Sql_mode_instant_set frame_sql_mode(thd, thd->variables.sql_mode |
                                        MODE_STRICT_ALL_TABLES);
    DBUG_ASSERT(thd->really_abort_on_warning());
    if (key_def->sp_prepare_create_field(thd, thd->mem_root))
      return true; // E.g. VARCHAR size is too large
  }

  return false;
}


bool Field_assoc_array::init_element_base(THD *thd)
{
  if (unlikely(m_table))
    return false;

  if (unlikely(create_fields(thd)))
    return true;
  
  if (unlikely(!(m_item_pack= create_packable(thd, get_element_field()))))
    return true;

  m_item= dynamic_cast<Item *>(m_item_pack);
  DBUG_ASSERT(m_item);

  auto item_pack_assign= create_packable(thd, m_table->field[2]);
  if (unlikely(!item_pack_assign))
    return true;

  m_item_pack->set_assign(item_pack_assign);

  return false;
}


Item_field_packable *Field_assoc_array::create_packable(THD *thd, Field *field)
{
  Item_field_packable *packable;
  Field_row *field_row= dynamic_cast<Field_row*>(field);
  if (field_row)
  {
    auto &value_def= *(++m_def->begin());
    if (field_row->row_create_fields(thd, value_def))
      return nullptr;
    
    auto pack_row= new (thd->mem_root)
                    Item_field_packable_row(thd,
                                            field);
    if (pack_row->add_array_of_item_field(thd))
      return nullptr;
    
    packable= pack_row;
  }
  else
    packable= new (thd->mem_root)
                      Item_field_packable_scalar(thd,
                                                 field);

  return packable;
}


Item_field *
Field_assoc_array::make_item_field_spvar(THD *thd,
                                         const Spvar_definition &def)
{
  auto item= new (thd->mem_root) Item_field_assoc_array(thd, this);
  if (!item)
    return nullptr;

  item->set_array_def(thd, def.row_field_definitions());

  if (init_element_base(thd))
    return nullptr;

  return item;
}


Item **Field_assoc_array::element_addr_by_key(THD *thd, String *key)
{
  if (!key)
    return NULL;

  String key_copy;
  if (copy_and_convert_key(*key, &key_copy))
    return NULL;

  Assoc_array_data *data= assoc_tree_search(&key_copy);
  if (!data)
    return NULL;

  m_item_pack->set_buffer(&data->m_value);
  m_item_pack->unpack();

  return &m_item;
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
  
  String key_copy;
  if (copy_and_convert_key(*key, &key_copy))
    return true;

  (void) tree_delete(&m_tree, &key_copy, 0, get_key_field());
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
    unpack_key(data->m_key, key);
    return false;
  }

  return true;
}


bool Field_assoc_array::get_next_key(const String *curr_key, String *next_key)
{
  return get_next_or_prior_key(curr_key, next_key, true);
}


bool Field_assoc_array::get_prior_key(const String *curr_key, String *prior_key)
{
  return get_next_or_prior_key(curr_key, prior_key, false);
}


bool Field_assoc_array::get_next_or_prior_key(const String *curr_key,
                                              String *new_key,
                                              bool is_next)
{
  DBUG_ASSERT(new_key);

  TREE_ELEMENT **last_pos;
  TREE_ELEMENT *parents[MAX_TREE_HEIGHT+1];

  if (!curr_key)
    return true;
  
  String key_copy;
  if (copy_and_convert_key(*curr_key, &key_copy))
    return true;

  Assoc_array_data *data= (Assoc_array_data *)
                          tree_search_key(&m_tree,
                                          &key_copy, 
                                          parents,
                                          &last_pos,
                                          is_next ? HA_READ_AFTER_KEY :
                                                    HA_READ_BEFORE_KEY,
                                          get_key_field());
  if (data)
  {
    unpack_key(data->m_key, new_key);
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
           system_charset_info);
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
                    system_charset_info);
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


bool Item_splocal_assoc_array_base::fix_key(THD *thd,
                                            const sp_rcontext_addr &array_addr)
{
  Field *generic_field= thd->get_variable(array_addr)->field;
  auto field= dynamic_cast<const Field_assoc_array*>(generic_field);
  return m_key->fix_fields_if_needed(thd, &m_key) ||
         Type_handler_assoc_array::
            check_subscript_expression(field->get_key_def()->type_handler(),
                                       m_key);
}


bool Item_splocal_assoc_array_base::is_element_exists(THD *thd,
                                                  const Field_composite *field,
                                                  const LEX_CSTRING &name)
                                                  const
{
  DBUG_ASSERT(field);
  DBUG_ASSERT(m_key->fixed());

  StringBufferKey buffer;
  auto key_str= m_key->val_str(&buffer);
  if (!key_str)
  {
    my_error(ER_NULL_FOR_ASSOC_ARRAY_INDEX, MYF(0),
             name.str);
    return false;
  }

  if (field->element_by_key(thd, key_str) == nullptr)
  {
    my_error(ER_ASSOC_ARRAY_ELEM_NOT_FOUND, MYF(0),
             ErrConvString(key_str).ptr());
    return false;
  }
  return true;
}


bool Item_splocal_assoc_array_element::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);

  if (fix_key(thd, rcontext_addr()))
    return true;

  auto field= get_composite_variable(thd->spcont)->get_composite_field();
  DBUG_ASSERT(field);

  if (!is_element_exists(thd, field, m_name))
    return true;

  auto item= field->get_element_item();
  DBUG_ASSERT(item);

  set_handler(item->type_handler());
  return fix_fields_from_item(thd, ref, item);
}


Item *
Item_splocal_assoc_array_element::this_item()
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(m_key->fixed());
  StringBufferKey buffer;
  return get_composite_variable(m_thd->spcont)->
            element_by_key(m_thd, m_key->val_str(&buffer));
}


const Item *
Item_splocal_assoc_array_element::this_item() const
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(m_key->fixed());
  StringBufferKey buffer;
  return get_composite_variable(m_thd->spcont)->
            element_by_key(m_thd, m_key->val_str(&buffer));
}


Item **
Item_splocal_assoc_array_element::this_item_addr(THD *thd, Item **ref)
{
  DBUG_ASSERT(m_sp == thd->spcont->m_sp);
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(m_key->fixed());
  StringBufferKey buffer;
  return get_composite_variable(thd->spcont)->
           element_addr_by_key(m_thd, ref, m_key->val_str(&buffer));
}


void Item_splocal_assoc_array_element::print(String *str, enum_query_type type)
{
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();
  str->append(prefix);
  str->append(&m_name);
  str->append('@');
  str->append_ulonglong(m_var_idx);
  str->append('[');
  m_key->print(str, type);
  str->append(']');
}


bool Item_splocal_assoc_array_element::set_value(THD *thd,
                                                 sp_rcontext *ctx,
                                                 Item **it)
{
  StringBufferKey buffer;
  const LEX_CSTRING key= Type_handler_assoc_array::singleton()->
        key_to_lex_cstring(thd, rcontext_addr(), &m_key, &buffer);
  if (!key.str)
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

  if (fix_key(thd, rcontext_addr()))
    return true;

  auto field= get_composite_variable(thd->spcont)->get_composite_field();
  DBUG_ASSERT(field);

  if (!is_element_exists(thd, field, m_name))
    return true;

  auto element_item= field->get_element_item();
  DBUG_ASSERT(element_item);

  auto element_handler= element_item->type_handler()->to_composite();
  if (!element_handler ||
      element_handler->get_item_index(thd,
                                      element_item->field_for_view_update(),
                                      m_field_name,
                                      m_field_idx))
  {
    my_error(ER_BAD_FIELD_ERROR, MYF(0), m_field_name.str, m_name.str);
    return true;
  }

  Item *item= element_item->element_index(m_field_idx);
  DBUG_ASSERT(item);
  set_handler(item->type_handler());
  return fix_fields_from_item(thd, ref, item);
}


Item *
Item_splocal_assoc_array_element_field::this_item()
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);
  DBUG_ASSERT(fixed());

  StringBufferKey buffer;
  auto elem= get_composite_variable(m_thd->spcont)->
                element_by_key(m_thd, m_key->val_str(&buffer));
  if (!elem)
    return nullptr;

  return elem->element_index(m_field_idx);
}


const Item *
Item_splocal_assoc_array_element_field::this_item() const
{
  DBUG_ASSERT(m_sp == m_thd->spcont->m_sp);
  DBUG_ASSERT(fixed());

  StringBufferKey buffer;
  auto elem= get_composite_variable(m_thd->spcont)->
                element_by_key(m_thd, m_key->val_str(&buffer));
  if (!elem)
    return nullptr;

  return elem->element_index(m_field_idx);
}


Item **
Item_splocal_assoc_array_element_field::this_item_addr(THD *thd, Item **)
{
  DBUG_ASSERT(m_sp == thd->spcont->m_sp);
  DBUG_ASSERT(fixed());

  StringBufferKey buffer;
  auto elem= get_composite_variable(m_thd->spcont)->
                element_by_key(m_thd, m_key->val_str(&buffer));
  if (!elem)
    return nullptr;

  return elem->addr(m_field_idx);
}


void Item_splocal_assoc_array_element_field::print(String *str,
                                                   enum_query_type type)
{
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();
  str->append(prefix);
  str->append(&m_name);
  str->append('@');
  str->append_ulonglong(m_var_idx);
  str->append('[');
  m_key->print(str, type);
  str->append(']');
  str->append('.');
  str->append(&m_field_name);
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
protected:
  Item *m_key;
  // Return the element definition as specified in the TABLE OF clause
  const Spvar_definition &get_element_definition(THD *thd) const
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
    return *(it++);
  };

public:
  my_var_sp_assoc_array_element(const Lex_ident_sys_st &varname, Item *key,
                                const sp_rcontext_addr &addr, sp_head *s)
   :my_var_sp(varname, addr,
              Type_handler_assoc_array::singleton(), s),
    m_key(key)
  { }

  bool check_assignability(THD *thd, const List<Item> &select_list,
                           bool *assign_as_row) const override
  {
    const Spvar_definition &table_of= get_element_definition(thd);
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
    StringBufferKey buffer;
    const LEX_CSTRING key= Type_handler_assoc_array::singleton()->
        key_to_lex_cstring(thd, *this, &m_key, &buffer);
    if (!key.str)
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


class my_var_sp_assoc_array_element_field: public my_var_sp_assoc_array_element
{
protected:
  const Lex_ident_sys_st m_field_name;

public:
  my_var_sp_assoc_array_element_field(const Lex_ident_sys_st &varname,
                                      Item *key,
                                      const Lex_ident_sys_st &field_name,
                                      const sp_rcontext_addr &addr, sp_head *s)
   :my_var_sp_assoc_array_element(varname, key, addr, s),
    m_field_name(field_name)
  { }
  bool check_assignability(THD *thd, const List<Item> &select_list,
                           bool *assign_as_row) const override
  {
    const Spvar_definition &table_of= get_element_definition(thd);

    auto row_defs= table_of.row_field_definitions();
    if (unlikely(!row_defs))
      return true;

    uint offset= 0;
    auto field_spv= row_defs->find_row_field_by_name(&m_field_name, &offset);
    if (unlikely(!field_spv))
      return true;

    // TABLE OF does not support nested ROWs yet
    DBUG_ASSERT(field_spv->row_field_definitions() == nullptr);
    return select_list.elements != 1;
  }
  bool set(THD *thd, Item *item) override
  {
    StringBufferKey buffer;
    const LEX_CSTRING key= Type_handler_assoc_array::singleton()->
        key_to_lex_cstring(thd, *this, &m_key, &buffer);
    if (!key.str)
      return true;

    return get_rcontext(thd->spcont)->
              set_variable_composite_field_by_key(thd, offset(),
                                                  key, m_field_name, &item);
  }
  bool set_row(THD *thd, List<Item> &select_list) override
  {
    DBUG_ASSERT(0); // TABLE OF does not support nested ROWs yet
    return true;
  }
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


bool Type_handler_assoc_array::
       check_subscript_expression(const Type_handler *formal_th, Item *key)
{
  Type_handler_hybrid_field_type th(formal_th);
  if (th.aggregate_for_result(key->type_handler()))
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
             key->type_handler()->name().ptr(), "<subscript expression>");
    return true;
  }
  if (!key->can_eval_in_optimize())
  {
    Item::Print tmp(key, QT_ORDINARY);
    my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), ErrConvString(&tmp).ptr());
    return true;
  }
  return false;
}


bool Type_handler_assoc_array::
       Column_definition_set_attributes(THD *thd,
                                        Column_definition *def,
                                        const Lex_field_type_st &attr,
                                        column_definition_type_t type)
                                                                 const
{
  const sp_type_def_composite2 *tdef;
  /*
    Disallow wrong use of associative_array:
      CREATE TABLE t1 (a ASSOCIATIVE_ARRAY);
      CREATE FUNCTION .. RETURN ASSOCIATEIVE ARRAY ..;
  */
  if (!(tdef= reinterpret_cast<const sp_type_def_composite2*>
                           (def->get_attr_const_void_ptr(0))))
  {
    my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), name().ptr());
    return true;
  }

  if (unlikely(tdef->m_def[1]->type_handler() == this))
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
             tdef->m_def[1]->type_handler()->name().ptr(),
             "<array element data type>");
    return true;
  }
  if (unlikely(tdef->m_def[0]->type_handler() != &type_handler_varchar &&
               !dynamic_cast<const Type_handler_general_purpose_int*>
                                            (tdef->m_def[0]->type_handler())))
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
             tdef->m_def[0]->type_handler()->name().ptr(),
             "<array index data type>");
    return true;
  }

  return Type_handler_composite::Column_definition_set_attributes(thd, def,
                                                                  attr,
                                                                  type);
}


bool Type_handler_assoc_array::
  sp_variable_declarations_finalize(THD *thd, LEX *lex, int nvars,
                                    const Column_definition &def)
                                                            const
{
  const sp_type_def_composite2 *spaa=
                                  static_cast<const sp_type_def_composite2*>
                                    (def.get_attr_const_void_ptr(0));
  DBUG_ASSERT(spaa);
  DBUG_ASSERT(spaa->m_def[0]);
  DBUG_ASSERT(spaa->m_def[1]);
  Spvar_definition *key_def= spaa->m_def[0];
  Spvar_definition *value_def= spaa->m_def[1];

  value_def= new (thd->mem_root) Spvar_definition(*value_def);
  if (value_def->type_handler() == &type_handler_row)
  {
    if (const sp_type_def_record *sprec=
        (sp_type_def_record *)value_def->get_attr_const_void_ptr(0))
    {
      /*
        Hack to ensure that we don't call sp_head::row_fill_field_definitions()
        for the same row definition twice.

        Check the pack_flag of the first field in the row definition.
        FIELDFLAG_MAYBE_NULL will be set if the row_fill_field_definitions()
        has been called.
      */
      List_iterator<Spvar_definition> it(*sprec->field);
      auto first= sprec->field->head();

      if (first && !(first->pack_flag & FIELDFLAG_MAYBE_NULL) &&
          lex->sphead->row_fill_field_definitions(thd, sprec->field))
        return true;

      value_def->set_row_field_definitions(&type_handler_row, sprec->field);
    }
  }

  if (lex->sphead->fill_spvar_definition(thd, value_def))
    return true;

  Row_definition_list *aa_def= new (thd->mem_root) Row_definition_list();
  if (unlikely(aa_def == nullptr))
    return true;

  aa_def->push_back(key_def, thd->mem_root);
  aa_def->push_back(value_def, thd->mem_root);

  for (uint i= 0 ; i < (uint) nvars ; i++)
  {
    uint offset= (uint) nvars - 1 - i;
    sp_variable *spvar= lex->spcont->get_last_context_variable(offset);
    spvar->field_def.set_row_field_definitions(this, aa_def);
    if (lex->sphead->fill_spvar_definition(thd, &spvar->field_def,
                                           &spvar->name))
      return true;
  }

  return false;
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

bool Type_handler_assoc_array::check_key_expression_type(Item *key)
{
  Item_func::Functype func_sp= Item_func::FUNC_SP;
  if (bool(key->with_flags & (item_with_t::WINDOW_FUNC |
                              item_with_t::FIELD |
                              item_with_t::SUM_FUNC |
                              item_with_t::SUBQUERY |
                              item_with_t::ROWNUM_FUNC)) ||
      key->walk(&Item::find_function_processor, FALSE, &func_sp))
  {
    Item::Print tmp(key, QT_ORDINARY);
    my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), ErrConvString(&tmp).ptr());
    return true;
  }
  return false;
}


/*
  Check arguments for:
    assoc_array_var(key)
    assoc_array_var(key).field
*/
bool Type_handler_assoc_array::check_functor_args(THD *thd, List<Item> *args,
                                                  const char *op)
{
  if (!args || args->elements != 1 || !args->head())
  {
    my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0), op,
             ErrConvDQName(thd->lex->sphead).ptr(),
             1, !args ? 0 : args->elements);
    return true;
  }
  return check_key_expression_type(args->head());
}


Item_splocal *
Type_handler_assoc_array::create_item_functor(THD *thd,
                                              const Lex_ident_sys &varname,
                                              const sp_rcontext_addr &addr,
                                              List<Item> *args,
                                              const Lex_ident_sys &member,
                                              const Lex_ident_cli_st &name_cli)
                                                                          const
{
  DBUG_ASSERT(!varname.is_null());
  if (check_functor_args(thd, args, "ASSOC_ARRAY_ELEMENT"))
    return nullptr;

  Item *key= args->head();

  Query_fragment pos(thd, thd->lex->sphead, name_cli.pos(), name_cli.end());
  if (!member.is_null())
  {
    return new (thd->mem_root)
      Item_splocal_assoc_array_element_field(thd, addr, varname, key,
                                             member, &type_handler_null,
                                             pos.pos(), pos.length());
  }

  return new (thd->mem_root)
      Item_splocal_assoc_array_element(thd, addr, varname, key,
                                       &type_handler_null,
                                       pos.pos(), pos.length());
}


/*
  Make instructions for:
    assoc_array('key')         := expr;
    assoc_array('key').member  := expr;
*/

sp_instr *
Type_handler_assoc_array::
  create_instr_set_assign_functor(THD *thd,
                                  LEX *lex,
                                  const Qualified_ident &ident,
                                  const sp_rcontext_addr &addr,
                                  List<Item> *args,
                                  const Lex_ident_sys_st &member,
                                  Item *expr,
                                  const LEX_CSTRING &expr_str) const
{
  using set_element=        sp_instr_set_composite_field_by_name;
  using set_element_member= sp_instr_set_composite_field_by_key;

  if (!ident.part(1).is_null())
  {
    raise_bad_data_type_for_functor(ident);
    return nullptr;
  }

  if (check_functor_args(thd, args, "ASSOC_ARRAY KEY"))
    return nullptr;

  if (member.is_null())
    return new (thd->mem_root) set_element(lex->sphead->instructions(),
                                           lex->spcont, addr, args->head(),
                                           expr, lex, true, expr_str);

  return new (thd->mem_root) set_element_member(lex->sphead->instructions(),
                                                lex->spcont, addr,
                                                args->head(),
                                                member,
                                                expr, lex, true, expr_str);
}


Item *Type_handler_assoc_array::create_item_method_func(THD *thd,
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


Item *Type_handler_assoc_array::create_item_method_proc(THD *thd,
                                                        const Lex_ident_sys &a,
                                                        const Lex_ident_sys &b,
                                                        List<Item> *args,
                                                        const Lex_ident_cli_st
                                                          &query_fragment) const
{
  Item_method_base *item= nullptr;
  if (b.length == 6)
  {
    if (Lex_ident_column(b).streq("DELETE"_Lex_ident_column))
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


LEX_CSTRING
Type_handler_assoc_array::key_to_lex_cstring(THD *thd,
                                             const sp_rcontext_addr &var,
                                             Item **key,
                                             String *buffer) const
{
  DBUG_ASSERT(key);
  DBUG_ASSERT(*key);

  auto const item_field= thd->get_variable(var);
  auto const field= dynamic_cast<const Field_assoc_array*>(item_field->field);
  const Field *key_field= field->get_key_field();

  if ((*key)->fix_fields_if_needed(thd, key) ||
      check_subscript_expression(key_field->type_handler(), *key))
    return Lex_cstring();

  if (key_field->type_handler()->field_type() != MYSQL_TYPE_VARCHAR)
  {
    auto str= (*key)->val_str(buffer);
    if (!str)
    {
     my_error(ER_NULL_FOR_ASSOC_ARRAY_INDEX, MYF(0), field->field_name.str);
     return Lex_cstring();
    }
    return str->to_lex_cstring();
  }

  StringBufferKey tmp;
  auto str= (*key)->val_str(&tmp);

  if (!str)
  {
    my_error(ER_NULL_FOR_ASSOC_ARRAY_INDEX, MYF(0), field->field_name.str);
    return Lex_cstring();
  }

  if (convert_charset_with_error(key_field->charset(), buffer, *str,
                                 "INDEX BY",
                                 key_field->char_length()))
    return Lex_cstring();
  return buffer->to_lex_cstring();
}


Item_field *Type_handler_assoc_array::get_item(THD *thd,
                                               const Item_field *item,
                                               const LEX_CSTRING& name) const
{
  DBUG_ASSERT(item);

  auto item_assoc= dynamic_cast<const Item_field_assoc_array *>(item);
  if (!item_assoc)
    return nullptr;

  const Field_assoc_array *field= dynamic_cast<const Field_assoc_array*>
                                                     (item_assoc->field);
  if (!field)
    return nullptr;

  /*
    The key passed in "name" must be in the character set
    explicitly or implicitly specified in the INDEX BY clause.
  */
  CHARSET_INFO *key_cs= field->get_key_field()->charset();
  DBUG_ASSERT(name.length == Well_formed_prefix(key_cs, name).length());

  String key(name.str, name.length, key_cs);
  auto elem= field->element_by_key(thd, &key);
  if (!elem)
  {
    my_error(ER_ASSOC_ARRAY_ELEM_NOT_FOUND, MYF(0),
             ErrConvString(&key).ptr());
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

  Field_assoc_array *field= dynamic_cast<Field_assoc_array*>
                                         (item_assoc->field);
  if (!field)
    return nullptr;

  /*
    The key passed in "name" must be in the character set
    explicitly or implicitly specified in the INDEX BY clause.
  */
  CHARSET_INFO *key_cs= field->get_key_field()->charset();
  DBUG_ASSERT(name.length == Well_formed_prefix(key_cs, name).length());

  String key(name.str, name.length, key_cs);
  return field->element_by_key(thd, &key);
}


Item_field* Type_handler_assoc_array::prepare_for_set(Item_field *item) const
{
  Item_field_packable *item_elem= dynamic_cast<Item_field_packable *>(item);
  if (!item_elem)
    return nullptr;

  auto assign= item_elem->get_assign();
  assign->unpack();
  return dynamic_cast<Item_field*>(assign);
}


bool Type_handler_assoc_array::finalize_for_set(Item_field *item) const
{
  Item_field_packable *item_elem= dynamic_cast<Item_field_packable *>(item);
  if (!item_elem)
    return false;

  return item_elem->pack();
}


/*
  SELECT 1 INTO spvar(arg);
  SELECT 1 INTO spvar(arg).field_name;
*/
my_var *Type_handler_assoc_array::
  make_outvar_lvalue_functor(THD *thd, const Lex_ident_sys_st &name,
                             Item *arg, const Lex_ident_sys &field_name,
                             sp_head *sphead,
                             const sp_rcontext_addr &addr,
                             bool validate_only) const
{
  auto spvar= thd->lex->spcont->get_pvariable(addr);
  DBUG_ASSERT(spvar);
  auto def= spvar->field_def.row_field_definitions();
  DBUG_ASSERT(def);
  DBUG_ASSERT(def->elements == 2);

  if (check_key_expression_type(arg))
    return nullptr;

  if (!field_name.str) // SELECT .. INTO spvar_assoc_array('key');
  {
    if (validate_only)
      return nullptr; // e.g. EXPLAIN SELECT .. INTO spvar_assoc_array('key');
    return new (thd->mem_root) my_var_sp_assoc_array_element(name, arg, addr,
                                                             sphead);
  }

  // SELECT .. INTO spvar_assoc_array('key').field;
  List_iterator<Spvar_definition> it(*const_cast<Row_definition_list*>(def));
  it++; // Skip the INDEX BY definition
  const Spvar_definition &table_of= *(it++); // The TABLE OF definition

  uint field_offset= 0;
  if (!table_of.row_field_definitions())
  {
    table_of.type_handler()->
      raise_bad_data_type_for_functor(Qualified_ident(name), field_name);
    return nullptr;
  }

  if (!table_of.row_field_definitions()->find_row_field_by_name(&field_name,
                                                                &field_offset))
  {
    my_error(ER_BAD_FIELD_ERROR, MYF(0), field_name.str, name.str);
    return nullptr;
  }

  if (validate_only)
    return nullptr; // e.g. EXPLAIN SELECT..INTO spvar_assoc_array('key').field;
  return new (thd->mem_root) my_var_sp_assoc_array_element_field(name, arg,
                                                                 field_name,
                                                                 addr,
                                                                 sphead);
}


// assoc_array_var:= assoc_array_type('key1'=>'val1', 'key2'=>'val2')
Item *Type_handler_assoc_array::
  make_typedef_constructor_item(THD *thd, const sp_type_def &def,
                                List<Item> *args) const
{
  if (unlikely(args == NULL))
    return new (thd->mem_root) Item_assoc_array(thd, def.get_name());

  if (unlikely(sp_check_assoc_array_args(def, *args)))
    return nullptr;

  return new (thd->mem_root) Item_assoc_array(thd, def.get_name(), *args);
}


/************************************************************************/

static Type_handler_assoc_array type_handler_assoc_array;


const Type_handler_assoc_array *Type_handler_assoc_array::singleton()
{
  return &type_handler_assoc_array;
}


static struct st_mariadb_data_type plugin_descriptor_type_assoc_array=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  &type_handler_assoc_array
};


maria_declare_plugin(type_assoc_array)
{
  MariaDB_DATA_TYPE_PLUGIN,     // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_type_assoc_array, // a pointer to the plugin descriptor
  "associative_array",          // plugin name
  "Rakuten Securities",         // plugin author
  "Data type ASSOCIATIVE_ARRAY",// the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL // Maturity(see include/mysql/plugin.h)*/
}
maria_declare_plugin_end;
