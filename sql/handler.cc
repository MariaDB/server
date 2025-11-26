/* Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2009, 2024, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/** @file handler.cc

    @brief
  Handler-calling-functions
*/

#include "mariadb.h"
#include <inttypes.h>
#include "sql_priv.h"
#include "unireg.h"
#include "rpl_rli.h"
#include "sql_cache.h"                   // query_cache, query_cache_*
#include "sql_connect.h"                 // global_table_stats
#include "key.h"     // key_copy, key_unpack, key_cmp_if_same, key_cmp
#include "sql_table.h"                   // build_table_filename
#include "sql_parse.h"                          // check_stack_overrun
#include "sql_base.h"           // TDC_element
#include "discover.h"           // extension_based_table_discovery, etc
#include "log_event.h"          // *_rows_log_event
#include "create_options.h"
#include <myisampack.h>
#include "transaction.h"
#include "myisam.h"
#include "probes_mysql.h"
#include <mysql/psi/mysql_table.h>
#include <pfs_transaction_provider.h>
#include <mysql/psi/mysql_transaction.h>
#include "debug_sync.h"         // DEBUG_SYNC
#include "debug.h"              // debug_decrement_counter
#include "sql_audit.h"
#include "ha_sequence.h"
#include "rowid_filter.h"
#include "mysys_err.h"
#include "optimizer_defaults.h"
#include "vector_mhnsw.h"

#ifdef WITH_PARTITION_STORAGE_ENGINE
#include "ha_partition.h"
#endif

#ifdef WITH_ARIA_STORAGE_ENGINE
#include "../storage/maria/ha_maria.h"
#endif
#include "semisync_master.h"

#include "wsrep_mysqld.h"
#ifdef WITH_WSREP
#include "wsrep_binlog.h"
#include "wsrep_xid.h"
#include "wsrep_thd.h"
#include "wsrep_trans_observer.h" /* wsrep transaction hooks */
#include "wsrep_var.h"            /* wsrep_hton_check() */
#endif /* WITH_WSREP */

/**
  @def MYSQL_TABLE_LOCK_WAIT
  Instrumentation helper for table io_waits.
  @param OP the table operation to be performed
  @param FLAGS per table operation flags.
  @param PAYLOAD the code to instrument.
  @sa MYSQL_END_TABLE_WAIT.
*/
#ifdef HAVE_PSI_TABLE_INTERFACE
  #define MYSQL_TABLE_LOCK_WAIT(OP, FLAGS, PAYLOAD)    \
    {                                                  \
      if (m_psi != NULL)                               \
      {                                                \
        PSI_table_locker *locker;                      \
        PSI_table_locker_state state;                  \
        locker= PSI_TABLE_CALL(start_table_lock_wait)  \
          (& state, m_psi, OP, FLAGS,                  \
          __FILE__, __LINE__);                         \
        PAYLOAD                                        \
        if (locker != NULL)                            \
          PSI_TABLE_CALL(end_table_lock_wait)(locker); \
      }                                                \
      else                                             \
      {                                                \
        PAYLOAD                                        \
      }                                                \
    }
#else
  #define MYSQL_TABLE_LOCK_WAIT(OP, FLAGS, PAYLOAD) \
    PAYLOAD
#endif


/*
  While we have legacy_db_type, we have this array to
  check for dups and to find handlerton from legacy_db_type.
  Remove when legacy_db_type is finally gone
*/
st_plugin_int *hton2plugin[MAX_HA];

static handlerton *installed_htons[128];

#define BITMAP_STACKBUF_SIZE (128/8)

KEY_CREATE_INFO default_key_create_info=
{ HA_KEY_ALG_UNDEF, 0, 0, {NullS, 0}, {NullS, 0}, false };

/* number of entries in handlertons[] */
ulong total_ha= 0;
/* number of storage engines (from handlertons[]) that support 2pc */
ulong total_ha_2pc= 0;
#ifdef DBUG_ASSERT_EXISTS
/*
  Number of non-mandatory 2pc handlertons whose initialization failed
  to estimate total_ha_2pc value under supposition of the failures
  have not occcured.
*/
ulong failed_ha_2pc= 0;
#endif
/* size of savepoint storage area (see ha_init) */
ulong savepoint_alloc_size= 0;

static const Lex_ident_engine sys_table_aliases[]=
{
  "INNOBASE"_Lex_ident_engine,  "INNODB"_Lex_ident_engine,
  "HEAP"_Lex_ident_engine,      "MEMORY"_Lex_ident_engine,
  "MERGE"_Lex_ident_engine,     "MRG_MYISAM"_Lex_ident_engine,
  "Maria"_Lex_ident_engine,     "Aria"_Lex_ident_engine,
  Lex_ident_engine()
};

const LEX_CSTRING ha_row_type[]=
{
  { STRING_WITH_LEN("") },
  { STRING_WITH_LEN("FIXED") },
  { STRING_WITH_LEN("DYNAMIC") },
  { STRING_WITH_LEN("COMPRESSED") },
  { STRING_WITH_LEN("REDUNDANT") },
  { STRING_WITH_LEN("COMPACT") },
  { STRING_WITH_LEN("PAGE") }
};

const char *tx_isolation_names[]=
{ "READ-UNCOMMITTED", "READ-COMMITTED", "REPEATABLE-READ", "SERIALIZABLE",
  NullS};
TYPELIB tx_isolation_typelib= CREATE_TYPELIB_FOR(tx_isolation_names);

static TYPELIB known_extensions= {0,"known_exts", NULL, NULL, NULL};
uint known_extensions_id= 0;


class Table_exists_error_handler : public Internal_error_handler
{
public:
  Table_exists_error_handler()
    : m_handled_errors(0), m_unhandled_errors(0)
  {}

  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl) override
  {
    *cond_hdl= NULL;
    if (non_existing_table_error(sql_errno))
    {
      m_handled_errors++;
      return TRUE;
    }

    if (*level == Sql_condition::WARN_LEVEL_ERROR)
      m_unhandled_errors++;
    return FALSE;
  }

  bool safely_trapped_errors()
  {
    return ((m_handled_errors > 0) && (m_unhandled_errors == 0));
  }

private:
  int m_handled_errors;
  int m_unhandled_errors;
};


static int commit_one_phase_2(THD *thd, bool all, THD_TRANS *trans,
                              bool is_real_trans);


static plugin_ref ha_default_plugin(THD *thd)
{
  if (thd->variables.table_plugin)
    return thd->variables.table_plugin;
  return my_plugin_lock(thd, global_system_variables.table_plugin);
}

static plugin_ref ha_default_tmp_plugin(THD *thd)
{
  if (thd->variables.tmp_table_plugin)
    return thd->variables.tmp_table_plugin;
  if (global_system_variables.tmp_table_plugin)
    return my_plugin_lock(thd, global_system_variables.tmp_table_plugin);
  return ha_default_plugin(thd);
}


/** @brief
  Return the default storage engine handlerton for thread

  SYNOPSIS
    ha_default_handlerton(thd)
    thd         current thread

  RETURN
    pointer to handlerton
*/
handlerton *ha_default_handlerton(THD *thd)
{
  plugin_ref plugin= ha_default_plugin(thd);
  DBUG_ASSERT(plugin);
  handlerton *hton= plugin_hton(plugin);
  DBUG_ASSERT(hton);
  return hton;
}


handlerton *ha_default_tmp_handlerton(THD *thd)
{
  plugin_ref plugin= ha_default_tmp_plugin(thd);
  DBUG_ASSERT(plugin);
  handlerton *hton= plugin_hton(plugin);
  DBUG_ASSERT(hton);
  return hton;
}


/** @brief
  Return the storage engine handlerton for the supplied name
  
  SYNOPSIS
    ha_resolve_by_name(thd, name)
    thd         current thread
    name        name of storage engine
  
  RETURN
    pointer to storage engine plugin handle
*/
plugin_ref ha_resolve_by_name(THD *thd, const LEX_CSTRING *name,
                              bool tmp_table)
{
  plugin_ref plugin;

redo:
  if (thd && "DEFAULT"_Lex_ident_engine.streq(*name))
    return tmp_table ?  ha_default_tmp_plugin(thd) : ha_default_plugin(thd);

  if ((plugin= my_plugin_lock_by_name(thd, name, MYSQL_STORAGE_ENGINE_PLUGIN)))
  {
    handlerton *hton= plugin_hton(plugin);
    if (hton && !(hton->flags & HTON_NOT_USER_SELECTABLE))
      return plugin;
      
    /*
      unlocking plugin immediately after locking is relatively low cost.
    */
    plugin_unlock(thd, plugin);
  }

  /*
    We check for the historical aliases.
  */
  for (const Lex_ident_engine *table_alias= sys_table_aliases;
       table_alias->str;
       table_alias+= 2)
  {
    if (table_alias->streq(*name))
    {
      name= table_alias + 1;
      goto redo;
    }
  }

  return NULL;
}


/*
  Resolve the storage engine by name.

  Succeed if the storage engine is found and initialised. Otherwise
  fail if the sql mode contains NO_ENGINE_SUBSTITUTION.
*/
bool
Storage_engine_name::resolve_storage_engine_with_error(THD *thd,
                                                       handlerton **ha,
                                                       bool tmp_table)
{
  plugin_ref plugin;
  if ((plugin= ha_resolve_by_name(thd, &m_storage_engine_name, tmp_table)) &&
      (plugin_ref_to_int(plugin)->state == PLUGIN_IS_READY))
  {
    *ha= plugin_hton(plugin);
    return false;
  }

  *ha= NULL;
  if ((thd_sql_command(thd) != SQLCOM_CREATE_TABLE &&
       thd_sql_command(thd) != SQLCOM_ALTER_TABLE) ||
      thd->variables.sql_mode & MODE_NO_ENGINE_SUBSTITUTION)
  {
    my_error(ER_UNKNOWN_STORAGE_ENGINE, MYF(0), m_storage_engine_name.str);
    return true;
  }
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_UNKNOWN_STORAGE_ENGINE,
                      ER_THD(thd, ER_UNKNOWN_STORAGE_ENGINE),
                      m_storage_engine_name.str);
  return false;
}


plugin_ref ha_lock_engine(THD *thd, const handlerton *hton)
{
  if (hton)
  {
    st_plugin_int *plugin= hton2plugin[hton->slot];
    return my_plugin_lock(thd, plugin_int_to_ref(plugin));
  }
  return NULL;
}


handlerton *ha_resolve_by_legacy_type(THD *thd, enum legacy_db_type db_type)
{
  plugin_ref plugin;
  switch (db_type) {
  case DB_TYPE_DEFAULT:
    return ha_default_handlerton(thd);
  default:
    if (db_type > DB_TYPE_UNKNOWN && db_type < DB_TYPE_DEFAULT &&
        (plugin= ha_lock_engine(thd, installed_htons[db_type])))
      return plugin_hton(plugin);
    /* fall through */
  case DB_TYPE_UNKNOWN:
    return NULL;
  }
}


/**
  Use other database handler if databasehandler is not compiled in.
*/
handlerton *ha_checktype(THD *thd, handlerton *hton, bool no_substitute)
{
  if (ha_storage_engine_is_enabled(hton))
    return hton;

  if (no_substitute)
    return NULL;

  return ha_default_handlerton(thd);
} /* ha_checktype */


handler *get_new_handler(TABLE_SHARE *share, MEM_ROOT *alloc,
                         handlerton *db_type)
{
  handler *file;
  DBUG_ENTER("get_new_handler");
  DBUG_PRINT("enter", ("alloc: %p", alloc));

  if (ha_storage_engine_is_enabled(db_type))
  {
    if ((file= db_type->create(db_type, share, alloc)))
      file->init();
    DBUG_RETURN(file);
  }
  /*
    Try the default table type
    Here the call to current_thd() is ok as we call this function a lot of
    times but we enter this branch very seldom.
  */
  file= get_new_handler(share, alloc, ha_default_handlerton(current_thd));
  DBUG_RETURN(file);
}


#ifdef WITH_PARTITION_STORAGE_ENGINE
handler *get_ha_partition(partition_info *part_info)
{
  ha_partition *partition;
  DBUG_ENTER("get_ha_partition");
  if ((partition= new ha_partition(partition_hton, part_info)))
  {
    if (partition->initialize_partition(current_thd->mem_root))
    {
      delete partition;
      partition= 0;
    }
    else
      partition->init();
  }
  else
  {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATAL), 
             static_cast<int>(sizeof(ha_partition)));
  }
  DBUG_RETURN(((handler*) partition));
}
#endif

static const char **handler_errmsgs;

C_MODE_START
static const char **get_handler_errmsgs(int nr)
{
  return handler_errmsgs;
}
C_MODE_END


/**
  Register handler error messages for use with my_error().

  @retval
    0           OK
  @retval
    !=0         Error
*/
#include "my_handler_errors.h"

int ha_init_errors(void)
{
#define SETMSG(nr, msg) handler_errmsgs[(nr) - HA_ERR_FIRST]= (msg)

  /* Allocate a pointer array for the error message strings. */
  if (! (handler_errmsgs= (const char**) my_malloc(key_memory_handler_errmsgs,
                                                   HA_ERR_ERRORS * sizeof(char*),
                                                   MYF(MY_WME))))
    return 1;

  /* Copy default handler error messages */
  memcpy(handler_errmsgs, handler_error_messages, HA_ERR_ERRORS * sizeof(char*));

  /* Set the dedicated error messages. */
  SETMSG(HA_ERR_KEY_NOT_FOUND,          ER_DEFAULT(ER_KEY_NOT_FOUND));
  SETMSG(HA_ERR_FOUND_DUPP_KEY,         ER_DEFAULT(ER_DUP_KEY));
  SETMSG(HA_ERR_RECORD_CHANGED,         "Update which is recoverable");
  SETMSG(HA_ERR_WRONG_INDEX,            "Wrong index given to function");
  SETMSG(HA_ERR_CRASHED,                ER_DEFAULT(ER_NOT_KEYFILE));
  SETMSG(HA_ERR_WRONG_IN_RECORD,        ER_DEFAULT(ER_CRASHED_ON_USAGE));
  SETMSG(HA_ERR_OUT_OF_MEM,             "Table handler out of memory");
  SETMSG(HA_ERR_NOT_A_TABLE,            "Incorrect file format '%.64s'");
  SETMSG(HA_ERR_WRONG_COMMAND,          "Command not supported");
  SETMSG(HA_ERR_OLD_FILE,               ER_DEFAULT(ER_OLD_KEYFILE));
  SETMSG(HA_ERR_NO_ACTIVE_RECORD,       "No record read in update");
  SETMSG(HA_ERR_RECORD_DELETED,         "Intern record deleted");
  SETMSG(HA_ERR_RECORD_FILE_FULL,       ER_DEFAULT(ER_RECORD_FILE_FULL));
  SETMSG(HA_ERR_INDEX_FILE_FULL,        "No more room in index file '%.64s'");
  SETMSG(HA_ERR_END_OF_FILE,            "End in next/prev/first/last");
  SETMSG(HA_ERR_UNSUPPORTED,            ER_DEFAULT(ER_ILLEGAL_HA));
  SETMSG(HA_ERR_TO_BIG_ROW,             "Too big row");
  SETMSG(HA_WRONG_CREATE_OPTION,        "Wrong create option");
  SETMSG(HA_ERR_FOUND_DUPP_UNIQUE,      ER_DEFAULT(ER_DUP_UNIQUE));
  SETMSG(HA_ERR_UNKNOWN_CHARSET,        "Can't open charset");
  SETMSG(HA_ERR_WRONG_MRG_TABLE_DEF,    ER_DEFAULT(ER_WRONG_MRG_TABLE));
  SETMSG(HA_ERR_CRASHED_ON_REPAIR,      ER_DEFAULT(ER_CRASHED_ON_REPAIR));
  SETMSG(HA_ERR_CRASHED_ON_USAGE,       ER_DEFAULT(ER_CRASHED_ON_USAGE));
  SETMSG(HA_ERR_LOCK_WAIT_TIMEOUT,      ER_DEFAULT(ER_LOCK_WAIT_TIMEOUT));
  SETMSG(HA_ERR_LOCK_TABLE_FULL,        ER_DEFAULT(ER_LOCK_TABLE_FULL));
  SETMSG(HA_ERR_READ_ONLY_TRANSACTION,  ER_DEFAULT(ER_READ_ONLY_TRANSACTION));
  SETMSG(HA_ERR_LOCK_DEADLOCK,          ER_DEFAULT(ER_LOCK_DEADLOCK));
  SETMSG(HA_ERR_CANNOT_ADD_FOREIGN,     ER_DEFAULT(ER_CANNOT_ADD_FOREIGN));
  SETMSG(HA_ERR_NO_REFERENCED_ROW,      ER_DEFAULT(ER_NO_REFERENCED_ROW_2));
  SETMSG(HA_ERR_ROW_IS_REFERENCED,      ER_DEFAULT(ER_ROW_IS_REFERENCED_2));
  SETMSG(HA_ERR_NO_SAVEPOINT,           "No savepoint with that name");
  SETMSG(HA_ERR_NON_UNIQUE_BLOCK_SIZE,  "Non unique key block size");
  SETMSG(HA_ERR_NO_SUCH_TABLE,          "No such table: '%.64s'");
  SETMSG(HA_ERR_TABLE_EXIST,            ER_DEFAULT(ER_TABLE_EXISTS_ERROR));
  SETMSG(HA_ERR_NO_CONNECTION,          "Could not connect to storage engine");
  SETMSG(HA_ERR_TABLE_DEF_CHANGED,      ER_DEFAULT(ER_TABLE_DEF_CHANGED));
  SETMSG(HA_ERR_FOREIGN_DUPLICATE_KEY,  "FK constraint would lead to duplicate key");
  SETMSG(HA_ERR_TABLE_NEEDS_UPGRADE,    ER_DEFAULT(ER_TABLE_NEEDS_UPGRADE));
  SETMSG(HA_ERR_TABLE_READONLY,         ER_DEFAULT(ER_OPEN_AS_READONLY));
  SETMSG(HA_ERR_AUTOINC_READ_FAILED,    ER_DEFAULT(ER_AUTOINC_READ_FAILED));
  SETMSG(HA_ERR_AUTOINC_ERANGE,         ER_DEFAULT(ER_WARN_DATA_OUT_OF_RANGE));
  SETMSG(HA_ERR_TOO_MANY_CONCURRENT_TRXS, ER_DEFAULT(ER_TOO_MANY_CONCURRENT_TRXS));
  SETMSG(HA_ERR_INDEX_COL_TOO_LONG,	ER_DEFAULT(ER_INDEX_COLUMN_TOO_LONG));
  SETMSG(HA_ERR_INDEX_CORRUPT,		ER_DEFAULT(ER_INDEX_CORRUPT));
  SETMSG(HA_FTS_INVALID_DOCID,		"Invalid InnoDB FTS Doc ID");
  SETMSG(HA_ERR_DISK_FULL,              "Disk got full writing '%s'");
  SETMSG(HA_ERR_FTS_TOO_MANY_WORDS_IN_PHRASE,  "Too many words in a FTS phrase or proximity search");
  SETMSG(HA_ERR_FK_DEPTH_EXCEEDED,      "Foreign key cascade delete/update exceeds");
  SETMSG(HA_ERR_TABLESPACE_MISSING,     ER_DEFAULT(ER_TABLESPACE_MISSING));
  SETMSG(HA_ERR_INCOMPATIBLE_DEFINITION,
         "Mismatch between table definitions in sql and storage layer");

  /* Register the error messages for use with my_error(). */
  return my_error_register(get_handler_errmsgs, HA_ERR_FIRST, HA_ERR_LAST);
}


/**
  Unregister handler error messages.

  @retval
    0           OK
  @retval
    !=0         Error
*/
static int ha_finish_errors(void)
{
  /* Allocate a pointer array for the error message strings. */
  my_error_unregister(HA_ERR_FIRST, HA_ERR_LAST);
  my_free(handler_errmsgs);
  handler_errmsgs= 0;
  return 0;
}

static Atomic_counter<int32> need_full_discover_for_existence(0);
static Atomic_counter<int32> engines_with_discover_file_names(0);
static Atomic_counter<int32> engines_with_discover(0);

static int full_discover_for_existence(handlerton *, const char *, const char *)
{ return 0; }

static int ext_based_existence(handlerton *, const char *, const char *)
{ return 0; }

static int hton_ext_based_table_discovery(handlerton *hton,
                             const LEX_CSTRING *db,
                             MY_DIR *dir, handlerton::discovered_list *result)
{
  /*
    tablefile_extensions[0] is the metadata file, see
    the comment above tablefile_extensions declaration
  */
  return extension_based_table_discovery(dir, hton->tablefile_extensions[0],
                                         result);
}

static void update_discovery_counters(handlerton *hton, int val)
{
  if (hton->discover_table_existence == full_discover_for_existence)
    need_full_discover_for_existence+= val;

  if (hton->discover_table_names && hton->tablefile_extensions[0])
    engines_with_discover_file_names+= val;

  if (hton->discover_table)
    engines_with_discover+= val;
}

static int hton_drop_table(handlerton *hton, const char *path)
{
  Table_path_buffer tmp_path;
  handler *file= get_new_handler(nullptr, current_thd->mem_root, hton);
  if (!file)
  {
    /*
      If file is not defined it means that the engine can't create a
      handler if share is not set or we got an out of memory error
    */
    return my_errno == ENOMEM ? ENOMEM : ENOENT;
  }
  path= file->get_canonical_filename(Lex_cstring_strlen(path), &tmp_path).str;
  int error= file->delete_table(path);
  delete file;
  return error;
}


int ha_finalize_handlerton(void *plugin_)
{
  st_plugin_int *plugin= static_cast<st_plugin_int *>(plugin_);
  int deinit_status= 0;
  handlerton *hton= (handlerton *)plugin->data;
  DBUG_ENTER("ha_finalize_handlerton");

  /* hton can be NULL here, if ha_initialize_handlerton() failed. */
  if (!hton)
    goto end;

  if (installed_htons[hton->db_type] == hton)
    installed_htons[hton->db_type]= NULL;

  if (hton->panic)
    hton->panic(hton, HA_PANIC_CLOSE);

  if (plugin->plugin->deinit)
    deinit_status= plugin->plugin->deinit(NULL);

  free_sysvar_table_options(hton->table_options);
  free_sysvar_table_options(hton->field_options);
  free_sysvar_table_options(hton->index_options);
  update_discovery_counters(hton, -1);

  /*
    In case a plugin is uninstalled and re-installed later, it should
    reuse an array slot. Otherwise the number of uninstall/install
    cycles would be limited.
  */
  if (hton->slot != HA_SLOT_UNDEF)
  {
    /* Make sure we are not unpluging another plugin */
    DBUG_ASSERT(hton2plugin[hton->slot] == plugin);
    DBUG_ASSERT(hton->slot < MAX_HA);
    hton2plugin[hton->slot]= NULL;
  }

  my_free(hton);

 end:
  DBUG_RETURN(deinit_status);
}


/*
  Get a pointer to the global engine optimizer costs (like
  innodb.disk_read_cost) and store the pointer in the handlerton.

  This is called once when a handlerton is created.
  We also update the not set global costs with the default costs
  to allow information_schema to print the real used values.
*/

static bool update_optimizer_costs(handlerton *hton)
{
  OPTIMIZER_COSTS costs= default_optimizer_costs;
  LEX_CSTRING *name= hton_name(hton);

  if (hton->update_optimizer_costs)
    hton->update_optimizer_costs(&costs);

  mysql_mutex_lock(&LOCK_optimizer_costs);
  hton->optimizer_costs= get_or_create_optimizer_costs(name->str,
                                                       name->length);
  if (!hton->optimizer_costs)
  {
    mysql_mutex_unlock(&LOCK_optimizer_costs);
    return 1;                                   // OOM
  }

  /* Update not set values from current default costs */
  for (uint i=0 ; i < sizeof(OPTIMIZER_COSTS)/sizeof(double) ; i++)
  {
    double *var= ((double*) hton->optimizer_costs)+i;
    if (*var == OPTIMIZER_COST_UNDEF)
      *var= ((double*) &costs)[i];
  }
  mysql_mutex_unlock(&LOCK_optimizer_costs);
  return 0;
}

const char *hton_no_exts[]= { 0 };
static bool ddl_recovery_done= false;

int setup_transaction_participant(void *plugin_)
{
  st_plugin_int *plugin= static_cast<st_plugin_int *>(plugin_);
  auto tp= static_cast<transaction_participant*>(plugin->data);
  ulong fslot;
  for (fslot= 0; fslot < total_ha; fslot++)
    if (!hton2plugin[fslot])
      break;
  if (fslot < total_ha)
    tp->slot= fslot;
  else
  {
    if (total_ha >= MAX_HA)
    {
      sql_print_error("Too many plugins loaded. Limit is %u. Failed on '%s'",
                      MAX_HA, plugin->name.str);
      return 1;
    }
    tp->slot= total_ha++;
  }
  uint tmp= tp->savepoint_offset;
  tp->savepoint_offset= savepoint_alloc_size;
  savepoint_alloc_size+= tmp;
  hton2plugin[tp->slot]=plugin;

  if (tp->prepare)
  {
    total_ha_2pc++;
    if (tc_log && tc_log != get_tc_log_implementation())
    {
      total_ha_2pc--;
      tp->prepare= 0;
      push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_UNKNOWN_ERROR,
                          "Cannot enable tc-log at run-time. "
                          "XA features of %s are disabled",
                          plugin->name.str);
    }
  }

  return 0;
}

int ha_initialize_handlerton(void *plugin_)
{
  st_plugin_int *plugin= static_cast<st_plugin_int *>(plugin_);
  handlerton *hton;
  int ret= 0;
  DBUG_ENTER("ha_initialize_handlerton");
  DBUG_PRINT("plugin", ("initialize plugin: '%s'", plugin->name.str));

  hton= (handlerton *)my_malloc(key_memory_handlerton, sizeof(handlerton),
                                MYF(MY_WME | MY_ZEROFILL));
  if (hton == NULL)
  {
    sql_print_error("Unable to allocate memory for plugin '%s' handlerton.",
                    plugin->name.str);
    ret= 1;
    goto err_no_hton_memory;
  }

  hton->tablefile_extensions= hton_no_exts;
  hton->discover_table_names= hton_ext_based_table_discovery;
  hton->drop_table= hton_drop_table;

  hton->slot= HA_SLOT_UNDEF;
  /* Historical Requirement */
  plugin->data= hton; // shortcut for the future
  if (plugin->plugin->init && (ret= plugin->plugin->init(hton)))
    goto err;

  // hton_ext_based_table_discovery() works only when discovery
  // is supported and the engine if file-based.
  if (hton->discover_table_names == hton_ext_based_table_discovery &&
      (!hton->discover_table || !hton->tablefile_extensions[0]))
    hton->discover_table_names= NULL;

  // default discover_table_existence implementation
  if (!hton->discover_table_existence && hton->discover_table)
  {
    if (hton->tablefile_extensions[0])
      hton->discover_table_existence= ext_based_existence;
    else
      hton->discover_table_existence= full_discover_for_existence;
  }

  DBUG_EXECUTE_IF("unstable_db_type", {
                    static int i= (int) DB_TYPE_FIRST_DYNAMIC;
                    while (installed_htons[i])
                      i++;
                    hton->db_type= (enum legacy_db_type)++i;
                  });

  /* now check the db_type for conflict */
  if (hton->db_type <= DB_TYPE_UNKNOWN ||
      hton->db_type >= DB_TYPE_DEFAULT ||
      installed_htons[hton->db_type])
  {
    int idx= (int) DB_TYPE_FIRST_DYNAMIC;

    while (idx < (int) DB_TYPE_DEFAULT && installed_htons[idx])
      idx++;

    if (idx == (int) DB_TYPE_DEFAULT)
    {
      sql_print_warning("Too many storage engines!");
      ret= 1;
      goto err_deinit;
    }
    if (hton->db_type != DB_TYPE_UNKNOWN)
      sql_print_warning("Storage engine '%s' has conflicting typecode. "
                        "Assigning value %d.", plugin->plugin->name, idx);
    hton->db_type= (enum legacy_db_type) idx;
  }

  if ((ret= setup_transaction_participant(plugin)))
    goto err_deinit;

  installed_htons[hton->db_type]= hton;

  if (!(hton->flags & HTON_HIDDEN) && update_optimizer_costs(hton))
    goto err_deinit;

  /* 
    This is entirely for legacy. We will create a new "disk based" hton and a 
    "memory" hton which will be configurable longterm. We should be able to 
    remove partition.
  */
  switch (hton->db_type) {
  case DB_TYPE_HEAP:
    heap_hton= hton;
    break;
  case DB_TYPE_MYISAM:
    myisam_hton= hton;
    break;
  case DB_TYPE_PARTITION_DB:
    partition_hton= hton;
    break;
  case DB_TYPE_SEQUENCE:
    sql_sequence_hton= hton;
    break;
  default:
    break;
  };

  resolve_sysvar_table_options(hton->table_options);
  resolve_sysvar_table_options(hton->field_options);
  resolve_sysvar_table_options(hton->index_options);
  update_discovery_counters(hton, 1);

  if (ddl_recovery_done && hton->signal_ddl_recovery_done)
    ret= hton->signal_ddl_recovery_done(hton);

  DBUG_RETURN(ret);

err_deinit:
  /* 
    Let plugin do its inner deinitialization as plugin->init() 
    was successfully called before.
  */
  if (plugin->plugin->deinit)
    (void) plugin->plugin->deinit(NULL);

err:
#ifdef DBUG_ASSERT_EXISTS
  if (hton->prepare)
    failed_ha_2pc++;
#endif
  my_free(hton);
err_no_hton_memory:
  plugin->data= NULL;
  DBUG_RETURN(ret);
}

int ha_init()
{
  int error= 0;
  DBUG_ENTER("ha_init");

  DBUG_ASSERT(total_ha < MAX_HA);
  /*
    Check if there is a transaction-capable storage engine besides the
    binary log (which is considered a transaction-capable storage engine in
    counting total_ha)
  */
  opt_using_transactions= total_ha > (ulong) opt_bin_log;
  savepoint_alloc_size+= sizeof(SAVEPOINT);
  DBUG_RETURN(error);
}

int ha_end()
{
  int error= 0;
  DBUG_ENTER("ha_end");

  /* 
    This should be eventually based on the graceful shutdown flag.
    So if flag is equal to HA_PANIC_CLOSE, the deallocate
    the errors.
  */
  if (unlikely(ha_finish_errors()))
    error= 1;

  DBUG_RETURN(error);
}


/*
  unlike plugin_foreach() this is called for all plugins from
  hton2plugin[], that is for anything that has a transaction_participant
  object. Not only for storage engines.
*/
typedef bool (tp_foreach_func)(THD *thd, transaction_participant *tp, void *arg);

static bool tp_foreach(THD *thd, tp_foreach_func *func, void *arg)
{
  int j=0, err= 0;
  plugin_ref locks[MAX_HA];
  for (uint i= 0; i < MAX_HA; i++)
  {
    if (st_plugin_int *pi= hton2plugin[i])
    {
      locks[j]= plugin_lock(NULL, plugin_int_to_ref(pi));
      if ((err= func(thd, plugin_hton(locks[j++]), arg)))
        break;
    }
  }
  plugin_unlock_list(NULL, locks, j);
  return err;
}


static my_bool dropdb_handlerton(THD *, plugin_ref plugin, void *path)
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->drop_database)
    hton->drop_database(hton, (char *)path);
  return FALSE;
}


void ha_drop_database(const char* path)
{
  plugin_foreach(NULL, dropdb_handlerton, MYSQL_STORAGE_ENGINE_PLUGIN,
                 (char*) path);
}


struct st_commit_checkpoint_request {
  void *cookie;
  void (*pre_hook)(void *);
};

static bool commit_checkpoint_request_handlerton(THD *,
                                                 transaction_participant *hton,
                                                 void *data)
{
  st_commit_checkpoint_request *st= (st_commit_checkpoint_request *)data;
  if (hton->commit_checkpoint_request)
  {
    void *cookie= st->cookie;
    if (st->pre_hook)
      (*st->pre_hook)(cookie);
    (*hton->commit_checkpoint_request)(cookie);
  }
  return FALSE;
}


/*
  Invoke commit_checkpoint_request() in all storage engines that implement it.

  If pre_hook is non-NULL, the hook will be called prior to each invocation.
*/
void
ha_commit_checkpoint_request(void *cookie, void (*pre_hook)(void *))
{
  st_commit_checkpoint_request st;
  st.cookie= cookie;
  st.pre_hook= pre_hook;
  tp_foreach(NULL, commit_checkpoint_request_handlerton, &st);
}


/**
  @note
    don't bother to rollback here, it's done already

  there's no need to rollback here as all transactions must
  be rolled back already
*/
void ha_close_connection(THD* thd)
{
  for (auto i= 0; i < MAX_HA; i++)
  {
    if (plugin_ref plugin= thd->ha_data[i].lock)
    {
      thd->ha_data[i].lock= NULL;
      handlerton *hton= plugin_hton(plugin);
      if (hton->close_connection)
        hton->close_connection(thd);
      thd_set_ha_data(thd, hton, 0);
      plugin_unlock(NULL, plugin);
    }
    DBUG_ASSERT(!thd->ha_data[i].ha_ptr);
  }
}

static my_bool kill_handlerton(THD *thd, plugin_ref plugin,
                               void *level)
{
  handlerton *hton= plugin_hton(plugin);

  mysql_mutex_assert_owner(&thd->LOCK_thd_data);
  if (hton->kill_query && thd_get_ha_data(thd, hton))
    hton->kill_query(hton, thd, *(enum thd_kill_levels *) level);
  return FALSE;
}

void ha_kill_query(THD* thd, enum thd_kill_levels level)
{
  DBUG_ENTER("ha_kill_query");
  plugin_foreach(thd, kill_handlerton, MYSQL_STORAGE_ENGINE_PLUGIN, &level);
  DBUG_VOID_RETURN;
}


static my_bool plugin_disable_internal_writes(THD *, plugin_ref plugin,
                                              void *disable)
{
  if (void(*diw)(bool)= plugin_hton(plugin)->disable_internal_writes)
    diw(*static_cast<bool*>(disable));
  return FALSE;
}


void ha_disable_internal_writes(bool disable)
{
  plugin_foreach(NULL, plugin_disable_internal_writes,
                 MYSQL_STORAGE_ENGINE_PLUGIN, &disable);
}


static my_bool signal_ddl_recovery_done(THD *, plugin_ref plugin, void *)
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->signal_ddl_recovery_done)
	  if ((hton->signal_ddl_recovery_done)(hton))
		  plugin_ref_to_int(plugin)->state= PLUGIN_IS_DELETED;
  return 0;
}


void ha_signal_ddl_recovery_done()
{
  DBUG_ENTER("ha_signal_ddl_recovery_done");
  plugin_foreach(NULL, signal_ddl_recovery_done, MYSQL_STORAGE_ENGINE_PLUGIN,
                 NULL);
  ddl_recovery_done= true;
  DBUG_VOID_RETURN;
}


/*****************************************************************************
  Backup functions
******************************************************************************/

static my_bool plugin_prepare_for_backup(THD *, plugin_ref plugin, void *)
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->prepare_for_backup)
    hton->prepare_for_backup();
  return FALSE;
}

void ha_prepare_for_backup()
{
  plugin_foreach_with_mask(0, plugin_prepare_for_backup,
                           MYSQL_STORAGE_ENGINE_PLUGIN,
                           PLUGIN_IS_DELETED|PLUGIN_IS_READY, 0);
}

static my_bool plugin_end_backup(THD *, plugin_ref plugin, void *)
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->end_backup)
    hton->end_backup();
  return FALSE;
}

void ha_end_backup()
{
  plugin_foreach_with_mask(0, plugin_end_backup,
                           MYSQL_STORAGE_ENGINE_PLUGIN,
                           PLUGIN_IS_DELETED|PLUGIN_IS_READY, 0);
}

/*
  Take a lock to block MDL_BACKUP_DDL (used by maria-backup) until
  the DDL operation is taking place
*/

bool handler::log_not_redoable_operation(const char *operation)
{
  DBUG_ENTER("log_not_redoable_operation");
  if (table->s->tmp_table == NO_TMP_TABLE)
  {
    /*
      Take a lock to ensure that mariadb-backup will notice the
      new log entry (and re-copy the table if needed).
    */
    THD *thd= table->in_use;
    MDL_request mdl_backup;
    backup_log_info ddl_log;

    MDL_REQUEST_INIT(&mdl_backup, MDL_key::BACKUP, "", "", MDL_BACKUP_DDL,
                     MDL_STATEMENT);
    if (thd->mdl_context.acquire_lock(&mdl_backup,
                                      thd->variables.lock_wait_timeout))
      DBUG_RETURN(1);

    bzero(&ddl_log, sizeof(ddl_log));
    lex_string_set(&ddl_log.query, operation);
    /*
      We can't use partition_engine() here as this function is called
      directly by the handler for the underlaying partition table
    */
#ifdef WITH_PARTITION_STORAGE_ENGINE
    ddl_log.org_partitioned= table->s->partition_info_str != 0;
#endif
    lex_string_set(&ddl_log.org_storage_engine_name, table_type());
    ddl_log.org_database=     table->s->db;
    ddl_log.org_table=        table->s->table_name;
    ddl_log.org_table_id=     table->s->tabledef_version;
    backup_log_ddl(&ddl_log);
  }
  DBUG_RETURN(0);
}

/*
  Inform plugin of the server shutdown.
  Called after all connections are down.

  Under some circumstances, storage engine might need to
  so some work, before deinit() can be safely called.
  (an example is Innodb purge that might call into server
   to calculate virtual columns, which might potentially also
  invoke other plugins, such as audit
*/
static my_bool plugin_pre_shutdown(THD *, plugin_ref plugin, void *)
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->pre_shutdown)
    hton->pre_shutdown();
  return FALSE;
}


void ha_pre_shutdown()
{
  plugin_foreach_with_mask(0, plugin_pre_shutdown,
    MYSQL_STORAGE_ENGINE_PLUGIN,
    PLUGIN_IS_DELETED | PLUGIN_IS_READY, 0);
}


/* ========================================================================
 ======================= TRANSACTIONS ===================================*/

