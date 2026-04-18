#define MYSQL_SERVER 1

#include "my_global.h"

#include "parquet_catalog.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <sstream>
#include <utility>

namespace parquet
{

namespace
{

using json = nlohmann::json;

struct HttpResponse {
  CURLcode curl_code = CURLE_OK;
  long status_code = 0;
  std::string body;
  std::map<std::string, std::string> headers;
  std::string curl_error;
};

std::once_flag curl_init_once;
std::atomic<uint64_t> idempotency_counter{1};

void EnsureCurlInitialized()
{
  std::call_once(curl_init_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t WriteBodyCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  auto *body = static_cast<std::string *>(userdata);
  const auto bytes = size * nmemb;
  body->append(ptr, bytes);
  return bytes;
}

std::string TrimAsciiWhitespace(const std::string &input)
{
  size_t begin = 0;
  while (begin < input.size() &&
         std::isspace(static_cast<unsigned char>(input[begin]))) {
    begin++;
  }

  size_t end = input.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    end--;
  }

  return input.substr(begin, end - begin);
}

std::string ToLowerAscii(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

size_t HeaderCallback(char *buffer, size_t size, size_t nitems, void *userdata)
{
  auto *headers = static_cast<std::map<std::string, std::string> *>(userdata);
  const auto bytes = size * nitems;
  std::string line(buffer, bytes);
  const auto colon = line.find(':');

  if (colon != std::string::npos) {
    auto key = ToLowerAscii(TrimAsciiWhitespace(line.substr(0, colon)));
    auto value = TrimAsciiWhitespace(line.substr(colon + 1));
    headers->insert_or_assign(key, value);
  }

  return bytes;
}

bool IsUnreservedUrlByte(unsigned char ch)
{
  return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

std::string PercentEncode(const std::string &value)
{
  static const char kHex[] = "0123456789ABCDEF";

  std::string encoded;
  encoded.reserve(value.size() * 3);

  for (unsigned char ch : value) {
    if (IsUnreservedUrlByte(ch)) {
      encoded.push_back(static_cast<char>(ch));
      continue;
    }

    encoded.push_back('%');
    encoded.push_back(kHex[(ch >> 4) & 0x0F]);
    encoded.push_back(kHex[ch & 0x0F]);
  }

  return encoded;
}

std::string EncodePathSegments(const std::string &path)
{
  std::stringstream stream(path);
  std::string segment;
  std::string encoded;
  bool first = true;

  while (std::getline(stream, segment, '/')) {
    if (!first) {
      encoded += "/";
    }
    first = false;
    encoded += PercentEncode(segment);
  }

  if (!path.empty() && path.back() == '/') {
    encoded += "/";
  }

  return encoded;
}

CatalogStatus MakeStatus(CatalogStatusCode code, long http_status,
                         bool retryable, bool commit_state_unknown,
                         std::string message)
{
  CatalogStatus status;
  status.code = code;
  status.http_status = http_status;
  status.retryable = retryable;
  status.commit_state_unknown = commit_state_unknown;
  status.message = std::move(message);
  return status;
}

CatalogStatus HttpStatusToCatalogStatus(long http_status,
                                        const std::string &message)
{
  if (http_status >= 200 && http_status < 300) {
    return MakeStatus(CatalogStatusCode::kOk, http_status, false, false,
                      message);
  }

  if (http_status == 400) {
    return MakeStatus(CatalogStatusCode::kInvalidArgument, http_status, false,
                      false, message);
  }

  if (http_status == 401) {
    return MakeStatus(CatalogStatusCode::kUnauthorized, http_status, false,
                      false, message);
  }

  if (http_status == 403) {
    return MakeStatus(CatalogStatusCode::kForbidden, http_status, false, false,
                      message);
  }

  if (http_status == 404) {
    return MakeStatus(CatalogStatusCode::kNotFound, http_status, false, false,
                      message);
  }

  if (http_status == 409) {
    return MakeStatus(CatalogStatusCode::kConflict, http_status, true, false,
                      message);
  }

  return MakeStatus(CatalogStatusCode::kServerError, http_status,
                    http_status >= 500, false, message);
}

CatalogStatus CommitHttpStatusToCatalogStatus(long http_status,
                                              const std::string &message)
{
  if (http_status == 500 || http_status == 502 || http_status == 504) {
    return MakeStatus(CatalogStatusCode::kCommitStateUnknown, http_status, true,
                      true, message);
  }

  return HttpStatusToCatalogStatus(http_status, message);
}

std::string ExtractErrorMessage(const HttpResponse &response)
{
  if (response.body.empty()) {
    return "";
  }

  try {
    auto payload = json::parse(response.body);

    if (payload.contains("error")) {
      const auto &error = payload["error"];
      if (error.is_object() && error.contains("message") &&
          error["message"].is_string()) {
        return error["message"].get<std::string>();
      }
    }

    if (payload.contains("message") && payload["message"].is_string()) {
      return payload["message"].get<std::string>();
    }
  } catch (const std::exception &) {
  }

  return response.body;
}

void ApplyCommonCurlOptions(CURL *curl, const CatalogClientConfig &config,
                            HttpResponse *response)
{
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, config.connect_timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config.timeout_ms);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, config.verify_peer ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, config.verify_host ? 2L : 0L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBodyCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response->body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response->headers);
}

HttpResponse ExecuteRequest(const CatalogClientConfig &config,
                            const std::string &method,
                            const std::string &url,
                            const std::string &body,
                            const std::vector<std::string> &headers)
{
  EnsureCurlInitialized();

  HttpResponse response;
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    response.curl_code = CURLE_FAILED_INIT;
    response.curl_error = "curl_easy_init failed";
    return response;
  }

