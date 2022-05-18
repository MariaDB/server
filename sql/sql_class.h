/*
   Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2009, 2022, MariaDB Corporation.

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

#include <atomic>
#include "dur_prop.h"
#include <waiting_threads.h>
#include "sql_const.h"
#include <mysql/plugin_audit.h>
#include "log.h"
#include "rpl_tblmap.h"
#include "mdl.h"
#include "field.h"                              // Create_field
#include "opt_trace_context.h"
#include "probes_mysql.h"
#include "sql_locale.h"     /* my_locale_st */
#include "sql_profile.h"    /* PROFILING */
#include "scheduler.h"      /* thd_scheduler */
#include "protocol.h"       /* Protocol_text, Protocol_binary */
#include "violite.h"        /* vio_is_connected */
#include "thr_lock.h"       /* thr_lock_type, THR_LOCK_DATA, THR_LOCK_INFO */
#include "thr_timer.h"
#include "thr_malloc.h"
#include "log_slow.h"       /* LOG_SLOW_DISABLE_... */
#include <my_tree.h>
#include "sql_digest_stream.h"            // sql_digest_state
#include <mysql/psi/mysql_stage.h>
#include <mysql/psi/mysql_statement.h>
#include <mysql/psi/mysql_idle.h>
#include <mysql/psi/mysql_table.h>
#include <mysql_com_server.h>
#include "session_tracker.h"
#include "backup.h"
#include "xa.h"
#include "ddl_log.h"                            /* DDL_LOG_STATE */

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
#ifdef WITH_WSREP
#include <inttypes.h>
/* wsrep-lib */
#include "wsrep_client_service.h"
#include "wsrep_client_state.h"
#include "wsrep_mutex.h"
#include "wsrep_condition_variable.h"

class Wsrep_applier_service;
#endif /* WITH_WSREP */

class Reprepare_observer;
class Relay_log_info;
struct rpl_group_info;
struct rpl_parallel_thread;
class Rpl_filter;
class Query_log_event;
class Load_log_event;
class Log_event_writer;
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
#ifdef HAVE_REPLICATION
struct Slave_info;
#endif

enum enum_ha_read_modes { RFIRST, RNEXT, RPREV, RLAST, RKEY, RNEXT_SAME };
enum enum_duplicates { DUP_ERROR, DUP_REPLACE, DUP_UPDATE };
enum enum_delay_key_write { DELAY_KEY_WRITE_NONE, DELAY_KEY_WRITE_ON,
			    DELAY_KEY_WRITE_ALL };
enum enum_slave_exec_mode { SLAVE_EXEC_MODE_STRICT,
                            SLAVE_EXEC_MODE_IDEMPOTENT,
                            SLAVE_EXEC_MODE_LAST_BIT };
enum enum_slave_run_triggers_for_rbr { SLAVE_RUN_TRIGGERS_FOR_RBR_NO,
                                       SLAVE_RUN_TRIGGERS_FOR_RBR_YES,
                                       SLAVE_RUN_TRIGGERS_FOR_RBR_LOGGING,
                                       SLAVE_RUN_TRIGGERS_FOR_RBR_ENFORCE};
enum enum_slave_type_conversions { SLAVE_TYPE_CONVERSIONS_ALL_LOSSY,
                                   SLAVE_TYPE_CONVERSIONS_ALL_NON_LOSSY};

/*
  MARK_COLUMNS_READ:  A column is goind to be read.
  MARK_COLUMNS_WRITE: A column is going to be written to.
  MARK_COLUMNS_READ:  A column is goind to be read.
                      A bit in read set is set to inform handler that the field
                      is to be read. If field list contains duplicates, then
                      thd->dup_field is set to point to the last found
                      duplicate.
  MARK_COLUMNS_WRITE: A column is going to be written to.
                      A bit is set in write set to inform handler that it needs
                      to update this field in write_row and update_row.
*/
enum enum_column_usage
{ COLUMNS_READ, COLUMNS_WRITE, MARK_COLUMNS_READ, MARK_COLUMNS_WRITE};

static inline bool should_mark_column(enum_column_usage column_usage)
{ return column_usage >= MARK_COLUMNS_READ; }

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
/* SQL mode bits defined above are common for MariaDB and MySQL */
#define MODE_MASK_MYSQL_COMPATIBLE      0xFFFFFFFFULL
/* The following modes are specific to MariaDB */
#define MODE_EMPTY_STRING_IS_NULL       (1ULL << 32)
#define MODE_SIMULTANEOUS_ASSIGNMENT    (1ULL << 33)
#define MODE_TIME_ROUND_FRACTIONAL      (1ULL << 34)
/* The following modes are specific to MySQL */
#define MODE_MYSQL80_TIME_TRUNCATE_FRACTIONAL (1ULL << 32)


/* Bits for different old style modes */
#define OLD_MODE_NO_DUP_KEY_WARNINGS_WITH_IGNORE	(1 << 0)
#define OLD_MODE_NO_PROGRESS_INFO			(1 << 1)
#define OLD_MODE_ZERO_DATE_TIME_CAST                    (1 << 2)
#define OLD_MODE_UTF8_IS_UTF8MB3      (1 << 3)
#define OLD_MODE_IGNORE_INDEX_ONLY_FOR_JOIN          (1 << 4)
#define OLD_MODE_COMPAT_5_1_CHECKSUM    (1 << 5)

extern char internal_table_name[2];
extern char empty_c_string[1];
extern MYSQL_PLUGIN_IMPORT const char **errmesg;

extern "C" LEX_STRING * thd_query_string (MYSQL_THD thd);
extern "C" unsigned long long thd_query_id(const MYSQL_THD thd);
extern "C" size_t thd_query_safe(MYSQL_THD thd, char *buf, size_t buflen);
extern "C" const char *thd_priv_user(MYSQL_THD thd,  size_t *length);
extern "C" const char *thd_priv_host(MYSQL_THD thd,  size_t *length);
extern "C" const char *thd_user_name(MYSQL_THD thd);
extern "C" const char *thd_client_host(MYSQL_THD thd);
extern "C" const char *thd_client_ip(MYSQL_THD thd);
extern "C" LEX_CSTRING *thd_current_db(MYSQL_THD thd);
extern "C" int thd_current_status(MYSQL_THD thd);
extern "C" enum enum_server_command thd_current_command(MYSQL_THD thd);

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
  size_t length;
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
  ha_rows records;        /**< Number of processed records */
  ha_rows deleted;        /**< Number of deleted records */
  ha_rows updated;        /**< Number of updated records */
  ha_rows copied;         /**< Number of copied records */
  ha_rows accepted_rows;  /**< Number of accepted original rows
                             (same as number of rows in RETURNING) */
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
  LEX_CSTRING field_name;
  uint length;
  bool generated, asc;
  Key_part_spec(const LEX_CSTRING *name, uint len, bool gen= false)
    : field_name(*name), length(len), generated(gen), asc(1)
  {}
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
  bool check_key_for_blob(const class handler *file) const;
  bool check_key_length_for_blob() const;
  bool check_primary_key_for_blob(const class handler *file) const
  {
    return check_key_for_blob(file) || check_key_length_for_blob();
  }
  bool check_foreign_key_for_blob(const class handler *file) const
  {
    return check_key_for_blob(file) || check_key_length_for_blob();
  }
  bool init_multiple_key_for_blob(const class handler *file);
};


class Alter_drop :public Sql_alloc {
public:
  enum drop_type { KEY, COLUMN, FOREIGN_KEY, CHECK_CONSTRAINT, PERIOD };
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
           type == PERIOD ? "PERIOD" :
           type == KEY ? "INDEX" : "FOREIGN KEY";
  }
};


class Alter_column :public Sql_alloc {
public:
  LEX_CSTRING name;
  LEX_CSTRING new_name;
  Virtual_column_info *default_value;
  bool alter_if_exists;
  Alter_column(LEX_CSTRING par_name, Virtual_column_info *expr, bool par_exists)
    :name(par_name), new_name{NULL, 0}, default_value(expr), alter_if_exists(par_exists) {}
  Alter_column(LEX_CSTRING par_name, LEX_CSTRING _new_name, bool exists)
    :name(par_name), new_name(_new_name), default_value(NULL), alter_if_exists(exists) {}
  /**
    Used to make a clone of this object for ALTER/CREATE TABLE
    @sa comment for Key_part_spec::clone
  */
  Alter_column *clone(MEM_ROOT *mem_root) const
    { return new (mem_root) Alter_column(*this); }
  bool is_rename()
  {
    DBUG_ASSERT(!new_name.str || !default_value);
    return new_name.str;
  }
};


class Alter_rename_key : public Sql_alloc
{
public:
  LEX_CSTRING old_name;
  LEX_CSTRING new_name;
  bool alter_if_exists;

  Alter_rename_key(LEX_CSTRING old_name_arg, LEX_CSTRING new_name_arg, bool exists)
      : old_name(old_name_arg), new_name(new_name_arg), alter_if_exists(exists) {}

  Alter_rename_key *clone(MEM_ROOT *mem_root) const
    { return new (mem_root) Alter_rename_key(*this); }

};


/* An ALTER INDEX operation that changes the ignorability of an index. */
class Alter_index_ignorability: public Sql_alloc
{
public:
  Alter_index_ignorability(const char *name, bool is_ignored, bool if_exists) :
    m_name(name), m_is_ignored(is_ignored), m_if_exists(if_exists)
  {
    assert(name != NULL);
  }

  const char *name() const { return m_name; }
  bool if_exists() const { return m_if_exists; }

  /* The ignorability after the operation is performed. */
  bool is_ignored() const { return m_is_ignored; }
  Alter_index_ignorability *clone(MEM_ROOT *mem_root) const
    { return new (mem_root) Alter_index_ignorability(*this); }

private:
  const char *m_name;
  bool m_is_ignored;
  bool m_if_exists;
};


class Key :public Sql_alloc, public DDL_options {
public:
  enum Keytype { PRIMARY, UNIQUE, MULTIPLE, FULLTEXT, SPATIAL, FOREIGN_KEY};
  enum Keytype type;
  KEY_CREATE_INFO key_create_info;
  List<Key_part_spec> columns;
  LEX_CSTRING name;
  engine_option_value *option_list;
  bool generated;
  bool invisible;
  bool without_overlaps;
  Lex_ident period;

  Key(enum Keytype type_par, const LEX_CSTRING *name_arg,
      ha_key_alg algorithm_arg, bool generated_arg, DDL_options_st ddl_options)
    :DDL_options(ddl_options),
     type(type_par), key_create_info(default_key_create_info),
    name(*name_arg), option_list(NULL), generated(generated_arg),
    invisible(false), without_overlaps(false)
  {
    key_create_info.algorithm= algorithm_arg;
  }
  Key(enum Keytype type_par, const LEX_CSTRING *name_arg,
      KEY_CREATE_INFO *key_info_arg,
      bool generated_arg, List<Key_part_spec> *cols,
      engine_option_value *create_opt, DDL_options_st ddl_options)
    :DDL_options(ddl_options),
     type(type_par), key_create_info(*key_info_arg), columns(*cols),
    name(*name_arg), option_list(create_opt), generated(generated_arg),
    invisible(false), without_overlaps(false)
  {}
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
  LEX_CSTRING constraint_name;
  LEX_CSTRING ref_db;
  LEX_CSTRING ref_table;
  List<Key_part_spec> ref_columns;
  enum enum_fk_option delete_opt, update_opt;
  enum fk_match_opt match_opt;
  Foreign_key(const LEX_CSTRING *name_arg, List<Key_part_spec> *cols,
              const LEX_CSTRING *constraint_name_arg,
	      const LEX_CSTRING *ref_db_arg, const LEX_CSTRING *ref_table_arg,
              List<Key_part_spec> *ref_cols,
              enum_fk_option delete_opt_arg, enum_fk_option update_opt_arg,
              fk_match_opt match_opt_arg,
	      DDL_options ddl_options)
    :Key(FOREIGN_KEY, name_arg, &default_key_create_info, 0, cols, NULL,
         ddl_options),
    constraint_name(*constraint_name_arg),
    ref_db(*ref_db_arg), ref_table(*ref_table_arg), ref_columns(*ref_cols),
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
  THR_LOCK_DATA **locks;
  uint table_count,lock_count;
  uint flags;
} MYSQL_LOCK;


class LEX_COLUMN : public Sql_alloc
{
public:
  String column;
  privilege_t rights;
  LEX_COLUMN (const String& x,const  privilege_t & y ): column (x),rights (y) {}
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
  DIAG_ROW_NUMBER= 12,
  LAST_DIAG_SET_PROPERTY= DIAG_ROW_NUMBER
} Diag_condition_item_name;

/**
  Name of each diagnostic condition item.
  This array is indexed by Diag_condition_item_name.
*/
extern const LEX_CSTRING Diag_condition_item_names[];

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
  ulonglong optimizer_trace;
  ulong optimizer_trace_max_mem_size;
  sql_mode_t sql_mode; ///< which non-standard SQL behaviour should be enabled
  sql_mode_t old_behavior; ///< which old SQL behaviour should be enabled
  ulonglong option_bits; ///< OPTION_xxx constants, e.g. OPTION_PROFILING
  ulonglong join_buff_space_limit;
  ulonglong log_slow_filter; 
  ulonglong log_slow_verbosity; 
  ulonglong log_slow_disabled_statements;
  ulonglong log_disabled_statements;
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
  ulong saved_lock_wait_timeout;
  ulonglong wsrep_gtid_seq_no;
#endif /* WITH_WSREP */
  uint eq_range_index_dive_limit;
  ulong column_compression_zlib_strategy;
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
  double sample_percentage;
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
  ulong alter_algorithm;
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
  my_bool old_passwords;
  my_bool big_tables;
  my_bool only_standard_compliant_cte;
  my_bool query_cache_strip_comments;
  my_bool sql_log_slow;
  my_bool sql_log_bin;
  my_bool binlog_annotate_row_events;
  my_bool binlog_direct_non_trans_update;
  my_bool column_compression_zlib_wrap;

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
  LEX_CSTRING default_master_connection;

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
  uint    wsrep_sync_wait;
  ulong   wsrep_retry_autocommit;
  ulonglong wsrep_trx_fragment_size;
  ulong   wsrep_trx_fragment_unit;
  ulong   wsrep_OSU_method;
  my_bool wsrep_dirty_reads;
  double long_query_time_double, max_statement_time_double;

  my_bool pseudo_slave_mode;

  char *session_track_system_variables;
  ulong session_track_transaction_info;
  my_bool session_track_schema;
  my_bool session_track_state_change;
