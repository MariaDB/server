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

#include "opt_trace_ddl_info.h"
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

/**
  @file

  @brief
    This file provides mechanism to: -
    1. Store the tables/views context including index stats, range stats
    into the trace.
    2. Parse the context in JSON format, and build in memory representation
    of the read stats
    3. Infuse the read stats into the optimzer.

  @detail
    1. Stores the tables, and views context (i.e. ddls, and basic stats)
    that are used in either SELECT, INSERT, DELETE, and UPDATE queries,
    into the optimizer trace. All the contexts are stored in
    one place as a JSON array object with name "list_contexts".
    Additionally, database name is also included in the trace.
    The json structure looks like: -
    {
      "current_database": "db_name",
      "list_contexts": [
        {
          "name": "table_name",
          "ddl": "create table/view definition",
          "num_of_records": n,
          "indexes": [
            {
              "index_name": "index_name1",
              "rec_per_key": ["n1", "n2", ...]
            }, ...,
          ],
          "list_ranges": [
            {
              "index_name": "index_name1",
              "ranges": ["(NULL) < (key1) < (foo)", ...],
              "num_rows": n
            }, ...
          ]
        }, ...
      ]
    }
    The function "store_tables_context_in_trace()" is used to dump the
    stats into trace.
    Additionally, before dumping the stats to trace, range_stats are gathered
    in memory using the class Range_list_recorder

    2. Later, when this JSON structure is given as input to the variable
    "optimizer_stored_context", it is parsed and an in-memory representation
    of the same structure is built using the class Optimizer_context_replay.

    3. To infuse the stats into the optimizer, the same class
    Optimizer_context_replay is used. TODO: out of date?
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
   - index name
   - number of records
   - list of ranges.
*/
class Multi_range_read_const_call_record : public Range_list_recorder
{
public:
  char *idx_name;
  size_t num_records;
  List<Range_record> range_list;
  double comp_cost;
};

/*
   structure to store all the index range records
   pertaining to a table
*/
class trace_table_index_range_context : public Sql_alloc
{
public:
  /*
     full name of the table or view
     i.e db_name.[table/view]_name
  */
  char *name;
  size_t name_len;
  List<Multi_range_read_const_call_record> index_list;
};

static void store_full_table_name(TABLE_LIST *tbl, String *buf);

struct DDL_Key
{
  char *name; //full name of the table or view
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
  return
    (tbl->table &&
     tbl->table->s &&
     !tbl->table_function &&
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

  int act_escape_stmt_len=
      json_escape_string(stmt->c_ptr(), stmt->c_ptr() + non_esc_stmt_len,
                         escaped_stmt, escaped_stmt + escape_stmt_len);

  if (act_escape_stmt_len < 0)
    return true;

  escaped_stmt[act_escape_stmt_len]= 0;
  ctx_wrapper.add("ddl", escaped_stmt);
  return false;
}


static void dump_index_range_stats_to_trace(THD *thd, uchar *tbl_name,
                                            size_t tbl_name_len)
{
  if (!thd->opt_ctx_recorder || !thd->opt_ctx_recorder->has_records())
    return;

  trace_table_index_range_context *context=
      thd->opt_ctx_recorder->search(tbl_name, tbl_name_len);

  if (!context)
    return;

  Json_writer_array list_ranges_wrapper(thd, "list_ranges");
  List_iterator irc_li(context->index_list);
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
    irc_wrapper.add("comp_cost", irc->comp_cost);
  }
}

