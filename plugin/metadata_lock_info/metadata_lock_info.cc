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
#include <my_global.h>
#include "mysql_version.h"
#include "mysql/plugin.h"
#include "sql_class.h"
#include "sql_i_s.h"

static const LEX_STRING metadata_lock_info_lock_name[] = {
  { C_STRING_WITH_LEN("Backup lock") },
  { C_STRING_WITH_LEN("Schema metadata lock") },
  { C_STRING_WITH_LEN("Table metadata lock") },
  { C_STRING_WITH_LEN("Stored function metadata lock") },
  { C_STRING_WITH_LEN("Stored procedure metadata lock") },
  { C_STRING_WITH_LEN("Stored package body metadata lock") },
  { C_STRING_WITH_LEN("Trigger metadata lock") },
  { C_STRING_WITH_LEN("Event metadata lock") },
  { C_STRING_WITH_LEN("User lock") },
};

namespace Show {

static ST_FIELD_INFO i_s_metadata_lock_info_fields_info[] =
{
  Column("THREAD_ID",     ULonglong(20), NOT_NULL, "thread_id"),
  Column("LOCK_MODE",     Varchar(24),   NULLABLE, "lock_mode"),
  Column("LOCK_DURATION", Varchar(30),   NULLABLE, "lock_duration"),
  Column("LOCK_TYPE",     Varchar(33),   NULLABLE, "lock_type"),
  Column("TABLE_SCHEMA",  Name(),        NULLABLE, "table_schema"),
  Column("TABLE_NAME",    Name(),        NULLABLE, "table_name"),
  CEnd()
};

} // namespace Show


struct st_i_s_metadata_param
{
  THD   *thd;
  TABLE *table;
};

int i_s_metadata_lock_info_fill_row(
  MDL_ticket *mdl_ticket,
  void *arg,
  bool granted
) {
  st_i_s_metadata_param *param = (st_i_s_metadata_param *) arg;
  THD *thd = param->thd;
  TABLE *table = param->table;
  DBUG_ENTER("i_s_metadata_lock_info_fill_row");
  MDL_context *mdl_ctx = mdl_ticket->get_ctx();
  MDL_key *mdl_key = mdl_ticket->get_key();
  MDL_key::enum_mdl_namespace mdl_namespace = mdl_key->mdl_namespace();
  if (!granted)
    DBUG_RETURN(0);
  table->field[0]->store((longlong) mdl_ctx->get_thread_id(), TRUE);
  table->field[1]->set_notnull();
  table->field[1]->store(mdl_ticket->get_type_name(), system_charset_info);
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

  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *) p;
  DBUG_ENTER("i_s_metadata_lock_info_init");
  schema->fields_info = Show::i_s_metadata_lock_info_fields_info;
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
