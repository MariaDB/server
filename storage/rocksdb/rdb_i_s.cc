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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/* C++ standard header files */
#include <string>
#include <vector>

/* MySQL header files */
#include <sql_show.h>

/* RocksDB header files */
#include "rocksdb/convenience.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/memtablerep.h"
#include "rocksdb/merge_operator.h"
#include "rocksdb/slice_transform.h"

/* MyRocks header files */
#include "./ha_rocksdb.h"
#include "./ha_rocksdb_proto.h"
#include "./rdb_cf_manager.h"
#include "./rdb_datadic.h"

namespace myrocks {

/**
  Define the INFORMATION_SCHEMA (I_S) structures needed by MyRocks storage engine.
*/

#define ROCKSDB_FIELD_INFO(_name_, _len_, _type_, _flag_) \
        { _name_, _len_, _type_, 0, _flag_, nullptr, 0 }

#define ROCKSDB_FIELD_INFO_END ROCKSDB_FIELD_INFO(nullptr, \
        0, MYSQL_TYPE_NULL, 0)

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_CFSTATS dynamic table
 */
static int rdb_i_s_cfstats_fill_table(
    my_core::THD *thd,
    my_core::TABLE_LIST *tables,
    my_core::Item *cond __attribute__((__unused__)))
{
  bool ret;
  uint64_t val;

  DBUG_ENTER("rdb_i_s_cfstats_fill_table");

  std::vector<std::pair<const std::string, std::string>> cf_properties = {
    {rocksdb::DB::Properties::kNumImmutableMemTable, "NUM_IMMUTABLE_MEM_TABLE"},
    {rocksdb::DB::Properties::kMemTableFlushPending,
        "MEM_TABLE_FLUSH_PENDING"},
    {rocksdb::DB::Properties::kCompactionPending, "COMPACTION_PENDING"},
    {rocksdb::DB::Properties::kCurSizeActiveMemTable,
        "CUR_SIZE_ACTIVE_MEM_TABLE"},
    {rocksdb::DB::Properties::kCurSizeAllMemTables, "CUR_SIZE_ALL_MEM_TABLES"},
    {rocksdb::DB::Properties::kNumEntriesActiveMemTable,
        "NUM_ENTRIES_ACTIVE_MEM_TABLE"},
    {rocksdb::DB::Properties::kNumEntriesImmMemTables,
        "NUM_ENTRIES_IMM_MEM_TABLES"},
    {rocksdb::DB::Properties::kEstimateTableReadersMem,
        "NON_BLOCK_CACHE_SST_MEM_USAGE"},
    {rocksdb::DB::Properties::kNumLiveVersions, "NUM_LIVE_VERSIONS"}
  };

  rocksdb::DB *rdb= rdb_get_rocksdb_db();
  Rdb_cf_manager& cf_manager= rdb_get_cf_manager();
  DBUG_ASSERT(rdb != nullptr);

  for (auto cf_name : cf_manager.get_cf_names())
  {
    rocksdb::ColumnFamilyHandle* cfh;
    bool is_automatic;

    /*
      Only the cf name is important. Whether it was generated automatically
      does not matter, so is_automatic is ignored.
    */
    cfh= cf_manager.get_cf(cf_name.c_str(), "", nullptr, &is_automatic);
    if (cfh == nullptr)
      continue;

    for (auto property : cf_properties)
    {
      if (!rdb->GetIntProperty(cfh, property.first, &val))
        continue;

      DBUG_ASSERT(tables != nullptr);

      tables->table->field[0]->store(cf_name.c_str(), cf_name.size(),
                                     system_charset_info);
      tables->table->field[1]->store(property.second.c_str(),
                                     property.second.size(),
                                     system_charset_info);
      tables->table->field[2]->store(val, true);

      ret= my_core::schema_table_store_record(thd, tables->table);

      if (ret)
        DBUG_RETURN(ret);
    }
  }
  DBUG_RETURN(0);
}

