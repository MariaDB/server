/* Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2009, 2016, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/** @file handler.cc

    @brief
  Handler-calling-functions
*/

#include <my_global.h>
#include "sql_priv.h"
#include "unireg.h"
#include "rpl_handler.h"
#include "sql_cache.h"                   // query_cache, query_cache_*
#include "sql_connect.h"                 // global_table_stats
#include "key.h"     // key_copy, key_unpack, key_cmp_if_same, key_cmp
#include "sql_table.h"                   // build_table_filename
#include "sql_parse.h"                          // check_stack_overrun
#include "sql_acl.h"            // SUPER_ACL
#include "sql_base.h"           // TDC_element
#include "discover.h"           // extension_based_table_discovery, etc
#include "log_event.h"          // *_rows_log_event
#include "create_options.h"
#include <myisampack.h>
#include "transaction.h"
#include "myisam.h"
#include "probes_mysql.h"
#include <mysql/psi/mysql_table.h>
#include "debug_sync.h"         // DEBUG_SYNC
#include "sql_audit.h"

#ifdef WITH_PARTITION_STORAGE_ENGINE
#include "ha_partition.h"
#endif

#ifdef WITH_ARIA_STORAGE_ENGINE
#include "../storage/maria/ha_maria.h"
#endif

#include "wsrep_mysqld.h"
#include "wsrep.h"
#include "wsrep_xid.h"

/*
  While we have legacy_db_type, we have this array to
  check for dups and to find handlerton from legacy_db_type.
  Remove when legacy_db_type is finally gone
*/
st_plugin_int *hton2plugin[MAX_HA];

static handlerton *installed_htons[128];

#define BITMAP_STACKBUF_SIZE (128/8)

KEY_CREATE_INFO default_key_create_info=
{ HA_KEY_ALG_UNDEF, 0, {NullS, 0}, {NullS, 0}, true };

/* number of entries in handlertons[] */
ulong total_ha= 0;
/* number of storage engines (from handlertons[]) that support 2pc */
ulong total_ha_2pc= 0;
/* size of savepoint storage area (see ha_init) */
ulong savepoint_alloc_size= 0;

static const LEX_STRING sys_table_aliases[]=
{
  { C_STRING_WITH_LEN("INNOBASE") },  { C_STRING_WITH_LEN("INNODB") },
  { C_STRING_WITH_LEN("HEAP") },      { C_STRING_WITH_LEN("MEMORY") },
  { C_STRING_WITH_LEN("MERGE") },     { C_STRING_WITH_LEN("MRG_MYISAM") },
  { C_STRING_WITH_LEN("Maria") },      { C_STRING_WITH_LEN("Aria") },
  {NullS, 0}
};

const char *ha_row_type[] = {
  "", "FIXED", "DYNAMIC", "COMPRESSED", "REDUNDANT", "COMPACT", "PAGE"
};

const char *tx_isolation_names[] =
{ "READ-UNCOMMITTED", "READ-COMMITTED", "REPEATABLE-READ", "SERIALIZABLE",
  NullS};
TYPELIB tx_isolation_typelib= {array_elements(tx_isolation_names)-1,"",
			       tx_isolation_names, NULL};

static TYPELIB known_extensions= {0,"known_exts", NULL, NULL};
uint known_extensions_id= 0;

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
plugin_ref ha_resolve_by_name(THD *thd, const LEX_STRING *name, bool tmp_table)
{
  const LEX_STRING *table_alias;
  plugin_ref plugin;

redo:
  /* my_strnncoll is a macro and gcc doesn't do early expansion of macro */
  if (thd && !my_charset_latin1.coll->strnncoll(&my_charset_latin1,
                           (const uchar *)name->str, name->length,
                           (const uchar *)STRING_WITH_LEN("DEFAULT"), 0))
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
  for (table_alias= sys_table_aliases; table_alias->str; table_alias+= 2)
  {
    if (!my_strnncoll(&my_charset_latin1,
                      (const uchar *)name->str, name->length,
                      (const uchar *)table_alias->str, table_alias->length))
    {
      name= table_alias + 1;
      goto redo;
    }
  }

  return NULL;
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
  DBUG_PRINT("enter", ("alloc: 0x%lx", (long) alloc));

  if (db_type && db_type->state == SHOW_OPTION_YES && db_type->create)
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
    my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR), 
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

int ha_init_errors(void)
{
#define SETMSG(nr, msg) handler_errmsgs[(nr) - HA_ERR_FIRST]= (msg)

  /* Allocate a pointer array for the error message strings. */
  /* Zerofill it to avoid uninitialized gaps. */
  if (! (handler_errmsgs= (const char**) my_malloc(HA_ERR_ERRORS * sizeof(char*),
                                                   MYF(MY_WME | MY_ZEROFILL))))
    return 1;

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
  SETMSG(HA_ERR_TABLE_NEEDS_UPGRADE,    "Table upgrade required. Please do \"REPAIR TABLE %`\" or dump/reload to fix it");
  SETMSG(HA_ERR_TABLE_READONLY,         ER_DEFAULT(ER_OPEN_AS_READONLY));
  SETMSG(HA_ERR_AUTOINC_READ_FAILED,    ER_DEFAULT(ER_AUTOINC_READ_FAILED));
  SETMSG(HA_ERR_AUTOINC_ERANGE,         ER_DEFAULT(ER_WARN_DATA_OUT_OF_RANGE));
  SETMSG(HA_ERR_TOO_MANY_CONCURRENT_TRXS, ER_DEFAULT(ER_TOO_MANY_CONCURRENT_TRXS));
  SETMSG(HA_ERR_INDEX_COL_TOO_LONG,	ER_DEFAULT(ER_INDEX_COLUMN_TOO_LONG));
  SETMSG(HA_ERR_INDEX_CORRUPT,		ER_DEFAULT(ER_INDEX_CORRUPT));
  SETMSG(HA_FTS_INVALID_DOCID,		"Invalid InnoDB FTS Doc ID");
  SETMSG(HA_ERR_TABLE_IN_FK_CHECK,	ER_DEFAULT(ER_TABLE_IN_FK_CHECK));
  SETMSG(HA_ERR_DISK_FULL,              ER_DEFAULT(ER_DISK_FULL));
  SETMSG(HA_ERR_FTS_TOO_MANY_WORDS_IN_PHRASE,  "Too many words in a FTS phrase or proximity search");

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

static volatile int32 need_full_discover_for_existence= 0;
static volatile int32 engines_with_discover_table_names= 0;
static volatile int32 engines_with_discover= 0;

static int full_discover_for_existence(handlerton *, const char *, const char *)
{ return 0; }

static int ext_based_existence(handlerton *, const char *, const char *)
{ return 0; }

static int hton_ext_based_table_discovery(handlerton *hton, LEX_STRING *db,
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
    my_atomic_add32(&need_full_discover_for_existence,  val);

  if (hton->discover_table_names)
    my_atomic_add32(&engines_with_discover_table_names, val);

  if (hton->discover_table)
    my_atomic_add32(&engines_with_discover, val);
}

int ha_finalize_handlerton(st_plugin_int *plugin)
{
  handlerton *hton= (handlerton *)plugin->data;
  DBUG_ENTER("ha_finalize_handlerton");

  /* hton can be NULL here, if ha_initialize_handlerton() failed. */
  if (!hton)
    goto end;

  switch (hton->state) {
  case SHOW_OPTION_NO:
  case SHOW_OPTION_DISABLED:
    break;
  case SHOW_OPTION_YES:
    if (installed_htons[hton->db_type] == hton)
      installed_htons[hton->db_type]= NULL;
    break;
  };

  if (hton->panic)
    hton->panic(hton, HA_PANIC_CLOSE);

  if (plugin->plugin->deinit)
  {
    /*
      Today we have no defined/special behavior for uninstalling
      engine plugins.
    */
    DBUG_PRINT("info", ("Deinitializing plugin: '%s'", plugin->name.str));
    if (plugin->plugin->deinit(NULL))
    {
      DBUG_PRINT("warning", ("Plugin '%s' deinit function returned error.",
                             plugin->name.str));
    }
  }

  free_sysvar_table_options(hton);
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
  DBUG_RETURN(0);
}


int ha_initialize_handlerton(st_plugin_int *plugin)
{
  handlerton *hton;
  static const char *no_exts[]= { 0 };
  DBUG_ENTER("ha_initialize_handlerton");
  DBUG_PRINT("plugin", ("initialize plugin: '%s'", plugin->name.str));

  hton= (handlerton *)my_malloc(sizeof(handlerton),
                                MYF(MY_WME | MY_ZEROFILL));
  if (hton == NULL)
  {
    sql_print_error("Unable to allocate memory for plugin '%s' handlerton.",
                    plugin->name.str);
    goto err_no_hton_memory;
  }

  hton->tablefile_extensions= no_exts;
  hton->discover_table_names= hton_ext_based_table_discovery;

  hton->slot= HA_SLOT_UNDEF;
  /* Historical Requirement */
  plugin->data= hton; // shortcut for the future
  if (plugin->plugin->init && plugin->plugin->init(hton))
  {
    sql_print_error("Plugin '%s' init function returned error.",
		    plugin->name.str);
    goto err;
  }

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

  switch (hton->state) {
  case SHOW_OPTION_NO:
    break;
  case SHOW_OPTION_YES:
    {
      uint tmp;
      ulong fslot;

      DBUG_EXECUTE_IF("unstable_db_type", {
                        static int i= (int) DB_TYPE_FIRST_DYNAMIC;
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
	  goto err_deinit;
        }
        if (hton->db_type != DB_TYPE_UNKNOWN)
          sql_print_warning("Storage engine '%s' has conflicting typecode. "
                            "Assigning value %d.", plugin->plugin->name, idx);
        hton->db_type= (enum legacy_db_type) idx;
      }

      /*
        In case a plugin is uninstalled and re-installed later, it should
        reuse an array slot. Otherwise the number of uninstall/install
        cycles would be limited. So look for a free slot.
      */
      DBUG_PRINT("plugin", ("total_ha: %lu", total_ha));
      for (fslot= 0; fslot < total_ha; fslot++)
      {
        if (!hton2plugin[fslot])
          break;
      }
      if (fslot < total_ha)
        hton->slot= fslot;
      else
      {
        if (total_ha >= MAX_HA)
        {
          sql_print_error("Too many plugins loaded. Limit is %lu. "
                          "Failed on '%s'", (ulong) MAX_HA, plugin->name.str);
          goto err_deinit;
        }
        hton->slot= total_ha++;
      }
      installed_htons[hton->db_type]= hton;
      tmp= hton->savepoint_offset;
      hton->savepoint_offset= savepoint_alloc_size;
      savepoint_alloc_size+= tmp;
      hton2plugin[hton->slot]=plugin;
      if (hton->prepare)
      {
        total_ha_2pc++;
        if (tc_log && tc_log != get_tc_log_implementation())
        {
          total_ha_2pc--;
          hton->prepare= 0;
          push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_UNKNOWN_ERROR,
                              "Cannot enable tc-log at run-time. "
                              "XA features of %s are disabled",
                              plugin->name.str);
        }
      }
      break;
    }
    /* fall through */
  default:
    hton->state= SHOW_OPTION_DISABLED;
    break;
  }
  
  /* 
    This is entirely for legacy. We will create a new "disk based" hton and a 
    "memory" hton which will be configurable longterm. We should be able to 
    remove partition and myisammrg.
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
  default:
    break;
  };

  resolve_sysvar_table_options(hton);
  update_discovery_counters(hton, 1);

  DBUG_RETURN(0);

err_deinit:
  /* 
    Let plugin do its inner deinitialization as plugin->init() 
    was successfully called before.
  */
  if (plugin->plugin->deinit)
    (void) plugin->plugin->deinit(NULL);
          
err:
  my_free(hton);
err_no_hton_memory:
  plugin->data= NULL;
  DBUG_RETURN(1);
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
  opt_using_transactions= total_ha>(ulong)opt_bin_log;
  savepoint_alloc_size+= sizeof(SAVEPOINT);
  DBUG_RETURN(error);
}

int ha_end()
{
  int error= 0;
  DBUG_ENTER("ha_end");


  /* 
    This should be eventualy based  on the graceful shutdown flag.
    So if flag is equal to HA_PANIC_CLOSE, the deallocate
    the errors.
  */
  if (ha_finish_errors())
    error= 1;

  DBUG_RETURN(error);
}

static my_bool dropdb_handlerton(THD *unused1, plugin_ref plugin,
                                 void *path)
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->state == SHOW_OPTION_YES && hton->drop_database)
    hton->drop_database(hton, (char *)path);
  return FALSE;
}


void ha_drop_database(char* path)
{
  plugin_foreach(NULL, dropdb_handlerton, MYSQL_STORAGE_ENGINE_PLUGIN, path);
}


static my_bool checkpoint_state_handlerton(THD *unused1, plugin_ref plugin,
                                           void *disable)
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->state == SHOW_OPTION_YES && hton->checkpoint_state)
    hton->checkpoint_state(hton, (int) *(bool*) disable);
  return FALSE;
}


void ha_checkpoint_state(bool disable)
{
  plugin_foreach(NULL, checkpoint_state_handlerton, MYSQL_STORAGE_ENGINE_PLUGIN, &disable);
}


struct st_commit_checkpoint_request {
  void *cookie;
  void (*pre_hook)(void *);
};

