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

#define MYSQL_SERVER 1
#include "mariadb.h"
#include "sql_class.h"
#include "key.h"
#include "../heap/ha_heap.h"

static MYSQL_THDVAR_ULONGLONG(prepares, PLUGIN_VAR_NOCMDOPT,
  "Calls of index_multi_key_read_with_pushdown_prepare(), accepted or refused",
  NULL, NULL, 0, 0, ULONGLONG_MAX, 0);

static MYSQL_THDVAR_ULONGLONG(calls, PLUGIN_VAR_NOCMDOPT,
  "Calls of index_multi_key_read_params()",
  NULL, NULL, 0, 0, ULONGLONG_MAX, 0);

static MYSQL_THDVAR_ULONGLONG(tuples, PLUGIN_VAR_NOCMDOPT,
  "Tuples the calls of index_multi_key_read_params() carried",
  NULL, NULL, 0, 0, ULONGLONG_MAX, 0);

static MYSQL_THDVAR_ULONGLONG(rows, PLUGIN_VAR_NOCMDOPT,
  "Rows index_multi_key_read() returned",
  NULL, NULL, 0, 0, ULONGLONG_MAX, 0);

static struct st_mysql_sys_var *sysvars[]=
{
  MYSQL_SYSVAR(prepares),
  MYSQL_SYSVAR(calls),
  MYSQL_SYSVAR(tuples),
  MYSQL_SYSVAR(rows),
  NULL
};


class test_ha_multi_key: public ha_heap
{
  uint mk_index;
  Field **mk_param;
  uint mk_params;
  Item *mk_pushdown;
  Item *mk_value[MAX_REF_PARTS];
  uint mk_parts, mk_key_length;
  /* read() returns the rows after params(), when the copies no longer point
     at their tuples: each is its tuple's offset, then its record */
  String mk_rows;
  size_t mk_next;

  void bind(KEY *key, Item *cond);
  int read_tuple(uint32 offset);

public:
  test_ha_multi_key(handlerton *hton, TABLE_SHARE *share)
    :ha_heap(hton, share), mk_pushdown(0), mk_next(0) {}

  ulonglong table_flags() const override
  { return ha_heap::table_flags() | HA_CAN_MULTI_KEY_READ; }

  ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                uint key_parts, uint *bufsz, uint *mrr_mode,
                                Cost_estimate *cost) override
  {
    ha_rows rows= handler::multi_range_read_info(keyno, n_ranges, keys,
                                                 key_parts, bufsz, mrr_mode,
                                                 cost);
    /* BKA, which the multi-key read replaces, needs an MRR other than the
       default; the default one serves BKA when the path is refused or off */
    *mrr_mode&= ~HA_MRR_USE_DEFAULT_IMPL;
    return rows;
  }

  int index_multi_key_read_with_pushdown_prepare(uint index, Field **param,
                                                 uint params,
                                                 Item *pushdown) override;
  int index_multi_key_read_params(uchar **row_data) override;
  int index_multi_key_read(uchar *record, uint32 *offset) override;

  int reset() override
  {
    mk_pushdown= 0;
    mk_rows.free();
    mk_next= 0;
    return ha_heap::reset();
  }
};


/* The server's own test for ref access: the lookup by the value converted to
   the key part then misses no row the = admits */
void test_ha_multi_key::bind(KEY *key, Item *cond)
{
  if (cond->type() != Item::FUNC_ITEM ||
      ((Item_func *) cond)->functype() != Item_func::EQ_FUNC)
    return;
  Item_func_eq *eq= (Item_func_eq *) cond;
  for (uint side= 0; side < 2; side++)
  {
    Item *field= eq->arguments()[side]->real_item();
    Item *value= eq->arguments()[1 - side];
    if (field->type() != Item::FIELD_ITEM ||
        (value->used_tables() & (table->map | RAND_TABLE_BIT)))
      continue;
    for (uint part= 0; part < key->user_defined_key_parts; part++)
    {
      Field *key_field= key->key_part[part].field;
      if (!mk_value[part] && key_field->eq(((Item_field *) field)->field) &&
          key_field->can_optimize_keypart_ref(eq, value) ==
            Data_type_compatibility::OK)
        mk_value[part]= value;
    }
  }
}


