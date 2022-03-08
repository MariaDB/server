/*
   Copyright (c) 2020 MariaDB Foundation

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

#include <my_global.h>
#include <mysql/plugin_data_type.h>
#include <sql_type.h>
#include <field.h>
#include <mysqld_error.h>
#include "mysql_json.h"

const LEX_CSTRING empty_clex_str= {"", 0};

class Type_handler_mysql_json: public Type_handler_blob
{
public:
  Field *make_conversion_table_field(MEM_ROOT *, TABLE *, uint, const Field *)
    const override;
  const Type_collection *type_collection() const override;
  Field *make_table_field_from_def(TABLE_SHARE *, MEM_ROOT *,
                                   const LEX_CSTRING *, const Record_addr &,
                                   const Bit_addr &,
                                   const Column_definition_attributes *,
                                   uint32) const override;
  Field *make_table_field(MEM_ROOT *, const LEX_CSTRING *,
                          const Record_addr &, const Type_all_attributes &,
                          TABLE_SHARE *) const override;
  void Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *def,
                                              const Field *field) const override;
};

Type_handler_mysql_json type_handler_mysql_json;


class Field_mysql_json: public Field_blob
{
public:
  Field_mysql_json(uchar *ptr_arg, uchar *null_ptr_arg,
                   uchar null_bit_arg, enum utype unireg_check_arg,
                   const LEX_CSTRING *field_name_arg, TABLE_SHARE *share,
                   uint blob_pack_length, const DTCollation &collation)
    : Field_blob(ptr_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
                 field_name_arg, share, blob_pack_length,
                 &my_charset_utf8mb4_bin)
  {}

  String *val_str(String *val_buffer, String *val_str);
  const Type_handler *type_handler() const { return &type_handler_mysql_json; }
  bool parse_mysql(String *dest, const char *data, size_t length) const;
  bool send(Protocol *protocol) { return Field::send(protocol); }
  void sql_type(String &s) const
  { s.set_ascii(STRING_WITH_LEN("json /* MySQL 5.7 */")); }
  /* this will make ALTER TABLE to consider it different from built-in field */
  Compression_method *compression_method() const { return (Compression_method*)1; }
};

Field *Type_handler_mysql_json::make_conversion_table_field(MEM_ROOT *root,
                    TABLE *table, uint metadata, const Field *target) const
{
  uint pack_length= metadata & 0x00ff;
  if (pack_length < 1 || pack_length > 4)
    return NULL; // Broken binary log?
  return new (root)
         Field_mysql_json(NULL, (uchar *) "", 1, Field::NONE, &empty_clex_str,
                          table->s, pack_length, target->charset());
}

Field *Type_handler_mysql_json::make_table_field_from_def(TABLE_SHARE *share,
                 MEM_ROOT *root, const LEX_CSTRING *name,
                 const Record_addr &addr, const Bit_addr &bit,
                 const Column_definition_attributes *attr, uint32 flags) const
{
  return new (root) Field_mysql_json(addr.ptr(), addr.null_ptr(),
                 addr.null_bit(), attr->unireg_check, name, share,
                 attr->pack_flag_to_pack_length(), attr->charset);
}

void Type_handler_mysql_json::
       Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *def,
                                              const Field *field) const
{
  Type_handler_blob::Column_definition_reuse_fix_attributes(thd, def, field);
  def->decimals= 0;
}



Field *Type_handler_mysql_json::make_table_field(MEM_ROOT *root,
                 const LEX_CSTRING *name, const Record_addr &addr,
                 const Type_all_attributes &attr, TABLE_SHARE *share) const
{
  return new (root) Field_mysql_json(addr.ptr(), addr.null_ptr(),
                 addr.null_bit(), Field::NONE, name, share, 2, attr.collation);
}


String *Field_mysql_json::val_str(String *val_buffer, String *val_ptr)
{
  String *raw_value= Field_blob::val_str(val_buffer, val_ptr);
  String data;

  data.copy(*raw_value);

  val_ptr->length(0);
  if (parse_mysql(val_ptr, data.ptr(), data.length()))
  {
    val_ptr->length(0);
    my_printf_error(ER_UNKNOWN_ERROR,
        "Error parsing MySQL JSON format, please dump this table from MySQL "
        "and then restore it to be able to use it in MariaDB.", MYF(0));
  }
  return val_ptr;
}

bool Field_mysql_json::parse_mysql(String *dest,
                                   const char *data, size_t length) const
{
  if (!data)
    return false;

  /* Each JSON blob must start with a type specifier. */
  if (length < 2)
    return true;

  if (parse_mysql_json_value(dest, static_cast<JSONB_TYPES>(data[0]),
                             reinterpret_cast<const uchar*>(data) + 1,
                             length - 1, 0))
    return true;

  return false;
}

class Type_collection_mysql_json: public Type_collection
{
public:
  const Type_handler *aggregate_for_result(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    if (a == b)
      return a;
    return NULL;
  }

  const Type_handler *aggregate_for_min_max(const Type_handler *a,
                                            const Type_handler *b)
                                            const override
  {
    return aggregate_for_result(a, b);
  }

  const Type_handler *aggregate_for_comparison(const Type_handler *a,
                                               const Type_handler *b)
                                               const override
  {
    return aggregate_for_result(a, b);
  }

  const Type_handler *aggregate_for_num_op(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    return NULL;
  }

  const Type_handler *handler_by_name(const LEX_CSTRING &name) const override
  {
    if (type_handler_mysql_json.name().eq(name))
      return &type_handler_mysql_json;
    return NULL;
  }
};

const Type_collection *Type_handler_mysql_json::type_collection() const
{
  static Type_collection_mysql_json type_collection_mysql_json;
  return &type_collection_mysql_json;
}

static struct st_mariadb_data_type plugin_descriptor_type_mysql_json=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  &type_handler_mysql_json
};

maria_declare_plugin(type_mysql_json)
{
  MariaDB_DATA_TYPE_PLUGIN,
  &plugin_descriptor_type_mysql_json,
  "MYSQL_JSON",
  "Anel Husaković, Vicențiu Ciorbaru",
  "Data type MYSQL_JSON",
  PLUGIN_LICENSE_GPL,
  0,
  0,
  0x0001,
  NULL,
  NULL,
  "0.1",
  MariaDB_PLUGIN_MATURITY_GAMMA
}
maria_declare_plugin_end;
