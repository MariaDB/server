#include <mysqld.h>
#include <mysql.h>
#include <xtrabackup.h>
#include <encryption_plugin.h>
#include <backup_copy.h>
#include <sql_plugin.h>
#include <sstream>
#include <vector>
#include <common.h>
#include <backup_mysql.h>


extern struct st_maria_plugin *mysql_optional_plugins[];
extern struct st_maria_plugin *mysql_mandatory_plugins[];
static void encryption_plugin_init(int argc, char **argv);

extern char *xb_plugin_load;
extern char *xb_plugin_dir;

const int PLUGIN_MAX_ARGS = 1024;
vector<string> backup_plugins_args;

const char *QUERY_PLUGIN =
"SELECT plugin_name, plugin_library, @@plugin_dir"
" FROM information_schema.plugins WHERE plugin_type='ENCRYPTION'"
" AND plugin_status='ACTIVE'";

string encryption_plugin_config;

static void add_to_plugin_load_list(const char *plugin_def)
{
  opt_plugin_load_list_ptr->push_back(new i_string(plugin_def));
}

static char XTRABACKUP_EXE[] = "xtrabackup";

void encryption_plugin_backup_init(MYSQL *mysql)
{
  MYSQL_RES *result;
  MYSQL_ROW row;
  ostringstream oss;
  char *argv[PLUGIN_MAX_ARGS];
  int argc;

  result = xb_mysql_query(mysql, QUERY_PLUGIN, true, true);
  row = mysql_fetch_row(result);
  if (!row)
  {
    mysql_free_result(result);
    return;
  }

  char *name= row[0];
  char *library= row[1];
  char *dir= row[2];

#ifdef _WIN32
  for (char *p = dir; *p; p++)
    if (*p == '\\') *p = '/';
#endif

  string plugin_load(name);
  if (library)
    plugin_load += string("=") + library;

  oss << "plugin_load=" << plugin_load << endl;

  /* Required  to load the plugin later.*/
  add_to_plugin_load_list(plugin_load.c_str());
  strncpy(opt_plugin_dir, dir, FN_REFLEN);

  oss << "plugin_dir=" << '"' << dir << '"' << endl;


  /* Read plugin variables. */
  char query[1024];
  snprintf(query, 1024, "SHOW variables like '%s_%%'", name);
  mysql_free_result(result);

  result = xb_mysql_query(mysql, query, true, true);
  while ((row = mysql_fetch_row(result)))
  {
    string arg("--");
    arg += row[0];
    arg += "=";
    arg += row[1];
    backup_plugins_args.push_back(arg);
    oss << row[0] << "=" << row[1] << endl;
  }

  mysql_free_result(result);

  /* Check whether to encrypt logs. */
  result = xb_mysql_query(mysql, "select @@innodb_encrypt_log", true, true);
  row = mysql_fetch_row(result);
  srv_encrypt_log = (row != 0 && row[0][0] == '1');
  oss << "innodb_encrypt_log=" << row[0] << endl;

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

extern int finalize_encryption_plugin(st_plugin_int *plugin);


void encryption_plugin_prepare_init(int argc, char **argv)
{

  if (!xb_plugin_load)
  {
    /* This prevents crashes e.g in --stats with wrong my.cnf*/
    finalize_encryption_plugin(0);
    return;
  }

  add_to_plugin_load_list(xb_plugin_load);

  if (xb_plugin_dir)
    strncpy(opt_plugin_dir, xb_plugin_dir, FN_REFLEN);

  char **new_argv = new char *[argc + 1];
  new_argv[0] = XTRABACKUP_EXE;
  memcpy(&new_argv[1], argv, argc*sizeof(char *));

  encryption_plugin_init(argc+1, new_argv);

  delete[] new_argv;
}

static void encryption_plugin_init(int argc, char **argv)
{
  /* Patch optional and mandatory plugins, we only need to load the one in xb_plugin_load. */
  mysql_optional_plugins[0] = mysql_mandatory_plugins[0] = 0;
  msg("Loading encryption plugin\n");
  for (int i= 1; i < argc; i++)
    msg("\t Encryption plugin parameter :  '%s'\n", argv[i]);
  plugin_init(&argc, argv, PLUGIN_INIT_SKIP_PLUGIN_TABLE);
}

