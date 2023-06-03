/* Copyright (c) 2023, MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_class.h"                          // killed_state
#include "mysql/services.h"                     // LEX_CSTRING
#include "sql_db.h"                             // load_opt()
#include "catalog.h"
#include <m_string.h>                           // LEX_CSTRING
#include "sql_lex.h"                            // empty_lex_str
#include "lock.h"                               // lock_schema
#include "sql_table.h"                          // build_table_filename
#include "mysys_err.h"                          // EE_STATE
#include "sql_db.h"                             // write_db_opt
#include "log_event.h"                          //
#ifdef _WIN32
#include <direct.h>
#endif

const LEX_CSTRING default_catalog_name= { STRING_WITH_LEN("def") };
const LEX_CSTRING default_catalog_path= { STRING_WITH_LEN("def" FN_DIRSEP) };
const LEX_CSTRING default_catalog_comment=
{ STRING_WITH_LEN("default catalog") };
SQL_CATALOG internal_default_catalog{&default_catalog_name, &empty_clex_str};
Hash_set <SQL_CATALOG> catalog_hash;
my_bool using_catalogs;

/* This is only used for the default catalog */
SQL_CATALOG::SQL_CATALOG(const LEX_CSTRING *name_arg,
                         const LEX_CSTRING *path_arg)
  :name(*name_arg), path(*path_arg)
{
  next= 0;                       // drop list
  cs= 0;
  acl= ALL_KNOWN_ACL;            // For no catalogs or default catalog
  initialized=0;
  deleted= 0;
  comment.str= 0; comment.length= 0;
  /*
    Initialization that allocates memory or depends on server startup options
    are done in initialize_from_env()
 */
}

#ifdef EMBEDDED_LIBRARY

bool check_if_using_catalogs()
{
  my_error(ER_NO_CATALOGS, MYF(0));
  return 1;
}
SQL_CATALOG *get_catalog(const LEX_CSTRING *name, bool initialize)
{
  return &internal_default_catalog;
}
SQL_CATALOG *get_catalog_with_error(const THD *thd,
                                    const LEX_CSTRING *name, bool initialize)
{
  return &internal_default_catalog;
}
bool check_catalog_access(THD *thd, const LEX_CSTRING *name)
{
  return 1;
}
bool init_catalogs(const char *datadir)
{
  return 0;
}
void free_catalogs()
{}
#else

/* Set to 1 in late_init_all_catalogs() */
static bool late_init_done= 0;

static SQL_I_List<SQL_CATALOG> deleted_catalogs;


/* Check if the server is configured with catalog support */

bool check_if_using_catalogs()
{
  if (using_catalogs)
    return 0;
  my_error(ER_NO_CATALOGS, MYF(0));
  return 1;
}

/*
  Get catalog object
*/

SQL_CATALOG *get_catalog(const LEX_CSTRING *name, bool initialize)
{
  SQL_CATALOG *catalog;
  DBUG_ENTER("get_catalog");
  DBUG_PRINT("enter", ("catalog: %.*s", name->length, name->str));
  if (name->length == internal_default_catalog.name.length &&
      !strncmp(name->str, internal_default_catalog.name.str,
               internal_default_catalog.name.length))
    DBUG_RETURN(&internal_default_catalog);

  if (!using_catalogs)
    DBUG_RETURN(0);

  mysql_mutex_lock(&LOCK_catalogs);
  if ((catalog= catalog_hash.find((void*) name->str, name->length)))
  {
    if (catalog->initialized < 2 && initialize && late_init_done)
    {
      if (catalog->late_init())
        catalog= 0;                             // Catalog not usable
    }
  }
  mysql_mutex_unlock(&LOCK_catalogs);
  DBUG_RETURN(catalog);
}

/*
  Get catalog object, give error if not found
*/

SQL_CATALOG *get_catalog_with_error(const THD *thd, const LEX_CSTRING *name,
                                    bool initialize)
{
  SQL_CATALOG *catalog= get_catalog(name, initialize);
  if (!catalog)
  {
    Security_context *sctx= thd->security_ctx;
    my_error(ER_ACCESS_NO_SUCH_CATALOG, MYF(ME_ERROR_LOG),
             sctx->priv_user, sctx->host_or_ip,
             name->length, name->str);
  }
  return catalog;
}


