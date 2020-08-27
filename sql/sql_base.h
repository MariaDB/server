/* Copyright (c) 2010, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2011, 2018, MariaDB


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

#ifndef SQL_BASE_INCLUDED
#define SQL_BASE_INCLUDED

#include "sql_class.h"                          /* enum_column_usage */
#include "sql_trigger.h"                        /* trg_event_type */
#include "mysqld.h"                             /* key_map */
#include "table_cache.h"

class Item_ident;
struct Name_resolution_context;
class Open_table_context;
class Open_tables_state;
class Prelocking_strategy;
struct TABLE_LIST;
class THD;
struct handlerton;
struct TABLE;

typedef class st_select_lex SELECT_LEX;

typedef struct st_lock_param_type ALTER_PARTITION_PARAM_TYPE;

/*
  This enumeration type is used only by the function find_item_in_list
  to return the info on how an item has been resolved against a list
  of possibly aliased items.
  The item can be resolved:
   - against an alias name of the list's element (RESOLVED_AGAINST_ALIAS)
   - against non-aliased field name of the list  (RESOLVED_WITH_NO_ALIAS)
   - against an aliased field name of the list   (RESOLVED_BEHIND_ALIAS)
   - ignoring the alias name in cases when SQL requires to ignore aliases
     (e.g. when the resolved field reference contains a table name or
     when the resolved item is an expression)   (RESOLVED_IGNORING_ALIAS)
*/
enum enum_resolution_type {
  NOT_RESOLVED=0,
  RESOLVED_IGNORING_ALIAS,
  RESOLVED_BEHIND_ALIAS,
  RESOLVED_WITH_NO_ALIAS,
  RESOLVED_AGAINST_ALIAS
};

/* Argument to flush_tables() of what to flush */
enum flush_tables_type {
  FLUSH_ALL,
  FLUSH_NON_TRANS_TABLES,
  FLUSH_SYS_TABLES
};

enum find_item_error_report_type {REPORT_ALL_ERRORS, REPORT_EXCEPT_NOT_FOUND,
				  IGNORE_ERRORS, REPORT_EXCEPT_NON_UNIQUE,
                                  IGNORE_EXCEPT_NON_UNIQUE};

/* Flag bits for unique_table() */
#define CHECK_DUP_ALLOW_DIFFERENT_ALIAS 1
#define CHECK_DUP_FOR_CREATE 2
#define CHECK_DUP_SKIP_TEMP_TABLE 4

uint get_table_def_key(const TABLE_LIST *table_list, const char **key);
TABLE *open_ltable(THD *thd, TABLE_LIST *table_list, thr_lock_type update,
                   uint lock_flags);

/* mysql_lock_tables() and open_table() flags bits */
#define MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK      0x0001
#define MYSQL_OPEN_IGNORE_FLUSH                 0x0002
/* MYSQL_OPEN_TEMPORARY_ONLY (0x0004) is not used anymore. */
#define MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY      0x0008
#define MYSQL_LOCK_LOG_TABLE                    0x0010
/**
  Do not try to acquire a metadata lock on the table: we
  already have one.
*/
#define MYSQL_OPEN_HAS_MDL_LOCK                 0x0020
/**
  If in locked tables mode, ignore the locked tables and get
  a new instance of the table.
*/
#define MYSQL_OPEN_GET_NEW_TABLE                0x0040
/* 0x0080 used to be MYSQL_OPEN_SKIP_TEMPORARY */
/** Fail instead of waiting when conficting metadata lock is discovered. */
#define MYSQL_OPEN_FAIL_ON_MDL_CONFLICT         0x0100
/** Open tables using MDL_SHARED lock instead of one specified in parser. */
#define MYSQL_OPEN_FORCE_SHARED_MDL             0x0200
/**
  Open tables using MDL_SHARED_HIGH_PRIO lock instead of one specified
  in parser.
*/
#define MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL   0x0400
/**
  When opening or locking the table, use the maximum timeout
  (LONG_TIMEOUT = 1 year) rather than the user-supplied timeout value.
*/
#define MYSQL_LOCK_IGNORE_TIMEOUT               0x0800
/**
  When acquiring "strong" (SNW, SNRW, X) metadata locks on tables to
  be open do not acquire global and schema-scope IX locks.
*/
#define MYSQL_OPEN_SKIP_SCOPED_MDL_LOCK         0x1000
#define MYSQL_LOCK_NOT_TEMPORARY		0x2000
#define MYSQL_LOCK_USE_MALLOC                   0x4000
/**
  Only check THD::killed if waits happen (e.g. wait on MDL, wait on
  table flush, wait on thr_lock.c locks) while opening and locking table.
*/
#define MYSQL_OPEN_IGNORE_KILLED                0x8000
/**
   Don't try to  auto-repair table
*/
#define MYSQL_OPEN_IGNORE_REPAIR                0x10000

