/*
   Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2009, 2017, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef SQL_CLASS_INCLUDED
#define SQL_CLASS_INCLUDED

/* Classes in mysql */

#include "my_global.h"                          /* NO_EMBEDDED_ACCESS_CHECKS */
#include "dur_prop.h"
#include <waiting_threads.h>
#include "sql_const.h"
#include <mysql/plugin_audit.h>
#include "log.h"
#include "rpl_tblmap.h"
#include "mdl.h"
#include "field.h"                              // Create_field
#include "probes_mysql.h"
#include "sql_locale.h"     /* my_locale_st */
#include "sql_profile.h"    /* PROFILING */
#include "scheduler.h"      /* thd_scheduler */
#include "protocol.h"       /* Protocol_text, Protocol_binary */
#include "violite.h"        /* vio_is_connected */
#include "thr_lock.h"       /* thr_lock_type, THR_LOCK_DATA, THR_LOCK_INFO */
#include "thr_timer.h"
#include "thr_malloc.h"
#include <my_tree.h>

#include "sql_digest_stream.h"            // sql_digest_state

#include <mysql/psi/mysql_stage.h>
#include <mysql/psi/mysql_statement.h>
#include <mysql/psi/mysql_idle.h>
#include <mysql/psi/mysql_table.h>
#include <mysql_com_server.h>
#include "session_tracker.h"

extern "C"
void set_thd_stage_info(void *thd,
                        const PSI_stage_info *new_stage,
                        PSI_stage_info *old_stage,
                        const char *calling_func,
                        const char *calling_file,
                        const unsigned int calling_line);

#define THD_STAGE_INFO(thd, stage) \
  (thd)->enter_stage(&stage, __func__, __FILE__, __LINE__)

#include "my_apc.h"
#include "rpl_gtid.h"
#include "wsrep_mysqld.h"

class Reprepare_observer;
class Relay_log_info;
struct rpl_group_info;
class Rpl_filter;
class Query_log_event;
class Load_log_event;
class sp_rcontext;
class sp_cache;
class Lex_input_stream;
class Parser_state;
class Rows_log_event;
class Sroutine_hash_entry;
class user_var_entry;
struct Trans_binlog_info;
class rpl_io_thread_info;
class rpl_sql_thread_info;

enum enum_ha_read_modes { RFIRST, RNEXT, RPREV, RLAST, RKEY, RNEXT_SAME };
enum enum_duplicates { DUP_ERROR, DUP_REPLACE, DUP_UPDATE };
enum enum_delay_key_write { DELAY_KEY_WRITE_NONE, DELAY_KEY_WRITE_ON,
			    DELAY_KEY_WRITE_ALL };
enum enum_slave_exec_mode { SLAVE_EXEC_MODE_STRICT,
                            SLAVE_EXEC_MODE_IDEMPOTENT,
                            SLAVE_EXEC_MODE_LAST_BIT };
enum enum_slave_run_triggers_for_rbr { SLAVE_RUN_TRIGGERS_FOR_RBR_NO,
                                       SLAVE_RUN_TRIGGERS_FOR_RBR_YES,
                                       SLAVE_RUN_TRIGGERS_FOR_RBR_LOGGING};
enum enum_slave_type_conversions { SLAVE_TYPE_CONVERSIONS_ALL_LOSSY,
                                   SLAVE_TYPE_CONVERSIONS_ALL_NON_LOSSY};
enum enum_mark_columns
{ MARK_COLUMNS_NONE, MARK_COLUMNS_READ, MARK_COLUMNS_WRITE};
enum enum_filetype { FILETYPE_CSV, FILETYPE_XML };

enum enum_binlog_row_image {
  /** PKE in the before image and changed columns in the after image */
  BINLOG_ROW_IMAGE_MINIMAL= 0,
  /** Whenever possible, before and after image contain all columns except blobs. */
  BINLOG_ROW_IMAGE_NOBLOB= 1,
  /** All columns in both before and after image. */
  BINLOG_ROW_IMAGE_FULL= 2
};


/* Bits for different SQL modes modes (including ANSI mode) */
#define MODE_REAL_AS_FLOAT              (1ULL << 0)
#define MODE_PIPES_AS_CONCAT            (1ULL << 1)
#define MODE_ANSI_QUOTES                (1ULL << 2)
#define MODE_IGNORE_SPACE               (1ULL << 3)
#define MODE_IGNORE_BAD_TABLE_OPTIONS   (1ULL << 4)
#define MODE_ONLY_FULL_GROUP_BY         (1ULL << 5)
#define MODE_NO_UNSIGNED_SUBTRACTION    (1ULL << 6)
#define MODE_NO_DIR_IN_CREATE           (1ULL << 7)
#define MODE_POSTGRESQL                 (1ULL << 8)
#define MODE_ORACLE                     (1ULL << 9)
#define MODE_MSSQL                      (1ULL << 10)
#define MODE_DB2                        (1ULL << 11)
#define MODE_MAXDB                      (1ULL << 12)
#define MODE_NO_KEY_OPTIONS             (1ULL << 13)
#define MODE_NO_TABLE_OPTIONS           (1ULL << 14)
#define MODE_NO_FIELD_OPTIONS           (1ULL << 15)
#define MODE_MYSQL323                   (1ULL << 16)
#define MODE_MYSQL40                    (1ULL << 17)
#define MODE_ANSI                       (1ULL << 18)
#define MODE_NO_AUTO_VALUE_ON_ZERO      (1ULL << 19)
#define MODE_NO_BACKSLASH_ESCAPES       (1ULL << 20)
#define MODE_STRICT_TRANS_TABLES        (1ULL << 21)
#define MODE_STRICT_ALL_TABLES          (1ULL << 22)
#define MODE_NO_ZERO_IN_DATE            (1ULL << 23)
#define MODE_NO_ZERO_DATE               (1ULL << 24)
#define MODE_INVALID_DATES              (1ULL << 25)
#define MODE_ERROR_FOR_DIVISION_BY_ZERO (1ULL << 26)
#define MODE_TRADITIONAL                (1ULL << 27)
#define MODE_NO_AUTO_CREATE_USER        (1ULL << 28)
#define MODE_HIGH_NOT_PRECEDENCE        (1ULL << 29)
#define MODE_NO_ENGINE_SUBSTITUTION     (1ULL << 30)
#define MODE_PAD_CHAR_TO_FULL_LENGTH    (1ULL << 31)

/* Bits for different old style modes */
#define OLD_MODE_NO_DUP_KEY_WARNINGS_WITH_IGNORE	(1 << 0)
#define OLD_MODE_NO_PROGRESS_INFO			(1 << 1)
#define OLD_MODE_ZERO_DATE_TIME_CAST                    (1 << 2)

extern char internal_table_name[2];
extern char empty_c_string[1];
extern LEX_STRING EMPTY_STR;
extern MYSQL_PLUGIN_IMPORT const char **errmesg;

extern bool volatile shutdown_in_progress;

extern "C" LEX_STRING * thd_query_string (MYSQL_THD thd);
extern "C" size_t thd_query_safe(MYSQL_THD thd, char *buf, size_t buflen);

/**
  @class CSET_STRING
  @brief Character set armed LEX_STRING
*/
class CSET_STRING
{
private:
  LEX_STRING string;
  CHARSET_INFO *cs;
public:
  CSET_STRING() : cs(&my_charset_bin)
  {
    string.str= NULL;
    string.length= 0;
  }
  CSET_STRING(char *str_arg, size_t length_arg, CHARSET_INFO *cs_arg) :
  cs(cs_arg)
  {
    DBUG_ASSERT(cs_arg != NULL);
    string.str= str_arg;
    string.length= length_arg;
  }

  inline char *str() const { return string.str; }
  inline size_t length() const { return string.length; }
  CHARSET_INFO *charset() const { return cs; }

  friend LEX_STRING * thd_query_string (MYSQL_THD thd);
};


#define TC_HEURISTIC_RECOVER_COMMIT   1
#define TC_HEURISTIC_RECOVER_ROLLBACK 2
extern ulong tc_heuristic_recover;

typedef struct st_user_var_events
{
  user_var_entry *user_var_event;
  char *value;
  ulong length;
  Item_result type;
  uint charset_number;
  bool unsigned_flag;
} BINLOG_USER_VAR_EVENT;

/*
  The COPY_INFO structure is used by INSERT/REPLACE code.
  The schema of the row counting by the INSERT/INSERT ... ON DUPLICATE KEY
  UPDATE code:
    If a row is inserted then the copied variable is incremented.
    If a row is updated by the INSERT ... ON DUPLICATE KEY UPDATE and the
      new data differs from the old one then the copied and the updated
      variables are incremented.
    The touched variable is incremented if a row was touched by the update part
      of the INSERT ... ON DUPLICATE KEY UPDATE no matter whether the row
      was actually changed or not.
*/
typedef struct st_copy_info {
  ha_rows records; /**< Number of processed records */
  ha_rows deleted; /**< Number of deleted records */
  ha_rows updated; /**< Number of updated records */
  ha_rows copied;  /**< Number of copied records */
  ha_rows error_count;
  ha_rows touched; /* Number of touched records */
  enum enum_duplicates handle_duplicates;
  int escape_char, last_errno;
  bool ignore;
  /* for INSERT ... UPDATE */
  List<Item> *update_fields;
  List<Item> *update_values;
  /* for VIEW ... WITH CHECK OPTION */
  TABLE_LIST *view;
  TABLE_LIST *table_list;                       /* Normal table */
} COPY_INFO;


class Key_part_spec :public Sql_alloc {
public:
  LEX_STRING field_name;
  uint length;
  Key_part_spec(const LEX_STRING &name, uint len)
    : field_name(name), length(len)
  {}
  Key_part_spec(const char *name, const size_t name_len, uint len)
    : length(len)
  { field_name.str= (char *)name; field_name.length= name_len; }
  bool operator==(const Key_part_spec& other) const;
  /**
    Construct a copy of this Key_part_spec. field_name is copied
    by-pointer as it is known to never change. At the same time
    'length' may be reset in mysql_prepare_create_table, and this
    is why we supply it with a copy.

    @return If out of memory, 0 is returned and an error is set in
    THD.
  */
  Key_part_spec *clone(MEM_ROOT *mem_root) const
  { return new (mem_root) Key_part_spec(*this); }
};


class Alter_drop :public Sql_alloc {
public:
  enum drop_type {KEY, COLUMN, FOREIGN_KEY, CHECK_CONSTRAINT };
  const char *name;
  enum drop_type type;
  bool drop_if_exists;
  Alter_drop(enum drop_type par_type,const char *par_name, bool par_exists)
    :name(par_name), type(par_type), drop_if_exists(par_exists)
  {
    DBUG_ASSERT(par_name != NULL);
  }
  /**
    Used to make a clone of this object for ALTER/CREATE TABLE
    @sa comment for Key_part_spec::clone
  */
  Alter_drop *clone(MEM_ROOT *mem_root) const
    { return new (mem_root) Alter_drop(*this); }
  const char *type_name()
  {
    return type == COLUMN ? "COLUMN" :
           type == CHECK_CONSTRAINT ? "CONSTRAINT" :
           type == KEY ? "INDEX" : "FOREIGN KEY";
  }
};


class Alter_column :public Sql_alloc {
public:
  const char *name;
  Virtual_column_info *default_value;
  Alter_column(const char *par_name, Virtual_column_info *expr)
    :name(par_name), default_value(expr) {}
  /**
    Used to make a clone of this object for ALTER/CREATE TABLE
    @sa comment for Key_part_spec::clone
  */
  Alter_column *clone(MEM_ROOT *mem_root) const
    { return new (mem_root) Alter_column(*this); }
};


class Key :public Sql_alloc, public DDL_options {
public:
  enum Keytype { PRIMARY, UNIQUE, MULTIPLE, FULLTEXT, SPATIAL, FOREIGN_KEY};
  enum Keytype type;
  KEY_CREATE_INFO key_create_info;
  List<Key_part_spec> columns;
  LEX_STRING name;
  engine_option_value *option_list;
  bool generated;

  Key(enum Keytype type_par, const LEX_STRING &name_arg,
      ha_key_alg algorithm_arg, bool generated_arg, DDL_options_st ddl_options)
    :DDL_options(ddl_options),
     type(type_par), key_create_info(default_key_create_info),
    name(name_arg), option_list(NULL), generated(generated_arg)
  {
    key_create_info.algorithm= algorithm_arg;
  } 
  Key(enum Keytype type_par, const LEX_STRING &name_arg,
      KEY_CREATE_INFO *key_info_arg,
      bool generated_arg, List<Key_part_spec> &cols,
      engine_option_value *create_opt, DDL_options_st ddl_options)
    :DDL_options(ddl_options),
     type(type_par), key_create_info(*key_info_arg), columns(cols),
    name(name_arg), option_list(create_opt), generated(generated_arg)
  {}
  Key(enum Keytype type_par, const char *name_arg, size_t name_len_arg,
      KEY_CREATE_INFO *key_info_arg, bool generated_arg,
      List<Key_part_spec> &cols,
      engine_option_value *create_opt, DDL_options_st ddl_options)
    :DDL_options(ddl_options),
    type(type_par), key_create_info(*key_info_arg), columns(cols),
    option_list(create_opt), generated(generated_arg)
  {
    name.str= (char *)name_arg;
    name.length= name_len_arg;
  }
  Key(const Key &rhs, MEM_ROOT *mem_root);
  virtual ~Key() {}
  /* Equality comparison of keys (ignoring name) */
  friend bool foreign_key_prefix(Key *a, Key *b);
  /**
    Used to make a clone of this object for ALTER/CREATE TABLE
    @sa comment for Key_part_spec::clone
  */
  virtual Key *clone(MEM_ROOT *mem_root) const
    { return new (mem_root) Key(*this, mem_root); }
};


class Foreign_key: public Key {
public:
  enum fk_match_opt { FK_MATCH_UNDEF, FK_MATCH_FULL,
		      FK_MATCH_PARTIAL, FK_MATCH_SIMPLE};

  LEX_STRING ref_db;
  LEX_STRING ref_table;
  List<Key_part_spec> ref_columns;
  uint delete_opt, update_opt, match_opt;
  Foreign_key(const LEX_STRING &name_arg, List<Key_part_spec> &cols,
	      const LEX_STRING &ref_db_arg, const LEX_STRING &ref_table_arg,
              List<Key_part_spec> &ref_cols,
	      uint delete_opt_arg, uint update_opt_arg, uint match_opt_arg,
	      DDL_options ddl_options)
    :Key(FOREIGN_KEY, name_arg, &default_key_create_info, 0, cols, NULL,
         ddl_options),
    ref_db(ref_db_arg), ref_table(ref_table_arg), ref_columns(ref_cols),
    delete_opt(delete_opt_arg), update_opt(update_opt_arg),
    match_opt(match_opt_arg)
   {
    // We don't check for duplicate FKs.
    key_create_info.check_for_duplicate_indexes= false;
  }
 Foreign_key(const Foreign_key &rhs, MEM_ROOT *mem_root);
  /**
    Used to make a clone of this object for ALTER/CREATE TABLE
    @sa comment for Key_part_spec::clone
  */
  virtual Key *clone(MEM_ROOT *mem_root) const
  { return new (mem_root) Foreign_key(*this, mem_root); }
  /* Used to validate foreign key options */
  bool validate(List<Create_field> &table_fields);
};

typedef struct st_mysql_lock
{
  TABLE **table;
  uint table_count,lock_count;
  THR_LOCK_DATA **locks;
} MYSQL_LOCK;


class LEX_COLUMN : public Sql_alloc
{
public:
  String column;
  uint rights;
  LEX_COLUMN (const String& x,const  uint& y ): column (x),rights (y) {}
};

class MY_LOCALE;

/**
  Query_cache_tls -- query cache thread local data.
*/

struct Query_cache_block;

struct Query_cache_tls
{
  /*
    'first_query_block' should be accessed only via query cache
    functions and methods to maintain proper locking.
  */
  Query_cache_block *first_query_block;
  void set_first_query_block(Query_cache_block *first_query_block_arg)
  {
    first_query_block= first_query_block_arg;
  }

  Query_cache_tls() :first_query_block(NULL) {}
};

/* SIGNAL / RESIGNAL / GET DIAGNOSTICS */

/**
  This enumeration list all the condition item names of a condition in the
  SQL condition area.
*/
typedef enum enum_diag_condition_item_name
{
  /*
    Conditions that can be set by the user (SIGNAL/RESIGNAL),
    and by the server implementation.
  */

  DIAG_CLASS_ORIGIN= 0,
  FIRST_DIAG_SET_PROPERTY= DIAG_CLASS_ORIGIN,
  DIAG_SUBCLASS_ORIGIN= 1,
  DIAG_CONSTRAINT_CATALOG= 2,
  DIAG_CONSTRAINT_SCHEMA= 3,
  DIAG_CONSTRAINT_NAME= 4,
  DIAG_CATALOG_NAME= 5,
  DIAG_SCHEMA_NAME= 6,
  DIAG_TABLE_NAME= 7,
  DIAG_COLUMN_NAME= 8,
  DIAG_CURSOR_NAME= 9,
  DIAG_MESSAGE_TEXT= 10,
  DIAG_MYSQL_ERRNO= 11,
  LAST_DIAG_SET_PROPERTY= DIAG_MYSQL_ERRNO
} Diag_condition_item_name;

/**
  Name of each diagnostic condition item.
  This array is indexed by Diag_condition_item_name.
*/
extern const LEX_STRING Diag_condition_item_names[];

/**
  These states are bit coded with HARD. For each state there must be a pair
  <state_even_num>, and <state_odd_num>_HARD.
*/
enum killed_state
{
  NOT_KILLED= 0,
  KILL_HARD_BIT= 1,                             /* Bit for HARD KILL */
  KILL_BAD_DATA= 2,
  KILL_BAD_DATA_HARD= 3,
  KILL_QUERY= 4,
  KILL_QUERY_HARD= 5,
  /*
    ABORT_QUERY signals to the query processor to stop execution ASAP without
    issuing an error. Instead a warning is issued, and when possible a partial
    query result is returned to the client.
  */
  ABORT_QUERY= 6,
  ABORT_QUERY_HARD= 7,
  KILL_TIMEOUT= 8,
  KILL_TIMEOUT_HARD= 9,
  /*
    When binlog reading thread connects to the server it kills
    all the binlog threads with the same ID.
  */
  KILL_SLAVE_SAME_ID= 10,
  /*
    All of the following killed states will kill the connection
    KILL_CONNECTION must be the first of these and it must start with
    an even number (becasue of HARD bit)!
  */
  KILL_CONNECTION= 12,
  KILL_CONNECTION_HARD= 13,
  KILL_SYSTEM_THREAD= 14,
  KILL_SYSTEM_THREAD_HARD= 15,
  KILL_SERVER= 16,
  KILL_SERVER_HARD= 17,
  /*
    Used in threadpool to signal wait timeout.
  */
  KILL_WAIT_TIMEOUT= 18,
  KILL_WAIT_TIMEOUT_HARD= 19

};

#define killed_mask_hard(killed) ((killed_state) ((killed) & ~KILL_HARD_BIT))

enum killed_type
{
  KILL_TYPE_ID,
  KILL_TYPE_USER,
  KILL_TYPE_QUERY
};

#include "sql_lex.h"				/* Must be here */

class Delayed_insert;
class select_result;
class Time_zone;

#define THD_SENTRY_MAGIC 0xfeedd1ff
#define THD_SENTRY_GONE  0xdeadbeef

#define THD_CHECK_SENTRY(thd) DBUG_ASSERT(thd->dbug_sentry == THD_SENTRY_MAGIC)

typedef struct system_variables
{
  /*
    How dynamically allocated system variables are handled:

    The global_system_variables and max_system_variables are "authoritative"
    They both should have the same 'version' and 'size'.
    When attempting to access a dynamic variable, if the session version
    is out of date, then the session version is updated and realloced if
    neccessary and bytes copied from global to make up for missing data.

    Note that one should use my_bool instead of bool here, as the variables
    are used with my_getopt.c
  */
  ulong dynamic_variables_version;
  char* dynamic_variables_ptr;
  uint dynamic_variables_head;    /* largest valid variable offset */
  uint dynamic_variables_size;    /* how many bytes are in use */
  
  ulonglong max_heap_table_size;
  ulonglong tmp_memory_table_size;
  ulonglong tmp_disk_table_size;
  ulonglong long_query_time;
  ulonglong max_statement_time;
  ulonglong optimizer_switch;
  sql_mode_t sql_mode; ///< which non-standard SQL behaviour should be enabled
  sql_mode_t old_behavior; ///< which old SQL behaviour should be enabled
  ulonglong option_bits; ///< OPTION_xxx constants, e.g. OPTION_PROFILING
  ulonglong join_buff_space_limit;
  ulonglong log_slow_filter; 
  ulonglong log_slow_verbosity; 
  ulonglong bulk_insert_buff_size;
  ulonglong join_buff_size;
  ulonglong sortbuff_size;
  ulonglong default_regex_flags;
  ulonglong max_mem_used;

  /**
     Place holders to store Multi-source variables in sys_var.cc during
     update and show of variables.
  */
  ulonglong slave_skip_counter;
  ulonglong max_relay_log_size;

  ha_rows select_limit;
  ha_rows max_join_size;
  ha_rows expensive_subquery_limit;
  ulong auto_increment_increment, auto_increment_offset;
#ifdef WITH_WSREP
  /*
    Stored values of the auto_increment_increment and auto_increment_offset
    that are will be restored when wsrep_auto_increment_control will be set
    to 'OFF', because the setting it to 'ON' leads to overwriting of the
    original values (which are set by the user) by calculated ones (which
    are based on the cluster size):
  */
  ulong saved_auto_increment_increment, saved_auto_increment_offset;
#endif /* WITH_WSREP */
  uint eq_range_index_dive_limit;
  ulong lock_wait_timeout;
  ulong join_cache_level;
  ulong max_allowed_packet;
  ulong max_error_count;
  ulong max_length_for_sort_data;
  ulong max_recursive_iterations;
  ulong max_sort_length;
  ulong max_tmp_tables;
  ulong max_insert_delayed_threads;
  ulong min_examined_row_limit;
  ulong multi_range_count;
  ulong net_buffer_length;
  ulong net_interactive_timeout;
  ulong net_read_timeout;
  ulong net_retry_count;
  ulong net_wait_timeout;
  ulong net_write_timeout;
  ulong optimizer_prune_level;
  ulong optimizer_search_depth;
  ulong optimizer_selectivity_sampling_limit;
  ulong optimizer_use_condition_selectivity;
  ulong use_stat_tables;
  ulong histogram_size;
  ulong histogram_type;
  ulong preload_buff_size;
  ulong profiling_history_size;
  ulong read_buff_size;
  ulong read_rnd_buff_size;
  ulong mrr_buff_size;
  ulong div_precincrement;
  /* Total size of all buffers used by the subselect_rowid_merge_engine. */
  ulong rowid_merge_buff_size;
  ulong max_sp_recursion_depth;
  ulong default_week_format;
  ulong max_seeks_for_key;
  ulong range_alloc_block_size;
  ulong query_alloc_block_size;
  ulong query_prealloc_size;
  ulong trans_alloc_block_size;
  ulong trans_prealloc_size;
  ulong log_warnings;
  /* Flags for slow log filtering */
  ulong log_slow_rate_limit; 
  ulong binlog_format; ///< binlog format for this thd (see enum_binlog_format)
  ulong binlog_row_image;
  ulong progress_report_time;
  ulong completion_type;
  ulong query_cache_type;
  ulong tx_isolation;
  ulong updatable_views_with_limit;
  int max_user_connections;
  ulong server_id;
  /**
    In slave thread we need to know in behalf of which
    thread the query is being run to replicate temp tables properly
  */
  my_thread_id pseudo_thread_id;
  /**
     When replicating an event group with GTID, keep these values around so
     slave binlog can receive the same GTID as the original.
  */
  uint32     gtid_domain_id;
  uint64     gtid_seq_no;

  uint group_concat_max_len;

  /**
    Default transaction access mode. READ ONLY (true) or READ WRITE (false).
  */
  my_bool tx_read_only;
  my_bool low_priority_updates;
  my_bool query_cache_wlock_invalidate;
  my_bool keep_files_on_create;

  my_bool old_mode;
  my_bool old_alter_table;
  my_bool old_passwords;
  my_bool big_tables;
  my_bool only_standard_compliant_cte;
  my_bool query_cache_strip_comments;
  my_bool sql_log_slow;
  my_bool sql_log_bin;
  /*
    A flag to help detect whether binary logging was temporarily disabled
    (see tmp_disable_binlog(A) macro).
  */
  my_bool sql_log_bin_off;
  my_bool binlog_annotate_row_events;
  my_bool binlog_direct_non_trans_update;

  plugin_ref table_plugin;
  plugin_ref tmp_table_plugin;
  plugin_ref enforced_table_plugin;

  /* Only charset part of these variables is sensible */
  CHARSET_INFO  *character_set_filesystem;
  CHARSET_INFO  *character_set_client;
  CHARSET_INFO  *character_set_results;

  /* Both charset and collation parts of these variables are important */
  CHARSET_INFO	*collation_server;
  CHARSET_INFO	*collation_database;
  CHARSET_INFO  *collation_connection;

  /* Names. These will be allocated in buffers in thd */
  LEX_STRING default_master_connection;

  /* Error messages */
  MY_LOCALE *lc_messages;
  const char ***errmsgs;             /* lc_messages->errmsg->errmsgs */

  /* Locale Support */
  MY_LOCALE *lc_time_names;

  Time_zone *time_zone;

  my_bool sysdate_is_now;

  /* deadlock detection */
  ulong wt_timeout_short, wt_deadlock_search_depth_short;
  ulong wt_timeout_long, wt_deadlock_search_depth_long;

  my_bool wsrep_on;
  my_bool wsrep_causal_reads;
  my_bool wsrep_dirty_reads;
  uint wsrep_sync_wait;
  ulong wsrep_retry_autocommit;
  ulong wsrep_OSU_method;
  double long_query_time_double, max_statement_time_double;

  my_bool pseudo_slave_mode;

  char *session_track_system_variables;
  ulong session_track_transaction_info;
  my_bool session_track_schema;
  my_bool session_track_state_change;

  ulong threadpool_priority;
} SV;