/*
  Check if user has the right to use catalogs
*/

bool check_catalog_access(THD *thd, const LEX_CSTRING *name)
{
  Security_context *sctx= thd->security_ctx;
  if (!(sctx->master_access & CATALOG_ACL))
  {
    status_var_increment(thd->status_var.access_denied_errors);
    my_error(ER_CATALOG_ACCESS_DENIED_ERROR, MYF(ME_ERROR_LOG),
             sctx->priv_user, sctx->host_or_ip, name->str);
    return true;
  }
  return false;
}


/*
  Change catalog.

  Catalog can only be changed by users of the 'def' catalog with the CATALOG
  privilege, a 'catalog root user'. (CRU)

  We allow any users to execute CHANGE CATALOG 'my-current-catalog' to
  make it easier to do scripts.

  The CRU keeps all privileges the user has in the 'def' catalog.

  Note that the CRU should not create any objects that includes
  his name or privileges in other catalogs (like VIEWS) as these will not
  be usable for users withing the catalog.
*/

bool mariadb_change_catalog(THD *thd, const LEX_CSTRING *catalog_name)
{
  SQL_CATALOG *catalog;

  if (thd->in_active_multi_stmt_transaction())
  {
    my_error(ER_CANT_CHANGE_TX_CHARACTERISTICS, MYF(0));
    return 1;
  }

  /* Allow 'change' to the current catalog */
  if (!strcmp(thd->catalog->name.str, catalog_name->str))
    return 0;

  if (check_if_using_catalogs() || check_catalog_access(thd, catalog_name))
    return 1;

  if (!(catalog= get_catalog_with_error(thd, catalog_name, 1)))
    return 1;

  thd->catalog= catalog;
  thd->set_db(&null_clex_str);
  return 0;
}


/* Initialize from variables that are set by MariaDB initialization */

void SQL_CATALOG::initialize_from_env()
{
  cs= default_charset_info;
  /*
    privileges allowed for this catalog. Note that the ACL's for the
    default_catalog is set in init_catalog_directories!
  */
  acl= using_catalogs ? CATALOG_ACLS : ALL_KNOWN_ACL;

  mysql_mutex_init(key_LOCK_catalogs, &lock_status, MY_MUTEX_INIT_SLOW);
  initialized= 1;
}

/*
  Update catalog variables after MariaDB has fully started and all
  engines are up and running.

  Things done:
  - TODO: Read privilege tables
*/

bool SQL_CATALOG::late_init()
{
  initialized= 2;
  return 0;
}

/*
  Call late_init() for all catalogs
*/

bool late_init_all_catalogs()
{
  mysql_mutex_lock(&LOCK_catalogs);
  Hash_set<SQL_CATALOG>::Iterator it{catalog_hash};
  while (SQL_CATALOG *cat= it++)
  {
    if (cat->initialized < 2)
    {
      if (cat->late_init())
      {
        mysql_mutex_unlock(&LOCK_catalogs);
        return 1;
      }
    }
  }
  late_init_done= 1;
  mysql_mutex_unlock(&LOCK_catalogs);
  return 0;
}

/*
  Check if MariaDB is configured for catalogs

  @return 1  Using catalogs
  @return 0  No catalogs
*/

static bool check_if_configured_for_catalogs(const char *datadir)
{
  char name[FN_REFLEN];
  strxnmov(name, sizeof(name)-1, datadir, "def", FN_DIRSEP, "mysql",
           FN_DIRSEP, "proc.frm", NullS);
  return access(name, F_OK) == 0;
}

static bool check_if_configured_for_original_setup(const char *datadir)
{
  char name[FN_REFLEN];
  strxnmov(name, sizeof(name)-1, datadir, "mysql",
           FN_DIRSEP, "proc.frm", NullS);
  return access(name, F_OK) == 0;
}

/*
  Functions to handle catalog hash
*/

static uchar *get_catalog_key(SQL_CATALOG *cat, size_t *length,
                              my_bool first)
{
  *length= cat->name.length;
  return (uchar*) cat->name.str;
}