/**
   Don't call decide_logging_format. Used for statistic tables etc
*/
#define MYSQL_OPEN_IGNORE_LOGGING_FORMAT        0x20000

/** Please refer to the internals manual. */
#define MYSQL_OPEN_REOPEN  (MYSQL_OPEN_IGNORE_FLUSH |\
                            MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK |\
                            MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY |\
                            MYSQL_LOCK_IGNORE_TIMEOUT |\
                            MYSQL_OPEN_GET_NEW_TABLE |\
                            MYSQL_OPEN_HAS_MDL_LOCK)

bool is_locked_view(THD *thd, TABLE_LIST *t);
bool open_table(THD *thd, TABLE_LIST *table_list, Open_table_context *ot_ctx);

bool get_key_map_from_key_list(key_map *map, TABLE *table,
                               List<String> *index_list);
TABLE *find_locked_table(TABLE *list, const char *db, const char *table_name);
TABLE *find_write_locked_table(TABLE *list, const char *db,
                               const char *table_name);
thr_lock_type read_lock_type_for_table(THD *thd,
                                       Query_tables_list *prelocking_ctx,
                                       TABLE_LIST *table_list,
                                       bool routine_modifies_data);

my_bool mysql_rm_tmp_tables(void);
void close_tables_for_reopen(THD *thd, TABLE_LIST **tables,
                             const MDL_savepoint &start_of_statement_svp);
bool table_already_fk_prelocked(TABLE_LIST *tl, LEX_CSTRING *db,
                                LEX_CSTRING *table, thr_lock_type lock_type);
TABLE_LIST *find_table_in_list(TABLE_LIST *table,
                               TABLE_LIST *TABLE_LIST::*link,
                               const LEX_CSTRING *db_name,
                               const LEX_CSTRING *table_name);
int close_thread_tables(THD *thd);
void switch_to_nullable_trigger_fields(List<Item> &items, TABLE *);
void switch_defaults_to_nullable_trigger_fields(TABLE *table);
bool fill_record_n_invoke_before_triggers(THD *thd, TABLE *table,
                                          List<Item> &fields,
                                          List<Item> &values,
                                          bool ignore_errors,
                                          enum trg_event_type event);
bool fill_record_n_invoke_before_triggers(THD *thd, TABLE *table,
                                          Field **field,
                                          List<Item> &values,
                                          bool ignore_errors,
                                          enum trg_event_type event);
bool insert_fields(THD *thd, Name_resolution_context *context,
		   const char *db_name, const char *table_name,
                   List_iterator<Item> *it, bool any_privileges,
                   uint *hidden_bit_fields);
void make_leaves_list(THD *thd, List<TABLE_LIST> &list, TABLE_LIST *tables,
                      bool full_table_list, TABLE_LIST *boundary);
int setup_wild(THD *thd, TABLE_LIST *tables, List<Item> &fields,
	       List<Item> *sum_func_list, SELECT_LEX *sl);
int setup_returning_fields(THD* thd, TABLE_LIST* table_list);
bool setup_fields(THD *thd, Ref_ptr_array ref_pointer_array,
                  List<Item> &item, enum_column_usage column_usage,
                  List<Item> *sum_func_list, List<Item> *pre_fix,
                  bool allow_sum_func);
void unfix_fields(List<Item> &items);
bool fill_record(THD * thd, TABLE *table_arg, List<Item> &fields,
                 List<Item> &values, bool ignore_errors, bool update);
bool fill_record(THD *thd, TABLE *table, Field **field, List<Item> &values,
                 bool ignore_errors, bool use_value);

