/*
   Copyright (c) 2012, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include <my_global.h>

/* C++ standard header files */
#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

/* MySQL header files */
#include <sql_show.h>

/* RocksDB header files */
#include "rocksdb/compaction_filter.h"
#include "rocksdb/convenience.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/memtablerep.h"
#include "rocksdb/merge_operator.h"
#include "rocksdb/slice_transform.h"
#include "rocksdb/utilities/transaction_db.h"

/* MyRocks header files */
#include "./ha_rocksdb.h"
#include "./ha_rocksdb_proto.h"
#include "./rdb_cf_manager.h"
#include "./rdb_datadic.h"
#include "./rdb_utils.h"
#include "./rdb_mariadb_server_port.h"

#include "./rdb_mariadb_port.h"

namespace myrocks {

/**
  Define the INFORMATION_SCHEMA (I_S) structures needed by MyRocks storage
  engine.
*/

#define ROCKSDB_FIELD_INFO(_name_, _len_, _type_, _flag_) \
  { _name_, _len_, _type_, 0, _flag_, nullptr, 0 }

#define ROCKSDB_FIELD_INFO_END \
  ROCKSDB_FIELD_INFO(nullptr, 0, MYSQL_TYPE_NULL, 0)

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_CFSTATS dynamic table
 */
namespace RDB_CFSTATS_FIELD {
enum { CF_NAME = 0, STAT_TYPE, VALUE };
}  // namespace RDB_CFSTATS_FIELD

using namespace Show;

static ST_FIELD_INFO rdb_i_s_cfstats_fields_info[] = {
    Column("CF_NAME",   Varchar(NAME_LEN + 1), NOT_NULL),
    Column("STAT_TYPE", Varchar(NAME_LEN + 1), NOT_NULL),
    Column("VALUE",     SLonglong(),           NOT_NULL),
    CEnd()
};

static int rdb_i_s_cfstats_fill_table(
    my_core::THD *const thd, my_core::TABLE_LIST *const tables,
    my_core::Item *const cond MY_ATTRIBUTE((__unused__))) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(tables != nullptr);
  DBUG_ASSERT(tables->table != nullptr);
  DBUG_ASSERT(tables->table->field != nullptr);

  int ret = 0;
  uint64_t val;

  const std::vector<std::pair<const std::string, std::string>> cf_properties = {
      {rocksdb::DB::Properties::kNumImmutableMemTable,
       "NUM_IMMUTABLE_MEM_TABLE"},
      {rocksdb::DB::Properties::kMemTableFlushPending,
       "MEM_TABLE_FLUSH_PENDING"},
      {rocksdb::DB::Properties::kCompactionPending, "COMPACTION_PENDING"},
      {rocksdb::DB::Properties::kCurSizeActiveMemTable,
       "CUR_SIZE_ACTIVE_MEM_TABLE"},
      {rocksdb::DB::Properties::kCurSizeAllMemTables,
       "CUR_SIZE_ALL_MEM_TABLES"},
      {rocksdb::DB::Properties::kNumEntriesActiveMemTable,
       "NUM_ENTRIES_ACTIVE_MEM_TABLE"},
      {rocksdb::DB::Properties::kNumEntriesImmMemTables,
       "NUM_ENTRIES_IMM_MEM_TABLES"},
      {rocksdb::DB::Properties::kEstimateTableReadersMem,
       "NON_BLOCK_CACHE_SST_MEM_USAGE"},
      {rocksdb::DB::Properties::kNumLiveVersions, "NUM_LIVE_VERSIONS"}};

  rocksdb::DB *const rdb = rdb_get_rocksdb_db();

  if (!rdb) {
    DBUG_RETURN(ret);
  }

  const Rdb_cf_manager &cf_manager = rdb_get_cf_manager();

  for (const auto &cf_name : cf_manager.get_cf_names()) {
    DBUG_ASSERT(!cf_name.empty());
    rocksdb::ColumnFamilyHandle *cfh = cf_manager.get_cf(cf_name);
    if (cfh == nullptr) {
      continue;
    }

    for (const auto &property : cf_properties) {
      if (!rdb->GetIntProperty(cfh, property.first, &val)) {
        continue;
      }

      tables->table->field[RDB_CFSTATS_FIELD::CF_NAME]->store(
          cf_name.c_str(), cf_name.size(), system_charset_info);
      tables->table->field[RDB_CFSTATS_FIELD::STAT_TYPE]->store(
          property.second.c_str(), property.second.size(), system_charset_info);
      tables->table->field[RDB_CFSTATS_FIELD::VALUE]->store(val, true);

      ret = static_cast<int>(
          my_core::schema_table_store_record(thd, tables->table));

      if (ret) {
        DBUG_RETURN(ret);
      }
    }
  }

  DBUG_RETURN(0);
}

static int rdb_i_s_cfstats_init(void *p) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  schema = (my_core::ST_SCHEMA_TABLE *)p;

  schema->fields_info = rdb_i_s_cfstats_fields_info;
  schema->fill_table = rdb_i_s_cfstats_fill_table;

  DBUG_RETURN(0);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_DBSTATS dynamic table
 */
namespace RDB_DBSTATS_FIELD {
enum { STAT_TYPE = 0, VALUE };
}  // namespace RDB_DBSTATS_FIELD

static ST_FIELD_INFO rdb_i_s_dbstats_fields_info[] = {
    Column("STAT_TYPE", Varchar(NAME_LEN + 1), NOT_NULL),
    Column("VALUE",     SLonglong(),           NOT_NULL),
    CEnd()};

static int rdb_i_s_dbstats_fill_table(
    my_core::THD *const thd, my_core::TABLE_LIST *const tables,
    my_core::Item *const cond MY_ATTRIBUTE((__unused__))) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(tables != nullptr);
  DBUG_ASSERT(tables->table != nullptr);
  DBUG_ASSERT(tables->table->field != nullptr);

  int ret = 0;
  uint64_t val;

  const std::vector<std::pair<std::string, std::string>> db_properties = {
      {rocksdb::DB::Properties::kBackgroundErrors, "DB_BACKGROUND_ERRORS"},
      {rocksdb::DB::Properties::kNumSnapshots, "DB_NUM_SNAPSHOTS"},
      {rocksdb::DB::Properties::kOldestSnapshotTime,
       "DB_OLDEST_SNAPSHOT_TIME"}};

  rocksdb::DB *const rdb = rdb_get_rocksdb_db();

  if (!rdb) {
    DBUG_RETURN(ret);
  }

  const rocksdb::BlockBasedTableOptions &table_options =
      rdb_get_table_options();

  for (const auto &property : db_properties) {
    if (!rdb->GetIntProperty(property.first, &val)) {
      continue;
    }

    tables->table->field[RDB_DBSTATS_FIELD::STAT_TYPE]->store(
        property.second.c_str(), property.second.size(), system_charset_info);
    tables->table->field[RDB_DBSTATS_FIELD::VALUE]->store(val, true);

    ret = static_cast<int>(
        my_core::schema_table_store_record(thd, tables->table));

    if (ret) {
      DBUG_RETURN(ret);
    }
  }

  /*
    Currently, this can only show the usage of a block cache allocated
    directly by the handlerton. If the column family config specifies a block
    cache (i.e. the column family option has a parameter such as
    block_based_table_factory={block_cache=1G}), then the block cache is
    allocated within the rocksdb::GetColumnFamilyOptionsFromString().

    There is no interface to retrieve this block cache, nor fetch the usage
    information from the column family.
   */
  val = (table_options.block_cache ? table_options.block_cache->GetUsage() : 0);

  tables->table->field[RDB_DBSTATS_FIELD::STAT_TYPE]->store(
      STRING_WITH_LEN("DB_BLOCK_CACHE_USAGE"), system_charset_info);
  tables->table->field[RDB_DBSTATS_FIELD::VALUE]->store(val, true);

  ret =
      static_cast<int>(my_core::schema_table_store_record(thd, tables->table));

  DBUG_RETURN(ret);
}

static int rdb_i_s_dbstats_init(void *const p) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  schema = (my_core::ST_SCHEMA_TABLE *)p;

  schema->fields_info = rdb_i_s_dbstats_fields_info;
  schema->fill_table = rdb_i_s_dbstats_fill_table;

  DBUG_RETURN(0);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_PERF_CONTEXT dynamic table
 */
namespace RDB_PERF_CONTEXT_FIELD {
enum { TABLE_SCHEMA = 0, TABLE_NAME, PARTITION_NAME, STAT_TYPE, VALUE };
}  // namespace RDB_PERF_CONTEXT_FIELD

static ST_FIELD_INFO rdb_i_s_perf_context_fields_info[] = {
    Column("TABLE_SCHEMA",   Varchar(NAME_LEN + 1), NOT_NULL),
    Column("TABLE_NAME",     Varchar(NAME_LEN + 1), NOT_NULL),
    Column("PARTITION_NAME", Varchar(NAME_LEN + 1), NULLABLE),
    Column("STAT_TYPE",      Varchar(NAME_LEN + 1), NOT_NULL),
    Column("VALUE",          SLonglong(),           NOT_NULL),
    CEnd()};

