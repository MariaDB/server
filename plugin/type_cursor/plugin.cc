/*
   Copyright (c) 2023, MariaDB Corporation

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

#include <my_global.h>
#include <sql_class.h>          // THD
#include <mysql/plugin_data_type.h>
#include "sql_type.h"


class Type_collection_cursor: public Type_collection
{
protected:
  const Type_handler *aggregate_common(const Type_handler *h1,
                                       const Type_handler *h2) const;
public:
  const Type_handler *aggregate_for_result(const Type_handler *h1,
                                           const Type_handler *h2)
                                           const override
  {
    return aggregate_common(h1, h2);
  }
  const Type_handler *aggregate_for_comparison(const Type_handler *h1,
                                               const Type_handler *h2)
                                               const override
  {
    DBUG_ASSERT(h1 == h1->type_handler_for_comparison());
    DBUG_ASSERT(h2 == h2->type_handler_for_comparison());
    return aggregate_common(h1, h2);
  }
  const Type_handler *aggregate_for_min_max(const Type_handler *h1,
                                            const Type_handler *h2)
                                            const override
  {
    return aggregate_common(h1, h2);
  }
  const Type_handler *aggregate_for_num_op(const Type_handler *h1,
                                           const Type_handler *h2)
                                           const override
  {
    return aggregate_common(h1, h2);
  }
};


static Type_collection_cursor type_collection_cursor;


class Field_sys_refcursor :public Field_longlong
{
public:
  Field_sys_refcursor(const LEX_CSTRING &name, const Record_addr &addr,
                      enum utype unireg_check_arg, uint32 len_arg)
    :Field_longlong(addr.ptr(), len_arg, addr.null_ptr(), addr.null_bit(),
                    Field::NONE, &name, false/*zerofill*/, true/*unsigned*/)
  {}
  void sql_type(String &res) const
  {
    res.set_ascii(STRING_WITH_LEN("sys_refcursor"));
  }
  const Type_handler *type_handler() const override;
};


class Type_handler_sys_refcursor: public Type_handler_ulonglong
{
public:
  const Type_collection *type_collection() const override
  {
    return &type_collection_cursor;
  }
  const Type_handler *type_handler_for_comparison() const override
  {
    return this;
  }
  const Type_handler *type_handler_signed() const override
  {
    return this;
  }
  /*
    Create a Field as a storage for an SP variable.
    Note, creating a field for a real table is prevented in the methods below:
    - make_table_field()
    - Column_definition_set_attributes()
  */
  Field *make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &rec, const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override
  {
    return new (root) Field_sys_refcursor(*name, rec, attr->unireg_check,
                                          (uint32) attr->length);
  }

  // Disallow "CREATE TABLE t1 AS SELECT sys_refcursor_var;"
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
             "SYS_REFCURSOR", "CREATE TABLE");
    return nullptr;
  }

  // Disallow "CREATE TABLE t1 (a SYS_REFCURSOR)"
  bool Column_definition_set_attributes(THD *thd,
                                        Column_definition *def,
                                        const Lex_field_type_st &attr,
                                        column_definition_type_t type)
                                        const override
  {
    if (type == COLUMN_DEFINITION_TABLE_FIELD)
    {
      my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
               "SYS_REFCURSOR", "CREATE TABLE");
      return true;
    }
    return Type_handler_longlong::Column_definition_set_attributes(thd, def,
                                                                   attr, type);
  }
};


static Type_handler_sys_refcursor type_handler_sys_refcursor;


const Type_handler *Field_sys_refcursor::type_handler() const
{
  return &type_handler_sys_refcursor;
}


const Type_handler *
Type_collection_cursor::aggregate_common(const Type_handler *h1,
                                         const Type_handler *h2) const
{
  if (h1 == h2)
   return h1;

  static const Type_aggregator::Pair agg[]=
  {
    {
      &type_handler_sys_refcursor,
      &type_handler_null,
      &type_handler_sys_refcursor
    },
    {NULL,NULL,NULL}
  };
  return Type_aggregator::find_handler_in_array(agg, h1, h2, true);
}


static struct st_mariadb_data_type plugin_descriptor_type_sys_refcursor=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  &type_handler_sys_refcursor
};



/*************************************************************************/


/*************************************************************************/

maria_declare_plugin(type_cursor)
{
  MariaDB_DATA_TYPE_PLUGIN,     // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_type_sys_refcursor, // a pointer to the plugin descriptor
  "sys_refcursor",              // plugin name
  "MariaDB Corporation",        // plugin author
  "Data type SYS_REFCURSOR",    // the plugin description
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
