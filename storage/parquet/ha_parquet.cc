#define MYSQL_SERVER 1


#include "ha_parquet.h"
#include "ha_parquet_pushdown.h"
#include "parquet_metadata.h"
#include "parquet_object_store.h"
#include "parquet_shared.h"
#include "parquet_transaction.h"
#include "duckdb.hpp"
#include "handler.h"
#include "my_sys.h"
#include "sql_class.h"

#include <json.hpp>
#include <curl/curl.h>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>


static std::mutex g_duckdb_mutex;
handlerton *parquet_hton = 0;
static THR_LOCK parquet_lock;
static duckdb::DuckDB *g_duckdb = nullptr;
static int ha_parquet_commit(THD *thd, bool all);

struct ha_table_option_struct
{
  char *catalog;
  char *connection;
};

namespace {

using json = nlohmann::json;

static ha_create_table_option parquet_table_option_list[] = {
    HA_TOPTION_STRING("CATALOG", catalog),
    HA_TOPTION_STRING("CONNECTION", connection),
    HA_TOPTION_END};

static char *parquet_tmp_lakekeeper_bearer_token = 0;
static char *parquet_tmp_s3_access_key_id = 0;
static char *parquet_tmp_s3_secret_access_key = 0;

static void update_lakekeeper_bearer_token(MYSQL_THD,
                                           struct st_mysql_sys_var *,
                                           void *, const void *)
{
  my_free(parquet_lakekeeper_bearer_token);
  parquet_lakekeeper_bearer_token = 0;
  if (parquet_tmp_lakekeeper_bearer_token &&
      parquet_tmp_lakekeeper_bearer_token[0]) {
    parquet_lakekeeper_bearer_token = parquet_tmp_lakekeeper_bearer_token;
    parquet_tmp_lakekeeper_bearer_token =
        my_strdup(PSI_NOT_INSTRUMENTED, "*****", MYF(MY_WME));
  }
}

static void update_s3_access_key_id(MYSQL_THD, struct st_mysql_sys_var *,
                                    void *, const void *)
{
  my_free(parquet_s3_access_key_id);
  parquet_s3_access_key_id = 0;
  if (parquet_tmp_s3_access_key_id && parquet_tmp_s3_access_key_id[0]) {
    parquet_s3_access_key_id = parquet_tmp_s3_access_key_id;
    parquet_tmp_s3_access_key_id =
        my_strdup(PSI_NOT_INSTRUMENTED, "*****", MYF(MY_WME));
  }
}

static void update_s3_secret_access_key(MYSQL_THD, struct st_mysql_sys_var *,
                                        void *, const void *)
{
  my_free(parquet_s3_secret_access_key);
  parquet_s3_secret_access_key = 0;
  if (parquet_tmp_s3_secret_access_key &&
      parquet_tmp_s3_secret_access_key[0]) {
    parquet_s3_secret_access_key = parquet_tmp_s3_secret_access_key;
    parquet_tmp_s3_secret_access_key =
        my_strdup(PSI_NOT_INSTRUMENTED, "*****", MYF(MY_WME));
  }
}

static MYSQL_SYSVAR_STR(lakekeeper_base_url, parquet_lakekeeper_base_url,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "LakeKeeper catalog base URL", 0, 0,
                        "http://localhost:8181/catalog/v1/");
static MYSQL_SYSVAR_STR(lakekeeper_warehouse_id,
                        parquet_lakekeeper_warehouse_id,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "LakeKeeper warehouse ID", 0, 0, "");
static MYSQL_SYSVAR_STR(lakekeeper_namespace, parquet_lakekeeper_namespace,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "LakeKeeper default namespace", 0, 0, "default");
static MYSQL_SYSVAR_STR(lakekeeper_bearer_token,
                        parquet_tmp_lakekeeper_bearer_token,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY |
                            PLUGIN_VAR_MEMALLOC,
                        "LakeKeeper bearer token", 0,
                        update_lakekeeper_bearer_token, "");
static MYSQL_SYSVAR_STR(s3_bucket, parquet_s3_bucket,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Default Parquet object-store bucket", 0, 0, "");
static MYSQL_SYSVAR_STR(s3_data_prefix, parquet_s3_data_prefix,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Default Parquet object-store key prefix", 0, 0,
                        "data");
static MYSQL_SYSVAR_STR(s3_region, parquet_s3_region,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Default Parquet object-store region", 0, 0,
                        "us-east-2");
static MYSQL_SYSVAR_STR(s3_access_key_id, parquet_tmp_s3_access_key_id,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY |
                            PLUGIN_VAR_MEMALLOC,
                        "Parquet object-store access key ID", 0,
                        update_s3_access_key_id, "");
static MYSQL_SYSVAR_STR(s3_secret_access_key,
                        parquet_tmp_s3_secret_access_key,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY |
                            PLUGIN_VAR_MEMALLOC,
                        "Parquet object-store secret access key", 0,
                        update_s3_secret_access_key, "");

static struct st_mysql_sys_var *parquet_system_variables[] = {
    MYSQL_SYSVAR(lakekeeper_base_url),
    MYSQL_SYSVAR(lakekeeper_warehouse_id),
    MYSQL_SYSVAR(lakekeeper_namespace),
    MYSQL_SYSVAR(lakekeeper_bearer_token),
    MYSQL_SYSVAR(s3_bucket),
    MYSQL_SYSVAR(s3_data_prefix),
    MYSQL_SYSVAR(s3_region),
    MYSQL_SYSVAR(s3_access_key_id),
    MYSQL_SYSVAR(s3_secret_access_key),
    NULL};

const ha_table_option_struct *parquet_table_options(HA_CREATE_INFO *create_info)
{
  return create_info != nullptr
             ? (const ha_table_option_struct *) create_info->option_struct
             : nullptr;
}

bool raise_unknown_error(const std::string &message)
{
  my_printf_error(ER_UNKNOWN_ERROR, "%s", MYF(0), message.c_str());
  return false;
}

bool raise_create_option_error(const std::string &message)
{
  my_printf_error(ER_ILLEGAL_HA_CREATE_OPTION, "%s", MYF(0),
                  message.c_str());
  return false;
}

bool resolve_runtime_metadata_or_error(const char *table_path,
                                       parquet::TableMetadata *metadata)
{
  std::string error;
  if (!parquet::ResolveRuntimeTableMetadata(table_path, metadata, &error)) {
    raise_unknown_error(error);
    return false;
  }
  return true;
}

bool validate_catalog_or_error(const parquet::TableMetadata &metadata)
{
  std::string error;
  if (!parquet::ValidateCatalogConfig(metadata, &error)) {
    raise_unknown_error(error);
    return false;
  }
  return true;
}

bool validate_object_store_or_error(const parquet::TableMetadata &metadata)
{
  std::string error;
  if (!parquet::ValidateObjectStoreConfig(metadata, true, &error)) {
    raise_unknown_error(error);
    return false;
  }
  return true;
}

void apply_catalog_request_options(
    const parquet::CatalogClientConfig &config, CURL *curl,
    struct curl_slist **headers)
{
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, config.connect_timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config.timeout_ms);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, config.verify_peer ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, config.verify_host ? 2L : 0L);