/**
  Transaction handling in the server
  ==================================

  In each client connection, MySQL maintains two transactional
  states:
  - a statement transaction,
  - a standard, also called normal transaction.

  Historical note
  ---------------
  "Statement transaction" is a non-standard term that comes
  from the times when MySQL supported BerkeleyDB storage engine.

  First of all, it should be said that in BerkeleyDB auto-commit
  mode auto-commits operations that are atomic to the storage
  engine itself, such as a write of a record, and are too
  high-granular to be atomic from the application perspective
  (MySQL). One SQL statement could involve many BerkeleyDB
  auto-committed operations and thus BerkeleyDB auto-commit was of
  little use to MySQL.

  Secondly, instead of SQL standard savepoints, BerkeleyDB
  provided the concept of "nested transactions". In a nutshell,
  transactions could be arbitrarily nested, but when the parent
  transaction was committed or aborted, all its child (nested)
  transactions were handled committed or aborted as well.
  Commit of a nested transaction, in turn, made its changes
  visible, but not durable: it destroyed the nested transaction,
  all its changes would become available to the parent and
  currently active nested transactions of this parent.

  So the mechanism of nested transactions was employed to
  provide "all or nothing" guarantee of SQL statements
  required by the standard.
  A nested transaction would be created at start of each SQL
  statement, and destroyed (committed or aborted) at statement
  end. Such nested transaction was internally referred to as
  a "statement transaction" and gave birth to the term.

  (Historical note ends)

  Since then a statement transaction is started for each statement
  that accesses transactional tables or uses the binary log.  If
  the statement succeeds, the statement transaction is committed.
  If the statement fails, the transaction is rolled back. Commits
  of statement transactions are not durable -- each such
  transaction is nested in the normal transaction, and if the
  normal transaction is rolled back, the effects of all enclosed
  statement transactions are undone as well.  Technically,
  a statement transaction can be viewed as a savepoint which is
  maintained automatically in order to make effects of one
  statement atomic.

  The normal transaction is started by the user and is ended
  usually upon a user request as well. The normal transaction
  encloses transactions of all statements issued between
  its beginning and its end.
  In autocommit mode, the normal transaction is equivalent
  to the statement transaction.

  Since MySQL supports PSEA (pluggable storage engine
  architecture), more than one transactional engine can be
  active at a time. Hence transactions, from the server
  point of view, are always distributed. In particular,
  transactional state is maintained independently for each
  engine. In order to commit a transaction the two phase
  commit protocol is employed.

  Not all statements are executed in context of a transaction.
  Administrative and status information statements do not modify
  engine data, and thus do not start a statement transaction and
  also have no effect on the normal transaction. Examples of such
  statements are SHOW STATUS and RESET SLAVE.

  Similarly DDL statements are not transactional,
  and therefore a transaction is [almost] never started for a DDL
  statement. The difference between a DDL statement and a purely
  administrative statement though is that a DDL statement always
  commits the current transaction before proceeding, if there is
  any.

  At last, SQL statements that work with non-transactional
  engines also have no effect on the transaction state of the
  connection. Even though they are written to the binary log,
  and the binary log is, overall, transactional, the writes
  are done in "write-through" mode, directly to the binlog
  file, followed with a OS cache sync, in other words,
  bypassing the binlog undo log (translog).
  They do not commit the current normal transaction.
  A failure of a statement that uses non-transactional tables
  would cause a rollback of the statement transaction, but
  in case there no non-transactional tables are used,
  no statement transaction is started.

  Data layout
  -----------

  The server stores its transaction-related data in
  thd->transaction. This structure has two members of type
  THD_TRANS. These members correspond to the statement and
  normal transactions respectively:

  - thd->transaction.stmt contains a list of engines
  that are participating in the given statement
  - thd->transaction.all contains a list of engines that
  have participated in any of the statement transactions started
  within the context of the normal transaction.
  Each element of the list contains a pointer to the storage
  engine, engine-specific transactional data, and engine-specific
  transaction flags.

  In autocommit mode thd->transaction.all is empty.
  Instead, data of thd->transaction.stmt is
  used to commit/rollback the normal transaction.

  The list of registered engines has a few important properties:
  - no engine is registered in the list twice
  - engines are present in the list a reverse temporal order --
  new participants are always added to the beginning of the list.

  Transaction life cycle
  ----------------------

  When a new connection is established, thd->transaction
  members are initialized to an empty state.
  If a statement uses any tables, all affected engines
  are registered in the statement engine list. In
  non-autocommit mode, the same engines are registered in
  the normal transaction list.
  At the end of the statement, the server issues a commit
  or a roll back for all engines in the statement list.
  At this point transaction flags of an engine, if any, are
  propagated from the statement list to the list of the normal
  transaction.
  When commit/rollback is finished, the statement list is
  cleared. It will be filled in again by the next statement,
  and emptied again at the next statement's end.

  The normal transaction is committed in a similar way
  (by going over all engines in thd->transaction.all list)
  but at different times:
  - upon COMMIT SQL statement is issued by the user
  - implicitly, by the server, at the beginning of a DDL statement
  or SET AUTOCOMMIT={0|1} statement.

  The normal transaction can be rolled back as well:
  - if the user has requested so, by issuing ROLLBACK SQL
  statement
  - if one of the storage engines requested a rollback
  by setting thd->transaction_rollback_request. This may
  happen in case, e.g., when the transaction in the engine was
  chosen a victim of the internal deadlock resolution algorithm
  and rolled back internally. When such a situation happens, there
  is little the server can do and the only option is to rollback
  transactions in all other participating engines.  In this case
  the rollback is accompanied by an error sent to the user.

  As follows from the use cases above, the normal transaction
  is never committed when there is an outstanding statement
  transaction. In most cases there is no conflict, since
  commits of the normal transaction are issued by a stand-alone
  administrative or DDL statement, thus no outstanding statement
  transaction of the previous statement exists. Besides,
  all statements that manipulate with the normal transaction
  are prohibited in stored functions and triggers, therefore
  no conflicting situation can occur in a sub-statement either.
  The remaining rare cases when the server explicitly has
  to commit the statement transaction prior to committing the normal
  one cover error-handling scenarios (see for example
  SQLCOM_LOCK_TABLES).

  When committing a statement or a normal transaction, the server
  either uses the two-phase commit protocol, or issues a commit
  in each engine independently. The two-phase commit protocol
  is used only if:
  - all participating engines support two-phase commit (provide
    handlerton::prepare PSEA API call) and
  - transactions in at least two engines modify data (i.e. are
  not read-only).

  Note that the two phase commit is used for
  statement transactions, even though they are not durable anyway.
  This is done to ensure logical consistency of data in a multiple-
  engine transaction.
  For example, imagine that some day MySQL supports unique
  constraint checks deferred till the end of statement. In such
  case a commit in one of the engines may yield ER_DUP_KEY,
  and MySQL should be able to gracefully abort statement
  transactions of other participants.

  After the normal transaction has been committed,
  thd->transaction.all list is cleared.

  When a connection is closed, the current normal transaction, if
  any, is rolled back.

  Roles and responsibilities
  --------------------------

  The server has no way to know that an engine participates in
  the statement and a transaction has been started
  in it unless the engine says so. Thus, in order to be
  a part of a transaction, the engine must "register" itself.
  This is done by invoking trans_register_ha() server call.
  Normally the engine registers itself whenever handler::external_lock()
  is called. trans_register_ha() can be invoked many times: if
  an engine is already registered, the call does nothing.
  In case autocommit is not set, the engine must register itself
  twice -- both in the statement list and in the normal transaction
  list.
  In which list to register is a parameter of trans_register_ha().

  Note, that although the registration interface in itself is
  fairly clear, the current usage practice often leads to undesired
  effects. E.g. since a call to trans_register_ha() in most engines
  is embedded into implementation of handler::external_lock(), some
  DDL statements start a transaction (at least from the server
  point of view) even though they are not expected to. E.g.
  CREATE TABLE does not start a transaction, since
  handler::external_lock() is never called during CREATE TABLE. But
  CREATE TABLE ... SELECT does, since handler::external_lock() is
  called for the table that is being selected from. This has no
  practical effects currently, but must be kept in mind
  nevertheless.

  Once an engine is registered, the server will do the rest
  of the work.

  During statement execution, whenever any of data-modifying
  PSEA API methods is used, e.g. handler::write_row() or
  handler::update_row(), the read-write flag is raised in the
  statement transaction for the involved engine.
  Currently All PSEA calls are "traced", and the data can not be
  changed in a way other than issuing a PSEA call. Important:
  unless this invariant is preserved the server will not know that
  a transaction in a given engine is read-write and will not
  involve the two-phase commit protocol!

  At the end of a statement, server call trans_commit_stmt is
  invoked. This call in turn invokes handlerton::prepare()
  for every involved engine. Prepare is followed by a call
  to handlerton::commit_one_phase() If a one-phase commit
  will suffice, handlerton::prepare() is not invoked and
  the server only calls handlerton::commit_one_phase().
  At statement commit, the statement-related read-write
  engine flag is propagated to the corresponding flag in the
  normal transaction.  When the commit is complete, the list
  of registered engines is cleared.

  Rollback is handled in a similar fashion.

  Additional notes on DDL and the normal transaction.
  ---------------------------------------------------

  DDLs and operations with non-transactional engines
  do not "register" in thd->transaction lists, and thus do not
  modify the transaction state. Besides, each DDL in
  MySQL is prefixed with an implicit normal transaction commit
  (a call to trans_commit_implicit()), and thus leaves nothing
  to modify.
  However, as it has been pointed out with CREATE TABLE .. SELECT,
  some DDL statements can start a *new* transaction.

  Behaviour of the server in this case is currently badly
  defined.
  DDL statements use a form of "semantic" logging
  to maintain atomicity: if CREATE TABLE .. SELECT failed,
  the newly created table is deleted.
  In addition, some DDL statements issue interim transaction
  commits: e.g. ALTER TABLE issues a commit after data is copied
  from the original table to the internal temporary table. Other
  statements, e.g. CREATE TABLE ... SELECT do not always commit
  after itself.
  And finally there is a group of DDL statements such as
  RENAME/DROP TABLE that doesn't start a new transaction
  and doesn't commit.

  This diversity makes it hard to say what will happen if
  by chance a stored function is invoked during a DDL --
  whether any modifications it makes will be committed or not
  is not clear. Fortunately, SQL grammar of few DDLs allows
  invocation of a stored function.

  A consistent behaviour is perhaps to always commit the normal
  transaction after all DDLs, just like the statement transaction
  is always committed at the end of all statements.
*/

/**
  Register a storage engine for a transaction.

  Every storage engine MUST call this function when it starts
  a transaction or a statement (that is it must be called both for the
  "beginning of transaction" and "beginning of statement").
  Only storage engines registered for the transaction/statement
  will know when to commit/rollback it.

  @note
    trans_register_ha is idempotent - storage engine may register many
    times per transaction.

*/
void trans_register_ha(THD *thd, bool all, transaction_participant *ht_arg, ulonglong trxid)
{
  THD_TRANS *trans;
  Ha_trx_info *ha_info;
  DBUG_ENTER("trans_register_ha");
  DBUG_PRINT("enter",("%s", all ? "all" : "stmt"));

  if (all)
  {
    trans= &thd->transaction->all;
    thd->server_status|= SERVER_STATUS_IN_TRANS;
    if (thd->tx_read_only)
      thd->server_status|= SERVER_STATUS_IN_TRANS_READONLY;
    DBUG_PRINT("info", ("setting SERVER_STATUS_IN_TRANS"));
  }
  else
    trans= &thd->transaction->stmt;

  ha_info= thd->ha_data[ht_arg->slot].ha_info + (all ? 1 : 0);

  if (ha_info->is_started())
    DBUG_VOID_RETURN; /* already registered, return */

  ha_info->register_ha(trans, ht_arg);

  trans->no_2pc|=(ht_arg->prepare==0);

  /* Set implicit xid even if there's explicit XA, it will be ignored anyway. */
  if (thd->transaction->implicit_xid.is_null())
    thd->transaction->implicit_xid.set(thd->query_id);

/*
  Register transaction start in performance schema if not done already.
  By doing this, we handle cases when the transaction is started implicitly in
  autocommit=0 mode, and cases when we are in normal autocommit=1 mode and the
  executed statement is a single-statement transaction.

  Explicitly started transactions are handled in trans_begin().

  Do not register transactions in which binary log is the only participating
  transactional storage engine.
*/
  if (thd->m_transaction_psi == NULL && ht_arg != &binlog_tp)
  {
    thd->m_transaction_psi= MYSQL_START_TRANSACTION(&thd->m_transaction_state,
          thd->get_xid(), trxid, thd->tx_isolation, thd->tx_read_only,
          !thd->in_multi_stmt_transaction_mode());
    DEBUG_SYNC(thd, "after_set_transaction_psi_before_set_transaction_gtid");
    //gtid_set_performance_schema_values(thd);
  }
  DBUG_VOID_RETURN;
}


static int prepare_or_error(transaction_participant *ht, THD *thd, bool all)
{
#ifdef WITH_WSREP
  const bool run_wsrep_hooks= wsrep_run_commit_hook(thd, all);
  if (run_wsrep_hooks && ht->flags & HTON_WSREP_REPLICATION &&
      wsrep_before_prepare(thd, all))
  {
    return(1);
  }
#endif /* WITH_WSREP */

  int err= ht->prepare(thd, all);
  status_var_increment(thd->status_var.ha_prepare_count);
  if (err)
      my_error(ER_GET_ERRNO, MYF(0), err, hton_name(ht)->str);
#ifdef WITH_WSREP
  if (run_wsrep_hooks && !err && ht->flags & HTON_WSREP_REPLICATION &&
      wsrep_after_prepare(thd, all))
  {
    err= 1;
  }
#endif /* WITH_WSREP */

  return err;
}


/**
  @retval
    0   ok
  @retval
    1   error, transaction was rolled back
*/
int ha_prepare(THD *thd)
{
  int error=0, all=1;
  THD_TRANS *trans=all ? &thd->transaction->all : &thd->transaction->stmt;
  Ha_trx_info *ha_info= trans->ha_list;
  DBUG_ENTER("ha_prepare");

  if (ha_info)
  {
    if (unlikely(tc_log->log_xa_prepare(thd, all)))
    {
      ha_rollback_trans(thd, all);
      error= 1;
      goto binlog_error;
    }
    for (; ha_info; ha_info= ha_info->next())
    {
      transaction_participant *ht= ha_info->ht();
      if (ht->prepare)
      {
        if (unlikely(prepare_or_error(ht, thd, all)))
        {
          thd->transaction->xid_state.set_rollback_only();
          ha_rollback_trans(thd, all);
          error=1;
          break;
        }
      }
      else
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_GET_ERRNO, ER_THD(thd, ER_GET_ERRNO),
                            HA_ERR_WRONG_COMMAND,
                            ha_resolve_storage_engine_name(ht));

      }
    }

binlog_error:
    DEBUG_SYNC(thd, "at_unlog_xa_prepare");

    if (tc_log->unlog_xa_prepare(thd, all))
    {
      ha_rollback_trans(thd, all);
      error=1;
    }
  }
  else if (thd->rgi_slave)
  {
    /*
      Slave threads will always process XA COMMITs in the binlog handler (see
      MDEV-25616 and MDEV-30423), so if this is a slave thread preparing a
      transaction which proved empty during replication (e.g. because of
      replication filters) then mark it as XA_ROLLBACK_ONLY so the follow up
      XA COMMIT will know to roll it back, rather than try to commit and binlog
      a standalone XA COMMIT (without its preceding XA START - XA PREPARE).

      If the xid_cache is cleared before the completion event comes, before
      issuing ER_XAER_NOTA, first check if the event targets an ignored
      database, and ignore the error if so.
    */
    thd->transaction->xid_state.set_rollback_only();
  }

  DBUG_RETURN(error);
}

/*
  Like ha_check_and_coalesce_trx_read_only to return counted number of
  read-write transaction participants limited to two, but works in the 'all'
  context.
  Also returns the last found rw ha_info through the 2nd argument.
*/
uint ha_count_rw_all(THD *thd, Ha_trx_info **ptr_ha_info)
{
  unsigned rw_ha_count= 0;

  for (auto ha_info= thd->transaction->all.ha_list; ha_info;
       ha_info= ha_info->next())
  {
    if (ha_info->is_trx_read_write())
    {
      *ptr_ha_info= ha_info;
      if (++rw_ha_count > 1)
        break;
    }
  }
  return rw_ha_count;
}

/*
  Returns counted number of
  read-write recoverable transaction participants.
*/
uint ha_count_rw_2pc(THD *thd, bool all)
{
  unsigned rw_ha_count= 0;
  THD_TRANS *trans=all ? &thd->transaction->all : &thd->transaction->stmt;

  for (Ha_trx_info * ha_info= trans->ha_list; ha_info;
       ha_info= ha_info->next())
  {
    if (ha_info->is_trx_read_write() && ha_info->ht()->recover)
      ++rw_ha_count;
  }
  return rw_ha_count;
}

/**
  Check if we can skip the two-phase commit.

  @param thd           Thread handler
  @param ha_list       List of all engines participating on the commit
  @param all           True if this is final commit (not statement commit)
  @param no_rollback   Set to 1 if one of the engines doing writes does
                       not support rollback

  A helper function to evaluate if two-phase commit is mandatory.
  As a side effect, propagates the read-only/read-write flags
  of the statement transaction to its enclosing normal transaction.
  
  If we have at least two engines with read-write changes we must
  run a two-phase commit. Otherwise we can run several independent
  commits as the only transactional engine has read-write changes
  and others are read-only.

  @retval   0   All engines are read-only.
  @retval   1   We have the only engine with read-write changes.
  @retval   >1  More than one engine have read-write changes.
                Note: return value might NOT be the exact number of
                engines with read-write changes.
*/

uint
ha_check_and_coalesce_trx_read_only(THD *thd, Ha_trx_info *ha_list,
                                    bool all, bool *no_rollback)
{
  /* The number of storage engines that have actual changes. */
  unsigned rw_ha_count= 0;
  Ha_trx_info *ha_info;

  *no_rollback= false;
  for (ha_info= ha_list; ha_info; ha_info= ha_info->next())
  {
    if (ha_info->is_trx_read_write())
    {
      ++rw_ha_count;
      if (ha_info->is_trx_no_rollback())
        *no_rollback= true;
    }

    if (! all)
    {
      Ha_trx_info *ha_info_all= &thd->ha_data[ha_info->ht()->slot].ha_info[1];
      DBUG_ASSERT(ha_info != ha_info_all);
      /*
        Merge read-only/read-write information about statement
        transaction to its enclosing normal transaction. Do this
        only if in a real transaction -- that is, if we know
        that ha_info_all is registered in thd->transaction.all.
        Since otherwise we only clutter the normal transaction flags.
      */
      if (ha_info_all->is_started()) /* FALSE if autocommit. */
        ha_info_all->coalesce_trx_with(ha_info);
    }
    else if (rw_ha_count > 1)
    {
      /*
        It is a normal transaction, so we don't need to merge read/write
        information up, and the need for two-phase commit has been
        already established. Break the loop prematurely.
      */
      if (*no_rollback == 0)
      {
        while ((ha_info= ha_info->next()))
        {
          if (ha_info->is_trx_read_write() && ha_info->is_trx_no_rollback())
          {
            *no_rollback= 1;
            break;
          }
        }
        break;
      }
    }
  }
  return rw_ha_count;
}

#ifdef WITH_WSREP
/**
  Check if transaction contains storage engine not supporting
  two-phase commit and transaction is read-write.

  @retval
    true Transaction contains storage engine not supporting
         two phase commit and transaction is read-write
  @retval
    false otherwise
*/
static bool wsrep_have_no2pc_rw_ha(Ha_trx_info* ha_list)
{
  for (Ha_trx_info *ha_info=ha_list; ha_info; ha_info= ha_info->next())
  {
    transaction_participant *ht= ha_info->ht();
    // Transaction is read-write and handler does not support 2pc
    if (ha_info->is_trx_read_write() && ht->prepare==0)
      return true;
  }
  return false;
}
#endif /* WITH_WSREP */

/**
  @retval
    0   ok
  @retval
    1   transaction was rolled back
  @retval
    2   error during commit, data may be inconsistent

  @todo
    Since we don't support nested statement transactions in 5.0,
    we can't commit or rollback stmt transactions while we are inside
    stored functions or triggers. So we simply do nothing now.
    TODO: This should be fixed in later ( >= 5.1) releases.
*/
int ha_commit_trans(THD *thd, bool all)
{
  int error= 0, cookie;
  /*
    'all' means that this is either an explicit commit issued by
    user, or an implicit commit issued by a DDL.
  */
  THD_TRANS *trans= all ? &thd->transaction->all : &thd->transaction->stmt;
  /*
    "real" is a nick name for a transaction for which a commit will
    make persistent changes. E.g. a 'stmt' transaction inside an 'all'
    transaction is not 'real': even though it's possible to commit it,
    the changes are not durable as they might be rolled back if the
    enclosing 'all' transaction is rolled back.
  */
  bool is_real_trans= ((all || thd->transaction->all.ha_list == 0) &&
                       !(thd->variables.option_bits & OPTION_GTID_BEGIN));
  Ha_trx_info *ha_info= trans->ha_list;
  bool need_prepare_ordered, need_commit_ordered;
  my_xid xid;
#ifdef WITH_WSREP
  const bool run_wsrep_hooks= wsrep_run_commit_hook(thd, all);
#endif /* WITH_WSREP */
  DBUG_ENTER("ha_commit_trans");
  DBUG_PRINT("info",("thd: %p  option_bits: %lu  all: %d",
                     thd, (ulong) thd->variables.option_bits, all));

  /* Just a random warning to test warnings pushed during autocommit. */
  DBUG_EXECUTE_IF("warn_during_ha_commit_trans",
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_WARNING_NOT_COMPLETE_ROLLBACK,
                 ER_THD(thd, ER_WARNING_NOT_COMPLETE_ROLLBACK)););

  DBUG_PRINT("info",
             ("all: %d  thd->in_sub_stmt: %d  ha_info: %p  is_real_trans: %d",
              all, thd->in_sub_stmt, ha_info, is_real_trans));
  /*
    We must not commit the normal transaction if a statement
    transaction is pending. Otherwise statement transaction
    flags will not get propagated to its normal transaction's
    counterpart.
  */
  DBUG_ASSERT(thd->transaction->stmt.ha_list == NULL ||
              trans == &thd->transaction->stmt);

  DBUG_ASSERT(!thd->in_sub_stmt);

  if (thd->in_sub_stmt)
  {
    /*
      Since we don't support nested statement transactions in 5.0,
      we can't commit or rollback stmt transactions while we are inside
      stored functions or triggers. So we simply do nothing now.
      TODO: This should be fixed in later ( >= 5.1) releases.
    */
    if (!all)
      DBUG_RETURN(0);
    /*
      We assume that all statements which commit or rollback main transaction
      are prohibited inside of stored functions or triggers. So they should
      bail out with error even before ha_commit_trans() call. To be 100% safe
      let us throw error in non-debug builds.
    */
    my_error(ER_COMMIT_NOT_ALLOWED_IN_SF_OR_TRG, MYF(0));
    DBUG_RETURN(2);
  }

  if (!ha_info)
  {
    /*
      Free resources and perform other cleanup even for 'empty' transactions.
    */
    if (is_real_trans)
    {
      thd->transaction->cleanup();
      MYSQL_COMMIT_TRANSACTION(thd->m_transaction_psi);
      thd->m_transaction_psi= NULL;
    }
#ifdef WITH_WSREP
    if (WSREP(thd) && wsrep_is_active(thd) && is_real_trans && !error)
      wsrep_commit_empty(thd, all);
#endif /* WITH_WSREP */

    DBUG_RETURN(0);
  }

  DBUG_EXECUTE_IF("crash_commit_before", DBUG_SUICIDE(););

  /* Close all cursors that can not survive COMMIT */
  if (is_real_trans)                          /* not a statement commit */
    thd->stmt_map.close_transient_cursors();

  bool no_rollback;
  uint rw_ha_count= ha_check_and_coalesce_trx_read_only(thd, ha_info, all,
                                                        &no_rollback);
  /* rw_trans is TRUE when we in a transaction changing data */
  bool rw_trans= is_real_trans && rw_ha_count > 0;
  MDL_request mdl_backup;
  DBUG_PRINT("info", ("is_real_trans: %d  rw_trans:  %d  rw_ha_count: %d",
                      is_real_trans, rw_trans, rw_ha_count));

  /*
    backup_commit_lock may have already been set.
    This can happen in case of spider that does xa_commit() by
    calling ha_commit_trans() from spader_commit().
  */

  if ((rw_trans || no_rollback) && !thd->backup_commit_lock)
  {
    /*
      Acquire a metadata lock which will ensure that COMMIT is blocked
      by an active FLUSH TABLES WITH READ LOCK (and vice versa:
      COMMIT in progress blocks FTWRL).

      We allow the owner of FTWRL to COMMIT; we assume that it knows
      what it does.
    */
    MDL_REQUEST_INIT(&mdl_backup, MDL_key::BACKUP, "", "", MDL_BACKUP_COMMIT,
                     MDL_EXPLICIT);

    if (!WSREP(thd))
    {
      if (thd->mdl_context.acquire_lock(&mdl_backup,
                                        thd->variables.lock_wait_timeout))
      {
        my_error(ER_ERROR_DURING_COMMIT, MYF(0), 1);
        ha_rollback_trans(thd, all);
        DBUG_RETURN(1);
      }
      thd->backup_commit_lock= &mdl_backup;
    }
    DEBUG_SYNC(thd, "ha_commit_trans_after_acquire_commit_lock");
  }

  if (rw_trans && thd->is_read_only_ctx())
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--read-only");
    goto err;
  }

#if 1 // FIXME: This should be done in ha_prepare().
  if (rw_trans || (thd->lex->sql_command == SQLCOM_ALTER_TABLE &&
                   thd->lex->alter_info.flags & ALTER_ADD_SYSTEM_VERSIONING &&
                   is_real_trans))
  {
    ulonglong trx_start_id= 0, trx_end_id= 0;
    for (Ha_trx_info *ha_info= trans->ha_list; ha_info; ha_info= ha_info->next())
    {
      if (ha_info->ht()->prepare_commit_versioned)
      {
        trx_end_id= ha_info->ht()->prepare_commit_versioned(thd, &trx_start_id);

        if (trx_end_id == ULONGLONG_MAX)
        {
          my_error(ER_ERROR_DURING_COMMIT, MYF(0), 1);
          goto err;
        }

        if (trx_end_id)
          break; // FIXME: use a common ID for cross-engine transactions
      }
    }

    if (trx_end_id)
    {
      if (!TR_table::use_transaction_registry)
      {
        my_error(ER_VERS_TRT_IS_DISABLED, MYF(0));
        goto err;
      }
      DBUG_ASSERT(trx_start_id);
#ifdef WITH_WSREP
      bool saved_wsrep_on= thd->variables.wsrep_on;
      thd->variables.wsrep_on= false;
#endif
      TR_table trt(thd, true);
      if (trt.update(trx_start_id, trx_end_id))
      {
#ifdef WITH_WSREP
        thd->variables.wsrep_on= saved_wsrep_on;
#endif
        (void) trans_rollback_stmt(thd);
        goto err;
      }
      trt.table->file->extra(HA_EXTRA_RESET_STATE);
      // Here, the call will not commit inside InnoDB. It is only working
      // around closing thd->transaction.stmt open by TR_table::open().
      if (all)
        commit_one_phase_2(thd, false, &thd->transaction->stmt, false);
#ifdef WITH_WSREP
      thd->variables.wsrep_on= saved_wsrep_on;
#endif
    }
  }
#endif

  if (trans->no_2pc || (rw_ha_count <= 1))
  {
#ifdef WITH_WSREP
    /*
      This commit will not go through log_and_order() where wsrep commit
      ordering is normally done. Commit ordering must be done here.
    */
    if (run_wsrep_hooks)
    {
      // This commit involves storage engines that do not support two phases.
      // We allow read only transactions to such storage engines but not
      // read write transactions.
      if (trans->no_2pc && rw_ha_count > 1 && wsrep_have_no2pc_rw_ha(trans->ha_list))
      {
        // This commit involves more than one storage engine and requires
        // two phases, but some engines don't support it.
        // Issue a message to the client and roll back the transaction.

        // REPLACE|INSERT INTO ... SELECT uses TOI for MyISAM|Aria
        if (WSREP(thd) && thd->wsrep_cs().mode() != wsrep::client_state::m_toi)
        {
          my_message(ER_ERROR_DURING_COMMIT, "Transactional commit not supported "
                     "by involved engine(s)", MYF(0));
          error= 1;
        }
      }

      if (!error)
          error= wsrep_before_commit(thd, all);
    }
    if (error)
    {
      ha_rollback_trans(thd, FALSE);
      goto wsrep_err;
    }
#endif /* WITH_WSREP */
    error= ha_commit_one_phase(thd, all);
    if (error)
      goto err;
#ifdef WITH_WSREP
    // Here in case of error we must return 2 for inconsistency
    if (run_wsrep_hooks && !error)
      error= wsrep_after_commit(thd, all) ? 2 : 0;
#endif /* WITH_WSREP */
    goto done;
  }

  need_prepare_ordered= FALSE;
  need_commit_ordered= FALSE;

  for (Ha_trx_info *hi= ha_info; hi; hi= hi->next())
  {
    transaction_participant *ht= hi->ht();
    /*
      Do not call two-phase commit if this particular
      transaction is read-only. This allows for simpler
      implementation in engines that are always read-only.
    */
    if (! hi->is_trx_read_write())
      continue;
    /* We do not need to 2pc the binlog with the engine that implements it. */
    /* ToDo: This needs refinement, at least to handle the case when we are not binlogging. And maybe the logic could happen more elegantly in a different place, higher in the call stack? */
    if (ht == opt_binlog_engine_hton)
      continue;
    /*
      Sic: we know that prepare() is not NULL since otherwise
      trans->no_2pc would have been set.
    */
    if (unlikely(prepare_or_error(ht, thd, all)))
      goto err;

    need_prepare_ordered|= (ht->prepare_ordered != NULL);
    need_commit_ordered|= (ht->commit_ordered != NULL);
  }
  DEBUG_SYNC(thd, "ha_commit_trans_after_prepare");
  DBUG_EXECUTE_IF("crash_commit_after_prepare", DBUG_SUICIDE(););

  if (!is_real_trans)
  {
    error= commit_one_phase_2(thd, all, trans, is_real_trans);
    goto done;
  }

  DBUG_ASSERT(thd->transaction->implicit_xid.get_my_xid() ==
              thd->transaction->implicit_xid.quick_get_my_xid());
  DBUG_ASSERT(!thd->transaction->xid_state.is_explicit_XA() ||
              thd->lex->xa_opt == XA_ONE_PHASE);
  xid= thd->transaction->implicit_xid.quick_get_my_xid();

#ifdef WITH_WSREP
  if (run_wsrep_hooks && !error)
  {
    wsrep::seqno const s= wsrep_xid_seqno(thd->wsrep_xid);
    if (!s.is_undefined())
    {
      // xid was rewritten by wsrep
      xid= s.get();
    }
  }
  if (run_wsrep_hooks && (error = wsrep_before_commit(thd, all)))
    goto wsrep_err;
#endif /* WITH_WSREP */
  DEBUG_SYNC(thd, "ha_commit_trans_before_log_and_order");
  cookie= tc_log->log_and_order(thd, xid, all, need_prepare_ordered,
                                need_commit_ordered);
  if (!cookie)
  {
    WSREP_DEBUG("log_and_order has failed %llu %d", thd->thread_id, cookie);
    goto err;
  }
  DEBUG_SYNC(thd, "ha_commit_trans_after_log_and_order");
  DBUG_EXECUTE_IF("crash_commit_after_log", DBUG_SUICIDE(););

  error= commit_one_phase_2(thd, all, trans, is_real_trans) ? 2 : 0;
#ifdef WITH_WSREP
  if (run_wsrep_hooks &&
      (error || (error = wsrep_after_commit(thd, all))))
  {
    error = 2;
    mysql_mutex_lock(&thd->LOCK_thd_data);
    if (wsrep_must_abort(thd))
    {
      mysql_mutex_unlock(&thd->LOCK_thd_data);
      (void)tc_log->unlog(thd, cookie, xid);
      goto wsrep_err;
    }
    mysql_mutex_unlock(&thd->LOCK_thd_data);
  }
#endif /* WITH_WSREP */
  DBUG_EXECUTE_IF("crash_commit_before_unlog", DBUG_SUICIDE(););
  if (tc_log->unlog(thd, cookie, xid))
    error= 2;                                /* Error during commit */

done:
  if (is_real_trans)
  {
    MYSQL_COMMIT_TRANSACTION(thd->m_transaction_psi);
    thd->m_transaction_psi= NULL;
  }

  DBUG_EXECUTE_IF("crash_commit_after", DBUG_SUICIDE(););

  mysql_mutex_assert_not_owner(&LOCK_prepare_ordered);
  mysql_mutex_assert_not_owner(mysql_bin_log.get_log_lock());
  mysql_mutex_assert_not_owner(&LOCK_after_binlog_sync);
  mysql_mutex_assert_not_owner(&LOCK_commit_ordered);
#ifdef HAVE_REPLICATION
  repl_semisync_master.wait_after_commit(thd, all);
  DEBUG_SYNC(thd, "after_group_after_commit");
#endif
  goto end;

  /* Come here if error and we need to rollback. */
#ifdef WITH_WSREP
wsrep_err:
  mysql_mutex_lock(&thd->LOCK_thd_data);
  if (run_wsrep_hooks && wsrep_must_abort(thd))
  {
    WSREP_DEBUG("BF abort has happened after prepare & certify");
    mysql_mutex_unlock(&thd->LOCK_thd_data);
    ha_rollback_trans(thd, TRUE);
  }
  else
    mysql_mutex_unlock(&thd->LOCK_thd_data);

#endif /* WITH_WSREP */
err:
  error= 1;                                  /* Transaction was rolled back */
  /*
    In parallel replication, rollback is delayed, as there is extra replication
    book-keeping to be done before rolling back and allowing a conflicting
    transaction to continue (MDEV-7458).
  */
  if (!(thd->rgi_slave && thd->rgi_slave->is_parallel_exec))
    ha_rollback_trans(thd, all);
  else
  {
    /*
      We are not really doing a rollback here, but the code in trans_commit()
      requres that m_transaction_psi is 0 when we return from this function.
    */
    MYSQL_ROLLBACK_TRANSACTION(thd->m_transaction_psi);
    thd->m_transaction_psi= NULL;
    WSREP_DEBUG("rollback skipped %p %d",thd->rgi_slave,
                thd->rgi_slave->is_parallel_exec);
  }
end:
  // reset the pointer to the ticket when it's stack instantiated
  if (thd->backup_commit_lock == &mdl_backup)
  {
    /*
      We do not always immediately release transactional locks
      after ha_commit_trans() (see uses of ha_enable_transaction()),
      thus we release the commit blocker lock as soon as it's
      not needed.
     */
    if (mdl_backup.ticket)
      thd->mdl_context.release_lock(mdl_backup.ticket);
    thd->backup_commit_lock= 0;
  }
#ifdef WITH_WSREP
  if (wsrep_is_active(thd) && is_real_trans && !error &&
      (rw_ha_count == 0 || all) &&
      wsrep_not_committed(thd))
  {
    wsrep_commit_empty(thd, all);
  }
#endif /* WITH_WSREP */

  DBUG_RETURN(error);
}

/**
  @note
  This function does not care about global read lock or backup locks,
  the caller should.

  @param[in]  all  Is set in case of explicit commit
                   (COMMIT statement), or implicit commit
                   issued by DDL. Is not set when called
                   at the end of statement, even if
                   autocommit=1.
*/

int ha_commit_one_phase(THD *thd, bool all)
{
  THD_TRANS *trans=all ? &thd->transaction->all : &thd->transaction->stmt;
  /*
    "real" is a nick name for a transaction for which a commit will
    make persistent changes. E.g. a 'stmt' transaction inside a 'all'
    transaction is not 'real': even though it's possible to commit it,
    the changes are not durable as they might be rolled back if the
    enclosing 'all' transaction is rolled back.
    We establish the value of 'is_real_trans' by checking
    if it's an explicit COMMIT/BEGIN statement, or implicit
    commit issued by DDL (all == TRUE), or if we're running
    in autocommit mode (it's only in the autocommit mode
    ha_commit_one_phase() can be called with an empty
    transaction.all.ha_list, see why in trans_register_ha()).
  */
  bool is_real_trans= ((all || thd->transaction->all.ha_list == 0) &&
                       !(thd->variables.option_bits & OPTION_GTID_BEGIN));
  int res;
  DBUG_ENTER("ha_commit_one_phase");
  if (is_real_trans)
  {
    DEBUG_SYNC(thd, "ha_commit_one_phase");
    if ((res= thd->wait_for_prior_commit()))
      DBUG_RETURN(res);
  }
  res= commit_one_phase_2(thd, all, trans, is_real_trans);
  DBUG_RETURN(res);
}

static bool is_ro_1pc_trans(THD *thd, Ha_trx_info *ha_info, bool all,
                            bool is_real_trans)
{
  bool no_rollback;
  uint rw_ha_count= ha_check_and_coalesce_trx_read_only(thd, ha_info, all,
                                                        &no_rollback);
  bool rw_trans= is_real_trans &&
    (rw_ha_count > (thd->is_current_stmt_binlog_disabled()?0U:1U));

  return !rw_trans;
}

static bool has_binlog_hton(Ha_trx_info *ha_info)
{
  bool rc;
  for (rc= false; ha_info && !rc; ha_info= ha_info->next())
    rc= ha_info->ht() == &binlog_tp;

  return rc;
}

static int
commit_one_phase_2(THD *thd, bool all, THD_TRANS *trans, bool is_real_trans)
{
  int error= 0;
  uint count= 0;
  Ha_trx_info *ha_info= trans->ha_list, *ha_info_next;
  DBUG_ENTER("commit_one_phase_2");
  if (is_real_trans)
    DEBUG_SYNC(thd, "commit_one_phase_2");

  if (ha_info)
  {
    int err= 0;

    bool is_binlogged= has_binlog_hton(ha_info);
    if (is_binlogged)
    {
      if ((err= binlog_commit(thd, all, is_ro_1pc_trans(thd, ha_info, all,
                                                        is_real_trans))))
      {
        my_error(ER_ERROR_DURING_COMMIT, MYF(0), err);
        error= 1;
        goto err;
      }
    }
#ifdef WITH_WSREP
    else
    {
      if (wsrep_on(thd))
        error= thd->binlog_flush_pending_rows_event(TRUE);
    }
#endif

    for (; ha_info; ha_info= ha_info_next)
    {
      transaction_participant *ht= ha_info->ht();
      if ((err= ht->commit(thd, all)))
      {
        my_error(ER_ERROR_DURING_COMMIT, MYF(0), err);
        error=1;
      }
      /* Should this be done only if is_real_trans is set ? */
      status_var_increment(thd->status_var.ha_commit_count);
      if (is_real_trans && ht != &binlog_tp && ha_info->is_trx_read_write())
        ++count;
      ha_info_next= ha_info->next();
      ha_info->reset(); /* keep it conveniently zero-filled */
    }
    DEBUG_SYNC(thd, "commit_handlerton_after");
    if (is_binlogged && is_real_trans)
      binlog_post_commit(thd, all);
    trans->ha_list= 0;
    trans->no_2pc=0;
    if (all)
    {
      if (thd->transaction->changed_tables)
        query_cache.invalidate(thd, thd->transaction->changed_tables);
    }
  }

  /* Free resources and perform other cleanup even for 'empty' transactions. */
  if (is_real_trans)
  {
    thd->has_waiter= false;
    thd->transaction->cleanup();
    if (count >= 2)
      statistic_increment(transactions_multi_engine, LOCK_status);
  }
 err:
  DBUG_RETURN(error);
}


