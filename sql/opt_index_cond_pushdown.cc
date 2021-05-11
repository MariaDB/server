/*
   Copyright (c) 2009, 2012, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "mariadb.h"
#include "sql_select.h"
#include "sql_test.h"

/****************************************************************************
 * Index Condition Pushdown code starts
 ***************************************************************************/
/* 
  Check if given expression uses only table fields covered by the given index

  SYNOPSIS
    uses_index_fields_only()
      item           Expression to check
      tbl            The table having the index
      keyno          The index number
      other_tbls_ok  TRUE <=> Fields of other non-const tables are allowed

  DESCRIPTION
    Check if given expression only uses fields covered by index #keyno in the
    table tbl. The expression can use any fields in any other tables.
    
    The expression is guaranteed not to be AND or OR - those constructs are 
    handled outside of this function.

  RETURN
    TRUE   Yes
    FALSE  No
*/

bool uses_index_fields_only(Item *item, TABLE *tbl, uint keyno,
                            bool other_tbls_ok)
{
  if (item->walk(&Item::limit_index_condition_pushdown_processor, FALSE, NULL))
  {
    return FALSE;
  }

  if (item->const_item())
    return TRUE;

  /* 
    Don't push down the triggered conditions. Nested outer joins execution 
    code may need to evaluate a condition several times (both triggered and
    untriggered), and there is no way to put thi
    TODO: Consider cloning the triggered condition and using the copies for:
      1. push the first copy down, to have most restrictive index condition
         possible
      2. Put the second copy into tab->select_cond. 
  */
  if (item->type() == Item::FUNC_ITEM && 
      ((Item_func*)item)->functype() == Item_func::TRIG_COND_FUNC)
    return FALSE;

  if (!(item->used_tables() & tbl->map))
    return other_tbls_ok;

  Item::Type item_type= item->type();
  switch (item_type) {
  case Item::FUNC_ITEM:
    {
      /* This is a function, apply condition recursively to arguments */
      Item_func *item_func= (Item_func*)item;
      Item **child;
      Item **item_end= (item_func->arguments()) + item_func->argument_count();
      for (child= item_func->arguments(); child != item_end; child++)
      {
        if (!uses_index_fields_only(*child, tbl, keyno, other_tbls_ok))
          return FALSE;
      }
      return TRUE;
    }
  case Item::COND_ITEM:
    {
      /*
        This is a AND/OR condition. Regular AND/OR clauses are handled by
        make_cond_for_index() which will chop off the part that can be
        checked with index. This code is for handling non-top-level AND/ORs,
        e.g. func(x AND y).
      */
      List_iterator<Item> li(*((Item_cond*)item)->argument_list());
      Item *item;
      while ((item=li++))
      {
        if (!uses_index_fields_only(item, tbl, keyno, other_tbls_ok))
          return FALSE;
      }
      return TRUE;
    }
  case Item::FIELD_ITEM:
    {
      Item_field *item_field= (Item_field*)item;
      Field *field= item_field->field;
      if (field->table != tbl)
        return TRUE;
      /*
        The below is probably a repetition - the first part checks the
        other two, but let's play it safe:
      */
      if(!field->part_of_key.is_set(keyno) ||
         field->type() == MYSQL_TYPE_GEOMETRY ||
         field->type() == MYSQL_TYPE_BLOB)
        return FALSE;
      KEY *key_info= tbl->key_info + keyno;
      KEY_PART_INFO *key_part= key_info->key_part;
      KEY_PART_INFO *key_part_end= key_part + key_info->user_defined_key_parts;
      for ( ; key_part < key_part_end; key_part++)
      {
        if (field->eq(key_part->field))
	  return !(key_part->key_part_flag & HA_PART_KEY_SEG);          
      }
      if ((tbl->file->ha_table_flags() & HA_PRIMARY_KEY_IN_READ_INDEX) &&
          tbl->s->primary_key != MAX_KEY &&
	  tbl->s->primary_key != keyno)
      {
        key_info= tbl->key_info + tbl->s->primary_key;
        key_part= key_info->key_part;
        key_part_end= key_part + key_info->user_defined_key_parts;
        for ( ; key_part < key_part_end; key_part++)
        {
          /* 
            It does not make sense to use the fact that the engine can read in
            a full field if the key if the index is built only over a part
            of this field.
	  */
          if (field->eq(key_part->field))
	    return !(key_part->key_part_flag & HA_PART_KEY_SEG);          
        }
      }  
      return FALSE;
    }
  case Item::REF_ITEM:
    return uses_index_fields_only(item->real_item(), tbl, keyno,
                                  other_tbls_ok);
  default:
    return FALSE; /* Play it safe, don't push unknown non-const items */
  }
}


