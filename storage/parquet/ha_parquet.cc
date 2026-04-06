#define MYSQL_SERVER 1

#include "ha_parquet.h"
#include "sql_class.h"
#include "handler.h"

handlerton *parquet_hton= 0;
static THR_LOCK parquet_lock;

ha_parquet::ha_parquet(handlerton *hton, TABLE_SHARE *table_arg)
  : handler(hton, table_arg)
{
  thr_lock_data_init(&parquet_lock, &lock, NULL);
}

ulonglong ha_parquet::table_flags() const
{
  return HA_NO_TRANSACTIONS | HA_FILE_BASED;
}

ulong ha_parquet::index_flags(uint, uint, bool) const
{
  return 0;
}

int ha_parquet::open(const char *, int, uint)
{

  DBUG_ENTER("ha_parquet::open");

  row_count = 0;
  flush_threshold = -1; // filler for now; ik that BlOCK_SIZE should be capped at 16MB, 
  // but I'm not sure how many rows (on average) that would be 
  duckdb_initialized = false;


  // Steps to get DuckDB stuff to work:
  // 1: Create the in-memory DuckDB database and connection
  //     - Code should look something like this (putting on heap so we can use it in other methods (like write_row() for example)):
  //        db = new duckdb::DuckDB(nullptr);
  //        con = new duckdb::Connection(*db);
  //.       con->Query("SET memory_limit='32MB'"); // memory_limit size isn't mentioned in the systems design document, so I set it to 32MB for now (double the BLOCK_SIZE)
  // 2: View Iceberg Table in DuckDB
  //     - Code should look something like this:
  //        con->Query("INSTALL iceberg;");   runs only once when needed; skips otherwise
  //        con->Query("LOAD iceberg;");      this needs to run everytime
  //        std::string s3_path = our path to s3 storage;
  //        std::string create_view_iceberg_query = "CREATE VIEW iceberg_view AS SELECT * FROM iceberg_scan('" + s3_path + "')";
  //        con->Query(create_view_iceberg_query);
  


  DBUG_RETURN(0);
  duckdb_initialized = true;


}

int ha_parquet::close(void)
{
  return 0;
}

int ha_parquet::create(const char *, TABLE *, HA_CREATE_INFO *)
{
  return 0;
}

int ha_parquet::write_row(const uchar *buf)
{
  return HA_ERR_WRONG_COMMAND;
}

int ha_parquet::update_row(const uchar *old_data, const uchar *new_data)
{
  return HA_ERR_WRONG_COMMAND;
}

int ha_parquet::delete_row(const uchar *buf)
{
  return HA_ERR_WRONG_COMMAND;
}

int ha_parquet::rnd_init(bool)
{
  return 0;
}

int ha_parquet::rnd_next(uchar *)
{
  return HA_ERR_END_OF_FILE;
}

int ha_parquet::rnd_pos(uchar *, uchar *)
{
  return HA_ERR_WRONG_COMMAND;
}

void ha_parquet::position(const uchar *)
{
}

int ha_parquet::info(uint)
{
  return 0;
}

enum_alter_inplace_result
ha_parquet::check_if_supported_inplace_alter(TABLE *,
                                             Alter_inplace_info *)
{
  return HA_ALTER_INPLACE_NOT_SUPPORTED;
}

int ha_parquet::external_lock(THD *thd, int lock_type) {

  DBUG_ENTER("ha_parquet::external_lock");

  if (lock_type == F_RDLCK) {
    trans_register_ha(thd, false, ht, 0);

    parquet_trx_data *trx = (parquet_trx_data *) thd_get_ha_data(thd, ht);

    if (trx == NULL) {
      trx = new parquet_trx_data();
      thd_set_ha_data(thd, ht, trx);
    }
  } else if (lock_type == F_WRLCK) {
    trans_register_ha(thd, false, ht, 0);
    parquet_trx_data *trx = (parquet_trx_data *) thd_get_ha_data(thd, ht);

    if (trx == NULL) {
      trx = new parquet_trx_data();
      thd_set_ha_data(thd, ht, trx);
    }
  } else if (lock_type == F_UNLCK) {
    // flush remaining buffered rows to S3
    
  }
  
  DBUG_RETURN(0);
}

THR_LOCK_DATA **ha_parquet::store_lock(THD *thd,
                                       THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK){
    lock.type= lock_type;
  }
  
  *to++ = &lock;
  return to;
}

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
    thr_lock_init(&parquet_lock);
    return 0;
}

static int ha_parquet_deinit(void *p)
{
  parquet_hton = 0;
  thr_lock_delete(&parquet_lock);
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
  0x0100,                            /* 1.0              */
  NULL,                              /* status variables */
  NULL,                               /* system variables */
  "1.0",                        /* string version   */
  MariaDB_PLUGIN_MATURITY_STABLE/* maturity         */
}
maria_declare_plugin_end;
