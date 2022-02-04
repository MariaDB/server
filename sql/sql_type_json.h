#ifndef SQL_TYPE_JSON_INCLUDED
#define SQL_TYPE_JSON_INCLUDED
/*
   Copyright (c) 2019, 2021 MariaDB

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


class Type_handler_json_common
{
public:
  static Virtual_column_info *make_json_valid_expr(THD *thd,
                                            const LEX_CSTRING *field_name);
  static bool make_json_valid_expr_if_needed(THD *thd, Column_definition *c);
  static bool set_format_name(Send_field_extended_metadata *to)
  {
    static const Lex_cstring fmt(STRING_WITH_LEN("json"));
    return to->set_format_name(fmt);
  }
  static const Type_handler *json_type_handler(uint max_octet_length);
  static const Type_handler *json_blob_type_handler_by_length_bytes(uint len);
  static const Type_handler *json_type_handler_sum(const Item_sum *sum);
  static const Type_handler *json_type_handler_from_generic(const Type_handler *th);
  static bool has_json_valid_constraint(const Field *field);
  static const Type_collection *type_collection();
  static bool is_json_type_handler(const Type_handler *handler)
  {
    return handler->type_collection() == type_collection();
  }
};


template <class BASE, const Named_type_handler<BASE> &thbase>
class Type_handler_general_purpose_string_to_json:
                                            public BASE,
                                            public Type_handler_json_common
{
public:
  const Type_handler *type_handler_base() const override
  {
    return &thbase;
  }
  const Type_collection *type_collection() const override
  {
    return Type_handler_json_common::type_collection();
  }
  bool Column_definition_validate_check_constraint(THD *thd,
                                                   Column_definition *c)
                                                   const override
  {
    return make_json_valid_expr_if_needed(thd, c) ||
           BASE::Column_definition_validate_check_constraint(thd, c);
  }
  bool Column_definition_data_type_info_image(Binary_string *to,
                                              const Column_definition &def)
                                              const override
  {
    /*
      Override the inherited method to avoid JSON type handlers writing any
      extended metadata to FRM. JSON type handlers are currently detected
      only by CHECK(JSON_VALID()) constraint. This may change in the future
      to do write extended metadata to FRM, for more reliable detection.
    */
    return false;
  }

  bool Item_append_extended_type_info(Send_field_extended_metadata *to,
                                      const Item *item) const override
  {
    return set_format_name(to); // Send "format=json" in the protocol
  }

  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *hybrid,
                                       Type_all_attributes *attr,
                                       Item **items, uint nitems)
                                       const override
  {
    if (BASE::Item_hybrid_func_fix_attributes(thd, name, hybrid, attr,
                                              items, nitems))
      return true;
    /*
      The above call can change the type handler on "hybrid", e.g.
      choose a proper BLOB type handler according to the calculated max_length.
      Convert general purpose string type handler to its JSON counterpart.
      This makes hybrid functions preserve JSON data types, e.g.:
        COALESCE(json_expr1, json_expr2) -> JSON
    */
    hybrid->set_handler(json_type_handler_from_generic(hybrid->type_handler()));
    return false;
  }
};


class Type_handler_string_json:
  public Type_handler_general_purpose_string_to_json<Type_handler_string,
                                                     type_handler_string>
{ };


class Type_handler_varchar_json:
  public Type_handler_general_purpose_string_to_json<Type_handler_varchar,
                                                     type_handler_varchar>
{ };

class Type_handler_tiny_blob_json:
  public Type_handler_general_purpose_string_to_json<Type_handler_tiny_blob,
                                                     type_handler_tiny_blob>
{ };

class Type_handler_blob_json:
  public Type_handler_general_purpose_string_to_json<Type_handler_blob,
                                                     type_handler_blob>
{ };


class Type_handler_medium_blob_json:
  public Type_handler_general_purpose_string_to_json<Type_handler_medium_blob,
                                                     type_handler_medium_blob>
{ };

class Type_handler_long_blob_json:
  public Type_handler_general_purpose_string_to_json<Type_handler_long_blob,
                                                     type_handler_long_blob>
{ };



extern MYSQL_PLUGIN_IMPORT
  Named_type_handler<Type_handler_string_json> type_handler_string_json;

extern MYSQL_PLUGIN_IMPORT
  Named_type_handler<Type_handler_varchar_json> type_handler_varchar_json;

extern MYSQL_PLUGIN_IMPORT
  Named_type_handler<Type_handler_tiny_blob_json> type_handler_tiny_blob_json;

extern MYSQL_PLUGIN_IMPORT
  Named_type_handler<Type_handler_blob_json> type_handler_blob_json;

extern MYSQL_PLUGIN_IMPORT
  Named_type_handler<Type_handler_medium_blob_json> type_handler_medium_blob_json;

extern MYSQL_PLUGIN_IMPORT
  Named_type_handler<Type_handler_long_blob_json> type_handler_long_blob_json;


#endif // SQL_TYPE_JSON_INCLUDED
