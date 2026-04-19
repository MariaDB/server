#define MYSQL_SERVER 1

#include "my_global.h"

#include "parquet_object_store.h"
#include "parquet_shared.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace parquet
{

namespace
{

struct HttpResponse {
  CURLcode curl_code = CURLE_OK;
  long status_code = 0;
  std::string body;
  std::map<std::string, std::string> headers;
  std::string curl_error;
};

std::once_flag curl_init_once;

void EnsureCurlInitialized()
{
  std::call_once(curl_init_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
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

std::string EncodeKeyPath(const std::string &key)
{
  std::stringstream stream(key);
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

  return encoded;
}

std::string NormalizeUrlBase(std::string value)
{
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }

  return value;
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

size_t WriteBodyCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  auto *body = static_cast<std::string *>(userdata);
  const auto bytes = size * nmemb;
  body->append(ptr, bytes);
  return bytes;
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

size_t FileReadCallback(char *buffer, size_t size, size_t nitems, void *userdata)
{
  auto *file = static_cast<std::FILE *>(userdata);
  return std::fread(buffer, 1, size * nitems, file);
}

uint64_t ReadFileSize(const std::string &path)
{
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return 0;
  }

  return static_cast<uint64_t>(stream.tellg());
}

ObjectStoreStatus MakeStatus(ObjectStoreStatusCode code, long http_status,
                             bool retryable, std::string message)
{
  ObjectStoreStatus status;
  status.code = code;
  status.http_status = http_status;
  status.retryable = retryable;
  status.message = std::move(message);
  return status;
}

ObjectStoreStatus HttpStatusToObjectStatus(long http_status,
                                           const std::string &message)
{
  if (http_status >= 200 && http_status < 300) {
    return MakeStatus(ObjectStoreStatusCode::kOk, http_status, false, message);
  }

  if (http_status == 400) {
    return MakeStatus(ObjectStoreStatusCode::kInvalidArgument, http_status,
                      false, message);
  }

  if (http_status == 401) {
    return MakeStatus(ObjectStoreStatusCode::kUnauthorized, http_status, false,
                      message);
  }

  if (http_status == 403) {
    return MakeStatus(ObjectStoreStatusCode::kForbidden, http_status, false,
                      message);
  }

  if (http_status == 404) {
    return MakeStatus(ObjectStoreStatusCode::kNotFound, http_status, false,
                      message);
  }

  if (http_status == 409) {
    return MakeStatus(ObjectStoreStatusCode::kConflict, http_status, true,
                      message);
  }

  return MakeStatus(ObjectStoreStatusCode::kServerError, http_status,
                    http_status >= 500, message);
}

ObjectStoreStatus ApplyAuth(CURL *curl, struct curl_slist **headers,
                            const ObjectStoreConfig &config,
                            std::string *userpwd_storage)
{
  if (config.auth_mode == ObjectStoreAuthMode::kRemoteSigning) {
    return MakeStatus(ObjectStoreStatusCode::kUnsupported, 0, false,
                      "remote signing is not implemented in this slice");
  }

  if (config.credentials.empty()) {
    return MakeStatus(ObjectStoreStatusCode::kInvalidConfig, 0, false,
                      "object store credentials are missing");
  }

  const auto sigv4 = "aws:amz:" + config.region + ":s3";
  *userpwd_storage = config.credentials.access_key_id + ":" +
                     config.credentials.secret_access_key;

  curl_easy_setopt(curl, CURLOPT_AWS_SIGV4, sigv4.c_str());
  curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd_storage->c_str());

  if (!config.credentials.session_token.empty()) {
    *headers = curl_slist_append(
        *headers,
        ("x-amz-security-token: " + config.credentials.session_token).c_str());
  }

  return MakeStatus(ObjectStoreStatusCode::kOk, 0, false, "");
}

HttpResponse ExecuteRequest(const ObjectStoreConfig &config,
                            const std::string &method,
                            const ObjectLocation &location,
                            std::FILE *upload_file,
                            uint64_t upload_size,
                            const std::string &content_type)
{
  EnsureCurlInitialized();
  parquet_log_info("S3 request method='" + method + "' uri='" +
                   BuildS3Uri(location.bucket, location.key) + "' url='" +
                   location.url + "' bytes=" + std::to_string(upload_size));

  HttpResponse response;
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    response.curl_code = CURLE_FAILED_INIT;
    response.curl_error = "curl_easy_init failed";
    return response;
  }

  char error_buffer[CURL_ERROR_SIZE] = {0};
  struct curl_slist *headers = nullptr;
  std::string userpwd_storage;

  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  curl_easy_setopt(curl, CURLOPT_URL, location.url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, config.connect_timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config.timeout_ms);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, config.verify_peer ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, config.verify_host ? 2L : 0L);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBodyCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

  auto auth_status = ApplyAuth(curl, &headers, config, &userpwd_storage);
  if (!auth_status.ok()) {
    curl_easy_cleanup(curl);
    response.curl_code = CURLE_LOGIN_DENIED;
    response.curl_error = auth_status.message;
    return response;
  }

  if (method == "PUT") {
    headers = curl_slist_append(headers,
                                ("Content-Type: " + content_type).c_str());
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, FileReadCallback);
    curl_easy_setopt(curl, CURLOPT_READDATA, upload_file);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                     static_cast<curl_off_t>(upload_size));
  } else if (method == "HEAD") {
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  } else if (method == "DELETE") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  }

  if (headers != nullptr) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }

  response.curl_code = curl_easy_perform(curl);
  if (response.curl_code != CURLE_OK) {
    response.curl_error = error_buffer[0] ? error_buffer
                                          : curl_easy_strerror(response.curl_code);
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);

  if (response.curl_code == CURLE_OK) {
    parquet_log_info("S3 response method='" + method + "' uri='" +
                     BuildS3Uri(location.bucket, location.key) +
                     "' http_status=" + std::to_string(response.status_code) +
                     " body=" + parquet_log_preview(response.body));
  } else {
    parquet_log_warning("S3 request failed method='" + method + "' uri='" +
                        BuildS3Uri(location.bucket, location.key) +
                        "' error='" + response.curl_error + "'");
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

std::string BuildVirtualHostUrl(const std::string &endpoint,
                                const std::string &bucket,
                                const std::string &encoded_key)
{
  const auto scheme_pos = endpoint.find("://");
  if (scheme_pos == std::string::npos) {
    return "";
  }

  const auto authority_begin = scheme_pos + 3;
  const auto path_pos = endpoint.find('/', authority_begin);
  const auto scheme = endpoint.substr(0, authority_begin);
  const auto authority = path_pos == std::string::npos
                             ? endpoint.substr(authority_begin)
                             : endpoint.substr(authority_begin,
                                               path_pos - authority_begin);
  const auto path_prefix =
      path_pos == std::string::npos ? "" : endpoint.substr(path_pos);

  std::string url = scheme + bucket + "." + authority + path_prefix;
  if (!encoded_key.empty()) {
    if (url.empty() || url.back() != '/') {
      url += "/";
    }
    url += encoded_key;
  }

  return url;
}

} // namespace

