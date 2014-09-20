/*
  Copyright (c) 2008, Roland Bouman
  http://rpbouman.blogspot.com/
  roland.bouman@gmail.com
  All rights reserved.
  
  Copyright (c) 2013, Roberto Spadim, SPAEmpresarial - Brazil
  http://www.spadim.com.br/
  roberto@spadim.com.br
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

#include <sql_cache.h>          /* QUERY_CACHE_QC_INFO_PLUGIN */
#include <sql_parse.h>          // check_global_access
#include <sql_acl.h>            // PROCESS_ACL
#include <sql_class.h>          // THD
#include <table.h>              // ST_SCHEMA_TABLE
#include <set_var.h>            // sql_mode_string_representation
#include "tztime.h"             // struct Time_zone
#include <mysql/plugin.h>
#include <handler.h>            // table type
#include <m_ctype.h>

class Accessible_Query_Cache : public Query_cache {
public:
  HASH *get_queries()
  {
    return &this->queries;
  }
  HASH *get_tables()
  {
    return &this->tables;
  }
} *qc;

bool schema_table_store_record(THD *thd, TABLE *table);

#define MAX_STATEMENT_TEXT_LENGTH 32767

/* query table column positions */
/* query with flags and stats table */


#ifdef QUERY_CACHE_QC_INFO_PLUGIN             /* for more information see MariaDB - MDEV-4682 */

#define COLUMN_QUERIES_QC_ID 0
#define COLUMN_QUERIES_STATEMENT_SCHEMA 1
#define COLUMN_QUERIES_STATEMENT_TEXT 2
#define COLUMN_QUERIES_RESULT_FOUND_ROWS 3
#define COLUMN_QUERIES_QUERY_ROWS 4
#define COLUMN_QUERIES_SELECT_ROWS_READ 5
#define COLUMN_QUERIES_QUERY_HITS 6
#define COLUMN_QUERIES_QUERY_HITS_PERIOD_LOW 7
#define COLUMN_QUERIES_QUERY_HITS_PERIOD_HIGH 8
#define COLUMN_QUERIES_QUERY_HITS_PERIOD_OUTLIERS 9
#define COLUMN_QUERIES_QUERY_HITS_TOTAL_TIME_US 10
#define COLUMN_QUERIES_QUERY_HITS_MEAN_PERIOD_US 11
#define COLUMN_QUERIES_QUERY_HITS_MEAN_PERIOD_LOW_US 12
#define COLUMN_QUERIES_QUERY_HITS_MEAN_PERIOD_HIGH_US 13
#define COLUMN_QUERIES_QUERY_INSERT_TIME 14
#define COLUMN_QUERIES_QUERY_LAST_HIT_TIME 15
#define COLUMN_QUERIES_SELECT_EXPEND_TIME_US 16
#define COLUMN_QUERIES_SELECT_LOCK_TIME_US 17
#define COLUMN_QUERIES_TABLES_TYPE 18
#define COLUMN_QUERIES_RESULT_LENGTH 19
#define COLUMN_QUERIES_RESULT_BLOCKS_COUNT 20
#define COLUMN_QUERIES_RESULT_BLOCKS_SIZE 21
#define COLUMN_QUERIES_RESULT_BLOCKS_SIZE_USED 22
#define COLUMN_QUERIES_FLAGS_CLIENT_LONG_FLAG 23
#define COLUMN_QUERIES_FLAGS_CLIENT_PROTOCOL_41 24
#define COLUMN_QUERIES_FLAGS_PROTOCOL_TYPE 25
#define COLUMN_QUERIES_FLAGS_MORE_RESULTS_EXISTS 26
#define COLUMN_QUERIES_FLAGS_IN_TRANS 27
#define COLUMN_QUERIES_FLAGS_AUTOCOMMIT 28
#define COLUMN_QUERIES_FLAGS_PKT_NR 29
#define COLUMN_QUERIES_FLAGS_CHARACTER_SET_CLIENT 30
#define COLUMN_QUERIES_FLAGS_CHARACTER_SET_RESULTS 31
#define COLUMN_QUERIES_FLAGS_COLLATION_CONNECTION 32
#define COLUMN_QUERIES_FLAGS_LIMIT 33
#define COLUMN_QUERIES_FLAGS_TIME_ZONE 34
#define COLUMN_QUERIES_FLAGS_SQL_MODE 35
#define COLUMN_QUERIES_FLAGS_MAX_SORT_LENGTH 36
#define COLUMN_QUERIES_FLAGS_GROUP_CONCAT_MAX_LEN 37
#define COLUMN_QUERIES_FLAGS_DIV_PRECISION_INCREMENT 38
#define COLUMN_QUERIES_FLAGS_DEFAULT_WEEK_FORMAT 39
#define COLUMN_QUERIES_FLAGS_LC_TIME_NAMES 40

