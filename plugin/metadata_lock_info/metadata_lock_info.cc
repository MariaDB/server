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
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

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
  { C_STRING_WITH_LEN("MDL_SHARED_NO_WRITE") },
  { C_STRING_WITH_LEN("MDL_SHARED_NO_READ_WRITE") },
  { C_STRING_WITH_LEN("MDL_EXCLUSIVE") },
};

static const LEX_STRING metadata_lock_info_duration[] = {
  { C_STRING_WITH_LEN("MDL_STATEMENT") },
  { C_STRING_WITH_LEN("MDL_TRANSACTION") },
  { C_STRING_WITH_LEN("MDL_EXPLICIT") },
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


class Ticket_info: public Apc_target::Apc_call
{
public:
  bool timed_out;
  THD *request_thd;
  THD *target_thd;
  TABLE *table;
  int error;

  Ticket_info(THD *request_thd_arg, TABLE *table_arg):
    request_thd(request_thd_arg), table(table_arg), error(0)
  { }

  void call_in_target_thread()
  {
    MDL_ticket *ticket;
    int i;

    for (i= 0; i < MDL_DURATION_END; i++)
    {
      MDL_context::Ticket_iterator it(request_thd->mdl_context.m_tickets[i]);
      while ((ticket= it++))
      {
        MDL_key *key= ticket->get_key();

        table->field[0]->store((longlong) target_thd->thread_id, TRUE);

        table->field[1]->set_notnull();
        table->field[1]->store(
          metadata_lock_info_lock_mode[(int) ticket->get_type()].str,
          metadata_lock_info_lock_mode[(int) ticket->get_type()].length,
          system_charset_info);

        table->field[2]->set_notnull();
        table->field[2]->store(
          metadata_lock_info_duration[i].str,
          metadata_lock_info_duration[i].length,
          system_charset_info);

        table->field[3]->set_notnull();
        table->field[3]->store(
          metadata_lock_info_lock_name[(int) key->mdl_namespace()].str,
          metadata_lock_info_lock_name[(int) key->mdl_namespace()].length,
          system_charset_info);

        table->field[4]->set_notnull();
        table->field[4]->store(key->db_name(), key->db_name_length(),
                               system_charset_info);

        table->field[5]->set_notnull();
        table->field[5]->store(key->name(), key->name_length(),
                               system_charset_info);

        if ((error= schema_table_store_record(request_thd, table)))
          return;
      }
    }
  }
};


static THD *find_thread(my_thread_id id)
{
  THD *tmp;

  mysql_mutex_lock(&LOCK_thread_count);
  I_List_iterator<THD> it(threads);
  while ((tmp= it++))
  {
    if (id == tmp->thread_id)
    {
      mysql_mutex_lock(&tmp->LOCK_thd_data);
      break;
    }
  }
  mysql_mutex_unlock(&LOCK_thread_count);
  return tmp;
}


static int i_s_metadata_lock_info_fill_table(THD *thd, TABLE_LIST *tables,
                                             COND *cond)
{
  Ticket_info info(thd, tables->table);
  DYNAMIC_ARRAY ids;
  THD *tmp;
  uint i;
  DBUG_ENTER("i_s_metadata_lock_info_fill_table");

  /* Gather thread identifiers */
  my_init_dynamic_array(&ids, sizeof(my_thread_id), 512, 1, MYF(0));
  mysql_mutex_lock(&LOCK_thread_count);
  I_List_iterator<THD> it(threads);
  while ((tmp= it++))
    if (tmp != thd && (info.error= insert_dynamic(&ids, &tmp->thread_id)))
      break;
  mysql_mutex_unlock(&LOCK_thread_count);

  /* Let foreign threads fill info */
  for (i= 0; i < ids.elements && info.error == 0; i++)
    if ((info.target_thd= find_thread(*dynamic_element(&ids, i, my_thread_id*))))
      info.target_thd->apc_target.make_apc_call(thd, &info, INT_MAX,
                                                &info.timed_out);

  delete_dynamic(&ids);
  if (info.error == 0)
  {
    info.target_thd= thd;
    info.call_in_target_thread();
  }
  DBUG_RETURN(info.error);
}

static int i_s_metadata_lock_info_init(
  void *p
) {
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
  MariaDB_PLUGIN_MATURITY_GAMMA,
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
