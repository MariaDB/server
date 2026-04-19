#define MYSQL_SERVER 1


#include "ha_parquet.h"
#include "ha_parquet_pushdown.h"
#include "parquet_shared.h"
#include "sql_class.h"
#include "handler.h"
#include "duckdb.hpp"
#include <iostream>
#include <curl/curl.h>
#include <mutex>
#include <cstdio>
#include <cctype>


static std::mutex g_duckdb_mutex;
handlerton *parquet_hton = 0;
static THR_LOCK parquet_lock;
static duckdb::DuckDB *g_duckdb = nullptr;
static int ha_parquet_commit(THD *thd, bool all);


ha_parquet::ha_parquet(handlerton *hton, TABLE_SHARE *table_arg) : handler(hton, table_arg)
{
 thr_lock_data_init(&parquet_lock, &lock, NULL);
}


ulonglong ha_parquet::table_flags() const { return HA_FILE_BASED; }
ulong ha_parquet::index_flags(uint, uint, bool) const { return 0; }


int ha_parquet::open(const char *name, int, uint)
{
 DBUG_ENTER("ha_parquet::open");
 row_count = 0;
 flush_threshold = 100;
 duckdb_initialized = false;
 buffer_table_created = false;


 std::string table_path(name);
 parquet_file_path = table_path + ".parquet";
 size_t pos = table_path.find_last_of("/\\");
 if (pos == std::string::npos)
   helper_db_path = "duckdb_helper.duckdb";
 else
   helper_db_path = table_path.substr(0, pos) + "/duckdb_helper.duckdb";


 duckdb_initialized = true;
 parquet_log_info("handler open table='" + std::string(name) +
                  "' parquet_file='" + parquet_file_path +
                  "' helper_db='" + helper_db_path + "'");
 DBUG_RETURN(0);
}


int ha_parquet::close(void) { return 0; }


static std::string item_to_sql(const Item *item)
{
 if (!item) return "";
 const Item_func *func = dynamic_cast<const Item_func *>(item);
 if (!func || func->argument_count() != 2) return "";


 const Item *left = func->arguments()[0];
 const Item *right = func->arguments()[1];


 if (left->type() != Item::FIELD_ITEM) return "";


 const Item_field *field = static_cast<const Item_field *>(left);
 std::string col = field->field_name.str;
 std::string val;


 if (right->type() == Item::CONST_ITEM) {
   String tmp;
   String *s = const_cast<Item*>(right)->val_str(&tmp);
   if (!s) return "";
   std::string strval(s->ptr(), s->length());
   bool is_number = !strval.empty() &&
                    (isdigit((unsigned char)strval[0]) || strval[0] == '-');
   val = is_number ? strval : quote_string_literal(strval);
 } else {
   return "";
 }


 std::string op = func->func_name();
 if (op == "=" || op == "eq") return col + " = " + val;
 if (op == ">" || op == "gt") return col + " > " + val;
 if (op == "<" || op == "lt") return col + " < " + val;
 return "";
}


const Item *ha_parquet::cond_push(const Item *cond)
{
 DBUG_ENTER("ha_parquet::cond_push");
 pushed_cond_sql.clear();
 has_pushed_cond = false;
 if (cond) {
   pushed_cond_sql = item_to_sql(cond);
   if (!pushed_cond_sql.empty()) has_pushed_cond = true;
 }
 DBUG_RETURN(nullptr);
}


void ha_parquet::cond_pop()
{
 pushed_cond_sql.clear();
 has_pushed_cond = false;
}


