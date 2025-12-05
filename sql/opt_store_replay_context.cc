/*
   Copyright (c) 2025, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA */

#include "opt_store_replay_context.h"
#include "sql_show.h"
#include "my_json_writer.h"
#include "sql_table.h"
#include "mysql.h"
#include "hash.h"

#include "sql_select.h"
#include "sql_explain.h"
#include "mysqld_error.h"
#include "sql_statistics.h"
#include "sql_json_lib.h"
#include "opt_histogram_json.h"

/**
  @file

  @brief
    This file provides mechanism to: -
    1. Record the range stats while the query is running
    2. Store/dump the tables/views context including index stats, range stats,
    and the cost of reading indexes, and ranges into the trace under the name
    "optimizer_context"
    3. During replay, parse the context which is in JSON format, and
    build an in memory representation of the read stats
    4. Infuse the read stats into the optimzer.

  @detail
    1. range_stats are gathered in memory using the class Range_list_recorder
    2. Stores the tables, and views context (i.e. ddls, and basic stats)
    that are used in either SELECT, INSERT, DELETE, and UPDATE queries,
    into the optimizer trace. All the contexts are stored in
    one place as a JSON array object with name "list_contexts".
    Additionally, database name is also included in the trace.
    The high level json structure looks like: -
    {
      "current_database": "db_name",
      "list_contexts": [
        {
          "name": "table_name",
          "ddl": "create table/view definition",
          "num_of_records": n,
          "read_cost_io": n,
          "read_cost_cpu": n,
          "indexes": [ //optional
            {
              ...
            }, ...,
          ],
          "list_ranges": [ //optional
            {
              ...
            }, ...
          ],
          "list_index_read_costs": [ //optional
            {
              ...
            }, ...,
          ]
        }, ...
      ]
    }
    Refer to opt_context_schema.inc file for the full schema information.
    The function "store_tables_context_in_trace()" is used to dump the
    stats into trace.
    3. Later, when this JSON structure is given as input to the variable
    "optimizer_replay_context" in the form of an user defined variable,
    it is parsed and an in-memory representation of the same structure is built
    using the class Optimizer_context_replay.
    4. To infuse the stats into the optimizer, the same class
    Optimizer_context_replay is used.
*/

/*
   One index range, aka interval.

   Currently we store the interval's text representation, like
   "1 < (key1) < 2"
*/
struct Range_record : public Sql_alloc
{
  char *range;
};

/*
   A record of one multi_range_read_info_const() call:
*/
class Multi_range_read_const_call_record : public Range_list_recorder
{
public:
  char *idx_name;
  size_t num_records;
  List<Range_record> range_list;
  Cost_estimate cost;
  ha_rows max_index_blocks;
  ha_rows max_row_blocks;
};

/*
   A record to hold one cost_for_index_read() call:
*/
class cost_index_read_call_record : public Sql_alloc
{
public:
  uint key;
  ha_rows records;
  bool eq_ref;
  ALL_READ_COST cost;
};

/*
   structure to store all the index range records,
   and the cost for reading indexes, pertaining to a table
*/
class trace_table_context : public Sql_alloc
{
public:
  /*
     full name of the table or view
     i.e db_name.[table/view]_name
  */
  char *name;
  size_t name_len;
  List<Multi_range_read_const_call_record> mrr_list;
  List<cost_index_read_call_record> irc_list;
};

static char *strdup_root(MEM_ROOT *root, String *buf);
static void store_full_table_name(const TABLE_LIST *tbl, String *buf);
static int parse_check_obj_start_in_array(json_engine_t *je, String *err_buf,
                                          const char *err_msg);
static int parse_table_context(THD *thd, json_engine_t *je, String *err_buf,
                               trace_table_context_read *table_ctx);
static int parse_index_context(THD *thd, json_engine_t *je, String *err_buf,
                               trace_index_context_read *index_ctx);
static int parse_range_context(THD *thd, json_engine_t *je, String *err_buf,
                               trace_range_context_read *range_ctx);
static int parse_index_read_cost_context(THD *thd, json_engine_t *je,
                                         String *err_buf,
                                         trace_irc_context_read *irc_ctx);
static int parse_range_cost_estimate(THD *thd, json_engine_t *je,
                                     String *err_buf, Cost_estimate *cost);

struct DDL_Key
{
  char *name; // full name of the table or view
  size_t name_len;
};

/*
  helper function to know the key portion of the record
  that is stored in hash.
*/
static const uchar *get_rec_key(const void *entry_, size_t *length,
                                my_bool flags)
{
  auto entry= static_cast<const DDL_Key *>(entry_);
  *length= entry->name_len;
  return reinterpret_cast<const uchar *>(entry->name);
}

/*
  @brief
    Check whether a table is a regular base table (for which we should
    dump the ddl) or not.

  @detail
    Besides base tables, the query may have:
     - Table functions (Currently it's only JSON_TABLE)
     - INFORMATION_SCHEMA tables
     - Tables in PERFORMANCE_SCHEMA and mysql database
     - Internal temporary ("work") tables
*/
static bool is_base_table(TABLE_LIST *tbl)
{
  return (tbl->table && tbl->table->s && !tbl->table_function &&
          !tbl->schema_table &&
          get_table_category(tbl->get_db_name(), tbl->get_table_name()) ==
              TABLE_CATEGORY_USER &&
          tbl->table->s->tmp_table != INTERNAL_TMP_TABLE &&
          tbl->table->s->tmp_table != SYSTEM_TMP_TABLE);
}

static bool dump_name_ddl_to_trace(THD *thd, DDL_Key *ddl_key, String *stmt,
                                   Json_writer_object &ctx_wrapper)
{
  ctx_wrapper.add("name", ddl_key->name);
  size_t non_esc_stmt_len= stmt->length();
  /*
    making escape_stmt size to be 4 times the non_esc_stmt
    4 is chosen as a worst case although 3 should suffice.
    "'" would be escaped to \"\'\"
  */
  size_t len_multiplier= sizeof(uint32_t);
  size_t escape_stmt_len= len_multiplier * non_esc_stmt_len;
  char *escaped_stmt= (char *) thd->alloc(escape_stmt_len + 1);

  if (!escaped_stmt)
    return true;

  int act_escape_stmt_len= json_escape_string(
      stmt->c_ptr_safe(), stmt->c_ptr_safe() + non_esc_stmt_len, escaped_stmt,
      escaped_stmt + escape_stmt_len);

  if (act_escape_stmt_len < 0)
    return true;

  escaped_stmt[act_escape_stmt_len]= 0;
  ctx_wrapper.add("ddl", escaped_stmt);
  return false;
}

