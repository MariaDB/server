#define MYSQL_SERVER 1

#include "my_global.h"

#include "parquet_transaction.h"

#include "handler.h"
#include "mysql/plugin.h"

#include <new>

namespace parquet
{

namespace
{

transaction_participant *TxnParticipant(handlerton *hton)
{
  return static_cast<transaction_participant *>(hton);
}

const transaction_participant *TxnParticipant(const handlerton *hton)
{
  return static_cast<const transaction_participant *>(hton);
}

} // namespace

ParquetTxnState *GetOrCreateTxnState(THD *thd, handlerton *hton)
{
  auto *txn_state = GetTxnState(thd, hton);

  if (txn_state != nullptr) {
    return txn_state;
  }

  txn_state = new (std::nothrow) ParquetTxnState();
  if (txn_state == nullptr) {
    return nullptr;
  }

  thd_set_ha_data(thd, TxnParticipant(hton), txn_state);
  return txn_state;
}

ParquetTxnState *GetTxnState(THD *thd, handlerton *hton)
{
  if (thd == nullptr || hton == nullptr) {
    return nullptr;
  }

  return static_cast<ParquetTxnState *>(thd_get_ha_data(
      thd, TxnParticipant(hton)));
}

void ClearTxnState(THD *thd, handlerton *hton)
{
  auto *txn_state = GetTxnState(thd, hton);

  if (txn_state == nullptr) {
    return;
  }

  delete txn_state;
  thd_set_ha_data(thd, TxnParticipant(hton), nullptr);
}

bool IsStagedFileComplete(const ParquetStagedFile &staged_file)
{
  return !staged_file.table_path.empty() &&
         !staged_file.table_name.empty() &&
         !staged_file.local_parquet_path.empty() &&
         !staged_file.target_object_path.empty() &&
         staged_file.record_count > 0 &&
         staged_file.file_size_bytes > 0 &&
         staged_file.flush_id > 0;
}

bool ValidateTxnState(const ParquetTxnState &txn_state, std::string *error)
{
  if (txn_state.has_error) {
    if (error != nullptr) {
      *error = "transaction state is marked as failed";
    }
    return false;
  }

  if (!txn_state.staged_files.empty() && !txn_state.registered_with_server) {
    if (error != nullptr) {
      *error = "transaction state has staged files but was never registered";
    }
    return false;
  }

  for (const auto &staged_file : txn_state.staged_files) {
    if (!IsStagedFileComplete(staged_file)) {
      if (error != nullptr) {
        *error = "transaction state contains an incomplete staged file entry";
      }
      return false;
    }
  }

  return true;
}

std::string BuildLocalStagePath(const std::string &canonical_parquet_path,
                                uint64_t flush_id)
{
  const std::string suffix = ".parquet";

  if (canonical_parquet_path.size() >= suffix.size() &&
      canonical_parquet_path.compare(canonical_parquet_path.size() -
                                         suffix.size(),
                                     suffix.size(), suffix) == 0) {
    return canonical_parquet_path.substr(
               0, canonical_parquet_path.size() - suffix.size()) +
           ".stage_" + std::to_string(flush_id) + suffix;
  }

  return canonical_parquet_path + ".stage_" + std::to_string(flush_id) +
         suffix;
}

std::string BuildPrototypeObjectPath(const std::string &table_name,
                                     uint64_t flush_id)
{
  return "s3://parquet-stage/" + table_name + "/flush_" +
         std::to_string(flush_id) + ".parquet";
}

} // namespace parquet