Field *
find_field_in_tables(THD *thd, Item_ident *item,
                     TABLE_LIST *first_table, TABLE_LIST *last_table,
                     Item **ref, find_item_error_report_type report_error,
                     bool check_privileges, bool register_tree_change);
Field *
find_field_in_table_ref(THD *thd, TABLE_LIST *table_list,
                        const char *name, size_t length,
                        const char *item_name, const char *db_name,
                        const char *table_name, Item **ref,
                        bool check_privileges, bool allow_rowid,
                        uint16 *cached_field_index_ptr,
                        bool register_tree_change, TABLE_LIST **actual_table);
Field *
find_field_in_table(THD *thd, TABLE *table, const char *name, size_t length,
                    bool allow_rowid, uint16 *cached_field_index_ptr);
Field *
find_field_in_table_sef(TABLE *table, const char *name);
Item ** find_item_in_list(Item *item, List<Item> &items, uint *counter,
                          find_item_error_report_type report_error,
                          enum_resolution_type *resolution, uint limit= 0);
bool setup_tables(THD *thd, Name_resolution_context *context,
                  List<TABLE_LIST> *from_clause, TABLE_LIST *tables,
                  List<TABLE_LIST> &leaves, bool select_insert,
                  bool full_table_list);
bool setup_tables_and_check_access(THD *thd,
                                   Name_resolution_context *context,
                                   List<TABLE_LIST> *from_clause,
                                   TABLE_LIST *tables,
                                   List<TABLE_LIST> &leaves, 
                                   bool select_insert,
                                   privilege_t want_access_first,
                                   privilege_t want_access,
                                   bool full_table_list);
bool wait_while_table_is_used(THD *thd, TABLE *table,
                              enum ha_extra_function function);

void drop_open_table(THD *thd, TABLE *table, const LEX_CSTRING *db_name,
                     const LEX_CSTRING *table_name);
void update_non_unique_table_error(TABLE_LIST *update,
                                   const char *operation,
                                   TABLE_LIST *duplicate);
int setup_conds(THD *thd, TABLE_LIST *tables, List<TABLE_LIST> &leaves,
		COND **conds);
void wrap_ident(THD *thd, Item **conds);
int setup_ftfuncs(SELECT_LEX* select);
void cleanup_ftfuncs(SELECT_LEX *select_lex);
int init_ftfuncs(THD *thd, SELECT_LEX* select, bool no_order);
bool lock_table_names(THD *thd, const DDL_options_st &options,
                      TABLE_LIST *table_list,
                      TABLE_LIST *table_list_end, ulong lock_wait_timeout,
                      uint flags);
static inline bool
lock_table_names(THD *thd, TABLE_LIST *table_list,
                 TABLE_LIST *table_list_end, ulong lock_wait_timeout,
                 uint flags)
{
  return lock_table_names(thd, thd->lex->create_info, table_list,
                          table_list_end, lock_wait_timeout, flags);
}
bool open_tables(THD *thd, const DDL_options_st &options,
                 TABLE_LIST **tables, uint *counter,
                 uint flags, Prelocking_strategy *prelocking_strategy);

static inline bool
open_tables(THD *thd, TABLE_LIST **tables, uint *counter, uint flags,
            Prelocking_strategy *prelocking_strategy)
{
  return open_tables(thd, thd->lex->create_info, tables, counter, flags,
                     prelocking_strategy);
}
/* open_and_lock_tables with optional derived handling */
bool open_and_lock_tables(THD *thd, const DDL_options_st &options,
                          TABLE_LIST *tables,
                          bool derived, uint flags,
                          Prelocking_strategy *prelocking_strategy);
static inline bool
open_and_lock_tables(THD *thd, TABLE_LIST *tables,
                     bool derived, uint flags,
                     Prelocking_strategy *prelocking_strategy)
{
  return open_and_lock_tables(thd, thd->lex->create_info,
                              tables, derived, flags, prelocking_strategy);
}
/* simple open_and_lock_tables without derived handling for single table */
TABLE *open_n_lock_single_table(THD *thd, TABLE_LIST *table_l,
                                thr_lock_type lock_type, uint flags,
                                Prelocking_strategy *prelocking_strategy);