  char error_buffer[CURL_ERROR_SIZE] = {0};
  struct curl_slist *curl_headers = nullptr;

  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());

  ApplyCommonCurlOptions(curl, config, &response);

  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));
  }

  for (const auto &header : headers) {
    curl_headers = curl_slist_append(curl_headers, header.c_str());
  }

  if (curl_headers != nullptr) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);
  }

  response.curl_code = curl_easy_perform(curl);
  if (response.curl_code != CURLE_OK) {
    response.curl_error = error_buffer[0] ? error_buffer
                                          : curl_easy_strerror(response.curl_code);
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);

  curl_slist_free_all(curl_headers);
  curl_easy_cleanup(curl);
  return response;
}

std::string JoinPath(const std::string &lhs, const std::string &rhs)
{
  if (lhs.empty()) {
    return rhs;
  }

  if (rhs.empty()) {
    return lhs;
  }

  if (lhs.back() == '/' && rhs.front() == '/') {
    return lhs + rhs.substr(1);
  }

  if (lhs.back() != '/' && rhs.front() != '/') {
    return lhs + "/" + rhs;
  }

  return lhs + rhs;
}

std::string NormalizePrefix(const std::string &prefix)
{
  if (prefix.empty()) {
    return "";
  }

  std::stringstream stream(prefix);
  std::string segment;
  std::string normalized;

  while (std::getline(stream, segment, '/')) {
    if (segment.empty()) {
      continue;
    }

    normalized = JoinPath(normalized, PercentEncode(segment));
  }

  return normalized;
}

std::string BuildApiBasePath(const std::string &prefix)
{
  const auto normalized_prefix = NormalizePrefix(prefix);
  if (normalized_prefix.empty()) {
    return "/v1";
  }

  return "/v1/" + normalized_prefix;
}

std::string BuildNamespacePath(const std::string &prefix,
                               const CatalogNamespaceIdent &ident,
                               const std::string &encoded_separator)
{
  return BuildApiBasePath(prefix) + "/namespaces/" +
         EncodeNamespaceForUrlPath(ident, encoded_separator);
}

std::string BuildTablePath(const std::string &prefix,
                           const CatalogTableIdent &ident,
                           const std::string &encoded_separator)
{
  return BuildNamespacePath(prefix, ident.namespace_ident, encoded_separator) +
         "/tables/" + PercentEncode(ident.table_name);
}

std::vector<std::string> BuildJsonHeaders(const CatalogClientConfig &config,
                                          const std::string &bearer_token,
                                          const std::string &idempotency_key,
                                          const std::string &if_match,
                                          const std::string &access_delegation)
{
  std::vector<std::string> headers;
  headers.emplace_back("Accept: application/json");

  const auto token =
      !bearer_token.empty() ? bearer_token : config.bearer_token;
  if (!token.empty()) {
    headers.emplace_back("Authorization: Bearer " + token);
  }

  if (!idempotency_key.empty()) {
    headers.emplace_back("X-Iceberg-Idempotency-Key: " + idempotency_key);
  }

  if (!if_match.empty()) {
    headers.emplace_back("If-Match: " + if_match);
  }

  if (!access_delegation.empty()) {
    headers.emplace_back("X-Iceberg-Access-Delegation: " + access_delegation);
  }

  return headers;
}

