#define MYSQL_SERVER 1

#include "ha_parquet.h"

#include "sql_class.h"
#include "handler.h"
//#include "duckdb.hpp"
//#include "sql/table.h"
//#include "sql/handler.h"
#include <iostream>

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

//helper to convert mariadb type to duckdb type
std::string mariadb_type_to_duckdb(Field *f) {
    enum_field_types t = f->type();

    switch (t) {
        case MYSQL_TYPE_TINY:
            return "TINYINT";

        case MYSQL_TYPE_SHORT:
            return "SMALLINT";

        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
            return "INTEGER";

        case MYSQL_TYPE_LONGLONG:
            return "BIGINT";

        case MYSQL_TYPE_FLOAT:
            return "FLOAT";

        case MYSQL_TYPE_DOUBLE:
            return "DOUBLE";

        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
            return "DECIMAL";

        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_ENUM:
        case MYSQL_TYPE_SET:
            return "VARCHAR";

        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
        case MYSQL_TYPE_BLOB:
            // make sure text is not becoming BLOB
            if (f->charset() == &my_charset_bin) {
                return "BLOB";
            }
            return "VARCHAR";

        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_NEWDATE:
            return "DATE";

        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_TIME2:
            return "TIME";

        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_DATETIME2:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIMESTAMP2:
            return "TIMESTAMP";

        case MYSQL_TYPE_YEAR:
            return "SMALLINT";

        case MYSQL_TYPE_BIT:
            return "BOOLEAN";

        default:
            return "";
    }
}

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


    //duckdb::DuckDB db(helper_db_path);
    //duckdb::Connection con(db);

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
    std::string query = "CREATE TABLE " + table_name + " (";
    //add the column name and type
    bool first = true; //comma after the first col
    for (Field **field = table_arg->s->field; *field; ++field) {
      Field *f = *field;
      std::string col_name = f->field_name.str;
      enum_field_types t = f->type();
      std::string duck_type = mariadb_type_to_duckdb(f);
      
      if (duck_type.empty()) return HA_ERR_UNSUPPORTED; //this means we are missing a type
      
      if (!first)query += ", ";
      first = false;
      query += col_name + " " + duck_type;
    }
    query += ")";

    std::cout << "DuckDB query: " << query << std::endl;
    //auto result = con.Query(query);

    

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