static int rdb_i_s_perf_context_fill_table(
    my_core::THD *const thd, my_core::TABLE_LIST *const tables,
    my_core::Item *const cond MY_ATTRIBUTE((__unused__))) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);
  DBUG_ASSERT(tables->table != nullptr);

  int ret = 0;
  Field **field = tables->table->field;
  DBUG_ASSERT(field != nullptr);

  rocksdb::DB *const rdb = rdb_get_rocksdb_db();

  if (!rdb) {
    DBUG_RETURN(ret);
  }

  const std::vector<std::string> tablenames = rdb_get_open_table_names();

  for (const auto &it : tablenames) {
    std::string str, dbname, tablename, partname;
    Rdb_perf_counters counters;

    int rc = rdb_normalize_tablename(it, &str);

    if (rc != HA_EXIT_SUCCESS) {
      DBUG_RETURN(rc);
    }

    if (rdb_split_normalized_tablename(str, &dbname, &tablename, &partname)) {
      continue;
    }

    if (rdb_get_table_perf_counters(it.c_str(), &counters)) {
      continue;
    }

    field[RDB_PERF_CONTEXT_FIELD::TABLE_SCHEMA]->store(
        dbname.c_str(), dbname.size(), system_charset_info);
    field[RDB_PERF_CONTEXT_FIELD::TABLE_NAME]->store(
        tablename.c_str(), tablename.size(), system_charset_info);

    if (partname.size() == 0) {
      field[RDB_PERF_CONTEXT_FIELD::PARTITION_NAME]->set_null();
    } else {
      field[RDB_PERF_CONTEXT_FIELD::PARTITION_NAME]->set_notnull();
      field[RDB_PERF_CONTEXT_FIELD::PARTITION_NAME]->store(
          partname.c_str(), partname.size(), system_charset_info);
    }

    for (int i = 0; i < PC_MAX_IDX; i++) {
      field[RDB_PERF_CONTEXT_FIELD::STAT_TYPE]->store(
          rdb_pc_stat_types[i].c_str(), rdb_pc_stat_types[i].size(),
          system_charset_info);
      field[RDB_PERF_CONTEXT_FIELD::VALUE]->store(counters.m_value[i], true);

      ret = static_cast<int>(
          my_core::schema_table_store_record(thd, tables->table));

      if (ret) {
        DBUG_RETURN(ret);
      }
    }
  }

  DBUG_RETURN(0);
}

static int rdb_i_s_perf_context_init(void *const p) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  schema = (my_core::ST_SCHEMA_TABLE *)p;

  schema->fields_info = rdb_i_s_perf_context_fields_info;
  schema->fill_table = rdb_i_s_perf_context_fill_table;

  DBUG_RETURN(0);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_PERF_CONTEXT_GLOBAL dynamic table
 */
namespace RDB_PERF_CONTEXT_GLOBAL_FIELD {
enum { STAT_TYPE = 0, VALUE };
}  // namespace RDB_PERF_CONTEXT_GLOBAL_FIELD

static ST_FIELD_INFO rdb_i_s_perf_context_global_fields_info[] = {
    Column("STAT_TYPE", Varchar(NAME_LEN + 1), NOT_NULL),
    Column("VALUE",     SLonglong(),           NOT_NULL),
    CEnd()};

static int rdb_i_s_perf_context_global_fill_table(
    my_core::THD *const thd, my_core::TABLE_LIST *const tables,
    my_core::Item *const cond MY_ATTRIBUTE((__unused__))) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);
  DBUG_ASSERT(tables->table != nullptr);
  DBUG_ASSERT(tables->table->field != nullptr);

  int ret = 0;

  rocksdb::DB *const rdb = rdb_get_rocksdb_db();

  if (!rdb) {
    DBUG_RETURN(ret);
  }

  // Get a copy of the global perf counters.
  Rdb_perf_counters global_counters;
  rdb_get_global_perf_counters(&global_counters);

  for (int i = 0; i < PC_MAX_IDX; i++) {
    tables->table->field[RDB_PERF_CONTEXT_GLOBAL_FIELD::STAT_TYPE]->store(
        rdb_pc_stat_types[i].c_str(), rdb_pc_stat_types[i].size(),
        system_charset_info);
    tables->table->field[RDB_PERF_CONTEXT_GLOBAL_FIELD::VALUE]->store(
        global_counters.m_value[i], true);

    ret = static_cast<int>(
        my_core::schema_table_store_record(thd, tables->table));

    if (ret) {
      DBUG_RETURN(ret);
    }
  }

  DBUG_RETURN(0);
}

static int rdb_i_s_perf_context_global_init(void *const p) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  schema = (my_core::ST_SCHEMA_TABLE *)p;

  schema->fields_info = rdb_i_s_perf_context_global_fields_info;
  schema->fill_table = rdb_i_s_perf_context_global_fill_table;

  DBUG_RETURN(0);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_CFOPTIONS dynamic table
 */
namespace RDB_CFOPTIONS_FIELD {
enum { CF_NAME = 0, OPTION_TYPE, VALUE };
}  // namespace RDB_CFOPTIONS_FIELD

static ST_FIELD_INFO rdb_i_s_cfoptions_fields_info[] = {
    Column("CF_NAME",     Varchar(NAME_LEN + 1), NOT_NULL),
    Column("OPTION_TYPE", Varchar(NAME_LEN + 1), NOT_NULL),
    Column("VALUE",       Varchar(NAME_LEN + 1), NOT_NULL),
    CEnd()};

