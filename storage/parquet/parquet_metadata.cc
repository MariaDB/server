#include "parquet_metadata.h"
#include "parquet_shared.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace
{

using json = nlohmann::json;

constexpr uint64_t PARQUET_DEFAULT_BLOCK_SIZE_BYTES = 16ULL * 1024ULL * 1024ULL;

std::string TrimAsciiWhitespace(const std::string &value)
{
  size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin]))) {
    begin++;
  }

  size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    end--;
  }

  return value.substr(begin, end - begin);
}

std::string ToLowerAscii(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

bool ParseBoolOption(const std::string &value, bool default_value)
{
  const auto lowered = ToLowerAscii(TrimAsciiWhitespace(value));
  if (lowered == "1" || lowered == "true" || lowered == "yes" ||
      lowered == "on") {
    return true;
  }
  if (lowered == "0" || lowered == "false" || lowered == "no" ||
      lowered == "off") {
    return false;
  }
  return default_value;
}

std::string ParentDirectoryName(const std::string &path)
{
  const auto last_separator = path.find_last_of("/\\");
  if (last_separator == std::string::npos || last_separator == 0) {
    return "";
  }

  const auto parent_path = path.substr(0, last_separator);
  const auto parent_separator = parent_path.find_last_of("/\\");
  if (parent_separator == std::string::npos) {
    return parent_path;
  }

  return parent_path.substr(parent_separator + 1);
}

std::string ObjectStoreAuthModeToString(parquet::ObjectStoreAuthMode auth_mode)
{
  switch (auth_mode) {
    case parquet::ObjectStoreAuthMode::kTemporaryCredentials:
      return "temporary";
    case parquet::ObjectStoreAuthMode::kRemoteSigning:
      return "remote_signing";
    case parquet::ObjectStoreAuthMode::kClientManagedStatic:
    default:
      return "static";
  }
}

parquet::ObjectStoreAuthMode ParseObjectStoreAuthMode(const std::string &value)
{
  const auto lowered = ToLowerAscii(TrimAsciiWhitespace(value));
  if (lowered == "temporary" || lowered == "temporary_credentials") {
    return parquet::ObjectStoreAuthMode::kTemporaryCredentials;
  }
  if (lowered == "remote_signing" || lowered == "remote-signing" ||
      lowered == "vended" || lowered == "delegated") {
    return parquet::ObjectStoreAuthMode::kRemoteSigning;
  }
  return parquet::ObjectStoreAuthMode::kClientManagedStatic;
}

json MapToJsonObject(const std::map<std::string, std::string> &map)
{
  json out = json::object();
  for (const auto &entry : map) {
    out[entry.first] = entry.second;
  }
  return out;
}

std::map<std::string, std::string> JsonObjectToMap(const json &value)
{
  std::map<std::string, std::string> out;

  if (!value.is_object()) {
    return out;
  }

  for (auto it = value.begin(); it != value.end(); ++it) {
    if (it.value().is_string()) {
      out[it.key()] = it.value().get<std::string>();
    } else {
      out[it.key()] = it.value().dump();
    }
  }

  return out;
}

json ActiveDataFilesToJson(const std::vector<parquet::ActiveDataFile> &files)
{
  json out = json::array();

  for (const auto &file : files) {
    out.push_back({
        {"path", file.path},
        {"record_count", file.record_count},
        {"file_size_bytes", file.file_size_bytes},
        {"snapshot_id", file.snapshot_id},
        {"data_sequence_number", file.data_sequence_number},
        {"file_sequence_number", file.file_sequence_number},
    });
  }

  return out;
}

std::vector<parquet::ActiveDataFile> JsonToActiveDataFiles(const json &value)
{
  std::vector<parquet::ActiveDataFile> files;

  if (!value.is_array()) {
    return files;
  }

  for (const auto &item : value) {
    if (!item.is_object()) {
      continue;
    }

    parquet::ActiveDataFile file;
    file.path = item.value("path", std::string());
    file.record_count = item.value("record_count", 0ULL);
    file.file_size_bytes = item.value("file_size_bytes", 0ULL);
    file.snapshot_id = item.value("snapshot_id", std::string());
    file.data_sequence_number = item.value("data_sequence_number", 0ULL);
    file.file_sequence_number = item.value("file_sequence_number", 0ULL);
    files.push_back(std::move(file));
  }

  return files;
}

void SetError(std::string *error, const std::string &message)
{
  if (error != nullptr) {
    *error = message;
  }
}

bool MetadataSidecarExists(const std::string &path)
{
  std::ifstream stream(path, std::ios::binary);
  return stream.good();
}

std::vector<std::string> SplitNamespace(const std::string &value)
{
  std::vector<std::string> parts;
  std::stringstream stream(value);
  std::string part;
  while (std::getline(stream, part, '.')) {
    part = TrimAsciiWhitespace(part);
    if (!part.empty()) {
      parts.push_back(part);
    }
  }
  return parts;
}

void ApplyCatalogDefaults(const ParquetPluginConfigSnapshot &defaults,
                          parquet::TableMetadata *metadata)
{
  if (metadata == nullptr) {
    return;
  }

  if (metadata->catalog_config.base_uri.empty()) {
    metadata->catalog_config.base_uri = defaults.lakekeeper_base_url;
  }
  if (metadata->catalog_config.warehouse.empty()) {
    metadata->catalog_config.warehouse = defaults.lakekeeper_warehouse_id;
  }
  if (metadata->catalog_table_ident.namespace_ident.parts.empty()) {
    metadata->catalog_table_ident.namespace_ident.parts =
        !defaults.lakekeeper_namespace.empty()
            ? SplitNamespace(defaults.lakekeeper_namespace)
            : std::vector<std::string>();
  }
  if (metadata->catalog_table_ident.namespace_ident.parts.empty()) {
    if (!metadata->local_paths.database_name.empty()) {
      metadata->catalog_table_ident.namespace_ident.parts.push_back(
          metadata->local_paths.database_name);
    } else {
      metadata->catalog_table_ident.namespace_ident.parts.push_back("default");
    }
  }
  if (metadata->catalog_table_ident.table_name.empty()) {
    metadata->catalog_table_ident.table_name = metadata->local_paths.table_name;
  }

  metadata->catalog_config.bearer_token = defaults.lakekeeper_bearer_token;
}

void ApplyObjectStoreDefaults(const ParquetPluginConfigSnapshot &defaults,
                              parquet::TableMetadata *metadata)
{
  if (metadata == nullptr) {
    return;
  }

  if (metadata->object_store_config.bucket.empty()) {
    metadata->object_store_config.bucket = defaults.s3_bucket;
  }
  if (metadata->object_store_config.key_prefix.empty()) {
    metadata->object_store_config.key_prefix = defaults.s3_data_prefix;
  }
  if (metadata->object_store_config.region.empty()) {
    metadata->object_store_config.region = defaults.s3_region;
  }
  if (metadata->object_store_config.endpoint.empty()) {
    metadata->object_store_config.endpoint =
        parquet_default_s3_endpoint_url(metadata->object_store_config.region);
  }
  if (metadata->object_store_config.url_style.empty()) {
    metadata->object_store_config.url_style = "path";
  }

  metadata->object_store_config.credentials.access_key_id =
      defaults.s3_access_key_id;
  metadata->object_store_config.credentials.secret_access_key =
      defaults.s3_secret_access_key;
  metadata->object_store_config.credentials.session_token.clear();
  metadata->object_store_config.credentials.expiration.clear();
}

void FinalizeResolvedMetadata(parquet::TableMetadata *metadata)
{
  if (metadata == nullptr) {
    return;
  }

  metadata->catalog_enabled = true;
  metadata->object_store_enabled = true;
}

} // namespace