/**
  Per thread status variables.
  Must be long/ulong up to last_system_status_var so that
  add_to_status/add_diff_to_status can work.
*/

typedef struct system_status_var
{
  ulong com_stat[(uint) SQLCOM_END];
  ulong com_create_tmp_table;
  ulong com_drop_tmp_table;
  ulong com_other;
  ulong com_multi;

  ulong com_stmt_prepare;
  ulong com_stmt_reprepare;
  ulong com_stmt_execute;
  ulong com_stmt_send_long_data;
  ulong com_stmt_fetch;
  ulong com_stmt_reset;
  ulong com_stmt_close;

  ulong com_register_slave;
  ulong created_tmp_disk_tables_;
  ulong created_tmp_tables_;
  ulong ha_commit_count;
  ulong ha_delete_count;
  ulong ha_read_first_count;
  ulong ha_read_last_count;
  ulong ha_read_key_count;
  ulong ha_read_next_count;
  ulong ha_read_prev_count;
  ulong ha_read_retry_count;
  ulong ha_read_rnd_count;
  ulong ha_read_rnd_next_count;
  ulong ha_read_rnd_deleted_count;

  /*
    This number doesn't include calls to the default implementation and
    calls made by range access. The intent is to count only calls made by
    BatchedKeyAccess.
  */
  ulong ha_mrr_init_count;
  ulong ha_mrr_key_refills_count;
  ulong ha_mrr_rowid_refills_count;

  ulong ha_rollback_count;
  ulong ha_update_count;
  ulong ha_write_count;
  /* The following are for internal temporary tables */
  ulong ha_tmp_update_count;
  ulong ha_tmp_write_count;
  ulong ha_prepare_count;
  ulong ha_icp_attempts;
  ulong ha_icp_match;
  ulong ha_discover_count;
  ulong ha_savepoint_count;
  ulong ha_savepoint_rollback_count;
  ulong ha_external_lock_count;

  ulong net_big_packet_count;
  ulong opened_tables;
  ulong opened_shares;
  ulong opened_views;               /* +1 opening a view */

  ulong select_full_join_count_;
  ulong select_full_range_join_count_;
  ulong select_range_count_;
  ulong select_range_check_count_;
  ulong select_scan_count_;
  ulong update_scan_count;
  ulong delete_scan_count;
  ulong executed_triggers;
  ulong long_query_count;
  ulong filesort_merge_passes_;
  ulong filesort_range_count_;
  ulong filesort_rows_;
  ulong filesort_scan_count_;
  ulong filesort_pq_sorts_;

  /* Features used */
  ulong feature_dynamic_columns;    /* +1 when creating a dynamic column */
  ulong feature_fulltext;	    /* +1 when MATCH is used */
  ulong feature_gis;                /* +1 opening a table with GIS features */
  ulong feature_locale;		    /* +1 when LOCALE is set */
  ulong feature_subquery;	    /* +1 when subqueries are used */
  ulong feature_timezone;	    /* +1 when XPATH is used */
  ulong feature_trigger;	    /* +1 opening a table with triggers */
  ulong feature_xml;		    /* +1 when XPATH is used */
  ulong feature_window_functions;   /* +1 when window functions are used */

  /* From MASTER_GTID_WAIT usage */
  ulong master_gtid_wait_timeouts;          /* Number of timeouts */
  ulong master_gtid_wait_time;              /* Time in microseconds */
  ulong master_gtid_wait_count;

  ulong empty_queries;
  ulong access_denied_errors;
  ulong lost_connections;
  ulong max_statement_time_exceeded;
  /*
    Number of statements sent from the client
  */
  ulong questions;
  /*
    IMPORTANT!
    SEE last_system_status_var DEFINITION BELOW.
    Below 'last_system_status_var' are all variables that cannot be handled
    automatically by add_to_status()/add_diff_to_status().
  */
  ulonglong bytes_received;
  ulonglong bytes_sent;
  ulonglong rows_read;
  ulonglong rows_sent;
  ulonglong rows_tmp_read;
  ulonglong binlog_bytes_written;
  double last_query_cost;
  double cpu_time, busy_time;
  /* Don't initialize */
  /* Memory used for thread local storage */
  volatile int64 local_memory_used;
  /* Memory allocated for global usage */
  volatile int64 global_memory_used;
} STATUS_VAR;

/*
  This is used for 'SHOW STATUS'. It must be updated to the last ulong
  variable in system_status_var which is makes sense to add to the global
  counter
*/

#define last_system_status_var questions
#define last_cleared_system_status_var local_memory_used

/*
  Global status variables
*/

extern ulong feature_files_opened_with_delayed_keys, feature_check_constraint;

void add_to_status(STATUS_VAR *to_var, STATUS_VAR *from_var);

void add_diff_to_status(STATUS_VAR *to_var, STATUS_VAR *from_var,
                        STATUS_VAR *dec_var);

/*
  Update global_memory_used. We have to do this with atomic_add as the
  global value can change outside of LOCK_status.
*/
inline void update_global_memory_status(int64 size)
{
  DBUG_PRINT("info", ("global memory_used: %lld  size: %lld",
                      (longlong) global_status_var.global_memory_used,
                      size));
  // workaround for gcc 4.2.4-1ubuntu4 -fPIE (from DEB_BUILD_HARDENING=1)
  int64 volatile * volatile ptr= &global_status_var.global_memory_used;
  my_atomic_add64_explicit(ptr, size, MY_MEMORY_ORDER_RELAXED);
}

/**
  Get collation by name, send error to client on failure.
  @param name     Collation name
  @param name_cs  Character set of the name string
  @return
  @retval         NULL on error
  @retval         Pointter to CHARSET_INFO with the given name on success
*/
inline CHARSET_INFO *
mysqld_collation_get_by_name(const char *name,
                             CHARSET_INFO *name_cs= system_charset_info)
{
  CHARSET_INFO *cs;
  MY_CHARSET_LOADER loader;
  my_charset_loader_init_mysys(&loader);
  if (!(cs= my_collation_get_by_name(&loader, name, MYF(0))))
  {
    ErrConvString err(name, name_cs);
    my_error(ER_UNKNOWN_COLLATION, MYF(0), err.ptr());
    if (loader.error[0])
      push_warning_printf(current_thd,
                          Sql_condition::WARN_LEVEL_WARN,
                          ER_UNKNOWN_COLLATION, "%s", loader.error);
  }
  return cs;
}

inline bool is_supported_parser_charset(CHARSET_INFO *cs)
{
  return MY_TEST(cs->mbminlen == 1);
}

#ifdef MYSQL_SERVER

void free_tmp_table(THD *thd, TABLE *entry);


/* The following macro is to make init of Query_arena simpler */
#ifndef DBUG_OFF
#define INIT_ARENA_DBUG_INFO is_backup_arena= 0; is_reprepared= FALSE;
#else
#define INIT_ARENA_DBUG_INFO
#endif

class Query_arena
{
public:
  /*
    List of items created in the parser for this query. Every item puts
    itself to the list on creation (see Item::Item() for details))
  */
  Item *free_list;
  MEM_ROOT *mem_root;                   // Pointer to current memroot
#ifndef DBUG_OFF
  bool is_backup_arena; /* True if this arena is used for backup. */
  bool is_reprepared;
#endif
  /*
    The states relfects three diffrent life cycles for three
    different types of statements:
    Prepared statement: STMT_INITIALIZED -> STMT_PREPARED -> STMT_EXECUTED.
    Stored procedure:   STMT_INITIALIZED_FOR_SP -> STMT_EXECUTED.
    Other statements:   STMT_CONVENTIONAL_EXECUTION never changes.
  */
  enum enum_state
  {
    STMT_INITIALIZED= 0, STMT_INITIALIZED_FOR_SP= 1, STMT_PREPARED= 2,
    STMT_CONVENTIONAL_EXECUTION= 3, STMT_EXECUTED= 4, STMT_ERROR= -1
  };

  enum_state state;

  /* We build without RTTI, so dynamic_cast can't be used. */
  enum Type
  {
    STATEMENT, PREPARED_STATEMENT, STORED_PROCEDURE, TABLE_ARENA
  };

  Query_arena(MEM_ROOT *mem_root_arg, enum enum_state state_arg) :
    free_list(0), mem_root(mem_root_arg), state(state_arg)
  { INIT_ARENA_DBUG_INFO; }
  /*
    This constructor is used only when Query_arena is created as
    backup storage for another instance of Query_arena.
  */
  Query_arena() { INIT_ARENA_DBUG_INFO; }

  virtual Type type() const;
  virtual ~Query_arena() {};

  inline bool is_stmt_prepare() const { return state == STMT_INITIALIZED; }
  inline bool is_stmt_prepare_or_first_sp_execute() const
  { return (int)state < (int)STMT_PREPARED; }
  inline bool is_stmt_prepare_or_first_stmt_execute() const
  { return (int)state <= (int)STMT_PREPARED; }
  inline bool is_stmt_execute() const
  { return state == STMT_PREPARED || state == STMT_EXECUTED; }
  inline bool is_conventional() const
  { return state == STMT_CONVENTIONAL_EXECUTION; }

  inline void* alloc(size_t size) { return alloc_root(mem_root,size); }
  inline void* calloc(size_t size)
  {
    void *ptr;
    if ((ptr=alloc_root(mem_root,size)))
      bzero(ptr, size);
    return ptr;
  }
  inline char *strdup(const char *str)
  { return strdup_root(mem_root,str); }
  inline char *strmake(const char *str, size_t size)
  { return strmake_root(mem_root,str,size); }
  inline void *memdup(const void *str, size_t size)
  { return memdup_root(mem_root,str,size); }
  inline void *memdup_w_gap(const void *str, size_t size, uint gap)
  {
    void *ptr;
    if ((ptr= alloc_root(mem_root,size+gap)))
      memcpy(ptr,str,size);
    return ptr;
  }

  void set_query_arena(Query_arena *set);

  void free_items();
  /* Close the active state associated with execution of this statement */
  virtual void cleanup_stmt();
};


class Query_arena_memroot: public Query_arena, public Sql_alloc
{
public:
  Query_arena_memroot(MEM_ROOT *mem_root_arg, enum enum_state state_arg) :
    Query_arena(mem_root_arg, state_arg)
  {}
  Query_arena_memroot() : Query_arena()
  {}

  virtual ~Query_arena_memroot() {}
};


class Server_side_cursor;

/**
  @class Statement
  @brief State of a single command executed against this connection.

  One connection can contain a lot of simultaneously running statements,
  some of which could be:
   - prepared, that is, contain placeholders,
   - opened as cursors. We maintain 1 to 1 relationship between
     statement and cursor - if user wants to create another cursor for his
     query, we create another statement for it.
  To perform some action with statement we reset THD part to the state  of
  that statement, do the action, and then save back modified state from THD
  to the statement. It will be changed in near future, and Statement will
  be used explicitly.
*/

class Statement: public ilink, public Query_arena
{
  Statement(const Statement &rhs);              /* not implemented: */
  Statement &operator=(const Statement &rhs);   /* non-copyable */
public:
  /*
    Uniquely identifies each statement object in thread scope; change during
    statement lifetime. FIXME: must be const
  */
   ulong id;

  /*
    MARK_COLUMNS_NONE:  Means mark_used_colums is not set and no indicator to
                        handler of fields used is set
    MARK_COLUMNS_READ:  Means a bit in read set is set to inform handler
	                that the field is to be read. If field list contains
                        duplicates, then thd->dup_field is set to point
                        to the last found duplicate.
    MARK_COLUMNS_WRITE: Means a bit is set in write set to inform handler
			that it needs to update this field in write_row
                        and update_row.
  */
  enum enum_mark_columns mark_used_columns;

  LEX_STRING name; /* name for named prepared statements */
  LEX *lex;                                     // parse tree descriptor
  /*
    Points to the query associated with this statement. It's const, but
    we need to declare it char * because all table handlers are written
    in C and need to point to it.

    Note that if we set query = NULL, we must at the same time set
    query_length = 0, and protect the whole operation with
    LOCK_thd_data mutex. To avoid crashes in races, if we do not
    know that thd->query cannot change at the moment, we should print
    thd->query like this:
      (1) reserve the LOCK_thd_data mutex;
      (2) print or copy the value of query and query_length
      (3) release LOCK_thd_data mutex.
    This printing is needed at least in SHOW PROCESSLIST and SHOW
    ENGINE INNODB STATUS.
  */
  CSET_STRING query_string;
  /*
    If opt_query_cache_strip_comments is set, this contains query without
    comments. If not set, it contains pointer to query_string.
  */
  String base_query;


  inline char *query() const { return query_string.str(); }
  inline uint32 query_length() const
  {
    return static_cast<uint32>(query_string.length());
  }
  CHARSET_INFO *query_charset() const { return query_string.charset(); }
  void set_query_inner(const CSET_STRING &string_arg)
  {
    query_string= string_arg;
  }
  void set_query_inner(char *query_arg, uint32 query_length_arg,
                       CHARSET_INFO *cs_arg)
  {
    set_query_inner(CSET_STRING(query_arg, query_length_arg, cs_arg));
  }
  void reset_query_inner()
  {
    set_query_inner(CSET_STRING());
  }
  /**
    Name of the current (default) database.

    If there is the current (default) database, "db" contains its name. If
    there is no current (default) database, "db" is NULL and "db_length" is
    0. In other words, "db", "db_length" must either be NULL, or contain a
    valid database name.

    @note this attribute is set and alloced by the slave SQL thread (for
    the THD of that thread); that thread is (and must remain, for now) the
    only responsible for freeing this member.
  */

  char *db;
  size_t db_length;

  /* This is set to 1 of last call to send_result_to_client() was ok */
  my_bool query_cache_is_applicable;

  /* This constructor is called for backup statements */
  Statement() {}

  Statement(LEX *lex_arg, MEM_ROOT *mem_root_arg,
            enum enum_state state_arg, ulong id_arg);
  virtual ~Statement();

  /* Assign execution context (note: not all members) of given stmt to self */
  virtual void set_statement(Statement *stmt);
  void set_n_backup_statement(Statement *stmt, Statement *backup);
  void restore_backup_statement(Statement *stmt, Statement *backup);
  /* return class type */
  virtual Type type() const;
};


/**
  Container for all statements created/used in a connection.
  Statements in Statement_map have unique Statement::id (guaranteed by id
  assignment in Statement::Statement)
  Non-empty statement names are unique too: attempt to insert a new statement
  with duplicate name causes older statement to be deleted

  Statements are auto-deleted when they are removed from the map and when the
  map is deleted.
*/

class Statement_map
{
public:
  Statement_map();

  int insert(THD *thd, Statement *statement);

  Statement *find_by_name(LEX_STRING *name)
  {
    Statement *stmt;
    stmt= (Statement*)my_hash_search(&names_hash, (uchar*)name->str,
                                     name->length);
    return stmt;
  }

  Statement *find(ulong id)
  {
    if (last_found_statement == 0 || id != last_found_statement->id)
    {
      Statement *stmt;
      stmt= (Statement *) my_hash_search(&st_hash, (uchar *) &id, sizeof(id));
      if (stmt && stmt->name.str)
        return NULL;
      last_found_statement= stmt;
    }
    return last_found_statement;
  }
  /*
    Close all cursors of this connection that use tables of a storage
    engine that has transaction-specific state and therefore can not
    survive COMMIT or ROLLBACK. Currently all but MyISAM cursors are closed.
  */
  void close_transient_cursors();
  void erase(Statement *statement);
  /* Erase all statements (calls Statement destructor) */
  void reset();
  ~Statement_map();
private:
  HASH st_hash;
  HASH names_hash;
  I_List<Statement> transient_cursor_list;
  Statement *last_found_statement;
};

struct st_savepoint {
  struct st_savepoint *prev;
  char                *name;
  uint                 length;
  Ha_trx_info         *ha_list;
  /** State of metadata locks before this savepoint was set. */
  MDL_savepoint        mdl_savepoint;
};

enum xa_states {XA_NOTR=0, XA_ACTIVE, XA_IDLE, XA_PREPARED, XA_ROLLBACK_ONLY};
extern const char *xa_state_names[];
class XID_cache_element;

typedef struct st_xid_state {
  /* For now, this is only used to catch duplicated external xids */
  XID  xid;                           // transaction identifier
  enum xa_states xa_state;            // used by external XA only
  /* Error reported by the Resource Manager (RM) to the Transaction Manager. */
  uint rm_error;
  XID_cache_element *xid_cache_element;

  /**
    Check that XA transaction has an uncommitted work. Report an error
    to the user in case when there is an uncommitted work for XA transaction.

    @return  result of check
      @retval  false  XA transaction is NOT in state IDLE, PREPARED
                      or ROLLBACK_ONLY.
      @retval  true   XA transaction is in state IDLE or PREPARED
                      or ROLLBACK_ONLY.
  */

  bool check_has_uncommitted_xa() const
  {
    if (xa_state == XA_IDLE ||
        xa_state == XA_PREPARED ||
        xa_state == XA_ROLLBACK_ONLY)
    {
      my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);
      return true;
    }
    return false;
  }
} XID_STATE;

void xid_cache_init(void);
void xid_cache_free(void);
XID_STATE *xid_cache_search(THD *thd, XID *xid);
bool xid_cache_insert(XID *xid, enum xa_states xa_state);
bool xid_cache_insert(THD *thd, XID_STATE *xid_state);
void xid_cache_delete(THD *thd, XID_STATE *xid_state);
int xid_cache_iterate(THD *thd, my_hash_walk_action action, void *argument);

/**
  @class Security_context
  @brief A set of THD members describing the current authenticated user.
*/

class Security_context {
public:
  Security_context() {}                       /* Remove gcc warning */
  /*
    host - host of the client
    user - user of the client, set to NULL until the user has been read from
    the connection
    priv_user - The user privilege we are using. May be "" for anonymous user.
    ip - client IP
  */
  const char *host;
  char   *user, *ip;
  char   priv_user[USERNAME_LENGTH];
  char   proxy_user[USERNAME_LENGTH + MAX_HOSTNAME + 5];
  /* The host privilege we are using */
  char   priv_host[MAX_HOSTNAME];
  /* The role privilege we are using */
  char   priv_role[USERNAME_LENGTH];
  /* The external user (if available) */
  char   *external_user;
  /* points to host if host is available, otherwise points to ip */
  const char *host_or_ip;
  ulong master_access;                 /* Global privileges from mysql.user */
  ulong db_access;                     /* Privileges for current db */

  void init();
  void destroy();
  void skip_grants();
  inline char *priv_host_name()
  {
    return (*priv_host ? priv_host : (char *)"%");
  }

  bool set_user(char *user_arg);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  bool
  change_security_context(THD *thd,
                          LEX_STRING *definer_user,
                          LEX_STRING *definer_host,
                          LEX_STRING *db,
                          Security_context **backup);

  void
  restore_security_context(THD *thd, Security_context *backup);
#endif
  bool user_matches(Security_context *);
};


/**
  A registry for item tree transformations performed during
  query optimization. We register only those changes which require
  a rollback to re-execute a prepared statement or stored procedure
  yet another time.
*/

struct Item_change_record;
class Item_change_list
{
  I_List<Item_change_record> change_list;
public:
  void nocheck_register_item_tree_change(Item **place, Item *old_value,
                                         MEM_ROOT *runtime_memroot);
  void check_and_register_item_tree_change(Item **place, Item **new_value,
                                           MEM_ROOT *runtime_memroot);
  void rollback_item_tree_changes();
  void move_elements_to(Item_change_list *to)
  {
    change_list.move_elements_to(&to->change_list);
  }
  bool is_empty() { return change_list.is_empty(); }
};


class Item_change_list_savepoint: public Item_change_list
{
public:
  Item_change_list_savepoint(Item_change_list *list)
  {
    list->move_elements_to(this);
  }
  void rollback(Item_change_list *list)
  {
    list->rollback_item_tree_changes();
    move_elements_to(list);
  }
  ~Item_change_list_savepoint()
  {
    DBUG_ASSERT(is_empty());
  }
};


/**
  Type of locked tables mode.
  See comment for THD::locked_tables_mode for complete description.
*/

enum enum_locked_tables_mode
{
  LTM_NONE= 0,
  LTM_LOCK_TABLES,
  LTM_PRELOCKED,
  LTM_PRELOCKED_UNDER_LOCK_TABLES,
  LTM_always_last
};

/**
  The following structure is an extension to TABLE_SHARE and is
  exclusively for temporary tables.

  @note:
  Although, TDC_element has data members (like next, prev &
  all_tables) to store the list of TABLE_SHARE & TABLE objects
  related to a particular TABLE_SHARE, they cannot be moved to
  TABLE_SHARE in order to be reused for temporary tables. This
  is because, as concurrent threads iterating through hash of
  TDC_element's may need access to all_tables, but if all_tables
  is made part of TABLE_SHARE, then TDC_element->share->all_tables
  is not always guaranteed to be valid, as TDC_element can live
  longer than TABLE_SHARE.
*/
struct TMP_TABLE_SHARE : public TABLE_SHARE
{
private:
  /*
   Link to all temporary table shares. Declared as private to
   avoid direct manipulation with those objects. One should
   use methods of I_P_List template instead.
  */
  TMP_TABLE_SHARE *tmp_next;
  TMP_TABLE_SHARE **tmp_prev;

  friend struct All_tmp_table_shares;

public:
  /*
    Doubly-linked (back-linked) lists of used and unused TABLE objects
    for this share.
  */
  All_share_tables_list all_tmp_tables;
};

/**
  Helper class which specifies which members of TMP_TABLE_SHARE are
  used for participation in the list of temporary tables.
*/

struct All_tmp_table_shares
{
  static inline TMP_TABLE_SHARE **next_ptr(TMP_TABLE_SHARE *l)
  {
    return &l->tmp_next;
  }
  static inline TMP_TABLE_SHARE ***prev_ptr(TMP_TABLE_SHARE *l)
  {
    return &l->tmp_prev;
  }
};

/* Also used in rpl_rli.h. */
typedef I_P_List <TMP_TABLE_SHARE, All_tmp_table_shares> All_tmp_tables_list;

/**
  Class that holds information about tables which were opened and locked
  by the thread. It is also used to save/restore this information in
  push_open_tables_state()/pop_open_tables_state().
*/

class Open_tables_state
{
public:
  /**
    As part of class THD, this member is set during execution
    of a prepared statement. When it is set, it is used
    by the locking subsystem to report a change in table metadata.

    When Open_tables_state part of THD is reset to open
    a system or INFORMATION_SCHEMA table, the member is cleared
    to avoid spurious ER_NEED_REPREPARE errors -- system and
    INFORMATION_SCHEMA tables are not subject to metadata version
    tracking.
    @sa check_and_update_table_version()
  */
  Reprepare_observer *m_reprepare_observer;

  /**
    List of regular tables in use by this thread. Contains temporary and
    base tables that were opened with @see open_tables().
  */
  TABLE *open_tables;

  /**
    A list of temporary tables used by this thread. This includes
    user-level temporary tables, created with CREATE TEMPORARY TABLE,
    and internal temporary tables, created, e.g., to resolve a SELECT,
    or for an intermediate table used in ALTER.
  */
  All_tmp_tables_list *temporary_tables;

  /*
    Derived tables.
  */
  TABLE *derived_tables;

  /* 
    Temporary tables created for recursive table references.
  */
  TABLE *rec_tables;