int ha_rollback_trans(THD *thd, bool all)
{
  int error=0;
  THD_TRANS *trans=all ? &thd->transaction->all : &thd->transaction->stmt;
  Ha_trx_info *ha_info= trans->ha_list, *ha_info_next;
  /*
    "real" is a nick name for a transaction for which a commit will
    make persistent changes. E.g. a 'stmt' transaction inside a 'all'
    transaction is not 'real': even though it's possible to commit it,
    the changes are not durable as they might be rolled back if the
    enclosing 'all' transaction is rolled back.
    We establish the value of 'is_real_trans' by checking
    if it's an explicit COMMIT or BEGIN statement, or implicit
    commit issued by DDL (in these cases all == TRUE),
    or if we're running in autocommit mode (it's only in the autocommit mode
    ha_commit_one_phase() is called with an empty
    transaction.all.ha_list, see why in trans_register_ha()).
  */
  bool is_real_trans=all || thd->transaction->all.ha_list == 0;
  DBUG_ENTER("ha_rollback_trans");

  /*
    We must not rollback the normal transaction if a statement
    transaction is pending.
  */
  DBUG_ASSERT(thd->transaction->stmt.ha_list == NULL ||
              trans == &thd->transaction->stmt);

#ifdef HAVE_REPLICATION
  if (is_real_trans)
  {
    /*
      In parallel replication, if we need to rollback during commit, we must
      first inform following transactions that we are going to abort our commit
      attempt. Otherwise those following transactions can run too early, and
      possibly cause replication to fail. See comments in retry_event_group().

      (This concerns rollbacks due to temporary errors where the transaction
      will be retried afterwards. For non-recoverable errors, following
      transactions will not start but just be skipped as the worker threads
      perform the error stop).

      There were several bugs with this in the past that were very hard to
      track down (MDEV-7458, MDEV-8302). So we add here an assertion for
      rollback without signalling following transactions. And in release
      builds, we explicitly do the signalling before rolling back.
    */
    DBUG_ASSERT(
        !(thd->rgi_slave &&
          !thd->rgi_slave->worker_error &&
          thd->rgi_slave->did_mark_start_commit) ||
        (thd->transaction->xid_state.is_explicit_XA() ||
         (thd->rgi_slave->gtid_ev_flags2 & Gtid_log_event::FL_PREPARED_XA)));

    if (thd->rgi_slave &&
        !thd->rgi_slave->worker_error &&
        thd->rgi_slave->did_mark_start_commit)
      thd->rgi_slave->unmark_start_commit();
  }
#endif

  if (thd->in_sub_stmt)
  {
    DBUG_ASSERT(0);
    /*
      If we are inside stored function or trigger we should not commit or
      rollback current statement transaction. See comment in ha_commit_trans()
      call for more information.
    */
    if (!all)
      DBUG_RETURN(0);
    my_error(ER_COMMIT_NOT_ALLOWED_IN_SF_OR_TRG, MYF(0));
    DBUG_RETURN(1);
  }

#ifdef WITH_WSREP
  // REPLACE|INSERT INTO ... SELECT uses TOI in consistency check
  if (thd->wsrep_consistency_check != CONSISTENCY_CHECK_RUNNING)
    if (thd->wsrep_cs().mode() != wsrep::client_state::m_toi)
      (void) wsrep_before_rollback(thd, all);
#endif /* WITH_WSREP */

  bool do_binlog= false;
  if (ha_info)
  {
    /* Close all cursors that can not survive ROLLBACK */
    if (is_real_trans)                          /* not a statement commit */
      thd->stmt_map.close_transient_cursors();

    for (; ha_info; ha_info= ha_info_next)
    {
      int err;
      transaction_participant *ht= ha_info->ht();
      do_binlog|= (ht == &binlog_tp);
      if ((err= ht->rollback(thd, all)))
      {
        // cannot happen
        my_error(ER_ERROR_DURING_ROLLBACK, MYF(0), err);
        error=1;
#ifdef WITH_WSREP
        if (WSREP(thd))
        {
          WSREP_WARN("handlerton rollback failed, thd %lld %lld "
                     "conf %d wsrep_err %s SQL %s",
                     thd->thread_id, thd->query_id, thd->wsrep_trx().state(),
                     wsrep::to_c_string(thd->wsrep_cs().current_error()),
                     wsrep_thd_query(thd));
        }
#endif /* WITH_WSREP */
      }
      DEBUG_SYNC(thd, "rollback_handlerton_after");
      status_var_increment(thd->status_var.ha_rollback_count);
      ha_info_next= ha_info->next();
      ha_info->reset(); /* keep it conveniently zero-filled */
    }
    trans->ha_list= 0;
    trans->no_2pc=0;
  }

  if (do_binlog)
    binlog_post_rollback(thd, all);

#ifdef WITH_WSREP
  if (WSREP(thd) && thd->is_error())
  {
    WSREP_DEBUG("ha_rollback_trans(%lld, %s) rolled back: msg %s is_real %d wsrep_err %s",
                thd->thread_id, all ? "TRUE" : "FALSE",
                thd->get_stmt_da()->message(), is_real_trans,
                wsrep::to_c_string(thd->wsrep_cs().current_error()));
  }

  // REPLACE|INSERT INTO ... SELECT uses TOI in consistency check
  if (thd->wsrep_consistency_check != CONSISTENCY_CHECK_RUNNING)
    if (thd->wsrep_cs().mode() != wsrep::client_state::m_toi)
      (void) wsrep_after_rollback(thd, all);
#endif /* WITH_WSREP */

  if (all || !thd->in_active_multi_stmt_transaction())
  {
    MYSQL_ROLLBACK_TRANSACTION(thd->m_transaction_psi);
    thd->m_transaction_psi= NULL;
  }

  /* Always cleanup. Even if nht==0. There may be savepoints. */
  if (is_real_trans)
  {
    /*
      Thanks to possibility of MDL deadlock rollback request can come even if
      transaction hasn't been started in any transactional storage engine.
    */
    if (thd->transaction_rollback_request &&
        thd->transaction->xid_state.is_explicit_XA())
      thd->transaction->xid_state.set_error(thd->get_stmt_da()->sql_errno());

    thd->has_waiter= false;
    thd->transaction->cleanup();
  }
  if (all)
    thd->transaction_rollback_request= FALSE;

  /*
    If a non-transactional table was updated, warn; don't warn if this is a
    slave thread (because when a slave thread executes a ROLLBACK, it has
    been read from the binary log, so it's 100% sure and normal to produce
    error ER_WARNING_NOT_COMPLETE_ROLLBACK. If we sent the warning to the
    slave SQL thread, it would not stop the thread but just be printed in
    the error log; but we don't want users to wonder why they have this
    message in the error log, so we don't send it.

    We don't have to test for thd->killed == KILL_SYSTEM_THREAD as
    it doesn't matter if a warning is pushed to a system thread or not:
    No one will see it...
  */
  if (is_real_trans && thd->transaction->all.modified_non_trans_table &&
      !thd->slave_thread && thd->killed < KILL_CONNECTION)
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_WARNING_NOT_COMPLETE_ROLLBACK,
                 ER_THD(thd, ER_WARNING_NOT_COMPLETE_ROLLBACK));
#ifdef HAVE_REPLICATION
  repl_semisync_master.wait_after_rollback(thd, all);
#endif
  DBUG_RETURN(error);
}


struct xahton_st {
  XID *xid;
  int result;
};

static bool xacommit_handlerton(THD *thd, transaction_participant *hton, void *arg)
{
  if (hton->recover)
  {
    hton->commit_by_xid(((struct xahton_st *)arg)->xid);
    ((struct xahton_st *)arg)->result= 0;
  }
  return FALSE;
}

static bool xarollback_handlerton(THD *, transaction_participant *hton, void *arg)
{
  if (hton->recover)
  {
    hton->rollback_by_xid(((struct xahton_st *)arg)->xid);
    ((struct xahton_st *)arg)->result= 0;
  }
  return FALSE;
}


int ha_commit_or_rollback_by_xid(XID *xid, bool commit)
{
  struct xahton_st xaop;
  xaop.xid= xid;
  xaop.result= 1;

  /*
    When the binlogging service is enabled complete the transaction
    by it first.
  */
  if (commit)
    binlog_commit_by_xid(xid);
  else
    binlog_rollback_by_xid(xid);

  tp_foreach(NULL, commit ? xacommit_handlerton : xarollback_handlerton, &xaop);

  if (commit)
    DEBUG_SYNC(current_thd, "xacommit_handlerton_after");
  else
    DEBUG_SYNC(current_thd, "xarollback_handlerton_after");

  if (commit)
    binlog_post_commit_by_xid(xid);
  else
    binlog_post_rollback_by_xid(xid);
  return xaop.result;
}


#ifndef DBUG_OFF
/** Converts XID to string.

@param[out] buf output buffer
@param[in] xid XID to convert

@return pointer to converted string

@note This does not need to be multi-byte safe or anything */
static char *xid_to_str(char *buf, const XID &xid)
{
  int i;
  char *s=buf;
  *s++='\'';
  for (i= 0; i < xid.gtrid_length + xid.bqual_length; i++)
  {
    uchar c= (uchar) xid.data[i];
    /* is_next_dig is set if next character is a number */
    bool is_next_dig= FALSE;
    if (i < XIDDATASIZE)
    {
      char ch= xid.data[i + 1];
      is_next_dig= (ch >= '0' && ch <='9');
    }
    if (i == xid.gtrid_length)
    {
      *s++='\'';
      if (xid.bqual_length)
      {
        *s++='.';
        *s++='\'';
      }
    }
    if (c < 32 || c > 126)
    {
      *s++='\\';
      /*
        If next character is a number, write current character with
        3 octal numbers to ensure that the next number is not seen
        as part of the octal number
      */
      if (c > 077 || is_next_dig)
        *s++=_dig_vec_lower[c >> 6];
      if (c > 007 || is_next_dig)
        *s++=_dig_vec_lower[(c >> 3) & 7];
      *s++=_dig_vec_lower[c & 7];
    }
    else
    {
      if (c == '\'' || c == '\\')
        *s++='\\';
      *s++=c;
    }
  }
  *s++='\'';
  *s=0;
  return buf;
}
#endif

static my_xid wsrep_order_and_check_continuity(XID *list, int len)
{
#ifdef WITH_WSREP
  wsrep_sort_xid_array(list, len);
  wsrep::gtid cur_position= wsrep_get_SE_checkpoint<wsrep::gtid>();
  long long cur_seqno= cur_position.seqno().get();
  for (int i= 0; i < len; ++i)
  {
    if (!wsrep_is_wsrep_xid(list + i) ||
        wsrep_xid_seqno(list + i) != cur_seqno + 1)
    {
      WSREP_WARN("Discovered discontinuity in recovered wsrep "
                 "transaction XIDs. Truncating the recovery list to "
                 "%d entries", i);
      break;
    }
    ++cur_seqno;
  }
  WSREP_INFO("Last wsrep seqno to be recovered %lld", cur_seqno);
  return (cur_seqno < 0 ? 0 : cur_seqno);
#else
  return 0;
#endif /* WITH_WSREP */
}
/**
  recover() step of xa.

  @note
    there are four modes of operation:
    - automatic recover after a crash
    in this case commit_list != 0, tc_heuristic_recover==0
    all xids from commit_list are committed, others are rolled back
    - manual (heuristic) recover
    in this case commit_list==0, tc_heuristic_recover != 0
    DBA has explicitly specified that all prepared transactions should
    be committed (or rolled back).
    - no recovery (MySQL did not detect a crash)
    in this case commit_list==0, tc_heuristic_recover == 0
    there should be no prepared transactions in this case.
    - automatic recovery for the semisync slave server: uncommitted
    transactions are rolled back and when they are in binlog it gets
    truncated to the first uncommitted transaction start offset.
*/
struct xarecover_st
{
  int len, found_foreign_xids, found_my_xids;
  XID *list;
  HASH *commit_list;
  bool dry_run;
  MEM_ROOT *mem_root;
  bool error;
};


/**
   Recovery for XID (internal 2pc and user XA) using engine-implemented binlog.

   The binlog provides the state of each XID - prepared, committed, rolled
   back. For prepared XA, it also provides the count of the number of engines
   participating in that transaction.

   Each XID found prepared in an engine will be committed, rolled back, or left
   in prepared state according to the state of the binlog. For an XID in the
   prepared state, if the number of engines found having that XID is too
   small, it means the server crashed in the middle of preparing a multi-
   engine transaction, and that XID will be rolled back both in engines and
   in the binlog.
*/
struct xarecover_engine_binlog
{
  static constexpr uint32_t MAX_HTONS= 32;

  /* Buffer for engines to return their prepared XID into. */
  XID *list;
  /* Hash (of handler_binlog_xid_info) of binlog state of XIDs. */
  HASH *xid_hash;
  /*
    Engine handlertons involved in XID recovery, used for bits in
    handler_binlog_xid_info::engine_map.
  */
  handlerton *htons[MAX_HTONS];
  /* Used entries in htons. */
  uint32_t num_htons;
  /* Size of the XID *list. */
  int len;
  /* Set in case of any error during the processing. */
  bool error;
};


/**
  Inserts a new hash member.

  returns a successfully created and inserted @c xid_recovery_member
           into hash @c hash_arg,
           or NULL.
*/
static xid_recovery_member*
xid_member_insert(HASH *hash_arg, my_xid xid_arg, MEM_ROOT *ptr_mem_root,
                  XID *full_xid_arg, decltype(::server_id) server_id_arg)
{
  xid_recovery_member *member= (xid_recovery_member *)
    alloc_root(ptr_mem_root, sizeof(xid_recovery_member));
  XID *xid_full= NULL;

  if (full_xid_arg)
    xid_full= (XID*) alloc_root(ptr_mem_root, sizeof(XID));

  if (!member || (full_xid_arg && !xid_full))
    return NULL;

  if (full_xid_arg)
    *xid_full= *full_xid_arg;
  *member= xid_recovery_member(xid_arg, 1, false, xid_full, server_id_arg);

  return
    my_hash_insert(hash_arg, (uchar*) member) ? NULL : member;
}

/*
  Inserts a new or updates an existing hash member to increment
  the member's prepare counter.

  returns false  on success,
           true   otherwise.
*/
static bool xid_member_replace(HASH *hash_arg, my_xid xid_arg,
                               MEM_ROOT *ptr_mem_root,
                               XID *full_xid_arg,
                               decltype(::server_id) server_id_arg)
{
  xid_recovery_member* member;
  if ((member= (xid_recovery_member *)
       my_hash_search(hash_arg, (uchar *)& xid_arg, sizeof(xid_arg))))
    member->in_engine_prepare++;
  else
    member= xid_member_insert(hash_arg, xid_arg, ptr_mem_root, full_xid_arg,  server_id_arg);

  return member == NULL;
}


static bool
record_hton_for_xid(xarecover_engine_binlog *info, handler_binlog_xid_info *rec,
                    handlerton *hton)
{
  uint32_t idx;
  for (idx= 0; idx < info->num_htons; ++idx)
  {
    if (info->htons[idx] == hton)
    {
      rec->engine_map|= 1<<idx;
      return false;
    }
  }
  if (info->num_htons >= xarecover_engine_binlog::MAX_HTONS)
  {
    sql_print_error("Too many transactional engines during binlog recovery "
                    "of prepared transactions (max is %u)",
                    (uint)xarecover_engine_binlog::MAX_HTONS);
    return true;
  }
  rec->engine_map|= 1<<info->num_htons;
  info->htons[info->num_htons++]= hton;
  return false;
}


static my_bool xarecover_engine_binlog(THD *unused, plugin_ref plugin,
                                       void *arg)
{
  handlerton *hton= plugin_hton(plugin);
  struct xarecover_engine_binlog *info=
    (struct xarecover_engine_binlog *) arg;
  int got;

  if (hton->recover)
  {
    while ((got= hton->recover(info->list, info->len)) > 0 )
    {
      sql_print_information("Found %d prepared transaction(s) in %s",
                            got, hton_name(hton)->str);

      for (int i=0; i < got; i ++)
      {
        XID *xid= &info->list[i];
        const uchar *key_ptr= xid->key();
        size_t key_len= xid->key_length();
        handler_binlog_xid_info *rec= (handler_binlog_xid_info *)
          my_hash_search(info->xid_hash, key_ptr, key_len);

        /* If the binlog says to roll back, or says nothing, then roll back. */
        if (!rec || rec->xid_state == handler_binlog_xid_info::BINLOG_ROLLBACK)
        {
          if (hton->rollback_by_xid(info->list+i))
            info->error= true;
          continue;
        }

        /* If the binlog says to commit, or says nothing, then commit. */
        if (rec->xid_state == handler_binlog_xid_info::BINLOG_COMMIT)
        {
          if (hton->commit_by_xid(xid))
            info->error= true;
          continue;
        }
        DBUG_ASSERT(rec->xid_state == handler_binlog_xid_info::BINLOG_PREPARE);

        /*
          If the binlog has the transaction in the prepared state, then we
          must check if all involved engines have it prepared as well. We might
          have crashed before all engines had time to (durably) prepare, in
          which case we will roll back the ones that did.
          So we record in the info->xid_hash that we found the XID in this
          engine, and at the end we then check whether to commit or roll back.
        */
        DBUG_ASSERT(rec->engine_count > 0);
        if (likely(rec->engine_count > 0))
          --rec->engine_count;
        if (record_hton_for_xid(info, rec, hton))
          info->error= true;
      }
      if (got < info->len)
        break;
    }
  }
  return FALSE;
}


int
ha_recover_engine_binlog(HASH *xid_hash)
{
  DBUG_ENTER("ha_recover_engine_binlog");
  DBUG_ASSERT(opt_binlog_engine_hton);
  struct xarecover_engine_binlog info;
  info.xid_hash= xid_hash;
  info.num_htons= 0;
  info.error= false;
  info.list= nullptr;

  sql_print_information("Starting recovery of prepared transactions...");

  for (info.len= MAX_XID_LIST_SIZE; info.len >= MIN_XID_LIST_SIZE; info.len/=2)
  {
    info.list=(XID *)my_malloc(key_memory_XID, info.len*sizeof(XID), MYF(0));
    if (likely(info.list))
      break;
  }
  if (!info.list)
  {
    sql_print_error(ER(ER_OUTOFMEMORY),
                    static_cast<int>(info.len*sizeof(XID)));
    DBUG_RETURN(1);
  }

  plugin_foreach(NULL, xarecover_engine_binlog,
                 MYSQL_STORAGE_ENGINE_PLUGIN, &info);

  my_free(info.list);

  if (info.error)
    DBUG_RETURN(1);

  /*
    Now handle any XID found in the prepared state in binlog. They will be
    left prepared if all engines that participated in the transaction managed
    to prepare them durably before the server restart; otherwise they will be
    rolled back in binlog and engines (if any).
  */
  for (uint32 i= 0; i < xid_hash->records; ++i)
  {
    handler_binlog_xid_info *rec= (handler_binlog_xid_info *)
      my_hash_element(xid_hash, i);
    if (rec->xid_state != handler_binlog_xid_info::BINLOG_PREPARE)
      continue;
    if (rec->engine_count == 0)
    {
      /* Recover the XID as a prepared XA transaction. */
      xid_cache_insert(&rec->xid);
    }
    else
    {
      /* Not all participating engines prepared, so roll back. */
      void *engine_data= nullptr;
      mysql_mutex_lock(&LOCK_commit_ordered);
      (*opt_binlog_engine_hton->binlog_xa_rollback_ordered)
        (current_thd, &rec->xid, &engine_data);
      mysql_mutex_unlock(&LOCK_commit_ordered);
      (*opt_binlog_engine_hton->binlog_xa_rollback)
        (current_thd, &rec->xid, &engine_data);
      for (uint32_t j= 0; j < info.num_htons; ++j)
      {
        if (rec->engine_map & (1<<j)) {
          handlerton *hton= info.htons[j];
          (*hton->rollback_by_xid)(&rec->xid);
        }
      }
      (*opt_binlog_engine_hton->binlog_unlog)(&rec->xid, &engine_data);
      (*opt_binlog_engine_hton->binlog_oob_free)(engine_data);
    }
  }

  sql_print_information("Recovery of prepared transaction finished.");
  DBUG_RETURN(0);
}


/*
  A "transport" type for recovery completion with ha_recover_complete()
*/
struct xarecover_complete_arg
{
  xid_recovery_member* member;
  Binlog_offset *binlog_coord;
  uint count;
};

/*
  Flagged to commit member confirms to get committed.
  Otherwise when
    A. ptr_commit_max is NULL (implies the normal recovery), or
    B. it's not NULL (can only be so in the semisync slave case)
       and the value referenced is not greater than the member's coordinate
       the decision is to rollback.
  When both A,B do not hold - which is the semisync slave recovery
  case - the decision is to commit.

  Returns  true  as commmit decision
           false as rollback one
*/
static bool xarecover_decide_to_commit(xid_recovery_member* member,
                                       Binlog_offset *ptr_commit_max)
{
  return
    member->decided_to_commit ? true :
    !ptr_commit_max ? false :
    (member->binlog_coord < *ptr_commit_max ?  // semisync slave recovery
     true : false);
}

/*
  Helper function for xarecover_do_commit_or_rollback_handlerton.
  For a given hton decides what to do with a xid passed in the 2nd arg
  and carries out the decision.
*/
static void xarecover_do_commit_or_rollback(transaction_participant *hton,
                                            xarecover_complete_arg *arg)
{
  XA_data x;
  my_bool rc;
  xid_recovery_member *member= arg->member;
  Binlog_offset *ptr_commit_max= arg->binlog_coord;

  if (!member->full_xid)
    // Populate xid using the server_id from original transaction
    x.set(member->xid, member->server_id);
  else
    x= *member->full_xid;

  if (xarecover_decide_to_commit(member, ptr_commit_max))
    rc= hton->commit_by_xid(&x);
  else if (hton->recover_rollback_by_xid &&
           IF_WSREP(!(WSREP_ON || wsrep_recovery), true))
    rc= hton->recover_rollback_by_xid(&x);
  else
    rc= hton->rollback_by_xid(&x);

  /*
    It's fine to have non-zero rc which would be from transaction
    non-participant hton:s.
  */
  DBUG_ASSERT(rc || member->in_engine_prepare > 0);

  if (!rc)
  {
    /*
      This block relies on Engine to report XAER_NOTA at
      "complete"_by_xid for unknown xid.
    */
    member->in_engine_prepare--;
    if (global_system_variables.log_warnings > 2)
      sql_print_information("%s transaction with xid %llu",
                            member->decided_to_commit ? "Committed" :
                            "Rolled back", (ulonglong) member->xid);
  }
}

/*
  Per hton recovery decider function.
*/
static bool xarecover_do_commit_or_rollback_handlerton(THD *,
                                      transaction_participant *hton, void *arg)
{
  if (hton->recover)
    xarecover_do_commit_or_rollback(hton, (xarecover_complete_arg *) arg);

  return FALSE;
}

/*
  Completes binlog recovery for an input xid in the passed
  member_arg to invoke decider functions for each handlerton.

  Returns always FALSE.
*/
static my_bool xarecover_complete_and_count(void *member_arg, void *param_arg)
{
  xid_recovery_member *member= (xid_recovery_member*) member_arg;
  xarecover_complete_arg *complete_params=
    (xarecover_complete_arg*) param_arg;
  complete_params->member= member;

  tp_foreach(NULL, xarecover_do_commit_or_rollback_handlerton, complete_params);

  if (member->in_engine_prepare)
  {
    complete_params->count++;
    if (global_system_variables.log_warnings > 2)
      sql_print_warning("Found prepared transaction with xid %llu",
                        (ulonglong) member->xid);
  }

  return false;
}

static my_bool tc_log_recover_done_handlerton(THD*, plugin_ref plugin, void *arg)
{
  handlerton *hton= plugin_hton(plugin);

  if (hton->signal_tc_log_recovery_done)
    hton->signal_tc_log_recovery_done();
  return false;
}

void ha_signal_tc_log_recovery_done()
{
  std::ignore = plugin_foreach(nullptr, tc_log_recover_done_handlerton,
                               MYSQL_STORAGE_ENGINE_PLUGIN, 0);
}

/*
  Completes binlog recovery to invoke decider functions for
  each xid.
  Returns the number of transactions remained doubtful.
*/
uint ha_recover_complete(HASH *commit_list, Binlog_offset *coord)
{
  xarecover_complete_arg complete= { NULL, coord, 0 };
  (void) my_hash_iterate(commit_list, xarecover_complete_and_count, &complete);

  return complete.count;
}

static bool xarecover_handlerton(THD *, transaction_participant *hton, void *arg)
{
  struct xarecover_st *info= (struct xarecover_st *) arg;
  int got;

  if (hton->recover)
  {
    while ((got= hton->recover(info->list, info->len)) > 0 )
    {
      sql_print_information("Found %d prepared transaction(s) in %s",
                            got, hton_name(hton)->str);
      /* If wsrep_on=ON, XIDs are first ordered and then the range of
         recovered XIDs is checked for continuity. All the XIDs which
         are in continuous range can be safely committed if binlog
         is off since they have already ordered and certified in the
         cluster.

         The discontinuity of wsrep XIDs may happen because the GTID
         is assigned for transaction in wsrep_before_prepare(), but the
         commit order is entered in wsrep_before_commit(). This means that
         transactions may run prepare step out of order and may
         result in gap in wsrep XIDs. This can be the case for example
         if we have T1 with seqno 1 and T2 with seqno 2 and the server
         crashes after T2 finishes prepare step but before T1 starts
         the prepare.
      */
      my_xid wsrep_limit __attribute__((unused))= 0;

      /* Note that we could call this for binlog also that
         will not have WSREP(thd) but global wsrep on might
         be true.
      */
      if (WSREP_ON)
        wsrep_limit= wsrep_order_and_check_continuity(info->list, got);

      for (int i=0; i < got; i ++)
      {
        my_xid x= info->list[i].get_my_xid();
        bool is_server_xid= x > 0;

#ifdef WITH_WSREP
        if (!is_server_xid && wsrep_is_wsrep_xid(&info->list[i]))
          x= wsrep_xid_seqno(&info->list[i]);
#endif
        if (!x) // not "mine" - that is generated by external TM
        {
          DBUG_EXECUTE("info",{
            char buf[XIDDATASIZE*4+6];
            _db_doprnt_("ignore xid %s", xid_to_str(buf, info->list[i]));
            });
          xid_cache_insert(info->list + i);
          info->found_foreign_xids++;
          continue;
        }
        if (IF_WSREP(!(wsrep_emulate_bin_log &&
                       wsrep_is_wsrep_xid(info->list + i) &&
                       x <= wsrep_limit) && info->dry_run,
                     info->dry_run))
        {
          info->found_my_xids++;
          continue;
        }

        /*
          Regular and semisync slave server recovery only collects
          xids to make decisions on them later by the caller.
        */
        if (info->mem_root)
        {
          // remember "full" xid too when it's not in mysql format.
          // Also record the transaction's original server_id. It will be used for
          // populating the input XID to be searched in hash.
          if (xid_member_replace(info->commit_list, x, info->mem_root,
                                 is_server_xid? NULL : &info->list[i],
                                 is_server_xid? info->list[i].get_trx_server_id() : server_id))
          {
            info->error= true;
            sql_print_error("Error in memory allocation at xarecover_handlerton");
            break;
          }
        }
        if (IF_WSREP((wsrep_emulate_bin_log &&
                      wsrep_is_wsrep_xid(info->list + i) &&
                      !wsrep_is_xid_gtid_undefined(info->list + i) &&
                      x <= wsrep_limit), false) ||
            tc_heuristic_recover == TC_HEURISTIC_RECOVER_COMMIT)
        {
          int rc= hton->commit_by_xid(info->list+i);
          if (rc == 0)
          {
            DBUG_EXECUTE("info",{
              char buf[XIDDATASIZE*4+6];
              _db_doprnt_("commit xid %s", xid_to_str(buf, info->list[i]));
              });
          }
        }
        else if (!info->mem_root)
        {
          int rc= hton->rollback_by_xid(info->list+i);
          if (rc == 0)
          {
            DBUG_EXECUTE("info",{
              char buf[XIDDATASIZE*4+6];
              _db_doprnt_("rollback xid %s", xid_to_str(buf, info->list[i]));
              });
          }
        }
      }
      if (got < info->len)
        break;
    }
  }
  return FALSE;
}


int ha_recover(HASH *commit_list, MEM_ROOT *arg_mem_root)
{
  struct xarecover_st info;
  DBUG_ENTER("ha_recover");
  info.found_foreign_xids= info.found_my_xids= 0;
  info.commit_list= commit_list;
  info.dry_run= (info.commit_list==0 && tc_heuristic_recover==0);
  info.list= NULL;
  info.mem_root= arg_mem_root;
  info.error= false;

  if (opt_binlog_engine_hton)
  {
    if (tc_heuristic_recover)
    {
      sql_print_error("The --tc-heuristic-recover option is not needed with, "
                      "and cannot  be used with --binlog-storage-engine");
      DBUG_RETURN(1);
    }
    /*
      With engine-implemented binlog, recovery is handled during binlog
      open, calling into ha_recover_engine_binlog().
    */
    DBUG_ASSERT(!arg_mem_root);
    DBUG_RETURN(0);
  }

  /* commit_list and tc_heuristic_recover cannot be set both */
  DBUG_ASSERT(info.commit_list==0 || tc_heuristic_recover==0);
  /* if either is set, total_ha_2pc must be set too */
  DBUG_ASSERT(info.dry_run ||
              (failed_ha_2pc + total_ha_2pc) > (ulong)opt_bin_log);

  if (total_ha_2pc <= (ulong)opt_bin_log)
    DBUG_RETURN(0);

  if (info.commit_list)
    sql_print_information("Starting table crash recovery...");

  for (info.len= MAX_XID_LIST_SIZE; info.len >= MIN_XID_LIST_SIZE; info.len/=2)
  {
    DBUG_EXECUTE_IF("min_xa_len", info.len = 16;);
    info.list=(XID *)my_malloc(key_memory_XID, info.len*sizeof(XID), MYF(0));
    if (likely(info.list))
      break;
  }
  if (!info.list)
  {
    sql_print_error(ER(ER_OUTOFMEMORY),
                    static_cast<int>(info.len*sizeof(XID)));
    DBUG_RETURN(1);
  }

  tp_foreach(NULL, xarecover_handlerton, &info);

  my_free(info.list);
  if (info.found_foreign_xids)
    sql_print_warning("Found %d prepared XA transactions", 
                      info.found_foreign_xids);
  if (info.dry_run && info.found_my_xids)
  {
    sql_print_error("Found %d prepared transactions! It means that server was "
                    "not shut down properly last time and critical recovery "
                    "information (last binlog or %s file) was manually deleted "
                    "after a crash. You have to start server with "
                    "--tc-heuristic-recover switch to commit or rollback "
                    "pending transactions.",
                    info.found_my_xids, opt_tc_log_file);
    DBUG_RETURN(1);
  }
  if (info.error)
    DBUG_RETURN(1);

  if (info.commit_list)
    sql_print_information("Crash table recovery finished.");
  DBUG_RETURN(0);
}


/*
  Called by engine to notify TC that a new commit checkpoint has been reached.
  See comments on handlerton method commit_checkpoint_request() for details.
*/
void commit_checkpoint_notify_ha(void *cookie)
{
  tc_log->commit_checkpoint_notify(cookie);
}


/**
  Check if all storage engines used in transaction agree that after
  rollback to savepoint it is safe to release MDL locks acquired after
  savepoint creation.

  @param thd   The client thread that executes the transaction.

  @return true  - It is safe to release MDL locks.
          false - If it is not.
*/
bool ha_rollback_to_savepoint_can_release_mdl(THD *thd)
{
  Ha_trx_info *ha_info;
  THD_TRANS *trans= (thd->in_sub_stmt ? &thd->transaction->stmt :
                                        &thd->transaction->all);

  DBUG_ENTER("ha_rollback_to_savepoint_can_release_mdl");

  /**
    Checking whether it is safe to release metadata locks after rollback to
    savepoint in all the storage engines that are part of the transaction.
  */
  for (ha_info= trans->ha_list; ha_info; ha_info= ha_info->next())
  {
    transaction_participant *ht= ha_info->ht();
    DBUG_ASSERT(ht);

    if (ht->savepoint_rollback_can_release_mdl == 0 ||
        ht->savepoint_rollback_can_release_mdl(thd) == false)
      DBUG_RETURN(false);
  }

  DBUG_RETURN(true);
}

int ha_rollback_to_savepoint(THD *thd, SAVEPOINT *sv)
{
  int error=0;
  THD_TRANS *trans= (thd->in_sub_stmt ? &thd->transaction->stmt :
                                        &thd->transaction->all);
  Ha_trx_info *ha_info, *ha_info_next;

  DBUG_ENTER("ha_rollback_to_savepoint");

  trans->no_2pc=0;
  /*
    rolling back to savepoint in all storage engines that were part of the
    transaction when the savepoint was set
  */
  for (ha_info= sv->ha_list; ha_info; ha_info= ha_info->next())
  {
    int err;
    transaction_participant *ht= ha_info->ht();
    DBUG_ASSERT(ht);
    DBUG_ASSERT(ht->savepoint_set != 0);
    if ((err= ht->savepoint_rollback(thd, (uchar *)(sv+1)+ht->savepoint_offset)))
    { // cannot happen
      my_error(ER_ERROR_DURING_ROLLBACK, MYF(0), err);
      error=1;
    }
    status_var_increment(thd->status_var.ha_savepoint_rollback_count);
    trans->no_2pc|= ht->prepare == 0;
  }
  /*
    rolling back the transaction in all storage engines that were not part of
    the transaction when the savepoint was set
  */
  for (ha_info= trans->ha_list; ha_info != sv->ha_list;
       ha_info= ha_info_next)
  {
    int err;
    transaction_participant *ht= ha_info->ht();
#ifdef WITH_WSREP
    if (WSREP(thd) && ht->flags & HTON_WSREP_REPLICATION)
    {
      WSREP_DEBUG("ha_rollback_to_savepoint: run before_rollbackha_rollback_trans hook");
      (void) wsrep_before_rollback(thd, !thd->in_sub_stmt);

    }
#endif // WITH_WSREP
    if ((err= ht->rollback(thd, !thd->in_sub_stmt)))
    { // cannot happen
      my_error(ER_ERROR_DURING_ROLLBACK, MYF(0), err);
      error=1;
    }
#ifdef WITH_WSREP
    if (WSREP(thd) && ht->flags & HTON_WSREP_REPLICATION)
    {
      WSREP_DEBUG("ha_rollback_to_savepoint: run after_rollback hook");
      (void) wsrep_after_rollback(thd, !thd->in_sub_stmt);
    }
#endif // WITH_WSREP
    status_var_increment(thd->status_var.ha_rollback_count);
    ha_info_next= ha_info->next();
    ha_info->reset(); /* keep it conveniently zero-filled */
  }
  trans->ha_list= sv->ha_list;

  if (thd->m_transaction_psi != NULL)
    MYSQL_INC_TRANSACTION_ROLLBACK_TO_SAVEPOINT(thd->m_transaction_psi, 1);

  DBUG_RETURN(error);
}

/**
  @note
  according to the sql standard (ISO/IEC 9075-2:2003)
  section "4.33.4 SQL-statements and transaction states",
  SAVEPOINT is *not* transaction-initiating SQL-statement
*/
int ha_savepoint(THD *thd, SAVEPOINT *sv)
{
#ifdef WITH_WSREP
  /*
    Register binlog hton for savepoint processing if wsrep binlog
    emulation is on.
   */
  if (WSREP_EMULATE_BINLOG(thd) && wsrep_thd_is_local(thd))
  {
    wsrep_register_binlog_handler(thd, thd->in_multi_stmt_transaction_mode());
  }
#endif /* WITH_WSREP */
  int error=0;
  THD_TRANS *trans= (thd->in_sub_stmt ? &thd->transaction->stmt :
                                        &thd->transaction->all);
  Ha_trx_info *ha_info= trans->ha_list;
  DBUG_ENTER("ha_savepoint");

  for (; ha_info; ha_info= ha_info->next())
  {
    int err;
    transaction_participant *ht= ha_info->ht();
    DBUG_ASSERT(ht);
    if (! ht->savepoint_set)
    {
      my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), "SAVEPOINT");
      error=1;
      break;
    }
    if ((err= ht->savepoint_set(thd, (uchar *)(sv+1)+ht->savepoint_offset)))
    { // cannot happen
      my_error(ER_GET_ERRNO, MYF(0), err, hton_name(ht)->str);
      error=1;
    }
    status_var_increment(thd->status_var.ha_savepoint_count);
  }
  /*
    Remember the list of registered storage engines. All new
    engines are prepended to the beginning of the list.
  */
  sv->ha_list= trans->ha_list;

  if (!error && thd->m_transaction_psi != NULL)
    MYSQL_INC_TRANSACTION_SAVEPOINTS(thd->m_transaction_psi, 1);

  DBUG_RETURN(error);
}

int ha_release_savepoint(THD *thd, SAVEPOINT *sv)
{
  int error=0;
  Ha_trx_info *ha_info= sv->ha_list;
  DBUG_ENTER("ha_release_savepoint");

  for (; ha_info; ha_info= ha_info->next())
  {
    int err;
    transaction_participant *ht= ha_info->ht();
    /* Savepoint life time is enclosed into transaction life time. */
    DBUG_ASSERT(ht);
    if (!ht->savepoint_release)
      continue;
    if ((err= ht->savepoint_release(thd, (uchar *)(sv+1) + ht->savepoint_offset)))
    { // cannot happen
      my_error(ER_GET_ERRNO, MYF(0), err, hton_name(ht)->str);
      error=1;
    }
  }

  if (thd->m_transaction_psi != NULL)
    MYSQL_INC_TRANSACTION_RELEASE_SAVEPOINT(thd->m_transaction_psi, 1);

  DBUG_RETURN(error);
}

static bool snapshot_handlerton(THD *thd, transaction_participant *hton, void *arg)
{
  if (hton->start_consistent_snapshot)
  {
    if (hton->start_consistent_snapshot(thd))
      return TRUE;
    *((bool *)arg)= false;
  }
  return FALSE;
}

