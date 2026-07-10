/*
  Copyright (c) 2026, MariaDB Foundation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#pragma once

#include "fiber_context.h"

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"

#include <string>

class THD;
struct TABLE;
class Item;
template <class T> class List;

namespace myduck
{

/**
  Custom select_result_interceptor that writes result rows into
  a DuckDB DataChunk buffer and yields to the caller fiber when
  the chunk is full.

  Lifecycle:
    1. Created by the fiber function before mysql_execute_command().
    2. MariaDB executor calls send_data() once per row.
    3. When the DataChunk is full (STANDARD_VECTOR_SIZE rows),
       send_data() yields back to the DuckDB scan function.
    4. When the query finishes, executor calls send_eof();
       the finished flag is set.
    5. On error, abort_result_set() sets the error flag.
*/
class select_to_duckdb : public select_result_interceptor
{
public:
  select_to_duckdb(THD *thd_arg,
                   struct fiber_context *ctx,
                   duckdb::DataChunk *buffer,
                   const duckdb::vector<duckdb::LogicalType> *types);

  int send_data(List<Item> &items) override;
  bool send_eof() override;
  void abort_result_set() override;

  bool is_finished() const { return finished_; }
  bool has_error() const { return error_; }
  duckdb::idx_t row_count() const { return row_count_; }

private:
  struct fiber_context *ctx_;
  duckdb::DataChunk *buffer_;
  const duckdb::vector<duckdb::LogicalType> *types_;
  duckdb::idx_t row_count_;
  bool finished_;
  bool error_;
};

duckdb::Value item_to_duckdb_value(Item *item,
                                   const duckdb::LogicalType &type);

/**
  Default fiber stack size (512 KB).
  The fiber runs mysql_execute_command() which may use significant stack
  for parsing, optimization and handler calls.  512 KB ensures that
  check_stack_overrun() (which uses my_thread_stack_size, ~292 KB by
  default) will fire before we exhaust the actual fiber stack.
*/
static constexpr size_t FIBER_STACK_SIZE= 512 * 1024;

/**
  Holds all state needed to run a MariaDB SELECT inside a fiber
  and stream result rows into a DuckDB DataChunk buffer.

  Owned by MdbScanGlobalState.  Created lazily on the first call
  to mdb_scan_function when a WHERE clause is available.

  Lifecycle:
    1. Created & initialized in mdb_scan_function (first call).
    2. fiber_context_spawn() starts fiber_scan_func().
    3. Each fiber_context_continue() produces one DataChunk.
    4. Destructor performs graceful teardown (KILL_QUERY + resume).
*/
struct FiberScanState
{
  struct fiber_context ctx;
  THD *fiber_thd= nullptr;
  select_to_duckdb *result= nullptr;

  duckdb::DataChunk buffer;
  duckdb::vector<duckdb::LogicalType> types;
  duckdb::vector<duckdb::idx_t> column_ids;

  TABLE *table= nullptr;
  std::string db_name;
  std::string table_name;
  std::string where_clause;

  bool fiber_started= false;
  bool finished= false;
  bool error= false;

  int init(TABLE *tbl,
           const duckdb::vector<duckdb::idx_t> &col_ids,
           const duckdb::vector<duckdb::LogicalType> &col_types,
           const std::string &where_sql);

  ~FiberScanState();
};

/**
  Fiber entry point.  Receives FiberScanState* as argument.
  Builds a synthetic SELECT, executes it via mysql_execute_command(),
  rows are streamed through select_to_duckdb → DataChunk → yield.
*/
void fiber_scan_func(void *arg);

} /* namespace myduck */