static std::string mariadb_type_to_duckdb(Field *f)
{
 switch (f->type()) {
   case MYSQL_TYPE_TINY:     return "TINYINT";
   case MYSQL_TYPE_SHORT:    return "SMALLINT";
   case MYSQL_TYPE_INT24:
   case MYSQL_TYPE_LONG:     return "INTEGER";
   case MYSQL_TYPE_LONGLONG: return "BIGINT";
   case MYSQL_TYPE_FLOAT:    return "FLOAT";
   case MYSQL_TYPE_DOUBLE:   return "DOUBLE";
   case MYSQL_TYPE_DECIMAL:
   case MYSQL_TYPE_NEWDECIMAL: return "DECIMAL";
   case MYSQL_TYPE_VARCHAR:
   case MYSQL_TYPE_VAR_STRING:
   case MYSQL_TYPE_STRING:
   case MYSQL_TYPE_ENUM:
   case MYSQL_TYPE_SET:      return "VARCHAR";
   case MYSQL_TYPE_TINY_BLOB:
   case MYSQL_TYPE_MEDIUM_BLOB:
   case MYSQL_TYPE_LONG_BLOB:
   case MYSQL_TYPE_BLOB:
     return (f->charset() == &my_charset_bin) ? "BLOB" : "VARCHAR";
   case MYSQL_TYPE_DATE:
   case MYSQL_TYPE_NEWDATE:  return "DATE";
   case MYSQL_TYPE_TIME:
   case MYSQL_TYPE_TIME2:    return "TIME";
   case MYSQL_TYPE_DATETIME:
   case MYSQL_TYPE_DATETIME2:
   case MYSQL_TYPE_TIMESTAMP:
   case MYSQL_TYPE_TIMESTAMP2: return "TIMESTAMP";
   case MYSQL_TYPE_YEAR:     return "SMALLINT";
   case MYSQL_TYPE_BIT:      return "BOOLEAN";
   default:                  return "";
 }
}


static std::string build_query_create(const std::string &table_name, TABLE *table_arg)
{
 std::string query = "CREATE TABLE IF NOT EXISTS " + table_name + " (";
 bool first = true;
 for (Field **field = table_arg->s->field; *field; ++field) {
   Field *f = *field;
   std::string duck_type = mariadb_type_to_duckdb(f);
   if (duck_type.empty()) return "";
   if (!first) query += ", ";
   first = false;
   query += std::string(f->field_name.str) + " " + duck_type;
 }
 query += ")";
 return query;
}


static std::string build_copy_to_parquet_query(const std::string &table_name,
                                              const std::string &parquet_file)
{
 return "COPY " + table_name + " TO " +
        quote_string_literal(parquet_file) + " (FORMAT PARQUET)";
}

static std::string build_select_union_copy_to_parquet_query(
    const std::string &existing_parquet_file,
    const std::string &buffer_table_name,
    const std::string &target_parquet_file)
{
 return "COPY (SELECT * FROM read_parquet(" +
        quote_string_literal(existing_parquet_file) + ") UNION ALL SELECT * FROM " +
        buffer_table_name + ") TO " +
        quote_string_literal(target_parquet_file) + " (FORMAT PARQUET)";
}

static bool needs_quoting(Field *f)
{
 switch (f->type()) {
   case MYSQL_TYPE_VARCHAR: case MYSQL_TYPE_VAR_STRING: case MYSQL_TYPE_STRING:
   case MYSQL_TYPE_ENUM: case MYSQL_TYPE_SET:
   case MYSQL_TYPE_TINY_BLOB: case MYSQL_TYPE_MEDIUM_BLOB:
   case MYSQL_TYPE_LONG_BLOB: case MYSQL_TYPE_BLOB:
   case MYSQL_TYPE_DATE: case MYSQL_TYPE_NEWDATE:
   case MYSQL_TYPE_TIME: case MYSQL_TYPE_TIME2:
   case MYSQL_TYPE_DATETIME: case MYSQL_TYPE_DATETIME2:
   case MYSQL_TYPE_TIMESTAMP: case MYSQL_TYPE_TIMESTAMP2:
     return true;
   default: return false;
 }
}