/*
  Delete a catalog from deleted_catalogs hash

  Catalogs does not have any usage counters, which means that
  there may still be THD objects using the catalog even after
  it is deleted.

  To ensure that we don't get crashes when removing catalogs,
  all old catalogs are put into a delete list which is freed
  when the server goes down.

  As droping catalogs is a rare things and the catalog objects
  are small, this approch should not be a problem. The faster
  access to catalog objects should compensate for this.
*/

static void move_catalog_to_delete_list(SQL_CATALOG *cat)
{
  /*
    Mark catalog as deleted to inform users that is still using
    the catalog
  */
  cat->deleted=1;
  /*
    Move deleted catalogs to delete_catalos list as they could still
    be in use by some thd, like replication.
  */
  deleted_catalogs.link_in_list(cat, &cat->next);
}


void SQL_CATALOG::free()
{
  my_free((char*) comment.str);
  comment.str= 0;
  if (initialized)
  {
    initialized= 0;
    mysql_mutex_destroy(&lock_status);
  }
  if (this != &internal_default_catalog)
    my_free(this);
}


/**
  Add a catalog to catalog_hash

  @param memroot   memroot to store data for load_opt(). This is a temporary
                   storage space that can be freed when this function returns.
  @param datadir   Top directory to scan.
  @param name      Catalog 'user display' name. May be different from
                   datadir if special symbols is in the name.
*/

static bool add_catalog(MEM_ROOT *memroot, const char *datadir,
                        const LEX_CSTRING *name)
{
  LEX_CSTRING *tmp;
  char opt_path[FN_REFLEN + 1], *ptr;
  Schema_specification_st create;
  SQL_CATALOG *cat;

  /*
    Read the catalog option file. It has the same structure
    and information as the database option file
  */
  strxnmov(opt_path, sizeof(opt_path) - 1, datadir, MY_CATALOG_OPT_FILE,
           NullS);
  bzero(&create, sizeof(create));
  create.default_table_charset= default_charset_info;
  (void) load_opt(opt_path, &create, memroot, MY_UTF8_IS_UTF8MB3);
  if (!create.schema_comment)
  {
    /* No comment or EOM */
    create.schema_comment= (LEX_CSTRING*) &empty_clex_str;
  }

  if (! my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME),
                        &cat, sizeof(*cat),
                        &ptr, (name->length+1)*2+1,
                        NullS))
    return 1;
  bzero((void*) cat, sizeof(*cat));

  memcpy(ptr, name->str, name->length+1);
  tmp= (LEX_CSTRING*) &cat->name;
  tmp->str= ptr;
  tmp->length= name->length;
  ptr+= name->length+1;

  strxmov(ptr, name->str, FN_DIRSEP, NullS);
  tmp= (LEX_CSTRING*) &cat->path;
  tmp->str= ptr;
  tmp->length= name->length+1;
  ptr+= name->length+2;

  lex_string_set3(&cat->comment,
                  (char*) my_memdup(PSI_INSTRUMENT_ME,
                                    create.schema_comment->str,
                                    create.schema_comment->length+1,
                                    MYF(MY_WME)),
                  create.schema_comment->length);
  if (!cat->comment.str)
    cat->comment.length= 0;

  cat->initialize_from_env();
  mysql_mutex_lock(&LOCK_catalogs);
  if (catalog_hash.insert(cat))
  {
    cat->free();
    mysql_mutex_unlock(&LOCK_catalogs);
    return 1;                               // End of memory
  }
  mysql_mutex_unlock(&LOCK_catalogs);
  return 0;
}