std::string JsonScalarToString(const json &value)
{
  if (value.is_string()) {
    return value.get<std::string>();
  }

  if (value.is_boolean()) {
    return value.get<bool>() ? "true" : "false";
  }

  if (value.is_number_integer()) {
    return std::to_string(value.get<long long>());
  }

  if (value.is_number_unsigned()) {
    return std::to_string(value.get<unsigned long long>());
  }

  if (value.is_number_float()) {
    return value.dump();
  }

  return value.dump();
}

std::map<std::string, std::string> JsonObjectToStringMap(const json &value)
{
  std::map<std::string, std::string> out;

  if (!value.is_object()) {
    return out;
  }

  for (auto it = value.begin(); it != value.end(); ++it) {
    out[it.key()] = JsonScalarToString(it.value());
  }

  return out;
}

CatalogLoadTableResult ParseLoadTableResponse(const HttpResponse &response)
{
  CatalogLoadTableResult result;

  if (response.curl_code != CURLE_OK) {
    result.status = MakeStatus(CatalogStatusCode::kTransportError, 0, true,
                               false, response.curl_error);
    return result;
  }

  result.status =
      HttpStatusToCatalogStatus(response.status_code, ExtractErrorMessage(response));
  if (!result.status.ok()) {
    return result;
  }

  try {
    auto payload = json::parse(response.body.empty() ? "{}" : response.body);

    if (payload.contains("config")) {
      result.config.properties = JsonObjectToStringMap(payload["config"]);
      auto token_it = result.config.properties.find("token");
      if (token_it != result.config.properties.end()) {
        result.config.bearer_token = token_it->second;
      }
    }

    if (payload.contains("metadata") && payload["metadata"].is_object()) {
      const auto &metadata = payload["metadata"];
      result.metadata.raw_metadata_json = metadata.dump();

      if (metadata.contains("table-uuid")) {
        result.metadata.table_uuid = JsonScalarToString(metadata["table-uuid"]);
      }
      if (metadata.contains("location")) {
        result.metadata.table_location =
            JsonScalarToString(metadata["location"]);
      }
      if (metadata.contains("format-version") &&
          metadata["format-version"].is_number_integer()) {
        result.metadata.format_version =
            metadata["format-version"].get<int>();
      }
      if (metadata.contains("current-snapshot-id")) {
        result.metadata.current_snapshot_id =
            JsonScalarToString(metadata["current-snapshot-id"]);
      }
      if (metadata.contains("schemas")) {
        result.metadata.schema_json = metadata["schemas"].dump();
      } else if (metadata.contains("schema")) {
        result.metadata.schema_json = metadata["schema"].dump();
      }
      if (metadata.contains("partition-specs")) {
        result.metadata.partition_spec_json =
            metadata["partition-specs"].dump();
      } else if (metadata.contains("partition-spec")) {
        result.metadata.partition_spec_json =
            metadata["partition-spec"].dump();
      }
      if (metadata.contains("sort-orders")) {
        result.metadata.sort_order_json = metadata["sort-orders"].dump();
      } else if (metadata.contains("sort-order")) {
        result.metadata.sort_order_json = metadata["sort-order"].dump();
      }
    }

    auto etag_it = response.headers.find("etag");
    if (etag_it != response.headers.end()) {
      result.etag = etag_it->second;
    }
  } catch (const std::exception &ex) {
    result.status = MakeStatus(CatalogStatusCode::kInvalidResponse,
                               response.status_code, false, false, ex.what());
  }

  return result;
}

CatalogStatus ParseBasicSuccessOrError(const HttpResponse &response)
{
  if (response.curl_code != CURLE_OK) {
    return MakeStatus(CatalogStatusCode::kTransportError, 0, true, false,
                      response.curl_error);
  }

  return HttpStatusToCatalogStatus(response.status_code,
                                   ExtractErrorMessage(response));
}

CatalogStatus ParseCommitSuccessOrError(const HttpResponse &response)
{
  if (response.curl_code != CURLE_OK) {
    return MakeStatus(CatalogStatusCode::kTransportError, 0, true, false,
                      response.curl_error);
  }

  return CommitHttpStatusToCatalogStatus(response.status_code,
                                         ExtractErrorMessage(response));
}