static int rdb_i_s_cfoptions_fill_table(
    my_core::THD *const thd, my_core::TABLE_LIST *const tables,
    my_core::Item *const cond MY_ATTRIBUTE((__unused__))) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);

  int ret = 0;

  rocksdb::DB *const rdb = rdb_get_rocksdb_db();

  if (!rdb) {
    DBUG_RETURN(ret);
  }

  Rdb_cf_manager &cf_manager = rdb_get_cf_manager();

  for (const auto &cf_name : cf_manager.get_cf_names()) {
    std::string val;
    rocksdb::ColumnFamilyOptions opts;

    DBUG_ASSERT(!cf_name.empty());
    cf_manager.get_cf_options(cf_name, &opts);

    std::vector<std::pair<std::string, std::string>> cf_option_types = {
        {"COMPARATOR", opts.comparator == nullptr
                           ? "NULL"
                           : std::string(opts.comparator->Name())},
        {"MERGE_OPERATOR", opts.merge_operator == nullptr
                               ? "NULL"
                               : std::string(opts.merge_operator->Name())},
        {"COMPACTION_FILTER",
         opts.compaction_filter == nullptr
             ? "NULL"
             : std::string(opts.compaction_filter->Name())},
        {"COMPACTION_FILTER_FACTORY",
         opts.compaction_filter_factory == nullptr
             ? "NULL"
             : std::string(opts.compaction_filter_factory->Name())},
        {"WRITE_BUFFER_SIZE", std::to_string(opts.write_buffer_size)},
        {"MAX_WRITE_BUFFER_NUMBER",
         std::to_string(opts.max_write_buffer_number)},
        {"MIN_WRITE_BUFFER_NUMBER_TO_MERGE",
         std::to_string(opts.min_write_buffer_number_to_merge)},
        {"NUM_LEVELS", std::to_string(opts.num_levels)},
        {"LEVEL0_FILE_NUM_COMPACTION_TRIGGER",
         std::to_string(opts.level0_file_num_compaction_trigger)},
        {"LEVEL0_SLOWDOWN_WRITES_TRIGGER",
         std::to_string(opts.level0_slowdown_writes_trigger)},
        {"LEVEL0_STOP_WRITES_TRIGGER",
         std::to_string(opts.level0_stop_writes_trigger)},
        {"MAX_MEM_COMPACTION_LEVEL",
         std::to_string(opts.max_mem_compaction_level)},
        {"TARGET_FILE_SIZE_BASE", std::to_string(opts.target_file_size_base)},
        {"TARGET_FILE_SIZE_MULTIPLIER",
         std::to_string(opts.target_file_size_multiplier)},
        {"MAX_BYTES_FOR_LEVEL_BASE",
         std::to_string(opts.max_bytes_for_level_base)},
        {"LEVEL_COMPACTION_DYNAMIC_LEVEL_BYTES",
         opts.level_compaction_dynamic_level_bytes ? "ON" : "OFF"},
        {"MAX_BYTES_FOR_LEVEL_MULTIPLIER",
         std::to_string(opts.max_bytes_for_level_multiplier)},
        {"SOFT_RATE_LIMIT", std::to_string(opts.soft_rate_limit)},
        {"HARD_RATE_LIMIT", std::to_string(opts.hard_rate_limit)},
        {"RATE_LIMIT_DELAY_MAX_MILLISECONDS",
         std::to_string(opts.rate_limit_delay_max_milliseconds)},
        {"ARENA_BLOCK_SIZE", std::to_string(opts.arena_block_size)},
        {"DISABLE_AUTO_COMPACTIONS",
         opts.disable_auto_compactions ? "ON" : "OFF"},
        {"PURGE_REDUNDANT_KVS_WHILE_FLUSH",
         opts.purge_redundant_kvs_while_flush ? "ON" : "OFF"},
        {"MAX_SEQUENTIAL_SKIP_IN_ITERATIONS",
         std::to_string(opts.max_sequential_skip_in_iterations)},
        {"MEMTABLE_FACTORY", opts.memtable_factory == nullptr
                                 ? "NULL"
                                 : opts.memtable_factory->Name()},
        {"INPLACE_UPDATE_SUPPORT", opts.inplace_update_support ? "ON" : "OFF"},
        {"INPLACE_UPDATE_NUM_LOCKS",
         opts.inplace_update_num_locks ? "ON" : "OFF"},
        {"MEMTABLE_PREFIX_BLOOM_BITS_RATIO",
         std::to_string(opts.memtable_prefix_bloom_size_ratio)},
        {"MEMTABLE_PREFIX_BLOOM_HUGE_PAGE_TLB_SIZE",
         std::to_string(opts.memtable_huge_page_size)},
        {"BLOOM_LOCALITY", std::to_string(opts.bloom_locality)},
        {"MAX_SUCCESSIVE_MERGES", std::to_string(opts.max_successive_merges)},
        {"OPTIMIZE_FILTERS_FOR_HITS",
         (opts.optimize_filters_for_hits ? "ON" : "OFF")},
    };

    // get MAX_BYTES_FOR_LEVEL_MULTIPLIER_ADDITIONAL option value
    val = opts.max_bytes_for_level_multiplier_additional.empty() ? "NULL" : "";

    for (const auto &level : opts.max_bytes_for_level_multiplier_additional) {
      val.append(std::to_string(level) + ":");
    }

    val.pop_back();
    cf_option_types.push_back(
        {"MAX_BYTES_FOR_LEVEL_MULTIPLIER_ADDITIONAL", val});

    // get COMPRESSION_TYPE option value
    GetStringFromCompressionType(&val, opts.compression);

    if (val.empty()) {
      val = "NULL";
    }

    cf_option_types.push_back({"COMPRESSION_TYPE", val});

    // get COMPRESSION_PER_LEVEL option value
    val = opts.compression_per_level.empty() ? "NULL" : "";

    for (const auto &compression_type : opts.compression_per_level) {
      std::string res;

      GetStringFromCompressionType(&res, compression_type);

      if (!res.empty()) {
        val.append(res + ":");
      }
    }

    val.pop_back();
    cf_option_types.push_back({"COMPRESSION_PER_LEVEL", val});

    // get compression_opts value
    val = std::to_string(opts.compression_opts.window_bits) + ":";
    val.append(std::to_string(opts.compression_opts.level) + ":");
    val.append(std::to_string(opts.compression_opts.strategy));

    cf_option_types.push_back({"COMPRESSION_OPTS", val});

    // bottommost_compression
    if (opts.bottommost_compression) {
      std::string res;

      GetStringFromCompressionType(&res, opts.bottommost_compression);

      if (!res.empty()) {
        cf_option_types.push_back({"BOTTOMMOST_COMPRESSION", res});
      }
    }

    // get PREFIX_EXTRACTOR option
    cf_option_types.push_back(
        {"PREFIX_EXTRACTOR", opts.prefix_extractor == nullptr
                                 ? "NULL"
                                 : std::string(opts.prefix_extractor->Name())});

    // get COMPACTION_STYLE option
    switch (opts.compaction_style) {
      case rocksdb::kCompactionStyleLevel:
        val = "kCompactionStyleLevel";
        break;
      case rocksdb::kCompactionStyleUniversal:
        val = "kCompactionStyleUniversal";
        break;
      case rocksdb::kCompactionStyleFIFO:
        val = "kCompactionStyleFIFO";
        break;
      case rocksdb::kCompactionStyleNone:
        val = "kCompactionStyleNone";
        break;
      default:
        val = "NULL";
    }

    cf_option_types.push_back({"COMPACTION_STYLE", val});

    // get COMPACTION_OPTIONS_UNIVERSAL related options
    const rocksdb::CompactionOptionsUniversal compac_opts =
        opts.compaction_options_universal;

    val = "{SIZE_RATIO=";

    val.append(std::to_string(compac_opts.size_ratio));
    val.append("; MIN_MERGE_WIDTH=");
    val.append(std::to_string(compac_opts.min_merge_width));
    val.append("; MAX_MERGE_WIDTH=");
    val.append(std::to_string(compac_opts.max_merge_width));
    val.append("; MAX_SIZE_AMPLIFICATION_PERCENT=");
    val.append(std::to_string(compac_opts.max_size_amplification_percent));
    val.append("; COMPRESSION_SIZE_PERCENT=");
    val.append(std::to_string(compac_opts.compression_size_percent));
    val.append("; STOP_STYLE=");

    switch (compac_opts.stop_style) {
      case rocksdb::kCompactionStopStyleSimilarSize:
        val.append("kCompactionStopStyleSimilarSize}");
        break;
      case rocksdb::kCompactionStopStyleTotalSize:
        val.append("kCompactionStopStyleTotalSize}");
        break;
      default:
        val.append("}");
    }

    cf_option_types.push_back({"COMPACTION_OPTIONS_UNIVERSAL", val});

    // get COMPACTION_OPTION_FIFO option
    cf_option_types.push_back(
        {"COMPACTION_OPTION_FIFO::MAX_TABLE_FILES_SIZE",
         std::to_string(opts.compaction_options_fifo.max_table_files_size)});

    // get table related options
    std::vector<std::string> table_options =
        split_into_vector(opts.table_factory->GetPrintableTableOptions(), '\n');

    for (auto option : table_options) {
      option.erase(std::remove(option.begin(), option.end(), ' '),
                   option.end());

      int pos = option.find(":");
      std::string option_name = option.substr(0, pos);
      std::string option_value = option.substr(pos + 1, option.length());
      std::transform(option_name.begin(), option_name.end(),
                     option_name.begin(),
                     [](unsigned char c) { return std::toupper(c); });

      cf_option_types.push_back(
          {"TABLE_FACTORY::" + option_name, option_value});
    }

    for (const auto &cf_option_type : cf_option_types) {
      DBUG_ASSERT(tables->table != nullptr);
      DBUG_ASSERT(tables->table->field != nullptr);

      tables->table->field[RDB_CFOPTIONS_FIELD::CF_NAME]->store(
          cf_name.c_str(), cf_name.size(), system_charset_info);
      tables->table->field[RDB_CFOPTIONS_FIELD::OPTION_TYPE]->store(
          cf_option_type.first.c_str(), cf_option_type.first.size(),
          system_charset_info);
      tables->table->field[RDB_CFOPTIONS_FIELD::VALUE]->store(
          cf_option_type.second.c_str(), cf_option_type.second.size(),
          system_charset_info);

      ret = static_cast<int>(
          my_core::schema_table_store_record(thd, tables->table));

      if (ret) {
        DBUG_RETURN(ret);
      }
    }
  }

  DBUG_RETURN(0);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_GLOBAL_INFO dynamic table
 */
namespace RDB_GLOBAL_INFO_FIELD {
enum { TYPE = 0, NAME, VALUE };
}

static ST_FIELD_INFO rdb_i_s_global_info_fields_info[] = {
    Column("TYPE",  Varchar(FN_REFLEN + 1), NOT_NULL),
    Column("NAME",  Varchar(FN_REFLEN + 1), NOT_NULL),
    Column("VALUE", Varchar(FN_REFLEN + 1), NOT_NULL),
    CEnd()};

/*
 * helper function for rdb_i_s_global_info_fill_table
 * to insert (TYPE, KEY, VALUE) rows into
 * information_schema.rocksdb_global_info
 */
static int rdb_global_info_fill_row(my_core::THD *const thd,
                                    my_core::TABLE_LIST *const tables,
                                    const char *const type,
                                    const char *const name,
                                    const char *const value) {
  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);
  DBUG_ASSERT(tables->table != nullptr);
  DBUG_ASSERT(type != nullptr);
  DBUG_ASSERT(name != nullptr);
  DBUG_ASSERT(value != nullptr);

  Field **field = tables->table->field;
  DBUG_ASSERT(field != nullptr);

  field[RDB_GLOBAL_INFO_FIELD::TYPE]->store(type, strlen(type),
                                            system_charset_info);
  field[RDB_GLOBAL_INFO_FIELD::NAME]->store(name, strlen(name),
                                            system_charset_info);
  field[RDB_GLOBAL_INFO_FIELD::VALUE]->store(value, strlen(value),
                                             system_charset_info);

  return my_core::schema_table_store_record(thd, tables->table);
}

