#include "ha_parquet"

#define MYSQL_SERVER 1

#include <my_global.h>
#include <m_string.h>
#include "maria_def.h"
#include "sql_class.h"
#include <mysys_err.h>
#include <libmarias3/marias3.h>
#include <discover.h>
#include "ha_s3.h"
#include "s3_func.h"
#include "aria_backup.h"

#define DEFAULT_AWS_HOST_NAME "s3.amazonaws.com"


handlerton *parquet_hton= 0;


static handler *parquet_create_handler(handlerton *p_hton,
                                  TABLE_SHARE * table,
                                  MEM_ROOT *mem_root)
{
  return new (mem_root) ha_s3(p_hton, table);
}

static int ha_parquet_init(void *p)
{
    parquet_hton->create = parquet_create_handler;
}

struct st_mysql_storage_engine parquet_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(pquet)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &parquet_storage_engine,
  "Parquet",
  "UIUC Disruption Lab",
  //PLUGIN_LICENSE_GPL,
  ha_parquet_init,                   /* Plugin Init      */
  //ha_s3_deinit,                 /* Plugin Deinit    */
  //0x0100,                       /* 1.0              */
  //status_variables,             /* status variables */
  //system_variables,             /* system variables */
  //"1.0",                        /* string version   */
  //MariaDB_PLUGIN_MATURITY_STABLE/* maturity         */
}


