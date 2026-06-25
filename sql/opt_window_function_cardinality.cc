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

/**
  @file

    Contains est_derived_window_fn_cardinality()
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_select.h"
#include "sql_statistics.h"
#include "item_windowfunc.h"
#include "opt_window_function_cardinality.h"


/*
  @brief
    Ignoring the selectivity of any join conditions, estimate the number of
    rows produced by the partition clause of a row_number window function
    in our derived table.  This is a simple multiplication of the estimate
    of the number of distinct values for each partition element, starting
    with 1 for zero elements.
*/

static ha_rows handle_single_part_rownumber( Item_window_func *item_row_number)
{
  if (item_row_number->window_spec->partition_list &&
      item_row_number->window_spec->partition_list->elements)
  {
    double records= 1;
    const uint max_tables= 16;
    TABLE *tables[max_tables];
    double table_distinct[max_tables];
    uint table_count= 0;
    ORDER *part_list=  item_row_number->window_spec->partition_list->first;
    for (; part_list; part_list= part_list->next)
    {
      if ((*part_list->item)->type() == Item::FIELD_ITEM)
      {
        Item_field *item_field= ((Item_field*)(*part_list->item));
        DBUG_ASSERT(item_field);
        Field *field= item_field->field;
        DBUG_ASSERT(field);
        TABLE *table= field->table;
        ha_rows table_records= table->used_stat_records;

        /*
          use EITS records per key if we have them,
          otherwise
          use number of records in the table (as a worst case scenario)
        */
        if (table_records == HA_POS_ERROR)
        {
          table_records= table->file->stats.records;
          if (table_records == HA_POS_ERROR)
            table_records= 1000; // Fallback default
        }

        double diff_values= 1.0;
        if (field->read_stats)
        {
          double freq= field->read_stats->get_avg_frequency();
          diff_values= freq > 0 ? (double)table_records / freq
                                : (double)table_records;
        }
        else
          diff_values= (double)table_records;

        uint idx= 0;
        for (; idx < table_count; idx++)
        {
          if (tables[idx] == table)
            break;
        }
        if (idx < max_tables)
        {
          if (idx == table_count)
          {
            tables[idx]= table;
            table_distinct[idx]= diff_values;
            table_count++;
          }
          else
            table_distinct[idx]*= diff_values;
          if (table_distinct[idx] > (double)table_records)
            table_distinct[idx]= (double)table_records;
        }
      }
    }

    for (uint i= 0; i < table_count; i++)
      records*= table_distinct[i];
    return records < 1.0 ? (ha_rows)1 : (ha_rows)records;
  }
  else
  {
    return (ha_rows)1;
  }
}


/*
  @brief
    Given a SELECT with a window function, estimate the number of output rows

  @detail
    Consider a derived table of the form

      SELECT ..., ROW_NUMBER ()  OVER (PARTITION BY c1,c2 order by ...)
      FROM t1, t2, t3 ...
      WHERE ...

    The optimizer can push outside predicates into this table and generate
    a temporary key on this materialized table.

    If a key generated on the derived table column of the row_number window
    function, we can infer row numbers by ignoring any join conditions and
    looking at the source of the data for columns c1, c2

    Currently we ignore the rest of the key parts.

  @param
    derived             the SELECT_LEX corresponding to our derived table
    out_records         pointer to out (maybe) written result.
    reg_fields          and array of field pointer to parts of the keys
    key_parts           the number of parts to the key

  @return
    true        if we have an estimate
    false       if no estimate is possible
*/

bool est_derived_window_fn_cardinality(st_select_lex* derived,
                                       ulong *out_records,
                                       Field **reg_fields,
                                       uint key_parts)
{
  uint fc= 0;

  while (fc < key_parts)
  {
    uint ic= 0;
    if (!reg_fields[fc])
    {
      fc++;
      continue;
    }
    uint field_index= reg_fields[fc]->field_index;
    List_iterator_fast<Item> li(*derived->get_item_list());
    Item *item;

    // find our window function in the item list
    while ((item= li++))
    {
      // if the single part key is on our window function
      if ((ic == field_index) && item->type() == Item::WINDOW_FUNC_ITEM)
      {
        Item_window_func *item_row_num= (Item_window_func*)item;

        if (item_row_num->window_func()->sum_func() == Item_sum::ROW_NUMBER_FUNC)
        {
          *out_records= (ulong)handle_single_part_rownumber(item_row_num);
          return true;
        }
      }
      ic++;
    }
    fc++;
  }

  return false;
}