  /*
    During a MySQL session, one can lock tables in two modes: automatic
    or manual. In automatic mode all necessary tables are locked just before
    statement execution, and all acquired locks are stored in 'lock'
    member. Unlocking takes place automatically as well, when the
    statement ends.
    Manual mode comes into play when a user issues a 'LOCK TABLES'
    statement. In this mode the user can only use the locked tables.
    Trying to use any other tables will give an error.
    The locked tables are also stored in this member, however,
    thd->locked_tables_mode is turned on.  Manual locking is described in
    the 'LOCK_TABLES' chapter of the MySQL manual.
    See also lock_tables() for details.
  */
  MYSQL_LOCK *lock;

  /*
    CREATE-SELECT keeps an extra lock for the table being
    created. This field is used to keep the extra lock available for
    lower level routines, which would otherwise miss that lock.
   */
  MYSQL_LOCK *extra_lock;

  /*
    Enum enum_locked_tables_mode and locked_tables_mode member are
    used to indicate whether the so-called "locked tables mode" is on,
    and what kind of mode is active.

    Locked tables mode is used when it's necessary to open and
    lock many tables at once, for usage across multiple
    (sub-)statements.
    This may be necessary either for queries that use stored functions
    and triggers, in which case the statements inside functions and
    triggers may be executed many times, or for implementation of
    LOCK TABLES, in which case the opened tables are reused by all
    subsequent statements until a call to UNLOCK TABLES.

    The kind of locked tables mode employed for stored functions and
    triggers is also called "prelocked mode".
    In this mode, first open_tables() call to open the tables used
    in a statement analyses all functions used by the statement
    and adds all indirectly used tables to the list of tables to
    open and lock.
    It also marks the parse tree of the statement as requiring
    prelocking. After that, lock_tables() locks the entire list
    of tables and changes THD::locked_tables_modeto LTM_PRELOCKED.
    All statements executed inside functions or triggers
    use the prelocked tables, instead of opening their own ones.
    Prelocked mode is turned off automatically once close_thread_tables()
    of the main statement is called.
  */
  enum enum_locked_tables_mode locked_tables_mode;
  uint current_tablenr;

  enum enum_flags {
    BACKUPS_AVAIL = (1U << 0)     /* There are backups available */
  };

  /*
    Flags with information about the open tables state.
  */
  uint state_flags;
  /**
     This constructor initializes Open_tables_state instance which can only
     be used as backup storage. To prepare Open_tables_state instance for
     operations which open/lock/close tables (e.g. open_table()) one has to
     call init_open_tables_state().
  */
  Open_tables_state() : state_flags(0U) { }

  void set_open_tables_state(Open_tables_state *state)
  {
    *this= *state;
  }

  void reset_open_tables_state(THD *thd)
  {
    open_tables= 0;
    temporary_tables= 0;
    derived_tables= 0;
    rec_tables= 0;
    extra_lock= 0;
    lock= 0;
    locked_tables_mode= LTM_NONE;
    state_flags= 0U;
    m_reprepare_observer= NULL;
  }
};


/**
  Storage for backup of Open_tables_state. Must
  be used only to open system tables (TABLE_CATEGORY_SYSTEM
  and TABLE_CATEGORY_LOG).
*/

class Open_tables_backup: public Open_tables_state
{
public:
  /**
    When we backup the open tables state to open a system
    table or tables, we want to save state of metadata
    locks which were acquired before the backup. It is used
    to release metadata locks on system tables after they are
    no longer used.
  */
  MDL_savepoint mdl_system_tables_svp;
};

/**
  @class Sub_statement_state
  @brief Used to save context when executing a function or trigger

  operations on stat tables aren't technically a sub-statement, but they are
  similar in a sense that they cannot change the transaction status.
*/

/* Defines used for Sub_statement_state::in_sub_stmt */

#define SUB_STMT_TRIGGER 1
#define SUB_STMT_FUNCTION 2
#define SUB_STMT_STAT_TABLES 4


class Sub_statement_state
{
public:
  ulonglong option_bits;
  ulonglong first_successful_insert_id_in_prev_stmt;
  ulonglong first_successful_insert_id_in_cur_stmt, insert_id_for_cur_row;
  Discrete_interval auto_inc_interval_for_cur_row;
  Discrete_intervals_list auto_inc_intervals_forced;
  ulonglong limit_found_rows;
  ha_rows    cuted_fields, sent_row_count, examined_row_count;
  ulonglong client_capabilities;
  ulong query_plan_flags; 
  uint in_sub_stmt;    /* 0,  SUB_STMT_TRIGGER or SUB_STMT_FUNCTION */
  bool enable_slow_log;
  bool last_insert_id_used;
  SAVEPOINT *savepoints;
  enum enum_check_fields count_cuted_fields;
};


/* Flags for the THD::system_thread variable */
enum enum_thread_type
{
  NON_SYSTEM_THREAD= 0,
  SYSTEM_THREAD_DELAYED_INSERT= 1,
  SYSTEM_THREAD_SLAVE_IO= 2,
  SYSTEM_THREAD_SLAVE_SQL= 4,
  SYSTEM_THREAD_EVENT_SCHEDULER= 8,
  SYSTEM_THREAD_EVENT_WORKER= 16,
  SYSTEM_THREAD_BINLOG_BACKGROUND= 32,
  SYSTEM_THREAD_SLAVE_BACKGROUND= 64,
  SYSTEM_THREAD_GENERIC= 128
};

inline char const *
show_system_thread(enum_thread_type thread)
{
#define RETURN_NAME_AS_STRING(NAME) case (NAME): return #NAME
  switch (thread) {
    static char buf[64];
    RETURN_NAME_AS_STRING(NON_SYSTEM_THREAD);
    RETURN_NAME_AS_STRING(SYSTEM_THREAD_DELAYED_INSERT);
    RETURN_NAME_AS_STRING(SYSTEM_THREAD_SLAVE_IO);
    RETURN_NAME_AS_STRING(SYSTEM_THREAD_SLAVE_SQL);
    RETURN_NAME_AS_STRING(SYSTEM_THREAD_EVENT_SCHEDULER);
    RETURN_NAME_AS_STRING(SYSTEM_THREAD_EVENT_WORKER);
    RETURN_NAME_AS_STRING(SYSTEM_THREAD_SLAVE_BACKGROUND);
  default:
    sprintf(buf, "<UNKNOWN SYSTEM THREAD: %d>", thread);
    return buf;
  }
#undef RETURN_NAME_AS_STRING
}

/**
  This class represents the interface for internal error handlers.
  Internal error handlers are exception handlers used by the server
  implementation.
*/

class Internal_error_handler
{
protected:
  Internal_error_handler() :
    m_prev_internal_handler(NULL)
  {}

  virtual ~Internal_error_handler() {}

public:
  /**
    Handle a sql condition.
    This method can be implemented by a subclass to achieve any of the
    following:
    - mask a warning/error internally, prevent exposing it to the user,
    - mask a warning/error and throw another one instead.
    When this method returns true, the sql condition is considered
    'handled', and will not be propagated to upper layers.
    It is the responsability of the code installing an internal handler
    to then check for trapped conditions, and implement logic to recover
    from the anticipated conditions trapped during runtime.

    This mechanism is similar to C++ try/throw/catch:
    - 'try' correspond to <code>THD::push_internal_handler()</code>,
    - 'throw' correspond to <code>my_error()</code>,
    which invokes <code>my_message_sql()</code>,
    - 'catch' correspond to checking how/if an internal handler was invoked,
    before removing it from the exception stack with
    <code>THD::pop_internal_handler()</code>.

    @param thd the calling thread
    @param cond the condition raised.
    @return true if the condition is handled
  */
  virtual bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char* sqlstate,
                                Sql_condition::enum_warning_level *level,
                                const char* msg,
                                Sql_condition ** cond_hdl) = 0;

private:
  Internal_error_handler *m_prev_internal_handler;
  friend class THD;
};


/**
  Implements the trivial error handler which cancels all error states
  and prevents an SQLSTATE to be set.
*/

class Dummy_error_handler : public Internal_error_handler
{
public:
  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl)
  {
    /* Ignore error */
    return TRUE;
  }
  Dummy_error_handler() {}                    /* Remove gcc warning */
};


/**
  Implements the trivial error handler which counts errors as they happen.
*/

class Counting_error_handler : public Internal_error_handler
{
public:
  int errors;
  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl)
  {
    if (*level == Sql_condition::WARN_LEVEL_ERROR)
      errors++;
    return false;
  }
  Counting_error_handler() : errors(0) {}
};


/**
  This class is an internal error handler implementation for
  DROP TABLE statements. The thing is that there may be warnings during
  execution of these statements, which should not be exposed to the user.
  This class is intended to silence such warnings.
*/

class Drop_table_error_handler : public Internal_error_handler
{
public:
  Drop_table_error_handler() {}

public:
  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl);

private:
};


/**
  Internal error handler to process an error from MDL_context::upgrade_lock()
  and mysql_lock_tables(). Used by implementations of HANDLER READ and
  LOCK TABLES LOCAL.
*/

class MDL_deadlock_and_lock_abort_error_handler: public Internal_error_handler
{
public:
  virtual
  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char *sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition **cond_hdl);

  bool need_reopen() const { return m_need_reopen; };
  void init() { m_need_reopen= FALSE; };
private:
  bool m_need_reopen;
};


/**
  Tables that were locked with LOCK TABLES statement.

  Encapsulates a list of TABLE_LIST instances for tables
  locked by LOCK TABLES statement, memory root for metadata locks,
  and, generally, the context of LOCK TABLES statement.

  In LOCK TABLES mode, the locked tables are kept open between
  statements.
  Therefore, we can't allocate metadata locks on execution memory
  root -- as well as tables, the locks need to stay around till
  UNLOCK TABLES is called.
  The locks are allocated in the memory root encapsulated in this
  class.

  Some SQL commands, like FLUSH TABLE or ALTER TABLE, demand that
  the tables they operate on are closed, at least temporarily.
  This class encapsulates a list of TABLE_LIST instances, one
  for each base table from LOCK TABLES list,
  which helps conveniently close the TABLEs when it's necessary
  and later reopen them.

  Implemented in sql_base.cc
*/

class Locked_tables_list
{
private:
  MEM_ROOT m_locked_tables_root;
  TABLE_LIST *m_locked_tables;
  TABLE_LIST **m_locked_tables_last;
  /** An auxiliary array used only in reopen_tables(). */
  TABLE_LIST **m_reopen_array;
  /**
    Count the number of tables in m_locked_tables list. We can't
    rely on thd->lock->table_count because it excludes
    non-transactional temporary tables. We need to know
    an exact number of TABLE objects.
  */
  uint m_locked_tables_count;
public:
  bool some_table_marked_for_reopen;

  Locked_tables_list()
    :m_locked_tables(NULL),
    m_locked_tables_last(&m_locked_tables),
    m_reopen_array(NULL),
    m_locked_tables_count(0),
    some_table_marked_for_reopen(0)
  {
    init_sql_alloc(&m_locked_tables_root, MEM_ROOT_BLOCK_SIZE, 0,
                   MYF(MY_THREAD_SPECIFIC));
  }
  void unlock_locked_tables(THD *thd);
  void unlock_locked_table(THD *thd, MDL_ticket *mdl_ticket);
  ~Locked_tables_list()
  {
    reset();
  }
  void reset();
  bool init_locked_tables(THD *thd);
  TABLE_LIST *locked_tables() { return m_locked_tables; }
  void unlink_from_list(THD *thd, TABLE_LIST *table_list,
                        bool remove_from_locked_tables);
  void unlink_all_closed_tables(THD *thd,
                                MYSQL_LOCK *lock,
                                size_t reopen_count);
  bool reopen_tables(THD *thd, bool need_reopen);
  bool restore_lock(THD *thd, TABLE_LIST *dst_table_list, TABLE *table,
                    MYSQL_LOCK *lock);
  void add_back_last_deleted_lock(TABLE_LIST *dst_table_list);
  void mark_table_for_reopen(THD *thd, TABLE *table);
};


/**
  Storage engine specific thread local data.
*/

struct Ha_data
{
  /**
    Storage engine specific thread local data.
    Lifetime: one user connection.
  */
  void *ha_ptr;
  /**
    0: Life time: one statement within a transaction. If @@autocommit is
    on, also represents the entire transaction.
    @sa trans_register_ha()

    1: Life time: one transaction within a connection.
    If the storage engine does not participate in a transaction,
    this should not be used.
    @sa trans_register_ha()
  */
  Ha_trx_info ha_info[2];
  /**
    NULL: engine is not bound to this thread
    non-NULL: engine is bound to this thread, engine shutdown forbidden
  */
  plugin_ref lock;
  Ha_data() :ha_ptr(NULL) {}
};

/**
  An instance of the global read lock in a connection.
  Implemented in lock.cc.
*/

class Global_read_lock
{
public:
  enum enum_grl_state
  {
    GRL_NONE,
    GRL_ACQUIRED,
    GRL_ACQUIRED_AND_BLOCKS_COMMIT
  };

  Global_read_lock()
    : m_state(GRL_NONE),
      m_mdl_global_shared_lock(NULL),
      m_mdl_blocks_commits_lock(NULL)
  {}

  bool lock_global_read_lock(THD *thd);
  void unlock_global_read_lock(THD *thd);
  /**
    Check if this connection can acquire protection against GRL and
    emit error if otherwise.
  */
  bool can_acquire_protection() const
  {
    if (m_state)
    {
      my_error(ER_CANT_UPDATE_WITH_READLOCK, MYF(0));
      return TRUE;
    }
    return FALSE;
  }
  bool make_global_read_lock_block_commit(THD *thd);
  bool is_acquired() const { return m_state != GRL_NONE; }
  void set_explicit_lock_duration(THD *thd);
private:
  enum_grl_state m_state;
  /**
    In order to acquire the global read lock, the connection must
    acquire shared metadata lock in GLOBAL namespace, to prohibit
    all DDL.
  */
  MDL_ticket *m_mdl_global_shared_lock;
  /**
    Also in order to acquire the global read lock, the connection
    must acquire a shared metadata lock in COMMIT namespace, to
    prohibit commits.
  */
  MDL_ticket *m_mdl_blocks_commits_lock;
};


/*
  Class to facilitate the commit of one transactions waiting for the commit of
  another transaction to complete first.

  This is used during (parallel) replication, to allow different transactions
  to be applied in parallel, but still commit in order.

  The transaction that wants to wait for a prior commit must first register
  to wait with register_wait_for_prior_commit(waitee). Such registration
  must be done holding the waitee->LOCK_wait_commit, to prevent the other
  THD from disappearing during the registration.

  Then during commit, if a THD is registered to wait, it will call
  wait_for_prior_commit() as part of ha_commit_trans(). If no wait is
  registered, or if the waitee for has already completed commit, then
  wait_for_prior_commit() returns immediately.

  And when a THD that may be waited for has completed commit (more precisely
  commit_ordered()), then it must call wakeup_subsequent_commits() to wake
  up any waiters. Note that this must be done at a point that is guaranteed
  to be later than any waiters registering themselves. It is safe to call
  wakeup_subsequent_commits() multiple times, as waiters are removed from
  registration as part of the wakeup.

  The reason for separate register and wait calls is that this allows to
  register the wait early, at a point where the waited-for THD is known to
  exist. And then the actual wait can be done much later, where the
  waited-for THD may have been long gone. By registering early, the waitee
  can signal before disappearing.
*/
struct wait_for_commit
{
  /*
    The LOCK_wait_commit protects the fields subsequent_commits_list and
    wakeup_subsequent_commits_running (for a waitee), and the pointer
    waiterr and associated COND_wait_commit (for a waiter).
  */
  mysql_mutex_t LOCK_wait_commit;
  mysql_cond_t COND_wait_commit;
  /* List of threads that did register_wait_for_prior_commit() on us. */
  wait_for_commit *subsequent_commits_list;
  /* Link field for entries in subsequent_commits_list. */
  wait_for_commit *next_subsequent_commit;
  /*
    Our waitee, if we did register_wait_for_prior_commit(), and were not
    yet woken up. Else NULL.

    When this is cleared for wakeup, the COND_wait_commit condition is
    signalled.
  */
  wait_for_commit *waitee;
  /*
    Generic pointer for use by the transaction coordinator to optimise the
    waiting for improved group commit.

    Currently used by binlog TC to signal that a waiter is ready to commit, so
    that the waitee can grab it and group commit it directly. It is free to be
    used by another transaction coordinator for similar purposes.
  */
  void *opaque_pointer;
  /* The wakeup error code from the waitee. 0 means no error. */
  int wakeup_error;
  /*
    Flag set when wakeup_subsequent_commits_running() is active, see comments
    on that function for details.
  */
  bool wakeup_subsequent_commits_running;
  /*
    This flag can be set when a commit starts, but has not completed yet.
    It is used by binlog group commit to allow a waiting transaction T2 to
    join the group commit of an earlier transaction T1. When T1 has queued
    itself for group commit, it will set the commit_started flag. Then when
    T2 becomes ready to commit and needs to wait for T1 to commit first, T2
    can queue itself before waiting, and thereby participate in the same
    group commit as T1.
  */
  bool commit_started;

  void register_wait_for_prior_commit(wait_for_commit *waitee);
  int wait_for_prior_commit(THD *thd)
  {
    /*
      Quick inline check, to avoid function call and locking in the common case
      where no wakeup is registered, or a registered wait was already signalled.
    */
    if (waitee)
      return wait_for_prior_commit2(thd);
    else
    {
      if (wakeup_error)
        my_error(ER_PRIOR_COMMIT_FAILED, MYF(0));
      return wakeup_error;
    }
  }
  void wakeup_subsequent_commits(int wakeup_error_arg)
  {
    /*
      Do the check inline, so only the wakeup case takes the cost of a function
      call for every commmit.

      Note that the check is done without locking. It is the responsibility of
      the user of the wakeup facility to ensure that no waiters can register
      themselves after the last call to wakeup_subsequent_commits().

      This avoids having to take another lock for every commit, which would be
      pointless anyway - even if we check under lock, there is nothing to
      prevent a waiter from arriving just after releasing the lock.
    */
    if (subsequent_commits_list)
      wakeup_subsequent_commits2(wakeup_error_arg);
  }
  void unregister_wait_for_prior_commit()
  {
    if (waitee)
      unregister_wait_for_prior_commit2();
    else
      wakeup_error= 0;
  }
  /*
    Remove a waiter from the list in the waitee. Used to unregister a wait.
    The caller must be holding the locks of both waiter and waitee.
  */
  void remove_from_list(wait_for_commit **next_ptr_ptr)
  {
    wait_for_commit *cur;

    while ((cur= *next_ptr_ptr) != NULL)
    {
      if (cur == this)
      {
        *next_ptr_ptr= this->next_subsequent_commit;
        break;
      }
      next_ptr_ptr= &cur->next_subsequent_commit;
    }
    waitee= NULL;
  }

  void wakeup(int wakeup_error);

  int wait_for_prior_commit2(THD *thd);
  void wakeup_subsequent_commits2(int wakeup_error);
  void unregister_wait_for_prior_commit2();

  wait_for_commit();
  ~wait_for_commit();
  void reinit();
};


extern "C" void my_message_sql(uint error, const char *str, myf MyFlags);

class THD;
#ifndef DBUG_OFF
void dbug_serve_apcs(THD *thd, int n_calls);
#endif 

/**
  @class THD
  For each client connection we create a separate thread with THD serving as
  a thread/connection descriptor
*/

