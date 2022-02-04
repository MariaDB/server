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

#include "sql_type_json.h"
#include "sql_class.h"


Named_type_handler<Type_handler_string_json>
  type_handler_string_json("char/json");

Named_type_handler<Type_handler_varchar_json>
  type_handler_varchar_json("varchar/json");

Named_type_handler<Type_handler_tiny_blob_json>
  type_handler_tiny_blob_json("tinyblob/json");

Named_type_handler<Type_handler_blob_json>
  type_handler_blob_json("blob/json");

Named_type_handler<Type_handler_medium_blob_json>
  type_handler_medium_blob_json("mediumblob/json");

Named_type_handler<Type_handler_long_blob_json>
  type_handler_long_blob_json("longblob/json");


// Convert general purpose string type handlers to their JSON counterparts
const Type_handler *
Type_handler_json_common::json_type_handler_from_generic(const Type_handler *th)
{
  // Test in the order of likelyhood.
  if (th == &type_handler_long_blob)
    return &type_handler_long_blob_json;
  if (th == &type_handler_varchar)
    return &type_handler_varchar_json;
  if (th == &type_handler_blob)
    return &type_handler_blob_json;
  if (th == &type_handler_tiny_blob)
    return &type_handler_tiny_blob_json;
  if (th == &type_handler_medium_blob)
    return &type_handler_medium_blob_json;
  if (th == &type_handler_string)
    return &type_handler_string_json;
  DBUG_ASSERT(is_json_type_handler(th));
  return th;
}


/*
  This method resembles what Type_handler::string_type_handler()
  does for general purpose string type handlers.
*/
const Type_handler *
Type_handler_json_common::json_type_handler(uint max_octet_length)
{
  if (max_octet_length >= 16777216)
    return &type_handler_long_blob_json;
  else if (max_octet_length >= 65536)
    return &type_handler_medium_blob_json;
  else if (max_octet_length >= MAX_FIELD_VARCHARLENGTH)
    return &type_handler_blob_json;
  return &type_handler_varchar_json;
}


/*
  This method resembles what Field_blob::type_handler()
  does for general purpose BLOB type handlers.
*/
const Type_handler *
Type_handler_json_common::json_blob_type_handler_by_length_bytes(uint len)
{
  switch (len) {
  case 1: return &type_handler_tiny_blob_json;
  case 2: return &type_handler_blob_json;
  case 3: return &type_handler_medium_blob_json;
  }
  return &type_handler_long_blob_json;
}


/*
  This method resembles what Item_sum_group_concat::type_handler()
  does for general purpose string type handlers.
*/
const Type_handler *
Type_handler_json_common::json_type_handler_sum(const Item_sum *item)
{
  if (item->too_big_for_varchar())
    return &type_handler_blob_json;
  return &type_handler_varchar_json;
}


bool Type_handler_json_common::has_json_valid_constraint(const Field *field)
{
  return field->check_constraint &&
         field->check_constraint->expr &&
         field->check_constraint->expr->type() == Item::FUNC_ITEM &&
         static_cast<const Item_func *>(field->check_constraint->expr)->
           functype() == Item_func::JSON_VALID_FUNC;
}


/**
   Create JSON_VALID(field_name) expression
*/


Virtual_column_info *
Type_handler_json_common::make_json_valid_expr(THD *thd,
                                               const LEX_CSTRING *field_name)
{
  Lex_ident_sys_st str;
  Item *field, *expr;
  str.set_valid_utf8(field_name);
  if (unlikely(!(field= thd->lex->create_item_ident_field(thd,
                                                          Lex_ident_sys(),
                                                          Lex_ident_sys(),
                                                          str))))
    return 0;
  if (unlikely(!(expr= new (thd->mem_root) Item_func_json_valid(thd, field))))
    return 0;
  return add_virtual_expression(thd, expr);
}


bool Type_handler_json_common::make_json_valid_expr_if_needed(THD *thd,
                                                 Column_definition *c)
{
  return !c->check_constraint &&
         !(c->check_constraint= make_json_valid_expr(thd, &c->field_name));
}


class Type_collection_json: public Type_collection
{
  const Type_handler *aggregate_common(const Type_handler *a,
                                       const Type_handler *b) const
  {
    if (a == b)
      return a;
    if (a == &type_handler_null)
      return b;
    if (b == &type_handler_null)
      return a;
    return NULL;
  }

  /*
    Aggregate two JSON type handlers for result.
    If one of the handlers is not JSON, NULL is returned.
  */
  const Type_handler *aggregate_json_for_result(const Type_handler *a,
                                                const Type_handler *b) const
  {
    if (!Type_handler_json_common::is_json_type_handler(a) ||
        !Type_handler_json_common::is_json_type_handler(b))
      return NULL;
    // Here we have two JSON data types. Let's aggregate their base types.
    const Type_handler *a0= a->type_handler_base();
    const Type_handler *b0= b->type_handler_base();
    // Base types are expected to belong to type_collection_std:
    DBUG_ASSERT(a0->type_collection() == type_handler_null.type_collection());
    DBUG_ASSERT(b0->type_collection() == type_handler_null.type_collection());
    const Type_handler *c= a0->type_collection()->aggregate_for_result(a0, b0);
    return Type_handler_json_common::json_type_handler_from_generic(c);
  }
public:
  const Type_handler *aggregate_for_result(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    const Type_handler *h;
    if ((h= aggregate_common(a, b)) ||
        (h= aggregate_json_for_result(a, b)))
      return h;
    /*
      One of the types is not JSON.
      Let the caller aggregate according to the derived rules:
        COALESCE(VARCHAR/JSON, TEXT) -> COALESCE(VARCHAR, TEXT)
    */
    return NULL;
  }

  const Type_handler *aggregate_for_min_max(const Type_handler *a,
                                            const Type_handler *b)
                                            const override
  {
    /*
      No JSON specific rules.
      Let the caller aggregate according to the derived rules:
        LEAST(VARCHAR/JSON, TEXT/JSON) -> LEAST(VARCHAR, TEXT)
    */
    return NULL;
  }

  const Type_handler *aggregate_for_comparison(const Type_handler *a,
                                               const Type_handler *b)
                                               const override
  {
    /*
      All JSON types return &type_handler_long_blob
      in type_handler_for_comparison(). We should not get here.
    */
    DBUG_ASSERT(0);
    return NULL;
  }

  const Type_handler *aggregate_for_num_op(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    /*
      No JSON specific rules.
      Let the caller aggregate according to the derived rules:
        (VARCHAR/JSON + TEXT/JSON) -> (VARCHAR + TEXT)
    */
    return NULL;
  }

  const Type_handler *handler_by_name(const LEX_CSTRING &name) const override
  {
    /*
      Name resolution is not needed yet.
      JSON is not fully pluggable at the moment:
      - It is parsed using a hard-coded rule in sql_yacc.yy
      - It does not store extended data type information into
        FRM file yet. JSON is detected by CHECK(JSON_VALID(col))
        and this detection is also hard-coded.
      This will change in the future.
    */
    return NULL;
  }
};


const Type_collection *Type_handler_json_common::type_collection()
{
  static Type_collection_json type_collection_json;
  return &type_collection_json;
}