int ha_parquet::create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info)
{
  (void) create_info;

  std::string table_path(name);
  if (helper_db_path.empty()) {
    size_t pos = table_path.find_last_of("/\\");
    helper_db_path = (pos == std::string::npos) ?
      "duckdb_helper.duckdb" :
      table_path.substr(0, pos) + "/duckdb_helper.duckdb";
  }

  std::string parquet_file = std::string(name) + ".parquet";
  parquet_file_path = parquet_file;
  std::string table_name = table_name_from_path(table_path);

  std::string buf_name = "buf_" + table_name;
  std::string query = build_query_create(buf_name, table_arg);
  if (query.empty()) return HA_ERR_UNSUPPORTED;

  try {
    std::lock_guard<std::mutex> lock(g_duckdb_mutex);
    duckdb::Connection *con = new duckdb::Connection(*g_duckdb);
    parquet_log_info("DuckDB create buffer connection table='" + table_name +
                     "'");
    parquet_log_info("DuckDB query [create/install] INSTALL parquet;");
    con->Query("INSTALL parquet;");
    parquet_log_info("DuckDB query [create/load] LOAD parquet;");
    con->Query("LOAD parquet;");

    parquet_log_info("DuckDB query [create/buffer-table] " +
                     parquet_log_preview(query));
    auto create_result = con->Query(query);
    if (!create_result || create_result->HasError()) {
      std::cerr << "DuckDB CREATE error: " << create_result->GetError() << std::endl;
      delete con;
      return HA_ERR_INTERNAL_ERROR;
    }

    const std::string copy_query =
        build_copy_to_parquet_query(buf_name, parquet_file);
    parquet_log_info("DuckDB query [create/seed-parquet] " +
                     parquet_log_preview(copy_query));
    auto copy_result = con->Query(copy_query);
    if (!copy_result || copy_result->HasError()) {
      std::cerr << "DuckDB COPY error: " << copy_result->GetError() << std::endl;
      delete con;
      return HA_ERR_INTERNAL_ERROR;
    }

    parquet_log_info("DuckDB create table seed complete table='" + table_name +
                     "' parquet_file='" + parquet_file + "'");

    delete con;
  } catch (const std::exception &e) {
    std::cerr << "create exception: " << e.what() << std::endl;
    return HA_ERR_INTERNAL_ERROR;
  }

  // register table in LakeKeeper Iceberg catalog
  const std::string lakekeeper_url = lakekeeper_table_collection_url();

  std::string fields = "";
  int field_id = 1;
  bool first = true;
  for (Field **field = table_arg->s->field; *field; ++field) {
    Field *f = *field;
    std::string iceberg_type;
    switch (f->type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:     iceberg_type = "int";    break;
      case MYSQL_TYPE_LONGLONG: iceberg_type = "long";   break;
      case MYSQL_TYPE_FLOAT:    iceberg_type = "float";  break;
      case MYSQL_TYPE_DOUBLE:   iceberg_type = "double"; break;
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_STRING:   iceberg_type = "string"; break;
      default:                  iceberg_type = "string"; break;
    }
    if (!first) fields += ",";
    first = false;
    char field_json[256];
    snprintf(field_json, sizeof(field_json),
      "{\"id\":%d,\"name\":\"%s\",\"required\":false,\"type\":\"%s\"}",
      field_id++, f->field_name.str, iceberg_type.c_str());
    fields += field_json;
  }

  char json_body[4096];
  snprintf(json_body, sizeof(json_body),
    "{"
      "\"name\": \"%s\","
      "\"schema\": {"
        "\"type\": \"struct\","
        "\"schema-id\": 0,"
        "\"fields\": [%s]"
      "}"
    "}",
    table_name.c_str(),
    fields.c_str()
  );

  CURL *curl = curl_easy_init();
  if (curl) {
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, lakekeeper_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    parquet_log_info("LakeKeeper create table begin table='" + table_name +
                     "' url='" + lakekeeper_url + "' payload=" +
                     parquet_log_preview(json_body));
    // suppress response output to terminal
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
      +[](char*, size_t s, size_t n, void*) -> size_t { return s*n; });
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
      std::cerr << "LakeKeeper create table error: " << curl_easy_strerror(res) << std::endl;
      return HA_ERR_INTERNAL_ERROR;
    } else if (http_code != 200 && http_code != 201 && http_code != 409) {
      std::cerr << "LakeKeeper create table HTTP error: " << http_code << std::endl;
      return HA_ERR_INTERNAL_ERROR;
    }

    parquet_log_info("LakeKeeper create table complete table='" + table_name +
                     "' http_status=" + std::to_string(http_code));
  }

  return 0;
}

int ha_parquet::delete_table(const char *name)
{
 DBUG_ENTER("ha_parquet::delete_table");

 const std::string table_path(name);
 const std::string table_name = table_name_from_path(table_path);

 const std::string lakekeeper_url = lakekeeper_table_url(table_name);

 CURL *curl = curl_easy_init();
 if (curl) {
   parquet_log_info("LakeKeeper delete table begin table='" + table_name +
                    "' url='" + lakekeeper_url + "'");
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
   curl_easy_setopt(curl, CURLOPT_URL, lakekeeper_url.c_str());
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
     +[](char*, size_t s, size_t n, void*) -> size_t { return s * n; });
   CURLcode res = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_easy_cleanup(curl);

   if (res != CURLE_OK) {
     std::cerr << "LakeKeeper delete table error: " << curl_easy_strerror(res) << std::endl;
     DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
   }

   if (http_code != 200 && http_code != 204 && http_code != 404) {
     std::cerr << "LakeKeeper delete table HTTP error: " << http_code << std::endl;
     DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
   }

   parquet_log_info("LakeKeeper delete table complete table='" + table_name +
                    "' http_status=" + std::to_string(http_code));
 }

 std::remove((table_path + ".parquet").c_str());
 DBUG_RETURN(0);
}