static ST_FIELD_INFO rdb_i_s_cfstats_fields_info[]=
{
  ROCKSDB_FIELD_INFO("CF_NAME", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("STAT_TYPE", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("VALUE", sizeof(uint64_t), MYSQL_TYPE_LONGLONG, 0),
  ROCKSDB_FIELD_INFO_END
};

static int rdb_i_s_cfstats_init(void *p)
{
  my_core::ST_SCHEMA_TABLE *schema;

  DBUG_ENTER("rdb_i_s_cfstats_init");
  DBUG_ASSERT(p != nullptr);

  schema= (my_core::ST_SCHEMA_TABLE*) p;

  schema->fields_info= rdb_i_s_cfstats_fields_info;
  schema->fill_table= rdb_i_s_cfstats_fill_table;

  DBUG_RETURN(0);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_DBSTATS dynamic table
 */
static int rdb_i_s_dbstats_fill_table(
    my_core::THD *thd,
    my_core::TABLE_LIST *tables,
    my_core::Item *cond __attribute__((__unused__)))
{
  bool ret;
  uint64_t val;

  DBUG_ENTER("rdb_i_s_dbstats_fill_table");

  std::vector<std::pair<std::string, std::string>> db_properties = {
    {rocksdb::DB::Properties::kBackgroundErrors, "DB_BACKGROUND_ERRORS"},
    {rocksdb::DB::Properties::kNumSnapshots, "DB_NUM_SNAPSHOTS"},
    {rocksdb::DB::Properties::kOldestSnapshotTime, "DB_OLDEST_SNAPSHOT_TIME"}
  };

  rocksdb::DB *rdb= rdb_get_rocksdb_db();
  const rocksdb::BlockBasedTableOptions& table_options=
    rdb_get_table_options();

  for (auto property : db_properties)
  {
    if (!rdb->GetIntProperty(property.first, &val))
      continue;

    DBUG_ASSERT(tables != nullptr);

    tables->table->field[0]->store(property.second.c_str(),
                                   property.second.size(),
                                   system_charset_info);
    tables->table->field[1]->store(val, true);

    ret= my_core::schema_table_store_record(thd, tables->table);

    if (ret)
      DBUG_RETURN(ret);
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
  val= (table_options.block_cache ? table_options.block_cache->GetUsage() : 0);
  tables->table->field[0]->store(STRING_WITH_LEN("DB_BLOCK_CACHE_USAGE"),
                                 system_charset_info);
  tables->table->field[1]->store(val, true);

  ret= my_core::schema_table_store_record(thd, tables->table);

  DBUG_RETURN(ret);
}

static ST_FIELD_INFO rdb_i_s_dbstats_fields_info[]=
{
  ROCKSDB_FIELD_INFO("STAT_TYPE", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("VALUE", sizeof(uint64_t), MYSQL_TYPE_LONGLONG, 0),
  ROCKSDB_FIELD_INFO_END
};

static int rdb_i_s_dbstats_init(void *p)
{
  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  DBUG_ENTER("rdb_i_s_dbstats_init");

  schema= (my_core::ST_SCHEMA_TABLE*) p;

  schema->fields_info= rdb_i_s_dbstats_fields_info;
  schema->fill_table= rdb_i_s_dbstats_fill_table;

  DBUG_RETURN(0);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_PERF_CONTEXT dynamic table
 */

static int rdb_i_s_perf_context_fill_table(
    my_core::THD *thd,
    my_core::TABLE_LIST *tables,
    my_core::Item *cond __attribute__((__unused__)))
{
  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);

  int ret= 0;
  Field** field= tables->table->field;

  DBUG_ENTER("rdb_i_s_perf_context_fill_table");

  std::vector<std::string> tablenames= rdb_get_open_table_names();
  for (const auto& it : tablenames)
  {
    std::string str, dbname, tablename, partname;
    Rdb_perf_counters counters;

    if (rdb_normalize_tablename(it, &str)) {
      return HA_ERR_INTERNAL_ERROR;
    }

    if (rdb_split_normalized_tablename(str, &dbname, &tablename, &partname))
    {
      continue;
    }

    if (rdb_get_table_perf_counters(it.c_str(), &counters))
    {
      continue;
    }

    DBUG_ASSERT(field != nullptr);

    field[0]->store(dbname.c_str(), dbname.size(), system_charset_info);
    field[1]->store(tablename.c_str(), tablename.size(), system_charset_info);
    if (partname.size() == 0)
    {
      field[2]->set_null();
    }
    else
    {
      field[2]->set_notnull();
      field[2]->store(partname.c_str(), partname.size(), system_charset_info);
    }

    for (int i= 0; i < PC_MAX_IDX; i++)
    {
      field[3]->store(rdb_pc_stat_types[i].c_str(), rdb_pc_stat_types[i].size(),
                      system_charset_info);
      field[4]->store(counters.m_value[i], true);

      ret= my_core::schema_table_store_record(thd, tables->table);
      if (ret)
        DBUG_RETURN(ret);
    }
  }

  DBUG_RETURN(0);
}

static ST_FIELD_INFO rdb_i_s_perf_context_fields_info[]=
{
  ROCKSDB_FIELD_INFO("TABLE_SCHEMA", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("TABLE_NAME", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("PARTITION_NAME", NAME_LEN+1, MYSQL_TYPE_STRING,
                     MY_I_S_MAYBE_NULL),
  ROCKSDB_FIELD_INFO("STAT_TYPE", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("VALUE", sizeof(uint64_t), MYSQL_TYPE_LONGLONG,
                     0),
  ROCKSDB_FIELD_INFO_END
};

static int rdb_i_s_perf_context_init(void *p)
{
  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  DBUG_ENTER("rdb_i_s_perf_context_init");

  schema= (my_core::ST_SCHEMA_TABLE*) p;

  schema->fields_info= rdb_i_s_perf_context_fields_info;
  schema->fill_table= rdb_i_s_perf_context_fill_table;

  DBUG_RETURN(0);
}

static int rdb_i_s_perf_context_global_fill_table(
    my_core::THD *thd,
    my_core::TABLE_LIST *tables,
    my_core::Item *cond __attribute__((__unused__)))
{
  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);

  int ret= 0;
  DBUG_ENTER("rdb_i_s_perf_context_global_fill_table");

  // Get a copy of the global perf counters.
  Rdb_perf_counters global_counters;
  rdb_get_global_perf_counters(&global_counters);

  for (int i= 0; i < PC_MAX_IDX; i++) {
    DBUG_ASSERT(tables->table != nullptr);
    DBUG_ASSERT(tables->table->field != nullptr);

    tables->table->field[0]->store(rdb_pc_stat_types[i].c_str(),
                                   rdb_pc_stat_types[i].size(),
                                   system_charset_info);
    tables->table->field[1]->store(global_counters.m_value[i], true);

    ret= my_core::schema_table_store_record(thd, tables->table);
    if (ret)
      DBUG_RETURN(ret);
  }

  DBUG_RETURN(0);
}

static ST_FIELD_INFO rdb_i_s_perf_context_global_fields_info[]=
{
  ROCKSDB_FIELD_INFO("STAT_TYPE", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("VALUE", sizeof(uint64_t), MYSQL_TYPE_LONGLONG, 0),
  ROCKSDB_FIELD_INFO_END
};

static int rdb_i_s_perf_context_global_init(void *p)
{
  DBUG_ASSERT(p != nullptr);

  my_core::ST_SCHEMA_TABLE *schema;

  DBUG_ENTER("rdb_i_s_perf_context_global_init");

  schema= (my_core::ST_SCHEMA_TABLE*) p;

  schema->fields_info= rdb_i_s_perf_context_global_fields_info;
  schema->fill_table= rdb_i_s_perf_context_global_fill_table;

  DBUG_RETURN(0);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_CFOPTIONS dynamic table
 */
static int rdb_i_s_cfoptions_fill_table(
    my_core::THD *thd,
    my_core::TABLE_LIST *tables,
    my_core::Item *cond __attribute__((__unused__)))
{
  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);

  bool ret;

  DBUG_ENTER("rdb_i_s_cfoptions_fill_table");

  Rdb_cf_manager& cf_manager= rdb_get_cf_manager();

  for (auto cf_name : cf_manager.get_cf_names())
  {
    std::string val;
    rocksdb::ColumnFamilyOptions opts;
    cf_manager.get_cf_options(cf_name, &opts);

    std::vector<std::pair<std::string, std::string>> cf_option_types = {
      {"COMPARATOR", opts.comparator == nullptr ? "NULL" :
          std::string(opts.comparator->Name())},
      {"MERGE_OPERATOR", opts.merge_operator == nullptr ? "NULL" :
          std::string(opts.merge_operator->Name())},
      {"COMPACTION_FILTER", opts.compaction_filter == nullptr ? "NULL" :
          std::string(opts.compaction_filter->Name())},
      {"COMPACTION_FILTER_FACTORY",
          opts.compaction_filter_factory == nullptr ? "NULL" :
          std::string(opts.compaction_filter_factory->Name())},
      {"WRITE_BUFFER_SIZE", std::to_string(opts.write_buffer_size)},
      {"MAX_WRITE_BUFFER_NUMBER", std::to_string(opts.max_write_buffer_number)},
      {"MIN_WRITE_BUFFER_NUMBER_TO_MERGE",
          std::to_string(opts.min_write_buffer_number_to_merge)},
      {"NUM_LEVELS", std::to_string(opts.num_levels)},
      {"LEVEL0_FILE_NUM_COMPACTION_TRIGGER",
          std::to_string(opts.level0_file_num_compaction_trigger)},
      {"LEVEL0_SLOWDOWN_WRITES_TRIGGER",
          std::to_string(opts.level0_slowdown_writes_trigger)},
      {"LEVEL0_STOP_WRITES_TRIGGER",
          std::to_string(opts.level0_stop_writes_trigger)},
      {"MAX_MEM_COMPACTION_LEVEL", std::to_string(opts.max_mem_compaction_level)},
      {"TARGET_FILE_SIZE_BASE", std::to_string(opts.target_file_size_base)},
      {"TARGET_FILE_SIZE_MULTIPLIER", std::to_string(opts.target_file_size_multiplier)},
      {"MAX_BYTES_FOR_LEVEL_BASE", std::to_string(opts.max_bytes_for_level_base)},
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
      {"VERIFY_CHECKSUM_IN_COMPACTION",
          opts.verify_checksums_in_compaction ? "ON" : "OFF"},
      {"MAX_SEQUENTIAL_SKIP_IN_ITERATIONS",
          std::to_string(opts.max_sequential_skip_in_iterations)},
      {"MEMTABLE_FACTORY",
          opts.memtable_factory == nullptr ? "NULL" :
            opts.memtable_factory->Name()},
      {"INPLACE_UPDATE_SUPPORT",
          opts.inplace_update_support ? "ON" : "OFF"},
      {"INPLACE_UPDATE_NUM_LOCKS",
          opts.inplace_update_num_locks ? "ON" : "OFF"},
      {"MEMTABLE_PREFIX_BLOOM_BITS_RATIO",
          std::to_string(opts.memtable_prefix_bloom_size_ratio)},
      {"MEMTABLE_PREFIX_BLOOM_HUGE_PAGE_TLB_SIZE",
          std::to_string(opts.memtable_huge_page_size)},
      {"BLOOM_LOCALITY", std::to_string(opts.bloom_locality)},
      {"MAX_SUCCESSIVE_MERGES",
          std::to_string(opts.max_successive_merges)},
      {"MIN_PARTIAL_MERGE_OPERANDS",
          std::to_string(opts.min_partial_merge_operands)},
      {"OPTIMIZE_FILTERS_FOR_HITS",
          (opts.optimize_filters_for_hits ? "ON" : "OFF")},
    };

    // get MAX_BYTES_FOR_LEVEL_MULTIPLIER_ADDITIONAL option value
    val = opts.max_bytes_for_level_multiplier_additional.empty() ? "NULL" : "";
    for (auto level : opts.max_bytes_for_level_multiplier_additional)
    {
      val.append(std::to_string(level) + ":");
    }
    val.pop_back();
    cf_option_types.push_back({"MAX_BYTES_FOR_LEVEL_MULTIPLIER_ADDITIONAL", val});

    // get COMPRESSION_TYPE option value
    GetStringFromCompressionType(&val, opts.compression);
    if (val.empty())
    {
      val = "NULL";
    }
    cf_option_types.push_back({"COMPRESSION_TYPE", val});

    // get COMPRESSION_PER_LEVEL option value
    val = opts.compression_per_level.empty() ? "NULL" : "";
    for (auto compression_type : opts.compression_per_level)
    {
      std::string res;
      GetStringFromCompressionType(&res, compression_type);
      if (!res.empty())
      {
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
    if (opts.bottommost_compression)
    {
      std::string res;
      GetStringFromCompressionType(&res, opts.bottommost_compression);
      if (!res.empty())
      {
        cf_option_types.push_back({"BOTTOMMOST_COMPRESSION", res});
      }
    }

    // get PREFIX_EXTRACTOR option
    cf_option_types.push_back({"PREFIX_EXTRACTOR",
        opts.prefix_extractor == nullptr ? "NULL" :
        std::string(opts.prefix_extractor->Name())});

    // get COMPACTION_STYLE option
    switch (opts.compaction_style)
    {
      case rocksdb::kCompactionStyleLevel: val = "kCompactionStyleLevel"; break;
      case rocksdb::kCompactionStyleUniversal: val = "kCompactionStyleUniversal"; break;
      case rocksdb:: kCompactionStyleFIFO: val = "kCompactionStyleFIFO"; break;
      case rocksdb:: kCompactionStyleNone: val = "kCompactionStyleNone"; break;
      default: val = "NULL";
    }
    cf_option_types.push_back({"COMPACTION_STYLE", val});

    // get COMPACTION_OPTIONS_UNIVERSAL related options
    rocksdb::CompactionOptionsUniversal compac_opts = opts.compaction_options_universal;
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
    switch (compac_opts.stop_style)
    {
      case rocksdb::kCompactionStopStyleSimilarSize:
        val.append("kCompactionStopStyleSimilarSize}"); break;
      case rocksdb::kCompactionStopStyleTotalSize:
        val.append("kCompactionStopStyleTotalSize}"); break;
      default: val.append("}");
    }
    cf_option_types.push_back({"COMPACTION_OPTIONS_UNIVERSAL", val});

    // get COMPACTION_OPTION_FIFO option
    cf_option_types.push_back({"COMPACTION_OPTION_FIFO::MAX_TABLE_FILES_SIZE",
        std::to_string(opts.compaction_options_fifo.max_table_files_size)});

    // get block-based table related options
    const rocksdb::BlockBasedTableOptions& table_options=
      rdb_get_table_options();

    // get BLOCK_BASED_TABLE_FACTORY::CACHE_INDEX_AND_FILTER_BLOCKS option
    cf_option_types.push_back(
        {"BLOCK_BASED_TABLE_FACTORY::CACHE_INDEX_AND_FILTER_BLOCKS",
        table_options.cache_index_and_filter_blocks ? "1" : "0"});

    // get BLOCK_BASED_TABLE_FACTORY::INDEX_TYPE option value
    switch (table_options.index_type)
    {
      case rocksdb::BlockBasedTableOptions::kBinarySearch: val = "kBinarySearch"; break;
      case rocksdb::BlockBasedTableOptions::kHashSearch: val = "kHashSearch"; break;
      default: val = "NULL";
    }
    cf_option_types.push_back({"BLOCK_BASED_TABLE_FACTORY::INDEX_TYPE", val});

    // get BLOCK_BASED_TABLE_FACTORY::HASH_INDEX_ALLOW_COLLISION option value
    cf_option_types.push_back({"BLOCK_BASED_TABLE_FACTORY::HASH_INDEX_ALLOW_COLLISION",
        table_options.hash_index_allow_collision ? "ON" : "OFF"});

    // get BLOCK_BASED_TABLE_FACTORY::CHECKSUM option value
    switch (table_options.checksum)
    {
      case rocksdb::kNoChecksum: val = "kNoChecksum"; break;
      case rocksdb::kCRC32c: val = "kCRC32c"; break;
      case rocksdb::kxxHash: val = "kxxHash"; break;
      default: val = "NULL";
    }
    cf_option_types.push_back({"BLOCK_BASED_TABLE_FACTORY::CHECKSUM", val});

    // get BLOCK_BASED_TABLE_FACTORY::NO_BLOCK_CACHE option value
    cf_option_types.push_back({"BLOCK_BASED_TABLE_FACTORY::NO_BLOCK_CACHE",
        table_options.no_block_cache ? "ON" : "OFF"});

    // get BLOCK_BASED_TABLE_FACTORY::FILTER_POLICY option
    cf_option_types.push_back({"BLOCK_BASED_TABLE_FACTORY::FILTER_POLICY",
        table_options.filter_policy == nullptr ? "NULL" :
          std::string(table_options.filter_policy->Name())});

    // get BLOCK_BASED_TABLE_FACTORY::WHOLE_KEY_FILTERING option
    cf_option_types.push_back({"BLOCK_BASED_TABLE_FACTORY::WHOLE_KEY_FILTERING",
        table_options.whole_key_filtering ? "1" : "0"});

    // get BLOCK_BASED_TABLE_FACTORY::BLOCK_CACHE option
    cf_option_types.push_back({"BLOCK_BASED_TABLE_FACTORY::BLOCK_CACHE",
        table_options.block_cache == nullptr ? "NULL" :
          std::to_string(table_options.block_cache->GetUsage())});

    // get BLOCK_BASED_TABLE_FACTORY::BLOCK_CACHE_COMPRESSED option
    cf_option_types.push_back({"BLOCK_BASED_TABLE_FACTORY::BLOCK_CACHE_COMPRESSED",
        table_options.block_cache_compressed == nullptr ? "NULL" :
          std::to_string(table_options.block_cache_compressed->GetUsage())});

    // get BLOCK_BASED_TABLE_FACTORY::BLOCK_SIZE option
    cf_option_types.push_back({"BLOCK_BASED_TABLE_FACTORY::BLOCK_SIZE",
        std::to_string(table_options.block_size)});

    // get BLOCK_BASED_TABLE_FACTORY::BLOCK_SIZE_DEVIATION option
    cf_option_types.push_back({"BLOCK_BASED_TABLE_FACTORY::BLOCK_SIZE_DEVIATION",
        std::to_string(table_options.block_size_deviation)});

    // get BLOCK_BASED_TABLE_FACTORY::BLOCK_RESTART_INTERVAL option
    cf_option_types.push_back({"BLOCK_BASED_TABLE_FACTORY::BLOCK_RESTART_INTERVAL",
        std::to_string(table_options.block_restart_interval)});

    // get BLOCK_BASED_TABLE_FACTORY::FORMAT_VERSION option
    cf_option_types.push_back({"BLOCK_BASED_TABLE_FACTORY::FORMAT_VERSION",
        std::to_string(table_options.format_version)});

    for (auto cf_option_type : cf_option_types)
    {
      DBUG_ASSERT(tables->table != nullptr);
      DBUG_ASSERT(tables->table->field != nullptr);

      tables->table->field[0]->store(cf_name.c_str(), cf_name.size(),
                                     system_charset_info);
      tables->table->field[1]->store(cf_option_type.first.c_str(),
                                     cf_option_type.first.size(),
                                     system_charset_info);
      tables->table->field[2]->store(cf_option_type.second.c_str(),
                                     cf_option_type.second.size(),
                                     system_charset_info);

      ret = my_core::schema_table_store_record(thd, tables->table);

      if (ret)
        DBUG_RETURN(ret);
    }
  }
  DBUG_RETURN(0);
}

static ST_FIELD_INFO rdb_i_s_cfoptions_fields_info[] =
{
  ROCKSDB_FIELD_INFO("CF_NAME", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("OPTION_TYPE", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("VALUE", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO_END
};

/*
 * helper function for rdb_i_s_global_info_fill_table
 * to insert (TYPE, KEY, VALUE) rows into
 * information_schema.rocksdb_global_info
 */
static int rdb_global_info_fill_row(my_core::THD *thd,
                                    my_core::TABLE_LIST *tables,
                                    const char *type,
                                    const char *name,
                                    const char *value)
{
  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);
  DBUG_ASSERT(tables->table != nullptr);
  DBUG_ASSERT(type != nullptr);
  DBUG_ASSERT(name != nullptr);
  DBUG_ASSERT(value != nullptr);

  Field **field= tables->table->field;
  DBUG_ASSERT(field != nullptr);

  field[0]->store(type, strlen(type), system_charset_info);
  field[1]->store(name, strlen(name), system_charset_info);
  field[2]->store(value, strlen(value), system_charset_info);

  return my_core::schema_table_store_record(thd, tables->table);
}

/*
  Support for INFORMATION_SCHEMA.ROCKSDB_GLOBAL_INFO dynamic table
 */
static int rdb_i_s_global_info_fill_table(
    my_core::THD *thd,
    my_core::TABLE_LIST *tables,
    my_core::Item *cond __attribute__((__unused__)))
{
  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);

  DBUG_ENTER("rdb_i_s_global_info_fill_table");
  static const uint32_t INT_BUF_LEN = 21;
  static const uint32_t GTID_BUF_LEN = 60;
  static const uint32_t CF_ID_INDEX_BUF_LEN = 60;

  int ret= 0;

  /* binlog info */
  Rdb_binlog_manager *blm= rdb_get_binlog_manager();
  DBUG_ASSERT(blm != nullptr);

  char file_buf[FN_REFLEN+1]= {0};
  my_off_t pos = 0;
  char pos_buf[INT_BUF_LEN]= {0};
  char gtid_buf[GTID_BUF_LEN]= {0};

  if (blm->read(file_buf, &pos, gtid_buf)) {
    snprintf(pos_buf, INT_BUF_LEN, "%lu", (uint64_t) pos);
    ret |= rdb_global_info_fill_row(thd, tables, "BINLOG", "FILE", file_buf);
    ret |= rdb_global_info_fill_row(thd, tables, "BINLOG", "POS", pos_buf);
    ret |= rdb_global_info_fill_row(thd, tables, "BINLOG", "GTID", gtid_buf);
  }

  /* max index info */
  Rdb_dict_manager *dict_manager= rdb_get_dict_manager();
  DBUG_ASSERT(dict_manager != nullptr);

  uint32_t max_index_id;
  char max_index_id_buf[INT_BUF_LEN]= {0};

  if (dict_manager->get_max_index_id(&max_index_id)) {
    snprintf(max_index_id_buf, INT_BUF_LEN, "%u", max_index_id);
    ret |= rdb_global_info_fill_row(thd, tables, "MAX_INDEX_ID", "MAX_INDEX_ID",
                                    max_index_id_buf);
  }

  /* cf_id -> cf_flags */
  char cf_id_buf[INT_BUF_LEN]= {0};
  char cf_value_buf[FN_REFLEN+1] = {0};
  Rdb_cf_manager& cf_manager= rdb_get_cf_manager();
  for (auto cf_handle : cf_manager.get_all_cf()) {
    uint flags;
    dict_manager->get_cf_flags(cf_handle->GetID(), &flags);
    snprintf(cf_id_buf, INT_BUF_LEN, "%u", cf_handle->GetID());
    snprintf(cf_value_buf, FN_REFLEN, "%s [%u]", cf_handle->GetName().c_str(),
        flags);
    ret |= rdb_global_info_fill_row(thd, tables, "CF_FLAGS", cf_id_buf,
        cf_value_buf);

    if (ret)
      break;
  }

  /* DDL_DROP_INDEX_ONGOING */
  std::vector<GL_INDEX_ID> gl_index_ids;
  dict_manager->get_ongoing_index_operation(&gl_index_ids,
      Rdb_key_def::DDL_DROP_INDEX_ONGOING);
  char cf_id_index_buf[CF_ID_INDEX_BUF_LEN]= {0};
  for (auto gl_index_id : gl_index_ids) {
    snprintf(cf_id_index_buf, CF_ID_INDEX_BUF_LEN, "cf_id:%u,index_id:%u",
        gl_index_id.cf_id, gl_index_id.index_id);
    ret |= rdb_global_info_fill_row(thd, tables, "DDL_DROP_INDEX_ONGOING",
        cf_id_index_buf, "");

    if (ret)
      break;
  }

  DBUG_RETURN(ret);
}

static ST_FIELD_INFO rdb_i_s_global_info_fields_info[] =
{
  ROCKSDB_FIELD_INFO("TYPE", FN_REFLEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("NAME", FN_REFLEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("VALUE", FN_REFLEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO_END
};


namespace  // anonymous namespace = not visible outside this source file
{
struct Rdb_ddl_scanner : public Rdb_tables_scanner
{
  my_core::THD   *m_thd;
  my_core::TABLE *m_table;

  int add_table(Rdb_tbl_def* tdef) override;
};
}  // anonymous namespace


int Rdb_ddl_scanner::add_table(Rdb_tbl_def *tdef)
{
  DBUG_ASSERT(tdef != nullptr);

  int ret= 0;

  DBUG_ASSERT(m_table != nullptr);
  Field** field= m_table->field;
  DBUG_ASSERT(field != nullptr);

  const std::string& dbname= tdef->base_dbname();
  field[0]->store(dbname.c_str(), dbname.size(), system_charset_info);

  const std::string& tablename= tdef->base_tablename();
  field[1]->store(tablename.c_str(), tablename.size(), system_charset_info);

  const std::string& partname= tdef->base_partition();
  if (partname.length() == 0)
  {
    field[2]->set_null();
  }
  else
  {
    field[2]->set_notnull();
    field[2]->store(partname.c_str(), partname.size(), system_charset_info);
  }

  for (uint i= 0; i < tdef->m_key_count; i++)
  {
    const std::shared_ptr<const Rdb_key_def>& kd= tdef->m_key_descr_arr[i];
    DBUG_ASSERT(kd != nullptr);

    field[3]->store(kd->m_name.c_str(), kd->m_name.size(), system_charset_info);

    GL_INDEX_ID gl_index_id = kd->get_gl_index_id();
    field[4]->store(gl_index_id.cf_id, true);
    field[5]->store(gl_index_id.index_id, true);
    field[6]->store(kd->m_index_type, true);
    field[7]->store(kd->m_kv_format_version, true);

    std::string cf_name= kd->get_cf()->GetName();
    field[8]->store(cf_name.c_str(), cf_name.size(), system_charset_info);

    ret= my_core::schema_table_store_record(m_thd, m_table);
    if (ret)
      return ret;
  }
  return 0;
}

static int rdb_i_s_ddl_fill_table(my_core::THD *thd,
                                  my_core::TABLE_LIST *tables,
                                  my_core::Item *cond)
{
  DBUG_ENTER("rdb_i_s_ddl_fill_table");

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);

  Rdb_ddl_scanner ddl_arg;
  ddl_arg.m_thd= thd;
  ddl_arg.m_table= tables->table;

  Rdb_ddl_manager *ddl_manager= rdb_get_ddl_manager();
  DBUG_ASSERT(ddl_manager != nullptr);
  int ret= ddl_manager->scan_for_tables(&ddl_arg);

  DBUG_RETURN(ret);
}

static ST_FIELD_INFO rdb_i_s_ddl_fields_info[] =
{
  ROCKSDB_FIELD_INFO("TABLE_SCHEMA", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("TABLE_NAME", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("PARTITION_NAME", NAME_LEN+1, MYSQL_TYPE_STRING,
                     MY_I_S_MAYBE_NULL),
  ROCKSDB_FIELD_INFO("INDEX_NAME", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("COLUMN_FAMILY", sizeof(uint32_t), MYSQL_TYPE_LONG, 0),
  ROCKSDB_FIELD_INFO("INDEX_NUMBER", sizeof(uint32_t), MYSQL_TYPE_LONG, 0),
  ROCKSDB_FIELD_INFO("INDEX_TYPE", sizeof(uint16_t), MYSQL_TYPE_SHORT, 0),
  ROCKSDB_FIELD_INFO("KV_FORMAT_VERSION", sizeof(uint16_t),
                     MYSQL_TYPE_SHORT, 0),
  ROCKSDB_FIELD_INFO("CF", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO_END
};

static int rdb_i_s_ddl_init(void *p)
{
  my_core::ST_SCHEMA_TABLE *schema;

  DBUG_ENTER("rdb_i_s_ddl_init");
  DBUG_ASSERT(p != nullptr);

  schema= (my_core::ST_SCHEMA_TABLE*) p;

  schema->fields_info= rdb_i_s_ddl_fields_info;
  schema->fill_table=  rdb_i_s_ddl_fill_table;

  DBUG_RETURN(0);
}

static int rdb_i_s_cfoptions_init(void *p)
{
  my_core::ST_SCHEMA_TABLE *schema;

  DBUG_ENTER("rdb_i_s_cfoptions_init");
  DBUG_ASSERT(p != nullptr);

  schema= (my_core::ST_SCHEMA_TABLE*) p;

  schema->fields_info= rdb_i_s_cfoptions_fields_info;
  schema->fill_table= rdb_i_s_cfoptions_fill_table;

  DBUG_RETURN(0);
}

static int rdb_i_s_global_info_init(void *p)
{
  my_core::ST_SCHEMA_TABLE *schema;

  DBUG_ENTER("rdb_i_s_global_info_init");
  DBUG_ASSERT(p != nullptr);

  schema= reinterpret_cast<my_core::ST_SCHEMA_TABLE*>(p);

  schema->fields_info= rdb_i_s_global_info_fields_info;
  schema->fill_table= rdb_i_s_global_info_fill_table;

  DBUG_RETURN(0);
}

/* Given a path to a file return just the filename portion. */
static std::string rdb_filename_without_path(
    const std::string& path)
{
  /* Find last slash in path */
  size_t pos = path.rfind('/');

  /* None found?  Just return the original string */
  if (pos == std::string::npos) {
    return std::string(path);
  }

  /* Return everything after the slash (or backslash) */
  return path.substr(pos + 1);
}

/* Fill the information_schema.rocksdb_index_file_map virtual table */
static int rdb_i_s_index_file_map_fill_table(
    my_core::THD        *thd,
    my_core::TABLE_LIST *tables,
    my_core::Item       *cond __attribute__((__unused__)))
{
  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tables != nullptr);
  DBUG_ASSERT(tables->table != nullptr);

  int     ret = 0;
  Field **field = tables->table->field;
  DBUG_ASSERT(field != nullptr);

  DBUG_ENTER("rdb_i_s_index_file_map_fill_table");

  /* Iterate over all the column families */
  rocksdb::DB *rdb= rdb_get_rocksdb_db();
  DBUG_ASSERT(rdb != nullptr);

  Rdb_cf_manager& cf_manager= rdb_get_cf_manager();
  for (auto cf_handle : cf_manager.get_all_cf()) {
    /* Grab the the properties of all the tables in the column family */
    rocksdb::TablePropertiesCollection table_props_collection;
    rocksdb::Status s = rdb->GetPropertiesOfAllTables(cf_handle,
        &table_props_collection);
    if (!s.ok()) {
      continue;
    }

    /* Iterate over all the items in the collection, each of which contains a
     * name and the actual properties */
    for (auto props : table_props_collection) {
      /* Add the SST name into the output */
      std::string sst_name = rdb_filename_without_path(props.first);
      field[2]->store(sst_name.data(), sst_name.size(), system_charset_info);

      /* Get the __indexstats__ data out of the table property */
      std::vector<Rdb_index_stats> stats;
      Rdb_tbl_prop_coll::read_stats_from_tbl_props(props.second, &stats);
      if (stats.empty()) {
        field[0]->store(-1, true);
        field[1]->store(-1, true);
        field[3]->store(-1, true);
        field[4]->store(-1, true);
        field[5]->store(-1, true);
        field[6]->store(-1, true);
        field[7]->store(-1, true);
        field[8]->store(-1, true);
      }
      else {
        for (auto it : stats) {
          /* Add the index number, the number of rows, and data size to the output */
          field[0]->store(it.m_gl_index_id.cf_id, true);
          field[1]->store(it.m_gl_index_id.index_id, true);
          field[3]->store(it.m_rows, true);
          field[4]->store(it.m_data_size, true);
          field[5]->store(it.m_entry_deletes, true);
          field[6]->store(it.m_entry_single_deletes, true);
          field[7]->store(it.m_entry_merges, true);
          field[8]->store(it.m_entry_others, true);

          /* Tell MySQL about this row in the virtual table */
          ret= my_core::schema_table_store_record(thd, tables->table);
          if (ret != 0) {
            break;
          }
        }
      }
    }
  }

  DBUG_RETURN(ret);
}

static ST_FIELD_INFO rdb_i_s_index_file_map_fields_info[] =
{
  /* The information_schema.rocksdb_index_file_map virtual table has four fields:
   *   COLUMN_FAMILY => the index's column family contained in the SST file
   *   INDEX_NUMBER => the index id contained in the SST file
   *   SST_NAME => the name of the SST file containing some indexes
   *   NUM_ROWS => the number of entries of this index id in this SST file
   *   DATA_SIZE => the data size stored in this SST file for this index id */
  ROCKSDB_FIELD_INFO("COLUMN_FAMILY", sizeof(uint32_t), MYSQL_TYPE_LONG, 0),
  ROCKSDB_FIELD_INFO("INDEX_NUMBER", sizeof(uint32_t), MYSQL_TYPE_LONG, 0),
  ROCKSDB_FIELD_INFO("SST_NAME", NAME_LEN+1, MYSQL_TYPE_STRING, 0),
  ROCKSDB_FIELD_INFO("NUM_ROWS", sizeof(int64_t), MYSQL_TYPE_LONGLONG, 0),
  ROCKSDB_FIELD_INFO("DATA_SIZE", sizeof(int64_t), MYSQL_TYPE_LONGLONG, 0),
  ROCKSDB_FIELD_INFO("ENTRY_DELETES", sizeof(int64_t), MYSQL_TYPE_LONGLONG, 0),
  ROCKSDB_FIELD_INFO("ENTRY_SINGLEDELETES", sizeof(int64_t),
                      MYSQL_TYPE_LONGLONG, 0),
  ROCKSDB_FIELD_INFO("ENTRY_MERGES", sizeof(int64_t), MYSQL_TYPE_LONGLONG, 0),
  ROCKSDB_FIELD_INFO("ENTRY_OTHERS", sizeof(int64_t), MYSQL_TYPE_LONGLONG, 0),
  ROCKSDB_FIELD_INFO_END
};

/* Initialize the information_schema.rocksdb_index_file_map virtual table */
static int rdb_i_s_index_file_map_init(void *p)
{
  my_core::ST_SCHEMA_TABLE *schema;

  DBUG_ENTER("rdb_i_s_index_file_map_init");
  DBUG_ASSERT(p != nullptr);

  schema= (my_core::ST_SCHEMA_TABLE*) p;

  schema->fields_info= rdb_i_s_index_file_map_fields_info;
  schema->fill_table= rdb_i_s_index_file_map_fill_table;

  DBUG_RETURN(0);
}

static int rdb_i_s_deinit(void *p __attribute__((__unused__)))
{
  DBUG_ENTER("rdb_i_s_deinit");
  DBUG_RETURN(0);
}

static struct st_mysql_information_schema rdb_i_s_info=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

struct st_mysql_plugin rdb_i_s_cfstats=
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &rdb_i_s_info,
  "ROCKSDB_CFSTATS",
  "Facebook",
  "RocksDB column family stats",
  PLUGIN_LICENSE_GPL,
  rdb_i_s_cfstats_init,
  rdb_i_s_deinit,
  0x0001,                             /* version number (0.1) */
  nullptr,                            /* status variables */
  nullptr,                            /* system variables */
  nullptr,                            /* config options */
  0,                                  /* flags */
};

struct st_mysql_plugin rdb_i_s_dbstats=
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &rdb_i_s_info,
  "ROCKSDB_DBSTATS",
  "Facebook",
  "RocksDB database stats",
  PLUGIN_LICENSE_GPL,
  rdb_i_s_dbstats_init,
  rdb_i_s_deinit,
  0x0001,                             /* version number (0.1) */
  nullptr,                            /* status variables */
  nullptr,                            /* system variables */
  nullptr,                            /* config options */
  0,                                  /* flags */
};

struct st_mysql_plugin rdb_i_s_perf_context=
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &rdb_i_s_info,
  "ROCKSDB_PERF_CONTEXT",
  "Facebook",
  "RocksDB perf context stats",
  PLUGIN_LICENSE_GPL,
  rdb_i_s_perf_context_init,
  rdb_i_s_deinit,
  0x0001,                             /* version number (0.1) */
  nullptr,                            /* status variables */
  nullptr,                            /* system variables */
  nullptr,                            /* config options */
  0,                                  /* flags */
};

struct st_mysql_plugin rdb_i_s_perf_context_global=
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &rdb_i_s_info,
  "ROCKSDB_PERF_CONTEXT_GLOBAL",
  "Facebook",
  "RocksDB perf context stats (all)",
  PLUGIN_LICENSE_GPL,
  rdb_i_s_perf_context_global_init,
  rdb_i_s_deinit,
  0x0001,                             /* version number (0.1) */
  nullptr,                            /* status variables */
  nullptr,                            /* system variables */
  nullptr,                            /* config options */
  0,                                  /* flags */
};

struct st_mysql_plugin rdb_i_s_cfoptions=
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &rdb_i_s_info,
  "ROCKSDB_CF_OPTIONS",
  "Facebook",
  "RocksDB column family options",
  PLUGIN_LICENSE_GPL,
  rdb_i_s_cfoptions_init,
  rdb_i_s_deinit,
  0x0001,                             /* version number (0.1) */
  nullptr,                            /* status variables */
  nullptr,                            /* system variables */
  nullptr,                            /* config options */
  0,                                  /* flags */
};

struct st_mysql_plugin rdb_i_s_global_info=
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &rdb_i_s_info,
  "ROCKSDB_GLOBAL_INFO",
  "Facebook",
  "RocksDB global info",
  PLUGIN_LICENSE_GPL,
  rdb_i_s_global_info_init,
  rdb_i_s_deinit,
  0x0001,                             /* version number (0.1) */
  nullptr,                            /* status variables */
  nullptr,                            /* system variables */
  nullptr,                            /* config options */
  0,                                  /* flags */
};

struct st_mysql_plugin rdb_i_s_ddl=
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &rdb_i_s_info,
  "ROCKSDB_DDL",
  "Facebook",
  "RocksDB Data Dictionary",
  PLUGIN_LICENSE_GPL,
  rdb_i_s_ddl_init,
  rdb_i_s_deinit,
  0x0001,                             /* version number (0.1) */
  nullptr,                            /* status variables */
  nullptr,                            /* system variables */
  nullptr,                            /* config options */
  0,                                  /* flags */
};

struct st_mysql_plugin rdb_i_s_index_file_map=
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &rdb_i_s_info,
  "ROCKSDB_INDEX_FILE_MAP",
  "Facebook",
  "RocksDB index file map",
  PLUGIN_LICENSE_GPL,
  rdb_i_s_index_file_map_init,
  rdb_i_s_deinit,
  0x0001,                             /* version number (0.1) */
  nullptr,                            /* status variables */
  nullptr,                            /* system variables */
  nullptr,                            /* config options */
  0,                                  /* flags */
};

}  // namespace myrocks
