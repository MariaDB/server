  #define MYSQL_SERVER 1
  
  #include "my_global.h"
  #include "my_config.h"

  #include "sql/table.h"
  #include "sql/field.h"
  #include "sql/sql_type.h"
  #include "parquet_catalog.h"
  #include "parquet_iceberg.h"
  #include "parquet_metadata.h"
  #include "parquet_object_store.h"
  #include "parquet_transaction.h"

  #include <cstdio>
  #include <string>
  #include <tap.h>


std::string build_query_create(std::string table_name, TABLE *table_arg);

static void test_build_query_basic_schema()
{
  LEX_CSTRING id_name= {STRING_WITH_LEN("id")};
  LEX_CSTRING name_name= {STRING_WITH_LEN("name")};

  TABLE table{};
  TABLE_SHARE share{};
  table.s= &share;

  Field_long id_field(11, false, &id_name, false);
  Field_varstring name_field(255, false, &name_name, &share,
                             DTCollation(&my_charset_bin));

  Field *fields[]= {&id_field, &name_field, nullptr};
  share.field= fields;

  std::string query= build_query_create("users", &table);
  ok(query == "CREATE TABLE users (id INTEGER, name VARCHAR)",
     "build_query maps INTEGER and VARCHAR columns");
}

static void test_build_query_blob_mapping()
{
  LEX_CSTRING payload_name= {STRING_WITH_LEN("payload")};

  TABLE table{};
  TABLE_SHARE share{};
  table.s= &share;

  Field_blob payload_field(1024, false, &payload_name,
                           DTCollation(&my_charset_bin));

  Field *fields[]= {&payload_field, nullptr};
  share.field= fields;

  std::string query= build_query_create("files", &table);
  ok(query == "CREATE TABLE files (payload BLOB)",
     "build_query maps binary blob columns to BLOB");
}

static void test_default_table_options()
{
  auto options= parquet::ResolveDefaultTableOptions();
  ok(options.parquet_version == "latest" &&
     options.block_size_bytes == 16ULL * 1024ULL * 1024ULL &&
     options.compression_codec == "gzip",
     "default table options match Stage 1 local defaults");
}

static void test_local_path_resolution()
{
  auto paths= parquet::ResolveLocalPaths("/tmp/test_db/users");
  ok(paths.table_name == "users" &&
     paths.helper_db_path == "/tmp/test_db/duckdb_helper.duckdb" &&
     paths.parquet_file_path == "/tmp/test_db/users.parquet",
     "local path resolution keeps helper and parquet paths consistent");
}

static void test_staged_file_helpers()
{
  parquet::ParquetStagedFile staged_file;
  ok(!parquet::IsStagedFileComplete(staged_file),
     "default staged file metadata is intentionally incomplete");

  staged_file.table_path= "/tmp/test_db/users";
  staged_file.table_name= "users";
  staged_file.local_parquet_path= "/tmp/test_db/users.stage_7.parquet";
  staged_file.target_object_path= "s3://parquet-stage/users/flush_7.parquet";
  staged_file.record_count= 3;
  staged_file.file_size_bytes= 128;
  staged_file.flush_id= 7;

  ok(parquet::IsStagedFileComplete(staged_file),
     "fully populated staged file metadata validates");
}

static void test_transaction_state_validation()
{
  parquet::ParquetTxnState txn_state;
  std::string error;

  ok(parquet::ValidateTxnState(txn_state, &error),
     "empty transaction state validates");

  txn_state.registered_with_server= true;
  txn_state.staged_files.push_back({
      "/tmp/test_db/users",
      "users",
      "/tmp/test_db/users.stage_7.parquet",
      "s3://parquet-stage/users/flush_7.parquet",
      3,
      128,
      7});
  ok(parquet::ValidateTxnState(txn_state, &error),
     "registered transaction state with staged files validates");

  txn_state.has_error= true;
  ok(!parquet::ValidateTxnState(txn_state, &error) &&
         error == "transaction state is marked as failed",
     "failed transaction state is rejected");
}

static void test_stage_path_helpers()
{
  ok(parquet::BuildLocalStagePath("/tmp/test_db/users.parquet", 7) ==
         "/tmp/test_db/users.stage_7.parquet" &&
         parquet::BuildPrototypeObjectPath("users", 7) ==
             "s3://parquet-stage/users/flush_7.parquet",
     "stage path helpers stay deterministic for Stage 1");
}

