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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

/*
  Expected structure of the grant tables in the 'mysql' schema.

  These definitions are used by Table_check_intact, when the grant tables
  are loaded (acl_reload() / grant_reload()), to compute the status
  variable Grant_tables_intact.

  IMPORTANT: they must be kept in sync with the CREATE TABLE statements in
  scripts/mysql_system_tables.sql (and the upgrade statements in
  scripts/mysql_system_tables_fix.sql). Whoever adds, removes or changes a
  column (name, type or character set) of one of the tables below must
  update the matching definition here, otherwise a freshly installed server
  will report its own grant tables as not intact.
*/

#include "mariadb.h"
#include "sql_acl.h"                            // MYSQL_DB_FIELD_COUNT
#include "grant_table_defs.h"

static const
TABLE_FIELD_TYPE mysql_db_table_fields[MYSQL_DB_FIELD_COUNT] = {
  {
    { STRING_WITH_LEN("Host") },
    { STRING_WITH_LEN("char(255)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Db") },
    { STRING_WITH_LEN("char(64)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("User") },
    { STRING_WITH_LEN("char(128)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Select_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Insert_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Update_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Delete_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Create_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Drop_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Grant_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("References_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Index_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Alter_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Create_tmp_table_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Lock_tables_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Create_view_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Show_view_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Create_routine_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Alter_routine_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Execute_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Event_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Trigger_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Delete_history_priv") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  }
};

const TABLE_FIELD_DEF
mysql_db_table_def= {MYSQL_DB_FIELD_COUNT, mysql_db_table_fields, 0, (uint*) 0 };

static const
TABLE_FIELD_TYPE mysql_roles_mapping_table_fields[4] = {
  {
    { STRING_WITH_LEN("Host") },
    { STRING_WITH_LEN("char(255)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("User") },
    { STRING_WITH_LEN("char(128)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Role") },
    { STRING_WITH_LEN("char(128)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Admin_option") },
    { STRING_WITH_LEN("enum('N','Y')") },
    { STRING_WITH_LEN("utf8") }
  }
};

const TABLE_FIELD_DEF
mysql_roles_mapping_table_def= {4, mysql_roles_mapping_table_fields, 0, (uint*) 0 };

static const
TABLE_FIELD_TYPE mysql_proxies_priv_table_fields[7] = {
  {
    { STRING_WITH_LEN("Host") },
    { STRING_WITH_LEN("char(255)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("User") },
    { STRING_WITH_LEN("char(128)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Proxied_host") },
    { STRING_WITH_LEN("char(255)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Proxied_user") },
    { STRING_WITH_LEN("char(128)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("With_grant") },
    { STRING_WITH_LEN("tinyint(1)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Grantor") },
    { STRING_WITH_LEN("varchar(384)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Timestamp") },
    { STRING_WITH_LEN("timestamp") },
    {NULL, 0}
  }
};

const TABLE_FIELD_DEF
mysql_proxies_priv_table_def= {7, mysql_proxies_priv_table_fields, 0, (uint*) 0 };

static const
TABLE_FIELD_TYPE mysql_tables_priv_table_fields[8] = {
  {
    { STRING_WITH_LEN("Host") },
    { STRING_WITH_LEN("char(255)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Db") },
    { STRING_WITH_LEN("char(64)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("User") },
    { STRING_WITH_LEN("char(128)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Table_name") },
    { STRING_WITH_LEN("char(64)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Grantor") },
    { STRING_WITH_LEN("varchar(384)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Timestamp") },
    { STRING_WITH_LEN("timestamp") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Table_priv") },
    { STRING_WITH_LEN("set('Select','Insert','Update','Delete','Create','Drop','Grant','References','Index','Alter','Create View','Show view','Trigger','Delete versioning rows')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Column_priv") },
    { STRING_WITH_LEN("set('Select','Insert','Update','References')") },
    { STRING_WITH_LEN("utf8") }
  }
};

const TABLE_FIELD_DEF
mysql_tables_priv_table_def= {8, mysql_tables_priv_table_fields, 0, (uint*) 0 };

static const
TABLE_FIELD_TYPE mysql_columns_priv_table_fields[7] = {
  {
    { STRING_WITH_LEN("Host") },
    { STRING_WITH_LEN("char(255)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Db") },
    { STRING_WITH_LEN("char(64)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("User") },
    { STRING_WITH_LEN("char(128)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Table_name") },
    { STRING_WITH_LEN("char(64)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Column_name") },
    { STRING_WITH_LEN("char(64)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Timestamp") },
    { STRING_WITH_LEN("timestamp") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Column_priv") },
    { STRING_WITH_LEN("set('Select','Insert','Update','References')") },
    { STRING_WITH_LEN("utf8") }
  }
};

const TABLE_FIELD_DEF
mysql_columns_priv_table_def= {7, mysql_columns_priv_table_fields, 0, (uint*) 0 };

static const
TABLE_FIELD_TYPE mysql_procs_priv_table_fields[8] = {
  {
    { STRING_WITH_LEN("Host") },
    { STRING_WITH_LEN("char(255)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Db") },
    { STRING_WITH_LEN("char(64)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("User") },
    { STRING_WITH_LEN("char(128)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Routine_name") },
    { STRING_WITH_LEN("char(64)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Routine_type") },
    { STRING_WITH_LEN("enum('FUNCTION','PROCEDURE','PACKAGE','PACKAGE BODY')") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Grantor") },
    { STRING_WITH_LEN("varchar(384)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Proc_priv") },
    { STRING_WITH_LEN("set('Execute','Alter Routine','Grant')") },
    { STRING_WITH_LEN("utf8") }
  },
  {
    { STRING_WITH_LEN("Timestamp") },
    { STRING_WITH_LEN("timestamp") },
    {NULL, 0}
  }
};

const TABLE_FIELD_DEF
mysql_procs_priv_table_def= {8, mysql_procs_priv_table_fields, 0, (uint*) 0 };

static const
TABLE_FIELD_TYPE mysql_global_priv_table_fields[3] = {
  {
    { STRING_WITH_LEN("Host") },
    { STRING_WITH_LEN("char(255)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("User") },
    { STRING_WITH_LEN("char(128)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("Priv") },
    { STRING_WITH_LEN("longtext") },
    {NULL, 0}
  }
};

const TABLE_FIELD_DEF
mysql_global_priv_table_def= {3, mysql_global_priv_table_fields, 0, (uint*) 0 };
