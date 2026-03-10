/*
   Copyright (c) 2019, MariaDB Corporation

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

#define MYSQL_SERVER

#include <my_global.h>
#include <sql_class.h>
#include <mysql/plugin_function.h>
#include <sql_prepare.h>
#include <sp_head.h>
#include <sp_rcontext.h>
#include <sp_cache.h>


/*** General purpose routines ************************************************/


/*
  SQL_SET_PARAM_BY_NAME(ps_name VARCHAR,
                        param_name VARCHAR,
                        value <datatype>)
*/
class Item_func_sql_set_param_by_name: public Item_bool_func
{
public:
  using Item_bool_func::Item_bool_func;
  using Create= TFuncPluginCreate<Create_func_arg3,
                                  Item_func_sql_set_param_by_name,
                                  Create_func::TYPE_PACKAGE_PUBLIC_PROCEDURE>;

  bool set_param_by_name(THD *thd, const Lex_ident_sys &ps_name,
                         Item *param_name_item, Item *param_value_item)
  {
    StringBuffer<64> param_name_value_buffer, param_name_conv_buffer;
    String *param_name= param_name_item->val_str(&param_name_value_buffer,
                                                 &param_name_conv_buffer,
                                                 system_charset_info);
    if ((null_value= param_name == nullptr))
      return 0;
    param_name->c_ptr(); // Lex_ident_column expects a 0-terminated string
    null_value= mysql_sql_stmt_set_placeholder_by_name(thd,
                   ps_name,
                   Lex_ident_column(param_name->to_lex_cstring()),
                   param_value_item);
    return false;
  }
  bool val_bool() override
  {
    StringBuffer<64> ps_name_value_buffer, ps_name_conv_buffer;
    String *ps_name= args[0]->val_str(&ps_name_value_buffer,
                                      &ps_name_conv_buffer,
                                      system_charset_info);
    if ((null_value= ps_name == nullptr))
      return 0;
    return set_param_by_name(current_thd, Lex_ident_sys(ps_name->ptr(),
                                                        ps_name->length()),
                             args[1], args[2]);
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "sql_set_param_by_name"_LEX_CSTRING;
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_sql_set_param_by_name>(thd, this);
  }
};


/*
  SQL_COLUMN_VALUE(sys_refcursor_id INTEGER,
                   position INTEGER,
                   destination OUT <datatype>)
*/
class Item_func_sql_column_value: public Item_bool_func
{
public:
  using Item_bool_func::Item_bool_func;
  using Create= TFuncPluginCreate<Create_func_arg3,
                                  Item_func_sql_column_value,
                                  Create_func::TYPE_PACKAGE_PUBLIC_PROCEDURE>;

  bool check_arguments() const override
  {
    for (uint i= 0; i < arg_count; i++)
    {
      // See comments about REF_ITEM in Item::check_type_scalar
      DBUG_ASSERT(args[i]->fixed() || args[i]->type() == REF_ITEM);
      const Type_handler *handler= args[i]->type_handler();
      if (!handler->is_scalar_type())
      {
        my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
                 handler->name().ptr(), func_name());
        return true;
      }
    }
    Settable_routine_parameter *dst= args[2]->get_settable_routine_parameter();
    if (!dst)
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
      return true;
    }
    return false;
  }
  bool column_value(THD *thd, Item *sys_refcursor_id_item,
                    Item *pos_item, Item *dst_item)
  {
    Longlong_hybrid_null id= sys_refcursor_id_item->to_longlong_hybrid_null();
    if ((null_value= id.is_null() || id.neg() ||
         thd->statement_cursors()->elements() <= (size_t) id.value() ||
         !thd->statement_cursors()->at(id.value()).is_open()))
    {
      my_error(ER_SP_CURSOR_MISMATCH, MYF(0), id.is_null() ? "NULL" :
                                              ErrConvInteger(id).ptr());
      return true;
    }
    sp_cursor_array_element *cursor= &thd->statement_cursors()->at(id.value());
    longlong pos= pos_item->val_int();
    if ((null_value= pos_item->null_value || pos < 1 || pos > cursor->cols()))
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
      return false;
    }
    Settable_routine_parameter *dst= dst_item->get_settable_routine_parameter();
    DBUG_ASSERT(dst);
    null_value= cursor->column_value(thd, (uint) pos - 1, dst);
    return false;
  }

  bool val_bool() override
  {
    return column_value(current_thd, args[0], args[1], args[2]);
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "sql_column_value"_LEX_CSTRING;
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_sql_column_value>(thd, this);
  }
};


