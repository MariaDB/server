#define MYSQL_SERVER 1

#include "ha_parquet.h"

#include "sql_class.h"
#include "handler.h"
#include "duckdb.hpp"
#include "duckdb.hpp"
#include "sql/table.h"
#include "sql/handler.h"
#include "thr_lock.h"
#include "my_base.h"
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
  db = new duckdb::DuckDB(nullptr);
  con = new duckdb::Connection(*db);
  con->Query("SET memory_limit='32MB'"); // memory_limit size isn't mentioned in the systems design document, so I set it to 32MB for now (double the BLOCK_SIZE)
  // 2: View Iceberg Table in DuckDB
  //     - Code should look something like this:
  con->Query("INSTALL iceberg;");   //runs only once when needed; skips otherwise
  con->Query("LOAD iceberg;");      //this needs to run everytime
  //std::string s3_path = "";     //our path to s3 storage;
  //std::string create_view_iceberg_query = "CREATE VIEW iceberg_view AS SELECT * FROM iceberg_scan('" + s3_path + "')";
  //con->Query(create_view_iceberg_query);
  


  
  duckdb_initialized = true;
  DBUG_RETURN(0);


}

int ha_parquet::close(void)
{
  return 0;
}

//Walks the AST and converts it into a string. Check this. 
static std::string item_to_sql(const Item *item)
{
  if (!item) return "";

  // Check if it has binary operation >,<. =
  const Item_func *func = dynamic_cast<const Item_func *>(item);
  if (!func || func->argument_count() != 2) {
    return "";
  }

  const Item *left = func->arguments()[0];
  const Item *right = func->arguments()[1];

  // Left must be a column
  if (left->type() != Item::FIELD_ITEM) {
    return "";
  }

  const Item_field *field = static_cast<const Item_field *>(left);
  std::string col = field->field_name.str;

  std::string val;

  // Right must be a int or string
  if (right->type() == Item::INT_ITEM) {
    val = std::to_string(right->val_int());
  }
  else if (right->type() == Item::STRING_ITEM) {
    String tmp;
    String *s = right->val_str(&tmp);
    if (!s) return "";
    val = quote_string_literal(std::string(s->ptr(), s->length()));
  }
  else {
    return "";
  }

  std::string op = func->func_name();

  if (op == "=" || op == "eq") return col + " = " + val;
  if (op == ">" || op == "gt") return col + " > " + val;
  if (op == "<" || op == "lt") return col + " < " + val;

  return "";
}

//Gets the condition information after WHERE and stores it in pushed_cond_sql
//Takes in the WHERE clause as a tree
const Item *ha_parquet::cond_push(const Item *cond)
{
  DBUG_ENTER("ha_parquet::cond_push");

  pushed_cond_sql.clear();
  has_pushed_cond = false;

  if (cond) {
    pushed_cond_sql = item_to_sql(cond);
    if (!pushed_cond_sql.empty()) {
      has_pushed_cond = true;
      //std::cout << "Pushed condition: " << pushed_cond_sql << std::endl;
    }
  }

  DBUG_RETURN(nullptr);
}

//Clears the current saved filter
void ha_parquet::cond_pop()
{
  pushed_cond_sql.clear();
  has_pushed_cond = false;
}

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

static std::string quote_string_literal(const std::string &value)
{
    std::string quoted = "'";

    for (char ch : value) {
      if (ch == '\'') {
        quoted += "''";
      } else {
        quoted += ch;
      }
    }

    quoted += "'";
    return quoted;
}



std::string build_query_create(std::string table_name, TABLE *table_arg){
    std::string query = "CREATE TABLE " + table_name + " (";

      
    //add the column name and type
    bool first = true; //comma after the first col
    for (Field **field = table_arg->s->field; *field; ++field) {
      Field *f = *field;
      std::string col_name = f->field_name.str;
      enum_field_types t = f->type();
      std::string duck_type = mariadb_type_to_duckdb(f);
            
      if (duck_type.empty()){
        return ""; // Missing a type
      }

      if(!first){
        query += ", ";
      }


      first = false;
      query += col_name + " " + duck_type;
    }
    query += ")";

    std::cout << "DuckDB query: " << query << std::endl;

    return query;
}

static std::string build_copy_to_parquet_query(const std::string &table_name,
                                               const std::string &parquet_file)
{
    return "COPY " + table_name + " TO " +
           quote_string_literal(parquet_file) + " (FORMAT PARQUET)";
}


int ha_parquet::create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info) {

    (void) create_info;

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

    // Path to the parquet file
    std::string parquet_file = std::string(name) + ".parquet";


    // Getting the table name itself (It will always be the end of the file path like /database/table)
    std::string table_name;
    size_t pos = table_path.find_last_of("/\\");
    if(pos == std::string::npos){
      table_name = table_path;
    }
    else{
      table_name = table_path.substr(pos+1);
    }

    // Now build the query

    std::string query = build_query_create(table_name, table_arg);
    if (query.empty()) {
      return HA_ERR_UNSUPPORTED;
    }

    try {
      duckdb::DuckDB db(helper_db_path);
      duckdb::Connection con(db);

      auto create_result = con.Query(query);
      if (!create_result || create_result->HasError()) {
        return HA_ERR_INTERNAL_ERROR;
      }

      auto copy_result = con.Query(build_copy_to_parquet_query(table_name, parquet_file));
      if (!copy_result || copy_result->HasError()) {
        return HA_ERR_INTERNAL_ERROR;
      }
        
    } catch (const duckdb::Exception &) {
      return HA_ERR_INTERNAL_ERROR;
    } catch (const std::exception &) {
      return HA_ERR_INTERNAL_ERROR;
    }
    return 0;
}