  if (headers != nullptr && !config.bearer_token.empty()) {
    *headers = curl_slist_append(
        *headers, ("Authorization: Bearer " + config.bearer_token).c_str());
  }
}

json namespace_json_array(const parquet::CatalogNamespaceIdent &ident)
{
  json namespace_parts = json::array();
  for (const auto &part : ident.parts) {
    namespace_parts.push_back(part);
  }
  return namespace_parts;
}

} // namespace


ha_parquet::ha_parquet(handlerton *hton, TABLE_SHARE *table_arg) : handler(hton, table_arg)
{
 thr_lock_data_init(&parquet_lock, &lock, NULL);
}


ulonglong ha_parquet::table_flags() const { return HA_FILE_BASED; }
ulong ha_parquet::index_flags(uint, uint, bool) const { return 0; }


int ha_parquet::open(const char *name, int, uint)
{
 DBUG_ENTER("ha_parquet::open");
 duckdb_initialized = false;


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
   if (!pushed_cond_sql.empty()) {
    has_pushed_cond = true;
    DBUG_RETURN(nullptr);
   }
   
 }
 DBUG_RETURN(cond);
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

static bool parquet_is_real_commit(THD *thd, bool all)
{
  return ((all || thd->transaction->all.ha_list == 0) &&
          !(thd->variables.option_bits & OPTION_GTID_BEGIN));
}

static bool parquet_is_real_rollback(THD *thd, bool all)
{
  return all || thd->transaction->all.ha_list == 0;
}

static uint64_t parquet_table_hash(const std::string &table_path)
{
  return static_cast<uint64_t>(std::hash<std::string>{}(table_path));
}

static std::string parquet_statement_buffer_name(THD *thd,
                                                 const std::string &table_path)
{
  return "buf_stmt_" + std::to_string(static_cast<unsigned long long>(
                           thd->thread_id)) +
         "_" +
         std::to_string(static_cast<unsigned long long>(thd->query_id)) +
         "_" + std::to_string(static_cast<unsigned long long>(
                   parquet_table_hash(table_path)));
}

static parquet_table_trx_data *parquet_table_txn_for_share(
    parquet_trx_data *trx, THD *thd, TABLE_SHARE *share)
{
  if (trx == nullptr || thd == nullptr || share == nullptr) {
    return nullptr;
  }

  const std::string table_path = share->normalized_path.str;
  auto &table_trx = trx->tables[table_path];
  if (table_trx.table_path.empty()) {
    table_trx.table_path = table_path;
    table_trx.table_name = share->table_name.str;
  }
  return &table_trx;
}

