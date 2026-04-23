#ifndef PARQUET_SHARED_INCLUDED
#define PARQUET_SHARED_INCLUDED

#define MYSQL_SERVER 1

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace parquet
{
struct ObjectStoreConfig;
struct TableMetadata;
}

extern char *parquet_lakekeeper_base_url;
extern char *parquet_lakekeeper_warehouse_id;
extern char *parquet_lakekeeper_namespace;
extern char *parquet_lakekeeper_bearer_token;
extern char *parquet_s3_bucket;
extern char *parquet_s3_data_prefix;
extern char *parquet_s3_region;
extern char *parquet_s3_access_key_id;
extern char *parquet_s3_secret_access_key;

struct ParquetPluginConfigSnapshot {
  std::string lakekeeper_base_url;
  std::string lakekeeper_warehouse_id;
  std::string lakekeeper_namespace;
  std::string lakekeeper_bearer_token;
  std::string s3_bucket;
  std::string s3_data_prefix;
  std::string s3_region;
  std::string s3_access_key_id;
  std::string s3_secret_access_key;
};

ParquetPluginConfigSnapshot parquet_plugin_config_snapshot();
std::string parquet_default_s3_endpoint_url(const std::string &region);

std::string quote_string_literal(const std::string &value);
std::string parquet_log_preview(const std::string &value,
                                size_t max_length = 240);
void parquet_log_info(const std::string &message);
void parquet_log_warning(const std::string &message);
std::string parquet_s3_object_path(const parquet::ObjectStoreConfig &config,
                                   const std::string &object_name);
bool configure_duckdb_s3(duckdb::Connection *con,
                         const parquet::ObjectStoreConfig &config,
                         std::string *error);

std::string lakekeeper_table_collection_url(
    const parquet::TableMetadata &metadata);
std::string lakekeeper_table_url(const parquet::TableMetadata &metadata);
std::string lakekeeper_transaction_commit_url(
    const parquet::TableMetadata &metadata);

std::string table_name_from_path(const std::string &table_path);
std::vector<std::string> extract_manifest_paths(const std::string &response_body);

bool fetch_lakekeeper_table_metadata(const parquet::TableMetadata &metadata,
                                     std::string *response_body,
                                     long *http_code);
bool resolve_parquet_data_files(const parquet::TableMetadata &metadata,
                                std::vector<std::string> *s3_files,
                                long *http_code = nullptr);
std::string fetch_current_snapshot_data_file(
    const parquet::TableMetadata &metadata);

#endif