int ha_start_consistent_snapshot(THD *thd)
{
  bool err, warn= true;

  /*
    Holding the LOCK_commit_ordered mutex ensures that we get the same
    snapshot for all engines (including the binary log).  This allows us
    among other things to do backups with
    START TRANSACTION WITH CONSISTENT SNAPSHOT and
    have a consistent binlog position.
  */
  mysql_mutex_lock(&LOCK_commit_ordered);
  err= tp_foreach(thd, snapshot_handlerton, &warn);
  mysql_mutex_unlock(&LOCK_commit_ordered);

  if (err)
  {
    ha_rollback_trans(thd, true);
    return 1;
  }

  /*
    Same idea as when one wants to CREATE TABLE in one engine which does not
    exist:
  */
  if (warn)
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                 "This MariaDB server does not support any "
                 "consistent-read capable storage engine");
  return 0;
}


static my_bool flush_handlerton(THD *thd, plugin_ref plugin,
                                void *arg)
{
  handlerton *hton= plugin_hton(plugin);
  return hton->flush_logs && hton->flush_logs(hton);
}


bool ha_flush_logs()
{
  return plugin_foreach(NULL, flush_handlerton,
                        MYSQL_STORAGE_ENGINE_PLUGIN, 0);
}


/**
  @brief make canonical filename

  @param[in]  path     original path
  @param[out] buff     buffer for canonized path

  @details Lower case db name and table name path parts for
           non file based tables when lower_case_table_names
           is 2 (store as is, compare in lower case).
           Filesystem path prefix (mysql_data_home or tmpdir)
           is left intact.

  @note buff may be left intact if no conversion was performed.

  @retval canonized path

  @todo This may be done more efficiently when table path
        gets built. Convert this function to something like
        ASSERT_CANONICAL_FILENAME.
*/

Lex_cstring handler::get_canonical_filename(const Lex_cstring &path,
                                            Table_path_buffer *buff) const
{
  /* Ensure that table handler get path in lower case */
  uint i;
  if (!needs_lower_case_filenames())
    return path;

  for (i= 0; i <= mysql_tmpdir_list.max; i++)
  {
    if (is_prefix(path.str, mysql_tmpdir_list.list[i]))
      return path;
  }
  /*
    we only should turn into lowercase database/table part
    so start lower-casing after homedirectory
  */
  DBUG_ASSERT(path.starts_with({mysql_data_home, mysql_data_home_len}));
  /*
    QQ: important for upgrade from MySQL-5.0 with --lower-case-table-names=2

    In case if both the table name and the database name are encoded
    using tablename_to_filename(), the it's ok to lower-case the entire
    "/db/table" part using files_charset_info.

    Otherwise, in case if either of the table name or the database name
    starts with '#mysql50#', it's probably not correct to lower-case using
    files_charset_info. Shoudn't we lower-case '#mysql50#name' using
    character_set_filesystem instead?
  */
  return buff->set_casedn(path.left(mysql_data_home_len),
                          files_charset_info,
                          path.substr(mysql_data_home_len)).to_lex_cstring();
}


/**
   Delete a table in the engine

   @param thd
   @param hton
   @param path                  no extension, e.g. "./test/t1"
   @param db                    for the error message only
   @param alias                 table name, for the error message only
   @param generate_warning      generate "table not found" warnings as needed

   @return 0   Table was deleted
   @return -1  Table didn't exists, no error given
   @return #   Error from table handler

  @note
  ENOENT and HA_ERR_NO_SUCH_TABLE are not considered errors.
  The .frm file should be deleted by the caller only if we return <= 0.
*/

int ha_delete_table(THD *thd, handlerton *hton, const char *path,
                    const LEX_CSTRING *db, const LEX_CSTRING *alias,
                    bool generate_warning)
{
  int error;
  bool is_error= thd->is_error();
  DBUG_ENTER("ha_delete_table");

  /* hton is NULL in ALTER TABLE when renaming only .frm files */
  if (hton == NULL || hton == view_pseudo_hton)
    DBUG_RETURN(0);

  if (ha_check_if_updates_are_ignored(thd, hton, "DROP"))
    DBUG_RETURN(0);

  error= hton->drop_table(hton, path);
  if (error > 0)
  {
    /*
      It's not an error if the table doesn't exist in the engine.
      warn the user, but still report DROP being a success
    */
    bool intercept= non_existing_table_error(error);

    if ((!intercept || generate_warning) && ! thd->is_error())
    {
      TABLE dummy_table;
      TABLE_SHARE dummy_share;
      handler *file= get_new_handler(nullptr, thd->mem_root, hton);
      if (file) {
        bzero((char*) &dummy_table, sizeof(dummy_table));
        bzero((char*) &dummy_share, sizeof(dummy_share));
        dummy_share.path.str= (char*) path;
        dummy_share.path.length= strlen(path);
        dummy_share.normalized_path= dummy_share.path;
        dummy_share.db= Lex_ident_db(*db);
        dummy_share.table_name= Lex_ident_table(*alias);
        dummy_table.s= &dummy_share;
        dummy_table.alias.set(alias->str, alias->length, table_alias_charset);
        file->change_table_ptr(&dummy_table, &dummy_share);
        file->print_error(error, MYF(intercept ? ME_WARNING : 0));
        delete file;
      }
    }
    if (intercept)
    {
      /* Clear error if we got it in this function */
      if (!is_error)
        thd->clear_error();
      error= -1;
    }
  }
  if (error)
    DBUG_PRINT("exit", ("error: %d", error));
  DBUG_RETURN(error);
}

/****************************************************************************
** General handler functions
****************************************************************************/


/**
   Clone a handler

   @param name     name of new table instance
   @param mem_root Where 'this->ref' should be allocated. It can't be
                   in this->table->mem_root as otherwise we will not be
                   able to reclaim that memory when the clone handler
                   object is destroyed.
*/

handler *handler::clone(const char *name, MEM_ROOT *mem_root)
{
  int error= 0;
  handler *new_handler= get_new_handler(table->s, mem_root, ht);

  if (!new_handler)
    return NULL;
  if (new_handler->set_ha_share_ref(ha_share))
  {
    error= ER_OUT_OF_RESOURCES;
    goto err;
  }

  /*
    TODO: Implement a more efficient way to have more than one index open for
    the same table instance. The ha_open call is not cacheable for clone.

    This is not critical as the engines already have the table open
    and should be able to use the original instance of the table.
  */
  if ((error= new_handler->ha_open(table, name,
                                   table->db_stat & HA_READ_ONLY ?
                                   O_RDONLY : O_RDWR,
                                   HA_OPEN_IGNORE_IF_LOCKED, mem_root)))
    goto err;

  new_handler->handler_stats= handler_stats;
  new_handler->set_optimizer_costs(ha_thd());

  return new_handler;

err:
  new_handler->print_error(error, MYF(0));
  delete new_handler;
  return NULL;
}


/**
  clone of current handler.

  Creates a clone of handler used for unique hash key and WITHOUT OVERLAPS.
  @return error code
*/
int handler::create_lookup_handler()
{
  handler *tmp;
  if (lookup_handler != this)
    return 0;
  if (!(tmp= clone(table->s->normalized_path.str, table->in_use->mem_root)))
    return 1;
  lookup_handler= tmp;
  return lookup_handler->ha_external_lock(table->in_use, F_WRLCK);
}

LEX_CSTRING *handler::engine_name()
{
  return hton_name(ht);
}


/*
  Calclate the number of index blocks we are going to access when
  doing 'ranges' index dives reading a total of 'rows' rows.
*/

ulonglong handler::index_blocks(uint index, uint ranges, ha_rows rows)
{
  if (!stats.block_size)
    return 0;                                   // No disk storage
  size_t len= table->key_storage_length(index);
  ulonglong blocks= (rows * len / INDEX_BLOCK_FILL_FACTOR_DIV *
                     INDEX_BLOCK_FILL_FACTOR_MUL) / stats.block_size + ranges;
  return blocks * stats.block_size / IO_SIZE;
}


/*
  Calculate cost for an index scan for given index and number of records.

  @param index   Index to use
  @param ranges  Number of ranges (b-tree dives in case of b-tree).
                 Used by partition engine
  @param rows    Number of expected rows
  @param blocks  Number of disk blocks to read (from range optimizer).
                 0 if not known

  This function does not take in account into looking up the key,
  copying the key to record and finding the next key. These cost are
  handled in ha_keyread_time()
*/

IO_AND_CPU_COST handler::keyread_time(uint index, ulong ranges, ha_rows rows,
                                      ulonglong blocks)
{
  IO_AND_CPU_COST cost;
  ulonglong io_blocks= 0;
  DBUG_ASSERT(ranges > 0);

  /* memory engine has stats.block_size == 0 */
  if (stats.block_size)
  {
    if (!blocks)
    {
      /* Estimate length of index data */
      if (rows <= 1)                              // EQ_REF optimization
      {
        blocks= 1;
        io_blocks= (stats.block_size + IO_SIZE - 1)/ IO_SIZE;
      }
      else
      {
        size_t len= table->key_storage_length(index);
        blocks= ((ulonglong) ((rows * len / INDEX_BLOCK_FILL_FACTOR_DIV *
                               INDEX_BLOCK_FILL_FACTOR_MUL +
                               stats.block_size-1)) / stats.block_size +
                 (ranges - 1));
        io_blocks= blocks * stats.block_size / IO_SIZE;
      }
    }
    else
      io_blocks= blocks * stats.block_size / IO_SIZE;
  }
  cost.io=  (double) io_blocks;
  cost.cpu= blocks * INDEX_BLOCK_COPY_COST;
  return cost;
}


/*
  Cost of doing a set of range scans and finding the key position.
  This function is used both with index scans (in which case there should be
  an additional KEY_COPY_COST) and when normal index + fetch row scan,
  in which case there should an additional rnd_pos_time() cost.
*/

IO_AND_CPU_COST handler::ha_keyread_time(uint index, ulong ranges,
                                         ha_rows rows,
                                         ulonglong blocks)
{
  if (rows < ranges)
    rows= ranges;
  IO_AND_CPU_COST cost= keyread_time(index, ranges, rows, blocks);
  cost.cpu+= ranges * KEY_LOOKUP_COST + (rows - ranges) * KEY_NEXT_FIND_COST;
  return cost;
}


/*
  Read rows from a clustered index

  Cost is similar to ha_rnd_pos_call_time() as a index_read() on a clustered
  key has identical code as rnd_pos() (At least in InnoDB:)
*/

IO_AND_CPU_COST
handler::ha_keyread_clustered_time(uint index, ulong ranges,
                                   ha_rows rows,
                                   ulonglong blocks)
{
  if (rows < ranges)
    rows= ranges;
  IO_AND_CPU_COST cost= keyread_time(index, ranges, rows, blocks);
  cost.cpu+= (ranges * ROW_LOOKUP_COST + (rows - ranges) * ROW_NEXT_FIND_COST);
  return cost;
}

THD *handler::ha_thd(void) const
{
  DBUG_ASSERT(!table || !table->in_use || table->in_use == current_thd);
  return (table && table->in_use) ? table->in_use : current_thd;
}

void handler::unbind_psi()
{
  /*
    Notify the instrumentation that this table is not owned
    by this thread any more.
  */
  PSI_CALL_unbind_table(m_psi);
}

void handler::rebind_psi()
{
  /*
    Notify the instrumentation that this table is now owned
    by this thread.
  */
  m_psi= PSI_CALL_rebind_table(ha_table_share_psi(), this, m_psi);
}


void handler::start_psi_batch_mode()
{
#ifdef HAVE_PSI_TABLE_INTERFACE
  DBUG_ASSERT(m_psi_batch_mode == PSI_BATCH_MODE_NONE);
  DBUG_ASSERT(m_psi_locker == NULL);
  m_psi_batch_mode= PSI_BATCH_MODE_STARTING;
  m_psi_numrows= 0;
#endif
}

void handler::end_psi_batch_mode()
{
#ifdef HAVE_PSI_TABLE_INTERFACE
  DBUG_ASSERT(m_psi_batch_mode != PSI_BATCH_MODE_NONE);
  if (m_psi_locker != NULL)
  {
    DBUG_ASSERT(m_psi_batch_mode == PSI_BATCH_MODE_STARTED);
    PSI_TABLE_CALL(end_table_io_wait)(m_psi_locker, m_psi_numrows);
    m_psi_locker= NULL;
  }
  m_psi_batch_mode= PSI_BATCH_MODE_NONE;
#endif
}

PSI_table_share *handler::ha_table_share_psi() const
{
  return table_share->m_psi;
}

const char *handler::index_type(uint key_number)
{
  static const char* alg2str[]= { "???", "BTREE", "SPATIAL", "HASH",
                                  "FULLTEXT", "HASH", "HASH", "VECTOR" };
  enum ha_key_alg alg= table_share->key_info[key_number].algorithm;
  if (!alg)
  {
    if (index_flags(key_number, 0, 1) & HA_READ_RANGE)
      alg= HA_KEY_ALG_BTREE;
    else
      alg= HA_KEY_ALG_HASH;
  }
  return alg2str[alg];
}


/** @brief
  Open database-handler.

  IMPLEMENTATION
    Try O_RDONLY if cannot open as O_RDWR
    Don't wait for locks if not HA_OPEN_WAIT_IF_LOCKED is set
*/
int handler::ha_open(TABLE *table_arg, const char *name, int mode,
                     uint test_if_locked, MEM_ROOT *mem_root,
                     List<String> *partitions_to_open)
{
  int error;
  DBUG_ENTER("handler::ha_open");
  DBUG_PRINT("enter",
             ("name: %s  db_type: %d  db_stat: %d  mode: %d  lock_test: %d",
              name, ht->db_type, table_arg->db_stat, mode,
              test_if_locked));

  set_table(table_arg);
  DBUG_ASSERT(table->s == table_share);
  DBUG_ASSERT(m_lock_type == F_UNLCK);
  DBUG_PRINT("info", ("old m_lock_type: %d F_UNLCK %d", m_lock_type, F_UNLCK));
  DBUG_ASSERT(alloc_root_inited(&table->mem_root));

  set_partitions_to_open(partitions_to_open);
  internal_tmp_table= MY_TEST(test_if_locked & HA_OPEN_INTERNAL_TABLE);

  if (!internal_tmp_table && (test_if_locked & HA_OPEN_TMP_TABLE) &&
      current_thd->slave_thread)
  {
    /*
      This is a temporary table used by replication that is not attached
      to a THD. Mark it as a global temporary table.
    */
    test_if_locked|= HA_OPEN_GLOBAL_TMP_TABLE;
  }

  if (unlikely((error=open(name,mode,test_if_locked))))
  {
    if ((error == EACCES || error == EROFS) && mode == O_RDWR &&
	(table->db_stat & HA_TRY_READ_ONLY))
    {
      table->db_stat|=HA_READ_ONLY;
      error=open(name,O_RDONLY,test_if_locked);
    }
  }
  if (unlikely(error))
  {
    my_errno= error;                            /* Safeguard */
    DBUG_PRINT("error",("error: %d  errno: %d",error,errno));
  }
  else
  {
    DBUG_ASSERT(m_psi == NULL);
    DBUG_ASSERT(table_share != NULL);
    /*
      Do not call this for partitions handlers, since it may take too much
      resources.
      So only use the m_psi on table level, not for individual partitions.
    */
    if (!(test_if_locked & HA_OPEN_NO_PSI_CALL))
    {
      m_psi= PSI_CALL_open_table(ha_table_share_psi(), this);
    }

    if (table_share->db_options_in_use & HA_OPTION_READ_ONLY_DATA)
      table->db_stat|=HA_READ_ONLY;
    (void) extra(HA_EXTRA_NO_READCHECK);	// Not needed in SQL

    /* Allocate ref in thd or on the table's mem_root */
    if (!(ref= (uchar*) alloc_root(mem_root ? mem_root : &table->mem_root, 
                                   ALIGN_SIZE(ref_length)*2)))
    {
      ha_close();
      error=HA_ERR_OUT_OF_MEM;
    }
    else
      dup_ref=ref+ALIGN_SIZE(ref_length);
    cached_table_flags= table_flags();
    /* Cache index flags */
    for (uint index= 0 ; index < table_share->keys ; index++)
      table->key_info[index].index_flags= index_flags(index, 0, 1);

    if (!table_share->optimizer_costs_inited)
    {
      table_share->optimizer_costs_inited=1;
      /* Copy data from global 'engine'.optimizer_costs to TABLE_SHARE */
      table_share->update_optimizer_costs(partition_ht());
      /* Update costs depend on table structure */
      update_optimizer_costs(&table_share->optimizer_costs);
    }

    /* Copy current optimizer costs. Needed in case clone() is used */
    reset_statistics();
  }

  DBUG_RETURN(error);
}

int handler::ha_close(void)
{
  DBUG_ENTER("ha_close");
  /*
    Increment global statistics for temporary tables.
    In_use is 0 for tables that was closed from the table cache.
  */
  if (table->in_use)
    status_var_add(table->in_use->status_var.rows_tmp_read,
                   rows_stats.tmp_read);
  PSI_CALL_close_table(table_share, m_psi);
  m_psi= NULL; /* instrumentation handle, invalid after close_table() */
  DBUG_ASSERT(m_psi_batch_mode == PSI_BATCH_MODE_NONE);
  DBUG_ASSERT(m_psi_locker == NULL);

  /* Detach from ANALYZE tracker */
  tracker= NULL;
  /* We use ref as way to check that open succeded */
  ref= 0;
  
  DBUG_ASSERT(m_lock_type == F_UNLCK);
  DBUG_ASSERT(inited == NONE);
  DBUG_RETURN(close());
}

void handler::change_table_ptr(TABLE *table_arg, TABLE_SHARE *share)
{
  DBUG_ASSERT(table_arg->s == share);
  table= table_arg;
  table_share= share;
  costs= &share->optimizer_costs;
  reset_statistics();
}


int handler::ha_rnd_next(uchar *buf)
{
  int result;
  DBUG_ENTER("handler::ha_rnd_next");
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(inited == RND);

  DBUG_EXECUTE_IF("ha_rnd_next_error",
  {
    LEX_CSTRING user_var= { STRING_WITH_LEN("ha_rnd_next_error_counter") };
    if (debug_decrement_counter(&user_var))
    {
      print_error(HA_ERR_WRONG_IN_RECORD,MYF(0));
      DBUG_RETURN(HA_ERR_WRONG_IN_RECORD);
    }
  });
  do
  {
    TABLE_IO_WAIT(tracker, PSI_TABLE_FETCH_ROW, MAX_KEY, result,
      { result= rnd_next(buf); })
    if (result != HA_ERR_RECORD_DELETED)
      break;
    status_var_increment(table->in_use->status_var.ha_read_rnd_deleted_count);
  } while (!table->in_use->check_killed(1));

  if (result == HA_ERR_RECORD_DELETED)
    result= HA_ERR_ABORTED_BY_USER;
  else
  {
    if (!result)
    {
      update_rows_read();
      if (table->vfield && buf == table->record[0])
        table->update_virtual_fields(this, VCOL_UPDATE_FOR_READ);
    }
    increment_statistics(&SSV::ha_read_rnd_next_count);
  }

  table->status=result ? STATUS_NOT_FOUND: 0;

  DEBUG_SYNC(ha_thd(), "handler_rnd_next_end");

  DBUG_RETURN(result);
}

int handler::ha_rnd_pos(uchar *buf, uchar *pos)
{
  int result;
  DBUG_ENTER("handler::ha_rnd_pos");
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(inited == RND);

  TABLE_IO_WAIT(tracker, PSI_TABLE_FETCH_ROW, MAX_KEY, result,
    { result= rnd_pos(buf, pos); })
  increment_statistics(&SSV::ha_read_rnd_count);
  if (result == HA_ERR_RECORD_DELETED)
    result= HA_ERR_KEY_NOT_FOUND;
  else if (!result)
  {
    update_rows_read();
    if (table->vfield && buf == table->record[0])
      table->update_virtual_fields(this, VCOL_UPDATE_FOR_READ);
  }
  table->status=result ? STATUS_NOT_FOUND: 0;
  DBUG_RETURN(result);
}


int handler::ha_index_init(uint idx, bool sorted)
{
  DBUG_EXECUTE_IF("ha_index_init_fail", return HA_ERR_TABLE_DEF_CHANGED;);
  int result;
  DBUG_ENTER("ha_index_init");
  DBUG_ASSERT(inited==NONE);
  if (!(result= index_init(idx, sorted)))
  {
    inited=       INDEX;
    active_index= idx;
    end_range= NULL;
    /*
      Do not allow reads from UNIQUE HASH indexes.
      (1) MyRocks sometimes uses hidden indexes that SQL layer isn't aware of,
          skip the check for such cases
    */
    DBUG_ASSERT(active_index >= table->s->keys ||  // (1)
                !(table->key_info[active_index].flags & HA_UNIQUE_HASH));
  }

  DBUG_RETURN(result);
}


int handler::ha_index_read_map(uchar *buf, const uchar *key,
                                      key_part_map keypart_map,
                                      enum ha_rkey_function find_flag)
{
  int result;
  DBUG_ENTER("handler::ha_index_read_map");
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(inited==INDEX);
  assert_icp_limitations(buf);

  TABLE_IO_WAIT(tracker, PSI_TABLE_FETCH_ROW, active_index, result,
    { result= index_read_map(buf, key, keypart_map, find_flag); })
  increment_statistics(&SSV::ha_read_key_count);
  if (!result)
  {
    rows_stats.key_read_hit++;                  // For userstat
    update_index_statistics();
    if (table->vfield && buf == table->record[0])
      table->update_virtual_fields(this, VCOL_UPDATE_FOR_READ);
  }
  else
  {
    status_var_increment(table->in_use->status_var.ha_read_key_miss);
    rows_stats.key_read_miss++;                 // For userstat
  }
  table->status=result ? STATUS_NOT_FOUND: 0;
  DBUG_RETURN(result);
}

/*
  @note: Other index lookup/navigation functions require prior
  handler->index_init() call. This function is different, it requires
  that the scan is not initialized, and accepts "uint index" as an argument.
*/

int handler::ha_index_read_idx_map(uchar *buf, uint index, const uchar *key,
                                          key_part_map keypart_map,
                                          enum ha_rkey_function find_flag)
{
  int result;
  DBUG_ASSERT(inited==NONE);
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(end_range == NULL);
  TABLE_IO_WAIT(tracker, PSI_TABLE_FETCH_ROW, index, result,
    { result= index_read_idx_map(buf, index, key, keypart_map, find_flag); })
  increment_statistics(&SSV::ha_read_key_count);
  if (!result)
  {
    rows_stats.key_read_hit++;
    update_rows_read();
    index_rows_read[index]++;
    if (table->vfield && buf == table->record[0])
      table->update_virtual_fields(this, VCOL_UPDATE_FOR_READ);
  }
  else
  {
    status_var_increment(table->in_use->status_var.ha_read_key_miss);
    rows_stats.key_read_miss++;
  }
  table->status=result ? STATUS_NOT_FOUND: 0;
  return result;
}

int handler::ha_index_next(uchar * buf)
{
  int result;
  DBUG_ENTER("handler::ha_index_next");
 DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(inited==INDEX);
  assert_icp_limitations(buf);

  TABLE_IO_WAIT(tracker, PSI_TABLE_FETCH_ROW, active_index, result,
    { result= index_next(buf); })
  increment_statistics(&SSV::ha_read_next_count);
  if (!result)
  {
    update_index_statistics();
    if (table->vfield && buf == table->record[0])
      table->update_virtual_fields(this, VCOL_UPDATE_FOR_READ);
  }
  table->status=result ? STATUS_NOT_FOUND: 0;

  DEBUG_SYNC(ha_thd(), "handler_ha_index_next_end");

  DBUG_RETURN(result);
}


void handler::assert_icp_limitations(uchar *buf)
{
  /*
    If we are using ICP, we must read the row to table->record[0], as
    pushed_idx_cond has Item_field objects that refer to table->record[0].
  */
  DBUG_ASSERT(!(pushed_idx_cond && active_index == pushed_idx_cond_keyno) ||
              (buf == table->record[0]));
  /*
    Also check that table fields were not "moved" with move_fields(). InnoDB
    calls Field::offset() and null_offset() which require this.
  */
  DBUG_ASSERT(table->field[0]->ptr >= table->record[0] &&
              table->field[0]->ptr <= table->record[0] + table->s->reclength);
}

int handler::ha_index_prev(uchar * buf)
{
  int result;
  DBUG_ENTER("handler::ha_index_prev");
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(inited==INDEX);

  TABLE_IO_WAIT(tracker, PSI_TABLE_FETCH_ROW, active_index, result,
    { result= index_prev(buf); })
  increment_statistics(&SSV::ha_read_prev_count);
  if (!result)
  {
    update_index_statistics();
    if (table->vfield && buf == table->record[0])
      table->update_virtual_fields(this, VCOL_UPDATE_FOR_READ);
  }
  table->status=result ? STATUS_NOT_FOUND: 0;
  DBUG_RETURN(result);
}

int handler::ha_index_first(uchar * buf)
{
  int result;
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(inited==INDEX);
  assert_icp_limitations(buf);

  TABLE_IO_WAIT(tracker, PSI_TABLE_FETCH_ROW, active_index, result,
    { result= index_first(buf); })
  increment_statistics(&SSV::ha_read_first_count);
  if (!result)
  {
    update_index_statistics();
    if (table->vfield && buf == table->record[0])
      table->update_virtual_fields(this, VCOL_UPDATE_FOR_READ);
  }
  table->status=result ? STATUS_NOT_FOUND: 0;
  return result;
}

int handler::ha_index_last(uchar * buf)
{
  int result;
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(inited==INDEX);

  TABLE_IO_WAIT(tracker, PSI_TABLE_FETCH_ROW, active_index, result,
    { result= index_last(buf); })
  increment_statistics(&SSV::ha_read_last_count);
  if (!result)
  {
    update_index_statistics();
    if (table->vfield && buf == table->record[0])
      table->update_virtual_fields(this, VCOL_UPDATE_FOR_READ);
  }
  table->status=result ? STATUS_NOT_FOUND: 0;
  return result;
}

int handler::ha_index_next_same(uchar *buf, const uchar *key, uint keylen)
{
  int result;
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(inited==INDEX);
  assert_icp_limitations(buf);

  TABLE_IO_WAIT(tracker, PSI_TABLE_FETCH_ROW, active_index, result,
    { result= index_next_same(buf, key, keylen); })
  increment_statistics(&SSV::ha_read_next_count);
  if (!result)
  {
    update_index_statistics();
    if (table->vfield && buf == table->record[0])
      table->update_virtual_fields(this, VCOL_UPDATE_FOR_READ);
  }
  table->status=result ? STATUS_NOT_FOUND: 0;
  return result;
}


bool handler::ha_was_semi_consistent_read()
{
  bool result= was_semi_consistent_read();
  if (result)
    increment_statistics(&SSV::ha_read_retry_count);
  return result;
}

/* Initialize handler for random reading, with error handling */

int handler::ha_rnd_init_with_error(bool scan)
{
  int error;
  if (likely(!(error= ha_rnd_init(scan))))
    return 0;
  table->file->print_error(error, MYF(0));
  return error;
}


/**
  Read first row (only) from a table. Used for reading tables with
  only one row, either based on table statistics or if table is a SEQUENCE.

  This is never called for normal InnoDB tables, as these table types
  does not have HA_STATS_RECORDS_IS_EXACT set.
*/
int handler::read_first_row(uchar * buf, uint primary_key)
{
  int error;
  DBUG_ENTER("handler::read_first_row");

  /*
    If there is very few deleted rows in the table, find the first row by
    scanning the table.
    TODO remove the test for HA_READ_ORDER
  */
  if (stats.deleted < 10 || primary_key >= MAX_KEY ||
      !(table->key_info[primary_key].index_flags & HA_READ_ORDER))
  {
    if (likely(!(error= ha_rnd_init(1))))
    {
      error= ha_rnd_next(buf);
      const int end_error= ha_rnd_end();
      if (likely(!error))
        error= end_error;
    }
  }
  else
  {
    /* Find the first row through the primary key */
    if (likely(!(error= ha_index_init(primary_key, 0))))
    {
      error= ha_index_first(buf);
      const int end_error= ha_index_end();
      if (likely(!error))
        error= end_error;
    }
  }
  DBUG_RETURN(error);
}

/**
  Generate the next auto-increment number based on increment and offset.
  computes the lowest number
  - strictly greater than "nr"
  - of the form: auto_increment_offset + N * auto_increment_increment
  If overflow happened then return MAX_ULONGLONG value as an
  indication of overflow.
  In most cases increment= offset= 1, in which case we get:
  @verbatim 1,2,3,4,5,... @endverbatim
    If increment=10 and offset=5 and previous number is 1, we get:
  @verbatim 1,5,15,25,35,... @endverbatim
*/
inline ulonglong
compute_next_insert_id(ulonglong nr,struct system_variables *variables)
{
  const ulonglong save_nr= nr;

  if (variables->auto_increment_increment == 1)
    nr= nr + 1; // optimization of the formula below
  else
  {
    /*
       Calculating the number of complete auto_increment_increment extents:
    */
    nr= (nr + variables->auto_increment_increment -
         variables->auto_increment_offset) /
        (ulonglong) variables->auto_increment_increment;
    /*
       Adding an offset to the auto_increment_increment extent boundary:
    */
    nr= nr * (ulonglong) variables->auto_increment_increment +
        variables->auto_increment_offset;
  }

  if (unlikely(nr <= save_nr))
    return ULONGLONG_MAX;

  return nr;
}


void handler::adjust_next_insert_id_after_explicit_value(ulonglong nr)
{
  /*
    If we have set THD::next_insert_id previously and plan to insert an
    explicitly-specified value larger than this, we need to increase
    THD::next_insert_id to be greater than the explicit value.
  */
  if ((next_insert_id > 0) && (nr >= next_insert_id))
    set_next_insert_id(compute_next_insert_id(nr, &table->in_use->variables));
}


/** @brief
  Computes the largest number X:
  - smaller than or equal to "nr"
  - of the form: auto_increment_offset + N * auto_increment_increment
  where N>=0.

  SYNOPSIS
    prev_insert_id
      nr            Number to "round down"
      variables     variables struct containing auto_increment_increment and
                    auto_increment_offset

  RETURN
    The number X if it exists, "nr" otherwise.
*/
inline ulonglong
prev_insert_id(ulonglong nr, struct system_variables *variables)
{
  if (unlikely(nr < variables->auto_increment_offset))
  {
    /*
      There's nothing good we can do here. That is a pathological case, where
      the offset is larger than the column's max possible value, i.e. not even
      the first sequence value may be inserted. User will receive warning.
    */
    DBUG_PRINT("info",("auto_increment: nr: %lu cannot honour "
                       "auto_increment_offset: %lu",
                       (ulong) nr, variables->auto_increment_offset));
    return nr;
  }
  if (variables->auto_increment_increment == 1)
    return nr; // optimization of the formula below
  /*
     Calculating the number of complete auto_increment_increment extents:
  */
  nr= (nr - variables->auto_increment_offset) /
      (ulonglong) variables->auto_increment_increment;
  /*
     Adding an offset to the auto_increment_increment extent boundary:
  */
  return (nr * (ulonglong) variables->auto_increment_increment +
          variables->auto_increment_offset);
}


/**
  Update the auto_increment field if necessary.

  Updates columns with type NEXT_NUMBER if:

  - If column value is set to NULL (in which case
    auto_increment_field_not_null is 0)
  - If column is set to 0 and (sql_mode & MODE_NO_AUTO_VALUE_ON_ZERO) is not
    set. In the future we will only set NEXT_NUMBER fields if one sets them
    to NULL (or they are not included in the insert list).

    In those cases, we check if the currently reserved interval still has
    values we have not used. If yes, we pick the smallest one and use it.
    Otherwise:

  - If a list of intervals has been provided to the statement via SET
    INSERT_ID or via an Intvar_log_event (in a replication slave), we pick the
    first unused interval from this list, consider it as reserved.

  - Otherwise we set the column for the first row to the value
    next_insert_id(get_auto_increment(column))) which is usually
    max-used-column-value+1.
    We call get_auto_increment() for the first row in a multi-row
    statement. get_auto_increment() will tell us the interval of values it
    reserved for us.

  - In both cases, for the following rows we use those reserved values without
    calling the handler again (we just progress in the interval, computing
    each new value from the previous one). Until we have exhausted them, then
    we either take the next provided interval or call get_auto_increment()
    again to reserve a new interval.

  - In both cases, the reserved intervals are remembered in
    thd->auto_inc_intervals_in_cur_stmt_for_binlog if statement-based
    binlogging; the last reserved interval is remembered in
    auto_inc_interval_for_cur_row. The number of reserved intervals is
    remembered in auto_inc_intervals_count. It differs from the number of
    elements in thd->auto_inc_intervals_in_cur_stmt_for_binlog() because the
    latter list is cumulative over all statements forming one binlog event
    (when stored functions and triggers are used), and collapses two
    contiguous intervals in one (see its append() method).

    The idea is that generated auto_increment values are predictable and
    independent of the column values in the table.  This is needed to be
    able to replicate into a table that already has rows with a higher
    auto-increment value than the one that is inserted.

    After we have already generated an auto-increment number and the user
    inserts a column with a higher value than the last used one, we will
    start counting from the inserted value.

    This function's "outputs" are: the table's auto_increment field is filled
    with a value, thd->next_insert_id is filled with the value to use for the
    next row, if a value was autogenerated for the current row it is stored in
    thd->insert_id_for_cur_row, if get_auto_increment() was called
    thd->auto_inc_interval_for_cur_row is modified, if that interval is not
    present in thd->auto_inc_intervals_in_cur_stmt_for_binlog it is added to
    this list.

  @todo
    Replace all references to "next number" or NEXT_NUMBER to
    "auto_increment", everywhere (see below: there is
    table->auto_increment_field_not_null, and there also exists
    table->next_number_field, it's not consistent).

  @retval
    0	ok
  @retval
    HA_ERR_AUTOINC_READ_FAILED  get_auto_increment() was called and
    returned ~(ulonglong) 0
  @retval
    HA_ERR_AUTOINC_ERANGE storing value in field caused strict mode
    failure.
*/

#define AUTO_INC_DEFAULT_NB_ROWS 1 // Some prefer 1024 here
#define AUTO_INC_DEFAULT_NB_MAX_BITS 16
#define AUTO_INC_DEFAULT_NB_MAX ((1 << AUTO_INC_DEFAULT_NB_MAX_BITS) - 1)

int handler::update_auto_increment()
{
  ulonglong nr, nb_reserved_values;
  bool append= FALSE;
  THD *thd= table->in_use;
  struct system_variables *variables= &thd->variables;
  int result=0, tmp;
  DBUG_ENTER("handler::update_auto_increment");

  /*
    next_insert_id is a "cursor" into the reserved interval, it may go greater
    than the interval, but not smaller.
  */
  DBUG_ASSERT(next_insert_id >= auto_inc_interval_for_cur_row.minimum());

  if ((nr= table->next_number_field->val_int()) != 0 ||
      (table->auto_increment_field_not_null &&
       thd->variables.sql_mode & MODE_NO_AUTO_VALUE_ON_ZERO))
  {

    /*
      There could be an error reported because value was truncated
      when strict mode is enabled.
    */
    if (thd->is_error())
      DBUG_RETURN(HA_ERR_AUTOINC_ERANGE);
    /*
      Update next_insert_id if we had already generated a value in this
      statement (case of INSERT VALUES(null),(3763),(null):
      the last NULL needs to insert 3764, not the value of the first NULL plus
      1).
      Ignore negative values.
    */
    if ((longlong) nr > 0 || (table->next_number_field->flags & UNSIGNED_FLAG))
      adjust_next_insert_id_after_explicit_value(nr);
    insert_id_for_cur_row= 0; // didn't generate anything
    DBUG_RETURN(0);
  }

  if (table->versioned())
  {
    Field *end= table->vers_end_field();
    DBUG_ASSERT(end);
    bitmap_set_bit(table->read_set, end->field_index);
    if (!end->is_max())
    {
      if (thd->lex->sql_command == SQLCOM_ALTER_TABLE)
      {
        if (!table->next_number_field->real_maybe_null())
          DBUG_RETURN(HA_ERR_UNSUPPORTED);
        table->next_number_field->set_null();
      }
      DBUG_RETURN(0);
    }
  }

  // ALTER TABLE ... ADD COLUMN ... AUTO_INCREMENT
  if (thd->lex->sql_command == SQLCOM_ALTER_TABLE)
    table->next_number_field->set_notnull();

  if ((nr= next_insert_id) >= auto_inc_interval_for_cur_row.maximum())
  {
    /* next_insert_id is beyond what is reserved, so we reserve more. */
    const Discrete_interval *forced=
      thd->auto_inc_intervals_forced.get_next();
    if (forced != NULL)
    {
      nr= forced->minimum();
      nb_reserved_values= forced->values();
    }
    else
    {
      /*
        handler::estimation_rows_to_insert was set by
        handler::ha_start_bulk_insert(); if 0 it means "unknown".
      */
      ulonglong nb_desired_values;
      /*
        If an estimation was given to the engine:
        - use it.
        - if we already reserved numbers, it means the estimation was
        not accurate, then we'll reserve 2*AUTO_INC_DEFAULT_NB_ROWS the 2nd
        time, twice that the 3rd time etc.
        If no estimation was given, use those increasing defaults from the
        start, starting from AUTO_INC_DEFAULT_NB_ROWS.
        Don't go beyond a max to not reserve "way too much" (because
        reservation means potentially losing unused values).
        Note that in prelocked mode no estimation is given.
      */

      if ((auto_inc_intervals_count == 0) && (estimation_rows_to_insert > 0))
        nb_desired_values= estimation_rows_to_insert;
      else if ((auto_inc_intervals_count == 0) &&
               (thd->lex->many_values.elements > 0))
      {
        /*
          For multi-row inserts, if the bulk inserts cannot be started, the
          handler::estimation_rows_to_insert will not be set. But we still
          want to reserve the autoinc values.
        */
        nb_desired_values= thd->lex->many_values.elements;
      }
      else /* go with the increasing defaults */
      {
        /* avoid overflow in formula, with this if() */
        if (auto_inc_intervals_count <= AUTO_INC_DEFAULT_NB_MAX_BITS)
        {
          nb_desired_values= AUTO_INC_DEFAULT_NB_ROWS *
            (1 << auto_inc_intervals_count);
          set_if_smaller(nb_desired_values, AUTO_INC_DEFAULT_NB_MAX);
        }
        else
          nb_desired_values= AUTO_INC_DEFAULT_NB_MAX;
      }
      get_auto_increment(variables->auto_increment_offset,
                         variables->auto_increment_increment,
                         nb_desired_values, &nr,
                         &nb_reserved_values);
      if (nr == ULONGLONG_MAX)
        DBUG_RETURN(HA_ERR_AUTOINC_READ_FAILED);  // Mark failure

      /*
        That rounding below should not be needed when all engines actually
        respect offset and increment in get_auto_increment(). But they don't
        so we still do it. Wonder if for the not-first-in-index we should do
        it. Hope that this rounding didn't push us out of the interval; even
        if it did we cannot do anything about it (calling the engine again
        will not help as we inserted no row).
      */
      nr= compute_next_insert_id(nr-1, variables);
    }

    if (table->s->next_number_keypart == 0)
    {
      /* We must defer the appending until "nr" has been possibly truncated */
      append= TRUE;
    }
    else
    {
      /*
        For such auto_increment there is no notion of interval, just a
        singleton. The interval is not even stored in
        thd->auto_inc_interval_for_cur_row, so we are sure to call the engine
        for next row.
      */
      DBUG_PRINT("info",("auto_increment: special not-first-in-index"));
    }
  }

  if (unlikely(nr == ULONGLONG_MAX))
      DBUG_RETURN(HA_ERR_AUTOINC_ERANGE);

  DBUG_ASSERT(nr != 0);
  DBUG_PRINT("info",("auto_increment: %llu  nb_reserved_values: %llu",
                     nr, append ? nb_reserved_values : 0));

  /* Store field without warning (Warning will be printed by insert) */
  {
    Check_level_instant_set check_level_save(thd, CHECK_FIELD_IGNORE);
    tmp= table->next_number_field->store((longlong)nr, TRUE);
  }

  if (unlikely(tmp))                            // Out of range value in store
  {
    /*
      First, test if the query was aborted due to strict mode constraints
      or new field value greater than maximum integer value:
    */
    if (thd->killed == KILL_BAD_DATA ||
        nr > table->next_number_field->get_max_int_value())
    {
      /*
        It's better to return an error here than getting a confusing
        'duplicate key error' later.
      */
      result= HA_ERR_AUTOINC_ERANGE;
    }
    else
    {
      /*
        Field refused this value (overflow) and truncated it, use the result
        of the truncation (which is going to be inserted); however we try to
        decrease it to honour auto_increment_* variables.
        That will shift the left bound of the reserved interval, we don't
        bother shifting the right bound (anyway any other value from this
        interval will cause a duplicate key).
      */
      nr= prev_insert_id(table->next_number_field->val_int(), variables);
      if (unlikely(table->next_number_field->store((longlong)nr, TRUE)))
        nr= table->next_number_field->val_int();
    }
  }
  if (append)
  {
    auto_inc_interval_for_cur_row.replace(nr, nb_reserved_values,
                                          variables->auto_increment_increment);
    auto_inc_intervals_count++;
    /* Row-based replication does not need to store intervals in binlog */
    if (((WSREP_NNULL(thd) && wsrep_emulate_bin_log) ||
         mysql_bin_log.is_open()) &&
        !thd->is_current_stmt_binlog_format_row())
      thd->auto_inc_intervals_in_cur_stmt_for_binlog.
        append(auto_inc_interval_for_cur_row.minimum(),
               auto_inc_interval_for_cur_row.values(),
               variables->auto_increment_increment);
  }

  /*
    Record this autogenerated value. If the caller then
    succeeds to insert this value, it will call
    record_first_successful_insert_id_in_cur_stmt()
    which will set first_successful_insert_id_in_cur_stmt if it's not
    already set.
  */
  insert_id_for_cur_row= nr;

  if (result)                                   // overflow
    DBUG_RETURN(result);

  /*
    Set next insert id to point to next auto-increment value to be able to
    handle multi-row statements.
  */
  set_next_insert_id(compute_next_insert_id(nr, variables));

  DBUG_RETURN(0);
}


