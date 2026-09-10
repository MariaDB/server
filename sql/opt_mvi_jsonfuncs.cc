/*
   Copyright (c) 2026, MariaDB

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

/*
  Making the JSON functions use a Multi-Value Index.

  This is the part of the MVI support that knows about the JSON predicates:
  which of them can be computed from an index, which argument holds the
  indexed expression, and what to search the index for. The index side of it
  lives in opt_multi_valued_index.cc.
*/

#include "mariadb.h"
#include "sql_select.h"
#include "item_func.h"

static Mvi_access *collect_mvi_keys(THD *thd, Mv_index *index,
                                    CHARSET_INFO *cs, const String *json,
                                    bool conjunctive, json_engine_t *je);


/*
  @brief
    Find Multi-Value Index created over array_indexed_expr.

  @detail
    Search the table for an index declared as

     INDEX idx ((CAST(array_indexed_expr AS <DATATYPE> ARRAY));

    NOTE: we currently we only locate one such index. What if there are
          multiple?
*/

static Mv_index *get_mvi_index(List<Mv_index> *indexes,
                               Item *array_indexed_expr)
{
  Mv_index *index;
  List_iterator<Mv_index> it(*indexes);
  Item_func_mvi_encode *mvitem;
  while ((index= it++))
  {
    Field *vcol_field= index->vcol;
    DBUG_ASSERT(vcol_field->vcol_info->expr->type() == Item::FUNC_ITEM);
    DBUG_ASSERT(((Item_func *) vcol_field->vcol_info->expr)->functype() ==
                Item_func::MVI_ENCODE_FUNC);
    mvitem= (Item_func_mvi_encode *) vcol_field->vcol_info->expr;
    if (mvitem->arguments()[0]->eq(array_indexed_expr, true))
    {
      return index;
    }
  }
  return NULL;
}


/*
  @brief
    Check if we can use Multi-Value Index access to read rows for this
    predicate, if yes create an access descriptor.

  @detail
    Check if this item is a

      JSON_CONTAINS(array_indexed_expr, '[foo, bar, ... ]')

    which is true when ALL of the elements have a match, so the keys are
    ANDed.

  @return
    The access descriptor, or NULL if the predicate cannot use an MVI.
*/

Mvi_access *Item_func_json_contains::get_mvi_access(THD *thd,
                                                    List<Mv_index> *indexes)
{
  Mv_index *index;
  DBUG_ASSERT(fixed());

  if (arg_count > 2 || !a2_constant)
    return NULL;
  /* Find the MVI that matches the first argument */
  if (!(index= get_mvi_index(indexes, args[0])))
    return NULL;

  if (!a2_parsed)
  {
    val= args[1]->val_json(&tmp_val);
    a2_parsed= true;
  }
  if (!val)
    return NULL;

  return collect_mvi_keys(thd, index, args[0]->collation.collation, val,
                          true, &je);
}

/*
  @brief
    Check if we can use Multi-Value Index access to read rows for this
    predicate, if yes create an access descriptor.
  
  @detail
    We can use MVI index when the predicate has either of the forms:

      JSON_OVERLAPS(array_indexed_expr, '[foo, bar, ... ]')
      JSON_OVERLAPS('[foo, bar, ... ]', array_indexed_expr)

    JSON_OVERLAPS is true when ANY of the elements has a match, so the keys
    are ORed. 

  @return
    The access descriptor, or NULL if the predicate cannot use an MVI.
*/

Mvi_access *Item_func_json_overlaps::get_mvi_access(THD *thd,
                                                    List<Mv_index> *indexes)
{
  Mv_index *index;
  uint literal_arg;
  String *json;
  StringBuffer<256> tmp;
  DBUG_ASSERT(fixed());

  if ((index= get_mvi_index(indexes, args[0])))
    literal_arg= 1;
  else if ((index= get_mvi_index(indexes, args[1])))
    literal_arg= 0;
  else
    return NULL;

  if (!args[literal_arg]->const_item())
    return NULL;
  if (!(json= args[literal_arg]->val_json(&tmp)))
    return NULL;

  /*
    TODO: is this really so:
    encode_mvi_key() must see the collation of the indexed expression: that
    is what decides how MVI_ENCODE built the keys that are in the index.
  */
  return collect_mvi_keys(thd, index,
                          args[1 - literal_arg]->collation.collation, json,
                          false, &je);
}