static my_bool commit_checkpoint_request_handlerton(THD *unused1, plugin_ref plugin,
                                           void *data)
{
  st_commit_checkpoint_request *st= (st_commit_checkpoint_request *)data;
  handlerton *hton= plugin_hton(plugin);
  if (hton->state == SHOW_OPTION_YES && hton->commit_checkpoint_request)
  {
    void *cookie= st->cookie;
    if (st->pre_hook)
      (*st->pre_hook)(cookie);
    (*hton->commit_checkpoint_request)(hton, cookie);
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
  plugin_foreach(NULL, commit_checkpoint_request_handlerton,
                 MYSQL_STORAGE_ENGINE_PLUGIN, &st);
}



static my_bool closecon_handlerton(THD *thd, plugin_ref plugin,
                                   void *unused)
{
  handlerton *hton= plugin_hton(plugin);
  /*
    there's no need to rollback here as all transactions must
    be rolled back already
  */
  if (hton->state == SHOW_OPTION_YES && thd_get_ha_data(thd, hton))
  {
    if (hton->close_connection)
      hton->close_connection(hton, thd);
    /* make sure ha_data is reset and ha_data_lock is released */
    thd_set_ha_data(thd, hton, NULL);
  }
  return FALSE;
}

/**
  @note
    don't bother to rollback here, it's done already
*/
void ha_close_connection(THD* thd)
{
  plugin_foreach(thd, closecon_handlerton, MYSQL_STORAGE_ENGINE_PLUGIN, 0);
}

static my_bool kill_handlerton(THD *thd, plugin_ref plugin,
                               void *level)
{
  handlerton *hton= plugin_hton(plugin);

  if (hton->state == SHOW_OPTION_YES && hton->kill_query &&
      thd_get_ha_data(thd, hton))
    hton->kill_query(hton, thd, *(enum thd_kill_levels *) level);
  return FALSE;
}

void ha_kill_query(THD* thd, enum thd_kill_levels level)
{
  DBUG_ENTER("ha_kill_query");
  plugin_foreach(thd, kill_handlerton, MYSQL_STORAGE_ENGINE_PLUGIN, &level);
  DBUG_VOID_RETURN;
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
void trans_register_ha(THD *thd, bool all, handlerton *ht_arg)
{
  THD_TRANS *trans;
  Ha_trx_info *ha_info;
  DBUG_ENTER("trans_register_ha");
  DBUG_PRINT("enter",("%s", all ? "all" : "stmt"));

  if (all)
  {
    trans= &thd->transaction.all;
    thd->server_status|= SERVER_STATUS_IN_TRANS;
    if (thd->tx_read_only)
      thd->server_status|= SERVER_STATUS_IN_TRANS_READONLY;
    DBUG_PRINT("info", ("setting SERVER_STATUS_IN_TRANS"));
  }
  else
    trans= &thd->transaction.stmt;

  ha_info= thd->ha_data[ht_arg->slot].ha_info + (all ? 1 : 0);

  if (ha_info->is_started())
    DBUG_VOID_RETURN; /* already registered, return */

  ha_info->register_ha(trans, ht_arg);

  trans->no_2pc|=(ht_arg->prepare==0);
  if (thd->transaction.xid_state.xid.is_null())
    thd->transaction.xid_state.xid.set(thd->query_id);
  DBUG_VOID_RETURN;
}


static int prepare_or_error(handlerton *ht, THD *thd, bool all)
{
  int err= ht->prepare(ht, thd, all);
  status_var_increment(thd->status_var.ha_prepare_count);
  if (err)
  {
    /* avoid sending error, if we're going to replay the transaction */
#ifdef WITH_WSREP
    if (ht != wsrep_hton ||
        err == EMSGSIZE || thd->wsrep_conflict_state != MUST_REPLAY)
#endif
      my_error(ER_ERROR_DURING_COMMIT, MYF(0), err);
  }
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
  THD_TRANS *trans=all ? &thd->transaction.all : &thd->transaction.stmt;
  Ha_trx_info *ha_info= trans->ha_list;
  DBUG_ENTER("ha_prepare");

  if (ha_info)
  {
    for (; ha_info; ha_info= ha_info->next())
    {
      handlerton *ht= ha_info->ht();
      if (ht->prepare)
      {
        if (prepare_or_error(ht, thd, all))
        {
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
  }

  DBUG_RETURN(error);
}

/**
  Check if we can skip the two-phase commit.

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

static
uint
ha_check_and_coalesce_trx_read_only(THD *thd, Ha_trx_info *ha_list,
                                    bool all)
{
  /* The number of storage engines that have actual changes. */
  unsigned rw_ha_count= 0;
  Ha_trx_info *ha_info;

  for (ha_info= ha_list; ha_info; ha_info= ha_info->next())
  {
    if (ha_info->is_trx_read_write())
      ++rw_ha_count;

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
      break;
    }
  }
  return rw_ha_count;
}


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
  THD_TRANS *trans= all ? &thd->transaction.all : &thd->transaction.stmt;
  /*
    "real" is a nick name for a transaction for which a commit will
    make persistent changes. E.g. a 'stmt' transaction inside a 'all'
    transation is not 'real': even though it's possible to commit it,
    the changes are not durable as they might be rolled back if the
    enclosing 'all' transaction is rolled back.
  */
  bool is_real_trans= ((all || thd->transaction.all.ha_list == 0) &&
                       !(thd->variables.option_bits & OPTION_GTID_BEGIN));
  Ha_trx_info *ha_info= trans->ha_list;
  bool need_prepare_ordered, need_commit_ordered;
  my_xid xid;
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
  DBUG_ASSERT(thd->transaction.stmt.ha_list == NULL ||
              trans == &thd->transaction.stmt);

  if (thd->in_sub_stmt)
  {
    DBUG_ASSERT(0);
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

#ifdef WITH_ARIA_STORAGE_ENGINE
    ha_maria::implicit_commit(thd, TRUE);
#endif

  if (!ha_info)
  {
    /*
      Free resources and perform other cleanup even for 'empty' transactions.
    */
    if (is_real_trans)
      thd->transaction.cleanup();
    DBUG_RETURN(0);
  }

  DBUG_EXECUTE_IF("crash_commit_before", DBUG_SUICIDE(););

  /* Close all cursors that can not survive COMMIT */
  if (is_real_trans)                          /* not a statement commit */
    thd->stmt_map.close_transient_cursors();

  uint rw_ha_count= ha_check_and_coalesce_trx_read_only(thd, ha_info, all);
  /* rw_trans is TRUE when we in a transaction changing data */
  bool rw_trans= is_real_trans && (rw_ha_count > 0);
  MDL_request mdl_request;
  DBUG_PRINT("info", ("is_real_trans: %d  rw_trans:  %d  rw_ha_count: %d",
                      is_real_trans, rw_trans, rw_ha_count));

  if (rw_trans)
  {
    /*
      Acquire a metadata lock which will ensure that COMMIT is blocked
      by an active FLUSH TABLES WITH READ LOCK (and vice versa:
      COMMIT in progress blocks FTWRL).

      We allow the owner of FTWRL to COMMIT; we assume that it knows
      what it does.
    */
    mdl_request.init(MDL_key::COMMIT, "", "", MDL_INTENTION_EXCLUSIVE,
                     MDL_EXPLICIT);

    if (!WSREP(thd) &&
      thd->mdl_context.acquire_lock(&mdl_request,
                                    thd->variables.lock_wait_timeout))
    {
      ha_rollback_trans(thd, all);
      DBUG_RETURN(1);
    }

    DEBUG_SYNC(thd, "ha_commit_trans_after_acquire_commit_lock");
  }

  if (rw_trans &&
      opt_readonly &&
      !(thd->security_ctx->master_access & SUPER_ACL) &&
      !thd->slave_thread)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--read-only");
    goto err;
  }

  if (trans->no_2pc || (rw_ha_count <= 1))
  {
    error= ha_commit_one_phase(thd, all);
    goto done;
  }

  need_prepare_ordered= FALSE;
  need_commit_ordered= FALSE;
  xid= thd->transaction.xid_state.xid.get_my_xid();

  for (Ha_trx_info *hi= ha_info; hi; hi= hi->next())
  {
    handlerton *ht= hi->ht();
    /*
      Do not call two-phase commit if this particular
      transaction is read-only. This allows for simpler
      implementation in engines that are always read-only.
    */
    if (! hi->is_trx_read_write())
      continue;
    /*
      Sic: we know that prepare() is not NULL since otherwise
      trans->no_2pc would have been set.
    */
    if (prepare_or_error(ht, thd, all))
      goto err;

    need_prepare_ordered|= (ht->prepare_ordered != NULL);
    need_commit_ordered|= (ht->commit_ordered != NULL);
  }
  DEBUG_SYNC(thd, "ha_commit_trans_after_prepare");
  DBUG_EXECUTE_IF("crash_commit_after_prepare", DBUG_SUICIDE(););

  if (!error && WSREP_ON && wsrep_is_wsrep_xid(&thd->transaction.xid_state.xid))
  {
    // xid was rewritten by wsrep
    xid= wsrep_xid_seqno(thd->transaction.xid_state.xid);
  }

  if (!is_real_trans)
  {
    error= commit_one_phase_2(thd, all, trans, is_real_trans);
    goto done;
  }

  DEBUG_SYNC(thd, "ha_commit_trans_before_log_and_order");
  cookie= tc_log->log_and_order(thd, xid, all, need_prepare_ordered,
                                need_commit_ordered);
  if (!cookie)
    goto err;

  DEBUG_SYNC(thd, "ha_commit_trans_after_log_and_order");
  DBUG_EXECUTE_IF("crash_commit_after_log", DBUG_SUICIDE(););

  error= commit_one_phase_2(thd, all, trans, is_real_trans) ? 2 : 0;

  DBUG_EXECUTE_IF("crash_commit_before_unlog", DBUG_SUICIDE(););
  if (tc_log->unlog(cookie, xid))
  {
    error= 2;                                /* Error during commit */
    goto end;
  }

done:
  DBUG_EXECUTE_IF("crash_commit_after", DBUG_SUICIDE(););

  mysql_mutex_assert_not_owner(&LOCK_prepare_ordered);
  mysql_mutex_assert_not_owner(mysql_bin_log.get_log_lock());
  mysql_mutex_assert_not_owner(&LOCK_after_binlog_sync);
  mysql_mutex_assert_not_owner(&LOCK_commit_ordered);
  RUN_HOOK(transaction, after_commit, (thd, FALSE));
  goto end;

  /* Come here if error and we need to rollback. */
err:
  error= 1;                                  /* Transaction was rolled back */
  /*
    In parallel replication, rollback is delayed, as there is extra replication
    book-keeping to be done before rolling back and allowing a conflicting
    transaction to continue (MDEV-7458).
  */
  if (!(thd->rgi_slave && thd->rgi_slave->is_parallel_exec))
    ha_rollback_trans(thd, all);

end:
  if (rw_trans && mdl_request.ticket)
  {
    /*
      We do not always immediately release transactional locks
      after ha_commit_trans() (see uses of ha_enable_transaction()),
      thus we release the commit blocker lock as soon as it's
      not needed.
    */
    thd->mdl_context.release_lock(mdl_request.ticket);
  }
  DBUG_RETURN(error);
}

/**
  @note
  This function does not care about global read lock. A caller should.

  @param[in]  all  Is set in case of explicit commit
                   (COMMIT statement), or implicit commit
                   issued by DDL. Is not set when called
                   at the end of statement, even if
                   autocommit=1.
*/

int ha_commit_one_phase(THD *thd, bool all)
{
  THD_TRANS *trans=all ? &thd->transaction.all : &thd->transaction.stmt;
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
  bool is_real_trans= ((all || thd->transaction.all.ha_list == 0) &&
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


static int
commit_one_phase_2(THD *thd, bool all, THD_TRANS *trans, bool is_real_trans)
{
  int error= 0;
  Ha_trx_info *ha_info= trans->ha_list, *ha_info_next;
  DBUG_ENTER("commit_one_phase_2");
  if (is_real_trans)
    DEBUG_SYNC(thd, "commit_one_phase_2");
  if (ha_info)
  {
    for (; ha_info; ha_info= ha_info_next)
    {
      int err;
      handlerton *ht= ha_info->ht();
      if ((err= ht->commit(ht, thd, all)))
      {
        my_error(ER_ERROR_DURING_COMMIT, MYF(0), err);
        error=1;
      }
      /* Should this be done only if is_real_trans is set ? */
      status_var_increment(thd->status_var.ha_commit_count);
      ha_info_next= ha_info->next();
      ha_info->reset(); /* keep it conveniently zero-filled */
    }
    trans->ha_list= 0;
    trans->no_2pc=0;
    if (all)
    {
#ifdef HAVE_QUERY_CACHE
      if (thd->transaction.changed_tables)
        query_cache.invalidate(thd, thd->transaction.changed_tables);
#endif
    }
  }
  /* Free resources and perform other cleanup even for 'empty' transactions. */
  if (is_real_trans)
  {
    thd->has_waiter= false;
    thd->transaction.cleanup();
  }

  DBUG_RETURN(error);
}


int ha_rollback_trans(THD *thd, bool all)
{
  int error=0;
  THD_TRANS *trans=all ? &thd->transaction.all : &thd->transaction.stmt;
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
  bool is_real_trans=all || thd->transaction.all.ha_list == 0;
  DBUG_ENTER("ha_rollback_trans");

  /*
    We must not rollback the normal transaction if a statement
    transaction is pending.
  */
  DBUG_ASSERT(thd->transaction.stmt.ha_list == NULL ||
              trans == &thd->transaction.stmt);

#ifdef HAVE_REPLICATION
  if (is_real_trans)
  {
    /*
      In parallel replication, if we need to rollback during commit, we must
      first inform following transactions that we are going to abort our commit
      attempt. Otherwise those following transactions can run too early, and
      possibly cause replication to fail. See comments in retry_event_group().

      There were several bugs with this in the past that were very hard to
      track down (MDEV-7458, MDEV-8302). So we add here an assertion for
      rollback without signalling following transactions. And in release
      builds, we explicitly do the signalling before rolling back.
    */
    DBUG_ASSERT(!(thd->rgi_slave && thd->rgi_slave->did_mark_start_commit));
    if (thd->rgi_slave && thd->rgi_slave->did_mark_start_commit)
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

  if (ha_info)
  {
    /* Close all cursors that can not survive ROLLBACK */
    if (is_real_trans)                          /* not a statement commit */
      thd->stmt_map.close_transient_cursors();

    for (; ha_info; ha_info= ha_info_next)
    {
      int err;
      handlerton *ht= ha_info->ht();
      if ((err= ht->rollback(ht, thd, all)))
      { // cannot happen
        my_error(ER_ERROR_DURING_ROLLBACK, MYF(0), err);
        error=1;
      }
      status_var_increment(thd->status_var.ha_rollback_count);
      ha_info_next= ha_info->next();
      ha_info->reset(); /* keep it conveniently zero-filled */
    }
    trans->ha_list= 0;
    trans->no_2pc=0;
  }

  /*
    Thanks to possibility of MDL deadlock rollback request can come even if
    transaction hasn't been started in any transactional storage engine.
  */
  if (is_real_trans && thd->transaction_rollback_request &&
      thd->transaction.xid_state.xa_state != XA_NOTR)
    thd->transaction.xid_state.rm_error= thd->get_stmt_da()->sql_errno();

  /* Always cleanup. Even if nht==0. There may be savepoints. */
  if (is_real_trans)
  {
    thd->has_waiter= false;
    thd->transaction.cleanup();
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
  if (is_real_trans && thd->transaction.all.modified_non_trans_table &&
      !thd->slave_thread && thd->killed < KILL_CONNECTION)
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_WARNING_NOT_COMPLETE_ROLLBACK,
                 ER_THD(thd, ER_WARNING_NOT_COMPLETE_ROLLBACK));
  (void) RUN_HOOK(transaction, after_rollback, (thd, FALSE));
  DBUG_RETURN(error);
}


struct xahton_st {
  XID *xid;
  int result;
};

static my_bool xacommit_handlerton(THD *unused1, plugin_ref plugin,
                                   void *arg)
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->state == SHOW_OPTION_YES && hton->recover)
  {
    hton->commit_by_xid(hton, ((struct xahton_st *)arg)->xid);
    ((struct xahton_st *)arg)->result= 0;
  }
  return FALSE;
}

static my_bool xarollback_handlerton(THD *unused1, plugin_ref plugin,
                                     void *arg)
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->state == SHOW_OPTION_YES && hton->recover)
  {
    hton->rollback_by_xid(hton, ((struct xahton_st *)arg)->xid);
    ((struct xahton_st *)arg)->result= 0;
  }
  return FALSE;
}


int ha_commit_or_rollback_by_xid(XID *xid, bool commit)
{
  struct xahton_st xaop;
  xaop.xid= xid;
  xaop.result= 1;

  plugin_foreach(NULL, commit ? xacommit_handlerton : xarollback_handlerton,
                 MYSQL_STORAGE_ENGINE_PLUGIN, &xaop);

  return xaop.result;
}


#ifndef DBUG_OFF
/**
  @note
    This does not need to be multi-byte safe or anything
*/
static char* xid_to_str(char *buf, XID *xid)
{
  int i;
  char *s=buf;
  *s++='\'';
  for (i=0; i < xid->gtrid_length+xid->bqual_length; i++)
  {
    uchar c=(uchar)xid->data[i];
    /* is_next_dig is set if next character is a number */
    bool is_next_dig= FALSE;
    if (i < XIDDATASIZE)
    {
      char ch= xid->data[i+1];
      is_next_dig= (ch >= '0' && ch <='9');
    }
    if (i == xid->gtrid_length)
    {
      *s++='\'';
      if (xid->bqual_length)
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

/**
  recover() step of xa.

  @note
    there are three modes of operation:
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
*/
struct xarecover_st
{
  int len, found_foreign_xids, found_my_xids;
  XID *list;
  HASH *commit_list;
  bool dry_run;
};

static my_bool xarecover_handlerton(THD *unused, plugin_ref plugin,
                                    void *arg)
{
  handlerton *hton= plugin_hton(plugin);
  struct xarecover_st *info= (struct xarecover_st *) arg;
  int got;

  if (hton->state == SHOW_OPTION_YES && hton->recover)
  {
    while ((got= hton->recover(hton, info->list, info->len)) > 0 )
    {
      sql_print_information("Found %d prepared transaction(s) in %s",
                            got, hton_name(hton)->str);
      for (int i=0; i < got; i ++)
      {
        my_xid x= WSREP_ON && wsrep_is_wsrep_xid(&info->list[i]) ?
                  wsrep_xid_seqno(info->list[i]) :
                  info->list[i].get_my_xid();
        if (!x) // not "mine" - that is generated by external TM
        {
#ifndef DBUG_OFF
          char buf[XIDDATASIZE*4+6]; // see xid_to_str
          sql_print_information("ignore xid %s", xid_to_str(buf, info->list+i));
#endif
          xid_cache_insert(info->list+i, XA_PREPARED);
          info->found_foreign_xids++;
          continue;
        }
        if (info->dry_run)
        {
          info->found_my_xids++;
          continue;
        }
        // recovery mode
        if (info->commit_list ?
            my_hash_search(info->commit_list, (uchar *)&x, sizeof(x)) != 0 :
            tc_heuristic_recover == TC_HEURISTIC_RECOVER_COMMIT)
        {
#ifndef DBUG_OFF
          char buf[XIDDATASIZE*4+6]; // see xid_to_str
          sql_print_information("commit xid %s", xid_to_str(buf, info->list+i));
#endif
          hton->commit_by_xid(hton, info->list+i);
        }
        else
        {
#ifndef DBUG_OFF
          char buf[XIDDATASIZE*4+6]; // see xid_to_str
          sql_print_information("rollback xid %s",
                                xid_to_str(buf, info->list+i));
#endif
          hton->rollback_by_xid(hton, info->list+i);
        }
      }
      if (got < info->len)
        break;
    }
  }
  return FALSE;
}

int ha_recover(HASH *commit_list)
{
  struct xarecover_st info;
  DBUG_ENTER("ha_recover");
  info.found_foreign_xids= info.found_my_xids= 0;
  info.commit_list= commit_list;
  info.dry_run= (info.commit_list==0 && tc_heuristic_recover==0);
  info.list= NULL;

  /* commit_list and tc_heuristic_recover cannot be set both */
  DBUG_ASSERT(info.commit_list==0 || tc_heuristic_recover==0);
  /* if either is set, total_ha_2pc must be set too */
  DBUG_ASSERT(info.dry_run || total_ha_2pc>(ulong)opt_bin_log);

  if (total_ha_2pc <= (ulong)opt_bin_log)
    DBUG_RETURN(0);

  if (info.commit_list)
    sql_print_information("Starting crash recovery...");

  for (info.len= MAX_XID_LIST_SIZE ; 
       info.list==0 && info.len > MIN_XID_LIST_SIZE; info.len/=2)
  {
    info.list=(XID *)my_malloc(info.len*sizeof(XID), MYF(0));
  }
  if (!info.list)
  {
    sql_print_error(ER(ER_OUTOFMEMORY),
                    static_cast<int>(info.len*sizeof(XID)));
    DBUG_RETURN(1);
  }

  plugin_foreach(NULL, xarecover_handlerton, 
                 MYSQL_STORAGE_ENGINE_PLUGIN, &info);

  my_free(info.list);
  if (info.found_foreign_xids)
    sql_print_warning("Found %d prepared XA transactions", 
                      info.found_foreign_xids);
  if (info.dry_run && info.found_my_xids)
  {
    sql_print_error("Found %d prepared transactions! It means that mysqld was "
                    "not shut down properly last time and critical recovery "
                    "information (last binlog or %s file) was manually deleted "
                    "after a crash. You have to start mysqld with "
                    "--tc-heuristic-recover switch to commit or rollback "
                    "pending transactions.",
                    info.found_my_xids, opt_tc_log_file);
    DBUG_RETURN(1);
  }
  if (info.commit_list)
    sql_print_information("Crash recovery finished.");
  DBUG_RETURN(0);
}

/**
  return the list of XID's to a client, the same way SHOW commands do.

  @note
    I didn't find in XA specs that an RM cannot return the same XID twice,
    so mysql_xa_recover does not filter XID's to ensure uniqueness.
    It can be easily fixed later, if necessary.
*/

static my_bool xa_recover_callback(XID_STATE *xs, Protocol *protocol)
{
  if (xs->xa_state == XA_PREPARED)
  {
    protocol->prepare_for_resend();
    protocol->store_longlong((longlong) xs->xid.formatID, FALSE);
    protocol->store_longlong((longlong) xs->xid.gtrid_length, FALSE);
    protocol->store_longlong((longlong) xs->xid.bqual_length, FALSE);
    protocol->store(xs->xid.data, xs->xid.gtrid_length + xs->xid.bqual_length,
                    &my_charset_bin);
    if (protocol->write())
      return TRUE;
  }
  return FALSE;
}


bool mysql_xa_recover(THD *thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  MEM_ROOT *mem_root= thd->mem_root;
  DBUG_ENTER("mysql_xa_recover");

  field_list.push_back(new (mem_root)
                       Item_int(thd, "formatID", 0,
                                MY_INT32_NUM_DECIMAL_DIGITS), mem_root);
  field_list.push_back(new (mem_root)
                       Item_int(thd, "gtrid_length", 0,
                                MY_INT32_NUM_DECIMAL_DIGITS), mem_root);
  field_list.push_back(new (mem_root)
                       Item_int(thd, "bqual_length", 0,
                                MY_INT32_NUM_DECIMAL_DIGITS), mem_root);
  field_list.push_back(new (mem_root)
                       Item_empty_string(thd, "data",
                                         XIDDATASIZE), mem_root);

  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(1);

  if (xid_cache_iterate(thd, (my_hash_walk_action) xa_recover_callback,
                        protocol))
    DBUG_RETURN(1);
  my_eof(thd);
  DBUG_RETURN(0);
}

/*
  Called by engine to notify TC that a new commit checkpoint has been reached.
  See comments on handlerton method commit_checkpoint_request() for details.
*/
void
commit_checkpoint_notify_ha(handlerton *hton, void *cookie)
{
  tc_log->commit_checkpoint_notify(cookie);
}


/**
  @details
  This function should be called when MySQL sends rows of a SELECT result set
  or the EOF mark to the client. It releases a possible adaptive hash index
  S-latch held by thd in InnoDB and also releases a possible InnoDB query
  FIFO ticket to enter InnoDB. To save CPU time, InnoDB allows a thd to
  keep them over several calls of the InnoDB handler interface when a join
  is executed. But when we let the control to pass to the client they have
  to be released because if the application program uses mysql_use_result(),
  it may deadlock on the S-latch if the application on another connection
  performs another SQL query. In MySQL-4.1 this is even more important because
  there a connection can have several SELECT queries open at the same time.

  @param thd           the thread handle of the current connection

  @return
    always 0
*/

int ha_release_temporary_latches(THD *thd)
{
  Ha_trx_info *info;

  /*
    Note that below we assume that only transactional storage engines
    may need release_temporary_latches(). If this will ever become false,
    we could iterate on thd->open_tables instead (and remove duplicates
    as if (!seen[hton->slot]) { seen[hton->slot]=1; ... }).
  */
  for (info= thd->transaction.stmt.ha_list; info; info= info->next())
  {
    handlerton *hton= info->ht();
    if (hton && hton->release_temporary_latches)
        hton->release_temporary_latches(hton, thd);
  }
  return 0;
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
  THD_TRANS *trans= (thd->in_sub_stmt ? &thd->transaction.stmt :
                                        &thd->transaction.all);

  DBUG_ENTER("ha_rollback_to_savepoint_can_release_mdl");

  /**
    Checking whether it is safe to release metadata locks after rollback to
    savepoint in all the storage engines that are part of the transaction.
  */
  for (ha_info= trans->ha_list; ha_info; ha_info= ha_info->next())
  {
    handlerton *ht= ha_info->ht();
    DBUG_ASSERT(ht);

    if (ht->savepoint_rollback_can_release_mdl == 0 ||
        ht->savepoint_rollback_can_release_mdl(ht, thd) == false)
      DBUG_RETURN(false);
  }

  DBUG_RETURN(true);
}

int ha_rollback_to_savepoint(THD *thd, SAVEPOINT *sv)
{
  int error=0;
  THD_TRANS *trans= (thd->in_sub_stmt ? &thd->transaction.stmt :
                                        &thd->transaction.all);
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
    handlerton *ht= ha_info->ht();
    DBUG_ASSERT(ht);
    DBUG_ASSERT(ht->savepoint_set != 0);
    if ((err= ht->savepoint_rollback(ht, thd,
                                     (uchar *)(sv+1)+ht->savepoint_offset)))
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
    handlerton *ht= ha_info->ht();
    if ((err= ht->rollback(ht, thd, !thd->in_sub_stmt)))
    { // cannot happen
      my_error(ER_ERROR_DURING_ROLLBACK, MYF(0), err);
      error=1;
    }
    status_var_increment(thd->status_var.ha_rollback_count);
    ha_info_next= ha_info->next();
    ha_info->reset(); /* keep it conveniently zero-filled */
  }
  trans->ha_list= sv->ha_list;
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
  int error=0;
  THD_TRANS *trans= (thd->in_sub_stmt ? &thd->transaction.stmt :
                                        &thd->transaction.all);
  Ha_trx_info *ha_info= trans->ha_list;
  DBUG_ENTER("ha_savepoint");

  for (; ha_info; ha_info= ha_info->next())
  {
    int err;
    handlerton *ht= ha_info->ht();
    DBUG_ASSERT(ht);
    if (! ht->savepoint_set)
    {
      my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), "SAVEPOINT");
      error=1;
      break;
    }
    if ((err= ht->savepoint_set(ht, thd, (uchar *)(sv+1)+ht->savepoint_offset)))
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
    handlerton *ht= ha_info->ht();
    /* Savepoint life time is enclosed into transaction life time. */
    DBUG_ASSERT(ht);
    if (!ht->savepoint_release)
      continue;
    if ((err= ht->savepoint_release(ht, thd,
                                    (uchar *)(sv+1) + ht->savepoint_offset)))
    { // cannot happen
      my_error(ER_GET_ERRNO, MYF(0), err, hton_name(ht)->str);
      error=1;
    }
  }
  DBUG_RETURN(error);
}


static my_bool snapshot_handlerton(THD *thd, plugin_ref plugin,
                                   void *arg)
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->state == SHOW_OPTION_YES &&
      hton->start_consistent_snapshot)
  {
    hton->start_consistent_snapshot(hton, thd);
    *((bool *)arg)= false;
  }
  return FALSE;
}

