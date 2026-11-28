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


/**
   The column a JSON_VALID() call is about, where the expression is that
   call and nothing else.

   NULL for anything else at all: another function, the call with more
   than the one argument it takes, or an argument that is not a column.
   Both questions below are that shape plus a way of naming the column,
   so the shape is asked once.

   RESOLVED says whether the expression has been fixed.  It has, wherever
   there is a Field to compare against, and the argument is then reached
   through Item::real_item() so that a reference standing in for the
   column is followed to it; it has not while a table is being defined,
   where following would be reaching through a pointer nothing has set.
*/

static const Item_field *json_valid_single_field_arg(const Item *expr,
                                                     bool resolved)
{
  const Item_func *func;
  const Item *arg;

  if (!expr || expr->type() != Item::FUNC_ITEM)
    return NULL;
  func= static_cast<const Item_func *>(expr);
  if (func->functype() != Item_func::JSON_VALID_FUNC ||
      func->argument_count() != 1)
    return NULL;
  arg= func->arguments()[0];
  if (resolved)
    arg= arg->real_item();
  if (arg->type() != Item::FIELD_ITEM)
    return NULL;
  return static_cast<const Item_field *>(arg);
}


/**
   Whether an expression asks nothing but whether one particular column
   holds a document.

   The COLUMN, and not the field object standing for it.  A table can
   have more than one of those for the same column: the second and third
   row images a trigger reads through OLD. and NEW. are reached through
   copies made by Field::make_new_field(), which memdups the field and
   points it at the other buffer.  A copy carries the same check
   constraint - the same pointer, to an expression that still names the
   field it was fixed against - so comparing the objects makes every
   copy of a JSON column stop being one, and what it holds is quoted
   into a document instead of going in as a document.

   Same table and same position is the whole of the question and gives
   nothing away.  A column check can only read columns of its own table,
   so a check that names a different position is a check about another
   column - which is the thing this asks in order to refuse - and one
   that names the same position in another table is a copy that was put
   somewhere else, where the constraint it brought along says nothing.
*/

bool Type_handler_json_common::is_json_valid_of_field_expr(const Item *expr,
                                                           const Field *field)
{
  const Item_field *arg= json_valid_single_field_arg(expr, true);

  return arg && arg->field->table == field->table &&
         arg->field->field_index == field->field_index;
}


/**
   Whether a column carries a check constraint that says that column holds
   a document, which is what makes it a JSON column.

   The question has to be about this column: a constraint reading another
   column, or none at all, promises nothing about what is in this one, and
   a column typed JSON on such a promise has its contents put into
   documents verbatim rather than quoted.

   A constraint the server wrote for a temporary table column carries no
   vcol type, so there is nothing to ask about it beyond the expression;
   a constraint belonging to the table rather than to a column never
   reaches a column's check_constraint in the first place.
*/

bool Type_handler_json_common::has_json_valid_constraint(const Field *field)
{
  return field->check_constraint &&
         is_json_valid_of_field_expr(field->check_constraint->expr, field);
}


/**
   The same question about a check constraint read from a table
   definition, where it also has to be the column's own check rather than
   the table's before its result can be trusted.
*/

bool Type_handler_json_common::is_json_valid_of_field(Virtual_column_info *check,
                                                      const Field *field)
{
  return check && check->get_vcol_type() == VCOL_CHECK_FIELD &&
         is_json_valid_of_field_expr(check->expr, field);
}


/**
   The same question asked while a table is being defined.

   There the expression has not been fixed yet, so there is no Field to
   compare against and the column is recognised by name instead.  Do not
   fold this into is_json_valid_of_field_expr(): that one runs on an open
   table, where names have already been resolved and the identity of the
   Field is the stronger answer.
*/

bool Type_handler_json_common::is_json_valid_of_name(const Item *expr,
                                                     const LEX_CSTRING &name)
{
  const Item_field *arg= json_valid_single_field_arg(expr, false);

  return arg && !my_strcasecmp(system_charset_info, arg->field_name.str,
                               name.str);
}


/**
   Whether a check constraint cannot pass without the named column holding
   a document.

   A conjunction has to hold in full, so a JSON_VALID() anywhere among its
   parts is required; anywhere else in an expression it may be what the
   constraint passes without.
*/

static bool json_valid_of_name_required(Item *expr, const LEX_CSTRING &name)
{
  Item_cond *cond;
  List_iterator_fast<Item> li;
  Item *item;

  if (Type_handler_json_common::is_json_valid_of_name(expr, name))
    return true;
  if (!expr || expr->type() != Item::COND_ITEM)
    return false;
  cond= static_cast<Item_cond *>(expr);
  if (cond->functype() != Item_func::COND_AND_FUNC)
    return false;
  li.init(*cond->argument_list());
  while ((item= li++))
  {
    if (json_valid_of_name_required(item, name))
      return true;
  }
  return false;
}


/**
   Say so when a check constraint mentions JSON_VALID() but leaves the
   column an ordinary one.

   Writing JSON_VALID() in a column's check constraint is how a JSON column
   is asked for, so a constraint that mentions it and still does not make
   the column a JSON column is worth reporting: it reads something other
   than this column, or it can be satisfied without the call holding, or it
   asks something else as well.  All three are quiet, and until the column
   is used in a document none of them shows.

   What is asked here is what types the column and nothing weaker.  A
   conjunction cannot pass unless every part of it does, so JSON_VALID()
   inside one is a promise that really is kept - but a promise kept is not
   the same thing as a column typed, and has_json_valid_constraint() wants
   the call and nothing else.  Asking whether the promise is kept, which is
   what this used to ask, left the commonest shape of all - JSON_VALID(c)
   AND something - typed as text, warned about not at all, and reported to
   the client as a JSON column by the protocol.
*/

void Type_handler_json_common::warn_if_json_valid_does_not_type(
                                            THD *thd,
                                            Virtual_column_info *check,
                                            const LEX_CSTRING &name)
{
  Item_func::Functype json_valid= Item_func::JSON_VALID_FUNC;

  if (!check || !check->expr || is_json_valid_of_name(check->expr, name))
    return;

  if (check->expr->walk(&Item::json_valid_of_column_processor, 0,
                        const_cast<LEX_CSTRING *>(&name)))
  {
    /*
      The call is there and it is about this column, so what stands in the
      way is one of two things, and they are worth telling apart: the
      constraint can be satisfied without the call holding, or it cannot
      and the constraint is still not the call alone.  Only the second is
      a constraint that does what its writer meant and is refused anyway.
    */
    uint code= json_valid_of_name_required(check->expr, name) ?
               ER_WARN_JSON_VALID_NOT_ALONE : ER_WARN_JSON_VALID_NOT_REQUIRED;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, code,
                        ER_THD(thd, code), name.str);
  }
  else if (check->expr->walk(&Item::find_function_processor, 0, &json_valid))
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WARN_JSON_VALID_OTHER_COLUMN,
                        ER_THD(thd, ER_WARN_JSON_VALID_OTHER_COLUMN),
                        name.str, name.str);
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
};


const Type_collection *Type_handler_json_common::type_collection()
{
  static Type_collection_json type_collection_json;
  return &type_collection_json;
}
