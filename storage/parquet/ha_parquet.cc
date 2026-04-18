#define MYSQL_SERVER 1

#include "ha_parquet.h"
#include "ha_parquet_pushdown.h"
#include "parquet_iceberg.h"

#include "my_dir.h"
#include "my_sys.h"
#include "mysql/plugin.h"
#include "mysqld_error.h"
#include "mysqld.h"
#ifndef ER_INVALID_USE_OF_ORA_JOIN_WRONG_FUNC
#define ER_INVALID_USE_OF_ORA_JOIN_WRONG_FUNC ER_UNKNOWN_ERROR
#endif
#include "field.h"
#include "item.h"
#include "sql_string.h"
#include "table.h"

#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

handlerton *parquet_hton= 0;
static THR_LOCK parquet_lock;

struct ha_table_option_struct
{
  char *connection;
  char *catalog;
  char *parquet_version;
  char *compression_codec;
  ulonglong block_size;
};

static ha_create_table_option parquet_table_option_list[]=
{
  HA_TOPTION_STRING("CONNECTION", connection),
  HA_TOPTION_STRING("CATALOG", catalog),
  HA_TOPTION_STRING("PARQUET_VERSION", parquet_version),
  HA_TOPTION_STRING("COMPRESSION_CODEC", compression_codec),
  HA_TOPTION_NUMBER("BLOCK_SIZE", block_size,
                    16ULL * 1024ULL * 1024ULL,
                    1024ULL, (1ULL << 34), 1024ULL),
  HA_TOPTION_END
};