namespace parquet
{

TableOptions ResolveDefaultTableOptions()
{
  return {"latest", PARQUET_DEFAULT_BLOCK_SIZE_BYTES, "gzip"};
}

LocalPaths ResolveLocalPaths(const char *table_path)
{
  LocalPaths paths;

  if (!table_path) {
    return paths;
  }

  paths.table_path = table_path;
  const auto pos = paths.table_path.find_last_of("/\\");

  if (pos == std::string::npos) {
    paths.table_name = paths.table_path;
    paths.helper_db_path = "duckdb_helper.duckdb";
  } else {
    paths.table_name = paths.table_path.substr(pos + 1);
    paths.helper_db_path = paths.table_path.substr(0, pos) +
                           "/duckdb_helper.duckdb";
  }

  paths.database_name = ParentDirectoryName(paths.table_path);
  paths.parquet_file_path = paths.table_path + ".parquet";
  return paths;
}

std::string ResolveMetadataFilePath(const char *table_path)
{
  if (table_path == nullptr) {
    return "";
  }

  return std::string(table_path) + ".parquet.meta";
}

bool ParseKeyValueOptions(const std::string &serialized,
                          std::map<std::string, std::string> *options,
                          std::string *error)
{
  if (options == nullptr) {
    if (error != nullptr) {
      *error = "options output pointer must not be null";
    }
    return false;
  }

  options->clear();
  if (serialized.empty()) {
    return true;
  }

  std::stringstream stream(serialized);
  std::string token;
  while (std::getline(stream, token, ';')) {
    token = TrimAsciiWhitespace(token);
    if (token.empty()) {
      continue;
    }

    const auto equal_sign = token.find('=');
    if (equal_sign == std::string::npos) {
      if (error != nullptr) {
        *error = "connection token is missing '=': " + token;
      }
      return false;
    }

    auto key = ToLowerAscii(TrimAsciiWhitespace(token.substr(0, equal_sign)));
    auto value = TrimAsciiWhitespace(token.substr(equal_sign + 1));

    if (key.empty()) {
      if (error != nullptr) {
        *error = "connection token is missing a key";
      }
      return false;
    }

    (*options)[key] = value;
  }

  return true;
}

CatalogTableIdent ResolveCatalogTableIdent(
    const LocalPaths &local_paths,
    const std::map<std::string, std::string> &options)
{
  CatalogTableIdent ident;

  auto namespace_it = options.find("namespace");
  if (namespace_it != options.end() && !namespace_it->second.empty()) {
    std::stringstream stream(namespace_it->second);
    std::string part;
    while (std::getline(stream, part, '.')) {
      part = TrimAsciiWhitespace(part);
      if (!part.empty()) {
        ident.namespace_ident.parts.push_back(part);
      }
    }
  }

  auto table_it = options.find("table");
  ident.table_name = table_it != options.end() && !table_it->second.empty()
                         ? table_it->second
                         : local_paths.table_name;
  return ident;
}

bool ParseObjectStoreConnectionString(const std::string &serialized,
                                      ObjectStoreConfig *config,
                                      std::string *error)
{
  if (config == nullptr) {
    SetError(error, "object store config must not be null");
    return false;
  }

  std::map<std::string, std::string> options;
  if (!ParseKeyValueOptions(serialized, &options, error)) {
    return false;
  }

  ObjectStoreConfig parsed;
  if (options.count("access_key_id")) {
    SetError(error,
             "CONNECTION must not include access_key_id=...; use parquet "
             "plugin variables for secrets");
    return false;
  }
  if (options.count("secret_access_key")) {
    SetError(error,
             "CONNECTION must not include secret_access_key=...; use parquet "
             "plugin variables for secrets");
    return false;
  }
  if (options.count("session_token")) {
    SetError(error,
             "CONNECTION must not include session_token=...; use parquet "
             "plugin variables for secrets");
    return false;
  }
  if (options.count("expiration")) {
    SetError(error,
             "CONNECTION must not include expiration=...; use parquet plugin "
             "variables for secrets");
    return false;
  }

  parsed.endpoint = options.count("endpoint") ? options["endpoint"] : "";
  parsed.bucket = options.count("bucket") ? options["bucket"] : "";
  parsed.region = options.count("region") ? options["region"] : "";
  parsed.key_prefix = options.count("key_prefix") ? options["key_prefix"] : "";
  parsed.url_style =
      options.count("url_style") ? options["url_style"] : "path";
  parsed.verify_peer =
      !options.count("verify_peer") ||
      ParseBoolOption(options["verify_peer"], true);
  parsed.verify_host =
      !options.count("verify_host") ||
      ParseBoolOption(options["verify_host"], true);
  parsed.auth_mode = options.count("auth_mode")
                         ? ParseObjectStoreAuthMode(options["auth_mode"])
                         : ObjectStoreAuthMode::kClientManagedStatic;

  if (options.count("connect_timeout_ms")) {
    parsed.connect_timeout_ms =
        std::stol(options["connect_timeout_ms"]);
  }
  if (options.count("timeout_ms")) {
    parsed.timeout_ms = std::stol(options["timeout_ms"]);
  }

  *config = parsed;
  return true;
}

bool ParseCatalogConnectionString(const std::string &serialized,
                                  const LocalPaths &local_paths,
                                  CatalogClientConfig *config,
                                  CatalogTableIdent *ident,
                                  std::string *access_delegation,
                                  std::string *error)
{
  if (config == nullptr || ident == nullptr || access_delegation == nullptr) {
    SetError(error, "catalog outputs must not be null");
    return false;
  }

  std::map<std::string, std::string> options;
  if (!ParseKeyValueOptions(serialized, &options, error)) {
    return false;
  }

  CatalogClientConfig parsed;
  if (options.count("bearer_token")) {
    SetError(error,
             "CATALOG must not include bearer_token=...; use parquet plugin "
             "variables for secrets");
    return false;
  }

  parsed.base_uri = options.count("uri") ? options["uri"] : "";
  parsed.warehouse = options.count("warehouse") ? options["warehouse"] : "";
  parsed.prefix = options.count("prefix") ? options["prefix"] : "";

  if (options.count("connect_timeout_ms")) {
    parsed.connect_timeout_ms = std::stol(options["connect_timeout_ms"]);
  }
  if (options.count("timeout_ms")) {
    parsed.timeout_ms = std::stol(options["timeout_ms"]);
  }
  if (options.count("verify_peer")) {
    parsed.verify_peer = ParseBoolOption(options["verify_peer"], true);
  }
  if (options.count("verify_host")) {
    parsed.verify_host = ParseBoolOption(options["verify_host"], true);
  }

  for (const auto &entry : options) {
    if (entry.first == "uri" || entry.first == "warehouse" ||
        entry.first == "prefix" || entry.first == "namespace" ||
        entry.first == "table" || entry.first == "bearer_token" ||
        entry.first == "access_delegation" ||
        entry.first == "connect_timeout_ms" ||
        entry.first == "timeout_ms" || entry.first == "verify_peer" ||
        entry.first == "verify_host") {
      continue;
    }
    parsed.client_properties[entry.first] = entry.second;
  }

  *ident = ResolveCatalogTableIdent(local_paths, options);
  *access_delegation =
      options.count("access_delegation") ? options["access_delegation"] : "";
  *config = parsed;
  return true;
}

bool ResolveCreateTableMetadata(const char *table_path,
                                const char *catalog_serialized,
                                const char *connection_serialized,
                                TableMetadata *metadata,
                                std::string *error)
{
  if (metadata == nullptr) {
    SetError(error, "metadata output pointer must not be null");
    return false;
  }

  TableMetadata resolved;
  resolved.table_options = ResolveDefaultTableOptions();
  resolved.local_paths = ResolveLocalPaths(table_path);
  resolved.metadata_file_path = ResolveMetadataFilePath(table_path);

  if (!ParseCatalogConnectionString(
          catalog_serialized != nullptr ? catalog_serialized : "",
          resolved.local_paths, &resolved.catalog_config,
          &resolved.catalog_table_ident, &resolved.access_delegation, error)) {
    return false;
  }

  if (!ParseObjectStoreConnectionString(
          connection_serialized != nullptr ? connection_serialized : "",
          &resolved.object_store_config, error)) {
    return false;
  }

  const auto defaults = parquet_plugin_config_snapshot();
  ApplyCatalogDefaults(defaults, &resolved);
  ApplyObjectStoreDefaults(defaults, &resolved);
  FinalizeResolvedMetadata(&resolved);

  *metadata = std::move(resolved);
  return true;
}

bool ResolveRuntimeTableMetadata(const char *table_path, TableMetadata *metadata,
                                 std::string *error)
{
  if (metadata == nullptr) {
    SetError(error, "metadata output pointer must not be null");
    return false;
  }

  TableMetadata resolved;
  const auto metadata_path = ResolveMetadataFilePath(table_path);

  if (MetadataSidecarExists(metadata_path)) {
    if (!LoadTableMetadata(table_path, &resolved, error)) {
      return false;
    }
  } else {
    resolved.table_options = ResolveDefaultTableOptions();
    resolved.local_paths = ResolveLocalPaths(table_path);
    resolved.metadata_file_path = metadata_path;
  }

  if (resolved.local_paths.table_path.empty()) {
    resolved.local_paths = ResolveLocalPaths(table_path);
  }
  if (resolved.metadata_file_path.empty()) {
    resolved.metadata_file_path = metadata_path;
  }

  const auto defaults = parquet_plugin_config_snapshot();
  ApplyCatalogDefaults(defaults, &resolved);
  ApplyObjectStoreDefaults(defaults, &resolved);
  FinalizeResolvedMetadata(&resolved);

  *metadata = std::move(resolved);
  return true;
}

bool ValidateCatalogConfig(const TableMetadata &metadata, std::string *error)
{
  if (metadata.catalog_config.base_uri.empty()) {
    SetError(error,
             "Parquet catalog base URI is missing; configure "
             "parquet_lakekeeper_base_url or pass CATALOG='uri=...'");
    return false;
  }
  if (metadata.catalog_config.warehouse.empty()) {
    SetError(error,
             "Parquet catalog warehouse is missing; configure "
             "parquet_lakekeeper_warehouse_id or pass CATALOG='warehouse=...'");
    return false;
  }
  if (metadata.catalog_table_ident.namespace_ident.parts.empty()) {
    SetError(error,
             "Parquet catalog namespace is missing; configure "
             "parquet_lakekeeper_namespace or pass CATALOG='namespace=...'");
    return false;
  }
  if (metadata.catalog_table_ident.table_name.empty()) {
    SetError(error, "Parquet catalog table name is missing");
    return false;
  }
  return true;
}

bool ValidateObjectStoreConfig(const TableMetadata &metadata,
                               bool require_credentials,
                               std::string *error)
{
  if (metadata.object_store_config.bucket.empty()) {
    SetError(error,
             "Parquet object store bucket is missing; configure "
             "parquet_s3_bucket or pass CONNECTION='bucket=...'");
    return false;
  }
  if (metadata.object_store_config.endpoint.empty()) {
    SetError(error,
             "Parquet object store endpoint is missing; pass "
             "CONNECTION='endpoint=...' or configure a region default");
    return false;
  }
  if (metadata.object_store_config.auth_mode ==
      ObjectStoreAuthMode::kRemoteSigning) {
    SetError(error,
             "Parquet object store auth_mode=remote_signing is not supported "
             "by the current handler path");
    return false;
  }
  if (metadata.object_store_config.auth_mode ==
      ObjectStoreAuthMode::kTemporaryCredentials) {
    SetError(error,
             "Parquet object store auth_mode=temporary is not yet supported "
             "by the current handler path");
    return false;
  }
  if (require_credentials &&
      metadata.object_store_config.credentials.empty()) {
    SetError(error,
             "Parquet object store credentials are missing; configure "
             "parquet_s3_access_key_id and parquet_s3_secret_access_key");
    return false;
  }
  return true;
}

bool SaveTableMetadata(const TableMetadata &metadata, std::string *error)
{
  const auto metadata_path = metadata.metadata_file_path.empty()
                                 ? ResolveMetadataFilePath(
                                       metadata.local_paths.table_path.c_str())
                                 : metadata.metadata_file_path;
  if (metadata_path.empty()) {
    if (error != nullptr) {
      *error = "metadata path must not be empty";
    }
    return false;
  }

  json payload;
  payload["table_options"] = {
      {"parquet_version", metadata.table_options.parquet_version},
      {"block_size_bytes", metadata.table_options.block_size_bytes},
      {"compression_codec", metadata.table_options.compression_codec}};
  payload["catalog_enabled"] = metadata.catalog_enabled;
  payload["object_store_enabled"] = metadata.object_store_enabled;
  payload["catalog"] = {
      {"base_uri", metadata.catalog_config.base_uri},
      {"warehouse", metadata.catalog_config.warehouse},
      {"prefix", metadata.catalog_config.prefix},
      {"client_properties",
       MapToJsonObject(metadata.catalog_config.client_properties)},
      {"namespace_parts", metadata.catalog_table_ident.namespace_ident.parts},
      {"table_name", metadata.catalog_table_ident.table_name},
      {"access_delegation", metadata.access_delegation}};
  payload["object_store"] = {
      {"endpoint", metadata.object_store_config.endpoint},
      {"region", metadata.object_store_config.region},
      {"bucket", metadata.object_store_config.bucket},
      {"key_prefix", metadata.object_store_config.key_prefix},
      {"url_style", metadata.object_store_config.url_style},
      {"verify_peer", metadata.object_store_config.verify_peer},
      {"verify_host", metadata.object_store_config.verify_host},
      {"connect_timeout_ms", metadata.object_store_config.connect_timeout_ms},
      {"timeout_ms", metadata.object_store_config.timeout_ms},
      {"auth_mode",
       ObjectStoreAuthModeToString(metadata.object_store_config.auth_mode)}};
  const auto active_scan_paths =
      !metadata.active_scan_paths.empty()
          ? metadata.active_scan_paths
          : [&metadata]() {
              std::vector<std::string> paths;
              for (const auto &file : metadata.active_files) {
                if (!file.path.empty()) {
                  paths.push_back(file.path);
                }
              }
              return paths;
            }();

  payload["table_uuid"] = metadata.table_uuid;
  payload["table_location"] = metadata.table_location;
  payload["current_snapshot_id"] = metadata.current_snapshot_id;
  payload["active_files"] = ActiveDataFilesToJson(metadata.active_files);
  payload["active_scan_paths"] = active_scan_paths;

  std::ofstream stream(metadata_path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    if (error != nullptr) {
      *error = "failed to open metadata sidecar for writing";
    }
    return false;
  }

  stream << payload.dump(2);
  if (!stream.good()) {
    if (error != nullptr) {
      *error = "failed to write metadata sidecar";
    }
    return false;
  }

  return true;
}

bool LoadTableMetadata(const char *table_path, TableMetadata *metadata,
                       std::string *error)
{
  if (metadata == nullptr) {
    if (error != nullptr) {
      *error = "metadata output pointer must not be null";
    }
    return false;
  }

  const auto metadata_path = ResolveMetadataFilePath(table_path);
  std::ifstream stream(metadata_path, std::ios::binary);
  if (!stream) {
    if (error != nullptr) {
      *error = "metadata sidecar does not exist";
    }
    return false;
  }

  try {
    auto payload = json::parse(stream);
    TableMetadata loaded;
    loaded.local_paths = ResolveLocalPaths(table_path);
    loaded.metadata_file_path = metadata_path;
    loaded.table_options = ResolveDefaultTableOptions();

    if (payload.contains("table_options")) {
      const auto &table_options = payload["table_options"];
      if (table_options.contains("parquet_version")) {
        loaded.table_options.parquet_version =
            table_options["parquet_version"].get<std::string>();
      }
      if (table_options.contains("block_size_bytes")) {
        loaded.table_options.block_size_bytes =
            table_options["block_size_bytes"].get<uint64_t>();
      }
      if (table_options.contains("compression_codec")) {
        loaded.table_options.compression_codec =
            table_options["compression_codec"].get<std::string>();
      }
    }

    loaded.catalog_enabled =
        payload.contains("catalog_enabled") &&
        payload["catalog_enabled"].get<bool>();
    loaded.object_store_enabled =
        payload.contains("object_store_enabled") &&
        payload["object_store_enabled"].get<bool>();

    if (payload.contains("catalog") && payload["catalog"].is_object()) {
      const auto &catalog = payload["catalog"];
      loaded.catalog_config.base_uri =
          catalog.value("base_uri", std::string());
      loaded.catalog_config.warehouse =
          catalog.value("warehouse", std::string());
      loaded.catalog_config.prefix =
          catalog.value("prefix", std::string());
      if (catalog.contains("client_properties")) {
        loaded.catalog_config.client_properties =
            JsonObjectToMap(catalog["client_properties"]);
      }
      if (catalog.contains("namespace_parts")) {
        loaded.catalog_table_ident.namespace_ident.parts =
            catalog["namespace_parts"].get<std::vector<std::string>>();
      }
      loaded.catalog_table_ident.table_name =
          catalog.value("table_name", loaded.local_paths.table_name);
      loaded.access_delegation =
          catalog.value("access_delegation", std::string());
    }

    if (payload.contains("object_store") && payload["object_store"].is_object()) {
      const auto &object_store = payload["object_store"];
      loaded.object_store_config.endpoint =
          object_store.value("endpoint", std::string());
      loaded.object_store_config.region =
          object_store.value("region", std::string());
      loaded.object_store_config.bucket =
          object_store.value("bucket", std::string());
      loaded.object_store_config.key_prefix =
          object_store.value("key_prefix", std::string());
      loaded.object_store_config.url_style =
          object_store.value("url_style", std::string("path"));
      loaded.object_store_config.verify_peer =
          object_store.value("verify_peer", true);
      loaded.object_store_config.verify_host =
          object_store.value("verify_host", true);
      loaded.object_store_config.connect_timeout_ms =
          object_store.value("connect_timeout_ms", 10000L);
      loaded.object_store_config.timeout_ms =
          object_store.value("timeout_ms", 30000L);
      loaded.object_store_config.auth_mode = ParseObjectStoreAuthMode(
          object_store.value("auth_mode", std::string("static")));
    }

    loaded.table_uuid = payload.value("table_uuid", std::string());
    loaded.table_location =
        payload.value("table_location", std::string());
    loaded.current_snapshot_id =
        payload.value("current_snapshot_id", std::string());
    if (payload.contains("active_files")) {
      loaded.active_files = JsonToActiveDataFiles(payload["active_files"]);
    }
    if (payload.contains("active_scan_paths")) {
      loaded.active_scan_paths =
          payload["active_scan_paths"].get<std::vector<std::string>>();
    }
    if (loaded.active_scan_paths.empty() && !loaded.active_files.empty()) {
      for (const auto &file : loaded.active_files) {
        if (!file.path.empty()) {
          loaded.active_scan_paths.push_back(file.path);
        }
      }
    }
    if (loaded.active_files.empty() && !loaded.active_scan_paths.empty()) {
      for (const auto &path : loaded.active_scan_paths) {
        ActiveDataFile file;
        file.path = path;
        file.snapshot_id = loaded.current_snapshot_id;
        loaded.active_files.push_back(std::move(file));
      }
    }

    *metadata = std::move(loaded);
    return true;
  } catch (const std::exception &ex) {
    if (error != nullptr) {
      *error = ex.what();
    }
    return false;
  }
}

} // namespace parquet