#else

#define COLUMN_QUERIES_QC_ID 0
#define COLUMN_QUERIES_STATEMENT_SCHEMA 1
#define COLUMN_QUERIES_STATEMENT_TEXT 2
#define COLUMN_QUERIES_RESULT_FOUND_ROWS 3
#define COLUMN_QUERIES_TABLES_TYPE 4
#define COLUMN_QUERIES_RESULT_LENGTH 5
#define COLUMN_QUERIES_RESULT_BLOCKS_COUNT 6
#define COLUMN_QUERIES_RESULT_BLOCKS_SIZE 7
#define COLUMN_QUERIES_RESULT_BLOCKS_SIZE_USED 8
#define COLUMN_QUERIES_FLAGS_CLIENT_LONG_FLAG 9
#define COLUMN_QUERIES_FLAGS_CLIENT_PROTOCOL_41 10
#define COLUMN_QUERIES_FLAGS_PROTOCOL_TYPE 11
#define COLUMN_QUERIES_FLAGS_MORE_RESULTS_EXISTS 12
#define COLUMN_QUERIES_FLAGS_IN_TRANS 13
#define COLUMN_QUERIES_FLAGS_AUTOCOMMIT 14
#define COLUMN_QUERIES_FLAGS_PKT_NR 15
#define COLUMN_QUERIES_FLAGS_CHARACTER_SET_CLIENT 16
#define COLUMN_QUERIES_FLAGS_CHARACTER_SET_RESULTS 17
#define COLUMN_QUERIES_FLAGS_COLLATION_CONNECTION 18
#define COLUMN_QUERIES_FLAGS_LIMIT 19
#define COLUMN_QUERIES_FLAGS_TIME_ZONE 20
#define COLUMN_QUERIES_FLAGS_SQL_MODE 21
#define COLUMN_QUERIES_FLAGS_MAX_SORT_LENGTH 22
#define COLUMN_QUERIES_FLAGS_GROUP_CONCAT_MAX_LEN 23
#define COLUMN_QUERIES_FLAGS_DIV_PRECISION_INCREMENT 24
#define COLUMN_QUERIES_FLAGS_DEFAULT_WEEK_FORMAT 25
#define COLUMN_QUERIES_FLAGS_LC_TIME_NAMES 26


#endif