/*
  @brief
    Collect the element keys to search `index' for from a JSON literal.

  @param cs           Collation of the indexed expression
                      TODO why does that matter?

  @param json         The JSON literal: an array, or a single scalar
  @param conjunctive  true when the keys are ANDed (JSON_CONTAINS),
                      false when they are ORed (JSON_OVERLAPS)
  @param je           A json_engine_t to scan with

  @detail
    An element that cannot be encoded for this index (a type mismatch, say)
    can only be skipped when the keys are ANDed. Dropping a key from an AND
    makes the index scan less selective, so it still returns a superset of
    the rows the predicate matches, and the predicate itself does the exact
    filtering afterwards.

    For an OR we cannot do that. A row can satisfy the predicate through the
    very element we failed to encode, and MVI_ENCODE skips such elements too,
    so that row has no key in the index for us to find it by. Dropping the
    key would lose it. Give up on the access instead.

    An element that is itself an array is flattened, the same way
    MVI_ENCODE flattens the document. JSON_OVERLAPS does not flatten:
    it only matches such an element against a document element that is
    an array too, compared whole (json_compare_arrays_in_order()). The
    flattening here is still safe as it will produce only false
    positives that will be eliminated by a recheck. The only exception
    is when the nested array yields no key at all i.e. [], [[]],
    [[],[]], [[[]]], etc. Such an element may match a document element
    that has no key of ours either, so nothing we could search for
    would find that row. Give up in this case, as for a failed
    encoding.

  @return
    The access descriptor, or NULL if the predicate cannot use this MVI.
*/

static Mvi_access *collect_mvi_keys(THD *thd, Mv_index *index,
                                    CHARSET_INFO *cs, const String *json,
                                    bool conjunctive, json_engine_t *je)
{
  Mvi_access *access= NULL;
  StringBuffer<256> buf;
  const uchar *start= reinterpret_cast<const uchar *>(json->ptr());
  const uchar *end= start + json->length();
  Item_func_mvi_encode *mvitem=
    (Item_func_mvi_encode *) index->vcol->vcol_info->expr;
  const Type_handler *cast_th= mvitem->cast_type().type_handler();
  int depth= 0;
  /*
    The number of keys collected when inside a current depth-2 array
    element. Only used for an OR / JSON_OVERLAPS
  */
  uint keys_before_level2_array= 0;

  buf.length(0);
  buf.set_charset(&my_charset_latin1_bin);

  if (json_scan_start(je, cs, start, end) || json_read_value(je))
    return NULL;

  if (je->value_type == JSON_VALUE_UNINITIALIZED ||
      je->value_type == JSON_VALUE_OBJECT)
    return NULL;

  if (je->value_type != JSON_VALUE_ARRAY)
  {
    /* A scalar: JSON_CONTAINS(expr, '123') */
    if (encode_mvi_key(je, cast_th, cs, &buf))
      return NULL;
    if (!(access= new (thd->mem_root) Mvi_access(index, conjunctive)) ||
        access->add_key(thd->mem_root, &buf))
      return NULL;
    return access;
  }
  // JSON_VALUE_ARRAY

  /* TODO: deduplicate? */
  /*
    TODO: the logic here parallels
    Item_func_mvi_encode::val_str_ascii. A refactoring is called for
  */
  do {
    buf.length(0);
    switch (je->state)
    {
      case JST_ARRAY_START:
        depth++;
        break;
      case JST_ARRAY_END:
        if (--depth == 0)
          return access;
        /*
          Closed a top-level element that was an array. See above: for an OR
          it has to have contributed at least one key.
        */
        if (depth == 1 && !conjunctive &&
            (access ? access->encoded.elements : 0) == keys_before_level2_array)
          return NULL;
        break;
      case JST_VALUE:
      {
        if (json_read_value(je))
          return NULL;
        if (je->state == JST_ARRAY_START)
        {
          if (++depth == 2)
            keys_before_level2_array= access ? access->encoded.elements : 0;
          break;
        }
        if (je->value_type == JSON_VALUE_OBJECT)
        {
          if (json_skip_level(je) || !conjunctive)
            return NULL;
          break;
        }
        if (encode_mvi_key(je, cast_th, cs, &buf))
        {
          /* See above: only an AND of the keys tolerates a missing one */
          if (!conjunctive)
            return NULL;
          break;
        }
        if (!access &&
            !(access= new (thd->mem_root) Mvi_access(index, conjunctive)))
          return NULL;
        if (access->add_key(thd->mem_root, &buf))
          return NULL;
        break;
      }
      default:
        return NULL;
    }
  } while (json_scan_next(je) == 0);

  return depth > 0 ? NULL : access;
}


/* Add `access' to the context, if there is one. Returns true on error */

static bool add_mvi_access(Mvi_context *ctx, Mvi_access *access)
{
  return access && ctx->accesses.push_back(access, ctx->thd->mem_root);
}


bool Item_func_json_contains::mvi_analyze(void *arg)
{
  Mvi_context *ctx= (Mvi_context *) arg;
  return add_mvi_access(ctx, get_mvi_access(ctx->thd, &ctx->indexes));
}


bool Item_func_json_overlaps::mvi_analyze(void *arg)
{
  Mvi_context *ctx= (Mvi_context *) arg;
  return add_mvi_access(ctx, get_mvi_access(ctx->thd, &ctx->indexes));
}
