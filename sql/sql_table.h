/* Copyright (c) 2006, 2014, Oracle and/or its affiliates.
   Copyright (c) 2011, 2017, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef SQL_TABLE_INCLUDED
#define SQL_TABLE_INCLUDED

#include <my_sys.h>                             // pthread_mutex_t
#include "m_string.h"                           // LEX_CUSTRING
#include "lex_charset.h"

#define ERROR_INJECT(code) \
  ((DBUG_IF("crash_" code) && (DBUG_SUICIDE(), 0)) || \
   (DBUG_IF("fail_" code) && (my_error(ER_UNKNOWN_ERROR, MYF(0)), 1)))

class Alter_info;
class Alter_table_ctx;
class Column_definition;
class Create_field;
struct TABLE_LIST;
class THD;
struct TABLE;
struct handlerton;
class handler;
class String;
typedef struct st_ha_check_opt HA_CHECK_OPT;
struct HA_CREATE_INFO;
struct Table_specification_st;
typedef struct st_key KEY;
typedef struct st_key_cache KEY_CACHE;
typedef struct st_lock_param_type ALTER_PARTITION_PARAM_TYPE;
typedef struct st_order ORDER;
typedef struct st_ddl_log_state DDL_LOG_STATE;

enum enum_explain_filename_mode
{
  EXPLAIN_ALL_VERBOSE= 0,
  EXPLAIN_PARTITIONS_VERBOSE,
  EXPLAIN_PARTITIONS_AS_COMMENT
};


/* depends on errmsg.txt Database `db`, Table `t` ... */
#define EXPLAIN_FILENAME_MAX_EXTRA_LENGTH 63

#define WFRM_WRITE_SHADOW 1
#define WFRM_INSTALL_SHADOW 2
#define WFRM_KEEP_SHARE 4
#define WFRM_WRITE_CONVERTED_TO 8
#define WFRM_BACKUP_ORIGINAL 16

/* Flags for conversion functions. */
static const uint FN_FROM_IS_TMP=  1 << 0;
static const uint FN_TO_IS_TMP=    1 << 1;
static const uint FN_IS_TMP=       FN_FROM_IS_TMP | FN_TO_IS_TMP;
static const uint NO_FRM_RENAME=   1 << 2;
static const uint FRM_ONLY=        1 << 3;
/** Don't remove table in engine. Remove only .FRM and maybe .PAR files. */
static const uint NO_HA_TABLE=     1 << 4;
/** Don't resolve MySQL's fake "foo.sym" symbolic directory names. */
static const uint SKIP_SYMDIR_ACCESS= 1 << 5;
/** Don't check foreign key constraints while renaming table */
static const uint NO_FK_CHECKS=    1 << 6;
/* Don't delete .par table in quick_rm_table() */
static const uint NO_PAR_TABLE=   1 << 7;

uint filename_to_tablename(const char *from, char *to, size_t to_length,
                           bool stay_quiet = false);
uint tablename_to_filename(const char *from, char *to, size_t to_length);
uint check_n_cut_mysql50_prefix(const char *from, char *to, size_t to_length);
bool check_mysql50_prefix(const char *name);
uint build_table_filename(char *buff, size_t bufflen, const char *db,
                          const char *table, const char *ext, uint flags);
uint build_table_shadow_filename(char *buff, size_t bufflen,
                                 ALTER_PARTITION_PARAM_TYPE *lpt,
                                 bool backup= false);
void build_lower_case_table_filename(char *buff, size_t bufflen,
                                     const LEX_CSTRING *db,
                                     const LEX_CSTRING *table,
                                     uint flags);
uint build_tmptable_filename(THD* thd, char *buff, size_t bufflen);
bool add_keyword_to_query(THD *thd, String *result, const LEX_CSTRING *keyword,
                          const LEX_CSTRING *add);

/*
  mysql_create_table_no_lock can be called in one of the following
  mutually exclusive situations:

  - Just a normal ordinary CREATE TABLE statement that explicitly
    defines the table structure.

  - CREATE TABLE ... SELECT. It is special, because only in this case,
    the list of fields is allowed to have duplicates, as long as one of the
    duplicates comes from the select list, and the other doesn't. For
    example in

       CREATE TABLE t1 (a int(5) NOT NUL) SELECT b+10 as a FROM t2;

    the list in alter_info->create_list will have two fields `a`.

  - ALTER TABLE, that creates a temporary table #sql-xxx, which will be later
    renamed to replace the original table.

  - ALTER TABLE as above, but which only modifies the frm file, it only
    creates an frm file for the #sql-xxx, the table in the engine is not
    created.

  - Assisted discovery, CREATE TABLE statement without the table structure.

  These situations are distinguished by the following "create table mode"
  values, where a CREATE ... SELECT is denoted by any non-negative number
  (which should be the number of fields in the SELECT ... part), and other
  cases use constants as defined below.
*/
#define C_CREATE_SELECT(X)        ((X) > 0 ? (X) : 0)
#define C_ORDINARY_CREATE         0
#define C_ALTER_TABLE            -1
#define C_ALTER_TABLE_FRM_ONLY   -2
#define C_ASSISTED_DISCOVERY     -3