static void test_catalog_base_uri_normalization()
{
  ok(parquet::NormalizeCatalogBaseUri("http://localhost:8181/catalog/") ==
         "http://localhost:8181/catalog",
     "catalog base URI normalization strips trailing slashes");
}

static void test_catalog_capability_defaults()
{
  auto capabilities= parquet::ResolveCatalogCapabilities({});
  ok(capabilities.supports_create_table &&
         capabilities.supports_commit_table &&
         capabilities.supports_commit_transaction &&
         capabilities.supports_register_table &&
         !capabilities.supports_scan_planning,
     "empty advertised endpoint list falls back to core REST defaults");
}

static void test_catalog_capability_scan_planning()
{
  auto capabilities= parquet::ResolveCatalogCapabilities(
      {"POST /v1/{prefix}/namespaces/{namespace}/tables/{table}/plan"});
  ok(capabilities.supports_scan_planning &&
         !capabilities.supports_create_table,
     "scan planning capability is detected from advertised endpoints");
}

static void test_namespace_path_encoding()
{
  parquet::CatalogNamespaceIdent ident{{"sales data", "west"}};
  ok(parquet::EncodeNamespaceForUrlPath(ident, "%1F") ==
         "sales%20data%1Fwest",
     "namespace path encoding preserves the configured separator");
}

static void test_object_location_resolution_path_style()
{
  parquet::ObjectStoreConfig config;
  config.endpoint= "https://s3.us-east-1.amazonaws.com/";
  config.bucket= "warehouse";
  config.key_prefix= "/iceberg/tables/";

  auto location= parquet::ResolveObjectLocation(
      config, "/db/users/part-1.parquet");
  ok(location.bucket == "warehouse" &&
         location.key == "iceberg/tables/db/users/part-1.parquet" &&
         location.url ==
             "https://s3.us-east-1.amazonaws.com/warehouse/"
             "iceberg/tables/db/users/part-1.parquet",
     "path-style object locations normalize bucket, prefix, and URL");
}

static void test_object_location_resolution_virtual_host()
{
  parquet::ObjectStoreConfig config;
  config.endpoint= "https://minio.local:9000/base";
  config.bucket= "lake";
  config.url_style= "virtual-host";

  auto location= parquet::ResolveObjectLocation(
      config, "ns/table/data file.parquet");
  ok(location.bucket == "lake" &&
         location.key == "ns/table/data file.parquet" &&
         location.url ==
             "https://lake.minio.local:9000/base/ns/table/data%20file.parquet",
     "virtual-host object locations put the bucket into the authority");
}

static void test_parse_s3_uri()
{
  parquet::ObjectLocation location;
  ok(parquet::ParseS3Uri("s3://warehouse/db/users/part-1.parquet",
                         &location) &&
         location.bucket == "warehouse" &&
         location.key == "db/users/part-1.parquet" &&
         location.url.empty(),
     "S3 URI parsing extracts bucket and key without inventing a URL");
}

static void test_parse_key_value_options()
{
  std::map<std::string, std::string> options;
  std::string error;
  ok(parquet::ParseKeyValueOptions(
         "endpoint=https://minio.local:9000; bucket=warehouse ; region=us-east-1",
         &options, &error) &&
         options["endpoint"] == "https://minio.local:9000" &&
         options["bucket"] == "warehouse" &&
         options["region"] == "us-east-1",
     "key=value option parsing trims whitespace and lowercases keys");
}

static void test_parse_object_store_connection()
{
  parquet::ObjectStoreConfig config;
  std::string error;
  ok(parquet::ParseObjectStoreConnectionString(
         "endpoint=https://minio.local:9000;bucket=warehouse;region=us-east-1;"
         "key_prefix=iceberg/db/t1;access_key_id=minio;secret_access_key=secret;"
         "url_style=virtual-host",
         &config, &error) &&
         config.endpoint == "https://minio.local:9000" &&
         config.bucket == "warehouse" &&
         config.key_prefix == "iceberg/db/t1" &&
         config.url_style == "virtual-host" &&
         config.credentials.access_key_id == "minio",
     "object store connection parsing fills the Stage 1 config shape");
}

