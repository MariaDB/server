#ifndef PARQUET_CATALOG_INCLUDED
#define PARQUET_CATALOG_INCLUDED

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace parquet
{

enum class CatalogStatusCode {
  kOk,
  kUnsupported,
  kInvalidArgument,
  kInvalidResponse,
  kNotFound,
  kConflict,
  kUnauthorized,
  kForbidden,
  kTransportError,
  kServerError,
  kCommitStateUnknown
};

struct CatalogStatus {
  CatalogStatusCode code = CatalogStatusCode::kOk;
  long http_status = 0;
  bool retryable = false;
  bool commit_state_unknown = false;
  std::string message;

  bool ok() const { return code == CatalogStatusCode::kOk; }
};

struct CatalogClientConfig {
  std::string base_uri;
  std::string warehouse;
  std::string prefix;
  std::string bearer_token;
  std::map<std::string, std::string> client_properties;
  long connect_timeout_ms = 10000;
  long timeout_ms = 30000;
  bool verify_peer = true;
  bool verify_host = true;
};

struct CatalogCapabilitySet {
  bool supports_create_table = true;
  bool supports_commit_table = true;
  bool supports_commit_transaction = true;
  bool supports_scan_planning = false;
  bool supports_register_table = true;
  std::vector<std::string> advertised_endpoints;
};

struct CatalogNamespaceIdent {
  std::vector<std::string> parts;
};

struct CatalogTableIdent {
  CatalogNamespaceIdent namespace_ident;
  std::string table_name;
};

struct CatalogTableConfig {
  std::map<std::string, std::string> properties;
  std::string bearer_token;
};

struct CatalogTableMetadata {
  std::string table_uuid;
  std::string table_location;
  int format_version = 0;
  std::string current_snapshot_id;
  std::string schema_json;
  std::string partition_spec_json;
  std::string sort_order_json;
  std::string raw_metadata_json;
};

struct CatalogLoadTableResult {
  CatalogStatus status;
  CatalogTableConfig config;
  CatalogTableMetadata metadata;
  std::string etag;
};

struct CatalogCreateTableRequest {
  CatalogTableIdent ident;
  std::string schema_json;
  std::string partition_spec_json;
  std::string write_order_json;
  std::string location;
  std::map<std::string, std::string> properties;
  bool stage_create = false;
};

struct CatalogCommitRequest {
  CatalogTableIdent ident;
  std::string request_json;
  std::string idempotency_key;
  std::string if_match;
};

struct CatalogTransactionCommitRequest {
  std::string request_json;
  std::string idempotency_key;
};

struct CatalogPlannedFile {
  std::string file_path;
  uint64_t file_size_bytes = 0;
  std::string partition_json;
  std::string residual_predicate_json;
  std::map<std::string, std::string> extra_fields;
};

struct CatalogPlanScanRequest {
  CatalogTableIdent ident;
  std::string request_json;
  std::string access_delegation;
};

struct CatalogPlanScanResult {
  CatalogStatus status;
  std::vector<CatalogPlannedFile> files;
  std::string residual_predicate_json;
  std::string raw_response_json;
};

std::string NormalizeCatalogBaseUri(const std::string &base_uri);
CatalogCapabilitySet ResolveCatalogCapabilities(
    const std::vector<std::string> &advertised_endpoints);
std::string EncodeNamespaceForUrlPath(const CatalogNamespaceIdent &ident,
                                      const std::string &encoded_separator);

class ParquetCatalogClient
{
public:
  explicit ParquetCatalogClient(CatalogClientConfig config);

  CatalogStatus BootstrapConfig();
  CatalogStatus EnsureNamespace(
      const CatalogNamespaceIdent &ident,
      const std::map<std::string, std::string> &properties = {});
  CatalogStatus CreateTable(const CatalogCreateTableRequest &request,
                            CatalogLoadTableResult *result);
  CatalogStatus LoadTable(const CatalogTableIdent &ident,
                          CatalogLoadTableResult *result,
                          const std::string &access_delegation = "");
  CatalogStatus CommitTable(const CatalogCommitRequest &request,
                            CatalogLoadTableResult *result);
  CatalogStatus CommitTransactionIfSupported(
      const CatalogTransactionCommitRequest &request);
  CatalogPlanScanResult PlanTableScan(const CatalogPlanScanRequest &request);
  CatalogStatus TableExists(const CatalogTableIdent &ident, bool *exists);

  const CatalogCapabilitySet &capabilities() const { return capabilities_; }
  const std::map<std::string, std::string> &effective_config() const
  {
    return effective_config_;
  }
  const std::string &namespace_separator() const
  {
    return namespace_separator_;
  }
  const std::string &prefix() const { return prefix_; }

private:
  CatalogClientConfig config_;
  CatalogCapabilitySet capabilities_;
  std::map<std::string, std::string> effective_config_;
  std::string namespace_separator_ = "%1F";
  std::string prefix_;
  bool bootstrapped_ = false;
};

} // namespace parquet

#endif
