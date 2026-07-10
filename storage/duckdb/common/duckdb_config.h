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

#ifndef DUCKDB_CONFIG_H
#define DUCKDB_CONFIG_H

#include "duckdb/common/types.hpp"
#include <my_global.h>
#include "typelib.h"

class THD;

namespace myduck
{

/* Global proxy variables */
extern ulonglong global_memory_limit;
extern char *global_duckdb_temp_directory;
extern ulonglong global_max_temp_directory_size;
extern ulonglong global_max_threads;
extern ulonglong appender_allocator_flush_threshold;
extern ulonglong checkpoint_threshold;
extern my_bool global_use_dio;
extern my_bool global_scheduler_process_partial;
extern my_bool use_double_for_decimal;
extern my_bool require_primary_key;

std::string BytesToHumanReadableString(uint64_t bytes,
                                       uint64_t multiplier= 1024);

/* ON_UPDATE callbacks for global proxy variables */
void update_memory_limit_cb(MYSQL_THD thd, struct st_mysql_sys_var *var,
                            void *var_ptr, const void *save);
void update_max_temp_directory_size_cb(MYSQL_THD thd,
                                       struct st_mysql_sys_var *var,
                                       void *var_ptr, const void *save);
void update_threads_cb(MYSQL_THD thd, struct st_mysql_sys_var *var,
                       void *var_ptr, const void *save);
void update_scheduler_process_partial_cb(MYSQL_THD thd,
                                         struct st_mysql_sys_var *var,
                                         void *var_ptr, const void *save);
void update_appender_flush_threshold_cb(MYSQL_THD thd,
                                        struct st_mysql_sys_var *var,
                                        void *var_ptr, const void *save);
void update_checkpoint_threshold_cb(MYSQL_THD thd,
                                    struct st_mysql_sys_var *var,
                                    void *var_ptr, const void *save);

/* Accessor functions for session THDVARs (defined in ha_duckdb.cc) */
ulonglong get_thd_merge_join_threshold(THD *thd);
my_bool get_thd_force_no_collation(THD *thd);
ulong get_thd_explain_output(THD *thd);
ulonglong get_thd_disabled_optimizers(THD *thd);

/* Explain output types */
enum enum_explain_output
{
  EXPLAIN_ALL= 0,
  EXPLAIN_OPTIMIZED_ONLY,
  EXPLAIN_PHYSICAL_ONLY
};

extern const char *explain_output_names[];
extern TYPELIB explain_output_typelib;

/* Disabled optimizers names */
extern const char *disabled_optimizers_names[];
extern TYPELIB disabled_optimizers_typelib;

} // namespace myduck

#endif // DUCKDB_CONFIG_H
