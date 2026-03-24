#define MYSQL_SERVER 1

#include "ha_parquet.h"
#include "sql_class.h"
#include "handler.h"

handlerton *parquet_hton= 0;


static handler *parquet_create_handler(handlerton *p_hton,
                                  TABLE_SHARE * table,
                                  MEM_ROOT *mem_root)
{
  return new (mem_root) ha_parquet(p_hton, table);
}

static int ha_parquet_init(void *p)
{
    parquet_hton = (handlerton *) p;
    parquet_hton->create = parquet_create_handler;
    return 0;
}

static int ha_parquet_deinit(void *p)
{
  parquet_hton = 0;
  return 0;
}

struct st_mysql_storage_engine parquet_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(parquet)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &parquet_storage_engine,
  "PARQUET",
  "UIUC Disruption Lab",
  "Parquet Storage Engine ",
  PLUGIN_LICENSE_GPL,
  ha_parquet_init,                   /* Plugin Init      */
  ha_parquet_deinit,                 /* Plugin Deinit    */
  0x0100,                       /* 1.0              */
  NULL,             /* status variables */
  NULL,             /* system variables */
  "1.0",                        /* string version   */
  MariaDB_PLUGIN_MATURITY_STABLE/* maturity         */
}
maria_declare_plugin_end;