std::string NormalizeObjectKeyPrefix(const std::string &key_prefix)
{
  size_t begin = 0;
  while (begin < key_prefix.size() && key_prefix[begin] == '/') {
    begin++;
  }

  size_t end = key_prefix.size();
  while (end > begin && key_prefix[end - 1] == '/') {
    end--;
  }

  return key_prefix.substr(begin, end - begin);
}

std::string BuildS3Uri(const std::string &bucket, const std::string &key)
{
  if (bucket.empty()) {
    return "";
  }

  if (key.empty()) {
    return "s3://" + bucket;
  }

  return "s3://" + bucket + "/" + key;
}

ObjectLocation ResolveAbsoluteObjectLocation(const ObjectStoreConfig &config,
                                             const std::string &bucket,
                                             const std::string &key)
{
  ObjectLocation location;
  location.bucket = bucket;
  location.key = NormalizeObjectKeyPrefix(key);

  const auto endpoint = NormalizeUrlBase(config.endpoint);
  const auto encoded_key = EncodeKeyPath(location.key);
  const auto url_style = ToLowerAscii(config.url_style);

  if (url_style == "virtual-host" || url_style == "virtual_host") {
    location.url = BuildVirtualHostUrl(endpoint, location.bucket, encoded_key);
  } else {
    location.url = endpoint + "/" + PercentEncode(location.bucket);
    if (!encoded_key.empty()) {
      location.url += "/" + encoded_key;
    }
  }

  return location;
}

ObjectLocation ResolveObjectLocation(const ObjectStoreConfig &config,
                                     const std::string &relative_key)
{
  const auto normalized_prefix = NormalizeObjectKeyPrefix(config.key_prefix);
  const auto trimmed_key = NormalizeObjectKeyPrefix(relative_key);

  std::string full_key;
  if (!normalized_prefix.empty() && !trimmed_key.empty()) {
    full_key = normalized_prefix + "/" + trimmed_key;
  } else if (!normalized_prefix.empty()) {
    full_key = normalized_prefix;
  } else {
    full_key = trimmed_key;
  }

  return ResolveAbsoluteObjectLocation(config, config.bucket, full_key);
}