static void dump_range_stats_to_trace(THD *thd, trace_table_context *context)
{
  if (!context)
    return;

  Json_writer_array list_ranges_wrapper(thd, "list_ranges");
  List_iterator irc_li(context->mrr_list);
  while (Multi_range_read_const_call_record *irc= irc_li++)
  {
    Json_writer_object irc_wrapper(thd);
    irc_wrapper.add("index_name", irc->idx_name);
    List_iterator rc_li(irc->range_list);
    Json_writer_array ranges_wrapper(thd, "ranges");
    while (Range_record *rc= rc_li++)
    {
      ranges_wrapper.add(rc->range, strlen(rc->range));
    }
    ranges_wrapper.end();

    irc_wrapper.add("num_rows", irc->num_records);
    {
      Json_writer_object cost_wrapper(thd, "cost");
      cost_wrapper.add("avg_io_cost", irc->cost.avg_io_cost);
      cost_wrapper.add("cpu_cost", irc->cost.cpu_cost);
      cost_wrapper.add("comp_cost", irc->cost.comp_cost);
      cost_wrapper.add("copy_cost", irc->cost.copy_cost);
      cost_wrapper.add("limit_cost", irc->cost.limit_cost);
      cost_wrapper.add("setup_cost", irc->cost.setup_cost);
      cost_wrapper.add("index_cost_io", irc->cost.index_cost.io);
      cost_wrapper.add("index_cost_cpu", irc->cost.index_cost.cpu);
      cost_wrapper.add("row_cost_io", irc->cost.row_cost.io);
      cost_wrapper.add("row_cost_cpu", irc->cost.row_cost.cpu);
    }

    irc_wrapper.add("max_index_blocks", irc->max_index_blocks);
    irc_wrapper.add("max_row_blocks", irc->max_row_blocks);
  }
}

static void dump_index_read_cost_to_trace(THD *thd,
                                          trace_table_context *context)
{
  if (!context)
    return;

  Json_writer_array list_irc_wrapper(thd, "list_index_read_costs");
  List_iterator irc_li(context->irc_list);

  while (cost_index_read_call_record *irc= irc_li++)
  {
    Json_writer_object irc_wrapper(thd);
    irc_wrapper.add("key_number", irc->key);
    irc_wrapper.add("num_records", irc->records);
    irc_wrapper.add("eq_ref", irc->eq_ref ? 1 : 0);
    irc_wrapper.add("index_cost_io", irc->cost.index_cost.io);
    irc_wrapper.add("index_cost_cpu", irc->cost.index_cost.cpu);
    irc_wrapper.add("row_cost_io", irc->cost.row_cost.io);
    irc_wrapper.add("row_cost_cpu", irc->cost.row_cost.cpu);
    irc_wrapper.add("max_index_blocks", irc->cost.max_index_blocks);
    irc_wrapper.add("max_row_blocks", irc->cost.max_row_blocks);
    irc_wrapper.add("copy_cost", irc->cost.copy_cost);
  }
}

static void dump_index_stats_to_trace(THD *thd, uchar *tbl_name,
                                      size_t tbl_name_len)
{
  if (!thd->opt_ctx_recorder->has_records())
    return;

  trace_table_context *table_context=
      thd->opt_ctx_recorder->search(tbl_name, tbl_name_len);

  dump_range_stats_to_trace(thd, table_context);
  dump_index_read_cost_to_trace(thd, table_context);
}

/*
  dump the following table stats to trace: -
  1. total number of records in the table
  2. if there any indexes for the table then
      their names, and the num of records per key
  3. range stats on the indexes
  4. cost of reading indexes
*/
static void dump_table_stats_to_trace(THD *thd, TABLE_LIST *tbl,
                                      uchar *tbl_name, size_t tbl_name_len,
                                      Json_writer_object &ctx_wrapper)
{
  TABLE *table= tbl->table;
  ha_rows records= table->stat_records();
  IO_AND_CPU_COST cost= table->file->ha_scan_time(records);
  ctx_wrapper.add("num_of_records", records);
  ctx_wrapper.add("read_cost_io", cost.io);
  ctx_wrapper.add("read_cost_cpu", cost.cpu);
  if (!table->key_info)
    return;

  Json_writer_array indexes_wrapper(thd, "indexes");
  for (uint idx= 0; idx < table->s->keys; idx++)
  {
    KEY key= table->key_info[idx];
    uint num_key_parts= key.user_defined_key_parts;
    Json_writer_object index_wrapper(thd);
    index_wrapper.add("index_name", key.name);
    Json_writer_array rpk_wrapper(thd, "rec_per_key");
    for (uint i= 0; i < num_key_parts; i++)
    {
      rpk_wrapper.add(key.actual_rec_per_key(i));
    }
  }
  indexes_wrapper.end();
  dump_index_stats_to_trace(thd, tbl_name, tbl_name_len);
}

static void create_view_def(THD *thd, TABLE_LIST *table, String *name,
                            String *buf)
{
  buf->append(STRING_WITH_LEN("CREATE "));
  view_store_options(thd, table, buf);
  buf->append(STRING_WITH_LEN("VIEW "));
  buf->append(*name);
  buf->append(STRING_WITH_LEN(" AS "));
  buf->append(table->select_stmt.str, table->select_stmt.length);
}

/*
  @brief
    Dump definitions, basic stats of all tables and views used by the
    statement into the optimizer trace.
    The goal is to eventually save everything that is needed to
    reproduce the query execution

  @detail
    Stores the ddls, stats of the tables, and views that are used
    in either SELECT, INSERT, DELETE, and UPDATE queries,
    into the optimizer trace.
    Global query_tables are read in reverse order from the thd->lex,
    and a record with table_name, and ddl of the table are created.
    Hash is used to store the records, where in no duplicates
    are stored. db_name.table_name is used as a key to discard any
    duplicates. If a new record that is created is not in the hash,
    then that is dumped into the trace.

  @return
    false when no error occurred during the computation
*/
bool store_tables_context_in_trace(THD *thd)
{
  LEX *lex= thd->lex;

  if (!get_opt_context_recorder(thd) ||
      lex->query_tables == *(lex->query_tables_last))
  {
    return false;
  }

  Json_writer_object main_wrapper(thd);
  Json_writer_object context(thd, "optimizer_context");
  context.add("current_database", thd->get_db());
  Json_writer_array context_list(thd, "list_contexts");
  HASH hash;
  List<TABLE_LIST> tables_list;

  /*
    lex->query_tables lists the VIEWs before their underlying tables.
    Create a list in the reverse order.
  */
  for (TABLE_LIST *tbl= lex->query_tables; tbl; tbl= tbl->next_global)
  {
    if (!tbl->is_view() && !is_base_table(tbl))
      continue;
    if (tables_list.push_front(tbl))
      return true;
  }

  if (tables_list.is_empty())
    return false;

  List_iterator li(tables_list);
  my_hash_init(key_memory_trace_ddl_info, &hash, system_charset_info, 16, 0, 0,
               get_rec_key, NULL, HASH_UNIQUE);
  bool res= false;
  for (TABLE_LIST *tbl= li++; tbl; li.remove(), tbl= li++)
  {
    String ddl;
    String name;
    DDL_Key *ddl_key;
    store_full_table_name(tbl, &name);

    /*
      A query can use the same table multiple times. Do not dump the DDL
      multiple times.
    */
    if (my_hash_search(&hash, (uchar *) name.c_ptr_safe(), name.length()))
      continue;

    if (!(ddl_key= (DDL_Key *) thd->alloc(sizeof(DDL_Key))))
    {
      res= true;
      break;
    }

    if (tbl->is_view())
      create_view_def(thd, tbl, &name, &ddl);
    else
    {
      if (show_create_table(thd, tbl, &ddl, NULL, WITH_DB_NAME))
      {
        res= true;
        break;
      }
    }

    if (!(ddl_key->name= strdup_root(thd->mem_root, &name)))
    {
      res= true; // OOM
      break;
    }

    ddl_key->name_len= strlen(ddl_key->name);

    if (my_hash_insert(&hash, (uchar *) ddl_key))
    {
      res= true; // OOM
      break;
    }

    Json_writer_object ctx_wrapper(thd);

    if (dump_name_ddl_to_trace(thd, ddl_key, &ddl, ctx_wrapper))
    {
      res= true;
      break;
    }

    if (!tbl->is_view())
    {
      dump_table_stats_to_trace(thd, tbl, (uchar *) ddl_key->name,
                                ddl_key->name_len, ctx_wrapper);
    }
  }
  my_hash_free(&hash);

  return res;
}