static bool init_catalog_directories(const char *datadir)
{
  MY_DIR *dirp;
  MEM_ROOT memroot;
  bool error= 1;
  DBUG_ENTER("init_catalog_directories");

  my_hash_init2(PSI_INSTRUMENT_ME, &catalog_hash.m_hash, 16, &my_charset_bin,
                32, 0, 0, (my_hash_get_key) get_catalog_key, 0,
                (my_hash_free_key) move_catalog_to_delete_list, HASH_UNIQUE);

  internal_default_catalog.initialize_from_env();
  /* Only the default catalog has CATALOG_ACLS */
  internal_default_catalog.acl= ALL_KNOWN_ACL;
  catalog_hash.insert(&internal_default_catalog);

  DBUG_ASSERT(strend(datadir)[-1] == FN_LIBCHAR);

  if (!(dirp= my_dir(datadir, MYF(MY_WANT_STAT))))
  {
    my_error(ER_CANT_READ_DIR, MYF(0), datadir, my_errno);
    DBUG_RETURN(0);
  }

  init_alloc_root(PSI_INSTRUMENT_ME, &memroot, 1024, 0, MYF(MY_WME));

  for (size_t i=0; i < dirp->number_of_files; i++)
  {
    FILEINFO *file= dirp->dir_entry+i;

    /* Ignore names starting with '.' or '#' and the default category */
    if (file->name[0] != '.' &&
        file->name[0] != '#' &&
        MY_S_ISDIR(file->mystat->st_mode) &&
        strcmp(file->name, default_catalog_name.str))
    {
      LEX_CSTRING name= { file->name, strlen(file->name) };
      if (add_catalog(&memroot, datadir, &name))
        goto err;
      free_root(&memroot, MY_MARK_BLOCKS_FREE); // Reuse memory
    }
  }
  error= 0;

err:
  my_dirend(dirp);
  free_root(&memroot, 0);
  DBUG_RETURN(error);
}


/*
  Initialize catalog handling
*/

