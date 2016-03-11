/*
  Copyright (c) 2008, Roland Bouman
  http://rpbouman.blogspot.com/
  roland.bouman@gmail.com
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:
      * Redistributions of source code must retain the above copyright
        notice, this list of conditions and the following disclaimer.
      * Redistributions in binary form must reproduce the above copyright
        notice, this list of conditions and the following disclaimer in the
        documentation and/or other materials provided with the distribution.
      * Neither the name of the Roland Bouman nor the
        names of the contributors may be used to endorse or promote products
        derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#ifndef MYSQL_SERVER
#define MYSQL_SERVER
#endif

#include <sql_cache.h>
#include <sql_parse.h>          // check_global_access
#include <sql_acl.h>            // PROCESS_ACL
#include <sql_class.h>          // THD
#include <table.h>              // ST_SCHEMA_TABLE
#include <set_var.h>            // sql_mode_string_representation
#include <tztime.h>
#include <mysql/plugin.h>

class Accessible_Query_Cache : public Query_cache {
public:
  HASH *get_queries()
  {
    return &this->queries;
  }
} *qc;

bool schema_table_store_record(THD *thd, TABLE *table);

#define MAX_STATEMENT_TEXT_LENGTH 32767
#define COLUMN_STATEMENT_SCHEMA 0
#define COLUMN_STATEMENT_TEXT 1
#define COLUMN_RESULT_BLOCKS_COUNT 2
#define COLUMN_RESULT_BLOCKS_SIZE 3
#define COLUMN_RESULT_BLOCKS_SIZE_USED 4
#define COLUMN_LIMIT 5
#define COLUMN_MAX_SORT_LENGTH 6
#define COLUMN_GROUP_CONCAT_MAX_LENGTH 7
#define COLUMN_CHARACTER_SET_CLIENT 8
#define COLUMN_CHARACTER_SET_RESULT 9
#define COLUMN_COLLATION 10
#define COLUMN_TIMEZONE 11
#define COLUMN_DEFAULT_WEEK_FORMAT 12
#define COLUMN_DIV_PRECISION_INCREMENT 13
#define COLUMN_SQL_MODE 14
#define COLUMN_LC_TIME_NAMES 15

#define COLUMN_CLIENT_LONG_FLAG 16
#define COLUMN_CLIENT_PROTOCOL_41 17
#define COLUMN_PROTOCOL_TYPE 18
#define COLUMN_MORE_RESULTS_EXISTS 19
#define COLUMN_IN_TRANS 20
#define COLUMN_AUTOCOMMIT 21
#define COLUMN_PKT_NR 22

/* ST_FIELD_INFO is defined in table.h */
static ST_FIELD_INFO qc_info_fields[]=
{
  {"STATEMENT_SCHEMA", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"STATEMENT_TEXT", MAX_STATEMENT_TEXT_LENGTH, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"RESULT_BLOCKS_COUNT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"RESULT_BLOCKS_SIZE", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"RESULT_BLOCKS_SIZE_USED", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"LIMIT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"MAX_SORT_LENGTH", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"GROUP_CONCAT_MAX_LENGTH", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"CHARACTER_SET_CLIENT", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"CHARACTER_SET_RESULT", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"COLLATION", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"TIMEZONE", 50, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"DEFAULT_WEEK_FORMAT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"DIV_PRECISION_INCREMENT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"SQL_MODE", 250, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"LC_TIME_NAMES", 100, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"CLIENT_LONG_FLAG", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_TINY, 0, 0, 0, 0},
  {"CLIENT_PROTOCOL_41", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_TINY, 0, 0, 0, 0},
  {"PROTOCOL_TYPE", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_TINY, 0, 0, 0, 0},
  {"MORE_RESULTS_EXISTS", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_TINY, 0, 0, 0, 0},
  {"IN_TRANS", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_TINY, 0, 0, 0, 0},
  {"AUTOCOMMIT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_TINY, 0, 0, 0, 0},
  {"PACKET_NUMBER", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_TINY, 0, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}
};

static int qc_info_fill_table(THD *thd, TABLE_LIST *tables,
                                              COND *cond)
{
  int status= 1;
  CHARSET_INFO *scs= system_charset_info;
  TABLE *table= tables->table;
  HASH *queries = qc->get_queries();

  /* one must have PROCESS privilege to see others' queries */
  if (check_global_access(thd, PROCESS_ACL, true))
    return 0;

  if (qc->try_lock(thd))
    return 0; // QC is or is being disabled

  /* loop through all queries in the query cache */
  for (uint i= 0; i < queries->records; i++)
  {
    const uchar *query_cache_block_raw;
    Query_cache_block* query_cache_block;
    Query_cache_query* query_cache_query;
    Query_cache_query_flags flags;
    uint result_blocks_count;
    ulonglong result_blocks_size;
    ulonglong result_blocks_size_used;
    Query_cache_block *first_result_block;
    Query_cache_block *result_block;
    const char *statement_text;
    size_t statement_text_length;
    size_t flags_length;
    const char *key, *db;
    size_t key_length, db_length;
    LEX_STRING sql_mode_str;
    const String *tz;
    CHARSET_INFO *cs_client;
    CHARSET_INFO *cs_result;
    CHARSET_INFO *collation;

    query_cache_block_raw = my_hash_element(queries, i);
    query_cache_block = (Query_cache_block*)query_cache_block_raw;
    if (query_cache_block->type != Query_cache_block::QUERY)
      continue;

    query_cache_query = query_cache_block->query();

    /* Get the actual SQL statement for this query cache query */
    statement_text = (const char*)query_cache_query->query();
    statement_text_length = strlen(statement_text);
    /* We truncate SQL statements up to MAX_STATEMENT_TEXT_LENGTH in our I_S table */
    table->field[COLUMN_STATEMENT_TEXT]->store((char*)statement_text,
           MY_MIN(statement_text_length, MAX_STATEMENT_TEXT_LENGTH), scs);

    /* get the entire key that identifies this query cache query */
    key = (const char*)query_cache_query_get_key(query_cache_block_raw,
                                                 &key_length, 0);
    /* get and store the flags */
    flags_length= key_length - QUERY_CACHE_FLAGS_SIZE;
    memcpy(&flags, key+flags_length, QUERY_CACHE_FLAGS_SIZE);
    table->field[COLUMN_LIMIT]->store(flags.limit, 0);
    table->field[COLUMN_MAX_SORT_LENGTH]->store(flags.max_sort_length, 0);
    table->field[COLUMN_GROUP_CONCAT_MAX_LENGTH]->store(flags.group_concat_max_len, 0);

    cs_client= get_charset(flags.character_set_client_num, MYF(MY_WME));
    table->field[COLUMN_CHARACTER_SET_CLIENT]->store(cs_client->csname, strlen(cs_client->csname), scs);
    cs_result= get_charset(flags.character_set_results_num, MYF(MY_WME));
    table->field[COLUMN_CHARACTER_SET_RESULT]->store(cs_result->csname, strlen(cs_result->csname), scs);
    collation= get_charset(flags.collation_connection_num, MYF(MY_WME));
    table->field[COLUMN_COLLATION]->store(collation->name, strlen(collation->name), scs);

    tz= flags.time_zone->get_name();
    table->field[COLUMN_TIMEZONE]->store(tz->ptr(), tz->length(), scs);
    table->field[COLUMN_DEFAULT_WEEK_FORMAT]->store(flags.default_week_format, 0);
    table->field[COLUMN_DIV_PRECISION_INCREMENT]->store(flags.div_precision_increment, 0);

    sql_mode_string_representation(thd, flags.sql_mode, &sql_mode_str);
    table->field[COLUMN_SQL_MODE]->store(sql_mode_str.str, sql_mode_str.length, scs);

    table->field[COLUMN_LC_TIME_NAMES]->store(flags.lc_time_names->name,strlen(flags.lc_time_names->name), scs);

    table->field[COLUMN_CLIENT_LONG_FLAG]->store(flags.client_long_flag, 0);
    table->field[COLUMN_CLIENT_PROTOCOL_41]->store(flags.client_protocol_41, 0);
    table->field[COLUMN_PROTOCOL_TYPE]->store(flags.protocol_type, 0);
    table->field[COLUMN_MORE_RESULTS_EXISTS]->store(flags.more_results_exists, 0);
    table->field[COLUMN_IN_TRANS]->store(flags.in_trans, 0);
    table->field[COLUMN_AUTOCOMMIT]->store(flags.autocommit, 0);
    table->field[COLUMN_PKT_NR]->store(flags.pkt_nr, 0);

    /* The database against which the statement is executed is part of the
       query cache query key
     */
    compile_time_assert(QUERY_CACHE_DB_LENGTH_SIZE == 2); 
    db= key + statement_text_length + 1 + QUERY_CACHE_DB_LENGTH_SIZE;
    db_length= uint2korr(db - QUERY_CACHE_DB_LENGTH_SIZE);

    table->field[COLUMN_STATEMENT_SCHEMA]->store(db, db_length, scs);

    /* If we have result blocks, process them */
    first_result_block= query_cache_query->result();
    if(first_result_block)
    {
      /* initialize so we can loop over the result blocks*/
      result_block= first_result_block;
      result_blocks_count = 1;
      result_blocks_size = result_block->length;
      result_blocks_size_used = result_block->used;

      /* loop over the result blocks*/
      while((result_block= result_block->next)!=first_result_block)
      {
        /* calculate total number of result blocks */
        result_blocks_count++;
        /* calculate total size of result blocks */
        result_blocks_size += result_block->length;
        /* calculate total of used size of result blocks */
        result_blocks_size_used += result_block->used;
      }
    }
    else
    {
      result_blocks_count = 0;
      result_blocks_size = 0;
      result_blocks_size_used = 0;
    }
    table->field[COLUMN_RESULT_BLOCKS_COUNT]->store(result_blocks_count, 0);
    table->field[COLUMN_RESULT_BLOCKS_SIZE]->store(result_blocks_size, 0);
    table->field[COLUMN_RESULT_BLOCKS_SIZE_USED]->store(result_blocks_size_used, 0);

    if (schema_table_store_record(thd, table))
      goto cleanup;
  }
  status = 0;

cleanup:
  qc->unlock();
  return status;
}

static int qc_info_plugin_init(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;

  schema->fields_info= qc_info_fields;
  schema->fill_table= qc_info_fill_table;

#ifdef _WIN32
  qc = (Accessible_Query_Cache *)
    GetProcAddress(GetModuleHandle(NULL), "?query_cache@@3VQuery_cache@@A");
#else
  qc = (Accessible_Query_Cache *)&query_cache;
#endif

  return qc == 0;
}


static struct st_mysql_information_schema qc_info_plugin=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

/*
  Plugin library descriptor
*/

maria_declare_plugin(query_cache_info)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &qc_info_plugin,
  "QUERY_CACHE_INFO",
  "Roland Bouman, Daniel Black",
  "Lists all queries in the query cache.",
  PLUGIN_LICENSE_BSD,
  qc_info_plugin_init, /* Plugin Init */
  0,                          /* Plugin Deinit        */
  0x0101,                     /* version, hex         */
  NULL,                       /* status variables     */
  NULL,                       /* system variables     */
  "1.1",                      /* version as a string  */
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;

