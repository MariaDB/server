#define MYSQL_SERVER 1

#include "ha_parquet.h"

#include "sql_class.h"
#include "handler.h"
#include "duckdb.hpp"
#include "sql/table.h"
#include "sql/handler.h"

handlerton *parquet_hton= 0;
static THR_LOCK parquet_lock;

ha_parquet::ha_parquet(handlerton *hton, TABLE_SHARE *table_arg) : handler(hton, table_arg)
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
  return 0;
}

int ha_parquet::close(void)
{
  return 0;
}

// int ha_parquet::create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info)
// {
//   // auto *options= create_info ? create_info->option_struct : nullptr;

//   auto *options = nullptr;

//   if(create_info->option_struct){
//     options = create_info;
//   }

//   if (!options || !options->file_name || !*options->file_name)
//   {
//     my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0), "PARQUET", "FILE_NAME");
//     return HA_ERR_GENERIC;
//   }

//   if ((create_info->options & HA_LEX_CREATE_TMP_TABLE) != 0)
//   {
//     my_error(ER_UNSUPPORTED_EXTENSION, MYF(0), "TEMPORARY PARQUET tables");
//     return HA_ERR_GENERIC;
//   }

//   try
//   {
//     ParquetTableNames dt(name);
//     std::string parquet_file= options->file_name;
    // std::string catalog_path= make_catalog_path(name);

//     duckdb::DuckDB db(catalog_path);
//     duckdb::Connection con(db);

//     std::string error;
//     if (!validate_parquet_schema(table_arg, con, parquet_file, error))
//     {
//       my_error(ER_UNKNOWN_ERROR, MYF(0), error.c_str());
//       return HA_ERR_GENERIC;
//     }

//     std::ostringstream sql;
//     sql << "CREATE SCHEMA IF NOT EXISTS " << sql_quote_identifier(dt.db_name)
//         << "; CREATE OR REPLACE VIEW "
//         << sql_quote_identifier(dt.db_name) << "."
//         << sql_quote_identifier(dt.table_name)
//         << " AS SELECT * FROM read_parquet(" << sql_quote_string(parquet_file)
//         << ")";

//     auto create_result= con.Query(sql.str());
//     if (!create_result || create_result->HasError())
//     {
//       const std::string error_msg=
//           create_result ? create_result->GetError() : "DuckDB query failed";
//       my_error(ER_UNKNOWN_ERROR, MYF(0), error_msg.c_str());
//       return HA_ERR_GENERIC;
//     }
//   }
//   catch (const std::exception &ex)
//   {
//     my_error(ER_UNKNOWN_ERROR, MYF(0), ex.what());
//     return HA_ERR_GENERIC;
//   }

//   return 0;
// }


int ha_parquet::create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info) {

    
  
    std::string table_path(name);
    // Setting the helper_db_path if it is empty, if not then it already exists so we can use it. 
    if(helper_db_path.empty()){
        size_t pos  = table_path.find_last_of("/\\");
        
        if(pos == std::string::npos) {
          helper_db_path = "duckdb_helper.duckdb";

         }
        else {
           helper_db_path = table_path.substr(0, pos) + "/duckdb_helper.duckdb";
        }
    }


    duckdb::DuckDB db(helper_db_path);
    duckdb::Connection con(db);

    // Path to the parquet file
    std::string parquet_file = std::string(name) + ".parquet";


    // Getting the table name itself (It will alwaus be the end of the file path like /database/table) get the table
    std::string table_name;
    size_t pos = table_path.find_last_of("/\\");
    if(pos == std::string::npos){
      table_name = table_path;
    }
    else{
      table_name = table_path.substr(pos+1);
    }

    // Now build the query
    std::string query;
    


    auto result = con.Query(query);

    

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
