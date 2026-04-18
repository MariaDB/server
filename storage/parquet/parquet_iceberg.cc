#include "parquet_iceberg.h"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace parquet
{

namespace
{

using json = nlohmann::json;

struct ParsedTableState {
  std::string table_uuid;
  std::string current_snapshot_id;
  int format_version = 2;
  uint64_t last_sequence_number = 0;
  int current_schema_id = 0;
  int default_spec_id = 0;
  std::string current_schema_json;
  std::string current_partition_fields_json = "[]";
};

struct ManifestDataFile {
  int status = 0;
  bool has_snapshot_id = false;
  uint64_t snapshot_id = 0;
  bool has_sequence_number = false;
  uint64_t sequence_number = 0;
  bool has_file_sequence_number = false;
  uint64_t file_sequence_number = 0;
  std::string file_path;
  uint64_t record_count = 0;
  uint64_t file_size_bytes = 0;
};

struct ManifestListEntry {
  std::string manifest_path;
  uint64_t manifest_length = 0;
  int partition_spec_id = 0;
  int content = 0;
  uint64_t sequence_number = 0;
  uint64_t min_sequence_number = 0;
  bool has_added_snapshot_id = false;
  uint64_t added_snapshot_id = 0;
  int added_files_count = 0;
  int existing_files_count = 0;
  int deleted_files_count = 0;
  uint64_t added_rows_count = 0;
  uint64_t existing_rows_count = 0;
  uint64_t deleted_rows_count = 0;
};

uint64_t NextUniqueId()
{
  static std::mt19937_64 generator(std::random_device{}());
  static std::uniform_int_distribution<uint64_t> distribution(
      1ULL << 40, std::numeric_limits<uint64_t>::max() >> 1);
  return distribution(generator);
}

std::string NextUniqueToken()
{
  std::stringstream stream;
  stream << std::hex << NextUniqueId();
  return stream.str();
}

void WriteLong(std::string *out, int64_t value)
{
  uint64_t encoded =
      (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);

  while ((encoded & ~0x7FULL) != 0) {
    out->push_back(static_cast<char>((encoded & 0x7F) | 0x80));
    encoded >>= 7;
  }

  out->push_back(static_cast<char>(encoded));
}

void WriteInt(std::string *out, int32_t value)
{
  WriteLong(out, value);
}

void WriteString(std::string *out, const std::string &value)
{
  WriteLong(out, static_cast<int64_t>(value.size()));
  out->append(value);
}

void WriteBytes(std::string *out, const std::string &value)
{
  WriteLong(out, static_cast<int64_t>(value.size()));
  out->append(value);
}

void WriteNullUnion(std::string *out)
{
  WriteLong(out, 0);
}

void WriteLongUnion(std::string *out, uint64_t value)
{
  WriteLong(out, 1);
  WriteLong(out, static_cast<int64_t>(value));
}

bool WriteAvroObjectContainerFile(
    const std::string &path,
    const std::string &schema_json,
    const std::map<std::string, std::string> &metadata,
    const std::string &records,
    size_t object_count,
    std::string *error)
{
  std::string payload;
  payload.append("Obj", 3);
  payload.push_back('\x01');

  std::map<std::string, std::string> header_metadata = metadata;
  header_metadata["avro.codec"] = "null";
  header_metadata["avro.schema"] = schema_json;

  WriteLong(&payload, static_cast<int64_t>(header_metadata.size()));
  for (const auto &entry : header_metadata) {
    WriteString(&payload, entry.first);
    WriteBytes(&payload, entry.second);
  }
  WriteLong(&payload, 0);

  std::string sync_marker;
  sync_marker.reserve(16);
  for (int index = 0; index < 16; ++index) {
    sync_marker.push_back(
        static_cast<char>(NextUniqueId() & 0xFF));
  }
  payload.append(sync_marker);

  WriteLong(&payload, static_cast<int64_t>(object_count));
  WriteLong(&payload, static_cast<int64_t>(records.size()));
  payload.append(records);
  payload.append(sync_marker);

  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    if (error != nullptr) {
      *error = "failed to open Avro file for writing: " + path;
    }
    return false;
  }

  stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (!stream.good()) {
    if (error != nullptr) {
      *error = "failed to write Avro file: " + path;
    }
    return false;
  }

  return true;
}

std::string BuildManifestSchemaJson()
{
  return json({
      {"type", "record"},
      {"name", "manifest_entry"},
      {"fields",
       json::array({
           {{"name", "status"}, {"type", "int"}, {"field-id", 0}},
           {{"name", "snapshot_id"},
            {"type", json::array({"null", "long"})},
            {"default", nullptr},
            {"field-id", 1}},
           {{"name", "sequence_number"},
            {"type", json::array({"null", "long"})},
            {"default", nullptr},
            {"field-id", 3}},
           {{"name", "file_sequence_number"},
            {"type", json::array({"null", "long"})},
            {"default", nullptr},
            {"field-id", 4}},
           {{"name", "data_file"},
            {"type",
             {
                 {"type", "record"},
                 {"name", "data_file"},
                 {"fields",
                  json::array({
                      {{"name", "content"}, {"type", "int"}, {"field-id", 134}},
                      {{"name", "file_path"},
                       {"type", "string"},
                       {"field-id", 100}},
                      {{"name", "file_format"},
                       {"type", "string"},
                       {"field-id", 101}},
                      {{"name", "partition"},
                       {"type",
                        {{"type", "record"},
                         {"name", "partition_data"},
                         {"fields", json::array()}}},
                       {"field-id", 102}},
                      {{"name", "record_count"},
                       {"type", "long"},
                       {"field-id", 103}},
                      {{"name", "file_size_in_bytes"},
                       {"type", "long"},
                       {"field-id", 104}},
                  })},
             }},
            {"field-id", 2}},
       })},
  }).dump();
}

std::string BuildManifestListSchemaJson()
{
  return json({
      {"type", "record"},
      {"name", "manifest_file"},
      {"fields",
       json::array({
           {{"name", "manifest_path"}, {"type", "string"}, {"field-id", 500}},
           {{"name", "manifest_length"}, {"type", "long"}, {"field-id", 501}},
           {{"name", "partition_spec_id"}, {"type", "int"}, {"field-id", 502}},
           {{"name", "added_snapshot_id"},
            {"type", json::array({"null", "long"})},
            {"default", nullptr},
            {"field-id", 503}},
           {{"name", "added_files_count"}, {"type", "int"}, {"field-id", 504}},
           {{"name", "existing_files_count"},
            {"type", "int"},
            {"field-id", 505}},
           {{"name", "deleted_files_count"},
            {"type", "int"},
            {"field-id", 506}},
           {{"name", "added_rows_count"}, {"type", "long"}, {"field-id", 512}},
           {{"name", "existing_rows_count"},
            {"type", "long"},
            {"field-id", 513}},
           {{"name", "deleted_rows_count"},
            {"type", "long"},
            {"field-id", 514}},
           {{"name", "sequence_number"}, {"type", "long"}, {"field-id", 515}},
           {{"name", "min_sequence_number"},
            {"type", "long"},
            {"field-id", 516}},
           {{"name", "content"}, {"type", "int"}, {"field-id", 517}},
       })},
  }).dump();
}

void EncodeManifestEntry(std::string *out, const ManifestDataFile &entry)
{
  WriteInt(out, entry.status);
  if (entry.has_snapshot_id) {
    WriteLongUnion(out, entry.snapshot_id);
  } else {
    WriteNullUnion(out);
  }
  if (entry.has_sequence_number) {
    WriteLongUnion(out, entry.sequence_number);
  } else {
    WriteNullUnion(out);
  }
  if (entry.has_file_sequence_number) {
    WriteLongUnion(out, entry.file_sequence_number);
  } else {
    WriteNullUnion(out);
  }

  WriteInt(out, 0);
  WriteString(out, entry.file_path);
  WriteString(out, "parquet");
  WriteLong(out, 0);
  WriteLong(out, static_cast<int64_t>(entry.record_count));
  WriteLong(out, static_cast<int64_t>(entry.file_size_bytes));
}

void EncodeManifestListEntry(std::string *out, const ManifestListEntry &entry)
{
  WriteString(out, entry.manifest_path);
  WriteLong(out, static_cast<int64_t>(entry.manifest_length));
  WriteInt(out, entry.partition_spec_id);
  if (entry.has_added_snapshot_id) {
    WriteLongUnion(out, entry.added_snapshot_id);
  } else {
    WriteNullUnion(out);
  }
  WriteInt(out, entry.added_files_count);
  WriteInt(out, entry.existing_files_count);
  WriteInt(out, entry.deleted_files_count);
  WriteLong(out, static_cast<int64_t>(entry.added_rows_count));
  WriteLong(out, static_cast<int64_t>(entry.existing_rows_count));
  WriteLong(out, static_cast<int64_t>(entry.deleted_rows_count));
  WriteLong(out, static_cast<int64_t>(entry.sequence_number));
  WriteLong(out, static_cast<int64_t>(entry.min_sequence_number));
  WriteInt(out, entry.content);
}

bool ParseUnsigned(const std::string &value, uint64_t *out)
{
  if (out == nullptr || value.empty()) {
    return false;
  }

  try {
    *out = std::stoull(value);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

bool ParseTableState(const CatalogLoadTableResult &load_result,
                     ParsedTableState *state, std::string *error)
{
  if (state == nullptr) {
    if (error != nullptr) {
      *error = "table state output must not be null";
    }
    return false;
  }

  if (load_result.metadata.raw_metadata_json.empty()) {
    if (error != nullptr) {
      *error = "catalog load result did not include raw table metadata";
    }
    return false;
  }

  try {
    const auto payload = json::parse(load_result.metadata.raw_metadata_json);
    ParsedTableState parsed;
    parsed.table_uuid = load_result.metadata.table_uuid;
    if (parsed.table_uuid.empty() && payload.contains("table-uuid")) {
      parsed.table_uuid = payload["table-uuid"].get<std::string>();
    }
    parsed.current_snapshot_id = load_result.metadata.current_snapshot_id;
    parsed.format_version = load_result.metadata.format_version > 0
                                ? load_result.metadata.format_version
                                : payload.value("format-version", 2);
    parsed.current_schema_id = payload.value("current-schema-id", 0);
    parsed.default_spec_id = payload.value("default-spec-id", 0);
    if (payload.contains("last-sequence-number")) {
      parsed.last_sequence_number =
          payload["last-sequence-number"].get<uint64_t>();
    }

    if (payload.contains("schemas") && payload["schemas"].is_array()) {
      for (const auto &schema : payload["schemas"]) {
        if (!schema.is_object()) {
          continue;
        }
        if (schema.value("schema-id", -1) == parsed.current_schema_id) {
          parsed.current_schema_json = schema.dump();
          break;
        }
      }
    }
    if (parsed.current_schema_json.empty() &&
        payload.contains("schema") && payload["schema"].is_object()) {
      parsed.current_schema_json = payload["schema"].dump();
    }

    if (parsed.current_schema_json.empty()) {
      if (error != nullptr) {
        *error = "table metadata does not contain a current schema";
      }
      return false;
    }

    if (payload.contains("partition-specs") && payload["partition-specs"].is_array()) {
      for (const auto &spec : payload["partition-specs"]) {
        if (!spec.is_object()) {
          continue;
        }
        if (spec.value("spec-id", -1) == parsed.default_spec_id) {
          if (spec.contains("fields")) {
            parsed.current_partition_fields_json = spec["fields"].dump();
            if (spec["fields"].is_array() && !spec["fields"].empty()) {
              if (error != nullptr) {
                *error = "partitioned Iceberg tables are not implemented";
              }
              return false;
            }
          }
          break;
        }
      }
    } else if (payload.contains("partition-spec") &&
               payload["partition-spec"].is_array()) {
      parsed.current_partition_fields_json = payload["partition-spec"].dump();
      if (!payload["partition-spec"].empty()) {
        if (error != nullptr) {
          *error = "partitioned Iceberg tables are not implemented";
        }
        return false;
      }
    }

    *state = std::move(parsed);
    return true;
  } catch (const std::exception &ex) {
    if (error != nullptr) {
      *error = ex.what();
    }
    return false;
  }
}

std::string BuildLocalTempPath(const std::string &base_path,
                               const std::string &label,
                               uint64_t snapshot_id,
                               const std::string &token)
{
  return base_path + "." + label + "_" + std::to_string(snapshot_id) + "_" +
         token + ".avro";
}

bool WriteManifestFile(const std::string &path,
                       const ParsedTableState &state,
                       const std::vector<ManifestDataFile> &entries,
                       std::string *error)
{
  std::string records;
  for (const auto &entry : entries) {
    EncodeManifestEntry(&records, entry);
  }

  std::map<std::string, std::string> metadata = {
      {"schema", state.current_schema_json},
      {"schema-id", std::to_string(state.current_schema_id)},
      {"partition-spec", state.current_partition_fields_json},
      {"partition-spec-id", std::to_string(state.default_spec_id)},
      {"format-version", std::to_string(state.format_version)},
      {"content", "data"},
  };
  return WriteAvroObjectContainerFile(path, BuildManifestSchemaJson(), metadata,
                                      records, entries.size(), error);
}

bool WriteManifestListFile(const std::string &path,
                           const ManifestListEntry &entry,
                           std::string *error)
{
  std::string records;
  EncodeManifestListEntry(&records, entry);
  return WriteAvroObjectContainerFile(path, BuildManifestListSchemaJson(), {},
                                      records, 1, error);
}

bool ReadLocalFileSize(const std::string &path, uint64_t *size_out)
{
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return false;
  }

  if (size_out != nullptr) {
    *size_out = static_cast<uint64_t>(stream.tellg());
  }
  return true;
}

uint64_t CurrentTimeMillis()
{
  const auto now = std::chrono::system_clock::now();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now.time_since_epoch())
                                   .count());
}