/** @brief
  MySQL signal that it changed the column bitmap

  USAGE
    This is for handlers that needs to setup their own column bitmaps.
    Normally the handler should set up their own column bitmaps in
    index_init() or rnd_init() and in any column_bitmaps_signal() call after
    this.

    The handler is allowed to do changes to the bitmap after a index_init or
    rnd_init() call is made as after this, MySQL will not use the bitmap
    for any program logic checking.
*/
void handler::column_bitmaps_signal()
{
  DBUG_ENTER("column_bitmaps_signal");
  if (table)
    DBUG_PRINT("info", ("read_set: %p  write_set: %p",
                        table->read_set, table->write_set));
  DBUG_VOID_RETURN;
}


/** @brief
  Reserves an interval of auto_increment values from the handler.

  SYNOPSIS
    get_auto_increment()
    offset              
    increment
    nb_desired_values   how many values we want
    first_value         (OUT) the first value reserved by the handler
    nb_reserved_values  (OUT) how many values the handler reserved

  offset and increment means that we want values to be of the form
  offset + N * increment, where N>=0 is integer.
  If the function sets *first_value to ~(ulonglong)0 it means an error.
  If the function sets *nb_reserved_values to ULONGLONG_MAX it means it has
  reserved to "positive infinite".
*/
void handler::get_auto_increment(ulonglong offset, ulonglong increment,
                                 ulonglong nb_desired_values,
                                 ulonglong *first_value,
                                 ulonglong *nb_reserved_values)
{
  ulonglong nr;
  int error;
  MY_BITMAP *old_read_set;
  bool rnd_inited= (inited ==  RND);
  bool rev= table->key_info[table->s->next_number_index].
              key_part[table->s->next_number_keypart].key_part_flag &
                HA_REVERSE_SORT;

  if (rnd_inited && ha_rnd_end())
    return;

  old_read_set= table->prepare_for_keyread(table->s->next_number_index);

  if (ha_index_init(table->s->next_number_index, 1))
  {
    /* This should never happen, assert in debug, and fail in release build */
    DBUG_ASSERT(0);
    (void) extra(HA_EXTRA_NO_KEYREAD);
    *first_value= ULONGLONG_MAX;
    if (rnd_inited && ha_rnd_init_with_error(0))
    {
      //TODO: it would be nice to return here an error
    }
    return;
  }

  if (table->s->next_number_keypart == 0)
  {						// Autoincrement at key-start
    error= rev ? ha_index_first(table->record[1])
               : ha_index_last(table->record[1]);
    /*
      MySQL implicitly assumes such method does locking (as MySQL decides to
      use nr+increment without checking again with the handler, in
      handler::update_auto_increment()), so reserves to infinite.
    */
    *nb_reserved_values= ULONGLONG_MAX;
  }
  else
  {
    uchar key[MAX_KEY_LENGTH];
    key_copy(key, table->record[0],
             table->key_info + table->s->next_number_index,
             table->s->next_number_key_offset);
    error= ha_index_read_map(table->record[1], key,
                          make_prev_keypart_map(table->s->next_number_keypart),
                          rev ? HA_READ_KEY_EXACT : HA_READ_PREFIX_LAST);
    /*
      MySQL needs to call us for next row: assume we are inserting ("a",null)
      here, we return 3, and next this statement will want to insert
      ("b",null): there is no reason why ("b",3+1) would be the good row to
      insert: maybe it already exists, maybe 3+1 is too large...
    */
    *nb_reserved_values= 1;
  }

  if (unlikely(error))
  {
    if (error == HA_ERR_END_OF_FILE || error == HA_ERR_KEY_NOT_FOUND)
      /* No entry found, that's fine */;
    else
      print_error(error, MYF(0));
    nr= 1;
  }
  else
    nr= ((ulonglong) table->next_number_field->
         val_int_offset(table->s->rec_buff_length)+1);
  ha_index_end();
  table->restore_column_maps_after_keyread(old_read_set);
  *first_value= nr;
  if (rnd_inited && ha_rnd_init_with_error(0))
  {
    //TODO: it would be nice to return here an error
  }
  return;
}


void handler::ha_release_auto_increment()
{
  DBUG_ENTER("ha_release_auto_increment");
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK ||
              (!next_insert_id && !insert_id_for_cur_row));
  release_auto_increment();
  insert_id_for_cur_row= 0;
  auto_inc_interval_for_cur_row.replace(0, 0, 0);
  auto_inc_intervals_count= 0;
  if (next_insert_id > 0)
  {
    next_insert_id= 0;
    /*
      this statement used forced auto_increment values if there were some,
      wipe them away for other statements.
    */
    table->in_use->auto_inc_intervals_forced.empty();
  }
  DBUG_VOID_RETURN;
}


/**
  Construct and emit duplicate key error message using information
  from table's record buffer.

  @param table    TABLE object which record buffer should be used as
                  source for column values.
  @param key      Key description.
  @param msg      Error message template to which key value should be
                  added.
  @param errflag  Flags for my_error() call.

  @notes
    The error message is from ER_DUP_ENTRY_WITH_KEY_NAME but to keep things compatibly
    with old code, the error number is ER_DUP_ENTRY
*/

void print_keydup_error(TABLE *table, KEY *key, const char *msg, myf errflag)
{
  /* Write the duplicated key in the error message */
  char key_buff[MAX_KEY_LENGTH];
  String str(key_buff,sizeof(key_buff),system_charset_info);

  if (key == NULL)
  {
    /*
      Key is unknown. Should only happen if storage engine reports wrong
      duplicate key number.
    */
    my_printf_error(ER_DUP_ENTRY, msg, errflag, "", "*UNKNOWN*");
  }
  else
  {
    if (key->algorithm == HA_KEY_ALG_LONG_HASH)
      setup_keyinfo_hash(key);
    /* Table is opened and defined at this point */
    key_unpack(&str,table, key);
    uint max_length=MYSQL_ERRMSG_SIZE-(uint) strlen(msg);
    if (str.length() >= max_length)
    {
      str.length(max_length-4);
      str.append(STRING_WITH_LEN("..."));
    }
    my_printf_error(ER_DUP_ENTRY, msg, errflag, str.c_ptr_safe(),
                    key->name.str);
    if (key->algorithm == HA_KEY_ALG_LONG_HASH)
      re_setup_keyinfo_hash(key);
  }
}

/**
  Construct and emit duplicate key error message using information
  from table's record buffer.

  @sa print_keydup_error(table, key, msg, errflag).
*/

void print_keydup_error(TABLE *table, KEY *key, myf errflag)
{
  print_keydup_error(table, key,
                     ER_THD(table->in_use, ER_DUP_ENTRY_WITH_KEY_NAME),
                     errflag);
}

/**
  Print error that we got from handler function.

  @note
    In case of delete table it's only safe to use the following parts of
    the 'table' structure:
    - table->s->path
    - table->alias
*/

#define SET_FATAL_ERROR fatal_error=1

void handler::print_error(int error, myf errflag)
{
  bool fatal_error= 0;
  DBUG_ENTER("handler::print_error");
  DBUG_PRINT("enter",("error: %d",error));

  if (ha_thd()->transaction_rollback_request)
  {
    /* Ensure this becomes a true error */
    errflag&= ~(ME_WARNING | ME_NOTE);
  }

  int textno= -1; // impossible value
  switch (error) {
  case EACCES:
    textno=ER_OPEN_AS_READONLY;
    break;
  case EAGAIN:
    textno=ER_FILE_USED;
    break;
  case ENOENT:
  case ENOTDIR:
  case ELOOP:
    textno=ER_FILE_NOT_FOUND;
    break;
  case ENOSPC:
  case HA_ERR_DISK_FULL:
    SET_FATAL_ERROR;                            // Ensure error is logged
    my_printf_error(ER_DISK_FULL, "Disk got full writing '%s.%s' (Errcode: %iE)",
                    MYF(errflag | ME_ERROR_LOG),
                    table_share->db.str, table_share->table_name.str,
                    error);
    DBUG_VOID_RETURN;
    break;
  case EE_GLOBAL_TMP_SPACE_FULL:                // Safety
  case EE_LOCAL_TMP_SPACE_FULL:                 // Safety
  case HA_ERR_GLOBAL_TMP_SPACE_FULL:
  case HA_ERR_LOCAL_TMP_SPACE_FULL:
    textno= error;
    break;
  case HA_ERR_KEY_NOT_FOUND:
  case HA_ERR_NO_ACTIVE_RECORD:
  case HA_ERR_RECORD_DELETED:
  case HA_ERR_END_OF_FILE:
    /*
      This errors is not normally fatal (for example for reads). However
      if you get it during an update or delete, then its fatal.
      As the user is calling print_error() (which is not done on read), we
      assume something when wrong with the update or delete.
    */
    SET_FATAL_ERROR;
    textno=ER_KEY_NOT_FOUND;
    break;
  case HA_ERR_ABORTED_BY_USER:
  {
    DBUG_ASSERT(ha_thd()->killed);
    ha_thd()->send_kill_message();
    DBUG_VOID_RETURN;
  }
  case HA_ERR_WRONG_MRG_TABLE_DEF:
    textno=ER_WRONG_MRG_TABLE;
    break;
  case HA_ERR_FOUND_DUPP_KEY:
  {
    if (table)
    {
      uint key_nr=get_dup_key(error);
      if ((int) key_nr >= 0 && key_nr < table->s->keys)
      {
        print_keydup_error(table, &table->key_info[key_nr], errflag);
        table->file->lookup_errkey= -1;
        DBUG_VOID_RETURN;
      }
    }
    textno=ER_DUP_KEY;
    break;
  }
  case HA_ERR_FOREIGN_DUPLICATE_KEY:
  {
    char rec_buf[MAX_KEY_LENGTH];
    String rec(rec_buf, sizeof(rec_buf), system_charset_info);
    /* Table is opened and defined at this point */

    /*
      Just print the subset of fields that are part of the first index,
      printing the whole row from there is not easy.
    */
    key_unpack(&rec, table, &table->key_info[0]);

    char child_table_name[NAME_LEN + 1];
    char child_key_name[NAME_LEN + 1];
    if (get_foreign_dup_key(child_table_name, sizeof(child_table_name),
                            child_key_name, sizeof(child_key_name)))
    {
      my_error(ER_FOREIGN_DUPLICATE_KEY_WITH_CHILD_INFO, errflag,
               table_share->table_name.str, rec.c_ptr_safe(),
               child_table_name, child_key_name);
      }
    else
    {
      my_error(ER_FOREIGN_DUPLICATE_KEY_WITHOUT_CHILD_INFO, errflag,
               table_share->table_name.str, rec.c_ptr_safe());
    }
    DBUG_VOID_RETURN;
  }
  case HA_ERR_NULL_IN_SPATIAL:
    my_error(ER_CANT_CREATE_GEOMETRY_OBJECT, errflag);
    DBUG_VOID_RETURN;
  case HA_ERR_FOUND_DUPP_UNIQUE:
    textno=ER_DUP_UNIQUE;
    break;
  case HA_ERR_RECORD_CHANGED:
    /*
      This is not fatal error when using HANDLER interface
      SET_FATAL_ERROR;
    */
    textno=ER_CHECKREAD;
    break;
  case HA_ERR_CRASHED:
    SET_FATAL_ERROR;
    textno=ER_NOT_KEYFILE;
    break;
  case HA_ERR_WRONG_IN_RECORD:
    SET_FATAL_ERROR;
    textno= ER_CRASHED_ON_USAGE;
    break;
  case HA_ERR_CRASHED_ON_USAGE:
    SET_FATAL_ERROR;
    textno=ER_CRASHED_ON_USAGE;
    break;
  case HA_ERR_NOT_A_TABLE:
    textno= error;
    break;
  case HA_ERR_CRASHED_ON_REPAIR:
    SET_FATAL_ERROR;
    textno=ER_CRASHED_ON_REPAIR;
    break;
  case HA_ERR_OUT_OF_MEM:
    textno=ER_OUT_OF_RESOURCES;
    break;
  case HA_ERR_WRONG_COMMAND:
    my_error(ER_ILLEGAL_HA, MYF(0), table_type(), table_share->db.str,
             table_share->table_name.str);
    DBUG_VOID_RETURN;
    break;
  case HA_ERR_OLD_FILE:
    textno=ER_OLD_KEYFILE;
    break;
  case HA_ERR_UNSUPPORTED:
    textno=ER_UNSUPPORTED_EXTENSION;
    break;
  case HA_ERR_RECORD_FILE_FULL:
  {
    textno=ER_RECORD_FILE_FULL;
    /* Write the error message to error log */
    errflag|= ME_ERROR_LOG;
    break;
  }
  case HA_ERR_INDEX_FILE_FULL:
  {
    textno=ER_INDEX_FILE_FULL;
    /* Write the error message to error log */
    errflag|= ME_ERROR_LOG;
    break;
  }
  case HA_ERR_LOCK_WAIT_TIMEOUT:
    textno=ER_LOCK_WAIT_TIMEOUT;
    break;
  case HA_ERR_LOCK_TABLE_FULL:
    textno=ER_LOCK_TABLE_FULL;
    break;
  case HA_ERR_LOCK_DEADLOCK:
  {
    String str, full_err_msg(ER_DEFAULT(ER_LOCK_DEADLOCK),
                             strlen(ER_DEFAULT(ER_LOCK_DEADLOCK)),
                             system_charset_info);

    get_error_message(error, &str);
    full_err_msg.append(str);
    my_printf_error(ER_LOCK_DEADLOCK, "%s", errflag, full_err_msg.c_ptr_safe());
    DBUG_VOID_RETURN;
  }
  case HA_ERR_READ_ONLY_TRANSACTION:
    textno=ER_READ_ONLY_TRANSACTION;
    break;
  case HA_ERR_CANNOT_ADD_FOREIGN:
    textno=ER_CANNOT_ADD_FOREIGN;
    break;
  case HA_ERR_ROW_IS_REFERENCED:
  {
    String str;
    get_error_message(error, &str);
    my_printf_error(ER_ROW_IS_REFERENCED_2,
                    ER(str.length() ? ER_ROW_IS_REFERENCED_2 : ER_ROW_IS_REFERENCED),
                    errflag, str.c_ptr_safe());
    DBUG_VOID_RETURN;
  }
  case HA_ERR_NO_REFERENCED_ROW:
  {
    String str;
    get_error_message(error, &str);
    my_printf_error(ER_NO_REFERENCED_ROW_2,
                    ER(str.length() ? ER_NO_REFERENCED_ROW_2 : ER_NO_REFERENCED_ROW),
                    errflag, str.c_ptr_safe());
    DBUG_VOID_RETURN;
  }
  case HA_ERR_TABLE_DEF_CHANGED:
    textno=ER_TABLE_DEF_CHANGED;
    break;
  case HA_ERR_NO_SUCH_TABLE:
    my_error(ER_NO_SUCH_TABLE_IN_ENGINE, errflag, table_share->db.str,
             table_share->table_name.str);
    DBUG_VOID_RETURN;
  case HA_ERR_RBR_LOGGING_FAILED:
    textno= ER_BINLOG_ROW_LOGGING_FAILED;
    break;
  case HA_ERR_DROP_INDEX_FK:
  {
    const char *ptr= "???";
    uint key_nr= get_dup_key(error);
    if ((int) key_nr >= 0)
      ptr= table->key_info[key_nr].name.str;
    my_error(ER_DROP_INDEX_FK, errflag, ptr);
    DBUG_VOID_RETURN;
  }
  case HA_ERR_TABLE_NEEDS_UPGRADE:
    textno= ER_TABLE_NEEDS_UPGRADE;
    my_error(ER_TABLE_NEEDS_UPGRADE, errflag,
             "TABLE", table_share->table_name.str);
    DBUG_VOID_RETURN;
  case HA_ERR_NO_PARTITION_FOUND:
    textno=ER_WRONG_PARTITION_NAME;
    break;
  case HA_ERR_TABLE_READONLY:
    textno= ER_OPEN_AS_READONLY;
    break;
  case HA_ERR_AUTOINC_READ_FAILED:
    textno= ER_AUTOINC_READ_FAILED;
    break;
  case HA_ERR_AUTOINC_ERANGE:
    textno= error;
    my_error(textno, errflag, table->found_next_number_field->field_name.str,
             table->in_use->get_stmt_da()->current_row_for_warning());
    DBUG_VOID_RETURN;
    break;
  case HA_ERR_TOO_MANY_CONCURRENT_TRXS:
    textno= ER_TOO_MANY_CONCURRENT_TRXS;
    break;
  case HA_ERR_INDEX_COL_TOO_LONG:
    textno= ER_INDEX_COLUMN_TOO_LONG;
    break;
  case HA_ERR_NOT_IN_LOCK_PARTITIONS:
    textno=ER_ROW_DOES_NOT_MATCH_GIVEN_PARTITION_SET;
    break;
  case HA_ERR_INDEX_CORRUPT:
    textno= ER_INDEX_CORRUPT;
    break;
  case HA_ERR_UNDO_REC_TOO_BIG:
    textno= ER_UNDO_RECORD_TOO_BIG;
    break;
  case HA_ERR_COMMIT_ERROR:
    textno= ER_ERROR_DURING_COMMIT;
    break;
  case HA_ERR_PARTITION_LIST:
    my_error(ER_VERS_NOT_ALLOWED, errflag, table->s->db.str, table->s->table_name.str);
    DBUG_VOID_RETURN;
  case HA_ERR_ROLLBACK:
    /* Crash if we run with --debug-assert-on-error */
    DBUG_ASSERT(!debug_assert_if_crashed_table);
    SET_FATAL_ERROR;
    textno= ER_ROLLBACK_ONLY;
    break;
  default:
    {
      /* The error was "unknown" to this function.
	 Ask handler if it has got a message for this error */
      bool temporary= FALSE;
      String str;
      temporary= get_error_message(error, &str);
      if (!str.is_empty())
      {
	const char* engine= table_type();
	if (temporary)
	  my_error(ER_GET_TEMPORARY_ERRMSG, errflag, error, str.c_ptr(),
                   engine);
	else
        {
          SET_FATAL_ERROR;
	  my_error(ER_GET_ERRMSG, errflag, error, str.c_ptr(), engine);
        }
      }
      else
      {
        if (!temporary)
          my_error(ER_GET_ERRNO, errflag, error, table_type());
        /* else no error message. */
      }
      DBUG_VOID_RETURN;
    }
  }
  DBUG_ASSERT(textno > 0);
  if (unlikely(fatal_error))
  {
    /* Ensure this becomes a true error */
    errflag&= ~(ME_WARNING | ME_NOTE);
    if ((debug_assert_if_crashed_table ||
         global_system_variables.log_warnings > 1))
    {
      /*
        Log error to log before we crash or if extended warnings are requested
      */
      errflag|= ME_ERROR_LOG;
    }
  }

  /* if we got an OS error from a file-based engine, specify a path of error */
  if (error < HA_ERR_FIRST && bas_ext()[0])
  {
    char buff[FN_REFLEN];
    strxnmov(buff, sizeof(buff)-1,
             table_share->normalized_path.str, bas_ext()[0], NULL);
    my_error(textno, errflag, buff, error);
  }
  else
    my_error(textno, errflag, table_share->table_name.str, error);
  DBUG_VOID_RETURN;
}


/**
  Return an error message specific to this handler.

  @param error  error code previously returned by handler
  @param buf    pointer to String where to add error message

  @return
    Returns true if this is a temporary error
*/
bool handler::get_error_message(int error, String* buf)
{
  DBUG_EXECUTE_IF("external_lock_failure",
                  buf->set_ascii(STRING_WITH_LEN("KABOOM!")););
  return FALSE;
}

/**
  Check for incompatible collation changes.
   
  @retval
    HA_ADMIN_NEEDS_UPGRADE   Table may have data requiring a recreate index
  @retval
    0                        No upgrade required.
*/

int handler::check_collation_compatibility()
{
  ulong mysql_version= table->s->mysql_version;

  if (mysql_version < Charset::latest_mariadb_version_with_collation_change())
  {
    KEY *key= table->key_info;
    KEY *key_end= key + table->s->keys;
    for (; key < key_end; key++)
    {
      KEY_PART_INFO *key_part= key->key_part;
      KEY_PART_INFO *key_part_end= key_part + key->user_defined_key_parts;
      for (; key_part < key_part_end; key_part++)
      {
        if (!key_part->fieldnr)
          continue;
        Field *field= table->field[key_part->fieldnr - 1];
        uint cs_number= field->charset()->number;
        if (Charset::collation_changed_order(mysql_version, cs_number))
          return HA_ADMIN_NEEDS_UPGRADE;
      }
    }
  }

  return 0;
}


int handler::check_long_hash_compatibility() const
{
  if (!table->s->old_long_hash_function())
    return 0;
  KEY *key= table->key_info;
  KEY *key_end= key + table->s->keys;
  for ( ; key < key_end; key++)
  {
    if (key->algorithm == HA_KEY_ALG_LONG_HASH)
    {
      /*
        The old (pre-MDEV-27653)  hash function was wrong.
        So the long hash unique constraint can have some
        duplicate records.
        We use HA_ADMIN_NEEDS_DATA_CONVERSION to ensure that
        key is re-generated and checked in ha_write_row().
        This will send the error ER_TABLE_NEEDS_REBUILD to the user.
      */
      return HA_ADMIN_NEEDS_DATA_CONVERSION;
    }
  }
  return 0;
}


int handler::check_versioned_compatibility() const
{
  /* Versioned timestamp extended in 11.5.0 for 64 bit systems */
  if (table->s->mysql_version < 110500 && table->versioned() &&
      TIMESTAMP_MAX_YEAR == 2106)
    return HA_ADMIN_NEEDS_DATA_CONVERSION;
  return 0;
}


int handler::check_versioned_compatibility(uint mysql_version) const
{
  /* Versioned timestamp extended in 11.4.0 for 64 bit systems */
  if (mysql_version < 110500 && table->versioned() &&
      TIMESTAMP_MAX_YEAR == 2106)
    return HA_ADMIN_NEEDS_DATA_CONVERSION;
  return 0;
}


int handler::ha_check_for_upgrade(HA_CHECK_OPT *check_opt)
{
  int error;
  KEY *keyinfo, *keyend;
  KEY_PART_INFO *keypart, *keypartend;

  if (table->s->incompatible_version ||
      check_old_types())
    return HA_ADMIN_NEEDS_DATA_CONVERSION;

  if (!table->s->mysql_version)
  {
    /* check for blob-in-key error */
    keyinfo= table->key_info;
    keyend= table->key_info + table->s->keys;
    for (; keyinfo < keyend; keyinfo++)
    {
      keypart= keyinfo->key_part;
      keypartend= keypart + keyinfo->user_defined_key_parts;
      for (; keypart < keypartend; keypart++)
      {
        if (!keypart->fieldnr)
          continue;
        Field *field= table->field[keypart->fieldnr-1];
        if (field->type() == MYSQL_TYPE_BLOB)
        {
          if (check_opt->sql_flags & TT_FOR_UPGRADE)
            check_opt->flags= T_MEDIUM;
          return HA_ADMIN_NEEDS_CHECK;
        }
      }
    }
  }

  /*
    True VARCHAR appeared in MySQL-5.0.3.
    If the FRM is older than 5.0.3, force alter even if the check_old_type()
    call above did not find data types that want upgrade.
  */
  if (table->s->frm_version < FRM_VER_TRUE_VARCHAR)
    return HA_ADMIN_NEEDS_DATA_CONVERSION;

  if (unlikely((error= check_collation_compatibility())))
    return error;

  if (unlikely((error= check_long_hash_compatibility())))
    return error;
    
  if (unlikely((error= check_versioned_compatibility())))
    return error;

  return check_for_upgrade(check_opt);
}


bool handler::check_old_types() const
{
  for (Field **field= table->field; (*field); field++)
  {
    const Type_handler *th= (*field)->type_handler();
    if (th != th->type_handler_for_implicit_upgrade())
      return true;
  }
  return false;
}


static bool update_frm_version(TABLE *table)
{
  char path[FN_REFLEN];
  File file;
  int result= 1;
  DBUG_ENTER("update_frm_version");

  /*
    No need to update frm version in case table was created or checked
    by server with the same version. This also ensures that we do not
    update frm version for temporary tables as this code doesn't support
    temporary tables.

    keep_original_mysql_version is set if the table version cannot be
    changed without rewriting the frm file.
  */
  if (table->s->mysql_version == MYSQL_VERSION_ID ||
      table->s->keep_original_mysql_version)
    DBUG_RETURN(0);

  strxmov(path, table->s->normalized_path.str, reg_ext, NullS);

  if ((file= mysql_file_open(key_file_frm,
                             path, O_RDWR|O_BINARY, MYF(MY_WME))) >= 0)
  {
    uchar version[4];

    int4store(version, MYSQL_VERSION_ID);

    if ((result= (int)mysql_file_pwrite(file, (uchar*) version, 4, 51L,
                                        MYF(MY_WME+MY_NABP))))
      goto err;

    table->s->mysql_version= MYSQL_VERSION_ID;
  }
err:
  if (file >= 0)
    (void) mysql_file_close(file, MYF(MY_WME));
  DBUG_RETURN(result);
}


/**
  @return
    key if error because of duplicated keys
*/
uint handler::get_dup_key(int error)
{
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE || m_lock_type != F_UNLCK);
  DBUG_ENTER("handler::get_dup_key");

  if (lookup_errkey != (uint)-1)
    DBUG_RETURN(errkey= lookup_errkey);

  errkey= (uint)-1;
  if (error == HA_ERR_FOUND_DUPP_KEY ||
      error == HA_ERR_FOREIGN_DUPLICATE_KEY ||
      error == HA_ERR_FOUND_DUPP_UNIQUE || error == HA_ERR_NULL_IN_SPATIAL ||
      error == HA_ERR_DROP_INDEX_FK)
    info(HA_STATUS_ERRKEY | HA_STATUS_NO_LOCK);
  DBUG_RETURN(errkey);
}


/**
  Delete all files with extension from bas_ext().

  @param name		Base name of table

  @note
    We assume that the handler may return more extensions than
    was actually used for the file. We also assume that the first
    extension is the most important one (see the comment near
    handlerton::tablefile_extensions). If this exist and we can't delete
    that it, we will abort the delete.
    If the first one doesn't exists, we have to try to delete all other
    extension as there is chance that the server had crashed between
    the delete of the first file and the next

  @retval
    0   If we successfully deleted at least one file from base_ext and
        didn't get any other errors than ENOENT

  @retval
    !0  Error
*/

int handler::delete_table(const char *name)
{
  int saved_error= ENOENT;
  bool abort_if_first_file_error= 1;
  bool some_file_deleted= 0;
  DBUG_ENTER("handler::delete_table");

  for (const char **ext= bas_ext(); *ext ; ext++)
  {
    int err= mysql_file_delete_with_symlink(key_file_misc, name, *ext, MYF(0));
    if (err)
    {
      if (my_errno != ENOENT)
      {
        saved_error= my_errno;
        /*
          If error other than file not found on the first existing file,
          return the error.
          Otherwise delete as much as possible.
        */
        if (abort_if_first_file_error)
          DBUG_RETURN(saved_error);
      }
    }
    else
      some_file_deleted= 1;
    abort_if_first_file_error= 0;
  }
  DBUG_RETURN(some_file_deleted && saved_error == ENOENT ? 0 : saved_error);
}


int handler::rename_table(const char * from, const char * to)
{
  int error= 0;
  const char **ext, **start_ext;
  start_ext= bas_ext();
  for (ext= start_ext; *ext ; ext++)
  {
    if (unlikely(rename_file_ext(from, to, *ext)))
    {
      if ((error=my_errno) != ENOENT)
	break;
      error= 0;
    }
  }
  if (unlikely(error))
  {
    /* Try to revert the rename. Ignore errors. */
    for (; ext >= start_ext; ext--)
      rename_file_ext(to, from, *ext);
  }
  return error;
}


void handler::drop_table(const char *name)
{
  ha_close();
  delete_table(name);
}


/**
   Return true if the error from drop table means that the
   table didn't exists
*/

bool non_existing_table_error(int error)
{
  return (error == ENOENT ||
          (error == EE_DELETE && my_errno == ENOENT) ||
          error == EE_FILENOTFOUND ||
          error == HA_ERR_NO_SUCH_TABLE ||
          error == HA_ERR_UNSUPPORTED ||
          error == ER_NO_SUCH_TABLE ||
          error == ER_NO_SUCH_TABLE_IN_ENGINE ||
          error == ER_WRONG_OBJECT);
}


/**
  Performs checks upon the table.

  @param thd                thread doing CHECK TABLE operation
  @param check_opt          options from the parser

  @retval
    HA_ADMIN_OK               Successful upgrade
  @retval
    HA_ADMIN_NEEDS_UPGRADE    Table has structures requiring REPAIR TABLE
  @retval
    HA_ADMIN_NEEDS_ALTER      Table has structures requiring ALTER TABLE
  @retval
    HA_ADMIN_NEEDS_DATA_CONVERSION
                              Table has structures requiring
                              ALTER TABLE FORCE, algortithm=COPY to
                              recreate data.
  @retval
    HA_ADMIN_NOT_IMPLEMENTED
*/

int handler::ha_check(THD *thd, HA_CHECK_OPT *check_opt)
{
  int error;
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);

  if ((table->s->mysql_version >= MYSQL_VERSION_ID) &&
      (check_opt->sql_flags & TT_FOR_UPGRADE))
    return 0;

  if (table->s->mysql_version < MYSQL_VERSION_ID)
  {
    error= ha_check_for_upgrade(check_opt);
    if (unlikely(error && (error != HA_ADMIN_NEEDS_CHECK)))
      return error;
    if (unlikely(!error && (check_opt->sql_flags & TT_FOR_UPGRADE)))
      return 0;
  }
  if (unlikely((error= check(thd, check_opt))))
    return error;
  for (uint i= table->s->keys; i < table->s->total_keys; i++)
  {
    DBUG_ASSERT(table->s->hlindexes() == 1);
    if (table->hlindex_open(i) || table->hlindex_lock(i))
      return HA_ADMIN_FAILED;
    if ((error= table->hlindex->file->check(thd, check_opt)))
      return error;
  }
  /* Skip updating frm version if not main handler. */
  if (table->file != this || opt_readonly)
    return error;
  return update_frm_version(table);
}

/**
  A helper function to mark a transaction read-write,
  if it is started.
*/

void handler::mark_trx_read_write_internal()
{
  THD *thd= ha_thd();
  Ha_trx_info *ha_info= &thd->ha_data[ht->slot].ha_info[0];
  /*
    When a storage engine method is called, the transaction must
    have been started, unless it's a DDL call, for which the
    storage engine starts the transaction internally, and commits
    it internally, without registering in the ha_list.
    Unfortunately here we can't know know for sure if the engine
    has registered the transaction or not, so we must check.
  */
  if (ha_info->is_started())
  {
    /*
      table_share can be NULL, for example, in ha_delete_table() or
      ha_rename_table().
    */
    if (table_share == NULL || table_share->tmp_table == NO_TMP_TABLE)
      ha_info->set_trx_read_write();
    /* Mark if we are using a table that cannot do rollback */
    if (ht->flags & HTON_NO_ROLLBACK)
      ha_info->set_trx_no_rollback();
  }
}


/**
  Repair table: public interface.

  @sa handler::repair()
*/

int handler::ha_repair(THD* thd, HA_CHECK_OPT* check_opt)
{
  int result;

  mark_trx_read_write();

  result= repair(thd, check_opt);
  DBUG_ASSERT(result == HA_ADMIN_NOT_IMPLEMENTED ||
              ha_table_flags() & HA_CAN_REPAIR);

  /*
    Update frm version if no errors and there are no version incompatibiltes
    in the data (as these are not fixed by repair).
  */
  if (result == HA_ADMIN_OK && !opt_readonly &&
      table->file->ha_check_for_upgrade(check_opt) == HA_ADMIN_OK)
    result= update_frm_version(table);
  return table->s->hlindexes() ? HA_ADMIN_TRY_ALTER : result;
}


/**
   End bulk insert
*/

int handler::ha_end_bulk_insert()
{
  DBUG_ENTER("handler::ha_end_bulk_insert");
  DBUG_EXECUTE_IF("crash_end_bulk_insert",
                  { extra(HA_EXTRA_FLUSH) ; DBUG_SUICIDE();});
  estimation_rows_to_insert= 0;
  DBUG_RETURN(end_bulk_insert());
}

/**
  Bulk update row: public interface.

  @sa handler::bulk_update_row()
*/

int
handler::ha_bulk_update_row(const uchar *old_data, const uchar *new_data,
                            ha_rows *dup_key_found)
{
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type == F_WRLCK);
  mark_trx_read_write();

  return bulk_update_row(old_data, new_data, dup_key_found);
}


/**
  Delete all rows: public interface.

  @sa handler::delete_all_rows()
*/

int
handler::ha_delete_all_rows()
{
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type == F_WRLCK);
  mark_trx_read_write();

  int err= delete_all_rows();
  if (!err)
    err= table->hlindexes_on_delete_all(false);

  return err;
}


/**
  Truncate table: public interface.

  @sa handler::truncate()
*/

int
handler::ha_truncate()
{
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type == F_WRLCK);
  mark_trx_read_write();

  int err= truncate();
  for (uint i= table->s->keys; !err && i < table->s->total_keys; i++)
  {
    DBUG_ASSERT(table->s->hlindexes() == 1);
    if (!(err= table->hlindex_open(i)) && !(err= table->hlindex_lock(i)))
      err= table->hlindexes_on_delete_all(true);
  }

  return err;
}


/**
  Reset auto increment: public interface.

  @sa handler::reset_auto_increment()
*/

int
handler::ha_reset_auto_increment(ulonglong value)
{
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type == F_WRLCK);
  mark_trx_read_write();

  return reset_auto_increment(value);
}


/**
  Optimize table: public interface.

  @sa handler::optimize()
*/

int
handler::ha_optimize(THD* thd, HA_CHECK_OPT* check_opt)
{
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type == F_WRLCK);
  mark_trx_read_write();

  // in-engine optimize can modify rowids, which will break hlindexes
  return table->s->hlindexes() ? HA_ADMIN_TRY_ALTER : optimize(thd, check_opt);
}


/**
  Analyze table: public interface.

  @sa handler::analyze()
*/

int
handler::ha_analyze(THD* thd, HA_CHECK_OPT* check_opt)
{
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  mark_trx_read_write();

  return analyze(thd, check_opt);
}


/**
  Check and repair table: public interface.

  @sa handler::check_and_repair()
*/

bool
handler::ha_check_and_repair(THD *thd)
{
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type == F_UNLCK);
  mark_trx_read_write();

  return check_and_repair(thd);
}


/**
  Disable indexes: public interface.

  @param map            has 0 for all indexes that should be disabled
  @param persist        indexes should stay disabled after server restart

  Currently engines don't support disabling an arbitrary subset of indexes.

  In particular, if the change is persistent:
  * auto-increment index should not be disabled
  * unique indexes should not be disabled

  if unique or auto-increment indexes are disabled (non-persistently),
  the caller should only insert data that does not require
  auto-inc generation and does not violate uniqueness

  @sa handler::disable_indexes()
*/

int
handler::ha_disable_indexes(key_map map, bool persist)
{
  DBUG_ASSERT(table->s->tmp_table != NO_TMP_TABLE || m_lock_type != F_UNLCK);
  mark_trx_read_write();

  return disable_indexes(map, persist);
}