/*
  Create a new table context if it is not already present in the
  hash.
  The table context is also persisted in the hash which is to be
  used later for dumping all the context infomation into the trace.
*/
trace_table_context *
Optimizer_context_recorder::get_table_context(MEM_ROOT *mem_root,
                                              const TABLE_LIST *tbl)
{
  String tbl_name;
  store_full_table_name(tbl, &tbl_name);
  trace_table_context *table_ctx=
      search((uchar *) tbl_name.c_ptr_safe(), tbl_name.length());

  if (!table_ctx)
  {
    if (!(table_ctx= new (mem_root) trace_table_context))
      return nullptr; // OOM

    if (!(table_ctx->name= strdup_root(mem_root, &tbl_name)))
      return nullptr; // OOM

    table_ctx->name_len= tbl_name.length();

    if (my_hash_insert(&tbl_trace_ctx_hash, (uchar *) table_ctx))
      return nullptr; // OOM
  }

  return table_ctx;
}

Optimizer_context_recorder::Optimizer_context_recorder()
{
  my_hash_init(key_memory_trace_ddl_info, &tbl_trace_ctx_hash,
               system_charset_info, 16, 0, 0,
               &Optimizer_context_recorder::get_tbl_trace_ctx_key, 0,
               HASH_UNIQUE);
}

Optimizer_context_recorder::~Optimizer_context_recorder()
{
  my_hash_free(&tbl_trace_ctx_hash);
}

bool Optimizer_context_recorder::has_records()
{
  return tbl_trace_ctx_hash.records > 0;
}

trace_table_context *Optimizer_context_recorder::search(uchar *tbl_name,
                                                        size_t tbl_name_len)
{
  return (trace_table_context *) my_hash_search(&tbl_trace_ctx_hash, tbl_name,
                                                tbl_name_len);
}

/*
  @detail
    Do not use thd->mem_root, allocate memory only on the passed mem_root.
*/
void Range_list_recorder::add_range(MEM_ROOT *mem_root, const char *range)
{
  Range_record *record= new (mem_root) Range_record;

  if (unlikely(!record))
    return; // OOM

  if (!(record->range= strdup_root(mem_root, range)))
    return; // OOM

  ((Multi_range_read_const_call_record *) this)
      ->range_list.push_back(record, mem_root);
}

/*
  @brief
    Start recording a range list for tbl.index_name

  @return
    Pointer one can use to add ranges.
*/
Range_list_recorder *Optimizer_context_recorder::start_range_list_record(
    MEM_ROOT *mem_root, TABLE_LIST *tbl, size_t found_records,
    const char *index_name, const Cost_estimate *cost,
    ha_rows max_index_blocks, ha_rows max_row_blocks)
{
  Multi_range_read_const_call_record *range_ctx=
      new (mem_root) Multi_range_read_const_call_record;

  if (unlikely(!range_ctx))
    return nullptr; // OOM

  if (!(range_ctx->idx_name= strdup_root(mem_root, index_name)))
    return nullptr; // OOM

  range_ctx->num_records= found_records;
  {
    range_ctx->cost= *cost;
    range_ctx->max_index_blocks= max_index_blocks;
    range_ctx->max_row_blocks= max_row_blocks;
  }

  /*
    Store the ranges of every index of the table into the
    table context.
  */
  trace_table_context *table_ctx= get_table_context(mem_root, tbl);

  if (unlikely(!table_ctx))
    return nullptr; // OOM

  if (table_ctx->mrr_list.push_back(range_ctx, mem_root))
    return nullptr;

  return range_ctx;
}

/*
  record cost of reading an index, and add it to the index read cost list
  of the table context.
*/
void Optimizer_context_recorder::record_cost_index_read(
    MEM_ROOT *mem_root, const TABLE_LIST *tbl, uint key, ha_rows records,
    bool eq_ref, const ALL_READ_COST *cost)
{
  cost_index_read_call_record *idx_read_rec=
      new (mem_root) cost_index_read_call_record;

  if (unlikely(!idx_read_rec))
    return; // OOM

  idx_read_rec->key= key;
  idx_read_rec->records= records;
  idx_read_rec->eq_ref= eq_ref;
  idx_read_rec->cost= *cost;

  trace_table_context *table_ctx= get_table_context(mem_root, tbl);

  if (unlikely(!table_ctx))
    return; // OOM

  table_ctx->irc_list.push_back(idx_read_rec, mem_root);
}

/*
  helper function to know the key portion of the
  trace table context that is stored in hash.
*/
const uchar *Optimizer_context_recorder::get_tbl_trace_ctx_key(
    const void *entry_, size_t *length, my_bool flags)
{
  auto entry= static_cast<const trace_table_context *>(entry_);
  *length= entry->name_len;
  return reinterpret_cast<const uchar *>(entry->name);
}

static char *strdup_root(MEM_ROOT *root, String *buf)
{
  return strdup_root(root, buf->c_ptr_safe());
}

/*
  store full table name i.e. "db_name.table_name",
  into the supplied variable buf
*/
static void store_full_table_name(const TABLE_LIST *tbl, String *buf)
{
  buf->append(tbl->get_db_name().str, tbl->get_db_name().length);
  buf->append(STRING_WITH_LEN("."));
  buf->append(tbl->get_table_name().str, tbl->get_table_name().length);
}

Optimizer_context_recorder *get_opt_context_recorder(THD *thd)
{
  if (!thd->variables.optimizer_trace ||
      !thd->variables.optimizer_record_context)
    return nullptr;

  if (thd->opt_ctx_recorder)
    return thd->opt_ctx_recorder;

  LEX *lex= thd->lex;
  if (lex->sql_command == SQLCOM_SELECT ||
      lex->sql_command == SQLCOM_INSERT_SELECT ||
      lex->sql_command == SQLCOM_DELETE || lex->sql_command == SQLCOM_UPDATE ||
      lex->sql_command == SQLCOM_DELETE_MULTI ||
      lex->sql_command == SQLCOM_UPDATE_MULTI)
  {
    thd->opt_ctx_recorder= new Optimizer_context_recorder();
    return thd->opt_ctx_recorder;
  }

  return nullptr;
}