bool BuildExistingManifestEntries(const TableMetadata &table_metadata,
                                  const ParsedTableState &state,
                                  std::vector<ManifestDataFile> *entries,
                                  std::string *error)
{
  if (entries == nullptr) {
    if (error != nullptr) {
      *error = "entries output must not be null";
    }
    return false;
  }

  entries->clear();
  if (state.current_snapshot_id.empty()) {
    return true;
  }

  if (table_metadata.current_snapshot_id != state.current_snapshot_id) {
    if (error != nullptr) {
      *error =
          "catalog snapshot differs from the local sidecar; refresh is required "
          "before appending";
    }
    return false;
  }

  if (table_metadata.active_files.empty()) {
    if (error != nullptr) {
      *error =
          "existing Iceberg snapshots require active file lineage in the sidecar";
    }
    return false;
  }

  for (const auto &active_file : table_metadata.active_files) {
    if (active_file.path.empty()) {
      continue;
    }

    if (active_file.snapshot_id.empty() || active_file.record_count == 0 ||
        active_file.file_size_bytes == 0) {
      if (error != nullptr) {
        *error =
            "active file lineage is incomplete; existing Iceberg files cannot "
            "be rewritten into a new manifest safely";
      }
      return false;
    }

    ManifestDataFile entry;
    entry.status = 0;
    entry.has_snapshot_id = true;
    if (!ParseUnsigned(active_file.snapshot_id, &entry.snapshot_id)) {
      if (error != nullptr) {
        *error = "active file snapshot_id is not a valid number";
      }
      return false;
    }
    entry.has_sequence_number = true;
    entry.sequence_number = active_file.data_sequence_number;
    entry.has_file_sequence_number = true;
    entry.file_sequence_number = active_file.file_sequence_number;
    entry.file_path = active_file.path;
    entry.record_count = active_file.record_count;
    entry.file_size_bytes = active_file.file_size_bytes;
    entries->push_back(std::move(entry));
  }

  return true;
}