static bool parquet_drop_duckdb_table(const std::string &table_name)
{
  if (table_name.empty()) {
    return true;
  }

  duckdb::Connection *con = nullptr;
  try {
    std::lock_guard<std::mutex> lock(g_duckdb_mutex);
    con = new duckdb::Connection(*g_duckdb);
    const std::string drop_query = "DROP TABLE IF EXISTS " + table_name;
    parquet_log_info("DuckDB query [txn/drop-buffer] " +
                     parquet_log_preview(drop_query));
    auto result = con->Query(drop_query);
    const bool ok = result && !result->HasError();
    const std::string error_message =
        ok ? std::string() : (result ? result->GetError() : "null result");
    delete con;
    if (!ok) {
      std::cerr << "DuckDB DROP TABLE error: " << error_message
                << std::endl;
    }
    return ok;
  } catch (const std::exception &e) {
    delete con;
    std::cerr << "DuckDB DROP TABLE exception: " << e.what() << std::endl;
    return false;
  }
}

static void parquet_reset_statement_buffer(parquet_table_trx_data *table_trx)
{
  if (table_trx == nullptr) {
    return;
  }

  if (!table_trx->statement_buffer_name.empty()) {
    parquet_log_info("Parquet dropping statement buffer table='" +
                     table_trx->table_name + "' buffer='" +
                     table_trx->statement_buffer_name + "'");
    parquet_drop_duckdb_table(table_trx->statement_buffer_name);
  }
  table_trx->statement_buffer_name.clear();
  table_trx->statement_row_count = 0;
}

static void parquet_remove_local_stage_files(parquet_table_trx_data *table_trx)
{
  if (table_trx == nullptr) {
    return;
  }

  for (const auto &staged_file : table_trx->staged_files) {
    if (!staged_file.local_path.empty()) {
      parquet_log_info("Parquet removing local staged file table='" +
                       table_trx->table_name + "' path='" +
                       staged_file.local_path + "'");
      std::remove(staged_file.local_path.c_str());
    }
  }
  table_trx->staged_files.clear();
}

static uint64_t parquet_next_local_stage_id()
{
  static uint64_t local_stage_counter = 0;
  return static_cast<uint64_t>(time(nullptr)) * 1000ULL +
         (local_stage_counter++ % 1000ULL);
}

static uint64_t parquet_next_remote_stage_id()
{
  static uint64_t remote_stage_counter = 0;
  return static_cast<uint64_t>(time(nullptr)) * 1000ULL +
         (remote_stage_counter++ % 1000ULL);
}

static bool parquet_stage_statement_buffer_to_local(
    parquet_table_trx_data *table_trx, const std::string &canonical_parquet_path,
    std::string *error)
{
  if (table_trx == nullptr || table_trx->statement_row_count == 0 ||
      table_trx->statement_buffer_name.empty()) {
    return true;
  }

  const auto stage_id = parquet_next_local_stage_id();
  const std::string local_stage_path =
      parquet::BuildLocalStagePath(canonical_parquet_path, stage_id);

  duckdb::Connection *con = nullptr;
  try {
    std::lock_guard<std::mutex> lock(g_duckdb_mutex);
    con = new duckdb::Connection(*g_duckdb);
    parquet_log_info("DuckDB query [txn/stage-local/install] INSTALL parquet;");
    con->Query("INSTALL parquet;");
    parquet_log_info("DuckDB query [txn/stage-local/load] LOAD parquet;");
    con->Query("LOAD parquet;");
    std::remove(local_stage_path.c_str());
    const std::string copy_query = build_copy_to_parquet_query(
        table_trx->statement_buffer_name, local_stage_path);
    parquet_log_info("DuckDB query [txn/stage-local/copy] " +
                     parquet_log_preview(copy_query));
    auto copy_result = con->Query(copy_query);
    if (!copy_result || copy_result->HasError()) {
      if (error != nullptr) {
        *error = copy_result ? copy_result->GetError()
                             : "DuckDB returned a null result while staging";
      }
      delete con;
      return false;
    }
    delete con;
  } catch (const std::exception &e) {
    delete con;
    if (error != nullptr) {
      *error = e.what();
    }
    return false;
  }

  table_trx->staged_files.push_back(
      {local_stage_path, table_trx->statement_row_count});
  parquet_log_info("Parquet staged local file table='" + table_trx->table_name +
                   "' path='" + local_stage_path + "' rows=" +
                   std::to_string(table_trx->statement_row_count));
  parquet_reset_statement_buffer(table_trx);
  return true;
}

static std::string parquet_local_stage_list_sql(
    const parquet_table_trx_data &table_trx)
{
  std::string file_list = "[";
  for (size_t i = 0; i < table_trx.staged_files.size(); ++i) {
    if (i != 0) {
      file_list += ", ";
    }
    file_list += quote_string_literal(table_trx.staged_files[i].local_path);
  }
  file_list += "]";
  return file_list;
}