static int rdb_i_s_global_info_fill_table(
    my_core::THD *const thd, my_core::TABLE_LIST *const tables,
    my_core::Item *const cond MY_ATTRIBUTE((__unused__))) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);

  static const uint32_t INT_BUF_LEN = 21;
  static const uint32_t CF_ID_INDEX_BUF_LEN = 60;

  int ret = 0;

  rocksdb::DB *const rdb = rdb_get_rocksdb_db();

  if (!rdb) {
    DBUG_RETURN(ret);
  }

  /* binlog info */
  Rdb_binlog_manager *const blm = rdb_get_binlog_manager();
  DBUG_ASSERT(blm != nullptr);

  char file_buf[FN_REFLEN + 1] = {0};
  my_off_t pos = 0;
  char pos_buf[INT_BUF_LEN] = {0};
  char gtid_buf[GTID_BUF_LEN] = {0};

  if (blm->read(file_buf, &pos, gtid_buf)) {
    snprintf(pos_buf, INT_BUF_LEN, "%llu", (ulonglong)pos);

    ret |= rdb_global_info_fill_row(thd, tables, "BINLOG", "FILE", file_buf);
    ret |= rdb_global_info_fill_row(thd, tables, "BINLOG", "POS", pos_buf);
    ret |= rdb_global_info_fill_row(thd, tables, "BINLOG", "GTID", gtid_buf);
  }

  /* max index info */
  const Rdb_dict_manager *const dict_manager = rdb_get_dict_manager();
  DBUG_ASSERT(dict_manager != nullptr);

  uint32_t max_index_id;
  char max_index_id_buf[INT_BUF_LEN] = {0};

  if (dict_manager->get_max_index_id(&max_index_id)) {
    snprintf(max_index_id_buf, INT_BUF_LEN, "%u", max_index_id);

    ret |= rdb_global_info_fill_row(thd, tables, "MAX_INDEX_ID", "MAX_INDEX_ID",
                                    max_index_id_buf);
  }

  /* cf_id -> cf_flags */
  char cf_id_buf[INT_BUF_LEN] = {0};
  char cf_value_buf[FN_REFLEN + 1] = {0};
  const Rdb_cf_manager &cf_manager = rdb_get_cf_manager();

  for (const auto &cf_handle : cf_manager.get_all_cf()) {
    DBUG_ASSERT(cf_handle != nullptr);

    uint flags;

    if (!dict_manager->get_cf_flags(cf_handle->GetID(), &flags)) {
      // NO_LINT_DEBUG
      sql_print_error(
          "RocksDB: Failed to get column family flags "
          "from CF with id = %u. MyRocks data dictionary may "
          "be corrupted.",
          cf_handle->GetID());
      abort();
    }

    snprintf(cf_id_buf, INT_BUF_LEN, "%u", cf_handle->GetID());
    snprintf(cf_value_buf, FN_REFLEN, "%s [%u]", cf_handle->GetName().c_str(),
             flags);

    ret |= rdb_global_info_fill_row(thd, tables, "CF_FLAGS", cf_id_buf,
                                    cf_value_buf);

    if (ret) {
      break;
    }
  }

  /* DDL_DROP_INDEX_ONGOING */
  std::unordered_set<GL_INDEX_ID> gl_index_ids;
  dict_manager->get_ongoing_index_operation(
      &gl_index_ids, Rdb_key_def::DDL_DROP_INDEX_ONGOING);
  char cf_id_index_buf[CF_ID_INDEX_BUF_LEN] = {0};

  for (auto gl_index_id : gl_index_ids) {
    snprintf(cf_id_index_buf, CF_ID_INDEX_BUF_LEN, "cf_id:%u,index_id:%u",
             gl_index_id.cf_id, gl_index_id.index_id);

    ret |= rdb_global_info_fill_row(thd, tables, "DDL_DROP_INDEX_ONGOING",
                                    cf_id_index_buf, "");

    if (ret) {
      break;
    }
  }

  DBUG_RETURN(ret);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_COMPACTION_STATS dynamic table
 */
static int rdb_i_s_compact_stats_fill_table(
    my_core::THD *thd, my_core::TABLE_LIST *tables,
    my_core::Item *cond MY_ATTRIBUTE((__unused__))) {
  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);

  DBUG_ENTER_FUNC();

  int ret = 0;
  rocksdb::DB *rdb = rdb_get_rocksdb_db();

  if (!rdb) {
    DBUG_RETURN(ret);
  }

  Rdb_cf_manager &cf_manager = rdb_get_cf_manager();

  for (auto cf_name : cf_manager.get_cf_names()) {
    rocksdb::ColumnFamilyHandle *cfh = cf_manager.get_cf(cf_name);

    if (cfh == nullptr) {
      continue;
    }

    std::map<std::string, std::string> props;
    bool bool_ret MY_ATTRIBUTE((__unused__));
    bool_ret = rdb->GetMapProperty(cfh, "rocksdb.cfstats", &props);
    DBUG_ASSERT(bool_ret);

    const std::string prop_name_prefix = "compaction.";
    for (auto const &prop_ent : props) {
      std::string prop_name = prop_ent.first;
      if (prop_name.find(prop_name_prefix) != 0) {
        continue;
      }
      std::string value = prop_ent.second;
      std::size_t del_pos = prop_name.find('.', prop_name_prefix.size());
      DBUG_ASSERT(del_pos != std::string::npos);
      std::string level_str = prop_name.substr(
          prop_name_prefix.size(), del_pos - prop_name_prefix.size());
      std::string type_str = prop_name.substr(del_pos + 1);

      Field **field = tables->table->field;
      DBUG_ASSERT(field != nullptr);

      field[0]->store(cf_name.c_str(), cf_name.size(), system_charset_info);
      field[1]->store(level_str.c_str(), level_str.size(), system_charset_info);
      field[2]->store(type_str.c_str(), type_str.size(), system_charset_info);
      field[3]->store(std::stod(value));

      ret |= static_cast<int>(
          my_core::schema_table_store_record(thd, tables->table));

      if (ret != 0) {
        DBUG_RETURN(ret);
      }
    }
  }

  DBUG_RETURN(ret);
}

static ST_FIELD_INFO rdb_i_s_compact_stats_fields_info[] = {
    Column("CF_NAME", Varchar(NAME_LEN + 1),               NOT_NULL),
    Column("LEVEL",   Varchar(FN_REFLEN + 1),              NOT_NULL),
    Column("TYPE",    Varchar(FN_REFLEN + 1),              NOT_NULL),
    Column("VALUE",   Double(MY_INT64_NUM_DECIMAL_DIGITS), NOT_NULL),
    CEnd()};

namespace  // anonymous namespace = not visible outside this source file
{
struct Rdb_ddl_scanner : public Rdb_tables_scanner {
  my_core::THD *m_thd;
  my_core::TABLE *m_table;

  int add_table(Rdb_tbl_def *tdef) override;
};
}  // anonymous namespace

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_DDL dynamic table
 */
namespace RDB_DDL_FIELD {
enum {
  TABLE_SCHEMA = 0,
  TABLE_NAME,
  PARTITION_NAME,
  INDEX_NAME,
  COLUMN_FAMILY,
  INDEX_NUMBER,
  INDEX_TYPE,
  KV_FORMAT_VERSION,
  TTL_DURATION,
  INDEX_FLAGS,
  CF,
  AUTO_INCREMENT
};
}  // namespace RDB_DDL_FIELD

static ST_FIELD_INFO rdb_i_s_ddl_fields_info[] = {
    Column("TABLE_SCHEMA",      Varchar(NAME_LEN + 1),  NOT_NULL),
    Column("TABLE_NAME",        Varchar(NAME_LEN + 1),  NOT_NULL),
    Column("PARTITION_NAME",    Varchar(NAME_LEN + 1),  NULLABLE),
    Column("INDEX_NAME",        Varchar(NAME_LEN + 1),  NOT_NULL),
    Column("COLUMN_FAMILY",     SLong(),                NOT_NULL),
    Column("INDEX_NUMBER",      SLong(),                NOT_NULL),
    Column("INDEX_TYPE",        SShort(6),              NOT_NULL),
    Column("KV_FORMAT_VERSION", SShort(6),              NOT_NULL),
    Column("TTL_DURATION",      SLonglong(),            NOT_NULL),
    Column("INDEX_FLAGS",       SLonglong(),            NOT_NULL),
    Column("CF",                Varchar(NAME_LEN + 1),  NOT_NULL),
    Column("AUTO_INCREMENT",    ULonglong(),            NULLABLE),
    CEnd()};