Range_list_recorder *
get_range_list_recorder(THD *thd, MEM_ROOT *mem_root, TABLE_LIST *tbl,
                        const char *index_name, ha_rows records,
                        Cost_estimate *cost, ha_rows max_index_blocks,
                        ha_rows max_row_blocks)
{
  Optimizer_context_recorder *ctx= get_opt_context_recorder(thd);

  if (ctx && !thd->lex->explain->is_query_plan_ready())
  {
    return ctx->start_range_list_record(mem_root, tbl, records, index_name,
                                        cost, max_index_blocks,
                                        max_row_blocks);
  }

  return nullptr;
}

/*
  This class is used to store the in-memory representation of
  one range context i.e. read from json
*/
class trace_range_context_read : public Sql_alloc
{
public:
  char *index_name;
  List<char> ranges;
  ha_rows num_rows;
  Cost_estimate cost;
  ha_rows max_index_blocks;
  ha_rows max_row_blocks;
};

/*
  This class is used to store the in-memory representation of
  one index context i.e. read from json
*/
class trace_index_context_read : public Sql_alloc
{
public:
  char *idx_name;
  List<ha_rows> list_rec_per_key;
};

/*
  This class is used to store the in-memory representation of
  one index read cost i.e. read from json
*/
class trace_irc_context_read : public Sql_alloc
{
public:
  uint key;
  ha_rows records;
  bool eq_ref;
  ALL_READ_COST cost;
};

/*
  This class is used to store the in-memory representation of
  a table context i.e. read from json.
  A list of index contexts, and range contexts are stored separately.
*/
class trace_table_context_read : public Sql_alloc
{
public:
  /*
     full name of the table or view
     i.e db_name.[table/view]_name
  */
  char *name;
  char *ddl;
  ha_rows total_rows;
  double read_cost_io;
  double read_cost_cpu;
  List<trace_index_context_read> index_list;
  List<trace_range_context_read> ranges_list;
  List<trace_irc_context_read> irc_list;
};

/*
  This class structure is used to temporarily store the old index stats
  that are in the optimizer, before they are updated by the stats
  from json trace.
  They are restored once the query that used json trace stats is done
  execution.
*/
class Saved_Index_stats : public Sql_alloc
{
public:
  KEY *key_info;
  bool original_is_statistics_from_stat_tables;
  Index_statistics *original_read_stats;
};

/*
  This class structure is used to temporarily store the old table stats
  that are in the optimizer, before they are updated by the stats
  from json trace.
  They are restored once the query that used json trace stats is done
  execution.
*/
class Saved_Table_stats : public Sql_alloc
{
public:
  TABLE *table;
  ha_rows original_rows;
  List<Saved_Index_stats> saved_indexstats_list;
};

/*
  Extends the Read_value interface to read a container of values,
  for eg: array of numbers or strings, an object with several fields, etc...
*/
class Read_container_value : public Read_value
{
private:
  int before_read(json_engine_t *je, const char *value_name, String *err_buf)
  {
    if (json_scan_next(je) || je->state != JST_ARRAY_START)
    {
      err_buf->append(STRING_WITH_LEN("error reading "));
      err_buf->append(value_name, strlen(value_name));
      err_buf->append(STRING_WITH_LEN(" value"));
      return 1;
    }
    return 0;
  }

  int after_read(int rc) { return rc > 0; }

public:
  int read_value(json_engine_t *je, const char *value_name,
                 String *err_buf) override
  {
    int rc= before_read(je, value_name, err_buf);
    if (rc <= 0)
    {
      rc= read_container(je, err_buf);
    }
    return after_read(rc);
  }
  virtual int read_container(json_engine_t *je, String *err_buf)= 0;
};

class Read_range_cost_estimate : public Read_value
{
  THD *thd;
  Cost_estimate *ptr;

public:
  Read_range_cost_estimate(THD *thd_arg, Cost_estimate *ptr_arg)
      : thd(thd_arg), ptr(ptr_arg)
  {
  }
  int read_value(json_engine_t *je, const char *value_name,
                 String *err_buf) override
  {
    return parse_range_cost_estimate(thd, je, err_buf, ptr);
  }
};

class Read_list_of_ha_rows : public Read_container_value
{
  THD *thd;
  List<ha_rows> *list_values;

public:
  Read_list_of_ha_rows(THD *thd_arg, List<ha_rows> *list_values_arg)
      : thd(thd_arg), list_values(list_values_arg)
  {
  }
  int read_container(json_engine_t *je, String *err_buf) override
  {
    while (je->state != JST_ARRAY_END)
    {
      ha_rows temp_value;
      if (read_ha_rows_and_check_limit(je, "rec_per_key", err_buf, temp_value,
                                       ULONGLONG_MAX, "unsigned longlong",
                                       true))
      {
        return 1;
      }

      ha_rows *records_ptr= (ha_rows *) thd->alloc(sizeof(ha_rows));

      if (unlikely(!records_ptr))
        return 1; // OOM

      *records_ptr= temp_value;

      if (list_values->push_back(records_ptr) || json_scan_next(je))
        return 1;
    }
    return 0;
  }
};

template <typename T> class Read_list_of_context : public Read_container_value
{
  THD *thd;
  List<T> *list_ctx;
  int (*parse_context_fn)(THD *, json_engine_t *, String *, T *);

public:
  Read_list_of_context(THD *thd_arg, List<T> *list_ctx_arg,
                       int (*parse_context_fn_arg)(THD *, json_engine_t *,
                                                   String *, T *))
      : thd(thd_arg), list_ctx(list_ctx_arg),
        parse_context_fn(parse_context_fn_arg)
  {
  }
  int read_container(json_engine_t *je, String *err_buf) override
  {
    int rc;

    while (1)
    {
      T *ctx= new T();

      if (unlikely(!ctx))
        return 1; // OOM

      rc= parse_context_fn(thd, je, err_buf, ctx);

      if (rc == 0)
      {
        if (list_ctx->push_back(ctx))
          return 1; // OOM
      }
      else
        break;
    }

    return rc;
  }
};

class Read_list_of_ranges : public Read_container_value
{
  THD *thd;
  List<char> *list_ranges;

public:
  Read_list_of_ranges(THD *thd_arg, List<char> *list_ranges_arg)
      : thd(thd_arg), list_ranges(list_ranges_arg)
  {
  }
  int read_container(json_engine_t *je, String *err_buf) override
  {
    if (json_scan_next(je))
      return 1;

    while (je->state != JST_ARRAY_END)
    {
      char *value;
      if (read_string(thd, je, "ranges", err_buf, value))
        return 1;

      list_ranges->push_back(value);
      if (json_scan_next(je))
        return 1;
    }

    return 0;
  }
};

