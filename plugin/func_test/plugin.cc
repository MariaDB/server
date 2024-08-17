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

class Item_func_sysconst_test :public Item_func_sysconst
{
public:
  Item_func_sysconst_test(THD *thd): Item_func_sysconst(thd) {}
  String *val_str(String *str) override
  {
    null_value= str->copy(STRING_WITH_LEN("sysconst_test"), system_charset_info);
    return null_value ? NULL : str;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= MAX_FIELD_NAME * system_charset_info->mbmaxlen;
    set_maybe_null();
    return false;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("sysconst_test") };
    return name;
  }
  const Lex_ident_routine fully_qualified_func_name() const override
  { return Lex_ident_routine("sysconst_test()"_LEX_CSTRING); }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_sysconst_test>(thd, this); }
};


class Create_func_sysconst_test : public Create_func_arg0
{
public:
  Item *create_builder(THD *thd) override;
  static Create_func_sysconst_test s_singleton;
protected:
  Create_func_sysconst_test() {}
};


Create_func_sysconst_test Create_func_sysconst_test::s_singleton;

Item* Create_func_sysconst_test::create_builder(THD *thd)
{
  return new (thd->mem_root) Item_func_sysconst_test(thd);
}


#define BUILDER(F) & F::s_singleton


static Plugin_function
  plugin_descriptor_function_sysconst_test(BUILDER(Create_func_sysconst_test));


class Strnxfrm_args: public Null_flag
{
  StringBuffer<128> m_srcbuf;
public:
  String *m_src;
  longlong m_dstlen;
  longlong m_nweights;
  longlong m_flags;
  Strnxfrm_args(Item **args)
   :Null_flag(true)
  {
    if (!(m_src= args[0]->val_str(&m_srcbuf)))
      return;
    m_dstlen= args[1]->val_int();
    if (args[1]->null_value || m_dstlen < 0)
      return;
    m_nweights= args[2]->val_int();
    if (args[2]->null_value || m_nweights < 0)
      return;
    m_flags= args[3]->val_int();
    if (args[3]->null_value || m_flags < 0)
      return;
    m_is_null= false;
  }
  my_strnxfrm_ret_t exec(CHARSET_INFO *cs, String *to)
  {
    DBUG_ASSERT(!is_null());
    if ((m_is_null= to->alloc(m_dstlen)))
      return {0,0,0};

    my_strnxfrm_ret_t rc= cs->strnxfrm((char*) to->ptr(),
                                       (size_t)m_dstlen,
                                       (uint) m_nweights,
                                       m_src->ptr(), m_src->length(),
                                       (uint) m_flags);

    to->length((uint32) rc.m_result_length);
    return rc;
  }
};


class Item_func_strnxfrm_source_length_used: public Item_longlong_func
{
  using Self = Item_func_strnxfrm_source_length_used;
public:
  using Item_longlong_func::Item_longlong_func;
  longlong val_int() override
  {
    Strnxfrm_args param(args);
    if ((null_value= param.is_null()))
      return 0;

    StringBuffer<128> dstbuf;
    my_strnxfrm_ret_t rc= param.exec(args[0]->collation.collation, &dstbuf);
    if ((null_value= param.is_null()))
      return 0;
    return (longlong) rc.m_source_length_used;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "strnxfrm_source_length_used"_LEX_CSTRING;
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  {
    return get_item_copy<Self>(thd, this);
  }

  class Create_func : public Create_native_func
  {
  public:
    using Create_native_func::Create_native_func;
    Item *create_native(THD *thd, const LEX_CSTRING *name,
                        List<Item> *item_list) override
    {
      uint arg_count= item_list ? item_list->elements : 0;
      if (arg_count != 4)
      {
        my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
        return nullptr;
      }
      return new (thd->mem_root) Self(thd, *item_list);
    }
  };

  static Plugin_function *plugin_descriptor()
  {
    static Create_func creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};


class Item_func_strnxfrm_warnings: public Item_long_func
{
  using Self = Item_func_strnxfrm_warnings;
public:
  using Item_long_func::Item_long_func;
  longlong val_int() override
  {
    Strnxfrm_args param(args);
    if ((null_value= param.is_null()))
      return 0;

    StringBuffer<128> dstbuf;
    my_strnxfrm_ret_t rc= param.exec(args[0]->collation.collation, &dstbuf);
    if ((null_value= param.is_null()))
      return 0;
    return (longlong) rc.m_warnings;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "strnxfrm_warnings"_LEX_CSTRING;
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  {
    return get_item_copy<Self>(thd, this);
  }

  class Create_func : public Create_native_func
  {
  public:
    using Create_native_func::Create_native_func;
    Item *create_native(THD *thd, const LEX_CSTRING *name,
                        List<Item> *item_list) override
    {
      uint arg_count= item_list ? item_list->elements : 0;
      if (arg_count != 4)
      {
        my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
        return nullptr;
      }
      return new (thd->mem_root) Self(thd, *item_list);
    }
  };

  static Plugin_function *plugin_descriptor()
  {
    static Create_func creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};


class Item_func_strnxfrm: public Item_str_func
{
  using Self = Item_func_strnxfrm;
public:
  using Item_str_func::Item_str_func;
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= MAX_BLOB_WIDTH;
    return false;
  }
  String *val_str(String *to) override
  {
    Strnxfrm_args param(args);
    if ((null_value= param.is_null()))
      return nullptr;
    param.exec(args[0]->collation.collation, to);
    if ((null_value= param.is_null()))
      return nullptr;
    return to;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "strnxfrm"_LEX_CSTRING;
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  {
    return get_item_copy<Self>(thd, this);
  }

  class Create_func : public Create_native_func
  {
  public:
    using Create_native_func::Create_native_func;
    Item *create_native(THD *thd, const LEX_CSTRING *name,
                        List<Item> *item_list) override
    {
      uint arg_count= item_list ? item_list->elements : 0;
      if (arg_count != 4)
      {
        my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
        return nullptr;
      }
      return new (thd->mem_root) Self(thd, *item_list);
    }
  };

  static Plugin_function *plugin_descriptor()
  {
    static Create_func creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

/*************************************************************************/

maria_declare_plugin(type_test)
{
  MariaDB_FUNCTION_PLUGIN,       // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_function_sysconst_test, // pointer to type-specific plugin descriptor
  "sysconst_test",              // plugin name
  "MariaDB Corporation",        // plugin author
  "Function SYSCONST_TEST()",   // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL // Maturity(see include/mysql/plugin.h)*/
},
{
  MariaDB_FUNCTION_PLUGIN,       // the plugin type
  Item_func_strnxfrm::plugin_descriptor(),
  "strnxfrm",                   // plugin name
  "MariaDB Corporation",        // plugin author
  "Function STRNXFRM()",        // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL // Maturity
},
{
  MariaDB_FUNCTION_PLUGIN,       // the plugin type
  Item_func_strnxfrm_source_length_used::plugin_descriptor(),
  "strnxfrm_source_length_used",// plugin name
  "MariaDB Corporation",        // plugin author
  "Function STRNXFRM_SOURCE_LENGTH_USED()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL // Maturity
},
{
  MariaDB_FUNCTION_PLUGIN,       // the plugin type
  Item_func_strnxfrm_warnings::plugin_descriptor(),
  "strnxfrm_warnings",// plugin name
  "MariaDB Corporation",        // plugin author
  "Function STRNXFRM_WARNINGS()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL // Maturity
}
maria_declare_plugin_end;