class THD :public Statement,
           /*
             This is to track items changed during execution of a prepared
             statement/stored procedure. It's created by
             nocheck_register_item_tree_change() in memory root of THD,
             and freed in rollback_item_tree_changes().
             For conventional execution it's always empty.
           */
           public Item_change_list,
           public MDL_context_owner,
           public Open_tables_state
{
private:
  inline bool is_stmt_prepare() const
  { DBUG_ASSERT(0); return Statement::is_stmt_prepare(); }

  inline bool is_stmt_prepare_or_first_sp_execute() const
  { DBUG_ASSERT(0); return Statement::is_stmt_prepare_or_first_sp_execute(); }

  inline bool is_stmt_prepare_or_first_stmt_execute() const
  { DBUG_ASSERT(0); return Statement::is_stmt_prepare_or_first_stmt_execute(); }

  inline bool is_conventional() const
  { DBUG_ASSERT(0); return Statement::is_conventional(); }

  void dec_thread_count(void)
  {
    DBUG_ASSERT(thread_count > 0);
    thread_safe_decrement32(&thread_count);
    signal_thd_deleted();
  }


  void inc_thread_count(void)
  {
    thread_safe_increment32(&thread_count);
  }

public:
  MDL_context mdl_context;

  /* Used to execute base64 coded binlog events in MySQL server */
  Relay_log_info* rli_fake;
  rpl_group_info* rgi_fake;
  /* Slave applier execution context */
  rpl_group_info* rgi_slave;

  union {
    rpl_io_thread_info *rpl_io_info;
    rpl_sql_thread_info *rpl_sql_info;
  } system_thread_info;

  void reset_for_next_command(bool do_clear_errors= 1);
  /*
    Constant for THD::where initialization in the beginning of every query.

    It's needed because we do not save/restore THD::where normally during
    primary (non subselect) query execution.
  */
  static const char * const DEFAULT_WHERE;

#ifdef EMBEDDED_LIBRARY
  struct st_mysql  *mysql;
  unsigned long	 client_stmt_id;
  unsigned long  client_param_count;
  struct st_mysql_bind *client_params;
  char *extra_data;
  ulong extra_length;
  struct st_mysql_data *cur_data;
  struct st_mysql_data *first_data;
  struct st_mysql_data **data_tail;
  void clear_data_list();
  struct st_mysql_data *alloc_new_dataset();
  /*
    In embedded server it points to the statement that is processed
    in the current query. We store some results directly in statement
    fields then.
  */
  struct st_mysql_stmt *current_stmt;
#endif
#ifdef HAVE_QUERY_CACHE
  Query_cache_tls query_cache_tls;
#endif
  NET	  net;				// client connection descriptor
  /** Aditional network instrumentation for the server only. */
  NET_SERVER m_net_server_extension;
  scheduler_functions *scheduler;       // Scheduler for this connection
  Protocol *protocol;			// Current protocol
  Protocol_text   protocol_text;	// Normal protocol
  Protocol_binary protocol_binary;	// Binary protocol
  HASH    user_vars;			// hash for user variables
  String  packet;			// dynamic buffer for network I/O
  String  convert_buffer;               // buffer for charset conversions
  struct  my_rnd_struct rand;		// used for authentication
  struct  system_variables variables;	// Changeable local variables
  struct  system_status_var status_var; // Per thread statistic vars
  struct  system_status_var org_status_var; // For user statistics
  struct  system_status_var *initial_status_var; /* used by show status */
  THR_LOCK_INFO lock_info;              // Locking info of this thread
  /**
    Protects THD data accessed from other threads:
    - thd->query and thd->query_length (used by SHOW ENGINE
      INNODB STATUS and SHOW PROCESSLIST
    - thd->db and thd->db_length (used in SHOW PROCESSLIST)
    - thd->mysys_var (used by KILL statement and shutdown).
    Is locked when THD is deleted.
  */
  mysql_mutex_t LOCK_thd_data;
  /* Protect kill information */
  mysql_mutex_t LOCK_thd_kill;

  /* all prepared statements and cursors of this connection */
  Statement_map stmt_map;

  /* Last created prepared statement */
  Statement *last_stmt;
  inline void set_last_stmt(Statement *stmt)
  { last_stmt= (is_error() ? NULL : stmt); }
  inline void clear_last_stmt() { last_stmt= NULL; }

  /*
    A pointer to the stack frame of handle_one_connection(),
    which is called first in the thread for handling a client
  */
  char	  *thread_stack;

  /**
    Currently selected catalog.
  */
  char *catalog;

  /**
    @note
    Some members of THD (currently 'Statement::db',
    'catalog' and 'query')  are set and alloced by the slave SQL thread
    (for the THD of that thread); that thread is (and must remain, for now)
    the only responsible for freeing these 3 members. If you add members
    here, and you add code to set them in replication, don't forget to
    free_them_and_set_them_to_0 in replication properly. For details see
    the 'err:' label of the handle_slave_sql() in sql/slave.cc.

    @see handle_slave_sql
  */

  Security_context main_security_ctx;
  Security_context *security_ctx;

  /*
    Points to info-string that we show in SHOW PROCESSLIST
    You are supposed to update thd->proc_info only if you have coded
    a time-consuming piece that MySQL can get stuck in for a long time.

    Set it using the  thd_proc_info(THD *thread, const char *message)
    macro/function.

    This member is accessed and assigned without any synchronization.
    Therefore, it may point only to constant (statically
    allocated) strings, which memory won't go away over time.
  */
  const char *proc_info;

private:
  unsigned int m_current_stage_key;

public:
  void enter_stage(const PSI_stage_info *stage,
                   const char *calling_func,
                   const char *calling_file,
                   const unsigned int calling_line)
  {
    DBUG_PRINT("THD::enter_stage", ("%s:%d", calling_file, calling_line));
    DBUG_ASSERT(stage);
    m_current_stage_key= stage->m_key;
    proc_info= stage->m_name;
#if defined(ENABLED_PROFILING)
    profiling.status_change(stage->m_name, calling_func, calling_file,
                            calling_line);
#endif
#ifdef HAVE_PSI_THREAD_INTERFACE
    MYSQL_SET_STAGE(m_current_stage_key, calling_file, calling_line);
#endif
  }

  void backup_stage(PSI_stage_info *stage)
  {
    stage->m_key= m_current_stage_key;
    stage->m_name= proc_info;
  }

  const char *get_proc_info() const
  { return proc_info; }

  /*
    Used in error messages to tell user in what part of MySQL we found an
    error. E. g. when where= "having clause", if fix_fields() fails, user
    will know that the error was in having clause.
  */
  const char *where;

  /* Needed by MariaDB semi sync replication */
  Trans_binlog_info *semisync_info;

  ulonglong client_capabilities;  /* What the client supports */
  ulong max_client_packet_length;

  HASH		handler_tables_hash;
  /*
    A thread can hold named user-level locks. This variable
    contains granted tickets if a lock is present. See item_func.cc and
    chapter 'Miscellaneous functions', for functions GET_LOCK, RELEASE_LOCK.
  */
  HASH ull_hash;
#ifndef DBUG_OFF
  uint dbug_sentry; // watch out for memory corruption
#endif
  struct st_my_thread_var *mysys_var;

  /* Original charset number from the first client packet, or COM_CHANGE_USER*/
  CHARSET_INFO *org_charset;
private:
  /*
    Type of current query: COM_STMT_PREPARE, COM_QUERY, etc. Set from
    first byte of the packet in do_command()
  */
  enum enum_server_command m_command;

public:
  uint32     file_id;			// for LOAD DATA INFILE
  /* remote (peer) port */
  uint16     peer_port;
  my_time_t  start_time;             // start_time and its sec_part 
  ulong      start_time_sec_part;    // are almost always used separately
  my_hrtime_t user_time;
  // track down slow pthread_create
  ulonglong  prior_thr_create_utime, thr_create_utime;
  ulonglong  start_utime, utime_after_lock, utime_after_query;

  // Process indicator
  struct {
    /*
      true, if the currently running command can send progress report
      packets to a client. Set by mysql_execute_command() for safe commands
      See CF_REPORT_PROGRESS
    */
    bool       report_to_client;
    /*
      true, if we will send progress report packets to a client
      (client has requested them, see MARIADB_CLIENT_PROGRESS; report_to_client
      is true; not in sub-statement)
    */
    bool       report;
    uint       stage, max_stage;
    ulonglong  counter, max_counter;
    ulonglong  next_report_time;
    Query_arena *arena;
  } progress;

  thr_lock_type update_lock_default;
  Delayed_insert *di;

  /* <> 0 if we are inside of trigger or stored function. */
  uint in_sub_stmt;
  /* True when opt_userstat_running is set at start of query */
  bool userstat_running;
  /*
    True if we have to log all errors. Are set by some engines to temporary
    force errors to the error log.
  */
  bool log_all_errors;

  /* Do not set socket timeouts for wait_timeout (used with threadpool) */
  bool skip_wait_timeout;

  bool prepare_derived_at_open;

  /* Set to 1 if status of this THD is already in global status */
  bool status_in_global;

  /* 
    To signal that the tmp table to be created is created for materialized
    derived table or a view.
  */ 
  bool create_tmp_table_for_derived;

  bool save_prep_leaf_list;

  /* container for handler's private per-connection data */
  Ha_data ha_data[MAX_HA];

  /**
    Bit field for the state of binlog warnings.

    The first Lex::BINLOG_STMT_UNSAFE_COUNT bits list all types of
    unsafeness that the current statement has.

    This must be a member of THD and not of LEX, because warnings are
    detected and issued in different places (@c
    decide_logging_format() and @c binlog_query(), respectively).
    Between these calls, the THD->lex object may change; e.g., if a
    stored routine is invoked.  Only THD persists between the calls.
  */
  uint32 binlog_unsafe_warning_flags;

#ifndef MYSQL_CLIENT
  binlog_cache_mngr *  binlog_setup_trx_data();

  /*
    Public interface to write RBR events to the binlog
  */
  void binlog_start_trans_and_stmt();
  void binlog_set_stmt_begin();
  int binlog_write_table_map(TABLE *table, bool is_transactional,
                             my_bool *with_annotate= 0);
  int binlog_write_row(TABLE* table, bool is_transactional,
                       const uchar *buf);
  int binlog_delete_row(TABLE* table, bool is_transactional,
                        const uchar *buf);
  int binlog_update_row(TABLE* table, bool is_transactional,
                        const uchar *old_data, const uchar *new_data);
  static void binlog_prepare_row_images(TABLE* table);

  void set_server_id(uint32 sid) { variables.server_id = sid; }

  /*
    Member functions to handle pending event for row-level logging.
  */
  template <class RowsEventT> Rows_log_event*
    binlog_prepare_pending_rows_event(TABLE* table, uint32 serv_id,
                                      size_t needed,
                                      bool is_transactional,
                                      RowsEventT* hint);
  Rows_log_event* binlog_get_pending_rows_event(bool is_transactional) const;
  void binlog_set_pending_rows_event(Rows_log_event* ev, bool is_transactional);
  inline int binlog_flush_pending_rows_event(bool stmt_end)
  {
    return (binlog_flush_pending_rows_event(stmt_end, FALSE) || 
            binlog_flush_pending_rows_event(stmt_end, TRUE));
  }
  int binlog_flush_pending_rows_event(bool stmt_end, bool is_transactional);
  int binlog_remove_pending_rows_event(bool clear_maps, bool is_transactional);

  /**
    Determine the binlog format of the current statement.

    @retval 0 if the current statement will be logged in statement
    format.
    @retval nonzero if the current statement will be logged in row
    format.
   */
  int is_current_stmt_binlog_format_row() const {
    DBUG_ASSERT(current_stmt_binlog_format == BINLOG_FORMAT_STMT ||
                current_stmt_binlog_format == BINLOG_FORMAT_ROW);
    return current_stmt_binlog_format == BINLOG_FORMAT_ROW;
  }
  /**
    Determine if binlogging is disabled for this session
    @retval 0 if the current statement binlogging is disabled
              (could be because of binlog closed/binlog option
               is set to false).
    @retval 1 if the current statement will be binlogged
  */
  inline bool is_current_stmt_binlog_disabled() const
  {
    return (!(variables.option_bits & OPTION_BIN_LOG) ||
            !mysql_bin_log.is_open());
  }

  enum binlog_filter_state
  {
    BINLOG_FILTER_UNKNOWN,
    BINLOG_FILTER_CLEAR,
    BINLOG_FILTER_SET
  };

  inline void reset_binlog_local_stmt_filter()
  {
    m_binlog_filter_state= BINLOG_FILTER_UNKNOWN;
  }

  inline void clear_binlog_local_stmt_filter()
  {
    DBUG_ASSERT(m_binlog_filter_state == BINLOG_FILTER_UNKNOWN);
    m_binlog_filter_state= BINLOG_FILTER_CLEAR;
  }

  inline void set_binlog_local_stmt_filter()
  {
    DBUG_ASSERT(m_binlog_filter_state == BINLOG_FILTER_UNKNOWN);
    m_binlog_filter_state= BINLOG_FILTER_SET;
  }

  inline binlog_filter_state get_binlog_local_stmt_filter()
  {
    return m_binlog_filter_state;
  }

private:
  /**
    Indicate if the current statement should be discarded
    instead of written to the binlog.
    This is used to discard special statements, such as
    DML or DDL that affects only 'local' (non replicated)
    tables, such as performance_schema.*
  */
  binlog_filter_state m_binlog_filter_state;

  /**
    Indicates the format in which the current statement will be
    logged.  This can only be set from @c decide_logging_format().
  */
  enum_binlog_format current_stmt_binlog_format;

  /*
    Number of outstanding table maps, i.e., table maps in the
    transaction cache.
  */
  uint binlog_table_maps;
public:
  void issue_unsafe_warnings();

  uint get_binlog_table_maps() const {
    return binlog_table_maps;
  }
  void clear_binlog_table_maps() {
    binlog_table_maps= 0;
  }
#endif /* MYSQL_CLIENT */

public:

  struct st_transactions {
    SAVEPOINT *savepoints;
    THD_TRANS all;			// Trans since BEGIN WORK
    THD_TRANS stmt;			// Trans for current statement
    bool on;                            // see ha_enable_transaction()
    XID_STATE xid_state;
    WT_THD wt;                          ///< for deadlock detection
    Rows_log_event *m_pending_rows_event;

    /*
       Tables changed in transaction (that must be invalidated in query cache).
       List contain only transactional tables, that not invalidated in query
       cache (instead of full list of changed in transaction tables).
    */
    CHANGED_TABLE_LIST* changed_tables;
    MEM_ROOT mem_root; // Transaction-life memory allocation pool
    void cleanup()
    {
      DBUG_ENTER("thd::cleanup");
      changed_tables= 0;
      savepoints= 0;
      /*
        If rm_error is raised, it means that this piece of a distributed
        transaction has failed and must be rolled back. But the user must
        rollback it explicitly, so don't start a new distributed XA until
        then.
      */
      if (!xid_state.rm_error)
        xid_state.xid.null();
      free_root(&mem_root,MYF(MY_KEEP_PREALLOC));
      DBUG_VOID_RETURN;
    }
    my_bool is_active()
    {
      return (all.ha_list != NULL);
    }
    st_transactions()
    {
      bzero((char*)this, sizeof(*this));
      xid_state.xid.null();
      init_sql_alloc(&mem_root, ALLOC_ROOT_MIN_BLOCK_SIZE, 0,
                     MYF(MY_THREAD_SPECIFIC));
    }
  } transaction;
  Global_read_lock global_read_lock;
  Field      *dup_field;
#ifndef __WIN__
  sigset_t signals;
#endif
#ifdef SIGNAL_WITH_VIO_CLOSE
  Vio* active_vio;
#endif

  /*
    A permanent memory area of the statement. For conventional
    execution, the parsed tree and execution runtime reside in the same
    memory root. In this case stmt_arena points to THD. In case of
    a prepared statement or a stored procedure statement, thd->mem_root
    conventionally points to runtime memory, and thd->stmt_arena
    points to the memory of the PS/SP, where the parsed tree of the
    statement resides. Whenever you need to perform a permanent
    transformation of a parsed tree, you should allocate new memory in
    stmt_arena, to allow correct re-execution of PS/SP.
    Note: in the parser, stmt_arena == thd, even for PS/SP.
  */
  Query_arena *stmt_arena;

  void *bulk_param;

  /*
    map for tables that will be updated for a multi-table update query
    statement, for other query statements, this will be zero.
  */
  table_map table_map_for_update;

  /* Tells if LAST_INSERT_ID(#) was called for the current statement */
  bool arg_of_last_insert_id_function;
  /*
    ALL OVER THIS FILE, "insert_id" means "*automatically generated* value for
    insertion into an auto_increment column".
  */
  /*
    This is the first autogenerated insert id which was *successfully*
    inserted by the previous statement (exactly, if the previous statement
    didn't successfully insert an autogenerated insert id, then it's the one
    of the statement before, etc).
    It can also be set by SET LAST_INSERT_ID=# or SELECT LAST_INSERT_ID(#).
    It is returned by LAST_INSERT_ID().
  */
  ulonglong  first_successful_insert_id_in_prev_stmt;
  /*
    Variant of the above, used for storing in statement-based binlog. The
    difference is that the one above can change as the execution of a stored
    function progresses, while the one below is set once and then does not
    change (which is the value which statement-based binlog needs).
  */
  ulonglong  first_successful_insert_id_in_prev_stmt_for_binlog;
  /*
    This is the first autogenerated insert id which was *successfully*
    inserted by the current statement. It is maintained only to set
    first_successful_insert_id_in_prev_stmt when statement ends.
  */
  ulonglong  first_successful_insert_id_in_cur_stmt;
  /*
    We follow this logic:
    - when stmt starts, first_successful_insert_id_in_prev_stmt contains the
    first insert id successfully inserted by the previous stmt.
    - as stmt makes progress, handler::insert_id_for_cur_row changes;
    every time get_auto_increment() is called,
    auto_inc_intervals_in_cur_stmt_for_binlog is augmented with the
    reserved interval (if statement-based binlogging).
    - at first successful insertion of an autogenerated value,
    first_successful_insert_id_in_cur_stmt is set to
    handler::insert_id_for_cur_row.
    - when stmt goes to binlog,
    auto_inc_intervals_in_cur_stmt_for_binlog is binlogged if
    non-empty.
    - when stmt ends, first_successful_insert_id_in_prev_stmt is set to
    first_successful_insert_id_in_cur_stmt.
  */
  /*
    stmt_depends_on_first_successful_insert_id_in_prev_stmt is set when
    LAST_INSERT_ID() is used by a statement.
    If it is set, first_successful_insert_id_in_prev_stmt_for_binlog will be
    stored in the statement-based binlog.
    This variable is CUMULATIVE along the execution of a stored function or
    trigger: if one substatement sets it to 1 it will stay 1 until the
    function/trigger ends, thus making sure that
    first_successful_insert_id_in_prev_stmt_for_binlog does not change anymore
    and is propagated to the caller for binlogging.
  */
  bool       stmt_depends_on_first_successful_insert_id_in_prev_stmt;
  /*
    List of auto_increment intervals reserved by the thread so far, for
    storage in the statement-based binlog.
    Note that its minimum is not first_successful_insert_id_in_cur_stmt:
    assuming a table with an autoinc column, and this happens:
    INSERT INTO ... VALUES(3);
    SET INSERT_ID=3; INSERT IGNORE ... VALUES (NULL);
    then the latter INSERT will insert no rows
    (first_successful_insert_id_in_cur_stmt == 0), but storing "INSERT_ID=3"
    in the binlog is still needed; the list's minimum will contain 3.
    This variable is cumulative: if several statements are written to binlog
    as one (stored functions or triggers are used) this list is the
    concatenation of all intervals reserved by all statements.
  */
  Discrete_intervals_list auto_inc_intervals_in_cur_stmt_for_binlog;
  /* Used by replication and SET INSERT_ID */
  Discrete_intervals_list auto_inc_intervals_forced;
  /*
    There is BUG#19630 where statement-based replication of stored
    functions/triggers with two auto_increment columns breaks.
    We however ensure that it works when there is 0 or 1 auto_increment
    column; our rules are
    a) on master, while executing a top statement involving substatements,
    first top- or sub- statement to generate auto_increment values wins the
    exclusive right to see its values be written to binlog (the write
    will be done by the statement or its caller), and the losers won't see
    their values be written to binlog.
    b) on slave, while replicating a top statement involving substatements,
    first top- or sub- statement to need to read auto_increment values from
    the master's binlog wins the exclusive right to read them (so the losers
    won't read their values from binlog but instead generate on their own).
    a) implies that we mustn't backup/restore
    auto_inc_intervals_in_cur_stmt_for_binlog.
    b) implies that we mustn't backup/restore auto_inc_intervals_forced.

    If there are more than 1 auto_increment columns, then intervals for
    different columns may mix into the
    auto_inc_intervals_in_cur_stmt_for_binlog list, which is logically wrong,
    but there is no point in preventing this mixing by preventing intervals
    from the secondly inserted column to come into the list, as such
    prevention would be wrong too.
    What will happen in the case of
    INSERT INTO t1 (auto_inc) VALUES(NULL);
    where t1 has a trigger which inserts into an auto_inc column of t2, is
    that in binlog we'll store the interval of t1 and the interval of t2 (when
    we store intervals, soon), then in slave, t1 will use both intervals, t2
    will use none; if t1 inserts the same number of rows as on master,
    normally the 2nd interval will not be used by t1, which is fine. t2's
    values will be wrong if t2's internal auto_increment counter is different
    from what it was on master (which is likely). In 5.1, in mixed binlogging
    mode, row-based binlogging is used for such cases where two
    auto_increment columns are inserted.
  */
  inline void record_first_successful_insert_id_in_cur_stmt(ulonglong id_arg)
  {
    if (first_successful_insert_id_in_cur_stmt == 0)
      first_successful_insert_id_in_cur_stmt= id_arg;
  }
  inline ulonglong read_first_successful_insert_id_in_prev_stmt(void)
  {
    if (!stmt_depends_on_first_successful_insert_id_in_prev_stmt)
    {
      /* It's the first time we read it */
      first_successful_insert_id_in_prev_stmt_for_binlog=
        first_successful_insert_id_in_prev_stmt;
      stmt_depends_on_first_successful_insert_id_in_prev_stmt= 1;
    }
    return first_successful_insert_id_in_prev_stmt;
  }
  /*
    Used by Intvar_log_event::do_apply_event() and by "SET INSERT_ID=#"
    (mysqlbinlog). We'll soon add a variant which can take many intervals in
    argument.
  */
  inline void force_one_auto_inc_interval(ulonglong next_id)
  {
    auto_inc_intervals_forced.empty(); // in case of multiple SET INSERT_ID
    auto_inc_intervals_forced.append(next_id, ULONGLONG_MAX, 0);
  }

  ulonglong  limit_found_rows;

private:
  /**
    Stores the result of ROW_COUNT() function.

    ROW_COUNT() function is a MySQL extention, but we try to keep it
    similar to ROW_COUNT member of the GET DIAGNOSTICS stack of the SQL
    standard (see SQL99, part 2, search for ROW_COUNT). It's value is
    implementation defined for anything except INSERT, DELETE, UPDATE.

    ROW_COUNT is assigned according to the following rules:

      - In my_ok():
        - for DML statements: to the number of affected rows;
        - for DDL statements: to 0.

      - In my_eof(): to -1 to indicate that there was a result set.

        We derive this semantics from the JDBC specification, where int
        java.sql.Statement.getUpdateCount() is defined to (sic) "return the
        current result as an update count; if the result is a ResultSet
        object or there are no more results, -1 is returned".

      - In my_error(): to -1 to be compatible with the MySQL C API and
        MySQL ODBC driver.

      - For SIGNAL statements: to 0 per WL#2110 specification (see also
        sql_signal.cc comment). Zero is used since that's the "default"
        value of ROW_COUNT in the diagnostics area.
  */

  longlong m_row_count_func;    /* For the ROW_COUNT() function */

public:
  inline longlong get_row_count_func() const
  {
    return m_row_count_func;
  }

  inline void set_row_count_func(longlong row_count_func)
  {
    m_row_count_func= row_count_func;
  }

  ha_rows    cuted_fields;

private:
  /*
    number of rows we actually sent to the client, including "synthetic"
    rows in ROLLUP etc.
  */
  ha_rows    m_sent_row_count;

  /**
    Number of rows read and/or evaluated for a statement. Used for
    slow log reporting.

    An examined row is defined as a row that is read and/or evaluated
    according to a statement condition, including in
    create_sort_index(). Rows may be counted more than once, e.g., a
    statement including ORDER BY could possibly evaluate the row in
    filesort() before reading it for e.g. update.
  */
  ha_rows    m_examined_row_count;

public:
  ha_rows get_sent_row_count() const
  { return m_sent_row_count; }

  ha_rows get_examined_row_count() const
  { return m_examined_row_count; }

  void set_sent_row_count(ha_rows count);
  void set_examined_row_count(ha_rows count);

  void inc_sent_row_count(ha_rows count);
  void inc_examined_row_count(ha_rows count);

  void inc_status_created_tmp_disk_tables();
  void inc_status_created_tmp_files();
  void inc_status_created_tmp_tables();
  void inc_status_select_full_join();
  void inc_status_select_full_range_join();
  void inc_status_select_range();
  void inc_status_select_range_check();
  void inc_status_select_scan();
  void inc_status_sort_merge_passes();
  void inc_status_sort_range();
  void inc_status_sort_rows(ha_rows count);
  void inc_status_sort_scan();
  void set_status_no_index_used();
  void set_status_no_good_index_used();

  /**
    The number of rows and/or keys examined by the query, both read,
    changed or written.
  */
  ulonglong accessed_rows_and_keys;

  /**
    Check if the number of rows accessed by a statement exceeded
    LIMIT ROWS EXAMINED. If so, signal the query engine to stop execution.
  */
  void check_limit_rows_examined()
  {
    if (++accessed_rows_and_keys > lex->limit_rows_examined_cnt)
      set_killed(ABORT_QUERY);
  }

  USER_CONN *user_connect;
  CHARSET_INFO *db_charset;
#if defined(ENABLED_PROFILING)
  PROFILING  profiling;
#endif

  /** Current statement digest. */
  sql_digest_state *m_digest;
  /** Current statement digest token array. */
  unsigned char *m_token_array;
  /** Top level statement digest. */
  sql_digest_state m_digest_state;

  /** Current statement instrumentation. */
  PSI_statement_locker *m_statement_psi;
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  /** Current statement instrumentation state. */
  PSI_statement_locker_state m_statement_state;
#endif /* HAVE_PSI_STATEMENT_INTERFACE */
  /** Idle instrumentation. */
  PSI_idle_locker *m_idle_psi;
#ifdef HAVE_PSI_IDLE_INTERFACE
  /** Idle instrumentation state. */
  PSI_idle_locker_state m_idle_state;
#endif /* HAVE_PSI_IDLE_INTERFACE */

  /*
    Id of current query. Statement can be reused to execute several queries
    query_id is global in context of the whole MySQL server.
    ID is automatically generated from mutex-protected counter.
    It's used in handler code for various purposes: to check which columns
    from table are necessary for this select, to check if it's necessary to
    update auto-updatable fields (like auto_increment and timestamp).
  */
  query_id_t query_id;
  ulong      col_access;

  /* Statement id is thread-wide. This counter is used to generate ids */
  ulong      statement_id_counter;
  ulong	     rand_saved_seed1, rand_saved_seed2;
  ulong      query_plan_flags; 
  ulong      query_plan_fsort_passes; 
  pthread_t  real_id;                           /* For debugging */
  my_thread_id  thread_id, thread_dbug_id;
  uint32      os_thread_id;
  uint	     tmp_table, global_disable_checkpoint;
  uint	     server_status,open_options;
  enum enum_thread_type system_thread;
  /*
    Current or next transaction isolation level.
    When a connection is established, the value is taken from
    @@session.tx_isolation (default transaction isolation for
    the session), which is in turn taken from @@global.tx_isolation
    (the global value).
    If there is no transaction started, this variable
    holds the value of the next transaction's isolation level.
    When a transaction starts, the value stored in this variable
    becomes "actual".
    At transaction commit or rollback, we assign this variable
    again from @@session.tx_isolation.
    The only statement that can otherwise change the value
    of this variable is SET TRANSACTION ISOLATION LEVEL.
    Its purpose is to effect the isolation level of the next
    transaction in this session. When this statement is executed,
    the value in this variable is changed. However, since
    this statement is only allowed when there is no active
    transaction, this assignment (naturally) only affects the
    upcoming transaction.
    At the end of the current active transaction the value is
    be reset again from @@session.tx_isolation, as described
    above.
  */
  enum_tx_isolation tx_isolation;
  /*
    Current or next transaction access mode.
    See comment above regarding tx_isolation.
  */
  bool              tx_read_only;
  enum_check_fields count_cuted_fields;

  DYNAMIC_ARRAY user_var_events;        /* For user variables replication */
  MEM_ROOT      *user_var_events_alloc; /* Allocate above array elements here */

  /*
    Define durability properties that engines may check to
    improve performance. Not yet used in MariaDB
  */
  enum durability_properties durability_property;
 
  /*
    If checking this in conjunction with a wait condition, please
    include a check after enter_cond() if you want to avoid a race
    condition. For details see the implementation of awake(),
    especially the "broadcast" part.
  */
  killed_state volatile killed;

  /*
    The following is used if one wants to have a specific error number and
    text for the kill
  */
  struct err_info
  {
    int no;
    const char msg[256];
  } *killed_err;

  /* See also thd_killed() */
  inline bool check_killed()
  {
    if (killed)
      return TRUE;
    if (apc_target.have_apc_requests())
      apc_target.process_apc_requests(); 
    return FALSE;
  }

  /* scramble - random string sent to client on handshake */
  char	     scramble[SCRAMBLE_LENGTH+1];

  /*
    If this is a slave, the name of the connection stored here.
    This is used for taging error messages in the log files.
  */
  LEX_STRING connection_name;
  char       default_master_connection_buff[MAX_CONNECTION_NAME+1];
  uint8      password; /* 0, 1 or 2 */
  uint8      failed_com_change_user;
  bool       slave_thread;
  bool       extra_port;                        /* If extra connection */

  bool	     no_errors;

  /**
    Set to TRUE if execution of the current compound statement
    can not continue. In particular, disables activation of
    CONTINUE or EXIT handlers of stored routines.
    Reset in the end of processing of the current user request, in
    @see THD::reset_for_next_command().
  */
  bool is_fatal_error;
  /**
    Set by a storage engine to request the entire
    transaction (that possibly spans multiple engines) to
    rollback. Reset in ha_rollback.
  */
  bool       transaction_rollback_request;
  /**
    TRUE if we are in a sub-statement and the current error can
    not be safely recovered until we left the sub-statement mode.
    In particular, disables activation of CONTINUE and EXIT
    handlers inside sub-statements. E.g. if it is a deadlock
    error and requires a transaction-wide rollback, this flag is
    raised (traditionally, MySQL first has to close all the reads
    via @see handler::ha_index_or_rnd_end() and only then perform
    the rollback).
    Reset to FALSE when we leave the sub-statement mode.
  */
  bool       is_fatal_sub_stmt_error;
  bool	     query_start_used, rand_used, time_zone_used;
  bool       query_start_sec_part_used;
  /* for IS NULL => = last_insert_id() fix in remove_eq_conds() */
  bool       substitute_null_with_insert_id;
  bool	     in_lock_tables;
  bool       bootstrap, cleanup_done, free_connection_done;

  /**  is set if some thread specific value(s) used in a statement. */
  bool       thread_specific_used;
  /**  
    is set if a statement accesses a temporary table created through
    CREATE TEMPORARY TABLE. 
  */
  bool	     charset_is_system_charset, charset_is_collation_connection;
  bool       charset_is_character_set_filesystem;
  bool       enable_slow_log;   /* enable slow log for current statement */
  bool	     abort_on_warning;
  bool 	     got_warning;       /* Set on call to push_warning() */
  /* set during loop of derived table processing */
  bool       derived_tables_processing;
  bool       tablespace_op;	/* This is TRUE in DISCARD/IMPORT TABLESPACE */
  /* True if we have to log the current statement */
  bool	     log_current_statement;
  /**
    True if a slave error. Causes the slave to stop. Not the same
    as the statement execution error (is_error()), since
    a statement may be expected to return an error, e.g. because
    it returned an error on master, and this is OK on the slave.
  */
  bool       is_slave_error;
  /*
    True when a transaction is queued up for binlog group commit.
    Used so that if another transaction needs to wait for a row lock held by
    this transaction, it can signal to trigger the group commit immediately,
    skipping the normal --binlog-commit-wait-count wait.
  */
  bool waiting_on_group_commit;
  /*
    Set true when another transaction goes to wait on a row lock held by this
    transaction. Used together with waiting_on_group_commit.
  */
  bool has_waiter;
  /*
    In case of a slave, set to the error code the master got when executing
    the query. 0 if no error on the master.
  */
  int	     slave_expected_error;

  sp_rcontext *spcont;		// SP runtime context
  sp_cache   *sp_proc_cache;
  sp_cache   *sp_func_cache;

  /** number of name_const() substitutions, see sp_head.cc:subst_spvars() */
  uint       query_name_consts;

  /*
    If we do a purge of binary logs, log index info of the threads
    that are currently reading it needs to be adjusted. To do that
    each thread that is using LOG_INFO needs to adjust the pointer to it
  */
  LOG_INFO*  current_linfo;
  NET*       slave_net;			// network connection from slave -> m.

  /*
    Used to update global user stats.  The global user stats are updated
    occasionally with the 'diff' variables.  After the update, the 'diff'
    variables are reset to 0.
  */
  /* Time when the current thread connected to MySQL. */
  time_t current_connect_time;
  /* Last time when THD stats were updated in global_user_stats. */
  time_t last_global_update_time;
  /* Number of commands not reflected in global_user_stats yet. */
  uint select_commands, update_commands, other_commands;
  ulonglong start_cpu_time;
  ulonglong start_bytes_received;

  /* Used by the sys_var class to store temporary values */
  union
  {
    my_bool   my_bool_value;
    int       int_value;
    uint      uint_value;
    long      long_value;
    ulong     ulong_value;
    ulonglong ulonglong_value;
    double    double_value;
    void      *ptr_value;
  } sys_var_tmp;

  struct {
    /*
      If true, mysql_bin_log::write(Log_event) call will not write events to
      binlog, and maintain 2 below variables instead (use
      mysql_bin_log.start_union_events to turn this on)
    */
    bool do_union;
    /*
      If TRUE, at least one mysql_bin_log::write(Log_event) call has been
      made after last mysql_bin_log.start_union_events() call.
    */
    bool unioned_events;
    /*
      If TRUE, at least one mysql_bin_log::write(Log_event e), where
      e.cache_stmt == TRUE call has been made after last
      mysql_bin_log.start_union_events() call.
    */
    bool unioned_events_trans;

    /*
      'queries' (actually SP statements) that run under inside this binlog
      union have thd->query_id >= first_query_id.
    */
    query_id_t first_query_id;
  } binlog_evt_union;

  mysql_cond_t              COND_wsrep_thd;
  /**
    Internal parser state.
    Note that since the parser is not re-entrant, we keep only one parser
    state here. This member is valid only when executing code during parsing.
  */
  Parser_state *m_parser_state;

  Locked_tables_list locked_tables_list;

#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_info *work_part_info;
#endif

#ifndef EMBEDDED_LIBRARY
  /**
    Array of active audit plugins which have been used by this THD.
    This list is later iterated to invoke release_thd() on those
    plugins.
  */
  DYNAMIC_ARRAY audit_class_plugins;
  /**
    Array of bits indicating which audit classes have already been
    added to the list of audit plugins which are currently in use.
  */
  unsigned long audit_class_mask[MYSQL_AUDIT_CLASS_MASK_SIZE];
  int audit_plugin_version;
#endif

#if defined(ENABLED_DEBUG_SYNC)
  /* Debug Sync facility. See debug_sync.cc. */
  struct st_debug_sync_control *debug_sync_control;
#endif /* defined(ENABLED_DEBUG_SYNC) */
  THD(my_thread_id id, bool is_wsrep_applier= false);

  ~THD();

  void init(void);
  /*
    Initialize memory roots necessary for query processing and (!)
    pre-allocate memory for it. We can't do that in THD constructor because
    there are use cases (acl_init, delayed inserts, watcher threads,
    killing mysqld) where it's vital to not allocate excessive and not used
    memory. Note, that we still don't return error from init_for_queries():
    if preallocation fails, we should notice that at the first call to
    alloc_root.
  */
  void init_for_queries();
  void update_all_stats();
  void update_stats(void);
  void change_user(void);
  void cleanup(void);
  void cleanup_after_query();
  void free_connection();
  void reset_for_reuse();
  bool store_globals();
  void reset_globals();
#ifdef SIGNAL_WITH_VIO_CLOSE
  inline void set_active_vio(Vio* vio)
  {
    mysql_mutex_lock(&LOCK_thd_data);
    active_vio = vio;
    mysql_mutex_unlock(&LOCK_thd_data);
  }
  inline void clear_active_vio()
  {
    mysql_mutex_lock(&LOCK_thd_data);
    active_vio = 0;
    mysql_mutex_unlock(&LOCK_thd_data);
  }
  void close_active_vio();
#endif
  void awake(killed_state state_to_set);
 
  /** Disconnect the associated communication endpoint. */
  void disconnect();


  /*
    Allows this thread to serve as a target for others to schedule Async 
    Procedure Calls on.

    It's possible to schedule any code to be executed this way, by
    inheriting from the Apc_call object. Currently, only
    Show_explain_request uses this.
  */
  Apc_target apc_target;

#ifndef MYSQL_CLIENT
  enum enum_binlog_query_type {
    /* The query can be logged in row format or in statement format. */
    ROW_QUERY_TYPE,
    
    /* The query has to be logged in statement format. */
    STMT_QUERY_TYPE,
    
    QUERY_TYPE_COUNT
  };

  int binlog_query(enum_binlog_query_type qtype,
                   char const *query, ulong query_len, bool is_trans,
                   bool direct, bool suppress_use,
                   int errcode);
#endif

  inline void
  enter_cond(mysql_cond_t *cond, mysql_mutex_t* mutex,
             const PSI_stage_info *stage, PSI_stage_info *old_stage,
             const char *src_function, const char *src_file,
             int src_line)
  {
    mysql_mutex_assert_owner(mutex);
    mysys_var->current_mutex = mutex;
    mysys_var->current_cond = cond;
    if (old_stage)
      backup_stage(old_stage);
    if (stage)
      enter_stage(stage, src_function, src_file, src_line);
  }
  inline void exit_cond(const PSI_stage_info *stage,
                        const char *src_function, const char *src_file,
                        int src_line)
  {
    /*
      Putting the mutex unlock in thd->exit_cond() ensures that
      mysys_var->current_mutex is always unlocked _before_ mysys_var->mutex is
      locked (if that would not be the case, you'll get a deadlock if someone
      does a THD::awake() on you).
    */
    mysql_mutex_unlock(mysys_var->current_mutex);
    mysql_mutex_lock(&mysys_var->mutex);
    mysys_var->current_mutex = 0;
    mysys_var->current_cond = 0;
    if (stage)
      enter_stage(stage, src_function, src_file, src_line);
    mysql_mutex_unlock(&mysys_var->mutex);
    return;
  }
  virtual int is_killed() { return killed; }
  virtual THD* get_thd() { return this; }

  /**
    A callback to the server internals that is used to address
    special cases of the locking protocol.
    Invoked when acquiring an exclusive lock, for each thread that
    has a conflicting shared metadata lock.

    This function:
    - aborts waiting of the thread on a data lock, to make it notice
      the pending exclusive lock and back off.
    - if the thread is an INSERT DELAYED thread, sends it a KILL
      signal to terminate it.

    @note This function does not wait for the thread to give away its
          locks. Waiting is done outside for all threads at once.

    @param ctx_in_use           The MDL context owner (thread) to wake up.
    @param needs_thr_lock_abort Indicates that to wake up thread
                                this call needs to abort its waiting
                                on table-level lock.

    @retval  TRUE  if the thread was woken up
    @retval  FALSE otherwise.
   */
  virtual bool notify_shared_lock(MDL_context_owner *ctx_in_use,
                                  bool needs_thr_lock_abort);

  // End implementation of MDL_context_owner interface.

  inline bool is_strict_mode() const
  {
    return (bool) (variables.sql_mode & (MODE_STRICT_TRANS_TABLES |
                                         MODE_STRICT_ALL_TABLES));
  }
  inline bool backslash_escapes() const
  {
    return !MY_TEST(variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES);
  }
  inline my_time_t query_start() { query_start_used=1; return start_time; }
  inline ulong query_start_sec_part()
  { query_start_sec_part_used=1; return start_time_sec_part; }
  inline void set_current_time()
  {
    my_hrtime_t hrtime= my_hrtime();
    start_time= hrtime_to_my_time(hrtime);
    start_time_sec_part= hrtime_sec_part(hrtime);
#ifdef HAVE_PSI_THREAD_INTERFACE
    PSI_THREAD_CALL(set_thread_start_time)(start_time);
#endif
  }
  inline void set_start_time()
  {
    if (user_time.val)
    {
      start_time= hrtime_to_my_time(user_time);
      start_time_sec_part= hrtime_sec_part(user_time);
#ifdef HAVE_PSI_THREAD_INTERFACE
      PSI_THREAD_CALL(set_thread_start_time)(start_time);
#endif
    }
    else
      set_current_time();
  }
  inline void set_time()
  {
    set_start_time();
    start_utime= utime_after_lock= microsecond_interval_timer();
  }
  inline void set_time(my_hrtime_t t)
  {
    user_time= t;
    set_time();
  }
  inline void set_time(my_time_t t, ulong sec_part)
  {
    my_hrtime_t hrtime= { hrtime_from_time(t) + sec_part };
    set_time(hrtime);
  }
  void set_time_after_lock()
  {
    utime_after_lock= microsecond_interval_timer();
    MYSQL_SET_STATEMENT_LOCK_TIME(m_statement_psi,
                                  (utime_after_lock - start_utime));
  }
  ulonglong current_utime()  { return microsecond_interval_timer(); }

  /* Tell SHOW PROCESSLIST to show time from this point */
  inline void set_time_for_next_stage()
  {
    utime_after_query= current_utime();
  }

  /**
   Update server status after execution of a top level statement.
   Currently only checks if a query was slow, and assigns
   the status accordingly.
   Evaluate the current time, and if it exceeds the long-query-time
   setting, mark the query as slow.
  */
  void update_server_status()
  {
    set_time_for_next_stage();
    if (utime_after_query > utime_after_lock + variables.long_query_time)
      server_status|= SERVER_QUERY_WAS_SLOW;
  }
  inline ulonglong found_rows(void)
  {
    return limit_found_rows;
  }
  /**
    Returns TRUE if session is in a multi-statement transaction mode.

    OPTION_NOT_AUTOCOMMIT: When autocommit is off, a multi-statement
    transaction is implicitly started on the first statement after a
    previous transaction has been ended.

    OPTION_BEGIN: Regardless of the autocommit status, a multi-statement
    transaction can be explicitly started with the statements "START
    TRANSACTION", "BEGIN [WORK]", "[COMMIT | ROLLBACK] AND CHAIN", etc.

    Note: this doesn't tell you whether a transaction is active.
    A session can be in multi-statement transaction mode, and yet
    have no active transaction, e.g., in case of:
    set @@autocommit=0;
    set @a= 3;                                     <-- these statements don't
    set transaction isolation level serializable;  <-- start an active
    flush tables;                                  <-- transaction

    I.e. for the above scenario this function returns TRUE, even
    though no active transaction has begun.
    @sa in_active_multi_stmt_transaction()
  */
  inline bool in_multi_stmt_transaction_mode()
  {
    return variables.option_bits & (OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
  }
  /**
    TRUE if the session is in a multi-statement transaction mode
    (@sa in_multi_stmt_transaction_mode()) *and* there is an
    active transaction, i.e. there is an explicit start of a
    transaction with BEGIN statement, or implicit with a
    statement that uses a transactional engine.

    For example, these scenarios don't start an active transaction
    (even though the server is in multi-statement transaction mode):

    set @@autocommit=0;
    select * from nontrans_table;
    set @var=TRUE;
    flush tables;

    Note, that even for a statement that starts a multi-statement
    transaction (i.e. select * from trans_table), this
    flag won't be set until we open the statement's tables
    and the engines register themselves for the transaction
    (see trans_register_ha()),
    hence this method is reliable to use only after
    open_tables() has completed.

    Why do we need a flag?
    ----------------------
    We need to maintain a (at first glance redundant)
    session flag, rather than looking at thd->transaction.all.ha_list
    because of explicit start of a transaction with BEGIN. 

    I.e. in case of
    BEGIN;
    select * from nontrans_t1; <-- in_active_multi_stmt_transaction() is true
  */
  inline bool in_active_multi_stmt_transaction()
  {
    return server_status & SERVER_STATUS_IN_TRANS;
  }
  inline bool fill_derived_tables()
  {
    return !stmt_arena->is_stmt_prepare() && !lex->only_view_structure();
  }
  inline bool fill_information_schema_tables()
  {
    return !stmt_arena->is_stmt_prepare();
  }
  inline void* trans_alloc(unsigned int size)
  {
    return alloc_root(&transaction.mem_root,size);
  }

  LEX_STRING *make_lex_string(LEX_STRING *lex_str, const char* str, uint length)
  {
    if (!(lex_str->str= strmake_root(mem_root, str, length)))
      return 0;
    lex_str->length= length;
    return lex_str;
  }

  LEX_STRING *make_lex_string(const char* str, uint length)
  {
    LEX_STRING *lex_str;
    if (!(lex_str= (LEX_STRING *)alloc_root(mem_root, sizeof(LEX_STRING))))
      return 0;
    return make_lex_string(lex_str, str, length);
  }

  // Allocate LEX_STRING for character set conversion
  bool alloc_lex_string(LEX_STRING *dst, uint length)
  {
    if ((dst->str= (char*) alloc(length)))
      return false;
    dst->length= 0;  // Safety
    return true;     // EOM
  }
  bool convert_string(LEX_STRING *to, CHARSET_INFO *to_cs,
		      const char *from, uint from_length,
		      CHARSET_INFO *from_cs);
  /*
    Convert a strings between character sets.
    Uses my_convert_fix(), which uses an mb_wc .. mc_mb loop internally.
    dstcs and srccs cannot be &my_charset_bin.
  */
  bool convert_fix(CHARSET_INFO *dstcs, LEX_STRING *dst,
                   CHARSET_INFO *srccs, const char *src, uint src_length,
                   String_copier *status);

  /*
    Same as above, but additionally sends ER_INVALID_CHARACTER_STRING
    in case of bad byte sequences or Unicode conversion problems.
  */
  bool convert_with_error(CHARSET_INFO *dstcs, LEX_STRING *dst,
                          CHARSET_INFO *srccs,
                          const char *src, uint src_length);

  /*
    If either "dstcs" or "srccs" is &my_charset_bin,
    then performs native copying using cs->cset->copy_fix().
    Otherwise, performs Unicode conversion using convert_fix().
  */
  bool copy_fix(CHARSET_INFO *dstcs, LEX_STRING *dst,
                CHARSET_INFO *srccs, const char *src, uint src_length,
                String_copier *status);

  /*
    Same as above, but additionally sends ER_INVALID_CHARACTER_STRING
    in case of bad byte sequences or Unicode conversion problems.
  */
  bool copy_with_error(CHARSET_INFO *dstcs, LEX_STRING *dst,
                       CHARSET_INFO *srccs, const char *src, uint src_length);

  bool convert_string(String *s, CHARSET_INFO *from_cs, CHARSET_INFO *to_cs);

  void add_changed_table(TABLE *table);
  void add_changed_table(const char *key, long key_length);
  CHANGED_TABLE_LIST * changed_table_dup(const char *key, long key_length);
  int prepare_explain_fields(select_result *result, List<Item> *field_list,
                             uint8 explain_flags, bool is_analyze);
  int send_explain_fields(select_result *result, uint8 explain_flags,
                          bool is_analyze);
  void make_explain_field_list(List<Item> &field_list, uint8 explain_flags,
                               bool is_analyze);
  void make_explain_json_field_list(List<Item> &field_list, bool is_analyze);

  /**
    Clear the current error, if any.
    We do not clear is_fatal_error or is_fatal_sub_stmt_error since we
    assume this is never called if the fatal error is set.

    @todo: To silence an error, one should use Internal_error_handler
    mechanism. Issuing an error that can be possibly later "cleared" is not
    compatible with other installed error handlers and audit plugins.
  */
  inline void clear_error(bool clear_diagnostics= 0)
  {
    DBUG_ENTER("clear_error");
    if (get_stmt_da()->is_error() || clear_diagnostics)
      get_stmt_da()->reset_diagnostics_area();
    is_slave_error= 0;
    if (killed == KILL_BAD_DATA)
      reset_killed();
    DBUG_VOID_RETURN;
  }

#ifndef EMBEDDED_LIBRARY
  inline bool vio_ok() const { return net.vio != 0; }
  /** Return FALSE if connection to client is broken. */
  bool is_connected()
  {
    /*
      All system threads (e.g., the slave IO thread) are connected but
      not using vio. So this function always returns true for all
      system threads.
    */
    return system_thread || (vio_ok() ? vio_is_connected(net.vio) : FALSE);
  }
#else
  inline bool vio_ok() const { return TRUE; }
  inline bool is_connected() { return TRUE; }
#endif
  /**
    Mark the current error as fatal. Warning: this does not
    set any error, it sets a property of the error, so must be
    followed or prefixed with my_error().
  */
  inline void fatal_error()
  {
    DBUG_ASSERT(get_stmt_da()->is_error() || killed);
    is_fatal_error= 1;
    DBUG_PRINT("error",("Fatal error set"));
  }
  /**
    TRUE if there is an error in the error stack.

    Please use this method instead of direct access to
    net.report_error.

    If TRUE, the current (sub)-statement should be aborted.
    The main difference between this member and is_fatal_error
    is that a fatal error can not be handled by a stored
    procedure continue handler, whereas a normal error can.

    To raise this flag, use my_error().
  */
  inline bool is_error() const { return m_stmt_da->is_error(); }
  void set_bulk_execution(void *bulk)
  {
    bulk_param= bulk;
    m_stmt_da->set_bulk_execution(MY_TEST(bulk));
  }
  bool is_bulk_op() const { return MY_TEST(bulk_param); }

  /// Returns Diagnostics-area for the current statement.
  Diagnostics_area *get_stmt_da()
  { return m_stmt_da; }

  /// Returns Diagnostics-area for the current statement.
  const Diagnostics_area *get_stmt_da() const
  { return m_stmt_da; }

  /// Sets Diagnostics-area for the current statement.
  void set_stmt_da(Diagnostics_area *da)
  { m_stmt_da= da; }

  inline CHARSET_INFO *charset() { return variables.character_set_client; }
  void update_charset();
  void update_charset(CHARSET_INFO *character_set_client,
                      CHARSET_INFO *collation_connection)
  {
    variables.character_set_client= character_set_client;
    variables.collation_connection= collation_connection;
    update_charset();
  }
  void update_charset(CHARSET_INFO *character_set_client,
                      CHARSET_INFO *collation_connection,
                      CHARSET_INFO *character_set_results)
  {
    variables.character_set_client= character_set_client;
    variables.collation_connection= collation_connection;
    variables.character_set_results= character_set_results;
    update_charset();
  }

  inline Query_arena *activate_stmt_arena_if_needed(Query_arena *backup)
  {
    /*
      Use the persistent arena if we are in a prepared statement or a stored
      procedure statement and we have not already changed to use this arena.
    */
    if (!stmt_arena->is_conventional() && mem_root != stmt_arena->mem_root)
    {
      set_n_backup_active_arena(stmt_arena, backup);
      return stmt_arena;
    }
    return 0;
  }


  bool is_item_tree_change_register_required()
  {
    return !stmt_arena->is_conventional()
           || stmt_arena->type() == Query_arena::TABLE_ARENA;
  }

  void change_item_tree(Item **place, Item *new_value)
  {
    DBUG_ENTER("THD::change_item_tree");
    DBUG_PRINT("enter", ("Register: %p (%p) <- %p",
                       *place, place, new_value));
    /* TODO: check for OOM condition here */
    if (is_item_tree_change_register_required())
      nocheck_register_item_tree_change(place, *place, mem_root);
    *place= new_value;
    DBUG_VOID_RETURN;
  }
  /**
    Make change in item tree after checking whether it needs registering


    @param place         place where we should assign new value
    @param new_value     place of the new value

    @details
    see check_and_register_item_tree_change details
  */
  void check_and_register_item_tree(Item **place, Item **new_value)
  {
    if (!stmt_arena->is_conventional())
      check_and_register_item_tree_change(place, new_value, mem_root);
    /*
      We have to use memcpy instead of  *place= *new_value merge to
      avoid problems with strict aliasing.
    */
    memcpy((char*) place, new_value, sizeof(*new_value));
  }

  /*
    Cleanup statement parse state (parse tree, lex) and execution
    state after execution of a non-prepared SQL statement.
  */
  void end_statement();

  /*
    Mark thread to be killed, with optional error number and string.
    string is not released, so it has to be allocted on thd mem_root
    or be a global string

    Ensure that we don't replace a kill with a lesser one. For example
    if user has done 'kill_connection' we shouldn't replace it with
    KILL_QUERY.
  */
  inline void set_killed(killed_state killed_arg,
                         int killed_errno_arg= 0,
                         const char *killed_err_msg_arg= 0)
  {
    mysql_mutex_lock(&LOCK_thd_kill);
    set_killed_no_mutex(killed_arg, killed_errno_arg, killed_err_msg_arg);
    mysql_mutex_unlock(&LOCK_thd_kill);
  }
  /*
    This is only used by THD::awake where we need to keep the lock mutex
    locked over some time.
    It's ok to have this inline, as in most cases killed_errno_arg will
    be a constant 0 and most of the function will disappear.
  */
  inline void set_killed_no_mutex(killed_state killed_arg,
                                  int killed_errno_arg= 0,
                                  const char *killed_err_msg_arg= 0)
  {
    if (killed <= killed_arg)
    {
      killed= killed_arg;
      if (killed_errno_arg)
      {
        /*
          If alloc fails, we only remember the killed flag.
          The worst things that can happen is that we get
          a suboptimal error message.
        */
        killed_err= (err_info*) alloc_root(&main_mem_root, sizeof(*killed_err));
        if (killed_err)
        {
          killed_err->no= killed_errno_arg;
          ::strmake((char*) killed_err->msg, killed_err_msg_arg,
                    sizeof(killed_err->msg)-1);
        }
      }
    }
  }
  int killed_errno();
  inline void reset_killed()
  {
    /*
      Resetting killed has to be done under a mutex to ensure
      its not done during an awake() call.
    */
    if (killed != NOT_KILLED)
    {
      mysql_mutex_lock(&LOCK_thd_kill);
      killed= NOT_KILLED;
      killed_err= 0;
      mysql_mutex_unlock(&LOCK_thd_kill);
    }
  }
  inline void reset_kill_query()
  {
    if (killed < KILL_CONNECTION)
    {
      reset_killed();
      mysys_var->abort= 0;
    }
  }
  inline void send_kill_message()
  {
    mysql_mutex_lock(&LOCK_thd_kill);
    int err= killed_errno();
    if (err)
      my_message(err, killed_err ? killed_err->msg : ER_THD(this, err),
                 MYF(0));
    mysql_mutex_unlock(&LOCK_thd_kill);
  }
  /* return TRUE if we will abort query if we make a warning now */
  inline bool really_abort_on_warning()
  {
    return (abort_on_warning &&
            (!transaction.stmt.modified_non_trans_table ||
             (variables.sql_mode & MODE_STRICT_ALL_TABLES)));
  }
  void set_status_var_init();
  void reset_n_backup_open_tables_state(Open_tables_backup *backup);
  void restore_backup_open_tables_state(Open_tables_backup *backup);
  void reset_sub_statement_state(Sub_statement_state *backup, uint new_state);
  void restore_sub_statement_state(Sub_statement_state *backup);
  void set_n_backup_active_arena(Query_arena *set, Query_arena *backup);
  void restore_active_arena(Query_arena *set, Query_arena *backup);

  inline void get_binlog_format(enum_binlog_format *format,
                                enum_binlog_format *current_format)
  {
    *format= (enum_binlog_format) variables.binlog_format;
    *current_format= current_stmt_binlog_format;
  }
  inline enum_binlog_format get_current_stmt_binlog_format()
  {
    return current_stmt_binlog_format;
  }
  inline void set_binlog_format(enum_binlog_format format,
                                enum_binlog_format current_format)
  {
    DBUG_ENTER("set_binlog_format");
    variables.binlog_format= format;
    current_stmt_binlog_format= current_format;
    DBUG_VOID_RETURN;
  }
  inline void set_binlog_format_stmt()
  {
    DBUG_ENTER("set_binlog_format_stmt");
    variables.binlog_format=    BINLOG_FORMAT_STMT;
    current_stmt_binlog_format= BINLOG_FORMAT_STMT;
    DBUG_VOID_RETURN;
  }
  /*
    @todo Make these methods private or remove them completely.  Only
    decide_logging_format should call them. /Sven
  */
  inline void set_current_stmt_binlog_format_row_if_mixed()
  {
    DBUG_ENTER("set_current_stmt_binlog_format_row_if_mixed");
    /*
      This should only be called from decide_logging_format.

      @todo Once we have ensured this, uncomment the following
      statement, remove the big comment below that, and remove the
      in_sub_stmt==0 condition from the following 'if'.
    */
    /* DBUG_ASSERT(in_sub_stmt == 0); */
    /*
      If in a stored/function trigger, the caller should already have done the
      change. We test in_sub_stmt to prevent introducing bugs where people
      wouldn't ensure that, and would switch to row-based mode in the middle
      of executing a stored function/trigger (which is too late, see also
      reset_current_stmt_binlog_format_row()); this condition will make their
      tests fail and so force them to propagate the
      lex->binlog_row_based_if_mixed upwards to the caller.
    */
    if ((wsrep_binlog_format() == BINLOG_FORMAT_MIXED) && (in_sub_stmt == 0))
      set_current_stmt_binlog_format_row();

    DBUG_VOID_RETURN;
  }

  inline void set_current_stmt_binlog_format_row()
  {
    DBUG_ENTER("set_current_stmt_binlog_format_row");
    current_stmt_binlog_format= BINLOG_FORMAT_ROW;
    DBUG_VOID_RETURN;
  }
  /* Set binlog format temporarily to statement. Returns old format */
  inline enum_binlog_format set_current_stmt_binlog_format_stmt()
  {
    enum_binlog_format orig_format= current_stmt_binlog_format;
    DBUG_ENTER("set_current_stmt_binlog_format_stmt");
    current_stmt_binlog_format= BINLOG_FORMAT_STMT;
    DBUG_RETURN(orig_format);
  }
  inline void restore_stmt_binlog_format(enum_binlog_format format)
  {
    DBUG_ENTER("restore_stmt_binlog_format");
    DBUG_ASSERT(!is_current_stmt_binlog_format_row());
    current_stmt_binlog_format= format;
    DBUG_VOID_RETURN;
  }
  inline void reset_current_stmt_binlog_format_row()
  {
    DBUG_ENTER("reset_current_stmt_binlog_format_row");
    /*
      If there are temporary tables, don't reset back to
      statement-based. Indeed it could be that:
      CREATE TEMPORARY TABLE t SELECT UUID(); # row-based
      # and row-based does not store updates to temp tables
      # in the binlog.
      INSERT INTO u SELECT * FROM t; # stmt-based
      and then the INSERT will fail as data inserted into t was not logged.
      So we continue with row-based until the temp table is dropped.
      If we are in a stored function or trigger, we mustn't reset in the
      middle of its execution (as the binary logging way of a stored function
      or trigger is decided when it starts executing, depending for example on
      the caller (for a stored function: if caller is SELECT or
      INSERT/UPDATE/DELETE...).
    */
    DBUG_PRINT("debug",
               ("temporary_tables: %s, in_sub_stmt: %s, system_thread: %s",
                YESNO(has_thd_temporary_tables()), YESNO(in_sub_stmt),
                show_system_thread(system_thread)));
    if (in_sub_stmt == 0)
    {
      if (wsrep_binlog_format() == BINLOG_FORMAT_ROW)
        set_current_stmt_binlog_format_row();
      else if (!has_thd_temporary_tables())
        set_current_stmt_binlog_format_stmt();
    }
    DBUG_VOID_RETURN;
  }

  /**
    Set the current database; use deep copy of C-string.

    @param new_db     a pointer to the new database name.
    @param new_db_len length of the new database name.

    Initialize the current database from a NULL-terminated string with
    length. If we run out of memory, we free the current database and
    return TRUE.  This way the user will notice the error as there will be
    no current database selected (in addition to the error message set by
    malloc).

    @note This operation just sets {db, db_length}. Switching the current
    database usually involves other actions, like switching other database
    attributes including security context. In the future, this operation
    will be made private and more convenient interface will be provided.

    @return Operation status
      @retval FALSE Success
      @retval TRUE  Out-of-memory error
  */
  bool set_db(const char *new_db, size_t new_db_len)
  {
    /*
      Acquiring mutex LOCK_thd_data as we either free the memory allocated
      for the database and reallocating the memory for the new db or memcpy
      the new_db to the db.
    */
    mysql_mutex_lock(&LOCK_thd_data);
    /* Do not reallocate memory if current chunk is big enough. */
    if (db && new_db && db_length >= new_db_len)
      memcpy(db, new_db, new_db_len+1);
    else
    {
      my_free(db);
      if (new_db)
        db= my_strndup(new_db, new_db_len, MYF(MY_WME | ME_FATALERROR));
      else
        db= NULL;
    }
    db_length= db ? new_db_len : 0;
    bool result= new_db && !db;
    mysql_mutex_unlock(&LOCK_thd_data);
#ifdef HAVE_PSI_THREAD_INTERFACE
    if (result)
      PSI_THREAD_CALL(set_thread_db)(new_db, (int) new_db_len);
#endif
    return result;
  }

  /**
    Set the current database; use shallow copy of C-string.

    @param new_db     a pointer to the new database name.
    @param new_db_len length of the new database name.

    @note This operation just sets {db, db_length}. Switching the current
    database usually involves other actions, like switching other database
    attributes including security context. In the future, this operation
    will be made private and more convenient interface will be provided.
  */
  void reset_db(char *new_db, size_t new_db_len)
  {
    if (new_db != db || new_db_len != db_length)
    {
      mysql_mutex_lock(&LOCK_thd_data);
      db= new_db;
      db_length= new_db_len;
      mysql_mutex_unlock(&LOCK_thd_data);
#ifdef HAVE_PSI_THREAD_INTERFACE
      PSI_THREAD_CALL(set_thread_db)(new_db, (int) new_db_len);
#endif
    }
  }
  /*
    Copy the current database to the argument. Use the current arena to
    allocate memory for a deep copy: current database may be freed after
    a statement is parsed but before it's executed.
  */
  bool copy_db_to(char **p_db, size_t *p_db_length)
  {
    if (db == NULL)
    {
      /*
        No default database is set. In this case if it's guaranteed that
        no CTE can be used in the statement then we can throw an error right
        now at the parser stage. Otherwise the decision about throwing such
        a message must be postponed until a post-parser stage when we are able
        to resolve all CTE names as we don't need this message to be thrown
        for any CTE references.
      */
      if (!lex->with_clauses_list)
      {
        my_message(ER_NO_DB_ERROR, ER(ER_NO_DB_ERROR), MYF(0));
        return TRUE;
      }
      /* This will allow to throw an error later for non-CTE references */
      *p_db= NULL;
      *p_db_length= 0;
    }
    else
    {
     *p_db= strmake(db, db_length);
     *p_db_length= db_length;
    }
    return FALSE;
  }
  thd_scheduler event_scheduler;

public:
  inline Internal_error_handler *get_internal_handler()
  { return m_internal_handler; }

  /**
    Add an internal error handler to the thread execution context.
    @param handler the exception handler to add
  */
  void push_internal_handler(Internal_error_handler *handler);

private:
  /**
    Handle a sql condition.
    @param sql_errno the condition error number
    @param sqlstate the condition sqlstate
    @param level the condition level
    @param msg the condition message text
    @param[out] cond_hdl the sql condition raised, if any
    @return true if the condition is handled
  */
  bool handle_condition(uint sql_errno,
                        const char* sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl);

public:
  /**
    Remove the error handler last pushed.
  */
  Internal_error_handler *pop_internal_handler();

  /**
    Raise an exception condition.
    @param code the MYSQL_ERRNO error code of the error
  */
  void raise_error(uint code);

  /**
    Raise an exception condition, with a formatted message.
    @param code the MYSQL_ERRNO error code of the error
  */
  void raise_error_printf(uint code, ...);

  /**
    Raise a completion condition (warning).
    @param code the MYSQL_ERRNO error code of the warning
  */
  void raise_warning(uint code);

  /**
    Raise a completion condition (warning), with a formatted message.
    @param code the MYSQL_ERRNO error code of the warning
  */
  void raise_warning_printf(uint code, ...);

  /**
    Raise a completion condition (note), with a fixed message.
    @param code the MYSQL_ERRNO error code of the note
  */
  void raise_note(uint code);

  /**
    Raise an completion condition (note), with a formatted message.
    @param code the MYSQL_ERRNO error code of the note
  */
  void raise_note_printf(uint code, ...);

private:
  /*
    Only the implementation of the SIGNAL and RESIGNAL statements
    is permitted to raise SQL conditions in a generic way,
    or to raise them by bypassing handlers (RESIGNAL).
    To raise a SQL condition, the code should use the public
    raise_error() or raise_warning() methods provided by class THD.
  */
  friend class Sql_cmd_common_signal;
  friend class Sql_cmd_signal;
  friend class Sql_cmd_resignal;
  friend void push_warning(THD*, Sql_condition::enum_warning_level, uint, const char*);
  friend void my_message_sql(uint, const char *, myf);

  /**
    Raise a generic SQL condition.
    @param sql_errno the condition error number
    @param sqlstate the condition SQLSTATE
    @param level the condition level
    @param msg the condition message text
    @return The condition raised, or NULL
  */
  Sql_condition*
  raise_condition(uint sql_errno,
                  const char* sqlstate,
                  Sql_condition::enum_warning_level level,
                  const char* msg);

public:
  /** Overloaded to guard query/query_length fields */
  virtual void set_statement(Statement *stmt);
  void set_command(enum enum_server_command command)
  {
    m_command= command;
#ifdef HAVE_PSI_THREAD_INTERFACE
    PSI_STATEMENT_CALL(set_thread_command)(m_command);
#endif
  }
  inline enum enum_server_command get_command() const
  { return m_command; }

  /**
    Assign a new value to thd->query and thd->query_id and mysys_var.
    Protected with LOCK_thd_data mutex.
  */
  void set_query(char *query_arg, uint32 query_length_arg,
                 CHARSET_INFO *cs_arg)
  {
    set_query(CSET_STRING(query_arg, query_length_arg, cs_arg));
  }
  void set_query(char *query_arg, uint32 query_length_arg) /*Mutex protected*/
  {
    set_query(CSET_STRING(query_arg, query_length_arg, charset()));
  }
  void set_query(const CSET_STRING &string_arg)
  {
    mysql_mutex_lock(&LOCK_thd_data);
    set_query_inner(string_arg);
    mysql_mutex_unlock(&LOCK_thd_data);

#ifdef HAVE_PSI_THREAD_INTERFACE
    PSI_THREAD_CALL(set_thread_info)(query(), query_length());
#endif
  }
  void reset_query()               /* Mutex protected */
  { set_query(CSET_STRING()); }
  void set_query_and_id(char *query_arg, uint32 query_length_arg,
                        CHARSET_INFO *cs, query_id_t new_query_id);
  void set_query_id(query_id_t new_query_id)
  {
    query_id= new_query_id;
  }
  void set_open_tables(TABLE *open_tables_arg)
  {
    mysql_mutex_lock(&LOCK_thd_data);
    open_tables= open_tables_arg;
    mysql_mutex_unlock(&LOCK_thd_data);
  }
  void set_mysys_var(struct st_my_thread_var *new_mysys_var);
  void enter_locked_tables_mode(enum_locked_tables_mode mode_arg)
  {
    DBUG_ASSERT(locked_tables_mode == LTM_NONE);

    if (mode_arg == LTM_LOCK_TABLES)
    {
      /*
        When entering LOCK TABLES mode we should set explicit duration
        for all metadata locks acquired so far in order to avoid releasing
        them till UNLOCK TABLES statement.
        We don't do this when entering prelocked mode since sub-statements
        don't release metadata locks and restoring status-quo after leaving
        prelocking mode gets complicated.
      */
      mdl_context.set_explicit_duration_for_all_locks();
    }

    locked_tables_mode= mode_arg;
  }
  void leave_locked_tables_mode();
  /* Relesae transactional locks if there are no active transactions */
  void release_transactional_locks()
  {
    if (!(server_status &
          (SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY)))
      mdl_context.release_transactional_locks();
  }
  int decide_logging_format(TABLE_LIST *tables);
  /*
   In Some cases when decide_logging_format is called it does not have all
   information to decide the logging format. So that cases we call decide_logging_format_2
   at later stages in execution.
   One example would be binlog format for IODKU but column with unique key is not inserted.
   We dont have inserted columns info when we call decide_logging_format so on later stage we call
   decide_logging_format_low

   @returns 0 if no format is changed
            1 if there is change in binlog format
  */
  int decide_logging_format_low(TABLE *table);

  enum need_invoker { INVOKER_NONE=0, INVOKER_USER, INVOKER_ROLE};
  void binlog_invoker(bool role) { m_binlog_invoker= role ? INVOKER_ROLE : INVOKER_USER; }
  enum need_invoker need_binlog_invoker() { return m_binlog_invoker; }
  void get_definer(LEX_USER *definer, bool role);
  void set_invoker(const LEX_STRING *user, const LEX_STRING *host)
  {
    invoker_user= *user;
    invoker_host= *host;
  }
  LEX_STRING get_invoker_user() { return invoker_user; }
  LEX_STRING get_invoker_host() { return invoker_host; }
  bool has_invoker() { return invoker_user.length > 0; }

  void print_aborted_warning(uint threshold, const char *reason)
  {
    if (global_system_variables.log_warnings > threshold)
    {
      Security_context *sctx= &main_security_ctx;
      sql_print_warning(ER_THD(this, ER_NEW_ABORTING_CONNECTION),
                        thread_id, (db ? db : "unconnected"),
                        sctx->user ? sctx->user : "unauthenticated",
                        sctx->host_or_ip, reason);
    }
  }

public:
  void clear_wakeup_ready() { wakeup_ready= false; }
  /*
    Sleep waiting for others to wake us up with signal_wakeup_ready().
    Must call clear_wakeup_ready() before waiting.
  */
  void wait_for_wakeup_ready();
  /* Wake this thread up from wait_for_wakeup_ready(). */
  void signal_wakeup_ready();

  void add_status_to_global()
  {
    DBUG_ASSERT(status_in_global == 0);
    mysql_mutex_lock(&LOCK_status);
    add_to_status(&global_status_var, &status_var);
    /* Mark that this THD status has already been added in global status */
    status_var.global_memory_used= 0;
    status_in_global= 1;
    mysql_mutex_unlock(&LOCK_status);
  }

  wait_for_commit *wait_for_commit_ptr;
  int wait_for_prior_commit()
  {
    if (wait_for_commit_ptr)
      return wait_for_commit_ptr->wait_for_prior_commit(this);
    return 0;
  }
  void wakeup_subsequent_commits(int wakeup_error)
  {
    if (wait_for_commit_ptr)
      wait_for_commit_ptr->wakeup_subsequent_commits(wakeup_error);
  }
  wait_for_commit *suspend_subsequent_commits() {
    wait_for_commit *suspended= wait_for_commit_ptr;
    wait_for_commit_ptr= NULL;
    return suspended;
  }
  void resume_subsequent_commits(wait_for_commit *suspended) {
    DBUG_ASSERT(!wait_for_commit_ptr);
    wait_for_commit_ptr= suspended;
  }

  void mark_transaction_to_rollback(bool all);
private:

  /** The current internal error handler for this thread, or NULL. */
  Internal_error_handler *m_internal_handler;

  /**
    The lex to hold the parsed tree of conventional (non-prepared) queries.
    Whereas for prepared and stored procedure statements we use an own lex
    instance for each new query, for conventional statements we reuse
    the same lex. (@see mysql_parse for details).
  */
  LEX main_lex;
  /**
    This memory root is used for two purposes:
    - for conventional queries, to allocate structures stored in main_lex
    during parsing, and allocate runtime data (execution plan, etc.)
    during execution.
    - for prepared queries, only to allocate runtime data. The parsed
    tree itself is reused between executions and thus is stored elsewhere.
  */
  MEM_ROOT main_mem_root;
  Diagnostics_area main_da;
  Diagnostics_area *m_stmt_da;

  /**
    It will be set if CURRENT_USER() or CURRENT_ROLE() is called in account
    management statements or default definer is set in CREATE/ALTER SP, SF,
    Event, TRIGGER or VIEW statements.

    Current user or role will be binlogged into Query_log_event if
    m_binlog_invoker is not NONE; It will be stored into invoker_host and
    invoker_user by SQL thread.
   */
  enum need_invoker m_binlog_invoker;

  /**
    It points to the invoker in the Query_log_event.
    SQL thread use it as the default definer in CREATE/ALTER SP, SF, Event,
    TRIGGER or VIEW statements or current user in account management
    statements if it is not NULL.
   */
  LEX_STRING invoker_user;
  LEX_STRING invoker_host;

public:
#ifndef EMBEDDED_LIBRARY
  Session_tracker session_tracker;
#endif //EMBEDDED_LIBRARY
  /*
    Flag, mutex and condition for a thread to wait for a signal from another
    thread.

    Currently used to wait for group commit to complete, can also be used for
    other purposes.
  */
  bool wakeup_ready;
  mysql_mutex_t LOCK_wakeup_ready;
  mysql_cond_t COND_wakeup_ready;
  /*
    The GTID assigned to the last commit. If no GTID was assigned to any commit
    so far, this is indicated by last_commit_gtid.seq_no == 0.
  */
private:
  rpl_gtid m_last_commit_gtid;

public:
  rpl_gtid get_last_commit_gtid() { return m_last_commit_gtid; }
  void set_last_commit_gtid(rpl_gtid &gtid);


  LF_PINS *tdc_hash_pins;
  LF_PINS *xid_hash_pins;
  bool fix_xid_hash_pins();

/* Members related to temporary tables. */
public:
  /* Opened table states. */
  enum Temporary_table_state {
    TMP_TABLE_IN_USE,
    TMP_TABLE_NOT_IN_USE,
    TMP_TABLE_ANY
  };
  bool has_thd_temporary_tables();

  TABLE *create_and_open_tmp_table(handlerton *hton,
                                   LEX_CUSTRING *frm,
                                   const char *path,
                                   const char *db,
                                   const char *table_name,
                                   bool open_in_engine);

  TABLE *find_temporary_table(const char *db, const char *table_name,
                              Temporary_table_state state= TMP_TABLE_IN_USE);
  TABLE *find_temporary_table(const TABLE_LIST *tl,
                              Temporary_table_state state= TMP_TABLE_IN_USE);

  TMP_TABLE_SHARE *find_tmp_table_share_w_base_key(const char *key,
                                                   uint key_length);
  TMP_TABLE_SHARE *find_tmp_table_share(const char *db,
                                        const char *table_name);
  TMP_TABLE_SHARE *find_tmp_table_share(const TABLE_LIST *tl);
  TMP_TABLE_SHARE *find_tmp_table_share(const char *key, uint key_length);

  bool open_temporary_table(TABLE_LIST *tl);
  bool open_temporary_tables(TABLE_LIST *tl);

  bool close_temporary_tables();
  bool rename_temporary_table(TABLE *table, const char *db,
                              const char *table_name);
  bool drop_temporary_table(TABLE *table, bool *is_trans, bool delete_table);
  bool rm_temporary_table(handlerton *hton, const char *path);
  void mark_tmp_tables_as_free_for_reuse();
  void mark_tmp_table_as_free_for_reuse(TABLE *table);

  TMP_TABLE_SHARE* save_tmp_table_share(TABLE *table);
  void restore_tmp_table_share(TMP_TABLE_SHARE *share);
  void close_unused_temporary_table_instances(const TABLE_LIST *tl);

private:
  /* Whether a lock has been acquired? */
  bool m_tmp_tables_locked;

  bool has_temporary_tables();
  uint create_tmp_table_def_key(char *key, const char *db,
                                const char *table_name);
  TMP_TABLE_SHARE *create_temporary_table(handlerton *hton, LEX_CUSTRING *frm,
                                          const char *path, const char *db,
                                          const char *table_name);
  TABLE *find_temporary_table(const char *key, uint key_length,
                              Temporary_table_state state);
  TABLE *open_temporary_table(TMP_TABLE_SHARE *share, const char *alias,
                              bool open_in_engine);
  bool find_and_use_tmp_table(const TABLE_LIST *tl, TABLE **out_table);
  bool use_temporary_table(TABLE *table, TABLE **out_table);
  void close_temporary_table(TABLE *table);
  bool log_events_and_free_tmp_shares();
  void free_tmp_table_share(TMP_TABLE_SHARE *share, bool delete_table);
  void free_temporary_table(TABLE *table);
  bool lock_temporary_tables();
  void unlock_temporary_tables();

  inline uint tmpkeyval(TMP_TABLE_SHARE *share)
  {
    return uint4korr(share->table_cache_key.str +
                     share->table_cache_key.length - 4);
  }

  inline TMP_TABLE_SHARE *tmp_table_share(TABLE *table)
  {
    DBUG_ASSERT(table->s->tmp_table);
    return static_cast<TMP_TABLE_SHARE *>(table->s);
  }

public:
  inline ulong wsrep_binlog_format() const
  {
    return WSREP_FORMAT(variables.binlog_format);
  }

#ifdef WITH_WSREP
  const bool                wsrep_applier; /* dedicated slave applier thread */
  bool                      wsrep_applier_closing; /* applier marked to close */
  bool                      wsrep_client_thread; /* to identify client threads*/
  bool                      wsrep_PA_safe;
  bool                      wsrep_converted_lock_session;
  bool                      wsrep_apply_toi; /* applier processing in TOI */
  enum wsrep_exec_mode      wsrep_exec_mode;
  query_id_t                wsrep_last_query_id;
  enum wsrep_query_state    wsrep_query_state;
  enum wsrep_conflict_state wsrep_conflict_state;
  wsrep_trx_meta_t          wsrep_trx_meta;
  uint32                    wsrep_rand;
  Relay_log_info            *wsrep_rli;
  rpl_group_info            *wsrep_rgi;
  wsrep_ws_handle_t         wsrep_ws_handle;
  ulong                     wsrep_retry_counter; // of autocommit
  char                      *wsrep_retry_query;
  size_t                    wsrep_retry_query_len;
  enum enum_server_command  wsrep_retry_command;
  enum wsrep_consistency_check_mode
                            wsrep_consistency_check;
  int                       wsrep_mysql_replicated;
  const char                *wsrep_TOI_pre_query; /* a query to apply before
                                                     the actual TOI query */
  size_t                    wsrep_TOI_pre_query_len;
  wsrep_po_handle_t         wsrep_po_handle;
  size_t                    wsrep_po_cnt;
#ifdef GTID_SUPPORT
  rpl_sid                   wsrep_po_sid;
#endif /*  GTID_SUPPORT */
  void                      *wsrep_apply_format;
  char                      wsrep_info[128]; /* string for dynamic proc info */
  /*
    When enabled, do not replicate/binlog updates from the current table that's
    being processed. At the moment, it is used to keep mysql.gtid_slave_pos
    table updates from being replicated to other nodes via galera replication.
  */
  bool                      wsrep_ignore_table;
  wsrep_gtid_t              wsrep_sync_wait_gtid;
  ulong                     wsrep_affected_rows;
  bool                      wsrep_replicate_GTID;
  bool                      wsrep_skip_wsrep_GTID;
  /* This flag is set when innodb do an intermediate commit to
  processing the LOAD DATA INFILE statement by splitting it into 10K
  rows chunks. If flag is set, then binlog rotation is not performed
  while intermediate transaction try to commit, because in this case
  rotation causes unregistration of innodb handler. Later innodb handler
  registered again, but replication of last chunk of rows is skipped
  by the innodb engine: */
  bool                      wsrep_split_flag;
#endif /* WITH_WSREP */

  /* Handling of timeouts for commands */
  thr_timer_t query_timer;
public:
  void set_query_timer()
  {
#ifndef EMBEDDED_LIBRARY
    /*
      Don't start a query timer if
      - If timeouts are not set
      - if we are in a stored procedure or sub statement
      - If this is a slave thread
      - If we already have set a timeout (happens when running prepared
        statements that calls mysql_execute_command())
    */
    if (!variables.max_statement_time || spcont  || in_sub_stmt ||
        slave_thread || query_timer.expired == 0)
      return;
    thr_timer_settime(&query_timer, variables.max_statement_time);
#endif
  }
  void reset_query_timer()
  {
#ifndef EMBEDDED_LIBRARY
    if (spcont || in_sub_stmt || slave_thread)
      return;
    if (!query_timer.expired)
      thr_timer_end(&query_timer);
#endif
  }
  void restore_set_statement_var()
  {
    main_lex.restore_set_statement_var();
  }
  /* Copy relevant `stmt` transaction flags to `all` transaction. */
  void merge_unsafe_rollback_flags()
  {
    if (transaction.stmt.modified_non_trans_table)
      transaction.all.modified_non_trans_table= TRUE;
    transaction.all.m_unsafe_rollback_flags|=
      (transaction.stmt.m_unsafe_rollback_flags &
       (THD_TRANS::DID_WAIT | THD_TRANS::CREATED_TEMP_TABLE |
        THD_TRANS::DROPPED_TEMP_TABLE | THD_TRANS::DID_DDL));
  }
  /*
    Reset current_linfo
    Setting current_linfo to 0 needs to be done with LOCK_thread_count to
    ensure that adjust_linfo_offsets doesn't use a structure that may
    be deleted.
  */
  inline void reset_current_linfo()
  {
    mysql_mutex_lock(&LOCK_thread_count);
    current_linfo= 0;
    mysql_mutex_unlock(&LOCK_thread_count);
  }
};

inline void add_to_active_threads(THD *thd)
{
  mysql_mutex_lock(&LOCK_thread_count);
  threads.append(thd);
  mysql_mutex_unlock(&LOCK_thread_count);
}

/*
  This should be called when you want to delete a thd that was not
  running any queries.
  This function will assert that the THD is linked.
*/

inline void unlink_not_visible_thd(THD *thd)
{
  thd->assert_linked();
  mysql_mutex_lock(&LOCK_thread_count);
  thd->unlink();
  mysql_mutex_unlock(&LOCK_thread_count);
}

/** A short cut for thd->get_stmt_da()->set_ok_status(). */

inline void
my_ok(THD *thd, ulonglong affected_rows= 0, ulonglong id= 0,
        const char *message= NULL)
{
  thd->set_row_count_func(affected_rows);
  thd->get_stmt_da()->set_ok_status(affected_rows, id, message);
}


/** A short cut for thd->get_stmt_da()->set_eof_status(). */

inline void
my_eof(THD *thd)
{
  thd->set_row_count_func(-1);
  thd->get_stmt_da()->set_eof_status(thd);

  TRANSACT_TRACKER(add_trx_state(thd, TX_RESULT_SET));
}

#define tmp_disable_binlog(A)                                              \
  {ulonglong tmp_disable_binlog__save_options= (A)->variables.option_bits; \
  (A)->variables.option_bits&= ~OPTION_BIN_LOG;                            \
  (A)->variables.sql_log_bin_off= 1;

#define reenable_binlog(A)                                                  \
  (A)->variables.option_bits= tmp_disable_binlog__save_options;             \
  (A)->variables.sql_log_bin_off= 0;}


inline sql_mode_t sql_mode_for_dates(THD *thd)
{
  return thd->variables.sql_mode &
          (MODE_NO_ZERO_DATE | MODE_NO_ZERO_IN_DATE | MODE_INVALID_DATES);
}

/*
  Used to hold information about file and file structure in exchange
  via non-DB file (...INTO OUTFILE..., ...LOAD DATA...)
  XXX: We never call destructor for objects of this class.
*/

class sql_exchange :public Sql_alloc
{
public:
  enum enum_filetype filetype; /* load XML, Added by Arnold & Erik */
  char *file_name;
  String *field_term,*enclosed,*line_term,*line_start,*escaped;
  bool opt_enclosed;
  bool dumpfile;
  ulong skip_lines;
  CHARSET_INFO *cs;
  sql_exchange(char *name, bool dumpfile_flag,
               enum_filetype filetype_arg= FILETYPE_CSV);
  bool escaped_given(void);
};

/*
  This is used to get result from a select
*/

class JOIN;

/* Pure interface for sending tabular data */
class select_result_sink: public Sql_alloc
{
public:
  THD *thd;
  select_result_sink(THD *thd_arg): thd(thd_arg) {}
  /*
    send_data returns 0 on ok, 1 on error and -1 if data was ignored, for
    example for a duplicate row entry written to a temp table.
  */
  virtual int send_data(List<Item> &items)=0;
  virtual ~select_result_sink() {};
};

class select_result_interceptor;

/*
  Interface for sending tabular data, together with some other stuff:

  - Primary purpose seems to be seding typed tabular data:
     = the DDL is sent with send_fields()
     = the rows are sent with send_data()
  Besides that,
  - there seems to be an assumption that the sent data is a result of 
    SELECT_LEX_UNIT *unit,
  - nest_level is used by SQL parser
*/

class select_result :public select_result_sink 
{
protected:
  /* 
    All descendant classes have their send_data() skip the first 
    unit->offset_limit_cnt rows sent.  Select_materialize
    also uses unit->get_column_types().
  */
  SELECT_LEX_UNIT *unit;
  /* Something used only by the parser: */
public:
  select_result(THD *thd_arg): select_result_sink(thd_arg) {}
  void set_unit(SELECT_LEX_UNIT *unit_arg) { unit= unit_arg; }
  virtual ~select_result() {};
  /**
    Change wrapped select_result.

    Replace the wrapped result object with new_result and call
    prepare() and prepare2() on new_result.

    This base class implementation doesn't wrap other select_results.

    @param new_result The new result object to wrap around

    @retval false Success
    @retval true  Error
  */
  virtual bool change_result(select_result *new_result)
  {
    return false;
  }
  virtual int prepare(List<Item> &list, SELECT_LEX_UNIT *u)
  {
    unit= u;
    return 0;
  }
  virtual int prepare2(void) { return 0; }
  /*
    Because of peculiarities of prepared statements protocol
    we need to know number of columns in the result set (if
    there is a result set) apart from sending columns metadata.
  */
  virtual uint field_count(List<Item> &fields) const
  { return fields.elements; }
  virtual bool send_result_set_metadata(List<Item> &list, uint flags)=0;
  virtual bool initialize_tables (JOIN *join=0) { return 0; }
  virtual bool send_eof()=0;
  /**
    Check if this query returns a result set and therefore is allowed in
    cursors and set an error message if it is not the case.

    @retval FALSE     success
    @retval TRUE      error, an error message is set
  */
  virtual bool check_simple_select() const;
  virtual void abort_result_set() {}
  /*
    Cleanup instance of this class for next execution of a prepared
    statement/stored procedure.
  */
  virtual void cleanup();
  void set_thd(THD *thd_arg) { thd= thd_arg; }
#ifdef EMBEDDED_LIBRARY
  virtual void begin_dataset() {}
#else
  void begin_dataset() {}
#endif
  virtual void update_used_tables() {}

  /* this method is called just before the first row of the table can be read */
  virtual void prepare_to_read_rows() {}

  void reset_offset_limit()
  {
    unit->offset_limit_cnt= 0;
  }

  /*
    This returns
    - NULL if the class sends output row to the client
    - this if the output is set elsewhere (a file, @variable, or table).
  */
  virtual select_result_interceptor *result_interceptor()=0;
};


/*
  This is a select_result_sink which simply writes all data into a (temporary)
  table. Creation/deletion of the table is outside of the scope of the class
  
  It is aimed at capturing SHOW EXPLAIN output, so:
  - Unlike select_result class, we don't assume that the sent data is an 
    output of a SELECT_LEX_UNIT (and so we dont apply "LIMIT x,y" from the
    unit)
  - We don't try to convert the target table to MyISAM 
*/

class select_result_explain_buffer : public select_result_sink
{
public:
  select_result_explain_buffer(THD *thd_arg, TABLE *table_arg) : 
    select_result_sink(thd_arg), dst_table(table_arg) {};

  TABLE *dst_table; /* table to write into */

  /* The following is called in the child thread: */
  int send_data(List<Item> &items);
};


/*
  This is a select_result_sink which stores the data in text form.

  It is only used to save EXPLAIN output.
*/

class select_result_text_buffer : public select_result_sink
{
public:
  select_result_text_buffer(THD *thd_arg): select_result_sink(thd_arg) {}
  int send_data(List<Item> &items);
  bool send_result_set_metadata(List<Item> &fields, uint flag);

  void save_to(String *res);
private:
  int append_row(List<Item> &items, bool send_names);

  List<char*> rows;
  int n_columns;
};


/*
  Base class for select_result descendands which intercept and
  transform result set rows. As the rows are not sent to the client,
  sending of result set metadata should be suppressed as well.
*/

class select_result_interceptor: public select_result
{
public:
  select_result_interceptor(THD *thd_arg):
    select_result(thd_arg), suppress_my_ok(false)
  {
    DBUG_ENTER("select_result_interceptor::select_result_interceptor");
    DBUG_PRINT("enter", ("this %p", this));
    DBUG_VOID_RETURN;
  }              /* Remove gcc warning */
  uint field_count(List<Item> &fields) const { return 0; }
  bool send_result_set_metadata(List<Item> &fields, uint flag) { return FALSE; }
  select_result_interceptor *result_interceptor() { return this; }

  /*
    Instruct the object to not call my_ok(). Client output will be handled
    elsewhere. (this is used by ANALYZE $stmt feature).
  */
  void disable_my_ok_calls() { suppress_my_ok= true; }
protected:
  bool suppress_my_ok;
};


class select_send :public select_result {
  /**
    True if we have sent result set metadata to the client.
    In this case the client always expects us to end the result
    set with an eof or error packet
  */
  bool is_result_set_started;
public:
  select_send(THD *thd_arg):
    select_result(thd_arg), is_result_set_started(FALSE) {}
  bool send_result_set_metadata(List<Item> &list, uint flags);
  int send_data(List<Item> &items);
  bool send_eof();
  virtual bool check_simple_select() const { return FALSE; }
  void abort_result_set();
  virtual void cleanup();
  select_result_interceptor *result_interceptor() { return NULL; }
};


/*
  We need this class, because select_send::send_eof() will call ::my_eof.

  See also class Protocol_discard.
*/

class select_send_analyze : public select_send
{
  bool send_result_set_metadata(List<Item> &list, uint flags) { return 0; }
  bool send_eof() { return 0; }
  void abort_result_set() {}
public:
  select_send_analyze(THD *thd_arg): select_send(thd_arg) {}
};


class select_to_file :public select_result_interceptor {
protected:
  sql_exchange *exchange;
  File file;
  IO_CACHE cache;
  ha_rows row_count;
  char path[FN_REFLEN];

public:
  select_to_file(THD *thd_arg, sql_exchange *ex):
    select_result_interceptor(thd_arg), exchange(ex), file(-1),row_count(0L)
  { path[0]=0; }
  ~select_to_file();
  bool send_eof();
  void cleanup();
};


#define ESCAPE_CHARS "ntrb0ZN" // keep synchronous with READ_INFO::unescape


/*
 List of all possible characters of a numeric value text representation.
*/
#define NUMERIC_CHARS ".0123456789e+-"


class select_export :public select_to_file {
  uint field_term_length;
  int field_sep_char,escape_char,line_sep_char;
  int field_term_char; // first char of FIELDS TERMINATED BY or MAX_INT
  /*
    The is_ambiguous_field_sep field is true if a value of the field_sep_char
    field is one of the 'n', 't', 'r' etc characters
    (see the READ_INFO::unescape method and the ESCAPE_CHARS constant value).
  */
  bool is_ambiguous_field_sep;
  /*
     The is_ambiguous_field_term is true if field_sep_char contains the first
     char of the FIELDS TERMINATED BY (ENCLOSED BY is empty), and items can
     contain this character.
  */
  bool is_ambiguous_field_term;
  /*
    The is_unsafe_field_sep field is true if a value of the field_sep_char
    field is one of the '0'..'9', '+', '-', '.' and 'e' characters
    (see the NUMERIC_CHARS constant value).
  */
  bool is_unsafe_field_sep;
  bool fixed_row_size;
  CHARSET_INFO *write_cs; // output charset
public:
  select_export(THD *thd_arg, sql_exchange *ex): select_to_file(thd_arg, ex) {}
  ~select_export();
  int prepare(List<Item> &list, SELECT_LEX_UNIT *u);
  int send_data(List<Item> &items);
};


class select_dump :public select_to_file {
public:
  select_dump(THD *thd_arg, sql_exchange *ex): select_to_file(thd_arg, ex) {}
  int prepare(List<Item> &list, SELECT_LEX_UNIT *u);
  int send_data(List<Item> &items);
};


class select_insert :public select_result_interceptor {
 public:
  TABLE_LIST *table_list;
  TABLE *table;
  List<Item> *fields;
  ulonglong autoinc_value_of_last_inserted_row; // autogenerated or not
  COPY_INFO info;
  bool insert_into_view;
  select_insert(THD *thd_arg, TABLE_LIST *table_list_par,
		TABLE *table_par, List<Item> *fields_par,
		List<Item> *update_fields, List<Item> *update_values,
		enum_duplicates duplic, bool ignore);
  ~select_insert();
  int prepare(List<Item> &list, SELECT_LEX_UNIT *u);
  virtual int prepare2(void);
  virtual int send_data(List<Item> &items);
  virtual void store_values(List<Item> &values);
  virtual bool can_rollback_data() { return 0; }
  bool prepare_eof();
  bool send_ok_packet();
  bool send_eof();
  virtual void abort_result_set();
  /* not implemented: select_insert is never re-used in prepared statements */
  void cleanup();
};


class select_create: public select_insert {
  TABLE_LIST *create_table;
  Table_specification_st *create_info;
  TABLE_LIST *select_tables;
  Alter_info *alter_info;
  Field **field;
  /* lock data for tmp table */
  MYSQL_LOCK *m_lock;
  /* m_lock or thd->extra_lock */
  MYSQL_LOCK **m_plock;
  bool       exit_done;
  TMP_TABLE_SHARE *saved_tmp_table_share;

public:
  select_create(THD *thd_arg, TABLE_LIST *table_arg,
                Table_specification_st *create_info_par,
                Alter_info *alter_info_arg,
                List<Item> &select_fields,enum_duplicates duplic, bool ignore,
                TABLE_LIST *select_tables_arg):
    select_insert(thd_arg, table_arg, NULL, &select_fields, 0, 0, duplic,
                  ignore),
    create_table(table_arg),
    create_info(create_info_par),
    select_tables(select_tables_arg),
    alter_info(alter_info_arg),
    m_plock(NULL), exit_done(0),
    saved_tmp_table_share(0)
    {}
  int prepare(List<Item> &list, SELECT_LEX_UNIT *u);

  int binlog_show_create_table(TABLE **tables, uint count);
  void store_values(List<Item> &values);
  bool send_eof();
  virtual void abort_result_set();
  virtual bool can_rollback_data() { return 1; }

  // Needed for access from local class MY_HOOKS in prepare(), since thd is proteted.
  const THD *get_thd(void) { return thd; }
  const HA_CREATE_INFO *get_create_info() { return create_info; };
  int prepare2(void) { return 0; }
};

#include <myisam.h>

#ifdef WITH_ARIA_STORAGE_ENGINE
#include <maria.h>
#else
#undef USE_ARIA_FOR_TMP_TABLES
#endif

#ifdef USE_ARIA_FOR_TMP_TABLES
#define TMP_ENGINE_COLUMNDEF MARIA_COLUMNDEF
#define TMP_ENGINE_HTON maria_hton
#define TMP_ENGINE_NAME "Aria"
inline uint tmp_table_max_key_length() { return maria_max_key_length(); }
inline uint tmp_table_max_key_parts() { return maria_max_key_segments(); }
#else
#define TMP_ENGINE_COLUMNDEF MI_COLUMNDEF
#define TMP_ENGINE_HTON myisam_hton
#define TMP_ENGINE_NAME "MyISAM"
inline uint tmp_table_max_key_length() { return MI_MAX_KEY_LENGTH; }
inline uint tmp_table_max_key_parts() { return MI_MAX_KEY_SEG; }
#endif

/*
  Param to create temporary tables when doing SELECT:s
  NOTE
    This structure is copied using memcpy as a part of JOIN.
*/

class TMP_TABLE_PARAM :public Sql_alloc
{
public:
  List<Item> copy_funcs;
  Copy_field *copy_field, *copy_field_end;
  uchar	    *group_buff;
  Item	    **items_to_copy;			/* Fields in tmp table */
  TMP_ENGINE_COLUMNDEF *recinfo, *start_recinfo;
  KEY *keyinfo;
  ha_rows end_write_records;
  /**
    Number of normal fields in the query, including those referred to
    from aggregate functions. Hence, "SELECT `field1`,
    SUM(`field2`) from t1" sets this counter to 2.

    @see count_field_types
  */
  uint	field_count; 
  /**
    Number of fields in the query that have functions. Includes both
    aggregate functions (e.g., SUM) and non-aggregates (e.g., RAND).
    Also counts functions referred to from aggregate functions, i.e.,
    "SELECT SUM(RAND())" sets this counter to 2.

    @see count_field_types
  */
  uint  func_count;  
  /**
    Number of fields in the query that have aggregate functions. Note
    that the optimizer may choose to optimize away these fields by
    replacing them with constants, in which case sum_func_count will
    need to be updated.

    @see opt_sum_query, count_field_types
  */
  uint  sum_func_count;   
  uint  hidden_field_count;
  uint	group_parts,group_length,group_null_parts;
  uint	quick_group;
  /**
    Enabled when we have atleast one outer_sum_func. Needed when used
    along with distinct.

    @see create_tmp_table
  */
  bool  using_outer_summary_function;
  CHARSET_INFO *table_charset;
  bool schema_table;
  /* TRUE if the temp table is created for subquery materialization. */
  bool materialized_subquery;
  /* TRUE if all columns of the table are guaranteed to be non-nullable */
  bool force_not_null_cols;
  /*
    True if GROUP BY and its aggregate functions are already computed
    by a table access method (e.g. by loose index scan). In this case
    query execution should not perform aggregation and should treat
    aggregate functions as normal functions.
  */
  bool precomputed_group_by;
  bool force_copy_fields;
  /*
    If TRUE, create_tmp_field called from create_tmp_table will convert
    all BIT fields to 64-bit longs. This is a workaround the limitation
    that MEMORY tables cannot index BIT columns.
  */
  bool bit_fields_as_long;
  /*
    Whether to create or postpone actual creation of this temporary table.
    TRUE <=> create_tmp_table will create only the TABLE structure.
  */
  bool skip_create_table;

  TMP_TABLE_PARAM()
    :copy_field(0), group_parts(0),
     group_length(0), group_null_parts(0),
     using_outer_summary_function(0),
     schema_table(0), materialized_subquery(0), force_not_null_cols(0),
     precomputed_group_by(0),
     force_copy_fields(0), bit_fields_as_long(0), skip_create_table(0)
  {}
  ~TMP_TABLE_PARAM()
  {
    cleanup();
  }
  void init(void);
  inline void cleanup(void)
  {
    if (copy_field)				/* Fix for Intel compiler */
    {
      delete [] copy_field;
      copy_field= NULL;
      copy_field_end= NULL;
    }
  }
};


class select_union :public select_result_interceptor
{
public:
  TMP_TABLE_PARAM tmp_table_param;
  int write_err; /* Error code from the last send_data->ha_write_row call. */
public:
  TABLE *table;
  ha_rows records;

  select_union(THD *thd_arg):
    select_result_interceptor(thd_arg), write_err(0), table(0), records(0)
  { tmp_table_param.init(); }
  int prepare(List<Item> &list, SELECT_LEX_UNIT *u);
  /**
    Do prepare() and prepare2() if they have been postponed until
    column type information is computed (used by select_union_direct).

    @param types Column types

    @return false on success, true on failure
  */
  virtual bool postponed_prepare(List<Item> &types)
  { return false; }
  int send_data(List<Item> &items);
  bool send_eof();
  virtual bool flush();
  void cleanup();
  virtual bool create_result_table(THD *thd, List<Item> *column_types,
                                   bool is_distinct, ulonglong options,
                                   const char *alias, 
                                   bool bit_fields_as_long,
                                   bool create_table,
                                   bool keep_row_order= FALSE);
  TMP_TABLE_PARAM *get_tmp_table_param() { return &tmp_table_param; }
};


class select_union_recursive :public select_union
{
 public:
  /* The temporary table with the new records generated by one iterative step */
  TABLE *incr_table;
  /* The TMP_TABLE_PARAM structure used to create incr_table */
  TMP_TABLE_PARAM incr_table_param;
  /* One of tables from the list rec_tables (determined dynamically) */
  TABLE *first_rec_table_to_update;
  /*
    The list of all recursive table references to the CTE for whose
    specification this select_union_recursive was created
 */
  List<TABLE_LIST> rec_table_refs;
  /*
    The count of how many times cleanup() was called with cleaned==false
    for the unit specifying the recursive CTE for which this object was created
    or for the unit specifying a CTE that mutually recursive with this CTE.
  */
  uint cleanup_count;

  select_union_recursive(THD *thd_arg):
    select_union(thd_arg),
    incr_table(0), first_rec_table_to_update(0), cleanup_count(0)
  { incr_table_param.init(); };

  int send_data(List<Item> &items);
  bool create_result_table(THD *thd, List<Item> *column_types,
                           bool is_distinct, ulonglong options,
                           const char *alias, 
                           bool bit_fields_as_long,
                           bool create_table,
                           bool keep_row_order= FALSE);
  void cleanup();
};

/**
  UNION result that is passed directly to the receiving select_result
  without filling a temporary table.

  Function calls are forwarded to the wrapped select_result, but some
  functions are expected to be called only once for each query, so
  they are only executed for the first SELECT in the union (execept
  for send_eof(), which is executed only for the last SELECT).

  This select_result is used when a UNION is not DISTINCT and doesn't
  have a global ORDER BY clause. @see st_select_lex_unit::prepare().
*/

class select_union_direct :public select_union
{
private:
  /* Result object that receives all rows */
  select_result *result;
  /* The last SELECT_LEX of the union */
  SELECT_LEX *last_select_lex;

  /* Wrapped result has received metadata */
  bool done_send_result_set_metadata;
  /* Wrapped result has initialized tables */
  bool done_initialize_tables;

  /* Accumulated limit_found_rows */
  ulonglong limit_found_rows;

  /* Number of rows offset */
  ha_rows offset;
  /* Number of rows limit + offset, @see select_union_direct::send_data() */
  ha_rows limit;

public:
  /* Number of rows in the union */
  ha_rows send_records; 
  select_union_direct(THD *thd_arg, select_result *result_arg,
                      SELECT_LEX *last_select_lex_arg):
  select_union(thd_arg), result(result_arg),
    last_select_lex(last_select_lex_arg),
    done_send_result_set_metadata(false), done_initialize_tables(false),
    limit_found_rows(0)
    { send_records= 0; }
  bool change_result(select_result *new_result);
  uint field_count(List<Item> &fields) const
  {
    // Only called for top-level select_results, usually select_send
    DBUG_ASSERT(false); /* purecov: inspected */
    return 0; /* purecov: inspected */
  }
  bool postponed_prepare(List<Item> &types);
  bool send_result_set_metadata(List<Item> &list, uint flags);
  int send_data(List<Item> &items);
  bool initialize_tables (JOIN *join= NULL);
  bool send_eof();
  bool flush() { return false; }
  bool check_simple_select() const
  {
    /* Only called for top-level select_results, usually select_send */
    DBUG_ASSERT(false); /* purecov: inspected */
    return false; /* purecov: inspected */
  }
  void abort_result_set()
  {
    result->abort_result_set(); /* purecov: inspected */
  }
  void cleanup()
  {
    send_records= 0;
  }
  void set_thd(THD *thd_arg)
  {
    /*
      Only called for top-level select_results, usually select_send,
      and for the results of subquery engines
      (select_<something>_subselect).
    */
    DBUG_ASSERT(false); /* purecov: inspected */
  }
  void reset_offset_limit_cnt()
  {
    // EXPLAIN should never output to a select_union_direct
    DBUG_ASSERT(false); /* purecov: inspected */
  }
  void begin_dataset()
  {
    // Only called for sp_cursor::Select_fetch_into_spvars
    DBUG_ASSERT(false); /* purecov: inspected */
  }
};


/* Base subselect interface class */
class select_subselect :public select_result_interceptor
{
protected:
  Item_subselect *item;
public:
  select_subselect(THD *thd_arg, Item_subselect *item_arg):
    select_result_interceptor(thd_arg), item(item_arg) {}
  int send_data(List<Item> &items)=0;
  bool send_eof() { return 0; };
};

/* Single value subselect interface class */
class select_singlerow_subselect :public select_subselect
{
public:
  select_singlerow_subselect(THD *thd_arg, Item_subselect *item_arg):
    select_subselect(thd_arg, item_arg)
  {}
  int send_data(List<Item> &items);
};


/*
  This class specializes select_union to collect statistics about the
  data stored in the temp table. Currently the class collects statistcs
  about NULLs.
*/

class select_materialize_with_stats : public select_union
{
protected:
  class Column_statistics
  {
  public:
    /* Count of NULLs per column. */
    ha_rows null_count;
    /* The row number that contains the first NULL in a column. */
    ha_rows min_null_row;
    /* The row number that contains the last NULL in a column. */
    ha_rows max_null_row;
  };

  /* Array of statistics data per column. */
  Column_statistics* col_stat;

  /*
    The number of columns in the biggest sub-row that consists of only
    NULL values.
  */
  uint max_nulls_in_row;
  /*
    Count of rows writtent to the temp table. This is redundant as it is
    already stored in handler::stats.records, however that one is relatively
    expensive to compute (given we need that for evry row).
  */
  ha_rows count_rows;

protected:
  void reset();

public:
  select_materialize_with_stats(THD *thd_arg): select_union(thd_arg)
  { tmp_table_param.init(); }
  bool create_result_table(THD *thd, List<Item> *column_types,
                           bool is_distinct, ulonglong options,
                           const char *alias, 
                           bool bit_fields_as_long,
                           bool create_table,
                           bool keep_row_order= FALSE);
  bool init_result_table(ulonglong select_options);
  int send_data(List<Item> &items);
  void cleanup();
  ha_rows get_null_count_of_col(uint idx)
  {
    DBUG_ASSERT(idx < table->s->fields);
    return col_stat[idx].null_count;
  }
  ha_rows get_max_null_of_col(uint idx)
  {
    DBUG_ASSERT(idx < table->s->fields);
    return col_stat[idx].max_null_row;
  }
  ha_rows get_min_null_of_col(uint idx)
  {
    DBUG_ASSERT(idx < table->s->fields);
    return col_stat[idx].min_null_row;
  }
  uint get_max_nulls_in_row() { return max_nulls_in_row; }
};


/* used in independent ALL/ANY optimisation */
class select_max_min_finder_subselect :public select_subselect
{
  Item_cache *cache;
  bool (select_max_min_finder_subselect::*op)();
  bool fmax;
  bool is_all;
public:
  select_max_min_finder_subselect(THD *thd_arg, Item_subselect *item_arg,
                                  bool mx, bool all):
    select_subselect(thd_arg, item_arg), cache(0), fmax(mx), is_all(all)
  {}
  void cleanup();
  int send_data(List<Item> &items);
  bool cmp_real();
  bool cmp_int();
  bool cmp_decimal();
  bool cmp_str();
};

/* EXISTS subselect interface class */
class select_exists_subselect :public select_subselect
{
public:
  select_exists_subselect(THD *thd_arg, Item_subselect *item_arg):
    select_subselect(thd_arg, item_arg) {}
  int send_data(List<Item> &items);
};




/*
  Optimizer and executor structure for the materialized semi-join info. This
  structure contains
   - The sj-materialization temporary table
   - Members needed to make index lookup or a full scan of the temptable.
*/
class SJ_MATERIALIZATION_INFO : public Sql_alloc
{
public:
  /* Optimal join sub-order */
  struct st_position *positions;

  uint tables; /* Number of tables in the sj-nest */

  /* Expected #rows in the materialized table */
  double rows;

  /* 
    Cost to materialize - execute the sub-join and write rows into temp.table
  */
  Cost_estimate materialization_cost;

  /* Cost to make one lookup in the temptable */
  Cost_estimate lookup_cost;
  
  /* Cost of scanning the materialized table */
  Cost_estimate scan_cost;

  /* --- Execution structures ---------- */
  
  /*
    TRUE <=> This structure is used for execution. We don't necessarily pick
    sj-materialization, so some of SJ_MATERIALIZATION_INFO structures are not
    used by materialization
  */
  bool is_used;
  
  bool materialized; /* TRUE <=> materialization already performed */
  /*
    TRUE  - the temptable is read with full scan
    FALSE - we use the temptable for index lookups
  */
  bool is_sj_scan; 
  
  /* The temptable and its related info */
  TMP_TABLE_PARAM sjm_table_param;
  List<Item> sjm_table_cols;
  TABLE *table;

  /* Structure used to make index lookups */
  struct st_table_ref *tab_ref;
  Item *in_equality; /* See create_subq_in_equalities() */

  Item *join_cond; /* See comments in make_join_select() */
  Copy_field *copy_field; /* Needed for SJ_Materialization scan */
};


/* Structs used when sorting */
struct SORT_FIELD_ATTR
{
  uint length;          /* Length of sort field */
  uint suffix_length;   /* Length suffix (0-4) */
};


struct SORT_FIELD: public SORT_FIELD_ATTR
{
  Field *field;				/* Field to sort */
  Item	*item;				/* Item if not sorting fields */
  bool reverse;				/* if descending sort */
};


typedef struct st_sort_buffer {
  uint index;					/* 0 or 1 */
  uint sort_orders;
  uint change_pos;				/* If sort-fields changed */
  char **buff;
  SORT_FIELD *sortorder;
} SORT_BUFFER;

/* Structure for db & table in sql_yacc */

class Table_ident :public Sql_alloc
{
public:
  LEX_STRING db;
  LEX_STRING table;
  SELECT_LEX_UNIT *sel;
  inline Table_ident(THD *thd, LEX_STRING db_arg, LEX_STRING table_arg,
		     bool force)
    :table(table_arg), sel((SELECT_LEX_UNIT *)0)
  {
    if (!force && (thd->client_capabilities & CLIENT_NO_SCHEMA))
      db.str=0;
    else
      db= db_arg;
  }
  inline Table_ident(LEX_STRING table_arg)
    :table(table_arg), sel((SELECT_LEX_UNIT *)0)
  {
    db.str=0;
  }
  /*
    This constructor is used only for the case when we create a derived
    table. A derived table has no name and doesn't belong to any database.
    Later, if there was an alias specified for the table, it will be set
    by add_table_to_list.
  */
  inline Table_ident(SELECT_LEX_UNIT *s) : sel(s)
  {
    /* We must have a table name here as this is used with add_table_to_list */
    db.str= empty_c_string;                    /* a subject to casedn_str */
    db.length= 0;
    table.str= internal_table_name;
    table.length=1;
  }
  bool is_derived_table() const { return MY_TEST(sel); }
  inline void change_db(char *db_name)
  {
    db.str= db_name; db.length= (uint) strlen(db_name);
  }
};

// this is needed for user_vars hash
class user_var_entry
{
  CHARSET_INFO *m_charset;
 public:
  user_var_entry() {}                         /* Remove gcc warning */
  LEX_STRING name;
  char *value;
  ulong length;
  query_id_t update_query_id, used_query_id;
  Item_result type;
  bool unsigned_flag;

  double val_real(bool *null_value);
  longlong val_int(bool *null_value) const;
  String *val_str(bool *null_value, String *str, uint decimals);
  my_decimal *val_decimal(bool *null_value, my_decimal *result);
  CHARSET_INFO *charset() const { return m_charset; }
  void set_charset(CHARSET_INFO *cs) { m_charset= cs; }
};

user_var_entry *get_variable(HASH *hash, LEX_STRING &name,
				    bool create_if_not_exists);

class SORT_INFO;
class multi_delete :public select_result_interceptor
{
  TABLE_LIST *delete_tables, *table_being_deleted;
  Unique **tempfiles;
  ha_rows deleted, found;
  uint num_of_tables;
  int error;
  bool do_delete;
  /* True if at least one table we delete from is transactional */
  bool transactional_tables;
  /* True if at least one table we delete from is not transactional */
  bool normal_tables;
  bool delete_while_scanning;
  /*
     error handling (rollback and binlogging) can happen in send_eof()
     so that afterward abort_result_set() needs to find out that.
  */
  bool error_handled;

public:
  multi_delete(THD *thd_arg, TABLE_LIST *dt, uint num_of_tables);
  ~multi_delete();
  int prepare(List<Item> &list, SELECT_LEX_UNIT *u);
  int send_data(List<Item> &items);
  bool initialize_tables (JOIN *join);
  int do_deletes();
  int do_table_deletes(TABLE *table, SORT_INFO *sort_info, bool ignore);
  bool send_eof();
  inline ha_rows num_deleted() const { return deleted; }
  virtual void abort_result_set();
  void prepare_to_read_rows();
};


class multi_update :public select_result_interceptor
{
  TABLE_LIST *all_tables; /* query/update command tables */
  List<TABLE_LIST> *leaves;     /* list of leves of join table tree */
  TABLE_LIST *update_tables, *table_being_updated;
  TABLE **tmp_tables, *main_table, *table_to_update;
  TMP_TABLE_PARAM *tmp_table_param;
  ha_rows updated, found;
  List <Item> *fields, *values;
  List <Item> **fields_for_table, **values_for_table;
  uint table_count;
  /*
   List of tables referenced in the CHECK OPTION condition of
   the updated view excluding the updated table.
  */
  List <TABLE> unupdated_check_opt_tables;
  Copy_field *copy_field;
  enum enum_duplicates handle_duplicates;
  bool do_update, trans_safe;
  /* True if the update operation has made a change in a transactional table */
  bool transactional_tables;
  bool ignore;
  /* 
     error handling (rollback and binlogging) can happen in send_eof()
     so that afterward  abort_result_set() needs to find out that.
  */
  bool error_handled;
  
  /* Need this to protect against multiple prepare() calls */
  bool prepared;
public:
  multi_update(THD *thd_arg, TABLE_LIST *ut, List<TABLE_LIST> *leaves_list,
	       List<Item> *fields, List<Item> *values,
	       enum_duplicates handle_duplicates, bool ignore);
  ~multi_update();
  int prepare(List<Item> &list, SELECT_LEX_UNIT *u);
  int send_data(List<Item> &items);
  bool initialize_tables (JOIN *join);
  int  do_updates();
  bool send_eof();
  inline ha_rows num_found() const { return found; }
  inline ha_rows num_updated() const { return updated; }
  virtual void abort_result_set();
  void update_used_tables();
  void prepare_to_read_rows();
};

class my_var : public Sql_alloc  {
public:
  const LEX_STRING name;
  enum type { SESSION_VAR, LOCAL_VAR, PARAM_VAR };
  type scope;
  my_var(const LEX_STRING& j, enum type s) : name(j), scope(s) { }
  virtual ~my_var() {}
  virtual bool set(THD *thd, Item *val) = 0;
};

class my_var_sp: public my_var {
public:
  uint offset;
  enum_field_types type;
  /*
    Routine to which this Item_splocal belongs. Used for checking if correct
    runtime context is used for variable handling.
  */
  sp_head *sp;
  my_var_sp(const LEX_STRING& j, uint o, enum_field_types t, sp_head *s)
    : my_var(j, LOCAL_VAR), offset(o), type(t), sp(s) { }
  ~my_var_sp() { }
  bool set(THD *thd, Item *val);
};

class my_var_user: public my_var {
public:
  my_var_user(const LEX_STRING& j)
    : my_var(j, SESSION_VAR) { }
  ~my_var_user() { }
  bool set(THD *thd, Item *val);
};

class select_dumpvar :public select_result_interceptor {
  ha_rows row_count;
public:
  List<my_var> var_list;
  select_dumpvar(THD *thd_arg): select_result_interceptor(thd_arg)
  { var_list.empty(); row_count= 0; }
  ~select_dumpvar() {}
  int prepare(List<Item> &list, SELECT_LEX_UNIT *u);
  int send_data(List<Item> &items);
  bool send_eof();
  virtual bool check_simple_select() const;
  void cleanup();
};

/* Bits in sql_command_flags */

#define CF_CHANGES_DATA           (1U << 0)
#define CF_REPORT_PROGRESS        (1U << 1)
#define CF_STATUS_COMMAND         (1U << 2)
#define CF_SHOW_TABLE_COMMAND     (1U << 3)
#define CF_WRITE_LOGS_COMMAND     (1U << 4)

/**
  Must be set for SQL statements that may contain
  Item expressions and/or use joins and tables.
  Indicates that the parse tree of such statement may
  contain rule-based optimizations that depend on metadata
  (i.e. number of columns in a table), and consequently
  that the statement must be re-prepared whenever
  referenced metadata changes. Must not be set for
  statements that themselves change metadata, e.g. RENAME,
  ALTER and other DDL, since otherwise will trigger constant
  reprepare. Consequently, complex item expressions and
  joins are currently prohibited in these statements.
*/
#define CF_REEXECUTION_FRAGILE    (1U << 5)
/**
  Implicitly commit before the SQL statement is executed.

  Statements marked with this flag will cause any active
  transaction to end (commit) before proceeding with the
  command execution.

  This flag should be set for statements that probably can't
  be rolled back or that do not expect any previously metadata
  locked tables.
*/
#define CF_IMPLICT_COMMIT_BEGIN   (1U << 6)
/**
  Implicitly commit after the SQL statement.

  Statements marked with this flag are automatically committed
  at the end of the statement.

  This flag should be set for statements that will implicitly
  open and take metadata locks on system tables that should not
  be carried for the whole duration of a active transaction.
*/
#define CF_IMPLICIT_COMMIT_END    (1U << 7)
/**
  CF_IMPLICT_COMMIT_BEGIN and CF_IMPLICIT_COMMIT_END are used
  to ensure that the active transaction is implicitly committed
  before and after every DDL statement and any statement that
  modifies our currently non-transactional system tables.
*/
#define CF_AUTO_COMMIT_TRANS  (CF_IMPLICT_COMMIT_BEGIN | CF_IMPLICIT_COMMIT_END)

/**
  Diagnostic statement.
  Diagnostic statements:
  - SHOW WARNING
  - SHOW ERROR
  - GET DIAGNOSTICS (WL#2111)
  do not modify the diagnostics area during execution.
*/
#define CF_DIAGNOSTIC_STMT        (1U << 8)

/**
  Identifies statements that may generate row events
  and that may end up in the binary log.
*/
#define CF_CAN_GENERATE_ROW_EVENTS (1U << 9)

/**
  Identifies statements which may deal with temporary tables and for which
  temporary tables should be pre-opened to simplify privilege checks.
*/
#define CF_PREOPEN_TMP_TABLES   (1U << 10)

/**
  Identifies statements for which open handlers should be closed in the
  beginning of the statement.
*/
#define CF_HA_CLOSE             (1U << 11)

/**
  Identifies statements that can be explained with EXPLAIN.
*/
#define CF_CAN_BE_EXPLAINED       (1U << 12)

/** Identifies statements which may generate an optimizer trace */
#define CF_OPTIMIZER_TRACE        (1U << 14)

/**
   Identifies statements that should always be disallowed in
   read only transactions.
*/
#define CF_DISALLOW_IN_RO_TRANS   (1U << 15)

/**
  Statement that need the binlog format to be unchanged.
*/
#define CF_FORCE_ORIGINAL_BINLOG_FORMAT (1U << 16)

/**
  Statement that inserts new rows (INSERT, REPLACE, LOAD, ALTER TABLE)
*/
#define CF_INSERTS_DATA (1U << 17)

/**
  Statement that updates existing rows (UPDATE, multi-update)
*/
#define CF_UPDATES_DATA (1U << 18)

/**
  Not logged into slow log as "admin commands"
*/
#define CF_ADMIN_COMMAND (1U << 19)

/**
  SP Bulk execution safe
*/
#define CF_PS_ARRAY_BINDING_SAFE (1U << 20)
/**
  SP Bulk execution optimized
*/
#define CF_PS_ARRAY_BINDING_OPTIMIZED (1U << 21)

/* Bits in server_command_flags */

/**
  Skip the increase of the global query id counter. Commonly set for
  commands that are stateless (won't cause any change on the server
  internal states).
*/
#define CF_SKIP_QUERY_ID        (1U << 0)

/**
  Skip the increase of the number of statements that clients have
  sent to the server. Commonly used for commands that will cause
  a statement to be executed but the statement might have not been
  sent by the user (ie: stored procedure).
*/
#define CF_SKIP_QUESTIONS       (1U << 1)
#ifdef WITH_WSREP
/**
  Do not check that wsrep snapshot is ready before allowing this command
*/
#define CF_SKIP_WSREP_CHECK     (1U << 2)
#else
#define CF_SKIP_WSREP_CHECK     0
#endif /* WITH_WSREP */

/**
  Do not allow it for COM_MULTI batch
*/
#define CF_NO_COM_MULTI         (1U << 3)

/* Inline functions */

inline bool add_item_to_list(THD *thd, Item *item)
{
  return thd->lex->current_select->add_item_to_list(thd, item);
}

inline bool add_value_to_list(THD *thd, Item *value)
{
  return thd->lex->value_list.push_back(value, thd->mem_root);
}

inline bool add_order_to_list(THD *thd, Item *item, bool asc)
{
  return thd->lex->current_select->add_order_to_list(thd, item, asc);
}

inline bool add_gorder_to_list(THD *thd, Item *item, bool asc)
{
  return thd->lex->current_select->add_gorder_to_list(thd, item, asc);
}

inline bool add_group_to_list(THD *thd, Item *item, bool asc)
{
  return thd->lex->current_select->add_group_to_list(thd, item, asc);
}

inline Item *and_conds(THD *thd, Item *a, Item *b)
{
  if (!b) return a;
  if (!a) return b;
  return new (thd->mem_root) Item_cond_and(thd, a, b);
}

/* inline handler methods that need to know TABLE and THD structures */
inline void handler::increment_statistics(ulong SSV::*offset) const
{
  status_var_increment(table->in_use->status_var.*offset);
  table->in_use->check_limit_rows_examined();
}

inline void handler::decrement_statistics(ulong SSV::*offset) const
{
  status_var_decrement(table->in_use->status_var.*offset);
}


inline int handler::ha_ft_read(uchar *buf)
{
  int error= ft_read(buf);
  if (!error)
    update_rows_read();

  table->status=error ? STATUS_NOT_FOUND: 0;
  return error;
}

inline int handler::ha_rnd_pos_by_record(uchar *buf)
{
  int error= rnd_pos_by_record(buf);
  table->status=error ? STATUS_NOT_FOUND: 0;
  return error;
}

inline int handler::ha_read_first_row(uchar *buf, uint primary_key)
{
  int error= read_first_row(buf, primary_key);
  if (!error)
    update_rows_read();
  table->status=error ? STATUS_NOT_FOUND: 0;
  return error;
}

inline int handler::ha_write_tmp_row(uchar *buf)
{
  int error;
  MYSQL_INSERT_ROW_START(table_share->db.str, table_share->table_name.str);
  increment_statistics(&SSV::ha_tmp_write_count);
  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_WRITE_ROW, MAX_KEY, 0,
                { error= write_row(buf); })
  MYSQL_INSERT_ROW_DONE(error);
  return error;
}