bool open_normal_and_derived_tables(THD *thd, TABLE_LIST *tables, uint flags,
                                    uint dt_phases);
bool open_tables_only_view_structure(THD *thd, TABLE_LIST *tables,
                                     bool can_deadlock);
bool open_and_lock_internal_tables(TABLE *table, bool lock);
bool lock_tables(THD *thd, TABLE_LIST *tables, uint counter, uint flags);
int decide_logging_format(THD *thd, TABLE_LIST *tables);
void close_thread_table(THD *thd, TABLE **table_ptr);
TABLE_LIST *unique_table(THD *thd, TABLE_LIST *table, TABLE_LIST *table_list,
                         uint check_flag);
bool is_equal(const LEX_CSTRING *a, const LEX_CSTRING *b);

class Open_tables_backup;
/* Functions to work with system tables. */
bool open_system_tables_for_read(THD *thd, TABLE_LIST *table_list);
void close_system_tables(THD *thd);
void close_mysql_tables(THD *thd);
TABLE *open_system_table_for_update(THD *thd, TABLE_LIST *one_table);
TABLE *open_log_table(THD *thd, TABLE_LIST *one_table, Open_tables_backup *backup);
void close_log_table(THD *thd, Open_tables_backup *backup);

bool close_cached_tables(THD *thd, TABLE_LIST *tables,
                         bool wait_for_refresh, ulong timeout);
void purge_tables();
bool flush_tables(THD *thd, flush_tables_type flag);
void close_all_tables_for_name(THD *thd, TABLE_SHARE *share,
                               ha_extra_function extra,
                               TABLE *skip_table);
OPEN_TABLE_LIST *list_open_tables(THD *thd, const char *db, const char *wild);
bool tdc_open_view(THD *thd, TABLE_LIST *table_list, uint flags);

TABLE *find_table_for_mdl_upgrade(THD *thd, const char *db,
                                  const char *table_name,
                                  int *p_error);
void mark_tmp_table_for_reuse(TABLE *table);

int dynamic_column_error_message(enum_dyncol_func_result rc);

/* open_and_lock_tables with optional derived handling */
int open_and_lock_tables_derived(THD *thd, TABLE_LIST *tables, bool derived);

extern "C" int simple_raw_key_cmp(void* arg, const void* key1,
                                  const void* key2);
extern "C" int count_distinct_walk(void *elem, element_count count, void *arg);
int simple_str_key_cmp(void* arg, uchar* key1, uchar* key2);

extern Item **not_found_item;
extern Field *not_found_field;
extern Field *view_ref_found;

/**
  clean/setup table fields and map.

  @param table        TABLE structure pointer (which should be setup)
  @param table_list   TABLE_LIST structure pointer (owner of TABLE)
  @param tablenr     table number
*/


inline void setup_table_map(TABLE *table, TABLE_LIST *table_list, uint tablenr)
{
  table->used_fields= 0;
  table_list->reset_const_table();
  table->null_row= 0;
  table->status= STATUS_NO_RECORD;
  table->maybe_null= table_list->outer_join;
  TABLE_LIST *embedding= table_list->embedding;
  while (!table->maybe_null && embedding)
  {
    table->maybe_null= embedding->outer_join;
    embedding= embedding->embedding;
  }
  table->tablenr= tablenr;
  table->map= (table_map) 1 << tablenr;
  table->force_index= table_list->force_index;
  table->force_index_order= table->force_index_group= 0;
  table->covering_keys= table->s->keys_for_keyread;
}

inline TABLE_LIST *find_table_in_global_list(TABLE_LIST *table,
                                             LEX_CSTRING *db_name,
                                             LEX_CSTRING *table_name)
{
  return find_table_in_list(table, &TABLE_LIST::next_global,
                            db_name, table_name);
}

inline bool setup_fields_with_no_wrap(THD *thd, Ref_ptr_array ref_pointer_array,
                                      List<Item> &item,
                                      enum_column_usage column_usage,
                                      List<Item> *sum_func_list,
                                      bool allow_sum_func)
{
  bool res;
  SELECT_LEX *first= thd->lex->first_select_lex();
  DBUG_ASSERT(thd->lex->current_select == first);
  first->no_wrap_view_item= TRUE;
  res= setup_fields(thd, ref_pointer_array, item, column_usage,
                    sum_func_list, NULL,  allow_sum_func);
  first->no_wrap_view_item= FALSE;
  return res;
}