int ha_start_consistent_snapshot(THD *thd)
{
  bool warn= true;

  /*
    Holding the LOCK_commit_ordered mutex ensures that we get the same
    snapshot for all engines (including the binary log).  This allows us
    among other things to do backups with
    START TRANSACTION WITH CONSISTENT SNAPSHOT and
    have a consistent binlog position.
  */
  mysql_mutex_lock(&LOCK_commit_ordered);
  plugin_foreach(thd, snapshot_handlerton, MYSQL_STORAGE_ENGINE_PLUGIN, &warn);
  mysql_mutex_unlock(&LOCK_commit_ordered);

  /*
    Same idea as when one wants to CREATE TABLE in one engine which does not
    exist:
  */
  if (warn)
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                 "This MySQL server does not support any "
                 "consistent-read capable storage engine");
  return 0;
}


static my_bool flush_handlerton(THD *thd, plugin_ref plugin,
                                void *arg)
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->state == SHOW_OPTION_YES && hton->flush_logs && 
      hton->flush_logs(hton))
    return TRUE;
  return FALSE;
}


bool ha_flush_logs(handlerton *db_type)
{
  if (db_type == NULL)
  {
    if (plugin_foreach(NULL, flush_handlerton,
                          MYSQL_STORAGE_ENGINE_PLUGIN, 0))
      return TRUE;
  }
  else
  {
    if (db_type->state != SHOW_OPTION_YES ||
        (db_type->flush_logs && db_type->flush_logs(db_type)))
      return TRUE;
  }
  return FALSE;
}