#ifdef USER_VAR_TRACKING
  my_bool session_track_user_variables;
#endif // USER_VAR_TRACKING
  my_bool tcp_nodelay;

  ulong threadpool_priority;

  uint idle_transaction_timeout;
  uint idle_readonly_transaction_timeout;
  uint idle_write_transaction_timeout;
  uint column_compression_threshold;
  uint column_compression_zlib_level;
  uint in_subquery_conversion_threshold;
  ulong optimizer_max_sel_arg_weight;
  ulonglong max_rowid_filter_size;

  vers_asof_timestamp_t vers_asof_timestamp;
  ulong vers_alter_history;
  my_bool binlog_alter_two_phase;
} SV;

/**
  Per thread status variables.
  Must be long/ulong up to last_system_status_var so that
  add_to_status/add_diff_to_status can work.
*/

typedef struct system_status_var
{
  ulong column_compressions;
  ulong column_decompressions;
  ulong com_stat[(uint) SQLCOM_END];
  ulong com_create_tmp_table;
  ulong com_drop_tmp_table;
  ulong com_other;

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
  ulong ha_tmp_delete_count;
  ulong ha_prepare_count;
  ulong ha_icp_attempts;
  ulong ha_icp_match;
  ulong ha_discover_count;
  ulong ha_savepoint_count;
  ulong ha_savepoint_rollback_count;
  ulong ha_external_lock_count;

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
  ulong feature_custom_aggregate_functions; /* +1 when custom aggregate
                                            functions are used */
  ulong feature_dynamic_columns;    /* +1 when creating a dynamic column */
  ulong feature_fulltext;	    /* +1 when MATCH is used */
  ulong feature_gis;                /* +1 opening a table with GIS features */
  ulong feature_invisible_columns;     /* +1 opening a table with invisible column */
  ulong feature_json;		    /* +1 when JSON function appears in the statement */
  ulong feature_locale;		    /* +1 when LOCALE is set */
  ulong feature_subquery;	    /* +1 when subqueries are used */
  ulong feature_system_versioning;  /* +1 opening a table WITH SYSTEM VERSIONING */
  ulong feature_application_time_periods;
                                    /* +1 opening a table with application-time period */
  ulong feature_insert_returning;  /* +1 when INSERT...RETURNING is used */
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
   Number of times where column info was not
   sent with prepared statement metadata.
  */
  ulong skip_metadata_count;

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
  ulonglong table_open_cache_hits;
  ulonglong table_open_cache_misses;
  ulonglong table_open_cache_overflows;
  ulonglong send_metadata_skips;
  double last_query_cost;
  double cpu_time, busy_time;
  uint32 threads_running;
  /* Don't initialize */
  /* Memory used for thread local storage */
  int64 max_local_memory_used;
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

/** Number of contiguous global status variables */
constexpr int COUNT_GLOBAL_STATUS_VARS= int(offsetof(STATUS_VAR,
                                                     last_system_status_var) /
                                            sizeof(ulong)) + 1;

/*
  Global status variables
*/

extern ulong feature_files_opened_with_delayed_keys, feature_check_constraint;

void add_to_status(STATUS_VAR *to_var, STATUS_VAR *from_var);

void add_diff_to_status(STATUS_VAR *to_var, STATUS_VAR *from_var,
                        STATUS_VAR *dec_var);

uint calc_sum_of_all_status(STATUS_VAR *to);
static inline void calc_sum_of_all_status_if_needed(STATUS_VAR *to)
{
  if (to->local_memory_used == 0)
  {
    mysql_mutex_lock(&LOCK_status);
    *to= global_status_var;
    mysql_mutex_unlock(&LOCK_status);
    calc_sum_of_all_status(to);
    DBUG_ASSERT(to->local_memory_used);
  }
}

/*
  Update global_memory_used. We have to do this with atomic_add as the
  global value can change outside of LOCK_status.
*/
static inline void update_global_memory_status(int64 size)
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
static inline CHARSET_INFO *
mysqld_collation_get_by_name(const char *name, myf utf8_flag,
                             CHARSET_INFO *name_cs= system_charset_info)
{
  CHARSET_INFO *cs;
  MY_CHARSET_LOADER loader;
  my_charset_loader_init_mysys(&loader);

  if (!(cs= my_collation_get_by_name(&loader, name, MYF(utf8_flag))))
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

static inline bool is_supported_parser_charset(CHARSET_INFO *cs)
{
  return MY_TEST(cs->mbminlen == 1 && cs->number != 17 /* filename */);
}

/** THD registry */
class THD_list_iterator
{
protected:
  I_List<THD> threads;
  mutable mysql_rwlock_t lock;

public:

  /**
    Iterates registered threads.

    @param action      called for every element
    @param argument    opque argument passed to action

    @return
      @retval 0 iteration completed successfully
      @retval 1 iteration was interrupted (action returned 1)
  */
  template <typename T> int iterate(my_bool (*action)(THD *thd, T *arg), T *arg= 0)
  {
    int res= 0;
    mysql_rwlock_rdlock(&lock);
    I_List_iterator<THD> it(threads);
    while (auto tmp= it++)
      if ((res= action(tmp, arg)))
        break;
    mysql_rwlock_unlock(&lock);
    return res;
  }
  static THD_list_iterator *iterator();
};

/**
  A counter of THDs

  It must be specified as a first base class of THD, so that increment is
  done before any other THD constructors and decrement - after any other THD
  destructors.

  Destructor unblocks close_conneciton() if there are no more THD's left.
*/
struct THD_count
{
  static Atomic_counter<uint32_t> count;
  static uint value() { return static_cast<uint>(count); }
  static uint connection_thd_count();
  THD_count() { count++; }
  ~THD_count() { count--; }
};

#ifdef MYSQL_SERVER

void free_tmp_table(THD *thd, TABLE *entry);


/* The following macro is to make init of Query_arena simpler */
#ifdef DBUG_ASSERT_EXISTS
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
#ifdef DBUG_ASSERT_EXISTS
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

public:
  /* We build without RTTI, so dynamic_cast can't be used. */
  enum Type
  {
    STATEMENT, PREPARED_STATEMENT, STORED_PROCEDURE
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
    if (likely((ptr=alloc_root(mem_root,size))))
      bzero(ptr, size);
    return ptr;
  }
  inline char *strdup(const char *str)
  { return strdup_root(mem_root,str); }
  inline char *strmake(const char *str, size_t size)
  { return strmake_root(mem_root,str,size); }
  inline void *memdup(const void *str, size_t size)
  { return memdup_root(mem_root,str,size); }
  inline void *memdup_w_gap(const void *str, size_t size, size_t gap)
  {
    void *ptr;
    if (likely((ptr= alloc_root(mem_root,size+gap))))
      memcpy(ptr,str,size);
    return ptr;
  }

  void set_query_arena(Query_arena *set);

  void free_items();
  /* Close the active state associated with execution of this statement */
  virtual bool cleanup_stmt(bool /*restore_set_statement_vars*/);
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


class Query_arena_stmt
{
  THD *thd;
  Query_arena backup;
  Query_arena *arena;

public:
  Query_arena_stmt(THD *_thd);
  ~Query_arena_stmt();
  bool arena_replaced()
  {
    return arena != NULL;
  }
};


class Server_side_cursor;

/*
  Struct to catch changes in column metadata that is sent to client. 
  in the "result set metadata". Used to support 
  MARIADB_CLIENT_CACHE_METADATA.
*/
struct send_column_info_state
{
  /* Last client charset (affects metadata) */
  CHARSET_INFO *last_charset= nullptr;

  /* Checksum, only used to check changes if 'immutable' is false*/
  uint32 checksum= 0;

  /*
    Column info can only be changed by PreparedStatement::reprepare()
 
    There is a class of "weird" prepared statements like SELECT ? or SELECT @a
    that are not immutable, and depend on input parameters or user variables
  */
  bool immutable= false;

  bool initialized= false;

  /*  Used by PreparedStatement::reprepare()*/
  void reset()
  {
    initialized= false;
    checksum= 0;
  }
};


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

  enum enum_column_usage column_usage;

  LEX_CSTRING name; /* name for named prepared statements */
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
  inline char *query_end() const
  {
    return query_string.str() + query_string.length();
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

    If there is the current (default) database, "db.str" contains its name. If
    there is no current (default) database, "db.str" is NULL and "db.length" is
    0. In other words, db must either be NULL, or contain a
    valid database name.
  */

  LEX_CSTRING db;

  send_column_info_state column_info_state;
 
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

  Statement *find_by_name(const LEX_CSTRING *name)
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

/**
  @class Security_context
  @brief A set of THD members describing the current authenticated user.
*/

class Security_context {
public:
  Security_context()
   :master_access(NO_ACL),
    db_access(NO_ACL)
  {}                      /* Remove gcc warning */
  /*
    host - host of the client
    user - user of the client, set to NULL until the user has been read from
    the connection
    priv_user - The user privilege we are using. May be "" for anonymous user.
    ip - client IP
  */
  const char *host;
  const char *user, *ip;
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
  privilege_t master_access;            /* Global privileges from mysql.user */
  privilege_t db_access;                /* Privileges for current db */

  bool password_expired;

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
                          LEX_CSTRING *definer_user,
                          LEX_CSTRING *definer_host,
                          LEX_CSTRING *db,
                          Security_context **backup);

  void
  restore_security_context(THD *thd, Security_context *backup);
#endif
  bool user_matches(Security_context *);
  /**
    Check global access
    @param want_access The required privileges
    @param match_any if the security context must match all or any of the req.
   *                 privileges.
    @return True if the security context fulfills the access requirements.
  */
  bool check_access(const privilege_t want_access, bool match_any = false);
  bool is_priv_user(const char *user, const char *host);
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
  /*
     TODO: remove LTM_PRELOCKED_UNDER_LOCK_TABLES: it is never used apart from
     LTM_LOCK_TABLES.
  */
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

  void reset_open_tables_state()
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
  Discrete_interval auto_inc_interval_for_cur_row;
  Discrete_intervals_list auto_inc_intervals_forced;
  SAVEPOINT *savepoints;
  ulonglong option_bits;
  ulonglong first_successful_insert_id_in_prev_stmt;
  ulonglong first_successful_insert_id_in_cur_stmt, insert_id_for_cur_row;
  ulonglong limit_found_rows;
  ulonglong tmp_tables_size;
  ulonglong client_capabilities;
  ulonglong cuted_fields, sent_row_count, examined_row_count;
  ulonglong affected_rows;
  ulonglong bytes_sent_old;
  ulong     tmp_tables_used;
  ulong     tmp_tables_disk_used;
  ulong     query_plan_fsort_passes;
  ulong query_plan_flags; 
  uint in_sub_stmt;    /* 0,  SUB_STMT_TRIGGER or SUB_STMT_FUNCTION */
  bool enable_slow_log;
  bool last_insert_id_used;
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
  SYSTEM_THREAD_GENERIC= 128,
  SYSTEM_THREAD_SEMISYNC_MASTER_BACKGROUND= 256
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
    RETURN_NAME_AS_STRING(SYSTEM_THREAD_SEMISYNC_MASTER_BACKGROUND);
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


class Turn_errors_to_warnings_handler : public Internal_error_handler
{
public:
  Turn_errors_to_warnings_handler() {}
  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl)
  {
    *cond_hdl= NULL;
    if (*level == Sql_condition::WARN_LEVEL_ERROR)
      *level= Sql_condition::WARN_LEVEL_WARN;
    return(0);
  }
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
public:
  MEM_ROOT m_locked_tables_root;
private:
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
    init_sql_alloc(key_memory_locked_table_list, &m_locked_tables_root,
                   MEM_ROOT_BLOCK_SIZE, 0, MYF(MY_THREAD_SPECIFIC));
  }
  int unlock_locked_tables(THD *thd);
  int unlock_locked_table(THD *thd, MDL_ticket *mdl_ticket);
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
  void mark_table_for_reopen(TABLE *table);
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

  void reset()
  {
    ha_ptr= nullptr;
    for (auto &info : ha_info)
      info.reset();
    lock= nullptr;
  }
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
      m_mdl_global_read_lock(NULL)
  {}

  bool lock_global_read_lock(THD *thd);
  void unlock_global_read_lock(THD *thd);
  bool make_global_read_lock_block_commit(THD *thd);
  bool is_acquired() const { return m_state != GRL_NONE; }
  void set_explicit_lock_duration(THD *thd);
private:
  enum_grl_state m_state;
  /**
    Global read lock is acquired in two steps:
    1. acquire MDL_BACKUP_FTWRL1 in BACKUP namespace to prohibit DDL and DML
    2. upgrade to MDL_BACKUP_FTWRL2 to prohibit commits
  */
  MDL_ticket *m_mdl_global_read_lock;
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
    waitee and associated COND_wait_commit (for a waiter).
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

    This pointer is protected by LOCK_wait_commit. But there is also a "fast
    path" where the waiter compares this to NULL without holding the lock.
    Such read must be done with acquire semantics (and all corresponding
    writes done with release semantics). This ensures that a wakeup with error
    is reliably detected as (waitee==NULL && wakeup_error != 0).
  */
  std::atomic<wait_for_commit *> waitee;
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
    if (waitee.load(std::memory_order_acquire))
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
    if (waitee.load(std::memory_order_relaxed))
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
    waitee.store(NULL, std::memory_order_relaxed);
  }

  void wakeup(int wakeup_error);

  int wait_for_prior_commit2(THD *thd);
  void wakeup_subsequent_commits2(int wakeup_error);
  void unregister_wait_for_prior_commit2();

  wait_for_commit();
  ~wait_for_commit();
  void reinit();
};


class Sp_caches
{
public:
  sp_cache *sp_proc_cache;
  sp_cache *sp_func_cache;
  sp_cache *sp_package_spec_cache;
  sp_cache *sp_package_body_cache;
  Sp_caches()
   :sp_proc_cache(NULL),
    sp_func_cache(NULL),
    sp_package_spec_cache(NULL),
    sp_package_body_cache(NULL)
  { }
  ~Sp_caches()
  {
    // All caches must be freed by the caller explicitly
    DBUG_ASSERT(sp_proc_cache == NULL);
    DBUG_ASSERT(sp_func_cache == NULL);
    DBUG_ASSERT(sp_package_spec_cache == NULL);
    DBUG_ASSERT(sp_package_body_cache == NULL);
  }
  void sp_caches_swap(Sp_caches &rhs)
  {
    swap_variables(sp_cache*, sp_proc_cache, rhs.sp_proc_cache);
    swap_variables(sp_cache*, sp_func_cache, rhs.sp_func_cache);
    swap_variables(sp_cache*, sp_package_spec_cache, rhs.sp_package_spec_cache);
    swap_variables(sp_cache*, sp_package_body_cache, rhs.sp_package_body_cache);
  }
  void sp_caches_clear();
};