static bool parquet_flush_local_stages_to_s3(
    parquet_table_trx_data *table_trx, const parquet::TableMetadata &metadata,
    std::string *s3_path, int64_t *record_count, int64_t *file_size,
    std::string *error)
{
  if (table_trx == nullptr || table_trx->staged_files.empty()) {
    if (s3_path != nullptr) {
      s3_path->clear();
    }
    if (record_count != nullptr) {
      *record_count = 0;
    }
    if (file_size != nullptr) {
      *file_size = 0;
    }
    return true;
  }

  int64_t total_rows = 0;
  for (const auto &staged_file : table_trx->staged_files) {
    total_rows += static_cast<int64_t>(staged_file.row_count);
  }

  const std::string target_s3_path = parquet_s3_object_path(
      metadata.object_store_config,
      "part_" + std::to_string(parquet_next_remote_stage_id()) + ".parquet");
  const std::string source_query =
      "SELECT * FROM read_parquet(" + parquet_local_stage_list_sql(*table_trx) +
      ")";
  const std::string copy_query =
      "COPY (" + source_query + ") TO " + quote_string_literal(target_s3_path) +
      " (FORMAT PARQUET)";

  duckdb::Connection *con = nullptr;
  int64_t compressed_size = 0;
  try {
    std::lock_guard<std::mutex> lock(g_duckdb_mutex);
    con = new duckdb::Connection(*g_duckdb);
    parquet_log_info("DuckDB query [commit/install] INSTALL parquet;");
    con->Query("INSTALL parquet;");
    parquet_log_info("DuckDB query [commit/load] LOAD parquet;");
    con->Query("LOAD parquet;");
    parquet_log_info("DuckDB query [commit/install-httpfs] INSTALL httpfs;");
    con->Query("INSTALL httpfs;");
    parquet_log_info("DuckDB query [commit/load-httpfs] LOAD httpfs;");
    con->Query("LOAD httpfs;");
    if (!configure_duckdb_s3(con, metadata.object_store_config, error)) {
      delete con;
      return false;
    }

    parquet_log_info("DuckDB query [commit/copy] " +
                     parquet_log_preview(copy_query));
    auto copy_result = con->Query(copy_query);
    if (!copy_result || copy_result->HasError()) {
      if (error != nullptr) {
        *error = copy_result ? copy_result->GetError()
                             : "DuckDB returned a null result while flushing";
      }
      delete con;
      return false;
    }

    const std::string metadata_query =
        "SELECT SUM(total_compressed_size) FROM parquet_metadata(" +
        quote_string_literal(target_s3_path) + ")";
    parquet_log_info("DuckDB query [commit/metadata] " +
                     parquet_log_preview(metadata_query));
    auto metadata_result = con->Query(metadata_query);
    if (metadata_result && !metadata_result->HasError() &&
        metadata_result->RowCount() > 0 &&
        !metadata_result->GetValue(0, 0).IsNull()) {
      compressed_size = metadata_result->GetValue(0, 0).GetValue<int64_t>();
    }
    delete con;
  } catch (const std::exception &e) {
    delete con;
    if (error != nullptr) {
      *error = e.what();
    }
    return false;
  }

  table_trx->uploaded_s3_file_paths.push_back(target_s3_path);
  parquet_log_info("S3 staged object path='" + target_s3_path + "' table='" +
                   table_trx->table_name + "' rows=" +
                   std::to_string(total_rows) + " bytes=" +
                   std::to_string(compressed_size));

  if (s3_path != nullptr) {
    *s3_path = target_s3_path;
  }
  if (record_count != nullptr) {
    *record_count = total_rows;
  }
  if (file_size != nullptr) {
    *file_size = compressed_size;
  }
  return true;
}