int Rdb_ddl_scanner::add_table(Rdb_tbl_def *tdef) {
  DBUG_ASSERT(tdef != nullptr);

  int ret = 0;

  DBUG_ASSERT(m_table != nullptr);
  Field **field = m_table->field;
  DBUG_ASSERT(field != nullptr);
  const Rdb_dict_manager *dict_manager = rdb_get_dict_manager();

  const std::string &dbname = tdef->base_dbname();
  field[RDB_DDL_FIELD::TABLE_SCHEMA]->store(dbname.c_str(), dbname.size(),
                                            system_charset_info);

  const std::string &tablename = tdef->base_tablename();
  field[RDB_DDL_FIELD::TABLE_NAME]->store(tablename.c_str(), tablename.size(),
                                          system_charset_info);

  const std::string &partname = tdef->base_partition();
  if (partname.length() == 0) {
    field[RDB_DDL_FIELD::PARTITION_NAME]->set_null();
  } else {
    field[RDB_DDL_FIELD::PARTITION_NAME]->set_notnull();
    field[RDB_DDL_FIELD::PARTITION_NAME]->store(
        partname.c_str(), partname.size(), system_charset_info);
  }

  for (uint i = 0; i < tdef->m_key_count; i++) {
    const Rdb_key_def &kd = *tdef->m_key_descr_arr[i];

    field[RDB_DDL_FIELD::INDEX_NAME]->store(kd.m_name.c_str(), kd.m_name.size(),
                                            system_charset_info);

    GL_INDEX_ID gl_index_id = kd.get_gl_index_id();
    field[RDB_DDL_FIELD::COLUMN_FAMILY]->store(gl_index_id.cf_id, true);
    field[RDB_DDL_FIELD::INDEX_NUMBER]->store(gl_index_id.index_id, true);
    field[RDB_DDL_FIELD::INDEX_TYPE]->store(kd.m_index_type, true);
    field[RDB_DDL_FIELD::KV_FORMAT_VERSION]->store(kd.m_kv_format_version,
                                                   true);
    field[RDB_DDL_FIELD::TTL_DURATION]->store(kd.m_ttl_duration, true);
    field[RDB_DDL_FIELD::INDEX_FLAGS]->store(kd.m_index_flags_bitmap, true);

    std::string cf_name = kd.get_cf()->GetName();
    field[RDB_DDL_FIELD::CF]->store(cf_name.c_str(), cf_name.size(),
                                    system_charset_info);
    ulonglong auto_incr;
    if (dict_manager->get_auto_incr_val(tdef->get_autoincr_gl_index_id(),
                                        &auto_incr)) {
      field[RDB_DDL_FIELD::AUTO_INCREMENT]->set_notnull();
      field[RDB_DDL_FIELD::AUTO_INCREMENT]->store(auto_incr, true);
    } else {
      field[RDB_DDL_FIELD::AUTO_INCREMENT]->set_null();
    }

    ret = my_core::schema_table_store_record(m_thd, m_table);
    if (ret) return ret;
  }
  return HA_EXIT_SUCCESS;
}

static int rdb_i_s_ddl_fill_table(my_core::THD *const thd,
                                  my_core::TABLE_LIST *const tables,
                                  my_core::Item *const cond) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);
  DBUG_ASSERT(tables->table != nullptr);

  int ret = 0;
  rocksdb::DB *const rdb = rdb_get_rocksdb_db();

  if (!rdb) {
    DBUG_RETURN(ret);
  }

  Rdb_ddl_scanner ddl_arg;

  ddl_arg.m_thd = thd;
  ddl_arg.m_table = tables->table;

  Rdb_ddl_manager *ddl_manager = rdb_get_ddl_manager();
  DBUG_ASSERT(ddl_manager != nullptr);

  ret = ddl_manager->scan_for_tables(&ddl_arg);

  DBUG_RETURN(ret);
}

static int rdb_i_s_ddl_init(void *const p) {
  DBUG_ENTER_FUNC();

  my_core::ST_SCHEMA_TABLE *schema;

  DBUG_ASSERT(p != nullptr);

  schema = (my_core::ST_SCHEMA_TABLE *)p;

  schema->fields_info = rdb_i_s_ddl_fields_info;
  schema->fill_table = rdb_i_s_ddl_fill_table;

  DBUG_RETURN(0);
}

static int rdb_i_s_cfoptions_init(void *const p) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  schema = (my_core::ST_SCHEMA_TABLE *)p;

  schema->fields_info = rdb_i_s_cfoptions_fields_info;
  schema->fill_table = rdb_i_s_cfoptions_fill_table;

  DBUG_RETURN(0);
}

static int rdb_i_s_global_info_init(void *const p) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  schema = reinterpret_cast<my_core::ST_SCHEMA_TABLE *>(p);

  schema->fields_info = rdb_i_s_global_info_fields_info;
  schema->fill_table = rdb_i_s_global_info_fill_table;

  DBUG_RETURN(0);
}

static int rdb_i_s_compact_stats_init(void *p) {
  my_core::ST_SCHEMA_TABLE *schema;

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(p != nullptr);

  schema = reinterpret_cast<my_core::ST_SCHEMA_TABLE *>(p);

  schema->fields_info = rdb_i_s_compact_stats_fields_info;
  schema->fill_table = rdb_i_s_compact_stats_fill_table;

  DBUG_RETURN(0);
}

/* Given a path to a file return just the filename portion. */
static std::string rdb_filename_without_path(const std::string &path) {
  /* Find last slash in path */
  const size_t pos = path.rfind('/');

  /* None found?  Just return the original string */
  if (pos == std::string::npos) {
    return std::string(path);
  }

  /* Return everything after the slash (or backslash) */
  return path.substr(pos + 1);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_SST_PROPS dynamic table
 */
namespace RDB_SST_PROPS_FIELD {
enum {
  SST_NAME = 0,
  COLUMN_FAMILY,
  DATA_BLOCKS,
  ENTRIES,
  RAW_KEY_SIZE,
  RAW_VALUE_SIZE,
  DATA_BLOCK_SIZE,
  INDEX_BLOCK_SIZE,
  INDEX_PARTITIONS,
  TOP_LEVEL_INDEX_SIZE,
  FILTER_BLOCK_SIZE,
  COMPRESSION_ALGO,
  CREATION_TIME,
  FILE_CREATION_TIME,
  OLDEST_KEY_TIME,
  FILTER_POLICY,
  COMPRESSION_OPTIONS,
};
}  // namespace RDB_SST_PROPS_FIELD

static ST_FIELD_INFO rdb_i_s_sst_props_fields_info[] = {
    Column("SST_NAME",             Varchar(NAME_LEN + 1), NOT_NULL),
    Column("COLUMN_FAMILY",        SLong(),               NOT_NULL),
    Column("DATA_BLOCKS",          SLonglong(),           NOT_NULL),
    Column("ENTRIES",              SLonglong(),           NOT_NULL),
    Column("RAW_KEY_SIZE",         SLonglong(),           NOT_NULL),
    Column("RAW_VALUE_SIZE",       SLonglong(),           NOT_NULL),
    Column("DATA_BLOCK_SIZE",      SLonglong(),           NOT_NULL),
    Column("INDEX_BLOCK_SIZE",     SLonglong(),           NOT_NULL),
    Column("INDEX_PARTITIONS",     SLong(),               NOT_NULL),
    Column("TOP_LEVEL_INDEX_SIZE", SLonglong(),           NOT_NULL),
    Column("FILTER_BLOCK_SIZE",    SLonglong(),           NOT_NULL),
    Column("COMPRESSION_ALGO",     Varchar(NAME_LEN + 1), NOT_NULL),
    Column("CREATION_TIME",        SLonglong(),           NOT_NULL),
    Column("FILE_CREATION_TIME",   SLonglong(),           NOT_NULL),
    Column("OLDEST_KEY_TIME",      SLonglong(),           NOT_NULL),
    Column("FILTER_POLICY",        Varchar(NAME_LEN + 1), NOT_NULL),
    Column("COMPRESSION_OPTIONS",  Varchar(NAME_LEN + 1), NOT_NULL),
    CEnd()};

static int rdb_i_s_sst_props_fill_table(
    my_core::THD *const thd, my_core::TABLE_LIST *const tables,
    my_core::Item *const cond MY_ATTRIBUTE((__unused__))) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);
  DBUG_ASSERT(tables->table != nullptr);

  int ret = 0;
  Field **field = tables->table->field;
  DBUG_ASSERT(field != nullptr);

  /* Iterate over all the column families */
  rocksdb::DB *const rdb = rdb_get_rocksdb_db();

  if (!rdb) {
    DBUG_RETURN(ret);
  }

  const Rdb_cf_manager &cf_manager = rdb_get_cf_manager();

  for (const auto &cf_handle : cf_manager.get_all_cf()) {
    /* Grab the the properties of all the tables in the column family */
    rocksdb::TablePropertiesCollection table_props_collection;
    const rocksdb::Status s =
        rdb->GetPropertiesOfAllTables(cf_handle, &table_props_collection);

    if (!s.ok()) {
      continue;
    }

    /* Iterate over all the items in the collection, each of which contains a
     * name and the actual properties */
    for (const auto &props : table_props_collection) {
      /* Add the SST name into the output */
      const std::string sst_name = rdb_filename_without_path(props.first);

      field[RDB_SST_PROPS_FIELD::SST_NAME]->store(
          sst_name.data(), sst_name.size(), system_charset_info);

      field[RDB_SST_PROPS_FIELD::COLUMN_FAMILY]->store(
          props.second->column_family_id, true);
      field[RDB_SST_PROPS_FIELD::DATA_BLOCKS]->store(
          props.second->num_data_blocks, true);
      field[RDB_SST_PROPS_FIELD::ENTRIES]->store(props.second->num_entries,
                                                 true);
      field[RDB_SST_PROPS_FIELD::RAW_KEY_SIZE]->store(
          props.second->raw_key_size, true);
      field[RDB_SST_PROPS_FIELD::RAW_VALUE_SIZE]->store(
          props.second->raw_value_size, true);
      field[RDB_SST_PROPS_FIELD::DATA_BLOCK_SIZE]->store(
          props.second->data_size, true);
      field[RDB_SST_PROPS_FIELD::INDEX_BLOCK_SIZE]->store(
          props.second->index_size, true);
      field[RDB_SST_PROPS_FIELD::INDEX_PARTITIONS]->store(
          props.second->index_partitions, true);
      field[RDB_SST_PROPS_FIELD::TOP_LEVEL_INDEX_SIZE]->store(
          props.second->top_level_index_size, true);
      field[RDB_SST_PROPS_FIELD::FILTER_BLOCK_SIZE]->store(
          props.second->filter_size, true);
      if (props.second->compression_name.empty()) {
        field[RDB_SST_PROPS_FIELD::COMPRESSION_ALGO]->set_null();
      } else {
        field[RDB_SST_PROPS_FIELD::COMPRESSION_ALGO]->store(
            props.second->compression_name.c_str(),
            props.second->compression_name.size(), system_charset_info);
      }
      field[RDB_SST_PROPS_FIELD::CREATION_TIME]->store(
          props.second->creation_time, true);
      field[RDB_SST_PROPS_FIELD::FILE_CREATION_TIME]->store(
          props.second->file_creation_time, true);
      field[RDB_SST_PROPS_FIELD::OLDEST_KEY_TIME]->store(
          props.second->oldest_key_time, true);
      if (props.second->filter_policy_name.empty()) {
        field[RDB_SST_PROPS_FIELD::FILTER_POLICY]->set_null();
      } else {
        field[RDB_SST_PROPS_FIELD::FILTER_POLICY]->store(
            props.second->filter_policy_name.c_str(),
            props.second->filter_policy_name.size(), system_charset_info);
      }
      if (props.second->compression_options.empty()) {
        field[RDB_SST_PROPS_FIELD::COMPRESSION_OPTIONS]->set_null();
      } else {
        field[RDB_SST_PROPS_FIELD::COMPRESSION_OPTIONS]->store(
            props.second->compression_options.c_str(),
            props.second->compression_options.size(), system_charset_info);
      }

      /* Tell MySQL about this row in the virtual table */
      ret = static_cast<int>(
          my_core::schema_table_store_record(thd, tables->table));

      if (ret != 0) {
        DBUG_RETURN(ret);
      }
    }
  }

  DBUG_RETURN(ret);
}