extern "C" void my_message_sql(uint error, const char *str, myf MyFlags);


class Gap_time_tracker;

/*
  Thread context for Gap_time_tracker class.
*/
class Gap_time_tracker_data
{
public:
  Gap_time_tracker_data(): bill_to(NULL) {}

  Gap_time_tracker *bill_to;
  ulonglong start_time;

  void init() { bill_to = NULL; }
};

/**
  Support structure for asynchronous group commit, or more generally
  any asynchronous operation that needs to finish before server writes
  response to client.

  An engine, or any other server component, can signal that there is
  a pending operation by incrementing a counter, i.e inc_pending_ops()
  and that pending operation is finished by decrementing that counter
  dec_pending_ops().

  NOTE: Currently, pending operations can not fail, i.e there is no
  way to pass a return code in dec_pending_ops()

  The server does not write response to the client before the counter
  becomes 0. In  case of group commit it ensures that data is persistent
  before success reported to client, i.e durability in ACID.
*/
struct thd_async_state
{
  enum class enum_async_state
  {
    NONE,
    SUSPENDED, /* do_command() did not finish, and needs to be resumed */
    RESUMED    /* do_command() is resumed*/
  };
  enum_async_state m_state{enum_async_state::NONE};

  /* Stuff we need to resume do_command where we finished last time*/
  enum enum_server_command m_command{COM_SLEEP};
  LEX_STRING m_packet{0,0};

  mysql_mutex_t m_mtx;
  mysql_cond_t m_cond;

  /** Pending counter*/
  Atomic_counter<int> m_pending_ops=0;

#ifndef DBUG_OFF
  /* Checks */
  pthread_t m_dbg_thread;
#endif

  thd_async_state()
  {
    mysql_mutex_init(PSI_NOT_INSTRUMENTED, &m_mtx, 0);
    mysql_cond_init(PSI_INSTRUMENT_ME, &m_cond, 0);
  }

  /*
   Currently only used with threadpool, one can "suspend" and "resume" a THD.
   Suspend only means leaving do_command earlier, after saving some state.
   Resume is continuing suspended THD's do_command(), from where it finished last time.
  */
  bool try_suspend()
  {
    bool ret;
    mysql_mutex_lock(&m_mtx);
    DBUG_ASSERT(m_state == enum_async_state::NONE);
    DBUG_ASSERT(m_pending_ops >= 0);

    if(m_pending_ops)
    {
      ret=true;
      m_state= enum_async_state::SUSPENDED;
    }
    else
    {
      /*
        If there is no pending operations, can't suspend, since
        nobody can resume it.
      */
      ret=false;
    }
    mysql_mutex_unlock(&m_mtx);
    return ret;
  }

  ~thd_async_state()
  {
    wait_for_pending_ops();
    mysql_mutex_destroy(&m_mtx);
    mysql_cond_destroy(&m_cond);
  }

  /*
    Increment pending asynchronous operations.
    The client response may not be written if
    this count > 0.
    So, without threadpool query needs to wait for
    the operations to finish.
    With threadpool, THD can be suspended and resumed
    when this counter goes to 0.
  */
  void inc_pending_ops()
  {
    mysql_mutex_lock(&m_mtx);

#ifndef DBUG_OFF
    /*
     Check that increments are always done by the same thread.
    */
    if (!m_pending_ops)
      m_dbg_thread= pthread_self();
    else
      DBUG_ASSERT(pthread_equal(pthread_self(),m_dbg_thread));
#endif

    m_pending_ops++;
    mysql_mutex_unlock(&m_mtx);
  }

  int dec_pending_ops(enum_async_state* state)
  {
    int ret;
    mysql_mutex_lock(&m_mtx);
    ret= --m_pending_ops;
    if (!ret)
      mysql_cond_signal(&m_cond);
    *state = m_state;
    mysql_mutex_unlock(&m_mtx);
    return ret;
  }

  /*
    This is used for "dirty" reading pending ops,
    when dirty read is OK.
  */
  int pending_ops()
  {
    return m_pending_ops;
  }

  /* Wait for pending operations to finish.*/
  void wait_for_pending_ops()
  {
    /*
      It is fine to read m_pending_ops and compare it with 0,
      without mutex protection.

      The value is only incremented by the current thread, and will
      be decremented by another one, thus "dirty" may show positive number
      when it is really 0, but this is not a problem, and the only
      bad thing from that will be rechecking under mutex.
    */
    if (!pending_ops())
      return;

    mysql_mutex_lock(&m_mtx);
    DBUG_ASSERT(m_pending_ops >= 0);
    while (m_pending_ops)
      mysql_cond_wait(&m_cond, &m_mtx);
    mysql_mutex_unlock(&m_mtx);
  }
};

extern "C" void thd_increment_pending_ops(MYSQL_THD);
extern "C" void thd_decrement_pending_ops(MYSQL_THD);


/**
  @class THD
  For each client connection we create a separate thread with THD serving as
  a thread/connection descriptor
*/

class THD: public THD_count, /* this must be first */
           public Statement,
           /*
             This is to track items changed during execution of a prepared
             statement/stored procedure. It's created by
             nocheck_register_item_tree_change() in memory root of THD,
             and freed in rollback_item_tree_changes().
             For conventional execution it's always empty.
           */
           public Item_change_list,
           public MDL_context_owner,
           public Open_tables_state,
           public Sp_caches
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
  /* Used for BACKUP LOCK */
  MDL_ticket *mdl_backup_ticket, *mdl_backup_lock;
  /* Used to register that thread has a MDL_BACKUP_WAIT_COMMIT lock */
  MDL_request *backup_commit_lock;

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
    - thd->db (used in SHOW PROCESSLIST)
    Is locked when THD is deleted.
  */
  mutable mysql_mutex_t LOCK_thd_data;
  /*
    Protects:
    - kill information
    - mysys_var (used by KILL statement and shutdown).
    - Also ensures that THD is not deleted while mutex is hold
  */
  mutable mysql_mutex_t LOCK_thd_kill;

  /* all prepared statements and cursors of this connection */
  Statement_map stmt_map;

  /* Last created prepared statement */
  Statement *last_stmt;
  Statement *cur_stmt= 0;

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
  Security_context *security_context() const { return security_ctx; }
  void set_security_context(Security_context *sctx) { security_ctx = sctx; }

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

  void set_psi(PSI_thread *psi)
  {
    my_atomic_storeptr((void*volatile*)&m_psi, psi);
  }

  PSI_thread* get_psi()
  {
    return static_cast<PSI_thread*>(my_atomic_loadptr((void*volatile*)&m_psi));
  }

private:
  unsigned int m_current_stage_key;

  /** Performance schema thread instrumentation for this session. */
  PSI_thread *m_psi;

