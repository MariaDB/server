/* Copyright (C) 2014 MariaDB Corporation.

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

#ifndef MYSQL_SERVER
#define MYSQL_SERVER
#endif

#include <my_global.h>
#include <mysql/plugin.h>
#include <sql_i_s.h>                            /* ST_SCHEMA_TABLE */
#include <sql_show.h>
#include <sql_acl.h>                            /* check_global_access() */
#include <wsrep_mysqld.h>
#include <wsrep_utils.h>

/* WSREP_MEMBERSHIP table fields */

/* Node index */
#define COLUMN_WSREP_MEMB_INDEX 0
/* Unique member ID */
#define COLUMN_WSREP_MEMB_UUID 1
/* Human-readable name */
#define COLUMN_WSREP_MEMB_NAME 2
/* Incoming address */
#define COLUMN_WSREP_MEMB_ADDRESS 3

/* WSREP_STATUS table fields */

/* Node index */
#define COLUMN_WSREP_STATUS_NODE_INDEX 0
/* Node status */
#define COLUMN_WSREP_STATUS_NODE_STATUS 1
/* Cluster status */
#define COLUMN_WSREP_STATUS_CLUSTER_STATUS 2
/* Cluster size */
#define COLUMN_WSREP_STATUS_CLUSTER_SIZE 3
/* Global cluster state UUID */
#define COLUMN_WSREP_STATUS_CLUSTER_STATE_UUID 4
/* Global cluster state Sequence number */
#define COLUMN_WSREP_STATUS_CLUSTER_STATE_SEQNO 5
/* Cluster membership changes */
#define COLUMN_WSREP_STATUS_CLUSTER_CONF_ID 6
/* Application protocol version */
#define COLUMN_WSREP_STATUS_PROTO_VERSION 7

namespace Show {

static ST_FIELD_INFO wsrep_memb_fields[]=
{
  Column("INDEX",   SLong(),                        NOT_NULL, "Index"),
  Column("UUID",    Varchar(WSREP_UUID_STR_LEN),    NOT_NULL, "Uuid"),
  Column("NAME",    Varchar(WSREP_MEMBER_NAME_LEN), NOT_NULL, "Name"),
  Column("ADDRESS", Varchar(WSREP_INCOMING_LEN),    NOT_NULL, "Address"),
  CEnd()
};

static ST_FIELD_INFO wsrep_status_fields[]=
{
  Column("NODE_INDEX",          SLong(),     NOT_NULL, "Node_Index"),
  Column("NODE_STATUS",         Varchar(16), NOT_NULL, "Node_Status"),
  Column("CLUSTER_STATUS",      Varchar(16), NOT_NULL, "Cluster_Status"),
  Column("CLUSTER_SIZE",        SLong(),     NOT_NULL, "Cluster_Size"),
  Column("CLUSTER_STATE_UUID",  Varchar(WSREP_UUID_STR_LEN), NOT_NULL),
  Column("CLUSTER_STATE_SEQNO", SLonglong(), NOT_NULL),
  Column("CLUSTER_CONF_ID",     SLonglong(), NOT_NULL),
  Column("PROTOCOL_VERSION",    SLong(),     NOT_NULL),
  CEnd()
};

} // namespace Show

static int wsrep_memb_fill_table(THD *thd, TABLE_LIST *tables, COND *cond)
{
  int rc= 0;

  if (check_global_access(thd, SUPER_ACL, true))
    return rc;

  wsrep_config_state->lock();

  const wsrep::view& view(wsrep_config_state->get_view_info());
  const std::vector<wsrep::view::member>& members(view.members());


  TABLE *table= tables->table;

  for (unsigned int i= 0; i < members.size(); i++)
  {
    table->field[COLUMN_WSREP_MEMB_INDEX]->store(i, 0);

    std::ostringstream os;
    os << members[i].id();
    table->field[COLUMN_WSREP_MEMB_UUID]->store(os.str().c_str(),
                                                os.str().length(),
                                                system_charset_info);
    table->field[COLUMN_WSREP_MEMB_NAME]->store(members[i].name().c_str(),
                                                members[i].name().length(),
                                                system_charset_info);
    table->field[COLUMN_WSREP_MEMB_ADDRESS]->store(members[i].incoming().c_str(),
                                                   members[i].incoming().length(),
                                                   system_charset_info);

    if (schema_table_store_record(thd, table))
    {
      rc= 1;
      goto end;
    }
  }

end:
  wsrep_config_state->unlock();
  return rc;
}

