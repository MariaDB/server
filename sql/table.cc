/* Copyright (c) 2000, 2017, Oracle and/or its affiliates.
   Copyright (c) 2008, 2022, MariaDB

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


/* Some general useful functions */

#include "mariadb.h"                 /* NO_EMBEDDED_ACCESS_CHECKS */
#include "sql_priv.h"
#include "table.h"
#include "key.h"                                // find_ref_key
#include "sql_table.h"                          // build_table_filename,
                                                // primary_key_name
#include "sql_parse.h"                          // free_items
#include "strfunc.h"                            // unhex_type2
#include "ha_partition.h"        // PART_EXT
                                 // mysql_unpack_partition,
                                 // fix_partition_func, partition_info
#include "sql_base.h"
#include "create_options.h"
#include "sql_trigger.h"
#include <m_ctype.h>
#include "my_md5.h"
#include "my_bit.h"
#include "sql_select.h"
#include "sql_derived.h"
#include "sql_statistics.h"
#include "discover.h"
#include "mdl.h"                 // MDL_wait_for_graph_visitor
#include "sql_view.h"
#include "rpl_filter.h"
#include "sql_cte.h"
#include "ha_sequence.h"
#include "sql_show.h"
#include "opt_trace.h"
#include "sql_db.h"              // get_default_db_collation
#include "sql_update.h"          // class Sql_cmd_update
#include "sql_delete.h"          // class Sql_cmd_delete


#ifdef WITH_WSREP
#include "wsrep_schema.h"
#endif
#include "log_event.h"           // MAX_TABLE_MAP_ID
#include "sql_class.h"

/* For MySQL 5.7 virtual fields */
#define MYSQL57_GENERATED_FIELD 128
#define MYSQL57_GCOL_HEADER_SIZE 4

bool TABLE::init_expr_arena(MEM_ROOT *mem_root)
{
  /*
    We need to use CONVENTIONAL_EXECUTION here to ensure that
    any new items created by fix_fields() are not reverted.
  */
  expr_arena= new (alloc_root(mem_root, sizeof(Query_arena)))
                Query_arena(mem_root, Query_arena::STMT_CONVENTIONAL_EXECUTION);
  return expr_arena == NULL;
}

struct extra2_fields
{
  LEX_CUSTRING version;
  LEX_CUSTRING options;
  Lex_ident engine;
  LEX_CUSTRING gis;
  LEX_CUSTRING field_flags;
  LEX_CUSTRING system_period;
  LEX_CUSTRING application_period;
  LEX_CUSTRING field_data_type_info;
  LEX_CUSTRING without_overlaps;
  LEX_CUSTRING index_flags;
  void reset()
  { bzero((void*)this, sizeof(*this)); }
};

static Virtual_column_info * unpack_vcol_info_from_frm(THD *,
              TABLE *, String *, Virtual_column_info **, bool *);

/* INFORMATION_SCHEMA name */
Lex_ident_db INFORMATION_SCHEMA_NAME= {STRING_WITH_LEN("information_schema")};

/* PERFORMANCE_SCHEMA name */
Lex_ident_db PERFORMANCE_SCHEMA_DB_NAME= {STRING_WITH_LEN("performance_schema")};

/* MYSQL_SCHEMA name */
Lex_ident_db MYSQL_SCHEMA_NAME= {STRING_WITH_LEN("mysql")};

/* GENERAL_LOG name */
Lex_ident_table GENERAL_LOG_NAME= {STRING_WITH_LEN("general_log")};

/* SLOW_LOG name */
Lex_ident_table SLOW_LOG_NAME= {STRING_WITH_LEN("slow_log")};

Lex_ident_table TRANSACTION_REG_NAME= {STRING_WITH_LEN("transaction_registry")};
Lex_ident_table MYSQL_PROC_NAME= {STRING_WITH_LEN("proc")};

/* 
  Keyword added as a prefix when parsing the defining expression for a
  virtual column read from the column definition saved in the frm file
*/
static LEX_CSTRING parse_vcol_keyword= { STRING_WITH_LEN("PARSE_VCOL_EXPR ") };

static std::atomic<ulonglong> last_table_id;

	/* Functions defined in this file */

static bool fix_type_pointers(const char ***typelib_value_names,
                              uint **typelib_value_lengths,
                              TYPELIB *point_to_type, uint types,
                              char *names, size_t names_length);

static field_index_t find_field(Field **fields, uchar *record, uint start,
                                uint length);

inline bool is_system_table_name(const char *name, size_t length);

/**************************************************************************
  Object_creation_ctx implementation.
**************************************************************************/

Object_creation_ctx *Object_creation_ctx::set_n_backup(THD *thd)
{
  Object_creation_ctx *backup_ctx;
  DBUG_ENTER("Object_creation_ctx::set_n_backup");

  backup_ctx= create_backup_ctx(thd);
  change_env(thd);

  DBUG_RETURN(backup_ctx);
}

void Object_creation_ctx::restore_env(THD *thd, Object_creation_ctx *backup_ctx)
{
  if (!backup_ctx)
    return;

  backup_ctx->change_env(thd);

  delete backup_ctx;
}

/**************************************************************************
  Default_object_creation_ctx implementation.
**************************************************************************/

Default_object_creation_ctx::Default_object_creation_ctx(THD *thd)
  : m_client_cs(thd->variables.character_set_client),
    m_connection_cl(thd->variables.collation_connection)
{ }

Default_object_creation_ctx::Default_object_creation_ctx(
  CHARSET_INFO *client_cs, CHARSET_INFO *connection_cl)
  : m_client_cs(client_cs),
    m_connection_cl(connection_cl)
{ }

Object_creation_ctx *
Default_object_creation_ctx::create_backup_ctx(THD *thd) const
{
  return new Default_object_creation_ctx(thd);
}

void Default_object_creation_ctx::change_env(THD *thd) const
{
  thd->update_charset(m_client_cs, m_connection_cl);
}

/**************************************************************************
  View_creation_ctx implementation.
**************************************************************************/

View_creation_ctx *View_creation_ctx::create(THD *thd)
{
  View_creation_ctx *ctx= new (thd->mem_root) View_creation_ctx(thd);

  return ctx;
}

/*************************************************************************/

View_creation_ctx * View_creation_ctx::create(THD *thd,
                                              TABLE_LIST *view)
{
  View_creation_ctx *ctx= new (thd->mem_root) View_creation_ctx(thd);

  /* Throw a warning if there is NULL cs name. */

  if (!view->view_client_cs_name.str ||
      !view->view_connection_cl_name.str)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_VIEW_NO_CREATION_CTX,
                        ER_THD(thd, ER_VIEW_NO_CREATION_CTX),
                        view->db.str,
                        view->table_name.str);

    ctx->m_client_cs= system_charset_info;
    ctx->m_connection_cl= system_charset_info;

    return ctx;
  }

  /* Resolve cs names. Throw a warning if there is unknown cs name. */

  bool invalid_creation_ctx;
  myf utf8_flag= thd->get_utf8_flag();
  invalid_creation_ctx= resolve_charset(view->view_client_cs_name.str,
                                        system_charset_info,
                                        &ctx->m_client_cs, MYF(utf8_flag));

  invalid_creation_ctx= resolve_collation(view->view_connection_cl_name.str,
                                          system_charset_info,
                                          &ctx->m_connection_cl, MYF(utf8_flag)) ||
                        invalid_creation_ctx;

  if (invalid_creation_ctx)
  {
    sql_print_warning("View '%s'.'%s': there is unknown charset/collation "
                      "names (client: '%s'; connection: '%s').",
                      view->db.str,
                      view->table_name.str,
                      (const char *) view->view_client_cs_name.str,
                      (const char *) view->view_connection_cl_name.str);

    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_VIEW_INVALID_CREATION_CTX,
                        ER_THD(thd, ER_VIEW_INVALID_CREATION_CTX),
                        view->db.str,
                        view->table_name.str);
  }

  return ctx;
}

/*************************************************************************/

/* Get column name from column hash */

static const uchar *get_field_name(const void *buff_, size_t *length, my_bool)
{
  auto buff= static_cast<const Field *const *>(buff_);
  *length= (*buff)->field_name.length;
  return reinterpret_cast<const uchar *>((*buff)->field_name.str);
}


/*
  Returns pointer to '.frm' extension of the file name.

  SYNOPSIS
    fn_frm_ext()
    name       file name

  DESCRIPTION
    Checks file name part starting with the rightmost '.' character,
    and returns it if it is equal to '.frm'. 

  RETURN VALUES
    Pointer to the '.frm' extension or NULL if not a .frm file
*/

const char *fn_frm_ext(const char *name)
{
  const char *res= strrchr(name, '.');
  if (res && !strcmp(res, reg_ext))
    return res;
  return 0;
}


TABLE_CATEGORY get_table_category(const Lex_ident_db &db,
                                  const Lex_ident_table &name)
{
  if (is_infoschema_db(&db))
    return TABLE_CATEGORY_INFORMATION;

  if (is_perfschema_db(&db))
    return TABLE_CATEGORY_PERFORMANCE;

  if (db.streq(MYSQL_SCHEMA_NAME))
  {
    if (is_system_table_name(name.str, name.length))
      return TABLE_CATEGORY_SYSTEM;

    if (name.streq(GENERAL_LOG_NAME))
      return TABLE_CATEGORY_LOG;

    if (name.streq(SLOW_LOG_NAME))
      return TABLE_CATEGORY_LOG;

    if (name.streq(TRANSACTION_REG_NAME))
      return TABLE_CATEGORY_LOG;
  }

#ifdef WITH_WSREP
  if (db.streq(WSREP_LEX_SCHEMA))
  {
    if(name.streq(WSREP_LEX_STREAMING))
      return TABLE_CATEGORY_INFORMATION;
    if (name.streq(WSREP_LEX_CLUSTER))
      return TABLE_CATEGORY_INFORMATION;
    if (name.streq(WSREP_LEX_MEMBERS))
      return TABLE_CATEGORY_INFORMATION;
    if (name.streq(WSREP_LEX_ALLOWLIST))
      return TABLE_CATEGORY_INFORMATION;
  }
#endif /* WITH_WSREP */

  return TABLE_CATEGORY_USER;
}


/*
  Allocate and setup a TABLE_SHARE structure

  SYNOPSIS
    alloc_table_share()
    db                  Database name
    table_name          Table name
    key			Table cache key (db \0 table_name \0...)
    key_length		Length of key

  RETURN
    0  Error (out of memory)
    #  Share
*/

TABLE_SHARE *alloc_table_share(const char *db, const char *table_name,
                               const char *key, uint key_length)
{
  MEM_ROOT mem_root;
  TABLE_SHARE *share;
  char *key_buff, *path_buff;
  char path[FN_REFLEN];
  uint path_length;
  DBUG_ENTER("alloc_table_share");
  DBUG_PRINT("enter", ("table: '%s'.'%s'", db, table_name));

  path_length= build_table_filename(path, sizeof(path) - 1,
                                    db, table_name, "", 0);
  init_sql_alloc(key_memory_table_share, &mem_root, TABLE_ALLOC_BLOCK_SIZE,
                 TABLE_PREALLOC_BLOCK_SIZE, MYF(0));
  if (multi_alloc_root(&mem_root,
                       &share, sizeof(*share),
                       &key_buff, key_length,
                       &path_buff, path_length + 1,
                       NULL))
  {
    bzero((char*) share, sizeof(*share));

    share->set_table_cache_key(key_buff, key, key_length);

    share->path.str= path_buff;
    share->path.length= path_length;
    strmov(path_buff, path);
    share->normalized_path.str=    share->path.str;
    share->normalized_path.length= path_length;
    share->table_category= get_table_category(Lex_ident_db(share->db),
                                           Lex_ident_table(share->table_name));
    share->open_errno= ENOENT;
    /* The following will be updated in open_table_from_share */
    share->can_do_row_logging= 1;
    if (share->table_category == TABLE_CATEGORY_LOG)
      share->no_replicate= 1;
    if (key_length > 6 &&
        table_alias_charset->strnncoll(key, 6, "mysql", 6) == 0)
      share->not_usable_by_query_cache= 1;

    memcpy((char*) &share->mem_root, (char*) &mem_root, sizeof(mem_root));
    mysql_mutex_init(key_TABLE_SHARE_LOCK_share,
                     &share->LOCK_share, MY_MUTEX_INIT_SLOW);
    mysql_mutex_init(key_TABLE_SHARE_LOCK_ha_data,
                     &share->LOCK_ha_data, MY_MUTEX_INIT_FAST);
    mysql_mutex_init(key_TABLE_SHARE_LOCK_statistics,
                     &share->LOCK_statistics, MY_MUTEX_INIT_SLOW);

    DBUG_EXECUTE_IF("simulate_big_table_id",
                    if (last_table_id < UINT_MAX32)
                      last_table_id= UINT_MAX32-1;);
    /*
      Replication is using 6 bytes as table_map_id. Ensure that
      the 6 lowest bytes are not 0.
      We also have to ensure that we do not use the special value
      UINT_MAX32 as this is used to mark a dummy event row event. See
      comments in Rows_log_event::Rows_log_event().
    */
    do
    {
      share->table_map_id=
        last_table_id.fetch_add(1, std::memory_order_relaxed);
    } while (unlikely((share->table_map_id & MAX_TABLE_MAP_ID) == 0) ||
             unlikely((share->table_map_id & MAX_TABLE_MAP_ID) == UINT_MAX32));
  }
  DBUG_RETURN(share);
}


/*
  Initialize share for temporary tables

  SYNOPSIS
    init_tmp_table_share()
    thd         thread handle
    share	Share to fill
    key		Table_cache_key, as generated from tdc_create_key.
		must start with db name.
    key_length	Length of key
    table_name	Table name
    path	Path to file (possible in lower case) without .frm

  NOTES
    This is different from alloc_table_share() because temporary tables
    don't have to be shared between threads or put into the table def
    cache, so we can do some things notable simpler and faster

    If table is not put in thd->temporary_tables (happens only when
    one uses OPEN TEMPORARY) then one can specify 'db' as key and
    use key_length= 0 as neither table_cache_key or key_length will be used).
*/

void init_tmp_table_share(THD *thd, TABLE_SHARE *share, const char *key,
                          uint key_length, const char *table_name,
                          const char *path)
{
  DBUG_ENTER("init_tmp_table_share");
  DBUG_PRINT("enter", ("table: '%s'.'%s'", key, table_name));

  bzero((char*) share, sizeof(*share));
  /*
    This can't be MY_THREAD_SPECIFIC for slaves as they are freed
    during cleanup() from Relay_log_info::close_temporary_tables().
    We can also not use pre-alloc here, as internal temporary tables
    are not freeing table->share->mem_root
  */
  init_sql_alloc(key_memory_table_share, &share->mem_root,
                 TABLE_PREALLOC_BLOCK_SIZE, 0,
                 MYF(thd->slave_thread ? 0 : MY_THREAD_SPECIFIC));
  share->table_category=         TABLE_CATEGORY_TEMPORARY;
  share->tmp_table=              INTERNAL_TMP_TABLE;
  share->db.str=                 (char*) key;
  share->db.length=		 strlen(key);
  share->table_cache_key.str=    (char*) key;
  share->table_cache_key.length= key_length;
  share->table_name.str=         (char*) table_name;
  share->table_name.length=      strlen(table_name);
  share->path.str=               (char*) path;
  share->normalized_path.str=    (char*) path;
  share->path.length= share->normalized_path.length= strlen(path);
  share->frm_version= 		 FRM_VER_CURRENT;
  share->not_usable_by_query_cache= 1;
  share->can_do_row_logging= 0;           // No row logging

  /*
    table_map_id is also used for MERGE tables to suppress repeated
    compatibility checks.
  */
  share->table_map_id= (ulonglong) thd->query_id;
  DBUG_VOID_RETURN;
}


/**
  Release resources (plugins) used by the share and free its memory.
  TABLE_SHARE is self-contained -- it's stored in its own MEM_ROOT.
  Free this MEM_ROOT.
*/

void TABLE_SHARE::destroy()
{
  uint idx;
  KEY *info_it;
  DBUG_ENTER("TABLE_SHARE::destroy");
  DBUG_PRINT("info", ("db: %s table: %s", db.str, table_name.str));

  if (ha_share)
  {
    delete ha_share;
    ha_share= NULL;                             // Safety
  }

  if (stats_cb)
  {
    stats_cb->usage_count--;
    delete stats_cb;
  }
  delete sequence;

  /* The mutexes are initialized only for shares that are part of the TDC */
  if (tmp_table == NO_TMP_TABLE)
  {
    mysql_mutex_destroy(&LOCK_share);
    mysql_mutex_destroy(&LOCK_ha_data);
    mysql_mutex_destroy(&LOCK_statistics);
  }
  my_hash_free(&name_hash);

  plugin_unlock(NULL, db_plugin);
  db_plugin= NULL;

  /* Release fulltext parsers */
  info_it= key_info;
  for (idx= keys; idx; idx--, info_it++)
  {
    if (info_it->flags & HA_USES_PARSER)
    {
      plugin_unlock(NULL, info_it->parser);
      info_it->flags= 0;
    }
  }

#ifdef HAVE_REPLICATION
  if (online_alter_binlog)
  {
    online_alter_binlog->release();
    online_alter_binlog= NULL;
  }
#endif

#ifdef WITH_PARTITION_STORAGE_ENGINE
  plugin_unlock(NULL, default_part_plugin);
#endif /* WITH_PARTITION_STORAGE_ENGINE */

  PSI_CALL_release_table_share(m_psi);

  /*
    Make a copy since the share is allocated in its own root,
    and free_root() updates its argument after freeing the memory.
  */
  MEM_ROOT own_root= mem_root;
  free_root(&own_root, MYF(0));
  DBUG_VOID_RETURN;
}

/*
  Free table share and memory used by it

  SYNOPSIS
    free_table_share()
    share		Table share
*/

void free_table_share(TABLE_SHARE *share)
{
  DBUG_ENTER("free_table_share");
  DBUG_PRINT("enter", ("table: %s.%s", share->db.str, share->table_name.str));
  share->destroy();
  DBUG_VOID_RETURN;
}


/**
  Return TRUE if a table name matches one of the system table names.
  Currently these are:

  help_category, help_keyword, help_relation, help_topic,
  proc, event
  time_zone, time_zone_leap_second, time_zone_name, time_zone_transition,
  time_zone_transition_type

  This function trades accuracy for speed, so may return false
  positives. Presumably mysql.* database is for internal purposes only
  and should not contain user tables.
*/

inline bool is_system_table_name(const char *name, size_t length)
{
  CHARSET_INFO *ci= system_charset_info;

  return (
          /* mysql.proc table */
          (length == 4 &&
           my_tolower(ci, name[0]) == 'p' && 
           my_tolower(ci, name[1]) == 'r' &&
           my_tolower(ci, name[2]) == 'o' &&
           my_tolower(ci, name[3]) == 'c') ||

          (length > 4 &&
           (
            /* one of mysql.help* tables */
            (my_tolower(ci, name[0]) == 'h' &&
             my_tolower(ci, name[1]) == 'e' &&
             my_tolower(ci, name[2]) == 'l' &&
             my_tolower(ci, name[3]) == 'p') ||

            /* one of mysql.time_zone* tables */
            (my_tolower(ci, name[0]) == 't' &&
             my_tolower(ci, name[1]) == 'i' &&
             my_tolower(ci, name[2]) == 'm' &&
             my_tolower(ci, name[3]) == 'e') ||

            /* one of mysql.*_stat tables, but not mysql.innodb* tables*/
            ((my_tolower(ci, name[length-5]) == 's' &&
              my_tolower(ci, name[length-4]) == 't' &&
              my_tolower(ci, name[length-3]) == 'a' &&
              my_tolower(ci, name[length-2]) == 't' &&
              my_tolower(ci, name[length-1]) == 's') &&
             !(my_tolower(ci, name[0]) == 'i' &&
               my_tolower(ci, name[1]) == 'n' &&
               my_tolower(ci, name[2]) == 'n' &&
               my_tolower(ci, name[3]) == 'o')) ||

            /* mysql.event table */
            (my_tolower(ci, name[0]) == 'e' &&
             my_tolower(ci, name[1]) == 'v' &&
             my_tolower(ci, name[2]) == 'e' &&
             my_tolower(ci, name[3]) == 'n' &&
             my_tolower(ci, name[4]) == 't')
            )
           )
         );
}


/*
  Read table definition from a binary / text based .frm file
  
  SYNOPSIS
  open_table_def()
  thd		  Thread handler
  share		Fill this with table definition
  flags	  Bit mask of the following flags: OPEN_VIEW

  NOTES
    This function is called when the table definition is not cached in
    table definition cache
    The data is returned in 'share', which is allocated by
    alloc_table_share().. The code assumes that share is initialized.
*/

enum open_frm_error open_table_def(THD *thd, TABLE_SHARE *share, uint flags)
{
  bool error_given= false;
  File file;
  uchar *buf;
  uchar head[FRM_HEADER_SIZE];
  char	path[FN_REFLEN];
  size_t frmlen, read_length;
  uint length;
  DBUG_ENTER("open_table_def");
  DBUG_PRINT("enter", ("table: '%s'.'%s'  path: '%s'", share->db.str,
                       share->table_name.str, share->normalized_path.str));

  share->error= OPEN_FRM_OPEN_ERROR;

  length=(uint) (strxmov(path, share->normalized_path.str, reg_ext, NullS) -
                 path);
  if (flags & GTS_FORCE_DISCOVERY)
  {
    const char *path2= share->normalized_path.str;
    DBUG_ASSERT(flags & GTS_TABLE);
    DBUG_ASSERT(flags & GTS_USE_DISCOVERY);
    /* Delete .frm and .par files */
    mysql_file_delete_with_symlink(key_file_frm, path2, reg_ext, MYF(0));
    mysql_file_delete_with_symlink(key_file_partition_ddl_log, path2, PAR_EXT,
                                   MYF(0));
    file= -1;
  }
  else
    file= mysql_file_open(key_file_frm, path, O_RDONLY | O_SHARE, MYF(0));

  if (file < 0)
  {
    if ((flags & GTS_TABLE) && (flags & GTS_USE_DISCOVERY))
    {
      ha_discover_table(thd, share);
      error_given= true;
    }
    goto err_not_open;
  }

  if (mysql_file_read(file, head, sizeof(head), MYF(MY_NABP)))
  {
    share->error = my_errno == HA_ERR_FILE_TOO_SHORT
                      ? OPEN_FRM_CORRUPTED : OPEN_FRM_READ_ERROR;
    goto err;
  }

  if (memcmp(head, STRING_WITH_LEN("TYPE=VIEW\n")) == 0)
  {
    share->is_view= 1;
    if (flags & GTS_VIEW)
    {
      LEX_CSTRING pathstr= { path, length };
      /*
        Create view file parser and hold it in TABLE_SHARE member
        view_def.
      */
      share->view_def= sql_parse_prepare(&pathstr, &share->mem_root, true);
      if (!share->view_def)
        share->error= OPEN_FRM_ERROR_ALREADY_ISSUED;
      else
      {
        share->error= OPEN_FRM_OK;
        if (mariadb_view_version_get(share))
          share->error= OPEN_FRM_ERROR_ALREADY_ISSUED;
      }
    }
    else
      share->error= OPEN_FRM_NOT_A_TABLE;
    goto err;
  }
  if (!is_binary_frm_header(head))
  {
    /* No handling of text based files yet */
    share->error = OPEN_FRM_CORRUPTED;
    goto err;
  }
  if (!(flags & GTS_TABLE))
  {
    share->error = OPEN_FRM_NOT_A_VIEW;
    goto err;
  }

  frmlen= uint4korr(head+10);
  set_if_smaller(frmlen, FRM_MAX_SIZE); // safety

  if (!(buf= (uchar*)my_malloc(PSI_INSTRUMENT_ME, frmlen,
                               MYF(MY_THREAD_SPECIFIC|MY_WME))))
    goto err;

  memcpy(buf, head, sizeof(head));

  read_length= mysql_file_read(file, buf + sizeof(head),
                               frmlen - sizeof(head), MYF(MY_WME));
  if (read_length == 0 || read_length == (size_t)-1)
  {
    share->error = OPEN_FRM_READ_ERROR;
    my_free(buf);
    goto err;
  }
  mysql_file_close(file, MYF(MY_WME));

  frmlen= read_length + sizeof(head);

  share->init_from_binary_frm_image(thd, false, buf, frmlen);
  /*
    Don't give any additional errors. If there would be a problem,
    init_from_binary_frm_image would call my_error() itself.
  */
  error_given= true;
  my_free(buf);

  goto err_not_open;

err:
  mysql_file_close(file, MYF(MY_WME));

err_not_open:
  /* Mark that table was created earlier and thus should have been logged */
  share->table_creation_was_logged= 1;

  if (unlikely(share->error && !error_given))
  {
    share->open_errno= my_errno;
    open_table_error(share, share->error, share->open_errno);
  }

  DBUG_RETURN(share->error);
}

static bool create_key_infos(const uchar *strpos, const uchar *frm_image_end,
                             uint keys, KEY *keyinfo,
                             uint new_frm_ver, uint *ext_key_parts,
                             TABLE_SHARE *share, uint len,
                             KEY *first_keyinfo,
                             LEX_STRING *keynames)
{
  uint i, j, n_length;
  uint primary_key_parts= 0;
  KEY_PART_INFO *key_part= NULL;
  ulong *rec_per_key= NULL;
  DBUG_ASSERT(keyinfo == first_keyinfo);

  if (!keys)
  {  
    if (!(keyinfo = (KEY*) alloc_root(&share->mem_root, len)))
      return 1;
    bzero((char*) keyinfo, len);
    key_part= reinterpret_cast<KEY_PART_INFO*> (keyinfo);
  }
  bzero((char*)first_keyinfo, sizeof(*first_keyinfo));

  /*
    If share->use_ext_keys is set to TRUE we assume that any not
    primary key, can be extended by the components of the primary key
    whose definition is read first from the frm file.
    This code only allocates space for the extend key information as
    we at this point don't know if there is a primary key or not.
    The extend key information is added in init_from_binary_frm_image().

    When in the future we support others schemes of extending of
    secondary keys with components of the primary key we'll have
    to change the type of this flag for an enumeration type.
  */

  for (i=0 ; i < keys ; i++, keyinfo++)
  {
    if (new_frm_ver >= 3)
    {
      if (strpos + 8 >= frm_image_end)
        return 1;
      keyinfo->flags=	   (uint) uint2korr(strpos) ^ HA_NOSAME;
      keyinfo->key_length= (uint) uint2korr(strpos+2);
      keyinfo->user_defined_key_parts=  (uint) strpos[4];
      keyinfo->algorithm=  (enum ha_key_alg) strpos[5];
      keyinfo->block_size= uint2korr(strpos+6);
      strpos+=8;
    }
    else
    {
      if (strpos + 4 >= frm_image_end)
        return 1;
      keyinfo->flags=	 ((uint) strpos[0]) ^ HA_NOSAME;
      keyinfo->key_length= (uint) uint2korr(strpos+1);
      keyinfo->user_defined_key_parts=  (uint) strpos[3];
      keyinfo->algorithm= HA_KEY_ALG_UNDEF;
      strpos+=4;
    }

    if (i == 0)
    {
      /*
        Allocate space for keys. We have to do it there as we need to know
        the number of used_defined_key_parts for the first key when doing
        this.
      */
      primary_key_parts= first_keyinfo->user_defined_key_parts;
      (*ext_key_parts)+= (share->use_ext_keys ?
                          primary_key_parts*(keys-1) :
                          0);
      n_length=keys * sizeof(KEY) + *ext_key_parts * sizeof(KEY_PART_INFO);
      if (!(keyinfo= (KEY*) alloc_root(&share->mem_root,
				       n_length + len)))
        return 1;
      share->key_info= keyinfo;

      /* Copy first keyinfo, read above */
      memcpy((char*) keyinfo, (char*) first_keyinfo, sizeof(*keyinfo));
      bzero(((char*) keyinfo) + sizeof(*keyinfo), n_length - sizeof(*keyinfo));

      key_part= reinterpret_cast<KEY_PART_INFO*> (keyinfo + keys);

      if (!(rec_per_key= (ulong*) alloc_root(&share->mem_root,
                                             sizeof(ulong) * *ext_key_parts)))
        return 1;
      bzero((char*) rec_per_key, sizeof(*rec_per_key) * *ext_key_parts);
    }

    keyinfo->key_part=	 key_part;
    keyinfo->rec_per_key= rec_per_key;
    for (j=keyinfo->user_defined_key_parts ; j-- ; key_part++)
    {
      if (strpos + (new_frm_ver >= 1 ? 9 : 7) >= frm_image_end)
        return 1;
      if (!(keyinfo->algorithm == HA_KEY_ALG_LONG_HASH))
        rec_per_key++;
      key_part->fieldnr=	(uint16) (uint2korr(strpos) & FIELD_NR_MASK);
      key_part->offset= (uint) uint2korr(strpos+2)-1;
      key_part->key_type=	(uint) uint2korr(strpos+5);
      // key_part->field=	(Field*) 0;	// Will be fixed later
      if (new_frm_ver >= 1)
      {
	key_part->key_part_flag= *(strpos+4);
	key_part->length=	(uint) uint2korr(strpos+7);
	strpos+=9;
      }
      else
      {
	key_part->length=	*(strpos+4);
	key_part->key_part_flag=0;
	if (key_part->length > 128)
	{
	  key_part->length&=127;		/* purecov: inspected */
	  key_part->key_part_flag=HA_REVERSE_SORT; /* purecov: inspected */
	}
	strpos+=7;
      }
      key_part->store_length=key_part->length;
    }

    keyinfo->ext_key_parts= keyinfo->user_defined_key_parts;
    keyinfo->ext_key_flags= keyinfo->flags;
    keyinfo->ext_key_part_map= 0;

    if (keyinfo->algorithm == HA_KEY_ALG_LONG_HASH)
    {
      /*
        We should not increase keyinfo->ext_key_parts here as it will
        later be changed to 1 as the engine will only see the generated hash
        key.
      */
      keyinfo->key_length= HA_HASH_KEY_LENGTH_WITHOUT_NULL;
      key_part++;      // This will be set to point to the hash key
      rec_per_key++;   // Only one rec_per_key needed for the hash
      share->ext_key_parts++;
    }

    if (i && share->use_ext_keys && !((keyinfo->flags & HA_NOSAME)))
    {
      /* Reserve place for extended key parts */
      key_part+=    primary_key_parts;
      rec_per_key+= primary_key_parts;
      share->ext_key_parts+= primary_key_parts; // For copy_keys_from_share()
    }
    share->ext_key_parts+= keyinfo->ext_key_parts;
    DBUG_ASSERT(share->ext_key_parts <= *ext_key_parts);
  }
  keynames->str= (char*) key_part;
  keynames->length= strnmov(keynames->str, (char *) strpos,
                            frm_image_end - strpos) - keynames->str;
  strpos+= keynames->length;
  if (*strpos++) // key names are \0-terminated
    return 1;
  keynames->length++; // Include '\0', to make fix_type_pointers() happy.

  //reading index comments
  for (keyinfo= share->key_info, i=0; i < keys; i++, keyinfo++)
  {
    if (keyinfo->flags & HA_USES_COMMENT)
    {
      if (strpos + 2 >= frm_image_end)
        return 1;
      keyinfo->comment.length= uint2korr(strpos);
      strpos+= 2;

      if (strpos + keyinfo->comment.length >= frm_image_end)
        return 1;
      keyinfo->comment.str= strmake_root(&share->mem_root, (char*) strpos,
                                         keyinfo->comment.length);
      strpos+= keyinfo->comment.length;
    } 
    DBUG_ASSERT(MY_TEST(keyinfo->flags & HA_USES_COMMENT) ==
                (keyinfo->comment.length > 0));
  }

  share->keys= keys; // do it *after* all key_info's are initialized

  return 0;
}


/** ensures that the enum value (read from frm) is within limits

    if not - issues a warning and resets the value to 0
    (that is, 0 is assumed to be a default value)
*/

static uint enum_value_with_check(THD *thd, TABLE_SHARE *share,
                                  const char *name, uint value, uint limit)
{
  if (value < limit)
    return value;

  sql_print_warning("%s.frm: invalid value %d for the field %s",
                share->normalized_path.str, value, name);
  return 0;
}


void Column_definition_attributes::frm_pack_basic(uchar *buff) const
{
  int2store(buff + 3, length);
  int2store(buff + 8, pack_flag);
  buff[10]= (uchar) unireg_check;
}


void Column_definition_attributes::frm_unpack_basic(const uchar *buff)
{
  length=       uint2korr(buff + 3);
  pack_flag=    uint2korr(buff + 8);
  unireg_check= (Field::utype) MTYP_TYPENR((uint) buff[10]);
}


void Column_definition_attributes::frm_pack_numeric_with_dec(uchar *buff) const
{
  DBUG_ASSERT(f_decimals(pack_flag) == 0);
  uint tmp_pack_flag= pack_flag | (decimals << FIELDFLAG_DEC_SHIFT);
  int2store(buff + 3, length);
  int2store(buff + 8, tmp_pack_flag);
  buff[10]= (uchar) unireg_check;
}


bool
Column_definition_attributes::frm_unpack_numeric_with_dec(TABLE_SHARE *share,
                                                          const uchar *buff)
{
  frm_unpack_basic(buff);
  decimals= f_decimals(pack_flag);
  pack_flag&= ~FIELDFLAG_DEC_MASK;
  return frm_unpack_charset(share, buff);
}


bool
Column_definition_attributes::frm_unpack_temporal_with_dec(TABLE_SHARE *share,
                                                           uint intlen,
                                                           const uchar *buff)
{
  frm_unpack_basic(buff);
  decimals= temporal_dec(intlen);
  return frm_unpack_charset(share, buff);
}


void Column_definition_attributes::frm_pack_charset(uchar *buff) const
{
  buff[11]= (uchar) (charset->number >> 8);
  buff[14]= (uchar) charset->number;
}


bool Column_definition_attributes::frm_unpack_charset(TABLE_SHARE *share,
                                                      const uchar *buff)
{
  uint cs_org= buff[14] + (((uint) buff[11]) << 8);
  uint cs_new= Charset::upgrade_collation_id(share->mysql_version, cs_org);
  if (cs_org != cs_new)
    share->incompatible_version|= HA_CREATE_USED_CHARSET;
  if (cs_new && !(charset= get_charset(cs_new, MYF(0))))
  {
    const char *csname= get_charset_name((uint) cs_new);
    char tmp[10];
    if (!csname || csname[0] =='?')
    {
      my_snprintf(tmp, sizeof(tmp), "#%u", cs_new);
      csname= tmp;
    }
    my_printf_error(ER_UNKNOWN_COLLATION,
                    "Unknown collation '%s' in table '%-.64s' definition",
                    MYF(0), csname, share->table_name.str);
    return true;
  }
  return false;
}


/*
  In MySQL 5.7 the null bits for not stored virtual fields are last.
  Calculate the position for these bits
*/

static void mysql57_calculate_null_position(TABLE_SHARE *share,
                                            uchar **null_pos,
                                            uint *null_bit_pos,
                                            const uchar *strpos,
                                            const uchar *vcol_screen_pos)
{
  uint field_pack_length= 17;

  for (uint i=0 ; i < share->fields; i++, strpos+= field_pack_length)
  {
    uint field_length, pack_flag;
    enum_field_types field_type;

    if ((strpos[10] & MYSQL57_GENERATED_FIELD))
    {
      /* Skip virtual (not stored) generated field */
      bool stored_in_db= vcol_screen_pos[3];
      vcol_screen_pos+= (uint2korr(vcol_screen_pos + 1) +
                         MYSQL57_GCOL_HEADER_SIZE);
      if (! stored_in_db)
        continue;
    }
    field_length= uint2korr(strpos+3);
    pack_flag=    uint2korr(strpos+8);
    field_type=   (enum_field_types) (uint) strpos[13];
    if (field_type == MYSQL_TYPE_BIT && !f_bit_as_char(pack_flag))
    {
      if (((*null_bit_pos)+= field_length & 7) > 7)
      {
        (*null_pos)++;
        (*null_bit_pos)-= 8;
      }
    }
    if (f_maybe_null(pack_flag))
    {
      if (!((*null_bit_pos)= ((*null_bit_pos) + 1) & 7))
        (*null_pos)++;
    }
  }
}


Item_func_hash *TABLE_SHARE::make_long_hash_func(THD *thd,
                                                 MEM_ROOT *mem_root,
                                                 List<Item> *field_list)
                                                 const
{
  if (old_long_hash_function())
    return new (mem_root) Item_func_hash_mariadb_100403(thd, *field_list);
  return new (mem_root) Item_func_hash(thd, *field_list);
}


/** Parse TABLE_SHARE::vcol_defs

  unpack_vcol_info_from_frm
  5.7
    byte 1      = 1
    byte 2,3    = expr length
    byte 4      = stored_in_db
    expression
  10.1-
    byte 1     = 1 | 2
    byte 2     = sql_type       ; but  TABLE::init_from_binary_frm_image()
    byte 3     = stored_in_db   ; has put expr_length here
    [byte 4]   = optional interval_id for sql_type (if byte 1 == 2)
    expression
  10.2+
    byte 1     = type
    byte 2,3   = field_number
    byte 4,5   = length of expression
    byte 6     = length of name
    name
    expression
*/
bool parse_vcol_defs(THD *thd, MEM_ROOT *mem_root, TABLE *table,
                     bool *error_reported, vcol_init_mode mode)
{
  struct check_vcol_forward_refs
  {
    static bool check(Field *field, Virtual_column_info *vcol)
    {
      return vcol &&
             vcol->expr->walk(&Item::check_field_expression_processor, 0, field);
    }
    static bool check(Field *field)
    {
      if (check(field, field->vcol_info) ||
          check(field, field->default_value))
        return true;
      return false;
    }
  };
  CHARSET_INFO *save_character_set_client= thd->variables.character_set_client;
  CHARSET_INFO *save_collation= thd->variables.collation_connection;
  Query_arena  *backup_stmt_arena_ptr= thd->stmt_arena;
  const uchar *pos= table->s->vcol_defs.str;
  const uchar *end= pos + table->s->vcol_defs.length;
  Field **field_ptr= table->field - 1;
  Field **vfield_ptr= table->vfield;
  Field **dfield_ptr= table->default_field;
  Virtual_column_info **check_constraint_ptr= table->check_constraints;
  Sql_mode_save_for_frm_handling sql_mode_save(thd);
  Query_arena backup_arena;
  Virtual_column_info *vcol= 0;
  StringBuffer<MAX_FIELD_WIDTH> expr_str;
  bool res= 1;
  DBUG_ENTER("parse_vcol_defs");

  if (check_constraint_ptr)
    memcpy(table->check_constraints + table->s->field_check_constraints,
           table->s->check_constraints,
           table->s->table_check_constraints * sizeof(Virtual_column_info*));

  DBUG_ASSERT(table->expr_arena == NULL);

  if (table->init_expr_arena(mem_root))
    DBUG_RETURN(1);

  thd->set_n_backup_active_arena(table->expr_arena, &backup_arena);
  thd->stmt_arena= table->expr_arena;
  thd->update_charset(&my_charset_utf8mb4_general_ci, table->s->table_charset);
  expr_str.append(&parse_vcol_keyword);

  while (pos < end)
  {
    uint type, expr_length;
    if (table->s->frm_version >= FRM_VER_EXPRESSSIONS)
    {
      uint field_nr, name_length;
      /* see pack_expression() for how data is stored */
      type= pos[0];
      field_nr= uint2korr(pos+1);
      expr_length= uint2korr(pos+3);
      name_length= pos[5];
      pos+= FRM_VCOL_NEW_HEADER_SIZE + name_length;
      field_ptr= table->field + field_nr;
    }
    else
    {
      /*
        see below in ::init_from_binary_frm_image for how data is stored
        in versions below 10.2 (that includes 5.7 too)
      */
      while (*++field_ptr && !(*field_ptr)->vcol_info) /* no-op */;
      if (!*field_ptr)
      {
        open_table_error(table->s, OPEN_FRM_CORRUPTED, 1);
        goto end;
      }
      type= (*field_ptr)->vcol_info->is_stored()
            ? VCOL_GENERATED_STORED : VCOL_GENERATED_VIRTUAL;
      expr_length= uint2korr(pos+1);
      if (table->s->mysql_version > 50700 && table->s->mysql_version < 100000)
      {
        table->s->keep_original_mysql_version= 1;
        pos+= 4;                        // MySQL from 5.7
      }
      else
        pos+= pos[0] == 2 ? 4 : 3;      // MariaDB from 5.2 to 10.1
    }

    expr_str.length(parse_vcol_keyword.length);
    expr_str.append((char*)pos, expr_length);
    thd->where= THD_WHERE::USE_WHERE_STRING;
    thd->where_str= vcol_type_name(static_cast<enum_vcol_info_type>(type));

    switch (type) {
    case VCOL_GENERATED_VIRTUAL:
    case VCOL_GENERATED_STORED:
      vcol= unpack_vcol_info_from_frm(thd, table, &expr_str,
                                    &((*field_ptr)->vcol_info), error_reported);
      *(vfield_ptr++)= *field_ptr;
      DBUG_ASSERT(table->map == 0);
      /*
        We need Item_field::const_item() to return false, so
        datetime_precision() and time_precision() do not try to calculate
        field values, e.g. val_str().
        Set table->map to non-zero temporarily.
      */
      table->map= 1;
      if (vcol && field_ptr[0]->check_vcol_sql_mode_dependency(thd, mode))
      {
        DBUG_ASSERT(thd->is_error());
        *error_reported= true;
        goto end;
      }
      table->map= 0;
      break;
    case VCOL_DEFAULT:
      vcol= unpack_vcol_info_from_frm(thd, table, &expr_str,
                                      &((*field_ptr)->default_value),
                                      error_reported);
      *(dfield_ptr++)= *field_ptr;
      if (vcol && (vcol->flags & (VCOL_NON_DETERMINISTIC | VCOL_SESSION_FUNC)))
        table->s->non_determinstic_insert= true;
      break;
    case VCOL_CHECK_FIELD:
      vcol= unpack_vcol_info_from_frm(thd, table, &expr_str,
                                      &((*field_ptr)->check_constraint),
                                      error_reported);
      *check_constraint_ptr++= (*field_ptr)->check_constraint;
      break;
    case VCOL_CHECK_TABLE:
      vcol= unpack_vcol_info_from_frm(thd, table, &expr_str,
                                      check_constraint_ptr, error_reported);
      check_constraint_ptr++;
      break;
    }
    if (!vcol)
      goto end;
    pos+= expr_length;
  }

  /* Now, initialize CURRENT_TIMESTAMP and UNIQUE_INDEX_HASH_FIELD fields */
  for (field_ptr= table->field; *field_ptr; field_ptr++)
  {
    Field *field= *field_ptr;
    if (field->flags & LONG_UNIQUE_HASH_FIELD)
    {
      List<Item> *field_list= new (mem_root) List<Item>();
      Item *list_item;
      KEY *key= 0;
      uint key_index, parts= 0;
      for (key_index= 0; key_index < table->s->keys; key_index++)
      {
        key=table->key_info + key_index;
        parts= key->user_defined_key_parts;
        if (key->key_part[parts].fieldnr == field->field_index + 1)
            break;
      }
      if (!key || key->algorithm != HA_KEY_ALG_LONG_HASH)
        goto end;
      KEY_PART_INFO *keypart;
      for (uint i=0; i < parts; i++)
      {
        keypart= key->key_part + i;
        if (keypart->key_part_flag & HA_PART_KEY_SEG)
        {
          int length= keypart->length/keypart->field->charset()->mbmaxlen;
          Field *kpf= table->field[keypart->field->field_index];
          list_item= new (mem_root) Item_func_left(thd,
                       new (mem_root) Item_field(thd, kpf),
                       new (mem_root) Item_int(thd, length));
          list_item->fix_fields(thd, NULL);
        }
        else
          list_item= new (mem_root) Item_field(thd, keypart->field);
        field_list->push_back(list_item, mem_root);
      }

      Item_func_hash *hash_item= table->s->make_long_hash_func(thd, mem_root,
                                                               field_list);

      Virtual_column_info *v= new (mem_root) Virtual_column_info();
      field->vcol_info= v;
      field->vcol_info->expr= hash_item;
      field->vcol_info->set_vcol_type(VCOL_USING_HASH);
      if (v->fix_and_check_expr(thd, table))
        goto end;
      /*
        The hash key used by unique consist of one key_part.
        It is stored in key_parts after the used defined parts.
        The engine will only see the hash.
      */
      key->user_defined_key_parts= key->usable_key_parts=
        key->ext_key_parts= 1;
      key->key_part+= parts;

      if (key->flags & HA_NULL_PART_KEY)
        key->key_length= HA_HASH_KEY_LENGTH_WITH_NULL;
      else
        key->key_length= HA_HASH_KEY_LENGTH_WITHOUT_NULL;

      *(vfield_ptr++)= *field_ptr;
    }
    if (field->has_default_now_unireg_check())
    {
      expr_str.length(parse_vcol_keyword.length);
      expr_str.append(STRING_WITH_LEN("current_timestamp("));
      expr_str.append_ulonglong(field->decimals());
      expr_str.append(')');
      vcol= unpack_vcol_info_from_frm(thd, table, &expr_str,
                                      &((*field_ptr)->default_value),
                                      error_reported);
      *(dfield_ptr++)= *field_ptr;
      if (!field->default_value->expr)
        goto end;
    }
    else if (field->has_update_default_function() && !field->default_value)
      *(dfield_ptr++)= *field_ptr;
  }

  if (vfield_ptr)
    *vfield_ptr= 0;

  if (dfield_ptr)
    *dfield_ptr= 0;

  if (check_constraint_ptr)
    *check_constraint_ptr= 0;

  /* Check that expressions aren't referring to not yet initialized fields */
  for (field_ptr= table->field; *field_ptr; field_ptr++)
  {
    if (check_vcol_forward_refs::check(*field_ptr))
    {
      *error_reported= true;
      goto end;
    }
    if ((*field_ptr)->check_constraint)
        (*field_ptr)->check_constraint->expr->
          walk(&Item::update_func_default_processor, 0, *field_ptr);
  }

  table->find_constraint_correlated_indexes();

  res=0;
end:
  thd->restore_active_arena(table->expr_arena, &backup_arena);
  thd->stmt_arena= backup_stmt_arena_ptr;
  if (save_character_set_client)
    thd->update_charset(save_character_set_client, save_collation);
  DBUG_RETURN(res);
}


static const Type_handler *old_frm_type_handler(uint pack_flag,
                                                uint interval_nr)
{
  enum_field_types field_type= (enum_field_types) f_packtype(pack_flag);
  DBUG_ASSERT(field_type < 16);

  if (!f_is_alpha(pack_flag))
    return Type_handler::get_handler_by_real_type(field_type);

  if (!f_is_packed(pack_flag))
  {
    if (field_type == MYSQL_TYPE_DECIMAL)  // 3.23 or 4.0 string
      return &type_handler_string;
    if (field_type == MYSQL_TYPE_VARCHAR)  // Since mysql-5.0
      return &type_handler_varchar;
    return NULL;  // Error (bad frm?)
  }

  if (f_is_blob(pack_flag))
    return &type_handler_blob; // QQ: exact type??

  if (interval_nr)
  {
    if (f_is_enum(pack_flag))
      return &type_handler_enum;
    return &type_handler_set;
  }
  return Type_handler::get_handler_by_real_type(field_type);
}

/* Set overlapped bitmaps for each index */

void TABLE_SHARE::set_overlapped_keys()
{
  KEY *key1= key_info;
  for (uint i= 0; i < keys; i++, key1++)
  {
    key1->overlapped.clear_all();
    key1->overlapped.set_bit(i);
  }
  key1= key_info;
  for (uint i= 0; i < keys; i++, key1++)
  {
    KEY *key2= key1 + 1;
    for (uint j= i+1; j < keys; j++, key2++)
    {
      KEY_PART_INFO *key_part1= key1->key_part;
      uint n1= key1->user_defined_key_parts;
      uint n2= key2->user_defined_key_parts;
      for (uint k= 0; k < n1; k++, key_part1++)
      {
        KEY_PART_INFO *key_part2= key2->key_part;
        for (uint l= 0; l < n2; l++, key_part2++)
	{
          if (key_part1->fieldnr == key_part2->fieldnr)
	  {
            key1->overlapped.set_bit(j);
            key2->overlapped.set_bit(i);
            goto end_checking_overlap;
          }
        }
      }
    end_checking_overlap:
      ;
    }
  }
}


/*
  @brief
    Set of indexes that are marked as IGNORE.
*/

void TABLE_SHARE::set_ignored_indexes()
{
  KEY *keyinfo= key_info;
  for (uint i= 0; i < keys; i++, keyinfo++)
  {
    if (keyinfo->is_ignored)
      ignored_indexes.set_bit(i);
  }
}


/*
  @brief
    Set of indexes that the optimizer may use when creating an execution plan.
*/

key_map TABLE_SHARE::usable_indexes(THD *thd)
{
  key_map usable_indexes(keys_in_use);
  usable_indexes.subtract(ignored_indexes);
  return usable_indexes;
}


bool Item_field::check_index_dependence(void *arg)
{
  TABLE *table= (TABLE *)arg;

  KEY *key= table->key_info;
  for (uint j= 0; j < table->s->keys; j++, key++)
  {
    if (table->constraint_dependent_keys.is_set(j))
      continue;

    KEY_PART_INFO *key_part= key->key_part;
    uint n= key->user_defined_key_parts;

    for (uint k= 0; k < n; k++, key_part++)
    {
      if (this->field == key_part->field)
      {
        table->constraint_dependent_keys.set_bit(j);
        break;
      }
    }
  }
  return false;
}


/**
  @brief
    Find keys that occur in the same constraint on this table

  @details
    Constraints on this table are checked only.

    The method goes through constraints list trying to find at
    least two keys which parts participate in some constraint.
    These keys are called constraint correlated.

    Each key has its own key map with the information about with
    which keys it is constraint correlated. Bit in this map is set
    only if keys are constraint correlated.
    This method fills each keys constraint correlated key map.
*/

void TABLE::find_constraint_correlated_indexes()
{
  if (s->keys == 0)
    return;

  KEY *key= key_info;
  for (uint i= 0; i < s->keys; i++, key++)
  {
    key->constraint_correlated.clear_all();
    key->constraint_correlated.set_bit(i);
  }

  if (!check_constraints)
    return;

  for (Virtual_column_info **chk= check_constraints ; *chk ; chk++)
  {
    constraint_dependent_keys.clear_all();
    (*chk)->expr->walk(&Item::check_index_dependence, 0, this);

    if (constraint_dependent_keys.bits_set() <= 1)
      continue;

    uint key_no= 0;
    key_map::Iterator ki(constraint_dependent_keys);
    while ((key_no= ki++) != key_map::Iterator::BITMAP_END)
      key_info[key_no].constraint_correlated.merge(constraint_dependent_keys);
  }
}


bool TABLE_SHARE::init_period_from_extra2(period_info_t *period,
                                          const uchar *data, const uchar *end)
{
  if (data + 2*frm_fieldno_size > end)
    return 1;
  period->start_fieldno= read_frm_fieldno(data);
  period->end_fieldno= read_frm_fieldno(data + frm_fieldno_size);
  return period->start_fieldno >= fields || period->end_fieldno >= fields;
}


static
bool read_extra2_section_once(const uchar *extra2, size_t len, LEX_CUSTRING *section)
{
  if (section->str)
    return true;
  *section= {extra2, len};
  return false;
}

static
bool read_extra2(const uchar *frm_image, size_t len, extra2_fields *fields)
{
  const uchar *extra2= frm_image + 64;

  DBUG_ENTER("read_extra2");

  fields->reset();

  if (*extra2 != '/')   // old frm had '/' there
  {
    const uchar *e2end= extra2 + len;
    while (extra2 + 3 <= e2end)
    {
      extra2_frm_value_type type= (extra2_frm_value_type)*extra2++;
      size_t length= extra2_read_len(&extra2, e2end);
      if (!length)
        DBUG_RETURN(true);

      bool fail= false;
      switch (type) {
        case EXTRA2_TABLEDEF_VERSION:
          if (fields->version.str) // see init_from_sql_statement_string()
          {
            if (length != fields->version.length)
              DBUG_RETURN(true);
          }
          else
          {
            fields->version.str= extra2;
            fields->version.length= length;
          }
          break;
        case EXTRA2_ENGINE_TABLEOPTS:
          fail= read_extra2_section_once(extra2, length, &fields->options);
          break;
        case EXTRA2_DEFAULT_PART_ENGINE:
          fields->engine.set((const char*)extra2, length);
          break;
        case EXTRA2_GIS:
          fail= read_extra2_section_once(extra2, length, &fields->gis);
          break;
        case EXTRA2_PERIOD_FOR_SYSTEM_TIME:
          fail= read_extra2_section_once(extra2, length, &fields->system_period)
                  || length != 2 * frm_fieldno_size;
          break;
        case EXTRA2_FIELD_FLAGS:
          fail= read_extra2_section_once(extra2, length, &fields->field_flags);
          break;
        case EXTRA2_APPLICATION_TIME_PERIOD:
          fail= read_extra2_section_once(extra2, length, &fields->application_period);
          break;
        case EXTRA2_PERIOD_WITHOUT_OVERLAPS:
          fail= read_extra2_section_once(extra2, length, &fields->without_overlaps);
          break;
        case EXTRA2_FIELD_DATA_TYPE_INFO:
          fail= read_extra2_section_once(extra2, length, &fields->field_data_type_info);
          break;
        case EXTRA2_INDEX_FLAGS:
          fail= read_extra2_section_once(extra2, length, &fields->index_flags);
          break;
        default:
          /* abort frm parsing if it's an unknown but important extra2 value */
          if (type >= EXTRA2_ENGINE_IMPORTANT)
            DBUG_RETURN(true);
      }
      if (fail)
        DBUG_RETURN(true);

      extra2+= length;
    }
    if (extra2 != e2end)
      DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}


class Field_data_type_info_array
{
public:
  class Elem
  {
    LEX_CSTRING m_type_info;
  public:
    void set(const LEX_CSTRING &type_info)
    {
      m_type_info= type_info;
    }
    const LEX_CSTRING &type_info() const
    {
      return m_type_info;
    }
  };
private:
  Elem *m_array;
  uint m_count;
  bool alloc(MEM_ROOT *root, uint count)
  {
    DBUG_ASSERT(!m_array);
    DBUG_ASSERT(!m_count);
    size_t nbytes= sizeof(Elem) * count;
    if (!(m_array= (Elem*) alloc_root(root, nbytes)))
      return true;
    m_count= count;
    bzero((void*) m_array, nbytes);
    return false;
  }
  static uint32 read_length(uchar **pos, const uchar *end)
  {
    ulonglong num= safe_net_field_length_ll(pos, end - *pos);
    if (num > UINT_MAX32)
      return 0;
    return (uint32) num;
  }
  static bool read_string(LEX_CSTRING *to, uchar **pos, const uchar *end)
  {
    to->length= read_length(pos, end);
    if (*pos + to->length > end)
      return true; // Not enough data
    to->str= (const char *) *pos;
    *pos+= to->length;
    return false;
  }
public:
  Field_data_type_info_array()
   :m_array(NULL), m_count(0)
  { }
  uint count() const
  {
    return m_count;
  }
  const Elem& element(uint i) const
  {
    DBUG_ASSERT(i < m_count);
    return m_array[i];
  }
  bool parse(MEM_ROOT *root, uint count, LEX_CUSTRING &image)
  {
    const uchar *pos= image.str;
    const uchar *end= pos + image.length;
    if (alloc(root, count))
      return true;
    for (uint i= 0; i < count && pos < end; i++)
    {
      LEX_CSTRING type_info;
      uint fieldnr= read_length((uchar**) &pos, end);
      if ((fieldnr == 0 && i > 0) || fieldnr >= count)
        return true; // Bad data
      if (read_string(&type_info, (uchar**) &pos, end) || type_info.length == 0)
        return true; // Bad data
      m_array[fieldnr].set(type_info);
    }
    return pos < end; // Error if some data is still left
  }
};


/*
  Change to use the partition storage engine
*/

#ifdef WITH_PARTITION_STORAGE_ENGINE
static bool change_to_partiton_engine(plugin_ref *se_plugin)
{
  LEX_CSTRING name= { STRING_WITH_LEN("partition") };
  /*
    Use partition handler
    tmp_plugin is locked with a local lock.
    we unlock the old value of se_plugin before
    replacing it with a globally locked version of tmp_plugin
  */
  /* Check if the partitioning engine is ready */
  if (!plugin_is_ready(&name, MYSQL_STORAGE_ENGINE_PLUGIN))
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--skip-partition");
    return 1;
  }
  plugin_unlock(NULL, *se_plugin);
  *se_plugin= ha_lock_engine(NULL, partition_hton);
  return 0;
}
#endif /* WITH_PARTITION_STORAGE_ENGINE */

/**
  Read data from a binary .frm file image into a TABLE_SHARE

  @param write   Write the .frm and .par file.  These are not created if
                 the function returns an error.

  @note
  frm bytes at the following offsets are unused in MariaDB 10.0:

  8..9    (used to be the number of "form names")
  28..29  (used to be key_info_length)

  They're still set, for compatibility reasons, but never read.

  42..46 are unused since 5.0 (were for RAID support)
  Also, there're few unused bytes in forminfo.
*/

int TABLE_SHARE::init_from_binary_frm_image(THD *thd, bool write,
                                            const uchar *frm_image,
                                            size_t frm_length,
                                            const uchar *par_image,
                                            size_t par_length)
{
  TABLE_SHARE *share= this;
  uint new_frm_ver, field_pack_length, new_field_pack_flag;
  uint interval_count, interval_parts, read_length, int_length;
  uint total_typelib_value_count;
  uint db_create_options, keys, key_parts, n_length;
  uint com_length, null_bit_pos, UNINIT_VAR(mysql57_vcol_null_bit_pos), bitmap_count;
  uint i, hash_fields= 0;
  bool use_hash, mysql57_null_bits= 0;
  LEX_STRING keynames= {NULL, 0};
  char *names, *comment_pos;
  const uchar *forminfo;
  const uchar *frm_image_end = frm_image + frm_length;
  uchar *record, *null_flags, *null_pos, *UNINIT_VAR(mysql57_vcol_null_pos);
  const uchar *disk_buff, *strpos;
  ulong pos, record_offset;
  ulong rec_buff_length;
  handler *handler_file= 0;
  KEY	*keyinfo;
  KEY_PART_INFO *key_part= NULL;
  Field  **field_ptr, *reg_field;
  const char **interval_array;
  uint *typelib_value_lengths= NULL;
  enum legacy_db_type legacy_db_type;
  my_bitmap_map *bitmaps;
  bool null_bits_are_used;
  uint vcol_screen_length;
  uchar *vcol_screen_pos;
  LEX_CUSTRING options;
  LEX_CSTRING se_name= empty_clex_str;
  KEY first_keyinfo;
  uint len;
  uint ext_key_parts= 0;
  plugin_ref se_plugin= 0;
  bool vers_can_native= false, frm_created= 0;
  Field_data_type_info_array field_data_type_info_array;
  MEM_ROOT *old_root= thd->mem_root;
  Virtual_column_info **table_check_constraints;
  bool *interval_unescaped= NULL;
  extra2_fields extra2;
  bool extra_index_flags_present= FALSE;
  DBUG_ENTER("TABLE_SHARE::init_from_binary_frm_image");

  keyinfo= &first_keyinfo;
  thd->mem_root= &share->mem_root;

  if (frm_length < FRM_HEADER_SIZE + FRM_FORMINFO_SIZE)
    goto err;

  if (write)
  {
    frm_created= 1;
    if (write_frm_image(frm_image, frm_length))
      goto err;
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (par_image)
      if (write_par_image(par_image, par_length))
        goto err;
#endif
  }

  share->frm_version= frm_image[2];
  /*
    Check if .frm file created by MySQL 5.0. In this case we want to
    display CHAR fields as CHAR and not as VARCHAR.
    We do it this way as we want to keep the old frm version to enable
    MySQL 4.1 to read these files.
  */
  if (share->frm_version == FRM_VER_TRUE_VARCHAR -1 && frm_image[33] == 5)
    share->frm_version= FRM_VER_TRUE_VARCHAR;

  new_field_pack_flag= frm_image[27];
  new_frm_ver= (frm_image[2] - FRM_VER);
  field_pack_length= new_frm_ver < 2 ? 11 : 17;

  /* Length of the MariaDB extra2 segment in the form file. */
  len = uint2korr(frm_image+4);

  if (read_extra2(frm_image, len, &extra2))
    goto err;

  tabledef_version.length= extra2.version.length;
  tabledef_version.str= (uchar*)memdup_root(&mem_root, extra2.version.str,
                                                       extra2.version.length);
  if (!tabledef_version.str)
    goto err;

  /* remember but delay parsing until we have read fields and keys */
  options= extra2.options;

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (extra2.engine)
  {
    share->default_part_plugin= ha_resolve_by_name(NULL, &extra2.engine, false);
    if (!share->default_part_plugin)
      goto err;
  }
#endif

  if (frm_length < FRM_HEADER_SIZE + len ||
      !(pos= uint4korr(frm_image + FRM_HEADER_SIZE + len)))
    goto err;

  forminfo= frm_image + pos;
  if (forminfo + FRM_FORMINFO_SIZE >= frm_image_end)
    goto err;

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (frm_image[61] && !share->default_part_plugin)
  {
    enum legacy_db_type db_type= (enum legacy_db_type) (uint) frm_image[61];
    share->default_part_plugin= ha_lock_engine(NULL, ha_checktype(thd, db_type, 1));
    if (!share->default_part_plugin)
      goto err;
  }
#endif
  legacy_db_type= (enum legacy_db_type) (uint) frm_image[3];
  /*
    if the storage engine is dynamic, no point in resolving it by its
    dynamically allocated legacy_db_type. We will resolve it later by name.
  */
  if (legacy_db_type > DB_TYPE_UNKNOWN && 
      legacy_db_type < DB_TYPE_FIRST_DYNAMIC)
    se_plugin= ha_lock_engine(NULL, ha_checktype(thd, legacy_db_type));
  share->db_create_options= db_create_options= uint2korr(frm_image+30);
  share->db_options_in_use= share->db_create_options;
  share->mysql_version= uint4korr(frm_image+51);
  share->table_type= TABLE_TYPE_NORMAL;
  share->null_field_first= 0;
  if (!frm_image[32])				// New frm file in 3.23
  {
    uint cs_org= (((uint) frm_image[41]) << 8) + (uint) frm_image[38];
    uint cs_new= Charset::upgrade_collation_id(share->mysql_version, cs_org);
    if (cs_org != cs_new)
      share->incompatible_version|= HA_CREATE_USED_CHARSET;

    share->avg_row_length= uint4korr(frm_image+34);
    share->transactional= (ha_choice)
      enum_value_with_check(thd, share, "transactional", frm_image[39] & 3, HA_CHOICE_MAX);
    share->page_checksum= (ha_choice)
      enum_value_with_check(thd, share, "page_checksum", (frm_image[39] >> 2) & 3, HA_CHOICE_MAX);
    if (((ha_choice) enum_value_with_check(thd, share, "sequence",
                                           (frm_image[39] >> 4) & 3,
                                           HA_CHOICE_MAX)) == HA_CHOICE_YES)
    {
      share->table_type= TABLE_TYPE_SEQUENCE;
      share->sequence= new (&share->mem_root) SEQUENCE();
      share->non_determinstic_insert= true;
    }
    share->row_type= (enum row_type)
      enum_value_with_check(thd, share, "row_format", frm_image[40], ROW_TYPE_MAX);

    if (cs_new && !(share->table_charset= get_charset(cs_new, MYF(MY_WME))))
      goto err;
    share->null_field_first= 1;
    share->stats_sample_pages= uint2korr(frm_image+42);
    share->stats_auto_recalc= (enum_stats_auto_recalc)(frm_image[44]);
    share->table_check_constraints= uint2korr(frm_image+45);
  }
  if (!share->table_charset)
  {
    const CHARSET_INFO *cs= thd->variables.collation_database;
    /* unknown charset in frm_image[38] or pre-3.23 frm */
    if (cs->use_mb())
    {
      /* Warn that we may be changing the size of character columns */
      sql_print_warning("'%s' had no or invalid character set, "
                        "and default character set is multi-byte, "
                        "so character column sizes may have changed",
                        share->path.str);
    }
    share->table_charset= cs;
  }

  share->db_record_offset= 1;
  share->max_rows= uint4korr(frm_image+18);
  share->min_rows= uint4korr(frm_image+22);

  /* Read keyinformation */
  disk_buff= frm_image + uint2korr(frm_image+6);

  if (disk_buff + 6 >= frm_image_end)
    goto err;

  if (disk_buff[0] & 0x80)
  {
    keys=      (disk_buff[1] << 7) | (disk_buff[0] & 0x7f);
    share->key_parts= key_parts= uint2korr(disk_buff+2);
  }
  else
  {
    keys=      disk_buff[0];
    share->key_parts= key_parts= disk_buff[1];
  }
  share->keys_for_keyread.init(0);
  share->ignored_indexes.init(0);
  share->keys_in_use.init(keys);
  ext_key_parts= key_parts;

  if (extra2.index_flags.str && extra2.index_flags.length != keys)
    goto err;

  len= (uint) uint2korr(disk_buff+4);

  share->reclength = uint2korr(frm_image+16);
  share->stored_rec_length= share->reclength;
  if (frm_image[26] == 1)
    share->system= 1;				/* one-record-database */

  record_offset= (ulong) (uint2korr(frm_image+6)+
                          ((uint2korr(frm_image+14) == 0xffff ?
                            uint4korr(frm_image+47) : uint2korr(frm_image+14))));

  if (record_offset + share->reclength >= frm_length)
    goto err;
 
  if ((n_length= uint4korr(frm_image+55)))
  {
    /* Read extra data segment */
    const uchar *next_chunk, *buff_end;
    DBUG_PRINT("info", ("extra segment size is %u bytes", n_length));
    next_chunk= frm_image + record_offset + share->reclength;
    buff_end= next_chunk + n_length;

    if (buff_end >= frm_image_end)
      goto err;

    share->connect_string.length= uint2korr(next_chunk);
    if (!(share->connect_string.str= strmake_root(&share->mem_root,
                                                  (char*) next_chunk + 2,
                                                  share->connect_string.
                                                  length)))
    {
      goto err;
    }
    next_chunk+= share->connect_string.length + 2;
    if (next_chunk + 2 < buff_end)
    {
      uint str_db_type_length= uint2korr(next_chunk);
      se_name.str= (char*) next_chunk + 2;
      se_name.length= str_db_type_length;

      plugin_ref tmp_plugin= ha_resolve_by_name(thd, &se_name, false);
      if (tmp_plugin != NULL && !plugin_equals(tmp_plugin, se_plugin) &&
          legacy_db_type != DB_TYPE_S3)
      {
        if (se_plugin)
        {
          /* bad file, legacy_db_type did not match the name */
          sql_print_warning("%s.frm is inconsistent: engine typecode %d, engine name %s (%d)",
                        share->normalized_path.str, legacy_db_type,
                        plugin_name(tmp_plugin)->str,
                        ha_legacy_type(plugin_data(tmp_plugin, handlerton *)));
        }
        /*
          tmp_plugin is locked with a local lock.
          we unlock the old value of se_plugin before
          replacing it with a globally locked version of tmp_plugin
        */
        plugin_unlock(NULL, se_plugin);
        se_plugin= plugin_lock(NULL, tmp_plugin);
      }
#ifdef WITH_PARTITION_STORAGE_ENGINE
      else if (str_db_type_length == 9 &&
               !strncmp((char *) next_chunk + 2, "partition", 9))
      {
        if (change_to_partiton_engine(&se_plugin))
          goto err;
      }
#endif
      else if (!tmp_plugin)
      {
        /* purecov: begin inspected */
        ((char*) se_name.str)[se_name.length]=0;
        my_error(ER_UNKNOWN_STORAGE_ENGINE, MYF(0), se_name.str);
        goto err;
        /* purecov: end */
      }
      next_chunk+= str_db_type_length + 2;
    }

    /*
      Check if engine supports extended keys. This is used by
      create_key_infos() to allocate room for extended keys
    */
    share->set_use_ext_keys_flag(plugin_hton(se_plugin)->flags &
                                 HTON_SUPPORTS_EXTENDED_KEYS);

    if (create_key_infos(disk_buff + 6, frm_image_end, keys, keyinfo,
                         new_frm_ver, &ext_key_parts,
                         share, len, &first_keyinfo, &keynames))
      goto err;

    if (next_chunk + 5 < buff_end)
    {
      uint32 partition_info_str_len = uint4korr(next_chunk);
#ifdef WITH_PARTITION_STORAGE_ENGINE
      if ((share->partition_info_buffer_size=
             share->partition_info_str_len= partition_info_str_len))
      {
        if (!(share->partition_info_str= (char*)
              memdup_root(&share->mem_root, next_chunk + 4,
                          partition_info_str_len + 1)))
        {
          goto err;
        }
        if (plugin_data(se_plugin, handlerton*) != partition_hton &&
            share->mysql_version >= 50600 && share->mysql_version <= 50799)
        {
          share->keep_original_mysql_version= 1;
          if (change_to_partiton_engine(&se_plugin))
            goto err;
        }
      }
#else
      if (partition_info_str_len)
      {
        DBUG_PRINT("info", ("WITH_PARTITION_STORAGE_ENGINE is not defined"));
        goto err;
      }
#endif
      next_chunk+= 5 + partition_info_str_len;
    }
    if (share->mysql_version >= 50110 && next_chunk < buff_end)
    {
      /* New auto_partitioned indicator introduced in 5.1.11 */
#ifdef WITH_PARTITION_STORAGE_ENGINE
      share->auto_partitioned= *next_chunk;
#endif
      next_chunk++;
    }
    keyinfo= share->key_info;
    for (i= 0; i < keys; i++, keyinfo++)
    {
      if (keyinfo->flags & HA_USES_PARSER)
      {
        LEX_CSTRING parser_name;
        if (next_chunk >= buff_end)
        {
          DBUG_PRINT("error",
                     ("fulltext key uses parser that is not defined in .frm"));
          goto err;
        }
        parser_name.str= (char*) next_chunk;
        parser_name.length= strlen((char*) next_chunk);
        next_chunk+= parser_name.length + 1;
        keyinfo->parser= my_plugin_lock_by_name(NULL, &parser_name,
                                                MYSQL_FTPARSER_PLUGIN);
        if (! keyinfo->parser)
        {
          my_error(ER_PLUGIN_IS_NOT_LOADED, MYF(0), parser_name.str);
          goto err;
        }
      }
    }

    if (forminfo[46] == (uchar)255)
    {
      //reading long table comment
      if (next_chunk + 2 > buff_end)
      {
          DBUG_PRINT("error",
                     ("long table comment is not defined in .frm"));
          goto err;
      }
      share->comment.length = uint2korr(next_chunk);
      if (! (share->comment.str= strmake_root(&share->mem_root,
             (char*)next_chunk + 2, share->comment.length)))
      {
          goto err;
      }
      next_chunk+= 2 + share->comment.length;
    }

    DBUG_ASSERT(next_chunk <= buff_end);

    if (share->db_create_options & HA_OPTION_TEXT_CREATE_OPTIONS_legacy)
    {
      if (options.str)
        goto err;
      options.length= uint4korr(next_chunk);
      options.str= next_chunk + 4;
      next_chunk+= options.length + 4;
    }
    DBUG_ASSERT(next_chunk <= buff_end);
  }
  else
  {
    if (create_key_infos(disk_buff + 6, frm_image_end, keys, keyinfo,
                         new_frm_ver, &ext_key_parts,
                         share, len, &first_keyinfo, &keynames))
      goto err;
  }
  share->key_block_size= uint2korr(frm_image+62);
  keyinfo= share->key_info;


  if (extra2.index_flags.str)
    extra_index_flags_present= TRUE;

  for (uint i= 0; i < share->keys; i++, keyinfo++)
  {
    if (extra_index_flags_present)
    {
      uchar flags= *extra2.index_flags.str++;
      keyinfo->is_ignored= (flags & EXTRA2_IGNORED_KEY);
    }
    else
      keyinfo->is_ignored= FALSE;

    if (keyinfo->algorithm == HA_KEY_ALG_LONG_HASH)
      hash_fields++;
  }

  share->set_ignored_indexes();

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (par_image && plugin_data(se_plugin, handlerton*) == partition_hton)
  {
    /*
      Discovery returned a partition plugin. Change to use it. The partition
      engine will then use discovery to find the rest of the plugin tables,
      which may be in the original engine used for discovery
    */
    share->db_plugin= se_plugin;
  }
#endif
  if (share->db_plugin && !plugin_equals(share->db_plugin, se_plugin))
    goto err; // wrong engine (someone changed the frm under our feet?)

  rec_buff_length= ALIGN_SIZE(share->reclength + 1);
  share->rec_buff_length= rec_buff_length;
  if (!(record= (uchar *) alloc_root(&share->mem_root, rec_buff_length)))
    goto err;                          /* purecov: inspected */
  /* Mark bytes after record as not accessable to catch overrun bugs */
  MEM_NOACCESS(record + share->reclength, rec_buff_length - share->reclength);
  share->default_values= record;
  memcpy(record, frm_image + record_offset, share->reclength);

  disk_buff= frm_image + pos + FRM_FORMINFO_SIZE;
  share->fields= uint2korr(forminfo+258);
  if (extra2.field_flags.str && extra2.field_flags.length != share->fields)
    goto err;
  pos= uint2korr(forminfo+260);   /* Length of all screens */
  n_length= uint2korr(forminfo+268);
  interval_count= uint2korr(forminfo+270);
  interval_parts= uint2korr(forminfo+272);
  int_length= uint2korr(forminfo+274);
  share->null_fields= uint2korr(forminfo+282);
  com_length= uint2korr(forminfo+284);
  vcol_screen_length= uint2korr(forminfo+286);
  share->virtual_fields= share->default_expressions=
    share->field_check_constraints= share->default_fields= 0;
  share->visible_fields= 0;
  share->stored_fields= share->fields;
  if (forminfo[46] != (uchar)255)
  {
    share->comment.length=  (int) (forminfo[46]);
    share->comment.str= strmake_root(&share->mem_root, (char*) forminfo+47,
                                     share->comment.length);
  }

  DBUG_PRINT("info",("i_count: %d  i_parts: %d  index: %d  n_length: %d  int_length: %d  com_length: %d  vcol_screen_length: %d", interval_count,interval_parts, keys,n_length,int_length, com_length, vcol_screen_length));

  /*
    We load the following things into TYPELIBs:
    - One TYPELIB for field names
    - interval_count TYPELIBs for ENUM/SET values
    - One TYPELIB for key names
    Every TYPELIB requires one extra value with a NULL pointer and zero length,
    which is the end-of-values marker.
    TODO-10.5+:
    Note, we should eventually reuse this total_typelib_value_count
    to allocate interval_array. The below code reserves less space
    than total_typelib_value_count pointers. So it seems `interval_array`
    and `names` overlap in the memory. Too dangerous to fix in 10.1.
  */
  total_typelib_value_count=
    (share->fields  +              1/*end-of-values marker*/) +
    (interval_parts + interval_count/*end-of-values markers*/) +
    (keys           +              1/*end-of-values marker*/);

  if (!multi_alloc_root(&share->mem_root,
                        &share->field, (uint)(share->fields+1)*sizeof(Field*),
                        &share->intervals, (uint)interval_count*sizeof(TYPELIB),
                        &share->check_constraints, (uint) share->table_check_constraints * sizeof(Virtual_column_info*),
                        /*
                           This looks wrong: shouldn't it be (+2+interval_count)
                           instread of (+3) ?
                        */
                        &interval_array, (uint) (share->fields+interval_parts+ keys+3)*sizeof(char *),
                        &typelib_value_lengths, total_typelib_value_count * sizeof(uint *),
                        &names, (uint) (n_length+int_length),
                        &comment_pos, (uint) com_length,
                        &vcol_screen_pos, vcol_screen_length,
                        NullS))

    goto err;

  if (interval_count)
  {
    if (!(interval_unescaped= (bool*) my_alloca(interval_count * sizeof(bool))))
      goto err;
    bzero(interval_unescaped, interval_count * sizeof(bool));
  }

  field_ptr= share->field;
  table_check_constraints= share->check_constraints;
  read_length=(uint) (share->fields * field_pack_length +
		      pos+ (uint) (n_length+int_length+com_length+
		                   vcol_screen_length));
  strpos= disk_buff+pos;

  if (!interval_count)
    share->intervals= 0;			// For better debugging

  share->vcol_defs.str= vcol_screen_pos;
  share->vcol_defs.length= vcol_screen_length;

  memcpy(names, strpos+(share->fields*field_pack_length), n_length+int_length);
  memcpy(comment_pos, disk_buff+read_length-com_length-vcol_screen_length, 
         com_length);
  memcpy(vcol_screen_pos, disk_buff+read_length-vcol_screen_length, 
         vcol_screen_length);

  if (fix_type_pointers(&interval_array, &typelib_value_lengths,
                        &share->fieldnames, 1, names, n_length) ||
      share->fieldnames.count != share->fields)
    goto err;

  if (fix_type_pointers(&interval_array, &typelib_value_lengths,
                        share->intervals, interval_count,
                        names + n_length, int_length))
    goto err;

  if (keynames.length &&
      (fix_type_pointers(&interval_array, &typelib_value_lengths,
                         &share->keynames, 1, keynames.str, keynames.length) ||
      share->keynames.count != keys))
    goto err;

  /* Allocate handler */
  if (!(handler_file= get_new_handler(share, thd->mem_root,
                                      plugin_hton(se_plugin))))
    goto err;

  if (handler_file->set_ha_share_ref(&share->ha_share))
    goto err;

  record= share->default_values-1;              /* Fieldstart = 1 */
  null_bits_are_used= share->null_fields != 0;
  if (share->null_field_first)
  {
    null_flags= null_pos= record+1;
    null_bit_pos= (db_create_options & HA_OPTION_PACK_RECORD) ? 0 : 1;
    /*
      null_bytes below is only correct under the condition that
      there are no bit fields.  Correct values is set below after the
      table struct is initialized
    */
    share->null_bytes= (share->null_fields + null_bit_pos + 7) / 8;
  }
#ifndef WE_WANT_TO_SUPPORT_VERY_OLD_FRM_FILES
  else
  {
    share->null_bytes= (share->null_fields+7)/8;
    null_flags= null_pos= record + 1 + share->reclength - share->null_bytes;
    null_bit_pos= 0;
  }
#endif

  use_hash= share->fields >= MAX_FIELDS_BEFORE_HASH;
  if (use_hash)
    use_hash= !my_hash_init(PSI_INSTRUMENT_ME, &share->name_hash,
                            system_charset_info, share->fields, 0, 0,
                            get_field_name, 0, 0);

  if (share->mysql_version >= 50700 && share->mysql_version < 100000 &&
      vcol_screen_length)
  {
    share->keep_original_mysql_version= 1;
    /*
      MySQL 5.7 stores the null bits for not stored fields last.
      Calculate the position for them.
    */
    mysql57_null_bits= 1;
    mysql57_vcol_null_pos= null_pos;
    mysql57_vcol_null_bit_pos= null_bit_pos;
    mysql57_calculate_null_position(share, &mysql57_vcol_null_pos,
                                    &mysql57_vcol_null_bit_pos,
                                    strpos, vcol_screen_pos);
  }

  /* Set system versioning information. */
  vers.name= Lex_ident(STRING_WITH_LEN("SYSTEM_TIME"));
  if (extra2.system_period.str == NULL)
  {
    versioned= VERS_UNDEFINED;
    vers.start_fieldno= 0;
    vers.end_fieldno= 0;
  }
  else
  {
    DBUG_PRINT("info", ("Setting system versioning information"));
    if (init_period_from_extra2(&vers, extra2.system_period.str,
                  extra2.system_period.str + extra2.system_period.length))
      goto err;
    DBUG_PRINT("info", ("Columns with system versioning: [%d, %d]",
                        vers.start_fieldno, vers.end_fieldno));
    versioned= VERS_TIMESTAMP;
    vers_can_native= handler_file->vers_can_native(thd);
    status_var_increment(thd->status_var.feature_system_versioning);
  } // if (system_period == NULL)

  if (extra2.application_period.str)
  {
    const uchar *pos= extra2.application_period.str;
    const uchar *end= pos + extra2.application_period.length;
    period.name.length= extra2_read_len(&pos, end);
    period.name.str= strmake_root(&mem_root, (char*)pos, period.name.length);
    pos+= period.name.length;

    period.constr_name.length= extra2_read_len(&pos, end);
    period.constr_name.str= strmake_root(&mem_root, (char*)pos,
                                         period.constr_name.length);
    pos+= period.constr_name.length;

    if (init_period_from_extra2(&period, pos, end))
      goto err;
    if (extra2_str_size(period.name.length)
         + extra2_str_size(period.constr_name.length)
         + 2 * frm_fieldno_size
        != extra2.application_period.length)
      goto err;
    status_var_increment(thd->status_var.feature_application_time_periods);
  }

  if (extra2.without_overlaps.str)
  {
    if (extra2.application_period.str == NULL)
      goto err;
    const uchar *key_pos= extra2.without_overlaps.str;
    period.unique_keys= read_frm_keyno(key_pos);
    for (uint k= 0; k < period.unique_keys; k++)
    {
      key_pos+= frm_keyno_size;
      uint key_nr= read_frm_keyno(key_pos);
      key_info[key_nr].without_overlaps= true;
    }

    if ((period.unique_keys + 1) * frm_keyno_size
        != extra2.without_overlaps.length)
      goto err;
  }

  if (extra2.field_data_type_info.length &&
      field_data_type_info_array.parse(old_root, share->fields,
                                       extra2.field_data_type_info))
    goto err;

  for (i=0 ; i < share->fields; i++, strpos+=field_pack_length, field_ptr++)
  {
    uint interval_nr= 0, recpos;
    LEX_CSTRING comment;
    LEX_CSTRING name;
    Virtual_column_info *vcol_info= 0;
    const Type_handler *handler;
    uint32 flags= 0;
    Column_definition_attributes attr;

    if (new_frm_ver >= 3)
    {
      /* new frm file in 4.1 */
      recpos=	    uint3korr(strpos+5);
      uint comment_length=uint2korr(strpos+15);

      if (!comment_length)
      {
	comment.str= (char*) "";
	comment.length=0;
      }
      else
      {
	comment.str=    (char*) comment_pos;
	comment.length= comment_length;
	comment_pos+=   comment_length;
      }

      if (strpos[13] == MYSQL_TYPE_VIRTUAL &&
          (share->mysql_version < 50600 || share->mysql_version >= 100000))
      {
        /*
          MariaDB 5.5 or 10.0 version.
          The interval_id byte in the .frm file stores the length of the
          expression statement for a virtual column.
        */
        uint vcol_info_length= (uint) strpos[12];

        if (!vcol_info_length) // Expect non-null expression
          goto err;

        attr.frm_unpack_basic(strpos);
        if (attr.frm_unpack_charset(share, strpos))
          goto err;
        /*
          Old virtual field information before 10.2

          Get virtual column data stored in the .frm file as follows:
          byte 1      = 1 | 2
          byte 2      = sql_type
          byte 3      = flags. 1 for stored_in_db
          [byte 4]    = optional interval_id for sql_type (if byte 1 == 2)
          next byte ...  = virtual column expression (text data)
        */

        vcol_info= new (&share->mem_root) Virtual_column_info();
        bool opt_interval_id= (uint)vcol_screen_pos[0] == 2;
        enum_field_types ftype= (enum_field_types) (uchar) vcol_screen_pos[1];
        if (!(handler= Type_handler::get_handler_by_real_type(ftype)))
          goto err;
        if (opt_interval_id)
          interval_nr= (uint)vcol_screen_pos[3];
        else if ((uint)vcol_screen_pos[0] != 1)
          goto err;
        bool stored= vcol_screen_pos[2] & 1;
        vcol_info->set_vcol_type(stored ? VCOL_GENERATED_STORED : VCOL_GENERATED_VIRTUAL);
        uint vcol_expr_length= vcol_info_length -
                              (uint)(FRM_VCOL_OLD_HEADER_SIZE(opt_interval_id));
        vcol_info->utf8= 0; // before 10.2.1 the charset was unknown
        int2store(vcol_screen_pos+1, vcol_expr_length); // for parse_vcol_defs()
        vcol_screen_pos+= vcol_info_length;
        share->virtual_fields++;
      }
      else
      {
        interval_nr=  (uint) strpos[12];
        enum_field_types field_type= (enum_field_types) strpos[13];
        if (!(handler= Type_handler::get_handler_by_real_type(field_type)))
        {
          if (field_type == 245 &&
              share->mysql_version >= 50700) // a.k.a MySQL 5.7 JSON
          {
            share->incompatible_version|= HA_CREATE_USED_ENGINE;
            const LEX_CSTRING mysql_json{STRING_WITH_LEN("MYSQL_JSON")};
            handler= Type_handler::handler_by_name_or_error(thd, mysql_json);
          }

          if (!handler)
            goto err; // Not supported field type
        }
        handler= handler->type_handler_frm_unpack(strpos);
        if (handler->Column_definition_attributes_frm_unpack(&attr, share,
                                                             strpos,
                                                             &extra2.gis))
          goto err;

        if (field_data_type_info_array.count())
        {
          const LEX_CSTRING &info= field_data_type_info_array.
                                     element(i).type_info();
          DBUG_EXECUTE_IF("frm_data_type_info",
            push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
            ER_UNKNOWN_ERROR, "DBUG: [%u] name='%s' type_info='%.*s'",
            i, share->fieldnames.type_names[i],
            (uint) info.length, info.str););

          if (info.length)
          {
            const Type_handler *h= Type_handler::handler_by_name_or_error(thd,
                                                                          info);
            /*
              This code will eventually be extended here:
              - If the handler was not found by name, we could
                still open the table using the fallback type handler "handler",
                at least for a limited set of commands.
              - If the handler was found by name, we could check
                that "h" and "handler" have the same type code
                (and maybe some other properties) to make sure
                that the FRM data is consistent.
            */
            if (!h)
              goto err;
            handler= h;
          }
        }
      }

      if (((uint) strpos[10]) & MYSQL57_GENERATED_FIELD)
      {
        attr.unireg_check= Field::NONE;

        /*
          MySQL 5.7 generated fields

          byte 1        = 1
          byte 2,3      = expr length
          byte 4        = stored_in_db
          byte 5..      = expr
        */
        if ((uint)(vcol_screen_pos)[0] != 1)
          goto err;
        vcol_info= new (&share->mem_root) Virtual_column_info();
        uint vcol_info_length= uint2korr(vcol_screen_pos + 1);
        if (!vcol_info_length) // Expect non-empty expression
          goto err;
        vcol_info->set_vcol_type(vcol_screen_pos[3] ? VCOL_GENERATED_STORED : VCOL_GENERATED_VIRTUAL);
        vcol_info->utf8= 0;
        vcol_screen_pos+= vcol_info_length + MYSQL57_GCOL_HEADER_SIZE;;
        share->virtual_fields++;
      }
    }
    else
    {
      attr.length= (uint) strpos[3];
      recpos=	    uint2korr(strpos+4),
      attr.pack_flag=    uint2korr(strpos+6);
      if (f_is_num(attr.pack_flag))
      {
        attr.decimals= f_decimals(attr.pack_flag);
        attr.pack_flag&= ~FIELDFLAG_DEC_MASK;
      }
      attr.pack_flag&=   ~FIELDFLAG_NO_DEFAULT;     // Safety for old files
      attr.unireg_check=  (Field::utype) MTYP_TYPENR((uint) strpos[8]);
      interval_nr=  (uint) strpos[10];

      /* old frm file */
      enum_field_types ftype= (enum_field_types) f_packtype(attr.pack_flag);
      if (!(handler= Type_handler::get_handler_by_real_type(ftype)))
        goto err; // Not supported field type

      if (f_is_binary(attr.pack_flag))
      {
        /*
          Try to choose the best 4.1 type:
          - for 4.0 "CHAR(N) BINARY" or "VARCHAR(N) BINARY" 
           try to find a binary collation for character set.
          - for other types (e.g. BLOB) just use my_charset_bin. 
        */
        if (!f_is_blob(attr.pack_flag))
        {
          // 3.23 or 4.0 string
          myf utf8_flag= thd->get_utf8_flag();
          if (!(attr.charset= get_charset_by_csname(share->table_charset->
                                                    cs_name.str,
                                                    MY_CS_BINSORT,
                                                    MYF(utf8_flag))))
            attr.charset= &my_charset_bin;
        }
      }
      else
        attr.charset= share->table_charset;
      bzero((char*) &comment, sizeof(comment));
      if ((!(handler= old_frm_type_handler(attr.pack_flag, interval_nr))))
        goto err; // Not supported field type
    }

    /* Remove >32 decimals from old files */
    if (share->mysql_version < 100200 &&
        (attr.pack_flag & FIELDFLAG_LONG_DECIMAL))
    {
      share->keep_original_mysql_version= 1;
      attr.pack_flag&= ~FIELDFLAG_LONG_DECIMAL;
    }

    if (interval_nr && attr.charset->mbminlen > 1 &&
        !interval_unescaped[interval_nr - 1])
    {
      /*
        Unescape UCS2/UTF16/UTF32 intervals from HEX notation.
        Note, ENUM/SET columns with equal value list share a single
        copy of TYPELIB. Unescape every TYPELIB only once.
      */
      TYPELIB *interval= share->intervals + interval_nr - 1;
      unhex_type2(interval);
      interval_unescaped[interval_nr - 1]= true;
    }

#ifndef TO_BE_DELETED_ON_PRODUCTION
    if (handler->real_field_type() == MYSQL_TYPE_NEWDECIMAL &&
        !share->mysql_version)
    {
      /*
        Fix pack length of old decimal values from 5.0.3 -> 5.0.4
        The difference is that in the old version we stored precision
        in the .frm table while we now store the display_length
      */
      uint decimals= f_decimals(attr.pack_flag);
      attr.length=
        my_decimal_precision_to_length((uint) attr.length, decimals,
                                       f_is_dec(attr.pack_flag) == 0);
      sql_print_error("Found incompatible DECIMAL field '%s' in %s; "
                      "Please do \"ALTER TABLE '%s' FORCE\" to fix it!",
                      share->fieldnames.type_names[i], share->table_name.str,
                      share->table_name.str);
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_CRASHED_ON_USAGE,
                          "Found incompatible DECIMAL field '%s' in %s; "
                          "Please do \"ALTER TABLE '%s' FORCE\" to fix it!",
                          share->fieldnames.type_names[i],
                          share->table_name.str,
                          share->table_name.str);
      share->crashed= 1;                        // Marker for CHECK TABLE
    }
#endif

    if (mysql57_null_bits && vcol_info && !vcol_info->is_stored())
    {
      swap_variables(uchar*, null_pos, mysql57_vcol_null_pos);
      swap_variables(uint, null_bit_pos, mysql57_vcol_null_bit_pos);
    }

    if (versioned)
    {
      if (i == vers.start_fieldno)
        flags|= VERS_ROW_START;
      else if (i == vers.end_fieldno)
        flags|= VERS_ROW_END;

      if (flags & VERS_SYSTEM_FIELD)
      {
        auto field_type= handler->real_field_type();

        DBUG_EXECUTE_IF("error_vers_wrong_type", field_type= MYSQL_TYPE_BLOB;);

        switch (field_type) {
        case MYSQL_TYPE_TIMESTAMP2:
          break;
        case MYSQL_TYPE_LONGLONG:
          if (vers_can_native)
          {
            versioned= VERS_TRX_ID;
            break;
          }
          /* Fallthrough */
        default:
          my_error(ER_VERS_FIELD_WRONG_TYPE,
                   (field_type == MYSQL_TYPE_LONGLONG ?
                    MYF(0) : MYF(ME_WARNING)),
                   fieldnames.type_names[i],
                   (versioned == VERS_TIMESTAMP ?
                    "TIMESTAMP(6)" : "BIGINT(20) UNSIGNED"),
                   table_name.str);
          goto err;
        }
      }
    }

    /* Convert pre-10.2.2 timestamps to use Field::default_value */
    name.str= fieldnames.type_names[i];
    name.length= strlen(name.str);
    attr.interval= interval_nr ? share->intervals + interval_nr - 1 : NULL;
    Record_addr addr(record + recpos, null_pos, null_bit_pos);
    *field_ptr= reg_field=
      attr.make_field(share, &share->mem_root, &addr, handler, &name, flags);
    if (!reg_field)				// Not supported field type
      goto err;

    if (attr.unireg_check == Field::TIMESTAMP_DNUN_FIELD ||
        attr.unireg_check == Field::TIMESTAMP_DN_FIELD)
    {
      reg_field->default_value= new (&share->mem_root) Virtual_column_info();
      reg_field->default_value->set_vcol_type(VCOL_DEFAULT);
      share->default_expressions++;
    }

    reg_field->field_index= i;
    reg_field->comment=comment;
    reg_field->vcol_info= vcol_info;
    reg_field->flags|= flags;
    if (extra2.field_flags.str)
    {
      uchar flags= *extra2.field_flags.str++;
      if (flags & VERS_OPTIMIZED_UPDATE)
        reg_field->flags|= VERS_UPDATE_UNVERSIONED_FLAG;

      reg_field->invisible= f_visibility(flags);
    }
    if (reg_field->invisible == INVISIBLE_USER)
      status_var_increment(thd->status_var.feature_invisible_columns);
    if (!reg_field->invisible)
      share->visible_fields++;
    if (handler->real_field_type() == MYSQL_TYPE_BIT &&
        !f_bit_as_char(attr.pack_flag))
    {
      null_bits_are_used= 1;
      if ((null_bit_pos+= (uint) (attr.length & 7)) > 7)
      {
        null_pos++;
        null_bit_pos-= 8;
      }
    }
    if (!(reg_field->flags & NOT_NULL_FLAG))
    {
      if (!(null_bit_pos= (null_bit_pos + 1) & 7))
        null_pos++;
    }

    if (vcol_info)
    {
      vcol_info->name= reg_field->field_name;
      if (mysql57_null_bits && !vcol_info->is_stored())
      {
        /* MySQL 5.7 has null bits last */
        swap_variables(uchar*, null_pos, mysql57_vcol_null_pos);
        swap_variables(uint, null_bit_pos, mysql57_vcol_null_bit_pos);
      }
    }

    if (f_no_default(attr.pack_flag))
      reg_field->flags|= NO_DEFAULT_VALUE_FLAG;

    if (reg_field->unireg_check == Field::NEXT_NUMBER)
      share->found_next_number_field= field_ptr;

    if (use_hash && my_hash_insert(&share->name_hash, (uchar*) field_ptr))
      goto err;
    if (!reg_field->stored_in_db())
    {
      share->stored_fields--;
      if (share->stored_rec_length>=recpos)
        share->stored_rec_length= recpos-1;
    }
    if (reg_field->has_update_default_function())
    {
      has_update_default_function= 1;
      if (!reg_field->default_value)
        share->default_fields++;
    }
  }
  *field_ptr=0;					// End marker
  /* Sanity checks: */
  DBUG_ASSERT(share->fields>=share->stored_fields);
  DBUG_ASSERT(share->reclength>=share->stored_rec_length);

  if (mysql57_null_bits)
  {
    /* We want to store the value for the last bits */
    swap_variables(uchar*, null_pos, mysql57_vcol_null_pos);
    swap_variables(uint, null_bit_pos, mysql57_vcol_null_bit_pos);
    DBUG_ASSERT((null_pos + (null_bit_pos + 7) / 8) <= share->field[0]->ptr);
  }

  share->primary_key= MAX_KEY;

  /* Fix key->name and key_part->field */
  if (key_parts)
  {
    keyinfo= share->key_info;
    uint hash_field_used_no= share->fields - hash_fields;
    KEY_PART_INFO *hash_keypart;
    Field *hash_field;
    uint offset= share->reclength - HA_HASH_FIELD_LENGTH * hash_fields;
    for (uint i= 0; i < share->keys; i++, keyinfo++)
    {
      /* We need set value in hash key_part */
      if (keyinfo->algorithm == HA_KEY_ALG_LONG_HASH)
      {
        share->long_unique_table= 1;
        hash_keypart= keyinfo->key_part + keyinfo->user_defined_key_parts;
        hash_keypart->length= HA_HASH_KEY_LENGTH_WITHOUT_NULL;
        hash_keypart->store_length= hash_keypart->length;
        hash_keypart->type= HA_KEYTYPE_ULONGLONG;
        hash_keypart->key_part_flag= 0;
        hash_keypart->key_type= 32834;
        /* Last n fields are unique_index_hash fields */
        hash_keypart->offset= offset;
        hash_keypart->fieldnr= hash_field_used_no + 1;
        hash_field= share->field[hash_field_used_no];
        hash_field->flags|= LONG_UNIQUE_HASH_FIELD;//Used in parse_vcol_defs
        DBUG_ASSERT(hash_field->invisible == INVISIBLE_FULL);
        keyinfo->flags|= HA_NOSAME;
        share->virtual_fields++;
        share->stored_fields--;
        if (record + share->stored_rec_length >= hash_field->ptr)
          share->stored_rec_length= (ulong)(hash_field->ptr - record - 1);
        hash_field_used_no++;
        offset+= HA_HASH_FIELD_LENGTH;
      }
    }
    longlong ha_option= handler_file->ha_table_flags();
    keyinfo= share->key_info;
    uint primary_key= my_strcasecmp(system_charset_info,
                                    share->keynames.type_names[0],
                                    primary_key_name.str) ? MAX_KEY : 0;
    KEY* key_first_info= NULL;

    if (primary_key >= MAX_KEY && keyinfo->flags & HA_NOSAME &&
        keyinfo->algorithm != HA_KEY_ALG_LONG_HASH)
    {
      /*
        If the UNIQUE key doesn't have NULL columns and is not a part key
        declare this as a primary key.
      */
      primary_key= 0;
      key_part= keyinfo->key_part;
      for (i=0 ; i < keyinfo->user_defined_key_parts ;i++)
      {
        DBUG_ASSERT(key_part[i].fieldnr > 0);
        // Table field corresponding to the i'th key part.
        Field *table_field= share->field[key_part[i].fieldnr - 1];

        /*
          If the key column is of NOT NULL BLOB type, then it
          will definitly have key prefix. And if key part prefix size
          is equal to the BLOB column max size, then we can promote
          it to primary key.
        */
        if (!table_field->real_maybe_null() &&
            table_field->type() == MYSQL_TYPE_BLOB &&
            table_field->field_length == key_part[i].length)
          continue;

        if (table_field->real_maybe_null() ||
            table_field->key_length() != key_part[i].length)
        {
          primary_key= MAX_KEY;		// Can't be used
          break;
        }
      }
    }

    /*
      Make sure that the primary key is not marked as IGNORE
      This can happen in the case
        1) when IGNORE is mentioned in the Key specification
        2) When a unique NON-NULLABLE key is promted to a primary key.
           The unqiue key could have been marked as IGNORE when there
           was a primary key in the table.

           Eg:
            CREATE TABLE t1(a INT NOT NULL, primary key(a), UNIQUE key1(a))
             so for this table when we try to IGNORE key1
             then we run:
                ALTER TABLE t1 ALTER INDEX key1 IGNORE
              this runs successsfully and key1 is marked as IGNORE.

              But lets say then we drop the primary key
               ALTER TABLE t1 DROP PRIMARY
              then the UNIQUE key will be promoted to become the primary key
              but then the UNIQUE key cannot be marked as IGNORE, so an
              error is thrown
    */
    if (primary_key != MAX_KEY && keyinfo && keyinfo->is_ignored)
    {
        my_error(ER_PK_INDEX_CANT_BE_IGNORED, MYF(0));
        goto err;
    }

    uint add_first_key_parts= 0;
    if (share->use_ext_keys)
    {
      if (primary_key >= MAX_KEY)
        share->set_use_ext_keys_flag(false);
      else
      {
        /* Add primary key to end of all non unique keys */

        KEY *curr_keyinfo= keyinfo, *keyinfo_end= keyinfo+ keys;
        KEY_PART_INFO *first_key_part= keyinfo->key_part;
        uint first_key_parts= keyinfo->user_defined_key_parts;

        /*
          We are skipping the first key (primary key) as it cannot be
          extended
        */
        while (++curr_keyinfo < keyinfo_end)
        {
          uint j;
          if (!(curr_keyinfo->flags & HA_NOSAME))
          {
            KEY_PART_INFO *key_part= (curr_keyinfo->key_part +
                                      curr_keyinfo->user_defined_key_parts);

            /* Extend key with primary key parts */
            for (j= 0;
                 j < first_key_parts &&
                   curr_keyinfo->ext_key_parts < MAX_REF_PARTS;
                 j++)
            {
              uint key_parts= curr_keyinfo->user_defined_key_parts;
              KEY_PART_INFO *curr_key_part= curr_keyinfo->key_part;
              KEY_PART_INFO *curr_key_part_end= curr_key_part+key_parts;

              for ( ; curr_key_part < curr_key_part_end; curr_key_part++)
              {
                if (curr_key_part->fieldnr == first_key_part[j].fieldnr)
                  break;
              }
              if (curr_key_part == curr_key_part_end)
              {
                /* Add primary key part not part of the current index */
                *key_part++= first_key_part[j];
                curr_keyinfo->ext_key_parts++;
                curr_keyinfo->ext_key_part_map|= 1 << j;
              }
            }
            if (j == first_key_parts)
            {
              /* Full primary key added to secondary keys makes it unique */
              curr_keyinfo->ext_key_flags= curr_keyinfo->flags | HA_EXT_NOSAME;
            }
          }
        }
        add_first_key_parts= keyinfo->user_defined_key_parts;

        /*
          If a primary key part is using a partial key, don't use it or any key part after
          it.
        */
        for (i= 0; i < first_key_parts; i++)
        {
          uint fieldnr= keyinfo[0].key_part[i].fieldnr;
          if (share->field[fieldnr-1]->key_length() !=
              keyinfo[0].key_part[i].length)
          {
            add_first_key_parts= i;
            break;
          }
        }
      }
    }

    /* Primary key must be set early as engine may use it in index_flag() */
    share->primary_key= (primary_key < MAX_KEY &&
                         share->keys_in_use.is_set(primary_key) ?
                         primary_key : MAX_KEY);

    key_first_info= keyinfo;
    for (uint key=0 ; key < keys ; key++,keyinfo++)
    {
      uint usable_parts= 0;
      keyinfo->name.str=    share->keynames.type_names[key];
      keyinfo->name.length= strlen(keyinfo->name.str);
      keyinfo->cache_name=
        (uchar*) alloc_root(&share->mem_root,
                            share->table_cache_key.length+
                            keyinfo->name.length + 1);
      if (keyinfo->cache_name)           // If not out of memory
      {
        uchar *pos= keyinfo->cache_name;
        memcpy(pos, share->table_cache_key.str, share->table_cache_key.length);
        memcpy(pos + share->table_cache_key.length, keyinfo->name.str,
               keyinfo->name.length+1);
      }

      if (ext_key_parts > share->key_parts && key)
      {
        KEY_PART_INFO *new_key_part= (keyinfo-1)->key_part +
                                     (keyinfo-1)->ext_key_parts;
        uint add_keyparts_for_this_key= add_first_key_parts;
        uint len_null_byte= 0, ext_key_length= 0;
        Field *field;

        if ((keyinfo-1)->algorithm == HA_KEY_ALG_LONG_HASH)
          new_key_part++; // reserved for the hash value

        /* 
          Do not extend the key that contains a component
          defined over the beginning of a field.
	*/ 
        for (i= 0; i < keyinfo->user_defined_key_parts; i++)
        {
          uint length_bytes= 0;
          uint fieldnr= keyinfo->key_part[i].fieldnr;
          field= share->field[fieldnr-1];

          if (field->null_ptr)
            len_null_byte= HA_KEY_NULL_LENGTH;

          if (keyinfo->algorithm != HA_KEY_ALG_LONG_HASH)
            length_bytes= field->key_part_length_bytes();

          ext_key_length+= keyinfo->key_part[i].length + len_null_byte
                            + length_bytes;
          if (field->key_length() != keyinfo->key_part[i].length)
	  {
            add_keyparts_for_this_key= 0;
            break;
          }
        }

        if (add_keyparts_for_this_key)
        {
          for (i= 0; i < add_keyparts_for_this_key; i++)
          {
            uint pk_part_length= key_first_info->key_part[i].store_length;
            if (keyinfo->ext_key_part_map & 1<<i)
            {
              if (ext_key_length + pk_part_length > MAX_DATA_LENGTH_FOR_KEY)
              {
                add_keyparts_for_this_key= i;
                break;
              }
              ext_key_length+= pk_part_length;
            }
          }
        }

        if (add_keyparts_for_this_key < keyinfo->ext_key_parts -
                                        keyinfo->user_defined_key_parts)
        {
          share->ext_key_parts-= keyinfo->ext_key_parts;
          key_part_map ext_key_part_map= keyinfo->ext_key_part_map;
          keyinfo->ext_key_parts= keyinfo->user_defined_key_parts;
          keyinfo->ext_key_flags= keyinfo->flags;
	  keyinfo->ext_key_part_map= 0; 
          for (i= 0; i < add_keyparts_for_this_key; i++)
	  {
            if (ext_key_part_map & 1<<i)
	    {
              keyinfo->ext_key_part_map|= 1<<i;
	      keyinfo->ext_key_parts++;
            }
          }
          share->ext_key_parts+= keyinfo->ext_key_parts;
        }
        if (new_key_part != keyinfo->key_part)
	{
          memmove(new_key_part, keyinfo->key_part,
                  sizeof(KEY_PART_INFO) * keyinfo->ext_key_parts);
          keyinfo->key_part= new_key_part;
        }
      }
 
      /* Fix fulltext keys for old .frm files */
      if (share->key_info[key].flags & HA_FULLTEXT)
	share->key_info[key].algorithm= HA_KEY_ALG_FULLTEXT;

      key_part= keyinfo->key_part;
      uint key_parts= share->use_ext_keys ? keyinfo->ext_key_parts :
	                                    keyinfo->user_defined_key_parts;
      if (keyinfo->algorithm == HA_KEY_ALG_LONG_HASH)
        key_parts++;
      for (i=0; i < key_parts; key_part++, i++)
      {
        Field *field;
	if (new_field_pack_flag <= 1)
	  key_part->fieldnr= find_field(share->field,
                                        share->default_values,
                                        (uint) key_part->offset,
                                        (uint) key_part->length);
	if (!key_part->fieldnr)
          goto err;

        field= key_part->field= share->field[key_part->fieldnr-1];
        if (Charset::collation_changed_order(share->mysql_version,
                                             field->charset()->number))
          share->incompatible_version|= HA_CREATE_USED_CHARSET;
        key_part->type= field->key_type();

        if (field->invisible > INVISIBLE_USER && !field->vers_sys_field())
          if (keyinfo->algorithm != HA_KEY_ALG_LONG_HASH)
            keyinfo->flags |= HA_INVISIBLE_KEY;
        if (field->null_ptr)
        {
          key_part->null_offset=(uint) ((uchar*) field->null_ptr -
                                        share->default_values);
          key_part->null_bit= field->null_bit;
          key_part->store_length+=HA_KEY_NULL_LENGTH;
          keyinfo->flags|=HA_NULL_PART_KEY;
          keyinfo->key_length+= HA_KEY_NULL_LENGTH;
        }

        key_part->key_part_flag|= field->key_part_flag();
        uint16 key_part_length_bytes= field->key_part_length_bytes();
        key_part->store_length+= key_part_length_bytes;
        if (i < keyinfo->user_defined_key_parts)
          keyinfo->key_length+= key_part_length_bytes;

        if (i == 0 && key != primary_key)
          field->flags |= (((keyinfo->flags & HA_NOSAME ||
                            keyinfo->algorithm == HA_KEY_ALG_LONG_HASH) &&
                           (keyinfo->user_defined_key_parts == 1)) ?
                           UNIQUE_KEY_FLAG : MULTIPLE_KEY_FLAG);
        if (i == 0)
          field->key_start.set_bit(key);
        if (field->key_length() == key_part->length &&
            !(field->flags & BLOB_FLAG) &&
            keyinfo->algorithm != HA_KEY_ALG_LONG_HASH)
        {
          if (handler_file->index_flags(key, i, 0) & HA_KEYREAD_ONLY)
          {
            share->keys_for_keyread.set_bit(key);
            /*
              part_of_key is used to check if we can use the field
              as part of covering key (which implies HA_KEYREAD_ONLY).
            */
            field->part_of_key.set_bit(key);
          }
          if (handler_file->index_flags(key, i, 1) & HA_READ_ORDER)
            field->part_of_sortkey.set_bit(key);

          if (i < keyinfo->user_defined_key_parts)
            field->part_of_key_not_clustered.set_bit(key);
        }
        if (!(key_part->key_part_flag & HA_REVERSE_SORT) &&
            usable_parts == i)
          usable_parts++;			// For FILESORT
        field->flags|= PART_KEY_FLAG;
        if (key == primary_key)
        {
          field->flags|= PRI_KEY_FLAG;
          /*
            If this field is part of the primary key and all keys contains
            the primary key, then we can use any key to find this column
          */
          if (ha_option & HA_PRIMARY_KEY_IN_READ_INDEX)
          {
            if (field->key_length() == key_part->length &&
                !(field->flags & BLOB_FLAG))
              field->part_of_key= share->keys_in_use;
            if (field->part_of_sortkey.is_set(key))
              field->part_of_sortkey= share->keys_in_use;
          }
        }
        if (field->key_length() != key_part->length)
        {
#ifndef TO_BE_DELETED_ON_PRODUCTION
          if (field->type() == MYSQL_TYPE_NEWDECIMAL &&
              keyinfo->algorithm != HA_KEY_ALG_LONG_HASH)
          {
            /*
              Fix a fatal error in decimal key handling that causes crashes
              on Innodb. We fix it by reducing the key length so that
              InnoDB never gets a too big key when searching.
              This allows the end user to do an ALTER TABLE to fix the
              error.
            */
            keyinfo->key_length-= (key_part->length - field->key_length());
            key_part->store_length-= (uint16)(key_part->length -
                                              field->key_length());
            key_part->length= (uint16)field->key_length();
            sql_print_error("Found wrong key definition in %s; "
                            "Please do \"ALTER TABLE '%s' FORCE \" to fix it!",
                            share->table_name.str,
                            share->table_name.str);
            push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                ER_CRASHED_ON_USAGE,
                                "Found wrong key definition in %s; "
                                "Please do \"ALTER TABLE '%s' FORCE\" to fix "
                                "it!",
                                share->table_name.str,
                                share->table_name.str);
            share->crashed= 1;                // Marker for CHECK TABLE
            continue;
          }
#endif
          key_part->key_part_flag|= HA_PART_KEY_SEG;
        }
        if (field->real_maybe_null())
          key_part->key_part_flag|= HA_NULL_PART;
        /*
          Sometimes we can compare key parts for equality with memcmp.
          But not always.
        */
        if (!(key_part->key_part_flag & (HA_BLOB_PART | HA_VAR_LENGTH_PART |
                                         HA_BIT_PART)) &&
            key_part->type != HA_KEYTYPE_FLOAT &&
            key_part->type == HA_KEYTYPE_DOUBLE &&
            keyinfo->algorithm != HA_KEY_ALG_LONG_HASH)
          key_part->key_part_flag|= HA_CAN_MEMCMP;
      }
      keyinfo->usable_key_parts= usable_parts; // Filesort

      set_if_bigger(share->max_key_length,keyinfo->key_length+
                    keyinfo->user_defined_key_parts);
      /*
        MERGE tables do not have unique indexes. But every key could be
        an unique index on the underlying MyISAM table. (Bug #10400)
      */
      if ((keyinfo->flags & HA_NOSAME) ||
          (ha_option & HA_ANY_INDEX_MAY_BE_UNIQUE))
        set_if_bigger(share->max_unique_length,keyinfo->key_length);
    }
    if (primary_key < MAX_KEY &&
	(share->keys_in_use.is_set(primary_key)))
    {
      DBUG_ASSERT(share->primary_key == primary_key);
      /*
	If we are using an integer as the primary key then allow the user to
	refer to it as '_rowid'
      */
      if (share->key_info[primary_key].user_defined_key_parts == 1)
      {
	Field *field= share->key_info[primary_key].key_part[0].field;
	if (field && field->result_type() == INT_RESULT)
        {
          /* note that fieldnr here (and rowid_field_offset) starts from 1 */
	  share->rowid_field_offset= (share->key_info[primary_key].key_part[0].
                                      fieldnr);
        }
      }
    }
    else
    {
      DBUG_ASSERT(share->primary_key == MAX_KEY);
    }
  }
  if (new_field_pack_flag <= 1)
  {
    /* Old file format with default as not null */
    uint null_length= (share->null_fields+7)/8;
    bfill(share->default_values + (null_flags - (uchar*) record),
          null_length, 255);
  }

  set_overlapped_keys();

  /* Handle virtual expressions */
  if (vcol_screen_length && share->frm_version >= FRM_VER_EXPRESSSIONS)
  {
    uchar *vcol_screen_end= vcol_screen_pos + vcol_screen_length;

    /* Skip header */
    vcol_screen_pos+= FRM_VCOL_NEW_BASE_SIZE;
    share->vcol_defs.str+= FRM_VCOL_NEW_BASE_SIZE;
    share->vcol_defs.length-= FRM_VCOL_NEW_BASE_SIZE;

    /*
      Read virtual columns, default values and check constraints
      See pack_expression() for how data is stored
    */
    while (vcol_screen_pos < vcol_screen_end)
    {
      Virtual_column_info *vcol_info;
      uint type=         (uint) vcol_screen_pos[0];
      uint field_nr=     uint2korr(vcol_screen_pos+1);
      uint expr_length=  uint2korr(vcol_screen_pos+3);
      uint name_length=  (uint) vcol_screen_pos[5];

      if (!(vcol_info=   new (&share->mem_root) Virtual_column_info()))
        goto err;

      /* The following can only be true for check_constraints */

      if (field_nr != UINT_MAX16)
      {
        DBUG_ASSERT(field_nr < share->fields);
        reg_field= share->field[field_nr];
      }
      else
      {
        reg_field= 0;
        DBUG_ASSERT(name_length);
      }

      vcol_screen_pos+= FRM_VCOL_NEW_HEADER_SIZE;
      vcol_info->set_vcol_type((enum_vcol_info_type) type);
      if (name_length)
      {
        vcol_info->name.str= strmake_root(&share->mem_root,
                                          (char*)vcol_screen_pos, name_length);
        vcol_info->name.length= name_length;
      }
      else
        vcol_info->name= reg_field->field_name;
      vcol_screen_pos+= name_length + expr_length;

      switch (type) {
      case VCOL_GENERATED_VIRTUAL:
      {
        uint recpos;
        reg_field->vcol_info= vcol_info;
        share->virtual_fields++;
        share->stored_fields--;
        if (reg_field->flags & BLOB_FLAG)
          share->virtual_not_stored_blob_fields++;
        if (reg_field->flags & PART_KEY_FLAG)
          vcol_info->set_vcol_type(VCOL_GENERATED_VIRTUAL_INDEXED);
        /* Correct stored_rec_length as non stored fields are last */
        recpos= (uint) (reg_field->ptr - record);
        if (share->stored_rec_length >= recpos)
          share->stored_rec_length= recpos-1;
        break;
      }
      case VCOL_GENERATED_STORED:
        DBUG_ASSERT(!reg_field->vcol_info);
        reg_field->vcol_info= vcol_info;
        share->virtual_fields++;
        break;
      case VCOL_DEFAULT:
        DBUG_ASSERT(!reg_field->default_value);
        reg_field->default_value=    vcol_info;
        share->default_expressions++;
        break;
      case VCOL_CHECK_FIELD:
        DBUG_ASSERT(!reg_field->check_constraint);
        reg_field->check_constraint= vcol_info;
        share->field_check_constraints++;
        break;
      case VCOL_CHECK_TABLE:
        *(table_check_constraints++)= vcol_info;
        break;
      }
    }
  }
  DBUG_ASSERT((uint) (table_check_constraints - share->check_constraints) ==
              (uint) (share->table_check_constraints -
                      share->field_check_constraints));

  if (options.str)
  {
    DBUG_ASSERT(options.length);
    if (engine_table_options_frm_read(options.str, options.length, share))
      goto err;
  }
  if (parse_engine_table_options(thd, handler_file->partition_ht(), share))
    goto err;

  if (share->found_next_number_field)
  {
    reg_field= *share->found_next_number_field;
    if ((int) (share->next_number_index= (uint)
	       find_ref_key(share->key_info, keys,
                            share->default_values, reg_field,
			    &share->next_number_key_offset,
                            &share->next_number_keypart)) < 0)
      goto err; // Wrong field definition
    reg_field->flags |= AUTO_INCREMENT_FLAG;
  }
  else
    share->next_number_index= MAX_KEY;

  if (share->blob_fields)
  {
    Field **ptr;
    uint k, *save;

    /* Store offsets to blob fields to find them fast */
    if (!(share->blob_field= save=
	  (uint*) alloc_root(&share->mem_root,
                             (uint) (share->blob_fields* sizeof(uint)))))
      goto err;
    for (k=0, ptr= share->field ; *ptr ; ptr++, k++)
    {
      if ((*ptr)->flags & BLOB_FLAG)
	(*save++)= k;
    }
  }

  /*
    the correct null_bytes can now be set, since bitfields have been taken
    into account
  */
  share->null_bytes= (uint)(null_pos - (uchar*) null_flags +
                      (null_bit_pos + 7) / 8);
  share->last_null_bit_pos= null_bit_pos;
  share->null_bytes_for_compare= null_bits_are_used ? share->null_bytes : 0;
  share->can_cmp_whole_record= (share->blob_fields == 0 &&
                                share->varchar_fields == 0);

  share->column_bitmap_size= bitmap_buffer_size(share->fields);

  bitmap_count= 1;
  if (share->table_check_constraints)
  {
    feature_check_constraint++;
    if (!(share->check_set= (MY_BITMAP*)
          alloc_root(&share->mem_root, sizeof(*share->check_set))))
      goto err;
    bitmap_count++;
  }
  if (!(bitmaps= (my_bitmap_map*) alloc_root(&share->mem_root,
                                             share->column_bitmap_size *
                                             bitmap_count)))
    goto err;
  my_bitmap_init(&share->all_set, bitmaps, share->fields);
  bitmap_set_all(&share->all_set);
  if (share->check_set)
  {
    /*
      Bitmap for fields used by CHECK constraint. Will be filled up
      at first usage of table.
    */
    my_bitmap_init(share->check_set,
                   (my_bitmap_map*) ((uchar*) bitmaps +
                                     share->column_bitmap_size),
                   share->fields);
    bitmap_clear_all(share->check_set);
  }

#ifndef DBUG_OFF
  if (use_hash)
    (void) my_hash_check(&share->name_hash);
#endif

  share->db_plugin= se_plugin;
  delete handler_file;

  share->error= OPEN_FRM_OK;
  thd->status_var.opened_shares++;
  thd->mem_root= old_root;
  my_afree(interval_unescaped);
  DBUG_RETURN(0);

err:
  if (frm_created)
  {
    char path[FN_REFLEN+1];
    strxnmov(path, FN_REFLEN, normalized_path.str, reg_ext, NullS);
    my_delete(path, MYF(0));
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (par_image)
    {
      strxnmov(path, FN_REFLEN, normalized_path.str, PAR_EXT, NullS);
      my_delete(path, MYF(0));
    }
#endif
  }
  share->db_plugin= NULL;
  share->error= OPEN_FRM_CORRUPTED;
  share->open_errno= my_errno;
  delete handler_file;
  plugin_unlock(0, se_plugin);
  my_hash_free(&share->name_hash);

  if (!thd->is_error())
    open_table_error(share, OPEN_FRM_CORRUPTED, share->open_errno);

  thd->mem_root= old_root;
  my_afree(interval_unescaped);
  DBUG_RETURN(HA_ERR_NOT_A_TABLE);
}


/*
  Make a copy of optimizer costs to be able to access these without any locks
  and to allow the engine to update costs.
*/

void TABLE_SHARE::update_optimizer_costs(handlerton *hton)
{
  if (hton != view_pseudo_hton && !(hton->flags & HTON_HIDDEN))
  {
    mysql_mutex_lock(&LOCK_optimizer_costs);
    memcpy(&optimizer_costs, hton->optimizer_costs, sizeof(optimizer_costs));
    mysql_mutex_unlock(&LOCK_optimizer_costs);
  }
  else
  {
    bzero(&optimizer_costs, sizeof(optimizer_costs));
    MEM_UNDEFINED(&optimizer_costs, sizeof(optimizer_costs));
  }
}


static bool sql_unusable_for_discovery(THD *thd, handlerton *engine,
                                       const char *sql)
{
  LEX *lex= thd->lex;
  HA_CREATE_INFO *create_info= &lex->create_info;

  // ... not CREATE TABLE
  if (lex->sql_command != SQLCOM_CREATE_TABLE &&
      lex->sql_command != SQLCOM_CREATE_SEQUENCE)
    return 1;
  // ... create like
  if (lex->create_info.like())
    return 1;
  // ... create select
  if (lex->first_select_lex()->item_list.elements)
    return 1;
  // ... temporary
  if (create_info->tmp_table())
    return 1;
  // ... if exists
  if (lex->create_info.if_not_exists())
    return 1;

  // XXX error out or rather ignore the following:
  // ... partitioning
  if (lex->part_info)
    return 1;
  // ... union
  if (create_info->used_fields & HA_CREATE_USED_UNION)
    return 1;
  // ... index/data directory
  if (create_info->data_file_name || create_info->index_file_name)
    return 1;
  // ... engine
  DBUG_ASSERT(lex->m_sql_cmd);
  if (lex->create_info.used_fields & HA_CREATE_USED_ENGINE)
  {
    /*
      TODO: we could just compare engine names here, without resolving.
      But this optimization is too late for 10.1.
    */
    Storage_engine_name *opt= lex->m_sql_cmd->option_storage_engine_name();
    DBUG_ASSERT(opt); // lex->m_sql_cmd must be an Sql_cmd_create_table instance
    if (opt->resolve_storage_engine_with_error(thd, &create_info->db_type,
                                               false) ||
        (create_info->db_type && create_info->db_type != engine))
      return 1;
  }
  // ... WITH SYSTEM VERSIONING
  if (create_info->versioned())
    return 1;

  return 0;
}

int TABLE_SHARE::init_from_sql_statement_string(THD *thd, bool write,
                                        const char *sql, size_t sql_length)
{
  CHARSET_INFO *old_cs= thd->variables.character_set_client;
  Parser_state parser_state;
  bool error;
  char *sql_copy;
  handler *file;
  LEX *old_lex;
  LEX tmp_lex;
  KEY *unused1;
  uint unused2;
  handlerton *hton= plugin_hton(db_plugin);
  LEX_CUSTRING frm= {0,0};
  LEX_CSTRING db_backup= thd->db;
  DBUG_ENTER("TABLE_SHARE::init_from_sql_statement_string");

  /*
    Ouch. Parser may *change* the string it's working on.
    Currently (2013-02-26) it is used to permanently disable
    conditional comments.
    Anyway, let's copy the caller's string...
  */
  if (!(sql_copy= thd->strmake(sql, sql_length)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  if (parser_state.init(thd, sql_copy, sql_length))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  Sql_mode_instant_set sms(thd, MODE_NO_ENGINE_SUBSTITUTION | MODE_NO_DIR_IN_CREATE);
  thd->variables.character_set_client= system_charset_info;
  tmp_disable_binlog(thd);
  old_lex= thd->lex;
  thd->lex= &tmp_lex;


  /*
    THD::reset_db() does not set THD::db_charset,
    so it keeps pointing to the character set and collation
    of the current database, rather than the database of the
    new initialized table. After reset_db() the result of
    get_default_db_collation() can be wrong. The latter is
    used inside charset_collation_context_create_table_in_db().
    Let's initialize ctx before calling reset_db().
    This makes sure the db.opt file to be loaded properly when needed.
  */
  Charset_collation_context
    ctx(thd->charset_collation_context_create_table_in_db(db.str));

  thd->reset_db(&db);
  lex_start(thd);

  if (unlikely((error= parse_sql(thd, & parser_state, NULL) ||
                sql_unusable_for_discovery(thd, hton, sql_copy))))
    goto ret;

  if (thd->lex->create_info.resolve_to_charset_collation_context(thd, ctx))
    DBUG_RETURN(true);

  tmp_lex.create_info.db_type= hton;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  thd->work_part_info= 0;                       // For partitioning
#endif

  if (tabledef_version.str)
    tmp_lex.create_info.tabledef_version= tabledef_version;

  tmp_lex.alter_info.db= db;
  tmp_lex.alter_info.table_name= table_name;
  promote_first_timestamp_column(&tmp_lex.alter_info.create_list);
  file= mysql_create_frm_image(thd, &tmp_lex.create_info, &tmp_lex.alter_info,
                               C_ORDINARY_CREATE, &unused1, &unused2, &frm);
  error|= file == 0;
  delete file;

  if (frm.str)
  {
    option_list= 0;             // cleanup existing options ...
    option_struct= 0;           // ... if it's an assisted discovery
    error= init_from_binary_frm_image(thd, write, frm.str, frm.length);
  }

ret:
  my_free(const_cast<uchar*>(frm.str));
  lex_end(&tmp_lex);
  thd->reset_db(&db_backup);
  thd->lex= old_lex;
  reenable_binlog(thd);
  thd->variables.character_set_client= old_cs;
  if (unlikely(thd->is_error() || error))
  {
    thd->clear_error();
    my_error(ER_SQL_DISCOVER_ERROR, MYF(0), hton_name(hton)->str,
             db.str, table_name.str, sql_copy);
    DBUG_RETURN(HA_ERR_GENERIC);
  }
  /* Treat the table as normal table from binary logging point of view */
  table_creation_was_logged= 1;
  DBUG_RETURN(0);
}

bool TABLE_SHARE::write_frm_image(const uchar *frm, size_t len)
{
  char file_name[FN_REFLEN+1];
  strxnmov(file_name, sizeof(file_name)-1, normalized_path.str, reg_ext,
           NullS);
  return writefile(file_name, db.str, table_name.str, false,
                   frm, len);
}

bool TABLE_SHARE::write_par_image(const uchar *par, size_t len)
{
  char file_name[FN_REFLEN+1];
  strxnmov(file_name, sizeof(file_name)-1, normalized_path.str, PAR_EXT,
           NullS);
  return writefile(file_name, db.str, table_name.str, false, par, len);
}


bool TABLE_SHARE::read_frm_image(const uchar **frm, size_t *len)
{
  if (IF_PARTITIONING(partition_info_str, 0))   // cannot discover a partition
  {
    DBUG_ASSERT(db_type()->discover_table == 0);
    return 1;
  }

  if (frm_image)
  {
    *frm= frm_image->str;
    *len= frm_image->length;
    frm_image->str= 0; // pass the ownership to the caller
    frm_image= 0;
    return 0;
  }
  return readfrm(normalized_path.str, frm, len);
}


void TABLE_SHARE::free_frm_image(const uchar *frm)
{
  if (frm)
    my_free(const_cast<uchar*>(frm));
}


bool Virtual_column_info::fix_expr(THD *thd)
{
  DBUG_ENTER("fix_vcol_expr");

  const enum enum_column_usage saved_column_usage= thd->column_usage;
  thd->column_usage= COLUMNS_WRITE;

  int error= expr->fix_fields(thd, &expr);

  thd->column_usage= saved_column_usage;

  if (unlikely(error))
  {
    StringBuffer<MAX_FIELD_WIDTH> str;
    print(&str);
    my_error(ER_ERROR_EVALUATING_EXPRESSION, MYF(0), str.c_ptr_safe());
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}

/** rerun fix_fields for vcols that returns time- or session- dependent values

    @note this is done for all vcols for INSERT/UPDATE/DELETE,
    and only as needed for SELECTs.
*/
bool Virtual_column_info::fix_session_expr(THD *thd)
{
  if (!need_refix())
    return false;

  DBUG_ASSERT(!expr->fixed());
  return fix_expr(thd);
}


bool Virtual_column_info::cleanup_session_expr()
{
  DBUG_ASSERT(need_refix());
  return expr->walk(&Item::cleanup_excluding_fields_processor, 0, 0);
}



class Vcol_expr_context
{
  bool inited;
  THD *thd;
  TABLE *table;
  Query_arena backup_arena, *stmt_arena;
  table_map old_map;
  Security_context *save_security_ctx;
  sql_mode_t save_sql_mode;

public:
  Vcol_expr_context(THD *_thd, TABLE *_table) :
    inited(false),
    thd(_thd),
    table(_table),
    stmt_arena(thd->stmt_arena),
    old_map(table->map),
    save_security_ctx(thd->security_ctx),
    save_sql_mode(thd->variables.sql_mode) {}
  bool init();

  ~Vcol_expr_context();
};


bool Vcol_expr_context::init()
{
  thd->variables.sql_mode= 0;

  TABLE_LIST const *tl= table->pos_in_table_list;
  DBUG_ASSERT(table->pos_in_table_list);

  if (table->pos_in_table_list->security_ctx)
    thd->security_ctx= tl->security_ctx;

  thd->set_n_backup_active_arena(table->expr_arena, &backup_arena);
  thd->stmt_arena= thd;

  inited= true;
  return false;
}

Vcol_expr_context::~Vcol_expr_context()
{
  if (!inited)
    return;
  table->map= old_map;
  thd->security_ctx= save_security_ctx;
  thd->restore_active_arena(table->expr_arena, &backup_arena);
  thd->variables.sql_mode= save_sql_mode;
  thd->stmt_arena= stmt_arena;
}


bool TABLE::check_sequence_privileges(THD *thd)
{
  if (internal_tables)
    for (Field **fp= field; *fp; fp++)
    {
      Virtual_column_info *vcol= (*fp)->default_value;
      if (vcol && vcol->check_access(thd))
        return 1;
    }
  return 0;
}


bool TABLE::vcol_fix_expr(THD *thd)
{
  if (pos_in_table_list->placeholder() || vcol_refix_list.is_empty())
    return false;

  if (!thd->stmt_arena->is_conventional() &&
      vcol_refix_list.head()->expr->fixed())
  {
    /* NOTE: Under trigger we already have fixed expressions */
    return false;
  }

  Vcol_expr_context expr_ctx(thd, this);
  if (expr_ctx.init())
    return true;

  List_iterator_fast<Virtual_column_info> it(vcol_refix_list);
  while (Virtual_column_info *vcol= it++)
    if (vcol->fix_session_expr(thd))
      goto error;

  return false;

error:
  DBUG_ASSERT(thd->get_stmt_da()->is_error());
  return true;
}


bool TABLE::vcol_cleanup_expr(THD *thd)
{
  if (vcol_refix_list.is_empty())
    return false;

  List_iterator<Virtual_column_info> it(vcol_refix_list);
  bool result= false;

  while (Virtual_column_info *vcol= it++)
    result|= vcol->cleanup_session_expr();

  DBUG_ASSERT(!result || thd->get_stmt_da()->is_error());
  return result;
}


/*
  @brief 
    Perform semantic analysis of the defining expression for a virtual column

  @param thd        The thread object
  @param table      The table containing the virtual column
  @param field	    Field if this is a DEFAULT or AS, otherwise NULL
  @param vcol       The Virtual_column object


  @details
    The function performs semantic analysis of the defining expression for
    the virtual column vcol_field. The expression is used to compute the
    values of this column.

  @retval
    TRUE           An error occurred, something was wrong with the function
  @retval
    FALSE          Otherwise
*/

bool Virtual_column_info::fix_and_check_expr(THD *thd, TABLE *table)
{
  DBUG_ENTER("Virtual_column_info::fix_and_check_expr");
  DBUG_PRINT("info", ("vcol: %p", this));
  DBUG_ASSERT(expr);

  /* NOTE: constants are fixed when constructed */
  if (expr->fixed())
    DBUG_RETURN(0); // nothing to do

  if (fix_expr(thd))
    DBUG_RETURN(1);

  if (flags)
    DBUG_RETURN(0); // already checked, no need to do it again


  /* this was checked in check_expression(), but the frm could be mangled... */
  if (unlikely(expr->result_type() == ROW_RESULT))
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), 1);
    DBUG_RETURN(1);
  }

  /*
    Walk through the Item tree checking if all items are valid
    to be part of the virtual column
  */
  Item::vcol_func_processor_result res;

  int error= expr->walk(&Item::check_vcol_func_processor, 0, &res);
  if (unlikely(error || (res.errors & VCOL_IMPOSSIBLE)))
  {
    // this can only happen if the frm was corrupted
    my_error(ER_VIRTUAL_COLUMN_FUNCTION_IS_NOT_ALLOWED, MYF(0), res.name,
             get_vcol_type_name(), name.str);
    DBUG_RETURN(1);
  }
  else if (res.errors & VCOL_AUTO_INC && vcol_type != VCOL_GENERATED_VIRTUAL)
  {
    /*
      An auto_increment field may not be used in an expression for
      a check constraint, a default value or a generated column

      Note that this error condition is not detected during parsing
      of the statement because the field item does not have a field
      pointer at that time
    */
    myf warn= table->s->frm_version < FRM_VER_EXPRESSSIONS ? ME_WARNING : 0;
    my_error(ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED, MYF(warn),
             "AUTO_INCREMENT", get_vcol_type_name(), res.name);
    if (!warn)
      DBUG_RETURN(1);
  }
  else if (vcol_type != VCOL_GENERATED_VIRTUAL && vcol_type != VCOL_DEFAULT &&
           res.errors & VCOL_NOT_STRICTLY_DETERMINISTIC)
  {
    my_error(ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED, MYF(0),
             res.name, get_vcol_type_name(), name.str);
    DBUG_RETURN(1);
  }
  flags= res.errors;

  if (!table->s->tmp_table && need_refix())
    table->vcol_refix_list.push_back(this, &table->mem_root);

  DBUG_RETURN(0);
}


bool Virtual_column_info::check_access(THD *thd)
{
  return flags & VCOL_NEXTVAL &&
         expr->walk(&Item::check_sequence_privileges, 0, thd);
}


/*
  @brief
    Unpack the definition of a virtual column from its linear representation

  @param thd             The thread object
  @param mem_root        Where to allocate memory
  @param table           The table containing the virtual column
  @param field           Field if this is a DEFAULT or AS, otherwise NULL
  @param vcol            The Virtual_column object
  @param[out] error_reported   Flag to inform the caller that no
                               other error messages are to be generated

  @details

    The function takes string expression from the 'vcol' object of the
    table 'table' and parses it, building an item object for it. The
    pointer to this item is placed into in a Virtual_column_info object
    that is created. After this the function performs
    semantic analysis of the item by calling the function
    fix_and_check_vcol_expr().  Since the defining expression is part of the table
    definition the item for it is created in table->memroot within the
    special arena TABLE::expr_arena or in the thd memroot for INSERT DELAYED

  @note
    Before passing 'vcol_expr' to the parser the function wraps it in
    parentheses and prepends a special keyword.
  
   @retval Virtual_column_info*   Success
   @retval NULL                   Error
*/

static Virtual_column_info *
unpack_vcol_info_from_frm(THD *thd, TABLE *table,
                          String *expr_str, Virtual_column_info **vcol_ptr,
                          bool *error_reported)
{
  Create_field vcol_storage; // placeholder for vcol_info
  Parser_state parser_state;
  Virtual_column_info *vcol= *vcol_ptr, *vcol_info= 0;
  LEX *old_lex= thd->lex;
  LEX lex;
  bool error;
  TABLE_LIST *sequence, *last;
  DBUG_ENTER("unpack_vcol_info_from_frm");

  DBUG_ASSERT(vcol->expr == NULL);
  
  if (parser_state.init(thd, expr_str->c_ptr_safe(), expr_str->length()))
    goto end;

  if (init_lex_with_single_table(thd, table, &lex))
    goto end;

  lex.parse_vcol_expr= true;
  lex.last_field= &vcol_storage;

  error= parse_sql(thd, &parser_state, NULL);
  if (unlikely(error))
    goto end;

  if ((sequence= lex.current_select->table_list.first[0].next_global))
  {
    /* We are using NEXT VALUE FOR sequence. Remember table for open */
    for (last= sequence ; last->next_global ; last= last->next_global)
      ;
    last->next_global= table->internal_tables;
    table->internal_tables= sequence;
  }

  vcol_storage.vcol_info->set_vcol_type(vcol->get_vcol_type());
  vcol_storage.vcol_info->name=              vcol->name;
  vcol_storage.vcol_info->utf8=              vcol->utf8;
  if (!vcol_storage.vcol_info->fix_and_check_expr(thd, table))
  {
    *vcol_ptr= vcol_info= vcol_storage.vcol_info;   // Expression ok
    DBUG_ASSERT(vcol_info->expr);
    goto end;
  }
  *error_reported= TRUE;

end:
  end_lex_with_single_table(thd, table, old_lex);

  DBUG_RETURN(vcol_info);
}

#ifndef DBUG_OFF
static void print_long_unique_table(TABLE *table)
{
  char buff[256];
  String str;
  KEY *key_info_table, *key_info_share;
  KEY_PART_INFO *key_part;
  Field *field;
  my_snprintf(buff, sizeof(buff), "Printing Table state, It will print table fields,"
          " fields->offset,field->null_bit, field->null_pos and key_info ... \n"
          "\nPrinting  Table  keyinfo\n");
  str.append(buff, strlen(buff));
  my_snprintf(buff, sizeof(buff), "\ntable->s->reclength %lu\n"
          "table->s->fields %u\n",
          table->s->reclength, table->s->fields);
  str.append(buff, strlen(buff));
  for (uint i= 0; i < table->s->keys; i++)
  {
    key_info_table= table->key_info + i;
    key_info_share= table->s->key_info + i;
    my_snprintf(buff, sizeof(buff),
                "\ntable->key_info[%u] user_defined_key_parts = %u\n"
                "table->key_info[%u] algorithm == HA_KEY_ALG_LONG_HASH = %d\n"
                "table->key_info[%u] flags & HA_NOSAME = %lu\n",
                i, key_info_table->user_defined_key_parts,
                i, key_info_table->algorithm == HA_KEY_ALG_LONG_HASH,
                i, key_info_table->flags & HA_NOSAME);
    str.append(buff, strlen(buff));
    my_snprintf(buff, sizeof(buff),
                "\ntable->s->key_info[%u] user_defined_key_parts = %u\n"
                "table->s->key_info[%u] algorithm == HA_KEY_ALG_LONG_HASH = %d\n"
                "table->s->key_info[%u] flags & HA_NOSAME = %lu\n",
                i, key_info_share->user_defined_key_parts,
                i, key_info_share->algorithm == HA_KEY_ALG_LONG_HASH,
                i, key_info_share->flags & HA_NOSAME);
    str.append(buff, strlen(buff));
    key_part = key_info_table->key_part;
    my_snprintf(buff, sizeof(buff),
                "\nPrinting table->key_info[%u].key_part[0] info\n"
                "key_part->offset = %u\n"
                "key_part->field_name = %s\n"
                "key_part->length = %u\n"
                "key_part->null_bit = %u\n"
                "key_part->null_offset = %u\n",
                i, key_part->offset, key_part->field->field_name.str, key_part->length,
                key_part->null_bit, key_part->null_offset);
    str.append(buff, strlen(buff));

    for (uint j= 0; j < key_info_share->user_defined_key_parts; j++)
    {
      key_part= key_info_share->key_part + j;
      my_snprintf(buff, sizeof(buff),
                  "\nPrinting share->key_info[%u].key_part[%u] info\n"
                  "key_part->offset = %u\n"
                  "key_part->field_name = %s\n"
                  "key_part->length = %u\n"
                  "key_part->null_bit = %u\n"
                  "key_part->null_offset = %u\n",
                  i, j, key_part->offset, key_part->field->field_name.str,
                  key_part->length, key_part->null_bit, key_part->null_offset);
      str.append(buff, strlen(buff));
    }
  }
  my_snprintf(buff, sizeof(buff), "\nPrinting table->fields\n");
  str.append(buff, strlen(buff));
  for(uint i= 0; i < table->s->fields; i++)
  {
    field= table->field[i];
    my_snprintf(buff, sizeof(buff),
                "\ntable->field[%u]->field_name %s\n"
                "table->field[%u]->offset = %" PRIdPTR "\n" // `%td` not available
                "table->field[%u]->field_length = %d\n"
                "table->field[%u]->null_pos wrt to record 0 = %" PRIdPTR "\n"
                "table->field[%u]->null_bit_pos = %d",
                i, field->field_name.str,
                i, field->ptr- table->record[0],
                i, field->pack_length(),
                i, field->null_bit ? field->null_ptr - table->record[0] : -1,
                i, field->null_bit);
    str.append(buff, strlen(buff));
  }
  (*error_handler_hook)(1, str.ptr(), ME_NOTE);
}
#endif


/**
   Copy key information from TABLE_SHARE to TABLE
*/

bool copy_keys_from_share(TABLE *outparam, MEM_ROOT *root)
{
  TABLE_SHARE *share= outparam->s;
  if (share->key_parts)
  {
    KEY	*key_info, *key_info_end;
    KEY_PART_INFO *key_part;

    if (!multi_alloc_root(root, &key_info, share->keys*sizeof(KEY),
                          &key_part,
                          share->ext_key_parts*sizeof(KEY_PART_INFO),
                          NullS))
      return 1;

    outparam->key_info= key_info;

    memcpy(key_info, share->key_info, sizeof(*key_info)*share->keys);
    memcpy(key_part, key_info->key_part,
           sizeof(*key_part)*share->ext_key_parts);

    my_ptrdiff_t adjust_ptrs= PTR_BYTE_DIFF(key_part, key_info->key_part);
    for (key_info_end= key_info + share->keys ;
         key_info < key_info_end ;
         key_info++)
    {
      key_info->table= outparam;
      key_info->key_part= reinterpret_cast<KEY_PART_INFO*>
        (reinterpret_cast<char*>(key_info->key_part) + adjust_ptrs);
      if (key_info->algorithm == HA_KEY_ALG_LONG_HASH)
      {
        /*
          From the user point of view, this key is unique.
          However from the engine point, the value is not unique
          as there can be hash collisions.
        */
        key_info->flags&= ~HA_NOSAME;
      }
    }

    /*
      We have to copy key parts separately as LONG HASH has invisible
      key parts not seen by key_info
    */
    for (KEY_PART_INFO *key_part_end= key_part+share->ext_key_parts;
         key_part < key_part_end;
         key_part++)
    {
      /*
        key_part->field is not set for key_parts that are here not used.
        This can happen with extended keys where a secondary key
        contains a primary key.  In this case no key_info will contain
        this key_part, but it can still be part of the memory region of
        share->key_part.
      */
      if (key_part->field)
      {
        Field *field= key_part->field= outparam->field[key_part->fieldnr - 1];
        if (field->key_length() != key_part->length &&
            !(field->flags & BLOB_FLAG))
        {
          /*
            We are using only a prefix of the column as a key:
            Create a new field for the key part that matches the index
          */
          field= key_part->field=field->make_new_field(root, outparam, 0);
          field->field_length= key_part->length;
        }
      }
    }
  }
  return 0;
}

void TABLE::update_keypart_vcol_info()
{
  for (uint k= 0; k < s->keys; k++)
  {
    KEY &info_k= key_info[k];
    uint parts = (s->use_ext_keys ? info_k.ext_key_parts :
                      info_k.user_defined_key_parts);
    for (uint p= 0; p < parts; p++)
    {
      KEY_PART_INFO &kp= info_k.key_part[p];
      if (kp.field != field[kp.fieldnr - 1])
      {
        kp.field->vcol_info = field[kp.fieldnr - 1]->vcol_info;
      }
    }
  }
}

/*
  Open a table based on a TABLE_SHARE

  SYNOPSIS
    open_table_from_share()
    thd			Thread handler
    share		Table definition
    alias       	Alias for table
    db_stat		open flags (for example HA_OPEN_KEYFILE|
    			HA_OPEN_RNDFILE..) can be 0 (example in
                        ha_example_table)
    prgflag   		READ_ALL etc..
    ha_open_flags	HA_OPEN_ABORT_IF_LOCKED etc..
    outparam       	result table
    partitions_to_open  open only these partitions.

  RETURN VALUES
   0	ok
   1	Error (see open_table_error)
   2    Error (see open_table_error)
   3    Wrong data in .frm file
   4    Error (see open_table_error)
   5    Error (see open_table_error: charset unavailable)
   7    Table definition has changed in engine
*/

enum open_frm_error open_table_from_share(THD *thd, TABLE_SHARE *share,
                       const LEX_CSTRING *alias, uint db_stat, uint prgflag,
                       uint ha_open_flags, TABLE *outparam,
                       bool is_create_table, List<String> *partitions_to_open)
{
  enum open_frm_error error;
  uint records, i, bitmap_size, bitmap_count;
  const char *tmp_alias;
  bool error_reported= FALSE;
  uchar *record, *bitmaps;
  Field **field_ptr;
  uint8 save_context_analysis_only= thd->lex->context_analysis_only;
  TABLE_SHARE::enum_v_keys check_set_initialized= share->check_set_initialized;
  DBUG_ENTER("open_table_from_share");
  DBUG_PRINT("enter",("name: '%s.%s'  form: %p", share->db.str,
                      share->table_name.str, outparam));

  thd->lex->context_analysis_only&= ~CONTEXT_ANALYSIS_ONLY_VIEW; // not a view

  error= OPEN_FRM_ERROR_ALREADY_ISSUED; // for OOM errors below
  bzero((char*) outparam, sizeof(*outparam));
  outparam->in_use= thd;
  outparam->s= share;
  outparam->db_stat= db_stat;
  outparam->status= STATUS_NO_RECORD;

  if (share->incompatible_version &&
      !(ha_open_flags & (HA_OPEN_FOR_ALTER | HA_OPEN_FOR_REPAIR |
                         HA_OPEN_FOR_FLUSH)))
  {
    /* one needs to run mysql_upgrade on the table */
    error= OPEN_FRM_NEEDS_REBUILD;
    goto err;
  }
  init_sql_alloc(key_memory_TABLE, &outparam->mem_root, TABLE_ALLOC_BLOCK_SIZE,
                 TABLE_PREALLOC_BLOCK_SIZE, MYF(0));

  /*
    We have to store the original alias in mem_root as constraints and virtual
    functions may store pointers to it
  */
  if (!(tmp_alias= strmake_root(&outparam->mem_root, alias->str, alias->length)))
    goto err;

  outparam->alias.set(tmp_alias, alias->length, table_alias_charset);
  outparam->vcol_refix_list.empty();

  /* Allocate handler */
  outparam->file= 0;
  if (!(prgflag & OPEN_FRM_FILE_ONLY))
  {
    if (!(outparam->file= get_new_handler(share, &outparam->mem_root,
                                          share->db_type())))
      goto err;

    if (outparam->file->set_ha_share_ref(&share->ha_share))
      goto err;
  }
  else
  {
    DBUG_ASSERT(!db_stat);
  }

  if (share->sequence && outparam->file)
  {
    ha_sequence *file;
    /* SEQUENCE table. Create a sequence handler over the original handler */
    if (!(file= (ha_sequence*) sql_sequence_hton->create(sql_sequence_hton, share,
                                                     &outparam->mem_root)))
      goto err;
    file->register_original_handler(outparam->file);
    outparam->file= file;
  }

  outparam->reginfo.lock_type= TL_UNLOCK;
  outparam->reginfo.skip_locked= false;
  outparam->current_lock= F_UNLCK;
  records=0;
  if ((db_stat & HA_OPEN_KEYFILE) || (prgflag & DELAYED_OPEN))
    records=1;
  if (prgflag & (READ_ALL + EXTRA_RECORD))
  {
    records++;
    if (share->versioned || share->period.name)
      records++;
  }

  if (records == 0)
  {
    /* We are probably in hard repair, and the buffers should not be used */
    record= share->default_values;
  }
  else
  {
    if (!(record= (uchar*) alloc_root(&outparam->mem_root,
                                      share->rec_buff_length * records)))
      goto err;                                   /* purecov: inspected */
  }

  for (i= 0; i < 3;)
  {
    outparam->record[i]= record;
    if (++i < records)
      record+= share->rec_buff_length;
  }
  /* Mark bytes between records as not accessable to catch overrun bugs */
  for (i= 0; i < records; i++)
    MEM_NOACCESS(outparam->record[i] + share->reclength,
                 share->rec_buff_length - share->reclength);

  if (!(field_ptr = (Field **) alloc_root(&outparam->mem_root,
                                          (uint) ((share->fields+1)*
                                                  sizeof(Field*)))))
    goto err;                                   /* purecov: inspected */

  /* Allocate storage for range optimizer */
  if (!multi_alloc_root(&outparam->mem_root,
                        &outparam->opt_range,
                        share->keys * sizeof(TABLE::OPT_RANGE),
                        &outparam->const_key_parts,
                        share->keys * sizeof(key_part_map),
                        NullS))
    goto err;

  outparam->field= field_ptr;

  record= (uchar*) outparam->record[0]-1;	/* Fieldstart = 1 */
  if (share->null_field_first)
    outparam->null_flags= (uchar*) record+1;
  else
    outparam->null_flags= (uchar*) (record+ 1+ share->reclength -
                                    share->null_bytes);

  /* Setup copy of fields from share, but use the right alias and record */
  for (i=0 ; i < share->fields; i++, field_ptr++)
  {
    if (!((*field_ptr)= share->field[i]->clone(&outparam->mem_root, outparam)))
      goto err;
  }
  (*field_ptr)= 0;                              // End marker

  DEBUG_SYNC(thd, "TABLE_after_field_clone");

  outparam->vers_write= share->versioned;

  if (share->found_next_number_field)
    outparam->found_next_number_field=
      outparam->field[(uint) (share->found_next_number_field - share->field)];

  if (copy_keys_from_share(outparam, &outparam->mem_root))
    goto err;

  /*
    Process virtual and default columns, if any.
  */
  if (share->virtual_fields || share->default_fields ||
      share->default_expressions || share->table_check_constraints)
  {
    Field **vfield_ptr, **dfield_ptr;
    Virtual_column_info **check_constraint_ptr;

    if (!multi_alloc_root(&outparam->mem_root,
                          &vfield_ptr, (uint) ((share->virtual_fields + 1)*
                                               sizeof(Field*)),
                          &dfield_ptr, (uint) ((share->default_fields +
                                                share->default_expressions +1)*
                                               sizeof(Field*)),
                          &check_constraint_ptr,
                          (uint) ((share->table_check_constraints +
                                   share->field_check_constraints + 1)*
                                  sizeof(Virtual_column_info*)),
                          NullS))
      goto err;
    if (share->virtual_fields)
      outparam->vfield= vfield_ptr;
    if (share->default_fields + share->default_expressions)
      outparam->default_field= dfield_ptr;
    if (share->table_check_constraints || share->field_check_constraints)
      outparam->check_constraints= check_constraint_ptr;

    vcol_init_mode mode= VCOL_INIT_DEPENDENCY_FAILURE_IS_WARNING;
    switch (thd->lex->sql_command)
    {
    case SQLCOM_CREATE_TABLE:
      mode= VCOL_INIT_DEPENDENCY_FAILURE_IS_ERROR;
      break;
    case SQLCOM_ALTER_TABLE:
    case SQLCOM_CREATE_INDEX:
    case SQLCOM_DROP_INDEX:
      if ((ha_open_flags & HA_OPEN_FOR_ALTER) == 0)
        mode= VCOL_INIT_DEPENDENCY_FAILURE_IS_ERROR;
      break;
    default:
      break;
    }

    if (parse_vcol_defs(thd, &outparam->mem_root, outparam,
                        &error_reported, mode))
    {
      error= OPEN_FRM_CORRUPTED;
      goto err;
    }

    /* Update to use trigger fields */
    switch_defaults_to_nullable_trigger_fields(outparam);

    outparam->update_keypart_vcol_info();
  }

#ifdef WITH_PARTITION_STORAGE_ENGINE
  bool work_part_info_used;
  if (share->partition_info_str_len && outparam->file)
  {
  /*
    In this execution we must avoid calling thd->change_item_tree since
    we might release memory before statement is completed. We do this
    by changing to a new statement arena. As part of this arena we also
    set the memory root to be the memory root of the table since we
    call the parser and fix_fields which both can allocate memory for
    item objects. We keep the arena to ensure that we can release the
    free_list when closing the table object.
    SEE Bug #21658
  */

    Query_arena *backup_stmt_arena_ptr= thd->stmt_arena;
    Query_arena backup_arena;
    Query_arena part_func_arena(&outparam->mem_root,
                                Query_arena::STMT_INITIALIZED);
    thd->set_n_backup_active_arena(&part_func_arena, &backup_arena);
    thd->stmt_arena= &part_func_arena;
    bool tmp;

    tmp= mysql_unpack_partition(thd, share->partition_info_str,
                                share->partition_info_str_len,
                                outparam, is_create_table,
                                plugin_hton(share->default_part_plugin),
                                &work_part_info_used);
    if (tmp)
    {
      thd->stmt_arena= backup_stmt_arena_ptr;
      thd->restore_active_arena(&part_func_arena, &backup_arena);
      goto partititon_err;
    }
    if (parse_engine_part_options(thd, outparam))
      goto err;
    outparam->part_info->is_auto_partitioned= share->auto_partitioned;
    DBUG_PRINT("info", ("autopartitioned: %u", share->auto_partitioned));
    /* 
      We should perform the fix_partition_func in either local or
      caller's arena depending on work_part_info_used value.
    */
    if (!work_part_info_used)
      tmp= fix_partition_func(thd, outparam, is_create_table);
    thd->stmt_arena= backup_stmt_arena_ptr;
    thd->restore_active_arena(&part_func_arena, &backup_arena);
    if (!tmp)
    {
      if (work_part_info_used)
        tmp= fix_partition_func(thd, outparam, is_create_table);
    }
    outparam->part_info->item_free_list= part_func_arena.free_list;
partititon_err:
    if (tmp)
    {
      if (is_create_table)
      {
        /*
          During CREATE/ALTER TABLE it is ok to receive errors here.
          It is not ok if it happens during the opening of an frm
          file as part of a normal query.
        */
        error_reported= TRUE;
      }
      goto err;
    }
  }
#endif

  /* Check virtual columns against table's storage engine. */
  if (share->virtual_fields &&
        (outparam->file && 
          !(outparam->file->ha_table_flags() & HA_CAN_VIRTUAL_COLUMNS)))
  {
    my_error(ER_UNSUPPORTED_ENGINE_FOR_VIRTUAL_COLUMNS, MYF(0),
             plugin_name(share->db_plugin)->str);
    error_reported= TRUE;
    goto err;
  }

  /* Allocate bitmaps */

  bitmap_size= share->column_bitmap_size;
  bitmap_count= 7;
  if (share->virtual_fields)
    bitmap_count++;

  if (!(bitmaps= (uchar*) alloc_root(&outparam->mem_root,
                                     bitmap_size * bitmap_count)))
    goto err;

  my_bitmap_init(&outparam->def_read_set,
                 (my_bitmap_map*) bitmaps, share->fields);
  bitmaps+= bitmap_size;
  my_bitmap_init(&outparam->def_write_set,
                 (my_bitmap_map*) bitmaps, share->fields);
  bitmaps+= bitmap_size;

  my_bitmap_init(&outparam->has_value_set,
                 (my_bitmap_map*) bitmaps, share->fields);
  bitmaps+= bitmap_size;
  my_bitmap_init(&outparam->tmp_set,
                 (my_bitmap_map*) bitmaps, share->fields);
  bitmaps+= bitmap_size;
  my_bitmap_init(&outparam->eq_join_set,
                 (my_bitmap_map*) bitmaps, share->fields);
  bitmaps+= bitmap_size;
  my_bitmap_init(&outparam->cond_set,
                 (my_bitmap_map*) bitmaps, share->fields);
  bitmaps+= bitmap_size;
  my_bitmap_init(&outparam->def_rpl_write_set,
                 (my_bitmap_map*) bitmaps, share->fields);
  outparam->default_column_bitmaps();

  outparam->cond_selectivity= 1.0;

  /* The table struct is now initialized;  Open the table */
  if (db_stat)
  {
    if (specialflag & SPECIAL_WAIT_IF_LOCKED)
      ha_open_flags|= HA_OPEN_WAIT_IF_LOCKED;
    else
      ha_open_flags|= HA_OPEN_IGNORE_IF_LOCKED;

    int ha_err= outparam->file->ha_open(outparam, share->normalized_path.str,
                                 (db_stat & HA_READ_ONLY ? O_RDONLY : O_RDWR),
                                 ha_open_flags, 0, partitions_to_open);
    if (ha_err)
    {
      share->open_errno= ha_err;
      /* Set a flag if the table is crashed and it can be auto. repaired */
      share->crashed= (outparam->file->auto_repair(ha_err) &&
                       !(ha_open_flags & HA_OPEN_FOR_REPAIR));
      if (!thd->is_error())
        outparam->file->print_error(ha_err, MYF(0));
      error_reported= TRUE;

      if (ha_err == HA_ERR_TABLE_DEF_CHANGED)
        error= OPEN_FRM_DISCOVER;

      /*
        We're here, because .frm file was successfully opened.

        But if the table doesn't exist in the engine and the engine
        supports discovery, we force rediscover to discover
        the fact that table doesn't in fact exist and remove
        the stray .frm file.
      */
      if (outparam->file->partition_ht()->discover_table &&
          (ha_err == ENOENT || ha_err == HA_ERR_NO_SUCH_TABLE))
        error= OPEN_FRM_DISCOVER;

      goto err;
    }
  }

  outparam->mark_columns_used_by_virtual_fields();
  if (!check_set_initialized &&
      share->check_set_initialized == TABLE_SHARE::V_KEYS)
  {
    // copy PART_INDIRECT_KEY_FLAG that was set meanwhile by *some* thread
    for (uint i= 0 ; i < share->fields ; i++)
    {
      if (share->field[i]->flags & PART_INDIRECT_KEY_FLAG)
        outparam->field[i]->flags|= PART_INDIRECT_KEY_FLAG;
    }
  }

  if (db_stat)
  {
    /* Set some flags in share on first open of the table */
    handler::Table_flags flags= outparam->file->ha_table_flags();
    if (! MY_TEST(flags & (HA_BINLOG_STMT_CAPABLE |
                           HA_BINLOG_ROW_CAPABLE)) ||
        MY_TEST(flags & HA_HAS_OWN_BINLOGGING))
      share->no_replicate= TRUE;
    if (outparam->file->table_cache_type() & HA_CACHE_TBL_NOCACHE)
      share->not_usable_by_query_cache= TRUE;
    if (outparam->file->ha_table_flags() & HA_CAN_ONLINE_BACKUPS)
      share->online_backup= 1;
  }

  if (share->no_replicate || !binlog_filter->db_ok(share->db.str))
    share->can_do_row_logging= 0;   // No row based replication

  /* Increment the opened_tables counter, only when open flags set. */
  if (db_stat)
    thd->status_var.opened_tables++;

  thd->lex->context_analysis_only= save_context_analysis_only;
  DBUG_EXECUTE_IF("print_long_unique_internal_state",
                  print_long_unique_table(outparam););
  DBUG_RETURN(OPEN_FRM_OK);

 err:
  if (! error_reported)
    open_table_error(share, error, my_errno);
  delete outparam->file;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (outparam->part_info)
    free_items(outparam->part_info->item_free_list);
#endif
  outparam->file= 0;				// For easier error checking
  outparam->db_stat=0;
  thd->lex->context_analysis_only= save_context_analysis_only;
  if (outparam->expr_arena)
    outparam->expr_arena->free_items();
  free_root(&outparam->mem_root, MYF(0));       // Safe to call on bzero'd root
  outparam->alias.free();
  DBUG_RETURN (error);
}


/**
  Free engine stats

  This is only called from closefrm() when the TABLE object is destroyed
**/

void TABLE::free_engine_stats()
{
  bool free_stats= 0;
  TABLE_STATISTICS_CB *stats= stats_cb;
  mysql_mutex_lock(&s->LOCK_share);
  free_stats= --stats->usage_count == 0;
  mysql_mutex_unlock(&s->LOCK_share);
  if (free_stats)
    delete stats;
}


/*
  Use engine stats from table_share if table_share has been updated
*/

void TABLE::update_engine_independent_stats()
{
  bool free_stats= 0;
  TABLE_STATISTICS_CB *org_stats= stats_cb;
  DBUG_ASSERT(stats_cb != s->stats_cb);

  if (stats_cb != s->stats_cb)
  {
    mysql_mutex_lock(&s->LOCK_share);
    if (org_stats)
      free_stats= --org_stats->usage_count == 0;
    if ((stats_cb= s->stats_cb))
      stats_cb->usage_count++;
    mysql_mutex_unlock(&s->LOCK_share);
    if (free_stats)
      delete org_stats;
  }
}


/*
  Update engine stats in table share to use new stats
*/

void
TABLE_SHARE::update_engine_independent_stats(TABLE_STATISTICS_CB *new_stats)
{
  TABLE_STATISTICS_CB *free_stats= 0;
  DBUG_ASSERT(new_stats->usage_count == 0);

  mysql_mutex_lock(&LOCK_share);
  if (stats_cb)
  {
    if (!--stats_cb->usage_count)
      free_stats= stats_cb;
  }
  stats_cb= new_stats;
  new_stats->usage_count++;
  mysql_mutex_unlock(&LOCK_share);
  if (free_stats)
    delete free_stats;
}


/* Check if we have statistics for histograms */

bool TABLE_SHARE::histograms_exists()
{
  bool res= 0;
  if (stats_cb)
  {
    mysql_mutex_lock(&LOCK_share);
    if (stats_cb)
      res= stats_cb->histograms_exists();
    mysql_mutex_unlock(&LOCK_share);
  }
  return res;
}


/*
  Free information allocated by openfrm

  SYNOPSIS
    closefrm()
    table		TABLE object to free
*/

int closefrm(TABLE *table)
{
  int error=0;
  DBUG_ENTER("closefrm");
  DBUG_PRINT("enter", ("table: %p", table));

  if (table->db_stat)
    error=table->file->ha_close();
  table->alias.free();
  if (table->expr_arena)
    table->expr_arena->free_items();
  if (table->field)
  {
    for (Field **ptr=table->field ; *ptr ; ptr++)
    {
      delete *ptr;
    }
    table->field= 0;
  }
  delete table->file;
  table->file= 0;				/* For easier errorchecking */
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (table->part_info)
  {
    /* Allocated through table->mem_root, freed below */
    free_items(table->part_info->item_free_list);
    table->part_info->item_free_list= 0;
    table->part_info= 0;
  }
#endif
  if (table->stats_cb)
  {
    DBUG_ASSERT(table->s->tmp_table == NO_TMP_TABLE);
    table->free_engine_stats();
  }

  free_root(&table->mem_root, MYF(0));
  DBUG_RETURN(error);
}


/* Deallocate temporary blob storage */

void free_blobs(TABLE *table)
{
  uint *ptr, *end;
  for (ptr= table->s->blob_field, end=ptr + table->s->blob_fields ;
       ptr != end ;
       ptr++)
  {
    /*
      Reduced TABLE objects which are used by row-based replication for
      type conversion might have some fields missing. Skip freeing BLOB
      buffers for such missing fields.
    */
    if (table->field[*ptr])
      ((Field_blob*) table->field[*ptr])->free();
  }
}


/**
  Reclaim temporary blob storage which is bigger than 
  a threshold.
 
  @param table A handle to the TABLE object containing blob fields
  @param size The threshold value.
 
*/

void free_field_buffers_larger_than(TABLE *table, uint32 size)
{
  uint *ptr, *end;
  for (ptr= table->s->blob_field, end=ptr + table->s->blob_fields ;
       ptr != end ;
       ptr++)
  {
    Field_blob *blob= (Field_blob*) table->field[*ptr];
    if (blob->get_field_buffer_size() > size)
        blob->free();
  }
}

/* error message when opening a form file */

void open_table_error(TABLE_SHARE *share, enum open_frm_error error,
                      int db_errno)
{
  char buff[FN_REFLEN];
  const myf errortype= ME_ERROR_LOG;  // Write fatals error to log
  DBUG_ENTER("open_table_error");
  DBUG_PRINT("info", ("error: %d  db_errno: %d", error, db_errno));

  switch (error) {
  case OPEN_FRM_OPEN_ERROR:
    /*
      Test if file didn't exists. We have to also test for EINVAL as this
      may happen on windows when opening a file with a not legal file name
    */
    if (db_errno == ENOENT || db_errno == EINVAL)
      my_error(ER_NO_SUCH_TABLE, MYF(0), share->db.str, share->table_name.str);
    else
    {
      strxmov(buff, share->normalized_path.str, reg_ext, NullS);
      my_error((db_errno == EMFILE) ? ER_CANT_OPEN_FILE : ER_FILE_NOT_FOUND,
               errortype, buff, db_errno);
    }
    break;
  case OPEN_FRM_OK:
    DBUG_ASSERT(0); // open_table_error() is never called for this one
    break;
  case OPEN_FRM_ERROR_ALREADY_ISSUED:
    break;
  case OPEN_FRM_NOT_A_VIEW:
    my_error(ER_WRONG_OBJECT, MYF(0), share->db.str,
             share->table_name.str, "VIEW");
    break;
  case OPEN_FRM_NOT_A_TABLE:
    my_error(ER_WRONG_OBJECT, MYF(0), share->db.str,
             share->table_name.str, "TABLE");
    break;
  case OPEN_FRM_DISCOVER:
    DBUG_ASSERT(0); // open_table_error() is never called for this one
    break;
  case OPEN_FRM_CORRUPTED:
    strxmov(buff, share->normalized_path.str, reg_ext, NullS);
    my_error(ER_NOT_FORM_FILE, errortype, buff);
    break;
  case OPEN_FRM_READ_ERROR:
    strxmov(buff, share->normalized_path.str, reg_ext, NullS);
    my_error(ER_ERROR_ON_READ, errortype, buff, db_errno);
    break;
  case OPEN_FRM_NEEDS_REBUILD:
    strxnmov(buff, sizeof(buff)-1,
             share->db.str, ".", share->table_name.str, NullS);
    my_error(ER_TABLE_NEEDS_REBUILD, errortype, buff);
    break;
  }
  DBUG_VOID_RETURN;
} /* open_table_error */


	/*
	** fix a str_type to a array type
	** typeparts separated with some char. differents types are separated
	** with a '\0'
	*/

static bool
fix_type_pointers(const char ***typelib_value_names,
                  uint **typelib_value_lengths,
                  TYPELIB *point_to_type, uint types,
                  char *ptr, size_t length)
{
  const char *end= ptr + length;

  while (types--)
  {
    char sep;
    point_to_type->name=0;
    point_to_type->type_names= *typelib_value_names;
    point_to_type->type_lengths= *typelib_value_lengths;

    /*
      Typelib can be encoded as:
      1) 0x00                     - empty typelib
      2) 0xFF 0x00                - empty typelib (index names)
      3) sep (value sep)... 0x00  - non-empty typelib (where sep is a separator)
    */
    if (length == 2 && ptr[0] == (char) 0xFF && ptr[1] == '\0')
    {
      /*
        This is a special case #2.
        If there are no indexes at all, index names can be encoded
        as a two byte sequence: 0xFF 0x00
        TODO: Check if it's a bug in the FRM packing routine.
        It should probably write just 0x00 instead of 0xFF00.
      */
      ptr+= 2;
    }
    else if ((sep= *ptr++))            // A non-empty typelib
    {
      for ( ; ptr < end; )
      {
        // Now scan the next value+sep pair
        char *vend= (char*) memchr(ptr, sep, end - ptr);
        if (!vend)
          return true;            // Bad format
        *((*typelib_value_names)++)= ptr;
        *((*typelib_value_lengths)++)= (uint) (vend - ptr);
        *vend= '\0';              // Change sep to '\0'
        ptr= vend + 1;            // Shift from sep to the next byte
        /*
          Now we can have either:
          - the end-of-typelib marker (0x00)
          - more value+sep pairs
        */
        if (!*ptr)
        {
          /*
            We have an ambiguity here. 0x00 can be an end-of-typelib marker,
            but it can also be a part of the next value:
              CREATE TABLE t1 (a ENUM(0x61, 0x0062) CHARACTER SET BINARY);
            If this is the last ENUM/SET in the table and there is still more
            packed data left after 0x00, then we know for sure that 0x00
            is a part of the next value.
            TODO-10.5+: we should eventually introduce a new unambiguous
            typelib encoding for FRM.
          */
          if (!types && ptr + 1 < end)
            continue;           // A binary value starting with 0x00
          ptr++;                // Consume the end-of-typelib marker
          break;                // End of the current typelib
        }
      }
    }
    point_to_type->count= (uint) (*typelib_value_names -
                                  point_to_type->type_names);
    point_to_type++;
    *((*typelib_value_names)++)= NullS; /* End of type */
    *((*typelib_value_lengths)++)= 0;   /* End of type */
  }
  return ptr != end;
} /* fix_type_pointers */


/*
 Search after a field with given start & length
 If an exact field isn't found, return longest field with starts
 at right position.
 
 NOTES
   This is needed because in some .frm fields 'fieldnr' was saved wrong

 RETURN
   0  error
   #  field number +1
*/

static field_index_t find_field(Field **fields, uchar *record, uint start,
                                uint length)
{
  Field **field;
  field_index_t i, pos;

  pos= 0;
  for (field= fields, i=1 ; *field ; i++,field++)
  {
    if ((*field)->offset(record) == start)
    {
      if ((*field)->key_length() == length)
	return (i);
      if (!pos || fields[pos-1]->pack_length() <
	  (*field)->pack_length())
	pos= i;
    }
  }
  return (pos);
}


/*
  Store an SQL quoted string.

  SYNOPSIS  
    append_unescaped()
    res		result String
    pos		string to be quoted
    length	it's length

  NOTE
    This function works correctly with utf8 or single-byte charset strings.
    May fail with some multibyte charsets though.
*/

void append_unescaped(String *res, const char *pos, size_t length)
{
  const char *end= pos+length;
  res->append('\'');

  for (; pos != end ; pos++)
  {
    switch (*pos) {
    case 0:				/* Must be escaped for 'mysql' */
      res->append('\\');
      res->append('0');
      break;
    case '\n':				/* Must be escaped for logs */
      res->append('\\');
      res->append('n');
      break;
    case '\r':
      res->append('\\');		/* This gives better readability */
      res->append('r');
      break;
    case '\\':
      res->append('\\');		/* Because of the sql syntax */
      res->append('\\');
      break;
    case '\'':
      res->append('\'');		/* Because of the sql syntax */
      res->append('\'');
      break;
    default:
      res->append(*pos);
      break;
    }
  }
  res->append('\'');
}


void prepare_frm_header(THD *thd, uint reclength, uchar *fileinfo,
                        HA_CREATE_INFO *create_info, uint keys, KEY *key_info)
{
  size_t key_comment_total_bytes= 0;
  uint i;
  uchar frm_format= create_info->expression_length ? FRM_VER_EXPRESSSIONS
                                                   : FRM_VER_TRUE_VARCHAR;
  DBUG_ENTER("prepare_frm_header");

  /* Fix this when we have new .frm files;  Current limit is 4G rows (TODO) */
  if (create_info->max_rows > UINT_MAX32)
    create_info->max_rows= UINT_MAX32;
  if (create_info->min_rows > UINT_MAX32)
    create_info->min_rows= UINT_MAX32;

  /*
    Keep in sync with pack_keys() in unireg.cc
    For each key:
    8 bytes for the key header
    9 bytes for each key-part (MAX_REF_PARTS)
    NAME_LEN bytes for the name
    1 byte for the NAMES_SEP_CHAR (before the name)
    For all keys:
    6 bytes for the header
    1 byte for the NAMES_SEP_CHAR (after the last name)
    9 extra bytes (padding for safety? alignment?)
  */
  for (i= 0; i < keys; i++)
  {
    DBUG_ASSERT(MY_TEST(key_info[i].flags & HA_USES_COMMENT) ==
                (key_info[i].comment.length > 0));
    if (key_info[i].flags & HA_USES_COMMENT)
      key_comment_total_bytes += 2 + key_info[i].comment.length;
    if (key_info[i].algorithm == HA_KEY_ALG_LONG_HASH)
      frm_format= FRM_VER_EXPRESSSIONS;
  }

  size_t key_length, tmp_key_length, tmp, csid;
  bzero((char*) fileinfo, FRM_HEADER_SIZE);
  /* header */
  fileinfo[0]=(uchar) 254;
  fileinfo[1]= 1;
  fileinfo[2]= frm_format;

  DBUG_ASSERT(ha_storage_engine_is_enabled(create_info->db_type));
  fileinfo[3]= (uchar) ha_legacy_type(create_info->db_type);

  key_length= keys * (8 + MAX_REF_PARTS * 9 + NAME_LEN + 1) + 16
              + key_comment_total_bytes;

  int2store(fileinfo+8,1);
  tmp_key_length= (key_length < 0xffff) ? key_length : 0xffff;
  int2store(fileinfo+14,tmp_key_length);
  int2store(fileinfo+16,reclength);
  int4store(fileinfo+18,create_info->max_rows);
  int4store(fileinfo+22,create_info->min_rows);
  /* fileinfo[26] is set in mysql_create_frm() */
  fileinfo[27]=2;				// Use long pack-fields
  /* fileinfo[28 & 29] is set to key_info_length in mysql_create_frm() */
  create_info->table_options|=HA_OPTION_LONG_BLOB_PTR; // Use portable blob pointers
  int2store(fileinfo+30,create_info->table_options);
  fileinfo[32]=0;				// No filename anymore
  fileinfo[33]=5;                             // Mark for 5.0 frm file
  int4store(fileinfo+34,create_info->avg_row_length);
  csid= (create_info->default_table_charset ?
         create_info->default_table_charset->number : 0);
  fileinfo[38]= (uchar) csid;
  fileinfo[39]= (uchar) ((uint) create_info->transactional |
                         ((uint) create_info->page_checksum << 2) |
                         ((create_info->sequence ? HA_CHOICE_YES : 0) << 4));
  fileinfo[40]= (uchar) create_info->row_type;
  /* Bytes 41-46 were for RAID support; now reused for other purposes */
  fileinfo[41]= (uchar) (csid >> 8);
  int2store(fileinfo+42, create_info->stats_sample_pages & 0xffff);
  fileinfo[44]= (uchar)  create_info->stats_auto_recalc;
  int2store(fileinfo+45, (create_info->check_constraint_list->elements+
                          create_info->field_check_constraints));
  int4store(fileinfo+47, key_length);
  tmp= MYSQL_VERSION_ID;          // Store to avoid warning from int4store
  int4store(fileinfo+51, tmp);
  int4store(fileinfo+55, create_info->extra_size);
  /*
    59-60 is unused since 10.2.4
    61 for default_part_db_type
  */
  int2store(fileinfo+62, create_info->key_block_size);
  DBUG_VOID_RETURN;
} /* prepare_fileinfo */


void update_create_info_from_table(HA_CREATE_INFO *create_info, TABLE *table)
{
  TABLE_SHARE *share= table->s;
  DBUG_ENTER("update_create_info_from_table");

  create_info->max_rows= share->max_rows;
  create_info->min_rows= share->min_rows;
  create_info->table_options= share->db_create_options;
  create_info->avg_row_length= share->avg_row_length;
  create_info->row_type= share->row_type;
  create_info->key_block_size= share->key_block_size;
  create_info->default_table_charset= share->table_charset;
  create_info->alter_table_convert_to_charset= 0;
  create_info->comment= share->comment;
  create_info->transactional= share->transactional;
  create_info->page_checksum= share->page_checksum;
  create_info->option_list= share->option_list;
  create_info->sequence= MY_TEST(share->sequence);

  DBUG_VOID_RETURN;
}

int
rename_file_ext(const char * from,const char * to,const char * ext)
{
  /* Reserve space for ./databasename/tablename.frm + NUL byte */
  char from_b[2 + FN_REFLEN + 4 + 1], to_b[2 + FN_REFLEN + 4 + 1];
  (void) strxmov(from_b,from,ext,NullS);
  (void) strxmov(to_b,to,ext,NullS);
  return mysql_file_rename(key_file_frm, from_b, to_b, MYF(0));
}


/*
  Allocate string field in MEM_ROOT and return it as NULL-terminated string

  SYNOPSIS
    get_field()
    mem   	MEM_ROOT for allocating
    field 	Field for retrieving of string

  RETURN VALUES
    NullS  string is empty
    #      pointer to NULL-terminated string value of field
*/

char *get_field(MEM_ROOT *mem, Field *field)
{
  THD *thd= field->get_thd();
  Sql_mode_instant_remove sms(thd, MODE_PAD_CHAR_TO_FULL_LENGTH);
  LEX_STRING ls= field->val_lex_string_strmake(mem);
  DBUG_ASSERT((!ls.str && !ls.length) || ls.str[ls.length] == '\0');
  // Empty string "" is intentionally returned as NullS
  return ls.length == 0 ? NullS : ls.str;
}

/*
  DESCRIPTION
    given a buffer with a key value, and a map of keyparts
    that are present in this value, returns the length of the value
*/
uint calculate_key_len(TABLE *table, uint key, const uchar *buf,
                       key_part_map keypart_map)
{
  /* works only with key prefixes */
  DBUG_ASSERT(((keypart_map + 1) & keypart_map) == 0);

  KEY *key_info= table->key_info+key;
  KEY_PART_INFO *key_part= key_info->key_part;
  KEY_PART_INFO *end_key_part= key_part + table->actual_n_key_parts(key_info);
  uint length= 0;

  while (key_part < end_key_part && keypart_map)
  {
    length+= key_part->store_length;
    keypart_map >>= 1;
    key_part++;
  }
  return length;
}

#ifndef DBUG_OFF
/**
  Verifies that database/table name is in lowercase, when it should be

  This is supposed to be used only inside DBUG_ASSERT()
*/
bool Lex_ident_fs::ok_for_lower_case_names() const
{
  return !lower_case_table_names || !str || is_in_lower_case();
}

bool Lex_ident_fs::is_in_lower_case() const
{
  DBNameBuffer buf(*this, true);
  return cmp(*this, buf.to_lex_cstring()) == 0;
}
#endif

/*
  Allow anything as a table name, as long as it doesn't contain an
  ' ' at the end
  returns 1 on error
*/

bool check_table_name(const char *name, size_t length, bool disallow_path_chars)
{
  if (!disallow_path_chars &&
      (disallow_path_chars= check_mysql50_prefix(name)))
  {
    name+= MYSQL50_TABLE_NAME_PREFIX_LENGTH;
    length-= MYSQL50_TABLE_NAME_PREFIX_LENGTH;
  }

  return Lex_ident_fs::check_body(name, length, disallow_path_chars);
}


bool Lex_ident_fs::check_body(const char *name, size_t length,
                              bool disallow_path_chars)
{
  if (!length || length > NAME_LEN)
    return 1;

  // name length in symbols
  size_t char_length= 0;
  const char *end= name + length;

  if (name[length-1]==' ')
    return 1;

  for ( ; name != end ; char_length++)
  {
    int len= my_ismbchar(&my_charset_utf8mb3_general_ci, name, end);
    if (len)
    {
      name+= len;
      continue;
    }
    if (disallow_path_chars &&
        (*name == '/' || *name == '\\' || *name == '~' || *name == FN_EXTCHAR))
      return 1;
    /*
      We don't allow zero byte in table/schema names:
      - Some code still uses NULL-terminated strings.
        Zero bytes will confuse this code.
      - There is a little practical use of zero bytes in names anyway.
      Note, if the string passed as "name" comes here
      from the parser as an identifier, it does not contain zero bytes,
      as the parser rejects zero bytes in identifiers.
      But "name" can also come here from queries like this:
        SELECT * FROM I_S.TABLES WHERE TABLE_NAME='str';
      In this case "name" is a general string expression
      and it can have any arbitrary bytes, including zero bytes.
    */
    if (*name == 0x00)
      return 1;
    name++;
  }
  return char_length > NAME_CHAR_LEN;
}


/**
  Check if the name is a valid database name
  @returns false - on success (valid)
  @returns true - on error (invalid)
*/
bool Lex_ident_fs::check_db_name() const
{
  DBUG_ASSERT(str);
  if (check_mysql50_prefix(str))
  {
    Lex_ident_fs name(Lex_cstring(str + MYSQL50_TABLE_NAME_PREFIX_LENGTH,
                                  length - MYSQL50_TABLE_NAME_PREFIX_LENGTH));
    return db_name_is_in_ignore_db_dirs_list(name.str) ||
           check_body(name.str, name.length, true);
  }
  return db_name_is_in_ignore_db_dirs_list(str) ||
         check_body(str, length, false);
}


/**
  Check if the name is a valid database name
  and raise an error in case of an invalid name.

  @returns false - on success (valid)
  @returns true - on error (invalid)
*/
bool Lex_ident_fs::check_db_name_with_error() const
{
  if (!check_db_name())
    return false;
  my_error(ER_WRONG_DB_NAME ,MYF(0), safe_str(str));
  return true;
}


bool check_column_name(const Lex_ident &ident)
{
  // name length in symbols
  const char *name= ident.str, *end= ident.str + ident.length;
  size_t name_length= 0;
  bool last_char_is_space= TRUE;

  while (*name)
  {
#if defined(USE_MB) && defined(USE_MB_IDENT)
    last_char_is_space= my_isspace(system_charset_info, *name);
    if (system_charset_info->use_mb())
    {
      if (int len= my_ismbchar(system_charset_info, name,  end))
      {
        name += len;
        name_length++;
        continue;
      }
    }
#else
    last_char_is_space= *name==' ';
    if (*name == '\377')
      return 1;
#endif
    name++;
    name_length++;
  }
  /* Error if empty or too long column name */
  return last_char_is_space || (name_length > NAME_CHAR_LEN);
}


/**
  Checks whether a table is intact. Should be done *just* after the table has
  been opened.

  @param[in] table             The table to check
  @param[in] table_def         Expected structure of the table (column name
                               and type)

  @retval  FALSE  OK
  @retval  TRUE   There was an error. An error message is output
                  to the error log.  We do not push an error
                  message into the error stack because this
                  function is currently only called at start up,
                  and such errors never reach the user.
*/

bool
Table_check_intact::check(TABLE *table, const TABLE_FIELD_DEF *table_def)
{
  uint i;
  my_bool error= FALSE;
  const TABLE_FIELD_TYPE *field_def= table_def->field;
  DBUG_ENTER("table_check_intact");
  DBUG_PRINT("info",("table: %s  expected_count: %d",
                     table->alias.c_ptr(), table_def->count));

  /* Whether the table definition has already been validated. */
  if (table->s->table_field_def_cache == table_def)
    goto end;

  if (table->s->fields != table_def->count)
  {
    THD *thd= current_thd;
    DBUG_PRINT("info", ("Column count has changed, checking the definition"));

    /* previous MySQL version */
    if (MYSQL_VERSION_ID > table->s->mysql_version)
    {
      report_error(ER_COL_COUNT_DOESNT_MATCH_PLEASE_UPDATE,
                   ER_THD(thd, ER_COL_COUNT_DOESNT_MATCH_PLEASE_UPDATE),
                   table->alias.c_ptr(), table_def->count, table->s->fields,
                   static_cast<int>(table->s->mysql_version),
                   MYSQL_VERSION_ID);
      DBUG_RETURN(TRUE);
    }
    else if (MYSQL_VERSION_ID == table->s->mysql_version)
    {
      report_error(ER_COL_COUNT_DOESNT_MATCH_CORRUPTED_V2,
                   ER_THD(thd, ER_COL_COUNT_DOESNT_MATCH_CORRUPTED_V2),
                   table->s->db.str, table->s->table_name.str,
                   table_def->count, table->s->fields);
      DBUG_RETURN(TRUE);
    }
    /*
      Something has definitely changed, but we're running an older
      version of MySQL with new system tables.
      Let's check column definitions. If a column was added at
      the end of the table, then we don't care much since such change
      is backward compatible.
    */
  }
  else
  {
  StringBuffer<1024> sql_type(system_charset_info);
  sql_type.extra_allocation(256); // Allocate min 256 characters at once
  for (i=0 ; i < table_def->count; i++, field_def++)
  {
    sql_type.length(0);
    if (i < table->s->fields)
    {
      Field *field= table->field[i];

      if (strncmp(field->field_name.str, field_def->name.str,
                  field_def->name.length))
      {
        /*
          Name changes are not fatal, we use ordinal numbers to access columns.
          Still this can be a sign of a tampered table, output an error
          to the error log.
        */
        report_error(0, "Incorrect definition of table %s.%s: "
                     "expected column '%s' at position %d, found '%s'.",
                     table->s->db.str, table->alias.c_ptr(),
                     field_def->name.str, i,
                     field->field_name.str);
      }
      field->sql_type(sql_type);
      /*
        Generally, if column types don't match, then something is
        wrong.

        However, we only compare column definitions up to the
        length of the original definition, since we consider the
        following definitions compatible:

        1. DATETIME and DATETIM
        2. INT(11) and INT(11
        3. SET('one', 'two') and SET('one', 'two', 'more')

        For SETs or ENUMs, if the same prefix is there it's OK to
        add more elements - they will get higher ordinal numbers and
        the new table definition is backward compatible with the
        original one.
       */
      if (strncmp(sql_type.c_ptr_safe(), field_def->type.str,
                  field_def->type.length - 1))
      {
        report_error(0, "Incorrect definition of table %s.%s: "
                     "expected column '%s' at position %d to have type "
                     "%s, found type %s.", table->s->db.str,
                     table->alias.c_ptr(),
                     field_def->name.str, i, field_def->type.str,
                     sql_type.c_ptr_safe());
        error= TRUE;
      }
      else if (field_def->cset.str && !field->has_charset())
      {
        report_error(0, "Incorrect definition of table %s.%s: "
                     "expected the type of column '%s' at position %d "
                     "to have character set '%s' but the type has no "
                     "character set.", table->s->db.str,
                     table->alias.c_ptr(),
                     field_def->name.str, i, field_def->cset.str);
        error= TRUE;
      }
      else if (field_def->cset.str &&
               strncmp(field->charset()->cs_name.str, field_def->cset.str,
                       field_def->cset.length))
      {
        report_error(0, "Incorrect definition of table %s.%s: "
                     "expected the type of column '%s' at position %d "
                     "to have character set '%s' but found "
                     "character set '%s'.", table->s->db.str,
                     table->alias.c_ptr(),
                     field_def->name.str, i, field_def->cset.str,
                     field->charset()->cs_name.str);
        error= TRUE;
      }
    }
    else
    {
      report_error(0, "Incorrect definition of table %s.%s: "
                   "expected column '%s' at position %d to have type %s "
                   " but the column is not found.",
                   table->s->db.str, table->alias.c_ptr(),
                   field_def->name.str, i, field_def->type.str);
      error= TRUE;
    }
  }
  }

  if (table_def->primary_key_parts)
  {
    if (table->s->primary_key == MAX_KEY)
    {
      report_error(0, "Incorrect definition of table %s.%s: "
                   "missing primary key.", table->s->db.str,
                   table->alias.c_ptr());
      error= TRUE;
    }
    else
    {
      KEY *pk= &table->s->key_info[table->s->primary_key];
      if (pk->user_defined_key_parts != table_def->primary_key_parts)
      {
        report_error(0, "Incorrect definition of table %s.%s: "
                     "Expected primary key to have %u columns, but instead "
                     "found %u columns.", table->s->db.str,
                     table->alias.c_ptr(), table_def->primary_key_parts,
                     pk->user_defined_key_parts);
        error= TRUE;
      }
      else
      {
        for (i= 0; i < pk->user_defined_key_parts; ++i)
        {
          if (table_def->primary_key_columns[i] + 1 != pk->key_part[i].fieldnr)
          {
            report_error(0, "Incorrect definition of table %s.%s: Expected "
                         "primary key part %u to refer to column %u, but "
                         "instead found column %u.", table->s->db.str,
                         table->alias.c_ptr(), i + 1,
                         table_def->primary_key_columns[i] + 1,
                         pk->key_part[i].fieldnr);
            error= TRUE;
          }
        }
      }
    }
  }

  if (likely(! error))
    table->s->table_field_def_cache= table_def;

end:

  if (has_keys && !error && !table->key_info)
  {
    report_error(0, "Incorrect definition of table %s.%s: "
                 "indexes are missing",
                 table->s->db.str, table->alias.c_ptr());
    error= TRUE;
  }

  DBUG_RETURN(error);
}


void Table_check_intact_log_error::report_error(uint, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  error_log_print(ERROR_LEVEL, fmt, args);
  va_end(args);
}


/**
  Traverse portion of wait-for graph which is reachable through edge
  represented by this flush ticket in search for deadlocks.

  @retval TRUE  A deadlock is found. A victim is remembered
                by the visitor.
  @retval FALSE Success, no deadlocks.
*/

bool Wait_for_flush::accept_visitor(MDL_wait_for_graph_visitor *gvisitor)
{
  return m_share->visit_subgraph(this, gvisitor);
}


uint Wait_for_flush::get_deadlock_weight() const
{
  return m_deadlock_weight;
}


/**
  Traverse portion of wait-for graph which is reachable through this
  table share in search for deadlocks.

  @param waiting_ticket  Ticket representing wait for this share.
  @param dvisitor        Deadlock detection visitor.

  @retval TRUE  A deadlock is found. A victim is remembered
                by the visitor.
  @retval FALSE No deadlocks, it's OK to begin wait.
*/

bool TABLE_SHARE::visit_subgraph(Wait_for_flush *wait_for_flush,
                                 MDL_wait_for_graph_visitor *gvisitor)
{
  TABLE *table;
  MDL_context *src_ctx= wait_for_flush->get_ctx();
  bool result= TRUE;

  /*
    To protect all_tables list from being concurrently modified
    while we are iterating through it we increment tdc.all_tables_refs.
    This does not introduce deadlocks in the deadlock detector
    because we won't try to acquire tdc.LOCK_table_share while
    holding a write-lock on MDL_lock::m_rwlock.
  */
  mysql_mutex_lock(&tdc->LOCK_table_share);
  tdc->all_tables_refs++;
  mysql_mutex_unlock(&tdc->LOCK_table_share);

  All_share_tables_list::Iterator tables_it(tdc->all_tables);

  /*
    In case of multiple searches running in parallel, avoid going
    over the same loop twice and shortcut the search.
    Do it after taking the lock to weed out unnecessary races.
  */
  if (src_ctx->m_wait.get_status() != MDL_wait::EMPTY)
  {
    result= FALSE;
    goto end;
  }

  if (gvisitor->enter_node(src_ctx))
    goto end;

  while ((table= tables_it++))
  {
    DBUG_ASSERT(table->in_use && tdc->flushed);
    if (gvisitor->inspect_edge(&table->in_use->mdl_context))
    {
      goto end_leave_node;
    }
  }

  tables_it.rewind();
  while ((table= tables_it++))
  {
    DBUG_ASSERT(table->in_use && tdc->flushed);
    if (table->in_use->mdl_context.visit_subgraph(gvisitor))
    {
      goto end_leave_node;
    }
  }

  result= FALSE;

end_leave_node:
  gvisitor->leave_node(src_ctx);

end:
  mysql_mutex_lock(&tdc->LOCK_table_share);
  if (!--tdc->all_tables_refs)
    mysql_cond_broadcast(&tdc->COND_release);
  mysql_mutex_unlock(&tdc->LOCK_table_share);

  return result;
}


/**
  Wait until the subject share is removed from the table
  definition cache and make sure it's destroyed.

  @param mdl_context     MDL context for thread which is going to wait.
  @param abstime         Timeout for waiting as absolute time value.
  @param deadlock_weight Weight of this wait for deadlock detector.

  @pre LOCK_table_share is locked, the share is marked for flush and
       this connection does not reference the share.
       LOCK_table_share will be unlocked temporarily during execution.

  It may happen that another FLUSH TABLES thread marked this share
  for flush, but didn't yet purge it from table definition cache.
  In this case we may start waiting for a table share that has no
  references (ref_count == 0). We do this with assumption that this
  another FLUSH TABLES thread is about to purge this share.

  @retval FALSE - Success.
  @retval TRUE  - Error (OOM, deadlock, timeout, etc...).
*/

bool TABLE_SHARE::wait_for_old_version(THD *thd, struct timespec *abstime,
                                       uint deadlock_weight)
{
  MDL_context *mdl_context= &thd->mdl_context;
  Wait_for_flush ticket(mdl_context, this, deadlock_weight);
  MDL_wait::enum_wait_status wait_status;

  mysql_mutex_assert_owner(&tdc->LOCK_table_share);
  DBUG_ASSERT(tdc->flushed);

  tdc->m_flush_tickets.push_front(&ticket);

  mdl_context->m_wait.reset_status();

  mysql_mutex_unlock(&tdc->LOCK_table_share);

  mdl_context->will_wait_for(&ticket);

  mdl_context->find_deadlock();

  wait_status= mdl_context->m_wait.timed_wait(thd, abstime, TRUE,
                                              &stage_waiting_for_table_flush);

  mdl_context->done_waiting_for();

  mysql_mutex_lock(&tdc->LOCK_table_share);
  tdc->m_flush_tickets.remove(&ticket);
  mysql_cond_broadcast(&tdc->COND_release);
  mysql_mutex_unlock(&tdc->LOCK_table_share);


  /*
    In cases when our wait was aborted by KILL statement,
    a deadlock or a timeout, the share might still be referenced,
    so we don't delete it. Note, that we can't determine this
    condition by checking wait_status alone, since, for example,
    a timeout can happen after all references to the table share
    were released, but before the share is removed from the
    cache and we receive the notification. This is why
    we first destroy the share, and then look at
    wait_status.
  */
  switch (wait_status)
  {
  case MDL_wait::GRANTED:
    return FALSE;
  case MDL_wait::VICTIM:
    my_error(ER_LOCK_DEADLOCK, MYF(0));
    return TRUE;
  case MDL_wait::TIMEOUT:
    my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
    return TRUE;
  case MDL_wait::KILLED:
    return TRUE;
  default:
    DBUG_ASSERT(0);
    return TRUE;
  }
}


/**
  Initialize TABLE instance (newly created, or coming either from table
  cache or THD::temporary_tables list) and prepare it for further use
  during statement execution. Set the 'alias' attribute from the specified
  TABLE_LIST element. Remember the TABLE_LIST element in the
  TABLE::pos_in_table_list member.

  @param thd  Thread context.
  @param tl   TABLE_LIST element.
*/

void TABLE::init(THD *thd, TABLE_LIST *tl)
{
  DBUG_ASSERT(s->tmp_table != NO_TMP_TABLE || s->tdc->ref_count > 0);

  if (thd->lex->need_correct_ident())
    alias_name_used= my_strcasecmp(table_alias_charset,
                                   s->table_name.str,
                                   tl->alias.str);
  /* Fix alias if table name changes. */
  if (!alias.alloced_length() || strcmp(alias.c_ptr(), tl->alias.str))
    alias.copy(tl->alias.str, tl->alias.length, alias.charset());

  tablenr= thd->current_tablenr++;
  used_fields= 0;
  const_table= 0;
  null_row= 0;
  maybe_null= 0;
  force_index= 0;
  force_index_order= 0;
  force_index_group= 0;
  status= STATUS_NO_RECORD;
  insert_values= 0;
  fulltext_searched= 0;
  file->ft_handler= 0;
  reginfo.impossible_range= 0;
  reginfo.join_tab= NULL;
  reginfo.not_exists_optimize= FALSE;
  reginfo.skip_locked= false;
  created= TRUE;
  cond_selectivity= 1.0;
  cond_selectivity_sampling_explain= NULL;
  range_rowid_filter_cost_info_elems= 0;
  range_rowid_filter_cost_info_ptr= NULL;
  range_rowid_filter_cost_info= NULL;
  vers_write= s->versioned;
  opt_range_condition_rows=0;
  no_cache= false;
  initialize_opt_range_structures();

  /*
    Update optimizer_costs to ensure that a SET STATEMENT of the
    variables it will work.
  */
  file->set_optimizer_costs(thd);

#ifdef HAVE_REPLICATION
  /* used in RBR Triggers */
  master_had_triggers= 0;
#endif
  /* Catch wrong handling of the auto_increment_field_not_null. */
  DBUG_ASSERT(!auto_increment_field_not_null);
  auto_increment_field_not_null= FALSE;

  pos_in_table_list= tl;

  clear_column_bitmaps();
  for (Field **f_ptr= field ; *f_ptr ; f_ptr++)
  {
    (*f_ptr)->next_equal_field= NULL;
    (*f_ptr)->cond_selectivity= 1.0;
  }

  /* enable and clear or disable engine query statistics */
  if (thd->should_collect_handler_stats())
    file->ha_handler_stats_reset();
  else
    file->ha_handler_stats_disable();

  notnull_cond= 0;
  DBUG_ASSERT(!file->keyread_enabled());

  restore_record(this, s->default_values);

  /* Tables may be reused in a sub statement. */
  DBUG_ASSERT(!file->extra(HA_EXTRA_IS_ATTACHED_CHILDREN));
}


/*
  Create Item_field for each column in the table.

  SYNPOSIS
    TABLE::fill_item_list()
      item_list          a pointer to an empty list used to store items

  DESCRIPTION
    Create Item_field object for each column in the table and
    initialize it with the corresponding Field. New items are
    created in the current THD memory root.

  RETURN VALUE
    0                    success
    1                    out of memory
*/

bool TABLE::fill_item_list(List<Item> *item_list) const
{
  /*
    All Item_field's created using a direct pointer to a field
    are fixed in Item_field constructor.
  */
  for (Field **ptr= field; *ptr; ptr++)
  {
    Item_field *item= new (in_use->mem_root) Item_field(in_use, *ptr);
    if (!item || item_list->push_back(item))
      return TRUE;
  }
  return FALSE;
}

/*
  Reset an existing list of Item_field items to point to the
  Fields of this table.

  SYNPOSIS
    TABLE::fill_item_list()
      item_list          a non-empty list with Item_fields

  DESCRIPTION
    This is a counterpart of fill_item_list used to redirect
    Item_fields to the fields of a newly created table.
    The caller must ensure that number of items in the item_list
    is the same as the number of columns in the table.
*/

void TABLE::reset_item_list(List<Item> *item_list, uint skip) const
{
  List_iterator_fast<Item> it(*item_list);
  Field **ptr= field;
  for ( ; skip && *ptr; skip--)
    ptr++;
  for (; *ptr; ptr++)
  {
    Item_field *item_field= (Item_field*) it++;
    DBUG_ASSERT(item_field != 0);
    item_field->reset_field(*ptr);
  }
}

TABLE_LIST::TABLE_LIST(THD *thd,
                       LEX_CSTRING db_str,
                       bool fqtn,
                       LEX_CSTRING alias_str,
                       bool has_alias_ptr,
                       Table_ident *table_ident,
                       thr_lock_type lock_t,
                       enum_mdl_type mdl_t,
                       ulong table_opts,
                       bool info_schema,
                       st_select_lex *sel,
                       List<Index_hint> *index_hints_ptr,
                       LEX_STRING *option_ptr)
{
  reset();
  db= db_str;
  is_fqtn= fqtn;
  alias= alias_str;
  is_alias= has_alias_ptr;
  if (lower_case_table_names)
  {
    if (table_ident->table.length)
      table_ident->table.length= my_casedn_str(files_charset_info,
                                         (char*) table_ident->table.str);
    if (db.length && db.str != any_db.str)
      db.length= my_casedn_str(files_charset_info, (char*) db.str);
  }

  table_name= table_ident->table;
  lock_type= lock_t;
  mdl_type= mdl_t;
  table_options= table_opts;
  updating= table_options & TL_OPTION_UPDATING;
  ignore_leaves= table_options & TL_OPTION_IGNORE_LEAVES;
  sequence= table_options & TL_OPTION_SEQUENCE;
  derived= table_ident->sel;

  if (!table_ident->sel && info_schema)
  {
    schema_table= find_schema_table(thd, &table_name);
    schema_table_name= table_name;
  }
  select_lex= sel;
  /*
    We can't cache internal temporary tables between prepares as the
    table may be deleted before next exection.
  */
  cacheable_table= !table_ident->is_derived_table();
  index_hints= index_hints_ptr;
  option= option_ptr ? option_ptr->str : 0;
}

/*
  calculate md5 of query

  SYNOPSIS
    TABLE_LIST::calc_md5()
    buffer	buffer for md5 writing
*/

void  TABLE_LIST::calc_md5(char *buffer)
{
  uchar digest[16];
  compute_md5_hash(digest, select_stmt.str,
                   select_stmt.length);
  sprintf(buffer,
	    "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
	    digest[0], digest[1], digest[2], digest[3],
	    digest[4], digest[5], digest[6], digest[7],
	    digest[8], digest[9], digest[10], digest[11],
	    digest[12], digest[13], digest[14], digest[15]);
}


/**
  @brief
  Create field translation for mergeable derived table/view.

  @param thd  Thread handle

  @details
  Create field translation for mergeable derived table/view.

  @return FALSE ok.
  @return TRUE an error occur.
*/

bool TABLE_LIST::create_field_translation(THD *thd)
{
  Item *item;
  Field_translator *transl;
  SELECT_LEX *select= get_single_select();
  List_iterator_fast<Item> it(select->item_list);
  uint field_count= 0;
  Query_arena *arena, backup;
  bool res= FALSE;
  DBUG_ENTER("TABLE_LIST::create_field_translation");
  DBUG_PRINT("enter", ("Alias: '%s'  Unit: %p",
                      (alias.str ? alias.str : "<NULL>"),
                       get_unit()));

  if (thd->stmt_arena->is_conventional() ||
      thd->stmt_arena->is_stmt_prepare_or_first_sp_execute())
  {
    /* initialize lists */
    used_items.empty();
    persistent_used_items.empty();
  }
  else
  {
    /*
      Copy the list created by natural join procedure because the procedure
      will not be repeated.
    */
    used_items= persistent_used_items;
  }

  if (field_translation)
  {
    /*
      Update items in the field translation after view have been prepared.
      It's needed because some items in the select list, like IN subselects,
      might be substituted for optimized ones.
    */
    if (is_view() && get_unit()->prepared && !field_translation_updated)
    {
      field_translation_updated= TRUE;
      if (static_cast<uint>(field_translation_end - field_translation) <
          select->item_list.elements)
        goto allocate;
      while ((item= it++))
      {
        field_translation[field_count++].item= item;
      }
    }

    DBUG_RETURN(FALSE);
  }

allocate:
  arena= thd->activate_stmt_arena_if_needed(&backup);

  /* Create view fields translation table */

  if (!(transl=
        (Field_translator*)(thd->
                            alloc(select->item_list.elements *
                                  sizeof(Field_translator)))))
  {
    res= TRUE;
    goto exit;
  }

  while ((item= it++))
  {
    transl[field_count].name.str=    thd->strmake(item->name.str, item->name.length);
    transl[field_count].name.length= item->name.length;
    transl[field_count++].item= item;
  }
  field_translation= transl;
  field_translation_end= transl + field_count;
  /* It's safe to cache this table for prepared statements */
  cacheable_table= 1;

exit:
  if (arena)
    thd->restore_active_arena(arena, &backup);

  DBUG_RETURN(res);
}


/**
  @brief
  Create field translation for mergeable derived table/view.

  @param thd  Thread handle

  @details
  Create field translation for mergeable derived table/view.

  @return FALSE ok.
  @return TRUE an error occur.
*/

bool TABLE_LIST::setup_underlying(THD *thd)
{
  DBUG_ENTER("TABLE_LIST::setup_underlying");

  if (!view || (!field_translation && merge_underlying_list))
  {
    SELECT_LEX *select= get_single_select();

    if (create_field_translation(thd))
      DBUG_RETURN(TRUE);

    /* full text function moving to current select */
    if (select->ftfunc_list->elements)
    {
      Item_func_match *ifm;
      SELECT_LEX *current_select= thd->lex->current_select;
      List_iterator_fast<Item_func_match>
        li(*(select_lex->ftfunc_list));
      while ((ifm= li++))
        current_select->ftfunc_list->push_front(ifm);
    }
  }
  DBUG_RETURN(FALSE);
}


/*
   Prepare where expression of derived table/view

  SYNOPSIS
    TABLE_LIST::prep_where()
    thd             - thread handler
    conds           - condition of this JOIN
    no_where_clause - do not build WHERE or ON outer qwery do not need it
                      (it is INSERT), we do not need conds if this flag is set

  NOTE: have to be called befor CHECK OPTION preparation, because it makes
  fix_fields for view WHERE clause

  RETURN
    FALSE - OK
    TRUE  - error
*/

bool TABLE_LIST::prep_where(THD *thd, Item **conds,
                               bool no_where_clause)
{
  DBUG_ENTER("TABLE_LIST::prep_where");
  bool res= FALSE;

  for (TABLE_LIST *tbl= merge_underlying_list; tbl; tbl= tbl->next_local)
  {
    if (tbl->is_view_or_derived() &&
        tbl->prep_where(thd, conds, no_where_clause))
    {
      DBUG_RETURN(TRUE);
    }
  }

  if (where)
  {
    if (where->fixed())
      where->update_used_tables();
    else if (where->fix_fields(thd, &where))
      DBUG_RETURN(TRUE);

    /*
      check that it is not VIEW in which we insert with INSERT SELECT
      (in this case we can't add view WHERE condition to main SELECT_LEX)
    */
    if (!no_where_clause && !where_processed)
    {
      TABLE_LIST *tbl= this;
      Query_arena *arena= thd->stmt_arena, backup;
      arena= thd->activate_stmt_arena_if_needed(&backup);  // For easier test

      /* Go up to join tree and try to find left join */
      for (; tbl; tbl= tbl->embedding)
      {
        if (tbl->outer_join)
        {
          /*
            Store WHERE condition to ON expression for outer join, because
            we can't use WHERE to correctly execute left joins on VIEWs and
            this expression will not be moved to WHERE condition (i.e. will
            be clean correctly for PS/SP)
          */
          tbl->on_expr= and_conds(thd, tbl->on_expr,
                                  where->copy_andor_structure(thd));
          break;
        }
      }
      if (tbl == 0)
      {
        if (*conds)
          res= (*conds)->fix_fields_if_needed_for_bool(thd, conds);
        if (!res)
          *conds= and_conds(thd, *conds, where->copy_andor_structure(thd));
        if (*conds && !res)
          res= (*conds)->fix_fields_if_needed_for_bool(thd, conds);
      }
      if (arena)
        thd->restore_active_arena(arena, &backup);
      where_processed= TRUE;
    }
  }

  DBUG_RETURN(res);
}

/**
  Check that table/view is updatable and if it has single
  underlying tables/views it is also updatable

  @return Result of the check.
*/

bool TABLE_LIST::single_table_updatable()
{
  if (!updatable)
    return false;
  if (view && view->first_select_lex()->table_list.elements == 1)
  {
    /*
      We need to check deeply only single table views. Multi-table views
      will be turned to multi-table updates and then checked by leaf tables
    */
    return (((TABLE_LIST *)view->first_select_lex()->table_list.first)->
            single_table_updatable());
  }
  return true;
}


/*
  Merge ON expressions for a view

  SYNOPSIS
    merge_on_conds()
    thd             thread handle
    table           table for the VIEW
    is_cascaded     TRUE <=> merge ON expressions from underlying views

  DESCRIPTION
    This function returns the result of ANDing the ON expressions
    of the given view and all underlying views. The ON expressions
    of the underlying views are added only if is_cascaded is TRUE.

  RETURN
    Pointer to the built expression if there is any.
    Otherwise and in the case of a failure NULL is returned.
*/

static Item *
merge_on_conds(THD *thd, TABLE_LIST *table, bool is_cascaded)
{
  DBUG_ENTER("merge_on_conds");

  Item *cond= NULL;
  DBUG_PRINT("info", ("alias: %s", table->alias.str));
  if (table->on_expr)
    cond= table->on_expr->copy_andor_structure(thd);
  if (!table->view)
    DBUG_RETURN(cond);
  for (TABLE_LIST *tbl=
         (TABLE_LIST*)table->view->first_select_lex()->table_list.first;
       tbl;
       tbl= tbl->next_local)
  {
    if (tbl->view && !is_cascaded)
      continue;
    cond= and_conds(thd, cond, merge_on_conds(thd, tbl, is_cascaded));
  }
  DBUG_RETURN(cond);
}


/*
  Prepare check option expression of table

  SYNOPSIS
    TABLE_LIST::prep_check_option()
    thd             - thread handler
    check_opt_type  - WITH CHECK OPTION type (VIEW_CHECK_NONE,
                      VIEW_CHECK_LOCAL, VIEW_CHECK_CASCADED)
                      we use this parameter instead of direct check of
                      effective_with_check to change type of underlying
                      views to VIEW_CHECK_CASCADED if outer view have
                      such option and prevent processing of underlying
                      view check options if outer view have just
                      VIEW_CHECK_LOCAL option.

  NOTE
    This method builds check option condition to use it later on
    every call (usual execution or every SP/PS call).
    This method have to be called after WHERE preparation
    (TABLE_LIST::prep_where)

  RETURN
    FALSE - OK
    TRUE  - error
*/

bool TABLE_LIST::prep_check_option(THD *thd, uint8 check_opt_type)
{
  DBUG_ENTER("TABLE_LIST::prep_check_option");
  bool is_cascaded= check_opt_type == VIEW_CHECK_CASCADED;
  TABLE_LIST *merge_underlying_list= view->first_select_lex()->get_table_list();
  for (TABLE_LIST *tbl= merge_underlying_list; tbl; tbl= tbl->next_local)
  {
    /* see comment of check_opt_type parameter */
    if (tbl->view && tbl->prep_check_option(thd, (is_cascaded ?
                                                  VIEW_CHECK_CASCADED :
                                                  VIEW_CHECK_NONE)))
      DBUG_RETURN(TRUE);
  }

  if (check_opt_type && !check_option_processed)
  {
    Query_arena *arena= thd->stmt_arena, backup;
    arena= thd->activate_stmt_arena_if_needed(&backup);  // For easier test

    if (where)
    {
      check_option= where->copy_andor_structure(thd);
    }
    if (is_cascaded)
    {
      for (TABLE_LIST *tbl= merge_underlying_list; tbl; tbl= tbl->next_local)
      {
        if (tbl->check_option)
          check_option= and_conds(thd, check_option, tbl->check_option);
      }
    }
    check_option= and_conds(thd, check_option,
                            merge_on_conds(thd, this, is_cascaded));

    if (arena)
      thd->restore_active_arena(arena, &backup);
    check_option_processed= TRUE;

  }

  if (check_option)
  {
    THD_WHERE save_where= thd->where;
    thd->where= THD_WHERE::CHECK_OPTION;
    if (check_option->fix_fields_if_needed_for_bool(thd, &check_option))
      DBUG_RETURN(TRUE);
    thd->where= save_where;
  }
  DBUG_RETURN(FALSE);
}


/**
  Hide errors which show view underlying table information. 
  There are currently two mechanisms at work that handle errors for views,
  this one and a more general mechanism based on an Internal_error_handler,
  see Show_create_error_handler. The latter handles errors encountered during
  execution of SHOW CREATE VIEW, while the mechanism using this method is
  handles SELECT from views. The two methods should not clash.

  @param[in,out]  thd     thread handler

  @pre This method can be called only if there is an error.
*/

void TABLE_LIST::replace_view_error_with_generic(THD *thd)
{
  if ((thd->killed && !thd->is_error()) || thd->get_internal_handler())
    return;
  /* Hide "Unknown column" or "Unknown function" error */
  DBUG_ASSERT(thd->is_error());
  switch (thd->get_stmt_da()->sql_errno()) {
    case ER_BAD_FIELD_ERROR:
    case ER_SP_DOES_NOT_EXIST:
    case ER_FUNC_INEXISTENT_NAME_COLLISION:
    case ER_PROCACCESS_DENIED_ERROR:
    case ER_COLUMNACCESS_DENIED_ERROR:
    case ER_TABLEACCESS_DENIED_ERROR:
    case ER_TABLE_NOT_LOCKED:
    case ER_NO_SUCH_TABLE:
    {
      TABLE_LIST *top= top_table();
      thd->clear_error();
      my_error(ER_VIEW_INVALID, MYF(0),
               top->view_db.str, top->view_name.str);
      break;
    }

    case ER_NO_DEFAULT_FOR_FIELD:
    {
      TABLE_LIST *top= top_table();
      thd->clear_error();
      // TODO: make correct error message
      my_error(ER_NO_DEFAULT_FOR_VIEW_FIELD, MYF(0),
               top->view_db.str, top->view_name.str);
      break;
    }
  }
}


/*
  Find underlying base tables (TABLE_LIST) which represent given
  table_to_find (TABLE)

  SYNOPSIS
    TABLE_LIST::find_underlying_table()
    table_to_find table to find

  RETURN
    0  table is not found
    found table reference
*/

TABLE_LIST *TABLE_LIST::find_underlying_table(TABLE *table_to_find)
{
  /* is this real table and table which we are looking for? */
  if (table == table_to_find && view == 0)
    return this;
  if (!view)
    return 0;

  for (TABLE_LIST *tbl= view->first_select_lex()->get_table_list();
       tbl;
       tbl= tbl->next_local)
  {
    TABLE_LIST *result;
    if ((result= tbl->find_underlying_table(table_to_find)))
      return result;
  }
  return 0;
}

/*
  cleanup items belonged to view fields translation table

  SYNOPSIS
    TABLE_LIST::cleanup_items()
*/

void TABLE_LIST::cleanup_items()
{
  if (!field_translation)
    return;

  for (Field_translator *transl= field_translation;
       transl < field_translation_end;
       transl++)
    transl->item->walk(&Item::cleanup_processor, 0, 0);
}


/*
  check CHECK OPTION condition both for view and underlying table

  SYNOPSIS
    TABLE_LIST::view_check_option()
    ignore_failure ignore check option fail

  RETURN
    VIEW_CHECK_OK     OK
    VIEW_CHECK_ERROR  FAILED
    VIEW_CHECK_SKIP   FAILED, but continue
*/


int TABLE_LIST::view_check_option(THD *thd, bool ignore_failure)
{
  if (check_option)
  {
    /* VIEW's CHECK OPTION CLAUSE */
    Counting_error_handler ceh;
    thd->push_internal_handler(&ceh);
    bool res= check_option->val_bool() == false;
    thd->pop_internal_handler();
    if (ceh.errors)
      return(VIEW_CHECK_ERROR);
    if (res)
    {
      TABLE_LIST *main_view= top_table();
      const char *name_db= (main_view->view ? main_view->view_db.str :
                            main_view->db.str);
      const char *name_table= (main_view->view ? main_view->view_name.str :
                               main_view->table_name.str);
      my_error(ER_VIEW_CHECK_FAILED, MYF(ignore_failure ? ME_WARNING : 0),
               name_db, name_table);
      return ignore_failure ? VIEW_CHECK_SKIP : VIEW_CHECK_ERROR;
    }
  }
  return table->verify_constraints(ignore_failure);
}


int TABLE::verify_constraints(bool ignore_failure)
{
  /*
    We have to check is_error() first as we are checking it for each
    constraint to catch fatal warnings.
  */
  if (in_use->is_error())
    return (VIEW_CHECK_ERROR);

  /* go trough check option clauses for fields and table */
  if (check_constraints &&
      !(in_use->variables.option_bits & OPTION_NO_CHECK_CONSTRAINT_CHECKS))
  {
    if (versioned() && !vers_end_field()->is_max())
      return VIEW_CHECK_OK;

    StringBuffer<MAX_FIELD_WIDTH> field_error(system_charset_info);
    for (Virtual_column_info **chk= check_constraints ; *chk ; chk++)
    {
      /*
        yes! NULL is ok.
        see 4.23.3.4 Table check constraints, part 2, SQL:2016
      */
      if (((*chk)->expr->val_bool() == false && !(*chk)->expr->null_value) ||
          in_use->is_error())
      {
        enum_vcol_info_type vcol_type= (*chk)->get_vcol_type();
        DBUG_ASSERT(vcol_type == VCOL_CHECK_TABLE ||
                    vcol_type == VCOL_CHECK_FIELD);

        field_error.set_buffer_if_not_allocated(system_charset_info);
        field_error.length(0);

        if (vcol_type == VCOL_CHECK_FIELD)
        {
          field_error.append(s->table_name);
          field_error.append('.');
        }
        field_error.append((*chk)->name);
        my_error(ER_CONSTRAINT_FAILED,
                 MYF(ignore_failure ? ME_WARNING : 0), field_error.c_ptr(),
                 s->db.str, s->table_name.str);
        return ignore_failure ? VIEW_CHECK_SKIP : VIEW_CHECK_ERROR;
      }
    }
  }
  /*
    We have to check in_use() as checking constraints may have generated
    warnings that should be treated as errors
  */
  return(!in_use->is_error() ? VIEW_CHECK_OK : VIEW_CHECK_ERROR);
}

/*
  Find table in underlying tables by mask and check that only this
  table belong to given mask

  SYNOPSIS
    TABLE_LIST::check_single_table()
    table_arg	reference on variable where to store found table
		(should be 0 on call, to find table, or point to table for
		unique test)
    map         bit mask of tables
    view_arg    view for which we are looking table

  RETURN
    FALSE table not found or found only one
    TRUE  found several tables
*/

bool TABLE_LIST::check_single_table(TABLE_LIST **table_arg,
                                       table_map map,
                                       TABLE_LIST *view_arg)
{
  if (!select_lex)
    return FALSE;
  DBUG_ASSERT(is_merged_derived());
  for (TABLE_LIST *tbl= get_single_select()->get_table_list();
       tbl;
       tbl= tbl->next_local)
  {
    /*
      Merged view has also temporary table attached (in 5.2 if it has table
      then it was real table), so we have filter such temporary tables out
      by checking that it is not merged view
    */
    if (tbl->table &&
        !(tbl->is_view() &&
          tbl->is_merged_derived()))
    {
      if (tbl->table->map & map)
      {
	if (*table_arg)
	  return TRUE;
        *table_arg= tbl;
        tbl->check_option= view_arg->check_option;
      }
    }
    else if (tbl->check_single_table(table_arg, map, view_arg))
      return TRUE;
  }
  return FALSE;
}


/*
  Set insert_values buffer

  SYNOPSIS
    set_insert_values()
    mem_root   memory pool for allocating

  RETURN
    FALSE - OK
    TRUE  - out of memory
*/

bool TABLE_LIST::set_insert_values(MEM_ROOT *mem_root)
{
  DBUG_ENTER("set_insert_values");
  if (table)
  {
    DBUG_PRINT("info", ("setting insert_value for table"));
    if (!table->insert_values &&
        !(table->insert_values= (uchar *)alloc_root(mem_root,
                                                   table->s->rec_buff_length)))
      DBUG_RETURN(TRUE);
  }
  else
  {
    DBUG_PRINT("info", ("setting insert_value for view"));
    DBUG_ASSERT(is_view_or_derived() && is_merged_derived());
    for (TABLE_LIST *tbl=
           (TABLE_LIST*)view->first_select_lex()->table_list.first;
         tbl;
         tbl= tbl->next_local)
      if (tbl->set_insert_values(mem_root))
        DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


/*
  Test if this is a leaf with respect to name resolution.

  SYNOPSIS
    TABLE_LIST::is_leaf_for_name_resolution()

  DESCRIPTION
    A table reference is a leaf with respect to name resolution if
    it is either a leaf node in a nested join tree (table, view,
    schema table, subquery), or an inner node that represents a
    NATURAL/USING join, or a nested join with materialized join
    columns.

  RETURN
    TRUE if a leaf, FALSE otherwise.
*/
bool TABLE_LIST::is_leaf_for_name_resolution()
{
  return (is_merged_derived() || is_natural_join || is_join_columns_complete ||
          !nested_join);
}


/*
  Retrieve the first (left-most) leaf in a nested join tree with
  respect to name resolution.

  SYNOPSIS
    TABLE_LIST::first_leaf_for_name_resolution()

  DESCRIPTION
    Given that 'this' is a nested table reference, recursively walk
    down the left-most children of 'this' until we reach a leaf
    table reference with respect to name resolution.

  IMPLEMENTATION
    The left-most child of a nested table reference is the last element
    in the list of children because the children are inserted in
    reverse order.

  RETURN
    If 'this' is a nested table reference - the left-most child of
      the tree rooted in 'this',
    else return 'this'
*/

TABLE_LIST *TABLE_LIST::first_leaf_for_name_resolution()
{
  TABLE_LIST *UNINIT_VAR(cur_table_ref);
  NESTED_JOIN *cur_nested_join;

  if (is_leaf_for_name_resolution())
    return this;
  DBUG_ASSERT(nested_join);

  for (cur_nested_join= nested_join;
       cur_nested_join;
       cur_nested_join= cur_table_ref->nested_join)
  {
    List_iterator_fast<TABLE_LIST> it(cur_nested_join->join_list);
    cur_table_ref= it++;
    /*
      If the current nested join is a RIGHT JOIN, the operands in
      'join_list' are in reverse order, thus the first operand is
      already at the front of the list. Otherwise the first operand
      is in the end of the list of join operands.
    */
    if (!(cur_table_ref->outer_join & JOIN_TYPE_RIGHT))
    {
      TABLE_LIST *next;
      while ((next= it++))
        cur_table_ref= next;
    }
    if (cur_table_ref->is_leaf_for_name_resolution())
      break;
  }
  return cur_table_ref;
}


/*
  Retrieve the last (right-most) leaf in a nested join tree with
  respect to name resolution.

  SYNOPSIS
    TABLE_LIST::last_leaf_for_name_resolution()

  DESCRIPTION
    Given that 'this' is a nested table reference, recursively walk
    down the right-most children of 'this' until we reach a leaf
    table reference with respect to name resolution.

  IMPLEMENTATION
    The right-most child of a nested table reference is the first
    element in the list of children because the children are inserted
    in reverse order.

  RETURN
    - If 'this' is a nested table reference - the right-most child of
      the tree rooted in 'this',
    - else - 'this'
*/

TABLE_LIST *TABLE_LIST::last_leaf_for_name_resolution()
{
  TABLE_LIST *cur_table_ref= this;
  NESTED_JOIN *cur_nested_join;

  if (is_leaf_for_name_resolution())
    return this;
  DBUG_ASSERT(nested_join);

  for (cur_nested_join= nested_join;
       cur_nested_join;
       cur_nested_join= cur_table_ref->nested_join)
  {
    cur_table_ref= cur_nested_join->join_list.head();
    /*
      If the current nested is a RIGHT JOIN, the operands in
      'join_list' are in reverse order, thus the last operand is in the
      end of the list.
    */
    if ((cur_table_ref->outer_join & JOIN_TYPE_RIGHT))
    {
      List_iterator_fast<TABLE_LIST> it(cur_nested_join->join_list);
      TABLE_LIST *next;
      cur_table_ref= it++;
      while ((next= it++))
        cur_table_ref= next;
    }
    if (cur_table_ref->is_leaf_for_name_resolution())
      break;
  }
  return cur_table_ref;
}


/*
  Register access mode which we need for underlying tables

  SYNOPSIS
    register_want_access()
    want_access          Acess which we require
*/

void TABLE_LIST::register_want_access(privilege_t want_access)
{
  /* Remove SHOW_VIEW_ACL, because it will be checked during making view */
  want_access&= ~SHOW_VIEW_ACL;
  if (belong_to_view)
  {
    grant.want_privilege= want_access;
    if (table)
      table->grant.want_privilege= want_access;
  }
  if (!view)
    return;
  for (TABLE_LIST *tbl= view->first_select_lex()->get_table_list();
       tbl;
       tbl= tbl->next_local)
    tbl->register_want_access(want_access);
}


/*
  Load security context information for this view

  SYNOPSIS
    TABLE_LIST::prepare_view_security_context()
    thd                  [in] thread handler

  RETURN
    FALSE  OK
    TRUE   Error
*/

#ifndef NO_EMBEDDED_ACCESS_CHECKS
bool TABLE_LIST::prepare_view_security_context(THD *thd, bool upgrade_check)
{
  DBUG_ENTER("TABLE_LIST::prepare_view_security_context");
  DBUG_PRINT("enter", ("table: %s", alias.str));

  DBUG_ASSERT(!prelocking_placeholder && view);
  if (view_suid)
  {
    DBUG_PRINT("info", ("This table is suid view => load contest"));
    DBUG_ASSERT(view && view_sctx);
    if (acl_getroot(view_sctx, definer.user.str, definer.host.str,
                                definer.host.str, thd->db.str))
    {
      if ((thd->lex->sql_command == SQLCOM_SHOW_CREATE) ||
          (thd->lex->sql_command == SQLCOM_SHOW_FIELDS))
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, 
                            ER_NO_SUCH_USER, 
                            ER_THD(thd, ER_NO_SUCH_USER),
                            definer.user.str, definer.host.str);
      }
      else
      {
        if (thd->security_ctx->master_access & PRIV_REVEAL_MISSING_DEFINER)
        {
          my_error(ER_NO_SUCH_USER, MYF(upgrade_check ? ME_WARNING: 0),
                   definer.user.str, definer.host.str);
        }
        else
        {
          if (thd->password == 2)
            my_error(ER_ACCESS_DENIED_NO_PASSWORD_ERROR, MYF(0),
                     thd->security_ctx->priv_user,
                     thd->security_ctx->priv_host);
          else
            my_error(ER_ACCESS_DENIED_ERROR, MYF(0),
                     thd->security_ctx->priv_user,
                     thd->security_ctx->priv_host,
                     (thd->password ?  ER_THD(thd, ER_YES) :
                      ER_THD(thd, ER_NO)));
        }
        DBUG_RETURN(TRUE);
      }
    }
  }
  DBUG_RETURN(FALSE);

}
#endif


/*
  Find security context of current view

  SYNOPSIS
    TABLE_LIST::find_view_security_context()
    thd                  [in] thread handler

*/

#ifndef NO_EMBEDDED_ACCESS_CHECKS
Security_context *TABLE_LIST::find_view_security_context(THD *thd)
{
  Security_context *sctx;
  TABLE_LIST *upper_view= this;
  DBUG_ENTER("TABLE_LIST::find_view_security_context");

  DBUG_ASSERT(view);
  while (upper_view && !upper_view->view_suid)
  {
    DBUG_ASSERT(!upper_view->prelocking_placeholder);
    upper_view= upper_view->referencing_view;
  }
  if (upper_view)
  {
    DBUG_PRINT("info", ("Securety context of view %s will be used",
                        upper_view->alias.str));
    sctx= upper_view->view_sctx;
    DBUG_ASSERT(sctx);
  }
  else
  {
    DBUG_PRINT("info", ("Current global context will be used"));
    sctx= thd->security_ctx;
  }
  DBUG_RETURN(sctx);
}
#endif


/*
  Prepare security context and load underlying tables priveleges for view

  SYNOPSIS
    TABLE_LIST::prepare_security()
    thd                  [in] thread handler

  RETURN
    FALSE  OK
    TRUE   Error
*/

bool TABLE_LIST::prepare_security(THD *thd)
{
  List_iterator_fast<TABLE_LIST> tb(*view_tables);
  TABLE_LIST *tbl;
  DBUG_ENTER("TABLE_LIST::prepare_security");
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  /*
    Check if we are running REPAIR VIEW FOR UPGRADE
    In this case we are probably comming from mysql_upgrade and
    should not get an error for mysql.user table we just created.
  */
  bool upgrade_check= (thd->lex->sql_command == SQLCOM_REPAIR &&
                       (thd->lex->check_opt.sql_flags &
                        (TT_FOR_UPGRADE | TT_FROM_MYSQL)) &&
                       (thd->security_ctx->master_access &
                        PRIV_REVEAL_MISSING_DEFINER));
  Security_context *save_security_ctx= thd->security_ctx;

  DBUG_ASSERT(!prelocking_placeholder);
  if (prepare_view_security_context(thd, upgrade_check))
  {
    if (upgrade_check)
    {
      /* REPAIR needs SELECT_ACL */
      while ((tbl= tb++))
      {
        tbl->grant.privilege= SELECT_ACL;
        tbl->security_ctx= save_security_ctx;
      }
      DBUG_RETURN(FALSE);
    }
    DBUG_RETURN(TRUE);                          // Fatal
  }
  thd->security_ctx= find_view_security_context(thd);
  opt_trace_disable_if_no_security_context_access(thd);
  while ((tbl= tb++))
  {
    DBUG_ASSERT(tbl->referencing_view);
    const char *local_db, *local_table_name;
    if (tbl->view)
    {
      local_db= tbl->view_db.str;
      local_table_name= tbl->view_name.str;
    }
    else
    {
      local_db= tbl->db.str;
      local_table_name= tbl->table_name.str;
    }
    fill_effective_table_privileges(thd, &tbl->grant, local_db,
                                    local_table_name);
    if (tbl->table)
      tbl->table->grant= grant;
  }
  thd->security_ctx= save_security_ctx;
#else
  while ((tbl= tb++))
    tbl->grant.privilege= ALL_KNOWN_ACL;
#endif /* NO_EMBEDDED_ACCESS_CHECKS */
  DBUG_RETURN(FALSE);
}

#ifndef DBUG_OFF
void TABLE_LIST::set_check_merged()
{
  if (is_view())
    return;

  DBUG_ASSERT(derived);
  /*
    It is not simple to check all, but at least this should be checked:
    this select is not excluded or the exclusion came from above.
  */
  DBUG_ASSERT(derived->is_excluded() ||
              !derived->first_select()->exclude_from_table_unique_test ||
              derived->outer_select()->
              exclude_from_table_unique_test);
}
#endif

void TABLE_LIST::set_check_materialized()
{
  DBUG_ENTER("TABLE_LIST::set_check_materialized");
  SELECT_LEX_UNIT *derived= this->derived;
  if (view)
    derived= this->derived= &view->unit;
  DBUG_ASSERT(derived);
  DBUG_ASSERT(!derived->is_excluded());
  if (!derived->first_select()->exclude_from_table_unique_test)
    derived->set_unique_exclude();
  else
  {
    /*
      The subtree should be already excluded
    */
    DBUG_ASSERT(!derived->first_select()->first_inner_unit() ||
                derived->first_select()->first_inner_unit()->with_element ||
                derived->first_select()->first_inner_unit()->first_select()->
                exclude_from_table_unique_test);
  }
  DBUG_VOID_RETURN;
}

TABLE *TABLE_LIST::get_real_join_table()
{
  TABLE_LIST *tbl= this;
  while (tbl->table == NULL || tbl->table->reginfo.join_tab == NULL)
  {
    if ((tbl->view == NULL && tbl->derived == NULL) ||
        tbl->is_materialized_derived())
      break;
    /* we do not support merging of union yet */
    DBUG_ASSERT(tbl->view == NULL ||
               tbl->view->first_select_lex()->next_select() == NULL);
    DBUG_ASSERT(tbl->derived == NULL ||
               tbl->derived->first_select()->next_select() == NULL);

    {
      List_iterator_fast<TABLE_LIST>
        ti(tbl->view != NULL ?
           tbl->view->first_select_lex()->top_join_list :
           tbl->derived->first_select()->top_join_list);
      for (;;)
      {
        tbl= NULL;
        /*
          Find left table in outer join on this level
          (the list is reverted).
        */
        for (TABLE_LIST *t= ti++; t; t= ti++)
          tbl= t;
        if (!tbl)
          return NULL; // view/derived with no tables
        if (!tbl->nested_join)
          break;
        /* go deeper if we've found nested join */
        ti= tbl->nested_join->join_list;
      }
    }
  }

  return tbl->table;
}


Natural_join_column::Natural_join_column(Field_translator *field_param,
                                         TABLE_LIST *tab)
{
  DBUG_ASSERT(tab->field_translation);
  view_field= field_param;
  table_field= NULL;
  table_ref= tab;
  is_common= FALSE;
}


Natural_join_column::Natural_join_column(Item_field *field_param,
                                         TABLE_LIST *tab)
{
  DBUG_ASSERT(tab->table == field_param->field->table);
  table_field= field_param;
  view_field= NULL;
  table_ref= tab;
  is_common= FALSE;
}


LEX_CSTRING *Natural_join_column::name()
{
  if (view_field)
  {
    DBUG_ASSERT(table_field == NULL);
    return &view_field->name;
  }

  return &table_field->field_name;
}


Item *Natural_join_column::create_item(THD *thd)
{
  if (view_field)
  {
    DBUG_ASSERT(table_field == NULL);
    return create_view_field(thd, table_ref, &view_field->item,
                             &view_field->name);
  }
  return table_field;
}


Field *Natural_join_column::field()
{
  if (view_field)
  {
    DBUG_ASSERT(table_field == NULL);
    return NULL;
  }
  return table_field->field;
}


const char *Natural_join_column::safe_table_name()
{
  DBUG_ASSERT(table_ref);
  return table_ref->alias.str ? table_ref->alias.str : "";
}


const char *Natural_join_column::safe_db_name()
{
  if (view_field)
    return table_ref->view_db.str ? table_ref->view_db.str : "";

  /*
    Test that TABLE_LIST::db is the same as TABLE_SHARE::db to
    ensure consistency. An exception are I_S schema tables, which
    are inconsistent in this respect.
  */
  DBUG_ASSERT(!cmp(&table_ref->db, &table_ref->table->s->db) ||
              table_ref->table_function ||
              (table_ref->schema_table &&
               is_infoschema_db(&table_ref->table->s->db)) ||
              table_ref->is_materialized_derived());
  return table_ref->db.str ? table_ref->db.str : "";
}


GRANT_INFO *Natural_join_column::grant()
{
/*  if (view_field)
    return &(table_ref->grant);
  return &(table_ref->table->grant);*/
  /*
    Have to check algorithm because merged derived also has
    field_translation.
  */
//if (table_ref->effective_algorithm == DTYPE_ALGORITHM_MERGE)
  if (table_ref->is_merged_derived())
    return &(table_ref->grant);
  return &(table_ref->table->grant);
}


void Field_iterator_view::set(TABLE_LIST *table)
{
  DBUG_ASSERT(table->field_translation);
  view= table;
  ptr= table->field_translation;
  array_end= table->field_translation_end;
}


LEX_CSTRING *Field_iterator_table::name()
{
  return &(*ptr)->field_name;
}


Item *Field_iterator_table::create_item(THD *thd)
{
  SELECT_LEX *select= thd->lex->current_select;

  Item_field *item= new (thd->mem_root) Item_field(thd, &select->context, *ptr);
  DBUG_ASSERT(strlen(item->name.str) == item->name.length);
  if (item && thd->variables.sql_mode & MODE_ONLY_FULL_GROUP_BY &&
      !thd->lex->in_sum_func && select->cur_pos_in_select_list != UNDEF_POS &&
      select->join)
  {
    select->join->non_agg_fields.push_back(item);
    item->marker= select->cur_pos_in_select_list;
    select->set_non_agg_field_used(true);
  }
  return item;
}


LEX_CSTRING *Field_iterator_view::name()
{
  return &ptr->name;
}


Item *Field_iterator_view::create_item(THD *thd)
{
  return create_view_field(thd, view, &ptr->item, &ptr->name);
}

Item *create_view_field(THD *thd, TABLE_LIST *view, Item **field_ref,
                        LEX_CSTRING *name)
{
  bool save_wrapper= thd->lex->current_select->no_wrap_view_item;
  Item *field= *field_ref;
  DBUG_ENTER("create_view_field");

  if (view->schema_table_reformed)
  {
    /*
      Translation table items are always Item_fields and already fixed
      ('mysql_schema_table' function). So we can return directly the
      field. This case happens only for 'show & where' commands.
    */
    DBUG_ASSERT(field && field->fixed());
    DBUG_RETURN(field);
  }

  DBUG_ASSERT(field);
  thd->lex->current_select->no_wrap_view_item= TRUE;
  if (!field->fixed())
  {
    if (field->fix_fields(thd, field_ref))
    {
      thd->lex->current_select->no_wrap_view_item= save_wrapper;
      DBUG_RETURN(0);
    }
    field= *field_ref;
  }
  thd->lex->current_select->no_wrap_view_item= save_wrapper;
  if (save_wrapper)
  {
    DBUG_RETURN(field);
  }
  Name_resolution_context *context= (view->view ?
                                     &view->view->first_select_lex()->context:
                                     &thd->lex->first_select_lex()->context);
  Item *item= (new (thd->mem_root)
               Item_direct_view_ref(thd, context, field_ref, view->alias,
                                    *name, view));
  if (!item)
    return NULL;
  /*
    Force creation of nullable item for the result tmp table for outer joined
    views/derived tables.
  */
  if (view->table && view->table->maybe_null)
    item->set_maybe_null();
  /* Save item in case we will need to fall back to materialization. */
  view->used_items.push_front(item, thd->mem_root);
  /*
    If we create this reference on persistent memory then it should be
    present in persistent list
  */
  if (thd->mem_root == thd->stmt_arena->mem_root)
    view->persistent_used_items.push_front(item, thd->mem_root);
  DBUG_RETURN(item);
}


void Field_iterator_natural_join::set(TABLE_LIST *table_ref)
{
  DBUG_ASSERT(table_ref->join_columns);
  column_ref_it.init(*(table_ref->join_columns));
  cur_column_ref= column_ref_it++;
}


void Field_iterator_natural_join::next()
{
  cur_column_ref= column_ref_it++;
  DBUG_ASSERT(!cur_column_ref || ! cur_column_ref->table_field ||
              !cur_column_ref->table_field->field ||
              cur_column_ref->table_ref->table ==
              cur_column_ref->table_field->field->table);
}


void Field_iterator_table_ref::set_field_iterator()
{
  DBUG_ENTER("Field_iterator_table_ref::set_field_iterator");
  /*
    If the table reference we are iterating over is a natural join, or it is
    an operand of a natural join, and TABLE_LIST::join_columns contains all
    the columns of the join operand, then we pick the columns from
    TABLE_LIST::join_columns, instead of the  orginial container of the
    columns of the join operator.
  */
  if (table_ref->is_join_columns_complete)
  {
    /* Necesary, but insufficient conditions. */
    DBUG_ASSERT(table_ref->is_natural_join ||
                table_ref->nested_join ||
                (table_ref->join_columns &&
                 /* This is a merge view. */
                 ((table_ref->field_translation &&
                   table_ref->join_columns->elements ==
                   (ulong)(table_ref->field_translation_end -
                           table_ref->field_translation)) ||
                  /* This is stored table or a tmptable view. */
                  (!table_ref->field_translation &&
                   table_ref->join_columns->elements ==
                   table_ref->table->s->fields))));
    field_it= &natural_join_it;
    DBUG_PRINT("info",("field_it for '%s' is Field_iterator_natural_join",
                       table_ref->alias.str));
  }
  /* This is a merge view, so use field_translation. */
  else if (!table_ref->is_materialized_derived() &&
           table_ref->is_merged_derived() && table_ref->field_translation)
  {
    field_it= &view_field_it;
    DBUG_PRINT("info", ("field_it for '%s' is Field_iterator_view",
                        table_ref->alias.str));
  }
  /* This is a base table or stored view. */
  else
  {
    DBUG_ASSERT(table_ref->table || table_ref->is_materialized_derived());
    field_it= &table_field_it;
    DBUG_PRINT("info", ("field_it for '%s' is Field_iterator_table",
                        table_ref->alias.str));
  }
  field_it->set(table_ref);
  DBUG_VOID_RETURN;
}


void Field_iterator_table_ref::set(TABLE_LIST *table)
{
  DBUG_ASSERT(table);
  first_leaf= table->first_leaf_for_name_resolution();
  last_leaf=  table->last_leaf_for_name_resolution();
  DBUG_ASSERT(first_leaf && last_leaf);
  table_ref= first_leaf;
  set_field_iterator();
}


void Field_iterator_table_ref::next()
{
  /* Move to the next field in the current table reference. */
  field_it->next();
  /*
    If all fields of the current table reference are exhausted, move to
    the next leaf table reference.
  */
  if (field_it->end_of_fields() && table_ref != last_leaf)
  {
    table_ref= table_ref->next_name_resolution_table;
    DBUG_ASSERT(table_ref);
    set_field_iterator();
  }
}


const char *Field_iterator_table_ref::get_table_name()
{
  if (table_ref->view)
    return table_ref->view_name.str;
  if (table_ref->is_derived())
    return table_ref->table->s->table_name.str;
  else if (table_ref->is_natural_join)
    return natural_join_it.column_ref()->safe_table_name();

  DBUG_ASSERT(!strcmp(table_ref->table_name.str,
                      table_ref->table->s->table_name.str) ||
              table_ref->schema_table || table_ref->table_function);
  return table_ref->table_name.str;
}


const char *Field_iterator_table_ref::get_db_name()
{
  if (table_ref->view)
    return table_ref->view_db.str;
  else if (table_ref->is_natural_join)
    return natural_join_it.column_ref()->safe_db_name();

  /*
    Test that TABLE_LIST::db is the same as TABLE_SHARE::db to
    ensure consistency. An exception are I_S schema tables, which
    are inconsistent in this respect.
  */
  DBUG_ASSERT(!cmp(&table_ref->db, &table_ref->table->s->db) ||
              (table_ref->schema_table &&
               is_infoschema_db(&table_ref->table->s->db)) ||
               table_ref->table_function);

  return table_ref->db.str;
}


GRANT_INFO *Field_iterator_table_ref::grant()
{
  if (table_ref->view)
    return &(table_ref->grant);
  else if (table_ref->is_natural_join)
    return natural_join_it.column_ref()->grant();
  return &(table_ref->table->grant);
}


/*
  Create new or return existing column reference to a column of a
  natural/using join.

  SYNOPSIS
    Field_iterator_table_ref::get_or_create_column_ref()
    parent_table_ref  the parent table reference over which the
                      iterator is iterating

  DESCRIPTION
    Create a new natural join column for the current field of the
    iterator if no such column was created, or return an already
    created natural join column. The former happens for base tables or
    views, and the latter for natural/using joins. If a new field is
    created, then the field is added to 'parent_table_ref' if it is
    given, or to the original table referene of the field if
    parent_table_ref == NULL.

  NOTES
    This method is designed so that when a Field_iterator_table_ref
    walks through the fields of a table reference, all its fields
    are created and stored as follows:
    - If the table reference being iterated is a stored table, view or
      natural/using join, store all natural join columns in a list
      attached to that table reference.
    - If the table reference being iterated is a nested join that is
      not natural/using join, then do not materialize its result
      fields. This is OK because for such table references
      Field_iterator_table_ref iterates over the fields of the nested
      table references (recursively). In this way we avoid the storage
      of unnecessay copies of result columns of nested joins.

  RETURN
    #     Pointer to a column of a natural join (or its operand)
    NULL  No memory to allocate the column
*/

Natural_join_column *
Field_iterator_table_ref::get_or_create_column_ref(THD *thd, TABLE_LIST *parent_table_ref)
{
  Natural_join_column *nj_col;
  bool is_created= TRUE;
  uint UNINIT_VAR(field_count);
  TABLE_LIST *add_table_ref= parent_table_ref ?
                             parent_table_ref : table_ref;

  if (field_it == &table_field_it)
  {
    /* The field belongs to a stored table. */
    Field *tmp_field= table_field_it.field();
    Item_field *tmp_item=
      new (thd->mem_root) Item_field(thd, &thd->lex->current_select->context, tmp_field);
    if (!tmp_item)
      return NULL;
    nj_col= new Natural_join_column(tmp_item, table_ref);
    field_count= table_ref->table->s->fields;
  }
  else if (field_it == &view_field_it)
  {
    /* The field belongs to a merge view or information schema table. */
    Field_translator *translated_field= view_field_it.field_translator();
    nj_col= new Natural_join_column(translated_field, table_ref);
    field_count= (uint)(table_ref->field_translation_end -
                 table_ref->field_translation);
  }
  else
  {
    /*
      The field belongs to a NATURAL join, therefore the column reference was
      already created via one of the two constructor calls above. In this case
      we just return the already created column reference.
    */
    DBUG_ASSERT(table_ref->is_join_columns_complete);
    is_created= FALSE;
    nj_col= natural_join_it.column_ref();
    DBUG_ASSERT(nj_col);
  }
  DBUG_ASSERT(!nj_col->table_field || !nj_col->table_field->field ||
              nj_col->table_ref->table == nj_col->table_field->field->table);

  /*
    If the natural join column was just created add it to the list of
    natural join columns of either 'parent_table_ref' or to the table
    reference that directly contains the original field.
  */
  if (is_created)
  {
    /* Make sure not all columns were materialized. */
    DBUG_ASSERT(!add_table_ref->is_join_columns_complete);
    if (!add_table_ref->join_columns)
    {
      /* Create a list of natural join columns on demand. */
      if (!(add_table_ref->join_columns= new List<Natural_join_column>))
        return NULL;
      add_table_ref->is_join_columns_complete= FALSE;
    }
    add_table_ref->join_columns->push_back(nj_col);
    /*
      If new fields are added to their original table reference, mark if
      all fields were added. We do it here as the caller has no easy way
      of knowing when to do it.
      If the fields are being added to parent_table_ref, then the caller
      must take care to mark when all fields are created/added.
    */
    if (!parent_table_ref &&
        add_table_ref->join_columns->elements == field_count)
      add_table_ref->is_join_columns_complete= TRUE;
  }

  return nj_col;
}


/*
  Return an existing reference to a column of a natural/using join.

  SYNOPSIS
    Field_iterator_table_ref::get_natural_column_ref()

  DESCRIPTION
    The method should be called in contexts where it is expected that
    all natural join columns are already created, and that the column
    being retrieved is a Natural_join_column.

  RETURN
    #     Pointer to a column of a natural join (or its operand)
    NULL  We didn't originally have memory to allocate the column
*/

Natural_join_column *
Field_iterator_table_ref::get_natural_column_ref()
{
  Natural_join_column *nj_col;

  DBUG_ASSERT(field_it == &natural_join_it);
  /*
    The field belongs to a NATURAL join, therefore the column reference was
    already created via one of the two constructor calls above. In this case
    we just return the already created column reference.
  */
  nj_col= natural_join_it.column_ref();
  DBUG_ASSERT(nj_col &&
              (!nj_col->table_field || !nj_col->table_field->field ||
               nj_col->table_ref->table == nj_col->table_field->field->table));
  return nj_col;
}

/*****************************************************************************
  Functions to handle column usage bitmaps (read_set, write_set etc...)
*****************************************************************************/

/* Reset all columns bitmaps */

void TABLE::clear_column_bitmaps()
{
  /*
    Reset column read/write usage. It's identical to:
    bitmap_clear_all(&table->def_read_set);
    bitmap_clear_all(&table->def_write_set);
    The code assumes that the bitmaps are allocated after each other, as
    guaranteed by open_table_from_share()
  */
  bzero((char*) def_read_set.bitmap,
        s->column_bitmap_size * (s->virtual_fields ? 3 : 2));
  column_bitmaps_set(&def_read_set, &def_write_set);
  rpl_write_set= 0;                             // Safety
}


/*
  Tell handler we are going to call position() and rnd_pos() later.
  
  NOTES:
  This is needed for handlers that uses the primary key to find the
  row. In this case we have to extend the read bitmap with the primary
  key fields.
*/

void TABLE::prepare_for_position()
{
  DBUG_ENTER("TABLE::prepare_for_position");

  if ((file->ha_table_flags() & HA_PRIMARY_KEY_IN_READ_INDEX) &&
      s->primary_key < MAX_KEY)
  {
    mark_index_columns_for_read(s->primary_key);
    /* signal change */
    file->column_bitmaps_signal();
  }
  DBUG_VOID_RETURN;
}


MY_BITMAP *TABLE::prepare_for_keyread(uint index, MY_BITMAP *map)
{
  MY_BITMAP *backup= read_set;
  DBUG_ENTER("TABLE::prepare_for_keyread");
  if (!no_keyread && !file->keyread_enabled())
    file->ha_start_keyread(index);
  if (map != read_set || !is_clustering_key(index))
  {
    mark_index_columns(index, map);
    column_bitmaps_set(map);
  }
  DBUG_RETURN(backup);
}


/*
  Mark that only fields from one key is used. Useful before keyread.
*/

void TABLE::mark_index_columns(uint index, MY_BITMAP *bitmap)
{
  DBUG_ENTER("TABLE::mark_index_columns");

  bitmap_clear_all(bitmap);
  mark_index_columns_no_reset(index, bitmap);
  DBUG_VOID_RETURN;
}

/*
  Restore to use normal column maps after key read

  NOTES
    This reverse the change done by mark_index_columns

  WARNING
    For this to work, one must have the normal table maps in place
    when calling mark_index_columns
*/

void TABLE::restore_column_maps_after_keyread(MY_BITMAP *backup)
{
  DBUG_ENTER("TABLE::restore_column_maps_after_mark_index");
  file->ha_end_keyread();
  read_set= backup;
  file->column_bitmaps_signal();
  DBUG_VOID_RETURN;
}

static void do_mark_index_columns(TABLE *table, uint index,
                                  MY_BITMAP *bitmap, bool read)
{
  KEY_PART_INFO *key_part= table->key_info[index].key_part;
  uint key_parts= table->key_info[index].user_defined_key_parts;
  for (uint k= 0; k < key_parts; k++)
    if (read)
      key_part[k].field->register_field_in_read_map();
    else
      bitmap_set_bit(bitmap, key_part[k].fieldnr-1);
  if (table->file->ha_table_flags() & HA_PRIMARY_KEY_IN_READ_INDEX &&
      table->s->primary_key != MAX_KEY && table->s->primary_key != index)
    do_mark_index_columns(table, table->s->primary_key, bitmap, read);

  if (table->versioned(VERS_TRX_ID))
  {
    table->vers_start_field()->register_field_in_read_map();
    table->vers_end_field()->register_field_in_read_map();
  }
}
/*
  mark columns used by key, but don't reset other fields
*/

inline void TABLE::mark_index_columns_no_reset(uint index, MY_BITMAP *bitmap)
{
  do_mark_index_columns(this, index, bitmap, false);
}


inline void TABLE::mark_index_columns_for_read(uint index)
{
  do_mark_index_columns(this, index, read_set, true);
}

/*
  Mark auto-increment fields as used fields in both read and write maps

  NOTES
    This is needed in insert & update as the auto-increment field is
    always set and sometimes read.
*/

void TABLE::mark_auto_increment_column(bool is_insert)
{
  DBUG_ASSERT(found_next_number_field);
  /*
    We must set bit in read set as update_auto_increment() is using the
    store() to check overflow of auto_increment values
  */
  bitmap_set_bit(read_set, found_next_number_field->field_index);
  if (is_insert)
    bitmap_set_bit(write_set, found_next_number_field->field_index);
  if (s->next_number_keypart)
    mark_index_columns_for_read(s->next_number_index);
  file->column_bitmaps_signal();
}


/*
  Mark columns needed for doing an delete of a row

  DESCRIPTON
    Some table engines don't have a cursor on the retrieve rows
    so they need either to use the primary key or all columns to
    be able to delete a row.

    If the engine needs this, the function works as follows:
    - If primary key exits, mark the primary key columns to be read.
    - If not, mark all columns to be read

    If the engine has HA_REQUIRES_KEY_COLUMNS_FOR_DELETE, we will
    mark all key columns as 'to-be-read'. This allows the engine to
    loop over the given record to find all keys and doesn't have to
    retrieve the row again.
*/

void TABLE::mark_columns_needed_for_delete()
{
  bool need_signal= false;
  mark_columns_per_binlog_row_image();

  if (triggers)
    triggers->mark_fields_used(TRG_EVENT_DELETE);
  if (file->ha_table_flags() & HA_REQUIRES_KEY_COLUMNS_FOR_DELETE)
  {
    Field **reg_field;
    for (reg_field= field ; *reg_field ; reg_field++)
    {
      if ((*reg_field)->flags & (PART_KEY_FLAG | PART_INDIRECT_KEY_FLAG))
        mark_column_with_deps(*reg_field);
    }
    need_signal= true;
  }
  if (file->ha_table_flags() & HA_PRIMARY_KEY_REQUIRED_FOR_DELETE)
  {
    /*
      If the handler has no cursor capabilites, we have to read either
      the primary key, the hidden primary key or all columns to be
      able to do an delete
    */
    if (s->primary_key == MAX_KEY)
      file->use_hidden_primary_key();
    else
    {
      mark_index_columns_for_read(s->primary_key);
      need_signal= true;
    }
  }

  if (s->versioned)
  {
    bitmap_set_bit(read_set, s->vers.start_fieldno);
    bitmap_set_bit(read_set, s->vers.end_fieldno);
    bitmap_set_bit(write_set, s->vers.end_fieldno);
    need_signal= true;
  }
#ifdef HAVE_REPLICATION
  if (s->online_alter_binlog)
  {
    /*
      For online alter we have to read all columns, because we need PK columns
      in the row event, and we don't know what columns will be in PK after ALTER
    */
    bitmap_set_all(read_set);
    need_signal= true;
  }
#endif

  if (need_signal)
    file->column_bitmaps_signal();
}


/*
  Mark columns needed for doing an update of a row

  DESCRIPTON
    Some engines needs to have all columns in an update (to be able to
    build a complete row). If this is the case, we mark all not
    updated columns to be read.

    If this is no the case, we do like in the delete case and mark
    if needed, either the primary key column or all columns to be read.
    (see mark_columns_needed_for_delete() for details)

    If the engine has HA_REQUIRES_KEY_COLUMNS_FOR_DELETE, we will
    mark all USED key columns as 'to-be-read'. This allows the engine to
    loop over the given record to find all changed keys and doesn't have to
    retrieve the row again.
*/

void TABLE::mark_columns_needed_for_update()
{
  DBUG_ENTER("TABLE::mark_columns_needed_for_update");
  bool need_signal= false;


  if (triggers)
    triggers->mark_fields_used(TRG_EVENT_UPDATE);
  if (default_field)
    mark_default_fields_for_write(FALSE);
  if (vfield)
    need_signal|= mark_virtual_columns_for_write(FALSE);
  if (file->ha_table_flags() & HA_REQUIRES_KEY_COLUMNS_FOR_DELETE)
  {
    KEY *end= key_info + s->keys;
    for (KEY *k= key_info; k < end; k++)
    {
      KEY_PART_INFO *kpend= k->key_part + k->ext_key_parts;
      int any_written= 0, all_read= 1;
      for (KEY_PART_INFO *kp= k->key_part; kp < kpend; kp++)
      {
        int idx= kp->fieldnr - 1;
        any_written|= bitmap_is_set(write_set, idx);
        all_read&= bitmap_is_set(read_set, idx);
      }
      if (any_written && !all_read)
      {
        for (KEY_PART_INFO *kp= k->key_part; kp < kpend; kp++)
          mark_column_with_deps(field[kp->fieldnr - 1]);
      }
    }
    need_signal= true;
  }
  else
  {
    if (found_next_number_field)
      mark_auto_increment_column(false);
  }

  if (file->ha_table_flags() & HA_PRIMARY_KEY_REQUIRED_FOR_DELETE)
  {
    /*
      If the handler has no cursor capabilites, we have to read either
      the primary key, the hidden primary key or all columns to be
      able to do an update
    */
    if (s->primary_key == MAX_KEY)
      file->use_hidden_primary_key();
    else
    {
      mark_index_columns_for_read(s->primary_key);
      need_signal= true;
    }
  }
  if (s->versioned)
  {
    bitmap_set_bit(write_set, s->vers.start_fieldno);
    bitmap_set_bit(write_set, s->vers.end_fieldno);
    /*
      For System Versioning we have to read all columns since we store
      a copy of previous row with modified row_end back to a table.
    */
    bitmap_set_all(read_set);
    need_signal= true;
  }
#ifdef HAVE_REPLICATION
  if (s->online_alter_binlog)
  {
    /*
      For online alter we have to read all columns, because we need PK columns
      in the row event, and we don't know what columns will be in PK after ALTER
    */
    bitmap_set_all(read_set);
    need_signal= true;
  }
#endif
  if (check_constraints)
  {
    mark_check_constraint_columns_for_read();
    need_signal= true;
  }

  /*
    If a timestamp field settable on UPDATE is present then to avoid wrong
    update force the table handler to retrieve write-only fields to be able
    to compare records and detect data change.
  */
  if ((file->ha_table_flags() & HA_PARTIAL_COLUMN_READ) &&
      default_field && s->has_update_default_function)
  {
    bitmap_union(read_set, write_set);
    need_signal= true;
  }
  mark_columns_per_binlog_row_image();
  if (need_signal)
    file->column_bitmaps_signal();
  DBUG_VOID_RETURN;
}


/*
  Mark columns the handler needs for doing an insert

  For now, this is used to mark fields used by the trigger
  as changed.
*/

void TABLE::mark_columns_needed_for_insert()
{
  DBUG_ENTER("mark_columns_needed_for_insert");

  if (triggers)
  {
    /*
      We don't need to mark columns which are used by ON DELETE and
      ON UPDATE triggers, which may be invoked in case of REPLACE or
      INSERT ... ON DUPLICATE KEY UPDATE, since before doing actual
      row replacement or update write_record() will mark all table
      fields as used.
    */
    triggers->mark_fields_used(TRG_EVENT_INSERT);
  }
  if (found_next_number_field)
    mark_auto_increment_column(true);
  if (default_field)
    mark_default_fields_for_write(TRUE);
  if (s->versioned)
  {
    bitmap_set_bit(write_set, s->vers.start_fieldno);
    bitmap_set_bit(write_set, s->vers.end_fieldno);
    bitmap_set_bit(read_set, s->vers.end_fieldno);
  }
  /* Mark virtual columns for insert */
  if (vfield)
    mark_virtual_columns_for_write(TRUE);
  mark_columns_per_binlog_row_image();

  /* FULL_NODUP is for replacing FULL mode, insert includes all columns. */
  if (in_use->variables.binlog_row_image == BINLOG_ROW_IMAGE_FULL_NODUP)
    rpl_write_set= read_set;

  if (check_constraints)
    mark_check_constraint_columns_for_read();
  DBUG_VOID_RETURN;
}

/*
  Mark columns according the binlog row image option
  or mark virtual columns for slave.

  Columns to be written are stored in 'rpl_write_set'

  When logging in RBR, the user can select whether to
  log partial or full rows, depending on the table
  definition, and the value of binlog_row_image.

  Semantics of the binlog_row_image are the following
  (PKE - primary key equivalent, ie, PK fields if PK
  exists, all fields otherwise):

  binlog_row_image= MINIMAL
    - This marks the PKE fields in the read_set
    - This marks all fields where a value was specified
      in the rpl_write_set

  binlog_row_image= NOBLOB
    - This marks PKE + all non-blob fields in the read_set
    - This marks all fields where a value was specified
      and all non-blob fields in the rpl_write_set

  binlog_row_image= FULL
    - all columns in the read_set
    - all columns in the rpl_write_set

  This marking is done without resetting the original
  bitmaps. This means that we will strip extra fields in
  the read_set at binlogging time (for those cases that
  we only want to log a PK and we needed other fields for
  execution).

  If binlog row image is off on slave we mark virtual columns
  for read as InnoDB requires correct field metadata which is set
  by update_virtual_fields().
*/

void TABLE::mark_columns_per_binlog_row_image()
{
  THD *thd= in_use;
  DBUG_ENTER("mark_columns_per_binlog_row_image");
  DBUG_ASSERT(read_set->bitmap);
  DBUG_ASSERT(write_set->bitmap);

  /**
    If in RBR we may need to mark some extra columns,
    depending on the binlog-row-image command line argument.
   */
  if (file->row_logging &&
      !ha_check_storage_engine_flag(s->db_type(), HTON_NO_BINLOG_ROW_OPT))
  {
#ifdef WITH_WSREP
    /**
     The marking of all columns will prevent update/set column values for the
     sequence table. For the sequence table column bitmap sent from master is
     used.
    */
    if (WSREP(thd) && wsrep_thd_is_applying(thd) &&
        s->sequence && s->primary_key >= MAX_KEY)
    {
      DBUG_VOID_RETURN;
    }
#endif /* WITH_WSREP */

    /* if there is no PK, then mark all columns for the BI. */
    if (s->primary_key >= MAX_KEY)
    {
      bitmap_set_all(read_set);
      rpl_write_set= read_set;
    }
    else
    {
      switch (thd->variables.binlog_row_image) {
      case BINLOG_ROW_IMAGE_FULL:
        bitmap_set_all(read_set);
        /* Set of columns that should be written (all) */
        rpl_write_set= read_set;
        break;
      case BINLOG_ROW_IMAGE_FULL_NODUP:
        bitmap_set_all(read_set);
        // TODO: After MDEV-18432 we don't pass history rows, so remove this:
        rpl_write_set= versioned() ? &s->all_set : write_set;
        break;
      case BINLOG_ROW_IMAGE_NOBLOB:
        /* Only write changed columns + not blobs */
        rpl_write_set= &def_rpl_write_set;
        bitmap_copy(rpl_write_set, write_set);

        /*
          for every field that is not set, mark it unless it is a blob or
          part of a primary key
        */
        for (Field **ptr=field ; *ptr ; ptr++)
        {
          Field *my_field= *ptr;
          /*
            bypass blob fields. These can be set or not set, we don't care.
            Later, at binlogging time, if we don't need them in the before
            image, we will discard them.

            If set in the AI, then the blob is really needed, there is
            nothing we can do about it.
          */
          if ((my_field->flags & PRI_KEY_FLAG) ||
              (my_field->type() != MYSQL_TYPE_BLOB))
          {
            my_field->register_field_in_read_map();
            bitmap_set_bit(rpl_write_set, my_field->field_index);
          }
        }
        break;
      case BINLOG_ROW_IMAGE_MINIMAL:
        /*
          mark the primary key in the read set so that we can find the row
          that is updated / deleted.
          We don't need to mark the primary key in the rpl_write_set as the
          binary log will include all columns read anyway.
        */
        mark_index_columns_for_read(s->primary_key);
        if (versioned())
        {
          // TODO: After MDEV-18432 we don't pass history rows, so remove this:
          rpl_write_set= &s->all_set;
        }
        else
        {
          /* Only write columns that have changed */
          rpl_write_set= write_set;
        }
        break;

      default:
        DBUG_ASSERT(FALSE);
      }
    }
    file->column_bitmaps_signal();
  }
  else
  {
    /* If not using row format */
    rpl_write_set= write_set;
    file->column_bitmaps_signal();
  }

  DBUG_VOID_RETURN;
}


/* 
  @brief Mark virtual columns for update/insert commands

  @param insert_fl    true if virtual columns are marked for insert command
                      For the moment this is not used, may be used in future.

  @details
    The function marks virtual columns used in a update/insert commands
    in the vcol_set bitmap.
    For an insert command a virtual column is always marked in write_set if
    it is a stored column.
    If a virtual column is from write_set it is always marked in vcol_set.
    If a stored virtual column is not from write_set but it is computed
    through columns from write_set it is also marked in vcol_set, and,
    besides, it is added to write_set. 

  @return whether a bitmap was updated

  @note
    Let table t1 have columns a,b,c and let column c be a stored virtual 
    column computed through columns a and b. Then for the query
      UPDATE t1 SET a=1
    column c will be placed into vcol_set and into write_set while
    column b will be placed into read_set.
    If column c was a virtual column, but not a stored virtual column
    then it would not be added to any of the sets. Column b would not
    be added to read_set either.
*/

bool TABLE::mark_virtual_columns_for_write(bool insert_fl
                                           __attribute__((unused)))
{
  Field **vfield_ptr, *tmp_vfield;
  bool bitmap_updated= false;
  DBUG_ENTER("mark_virtual_columns_for_write");

  for (vfield_ptr= vfield; *vfield_ptr; vfield_ptr++)
  {
    tmp_vfield= *vfield_ptr;
    if (bitmap_is_set(write_set, tmp_vfield->field_index))
      bitmap_updated|= mark_virtual_column_with_deps(tmp_vfield);
    else if (tmp_vfield->vcol_info->is_stored() ||
             (tmp_vfield->flags & (PART_KEY_FLAG | FIELD_IN_PART_FUNC_FLAG |
                                   PART_INDIRECT_KEY_FLAG)))
    {
      bitmap_set_bit(write_set, tmp_vfield->field_index);
      mark_virtual_column_with_deps(tmp_vfield);
      bitmap_updated= true;
    }
  }
  if (bitmap_updated)
    file->column_bitmaps_signal();
  DBUG_RETURN(bitmap_updated);
}


/**
   Check if a virtual not stored column field is in read set

   @retval FALSE  No virtual not stored column is used
   @retval TRUE   At least one virtual not stored column is used
*/

bool TABLE::check_virtual_columns_marked_for_read()
{
  if (vfield)
  {
    Field **vfield_ptr;
    for (vfield_ptr= vfield; *vfield_ptr; vfield_ptr++)
    {
      Field *tmp_vfield= *vfield_ptr;
      if (bitmap_is_set(read_set, tmp_vfield->field_index) &&
          !tmp_vfield->vcol_info->is_stored())
        return TRUE;
    }
  }
  return FALSE;
}


/**
   Check if a stored virtual column field is marked for write

   This can be used to check if any column that is part of a virtual
   stored column is changed

   @retval FALSE  No stored virtual column is used
   @retval TRUE   At least one stored virtual column is used
*/

bool TABLE::check_virtual_columns_marked_for_write()
{
  if (vfield)
  {
    Field **vfield_ptr;
    for (vfield_ptr= vfield; *vfield_ptr; vfield_ptr++)
    {
      Field *tmp_vfield= *vfield_ptr;
      if (bitmap_is_set(write_set, tmp_vfield->field_index) &&
                        tmp_vfield->vcol_info->is_stored())
        return TRUE;
    }
  }
  return FALSE;
}


/*
  Mark fields used by check constraints into s->check_set.
  Mark all fields used in an expression that is part of an index
  with PART_INDIRECT_KEY_FLAG

  This is done once for the TABLE_SHARE the first time the table is opened.
  The marking must be done non-destructively to handle the case when
  this could be run in parallely by two threads
*/

void TABLE::mark_columns_used_by_virtual_fields(void)
{
  MY_BITMAP *save_read_set;
  Field **vfield_ptr;
  TABLE_SHARE::enum_v_keys v_keys= TABLE_SHARE::NO_V_KEYS;

  /* If there is virtual fields are already initialized */
  if (s->check_set_initialized)
    return;

  if (s->tmp_table == NO_TMP_TABLE)
    mysql_mutex_lock(&s->LOCK_share);
  if (s->check_set)
  {
    /* Mark fields used by check constraint */
    save_read_set= read_set;
    read_set= s->check_set;

    for (Virtual_column_info **chk= check_constraints ; *chk ; chk++)
      (*chk)->expr->walk(&Item::register_field_in_read_map, 1, 0);
    read_set= save_read_set;
  }

  /*
    mark all fields that part of a virtual indexed field with
    PART_INDIRECT_KEY_FLAG. This is used to ensure that all fields
    that are part of an index exits before write/delete/update.

    As this code is only executed once per open share, it's reusing
    existing functionality instead of adding an extra argument to
    add_field_to_set_processor or adding another processor.
  */
  if (vfield)
  {
    for (vfield_ptr= vfield; *vfield_ptr; vfield_ptr++)
    {
      if ((*vfield_ptr)->flags & PART_KEY_FLAG)
        (*vfield_ptr)->vcol_info->expr->walk(&Item::add_field_to_set_processor,
                                             1, this);
    }
    for (uint i= 0 ; i < s->fields ; i++)
    {
      if (bitmap_is_set(&tmp_set, i))
      {
        s->field[i]->flags|= PART_INDIRECT_KEY_FLAG;
        v_keys= TABLE_SHARE::V_KEYS;
      }
    }
    bitmap_clear_all(&tmp_set);
  }
  s->check_set_initialized= v_keys;
  if (s->tmp_table == NO_TMP_TABLE)
    mysql_mutex_unlock(&s->LOCK_share);
}

/* Add fields used by CHECK CONSTRAINT to read map */

void TABLE::mark_check_constraint_columns_for_read(void)
{
  bitmap_union(read_set, s->check_set);
}


/**
  Add all fields that have a default function to the table write set.
*/

void TABLE::mark_default_fields_for_write(bool is_insert)
{
  DBUG_ENTER("mark_default_fields_for_write");
  Field **field_ptr, *field;
  for (field_ptr= default_field; *field_ptr; field_ptr++)
  {
    field= (*field_ptr);
    if (is_insert && field->default_value)
    {
      bitmap_set_bit(write_set, field->field_index);
      field->default_value->expr->
        walk(&Item::register_field_in_read_map, 1, 0);
    }
    else if (!is_insert && field->has_update_default_function())
      bitmap_set_bit(write_set, field->field_index);
  }
  DBUG_VOID_RETURN;
}


void TABLE::move_fields(Field **ptr, const uchar *to, const uchar *from)
{
  my_ptrdiff_t diff= to - from;
  if (diff)
  {
    do
    {
      (*ptr)->move_field_offset(diff);
    } while (*(++ptr));
  }
}


/*
  Store all allocated virtual fields blob values
  Used by InnoDB when calculating virtual fields for it's own internal
  records
*/

void TABLE::remember_blob_values(String *blob_storage)
{
  Field **vfield_ptr;
  for (vfield_ptr= vfield; *vfield_ptr; vfield_ptr++)
  {
    if ((*vfield_ptr)->type() == MYSQL_TYPE_BLOB &&
        !(*vfield_ptr)->vcol_info->is_stored())
    {
      Field_blob *blob= ((Field_blob*) *vfield_ptr);
      memcpy((void*) blob_storage, (void*) &blob->value, sizeof(blob->value));
      blob_storage++;
      blob->value.release();
    }
  }
}


/*
  Restore all allocated virtual fields blob values
  Used by InnoDB when calculating virtual fields for it's own internal
  records
*/

void TABLE::restore_blob_values(String *blob_storage)
{
  Field **vfield_ptr;
  for (vfield_ptr= vfield; *vfield_ptr; vfield_ptr++)
  {
    if ((*vfield_ptr)->type() == MYSQL_TYPE_BLOB &&
        !(*vfield_ptr)->vcol_info->is_stored())
    {
      Field_blob *blob= ((Field_blob*) *vfield_ptr);
      blob->value.free();
      memcpy((void*) &blob->value, (void*) blob_storage, sizeof(blob->value));
      blob_storage++;
    }
  }
}


/**
  @brief
  Allocate space for keys

  @param key_count  number of keys to allocate additionally

  @details
  The function allocates memory to fit additionally 'key_count' keys
  for this table.

  @return FALSE   space was successfully allocated
  @return TRUE    an error occur
*/

bool TABLE::alloc_keys(uint key_count)
{
  KEY *new_key_info;
  key_part_map *new_const_key_parts;
  DBUG_ASSERT(s->tmp_table == INTERNAL_TMP_TABLE);

  if (!multi_alloc_root(&mem_root,
                        &new_key_info, sizeof(*key_info)*(s->keys+key_count),
                        &new_const_key_parts,
                        sizeof(*new_const_key_parts)*(s->keys+key_count),
                        NullS))
    return TRUE;
  if (s->keys)
  {
    memmove(new_key_info, s->key_info, sizeof(*key_info) * s->keys);
    memmove(new_const_key_parts, const_key_parts,
            s->keys * sizeof(const_key_parts));
  }
  s->key_info= key_info= new_key_info;
  const_key_parts= new_const_key_parts;
  bzero((char*) (const_key_parts + s->keys),
        sizeof(*const_key_parts) * key_count);
  max_keys= s->keys+key_count;
  return FALSE;
}


/**
  @brief
  Populate a KEY_PART_INFO structure with the data related to a field entry.

  @param key_part_info  The structure to fill.
  @param field          The field entry that represents the key part.
  @param fleldnr        The number of the field, count starting from 1.

  TODO: This method does not make use of any table specific fields. It
  could be refactored to act as a constructor for KEY_PART_INFO instead.
*/

void TABLE::create_key_part_by_field(KEY_PART_INFO *key_part_info,
                                     Field *field, uint fieldnr)
{
  DBUG_ASSERT(field->field_index + 1 == (int)fieldnr);
  key_part_info->null_bit= field->null_bit;
  key_part_info->null_offset= (uint) (field->null_ptr -
                                      (uchar*) record[0]);
  key_part_info->field= field;
  key_part_info->fieldnr= fieldnr;
  key_part_info->offset= field->offset(record[0]);
  /*
     field->key_length() accounts for the raw length of the field, excluding
     any metadata such as length of field or the NULL flag.
  */
  key_part_info->length= (uint16) field->key_length();
  key_part_info->key_part_flag= 0;
  /* TODO:
    The below method of computing the key format length of the
    key part is a copy/paste from opt_range.cc, and table.cc.
    This should be factored out, e.g. as a method of Field.
    In addition it is not clear if any of the Field::*_length
    methods is supposed to compute the same length. If so, it
    might be reused.
  */
  key_part_info->store_length= key_part_info->length;
  /*
    For BIT fields null_bit is not set to 0 even if the field is defined
    as NOT NULL, look at Field_bit::Field_bit
  */
  if (!field->real_maybe_null())
  {
    key_part_info->null_bit= 0;
  }

  /*
     The total store length of the key part is the raw length of the field +
     any metadata information, such as its length for strings and/or the null
     flag.
  */
  if (field->real_maybe_null())
  {
    key_part_info->store_length+= HA_KEY_NULL_LENGTH;
  }

  key_part_info->key_part_flag|= field->key_part_flag();
  key_part_info->store_length+= field->key_part_length_bytes();

  key_part_info->type=     (uint8) field->key_type();
  key_part_info->key_type =
    ((ha_base_keytype) key_part_info->type == HA_KEYTYPE_TEXT ||
    (ha_base_keytype) key_part_info->type == HA_KEYTYPE_VARTEXT1 ||
    (ha_base_keytype) key_part_info->type == HA_KEYTYPE_VARTEXT2) ?
    0 : FIELDFLAG_BINARY;
}


/**
  @brief
  Check validity of a possible key for the derived table

  @param key            the number of the key
  @param key_parts      number of components of the key
  @param next_field_no  the call-back function that returns the number of
                        the field used as the next component of the key
  @param arg            the argument for the above function

  @details
  The function checks whether a possible key satisfies the constraints
  imposed on the keys of any temporary table.

  We need to filter out BLOB columns here, because ref access optimizer creates
  KEYUSE objects for equalities for non-key columns for two puproses:
  1. To discover possible keys for derived_with_keys optimization
  2. To do hash joins
  For the purpose of #1, KEYUSE objects are not created for "blob_column=..." .
  However, they might be created for #2. In order to catch that case, we filter
  them out here.

  @return TRUE if the key is valid
  @return FALSE otherwise
*/

bool TABLE::check_tmp_key(uint key, uint key_parts,
                          uint (*next_field_no) (uchar *), uchar *arg)
{
  Field **reg_field;
  uint i;
  uint key_len= 0;

  for (i= 0; i < key_parts; i++)
  {
    uint fld_idx= next_field_no(arg);
    reg_field= field + fld_idx;
    if ((*reg_field)->type() == MYSQL_TYPE_BLOB)
      return FALSE;
    uint fld_store_len= (uint16) (*reg_field)->key_length();
    if ((*reg_field)->real_maybe_null())
      fld_store_len+= HA_KEY_NULL_LENGTH;
    if ((*reg_field)->real_type() == MYSQL_TYPE_VARCHAR ||
        (*reg_field)->type() == MYSQL_TYPE_GEOMETRY)
      fld_store_len+= HA_KEY_BLOB_LENGTH;
    key_len+= fld_store_len;
  }
  /*
    We use MI_MAX_KEY_LENGTH (myisam's default) below because it is
    smaller than MAX_KEY_LENGTH (heap's default) and it's unknown whether
    myisam or heap will be used for the temporary table.
  */
  return key_len <= MI_MAX_KEY_LENGTH;
}

/**
  @brief
  Add one key to a temporary table

  @param key            the number of the key
  @param key_parts      number of components of the key
  @param next_field_no  the call-back function that returns the number of
                        the field used as the next component of the key
  @param arg            the argument for the above function
  @param unique         TRUE <=> it is a unique index

  @details
  The function adds a new key to the table that is assumed to be a temporary
  table. At each its invocation the call-back function must return
  the number of the field that is used as the next component of this key.

  @return FALSE is a success
  @return TRUE if a failure

*/

bool TABLE::add_tmp_key(uint key, uint key_parts,
                        uint (*next_field_no) (uchar *), uchar *arg,
                        bool unique)
{
  DBUG_ASSERT(key < max_keys);

  char buf[NAME_CHAR_LEN];
  KEY *keyinfo= key_info + key;
  KEY_PART_INFO *key_part_info;
  Field **reg_field;
  uint i;
  bool key_start= TRUE;

  keyinfo->name.length= sprintf(buf, "key%i", key);

  if (!multi_alloc_root(&mem_root,
                        &key_part_info, sizeof(KEY_PART_INFO)*key_parts,
                        &keyinfo->rec_per_key,
                        sizeof(key_info->rec_per_key) * key_parts,
                        &keyinfo->name.str, keyinfo->name.length+1,
                        NullS))
    return TRUE;
  keyinfo->key_part= key_part_info;
  strmake((char*) keyinfo->name.str, buf, keyinfo->name.length);

  keyinfo->usable_key_parts= keyinfo->user_defined_key_parts= key_parts;
  keyinfo->ext_key_parts= keyinfo->user_defined_key_parts;
  keyinfo->key_length=0;
  keyinfo->algorithm= HA_KEY_ALG_UNDEF;
  keyinfo->flags= HA_GENERATED_KEY;
  keyinfo->ext_key_flags= keyinfo->flags;
  keyinfo->is_statistics_from_stat_tables= FALSE;
  if (unique)
    keyinfo->flags|= HA_NOSAME;
  bzero(keyinfo->rec_per_key, sizeof(ulong)*key_parts);
  keyinfo->read_stats= NULL;
  keyinfo->collected_stats= NULL;

  for (i= 0; i < key_parts; i++)
  {
    uint fld_idx= next_field_no(arg); 
    reg_field= field + fld_idx;
    if (key_start)
      (*reg_field)->key_start.set_bit(key);
    (*reg_field)->part_of_key.set_bit(key);
    create_key_part_by_field(key_part_info, *reg_field, fld_idx+1);
    keyinfo->key_length += key_part_info->store_length;
    (*reg_field)->flags|= PART_KEY_FLAG;
    key_start= FALSE;
    key_part_info++;
  }
  /*
    We have to cache index_flags here as the table may be used by the
    optimizer before it's opened.
  */
  keyinfo->index_flags= file->index_flags(key, 0, 1);

  /*
    For the case when there is a derived table that would give distinct rows,
    the index statistics are passed to the join optimizer to tell that a ref
    access to all the fields of the derived table will produce only one row.
  */

  st_select_lex_unit* derived= pos_in_table_list ?
                               pos_in_table_list->derived: NULL;
  if (derived)
  {
    st_select_lex* first= derived->first_select();
    uint select_list_items= first->get_item_list()->elements;
    if (key_parts == select_list_items)
    {
      if ((!first->is_part_of_union() && (first->options & SELECT_DISTINCT)) ||
          derived->check_distinct_in_union())
        keyinfo->rec_per_key[key_parts - 1]= 1;
    }
  }

  set_if_bigger(s->max_key_length, keyinfo->key_length);
  s->keys++;
  s->ext_key_parts+= keyinfo->ext_key_parts;
  s->key_parts+= keyinfo->user_defined_key_parts;
  return FALSE;
}

/*
  @brief
  Drop all indexes except specified one and optionally unique keys.

  @param  key_to_save   The key to save
  @param  map_to_update Bitmap showing some of the table's keys. Update it
                        to show the same keys, if they are not dropped.
  @param  unique_keys   Keep unique keys

  @details
  Drop all indexes on this table except 'key_to_save' and unique keys.

  The saved key becomes key #0. If key_to_save=-1 then only unique keys
  remain.
*/

void TABLE::use_index(int key_to_save, key_map *map_to_update)
{
  DBUG_ASSERT(!created && key_to_save < (int)s->keys);
  uint saved_keys= 0, key_parts= 0;
  key_map new_bitmap;
  new_bitmap.clear_all();

  /*
    If we have key_to_save, move it to be key#0.
  */
  if (key_to_save != -1)
  {
    new_bitmap.set_bit(saved_keys);

    KEY tmp_buff= key_info[saved_keys];
    key_info[saved_keys]= key_info[key_to_save];
    key_info[key_to_save]= tmp_buff;
    key_parts= key_info[saved_keys].user_defined_key_parts;
    saved_keys++;
  }

  /*
    Now, move all unique keys to the front.
  */
  for (uint i= saved_keys; i < s->keys; i++)
  {
    if (key_info[i].flags & HA_NOSAME)
    {
      if (map_to_update->is_set(i))
        new_bitmap.set_bit(saved_keys);
      if (i != saved_keys)
        key_info[saved_keys]= key_info[i];
      key_parts+= key_info[saved_keys].user_defined_key_parts;
      saved_keys++;
    }
  }
  *map_to_update= new_bitmap;
  s->keys= saved_keys;
  s->key_parts= s->ext_key_parts= key_parts;
}

/*
  Return TRUE if the table is filled at execution phase 
  
  (and so, the optimizer must not do anything that depends on the contents of
   the table, like range analysis or constant table detection)
*/

bool TABLE::is_filled_at_execution()
{ 
  /*
    pos_in_table_list == NULL for internal temporary tables because they
    do not have a corresponding table reference. Such tables are filled
    during execution.
  */
  return MY_TEST(!pos_in_table_list ||
                 pos_in_table_list->jtbm_subselect ||
                 pos_in_table_list->is_active_sjm() ||
                 pos_in_table_list->table_function);
}


/**
  @brief
  Get actual number of key components

  @param keyinfo

  @details
  The function calculates actual number of key components, possibly including
  components of extended keys, taken into consideration by the optimizer for the
  key described by the parameter keyinfo.

  @return number of considered key components
*/ 

uint TABLE::actual_n_key_parts(KEY *keyinfo)
{
  return optimizer_flag(in_use, OPTIMIZER_SWITCH_EXTENDED_KEYS) ?
           keyinfo->ext_key_parts : keyinfo->user_defined_key_parts;
}

 
/**
  @brief
  Get actual key flags for a table key 

  @param keyinfo

  @details
  The function finds out actual key flags taken into consideration by the
  optimizer for the key described by the parameter keyinfo.

  @return actual key flags
*/ 

ulong TABLE::actual_key_flags(KEY *keyinfo)
{
  return optimizer_flag(in_use, OPTIMIZER_SWITCH_EXTENDED_KEYS) ?
           keyinfo->ext_key_flags : keyinfo->flags;
} 


/*
  Cleanup this table for re-execution.

  SYNOPSIS
    TABLE_LIST::reinit_before_use()
*/

void TABLE_LIST::reinit_before_use(THD *thd)
{
  /*
    Reset old pointers to TABLEs: they are not valid since the tables
    were closed in the end of previous prepare or execute call.
  */
  table= 0;
  /* Reset is_schema_table_processed value(needed for I_S tables */
  schema_table_state= NOT_PROCESSED;

  TABLE_LIST *embedded; /* The table at the current level of nesting. */
  TABLE_LIST *parent_embedding= this; /* The parent nested table reference. */
  do
  {
    embedded= parent_embedding;
    if (embedded->prep_on_expr)
      embedded->on_expr= embedded->prep_on_expr->copy_andor_structure(thd);
    parent_embedding= embedded->embedding;
  }
  while (parent_embedding &&
         parent_embedding->nested_join->join_list.head() == embedded);

  mdl_request.ticket= NULL;
}


/*
  Return subselect that contains the FROM list this table is taken from

  SYNOPSIS
    TABLE_LIST::containing_subselect()
 
  RETURN
    Subselect item for the subquery that contains the FROM list
    this table is taken from if there is any
    0 - otherwise

*/

Item_subselect *TABLE_LIST::containing_subselect()
{
  return (select_lex ? select_lex->master_unit()->item : 0);
}

/*
  Compiles the tagged hints list and fills up the bitmasks.

  SYNOPSIS
    process_index_hints()
      table         the TABLE to operate on.

  DESCRIPTION
    The parser collects the index hints for each table in a "tagged list" 
    (TABLE_LIST::index_hints). Using the information in this tagged list
    this function sets the members TABLE::keys_in_use_for_query,
    TABLE::keys_in_use_for_group_by, TABLE::keys_in_use_for_order_by,
    TABLE::force_index, TABLE::force_index_order,
    TABLE::force_index_group and TABLE::covering_keys.

    Current implementation of the runtime does not allow mixing FORCE INDEX
    and USE INDEX, so this is checked here. Then the FORCE INDEX list 
    (if non-empty) is appended to the USE INDEX list and a flag is set.

    Multiple hints of the same kind are processed so that each clause 
    is applied to what is computed in the previous clause.
    For example:
        USE INDEX (i1) USE INDEX (i2)
    is equivalent to
        USE INDEX (i1,i2)
    and means "consider only i1 and i2".

    Similarly
        USE INDEX () USE INDEX (i1)
    is equivalent to
        USE INDEX (i1)
    and means "consider only the index i1"

    It is OK to have the same index several times, e.g. "USE INDEX (i1,i1)" is
    not an error.

    Different kind of hints (USE/FORCE/IGNORE) are processed in the following
    order:
      1. All indexes in USE (or FORCE) INDEX are added to the mask.
      2. All IGNORE INDEX

    e.g. "USE INDEX i1, IGNORE INDEX i1, USE INDEX i1" will not use i1 at all
    as if we had "USE INDEX i1, USE INDEX i1, IGNORE INDEX i1".

    As an optimization if there is a covering index, and we have 
    IGNORE INDEX FOR GROUP/ORDER, and this index is used for the JOIN part, 
    then we have to ignore the IGNORE INDEX FROM GROUP/ORDER.

  RETURN VALUE
    FALSE                no errors found
    TRUE                 found and reported an error.
*/
bool TABLE_LIST::process_index_hints(TABLE *tbl)
{
  /* initialize the result variables */
  tbl->keys_in_use_for_query= tbl->keys_in_use_for_group_by= 
    tbl->keys_in_use_for_order_by= tbl->s->usable_indexes(tbl->in_use);

  /* index hint list processing */
  if (index_hints)
  {
    key_map index_join[INDEX_HINT_FORCE + 1];
    key_map index_order[INDEX_HINT_FORCE + 1];
    key_map index_group[INDEX_HINT_FORCE + 1];
    Index_hint *hint;
    int type;
    bool have_empty_use_join= FALSE, have_empty_use_order= FALSE, 
         have_empty_use_group= FALSE;
    List_iterator <Index_hint> iter(*index_hints);

    /* initialize temporary variables used to collect hints of each kind */
    for (type= INDEX_HINT_IGNORE; type <= INDEX_HINT_FORCE; type++)
    {
      index_join[type].clear_all();
      index_order[type].clear_all();
      index_group[type].clear_all();
    }

    /* iterate over the hints list */
    while ((hint= iter++))
    {
      uint pos;

      /* process empty USE INDEX () */
      if (hint->type == INDEX_HINT_USE && !hint->key_name.str)
      {
        if (hint->clause & INDEX_HINT_MASK_JOIN)
        {
          index_join[hint->type].clear_all();
          have_empty_use_join= TRUE;
        }
        if (hint->clause & INDEX_HINT_MASK_ORDER)
        {
          index_order[hint->type].clear_all();
          have_empty_use_order= TRUE;
        }
        if (hint->clause & INDEX_HINT_MASK_GROUP)
        {
          index_group[hint->type].clear_all();
          have_empty_use_group= TRUE;
        }
        continue;
      }

      /* 
        Check if an index with the given name exists and get his offset in 
        the keys bitmask for the table 
      */
      if (tbl->s->keynames.type_names == 0 ||
          (pos= find_type(&tbl->s->keynames, hint->key_name.str,
                          hint->key_name.length, 1)) <= 0 ||
          (tbl->s->key_info[pos - 1].is_ignored))
      {
        my_error(ER_KEY_DOES_NOT_EXISTS, MYF(0), hint->key_name.str, alias.str);
        return 1;
      }

      pos--;

      /* add to the appropriate clause mask */
      if (hint->clause & INDEX_HINT_MASK_JOIN)
        index_join[hint->type].set_bit (pos);
      if (hint->clause & INDEX_HINT_MASK_ORDER)
        index_order[hint->type].set_bit (pos);
      if (hint->clause & INDEX_HINT_MASK_GROUP)
        index_group[hint->type].set_bit (pos);
    }

    /* cannot mix USE INDEX and FORCE INDEX */
    if ((!index_join[INDEX_HINT_FORCE].is_clear_all() ||
         !index_order[INDEX_HINT_FORCE].is_clear_all() ||
         !index_group[INDEX_HINT_FORCE].is_clear_all()) &&
        (!index_join[INDEX_HINT_USE].is_clear_all() ||  have_empty_use_join ||
         !index_order[INDEX_HINT_USE].is_clear_all() || have_empty_use_order ||
         !index_group[INDEX_HINT_USE].is_clear_all() || have_empty_use_group))
    {
      my_error(ER_WRONG_USAGE, MYF(0), index_hint_type_name[INDEX_HINT_USE],
               index_hint_type_name[INDEX_HINT_FORCE]);
      return 1;
    }

    /* process FORCE INDEX as USE INDEX with a flag */
    if (!index_order[INDEX_HINT_FORCE].is_clear_all())
    {
      tbl->force_index_order= TRUE;
      index_order[INDEX_HINT_USE].merge(index_order[INDEX_HINT_FORCE]);
    }

    if (!index_group[INDEX_HINT_FORCE].is_clear_all())
    {
      tbl->force_index_group= TRUE;
      index_group[INDEX_HINT_USE].merge(index_group[INDEX_HINT_FORCE]);
    }

    if (!index_join[INDEX_HINT_FORCE].is_clear_all())
    {
      tbl->force_index_join= TRUE;
      index_join[INDEX_HINT_USE].merge(index_join[INDEX_HINT_FORCE]);
    }

    /*
      TODO: get rid of tbl->force_index (on if any FORCE INDEX is specified)
      Use the correct force_index_XX in all places instead of the global one.
    */
    tbl->force_index= (tbl->force_index_order | tbl->force_index_group |
                       tbl->force_index_join);

    /* apply USE INDEX */
    if (!index_join[INDEX_HINT_USE].is_clear_all() || have_empty_use_join)
      tbl->keys_in_use_for_query.intersect(index_join[INDEX_HINT_USE]);
    if (!index_order[INDEX_HINT_USE].is_clear_all() || have_empty_use_order)
      tbl->keys_in_use_for_order_by.intersect (index_order[INDEX_HINT_USE]);
    if (!index_group[INDEX_HINT_USE].is_clear_all() || have_empty_use_group)
      tbl->keys_in_use_for_group_by.intersect (index_group[INDEX_HINT_USE]);

    /* apply IGNORE INDEX */
    tbl->keys_in_use_for_query.subtract (index_join[INDEX_HINT_IGNORE]);
    tbl->keys_in_use_for_order_by.subtract (index_order[INDEX_HINT_IGNORE]);
    tbl->keys_in_use_for_group_by.subtract (index_group[INDEX_HINT_IGNORE]);
  }

  /* make sure covering_keys don't include indexes disabled with a hint */
  tbl->covering_keys.intersect(tbl->keys_in_use_for_query);
  return 0;
}


size_t max_row_length(TABLE *table, MY_BITMAP const *cols, const uchar *data)
{
  TABLE_SHARE *table_s= table->s;
  size_t length= table_s->reclength + 2 * table_s->fields;
  uint *const beg= table_s->blob_field;
  uint *const end= beg + table_s->blob_fields;
  my_ptrdiff_t const rec_offset= (my_ptrdiff_t) (data - table->record[0]);
  DBUG_ENTER("max_row_length");

  for (uint *ptr= beg ; ptr != end ; ++ptr)
  {
    Field * const field= table->field[*ptr];
    if (bitmap_is_set(cols, field->field_index) &&
        !field->is_null(rec_offset))
    {
      Field_blob * const blob= (Field_blob*) field;
      length+= blob->get_length(rec_offset) + 8; /* max blob store length */
    }
  }
  DBUG_PRINT("exit", ("length: %lld", (longlong) length));
  DBUG_RETURN(length);
}


/**
   Helper function which allows to allocate metadata lock request
   objects for all elements of table list.
*/

void init_mdl_requests(TABLE_LIST *table_list)
{
  for ( ; table_list ; table_list= table_list->next_global)
    MDL_REQUEST_INIT(&table_list->mdl_request, MDL_key::TABLE,
                     table_list->db.str, table_list->table_name.str,
                     table_list->lock_type >= TL_FIRST_WRITE
                     ? MDL_SHARED_WRITE : MDL_SHARED_READ, MDL_TRANSACTION);
}


/**
  Update TABLE::const_key_parts for single table UPDATE/DELETE query

  @param conds               WHERE clause expression

  @retval TRUE   error (OOM)
  @retval FALSE  success

  @note
    Set const_key_parts bits if key fields are equal to constants in
    the WHERE expression.
*/

bool TABLE::update_const_key_parts(COND *conds)
{
  bzero((char*) const_key_parts, sizeof(key_part_map) * s->keys);

  if (conds == NULL)
    return FALSE;

  for (uint index= 0; index < s->keys; index++)
  {
    KEY_PART_INFO *keyinfo= key_info[index].key_part;
    KEY_PART_INFO *keyinfo_end= keyinfo + key_info[index].user_defined_key_parts;

    for (key_part_map part_map= (key_part_map)1; 
        keyinfo < keyinfo_end;
        keyinfo++, part_map<<= 1)
    {
      if (const_expression_in_where(conds, NULL, keyinfo->field))
        const_key_parts[index]|= part_map;
    }
  }
  return FALSE;
}

/**
  Test if the order list consists of simple field expressions

  @param order                Linked list of ORDER BY arguments

  @return TRUE if @a order is empty or consist of simple field expressions
*/

bool is_simple_order(ORDER *order)
{
  for (ORDER *ord= order; ord; ord= ord->next)
  {
    if (ord->item[0]->real_item()->type() != Item::FIELD_ITEM)
      return FALSE;
  }
  return TRUE;
}

/*
  to satisfy marked_for_write_or_computed() Field's assert we temporarily
  mark field for write before storing the generated value in it
*/
#ifdef DBUG_ASSERT_EXISTS
#define DBUG_FIX_WRITE_SET(f) bool _write_set_fixed= !bitmap_fast_test_and_set(write_set, (f)->field_index)
#define DBUG_RESTORE_WRITE_SET(f) if (_write_set_fixed) bitmap_clear_bit(write_set, (f)->field_index)
#else
#define DBUG_FIX_WRITE_SET(f)
#define DBUG_RESTORE_WRITE_SET(f)
#endif


/*
  @brief Compute values for virtual columns used in query

  @param  update_mode Specifies what virtual column are computed
  
  @details
    The function computes the values of the virtual columns of the table and
    stores them in the table record buffer.
    This will be done even if is_error() is set either when function was called
    or by calculating the virtual function, as most calls to this
    function doesn't check the result. We also want to ensure that as many
    fields as possible has the right value so that we can optionally
    return the partly-faulty-row from a storage engine with a virtual
    field that gives an error on storage for an existing row.

  @todo
    Ensure that all caller checks the value of this function and
    either properly ignores it (and resets the error) or sends the
    error forward to the caller.

  @retval
    0    Success
  @retval
    >0   Error occurred when storing a virtual field value or potentially
         is_error() was set when function was called.
*/

int TABLE::update_virtual_fields(handler *h, enum_vcol_update_mode update_mode)
{
  DBUG_ENTER("TABLE::update_virtual_fields");
  DBUG_PRINT("enter", ("update_mode: %d  is_error: %d", update_mode,
                       in_use->is_error()));
  Field **vfield_ptr, *vf;
  Query_arena backup_arena;
  Turn_errors_to_warnings_handler Suppress_errors;
  bool handler_pushed= 0, update_all_columns= 1;
  DBUG_ASSERT(vfield);

  if (h->keyread_enabled())
    DBUG_RETURN(0);
  /*
    TODO: this imposes memory leak until table flush when save_in_field()
          does expr_arena allocation. F.ex. case in
          gcol.gcol_supported_sql_funcs_innodb (see CONVERT_TZ):

          create table t1 (
            a datetime, b datetime generated always as
            (convert_tz(a, 'MET', 'UTC')) virtual);
          insert into t1 values ('2008-08-31', default);
  */
  in_use->set_n_backup_active_arena(expr_arena, &backup_arena);

  /* When reading or deleting row, ignore errors from virtual columns */
  if (update_mode == VCOL_UPDATE_FOR_READ ||
      update_mode == VCOL_UPDATE_FOR_DELETE ||
      update_mode == VCOL_UPDATE_INDEXED)
  {
    in_use->push_internal_handler(&Suppress_errors);
    handler_pushed= 1;
  }
  else if (update_mode == VCOL_UPDATE_FOR_REPLACE &&
           in_use->is_current_stmt_binlog_format_row() &&
           in_use->variables.binlog_row_image != BINLOG_ROW_IMAGE_MINIMAL)
  {
    /*
      If we are doing a replace with not minimal binary logging, we have to
      calculate all virtual columns.
    */
    update_all_columns= 1;
  }

  /* Iterate over virtual fields in the table */
  for (vfield_ptr= vfield; *vfield_ptr ; vfield_ptr++)
  {
    vf= (*vfield_ptr);
    Virtual_column_info *vcol_info= vf->vcol_info;
    DBUG_ASSERT(vcol_info);
    DBUG_ASSERT(vcol_info->expr);

    bool update= 0, swap_values= 0;
    switch (update_mode) {
    case VCOL_UPDATE_FOR_READ:
      update= (!vcol_info->is_stored() &&
               bitmap_is_set(read_set, vf->field_index));
      swap_values= 1;
      break;
    case VCOL_UPDATE_FOR_DELETE:
    case VCOL_UPDATE_FOR_WRITE:
      update= bitmap_is_set(read_set, vf->field_index);
      break;
    case VCOL_UPDATE_FOR_REPLACE:
      update= ((!vcol_info->is_stored() &&
                (vf->flags & (PART_KEY_FLAG | PART_INDIRECT_KEY_FLAG)) &&
                bitmap_is_set(read_set, vf->field_index)) ||
               update_all_columns);
      if (update && (vf->flags & BLOB_FLAG))
      {
        /*
          The row has been read into record[1] and Field_blob::value
          contains the value for record[0].  Swap value and read_value
          to ensure that the virtual column data for the read row will
          be in read_value at the end of this function
        */
        ((Field_blob*) vf)->swap_value_and_read_value();
        /* Ensure we call swap_value_and_read_value() after update */
        swap_values= 1;
      }
      break;
    case VCOL_UPDATE_INDEXED:
    case VCOL_UPDATE_INDEXED_FOR_UPDATE:
      /* Read indexed fields that was not updated in VCOL_UPDATE_FOR_READ */
      update= (!vcol_info->is_stored() &&
               (vf->flags & (PART_KEY_FLAG | PART_INDIRECT_KEY_FLAG)) &&
               !bitmap_is_set(read_set, vf->field_index));
      swap_values= 1;
      break;
    }

    if (update)
    {
      /* Compute the actual value of the virtual fields */
      DBUG_FIX_WRITE_SET(vf);
# ifdef DBUG_TRACE
      int field_error=
# endif
      vcol_info->expr->save_in_field(vf, 0);
      DBUG_RESTORE_WRITE_SET(vf);
      DBUG_PRINT("info", ("field '%s' - updated  error: %d",
                          vf->field_name.str, field_error));
      if (swap_values && (vf->flags & BLOB_FLAG))
      {
        /*
          Remember the read value to allow other update_virtual_field() calls
          for the same blob field for the row to be updated.
          Field_blob->read_value always contains the virtual column data for
          any read row.
        */
        ((Field_blob*) vf)->swap_value_and_read_value();
      }
    }
    else
    {
      DBUG_PRINT("info", ("field '%s' - skipped", vf->field_name.str));
    }
  }
  if (handler_pushed)
    in_use->pop_internal_handler();
  in_use->restore_active_arena(expr_arena, &backup_arena);
  
  /* Return 1 only of we got a fatal error, not a warning */
  DBUG_RETURN(in_use->is_error());
}

/*
  Calculate the virtual field value for a specified field.
  @param vf                     A field to calculate
  @param ignore_warnings        Ignore the warnings and also make the
                                calculations permissive. This usually means
                                that a calculation is internal and is not
                                expected to fail.
*/
int TABLE::update_virtual_field(Field *vf, bool ignore_warnings)
{
  DBUG_ENTER("TABLE::update_virtual_field");
  Query_arena backup_arena;
  Counting_error_handler count_errors;
  Suppress_warnings_error_handler warning_handler;
  in_use->push_internal_handler(&count_errors);
  bool abort_on_warning= ignore_warnings;
  if (ignore_warnings)
  {
    abort_on_warning= in_use->abort_on_warning;
    in_use->abort_on_warning= false;
    in_use->push_internal_handler(&warning_handler);
  }
  /*
    TODO: this may impose memory leak until table flush.
          See comment in
          TABLE::update_virtual_fields(handler *, enum_vcol_update_mode).
  */
  in_use->set_n_backup_active_arena(expr_arena, &backup_arena);
  bitmap_clear_all(&tmp_set);
  vf->vcol_info->expr->walk(&Item::update_vcol_processor, 0, &tmp_set);
  DBUG_FIX_WRITE_SET(vf);
  vf->vcol_info->expr->save_in_field(vf, 0);
  DBUG_RESTORE_WRITE_SET(vf);
  in_use->restore_active_arena(expr_arena, &backup_arena);
  in_use->pop_internal_handler();
  if (ignore_warnings)
  {
    in_use->abort_on_warning= abort_on_warning;
    in_use->pop_internal_handler();
    // This is an internal calculation, we expect it to always succeed
    DBUG_ASSERT(count_errors.errors == 0);
  }
  DBUG_RETURN(count_errors.errors);
}


/**
  Update all DEFAULT and/or ON INSERT fields.

  @details
    Compute and set the default value of all fields with a default function.
    There are two kinds of default functions - one is used for INSERT-like
    operations, the other for UPDATE-like operations. Depending on the field
    definition and the current operation one or the other kind of update
    function is evaluated.

  @param update_command   True if command was an update else insert
  @param ignore_errors    True if we should ignore errors

  @retval
    0    Success
  @retval
    >0   Error occurred when storing a virtual field value and
         ignore_errors == 0. If set then an error was generated.
*/

int TABLE::update_default_fields(bool ignore_errors)
{
  Query_arena backup_arena;
  Field **field_ptr;
  int res= 0;
  DBUG_ENTER("TABLE::update_default_fields");
  DBUG_ASSERT(default_field);

  /*
    TODO: this may impose memory leak until table flush.
          See comment in
          TABLE::update_virtual_fields(handler *, enum_vcol_update_mode).
  */
  in_use->set_n_backup_active_arena(expr_arena, &backup_arena);

  /* Iterate over fields with default functions in the table */
  for (field_ptr= default_field; *field_ptr ; field_ptr++)
  {
    Field *field= (*field_ptr);
    /*
      If an explicit default value for a field overrides the default,
      do not update the field with its automatic default value.
    */
    if (!field->has_explicit_value())
    {
      if (field->default_value &&
          (field->default_value->flags || field->flags & BLOB_FLAG))
        res|= (field->default_value->expr->save_in_field(field, 0) < 0);
      if (!ignore_errors && res)
      {
        my_error(ER_CALCULATING_DEFAULT_VALUE, MYF(0), field->field_name.str);
        break;
      }
      res= 0;
    }
  }
  in_use->restore_active_arena(expr_arena, &backup_arena);
  DBUG_RETURN(res);
}

int TABLE::update_generated_fields()
{
  int res= 0;
  if (next_number_field)
  {
    res= next_number_field->set_default();
    if (likely(!res))
      res= file->update_auto_increment();
    next_number_field= NULL;
  }

  if (likely(!res) && vfield)
    res= update_virtual_fields(file, VCOL_UPDATE_FOR_WRITE);
  if (likely(!res) && versioned())
    vers_update_fields();
  if (likely(!res))
    res= verify_constraints(false) == VIEW_CHECK_ERROR;
  return res;
}

void TABLE::period_prepare_autoinc()
{
  if (!found_next_number_field)
    return;
  /* Don't generate a new value if the autoinc index is WITHOUT OVERLAPS */
  DBUG_ASSERT(s->next_number_index < MAX_KEY);
  if (key_info[s->next_number_index].without_overlaps)
    return;

  next_number_field= found_next_number_field;
}

int TABLE::period_make_insert(Item *src, Field *dst)
{
  THD *thd= in_use;

  ulonglong prev_insert_id= file->next_insert_id;
  store_record(this, record[1]);
  int res= src->save_in_field(dst, true);

  if (likely(!res))
  {
    period_prepare_autoinc();
    res= update_generated_fields();
  }

  if (likely(!res) && triggers)
    res= triggers->process_triggers(thd, TRG_EVENT_INSERT,
                                    TRG_ACTION_BEFORE, true);

  if (likely(!res))
    res = file->ha_write_row(record[0]);

  if (likely(!res) && triggers)
    res= triggers->process_triggers(thd, TRG_EVENT_INSERT,
                                    TRG_ACTION_AFTER, true);

  restore_record(this, record[1]);
  if (res)
    file->restore_auto_increment(prev_insert_id);
  return res;
}

int TABLE::insert_portion_of_time(THD *thd,
                                  const vers_select_conds_t &period_conds,
                                  ha_rows *rows_inserted)
{
  bool lcond= period_conds.field_start->val_datetime_packed(thd)
              < period_conds.start.item->val_datetime_packed(thd);
  bool rcond= period_conds.field_end->val_datetime_packed(thd)
              > period_conds.end.item->val_datetime_packed(thd);

  int res= 0;
  if (lcond)
  {
    res= period_make_insert(period_conds.start.item,
                            field[s->period.end_fieldno]);
    if (likely(!res))
      ++*rows_inserted;
  }
  if (likely(!res) && rcond)
  {
    res= period_make_insert(period_conds.end.item,
                            field[s->period.start_fieldno]);
    if (likely(!res))
      ++*rows_inserted;
  }

  return res;
}

void TABLE::evaluate_update_default_function()
{
  DBUG_ENTER("TABLE::evaluate_update_default_function");

  if (s->has_update_default_function)
    for (Field **field_ptr= default_field; *field_ptr ; field_ptr++)
    {
      Field *field= (*field_ptr);
      if (!field->has_explicit_value() && field->has_update_default_function())
        field->set_time();
    }
  DBUG_VOID_RETURN;
}

/**
  Compare two records by a specific key (that has WITHOUT OVERLAPS clause)

  @return  true,     key values are equal and periods overlap
           false,    either key values differ or periods don't overlap
 */
bool TABLE::check_period_overlaps(const KEY &key,
                                 const uchar *lhs, const uchar *rhs)
{
  DBUG_ASSERT(key.without_overlaps);
  uint base_part_nr= key.user_defined_key_parts - 2;
  for (uint part_nr= 0; part_nr < base_part_nr; part_nr++)
  {
    Field *f= key.key_part[part_nr].field;
    if (key.key_part[part_nr].null_bit)
      if (f->is_null_in_record(lhs) || f->is_null_in_record(rhs))
        return false;
    uint kp_len= key.key_part[part_nr].length;
    if (f->cmp_prefix(f->ptr_in_record(lhs), f->ptr_in_record(rhs),
                      kp_len / f->charset()->mbmaxlen) != 0)
      return false;
  }

  uint period_start= key.user_defined_key_parts - 1;
  uint period_end= key.user_defined_key_parts - 2;
  const Field *fs= key.key_part[period_start].field;
  const Field *fe= key.key_part[period_end].field;

  if (fs->cmp(fe->ptr_in_record(lhs), fs->ptr_in_record(rhs)) <= 0)
    return false;
  if (fs->cmp(fs->ptr_in_record(lhs), fe->ptr_in_record(rhs)) >= 0)
    return false;
  return true;
}

/* returns true if vers_end_field was updated */
bool TABLE::vers_update_fields()
{
  bool res= false;
  if (versioned(VERS_TIMESTAMP) && !vers_start_field()->has_explicit_value())
  {
    if (vers_start_field()->set_time())
      DBUG_ASSERT(0);
  }

  if (!versioned(VERS_TIMESTAMP) || !vers_end_field()->has_explicit_value())
  {
    vers_end_field()->set_max();
    res= true;
  }

  if (vfield)
    update_virtual_fields(file, VCOL_UPDATE_FOR_READ);
  return res;
}


void TABLE::vers_update_end()
{
  if (vers_end_field()->store_timestamp(in_use->query_start(),
                                        in_use->query_start_sec_part()))
    DBUG_ASSERT(0);
  if (vfield)
    update_virtual_fields(file, VCOL_UPDATE_FOR_WRITE);
}

/**
   Reset markers that fields are being updated
*/

void TABLE::reset_default_fields()
{
  DBUG_ENTER("reset_default_fields");
  bitmap_clear_all(&has_value_set);
  DBUG_VOID_RETURN;
}

/*
  Prepare triggers  for INSERT-like statement.

  SYNOPSIS
    prepare_triggers_for_insert_stmt_or_event()

  NOTE
    Prepare triggers for INSERT-like statement by marking fields
    used by triggers and inform handlers that batching of UPDATE/DELETE 
    cannot be done if there are BEFORE UPDATE/DELETE triggers.
*/

void TABLE::prepare_triggers_for_insert_stmt_or_event()
{
  if (triggers)
  {
    triggers->clear_extra_null_bitmap();
    if (triggers->has_triggers(TRG_EVENT_DELETE, TRG_ACTION_AFTER))
    {
      /*
        The table has AFTER DELETE triggers that might access to
        subject table and therefore might need delete to be done
        immediately. So we turn-off the batching.
      */
      (void) file->extra(HA_EXTRA_DELETE_CANNOT_BATCH);
    }
    if (triggers->has_triggers(TRG_EVENT_UPDATE, TRG_ACTION_AFTER))
    {
      /*
        The table has AFTER UPDATE triggers that might access to subject
        table and therefore might need update to be done immediately.
        So we turn-off the batching.
      */
      (void) file->extra(HA_EXTRA_UPDATE_CANNOT_BATCH);
    }
  }
}


bool TABLE::prepare_triggers_for_delete_stmt_or_event()
{
  if (triggers)
  {
    triggers->clear_extra_null_bitmap();
    if (triggers->has_triggers(TRG_EVENT_DELETE, TRG_ACTION_AFTER))
    {
      /*
        The table has AFTER DELETE triggers that might access to subject table
        and therefore might need delete to be done immediately. So we turn-off
        the batching.
      */
      (void) file->extra(HA_EXTRA_DELETE_CANNOT_BATCH);
      return TRUE;
    }
  }
  return FALSE;
}


bool TABLE::prepare_triggers_for_update_stmt_or_event()
{
  if (triggers)
  {
    triggers->clear_extra_null_bitmap();
    if (triggers->has_triggers(TRG_EVENT_UPDATE, TRG_ACTION_AFTER))
    {
      /*
        The table has AFTER UPDATE triggers that might access to subject
        table and therefore might need update to be done immediately.
        So we turn-off the batching.
      */
      (void) file->extra(HA_EXTRA_UPDATE_CANNOT_BATCH);
      return TRUE;
    }
  }
  return FALSE;
}


/**
  Validates default value of fields which are not specified in
  the column list of INSERT/LOAD statement.

  @Note s->default_values should be properly populated
        before calling this function.

  @param thd              thread context
  @param record           the record to check values in

  @return
    @retval false Success.
    @retval true  Failure.
*/

bool TABLE::validate_default_values_of_unset_fields(THD *thd) const
{
  DBUG_ENTER("TABLE::validate_default_values_of_unset_fields");
  for (Field **fld= field; *fld; fld++)
  {
    if (!bitmap_is_set(write_set, (*fld)->field_index) &&
        !((*fld)->flags & (NO_DEFAULT_VALUE_FLAG | VERS_SYSTEM_FIELD)))
    {
      if (!(*fld)->is_null_in_record(s->default_values) &&
          (*fld)->validate_value_in_record_with_warn(thd, s->default_values) &&
          thd->is_error())
      {
        /*
          We're here if:
          - validate_value_in_record_with_warn() failed and
            strict mo validate_default_values_of_unset_fieldsde converted WARN to ERROR
          - or the connection was killed, or closed unexpectedly
        */
        DBUG_RETURN(true);
      }
    }
  }
  DBUG_RETURN(false);
}


/*
  Check assignment compatibility of a value list against an explicitly
  specified field list, e.g.
    INSERT INTO t1 (a,b) VALUES (1,2);
*/
bool TABLE::check_assignability_explicit_fields(List<Item> fields,
                                                List<Item> values,
                                                bool ignore)
{
  DBUG_ENTER("TABLE::check_assignability_explicit_fields");
  DBUG_ASSERT(fields.elements == values.elements);

  List_iterator<Item> fi(fields);
  List_iterator<Item> vi(values);
  Item *f, *value;
  while ((f= fi++) && (value= vi++))
  {
    Item_field *item_field= f->field_for_view_update();
    if (!item_field)
    {
      /*
        A non-updatable field of a view found.
        This scenario is caught later and an error is raised.
        We could eventually move error reporting here. For now just continue.
      */
      continue;
    }
    if (value->check_assignability_to(item_field->field, ignore))
      DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}


/*
  Check assignment compatibility for a value list against
  all visible fields of the table, e.g.
    INSERT INTO t1 VALUES (1,2);
*/
bool TABLE::check_assignability_all_visible_fields(List<Item> &values,
                                                   bool ignore) const
{
  DBUG_ENTER("TABLE::check_assignability_all_visible_fields");
  DBUG_ASSERT(s->visible_fields == values.elements);

  List_iterator<Item> vi(values);
  for (uint i= 0; i < s->fields; i++)
  {
    if (!field[i]->invisible &&
        (vi++)->check_assignability_to(field[i], ignore))
      DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}


bool TABLE::insert_all_rows_into_tmp_table(THD *thd,
                                           TABLE *tmp_table,
                                           TMP_TABLE_PARAM *tmp_table_param,
                                           bool with_cleanup)
{
  int write_err= 0;

  DBUG_ENTER("TABLE::insert_all_rows_into_tmp_table");

  if (with_cleanup)
  {
   if ((write_err= tmp_table->file->ha_delete_all_rows()))
      goto err;
  }
   
  if (file->indexes_are_disabled())
    tmp_table->file->ha_disable_indexes(key_map(0), false);

  file->ha_index_or_rnd_end();

  if (unlikely(file->ha_rnd_init_with_error(1)))
    DBUG_RETURN(1);

  if (tmp_table->no_rows)
    tmp_table->file->extra(HA_EXTRA_NO_ROWS);
  else
  {
    /* update table->file->stats.records */
    file->info(HA_STATUS_VARIABLE);
    tmp_table->file->ha_start_bulk_insert(file->stats.records);
  }

  while (likely(!file->ha_rnd_next(tmp_table->record[0])))
  {
    write_err= tmp_table->file->ha_write_tmp_row(tmp_table->record[0]);
    if (unlikely(write_err))
    {
      bool is_duplicate;
      if (tmp_table->file->is_fatal_error(write_err, HA_CHECK_DUP) &&
          create_internal_tmp_table_from_heap(thd, tmp_table,
                                              tmp_table_param->start_recinfo, 
                                              &tmp_table_param->recinfo,
                                              write_err, 1, &is_duplicate))
	DBUG_RETURN(1);
       
    }  
    if (unlikely(thd->check_killed()))
      goto err_killed;
  }
  if (!tmp_table->no_rows && tmp_table->file->ha_end_bulk_insert())
    goto err;
  DBUG_RETURN(0);

err:
  DBUG_PRINT("error",("Got error: %d",write_err));
  file->print_error(write_err, MYF(0));
err_killed:
  (void) file->ha_rnd_end();
  DBUG_RETURN(1);
}



/*
  @brief Reset const_table flag

  @detail
  Reset const_table flag for this table. If this table is a merged derived
  table/view the flag is recursively reseted for all tables of the underlying
  select.
*/

void TABLE_LIST::reset_const_table()
{
  table->const_table= 0;
  if (is_merged_derived())
  {
    SELECT_LEX *select_lex= get_unit()->first_select();
    TABLE_LIST *tl;
    List_iterator<TABLE_LIST> ti(select_lex->leaf_tables);
    while ((tl= ti++))
      tl->reset_const_table();
  }
}


/*
  @brief Run derived tables/view handling phases on underlying select_lex.

  @param lex    LEX for this thread
  @param phases derived tables/views handling phases to run
                (set of DT_XXX constants)
  @details
  This function runs this derived table through specified 'phases'.
  Underlying tables of this select are handled prior to this derived.
  'lex' is passed as an argument to called functions.

  @return TRUE on error
  @return FALSE ok
*/

bool TABLE_LIST::handle_derived(LEX *lex, uint phases)
{
  SELECT_LEX_UNIT *unit= get_unit();
  DBUG_ENTER("handle_derived");
  DBUG_PRINT("enter", ("phases: 0x%x", phases));

  if (unit)
  {
    if (!is_with_table_recursive_reference())
    {
      for (SELECT_LEX *sl= unit->first_select(); sl; sl= sl->next_select())
        if (sl->handle_derived(lex, phases))
          DBUG_RETURN(TRUE);
    }
    if (mysql_handle_single_derived(lex, this, phases))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

/**
  @brief
  Return unit of this derived table/view

  @return reference to a unit  if it's a derived table/view.
  @return 0                    when it's not a derived table/view.
*/

st_select_lex_unit *TABLE_LIST::get_unit()
{
  return (view ? &view->unit : derived);
}


/**
  @brief
  Return select_lex of this derived table/view

  @return select_lex of this derived table/view.
  @return 0          when it's not a derived table.
*/

st_select_lex *TABLE_LIST::get_single_select()
{
  SELECT_LEX_UNIT *unit= get_unit();
  return (unit ? unit->first_select() : 0);
}


/**
  @brief
  Attach a join table list as a nested join to this TABLE_LIST.

  @param join_list join table list to attach

  @details
  This function wraps 'join_list' into a nested_join of this table, thus
  turning it to a nested join leaf.
*/

void TABLE_LIST::wrap_into_nested_join(List<TABLE_LIST> &join_list)
{
  TABLE_LIST *tl;
  /*
    Walk through derived table top list and set 'embedding' to point to
    the nesting table.
  */
  nested_join->join_list.empty();
  List_iterator_fast<TABLE_LIST> li(join_list);
  nested_join->join_list= join_list;
  while ((tl= li++))
  {
    tl->embedding= this;
    tl->join_list= &nested_join->join_list;
  }
}


/**
  Check whether optimization has been performed and a derived table either
  been merged to upper select level or materialized.

  @param table  a TABLE_LIST object containing a derived table

  @return true in case the derived table has been merged to surrounding select,
          false otherwise
*/

static inline bool derived_table_optimization_done(TABLE_LIST *table)
{
  SELECT_LEX_UNIT *derived= (table->derived ?
                             table->derived :
                             (table->view ?
                              &table->view->unit:
                              NULL));
  return derived &&
      (derived->is_excluded() ||
       table->is_materialized_derived());
}


/**
  @brief
  Initialize this derived table/view

  @param thd  Thread handle

  @details
  This function makes initial preparations of this derived table/view for
  further processing:
    if it's a derived table this function marks it either as mergeable or
      materializable
    creates temporary table for name resolution purposes
    creates field translation for mergeable derived table/view

  @return TRUE  an error occur
  @return FALSE ok
*/

bool TABLE_LIST::init_derived(THD *thd, bool init_view)
{
  SELECT_LEX *first_select= get_single_select();
  SELECT_LEX_UNIT *unit= get_unit();

  if (!unit)
    return FALSE;
  /*
    Check whether we can merge this derived table into main select.
    Depending on the result field translation will or will not
    be created.
  */
  TABLE_LIST *first_table= (TABLE_LIST *) first_select->table_list.first;
  if (first_select->table_list.elements > 1 ||
      (first_table && first_table->is_multitable()))
    set_multitable();

  if (!unit->derived)
    unit->derived= this;
  else if (!is_with_table_recursive_reference() && unit->derived != this)
  {
    if (unit->derived->is_with_table_recursive_reference())
      unit->derived= this;
    else if (vers_conditions.eq(unit->derived->vers_conditions))
      vers_conditions.empty();
    else
    {
      my_error(ER_CONFLICTING_FOR_SYSTEM_TIME, MYF(0));
      return TRUE;
    }
  }

  if (init_view && !view &&
      !derived_table_optimization_done(this))
  {
    /* This is all what we can do for a derived table for now. */
    set_derived();
  }

  if (!derived_table_optimization_done(this))
  {
    /* A subquery might be forced to be materialized due to a side-effect. */
    bool forced_no_merge_for_update_delete=
           belong_to_view ? belong_to_view->updating :
                           !unit->outer_select()->outer_select();
    if (!is_materialized_derived() && unit->can_be_merged() &&
        /*
          Following is special case of
          SELECT * FROM (<limited-select>) WHERE ROWNUM() <= nnn
        */
        (unit->outer_select() &&
         !(unit->outer_select()->with_rownum &&
           unit->outer_select()->table_list.elements == 1 &&
           (thd->lex->sql_command == SQLCOM_SELECT ||
            !unit->outer_select()->is_query_topmost(thd)) &&
           !is_view())) &&

        (!thd->lex->with_rownum ||
         (!first_select->group_list.elements &&
          !first_select->order_list.elements)) &&
        (is_view() ||
         optimizer_flag(thd, OPTIMIZER_SWITCH_DERIVED_MERGE)) &&
          !thd->lex->can_not_use_merged() &&
        !(!is_view() && forced_no_merge_for_update_delete &&
          (thd->lex->sql_command == SQLCOM_UPDATE_MULTI ||
           thd->lex->sql_command == SQLCOM_DELETE_MULTI ||
           (thd->lex->sql_command == SQLCOM_UPDATE &&
            (((Sql_cmd_update *) thd->lex->m_sql_cmd)->is_multitable() ||
             thd->lex->query_tables->is_multitable())) ||
           (thd->lex->sql_command == SQLCOM_DELETE &&
            (((Sql_cmd_delete *) thd->lex->m_sql_cmd)->is_multitable() ||
             thd->lex->query_tables->is_multitable())))) &&
        !is_recursive_with_table())
      set_merged_derived();
    else
      set_materialized_derived();
  }
  /*
    Derived tables/view are materialized prior to UPDATE, thus we can skip
    them from table uniqueness check
  */
  if (is_materialized_derived())
  {
    set_check_materialized();
  }

  /*
    Create field translation for mergeable derived tables/views.
    For derived tables field translation can be created only after
    unit is prepared so all '*' are get unrolled.
  */
  if (is_merged_derived())
  {
    if (is_view() ||
        (unit->prepared &&
	!(thd->lex->context_analysis_only & CONTEXT_ANALYSIS_ONLY_VIEW)))
      create_field_translation(thd);
  }

  return FALSE;
}


/**
  @brief
  Retrieve number of rows in the table

  @details
  Retrieve number of rows in the table referred by this TABLE_LIST and
  store it in the table's stats.records variable. If this TABLE_LIST refers
  to a materialized derived table/view then the estimated number of rows of
  the derived table/view is used instead.

  @return 0          ok
  @return non zero   error
*/

int TABLE_LIST::fetch_number_of_rows()
{
  int error= 0;
  if (jtbm_subselect)
  {
    if (jtbm_subselect->is_jtbm_merged)
    {
      table->file->stats.records= (ha_rows)jtbm_subselect->jtbm_record_count;
      set_if_bigger(table->file->stats.records, 2);
      table->used_stat_records= table->file->stats.records;
    }
    return 0;
  }
  if (is_materialized_derived() && !fill_me)
  {
    table->file->stats.records= get_unit()->result->est_records;
    set_if_bigger(table->file->stats.records, 2);
    table->used_stat_records= table->file->stats.records;
  }
  else
    error= table->file->info(HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK);
  return error;
}


/*
  @brief
  Change references to underlying items of a merged derived table/view
  for fields in derived table's result table.

  @return FALSE ok
  @return TRUE  Out of memory
*/
bool TABLE_LIST::change_refs_to_fields()
{
  List_iterator<Item> li(used_items);
  Item_direct_ref *ref;
  Field_iterator_view field_it;
  Name_resolution_context *ctx;
  THD *thd= table->in_use;
  Item **materialized_items;
  DBUG_ASSERT(is_merged_derived());

  if (!used_items.elements)
    return FALSE;

  materialized_items= (Item **)thd->calloc(sizeof(void *) * table->s->fields);
  ctx= new (thd->mem_root) Name_resolution_context(this);
  if (!materialized_items || !ctx)
    return TRUE;

  while ((ref= (Item_direct_ref*)li++))
  {
    uint idx;
    Item *orig_item= *ref->ref;
    field_it.set(this);
    for (idx= 0; !field_it.end_of_fields(); field_it.next(), idx++)
    {
      if (field_it.item() == orig_item)
        break;
    }
    DBUG_ASSERT(!field_it.end_of_fields());
    if (!materialized_items[idx])
    {
      materialized_items[idx]=
        new (thd->mem_root) Item_field(thd, ctx, table->field[idx]);
      if (!materialized_items[idx])
        return TRUE;
    }
    /*
      We need to restore the pointers after the execution of the
      prepared statement.
    */
    thd->change_item_tree((Item **)&ref->ref,
                          (Item*)(materialized_items + idx));
    /* Inform Item_direct_ref that what it points to has changed */
    ref->ref_changed();
  }

  return FALSE;
}


void TABLE_LIST::set_lock_type(THD *thd, enum thr_lock_type lock)
{
  if (check_stack_overrun(thd, STACK_MIN_SIZE, (uchar *)&lock))
    return;
  /* we call it only when table is opened and it is "leaf" table*/
  DBUG_ASSERT(table);
  lock_type= lock;
  /* If not derived tables */
  if (table->file && table->file->is_open())
    table->file->set_lock_type(lock);
  if (is_merged_derived())
  {
    for (TABLE_LIST *table= get_single_select()->get_table_list();
         table;
         table= table->next_local)
    {
      table->set_lock_type(thd, lock);
    }
  }
}

bool TABLE_LIST::is_with_table()
{
  return derived && derived->with_element;
}


/**
  Check if the definition are the same.

  If versions do not match it check definitions (with checking and setting
  trigger definition versions (times)

  @param[in]  view                TABLE_LIST of the view
  @param[in]  share               Share object of view

  @return false on error or different definitions.

  @sa check_and_update_table_version()
*/

bool TABLE_LIST::is_the_same_definition(THD* thd, TABLE_SHARE *s)
{
  enum enum_table_ref_type tp= s->get_table_ref_type();
  if (m_table_ref_type == tp)
  {
    /*
      Cache have not changed which means that definition was not changed
      including triggers
    */
    if (m_table_ref_version == s->get_table_ref_version())
      return TRUE;

    /*
      If cache changed then check content version
    */
    if ((tabledef_version.length &&
         tabledef_version.length == s->tabledef_version.length &&
         memcmp(tabledef_version.str, s->tabledef_version.str,
                tabledef_version.length) == 0))
    {
      // Definition have not changed, let's check if triggers changed.
      if (table && table->triggers)
      {

        my_hrtime_t hr_stmt_prepare= thd->hr_prepare_time;
        if (hr_stmt_prepare.val)
          for(uint i= 0; i < TRG_EVENT_MAX; i++)
            for (uint j= 0; j < TRG_ACTION_MAX; j++)
            {
              Trigger *tr=
                table->triggers->get_trigger((trg_event_type)i,
                                             (trg_action_time_type)j);
              if (tr)
                if (hr_stmt_prepare.val <= tr->hr_create_time.val)
                {
                  set_tabledef_version(s);
                  return FALSE;
                }
            }
      }
      set_table_id(s);
      return TRUE;
    }
    else
      tabledef_version.length= 0;
  }
  else
  {
    set_tabledef_version(s);
    if (m_table_ref_type == TABLE_REF_NULL)
    {
      set_table_ref_id(s);
      return TRUE;
    }
  }
  return FALSE;
}


uint TABLE_SHARE::actual_n_key_parts(THD *thd)
{
  return use_ext_keys &&
         optimizer_flag(thd, OPTIMIZER_SWITCH_EXTENDED_KEYS) ?
           ext_key_parts : key_parts;
}  


/**
  Get records-per-key estimate for an index prefix.

  Returns average number of records per key value for the given index prefix.
  Prefers engine-independent statistics (EITS) if available and falls back
  to engine-dependent statistics otherwise.

  @param max_key_part  Index of the last key part in the prefix (0-based)

  @return  Estimated records per key value:
           - 0.0 if no statistics available
           - avg_frequency from EITS if available
           - rec_per_key from engine statistics if EITS is not available
*/
double KEY::actual_rec_per_key(uint last_key_part_in_prefix) const
{ 
  if (is_statistics_from_stat_tables)
  {
    // Use engine-independent statistics (EITS)
    return read_stats->get_avg_frequency(last_key_part_in_prefix);
  }
  // Fall back to engine-dependent statistics if EITS is not available
  return rec_per_key ? (double) rec_per_key[last_key_part_in_prefix] : 0.0;
}


/**
  Get records-per-key estimate for an index prefix with NULL-aware optimization.

  Returns average number of records per key value for the given index prefix.
  When EITS statistics show avg_frequency == 0 (typically all NULL values) and
  the query uses NULL-rejecting conditions (e.g., =), returns 1.0 to indicate
  high selectivity since NULL = NULL never matches.

  @param max_key_part  Index of the last key part in the prefix (0-based)
  @param notnull_part  Bitmap indicating which key parts have NULL-rejecting
                       conditions (bit N set means key part N uses =, not <=>)

  @return  Estimated records per key value:
           - 0.0 if no statistics available
           - avg_frequency from EITS if available
           - 1.0 if all values are NULL with NULL-rejecting condition
           - rec_per_key from engine statistics if EITS is not available
*/
double KEY::rec_per_key_null_aware(uint last_key_part_in_prefix,
                                   key_part_map notnull_part) const
{
  if (!is_statistics_from_stat_tables)
  {
    // Fall back to engine-dependent statistics if EITS is not available
    return rec_per_key ? (double) rec_per_key[last_key_part_in_prefix] : 0.0;
  }

  // Use engine-independent statistics (EITS)
  double records= read_stats->get_avg_frequency(last_key_part_in_prefix);
  if (records != 0.0)
    return records;

  /*
    The index statistics show avg_frequency == 0 for this index prefix.
    This typically means all values in the indexed columns are NULL.

    For NULL-rejecting conditions like `t1.key_col = t2.col`, we know
    there will be no matches (since NULL = NULL is never true).
    However, for non-NULL-rejecting conditions like `t1.key_col <=> t2.col`,
    matches are possible.

    Check each key part in the prefix: if any key part has a NULL-rejecting
    condition (indicated by bit set in `notnull_part`) and the statistics
    confirm all values are NULL (nulls_ratio == 1.0), we can return 1.0
    (highly selective estimate) instead of 0.0 (unknown), indicating
    no expected matches.
  */
  for (int bit= last_key_part_in_prefix; bit >= 0; bit--)
  {
    key_part_map mask = (key_part_map)1 << bit;
    if ((notnull_part & mask) == 0)
    {
      // Non-NULL-rejecting condition for the key part
      continue;
    }

    Field *field= table->field[key_part[bit].field->field_index];
    if (!field->read_stats)
    {
      // No column statistics available
      continue;
    }

    // Check if all values in this column are NULL according to statistics
    double nulls_ratio= field->read_stats->get_nulls_ratio();
    if (nulls_ratio == 1.0)
    {
      /*
        All values are NULL and the condition is NULL-rejecting.
        Return 1.0 (highly selective), indicating no expected matches.
      */
      return 1.0;
    }
  }
  return records;
}

/*
   find total number of field in hash expr
*/
int fields_in_hash_keyinfo(KEY *keyinfo)
{
  Item_func_hash * temp= (Item_func_hash *)
                     keyinfo->key_part->field->vcol_info->expr;
  return temp->argument_count();
}
/*
  setup_keyinfo_hash changes the key_info->key_part
  to be same as defined by user
 */
void setup_keyinfo_hash(KEY *key_info)
{
  DBUG_ASSERT(key_info->algorithm == HA_KEY_ALG_LONG_HASH);
  DBUG_ASSERT(key_info->key_part->field->flags & LONG_UNIQUE_HASH_FIELD);
  uint no_of_keyparts= fields_in_hash_keyinfo(key_info);
  key_info->key_part-= no_of_keyparts;
  key_info->user_defined_key_parts= key_info->usable_key_parts=
               key_info->ext_key_parts= no_of_keyparts;
  key_info->flags|= HA_NOSAME;
}
/*
  re_setup_keyinfo_hash reverts th setup_keyinfo_hash and this type of
  arrangement is expected by storage engine
 */

void re_setup_keyinfo_hash(KEY *key_info)
{
  DBUG_ASSERT(key_info->algorithm == HA_KEY_ALG_LONG_HASH);
  DBUG_ASSERT(!(key_info->key_part->field->flags & LONG_UNIQUE_HASH_FIELD));
  while(!(key_info->key_part->field->flags & LONG_UNIQUE_HASH_FIELD))
    key_info->key_part++;
  key_info->user_defined_key_parts= key_info->usable_key_parts=
               key_info->ext_key_parts= 1;
  key_info->flags&= ~HA_NOSAME;
}

LEX_CSTRING *fk_option_name(enum_fk_option opt)
{
  static LEX_CSTRING names[]=
  {
    { STRING_WITH_LEN("???") },
    { STRING_WITH_LEN("RESTRICT") },
    { STRING_WITH_LEN("NO ACTION") },
    { STRING_WITH_LEN("CASCADE") },
    { STRING_WITH_LEN("SET NULL") },
    { STRING_WITH_LEN("SET DEFAULT") }
  };
  return names + opt;
}

enum TR_table::enabled TR_table::use_transaction_registry= TR_table::MAYBE;

TR_table::TR_table(THD* _thd, bool rw) :
  thd(_thd), open_tables_backup(NULL)
{
  init_one_table(&MYSQL_SCHEMA_NAME, &TRANSACTION_REG_NAME,
                 NULL, rw ? TL_WRITE : TL_READ);
}

bool TR_table::open()
{
  DBUG_ASSERT(!table);
  open_tables_backup= new Open_tables_backup;
  if (!open_tables_backup)
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return true;
  }

  All_tmp_tables_list *temporary_tables= thd->temporary_tables;
  bool error= !open_log_table(thd, this, open_tables_backup);
  thd->temporary_tables= temporary_tables;

  if (use_transaction_registry == MAYBE)
    error= check(error);

  use_transaction_registry= error ? NO : YES;

  return error;
}

TR_table::~TR_table()
{
  if (table)
  {
    thd->temporary_tables= NULL;
    close_log_table(thd, open_tables_backup);
  }
  delete open_tables_backup;
}

void TR_table::store(uint field_id, ulonglong val)
{
  table->field[field_id]->store(val, true);
  table->field[field_id]->set_notnull();
}

void TR_table::store(uint field_id, timeval ts)
{
  table->field[field_id]->store_timestamp(ts.tv_sec, ts.tv_usec);
  table->field[field_id]->set_notnull();
}

enum_tx_isolation TR_table::iso_level() const
{
  enum_tx_isolation res= (enum_tx_isolation) ((*this)[FLD_ISO_LEVEL]->val_int() - 1);
  DBUG_ASSERT(res <= ISO_SERIALIZABLE);
  return res;
}

bool TR_table::update(ulonglong start_id, ulonglong end_id)
{
  if (!table && open())
    return true;

  store(FLD_BEGIN_TS, thd->transaction_time());
  thd->set_time();
  timeval end_time= {thd->query_start(), int(thd->query_start_sec_part())};
  store(FLD_TRX_ID, start_id);
  store(FLD_COMMIT_ID, end_id);
  store(FLD_COMMIT_TS, end_time);
  store_iso_level(thd->tx_isolation);

  int error= table->file->ha_write_row(table->record[0]);
  if (unlikely(error))
    table->file->print_error(error, MYF(0));
  /* extra() is used to apply the bulk insert operation
  on mysql/transaction_registry table */
  return error;
}

#define newx new (thd->mem_root)
bool TR_table::query(ulonglong trx_id)
{
  if (!table && open())
    return false;
  SQL_SELECT_auto select;
  READ_RECORD info;
  int error;
  List<TABLE_LIST> dummy;
  SELECT_LEX &slex= *(thd->lex->first_select_lex());
  Name_resolution_context_backup backup(slex.context, *this);
  Item *field= newx Item_field(thd, &slex.context, (*this)[FLD_TRX_ID]);
  Item *value= newx Item_int(thd, trx_id);
  COND *conds= newx Item_func_eq(thd, field, value);
  if (unlikely((error= setup_conds(thd, this, dummy, &conds))))
    return false;
  select= make_select(table, 0, 0, conds, NULL, 0, &error);
  if (unlikely(error || !select))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return false;
  }
  // FIXME: (performance) force index 'transaction_id'
  error= init_read_record(&info, thd, table, select, NULL,
                          1 /* use_record_cache */, true /* print_error */,
                          false /* disable_rr_cache */);
  while (!(error= info.read_record()) && !thd->killed && !thd->is_error())
  {
    if (select->skip_record(thd) > 0)
      return true;
  }
  my_error(ER_VERS_NO_TRX_ID, MYF(0), (longlong) trx_id);
  return false;
}

bool TR_table::query(MYSQL_TIME &commit_time, bool backwards)
{
  if (!table && open())
    return false;
  SQL_SELECT_auto select;
  READ_RECORD info;
  int error;
  List<TABLE_LIST> dummy;
  SELECT_LEX &slex= *(thd->lex->first_select_lex());
  Name_resolution_context_backup backup(slex.context, *this);
  Item *field= newx Item_field(thd, &slex.context, (*this)[FLD_COMMIT_TS]);
  Datetime dt(&commit_time);
  Item *value= newx Item_datetime_literal(thd, &dt, 6);
  COND *conds;
  if (backwards)
    conds= newx Item_func_ge(thd, field, value);
  else
    conds= newx Item_func_le(thd, field, value);
  if (unlikely((error= setup_conds(thd, this, dummy, &conds))))
    return false;
  // FIXME: (performance) force index 'commit_timestamp'
  select= make_select(table, 0, 0, conds, NULL, 0, &error);
  if (unlikely(error || !select))
    return false;
  error= init_read_record(&info, thd, table, select, NULL,
                          1 /* use_record_cache */, true /* print_error */,
                          false /* disable_rr_cache */);

  // With PK by transaction_id the records are ordered by PK, so we have to
  // scan TRT fully and collect min (backwards == true)
  // or max (backwards == false) stats.
  bool found= false;
  MYSQL_TIME found_ts;
  while (!(error= info.read_record()) && !thd->killed && !thd->is_error())
  {
    int res= select->skip_record(thd);
    if (res > 0)
    {
      MYSQL_TIME commit_ts;
      if ((*this)[FLD_COMMIT_TS]->get_date(&commit_ts, date_mode_t(0)))
      {
        found= false;
        break;
      }
      int c;
      if (!found || ((c= my_time_compare(&commit_ts, &found_ts)) &&
        (backwards ? c < 0 : c > 0)))
      {
        found_ts= commit_ts;
        found= true;
        // TODO: (performance) make ORDER DESC and break after first found.
        // Otherwise it is O(n) scan (+copy)!
        store_record(table, record[1]);
      }
    }
    else if (res < 0)
    {
      found= false;
      break;
    }
 }
  if (found)
    restore_record(table, record[1]);
  return found;
}
#undef newx

bool TR_table::query_sees(bool &result, ulonglong trx_id1, ulonglong trx_id0,
                          ulonglong commit_id1, enum_tx_isolation iso_level1,
                          ulonglong commit_id0)
{
  if (trx_id1 == trx_id0)
  {
    return false;
  }

  if (trx_id1 == ULONGLONG_MAX || trx_id0 == 0)
  {
    result= true;
    return false;
  }

  if (trx_id0 == ULONGLONG_MAX || trx_id1 == 0)
  {
    result= false;
    return false;
  }

  if (!commit_id1)
  {
    if (!query(trx_id1))
      return true;

    commit_id1= (*this)[FLD_COMMIT_ID]->val_int();
    iso_level1= iso_level();
  }

  if (!commit_id0)
  {
    if (!query(trx_id0))
      return true;

    commit_id0= (*this)[FLD_COMMIT_ID]->val_int();
  }

  // Trivial case: TX1 started after TX0 committed
  if (trx_id1 > commit_id0
      // Concurrent transactions: TX1 committed after TX0 and TX1 is read (un)committed
      || (commit_id1 > commit_id0 && iso_level1 < ISO_REPEATABLE_READ))
  {
    result= true;
  }
  else // All other cases: TX1 does not see TX0
  {
    result= false;
  }

  return false;
}

void TR_table::warn_schema_incorrect(const char *reason)
{
  if (MYSQL_VERSION_ID == table->s->mysql_version)
  {
    sql_print_error("%`s.%`s schema is incorrect: %s.",
                    db.str, table_name.str, reason);
  }
  else
  {
    sql_print_error("%`s.%`s schema is incorrect: %s. Created with MariaDB %d, "
                    "now running %d.",
                    db.str, table_name.str, reason, MYSQL_VERSION_ID,
                    static_cast<int>(table->s->mysql_version));
  }
}

bool TR_table::check(bool error)
{
  if (error)
  {
    sql_print_warning("%`s.%`s does not exist (open failed).", db.str,
                      table_name.str);
    return true;
  }

  if (table->file->ht->db_type != DB_TYPE_INNODB)
  {
    warn_schema_incorrect("Wrong table engine (expected InnoDB)");
    return true;
  }

#define WARN_SCHEMA(...) \
  char reason[128]; \
  snprintf(reason, 128, __VA_ARGS__); \
  warn_schema_incorrect(reason);

  if (table->s->fields != FIELD_COUNT)
  {
    WARN_SCHEMA("Wrong field count (expected %d)", FIELD_COUNT);
    return true;
  }

  if (table->field[FLD_TRX_ID]->type() != MYSQL_TYPE_LONGLONG)
  {
    WARN_SCHEMA("Wrong field %d type (expected BIGINT UNSIGNED)", FLD_TRX_ID);
    return true;
  }

  if (table->field[FLD_COMMIT_ID]->type() != MYSQL_TYPE_LONGLONG)
  {
    WARN_SCHEMA("Wrong field %d type (expected BIGINT UNSIGNED)", FLD_COMMIT_ID);
    return true;
  }

  if (table->field[FLD_BEGIN_TS]->type() != MYSQL_TYPE_TIMESTAMP)
  {
    WARN_SCHEMA("Wrong field %d type (expected TIMESTAMP(6))", FLD_BEGIN_TS);
    return true;
  }

  if (table->field[FLD_COMMIT_TS]->type() != MYSQL_TYPE_TIMESTAMP)
  {
    WARN_SCHEMA("Wrong field %d type (expected TIMESTAMP(6))", FLD_COMMIT_TS);
    return true;
  }

  if (table->field[FLD_ISO_LEVEL]->type() != MYSQL_TYPE_STRING ||
      !(table->field[FLD_ISO_LEVEL]->flags & ENUM_FLAG))
  {
  wrong_enum:
    WARN_SCHEMA("Wrong field %d type (expected ENUM('READ-UNCOMMITTED', "
                "'READ-COMMITTED', 'REPEATABLE-READ', 'SERIALIZABLE'))",
                FLD_ISO_LEVEL);
    return true;
  }

  Field_enum *iso_level= static_cast<Field_enum *>(table->field[FLD_ISO_LEVEL]);
  const st_typelib *typelib= iso_level->typelib;

  if (typelib->count != 4)
    goto wrong_enum;

  if (strcmp(typelib->type_names[0], "READ-UNCOMMITTED") ||
      strcmp(typelib->type_names[1], "READ-COMMITTED") ||
      strcmp(typelib->type_names[2], "REPEATABLE-READ") ||
      strcmp(typelib->type_names[3], "SERIALIZABLE"))
  {
    goto wrong_enum;
  }

  if (!table->key_info || !table->key_info->key_part)
    goto wrong_pk;

  if (strcmp(table->key_info->key_part->field->field_name.str, "transaction_id"))
  {
  wrong_pk:
    WARN_SCHEMA("Wrong PRIMARY KEY (expected `transaction_id`)");
    return true;
  }

  return false;
}

bool vers_select_conds_t::check_units(THD *thd)
{
  DBUG_ASSERT(type != SYSTEM_TIME_UNSPECIFIED);
  DBUG_ASSERT(start.item);
  return start.check_unit(thd, this) ||
         end.check_unit(thd, this);
}

bool vers_select_conds_t::eq(const vers_select_conds_t &conds) const
{
  if (type != conds.type)
    return false;
  switch (type) {
  case SYSTEM_TIME_UNSPECIFIED:
  case SYSTEM_TIME_ALL:
    return true;
  case SYSTEM_TIME_BEFORE:
    break;
  case SYSTEM_TIME_HISTORY:
    break;
  case SYSTEM_TIME_AS_OF:
    return start.eq(conds.start);
  case SYSTEM_TIME_FROM_TO:
  case SYSTEM_TIME_BETWEEN:
    return start.eq(conds.start) && end.eq(conds.end);
  }
  DBUG_ASSERT(0);
  return false;
}


bool Vers_history_point::check_unit(THD *thd, vers_select_conds_t *vers_conds)
{
  if (!item)
    return false;
  if (item->real_type() == Item::FIELD_ITEM)
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
             item->full_name(), "FOR SYSTEM_TIME");
    return true;
  }
  else if (item->with_param())
    vers_conds->has_param= true;

  if (item->fix_fields_if_needed(thd, &item))
    return true;
  const Type_handler *t= item->this_item()->real_type_handler();
  DBUG_ASSERT(t);
  if (!t->vers())
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
             t->name().ptr(), "FOR SYSTEM_TIME");
    return true;
  }
  return false;
}


void Vers_history_point::fix_item()
{
  if (item && item->decimals == 0 && item->type() == Item::FUNC_ITEM &&
      ((Item_func*)item)->functype() == Item_func::NOW_FUNC)
    item->decimals= 6;
}


bool Vers_history_point::eq(const vers_history_point_t &point) const
{
  return unit == point.unit && item->eq(point.item, false);
}

void Vers_history_point::print(String *str, enum_query_type query_type,
                               const char *prefix, size_t plen) const
{
  const static LEX_CSTRING unit_type[]=
  {
    { STRING_WITH_LEN("") },
    { STRING_WITH_LEN("TIMESTAMP ") },
    { STRING_WITH_LEN("TRANSACTION ") }
  };
  str->append(prefix, plen);
  str->append(unit_type + unit);
  item->print(str, query_type);
}

Field *TABLE::find_field_by_name(const LEX_CSTRING *str) const
{
  Field **tmp;
  size_t length= str->length;
  if (s->name_hash.records)
  {
    tmp= (Field**) my_hash_search(&s->name_hash, (uchar*) str->str, length);
    return tmp ? field[tmp - s->field] : NULL;
  }
  else
  {
    for (tmp= field; *tmp; tmp++)
    {
      if ((*tmp)->field_name.length == length &&
          !lex_string_cmp(system_charset_info, &(*tmp)->field_name, str))
        return *tmp;
    }
  }
  return NULL;
}


bool TABLE::export_structure(THD *thd, Row_definition_list *defs)
{
  for (Field **src= field; *src; src++)
  {
    uint offs;
    if (defs->find_row_field_by_name(&src[0]->field_name, &offs))
    {
      my_error(ER_DUP_FIELDNAME, MYF(0), src[0]->field_name.str);
      return true;
    }
    Spvar_definition *def= new (thd->mem_root) Spvar_definition(thd, *src);
    if (!def)
      return true;
    def->flags&= (uint) ~NOT_NULL_FLAG;
    if ((def->sp_prepare_create_field(thd, thd->mem_root)) ||
        (defs->push_back(def, thd->mem_root)))
      return true;
  }
  return false;
}

/**
  @brief
    Initialize all the opt_range structures that are used to stored the
    estimates when the range optimizer is run.
    As these are initialized by the range optimizer for all index
    marked in opt_range_keys, we only mark the memory as undefined
    to be able to find wrong usage of data with valgrind or MSAN.
*/

inline void TABLE::initialize_opt_range_structures()
{
  TRASH_ALLOC((void*)&opt_range_keys, sizeof(opt_range_keys));
  TRASH_ALLOC((void*)opt_range, s->keys * sizeof(*opt_range));
  TRASH_ALLOC(const_key_parts, s->keys * sizeof(*const_key_parts));
}


double TABLE::OPT_RANGE::index_only_fetch_cost(TABLE *table)
{
  return (table->file->cost(cost.index_cost)+
          (double) rows * table->s->optimizer_costs.key_copy_cost);
}


/*
  Convert range cost to ALL_READ_COST
  Note that the returned cost does not include the WHERE cost
  (costs.comp_cost).
*/

void TABLE::OPT_RANGE::get_costs(ALL_READ_COST *res)
{
  res->index_cost=       cost.index_cost;
  res->row_cost=         cost.row_cost;
  res->copy_cost=        cost.copy_cost;
  res->max_index_blocks= max_index_blocks;
  res->max_row_blocks=   max_row_blocks;
}


/*
  Mark table to be reopened after query
*/

void TABLE::mark_table_for_reopen()
{
  THD *thd= in_use;
  DBUG_ASSERT(thd);
  thd->locked_tables_list.mark_table_for_reopen(this);
}