public:
  void enter_stage(const PSI_stage_info *stage,
                   const char *calling_func,
                   const char *calling_file,
                   const unsigned int calling_line)
  {
    DBUG_PRINT("THD::enter_stage", ("%s at %s:%d", stage->m_name,
                                    calling_file, calling_line));
    DBUG_ASSERT(stage);
    m_current_stage_key= stage->m_key;
    proc_info= stage->m_name;
#if defined(ENABLED_PROFILING)
    profiling.status_change(proc_info, calling_func, calling_file,
                            calling_line);
#endif
#ifdef HAVE_PSI_THREAD_INTERFACE
    m_stage_progress_psi= MYSQL_SET_STAGE(m_current_stage_key, calling_file, calling_line);
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
  /* If this is a semisync slave connection. */
  bool semi_sync_slave;
  ulonglong client_capabilities;  /* What the client supports */
  ulong max_client_packet_length;

  HASH		handler_tables_hash;
  /*
    A thread can hold named user-level locks. This variable
    contains granted tickets if a lock is present. See item_func.cc and
    chapter 'Miscellaneous functions', for functions GET_LOCK, RELEASE_LOCK.
  */
  HASH ull_hash;
  /* Hash of used seqeunces (for PREVIOUS value) */
  HASH sequences;
#ifdef DBUG_ASSERT_EXISTS
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
  /* This can be used by handlers to send signals to the SQL level */
  ulonglong  replication_flags;
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
    If set, tell binlog to store the value as query 'xid' in the next
    Query_log_event
  */
  ulonglong binlog_xid;

  /*
    Public interface to write RBR events to the binlog
  */
  void binlog_start_trans_and_stmt();
  void binlog_set_stmt_begin();
  int binlog_write_row(TABLE* table, bool is_transactional,
                       const uchar *buf);
  int binlog_delete_row(TABLE* table, bool is_transactional,
                        const uchar *buf);
  int binlog_update_row(TABLE* table, bool is_transactional,
                        const uchar *old_data, const uchar *new_data);
  bool prepare_handlers_for_update(uint flag);
  bool binlog_write_annotated_row(Log_event_writer *writer);
  void binlog_prepare_for_row_logging();
  bool binlog_write_table_maps();
  bool binlog_write_table_map(TABLE *table, bool with_annotate);
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

  bool binlog_need_stmt_format(bool is_transactional) const
  {
    return log_current_statement() &&
           !binlog_get_pending_rows_event(is_transactional);
  }

  bool binlog_for_noop_dml(bool transactional_table);

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

public:

  /* 1 if binlog table maps has been written */
  bool binlog_table_maps;

  void issue_unsafe_warnings();
  void reset_unsafe_warnings()
  { binlog_unsafe_warning_flags= 0; }

  void reset_binlog_for_next_statement()
  {
    binlog_table_maps= 0;
  }
  bool binlog_table_should_be_logged(const LEX_CSTRING *db);

  // Accessors and setters of two-phase loggable ALTER binlog properties
  uchar get_binlog_flags_for_alter();
  void   set_binlog_flags_for_alter(uchar);
  uint64 get_binlog_start_alter_seq_no();
  void   set_binlog_start_alter_seq_no(uint64);
#endif /* MYSQL_CLIENT */

public:

  struct st_transactions {
    SAVEPOINT *savepoints;
    THD_TRANS all;			// Trans since BEGIN WORK
    THD_TRANS stmt;			// Trans for current statement
    bool on;                            // see ha_enable_transaction()
    XID_STATE xid_state;
    XID implicit_xid;
    WT_THD wt;                          ///< for deadlock detection
    Rows_log_event *m_pending_rows_event;

    struct st_trans_time : public timeval
    {
      void reset(THD *thd)
      {
        tv_sec= thd->query_start();
        tv_usec= (long) thd->query_start_sec_part();
      }
    } start_time;

    /*
       Tables changed in transaction (that must be invalidated in query cache).
       List contain only transactional tables, that not invalidated in query
       cache (instead of full list of changed in transaction tables).
    */
    CHANGED_TABLE_LIST* changed_tables;
    MEM_ROOT mem_root; // Transaction-life memory allocation pool
    void cleanup()
    {
      DBUG_ENTER("THD::st_transactions::cleanup");
      changed_tables= 0;
      savepoints= 0;
      implicit_xid.null();
      free_root(&mem_root,MYF(MY_KEEP_PREALLOC));
      DBUG_VOID_RETURN;
    }
    void free()
    {
      free_root(&mem_root,MYF(0));
    }
    bool is_active()
    {
      return (all.ha_list != NULL);
    }
    bool is_empty()
    {
      return all.is_empty() && stmt.is_empty();
    }
    st_transactions()
    {
      bzero((char*)this, sizeof(*this));
      implicit_xid.null();
      init_sql_alloc(key_memory_thd_transactions, &mem_root, 256,
                     0, MYF(MY_THREAD_SPECIFIC));
    }
  } default_transaction, *transaction;
  Global_read_lock global_read_lock;
  Field      *dup_field;
#ifndef _WIN32
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
  inline void set_affected_rows(longlong row_count_func)
  {
    /*
      We have to add to affected_rows (used by slow log), as otherwise
      information for 'call' will be wrong
    */
    affected_rows+= (row_count_func >= 0 ? row_count_func : 0);
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

  ulonglong get_affected_rows() const
  { return affected_rows; }

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

  /** Current stage progress instrumentation. */
  PSI_stage_progress *m_stage_progress_psi;
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

  /** Current transaction instrumentation. */
  PSI_transaction_locker *m_transaction_psi;
#ifdef HAVE_PSI_TRANSACTION_INTERFACE
  /** Current transaction instrumentation state. */
  PSI_transaction_locker_state m_transaction_state;
#endif /* HAVE_PSI_TRANSACTION_INTERFACE */

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
  privilege_t col_access;

  /* Statement id is thread-wide. This counter is used to generate ids */
  ulong      statement_id_counter;
  ulong	     rand_saved_seed1, rand_saved_seed2;

  /* The following variables are used when printing to slow log */
  ulong      query_plan_flags; 
  ulong      query_plan_fsort_passes; 
  ulong      tmp_tables_used;
  ulong      tmp_tables_disk_used;
  ulonglong  tmp_tables_size;
  ulonglong  bytes_sent_old;
  ulonglong  affected_rows;                     /* Number of changed rows */

  Opt_trace_context opt_trace;
  pthread_t  real_id;                           /* For debugging */
  my_thread_id  thread_id, thread_dbug_id;
  uint32      os_thread_id;
  uint	     tmp_table, global_disable_checkpoint;
  uint	     server_status,open_options;
  enum enum_thread_type system_thread;
  enum backup_stages current_backup_stage;
#ifdef WITH_WSREP
  bool wsrep_desynced_backup_stage;
#endif /* WITH_WSREP */
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
  inline bool check_killed(bool dont_send_error_message= 0)
  {
    if (unlikely(killed))
    {
      if (!dont_send_error_message)
        send_kill_message();
      return TRUE;
    }
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
  LEX_CSTRING connection_name;
  char       default_master_connection_buff[MAX_CONNECTION_NAME+1];
  uint8      password; /* 0, 1 or 2 */
  uint8      failed_com_change_user;
  bool       slave_thread;
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
  bool	     rand_used, time_zone_used;
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
private:
  bool       charset_is_system_charset, charset_is_collation_connection;
  bool       charset_is_character_set_filesystem;
public:
  bool       enable_slow_log;    /* Enable slow log for current statement */
  bool	     abort_on_warning;
  bool 	     got_warning;       /* Set on call to push_warning() */
  /* set during loop of derived table processing */
  bool       derived_tables_processing;
  bool       tablespace_op;	/* This is TRUE in DISCARD/IMPORT TABLESPACE */
  bool       log_current_statement() const
  {
    return variables.option_bits & OPTION_BINLOG_THIS_STMT;
  }
  /**
    True if a slave error. Causes the slave to stop. Not the same
    as the statement execution error (is_error()), since
    a statement may be expected to return an error, e.g. because
    it returned an error on master, and this is OK on the slave.
  */
  bool       is_slave_error;
  /* True if we have printed something to the error log for this statement */
  bool       error_printed_to_log;

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
  enum_sql_command last_sql_command;  // Last sql_command exceuted in mysql_execute_command()

  sp_rcontext *spcont;		// SP runtime context

  /** number of name_const() substitutions, see sp_head.cc:subst_spvars() */
  uint       query_name_consts;

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
  /**
    @param id                thread identifier
    @param is_wsrep_applier  thread type
  */
  THD(my_thread_id id, bool is_wsrep_applier= false);

  ~THD();

  void init();
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
  void store_globals();
  void reset_globals();
  bool trace_started()
  {
    return opt_trace.is_started();
  }
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
  void awake_no_mutex(killed_state state_to_set);
  void awake(killed_state state_to_set)
  {
    mysql_mutex_lock(&LOCK_thd_kill);
    mysql_mutex_lock(&LOCK_thd_data);
    awake_no_mutex(state_to_set);
    mysql_mutex_unlock(&LOCK_thd_data);
    mysql_mutex_unlock(&LOCK_thd_kill);
  }
  void abort_current_cond_wait(bool force);
 
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

  Gap_time_tracker_data gap_tracker_data;
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
  bool binlog_current_query_unfiltered();
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
  const Type_handler *type_handler_for_datetime() const;
  bool timestamp_to_TIME(MYSQL_TIME *ltime, my_time_t ts,
                         ulong sec_part, date_mode_t fuzzydate);
  inline my_time_t query_start() { return start_time; }
  inline ulong query_start_sec_part()
  { query_start_sec_part_used=1; return start_time_sec_part; }
  MYSQL_TIME query_start_TIME();
  time_round_mode_t temporal_round_mode() const
  {
    return variables.sql_mode & MODE_TIME_ROUND_FRACTIONAL ?
           TIME_FRAC_ROUND : TIME_FRAC_TRUNCATE;
  }

private:
  struct {
    my_hrtime_t start;
    my_time_t sec;
    ulong sec_part;
  } system_time;

  void set_system_time()
  {
    my_hrtime_t hrtime= my_hrtime();
    my_time_t sec= hrtime_to_my_time(hrtime);
    ulong sec_part= hrtime_sec_part(hrtime);
    if (sec > system_time.sec ||
        (sec == system_time.sec && sec_part > system_time.sec_part) ||
        hrtime.val < system_time.start.val)
    {
      system_time.sec= sec;
      system_time.sec_part= sec_part;
      system_time.start= hrtime;
    }
    else
    {
      if (system_time.sec_part < TIME_MAX_SECOND_PART)
        system_time.sec_part++;
      else
      {
        system_time.sec++;
        system_time.sec_part= 0;
      }
    }
  }

public:
  timeval transaction_time()
  {
    if (!in_multi_stmt_transaction_mode())
      transaction->start_time.reset(this);
    return transaction->start_time;
  }

  inline void set_start_time()
  {
    if (user_time.val)
    {
      start_time= hrtime_to_my_time(user_time);
      start_time_sec_part= hrtime_sec_part(user_time);
    }
    else
    {
      set_system_time();
      start_time= system_time.sec;
      start_time_sec_part= system_time.sec_part;
    }
    PSI_CALL_set_thread_start_time(start_time);
  }
  inline void set_time()
  {
    set_start_time();
    start_utime= utime_after_lock= microsecond_interval_timer();
  }
  /* only used in SET @@timestamp=... */
  inline void set_time(my_hrtime_t t)
  {
    user_time= t;
    set_time();
  }
  inline void force_set_time(my_time_t t, ulong sec_part)
  {
    start_time= system_time.sec= t;
    start_time_sec_part= system_time.sec_part= sec_part;
  }
  /*
    this is only used by replication and BINLOG command.
    usecs > TIME_MAX_SECOND_PART means "was not in binlog"
  */
  inline void set_time(my_time_t t, ulong sec_part)
  {
    if (opt_secure_timestamp > (slave_thread ? SECTIME_REPL : SECTIME_SUPER))
      set_time();                 // note that BINLOG itself requires SUPER
    else
    {
      if (sec_part <= TIME_MAX_SECOND_PART)
        force_set_time(t, sec_part);
      else if (t != system_time.sec)
        force_set_time(t, 0);
      else
      {
        start_time= t;
        start_time_sec_part= ++system_time.sec_part;
      }
      user_time.val= hrtime_from_time(start_time) + start_time_sec_part;
      PSI_CALL_set_thread_start_time(start_time);
      start_utime= utime_after_lock= microsecond_interval_timer();
    }
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
    if (utime_after_query >= utime_after_lock + variables.long_query_time)
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
  /* Commit both statement and full transaction */
  int commit_whole_transaction_and_close_tables();
  void give_protection_error();
  /*
    Give an error if any of the following is true for this connection
    - BACKUP STAGE is active
    - FLUSH TABLE WITH READ LOCK is active
    - BACKUP LOCK table_name is active
  */
  inline bool has_read_only_protection()
  {
    if (current_backup_stage == BACKUP_FINISHED &&
        !global_read_lock.is_acquired() &&
        !mdl_backup_lock)
      return FALSE;
    give_protection_error();
    return TRUE;
  }
  inline bool fill_information_schema_tables()
  {
    return !stmt_arena->is_stmt_prepare();
  }
  inline void* trans_alloc(size_t size)
  {
    return alloc_root(&transaction->mem_root,size);
  }

  LEX_CSTRING strmake_lex_cstring(const char *str, size_t length)
  {
    const char *tmp= strmake_root(mem_root, str, length);
    if (!tmp)
      return {0,0};
    return {tmp, length};
  }
  LEX_CSTRING strmake_lex_cstring(const LEX_CSTRING &from)
  {
    return strmake_lex_cstring(from.str, from.length);
  }

  LEX_STRING *make_lex_string(LEX_STRING *lex_str, const char* str, size_t length)
  {
    if (!(lex_str->str= strmake_root(mem_root, str, length)))
    {
      lex_str->length= 0;
      return 0;
    }
    lex_str->length= length;
    return lex_str;
  }
  LEX_CSTRING *make_lex_string(LEX_CSTRING *lex_str, const char* str, size_t length)
  {
    if (!(lex_str->str= strmake_root(mem_root, str, length)))
    {
      lex_str->length= 0;
      return 0;
    }
    lex_str->length= length;
    return lex_str;
  }
  // Remove double quotes:  aaa""bbb -> aaa"bbb
  bool quote_unescape(LEX_CSTRING *dst, const LEX_CSTRING *src, char quote)
  {
    const char *tmp= src->str;
    const char *tmpend= src->str + src->length;
    char *to;
    if (!(dst->str= to= (char *) alloc(src->length + 1)))
    {
      dst->length= 0; // Safety
      return true;
    }
    for ( ; tmp < tmpend; )
    {
      if ((*to++= *tmp++) == quote)
        tmp++;                                  // Skip double quotes
    }
    *to= 0;                                     // End null for safety
    dst->length= to - dst->str;
    return false;
  }

  LEX_CSTRING *make_clex_string(const char* str, size_t length)
  {
    LEX_CSTRING *lex_str;
    char *tmp;
    if (unlikely(!(lex_str= (LEX_CSTRING *)alloc_root(mem_root,
                                                      sizeof(LEX_CSTRING) +
                                                      length+1))))
      return 0;
    tmp= (char*) (lex_str+1);
    lex_str->str= tmp;
    memcpy(tmp, str, length);
    tmp[length]= 0;
    lex_str->length= length;
    return lex_str;
  }
  LEX_CSTRING *make_clex_string(const LEX_CSTRING from)
  {
    return make_clex_string(from.str, from.length);
  }

  // Allocate LEX_STRING for character set conversion
  bool alloc_lex_string(LEX_STRING *dst, size_t length)
  {
    if (likely((dst->str= (char*) alloc(length))))
      return false;
    dst->length= 0;  // Safety
    return true;     // EOM
  }
  bool convert_string(LEX_STRING *to, CHARSET_INFO *to_cs,
		      const char *from, size_t from_length,
		      CHARSET_INFO *from_cs);
  bool reinterpret_string_from_binary(LEX_CSTRING *to, CHARSET_INFO *to_cs,
                                      const char *from, size_t from_length);
  bool convert_string(LEX_CSTRING *to, CHARSET_INFO *to_cs,
                      const char *from, size_t from_length,
                      CHARSET_INFO *from_cs)
  {
    LEX_STRING tmp;
    bool rc= convert_string(&tmp, to_cs, from, from_length, from_cs);
    to->str= tmp.str;
    to->length= tmp.length;
    return rc;
  }
  bool convert_string(LEX_CSTRING *to, CHARSET_INFO *tocs,
                      const LEX_CSTRING *from, CHARSET_INFO *fromcs,
                      bool simple_copy_is_possible)
  {
    if (!simple_copy_is_possible)
      return unlikely(convert_string(to, tocs, from->str, from->length, fromcs));
    if (fromcs == &my_charset_bin)
      return reinterpret_string_from_binary(to, tocs, from->str, from->length);
    *to= *from;
    return false;
  }
  /*
    Convert a strings between character sets.
    Uses my_convert_fix(), which uses an mb_wc .. mc_mb loop internally.
    dstcs and srccs cannot be &my_charset_bin.
  */
  bool convert_fix(CHARSET_INFO *dstcs, LEX_STRING *dst,
                   CHARSET_INFO *srccs, const char *src, size_t src_length,
                   String_copier *status);

  /*
    Same as above, but additionally sends ER_INVALID_CHARACTER_STRING
    in case of bad byte sequences or Unicode conversion problems.
  */
  bool convert_with_error(CHARSET_INFO *dstcs, LEX_STRING *dst,
                          CHARSET_INFO *srccs,
                          const char *src, size_t src_length);
  /*
    If either "dstcs" or "srccs" is &my_charset_bin,
    then performs native copying using copy_fix().
    Otherwise, performs Unicode conversion using convert_fix().
  */
  bool copy_fix(CHARSET_INFO *dstcs, LEX_STRING *dst,
                CHARSET_INFO *srccs, const char *src, size_t src_length,
                String_copier *status);

  /*
    Same as above, but additionally sends ER_INVALID_CHARACTER_STRING
    in case of bad byte sequences or Unicode conversion problems.
  */
  bool copy_with_error(CHARSET_INFO *dstcs, LEX_STRING *dst,
                       CHARSET_INFO *srccs, const char *src, size_t src_length);

  bool convert_string(String *s, CHARSET_INFO *from_cs, CHARSET_INFO *to_cs);

  /*
    Check if the string is wellformed, raise an error if not wellformed.
    @param str    - The string to check.
    @param length - the string length.
  */
  bool check_string_for_wellformedness(const char *str,
                                       size_t length,
                                       CHARSET_INFO *cs) const;

  bool to_ident_sys_alloc(Lex_ident_sys_st *to, const Lex_ident_cli_st *from);

  /*
    Create a string literal with optional client->connection conversion.
    @param str        - the string in the client character set
    @param length     - length of the string
    @param repertoire - the repertoire of the string
  */
  Item_basic_constant *make_string_literal(const char *str, size_t length,
                                           my_repertoire_t repertoire);
  Item_basic_constant *make_string_literal(const Lex_string_with_metadata_st &str)
  {
    my_repertoire_t repertoire= str.repertoire(variables.character_set_client);
    return make_string_literal(str.str, str.length, repertoire);
  }
  Item_basic_constant *make_string_literal_nchar(const Lex_string_with_metadata_st &str);
  Item_basic_constant *make_string_literal_charset(const Lex_string_with_metadata_st &str,
                                                   CHARSET_INFO *cs);
  bool make_text_string_sys(LEX_CSTRING *to,
                            const Lex_string_with_metadata_st *from)
  {
    return convert_string(to, system_charset_info,
                          from, charset(), charset_is_system_charset);
  }
  bool make_text_string_connection(LEX_CSTRING *to,
                                   const Lex_string_with_metadata_st *from)
  {
    return convert_string(to, variables.collation_connection,
                          from, charset(), charset_is_collation_connection);
  }
  bool make_text_string_filesystem(LEX_CSTRING *to,
                                   const Lex_string_with_metadata_st *from)
  {
    return convert_string(to, variables.character_set_filesystem,
                          from, charset(), charset_is_character_set_filesystem);
  }
  void add_changed_table(TABLE *table);
  void add_changed_table(const char *key, size_t key_length);
  CHANGED_TABLE_LIST * changed_table_dup(const char *key, size_t key_length);
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

  inline CHARSET_INFO *charset() const { return variables.character_set_client; }
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
    return !stmt_arena->is_conventional();
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
        if (likely(killed_err))
        {
          killed_err->no= killed_errno_arg;
          ::strmake((char*) killed_err->msg, killed_err_msg_arg,
                    sizeof(killed_err->msg)-1);
        }
      }
    }
  }
  int killed_errno();
  void reset_killed();
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
      my_message(err, killed_err ? killed_err->msg : ER_THD(this, err), MYF(0));
    mysql_mutex_unlock(&LOCK_thd_kill);
  }
  /* return TRUE if we will abort query if we make a warning now */
  inline bool really_abort_on_warning()
  {
    return (abort_on_warning &&
            (!transaction->stmt.modified_non_trans_table ||
             (variables.sql_mode & MODE_STRICT_ALL_TABLES)));
  }
  void set_status_var_init();
  void reset_n_backup_open_tables_state(Open_tables_backup *backup);
  void restore_backup_open_tables_state(Open_tables_backup *backup);
  void reset_sub_statement_state(Sub_statement_state *backup, uint new_state);
  void restore_sub_statement_state(Sub_statement_state *backup);
  void store_slow_query_state(Sub_statement_state *backup);
  void reset_slow_query_state();
  void add_slow_query_state(Sub_statement_state *backup);
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

  inline void set_current_stmt_binlog_format(enum_binlog_format format)
  {
    current_stmt_binlog_format= format;
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
                YESNO(has_temporary_tables()), YESNO(in_sub_stmt),
                show_system_thread(system_thread)));
    if (in_sub_stmt == 0)
    {
      if (wsrep_binlog_format() == BINLOG_FORMAT_ROW)
        set_current_stmt_binlog_format_row();
      else if (!has_temporary_tables())
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
  bool set_db(const LEX_CSTRING *new_db);

  /** Set the current database, without copying */
  void reset_db(const LEX_CSTRING *new_db);

  /*
    Copy the current database to the argument. Use the current arena to
    allocate memory for a deep copy: current database may be freed after
    a statement is parsed but before it's executed.

    Can only be called by owner of thd (no mutex protection)
  */
  bool copy_db_to(LEX_CSTRING *to)
  {
    if (db.str == NULL)
    {
      /*
        No default database is set. In this case if it's guaranteed that
        no CTE can be used in the statement then we can throw an error right
        now at the parser stage. Otherwise the decision about throwing such
        a message must be postponed until a post-parser stage when we are able
        to resolve all CTE names as we don't need this message to be thrown
        for any CTE references.
      */
      if (!lex->with_cte_resolution)
        my_message(ER_NO_DB_ERROR, ER(ER_NO_DB_ERROR), MYF(0));
      return TRUE;
    }

    to->str= strmake(db.str, db.length);
    to->length= db.length;
    return to->str == NULL;                     /* True on error */
  }
  /* Get db name or "". Use for printing current db */
  const char *get_db()
  { return safe_str(db.str); }

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

  /**
    @brief Push an error message into MySQL error stack with line
    and position information.

    This function provides semantic action implementers with a way
    to push the famous "You have a syntax error near..." error
    message into the error stack, which is normally produced only if
    a parse error is discovered internally by the Bison generated
    parser.
  */
  void parse_error(const char *err_text, const char *yytext)
  {
    Lex_input_stream *lip= &m_parser_state->m_lip;
    if (!yytext && !(yytext= lip->get_tok_start()))
        yytext= "";
    /* Push an error into the error stack */
    ErrConvString err(yytext, strlen(yytext), variables.character_set_client);
    my_printf_error(ER_PARSE_ERROR,  ER_THD(this, ER_PARSE_ERROR), MYF(0),
                    err_text, err.ptr(), lip->yylineno);
  }
  void parse_error(uint err_number, const char *yytext= 0)
  {
    parse_error(ER_THD(this, err_number), yytext);
  }
  void parse_error()
  {
    parse_error(ER_SYNTAX_ERROR);
  }
#ifdef mysqld_error_find_printf_error_used
  void parse_error(const char *t)
  {
  }
#endif
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
  Sql_condition* raise_condition(uint sql_errno, const char* sqlstate,
                  Sql_condition::enum_warning_level level, const char* msg)
  {
    Sql_condition cond(NULL, // don't strdup the msg
                       Sql_condition_identity(sql_errno, sqlstate, level,
                                              Sql_user_condition_identity()),
                       msg, get_stmt_da()->current_row_for_warning());
    return raise_condition(&cond);
  }

  Sql_condition* raise_condition(const Sql_condition *cond);

private:
  void push_warning_truncated_priv(Sql_condition::enum_warning_level level,
                                   uint sql_errno,
                                   const char *type_str, const char *val)
  {
    DBUG_ASSERT(sql_errno == ER_TRUNCATED_WRONG_VALUE ||
                sql_errno == ER_WRONG_VALUE);
    char buff[MYSQL_ERRMSG_SIZE];
    CHARSET_INFO *cs= &my_charset_latin1;
    cs->cset->snprintf(cs, buff, sizeof(buff),
                       ER_THD(this, sql_errno), type_str, val);
    /*
      Note: the format string can vary between ER_TRUNCATED_WRONG_VALUE
      and ER_WRONG_VALUE, but the code passed to push_warning() is
      always ER_TRUNCATED_WRONG_VALUE. This is intentional.
    */
    push_warning(this, level, ER_TRUNCATED_WRONG_VALUE, buff);
  }
public:
  void push_warning_truncated_wrong_value(Sql_condition::enum_warning_level level,
                                          const char *type_str, const char *val)
  {
    return push_warning_truncated_priv(level, ER_TRUNCATED_WRONG_VALUE,
                                       type_str, val);
  }
  void push_warning_wrong_value(Sql_condition::enum_warning_level level,
                                const char *type_str, const char *val)
  {
    return push_warning_truncated_priv(level, ER_WRONG_VALUE, type_str, val);
  }
  void push_warning_truncated_wrong_value(const char *type_str, const char *val)
  {
    return push_warning_truncated_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                                              type_str, val);
  }
  void push_warning_truncated_value_for_field(Sql_condition::enum_warning_level
                                              level, const char *type_str,
                                              const char *val,
                                              const char *db_name,
                                              const char *table_name,
                                              const char *name)
  {
    DBUG_ASSERT(name);
    char buff[MYSQL_ERRMSG_SIZE];
    CHARSET_INFO *cs= &my_charset_latin1;

    if (!db_name)
      db_name= "";
    if (!table_name)
      table_name= "";
    cs->cset->snprintf(cs, buff, sizeof(buff),
                       ER_THD(this, ER_TRUNCATED_WRONG_VALUE_FOR_FIELD),
                       type_str, val, db_name, table_name, name,
                       (ulong) get_stmt_da()->current_row_for_warning());
    push_warning(this, level, ER_TRUNCATED_WRONG_VALUE, buff);

  }
  void push_warning_wrong_or_truncated_value(Sql_condition::enum_warning_level level,
                                             bool totally_useless_value,
                                             const char *type_str,
                                             const char *val,
                                             const char *db_name,
                                             const char *table_name,
                                             const char *field_name)
  {
    if (field_name)
      push_warning_truncated_value_for_field(level, type_str, val,
                                             db_name, table_name, field_name);
    else if (totally_useless_value)
      push_warning_wrong_value(level, type_str, val);
    else
      push_warning_truncated_wrong_value(level, type_str, val);
  }

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
  void set_query(char *query_arg, size_t query_length_arg,
                 CHARSET_INFO *cs_arg)
  {
    set_query(CSET_STRING(query_arg, query_length_arg, cs_arg));
  }
  void set_query(char *query_arg, size_t query_length_arg) /*Mutex protected*/
  {
    set_query(CSET_STRING(query_arg, query_length_arg, charset()));
  }
  void set_query(const CSET_STRING &string_arg)
  {
    mysql_mutex_lock(&LOCK_thd_data);
    set_query_inner(string_arg);
    mysql_mutex_unlock(&LOCK_thd_data);

    PSI_CALL_set_thread_info(query(), query_length());
  }
  void reset_query()               /* Mutex protected */
  { set_query(CSET_STRING()); }
  void set_query_and_id(char *query_arg, uint32 query_length_arg,
                        CHARSET_INFO *cs, query_id_t new_query_id);
  void set_query_id(query_id_t new_query_id)
  {
    query_id= new_query_id;
#ifdef WITH_WSREP
    if (WSREP_NNULL(this))
    {
      set_wsrep_next_trx_id(query_id);
      WSREP_DEBUG("assigned new next trx id: %" PRIu64, wsrep_next_trx_id());
    }
#endif /* WITH_WSREP */
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
      mdl_context.release_transactional_locks(this);
  }
  int decide_logging_format(TABLE_LIST *tables);

  /*
   In Some cases when decide_logging_format is called it does not have
   all information to decide the logging format. So that cases we call
   decide_logging_format_2 at later stages in execution.

   One example would be binlog format for insert on duplicate key
   (IODKU) but column with unique key is not inserted.  We do not have
   inserted columns info when we call decide_logging_format so on
   later stage we call reconsider_logging_format_for_iodup()
  */
  void reconsider_logging_format_for_iodup(TABLE *table);

  enum need_invoker { INVOKER_NONE=0, INVOKER_USER, INVOKER_ROLE};
  void binlog_invoker(bool role) { m_binlog_invoker= role ? INVOKER_ROLE : INVOKER_USER; }
  enum need_invoker need_binlog_invoker() { return m_binlog_invoker; }
  void get_definer(LEX_USER *definer, bool role);
  void set_invoker(const LEX_CSTRING *user, const LEX_CSTRING *host)
  {
    invoker.user= *user;
    invoker.host= *host;
  }
  LEX_CSTRING get_invoker_user() { return invoker.user; }
  LEX_CSTRING get_invoker_host() { return invoker.host; }
  bool has_invoker() { return invoker.user.length > 0; }

  void print_aborted_warning(uint threshold, const char *reason)
  {
    if (global_system_variables.log_warnings > threshold)
    {
      Security_context *sctx= &main_security_ctx;
      sql_print_warning(ER_THD(this, ER_NEW_ABORTING_CONNECTION),
                        thread_id, (db.str ? db.str : "unconnected"),
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
  bool internal_transaction() { return transaction != &default_transaction; }
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
  AUTHID invoker;

public:
  Session_tracker session_tracker;
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

  const XID *get_xid() const
  {
#ifdef WITH_WSREP
    if (!wsrep_xid.is_null())
      return &wsrep_xid;
#endif /* WITH_WSREP */
    return (transaction->xid_state.is_explicit_XA() ?
            transaction->xid_state.get_xid() :
            &transaction->implicit_xid);
  }

/* Members related to temporary tables. */
public:
  /* Opened table states. */
  enum Temporary_table_state {
    TMP_TABLE_IN_USE,
    TMP_TABLE_NOT_IN_USE,
    TMP_TABLE_ANY
  };
  bool has_thd_temporary_tables();
  bool has_temporary_tables();

  TABLE *create_and_open_tmp_table(LEX_CUSTRING *frm,
                                   const char *path,
                                   const char *db,
                                   const char *table_name,
                                   bool open_internal_tables);

  TABLE *find_temporary_table(const char *db, const char *table_name,
                              Temporary_table_state state= TMP_TABLE_IN_USE);
  TABLE *find_temporary_table(const TABLE_LIST *tl,
                              Temporary_table_state state= TMP_TABLE_IN_USE);

  TMP_TABLE_SHARE *find_tmp_table_share_w_base_key(const char *key,
                                                   uint key_length);
  TMP_TABLE_SHARE *find_tmp_table_share(const char *db,
                                        const char *table_name);
  TMP_TABLE_SHARE *find_tmp_table_share(const TABLE_LIST *tl);
  TMP_TABLE_SHARE *find_tmp_table_share(const char *key, size_t key_length);

  bool open_temporary_table(TABLE_LIST *tl);
  bool open_temporary_tables(TABLE_LIST *tl);

  bool close_temporary_tables();
  bool rename_temporary_table(TABLE *table, const LEX_CSTRING *db,
                              const LEX_CSTRING *table_name);
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

  uint create_tmp_table_def_key(char *key, const char *db,
                                const char *table_name);
  TMP_TABLE_SHARE *create_temporary_table(LEX_CUSTRING *frm,
                                          const char *path, const char *db,
                                          const char *table_name);
  TABLE *find_temporary_table(const char *key, uint key_length,
                              Temporary_table_state state);
  TABLE *open_temporary_table(TMP_TABLE_SHARE *share, const char *alias);
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
  thd_async_state async_state;
#ifdef HAVE_REPLICATION
  /*
    If we do a purge of binary logs, log index info of the threads
    that are currently reading it needs to be adjusted. To do that
    each thread that is using LOG_INFO needs to adjust the pointer to it
  */
  LOG_INFO *current_linfo;
  Slave_info *slave_info;

  void set_current_linfo(LOG_INFO *linfo);
  void reset_current_linfo() { set_current_linfo(0); }

  int register_slave(uchar *packet, size_t packet_length);
  void unregister_slave();
  bool is_binlog_dump_thread();
#endif

  /*
    Indicates if this thread is suspended due to awaiting an ACK from a
    replica. True if suspended, false otherwise.

    Note that this variable is protected by Repl_semi_sync_master::LOCK_binlog
  */
  bool is_awaiting_semisync_ack;

  inline ulong wsrep_binlog_format() const
  {
    return WSREP_BINLOG_FORMAT(variables.binlog_format);
  }

#ifdef WITH_WSREP
  bool                      wsrep_applier; /* dedicated slave applier thread */
  bool                      wsrep_applier_closing; /* applier marked to close */
  bool                      wsrep_client_thread; /* to identify client threads*/
  query_id_t                wsrep_last_query_id;
  XID                       wsrep_xid;

  /** This flag denotes that record locking should be skipped during INSERT
  and gap locking during SELECT. Only used by the streaming replication thread
  that only modifies the wsrep_schema.SR table. */
  my_bool                   wsrep_skip_locking;

  mysql_cond_t              COND_wsrep_thd;

  // changed from wsrep_seqno_t to wsrep_trx_meta_t in wsrep API rev 75
  uint32                    wsrep_rand;
  rpl_group_info            *wsrep_rgi;
  bool                      wsrep_converted_lock_session;
  char                      wsrep_info[128]; /* string for dynamic proc info */
  ulong                     wsrep_retry_counter; // of autocommit
  bool                      wsrep_PA_safe;
  char*                     wsrep_retry_query;
  size_t                    wsrep_retry_query_len;
  enum enum_server_command  wsrep_retry_command;
  enum wsrep_consistency_check_mode 
                            wsrep_consistency_check;
  std::vector<wsrep::provider::status_variable> wsrep_status_vars;
  int                       wsrep_mysql_replicated;
  const char*               wsrep_TOI_pre_query; /* a query to apply before 
                                                    the actual TOI query */
  size_t                    wsrep_TOI_pre_query_len;
  wsrep_po_handle_t         wsrep_po_handle;
  size_t                    wsrep_po_cnt;
  void                      *wsrep_apply_format;
  uchar*                    wsrep_rbr_buf;
  wsrep_gtid_t              wsrep_sync_wait_gtid;
  uint64                    wsrep_last_written_gtid_seqno;
  uint64                    wsrep_current_gtid_seqno;
  ulong                     wsrep_affected_rows;
  bool                      wsrep_has_ignored_error;
  /* true if wsrep_on was ON in last wsrep_on_update */
  bool                      wsrep_was_on;

  /*
    When enabled, do not replicate/binlog updates from the current table that's
    being processed. At the moment, it is used to keep mysql.gtid_slave_pos
    table updates from being replicated to other nodes via galera replication.
  */
  bool                      wsrep_ignore_table;
  /* thread who has started kill for this THD protected by LOCK_thd_data*/
  my_thread_id              wsrep_aborter;

  /* true if BF abort is observed in do_command() right after reading
  client's packet, and if the client has sent PS execute command. */
  bool                      wsrep_delayed_BF_abort;

  /*
    Transaction id:
    * m_wsrep_next_trx_id is assigned on the first query after
      wsrep_next_trx_id() return WSREP_UNDEFINED_TRX_ID
    * Each storage engine must assign value of wsrep_next_trx_id()
      when the transaction starts.
    * Effective transaction id is returned via wsrep_trx_id()
   */
  /*
    Return effective transaction id
  */
  wsrep_trx_id_t wsrep_trx_id() const
  {
    return m_wsrep_client_state.transaction().id().get();
  }


  /*
    Set next trx id
   */
  void set_wsrep_next_trx_id(query_id_t query_id)
  {
    m_wsrep_next_trx_id = (wsrep_trx_id_t) query_id;
  }
  /*
    Return next trx id
   */
  wsrep_trx_id_t wsrep_next_trx_id() const
  {
    return m_wsrep_next_trx_id;
  }
  /*
    If node is async slave and have parallel execution, wait for prior commits.
   */
  bool wsrep_parallel_slave_wait_for_prior_commit();
private:
  wsrep_trx_id_t m_wsrep_next_trx_id; /* cast from query_id_t */
  /* wsrep-lib */
  Wsrep_mutex m_wsrep_mutex;
  Wsrep_condition_variable m_wsrep_cond;
  Wsrep_client_service m_wsrep_client_service;
  Wsrep_client_state m_wsrep_client_state;

public:
  Wsrep_client_state& wsrep_cs() { return m_wsrep_client_state; }
  const Wsrep_client_state& wsrep_cs() const { return m_wsrep_client_state; }
  const wsrep::transaction& wsrep_trx() const
  { return m_wsrep_client_state.transaction(); }
  const wsrep::streaming_context& wsrep_sr() const
  { return m_wsrep_client_state.transaction().streaming_context(); }
  /* Pointer to applier service for streaming THDs. This is needed to
     be able to delete applier service object in case of background
     rollback. */
  Wsrep_applier_service* wsrep_applier_service;
  /* wait_for_commit struct for binlog group commit */
  wait_for_commit wsrep_wfc;
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
  bool restore_set_statement_var()
  {
    return main_lex.restore_set_statement_var();
  }
  /* Copy relevant `stmt` transaction flags to `all` transaction. */
  void merge_unsafe_rollback_flags()
  {
    if (transaction->stmt.modified_non_trans_table)
      transaction->all.modified_non_trans_table= TRUE;
    transaction->all.m_unsafe_rollback_flags|=
      (transaction->stmt.m_unsafe_rollback_flags &
       (THD_TRANS::MODIFIED_NON_TRANS_TABLE |
        THD_TRANS::DID_WAIT | THD_TRANS::CREATED_TEMP_TABLE |
        THD_TRANS::DROPPED_TEMP_TABLE | THD_TRANS::DID_DDL |
        THD_TRANS::EXECUTED_TABLE_ADMIN_CMD));
  }

  uint get_net_wait_timeout()
  {
    if (in_active_multi_stmt_transaction())
    {
      if (transaction->all.is_trx_read_write())
      {
        if (variables.idle_write_transaction_timeout > 0)
          return variables.idle_write_transaction_timeout;
      }
      else
      {
        if (variables.idle_readonly_transaction_timeout > 0)
          return variables.idle_readonly_transaction_timeout;
      }

      if (variables.idle_transaction_timeout > 0)
        return variables.idle_transaction_timeout;
    }

    return variables.net_wait_timeout;
  }

  /**
    Switch to a sublex, to parse a substatement or an expression.
  */
  void set_local_lex(sp_lex_local *sublex)
  {
    DBUG_ASSERT(lex->sphead);
    lex= sublex;
    /* Reset part of parser state which needs this. */
    m_parser_state->m_yacc.reset_before_substatement();
  }

  /**
    Switch back from a sublex (currently pointed by this->lex) to the old lex.
    Sublex is merged to "oldlex" and this->lex is set to "oldlex".

    This method is called after parsing a substatement or an expression.
    set_local_lex() must be previously called.
    @param oldlex - The old lex which was active before set_local_lex().
    @returns      - false on success, true on error (failed to merge LEX's).

    See also sp_head::merge_lex().
  */
  bool restore_from_local_lex_to_old_lex(LEX *oldlex);

  Item *sp_fix_func_item(Item **it_addr);
  Item *sp_prepare_func_item(Item **it_addr, uint cols= 1);
  bool sp_eval_expr(Field *result_field, Item **expr_item_ptr);

  bool sql_parser(LEX *old_lex, LEX *lex,
                  char *str, uint str_len, bool stmt_prepare_mode);

  myf get_utf8_flag() const
  {
    return (variables.old_behavior & OLD_MODE_UTF8_IS_UTF8MB3 ?
            MY_UTF8_IS_UTF8MB3 : 0);
  }

  /**
    Save current lex to the output parameter and reset it to point to
    main_lex. This method is called from mysql_client_binlog_statement()
    to temporary

    @param[out] backup_lex  original value of current lex
  */

  void backup_and_reset_current_lex(LEX **backup_lex)
  {
    *backup_lex= lex;
    lex= &main_lex;
  }


  /**
    Restore current lex to its original value it had before calling the method
    backup_and_reset_current_lex().

    @param backup_lex  original value of current lex
  */

  void restore_current_lex(LEX *backup_lex)
  {
    lex= backup_lex;
  }
};


/*
  Start a new independent transaction for the THD.
  The old one is stored in this object and restored when calling
  restore_old_transaction() or when the object is freed
*/

class start_new_trans
{
  /* container for handler's private per-connection data */
  Ha_data old_ha_data[MAX_HA];
  struct THD::st_transactions *old_transaction, new_transaction;
  Open_tables_backup open_tables_state_backup;
  MDL_savepoint mdl_savepoint;
  PSI_transaction_locker *m_transaction_psi;
  THD *org_thd;
  uint in_sub_stmt;
  uint server_status;
  my_bool wsrep_on;

public:
  start_new_trans(THD *thd);
  ~start_new_trans()
  {
    destroy();
  }
  void destroy()
  {
    if (org_thd)                                // Safety
      restore_old_transaction();
    new_transaction.free();
  }
  void restore_old_transaction();
};

/** A short cut for thd->get_stmt_da()->set_ok_status(). */

inline void
my_ok(THD *thd, ulonglong affected_rows_arg= 0, ulonglong id= 0,
        const char *message= NULL)
{
  thd->set_row_count_func(affected_rows_arg);
  thd->set_affected_rows(affected_rows_arg);
  thd->get_stmt_da()->set_ok_status(affected_rows_arg, id, message);
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
  (A)->variables.option_bits|= OPTION_BIN_TMP_LOG_OFF;

#define reenable_binlog(A)                                                  \
  (A)->variables.option_bits= tmp_disable_binlog__save_options; }


inline date_conv_mode_t sql_mode_for_dates(THD *thd)
{
  static_assert((ulonglong(date_conv_mode_t::KNOWN_MODES) &
                 ulonglong(time_round_mode_t::KNOWN_MODES)) == 0,
                "date_conv_mode_t and time_round_mode_t must use different "
                "bit values");
  static_assert(MODE_NO_ZERO_DATE    == date_mode_t::NO_ZERO_DATE &&
                MODE_NO_ZERO_IN_DATE == date_mode_t::NO_ZERO_IN_DATE &&
                MODE_INVALID_DATES   == date_mode_t::INVALID_DATES,
                "sql_mode_t and date_mode_t values must be equal");
  return date_conv_mode_t(thd->variables.sql_mode &
          (MODE_NO_ZERO_DATE | MODE_NO_ZERO_IN_DATE | MODE_INVALID_DATES));
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
  const char *file_name;
  String *field_term,*enclosed,*line_term,*line_start,*escaped;
  bool opt_enclosed;
  bool dumpfile;
  ulong skip_lines;
  CHARSET_INFO *cs;
  sql_exchange(const char *name, bool dumpfile_flag,
               enum_filetype filetype_arg= FILETYPE_CSV);
  bool escaped_given(void) const;
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
  inline int send_data_with_check(List<Item> &items,
                              SELECT_LEX_UNIT *u,
                              ha_rows sent)
  {
    if (u->lim.check_offset(sent))
      return 0;

    if (u->thd->killed == ABORT_QUERY)
      return 0;

    return send_data(items);
  }
  /*
    send_data returns 0 on ok, 1 on error and -1 if data was ignored, for
    example for a duplicate row entry written to a temp table.
  */
  virtual int send_data(List<Item> &items)=0;
  virtual ~select_result_sink() {};
  void reset(THD *thd_arg) { thd= thd_arg; }
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
  ha_rows est_records;  /* estimated number of records in the result */
  select_result(THD *thd_arg): select_result_sink(thd_arg), est_records(0) {}
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
  virtual int prepare2(JOIN *join) { return 0; }
  /*
    Because of peculiarities of prepared statements protocol
    we need to know number of columns in the result set (if
    there is a result set) apart from sending columns metadata.
  */
  virtual uint field_count(List<Item> &fields) const
  { return fields.elements; }
  virtual bool send_result_set_metadata(List<Item> &list, uint flags)=0;
  virtual bool initialize_tables (JOIN *join) { return 0; }
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
  void reset(THD *thd_arg)
  {
    select_result_sink::reset(thd_arg);
    unit= NULL;
  }
#ifdef EMBEDDED_LIBRARY
  virtual void begin_dataset() {}
#else
  void begin_dataset() {}
#endif
  virtual void update_used_tables() {}

  /* this method is called just before the first row of the table can be read */
  virtual void prepare_to_read_rows() {}

  void remove_offset_limit()
  {
    unit->lim.remove_offset();
  }

  /*
    This returns
    - NULL if the class sends output row to the client
    - this if the output is set elsewhere (a file, @variable, or table).
  */
  virtual select_result_interceptor *result_interceptor()=0;

  /*
    This method is used to distinguish an normal SELECT from the cursor
    structure discovery for cursor%ROWTYPE routine variables.
    If this method returns "true", then a SELECT execution performs only
    all preparation stages, but does not fetch any rows.
  */
  virtual bool view_structure_only() const { return false; }
};


/*
  This is a select_result_sink which simply writes all data into a (temporary)
  table. Creation/deletion of the table is outside of the scope of the class
  
  It is aimed at capturing SHOW EXPLAIN output, so:
  - Unlike select_result class, we don't assume that the sent data is an 
    output of a SELECT_LEX_UNIT (and so we don't apply "LIMIT x,y" from the
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
  void reset(THD *thd_arg)
  {
    select_result::reset(thd_arg);
    suppress_my_ok= false;
  }
protected:
  bool suppress_my_ok;
};


class sp_cursor_statistics
{
protected:
  ulonglong m_fetch_count; // Number of FETCH commands since last OPEN
  ulonglong m_row_count;   // Number of successful FETCH since last OPEN
  bool m_found;            // If last FETCH fetched a row
public:
  sp_cursor_statistics()
   :m_fetch_count(0),
    m_row_count(0),
    m_found(false)
  { }
  bool found() const
  { return m_found; }

  ulonglong row_count() const
  { return m_row_count; }

  ulonglong fetch_count() const
  { return m_fetch_count; }
  void reset() { *this= sp_cursor_statistics(); }
};


/* A mediator between stored procedures and server side cursors */
class sp_lex_keeper;
class sp_cursor: public sp_cursor_statistics
{
private:
  /// An interceptor of cursor result set used to implement
  /// FETCH <cname> INTO <varlist>.
  class Select_fetch_into_spvars: public select_result_interceptor
  {
    List<sp_variable> *spvar_list;
    uint field_count;
    bool m_view_structure_only;
    bool send_data_to_variable_list(List<sp_variable> &vars, List<Item> &items);
  public:
    Select_fetch_into_spvars(THD *thd_arg, bool view_structure_only)
     :select_result_interceptor(thd_arg),
      m_view_structure_only(view_structure_only)
    {}
    void reset(THD *thd_arg)
    {
      select_result_interceptor::reset(thd_arg);
      spvar_list= NULL;
      field_count= 0;
    }
    uint get_field_count() { return field_count; }
    void set_spvar_list(List<sp_variable> *vars) { spvar_list= vars; }

    virtual bool send_eof() { return FALSE; }
    virtual int send_data(List<Item> &items);
    virtual int prepare(List<Item> &list, SELECT_LEX_UNIT *u);
    virtual bool view_structure_only() const { return m_view_structure_only; }
};

public:
  sp_cursor()
   :result(NULL, false),
    m_lex_keeper(NULL),
    server_side_cursor(NULL)
  { }
  sp_cursor(THD *thd_arg, sp_lex_keeper *lex_keeper, bool view_structure_only)
   :result(thd_arg, view_structure_only),
    m_lex_keeper(lex_keeper),
    server_side_cursor(NULL)
  {}

  virtual ~sp_cursor()
  { destroy(); }

  sp_lex_keeper *get_lex_keeper() { return m_lex_keeper; }

  int open(THD *thd);

  int close(THD *thd);

  my_bool is_open()
  { return MY_TEST(server_side_cursor); }

  int fetch(THD *, List<sp_variable> *vars, bool error_on_no_data);

  bool export_structure(THD *thd, Row_definition_list *list);

  void reset(THD *thd_arg, sp_lex_keeper *lex_keeper)
  {
    sp_cursor_statistics::reset();
    result.reset(thd_arg);
    m_lex_keeper= lex_keeper;
    server_side_cursor= NULL;
  }

private:
  Select_fetch_into_spvars result;
  sp_lex_keeper *m_lex_keeper;
  Server_side_cursor *server_side_cursor;
  void destroy();
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
  select_result *sel_result;
  TABLE_LIST *table_list;
  TABLE *table;
  List<Item> *fields;
  ulonglong autoinc_value_of_last_inserted_row; // autogenerated or not
  COPY_INFO info;
  bool insert_into_view;
  select_insert(THD *thd_arg, TABLE_LIST *table_list_par, TABLE *table_par,
                List<Item> *fields_par, List<Item> *update_fields,
                List<Item> *update_values, enum_duplicates duplic,
                bool ignore, select_result *sel_ret_list);
  ~select_insert();
  int prepare(List<Item> &list, SELECT_LEX_UNIT *u);
  virtual int prepare2(JOIN *join);
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
  DDL_LOG_STATE ddl_log_state_create, ddl_log_state_rm;

public:
  select_create(THD *thd_arg, TABLE_LIST *table_arg,
                Table_specification_st *create_info_par,
                Alter_info *alter_info_arg,
                List<Item> &select_fields,enum_duplicates duplic, bool ignore,
                TABLE_LIST *select_tables_arg):
    select_insert(thd_arg, table_arg, NULL, &select_fields, 0, 0, duplic,
                  ignore, NULL),
    create_table(table_arg),
    create_info(create_info_par),
    select_tables(select_tables_arg),
    alter_info(alter_info_arg),
    m_plock(NULL), exit_done(0),
    saved_tmp_table_share(0)
    {
      bzero(&ddl_log_state_create, sizeof(ddl_log_state_create));
      bzero(&ddl_log_state_rm, sizeof(ddl_log_state_rm));
    }
  int prepare(List<Item> &list, SELECT_LEX_UNIT *u);

  void store_values(List<Item> &values);
  bool send_eof();
  virtual void abort_result_set();
  virtual bool can_rollback_data() { return 1; }

  // Needed for access from local class MY_HOOKS in prepare(), since thd is proteted.
  const THD *get_thd(void) { return thd; }
  const HA_CREATE_INFO *get_create_info() { return create_info; };
  int prepare2(JOIN *join) { return 0; }

private:
  TABLE *create_table_from_items(THD *thd,
                                  List<Item> *items,
                                  MYSQL_LOCK **lock,
                                  TABLEOP_HOOKS *hooks);
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
  const char *tmp_name;
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
  {
    init();
  }
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


class select_unit :public select_result_interceptor
{
protected:
  uint curr_step, prev_step, curr_sel;
  enum sub_select_type step;
public:
  TMP_TABLE_PARAM tmp_table_param;
  /* Number of additional (hidden) field of the used temporary table */
  int addon_cnt;
  int write_err; /* Error code from the last send_data->ha_write_row call. */
  TABLE *table;

  select_unit(THD *thd_arg):
    select_result_interceptor(thd_arg), addon_cnt(0), table(0)
  {
    init();
    tmp_table_param.init();
  }
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
  int write_record();
  int update_counter(Field *counter, longlong value);
  int delete_record();
  bool send_eof();
  virtual bool flush();
  void cleanup();
  virtual bool create_result_table(THD *thd, List<Item> *column_types,
                                   bool is_distinct, ulonglong options,
                                   const LEX_CSTRING *alias,
                                   bool bit_fields_as_long,
                                   bool create_table,
                                   bool keep_row_order,
                                   uint hidden);
  TMP_TABLE_PARAM *get_tmp_table_param() { return &tmp_table_param; }
  void init()
  {
    curr_step= prev_step= 0;
    curr_sel= UINT_MAX;
    step= UNION_TYPE;
    write_err= 0;
  }
  virtual void change_select();
  virtual bool force_enable_index_if_needed() { return false; }
};


/**
  @class select_unit_ext

  The class used when processing rows produced by operands of query expressions
  containing INTERSECT ALL and/or EXCEPT all operations. One or two extra fields
  of the temporary to store the rows of the partial and final result can be employed.
  Both of them contain counters. The second additional field is used only when
  the processed query expression contains INTERSECT ALL.

  Consider how these extra fields are used.

  Let
    table t1 (f char(8))
    table t2 (f char(8))
    table t3 (f char(8))
  contain the following sets:
    ("b"),("a"),("d"),("c"),("b"),("a"),("c"),("a")
    ("c"),("b"),("c"),("c"),("a"),("b"),("g")
    ("c"),("a"),("b"),("d"),("b"),("e")

  - Let's demonstrate how the the set operation INTERSECT ALL is proceesed
    for the query
              SELECT f FROM t1 INTERSECT ALL SELECT f FROM t2

    When send_data() is called for the rows of the first operand we put
    the processed record into the temporary table if there was no such record
    setting dup_cnt field to 1 and add_cnt field to 0 and increment the
    counter in the dup_cnt field by one otherwise. We get

      |add_cnt|dup_cnt| f |
      |0      |2      |b  |
      |0      |3      |a  |
      |0      |1      |d  |
      |0      |2      |c  |

    The call of send_eof() for the first operand swaps the values stored in
    dup_cnt and add_cnt. After this, we'll see the following rows in the
    temporary table

      |add_cnt|dup_cnt| f |
      |2      |0      |b  |
      |3      |0      |a  |
      |1      |0      |d  |
      |2      |0      |c  |

    When send_data() is called for the rows of the second operand we increment
    the counter in dup_cnt if the processed row is found in the table and do
    nothing otherwise. As a result we get

      |add_cnt|dup_cnt| f |
      |2      |2      |b  |
      |3      |1      |a  |
      |1      |0      |d  |
      |2      |3      |c  |

    At the call of send_eof() for the second operand first we disable index.
    Then for each record, the minimum of counters from dup_cnt and add_cnt m is
    taken. If m == 0 then the record is deleted. Otherwise record is replaced
    with m copies of it. Yet the counter in this copies are set to 1 for
    dup_cnt and to 0 for add_cnt

      |add_cnt|dup_cnt| f |
      |0      |1      |b  |
      |0      |1      |b  |
      |0      |1      |a  |
      |0      |1      |c  |
      |0      |1      |c  |

  - Let's demonstrate how the the set operation EXCEPT ALL is proceesed
    for the query
              SELECT f FROM t1 EXCEPT ALL SELECT f FROM t3

    Only one additional counter field dup_cnt is used for EXCEPT ALL.
    After the first operand has been processed we have in the temporary table

      |dup_cnt| f |
      |2      |b  |
      |3      |a  |
      |1      |d  |
      |2      |c  |

    When send_data() is called for the rows of the second operand we decrement
    the counter in dup_cnt if the processed row is found in the table and do
    nothing otherwise. If the counter becomes 0 we delete the record

      |dup_cnt| f |
      |2      |a  |
      |1      |c  |

    Finally at the call of send_eof() for the second operand we disable index
    unfold rows adding duplicates

      |dup_cnt| f |
      |1      |a  |
      |1      |a  |
      |1      |c  |
 */

class select_unit_ext :public select_unit
{
public:
  select_unit_ext(THD *thd_arg):
    select_unit(thd_arg), increment(0), is_index_enabled(TRUE), 
    curr_op_type(UNSPECIFIED)
  {
  };
  int send_data(List<Item> &items);
  void change_select();
  int unfold_record(ha_rows cnt);
  bool send_eof();
  bool force_enable_index_if_needed()
  {
    is_index_enabled= true;
    return true;
  }
  bool disable_index_if_needed(SELECT_LEX *curr_sl);
  
  /* 
    How to change increment/decrement the counter in duplicate_cnt field 
    when processing a record produced by the current operand in send_data().
    The value can be 1 or -1
  */
  int increment;
  /* TRUE <=> the index of the result temporary table is enabled */
  bool is_index_enabled;
  /* The type of the set operation currently executed */
  enum set_op_type curr_op_type;
  /* 
    Points to the extra field of the temporary table where
    duplicate counters are stored
  */ 
  Field *duplicate_cnt;
  /* 
    Points to the extra field of the temporary table where additional
    counters used only for INTERSECT ALL operations are stored
  */
  Field *additional_cnt;
};

class select_union_recursive :public select_unit
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
    select_unit(thd_arg),
    incr_table(0), first_rec_table_to_update(0), cleanup_count(0)
  { incr_table_param.init(); };

  int send_data(List<Item> &items);
  bool create_result_table(THD *thd, List<Item> *column_types,
                           bool is_distinct, ulonglong options,
                           const LEX_CSTRING *alias,
                           bool bit_fields_as_long,
                           bool create_table,
                           bool keep_row_order,
                           uint hidden);
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

class select_union_direct :public select_unit
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
  select_unit(thd_arg), result(result_arg),
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
  bool initialize_tables (JOIN *join);
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
  void remove_offset_limit()
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

class select_materialize_with_stats : public select_unit
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
  select_materialize_with_stats(THD *thd_arg): select_unit(thd_arg)
  { tmp_table_param.init(); }
  bool create_result_table(THD *thd, List<Item> *column_types,
                           bool is_distinct, ulonglong options,
                           const LEX_CSTRING *alias,
                           bool bit_fields_as_long,
                           bool create_table,
                           bool keep_row_order,
                           uint hidden);
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
  bool cmp_time();
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
class POSITION;

class SJ_MATERIALIZATION_INFO : public Sql_alloc
{
public:
  /* Optimal join sub-order */
  POSITION *positions;

  uint tables; /* Number of tables in the sj-nest */

  /* Number of rows in the materialized table, before the de-duplication */
  double rows_with_duplicates;

  /* Expected #rows in the materialized table, after de-duplication */
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
  /*
    If using mem-comparable fixed-size keys:
    length of the mem-comparable image of the field, in bytes.

    If using packed keys: still the same? Not clear what is the use of it.
  */
  uint length;

  /*
    For most datatypes, this is 0.
    The exception are the VARBINARY columns.
    For those columns, the comparison actually compares

      (value_prefix(N), suffix=length(value))

    Here value_prefix is either the whole value or its prefix if it was too
    long, and the suffix is the length of the original value.
    (this way, for values X and Y:  if X=prefix(Y) then X compares as less
    than Y
  */
  uint suffix_length;

  /*
    If using packed keys, number of bytes that are used to store the length
    of the packed key.

  */
  uint length_bytes;

  /* Max. length of the original value, in bytes */
  uint original_length;
  enum Type { FIXED_SIZE, VARIABLE_SIZE } type;
  /*
    TRUE  : if the item or field is NULLABLE
    FALSE : otherwise
  */
  bool maybe_null;
  CHARSET_INFO *cs;
  uint pack_sort_string(uchar *to, const Binary_string *str,
                        CHARSET_INFO *cs) const;
  int compare_packed_fixed_size_vals(uchar *a, size_t *a_len,
                                     uchar *b, size_t *b_len);
  int compare_packed_varstrings(uchar *a, size_t *a_len,
                                uchar *b, size_t *b_len);
  bool check_if_packing_possible(THD *thd) const;
  bool is_variable_sized() { return type == VARIABLE_SIZE; }
  void set_length_and_original_length(THD *thd, uint length_arg);
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
  LEX_CSTRING db;
  LEX_CSTRING table;
  SELECT_LEX_UNIT *sel;
  inline Table_ident(THD *thd, const LEX_CSTRING *db_arg,
                     const LEX_CSTRING *table_arg,
		     bool force)
    :table(*table_arg), sel((SELECT_LEX_UNIT *)0)
  {
    if (!force && (thd->client_capabilities & CLIENT_NO_SCHEMA))
      db= null_clex_str;
    else
      db= *db_arg;
  }
  inline Table_ident(const LEX_CSTRING *table_arg)
    :table(*table_arg), sel((SELECT_LEX_UNIT *)0)
  {
    db= null_clex_str;
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
  inline void change_db(LEX_CSTRING *db_name)
  {
    db= *db_name;
  }
  bool resolve_table_rowtype_ref(THD *thd, Row_definition_list &defs);
  bool append_to(THD *thd, String *to) const;
};


class Qualified_column_ident: public Table_ident
{
public:
  LEX_CSTRING m_column;
public:
  Qualified_column_ident(const LEX_CSTRING *column)
    :Table_ident(&null_clex_str),
    m_column(*column)
  { }
  Qualified_column_ident(const LEX_CSTRING *table, const LEX_CSTRING *column)
   :Table_ident(table),
    m_column(*column)
  { }
  Qualified_column_ident(THD *thd,
                         const LEX_CSTRING *db,
                         const LEX_CSTRING *table,
                         const LEX_CSTRING *column)
   :Table_ident(thd, db, table, false),
    m_column(*column)
  { }
  bool resolve_type_ref(THD *thd, Column_definition *def);
  bool append_to(THD *thd, String *to) const;
};


// this is needed for user_vars hash
class user_var_entry
{
  CHARSET_INFO *m_charset;
 public:
  user_var_entry() {}                         /* Remove gcc warning */
  LEX_CSTRING name;
  char *value;
  size_t length;
  query_id_t update_query_id, used_query_id;
  Item_result type;
  bool unsigned_flag;

  double val_real(bool *null_value);
  longlong val_int(bool *null_value) const;
  String *val_str(bool *null_value, String *str, uint decimals) const;
  my_decimal *val_decimal(bool *null_value, my_decimal *result);
  CHARSET_INFO *charset() const { return m_charset; }
  void set_charset(CHARSET_INFO *cs) { m_charset= cs; }
};

user_var_entry *get_variable(HASH *hash, LEX_CSTRING *name,
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
  // Methods used by ColumnStore
  uint get_num_of_tables() const { return num_of_tables; }
  TABLE_LIST* get_tables() const { return delete_tables; }
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
  List<TABLE_LIST> *leaves;     /* list of leaves of join table tree */
  List<TABLE_LIST> updated_leaves;  /* list of of updated leaves */
  TABLE_LIST *update_tables;
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

  // For System Versioning (may need to insert new fields to a table).
  ha_rows updated_sys_ver;

  bool has_vers_fields;

public:
  multi_update(THD *thd_arg, TABLE_LIST *ut, List<TABLE_LIST> *leaves_list,
	       List<Item> *fields, List<Item> *values,
	       enum_duplicates handle_duplicates, bool ignore);
  ~multi_update();
  bool init(THD *thd);
  int prepare(List<Item> &list, SELECT_LEX_UNIT *u);
  int send_data(List<Item> &items);
  bool initialize_tables (JOIN *join);
  int prepare2(JOIN *join);
  int  do_updates();
  bool send_eof();
  inline ha_rows num_found() const { return found; }
  inline ha_rows num_updated() const { return updated; }
  virtual void abort_result_set();
  void update_used_tables();
  void prepare_to_read_rows();
};

class my_var_sp;
class my_var : public Sql_alloc  {
public:
  const LEX_CSTRING name;
  enum type { SESSION_VAR, LOCAL_VAR, PARAM_VAR };
  type scope;
  my_var(const LEX_CSTRING *j, enum type s) : name(*j), scope(s) { }
  virtual ~my_var() {}
  virtual bool set(THD *thd, Item *val) = 0;
  virtual my_var_sp *get_my_var_sp() { return NULL; }
};

class my_var_sp: public my_var {
  const Sp_rcontext_handler *m_rcontext_handler;
  const Type_handler *m_type_handler;
public:
  uint offset;
  /*
    Routine to which this Item_splocal belongs. Used for checking if correct
    runtime context is used for variable handling.
  */
  sp_head *sp;
  my_var_sp(const Sp_rcontext_handler *rcontext_handler,
            const LEX_CSTRING *j, uint o, const Type_handler *type_handler,
            sp_head *s)
    : my_var(j, LOCAL_VAR),
      m_rcontext_handler(rcontext_handler),
      m_type_handler(type_handler), offset(o), sp(s) { }
  ~my_var_sp() { }
  bool set(THD *thd, Item *val);
  my_var_sp *get_my_var_sp() { return this; }
  const Type_handler *type_handler() const
  { return m_type_handler; }
  sp_rcontext *get_rcontext(sp_rcontext *local_ctx) const;
};

/*
  This class handles fields of a ROW SP variable when it's used as a OUT
  parameter in a stored procedure.
*/
class my_var_sp_row_field: public my_var_sp
{
  uint m_field_offset;
public:
  my_var_sp_row_field(const Sp_rcontext_handler *rcontext_handler,
                      const LEX_CSTRING *varname, const LEX_CSTRING *fieldname,
                      uint var_idx, uint field_idx, sp_head *s)
   :my_var_sp(rcontext_handler, varname, var_idx,
              &type_handler_double/*Not really used*/, s),
    m_field_offset(field_idx)
  { }
  bool set(THD *thd, Item *val);
};

class my_var_user: public my_var {
public:
  my_var_user(const LEX_CSTRING *j)
    : my_var(j, SESSION_VAR) { }
  ~my_var_user() { }
  bool set(THD *thd, Item *val);
};

class select_dumpvar :public select_result_interceptor {
  ha_rows row_count;
  my_var_sp *m_var_sp_row; // Not NULL if SELECT INTO row_type_sp_variable
  bool send_data_to_var_list(List<Item> &items);
public:
  List<my_var> var_list;
  select_dumpvar(THD *thd_arg)
   :select_result_interceptor(thd_arg), row_count(0), m_var_sp_row(NULL)
  { var_list.empty(); }
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
#define CF_IMPLICIT_COMMIT_BEGIN   (1U << 6)
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
#define CF_AUTO_COMMIT_TRANS  (CF_IMPLICIT_COMMIT_BEGIN | CF_IMPLICIT_COMMIT_END)

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
/**
  If command creates or drops a table
*/
#define CF_SCHEMA_CHANGE (1U << 22)
/**
  If command creates or drops a database
*/
#define CF_DB_CHANGE (1U << 23)

#ifdef WITH_WSREP
/**
  DDL statement that may be subject to error filtering.
*/
#define CF_WSREP_MAY_IGNORE_ERRORS (1U << 24)
#endif /* WITH_WSREP */


/* Bits in server_command_flags */

/**
  Statement that deletes existing rows (DELETE, DELETE_MULTI)
*/
#define CF_DELETES_DATA (1U << 24)

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


/* Inline functions */

inline bool add_item_to_list(THD *thd, Item *item)
{
  bool res= thd->lex->current_select->add_item_to_list(thd, item);
  return res;
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
  TABLE_IO_WAIT(tracker, PSI_TABLE_WRITE_ROW, MAX_KEY, error,
          { error= write_row(buf); })
  MYSQL_INSERT_ROW_DONE(error);
  return error;
}

inline int handler::ha_delete_tmp_row(uchar *buf)
{
  int error;
  MYSQL_DELETE_ROW_START(table_share->db.str, table_share->table_name.str);
  increment_statistics(&SSV::ha_tmp_delete_count);
  TABLE_IO_WAIT(tracker, PSI_TABLE_DELETE_ROW, MAX_KEY, error,
                { error= delete_row(buf); })
  MYSQL_DELETE_ROW_DONE(error);
  return error;
}

inline int handler::ha_update_tmp_row(const uchar *old_data, uchar *new_data)
{
  int error;
  MYSQL_UPDATE_ROW_START(table_share->db.str, table_share->table_name.str);
  increment_statistics(&SSV::ha_tmp_update_count);
  TABLE_IO_WAIT(tracker, PSI_TABLE_UPDATE_ROW, active_index, error,
          { error= update_row(old_data, new_data);})
  MYSQL_UPDATE_ROW_DONE(error);
  return error;
}

inline bool handler::has_long_unique()
{
  return table->s->long_unique_table;
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

inline bool binlog_should_compress(size_t len)
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


class Sql_mode_instant_set: public Sql_mode_save
{
public:
  Sql_mode_instant_set(THD *thd, sql_mode_t temporary_value)
   :Sql_mode_save(thd)
  {
    thd->variables.sql_mode= temporary_value;
  }
};


class Sql_mode_instant_remove: public Sql_mode_save
{
public:
  Sql_mode_instant_remove(THD *thd, sql_mode_t temporary_remove_flags)
   :Sql_mode_save(thd)
  {
    thd->variables.sql_mode&= ~temporary_remove_flags;
  }
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


/**
  This class resembles the SQL Standard schema qualified object name:
  <schema qualified name> ::= [ <schema name> <period> ] <qualified identifier>
*/
class Database_qualified_name
{
public:
  LEX_CSTRING m_db;
  LEX_CSTRING m_name;
  Database_qualified_name(const LEX_CSTRING *db, const LEX_CSTRING *name)
   :m_db(*db), m_name(*name)
  { }
  Database_qualified_name(const LEX_CSTRING &db, const LEX_CSTRING &name)
   :m_db(db), m_name(name)
  { }
  Database_qualified_name(const char *db, size_t db_length,
                          const char *name, size_t name_length)
  {
    m_db.str= db;
    m_db.length= db_length;
    m_name.str= name;
    m_name.length= name_length;
  }

  bool eq(const Database_qualified_name *other) const
  {
    CHARSET_INFO *cs= lower_case_table_names ?
                      &my_charset_utf8mb3_general_ci :
                      &my_charset_utf8mb3_bin;
    return
      m_db.length == other->m_db.length &&
      m_name.length == other->m_name.length &&
      !cs->strnncoll(m_db.str, m_db.length,
                     other->m_db.str, other->m_db.length) &&
      !cs->strnncoll(m_name.str, m_name.length,
                     other->m_name.str, other->m_name.length);
  }
  void copy(MEM_ROOT *mem_root, const LEX_CSTRING &db,
                                const LEX_CSTRING &name);

  static Database_qualified_name split(const LEX_CSTRING &txt)
  {
    DBUG_ASSERT(txt.str[txt.length] == '\0'); // Expect 0-terminated input
    const char *dot= strchr(txt.str, '.');
    if (!dot)
      return Database_qualified_name(NULL, 0, txt.str, txt.length);
    size_t dblen= dot - txt.str;
    Lex_cstring db(txt.str, dblen);
    Lex_cstring name(txt.str + dblen + 1, txt.length - dblen - 1);
    return Database_qualified_name(db, name);
  }

  // Export db and name as a qualified name string: 'db.name'
  size_t make_qname(char *dst, size_t dstlen) const
  {
    return my_snprintf(dst, dstlen, "%.*s.%.*s",
                       (int) m_db.length, m_db.str,
                       (int) m_name.length, m_name.str);
  }
  // Export db and name as a qualified name string, allocate on mem_root.
  bool make_qname(MEM_ROOT *mem_root, LEX_CSTRING *dst) const
  {
    const uint dot= !!m_db.length;
    char *tmp;
    /* format: [database + dot] + name + '\0' */
    dst->length= m_db.length + dot + m_name.length;
    if (unlikely(!(dst->str= tmp= (char*) alloc_root(mem_root,
                                                     dst->length + 1))))
      return true;
    sprintf(tmp, "%.*s%.*s%.*s",
            (int) m_db.length, (m_db.length ? m_db.str : ""),
            dot, ".",
            (int) m_name.length, m_name.str);
    DBUG_SLOW_ASSERT(ok_for_lower_case_names(m_db.str));
    return false;
  }

  bool make_package_routine_name(MEM_ROOT *mem_root,
                                 const LEX_CSTRING &package,
                                 const LEX_CSTRING &routine)
  {
    char *tmp;
    size_t length= package.length + 1 + routine.length + 1;
    if (unlikely(!(tmp= (char *) alloc_root(mem_root, length))))
      return true;
    m_name.length= my_snprintf(tmp, length, "%.*s.%.*s",
                               (int) package.length, package.str,
                               (int) routine.length, routine.str);
    m_name.str= tmp;
    return false;
  }

  bool make_package_routine_name(MEM_ROOT *mem_root,
                                 const LEX_CSTRING &db,
                                 const LEX_CSTRING &package,
                                 const LEX_CSTRING &routine)
  {
    if (unlikely(make_package_routine_name(mem_root, package, routine)))
      return true;
    if (unlikely(!(m_db.str= strmake_root(mem_root, db.str, db.length))))
      return true;
    m_db.length= db.length;
    return false;
  }
};


class ErrConvDQName: public ErrConv
{
  const Database_qualified_name *m_name;
public:
  ErrConvDQName(const Database_qualified_name *name)
   :m_name(name)
  { }
  LEX_CSTRING lex_cstring() const override
  {
    size_t length= m_name->make_qname(err_buffer, sizeof(err_buffer));
    return {err_buffer, length};
  }
};

class Type_holder: public Sql_alloc,
                   public Item_args,
                   public Type_handler_hybrid_field_type,
                   public Type_all_attributes
{
  const TYPELIB *m_typelib;
  bool m_maybe_null;
public:
  Type_holder()
   :m_typelib(NULL),
    m_maybe_null(false)
  { }

  void set_type_maybe_null(bool maybe_null_arg) { m_maybe_null= maybe_null_arg; }
  bool get_maybe_null() const { return m_maybe_null; }

  decimal_digits_t decimal_precision() const
  {
    /*
      Type_holder is not used directly to create fields, so
      its virtual decimal_precision() is never called.
      We should eventually extend create_result_table() to accept
      an array of Type_holders directly, without having to allocate
      Item_type_holder's and put them into List<Item>.
    */
    DBUG_ASSERT(0);
    return 0;
  }
  void set_typelib(const TYPELIB *typelib)
  {
    m_typelib= typelib;
  }
  const TYPELIB *get_typelib() const
  {
    return m_typelib;
  }

  bool aggregate_attributes(THD *thd)
  {
    static LEX_CSTRING union_name= { STRING_WITH_LEN("UNION") };
    for (uint i= 0; i < arg_count; i++)
      m_maybe_null|= args[i]->maybe_null();
    return
       type_handler()->Item_hybrid_func_fix_attributes(thd,
                                                       union_name, this, this,
                                                       args, arg_count);
  }
};


/*
  A helper class to set THD flags to emit warnings/errors in case of
  overflow/type errors during assigning values into the SP variable fields.
  Saves original flags values in constructor.
  Restores original flags in destructor.
*/
class Sp_eval_expr_state
{
  THD *m_thd;
  enum_check_fields m_count_cuted_fields;
  bool m_abort_on_warning;
  bool m_stmt_modified_non_trans_table;
  void start()
  {
    m_thd->count_cuted_fields= CHECK_FIELD_ERROR_FOR_NULL;
    m_thd->abort_on_warning= m_thd->is_strict_mode();
    m_thd->transaction->stmt.modified_non_trans_table= false;
  }
  void stop()
  {
    m_thd->count_cuted_fields= m_count_cuted_fields;
    m_thd->abort_on_warning= m_abort_on_warning;
    m_thd->transaction->stmt.modified_non_trans_table=
      m_stmt_modified_non_trans_table;
  }
public:
  Sp_eval_expr_state(THD *thd)
   :m_thd(thd),
    m_count_cuted_fields(thd->count_cuted_fields),
    m_abort_on_warning(thd->abort_on_warning),
    m_stmt_modified_non_trans_table(thd->transaction->stmt.
                                    modified_non_trans_table)
  {
    start();
  }
  ~Sp_eval_expr_state()
  {
    stop();
  }
};


#ifndef DBUG_OFF
void dbug_serve_apcs(THD *thd, int n_calls);
#endif 

class StatementBinlog
{
  const enum_binlog_format saved_binlog_format;
  THD *const thd;

public:
  StatementBinlog(THD *thd, bool need_stmt) :
    saved_binlog_format(thd->get_current_stmt_binlog_format()),
    thd(thd)
  {
    if (need_stmt && saved_binlog_format != BINLOG_FORMAT_STMT)
    {
      thd->set_current_stmt_binlog_format_stmt();
    }
  }
  ~StatementBinlog()
  {
    thd->set_current_stmt_binlog_format(saved_binlog_format);
  }
};


/** THD registry */
class THD_list: public THD_list_iterator
{
public:
  /**
    Constructor replacement.

    Unfortunately we can't use fair constructor to initialize mutex
    for two reasons: PFS and embedded. The former can probably be fixed,
    the latter can probably be dropped.
  */
  void init()
  {
    mysql_rwlock_init(key_rwlock_THD_list, &lock);
  }

  /** Destructor replacement. */
  void destroy()
  {
    mysql_rwlock_destroy(&lock);
  }

  /**
    Inserts thread to registry.

    @param thd         thread

    Thread becomes accessible via server_threads.
  */
  void insert(THD *thd)
  {
    mysql_rwlock_wrlock(&lock);
    threads.append(thd);
    mysql_rwlock_unlock(&lock);
  }

  /**
    Removes thread from registry.

    @param thd         thread

    Thread becomes not accessible via server_threads.
  */
  void erase(THD *thd)
  {
    thd->assert_linked();
    mysql_rwlock_wrlock(&lock);
    thd->unlink();
    mysql_rwlock_unlock(&lock);
  }
};

extern THD_list server_threads;

void setup_tmp_table_column_bitmaps(TABLE *table, uchar *bitmaps,
                                    uint field_count);

/*
  RAII utility class to ease binlogging with temporary setting
  THD etc context and restoring the original one upon logger execution.
*/
class Write_log_with_flags
{
  THD*   m_thd;
#ifdef WITH_WSREP
  bool wsrep_to_isolation;
#endif

public:
~Write_log_with_flags()
  {
    m_thd->set_binlog_flags_for_alter(0);
    m_thd->set_binlog_start_alter_seq_no(0);
#ifdef WITH_WSREP
    if (wsrep_to_isolation)
      wsrep_to_isolation_end(m_thd);
#endif
  }

  Write_log_with_flags(THD *thd, uchar flags,
                       bool do_wsrep_iso __attribute__((unused))= false) :
    m_thd(thd)
  {
    m_thd->set_binlog_flags_for_alter(flags);
#ifdef WITH_WSREP
    wsrep_to_isolation= do_wsrep_iso && WSREP(m_thd);
#endif
  }
};

#endif /* MYSQL_SERVER */
#endif /* SQL_CLASS_INCLUDED */
