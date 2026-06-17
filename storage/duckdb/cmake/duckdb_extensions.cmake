# Extensions required by the DuckDB storage engine plugin for MariaDB.
# This config is passed to DuckDB via DUCKDB_EXTENSION_CONFIGS.

duckdb_extension_load(core_functions)
duckdb_extension_load(icu)
duckdb_extension_load(json)