static int wsrep_memb_plugin_init(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;

  schema->fields_info= Show::wsrep_memb_fields;
  schema->fill_table= wsrep_memb_fill_table;

  return 0;
}

static struct st_mysql_information_schema wsrep_memb_plugin=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

static int wsrep_status_fill_table(THD *thd, TABLE_LIST *tables, COND *cond)
{
  int rc= 0;

  if (check_global_access(thd, SUPER_ACL, true))
    return rc;

  wsrep_config_state->lock();

  const wsrep::view& view= wsrep_config_state->get_view_info();
  enum wsrep::server_state::state status= wsrep_config_state->get_status();

  TABLE *table= tables->table;

  table->field[COLUMN_WSREP_STATUS_NODE_INDEX]
    ->store(view.own_index(), 0);
  table->field[COLUMN_WSREP_STATUS_NODE_STATUS]
    ->store(to_c_string(status),
            strlen(to_c_string(status)),
            system_charset_info);
  table->field[COLUMN_WSREP_STATUS_CLUSTER_STATUS]
    ->store(to_c_string(view.status()),
            strlen(to_c_string(view.status())),
            system_charset_info);
  table->field[COLUMN_WSREP_STATUS_CLUSTER_SIZE]->store(view.members().size(), 0);

  std::ostringstream os;
  os << view.state_id().id();
  table->field[COLUMN_WSREP_STATUS_CLUSTER_STATE_UUID]
    ->store(os.str().c_str(), os.str().length(), system_charset_info);

  table->field[COLUMN_WSREP_STATUS_CLUSTER_STATE_SEQNO]
    ->store(view.state_id().seqno().get(), 0);
  table->field[COLUMN_WSREP_STATUS_CLUSTER_CONF_ID]
    ->store(view.view_seqno().get(), 0);
  table->field[COLUMN_WSREP_STATUS_PROTO_VERSION]
    ->store(view.protocol_version(), 0);

  if (schema_table_store_record(thd, table))
    rc= 1;

  wsrep_config_state->unlock();
  return rc;
}

static int wsrep_status_plugin_init(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;

  schema->fields_info= Show::wsrep_status_fields;
  schema->fill_table= wsrep_status_fill_table;

  return 0;
}

static struct st_mysql_information_schema wsrep_status_plugin=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

/*
  Plugin library descriptor
*/

maria_declare_plugin(wsrep_info)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &wsrep_memb_plugin,
  "WSREP_MEMBERSHIP",                           /* Plugin name        */
  "Nirbhay Choubey",                            /* Plugin author      */
  "Information about group members",            /* Plugin description */
  PLUGIN_LICENSE_GPL,                           /* License            */
  wsrep_memb_plugin_init,                       /* Plugin Init        */
  0,                                            /* Plugin Deinit      */
  0x0100,                                       /* Version (hex)      */
  NULL,                                         /* Status variables   */
  NULL,                                         /* System variables   */
  "1.0",                                        /* Version (string)   */
  MariaDB_PLUGIN_MATURITY_STABLE                /* Maturity           */
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &wsrep_status_plugin,
  "WSREP_STATUS",                               /* Plugin name        */
  "Nirbhay Choubey",                            /* Plugin author      */
  "Group view information",                     /* Plugin description */
  PLUGIN_LICENSE_GPL,                           /* License            */
  wsrep_status_plugin_init,                     /* Plugin Init        */
  0,                                            /* Plugin Deinit      */
  0x0100,                                       /* Version (hex)      */
  NULL,                                         /* Status variables   */
  NULL,                                         /* System variables   */
  "1.0",                                        /* Version (string)   */
  MariaDB_PLUGIN_MATURITY_STABLE                /* Maturity           */
}
maria_declare_plugin_end;