bool ParseS3Uri(const std::string &uri, ObjectLocation *location)
{
  static const std::string prefix = "s3://";
  if (location == nullptr || uri.rfind(prefix, 0) != 0) {
    return false;
  }

  const auto remainder = uri.substr(prefix.size());
  const auto slash = remainder.find('/');
  if (slash == std::string::npos) {
    location->bucket = remainder;
    location->key.clear();
  } else {
    location->bucket = remainder.substr(0, slash);
    location->key = remainder.substr(slash + 1);
  }

  location->url.clear();
  return !location->bucket.empty();
}

ParquetObjectStoreClient::ParquetObjectStoreClient(ObjectStoreConfig config)
    : config_(std::move(config))
{
  config_.endpoint = NormalizeUrlBase(config_.endpoint);
  config_.key_prefix = NormalizeObjectKeyPrefix(config_.key_prefix);
}

ObjectStoreStatus ParquetObjectStoreClient::PutFile(
    const PutObjectRequest &request)
{
  if (request.local_file_path.empty()) {
    return MakeStatus(ObjectStoreStatusCode::kInvalidArgument, 0, false,
                      "local_file_path must not be empty");
  }

  if (request.location.url.empty()) {
    return MakeStatus(ObjectStoreStatusCode::kInvalidArgument, 0, false,
                      "object location URL must not be empty");
  }

  const auto file_size = request.expected_content_length != 0
                             ? request.expected_content_length
                             : ReadFileSize(request.local_file_path);
  std::FILE *file = std::fopen(request.local_file_path.c_str(), "rb");
  if (file == nullptr) {
    return MakeStatus(ObjectStoreStatusCode::kInvalidArgument, 0, false,
                      "failed to open local Parquet file for upload");
  }

  const auto response = ExecuteRequest(config_, "PUT", request.location, file,
                                       file_size, request.content_type);
  std::fclose(file);

  if (response.curl_code != CURLE_OK) {
    return MakeStatus(ObjectStoreStatusCode::kTransportError, 0, true,
                      response.curl_error);
  }

  parquet_log_info("S3 put object complete uri='" +
                   BuildS3Uri(request.location.bucket, request.location.key) +
                   "' local_file='" + request.local_file_path + "' bytes=" +
                   std::to_string(file_size) + " http_status=" +
                   std::to_string(response.status_code));

  return HttpStatusToObjectStatus(response.status_code, response.body);
}

HeadObjectResult ParquetObjectStoreClient::HeadObject(
    const ObjectLocation &location)
{
  HeadObjectResult result;

  if (location.url.empty()) {
    result.status = MakeStatus(ObjectStoreStatusCode::kInvalidArgument, 0, false,
                               "object location URL must not be empty");
    return result;
  }

  const auto response =
      ExecuteRequest(config_, "HEAD", location, nullptr, 0, "");
  if (response.curl_code != CURLE_OK) {
    result.status = MakeStatus(ObjectStoreStatusCode::kTransportError, 0, true,
                               response.curl_error);
    return result;
  }

  result.status = HttpStatusToObjectStatus(response.status_code, response.body);
  if (result.status.code == ObjectStoreStatusCode::kNotFound) {
    result.exists = false;
    return result;
  }

  if (!result.status.ok()) {
    return result;
  }

  result.exists = true;

  auto content_length_it = response.headers.find("content-length");
  if (content_length_it != response.headers.end()) {
    result.content_length = static_cast<uint64_t>(
        std::strtoull(content_length_it->second.c_str(), nullptr, 10));
  }

  auto etag_it = response.headers.find("etag");
  if (etag_it != response.headers.end()) {
    result.etag = etag_it->second;
  }

  return result;
}

ObjectStoreStatus ParquetObjectStoreClient::DeleteObject(
    const ObjectLocation &location)
{
  if (location.url.empty()) {
    return MakeStatus(ObjectStoreStatusCode::kInvalidArgument, 0, false,
                      "object location URL must not be empty");
  }

  const auto response =
      ExecuteRequest(config_, "DELETE", location, nullptr, 0, "");
  if (response.curl_code != CURLE_OK) {
    return MakeStatus(ObjectStoreStatusCode::kTransportError, 0, true,
                      response.curl_error);
  }

  auto status = HttpStatusToObjectStatus(response.status_code, response.body);
  if (status.code == ObjectStoreStatusCode::kNotFound) {
    return MakeStatus(ObjectStoreStatusCode::kOk, response.status_code, false,
                      "");
  }

  parquet_log_info("S3 delete object complete uri='" +
                   BuildS3Uri(location.bucket, location.key) +
                   "' http_status=" + std::to_string(response.status_code));

  return status;
}

std::vector<DeleteObjectResult> ParquetObjectStoreClient::DeleteObjectsBestEffort(
    const std::vector<ObjectLocation> &locations)
{
  std::vector<DeleteObjectResult> results;
  results.reserve(locations.size());

  for (const auto &location : locations) {
    results.push_back({location, DeleteObject(location)});
  }

  return results;
}

} // namespace parquet