inline int handler::ha_update_tmp_row(const uchar *old_data, uchar *new_data)
{
  int error;
  MYSQL_UPDATE_ROW_START(table_share->db.str, table_share->table_name.str);
  increment_statistics(&SSV::ha_tmp_update_count);
  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_UPDATE_ROW, active_index, 0,
                { error= update_row(old_data, new_data);})
  MYSQL_UPDATE_ROW_DONE(error);
  return error;
}


extern pthread_attr_t *get_connection_attrib(void);

/**
   Set thread entering a condition

   This function should be called before putting a thread to wait for
   a condition. @a mutex should be held before calling this
   function. After being waken up, @f thd_exit_cond should be called.

   @param thd      The thread entering the condition, NULL means current thread
   @param cond     The condition the thread is going to wait for
   @param mutex    The mutex associated with the condition, this must be
                   held before call this function
   @param stage    The new process message for the thread
   @param old_stage The old process message for the thread
   @param src_function The caller source function name
   @param src_file The caller source file name
   @param src_line The caller source line number
*/
void thd_enter_cond(MYSQL_THD thd, mysql_cond_t *cond, mysql_mutex_t *mutex,
                    const PSI_stage_info *stage, PSI_stage_info *old_stage,
                    const char *src_function, const char *src_file,
                    int src_line);

