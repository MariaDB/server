/*
   Copyright (c) 2009, 2021, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */


#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
#include <m_ctype.h>
#include "sql_select.h"
#include "opt_trace.h"

/**
  @file

  @brief
    Virtual Column Substitution feature makes the optimizer recognize usage of
    virtual column expressions in the WHERE/ON clauses. If there is an index
    on the virtual column, the optimizer is able to construct query plans that
    use that index.
*/


/*
  == Virtual Column Substitution In a Nutshell ==

  Consider a table that defines a virtual column and an index on it:

  CREATE TABLE t1 (
     json_col BLOB,
     ...
     vcol1 VARCHAR(100) AS (json_extract(json_col, '$.name')), //(*), see below
     INDEX idx1(vcol1)
  );

  And then a query that uses virtual column's expression:

  SELECT * FROM t1 WHERE json_extract(json_col, '$.name')='foo'

  We'd like this query to use index idx1.
  In order to achieve that, we look through potentially sargable conditions
  to find the virtual column expression (we only accept exact match) and
  replace it with a reference to virtual column field so the query becomes:

    SELECT * FROM t1 WHERE vcol1='foo'

  Then, the optimizer is able to construct ref access on index idx1.

  (*) When extracting JSON fields in the real world, you'll probably want to
      use json_unquote().

  == Datatypes must match ==

  The type of vcol_field and vcol_expr may not match. Consider

  CREATE TABLE t1 (
    a varchar(10),
    vcol INT as CONCAT(a,'1')
  );

  and conditions

    concat(a,'1')=1.5  vs  vcol=1.5.

  == The same expression in multiple virtual columns ==

  What if there are multiple options to replace:

  CREATE TABLE t1 (
     col1 int,
     ...
     vcol1 INT as (col1 + 1),
     vcol2 INT as (col1 + 1),
     ...
     INDEX idx1(vcol1, ...)
     INDEX idx2(vcol2, ...)
  );
  SELECT * FROM t1 WHERE col1 + 1 = 5;

  Currently, we will replace with the first matching column (vcol1), like MySQL
  does. Since we rely on the user to define the virtual columns, we can
  request that they define one virtual column instead of multiple identical
  ones.
*/


class Vcol_subst_context
{
 public:
  THD *thd;
  /* Indexed virtual columns that we can try substituting */
  List<Field> vcol_fields;

  /*
    How many times substitution was done. Used to determine whether to print
    the conversion info to the Optimizer Trace
  */
  uint subst_count;

  Vcol_subst_context(THD *thd_arg) : thd(thd_arg) {}
};

static Field *is_vcol_expr(Vcol_subst_context *ctx, const Item *item);
static
void subst_vcol_if_compatible(Vcol_subst_context *ctx,
                              Item_bool_func *cond,
                              Item **vcol_expr_ref,
                              Field *vcol_field);

static
bool collect_indexed_vcols_for_table(TABLE *table, List<Field> *vcol_fields)
{
  // TODO: Make use of iterator to loop through keys_in_use_for_query, instead.
  for (uint i=0; i < table->s->keys; i++)
  {
    // note: we could also support histograms here
    if (!table->keys_in_use_for_query.is_set(i))
      continue;

    KEY *key= &table->key_info[i];
    for (uint kp=0; kp < key->user_defined_key_parts; kp++)
    {
      Field *field= key->key_part[kp].field;
      if (field->vcol_info && vcol_fields->push_back(field))
        return TRUE; // Out of memory
    }
  }
  return FALSE; // Ok
}


/*
  Collect a list of indexed virtual columns in the JOIN's tables
*/

static
bool collect_indexed_vcols_for_join(JOIN *join, List<Field> *vcol_fields)
{
  List_iterator<TABLE_LIST> ti(join->select_lex->leaf_tables);
  TABLE_LIST *tl;
  while ((tl= ti++))
  {
    if (!tl->table) // non-merged semi-join or something like that
      continue;
    if (collect_indexed_vcols_for_table(tl->table, vcol_fields))
      return TRUE; // Out of memory
  }
  return FALSE; // Ok
}


/* Substitute virtual columns in an Item tree */
static void subst_vcols_in_item(Vcol_subst_context *ctx, Item *item,
                                const char *location)
{
  uchar *yes= (uchar*) 1;
  ctx->subst_count= 0;

  item->top_level_compile(ctx->thd,
                          &Item::vcol_subst_analyzer, &yes,
                          &Item::vcol_subst_transformer, (uchar*)ctx);

  if (ctx->subst_count && unlikely(ctx->thd->trace_started()))
    trace_condition(ctx->thd, location, "virtual_column_substitution", item);
}


