#define MYSQL_SERVER 1

#include "parquet_shared.h"

#include "parquet_catalog.h"
#include "parquet_metadata.h"
#include "parquet_object_store.h"

#include "log.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

char *parquet_lakekeeper_base_url = nullptr;
char *parquet_lakekeeper_warehouse_id = nullptr;
char *parquet_lakekeeper_namespace = nullptr;
char *parquet_lakekeeper_bearer_token = nullptr;
char *parquet_s3_bucket = nullptr;
char *parquet_s3_data_prefix = nullptr;
char *parquet_s3_region = nullptr;
char *parquet_s3_access_key_id = nullptr;
char *parquet_s3_secret_access_key = nullptr;

namespace {

std::string trim_copy(std::string value)
{
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::string ensure_trailing_slash(const std::string &value)
{
  if (value.empty() || value.back() == '/')
    return value;
  return value + "/";
}

std::string trim_slashes_copy(std::string value)
{
  while (!value.empty() && value.front() == '/')
    value.erase(value.begin());
  while (!value.empty() && value.back() == '/')
    value.pop_back();
  return value;
}

std::string normalize_url_base(std::string value)
{
  while (!value.empty() && value.back() == '/')
    value.pop_back();
  return value;
}

std::string single_line_copy(std::string value)
{
  for (char &ch : value) {
    if (ch == '\n' || ch == '\r' || ch == '\t')
      ch = ' ';
  }
  return trim_copy(value);
}

std::string string_from_sysvar(const char *value, const char *fallback = "")
{
  if (value != nullptr)
    return value;
  return fallback;
}

size_t curl_write_to_string(char *ptr, size_t size, size_t nmemb,
                            void *userdata)
{
  auto *body = static_cast<std::string *>(userdata);
  body->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string namespace_path_from_metadata(const parquet::TableMetadata &metadata)
{
  if (metadata.catalog_table_ident.namespace_ident.parts.empty()) {
    parquet::CatalogNamespaceIdent default_ident{{"default"}};
    return parquet::EncodeNamespaceForUrlPath(default_ident, "%1F");
  }

  return parquet::EncodeNamespaceForUrlPath(
      metadata.catalog_table_ident.namespace_ident, "%1F");
}

void apply_catalog_auth_header(const parquet::CatalogClientConfig &config,
                               struct curl_slist **headers)
{
  if (headers == nullptr || config.bearer_token.empty())
    return;

  *headers = curl_slist_append(
      *headers, ("Authorization: Bearer " + config.bearer_token).c_str());
}

void apply_catalog_curl_options(CURL *curl,
                                const parquet::CatalogClientConfig &config)
{
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, config.connect_timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config.timeout_ms);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, config.verify_peer ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, config.verify_host ? 2L : 0L);
}

std::string duckdb_s3_endpoint_setting(const std::string &endpoint)
{
  auto trimmed = trim_copy(endpoint);
  const std::string https_prefix = "https://";
  const std::string http_prefix = "http://";

  if (trimmed.rfind(https_prefix, 0) == 0)
    trimmed.erase(0, https_prefix.size());
  else if (trimmed.rfind(http_prefix, 0) == 0)
    trimmed.erase(0, http_prefix.size());

  while (!trimmed.empty() && trimmed.back() == '/')
    trimmed.pop_back();

  return trimmed;
}

bool duckdb_s3_use_ssl(const std::string &endpoint)
{
  return endpoint.rfind("http://", 0) != 0;
}

std::string duckdb_s3_url_style(const std::string &url_style)
{
  std::string lowered = url_style;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });

  if (lowered == "virtual-host" || lowered == "virtual_host" ||
      lowered == "vhost") {
    return "vhost";
  }

  return "path";
}

} // namespace

ParquetPluginConfigSnapshot parquet_plugin_config_snapshot()
{
  ParquetPluginConfigSnapshot config;
  config.lakekeeper_base_url =
      ensure_trailing_slash(string_from_sysvar(
          parquet_lakekeeper_base_url, "http://localhost:8181/catalog/v1/"));
  config.lakekeeper_warehouse_id =
      string_from_sysvar(parquet_lakekeeper_warehouse_id);
  config.lakekeeper_namespace =
      string_from_sysvar(parquet_lakekeeper_namespace, "default");
  config.lakekeeper_bearer_token =
      string_from_sysvar(parquet_lakekeeper_bearer_token);
  config.s3_bucket = string_from_sysvar(parquet_s3_bucket);
  config.s3_data_prefix =
      trim_slashes_copy(string_from_sysvar(parquet_s3_data_prefix, "data"));
  config.s3_region = string_from_sysvar(parquet_s3_region, "us-east-2");
  config.s3_access_key_id = string_from_sysvar(parquet_s3_access_key_id);
  config.s3_secret_access_key =
      string_from_sysvar(parquet_s3_secret_access_key);
  return config;
}