/*
  check if the next element being parsed is an object within an array.
  fill the err_buf with err_msg if the parsing check fails.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_check_obj_start_in_array(json_engine_t *je, String *err_buf,
                                          const char *err_msg)
{
  if (json_scan_next(je))
    return 1;

  if (je->state != JST_VALUE)
  {
    if (je->state == JST_ARRAY_END)
      return -1; // EOF
    else
      return 1; // An error
  }

  if (json_scan_next(je) || je->state != JST_OBJ_START)
  {
    err_buf->append(err_msg, strlen(err_msg));
    return 1;
  }

  return 0;
}

/*
  parse a single context object from a json array of contexts.
  The conext object should contain the elements that are
  defined in the argument array

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_context_obj_from_json_array(json_engine_t *je,
                                             String *err_buf,
                                             const char *err_msg,
                                             Read_named_member *array)
{
  if (int rc= parse_check_obj_start_in_array(je, err_buf, err_msg))
    return rc;

  return read_all_elements(je, array, err_buf);
}

/*
  Parses the table context of the JSON structure
  of the optimizer context.
  A single array element of list_contexts is parsed
  in this method.
  Refer to the file opt_context_schema.inc, and
  the description at the start of this file.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_table_context(THD *thd, json_engine_t *je, String *err_buf,
                               trace_table_context_read *table_ctx)
{
  const char *err_msg= "Expected an object in the list_contexts array";

  Read_named_member array[]= {
      {"name", Read_string(thd, &table_ctx->name), false},
      {"ddl", Read_string(thd, &table_ctx->ddl), false},
      {"num_of_records",
       Read_non_neg_integer<ha_rows, ULONGLONG_MAX>(&table_ctx->total_rows),
       false},
      {"read_cost_io", Read_double(&table_ctx->read_cost_io), false},
      {"read_cost_cpu", Read_double(&table_ctx->read_cost_cpu), false},
      {"indexes",
       Read_list_of_context<trace_index_context_read>(
           thd, &table_ctx->index_list, parse_index_context),
       true},
      {"list_ranges",
       Read_list_of_context<trace_range_context_read>(
           thd, &table_ctx->ranges_list, parse_range_context),
       true},
      {"list_index_read_costs",
       Read_list_of_context<trace_irc_context_read>(
           thd, &table_ctx->irc_list, parse_index_read_cost_context),
       true},
      {NULL, Read_double(NULL), true}};

  return parse_context_obj_from_json_array(je, err_buf, err_msg, array);
}

/*
  Parses the index context of the JSON structure
  of the optimizer context.
  To be specific, single array element of indexes
  is parsed in this method.
  Refer to the file opt_context_schema.inc, and
  the description at the start of this file.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_index_context(THD *thd, json_engine_t *je, String *err_buf,
                               trace_index_context_read *index_ctx)
{
  const char *err_msg= "Expected an object in the indexes array";

  Read_named_member array[]= {
      {"index_name", Read_string(thd, &index_ctx->idx_name), false},
      {"rec_per_key", Read_list_of_ha_rows(thd, &index_ctx->list_rec_per_key),
       false},
      {NULL, Read_double(NULL), true}};

  return parse_context_obj_from_json_array(je, err_buf, err_msg, array);
}

/*
  Parses the range context of the JSON structure
  of the optimizer context.
  To be specific, a single array element of list_ranges
  is parsed in this method.
  Refer to the file opt_context_schema.inc, and
  the description at the start of this file.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_range_context(THD *thd, json_engine_t *je, String *err_buf,
                               trace_range_context_read *range_ctx)
{
  const char *err_msg= "Expected an object in the list_ranges array";

  Read_named_member array[]= {
      {"index_name", Read_string(thd, &range_ctx->index_name), false},
      {"ranges", Read_list_of_ranges(thd, &range_ctx->ranges), false},
      {"num_rows",
       Read_non_neg_integer<ha_rows, ULONGLONG_MAX>(&range_ctx->num_rows),
       false},
      {"cost", Read_range_cost_estimate(thd, &range_ctx->cost), false},
      {"max_index_blocks",
       Read_non_neg_integer<ha_rows, ULONGLONG_MAX>(
           &range_ctx->max_index_blocks),
       false},
      {"max_row_blocks",
       Read_non_neg_integer<ha_rows, ULONGLONG_MAX>(
           &range_ctx->max_row_blocks),
       false},
      {NULL, Read_double(NULL), true}};

  return parse_context_obj_from_json_array(je, err_buf, err_msg, array);
}

/*
  Parses the cost information present in the
  range context of the JSON structure.
  Refer to the file opt_context_schema.inc, and
  the description at the start of this file.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_range_cost_estimate(THD *thd, json_engine_t *je,
                                     String *err_buf, Cost_estimate *cost)
{
  if (json_scan_next(je) || je->state != JST_OBJ_START)
  {
    err_buf->append(
        STRING_WITH_LEN("Expected an object while reading range cost"));
    return 1;
  }

  Read_named_member array[]= {
      {"avg_io_cost", Read_double(&cost->avg_io_cost), false},
      {"cpu_cost", Read_double(&cost->cpu_cost), false},
      {"comp_cost", Read_double(&cost->comp_cost), false},
      {"copy_cost", Read_double(&cost->copy_cost), false},
      {"limit_cost", Read_double(&cost->limit_cost), false},
      {"setup_cost", Read_double(&cost->setup_cost), false},
      {"index_cost_io", Read_double(&cost->index_cost.io), false},
      {"index_cost_cpu", Read_double(&cost->index_cost.cpu), false},
      {"row_cost_io", Read_double(&cost->row_cost.io), false},
      {"row_cost_cpu", Read_double(&cost->row_cost.cpu), false},
      {NULL, Read_double(NULL), true}};

  return read_all_elements(je, array, err_buf);
}

/*
  Parses the cost information for reading an index using
  ref access of the JSON structure of the optimizer context.
  To be specific, single array element of list_index_read_costs
  is parsed in this method.
  Refer to the file opt_context_schema.inc, and
  the description at the start of this file.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_index_read_cost_context(THD *thd, json_engine_t *je,
                                         String *err_buf,
                                         trace_irc_context_read *irc_ctx)
{
  const char *err_msg= "Expected an object in the index_read_costs array";

  Read_named_member array[]= {
      {"key_number", Read_non_neg_integer<uint, UINT_MAX>(&irc_ctx->key),
       false},
      {"num_records",
       Read_non_neg_integer<ha_rows, ULONGLONG_MAX>(&irc_ctx->records), false},
      {"eq_ref", Read_non_neg_integer<bool, 1>(&irc_ctx->eq_ref), false},
      {"index_cost_io", Read_double(&irc_ctx->cost.index_cost.io), false},
      {"index_cost_cpu", Read_double(&irc_ctx->cost.index_cost.cpu), false},
      {"row_cost_io", Read_double(&irc_ctx->cost.row_cost.io), false},
      {"row_cost_cpu", Read_double(&irc_ctx->cost.row_cost.cpu), false},
      {"max_index_blocks",
       Read_non_neg_integer<longlong, LONGLONG_MAX>(
           &irc_ctx->cost.max_index_blocks),
       false},
      {"max_row_blocks",
       Read_non_neg_integer<longlong, LONGLONG_MAX>(
           &irc_ctx->cost.max_row_blocks),
       false},
      {"copy_cost", Read_double(&irc_ctx->cost.copy_cost), false},
      {NULL, Read_double(NULL), true}};

  return parse_context_obj_from_json_array(je, err_buf, err_msg, array);
}

Optimizer_context_replay::Optimizer_context_replay(THD *thd)
{
  this->thd= thd;
  this->db_name= NULL;
  this->parse();
}

/*
  search the in memory representation of the parsed contents
  of json trace context, and set read_cost for the given table.

  @return
    false  OK
    true  Error
*/
bool Optimizer_context_replay::infuse_read_cost(const TABLE *tbl,
                                                IO_AND_CPU_COST *cost)
{
  if (!has_records() || !is_base_table(tbl->pos_in_table_list))
    return true;

  String tbl_name;
  store_full_table_name(tbl->pos_in_table_list, &tbl_name);
  List_iterator<trace_table_context_read> table_itr(this->ctx_list);

  while (trace_table_context_read *tbl_ctx= table_itr++)
  {
    if (strcmp(tbl_name.c_ptr_safe(), tbl_ctx->name) == 0)
    {
      cost->io= tbl_ctx->read_cost_io;
      cost->cpu= tbl_ctx->read_cost_cpu;
      return false;
    }
  }

  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED),
      tbl_name.c_ptr_safe(), "list of table contexts");
  return true;
}