/**
  Enable indexes: public interface.

  @param map            has 1 for all indexes that should be enabled
  @param persist        indexes should stay enabled after server restart

  @sa handler::enable_indexes()
*/

int
handler::ha_enable_indexes(key_map map, bool persist)
{
  DBUG_ASSERT(table->s->tmp_table != NO_TMP_TABLE || m_lock_type != F_UNLCK);
  mark_trx_read_write();

  return enable_indexes(map, persist);
}


/**
  Discard or import tablespace: public interface.

  @sa handler::discard_or_import_tablespace()
*/

int
handler::ha_discard_or_import_tablespace(my_bool discard)
{
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type == F_WRLCK);
  mark_trx_read_write();

  return discard_or_import_tablespace(discard);
}


bool handler::ha_prepare_inplace_alter_table(TABLE *altered_table,
                                             Alter_inplace_info *ha_alter_info)
{
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  mark_trx_read_write();

  return prepare_inplace_alter_table(altered_table, ha_alter_info);
}


bool handler::ha_commit_inplace_alter_table(TABLE *altered_table,
                                            Alter_inplace_info *ha_alter_info,
                                            bool commit)
{
   /*
     At this point we should have an exclusive metadata lock on the table.
     The exception is if we're about to roll back changes (commit= false).
     In this case, we might be rolling back after a failed lock upgrade,
     so we could be holding the same lock level as for inplace_alter_table().
   */
   DBUG_ASSERT(ha_thd()->mdl_context.is_lock_owner(MDL_key::TABLE,
                                                   table->s->db.str,
                                                   table->s->table_name.str,
                                                   MDL_EXCLUSIVE) ||
               !commit);

   return commit_inplace_alter_table(altered_table, ha_alter_info, commit);
}


/*
   Default implementation to support in-place alter table
   and old online add/drop index API
*/

enum_alter_inplace_result
handler::check_if_supported_inplace_alter(TABLE *altered_table,
                                          Alter_inplace_info *ha_alter_info)
{
  DBUG_ENTER("handler::check_if_supported_inplace_alter");

  HA_CREATE_INFO *create_info= ha_alter_info->create_info;

  if (altered_table->versioned(VERS_TIMESTAMP))
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);

  alter_table_operations inplace_offline_operations=
    ALTER_COLUMN_TYPE_CHANGE_BY_ENGINE |
    ALTER_COLUMN_NAME |
    ALTER_RENAME_COLUMN |
    ALTER_CHANGE_COLUMN_DEFAULT |
    ALTER_COLUMN_DEFAULT |
    ALTER_COLUMN_OPTION |
    ALTER_CHANGE_CREATE_OPTION |
    ALTER_DROP_CHECK_CONSTRAINT |
    ALTER_PARTITIONED |
    ALTER_VIRTUAL_GCOL_EXPR |
    ALTER_RENAME |
    ALTER_RENAME_INDEX |
    ALTER_INDEX_IGNORABILITY;

  /* Is there at least one operation that requires copy algorithm? */
  if (ha_alter_info->handler_flags & ~inplace_offline_operations)
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);

  /*
    The following checks for changes related to ALTER_OPTIONS

    ALTER TABLE tbl_name CONVERT TO CHARACTER SET .. and
    ALTER TABLE table_name DEFAULT CHARSET = .. most likely
    change column charsets and so not supported in-place through
    old API.

    Changing of PACK_KEYS, MAX_ROWS and ROW_FORMAT options were
    not supported as in-place operations in old API either.
  */
  if (create_info->used_fields & (HA_CREATE_USED_CHARSET |
                                  HA_CREATE_USED_DEFAULT_CHARSET |
                                  HA_CREATE_USED_PACK_KEYS |
                                  HA_CREATE_USED_CHECKSUM |
                                  HA_CREATE_USED_MAX_ROWS) ||
      (table->s->row_type != create_info->row_type))
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);

  if (create_info->sequence)
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);

  uint table_changes= (ha_alter_info->handler_flags &
                       ALTER_COLUMN_TYPE_CHANGE_BY_ENGINE) ?
    IS_EQUAL_PACK_LENGTH : IS_EQUAL_YES;
  if (table->file->check_if_incompatible_data(create_info, table_changes)
      == COMPATIBLE_DATA_YES)
    DBUG_RETURN(HA_ALTER_INPLACE_NO_LOCK);

  DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
}

Alter_inplace_info::Alter_inplace_info(HA_CREATE_INFO *create_info_arg,
                     Alter_info *alter_info_arg,
                     KEY *key_info_arg, uint key_count_arg,
                     partition_info *modified_part_info_arg,
                     bool ignore_arg, bool error_non_empty)
    : create_info(create_info_arg),
    alter_info(alter_info_arg),
    key_info_buffer(key_info_arg),
    key_count(key_count_arg),
    rename_keys(current_thd->mem_root),
    modified_part_info(modified_part_info_arg),
    ignore(ignore_arg),
    inplace_supported(HA_ALTER_ERROR),
    error_if_not_empty(error_non_empty)
  {}

void Alter_inplace_info::report_unsupported_error(const char *not_supported,
                                                  const char *try_instead) const
{
  if (unsupported_reason == NULL)
    my_error(ER_ALTER_OPERATION_NOT_SUPPORTED, MYF(0),
             not_supported, try_instead);
  else
    my_error(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON, MYF(0),
             not_supported, unsupported_reason, try_instead);
}


/**
  Rename table: public interface.

  @sa handler::rename_table()
*/

int
handler::ha_rename_table(const char *from, const char *to)
{
  DBUG_ASSERT(m_lock_type == F_UNLCK);
  mark_trx_read_write();

  return rename_table(from, to);
}


/**
   Structure used during force drop table.
*/

struct st_force_drop_table_params
{
  const char *path;
  const LEX_CSTRING *db;
  const LEX_CSTRING *alias;
  int error;
  bool discovering;
};


/**
   Try to delete table from a given plugin
   Table types with discovery is ignored as these .frm files would have
   been created during discovery and thus doesn't need to be found
   for drop table force
*/

static my_bool delete_table_force(THD *thd, plugin_ref plugin, void *arg)
{
  handlerton *hton = plugin_hton(plugin);
  st_force_drop_table_params *param = (st_force_drop_table_params *)arg;

  if (param->discovering == (hton->discover_table != NULL) &&
      !(thd->slave_thread && (hton->flags & HTON_IGNORE_UPDATES)))
  {
    int error;
    error= ha_delete_table(thd, hton, param->path, param->db, param->alias, 0);
    if (error > 0 && !non_existing_table_error(error))
      param->error= error;
    if (error == 0)
    {
      if (hton && hton->flags & HTON_TABLE_MAY_NOT_EXIST_ON_SLAVE)
        thd->replication_flags |= OPTION_IF_EXISTS;
      param->error= 0;
      return TRUE;                                // Table was deleted
    }
  }
  return FALSE;
}

/**
   @brief
   Traverse all plugins to delete table when .frm file is missing.

   @return -1  Table was not found in any engine
   @return 0  Table was found in some engine and delete succeded
   @return #  Error from first engine that had a table but didn't succeed to
              delete the table
   @return HA_ERR_ROW_IS_REFERENCED if foreign key reference is encountered,

*/

int ha_delete_table_force(THD *thd, const char *path, const LEX_CSTRING *db,
                          const LEX_CSTRING *alias)
{
  st_force_drop_table_params param;
  Table_exists_error_handler no_such_table_handler;
  DBUG_ENTER("ha_delete_table_force");

  param.path=        path;
  param.db=          db;
  param.alias=       alias;
  param.error=       -1;                   // Table not found
  param.discovering= true;

  thd->push_internal_handler(&no_such_table_handler);
  if (plugin_foreach(thd, delete_table_force, MYSQL_STORAGE_ENGINE_PLUGIN,
                     &param))
    param.error= 0;                            // Delete succeded
  else
  {
    param.discovering= false;
    if (plugin_foreach(thd, delete_table_force, MYSQL_STORAGE_ENGINE_PLUGIN,
                       &param))
      param.error= 0;                            // Delete succeded
  }
  thd->pop_internal_handler();
  DBUG_RETURN(param.error);
}


/**
  Create a table in the engine: public interface.

  @sa handler::create()
*/

int
handler::ha_create(const char *name, TABLE *form, HA_CREATE_INFO *info_arg)
{
  DBUG_ASSERT(m_lock_type == F_UNLCK);
  mark_trx_read_write();
  if ((info_arg->options & HA_LEX_CREATE_TMP_TABLE) &&
      current_thd->slave_thread)
    info_arg->options|= HA_LEX_CREATE_GLOBAL_TMP_TABLE;
  int error= create(name, form, info_arg);
  if (!error &&
      !(info_arg->options & (HA_LEX_CREATE_TMP_TABLE | HA_CREATE_TMP_ALTER)) &&
      /*
        DO not notify if not main handler.
        So skip notifications for partitions.
      */
      form->file == this)
    mysql_audit_create_table(form);
  return error;
}


/**
  Create handler files for CREATE TABLE: public interface.

  @sa handler::create_partitioning_metadata()
*/

int
handler::ha_create_partitioning_metadata(const char *name,
                                         const char *old_name,
                                         chf_create_flags action_flag)
{
  /*
    Normally this is done when unlocked, but in fast_alter_partition_table,
    it is done on an already locked handler when preparing to alter/rename
    partitions.
  */
  DBUG_ASSERT(m_lock_type == F_UNLCK ||
              (!old_name && strcmp(name, table_share->path.str)));

  return create_partitioning_metadata(name, old_name, action_flag);
}


/**
  Change partitions: public interface.

  @sa handler::change_partitions()
*/

int
handler::ha_change_partitions(HA_CREATE_INFO *create_info,
                              const char *path,
                              ulonglong * const copied,
                              ulonglong * const deleted,
                              const uchar *pack_frm_data,
                              size_t pack_frm_len)
{
  /*
    Must have at least RDLCK or be a TMP table. Read lock is needed to read
    from current partitions and write lock will be taken on new partitions.
  */
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);

  mark_trx_read_write();

  return change_partitions(create_info, path, copied, deleted,
                           pack_frm_data, pack_frm_len);
}


/**
  Drop partitions: public interface.

  @sa handler::drop_partitions()
*/

int
handler::ha_drop_partitions(const char *path)
{
  DBUG_ASSERT(!table->db_stat);

  mark_trx_read_write();

  return drop_partitions(path);
}


/**
  Rename partitions: public interface.

  @sa handler::rename_partitions()
*/

int
handler::ha_rename_partitions(const char *path)
{
  DBUG_ASSERT(!table->db_stat);

  mark_trx_read_write();

  return rename_partitions(path);
}


/**
  Tell the storage engine that it is allowed to "disable transaction" in the
  handler. It is a hint that ACID is not required - it was used in NDB for
  ALTER TABLE, for example, when data are copied to temporary table.
  A storage engine may treat this hint any way it likes. NDB for example
  started to commit every now and then automatically.
  This hint can be safely ignored.
*/
int ha_enable_transaction(THD *thd, bool on)
{
  int error=0;
  DBUG_ENTER("ha_enable_transaction");
  DBUG_PRINT("enter", ("on: %d", (int) on));

  if ((thd->transaction->on= on))
  {
    /*
      Now all storage engines should have transaction handling enabled.
      But some may have it enabled all the time - "disabling" transactions
      is an optimization hint that storage engine is free to ignore.
      So, let's commit an open transaction (if any) now.
    */
    if (likely(!(error= ha_commit_trans(thd, 0))))
      error= trans_commit_implicit(thd);
  }
  DBUG_RETURN(error);
}

int handler::index_next_same(uchar *buf, const uchar *key, uint keylen)
{
  int error;
  DBUG_ENTER("handler::index_next_same");
  if (!(error=index_next(buf)))
  {
    my_ptrdiff_t ptrdiff= buf - table->record[0];
    uchar *UNINIT_VAR(save_record_0);
    KEY *UNINIT_VAR(key_info);
    KEY_PART_INFO *UNINIT_VAR(key_part);
    KEY_PART_INFO *UNINIT_VAR(key_part_end);

    /*
      key_cmp_if_same() compares table->record[0] against 'key'.
      In parts it uses table->record[0] directly, in parts it uses
      field objects with their local pointers into table->record[0].
      If 'buf' is distinct from table->record[0], we need to move
      all record references. This is table->record[0] itself and
      the field pointers of the fields used in this key.
    */
    if (ptrdiff)
    {
      save_record_0= table->record[0];
      table->record[0]= buf;
      key_info= table->key_info + active_index;
      key_part= key_info->key_part;
      key_part_end= key_part + key_info->user_defined_key_parts;
      for (; key_part < key_part_end; key_part++)
      {
        DBUG_ASSERT(key_part->field);
        key_part->field->move_field_offset(ptrdiff);
      }
    }

    if (key_cmp_if_same(table, key, active_index, keylen))
    {
      table->status=STATUS_NOT_FOUND;
      error=HA_ERR_END_OF_FILE;
    }

    /* Move back if necessary. */
    if (ptrdiff)
    {
      table->record[0]= save_record_0;
      for (key_part= key_info->key_part; key_part < key_part_end; key_part++)
        key_part->field->move_field_offset(-ptrdiff);
    }
  }
  DBUG_PRINT("return",("%i", error));
  DBUG_RETURN(error);
}


void handler::get_dynamic_partition_info(PARTITION_STATS *stat_info,
                                         uint part_id)
{
  info(HA_STATUS_CONST | HA_STATUS_TIME | HA_STATUS_VARIABLE |
       HA_STATUS_NO_LOCK);
  stat_info->records=              stats.records;
  stat_info->mean_rec_length=      stats.mean_rec_length;
  stat_info->data_file_length=     stats.data_file_length;
  stat_info->max_data_file_length= stats.max_data_file_length;
  stat_info->index_file_length=    stats.index_file_length;
  stat_info->max_index_file_length=stats.max_index_file_length;
  stat_info->delete_length=        stats.delete_length;
  stat_info->create_time=          stats.create_time;
  stat_info->update_time=          stats.update_time;
  stat_info->check_time=           stats.check_time;
  stat_info->check_sum=            stats.checksum;
  stat_info->check_sum_null=       stats.checksum_null;
}


/*
  Updates the global table stats with the TABLE this handler represents
*/

void handler::update_global_table_stats()
{
  ulonglong changed;
  TABLE_STATS * table_stats;

  status_var_add(table->in_use->status_var.rows_read, rows_stats.read);
  DBUG_ASSERT(rows_stats.tmp_read == 0);

  if (!table->in_use->userstat_running ||
      table->s->table_category != TABLE_CATEGORY_USER)
    goto reset;

  if (rows_stats.read + rows_stats.updated + rows_stats.inserted +
      rows_stats.deleted + rows_stats.key_read_miss == 0)
    goto reset;                                   // Nothing to update.

  DBUG_ASSERT(table->s);
  DBUG_ASSERT(table->s->table_cache_key.str);

  mysql_mutex_lock(&LOCK_global_table_stats);
  /* Gets the global table stats, creating one if necessary. */
  if (!(table_stats= (TABLE_STATS*)
        my_hash_search(&global_table_stats,
                    (uchar*) table->s->table_cache_key.str,
                    table->s->table_cache_key.length)))
  {
    if (!(table_stats = ((TABLE_STATS*)
                         my_malloc(PSI_INSTRUMENT_ME, sizeof(TABLE_STATS),
                                   MYF(MY_WME | MY_ZEROFILL)))))
    {
      /* Out of memory error already given */
      goto end;
    }
    memcpy(table_stats->table, table->s->table_cache_key.str,
           table->s->table_cache_key.length);
    table_stats->table_name_length= (uint)table->s->table_cache_key.length;
    table_stats->engine_type= ht->db_type;
    /* No need to set variables to 0, as we use MY_ZEROFILL above */

    if (my_hash_insert(&global_table_stats, (uchar*) table_stats))
    {
      /* Out of memory error is already given */
      my_free(table_stats);
      goto end;
    }
  }
  // Updates the global table stats.
  table_stats->rows_stats.read+=          rows_stats.read;
  table_stats->rows_stats.updated+=       rows_stats.updated;
  table_stats->rows_stats.inserted+=      rows_stats.inserted;
  table_stats->rows_stats.deleted+=       rows_stats.deleted;
  table_stats->rows_stats.key_read_hit+=  rows_stats.key_read_hit;
  table_stats->rows_stats.key_read_miss+= rows_stats.key_read_miss;
  if (handler_stats)
  {
    table_stats->rows_stats.pages_accessed+=   handler_stats->pages_accessed;
    table_stats->rows_stats.pages_read_count+= handler_stats->pages_read_count;
  }
  changed= rows_stats.updated + rows_stats.inserted + rows_stats.deleted;
  table_stats->rows_changed_x_indexes+= (changed *
                                         (table->s->keys ? table->s->keys :
                                          1));
end:
  mysql_mutex_unlock(&LOCK_global_table_stats);
reset:
  bzero(&rows_stats, sizeof(rows_stats));
}


/*
  Updates the global index stats with this handler's accumulated index reads.
*/

void handler::update_global_index_stats()
{
  DBUG_ASSERT(table->s);

  if (table->s->table_category != TABLE_CATEGORY_USER)
    return; // Ignore stat tables, performance_schema, information_schema etc.

  if (!table->in_use->userstat_running)
  {
    /* Reset all index read values */
    bzero(index_rows_read, sizeof(index_rows_read[0]) * table->s->keys);
    return;
  }

  for (uint index = 0; index < table->s->keys; index++)
  {
    if (index_rows_read[index])
    {
      INDEX_STATS* index_stats;
      size_t key_length;
      KEY *key_info = &table->key_info[index];  // Rows were read using this

      DBUG_ASSERT(key_info->cache_name);
      if (!key_info->cache_name)
        continue;
      key_length= table->s->table_cache_key.length + key_info->name.length + 1;
      mysql_mutex_lock(&LOCK_global_index_stats);
      // Gets the global index stats, creating one if necessary.
      if (!(index_stats= (INDEX_STATS*) my_hash_search(&global_index_stats,
                                                    key_info->cache_name,
                                                    key_length)))
      {
        if (!(index_stats = ((INDEX_STATS*)
                             my_malloc(PSI_INSTRUMENT_ME, sizeof(INDEX_STATS),
                                       MYF(MY_WME | MY_ZEROFILL)))))
          goto end;                             // Error is already given

        memcpy(index_stats->index, key_info->cache_name, key_length);
        index_stats->index_name_length= key_length;
        if (my_hash_insert(&global_index_stats, (uchar*) index_stats))
        {
          my_free(index_stats);
          goto end;
        }
      }
      /* Updates the global index stats. */
      index_stats->rows_read+= index_rows_read[index];
      /*
        Ensure we do not update queries if the table is used
        twice in the same statement.
      */
      if (index_stats->query_id != table->in_use->query_id)
      {
        index_stats->query_id= table->in_use->query_id;
        index_stats->queries++;
      }
      index_rows_read[index]= 0;
end:
      mysql_mutex_unlock(&LOCK_global_index_stats);
    }
  }
}


static void flush_checksum(ha_checksum *row_crc, uchar **checksum_start,
                           size_t *checksum_length)
{
  if (*checksum_start)
  {
    *row_crc= my_checksum(*row_crc, *checksum_start, *checksum_length);
    *checksum_start= NULL;
    *checksum_length= 0;
  }
}


/* calculating table's checksum */
int handler::calculate_checksum()
{
  int error;
  THD *thd=ha_thd();
  DBUG_ASSERT(table->s->last_null_bit_pos < 8);
  uchar null_mask= table->s->last_null_bit_pos
                   ? 256 -  (1 << table->s->last_null_bit_pos) : 0;

  table->use_all_stored_columns();
  stats.checksum= 0;

  if ((error= ha_rnd_init(1)))
    return error;

  for (;;)
  {
    if (thd->killed)
    {
      error= HA_ERR_ABORTED_BY_USER;
      break;
    }

    ha_checksum row_crc= 0;
    error= ha_rnd_next(table->record[0]);
    if (error)
      break;

    if (table->s->null_bytes)
    {
      /* fix undefined null bits */
      table->record[0][table->s->null_bytes-1] |= null_mask;
      if (!(table->s->db_create_options & HA_OPTION_PACK_RECORD))
        table->record[0][0] |= 1;

      row_crc= my_checksum(row_crc, table->record[0], table->s->null_bytes);
    }

    uchar *checksum_start= NULL;
    size_t checksum_length= 0;
    for (uint i= 0; i < table->s->fields; i++ )
    {
      Field *f= table->field[i];
      if (!f->stored_in_db())
        continue;


      if (! (thd->variables.old_behavior & OLD_MODE_COMPAT_5_1_CHECKSUM) &&
            f->is_real_null(0))
      {
        flush_checksum(&row_crc, &checksum_start, &checksum_length);
        continue;
      }
     /*
       BLOB and VARCHAR have pointers in their field, we must convert
       to string; GEOMETRY is implemented on top of BLOB.
       BIT may store its data among NULL bits, convert as well.
     */
      switch (f->type()) {
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_GEOMETRY:
        case MYSQL_TYPE_BIT:
        {
          flush_checksum(&row_crc, &checksum_start, &checksum_length);
          String tmp;
          f->val_str(&tmp);
          row_crc= my_checksum(row_crc, (uchar*) tmp.ptr(), tmp.length());
          break;
        }
        default:
          if (!checksum_start)
            checksum_start= f->ptr;
          DBUG_ASSERT(checksum_start + checksum_length == f->ptr);
          checksum_length+= f->pack_length();
          break;
      }
    }
    flush_checksum(&row_crc, &checksum_start, &checksum_length);

    stats.checksum+= row_crc;
  }
  ha_rnd_end();
  return error == HA_ERR_END_OF_FILE ? 0 : error;
}


/****************************************************************************
** Some general functions that isn't in the handler class
****************************************************************************/

static int ha_create_table_from_share(THD *thd, TABLE_SHARE *share,
                                      HA_CREATE_INFO *create_info,
                                      uint *ref_length)
{
  TABLE table;

  if (open_table_from_share(thd, share, &empty_clex_str, 0, READ_ALL, 0,
                            &table, true))
    return 1;

  update_create_info_from_table(create_info, &table);

  Table_path_buffer name_buff;
  Lex_cstring name= table.file->get_canonical_filename(share->path, &name_buff);
  int error= table.check_sequence_privileges(thd) ? 1 :
             table.file->ha_create(name.str, &table, create_info);

  if (error)
  {
    if (!thd->is_error())
      my_error(ER_CANT_CREATE_TABLE, MYF(0), share->db.str,
               share->table_name.str, error);
    table.file->print_error(error, MYF(ME_WARNING));
  }
  *ref_length= table.file->ref_length; // for hlindexes

  (void) closefrm(&table);
  return error;
}

/**
  Initiates table-file and calls appropriate database-creator.

  @param thd
  @param path           no extension, e.g. "./test/t1"
  @param db
  @param table_name
  @param create_info
  @param frm            an frm image or NULL (meaning, read it from the file)
  @param skip_frm_file  do not write the frm image to the .frm file

  @retval
   0  ok
  @retval
   1  error
*/
int ha_create_table(THD *thd, const char *path, const char *db,
                    const char *table_name, HA_CREATE_INFO *create_info,
                    LEX_CUSTRING *frm, bool skip_frm_file)
{
  int error= 1;
  uint ref_length;
  TABLE_SHARE share;
  Abort_on_warning_instant_set old_abort_on_warning(thd, 0);
  bool is_tmp __attribute__((unused)) =
    create_info->options & (HA_LEX_CREATE_TMP_TABLE | HA_CREATE_TMP_ALTER);
  DBUG_ENTER("ha_create_table");

  init_tmp_table_share(thd, &share, db, 0, table_name, path, true);

  if (frm)
  {
    bool write_frm_now= (!create_info->db_type->discover_table &&
                         !create_info->tmp_table() && !skip_frm_file);

    share.frm_image= frm;

    // open an frm image
    if (share.init_from_binary_frm_image(thd, write_frm_now,
                                         frm->str, frm->length))
      goto err;
  }
  else
  {
    // open an frm file
    share.db_plugin= ha_lock_engine(thd, create_info->db_type);

    if (open_table_def(thd, &share))
      goto err;
  }

  share.m_psi= PSI_CALL_get_table_share(is_tmp, &share);
  if ((error= ha_create_table_from_share(thd, &share, create_info, &ref_length)))
  {
    PSI_CALL_drop_table_share(is_tmp, share.db.str, (uint)share.db.length,
                        share.table_name.str, (uint)share.table_name.length);
    goto err;
  }

  /* create secondary tables for high level indexes */
  if (share.hlindexes())
  {
    /* as of now: only one vector index can be here */
    DBUG_ASSERT(share.hlindexes() == 1);
    DBUG_ASSERT(share.key_info[share.keys].algorithm == HA_KEY_ALG_VECTOR);
    TABLE_SHARE index_share;
    char file_name[FN_REFLEN+1];
    char index_file_name[FN_REFLEN+1], *index_file_name_end;
    Alter_info index_ainfo;
    HA_CREATE_INFO index_cinfo;
    char *path_end= strmov(file_name, path);

    bzero((char*) &index_cinfo, sizeof(index_cinfo));
    index_cinfo.alter_info= &index_ainfo;
    index_file_name_end= const_cast<char*>(create_info->index_file_name);
    if (index_file_name_end)
    {
      index_file_name_end= strmov(index_file_name, index_file_name_end);
      index_cinfo.index_file_name= index_file_name;
      index_cinfo.data_file_name= index_file_name;
    }

    if ((error= share.path.length > sizeof(file_name) - HLINDEX_BUF_LEN))
      goto err;

    enum_sql_command old_sql_command= thd->lex->sql_command;
    for (uint i= share.keys; i < share.total_keys; i++)
    {
      thd->lex->sql_command= SQLCOM_CREATE_INDEX;
      my_snprintf(path_end, HLINDEX_BUF_LEN, HLINDEX_TEMPLATE, i);
      if (create_info->index_file_name)
        my_snprintf(index_file_name_end, HLINDEX_BUF_LEN, HLINDEX_TEMPLATE, i);
      init_tmp_table_share(thd, &index_share, db, 0, table_name, file_name, 1);
      index_share.db_plugin= share.db_plugin;
      LEX_CSTRING sql= mhnsw_hlindex_table_def(thd, ref_length);
      error= !sql.length ||
        index_share.init_from_sql_statement_string(thd, 0, sql.str, sql.length);
      if (error)
      {
        index_share.db_plugin= NULL;
        break;
      }

      uint unused;
      if ((error= ha_create_table_from_share(thd, &index_share, &index_cinfo,
                                             &unused)))
        break;
    }
    thd->lex->sql_command= old_sql_command;
    free_table_share(&index_share);
  }

err:
  free_table_share(&share);
  DBUG_RETURN(error != 0);
}

void st_ha_check_opt::init()
{
  flags= sql_flags= handler_flags= 0;
  start_time= my_time(0);
}


/*****************************************************************************
  Key cache handling.

  This code is only relevant for ISAM/MyISAM tables

  key_cache->cache may be 0 only in the case where a key cache is not
  initialized or when we where not able to init the key cache in a previous
  call to ha_init_key_cache() (probably out of memory)
*****************************************************************************/

/**
  Init a key cache if it has not been initied before.
*/
int ha_init_key_cache(const char *name, KEY_CACHE *key_cache, void *)
{
  DBUG_ENTER("ha_init_key_cache");

  if (!key_cache->key_cache_inited)
  {
    mysql_mutex_lock(&LOCK_global_system_variables);
    size_t tmp_buff_size= (size_t) key_cache->param_buff_size;
    uint tmp_block_size= (uint) key_cache->param_block_size;
    uint division_limit= (uint)key_cache->param_division_limit;
    uint age_threshold=  (uint)key_cache->param_age_threshold;
    uint partitions=     (uint)key_cache->param_partitions;
    uint changed_blocks_hash_size=  (uint)key_cache->changed_blocks_hash_size;
    mysql_mutex_unlock(&LOCK_global_system_variables);
    DBUG_RETURN(!init_key_cache(key_cache,
				tmp_block_size,
				tmp_buff_size,
				division_limit, age_threshold,
                                changed_blocks_hash_size,
                                partitions));
  }
  DBUG_RETURN(0);
}


/**
  Resize key cache.
*/
int ha_resize_key_cache(KEY_CACHE *key_cache)
{
  DBUG_ENTER("ha_resize_key_cache");

  if (key_cache->key_cache_inited)
  {
    mysql_mutex_lock(&LOCK_global_system_variables);
    size_t tmp_buff_size= (size_t) key_cache->param_buff_size;
    long tmp_block_size= (long) key_cache->param_block_size;
    uint division_limit= (uint)key_cache->param_division_limit;
    uint age_threshold=  (uint)key_cache->param_age_threshold;
    uint changed_blocks_hash_size=  (uint)key_cache->changed_blocks_hash_size;
    mysql_mutex_unlock(&LOCK_global_system_variables);
    DBUG_RETURN(!resize_key_cache(key_cache, tmp_block_size,
				  tmp_buff_size,
				  division_limit, age_threshold,
                                  changed_blocks_hash_size));
  }
  DBUG_RETURN(0);
}


/**
  Change parameters for key cache (like division_limit)
*/
int ha_change_key_cache_param(KEY_CACHE *key_cache)
{
  DBUG_ENTER("ha_change_key_cache_param");

  if (key_cache->key_cache_inited)
  {
    mysql_mutex_lock(&LOCK_global_system_variables);
    uint division_limit= (uint)key_cache->param_division_limit;
    uint age_threshold=  (uint)key_cache->param_age_threshold;
    mysql_mutex_unlock(&LOCK_global_system_variables);
    change_key_cache_param(key_cache, division_limit, age_threshold);
  }
  DBUG_RETURN(0);
}


/**
  Repartition key cache 
*/
int ha_repartition_key_cache(KEY_CACHE *key_cache)
{
  DBUG_ENTER("ha_repartition_key_cache");

  if (key_cache->key_cache_inited)
  {
    mysql_mutex_lock(&LOCK_global_system_variables);
    size_t tmp_buff_size= (size_t) key_cache->param_buff_size;
    long tmp_block_size= (long) key_cache->param_block_size;
    uint division_limit= (uint)key_cache->param_division_limit;
    uint age_threshold=  (uint)key_cache->param_age_threshold;
    uint partitions=     (uint)key_cache->param_partitions;
    uint changed_blocks_hash_size=  (uint)key_cache->changed_blocks_hash_size;
    mysql_mutex_unlock(&LOCK_global_system_variables);
    DBUG_RETURN(!repartition_key_cache(key_cache, tmp_block_size,
				       tmp_buff_size,
				       division_limit, age_threshold,
                                       changed_blocks_hash_size,
                                       partitions));
  }
  DBUG_RETURN(0);
}


/**
  Move all tables from one key cache to another one.
*/
int ha_change_key_cache(KEY_CACHE *old_key_cache,
			KEY_CACHE *new_key_cache)
{
  mi_change_key_cache(old_key_cache, new_key_cache);
  return 0;
}


static my_bool discover_handlerton(THD *thd, plugin_ref plugin,
                                   void *arg)
{
  TABLE_SHARE *share= (TABLE_SHARE *)arg;
  handlerton *hton= plugin_hton(plugin);
  if (hton->discover_table)
  {
    share->db_plugin= plugin;
    int error= hton->discover_table(hton, thd, share);
    if (error != HA_ERR_NO_SUCH_TABLE)
    {
      if (unlikely(error))
      {
        if (!share->error)
        {
          share->error= OPEN_FRM_ERROR_ALREADY_ISSUED;
          plugin_unlock(0, share->db_plugin);
        }

        /*
          report an error, unless it is "generic" and a more
          specific one was already reported
        */
        if (error != HA_ERR_GENERIC || !thd->is_error())
          my_error(ER_GET_ERRNO, MYF(0), error, plugin_name(plugin)->str);
        share->db_plugin= 0;
      }
      else
        share->error= OPEN_FRM_OK;

      status_var_increment(thd->status_var.ha_discover_count);
      return TRUE; // abort the search
    }
    share->db_plugin= 0;
  }

  DBUG_ASSERT(share->error == OPEN_FRM_OPEN_ERROR);
  return FALSE;    // continue with the next engine
}

int ha_discover_table(THD *thd, TABLE_SHARE *share)
{
  DBUG_ENTER("ha_discover_table");
  int found;

  DBUG_ASSERT(share->error == OPEN_FRM_OPEN_ERROR);   // share is not OK yet

  if (!engines_with_discover)
    found= FALSE;
  else if (share->db_plugin)
    found= discover_handlerton(thd, share->db_plugin, share);
  else
    found= plugin_foreach(thd, discover_handlerton,
                        MYSQL_STORAGE_ENGINE_PLUGIN, share);

  if (thd->lex->query_tables && thd->lex->query_tables->sequence && !found)
    my_error(ER_UNKNOWN_SEQUENCES, MYF(0),share->table_name.str);
  if (!found)
    open_table_error(share, OPEN_FRM_OPEN_ERROR, ENOENT); // not found

  DBUG_RETURN(share->error != OPEN_FRM_OK);
}

static my_bool file_ext_exists(char *path, size_t path_len, const char *ext)
{
  strmake(path + path_len, ext, FN_REFLEN - path_len);
  return !access(path, F_OK);
}

struct st_discover_existence_args
{
  char *path;
  size_t  path_len;
  const char *db, *table_name;
  handlerton *hton;
  bool frm_exists;
};

static my_bool discover_existence(THD *thd, plugin_ref plugin,
                                  void *arg)
{
  st_discover_existence_args *args= (st_discover_existence_args*)arg;
  handlerton *ht= plugin_hton(plugin);
  if (!ht->discover_table_existence)
    return args->frm_exists;

  args->hton= ht;

  if (ht->discover_table_existence == ext_based_existence)
    return file_ext_exists(args->path, args->path_len,
                           ht->tablefile_extensions[0]);

  return ht->discover_table_existence(ht, args->db, args->table_name);
}


/**
  Check if a given table exists, without doing a full discover, if possible

  If the 'hton' is not NULL, it's set to the handlerton of the storage engine
  of this table, or to view_pseudo_hton if the frm belongs to a view.

  This function takes discovery correctly into account. If frm is found,
  it discovers the table to make sure it really exists in the engine.
  If no frm is found it discovers the table, in case it still exists in
  the engine.

  While it tries to cut corners (don't open .frm if no discovering engine is
  enabled, no full discovery if all discovering engines support
  discover_table_existence, etc), it still *may* be quite expensive
  and must be used sparingly.

  @retval true    Table exists (even if the error occurred, like bad frm)
  @retval false   Table does not exist (one can do CREATE TABLE table_name)

  @note if frm exists and the table in engine doesn't, *hton will be set,
        but the return value will be false.

  @note if frm file exists, but the table cannot be opened (engine not
        loaded, frm is invalid), the return value will be true, but
        *hton will be NULL.
*/

bool ha_table_exists(THD *thd, const LEX_CSTRING *db,
                     const LEX_CSTRING *table_name, LEX_CUSTRING *table_id,
                     handlerton **hton, bool *is_sequence)
{
  handlerton *dummy;
  bool dummy2;
  DBUG_ENTER("ha_table_exists");

  if (hton)
    *hton= 0;
  else if (engines_with_discover)
    hton= &dummy;
  if (!is_sequence)
    is_sequence= &dummy2;
  *is_sequence= 0;
  if (table_id)
  {
    table_id->str= 0;
    table_id->length= 0;
  }

  TDC_element *element= tdc_lock_share(thd, db->str, table_name->str);
  if (element && element != MY_ERRPTR)
  {
    if (!hton)
      hton= &dummy;
    *hton= element->share->db_type();
    *is_sequence= element->share->table_type == TABLE_TYPE_SEQUENCE;
    if (*hton != view_pseudo_hton && element->share->tabledef_version.length &&
        table_id &&
        (table_id->str= (uchar*)
         thd->memdup(element->share->tabledef_version.str, MY_UUID_SIZE)))
      table_id->length= MY_UUID_SIZE;
    tdc_unlock_share(element);
    DBUG_RETURN(TRUE);
  }

  char path[FN_REFLEN + 1];
  size_t path_len = build_table_filename(path, sizeof(path) - 1,
                                         db->str, table_name->str, "", 0);
  st_discover_existence_args args= {path, path_len, db->str, table_name->str, 0, true};

  if (file_ext_exists(path, path_len, reg_ext))
  {
    bool exists= true;
    if (hton)
    {
      char engine_buf[NAME_CHAR_LEN + 1];
      LEX_CSTRING engine= { engine_buf, 0 };
      Table_type type= dd_frm_type(thd, path, &engine, table_id);

      switch (type) {
      case TABLE_TYPE_UNKNOWN:
        DBUG_PRINT("exit", ("Exist, cannot be opened"));
        DBUG_RETURN(true);                      // Frm exists
      case TABLE_TYPE_VIEW:
        *hton= view_pseudo_hton;
        DBUG_PRINT("exit", ("Exist, view"));
        DBUG_RETURN(true);                      // Frm exists
      case TABLE_TYPE_SEQUENCE:
        *is_sequence= true;
        /* fall through */
      case TABLE_TYPE_NORMAL:
        {
          plugin_ref p=  plugin_lock_by_name(thd, &engine,
                                             MYSQL_STORAGE_ENGINE_PLUGIN);
          *hton= p ? plugin_hton(p) : NULL;
          if (*hton)      // verify that the table really exists
            exists= discover_existence(thd, p, &args);
        }
      }
    }
    DBUG_PRINT("exit", (exists ? "Exists" : "Does not exist"));
    DBUG_RETURN(exists);
  }

  args.frm_exists= false;
  if (plugin_foreach(thd, discover_existence, MYSQL_STORAGE_ENGINE_PLUGIN,
                     &args))
  {
    if (hton)
      *hton= args.hton;
    DBUG_PRINT("exit", ("discovery found file"));
    DBUG_RETURN(TRUE);
  }

  if (need_full_discover_for_existence)
  {
    TABLE_LIST table;
    bool exists;
    uint flags = GTS_TABLE | GTS_VIEW;

    if (!hton)
      flags|= GTS_NOLOCK;

    Table_exists_error_handler no_such_table_handler;
    thd->push_internal_handler(&no_such_table_handler);
    table.init_one_table(db, table_name, 0, TL_READ);
    TABLE_SHARE *share= tdc_acquire_share(thd, &table, flags);
    thd->pop_internal_handler();

    if (hton && share)
    {
      *hton= share->db_type();
      if (table_id && share->tabledef_version.length &&
          (table_id->str=
           (uchar*) thd->memdup(share->tabledef_version.str, MY_UUID_SIZE)))
        table_id->length= MY_UUID_SIZE;
      tdc_release_share(share);
    }

    // the table doesn't exist if we've caught ER_NO_SUCH_TABLE and nothing else
    exists= !no_such_table_handler.safely_trapped_errors();
    DBUG_PRINT("exit", (exists ? "Exists" : "Does not exist"));
    DBUG_RETURN(exists);
  }

  DBUG_PRINT("exit", ("Does not exist"));
  DBUG_RETURN(FALSE);
}