/* Initialize the information_schema.rocksdb_sst_props virtual table */
static int rdb_i_s_sst_props_init(void *const p) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  schema = (my_core::ST_SCHEMA_TABLE *)p;

  schema->fields_info = rdb_i_s_sst_props_fields_info;
  schema->fill_table = rdb_i_s_sst_props_fill_table;

  DBUG_RETURN(0);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_INDEX_FILE_MAP dynamic table
 */
namespace RDB_INDEX_FILE_MAP_FIELD {
enum {
  COLUMN_FAMILY = 0,
  INDEX_NUMBER,
  SST_NAME,
  NUM_ROWS,
  DATA_SIZE,
  ENTRY_DELETES,
  ENTRY_SINGLEDELETES,
  ENTRY_MERGES,
  ENTRY_OTHERS,
  DISTINCT_KEYS_PREFIX
};
}  // namespace RDB_INDEX_FILE_MAP_FIELD

static ST_FIELD_INFO rdb_i_s_index_file_map_fields_info[] = {
    /* The information_schema.rocksdb_index_file_map virtual table has four
     * fields:
     *   COLUMN_FAMILY => the index's column family contained in the SST file
     *   INDEX_NUMBER => the index id contained in the SST file
     *   SST_NAME => the name of the SST file containing some indexes
     *   NUM_ROWS => the number of entries of this index id in this SST file
     *   DATA_SIZE => the data size stored in this SST file for this index id */
    Column("COLUMN_FAMILY",       SLong(),                     NOT_NULL),
    Column("INDEX_NUMBER",        SLong(),                     NOT_NULL),
    Column("SST_NAME",            Varchar(NAME_LEN + 1),       NOT_NULL),
    Column("NUM_ROWS",            SLonglong(),                 NOT_NULL),
    Column("DATA_SIZE",           SLonglong(),                 NOT_NULL),
    Column("ENTRY_DELETES",       SLonglong(),                 NOT_NULL),
    Column("ENTRY_SINGLEDELETES", SLonglong(),                 NOT_NULL),
    Column("ENTRY_MERGES",        SLonglong(),                 NOT_NULL),
    Column("ENTRY_OTHERS",        SLonglong(),                 NOT_NULL),
    Column("DISTINCT_KEYS_PREFIX",Varchar(MAX_REF_PARTS * 25), NOT_NULL),
    CEnd()};

/* Fill the information_schema.rocksdb_index_file_map virtual table */
static int rdb_i_s_index_file_map_fill_table(
    my_core::THD *const thd, my_core::TABLE_LIST *const tables,
    my_core::Item *const cond MY_ATTRIBUTE((__unused__))) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);
  DBUG_ASSERT(tables->table != nullptr);

  int ret = 0;
  Field **field = tables->table->field;
  DBUG_ASSERT(field != nullptr);

  /* Iterate over all the column families */
  rocksdb::DB *const rdb = rdb_get_rocksdb_db();

  if (!rdb) {
    DBUG_RETURN(ret);
  }

  const Rdb_cf_manager &cf_manager = rdb_get_cf_manager();

  for (const auto &cf_handle : cf_manager.get_all_cf()) {
    /* Grab the the properties of all the tables in the column family */
    rocksdb::TablePropertiesCollection table_props_collection;
    const rocksdb::Status s =
        rdb->GetPropertiesOfAllTables(cf_handle, &table_props_collection);

    if (!s.ok()) {
      continue;
    }

    /* Iterate over all the items in the collection, each of which contains a
     * name and the actual properties */
    for (const auto &props : table_props_collection) {
      /* Add the SST name into the output */
      const std::string sst_name = rdb_filename_without_path(props.first);

      field[RDB_INDEX_FILE_MAP_FIELD::SST_NAME]->store(
          sst_name.data(), sst_name.size(), system_charset_info);

      /* Get the __indexstats__ data out of the table property */
      std::vector<Rdb_index_stats> stats;
      Rdb_tbl_prop_coll::read_stats_from_tbl_props(props.second, &stats);

      if (stats.empty()) {
        field[RDB_INDEX_FILE_MAP_FIELD::COLUMN_FAMILY]->store(-1, true);
        field[RDB_INDEX_FILE_MAP_FIELD::INDEX_NUMBER]->store(-1, true);
        field[RDB_INDEX_FILE_MAP_FIELD::NUM_ROWS]->store(-1, true);
        field[RDB_INDEX_FILE_MAP_FIELD::DATA_SIZE]->store(-1, true);
        field[RDB_INDEX_FILE_MAP_FIELD::ENTRY_DELETES]->store(-1, true);
        field[RDB_INDEX_FILE_MAP_FIELD::ENTRY_SINGLEDELETES]->store(-1, true);
        field[RDB_INDEX_FILE_MAP_FIELD::ENTRY_MERGES]->store(-1, true);
        field[RDB_INDEX_FILE_MAP_FIELD::ENTRY_OTHERS]->store(-1, true);
      } else {
        for (const auto &it : stats) {
          /* Add the index number, the number of rows, and data size to the
           * output */
          field[RDB_INDEX_FILE_MAP_FIELD::COLUMN_FAMILY]->store(
              it.m_gl_index_id.cf_id, true);
          field[RDB_INDEX_FILE_MAP_FIELD::INDEX_NUMBER]->store(
              it.m_gl_index_id.index_id, true);
          field[RDB_INDEX_FILE_MAP_FIELD::NUM_ROWS]->store(it.m_rows, true);
          field[RDB_INDEX_FILE_MAP_FIELD::DATA_SIZE]->store(it.m_data_size,
                                                            true);
          field[RDB_INDEX_FILE_MAP_FIELD::ENTRY_DELETES]->store(
              it.m_entry_deletes, true);
          field[RDB_INDEX_FILE_MAP_FIELD::ENTRY_SINGLEDELETES]->store(
              it.m_entry_single_deletes, true);
          field[RDB_INDEX_FILE_MAP_FIELD::ENTRY_MERGES]->store(
              it.m_entry_merges, true);
          field[RDB_INDEX_FILE_MAP_FIELD::ENTRY_OTHERS]->store(
              it.m_entry_others, true);

          std::string distinct_keys_prefix;

          for (size_t i = 0; i < it.m_distinct_keys_per_prefix.size(); i++) {
            if (i > 0) {
              distinct_keys_prefix += ",";
            }

            distinct_keys_prefix +=
                std::to_string(it.m_distinct_keys_per_prefix[i]);
          }

          field[RDB_INDEX_FILE_MAP_FIELD::DISTINCT_KEYS_PREFIX]->store(
              distinct_keys_prefix.data(), distinct_keys_prefix.size(),
              system_charset_info);

          /* Tell MySQL about this row in the virtual table */
          ret = static_cast<int>(
              my_core::schema_table_store_record(thd, tables->table));

          if (ret != 0) {
            break;
          }
        }
      }
    }
  }

  DBUG_RETURN(ret);
}

/* Initialize the information_schema.rocksdb_index_file_map virtual table */
static int rdb_i_s_index_file_map_init(void *const p) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  schema = (my_core::ST_SCHEMA_TABLE *)p;

  schema->fields_info = rdb_i_s_index_file_map_fields_info;
  schema->fill_table = rdb_i_s_index_file_map_fill_table;

  DBUG_RETURN(0);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_LOCKS dynamic table
 */