class Item_func_sql_refcursor_try_fetch: public Item_longlong_func
{
public:
  using Item_longlong_func::Item_longlong_func;

// TODO: check it's a cursor: can_return_int returns false
//  bool check_arguments() const override
//  {
//    const LEX_CSTRING op= func_name_cstring();
//    if (args[0]->check_type_can_return_int(op))
//      return true;
//    return false;
//  }

  bool fix_length_and_dec(THD *thd) override
  {
    max_length= 21;
    unsigned_flag= true;
    return false;
  }

  longlong val_int() override
  {
    THD *thd= current_thd;
    Longlong_hybrid_null id= args[0]->to_longlong_hybrid_null();
    if ((null_value= id.is_null() || id.neg() ||
         thd->statement_cursors()->elements() <= (size_t) id.value() ||
         !thd->statement_cursors()->at(id.value()).is_open()))
    {
      my_error(ER_SP_CURSOR_MISMATCH, MYF(0), id.is_null() ? "NULL" :
                                              ErrConvInteger(id).ptr());
      return 0;
    }
    longlong num_rows= args[1]->val_int();
    if ((null_value= args[1]->null_value ||
                     (num_rows < 0 && !args[1]->unsigned_flag)))
      return 0;

    sp_cursor_array_element *cursor= &thd->statement_cursors()->at(id.value());
    null_value= false;
    return cursor->try_fetch((ulonglong) num_rows);
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "sql_refcursor_try_fetch"_LEX_CSTRING;
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_sql_refcursor_try_fetch>(thd, this);
  }

  class Create: public Create_native_func
  {
  public:
    Item *create_native(THD *thd, const LEX_CSTRING *name,
                        List<Item> *item_list) override
    {
      if (!item_list || item_list->elements != 2)
      {
        my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
        return nullptr;
      }
      return new (thd->mem_root) Item_func_sql_refcursor_try_fetch(thd,
                                                                   *item_list);
    }
    static Plugin_function *plugin_descriptor()
    {
      static Create creator;
      static Plugin_function descriptor(&creator);
      return &descriptor;
    }
  };

};



/*** DBMS_SQL specific routines **********************************************/


class RContext_package_body
{
public:
  sp_package *m_package_body;
  Item_field *m_assoc0;
  Item_field *m_assoc1;
  Item_composite_base *m_assoc0_composite;
  RContext_package_body()
   :m_package_body(nullptr),
    m_assoc0(nullptr),
    m_assoc1(nullptr),
    m_assoc0_composite(nullptr)
  { }
  bool is_null() const
  {
    return !m_package_body || !m_assoc0 || !m_assoc1 || !m_assoc0_composite;
  }
};


class RContext_assoc0_row
{
public:
  Item *m_assoc0_row_item;
  Item *m_sys_refcursor_id_item;
  RContext_assoc0_row()
   :m_assoc0_row_item(nullptr),
    m_sys_refcursor_id_item(nullptr)
  { }
  bool is_null() const
  {
    return !m_assoc0_row_item || !m_sys_refcursor_id_item;
  }
};


class Dbms_sql_common
{
public:
  static sp_package *find_package_body_dbms_sql(THD *thd)
  {
    if (!thd->spcont)
      return nullptr;
    Database_qualified_name tmp(thd->spcont->m_sp->m_db,
                                "DBMS_SQL"_LEX_CSTRING);
    sp_head *sp= sp_cache_lookup(&thd->sp_package_body_cache, &tmp);
    sp_package *pkg= sp ? sp->get_package() : nullptr;
    return pkg;
  }
  /*
    Find the variable "assoc0" at the position 0.
    For safety, let's check that the name of the variable is really "assoc0".
    This is how 'CREATE PACKAGE BODY DBMS_SQL' is defined.
  */
  static RContext_package_body find_package_body_rcontext(THD *thd)
  {
    RContext_package_body res;
    if (!(res.m_package_body= find_package_body_dbms_sql(thd)) ||
        res.m_package_body->m_rcontext->max_var_index() < 2)
      return res;
    if (!(res.m_assoc0= res.m_package_body->m_rcontext->get_variable(0)) ||
        !res.m_assoc0->name.streq("assoc0"_Lex_ident_column))
      return res;
    if (!(res.m_assoc1= res.m_package_body->m_rcontext->get_variable(1)) ||
        !res.m_assoc1->name.streq("assoc1"_Lex_ident_column))
      return res;
    res.m_assoc0_composite= dynamic_cast<Item_composite_base *>(res.m_assoc0);
    return res;
  }

