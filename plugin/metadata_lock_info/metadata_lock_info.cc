/* Copyright (C) 2013 Kentoku Shiba

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

#define MYSQL_SERVER 1
#include "my_config.h"
#include "mysql_version.h"
#include "mysql/plugin.h"
#include "sql_class.h"
#include "sql_show.h"

static const LEX_STRING metadata_lock_info_lock_name[] = {
  { C_STRING_WITH_LEN("Global read lock") },
  { C_STRING_WITH_LEN("Schema metadata lock") },
  { C_STRING_WITH_LEN("Table metadata lock") },
  { C_STRING_WITH_LEN("Stored function metadata lock") },
  { C_STRING_WITH_LEN("Stored procedure metadata lock") },
  { C_STRING_WITH_LEN("Trigger metadata lock") },
  { C_STRING_WITH_LEN("Event metadata lock") },
  { C_STRING_WITH_LEN("Commit lock") },
  { C_STRING_WITH_LEN("User lock") },
};

static const LEX_STRING metadata_lock_info_lock_mode[] = {
  { C_STRING_WITH_LEN("MDL_INTENTION_EXCLUSIVE") },
  { C_STRING_WITH_LEN("MDL_SHARED") },
  { C_STRING_WITH_LEN("MDL_SHARED_HIGH_PRIO") },
  { C_STRING_WITH_LEN("MDL_SHARED_READ") },
  { C_STRING_WITH_LEN("MDL_SHARED_WRITE") },
  { C_STRING_WITH_LEN("MDL_SHARED_UPGRADABLE") },
  { C_STRING_WITH_LEN("MDL_SHARED_READ_ONLY") },
  { C_STRING_WITH_LEN("MDL_SHARED_NO_WRITE") },
  { C_STRING_WITH_LEN("MDL_SHARED_NO_READ_WRITE") },
  { C_STRING_WITH_LEN("MDL_EXCLUSIVE") },
};

static ST_FIELD_INFO i_s_metadata_lock_info_fields_info[] =
{
  {"THREAD_ID", 20, MYSQL_TYPE_LONGLONG, 0,
    MY_I_S_UNSIGNED, "thread_id", SKIP_OPEN_TABLE},
  {"LOCK_MODE", 24, MYSQL_TYPE_STRING, 0,
    MY_I_S_MAYBE_NULL, "lock_mode", SKIP_OPEN_TABLE},
  {"LOCK_DURATION", 30, MYSQL_TYPE_STRING, 0,
    MY_I_S_MAYBE_NULL, "lock_duration", SKIP_OPEN_TABLE},
  {"LOCK_TYPE", 30, MYSQL_TYPE_STRING, 0,
    MY_I_S_MAYBE_NULL, "lock_type", SKIP_OPEN_TABLE},
  {"TABLE_SCHEMA", 64, MYSQL_TYPE_STRING, 0,
    MY_I_S_MAYBE_NULL, "table_schema", SKIP_OPEN_TABLE},
  {"TABLE_NAME", 64, MYSQL_TYPE_STRING, 0,
    MY_I_S_MAYBE_NULL, "table_name", SKIP_OPEN_TABLE},
  {NULL, 0,  MYSQL_TYPE_STRING, 0, 0, NULL, 0}
};

struct st_i_s_metadata_param
{
  THD   *thd;
  TABLE *table;
};

int i_s_metadata_lock_info_fill_row(
  MDL_ticket *mdl_ticket,
  void *arg
) {
  st_i_s_metadata_param *param = (st_i_s_metadata_param *) arg;
  THD *thd = param->thd;
  TABLE *table = param->table;
  DBUG_ENTER("i_s_metadata_lock_info_fill_row");
  MDL_context *mdl_ctx = mdl_ticket->get_ctx();
  enum_mdl_type mdl_ticket_type = mdl_ticket->get_type();
  MDL_key *mdl_key = mdl_ticket->get_key();
  MDL_key::enum_mdl_namespace mdl_namespace = mdl_key->mdl_namespace();
  table->field[0]->store((longlong) mdl_ctx->get_thread_id(), TRUE);
  table->field[1]->set_notnull();
  table->field[1]->store(
    metadata_lock_info_lock_mode[(int) mdl_ticket_type].str,
    metadata_lock_info_lock_mode[(int) mdl_ticket_type].length,
    system_charset_info);
  table->field[2]->set_null();
  table->field[3]->set_notnull();
  table->field[3]->store(
    metadata_lock_info_lock_name[(int) mdl_namespace].str,
    metadata_lock_info_lock_name[(int) mdl_namespace].length,
    system_charset_info);
  table->field[4]->set_notnull();
  table->field[4]->store(mdl_key->db_name(),
    mdl_key->db_name_length(), system_charset_info);
  table->field[5]->set_notnull();
  table->field[5]->store(mdl_key->name(),
    mdl_key->name_length(), system_charset_info);
  if (schema_table_store_record(thd, table))
    DBUG_RETURN(1);
  DBUG_RETURN(0);
}

int i_s_metadata_lock_info_fill_table(
  THD *thd,
  TABLE_LIST *tables,
  COND *cond
) {
  st_i_s_metadata_param param;
  DBUG_ENTER("i_s_metadata_lock_info_fill_table");
  param.table = tables->table;
  param.thd = thd;
  DBUG_RETURN(mdl_iterate(i_s_metadata_lock_info_fill_row, &param));
}

static int i_s_metadata_lock_info_init(
  void *p
) {

  compile_time_assert(sizeof(metadata_lock_info_lock_name)/sizeof(LEX_STRING)
                      == MDL_key::NAMESPACE_END);
  compile_time_assert(sizeof(metadata_lock_info_lock_mode)/sizeof(LEX_STRING)
                      == MDL_TYPE_END);

  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *) p;
  DBUG_ENTER("i_s_metadata_lock_info_init");
  schema->fields_info = i_s_metadata_lock_info_fields_info;
  schema->fill_table = i_s_metadata_lock_info_fill_table;
  schema->idx_field1 = 0;
  DBUG_RETURN(0);
}

static int i_s_metadata_lock_info_deinit(
  void *p
) {
  DBUG_ENTER("i_s_metadata_lock_info_deinit");
  DBUG_RETURN(0);
}

static struct st_mysql_information_schema i_s_metadata_lock_info_plugin =
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

#ifdef MARIADB_BASE_VERSION
maria_declare_plugin(metadata_lock_info)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &i_s_metadata_lock_info_plugin,
  "METADATA_LOCK_INFO",
  "Kentoku Shiba",
  "Metadata locking viewer",
  PLUGIN_LICENSE_GPL,
  i_s_metadata_lock_info_init,
  i_s_metadata_lock_info_deinit,
  0x0001,
  NULL,
  NULL,
  NULL,
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
#else
mysql_declare_plugin(metadata_lock_info)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &i_s_metadata_lock_info_plugin,
  "METADATA_LOCK_INFO",
  "Kentoku Shiba",
  "Metadata locking viewer",
  PLUGIN_LICENSE_GPL,
  i_s_metadata_lock_info_init,
  i_s_metadata_lock_info_deinit,
  0x0001,
  NULL,
  NULL,
  NULL,
#if MYSQL_VERSION_ID >= 50600
  0,
#endif
}
mysql_declare_plugin_end;
#endif