int test_ha_multi_key::index_multi_key_read_with_pushdown_prepare(
  uint index, Field **param, uint params, Item *pushdown)
{
  KEY *key= table->key_info + index;

  THDVAR(ha_thd(), prepares)++;
  mk_pushdown= 0;
  bzero(mk_value, sizeof(mk_value));
  if (is_cond_and(pushdown))
  {
    List_iterator_fast<Item> li(*((Item_cond *) pushdown)->argument_list());
    while (Item *item= li++)
      bind(key, item);
  }
  else
    bind(key, pushdown);
  for (mk_parts= mk_key_length= 0;
       mk_parts < key->user_defined_key_parts && mk_value[mk_parts];
       mk_parts++)
    mk_key_length+= key->key_part[mk_parts].store_length;
  if (!mk_parts ||
      (mk_parts < key->user_defined_key_parts &&
       (index_flags(index, 0, 1) & HA_ONLY_WHOLE_INDEX)))
    return HA_ERR_UNSUPPORTED;
  mk_index= index;
  mk_param= param;
  mk_params= params;
  mk_pushdown= pushdown;
  return 0;
}


int test_ha_multi_key::read_tuple(uint32 offset)
{
  THD *thd= ha_thd();
  KEY *key= table->key_info + mk_index;
  uchar key_buff[MAX_KEY_LENGTH];
  int error;

  {
    /* As in store_key::copy(): a value its key part cannot hold is clipped
       with no warning, which INSERT ... SELECT would turn into an error */
    Check_level_instant_set check_level(thd, CHECK_FIELD_IGNORE);
    MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->write_set);
    for (uint part= 0; part < mk_parts; part++)
      mk_value[part]->save_in_field(key->key_part[part].field, true);
    dbug_tmp_restore_column_map(&table->write_set, old_map);
  }
  key_copy(key_buff, table->record[0], key, mk_key_length);
  for (error= index_read_map(table->record[0], key_buff,
                             make_prev_keypart_map(mk_parts),
                             HA_READ_KEY_EXACT);
       !error;
       error= index_next_same(table->record[0], key_buff, mk_key_length))
  {
    uchar slot[4];
    if (!mk_pushdown->val_int())
      continue;
    int4store(slot, offset);
    if (mk_rows.append((const char *) slot, sizeof(slot)) ||
        mk_rows.append((const char *) table->record[0], table->s->reclength))
      return HA_ERR_OUT_OF_MEM;
  }
  return error == HA_ERR_KEY_NOT_FOUND || error == HA_ERR_END_OF_FILE ?
         0 : error;
}


int test_ha_multi_key::index_multi_key_read_params(uchar **row_data)
{
  THD *thd= ha_thd();
  int error= 0;

  DBUG_ASSERT(mk_pushdown);
  DBUG_ASSERT(active_index == mk_index);
  THDVAR(thd, calls)++;
  mk_rows.length(0);
  mk_next= 0;
  for (uchar **row= row_data; *row && !error; row++)
  {
    my_ptrdiff_t diff= *row - row_data[0];
    THDVAR(thd, tuples)++;
    for (uint k= 0; k < mk_params; k++)
      mk_param[k]->move_field_offset(diff);
    error= read_tuple((uint32) (row - row_data));
    for (uint k= 0; k < mk_params; k++)
      mk_param[k]->move_field_offset(-diff);
  }
  return error;
}


int test_ha_multi_key::index_multi_key_read(uchar *record, uint32 *offset)
{
  if (mk_next == mk_rows.length())
  {
    table->status= STATUS_NOT_FOUND;
    return HA_ERR_END_OF_FILE;
  }
  const uchar *slot= (const uchar *) mk_rows.ptr() + mk_next;
  *offset= uint4korr(slot);
  memcpy(record, slot + 4, table->s->reclength);
  mk_next+= 4 + table->s->reclength;
  THDVAR(ha_thd(), rows)++;
  table->status= 0;
  return 0;
}


static handler *create_handler(handlerton *hton, TABLE_SHARE *table,
                               MEM_ROOT *mem_root)
{
  return new (mem_root) test_ha_multi_key(hton, table);
}


static int init(void *p)
{
  ((handlerton *) p)->create= create_handler;
  return 0;
}


static struct st_mysql_storage_engine descriptor=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(test_multi_key)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &descriptor,
  "TEST_MULTI_KEY",
  "MariaDB plc",
  "MEMORY tables that serve the multi-key read, to test the server's side",
  PLUGIN_LICENSE_GPL,
  init,
  NULL,
  0x0100,
  NULL,
  sysvars,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