/*
  Get a part of the condition that can be checked using only index fields

  SYNOPSIS
    make_cond_for_index()
      cond           The source condition
      table          The table that is partially available
      keyno          The index in the above table. Only fields covered by the
                     index are available
      other_tbls_ok  TRUE <=> Fields of other non-const tables are allowed

  DESCRIPTION
    Get a part of the condition that can be checked when for the given table 
    we have values only of fields covered by some index. The condition may
    refer to other tables, it is assumed that we have values of all of their 
    fields.

    Example:
      make_cond_for_index(
       "cond(t1.field) AND cond(t2.key1) AND cond(t2.non_key) AND cond(t2.key2)",
        t2, keyno(t2.key1))
      will return
        "cond(t1.field) AND cond(t2.key2)"

  RETURN
    Index condition, or NULL if no condition could be inferred.
*/

static Item *make_cond_for_index(THD *thd, Item *cond, TABLE *table, uint keyno,
                                 bool other_tbls_ok)
{
  if (!cond || cond->basic_const_item())
    return cond;
  if (cond->type() == Item::COND_ITEM)
  {
    if (((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
    {
      table_map used_tables= 0;
      Item_cond_and *new_cond= new (thd->mem_root) Item_cond_and(thd);
      if (!new_cond)
	return (COND*) 0;
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item=li++))
      {
	Item *fix= make_cond_for_index(thd, item, table, keyno, other_tbls_ok);
	if (fix)
        {
	  new_cond->argument_list()->push_back(fix, thd->mem_root);
          used_tables|= fix->used_tables();
        }
      }
      switch (new_cond->argument_list()->elements) {
      case 0:
	return (COND*) 0;
      case 1:
        /* remove AND level if there is only one argument */
	return new_cond->argument_list()->head();
      default:
	new_cond->quick_fix_field();
        new_cond->used_tables_cache= used_tables;
	return new_cond;
      }
    }
    else /* It's OR */
    {
      Item_cond_or *new_cond= new (thd->mem_root) Item_cond_or(thd);
      if (!new_cond)
	return (COND*) 0;
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item=li++))
      {
        Item *fix= make_cond_for_index(thd, item, table, keyno, other_tbls_ok);
	if (!fix)
	  return (COND*) 0;
	new_cond->argument_list()->push_back(fix, thd->mem_root);
      }
      new_cond->quick_fix_field();
      new_cond->used_tables_cache= ((Item_cond_or*) cond)->used_tables_cache;
      new_cond->top_level_item();
      return new_cond;
    }
  }

  if (!uses_index_fields_only(cond, table, keyno, other_tbls_ok))
    return (COND*) 0;
  return cond;
}


static Item *make_cond_remainder(THD *thd, Item *cond, TABLE *table, uint keyno,
                                 bool other_tbls_ok, bool exclude_index)
{
  if (exclude_index && 
      uses_index_fields_only(cond, table, keyno, other_tbls_ok))
    return 0;

  if (cond->type() == Item::COND_ITEM)
  {
    table_map tbl_map= 0;
    if (((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
    {
      /* Create new top level AND item */
      Item_cond_and *new_cond= new (thd->mem_root) Item_cond_and(thd);
      if (!new_cond)
        return (COND*) 0;
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item=li++))
      {
        Item *fix= make_cond_remainder(thd, item, table, keyno,
                                       other_tbls_ok, exclude_index);
	if (fix)
        {
	  new_cond->argument_list()->push_back(fix, thd->mem_root);
          tbl_map |= fix->used_tables();
        }
      }
      switch (new_cond->argument_list()->elements) {
      case 0:
	return (COND*) 0;
      case 1:
	return new_cond->argument_list()->head();
      default:
	new_cond->quick_fix_field();
        ((Item_cond*)new_cond)->used_tables_cache= tbl_map;
	return new_cond;
      }
    }
    else /* It's OR */
    {
      Item_cond_or *new_cond= new (thd->mem_root) Item_cond_or(thd);
      if (!new_cond)
	return (COND*) 0;
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item=li++))
      {
        Item *fix= make_cond_remainder(thd, item, table, keyno,
                                       other_tbls_ok, FALSE);
	if (!fix)
	  return (COND*) 0;
	new_cond->argument_list()->push_back(fix, thd->mem_root);
        tbl_map |= fix->used_tables();
      }
      new_cond->quick_fix_field();
      ((Item_cond*)new_cond)->used_tables_cache= tbl_map;
      new_cond->top_level_item();
      return new_cond;
    }
  }
  return cond;
}


