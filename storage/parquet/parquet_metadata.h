#ifndef PARQUET_METADATA_INCLUDED
#define PARQUET_METADATA_INCLUDED

#include "parquet_catalog.h"
#include "parquet_object_store.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace parquet
{

struct TableOptions {
  std::string parquet_version;
  uint64_t block_size_bytes;
  std::string compression_codec;
};

struct LocalPaths {
  std::string table_path;
  std::string table_name;
  std::string database_name;
  std::string helper_db_path;
  std::string parquet_file_path;
};

struct ActiveDataFile {
  std::string path;
  uint64_t record_count = 0;
  uint64_t file_size_bytes = 0;
  std::string snapshot_id;
  uint64_t data_sequence_number = 0;
  uint64_t file_sequence_number = 0;
};

struct TableMetadata {
  TableOptions table_options;
  LocalPaths local_paths;
  bool catalog_enabled = false;
  bool object_store_enabled = false;
  CatalogClientConfig catalog_config;
  CatalogTableIdent catalog_table_ident;
  std::string access_delegation;
  ObjectStoreConfig object_store_config;
  std::string table_uuid;
  std::string table_location;
  std::string current_snapshot_id;
  std::string metadata_file_path;
  std::vector<ActiveDataFile> active_files;
  std::vector<std::string> active_scan_paths;
};

TableOptions ResolveDefaultTableOptions();
LocalPaths ResolveLocalPaths(const char *table_path);
std::string ResolveMetadataFilePath(const char *table_path);

bool ParseKeyValueOptions(const std::string &serialized,
                          std::map<std::string, std::string> *options,
                          std::string *error);
CatalogTableIdent ResolveCatalogTableIdent(
    const LocalPaths &local_paths,
    const std::map<std::string, std::string> &options);
bool ParseObjectStoreConnectionString(const std::string &serialized,
                                      ObjectStoreConfig *config,
                                      std::string *error);
bool ParseCatalogConnectionString(const std::string &serialized,
                                  const LocalPaths &local_paths,
                                  CatalogClientConfig *config,
                                  CatalogTableIdent *ident,
                                  std::string *access_delegation,
                                  std::string *error);
bool SaveTableMetadata(const TableMetadata &metadata, std::string *error);
bool LoadTableMetadata(const char *table_path, TableMetadata *metadata,
                       std::string *error);

} // namespace parquet

#endif