/*
  Check if the CREATE/ALTER table should be ignored
  This could happen for slaves where the table is shared between master
  and slave

  If statement is ignored, write a note
*/

bool handler::check_if_updates_are_ignored(const char *op) const
{
  return ha_check_if_updates_are_ignored(table->in_use, ht, op);
}


bool ha_check_if_updates_are_ignored(THD *thd, handlerton *hton,
                                     const char *op)
{
  DBUG_ENTER("ha_check_if_updates_are_ignored");
  if (!thd->slave_thread || !(hton= ha_checktype(thd, hton, 1)))
    DBUG_RETURN(0);                                   // Not slave or no engine
  if (!(hton->flags & HTON_IGNORE_UPDATES))
    DBUG_RETURN(0);                                   // Not shared table
  my_error(ER_SLAVE_IGNORED_SHARED_TABLE, MYF(ME_NOTE), op);
  DBUG_RETURN(1);
}


/**
  Discover all table names in a given database
*/
extern "C" {

static int cmp_file_names(const void *a, const void *b)
{
  CHARSET_INFO *cs= character_set_filesystem;
  char *aa= ((FILEINFO *)a)->name;
  char *bb= ((FILEINFO *)b)->name;
  return cs->strnncoll(aa, strlen(aa), bb, strlen(bb));
}

static int cmp_table_names(const void *a_, const void *b_)
{
  auto a= static_cast<const LEX_CSTRING *const *>(a_);
  auto b= static_cast<const LEX_CSTRING *const *>(b_);
  return my_charset_bin.strnncoll((*a)->str, (*a)->length,
                                  (*b)->str, (*b)->length);
}

#ifndef DBUG_OFF
static int cmp_table_names_desc(const void *a_, const void *b_)
{
  auto a= static_cast<const LEX_CSTRING *const *>(a_);
  auto b= static_cast<const LEX_CSTRING *const *>(b_);
  return -cmp_table_names(a, b);
}
#endif

}

Discovered_table_list::Discovered_table_list(THD *thd_arg,
                 Dynamic_array<LEX_CSTRING*> *tables_arg,
                 const LEX_CSTRING *wild_arg) :
  thd(thd_arg), with_temps(false), tables(tables_arg)
{
  if (wild_arg->str && wild_arg->str[0])
  {
    wild= wild_arg->str;
    wend= wild + wild_arg->length;
  }
  else
    wild= 0;
}

bool Discovered_table_list::add_table(const char *tname, size_t tlen)
{
  /*
    TODO Check with_temps and filter out temp tables.
    Implement the check, when we'll have at least one affected engine (with
    custom discover_table_names() method, that calls add_table() directly).
    Note: avoid comparing the same name twice (here and in add_file).
  */
  if (wild && table_alias_charset->wildcmp(tname, tname + tlen, wild, wend,
                                           wild_prefix, wild_one, wild_many))
      return 0;

  LEX_CSTRING *name= thd->make_clex_string(tname, tlen);
  if (!name || tables->append(name))
    return 1;
  return 0;
}

bool Discovered_table_list::add_file(const char *fname)
{
  bool is_temp= strncmp(fname, STRING_WITH_LEN(tmp_file_prefix)) == 0;

  if ((is_temp && !with_temps) || !strncmp(fname,STRING_WITH_LEN(ROCKSDB_DIRECTORY_NAME)))
    return 0;

  char tname[SAFE_NAME_LEN + 1];
  size_t tlen= filename_to_tablename(fname, tname, sizeof(tname), is_temp);
  return add_table(tname, tlen);
}


void Discovered_table_list::sort()
{
  tables->sort(cmp_table_names);
}


#ifndef DBUG_OFF
void Discovered_table_list::sort_desc()
{
  tables->sort(cmp_table_names_desc);
}
#endif


void Discovered_table_list::remove_duplicates()
{
  LEX_CSTRING **src= tables->front();
  LEX_CSTRING **dst= src;
  sort();
  while (++dst <= tables->back())
  {
    LEX_CSTRING *s= *src, *d= *dst;
    DBUG_ASSERT(strncmp(s->str, d->str, MY_MIN(s->length, d->length)) <= 0);
    if ((s->length != d->length || strncmp(s->str, d->str, d->length)))
    {
      src++;
      if (src != dst)
        *src= *dst;
    }
  }
  tables->elements(src - tables->front() + 1);
}

struct st_discover_names_args
{
  const LEX_CSTRING *db;
  MY_DIR *dirp;
  Discovered_table_list *result;
  uint possible_duplicates;
};

static my_bool discover_names(THD *thd, plugin_ref plugin,
                              void *arg)
{
  st_discover_names_args *args= (st_discover_names_args *)arg;
  handlerton *ht= plugin_hton(plugin);

  if (ht->discover_table_names)
  {
    size_t old_elements= args->result->tables->elements();
    if (ht->discover_table_names(ht, args->db, args->dirp, args->result))
      return 1;

    /*
      hton_ext_based_table_discovery never discovers a table that has
      a corresponding .frm file; but custom engine discover methods might
    */
    if (ht->discover_table_names != hton_ext_based_table_discovery)
      args->possible_duplicates+= (uint)(args->result->tables->elements() - old_elements);
  }

  return 0;
}

/**
  Return the list of tables

  @param thd
  @param db         database to look into
  @param dirp       list of files in this database (as returned by my_dir())
  @param result     the object to return the list of files in
  @param reusable   if true, on return, 'dirp' will be a valid list of all
                    non-table files. If false, discovery will work much faster,
                    but it will leave 'dirp' corrupted and completely unusable,
                    only good for my_dirend().

  Normally, reusable=false for SHOW and INFORMATION_SCHEMA, and reusable=true
  for DROP DATABASE (as it needs to know and delete non-table files).
*/

int ha_discover_table_names(THD *thd, const LEX_CSTRING *db, MY_DIR *dirp,
                            Discovered_table_list *result, bool reusable)
{
  int error;
  DBUG_ENTER("ha_discover_table_names");

  if (engines_with_discover_file_names == 0 && !reusable)
  {
    st_discover_names_args args= {db, NULL, result, 0};
    error= ext_table_discovery_simple(dirp, result) ||
           plugin_foreach(thd, discover_names,
                            MYSQL_STORAGE_ENGINE_PLUGIN, &args);
    if (args.possible_duplicates > 0)
      result->remove_duplicates();
  }
  else
  {
    st_discover_names_args args= {db, dirp, result, 0};

    /* extension_based_table_discovery relies on dirp being sorted */
    my_qsort(dirp->dir_entry, dirp->number_of_files,
             sizeof(FILEINFO), cmp_file_names);

    error= extension_based_table_discovery(dirp, reg_ext, result) ||
           plugin_foreach(thd, discover_names,
                            MYSQL_STORAGE_ENGINE_PLUGIN, &args);
    if (args.possible_duplicates > 0)
      result->remove_duplicates();
  }

  DBUG_RETURN(error);
}


/*
int handler::pre_read_multi_range_first(KEY_MULTI_RANGE **found_range_p,
                                        KEY_MULTI_RANGE *ranges,
                                        uint range_count,
                                        bool sorted, HANDLER_BUFFER *buffer,
                                        bool use_parallel)
{
  int result;
  DBUG_ENTER("handler::pre_read_multi_range_first");
  result = pre_read_range_first(ranges->start_key.keypart_map ?
                                &ranges->start_key : 0,
                                ranges->end_key.keypart_map ?
                                &ranges->end_key : 0,
                                test(ranges->range_flag & EQ_RANGE),
                                sorted,
                                use_parallel);
  DBUG_RETURN(result);
}
*/


/**
  Read first row between two ranges.
  Store ranges for future calls to read_range_next.

  @param start_key		Start key. Is 0 if no min range
  @param end_key		End key.  Is 0 if no max range
  @param eq_range_arg	        Set to 1 if start_key == end_key
  @param sorted		Set to 1 if result should be sorted per key

  @note
    Record is read into table->record[0]

  @retval
    0			Found row
  @retval
    HA_ERR_END_OF_FILE	No rows in range
  @retval
    \#			Error code
*/
int handler::read_range_first(const key_range *start_key,
			      const key_range *end_key,
			      bool eq_range_arg, bool sorted)
{
  int result;
  DBUG_ENTER("handler::read_range_first");

  eq_range= eq_range_arg;
  set_end_range(end_key);
  range_key_part= table->key_info[active_index].key_part;

  if (!start_key)			// Read first record
    result= ha_index_first(table->record[0]);
  else
    result= ha_index_read_map(table->record[0],
                              start_key->key,
                              start_key->keypart_map,
                              start_key->flag);
  if (result)
    DBUG_RETURN((result == HA_ERR_KEY_NOT_FOUND) 
		? HA_ERR_END_OF_FILE
		: result);

  if (compare_key(end_range) <= 0)
  {
    DBUG_RETURN(0);
  }
  else
  {
    /*
      The last read row does not fall in the range. So request
      storage engine to release row lock if possible.
    */
    unlock_row();
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
}


/**
  Read next row between two ranges.

  @note
    Record is read into table->record[0]

  @retval
    0			Found row
  @retval
    HA_ERR_END_OF_FILE	No rows in range
  @retval
    \#			Error code
*/
int handler::read_range_next()
{
  int result;
  DBUG_ENTER("handler::read_range_next");

  if (eq_range)
  {
    /* We trust that index_next_same always gives a row in range */
    DBUG_RETURN(ha_index_next_same(table->record[0],
                                   end_range->key,
                                   end_range->length));
  }
  result= ha_index_next(table->record[0]);
  if (result)
    DBUG_RETURN(result);

  if (compare_key(end_range) <= 0)
  {
    DBUG_RETURN(0);
  }
  else
  {
    /*
      The last read row does not fall in the range. So request
      storage engine to release row lock if possible.
    */
    unlock_row();
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
}


void handler::set_end_range(const key_range *end_key)
{
  end_range= 0;
  if (end_key)
  {
    end_range= &save_end_range;
    save_end_range= *end_key;
    key_compare_result_on_equal=
      ((end_key->flag == HA_READ_BEFORE_KEY) ? 1 :
       (end_key->flag == HA_READ_AFTER_KEY) ? -1 : 0);
  }
}


/**
  Compare if found key (in row) is over max-value.

  @param range		range to compare to row. May be 0 for no range

  @see also
    key.cc::key_cmp()

  @return
    The return value is SIGN(key_in_row - range_key):

    - 0   : Key is equal to range or 'range' == 0 (no range)
    - -1  : Key is less than range
    - 1   : Key is larger than range
*/
int handler::compare_key(key_range *range)
{
  int cmp;
  if (!range || in_range_check_pushed_down)
    return 0;					// No max range
  cmp= key_cmp(range_key_part, range->key, range->length);
  if (!cmp)
    cmp= key_compare_result_on_equal;
  return cmp;
}


/*
  Same as compare_key() but doesn't check have in_range_check_pushed_down.
  This is used by index condition pushdown implementation.
*/

int handler::compare_key2(key_range *range) const
{
  int cmp;
  if (!range)
    return 0;					// no max range
  cmp= key_cmp(range_key_part, range->key, range->length);
  if (!cmp)
    cmp= key_compare_result_on_equal;
  return cmp;
}


/**
  ICP callback - to be called by an engine to check the pushed condition
*/
extern "C" check_result_t handler_index_cond_check(void* h_arg)
{
  handler *h= (handler*)h_arg;
  THD *thd= h->table->in_use;
  check_result_t res;

  DEBUG_SYNC(thd, "handler_index_cond_check");

  enum thd_kill_levels killed= thd_kill_level(thd);
  if (unlikely(killed != THD_IS_NOT_KILLED))
  {
    enum thd_kill_levels abort_at= (h->has_transactions() ?
                                    THD_ABORT_SOFTLY :
                                    THD_ABORT_ASAP);
    if (killed > abort_at)
      return CHECK_ABORTED_BY_USER;
  }
  if (unlikely(h->end_range) && h->compare_key2(h->end_range) > 0)
    return CHECK_OUT_OF_RANGE;
  h->increment_statistics(&SSV::ha_icp_attempts);
  if (unlikely(h->handler_stats))
    h->handler_stats->icp_attempts++;
  res= CHECK_NEG;
  if  (h->pushed_idx_cond->val_bool())
  {
    res= CHECK_POS;
    h->fast_increment_statistics(&SSV::ha_icp_match);
    if (unlikely(h->handler_stats))
      h->handler_stats->icp_match++;
  }
  return res;
}


/**
  Rowid filter callback - to be called by an engine to check rowid / primary
  keys of the rows whose data is to be fetched against the used rowid filter
*/

extern "C"
check_result_t handler_rowid_filter_check(void *h_arg)
{
  handler *h= (handler*) h_arg;
  TABLE *tab= h->get_table();

  /*
    Check for out-of-range and killed conditions only if we haven't done it
    already in the pushed index condition check
  */
  if (!h->pushed_idx_cond)
  {
    THD *thd= h->table->in_use;
    DEBUG_SYNC(thd, "handler_rowid_filter_check");

    enum thd_kill_levels killed= thd_kill_level(thd);
    if (unlikely(killed != THD_IS_NOT_KILLED))
    {
      enum thd_kill_levels abort_at= (h->has_transactions() ?
                                      THD_ABORT_SOFTLY :
                                      THD_ABORT_ASAP);
      if (killed > abort_at)
        return CHECK_ABORTED_BY_USER;
    }

    if (h->end_range && h->compare_key2(h->end_range) > 0)
      return CHECK_OUT_OF_RANGE;
  }

  h->position(tab->record[0]);
  return h->pushed_rowid_filter->check((char*)h->ref) ? CHECK_POS: CHECK_NEG;
}


/**
  Callback function for an engine to check whether the used rowid filter
  has been already built
*/

extern "C" int handler_rowid_filter_is_active(void *h_arg)
{
  DBUG_ASSERT(h_arg);
  handler *h= (handler*) h_arg;
  return h->rowid_filter_is_active;
}


int handler::index_read_idx_map(uchar * buf, uint index, const uchar * key,
                                key_part_map keypart_map,
                                enum ha_rkey_function find_flag)
{
  int error, UNINIT_VAR(error1);

  error= ha_index_init(index, 0);
  if (likely(!error))
  {
    error= index_read_map(buf, key, keypart_map, find_flag);
    error1= ha_index_end();
  }
  return error ? error : error1;
}


/**
  Returns a list of all known extensions.

    No mutexes, worst case race is a minor surplus memory allocation
    We have to recreate the extension map if mysqld is restarted (for example
    within libmysqld)

  @retval
    pointer		pointer to TYPELIB structure
*/
static my_bool exts_handlerton(THD *, plugin_ref plugin, void *arg)
{
  List<char> *found_exts= (List<char> *) arg;
  handlerton *hton= plugin_hton(plugin);
  List_iterator_fast<char> it(*found_exts);
  const char **ext, *old_ext;

  for (ext= hton->tablefile_extensions; *ext; ext++)
  {
    while ((old_ext= it++))
    {
      if (!strcmp(old_ext, *ext))
        break;
    }
    if (!old_ext)
      found_exts->push_back((char *) *ext);

    it.rewind();
  }
  return FALSE;
}

TYPELIB *ha_known_exts(void)
{
  if (!known_extensions.type_names || mysys_usage_id != known_extensions_id)
  {
    List<char> found_exts;
    const char **ext, *old_ext;

    known_extensions_id= mysys_usage_id;
    found_exts.push_back((char*) TRG_EXT);
    found_exts.push_back((char*) TRN_EXT);

    plugin_foreach(NULL, exts_handlerton,
                   MYSQL_STORAGE_ENGINE_PLUGIN, &found_exts);

    ext= (const char **) my_once_alloc(sizeof(char *)*
                                       (found_exts.elements+1),
                                       MYF(MY_WME | MY_FAE));

    DBUG_ASSERT(ext != 0);
    known_extensions.count= found_exts.elements;
    known_extensions.type_names= ext;

    List_iterator_fast<char> it(found_exts);
    while ((old_ext= it++))
      *ext++= old_ext;
    *ext= 0;
  }
  return &known_extensions;
}


static bool stat_print(THD *thd, const char *type, size_t type_len,
                       const char *file, size_t file_len,
                       const char *status, size_t status_len)
{
  Protocol *protocol= thd->protocol;
  protocol->prepare_for_resend();
  protocol->store(type, type_len, system_charset_info);
  protocol->store(file, file_len, system_charset_info);
  protocol->store(status, status_len, system_charset_info);
  if (protocol->write())
    return TRUE;
  return FALSE;
}


static my_bool showstat_handlerton(THD *thd, plugin_ref plugin,
                                   void *arg)
{
  enum ha_stat_type stat= *(enum ha_stat_type *) arg;
  handlerton *hton= plugin_hton(plugin);
  if (hton->show_status &&
      hton->show_status(hton, thd, stat_print, stat))
    return TRUE;
  return FALSE;
}

bool ha_show_status(THD *thd, handlerton *db_type, enum ha_stat_type stat)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  MEM_ROOT *mem_root= thd->mem_root;
  bool result;

  field_list.push_back(new (mem_root) Item_empty_string(thd, "Type", 10),
                       mem_root);
  field_list.push_back(new (mem_root)
                       Item_empty_string(thd, "Name", FN_REFLEN), mem_root);
  field_list.push_back(new (mem_root)
                       Item_empty_string(thd, "Status", 10),
                       mem_root);

  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    return TRUE;

  if (db_type == NULL)
  {
    result= plugin_foreach(thd, showstat_handlerton,
                           MYSQL_STORAGE_ENGINE_PLUGIN, &stat);
  }
  else
  {
    result= db_type->show_status &&
            db_type->show_status(db_type, thd, stat_print, stat) ? 1 : 0;
  }

  /*
    We also check thd->is_error() as Innodb may return 0 even if
    there was an error.
  */
  if (likely(!result && !thd->is_error()))
    my_eof(thd);
  else if (!thd->is_error())
    my_error(ER_GET_ERRNO, MYF(0), errno, hton_name(db_type)->str);
  return result;
}

/*
  Function to check if the conditions for row-based binlogging is
  correct for the table.

  A row in the given table should be replicated if:
  - It's not called by partition engine
  - Row-based replication is enabled in the current thread
  - The binlog is enabled
  - It is not a temporary table
  - The binary log is open
  - The database the table resides in shall be binlogged (binlog_*_db rules)
  - table is not mysql.event

  RETURN VALUE
    0  No binary logging in row format
    1  Row needs to be logged
*/

bool handler::check_table_binlog_row_based()
{
  if (unlikely((!check_table_binlog_row_based_done)))
  {
    check_table_binlog_row_based_done= 1;
    check_table_binlog_row_based_result=
      check_table_binlog_row_based_internal();
  }
  return check_table_binlog_row_based_result;
}

bool handler::check_table_binlog_row_based_internal()
{
  THD *thd= table->in_use;

#ifdef WITH_WSREP
  if (!thd->variables.sql_log_bin &&
      wsrep_thd_is_applying(table->in_use))
  {
    /*
      wsrep patch sets sql_log_bin to silence binlogging from high
      priority threads
    */
    return 0;
  }
#endif
  return (table->s->can_do_row_logging &&
          !table->versioned(VERS_TRX_ID) &&
          !(thd->variables.option_bits & OPTION_BIN_TMP_LOG_OFF) &&
          thd->is_current_stmt_binlog_format_row() &&
          /*
            Wsrep partially enables binary logging if it have not been
            explicitly turned on. As a result we return 'true' if we are in
            wsrep binlog emulation mode and the current thread is not a wsrep
            applier or replayer thread. This decision is not affected by
            @@sql_log_bin as we want the events to make into the binlog
            cache only to filter them later before they make into binary log
            file.

            However, we do return 'false' if binary logging was temporarily
            turned off (see tmp_disable_binlog(A)).

            Otherwise, return 'true' if binary logging is on.
          */
          IF_WSREP(((WSREP_EMULATE_BINLOG_NNULL(thd) &&
                     wsrep_thd_is_local(thd)) ||
                    ((WSREP_NNULL(thd) ||
                      (thd->variables.option_bits & OPTION_BIN_LOG)) &&
                     mysql_bin_log.is_open())),
                    (thd->variables.option_bits & OPTION_BIN_LOG) &&
                    mysql_bin_log.is_open()));
}

static int binlog_log_row_to_binlog(TABLE* table,
                                    const uchar *before_record,
                                    const uchar *after_record,
                                    Log_func *log_func,
                                    bool has_trans)
{
  bool error= 0;
  THD *const thd= table->in_use;

  DBUG_ENTER("binlog_log_row_to_binlog");

  if (!thd->binlog_table_maps &&
      thd->binlog_write_table_maps())
    DBUG_RETURN(HA_ERR_RBR_LOGGING_FAILED);

  DBUG_ASSERT(thd->is_current_stmt_binlog_format_row());
  DBUG_ASSERT((WSREP_NNULL(thd) && wsrep_emulate_bin_log)
              || mysql_bin_log.is_open());

  auto *cache_mngr= thd->binlog_setup_trx_data();
  if (cache_mngr == NULL)
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  /* Ensure that all events in a GTID group are in the same cache */
  if (thd->variables.option_bits & OPTION_GTID_BEGIN)
    has_trans= 1;

  auto *cache= binlog_get_cache_data(cache_mngr,
                                     use_trans_cache(thd, has_trans));

    error= (*log_func)(thd, table, mysql_bin_log.as_event_log(), cache,
                       has_trans, thd->variables.binlog_row_image,
                       before_record, after_record);
  DBUG_RETURN(error ? HA_ERR_RBR_LOGGING_FAILED : 0);
}

int handler::binlog_log_row(const uchar *before_record,
                            const uchar *after_record,
                            Log_func *log_func)
{
  DBUG_ENTER("handler::binlog_log_row");

  int error = 0;
  if (row_logging)
    error= binlog_log_row_to_binlog(table, before_record, after_record,
                                    log_func, row_logging_has_trans);

#ifdef HAVE_REPLICATION
  if (unlikely(!error && table->s->online_alter_binlog && is_root_handler()))
    error= online_alter_log_row(table, before_record, after_record,
                                log_func);
#endif // HAVE_REPLICATION

  DBUG_RETURN(error);
}


int handler::ha_external_lock(THD *thd, int lock_type)
{
  int error;
  DBUG_ENTER("handler::ha_external_lock");
  /*
    Whether this is lock or unlock, this should be true, and is to verify that
    if get_auto_increment() was called (thus may have reserved intervals or
    taken a table lock), ha_release_auto_increment() was too.
  */
  DBUG_ASSERT(next_insert_id == 0);
  /* Consecutive calls for lock without unlocking in between is not allowed */
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              ((lock_type != F_UNLCK && m_lock_type == F_UNLCK) ||
               lock_type == F_UNLCK));
  /* SQL HANDLER call locks/unlock while scanning (RND/INDEX). */
  DBUG_ASSERT(inited == NONE || table->open_by_handler);

  if (MYSQL_HANDLER_RDLOCK_START_ENABLED() ||
      MYSQL_HANDLER_WRLOCK_START_ENABLED() ||
      MYSQL_HANDLER_UNLOCK_START_ENABLED())
  {
    if (lock_type == F_RDLCK)
    {
      MYSQL_HANDLER_RDLOCK_START(table_share->db.str,
                                 table_share->table_name.str);
    }
    else if (lock_type == F_WRLCK)
    {
      MYSQL_HANDLER_WRLOCK_START(table_share->db.str,
                                 table_share->table_name.str);
    }
    else if (lock_type == F_UNLCK)
    {
      MYSQL_HANDLER_UNLOCK_START(table_share->db.str,
                                 table_share->table_name.str);
    }
  }

  if (lock_type == F_UNLCK)
    (void) table->unlock_hlindexes();

  /*
    We cache the table flags if the locking succeeded. Otherwise, we
    keep them as they were when they were fetched in ha_open().
  */
  MYSQL_TABLE_LOCK_WAIT(PSI_TABLE_EXTERNAL_LOCK, lock_type,
    { error= external_lock(thd, lock_type); })

  DBUG_EXECUTE_IF("external_lock_failure", error= HA_ERR_GENERIC;);

  if (likely(error == 0 || lock_type == F_UNLCK))
  {
    m_lock_type= lock_type;
    cached_table_flags= table_flags();
    if (table_share->tmp_table == NO_TMP_TABLE)
      mysql_audit_external_lock(thd, table_share, lock_type);
  }

  if (MYSQL_HANDLER_RDLOCK_DONE_ENABLED() ||
      MYSQL_HANDLER_WRLOCK_DONE_ENABLED() ||
      MYSQL_HANDLER_UNLOCK_DONE_ENABLED())
  {
    if (lock_type == F_RDLCK)
    {
      MYSQL_HANDLER_RDLOCK_DONE(error);
    }
    else if (lock_type == F_WRLCK)
    {
      MYSQL_HANDLER_WRLOCK_DONE(error);
    }
    else if (lock_type == F_UNLCK)
    {
      MYSQL_HANDLER_UNLOCK_DONE(error);
    }
  }
  DBUG_RETURN(error);
}


/** @brief
  Check handler usage and reset state of file to after 'open'
*/
int handler::ha_reset()
{
  DBUG_ENTER("ha_reset");

  /* Check that we have called all proper deallocation functions */
  DBUG_ASSERT((uchar*) table->def_read_set.bitmap +
              table->s->column_bitmap_size ==
              (uchar*) table->def_write_set.bitmap);
  DBUG_ASSERT(bitmap_is_set_all(&table->s->all_set));
  DBUG_ASSERT(!table->file->keyread_enabled());
  /* ensure that ha_index_end / ha_rnd_end has been called */
  DBUG_ASSERT(inited == NONE);
  /* reset the bitmaps to point to defaults */
  table->default_column_bitmaps();
  pushed_cond= NULL;
  tracker= NULL;
  mark_trx_read_write_done= 0;
  /*
    Disable row logging.
  */
  row_logging= row_logging_init= 0;
  clear_cached_table_binlog_row_based_flag();
  /* Reset information about pushed engine conditions */
  cancel_pushed_idx_cond();
  /* Reset information about pushed index conditions */
  cancel_pushed_rowid_filter();
  if (lookup_handler != this)
  {
    lookup_handler->ha_external_unlock(table->in_use);
    lookup_handler->close();
    PSI_CALL_close_table(table_share, lookup_handler->m_psi);
    delete lookup_handler;
    lookup_handler= this;
  }
  DBUG_RETURN(reset());
}

static int wsrep_after_row(THD *thd)
{
  DBUG_ENTER("wsrep_after_row");
#ifdef WITH_WSREP
  if (thd->internal_transaction())
    DBUG_RETURN(0);

  /* enforce wsrep_max_ws_rows */
  thd->wsrep_affected_rows++;
  if (wsrep_max_ws_rows &&
      thd->wsrep_affected_rows > wsrep_max_ws_rows &&
      wsrep_thd_is_local(thd))
  {
    /*
      If we are inside stored function or trigger we should not commit or
      rollback current statement transaction. See comment in ha_commit_trans()
      call for more information.
    */
    if (!thd->in_sub_stmt)
      trans_rollback_stmt(thd) || trans_rollback(thd);
    my_message(ER_ERROR_DURING_COMMIT, "wsrep_max_ws_rows exceeded", MYF(0));
    DBUG_RETURN(ER_ERROR_DURING_COMMIT);
  }
  else if (wsrep_after_row_internal(thd))
  {
    DBUG_RETURN(ER_LOCK_DEADLOCK);
  }
#endif /* WITH_WSREP */
  DBUG_RETURN(0);
}


static bool long_unique_fields_differ(KEY *keyinfo, const uchar *other)
{
  uint key_parts= fields_in_hash_keyinfo(keyinfo);
  KEY_PART_INFO *keypart= keyinfo->key_part - key_parts;
  my_ptrdiff_t off= other - keypart->field->table->record[0];
  DBUG_ASSERT(off);
  do
  {
    Field *field= keypart->field;
    if (field->is_null() || field->is_null(off))
      return true;
    else if (f_is_blob(keypart->key_type) && keypart->length)
    {
      if (field->cmp_prefix(field->ptr, field->ptr + off, keypart->length))
        return true;
    }
    else if (field->cmp_offset(off))
      return true;
  } while (keypart++ < keyinfo->key_part);
  return false;
}


/**
   Check if there is a conflicting unique hash key
*/

int handler::check_duplicate_long_entry_key(const uchar *new_rec, uint key_no)
{
  int result;
  /* Skip just written row in the case of HA_CHECK_UNIQUE_AFTER_WRITE */
  bool skip_self= ha_table_flags() & HA_CHECK_UNIQUE_AFTER_WRITE;
  KEY *key_info= table->key_info + key_no;
  uchar ptr[HA_HASH_KEY_LENGTH_WITH_NULL];
  DBUG_ENTER("handler::check_duplicate_long_entry_key");

  DBUG_ASSERT((key_info->flags & HA_NULL_PART_KEY &&
               key_info->key_length == HA_HASH_KEY_LENGTH_WITH_NULL) ||
              key_info->key_length == HA_HASH_KEY_LENGTH_WITHOUT_NULL);

  if (key_info->key_part->field->is_real_null())
    DBUG_RETURN(0);

  if (skip_self)
    position(table->record[0]);

  key_copy(ptr, new_rec, key_info, key_info->key_length, false);

  result= lookup_handler->ha_index_init(key_no, 0);
  if (result)
    DBUG_RETURN(result);
  auto blob_storage= (String*)alloca(sizeof(String)*table->s->virtual_not_stored_blob_fields);
  table->remember_blob_values(blob_storage);
  store_record(table, file->lookup_buffer);
  result= lookup_handler->ha_index_read_map(table->record[0], ptr,
                                            HA_WHOLE_KEY, HA_READ_KEY_EXACT);
  if (result)
    goto end;

  // restore pointers after swap_values in TABLE::update_virtual_fields()
  for (Field **vf= table->vfield; *vf; vf++)
  {
    if (!(*vf)->stored_in_db() && (*vf)->flags & BLOB_FLAG &&
        bitmap_is_set(table->read_set, (*vf)->field_index))
      ((Field_blob*)*vf)->swap_value_and_read_value();
  }
  do
  {
    if (!long_unique_fields_differ(key_info, lookup_buffer))
    {
      lookup_handler->position(table->record[0]);
      if (skip_self && !memcmp(ref, lookup_handler->ref, ref_length))
      {
        skip_self= false; // cannot happen twice, so let's save a memcpy
        continue;
      }
      result= HA_ERR_FOUND_DUPP_KEY;
      table->file->lookup_errkey= key_no;
      memcpy(table->file->dup_ref, lookup_handler->ref, ref_length);
      goto end;
    }
  }
  while (!(result= lookup_handler->ha_index_next_same(table->record[0], ptr,
                                                      key_info->key_length)));

end:
  if (result == HA_ERR_END_OF_FILE || result == HA_ERR_KEY_NOT_FOUND)
    result= 0;

  restore_record(table, file->lookup_buffer);
  table->restore_blob_values(blob_storage);
  lookup_handler->ha_index_end();
  DBUG_RETURN(result);
}

void handler::alloc_lookup_buffer()
{
  if (!lookup_buffer)
    lookup_buffer= (uchar*)alloc_root(&table->mem_root,
                                      table_share->max_unique_length
                                      + table_share->null_fields
                                      + table_share->reclength);
}


int handler::ha_check_inserver_constraints(const uchar *old_data,
                                           const uchar* new_data)
{
  int error= 0;
  if (is_root_handler())
  {
    uint saved_status= table->status;
    if (!(error= ha_check_overlaps(old_data, new_data)))
      error= ha_check_long_uniques(old_data, new_data);
    table->status= saved_status;
  }
  return error;
}


/** @brief
    check whether inserted records breaks the unique constraint on long columns.
    @returns 0 if no duplicate else returns error
  */
int handler::ha_check_long_uniques(const uchar *old_rec, const uchar *new_rec)
{
  if (!table->s->long_unique_table)
    return 0;
  DBUG_ASSERT(inited == NONE || lookup_handler != this);
  DBUG_ASSERT(new_rec == table->record[0]);
  DBUG_ASSERT(!old_rec || old_rec == table->record[1]);
  lookup_errkey= (uint)-1;
  for (uint i= 0; i < table->s->keys; i++)
  {
    KEY *keyinfo= table->key_info + i;
    if (keyinfo->algorithm == HA_KEY_ALG_LONG_HASH)
    {
      if (!old_rec || long_unique_fields_differ(keyinfo, old_rec))
      {
        if (int res= check_duplicate_long_entry_key(new_rec, i))
        {
          if (!old_rec && table->next_number_field &&
              !(ha_table_flags() & HA_CHECK_UNIQUE_AFTER_WRITE))
            if (int err= update_auto_increment())
              return err;
          return res;
        }
      }
    }
  }
  return 0;
}


int handler::ha_check_overlaps(const uchar *old_data, const uchar* new_data)
{
  DBUG_ASSERT(new_data);
  DBUG_ASSERT(this == table->file);
  if (!table_share->period.unique_keys)
    return 0;
  if (table->versioned())
  {
    Field *end= table->vers_end_field();
    if (!end->is_max(end->ptr_in_record(new_data)))
      return 0;
  }

  const bool after_write= ha_table_flags() & HA_CHECK_UNIQUE_AFTER_WRITE;
  const bool is_update= !after_write && old_data;
  uchar *record_buffer= lookup_buffer + table_share->max_unique_length
                                      + table_share->null_fields;

  // Needed to compare record refs later
  if (is_update)
    position(old_data);

  DBUG_ASSERT(!keyread_enabled());

  int error= 0;
  lookup_errkey= (uint)-1;

  for (uint key_nr= 0; key_nr < table_share->keys && !error; key_nr++)
  {
    const KEY &key_info= table->key_info[key_nr];
    const uint key_parts= key_info.user_defined_key_parts;
    if (!key_info.without_overlaps)
      continue;

    if (is_update)
    {
      bool key_used= false;
      for (uint k= 0; k < key_parts && !key_used; k++)
        key_used= bitmap_is_set(table->write_set,
                                key_info.key_part[k].fieldnr - 1);
      if (!key_used)
        continue;
    }

    error= lookup_handler->ha_index_init(key_nr, 0);
    if (error)
      return error;

    error= lookup_handler->ha_start_keyread(key_nr);
    DBUG_ASSERT(!error);

    const uint period_field_length= key_info.key_part[key_parts - 1].length;
    const uint key_base_length= key_info.key_length - 2 * period_field_length;

    key_copy(lookup_buffer, new_data, &key_info, 0);

    /* Copy period_start to period_end.
       the value in period_start field is not significant, but anyway let's leave
       it defined to avoid uninitialized memory access
     */
    memcpy(lookup_buffer + key_base_length,
           lookup_buffer + key_base_length + period_field_length,
           period_field_length);

    /* Find row with period_end > (period_start of new_data) */
    error = lookup_handler->ha_index_read_map(record_buffer, lookup_buffer,
                                       key_part_map((1 << (key_parts - 1)) - 1),
                                       HA_READ_AFTER_KEY);

    if (!error)
    {
      if (is_update)
      {
        /* In case of update it could happen that the nearest neighbour is
           a record we are updating. It means, that there are no overlaps
           from this side.
        */
        DBUG_ASSERT(lookup_handler != this);
        DBUG_ASSERT(ref_length == lookup_handler->ref_length);

        lookup_handler->position(record_buffer);
        if (memcmp(ref, lookup_handler->ref, ref_length) == 0)
          error= lookup_handler->ha_index_next(record_buffer);
      }
      else if (after_write)
        error= lookup_handler->ha_index_next(record_buffer);
    }

    if (!error && table->check_period_overlaps(key_info, new_data, record_buffer))
      error= HA_ERR_FOUND_DUPP_KEY;

    if (error == HA_ERR_KEY_NOT_FOUND || error == HA_ERR_END_OF_FILE)
      error= 0;

    if (error == HA_ERR_FOUND_DUPP_KEY)
      lookup_errkey= key_nr;

    int end_error= lookup_handler->ha_end_keyread();
    DBUG_ASSERT(!end_error);

    end_error= lookup_handler->ha_index_end();
    if (!error && end_error)
      error= end_error;
  }

  return error;
}


/**
  Check if galera disables binary logging for this table

  @return 0  Binary logging disabled
  @return 1  Binary logging can be enabled
*/


static inline bool wsrep_check_if_binlog_row(TABLE *table)
{
#ifdef WITH_WSREP
  THD *const thd= table->in_use;

  /* only InnoDB tables will be replicated through binlog emulation */
  if ((WSREP_EMULATE_BINLOG(thd) &&
       !(table->file->partition_ht()->flags & HTON_WSREP_REPLICATION)) ||
      thd->wsrep_ignore_table == true)
    return 0;
#endif
  return 1;
}


/**
   Prepare handler for row logging

   @return 0 if handler will not participate in row logging
   @return 1 handler will participate in row logging

   This function is always safe to call on an opened table.
*/

bool handler::prepare_for_row_logging()
{
  DBUG_ENTER("handler::prepare_for_row_logging");

  /* Check if we should have row logging */
  if (wsrep_check_if_binlog_row(table) &&
      check_table_binlog_row_based())
  {
    /*
      Row logging enabled. Intialize all variables and write
      annotated and table maps
    */
    row_logging= row_logging_init= 1;

    /*
      We need to have a transactional behavior for SQLCOM_CREATE_TABLE
      (e.g. CREATE TABLE... SELECT * FROM TABLE) in order to keep a
      compatible behavior with the STMT based replication even when
      the table is not transactional. In other words, if the operation
      fails while executing the insert phase nothing is written to the
      binlog.
    */
    row_logging_has_trans=
      ((sql_command_flags[table->in_use->lex->sql_command] &
        (CF_SCHEMA_CHANGE | CF_ADMIN_COMMAND)) ||
       table->file->has_transactions_and_rollback());
  }
  else
  {
    /* Check row_logging has not been properly cleared from previous command */
    DBUG_ASSERT(row_logging == 0);
  }
  DBUG_RETURN(row_logging);
}


/*
  Do all initialization needed for writes: INSERT/UPDATE/DELETE

  can_set_fields is true if values of individual fields in a record
  can be set. That is, INSERT/UPDATE, need uniqueness checks, for example.

  can_lookup is true if the operation needs to look up rows in the table,
  that is UPDATE/DELETE, and here we need a separate `lookup_handler`
  to avoid disrupting the state of `this`
*/

int handler::prepare_for_modify(bool can_set_fields, bool can_lookup)
{
  if (table->open_hlindexes_for_write())
    return 1;

  if (can_set_fields)
  {
    /* Preparation for unique of blob's */
    if (table->s->long_unique_table || table->s->period.unique_keys)
    {
      if (can_lookup && create_lookup_handler())
        return 1;
      alloc_lookup_buffer();
    }
  }
  return 0;
}