/*
  search the list of range stats from the in memory representation of the
  parsed json trace context, for the given table_name, and index_name.
  If they are found, then compare the ranges one by one until all of them
  match. If so, load the num_records, and the computation cost associated
  with it into the arguments passed.

  @return
    false  OK
    true  Error
*/
bool Optimizer_context_replay::infuse_range_stats(
    TABLE *table, uint keynr, RANGE_SEQ_IF *seq_if, SEL_ARG_RANGE_SEQ *seq,
    Cost_estimate *cost, ha_rows *rows, ha_rows *max_index_blocks,
    ha_rows *max_row_blocks)
{
  if (!has_records() || !is_base_table(table->pos_in_table_list))
    return true;

  KEY *keyinfo= table->key_info + keynr;
  const char *idx_name= keyinfo->name.str;
  const KEY_PART_INFO *key_part= keyinfo->key_part;
  uint n_key_parts= table->actual_n_key_parts(keyinfo);
  KEY_MULTI_RANGE multi_range;
  range_seq_t seq_it;
  List<trace_range_context_read> range_ctx_list;
  store_range_contexts(table, idx_name, &range_ctx_list);
  String act_ranges;
  seq_it= seq_if->init((void *) seq, 0, 0);
  act_ranges.append(STRING_WITH_LEN("["));

  while (!seq_if->next(seq_it, &multi_range))
  {
    StringBuffer<128> range_info(system_charset_info);
    print_range(&range_info, key_part, &multi_range, n_key_parts);
    char *r1= range_info.c_ptr_safe();
    act_ranges.append(r1, strlen(r1));
    act_ranges.append(STRING_WITH_LEN(", "));
  }

  act_ranges.append(STRING_WITH_LEN("]"));

  if (!range_ctx_list.is_empty())
  {
    List_iterator<trace_range_context_read> range_ctx_itr(range_ctx_list);
    while (trace_range_context_read *range_ctx= range_ctx_itr++)
    {
      List_iterator<char> range_itr(range_ctx->ranges);
      seq_it= seq_if->init((void *) seq, 0, 0);
      bool matched= true;

      while (!seq_if->next(seq_it, &multi_range))
      {
        StringBuffer<128> range_info(system_charset_info);
        print_range(&range_info, key_part, &multi_range, n_key_parts);
        char *r1= range_info.c_ptr_safe();
        char *r2= range_itr++;
        if (r2 == NULL || strcmp(r1, r2) != 0)
        {
          matched= false;
          break;
        }
      }
      if (matched)
      {
        *cost= range_ctx->cost;
        *rows= range_ctx->num_rows;
        *max_index_blocks= range_ctx->max_index_blocks;
        *max_row_blocks= range_ctx->max_row_blocks;
        return false;
      }
    }

    String arg1;
    String arg2;
    String tbl_name;
    store_full_table_name(table->pos_in_table_list, &tbl_name);
    arg1.append(STRING_WITH_LEN("the given list of ranges i.e. "));
    arg1.append(act_ranges);
    arg2.append(STRING_WITH_LEN("the list of ranges for table_name "));
    arg2.append(tbl_name);
    arg2.append(STRING_WITH_LEN(" and index_name "));
    arg2.append(idx_name, strlen(idx_name));
    push_warning_printf(
        thd, Sql_condition::WARN_LEVEL_WARN,
        ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED,
        ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED),
        arg1.c_ptr_safe(), arg2.c_ptr_safe());
  }
  return true;
}

/*
  search the index read cost info from the in memory representation of the
  parsed json trace context, for the given table, keynr, records, and eq_ref,
  and set it into the cost if found.

  @return
    false  OK
    true  Error
*/
bool Optimizer_context_replay::infuse_index_read_cost(const TABLE *tbl,
                                                      uint keynr,
                                                      ha_rows records,
                                                      bool eq_ref,
                                                      ALL_READ_COST *cost)
{
  if (!has_records() || !is_base_table(tbl->pos_in_table_list))
    return true;

  String tbl_name;
  store_full_table_name(tbl->pos_in_table_list, &tbl_name);
  List_iterator<trace_table_context_read> table_itr(this->ctx_list);

  while (trace_table_context_read *tbl_ctx= table_itr++)
  {
    if (strcmp(tbl_name.c_ptr_safe(), tbl_ctx->name) == 0)
    {
      List_iterator<trace_irc_context_read> irc_itr(tbl_ctx->irc_list);
      while (trace_irc_context_read *irc_ctx= irc_itr++)
      {
        if (irc_ctx->key == keynr && irc_ctx->records == records &&
            irc_ctx->eq_ref == eq_ref)
        {
          *cost= irc_ctx->cost;
          return false;
        }
      }
    }
  }

  String warn_msg;
  warn_msg.append(tbl_name);
  warn_msg.append(STRING_WITH_LEN(" with key_number:"));
  warn_msg.append(keynr);
  warn_msg.append(STRING_WITH_LEN(", records:"));
  warn_msg.q_append_int64(records);
  warn_msg.append(STRING_WITH_LEN(", eq_ref:"));
  warn_msg.append(eq_ref);
  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED),
      warn_msg.c_ptr_safe(), "list_index_read_costs");
  return true;
}