static bool needs_quoting(Field *f) {
  switch (f->type())
  {
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2:
      return true;
    default:
      return false;
  }
}

std::string ha_parquet::flush_remaining_rows_to_s3() {
  if (row_count == 0) {
    return "";
  }

  static uint64_t flush_counter = 0;
  std::string s3_path = "s3://parquet-bucket/data/part_" + std::to_string(time(nullptr)) + "_" + std::to_string(flush_counter++) + ".parquet";
  auto copy_result = con->Query(build_copy_to_parquet("buffer", s3_path));

  if (copy_result->HasError() == true) {
    return "";
  }

  auto delete_result = con->Query("DELETE FROM buffer");

  if (delete_result->HasError() == true) {
    return "";
  }

  row_count = 0;

  return s3_path;
}


int ha_parquet::write_row(const uchar *buf)
{
  DBUG_ENTER("ha_parquet::write_row");

  // AK notes - Initial setup stuff (just building CREATE if the buffer table hasn't been created yet)
  if (buffer_table_created == false) {
    std::string create_sql = build_query_create("buffer", table);

    if (create_sql.empty() == true) {
      DBUG_RETURN(HA_ERR_UNSUPPORTED);
    }

    auto result = con->Query(create_sql);

    if (result->HasError() == true) {
      DBUG_RETURN(HA_ERR_GENERIC);
    }

    buffer_table_created = true;
  }

  // AK notes - Creating the INSERT string for DuckDB
  std::string insert_sql = "INSERT INTO buffer VALUES (";
  bool first_flag = false;

  for (Field **field = table->s->field; *field; ++field) {
    Field *f = *field;

    if (first_flag == true) {
      insert_sql = insert_sql + ", ";
    }

    first_flag = true;

    if (f->is_null() == true) {
      insert_sql = insert_sql + "NULL";
    } else {
      String val;
      f->val_str(&val);
      std::string val_cpp(val.ptr(), val.length());

      if (needs_quoting(f) == true) {
        insert_sql = insert_sql + quote_string_literal(val_cpp);
      } else {
        insert_sql = insert_sql + val_cpp;
      }
    } 
  }

  insert_sql = insert_sql + ")";

  auto result = con->Query(insert_sql);

  if (result->HasError() == true) {
    DBUG_RETURN(HA_ERR_GENERIC);
  }

  row_count = row_count + 1;

  // AK Notes - This is the flushing stuff (flushing to S3)
  if (row_count >= flush_threshold) {
    std::string s3_path = flush_remaining_rows_to_s3();

    parquet_trx_data *trx = (parquet_trx_data *) thd_get_ha_data(ha_thd(), ht);

    if (trx && !s3_path.empty()) {
      trx->s3_file_paths.push_back(s3_path);
    }
  }

  DBUG_RETURN(0);
}

int ha_parquet::update_row(const uchar *old_data, const uchar *new_data)
{
  return HA_ERR_WRONG_COMMAND;
}

int ha_parquet::delete_row(const uchar *buf)
{
  return HA_ERR_WRONG_COMMAND;
}

int ha_parquet::rnd_init(bool scan)
{
  (void) scan;
  DBUG_ENTER("ha_parquet::rnd_init");
  
  current_row = 0;
  scan_result.reset();
  
  if (!duckdb_initialized || con == nullptr) DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  std::string parquet_path = std::string(table->s->table_name.str) + ".parquet";
  std::string query = "SELECT * FROM read_parquet(" + quote_string_literal(parquet_path) + ")";
  std::cout << "rnd_init DuckDB query: " << query << std::endl;

  try {
    auto result = con->Query(query);

    if (!result || result->HasError()) {
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }

    scan_result = std::move(result);
  } catch (const duckdb::Exception &) {
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  } catch (const std::exception &) {
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  DBUG_RETURN(0);
  
  return 0;
}

int ha_parquet::rnd_next(uchar *buf)
{
  (void) buf;
  DBUG_ENTER("ha_parquet::rnd_next");

  if (!scan_result || current_row >= scan_result->RowCount()) {
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }

  for (uint i = 0; i < table->s->fields; i++) {
    Field *f = table->field[i];
    auto val = scan_result->GetValue(i, current_row);

    if (val.IsNull()) {
      f->set_null();
      continue;
    }

    enum_field_types t = f->type();

    // integer family
    if (t == MYSQL_TYPE_TINY || t == MYSQL_TYPE_SHORT ||
        t == MYSQL_TYPE_INT24 || t == MYSQL_TYPE_LONG) {
      f->store((longlong)val.GetValue<int32_t>(), false);
    }

    // bigint
    else if (t == MYSQL_TYPE_LONGLONG) {
      f->store((longlong)val.GetValue<int64_t>(), false);
    }

    // numeric
    else if (t == MYSQL_TYPE_FLOAT || t == MYSQL_TYPE_DOUBLE) {
      f->store(val.GetValue<double>());
    }

    // everything else → treat as string
    else {
      std::string s = val.ToString();
      f->store(s.c_str(), s.length(), f->charset());
    }
  }

  current_row++;
  DBUG_RETURN(0);
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

    std::string s3_path = flush_remaining_rows_to_s3();

    if (s3_path.empty() == true) {
      parquet_trx_data *trx = (parquet_trx_data *) thd_get_ha_data(thd, ht);

      if (trx) {
        trx->s3_file_paths.push_back(s3_path);
      }
    }
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