void ha_parquet::flush_remaining_rows_to_s3(parquet_trx_data *trx)
{
  if (row_count == 0 || !trx) return;

  std::string buf_name = "buf_" + std::string(table_share->table_name.str);
  const std::string existing_snapshot_file =
    fetch_current_snapshot_data_file(table_share->table_name.str);

  static uint64_t flush_counter = 0;
  std::string s3_path = parquet_s3_object_path(
    "part_" + std::to_string(time(nullptr)) + "_" +
    std::to_string(flush_counter++) + ".parquet");
  parquet_log_info("DuckDB flush begin table='" +
                   std::string(table_share->table_name.str) + "' rows=" +
                   std::to_string(row_count) + " target='" + s3_path +
                   "' existing_snapshot='" +
                   (existing_snapshot_file.empty() ? std::string("<empty>")
                                                   : existing_snapshot_file) +
                   "'");

  try {
    std::lock_guard<std::mutex> lock(g_duckdb_mutex);
    duckdb::Connection *con = new duckdb::Connection(*g_duckdb);
    parquet_log_info("DuckDB query [flush/install] INSTALL parquet;");
    con->Query("INSTALL parquet;");
    parquet_log_info("DuckDB query [flush/load] LOAD parquet;");
    con->Query("LOAD parquet;");
    parquet_log_info("DuckDB query [flush/install-httpfs] INSTALL httpfs;");
    con->Query("INSTALL httpfs;");
    parquet_log_info("DuckDB query [flush/load-httpfs] LOAD httpfs;");
    con->Query("LOAD httpfs;");
    configure_duckdb_s3(con);

    std::string copy_query = existing_snapshot_file.empty()
      ? build_copy_to_parquet_query(buf_name, s3_path)
      : build_select_union_copy_to_parquet_query(existing_snapshot_file, buf_name, s3_path);

    parquet_log_info("DuckDB query [flush/copy] " +
                     parquet_log_preview(copy_query));
    auto copy_result = con->Query(copy_query);
    if (!copy_result || copy_result->HasError()) {
      std::cerr << "flush COPY error: " << copy_result->GetError() << std::endl;
      delete con;
      return;
    }

    int64_t file_size = 0;
    const std::string metadata_query =
      "SELECT SUM(total_compressed_size) FROM parquet_metadata('" + s3_path + "')";
    parquet_log_info("DuckDB query [flush/metadata] " +
                     parquet_log_preview(metadata_query));
    auto meta_result = con->Query(metadata_query);
    if (meta_result && !meta_result->HasError() && meta_result->RowCount() > 0)
      file_size = meta_result->GetValue(0, 0).GetValue<int64_t>();

    parquet_log_info("DuckDB query [flush/clear-buffer] DELETE FROM " + buf_name);
    con->Query("DELETE FROM " + buf_name);
    delete con;

    trx->s3_file_paths.push_back(s3_path);
    trx->row_counts.push_back((int64_t)row_count);
    trx->file_sizes.push_back(file_size);
    parquet_log_info("S3 staged object path='" + s3_path + "' table='" +
                     std::string(table_share->table_name.str) + "' rows=" +
                     std::to_string(row_count) + " bytes=" +
                     std::to_string(file_size));

  } catch (const std::exception &e) {
    std::cerr << "flush exception: " << e.what() << std::endl;
    return;
  }

  row_count = 0;
}

