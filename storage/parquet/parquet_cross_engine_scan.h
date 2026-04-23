#ifndef PARQUET_CROSS_ENGINE_SCAN_INCLUDED
#define PARQUET_CROSS_ENGINE_SCAN_INCLUDED

#define MYSQL_SERVER 1

#include "duckdb/common/types.hpp"
#include "duckdb/function/replacement_scan.hpp"

#include <string>

struct TABLE;

namespace duckdb
{
class DatabaseInstance;
}

namespace myparquet
{

void register_external_table(const std::string &name, TABLE *table);
void clear_external_tables();
TABLE *find_external_table(const std::string &name);

duckdb::unique_ptr<duckdb::TableRef> mariadb_replacement_scan(
    duckdb::ClientContext &context, duckdb::ReplacementScanInput &input,
    duckdb::optional_ptr<duckdb::ReplacementScanData> data);

void register_cross_engine_scan(duckdb::DatabaseInstance &db);

} // namespace myparquet

#endif