static
void subst_vcols_in_join_list(Vcol_subst_context *ctx,
                              List<TABLE_LIST> *join_list)
{
  TABLE_LIST *table;
  List_iterator<TABLE_LIST> li(*join_list);

  while ((table= li++))
  {
    if (NESTED_JOIN* nested_join= table->nested_join)
      subst_vcols_in_join_list(ctx, &nested_join->join_list);

    if (table->on_expr)
      subst_vcols_in_item(ctx, table->on_expr, "ON expression");
  }
}


/*
  Substitute vcol expressions with vcol fields in ORDER BY or GROUP
  BY, and re-initialise affected tables on substitution.
*/
static
void subst_vcols_in_order(Vcol_subst_context *ctx,
                          ORDER *order,
                          JOIN *join,
                          bool is_group_by)
{
  Field *vcol_field;
  const char *location= is_group_by ? "GROUP BY" : "ORDER BY";
  for (; order; order= order->next)
  {
    Item *item= *order->item;
    uint old_count= ctx->subst_count;
    /*
      Extra safety: do not rewrite if there is no room in
      ref_pointer_array's slices (see st_select_lex::setup_ref_array)
      This check shouldn't fail, but it's better to have it just in
      case.
    */
    if (join->all_fields.elements * 5 >=
        join->select_lex->ref_pointer_array.size() - 1)
      break;

    if ((vcol_field= is_vcol_expr(ctx, item)))
      subst_vcol_if_compatible(ctx, NULL, order->item, vcol_field);
    if (ctx->subst_count > old_count)
    {
      Item *new_item= *order->item;
      /*
        If the old ORDER BY item is a SELECT item, then insert the new
        item to all_fields and keep it in sync with ref_pointer_array.
        Otherwise it is safe to replace the old item with the new item
        in all_fields.
      */
      if (order->in_field_list)
      {
        uint el= join->all_fields.elements;
        join->all_fields.push_front(new_item);
        join->select_lex->ref_pointer_array[el]= new_item;
        order->item= &join->select_lex->ref_pointer_array[el];
        order->in_field_list= false;
      }
      /*
        TODO: should we deduplicate by calling find_item_in_list on
        new_item like in find_order_in_list, and remove item instead
        of replacing it if new_item is already in all_fields?
      */
      else
      {
        List_iterator<Item> it(join->all_fields);
        while (Item *item_in_all_fields= it++)
        {
          if (item_in_all_fields == item)
            it.replace(new_item);
        }
      }
      /*
        Re-initialise index covering of affected tables, which will
        be re-computed to account for the substitution.
      */
      TABLE *tab= vcol_field->table;
      tab->covering_keys= tab->s->keys_for_keyread;
      tab->covering_keys.intersect(tab->keys_in_use_for_query);
      if (unlikely(ctx->thd->trace_started()))
      {
        Json_writer_object trace_wrapper(ctx->thd);
        Json_writer_object trace_order_by(ctx->thd, "virtual_column_substitution");
        trace_order_by.add("location", location);
        trace_order_by.add("from", item);
        trace_order_by.add("to", new_item);
      }
    }
  }
}

/*
  @brief
    Do substitution for all condition in a JOIN, and all ORDER BY and
    GROUP BY items. This is the primary entry point. Recount field
    types and re-compute index coverings when any substitution has
    happened in ORDER BY or GROUP BY.
*/

bool substitute_indexed_vcols_for_join(JOIN *join)
{
  Vcol_subst_context ctx(join->thd);
  if (collect_indexed_vcols_for_join(join, &ctx.vcol_fields))
    return true; // Out of memory

  if (!ctx.vcol_fields.elements)
    return false; // Ok, nothing to do

  if (join->conds)
    subst_vcols_in_item(&ctx, join->conds, "WHERE");
  if (join->join_list)
    subst_vcols_in_join_list(&ctx, join->join_list);
  ctx.subst_count= 0;
  if (join->order)
    subst_vcols_in_order(&ctx, join->order, join, false);
  if (join->group_list)
    subst_vcols_in_order(&ctx, join->group_list, join, true);
  if (ctx.subst_count)
  {
    count_field_types(join->select_lex, &join->tmp_table_param,
                      join->all_fields, 0);
    join->select_lex->update_used_tables();
  }

  if (join->thd->is_error())
    return true; // Out of memory
  return false; // Ok
}