std::string GenerateIdempotencyKey()
{
  const auto counter = idempotency_counter.fetch_add(1);
  return "parquet-" + std::to_string(counter);
}

bool EndpointMatches(const std::string &endpoint, const char *method,
                     const char *path_suffix)
{
  const auto normalized = ToLowerAscii(endpoint);
  const auto method_prefix = ToLowerAscii(std::string(method)) + " ";

  return normalized.find(method_prefix) == 0 &&
         normalized.find(ToLowerAscii(path_suffix)) != std::string::npos;
}

CatalogPlannedFile ExtractPlannedFile(const json &task)
{
  const auto *source = &task;

  if (task.is_object()) {
    if (task.contains("data-file") && task["data-file"].is_object()) {
      source = &task["data-file"];
    } else if (task.contains("content-file") && task["content-file"].is_object()) {
      source = &task["content-file"];
    } else if (task.contains("file") && task["file"].is_object()) {
      source = &task["file"];
    }
  }

  CatalogPlannedFile file;

  if (source->is_object()) {
    if (source->contains("file-path")) {
      file.file_path = JsonScalarToString((*source)["file-path"]);
    } else if (source->contains("file_path")) {
      file.file_path = JsonScalarToString((*source)["file_path"]);
    } else if (source->contains("path")) {
      file.file_path = JsonScalarToString((*source)["path"]);
    }

    if (source->contains("file-size-in-bytes")) {
      file.file_size_bytes =
          static_cast<uint64_t>((*source)["file-size-in-bytes"].get<long long>());
    } else if (source->contains("file_size_in_bytes")) {
      file.file_size_bytes =
          static_cast<uint64_t>((*source)["file_size_in_bytes"].get<long long>());
    } else if (source->contains("length")) {
      file.file_size_bytes =
          static_cast<uint64_t>((*source)["length"].get<long long>());
    }

    if (source->contains("partition")) {
      file.partition_json = (*source)["partition"].dump();
    } else if (source->contains("partition-data")) {
      file.partition_json = (*source)["partition-data"].dump();
    } else if (source->contains("partition_data")) {
      file.partition_json = (*source)["partition_data"].dump();
    }
  }

  if (task.is_object()) {
    if (task.contains("residual-filter")) {
      file.residual_predicate_json = task["residual-filter"].dump();
    } else if (task.contains("residual_filter")) {
      file.residual_predicate_json = task["residual_filter"].dump();
    } else if (task.contains("residual")) {
      file.residual_predicate_json = task["residual"].dump();
    }
  }

  return file;
}

void ExtractPlanFilesFromPayload(const json &payload,
                                 std::vector<CatalogPlannedFile> *files,
                                 std::string *residual_predicate_json)
{
  const auto append_array = [&](const json &array) {
    if (!array.is_array()) {
      return;
    }

    for (const auto &entry : array) {
      auto file = ExtractPlannedFile(entry);
      if (!file.file_path.empty()) {
        files->push_back(std::move(file));
      }
    }
  };

  if (payload.is_object()) {
    if (payload.contains("residual-filter")) {
      *residual_predicate_json = payload["residual-filter"].dump();
    } else if (payload.contains("residual_filter")) {
      *residual_predicate_json = payload["residual_filter"].dump();
    } else if (payload.contains("residual")) {
      *residual_predicate_json = payload["residual"].dump();
    }

    if (payload.contains("files")) {
      append_array(payload["files"]);
    }
    if (payload.contains("scan-tasks")) {
      append_array(payload["scan-tasks"]);
    }
    if (payload.contains("scanTasks")) {
      append_array(payload["scanTasks"]);
    }
    if (payload.contains("plan-tasks")) {
      append_array(payload["plan-tasks"]);
    }
    if (payload.contains("planTasks")) {
      append_array(payload["planTasks"]);
    }
    if (payload.contains("plan") && payload["plan"].is_object()) {
      ExtractPlanFilesFromPayload(payload["plan"], files,
                                  residual_predicate_json);
    }
  }
}

} // namespace

std::string NormalizeCatalogBaseUri(const std::string &base_uri)
{
  std::string normalized = base_uri;

  while (!normalized.empty() && normalized.back() == '/') {
    normalized.pop_back();
  }

  return normalized;
}