bool init_catalogs(const char *datadir)
{
  bool have_catalogs= check_if_configured_for_catalogs(datadir);
  DBUG_ENTER("init_catalogs");

  my_free((char*) internal_default_catalog.comment.str);
  lex_string_set3(&internal_default_catalog.comment,
                  (char*) my_memdup(PSI_INSTRUMENT_ME,
                                    default_catalog_comment.str,
                                    default_catalog_comment.length+1,
                                    MYF(MY_WME)),
                  default_catalog_comment.length);
  if (!internal_default_catalog.comment.str)
    internal_default_catalog.comment.length= 0;

  if (using_catalogs && !have_catalogs)
  {
    if (!opt_bootstrap && check_if_configured_for_original_setup(datadir))
    {
      sql_print_error("--catalog option is used but MariaDB is not configured "
                      "for catalogs");
      DBUG_RETURN(1);
    }
  }
  using_catalogs|= (my_bool) have_catalogs;
  if (using_catalogs)
  {
    /*
      We are using the catalogs.  Change default catalog to have a path.
    */
    LEX_CSTRING *tmp= const_cast<LEX_CSTRING*>(&internal_default_catalog.path);
    *tmp= default_catalog_path;
    if (init_catalog_directories(datadir))
    {
      sql_print_error("Catalogs could not be initialized");
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}


void free_catalogs()
{
  SQL_CATALOG *catalog, *next;
  /* Move all catalogs to delete_catalog list and free hash */
  catalog_hash.free();

  for (catalog= deleted_catalogs.first ; catalog ; catalog= next)
  {
    next= catalog->next;
    catalog->free();
  }
  deleted_catalogs.empty();
  /* Free data from internal_default_catalog last */
  internal_default_catalog.free();
  late_init_done= 0;
}


/*
  Create a catalog

  SYNOPSIS
  maria_create_catalog_internal()
  thd		Thread handler
  name		Name of catalog to create
		Function assumes that this is already validated.
  options       DDL options, e.g. IF NOT EXISTS
  create_info	Database create options (like character set)
  silent	Used by replication when internally creating a database.
		In this case the entry should not be logged.

  SIDE-EFFECTS
   1. Report back to client that command succeeded (my_ok)
   2. Report errors to client
   3. Log event to binary log
   (The 'silent' flags turns off 1 and 3.)

  RETURN VALUES
  FALSE ok
  TRUE  Error

*/

/* tmp_catalog is used for locking catalogs with lock_schema_name */
static SQL_CATALOG tmp_catalog(&empty_clex_str, &empty_clex_str);

static int
maria_create_catalog_internal(THD *thd, const LEX_CSTRING *name,
                              const DDL_options_st &options,
                              Schema_specification_st *create_info,
                              bool silent)
{
  char	 path[FN_REFLEN+16];
  MY_STAT stat_info;
  uint path_len;
  char tmp[SAFE_NAME_LEN+1];
  const char *norm= normalize_db_name(name->str, tmp, sizeof(tmp));
  SQL_CATALOG *org_catalog;
  bool err;
  DBUG_ENTER("maria_create_catalog_internal");

  org_catalog= thd->catalog;
  thd->catalog= &tmp_catalog;
  err= lock_schema_name(thd, norm);
  thd->catalog= org_catalog;
  if (err)
    DBUG_RETURN(-1);

  /* Check directory */
  path_len= build_table_filename(&tmp_catalog, path, sizeof(path) - 1,
                                 name->str, "", "", 0);
  path[path_len-1]= 0;                    // Remove last '/' from path

  long affected_rows= 1;
  if (!mysql_file_stat(key_file_misc, path, &stat_info, MYF(0)))
  {
    // The database directory does not exist, or my_file_stat() failed
    if (my_errno != ENOENT)
    {
      my_error(EE_STAT, MYF(0), path, my_errno);
      DBUG_RETURN(1);
    }
  }
  else if (options.if_not_exists())
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_CATALOG_CREATE_EXISTS,
                        ER_THD(thd, ER_DB_CREATE_EXISTS),
                        name->str);
    goto log_to_binlog;
  }
  else
  {
    my_error(ER_CATALOG_CREATE_EXISTS, MYF(0), name->str);
    DBUG_RETURN(-1);
  }

  if (my_mkdir(path, 0777, MYF(0)) < 0)
  {
    my_error(ER_CANT_CREATE_CATALOG, MYF(0), name->str, my_errno);
    DBUG_RETURN(-1);
  }

  path[path_len-1]= FN_LIBCHAR;
  strmake(path+path_len, MY_CATALOG_OPT_FILE, sizeof(path)-path_len-1);
  if (write_db_opt(thd, path, name, create_info))
  {
    /*
      Could not create options file.
      Restore things to beginning.
    */
    path[path_len]= 0;
    if (rmdir(path) >= 0)
      DBUG_RETURN(-1);
    /*
      We come here when we managed to create the database, but not the option
      file.  In this case it's best to just continue as if nothing has
      happened.  (This is a very unlikely senario)
    */
    thd->clear_error();
  }

  add_catalog(thd->mem_root, mysql_real_data_home, name);

  /* Log command to ddl log */
  backup_log_info ddl_log;
  bzero(&ddl_log, sizeof(ddl_log));
  ddl_log.query=                   { C_STRING_WITH_LEN("CREATE") };
  ddl_log.org_storage_engine_name= { C_STRING_WITH_LEN("CATALOG") };
  ddl_log.catalog= get_catalog(name, 0);
  if (ddl_log.catalog)
    backup_log_ddl(&ddl_log);

log_to_binlog:
  if (!silent)
  {
    char *query;
    uint query_length;

    query=        thd->query();
    query_length= thd->query_length();
    DBUG_ASSERT(query);

    if (mysql_bin_log.is_open())
    {
      int errcode= query_error_code(thd, TRUE);
      Query_log_event qinfo(thd, query, query_length, FALSE, TRUE,
			    /* suppress_use */ TRUE, errcode);
      if (mysql_bin_log.write(&qinfo))
        DBUG_RETURN(-1);
    }
    my_ok(thd, affected_rows);
  }
  DBUG_RETURN(0);
}


int maria_create_catalog(THD *thd, const LEX_CSTRING *name,
                         DDL_options_st options,
                         const Schema_specification_st *create_info)
{
  if (check_if_using_catalogs())
    return -1;

  DBUG_ASSERT(create_info->default_table_charset);
  /*
    As mysql_create_db_internal() may modify Db_create_info structure passed
    to it, we need to use a copy to make execution prepared statement- safe.
  */
  Schema_specification_st tmp(*create_info);
  if (thd->slave_thread &&
      slave_ddl_exec_mode_options == SLAVE_EXEC_MODE_IDEMPOTENT)
    options.add(DDL_options::OPT_IF_NOT_EXISTS);
  return maria_create_catalog_internal(thd, name, options, &tmp, false);
}

