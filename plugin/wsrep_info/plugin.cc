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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef MYSQL_SERVER
#define MYSQL_SERVER
#endif

#include <my_config.h>
#include <mysql/plugin.h>
#include <table.h>                              /* ST_SCHEMA_TABLE */
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
/* Gap between global and local states ? */
#define COLUMN_WSREP_STATUS_GAP 7
/* Application protocol version */
#define COLUMN_WSREP_STATUS_PROTO_VERSION 8

static const char* get_member_status(wsrep_member_status_t status)
{
  switch (status)
  {
    case WSREP_MEMBER_UNDEFINED: return "Undefined";
    case WSREP_MEMBER_JOINER: return "Joiner";
    case WSREP_MEMBER_DONOR: return "Donor";
    case WSREP_MEMBER_JOINED: return "Joined";
    case WSREP_MEMBER_SYNCED: return "Synced";
    case WSREP_MEMBER_ERROR: return "Error";
    default: break;
  }
  return "UNKNOWN";
}

static const char* get_cluster_status(wsrep_view_status_t status)
{
  switch (status)
  {
    case WSREP_VIEW_PRIMARY: return "Primary";
    case WSREP_VIEW_NON_PRIMARY: return "Non-primary";
    case WSREP_VIEW_DISCONNECTED: return "Disconnected";
    default: break;
  }
  return "UNKNOWN";
}

static ST_FIELD_INFO wsrep_memb_fields[]=
{
  {"INDEX", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, "Index", 0},
  {"UUID", WSREP_UUID_STR_LEN, MYSQL_TYPE_STRING, 0, 0, "Uuid", 0},
  {"NAME", WSREP_MEMBER_NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "Name", 0},
  {"ADDRESS", WSREP_INCOMING_LEN, MYSQL_TYPE_STRING, 0, 0, "Address", 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}
};

static ST_FIELD_INFO wsrep_status_fields[]=
{
  {"NODE_INDEX", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG,
    0, 0, "Node_Index", 0},
  {"NODE_STATUS", 16, MYSQL_TYPE_STRING, 0, 0, "Node_Status", 0},
  {"CLUSTER_STATUS", 16, MYSQL_TYPE_STRING, 0, 0, "Cluster_Status", 0},
  {"CLUSTER_SIZE", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG,
    0, 0, "Cluster_Size", 0},
  {"CLUSTER_STATE_UUID", WSREP_UUID_STR_LEN, MYSQL_TYPE_STRING,
    0, 0, 0, 0},
  {"CLUSTER_STATE_SEQNO", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG,
    0, 0, 0, 0},
  {"CLUSTER_CONF_ID", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG,
    0, 0, 0, 0},
  {"GAP", 10, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"PROTOCOL_VERSION", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG,
    0, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}
};

static int wsrep_memb_fill_table(THD *thd, TABLE_LIST *tables, COND *cond)
{
  int rc= 0;

  if (check_global_access(thd, SUPER_ACL, true))
    return rc;

  wsrep_config_state.lock();

  Dynamic_array<wsrep_member_info_t> *memb_arr=
    wsrep_config_state.get_member_info();

  TABLE *table= tables->table;

  for (unsigned int i= 0; i < memb_arr->elements(); i ++)
  {
    wsrep_member_info_t memb= memb_arr->at(i);

    table->field[COLUMN_WSREP_MEMB_INDEX]->store(i, 0);

    char uuid[40];
    wsrep_uuid_print(&memb.id, uuid, sizeof(uuid));
    table->field[COLUMN_WSREP_MEMB_UUID]->store(uuid, sizeof(uuid),
                                                system_charset_info);
    table->field[COLUMN_WSREP_MEMB_NAME]->store(memb.name, strlen(memb.name),
                                                system_charset_info);
    table->field[COLUMN_WSREP_MEMB_ADDRESS]->store(memb.incoming,
                                                   strlen(memb.incoming),
                                                   system_charset_info);

    if (schema_table_store_record(thd, table))
    {
      rc= 1;
      goto end;
    }
  }

end:
  wsrep_config_state.unlock();
  return rc;
}

static int wsrep_memb_plugin_init(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;

  schema->fields_info= wsrep_memb_fields;
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

  wsrep_config_state.lock();

  wsrep_view_info_t view= wsrep_config_state.get_view_info();
  wsrep_member_status_t status= wsrep_config_state.get_status();

  TABLE *table= tables->table;

  table->field[COLUMN_WSREP_STATUS_NODE_INDEX]
    ->store(view.my_idx, 0);
  table->field[COLUMN_WSREP_STATUS_NODE_STATUS]
    ->store(get_member_status(status), strlen(get_member_status(status)),
            system_charset_info);
  table->field[COLUMN_WSREP_STATUS_CLUSTER_STATUS]
    ->store(get_cluster_status(view.status),
            strlen(get_cluster_status(view.status)),
            system_charset_info);
  table->field[COLUMN_WSREP_STATUS_CLUSTER_SIZE]->store(view.memb_num, 0);

  char uuid[40];
  wsrep_uuid_print(&view.state_id.uuid, uuid, sizeof(uuid));
  table->field[COLUMN_WSREP_STATUS_CLUSTER_STATE_UUID]
    ->store(uuid, sizeof(uuid), system_charset_info);

  table->field[COLUMN_WSREP_STATUS_CLUSTER_STATE_SEQNO]
    ->store(view.state_id.seqno, 0);
  table->field[COLUMN_WSREP_STATUS_CLUSTER_CONF_ID]->store(view.view, 0);

  const char *gap= (view.state_gap == true) ? "YES" : "NO";
  table->field[COLUMN_WSREP_STATUS_GAP]->store(gap, strlen(gap),
                                               system_charset_info);
  table->field[COLUMN_WSREP_STATUS_PROTO_VERSION]->store(view.proto_ver, 0);

  if (schema_table_store_record(thd, table))
    rc= 1;

  wsrep_config_state.unlock();
  return rc;
}

static int wsrep_status_plugin_init(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;

  schema->fields_info= wsrep_status_fields;
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
  MariaDB_PLUGIN_MATURITY_GAMMA                 /* Maturity           */
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
  MariaDB_PLUGIN_MATURITY_GAMMA                 /* Maturity           */
}
maria_declare_plugin_end;