#define THD_ENTER_COND(P1, P2, P3, P4, P5) \
  thd_enter_cond(P1, P2, P3, P4, P5, __func__, __FILE__, __LINE__)

/**
   Set thread leaving a condition

   This function should be called after a thread being waken up for a
   condition.

   @param thd      The thread entering the condition, NULL means current thread
   @param stage    The process message, ususally this should be the old process
                   message before calling @f thd_enter_cond
   @param src_function The caller source function name
   @param src_file The caller source file name
   @param src_line The caller source line number
*/
void thd_exit_cond(MYSQL_THD thd, const PSI_stage_info *stage,
                   const char *src_function, const char *src_file,
                   int src_line);

#define THD_EXIT_COND(P1, P2) \
  thd_exit_cond(P1, P2, __func__, __FILE__, __LINE__)

inline bool binlog_should_compress(ulong len)
{
  return opt_bin_log_compress &&
    len >= opt_bin_log_compress_min_len;
}


/**
   Save thd sql_mode on instantiation.
   On destruction it resets the mode to the previously stored value.
*/
class Sql_mode_save
{
 public:
  Sql_mode_save(THD *thd) : thd(thd), old_mode(thd->variables.sql_mode) {}
  ~Sql_mode_save() { thd->variables.sql_mode = old_mode; }