/**
  Remove a catalog
  For now it only removes the catalog.log file and the catalog directory

  @param  thd        Thread handle
  @param  catalog    Catalog name in the case given by user
                     It's already validated and set to lower case
                     (if needed) when we come here
  @param  if_exists  Don't give error if catalog doesn't exists
  @param  silent     Don't write the statement to the binary log and don't
                     send ok packet to the client

  @retval  false  OK (Catalog dropped)
  @retval  true   Error
*/

static bool
rm_catalog_internal(THD *thd, const LEX_CSTRING *name, bool if_exists,
                    bool silent)
{
  bool error= true, err;
  char path[FN_REFLEN + 16], tmp[SAFE_NAME_LEN+2];
  MY_DIR *dirp;
  LEX_CSTRING rm_catalog;
  char *normalized_name= (char*) normalize_db_name(name->str, tmp, sizeof(tmp));
  SQL_CATALOG *catalog, *org_catalog= thd->catalog;
  DBUG_ENTER("rm_catalog_internal");

  if (normalized_name != tmp)
  {
    /* Ensure normalized name is stored in tmp so that we can change it later */
    strnmov(tmp, normalized_name, sizeof(tmp)-2);
    normalized_name= tmp;
  }

  lex_string_set(&rm_catalog, normalized_name);
  thd->catalog= &tmp_catalog;
  err= lock_schema_name(thd, normalized_name);
  build_table_filename(thd->catalog, path, sizeof(path) - 1,
                       name->str, "", "", 0);
  thd->catalog= org_catalog;
  if (err)
    DBUG_RETURN(1);

  if (!(catalog= get_catalog(&rm_catalog, 0)))
  {
    my_error(ER_NO_SUCH_CATALOG, MYF(0), rm_catalog.str);
    DBUG_RETURN(1);
  }

  /* See if the directory exists */
  if (!(dirp= my_dir(path, MYF(MY_DONT_SORT))))
  {
    if (!if_exists)
    {
      my_error(ER_CATALOG_DROP_EXISTS, MYF(0), name->str);
      DBUG_RETURN(true);
    }
    else
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
			  ER_CATALOG_DROP_EXISTS,
                          ER_THD(thd, ER_CATALOG_DROP_EXISTS),
                          name->str);
      /* Log the command to binlog to ensure that the slave is in sync */
      goto log_to_binlog;
    }
  }

  /* Check if catalog is empty */
  for (size_t i= 0; i < dirp->number_of_files; i++)
  {
    FILEINFO *file= dirp->dir_entry + i;

    /* Ignore names starting with '.' or '#' and the catalog.opt file */
    if (strcmp(file->name, ".") &&
        strcmp(file->name, "..") &&
        strcmp(file->name, MY_CATALOG_OPT_FILE))
    {
      my_error(ER_CATALOG_NOT_EMPTY, MYF(0), rm_catalog.str);
      goto err;
    }
  }

  strxnmov(strend(path), sizeof(path)-1, FN_DIRSEP, MY_CATALOG_OPT_FILE, NullS);
  if (mysql_file_delete_with_symlink(key_file_misc, path, "", MYF(0)) &&
      my_errno != ENOENT)
  {
    my_error(EE_DELETE, MYF(0), path, my_errno);
    goto err;
  }
  if (rm_dir_w_symlink(rm_catalog.str, 1))
    goto err;

  catalog->deleted= 1;
  catalog_hash.remove(catalog);

  /*
    If current catalog was removed, change to the 'def' catalog
    This ensures that thd always points to a valid catalog object.
   */
  if (thd->catalog == catalog)
    thd->catalog= default_catalog();

  /* Log command to ddl log */
  backup_log_info ddl_log;
  bzero(&ddl_log, sizeof(ddl_log));
  ddl_log.query=                   { C_STRING_WITH_LEN("DROP") };
  ddl_log.org_storage_engine_name= { C_STRING_WITH_LEN("CATALOG") };
  ddl_log.org_database=            rm_catalog;
  ddl_log.catalog=                 thd->catalog;
  backup_log_ddl(&ddl_log);

