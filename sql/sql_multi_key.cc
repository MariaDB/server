/*
   Copyright (c) 2026, MariaDB plc

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include "mariadb.h"
#include "key.h"
#include "sql_select.h"
#include "sql_multi_key.h"


class Multi_key_param_rewriter :public Field_enumerator
{
  const Copy_field *copy_field, *copy_field_end;
public:
  Multi_key_param_rewriter(const Copy_field *begin, const Copy_field *end)
    :copy_field(begin), copy_field_end(end) {}
  void visit_field(Item_field *item) override
  {
    for (const Copy_field *copy= copy_field; copy < copy_field_end; copy++)
    {
      if (item->field == copy->from_field)
      {
        item->reset_field(copy->to_field);
        return;
      }
    }
  }
};


bool Multi_key_params::init(THD *thd, JOIN_TAB *tab, Item **cond)
{
  TABLE_REF *ref= &tab->ref;
  List<Item_field> fields;
  List_iterator<Item_field> it(fields);
  Item_field *item;

  for (uint i= 0; i < ref->key_parts; i++)
  {
    if (ref->items[i]->maybe_null() &&
        !(ref->null_rejecting & ((key_part_map) 1 << i)))
      return true;
  }

  (*cond)->walk(&Item::collect_item_field_processor, &fields, 0);
  /* A push_back out of memory leaves a NULL link that the next scan of the
     list dereferences; the fault point raises the error such a push_back
     sets when no scan follows it */
  DBUG_EXECUTE_IF("multi_key_collect_simulate_oom",
                  thd->get_stmt_da()->set_error_status(ER_OUT_OF_RESOURCES););
  if (thd->is_error())
    return true;
  handlerton *ht= tab->table->file->partition_ht();
  bool other_engine= false;
  while ((item= it++))
  {
    Field *field= item->field;
    if (!(item->used_tables() & ~(tab->table->map | PSEUDO_TABLE_BITS)))
      it.remove();
    /* create_tmp_field() copies a BLOB without the Field_blob constructor
       that counts it for add(), and param_table is never destructed;
       open() puts an uneven BIT's bits in null bytes sized without them */
    else if ((field->flags & BLOB_FLAG) ||
             (field->type() == MYSQL_TYPE_BIT && ((Field_bit*) field)->bit_len))
      return true;
    else if (field->table->file->partition_ht() != ht)
      other_engine= true;
  }
  /* TABLE::move_fields() needs a field to move */
  if (!fields.elements)
    return true;
  /* Values that all come from the inner table's engine were fetched from it
     already: plain BKA reads the keys in one MRR request, while this path
     sends one statement per tuple */
  if (!other_engine)
    return true;

  join_tab= tab;
  params= fields.elements;
  DBUG_EXECUTE_IF("multi_key_param_table_simulate_oom",
                  DBUG_SET("+d,simulate_out_of_memory"););
  if (!(param_table= new (thd) Virtual_tmp_table(thd)) ||
      param_table->init(params))
    return true;
  for (it.rewind(); (item= it++); )
  {
    DBUG_EXECUTE_IF("multi_key_param_field_simulate_oom",
                    DBUG_SET("+d,simulate_out_of_memory"););
    Field *copy= item->field->create_tmp_field(thd->mem_root, param_table,
                                               item->maybe_null(), nullptr);
    if (!copy || param_table->add(copy))
      return true;
    /* Item_field::used_tables() reads the table map: the rewritten
       references stay non-constant */
    param_table->map|= item->field->table->map;
  }
  DBUG_EXECUTE_IF("multi_key_param_open_simulate_oom",
                  DBUG_SET("+d,simulate_out_of_memory"););
  if (param_table->open() ||
      !(copy_field= new (thd->mem_root) Copy_field[params]))
    return true;

  param= param_table->field;
  base= param_table->record[0];
  rec_length= param_table->s->reclength + ref->key_length;
  it.rewind();
  for (uint k= 0; k < params; k++)
    copy_field[k].set(param[k], it++->field, false);

  Multi_key_param_rewriter rewriter(copy_field, copy_field + params);
  (*cond)->walk(&Item::enumerate_field_refs_processor, &rewriter, 0);
  /* deep_copy() leaves each Arg_comparator on the original's arguments: fixed
     again, the copy compares the rewritten ones */
  (*cond)->walk(&Item::cleanup_excluding_fields_processor, 0, 0);
  return (*cond)->fix_fields(thd, cond);
}


bool Multi_key_params::pack(uchar *record)
{
  TABLE_REF *ref= &join_tab->ref;
  uchar *tuple= param_table->record[0];
  uint length= param_table->s->reclength;

  if (cp_buffer_from_ref(join_tab->join->thd, join_tab->table, ref))
    return true;
  /* do_copy() resets a NULL copy where the copy points, and writes only the
     used bytes of a VARCHAR, while cmp() compares every byte */
  move_to(tuple);
  memset(tuple, 0, length);
  for (Copy_field *copy= copy_field; copy < copy_field + params; copy++)
    copy->do_copy(copy);
  memcpy(record, tuple, length);
  memcpy(record + length, ref->key_buff, ref->key_length);
  return false;
}


void Multi_key_params::move_to(uchar *record)
{
  param_table->move_fields(param, record, base);
  base= record;
}


int Multi_key_params::cmp_key(const uchar *a, const uchar *b) const
{
  uint key_offset= param_table->s->reclength;
  return key_tuple_cmp(join_tab->table->key_info[join_tab->ref.key].key_part,
                       a + key_offset, b + key_offset,
                       join_tab->ref.key_length);
}


int Multi_key_params::cmp(const uchar *a, const uchar *b) const
{
  return memcmp(a, b, param_table->s->reclength);
}