namespace
{

using json = nlohmann::json;

std::atomic<uint64_t> parquet_flush_counter{1};

std::string quote_string_literal(const std::string &value)
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

std::string mariadb_type_to_duckdb(Field *field)
{
  switch (field->type()) {
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
      if (field->charset() == &my_charset_bin) {
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

std::string build_copy_to_parquet_query(const std::string &table_name,
                                        const std::string &parquet_file,
                                        const parquet::TableOptions &table_options)
{
  std::string query = "COPY " + table_name + " TO " +
                      quote_string_literal(parquet_file) + " (FORMAT PARQUET";
  if (!table_options.compression_codec.empty()) {
    std::string compression = table_options.compression_codec;
    std::transform(compression.begin(), compression.end(), compression.begin(),
                   [](unsigned char ch) {
                     return static_cast<char>(std::toupper(ch));
                   });
    query += ", COMPRESSION " + compression;
  }
  query += ")";
  return query;
}

std::string mariadb_type_to_iceberg(Field *field)
{
  switch (field->type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_YEAR:
      return "int";

    case MYSQL_TYPE_LONGLONG:
      return "long";

    case MYSQL_TYPE_FLOAT:
      return "float";

    case MYSQL_TYPE_DOUBLE:
      return "double";

    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      return "decimal(" + std::to_string(field->field_length) + "," +
             std::to_string(field->decimals()) + ")";

    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
      return "string";

    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
      return field->charset() == &my_charset_bin ? "binary" : "string";

    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
      return "date";

    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2:
      return "time";

    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2:
      return "timestamp";

    case MYSQL_TYPE_BIT:
      return "boolean";

    default:
      return "";
  }
}

std::string build_iceberg_schema_json(TABLE *table_arg)
{
  json fields = json::array();
  int next_field_id = 1;

  for (Field **field = table_arg->s->field; *field; ++field) {
    auto *current_field = *field;
    const auto iceberg_type = mariadb_type_to_iceberg(current_field);
    if (iceberg_type.empty()) {
      return "";
    }

    fields.push_back({
        {"id", next_field_id++},
        {"name", std::string(current_field->field_name.str,
                              current_field->field_name.length)},
        {"required", !current_field->real_maybe_null()},
        {"type", iceberg_type},
    });
  }

  return json({
      {"type", "struct"},
      {"schema-id", 0},
      {"identifier-field-ids", json::array()},
      {"fields", fields},
  }).dump();
}

uint64_t estimate_field_size(Field *field, const String *value)
{
  switch (field->type()) {
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
      return value != nullptr ? value->length() : field->field_length;
    default:
      return static_cast<uint64_t>(field->pack_length());
  }
}

bool append_field_to_buffer(duckdb::Appender *appender, Field *field,
                            uint64_t *row_bytes)
{
  if (appender == nullptr || field == nullptr || row_bytes == nullptr) {
    return false;
  }

  if (field->is_null()) {
    *row_bytes += 1;
    appender->Append(nullptr);
    return true;
  }

  switch (field->type()) {
    case MYSQL_TYPE_TINY:
      *row_bytes += field->pack_length();
      appender->Append(static_cast<int8_t>(field->val_int()));
      return true;

    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_YEAR:
      *row_bytes += field->pack_length();
      appender->Append(static_cast<int16_t>(field->val_int()));
      return true;

    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      *row_bytes += field->pack_length();
      appender->Append(static_cast<int32_t>(field->val_int()));
      return true;

    case MYSQL_TYPE_LONGLONG:
      *row_bytes += field->pack_length();
      appender->Append(static_cast<int64_t>(field->val_int()));
      return true;

    case MYSQL_TYPE_FLOAT:
      *row_bytes += field->pack_length();
      appender->Append(static_cast<float>(field->val_real()));
      return true;

    case MYSQL_TYPE_DOUBLE:
      *row_bytes += field->pack_length();
      appender->Append(field->val_real());
      return true;

    case MYSQL_TYPE_BIT:
      *row_bytes += field->pack_length();
      appender->Append(field->val_int() != 0);
      return true;

    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE: {
      MYSQL_TIME value {};
      *row_bytes += field->pack_length();
      if (field->get_date(&value, date_mode_t(0))) {
        appender->Append(nullptr);
      } else {
        appender->Append(duckdb::Value::DATE(value.year, value.month,
                                             value.day));
      }
      return true;
    }

    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2: {
      MYSQL_TIME value {};
      *row_bytes += field->pack_length();
      if (field->get_date(&value, date_mode_t(0))) {
        appender->Append(nullptr);
      } else {
        appender->Append(duckdb::Value::TIME(
            value.hour, value.minute, value.second, value.second_part));
      }
      return true;
    }

    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2: {
      MYSQL_TIME value {};
      *row_bytes += field->pack_length();
      if (field->get_date(&value, date_mode_t(0))) {
        appender->Append(nullptr);
      } else {
        appender->Append(duckdb::Value::TIMESTAMP(
            value.year, value.month, value.day, value.hour, value.minute,
            value.second, value.second_part));
      }
      return true;
    }

    default: {
      String value_buffer;
      auto *value = field->val_str(&value_buffer);
      if (value == nullptr) {
        *row_bytes += 1;
        appender->Append(nullptr);
        return true;
      }

      *row_bytes += estimate_field_size(field, value);
      const std::string string_value(value->ptr(), value->length());
      if (field->charset() == &my_charset_bin &&
          (field->type() == MYSQL_TYPE_TINY_BLOB ||
           field->type() == MYSQL_TYPE_MEDIUM_BLOB ||
           field->type() == MYSQL_TYPE_LONG_BLOB ||
           field->type() == MYSQL_TYPE_BLOB)) {
        appender->Append(duckdb::Value::BLOB(
            reinterpret_cast<duckdb::const_data_ptr_t>(
                string_value.data()),
            string_value.size()));
      } else {
        appender->Append(duckdb::Value(string_value));
      }
      return true;
    }
  }
}

std::string normalize_printed_predicate(std::string predicate)
{
  std::replace(predicate.begin(), predicate.end(), '`', '"');
  return predicate;
}

std::string render_condition_sql(const Item *cond)
{
  if (cond == nullptr) {
    return "";
  }

  String rendered;
  const_cast<Item *>(cond)->print(
      &rendered, static_cast<enum_query_type>(
                     QT_TO_SYSTEM_CHARSET |
                     QT_ITEM_IDENT_SKIP_DB_NAMES |
                     QT_ITEM_IDENT_SKIP_TABLE_NAMES));
  return normalize_printed_predicate(
      std::string(rendered.ptr(), rendered.length()));
}

std::string build_read_parquet_argument(
    const std::vector<std::string> &scan_paths)
{
  if (scan_paths.empty()) {
    return "";
  }

  if (scan_paths.size() == 1) {
    return quote_string_literal(scan_paths.front());
  }

  std::string argument = "[";
  bool first = true;
  for (const auto &path : scan_paths) {
    if (!first) {
      argument += ", ";
    }
    first = false;
    argument += quote_string_literal(path);
  }
  argument += "]";
  return argument;
}

std::string build_empty_scan_query(TABLE *table_arg)
{
  std::string query = "SELECT ";
  bool first = true;

  for (Field **field = table_arg->s->field; *field; ++field) {
    const auto duckdb_type = mariadb_type_to_duckdb(*field);
    if (duckdb_type.empty()) {
      return "";
    }

    if (!first) {
      query += ", ";
    }
    first = false;

    query += "CAST(NULL AS " + duckdb_type + ") AS ";
    query += (*field)->field_name.str;
  }

  query += " WHERE 1=0";
  return query;
}

bool execute_duckdb_query(duckdb::Connection *con, const std::string &query,
                          std::string *error)
{
  if (con == nullptr) {
    if (error != nullptr) {
      *error = "DuckDB connection is not initialized";
    }
    return false;
  }

  auto result = con->Query(query);
  if (!result || result->HasError()) {
    if (error != nullptr) {
      *error = result ? result->GetError()
                      : "DuckDB failed to produce a query result";
    }
    return false;
  }
  return true;
}

parquet::TableOptions resolve_table_options(
    const ha_table_option_struct *options)
{
  auto resolved = parquet::ResolveDefaultTableOptions();
  if (options == nullptr) {
    return resolved;
  }

  if (options->parquet_version && options->parquet_version[0]) {
    resolved.parquet_version = options->parquet_version;
  }
  if (options->compression_codec && options->compression_codec[0]) {
    resolved.compression_codec = options->compression_codec;
  }
  if (options->block_size > 0) {
    resolved.block_size_bytes = options->block_size;
  }

  return resolved;
}

void maybe_apply_table_location_to_object_store(
    const std::string &table_location,
    parquet::ObjectStoreConfig *config)
{
  if (config == nullptr || table_location.empty()) {
    return;
  }

  parquet::ObjectLocation location;
  if (!parquet::ParseS3Uri(table_location, &location)) {
    return;
  }

  config->bucket = location.bucket;
  config->key_prefix = location.key;
}

bool load_existing_table_metadata(const char *name,
                                  parquet::TableMetadata *metadata,
                                  std::string *error)
{
  if (metadata == nullptr) {
    if (error != nullptr) {
      *error = "metadata output pointer must not be null";
    }
    return false;
  }

  if (parquet::LoadTableMetadata(name, metadata, error)) {
    return true;
  }

  metadata->local_paths = parquet::ResolveLocalPaths(name);
  metadata->table_options = parquet::ResolveDefaultTableOptions();
  metadata->metadata_file_path = parquet::ResolveMetadataFilePath(name);
  metadata->catalog_table_ident.table_name = metadata->local_paths.table_name;
  if (!metadata->local_paths.database_name.empty()) {
    metadata->catalog_table_ident.namespace_ident.parts.push_back(
        metadata->local_paths.database_name);
  }
  return false;
}

void synchronize_active_scan_paths(parquet::TableMetadata *metadata)
{
  if (metadata == nullptr) {
    return;
  }

  if (!metadata->active_files.empty()) {
    metadata->active_scan_paths =
        parquet::ExtractActiveScanPaths(metadata->active_files);
  }
}

bool configure_duckdb_for_table(duckdb::Connection *con,
                                const parquet::TableMetadata &metadata,
                                std::string *error)
{
  if (!metadata.object_store_enabled) {
    return true;
  }

  if (!execute_duckdb_query(con, "LOAD httpfs", error)) {
    return false;
  }

  const auto &config = metadata.object_store_config;
  if (!config.region.empty() &&
      !execute_duckdb_query(
          con, "SET s3_region=" + quote_string_literal(config.region), error)) {
    return false;
  }
  if (!config.endpoint.empty() &&
      !execute_duckdb_query(
          con, "SET s3_endpoint=" + quote_string_literal(config.endpoint), error)) {
    return false;
  }
  if (!config.credentials.access_key_id.empty() &&
      !execute_duckdb_query(
          con, "SET s3_access_key_id=" +
                   quote_string_literal(config.credentials.access_key_id),
          error)) {
    return false;
  }
  if (!config.credentials.secret_access_key.empty() &&
      !execute_duckdb_query(
          con, "SET s3_secret_access_key=" +
                   quote_string_literal(config.credentials.secret_access_key),
          error)) {
    return false;
  }
  if (!config.credentials.session_token.empty() &&
      !execute_duckdb_query(
          con, "SET s3_session_token=" +
                   quote_string_literal(config.credentials.session_token),
          error)) {
    return false;
  }
  if (!config.url_style.empty() &&
      !execute_duckdb_query(
          con, "SET s3_url_style=" + quote_string_literal(config.url_style),
          error)) {
    return false;
  }

  const auto use_ssl = config.endpoint.rfind("https://", 0) == 0 ? "true"
                                                                  : "false";
  return execute_duckdb_query(con, "SET s3_use_ssl=" + std::string(use_ssl),
                              error);
}

bool store_scan_value(Field *field, const duckdb::Value &value)
{
  if (field == nullptr) {
    return false;
  }

  if (value.IsNull()) {
    field->set_null();
    return true;
  }

  field->set_notnull();

  switch (field->type()) {
    case MYSQL_TYPE_TINY:
      field->store(static_cast<longlong>(value.GetValue<int8_t>()), false);
      return true;

    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_YEAR:
      field->store(static_cast<longlong>(value.GetValue<int16_t>()), false);
      return true;

    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      field->store(static_cast<longlong>(value.GetValue<int32_t>()), false);
      return true;

    case MYSQL_TYPE_LONGLONG:
      field->store(static_cast<longlong>(value.GetValue<int64_t>()), false);
      return true;

    case MYSQL_TYPE_FLOAT:
      field->store(static_cast<double>(value.GetValue<float>()));
      return true;

    case MYSQL_TYPE_DOUBLE:
      field->store(value.GetValue<double>());
      return true;

    case MYSQL_TYPE_BIT:
      field->store(static_cast<longlong>(value.GetValue<bool>() ? 1 : 0), false);
      return true;

    default: {
      std::string string_value;
      try {
        string_value = value.GetValue<std::string>();
      } catch (const std::exception &) {
        string_value = value.ToString();
      }

      field->store(string_value.c_str(), string_value.length(),
                   field->charset());
      return true;
    }
  }
}

bool delete_local_file_if_exists(const std::string &path)
{
  if (path.empty()) {
    return true;
  }

  return my_delete(path.c_str(), MYF(MY_IGNORE_ENOENT)) == 0;
}

bool read_local_file_size(const std::string &path, uint64_t *file_size_bytes)
{
  MY_STAT stat_info;

  if (!my_stat(path.c_str(), &stat_info, MYF(0))) {
    return false;
  }

  if (file_size_bytes != nullptr) {
    *file_size_bytes = static_cast<uint64_t>(stat_info.st_size);
  }

  return true;
}

parquet::ParquetTxnState *ensure_statement_txn_state(THD *thd,
                                                     handlerton *hton)
{
  auto *txn_state = parquet::GetOrCreateTxnState(thd, hton);

  if (txn_state == nullptr) {
    return nullptr;
  }

  if (!txn_state->registered_with_server) {
    trans_register_ha(thd, false, hton, 0);
    txn_state->registered_with_server = true;
  }

  return txn_state;
}

std::string build_commit_union_query(
    const std::string &canonical_parquet_path,
    const std::vector<parquet::ParquetStagedFile> &staged_files)
{
  std::string union_query;
  bool first_source = true;

  const auto append_source = [&](const std::string &parquet_path) {
    if (!first_source) {
      union_query += " UNION ALL ";
    }

    first_source = false;
    union_query += "SELECT * FROM read_parquet(" +
                   quote_string_literal(parquet_path) + ")";
  };

  if (my_access(canonical_parquet_path.c_str(), F_OK) == 0) {
    append_source(canonical_parquet_path);
  }

  for (const auto &staged_file : staged_files) {
    append_source(staged_file.local_parquet_path);
  }

  return union_query;
}

std::string build_commit_temp_path(const std::string &canonical_parquet_path,
                                   uint64_t flush_id)
{
  return parquet::BuildLocalStagePath(canonical_parquet_path,
                                      flush_id + 1000000000ULL);
}

void delete_iceberg_artifact_files(const parquet::IcebergCommitArtifacts &artifacts)
{
  delete_local_file_if_exists(artifacts.manifest_local_path);
  delete_local_file_if_exists(artifacts.manifest_list_local_path);
}

void delete_iceberg_artifact_objects(
    const parquet::TableMetadata &table_metadata,
    const parquet::IcebergCommitArtifacts &artifacts)
{
  parquet::ParquetObjectStoreClient object_store_client(
      table_metadata.object_store_config);
  (void) object_store_client.DeleteObject(artifacts.manifest_location);
  (void) object_store_client.DeleteObject(artifacts.manifest_list_location);
}

bool finalize_table_staged_files(
    const std::vector<parquet::ParquetStagedFile> &staged_files,
    std::string *error)
{
  if (staged_files.empty()) {
    return true;
  }

  const auto canonical_paths =
      parquet::ResolveLocalPaths(staged_files.front().table_path.c_str());
  const auto canonical_parquet_path = canonical_paths.parquet_file_path;
  parquet::TableMetadata table_metadata;
  std::string metadata_error;
  const bool metadata_loaded = parquet::LoadTableMetadata(
      staged_files.front().table_path.c_str(), &table_metadata, &metadata_error);
  synchronize_active_scan_paths(&table_metadata);

  if (metadata_loaded && table_metadata.catalog_enabled) {
    if (!table_metadata.object_store_enabled) {
      if (error != nullptr) {
        *error =
            "catalog-backed PARQUET tables require object store metadata writes";
      }
      return false;
    }

    parquet::ParquetCatalogClient catalog_client(table_metadata.catalog_config);
    auto status = catalog_client.BootstrapConfig();
    if (!status.ok()) {
      if (error != nullptr) {
        *error = status.message;
      }
      return false;
    }

    parquet::CatalogLoadTableResult load_result;
    status = catalog_client.LoadTable(table_metadata.catalog_table_ident,
                                      &load_result,
                                      table_metadata.access_delegation);
    if (!status.ok()) {
      if (error != nullptr) {
        *error = status.message;
      }
      return false;
    }

    parquet::IcebergCommitArtifacts artifacts;
    if (!parquet::BuildIcebergCommitArtifacts(table_metadata, load_result,
                                              staged_files, &artifacts, error)) {
      return false;
    }

    parquet::ParquetObjectStoreClient object_store_client(
        table_metadata.object_store_config);
    parquet::PutObjectRequest manifest_request;
    manifest_request.local_file_path = artifacts.manifest_local_path;
    manifest_request.location = artifacts.manifest_location;
    manifest_request.content_type = "application/avro-binary";
    auto object_status = object_store_client.PutFile(manifest_request);
    if (!object_status.ok()) {
      delete_iceberg_artifact_files(artifacts);
      if (error != nullptr) {
        *error = object_status.message;
      }
      return false;
    }

    parquet::PutObjectRequest manifest_list_request;
    manifest_list_request.local_file_path = artifacts.manifest_list_local_path;
    manifest_list_request.location = artifacts.manifest_list_location;
    manifest_list_request.content_type = "application/avro-binary";
    object_status = object_store_client.PutFile(manifest_list_request);
    if (!object_status.ok()) {
      delete_iceberg_artifact_objects(table_metadata, artifacts);
      delete_iceberg_artifact_files(artifacts);
      if (error != nullptr) {
        *error = object_status.message;
      }
      return false;
    }

    parquet::CatalogCommitRequest request;
    request.ident = table_metadata.catalog_table_ident;
    request.request_json = artifacts.commit_request_json;
    request.if_match = load_result.etag;

    parquet::CatalogLoadTableResult commit_result;
    status = catalog_client.CommitTable(request, &commit_result);
    delete_iceberg_artifact_files(artifacts);
    if (!status.ok()) {
      if (!status.commit_state_unknown) {
        delete_iceberg_artifact_objects(table_metadata, artifacts);
      }
      if (error != nullptr) {
        *error = status.message;
      }
      return false;
    }

    table_metadata.table_uuid =
        !commit_result.metadata.table_uuid.empty()
            ? commit_result.metadata.table_uuid
            : load_result.metadata.table_uuid;
    table_metadata.current_snapshot_id =
        !commit_result.metadata.current_snapshot_id.empty()
            ? commit_result.metadata.current_snapshot_id
            : std::to_string(artifacts.snapshot_id);
    if (!commit_result.metadata.table_location.empty()) {
      table_metadata.table_location = commit_result.metadata.table_location;
    }
    table_metadata.active_files = artifacts.active_files;
    synchronize_active_scan_paths(&table_metadata);
    (void) parquet::SaveTableMetadata(table_metadata, nullptr);

    for (const auto &staged_file : staged_files) {
      delete_local_file_if_exists(staged_file.local_parquet_path);
    }
    return true;
  }

  const auto copy_table_options =
      metadata_loaded ? table_metadata.table_options
                      : parquet::ResolveDefaultTableOptions();
  const auto temp_parquet_path =
      build_commit_temp_path(canonical_parquet_path,
                             staged_files.back().flush_id);
  const auto backup_parquet_path = canonical_parquet_path + ".bak";
  const auto union_query =
      build_commit_union_query(canonical_parquet_path, staged_files);

  if (union_query.empty()) {
    if (error != nullptr) {
      *error = "commit found no Parquet sources to finalize";
    }
    return false;
  }

  delete_local_file_if_exists(temp_parquet_path);
  delete_local_file_if_exists(backup_parquet_path);

  try {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    const auto copy_query = build_copy_to_parquet_query(
        "(" + union_query + ")", temp_parquet_path, copy_table_options);
    auto copy_result = con.Query(copy_query);

    if (!copy_result || copy_result->HasError()) {
      delete_local_file_if_exists(temp_parquet_path);
      if (error != nullptr) {
        *error = copy_result ? copy_result->GetError()
                             : "DuckDB failed to produce a query result";
      }
      return false;
    }
  } catch (const std::exception &ex) {
    delete_local_file_if_exists(temp_parquet_path);
    if (error != nullptr) {
      *error = ex.what();
    }
    return false;
  }

  const bool had_existing_canonical =
      my_access(canonical_parquet_path.c_str(), F_OK) == 0;

  if (had_existing_canonical &&
      my_rename(canonical_parquet_path.c_str(), backup_parquet_path.c_str(),
                MYF(0))) {
    delete_local_file_if_exists(temp_parquet_path);
    if (error != nullptr) {
      *error = "failed to move the canonical Parquet file aside during commit";
    }
    return false;
  }

  if (my_rename(temp_parquet_path.c_str(), canonical_parquet_path.c_str(),
                MYF(0))) {
    if (had_existing_canonical) {
      (void) my_rename(backup_parquet_path.c_str(),
                       canonical_parquet_path.c_str(), MYF(0));
    }
    delete_local_file_if_exists(temp_parquet_path);
    if (error != nullptr) {
      *error = "failed to publish the finalized Parquet file during commit";
    }
    return false;
  }

  if (had_existing_canonical) {
    delete_local_file_if_exists(backup_parquet_path);
  }

  for (const auto &staged_file : staged_files) {
    delete_local_file_if_exists(staged_file.local_parquet_path);
  }

  if (metadata_loaded) {
    if (table_metadata.object_store_enabled) {
      for (const auto &staged_file : staged_files) {
        if (!staged_file.target_object_path.empty()) {
          table_metadata.active_scan_paths.push_back(
              staged_file.target_object_path);
          parquet::ActiveDataFile active_file;
          active_file.path = staged_file.target_object_path;
          active_file.record_count = staged_file.record_count;
          active_file.file_size_bytes = staged_file.file_size_bytes;
          table_metadata.active_files.push_back(std::move(active_file));
        }
      }
    } else {
      table_metadata.active_files.clear();
      table_metadata.active_scan_paths.clear();
      table_metadata.active_scan_paths.push_back(canonical_parquet_path);
    }

    synchronize_active_scan_paths(&table_metadata);
    if (!parquet::SaveTableMetadata(table_metadata, error)) {
      return false;
    }
  }

  return true;
}

bool finalize_txn_state(const parquet::ParquetTxnState &txn_state,
                        std::string *error)
{
  if (!parquet::ValidateTxnState(txn_state, error)) {
    return false;
  }

  std::map<std::string, std::vector<parquet::ParquetStagedFile>>
      staged_files_by_table;

  for (const auto &staged_file : txn_state.staged_files) {
    if (my_access(staged_file.local_parquet_path.c_str(), F_OK) != 0) {
      if (error != nullptr) {
        *error = "commit could not find a staged local Parquet file";
      }
      return false;
    }

    staged_files_by_table[staged_file.table_path].push_back(staged_file);
  }

  for (auto &entry : staged_files_by_table) {
    auto &table_staged_files = entry.second;
    std::sort(table_staged_files.begin(), table_staged_files.end(),
              [](const parquet::ParquetStagedFile &lhs,
                 const parquet::ParquetStagedFile &rhs) {
                return lhs.flush_id < rhs.flush_id;
              });

    if (!finalize_table_staged_files(table_staged_files, error)) {
      return false;
    }
  }

  return true;
}

void cleanup_txn_state_files(const parquet::ParquetTxnState &txn_state)
{
  for (const auto &staged_file : txn_state.staged_files) {
    delete_local_file_if_exists(staged_file.local_parquet_path);

    parquet::TableMetadata table_metadata;
    std::string metadata_error;
    if (!parquet::LoadTableMetadata(staged_file.table_path.c_str(),
                                    &table_metadata, &metadata_error) ||
        !table_metadata.object_store_enabled) {
      continue;
    }

    parquet::ObjectLocation parsed_location;
    if (!parquet::ParseS3Uri(staged_file.target_object_path, &parsed_location)) {
      continue;
    }

    auto location = parquet::ResolveAbsoluteObjectLocation(
        table_metadata.object_store_config, parsed_location.bucket,
        parsed_location.key);
    parquet::ParquetObjectStoreClient object_store_client(
        table_metadata.object_store_config);
    (void) object_store_client.DeleteObject(location);
  }
}

int parquet_commit(THD *thd, bool all)
{
  (void) all;
  DBUG_ENTER("parquet_commit");

  auto *txn_state = parquet::GetTxnState(thd, parquet_hton);

  if (txn_state == nullptr) {
    DBUG_RETURN(0);
  }

  if (txn_state->staged_files.empty() && !txn_state->has_error) {
    parquet::ClearTxnState(thd, parquet_hton);
    DBUG_RETURN(0);
  }

  std::string error;
  if (!finalize_txn_state(*txn_state, &error)) {
    my_error(ER_UNKNOWN_ERROR, MYF(0),
             error.empty() ? "PARQUET commit failed" : error.c_str());
    DBUG_RETURN(1);
  }

  parquet::ClearTxnState(thd, parquet_hton);
  DBUG_RETURN(0);
}

int parquet_rollback(THD *thd, bool all)
{
  (void) all;
  DBUG_ENTER("parquet_rollback");

  auto *txn_state = parquet::GetTxnState(thd, parquet_hton);

  if (txn_state == nullptr) {
    DBUG_RETURN(0);
  }

  cleanup_txn_state_files(*txn_state);
  parquet::ClearTxnState(thd, parquet_hton);
  DBUG_RETURN(0);
}

int parquet_close_connection(THD *thd)
{
  parquet::ClearTxnState(thd, parquet_hton);
  return 0;
}

} // namespace

ha_parquet::ha_parquet(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg)
{
  thr_lock_data_init(&parquet_lock, &lock, nullptr);
  table_options = parquet::ResolveDefaultTableOptions();
  flush_threshold = std::numeric_limits<uint64_t>::max();
}

ulonglong ha_parquet::table_flags() const
{
  return HA_FILE_BASED;
}

ulong ha_parquet::index_flags(uint, uint, bool) const
{
  return 0;
}

int ha_parquet::open(const char *name, int, uint)
{
  DBUG_ENTER("ha_parquet::open");

  teardown_duckdb();
  resolve_local_metadata(name);
  std::string metadata_error;
  load_existing_table_metadata(name, &table_metadata, &metadata_error);
  synchronize_active_scan_paths(&table_metadata);
  table_metadata.local_paths = local_paths;
  table_metadata.metadata_file_path = parquet::ResolveMetadataFilePath(name);
  table_options = table_metadata.table_options;
  if (table_options.block_size_bytes == 0) {
    table_options = parquet::ResolveDefaultTableOptions();
    table_metadata.table_options = table_options;
  }

  row_count = 0;
  buffered_bytes = 0;
  flush_threshold = table_options.block_size_bytes;
  if (flush_threshold == 0) {
    flush_threshold = std::numeric_limits<uint64_t>::max();
  }
  DBUG_EXECUTE_IF("parquet_flush_threshold_one", flush_threshold = 1;);
  buffer_table_created = false;
  current_row = 0;
  pushed_condition_sql.clear();

  if (initialize_duckdb()) {
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  DBUG_RETURN(0);
}

int ha_parquet::close(void)
{
  teardown_duckdb();
  pushed_condition_sql.clear();
  table_metadata = parquet::TableMetadata();
  return 0;
}

const Item *ha_parquet::cond_push(const Item *cond)
{
  DBUG_ENTER("ha_parquet::cond_push");
  pushed_condition_sql = render_condition_sql(cond);
  if (!pushed_condition_sql.empty()) {
    DBUG_RETURN(nullptr);
  }
  DBUG_RETURN(cond);
}

void ha_parquet::cond_pop()
{
  pushed_condition_sql.clear();
}

std::string build_query_create(std::string table_name, TABLE *table_arg)
{
  std::string query = "CREATE TABLE " + table_name + " (";
  bool first = true;

  for (Field **field = table_arg->s->field; *field; ++field) {
    Field *current_field = *field;
    std::string duck_type = mariadb_type_to_duckdb(current_field);

    if (duck_type.empty()) {
      return "";
    }

    if (!first) {
      query += ", ";
    }

    first = false;
    query += current_field->field_name.str;
    query += " ";
    query += duck_type;
  }

  query += ")";
  return query;
}

int ha_parquet::create(const char *name, TABLE *table_arg,
                       HA_CREATE_INFO *create_info)
{
  resolve_local_metadata(name);
  table_metadata = parquet::TableMetadata();
  table_metadata.local_paths = local_paths;
  table_metadata.metadata_file_path = parquet::ResolveMetadataFilePath(name);
  table_metadata.catalog_table_ident.table_name = local_paths.table_name;
  if (!local_paths.database_name.empty()) {
    table_metadata.catalog_table_ident.namespace_ident.parts.push_back(
        local_paths.database_name);
  }

  if (build_query_create(local_paths.table_name, table_arg).empty()) {
    return HA_ERR_UNSUPPORTED;
  }

  const auto *options = create_info != nullptr
                            ? static_cast<const ha_table_option_struct *>(
                                  create_info->option_struct)
                            : nullptr;
  table_options = resolve_table_options(options);
  table_metadata.table_options = table_options;

  if (options != nullptr && options->connection && options->connection[0]) {
    std::string error;
    if (!parquet::ParseObjectStoreConnectionString(options->connection,
                                                   &table_metadata.object_store_config,
                                                   &error)) {
      my_error(ER_UNKNOWN_ERROR, MYF(0), error.c_str());
      return HA_ERR_NO_CONNECTION;
    }
    table_metadata.object_store_enabled = true;
    if (table_metadata.object_store_config.key_prefix.empty()) {
      table_metadata.object_store_config.key_prefix =
          local_paths.database_name.empty()
              ? local_paths.table_name
              : local_paths.database_name + "/" + local_paths.table_name;
    }
  }

  if (options != nullptr && options->catalog && options->catalog[0]) {
    std::string error;
    if (!parquet::ParseCatalogConnectionString(options->catalog, local_paths,
                                               &table_metadata.catalog_config,
                                               &table_metadata.catalog_table_ident,
                                               &table_metadata.access_delegation,
                                               &error)) {
      my_error(ER_UNKNOWN_ERROR, MYF(0), error.c_str());
      return HA_ERR_NO_CONNECTION;
    }

    const auto schema_json = build_iceberg_schema_json(table_arg);
    if (schema_json.empty()) {
      return HA_ERR_UNSUPPORTED;
    }

    parquet::ParquetCatalogClient catalog_client(table_metadata.catalog_config);
    auto status = catalog_client.BootstrapConfig();
    if (!status.ok()) {
      my_error(ER_UNKNOWN_ERROR, MYF(0), status.message.c_str());
      return HA_ERR_NO_CONNECTION;
    }

    status = catalog_client.EnsureNamespace(
        table_metadata.catalog_table_ident.namespace_ident);
    if (!status.ok() && status.code != parquet::CatalogStatusCode::kConflict) {
      my_error(ER_UNKNOWN_ERROR, MYF(0), status.message.c_str());
      return HA_ERR_NO_CONNECTION;
    }

    parquet::CatalogCreateTableRequest request;
    request.ident = table_metadata.catalog_table_ident;
    request.schema_json = schema_json;
    request.properties["engine"] = "mariadb-parquet";
    request.properties["compression_codec"] = table_options.compression_codec;
    request.properties["parquet_version"] = table_options.parquet_version;

    parquet::CatalogLoadTableResult result;
    status = catalog_client.CreateTable(request, &result);
    if (!status.ok() && status.code != parquet::CatalogStatusCode::kConflict) {
      my_error(ER_UNKNOWN_ERROR, MYF(0), status.message.c_str());
      return HA_ERR_NO_CONNECTION;
    }
    if (status.code == parquet::CatalogStatusCode::kConflict) {
      status = catalog_client.LoadTable(table_metadata.catalog_table_ident,
                                        &result,
                                        table_metadata.access_delegation);
      if (!status.ok()) {
        my_error(ER_UNKNOWN_ERROR, MYF(0), status.message.c_str());
        return HA_ERR_NO_CONNECTION;
      }
    }

    table_metadata.catalog_enabled = true;
    table_metadata.table_uuid = result.metadata.table_uuid;
    table_metadata.table_location = result.metadata.table_location;
    table_metadata.current_snapshot_id = result.metadata.current_snapshot_id;
    maybe_apply_table_location_to_object_store(table_metadata.table_location,
                                               &table_metadata.object_store_config);
  }

  synchronize_active_scan_paths(&table_metadata);
  if (!parquet::SaveTableMetadata(table_metadata, nullptr)) {
    return HA_ERR_INTERNAL_ERROR;
  }

  return 0;
}

ha_parquet::FlushStatus ha_parquet::flush_remaining_rows_to_stage(
    parquet::ParquetStagedFile *staged_file)
{
  if (staged_file == nullptr || !duckdb_initialized || con == nullptr) {
    return FlushStatus::kError;
  }

  if (row_count == 0) {
    return FlushStatus::kNoRows;
  }

  const auto flush_id = parquet_flush_counter.fetch_add(1);
  const auto stage_path =
      parquet::BuildLocalStagePath(local_paths.parquet_file_path, flush_id);
  const auto rows_in_flush = row_count;

  if (buffer_appender) {
    buffer_appender->Close();
    buffer_appender.reset();
  }

  auto copy_result = con->Query(
      build_copy_to_parquet_query("buffer", stage_path, table_options));
  if (!copy_result || copy_result->HasError()) {
    return FlushStatus::kError;
  }

  uint64_t file_size_bytes = 0;
  if (!read_local_file_size(stage_path, &file_size_bytes)) {
    delete_local_file_if_exists(stage_path);
    return FlushStatus::kError;
  }

  std::string object_path = stage_path;
  if (table_metadata.object_store_enabled) {
    const auto relative_key =
        "data/flush_" + std::to_string(flush_id) + ".parquet";
    const auto location = parquet::ResolveObjectLocation(
        table_metadata.object_store_config, relative_key);
    parquet::ParquetObjectStoreClient object_store_client(
        table_metadata.object_store_config);
    parquet::PutObjectRequest request;
    request.local_file_path = stage_path;
    request.location = location;
    request.content_type = "application/octet-stream";
    request.expected_content_length = file_size_bytes;

    const auto status = object_store_client.PutFile(request);
    if (!status.ok()) {
      delete_local_file_if_exists(stage_path);
      return FlushStatus::kError;
    }

    object_path = parquet::BuildS3Uri(location.bucket, location.key);
  }

  auto delete_result = con->Query("DELETE FROM buffer");
  if (!delete_result || delete_result->HasError()) {
    delete_local_file_if_exists(stage_path);
    return FlushStatus::kError;
  }

  row_count = 0;
  buffered_bytes = 0;
  *staged_file = {table_metadata.local_paths.table_path,
                  table_metadata.catalog_table_ident.table_name.empty()
                      ? local_paths.table_name
                      : table_metadata.catalog_table_ident.table_name,
                  stage_path,
                  object_path,
                  rows_in_flush,
                  file_size_bytes,
                  flush_id};
  return FlushStatus::kSuccess;
}

int ha_parquet::write_row(const uchar *buf)
{
  (void) buf;
  DBUG_ENTER("ha_parquet::write_row");

  if (!buffer_table_created) {
    const auto create_sql = build_query_create("buffer", table);
    if (create_sql.empty()) {
      DBUG_RETURN(HA_ERR_UNSUPPORTED);
    }

    auto result = con->Query(create_sql);
    if (!result || result->HasError()) {
      DBUG_RETURN(HA_ERR_GENERIC);
    }

    buffer_table_created = true;
  }

  if (!buffer_appender) {
    try {
      buffer_appender =
          std::make_unique<duckdb::Appender>(*con, "buffer");
    } catch (const std::exception &) {
      DBUG_RETURN(HA_ERR_GENERIC);
    }
  }

  uint64_t row_bytes = 0;

  try {
    buffer_appender->BeginRow();
    for (Field **field = table->s->field; *field; ++field) {
      if (!append_field_to_buffer(buffer_appender.get(), *field, &row_bytes)) {
        buffer_appender.reset();
        DBUG_RETURN(HA_ERR_GENERIC);
      }
    }
    buffer_appender->EndRow();
  } catch (const std::exception &) {
    buffer_appender.reset();
    DBUG_RETURN(HA_ERR_GENERIC);
  }

  row_count += 1;
  buffered_bytes += row_bytes;

  if (buffered_bytes >= flush_threshold) {
    auto *txn_state = ensure_statement_txn_state(ha_thd(), ht);
    if (txn_state == nullptr) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }

    parquet::ParquetStagedFile staged_file;
    const auto flush_status = flush_remaining_rows_to_stage(&staged_file);

    if (flush_status == FlushStatus::kError) {
      txn_state->has_error = true;
      DBUG_RETURN(HA_ERR_GENERIC);
    }

    if (flush_status == FlushStatus::kSuccess) {
      txn_state->staged_files.push_back(staged_file);
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

  if (!duckdb_initialized || con == nullptr) {
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  std::vector<std::string> scan_paths = table_metadata.active_scan_paths;
  if (scan_paths.empty() &&
      !local_paths.parquet_file_path.empty() &&
      my_access(local_paths.parquet_file_path.c_str(), F_OK) == 0) {
    scan_paths.push_back(local_paths.parquet_file_path);
  }

  std::string query;
  if (scan_paths.empty()) {
    query = build_empty_scan_query(table);
  } else {
    query = "SELECT * FROM read_parquet(" +
            build_read_parquet_argument(scan_paths) + ")";
    if (!pushed_condition_sql.empty()) {
      query += " WHERE " + pushed_condition_sql;
    }
  }

  if (query.empty()) {
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  try {
    auto result = con->Query(query);
    if (!result || result->HasError()) {
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }

    scan_result = std::move(result);
  } catch (const std::exception &) {
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  DBUG_RETURN(0);
}

int ha_parquet::rnd_next(uchar *buf)
{
  (void) buf;
  DBUG_ENTER("ha_parquet::rnd_next");

  if (!scan_result || current_row >= scan_result->RowCount()) {
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }

  for (uint i = 0; i < table->s->fields; i++) {
    Field *field = table->field[i];
    auto value = scan_result->GetValue(i, current_row);

    if (!store_scan_value(field, value)) {
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
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

enum_alter_inplace_result ha_parquet::check_if_supported_inplace_alter(
    TABLE *, Alter_inplace_info *)
{
  return HA_ALTER_INPLACE_NOT_SUPPORTED;
}

int ha_parquet::external_lock(THD *thd, int lock_type)
{
  DBUG_ENTER("ha_parquet::external_lock");

  if (lock_type == F_RDLCK || lock_type == F_WRLCK) {
    if (ensure_statement_txn_state(thd, ht) == nullptr) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  } else if (lock_type == F_UNLCK) {
    auto *txn_state = parquet::GetTxnState(thd, ht);

    if (txn_state != nullptr) {
      parquet::ParquetStagedFile staged_file;
      const auto flush_status = flush_remaining_rows_to_stage(&staged_file);

      if (flush_status == FlushStatus::kError) {
        txn_state->has_error = true;
        DBUG_RETURN(HA_ERR_GENERIC);
      }

      if (flush_status == FlushStatus::kSuccess) {
        txn_state->staged_files.push_back(staged_file);
      }
    }
  }

  DBUG_RETURN(0);
}

THR_LOCK_DATA **ha_parquet::store_lock(THD *thd, THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type)
{
  (void) thd;
  if (lock_type != TL_IGNORE) {
    lock.type = lock_type;
  }

  *to++ = &lock;
  return to;
}

static handler *parquet_create_handler(handlerton *p_hton,
                                       TABLE_SHARE *table_share,
                                       MEM_ROOT *mem_root)
{
  return new (mem_root) ha_parquet(p_hton, table_share);
}

static int ha_parquet_init(void *p)
{
  parquet_hton = static_cast<handlerton *>(p);
  parquet_hton->create = parquet_create_handler;
  parquet_hton->create_select = create_duckdb_select_handler;
  parquet_hton->commit = parquet_commit;
  parquet_hton->rollback = parquet_rollback;
  parquet_hton->close_connection = parquet_close_connection;
  parquet_hton->table_options = parquet_table_option_list;
  thr_lock_init(&parquet_lock);
  return 0;
}

static int ha_parquet_deinit(void *p)
{
  (void) p;
  parquet_hton = 0;
  thr_lock_delete(&parquet_lock);
  return 0;
}

struct st_mysql_storage_engine parquet_storage_engine =
    {MYSQL_HANDLERTON_INTERFACE_VERSION};

maria_declare_plugin(parquet)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &parquet_storage_engine,
  "PARQUET",
  "UIUC Disruption Lab",
  "Parquet Storage Engine ",
  PLUGIN_LICENSE_GPL,
  ha_parquet_init,                  /* Plugin Init      */
  ha_parquet_deinit,                /* Plugin Deinit    */
  0x0100,                           /* 1.0              */
  nullptr,                          /* status variables */
  nullptr,                          /* system variables */
  "1.0",                            /* string version   */
  MariaDB_PLUGIN_MATURITY_STABLE    /* maturity         */
}
maria_declare_plugin_end;

void ha_parquet::resolve_local_metadata(const char *name)
{
  local_paths = parquet::ResolveLocalPaths(name);
  table_metadata.local_paths = local_paths;
  table_metadata.metadata_file_path = parquet::ResolveMetadataFilePath(name);
}

int ha_parquet::initialize_duckdb()
{
  try {
    db = new duckdb::DuckDB(nullptr);
    con = new duckdb::Connection(*db);
  } catch (const std::exception &) {
    teardown_duckdb();
    return HA_ERR_INTERNAL_ERROR;
  }

  const auto memory_limit_mb =
      static_cast<unsigned long long>((table_options.block_size_bytes * 2) /
                                      (1024ULL * 1024ULL));
  const auto query =
      "SET memory_limit='" +
      std::to_string(memory_limit_mb ? memory_limit_mb : 32ULL) + "MB'";
  auto result = con->Query(query);

  if (!result || result->HasError()) {
    teardown_duckdb();
    return HA_ERR_INTERNAL_ERROR;
  }

  std::string error;
  if (!configure_duckdb_for_table(con, table_metadata, &error)) {
    teardown_duckdb();
    return HA_ERR_INTERNAL_ERROR;
  }

  duckdb_initialized = true;
  return 0;
}

void ha_parquet::teardown_duckdb()
{
  scan_result.reset();
  buffer_appender.reset();

  delete con;
  con = nullptr;

  delete db;
  db = nullptr;

  duckdb_initialized = false;
  buffer_table_created = false;
  row_count = 0;
  buffered_bytes = 0;
  current_row = 0;
}