  /*
    Find a ROW of the variable "assoc0" by the key "key".
    For safety, let's check that the name of the field "cursor_id" is
    on the proper place. It's expected to be at the offset 1.
    This is how 'CREATE PACKAGE BODY DBMS_SQL' is defined.
  */
  static RContext_assoc0_row find_assoc0_row_by_key(THD *thd,
                              const RContext_package_body rcontext_package_body,
                              Item *key)
  {
    RContext_assoc0_row res;
    StringBuffer<64> buffer;
    String *args0str;
    if (rcontext_package_body.is_null() ||
        !(args0str= key->val_str(&buffer)) ||
        !(res.m_assoc0_row_item= rcontext_package_body.m_assoc0_composite->
                                   element_by_key(thd, args0str)))
      return res;
    Item *el= res.m_assoc0_row_item->element_index(1);
    if (!el || !el->name.streq("cursor_id"_Lex_ident_column))
      return res;
    res.m_sys_refcursor_id_item= el;
    return res;
  }


  static bool store_define_column_or_array(THD *thd,
                        const RContext_package_body & rcontext_package_body,
                        const char *func_name,
                        longlong dbms_sql_cursor_id,
                        longlong position,
                        const LEX_CSTRING &type_handler,
                        const Longlong_null &len,
                        const Longlong_null &lower_bound)
  {
    DBUG_ASSERT(len.is_null() || len.value() >= 0); // Checked in the caller
    CharBuffer<MAX_BIGINT_WIDTH> idx;
    idx.append_ulonglong((ulonglong) ((dbms_sql_cursor_id << 32) + position));

    const Type_handler_composite *thc= rcontext_package_body.m_assoc1->
                                         type_handler()->to_composite();
    Item_field *elem= thc->get_or_create_item(thd,
                                              rcontext_package_body.m_assoc1,
                                              idx.to_lex_cstring());
    if (!elem)
      return true;

    elem= thc->prepare_for_set(elem);
    DBUG_ASSERT(elem);
    if (elem->cols() != 3)
    {
      my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), func_name);
      return true;
    }
    elem->field->set_notnull();
    dynamic_cast<Item_field*>(elem->element_index(0))->field->set_notnull();
    dynamic_cast<Item_field*>(elem->element_index(0))->field->store(
                                type_handler, system_charset_info);
    dynamic_cast<Item_field*>(elem->element_index(1))->field->
                                       store_longlong_null(len, false);
    dynamic_cast<Item_field*>(elem->element_index(2))->field->
                                       store_longlong_null(lower_bound, false);
    return thc->finalize_for_set(elem);
  }

};


/*
  DBMS_SQL.BIND_VARIABLE (c     IN INTEGER,
                          name  IN VARCHAR2,
                          value IN <datatype>);
*/
class Item_func_dbms_sql_bind_variable: public Item_func_sql_set_param_by_name
{
public:
  using Item_func_sql_set_param_by_name::Item_func_sql_set_param_by_name;
  using Create= TFuncPluginCreate<Create_func_arg3,
                                  Item_func_dbms_sql_bind_variable,
                                  Create_func::TYPE_PACKAGE_PUBLIC_PROCEDURE>;

  bool val_bool() override
  {
    THD *thd= current_thd;
    // Evaluate DBMS_SQL cursor ID
    Longlong_null dbms_sql_cursor_id= args[0]->to_longlong_null();
    if ((null_value= dbms_sql_cursor_id.is_null() ||
                     dbms_sql_cursor_id.value() < 0))
      return true;
    // Make DBMS_SQL prepared statement name
    char buff[NAME_LEN + 1];
    Lex_ident_sys ps_name= {buff, 0};
    ps_name.length= my_snprintf(buff, sizeof(buff), "_dbms_sql_%lld",
                                dbms_sql_cursor_id.value());
    return set_param_by_name(thd, ps_name, args[1], args[2]);
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "dbms_sql_bind_variable"_LEX_CSTRING;
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_dbms_sql_bind_variable>(thd, this);
  }


};


