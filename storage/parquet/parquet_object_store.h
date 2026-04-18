#ifndef PARQUET_OBJECT_STORE_INCLUDED
#define PARQUET_OBJECT_STORE_INCLUDED

#include <cstdint>
#include <string>
#include <vector>

namespace parquet
{

enum class ObjectStoreStatusCode {
  kOk,
  kInvalidArgument,
  kInvalidConfig,
  kUnsupported,
  kNotFound,
  kUnauthorized,
  kForbidden,
  kConflict,
  kTransportError,
  kServerError
};

struct ObjectStoreStatus {
  ObjectStoreStatusCode code = ObjectStoreStatusCode::kOk;
  long http_status = 0;
  bool retryable = false;
  std::string message;

  bool ok() const { return code == ObjectStoreStatusCode::kOk; }
};

enum class ObjectStoreAuthMode {
  kClientManagedStatic,
  kTemporaryCredentials,
  kRemoteSigning
};

struct ObjectStoreCredentials {
  std::string access_key_id;
  std::string secret_access_key;
  std::string session_token;
  std::string expiration;

  bool empty() const
  {
    return access_key_id.empty() || secret_access_key.empty();
  }
};

struct ObjectStoreConfig {
  std::string endpoint;
  std::string region;
  std::string bucket;
  std::string key_prefix;
  std::string url_style = "path";
  bool verify_peer = true;
  bool verify_host = true;
  long connect_timeout_ms = 10000;
  long timeout_ms = 30000;
  ObjectStoreAuthMode auth_mode = ObjectStoreAuthMode::kClientManagedStatic;
  ObjectStoreCredentials credentials;
};

struct ObjectLocation {
  std::string bucket;
  std::string key;
  std::string url;
};

struct PutObjectRequest {
  std::string local_file_path;
  ObjectLocation location;
  std::string content_type = "application/octet-stream";
  uint64_t expected_content_length = 0;
};

struct HeadObjectResult {
  ObjectStoreStatus status;
  bool exists = false;
  uint64_t content_length = 0;
  std::string etag;
};

struct DeleteObjectResult {
  ObjectLocation location;
  ObjectStoreStatus status;
};

std::string NormalizeObjectKeyPrefix(const std::string &key_prefix);
std::string BuildS3Uri(const std::string &bucket, const std::string &key);
ObjectLocation ResolveObjectLocation(const ObjectStoreConfig &config,
                                     const std::string &relative_key);
ObjectLocation ResolveAbsoluteObjectLocation(const ObjectStoreConfig &config,
                                             const std::string &bucket,
                                             const std::string &key);
bool ParseS3Uri(const std::string &uri, ObjectLocation *location);

class ParquetObjectStoreClient
{
public:
  explicit ParquetObjectStoreClient(ObjectStoreConfig config);

  ObjectStoreStatus PutFile(const PutObjectRequest &request);
  HeadObjectResult HeadObject(const ObjectLocation &location);
  ObjectStoreStatus DeleteObject(const ObjectLocation &location);
  std::vector<DeleteObjectResult> DeleteObjectsBestEffort(
      const std::vector<ObjectLocation> &locations);

private:
  ObjectStoreConfig config_;
};

} // namespace parquet

#endif