int ha_parquet::create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info)
{
  parquet::TableMetadata metadata;
  std::string error;
  const auto *options = parquet_table_options(create_info);
  if (!parquet::ResolveCreateTableMetadata(
          name, options != nullptr ? options->catalog : nullptr,
          options != nullptr ? options->connection : nullptr, &metadata,
          &error)) {
    raise_create_option_error(error);
    return HA_ERR_UNSUPPORTED;
  }
  if (!parquet::ValidateCatalogConfig(metadata, &error) ||
      !parquet::ValidateObjectStoreConfig(metadata, true, &error)) {
    raise_unknown_error(error);
    return HA_ERR_INTERNAL_ERROR;
  }

  std::string table_path(name);
  if (helper_db_path.empty()) {
    size_t pos = table_path.find_last_of("/\\");
    helper_db_path = (pos == std::string::npos) ? "duckdb_helper.duckdb"
                                                : table_path.substr(0, pos) +
                                                      "/duckdb_helper.duckdb";
  }

  std::string parquet_file = std::string(name) + ".parquet";
  parquet_file_path = parquet_file;
  std::string table_name = table_name_from_path(table_path);

  std::string buf_name = "buf_" + table_name;
  std::string query = build_query_create(buf_name, table_arg);
  if (query.empty())
    return HA_ERR_UNSUPPORTED;

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
      std::cerr << "DuckDB CREATE error: "
                << (create_result ? create_result->GetError()
                                  : "null result")
                << std::endl;
      delete con;
      return HA_ERR_INTERNAL_ERROR;
    }

    const std::string copy_query =
        build_copy_to_parquet_query(buf_name, parquet_file);
    parquet_log_info("DuckDB query [create/seed-parquet] " +
                     parquet_log_preview(copy_query));
    auto copy_result = con->Query(copy_query);
    if (!copy_result || copy_result->HasError()) {
      std::cerr << "DuckDB COPY error: "
                << (copy_result ? copy_result->GetError() : "null result")
                << std::endl;
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

  const std::string lakekeeper_url = lakekeeper_table_collection_url(metadata);
  if (lakekeeper_url.empty()) {
    raise_unknown_error("Parquet catalog collection URL could not be resolved");
    return HA_ERR_INTERNAL_ERROR;
  }

  json fields = json::array();
  int field_id = 1;
  for (Field **field = table_arg->s->field; *field; ++field) {
    Field *f = *field;
    std::string iceberg_type;
    switch (f->type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
        iceberg_type = "int";
        break;
      case MYSQL_TYPE_LONGLONG:
        iceberg_type = "long";
        break;
      case MYSQL_TYPE_FLOAT:
        iceberg_type = "float";
        break;
      case MYSQL_TYPE_DOUBLE:
        iceberg_type = "double";
        break;
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_STRING:
        iceberg_type = "string";
        break;
      default:
        iceberg_type = "string";
        break;
    }
    fields.push_back({{"id", field_id++},
                      {"name", std::string(f->field_name.str)},
                      {"required", false},
                      {"type", iceberg_type}});
  }

  const std::string json_body =
      json({{"name", metadata.catalog_table_ident.table_name},
            {"schema",
             {{"type", "struct"}, {"schema-id", 0}, {"fields", fields}}}})
          .dump();

  CURL *curl = curl_easy_init();
  if (curl) {
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, lakekeeper_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    apply_catalog_request_options(metadata.catalog_config, curl, &headers);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    parquet_log_info("LakeKeeper create table begin table='" + table_name +
                     "' url='" + lakekeeper_url + "' payload=" +
                     parquet_log_preview(json_body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     +[](char *, size_t s, size_t n, void *) -> size_t {
                       return s * n;
                     });
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
      std::cerr << "LakeKeeper create table error: "
                << curl_easy_strerror(res) << std::endl;
      return HA_ERR_INTERNAL_ERROR;
    } else if (http_code != 200 && http_code != 201 && http_code != 409) {
      std::cerr << "LakeKeeper create table HTTP error: " << http_code
                << std::endl;
      return HA_ERR_INTERNAL_ERROR;
    }

    parquet_log_info("LakeKeeper create table complete table='" + table_name +
                     "' http_status=" + std::to_string(http_code));
  }

  if (!parquet::SaveTableMetadata(metadata, &error)) {
    raise_unknown_error("Failed to persist Parquet table metadata: " + error);
    return HA_ERR_INTERNAL_ERROR;
  }

  return 0;
}

int ha_parquet::delete_table(const char *name)
{
 DBUG_ENTER("ha_parquet::delete_table");

 const std::string table_path(name);
 parquet::TableMetadata metadata;
 if (!resolve_runtime_metadata_or_error(name, &metadata) ||
     !validate_catalog_or_error(metadata)) {
   DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
 }

 const std::string lakekeeper_url = lakekeeper_table_url(metadata);

 CURL *curl = curl_easy_init();
 if (curl) {
   struct curl_slist *headers = NULL;
   parquet_log_info("LakeKeeper delete table begin table='" +
                    metadata.catalog_table_ident.table_name +
                    "' url='" + lakekeeper_url + "'");
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
   curl_easy_setopt(curl, CURLOPT_URL, lakekeeper_url.c_str());
   apply_catalog_request_options(metadata.catalog_config, curl, &headers);
   if (headers != NULL)
     curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
     +[](char*, size_t s, size_t n, void*) -> size_t { return s * n; });
   CURLcode res = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   if (headers != NULL)
     curl_slist_free_all(headers);
   curl_easy_cleanup(curl);

   if (res != CURLE_OK) {
     std::cerr << "LakeKeeper delete table error: " << curl_easy_strerror(res) << std::endl;
     DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
   }

   if (http_code != 200 && http_code != 204 && http_code != 404) {
     std::cerr << "LakeKeeper delete table HTTP error: " << http_code << std::endl;
     DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
   }

   parquet_log_info("LakeKeeper delete table complete table='" +
                    metadata.catalog_table_ident.table_name +
                    "' http_status=" + std::to_string(http_code));
 }

 std::remove((table_path + ".parquet").c_str());
 std::remove((table_path + ".parquet.meta").c_str());
 DBUG_RETURN(0);
}