static void test_parse_catalog_connection()
{
  parquet::CatalogClientConfig config;
  parquet::CatalogTableIdent ident;
  std::string access_delegation;
  std::string error;
  auto local_paths= parquet::ResolveLocalPaths("/tmp/test_db/users");

  ok(parquet::ParseCatalogConnectionString(
         "uri=http://127.0.0.1:8181/catalog;warehouse=warehouse;"
         "namespace=analytics.stage1;table=users_iceberg;"
         "access_delegation=vended-credentials",
         local_paths, &config, &ident, &access_delegation, &error) &&
         config.base_uri == "http://127.0.0.1:8181/catalog" &&
         config.warehouse == "warehouse" &&
         ident.namespace_ident.parts.size() == 2 &&
         ident.namespace_ident.parts[0] == "analytics" &&
         ident.namespace_ident.parts[1] == "stage1" &&
         ident.table_name == "users_iceberg" &&
         access_delegation == "vended-credentials",
     "catalog connection parsing resolves table identity and delegation");
}

static void test_build_s3_uri_and_absolute_location()
{
  parquet::ObjectStoreConfig config;
  config.endpoint= "https://minio.local:9000";
  config.url_style= "path";

  auto location= parquet::ResolveAbsoluteObjectLocation(
      config, "warehouse", "iceberg/db/users/part-1.parquet");
  ok(parquet::BuildS3Uri("warehouse", "iceberg/db/users/part-1.parquet") ==
         "s3://warehouse/iceberg/db/users/part-1.parquet" &&
         location.url ==
             "https://minio.local:9000/warehouse/"
             "iceberg/db/users/part-1.parquet",
     "absolute object resolution keeps bucket and full object key intact");
}

static void test_metadata_roundtrip()
{
  parquet::TableMetadata metadata;
  metadata.local_paths= parquet::ResolveLocalPaths("/tmp/users_roundtrip");
  metadata.metadata_file_path=
      parquet::ResolveMetadataFilePath(metadata.local_paths.table_path.c_str());
  metadata.table_options= {"2.6", 8ULL * 1024ULL * 1024ULL, "zstd"};
  metadata.catalog_enabled= true;
  metadata.object_store_enabled= true;
  metadata.catalog_config.base_uri= "http://127.0.0.1:8181/catalog";
  metadata.catalog_config.warehouse= "warehouse";
  metadata.catalog_table_ident.namespace_ident.parts= {"analytics"};
  metadata.catalog_table_ident.table_name= "users";
  metadata.access_delegation= "vended-credentials";
  metadata.object_store_config.endpoint= "https://minio.local:9000";
  metadata.object_store_config.bucket= "warehouse";
  metadata.object_store_config.key_prefix= "iceberg/analytics/users";
  metadata.object_store_config.credentials.access_key_id= "minio";
  metadata.object_store_config.credentials.secret_access_key= "secret";
  metadata.table_uuid= "9d4796f7-3c97-4f4f-b1af-3b87b77b4d53";
  metadata.table_location= "s3://warehouse/iceberg/analytics/users";
  metadata.current_snapshot_id= "12345";
  metadata.active_files= {{
      "s3://warehouse/iceberg/analytics/users/data/flush_1.parquet",
      7,
      1024,
      "12345",
      3,
      3,
  }};
  metadata.active_scan_paths= {
      "s3://warehouse/iceberg/analytics/users/data/flush_1.parquet"};

  std::string error;
  ok(parquet::SaveTableMetadata(metadata, &error),
     "table metadata saves to the sidecar file");

  parquet::TableMetadata loaded;
  ok(parquet::LoadTableMetadata(metadata.local_paths.table_path.c_str(),
                                &loaded, &error) &&
         loaded.table_options.parquet_version == "2.6" &&
         loaded.object_store_enabled &&
         loaded.catalog_enabled &&
         loaded.catalog_table_ident.table_name == "users" &&
         loaded.table_uuid == "9d4796f7-3c97-4f4f-b1af-3b87b77b4d53" &&
         loaded.current_snapshot_id == "12345" &&
         loaded.active_files.size() == 1 &&
         loaded.active_files[0].record_count == 7 &&
         loaded.active_scan_paths.size() == 1 &&
         loaded.active_scan_paths[0] ==
             "s3://warehouse/iceberg/analytics/users/data/flush_1.parquet",
     "table metadata round-trips through the JSON sidecar");

  std::remove(metadata.metadata_file_path.c_str());
}