/**
  @brief make canonical filename

  @param[in]  file     table handler
  @param[in]  path     original path
  @param[out] tmp_path buffer for canonized path

  @details Lower case db name and table name path parts for
           non file based tables when lower_case_table_names
           is 2 (store as is, compare in lower case).
           Filesystem path prefix (mysql_data_home or tmpdir)
           is left intact.

  @note tmp_path may be left intact if no conversion was
        performed.

  @retval canonized path

  @todo This may be done more efficiently when table path
        gets built. Convert this function to something like
        ASSERT_CANONICAL_FILENAME.
*/
const char *get_canonical_filename(handler *file, const char *path,
                                   char *tmp_path)
{
  uint i;
  if (lower_case_table_names != 2 || (file->ha_table_flags() & HA_FILE_BASED))
    return path;

  for (i= 0; i <= mysql_tmpdir_list.max; i++)
  {
    if (is_prefix(path, mysql_tmpdir_list.list[i]))
      return path;
  }

  /* Ensure that table handler get path in lower case */
  if (tmp_path != path)
    strmov(tmp_path, path);

  /*
    we only should turn into lowercase database/table part
    so start the process after homedirectory
  */
  my_casedn_str(files_charset_info, tmp_path + mysql_data_home_len);
  return tmp_path;
}


/** delete a table in the engine

  @note
  ENOENT and HA_ERR_NO_SUCH_TABLE are not considered errors.
  The .frm file will be deleted only if we return 0.
*/
int ha_delete_table(THD *thd, handlerton *table_type, const char *path,
                    const char *db, const char *alias, bool generate_warning)
{
  handler *file;
  char tmp_path[FN_REFLEN];
  int error;
  TABLE dummy_table;
  TABLE_SHARE dummy_share;
  DBUG_ENTER("ha_delete_table");

  /* table_type is NULL in ALTER TABLE when renaming only .frm files */
  if (table_type == NULL || table_type == view_pseudo_hton ||
      ! (file=get_new_handler((TABLE_SHARE*)0, thd->mem_root, table_type)))
    DBUG_RETURN(0);

  bzero((char*) &dummy_table, sizeof(dummy_table));
  bzero((char*) &dummy_share, sizeof(dummy_share));
  dummy_table.s= &dummy_share;

  path= get_canonical_filename(file, path, tmp_path);
  if ((error= file->ha_delete_table(path)))
  {
    /*
      it's not an error if the table doesn't exist in the engine.
      warn the user, but still report DROP being a success
    */
    bool intercept= error == ENOENT || error == HA_ERR_NO_SUCH_TABLE;

    if (!intercept || generate_warning)
    {
      /* Fill up strucutures that print_error may need */
      dummy_share.path.str= (char*) path;
      dummy_share.path.length= strlen(path);
      dummy_share.normalized_path= dummy_share.path;
      dummy_share.db.str= (char*) db;
      dummy_share.db.length= strlen(db);
      dummy_share.table_name.str= (char*) alias;
      dummy_share.table_name.length= strlen(alias);
      dummy_table.alias.set(alias, dummy_share.table_name.length,
                            table_alias_charset);
      file->change_table_ptr(&dummy_table, &dummy_share);
      file->print_error(error, MYF(intercept ? ME_JUST_WARNING : 0));
    }
    if (intercept)
      error= 0;
  }
  delete file;

  DBUG_RETURN(error);
}

/****************************************************************************
** General handler functions
****************************************************************************/
handler *handler::clone(const char *name, MEM_ROOT *mem_root)
{
  handler *new_handler= get_new_handler(table->s, mem_root, ht);

  if (!new_handler)
    return NULL;
  if (new_handler->set_ha_share_ref(ha_share))
    goto err;

  /*
    Allocate handler->ref here because otherwise ha_open will allocate it
    on this->table->mem_root and we will not be able to reclaim that memory 
    when the clone handler object is destroyed.
  */

  if (!(new_handler->ref= (uchar*) alloc_root(mem_root,
                                              ALIGN_SIZE(ref_length)*2)))
    goto err;

  /*
    TODO: Implement a more efficient way to have more than one index open for
    the same table instance. The ha_open call is not cachable for clone.

    This is not critical as the engines already have the table open
    and should be able to use the original instance of the table.
  */
  if (new_handler->ha_open(table, name, table->db_stat,
                           HA_OPEN_IGNORE_IF_LOCKED))
    goto err;

  return new_handler;

err:
  delete new_handler;
  return NULL;
}


double handler::keyread_time(uint index, uint ranges, ha_rows rows)
{
  /*
    It is assumed that we will read trough the whole key range and that all
    key blocks are half full (normally things are much better). It is also
    assumed that each time we read the next key from the index, the handler
    performs a random seek, thus the cost is proportional to the number of
    blocks read. This model does not take into account clustered indexes -
    engines that support that (e.g. InnoDB) may want to overwrite this method.
    The model counts in the time to read index entries from cache.
  */
  ulong len= table->key_info[index].key_length + ref_length;
  if (index == table->s->primary_key && table->file->primary_key_is_clustered())
    len= table->s->stored_rec_length;
  double keys_per_block= (stats.block_size/2.0/len+1);
  return (rows + keys_per_block-1)/ keys_per_block +
         len*rows/(stats.block_size+1)/TIME_FOR_COMPARE ;
}

void **handler::ha_data(THD *thd) const
{
  return thd_ha_data(thd, ht);
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


PSI_table_share *handler::ha_table_share_psi() const
{
  return table_share->m_psi;
}

/** @brief
  Open database-handler.

  IMPLEMENTATION
    Try O_RDONLY if cannot open as O_RDWR
    Don't wait for locks if not HA_OPEN_WAIT_IF_LOCKED is set
*/
int handler::ha_open(TABLE *table_arg, const char *name, int mode,
                     uint test_if_locked)
{
  int error;
  DBUG_ENTER("handler::ha_open");
  DBUG_PRINT("enter",
             ("name: %s  db_type: %d  db_stat: %d  mode: %d  lock_test: %d",
              name, ht->db_type, table_arg->db_stat, mode,
              test_if_locked));

  table= table_arg;
  DBUG_ASSERT(table->s == table_share);
  DBUG_ASSERT(m_lock_type == F_UNLCK);
  DBUG_PRINT("info", ("old m_lock_type: %d F_UNLCK %d", m_lock_type, F_UNLCK));
  DBUG_ASSERT(alloc_root_inited(&table->mem_root));

  if ((error=open(name,mode,test_if_locked)))
  {
    if ((error == EACCES || error == EROFS) && mode == O_RDWR &&
	(table->db_stat & HA_TRY_READ_ONLY))
    {
      table->db_stat|=HA_READ_ONLY;
      error=open(name,O_RDONLY,test_if_locked);
    }
  }
  if (error)
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

    if (table->s->db_options_in_use & HA_OPTION_READ_ONLY_DATA)
      table->db_stat|=HA_READ_ONLY;
    (void) extra(HA_EXTRA_NO_READCHECK);	// Not needed in SQL

    /* ref is already allocated for us if we're called from handler::clone() */
    if (!ref && !(ref= (uchar*) alloc_root(&table->mem_root, 
                                          ALIGN_SIZE(ref_length)*2)))
    {
      ha_close();
      error=HA_ERR_OUT_OF_MEM;
    }
    else
      dup_ref=ref+ALIGN_SIZE(ref_length);
    cached_table_flags= table_flags();
  }
  reset_statistics();
  internal_tmp_table= MY_TEST(test_if_locked & HA_OPEN_INTERNAL_TABLE);
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
    status_var_add(table->in_use->status_var.rows_tmp_read, rows_tmp_read);
  PSI_CALL_close_table(m_psi);
  m_psi= NULL; /* instrumentation handle, invalid after close_table() */

  /* Detach from ANALYZE tracker */
  tracker= NULL;
  
  DBUG_ASSERT(m_lock_type == F_UNLCK);
  DBUG_ASSERT(inited == NONE);
  DBUG_RETURN(close());
}


int handler::ha_rnd_next(uchar *buf)
{
  int result;
  DBUG_ENTER("handler::ha_rnd_next");
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(inited == RND);

  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_FETCH_ROW, MAX_KEY, 0,
    { result= rnd_next(buf); })
  if (!result)
  {
    update_rows_read();
    increment_statistics(&SSV::ha_read_rnd_next_count);
  }
  else if (result == HA_ERR_RECORD_DELETED)
    increment_statistics(&SSV::ha_read_rnd_deleted_count);
  else
    increment_statistics(&SSV::ha_read_rnd_next_count);

  table->status=result ? STATUS_NOT_FOUND: 0;
  DBUG_RETURN(result);
}

int handler::ha_rnd_pos(uchar *buf, uchar *pos)
{
  int result;
  DBUG_ENTER("handler::ha_rnd_pos");
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  /* TODO: Find out how to solve ha_rnd_pos when finding duplicate update. */
  /* DBUG_ASSERT(inited == RND); */

  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_FETCH_ROW, MAX_KEY, 0,
    { result= rnd_pos(buf, pos); })
  increment_statistics(&SSV::ha_read_rnd_count);
  if (!result)
    update_rows_read();
  table->status=result ? STATUS_NOT_FOUND: 0;
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

  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_FETCH_ROW, active_index, 0,
    { result= index_read_map(buf, key, keypart_map, find_flag); })
  increment_statistics(&SSV::ha_read_key_count);
  if (!result)
    update_index_statistics();
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
  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_FETCH_ROW, index, 0,
    { result= index_read_idx_map(buf, index, key, keypart_map, find_flag); })
  increment_statistics(&SSV::ha_read_key_count);
  if (!result)
  {
    update_rows_read();
    index_rows_read[index]++;
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

  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_FETCH_ROW, active_index, 0,
    { result= index_next(buf); })
  increment_statistics(&SSV::ha_read_next_count);
  if (!result)
    update_index_statistics();
  table->status=result ? STATUS_NOT_FOUND: 0;
  DBUG_RETURN(result);
}

int handler::ha_index_prev(uchar * buf)
{
  int result;
  DBUG_ENTER("handler::ha_index_prev");
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(inited==INDEX);

  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_FETCH_ROW, active_index, 0,
    { result= index_prev(buf); })
  increment_statistics(&SSV::ha_read_prev_count);
  if (!result)
    update_index_statistics();
  table->status=result ? STATUS_NOT_FOUND: 0;
  DBUG_RETURN(result);
}

int handler::ha_index_first(uchar * buf)
{
  int result;
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(inited==INDEX);

  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_FETCH_ROW, active_index, 0,
    { result= index_first(buf); })
  increment_statistics(&SSV::ha_read_first_count);
  if (!result)
    update_index_statistics();
  table->status=result ? STATUS_NOT_FOUND: 0;
  return result;
}

int handler::ha_index_last(uchar * buf)
{
  int result;
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(inited==INDEX);

  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_FETCH_ROW, active_index, 0,
    { result= index_last(buf); })
  increment_statistics(&SSV::ha_read_last_count);
  if (!result)
    update_index_statistics();
  table->status=result ? STATUS_NOT_FOUND: 0;
  return result;
}

int handler::ha_index_next_same(uchar *buf, const uchar *key, uint keylen)
{
  int result;
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ASSERT(inited==INDEX);

  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_FETCH_ROW, active_index, 0,
    { result= index_next_same(buf, key, keylen); })
  increment_statistics(&SSV::ha_read_next_count);
  if (!result)
    update_index_statistics();
  table->status=result ? STATUS_NOT_FOUND: 0;
  return result;
}

/* Initialize handler for random reading, with error handling */

int handler::ha_rnd_init_with_error(bool scan)
{
  int error;
  if (!(error= ha_rnd_init(scan)))
    return 0;
  table->file->print_error(error, MYF(0));
  return error;
}