std::string parquet_default_s3_endpoint_url(const std::string &region)
{
  if (region.empty() || region == "us-east-1")
    return "https://s3.amazonaws.com";

  return "https://s3." + region + ".amazonaws.com";
}

std::string quote_string_literal(const std::string &value)
{
  std::string quoted = "'";
  for (char ch : value) {
    if (ch == '\'')
      quoted += "''";
    else
      quoted += ch;
  }
  quoted += "'";
  return quoted;
}

std::string parquet_log_preview(const std::string &value, size_t max_length)
{
  std::string normalized = single_line_copy(value);
  if (normalized.size() <= max_length)
    return normalized;

  return normalized.substr(0, max_length) + "...<truncated>";
}

void parquet_log_info(const std::string &message)
{
  sql_print_information("Parquet: %s", message.c_str());
}

void parquet_log_warning(const std::string &message)
{
  sql_print_warning("Parquet: %s", message.c_str());
}

std::string parquet_s3_object_path(const parquet::ObjectStoreConfig &config,
                                   const std::string &object_name)
{
  const auto location = parquet::ResolveObjectLocation(config, object_name);
  return parquet::BuildS3Uri(location.bucket, location.key);
}

bool configure_duckdb_s3(duckdb::Connection *con,
                         const parquet::ObjectStoreConfig &config,
                         std::string *error)
{
  if (con == nullptr) {
    if (error != nullptr)
      *error = "DuckDB connection must not be null";
    return false;
  }

  if (config.bucket.empty()) {
    if (error != nullptr)
      *error = "object store bucket is missing";
    return false;
  }

  if (config.auth_mode == parquet::ObjectStoreAuthMode::kRemoteSigning) {
    if (error != nullptr) {
      *error =
          "object store auth_mode=remote_signing is not supported by the "
          "current Parquet handler path";
    }
    return false;
  }

  if (config.credentials.empty()) {
    if (error != nullptr) {
      *error =
          "object store credentials are missing; configure "
          "parquet_s3_access_key_id and parquet_s3_secret_access_key";
    }
    return false;
  }

  if (config.auth_mode == parquet::ObjectStoreAuthMode::kTemporaryCredentials &&
      config.credentials.session_token.empty()) {
    if (error != nullptr) {
      *error =
          "object store auth_mode=temporary requires a session_token, which "
          "is not currently available through Parquet plugin variables";
    }
    return false;
  }

  parquet_log_info("DuckDB configuring S3 endpoint='" + config.endpoint +
                   "' region='" + config.region + "' bucket='" +
                   config.bucket + "' key_prefix='" + config.key_prefix +
                   "' url_style='" + config.url_style + "'");

  if (!config.region.empty()) {
    con->Query("SET s3_region=" + quote_string_literal(config.region));
  }
  con->Query("SET s3_access_key_id=" +
             quote_string_literal(config.credentials.access_key_id));
  con->Query("SET s3_secret_access_key=" +
             quote_string_literal(config.credentials.secret_access_key));
  if (!config.credentials.session_token.empty()) {
    con->Query("SET s3_session_token=" +
               quote_string_literal(config.credentials.session_token));
  }

  if (!config.endpoint.empty()) {
    con->Query("SET s3_endpoint=" +
               quote_string_literal(duckdb_s3_endpoint_setting(config.endpoint)));
    con->Query(std::string("SET s3_use_ssl=") +
               (duckdb_s3_use_ssl(config.endpoint) ? "true" : "false"));
  }
  con->Query("SET s3_url_style=" +
             quote_string_literal(duckdb_s3_url_style(config.url_style)));
  return true;
}

std::string lakekeeper_table_collection_url(
    const parquet::TableMetadata &metadata)
{
  if (metadata.catalog_config.base_uri.empty() ||
      metadata.catalog_config.warehouse.empty()) {
    return "";
  }

  return normalize_url_base(
             parquet::NormalizeCatalogBaseUri(metadata.catalog_config.base_uri)) +
         "/" + metadata.catalog_config.warehouse + "/namespaces/" +
         namespace_path_from_metadata(metadata) + "/tables";
}

