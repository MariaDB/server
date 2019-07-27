#ifndef SQL_TYPE_JSON_BINARY_INCLUDED
#define SQL_TYPE_JSON_BINARY_INCLUDED
/*
   Copyright (c) 2019, Yubao Liu

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "mariadb.h"
#include "sql_type.h"
#include "field.h"

class Field_json_binary: public Field_blob {
public:
  Field_json_binary(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
                    enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
                    TABLE_SHARE *share, uint blob_pack_length,
                    const DTCollation &collation)
    : Field_blob(ptr_arg, null_ptr_arg, null_bit_arg, unireg_check_arg, field_name_arg,
                 share, blob_pack_length, collation) {}

  String *val_str(String*,String *);
};

class Type_handler_json_binary: public Type_handler_blob_common
{
  static const Name m_name_json_binary;
public:
  virtual ~Type_handler_json_binary() {}
  const Name name() const { return m_name_json_binary; }
  enum_field_types field_type() const { return MYSQL_TYPE_JSON; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const;
  const Type_handler *type_handler_for_tmp_table(const Item *item) const
  {
    return this;
  }
  const Type_handler *type_handler_for_union(const Item *item) const
  {
    return this;
  }
  const Type_handler *type_handler_for_comparison() const
  {
    return this;
  }
  const Type_handler *type_handler_adjusted_to_max_octet_length(uint max_octet_length,
                                                                CHARSET_INFO *cs) const
  {
    return this;
  }
  uint32 calc_pack_length(uint32 length) const
  {
    return 4 + portable_sizeof_char_ptr;
  }
  uint max_octet_length() const
  {
    return UINT_MAX32;
  }
};

extern MYSQL_PLUGIN_IMPORT
  Type_handler_json_binary type_handler_json_binary;

#endif // SQL_TYPE_JSON_BINARY_INCLUDED