/**
  An abstract class for a strategy specifying how the prelocking
  algorithm should extend the prelocking set while processing
  already existing elements in the set.
*/

class Prelocking_strategy
{
public:
  virtual ~Prelocking_strategy() { }

  virtual void reset(THD *thd) { };
  virtual bool handle_routine(THD *thd, Query_tables_list *prelocking_ctx,
                              Sroutine_hash_entry *rt, sp_head *sp,
                              bool *need_prelocking) = 0;
  virtual bool handle_table(THD *thd, Query_tables_list *prelocking_ctx,
                            TABLE_LIST *table_list, bool *need_prelocking) = 0;
  virtual bool handle_view(THD *thd, Query_tables_list *prelocking_ctx,
                           TABLE_LIST *table_list, bool *need_prelocking)= 0;
  virtual bool handle_end(THD *thd) { return 0; };
};


/**
  A Strategy for prelocking algorithm suitable for DML statements.

  Ensures that all tables used by all statement's SF/SP/triggers and
  required for foreign key checks are prelocked and SF/SPs used are
  cached.
*/

class DML_prelocking_strategy : public Prelocking_strategy
{
public:
  virtual bool handle_routine(THD *thd, Query_tables_list *prelocking_ctx,
                              Sroutine_hash_entry *rt, sp_head *sp,
                              bool *need_prelocking);
  virtual bool handle_table(THD *thd, Query_tables_list *prelocking_ctx,
                            TABLE_LIST *table_list, bool *need_prelocking);
  virtual bool handle_view(THD *thd, Query_tables_list *prelocking_ctx,
                           TABLE_LIST *table_list, bool *need_prelocking);
};


/**
  A strategy for prelocking algorithm to be used for LOCK TABLES
  statement.
*/

class Lock_tables_prelocking_strategy : public DML_prelocking_strategy
{
  virtual bool handle_table(THD *thd, Query_tables_list *prelocking_ctx,
                            TABLE_LIST *table_list, bool *need_prelocking);
};


/**
  Strategy for prelocking algorithm to be used for ALTER TABLE statements.

  Unlike DML or LOCK TABLES strategy, it doesn't
  prelock triggers, views or stored routines, since they are not
  used during ALTER.
*/

class Alter_table_prelocking_strategy : public Prelocking_strategy
{
public:
  virtual bool handle_routine(THD *thd, Query_tables_list *prelocking_ctx,
                              Sroutine_hash_entry *rt, sp_head *sp,
                              bool *need_prelocking);
  virtual bool handle_table(THD *thd, Query_tables_list *prelocking_ctx,
                            TABLE_LIST *table_list, bool *need_prelocking);
  virtual bool handle_view(THD *thd, Query_tables_list *prelocking_ctx,
                           TABLE_LIST *table_list, bool *need_prelocking);
};


inline bool
open_tables(THD *thd, const DDL_options_st &options,
            TABLE_LIST **tables, uint *counter, uint flags)
{
  DML_prelocking_strategy prelocking_strategy;

  return open_tables(thd, options, tables, counter, flags,
                     &prelocking_strategy);
}
inline bool
open_tables(THD *thd, TABLE_LIST **tables, uint *counter, uint flags)
{
  DML_prelocking_strategy prelocking_strategy;

  return open_tables(thd, thd->lex->create_info, tables, counter, flags,
                     &prelocking_strategy);
}

inline TABLE *open_n_lock_single_table(THD *thd, TABLE_LIST *table_l,
                                       thr_lock_type lock_type, uint flags)
{
  DML_prelocking_strategy prelocking_strategy;

  return open_n_lock_single_table(thd, table_l, lock_type, flags,
                                  &prelocking_strategy);
}