/*
  @brief
    Save the current stats of the table and its associated table.
*/
void Optimizer_context_replay::infuse_table_stats(TABLE *table)
{
  if (!has_records() || !is_base_table(table->pos_in_table_list))
    return;

  Saved_Table_stats *saved_ts= new Saved_Table_stats();

  if (unlikely(!saved_ts))
    return; // OOM

  saved_ts->table= table;
  saved_ts->original_rows= table->used_stat_records;

  if (saved_tablestats_list.push_back(saved_ts))
    return;

  ha_rows temp_rows;
  if (!infuse_table_rows(table, &temp_rows))
  {
    table->used_stat_records= temp_rows;

    KEY *key_info, *key_info_end;
    for (key_info= table->key_info, key_info_end= key_info + table->s->keys;
         key_info < key_info_end; key_info++)
    {
      List<ha_rows> *index_freq_list=
          get_index_rec_per_key_list(table, key_info->name.str);

      if (index_freq_list && !index_freq_list->is_empty())
      {
        Saved_Index_stats *saved_is= new Saved_Index_stats();

        if (unlikely(!saved_is))
          return; // OOM

        uint i= 0;
        uint num_key_parts= key_info->user_defined_key_parts;
        Index_statistics *original_read_stats= key_info->read_stats;
        bool original_is_statistics_from_stat_tables=
            key_info->is_statistics_from_stat_tables;
        Index_statistics *new_read_stats= new Index_statistics();

        if (unlikely(!new_read_stats))
          return; // OOM

        ulonglong *frequencies=
            (ulonglong *) thd->alloc(sizeof(ulonglong) * num_key_parts);

        if (unlikely(!frequencies))
          return; // OOM

        new_read_stats->init_avg_frequency(frequencies);
        List_iterator li(*index_freq_list);
        ha_rows *freq= li++;
        key_info->read_stats= new_read_stats;

        while (freq && i < num_key_parts)
        {
          DBUG_ASSERT(*freq > 0);
          key_info->read_stats->set_avg_frequency(i, (double) *freq);
          freq= li++;
          i++;
        }

        key_info->is_statistics_from_stat_tables= true;
        saved_is->key_info= key_info;
        saved_is->original_is_statistics_from_stat_tables=
            original_is_statistics_from_stat_tables;
        saved_is->original_read_stats= original_read_stats;
        saved_ts->saved_indexstats_list.push_back(saved_is);
      }
    }
  }
}

/*
  @brief
    restore the saved stats for the tables, and indexes that were
    earlier recorded using set_table_stats_from_context()
*/
void Optimizer_context_replay::restore_modified_table_stats()
{
  List_iterator<Saved_Table_stats> table_li(saved_tablestats_list);
  while (Saved_Table_stats *saved_ts= table_li++)
  {
    saved_ts->table->used_stat_records= saved_ts->original_rows;
    List_iterator<Saved_Index_stats> index_li(saved_ts->saved_indexstats_list);
    while (Saved_Index_stats *saved_is= index_li++)
    {
      saved_is->key_info->is_statistics_from_stat_tables=
          saved_is->original_is_statistics_from_stat_tables;
      saved_is->key_info->read_stats= saved_is->original_read_stats;
    }
  }
}

/*
  Returns if the in memory representation of the
  parsed json trace context contain any records
*/
bool Optimizer_context_replay::has_records()
{
  return db_name != NULL && !ctx_list.is_empty();
}

/*
  parse the trace context that abides to the structure defined in
  opt_context_schema.inc

  @return
    FALSE  OK
    TRUE  Parse Error
*/
bool Optimizer_context_replay::parse()
{
  json_engine_t je;
  char *context= NULL;
  String str;
  String *value;
  String err_buf;
  user_var_entry *var;
  char *var_name= thd->variables.optimizer_replay_context;
  LEX_CSTRING varname= {var_name, strlen(var_name)};

  Read_named_member array[]= {
      {"current_database", Read_string(this->thd, &this->db_name), false},
      {"list_contexts",
       Read_list_of_context<trace_table_context_read>(
           this->thd, &this->ctx_list, parse_table_context),
       false},
      {NULL, Read_double(NULL), true}};

  DBUG_ENTER("Optimizer_context_replay::parse");

  if ((var= get_variable(&thd->user_vars, &varname, FALSE)))
  {
    bool null_value;
    value= var->val_str(&null_value, &str, 0);
    if (null_value || !value->length())
      goto err;
  }
  else
  {
    goto err;
  }

  context= value->c_ptr_safe();
  mem_root_dynamic_array_init(thd->mem_root, PSI_INSTRUMENT_MEM, &je.stack,
                              sizeof(int), NULL, JSON_DEPTH_DEFAULT,
                              JSON_DEPTH_INC, MYF(0));

  json_scan_start(&je, system_charset_info, (const uchar *) context,
                  (const uchar *) context + strlen(context));

  if (json_scan_next(&je))
  {
    err_buf.append(STRING_WITH_LEN("JSON parse error"));
    goto err;
  }

  if (je.state != JST_OBJ_START)
  {
    err_buf.append(STRING_WITH_LEN("Root JSON element must be a JSON object"));
    goto err;
  }

  if (read_all_elements(&je, array, &err_buf))
    goto err;

#ifndef DBUG_OFF
  dbug_print_read_stats();
#endif
  DBUG_RETURN(false); // Ok
err:
  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_REPLAY_CONTEXT_PARSE_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_PARSE_FAILED),
      err_buf.c_ptr_safe(), (je.s.c_str - (const uchar *) context));
  DBUG_RETURN(true);
}