int ha_parquet::write_row(const uchar *buf)
{
  DBUG_ENTER("ha_parquet::write_row");

  std::string buf_name = "buf_" + std::string(table->s->table_name.str);
  try {
    std::lock_guard<std::mutex> lock(g_duckdb_mutex);
    duckdb::Connection *con = new duckdb::Connection(*g_duckdb);
    parquet_log_info("DuckDB query [write/install] INSTALL parquet;");
    con->Query("INSTALL parquet;");
    parquet_log_info("DuckDB query [write/load] LOAD parquet;");
    con->Query("LOAD parquet;");

    std::string create_sql = build_query_create(buf_name, table);
    if (create_sql.empty()) { delete con; DBUG_RETURN(HA_ERR_UNSUPPORTED); }
    parquet_log_info("DuckDB query [write/ensure-buffer] " +
                     parquet_log_preview(create_sql));
    con->Query(create_sql);

    std::string insert_sql = "INSERT INTO " + buf_name + " VALUES (";
    bool first_flag = false;

    for (Field **field = table->field; *field; ++field) {
      Field *f = *field;
      if (first_flag) insert_sql += ", ";
      first_flag = true;
      const bool is_null = f->is_null_in_record(buf);
      if (is_null) {
        insert_sql += "NULL";
      } else {
        String val;
        String *res = f->val_str(&val, f->ptr_in_record(buf));
        std::string val_cpp;
        if (res != nullptr)
          val_cpp.assign(res->ptr(), res->length());
        insert_sql += needs_quoting(f) ? quote_string_literal(val_cpp) : val_cpp;
      }
    }

    insert_sql += ")";
    parquet_log_info("DuckDB query [write/insert-buffer-row] " +
                     parquet_log_preview(insert_sql));

    auto result = con->Query(insert_sql);
    if (!result || result->HasError()) {
      std::cerr << "INSERT error: " << result->GetError() << std::endl;
      delete con;
      DBUG_RETURN(HA_ERR_GENERIC);
    }

    row_count++;
    parquet_log_info("DuckDB buffered row table='" +
                     std::string(table->s->table_name.str) + "' row_count=" +
                     std::to_string(row_count));
    delete con;

  } catch (const duckdb::Exception &e) {
    std::cerr << "write_row DuckDB exception: " << e.what() << std::endl;
    DBUG_RETURN(HA_ERR_GENERIC);
  } catch (const std::exception &e) {
    std::cerr << "write_row std exception: " << e.what() << std::endl;
    DBUG_RETURN(HA_ERR_GENERIC);
  }

  DBUG_RETURN(0);
}

int ha_parquet::update_row(const uchar *, const uchar *) { return HA_ERR_WRONG_COMMAND; }
int ha_parquet::delete_row(const uchar *) { return HA_ERR_WRONG_COMMAND; }