int mysql_create_table_no_lock(THD *thd,
                               DDL_LOG_STATE *ddl_log_state,
                               DDL_LOG_STATE *ddl_log_state_rm,
                               Table_specification_st *create_info,
                               Alter_info *alter_info, bool *is_trans,
                               int create_table_mode, TABLE_LIST *table);

handler *mysql_create_frm_image(THD *thd, HA_CREATE_INFO *create_info,
                                Alter_info *alter_info, int create_table_mode,
                                KEY **key_info, uint *key_count,
                                LEX_CUSTRING *frm);

int mysql_discard_or_import_tablespace(THD *thd, TABLE_LIST *table_list,
                                       bool discard);

bool mysql_prepare_alter_table(THD *thd, TABLE *table,
                               Table_specification_st *create_info,
                               Alter_info *alter_info,
                               Alter_table_ctx *alter_ctx);
bool mysql_trans_prepare_alter_copy_data(THD *thd);
bool mysql_trans_commit_alter_copy_data(THD *thd);
bool mysql_alter_table(THD *thd, const LEX_CSTRING *new_db,
                       const LEX_CSTRING *new_name,
                       Table_specification_st *create_info,
                       TABLE_LIST *table_list,
                       class Recreate_info *recreate_info,
                       Alter_info *alter_info,
                       uint order_num, ORDER *order, bool ignore,
                       bool if_exists);
bool mysql_compare_tables(TABLE *table,
                          Alter_info *alter_info,
                          HA_CREATE_INFO *create_info,
                          bool *metadata_equal);
bool mysql_recreate_table(THD *thd, TABLE_LIST *table_list,
                          class Recreate_info *recreate_info, bool table_copy);
bool mysql_rename_table(handlerton *base, const LEX_CSTRING *old_db,
                        const LEX_CSTRING *old_name, const LEX_CSTRING *new_db,
                        const LEX_CSTRING *new_name, LEX_CUSTRING *id,
                        uint flags);
bool mysql_backup_table(THD* thd, TABLE_LIST* table_list);
bool mysql_restore_table(THD* thd, TABLE_LIST* table_list);

template<typename T> class List;
void fill_checksum_table_metadata_fields(THD *thd, List<Item> *fields);
bool mysql_checksum_table(THD* thd, TABLE_LIST* table_list,
                          HA_CHECK_OPT* check_opt);
bool mysql_rm_table(THD *thd,TABLE_LIST *tables, bool if_exists,
                    bool drop_temporary, bool drop_sequence,
                    bool dont_log_query);
int mysql_rm_table_no_locks(THD *thd, TABLE_LIST *tables,
                            const LEX_CSTRING *db,
                            DDL_LOG_STATE *ddl_log_state,
                            bool if_exists,
                            bool drop_temporary, bool drop_view,
                            bool drop_sequence,
                            bool dont_log_query, bool dont_free_locks);
bool log_drop_table(THD *thd, const LEX_CSTRING *db_name,
                    const LEX_CSTRING *table_name, const LEX_CSTRING *handler,
                    bool partitioned, const LEX_CUSTRING *id,
                    bool temporary_table);
bool quick_rm_table(THD *thd, handlerton *base, const LEX_CSTRING *db,
                    const LEX_CSTRING *table_name, uint flags,
                    const char *table_path=0);
void close_cached_table(THD *thd, TABLE *table);
void sp_prepare_create_field(THD *thd, Column_definition *sql_field);
bool mysql_write_frm(ALTER_PARTITION_PARAM_TYPE *lpt, uint flags);
int write_bin_log(THD *thd, bool clear_error,
                  char const *query, ulong query_length,
                  bool is_trans= FALSE);
int write_bin_log_with_if_exists(THD *thd, bool clear_error,
                                 bool is_trans, bool add_if_exists,
                                 bool commit_alter= false);

void promote_first_timestamp_column(List<Create_field> *column_definitions);

/*
  These prototypes where under INNODB_COMPATIBILITY_HOOKS.
*/
uint explain_filename(THD* thd, const char *from, char *to, uint to_length,
                      enum_explain_filename_mode explain_mode);


extern MYSQL_PLUGIN_IMPORT const LEX_CSTRING primary_key_name;

bool check_engine(THD *, const char *, const char *, HA_CREATE_INFO *);

#ifdef WITH_WSREP
bool wsrep_check_sequence(THD* thd, const class sequence_definition *seq);
#endif

#endif /* SQL_TABLE_INCLUDED */