std::vector<ActiveDataFile> BuildCommittedActiveFiles(
    const std::vector<ActiveDataFile> &existing_files,
    const std::vector<ParquetStagedFile> &staged_files,
    uint64_t snapshot_id,
    uint64_t sequence_number)
{
  auto active_files = existing_files;

  for (const auto &staged_file : staged_files) {
    ActiveDataFile active_file;
    active_file.path = staged_file.target_object_path;
    active_file.record_count = staged_file.record_count;
    active_file.file_size_bytes = staged_file.file_size_bytes;
    active_file.snapshot_id = std::to_string(snapshot_id);
    active_file.data_sequence_number = sequence_number;
    active_file.file_sequence_number = sequence_number;
    active_files.push_back(std::move(active_file));
  }

  return active_files;
}

json BuildCommitRequestJson(const ParsedTableState &state,
                            uint64_t snapshot_id,
                            uint64_t sequence_number,
                            const std::string &manifest_list_path,
                            const std::vector<ActiveDataFile> &active_files,
                            const std::vector<ParquetStagedFile> &staged_files)
{
  const uint64_t timestamp_ms = CurrentTimeMillis();
  uint64_t added_records = 0;
  uint64_t added_files_size = 0;
  uint64_t total_records = 0;
  uint64_t total_files_size = 0;

  for (const auto &staged_file : staged_files) {
    added_records += staged_file.record_count;
    added_files_size += staged_file.file_size_bytes;
  }
  for (const auto &active_file : active_files) {
    total_records += active_file.record_count;
    total_files_size += active_file.file_size_bytes;
  }

  json snapshot = {
      {"snapshot-id", snapshot_id},
      {"sequence-number", sequence_number},
      {"timestamp-ms", timestamp_ms},
      {"manifest-list", manifest_list_path},
      {"summary",
       {
           {"operation", "append"},
           {"added-data-files", std::to_string(staged_files.size())},
           {"added-files-size", std::to_string(added_files_size)},
           {"added-records", std::to_string(added_records)},
           {"total-data-files", std::to_string(active_files.size())},
           {"total-files-size", std::to_string(total_files_size)},
           {"total-records", std::to_string(total_records)},
       }},
      {"schema-id", state.current_schema_id},
  };

  uint64_t parent_snapshot_id = 0;
  const bool has_parent_snapshot =
      ParseUnsigned(state.current_snapshot_id, &parent_snapshot_id);
  if (has_parent_snapshot) {
    snapshot["parent-snapshot-id"] = parent_snapshot_id;
  }

  json requirements = json::array();
  requirements.push_back(
      {{"type", "assert-table-uuid"}, {"uuid", state.table_uuid}});
  if (has_parent_snapshot) {
    requirements.push_back({{"type", "assert-ref-snapshot-id"},
                            {"ref", "main"},
                            {"snapshot-id", parent_snapshot_id}});
  }

  json updates = json::array();
  updates.push_back({{"action", "add-snapshot"}, {"snapshot", snapshot}});
  updates.push_back({{"action", "set-snapshot-ref"},
                     {"ref-name", "main"},
                     {"snapshot-id", snapshot_id},
                     {"type", "branch"}});

  return {{"requirements", requirements}, {"updates", updates}};
}

} // namespace