#ifndef DBUG_OFF
/*
  Print the contents of the stats that are read from the json trace
*/
void Optimizer_context_replay::dbug_print_read_stats()
{
  DBUG_ENTER("Optimizer_context_replay::print()");
  DBUG_PRINT("info", ("----------Printing Stored Context-------------"));
  DBUG_PRINT("info", ("current_database : %s", this->db_name));
  List_iterator<trace_table_context_read> table_itr(this->ctx_list);

  while (trace_table_context_read *tbl_ctx= table_itr++)
  {
    DBUG_PRINT("info", ("New Table Context"));
    DBUG_PRINT("info", ("-----------------"));
    DBUG_PRINT("info", ("name: %s", tbl_ctx->name));
    DBUG_PRINT("info", ("ddl: %s", tbl_ctx->ddl));
    DBUG_PRINT("info", ("num_of_records: %llx", tbl_ctx->total_rows));

    List_iterator<trace_index_context_read> index_itr(tbl_ctx->index_list);

    while (trace_index_context_read *idx_ctx= index_itr++)
    {
      DBUG_PRINT("info", ("...........New Index Context........."));
      DBUG_PRINT("info", ("index_name: %s", idx_ctx->idx_name));
      DBUG_PRINT("info", ("list_rec_per_key: [ "));
      List_iterator<ha_rows> rec_itr(idx_ctx->list_rec_per_key);
      while (true)
      {
        ha_rows *num_rec= rec_itr++;
        if (!num_rec)
          break;
        DBUG_PRINT("info", ("%llx, ", *num_rec));
      }
      DBUG_PRINT("info", ("]"));
    }

    List_iterator<trace_range_context_read> range_itr(tbl_ctx->ranges_list);

    while (trace_range_context_read *range_ctx= range_itr++)
    {
      DBUG_PRINT("info", ("...........New Range Context........."));
      DBUG_PRINT("info", ("index_name: %s", range_ctx->index_name));
      DBUG_PRINT("info", ("ranges: [ "));

      List_iterator<char> range_itr(range_ctx->ranges);

      while (true)
      {
        char *range= range_itr++;
        if (!range)
          break;
        DBUG_PRINT("info", ("%s, ", range));
      }

      DBUG_PRINT("info", ("]"));
      DBUG_PRINT("info", ("num_rows: %llx", range_ctx->num_rows));
      {
        DBUG_PRINT("info", ("avg_io_cost: %f", range_ctx->cost.avg_io_cost));
        DBUG_PRINT("info", ("cpu_cost: %f", range_ctx->cost.cpu_cost));
        DBUG_PRINT("info", ("comp_cost: %f", range_ctx->cost.comp_cost));
        DBUG_PRINT("info", ("copy_cost: %f", range_ctx->cost.copy_cost));
        DBUG_PRINT("info", ("limit_cost: %f", range_ctx->cost.limit_cost));
        DBUG_PRINT("info", ("setup_cost: %f", range_ctx->cost.setup_cost));
        DBUG_PRINT("info",
                   ("index_cost.io: %f", range_ctx->cost.index_cost.io));
        DBUG_PRINT("info",
                   ("index_cost.cpu: %f", range_ctx->cost.index_cost.cpu));
        DBUG_PRINT("info", ("row_cost.io: %f", range_ctx->cost.row_cost.io));
        DBUG_PRINT("info", ("row_cost.cpu: %f", range_ctx->cost.row_cost.cpu));
      }
      DBUG_PRINT("info",
                 ("max_index_blocks: %llx", range_ctx->max_index_blocks));
      DBUG_PRINT("info", ("max_row_blocks: %llx", range_ctx->max_row_blocks));
    }

    List_iterator<trace_irc_context_read> irc_itr(tbl_ctx->irc_list);

    for (trace_irc_context_read *irc= irc_itr++; irc; irc= irc_itr++)
    {
      DBUG_PRINT("info", ("...........New Index Read Cost Context........."));
      DBUG_PRINT("info", ("key_number: %u", irc->key));
      DBUG_PRINT("info", ("num_records: %llx", irc->records));
      DBUG_PRINT("info", ("eq_ref: %d", irc->eq_ref));
      {
        DBUG_PRINT("info", ("index_cost_io: %f", irc->cost.index_cost.io));
        DBUG_PRINT("info", ("index_cost_cpu: %f", irc->cost.index_cost.cpu));
        DBUG_PRINT("info", ("row_cost_io: %f", irc->cost.row_cost.io));
        DBUG_PRINT("info", ("row_cost_cpu: %f", irc->cost.row_cost.cpu));
        DBUG_PRINT("info",
                   ("max_index_blocks: %llx", irc->cost.max_index_blocks));
        DBUG_PRINT("info", ("max_row_blocks: %llx", irc->cost.max_row_blocks));
        DBUG_PRINT("info", ("copy_cost: %f", irc->cost.copy_cost));
      }
    }
  }
  DBUG_VOID_RETURN;
}
#endif

/*
  store the extracted contents from the in memory representation of the
  parsed json trace context, into the variable rows.

  @return
    false  OK
    true  Error
*/
bool Optimizer_context_replay::infuse_table_rows(const TABLE *tbl,
                                                 ha_rows *rows)
{
  if (!has_records() || !is_base_table(tbl->pos_in_table_list))
    return true;

  String tbl_name;
  store_full_table_name(tbl->pos_in_table_list, &tbl_name);
  List_iterator<trace_table_context_read> table_itr(this->ctx_list);

  while (trace_table_context_read *tbl_ctx= table_itr++)
  {
    if (strcmp(tbl_name.c_ptr_safe(), tbl_ctx->name) == 0)
    {
      *rows= tbl_ctx->total_rows;
      return false;
    }
  }

  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED),
      tbl_name.c_ptr_safe(), "list of table contexts");
  return true;
}

/*
  check the extracted contents from from the in memory representation of the
  parsed json trace context, and return the List of number of records per key
  for the given table and index name
*/
List<ha_rows> *
Optimizer_context_replay::get_index_rec_per_key_list(const TABLE *tbl,
                                                     const char *idx_name)
{
  if (!has_records() || !is_base_table(tbl->pos_in_table_list))
    return NULL;

  String tbl_name;
  store_full_table_name(tbl->pos_in_table_list, &tbl_name);
  List_iterator<trace_table_context_read> table_itr(this->ctx_list);

  while (trace_table_context_read *tbl_ctx= table_itr++)
  {
    if (strcmp(tbl_name.c_ptr_safe(), tbl_ctx->name) == 0)
    {
      List_iterator<trace_index_context_read> index_itr(tbl_ctx->index_list);
      while (trace_index_context_read *idx_ctx= index_itr++)
      {
        if (strcmp(idx_name, idx_ctx->idx_name) == 0)
        {
          return &idx_ctx->list_rec_per_key;
        }
      }
    }
  }

  String name;
  name.append(tbl_name);
  name.append(STRING_WITH_LEN("."));
  name.append(idx_name, strlen(idx_name));

  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED),
      name.c_ptr_safe(), "list of index contexts");
  return NULL;
}

/*
  check the extracted contents from the in memory representation of the
  parsed json trace context, and add the range contexts for the given table,
  and index to the list
*/
void Optimizer_context_replay::store_range_contexts(
    const TABLE *tbl, const char *idx_name,
    List<trace_range_context_read> *list)
{
  if (!has_records() || !list)
    return;

  String tbl_name;
  store_full_table_name(tbl->pos_in_table_list, &tbl_name);
  List_iterator<trace_table_context_read> table_itr(this->ctx_list);

  while (trace_table_context_read *tbl_ctx= table_itr++)
  {
    if (strcmp(tbl_name.c_ptr_safe(), tbl_ctx->name) == 0)
    {
      List_iterator<trace_range_context_read> range_ctx_itr(
          tbl_ctx->ranges_list);
      while (trace_range_context_read *range_ctx= range_ctx_itr++)
      {
        if (strcmp(idx_name, range_ctx->index_name) == 0)
        {
          list->push_back(range_ctx);
        }
      }
    }
  }

  if (list->is_empty())
  {
    String name;
    name.append(tbl_name);
    name.append(STRING_WITH_LEN("."));
    name.append(idx_name, strlen(idx_name));
    push_warning_printf(
        thd, Sql_condition::WARN_LEVEL_WARN,
        ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED,
        ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED),
        name.c_ptr_safe(), "list of range contexts");
  }
}
