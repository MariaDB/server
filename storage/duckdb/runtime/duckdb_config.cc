/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

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

#include "duckdb_config.h"
#include "duckdb_query.h"
#include "duckdb/common/string_util.hpp"

#include <sstream>

namespace myduck
{

ulonglong global_memory_limit= 0;
char *global_duckdb_temp_directory= nullptr;
ulonglong global_max_temp_directory_size= 0;
ulonglong global_max_threads= 0;
ulonglong appender_allocator_flush_threshold= 0;
ulonglong checkpoint_threshold= 268435456; /* 256 MB */
my_bool global_use_dio= FALSE;
my_bool global_scheduler_process_partial= TRUE;
my_bool use_double_for_decimal= TRUE;
my_bool require_primary_key= TRUE;

const char *explain_output_names[]= {"ALL", "OPTIMIZED_ONLY", "PHYSICAL_ONLY",
                                     NullS};

TYPELIB explain_output_typelib= CREATE_TYPELIB_FOR(explain_output_names);

const char *disabled_optimizers_names[]= {"EXPRESSION_REWRITER",
                                          "FILTER_PULLUP",
                                          "FILTER_PUSHDOWN",
                                          "EMPTY_RESULT_PULLUP",
                                          "CTE_FILTER_PUSHER",
                                          "REGEX_RANGE",
                                          "IN_CLAUSE",
                                          "JOIN_ORDER",
                                          "DELIMINATOR",
                                          "UNNEST_REWRITER",
                                          "UNUSED_COLUMNS",
                                          "STATISTICS_PROPAGATION",
                                          "COMMON_SUBEXPRESSIONS",
                                          "COMMON_AGGREGATE",
                                          "COLUMN_LIFETIME",
                                          "BUILD_SIDE_PROBE_SIDE",
                                          "LIMIT_PUSHDOWN",
                                          "TOP_N",
                                          "COMPRESSED_MATERIALIZATION",
                                          "DUPLICATE_GROUPS",
                                          "REORDER_FILTER",
                                          "SAMPLING_PUSHDOWN",
                                          "JOIN_FILTER_PUSHDOWN",
                                          "EXTENSION",
                                          "MATERIALIZED_CTE",
                                          "SUM_REWRITER",
                                          "LATE_MATERIALIZATION",
                                          NullS};

TYPELIB disabled_optimizers_typelib=
    CREATE_TYPELIB_FOR(disabled_optimizers_names);

std::string BytesToHumanReadableString(uint64_t bytes, uint64_t multiplier)
{
  return duckdb::StringUtil::BytesToHumanReadableString(bytes, multiplier);
}

/* ---- ON_UPDATE callbacks for global proxy variables ---- */

void update_memory_limit_cb(MYSQL_THD thd, struct st_mysql_sys_var *var,
                            void *var_ptr, const void *save)
{
  *(ulonglong *) var_ptr= *(const ulonglong *) save;
  std::ostringstream oss;
  if (global_memory_limit == 0)
    oss << "RESET GLOBAL memory_limit";
  else
  {
    oss << "SET GLOBAL memory_limit = '";
    oss << BytesToHumanReadableString(global_memory_limit) << "'";
  }
  duckdb_query(oss.str());
}

void update_max_temp_directory_size_cb(MYSQL_THD thd,
                                       struct st_mysql_sys_var *var,
                                       void *var_ptr, const void *save)
{
  *(ulonglong *) var_ptr= *(const ulonglong *) save;
  std::ostringstream oss;
  if (global_max_temp_directory_size == 0)
    oss << "RESET GLOBAL max_temp_directory_size";
  else
  {
    oss << "SET GLOBAL max_temp_directory_size = '";
    oss << BytesToHumanReadableString(global_max_temp_directory_size) << "'";
  }
  duckdb_query(oss.str());
}

void update_threads_cb(MYSQL_THD thd, struct st_mysql_sys_var *var,
                       void *var_ptr, const void *save)
{
  *(ulonglong *) var_ptr= *(const ulonglong *) save;
  std::ostringstream oss;
  if (global_max_threads == 0)
    oss << "RESET GLOBAL threads";
  else
    oss << "SET GLOBAL threads = " << global_max_threads;
  duckdb_query(oss.str());
}

void update_scheduler_process_partial_cb(MYSQL_THD thd,
                                         struct st_mysql_sys_var *var,
                                         void *var_ptr, const void *save)
{
  *(my_bool *) var_ptr= *(const my_bool *) save;
  std::ostringstream oss;
  oss << "SET scheduler_process_partial = "
      << (global_scheduler_process_partial ? "true" : "false");
  duckdb_query(oss.str());
}

void update_appender_flush_threshold_cb(MYSQL_THD thd,
                                        struct st_mysql_sys_var *var,
                                        void *var_ptr, const void *save)
{
  *(ulonglong *) var_ptr= *(const ulonglong *) save;
  std::ostringstream oss;
  oss << "SET GLOBAL allocator_flush_threshold = '"
      << BytesToHumanReadableString(appender_allocator_flush_threshold) << "'";
  duckdb_query(oss.str());
}

void update_checkpoint_threshold_cb(MYSQL_THD thd,
                                    struct st_mysql_sys_var *var,
                                    void *var_ptr, const void *save)
{
  *(ulonglong *) var_ptr= *(const ulonglong *) save;
  std::ostringstream oss;
  oss << "SET GLOBAL checkpoint_threshold = '";
  oss << BytesToHumanReadableString(checkpoint_threshold) << "'";
  duckdb_query(oss.str());
}

} // namespace myduck