/*
  Try to extract and push the index condition

  SYNOPSIS
    push_index_cond()
      tab            A join tab that has tab->table->file and its condition
                     in tab->select_cond
      keyno          Index for which extract and push the condition

  DESCRIPTION
    Try to extract and push the index condition down to table handler
*/

void push_index_cond(JOIN_TAB *tab, uint keyno)
{
  DBUG_ENTER("push_index_cond");
  Item *idx_cond;
  
  /*
  Backported the following from MySQL 5.6:
    6. The index is not a clustered index. The performance improvement
       of pushing an index condition on a clustered key is much lower 
       than on a non-clustered key. This restriction should be 
       re-evaluated when WL#6061 is implemented.
  */
  if ((tab->table->file->index_flags(keyno, 0, 1) &
      HA_DO_INDEX_COND_PUSHDOWN) &&
      optimizer_flag(tab->join->thd, OPTIMIZER_SWITCH_INDEX_COND_PUSHDOWN) &&
      tab->join->thd->lex->sql_command != SQLCOM_UPDATE_MULTI &&
      tab->join->thd->lex->sql_command != SQLCOM_DELETE_MULTI &&
      tab->type != JT_CONST && tab->type != JT_SYSTEM &&
      !tab->table->file->is_clustering_key(keyno)) // 6
  {
    DBUG_EXECUTE("where",
                 print_where(tab->select_cond, "full cond", QT_ORDINARY););

    idx_cond= make_cond_for_index(tab->join->thd, tab->select_cond, tab->table,
                                  keyno, tab->icp_other_tables_ok);

    DBUG_EXECUTE("where",
                 print_where(idx_cond, "idx cond", QT_ORDINARY););

    if (idx_cond)
    {
      Item *idx_remainder_cond= 0;
      tab->pre_idx_push_select_cond= tab->select_cond;
      /*
        For BKA cache we store condition to special BKA cache field
        because evaluation of the condition requires additional operations
        before the evaluation. This condition is used in 
        JOIN_CACHE_BKA[_UNIQUE]::skip_index_tuple() functions.
      */
      if (tab->use_join_cache &&
          /*
            if cache is used then the value is TRUE only 
            for BKA[_UNIQUE] cache (see check_join_cache_usage func).
          */
          tab->icp_other_tables_ok &&
          (idx_cond->used_tables() &
           ~(tab->table->map | tab->join->const_table_map)))
        tab->cache_idx_cond= idx_cond;
      else
      {
        idx_remainder_cond= tab->table->file->idx_cond_push(keyno, idx_cond);

        /*
          If (1) there is an index condition that we couldn't push using ICP,
             (2) we are using Join Buffering
             (3) and we are using BKA
          then use BKA's Index Condition Pushdown mechanism to check it.
        */
        if (idx_remainder_cond && tab->use_join_cache &&   // (1) && (2)
            tab->icp_other_tables_ok)                      // (3)
        {
          tab->cache_idx_cond= idx_remainder_cond;
          idx_remainder_cond= NULL;
        }
      }

      /*
        Disable eq_ref's "lookup cache" if we've pushed down an index
        condition. 
        TODO: This check happens to work on current ICP implementations, but
        there may exist a compliant implementation that will not work 
        correctly with it. Sort this out when we stabilize the condition
        pushdown APIs.
      */
      if (idx_remainder_cond != idx_cond)
        tab->ref.disable_cache= TRUE;

      Item *row_cond= tab->idx_cond_fact_out ? 
                        make_cond_remainder(tab->join->thd, tab->select_cond,
                                            tab->table, keyno,
			                    tab->icp_other_tables_ok, TRUE) :
	                tab->pre_idx_push_select_cond;

      DBUG_EXECUTE("where",
                   print_where(row_cond, "remainder cond", QT_ORDINARY););
      
      if (row_cond)
      {
        if (!idx_remainder_cond)
          tab->select_cond= row_cond;
        else
        {
          COND *new_cond= new (tab->join->thd->mem_root)
            Item_cond_and(tab->join->thd, row_cond, idx_remainder_cond);
          tab->select_cond= new_cond;
	  tab->select_cond->quick_fix_field();
          ((Item_cond_and*)tab->select_cond)->used_tables_cache= 
            row_cond->used_tables() | idx_remainder_cond->used_tables();
        }
      }
      else
        tab->select_cond= idx_remainder_cond;
      if (tab->select)
      {
        DBUG_EXECUTE("where",
                     print_where(tab->select->cond,
                                 "select_cond",
                                 QT_ORDINARY););

        tab->select->cond= tab->select_cond;
        tab->select->pre_idx_push_select_cond= tab->pre_idx_push_select_cond;
      }
    }
  }
  DBUG_VOID_RETURN;
}


