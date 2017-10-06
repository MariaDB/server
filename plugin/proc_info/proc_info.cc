#include <my_global.h>
#include <table.h>
#include <sql_show.h>
#include <sql_class.h>
#include <mysql/plugin.h>


static ST_FIELD_INFO fields[]=
{
  { "NAME", 100, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE },
  { "VALUE", 21, MYSQL_TYPE_LONG, 0, MY_I_S_UNSIGNED, 0, SKIP_OPEN_TABLE },
  { 0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0 }
};


static int fill(MYSQL_THD thd, TABLE_LIST *tables, COND *cond)
{
  TABLE *table= tables->table;
  char name[1024];
  unsigned long value;
  FILE *fp;
  int res;

  if (!(fp= fopen("/proc/meminfo", "r")))
    return 1;

  while ((res= fscanf(fp, "%[^:]: %lu kB\n", name, &value)) != EOF)
  {
    if (res != 2)
      continue;
    table->field[0]->store(name, strlen(name), system_charset_info);
    table->field[1]->store(value);
    if (schema_table_store_record(thd, table))
    {
      fclose(fp);
      return 1;
    }
  }
  fclose(fp);

  return 0;
}


static int init(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *) p;

  schema->fields_info= fields;
  schema->fill_table= fill;

  return 0;
}


static struct st_mysql_information_schema plugin=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };


maria_declare_plugin(proc_info)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN, /* type                */
  &plugin,                         /* information schema  */
  "PROC_MEMINFO",                  /* name                */
  ""      ,                        /* author              */
  "Useful information from /proc", /* description         */
  PLUGIN_LICENSE_BSD,              /* license             */
  init,                            /* init callback       */
  0,                               /* deinit callback     */
  0x0100,                          /* version as hex      */
  NULL,                            /* status variables    */
  NULL,                            /* system variables    */
  "1.0",                           /* version as a string */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