int handler::ha_write_row(const uchar *buf)
{
  int error;
  DBUG_ENTER("handler::ha_write_row");
  DEBUG_SYNC_C("ha_write_row_start");
  DBUG_EXECUTE_IF("wsrep_ha_write_row",
                  {
                    const char act[]=
                      "now "
                      "SIGNAL wsrep_ha_write_row_reached "
                      "WAIT_FOR wsrep_ha_write_row_continue";
                    DBUG_ASSERT(!debug_sync_set_action(ha_thd(), STRING_WITH_LEN(act)));
                  });
  DBUG_ASSERT(table_share->tmp_table || m_lock_type == F_WRLCK);
  DBUG_ASSERT(buf == table->record[0]);

  if (!(ha_table_flags() & HA_CHECK_UNIQUE_AFTER_WRITE) &&
      (error= ha_check_inserver_constraints(NULL, buf)))
    goto err;

  MYSQL_INSERT_ROW_START(table_share->db.str, table_share->table_name.str);
  mark_trx_read_write();
  increment_statistics(&SSV::ha_write_count);

  TABLE_IO_WAIT(tracker, PSI_TABLE_WRITE_ROW, MAX_KEY, error,
                      { error= write_row(buf); })
  DBUG_PRINT("dml", ("INSERT: %s = %d",
                     dbug_format_row(table, buf, false).c_ptr_safe(), error));

  MYSQL_INSERT_ROW_DONE(error);
  if (error)
    goto err;

  if ((ha_table_flags() & HA_CHECK_UNIQUE_AFTER_WRITE) &&
      (error= ha_check_inserver_constraints(NULL, buf)))
  {
    if (lookup_handler != this) // INSERT IGNORE or REPLACE or ODKU
    {
      int olderror= error;
      if ((error= lookup_handler->rnd_init(0)))
        goto err;
      position(buf);
      if ((error= lookup_handler->rnd_pos(lookup_buffer, ref)))
        goto err;

      increment_statistics(&SSV::ha_delete_count);
      TABLE_IO_WAIT(tracker, PSI_TABLE_DELETE_ROW, MAX_KEY, error,
                    { error= lookup_handler->delete_row(buf);})
      lookup_handler->rnd_end();
      if (!error)
        error= olderror;
    }
    goto err;
  }

  if ((error= table->hlindexes_on_insert()))
    goto err;

  rows_stats.inserted++;
  error= binlog_log_row(0, buf,
                        Write_rows_log_event::binlog_row_logging_function);

  if (WSREP_NNULL(ha_thd()) && table_share->tmp_table == NO_TMP_TABLE &&
      ht->flags & HTON_WSREP_REPLICATION && !error)
    error= wsrep_after_row(ha_thd());

err:
  DEBUG_SYNC_C("ha_write_row_end");
  DBUG_RETURN(error);
}


int handler::ha_update_row(const uchar *old_data, const uchar *new_data)
{
  int error;
  DBUG_ASSERT(table_share->tmp_table || m_lock_type == F_WRLCK);
  /*
    Some storage engines require that the new record is in record[0]
    (and the old record is in record[1]).
  */
  DBUG_ASSERT(new_data == table->record[0]);
  DBUG_ASSERT(old_data == table->record[1]);

  if (!(ha_table_flags() & HA_CHECK_UNIQUE_AFTER_WRITE) &&
      (error= ha_check_inserver_constraints(old_data, new_data)))
    return error;

  MYSQL_UPDATE_ROW_START(table_share->db.str, table_share->table_name.str);
  mark_trx_read_write();
  increment_statistics(&SSV::ha_update_count);

  TABLE_IO_WAIT(tracker, PSI_TABLE_UPDATE_ROW, active_index, 0,
                      { error= update_row(old_data, new_data);})
  DBUG_PRINT("dml", ("UPDATE: %s => %s = %d",
                     dbug_format_row(table, old_data, false).c_ptr_safe(),
                     dbug_format_row(table, new_data, false).c_ptr_safe(),
                     error));

  MYSQL_UPDATE_ROW_DONE(error);
  if (error)
    return error;

  if ((ha_table_flags() & HA_CHECK_UNIQUE_AFTER_WRITE) &&
      (error= ha_check_inserver_constraints(old_data, new_data)))
  {
    int e= 0;
    if (ha_thd()->lex->ignore)
    {
      my_printf_error(ER_NOT_SUPPORTED_YET, "UPDATE IGNORE in READ "
        "COMMITTED isolation mode of a table with a UNIQUE constraint "
        "%s is not currently supported", MYF(0),
        table->s->long_unique_table ? "USING HASH" : "WITHOUT OVERLAPS");
      return HA_ERR_UNSUPPORTED;
    }
    return e ? e : error;
  }

  if ((error= table->hlindexes_on_update()))
    return error;

  rows_stats.updated++;
  error= binlog_log_row(old_data, new_data,
                        Update_rows_log_event::binlog_row_logging_function);

#ifdef WITH_WSREP
  THD *thd= ha_thd();
  if (WSREP_NNULL(thd))
  {
    /* for streaming replication, the following wsrep_after_row()
    may replicate a fragment, so we have to declare potential PA
    unsafe before that */
    if (table->s->primary_key == MAX_KEY && wsrep_thd_is_local(thd))
    {
      WSREP_DEBUG("marking trx as PA unsafe pk %d", table->s->primary_key);
      if (thd->wsrep_cs().mark_transaction_pa_unsafe())
        WSREP_DEBUG("session does not have active transaction,"
                    " can not mark as PA unsafe");
    }

    if (!error && table_share->tmp_table == NO_TMP_TABLE &&
        ht->flags & HTON_WSREP_REPLICATION)
      error= wsrep_after_row(thd);
  }
#endif /* WITH_WSREP */
  return error;
}

/*
  Update first row. Only used by sequence tables
*/

int handler::update_first_row(const uchar *new_data)
{
  int error;
  if (likely(!(error= ha_rnd_init(1))))
  {
    int end_error;
    if (likely(!(error= ha_rnd_next(table->record[1]))))
    {
      /*
        We have to do the memcmp as otherwise we may get error 169 from InnoDB
      */
      if (memcmp(new_data, table->record[1], table->s->reclength))
        error= update_row(table->record[1], new_data);
    }
    end_error= ha_rnd_end();
    if (likely(!error))
      error= end_error;
    /* Logging would be wrong if update_row works but ha_rnd_end fails */
    DBUG_ASSERT(!end_error || error != 0);
  }
  return error;
}


int handler::ha_delete_row(const uchar *buf)
{
  int error;
  DBUG_ASSERT(table_share->tmp_table || m_lock_type == F_WRLCK);
  /*
    Normally table->record[0] is used, but sometimes table->record[1] is used.
    (notably, for REPLACE and in sql_acl.cc)
  */
  DBUG_ASSERT(buf == table->record[0] || buf == table->record[1]);

  MYSQL_DELETE_ROW_START(table_share->db.str, table_share->table_name.str);
  mark_trx_read_write();
  increment_statistics(&SSV::ha_delete_count);

  TABLE_IO_WAIT(tracker, PSI_TABLE_DELETE_ROW, active_index, error,
    { error= delete_row(buf);})
  DBUG_PRINT("dml", ("DELETE: %s = %d",
                     dbug_format_row(table, buf, false).c_ptr_safe(), error));
  MYSQL_DELETE_ROW_DONE(error);
  if (likely(!error) && !(error= table->hlindexes_on_delete(buf)))
  {
    rows_stats.deleted++;
    error= binlog_log_row(buf, 0,
                          Delete_rows_log_event::binlog_row_logging_function);

#ifdef WITH_WSREP
    THD *thd= ha_thd();
    /* For streaming replication, when removing fragments, don't call
    wsrep_after_row() as that would initiate new streaming transaction */
    if (WSREP_NNULL(thd) && !thd->wsrep_ignore_table)
    {
      /* for streaming replication, the following wsrep_after_row()
      may replicate a fragment, so we have to declare potential PA
      unsafe before that */
      if (table->s->primary_key == MAX_KEY && wsrep_thd_is_local(thd))
      {
        WSREP_DEBUG("marking trx as PA unsafe pk %d", table->s->primary_key);
        if (thd->wsrep_cs().mark_transaction_pa_unsafe())
          WSREP_DEBUG("session does not have active transaction,"
                      " can not mark as PA unsafe");
      }

      if (!error && table_share->tmp_table == NO_TMP_TABLE &&
          ht->flags & HTON_WSREP_REPLICATION)
        error= wsrep_after_row(thd);
    }
#endif /* WITH_WSREP */
  }
  return error;
}


/**
  Execute a direct update request.  A direct update request updates all
  qualified rows in a single operation, rather than one row at a time.
  In a Spider cluster the direct update operation is pushed down to the
  child levels of the cluster.

  Note that this can't be used in case of statment logging

  @param  update_rows   Number of updated rows.

  @retval 0             Success.
  @retval != 0          Failure.
*/

int handler::ha_direct_update_rows(ha_rows *update_rows, ha_rows *found_rows)
{
  int error;
  MYSQL_UPDATE_ROW_START(table_share->db.str, table_share->table_name.str);
  mark_trx_read_write();

  error= direct_update_rows(update_rows, found_rows);
  MYSQL_UPDATE_ROW_DONE(error);
  return error;
}


/**
  Execute a direct delete request.  A direct delete request deletes all
  qualified rows in a single operation, rather than one row at a time.
  In a Spider cluster the direct delete operation is pushed down to the
  child levels of the cluster.

  @param  delete_rows   Number of deleted rows.

  @retval 0             Success.
  @retval != 0          Failure.
*/

int handler::ha_direct_delete_rows(ha_rows *delete_rows)
{
  int error;
  /* Ensure we are not using binlog row */
  DBUG_ASSERT(!table->in_use->is_current_stmt_binlog_format_row());

  MYSQL_DELETE_ROW_START(table_share->db.str, table_share->table_name.str);
  mark_trx_read_write();

  error = direct_delete_rows(delete_rows);
  MYSQL_DELETE_ROW_DONE(error);
  return error;
}


/** @brief
  use_hidden_primary_key() is called in case of an update/delete when
  (table_flags() and HA_PRIMARY_KEY_REQUIRED_FOR_DELETE) is defined
  but we don't have a primary key
*/
void handler::use_hidden_primary_key()
{
  /* fallback to use all columns in the table to identify row */
  table->column_bitmaps_set(&table->s->all_set, table->write_set);
}


/**
  Get an initialized ha_share.

  @return Initialized ha_share
    @retval NULL    ha_share is not yet initialized.
    @retval != NULL previous initialized ha_share.

  @note
  If not a temp table, then LOCK_ha_data must be held.
*/

Handler_share *handler::get_ha_share_ptr()
{
  DBUG_ENTER("handler::get_ha_share_ptr");
  DBUG_ASSERT(ha_share);
  DBUG_ASSERT(table_share);

#ifndef DBUG_OFF
  if (table_share->tmp_table == NO_TMP_TABLE)
    mysql_mutex_assert_owner(&table_share->LOCK_ha_data);
#endif

  DBUG_RETURN(*ha_share);
}


/**
  Set ha_share to be used by all instances of the same table/partition.

  @param ha_share    Handler_share to be shared.

  @note
  If not a temp table, then LOCK_ha_data must be held.
*/

void handler::set_ha_share_ptr(Handler_share *arg_ha_share)
{
  DBUG_ENTER("handler::set_ha_share_ptr");
  DBUG_ASSERT(ha_share);
#ifndef DBUG_OFF
  if (table_share->tmp_table == NO_TMP_TABLE)
    mysql_mutex_assert_owner(&table_share->LOCK_ha_data);
#endif

  *ha_share= arg_ha_share;
  DBUG_VOID_RETURN;
}


/**
  Take a lock for protecting shared handler data.
*/

void handler::lock_shared_ha_data()
{
  DBUG_ASSERT(table_share);
  if (table_share->tmp_table == NO_TMP_TABLE)
    mysql_mutex_lock(&table_share->LOCK_ha_data);
}


/**
  Release lock for protecting ha_share.
*/

void handler::unlock_shared_ha_data()
{
  DBUG_ASSERT(table_share);
  if (table_share->tmp_table == NO_TMP_TABLE)
    mysql_mutex_unlock(&table_share->LOCK_ha_data);
}

void handler::set_lock_type(enum thr_lock_type lock)
{
  table->reginfo.lock_type= lock;
}

Compare_keys handler::compare_key_parts(const Field &old_field,
                                        const Column_definition &new_field,
                                        const KEY_PART_INFO &old_part,
                                        const KEY_PART_INFO &new_part) const
{
  if (!old_field.is_equal(new_field))
    return Compare_keys::NotEqual;

  if (old_part.length != new_part.length)
    return Compare_keys::NotEqual;

  return Compare_keys::Equal;
}

#ifdef WITH_WSREP
/**
  @details
  This function makes the storage engine to force the victim transaction
  to abort. Currently, only innodb has this functionality, but any SE
  implementing the wsrep API should provide this service to support
  multi-master operation.

  @note Aborting the transaction does NOT end it, it still has to
  be rolled back with hton->rollback().

  @note It is safe to abort from one thread (bf_thd) the transaction,
  running in another thread (victim_thd), because InnoDB's lock_sys and
  trx_mutex guarantee the necessary protection. However, its not safe
  to access victim_thd->transaction, because it's not protected from
  concurrent accesses. And it's an overkill to take LOCK_plugin and
  iterate the whole installed_htons[] array every time.

  @note Object victim_thd is not guaranteed to exist after this
        function returns.

  @param bf_thd       brute force THD asking for the abort
  @param victim_thd   victim THD to be aborted

  @return
    always 0
*/

int ha_abort_transaction(THD *bf_thd, THD *victim_thd, my_bool signal)
{
  DBUG_ENTER("ha_abort_transaction");
  if (!WSREP(bf_thd) &&
      !(bf_thd->variables.wsrep_OSU_method == WSREP_OSU_RSU &&
        wsrep_thd_is_toi(bf_thd))) {
    DBUG_RETURN(0);
  }

  handlerton *hton= installed_htons[DB_TYPE_INNODB];
  if (hton && hton->abort_transaction)
  {
    hton->abort_transaction(hton, bf_thd, victim_thd, signal);
  }
  else
  {
    WSREP_WARN("Cannot abort InnoDB transaction");
  }

  DBUG_RETURN(0);
}
#endif /* WITH_WSREP */


/* Remove all indexes for a given table from global index statistics */

static
int del_global_index_stats_for_table(THD *thd, uchar* cache_key, size_t cache_key_length)
{
  int res = 0;
  uint to_delete_counter= 0;
  INDEX_STATS *index_stats_to_delete[MAX_INDEXES];
  DBUG_ENTER("del_global_index_stats_for_table");

  mysql_mutex_lock(&LOCK_global_index_stats);

  for (uint i= 0; i < global_index_stats.records; i++)
  {
    INDEX_STATS *index_stats =
      (INDEX_STATS*) my_hash_element(&global_index_stats, i);

    /* We search correct db\0table_name\0 string */
    if (index_stats &&
	index_stats->index_name_length >= cache_key_length &&
	!memcmp(index_stats->index, cache_key, cache_key_length))
    {
      index_stats_to_delete[to_delete_counter++]= index_stats;
    }
  }

  for (uint i= 0; i < to_delete_counter; i++)
    res= my_hash_delete(&global_index_stats, (uchar*)index_stats_to_delete[i]);

  mysql_mutex_unlock(&LOCK_global_index_stats);
  DBUG_RETURN(res);
}

/* Remove a table from global table statistics */

int del_global_table_stat(THD *thd, const LEX_CSTRING *db, const LEX_CSTRING *table)
{
  TABLE_STATS *table_stats;
  int res = 0;
  uchar *cache_key;
  size_t cache_key_length;
  DBUG_ENTER("del_global_table_stat");

  cache_key_length= db->length + 1 + table->length + 1;

  if(!(cache_key= (uchar *)my_malloc(PSI_INSTRUMENT_ME, cache_key_length,
                                     MYF(MY_WME | MY_ZEROFILL))))
  {
    /* Out of memory error already given */
    res = 1;
    goto end;
  }

  memcpy(cache_key, db->str, db->length);
  memcpy(cache_key + db->length + 1, table->str, table->length);

  res= del_global_index_stats_for_table(thd, cache_key, cache_key_length);

  mysql_mutex_lock(&LOCK_global_table_stats);

  if((table_stats= (TABLE_STATS*) my_hash_search(&global_table_stats,
                                                cache_key,
                                                cache_key_length)))
    res= my_hash_delete(&global_table_stats, (uchar*)table_stats);

  my_free(cache_key);
  mysql_mutex_unlock(&LOCK_global_table_stats);

end:
  DBUG_RETURN(res);
}

/* Remove a index from global index statistics */

int del_global_index_stat(THD *thd, TABLE* table, KEY* key_info)
{
  INDEX_STATS *index_stats;
  size_t key_length= table->s->table_cache_key.length + key_info->name.length + 1;
  int res = 0;
  DBUG_ENTER("del_global_index_stat");
  mysql_mutex_lock(&LOCK_global_index_stats);

  if((index_stats= (INDEX_STATS*) my_hash_search(&global_index_stats,
                                                key_info->cache_name,
                                                key_length)))
    res= my_hash_delete(&global_index_stats, (uchar*)index_stats);

  mysql_mutex_unlock(&LOCK_global_index_stats);
  DBUG_RETURN(res);
}

/*****************************************************************************
  VERSIONING functions
******************************************************************************/

bool Vers_parse_info::is_start(const Create_field &f) const
{
  return f.flags & VERS_ROW_START;
}
bool Vers_parse_info::is_end(const Create_field &f) const
{
  return f.flags & VERS_ROW_END;
}

static Create_field *vers_init_sys_field(THD *thd,
                                         const Lex_ident_column &field_name,
                                         int flags, bool integer)
{
  Create_field *f= new (thd->mem_root) Create_field();
  if (!f)
    return NULL;

  f->field_name= field_name;
  f->charset= system_charset_info;
  f->flags= flags | NO_DEFAULT_VALUE_FLAG | NOT_NULL_FLAG;
  if (integer)
  {
    f->set_handler(&type_handler_vers_trx_id);
    f->length= MY_INT64_NUM_DECIMAL_DIGITS - 1;
    f->flags|= UNSIGNED_FLAG;
  }
  else
  {
    f->set_handler(&type_handler_timestamp2);
    f->length= MAX_DATETIME_PRECISION;
  }
  f->invisible= DBUG_IF("sysvers_show") ? VISIBLE : INVISIBLE_SYSTEM;

  if (f->check(thd))
    return NULL;

  return f;
}

bool Vers_parse_info::create_sys_field(THD *thd,
                                       const Lex_ident_column &field_name,
                                       Alter_info *alter_info, int flags)
{
  DBUG_ASSERT(can_native >= 0); /* Requires vers_check_native() called */
  Create_field *f= vers_init_sys_field(thd, field_name, flags,
                                       DBUG_IF("sysvers_force_trx") &&
                                       can_native);
  if (!f)
    return true;

  alter_info->flags|= ALTER_PARSER_ADD_COLUMN;
  alter_info->create_list.push_back(f);

  return false;
}

const Lex_ident_column
  Vers_parse_info::default_start= "row_start"_Lex_ident_column,
  Vers_parse_info::default_end= "row_end"_Lex_ident_column;

bool Vers_parse_info::fix_implicit(THD *thd, Alter_info *alter_info)
{
  // If user specified some of these he must specify the others too. Do nothing.
  if (*this)
    return false;

  alter_info->flags|= ALTER_PARSER_ADD_COLUMN;

  period= start_end_t(default_start, default_end);
  as_row= period;

  if (create_sys_field(thd, default_start, alter_info, VERS_ROW_START) ||
      create_sys_field(thd, default_end, alter_info, VERS_ROW_END))
  {
    return true;
  }
  return false;
}


void Table_scope_and_contents_source_st::vers_check_native()
{
  vers_info.can_native= (db_type->db_type == DB_TYPE_PARTITION_DB ||
                         ha_check_storage_engine_flag(db_type,
                                                      HTON_NATIVE_SYS_VERSIONING));
}


bool Table_scope_and_contents_source_st::vers_fix_system_fields(
  THD *thd, Alter_info *alter_info, const TABLE_LIST &create_table)
{
  DBUG_ASSERT(!(alter_info->flags & ALTER_DROP_SYSTEM_VERSIONING));

  if (DBUG_IF("sysvers_force") || DBUG_IF("sysvers_force_trx"))
  {
    alter_info->flags|= ALTER_ADD_SYSTEM_VERSIONING;
    options|= HA_VERSIONED_TABLE;
  }

  if (!vers_info.need_check(alter_info))
    return false;

  const bool add_versioning= alter_info->flags & ALTER_ADD_SYSTEM_VERSIONING;

  if (!vers_info.versioned_fields && vers_info.unversioned_fields && !add_versioning)
  {
    // All is correct but this table is not versioned.
    options&= ~HA_VERSIONED_TABLE;
    return false;
  }

  if (!add_versioning && vers_info && !vers_info.versioned_fields)
  {
    my_error(ER_MISSING, MYF(0), create_table.table_name.str,
             "WITH SYSTEM VERSIONING");
    return true;
  }

  List_iterator<Create_field> it(alter_info->create_list);
  while (Create_field *f= it++)
  {
    if (f->vers_sys_field())
      continue;
    if ((f->versioning == Column_definition::VERSIONING_NOT_SET && !add_versioning) ||
        f->versioning == Column_definition::WITHOUT_VERSIONING)
    {
      f->flags|= VERS_UPDATE_UNVERSIONED_FLAG;
    }
  } // while

  vers_check_native();

  if (vers_info.fix_implicit(thd, alter_info))
    return true;

  return false;
}


int get_select_field_pos(Alter_info *alter_info, int select_field_count,
                         bool versioned)
{
  int select_field_pos= alter_info->create_list.elements - select_field_count;
  if (select_field_count && versioned &&
      /*
        ALTER_PARSER_ADD_COLUMN indicates system fields was created implicitly,
        select_field_count guarantees it's not ALTER TABLE
      */
      alter_info->flags & ALTER_PARSER_ADD_COLUMN)
    select_field_pos-= 2;
  return select_field_pos;
}


bool Table_scope_and_contents_source_st::vers_check_system_fields(
        THD *thd, Alter_info *alter_info, const Lex_ident_table &table_name,
        const Lex_ident_db &db, int select_count)
{
  if (!(options & HA_VERSIONED_TABLE))
    return false;

  uint versioned_fields= 0;

  if (!(alter_info->flags & ALTER_DROP_SYSTEM_VERSIONING))
  {
    uint fieldnr= 0;
    List_iterator<Create_field> field_it(alter_info->create_list);
    uint select_field_pos= (uint) get_select_field_pos(alter_info, select_count,
                                                       true);
    while (Create_field *f= field_it++)
    {
      /*
         The field from the CREATE part can be duplicated in the SELECT part of
         CREATE...SELECT. In that case double counts should be avoided.
         select_create::create_table_from_items just pushes the fields back into
         the create_list, without additional manipulations, so the fields from
         SELECT go last there.
       */
      bool is_dup= false;
      if (fieldnr >= select_field_pos && f->invisible < INVISIBLE_SYSTEM)
      {
        List_iterator<Create_field> dup_it(alter_info->create_list);
        for (Create_field *dup= dup_it++; !is_dup && dup != f; dup= dup_it++)
          is_dup= dup->field_name.streq(f->field_name);
      }

      if (!(f->flags & VERS_UPDATE_UNVERSIONED_FLAG) && !is_dup)
        versioned_fields++;
      fieldnr++;
    }
    if (versioned_fields == VERSIONING_FIELDS)
    {
      my_error(ER_VERS_TABLE_MUST_HAVE_COLUMNS, MYF(0), table_name.str);
      return true;
    }
  }

  if (!(alter_info->flags & ALTER_ADD_SYSTEM_VERSIONING) && !versioned_fields)
    return false;

  return vers_info.check_sys_fields(table_name, db, alter_info);
}


bool Vers_parse_info::fix_alter_info(THD *thd, Alter_info *alter_info,
                                     HA_CREATE_INFO *create_info, TABLE *table)
{
  TABLE_SHARE *share= table->s;
  const Lex_ident_table &table_name= share->table_name;

  if (!need_check(alter_info) && !share->versioned)
    return false;

  if (share->tmp_table &&
      !DBUG_IF("sysvers_force") && !DBUG_IF("sysvers_force_trx"))
  {
    my_error(ER_VERS_NOT_SUPPORTED, MYF(0), "CREATE TEMPORARY TABLE");
    return true;
  }

  if (alter_info->flags & ALTER_ADD_SYSTEM_VERSIONING &&
      table->versioned())
  {
    my_error(ER_VERS_ALREADY_VERSIONED, MYF(0), table_name.str);
    return true;
  }

  if (alter_info->flags & ALTER_DROP_SYSTEM_VERSIONING)
  {
    if (!share->versioned)
    {
      my_error(ER_VERS_NOT_VERSIONED, MYF(0), table_name.str);
      return true;
    }
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (table->part_info &&
        table->part_info->part_type == VERSIONING_PARTITION)
    {
      my_error(ER_DROP_VERSIONING_SYSTEM_TIME_PARTITION, MYF(0), table_name.str);
      return true;
    }
#endif

    return false;
  }

  if (!(alter_info->flags & ALTER_ADD_SYSTEM_VERSIONING))
  {
    List_iterator<Create_field> it(alter_info->create_list);
    while (Create_field *f= it++)
    {
      if (f->flags & VERS_SYSTEM_FIELD)
      {
        if (!table->versioned())
        {
          my_error(ER_VERS_NOT_VERSIONED, MYF(0), table->s->table_name.str);
          return true;
        }
        if (!table->vers_implicit())
        {
          my_error(ER_VERS_DUPLICATE_ROW_START_END, MYF(0),
                  f->flags & VERS_ROW_START ? "START" : "END", f->field_name.str);

          return true;
        }
        Field *old= f->flags & VERS_ROW_START ? table->vers_start_field() : table->vers_end_field();
        if (old->type_handler() == f->type_handler() &&
            old->field_length == f->length &&
            (old->flags & UNSIGNED_FLAG) == (f->flags & UNSIGNED_FLAG))
        {
          alter_info->flags|= ALTER_VERS_EXPLICIT;
          alter_info->add_alter_list(thd, old->field_name, f->field_name, false);
          it.remove();
        }
        else
        {
          my_error(ER_WRONG_FIELD_SPEC, MYF(0), f->field_name.str);
          return true;
        }
      }
    }
  }

  if ((alter_info->flags & ALTER_DROP_PERIOD ||
       versioned_fields || unversioned_fields) && !share->versioned)
  {
    my_error(ER_VERS_NOT_VERSIONED, MYF(0), table_name.str);
    return true;
  }

  if (share->versioned)
  {
    if (!table->vers_implicit() &&
        (alter_info->flags & ALTER_ADD_PERIOD))
    {
      my_error(ER_VERS_ALREADY_VERSIONED, MYF(0), table_name.str);
      return true;
    }

    // copy info from existing table
    create_info->options|= HA_VERSIONED_TABLE;

    DBUG_ASSERT(share->vers_start_field());
    DBUG_ASSERT(share->vers_end_field());

    if (!(alter_info->flags & ALTER_VERS_EXPLICIT))
    {
      Lex_ident_column start(share->vers_start_field()->field_name);
      Lex_ident_column end(share->vers_end_field()->field_name);
      DBUG_ASSERT(start.str);
      DBUG_ASSERT(end.str);

      as_row= start_end_t(start, end);
      period= as_row;

      if (alter_info->create_list.elements)
      {
        List_iterator_fast<Create_field> it(alter_info->create_list);
        while (Create_field *f= it++)
        {
          if (f->versioning == Column_definition::WITHOUT_VERSIONING)
            f->flags|= VERS_UPDATE_UNVERSIONED_FLAG;

          if (f->change.str && (start.streq(f->change) || end.streq(f->change)))
          {
            my_error(ER_VERS_ALTER_SYSTEM_FIELD, MYF(0), f->change.str);
            return true;
          }
        }
      }
    } /* if (!convert_explicit) */
    return check_conditions(table_name, share->db);
  } /* if (share->versioned) */

  if ((alter_info->flags & ALTER_ADD_SYSTEM_VERSIONING) &&
      (fix_implicit(thd, alter_info) ||
        check_sys_fields(table_name, share->db, alter_info)))
    return true;

  return false;
}

bool
Vers_parse_info::fix_create_like(Alter_info &alter_info, HA_CREATE_INFO &create_info,
                                 TABLE_LIST &src_table, TABLE_LIST &table)
{
  List_iterator<Create_field> it(alter_info.create_list);
  List_iterator<Key> key_it(alter_info.key_list);
  List_iterator<Key_part_spec> kp_it;
  Create_field *f, *f_start=NULL, *f_end= NULL;

  DBUG_ASSERT(alter_info.create_list.elements > 2);

  if (create_info.tmp_table())
  {
    int remove= 2;
    while (remove && (f= it++))
    {
      if (f->flags & VERS_SYSTEM_FIELD)
      {
        it.remove();
        remove--;
      }
      key_it.rewind();
      while (Key *key= key_it++)
      {
        kp_it.init(key->columns);
        while (Key_part_spec *kp= kp_it++)
        {
          if (kp->field_name.streq(f->field_name))
          {
            kp_it.remove();
          }
        }
        if (0 == key->columns.elements)
        {
          key_it.remove();
        }
      }
    }
    DBUG_ASSERT(remove == 0);
    push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_UNKNOWN_ERROR,
                        "System versioning is stripped from temporary `%s.%s`",
                        table.db.str, table.table_name.str);
    return false;
  }

  while ((f= it++))
  {
    if (f->flags & VERS_ROW_START)
    {
      f_start= f;
      if (f_end)
        break;
    }
    else if (f->flags & VERS_ROW_END)
    {
      f_end= f;
      if (f_start)
        break;
    }
  }

  if (!f_start || !f_end)
  {
    my_error(ER_MISSING, MYF(0), src_table.table_name.str,
             f_start ? "AS ROW END" : "AS ROW START");
    return true;
  }

  as_row= start_end_t(f_start->field_name, f_end->field_name);
  period= as_row;

  create_info.options|= HA_VERSIONED_TABLE;
  return false;
}

bool Vers_parse_info::need_check(const Alter_info *alter_info) const
{
  return versioned_fields || unversioned_fields ||
         alter_info->flags & ALTER_ADD_PERIOD ||
         alter_info->flags & ALTER_DROP_PERIOD ||
         alter_info->flags & ALTER_ADD_SYSTEM_VERSIONING ||
         alter_info->flags & ALTER_DROP_SYSTEM_VERSIONING || *this;
}

bool Vers_parse_info::check_conditions(const Lex_ident_table &table_name,
                                       const Lex_ident_db &db) const
{
  if (!as_row.start || !as_row.end)
  {
    my_error(ER_MISSING, MYF(0), table_name.str,
                as_row.start ? "AS ROW END" : "AS ROW START");
    return true;
  }

  if (!period.start || !period.end)
  {
    my_error(ER_MISSING, MYF(0), table_name.str, "PERIOD FOR SYSTEM_TIME");
    return true;
  }

  if (!as_row.start.streq(period.start) ||
      !as_row.end.streq(period.end))
  {
    my_error(ER_VERS_PERIOD_COLUMNS, MYF(0), as_row.start.str, as_row.end.str);
    return true;
  }

  if (db.streq(MYSQL_SCHEMA_NAME))
  {
    my_error(ER_VERS_DB_NOT_SUPPORTED, MYF(0), MYSQL_SCHEMA_NAME.str);
    return true;
  }
  return false;
}

static bool is_versioning_timestamp(const Column_definition *f)
{
  return f->type_handler() == &type_handler_timestamp2 &&
         f->length == MAX_DATETIME_FULL_WIDTH;
}

static bool is_some_bigint(const Column_definition *f)
{
  return f->type_handler() == &type_handler_slonglong ||
         f->type_handler() == &type_handler_ulonglong ||
         f->type_handler() == &type_handler_vers_trx_id;
}

static bool is_versioning_bigint(const Column_definition *f)
{
  return is_some_bigint(f) && f->flags & UNSIGNED_FLAG &&
         f->length == MY_INT64_NUM_DECIMAL_DIGITS - 1;
}

static void require_timestamp_error(const char *field, const char *table)
{
  my_error(ER_VERS_FIELD_WRONG_TYPE, MYF(0), field, "TIMESTAMP(6)", table);
}

static void require_trx_id_error(const char *field, const char *table)
{
  my_error(ER_VERS_FIELD_WRONG_TYPE, MYF(0), field, "BIGINT(20) UNSIGNED",
           table);
}


bool Vers_type_timestamp::check_sys_fields(const LEX_CSTRING &table_name,
                                           const Column_definition *row_start,
                                           const Column_definition *row_end) const
{
  if (!is_versioning_timestamp(row_start))
  {
    require_timestamp_error(row_start->field_name.str, table_name.str);
    return true;
  }

  if (row_end->type_handler()->vers() != this ||
      !is_versioning_timestamp(row_end))
  {
    require_timestamp_error(row_end->field_name.str, table_name.str);
    return true;
  }

  return false;
}


bool Vers_type_trx::check_sys_fields(const LEX_CSTRING &table_name,
                                     const Column_definition *row_start,
                                     const Column_definition *row_end) const
{
  if (!is_versioning_bigint(row_start))
  {
    require_trx_id_error(row_start->field_name.str, table_name.str);
    return true;
  }

  if (row_end->type_handler()->vers() != this ||
      !is_versioning_bigint(row_end))
  {
    require_trx_id_error(row_end->field_name.str, table_name.str);
    return true;
  }

  if (!is_some_bigint(row_start))
  {
    require_timestamp_error(row_start->field_name.str, table_name.str);
    return true;
  }

  if (!TR_table::use_transaction_registry)
  {
    my_error(ER_VERS_TRT_IS_DISABLED, MYF(0));
    return true;
  }

  return false;
}


bool Vers_parse_info::check_sys_fields(const Lex_ident_table &table_name,
                                       const Lex_ident_db &db,
                                       Alter_info *alter_info) const
{
  if (check_conditions(table_name, db))
    return true;

  List_iterator<Create_field> it(alter_info->create_list);
  const Create_field *row_start= nullptr;
  const Create_field *row_end= nullptr;
  while (const Create_field *f= it++)
  {
    if (f->flags & VERS_ROW_START && !row_start)
      row_start= f;
    if (f->flags & VERS_ROW_END && !row_end)
      row_end= f;
  }

  if (!row_start || !row_end)
  {
    my_error(ER_VERS_PERIOD_COLUMNS, MYF(0), as_row.start.str, as_row.end.str);
    return true;
  }

  const Vers_type_handler *row_start_vers= row_start->type_handler()->vers();

  if (!row_start_vers)
  {
    require_timestamp_error(row_start->field_name.str, table_name.str);
    return true;
  }

  return row_start_vers->check_sys_fields(table_name, row_start, row_end);
}

bool Table_period_info::check_field(const Create_field* f,
                                    const Lex_ident_column& f_name) const
{
  bool res= false;
  if (!f)
  {
    my_error(ER_BAD_FIELD_ERROR, MYF(0), f_name.str, name.str);
    res= true;
  }
  else if (f->type_handler()->mysql_timestamp_type() != MYSQL_TIMESTAMP_DATE &&
           f->type_handler()->mysql_timestamp_type() != MYSQL_TIMESTAMP_DATETIME)
  {
    my_error(ER_WRONG_FIELD_SPEC, MYF(0), f->field_name.str);
    res= true;
  }
  else if (f->vcol_info || f->flags & VERS_SYSTEM_FIELD)
  {
    my_error(ER_PERIOD_FIELD_WRONG_ATTRIBUTES, MYF(0),
             f->field_name.str, "GENERATED ALWAYS AS");
    res= true;
  }

  return res;
}

bool Table_scope_and_contents_source_st::check_fields(
  THD *thd, Alter_info *alter_info,
  const Lex_ident_table &table_name, const Lex_ident_db &db, int select_count)
{
  return vers_check_system_fields(thd, alter_info,
                                  table_name, db, select_count) ||
    check_period_fields(thd, alter_info);
}

bool Table_scope_and_contents_source_st::check_period_fields(
                THD *thd, Alter_info *alter_info)
{
  if (!period_info.name)
    return false;

  if (tmp_table())
  {
    my_error(ER_PERIOD_TEMPORARY_NOT_ALLOWED, MYF(0));
    return true;
  }

  Table_period_info::start_end_t &period= period_info.period;
  const Create_field *row_start= NULL;
  const Create_field *row_end= NULL;
  List_iterator<Create_field> it(alter_info->create_list);
  while (const Create_field *f= it++)
  {
    if (period.start.streq(f->field_name)) row_start= f;
    else if (period.end.streq(f->field_name)) row_end= f;

    if (period_info.name.streq(f->field_name))
    {
      my_error(ER_DUP_FIELDNAME, MYF(0), f->field_name.str);
      return true;
    }
  }

  bool res= period_info.check_field(row_start, period.start)
            || period_info.check_field(row_end, period.end);
  if (res)
    return true;

  if (row_start->type_handler() != row_end->type_handler()
      || row_start->length != row_end->length)
  {
    my_error(ER_PERIOD_TYPES_MISMATCH, MYF(0), period_info.name.str);
    res= true;
  }

  return res;
}

bool
Table_scope_and_contents_source_st::fix_create_fields(THD *thd,
                                                      Alter_info *alter_info,
                                                      const TABLE_LIST &create_table)
{
  return vers_fix_system_fields(thd, alter_info, create_table)
         || fix_period_fields(thd, alter_info);
}

bool
Table_scope_and_contents_source_st::fix_period_fields(THD *thd,
                                                      Alter_info *alter_info)
{
  if (!period_info.name)
    return false;

  Table_period_info::start_end_t &period= period_info.period;
  List_iterator<Create_field> it(alter_info->create_list);
  while (Create_field *f= it++)
  {
    if (period.start.streq(f->field_name) || period.end.streq(f->field_name))
    {
      f->period= &period_info;
      f->flags|= NOT_NULL_FLAG;
    }
  }
  return false;
}

/*
  Copy upper level cost to the engine as part of start statement

  This is needed to provide fast access to these variables during
  optimization (as we refer to them multiple times during one query).

  The other option would be to access them from THD, but that would
  require a function call (as we cannot easily access THD from an
  inline handler function) and two extra memory accesses for each
  variable.
*/

void handler::set_optimizer_costs(THD *thd)
{
  optimizer_where_cost=      thd->variables.optimizer_where_cost;
  optimizer_scan_setup_cost= thd->variables.optimizer_scan_setup_cost;
}
