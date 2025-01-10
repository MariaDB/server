/* Copyright (c) 2017, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */

#include <my_global.h>
#include <mysqld.h>
#include <mysql.h>
#include <xtrabackup.h>
#include <encryption_plugin.h>
#include <sql_plugin.h>
#include <sstream>
#include <vector>
#include <common.h>
#include <backup_mysql.h>
#include <log0crypt.h>


extern struct st_maria_plugin *mysql_optional_plugins[];
extern struct st_maria_plugin *mysql_mandatory_plugins[];
static void encryption_plugin_init(int argc, char **argv);

extern char *xb_plugin_load;
extern char *xb_plugin_dir;

const int PLUGIN_MAX_ARGS = 1024;
std::vector<std::string> backup_plugins_args;

const char *QUERY_PLUGIN =
"SELECT plugin_name, plugin_library, @@plugin_dir"
" FROM information_schema.plugins WHERE plugin_type='ENCRYPTION'"
" OR (plugin_type = 'DAEMON' AND plugin_name LIKE 'provider\\_%')"
" AND plugin_status='ACTIVE'";

std::string encryption_plugin_config;

static void add_to_plugin_load_list(const char *plugin_def)
{
  opt_plugin_load_list_ptr->push_back(new i_string(plugin_def));
}

static char XTRABACKUP_EXE[] = "xtrabackup";

/*
  Read "plugin-load" value (encryption plugin) from backup-my.cnf during
  prepare phase.
  The value is stored during backup phase.
*/
static std::string get_encryption_plugin_from_cnf()
{
  FILE *f = fopen("backup-my.cnf", "r");
  if (!f)
  {
    die("Can't open backup-my.cnf for reading");
  }
  char line[512];
  std::string plugin_load;
  while (fgets(line, sizeof(line), f))
  {
    if (strncmp(line, "plugin_load=", 12) == 0)
    {
      plugin_load = line + 12;
      // remote \n at the end of string
      plugin_load.resize(plugin_load.size() - 1);
    }

    if (strncmp(line, "innodb_encrypt_tables=", 22) == 0)
    {
      if (!strncmp(line + 22, "ON", 2) ||
          !strncmp(line + 22, "1", 1))
        srv_encrypt_tables= 1;
      else if (!strncmp(line + 22, "FORCE", 5) ||
               !strncmp(line + 22, "2", 1))
        srv_encrypt_tables= 2;
    }
  }
  fclose(f);
  return plugin_load;
}


void encryption_plugin_backup_init(MYSQL *mysql)
{
  MYSQL_RES *result;
  MYSQL_ROW row;
  std::ostringstream oss;
  char *argv[PLUGIN_MAX_ARGS];
  char show_query[1024] = "";
  std::string plugin_load;
  int argc;

  result = xb_mysql_query(mysql, QUERY_PLUGIN, true, true);
  while ((row = mysql_fetch_row(result)))
  {
    char *name= row[0];
    char *library= row[1];
    char *dir= row[2];

    if (!plugin_load.length())
    {
#ifdef _WIN32
      for (char *p = dir; *p; p++)
        if (*p == '\\') *p = '/';
#endif
      strncpy(opt_plugin_dir, dir, FN_REFLEN - 1);
      opt_plugin_dir[FN_REFLEN - 1] = '\0';
      oss << "plugin_dir=" << '"' << dir << '"' << std::endl;
    }

    plugin_load += std::string(";") + name;

    if (library)
    {
      /* Remove shared library suffixes, in case we'll prepare on different OS.*/
      const char *extensions[] = { ".dll", ".so", 0 };
      for (size_t i = 0; extensions[i]; i++)
      {
        const char *ext = extensions[i];
        if (ends_with(library, ext))
          library[strlen(library) - strlen(ext)] = 0;
      }
      plugin_load += std::string("=") + library;
    }

    if (strncmp(name, "provider_", 9) == 0)
      continue;

    /* Read plugin variables. */
    snprintf(show_query, sizeof(show_query), "SHOW variables like '%s_%%'", name);
  }
  mysql_free_result(result);
  if (!plugin_load.length())
    return;

  oss << "plugin_load=" << plugin_load.c_str() + 1 << std::endl;

  /* Required  to load the plugin later.*/
  add_to_plugin_load_list(plugin_load.c_str() + 1);


  if (*show_query)
  {
    result = xb_mysql_query(mysql, show_query, true, true);
    while ((row = mysql_fetch_row(result)))
    {
      std::string arg("--");
      arg += row[0];
      arg += "=";
      arg += row[1];
      backup_plugins_args.push_back(arg);
      oss << row[0] << "=" << row[1] << std::endl;
    }

    mysql_free_result(result);

    /* Check whether to encrypt logs. */
    result = xb_mysql_query(mysql, "select @@innodb_encrypt_log", true, true);
    row = mysql_fetch_row(result);
    srv_encrypt_log = (row != 0 && row[0][0] == '1');
    oss << "innodb_encrypt_log=" << row[0] << std::endl;

    mysql_free_result(result);
  }

  result = xb_mysql_query(mysql, "select @@innodb_encrypt_tables", true, true);
  row = mysql_fetch_row(result);
  if (!row);
  else if (const char *r= row[0])
  {
    if (!strcmp(r, "ON")) srv_encrypt_tables= 1;
    else if (!strcmp(r, "FORCE")) srv_encrypt_tables= 2;
    oss << "innodb_encrypt_tables=" << r << std::endl;
  }

  mysql_free_result(result);
  encryption_plugin_config = oss.str();

  argc = 0;
  argv[argc++] = XTRABACKUP_EXE;
  for(size_t i = 0; i <  backup_plugins_args.size(); i++)
  {
    argv[argc++] = (char *)backup_plugins_args[i].c_str();
    if (argc == PLUGIN_MAX_ARGS - 2)
      break;
  }
  argv[argc] = 0;

  encryption_plugin_init(argc, argv);
}

const char *encryption_plugin_get_config()
{
  return encryption_plugin_config.c_str();
}

extern int finalize_encryption_plugin(void *plugin);


void encryption_plugin_prepare_init(int argc, char **argv)
{
  std::string plugin_load= get_encryption_plugin_from_cnf();
  if (plugin_load.size())
  {
    msg("Loading encryption plugin from %s", plugin_load.c_str());
  }
  else
  {
    finalize_encryption_plugin(0);
    return;
  }

  add_to_plugin_load_list(plugin_load.c_str());

  if (xb_plugin_dir)
  {
    strncpy(opt_plugin_dir, xb_plugin_dir, FN_REFLEN - 1);
    opt_plugin_dir[FN_REFLEN - 1] = '\0';
  }

  char **new_argv = new char *[argc + 2];
  new_argv[0] = XTRABACKUP_EXE;
  memcpy(&new_argv[1], argv, argc*sizeof(char *));
  new_argv[argc+1]= 0;

  encryption_plugin_init(argc+1, new_argv);

  delete[] new_argv;
}

static void encryption_plugin_init(int argc, char **argv)
{
  /* Patch optional and mandatory plugins, we only need to load the one in xb_plugin_load. */
  mysql_optional_plugins[0] = mysql_mandatory_plugins[0] = 0;
  plugin_maturity = MariaDB_PLUGIN_MATURITY_UNKNOWN; /* mariabackup accepts all plugins */
  msg("Loading encryption plugin");
  for (int i= 1; i < argc; i++)
    msg("\t Encryption plugin parameter :  '%s'", argv[i]);
  plugin_init(&argc, argv, PLUGIN_INIT_SKIP_PLUGIN_TABLE);
}