/*
  dump the following table stats to trace: -
  1. total number of records in the table
  2. if there any indexes for the table then
      their names, and the num of records per key
  3. range stats on the indexes
*/
static void dump_table_stats_to_trace(THD *thd, TABLE_LIST *tbl,
                                      uchar *tbl_name, size_t tbl_name_len,
                                      Json_writer_object &ctx_wrapper)
{
  TABLE *table= tbl->table;
  ctx_wrapper.add("num_of_records", tbl->table->stat_records());

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
  dump_index_range_stats_to_trace(thd, tbl_name, tbl_name_len);
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
    Can read or write operation be performed on the trace context
*/
bool can_rw_trace_context(THD* thd)
{
  LEX *lex= thd->lex;
  return (thd->variables.optimizer_trace &&
          (thd->variables.optimizer_record_context ||
           (thd->variables.optimizer_stored_context &&
            strlen(thd->variables.optimizer_stored_context)>0)
          ) &&
          (lex->sql_command == SQLCOM_SELECT ||
           lex->sql_command == SQLCOM_INSERT_SELECT ||
           lex->sql_command == SQLCOM_DELETE ||
           lex->sql_command == SQLCOM_UPDATE ||
           lex->sql_command == SQLCOM_DELETE_MULTI ||
           lex->sql_command == SQLCOM_UPDATE_MULTI));
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

  if (lex->query_tables == *(lex->query_tables_last))
    return false;

  Json_writer_object main_wrapper(thd);
  Json_writer_object context(thd, "optimizer_context");
  main_wrapper.add("current_database", thd->get_db());
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
    if (my_hash_search(&hash, (uchar *) name.c_ptr(), name.length()))
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

    ddl_key->name= strdup_root(thd->mem_root, name.c_ptr());
    ddl_key->name_len= name.length();
    my_hash_insert(&hash, (uchar *) ddl_key);
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

trace_table_index_range_context *
Optimizer_context_recorder::search(uchar *tbl_name, size_t tbl_name_len)
{
  return (trace_table_index_range_context *) my_hash_search(
      &tbl_trace_ctx_hash, tbl_name, tbl_name_len);
}

/*
  @detail
    Do not use thd->mem_root, allocate memory only on the passed mem_root.
*/
void Range_list_recorder::add_range(MEM_ROOT *mem_root, const char *range)
{
  Range_record *record= new (mem_root) Range_record;
  record->range= strdup_root(mem_root, range);
  ((Multi_range_read_const_call_record *) this)->range_list.
                                                 push_back(record, mem_root);
}


/*
  @brief
    Start recording a range list for tbl.index_name

  @return
    Pointer one can use to add ranges.
*/

Range_list_recorder*
Optimizer_context_recorder::start_range_list_record(
  MEM_ROOT *mem_root, TABLE_LIST *tbl, size_t found_records,
  const char *index_name, double comp_cost)
{
  String tbl_name;

  Multi_range_read_const_call_record *index_ctx=
      new (mem_root) Multi_range_read_const_call_record;

  /*
    Create a new table context if it is not already present in the
    hash.
    Store the ranges of every index of the table into the
    table context.
    The table context is also persisted in the hash which is to be
    used later for dumping all the context infomation into the trace.
  */
  store_full_table_name(tbl, &tbl_name);
  trace_table_index_range_context *table_ctx=
      search((uchar *) tbl_name.c_ptr(), tbl_name.length());

  if (!table_ctx)
  {
    table_ctx= new (mem_root) trace_table_index_range_context;
    table_ctx->name= strdup_root(mem_root, tbl_name.c_ptr());
    table_ctx->name_len= tbl_name.length();
    table_ctx->index_list.empty();
    my_hash_insert(&tbl_trace_ctx_hash, (uchar *) table_ctx);
  }

  index_ctx->idx_name= strdup_root(mem_root, index_name);
  index_ctx->num_records= found_records;
  index_ctx->comp_cost= comp_cost;
  table_ctx->index_list.push_back(index_ctx, mem_root);
  return index_ctx;
}

/*
  helper function to know the key portion of the
  trace table context that is stored in hash.
*/
const uchar *Optimizer_context_recorder::get_tbl_trace_ctx_key(
    const void *entry_, size_t *length, my_bool flags)
{
  auto entry= static_cast<const trace_table_index_range_context *>(entry_);
  *length= entry->name_len;
  return reinterpret_cast<const uchar *>(entry->name);
}

/*
  store full table name i.e. "db_name.table_name",
  into the supplied variable buf
*/
static void store_full_table_name(TABLE_LIST *tbl, String *buf)
{
  buf->append(tbl->get_db_name().str, tbl->get_db_name().length);
  buf->append(STRING_WITH_LEN("."));
  buf->append(tbl->get_table_name().str, tbl->get_table_name().length);
}

Optimizer_context_recorder *get_opt_context_recorder(THD *thd)
{
  if (thd->variables.optimizer_record_context &&
      !thd->lex->explain->is_query_plan_ready())
  {
    if (!thd->opt_ctx_recorder)
      thd->opt_ctx_recorder= new Optimizer_context_recorder();
    return thd->opt_ctx_recorder;
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
  double comp_cost;
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
  the one table context i.e. read from json.
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
  List<trace_index_context_read> index_list;
  List<trace_range_context_read> ranges_list;
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


Optimizer_context_replay::Optimizer_context_replay(THD *thd)
{
  db_name= NULL;
  ctx_list.empty();
  saved_tablestats_list.empty();
  parse(thd);
}

/*
  check the list of range stats that are already extracted from the
  json trace context, for the given table_name, and index_name.
  If they match, then compare the ranges one by one until all of them
  match. If so, load the num_records, and the computation cost associated
  with it into the arguments passed.
*/
void
Optimizer_context_replay::load_range_stats_into_client(
    THD *thd,
    TABLE *table,
    uint keynr,
    RANGE_SEQ_IF *seq_if,
    SEL_ARG_RANGE_SEQ *seq,
    Cost_estimate *cost,
    ha_rows *rows)
{
  if (!has_records() || !is_base_table(table->pos_in_table_list))
    return;

  KEY *keyinfo= table->key_info + keynr;
  const char *idx_name= keyinfo->name.str;
  const KEY_PART_INFO *key_part= keyinfo->key_part;
  uint n_key_parts= table->actual_n_key_parts(keyinfo);
  KEY_MULTI_RANGE multi_range;
  range_seq_t seq_it= seq_if->init((void *) seq, 0, 0);
  trace_range_context_read *range_ctx= get_range_context(thd, table, idx_name);
  if (range_ctx != NULL)
  {
    List_iterator<char> range_itr(range_ctx->ranges);
    while (!seq_if->next(seq_it, &multi_range))
    {
      StringBuffer<128> range_info(system_charset_info);
      print_range(&range_info, key_part, &multi_range, n_key_parts);
      char *r1= range_info.c_ptr_safe();
      char *r2= range_itr++;
      if (r2 == NULL || strcmp(r1, r2) != 0)
      {
        String err;
        String tbl_name;
        store_full_table_name(table->pos_in_table_list, &tbl_name);
        err.append(STRING_WITH_LEN("the list of ranges for "));
        err.append(tbl_name);
        err.append(STRING_WITH_LEN("."));
        err.append(idx_name, strlen(idx_name));
        err.append(
            STRING_WITH_LEN(
                " is not in the list of range contexts loaded " \
                "from optimizer_stored_context"));
        push_warning_printf(
            thd, Sql_condition::WARN_LEVEL_WARN,
            ER_JSON_OPTIMIZER_STORED_CONTEXT_PARSE_FAILED,
            ER_THD(thd, ER_JSON_OPTIMIZER_STORED_CONTEXT_PARSE_FAILED),
            err.c_ptr_safe(),
            0);
        return;
      }
    }
    cost->comp_cost= range_ctx->comp_cost;
    *rows= range_ctx->num_rows;
  }
}


/*
  @brief
    Save the current stats of the table and its associated indexes.
    And then use the trace_context stats,
    to update the table, and its associated indexes.
*/

void
Optimizer_context_replay::set_table_stats_from_context(THD *thd, TABLE *table)
{
  if (!is_base_table(table->pos_in_table_list))
    return;

  Saved_Table_stats *saved_ts= new Saved_Table_stats();
  saved_ts->saved_indexstats_list.empty();
  saved_ts->table= table;
  saved_ts->original_rows= table->used_stat_records;
  saved_tablestats_list.push_back(saved_ts);

   ha_rows temp_rows= get_table_rows(thd, table);
  if (temp_rows > 0)
    table->used_stat_records= temp_rows;

  KEY *key_info, *key_info_end;
  for (key_info= table->key_info, key_info_end= key_info + table->s->keys;
       key_info < key_info_end; key_info++)
  {
    List<ha_rows> *index_freq_list=
        get_index_rec_per_key_list(thd, table, key_info->name.str);

    if (index_freq_list && !index_freq_list->is_empty())
    {
      Saved_Index_stats *saved_is= new Saved_Index_stats();
      uint i= 0;
      uint num_key_parts= key_info->user_defined_key_parts;
      Index_statistics *original_read_stats= key_info->read_stats;
      bool original_is_statistics_from_stat_tables=
          key_info->is_statistics_from_stat_tables;
      Index_statistics *new_read_stats= new Index_statistics();
      ulonglong *frequencies=
          (ulonglong *) thd->alloc(sizeof(ulonglong) * num_key_parts);
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
    while (Saved_Index_stats* saved_is = index_li++)
    {
      saved_is->key_info->is_statistics_from_stat_tables=
          saved_is->original_is_statistics_from_stat_tables;
      saved_is->key_info->read_stats= saved_is->original_read_stats;
    }
  }
}

/*
  does the extractor hold any records that are read from the json trace
*/
bool Optimizer_context_replay::has_records()
{
  return db_name != NULL && !ctx_list.is_empty();
}

/*
  parse the trace context that abides to the following structure: -
  {
    "current_database": "db_name",
    "list_contexts": [
      {
        "name": "table_name",
        "ddl": "create table/view definition",
        "num_of_records": n,
        "indexes": [
          {
            "index_name": "index_name1",
            "rec_per_key": ["n1", "n2", ...]
          }, ...,
        ],
        "list_ranges": [
          {
            "index_name": "index_name1",
            "ranges": ["(NULL) < (key1) < (foo)", ...],
            "num_rows": n
          }, ...
        ]
      }, ...
    ]
  }

  @return
    FALSE  OK
    TRUE  Parse Error
*/

bool Optimizer_context_replay::parse(THD *thd)
{
  json_engine_t je;
  int rc;
  const char *err= "JSON parse error";
  constexpr const char* DB_NAME_KEY="current_database";
  constexpr const char* LIST_CONTEXTS_KEY="list_contexts";
  char *context= thd->variables.optimizer_stored_context;
  bool have_db_name= false;
  bool have_list_contexts= false;
  DBUG_ENTER("Optimizer_context_replay::parse");

  json_scan_start(&je, system_charset_info,
                  (const uchar*)context,
                  (const uchar*)context+strlen(context));

  if (json_scan_next(&je))
    goto err;

  if (je.state != JST_OBJ_START)
  {
    err= "Root JSON element must be a JSON object";
    goto err;
  }

  while (!(rc= json_scan_next(&je)) && je.state != JST_OBJ_END)
  {
    Json_saved_parser_state save1(&je);
    Json_string db_name_key(DB_NAME_KEY);
    if (json_key_matches(&je, db_name_key.get()))
    {
      if (json_read_value(&je))
      {
        err= "optimizer_stored_context must contain db name value";
        goto err;
      }

      char *db_name= thd->calloc(sizeof(char) * (je.value_len + 1));
      int res= json_unescape(
          system_charset_info, (const uchar *) je.value,
          (const uchar *) je.value + je.value_len, system_charset_info,
          (uchar *) db_name, (uchar *) db_name + je.value_len);
      if (res < 0)
      {
        err= "un-escaping error of db name element";
        goto err;
      }
      this->db_name= db_name;
      have_db_name= true;
      continue;
    }
    save1.restore_to(&je);

    Json_string list_contexts_key(LIST_CONTEXTS_KEY);
    if (json_key_matches(&je, list_contexts_key.get()))
    {
      if (json_scan_next(&je))
        goto err;

      if (je.state != JST_ARRAY_START)
      {
        err= "optimizer_stored_context must contain an array";
        goto err;
      }

      while (1)
      {
        trace_table_context_read *table_ctx= new trace_table_context_read();
        table_ctx->index_list.empty();
        table_ctx->ranges_list.empty();
        rc= parse_table_context(thd, &je, &err, table_ctx);
        if (rc > 0) // Got error other than EOF
          goto err;
        if (rc < 0)
          break;
        this->ctx_list.push_back(table_ctx);
      }
      have_list_contexts= true;
      continue;
    }
    save1.restore_to(&je);
  }

  if (rc)
    goto err;
  if (!have_db_name)
  {
    err= "\"current_database\" element not present";
    goto err;
  }
  if (!have_list_contexts)
  {
    err= "\"list_contexts\" element not present";
    goto err;
  }
#ifndef DBUG_OFF
  dbug_print_read_stats();
#endif
  DBUG_RETURN(false); // Ok
err:
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_STORED_CONTEXT_PARSE_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_STORED_CONTEXT_PARSE_FAILED), err,
      (je.s.c_str - (const uchar *) context));
  DBUG_RETURN(true);
}

/*
  Parses the table context having the following structure: -
  {
    "name": "table_name",
    "ddl": "create table/view definition",
    "num_of_records": n,
    "indexes": [
      {
        "index_name": "index_name1",
         "rec_per_key": ["n1", "n2", ...]
      }, ...,
    ],
    "list_ranges": [
      {
        "index_name": "index_name1",
        "ranges": ["(NULL) < (key1) < (foo)", ...],
        "num_rows": n
      }, ...
    ]
  }

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
int Optimizer_context_replay::parse_table_context(
    THD *thd, json_engine_t *je, const char **err,
    trace_table_context_read *table_ctx)
{
  int rc;
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
    *err= "Expected an object in the list_contexts array";
    return 1;
  }

  bool have_name= false;
  bool have_ddl= false;
  bool have_num_records= false;
  bool have_indexes= false;
  bool have_ranges= false;

  while (!(rc= json_scan_next(je)) && je->state != JST_OBJ_END)
  {
    Json_saved_parser_state save1(je);
    Json_string name_key("name");
    if (json_key_matches(je, name_key.get()))
    {
      if (json_read_value(je))
      {
        *err= "error reading name value";
        return 1;
      }
      char *name= thd->calloc(sizeof(char) * (je->value_len + 1));
      int res= json_unescape(system_charset_info, (const uchar *) je->value,
                             (const uchar *) je->value + je->value_len,
                             system_charset_info, (uchar *) name,
                             (uchar *) name + je->value_len);
      if (res < 0)
      {
        *err= "un-escaping error of db name element";
        return 1;
      }
      table_ctx->name= name;
      have_name= true;
      continue;
    }
    save1.restore_to(je);

    Json_string ddl_str("ddl");
    if (json_key_matches(je, ddl_str.get()))
    {
      if (json_read_value(je))
      {
        *err= "error reading ddl value";
        return 1;
      }

      char *ddl= thd->calloc(sizeof(char) * (je->value_len + 1));
      int res= json_unescape(system_charset_info, (const uchar *) je->value,
                             (const uchar *) je->value + je->value_len,
                             system_charset_info, (uchar *) ddl,
                             (uchar *) ddl + je->value_len);
      if (res < 0)
      {
        *err= "un-escaping error of db name element";
        return 1;
      }
      table_ctx->ddl= ddl;
      have_ddl= true;
      continue;
    }
    save1.restore_to(je);

    Json_string num_of_records_key("num_of_records");
    if (json_key_matches(je, num_of_records_key.get()))
    {
      if (json_read_value(je))
      {
        *err= "error reading num_of_records value";
        return 1;
      }

      const char *size= (const char *) je->value_begin;
      char *size_end= (char *) je->value_end;
      int conv_err;
      ha_rows num_of_records= my_strtoll10(size, &size_end, &conv_err);
      if (conv_err)
      {
        *err= "num_of_records member must be a integer value";
        return 1;
      }
      table_ctx->total_rows= num_of_records;
      have_num_records= true;
      continue;
    }
    save1.restore_to(je);

    Json_string indexes_key("indexes");
    if (json_key_matches(je, indexes_key.get()))
    {
      if (json_scan_next(je))
        return 1;

      if (je->state != JST_ARRAY_START)
      {
        *err= "optimizer_stored_context must contain an array";
        return 1;
      }

      while (1)
      {
        trace_index_context_read *index_ctx=
            new trace_index_context_read();
        index_ctx->list_rec_per_key.empty();
        rc= parse_index_context(thd, je, err, index_ctx);
        if (rc > 0) // Got error other than EOF
          return 1;
        if (rc < 0)
          break;
        table_ctx->index_list.push_back(index_ctx);
      }
      have_indexes= true;
      continue;
    }
    save1.restore_to(je);

    Json_string list_ranges_key("list_ranges");
    if (json_key_matches(je, list_ranges_key.get()))
    {
      if (json_scan_next(je))
        return 1;

      if (je->state != JST_ARRAY_START)
      {
        *err= "optimizer_stored_context must contain an array";
        return 1;
      }

      while (1)
      {
        trace_range_context_read *range_ctx= new trace_range_context_read();
        range_ctx->ranges.empty();
        rc= parse_range_context(thd, je, err, range_ctx);
        if (rc > 0) // Got error other than EOF
          return 1;
        if (rc < 0)
          break;
        table_ctx->ranges_list.push_back(range_ctx);
      }
      have_ranges= true;
      continue;
    }
    save1.restore_to(je);
  }

  if (rc)
    return 1;

  if (!have_name)
  {
    *err= "\"name\" element not present";
    return 1;
  }

  if (!have_ddl)
  {
    *err= "\"ddl\" element not present";
    return 1;
  }

  if (!have_num_records)
  {
    *err= "\"num_of_records\" element not present";
    return 1;
  }

  if (!have_indexes)
  {
    *err= "\"indexes\" element not present";
    return 1;
  }

  if (!have_ranges)
  {
    *err= "\"list_ranges\" element not present";
    return 1;
  }

  return false;
}


/*
  Parses the index context having the following structure: -
  {
    "index_name": "index_name1",
    "rec_per_key": ["n1", "n2", ...]
  }

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/

int Optimizer_context_replay::parse_index_context(
    THD *thd, json_engine_t *je, const char **err,
    trace_index_context_read *index_ctx)
{
  int rc;
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
    *err= "Expected an object in the indexes array";
    return 1;
  }

  bool have_index_name= false;
  bool have_rec_per_key= false;

  while (!(rc= json_scan_next(je)) && je->state != JST_OBJ_END)
  {
    Json_saved_parser_state save1(je);
    Json_string index_name_key("index_name");
    if (json_key_matches(je, index_name_key.get()))
    {
      if (json_read_value(je))
      {
        *err= "error reading index_name value";
        return 1;
      }
      char *index_name= thd->calloc(sizeof(char) * (je->value_len + 1));
      int res= json_unescape(system_charset_info, (const uchar *) je->value,
                             (const uchar *) je->value + je->value_len,
                             system_charset_info, (uchar *) index_name,
                             (uchar *) index_name + je->value_len);
      if (res < 0)
      {
        *err= "un-escaping error of index_name element";
        return 1;
      }
      index_ctx->idx_name= index_name;
      have_index_name= true;
      continue;
    }
    save1.restore_to(je);

    Json_string rec_per_key("rec_per_key");
    if (json_key_matches(je, rec_per_key.get()))
    {
      if (json_scan_next(je))
        return 1;

      if (je->state != JST_ARRAY_START)
        return 1;

      if (json_scan_next(je))
        return 1;

      while (je->state != JST_ARRAY_END)
      {
        if (json_read_value(je))
        {
          *err= "error reading rec_per_key value";
          return 1;
        }
        char *size= thd->calloc(sizeof(char) * (je->value_len + 1));
        int res= json_unescape(system_charset_info, (const uchar *) je->value,
                               (const uchar *) je->value + je->value_len,
                               system_charset_info, (uchar *) size,
                               (uchar *) size + je->value_len);
        if (res < 0)
        {
          *err= "un-escaping error of rec_per_key value";
          return 1;
        }
        int conv_err;
        char *endptr= size + strlen(size);
        ha_rows records= my_strtoll10(size, &endptr, &conv_err);
        if (conv_err)
        {
          *err= "records must be a numeric value";
          return 1;
        }
        ha_rows *records_ptr= (ha_rows *) thd->alloc(sizeof(ha_rows));
        *records_ptr= records;
        index_ctx->list_rec_per_key.push_back(records_ptr);
        if (json_scan_next(je))
          return 1;
      }
      have_rec_per_key= true;
      continue;
    }
    save1.restore_to(je);
  }

  if (rc)
    return 1;

  if (!have_index_name)
  {
    *err= "\"index_name\" element not present";
    return 1;
  }
  if (!have_rec_per_key)
  {
    *err= "\"rec_per_key\" element not present";
    return 1;
  }
  return false;
}

/*
  Parses the range context having the following structure: -
  {
    "index_name": "index_name1",
    "ranges": ["(NULL) < (key1) < (foo)", ...],
    "num_rows": n
  }

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
int Optimizer_context_replay::parse_range_context(
    THD *thd, json_engine_t *je, const char **err,
    trace_range_context_read *range_ctx)
{
  int rc;
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
    *err= "Expected an object in the list_contexts array";
    return 1;
  }

  bool have_index_name= false;
  bool have_ranges= false;
  bool have_num_rows= false;
  bool have_comp_cost= false;

  while (!(rc= json_scan_next(je)) && je->state != JST_OBJ_END)
  {
    Json_saved_parser_state save1(je);
    Json_string index_name_key("index_name");
    if (json_key_matches(je, index_name_key.get()))
    {
      if (json_read_value(je))
      {
        *err= "error reading index_name value";
        return 1;
      }
      char *index_name= thd->calloc(sizeof(char) * (je->value_len + 1));
      int res= json_unescape(system_charset_info, (const uchar *) je->value,
                             (const uchar *) je->value + je->value_len,
                             system_charset_info, (uchar *) index_name,
                             (uchar *) index_name + je->value_len);
      if (res < 0)
      {
        *err= "un-escaping error of index_name element";
        return 1;
      }
      range_ctx->index_name= index_name;
      have_index_name= true;
      continue;
    }
    save1.restore_to(je);

    Json_string ranges_key("ranges");
    if (json_key_matches(je, ranges_key.get()))
    {
      if (json_scan_next(je))
        return 1;

      if (je->state != JST_ARRAY_START)
        return 1;

      if (json_scan_next(je))
        return 1;

      while (je->state != JST_ARRAY_END)
      {
        if (json_read_value(je))
        {
          *err= "error reading ranges value";
          return 1;
        }
        char *range= thd->calloc(sizeof(char) * (je->value_len + 1));
        int res= json_unescape(system_charset_info, (const uchar *) je->value,
                               (const uchar *) je->value + je->value_len,
                               system_charset_info, (uchar *) range,
                               (uchar *) range + je->value_len);
        if (res < 0)
        {
          *err= "un-escaping error of range element";
          return 1;
        }
        range_ctx->ranges.push_back(range);
        if (json_scan_next(je))
          return 1;
      }
      have_ranges= true;
      continue;
    }
    save1.restore_to(je);

    Json_string num_rows_key("num_rows");
    if (json_key_matches(je, num_rows_key.get()))
    {
      if (json_read_value(je))
      {
        *err= "error reading num_rows value";
        return 1;
      }

      const char *size= (const char *) je->value_begin;
      char *size_end= (char *) je->value_end;
      int conv_err;
      ha_rows num_rows= my_strtoll10(size, &size_end, &conv_err);
      if (conv_err)
      {
        *err= "num_rows member must be a numeric value";
        return 1;
      }
      range_ctx->num_rows= num_rows;
      have_num_rows= true;
      continue;
    }
    save1.restore_to(je);

    Json_string comp_cost_key("comp_cost");
    if (json_key_matches(je, comp_cost_key.get()))
    {
      if (json_read_value(je))
      {
        *err= "error reading comp_cost value";
        return 1;
      }

      const char *size= (const char *) je->value_begin;
      char *size_end= (char *) je->value_end;
      int conv_err;
      double comp_cost= my_strtod(size, &size_end, &conv_err);
      if (conv_err)
      {
        *err= "comp_cost member must be a floating point value";
        return 1;
      }
      range_ctx->comp_cost= comp_cost;
      have_comp_cost= true;
      continue;
    }
    save1.restore_to(je);
  }

  if (rc)
    return 1;

  if (!have_index_name)
  {
    *err= "\"index_name\" element not present";
    return 1;
  }
  if (!have_ranges)
  {
    *err= "\"ranges\" element not present";
    return 1;
  }
  if (!have_num_rows)
  {
    *err= "\"num_rows\" element not present";
    return 1;
  }
  if (!have_comp_cost)
  {
    *err= "\"comp_cost\" element not present";
    return 1;
  }
  return false;
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
    DBUG_PRINT("info", ("num_of_records: %lld", tbl_ctx->total_rows));
    List_iterator<trace_index_context_read> index_itr(
        tbl_ctx->index_list);
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
      DBUG_PRINT("info", ("num_of_records: %llx", range_ctx->num_rows));
      DBUG_PRINT("info", ("comp_cost: %f", range_ctx->comp_cost));
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
    }
  }
  DBUG_VOID_RETURN;
}
#endif

/*
  check the extracted contents from json trace context, and
  return the number of rows that are in the given table
*/
ha_rows Optimizer_context_replay::get_table_rows(
    THD *thd, const TABLE *tbl)
{
  if (!has_records())
    return 0;

  String tbl_name;
  store_full_table_name(tbl->pos_in_table_list, &tbl_name);
  List_iterator<trace_table_context_read> table_itr(this->ctx_list);
  while (trace_table_context_read *tbl_ctx= table_itr++)
  {
    if (strcmp(tbl_name.c_ptr(), tbl_ctx->name) == 0)
    {
      return tbl_ctx->total_rows;
    }
  }
  String err;
  err.append(tbl_name);
  err.append(STRING_WITH_LEN(" is not in the list of table contexts loaded " \
                             "from optimizer_stored_context"));
  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_STORED_CONTEXT_PARSE_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_STORED_CONTEXT_PARSE_FAILED),
      err.c_ptr_safe(), 0);
  return 0;
}


/*
  check the extracted contents from json trace context, and
  return the List of number of records per key that are in
  the given table and index
*/
List<ha_rows> *
Optimizer_context_replay::get_index_rec_per_key_list(
    THD *thd, const TABLE *tbl, const char *idx_name)
{
  if (!has_records())
    return NULL;

  String tbl_name;
  store_full_table_name(tbl->pos_in_table_list, &tbl_name);
  List_iterator<trace_table_context_read> table_itr(this->ctx_list);
  while (trace_table_context_read *tbl_ctx= table_itr++)
  {
    if (strcmp(tbl_name.c_ptr(), tbl_ctx->name) == 0)
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
  String err;
  err.append(tbl_name);
  err.append(STRING_WITH_LEN("."));
  err.append(idx_name, strlen(idx_name));
  err.append(STRING_WITH_LEN(" is not in the list of index contexts loaded " \
             "from optimizer_stored_context"));
  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_STORED_CONTEXT_PARSE_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_STORED_CONTEXT_PARSE_FAILED), err.c_ptr_safe(), 0);
  return NULL;
}