namespace RDB_LOCKS_FIELD {
enum { COLUMN_FAMILY_ID = 0, TRANSACTION_ID, KEY, MODE };
}  // namespace RDB_LOCKS_FIELD

static ST_FIELD_INFO rdb_i_s_lock_info_fields_info[] = {
    Column("COLUMN_FAMILY_ID", SLong(),                NOT_NULL),
    Column("TRANSACTION_ID",   SLong(),                NOT_NULL),
    Column("KEY",              Varchar(FN_REFLEN + 1), NOT_NULL),
    Column("MODE",             Varchar(32),            NOT_NULL),
    CEnd()};

/* Fill the information_schema.rocksdb_locks virtual table */
static int rdb_i_s_lock_info_fill_table(
    my_core::THD *const thd, my_core::TABLE_LIST *const tables,
    my_core::Item *const cond MY_ATTRIBUTE((__unused__))) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);
  DBUG_ASSERT(tables->table != nullptr);
  DBUG_ASSERT(tables->table->field != nullptr);

  int ret = 0;

  rocksdb::TransactionDB *const rdb = rdb_get_rocksdb_db();

  if (!rdb) {
    DBUG_RETURN(ret);
  }

  /* cf id -> rocksdb::KeyLockInfo */
  std::unordered_multimap<uint32_t, rocksdb::KeyLockInfo> lock_info =
      rdb->GetLockStatusData();

  for (const auto &lock : lock_info) {
    const uint32_t cf_id = lock.first;
    const auto &key_lock_info = lock.second;
    const auto key_hexstr = rdb_hexdump(key_lock_info.key.c_str(),
                                        key_lock_info.key.length(), FN_REFLEN);

    for (const auto &id : key_lock_info.ids) {
      tables->table->field[RDB_LOCKS_FIELD::COLUMN_FAMILY_ID]->store(cf_id,
                                                                     true);
      tables->table->field[RDB_LOCKS_FIELD::TRANSACTION_ID]->store(id, true);

      tables->table->field[RDB_LOCKS_FIELD::KEY]->store(
          key_hexstr.c_str(), key_hexstr.size(), system_charset_info);
      tables->table->field[RDB_LOCKS_FIELD::MODE]->store(
          key_lock_info.exclusive ? "X" : "S", 1, system_charset_info);

      /* Tell MySQL about this row in the virtual table */
      ret = static_cast<int>(
          my_core::schema_table_store_record(thd, tables->table));

      if (ret != 0) {
        break;
      }
    }
  }

  DBUG_RETURN(ret);
}

/* Initialize the information_schema.rocksdb_lock_info virtual table */
static int rdb_i_s_lock_info_init(void *const p) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  schema = (my_core::ST_SCHEMA_TABLE *)p;

  schema->fields_info = rdb_i_s_lock_info_fields_info;
  schema->fill_table = rdb_i_s_lock_info_fill_table;

  DBUG_RETURN(0);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_TRX dynamic table
 */
namespace RDB_TRX_FIELD {
enum {
  TRANSACTION_ID = 0,
  STATE,
  NAME,
  WRITE_COUNT,
  LOCK_COUNT,
  TIMEOUT_SEC,
  WAITING_KEY,
  WAITING_COLUMN_FAMILY_ID,
  IS_REPLICATION,
  SKIP_TRX_API,
  READ_ONLY,
  HAS_DEADLOCK_DETECTION,
  NUM_ONGOING_BULKLOAD,
  THREAD_ID,
  QUERY
};
}  // namespace RDB_TRX_FIELD

static ST_FIELD_INFO rdb_i_s_trx_info_fields_info[] = {
  Column("TRANSACTION_ID",         SLonglong(),            NOT_NULL),
  Column("STATE",                  Varchar(NAME_LEN + 1),  NOT_NULL),
  Column("NAME",                   Varchar(NAME_LEN + 1),  NOT_NULL),
  Column("WRITE_COUNT",            SLonglong(),            NOT_NULL),
  Column("LOCK_COUNT",             SLonglong(),            NOT_NULL),
  Column("TIMEOUT_SEC",            SLong(),                NOT_NULL),
  Column("WAITING_KEY",            Varchar(FN_REFLEN + 1), NOT_NULL),
  Column("WAITING_COLUMN_FAMILY_ID",SLong(),               NOT_NULL),
  Column("IS_REPLICATION",         SLong(),                NOT_NULL),
  Column("SKIP_TRX_API",           SLong(),                NOT_NULL),
  Column("READ_ONLY",              SLong(),                NOT_NULL),
  Column("HAS_DEADLOCK_DETECTION", SLong(),                NOT_NULL),
  Column("NUM_ONGOING_BULKLOAD",   SLong(),                NOT_NULL),
  Column("THREAD_ID",              SLong(),                NOT_NULL),
  Column("QUERY",                  Varchar(NAME_LEN + 1),  NOT_NULL),
  CEnd()};

/* Fill the information_schema.rocksdb_trx virtual table */
static int rdb_i_s_trx_info_fill_table(
    my_core::THD *const thd, my_core::TABLE_LIST *const tables,
    my_core::Item *const cond MY_ATTRIBUTE((__unused__))) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);
  DBUG_ASSERT(tables->table != nullptr);
  DBUG_ASSERT(tables->table->field != nullptr);

  int ret = 0;
  rocksdb::DB *const rdb = rdb_get_rocksdb_db();

  if (!rdb) {
    DBUG_RETURN(ret);
  }

  const std::vector<Rdb_trx_info> &all_trx_info = rdb_get_all_trx_info();

  for (const auto &info : all_trx_info) {
    auto name_hexstr =
        rdb_hexdump(info.name.c_str(), info.name.length(), NAME_LEN);
    auto key_hexstr = rdb_hexdump(info.waiting_key.c_str(),
                                  info.waiting_key.length(), FN_REFLEN);

    tables->table->field[RDB_TRX_FIELD::TRANSACTION_ID]->store(info.trx_id,
                                                               true);
    tables->table->field[RDB_TRX_FIELD::STATE]->store(
        info.state.c_str(), info.state.length(), system_charset_info);
    tables->table->field[RDB_TRX_FIELD::NAME]->store(
        name_hexstr.c_str(), name_hexstr.length(), system_charset_info);
    tables->table->field[RDB_TRX_FIELD::WRITE_COUNT]->store(info.write_count,
                                                            true);
    tables->table->field[RDB_TRX_FIELD::LOCK_COUNT]->store(info.lock_count,
                                                           true);
    tables->table->field[RDB_TRX_FIELD::TIMEOUT_SEC]->store(info.timeout_sec,
                                                            false);
    tables->table->field[RDB_TRX_FIELD::WAITING_KEY]->store(
        key_hexstr.c_str(), key_hexstr.length(), system_charset_info);
    tables->table->field[RDB_TRX_FIELD::WAITING_COLUMN_FAMILY_ID]->store(
        info.waiting_cf_id, true);
    tables->table->field[RDB_TRX_FIELD::IS_REPLICATION]->store(
        info.is_replication, false);
    tables->table->field[RDB_TRX_FIELD::SKIP_TRX_API]->store(info.skip_trx_api,
                                                             false);
    tables->table->field[RDB_TRX_FIELD::READ_ONLY]->store(info.read_only,
                                                          false);
    tables->table->field[RDB_TRX_FIELD::HAS_DEADLOCK_DETECTION]->store(
        info.deadlock_detect, false);
    tables->table->field[RDB_TRX_FIELD::NUM_ONGOING_BULKLOAD]->store(
        info.num_ongoing_bulk_load, false);
    tables->table->field[RDB_TRX_FIELD::THREAD_ID]->store(info.thread_id, true);
    tables->table->field[RDB_TRX_FIELD::QUERY]->store(
        info.query_str.c_str(), info.query_str.length(), system_charset_info);

    /* Tell MySQL about this row in the virtual table */
    ret = static_cast<int>(
        my_core::schema_table_store_record(thd, tables->table));

    if (ret != 0) {
      break;
    }
  }

  DBUG_RETURN(ret);
}

/* Initialize the information_schema.rocksdb_trx_info virtual table */
static int rdb_i_s_trx_info_init(void *const p) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  schema = (my_core::ST_SCHEMA_TABLE *)p;

  schema->fields_info = rdb_i_s_trx_info_fields_info;
  schema->fill_table = rdb_i_s_trx_info_fill_table;

  DBUG_RETURN(0);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_DEADLOCK dynamic table
 */
namespace RDB_DEADLOCK_FIELD {
enum {
  DEADLOCK_ID = 0,
  TIMESTAMP,
  TRANSACTION_ID,
  CF_NAME,
  WAITING_KEY,
  LOCK_TYPE,
  INDEX_NAME,
  TABLE_NAME,
  ROLLED_BACK,
};
}  // namespace RDB_DEADLOCK_FIELD