/* ST_FIELD_INFO is defined in table.h */
/* 
  QUERY BLOCK 

  some fields are null when we have 0 hits
  some fields are null because we don't have QUERY_CACHE_QC_INFO_PLUGIN in sql_cache.h (MariaDB MDEV-4581)
*/
static ST_FIELD_INFO qc_info_fields_queries[]=
{
  {"QUERY_CACHE_ID", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"STATEMENT_SCHEMA", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"STATEMENT_TEXT", MAX_STATEMENT_TEXT_LENGTH, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"RESULT_FOUND_ROWS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
#ifdef QUERY_CACHE_QC_INFO_PLUGIN
  {"QUERY_ROWS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"SELECT_ROWS_READ", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"QUERY_HITS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"QUERY_HITS_PERIOD_LOW", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"QUERY_HITS_PERIOD_HIGH", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"QUERY_HITS_PERIOD_OUTLIERS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"QUERY_HITS_TOTAL_TIME_US", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"QUERY_HITS_MEAN_PERIOD_US", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 1, 0, 0},
  {"QUERY_HITS_MEAN_PERIOD_LOW_US", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 1, 0, 0},
  {"QUERY_HITS_MEAN_PERIOD_HIGH_US", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 1, 0, 0},
  {"QUERY_INSERT_TIME", 100 * (MY_INT64_NUM_DECIMAL_DIGITS + 1) + 6, MYSQL_TYPE_DECIMAL, 0, 0, 0, 0},
  {"QUERY_LAST_HIT_TIME", 100 * (MY_INT64_NUM_DECIMAL_DIGITS + 1) + 6, MYSQL_TYPE_DECIMAL, 0, 1, 0, 0},
  {"SELECT_EXPEND_TIME_US", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"SELECT_LOCK_TIME_US", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
#endif
  {"TABLES_TYPE", MAX_STATEMENT_TEXT_LENGTH, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"RESULT_LENGTH", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"RESULT_BLOCKS_COUNT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"RESULT_BLOCKS_SIZE", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"RESULT_BLOCKS_SIZE_USED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"FLAGS_CLIENT_LONG_FLAG", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"FLAGS_CLIENT_PROTOCOL_41", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"FLAGS_PROTOCOL_TYPE", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"FLAGS_MORE_RESULTS_EXISTS", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"FLAGS_IN_TRANS", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"FLAGS_AUTOCOMMIT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"FLAGS_PKT_NR", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"FLAGS_CHARACTER_SET_CLIENT", MAX_STATEMENT_TEXT_LENGTH, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"FLAGS_CHARACTER_SET_RESULTS", MAX_STATEMENT_TEXT_LENGTH, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"FLAGS_COLLATION_CONNECTION", MAX_STATEMENT_TEXT_LENGTH, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"FLAGS_LIMIT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"FLAGS_TIME_ZONE", MAX_STATEMENT_TEXT_LENGTH, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"FLAGS_SQL_MODE", MAX_STATEMENT_TEXT_LENGTH, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"FLAGS_MAX_SORT_LENGTH", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"FLAGS_GROUP_CONCAT_MAX_LEN", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"FLAGS_DIV_PRECISION_INCREMENT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"FLAGS_DEFAULT_WEEK_FORMAT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"FLAGS_LC_TIME_NAMES", MAX_STATEMENT_TEXT_LENGTH, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}
};
static ST_FIELD_INFO qc_info_fields_queries_tables[]=
{
  {"QUERY_CACHE_ID", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"SCHEMA", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"TABLE", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}
};

static int qc_info_fill_table_queries(THD *thd, TABLE_LIST *tables, COND *cond){
  int status=1; /* return 1 if can't add row to *table */
  CHARSET_INFO *scs= system_charset_info;
  TABLE *table= tables->table;
  HASH *queries = qc->get_queries();

  /* one must have PROCESS privilege to see others' queries */
  if (check_global_access(thd, PROCESS_ACL, true))
    return 0;

  if (qc->try_lock(thd))
    return 0;

  /* loop through all queries in the query cache */
  for (uint i= 0; i < queries->records; i++)
  {
    const uchar *query_cache_block_raw;
    Query_cache_block* query_cache_block;
    Query_cache_query* query_cache_query;
    uint result_blocks_count;
    ulonglong result_blocks_size;
    ulonglong result_blocks_size_used;
    Query_cache_block *first_result_block;
    Query_cache_block *result_block;
    const char *statement_text;
    size_t statement_text_length;
    char *key, *db;
    size_t key_length, db_length;
    Query_cache_query_flags flags;
    LEX_STRING sql_mode;
    char str[MAX_STATEMENT_TEXT_LENGTH];

    query_cache_block_raw = my_hash_element(queries, i);
    query_cache_block = (Query_cache_block*)query_cache_block_raw;
    if (query_cache_block->type != Query_cache_block::QUERY)
      continue;

    query_cache_query = query_cache_block->query();
    
    /* rows */
    table->field[COLUMN_QUERIES_QC_ID]->store((long)i + 1, 0);
    table->field[COLUMN_QUERIES_RESULT_FOUND_ROWS]->store(query_cache_query->found_rows(), 0);
#ifdef QUERY_CACHE_QC_INFO_PLUGIN
    table->field[COLUMN_QUERIES_QUERY_ROWS]->store(query_cache_query->query_rows_sent(), 0);                              /* qc_info header */
    table->field[COLUMN_QUERIES_QUERY_HITS]->store(query_cache_query->query_hits(), 0);                                   /* qc_info header */
    table->field[COLUMN_QUERIES_RESULT_LENGTH]->store(query_cache_query->length(), 0);
    table->field[COLUMN_QUERIES_SELECT_EXPEND_TIME_US]->store(query_cache_query->query_expend_time(),0);                  /* qc_info header */
    table->field[COLUMN_QUERIES_SELECT_LOCK_TIME_US]->store(query_cache_query->query_lock_time(),0);                      /* qc_info header */
    table->field[COLUMN_QUERIES_SELECT_ROWS_READ]->store(query_cache_query->query_rows_read(), 0);                        /* qc_info header */
    table->field[COLUMN_QUERIES_QUERY_HITS_PERIOD_LOW]->store(query_cache_query->query_hits_low(),0);                     /* qc_info header */
    table->field[COLUMN_QUERIES_QUERY_HITS_PERIOD_HIGH]->store(query_cache_query->query_hits_high(),0);                   /* qc_info header */
    table->field[COLUMN_QUERIES_QUERY_HITS_PERIOD_OUTLIERS]->store(query_cache_query->query_hits_outlier(),0);            /* qc_info header */
    table->field[COLUMN_QUERIES_QUERY_HITS_TOTAL_TIME_US]->store(query_cache_query->query_hits_total_time(),0);           /* qc_info header */
    if (query_cache_query->query_freq_mean_time()<0) {
      table->field[COLUMN_QUERIES_QUERY_HITS_MEAN_PERIOD_US]->set_null();
    } else {
      table->field[COLUMN_QUERIES_QUERY_HITS_MEAN_PERIOD_US]->store(query_cache_query->query_freq_mean_time(),0);           /* qc_info header */
      table->field[COLUMN_QUERIES_QUERY_HITS_MEAN_PERIOD_US]->set_notnull();
    }
    if (query_cache_query->query_freq_mean_low_time()<0) {
      table->field[COLUMN_QUERIES_QUERY_HITS_MEAN_PERIOD_LOW_US]->set_null();
    } else {
      table->field[COLUMN_QUERIES_QUERY_HITS_MEAN_PERIOD_LOW_US]->store(query_cache_query->query_freq_mean_low_time(),0);   /* qc_info header */
      table->field[COLUMN_QUERIES_QUERY_HITS_MEAN_PERIOD_LOW_US]->set_notnull();
    }
    if (query_cache_query->query_freq_mean_high_time()<0) {
      table->field[COLUMN_QUERIES_QUERY_HITS_MEAN_PERIOD_HIGH_US]->set_null();
    } else {
      table->field[COLUMN_QUERIES_QUERY_HITS_MEAN_PERIOD_HIGH_US]->store(query_cache_query->query_freq_mean_high_time(),0); /* qc_info header */
      table->field[COLUMN_QUERIES_QUERY_HITS_MEAN_PERIOD_HIGH_US]->set_notnull();
    }
    if (query_cache_query->query_hits_last_time()<=0) {
      table->field[COLUMN_QUERIES_QUERY_LAST_HIT_TIME]->set_null();
    } else {
      table->field[COLUMN_QUERIES_QUERY_LAST_HIT_TIME]->store(
        ((double)query_cache_query->query_hits_last_time()) / HRTIME_RESOLUTION                                     /* qc_info header */
      );
      table->field[COLUMN_QUERIES_QUERY_LAST_HIT_TIME]->set_notnull();
    }
    table->field[COLUMN_QUERIES_QUERY_INSERT_TIME]->store(
      ((double)query_cache_query->query_insert_time()) / HRTIME_RESOLUTION                                        /* qc_info header */
    );
#endif
    str[0]=0;
    if((query_cache_query->tables_type() & HA_CACHE_TBL_NONTRANSACT) == HA_CACHE_TBL_NONTRANSACT){
      /* every body is nontransact since HA_CACHE_TBL_NONTRANSACT == 0 */
      if(strlen(str)>0)
        strncat(str, ", ", MAX_STATEMENT_TEXT_LENGTH-strlen(str)-1);
      strncat(str, "NON TRANSACT", MAX_STATEMENT_TEXT_LENGTH-strlen(str)-1);
    }
    if((query_cache_query->tables_type() & HA_CACHE_TBL_NOCACHE) == HA_CACHE_TBL_NOCACHE){
      if(strlen(str)>0)
        strncat(str, ", ", MAX_STATEMENT_TEXT_LENGTH-strlen(str)-1);
      strncat(str, "NO CACHE", MAX_STATEMENT_TEXT_LENGTH-strlen(str)-1);
    }
    if((query_cache_query->tables_type() & HA_CACHE_TBL_ASKTRANSACT) == HA_CACHE_TBL_ASKTRANSACT){
      if(strlen(str)>0)
        strncat(str, ", ", MAX_STATEMENT_TEXT_LENGTH-strlen(str)-1);
      strncat(str, "ASK TRANSACT", MAX_STATEMENT_TEXT_LENGTH-strlen(str)-1);
    }
    if((query_cache_query->tables_type() & HA_CACHE_TBL_TRANSACT) == HA_CACHE_TBL_TRANSACT){
      if(strlen(str)>0)
        strncat(str, ", ", MAX_STATEMENT_TEXT_LENGTH-strlen(str)-1);
      strncat(str, "TRANSACT", MAX_STATEMENT_TEXT_LENGTH-strlen(str)-1);
    }
    if((query_cache_query->tables_type() >> 3) != 0 ){
      if(strlen(str)>0)
        strncat(str, ", ", MAX_STATEMENT_TEXT_LENGTH-strlen(str)-1);
      sprintf(str, "%sUNKNOWN %u", 
              str, query_cache_query->tables_type());
    }
    table->field[COLUMN_QUERIES_TABLES_TYPE]->store(str, strlen(str), scs);
    str[0]=0;

    /* Key is query + database + flag */
    /* Get the actual SQL statement for this query cache query */
    statement_text = (const char*)query_cache_query->query();
    statement_text_length = strlen(statement_text);
    /* We truncate SQL statements up to MAX_STATEMENT_TEXT_LENGTH in our I_S table */
    table->field[COLUMN_QUERIES_STATEMENT_TEXT]->store((char*)statement_text,
           min(statement_text_length, MAX_STATEMENT_TEXT_LENGTH), scs);

    /* get the entire key that identifies this query cache query */
    key = (char*)query_cache_query_get_key(query_cache_block_raw,
                                                 &key_length, 0);
                                                 
    /* Flags */
    key_length-= QUERY_CACHE_FLAGS_SIZE;                  // Point at flags
    memcpy( &flags, key+key_length, QUERY_CACHE_FLAGS_SIZE);
    table->field[COLUMN_QUERIES_FLAGS_CLIENT_LONG_FLAG]->store(flags.client_long_flag, 0);
    table->field[COLUMN_QUERIES_FLAGS_CLIENT_PROTOCOL_41]->store(flags.client_protocol_41, 0);
    table->field[COLUMN_QUERIES_FLAGS_PROTOCOL_TYPE]->store(flags.protocol_type, 0);
    table->field[COLUMN_QUERIES_FLAGS_MORE_RESULTS_EXISTS]->store(flags.more_results_exists, 0);
    table->field[COLUMN_QUERIES_FLAGS_IN_TRANS]->store(flags.in_trans, 0);
    table->field[COLUMN_QUERIES_FLAGS_AUTOCOMMIT]->store(flags.autocommit, 0);
    table->field[COLUMN_QUERIES_FLAGS_PKT_NR]->store(flags.pkt_nr, 0);
    table->field[COLUMN_QUERIES_FLAGS_LIMIT]->store(flags.limit, 0);

    CHARSET_INFO *cs1=get_charset(flags.character_set_client_num,MYF(0));
    CHARSET_INFO *cs2=get_charset(flags.character_set_results_num,MYF(0));
    CHARSET_INFO *cs3=get_charset(flags.collation_connection_num,MYF(0));
    if (cs1 && cs1->csname) {
      table->field[COLUMN_QUERIES_FLAGS_CHARACTER_SET_CLIENT]->store(
        cs1->csname,
        min(strlen(cs1->csname), MAX_STATEMENT_TEXT_LENGTH), 
        scs
      );
    } else {
      table->field[COLUMN_QUERIES_FLAGS_CHARACTER_SET_CLIENT]->store("",0, scs);
    }
    if (cs2 && cs2->csname) {
      table->field[COLUMN_QUERIES_FLAGS_CHARACTER_SET_RESULTS]->store(
        cs2->csname,
        min(strlen(cs2->csname), MAX_STATEMENT_TEXT_LENGTH),
        scs
      );
    } else {
      table->field[COLUMN_QUERIES_FLAGS_CHARACTER_SET_RESULTS]->store("",0, scs);
    }
    if (cs3 && cs3->name) {
      table->field[COLUMN_QUERIES_FLAGS_COLLATION_CONNECTION]->store(
        cs3->name, 
        min(strlen(cs3->name), MAX_STATEMENT_TEXT_LENGTH),
        scs
      );
    } else {
      table->field[COLUMN_QUERIES_FLAGS_COLLATION_CONNECTION]->store("",0, scs);
    }
    const String *tz_name;
    tz_name=flags.time_zone->get_name();    
    table->field[COLUMN_QUERIES_FLAGS_TIME_ZONE]->store(
      tz_name->ptr(),
      min(tz_name->length(), MAX_STATEMENT_TEXT_LENGTH),
      scs
    );
    if (!sql_mode_string_representation(thd, flags.sql_mode, &sql_mode)){
      table->field[COLUMN_QUERIES_FLAGS_SQL_MODE]->store(
        sql_mode.str,
        min(sql_mode.length, MAX_STATEMENT_TEXT_LENGTH),
        scs
      );
    }else{
      table->field[COLUMN_QUERIES_FLAGS_SQL_MODE]->store("",0, scs);
    }
    table->field[COLUMN_QUERIES_FLAGS_LC_TIME_NAMES]->store(
      flags.lc_time_names->name,
      min(strlen(flags.lc_time_names->name), MAX_STATEMENT_TEXT_LENGTH),
      scs
    );
    table->field[COLUMN_QUERIES_FLAGS_MAX_SORT_LENGTH]->store(flags.max_sort_length, 0);
    table->field[COLUMN_QUERIES_FLAGS_GROUP_CONCAT_MAX_LEN]->store(flags.group_concat_max_len, 0);
    table->field[COLUMN_QUERIES_FLAGS_DIV_PRECISION_INCREMENT]->store(flags.div_precision_increment, 0);
    table->field[COLUMN_QUERIES_FLAGS_DEFAULT_WEEK_FORMAT]->store(flags.default_week_format, 0);
    memcpy(key + key_length, &flags, QUERY_CACHE_FLAGS_SIZE); // restore flags

    /* The database against which the statement is executed is part of the
       query cache query key
     */
    compile_time_assert(QUERY_CACHE_DB_LENGTH_SIZE == 2);
    db= key + statement_text_length + 1 + QUERY_CACHE_DB_LENGTH_SIZE;
    db_length= uint2korr(db - QUERY_CACHE_DB_LENGTH_SIZE);

    table->field[COLUMN_QUERIES_STATEMENT_SCHEMA]->store(db, db_length, scs);

    /* If we have result blocks, process them */
    first_result_block= query_cache_query->result();
    if (first_result_block) {
      /* initialize so we can loop over the result blocks*/
      result_block= first_result_block;
      result_blocks_count = 1;
      result_blocks_size = result_block->length;
      result_blocks_size_used = result_block->used;

      /* loop over the result blocks*/
      while ((result_block= result_block->next)!=first_result_block)
      {
        /* calculate total number of result blocks */
        result_blocks_count++;
        /* calculate total size of result blocks */
        result_blocks_size += result_block->length;
        /* calculate total of used size of result blocks */
        result_blocks_size_used += result_block->used;
      }
    } else {
      result_blocks_count = 0;
      result_blocks_size = 0;
      result_blocks_size_used = 0;
    }


    table->field[COLUMN_QUERIES_RESULT_BLOCKS_COUNT]->store(result_blocks_count, 0);
    table->field[COLUMN_QUERIES_RESULT_BLOCKS_SIZE]->store(result_blocks_size, 0);
    table->field[COLUMN_QUERIES_RESULT_BLOCKS_SIZE_USED]->store(result_blocks_size_used, 0);

    if (schema_table_store_record(thd, table))
      goto cleanup;
  }
  status=0;
cleanup:
  qc->unlock();
  return status;
}

static int qc_info_fill_table_queries_tables(THD *thd, TABLE_LIST *tables, COND *cond){
  int status=1; /* return 1 if can't add row to *table */
  CHARSET_INFO *scs= system_charset_info;
  TABLE *table= tables->table;
  HASH *queries = qc->get_queries();

  /* one must have PROCESS privilege to see others' queries */
  if (check_global_access(thd, PROCESS_ACL, true))
    return 0;

  if (qc->try_lock(thd))
    return 0;

  /* loop through all queries in the query cache */
  for (uint i= 0; i < queries->records; i++)
  {
    const uchar *query_cache_block_raw;
    Query_cache_block* query_cache_block;
    
    query_cache_block_raw = my_hash_element(queries, i);
    query_cache_block = (Query_cache_block*)query_cache_block_raw;
    if (query_cache_block->type != Query_cache_block::QUERY)
      continue;

    /* rows */
    for (TABLE_COUNTER_TYPE t= 0; t < query_cache_block->n_tables; t++)
    {
      Query_cache_table *tmp_table= query_cache_block->table(t)->parent;
      table->field[0]->store((long)i + 1, 0);
      table->field[1]->store(tmp_table->db(),strlen(tmp_table->db()),scs);
      table->field[2]->store(tmp_table->table(),strlen(tmp_table->table()),scs);
      if (schema_table_store_record(thd, table))
        goto cleanup;
    }
  }
  status=0;
cleanup:
  qc->unlock();
  return status;
}



/* 
  TABLES BLOCK 
*/
/* table tables */
static ST_FIELD_INFO qc_info_fields_tables[]=
{
  {"TABLE_SCHEMA", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"TABLE_NAME", MAX_STATEMENT_TEXT_LENGTH, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"TABLE_HASHED", MAX_TINYINT_WIDTH, MYSQL_TYPE_TINY, 0, 0, 0, 0},
  {"TABLE_TYPE", MAX_STATEMENT_TEXT_LENGTH, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"QUERIES_IN_CACHE", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}
};

static int qc_info_fill_table_tables(THD *thd, TABLE_LIST *tables,
                                              COND *cond)
{
  int status = 1;
  CHARSET_INFO *scs= system_charset_info;
  TABLE *table= tables->table;
  HASH *qc_tables = qc->get_tables();

  /* one must have PROCESS privilege to see others' queries */
  if (check_global_access(thd, PROCESS_ACL, true))
    return 0;

  if (qc->try_lock(thd))
    return 0;

  /* loop through all queries in the query cache */
  for (uint i= 0; i < qc_tables->records; i++)
  {
    const uchar *query_cache_block_raw;
    Query_cache_block* query_cache_block;
    Query_cache_table* query_cache_table_entry;
    char *table_name, *db_name, table_type_unknown_buffer[15];
    size_t table_name_length, db_name_length;

    query_cache_block_raw = my_hash_element(qc_tables, i);
    query_cache_block = (Query_cache_block*)query_cache_block_raw;
    if (query_cache_block->type != Query_cache_block::TABLE)
      continue;
    query_cache_table_entry = query_cache_block->table();

    table_name=query_cache_table_entry->table();
    table_name_length=strlen(table_name);
    db_name=query_cache_table_entry->db();
    db_name_length=strlen(db_name);


    table->field[0]->store(db_name, db_name_length, scs);
    table->field[1]->store(table_name, table_name_length, scs);
    table->field[2]->store(query_cache_table_entry->is_hashed(), 0 );
    if(query_cache_table_entry->table_type==HA_CACHE_TBL_NONTRANSACT)
      table->field[3]->store("NON_TRANSACT", 12, scs);
    else if(query_cache_table_entry->table_type==HA_CACHE_TBL_NOCACHE)
      table->field[3]->store("NO_CACHE", 8, scs);
    else if(query_cache_table_entry->table_type==HA_CACHE_TBL_ASKTRANSACT)
      table->field[3]->store("ASK_TRANSACT", 12, scs);
    else if(query_cache_table_entry->table_type==HA_CACHE_TBL_TRANSACT)
      table->field[3]->store("TRANSACT", 8, scs);
    else{
      sprintf(table_type_unknown_buffer, "UNKNOWN %u", 
              query_cache_table_entry->table_type);
      table->field[3]->store(table_type_unknown_buffer, 
                             strlen(table_type_unknown_buffer), 
           scs);
    }
    table->field[4]->store(query_cache_table_entry->m_cached_query_count, 0 );
    if (schema_table_store_record(thd, table))
      goto cleanup;
  }
  status=0;
cleanup:
  qc->unlock();
  return status;
}

/* 
  PLUGIN DECLARATIONS AND INITS 
*/
static int qc_info_plugin_init_queries_tables(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;
  schema->fields_info= qc_info_fields_queries_tables;
  schema->fill_table= qc_info_fill_table_queries_tables;

#ifdef _WIN32
  qc = (Accessible_Query_Cache *)
    GetProcAddress(GetModuleHandle(NULL), "?query_cache@@3VQuery_cache@@A");
#else
  qc = (Accessible_Query_Cache *)&query_cache;
#endif

  return qc == 0;
}
static int qc_info_plugin_init_queries(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;
  schema->fields_info= qc_info_fields_queries;
  schema->fill_table= qc_info_fill_table_queries;

#ifdef _WIN32
  qc = (Accessible_Query_Cache *)
    GetProcAddress(GetModuleHandle(NULL), "?query_cache@@3VQuery_cache@@A");
#else
  qc = (Accessible_Query_Cache *)&query_cache;
#endif

  return qc == 0;
}
static int qc_info_plugin_init_tables(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;
  schema->fields_info= qc_info_fields_tables;
  schema->fill_table= qc_info_fill_table_tables;
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
  MYSQL_INFORMATION_SCHEMA_PLUGIN,      /* the plugin type (see include/mysql/plugin.h) */
  &qc_info_plugin,                      /* pointer to type-specific plugin descriptor   */
  "QUERY_CACHE_QUERIES",                /* plugin name */
  "Roland Bouman / Roberto Spadim, SPAEmpresarial - Brazil",     /* plugin author */
  "Lists all queries in the query cache.", /* the plugin description */
  PLUGIN_LICENSE_BSD,                   /* the plugin license (see include/mysql/plugin.h) */
  qc_info_plugin_init_queries,          /* Pointer to plugin initialization function */
  0,                                    /* Pointer to plugin deinitialization function */
  0x0101,                               /* Numeric version 0xAABB means AA.BB veriosn */
  NULL,                                 /* Status variables */
  NULL,                                 /* System variables */
  "1.1",                                /* String version representation */
  MariaDB_PLUGIN_MATURITY_ALPHA         /* Maturity (see include/mysql/plugin.h)*/
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,      /* the plugin type (see include/mysql/plugin.h) */
  &qc_info_plugin,                      /* pointer to type-specific plugin descriptor   */
  "QUERY_CACHE_QUERIES_TABLES",         /* plugin name */
  "Roberto Spadim, SPAEmpresarial - Brazil",     /* plugin author */
  "Relation between query cache query and tables used.", /* the plugin description */
  PLUGIN_LICENSE_BSD,                   /* the plugin license (see include/mysql/plugin.h) */
  qc_info_plugin_init_queries_tables,   /* Pointer to plugin initialization function */
  0,                                    /* Pointer to plugin deinitialization function */
  0x0101,                               /* Numeric version 0xAABB means AA.BB veriosn */
  NULL,                                 /* Status variables */
  NULL,                                 /* System variables */
  "1.1",                                /* String version representation */
  MariaDB_PLUGIN_MATURITY_ALPHA         /* Maturity (see include/mysql/plugin.h)*/
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,      /* the plugin type (see include/mysql/plugin.h) */
  &qc_info_plugin,                      /* pointer to type-specific plugin descriptor   */
  "QUERY_CACHE_TABLES",                 /* plugin name */
  "Roberto Spadim, SPAEmpresarial - Brazil",                     /* plugin author */
  "Lists all table in the query cache.",/* the plugin description */
  PLUGIN_LICENSE_BSD,                   /* the plugin license (see include/mysql/plugin.h) */
  qc_info_plugin_init_tables,           /* Pointer to plugin initialization function */
  0,                                    /* Pointer to plugin deinitialization function */
  0x0101,                               /* Numeric version 0xAABB means AA.BB veriosn */
  NULL,                                 /* Status variables */
  NULL,                                 /* System variables */
  "1.1",                                /* String version representation */
  MariaDB_PLUGIN_MATURITY_ALPHA         /* Maturity (see include/mysql/plugin.h)*/
}
maria_declare_plugin_end;
