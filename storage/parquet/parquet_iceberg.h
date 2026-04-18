#ifndef PARQUET_ICEBERG_INCLUDED
#define PARQUET_ICEBERG_INCLUDED

#include "parquet_catalog.h"
#include "parquet_metadata.h"
#include "parquet_object_store.h"
#include "parquet_transaction.h"

#include <cstdint>
#include <string>
#include <vector>

namespace parquet
{

struct IcebergCommitArtifacts {
  uint64_t snapshot_id = 0;
  uint64_t sequence_number = 0;
  std::string manifest_local_path;
  ObjectLocation manifest_location;
  std::string manifest_list_local_path;
  ObjectLocation manifest_list_location;
  std::string commit_request_json;
  std::vector<ActiveDataFile> active_files;
};

std::vector<std::string> ExtractActiveScanPaths(
    const std::vector<ActiveDataFile> &active_files);

bool BuildIcebergCommitArtifacts(
    const TableMetadata &table_metadata,
    const CatalogLoadTableResult &load_result,
    const std::vector<ParquetStagedFile> &staged_files,
    IcebergCommitArtifacts *artifacts,
    std::string *error);

} // namespace parquet

#endif