/*
  check the extracted contents from json trace context, and
  return the range context for the given table, and index
*/
trace_range_context_read *
Optimizer_context_replay::get_range_context(
    THD *thd, const TABLE *tbl, const char *idx_name)
{
  if (!has_records())
    return NULL;

  String tbl_name;
  store_full_table_name(tbl->pos_in_table_list, &tbl_name);
  List_iterator<trace_table_context_read> table_itr(this->ctx_list);
  while (trace_table_context_read *tbl_ctx= table_itr++)
  {
    if (strcmp(tbl_name.c_ptr(), tbl_ctx->name) == 0)
    {
      List_iterator<trace_range_context_read> range_ctx_itr(
          tbl_ctx->ranges_list);
      while (trace_range_context_read *range_ctx= range_ctx_itr++)
      {
        if (strcmp(idx_name, range_ctx->index_name) == 0)
        {
          return range_ctx;
        }
      }
    }
  }
  String err;
  err.append(tbl_name);
  err.append(STRING_WITH_LEN("."));
  err.append(idx_name, strlen(idx_name));
  err.append(STRING_WITH_LEN(" is not in the list of range contexts loaded " \
             "from optimizer_stored_context"));
  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_STORED_CONTEXT_PARSE_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_STORED_CONTEXT_PARSE_FAILED),
      err.c_ptr_safe(), 0);
  return NULL;
}