/* open_and_lock_tables with derived handling */
inline bool open_and_lock_tables(THD *thd,
                                 const DDL_options_st &options,
                                 TABLE_LIST *tables,
                                 bool derived, uint flags)
{
  DML_prelocking_strategy prelocking_strategy;

  return open_and_lock_tables(thd, options, tables, derived, flags,
                              &prelocking_strategy);
}
inline bool open_and_lock_tables(THD *thd, TABLE_LIST *tables,
                                  bool derived, uint flags)
{
  DML_prelocking_strategy prelocking_strategy;

  return open_and_lock_tables(thd, thd->lex->create_info,
                              tables, derived, flags,
                              &prelocking_strategy);
}


bool restart_trans_for_tables(THD *thd, TABLE_LIST *table);

bool extend_table_list(THD *thd, TABLE_LIST *tables,
                       Prelocking_strategy *prelocking_strategy,
                       bool has_prelocking_list);

/**
  A context of open_tables() function, used to recover
  from a failed open_table() or open_routine() attempt.
*/

class Open_table_context
{
public:
  enum enum_open_table_action
  {
    OT_NO_ACTION= 0,
    OT_BACKOFF_AND_RETRY,
    OT_REOPEN_TABLES,
    OT_DISCOVER,
    OT_REPAIR
  };
  Open_table_context(THD *thd, uint flags);

  bool recover_from_failed_open();
  bool request_backoff_action(enum_open_table_action action_arg,
                              TABLE_LIST *table);

  bool can_recover_from_failed_open() const
  { return m_action != OT_NO_ACTION; }

  /**
    When doing a back-off, we close all tables acquired by this
    statement.  Return an MDL savepoint taken at the beginning of
    the statement, so that we can rollback to it before waiting on
    locks.
  */
  const MDL_savepoint &start_of_statement_svp() const
  {
    return m_start_of_statement_svp;
  }

  inline ulong get_timeout() const
  {
    return m_timeout;
  }

  uint get_flags() const { return m_flags; }

  /**
    Set flag indicating that we have already acquired metadata lock
    protecting this statement against GRL while opening tables.
  */
  void set_has_protection_against_grl(enum_mdl_type mdl_type)
  {
    m_has_protection_against_grl|= MDL_BIT(mdl_type);
  }

  bool has_protection_against_grl(enum_mdl_type mdl_type) const
  {
    return (bool) (m_has_protection_against_grl & MDL_BIT(mdl_type));
  }

private:
  /* THD for which tables are opened. */
  THD *m_thd;
  /**
    For OT_DISCOVER and OT_REPAIR actions, the table list element for
    the table which definition should be re-discovered or which
    should be repaired.
  */
  TABLE_LIST *m_failed_table;
  MDL_savepoint m_start_of_statement_svp;
  /**
    Lock timeout in seconds. Initialized to LONG_TIMEOUT when opening system
    tables or to the "lock_wait_timeout" system variable for regular tables.
  */
  ulong m_timeout;
  /* open_table() flags. */
  uint m_flags;
  /** Back off action. */
  enum enum_open_table_action m_action;
  /**
    Whether we had any locks when this context was created.
    If we did, they are from the previous statement of a transaction,
    and we can't safely do back-off (and release them).
  */
  bool m_has_locks;
  /**
    Indicates that in the process of opening tables we have acquired
    protection against global read lock.
  */
  mdl_bitmap_t m_has_protection_against_grl;
};


/**
  Check if a TABLE_LIST instance represents a pre-opened temporary table.
*/

inline bool is_temporary_table(TABLE_LIST *tl)
{
  if (tl->view || tl->schema_table)
    return FALSE;

  if (!tl->table)
    return FALSE;

  /*
    NOTE: 'table->s' might be NULL for specially constructed TABLE
    instances. See SHOW TRIGGERS for example.
  */

  if (!tl->table->s)
    return FALSE;

  return tl->table->s->tmp_table != NO_TMP_TABLE;
}


/**
  This internal handler is used to trap ER_NO_SUCH_TABLE.
*/

class No_such_table_error_handler : public Internal_error_handler
{
public:
  No_such_table_error_handler()
    : m_handled_errors(0), m_unhandled_errors(0)
  {}

  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl);

  /**
    Returns TRUE if one or more ER_NO_SUCH_TABLE errors have been
    trapped and no other errors have been seen. FALSE otherwise.
  */
  bool safely_trapped_errors();

private:
  int m_handled_errors;
  int m_unhandled_errors;
};


#endif /* SQL_BASE_INCLUDED */