 private:
  THD *thd;
  sql_mode_t old_mode; // SQL mode saved at construction time.
};

class Abort_on_warning_instant_set
{
  THD *m_thd;
  bool m_save_abort_on_warning;
public:
  Abort_on_warning_instant_set(THD *thd, bool temporary_value)
   :m_thd(thd), m_save_abort_on_warning(thd->abort_on_warning)
  {
    thd->abort_on_warning= temporary_value;
  }
  ~Abort_on_warning_instant_set()
  {
    m_thd->abort_on_warning= m_save_abort_on_warning;
  }
};

class Check_level_instant_set
{
  THD *m_thd;
  enum_check_fields m_check_level;
public:
  Check_level_instant_set(THD *thd, enum_check_fields temporary_value)
   :m_thd(thd), m_check_level(thd->count_cuted_fields)
  {
    thd->count_cuted_fields= temporary_value;
  }
  ~Check_level_instant_set()
  {
    m_thd->count_cuted_fields= m_check_level;
  }
};

class Switch_to_definer_security_ctx
{
 public:
  Switch_to_definer_security_ctx(THD *thd, TABLE_LIST *table) :
    m_thd(thd), m_sctx(thd->security_ctx)
  {
    if (table->security_ctx)
      thd->security_ctx= table->security_ctx;
  }
  ~Switch_to_definer_security_ctx() { m_thd->security_ctx = m_sctx; }

 private:
  THD *m_thd;
  Security_context *m_sctx;
};

#endif /* MYSQL_SERVER */

#endif /* SQL_CLASS_INCLUDED */