CatalogCapabilitySet ResolveCatalogCapabilities(
    const std::vector<std::string> &advertised_endpoints)
{
  CatalogCapabilitySet capabilities;
  capabilities.advertised_endpoints = advertised_endpoints;

  if (advertised_endpoints.empty()) {
    capabilities.supports_create_table = true;
    capabilities.supports_commit_table = true;
    capabilities.supports_commit_transaction = true;
    capabilities.supports_register_table = true;
    capabilities.supports_scan_planning = false;
    return capabilities;
  }

  capabilities.supports_create_table = false;
  capabilities.supports_commit_table = false;
  capabilities.supports_commit_transaction = false;
  capabilities.supports_register_table = false;
  capabilities.supports_scan_planning = false;

  for (const auto &endpoint : advertised_endpoints) {
    if (EndpointMatches(endpoint, "POST",
                        "/namespaces/{namespace}/tables")) {
      capabilities.supports_create_table = true;
    }

    if (EndpointMatches(endpoint, "POST",
                        "/namespaces/{namespace}/tables/{table}")) {
      capabilities.supports_commit_table = true;
    }

    if (EndpointMatches(endpoint, "POST", "/transactions/commit")) {
      capabilities.supports_commit_transaction = true;
    }

    if (EndpointMatches(endpoint, "POST",
                        "/namespaces/{namespace}/register")) {
      capabilities.supports_register_table = true;
    }

    if (EndpointMatches(endpoint, "POST",
                        "/namespaces/{namespace}/tables/{table}/plan")) {
      capabilities.supports_scan_planning = true;
    }
  }

  return capabilities;
}

std::string EncodeNamespaceForUrlPath(const CatalogNamespaceIdent &ident,
                                      const std::string &encoded_separator)
{
  std::string encoded;
  bool first = true;

  for (const auto &part : ident.parts) {
    if (!first) {
      encoded += encoded_separator;
    }
    first = false;
    encoded += PercentEncode(part);
  }

  return encoded;
}

ParquetCatalogClient::ParquetCatalogClient(CatalogClientConfig config)
    : config_(std::move(config))
{
  config_.base_uri = NormalizeCatalogBaseUri(config_.base_uri);
  prefix_ = config_.prefix;
}

CatalogStatus ParquetCatalogClient::BootstrapConfig()
{
  std::string url = JoinPath(config_.base_uri, "/v1/config");
  if (!config_.warehouse.empty()) {
    url += "?warehouse=" + PercentEncode(config_.warehouse);
  }

  auto headers = BuildJsonHeaders(config_, "", "", "", "");
  auto response = ExecuteRequest(config_, "GET", url, "", headers);
  auto status = ParseBasicSuccessOrError(response);
  if (!status.ok()) {
    return status;
  }

  try {
    auto payload = json::parse(response.body.empty() ? "{}" : response.body);
    effective_config_.clear();

    if (payload.contains("defaults")) {
      effective_config_ = JsonObjectToStringMap(payload["defaults"]);
    }

    for (const auto &entry : config_.client_properties) {
      effective_config_[entry.first] = entry.second;
    }

    if (payload.contains("overrides")) {
      auto overrides = JsonObjectToStringMap(payload["overrides"]);
      effective_config_.insert(overrides.begin(), overrides.end());
      for (const auto &entry : overrides) {
        effective_config_[entry.first] = entry.second;
      }
    }

    std::vector<std::string> endpoints;
    if (payload.contains("endpoints") && payload["endpoints"].is_array()) {
      for (const auto &endpoint : payload["endpoints"]) {
        if (endpoint.is_string()) {
          endpoints.push_back(endpoint.get<std::string>());
        }
      }
    }

    capabilities_ = ResolveCatalogCapabilities(endpoints);

    auto ns_it = effective_config_.find("namespace-separator");
    namespace_separator_ =
        ns_it != effective_config_.end() ? ns_it->second : "%1F";

    auto prefix_it = effective_config_.find("prefix");
    if (prefix_it != effective_config_.end()) {
      prefix_ = prefix_it->second;
    }

    auto token_it = effective_config_.find("token");
    if (config_.bearer_token.empty() && token_it != effective_config_.end()) {
      config_.bearer_token = token_it->second;
    }

    bootstrapped_ = true;
    return MakeStatus(CatalogStatusCode::kOk, response.status_code, false, false,
                      "");
  } catch (const std::exception &ex) {
    return MakeStatus(CatalogStatusCode::kInvalidResponse,
                      response.status_code, false, false, ex.what());
  }
}