std::string lakekeeper_table_url(const parquet::TableMetadata &metadata)
{
  const auto collection_url = lakekeeper_table_collection_url(metadata);
  if (collection_url.empty() || metadata.catalog_table_ident.table_name.empty())
    return "";

  return collection_url + "/" + metadata.catalog_table_ident.table_name;
}

std::string lakekeeper_transaction_commit_url(
    const parquet::TableMetadata &metadata)
{
  if (metadata.catalog_config.base_uri.empty() ||
      metadata.catalog_config.warehouse.empty()) {
    return "";
  }

  return normalize_url_base(
             parquet::NormalizeCatalogBaseUri(metadata.catalog_config.base_uri)) +
         "/" + metadata.catalog_config.warehouse + "/transactions/commit";
}

std::string table_name_from_path(const std::string &table_path)
{
  size_t pos = table_path.find_last_of("/\\");
  return (pos == std::string::npos) ? table_path : table_path.substr(pos + 1);
}

std::vector<std::string> extract_manifest_paths(const std::string &response_body)
{
  std::vector<std::string> s3_files;
  std::unordered_set<std::string> seen_s3_files;
  size_t pos = 0;
  while ((pos = response_body.find("\"manifest-list\"", pos)) !=
         std::string::npos) {
    size_t colon = response_body.find(':', pos);
    if (colon == std::string::npos)
      break;
    size_t value_start = response_body.find('"', colon + 1);
    if (value_start == std::string::npos)
      break;
    size_t value_end = response_body.find('"', value_start + 1);
    if (value_end == std::string::npos)
      break;

    std::string path = response_body.substr(value_start + 1,
                                            value_end - value_start - 1);
    if (path.rfind("s3://", 0) == 0 && seen_s3_files.insert(path).second)
      s3_files.push_back(path);

    pos = value_end + 1;
  }
  return s3_files;
}

bool fetch_lakekeeper_table_metadata(const parquet::TableMetadata &metadata,
                                     std::string *response_body,
                                     long *http_code)
{
  if (!response_body || !http_code)
    return false;

  response_body->clear();
  *http_code = 0;

  const std::string lakekeeper_url = lakekeeper_table_url(metadata);
  if (lakekeeper_url.empty())
    return false;

  parquet_log_info("LakeKeeper load table metadata table='" +
                   metadata.catalog_table_ident.table_name + "' url='" +
                   lakekeeper_url + "'");

  CURL *curl = curl_easy_init();
  if (!curl)
    return false;

  struct curl_slist *headers = nullptr;
  apply_catalog_auth_header(metadata.catalog_config, &headers);
  curl_easy_setopt(curl, CURLOPT_URL, lakekeeper_url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +curl_write_to_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_body);
  apply_catalog_curl_options(curl, metadata.catalog_config);
  if (headers != nullptr)
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  CURLcode res = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
  if (headers != nullptr)
    curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res == CURLE_OK) {
    parquet_log_info("LakeKeeper load table metadata complete table='" +
                     metadata.catalog_table_ident.table_name +
                     "' http_status=" + std::to_string(*http_code) +
                     " response=" + parquet_log_preview(*response_body));
  } else {
    parquet_log_warning("LakeKeeper load table metadata failed table='" +
                        metadata.catalog_table_ident.table_name + "' error='" +
                        std::string(curl_easy_strerror(res)) + "'");
  }

  return res == CURLE_OK;
}

bool resolve_parquet_data_files(const parquet::TableMetadata &metadata,
                                std::vector<std::string> *s3_files,
                                long *http_code)
{
  if (!s3_files)
    return false;

  std::string response_body;
  long local_http_code = 0;
  if (!fetch_lakekeeper_table_metadata(metadata, &response_body,
                                       &local_http_code)) {
    if (http_code)
      *http_code = local_http_code;
    return false;
  }

  if (http_code)
    *http_code = local_http_code;

  if (local_http_code != 200) {
    s3_files->clear();
    return true;
  }

  *s3_files = extract_manifest_paths(response_body);
  return true;
}

std::string fetch_current_snapshot_data_file(
    const parquet::TableMetadata &metadata)
{
  std::vector<std::string> s3_files;
  long http_code = 0;
  if (!resolve_parquet_data_files(metadata, &s3_files, &http_code))
    return "";
  if (http_code != 200 || s3_files.empty())
    return "";
  return s3_files.front();
}