int ha_parquet::write_row(const uchar *buf)
{
  DBUG_ENTER("ha_parquet::write_row");

  THD *thd = table != nullptr ? table->in_use : nullptr;
  if (thd == nullptr) {
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  parquet_trx_data *trx =
      static_cast<parquet_trx_data *>(thd_get_ha_data(thd, parquet_hton));
  if (trx == nullptr) {
    trx = new parquet_trx_data();
    thd_set_ha_data(thd, parquet_hton, trx);
  }

  parquet_table_trx_data *table_trx =
      parquet_table_txn_for_share(trx, thd, table->s);
  if (table_trx == nullptr) {
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  const std::string desired_buffer_name =
      parquet_statement_buffer_name(thd, table->s->normalized_path.str);
  if (!table_trx->statement_buffer_name.empty() &&
      table_trx->statement_buffer_name != desired_buffer_name) {
    parquet_reset_statement_buffer(table_trx);
  }
  if (table_trx->statement_buffer_name != desired_buffer_name) {
    table_trx->statement_buffer_name = desired_buffer_name;
    table_trx->statement_row_count = 0;
  }

  duckdb::Connection *con = nullptr;
  try {
    std::lock_guard<std::mutex> lock(g_duckdb_mutex);
    con = new duckdb::Connection(*g_duckdb);
    parquet_log_info("DuckDB query [write/install] INSTALL parquet;");
    con->Query("INSTALL parquet;");
    parquet_log_info("DuckDB query [write/load] LOAD parquet;");
    con->Query("LOAD parquet;");

    std::string create_sql =
        build_query_create(table_trx->statement_buffer_name, table);
    if (create_sql.empty()) { delete con; DBUG_RETURN(HA_ERR_UNSUPPORTED); }
    parquet_log_info("DuckDB query [write/ensure-buffer] " +
                     parquet_log_preview(create_sql));
    con->Query(create_sql);

    std::string insert_sql =
        "INSERT INTO " + table_trx->statement_buffer_name + " VALUES (";
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
      std::cerr << "INSERT error: " << (result ? result->GetError() : "null result") << std::endl;
      delete con;
      DBUG_RETURN(HA_ERR_GENERIC);
    }

    table_trx->statement_row_count++;
    parquet_log_info("DuckDB buffered row table='" +
                     std::string(table->s->table_name.str) +
                     "' statement_rows=" +
                     std::to_string(table_trx->statement_row_count));
    delete con;

  } catch (const duckdb::Exception &e) {
    delete con;
    std::cerr << "write_row DuckDB exception: " << e.what() << std::endl;
    DBUG_RETURN(HA_ERR_GENERIC);
  } catch (const std::exception &e) {
    delete con;
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
  parquet::TableMetadata metadata;
  if (!resolve_runtime_metadata_or_error(table->s->normalized_path.str,
                                         &metadata) ||
      !validate_catalog_or_error(metadata) ||
      !validate_object_store_or_error(metadata)) {
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  std::string response_body;
  long http_code = 0;
  if (!fetch_lakekeeper_table_metadata(metadata, &response_body, &http_code)) {
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

  if (has_pushed_cond && !pushed_cond_sql.empty()) {
    query += " WHERE " + pushed_cond_sql;
  }

  duckdb::Connection *con = nullptr;
  try {
    std::lock_guard<std::mutex> lock(g_duckdb_mutex);
    con = new duckdb::Connection(*g_duckdb);
    parquet_log_info("DuckDB query [read/install] INSTALL parquet;");
    con->Query("INSTALL parquet;");
    parquet_log_info("DuckDB query [read/load] LOAD parquet;");
    con->Query("LOAD parquet;");
    parquet_log_info("DuckDB query [read/install-httpfs] INSTALL httpfs;");
    con->Query("INSTALL httpfs;");
    parquet_log_info("DuckDB query [read/load-httpfs] LOAD httpfs;");
    con->Query("LOAD httpfs;");
    std::string error;
    if (!configure_duckdb_s3(con, metadata.object_store_config, &error)) {
      raise_unknown_error(error);
      delete con;
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }

    parquet_log_info("DuckDB query [read/scan] " +
                     parquet_log_preview(query));
    auto result = con->Query(query);
    if (!result || result->HasError()) {
      std::cerr << "rnd_init read error: " << (result ? result->GetError() : "null result") << std::endl;
      delete con;
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    scan_result = std::move(result);
    delete con;

  } catch (const std::exception &e) {
    delete con;
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
     std::string s = val.ToString();
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
   if (lock_type == F_WRLCK) {
     parquet::TableMetadata metadata;
     if (!resolve_runtime_metadata_or_error(table_share->normalized_path.str,
                                            &metadata) ||
         !validate_catalog_or_error(metadata) ||
         !validate_object_store_or_error(metadata)) {
       DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
     }
   }
   trans_register_ha(thd, false, parquet_hton, 0);  
   if (thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))  
     trans_register_ha(thd, true, parquet_hton, 0);                  
   if (lock_type == F_WRLCK) {
     parquet_trx_data *trx =
         static_cast<parquet_trx_data *>(thd_get_ha_data(thd, parquet_hton));
     if (trx == NULL) {
       trx = new parquet_trx_data();
       thd_set_ha_data(thd, parquet_hton, trx);
     }
     parquet_table_trx_data *table_trx =
         parquet_table_txn_for_share(trx, thd, table_share);
     if (table_trx == nullptr) {
       DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
     }
     const std::string desired_buffer_name =
         parquet_statement_buffer_name(thd, table_share->normalized_path.str);
     if (!table_trx->statement_buffer_name.empty() &&
         table_trx->statement_buffer_name != desired_buffer_name) {
       parquet_reset_statement_buffer(table_trx);
     }
     if (table_trx->statement_buffer_name != desired_buffer_name) {
       table_trx->statement_buffer_name = desired_buffer_name;
       table_trx->statement_row_count = 0;
       parquet_log_info("Parquet registered write statement table='" +
                        table_trx->table_name + "' buffer='" +
                        table_trx->statement_buffer_name + "' query_id=" +
                        std::to_string(static_cast<unsigned long long>(
                            thd->query_id)));
     }
   }
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
 parquet_trx_data *trx =
   static_cast<parquet_trx_data *>(thd_get_ha_data(thd, parquet_hton));
 if (!trx) return 0;

 std::string stage_error;
 bool has_pending_work = false;
 int pending_table_count = 0;
 for (auto &entry : trx->tables) {
   parquet_table_trx_data &table_trx = entry.second;
   if (table_trx.statement_row_count != 0 &&
       !parquet_stage_statement_buffer_to_local(
           &table_trx, table_trx.table_path + ".parquet", &stage_error)) {
     raise_unknown_error("Failed to stage Parquet statement rows locally: " +
                         stage_error);
     return 1;
   }
   if (!table_trx.staged_files.empty()) {
     has_pending_work = true;
     pending_table_count++;
   }
  }

 if (!parquet_is_real_commit(thd, all)) {
   if (has_pending_work) {
     parquet_log_info(
         "Parquet statement commit complete; remote flush deferred until "
         "transaction commit tables=" +
         std::to_string(pending_table_count));
   }
   return 0;
 }

 if (!has_pending_work) {
   parquet_log_info("Parquet transaction commit has no staged write work");
   delete trx;
   thd_set_ha_data(thd, parquet_hton, nullptr);
   return 0;
 }

 if (pending_table_count > 1) {
   raise_unknown_error(
       "Parquet currently supports committing writes to only one table per "
       "transaction");
   return 1;
 }

 parquet_log_info("Parquet transaction commit begin tables=" +
                  std::to_string(pending_table_count));

 static long long snapshot_counter = 0;
 for (auto &entry : trx->tables) {
   parquet_table_trx_data &table_trx = entry.second;
   if (table_trx.staged_files.empty()) {
     continue;
   }

   parquet::TableMetadata metadata;
   if (!resolve_runtime_metadata_or_error(table_trx.table_path.c_str(),
                                          &metadata) ||
       !validate_catalog_or_error(metadata) ||
       !validate_object_store_or_error(metadata)) {
     return 1;
   }

   std::string s3_path;
   int64_t record_count = 0;
   int64_t file_size = 0;
   std::string flush_error;
   if (!parquet_flush_local_stages_to_s3(&table_trx, metadata, &s3_path,
                                         &record_count, &file_size,
                                         &flush_error)) {
     raise_unknown_error("Failed to flush Parquet rows to S3 during commit: " +
                         flush_error);
     return 1;
   }

   parquet_log_info("Parquet commit uploaded staged files table='" +
                    table_trx.table_name + "' staged_file_count=" +
                    std::to_string(table_trx.staged_files.size()) +
                    " target='" + s3_path + "'");
   long long snapshot_id =
       static_cast<long long>(time(NULL)) * 1000 +
       (snapshot_counter++ % 1000);
   json snapshot_summary = {{"operation", "append"},
                            {"added-data-files", "1"},
                            {"added-records",
                             std::to_string(record_count)},
                            {"added-files-size",
                             std::to_string(file_size)}};
   json snapshot = {{"snapshot-id", snapshot_id},
                    {"sequence-number", 1},
                    {"timestamp-ms", snapshot_id * 1000},
                    {"summary", snapshot_summary},
                    {"schema-id", 0},
                    {"manifest-list", s3_path}};
   json updates = json::array();
   updates.push_back({{"action", "add-snapshot"}, {"snapshot", snapshot}});
   updates.push_back({{"action", "set-snapshot-ref"},
                      {"ref-name", "main"},
                      {"type", "branch"},
                      {"snapshot-id", snapshot_id}});
   json identifier = {{"namespace",
                       namespace_json_array(
                           metadata.catalog_table_ident.namespace_ident)},
                      {"name", metadata.catalog_table_ident.table_name}};
   json table_change = {{"identifier", identifier},
                        {"requirements", json::array()},
                        {"updates", updates}};
   const std::string json_body =
       json({{"table-changes", json::array({table_change})}}).dump();

   CURL *curl = curl_easy_init();
   if (!curl) {
     return 1;
   }

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/json");
   curl_easy_setopt(curl, CURLOPT_POST, 1L);
   const std::string lakekeeper_url =
       lakekeeper_transaction_commit_url(metadata);
   curl_easy_setopt(curl, CURLOPT_URL, lakekeeper_url.c_str());
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
   apply_catalog_request_options(metadata.catalog_config, curl, &headers);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   parquet_log_info("LakeKeeper commit begin table='" + table_trx.table_name +
                    "' snapshot_id=" + std::to_string(snapshot_id) +
                    " manifest='" + s3_path + "' url='" + lakekeeper_url +
                    "' payload=" + parquet_log_preview(json_body));

   CURLcode res = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_slist_free_all(headers);
   curl_easy_cleanup(curl);

   if (res != CURLE_OK) {
     std::cerr << "LakeKeeper commit error: " << curl_easy_strerror(res)
               << std::endl;
     return 1;
   }
   if (http_code != 200) {
     std::cerr << "LakeKeeper commit HTTP error: " << http_code
               << std::endl;
     return 1;
   }

   parquet_log_info("LakeKeeper commit complete table='" +
                    table_trx.table_name + "' snapshot_id=" +
                    std::to_string(snapshot_id) + " http_status=" +
                    std::to_string(http_code));
   parquet_remove_local_stage_files(&table_trx);
   table_trx.uploaded_s3_file_paths.clear();
 }

 parquet_log_info("Parquet transaction commit complete");
 delete trx;
 thd_set_ha_data(thd, parquet_hton, nullptr);
 return 0;
}


static int ha_parquet_rollback(THD *thd, bool all)
{
    parquet_trx_data *trx =
        static_cast<parquet_trx_data *>(thd_get_ha_data(thd, parquet_hton));
    if (!trx) return 0;

    const bool real_rollback = parquet_is_real_rollback(thd, all);
    parquet_log_info(std::string("Parquet rollback begin scope='") +
                     (real_rollback ? "transaction" : "statement") + "'");
    int error = 0;
    for (auto &entry : trx->tables) {
        parquet_table_trx_data &table_trx = entry.second;
        parquet_reset_statement_buffer(&table_trx);

        if (!real_rollback) {
            continue;
        }

        parquet_remove_local_stage_files(&table_trx);
        if (table_trx.uploaded_s3_file_paths.empty()) {
            continue;
        }

        parquet::TableMetadata metadata;
        if (!resolve_runtime_metadata_or_error(table_trx.table_path.c_str(),
                                               &metadata) ||
            !validate_object_store_or_error(metadata)) {
            error = 1;
            continue;
        }

        parquet::ParquetObjectStoreClient object_store(
            metadata.object_store_config);
        for (const std::string &s3_path : table_trx.uploaded_s3_file_paths) {
            parquet::ObjectLocation location;
            if (!parquet::ParseS3Uri(s3_path, &location)) {
                std::cerr << "rollback: failed to parse staged object path "
                          << s3_path << std::endl;
                error = 1;
                continue;
            }
            location = parquet::ResolveAbsoluteObjectLocation(
                metadata.object_store_config, location.bucket, location.key);

            parquet_log_info("Parquet rollback deleting uploaded object table='" +
                             table_trx.table_name + "' path='" + s3_path + "'");
            auto status = object_store.DeleteObject(location);
            if (!status.ok()) {
                std::cerr << "rollback: failed to delete " << s3_path << ": "
                          << status.message << " (HTTP " << status.http_status
                          << ")" << std::endl;
                error = 1;
            }
        }
    }

    if (real_rollback) {
        delete trx;
        thd_set_ha_data(thd, parquet_hton, nullptr);
    }
    parquet_log_info("Parquet rollback complete");
    return error;
}

static int ha_parquet_init(void *p)
{
 parquet_hton = (handlerton *) p;
 parquet_hton->create   = parquet_create_handler;
 parquet_hton->create_select = create_duckdb_select_handler;
 parquet_hton->commit   = ha_parquet_commit;
 parquet_hton->rollback = ha_parquet_rollback;
 parquet_hton->table_options = parquet_table_option_list;
 thr_lock_init(&parquet_lock);

 /* Mirror S3: copy startup-only secret sysvars into runtime globals and mask
    SHOW VARIABLES output before any table metadata resolution happens. */
 update_lakekeeper_bearer_token(0, 0, 0, 0);
 update_s3_access_key_id(0, 0, 0, 0);
 update_s3_secret_access_key(0, 0, 0, 0);


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
 parquet_system_variables,
 "1.0",
 MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