static void test_extract_active_scan_paths()
{
  std::vector<parquet::ActiveDataFile> active_files = {{
      "s3://warehouse/db/t1/data/part-1.parquet", 3, 111, "7", 4, 4},
      {"s3://warehouse/db/t1/data/part-2.parquet", 5, 222, "8", 5, 5},
  };
  auto paths = parquet::ExtractActiveScanPaths(active_files);
  ok(paths.size() == 2 &&
         paths[0] == "s3://warehouse/db/t1/data/part-1.parquet" &&
         paths[1] == "s3://warehouse/db/t1/data/part-2.parquet",
     "active scan paths are derived from active file lineage");
}

static void test_build_iceberg_commit_artifacts()
{
  parquet::TableMetadata table_metadata;
  table_metadata.local_paths = parquet::ResolveLocalPaths("/tmp/iceberg_users");
  table_metadata.catalog_enabled = true;
  table_metadata.object_store_enabled = true;
  table_metadata.catalog_table_ident.namespace_ident.parts = {"analytics"};
  table_metadata.catalog_table_ident.table_name = "users";
  table_metadata.object_store_config.endpoint = "https://minio.local:9000";
  table_metadata.object_store_config.bucket = "warehouse";
  table_metadata.object_store_config.key_prefix = "iceberg/analytics/users";
  table_metadata.table_uuid = "c3163f0d-b617-4d47-bfab-8f5312fdc810";
  table_metadata.table_location = "s3://warehouse/iceberg/analytics/users";

  parquet::CatalogLoadTableResult load_result;
  load_result.metadata.table_uuid = table_metadata.table_uuid;
  load_result.metadata.format_version = 2;
  load_result.metadata.raw_metadata_json =
      R"json({
        "format-version": 2,
        "table-uuid": "c3163f0d-b617-4d47-bfab-8f5312fdc810",
        "current-schema-id": 0,
        "schemas": [{
          "type": "struct",
          "schema-id": 0,
          "identifier-field-ids": [],
          "fields": [{"id": 1, "name": "id", "required": true, "type": "long"}]
        }],
        "default-spec-id": 0,
        "partition-specs": [{"spec-id": 0, "fields": []}],
        "last-sequence-number": 0
      })json";

  std::vector<parquet::ParquetStagedFile> staged_files = {{
      "/tmp/iceberg_users",
      "users",
      "/tmp/iceberg_users.stage_1.parquet",
      "s3://warehouse/iceberg/analytics/users/data/flush_1.parquet",
      3,
      128,
      1,
  }};

  parquet::IcebergCommitArtifacts artifacts;
  std::string error;
  ok(parquet::BuildIcebergCommitArtifacts(table_metadata, load_result,
                                          staged_files, &artifacts, &error) &&
         artifacts.snapshot_id != 0 &&
         artifacts.sequence_number == 1 &&
         artifacts.active_files.size() == 1 &&
         artifacts.active_files[0].snapshot_id ==
             std::to_string(artifacts.snapshot_id) &&
         artifacts.commit_request_json.find("\"action\":\"add-snapshot\"") !=
             std::string::npos &&
         artifacts.commit_request_json.find("\"ref-name\":\"main\"") !=
             std::string::npos,
     "Iceberg commit artifacts include manifests, lineage, and commit updates");

  ok(std::remove(artifacts.manifest_local_path.c_str()) == 0 &&
         std::remove(artifacts.manifest_list_local_path.c_str()) == 0,
     "temporary Iceberg manifest artifacts can be cleaned up locally");
}

int main()
{
  plan(25);

  test_build_query_basic_schema();
  test_build_query_blob_mapping();
  test_default_table_options();
  test_local_path_resolution();
  test_staged_file_helpers();
  test_transaction_state_validation();
  test_stage_path_helpers();
  test_catalog_base_uri_normalization();
  test_catalog_capability_defaults();
  test_catalog_capability_scan_planning();
  test_namespace_path_encoding();
  test_object_location_resolution_path_style();
  test_object_location_resolution_virtual_host();
  test_parse_s3_uri();
  test_parse_key_value_options();
  test_parse_object_store_connection();
  test_parse_catalog_connection();
  test_build_s3_uri_and_absolute_location();
  test_metadata_roundtrip();
  test_extract_active_scan_paths();
  test_build_iceberg_commit_artifacts();

  return exit_status();
}