std::vector<std::string> ExtractActiveScanPaths(
    const std::vector<ActiveDataFile> &active_files)
{
  std::vector<std::string> paths;
  paths.reserve(active_files.size());

  for (const auto &file : active_files) {
    if (!file.path.empty()) {
      paths.push_back(file.path);
    }
  }

  return paths;
}

bool BuildIcebergCommitArtifacts(
    const TableMetadata &table_metadata,
    const CatalogLoadTableResult &load_result,
    const std::vector<ParquetStagedFile> &staged_files,
    IcebergCommitArtifacts *artifacts,
    std::string *error)
{
  if (artifacts == nullptr) {
    if (error != nullptr) {
      *error = "artifacts output must not be null";
    }
    return false;
  }

  if (staged_files.empty()) {
    if (error != nullptr) {
      *error = "staged_files must not be empty";
    }
    return false;
  }

  if (!table_metadata.catalog_enabled || !table_metadata.object_store_enabled) {
    if (error != nullptr) {
      *error =
          "Iceberg commits require both catalog and object store configuration";
    }
    return false;
  }

  ParsedTableState state;
  if (!ParseTableState(load_result, &state, error)) {
    return false;
  }

  if (state.table_uuid.empty()) {
    if (error != nullptr) {
      *error = "table UUID is required for Iceberg commits";
    }
    return false;
  }

  std::vector<ManifestDataFile> manifest_entries;
  if (!BuildExistingManifestEntries(table_metadata, state, &manifest_entries,
                                    error)) {
    return false;
  }

  const uint64_t snapshot_id = NextUniqueId();
  const uint64_t sequence_number = state.last_sequence_number + 1;

  for (const auto &staged_file : staged_files) {
    if (staged_file.target_object_path.empty()) {
      if (error != nullptr) {
        *error = "staged file is missing its target object path";
      }
      return false;
    }

    ManifestDataFile entry;
    entry.status = 1;
    entry.file_path = staged_file.target_object_path;
    entry.record_count = staged_file.record_count;
    entry.file_size_bytes = staged_file.file_size_bytes;
    manifest_entries.push_back(std::move(entry));
  }

  const auto token = NextUniqueToken();
  const auto manifest_name =
      "manifest-" + std::to_string(snapshot_id) + "-" + token + ".avro";
  const auto manifest_list_name =
      "snap-" + std::to_string(snapshot_id) + "-1-" + token + ".avro";
  const auto manifest_local_path = BuildLocalTempPath(
      table_metadata.local_paths.table_path, "iceberg_manifest", snapshot_id,
      token);
  const auto manifest_list_local_path = BuildLocalTempPath(
      table_metadata.local_paths.table_path, "iceberg_manifest_list", snapshot_id,
      token);
  const auto manifest_location = ResolveObjectLocation(
      table_metadata.object_store_config, "metadata/" + manifest_name);
  const auto manifest_list_location = ResolveObjectLocation(
      table_metadata.object_store_config, "metadata/" + manifest_list_name);

  if (!WriteManifestFile(manifest_local_path, state, manifest_entries, error)) {
    return false;
  }

  uint64_t manifest_length = 0;
  if (!ReadLocalFileSize(manifest_local_path, &manifest_length)) {
    std::remove(manifest_local_path.c_str());
    if (error != nullptr) {
      *error = "failed to stat the generated Iceberg manifest";
    }
    return false;
  }

  uint64_t added_rows_count = 0;
  uint64_t existing_rows_count = 0;
  uint64_t min_sequence_number = sequence_number;
  for (const auto &entry : manifest_entries) {
    if (entry.status == 0) {
      existing_rows_count += entry.record_count;
      min_sequence_number =
          std::min(min_sequence_number, entry.sequence_number);
    } else if (entry.status == 1) {
      added_rows_count += entry.record_count;
    }
  }

  ManifestListEntry manifest_list_entry;
  manifest_list_entry.manifest_path =
      BuildS3Uri(manifest_location.bucket, manifest_location.key);
  manifest_list_entry.manifest_length = manifest_length;
  manifest_list_entry.partition_spec_id = state.default_spec_id;
  manifest_list_entry.content = 0;
  manifest_list_entry.sequence_number = sequence_number;
  manifest_list_entry.min_sequence_number = min_sequence_number;
  manifest_list_entry.has_added_snapshot_id = true;
  manifest_list_entry.added_snapshot_id = snapshot_id;
  manifest_list_entry.added_files_count = static_cast<int>(staged_files.size());
  manifest_list_entry.existing_files_count =
      static_cast<int>(manifest_entries.size() - staged_files.size());
  manifest_list_entry.deleted_files_count = 0;
  manifest_list_entry.added_rows_count = added_rows_count;
  manifest_list_entry.existing_rows_count = existing_rows_count;
  manifest_list_entry.deleted_rows_count = 0;

  if (!WriteManifestListFile(manifest_list_local_path, manifest_list_entry,
                             error)) {
    std::remove(manifest_local_path.c_str());
    return false;
  }

  uint64_t manifest_list_length = 0;
  if (!ReadLocalFileSize(manifest_list_local_path, &manifest_list_length)) {
    std::remove(manifest_local_path.c_str());
    std::remove(manifest_list_local_path.c_str());
    if (error != nullptr) {
      *error = "failed to stat the generated Iceberg manifest list";
    }
    return false;
  }

  auto active_files = BuildCommittedActiveFiles(table_metadata.active_files,
                                                staged_files, snapshot_id,
                                                sequence_number);
  auto commit_request = BuildCommitRequestJson(
      state, snapshot_id, sequence_number,
      BuildS3Uri(manifest_list_location.bucket, manifest_list_location.key),
      active_files, staged_files);

  artifacts->snapshot_id = snapshot_id;
  artifacts->sequence_number = sequence_number;
  artifacts->manifest_local_path = manifest_local_path;
  artifacts->manifest_location = manifest_location;
  artifacts->manifest_list_local_path = manifest_list_local_path;
  artifacts->manifest_list_location = manifest_list_location;
  artifacts->commit_request_json = commit_request.dump();
  artifacts->active_files = std::move(active_files);
  return true;
}

} // namespace parquet
