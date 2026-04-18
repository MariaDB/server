#include "parquet_metadata.h"

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
  } else if (!local_paths.database_name.empty()) {
    ident.namespace_ident.parts.push_back(local_paths.database_name);
  } else {
    ident.namespace_ident.parts.push_back("default");
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
    if (error != nullptr) {
      *error = "object store config must not be null";
    }
    return false;
  }

  std::map<std::string, std::string> options;
  if (!ParseKeyValueOptions(serialized, &options, error)) {
    return false;
  }

  ObjectStoreConfig parsed;
  auto endpoint_it = options.find("endpoint");
  auto bucket_it = options.find("bucket");

  if (endpoint_it == options.end() || endpoint_it->second.empty()) {
    if (error != nullptr) {
      *error = "CONNECTION must include endpoint=...";
    }
    return false;
  }

  if (bucket_it == options.end() || bucket_it->second.empty()) {
    if (error != nullptr) {
      *error = "CONNECTION must include bucket=...";
    }
    return false;
  }

  parsed.endpoint = endpoint_it->second;
  parsed.bucket = bucket_it->second;
  parsed.region =
      options.count("region") ? options["region"] : "us-east-1";
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

  parsed.credentials.access_key_id =
      options.count("access_key_id") ? options["access_key_id"] : "";
  parsed.credentials.secret_access_key =
      options.count("secret_access_key") ? options["secret_access_key"] : "";
  parsed.credentials.session_token =
      options.count("session_token") ? options["session_token"] : "";
  parsed.credentials.expiration =
      options.count("expiration") ? options["expiration"] : "";

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
    if (error != nullptr) {
      *error = "catalog outputs must not be null";
    }
    return false;
  }

  std::map<std::string, std::string> options;
  if (!ParseKeyValueOptions(serialized, &options, error)) {
    return false;
  }

  CatalogClientConfig parsed;
  auto uri_it = options.find("uri");
  if (uri_it == options.end() || uri_it->second.empty()) {
    if (error != nullptr) {
      *error = "CATALOG must include uri=...";
    }
    return false;
  }

  parsed.base_uri = uri_it->second;
  parsed.warehouse = options.count("warehouse") ? options["warehouse"] : "";
  parsed.prefix = options.count("prefix") ? options["prefix"] : "";
  parsed.bearer_token =
      options.count("bearer_token") ? options["bearer_token"] : "";

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
      {"bearer_token", metadata.catalog_config.bearer_token},
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
       ObjectStoreAuthModeToString(metadata.object_store_config.auth_mode)},
      {"credentials",
       {
           {"access_key_id",
            metadata.object_store_config.credentials.access_key_id},
           {"secret_access_key",
            metadata.object_store_config.credentials.secret_access_key},
           {"session_token",
            metadata.object_store_config.credentials.session_token},
           {"expiration", metadata.object_store_config.credentials.expiration},
       }}};
  payload["table_location"] = metadata.table_location;
  payload["active_scan_paths"] = metadata.active_scan_paths;

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
      loaded.catalog_config.bearer_token =
          catalog.value("bearer_token", std::string());
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

      if (object_store.contains("credentials") &&
          object_store["credentials"].is_object()) {
        const auto &credentials = object_store["credentials"];
        loaded.object_store_config.credentials.access_key_id =
            credentials.value("access_key_id", std::string());
        loaded.object_store_config.credentials.secret_access_key =
            credentials.value("secret_access_key", std::string());
        loaded.object_store_config.credentials.session_token =
            credentials.value("session_token", std::string());
        loaded.object_store_config.credentials.expiration =
            credentials.value("expiration", std::string());
      }
    }

    loaded.table_location =
        payload.value("table_location", std::string());
    if (payload.contains("active_scan_paths")) {
      loaded.active_scan_paths =
          payload["active_scan_paths"].get<std::vector<std::string>>();
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