CatalogStatus ParquetCatalogClient::EnsureNamespace(
    const CatalogNamespaceIdent &ident,
    const std::map<std::string, std::string> &properties)
{
  if (ident.parts.empty()) {
    return MakeStatus(CatalogStatusCode::kInvalidArgument, 0, false, false,
                      "namespace must not be empty");
  }

  const auto url =
      JoinPath(config_.base_uri,
               BuildNamespacePath(prefix_, ident, namespace_separator_));
  auto headers = BuildJsonHeaders(config_, "", "", "", "");
  auto response = ExecuteRequest(config_, "HEAD", url, "", headers);
  auto status = ParseBasicSuccessOrError(response);

  if (status.ok()) {
    return status;
  }

  if (status.code != CatalogStatusCode::kNotFound) {
    return status;
  }

  json body;
  body["namespace"] = ident.parts;
  if (!properties.empty()) {
    body["properties"] = properties;
  }

  headers = BuildJsonHeaders(config_, "", GenerateIdempotencyKey(), "", "");
  headers.emplace_back("Content-Type: application/json");
  response = ExecuteRequest(config_, "POST",
                            JoinPath(config_.base_uri,
                                     BuildApiBasePath(prefix_) + "/namespaces"),
                            body.dump(), headers);
  return ParseBasicSuccessOrError(response);
}

CatalogStatus ParquetCatalogClient::CreateTable(
    const CatalogCreateTableRequest &request, CatalogLoadTableResult *result)
{
  if (!capabilities_.supports_create_table) {
    return MakeStatus(CatalogStatusCode::kUnsupported, 0, false, false,
                      "catalog does not advertise create table support");
  }

  if (request.ident.table_name.empty() || request.ident.namespace_ident.parts.empty()) {
    return MakeStatus(CatalogStatusCode::kInvalidArgument, 0, false, false,
                      "table identifier is incomplete");
  }

  if (request.schema_json.empty()) {
    return MakeStatus(CatalogStatusCode::kInvalidArgument, 0, false, false,
                      "schema_json must not be empty");
  }

  json body;
  body["name"] = request.ident.table_name;
  body["schema"] = json::parse(request.schema_json);

  if (!request.partition_spec_json.empty()) {
    body["partition-spec"] = json::parse(request.partition_spec_json);
  }

  if (!request.write_order_json.empty()) {
    body["write-order"] = json::parse(request.write_order_json);
  }

  if (!request.location.empty()) {
    body["location"] = request.location;
  }

  if (!request.properties.empty()) {
    body["properties"] = request.properties;
  }

  if (request.stage_create) {
    body["stage-create"] = true;
  }

  auto headers = BuildJsonHeaders(config_, "", GenerateIdempotencyKey(), "", "");
  headers.emplace_back("Content-Type: application/json");
  const auto url =
      JoinPath(config_.base_uri,
               BuildNamespacePath(prefix_, request.ident.namespace_ident,
                                  namespace_separator_) +
                   "/tables");
  const auto response = ExecuteRequest(config_, "POST", url, body.dump(), headers);
  const auto parsed = ParseLoadTableResponse(response);

  if (result != nullptr) {
    *result = parsed;
  }

  return parsed.status;
}

CatalogStatus ParquetCatalogClient::LoadTable(const CatalogTableIdent &ident,
                                              CatalogLoadTableResult *result,
                                              const std::string &access_delegation)
{
  if (result == nullptr) {
    return MakeStatus(CatalogStatusCode::kInvalidArgument, 0, false, false,
                      "result must not be null");
  }

  const auto url = JoinPath(config_.base_uri,
                            BuildTablePath(prefix_, ident, namespace_separator_));
  auto headers =
      BuildJsonHeaders(config_, "", "", "", access_delegation);
  const auto response = ExecuteRequest(config_, "GET", url, "", headers);
  *result = ParseLoadTableResponse(response);
  return result->status;
}