/*
  DEFINE_ARRAY (c           IN INTEGER, 
                position    INTEGER,
                <variable>  <table_type>
                cnt         INTEGER, 
                lower_bound INTEGER);
*/
class Item_func_dbms_sql_define_array: public Item_bool_func,
                                       public Dbms_sql_common
{
public:
  using Item_bool_func::Item_bool_func;

  bool check_arguments() const
  {
    const LEX_CSTRING op= func_name_cstring();
    if (args[0]->check_type_can_return_int(op) ||
        args[1]->check_type_can_return_int(op) ||
        args[3]->check_type_can_return_int(op) ||
        args[4]->check_type_can_return_int(op))
      return true;
    if (!args[2]->type_handler()->to_composite())
    {
      my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
               args[2]->type_handler()->name().ptr(), op.str);
      return true;
    }
    return false;
  }

  bool val_bool() override
  {
    THD *thd= current_thd;
    RContext_package_body rcontext_package_body=
                            find_package_body_rcontext(thd);
    RContext_assoc0_row rcontext_assoc0_row= find_assoc0_row_by_key(thd,
                                                         rcontext_package_body,
                                                         args[0]);
    if ((null_value= rcontext_package_body.is_null() ||
                     rcontext_assoc0_row.is_null()))
    {
      // Unexpected variable structure
      my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), func_name());
      return false;
    }

    // TODO: check if the cursor is valid and exists
    const Longlong_null dbms_sql_cursor_id= args[0]->to_longlong_null();
    if ((null_value= dbms_sql_cursor_id.is_null() ||
                     dbms_sql_cursor_id.value() < 0))
    {
      // Unexpected dbms_cursor_id value
      my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), func_name());
      return false;
    }

    const Longlong_null position= args[1]->to_longlong_null();
    if ((null_value= position.is_null() || position.value() < 0))
    {
      // Unexpected position value
      my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), func_name());
      return false;
    }

    const Longlong_null len= args[3]->to_longlong_null();
    if ((null_value= !len.is_null() && len.value() < 0))
    {
      // Unexpected len value
      my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), func_name());
      return false;
    }

    const Longlong_null lower_bound= args[4]->to_longlong_null();
    if ((null_value= lower_bound.is_null()))
    {
      // Unexpected lower_bound value
      my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), func_name());
      return false;
    }

    null_value= store_define_column_or_array(thd,
                                             rcontext_package_body,
                                             func_name(),
                                             dbms_sql_cursor_id.value(),
                                             position.value(),
                                             args[2]->type_handler()->
                                               name().lex_cstring(),
                                             len,
                                             lower_bound);
    return false;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "dbms_sql_define_array"_LEX_CSTRING;
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_dbms_sql_define_array>(thd, this);
  }

  class Create: public Create_native_func
  {
  public:
    Item *create_native(THD *thd, const LEX_CSTRING *name,
                        List<Item> *item_list) override
    {
      if (!item_list || item_list->elements != 5)
      {
        my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
        return nullptr;
      }
      return new (thd->mem_root) Item_func_dbms_sql_define_array(thd,
                                                                 *item_list);
    }
    Create_func::Type type() const override
    {
      return Create_func::TYPE_PACKAGE_PUBLIC_PROCEDURE;
    }
    static Plugin_function *plugin_descriptor()
    {
      static Create creator;
      static Plugin_function descriptor(&creator);
      return &descriptor;
    }
  };

};


/*
  DEFINE_COLUMN(c INTEGER,
                position INTEGER,
                column <datatype>);

  DEFINE_COLUMN (c           INTEGER,
                 position    INTEGER,
                 column      VARCHAR2 CHARACTER SET ANY_CS,
                 column_size INTEGER);
*/
class Item_func_dbms_sql_define_column: public Item_bool_func,
                                        public Dbms_sql_common
{
public:
  using Item_bool_func::Item_bool_func;

  bool val_bool() override
  {
    THD *thd= current_thd;
    RContext_package_body rcontext_package_body= find_package_body_rcontext(thd);
    RContext_assoc0_row rcontext_assoc0_row= find_assoc0_row_by_key(thd,
                                                          rcontext_package_body,
                                                          args[0]);
    if ((null_value= rcontext_package_body.is_null() ||
                     rcontext_assoc0_row.is_null()))
    {
      // Unexpected variable structure
      my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), func_name());
      return false;
    }

    // TODO: check if the cursor is valid and exists
    const Longlong_null dbms_sql_cursor_id= args[0]->to_longlong_null();
    if ((null_value= dbms_sql_cursor_id.is_null() ||
                     dbms_sql_cursor_id.value() < 0))
    {
      // Unexpected dbms_cursor_id value
      my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), func_name());
      return false;
    }

    const Longlong_null position= args[1]->to_longlong_null();
    if ((null_value= position.is_null() || position.value() < 0))
    {
      // Unexpected position value
      my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), func_name());
      return false;
    }

    const Longlong_null len= arg_count < 4 ? Longlong_null() :
                                             args[3]->to_longlong_null();
    if ((null_value= !len.is_null() && len.value() < 0))
    {
      // Unexpected len value
      my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), func_name());
      return false;
    }

    null_value= store_define_column_or_array(thd,
                                             rcontext_package_body,
                                             func_name(),
                                             dbms_sql_cursor_id.value(),
                                             position.value(),
                                             args[2]->type_handler()->
                                               name().lex_cstring(),
                                             len,
                                             Longlong_null()/*lower_bound*/);
    return false;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "dbms_sql_define_column"_LEX_CSTRING;
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_dbms_sql_define_column>(thd, this);
  }

  class Create: public Create_native_func
  {
  public:
    Item *create_native(THD *thd, const LEX_CSTRING *name,
                        List<Item> *item_list) override
    {
      if (!item_list || item_list->elements < 3 || item_list->elements > 4)
      {
        my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
        return nullptr;
      }
      return new (thd->mem_root) Item_func_dbms_sql_define_column(thd,
                                                                  *item_list);
    }
    Create_func::Type type() const override
    {
      return Create_func::TYPE_PACKAGE_PUBLIC_PROCEDURE;
    }
    static Plugin_function *plugin_descriptor()
    {
      static Create creator;
      static Plugin_function descriptor(&creator);
      return &descriptor;
    }
  };

};


