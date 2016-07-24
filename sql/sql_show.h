/* Copyright (c) 2005, 2010, Oracle and/or its affiliates.
   Copyright (c) 2012, 2016, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef SQL_SHOW_H
#define SQL_SHOW_H

#include "sql_list.h"                           /* List */
#include "handler.h"                            /* enum_schema_tables */
#include "table.h"                              /* enum_schema_table_state */
#include "my_apc.h"

/* Forward declarations */
class JOIN;
class String;
class THD;
class sp_name;
struct TABLE_LIST;
typedef class st_select_lex SELECT_LEX;
struct LEX;
typedef struct st_mysql_show_var SHOW_VAR;
typedef struct st_schema_table ST_SCHEMA_TABLE;
struct TABLE;
typedef struct system_status_var STATUS_VAR;

/* Used by handlers to store things in schema tables */
#define IS_FILES_FILE_ID              0
#define IS_FILES_FILE_NAME            1
#define IS_FILES_FILE_TYPE            2
#define IS_FILES_TABLESPACE_NAME      3
#define IS_FILES_TABLE_CATALOG        4
#define IS_FILES_TABLE_SCHEMA         5
#define IS_FILES_TABLE_NAME           6
#define IS_FILES_LOGFILE_GROUP_NAME   7
#define IS_FILES_LOGFILE_GROUP_NUMBER 8
#define IS_FILES_ENGINE               9
#define IS_FILES_FULLTEXT_KEYS       10
#define IS_FILES_DELETED_ROWS        11
#define IS_FILES_UPDATE_COUNT        12
#define IS_FILES_FREE_EXTENTS        13
#define IS_FILES_TOTAL_EXTENTS       14
#define IS_FILES_EXTENT_SIZE         15
#define IS_FILES_INITIAL_SIZE        16
#define IS_FILES_MAXIMUM_SIZE        17
#define IS_FILES_AUTOEXTEND_SIZE     18
#define IS_FILES_CREATION_TIME       19
#define IS_FILES_LAST_UPDATE_TIME    20
#define IS_FILES_LAST_ACCESS_TIME    21
#define IS_FILES_RECOVER_TIME        22
#define IS_FILES_TRANSACTION_COUNTER 23
#define IS_FILES_VERSION             24
#define IS_FILES_ROW_FORMAT          25
#define IS_FILES_TABLE_ROWS          26
#define IS_FILES_AVG_ROW_LENGTH      27
#define IS_FILES_DATA_LENGTH         28
#define IS_FILES_MAX_DATA_LENGTH     29
#define IS_FILES_INDEX_LENGTH        30
#define IS_FILES_DATA_FREE           31
#define IS_FILES_CREATE_TIME         32
#define IS_FILES_UPDATE_TIME         33
#define IS_FILES_CHECK_TIME          34
#define IS_FILES_CHECKSUM            35
#define IS_FILES_STATUS              36
#define IS_FILES_EXTRA               37

typedef enum { WITHOUT_DB_NAME, WITH_DB_NAME } enum_with_db_name;
int show_create_table(THD *thd, TABLE_LIST *table_list, String *packet,
                      Table_specification_st *create_info_arg,
                      enum_with_db_name with_db_name);

int copy_event_to_schema_table(THD *thd, TABLE *sch_table, TABLE *event_table);

bool append_identifier(THD *thd, String *packet, const char *name,
		       uint length);
void mysqld_list_fields(THD *thd,TABLE_LIST *table, const char *wild);
int mysqld_dump_create_info(THD *thd, TABLE_LIST *table_list, int fd);
bool mysqld_show_create_get_fields(THD *thd, TABLE_LIST *table_list,
                                   List<Item> *field_list, String *buffer);
bool mysqld_show_create(THD *thd, TABLE_LIST *table_list);
void mysqld_show_create_db_get_fields(THD *thd, List<Item> *field_list);
bool mysqld_show_create_db(THD *thd, LEX_STRING *db_name,
                           LEX_STRING *orig_db_name,
                           const DDL_options_st &options);

void mysqld_list_processes(THD *thd,const char *user,bool verbose);
int mysqld_show_status(THD *thd);
int mysqld_show_variables(THD *thd,const char *wild);
bool mysqld_show_storage_engines(THD *thd);
bool mysqld_show_authors(THD *thd);
bool mysqld_show_contributors(THD *thd);
bool mysqld_show_privileges(THD *thd);
char *make_backup_log_name(char *buff, const char *name, const char* log_ext);
uint calc_sum_of_all_status(STATUS_VAR *to);
void append_definer(THD *thd, String *buffer, const LEX_STRING *definer_user,
                    const LEX_STRING *definer_host);