static ST_FIELD_INFO rdb_i_s_deadlock_info_fields_info[] = {
    Column("DEADLOCK_ID",    SLonglong(),            NOT_NULL),
    Column("TIMESTAMP",      SLonglong(),            NOT_NULL),
    Column("TRANSACTION_ID", SLonglong(),            NOT_NULL),
    Column("CF_NAME",        Varchar(NAME_LEN + 1),  NOT_NULL),
    Column("WAITING_KEY",    Varchar(FN_REFLEN + 1), NOT_NULL),
    Column("LOCK_TYPE",      Varchar(NAME_LEN + 1),  NOT_NULL),
    Column("INDEX_NAME",     Varchar(NAME_LEN + 1),  NOT_NULL),
    Column("TABLE_NAME",     Varchar(NAME_LEN + 1),  NOT_NULL),
    Column("ROLLED_BACK",    SLonglong(),            NOT_NULL),
    CEnd()};

/* Fill the information_schema.rocksdb_trx virtual table */
static int rdb_i_s_deadlock_info_fill_table(
    my_core::THD *const thd, my_core::TABLE_LIST *const tables,
    my_core::Item *const cond MY_ATTRIBUTE((__unused__))) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);
  DBUG_ASSERT(tables->table != nullptr);
  DBUG_ASSERT(tables->table->field != nullptr);

  static const std::string str_exclusive("EXCLUSIVE");
  static const std::string str_shared("SHARED");

  int ret = 0;
  rocksdb::DB *const rdb = rdb_get_rocksdb_db();

  if (!rdb) {
    DBUG_RETURN(ret);
  }

  const std::vector<Rdb_deadlock_info> &all_dl_info = rdb_get_deadlock_info();

  ulonglong id = 0;
  for (const auto &info : all_dl_info) {
    auto deadlock_time = info.deadlock_time;
    for (const auto &trx_info : info.path) {
      tables->table->field[RDB_DEADLOCK_FIELD::DEADLOCK_ID]->store(id, true);
      tables->table->field[RDB_DEADLOCK_FIELD::TIMESTAMP]->store(deadlock_time,
                                                                 true);
      tables->table->field[RDB_DEADLOCK_FIELD::TRANSACTION_ID]->store(
          trx_info.trx_id, true);
      tables->table->field[RDB_DEADLOCK_FIELD::CF_NAME]->store(
          trx_info.cf_name.c_str(), trx_info.cf_name.length(),
          system_charset_info);
      tables->table->field[RDB_DEADLOCK_FIELD::WAITING_KEY]->store(
          trx_info.waiting_key.c_str(), trx_info.waiting_key.length(),
          system_charset_info);
      if (trx_info.exclusive_lock) {
        tables->table->field[RDB_DEADLOCK_FIELD::LOCK_TYPE]->store(
            str_exclusive.c_str(), str_exclusive.length(), system_charset_info);
      } else {
        tables->table->field[RDB_DEADLOCK_FIELD::LOCK_TYPE]->store(
            str_shared.c_str(), str_shared.length(), system_charset_info);
      }
      tables->table->field[RDB_DEADLOCK_FIELD::INDEX_NAME]->store(
          trx_info.index_name.c_str(), trx_info.index_name.length(),
          system_charset_info);
      tables->table->field[RDB_DEADLOCK_FIELD::TABLE_NAME]->store(
          trx_info.table_name.c_str(), trx_info.table_name.length(),
          system_charset_info);
      tables->table->field[RDB_DEADLOCK_FIELD::ROLLED_BACK]->store(
          trx_info.trx_id == info.victim_trx_id, true);

      /* Tell MySQL about this row in the virtual table */
      ret = static_cast<int>(
          my_core::schema_table_store_record(thd, tables->table));

      if (ret != 0) {
        break;
      }
    }
    id++;
  }

  DBUG_RETURN(ret);
}

/* Initialize the information_schema.rocksdb_trx_info virtual table */
static int rdb_i_s_deadlock_info_init(void *const p) {
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  schema = (my_core::ST_SCHEMA_TABLE *)p;

  schema->fields_info = rdb_i_s_deadlock_info_fields_info;
  schema->fill_table = rdb_i_s_deadlock_info_fill_table;

  DBUG_RETURN(0);
}

static int rdb_i_s_deinit(void *p MY_ATTRIBUTE((__unused__))) {
  DBUG_ENTER_FUNC();
  /* see the comment at the end of rocksdb_done_func() */
  DBUG_RETURN(1);
}

static struct st_mysql_information_schema rdb_i_s_info = {
    MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};

struct st_maria_plugin rdb_i_s_cfstats = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &rdb_i_s_info,
    "ROCKSDB_CFSTATS",
    "Facebook",
    "RocksDB column family stats",
    PLUGIN_LICENSE_GPL,
    rdb_i_s_cfstats_init,
    rdb_i_s_deinit,
    0x0001,  /* version number (0.1) */
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL
};

struct st_maria_plugin rdb_i_s_dbstats = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &rdb_i_s_info,
    "ROCKSDB_DBSTATS",
    "Facebook",
    "RocksDB database stats",
    PLUGIN_LICENSE_GPL,
    rdb_i_s_dbstats_init,
    rdb_i_s_deinit,
    0x0001,  /* version number (0.1) */
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL
};

struct st_maria_plugin rdb_i_s_perf_context = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &rdb_i_s_info,
    "ROCKSDB_PERF_CONTEXT",
    "Facebook",
    "RocksDB perf context stats",
    PLUGIN_LICENSE_GPL,
    rdb_i_s_perf_context_init,
    rdb_i_s_deinit,
    0x0001,  /* version number (0.1) */
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL
};

struct st_maria_plugin rdb_i_s_perf_context_global = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &rdb_i_s_info,
    "ROCKSDB_PERF_CONTEXT_GLOBAL",
    "Facebook",
    "RocksDB perf context stats (all)",
    PLUGIN_LICENSE_GPL,
    rdb_i_s_perf_context_global_init,
    rdb_i_s_deinit,
    0x0001,  /* version number (0.1) */
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL
};

struct st_maria_plugin rdb_i_s_cfoptions = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &rdb_i_s_info,
    "ROCKSDB_CF_OPTIONS",
    "Facebook",
    "RocksDB column family options",
    PLUGIN_LICENSE_GPL,
    rdb_i_s_cfoptions_init,
    rdb_i_s_deinit,
    0x0001,  /* version number (0.1) */
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL
};

struct st_maria_plugin rdb_i_s_global_info = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &rdb_i_s_info,
    "ROCKSDB_GLOBAL_INFO",
    "Facebook",
    "RocksDB global info",
    PLUGIN_LICENSE_GPL,
    rdb_i_s_global_info_init,
    rdb_i_s_deinit,
    0x0001,  /* version number (0.1) */
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL
};

struct st_maria_plugin rdb_i_s_compact_stats = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &rdb_i_s_info,
    "ROCKSDB_COMPACTION_STATS",
    "Facebook",
    "RocksDB compaction stats",
    PLUGIN_LICENSE_GPL,
    rdb_i_s_compact_stats_init,
    rdb_i_s_deinit,
    0x0001,  /* version number (0.1) */
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL
};

struct st_maria_plugin rdb_i_s_ddl = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &rdb_i_s_info,
    "ROCKSDB_DDL",
    "Facebook",
    "RocksDB Data Dictionary",
    PLUGIN_LICENSE_GPL,
    rdb_i_s_ddl_init,
    rdb_i_s_deinit,
    0x0001,  /* version number (0.1) */
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL
};

struct st_maria_plugin rdb_i_s_sst_props = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &rdb_i_s_info,
    "ROCKSDB_SST_PROPS",
    "Facebook",
    "RocksDB SST Properties",
    PLUGIN_LICENSE_GPL,
    rdb_i_s_sst_props_init,
    rdb_i_s_deinit,
    0x0001,  /* version number (0.1) */
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL
};

struct st_maria_plugin rdb_i_s_index_file_map = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &rdb_i_s_info,
    "ROCKSDB_INDEX_FILE_MAP",
    "Facebook",
    "RocksDB index file map",
    PLUGIN_LICENSE_GPL,
    rdb_i_s_index_file_map_init,
    rdb_i_s_deinit,
    0x0001,  /* version number (0.1) */
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL
};

struct st_maria_plugin rdb_i_s_lock_info = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &rdb_i_s_info,
    "ROCKSDB_LOCKS",
    "Facebook",
    "RocksDB lock information",
    PLUGIN_LICENSE_GPL,
    rdb_i_s_lock_info_init,
    rdb_i_s_deinit,
    0x0001,  /* version number (0.1) */
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL
};

struct st_maria_plugin rdb_i_s_trx_info = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &rdb_i_s_info,
    "ROCKSDB_TRX",
    "Facebook",
    "RocksDB transaction information",
    PLUGIN_LICENSE_GPL,
    rdb_i_s_trx_info_init,
    rdb_i_s_deinit,
    0x0001,  /* version number (0.1) */
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL
};

struct st_maria_plugin rdb_i_s_deadlock_info = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &rdb_i_s_info,
    "ROCKSDB_DEADLOCK",
    "Facebook",
    "RocksDB transaction information",
    PLUGIN_LICENSE_GPL,
    rdb_i_s_deadlock_info_init,
    rdb_i_s_deinit,
    0x0001,  /* version number (0.1) */
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL
};
}  // namespace myrocks