/*
   DBMS_SQL.COLUMN_VALUE(c                IN  INTEGER,
                         position         IN  INTEGER,
                         value            OUT <datatype>
                         [,column_error   OUT NUMBER]    -- Not yet
                         [,actual_length  OUT INTEGER]); -- Not yet
*/
class Item_func_dbms_sql_column_value: public Item_func_sql_column_value,
                                       public Dbms_sql_common
{
public:
  using Item_func_sql_column_value::Item_func_sql_column_value;
  using Create= TFuncPluginCreate<Create_func_arg3,
                                  Item_func_dbms_sql_column_value,
                                  Create_func::TYPE_PACKAGE_PUBLIC_PROCEDURE>;
  bool val_bool() override
  {
    THD *thd= current_thd;
    RContext_package_body rcontext_package_body=
                            find_package_body_rcontext(thd);
    RContext_assoc0_row rcontext_assoc0_row= find_assoc0_row_by_key(thd,
                                                          rcontext_package_body,
                                                          args[0]);
    /*
      Evaluate the DBMS_SQL cursor ID from args[0] and convert it into
      SYS_REFCURSOR cursor ID. The below code does effectively
      the same thing with this PL/SQL code:
        sys_refcursor_id:= assoc0(dbms_sql_cursor_id).cursor_id;
    */
    if ((null_value= rcontext_package_body.is_null() ||
                     rcontext_assoc0_row.is_null()))
    {
      // Unexpected variable structure
      my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), func_name());
      return false;
    }
    return column_value(thd, rcontext_assoc0_row.m_sys_refcursor_id_item,
                        args[1], args[2]);
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "dbms_sql_column_value"_LEX_CSTRING;
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_dbms_sql_column_value>(thd, this);
  }
};


/*************************************************************************/

maria_declare_plugin(pkg_dbms_sql)
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type
  Item_func_sql_set_param_by_name::Create::plugin_descriptor(),
  "sql_set_param_by_name",      // plugin name
  "MariaDB Corporation",        // plugin author
  "Function SQL_SET_PARAM_BY_NAME()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity
},
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type
  Item_func_sql_column_value::Create::plugin_descriptor(),
  "sql_column_value",           // plugin name
  "MariaDB Corporation",        // plugin author
  "Function SQL_COLUMN_VALUE()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity
},
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type
  Item_func_sql_refcursor_try_fetch::Create::plugin_descriptor(),
  "sql_refcursor_try_fetch",    // plugin name
  "MariaDB Corporation",        // plugin author
  "Function SQL_REFCURSOR_TRY_FETCH()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity
},
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type
  Item_func_dbms_sql_bind_variable::Create::plugin_descriptor(),
  "dbms_sql_bind_variable",     // plugin name
  "MariaDB Corporation",        // plugin author
  "Function DBMS_SQL_BIND_VARIABLE()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity
},
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type
  Item_func_dbms_sql_define_array::Create::plugin_descriptor(),
  "dbms_sql_define_array",     // plugin name
  "MariaDB Corporation",        // plugin author
  "Function DBMS_SQL_DEFINE_COLUMN()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity
},
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type
  Item_func_dbms_sql_define_column::Create::plugin_descriptor(),
  "dbms_sql_define_column",     // plugin name
  "MariaDB Corporation",        // plugin author
  "Function DBMS_SQL_DEFINE_COLUMN()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity
},
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type
  Item_func_dbms_sql_column_value::Create::plugin_descriptor(),
  "dbms_sql_column_value",      // plugin name
  "MariaDB Corporation",        // plugin author
  "Function DBMS_SQL_COLUMN_VALUE()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity
}

maria_declare_plugin_end;