log_to_binlog:
  if (!silent)
  {
    char *query;
    uint query_length;

    query=        thd->query();
    query_length= thd->query_length();
    DBUG_ASSERT(query);

    if (mysql_bin_log.is_open())
    {
      int errcode= query_error_code(thd, TRUE);
      Query_log_event qinfo(thd, query, query_length, FALSE, TRUE,
			    /* suppress_use */ TRUE, errcode);
      if (mysql_bin_log.write(&qinfo))
        goto err;
    }
  }
  error=0;
  my_ok(thd);

err:
  my_dirend(dirp);
  DBUG_RETURN(error);
}


bool maria_rm_catalog(THD *thd, const LEX_CSTRING *catalog, bool if_exists)
{
  if (check_if_using_catalogs())
    return 1;
  if (thd->slave_thread &&
      slave_ddl_exec_mode_options == SLAVE_EXEC_MODE_IDEMPOTENT)
    if_exists= true;
  return rm_catalog_internal(thd, catalog, if_exists, false);
}

/*
  ALTER catalog definitions in catalog.opt
*/

static bool
maria_alter_catalog_internal(THD *thd, SQL_CATALOG *catalog,
                             Schema_specification_st *create_info)
{
  char path[FN_REFLEN+16];
  int error= 1;
  SQL_CATALOG *org_catalog= thd->catalog;
  DBUG_ENTER("maria_alter_catalog_internal");

  thd->catalog= &tmp_catalog;
  if (lock_schema_name(thd, catalog->name.str))
    DBUG_RETURN(TRUE);

  /*
     Recreate db options file: /dbpath/.db.opt
     We pass MY_DB_OPT_FILE as "extension" to avoid
     "table name to file name" encoding.
  */
  strxnmov(path, sizeof(path)-1, catalog->path.str, MY_CATALOG_OPT_FILE, NullS);
  if (unlikely((error= write_db_opt(thd, path, &catalog->name, create_info))))
    goto exit;

  /* Change options for the catalog */
  catalog->cs= create_info->default_table_charset;
  if (create_info->schema_comment)
  {
    my_free((char*) catalog->comment.str);
    lex_string_set3(&catalog->comment,
                    (char*) my_memdup(PSI_INSTRUMENT_ME,
                                      create_info->schema_comment->str,
                                      create_info->schema_comment->length+1,
                                      MYF(MY_WME)),
                    create_info->schema_comment->length);
    if (!catalog->comment.str)
      catalog->comment.length= 0;
  }

  /* Log command to ddl log */
  backup_log_info ddl_log;
  bzero(&ddl_log, sizeof(ddl_log));
  ddl_log.query=                   { C_STRING_WITH_LEN("ALTER") };
  ddl_log.org_storage_engine_name= { C_STRING_WITH_LEN("CATALOG") };
  ddl_log.catalog=                 catalog;
  backup_log_ddl(&ddl_log);

  if (mysql_bin_log.is_open())
  {
    int errcode= query_error_code(thd, TRUE);
    /*
      Write should use the catalob being created as the "current
      catalog" and not the threads current catalog.
    */
    thd->catalog= catalog;
    Query_log_event qinfo(thd, thd->query(),
                          thd->query_length(), FALSE, TRUE,
			  /* suppress_use */ TRUE, errcode);
    if (unlikely((error= mysql_bin_log.write(&qinfo))))
      goto exit;
  }
  error= 0;
  my_ok(thd);

exit:
  thd->catalog= org_catalog;
  DBUG_RETURN(error);
}


bool maria_alter_catalog(THD *thd, SQL_CATALOG *catalog,
                         const Schema_specification_st *create_info)
{
  /*
    As mysql_alter_db_internal() may modify Db_create_info structure passed
    to it, we need to use a copy to make execution prepared statement- safe.
  */
  Schema_specification_st tmp(*create_info);
  return maria_alter_catalog_internal(thd, catalog, &tmp);
}

#endif /* EMBEDDED_LIBRARY */
