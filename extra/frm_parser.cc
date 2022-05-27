#define MYSQL_SERVER
#include "my_global.h"
#include "mysql_version.h"
#include "sql_class.h"
#include "mysql.h"
#include "set_var.h"
#include "sql_plugin.h"
#include "sql_show.h"

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

int64_t get_file_size(const char *file_name)
{
  int64_t _file_size= 0;
  FILE *fd= fopen(file_name, "rb");
  if (fd == NULL)
  {
    _file_size= -1;
  }
  else
  {
    while (getc(fd) != EOF)
      _file_size++;
    fclose(fd);
  }
  return _file_size;
}

void print_ddl() {
  THD *thd;
  thd= new THD(0);
  TABLE_SHARE share;
  TABLE_LIST list;
  TABLE table;
  char buf[2048];

  init_tmp_table_share(thd, &share, "", 0, "table2", "C:/Users/OMEN/Desktop/sample");
  open_table_def(thd, &share);
  open_table_from_share(thd, &share, &empty_clex_str, 0, READ_ALL, 0, &table,
                        true);

  list.table= &table;

  String query(buf, sizeof(buf), system_charset_info);
  show_create_table(thd, &list, &query, NULL, WITHOUT_DB_NAME);

  delete thd;
}

int main(int argc, char **argv)
{
  mysql_server_init(-1, NULL, NULL);
  wsrep_thr_init();
#ifdef WITH_WSEP
  if (wsrep_init_server())
    unireg_abort(1);
#endif // WITH_WSEP

  MY_INIT(argv[0]);
  system_charset_info= &my_charset_utf8mb3_general_ci;
  sys_var_init();
  init_common_variables(&argc, &argv);
  plugin_mutex_init();
  init_thr_timer(5);
  my_rnd_init(&sql_rand, (ulong) 123456, (ulong) 123);
  frm_plugin_init(argc, argv);

  const uchar str[33]= "C:/Users/OMEN/Desktop/sample";
  print_ddl();

  return 0;
}