int ha_parquet::rnd_init(bool scan)
{
  (void) scan;
  DBUG_ENTER("ha_parquet::rnd_init");

  current_row = 0;
  scan_result.reset();

  std::string response_body;
  long http_code = 0;
  if (!fetch_lakekeeper_table_metadata(table->s->table_name.str, &response_body, &http_code)) {
    std::cerr << "rnd_init: failed to fetch LakeKeeper metadata" << std::endl;
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }
  if (http_code != 200) {
    std::cerr << "rnd_init: LakeKeeper table load HTTP error: " << http_code << std::endl;
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  std::vector<std::string> s3_files = extract_manifest_paths(response_body);

  if (s3_files.empty()) {
    std::cerr << "rnd_init: no S3 files found in LakeKeeper" << std::endl;
    DBUG_RETURN(0);
  }

  std::string parquet_file_list = "[";
  for (size_t i = 0; i < s3_files.size(); ++i) {
    if (i != 0) parquet_file_list += ", ";
    parquet_file_list += quote_string_literal(s3_files[i]);
  }
  parquet_file_list += "]";

  std::string query = "SELECT * FROM read_parquet(" + parquet_file_list + ")";
  parquet_log_info("DuckDB read init table='" +
                   std::string(table->s->table_name.str) + "' files=" +
                   std::to_string(s3_files.size()));

  try {
    std::lock_guard<std::mutex> lock(g_duckdb_mutex);
    duckdb::Connection *con = new duckdb::Connection(*g_duckdb);
    parquet_log_info("DuckDB query [read/install] INSTALL parquet;");
    con->Query("INSTALL parquet;");
    parquet_log_info("DuckDB query [read/load] LOAD parquet;");
    con->Query("LOAD parquet;");
    parquet_log_info("DuckDB query [read/install-httpfs] INSTALL httpfs;");
    con->Query("INSTALL httpfs;");
    parquet_log_info("DuckDB query [read/load-httpfs] LOAD httpfs;");
    con->Query("LOAD httpfs;");
    configure_duckdb_s3(con);

    parquet_log_info("DuckDB query [read/scan] " +
                     parquet_log_preview(query));
    auto result = con->Query(query);
    if (!result || result->HasError()) {
      std::cerr << "rnd_init read error: " << result->GetError() << std::endl;
      delete con;
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    scan_result = std::move(result);
    delete con;

  } catch (const std::exception &e) {
    std::cerr << "rnd_init exception: " << e.what() << std::endl;
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  DBUG_RETURN(0);
}

int ha_parquet::rnd_next(uchar *buf)
{
 (void) buf;
 DBUG_ENTER("ha_parquet::rnd_next");


 if (!scan_result || current_row >= scan_result->RowCount())
   DBUG_RETURN(HA_ERR_END_OF_FILE);


 for (uint i = 0; i < table->s->fields; i++) {
   Field *f = table->field[i];
   auto val = scan_result->GetValue(i, current_row);
   std::string val_string = val.ToString();

   if (val.IsNull()) {
     f->set_null();
     continue;
   }

   f->set_notnull();


   enum_field_types t = f->type();
   if (t == MYSQL_TYPE_TINY || t == MYSQL_TYPE_SHORT ||
       t == MYSQL_TYPE_INT24 || t == MYSQL_TYPE_LONG)
     f->store((longlong)val.GetValue<int32_t>(), false);
   else if (t == MYSQL_TYPE_LONGLONG)
     f->store((longlong)val.GetValue<int64_t>(), false);
   else if (t == MYSQL_TYPE_FLOAT || t == MYSQL_TYPE_DOUBLE)
     f->store(val.GetValue<double>());
   else {
     std::string s = val_string;
     f->store(s.c_str(), s.length(), f->charset());
   }
 }


 current_row++;
 DBUG_RETURN(0);
}


int ha_parquet::rnd_pos(uchar *, uchar *) { return HA_ERR_WRONG_COMMAND; }
void ha_parquet::position(const uchar *) {}
int ha_parquet::info(uint) { return 0; }


enum_alter_inplace_result
ha_parquet::check_if_supported_inplace_alter(TABLE *, Alter_inplace_info *)
{ return HA_ALTER_INPLACE_NOT_SUPPORTED; }


int ha_parquet::external_lock(THD *thd, int lock_type)
{
 DBUG_ENTER("ha_parquet::external_lock");


 if (lock_type == F_RDLCK || lock_type == F_WRLCK) {
   trans_register_ha(thd, true, parquet_hton, 0);
   parquet_trx_data *trx = (parquet_trx_data *) thd_get_ha_data(thd, parquet_hton);
   if (trx == NULL) {
     trx = new parquet_trx_data();
     trx->table_name = table_share->table_name.str;
     thd_set_ha_data(thd, parquet_hton, trx);
   }
 } else if (lock_type == F_UNLCK) {
   parquet_trx_data *trx = (parquet_trx_data *) thd_get_ha_data(thd, parquet_hton);
   flush_remaining_rows_to_s3(trx);
   if (trx && !trx->s3_file_paths.empty())
     ha_parquet_commit(thd, true);
 }


 DBUG_RETURN(0);
}


THR_LOCK_DATA **ha_parquet::store_lock(THD *thd, THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type)
{
 if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
   lock.type = lock_type;
 *to++ = &lock;
 return to;
}


static handler *parquet_create_handler(handlerton *p_hton, TABLE_SHARE *table,
                                       MEM_ROOT *mem_root)
{ return new (mem_root) ha_parquet(p_hton, table); }


static int ha_parquet_commit(THD *thd, bool all)
{
 const auto &config = parquet_runtime_config();
 parquet_trx_data *trx =
   (parquet_trx_data *) thd_get_ha_data(thd, parquet_hton);


 if (!trx || trx->s3_file_paths.empty()) return 0;


 for (size_t i = 0; i < trx->s3_file_paths.size(); i++) {
   const std::string &s3_path = trx->s3_file_paths[i];


   static long long snapshot_counter = 0;
   long long snapshot_id = (long long)time(NULL) * 1000 + (snapshot_counter++ % 1000);

   char json_body[4096];
   snprintf(json_body, sizeof(json_body),
     "{"
       "\"table-changes\": [{"
         "\"identifier\": {\"namespace\": [\"%s\"], \"name\": \"%s\"},"
         "\"requirements\": [],"
         "\"updates\": ["
           "{\"action\": \"add-snapshot\", \"snapshot\": {"
             "\"snapshot-id\": %lld, \"sequence-number\": 1,"
             "\"timestamp-ms\": %lld,"
             "\"summary\": {\"operation\": \"append\","
               "\"added-data-files\": \"1\","
               "\"added-records\": \"%lld\","
               "\"added-files-size\": \"%lld\"},"
             "\"schema-id\": 0, \"manifest-list\": \"%s\"}},"
           "{\"action\": \"set-snapshot-ref\", \"ref-name\": \"main\","
             "\"type\": \"branch\", \"snapshot-id\": %lld}"
         "]"
       "}]"
     "}",
     config.lakekeeper_namespace.c_str(),
     trx->table_name.c_str(), snapshot_id, snapshot_id * 1000,
     trx->row_counts[i],
     trx->file_sizes[i],
     s3_path.c_str(), snapshot_id);


   CURL *curl = curl_easy_init();
   if (!curl) { delete trx; thd_set_ha_data(thd, parquet_hton, nullptr); return 1; }


   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/json");
   curl_easy_setopt(curl, CURLOPT_POST, 1L);
   const std::string lakekeeper_url = lakekeeper_transaction_commit_url();
   curl_easy_setopt(curl, CURLOPT_URL, lakekeeper_url.c_str());
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   parquet_log_info("LakeKeeper commit begin table='" + trx->table_name +
                    "' snapshot_id=" + std::to_string(snapshot_id) +
                    " manifest='" + s3_path + "' url='" + lakekeeper_url +
                    "' payload=" + parquet_log_preview(json_body));


   CURLcode res = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_slist_free_all(headers);
   curl_easy_cleanup(curl);


   if (res != CURLE_OK) {
     std::cerr << "LakeKeeper commit error: " << curl_easy_strerror(res) << std::endl;
     delete trx; thd_set_ha_data(thd, parquet_hton, nullptr); return 1;
   }
   if (http_code != 200) {
     std::cerr << "LakeKeeper commit HTTP error: " << http_code << std::endl;
     delete trx; thd_set_ha_data(thd, parquet_hton, nullptr); return 1;
   }

   parquet_log_info("LakeKeeper commit complete table='" + trx->table_name +
                    "' snapshot_id=" + std::to_string(snapshot_id) +
                    " http_status=" + std::to_string(http_code));
 }


 delete trx;
 thd_set_ha_data(thd, parquet_hton, nullptr);
 return 0;
}


static int ha_parquet_rollback(THD *thd, bool all)
{
 parquet_trx_data *trx =
   (parquet_trx_data *) thd_get_ha_data(thd, parquet_hton);


 if (!trx) return 0;


 // Orphaned S3 files are not registered in LakeKeeper
 // and are invisible to all query engines.
 // Production cleanup is handled by S3 lifecycle policies
 // or LakeKeeper's built-in garbage collection.
 for (const std::string &s3_path : trx->s3_file_paths)
   parquet_log_warning("rollback orphaned staged object path='" + s3_path +
                       "' will rely on external GC");


 delete trx;
 thd_set_ha_data(thd, parquet_hton, nullptr);
 return 0;
}


static int ha_parquet_init(void *p)
{
 parquet_hton = (handlerton *) p;
 parquet_hton->create   = parquet_create_handler;
 parquet_hton->create_select = create_duckdb_select_handler;
 parquet_hton->commit   = ha_parquet_commit;
 parquet_hton->rollback = ha_parquet_rollback;
 thr_lock_init(&parquet_lock);


 g_duckdb = new duckdb::DuckDB(nullptr);
 parquet_log_info("DuckDB global instance initialized");


 return 0;
}


static int ha_parquet_deinit(void *p)
{
 delete g_duckdb;
 g_duckdb = nullptr;
 parquet_log_info("DuckDB global instance deinitialized");
 parquet_hton = 0;
 thr_lock_delete(&parquet_lock);
 return 0;
}


struct st_mysql_storage_engine parquet_storage_engine =
{ MYSQL_HANDLERTON_INTERFACE_VERSION };


maria_declare_plugin(parquet)
{
 MYSQL_STORAGE_ENGINE_PLUGIN,
 &parquet_storage_engine,
 "PARQUET",
 "UIUC Disruption Lab",
 "Parquet Storage Engine",
 PLUGIN_LICENSE_GPL,
 ha_parquet_init,
 ha_parquet_deinit,
 0x0100,
 NULL,
 NULL,
 "1.0",
 MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