int add_status_vars(SHOW_VAR *list);
void remove_status_vars(SHOW_VAR *list);
void init_status_vars();
void free_status_vars();
void reset_status_vars();
bool show_create_trigger(THD *thd, const sp_name *trg_name);
void view_store_options(THD *thd, TABLE_LIST *table, String *buff);

void init_fill_schema_files_row(TABLE* table);
bool schema_table_store_record(THD *thd, TABLE *table);
void initialize_information_schema_acl();
COND *make_cond_for_info_schema(THD *thd, COND *cond, TABLE_LIST *table);

ST_SCHEMA_TABLE *find_schema_table(THD *thd, const char* table_name, bool *in_plugin);
static inline ST_SCHEMA_TABLE *find_schema_table(THD *thd, const char* table_name)
{ bool unused; return find_schema_table(thd, table_name, &unused); }

ST_SCHEMA_TABLE *get_schema_table(enum enum_schema_tables schema_table_idx);
int make_schema_select(THD *thd,  SELECT_LEX *sel,
                       ST_SCHEMA_TABLE *schema_table);
int mysql_schema_table(THD *thd, LEX *lex, TABLE_LIST *table_list);
bool get_schema_tables_result(JOIN *join,
                              enum enum_schema_table_state executed_place);
enum enum_schema_tables get_schema_table_idx(ST_SCHEMA_TABLE *schema_table);
TABLE *create_schema_table(THD *thd, TABLE_LIST *table_list);

/* These functions were under INNODB_COMPATIBILITY_HOOKS */
int get_quote_char_for_identifier(THD *thd, const char *name, uint length);
THD *find_thread_by_id(longlong id, bool query_id= false);

class select_result_explain_buffer;
/*
  SHOW EXPLAIN request object. 
*/

class Show_explain_request : public Apc_target::Apc_call
{
public:
  THD *target_thd;  /* thd that we're running SHOW EXPLAIN for */
  THD *request_thd; /* thd that run SHOW EXPLAIN command */
  
  /* If true, there was some error when producing EXPLAIN output. */
  bool failed_to_produce;
   
  /* SHOW EXPLAIN will be stored here */
  select_result_explain_buffer *explain_buf;
  
  /* Query that we've got SHOW EXPLAIN for */
  String query_str;
  
  /* Overloaded virtual function */
  void call_in_target_thread();
};

/**
  Condition pushdown used for INFORMATION_SCHEMA / SHOW queries.
  This structure is to implement an optimization when
  accessing data dictionary data in the INFORMATION_SCHEMA
  or SHOW commands.
  When the query contain a TABLE_SCHEMA or TABLE_NAME clause,
  narrow the search for data based on the constraints given.
*/
typedef struct st_lookup_field_values
{
  /**
    Value of a TABLE_SCHEMA clause.
    Note that this value length may exceed @c NAME_LEN.
    @sa wild_db_value
  */
  LEX_STRING db_value;
  /**
    Value of a TABLE_NAME clause.
    Note that this value length may exceed @c NAME_LEN.
    @sa wild_table_value
  */
  LEX_STRING table_value;
  /**
    True when @c db_value is a LIKE clause,
    false when @c db_value is an '=' clause.
  */
  bool wild_db_value;
  /**
    True when @c table_value is a LIKE clause,
    false when @c table_value is an '=' clause.
  */
  bool wild_table_value;
} LOOKUP_FIELD_VALUES;  


/*
  INFORMATION_SCHEMA: Execution plan for get_all_tables() call
*/

class IS_table_read_plan : public Sql_alloc
{
public:
  IS_table_read_plan() : no_rows(false), trivial_show_command(FALSE) {}

  bool no_rows;
  /*
    For EXPLAIN only: For SHOW KEYS and SHOW COLUMNS, we know which
    db_name.table_name will be read, however for some reason we don't
    set the fields in this->lookup_field_vals.
    In order to not have JOIN::save_explain_data() walking over uninitialized
    data, we set trivial_show_command=true.
  */
  bool trivial_show_command;

  LOOKUP_FIELD_VALUES lookup_field_vals;
  Item *partial_cond;

  bool has_db_lookup_value()
  {
    return (lookup_field_vals.db_value.length &&
           !lookup_field_vals.wild_db_value);
  }
  bool has_table_lookup_value()
  {
    return (lookup_field_vals.table_value.length &&
            !lookup_field_vals.wild_table_value);
  }
};

bool optimize_schema_tables_reads(JOIN *join);

/* Handle the ignored database directories list for SHOW/I_S. */
bool ignore_db_dirs_init();
void ignore_db_dirs_free();
void ignore_db_dirs_reset();
bool ignore_db_dirs_process_additions();
bool push_ignored_db_dir(char *path);
extern char *opt_ignore_db_dirs;

#endif /* SQL_SHOW_H */