/*
  @brief
    Do substitution for one table and condition. This is for single-table
    UPDATE/DELETE.
*/

bool substitute_indexed_vcols_for_table(TABLE *table, Item *item,
                                        ORDER *order, SELECT_LEX *select_lex)
{
  Vcol_subst_context ctx(table->in_use);
  if (collect_indexed_vcols_for_table(table, &ctx.vcol_fields))
    return true; // Out of memory

  if (!ctx.vcol_fields.elements)
    return false; // Ok, nothing to do

  if (item)
    subst_vcols_in_item(&ctx, item, "WHERE");

  ctx.subst_count= 0;           /* used in subst_vcol_in_order */
  if (order)
    subst_vcols_in_order(&ctx, order, select_lex->join, false);

  if (table->in_use->is_error())
    return true; // Out of memory

  return false; // Ok
}


/*
  @brief
    Check if passed item matches Virtual Column definition for some column in
    the Vcol_subst_context list.
*/

static Field *is_vcol_expr(Vcol_subst_context *ctx, const Item *item)
{
  table_map map= item->used_tables();
  if ((map!=0) && !(map & OUTER_REF_TABLE_BIT) &&
      !(map & (map - 1))) // has exactly one bit set
  {
    List_iterator<Field> it(ctx->vcol_fields);
    Field *field;
    while ((field= it++))
    {
      if (field->vcol_info->expr->eq(item, true))
        return field;
    }
  }
  return NULL;
}


/*
  @brief
    Produce a warning similar to raise_note_cannot_use_key_part().
*/

void print_vcol_subst_warning(THD *thd, Field *field, Item *expr,
                              const char *cause)
{
  StringBuffer<128> expr_buffer;
  size_t expr_length;

  expr->print(&expr_buffer, QT_EXPLAIN);
  expr_length= Well_formed_prefix(expr_buffer.charset(),
                                  expr_buffer.ptr(),
                                  MY_MIN(expr_buffer.length(), 64)).length();

  push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                      ER_UNKNOWN_ERROR,
                      "Cannot substitute virtual column expression %*s -> %*s "
                      "due to %s",
                      (int) expr_length, expr_buffer.c_ptr_safe(),
                      (int) field->field_name.length, field->field_name.str,
                      cause);
}


/*
  @brief
    Check if we can substitute (*vcol_expr_ref) with vcol_field in condition
    cond and if we can, do the substitution.

  @detail
    Generally, we can do the substitution if the condition after substitution
    is equivalent to the condition before the substitution.

    They are equivalent if the datatypes of vcol_field and *vcol_expr_ref are
    the same. This requirement can be relaxed - one can come up with cases
    where the datatypes are different but substitution doesn't change the
    condition.

    Note that the data type of the virtual column is specified by the user and
    so can be totally different from virtual column's expression. For example,
    one can do:

      col3 INT AS (CONCAT(col1, col2))

    For strings, we allow two cases:
    - vcol_expr and vcol_field have the same collation
    - vcol_field has the same collation as the condition's comparison collation

    (Note: MySQL calls resolve_type() after it has done the substitution.
     This can potentially update the comparator. The idea is that this
     shouldn't be necessary as we do not want to change the comparator.
     Changing the comparator will change the semantics of the condition,
     our point is that this must not happen)

  @return
     None.
     In case OOM error happens in this function, we have no way to propagate
     the error out of Item::compile(). So, the code that calls Item::compile()
     for vcol substitution will check thd->is_error() afterwards.
*/

static
void subst_vcol_if_compatible(Vcol_subst_context *ctx,
                              Item_bool_func *cond,
                              Item **vcol_expr_ref,
                              Field *vcol_field)
{
  Item *vcol_expr= *vcol_expr_ref;
  THD *thd= ctx->thd;

  const char *fail_cause= NULL;
  if (vcol_expr->type_handler_for_comparison() !=
      vcol_field->type_handler_for_comparison() ||
      (vcol_expr->maybe_null() && !vcol_field->maybe_null()))
    fail_cause="type mismatch";
  else
  {
    CHARSET_INFO *cs= cond ? cond->compare_collation() : NULL;
    if (vcol_expr->collation.collation != vcol_field->charset() &&
        cs != vcol_field->charset())
      fail_cause="collation mismatch";
  }

  if (fail_cause)
  {
    if (thd->give_notes_for_unusable_keys())
      print_vcol_subst_warning(thd, vcol_field, vcol_expr, fail_cause);
    return;
  }
  Item_field *itf= new (thd->mem_root) Item_field(thd, vcol_field);
  if (!itf)
    return; // Out of memory, caller will know from thd->is_error()
  if (vcol_expr->type() == Item::FIELD_ITEM)
    itf->context= ((Item_field *)vcol_expr)->context;
  bitmap_set_bit(vcol_field->table->read_set, vcol_field->field_index);
  DBUG_ASSERT(itf->fixed());
  thd->change_item_tree(vcol_expr_ref, itf);
  ctx->subst_count++;
  return;
}


