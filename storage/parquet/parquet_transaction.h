#ifndef PARQUET_TRANSACTION_INCLUDED
#define PARQUET_TRANSACTION_INCLUDED

#include <cstdint>
#include <string>
#include <vector>

class THD;
struct handlerton;

namespace parquet
{

struct ParquetStagedFile {
  std::string table_path;
  std::string table_name;
  std::string local_parquet_path;
  std::string target_object_path;
  uint64_t record_count = 0;
  uint64_t file_size_bytes = 0;
  uint64_t flush_id = 0;
};

struct ParquetTxnState {
  std::vector<ParquetStagedFile> staged_files;
  bool registered_with_server = false;
  bool has_error = false;
};

ParquetTxnState *GetOrCreateTxnState(THD *thd, handlerton *hton);
ParquetTxnState *GetTxnState(THD *thd, handlerton *hton);
void ClearTxnState(THD *thd, handlerton *hton);

bool IsStagedFileComplete(const ParquetStagedFile &staged_file);
bool ValidateTxnState(const ParquetTxnState &txn_state, std::string *error);

std::string BuildLocalStagePath(const std::string &canonical_parquet_path,
                                uint64_t flush_id);
std::string BuildPrototypeObjectPath(const std::string &table_name,
                                     uint64_t flush_id);

} // namespace parquet

#endif
