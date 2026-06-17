/*
  Copyright (c) 2026, MariaDB Foundation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.
*/

#pragma once

#include "my_base.h"

/* Handler-level error codes for DuckDB operations */
#define HA_DUCKDB_DML_ERROR HA_ERR_GENERIC
#define HA_DUCKDB_APPEND_ERROR HA_ERR_GENERIC
#define HA_DUCKDB_CREATE_ERROR HA_WRONG_CREATE_OPTION
#define HA_DUCKDB_DROP_TABLE_ERROR HA_ERR_GENERIC
#define HA_DUCKDB_RENAME_ERROR HA_ERR_GENERIC
#define HA_DUCKDB_TRUNCATE_TABLE_ERROR HA_ERR_GENERIC