/**
  Read first row (only) from a table.

  This is never called for InnoDB tables, as these table types
  has the HA_STATS_RECORDS_IS_EXACT set.
*/
int handler::read_first_row(uchar * buf, uint primary_key)
{
  register int error;
  DBUG_ENTER("handler::read_first_row");

  /*
    If there is very few deleted rows in the table, find the first row by
    scanning the table.
    TODO remove the test for HA_READ_ORDER
  */
  if (stats.deleted < 10 || primary_key >= MAX_KEY ||
      !(index_flags(primary_key, 0, 0) & HA_READ_ORDER))
  {
    if (!(error= ha_rnd_init(1)))
    {
      while ((error= ha_rnd_next(buf)) == HA_ERR_RECORD_DELETED)
        /* skip deleted row */;
      const int end_error= ha_rnd_end();
      if (!error)
        error= end_error;
    }
  }
  else
  {
    /* Find the first row through the primary key */
    if (!(error= ha_index_init(primary_key, 0)))
    {
      error= ha_index_first(buf);
      const int end_error= ha_index_end();
      if (!error)
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
    nr= (((nr+ variables->auto_increment_increment -
           variables->auto_increment_offset)) /
         (ulonglong) variables->auto_increment_increment);
    nr= (nr* (ulonglong) variables->auto_increment_increment +
         variables->auto_increment_offset);
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
  nr= (((nr - variables->auto_increment_offset)) /
       (ulonglong) variables->auto_increment_increment);
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
  enum enum_check_fields save_count_cuted_fields;
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
  save_count_cuted_fields= thd->count_cuted_fields;
  thd->count_cuted_fields= CHECK_FIELD_IGNORE;
  tmp= table->next_number_field->store((longlong) nr, TRUE);
  thd->count_cuted_fields= save_count_cuted_fields;

  if (unlikely(tmp))                            // Out of range value in store
  {
    /*
      It's better to return an error here than getting a confusing
      'duplicate key error' later.
    */
    result= HA_ERR_AUTOINC_ERANGE;
  }
  if (append)
  {
    auto_inc_interval_for_cur_row.replace(nr, nb_reserved_values,
                                          variables->auto_increment_increment);
    auto_inc_intervals_count++;
    /* Row-based replication does not need to store intervals in binlog */
    if (((WSREP(thd) && wsrep_emulate_bin_log ) || mysql_bin_log.is_open())
        && !thd->is_current_stmt_binlog_format_row())
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

    The handler is allowd to do changes to the bitmap after a index_init or
    rnd_init() call is made as after this, MySQL will not use the bitmap
    for any program logic checking.
*/
void handler::column_bitmaps_signal()
{
  DBUG_ENTER("column_bitmaps_signal");
  if (table)
    DBUG_PRINT("info", ("read_set: 0x%lx  write_set: 0x%lx",
                        (long) table->read_set, (long) table->write_set));
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

  (void) extra(HA_EXTRA_KEYREAD);
  table->mark_columns_used_by_index_no_reset(table->s->next_number_index,
                                        table->read_set);
  column_bitmaps_signal();

  if (ha_index_init(table->s->next_number_index, 1))
  {
    /* This should never happen, assert in debug, and fail in release build */
    DBUG_ASSERT(0);
    (void) extra(HA_EXTRA_NO_KEYREAD);
    *first_value= ULONGLONG_MAX;
    return;
  }

  if (table->s->next_number_keypart == 0)
  {						// Autoincrement at key-start
    error= ha_index_last(table->record[1]);
    /*
      MySQL implicitely assumes such method does locking (as MySQL decides to
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
                             make_prev_keypart_map(table->s->
                                                   next_number_keypart),
                             HA_READ_PREFIX_LAST);
    /*
      MySQL needs to call us for next row: assume we are inserting ("a",null)
      here, we return 3, and next this statement will want to insert
      ("b",null): there is no reason why ("b",3+1) would be the good row to
      insert: maybe it already exists, maybe 3+1 is too large...
    */
    *nb_reserved_values= 1;
  }

  if (error)
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
  (void) extra(HA_EXTRA_NO_KEYREAD);
  *first_value= nr;
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
*/

void print_keydup_error(TABLE *table, KEY *key, const char *msg, myf errflag)
{
  /* Write the duplicated key in the error message */
  if (table->dupp_key != -1 && table->err_message)
  {
    my_printf_error(ER_DUP_ENTRY, msg, errflag,
                    table->err_message, key->name);
    return;
  }
  char key_buff[MAX_KEY_LENGTH];
  String str(key_buff,sizeof(key_buff),system_charset_info);

  if (key == NULL)
  {
    /* Key is unknown */
    str.copy("", 0, system_charset_info);
    my_printf_error(ER_DUP_ENTRY, msg, errflag, str.c_ptr(), "*UNKNOWN*");
  }
  else
  {
    /* Table is opened and defined at this point */
    key_unpack(&str,table, key);
    uint max_length=MYSQL_ERRMSG_SIZE-(uint) strlen(msg);
    if (str.length() >= max_length)
    {
      str.length(max_length-4);
      str.append(STRING_WITH_LEN("..."));
    }
    my_printf_error(ER_DUP_ENTRY, msg, errflag, str.c_ptr_safe(), key->name);
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

  int textno= -1; // impossible value
  switch (error) {
  case EACCES:
    textno=ER_OPEN_AS_READONLY;
    break;
  case EAGAIN:
    textno=ER_FILE_USED;
    break;
  case ENOENT:
    textno=ER_FILE_NOT_FOUND;
    break;
  case ENOSPC:
  case HA_ERR_DISK_FULL:
    textno= ER_DISK_FULL;
    SET_FATAL_ERROR;                            // Ensure error is logged
    break;
  case HA_ERR_KEY_NOT_FOUND:
  case HA_ERR_NO_ACTIVE_RECORD:
  case HA_ERR_RECORD_DELETED:
  case HA_ERR_END_OF_FILE:
    /*
      This errors is not not normally fatal (for example for reads). However
      if you get it during an update or delete, then its fatal.
      As the user is calling print_error() (which is not done on read), we
      assume something when wrong with the update or delete.
    */
    SET_FATAL_ERROR;
    textno=ER_KEY_NOT_FOUND;
    break;
  case HA_ERR_ABORTED_BY_USER:
  {
    DBUG_ASSERT(table->in_use->killed);
    table->in_use->send_kill_message();
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
      if ((int) key_nr >= 0)
      {
        print_keydup_error(table,
                           key_nr == MAX_KEY ? NULL : &table->key_info[key_nr],
                           errflag);
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
  case HA_ERR_INDEX_FILE_FULL:
  {
    textno=ER_RECORD_FILE_FULL;
    /* Write the error message to error log */
    errflag|= ME_NOREFRESH;
    break;
  }
  case HA_ERR_LOCK_WAIT_TIMEOUT:
    textno=ER_LOCK_WAIT_TIMEOUT;
    break;
  case HA_ERR_LOCK_TABLE_FULL:
    textno=ER_LOCK_TABLE_FULL;
    break;
  case HA_ERR_LOCK_DEADLOCK:
    textno=ER_LOCK_DEADLOCK;
    /* cannot continue. the statement was already aborted in the engine */
    SET_FATAL_ERROR;
    break;
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
      ptr= table->key_info[key_nr].name;
    my_error(ER_DROP_INDEX_FK, errflag, ptr);
    DBUG_VOID_RETURN;
  }
  case HA_ERR_TABLE_NEEDS_UPGRADE:
    my_error(ER_TABLE_NEEDS_UPGRADE, errflag,
             "TABLE", table_share->table_name.str);
    break;
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
    my_error(textno, errflag, table->next_number_field->field_name,
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
  case HA_ERR_TABLE_IN_FK_CHECK:
    textno= ER_TABLE_IN_FK_CHECK;
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
        my_error(ER_GET_ERRNO, errflag, error, table_type());
      DBUG_VOID_RETURN;
    }
  }
  DBUG_ASSERT(textno > 0);
  if (fatal_error)
  {
    /* Ensure this becomes a true error */
    errflag&= ~(ME_JUST_WARNING | ME_JUST_INFO);
    if ((debug_assert_if_crashed_table ||
                      global_system_variables.log_warnings > 1))
    {
      /*
        Log error to log before we crash or if extended warnings are requested
      */
      errflag|= ME_NOREFRESH;
    }
  }    

  /* if we got an OS error from a file-based engine, specify a path of error */
  if (error < HA_ERR_FIRST && bas_ext()[0])
  {
    char buff[FN_REFLEN];
    strxnmov(buff, sizeof(buff),
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
    HA_ADMIN_NEEDS_UPGRADE   Table may have data requiring upgrade.
  @retval
    0                        No upgrade required.
*/

int handler::check_collation_compatibility()
{
  ulong mysql_version= table->s->mysql_version;

  if (mysql_version < 50124)
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
        if ((mysql_version < 50048 &&
             (cs_number == 11 || /* ascii_general_ci - bug #29499, bug #27562 */
              cs_number == 41 || /* latin7_general_ci - bug #29461 */
              cs_number == 42 || /* latin7_general_cs - bug #29461 */
              cs_number == 20 || /* latin7_estonian_cs - bug #29461 */
              cs_number == 21 || /* latin2_hungarian_ci - bug #29461 */
              cs_number == 22 || /* koi8u_general_ci - bug #29461 */
              cs_number == 23 || /* cp1251_ukrainian_ci - bug #29461 */
              cs_number == 26)) || /* cp1250_general_ci - bug #29461 */
             (mysql_version < 50124 &&
             (cs_number == 33 || /* utf8_general_ci - bug #27877 */
              cs_number == 35))) /* ucs2_general_ci - bug #27877 */
          return HA_ADMIN_NEEDS_UPGRADE;
      }
    }
  }

  return 0;
}


int handler::ha_check_for_upgrade(HA_CHECK_OPT *check_opt)
{
  int error;
  KEY *keyinfo, *keyend;
  KEY_PART_INFO *keypart, *keypartend;

  if (table->s->incompatible_version)
    return HA_ADMIN_NEEDS_ALTER;

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
  if (table->s->frm_version < FRM_VER_TRUE_VARCHAR)
    return HA_ADMIN_NEEDS_ALTER;

  if ((error= check_collation_compatibility()))
    return error;
    
  return check_for_upgrade(check_opt);
}


int handler::check_old_types()
{
  Field** field;

  if (!table->s->mysql_version)
  {
    /* check for bad DECIMAL field */
    for (field= table->field; (*field); field++)
    {
      if ((*field)->type() == MYSQL_TYPE_NEWDECIMAL)
      {
        return HA_ADMIN_NEEDS_ALTER;
      }
      if ((*field)->type() == MYSQL_TYPE_VAR_STRING)
      {
        return HA_ADMIN_NEEDS_ALTER;
      }
    }
  }
  return 0;
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
  */
  if (table->s->mysql_version == MYSQL_VERSION_ID)
    DBUG_RETURN(0);

  strxmov(path, table->s->normalized_path.str, reg_ext, NullS);

  if ((file= mysql_file_open(key_file_frm,
                             path, O_RDWR|O_BINARY, MYF(MY_WME))) >= 0)
  {
    uchar version[4];

    int4store(version, MYSQL_VERSION_ID);

    if ((result= mysql_file_pwrite(file, (uchar*) version, 4, 51L, MYF_RW)))
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
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  DBUG_ENTER("handler::get_dup_key");
  table->file->errkey  = (uint) -1;
  if (table->dupp_key != -1)
    DBUG_RETURN(table->dupp_key);
  if (error == HA_ERR_FOUND_DUPP_KEY || error == HA_ERR_FOREIGN_DUPLICATE_KEY ||
      error == HA_ERR_FOUND_DUPP_UNIQUE || error == HA_ERR_NULL_IN_SPATIAL ||
      error == HA_ERR_DROP_INDEX_FK)
    table->file->info(HA_STATUS_ERRKEY | HA_STATUS_NO_LOCK);
  DBUG_RETURN(table->file->errkey);
}


/**
  Delete all files with extension from bas_ext().

  @param name		Base name of table

  @note
    We assume that the handler may return more extensions than
    was actually used for the file.

  @retval
    0   If we successfully deleted at least one file from base_ext and
    didn't get any other errors than ENOENT
  @retval
    !0  Error
*/
int handler::delete_table(const char *name)
{
  int saved_error= 0;
  int error= 0;
  int enoent_or_zero;
  char buff[FN_REFLEN];

  if (ht->discover_table)
    enoent_or_zero= 0; // the table may not exist in the engine, it's ok
  else
    enoent_or_zero= ENOENT;  // the first file of bas_ext() *must* exist

  for (const char **ext=bas_ext(); *ext ; ext++)
  {
    fn_format(buff, name, "", *ext, MY_UNPACK_FILENAME|MY_APPEND_EXT);
    if (mysql_file_delete_with_symlink(key_file_misc, buff, MYF(0)))
    {
      if (my_errno != ENOENT)
      {
        /*
          If error on the first existing file, return the error.
          Otherwise delete as much as possible.
        */
        if (enoent_or_zero)
          return my_errno;
	saved_error= my_errno;
      }
    }
    else
      enoent_or_zero= 0;                        // No error for ENOENT
    error= enoent_or_zero;
  }
  return saved_error ? saved_error : error;
}


int handler::rename_table(const char * from, const char * to)
{
  int error= 0;
  const char **ext, **start_ext;
  start_ext= bas_ext();
  for (ext= start_ext; *ext ; ext++)
  {
    if (rename_file_ext(from, to, *ext))
    {
      if ((error=my_errno) != ENOENT)
	break;
      error= 0;
    }
  }
  if (error)
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
  Performs checks upon the table.

  @param thd                thread doing CHECK TABLE operation
  @param check_opt          options from the parser

  @retval
    HA_ADMIN_OK               Successful upgrade
  @retval
    HA_ADMIN_NEEDS_UPGRADE    Table has structures requiring upgrade
  @retval
    HA_ADMIN_NEEDS_ALTER      Table has structures requiring ALTER TABLE
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
    if ((error= check_old_types()))
      return error;
    error= ha_check_for_upgrade(check_opt);
    if (error && (error != HA_ADMIN_NEEDS_CHECK))
      return error;
    if (!error && (check_opt->sql_flags & TT_FOR_UPGRADE))
      return 0;
  }
  if ((error= check(thd, check_opt)))
    return error;
  /* Skip updating frm version if not main handler. */
  if (table->file != this)
    return error;
  return update_frm_version(table);
}

/**
  A helper function to mark a transaction read-write,
  if it is started.
*/

void handler::mark_trx_read_write_internal()
{
  Ha_trx_info *ha_info= &ha_thd()->ha_data[ht->slot].ha_info[0];
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
    DBUG_ASSERT(has_transactions());
    /*
      table_share can be NULL in ha_delete_table(). See implementation
      of standalone function ha_delete_table() in sql_base.cc.
    */
    if (table_share == NULL || table_share->tmp_table == NO_TMP_TABLE)
      ha_info->set_trx_read_write();
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

  if (result == HA_ADMIN_OK)
    result= update_frm_version(table);
  return result;
}


/**
  Bulk update row: public interface.

  @sa handler::bulk_update_row()
*/

int
handler::ha_bulk_update_row(const uchar *old_data, uchar *new_data,
                            uint *dup_key_found)
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

  return delete_all_rows();
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

  return truncate();
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

  return optimize(thd, check_opt);
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

  @sa handler::disable_indexes()
*/

int
handler::ha_disable_indexes(uint mode)
{
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  mark_trx_read_write();

  return disable_indexes(mode);
}


/**
  Enable indexes: public interface.

  @sa handler::enable_indexes()
*/

int
handler::ha_enable_indexes(uint mode)
{
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type != F_UNLCK);
  mark_trx_read_write();

  return enable_indexes(mode);
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
  DBUG_ENTER("check_if_supported_alter");

  HA_CREATE_INFO *create_info= ha_alter_info->create_info;

  Alter_inplace_info::HA_ALTER_FLAGS inplace_offline_operations=
    Alter_inplace_info::ALTER_COLUMN_EQUAL_PACK_LENGTH |
    Alter_inplace_info::ALTER_COLUMN_NAME |
    Alter_inplace_info::ALTER_COLUMN_DEFAULT |
    Alter_inplace_info::ALTER_COLUMN_OPTION |
    Alter_inplace_info::CHANGE_CREATE_OPTION |
    Alter_inplace_info::ALTER_PARTITIONED |
    Alter_inplace_info::ALTER_RENAME;

  /* Is there at least one operation that requires copy algorithm? */
  if (ha_alter_info->handler_flags & ~inplace_offline_operations)
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);

  /*
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
                                  HA_CREATE_USED_MAX_ROWS) ||
      (table->s->row_type != create_info->row_type))
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);

  uint table_changes= (ha_alter_info->handler_flags &
                       Alter_inplace_info::ALTER_COLUMN_EQUAL_PACK_LENGTH) ?
    IS_EQUAL_PACK_LENGTH : IS_EQUAL_YES;
  if (table->file->check_if_incompatible_data(create_info, table_changes)
      == COMPATIBLE_DATA_YES)
    DBUG_RETURN(HA_ALTER_INPLACE_NO_LOCK);

  DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
}


/*
   Default implementation to support in-place alter table
   and old online add/drop index API
*/

void handler::notify_table_changed()
{
  ha_create_partitioning_metadata(table->s->path.str, NULL, CHF_INDEX_FLAG);
}


void Alter_inplace_info::report_unsupported_error(const char *not_supported,
                                                  const char *try_instead)
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
  Delete table: public interface.

  @sa handler::delete_table()
*/

int
handler::ha_delete_table(const char *name)
{
  mark_trx_read_write();
  return delete_table(name);
}


/**
  Drop table in the engine: public interface.

  @sa handler::drop_table()

  The difference between this and delete_table() is that the table is open in
  drop_table().
*/

void
handler::ha_drop_table(const char *name)
{
  DBUG_ASSERT(m_lock_type == F_UNLCK);
  mark_trx_read_write();

  return drop_table(name);
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
  int error= create(name, form, info_arg);
  if (!error &&
      !(info_arg->options & (HA_LEX_CREATE_TMP_TABLE | HA_CREATE_TMP_ALTER)))
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
                                         int action_flag)
{
  /*
    Normally this is done when unlocked, but in fast_alter_partition_table,
    it is done on an already locked handler when preparing to alter/rename
    partitions.
  */
  DBUG_ASSERT(m_lock_type == F_UNLCK ||
              (!old_name && strcmp(name, table_share->path.str)));
  mark_trx_read_write();

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

  if ((thd->transaction.on= on))
  {
    /*
      Now all storage engines should have transaction handling enabled.
      But some may have it enabled all the time - "disabling" transactions
      is an optimization hint that storage engine is free to ignore.
      So, let's commit an open transaction (if any) now.
    */
    if (!(error= ha_commit_trans(thd, 0)))
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
  stat_info->delete_length=        stats.delete_length;
  stat_info->create_time=          stats.create_time;
  stat_info->update_time=          stats.update_time;
  stat_info->check_time=           stats.check_time;
  stat_info->check_sum=            0;
  if (table_flags() & (HA_HAS_OLD_CHECKSUM | HA_HAS_OLD_CHECKSUM))
    stat_info->check_sum= checksum();
  return;
}


/*
  Updates the global table stats with the TABLE this handler represents
*/

void handler::update_global_table_stats()
{
  TABLE_STATS * table_stats;

  status_var_add(table->in_use->status_var.rows_read, rows_read);
  DBUG_ASSERT(rows_tmp_read == 0);

  if (!table->in_use->userstat_running)
  {
    rows_read= rows_changed= 0;
    return;
  }

  if (rows_read + rows_changed == 0)
    return;                                     // Nothing to update.

  DBUG_ASSERT(table->s && table->s->table_cache_key.str);

  mysql_mutex_lock(&LOCK_global_table_stats);
  /* Gets the global table stats, creating one if necessary. */
  if (!(table_stats= (TABLE_STATS*)
        my_hash_search(&global_table_stats,
                    (uchar*) table->s->table_cache_key.str,
                    table->s->table_cache_key.length)))
  {
    if (!(table_stats = ((TABLE_STATS*)
                         my_malloc(sizeof(TABLE_STATS),
                                   MYF(MY_WME | MY_ZEROFILL)))))
    {
      /* Out of memory error already given */
      goto end;
    }
    memcpy(table_stats->table, table->s->table_cache_key.str,
           table->s->table_cache_key.length);
    table_stats->table_name_length= table->s->table_cache_key.length;
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
  table_stats->rows_read+=    rows_read;
  table_stats->rows_changed+= rows_changed;
  table_stats->rows_changed_x_indexes+= (rows_changed *
                                         (table->s->keys ? table->s->keys :
                                          1));
  rows_read= rows_changed= 0;
end:
  mysql_mutex_unlock(&LOCK_global_table_stats);
}


/*
  Updates the global index stats with this handler's accumulated index reads.
*/

void handler::update_global_index_stats()
{
  DBUG_ASSERT(table->s);

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
      uint key_length;
      KEY *key_info = &table->key_info[index];  // Rows were read using this

      DBUG_ASSERT(key_info->cache_name);
      if (!key_info->cache_name)
        continue;
      key_length= table->s->table_cache_key.length + key_info->name_length + 1;
      mysql_mutex_lock(&LOCK_global_index_stats);
      // Gets the global index stats, creating one if necessary.
      if (!(index_stats= (INDEX_STATS*) my_hash_search(&global_index_stats,
                                                    key_info->cache_name,
                                                    key_length)))
      {
        if (!(index_stats = ((INDEX_STATS*)
                             my_malloc(sizeof(INDEX_STATS),
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
      index_rows_read[index]= 0;
end:
      mysql_mutex_unlock(&LOCK_global_index_stats);
    }
  }
}


/****************************************************************************
** Some general functions that isn't in the handler class
****************************************************************************/

/**
  Initiates table-file and calls appropriate database-creator.

  @retval
   0  ok
  @retval
   1  error
*/
int ha_create_table(THD *thd, const char *path,
                    const char *db, const char *table_name,
                    HA_CREATE_INFO *create_info, LEX_CUSTRING *frm)
{
  int error= 1;
  TABLE table;
  char name_buff[FN_REFLEN];
  const char *name;
  TABLE_SHARE share;
  bool temp_table __attribute__((unused)) =
    create_info->options & (HA_LEX_CREATE_TMP_TABLE | HA_CREATE_TMP_ALTER);
                                 
  DBUG_ENTER("ha_create_table");

  init_tmp_table_share(thd, &share, db, 0, table_name, path);

  if (frm)
  {
    bool write_frm_now= !create_info->db_type->discover_table &&
                        !create_info->tmp_table();

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

  share.m_psi= PSI_CALL_get_table_share(temp_table, &share);

  if (open_table_from_share(thd, &share, "", 0, READ_ALL, 0, &table, true))
    goto err;

  update_create_info_from_table(create_info, &table);

  name= get_canonical_filename(table.file, share.path.str, name_buff);

  error= table.file->ha_create(name, &table, create_info);

  if (error)
  {
    if (!thd->is_error())
      my_error(ER_CANT_CREATE_TABLE, MYF(0), db, table_name, error);
    table.file->print_error(error, MYF(ME_JUST_WARNING));
    PSI_CALL_drop_table_share(temp_table, share.db.str, share.db.length,
                              share.table_name.str, share.table_name.length);
  }

  (void) closefrm(&table);
 
err:
  free_table_share(&share);
  DBUG_RETURN(error != 0);
}

void st_ha_check_opt::init()
{
  flags= sql_flags= 0;
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
int ha_init_key_cache(const char *name, KEY_CACHE *key_cache, void *unused
                      __attribute__((unused)))
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
  if (hton->state == SHOW_OPTION_YES && hton->discover_table)
  {
    share->db_plugin= plugin;
    int error= hton->discover_table(hton, thd, share);
    if (error != HA_ERR_NO_SUCH_TABLE)
    {
      if (error)
      {
        DBUG_ASSERT(share->error); // tdc_lock_share needs that
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
  if (ht->state != SHOW_OPTION_YES || !ht->discover_table_existence)
    return args->frm_exists;

  args->hton= ht;

  if (ht->discover_table_existence == ext_based_existence)
    return file_ext_exists(args->path, args->path_len,
                           ht->tablefile_extensions[0]);

  return ht->discover_table_existence(ht, args->db, args->table_name);
}

class Table_exists_error_handler : public Internal_error_handler
{
public:
  Table_exists_error_handler()
    : m_handled_errors(0), m_unhandled_errors(0)
  {}

  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sqlstate,
                        Sql_condition::enum_warning_level level,
                        const char* msg,
                        Sql_condition ** cond_hdl)
  {
    *cond_hdl= NULL;
    if (sql_errno == ER_NO_SUCH_TABLE ||
        sql_errno == ER_NO_SUCH_TABLE_IN_ENGINE ||
        sql_errno == ER_WRONG_OBJECT)
    {
      m_handled_errors++;
      return TRUE;
    }

    if (level == Sql_condition::WARN_LEVEL_ERROR)
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
bool ha_table_exists(THD *thd, const char *db, const char *table_name,
                     handlerton **hton)
{
  handlerton *dummy;
  DBUG_ENTER("ha_table_exists");

  if (hton)
    *hton= 0;
  else if (engines_with_discover)
    hton= &dummy;

  TDC_element *element= tdc_lock_share(thd, db, table_name);
  if (element && element != MY_ERRPTR)
  {
    if (hton)
      *hton= element->share->db_type();
    tdc_unlock_share(element);
    DBUG_RETURN(TRUE);
  }

  char path[FN_REFLEN + 1];
  size_t path_len = build_table_filename(path, sizeof(path) - 1,
                                         db, table_name, "", 0);
  st_discover_existence_args args= {path, path_len, db, table_name, 0, true};

  if (file_ext_exists(path, path_len, reg_ext))
  {
    bool exists= true;
    if (hton)
    {
      enum legacy_db_type db_type;
      if (dd_frm_type(thd, path, &db_type) != FRMTYPE_VIEW)
      {
        handlerton *ht= ha_resolve_by_legacy_type(thd, db_type);
        if ((*hton= ht))
          // verify that the table really exists
          exists= discover_existence(thd,
                             plugin_int_to_ref(hton2plugin[ht->slot]), &args);
      }
      else
        *hton= view_pseudo_hton;
    }
    DBUG_RETURN(exists);
  }

  args.frm_exists= false;
  if (plugin_foreach(thd, discover_existence, MYSQL_STORAGE_ENGINE_PLUGIN,
                     &args))
  {
    if (hton)
      *hton= args.hton;
    DBUG_RETURN(TRUE);
  }


  if (need_full_discover_for_existence)
  {
    TABLE_LIST table;
    uint flags = GTS_TABLE | GTS_VIEW;

    if (!hton)
      flags|= GTS_NOLOCK;

    Table_exists_error_handler no_such_table_handler;
    thd->push_internal_handler(&no_such_table_handler);
    table.init_one_table(db, strlen(db), table_name, strlen(table_name),
                         table_name, TL_READ);
    TABLE_SHARE *share= tdc_acquire_share(thd, &table, flags);
    thd->pop_internal_handler();

    if (hton && share)
    {
      *hton= share->db_type();
      tdc_release_share(share);
    }

    // the table doesn't exist if we've caught ER_NO_SUCH_TABLE and nothing else
    DBUG_RETURN(!no_such_table_handler.safely_trapped_errors());
  }

  DBUG_RETURN(FALSE);
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
  return my_strnncoll(cs, (uchar*)aa, strlen(aa), (uchar*)bb, strlen(bb));
}

static int cmp_table_names(LEX_STRING * const *a, LEX_STRING * const *b)
{
  return my_strnncoll(&my_charset_bin, (uchar*)((*a)->str), (*a)->length,
                                       (uchar*)((*b)->str), (*b)->length);
}

}

Discovered_table_list::Discovered_table_list(THD *thd_arg,
                 Dynamic_array<LEX_STRING*> *tables_arg,
                 const LEX_STRING *wild_arg) :
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
  if (wild && my_wildcmp(table_alias_charset, tname, tname + tlen, wild, wend,
                         wild_prefix, wild_one, wild_many))
      return 0;

  LEX_STRING *name= thd->make_lex_string(tname, tlen);
  if (!name || tables->append(name))
    return 1;
  return 0;
}

bool Discovered_table_list::add_file(const char *fname)
{
  bool is_temp= strncmp(fname, STRING_WITH_LEN(tmp_file_prefix)) == 0;

  if (is_temp && !with_temps)
    return 0;

  char tname[SAFE_NAME_LEN + 1];
  size_t tlen= filename_to_tablename(fname, tname, sizeof(tname), is_temp);
  return add_table(tname, tlen);
}


void Discovered_table_list::sort()
{
  tables->sort(cmp_table_names);
}

void Discovered_table_list::remove_duplicates()
{
  LEX_STRING **src= tables->front();
  LEX_STRING **dst= src;
  while (++dst <= tables->back())
  {
    LEX_STRING *s= *src, *d= *dst;
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
  LEX_STRING *db;
  MY_DIR *dirp;
  Discovered_table_list *result;
  uint possible_duplicates;
};

static my_bool discover_names(THD *thd, plugin_ref plugin,
                              void *arg)
{
  st_discover_names_args *args= (st_discover_names_args *)arg;
  handlerton *ht= plugin_hton(plugin);

  if (ht->state == SHOW_OPTION_YES && ht->discover_table_names)
  {
    uint old_elements= args->result->tables->elements();
    if (ht->discover_table_names(ht, args->db, args->dirp, args->result))
      return 1;

    /*
      hton_ext_based_table_discovery never discovers a table that has
      a corresponding .frm file; but custom engine discover methods might
    */
    if (ht->discover_table_names != hton_ext_based_table_discovery)
      args->possible_duplicates+= args->result->tables->elements() - old_elements;
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

int ha_discover_table_names(THD *thd, LEX_STRING *db, MY_DIR *dirp,
                            Discovered_table_list *result, bool reusable)
{
  int error;
  DBUG_ENTER("ha_discover_table_names");

  if (engines_with_discover_table_names == 0 && !reusable)
  {
    error= ext_table_discovery_simple(dirp, result);
    result->sort();
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
    result->sort();

    if (args.possible_duplicates > 0)
      result->remove_duplicates();
  }

  DBUG_RETURN(error);
}


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

int handler::compare_key2(key_range *range)
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
extern "C" enum icp_result handler_index_cond_check(void* h_arg)
{
  handler *h= (handler*)h_arg;
  THD *thd= h->table->in_use;
  enum icp_result res;

  enum thd_kill_levels abort_at= h->has_transactions() ?
    THD_ABORT_SOFTLY : THD_ABORT_ASAP;
  if (thd_kill_level(thd) > abort_at)
    return ICP_ABORTED_BY_USER;

  if (h->end_range && h->compare_key2(h->end_range) > 0)
    return ICP_OUT_OF_RANGE;
  h->increment_statistics(&SSV::ha_icp_attempts);
  if ((res= h->pushed_idx_cond->val_int()? ICP_MATCH : ICP_NO_MATCH) ==
      ICP_MATCH)
    h->increment_statistics(&SSV::ha_icp_match);
  return res;
}

int handler::index_read_idx_map(uchar * buf, uint index, const uchar * key,
                                key_part_map keypart_map,
                                enum ha_rkey_function find_flag)
{
  int error, UNINIT_VAR(error1);

  error= ha_index_init(index, 0);
  if (!error)
  {
    error= index_read_map(buf, key, keypart_map, find_flag);
    error1= ha_index_end();
  }
  return error ?  error : error1;
}


/**
  Returns a list of all known extensions.

    No mutexes, worst case race is a minor surplus memory allocation
    We have to recreate the extension map if mysqld is restarted (for example
    within libmysqld)

  @retval
    pointer		pointer to TYPELIB structure
*/
static my_bool exts_handlerton(THD *unused, plugin_ref plugin,
                               void *arg)
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


static bool stat_print(THD *thd, const char *type, uint type_len,
                       const char *file, uint file_len,
                       const char *status, uint status_len)
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
  if (hton->state == SHOW_OPTION_YES && hton->show_status &&
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
    if (db_type->state != SHOW_OPTION_YES)
    {
      const LEX_STRING *name= hton_name(db_type);
      result= stat_print(thd, name->str, name->length,
                         "", 0, "DISABLED", 8) ? 1 : 0;
    }
    else
    {
      result= db_type->show_status &&
              db_type->show_status(db_type, thd, stat_print, stat) ? 1 : 0;
    }
  }

  /*
    We also check thd->is_error() as Innodb may return 0 even if
    there was an error.
  */
  if (!result && !thd->is_error())
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

inline bool handler::check_table_binlog_row_based(bool binlog_row)
{
  if (unlikely((table->in_use->variables.sql_log_bin_off)))
    return 0;                            /* Called by partitioning engine */
  if (unlikely((!check_table_binlog_row_based_done)))
  {
    check_table_binlog_row_based_done= 1;
    check_table_binlog_row_based_result=
      check_table_binlog_row_based_internal(binlog_row);
  }
  return check_table_binlog_row_based_result;
}

bool handler::check_table_binlog_row_based_internal(bool binlog_row)
{
  THD *thd= table->in_use;

#ifdef WITH_WSREP
  /* only InnoDB tables will be replicated through binlog emulation */
  if (binlog_row &&
      ((WSREP_EMULATE_BINLOG(thd) &&
       table->file->partition_ht()->db_type != DB_TYPE_INNODB) ||
       (thd->wsrep_ignore_table == true)))
    return 0;
#endif

  return (table->s->cached_row_logging_check &&
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
          IF_WSREP(((WSREP_EMULATE_BINLOG(thd) &&
                     (thd->wsrep_exec_mode != REPL_RECV)) ||
                    ((WSREP(thd) ||
                      (thd->variables.option_bits & OPTION_BIN_LOG)) &&
                     mysql_bin_log.is_open())),
                   (thd->variables.option_bits & OPTION_BIN_LOG) &&
                   mysql_bin_log.is_open()));
}


/** @brief
   Write table maps for all (manually or automatically) locked tables
   to the binary log. Also, if binlog_annotate_row_events is ON,
   write Annotate_rows event before the first table map.

   SYNOPSIS
     write_locked_table_maps()
       thd     Pointer to THD structure

   DESCRIPTION
       This function will generate and write table maps for all tables
       that are locked by the thread 'thd'.

   RETURN VALUE
       0   All OK
       1   Failed to write all table maps

   SEE ALSO
       THD::lock
*/

static int write_locked_table_maps(THD *thd)
{
  DBUG_ENTER("write_locked_table_maps");
  DBUG_PRINT("enter", ("thd: 0x%lx  thd->lock: 0x%lx "
                       "thd->extra_lock: 0x%lx",
                       (long) thd, (long) thd->lock, (long) thd->extra_lock));

  DBUG_PRINT("debug", ("get_binlog_table_maps(): %d", thd->get_binlog_table_maps()));

  MYSQL_LOCK *locks[2];
  locks[0]= thd->extra_lock;
  locks[1]= thd->lock;
  my_bool with_annotate= thd->variables.binlog_annotate_row_events &&
    thd->query() && thd->query_length();

  for (uint i= 0 ; i < sizeof(locks)/sizeof(*locks) ; ++i )
  {
    MYSQL_LOCK const *const lock= locks[i];
    if (lock == NULL)
      continue;

    TABLE **const end_ptr= lock->table + lock->table_count;
    for (TABLE **table_ptr= lock->table ; 
         table_ptr != end_ptr ;
         ++table_ptr)
    {
      TABLE *const table= *table_ptr;
      DBUG_PRINT("info", ("Checking table %s", table->s->table_name.str));
      if (table->current_lock == F_WRLCK &&
          table->file->check_table_binlog_row_based(0))
      {
        /*
          We need to have a transactional behavior for SQLCOM_CREATE_TABLE
          (e.g. CREATE TABLE... SELECT * FROM TABLE) in order to keep a
          compatible behavior with the STMT based replication even when
          the table is not transactional. In other words, if the operation
          fails while executing the insert phase nothing is written to the
          binlog.

          Note that at this point, we check the type of a set of tables to
          create the table map events. In the function binlog_log_row(),
          which calls the current function, we check the type of the table
          of the current row.
        */
        bool const has_trans= thd->lex->sql_command == SQLCOM_CREATE_TABLE ||
          table->file->has_transactions();
        int const error= thd->binlog_write_table_map(table, has_trans,
                                                     &with_annotate);
        /*
          If an error occurs, it is the responsibility of the caller to
          roll back the transaction.
        */
        if (unlikely(error))
          DBUG_RETURN(1);
      }
    }
  }
  DBUG_RETURN(0);
}


typedef bool Log_func(THD*, TABLE*, bool, const uchar*, const uchar*);



static int binlog_log_row_internal(TABLE* table,
                                   const uchar *before_record,
                                   const uchar *after_record,
                                   Log_func *log_func)
{
  bool error= 0;
  THD *const thd= table->in_use;

  /*
    If there are no table maps written to the binary log, this is
    the first row handled in this statement. In that case, we need
    to write table maps for all locked tables to the binary log.
  */
  if (likely(!(error= ((thd->get_binlog_table_maps() == 0 &&
                        write_locked_table_maps(thd))))))
  {
    /*
      We need to have a transactional behavior for SQLCOM_CREATE_TABLE
      (i.e. CREATE TABLE... SELECT * FROM TABLE) in order to keep a
      compatible behavior with the STMT based replication even when
      the table is not transactional. In other words, if the operation
      fails while executing the insert phase nothing is written to the
      binlog.
    */
    bool const has_trans= thd->lex->sql_command == SQLCOM_CREATE_TABLE ||
      table->file->has_transactions();
    error= (*log_func)(thd, table, has_trans, before_record, after_record);
  }
  return error ? HA_ERR_RBR_LOGGING_FAILED : 0;
}

static inline int binlog_log_row(TABLE* table,
                                 const uchar *before_record,
                                 const uchar *after_record,
                                 Log_func *log_func)
{
  if (!table->file->check_table_binlog_row_based(1))
    return 0;
  return binlog_log_row_internal(table, before_record, after_record, log_func);
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

  ha_statistic_increment(&SSV::ha_external_lock_count);

  /*
    We cache the table flags if the locking succeeded. Otherwise, we
    keep them as they were when they were fetched in ha_open().
  */
  MYSQL_TABLE_LOCK_WAIT(m_psi, PSI_TABLE_EXTERNAL_LOCK, lock_type,
    { error= external_lock(thd, lock_type); })

  DBUG_EXECUTE_IF("external_lock_failure", error= HA_ERR_GENERIC;);

  if (error == 0)
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
  DBUG_ASSERT(table->key_read == 0);
  /* ensure that ha_index_end / ha_rnd_end has been called */
  DBUG_ASSERT(inited == NONE);
  /* reset the bitmaps to point to defaults */
  table->default_column_bitmaps();
  pushed_cond= NULL;
  tracker= NULL;
  mark_trx_read_write_done= check_table_binlog_row_based_done=
    check_table_binlog_row_based_result= 0;
  /* Reset information about pushed engine conditions */
  cancel_pushed_idx_cond();
  /* Reset information about pushed index conditions */
  DBUG_RETURN(reset());
}

/** @brief
    check whether inserted/updated records breaks the
    unique constraint on long columns.
    In the case of update we just need to check the specic key
    reason for that is consider case
    create table t1(a blob , b blob , x blob , y blob ,unique(a,b)
                                                    ,unique(c,d))
    and update statement like this
    update t1 set a=23+a; in this case if we try to scan for
    whole keys in table then index scan on c_d will return 0
    because data is same so in the case of update we take
    key as a parameter in normal insert key should be -1
   @returns 0 if no duplicate else returns error
  */
int check_duplicate_long_entries(TABLE *table, handler *h, uchar *new_rec,
                                  int key)
{
  Field *hash_field;
  int result;
  table->dupp_key= -1;
  for (uint i= 0; i < table->s->keys; i++)
  {
    if (key != -1)
      i= key;
    if (table->key_info[i].flags & HA_UNIQUE_HASH)
    {
      hash_field= table->key_info[i].key_part->field;
      DBUG_ASSERT(table->key_info[i].key_length == HA_HASH_KEY_LENGTH_WITH_NULL);
      uchar ptr[HA_HASH_KEY_LENGTH_WITH_NULL];

      if (hash_field->is_null())
        continue;

      key_copy(ptr, new_rec, &table->key_info[i],
                      table->key_info[i].key_length, false);

      if (!table->check_unique_buf)
        table->check_unique_buf= (uchar *)alloc_root(&table->mem_root,
                                        table->s->reclength*sizeof(uchar));

      result= h->ha_index_read_idx_map(table->check_unique_buf,
                                  i, ptr, HA_WHOLE_KEY, HA_READ_KEY_EXACT);
      if (!result)
      {
        Item_func_or_sum * temp= static_cast<Item_func_or_sum *>(hash_field->
                                                        vcol_info->expr_item);
        Item_args * t_item= static_cast<Item_args *>(temp);
        uint arg_count= t_item->argument_count();
        Item ** arguments= t_item->arguments();
        int diff= table->check_unique_buf-new_rec;
        Field * t_field;

        for (uint j=0; j < arg_count; j++)
        {
          t_field= static_cast<Item_field *>(arguments[j])->field;
          if(t_field->cmp_binary_offset(diff))
            continue;
        }
        table->dupp_key= i;
        if (!table->err_message)
        {
          table->err_message= (char *) alloc_root(&table->mem_root,
                                                 MAX_KEY_LENGTH);
        }
        StringBuffer<MAX_KEY_LENGTH> str;
        str.length(0);
        for(uint i= 0; i < arg_count; i++)
        {
          t_field= ((Item_field *)arguments[i])->field;
          if (str.length())
            str.append('-');
          field_unpack(&str, t_field, new_rec, 5,//since blob can be to long
                       false);
        }
        memcpy(table->err_message,str.ptr(),str.length());
        return HA_ERR_FOUND_DUPP_KEY;
      }
    }
    if (key != -1)
      break;
  }
  return 0;
}

/** @brief
    check whether updated records breaks the
    unique constraint on long columns.
   @returns 0 if no duplicate else returns error
  */
int check_duplicate_long_entries_update(TABLE *table, handler *h, uchar *new_rec)
{
  Field **f, *field;
  LEX_STRING *ls;
  int error= 0;
  bool is_update_handler_null= false;
  /*
     Here we are comparing whether new record and old record are same
     with respect to fields in hash_str
   */
  long reclength= table->record[1]-table->record[0];
  for (uint i= 0; i < table->s->keys; i++)
  {
    if (table->key_info[i].flags & HA_UNIQUE_HASH)
    {
      /*
         Currently mysql_update is pacthed so that it will automatically set the
         Update handler and then free it but ha_update_row is used in many function (
         like in case of reinsert) Instead of patching them all here we check is
         update_handler is null then set it  And then set it null again
       */
      if (!table->update_handler)
      {
        table->update_handler= table->file->clone(table->s->normalized_path.str,
                                                          &table->mem_root);
        table->update_handler->ha_external_lock(current_thd, F_RDLCK);
        is_update_handler_null= true;
      }
      ls= &table->key_info[i].key_part->field->vcol_info->expr_str;
      for (f= table->field; f && (field= *f); f++)
      {
        if ( find_field_index_in_hash(ls, field->field_name) != -1)
        {
          /* Compare fields if they are different then check for duplicates*/
          if(field->cmp_binary_offset(reclength))
          {
            if((error= check_duplicate_long_entries(table, table->update_handler,
                                                   new_rec, i)))
              goto exit;
            /*
              break beacuse check_duplicate_long_entries will
              take care of remaning fields
             */
            break;
          }
        }
      }
    }
  }
  exit:
  if (is_update_handler_null)
  {
    table->update_handler->ha_external_lock(current_thd, F_UNLCK);
    table->update_handler->ha_close();
    delete table->update_handler;
    table->update_handler= NULL;
  }
  return error;
}

int handler::ha_write_row(uchar *buf)
{
  int error;
  Log_func *log_func= Write_rows_log_event::binlog_row_logging_function;
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type == F_WRLCK);
  DBUG_ENTER("handler::ha_write_row");
  DEBUG_SYNC_C("ha_write_row_start");

  MYSQL_INSERT_ROW_START(table_share->db.str, table_share->table_name.str);
  mark_trx_read_write();
  increment_statistics(&SSV::ha_write_count);

  if ((error= check_duplicate_long_entries(table, table->file, buf, -1)))
    DBUG_RETURN(error);
  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_WRITE_ROW, MAX_KEY, 0,
                      { error= write_row(buf); })

  MYSQL_INSERT_ROW_DONE(error);
  if (likely(!error))
  {
    rows_changed++;
    error= binlog_log_row(table, 0, buf, log_func);
  }
  DEBUG_SYNC_C("ha_write_row_end");
  DBUG_RETURN(error);
}


int handler::ha_update_row(const uchar *old_data, uchar *new_data)
{
  int error;
  Log_func *log_func= Update_rows_log_event::binlog_row_logging_function;
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type == F_WRLCK);

  /*
    Some storage engines require that the new record is in record[0]
    (and the old record is in record[1]).
   */
  DBUG_ASSERT(new_data == table->record[0]);
  DBUG_ASSERT(old_data == table->record[1]);

  MYSQL_UPDATE_ROW_START(table_share->db.str, table_share->table_name.str);
  mark_trx_read_write();
  increment_statistics(&SSV::ha_update_count);

  if ((error= check_duplicate_long_entries_update(table, table->file, new_data)))
    return error;
  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_UPDATE_ROW, active_index, 0,
                      { error= update_row(old_data, new_data);})

  MYSQL_UPDATE_ROW_DONE(error);
  if (likely(!error))
  {
    rows_changed++;
    error= binlog_log_row(table, old_data, new_data, log_func);
  }
  return error;
}

int handler::ha_delete_row(const uchar *buf)
{
  int error;
  Log_func *log_func= Delete_rows_log_event::binlog_row_logging_function;
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              m_lock_type == F_WRLCK);
  /*
    Normally table->record[0] is used, but sometimes table->record[1] is used.
  */
  DBUG_ASSERT(buf == table->record[0] ||
              buf == table->record[1]);

  MYSQL_DELETE_ROW_START(table_share->db.str, table_share->table_name.str);
  mark_trx_read_write();
  increment_statistics(&SSV::ha_delete_count);

  TABLE_IO_WAIT(tracker, m_psi, PSI_TABLE_DELETE_ROW, active_index, 0,
    { error= delete_row(buf);})
  MYSQL_DELETE_ROW_DONE(error);
  if (likely(!error))
  {
    rows_changed++;
    error= binlog_log_row(table, buf, 0, log_func);
  }
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
  DBUG_ASSERT(ha_share && table_share);

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

/** @brief
  Dummy function which accept information about log files which is not need
  by handlers
*/
void signal_log_not_needed(struct handlerton, char *log_file)
{
  DBUG_ENTER("signal_log_not_needed");
  DBUG_PRINT("enter", ("logfile '%s'", log_file));
  DBUG_VOID_RETURN;
}

void handler::set_lock_type(enum thr_lock_type lock)
{
  table->reginfo.lock_type= lock;
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
        bf_thd->wsrep_exec_mode == TOTAL_ORDER)) {
    DBUG_RETURN(0);
  }

  THD_TRANS *trans= &victim_thd->transaction.all;
  Ha_trx_info *ha_info= trans->ha_list, *ha_info_next;

  for (; ha_info; ha_info= ha_info_next)
  {
    handlerton *hton= ha_info->ht();
    if (!hton->abort_transaction)
    {
      /* Skip warning for binlog SE */
      if (hton->db_type != DB_TYPE_BINLOG)
      {
        WSREP_WARN("Cannot abort transaction.");
      }
    }
    else
    {
      hton->abort_transaction(hton, bf_thd, victim_thd, signal);
    }
    ha_info_next= ha_info->next();
  }
  DBUG_RETURN(0);
}

void ha_fake_trx_id(THD *thd)
{
  DBUG_ENTER("ha_fake_trx_id");

  bool no_fake_trx_id= true;

  if (!WSREP(thd))
  {
    DBUG_VOID_RETURN;
  }

  /* Try statement transaction if standard one is not set. */
  THD_TRANS *trans= (thd->transaction.all.ha_list) ?  &thd->transaction.all :
    &thd->transaction.stmt;

  Ha_trx_info *ha_info= trans->ha_list, *ha_info_next;

  for (; ha_info; ha_info= ha_info_next)
  {
    handlerton *hton= ha_info->ht();
    if (hton->fake_trx_id)
    {
      hton->fake_trx_id(hton, thd);

      /* Got a fake trx id. */
      no_fake_trx_id= false;

      /*
        We need transaction ID from just one storage engine providing
        fake_trx_id (which will most likely be the case).
      */
      break;
    }
    ha_info_next= ha_info->next();
  }

  if (unlikely(no_fake_trx_id))
    WSREP_WARN("Cannot get fake transaction ID from storage engine.");

  DBUG_VOID_RETURN;
}
#endif /* WITH_WSREP */


#ifdef TRANS_LOG_MGM_EXAMPLE_CODE
/*
  Example of transaction log management functions based on assumption that logs
  placed into a directory
*/
#include <my_dir.h>
#include <my_sys.h>
int example_of_iterator_using_for_logs_cleanup(handlerton *hton)
{
  void *buffer;
  int res= 1;
  struct handler_iterator iterator;
  struct handler_log_file_data data;

  if (!hton->create_iterator)
    return 1; /* iterator creator is not supported */

  if ((*hton->create_iterator)(hton, HA_TRANSACTLOG_ITERATOR, &iterator) !=
      HA_ITERATOR_OK)
  {
    /* error during creation of log iterator or iterator is not supported */
    return 1;
  }
  while((*iterator.next)(&iterator, (void*)&data) == 0)
  {
    printf("%s\n", data.filename.str);
    if (data.status == HA_LOG_STATUS_FREE &&
        mysql_file_delete(INSTRUMENT_ME,
                          data.filename.str, MYF(MY_WME)))
      goto err;
  }
  res= 0;
err:
  (*iterator.destroy)(&iterator);
  return res;
}


/*
  Here we should get info from handler where it save logs but here is
  just example, so we use constant.
  IMHO FN_ROOTDIR ("/") is safe enough for example, because nobody has
  rights on it except root and it consist of directories only at lest for
  *nix (sorry, can't find windows-safe solution here, but it is only example).
*/
#define fl_dir FN_ROOTDIR


/** @brief
  Dummy function to return log status should be replaced by function which
  really detect the log status and check that the file is a log of this
  handler.
*/
enum log_status fl_get_log_status(char *log)
{
  MY_STAT stat_buff;
  if (mysql_file_stat(INSTRUMENT_ME, log, &stat_buff, MYF(0)))
    return HA_LOG_STATUS_INUSE;
  return HA_LOG_STATUS_NOSUCHLOG;
}


struct fl_buff
{
  LEX_STRING *names;
  enum log_status *statuses;
  uint32 entries;
  uint32 current;
};


int fl_log_iterator_next(struct handler_iterator *iterator,
                          void *iterator_object)
{
  struct fl_buff *buff= (struct fl_buff *)iterator->buffer;
  struct handler_log_file_data *data=
    (struct handler_log_file_data *) iterator_object;
  if (buff->current >= buff->entries)
    return 1;
  data->filename= buff->names[buff->current];
  data->status= buff->statuses[buff->current];
  buff->current++;
  return 0;
}


void fl_log_iterator_destroy(struct handler_iterator *iterator)
{
  my_free(iterator->buffer);
}


/** @brief
  returns buffer, to be assigned in handler_iterator struct
*/
enum handler_create_iterator_result
fl_log_iterator_buffer_init(struct handler_iterator *iterator)
{
  MY_DIR *dirp;
  struct fl_buff *buff;
  char *name_ptr;
  uchar *ptr;
  FILEINFO *file;
  uint32 i;

  /* to be able to make my_free without crash in case of error */
  iterator->buffer= 0;

  if (!(dirp = my_dir(fl_dir, MYF(MY_THREAD_SPECIFIC))))
  {
    return HA_ITERATOR_ERROR;
  }
  if ((ptr= (uchar*)my_malloc(ALIGN_SIZE(sizeof(fl_buff)) +
                             ((ALIGN_SIZE(sizeof(LEX_STRING)) +
                               sizeof(enum log_status) +
                               + FN_REFLEN + 1) *
                              (uint) dirp->number_off_files),
                             MYF(MY_THREAD_SPECIFIC))) == 0)
  {
    return HA_ITERATOR_ERROR;
  }
  buff= (struct fl_buff *)ptr;
  buff->entries= buff->current= 0;
  ptr= ptr + (ALIGN_SIZE(sizeof(fl_buff)));
  buff->names= (LEX_STRING*) (ptr);
  ptr= ptr + ((ALIGN_SIZE(sizeof(LEX_STRING)) *
               (uint) dirp->number_off_files));
  buff->statuses= (enum log_status *)(ptr);
  name_ptr= (char *)(ptr + (sizeof(enum log_status) *
                            (uint) dirp->number_off_files));
  for (i=0 ; i < (uint) dirp->number_off_files  ; i++)
  {
    enum log_status st;
    file= dirp->dir_entry + i;
    if ((file->name[0] == '.' &&
         ((file->name[1] == '.' && file->name[2] == '\0') ||
            file->name[1] == '\0')))
      continue;
    if ((st= fl_get_log_status(file->name)) == HA_LOG_STATUS_NOSUCHLOG)
      continue;
    name_ptr= strxnmov(buff->names[buff->entries].str= name_ptr,
                       FN_REFLEN, fl_dir, file->name, NullS);
    buff->names[buff->entries].length= (name_ptr -
                                        buff->names[buff->entries].str);
    buff->statuses[buff->entries]= st;
    buff->entries++;
  }

  iterator->buffer= buff;
  iterator->next= &fl_log_iterator_next;
  iterator->destroy= &fl_log_iterator_destroy;
  my_dirend(dirp);
  return HA_ITERATOR_OK;
}


/* An example of a iterator creator */
enum handler_create_iterator_result
fl_create_iterator(enum handler_iterator_type type,
                   struct handler_iterator *iterator)
{
  switch(type) {
  case HA_TRANSACTLOG_ITERATOR:
    return fl_log_iterator_buffer_init(iterator);
  default:
    return HA_ITERATOR_UNSUPPORTED;
  }
}
#endif /*TRANS_LOG_MGM_EXAMPLE_CODE*/


bool HA_CREATE_INFO::check_conflicting_charset_declarations(CHARSET_INFO *cs)
{
  if ((used_fields & HA_CREATE_USED_DEFAULT_CHARSET) &&
      /* DEFAULT vs explicit, or explicit vs DEFAULT */
      (((default_table_charset == NULL) != (cs == NULL)) ||
      /* Two different explicit character sets */
       (default_table_charset && cs &&
        !my_charset_same(default_table_charset, cs))))
  {
    my_error(ER_CONFLICTING_DECLARATIONS, MYF(0),
             "CHARACTER SET ", default_table_charset ?
                               default_table_charset->csname : "DEFAULT",
             "CHARACTER SET ", cs ? cs->csname : "DEFAULT");
    return true;
  }
  return false;
}

/* Remove all indexes for a given table from global index statistics */

static
int del_global_index_stats_for_table(THD *thd, uchar* cache_key, uint cache_key_length)
{
  int res = 0;
  DBUG_ENTER("del_global_index_stats_for_table");

  mysql_mutex_lock(&LOCK_global_index_stats);

  for (uint i= 0; i < global_index_stats.records;)
  {
    INDEX_STATS *index_stats =
      (INDEX_STATS*) my_hash_element(&global_index_stats, i);

    /* We search correct db\0table_name\0 string */
    if (index_stats &&
	index_stats->index_name_length >= cache_key_length &&
	!memcmp(index_stats->index, cache_key, cache_key_length))
    {
      res= my_hash_delete(&global_index_stats, (uchar*)index_stats);
      /*
          In our HASH implementation on deletion one elements
          is moved into a place where a deleted element was,
          and the last element is moved into the empty space.
          Thus we need to re-examine the current element, but
          we don't have to restart the search from the beginning.
      */
    }
    else
      i++;
  }

  mysql_mutex_unlock(&LOCK_global_index_stats);
  DBUG_RETURN(res);
}

/* Remove a table from global table statistics */

int del_global_table_stat(THD *thd, LEX_STRING *db, LEX_STRING *table)
{
  TABLE_STATS *table_stats;
  int res = 0;
  uchar *cache_key;
  uint cache_key_length;
  DBUG_ENTER("del_global_table_stat");

  cache_key_length= db->length + 1 + table->length + 1;

  if(!(cache_key= (uchar *)my_malloc(cache_key_length,
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
  uint key_length= table->s->table_cache_key.length + key_info->name_length + 1;
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
