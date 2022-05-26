#define MYSQL_SERVER
#include "my_global.h"
#include "mysql_version.h"
#include "sql_class.h"
#include "mysql.h"
#include "set_var.h"
#include "sql_plugin.h"
extern struct st_maria_plugin *mysql_optional_plugins[];
extern struct st_maria_plugin *mysql_mandatory_plugins[];
static void frm_plugin_init(int argc, char **argv)
{
  /* Patch optional and mandatory plugins, we only need to load the one in
   * xb_plugin_load. */
  mysql_optional_plugins[0]= mysql_mandatory_plugins[0]= 0;
  plugin_maturity=
      MariaDB_PLUGIN_MATURITY_UNKNOWN; /* mariabackup accepts all plugins */

  plugin_init(&argc, argv, PLUGIN_INIT_SKIP_PLUGIN_TABLE);
}

int init_common_variables(int *argc_ptr, char ***argv_ptr);
int main(int argc, char **argv)
{
  mysql_server_init(-1, NULL, NULL);
  wsrep_thr_init();
#ifdef WITH_WSEP
  if (wsrep_init_server())
    unireg_abort(1);
#endif // WSREP

  system_charset_info= &my_charset_utf8mb3_general_ci;
  sys_var_init();
  init_common_variables(&argc, &argv);
  plugin_mutex_init();
  init_thr_timer(5);
  my_rnd_init(&sql_rand, (ulong) 123456, (ulong) 123);
  frm_plugin_init(argc, argv);

  THD *thd;
  thd= new THD(0);

  delete thd;
  return 0;
}