CatalogStatus ParquetCatalogClient::CommitTable(
    const CatalogCommitRequest &request, CatalogLoadTableResult *result)
{
  if (!capabilities_.supports_commit_table) {
    return MakeStatus(CatalogStatusCode::kUnsupported, 0, false, false,
                      "catalog does not advertise commit table support");
  }

  if (request.request_json.empty()) {
    return MakeStatus(CatalogStatusCode::kInvalidArgument, 0, false, false,
                      "commit request_json must not be empty");
  }

  auto headers = BuildJsonHeaders(
      config_, "", request.idempotency_key.empty() ? GenerateIdempotencyKey()
                                                    : request.idempotency_key,
      request.if_match, "");
  headers.emplace_back("Content-Type: application/json");

  const auto url = JoinPath(config_.base_uri,
                            BuildTablePath(prefix_, request.ident,
                                           namespace_separator_));
  const auto response =
      ExecuteRequest(config_, "POST", url, request.request_json, headers);

  auto status = ParseCommitSuccessOrError(response);
  if (!status.ok()) {
    return status;
  }

  if (result != nullptr && !response.body.empty()) {
    *result = ParseLoadTableResponse(response);
    return result->status;
  }

  return status;
}

CatalogStatus ParquetCatalogClient::CommitTransactionIfSupported(
    const CatalogTransactionCommitRequest &request)
{
  if (!capabilities_.supports_commit_transaction) {
    return MakeStatus(CatalogStatusCode::kUnsupported, 0, false, false,
                      "catalog does not advertise transaction commit support");
  }

  if (request.request_json.empty()) {
    return MakeStatus(CatalogStatusCode::kInvalidArgument, 0, false, false,
                      "transaction request_json must not be empty");
  }

  auto headers = BuildJsonHeaders(
      config_, "", request.idempotency_key.empty() ? GenerateIdempotencyKey()
                                                    : request.idempotency_key,
      "", "");
  headers.emplace_back("Content-Type: application/json");

  const auto url = JoinPath(config_.base_uri,
                            BuildApiBasePath(prefix_) + "/transactions/commit");
  const auto response =
      ExecuteRequest(config_, "POST", url, request.request_json, headers);
  return ParseCommitSuccessOrError(response);
}

CatalogPlanScanResult ParquetCatalogClient::PlanTableScan(
    const CatalogPlanScanRequest &request)
{
  CatalogPlanScanResult result;

  if (!capabilities_.supports_scan_planning) {
    result.status = MakeStatus(CatalogStatusCode::kUnsupported, 0, false, false,
                               "catalog does not advertise scan planning");
    return result;
  }

  if (request.request_json.empty()) {
    result.status = MakeStatus(CatalogStatusCode::kInvalidArgument, 0, false,
                               false, "scan request_json must not be empty");
    return result;
  }

  auto headers = BuildJsonHeaders(config_, "", GenerateIdempotencyKey(), "",
                                  request.access_delegation);
  headers.emplace_back("Content-Type: application/json");

  const auto url = JoinPath(config_.base_uri,
                            BuildTablePath(prefix_, request.ident,
                                           namespace_separator_) +
                                "/plan");
  const auto response =
      ExecuteRequest(config_, "POST", url, request.request_json, headers);
  result.status = ParseBasicSuccessOrError(response);
  result.raw_response_json = response.body;

  if (!result.status.ok()) {
    return result;
  }

  try {
    const auto payload =
        json::parse(response.body.empty() ? "{}" : response.body);
    ExtractPlanFilesFromPayload(payload, &result.files,
                                &result.residual_predicate_json);
  } catch (const std::exception &ex) {
    result.status = MakeStatus(CatalogStatusCode::kInvalidResponse,
                               response.status_code, false, false, ex.what());
  }

  return result;
}

CatalogStatus ParquetCatalogClient::TableExists(const CatalogTableIdent &ident,
                                                bool *exists)
{
  if (exists == nullptr) {
    return MakeStatus(CatalogStatusCode::kInvalidArgument, 0, false, false,
                      "exists must not be null");
  }

  const auto url = JoinPath(config_.base_uri,
                            BuildTablePath(prefix_, ident, namespace_separator_));
  auto headers = BuildJsonHeaders(config_, "", "", "", "");
  const auto response = ExecuteRequest(config_, "HEAD", url, "", headers);
  auto status = ParseBasicSuccessOrError(response);

  if (status.ok()) {
    *exists = true;
    return status;
  }

  if (status.code == CatalogStatusCode::kNotFound) {
    *exists = false;
    return MakeStatus(CatalogStatusCode::kOk, response.status_code, false, false,
                      "");
  }

  return status;
}

} // namespace parquet