/*
  @brief
    Do a quick and imprecise check if it makes sense to try Virtual Column
    Substitution transformation for this item.

  @detail
    For vcol_expr='FOO' the item to be trans formed is the comparison item
    (Item_func_eq in this example), not the item representing vcol_expr.
*/

bool Item::vcol_subst_analyzer(uchar **)
{
  const ulonglong allowed_cmp_funcs=
     Item_func::BITMAP_EQ |
     Item_func::BITMAP_EQUAL |
     Item_func::BITMAP_LT |
     Item_func::BITMAP_GT |
     Item_func::BITMAP_LE |
     Item_func::BITMAP_GE |
     Item_func::BITMAP_BETWEEN |
     Item_func::BITMAP_IN |
     Item_func::BITMAP_ISNULL |
     Item_func::BITMAP_ISNOTNULL;

  Item::Type this_type= type();
  /*
    Do transformation
    1. Inside AND/OR
    2. In selected list of comparison predicates
  */
  return (this_type == Item::COND_ITEM ||                            // (1)
          (this_type == Item::FUNC_ITEM &&                           // (2)
           (((Item_func*)this)->bitmap_bit() & allowed_cmp_funcs))); // (2)
}


Item* Item_bool_rowready_func2::vcol_subst_transformer(THD *thd, uchar *arg)
{
  DBUG_ASSERT(this->vcol_subst_analyzer(NULL));
  Vcol_subst_context *ctx= (Vcol_subst_context*)arg;
  Field *vcol_field;
  Item **vcol_expr;

  if (!args[0]->used_tables() && (vcol_field= is_vcol_expr(ctx, args[1])))
    vcol_expr= &args[1];
  else if (!args[1]->used_tables() && (vcol_field= is_vcol_expr(ctx, args[0])))
    vcol_expr= &args[0];
  else
    return this; /* No substitution */

  DBUG_EXECUTE_IF("vcol_subst_simulate_oom",
                  DBUG_SET("+d,simulate_out_of_memory"););

  subst_vcol_if_compatible(ctx, this, vcol_expr, vcol_field);

  DBUG_EXECUTE_IF("vcol_subst_simulate_oom",
                  DBUG_SET("-d,vcol_subst_simulate_oom"););
  return this;
}


Item* Item_func_between::vcol_subst_transformer(THD *thd, uchar *arg)
{
  Vcol_subst_context *ctx= (Vcol_subst_context*)arg;
  Field *vcol_field;
  if (!args[1]->used_tables() &&
      !args[2]->used_tables() &&
      (vcol_field= is_vcol_expr(ctx, args[0])))
  {
    subst_vcol_if_compatible(ctx, this, &args[0], vcol_field);
  }
  return this;
}


Item* Item_func_null_predicate::vcol_subst_transformer(THD *thd, uchar *arg)
{
  Vcol_subst_context *ctx= (Vcol_subst_context*)arg;
  Field *vcol_field;
  if ((vcol_field= is_vcol_expr(ctx, args[0])))
    subst_vcol_if_compatible(ctx, this, &args[0], vcol_field);
  return this;
}


Item* Item_func_in::vcol_subst_transformer(THD *thd, uchar *arg)
{
  Vcol_subst_context *ctx= (Vcol_subst_context*)arg;
  Field *vcol_field= nullptr;

  /*
    Check that the left hand side of IN() is a virtual column expression and
    that all arguments inside IN() are constants.
  */
  if (!(vcol_field= is_vcol_expr(ctx, args[0])) ||
      !compatible_types_scalar_bisection_possible())
    return this;

  subst_vcol_if_compatible(ctx, this, &args[0], vcol_field);
  return this;
}

