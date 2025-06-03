/* Copyright (c) 2019,2021, MariaDB Corporation

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

#define MYSQL_SERVER
#include "mariadb.h"
#include "sql_class.h"
#include "sql_type_uuid.h"
#include "item_uuidfunc.h"
#include <mysql/plugin_data_type.h>
#include <mysql/plugin_function.h>

/*
  The whole purpose of this Type_handler_uuid_dispatcher is to choose
  whether the field should use Type_handler_uuid_new or Type_handler_uuid_old
  based on the version of MariaDB that created the table.
  When created every field will use either Type_handler_uuid_new or _old.
  Literals and functions always use _new.
*/
class Type_handler_uuid_dispatcher: public Type_handler_uuid_new
{
public:
  Field *make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *root,
                            const LEX_CSTRING *name, const Record_addr &addr,
                            const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const override
  {
    bool new_uuid= share->mysql_version == 0 ||
         (share->mysql_version >= 100908 && share->mysql_version < 100999) ||
         (share->mysql_version >= 101006 && share->mysql_version < 101099) ||
         (share->mysql_version >= 101105 && share->mysql_version < 101199) ||
         (share->mysql_version >= 110003 && share->mysql_version < 110099) ||
         (share->mysql_version >= 110102 && share->mysql_version < 110199) ||
          share->mysql_version >= 110201;
    static Type_handler *th[]= {
      Type_handler_uuid_old::singleton(), Type_handler_uuid_new::singleton()
    };
    return th[new_uuid]->
        make_table_field_from_def(share, root, name, addr, bit, attr, flags);
  }
};

static Type_handler_uuid_dispatcher type_handler_uuid_dispatcher;

static struct st_mariadb_data_type plugin_descriptor_type_uuid=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  &type_handler_uuid_dispatcher
};


const Type_handler *Type_collection_uuid::find_in_array(const Type_handler *a,
                                                        const Type_handler *b,
                                                        bool for_cmp) const
{
  if (a == b) return a;

  /*
    in the search below we'll find if we can convert `b` to `a`.
    So, if one of the arguments is uuid and the other is not,
    we should put uuid type in `a` and not-uuid in `b`. And if one type is
    new uuid and the other is old uuid, new uuid should be in `a`
  */
  if (a != Type_handler_uuid_new::singleton() && b->type_collection() == this)
    std::swap(a, b);

  DBUG_ASSERT(a != &type_handler_uuid_dispatcher);
  DBUG_ASSERT(b != &type_handler_uuid_dispatcher);

  /*
    Search in the array for an element, equal to `b`.
    If found - return `a`, if not found - return NULL.
    Array is terminated by `a`.
  */
  static const Type_handler *arr[]={ &type_handler_varchar,
    &type_handler_string, &type_handler_tiny_blob, &type_handler_blob,
    &type_handler_medium_blob, &type_handler_hex_hybrid,
    // in aggregate_for_comparison() all types above cannot happen,
    // so we'll start the search from here:
    &type_handler_null, &type_handler_long_blob,
    Type_handler_uuid_old::singleton(), Type_handler_uuid_new::singleton() };

  for (int i= for_cmp ? 6 : 0; arr[i] != a; i++)
    if (arr[i] == b)
      return a;
  return NULL;
}


const Type_handler *Type_collection_uuid::type_handler_for_implicit_upgrade(
                                              const Type_handler *from) const
{
  return Type_handler_uuid_new::singleton();
}


/*************************************************************************/

class Create_func_uuid : public Create_func_arg0
{
public:
  Item *create_builder(THD *thd) override
  {
    DBUG_ENTER("Create_func_uuid::create");
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    thd->lex->uncacheable(UNCACHEABLE_RAND);
    DBUG_RETURN(new (thd->mem_root) Item_func_uuid(thd));
  }
  static Create_func_uuid s_singleton;

protected:
  Create_func_uuid() {}
  virtual ~Create_func_uuid() {}
};


class Create_func_sys_guid : public Create_func_arg0
{
public:
  Item *create_builder(THD *thd) override
  {
    DBUG_ENTER("Create_func_sys_guid::create");
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    thd->lex->uncacheable(UNCACHEABLE_RAND);
    DBUG_RETURN(new (thd->mem_root) Item_func_sys_guid(thd));
  }
  static Create_func_sys_guid s_singleton;

protected:
  Create_func_sys_guid() {}
  virtual ~Create_func_sys_guid() {}
};

Create_func_uuid Create_func_uuid::s_singleton;
Create_func_sys_guid Create_func_sys_guid::s_singleton;

static Plugin_function
  plugin_descriptor_function_uuid(&Create_func_uuid::s_singleton),
  plugin_descriptor_function_sys_guid(&Create_func_sys_guid::s_singleton);

static constexpr Name type_name={STRING_WITH_LEN("uuid")};

int uuid_init(void*)
{
  Type_handler_uuid_new::singleton()->set_name(type_name);
  Type_handler_uuid_old::singleton()->set_name(type_name);
  return 0;
}

/*************************************************************************/

maria_declare_plugin(type_uuid)
{
  MariaDB_DATA_TYPE_PLUGIN,     // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_type_uuid, // pointer to type-specific plugin descriptor
  type_name.ptr(),              // plugin name
  "MariaDB Corporation",        // plugin author
  "Data type UUID",             // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  uuid_init,                    // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE// Maturity(see include/mysql/plugin.h)*/
},
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_function_uuid,// pointer to type-specific plugin descriptor
  "uuid",                       // plugin name
  "MariaDB Corporation",        // plugin author
  "Function UUID()",            // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE// Maturity(see include/mysql/plugin.h)*/
},
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_function_sys_guid,// pointer to type-specific plugin descriptor
  "sys_guid",                   // plugin name
  "MariaDB Corporation",        // plugin author
  "Function SYS_GUID()",        // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE// Maturity(see include/mysql/plugin.h)*/
}
maria_declare_plugin_end;
