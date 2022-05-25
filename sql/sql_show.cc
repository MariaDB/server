/* Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2009, 2022, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


/* Function with list databases, tables or fields */

#include "sql_plugin.h"                         // SHOW_MY_BOOL
#include "sql_priv.h"
#include "unireg.h"
#include "sql_acl.h"                        // fill_schema_*_privileges
#include "sql_select.h"                         // For select_describe
#include "sql_base.h"                       // close_tables_for_reopen
#include "create_options.h"
#include "sql_show.h"
#include "sql_table.h"                        // filename_to_tablename,
                                              // primary_key_name,
                                              // build_table_filename
#include "sql_view.h"
#include "repl_failsafe.h"
#include "sql_parse.h"             // check_access, check_table_access
#include "sql_partition.h"         // partition_element
#include "sql_derived.h"           // mysql_derived_prepare,
                                   // mysql_handle_derived,
#include "sql_db.h"     // check_db_dir_existence, load_db_opt_by_name
#include "sql_time.h"   // interval_type_to_name
#include "tztime.h"                             // struct Time_zone
#include "sql_acl.h"     // TABLE_ACLS, check_grant, DB_ACLS, acl_get,
                         // check_grant_db
#include "sp.h"
#include "sp_head.h"
#include "sp_pcontext.h"
#include "set_var.h"
#include "sql_trigger.h"
#include "sql_derived.h"
#include "sql_statistics.h"
#include "sql_connect.h"
#include "authors.h"
#include "contributors.h"
#include "sql_partition.h"
#ifdef HAVE_EVENT_SCHEDULER
#include "events.h"
#include "event_data_objects.h"
#endif
#include <my_dir.h>
#include "lock.h"                           // MYSQL_OPEN_IGNORE_FLUSH
#include "debug_sync.h"
#include "keycaches.h"
#include "ha_sequence.h"
#ifdef WITH_PARTITION_STORAGE_ENGINE
#include "ha_partition.h"
#endif
#include "transaction.h"
#include "opt_trace.h"
#include "my_cpu.h"


#include "lex_symbol.h"
#define KEYWORD_SIZE 64

extern SYMBOL symbols[];
extern size_t symbols_length;

extern SYMBOL sql_functions[];
extern size_t sql_functions_length;

extern Native_func_registry_array native_func_registry_array;

enum enum_i_s_events_fields
{
  ISE_EVENT_CATALOG= 0,
  ISE_EVENT_SCHEMA,
  ISE_EVENT_NAME,
  ISE_DEFINER,
  ISE_TIME_ZONE,
  ISE_EVENT_BODY,
  ISE_EVENT_DEFINITION,
  ISE_EVENT_TYPE,
  ISE_EXECUTE_AT,
  ISE_INTERVAL_VALUE,
  ISE_INTERVAL_FIELD,
  ISE_SQL_MODE,
  ISE_STARTS,
  ISE_ENDS,
  ISE_STATUS,
  ISE_ON_COMPLETION,
  ISE_CREATED,
  ISE_LAST_ALTERED,
  ISE_LAST_EXECUTED,
  ISE_EVENT_COMMENT,
  ISE_ORIGINATOR,
  ISE_CLIENT_CS,
  ISE_CONNECTION_CL,
  ISE_DB_CL
};


static const LEX_CSTRING trg_action_time_type_names[]=
{
  { STRING_WITH_LEN("BEFORE") },
  { STRING_WITH_LEN("AFTER") }
};

static const LEX_CSTRING trg_event_type_names[]=
{
  { STRING_WITH_LEN("INSERT") },
  { STRING_WITH_LEN("UPDATE") },
  { STRING_WITH_LEN("DELETE") }
};


LEX_CSTRING DATA_clex_str= { STRING_WITH_LEN("DATA") };
LEX_CSTRING INDEX_clex_str= { STRING_WITH_LEN("INDEX") };

#ifndef NO_EMBEDDED_ACCESS_CHECKS
static const char *grant_names[]={
  "select","insert","update","delete","create","drop","reload","shutdown",
  "process","file","grant","references","index","alter"};

static TYPELIB grant_types = { sizeof(grant_names)/sizeof(char **),
                               "grant_types",
                               grant_names, NULL};
#endif

/* Match the values of enum ha_choice */
static const LEX_CSTRING ha_choice_values[]=
{
  { STRING_WITH_LEN("") },
  { STRING_WITH_LEN("0") },
  { STRING_WITH_LEN("1") }
};

static void store_key_options(THD *, String *, TABLE *, KEY *);

static int show_create_view(THD *thd, TABLE_LIST *table, String *buff);
static int show_create_sequence(THD *thd, TABLE_LIST *table_list,
                                String *packet);

static const LEX_CSTRING *view_algorithm(TABLE_LIST *table);

bool get_lookup_field_values(THD *, COND *, TABLE_LIST *, LOOKUP_FIELD_VALUES *);

/**
  Try to lock a mutex, but give up after a short while to not cause deadlocks

  The loop is short, as the mutex we are trying to lock are mutex the should
  never be locked a long time, just over a few instructions.

  @return 0 ok
  @return 1 error
*/

static bool trylock_short(mysql_mutex_t *mutex)
{
  uint i;
  for (i= 0 ; i < 100 ; i++)
  {
    if (!mysql_mutex_trylock(mutex))
      return 0;
    LF_BACKOFF();
  }
  return 1;
}


/***************************************************************************
** List all table types supported
***************************************************************************/


static bool is_show_command(THD *thd)
{
  return sql_command_flags[thd->lex->sql_command] & CF_STATUS_COMMAND;
}

static int make_version_string(char *buf, int buf_length, uint version)
{
  return (int)my_snprintf(buf, buf_length, "%d.%d", version>>8,version&0xff);
}


static const LEX_CSTRING maturity_name[]={
  { STRING_WITH_LEN("Unknown") },
  { STRING_WITH_LEN("Experimental") },
  { STRING_WITH_LEN("Alpha") },
  { STRING_WITH_LEN("Beta") },
  { STRING_WITH_LEN("Gamma") },
  { STRING_WITH_LEN("Stable") }};


static my_bool show_plugins(THD *thd, plugin_ref plugin,
                            void *arg)
{
  TABLE *table= (TABLE*) arg;
  struct st_maria_plugin *plug= plugin_decl(plugin);
  struct st_plugin_dl *plugin_dl= plugin_dlib(plugin);
  CHARSET_INFO *cs= system_charset_info;
  char version_buf[20];

  restore_record(table, s->default_values);

  table->field[0]->store(plugin_name(plugin)->str,
                         plugin_name(plugin)->length, cs);

  table->field[1]->store(version_buf,
        make_version_string(version_buf, sizeof(version_buf), plug->version),
        cs);

  switch (plugin_state(plugin)) {
  case PLUGIN_IS_DELETED:
    table->field[2]->store(STRING_WITH_LEN("DELETED"), cs);
    break;
  case PLUGIN_IS_UNINITIALIZED:
    table->field[2]->store(STRING_WITH_LEN("INACTIVE"), cs);
    break;
  case PLUGIN_IS_READY:
    table->field[2]->store(STRING_WITH_LEN("ACTIVE"), cs);
    break;
  case PLUGIN_IS_DISABLED:
    table->field[2]->store(STRING_WITH_LEN("DISABLED"), cs);
    break;
  case PLUGIN_IS_DYING:
    table->field[2]->store(STRING_WITH_LEN("INACTIVE"), cs);
    break;
  case PLUGIN_IS_FREED: // filtered in fill_plugins, used in fill_all_plugins
    table->field[2]->store(STRING_WITH_LEN("NOT INSTALLED"), cs);
    break;
  default:
    DBUG_ASSERT(0);
  }

  table->field[3]->store(plugin_type_names[plug->type].str,
                         plugin_type_names[plug->type].length,
                         cs);
  table->field[4]->store(version_buf,
        make_version_string(version_buf, sizeof(version_buf),
                            *(uint *)plug->info), cs);

  if (plugin_dl)
  {
    table->field[5]->store(plugin_dl->dl.str, plugin_dl->dl.length, cs);
    table->field[5]->set_notnull();
    table->field[6]->store(version_buf,
          make_version_string(version_buf, sizeof(version_buf),
                              plugin_dl->mariaversion),
          cs);
    table->field[6]->set_notnull();
  }
  else
  {
    table->field[5]->set_null();
    table->field[6]->set_null();
  }


  if (plug->author)
  {
    table->field[7]->store(plug->author, strlen(plug->author), cs);
    table->field[7]->set_notnull();
  }
  else
    table->field[7]->set_null();

  if (plug->descr)
  {
    table->field[8]->store(plug->descr, strlen(plug->descr), cs);
    table->field[8]->set_notnull();
  }
  else
    table->field[8]->set_null();

  switch (plug->license) {
  case PLUGIN_LICENSE_GPL:
    table->field[9]->store(PLUGIN_LICENSE_GPL_STRING,
                           strlen(PLUGIN_LICENSE_GPL_STRING), cs);
    break;
  case PLUGIN_LICENSE_BSD:
    table->field[9]->store(PLUGIN_LICENSE_BSD_STRING,
                           strlen(PLUGIN_LICENSE_BSD_STRING), cs);
    break;
  default:
    table->field[9]->store(PLUGIN_LICENSE_PROPRIETARY_STRING,
                           strlen(PLUGIN_LICENSE_PROPRIETARY_STRING), cs);
    break;
  }

  table->field[10]->store(
    global_plugin_typelib_names[plugin_load_option(plugin)],
    strlen(global_plugin_typelib_names[plugin_load_option(plugin)]),
    cs);

  if (plug->maturity <= MariaDB_PLUGIN_MATURITY_STABLE)
     table->field[11]->store(maturity_name[plug->maturity].str,
                             maturity_name[plug->maturity].length,
                             cs);
   else
     table->field[11]->store("Unknown", 7, cs);

  if (plug->version_info)
  {
    table->field[12]->store(plug->version_info,
                            strlen(plug->version_info), cs);
    table->field[12]->set_notnull();
  }
  else
    table->field[12]->set_null();

  return schema_table_store_record(thd, table);
}


int fill_plugins(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_plugins");
  TABLE *table= tables->table;

  if (plugin_foreach_with_mask(thd, show_plugins, MYSQL_ANY_PLUGIN,
                               ~PLUGIN_IS_FREED, table))
    DBUG_RETURN(1);

  DBUG_RETURN(0);
}


int fill_all_plugins(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_all_plugins");
  TABLE *table= tables->table;
  LOOKUP_FIELD_VALUES lookup;

  if (get_lookup_field_values(thd, cond, tables, &lookup))
    DBUG_RETURN(0);

  if (lookup.db_value.str && !lookup.db_value.str[0])
    DBUG_RETURN(0); // empty string never matches a valid SONAME

  MY_DIR *dirp= my_dir(opt_plugin_dir, MY_THREAD_SPECIFIC);
  if (!dirp)
  {
    my_error(ER_CANT_READ_DIR, MYF(0), opt_plugin_dir, my_errno);
    DBUG_RETURN(1);
  }

  if (!lookup.db_value.str)
    plugin_dl_foreach(thd, 0, show_plugins, table);

  const char *wstr= lookup.db_value.str, *wend= wstr + lookup.db_value.length;
  for (size_t i=0; i < dirp->number_of_files; i++)
  {
    FILEINFO *file= dirp->dir_entry+i;
    LEX_CSTRING dl= { file->name, strlen(file->name) };
    const char *dlend= dl.str + dl.length;
    const size_t so_ext_len= sizeof(SO_EXT) - 1;

    if (strcasecmp(dlend - so_ext_len, SO_EXT))
      continue;

    if (lookup.db_value.str)
    {
      if (lookup.wild_db_value)
      {
        if (files_charset_info->wildcmp(dl.str, dlend, wstr, wend,
                                        wild_prefix, wild_one, wild_many))
          continue;
      }
      else
      {
        if (files_charset_info->strnncoll(dl.str, dl.length,
                                          lookup.db_value.str, lookup.db_value.length))
          continue;
      }
    }

    plugin_dl_foreach(thd, &dl, show_plugins, table);
    thd->clear_error();
  }

  my_dirend(dirp);
  DBUG_RETURN(0);
}


/***************************************************************************
** List all Authors.
** If you can update it, you get to be in it :)
***************************************************************************/

bool mysqld_show_authors(THD *thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  MEM_ROOT *mem_root= thd->mem_root;
  DBUG_ENTER("mysqld_show_authors");

  field_list.push_back(new (mem_root) Item_empty_string(thd, "Name", 40),
                       mem_root);
  field_list.push_back(new (mem_root) Item_empty_string(thd, "Location", 40),
                       mem_root);
  field_list.push_back(new (mem_root) Item_empty_string(thd, "Comment", 512),
                       mem_root);

  if (protocol->send_result_set_metadata(&field_list,
                                         Protocol::SEND_NUM_ROWS |
                                         Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  show_table_authors_st *authors;
  for (authors= show_table_authors; authors->name; authors++)
  {
    protocol->prepare_for_resend();
    protocol->store(authors->name, strlen(authors->name), system_charset_info);
    protocol->store(authors->location, strlen(authors->location),
                    system_charset_info);
    protocol->store(authors->comment, strlen(authors->comment),
                    system_charset_info);
    if (protocol->write())
      DBUG_RETURN(TRUE);
  }
  my_eof(thd);
  DBUG_RETURN(FALSE);
}


/***************************************************************************
** List all Contributors.
** Please get permission before updating
***************************************************************************/

bool mysqld_show_contributors(THD *thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  MEM_ROOT *mem_root= thd->mem_root;
  DBUG_ENTER("mysqld_show_contributors");

  field_list.push_back(new (mem_root) Item_empty_string(thd, "Name", 40),
                       mem_root);
  field_list.push_back(new (mem_root) Item_empty_string(thd, "Location", 40),
                       mem_root);
  field_list.push_back(new (mem_root) Item_empty_string(thd, "Comment",  512),
                       mem_root);

  if (protocol->send_result_set_metadata(&field_list,
                                         Protocol::SEND_NUM_ROWS |
                                         Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  show_table_contributors_st *contributors;
  for (contributors= show_table_contributors; contributors->name; contributors++)
  {
    protocol->prepare_for_resend();
    protocol->store(contributors->name, strlen(contributors->name),
                    system_charset_info);
    protocol->store(contributors->location, strlen(contributors->location),
                    system_charset_info);
    protocol->store(contributors->comment, strlen(contributors->comment),
                    system_charset_info);
    if (protocol->write())
      DBUG_RETURN(TRUE);
  }
  my_eof(thd);
  DBUG_RETURN(FALSE);
}


/***************************************************************************
 List all privileges supported
***************************************************************************/

struct show_privileges_st {
  const char *privilege;
  const char *context;
  const char *comment;
};

static struct show_privileges_st sys_privileges[]=
{
  {"Alter", "Tables",  "To alter the table"},
  {"Alter routine", "Functions,Procedures",  "To alter or drop stored functions/procedures"},
  {"Create", "Databases,Tables,Indexes",  "To create new databases and tables"},
  {"Create routine","Databases","To use CREATE FUNCTION/PROCEDURE"},
  {"Create temporary tables","Databases","To use CREATE TEMPORARY TABLE"},
  {"Create view", "Tables",  "To create new views"},
  {"Create user", "Server Admin",  "To create new users"},
  {"Delete", "Tables",  "To delete existing rows"},
  {"Delete history", "Tables", "To delete versioning table historical rows"},
  {"Drop", "Databases,Tables", "To drop databases, tables, and views"},
#ifdef HAVE_EVENT_SCHEDULER
  {"Event","Server Admin","To create, alter, drop and execute events"},
#endif
  {"Execute", "Functions,Procedures", "To execute stored routines"},
  {"File", "File access on server",   "To read and write files on the server"},
  {"Grant option",  "Databases,Tables,Functions,Procedures", "To give to other users those privileges you possess"},
  {"Index", "Tables",  "To create or drop indexes"},
  {"Insert", "Tables",  "To insert data into tables"},
  {"Lock tables","Databases","To use LOCK TABLES (together with SELECT privilege)"},
  {"Process", "Server Admin", "To view the plain text of currently executing queries"},
  {"Proxy", "Server Admin", "To make proxy user possible"},
  {"References", "Databases,Tables", "To have references on tables"},
  {"Reload", "Server Admin", "To reload or refresh tables, logs and privileges"},
  {"Binlog admin", "Server", "To purge binary logs"},
  {"Binlog monitor", "Server", "To use SHOW BINLOG STATUS and SHOW BINARY LOG"},
  {"Binlog replay", "Server", "To use BINLOG (generated by mariadb-binlog)"},
  {"Replication master admin", "Server", "To monitor connected slaves"},
  {"Replication slave admin", "Server", "To start/stop slave and apply binlog events"},
  {"Slave monitor", "Server", "To use SHOW SLAVE STATUS and SHOW RELAYLOG EVENTS"},
  {"Replication slave","Server Admin","To read binary log events from the master"},
  {"Select", "Tables",  "To retrieve rows from table"},
  {"Show databases","Server Admin","To see all databases with SHOW DATABASES"},
  {"Show view","Tables","To see views with SHOW CREATE VIEW"},
  {"Shutdown","Server Admin", "To shut down the server"},
  {"Super","Server Admin","To use KILL thread, SET GLOBAL, CHANGE MASTER, etc."},
  {"Trigger","Tables", "To use triggers"},
  {"Create tablespace", "Server Admin", "To create/alter/drop tablespaces"},
  {"Update", "Tables",  "To update existing rows"},
  {"Set user","Server", "To create views and stored routines with a different definer"},
  {"Federated admin", "Server", "To execute the CREATE SERVER, ALTER SERVER, DROP SERVER statements"},
  {"Connection admin", "Server", "To bypass connection limits and kill other users' connections"},
  {"Read_only admin", "Server", "To perform write operations even if @@read_only=ON"},
  {"Usage","Server Admin","No privileges - allow connect only"},
  {NullS, NullS, NullS}
};

bool mysqld_show_privileges(THD *thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  MEM_ROOT *mem_root= thd->mem_root;
  DBUG_ENTER("mysqld_show_privileges");

  field_list.push_back(new (mem_root) Item_empty_string(thd, "Privilege", 10),
                       mem_root);
  field_list.push_back(new (mem_root) Item_empty_string(thd, "Context", 15),
                       mem_root);
  field_list.push_back(new (mem_root) Item_empty_string(thd, "Comment",
                                                        NAME_CHAR_LEN),
                       mem_root);

  if (protocol->send_result_set_metadata(&field_list,
                                         Protocol::SEND_NUM_ROWS |
                                         Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  show_privileges_st *privilege= sys_privileges;
  for (privilege= sys_privileges; privilege->privilege ; privilege++)
  {
    protocol->prepare_for_resend();
    protocol->store(privilege->privilege, strlen(privilege->privilege),
                    system_charset_info);
    protocol->store(privilege->context, strlen(privilege->context),
                    system_charset_info);
    protocol->store(privilege->comment, strlen(privilege->comment),
                    system_charset_info);
    if (protocol->write())
      DBUG_RETURN(TRUE);
  }
  my_eof(thd);
  DBUG_RETURN(FALSE);
}


/** Hash of LEX_STRINGs used to search for ignored db directories. */
static HASH ignore_db_dirs_hash;

/** 
  An array of LEX_STRING pointers to collect the options at 
  option parsing time.
*/
static DYNAMIC_ARRAY ignore_db_dirs_array;

/**
  A value for the read only system variable to show a list of
  ignored directories.
*/
char *opt_ignore_db_dirs= NULL;

/**
  This flag is ON if:
        - the list of ignored directories is not empty

        - and some of the ignored directory names
        need no tablename-to-filename conversion.
        Otherwise, if the name of the directory contains
        unconditional characters like '+' or '.', they
        never can match the database directory name. So the
        db_name_is_in_ignore_db_dirs_list() can just return at once.
*/
static bool skip_ignored_dir_check= TRUE;

/**
  Sets up the data structures for collection of directories at option
  processing time.
  We need to collect the directories in an array first, because
  we need the character sets initialized before setting up the hash.

  @return state
  @retval TRUE  failed
  @retval FALSE success
*/

bool
ignore_db_dirs_init()
{
  return my_init_dynamic_array(key_memory_ignored_db, &ignore_db_dirs_array,
                               sizeof(LEX_STRING *), 0, 0, MYF(0));
}


/**
  Retrieves the key (the string itself) from the LEX_STRING hash members.

  Needed by hash_init().

  @param     data         the data element from the hash
  @param out len_ret      Placeholder to return the length of the key
  @param                  unused
  @return                 a pointer to the key
*/

static uchar *
db_dirs_hash_get_key(const uchar *data, size_t *len_ret,
                     my_bool __attribute__((unused)))
{
  LEX_CSTRING *e= (LEX_CSTRING *) data;

  *len_ret= e->length;
  return (uchar *) e->str;
}


/**
  Wrap a directory name into a LEX_STRING and push it to the array.

  Called at option processing time for each --ignore-db-dir option.

  @param    path  the name of the directory to push
  @return state
  @retval TRUE  failed
  @retval FALSE success
*/

bool push_ignored_db_dir(const char *path)
{
  LEX_CSTRING *new_elt;
  char *new_elt_buffer;
  size_t path_len= strlen(path);

  if (!path_len || path_len >= FN_REFLEN)
    return true;

  // No need to normalize, it's only a directory name, not a path.
  if (!my_multi_malloc(key_memory_ignored_db, MYF(0),
                       &new_elt, sizeof(LEX_STRING),
                       &new_elt_buffer, path_len + 1,
                       NullS))
    return true;
  new_elt->str= new_elt_buffer;
  memcpy(new_elt_buffer, path, path_len);
  new_elt_buffer[path_len]= 0;
  new_elt->length= path_len;
  return insert_dynamic(&ignore_db_dirs_array, (uchar*) &new_elt);
}


/**
  Clean up the directory ignore options accumulated so far.

  Called at option processing time for each --ignore-db-dir option
  with an empty argument.
*/

void
ignore_db_dirs_reset()
{
  LEX_CSTRING **elt;
  while (NULL!= (elt= (LEX_CSTRING **) pop_dynamic(&ignore_db_dirs_array)))
    if (elt && *elt)
      my_free(*elt);
}


/**
  Free the directory ignore option variables.

  Called at server shutdown.
*/

void
ignore_db_dirs_free()
{
  if (opt_ignore_db_dirs)
  {
    my_free(opt_ignore_db_dirs);
    opt_ignore_db_dirs= NULL;
  }
  ignore_db_dirs_reset();
  delete_dynamic(&ignore_db_dirs_array);
  my_hash_free(&ignore_db_dirs_hash);
}


/**
  Initialize the ignore db directories hash and status variable from
  the options collected in the array.

  Called when option processing is over and the server's in-memory 
  structures are fully initialized.

  @return state
  @retval TRUE  failed
  @retval FALSE success
*/

static void dispose_db_dir(void *ptr)
{
  my_free(ptr);
}


/*
  Append an element into @@ignore_db_dirs

  This is a function to be called after regular option processing has been
  finalized.
*/

void ignore_db_dirs_append(const char *dirname_arg)
{
  char *new_entry_buf;
  LEX_STRING *new_entry;
  size_t len= strlen(dirname_arg);

  if (!my_multi_malloc(PSI_INSTRUMENT_ME, MYF(0),
                       &new_entry, sizeof(LEX_STRING),
                       &new_entry_buf, len + 1,
                       NullS))
    return;

  memcpy(new_entry_buf, dirname_arg, len+1);
  new_entry->str = new_entry_buf;
  new_entry->length= len;

  if (my_hash_insert(&ignore_db_dirs_hash, (uchar *)new_entry))
  {
    // Either the name is already there or out-of-memory.
    my_free(new_entry);
    return;
  }

  // Append the name to the option string.
  size_t curlen= strlen(opt_ignore_db_dirs);
  // Add one for comma and one for \0.
  size_t newlen= curlen + len + 1 + 1;
  char *new_db_dirs;
  if (!(new_db_dirs= (char*)my_malloc(PSI_INSTRUMENT_ME, newlen, MYF(0))))
  {
    // This is not a critical condition
    return;
  }

  memcpy(new_db_dirs, opt_ignore_db_dirs, curlen);
  if (curlen != 0)
    new_db_dirs[curlen]=',';
  memcpy(new_db_dirs + (curlen + ((curlen!=0)?1:0)), dirname_arg, len+1);

  if (opt_ignore_db_dirs)
    my_free(opt_ignore_db_dirs);
  opt_ignore_db_dirs= new_db_dirs;
}

bool
ignore_db_dirs_process_additions()
{
  ulong i;
  size_t len;
  char *ptr;
  LEX_CSTRING *dir;

  skip_ignored_dir_check= TRUE;

  if (my_hash_init(key_memory_ignored_db, &ignore_db_dirs_hash,
                   lower_case_table_names ?  character_set_filesystem :
                   &my_charset_bin, 0, 0, 0, db_dirs_hash_get_key,
                   dispose_db_dir, HASH_UNIQUE))
    return true;

  /* len starts from 1 because of the terminating zero. */
  len= 1;
  for (i= 0; i < ignore_db_dirs_array.elements; i++)
  {
    get_dynamic(&ignore_db_dirs_array, (uchar *) &dir, i);
    len+= dir->length + 1;                      // +1 for the comma
    if (skip_ignored_dir_check)
    {
      char buff[FN_REFLEN];
      (void) tablename_to_filename(dir->str, buff, sizeof(buff));
      skip_ignored_dir_check= strcmp(dir->str, buff) != 0;
    }
  }

  /* No delimiter for the last directory. */
  if (len > 1)
    len--;

  /* +1 the terminating zero */
  ptr= opt_ignore_db_dirs= (char *) my_malloc(key_memory_ignored_db, len + 1,
                                              MYF(0));
  if (!ptr)
    return true;

  /* Make sure we have an empty string to start with. */
  *ptr= 0;

  for (i= 0; i < ignore_db_dirs_array.elements; i++)
  {
    get_dynamic(&ignore_db_dirs_array, (uchar *) &dir, i);
    if (my_hash_insert(&ignore_db_dirs_hash, (uchar *)dir))
    {
      /* ignore duplicates from the config file */
      if (my_hash_search(&ignore_db_dirs_hash, (uchar *)dir->str, dir->length))
      {
        sql_print_warning("Duplicate ignore-db-dir directory name '%.*s' "
                          "found in the config file(s). Ignoring the duplicate.",
                          (int) dir->length, dir->str);
        my_free(dir);
        goto continue_loop;
      }

      return true;
    }
    ptr= strnmov(ptr, dir->str, dir->length);
    *(ptr++)= ',';

continue_loop:
    /*
      Set the transferred array element to NULL to avoid double free
      in case of error.
    */
    dir= NULL;
    set_dynamic(&ignore_db_dirs_array, (uchar *) &dir, i);
  }

  if (ptr > opt_ignore_db_dirs)
  {
    ptr--;
    DBUG_ASSERT(*ptr == ',');
  }

  /* make sure the string is terminated */
  DBUG_ASSERT(ptr - opt_ignore_db_dirs <= (ptrdiff_t) len);
  *ptr= 0;

  /* 
    It's OK to empty the array here as the allocated elements are
    referenced through the hash now.
  */
  reset_dynamic(&ignore_db_dirs_array);

  return false;
}


/**
  Check if a directory name is in the hash of ignored directories.

  @return search result
  @retval TRUE  found
  @retval FALSE not found
*/

static inline bool
is_in_ignore_db_dirs_list(const char *directory)
{
  return ignore_db_dirs_hash.records &&
    NULL != my_hash_search(&ignore_db_dirs_hash, (const uchar *) directory, 
                           strlen(directory));
}


/**
  Check if a database name is in the hash of ignored directories.

  @return search result
  @retval TRUE  found
  @retval FALSE not found
*/

bool
db_name_is_in_ignore_db_dirs_list(const char *directory)
{
  char buff[FN_REFLEN];
  uint buff_len;

  if (skip_ignored_dir_check)
    return 0;

  buff_len= tablename_to_filename(directory, buff, sizeof(buff));

  return my_hash_search(&ignore_db_dirs_hash, (uchar *) buff, buff_len)!=NULL;
}

enum find_files_result {
  FIND_FILES_OK,
  FIND_FILES_OOM,
  FIND_FILES_DIR
};

/*
  find_files() - find files in a given directory.

  SYNOPSIS
    find_files()
    thd                 thread handler
    files               put found files in this list
    db                  database name to search tables in
                        or NULL to search for databases
    path                path to database
    wild                filter for found files

  RETURN
    FIND_FILES_OK       success
    FIND_FILES_OOM      out of memory error
    FIND_FILES_DIR      no such directory, or directory can't be read
*/


static find_files_result
find_files(THD *thd, Dynamic_array<LEX_CSTRING*> *files, LEX_CSTRING *db,
           const char *path, const LEX_CSTRING *wild)
{
  MY_DIR *dirp;
  Discovered_table_list tl(thd, files, wild);
  DBUG_ENTER("find_files");

  if (!(dirp = my_dir(path, MY_THREAD_SPECIFIC | (db ? 0 : MY_WANT_STAT))))
  {
    if (my_errno == ENOENT)
      my_error(ER_BAD_DB_ERROR, MYF(0), db->str);
    else
      my_error(ER_CANT_READ_DIR, MYF(0), path, my_errno);
    DBUG_RETURN(FIND_FILES_DIR);
  }

  if (!db)                                           /* Return databases */
  {
    for (size_t i=0; i < dirp->number_of_files; i++)
    {
      FILEINFO *file= dirp->dir_entry+i;
#ifdef USE_SYMDIR
      char *ext;
      char buff[FN_REFLEN];
      if (my_use_symdir && !strcmp(ext=fn_ext(file->name), ".sym"))
      {
        /* Only show the sym file if it points to a directory */
        char *end;
        *ext=0;                                 /* Remove extension */
        unpack_dirname(buff, file->name);
        end= strend(buff);
        if (end != buff && end[-1] == FN_LIBCHAR)
          end[-1]= 0;				// Remove end FN_LIBCHAR
        if (!mysql_file_stat(key_file_misc, buff, file->mystat, MYF(0)))
               continue;
       }
#endif
      if (!MY_S_ISDIR(file->mystat->st_mode))
        continue;

      if (is_in_ignore_db_dirs_list(file->name))
        continue;

      if (tl.add_file(file->name))
        goto err;
    }
  }
  else
  {
    if (ha_discover_table_names(thd, db, dirp, &tl, false))
      goto err;
  }
  if (is_show_command(thd))
    tl.sort();
#ifndef DBUG_OFF
  else
  {
    /*
      sort_desc() is used to find easier unstable mtr tests that query
      INFORMATION_SCHEMA.{SCHEMATA|TABLES} without a proper ORDER BY.
      This can be removed in some release after 10.3 (e.g. in 10.4).
    */
    tl.sort_desc();
  }
#endif

  DBUG_PRINT("info",("found: %zu files", files->elements()));
  my_dirend(dirp);

  DBUG_RETURN(FIND_FILES_OK);

err:
  my_dirend(dirp);
  DBUG_RETURN(FIND_FILES_OOM);
}


/**
   An Internal_error_handler that suppresses errors regarding views'
   underlying tables that occur during privilege checking within SHOW CREATE
   VIEW commands. This happens in the cases when

   - A view's underlying table (e.g. referenced in its SELECT list) does not
     exist. There should not be an error as no attempt was made to access it
     per se.

   - Access is denied for some table, column, function or stored procedure
     such as mentioned above. This error gets raised automatically, since we
     can't untangle its access checking from that of the view itself.
 */
class Show_create_error_handler : public Internal_error_handler {
  
  TABLE_LIST *m_top_view;
  bool m_handling;
  Security_context *m_sctx;

  char m_view_access_denied_message[MYSQL_ERRMSG_SIZE];
  char *m_view_access_denied_message_ptr;

public:

  /**
     Creates a new Show_create_error_handler for the particular security
     context and view. 

     @thd Thread context, used for security context information if needed.
     @top_view The view. We do not verify at this point that top_view is in
     fact a view since, alas, these things do not stay constant.
  */
  explicit Show_create_error_handler(THD *thd, TABLE_LIST *top_view) : 
    m_top_view(top_view), m_handling(FALSE),
    m_view_access_denied_message_ptr(NULL) 
  {
    
    m_sctx= MY_TEST(m_top_view->security_ctx) ?
      m_top_view->security_ctx : thd->security_ctx;
  }

  /**
     Lazy instantiation of 'view access denied' message. The purpose of the
     Show_create_error_handler is to hide details of underlying tables for
     which we have no privileges behind ER_VIEW_INVALID messages. But this
     obviously does not apply if we lack privileges on the view itself.
     Unfortunately the information about for which table privilege checking
     failed is not available at this point. The only way for us to check is by
     reconstructing the actual error message and see if it's the same.
  */
  char* get_view_access_denied_message(THD *thd) 
  {
    if (!m_view_access_denied_message_ptr)
    {
      m_view_access_denied_message_ptr= m_view_access_denied_message;
      my_snprintf(m_view_access_denied_message, MYSQL_ERRMSG_SIZE,
                  ER_THD(thd, ER_TABLEACCESS_DENIED_ERROR), "SHOW VIEW",
                  m_sctx->priv_user,
                  m_sctx->host_or_ip, m_top_view->get_table_name());
    }
    return m_view_access_denied_message_ptr;
  }

  bool handle_condition(THD *thd, uint sql_errno, const char * /* sqlstate */,
                        Sql_condition::enum_warning_level *level,
                        const char *message, Sql_condition ** /* cond_hdl */)
  {
    /*
       The handler does not handle the errors raised by itself.
       At this point we know if top_view is really a view.
    */
    if (m_handling || !m_top_view->view)
      return FALSE;

    m_handling= TRUE;

    bool is_handled;

    switch (sql_errno)
    {
    case ER_TABLEACCESS_DENIED_ERROR:
      if (!strcmp(get_view_access_denied_message(thd), message))
      {
        /* Access to top view is not granted, don't interfere. */
        is_handled= FALSE;
        break;
      }
      /* fall through */
    case ER_COLUMNACCESS_DENIED_ERROR:
    case ER_VIEW_NO_EXPLAIN: /* Error was anonymized, ignore all the same. */
    case ER_PROCACCESS_DENIED_ERROR:
      is_handled= TRUE;
      break;

    case ER_BAD_FIELD_ERROR:
    case ER_SP_DOES_NOT_EXIST:
    case ER_NO_SUCH_TABLE:
    case ER_NO_SUCH_TABLE_IN_ENGINE:
      /* Established behavior: warn if underlying tables, columns, or functions
         are missing. */
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, 
                          ER_VIEW_INVALID,
                          ER_THD(thd, ER_VIEW_INVALID),
                          m_top_view->get_db_name(),
                          m_top_view->get_table_name());
      is_handled= TRUE;
      break;

    default:
      is_handled= FALSE;
    }

    m_handling= FALSE;
    return is_handled;
  }
};


/*
  Return metadata for CREATE command for table or view

  @param thd	     Thread handler
  @param table_list  Table / view
  @param field_list  resulting list of fields
  @param buffer      resulting CREATE statement

  @return
  @retval 0      OK
  @retval 1      Error

*/

bool
mysqld_show_create_get_fields(THD *thd, TABLE_LIST *table_list,
                              List<Item> *field_list, String *buffer)
{
  bool error= TRUE;
  LEX *lex= thd->lex;
  MEM_ROOT *mem_root= thd->mem_root;
  DBUG_ENTER("mysqld_show_create_get_fields");
  DBUG_PRINT("enter",("db: %s  table: %s",table_list->db.str,
                      table_list->table_name.str));

  if (lex->table_type == TABLE_TYPE_VIEW)
  {
    if (check_table_access(thd, SELECT_ACL, table_list, FALSE, 1, FALSE))
    {
      DBUG_PRINT("debug", ("check_table_access failed"));
      my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
              "SHOW", thd->security_ctx->priv_user,
              thd->security_ctx->host_or_ip, table_list->alias.str);
      goto exit;
    }
    DBUG_PRINT("debug", ("check_table_access succeeded"));

    /* Ignore temporary tables if this is "SHOW CREATE VIEW" */
    table_list->open_type= OT_BASE_ONLY;
  }
  else
  {
    /*
      Temporary tables should be opened for SHOW CREATE TABLE, but not
      for SHOW CREATE VIEW.
    */
    if (thd->open_temporary_tables(table_list))
      goto exit;

    /*
      The fact that check_some_access() returned FALSE does not mean that
      access is granted. We need to check if table_list->grant.privilege
      contains any table-specific privilege.
    */
    DBUG_PRINT("debug", ("table_list->grant.privilege: %llx",
                         (longlong) (table_list->grant.privilege)));
    if (check_some_access(thd, SHOW_CREATE_TABLE_ACLS, table_list) ||
        (table_list->grant.privilege & SHOW_CREATE_TABLE_ACLS) == NO_ACL)
    {
      my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
              "SHOW", thd->security_ctx->priv_user,
              thd->security_ctx->host_or_ip, table_list->alias.str);
      goto exit;
    }
  }
  /* Access is granted. Execute the command.  */

  /* We want to preserve the tree for views. */
  lex->context_analysis_only|= CONTEXT_ANALYSIS_ONLY_VIEW;

  {
    /*
      Use open_tables() directly rather than
      open_normal_and_derived_tables().  This ensures that
      close_thread_tables() is not called if open tables fails and the
      error is ignored. This allows us to handle broken views nicely.
    */
    uint counter;
    Show_create_error_handler view_error_suppressor(thd, table_list);
    thd->push_internal_handler(&view_error_suppressor);
    bool open_error=
      open_tables(thd, &table_list, &counter,
                  MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL) ||
                  mysql_handle_derived(lex, DT_INIT | DT_PREPARE);
    thd->pop_internal_handler();
    if (unlikely(open_error && (thd->killed || thd->is_error())))
      goto exit;
  }

  /* TODO: add environment variables show when it become possible */
  if (lex->table_type == TABLE_TYPE_VIEW && !table_list->view)
  {
    my_error(ER_WRONG_OBJECT, MYF(0),
             table_list->db.str, table_list->table_name.str, "VIEW");
    goto exit;
  }
  else if (lex->table_type == TABLE_TYPE_SEQUENCE &&
           (!table_list->table ||
            table_list->table->s->table_type != TABLE_TYPE_SEQUENCE))
  {
    my_error(ER_NOT_SEQUENCE, MYF(0),
             table_list->db.str, table_list->table_name.str);
    goto exit;
  }

  buffer->length(0);

  if (table_list->view)
    buffer->set_charset(table_list->view_creation_ctx->get_client_cs());

  if ((table_list->view ?
       show_create_view(thd, table_list, buffer) :
       lex->table_type == TABLE_TYPE_SEQUENCE ?
       show_create_sequence(thd, table_list, buffer) :
       show_create_table(thd, table_list, buffer, NULL, WITHOUT_DB_NAME)))
    goto exit;

  if (table_list->view)
  {
    field_list->push_back(new (mem_root)
                         Item_empty_string(thd, "View", NAME_CHAR_LEN),
                         mem_root);
    field_list->push_back(new (mem_root)
                         Item_empty_string(thd, "Create View",
                                           MY_MAX(buffer->length(),1024)),
                         mem_root);
    field_list->push_back(new (mem_root)
                         Item_empty_string(thd, "character_set_client",
                                           MY_CS_NAME_SIZE),
                         mem_root);
    field_list->push_back(new (mem_root)
                         Item_empty_string(thd, "collation_connection",
                                           MY_CS_NAME_SIZE),
                         mem_root);
  }
  else
  {
    field_list->push_back(new (mem_root)
                         Item_empty_string(thd, "Table", NAME_CHAR_LEN),
                         mem_root);
    // 1024 is for not to confuse old clients
    field_list->push_back(new (mem_root)
                         Item_empty_string(thd, "Create Table",
                                           MY_MAX(buffer->length(),1024)),
                         mem_root);
  }
  error= FALSE;

exit:
  DBUG_RETURN(error);
}


/*
  Return CREATE command for table or view

  @param thd	     Thread handler
  @param table_list  Table / view

  @return
  @retval 0      OK
  @retval 1      Error

  @notes
  table_list->db and table_list->table_name are kept unchanged to
  not cause problems with SP.
*/

bool
mysqld_show_create(THD *thd, TABLE_LIST *table_list)
{
  Protocol *protocol= thd->protocol;
  char buff[2048];
  String buffer(buff, sizeof(buff), system_charset_info);
  List<Item> field_list;
  bool error= TRUE;
  DBUG_ENTER("mysqld_show_create");
  DBUG_PRINT("enter",("db: %s  table: %s",table_list->db.str,
                      table_list->table_name.str));

  /*
    Metadata locks taken during SHOW CREATE should be released when
    the statmement completes as it is an information statement.
  */
  MDL_savepoint mdl_savepoint= thd->mdl_context.mdl_savepoint();

  TABLE_LIST archive;

  if (mysqld_show_create_get_fields(thd, table_list, &field_list, &buffer))
    goto exit;

  if (protocol->send_result_set_metadata(&field_list,
                                         Protocol::SEND_NUM_ROWS |
                                         Protocol::SEND_EOF))
    goto exit;

  protocol->prepare_for_resend();
  if (table_list->view)
    protocol->store(&table_list->view_name, system_charset_info);
  else
  {
    if (table_list->schema_table)
      protocol->store(table_list->schema_table->table_name,
                      strlen(table_list->schema_table->table_name),
                      system_charset_info);
    else
      protocol->store(table_list->table->alias.ptr(),
                      table_list->table->alias.length(),
                      system_charset_info);
  }

  if (table_list->view)
  {
    buffer.set_charset(table_list->view_creation_ctx->get_client_cs());
    protocol->store(&buffer);

    protocol->store(&table_list->view_creation_ctx->get_client_cs()->cs_name,
                    system_charset_info);

    protocol->store(&table_list->view_creation_ctx->get_connection_cl()->
                    coll_name, system_charset_info);
  }
  else
    protocol->store(&buffer);

  if (protocol->write())
    goto exit;

  error= FALSE;
  my_eof(thd);

exit:
  close_thread_tables(thd);
  /* Release any metadata locks taken during SHOW CREATE. */
  thd->mdl_context.rollback_to_savepoint(mdl_savepoint);
  DBUG_RETURN(error);
}


void mysqld_show_create_db_get_fields(THD *thd, List<Item> *field_list)
{
  MEM_ROOT *mem_root= thd->mem_root;
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Database", NAME_CHAR_LEN),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Create Database", 1024),
                        mem_root);
}


bool mysqld_show_create_db(THD *thd, LEX_CSTRING *dbname,
                           LEX_CSTRING *orig_dbname,
                           const DDL_options_st &options)
{
  char buff[2048+DATABASE_COMMENT_MAXLEN];
  String buffer(buff, sizeof(buff), system_charset_info);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *sctx= thd->security_ctx;
  privilege_t db_access(NO_ACL);
#endif
  Schema_specification_st create;
  Protocol *protocol=thd->protocol;
  List<Item> field_list;
  DBUG_ENTER("mysql_show_create_db");

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (test_all_bits(sctx->master_access, DB_ACLS))
    db_access=DB_ACLS;
  else
  {
    db_access= acl_get(sctx->host, sctx->ip, sctx->priv_user, dbname->str, 0) |
               sctx->master_access;
    if (sctx->priv_role[0])
      db_access|= acl_get("", "", sctx->priv_role, dbname->str, 0);
  }

  if (!(db_access & DB_ACLS) && check_grant_db(thd,dbname->str))
  {
    status_var_increment(thd->status_var.access_denied_errors);
    my_error(ER_DBACCESS_DENIED_ERROR, MYF(0),
             sctx->priv_user, sctx->host_or_ip, dbname->str);
    general_log_print(thd,COM_INIT_DB,ER_THD(thd, ER_DBACCESS_DENIED_ERROR),
                      sctx->priv_user, sctx->host_or_ip, orig_dbname->str);
    DBUG_RETURN(TRUE);
  }
#endif
  if (is_infoschema_db(dbname))
  {
    *dbname= INFORMATION_SCHEMA_NAME;
    create.default_table_charset= system_charset_info;
    create.schema_comment= NULL;
  }
  else
  {
    if (check_db_dir_existence(dbname->str))
    {
      my_error(ER_BAD_DB_ERROR, MYF(0), dbname->str);
      DBUG_RETURN(TRUE);
    }

    load_db_opt_by_name(thd, dbname->str, &create);
  }

  mysqld_show_create_db_get_fields(thd, &field_list);

  if (protocol->send_result_set_metadata(&field_list,
                                         Protocol::SEND_NUM_ROWS |
                                         Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  protocol->prepare_for_resend();
  protocol->store(orig_dbname->str, orig_dbname->length, system_charset_info);
  buffer.length(0);
  buffer.append(STRING_WITH_LEN("CREATE DATABASE "));
  if (options.if_not_exists())
    buffer.append(STRING_WITH_LEN("/*!32312 IF NOT EXISTS*/ "));
  append_identifier(thd, &buffer, dbname);

  if (create.default_table_charset)
  {
    buffer.append(STRING_WITH_LEN(" /*!40100"));
    buffer.append(STRING_WITH_LEN(" DEFAULT CHARACTER SET "));
    buffer.append(create.default_table_charset->cs_name);
    if (!(create.default_table_charset->state & MY_CS_PRIMARY))
    {
      buffer.append(STRING_WITH_LEN(" COLLATE "));
      buffer.append(create.default_table_charset->coll_name);
    }
    buffer.append(STRING_WITH_LEN(" */"));
  }

  if (create.schema_comment)
  {
    buffer.append(STRING_WITH_LEN(" COMMENT "));
    append_unescaped(&buffer, create.schema_comment->str,
                     create.schema_comment->length);
  }
  protocol->store(buffer.ptr(), buffer.length(), buffer.charset());

  if (protocol->write())
    DBUG_RETURN(TRUE);
  my_eof(thd);
  DBUG_RETURN(FALSE);
}



/****************************************************************************
  Return only fields for API mysql_list_fields
  Use "show table wildcard" in mysql instead of this
****************************************************************************/

void
mysqld_list_fields(THD *thd, TABLE_LIST *table_list, const char *wild)
{
  TABLE *table;
  DBUG_ENTER("mysqld_list_fields");
  DBUG_PRINT("enter",("table: %s", table_list->table_name.str));

  if (open_normal_and_derived_tables(thd, table_list,
                                     MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL,
                                     DT_INIT | DT_PREPARE))
    DBUG_VOID_RETURN;
  table= table_list->table;

  List<Field> field_list;

  Field **ptr,*field;
  for (ptr=table->field ; (field= *ptr); ptr++)
  {
    if (!wild || !wild[0] ||
        !wild_case_compare(system_charset_info, field->field_name.str,wild))
      field_list.push_back(field);
  }
  restore_record(table, s->default_values);              // Get empty record
  table->use_all_columns();
  if (thd->protocol->send_list_fields(&field_list, table_list))
    DBUG_VOID_RETURN;
  my_eof(thd);
  DBUG_VOID_RETURN;
}

/*
  Go through all character combinations and ensure that sql_lex.cc can
  parse it as an identifier.

  SYNOPSIS
  require_quotes()
  name			attribute name
  name_length		length of name

  RETURN
    #	Pointer to conflicting character
    0	No conflicting character
*/

static const char *require_quotes(const char *name, uint name_length)
{
  bool pure_digit= TRUE;
  const char *end= name + name_length;

  for (; name < end ; name++)
  {
    uchar chr= (uchar) *name;
    int length= system_charset_info->charlen(name, end);
    if (length == 1 && !system_charset_info->ident_map[chr])
      return name;
    if (length == 1 && (chr < '0' || chr > '9'))
      pure_digit= FALSE;
  }
  if (pure_digit)
    return name;
  return 0;
}


/**
  Convert and quote the given identifier if needed and append it to the
  target string. If the given identifier is empty, it will be quoted.
  @thd                         thread handler
  @packet                      target string
  @name                        the identifier to be appended
  @length                      length of the appending identifier

  @return
    0             success
    1             error
*/

bool
append_identifier(THD *thd, String *packet, const char *name, size_t length)
{
  const char *name_end;
  char quote_char;
  int q= get_quote_char_for_identifier(thd, name, length);

  if (q == EOF)
    return packet->append(name, length, packet->charset());

  /*
    The identifier must be quoted as it includes a quote character or
    it's a keyword
  */

  /*
    Special code for swe7. It encodes the letter "E WITH ACUTE" on
    the position 0x60, where backtick normally resides.
    In swe7 we cannot append 0x60 using system_charset_info,
    because it cannot be converted to swe7 and will be replaced to
    question mark '?'. Use &my_charset_bin to avoid this.
    It will prevent conversion and will append the backtick as is.
  */
  CHARSET_INFO *quote_charset= q == 0x60 &&
                               (packet->charset()->state & MY_CS_NONASCII) &&
                               packet->charset()->mbmaxlen == 1 ?
                               &my_charset_bin : system_charset_info;

  (void) packet->reserve(length*2 + 2);
  quote_char= (char) q;
  if (packet->append(&quote_char, 1, quote_charset))
    return true;

  for (name_end= name+length ; name < name_end ; )
  {
    uchar chr= (uchar) *name;
    int char_length= system_charset_info->charlen(name, name_end);
    /*
      charlen can return 0 and negative numbers on a wrong multibyte
      sequence. It is possible when upgrading from 4.0,
      and identifier contains some accented characters.
      The manual says it does not work. So we'll just
      change char_length to 1 not to hang in the endless loop.
    */
    if (char_length <= 0)
      char_length= 1;
    if (char_length == 1 && chr == (uchar) quote_char &&
        packet->append(&quote_char, 1, quote_charset))
      return true;
    if (packet->append(name, char_length, system_charset_info))
      return true;
    name+= char_length;
  }
  return packet->append(&quote_char, 1, quote_charset);
}


/*
  Get the quote character for displaying an identifier.

  SYNOPSIS
    get_quote_char_for_identifier()
    thd		Thread handler
    name	name to quote
    length	length of name

  IMPLEMENTATION
    Force quoting in the following cases:
      - name is empty (for one, it is possible when we use this function for
        quoting user and host names for DEFINER clause);
      - name is a keyword;
      - name includes a special character;
    Otherwise identifier is quoted only if the option OPTION_QUOTE_SHOW_CREATE
    is set.

  RETURN
    EOF	  No quote character is needed
    #	  Quote character
*/

int get_quote_char_for_identifier(THD *thd, const char *name, size_t length)
{
  if (length &&
      !is_keyword(name,(uint)length) &&
      !require_quotes(name, (uint)length) &&
      !(thd->variables.option_bits & OPTION_QUOTE_SHOW_CREATE))
    return EOF;
  if (thd->variables.sql_mode & MODE_ANSI_QUOTES)
    return '"';
  return '`';
}


/* Append directory name (if exists) to CREATE INFO */

static void append_directory(THD *thd, String *packet, LEX_CSTRING *dir_type,
			     const char *filename)
{
  if (filename && !(thd->variables.sql_mode & MODE_NO_DIR_IN_CREATE))
  {
    size_t length= dirname_length(filename);
    packet->append(' ');
    packet->append(dir_type);
    packet->append(STRING_WITH_LEN(" DIRECTORY='"));
#ifdef _WIN32
    /* Convert \ to / to be able to create table on unix */
    char *winfilename= (char*) thd->memdup(filename, length);
    char *pos, *end;
    for (pos= winfilename, end= pos+length ; pos < end ; pos++)
    {
      if (*pos == '\\')
        *pos = '/';
    }
    filename= winfilename;
#endif
    packet->append(filename, length);
    packet->append('\'');
  }
}


#define LIST_PROCESS_HOST_LEN 64


/**
  Print "ON UPDATE" clause of a field into a string.

  @param timestamp_field   Pointer to timestamp field of a table.
  @param field             The field to generate ON UPDATE clause for.
  @bool  lcase             Whether to print in lower case.
  @return                  false on success, true on error.
*/
static bool print_on_update_clause(Field *field, String *val, bool lcase)
{
  DBUG_ASSERT(val->charset()->mbminlen == 1);
  val->length(0);
  if (field->has_update_default_function())
  {
    if (lcase)
      val->append(STRING_WITH_LEN("on update "));
    else
      val->append(STRING_WITH_LEN("ON UPDATE "));
    val->append(STRING_WITH_LEN("current_timestamp"));
    if (field->decimals() > 0)
      val->append_parenthesized(field->decimals());
    else
      val->append(STRING_WITH_LEN("()"));
    return true;
  }
  return false;
}


static bool get_field_default_value(THD *thd, Field *field, String *def_value,
                                    bool quoted)
{
  bool has_default;
  enum enum_field_types field_type= field->type();

  has_default= (field->default_value ||
                (!(field->flags & NO_DEFAULT_VALUE_FLAG) &&
		 !field->vers_sys_field() &&
                 field->unireg_check != Field::NEXT_NUMBER));

  def_value->length(0);
  if (has_default)
  {
    StringBuffer<MAX_FIELD_WIDTH> str(field->charset());
    if (field->default_value)
    {
      field->default_value->print(&str);
      if (field->default_value->expr->need_parentheses_in_default())
      {
        def_value->set_charset(&my_charset_utf8mb4_general_ci);
        def_value->append('(');
        def_value->append(str);
        def_value->append(')');
      }
      else
        def_value->append(str);
    }
    else if (!field->is_null())
    {                                             // Not null by default
      if (field_type == MYSQL_TYPE_BIT)
      {
        str.qs_append('b');
        str.qs_append('\'');
        str.qs_append(field->val_int(), 2);
        str.qs_append('\'');
        quoted= 0;
      }
      else
      {
        field->val_str(&str);
        if (!field->str_needs_quotes())
          quoted= 0;
      }
      if (str.length())
      {
        StringBuffer<MAX_FIELD_WIDTH> def_val;
        uint dummy_errors;
        /* convert to system_charset_info == utf8 */
        def_val.copy(str.ptr(), str.length(), field->charset(),
                     system_charset_info, &dummy_errors);
        if (quoted)
          append_unescaped(def_value, def_val.ptr(), def_val.length());
        else
          def_value->append(def_val);
      }
      else if (quoted)
        def_value->set(STRING_WITH_LEN("''"), system_charset_info);
    }
    else if (field->maybe_null() && quoted)
      def_value->set(STRING_WITH_LEN("NULL"), system_charset_info);    // Null as default
    else
      return 0;

  }
  return has_default;
}


/**
  Appends list of options to string

  @param thd             thread handler
  @param packet          string to append
  @param opt             list of options
  @param check_options   print all used options
  @param rules           list of known options
*/

static void append_create_options(THD *thd, String *packet,
				  engine_option_value *opt,
                                  bool check_options,
                                  ha_create_table_option *rules)
{
  bool in_comment= false;
  for(; opt; opt= opt->next)
  {
    if (check_options)
    {
      if (is_engine_option_known(opt, rules))
      {
        if (in_comment)
          packet->append(STRING_WITH_LEN(" */"));
        in_comment= false;
      }
      else
      {
        if (!in_comment)
          packet->append(STRING_WITH_LEN(" /*"));
        in_comment= true;
      }
    }

    DBUG_ASSERT(opt->value.str);
    packet->append(' ');
    append_identifier(thd, packet, &opt->name);
    packet->append('=');
    if (opt->quoted_value)
      append_unescaped(packet, opt->value.str, opt->value.length);
    else
      packet->append(&opt->value);
  }
  if (in_comment)
    packet->append(STRING_WITH_LEN(" */"));
}

/**
   Add table options to end of CREATE statement

   @param schema_table  1 if schema table
   @param sequence      1 if sequence. If sequence, we flush out options
                          not relevant for sequences.
*/

static void add_table_options(THD *thd, TABLE *table,
                              Table_specification_st *create_info_arg,
                              bool schema_table, bool sequence,
                              String *packet)
{
  sql_mode_t sql_mode= thd->variables.sql_mode;
  TABLE_SHARE *share= table->s;
  handlerton *hton;
  HA_CREATE_INFO create_info;
  bool check_options= (!(sql_mode & MODE_IGNORE_BAD_TABLE_OPTIONS) &&
                       (!create_info_arg ||
                        create_info_arg->used_fields &
                        HA_CREATE_PRINT_ALL_OPTIONS));

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (table->part_info)
    hton= table->part_info->default_engine_type;
  else
#endif
    hton= table->file->ht;

  bzero((char*) &create_info, sizeof(create_info));
  /* Allow update_create_info to update row type, page checksums and options */
  create_info.row_type= share->row_type;
  create_info.page_checksum= share->page_checksum;
  create_info.options= share->db_create_options;
  table->file->update_create_info(&create_info);

  /*
    IF   check_create_info
    THEN add ENGINE only if it was used when creating the table
  */
  if (!create_info_arg ||
      (create_info_arg->used_fields & HA_CREATE_USED_ENGINE))
  {
    LEX_CSTRING *engine_name= table->file->engine_name();

    if (sql_mode & (MODE_MYSQL323 | MODE_MYSQL40))
      packet->append(STRING_WITH_LEN(" TYPE="));
    else
      packet->append(STRING_WITH_LEN(" ENGINE="));

    packet->append(engine_name->str, engine_name->length);
  }

  if (sequence)
    goto end_options;

  /*
    Add AUTO_INCREMENT=... if there is an AUTO_INCREMENT column,
    and NEXT_ID > 1 (the default).  We must not print the clause
    for engines that do not support this as it would break the
    import of dumps, but as of this writing, the test for whether
    AUTO_INCREMENT columns are allowed and wether AUTO_INCREMENT=...
    is supported is identical, !(file->table_flags() & HA_NO_AUTO_INCREMENT))
    Because of that, we do not explicitly test for the feature,
    but may extrapolate its existence from that of an AUTO_INCREMENT column.
  */

  if (create_info.auto_increment_value > 1)
  {
    packet->append(STRING_WITH_LEN(" AUTO_INCREMENT="));
    packet->append_ulonglong(create_info.auto_increment_value);
  }

  if (share->table_charset && !(sql_mode & (MODE_MYSQL323 | MODE_MYSQL40)) &&
      share->table_type != TABLE_TYPE_SEQUENCE)
  {
    /*
      IF   check_create_info
      THEN add DEFAULT CHARSET only if it was used when creating the table
    */
    if (!create_info_arg ||
        (create_info_arg->used_fields & HA_CREATE_USED_DEFAULT_CHARSET))
    {
      packet->append(STRING_WITH_LEN(" DEFAULT CHARSET="));
      packet->append(share->table_charset->cs_name);
      if (!(share->table_charset->state & MY_CS_PRIMARY))
      {
        packet->append(STRING_WITH_LEN(" COLLATE="));
        packet->append(table->s->table_charset->coll_name);
      }
    }
  }

  if (share->min_rows)
  {
    packet->append(STRING_WITH_LEN(" MIN_ROWS="));
    packet->append_ulonglong(share->min_rows);
  }

  if (share->max_rows && !schema_table && !sequence)
  {
    packet->append(STRING_WITH_LEN(" MAX_ROWS="));
    packet->append_ulonglong(share->max_rows);
  }

  if (share->avg_row_length)
  {
    packet->append(STRING_WITH_LEN(" AVG_ROW_LENGTH="));
    packet->append_ulonglong(share->avg_row_length);
  }

  if (create_info.options & HA_OPTION_PACK_KEYS)
    packet->append(STRING_WITH_LEN(" PACK_KEYS=1"));
  if (create_info.options & HA_OPTION_NO_PACK_KEYS)
    packet->append(STRING_WITH_LEN(" PACK_KEYS=0"));
  if (share->db_create_options & HA_OPTION_STATS_PERSISTENT)
    packet->append(STRING_WITH_LEN(" STATS_PERSISTENT=1"));
  if (share->db_create_options & HA_OPTION_NO_STATS_PERSISTENT)
    packet->append(STRING_WITH_LEN(" STATS_PERSISTENT=0"));
  if (share->stats_auto_recalc == HA_STATS_AUTO_RECALC_ON)
    packet->append(STRING_WITH_LEN(" STATS_AUTO_RECALC=1"));
  else if (share->stats_auto_recalc == HA_STATS_AUTO_RECALC_OFF)
    packet->append(STRING_WITH_LEN(" STATS_AUTO_RECALC=0"));
  if (share->stats_sample_pages != 0)
  {
    packet->append(STRING_WITH_LEN(" STATS_SAMPLE_PAGES="));
    packet->append_ulonglong(share->stats_sample_pages);
  }

  /* We use CHECKSUM, instead of TABLE_CHECKSUM, for backward compatibility */
  if (create_info.options & HA_OPTION_CHECKSUM)
    packet->append(STRING_WITH_LEN(" CHECKSUM=1"));
  if (create_info.page_checksum != HA_CHOICE_UNDEF)
  {
    packet->append(STRING_WITH_LEN(" PAGE_CHECKSUM="));
    packet->append(ha_choice_values[create_info.page_checksum]);
  }
  if (create_info.options & HA_OPTION_DELAY_KEY_WRITE)
    packet->append(STRING_WITH_LEN(" DELAY_KEY_WRITE=1"));
  if (create_info.row_type != ROW_TYPE_DEFAULT)
  {
    packet->append(STRING_WITH_LEN(" ROW_FORMAT="));
    packet->append(&ha_row_type[(uint) create_info.row_type]);
  }
  if (share->transactional != HA_CHOICE_UNDEF)
  {
    packet->append(STRING_WITH_LEN(" TRANSACTIONAL="));
    packet->append(ha_choice_values[(uint) share->transactional]);
  }
  if (share->table_type == TABLE_TYPE_SEQUENCE)
    packet->append(STRING_WITH_LEN(" SEQUENCE=1"));
  if (table->s->key_block_size)
  {
    packet->append(STRING_WITH_LEN(" KEY_BLOCK_SIZE="));
    packet->append_ulonglong(table->s->key_block_size);
  }
  table->file->append_create_info(packet);

end_options:
  if (share->comment.length)
  {
    packet->append(STRING_WITH_LEN(" COMMENT="));
    append_unescaped(packet, share->comment.str, share->comment.length);
  }
  if (share->connect_string.length)
  {
    packet->append(STRING_WITH_LEN(" CONNECTION="));
    append_unescaped(packet, share->connect_string.str, share->connect_string.length);
  }
  append_create_options(thd, packet, share->option_list, check_options,
                        hton->table_options);
  append_directory(thd, packet, &DATA_clex_str,  create_info.data_file_name);
  append_directory(thd, packet, &INDEX_clex_str, create_info.index_file_name);
}

static void append_period(THD *thd, String *packet, const LEX_CSTRING &start,
                          const LEX_CSTRING &end, const LEX_CSTRING &period,
                          bool ident)
{
  packet->append(STRING_WITH_LEN(",\n  PERIOD FOR "));
  if (ident)
    append_identifier(thd, packet, period.str, period.length);
  else
    packet->append(period);
  packet->append(STRING_WITH_LEN(" ("));
  append_identifier(thd, packet, start.str, start.length);
  packet->append(STRING_WITH_LEN(", "));
  append_identifier(thd, packet, end.str, end.length);
  packet->append(STRING_WITH_LEN(")"));
}

int show_create_table(THD *thd, TABLE_LIST *table_list, String *packet,
                      Table_specification_st *create_info_arg,
                      enum_with_db_name with_db_name)
{
  return show_create_table_ex(thd, table_list, NULL, NULL, packet,
                              create_info_arg, with_db_name);
}

/*
  Build a CREATE TABLE statement for a table.

  SYNOPSIS
    show_create_table()
    thd               The thread
    table_list        A list containing one table to write statement
                      for.
    force_db          If not NULL, database name to use in the CREATE
                      TABLE statement.
    force_name        If not NULL, table name to use in the CREATE TABLE
                      statement. if NULL, the name from table_list will be
                      used.
    packet            Pointer to a string where statement will be
                      written.
    create_info_arg   Pointer to create information that can be used
                      to tailor the format of the statement.  Can be
                      NULL, in which case only SQL_MODE is considered
                      when building the statement.
    with_db_name     Add database name to table name

  NOTE
    Currently always return 0, but might return error code in the
    future.

  RETURN
    0       OK
 */

int show_create_table_ex(THD *thd, TABLE_LIST *table_list,
                         const char *force_db, const char *force_name,
                         String *packet,
                         Table_specification_st *create_info_arg,
                         enum_with_db_name with_db_name)
{
  List<Item> field_list;
  char tmp[MAX_FIELD_WIDTH], *for_str, def_value_buf[MAX_FIELD_WIDTH];
  LEX_CSTRING alias;
  String type;
  String def_value;
  Field **ptr,*field;
  uint primary_key;
  KEY *key_info;
  TABLE *table= table_list->table;
  TABLE_SHARE *share= table->s;
  TABLE_SHARE::period_info_t &period= share->period;
  sql_mode_t sql_mode= thd->variables.sql_mode;
  bool explicit_fields= false;
  bool foreign_db_mode=  sql_mode & (MODE_POSTGRESQL | MODE_ORACLE |
                                     MODE_MSSQL | MODE_DB2 |
                                     MODE_MAXDB | MODE_ANSI);
  bool limited_mysql_mode= sql_mode & (MODE_NO_FIELD_OPTIONS | MODE_MYSQL323 |
                                       MODE_MYSQL40);
  bool show_table_options= !(sql_mode & MODE_NO_TABLE_OPTIONS) &&
                           !foreign_db_mode;
  bool check_options= !(sql_mode & MODE_IGNORE_BAD_TABLE_OPTIONS) &&
                      !create_info_arg;
  handlerton *hton;
  int error= 0;
  DBUG_ENTER("show_create_table");
  DBUG_PRINT("enter",("table: %s", table->s->table_name.str));

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (table->part_info)
    hton= table->part_info->default_engine_type;
  else
#endif
    hton= table->file->ht;

  restore_record(table, s->default_values); // Get empty record

  packet->append(STRING_WITH_LEN("CREATE "));
  if (create_info_arg &&
      ((create_info_arg->or_replace() &&
        !create_info_arg->or_replace_slave_generated()) ||
       create_info_arg->table_was_deleted))
    packet->append(STRING_WITH_LEN("OR REPLACE "));
  if (share->tmp_table)
    packet->append(STRING_WITH_LEN("TEMPORARY "));
  packet->append(STRING_WITH_LEN("TABLE "));
  if (create_info_arg && create_info_arg->if_not_exists())
    packet->append(STRING_WITH_LEN("IF NOT EXISTS "));

  if (force_name)
  {
    if (force_db)
    {
      append_identifier(thd, packet, force_db, strlen(force_db));
      packet->append(STRING_WITH_LEN("."));
    }
    append_identifier(thd, packet, force_name, strlen(force_name));
  }
  else
  {
    if (table_list->schema_table)
    {
      alias.str= table_list->schema_table->table_name;
      alias.length= strlen(alias.str);
    }
    else
    {
      if (lower_case_table_names == 2)
      {
        alias.str= table->alias.c_ptr();
        alias.length= table->alias.length();
      }
      else
        alias= share->table_name;
    }

    /*
      Print the database before the table name if told to do that. The
      database name is only printed in the event that it is different
      from the current database.  The main reason for doing this is to
      avoid having to update gazillions of tests and result files, but
      it also saves a few bytes of the binary log.
     */
    if (with_db_name == WITH_DB_NAME)
    {
      const LEX_CSTRING *const db=
        table_list->schema_table ? &INFORMATION_SCHEMA_NAME : &table->s->db;
      if (!thd->db.str || cmp(db, &thd->db))
      {
        append_identifier(thd, packet, db);
        packet->append(STRING_WITH_LEN("."));
      }
    }

    append_identifier(thd, packet, &alias);
  }

  packet->append(STRING_WITH_LEN(" (\n"));
  /*
    We need this to get default values from the table
    We have to restore the read_set if we are called from insert in case
    of row based replication.
  */
  MY_BITMAP *old_map= tmp_use_all_columns(table, &table->read_set);

  bool not_the_first_field= false;
  for (ptr=table->field ; (field= *ptr); ptr++)
  {

    uint flags = field->flags;

    if (field->invisible > INVISIBLE_USER)
       continue;
    if (not_the_first_field)
      packet->append(STRING_WITH_LEN(",\n"));

    not_the_first_field= true;
    packet->append(STRING_WITH_LEN("  "));
    append_identifier(thd, packet, &field->field_name);
    packet->append(' ');

    const Type_handler *th= field->type_handler();
    const Schema *implied_schema= Schema::find_implied(thd);
    if (th != implied_schema->map_data_type(thd, th))
    {
      packet->append(th->schema()->name(), system_charset_info);
      packet->append(STRING_WITH_LEN("."), system_charset_info);
    }
    type.set(tmp, sizeof(tmp), system_charset_info);
    field->sql_type(type);
    packet->append(type.ptr(), type.length(), system_charset_info);

    if (field->has_charset() && !(sql_mode & (MODE_MYSQL323 | MODE_MYSQL40)))
    {
      if (field->charset() != share->table_charset)
      {
	packet->append(STRING_WITH_LEN(" CHARACTER SET "));
	packet->append(field->charset()->cs_name);
      }
      /*
	For string types dump collation name only if
	collation is not primary for the given charset

        For generated fields don't print the COLLATE clause if
        the collation matches the expression's collation.
      */
      if (!(field->charset()->state & MY_CS_PRIMARY) &&
          (!field->vcol_info ||
           field->charset() != field->vcol_info->expr->collation.collation))
      {
	packet->append(STRING_WITH_LEN(" COLLATE "));
	packet->append(field->charset()->coll_name);
      }
    }

    if (field->vcol_info)
    {
      StringBuffer<MAX_FIELD_WIDTH> str(&my_charset_utf8mb4_general_ci);
      field->vcol_info->print(&str);
      packet->append(STRING_WITH_LEN(" GENERATED ALWAYS AS ("));
      packet->append(str);
      packet->append(STRING_WITH_LEN(")"));
      if (field->vcol_info->stored_in_db)
        packet->append(STRING_WITH_LEN(" STORED"));
      else
        packet->append(STRING_WITH_LEN(" VIRTUAL"));
      if (field->invisible == INVISIBLE_USER)
      {
        packet->append(STRING_WITH_LEN(" INVISIBLE"));
      }
    }
    else
    {
      if (field->flags & VERS_ROW_START)
      {
        packet->append(STRING_WITH_LEN(" GENERATED ALWAYS AS ROW START"));
      }
      else if (field->flags & VERS_ROW_END)
      {
        packet->append(STRING_WITH_LEN(" GENERATED ALWAYS AS ROW END"));
      }
      else if (flags & NOT_NULL_FLAG)
        packet->append(STRING_WITH_LEN(" NOT NULL"));
      else if (field->type() == MYSQL_TYPE_TIMESTAMP)
      {
        /*
          TIMESTAMP field require explicit NULL flag, because unlike
          all other fields they are treated as NOT NULL by default.
        */
        packet->append(STRING_WITH_LEN(" NULL"));
      }

      if (field->invisible == INVISIBLE_USER)
      {
        packet->append(STRING_WITH_LEN(" INVISIBLE"));
      }
      def_value.set(def_value_buf, sizeof(def_value_buf), system_charset_info);
      if (get_field_default_value(thd, field, &def_value, 1))
      {
        packet->append(STRING_WITH_LEN(" DEFAULT "));
        packet->append(def_value.ptr(), def_value.length(), system_charset_info);
      }

      if (field->vers_update_unversioned())
      {
        packet->append(STRING_WITH_LEN(" WITHOUT SYSTEM VERSIONING"));
      }

      if (!limited_mysql_mode &&
          print_on_update_clause(field, &def_value, false))
      {
        packet->append(STRING_WITH_LEN(" "));
        packet->append(def_value);
      }

      if (field->unireg_check == Field::NEXT_NUMBER &&
          !(sql_mode & MODE_NO_FIELD_OPTIONS))
        packet->append(STRING_WITH_LEN(" AUTO_INCREMENT"));
    }

    if (field->comment.length)
    {
      packet->append(STRING_WITH_LEN(" COMMENT "));
      append_unescaped(packet, field->comment.str, field->comment.length);
    }

    append_create_options(thd, packet, field->option_list, check_options,
                          hton->field_options);
    
    if (field->check_constraint)
    {
      StringBuffer<MAX_FIELD_WIDTH> str(&my_charset_utf8mb4_general_ci);
      field->check_constraint->print(&str);
      packet->append(STRING_WITH_LEN(" CHECK ("));
      packet->append(str);
      packet->append(STRING_WITH_LEN(")"));
    }

  }

  if (period.name)
  {
    append_period(thd, packet,
                  period.start_field(share)->field_name,
                  period.end_field(share)->field_name,
                  period.name, true);
  }

  key_info= table->s->key_info;
  primary_key= share->primary_key;

  for (uint i=0 ; i < share->keys ; i++,key_info++)
  {
    if (key_info->flags & HA_INVISIBLE_KEY)
      continue;
    KEY_PART_INFO *key_part= key_info->key_part;
    bool found_primary=0;
    packet->append(STRING_WITH_LEN(",\n  "));

    if (i == primary_key && !strcmp(key_info->name.str, primary_key_name.str))
    {
      found_primary=1;
      /*
        No space at end, because a space will be added after where the
        identifier would go, but that is not added for primary key.
      */
      packet->append(STRING_WITH_LEN("PRIMARY KEY"));
    }
    else if (key_info->flags & HA_NOSAME)
      packet->append(STRING_WITH_LEN("UNIQUE KEY "));
    else if (key_info->flags & HA_FULLTEXT)
      packet->append(STRING_WITH_LEN("FULLTEXT KEY "));
    else if (key_info->flags & HA_SPATIAL)
      packet->append(STRING_WITH_LEN("SPATIAL KEY "));
    else
      packet->append(STRING_WITH_LEN("KEY "));

    if (!found_primary)
      append_identifier(thd, packet, &key_info->name);

    packet->append(STRING_WITH_LEN(" ("));

    uint key_parts= key_info->user_defined_key_parts;
    if (key_info->without_overlaps)
      key_parts-= 2;

    for (uint j=0 ; j < key_parts ; j++,key_part++)
    {
      Field *field= key_part->field;
      if (field->invisible > INVISIBLE_USER)
        continue;

      if (j)
        packet->append(',');

      if (key_part->field)
        append_identifier(thd, packet, &key_part->field->field_name);
      if (key_part->field &&
          (key_part->length !=
           table->field[key_part->fieldnr-1]->key_length() &&
           !(key_info->flags & (HA_FULLTEXT | HA_SPATIAL))))
      {
        packet->append_parenthesized((long) key_part->length /
                                      key_part->field->charset()->mbmaxlen);
      }
      if (table->file->index_flags(i, j, 0) & HA_READ_ORDER &&
          key_part->key_part_flag & HA_REVERSE_SORT) /* same in SHOW KEYS */
        packet->append(STRING_WITH_LEN(" DESC"));
    }

    if (key_info->without_overlaps)
    {
      packet->append(',');
      append_identifier(thd, packet, &share->period.name);
      packet->append(STRING_WITH_LEN(" WITHOUT OVERLAPS"));
    }

    packet->append(')');
    store_key_options(thd, packet, table, &table->key_info[i]);
    if (key_info->parser)
    {
      LEX_CSTRING *parser_name= plugin_name(key_info->parser);
      packet->append(STRING_WITH_LEN(" /*!50100 WITH PARSER "));
      append_identifier(thd, packet, parser_name);
      packet->append(STRING_WITH_LEN(" */ "));
    }
    append_create_options(thd, packet, key_info->option_list, check_options,
                          hton->index_options);
  }

  if (table->versioned())
  {
    const Field *fs = table->vers_start_field();
    const Field *fe = table->vers_end_field();
    DBUG_ASSERT(fs);
    DBUG_ASSERT(fe);
    explicit_fields= fs->invisible < INVISIBLE_SYSTEM;
    DBUG_ASSERT(!explicit_fields || fe->invisible < INVISIBLE_SYSTEM);
    if (explicit_fields)
    {
      append_period(thd, packet, fs->field_name, fe->field_name,
                    table->s->vers.name, false);
    }
    else
    {
      DBUG_ASSERT(fs->invisible == INVISIBLE_SYSTEM);
      DBUG_ASSERT(fe->invisible == INVISIBLE_SYSTEM);
    }
  }

  /*
    Get possible foreign key definitions stored in InnoDB and append them
    to the CREATE TABLE statement
  */

  if ((for_str= table->file->get_foreign_key_create_info()))
  {
    packet->append(for_str, strlen(for_str));
    table->file->free_foreign_key_create_info(for_str);
  }

  /* Add table level check constraints */
  if (share->table_check_constraints)
  {
    StringBuffer<MAX_FIELD_WIDTH> str(&my_charset_utf8mb4_general_ci);
    for (uint i= share->field_check_constraints;
         i < share->table_check_constraints ; i++)
    {
      Virtual_column_info *check= table->check_constraints[i];
      // period constraint is implicit
      if (share->period.constr_name.streq(check->name))
        continue;

      str.set_buffer_if_not_allocated(&my_charset_utf8mb4_general_ci);
      str.length(0);                            // Print appends to str
      check->print(&str);

      packet->append(STRING_WITH_LEN(",\n  "));
      if (check->name.str)
      {
        packet->append(STRING_WITH_LEN("CONSTRAINT "));
        append_identifier(thd, packet, &check->name);
      }
      packet->append(STRING_WITH_LEN(" CHECK ("));
      packet->append(str);
      packet->append(STRING_WITH_LEN(")"));
    }
  }

  packet->append(STRING_WITH_LEN("\n)"));
  if (show_table_options)
    add_table_options(thd, table, create_info_arg,
                      table_list->schema_table != 0, 0, packet);

  if (table->versioned())
    packet->append(STRING_WITH_LEN(" WITH SYSTEM VERSIONING"));

#ifdef WITH_PARTITION_STORAGE_ENGINE
  {
    if (table->part_info &&
        !((table->s->db_type()->partition_flags() & HA_USE_AUTO_PARTITION) &&
          table->part_info->is_auto_partitioned))
    {
      /*
        Partition syntax for CREATE TABLE is at the end of the syntax.
      */
      uint part_syntax_len;
      char *part_syntax;
      if ((part_syntax= generate_partition_syntax(thd, table->part_info,
                                                  &part_syntax_len,
                                                  show_table_options,
                                                  NULL, NULL)))
      {
         packet->append('\n');
         if (packet->append(part_syntax, part_syntax_len))
          error= 1;
      }
    }
  }
#endif
  tmp_restore_column_map(&table->read_set, old_map);
  DBUG_RETURN(error);
}


static void store_key_options(THD *thd, String *packet, TABLE *table,
                              KEY *key_info)
{
  bool limited_mysql_mode= (thd->variables.sql_mode &
                            (MODE_NO_FIELD_OPTIONS | MODE_MYSQL323 |
                             MODE_MYSQL40)) != 0;
  bool foreign_db_mode=  (thd->variables.sql_mode & (MODE_POSTGRESQL |
                                                     MODE_ORACLE |
                                                     MODE_MSSQL |
                                                     MODE_DB2 |
                                                     MODE_MAXDB |
                                                     MODE_ANSI)) != 0;
  char *end, buff[32];

  if (!(thd->variables.sql_mode & MODE_NO_KEY_OPTIONS) &&
      !limited_mysql_mode && !foreign_db_mode)
  {

    if (key_info->algorithm == HA_KEY_ALG_BTREE)
      packet->append(STRING_WITH_LEN(" USING BTREE"));

    if (key_info->algorithm == HA_KEY_ALG_HASH ||
        key_info->algorithm == HA_KEY_ALG_LONG_HASH)
      packet->append(STRING_WITH_LEN(" USING HASH"));

    /* send USING only in non-default case: non-spatial rtree */
    if ((key_info->algorithm == HA_KEY_ALG_RTREE) &&
        !(key_info->flags & HA_SPATIAL))
      packet->append(STRING_WITH_LEN(" USING RTREE"));

    if ((key_info->flags & HA_USES_BLOCK_SIZE) &&
        table->s->key_block_size != key_info->block_size)
    {
      packet->append(STRING_WITH_LEN(" KEY_BLOCK_SIZE="));
      end= longlong10_to_str(key_info->block_size, buff, 10);
      packet->append(buff, (uint) (end - buff));
    }
    DBUG_ASSERT(MY_TEST(key_info->flags & HA_USES_COMMENT) ==
               (key_info->comment.length > 0));
    if (key_info->flags & HA_USES_COMMENT)
    {
      packet->append(STRING_WITH_LEN(" COMMENT "));
      append_unescaped(packet, key_info->comment.str, 
                       key_info->comment.length);
    }

    if (key_info->is_ignored)
      packet->append(STRING_WITH_LEN(" IGNORED"));
  }
}


void view_store_options(THD *thd, TABLE_LIST *table, String *buff)
{
  if (table->algorithm != VIEW_ALGORITHM_INHERIT)
  {
    buff->append(STRING_WITH_LEN("ALGORITHM="));
    buff->append(view_algorithm(table));
  }
  buff->append(' ');
  append_definer(thd, buff, &table->definer.user, &table->definer.host);
  if (table->view_suid)
    buff->append(STRING_WITH_LEN("SQL SECURITY DEFINER "));
  else
    buff->append(STRING_WITH_LEN("SQL SECURITY INVOKER "));
}


/**
  Returns ALGORITHM clause of a view
*/

static const LEX_CSTRING *view_algorithm(TABLE_LIST *table)
{
  static const LEX_CSTRING undefined= { STRING_WITH_LEN("UNDEFINED") };
  static const LEX_CSTRING merge=     { STRING_WITH_LEN("MERGE") };
  static const LEX_CSTRING temptable= { STRING_WITH_LEN("TEMPTABLE") };
  switch (table->algorithm) {
  case VIEW_ALGORITHM_TMPTABLE:
    return &temptable;
  case VIEW_ALGORITHM_MERGE:
    return &merge;
  default:
    DBUG_ASSERT(0); // never should happen
    /* fall through */
  case VIEW_ALGORITHM_UNDEFINED:
    return &undefined;
  }
}


static bool append_at_host(THD *thd, String *buffer, const LEX_CSTRING *host)
{
  if (!host->str || !host->str[0])
    return false;
  return
    buffer->append('@') ||
    append_identifier(thd, buffer, host);
}


/*
  Append DEFINER clause to the given buffer.

  SYNOPSIS
    append_definer()
    thd           [in] thread handle
    buffer        [inout] buffer to hold DEFINER clause
    definer_user  [in] user name part of definer
    definer_host  [in] host name part of definer
*/

bool append_definer(THD *thd, String *buffer, const LEX_CSTRING *definer_user,
                    const LEX_CSTRING *definer_host)
{
  return
    buffer->append(STRING_WITH_LEN("DEFINER=")) ||
    append_identifier(thd, buffer, definer_user) ||
    append_at_host(thd, buffer, definer_host) ||
    buffer->append(' ');
}


static int show_create_view(THD *thd, TABLE_LIST *table, String *buff)
{
  my_bool compact_view_name= TRUE;
  my_bool foreign_db_mode= (thd->variables.sql_mode & (MODE_POSTGRESQL |
                                                       MODE_ORACLE |
                                                       MODE_MSSQL |
                                                       MODE_DB2 |
                                                       MODE_MAXDB |
                                                       MODE_ANSI)) != 0;

  if (!thd->db.str || cmp(&thd->db, &table->view_db))
    /*
      print compact view name if the view belongs to the current database
    */
    compact_view_name= table->compact_view_format= FALSE;
  else
  {
    /*
      Compact output format for view body can be used
      if this view only references table inside it's own db
    */
    TABLE_LIST *tbl;
    table->compact_view_format= TRUE;
    for (tbl= thd->lex->query_tables;
         tbl;
         tbl= tbl->next_global)
    {
      if (!tbl->is_derived() &&
          cmp(&table->view_db, tbl->view ? &tbl->view_db : &tbl->db))
      {
        table->compact_view_format= FALSE;
        break;
      }
    }
  }

  buff->append(STRING_WITH_LEN("CREATE "));
  if (!foreign_db_mode)
  {
    view_store_options(thd, table, buff);
  }
  buff->append(STRING_WITH_LEN("VIEW "));
  if (!compact_view_name)
  {
    append_identifier(thd, buff, &table->view_db);
    buff->append('.');
  }
  append_identifier(thd, buff, &table->view_name);
  buff->append(STRING_WITH_LEN(" AS "));

  /*
    We can't just use table->query, because our SQL_MODE may trigger
    a different syntax, like when ANSI_QUOTES is defined.
  */
  table->view->unit.print(buff, enum_query_type(QT_VIEW_INTERNAL |
                                                QT_ITEM_ORIGINAL_FUNC_NULLIF));

  if (table->with_check != VIEW_CHECK_NONE)
  {
    if (table->with_check == VIEW_CHECK_LOCAL)
      buff->append(STRING_WITH_LEN(" WITH LOCAL CHECK OPTION"));
    else
      buff->append(STRING_WITH_LEN(" WITH CASCADED CHECK OPTION"));
  }
  return 0;
}


static int show_create_sequence(THD *thd, TABLE_LIST *table_list,
                                String *packet)
{
  TABLE *table= table_list->table;
  SEQUENCE *seq= table->s->sequence;
  LEX_CSTRING alias;
  sql_mode_t sql_mode= thd->variables.sql_mode;
  bool foreign_db_mode=  sql_mode & (MODE_POSTGRESQL | MODE_ORACLE |
                                     MODE_MSSQL | MODE_DB2 |
                                     MODE_MAXDB | MODE_ANSI);
  bool show_table_options= !(sql_mode & MODE_NO_TABLE_OPTIONS) &&
                           !foreign_db_mode;

  if (lower_case_table_names == 2)
  {
    alias.str=    table->alias.c_ptr();
    alias.length= table->alias.length();
  }
  else
    alias= table->s->table_name;

  packet->append(STRING_WITH_LEN("CREATE SEQUENCE "));
  append_identifier(thd, packet, &alias);
  packet->append(STRING_WITH_LEN(" start with "));
  packet->append_longlong(seq->start);
  packet->append(STRING_WITH_LEN(" minvalue "));
  packet->append_longlong(seq->min_value);
  packet->append(STRING_WITH_LEN(" maxvalue "));
  packet->append_longlong(seq->max_value);
  packet->append(STRING_WITH_LEN(" increment by "));
  packet->append_longlong(seq->increment);
  if (seq->cache)
  {
    packet->append(STRING_WITH_LEN(" cache "));
    packet->append_longlong(seq->cache);
  }
  else
    packet->append(STRING_WITH_LEN(" nocache"));
  if (seq->cycle)
    packet->append(STRING_WITH_LEN(" cycle"));
  else
    packet->append(STRING_WITH_LEN(" nocycle"));

  if (show_table_options)
    add_table_options(thd, table, 0, 0, 1, packet);
  return 0;
}


/****************************************************************************
  Return info about all processes
  returns for each thread: thread id, user, host, db, command, info
****************************************************************************/

class thread_info :public ilink {
public:
  static void *operator new(size_t size, MEM_ROOT *mem_root) throw ()
  { return alloc_root(mem_root, size); }
  static void operator delete(void *ptr __attribute__((unused)),
                              size_t size __attribute__((unused)))
  { TRASH_FREE(ptr, size); }
  static void operator delete(void *, MEM_ROOT *){}

  my_thread_id thread_id;
  uint32 os_thread_id;
  ulonglong start_time;
  uint   command;
  const char *user,*host,*db,*proc_info,*state_info;
  CSET_STRING query_string;
  double progress;
};

static const char *thread_state_info(THD *tmp)
{
#ifndef EMBEDDED_LIBRARY
  if (tmp->net.reading_or_writing)
  {
    if (tmp->net.reading_or_writing == 2)
      return "Writing to net";
    if (tmp->get_command() == COM_SLEEP)
      return "";
    return "Reading from net";
  }
#else
  if (tmp->get_command() == COM_SLEEP)
    return "";
#endif

  if (tmp->proc_info)
    return tmp->proc_info;

  /* Check if we are waiting on a condition */
  if (!trylock_short(&tmp->LOCK_thd_kill))
  {
    /* mysys_var is protected by above mutex */
    bool cond= tmp->mysys_var && tmp->mysys_var->current_cond;
    mysql_mutex_unlock(&tmp->LOCK_thd_kill);
    if (cond)
      return "Waiting on cond";
  }
  return NULL;
}


struct list_callback_arg
{
  list_callback_arg(const char *u, THD *t, ulong m):
    user(u), thd(t), max_query_length(m) {}
  I_List<thread_info> thread_infos;
  const char *user;
  THD *thd;
  ulong max_query_length;
};


static my_bool list_callback(THD *tmp, list_callback_arg *arg)
{

  Security_context *tmp_sctx= tmp->security_ctx;
  bool got_thd_data;
  if ((tmp->vio_ok() || tmp->system_thread) &&
      (!arg->user || (!tmp->system_thread &&
                      tmp_sctx->user && !strcmp(tmp_sctx->user, arg->user))))
  {
    thread_info *thd_info= new (arg->thd->mem_root) thread_info;

    thd_info->thread_id=tmp->thread_id;
    thd_info->os_thread_id=tmp->os_thread_id;
    thd_info->user= arg->thd->strdup(tmp_sctx->user ? tmp_sctx->user :
                                     (tmp->system_thread ?
                                     "system user" : "unauthenticated user"));
    if (tmp->peer_port && (tmp_sctx->host || tmp_sctx->ip) &&
        arg->thd->security_ctx->host_or_ip[0])
    {
      if ((thd_info->host= (char*) arg->thd->alloc(LIST_PROCESS_HOST_LEN+1)))
        my_snprintf((char *) thd_info->host, LIST_PROCESS_HOST_LEN,
                    "%s:%u", tmp_sctx->host_or_ip, tmp->peer_port);
    }
    else
      thd_info->host= arg->thd->strdup(tmp_sctx->host_or_ip[0] ?
                                       tmp_sctx->host_or_ip :
                                       tmp_sctx->host ? tmp_sctx->host : "");
    thd_info->command=(int) tmp->get_command();

    if ((got_thd_data= !trylock_short(&tmp->LOCK_thd_data)))
    {
      /* This is an approximation */
      thd_info->proc_info= (char*) (tmp->killed >= KILL_QUERY ?
                                    "Killed" : 0);

      /* The following variables are only safe to access under a lock */
      thd_info->db= 0;
      if (tmp->db.str)
        thd_info->db= arg->thd->strmake(tmp->db.str, tmp->db.length);

      if (tmp->query())
      {
        uint length= MY_MIN(arg->max_query_length, tmp->query_length());
        char *q= arg->thd->strmake(tmp->query(),length);
        /* Safety: in case strmake failed, we set length to 0. */
        thd_info->query_string=
          CSET_STRING(q, q ? length : 0, tmp->query_charset());
      }

      /*
        Progress report. We need to do this under a lock to ensure that all
        is from the same stage.
      */
      if (tmp->progress.max_counter)
      {
        uint max_stage= MY_MAX(tmp->progress.max_stage, 1);
        thd_info->progress= (((tmp->progress.stage / (double) max_stage) +
                              ((tmp->progress.counter /
                                (double) tmp->progress.max_counter) /
                               (double) max_stage)) *
                             100.0);
        set_if_smaller(thd_info->progress, 100);
      }
      else
        thd_info->progress= 0.0;
    }
    else
    {
      thd_info->proc_info= "Busy";
      thd_info->progress= 0.0;
      thd_info->db= "";
    }

    thd_info->state_info= thread_state_info(tmp);
    thd_info->start_time= tmp->start_utime;
    ulonglong utime_after_query_snapshot= tmp->utime_after_query;
    if (thd_info->start_time < utime_after_query_snapshot)
      thd_info->start_time= utime_after_query_snapshot; // COM_SLEEP

    if (got_thd_data)
      mysql_mutex_unlock(&tmp->LOCK_thd_data);
    arg->thread_infos.append(thd_info);
  }
  return 0;
}


void mysqld_list_processes(THD *thd,const char *user, bool verbose)
{
  Item *field;
  List<Item> field_list;
  list_callback_arg arg(user, thd,
                        verbose ? thd->variables.max_allowed_packet :
                        PROCESS_LIST_WIDTH);
  Protocol *protocol= thd->protocol;
  MEM_ROOT *mem_root= thd->mem_root;
  DBUG_ENTER("mysqld_list_processes");

  field_list.push_back(new (mem_root)
                       Item_int(thd, "Id", 0, MY_INT32_NUM_DECIMAL_DIGITS),
                       mem_root);
  field_list.push_back(new (mem_root)
                       Item_empty_string(thd, "User",
                                         USERNAME_CHAR_LENGTH),
                       mem_root);
  field_list.push_back(new (mem_root)
                       Item_empty_string(thd, "Host",
                                         LIST_PROCESS_HOST_LEN),
                       mem_root);
  field_list.push_back(field=new (mem_root)
                       Item_empty_string(thd, "db", NAME_CHAR_LEN),
                       mem_root);
  field->set_maybe_null();;
  field_list.push_back(new (mem_root) Item_empty_string(thd, "Command", 16),
                       mem_root);
  field_list.push_back(field= new (mem_root)
                       Item_return_int(thd, "Time", 7, MYSQL_TYPE_LONG),
                       mem_root);
  field->unsigned_flag= 0;
  field_list.push_back(field=new (mem_root)
                       Item_empty_string(thd, "State", 30),
                       mem_root);
  field->set_maybe_null();;
  field_list.push_back(field=new (mem_root)
                       Item_empty_string(thd, "Info", arg.max_query_length),
                       mem_root);
  field->set_maybe_null();;
  if (!(thd->variables.old_behavior & OLD_MODE_NO_PROGRESS_INFO))
  {
    field_list.push_back(field= new (mem_root)
                         Item_float(thd, "Progress", 0.0, 3, 7),
                         mem_root);
    field->base_flags&= ~item_base_t::MAYBE_NULL;
  }
  if (protocol->send_result_set_metadata(&field_list,
                                         Protocol::SEND_NUM_ROWS |
                                         Protocol::SEND_EOF))
    DBUG_VOID_RETURN;

  if (thd->killed)
    DBUG_VOID_RETURN;

  server_threads.iterate(list_callback, &arg);

  ulonglong now= microsecond_interval_timer();

  while (thread_info *thd_info= arg.thread_infos.get())
  {
    protocol->prepare_for_resend();
    protocol->store(thd_info->thread_id);
    protocol->store(thd_info->user, strlen(thd_info->user), system_charset_info);
    protocol->store(thd_info->host, strlen(thd_info->host), system_charset_info);
    protocol->store_string_or_null(thd_info->db, system_charset_info);
    if (thd_info->proc_info)
      protocol->store(thd_info->proc_info, strlen(thd_info->proc_info),
                      system_charset_info);
    else
      protocol->store(&command_name[thd_info->command], system_charset_info);
    if (thd_info->start_time && now > thd_info->start_time)
      protocol->store_long((now - thd_info->start_time) / HRTIME_RESOLUTION);
    else
      protocol->store_null();
    protocol->store_string_or_null(thd_info->state_info, system_charset_info);
    if (thd_info->query_string.length())
      protocol->store(thd_info->query_string.str(),
                      thd_info->query_string.length(),
                      thd_info->query_string.charset());
    else
      protocol->store_null();
    if (!(thd->variables.old_behavior & OLD_MODE_NO_PROGRESS_INFO))
      protocol->store_double(thd_info->progress, 3);
    if (protocol->write())
      break; /* purecov: inspected */
  }
  my_eof(thd);
  DBUG_VOID_RETURN;
}


/*
  Produce EXPLAIN data.

  This function is APC-scheduled to be run in the context of the thread that
  we're producing EXPLAIN for.
*/

void Show_explain_request::call_in_target_thread()
{
  Query_arena backup_arena;
  bool printed_anything= FALSE;

  /* 
    Change the arena because JOIN::print_explain and co. are going to allocate
    items. Let them allocate them on our arena.
  */
  target_thd->set_n_backup_active_arena((Query_arena*)request_thd,
                                        &backup_arena);

  query_str.copy(target_thd->query(), 
                 target_thd->query_length(),
                 target_thd->query_charset());

  DBUG_ASSERT(current_thd == target_thd);

  /*
    When producing JSON output, one should not change current_thd.
    (If one does that, they will hit an assert when printing constant item
    fields).
  */
  if (target_thd->lex->print_explain(explain_buf, 0 /* explain flags*/,
                                     is_analyze, is_json_format,
                                     &printed_anything))
  {
    failed_to_produce= TRUE;
  }

  if (!printed_anything)
    failed_to_produce= TRUE;

  target_thd->restore_active_arena((Query_arena*)request_thd, &backup_arena);
}


int select_result_explain_buffer::send_data(List<Item> &items)
{
  int res;
  THD *cur_thd= current_thd;
  DBUG_ENTER("select_result_explain_buffer::send_data");

  /*
    Switch to the receiveing thread, so that we correctly count memory used
    by it. This is needed as it's the receiving thread that will free the
    memory.
    (TODO: Now that we don't change current_thd in
    Show_explain_request::call_in_target_thread, is this necessary anymore?)
  */
  set_current_thd(thd);
  fill_record(thd, dst_table, dst_table->field, items, TRUE, FALSE);
  res= dst_table->file->ha_write_tmp_row(dst_table->record[0]);
  set_current_thd(cur_thd);  
  DBUG_RETURN(MY_TEST(res));
}

bool select_result_text_buffer::send_result_set_metadata(List<Item> &fields,
                                                         uint flag)
{
  n_columns= fields.elements;
  return append_row(fields, true /*send item names */);
}


int select_result_text_buffer::send_data(List<Item> &items)
{
  return append_row(items, false /*send item values */);
}

int select_result_text_buffer::append_row(List<Item> &items, bool send_names)
{
  List_iterator<Item> it(items);
  Item *item;
  char **row;
  int column= 0;

  if (!(row= (char**) thd->alloc(sizeof(char*) * n_columns)) ||
      rows.push_back(row, thd->mem_root))
    return true;

  StringBuffer<32> buf;

  while ((item= it++))
  {
    DBUG_ASSERT(column < n_columns);
    const char *data_ptr; 
    char *ptr;
    size_t data_len;

    buf.set_buffer_if_not_allocated(&my_charset_bin);
    if (send_names)
    {
      DBUG_ASSERT(strlen(item->name.str) == item->name.length);
      data_ptr= item->name.str;
      data_len= item->name.length;
    }
    else
    {
      String *res;
      res= item->val_str(&buf);
      if (item->null_value)
      {
        data_ptr= "NULL";
        data_len=4;
      }
      else
      {
        data_ptr= res->c_ptr_safe();
        data_len= res->length();
      }
    }

    if (!(ptr= (char*) thd->memdup(data_ptr, data_len + 1)))
      return true;
    row[column]= ptr;

    column++;
  }
  return false;
}


void select_result_text_buffer::save_to(String *res)
{
  List_iterator<char*> it(rows);
  char **row;
  res->append(STRING_WITH_LEN("#\n"));
  while ((row= it++))
  {
    res->append(STRING_WITH_LEN("# explain: "));
    for (int i=0; i < n_columns; i++)
    {
      if (i)
        res->append('\t');
      res->append(row[i], strlen(row[i]));
    }
    res->append('\n');
  }
  res->append(STRING_WITH_LEN("#\n"));
}


/*
  Store the SHOW EXPLAIN/SHOW ANALYZE output in the temporary table.
*/

int fill_show_explain_or_analyze(THD *thd, TABLE_LIST *table, COND *cond,
                                 bool json_format, bool is_analyze)
{
  const char *calling_user;
  THD *tmp;
  my_thread_id  thread_id;
  DBUG_ENTER("fill_show_explain_or_analyze");

  DBUG_ASSERT(cond==NULL);
  thread_id= thd->lex->value_list.head()->val_int();
  calling_user= (thd->security_ctx->master_access & PRIV_STMT_SHOW_EXPLAIN) ?
                 NullS : thd->security_ctx->priv_user;

  if ((tmp= find_thread_by_id(thread_id)))
  {
    Security_context *tmp_sctx= tmp->security_ctx;
    /*
      If calling_user==NULL, calling thread has SUPER or PROCESS
      privilege, and so can do SHOW EXPLAIN/SHOW ANALYZE on any user.
      
      if calling_user!=NULL, he's only allowed to view
      SHOW EXPLAIN/SHOW ANALYZE on his own threads.
    */
    if (calling_user && (!tmp_sctx->user || strcmp(calling_user, 
                                                   tmp_sctx->user)))
    {
      my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "PROCESS");
      mysql_mutex_unlock(&tmp->LOCK_thd_kill);
      DBUG_RETURN(1);
    }

    if (tmp == thd)
    {
      mysql_mutex_unlock(&tmp->LOCK_thd_kill);
      my_error(ER_TARGET_NOT_EXPLAINABLE, MYF(0));
      DBUG_RETURN(1);
    }

    bool bres;
    /* 
      Ok we've found the thread of interest and it won't go away because 
      we're holding its LOCK_thd_kill. Post it a SHOW EXPLAIN/SHOW ANALYZE request.
    */
    bool timed_out;
    int timeout_sec= 30;
    Show_explain_request explain_req;
    explain_req.is_json_format= json_format;
    select_result_explain_buffer *explain_buf;
    
    explain_buf= new select_result_explain_buffer(thd, table->table);

    explain_req.is_analyze= is_analyze;
    explain_req.explain_buf= explain_buf;
    explain_req.target_thd= tmp;
    explain_req.request_thd= thd;
    explain_req.failed_to_produce= FALSE;
    
    /* Ok, we have a lock on target->LOCK_thd_kill, can call: */
    bres= tmp->apc_target.make_apc_call(thd, &explain_req, timeout_sec, &timed_out);

    if (bres || explain_req.failed_to_produce)
    {
      if (thd->killed)
        thd->send_kill_message();
      else if (timed_out)
        my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
      else
        my_error(ER_TARGET_NOT_EXPLAINABLE, MYF(0));

      bres= TRUE;
    }
    else
    {
      /*
        Push the query string as a warning. The query may be in a different
        charset than the charset that's used for error messages, so, convert it
        if needed.
      */
      CHARSET_INFO *fromcs= explain_req.query_str.charset();
      CHARSET_INFO *tocs= error_message_charset_info;
      char *warning_text;
      if (!my_charset_same(fromcs, tocs))
      {
        uint conv_length= 1 + tocs->mbmaxlen * explain_req.query_str.length() / 
                              fromcs->mbminlen;
        uint dummy_errors;
        char *to, *p;
        if (!(to= (char*)thd->alloc(conv_length + 1)))
          DBUG_RETURN(1);
        p= to;
        p+= copy_and_convert(to, conv_length, tocs,
                             explain_req.query_str.c_ptr(), 
                             explain_req.query_str.length(), fromcs,
                             &dummy_errors);
        *p= 0;
        warning_text= to;
      }
      else
        warning_text= explain_req.query_str.c_ptr_safe();

      push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                   ER_YES, warning_text);
    }
    DBUG_RETURN(bres);
  }
  my_error(ER_NO_SUCH_THREAD, MYF(0), (ulong) thread_id);
  DBUG_RETURN(1);
}


int fill_show_explain_tabular(THD *thd, TABLE_LIST *table, COND *cond)
{
  return fill_show_explain_or_analyze(
    thd, table, cond, FALSE /* json_format */, FALSE /* is_analyze */);
}


int fill_show_explain_json(THD *thd, TABLE_LIST *table, COND *cond)
{
  return fill_show_explain_or_analyze(
    thd, table, cond, TRUE /* json_format */, FALSE /* is_analyze */);
}


int fill_show_analyze_tabular(THD * thd, TABLE_LIST * table, COND * cond)
{
  return fill_show_explain_or_analyze(
    thd, table, cond, FALSE /* json_format */, TRUE /* is_analyze */);
}


int fill_show_analyze_json(THD * thd, TABLE_LIST * table, COND * cond)
{
  return fill_show_explain_or_analyze(
    thd, table, cond, TRUE /* json_format */, TRUE /* is_analyze */);
}


struct processlist_callback_arg
{
  processlist_callback_arg(THD *thd_arg, TABLE *table_arg):
    thd(thd_arg), table(table_arg), unow(microsecond_interval_timer()) {}
  THD *thd;
  TABLE *table;
  ulonglong unow;
};


static my_bool processlist_callback(THD *tmp, processlist_callback_arg *arg)
{
  Security_context *tmp_sctx= tmp->security_ctx;
  CHARSET_INFO *cs= system_charset_info;
  const char *val;
  ulonglong max_counter;
  bool got_thd_data;
  char *user=
          arg->thd->security_ctx->master_access & PRIV_STMT_SHOW_PROCESSLIST ?
          NullS : arg->thd->security_ctx->priv_user;

  if ((!tmp->vio_ok() && !tmp->system_thread) ||
      (user && (tmp->system_thread || !tmp_sctx->user ||
                strcmp(tmp_sctx->user, user))))
    return 0;

  restore_record(arg->table, s->default_values);
  /* ID */
  arg->table->field[0]->store((longlong) tmp->thread_id, TRUE);
  /* USER */
  val= tmp_sctx->user ? tmp_sctx->user :
        (tmp->system_thread ? "system user" : "unauthenticated user");
  arg->table->field[1]->store(val, strlen(val), cs);
  /* HOST */
  if (tmp->peer_port && (tmp_sctx->host || tmp_sctx->ip) &&
      arg->thd->security_ctx->host_or_ip[0])
  {
    char host[LIST_PROCESS_HOST_LEN + 1];
    my_snprintf(host, LIST_PROCESS_HOST_LEN, "%s:%u",
                tmp_sctx->host_or_ip, tmp->peer_port);
    arg->table->field[2]->store(host, strlen(host), cs);
  }
  else
    arg->table->field[2]->store(tmp_sctx->host_or_ip,
                                strlen(tmp_sctx->host_or_ip), cs);

  if ((got_thd_data= !trylock_short(&tmp->LOCK_thd_data)))
  {
    /* DB */
    if (tmp->db.str)
    {
      arg->table->field[3]->store(tmp->db.str, tmp->db.length, cs);
      arg->table->field[3]->set_notnull();
    }
  }

  /* COMMAND */
  if ((val= (char *) (!got_thd_data ? "Busy" :
                      (tmp->killed >= KILL_QUERY ?
                       "Killed" : 0))))
    arg->table->field[4]->store(val, strlen(val), cs);
  else
    arg->table->field[4]->store(command_name[tmp->get_command()].str,
                                command_name[tmp->get_command()].length, cs);

  /* MYSQL_TIME */
  ulonglong utime= tmp->start_utime;
  ulonglong utime_after_query_snapshot= tmp->utime_after_query;
  if (utime < utime_after_query_snapshot)
    utime= utime_after_query_snapshot; // COM_SLEEP
  utime= utime && utime < arg->unow ? arg->unow - utime : 0;

  arg->table->field[5]->store(utime / HRTIME_RESOLUTION, TRUE);

  if (got_thd_data)
  {
    if (tmp->query())
    {
      arg->table->field[7]->store(tmp->query(),
                                  MY_MIN(PROCESS_LIST_INFO_WIDTH,
                                  tmp->query_length()), cs);
      arg->table->field[7]->set_notnull();

      /* INFO_BINARY */
      arg->table->field[16]->store(tmp->query(),
                                   MY_MIN(PROCESS_LIST_INFO_WIDTH,
                                          tmp->query_length()),
                                   &my_charset_bin);
      arg->table->field[16]->set_notnull();
    }

    /*
      Progress report. We need to do this under a lock to ensure that all
      is from the same stage.
    */
    if ((max_counter= tmp->progress.max_counter))
    {
      arg->table->field[9]->store((longlong) tmp->progress.stage + 1, 1);
      arg->table->field[10]->store((longlong) tmp->progress.max_stage, 1);
      arg->table->field[11]->store((double) tmp->progress.counter /
                                   (double) max_counter*100.0);
    }
    mysql_mutex_unlock(&tmp->LOCK_thd_data);
  }

  /* STATE */
  if ((val= thread_state_info(tmp)))
  {
    arg->table->field[6]->store(val, strlen(val), cs);
    arg->table->field[6]->set_notnull();
  }

  /* TIME_MS */
  arg->table->field[8]->store((double)(utime / (HRTIME_RESOLUTION / 1000.0)));

  /*
    This may become negative if we free a memory allocated by another
    thread in this thread. However it's better that we notice it eventually
    than hide it.
  */
  arg->table->field[12]->store((longlong) tmp->status_var.local_memory_used,
                               FALSE);
  arg->table->field[13]->store((longlong) tmp->status_var.max_local_memory_used,
                               FALSE);
  arg->table->field[14]->store((longlong) tmp->get_examined_row_count(), TRUE);

  /* QUERY_ID */
  arg->table->field[15]->store(tmp->query_id, TRUE);

  arg->table->field[17]->store(tmp->os_thread_id);

  if (schema_table_store_record(arg->thd, arg->table))
    return 1;
  return 0;
}


int fill_schema_processlist(THD* thd, TABLE_LIST* tables, COND* cond)
{
  processlist_callback_arg arg(thd, tables->table);
  DBUG_ENTER("fill_schema_processlist");
  DEBUG_SYNC(thd,"fill_schema_processlist_after_unow");
  if (!thd->killed &&
      server_threads.iterate(processlist_callback, &arg))
    DBUG_RETURN(1);
  DBUG_RETURN(0);
}

/*****************************************************************************
  Status functions
*****************************************************************************/

DYNAMIC_ARRAY all_status_vars;
static bool status_vars_inited= 0;
ulonglong status_var_array_version= 0;

C_MODE_START
static int show_var_cmp(const void *var1, const void *var2)
{
  return strcasecmp(((SHOW_VAR*)var1)->name, ((SHOW_VAR*)var2)->name);
}
C_MODE_END

/*
  deletes all the SHOW_UNDEF elements from the array and calls
  delete_dynamic() if it's completely empty.
*/
static void shrink_var_array(DYNAMIC_ARRAY *array)
{
  uint a,b;
  SHOW_VAR *all= dynamic_element(array, 0, SHOW_VAR *);

  for (a= b= 0; b < array->elements; b++)
    if (all[b].type != SHOW_UNDEF)
      all[a++]= all[b];
  if (a)
  {
    bzero(all+a, sizeof(SHOW_VAR)); // writing NULL-element to the end
    array->elements= a;
  }
  else // array is completely empty - delete it
    delete_dynamic(array);
  status_var_array_version++;
}

/*
  Adds an array of SHOW_VAR entries to the output of SHOW STATUS

  SYNOPSIS
    add_status_vars(SHOW_VAR *list)
    list - an array of SHOW_VAR entries to add to all_status_vars
           the last entry must be {0,0,SHOW_UNDEF}

  NOTE
    The handling of all_status_vars[] is completely internal, it's allocated
    automatically when something is added to it, and deleted completely when
    the last entry is removed.

    As a special optimization, if add_status_vars() is called before
    init_status_vars(), it assumes "startup mode" - neither concurrent access
    to the array nor SHOW STATUS are possible (thus it skips locks and qsort)

    The last entry of the all_status_vars[] should always be {0,0,SHOW_UNDEF}
*/
int add_status_vars(SHOW_VAR *list)
{
  int res= 0;
  if (status_vars_inited)
    mysql_rwlock_wrlock(&LOCK_all_status_vars);
  if (!all_status_vars.buffer && // array is not allocated yet - do it now
      my_init_dynamic_array(PSI_INSTRUMENT_ME, &all_status_vars,
                            sizeof(SHOW_VAR), 250, 50, MYF(0)))
  {
    res= 1;
    goto err;
  }
  while (list->name)
    res|= insert_dynamic(&all_status_vars, (uchar*)list++);
  res|= insert_dynamic(&all_status_vars, (uchar*)list); // appending NULL-element
  all_status_vars.elements--; // but next insert_dynamic should overwite it
  if (status_vars_inited)
    sort_dynamic(&all_status_vars, show_var_cmp);
  status_var_array_version++;
err:
  if (status_vars_inited)
    mysql_rwlock_unlock(&LOCK_all_status_vars);
  return res;
}

/*
  Make all_status_vars[] usable for SHOW STATUS

  NOTE
    See add_status_vars(). Before init_status_vars() call, add_status_vars()
    works in a special fast "startup" mode. Thus init_status_vars()
    should be called as late as possible but before enabling multi-threading.
*/
void init_status_vars()
{
  status_vars_inited=1;
  sort_dynamic(&all_status_vars, show_var_cmp);
  status_var_array_version++;
}

void reset_status_vars()
{
  SHOW_VAR *ptr= (SHOW_VAR*) all_status_vars.buffer;
  SHOW_VAR *last= ptr + all_status_vars.elements;
  for (; ptr < last; ptr++)
  {
    /* Note that SHOW_LONG_NOFLUSH variables are not reset */
    if (ptr->type == SHOW_LONG)
      *(ulong*) ptr->value= 0;
  }
}

/*
  catch-all cleanup function, cleans up everything no matter what

  DESCRIPTION
    This function is not strictly required if all add_status_vars/
    remove_status_vars are properly paired, but it's a safety measure that
    deletes everything from the all_status_vars[] even if some
    remove_status_vars were forgotten
*/
void free_status_vars()
{
  delete_dynamic(&all_status_vars);
  status_var_array_version++;
}

/*
  Removes an array of SHOW_VAR entries from the output of SHOW STATUS

  SYNOPSIS
    remove_status_vars(SHOW_VAR *list)
    list - an array of SHOW_VAR entries to remove to all_status_vars
           the last entry must be {0,0,SHOW_UNDEF}

  NOTE
    there's lots of room for optimizing this, especially in non-sorted mode,
    but nobody cares - it may be called only in case of failed plugin
    initialization in the mysqld startup.
*/

void remove_status_vars(SHOW_VAR *list)
{
  if (status_vars_inited)
  {
    mysql_rwlock_wrlock(&LOCK_all_status_vars);
    SHOW_VAR *all= dynamic_element(&all_status_vars, 0, SHOW_VAR *);

    for (; list->name; list++)
    {
      int first= 0, last= ((int) all_status_vars.elements) - 1;
      for ( ; first <= last; )
      {
        int res, middle= (first + last) / 2;
        if ((res= show_var_cmp(list, all + middle)) < 0)
          last= middle - 1;
        else if (res > 0)
          first= middle + 1;
        else
        {
          all[middle].type= SHOW_UNDEF;
          break;
        }
      }
    }
    shrink_var_array(&all_status_vars);
    mysql_rwlock_unlock(&LOCK_all_status_vars);
  }
  else
  {
    SHOW_VAR *all= dynamic_element(&all_status_vars, 0, SHOW_VAR *);
    uint i;
    for (; list->name; list++)
    {
      for (i= 0; i < all_status_vars.elements; i++)
      {
        if (show_var_cmp(list, all+i))
          continue;
        all[i].type= SHOW_UNDEF;
        break;
      }
    }
    shrink_var_array(&all_status_vars);
  }
}

/* Current version of the all_status_vars.  */
ulonglong get_status_vars_version(void)
{
  return status_var_array_version;
}

/**
  A union holding a pointer to a type that can be referred by a status variable.
 */
union Any_pointer {
  const void *as_void;
  const uchar *as_uchar;
  const char *as_char;
  const char ** as_charptr;
  const double *as_double;
  const int * as_int;
  const uint * as_uint;
  const long *as_long;
  const longlong *as_longlong;
  const bool *as_bool;
  const my_bool *as_my_bool;
  const sys_var *as_sys_var;
  const system_status_var *as_system_status_var;
  const ha_rows *as_ha_rows;
  const LEX_STRING *as_lex_cstring;
  const SHOW_COMP_OPTION *as_show_comp_options;
  intptr as_intptr;
  Atomic_counter<uint32_t>* as_atomic_counter;
};

/**
  @brief Returns the value of a system or a status variable.

  @param thd         [in]     The handle of the current THD.
  @param variable    [in]     Details of the variable.
  @param value_type  [in]     Variable type.
  @param show_type   [in]     Variable show type.
  @param charset     [out]    Character set of the value.
  @param buff        [in,out] Buffer to store the value.
                              (Needs to have enough memory
                              to hold the value of variable.)
  @param length      [out]    Length of the value.

  @return                     Pointer to the value buffer.
*/

const char* get_one_variable(THD *thd,
                             const SHOW_VAR *variable,
                             enum_var_type value_type, SHOW_TYPE show_type,
                             system_status_var *status_var,
                             const CHARSET_INFO **charset, char *buff,
                             size_t *length)
{
  Any_pointer value, status_var_value;
  value.as_void= variable->value;
  status_var_value.as_system_status_var= status_var;
  const char *pos= buff;
  const char *end= buff;


  if (show_type == SHOW_SYS)
  {
    const sys_var *var= value.as_sys_var;
    show_type= var->show_type();
    value.as_uchar= var->value_ptr(thd, value_type, &null_clex_str);
    *charset= var->charset(thd);
  }

  /*
    note that value may be == buff. All SHOW_xxx code below
    should still work in this case
  */
  switch (show_type) {
  case SHOW_DOUBLE_STATUS:
    value.as_char= status_var_value.as_char + value.as_intptr;
    /* fall through */
  case SHOW_DOUBLE:
    /* 6 is the default precision for '%f' in sprintf() */
    end= buff + my_fcvt(*value.as_double, 6, buff, NULL);
    break;
  case SHOW_LONG_STATUS:
    value.as_char= status_var_value.as_char + value.as_intptr;
    /* fall through */
  case SHOW_ULONG:
  case SHOW_LONG_NOFLUSH: // the difference lies in refresh_status()
#ifndef _WIN64
  case SHOW_SIZE_T:
#endif
    end= int10_to_str(*value.as_long, buff, 10);
    break;
  case SHOW_LONGLONG_STATUS:
    value.as_char= status_var_value.as_char + value.as_intptr;
    /* fall through */
  case SHOW_ULONGLONG:
#ifdef _WIN64
  case SHOW_SIZE_T:
#endif
    end= longlong10_to_str(*value.as_longlong, buff, 10);
    break;
  case SHOW_HA_ROWS:
    end= longlong10_to_str((longlong) *value.as_ha_rows, buff, 10);
    break;
  case SHOW_BOOL:
    end= strmov(buff, *value.as_bool ? "ON" : "OFF");
    break;
  case SHOW_MY_BOOL:
    end= strmov(buff, *value.as_my_bool ? "ON" : "OFF");
    break;
  case SHOW_UINT32_STATUS:
    value.as_char= status_var_value.as_char + value.as_intptr;
    /* fall through */
  case SHOW_UINT:
    end= int10_to_str((long) *value.as_uint, buff, 10);
    break;
  case SHOW_SINT:
    end= int10_to_str((long) *value.as_int, buff, -10);
    break;
  case SHOW_SLONG:
    end= int10_to_str(*value.as_long, buff, -10);
    break;
  case SHOW_SLONGLONG:
    end= longlong10_to_str(*value.as_longlong, buff, -10);
    break;
  case SHOW_HAVE:
    {
      pos= show_comp_option_name[(int) *value.as_show_comp_options];
      end= strend(pos);
      break;
    }
  case SHOW_CHAR:
    {
      if (!(pos= value.as_char))
        pos= "";
      end= strend(pos);
      break;
    }
  case SHOW_CHAR_PTR:
    {
      if (!(pos= *value.as_charptr))
        pos= "";

      end= strend(pos);
      break;
    }
  case SHOW_LEX_STRING:
    {
      if (!(pos= value.as_lex_cstring->str))
        end= pos= "";
      else
        end= pos + value.as_lex_cstring->length;
      break;
    }
  case SHOW_ATOMIC_COUNTER_UINT32_T:
    end= int10_to_str(static_cast<long>(*value.as_atomic_counter), buff, 10);
    break;
  case SHOW_UNDEF:
    break;                                        // Return empty string
  case SHOW_SYS:                                  // Cannot happen
  default:
    DBUG_ASSERT(0);
    break;
  }

  *length= (size_t) (end - pos);
  return pos;
}


static bool show_status_array(THD *thd, const char *wild,
                              SHOW_VAR *variables,
                              enum enum_var_type scope,
                              struct system_status_var *status_var,
                              const char *prefix, TABLE *table,
                              bool ucase_names,
                              COND *cond)
{
  my_aligned_storage<SHOW_VAR_FUNC_BUFF_SIZE, MY_ALIGNOF(long)> buffer;
  char * const buff= buffer.data;
  char *prefix_end;
  char name_buffer[NAME_CHAR_LEN];
  int len;
  SHOW_VAR tmp, *var;
  bool res= FALSE;
  CHARSET_INFO *charset= system_charset_info;
  DBUG_ENTER("show_status_array");

  prefix_end=strnmov(name_buffer, prefix, sizeof(name_buffer)-1);
  if (*prefix)
    *prefix_end++= '_';
  len=(int)(name_buffer + sizeof(name_buffer) - prefix_end);

#ifdef WITH_WSREP
  bool is_wsrep_var= FALSE;
  /*
    This is a workaround for lp:1306875 (PBX) to skip switching of wsrep
    status variable name's first letter to uppercase. This is an optimization
    for status variables defined under wsrep plugin.
    TODO: remove once lp:1306875 has been addressed.
  */
  if (*prefix && !my_strcasecmp(system_charset_info, prefix, "wsrep"))
  {
    is_wsrep_var= TRUE;
  }
#endif /* WITH_WSREP */

  for (; variables->name; variables++)
  {
    bool wild_checked= false;
    strnmov(prefix_end, variables->name, len);
    name_buffer[sizeof(name_buffer)-1]=0;       /* Safety */

#ifdef WITH_WSREP
    /*
      If the prefix is NULL, that means we are looking into the status variables
      defined directly under mysqld.cc. Do not capitalize wsrep status variable
      names until lp:1306875 has been fixed.
      TODO: remove once lp:1306875 has been addressed.
     */
    if (!(*prefix) && !strncasecmp(name_buffer, "wsrep", strlen("wsrep")))
    {
      is_wsrep_var= TRUE;
    }
#endif /* WITH_WSREP */

    if (ucase_names)
      my_caseup_str(system_charset_info, name_buffer);
    else
    {
      my_casedn_str(system_charset_info, name_buffer);
      DBUG_ASSERT(name_buffer[0] >= 'a');
      DBUG_ASSERT(name_buffer[0] <= 'z');

      // WSREP_TODO: remove once lp:1306875 has been addressed.
      if (IF_WSREP(is_wsrep_var == FALSE, 1) &&
          status_var)
        name_buffer[0]-= 'a' - 'A';
    }


    restore_record(table, s->default_values);
    table->field[0]->store(name_buffer, strlen(name_buffer),
                           system_charset_info);

    /*
      Compare name for types that can't return arrays. We do this to not
      calculate the value for function variables that we will not access
    */
    if ((variables->type != SHOW_FUNC && variables->type != SHOW_ARRAY))
    {
      if (wild && wild[0] && wild_case_compare(system_charset_info,
                                               name_buffer, wild))
        continue;
      wild_checked= 1;                          // Avoid checking it again
    }

    /*
      if var->type is SHOW_FUNC or SHOW_SIMPLE_FUNC, call the function.
      Repeat as necessary, if new var is again one of the above
    */
    for (var=variables; var->type == SHOW_FUNC ||
           var->type == SHOW_SIMPLE_FUNC; var= &tmp)
      ((mysql_show_var_func)(var->value))(thd, &tmp, buff,
                                          status_var, scope);
    
    SHOW_TYPE show_type=var->type;
    if (show_type == SHOW_ARRAY)
    {
      show_status_array(thd, wild, (SHOW_VAR *) var->value, scope,
                        status_var, name_buffer, table, ucase_names, cond);
    }
    else
    {
      if ((wild_checked ||
           !(wild && wild[0] && wild_case_compare(system_charset_info,
                                                  name_buffer, wild))) &&
          (!cond || cond->val_int()))
      {
        const char *pos;                  // We assign a lot of const's
        size_t length;

        if (show_type == SHOW_SYS)
          mysql_mutex_lock(&LOCK_global_system_variables);
        else if (show_type >= SHOW_LONG_STATUS && scope == OPT_GLOBAL)
          calc_sum_of_all_status_if_needed(status_var);

        pos= get_one_variable(thd, var, scope, show_type, status_var,
                              &charset, buff, &length);

        if (table->field[1]->field_length)
          thd->count_cuted_fields= CHECK_FIELD_WARN;
        table->field[1]->store(pos, (uint32) length, charset);
        thd->count_cuted_fields= CHECK_FIELD_IGNORE;
        table->field[1]->set_notnull();
        if (show_type == SHOW_SYS)
          mysql_mutex_unlock(&LOCK_global_system_variables);


        if (schema_table_store_record(thd, table))
        {
          res= TRUE;
          goto end;
        }
        thd->get_stmt_da()->inc_current_row_for_warning();
      }
    }
  }
end:
  DBUG_RETURN(res);
}

/*
  collect status for all running threads
  Return number of threads used
*/

struct calc_sum_callback_arg
{
  calc_sum_callback_arg(STATUS_VAR *to_arg): to(to_arg), count(0) {}
  STATUS_VAR *to;
  uint count;
};


static my_bool calc_sum_callback(THD *thd, calc_sum_callback_arg *arg)
{
  arg->count++;
  if (!thd->status_in_global)
  {
    add_to_status(arg->to, &thd->status_var);
    arg->to->local_memory_used+= thd->status_var.local_memory_used;
  }
  if (thd->get_command() != COM_SLEEP)
    arg->to->threads_running++;
  return 0;
}


uint calc_sum_of_all_status(STATUS_VAR *to)
{
  calc_sum_callback_arg arg(to);
  DBUG_ENTER("calc_sum_of_all_status");

  to->local_memory_used= 0;
  /* Add to this status from existing threads */
  server_threads.iterate(calc_sum_callback, &arg);
  DBUG_RETURN(arg.count);
}


/* This is only used internally, but we need it here as a forward reference */
extern ST_SCHEMA_TABLE schema_tables[];

/*
  Store record to I_S table, convert HEAP table
  to MyISAM if necessary

  SYNOPSIS
    schema_table_store_record()
    thd                   thread handler
    table                 Information schema table to be updated

  RETURN
    0	                  success
    1	                  error
*/

bool schema_table_store_record(THD *thd, TABLE *table)
{
  int error;

  if (unlikely(thd->killed))
  {
    thd->send_kill_message();
    return 1;
  }

  if (unlikely((error= table->file->ha_write_tmp_row(table->record[0]))))
  {
    TMP_TABLE_PARAM *param= table->pos_in_table_list->schema_table_param;
    if (unlikely(create_internal_tmp_table_from_heap(thd, table,
                                                     param->start_recinfo,
                                                     &param->recinfo, error, 0,
                                                     NULL)))

      return 1;
  }
  return 0;
}


static int make_table_list(THD *thd, SELECT_LEX *sel,
                           LEX_CSTRING *db_name, LEX_CSTRING *table_name)
{
  Table_ident *table_ident;
  table_ident= new Table_ident(thd, db_name, table_name, 1);
  if (!sel->add_table_to_list(thd, table_ident, 0, 0, TL_READ, MDL_SHARED_READ))
    return 1;
  return 0;
}


/**
  @brief    Get lookup value from the part of 'WHERE' condition

  @details This function gets lookup value from
           the part of 'WHERE' condition if it's possible and
           fill appropriate lookup_field_vals struct field
           with this value.

  @param[in]      thd                   thread handler
  @param[in]      item_func             part of WHERE condition
  @param[in]      table                 I_S table
  @param[in, out] lookup_field_vals     Struct which holds lookup values

  @return
    0             success
    1             error, there can be no matching records for the condition
*/

bool get_lookup_value(THD *thd, Item_func *item_func,
                      TABLE_LIST *table,
                      LOOKUP_FIELD_VALUES *lookup_field_vals)
{
  ST_SCHEMA_TABLE *schema_table= table->schema_table;
  ST_FIELD_INFO *field_info= schema_table->fields_info;
  const char *field_name1= schema_table->idx_field1 >= 0 ?
    field_info[schema_table->idx_field1].name().str : "";
  const char *field_name2= schema_table->idx_field2 >= 0 ?
    field_info[schema_table->idx_field2].name().str : "";

  if (item_func->functype() == Item_func::EQ_FUNC ||
      item_func->functype() == Item_func::EQUAL_FUNC)
  {
    int idx_field, idx_val;
    char tmp[MAX_FIELD_WIDTH];
    String *tmp_str, str_buff(tmp, sizeof(tmp), system_charset_info);
    Item_field *item_field;
    CHARSET_INFO *cs= system_charset_info;

    if (item_func->arguments()[0]->real_item()->type() == Item::FIELD_ITEM &&
        item_func->arguments()[1]->const_item())
    {
      idx_field= 0;
      idx_val= 1;
    }
    else if (item_func->arguments()[1]->real_item()->type() == Item::FIELD_ITEM &&
             item_func->arguments()[0]->const_item())
    {
      idx_field= 1;
      idx_val= 0;
    }
    else
      return 0;

    item_field= (Item_field*) item_func->arguments()[idx_field]->real_item();
    if (table->table != item_field->field->table)
      return 0;
    tmp_str= item_func->arguments()[idx_val]->val_str(&str_buff);

    /* impossible value */
    if (!tmp_str)
      return 1;

    /* Lookup value is database name */
    if (!cs->strnncollsp(field_name1, strlen(field_name1),
                         item_field->field_name.str,
                         item_field->field_name.length))
    {
      thd->make_lex_string(&lookup_field_vals->db_value,
                           tmp_str->ptr(), tmp_str->length());
    }
    /* Lookup value is table name */
    else if (!cs->strnncollsp(field_name2,
                              strlen(field_name2),
                              item_field->field_name.str,
                              item_field->field_name.length))
    {
      thd->make_lex_string(&lookup_field_vals->table_value,
                           tmp_str->ptr(), tmp_str->length());
    }
  }
  return 0;
}


/**
  @brief    Calculates lookup values from 'WHERE' condition

  @details This function calculates lookup value(database name, table name)
           from 'WHERE' condition if it's possible and
           fill lookup_field_vals struct fields with these values.

  @param[in]      thd                   thread handler
  @param[in]      cond                  WHERE condition
  @param[in]      table                 I_S table
  @param[in, out] lookup_field_vals     Struct which holds lookup values

  @return
    0             success
    1             error, there can be no matching records for the condition
*/

bool calc_lookup_values_from_cond(THD *thd, COND *cond, TABLE_LIST *table,
                                  LOOKUP_FIELD_VALUES *lookup_field_vals)
{
  if (!cond)
    return 0;

  if (cond->type() == Item::COND_ITEM)
  {
    if (((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
    {
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item= li++))
      {
        if (item->type() == Item::FUNC_ITEM)
        {
          if (get_lookup_value(thd, (Item_func*)item, table, lookup_field_vals))
            return 1;
        }
        else
        {
          if (calc_lookup_values_from_cond(thd, item, table, lookup_field_vals))
            return 1;
        }
      }
    }
    return 0;
  }
  else if (cond->type() == Item::FUNC_ITEM &&
           get_lookup_value(thd, (Item_func*) cond, table, lookup_field_vals))
    return 1;
  return 0;
}


bool uses_only_table_name_fields(Item *item, TABLE_LIST *table)
{
  if (item->type() == Item::FUNC_ITEM)
  {
    Item_func *item_func= (Item_func*)item;
    for (uint i=0; i<item_func->argument_count(); i++)
    {
      if (!uses_only_table_name_fields(item_func->arguments()[i], table))
        return 0;
    }
  }
  else if (item->type() == Item::ROW_ITEM)
  {
    Item_row *item_row= static_cast<Item_row*>(item);
    for (uint i= 0; i < item_row->cols(); i++)
    {
      if (!uses_only_table_name_fields(item_row->element_index(i), table))
        return 0;
    }
  }
  else if (item->type() == Item::FIELD_ITEM)
  {
    Item_field *item_field= (Item_field*)item;
    CHARSET_INFO *cs= system_charset_info;
    ST_SCHEMA_TABLE *schema_table= table->schema_table;
    ST_FIELD_INFO *field_info= schema_table->fields_info;
    const char *field_name1= schema_table->idx_field1 >= 0 ?
      field_info[schema_table->idx_field1].name().str : "";
    const char *field_name2= schema_table->idx_field2 >= 0 ?
      field_info[schema_table->idx_field2].name().str : "";
    if (table->table != item_field->field->table ||
        (cs->strnncollsp(field_name1, strlen(field_name1),
                         item_field->field_name.str,
                         item_field->field_name.length) &&
         cs->strnncollsp(field_name2, strlen(field_name2),
                         item_field->field_name.str,
                         item_field->field_name.length)))
      return 0;
  }
  else if (item->type() == Item::EXPR_CACHE_ITEM)
  {
    Item_cache_wrapper *tmp= static_cast<Item_cache_wrapper*>(item);
    return uses_only_table_name_fields(tmp->get_orig_item(), table);
  }
  else if (item->type() == Item::REF_ITEM)
    return uses_only_table_name_fields(item->real_item(), table);

  if (item->real_type() == Item::SUBSELECT_ITEM && !item->const_item())
    return 0;

  return 1;
}


COND *make_cond_for_info_schema(THD *thd, COND *cond, TABLE_LIST *table)
{
  if (!cond)
    return (COND*) 0;
  if (cond->type() == Item::COND_ITEM)
  {
    if (((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
    {
      /* Create new top level AND item */
      Item_cond_and *new_cond=new (thd->mem_root) Item_cond_and(thd);
      if (!new_cond)
	return (COND*) 0;
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item=li++))
      {
	Item *fix= make_cond_for_info_schema(thd, item, table);
	if (fix)
	  new_cond->argument_list()->push_back(fix, thd->mem_root);
      }
      switch (new_cond->argument_list()->elements) {
      case 0:
	return (COND*) 0;
      case 1:
	return new_cond->argument_list()->head();
      default:
	new_cond->quick_fix_field();
	return new_cond;
      }
    }
    else
    {						// Or list
      Item_cond_or *new_cond= new (thd->mem_root) Item_cond_or(thd);
      if (!new_cond)
	return (COND*) 0;
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item=li++))
      {
	Item *fix=make_cond_for_info_schema(thd, item, table);
	if (!fix)
	  return (COND*) 0;
	new_cond->argument_list()->push_back(fix, thd->mem_root);
      }
      new_cond->quick_fix_field();
      new_cond->top_level_item();
      return new_cond;
    }
  }

  if (!uses_only_table_name_fields(cond, table))
    return (COND*) 0;
  return cond;
}


/**
  @brief   Calculate lookup values(database name, table name)

  @details This function calculates lookup values(database name, table name)
           from 'WHERE' condition or wild values (for 'SHOW' commands only)
           from LEX struct and fill lookup_field_vals struct field
           with these values.

  @param[in]      thd                   thread handler
  @param[in]      cond                  WHERE condition
  @param[in]      tables                I_S table
  @param[in, out] lookup_field_values   Struct which holds lookup values

  @return
    0             success
    1             error, there can be no matching records for the condition
*/

bool get_lookup_field_values(THD *thd, COND *cond, TABLE_LIST *tables,
                             LOOKUP_FIELD_VALUES *lookup_field_values)
{
  LEX *lex= thd->lex;
  String *wild= lex->wild;
  bool rc= 0;

  bzero((char*) lookup_field_values, sizeof(LOOKUP_FIELD_VALUES));
  switch (lex->sql_command) {
  case SQLCOM_SHOW_PLUGINS:
    if (lex->ident.str)
    {
      thd->make_lex_string(&lookup_field_values->db_value,
                           lex->ident.str, lex->ident.length);
      break;
    }
    /* fall through */
  case SQLCOM_SHOW_GENERIC:
  case SQLCOM_SHOW_DATABASES:
    if (wild)
    {
      thd->make_lex_string(&lookup_field_values->db_value,
                           wild->ptr(), wild->length());
      lookup_field_values->wild_db_value= 1;
    }
    break;
  case SQLCOM_SHOW_TABLES:
  case SQLCOM_SHOW_TABLE_STATUS:
  case SQLCOM_SHOW_TRIGGERS:
  case SQLCOM_SHOW_EVENTS:
    thd->make_lex_string(&lookup_field_values->db_value,
                         lex->first_select_lex()->db.str,
                         lex->first_select_lex()->db.length);
    if (wild)
    {
      thd->make_lex_string(&lookup_field_values->table_value, 
                           wild->ptr(), wild->length());
      lookup_field_values->wild_table_value= 1;
    }
    break;
  default:
    /*
      The "default" is for queries over I_S.
      All previous cases handle SHOW commands.
    */
    rc= calc_lookup_values_from_cond(thd, cond, tables, lookup_field_values);
    break;
  }

  if (lower_case_table_names && !rc)
  {
    /* 
      We can safely do in-place upgrades here since all of the above cases
      are allocating a new memory buffer for these strings.
    */  
    if (lookup_field_values->db_value.str && lookup_field_values->db_value.str[0])
      my_casedn_str(system_charset_info,
                    (char*) lookup_field_values->db_value.str);
    if (lookup_field_values->table_value.str && 
        lookup_field_values->table_value.str[0])
      my_casedn_str(system_charset_info,
                    (char*) lookup_field_values->table_value.str);
  }

  return rc;
}


enum enum_schema_tables get_schema_table_idx(ST_SCHEMA_TABLE *schema_table)
{
  return (enum enum_schema_tables) (schema_table - &schema_tables[0]);
}


/*
  Create db names list. Information schema name always is first in list

  SYNOPSIS
    make_db_list()
    thd                   thread handler
    files                 list of db names
    wild                  wild string
    idx_field_vals        idx_field_vals->db_name contains db name or
                          wild string

  RETURN
    zero                  success
    non-zero              error
*/

static int make_db_list(THD *thd, Dynamic_array<LEX_CSTRING*> *files,
                        LOOKUP_FIELD_VALUES *lookup_field_vals)
{
  if (lookup_field_vals->wild_db_value)
  {
    /*
      This part of code is only for SHOW DATABASES command.
      idx_field_vals->db_value can be 0 when we don't use
      LIKE clause (see also get_index_field_values() function)
    */
    if (!lookup_field_vals->db_value.str ||
        !wild_case_compare(system_charset_info,
                           INFORMATION_SCHEMA_NAME.str,
                           lookup_field_vals->db_value.str))
    {
      if (files->append_val(&INFORMATION_SCHEMA_NAME))
        return 1;
    }
    return find_files(thd, files, 0, mysql_data_home,
                      &lookup_field_vals->db_value);
  }


  /*
    If we have db lookup value we just add it to list and
    exit from the function.
    We don't do this for database names longer than the maximum
    name length.
  */
  if (lookup_field_vals->db_value.str)
  {
    if (lookup_field_vals->db_value.length > NAME_LEN)
    {
      /*
        Impossible value for a database name,
        found in a WHERE DATABASE_NAME = 'xxx' clause.
      */
      return 0;
    }

    if (is_infoschema_db(&lookup_field_vals->db_value))
    {
      if (files->append_val(&INFORMATION_SCHEMA_NAME))
        return 1;
      return 0;
    }
    if (files->append_val(&lookup_field_vals->db_value))
      return 1;
    return 0;
  }

  /*
    Create list of existing databases. It is used in case
    of select from information schema table
  */
  if (files->append_val(&INFORMATION_SCHEMA_NAME))
    return 1;
  return find_files(thd, files, 0, mysql_data_home, &null_clex_str);
}


struct st_add_schema_table
{
  Dynamic_array<LEX_CSTRING*> *files;
  const char *wild;
};


static my_bool add_schema_table(THD *thd, plugin_ref plugin,
                                void* p_data)
{
  LEX_CSTRING *file_name= 0;
  st_add_schema_table *data= (st_add_schema_table *)p_data;
  Dynamic_array<LEX_CSTRING*> *file_list= data->files;
  const char *wild= data->wild;
  ST_SCHEMA_TABLE *schema_table= plugin_data(plugin, ST_SCHEMA_TABLE *);
  DBUG_ENTER("add_schema_table");

  if (schema_table->hidden)
      DBUG_RETURN(0);
  if (wild)
  {
    if (lower_case_table_names)
    {
      if (wild_case_compare(files_charset_info,
                            schema_table->table_name,
                            wild))
        DBUG_RETURN(0);
    }
    else if (wild_compare(schema_table->table_name, wild, 0))
      DBUG_RETURN(0);
  }

  if ((file_name= thd->make_clex_string(schema_table->table_name,
                                        strlen(schema_table->table_name))) &&
      !file_list->append(file_name))
    DBUG_RETURN(0);
  DBUG_RETURN(1);
}


int schema_tables_add(THD *thd, Dynamic_array<LEX_CSTRING*> *files,
                      const char *wild)
{
  LEX_CSTRING *file_name;
  ST_SCHEMA_TABLE *tmp_schema_table= schema_tables;
  st_add_schema_table add_data;
  DBUG_ENTER("schema_tables_add");

  for (; tmp_schema_table->table_name; tmp_schema_table++)
  {
    if (tmp_schema_table->hidden)
      continue;
    if (wild)
    {
      if (lower_case_table_names)
      {
        if (wild_case_compare(files_charset_info,
                              tmp_schema_table->table_name,
                              wild))
          continue;
      }
      else if (wild_compare(tmp_schema_table->table_name, wild, 0))
        continue;
    }
    if ((file_name=
         thd->make_clex_string(tmp_schema_table->table_name,
                               strlen(tmp_schema_table->table_name))) &&
        !files->append(file_name))
      continue;
    DBUG_RETURN(1);
  }

  add_data.files= files;
  add_data.wild= wild;
  if (plugin_foreach(thd, add_schema_table,
                     MYSQL_INFORMATION_SCHEMA_PLUGIN, &add_data))
      DBUG_RETURN(1);

  DBUG_RETURN(0);
}


/**
  @brief          Create table names list

  @details        The function creates the list of table names in
                  database

  @param[in]      thd                   thread handler
  @param[in]      table_names           List of table names in database
  @param[in]      lex                   pointer to LEX struct
  @param[in]      lookup_field_vals     pointer to LOOKUP_FIELD_VALUE struct
  @param[in]      db_name               database name

  @return         Operation status
    @retval       0           ok
    @retval       1           fatal error
    @retval       2           Not fatal error; Safe to ignore this file list
*/

static int
make_table_name_list(THD *thd, Dynamic_array<LEX_CSTRING*> *table_names,
                     LEX *lex, LOOKUP_FIELD_VALUES *lookup_field_vals,
                     LEX_CSTRING *db_name)
{
  char path[FN_REFLEN + 1];
  build_table_filename(path, sizeof(path) - 1, db_name->str, "", "", 0);
  if (!lookup_field_vals->wild_table_value &&
      lookup_field_vals->table_value.str)
  {
    if (check_table_name(lookup_field_vals->table_value.str,
                         lookup_field_vals->table_value.length,
                         false))
    {
      /*
        Impossible value for a table name,
        found in a WHERE TABLE_NAME = 'xxx' clause.
      */
      return 0;
    }
    if (db_name == &INFORMATION_SCHEMA_NAME)
    {
      LEX_CSTRING *name;
      ST_SCHEMA_TABLE *schema_table=
        find_schema_table(thd, &lookup_field_vals->table_value);
      if (schema_table && !schema_table->hidden)
      {
        if (!(name= thd->make_clex_string(schema_table->table_name,
                                          strlen(schema_table->table_name))) ||
            table_names->append(name))
          return 1;
      }
    }
    else
    {
      if (table_names->append_val(&lookup_field_vals->table_value))
        return 1;
    }
    return 0;
  }

  /*
    This call will add all matching the wildcards (if specified) IS tables
    to the list
  */
  if (db_name == &INFORMATION_SCHEMA_NAME)
    return (schema_tables_add(thd, table_names,
                              lookup_field_vals->table_value.str));

  if (check_db_name((LEX_STRING*)db_name))
    return 0; // Impossible TABLE_SCHEMA name

  find_files_result res= find_files(thd, table_names, db_name, path,
                                    &lookup_field_vals->table_value);
  if (res != FIND_FILES_OK)
  {
    /*
      Downgrade errors about problems with database directory to
      warnings if this is not a 'SHOW' command.  Another thread
      may have dropped database, and we may still have a name
      for that directory.
    */
    if (res == FIND_FILES_DIR)
    {
      if (is_show_command(thd))
        return 1;
      thd->clear_error();
      return 2;
    }
    return 1;
  }
  return 0;
}


static void get_table_engine_for_i_s(THD *thd, char *buf, TABLE_LIST *tl,
                                     LEX_CSTRING *db, LEX_CSTRING *table)
{
  LEX_CSTRING engine_name= { buf, 0 };

  if (thd->get_stmt_da()->sql_errno() == ER_UNKNOWN_STORAGE_ENGINE)
  {
    char path[FN_REFLEN];
    build_table_filename(path, sizeof(path) - 1,
                         db->str, table->str, reg_ext, 0);
    if (dd_frm_type(thd, path, &engine_name, NULL, NULL) == TABLE_TYPE_NORMAL)
      tl->option= engine_name.str;
  }
}


/**
  Fill I_S table with data obtained by performing full-blown table open.

  @param  thd                       Thread handler.
  @param  is_show_fields_or_keys    Indicates whether it is a legacy SHOW
                                    COLUMNS or SHOW KEYS statement.
  @param  table                     TABLE object for I_S table to be filled.
  @param  schema_table              I_S table description structure.
  @param  orig_db_name              Database name.
  @param  orig_table_name           Table name.
  @param  open_tables_state_backup  Open_tables_state object which is used
                                    to save/restore original status of
                                    variables related to open tables state.
  @param  can_deadlock              Indicates that deadlocks are possible
                                    due to metadata locks, so to avoid
                                    them we should not wait in case if
                                    conflicting lock is present.

  @retval FALSE - Success.
  @retval TRUE  - Failure.
*/
static bool
fill_schema_table_by_open(THD *thd, MEM_ROOT *mem_root,
                          bool is_show_fields_or_keys,
                          TABLE *table, ST_SCHEMA_TABLE *schema_table,
                          LEX_CSTRING *orig_db_name,
                          LEX_CSTRING *orig_table_name,
                          Open_tables_backup *open_tables_state_backup,
                          bool can_deadlock)
{
  Query_arena i_s_arena(mem_root, Query_arena::STMT_CONVENTIONAL_EXECUTION),
              backup_arena, *old_arena;
  LEX *old_lex= thd->lex, temp_lex, *lex;
  LEX_CSTRING db_name, table_name;
  TABLE_LIST *table_list;
  bool result= true;
  DBUG_ENTER("fill_schema_table_by_open");

  /*
    When a view is opened its structures are allocated on a permanent
    statement arena and linked into the LEX tree for the current statement
    (this happens even in cases when view is handled through TEMPTABLE
    algorithm).

    To prevent this process from unnecessary hogging of memory in the permanent
    arena of our I_S query and to avoid damaging its LEX we use temporary
    arena and LEX for table/view opening.

    Use temporary arena instead of statement permanent arena. Also make
    it active arena and save original one for successive restoring.
  */
  old_arena= thd->stmt_arena;
  thd->stmt_arena= &i_s_arena;
  thd->set_n_backup_active_arena(&i_s_arena, &backup_arena);

  /* Prepare temporary LEX. */
  thd->lex= lex= &temp_lex;
  lex_start(thd);
  lex->sql_command= old_lex->sql_command;

  /* Disable constant subquery evaluation as we won't be locking tables. */
  lex->context_analysis_only= CONTEXT_ANALYSIS_ONLY_VIEW;

  /*
    Some of process_table() functions rely on wildcard being passed from
    old LEX (or at least being initialized).
  */
  lex->wild= old_lex->wild;

  /*
    Since make_table_list() might change database and table name passed
    to it (if lower_case_table_names) we create copies of orig_db_name and
    orig_table_name here.  These copies are used for make_table_list()
    while unaltered values are passed to process_table() functions.
  */
  if (!thd->make_lex_string(&db_name,
                            orig_db_name->str, orig_db_name->length) ||
      !thd->make_lex_string(&table_name,
                            orig_table_name->str, orig_table_name->length))
    goto end;

  /*
    Create table list element for table to be open. Link it with the
    temporary LEX. The latter is required to correctly open views and
    produce table describing their structure.
  */
  if (make_table_list(thd, lex->first_select_lex(), &db_name, &table_name))
    goto end;

  table_list= lex->first_select_lex()->table_list.first;

  if (is_show_fields_or_keys)
  {
    /*
      Restore thd->temporary_tables to be able to process
      temporary tables (only for 'show index' & 'show columns').
      This should be changed when processing of temporary tables for
      I_S tables will be done.
    */
    thd->temporary_tables= open_tables_state_backup->temporary_tables;
  }
  else
  {
    /*
      Apply optimization flags for table opening which are relevant for
      this I_S table. We can't do this for SHOW COLUMNS/KEYS because of
      backward compatibility.
    */
    table_list->i_s_requested_object= schema_table->i_s_requested_object;
  }

  DBUG_ASSERT(thd->lex == lex);
  result= open_tables_only_view_structure(thd, table_list, can_deadlock);

  DEBUG_SYNC(thd, "after_open_table_ignore_flush");

  /*
    XXX:  show_table_list has a flag i_is_requested,
    and when it's set, open_normal_and_derived_tables()
    can return an error without setting an error message
    in THD, which is a hack. This is why we have to
    check for res, then for thd->is_error() and only then
    for thd->main_da.sql_errno().

    Again we don't do this for SHOW COLUMNS/KEYS because
    of backward compatibility.
  */
  if (!is_show_fields_or_keys && result && thd->is_error() &&
      (thd->get_stmt_da()->sql_errno() == ER_NO_SUCH_TABLE ||
       thd->get_stmt_da()->sql_errno() == ER_WRONG_OBJECT ||
       thd->get_stmt_da()->sql_errno() == ER_NOT_SEQUENCE))
  {
    /*
      Hide error for a non-existing table.
      For example, this error can occur when we use a where condition
      with a db name and table, but the table does not exist or
      there is a view with the same name.
    */
    result= false;
    thd->clear_error();
  }
  else
  {
    char buf[NAME_CHAR_LEN + 1];
    if (unlikely(thd->is_error()))
      get_table_engine_for_i_s(thd, buf, table_list, &db_name, &table_name);

    result= schema_table->process_table(thd, table_list,
                                        table, result,
                                        orig_db_name,
                                        orig_table_name);
  }


end:
  lex->unit.cleanup();

  /* Restore original LEX value, statement's arena and THD arena values. */
  lex_end(thd->lex);

  // Free items, before restoring backup_arena below.
  DBUG_ASSERT(i_s_arena.free_list == NULL);
  thd->free_items();

  /*
    For safety reset list of open temporary tables before closing
    all tables open within this Open_tables_state.
  */
  thd->temporary_tables= NULL;

  close_thread_tables(thd);
  /*
    Release metadata lock we might have acquired.
    See comment in fill_schema_table_from_frm() for details.
  */
  thd->mdl_context.rollback_to_savepoint(open_tables_state_backup->mdl_system_tables_svp);

  thd->lex= old_lex;

  thd->stmt_arena= old_arena;
  thd->restore_active_arena(&i_s_arena, &backup_arena);

  DBUG_RETURN(result);
}


/**
  @brief          Fill I_S table for SHOW TABLE NAMES commands

  @param[in]      thd                      thread handler
  @param[in]      table                    TABLE struct for I_S table
  @param[in]      db_name                  database name
  @param[in]      table_name               table name

  @return         Operation status
    @retval       0           success
    @retval       1           error
*/

static int fill_schema_table_names(THD *thd, TABLE_LIST *tables,
                                   LEX_CSTRING *db_name,
                                   LEX_CSTRING *table_name)
{
  TABLE *table= tables->table;
  if (db_name == &INFORMATION_SCHEMA_NAME)
  {
    table->field[3]->store(STRING_WITH_LEN("SYSTEM VIEW"),
                           system_charset_info);
  }
  else if (tables->table_open_method != SKIP_OPEN_TABLE)
  {
    CHARSET_INFO *cs= system_charset_info;
    handlerton *hton;
    bool is_sequence;

    if (ha_table_exists(thd, db_name, table_name, NULL, NULL,
                        &hton, &is_sequence))
    {
      if (hton == view_pseudo_hton)
        table->field[3]->store(STRING_WITH_LEN("VIEW"), cs);
      else if (is_sequence)
        table->field[3]->store(STRING_WITH_LEN("SEQUENCE"), cs);
      else
        table->field[3]->store(STRING_WITH_LEN("BASE TABLE"), cs);
    }
    else
      table->field[3]->store(STRING_WITH_LEN("ERROR"), cs);

    if (unlikely(thd->is_error() &&
                 thd->get_stmt_da()->sql_errno() == ER_NO_SUCH_TABLE))
    {
      thd->clear_error();
      return 0;
    }
  }
  if (unlikely(schema_table_store_record(thd, table)))
    return 1;
  return 0;
}


/**
  @brief          Get open table method

  @details        The function calculates the method which will be used
                  for table opening:
                  SKIP_OPEN_TABLE - do not open table
                  OPEN_FRM_ONLY   - open FRM file only
                  OPEN_FULL_TABLE - open FRM, data, index files
  @param[in]      tables               I_S table table_list
  @param[in]      schema_table         I_S table struct
  @param[in]      schema_table_idx     I_S table index

  @return         return a set of flags
    @retval       SKIP_OPEN_TABLE | OPEN_FRM_ONLY | OPEN_FULL_TABLE
*/

uint get_table_open_method(TABLE_LIST *tables,
                                  ST_SCHEMA_TABLE *schema_table,
                                  enum enum_schema_tables schema_table_idx)
{
  /*
    determine which method will be used for table opening
  */
  if (schema_table->i_s_requested_object & OPTIMIZE_I_S_TABLE)
  {
    Field **ptr, *field;
    uint table_open_method= 0, field_indx= 0;
    uint star_table_open_method= OPEN_FULL_TABLE;
    bool used_star= true;                  // true if '*' is used in select
    for (ptr=tables->table->field; (field= *ptr) ; ptr++)
    {
      const ST_FIELD_INFO &def= schema_table->fields_info[field_indx];
      star_table_open_method=
        MY_MIN(star_table_open_method, (uint) def.open_method());
      if (bitmap_is_set(tables->table->read_set, field->field_index))
      {
        used_star= false;
        table_open_method|= (uint) def.open_method();
      }
      field_indx++;
    }
    if (used_star)
      return star_table_open_method;
    return table_open_method;
  }
  /* I_S tables which use get_all_tables but can not be optimized */
  return (uint) OPEN_FULL_TABLE;
}


/**
   Try acquire high priority share metadata lock on a table (with
   optional wait for conflicting locks to go away).

   @param thd            Thread context.
   @param mdl_request    Pointer to memory to be used for MDL_request
                         object for a lock request.
   @param table          Table list element for the table
   @param can_deadlock   Indicates that deadlocks are possible due to
                         metadata locks, so to avoid them we should not
                         wait in case if conflicting lock is present.

   @note This is an auxiliary function to be used in cases when we want to
         access table's description by looking up info in TABLE_SHARE without
         going through full-blown table open.
   @note This function assumes that there are no other metadata lock requests
         in the current metadata locking context.

   @retval FALSE  No error, if lock was obtained TABLE_LIST::mdl_request::ticket
                  is set to non-NULL value.
   @retval TRUE   Some error occurred (probably thread was killed).
*/

static bool
try_acquire_high_prio_shared_mdl_lock(THD *thd, TABLE_LIST *table,
                                      bool can_deadlock)
{
  bool error;
  MDL_REQUEST_INIT(&table->mdl_request, MDL_key::TABLE, table->db.str,
                   table->table_name.str, MDL_SHARED_HIGH_PRIO,
                   MDL_TRANSACTION);

  if (can_deadlock)
  {
    /*
      When .FRM is being open in order to get data for an I_S table,
      we might have some tables not only open but also locked.
      E.g. this happens when a SHOW or I_S statement is run
      under LOCK TABLES or inside a stored function.
      By waiting for the conflicting metadata lock to go away we
      might create a deadlock which won't entirely belong to the
      MDL subsystem and thus won't be detectable by this subsystem's
      deadlock detector. To avoid such situation, when there are
      other locked tables, we prefer not to wait on a conflicting
      lock.
    */
    error= thd->mdl_context.try_acquire_lock(&table->mdl_request);
  }
  else
    error= thd->mdl_context.acquire_lock(&table->mdl_request,
                                         thd->variables.lock_wait_timeout);

  return error;
}


/**
  @brief          Fill I_S table with data from FRM file only

  @param[in]      thd                      thread handler
  @param[in]      table                    TABLE struct for I_S table
  @param[in]      schema_table             I_S table struct
  @param[in]      db_name                  database name
  @param[in]      table_name               table name
  @param[in]      schema_table_idx         I_S table index
  @param[in]      open_tables_state_backup Open_tables_state object which is used
                                           to save/restore original state of metadata
                                           locks.
  @param[in]      can_deadlock             Indicates that deadlocks are possible
                                           due to metadata locks, so to avoid
                                           them we should not wait in case if
                                           conflicting lock is present.

  @return         Operation status
    @retval       0           Table is processed and we can continue
                              with new table
    @retval       1           It's view and we have to use
                              open_tables function for this table
*/

static int fill_schema_table_from_frm(THD *thd, TABLE *table,
                                      ST_SCHEMA_TABLE *schema_table,
                                      LEX_CSTRING *db_name,
                                      LEX_CSTRING *table_name,
                                      Open_tables_backup *open_tables_state_backup,
                                      bool can_deadlock)
{
  TABLE_SHARE *share;
  TABLE tbl;
  TABLE_LIST table_list;
  uint res= 0;
  char db_name_buff[NAME_LEN + 1], table_name_buff[NAME_LEN + 1];

  bzero((char*) &table_list, sizeof(TABLE_LIST));
  bzero((char*) &tbl, sizeof(TABLE));

  DBUG_ASSERT(db_name->length <= NAME_LEN);
  DBUG_ASSERT(table_name->length <= NAME_LEN);

  if (lower_case_table_names)
  {
    /*
      In lower_case_table_names > 0 metadata locking and table definition
      cache subsystems require normalized (lowercased) database and table
      names as input.
    */
    strmov(db_name_buff, db_name->str);
    strmov(table_name_buff, table_name->str);
    table_list.db.length=         my_casedn_str(files_charset_info, db_name_buff);
    table_list.table_name.length= my_casedn_str(files_charset_info, table_name_buff);
    table_list.db.str= db_name_buff;
    table_list.table_name.str= table_name_buff;
  }
  else
  {
    table_list.table_name= *table_name;
    table_list.db= *db_name;
  }

  /*
    TODO: investigate if in this particular situation we can get by
          simply obtaining internal lock of the data-dictionary
          instead of obtaining full-blown metadata lock.
  */
  if (try_acquire_high_prio_shared_mdl_lock(thd, &table_list, can_deadlock))
  {
    /*
      Some error occurred (most probably we have been killed while
      waiting for conflicting locks to go away), let the caller to
      handle the situation.
    */
    return 1;
  }

  if (! table_list.mdl_request.ticket)
  {
    /*
      We are in situation when we have encountered conflicting metadata
      lock and deadlocks can occur due to waiting for it to go away.
      So instead of waiting skip this table with an appropriate warning.
    */
    DBUG_ASSERT(can_deadlock);

    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WARN_I_S_SKIPPED_TABLE,
                        ER_THD(thd, ER_WARN_I_S_SKIPPED_TABLE),
                        table_list.db.str, table_list.table_name.str);
    return 0;
  }

  if (schema_table->i_s_requested_object & OPEN_TRIGGER_ONLY)
  {
    init_sql_alloc(key_memory_table_triggers_list,
                   &tbl.mem_root, TABLE_ALLOC_BLOCK_SIZE, 0, MYF(0));
    if (!Table_triggers_list::check_n_load(thd, db_name,
                                           table_name, &tbl, 1))
    {
      table_list.table= &tbl;
      res= schema_table->process_table(thd, &table_list, table,
                                       res, db_name, table_name);
      delete tbl.triggers;
    }
    free_root(&tbl.mem_root, MYF(0));
    goto end;
  }

  share= tdc_acquire_share(thd, &table_list, GTS_TABLE | GTS_VIEW);
  if (!share)
  {
    if (thd->get_stmt_da()->sql_errno() == ER_NO_SUCH_TABLE ||
        thd->get_stmt_da()->sql_errno() == ER_WRONG_OBJECT ||
        thd->get_stmt_da()->sql_errno() == ER_NOT_SEQUENCE)
    {
      res= 0;
    }
    else
    {
      char buf[NAME_CHAR_LEN + 1];
      get_table_engine_for_i_s(thd, buf, &table_list, db_name, table_name);

      res= schema_table->process_table(thd, &table_list, table,
                                       true, db_name, table_name);
    }
    goto end;
  }

  if (share->is_view)
  {
    if (schema_table->i_s_requested_object & OPEN_TABLE_ONLY)
    {
      /* skip view processing */
      res= 0;
      goto end_share;
    }
    else if (schema_table->i_s_requested_object & OPEN_VIEW_FULL)
    {
      /*
        tell get_all_tables() to fall back to
        open_normal_and_derived_tables()
      */
      res= 1;
      goto end_share;
    }

    if (mysql_make_view(thd, share, &table_list, true))
      goto end_share;
    table_list.view= (LEX*) share->is_view;
    res= schema_table->process_table(thd, &table_list, table,
                                     res, db_name, table_name);
    goto end_share;
  }

  if (!open_table_from_share(thd, share, table_name, 0,
                             (EXTRA_RECORD | OPEN_FRM_FILE_ONLY),
                             thd->open_options, &tbl, FALSE))
  {
    tbl.s= share;
    table_list.table= &tbl;
    table_list.view= (LEX*) share->is_view;
    res= schema_table->process_table(thd, &table_list, table,
                                     res, db_name, table_name);
    closefrm(&tbl);
  }


end_share:
  tdc_release_share(share);

end:
  /*
    Release metadata lock we might have acquired.

    Without this step metadata locks acquired for each table processed
    will be accumulated. In situation when a lot of tables are processed
    by I_S query this will result in transaction with too many metadata
    locks. As result performance of acquisition of new lock will suffer.

    Of course, the fact that we don't hold metadata lock on tables which
    were processed till the end of I_S query makes execution less isolated
    from concurrent DDL. Consequently one might get 'dirty' results from
    such a query. But we have never promised serializability of I_S queries
    anyway.

    We don't have any tables open since we took backup, so rolling back to
    savepoint is safe.
  */
  DBUG_ASSERT(thd->open_tables == NULL);
  thd->mdl_context.rollback_to_savepoint(open_tables_state_backup->mdl_system_tables_svp);
  if (!thd->is_fatal_error)
    thd->clear_error();
  return res;
}


class Warnings_only_error_handler : public Internal_error_handler
{
public:
  bool handle_condition(THD *thd, uint sql_errno, const char* sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg, Sql_condition ** cond_hdl)
  {
    if (sql_errno == ER_TRG_NO_DEFINER || sql_errno == ER_TRG_NO_CREATION_CTX
        || sql_errno == ER_PARSE_ERROR)
      return true;

    if (*level != Sql_condition::WARN_LEVEL_ERROR)
      return false;

    if (likely(!thd->get_stmt_da()->is_error()))
      thd->get_stmt_da()->set_error_status(sql_errno, msg, sqlstate, *cond_hdl);
    return true; // handled!
  }
};

/**
  @brief          Fill I_S tables whose data are retrieved
                  from frm files and storage engine

  @details        The information schema tables are internally represented as
                  temporary tables that are filled at query execution time.
                  Those I_S tables whose data are retrieved
                  from frm files and storage engine are filled by the function
                  get_all_tables().

  @note           This function assumes optimize_for_get_all_tables() has been
                  run for the table and produced a "read plan" in 
                  tables->is_table_read_plan.

  @param[in]      thd                      thread handler
  @param[in]      tables                   I_S table
  @param[in]      cond                     'WHERE' condition

  @return         Operation status
    @retval       0                        success
    @retval       1                        error
*/

int get_all_tables(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("get_all_tables");
  LEX *lex= thd->lex;
  TABLE *table= tables->table;
  TABLE_LIST table_acl_check;
  SELECT_LEX *lsel= tables->schema_select_lex;
  ST_SCHEMA_TABLE *schema_table= tables->schema_table;
  IS_table_read_plan *plan= tables->is_table_read_plan;
  enum enum_schema_tables schema_table_idx;
  Dynamic_array<LEX_CSTRING*> db_names(PSI_INSTRUMENT_MEM);
  Item *partial_cond= plan->partial_cond;
  int error= 1;
  Open_tables_backup open_tables_state_backup;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *sctx= thd->security_ctx;
#endif
  uint table_open_method= tables->table_open_method;
  bool can_deadlock;
  MEM_ROOT tmp_mem_root;
  /*
    We're going to open FRM files for tables.
    In case of VIEWs that contain stored function calls,
    these stored functions will be parsed and put to the SP cache.

    Suppose we have a view containing a stored function call:
      CREATE VIEW v1 AS SELECT f1() AS c1;
    and now we're running:
      SELECT * FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_NAME=f1();
    If a parallel thread invalidates the cache,
    e.g. by creating or dropping some stored routine,
    the SELECT query will re-parse f1() when processing "v1"
    and replace the outdated cached version of f1() to a new one.
    But the old version of f1() is referenced from the m_sp member
    of the Item_func_sp instances used in the WHERE condition.
    We cannot destroy it. To avoid such clashes, let's remember
    all old routines into a temporary SP cache collection
    and process tables with a new empty temporary SP cache collection.
    Then restore to the old SP cache collection at the end.
  */
  Sp_caches old_sp_caches;

  old_sp_caches.sp_caches_swap(*thd);

  bzero(&tmp_mem_root, sizeof(tmp_mem_root));

  /*
    In cases when SELECT from I_S table being filled by this call is
    part of statement which also uses other tables or is being executed
    under LOCK TABLES or is part of transaction which also uses other
    tables waiting for metadata locks which happens below might result
    in deadlocks.
    To avoid them we don't wait if conflicting metadata lock is
    encountered and skip table with emitting an appropriate warning.
  */
  can_deadlock= thd->mdl_context.has_locks();

  /*
    We should not introduce deadlocks even if we already have some
    tables open and locked, since we won't lock tables which we will
    open and will ignore pending exclusive metadata locks for these
    tables by using high-priority requests for shared metadata locks.
  */
  thd->reset_n_backup_open_tables_state(&open_tables_state_backup);

  schema_table_idx= get_schema_table_idx(schema_table);
  /* 
    this branch processes SHOW FIELDS, SHOW INDEXES commands.
    see sql_parse.cc, prepare_schema_table() function where
    this values are initialized
  */
  if (lsel && lsel->table_list.first)
  {
    error= fill_schema_table_by_open(thd, thd->mem_root, TRUE,
                                     table, schema_table,
                                     &lsel->table_list.first->db,
                                     &lsel->table_list.first->table_name,
                                     &open_tables_state_backup,
                                     can_deadlock);
    goto err;
  }

  if (plan->no_rows)
  {
    error= 0;
    goto err;
  }

  if (lex->describe)
  {
    /* EXPLAIN SELECT */
    error= 0;
    goto err;
  }

  bzero((char*) &table_acl_check, sizeof(table_acl_check));

  if (make_db_list(thd, &db_names, &plan->lookup_field_vals))
    goto err;

  /* Use tmp_mem_root to allocate data for opened tables */
  init_alloc_root(PSI_INSTRUMENT_ME, &tmp_mem_root, SHOW_ALLOC_BLOCK_SIZE,
                  SHOW_ALLOC_BLOCK_SIZE, MY_THREAD_SPECIFIC);

  for (size_t i=0; i < db_names.elements(); i++)
  {
    LEX_CSTRING *db_name= db_names.at(i);
    DBUG_ASSERT(db_name->length <= NAME_LEN);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    if (!(check_access(thd, SELECT_ACL, db_name->str,
                       &thd->col_access, NULL, 0, 1) ||
          (!thd->col_access && check_grant_db(thd, db_name->str))) ||
        sctx->master_access & (DB_ACLS | SHOW_DB_ACL) ||
        acl_get(sctx->host, sctx->ip, sctx->priv_user, db_name->str, 0))
#endif
    {
      Dynamic_array<LEX_CSTRING*> table_names(PSI_INSTRUMENT_MEM);
      int res= make_table_name_list(thd, &table_names, lex,
                                    &plan->lookup_field_vals, db_name);
      if (unlikely(res == 2))   /* Not fatal error, continue */
        continue;
      if (unlikely(res))
        goto err;

      for (size_t i=0; i < table_names.elements(); i++)
      {
        LEX_CSTRING *table_name= table_names.at(i);
        DBUG_ASSERT(table_name->length <= NAME_LEN);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
        if (!(thd->col_access & TABLE_ACLS))
        {
          table_acl_check.db= *db_name;
          table_acl_check.table_name= *table_name;
          table_acl_check.grant.privilege= thd->col_access;
          if (check_grant(thd, TABLE_ACLS, &table_acl_check, TRUE, 1, TRUE))
            continue;
        }
#endif
	restore_record(table, s->default_values);
        table->field[schema_table->idx_field1]->
          store(db_name->str, db_name->length, system_charset_info);
        table->field[schema_table->idx_field2]->
          store(table_name->str, table_name->length, system_charset_info);

        if (!partial_cond || partial_cond->val_int())
        {
          /*
            If table is I_S.tables and open_table_method is 0 (eg SKIP_OPEN)
            we can skip table opening and we don't have lookup value for
            table name or lookup value is wild string(table name list is
            already created by make_table_name_list() function).
          */
          if (!table_open_method && schema_table_idx == SCH_TABLES &&
              (!plan->lookup_field_vals.table_value.length ||
               plan->lookup_field_vals.wild_table_value))
          {
            table->field[0]->store(STRING_WITH_LEN("def"), system_charset_info);
            if (schema_table_store_record(thd, table))
              goto err;      /* Out of space in temporary table */
            continue;
          }

          /* SHOW TABLE NAMES command */
          if (schema_table_idx == SCH_TABLE_NAMES)
          {
            if (fill_schema_table_names(thd, tables, db_name, table_name))
              continue;
          }
          else if (schema_table_idx == SCH_TRIGGERS &&
                   db_name == &INFORMATION_SCHEMA_NAME)
          {
            continue;
          }
          else
          {
            if (!(table_open_method & ~OPEN_FRM_ONLY) &&
                db_name != &INFORMATION_SCHEMA_NAME)
            {
              if (!fill_schema_table_from_frm(thd, table, schema_table,
                                              db_name, table_name,
                                              &open_tables_state_backup,
                                              can_deadlock))
                continue;
            }

            if (thd->killed == ABORT_QUERY)
            {
              error= 0;
              goto err;
            }
            if (thd->is_fatal_error)
              goto err;

            DEBUG_SYNC(thd, "before_open_in_get_all_tables");
            if (fill_schema_table_by_open(thd, &tmp_mem_root, FALSE,
                                          table, schema_table,
                                          db_name, table_name,
                                          &open_tables_state_backup,
                                          can_deadlock))
              goto err;
            free_root(&tmp_mem_root, MY_MARK_BLOCKS_FREE);
          }
        }
        if (thd->killed == ABORT_QUERY)
        {
          error= 0;
          goto err;
        }
      }
    }
  }

  error= 0;
err:
  thd->restore_backup_open_tables_state(&open_tables_state_backup);
  free_root(&tmp_mem_root, 0);

  /*
    Now restore to the saved SP cache collection
    and clear the temporary SP cache collection.
  */
  old_sp_caches.sp_caches_swap(*thd);
  old_sp_caches.sp_caches_clear();

  DBUG_RETURN(error);
}


bool store_schema_schemata(THD* thd, TABLE *table, LEX_CSTRING *db_name,
                           CHARSET_INFO *cs, LEX_CSTRING *schema_comment= NULL)
{
  restore_record(table, s->default_values);
  table->field[0]->store(STRING_WITH_LEN("def"), system_charset_info);
  table->field[1]->store(db_name, system_charset_info);
  table->field[2]->store(&cs->cs_name, system_charset_info);
  table->field[3]->store(&cs->coll_name, system_charset_info);
  if (schema_comment)
    table->field[5]->store(schema_comment->str, schema_comment->length,
                           system_charset_info);
  return schema_table_store_record(thd, table);
}


/*
  Check if the specified database exists on disk.

  @param dbname - the database name
  @retval true  - on error, the database directory does not exists
  @retval false - on success, the database directory exists
*/
static bool verify_database_directory_exists(const LEX_CSTRING &dbname)
{
  DBUG_ENTER("verify_database_directory_exists");
  char path[FN_REFLEN + 16];
  uint path_len;
  MY_STAT stat_info;
  if (!dbname.str[0])
    DBUG_RETURN(true); // Empty database name: does not exist.
  path_len= build_table_filename(path, sizeof(path) - 1, dbname.str, "", "", 0);
  path[path_len - 1]= 0;
  if (!mysql_file_stat(key_file_misc, path, &stat_info, MYF(0)))
    DBUG_RETURN(true); // The database directory was not found: does not exist.
  DBUG_RETURN(false);  // The database directory was found.
}


int fill_schema_schemata(THD *thd, TABLE_LIST *tables, COND *cond)
{
  /*
    TODO: fill_schema_shemata() is called when new client is connected.
    Returning error status in this case leads to client hangup.
  */

  LOOKUP_FIELD_VALUES lookup_field_vals;
  Dynamic_array<LEX_CSTRING*> db_names(PSI_INSTRUMENT_MEM);
  Schema_specification_st create;
  TABLE *table= tables->table;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *sctx= thd->security_ctx;
#endif
  DBUG_ENTER("fill_schema_shemata");

  if (get_lookup_field_values(thd, cond, tables, &lookup_field_vals))
    DBUG_RETURN(0);
  DBUG_PRINT("INDEX VALUES",("db_name: %s  table_name: %s",
                             lookup_field_vals.db_value.str,
                             lookup_field_vals.table_value.str));
  if (make_db_list(thd, &db_names, &lookup_field_vals))
    DBUG_RETURN(1);

  /*
    If we have lookup db value we should check that the database exists
  */
  if(lookup_field_vals.db_value.str && !lookup_field_vals.wild_db_value &&
     (!db_names.elements() /* The database name was too long */||
      (db_names.at(0) != &INFORMATION_SCHEMA_NAME &&
       verify_database_directory_exists(lookup_field_vals.db_value))))
    DBUG_RETURN(0);

  for (size_t i=0; i < db_names.elements(); i++)
  {
    LEX_CSTRING *db_name= db_names.at(i);
    DBUG_ASSERT(db_name->length <= NAME_LEN);
    if (db_name == &INFORMATION_SCHEMA_NAME)
    {
      if (store_schema_schemata(thd, table, db_name,
                                system_charset_info))
        DBUG_RETURN(1);
      continue;
    }
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    if (sctx->master_access & (DB_ACLS | SHOW_DB_ACL) ||
        acl_get(sctx->host, sctx->ip, sctx->priv_user, db_name->str, false) ||
        (sctx->priv_role[0] ?
             acl_get("", "", sctx->priv_role, db_name->str, false) : NO_ACL) ||
        !check_grant_db(thd, db_name->str))
#endif
    {
      load_db_opt_by_name(thd, db_name->str, &create);
      if (store_schema_schemata(thd, table, db_name,
                                create.default_table_charset,
                                create.schema_comment))
        DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}


static int get_schema_tables_record(THD *thd, TABLE_LIST *tables,
				    TABLE *table, bool res,
				    const LEX_CSTRING *db_name,
				    const LEX_CSTRING *table_name)
{
  const char *tmp_buff;
  MYSQL_TIME time;
  int info_error= 0;
  CHARSET_INFO *cs= system_charset_info;
  DBUG_ENTER("get_schema_tables_record");

  restore_record(table, s->default_values);
  table->field[0]->store(STRING_WITH_LEN("def"), cs);
  table->field[1]->store(db_name->str, db_name->length, cs);
  table->field[2]->store(table_name->str, table_name->length, cs);

  if (res)
  {
    /* There was a table open error, so set the table type and return */
    if (tables->view)
      table->field[3]->store(STRING_WITH_LEN("VIEW"), cs);
    else if (tables->schema_table)
      table->field[3]->store(STRING_WITH_LEN("SYSTEM VIEW"), cs);
    else
      table->field[3]->store(STRING_WITH_LEN("BASE TABLE"), cs);

    if (tables->option)
    {
      table->field[4]->store(tables->option, strlen(tables->option), cs);
      table->field[4]->set_notnull();
    }
    goto err;
  }

  if (tables->view)
  {
    table->field[3]->store(STRING_WITH_LEN("VIEW"), cs);
    table->field[20]->store(STRING_WITH_LEN("VIEW"), cs);
  }
  else
  {
    char option_buff[512];
    String str(option_buff,sizeof(option_buff), system_charset_info);
    TABLE *show_table= tables->table;
    TABLE_SHARE *share= show_table->s;
    handler *file= show_table->db_stat ? show_table->file : 0;
    handlerton *tmp_db_type= share->db_type();
#ifdef WITH_PARTITION_STORAGE_ENGINE
    bool is_partitioned= FALSE;
#endif

    if (share->tmp_table == SYSTEM_TMP_TABLE)
      table->field[3]->store(STRING_WITH_LEN("SYSTEM VIEW"), cs);
    else if (share->table_type == TABLE_TYPE_SEQUENCE)
      table->field[3]->store(STRING_WITH_LEN("SEQUENCE"), cs);
    else
    {
      DBUG_ASSERT(share->tmp_table == NO_TMP_TABLE);
      if (share->versioned)
        table->field[3]->store(STRING_WITH_LEN("SYSTEM VERSIONED"), cs);
      else
        table->field[3]->store(STRING_WITH_LEN("BASE TABLE"), cs);
    }

    for (uint i= 4; i < table->s->fields; i++)
    {
      if (i == 7 || (i > 12 && i < 17) || i == 18)
        continue;
      table->field[i]->set_notnull();
    }

    /* Collect table info from the table share */

#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (share->db_type() == partition_hton &&
        share->partition_info_str_len)
    {
      tmp_db_type= plugin_hton(share->default_part_plugin);
      is_partitioned= TRUE;
    }
#endif

    tmp_buff= (char *) ha_resolve_storage_engine_name(tmp_db_type);
    table->field[4]->store(tmp_buff, strlen(tmp_buff), cs);
    table->field[5]->store((longlong) share->frm_version, TRUE);

    str.length(0);

    if (share->min_rows)
    {
      str.qs_append(STRING_WITH_LEN(" min_rows="));
      str.qs_append(share->min_rows);
    }

    if (share->max_rows)
    {
      str.qs_append(STRING_WITH_LEN(" max_rows="));
      str.qs_append(share->max_rows);
    }

    if (share->avg_row_length)
    {
      str.qs_append(STRING_WITH_LEN(" avg_row_length="));
      str.qs_append(share->avg_row_length);
    }

    if (share->db_create_options & HA_OPTION_PACK_KEYS)
      str.qs_append(STRING_WITH_LEN(" pack_keys=1"));

    if (share->db_create_options & HA_OPTION_NO_PACK_KEYS)
      str.qs_append(STRING_WITH_LEN(" pack_keys=0"));

    if (share->db_create_options & HA_OPTION_STATS_PERSISTENT)
      str.qs_append(STRING_WITH_LEN(" stats_persistent=1"));

    if (share->db_create_options & HA_OPTION_NO_STATS_PERSISTENT)
      str.qs_append(STRING_WITH_LEN(" stats_persistent=0"));

    if (share->stats_auto_recalc == HA_STATS_AUTO_RECALC_ON)
      str.qs_append(STRING_WITH_LEN(" stats_auto_recalc=1"));
    else if (share->stats_auto_recalc == HA_STATS_AUTO_RECALC_OFF)
      str.qs_append(STRING_WITH_LEN(" stats_auto_recalc=0"));

    if (share->stats_sample_pages != 0)
    {
      str.qs_append(STRING_WITH_LEN(" stats_sample_pages="));
      str.qs_append(share->stats_sample_pages);
    }

    /* We use CHECKSUM, instead of TABLE_CHECKSUM, for backward compatibility */
    if (share->db_create_options & HA_OPTION_CHECKSUM)
      str.qs_append(STRING_WITH_LEN(" checksum=1"));

    if (share->page_checksum != HA_CHOICE_UNDEF)
    {
      str.qs_append(STRING_WITH_LEN(" page_checksum="));
      str.qs_append(&ha_choice_values[(uint) share->page_checksum]);
    }

    if (share->db_create_options & HA_OPTION_DELAY_KEY_WRITE)
      str.qs_append(STRING_WITH_LEN(" delay_key_write=1"));

    if (share->row_type != ROW_TYPE_DEFAULT)
    {
      str.qs_append(STRING_WITH_LEN(" row_format="));
      str.qs_append(&ha_row_type[(uint) share->row_type]);
    }

    if (share->key_block_size)
    {
      str.qs_append(STRING_WITH_LEN(" key_block_size="));
      str.qs_append(share->key_block_size);
    }

#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (is_partitioned)
      str.qs_append(STRING_WITH_LEN(" partitioned"));
#endif

    /*
      Write transactional=0|1 for tables where the user has specified the
      option or for tables that supports both transactional and non
      transactional tables
    */
    if (share->transactional != HA_CHOICE_UNDEF ||
        (share->db_type() &&
         share->db_type()->flags & HTON_TRANSACTIONAL_AND_NON_TRANSACTIONAL &&
         file))
    {
      uint choice= share->transactional;
      if (choice == HA_CHOICE_UNDEF)
        choice= ((file->ha_table_flags() &
                  (HA_NO_TRANSACTIONS | HA_CRASH_SAFE)) ==
                 HA_NO_TRANSACTIONS ?
                 HA_CHOICE_NO : HA_CHOICE_YES);

      str.qs_append(STRING_WITH_LEN(" transactional="));
      str.qs_append(&ha_choice_values[choice]);
    }
    append_create_options(thd, &str, share->option_list, false, 0);

    if (file)
    {
      HA_CREATE_INFO create_info;
      create_info.init();
      file->update_create_info(&create_info);
      append_directory(thd, &str, &DATA_clex_str, create_info.data_file_name);
      append_directory(thd, &str, &INDEX_clex_str, create_info.index_file_name);
    }

    if (str.length())
      table->field[19]->store(str.ptr()+1, str.length()-1, cs);

    LEX_CSTRING tmp_str;
    if (share->table_charset)
      tmp_str= share->table_charset->coll_name;
    else
      tmp_str= { STRING_WITH_LEN("default") };
    table->field[17]->store(&tmp_str, cs);

    if (share->comment.str)
      table->field[20]->store(&share->comment, cs);

    /* Collect table info from the storage engine  */

    if (file)
    {
      /* If info() fails, then there's nothing else to do */
      if (unlikely((info_error= file->info(HA_STATUS_VARIABLE |
                                           HA_STATUS_TIME |
                                           HA_STATUS_VARIABLE_EXTRA |
                                           HA_STATUS_AUTO)) != 0))
      {
        file->print_error(info_error, MYF(0));
        goto err;
      }

      enum row_type row_type = file->get_row_type();
      switch (row_type) {
      case ROW_TYPE_NOT_USED:
      case ROW_TYPE_DEFAULT:
        tmp_buff= ((share->db_options_in_use &
                    HA_OPTION_COMPRESS_RECORD) ? "Compressed" :
                   (share->db_options_in_use & HA_OPTION_PACK_RECORD) ?
                   "Dynamic" : "Fixed");
        break;
      case ROW_TYPE_FIXED:
        tmp_buff= "Fixed";
        break;
      case ROW_TYPE_DYNAMIC:
        tmp_buff= "Dynamic";
        break;
      case ROW_TYPE_COMPRESSED:
        tmp_buff= "Compressed";
        break;
      case ROW_TYPE_REDUNDANT:
        tmp_buff= "Redundant";
        break;
      case ROW_TYPE_COMPACT:
        tmp_buff= "Compact";
        break;
      case ROW_TYPE_PAGE:
        tmp_buff= "Page";
        break;
      }

      table->field[6]->store(tmp_buff, strlen(tmp_buff), cs);

      if (!tables->schema_table)
      {
        table->field[7]->store((longlong) file->stats.records, TRUE);
        table->field[7]->set_notnull();
      }
      table->field[8]->store((longlong) file->stats.mean_rec_length, TRUE);
      table->field[9]->store((longlong) file->stats.data_file_length, TRUE);
      if (file->stats.max_data_file_length)
      {
        table->field[10]->store((longlong) file->stats.max_data_file_length,
                                TRUE);
        table->field[10]->set_notnull();
      }
      table->field[11]->store((longlong) file->stats.index_file_length, TRUE);
      if (file->stats.max_index_file_length)
      {
        table->field[21]->store((longlong) file->stats.max_index_file_length,
                                TRUE);
        table->field[21]->set_notnull();
      }
      table->field[12]->store((longlong) file->stats.delete_length, TRUE);
      if (show_table->found_next_number_field)
      {
        table->field[13]->store((longlong) file->stats.auto_increment_value,
                                TRUE);
        table->field[13]->set_notnull();
      }
      if (file->stats.create_time)
      {
        thd->variables.time_zone->gmt_sec_to_TIME(&time,
                                                  (my_time_t) file->stats.create_time);
        table->field[14]->store_time(&time);
        table->field[14]->set_notnull();
      }
      if (file->stats.update_time)
      {
        thd->variables.time_zone->gmt_sec_to_TIME(&time,
                                                  (my_time_t) file->stats.update_time);
        table->field[15]->store_time(&time);
        table->field[15]->set_notnull();
      }
      if (file->stats.check_time)
      {
        thd->variables.time_zone->gmt_sec_to_TIME(&time,
                                                  (my_time_t) file->stats.check_time);
        table->field[16]->store_time(&time);
        table->field[16]->set_notnull();
      }
      if ((file->ha_table_flags() &
            (HA_HAS_OLD_CHECKSUM | HA_HAS_NEW_CHECKSUM)) &&
           !file->stats.checksum_null)
      {
        table->field[18]->store((longlong) file->stats.checksum, TRUE);
        table->field[18]->set_notnull();
      }
    }
    /* If table is a temporary table */
    LEX_CSTRING tmp= { STRING_WITH_LEN("N") };
    if (show_table->s->tmp_table != NO_TMP_TABLE)
      tmp.str= "Y";
    table->field[22]->store(tmp.str, tmp.length, cs);
  }

err:
  if (unlikely(res || info_error))
  {
    /*
      If an error was encountered, push a warning, set the TABLE COMMENT
      column with the error text, and clear the error so that the operation
      can continue.
    */
    const char *error= thd->get_stmt_da()->message();
    table->field[20]->store(error, strlen(error), cs);

    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 thd->get_stmt_da()->sql_errno(), error);
    thd->clear_error();
  }

  DBUG_RETURN(schema_table_store_record(thd, table));
}


/**
  @brief    Store field characteristics into appropriate I_S table columns

  @param[in]      table             I_S table
  @param[in]      field             processed field
  @param[in]      cs                I_S table charset
  @param[in]      offset            offset from beginning of table
                                    to DATE_TYPE column in I_S table
                                    
  @return         void
*/

static void store_column_type(TABLE *table, Field *field, CHARSET_INFO *cs,
                              uint offset)
{
  const char *tmp_buff;
  char column_type_buff[MAX_FIELD_WIDTH];
  String column_type(column_type_buff, sizeof(column_type_buff), cs);

  field->sql_type(column_type);
  /* DTD_IDENTIFIER column */
  table->field[offset + 8]->store(column_type.ptr(), column_type.length(), cs);
  table->field[offset + 8]->set_notnull();
  /*
    DATA_TYPE column:
    MySQL column type has the following format:
    base_type [(dimension)] [unsigned] [zerofill].
    For DATA_TYPE column we extract only base type.
  */
  tmp_buff= strchr(column_type.c_ptr_safe(), '(');
  if (!tmp_buff)
    /*
      if there is no dimention part then check the presence of
      [unsigned] [zerofill] attributes and cut them of if exist.
    */
    tmp_buff= strchr(column_type.c_ptr_safe(), ' ');
  table->field[offset]->store(column_type.ptr(),
                              (tmp_buff ? (uint)(tmp_buff - column_type.ptr()) :
                               column_type.length()), cs);

  Information_schema_character_attributes cattr=
    field->information_schema_character_attributes();
  if (cattr.has_char_length())
  {
    /* CHARACTER_MAXIMUM_LENGTH column*/
    table->field[offset + 1]->store((longlong) cattr.char_length(), true);
    table->field[offset + 1]->set_notnull();
  }
  if (cattr.has_octet_length())
  {
    /* CHARACTER_OCTET_LENGTH column */
    table->field[offset + 2]->store((longlong) cattr.octet_length(), true);
    table->field[offset + 2]->set_notnull();
  }

  /*
    Calculate field_length and decimals.
    They are set to -1 if they should not be set (we should return NULL)
  */

  Information_schema_numeric_attributes num=
    field->information_schema_numeric_attributes();

  switch (field->type()) {
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_DATETIME:
    /* DATETIME_PRECISION column */
    table->field[offset + 5]->store((longlong) field->decimals(), TRUE);
    table->field[offset + 5]->set_notnull();
    break;
  default:
    break;
  }

  /* NUMERIC_PRECISION column */
  if (num.has_precision())
  {
    table->field[offset + 3]->store((longlong) num.precision(), true);
    table->field[offset + 3]->set_notnull();

    /* NUMERIC_SCALE column */
    if (num.has_scale())
    {
      table->field[offset + 4]->store((longlong) num.scale(), true);
      table->field[offset + 4]->set_notnull();
    }
  }
  if (field->has_charset())
  {
    /* CHARACTER_SET_NAME column*/
    table->field[offset + 6]->store(&field->charset()->cs_name, cs);
    table->field[offset + 6]->set_notnull();
    /* COLLATION_NAME column */
    table->field[offset + 7]->store(&field->charset()->coll_name, cs);
    table->field[offset + 7]->set_notnull();
  }
}


/*
  Print DATA_TYPE independently from sql_mode.
  It's only a brief human-readable description, without attributes,
  so it should not be used by client programs to generate SQL scripts.
*/
static bool print_anchor_data_type(const Spvar_definition *def,
                                   String *data_type)
{
  if (def->column_type_ref())
    return data_type->append(STRING_WITH_LEN("TYPE OF"));
  if (def->is_table_rowtype_ref())
    return data_type->append(STRING_WITH_LEN("ROW TYPE OF"));
  /*
    "ROW TYPE OF cursor" is not possible yet.
    May become possible when we add package-wide cursors.
  */
  DBUG_ASSERT(0);
  return false;
}


/*
  DTD_IDENTIFIER is the full data type description with attributes.
  It can be used by client programs to generate SQL scripts.
  Let's print it according to the current sql_mode.
  It will make output in line with the value in mysql.proc.param_list,
  so both I_S.XXX.DTD_IDENTIFIER and mysql.proc.param_list use the same notation:
  default or Oracle, according to the sql_mode at the SP creation time.
  The caller must make sure to set thd->variables.sql_mode to the routine sql_mode.
*/
static bool print_anchor_dtd_identifier(THD *thd, const Spvar_definition *def,
                                        String *dtd_identifier)
{
  if (def->column_type_ref())
    return (thd->variables.sql_mode & MODE_ORACLE) ?
           def->column_type_ref()->append_to(thd, dtd_identifier) ||
           dtd_identifier->append(STRING_WITH_LEN("%TYPE")) :
           dtd_identifier->append(STRING_WITH_LEN("TYPE OF ")) ||
           def->column_type_ref()->append_to(thd, dtd_identifier);
  if (def->is_table_rowtype_ref())
    return (thd->variables.sql_mode & MODE_ORACLE) ?
           def->table_rowtype_ref()->append_to(thd, dtd_identifier) ||
           dtd_identifier->append(STRING_WITH_LEN("%ROWTYPE")) :
           dtd_identifier->append(STRING_WITH_LEN("ROW TYPE OF ")) ||
           def->table_rowtype_ref()->append_to(thd, dtd_identifier);
  DBUG_ASSERT(0); // See comments in print_anchor_data_type()
  return false;
}


/*
  Set columns DATA_TYPE and DTD_IDENTIFIER from an SP variable definition
*/
static void store_variable_type(THD *thd, const sp_variable *spvar,
                                TABLE *tmptbl,
                                TABLE_SHARE *tmpshare,
                                CHARSET_INFO *cs,
                                TABLE *table, uint offset)
{
  if (spvar->field_def.is_explicit_data_type())
  {
    if (spvar->field_def.is_row())
    {
      // Explicit ROW
      table->field[offset]->store(STRING_WITH_LEN("ROW"), cs);
      table->field[offset]->set_notnull();
      // Perhaps eventually we need to print all ROW elements in DTD_IDENTIFIER
      table->field[offset + 8]->store(STRING_WITH_LEN("ROW"), cs);
      table->field[offset + 8]->set_notnull();
    }
    else
    {
      // Explicit scalar data type
      Field *field= spvar->field_def.make_field(tmpshare, thd->mem_root,
                                                &spvar->name);
      field->table= tmptbl;
      tmptbl->in_use= thd;
      store_column_type(table, field, cs, offset);
    }
  }
  else
  {
    StringBuffer<128> data_type(cs), dtd_identifier(cs);

    if (print_anchor_data_type(&spvar->field_def, &data_type))
    {
      table->field[offset]->store(STRING_WITH_LEN("ERROR"), cs); // EOM?
      table->field[offset]->set_notnull();
    }
    else
    {
      DBUG_ASSERT(data_type.length());
      table->field[offset]->store(data_type.ptr(), data_type.length(), cs);
      table->field[offset]->set_notnull();
    }

    if (print_anchor_dtd_identifier(thd, &spvar->field_def, &dtd_identifier))
    {
      table->field[offset + 8]->store(STRING_WITH_LEN("ERROR"), cs); // EOM?
      table->field[offset + 8]->set_notnull();
    }
    else
    {
      DBUG_ASSERT(dtd_identifier.length());
      table->field[offset + 8]->store(dtd_identifier.ptr(),
                                      dtd_identifier.length(), cs);
      table->field[offset + 8]->set_notnull();
    }
  }
}


static int get_schema_column_record(THD *thd, TABLE_LIST *tables,
				    TABLE *table, bool res,
				    const LEX_CSTRING *db_name,
				    const LEX_CSTRING *table_name)
{
  LEX *lex= thd->lex;
  const char *wild= lex->wild ? lex->wild->ptr() : NullS;
  CHARSET_INFO *cs= system_charset_info;
  TABLE *show_table;
  Field **ptr, *field;
  int count;
  bool quoted_defaults= lex->sql_command != SQLCOM_SHOW_FIELDS;
  DBUG_ENTER("get_schema_column_record");

  if (res)
  {
    if (lex->sql_command != SQLCOM_SHOW_FIELDS)
    {
      /*
        I.e. we are in SELECT FROM INFORMATION_SCHEMA.COLUMS
        rather than in SHOW COLUMNS
      */
      if (thd->is_error())
        convert_error_to_warning(thd);
      res= 0;
    }
    DBUG_RETURN(res);
  }
  show_table= tables->table;
  count= 0;
  ptr= show_table->field;
  show_table->use_all_columns();               // Required for default
  restore_record(show_table, s->default_values);

  for (; (field= *ptr) ; ptr++)
  {
    if(field->invisible > INVISIBLE_USER)
      continue;
    uchar *pos;
    char tmp[MAX_FIELD_WIDTH];
    String type(tmp,sizeof(tmp), system_charset_info);

    DEBUG_SYNC(thd, "get_schema_column");

    if (wild && wild[0] &&
        wild_case_compare(system_charset_info, field->field_name.str, wild))
      continue;

    count++;
    /* Get default row, with all NULL fields set to NULL */
    restore_record(table, s->default_values);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
    ulonglong col_access;
    check_access(thd,SELECT_ACL, db_name->str,
                 &tables->grant.privilege, 0, 0, MY_TEST(tables->schema_table));
    col_access= get_column_grant(thd, &tables->grant,
                                 db_name->str, table_name->str,
                                 field->field_name.str) & COL_ACLS;
    if (!tables->schema_table && !col_access)
      continue;
    char *end= tmp;
    for (uint bitnr=0; col_access ; col_access>>=1,bitnr++)
    {
      if (col_access & 1)
      {
        *end++=',';
        end=strmov(end,grant_types.type_names[bitnr]);
      }
    }
    table->field[18]->store(tmp+1,end == tmp ? 0 : (uint) (end-tmp-1), cs);

#endif
    table->field[0]->store(STRING_WITH_LEN("def"), cs);
    table->field[1]->store(db_name->str, db_name->length, cs);
    table->field[2]->store(table_name->str, table_name->length, cs);
    table->field[3]->store(field->field_name.str, field->field_name.length,
                           cs);
    table->field[4]->store((longlong) count, TRUE);

    if (get_field_default_value(thd, field, &type, quoted_defaults))
    {
      table->field[5]->store(type.ptr(), type.length(), cs);
      table->field[5]->set_notnull();
    }
    pos=(uchar*) ((field->flags & NOT_NULL_FLAG) ?  "NO" : "YES");
    table->field[6]->store((const char*) pos,
                           strlen((const char*) pos), cs);
    store_column_type(table, field, cs, 7);
    pos=(uchar*) ((field->flags & PRI_KEY_FLAG) ? "PRI" :
                 (field->flags & UNIQUE_KEY_FLAG) ? "UNI" :
                 (field->flags & MULTIPLE_KEY_FLAG) ? "MUL":"");
    table->field[16]->store((const char*) pos,
                            strlen((const char*) pos), cs);

    StringBuffer<256> buf;
    if (field->unireg_check == Field::NEXT_NUMBER)
        buf.set(STRING_WITH_LEN("auto_increment"),cs);
    if (print_on_update_clause(field, &type, true))
        buf.set(type.ptr(), type.length(),cs);
    if (field->vcol_info)
    {
      String gen_s(tmp,sizeof(tmp), system_charset_info);
      gen_s.length(0);
      field->vcol_info->print(&gen_s);
      table->field[21]->store(gen_s.ptr(), gen_s.length(), cs);
      table->field[21]->set_notnull();
      table->field[20]->store(STRING_WITH_LEN("ALWAYS"), cs);

      if (field->vcol_info->stored_in_db)
        buf.set(STRING_WITH_LEN("STORED GENERATED"), cs);
      else
        buf.set(STRING_WITH_LEN("VIRTUAL GENERATED"), cs);
    }
    else if (field->flags & VERS_SYSTEM_FIELD)
    {
      if (field->flags & VERS_ROW_START)
      {
        table->field[21]->store(STRING_WITH_LEN("ROW START"), cs);
        buf.set(STRING_WITH_LEN("STORED GENERATED"), cs);
      }
      else
      {
        table->field[21]->store(STRING_WITH_LEN("ROW END"), cs);
        buf.set(STRING_WITH_LEN("STORED GENERATED"), cs);
      }
      table->field[21]->set_notnull();
      table->field[20]->store(STRING_WITH_LEN("ALWAYS"), cs);
    }
    else
      table->field[20]->store(STRING_WITH_LEN("NEVER"), cs);
    /*Invisible can coexist with auto_increment and virtual */
    if (field->invisible == INVISIBLE_USER)
    {
      if (buf.length())
        buf.append(STRING_WITH_LEN(", "));
      buf.append(STRING_WITH_LEN("INVISIBLE"),cs);
    }
    if (field->vers_update_unversioned())
    {
      if (buf.length())
        buf.append(STRING_WITH_LEN(", "));
      buf.append(STRING_WITH_LEN("WITHOUT SYSTEM VERSIONING"), cs);
    }
    table->field[17]->store(buf.ptr(), buf.length(), cs);
    table->field[19]->store(field->comment.str, field->comment.length, cs);
    if (schema_table_store_record(thd, table))
      DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


int fill_schema_charsets(THD *thd, TABLE_LIST *tables, COND *cond)
{
  CHARSET_INFO **cs;
  const char *wild= thd->lex->wild ? thd->lex->wild->ptr() : NullS;
  TABLE *table= tables->table;
  CHARSET_INFO *scs= system_charset_info;

  for (cs= all_charsets ;
       cs < all_charsets + array_elements(all_charsets) ;
       cs++)
  {
    CHARSET_INFO *tmp_cs= cs[0];
    if (tmp_cs && (tmp_cs->state & MY_CS_PRIMARY) &&
        (tmp_cs->state & MY_CS_AVAILABLE) &&
        !(tmp_cs->state & MY_CS_HIDDEN) &&
        !(wild && wild[0] &&
	  wild_case_compare(scs, tmp_cs->cs_name.str,wild)))
    {
      const char *comment;
      restore_record(table, s->default_values);
      table->field[0]->store(&tmp_cs->cs_name, scs);
      table->field[1]->store(&tmp_cs->coll_name, scs);
      comment= tmp_cs->comment ? tmp_cs->comment : "";
      table->field[2]->store(comment, strlen(comment), scs);
      table->field[3]->store((longlong) tmp_cs->mbmaxlen, TRUE);
      if (schema_table_store_record(thd, table))
        return 1;
    }
  }
  return 0;
}


static my_bool iter_schema_engines(THD *thd, plugin_ref plugin,
                                   void *ptable)
{
  TABLE *table= (TABLE *) ptable;
  handlerton *hton= plugin_hton(plugin);
  const char *wild= thd->lex->wild ? thd->lex->wild->ptr() : NullS;
  CHARSET_INFO *scs= system_charset_info;
  handlerton *default_type= ha_default_handlerton(thd);
  DBUG_ENTER("iter_schema_engines");


  /* Disabled plugins */
  if (plugin_state(plugin) != PLUGIN_IS_READY)
  {

    struct st_maria_plugin *plug= plugin_decl(plugin);
    if (!(wild && wild[0] &&
          wild_case_compare(scs, plug->name,wild)))
    {
      restore_record(table, s->default_values);
      table->field[0]->store(plug->name, strlen(plug->name), scs);
      table->field[1]->store(STRING_WITH_LEN("NO"), scs);
      table->field[2]->store(plug->descr, strlen(plug->descr), scs);
      if (schema_table_store_record(thd, table))
        DBUG_RETURN(1);
    }
    DBUG_RETURN(0);
  }

  if (!(hton->flags & HTON_HIDDEN))
  {
    LEX_CSTRING *name= plugin_name(plugin);
    if (!(wild && wild[0] &&
          wild_case_compare(scs, name->str,wild)))
    {
      LEX_CSTRING yesno[2]= {{ STRING_WITH_LEN("NO") },
                             { STRING_WITH_LEN("YES") }};
      LEX_CSTRING *tmp;
      const char *option_name= default_type != hton ? yesno[1].str
                                                    : "DEFAULT";
      restore_record(table, s->default_values);

      table->field[0]->store(name->str, name->length, scs);
      table->field[1]->store(option_name, strlen(option_name), scs);
      table->field[2]->store(plugin_decl(plugin)->descr,
                             strlen(plugin_decl(plugin)->descr), scs);
      tmp= &yesno[MY_TEST(hton->commit && !(hton->flags & HTON_NO_ROLLBACK))];
      table->field[3]->store(tmp->str, tmp->length, scs);
      table->field[3]->set_notnull();
      tmp= &yesno[MY_TEST(hton->prepare)];
      table->field[4]->store(tmp->str, tmp->length, scs);
      table->field[4]->set_notnull();
      tmp= &yesno[MY_TEST(hton->savepoint_set)];
      table->field[5]->store(tmp->str, tmp->length, scs);
      table->field[5]->set_notnull();

      if (schema_table_store_record(thd, table))
        DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}

int fill_schema_engines(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_schema_engines");
  if (plugin_foreach_with_mask(thd, iter_schema_engines,
                               MYSQL_STORAGE_ENGINE_PLUGIN,
                               ~(PLUGIN_IS_FREED | PLUGIN_IS_DYING),
                               tables->table))
    DBUG_RETURN(1);
  DBUG_RETURN(0);
}


int fill_schema_collation(THD *thd, TABLE_LIST *tables, COND *cond)
{
  CHARSET_INFO **cs;
  const char *wild= thd->lex->wild ? thd->lex->wild->ptr() : NullS;
  TABLE *table= tables->table;
  CHARSET_INFO *scs= system_charset_info;
  for (cs= all_charsets ;
       cs < all_charsets + array_elements(all_charsets)  ;
       cs++ )
  {
    CHARSET_INFO **cl;
    CHARSET_INFO *tmp_cs= cs[0];
    if (!tmp_cs || !(tmp_cs->state & MY_CS_AVAILABLE) ||
         (tmp_cs->state & MY_CS_HIDDEN) ||
        !(tmp_cs->state & MY_CS_PRIMARY))
      continue;
    for (cl= all_charsets;
         cl < all_charsets + array_elements(all_charsets)  ;
         cl ++)
    {
      CHARSET_INFO *tmp_cl= cl[0];
      if (!tmp_cl || !(tmp_cl->state & MY_CS_AVAILABLE) ||
          !my_charset_same(tmp_cs, tmp_cl))
	continue;
      if (!(wild && wild[0] &&
	  wild_case_compare(scs, tmp_cl->coll_name.str, wild)))
      {
	const char *tmp_buff;
	restore_record(table, s->default_values);
	table->field[0]->store(tmp_cl->coll_name.str, tmp_cl->coll_name.length,
                               scs);
        table->field[1]->store(&tmp_cl->cs_name, scs);
        table->field[2]->store((longlong) tmp_cl->number, TRUE);
        tmp_buff= (tmp_cl->state & MY_CS_PRIMARY) ? "Yes" : "";
	table->field[3]->store(tmp_buff, strlen(tmp_buff), scs);
        tmp_buff= (tmp_cl->state & MY_CS_COMPILED)? "Yes" : "";
	table->field[4]->store(tmp_buff, strlen(tmp_buff), scs);
        table->field[5]->store((longlong) tmp_cl->strxfrm_multiply, TRUE);
        if (schema_table_store_record(thd, table))
          return 1;
      }
    }
  }
  return 0;
}


int fill_schema_coll_charset_app(THD *thd, TABLE_LIST *tables, COND *cond)
{
  CHARSET_INFO **cs;
  TABLE *table= tables->table;
  CHARSET_INFO *scs= system_charset_info;
  for (cs= all_charsets ;
       cs < all_charsets + array_elements(all_charsets) ;
       cs++ )
  {
    CHARSET_INFO **cl;
    CHARSET_INFO *tmp_cs= cs[0];
    if (!tmp_cs || !(tmp_cs->state & MY_CS_AVAILABLE) ||
        !(tmp_cs->state & MY_CS_PRIMARY))
      continue;
    for (cl= all_charsets;
         cl < all_charsets + array_elements(all_charsets) ;
         cl ++)
    {
      CHARSET_INFO *tmp_cl= cl[0];
      if (!tmp_cl || !(tmp_cl->state & MY_CS_AVAILABLE) ||
          (tmp_cl->state & MY_CS_HIDDEN) ||
          !my_charset_same(tmp_cs,tmp_cl))
	continue;
      restore_record(table, s->default_values);
      table->field[0]->store(&tmp_cl->coll_name, scs);
      table->field[1]->store(&tmp_cl->cs_name, scs);
      if (schema_table_store_record(thd, table))
        return 1;
    }
  }
  return 0;
}


static inline void copy_field_as_string(Field *to_field, Field *from_field)
{
  char buff[MAX_FIELD_WIDTH];
  String tmp_str(buff, sizeof(buff), system_charset_info);
  from_field->val_str(&tmp_str);
  to_field->store(tmp_str.ptr(), tmp_str.length(), system_charset_info);
}


/**
  @brief Store record into I_S.PARAMETERS table

  @param[in]      thd                   thread handler
  @param[in]      table                 I_S table
  @param[in]      proc_table            'mysql.proc' table
  @param[in]      wild                  wild string, not used for now,
                                        will be useful
                                        if we add 'SHOW PARAMETERs'
  @param[in]      full_access           if 1 user has privileges on the routine
  @param[in]      sp_user               user in 'user@host' format

  @return         Operation status
    @retval       0                     ok
    @retval       1                     error
*/

bool store_schema_params(THD *thd, TABLE *table, TABLE *proc_table,
                         const char *wild, bool full_access,
                         const char *sp_user)
{
  TABLE_SHARE share;
  TABLE tbl;
  CHARSET_INFO *cs= system_charset_info;
  LEX_CSTRING definer, params, returns= empty_clex_str;
  LEX_CSTRING db, name;
  char path[FN_REFLEN];
  sp_head *sp;
  const Sp_handler *sph;
  bool free_sp_head;
  bool error= 0;
  sql_mode_t sql_mode;
  DBUG_ENTER("store_schema_params");

  bzero((char*) &tbl, sizeof(TABLE));
  (void) build_table_filename(path, sizeof(path), "", "", "", 0);
  init_tmp_table_share(thd, &share, "", 0, "", path);

  proc_table->field[MYSQL_PROC_FIELD_DB]->val_str_nopad(thd->mem_root, &db);
  proc_table->field[MYSQL_PROC_FIELD_NAME]->val_str_nopad(thd->mem_root, &name);
  proc_table->field[MYSQL_PROC_FIELD_DEFINER]->val_str_nopad(thd->mem_root, &definer);
  sql_mode= (sql_mode_t) proc_table->field[MYSQL_PROC_FIELD_SQL_MODE]->val_int();
  sph= Sp_handler::handler_mysql_proc((enum_sp_type)
                                      proc_table->field[MYSQL_PROC_MYSQL_TYPE]->
                                      val_int());
  if (!sph || sph->type() == SP_TYPE_PACKAGE ||
      sph->type() == SP_TYPE_PACKAGE_BODY)
    DBUG_RETURN(0);

  if (!full_access)
    full_access= !strcmp(sp_user, definer.str);
  if (!full_access &&
      check_some_routine_access(thd, db.str, name.str, sph))
    DBUG_RETURN(0);

  proc_table->field[MYSQL_PROC_FIELD_PARAM_LIST]->val_str_nopad(thd->mem_root,
                                                                &params);
  if (sph->type() == SP_TYPE_FUNCTION)
    proc_table->field[MYSQL_PROC_FIELD_RETURNS]->val_str_nopad(thd->mem_root,
                                                               &returns);
  sp= sph->sp_load_for_information_schema(thd, proc_table, db, name,
                                          params, returns, sql_mode,
                                          &free_sp_head);
  if (sp)
  {
    Field *field;
    LEX_CSTRING tmp_string;
    Sql_mode_save sql_mode_backup(thd);
    thd->variables.sql_mode= sql_mode;

    if (sph->type() == SP_TYPE_FUNCTION)
    {
      restore_record(table, s->default_values);
      table->field[0]->store(STRING_WITH_LEN("def"), cs);
      table->field[1]->store(db, cs);
      table->field[2]->store(name, cs);
      table->field[3]->store((longlong) 0, TRUE);
      proc_table->field[MYSQL_PROC_MYSQL_TYPE]->val_str_nopad(thd->mem_root,
                                                              &tmp_string);
      table->field[15]->store(tmp_string, cs);
      field= sp->m_return_field_def.make_field(&share, thd->mem_root,
                                               &empty_clex_str);
      field->table= &tbl;
      tbl.in_use= thd;
      store_column_type(table, field, cs, 6);
      if (schema_table_store_record(thd, table))
      {
        free_table_share(&share);
        if (free_sp_head)
          sp_head::destroy(sp);
        DBUG_RETURN(1);
      }
    }

    sp_pcontext *spcont= sp->get_parse_context();
    uint params= spcont->context_var_count();
    for (uint i= 0 ; i < params ; i++)
    {
      const char *tmp_buff;
      sp_variable *spvar= spcont->find_variable(i);
      switch (spvar->mode) {
      case sp_variable::MODE_IN:
        tmp_buff= "IN";
        break;
      case sp_variable::MODE_OUT:
        tmp_buff= "OUT";
        break;
      case sp_variable::MODE_INOUT:
        tmp_buff= "INOUT";
        break;
      default:
        tmp_buff= "";
        break;
      }  

      restore_record(table, s->default_values);
      table->field[0]->store(STRING_WITH_LEN("def"), cs);
      table->field[1]->store(db, cs);
      table->field[2]->store(name, cs);
      table->field[3]->store((longlong) i + 1, TRUE);
      table->field[4]->store(tmp_buff, strlen(tmp_buff), cs);
      table->field[4]->set_notnull();
      table->field[5]->store(spvar->name.str, spvar->name.length, cs);
      table->field[5]->set_notnull();
      proc_table->field[MYSQL_PROC_MYSQL_TYPE]->val_str_nopad(thd->mem_root,
                                                              &tmp_string);
      table->field[15]->store(tmp_string, cs);

      store_variable_type(thd, spvar, &tbl, &share, cs, table, 6);
      if (schema_table_store_record(thd, table))
      {
        error= 1;
        break;
      }
    }
    if (free_sp_head)
      sp_head::destroy(sp);
  }
  free_table_share(&share);
  DBUG_RETURN(error);
}


bool store_schema_proc(THD *thd, TABLE *table, TABLE *proc_table,
                       const char *wild, bool full_access, const char *sp_user)
{
  LEX *lex= thd->lex;
  CHARSET_INFO *cs= system_charset_info;
  const Sp_handler *sph;
  LEX_CSTRING db, name, definer, returns= empty_clex_str;

  proc_table->field[MYSQL_PROC_FIELD_DB]->val_str_nopad(thd->mem_root, &db);
  proc_table->field[MYSQL_PROC_FIELD_NAME]->val_str_nopad(thd->mem_root, &name);
  proc_table->field[MYSQL_PROC_FIELD_DEFINER]->val_str_nopad(thd->mem_root, &definer);
  sph= Sp_handler::handler_mysql_proc((enum_sp_type)
                                      proc_table->field[MYSQL_PROC_MYSQL_TYPE]->
                                      val_int());
  if (!sph)
    return 0;

  if (!full_access)
    full_access= !strcmp(sp_user, definer.str);
  if (!full_access &&
      check_some_routine_access(thd, db.str, name.str, sph))
    return 0;

  if (!is_show_command(thd) ||
      sph == Sp_handler::handler(lex->sql_command))
  {
    restore_record(table, s->default_values);
    if (!wild || !wild[0] || !wild_case_compare(system_charset_info,
                                                name.str, wild))
    {
      int enum_idx= (int) proc_table->field[MYSQL_PROC_FIELD_ACCESS]->val_int();
      table->field[3]->store(name, cs);

      copy_field_as_string(table->field[0],
                           proc_table->field[MYSQL_PROC_FIELD_SPECIFIC_NAME]);
      table->field[1]->store(STRING_WITH_LEN("def"), cs);
      table->field[2]->store(db, cs);
      copy_field_as_string(table->field[4],
                           proc_table->field[MYSQL_PROC_MYSQL_TYPE]);

      if (sph->type() == SP_TYPE_FUNCTION)
      {
        sp_head *sp;
        bool free_sp_head;
        proc_table->field[MYSQL_PROC_FIELD_RETURNS]->val_str_nopad(thd->mem_root,
                                                                   &returns);
        sp= sph->sp_load_for_information_schema(thd, proc_table,
                                                db, name,
                                                empty_clex_str /*params*/,
                                                returns,
                                                (ulong) proc_table->
                                                field[MYSQL_PROC_FIELD_SQL_MODE]->
                                                val_int(),
                                                &free_sp_head);
        if (sp)
        {
          char path[FN_REFLEN];
          TABLE_SHARE share;
          TABLE tbl;
          Field *field;

          bzero((char*) &tbl, sizeof(TABLE));
          (void) build_table_filename(path, sizeof(path), "", "", "", 0);
          init_tmp_table_share(thd, &share, "", 0, "", path);
          field= sp->m_return_field_def.make_field(&share, thd->mem_root,
                                                   &empty_clex_str);
          field->table= &tbl;
          tbl.in_use= thd;
          store_column_type(table, field, cs, 5);
          free_table_share(&share);
          if (free_sp_head)
            sp_head::destroy(sp);
        }
      }

      if (full_access)
      {
        copy_field_as_string(table->field[15],
                             proc_table->field[MYSQL_PROC_FIELD_BODY_UTF8]);
        table->field[15]->set_notnull();
      }
      table->field[14]->store(STRING_WITH_LEN("SQL"), cs);
      table->field[18]->store(STRING_WITH_LEN("SQL"), cs);
      copy_field_as_string(table->field[19],
                           proc_table->field[MYSQL_PROC_FIELD_DETERMINISTIC]);
      table->field[20]->store(sp_data_access_name[enum_idx].str, 
                              sp_data_access_name[enum_idx].length , cs);
      copy_field_as_string(table->field[22],
                           proc_table->field[MYSQL_PROC_FIELD_SECURITY_TYPE]);

      proc_table->field[MYSQL_PROC_FIELD_CREATED]->
             save_in_field(table->field[23]);
      proc_table->field[MYSQL_PROC_FIELD_MODIFIED]->
             save_in_field(table->field[24]);

      copy_field_as_string(table->field[25],
                           proc_table->field[MYSQL_PROC_FIELD_SQL_MODE]);
      copy_field_as_string(table->field[26],
                           proc_table->field[MYSQL_PROC_FIELD_COMMENT]);

      table->field[27]->store(definer, cs);
      copy_field_as_string(table->field[28],
                           proc_table->
                           field[MYSQL_PROC_FIELD_CHARACTER_SET_CLIENT]);
      copy_field_as_string(table->field[29],
                           proc_table->
                           field[MYSQL_PROC_FIELD_COLLATION_CONNECTION]);
      copy_field_as_string(table->field[30],
			   proc_table->field[MYSQL_PROC_FIELD_DB_COLLATION]);

      return schema_table_store_record(thd, table);
    }
  }
  return 0;
}


int fill_schema_proc(THD *thd, TABLE_LIST *tables, COND *cond)
{
  TABLE *proc_table;
  TABLE_LIST proc_tables;
  const char *wild= thd->lex->wild ? thd->lex->wild->ptr() : NullS;
  int res= 0;
  TABLE *table= tables->table;
  bool full_access;
  char definer[USER_HOST_BUFF_SIZE];
  enum enum_schema_tables schema_table_idx=
    get_schema_table_idx(tables->schema_table);
  DBUG_ENTER("fill_schema_proc");

  strxmov(definer, thd->security_ctx->priv_user, "@",
          thd->security_ctx->priv_host, NullS);
  /* We use this TABLE_LIST instance only for checking of privileges. */
  bzero((char*) &proc_tables,sizeof(proc_tables));
  proc_tables.db= MYSQL_SCHEMA_NAME;
  proc_tables.table_name= MYSQL_PROC_NAME;
  proc_tables.alias= MYSQL_PROC_NAME;
  proc_tables.lock_type= TL_READ;
  full_access= !check_table_access(thd, SELECT_ACL, &proc_tables, FALSE,
                                   1, TRUE);

  start_new_trans new_trans(thd);

  if (!(proc_table= open_proc_table_for_read(thd)))
  {
    new_trans.restore_old_transaction();
    DBUG_RETURN(1);
  }

  /* Disable padding temporarily so it doesn't break the query */
  ulonglong sql_mode_was = thd->variables.sql_mode;
  thd->variables.sql_mode &= ~MODE_PAD_CHAR_TO_FULL_LENGTH;

  if (proc_table->file->ha_index_init(0, 1))
  {
    res= 1;
    goto err;
  }

  if ((res= proc_table->file->ha_index_first(proc_table->record[0])))
  {
    res= (res == HA_ERR_END_OF_FILE) ? 0 : 1;
    goto err;
  }

  if (schema_table_idx == SCH_PROCEDURES ?
      store_schema_proc(thd, table, proc_table, wild, full_access, definer) :
      store_schema_params(thd, table, proc_table, wild, full_access, definer))
  {
    res= 1;
    goto err;
  }
  while (!proc_table->file->ha_index_next(proc_table->record[0]))
  {
    if (schema_table_idx == SCH_PROCEDURES ?
        store_schema_proc(thd, table, proc_table, wild, full_access, definer): 
        store_schema_params(thd, table, proc_table, wild, full_access, definer))
    {
      res= 1;
      goto err;
    }
  }

err:
  if (proc_table->file->inited)
    (void) proc_table->file->ha_index_end();

  thd->commit_whole_transaction_and_close_tables();
  new_trans.restore_old_transaction();

  thd->variables.sql_mode = sql_mode_was;
  DBUG_RETURN(res);
}


static int get_schema_stat_record(THD *thd, TABLE_LIST *tables,
				  TABLE *table, bool res,
				  const LEX_CSTRING *db_name,
				  const LEX_CSTRING *table_name)
{
  CHARSET_INFO *cs= system_charset_info;
  DBUG_ENTER("get_schema_stat_record");
  if (res)
  {
    if (thd->lex->sql_command != SQLCOM_SHOW_KEYS)
    {
      /*
        I.e. we are in SELECT FROM INFORMATION_SCHEMA.STATISTICS
        rather than in SHOW KEYS
      */
      if (unlikely(thd->is_error()))
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                     thd->get_stmt_da()->sql_errno(),
                     thd->get_stmt_da()->message());
      thd->clear_error();
      res= 0;
    }
    DBUG_RETURN(res);
  }
  else if (!tables->view)
  {
    TABLE *show_table= tables->table;
    KEY *key_info=show_table->s->key_info;
    if (show_table->file)
    {
      (void) read_statistics_for_tables(thd, tables);
      show_table->file->info(HA_STATUS_VARIABLE |
                             HA_STATUS_NO_LOCK |
                             HA_STATUS_CONST |
                             HA_STATUS_TIME);
      set_statistics_for_table(thd, show_table);
    }
    for (uint i=0 ; i < show_table->s->keys ; i++,key_info++)
    {
      if ((key_info->flags & HA_INVISIBLE_KEY) &&
          !DBUG_IF("test_invisible_index"))
        continue;
      KEY_PART_INFO *key_part= key_info->key_part;
      LEX_CSTRING *str;
      LEX_CSTRING unknown= {STRING_WITH_LEN("?unknown field?") };
      for (uint j=0 ; j < key_info->user_defined_key_parts ; j++,key_part++)
      {
        if (key_part->field->invisible >= INVISIBLE_SYSTEM &&
            !DBUG_IF("test_completely_invisible"))
        {
          /*
            NOTE: we will get SEQ_IN_INDEX gap inside the result if this key_part
            is not last (currently not possible). Though nothing is wrong with
            that probably.
          */
          continue;
        }
        restore_record(table, s->default_values);
        table->field[0]->store(STRING_WITH_LEN("def"), cs);
        table->field[1]->store(db_name->str, db_name->length, cs);
        table->field[2]->store(table_name->str, table_name->length, cs);
        table->field[3]->store((longlong) ((key_info->flags &
                                            HA_NOSAME) ? 0 : 1), TRUE);
        table->field[4]->store(db_name->str, db_name->length, cs);
        table->field[5]->store(key_info->name.str, key_info->name.length, cs);
        table->field[6]->store((longlong) (j+1), TRUE);
        str= (key_part->field ? &key_part->field->field_name :
              &unknown);
        table->field[7]->store(str->str, str->length, cs);
        if (show_table->file)
        {
          if (show_table->file->index_flags(i, j, 0) & HA_READ_ORDER)
          {
            table->field[8]->store(((key_part->key_part_flag &
                                     HA_REVERSE_SORT) ?
                                    "D" : "A"), 1, cs);
            table->field[8]->set_notnull();
          }
          if (key_info->algorithm == HA_KEY_ALG_LONG_HASH)
            table->field[13]->store(STRING_WITH_LEN("HASH"), cs);
          else
          {
            /*
              We have to use table key information to get the key statistics
              from table as key_info points to TABLE_SHARE which has no
              statistics.
            */
            KEY *key_info= show_table->key_info + i;
            if (key_info->rec_per_key[j])
            {
              ha_rows records= (ha_rows) ((double) show_table->stat_records() /
                                          key_info->actual_rec_per_key(j));
              table->field[9]->store((longlong) records, TRUE);
              table->field[9]->set_notnull();
            }
            const char *tmp= show_table->file->index_type(i);
            table->field[13]->store(tmp, strlen(tmp), cs);
          }
        }
        if (!(key_info->flags & HA_FULLTEXT) &&
            (key_part->field &&
             key_part->length !=
             show_table->s->field[key_part->fieldnr-1]->key_length()))
        {
          table->field[10]->store((longlong) key_part->length /
                                  key_part->field->charset()->mbmaxlen, TRUE);
          table->field[10]->set_notnull();
        }
        uint flags= key_part->field ? key_part->field->flags : 0;
        const char *pos=(char*) ((flags & NOT_NULL_FLAG) ? "" : "YES");
        table->field[12]->store(pos, strlen(pos), cs);
        if (!show_table->s->keys_in_use.is_set(i))
          table->field[14]->store(STRING_WITH_LEN("disabled"), cs);
        else
          table->field[14]->store("", 0, cs);
        table->field[14]->set_notnull();
        DBUG_ASSERT(MY_TEST(key_info->flags & HA_USES_COMMENT) ==
                   (key_info->comment.length > 0));
        if (key_info->flags & HA_USES_COMMENT)
          table->field[15]->store(key_info->comment.str, 
                                  key_info->comment.length, cs);

        // IGNORED column
        const char *is_ignored= key_info->is_ignored ? "YES" : "NO";
        table->field[16]->store(is_ignored, strlen(is_ignored), cs);
        table->field[16]->set_notnull();

        if (schema_table_store_record(thd, table))
          DBUG_RETURN(1);
      }
    }
  }
  DBUG_RETURN(res);
}


static int get_schema_views_record(THD *thd, TABLE_LIST *tables,
				   TABLE *table, bool res,
				   const LEX_CSTRING *db_name,
				   const LEX_CSTRING *table_name)
{
  CHARSET_INFO *cs= system_charset_info;
  char definer[USER_HOST_BUFF_SIZE];
  uint definer_len;
  bool updatable_view;
  DBUG_ENTER("get_schema_views_record");

  if (tables->view)
  {
    Security_context *sctx= thd->security_ctx;
    if (!tables->allowed_show)
    {
      if (!my_strcasecmp(system_charset_info, tables->definer.user.str,
                         sctx->priv_user) &&
          !my_strcasecmp(system_charset_info, tables->definer.host.str,
                         sctx->priv_host))
        tables->allowed_show= TRUE;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
      else
      {
        if ((thd->col_access & (SHOW_VIEW_ACL|SELECT_ACL)) ==
            (SHOW_VIEW_ACL|SELECT_ACL))
          tables->allowed_show= TRUE;
        else
        {
          TABLE_LIST table_list;
          table_list.reset();
          table_list.db= tables->db;
          table_list.table_name= tables->table_name;
          table_list.grant.privilege= thd->col_access;
          privilege_t view_access(get_table_grant(thd, &table_list));
	  if ((view_access & (SHOW_VIEW_ACL|SELECT_ACL)) ==
	      (SHOW_VIEW_ACL|SELECT_ACL))
	    tables->allowed_show= TRUE;
        }
      }
#endif
    }
    restore_record(table, s->default_values);
    table->field[0]->store(STRING_WITH_LEN("def"), cs);
    table->field[1]->store(db_name->str, db_name->length, cs);
    table->field[2]->store(table_name->str, table_name->length, cs);

    if (tables->allowed_show)
    {
      table->field[3]->store(tables->view_body_utf8.str,
                             tables->view_body_utf8.length,
                             cs);
    }

    if (tables->with_check != VIEW_CHECK_NONE)
    {
      if (tables->with_check == VIEW_CHECK_LOCAL)
        table->field[4]->store(STRING_WITH_LEN("LOCAL"), cs);
      else
        table->field[4]->store(STRING_WITH_LEN("CASCADED"), cs);
    }
    else
      table->field[4]->store(STRING_WITH_LEN("NONE"), cs);

    /*
      Only try to fill in the information about view updatability
      if it is requested as part of the top-level query (i.e.
      it's select * from i_s.views, as opposed to, say, select
      security_type from i_s.views).  Do not try to access the
      underlying tables if there was an error when opening the
      view: all underlying tables are released back to the table
      definition cache on error inside open_normal_and_derived_tables().
      If a field is not assigned explicitly, it defaults to NULL.
    */
    if (res == FALSE &&
        table->pos_in_table_list->table_open_method & OPEN_FULL_TABLE)
    {
      updatable_view= 0;
      if (tables->algorithm != VIEW_ALGORITHM_TMPTABLE)
      {
        /*
          We should use tables->view->select_lex.item_list here
          and can not use Field_iterator_view because the view
          always uses temporary algorithm during opening for I_S
          and TABLE_LIST fields 'field_translation'
          & 'field_translation_end' are uninitialized is this
          case.
        */
        List<Item> *fields= &tables->view->first_select_lex()->item_list;
        List_iterator<Item> it(*fields);
        Item *item;
        Item_field *field;
        /*
          check that at least one column in view is updatable
        */
        while ((item= it++))
        {
          if ((field= item->field_for_view_update()) && field->field &&
              !field->field->table->pos_in_table_list->schema_table)
          {
            updatable_view= 1;
            break;
          }
        }
        if (updatable_view && !tables->view->can_be_merged())
          updatable_view= 0;
      }
      if (updatable_view)
        table->field[5]->store(STRING_WITH_LEN("YES"), cs);
      else
        table->field[5]->store(STRING_WITH_LEN("NO"), cs);
    }

    definer_len= (uint)(strxmov(definer, tables->definer.user.str, "@",
                          tables->definer.host.str, NullS) - definer);
    table->field[6]->store(definer, definer_len, cs);
    if (tables->view_suid)
      table->field[7]->store(STRING_WITH_LEN("DEFINER"), cs);
    else
      table->field[7]->store(STRING_WITH_LEN("INVOKER"), cs);

    table->field[8]->store(&tables->view_creation_ctx->get_client_cs()->cs_name,
                           cs);
    table->field[9]->store(&tables->view_creation_ctx->
                           get_connection_cl()->coll_name, cs);
    table->field[10]->store(view_algorithm(tables), cs);

    if (schema_table_store_record(thd, table))
      DBUG_RETURN(1);
    if (unlikely(res && thd->is_error()))
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                   thd->get_stmt_da()->sql_errno(),
                   thd->get_stmt_da()->message());
  }
  if (res)
    thd->clear_error();
  DBUG_RETURN(0);
}


static bool
store_constraints(THD *thd, TABLE *table, const LEX_CSTRING *db_name,
                  const LEX_CSTRING *table_name, const char *key_name,
                  size_t key_len, const char *con_type, size_t con_len)
{
  CHARSET_INFO *cs= system_charset_info;
  restore_record(table, s->default_values);
  table->field[0]->store(STRING_WITH_LEN("def"), cs);
  table->field[1]->store(db_name->str, db_name->length, cs);
  table->field[2]->store(key_name, key_len, cs);
  table->field[3]->store(db_name->str, db_name->length, cs);
  table->field[4]->store(table_name->str, table_name->length, cs);
  table->field[5]->store(con_type, con_len, cs);
  return schema_table_store_record(thd, table);
}

static int get_check_constraints_record(THD *thd, TABLE_LIST *tables,
                                        TABLE *table, bool res,
                                        const LEX_CSTRING *db_name,
                                        const LEX_CSTRING *table_name)
{
  DBUG_ENTER("get_check_constraints_record");
  if (res)
  {
    if (thd->is_error())
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                   thd->get_stmt_da()->sql_errno(),
                   thd->get_stmt_da()->message());
    thd->clear_error();
    DBUG_RETURN(0);
  }
  if (!tables->view)
  {
    StringBuffer<MAX_FIELD_WIDTH> str(system_charset_info);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    TABLE_LIST table_acl_check;
    bzero((char*) &table_acl_check, sizeof(table_acl_check));
#endif
    for (uint i= 0; i < tables->table->s->table_check_constraints; i++)
    {
#ifndef NO_EMBEDDED_ACCESS_CHECKS
      if (!(thd->col_access & TABLE_ACLS))
      {
        table_acl_check.db= *db_name;
        table_acl_check.table_name= *table_name;
        table_acl_check.grant.privilege= thd->col_access;
        if (check_grant(thd, TABLE_ACLS, &table_acl_check, FALSE, 1, TRUE))
          continue;
      }
#endif
      Virtual_column_info *check= tables->table->check_constraints[i];
      table->field[0]->store(STRING_WITH_LEN("def"), system_charset_info);
      table->field[3]->store(check->name.str, check->name.length,
                             system_charset_info);
      const char *tmp_buff;
      tmp_buff= (check->get_vcol_type() == VCOL_CHECK_FIELD ?
                 "Column" : "Table");
      table->field[4]->store(tmp_buff, strlen(tmp_buff), system_charset_info);
      /* Make sure the string is empty between each print. */
      str.length(0);
      check->print(&str);
      table->field[5]->store(str.ptr(), str.length(), system_charset_info);
      if (schema_table_store_record(thd, table))
        DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(res);
}

static int get_schema_constraints_record(THD *thd, TABLE_LIST *tables,
					 TABLE *table, bool res,
					 const LEX_CSTRING *db_name,
					 const LEX_CSTRING *table_name)
{
  DBUG_ENTER("get_schema_constraints_record");
  if (res)
  {
    if (unlikely(thd->is_error()))
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                   thd->get_stmt_da()->sql_errno(),
                   thd->get_stmt_da()->message());
    thd->clear_error();
    DBUG_RETURN(0);
  }
  else if (!tables->view)
  {
    List<FOREIGN_KEY_INFO> f_key_list;
    TABLE *show_table= tables->table;
    KEY *key_info=show_table->s->key_info;
    uint primary_key= show_table->s->primary_key;
    show_table->file->info(HA_STATUS_VARIABLE |
                           HA_STATUS_NO_LOCK |
                           HA_STATUS_TIME);
    for (uint i=0 ; i < show_table->s->keys ; i++, key_info++)
    {
      if (i != primary_key && !(key_info->flags & HA_NOSAME))
        continue;

      if (i == primary_key && !strcmp(key_info->name.str, primary_key_name.str))
      {
        if (store_constraints(thd, table, db_name, table_name,
                              key_info->name.str, key_info->name.length,
                              STRING_WITH_LEN("PRIMARY KEY")))
          DBUG_RETURN(1);
      }
      else if (key_info->flags & HA_NOSAME)
      {
        if (store_constraints(thd, table, db_name, table_name,
                              key_info->name.str, key_info->name.length,
                              STRING_WITH_LEN("UNIQUE")))
          DBUG_RETURN(1);
      }
    }

    // Table check constraints
    for ( uint i = 0; i < show_table->s->table_check_constraints; i++ )
    {
        Virtual_column_info *check = show_table->check_constraints[ i ];

        if ( store_constraints( thd, table, db_name, table_name, check->name.str,
                                check->name.length,
                                STRING_WITH_LEN( "CHECK" ) ) )
        {
            DBUG_RETURN( 1 );
        }
    }

    show_table->file->get_foreign_key_list(thd, &f_key_list);
    FOREIGN_KEY_INFO *f_key_info;
    List_iterator_fast<FOREIGN_KEY_INFO> it(f_key_list);
    while ((f_key_info=it++))
    {
      if (store_constraints(thd, table, db_name, table_name,
                            f_key_info->foreign_id->str,
                            strlen(f_key_info->foreign_id->str),
                            "FOREIGN KEY", 11))
        DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(res);
}


static bool store_trigger(THD *thd, Trigger *trigger,
                          TABLE *table, const LEX_CSTRING *db_name,
                          const LEX_CSTRING *table_name)
{
  CHARSET_INFO *cs= system_charset_info;
  LEX_CSTRING sql_mode_rep;
  MYSQL_TIME timestamp;
  char definer_holder[USER_HOST_BUFF_SIZE];
  LEX_STRING definer_buffer;
  LEX_CSTRING trigger_stmt, trigger_body;
  definer_buffer.str= definer_holder;

  trigger->get_trigger_info(&trigger_stmt, &trigger_body, &definer_buffer);

  restore_record(table, s->default_values);
  table->field[0]->store(STRING_WITH_LEN("def"), cs);
  table->field[1]->store(db_name->str, db_name->length, cs);
  table->field[2]->store(trigger->name.str, trigger->name.length, cs);
  table->field[3]->store(trg_event_type_names[trigger->event].str,
                         trg_event_type_names[trigger->event].length, cs);
  table->field[4]->store(STRING_WITH_LEN("def"), cs);
  table->field[5]->store(db_name->str, db_name->length, cs);
  table->field[6]->store(table_name->str, table_name->length, cs);
  table->field[7]->store(trigger->action_order);
  table->field[9]->store(trigger_body.str, trigger_body.length, cs);
  table->field[10]->store(STRING_WITH_LEN("ROW"), cs);
  table->field[11]->store(trg_action_time_type_names[trigger->action_time].str,
                          trg_action_time_type_names[trigger->action_time].length, cs);
  table->field[14]->store(STRING_WITH_LEN("OLD"), cs);
  table->field[15]->store(STRING_WITH_LEN("NEW"), cs);

  if (trigger->create_time)
  {
    table->field[16]->set_notnull();
    thd->variables.time_zone->gmt_sec_to_TIME(&timestamp,
                                              (my_time_t)(trigger->create_time/100));
    /* timestamp is with 6 digits */
    timestamp.second_part= (trigger->create_time % 100) * 10000;
    table->field[16]->store_time_dec(&timestamp, 2);
  }

  sql_mode_string_representation(thd, trigger->sql_mode, &sql_mode_rep);
  table->field[17]->store(sql_mode_rep.str, sql_mode_rep.length, cs);
  table->field[18]->store(definer_buffer.str, definer_buffer.length, cs);
  table->field[19]->store(&trigger->client_cs_name, cs);
  table->field[20]->store(&trigger->connection_cl_name, cs);
  table->field[21]->store(&trigger->db_cl_name, cs);

  return schema_table_store_record(thd, table);
}


static int get_schema_triggers_record(THD *thd, TABLE_LIST *tables,
				      TABLE *table, bool res,
				      const LEX_CSTRING *db_name,
				      const LEX_CSTRING *table_name)
{
  DBUG_ENTER("get_schema_triggers_record");
  /*
    res can be non zero value when processed table is a view or
    error happened during opening of processed table.
  */
  if (res)
  {
    if (unlikely(thd->is_error()))
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                   thd->get_stmt_da()->sql_errno(),
                   thd->get_stmt_da()->message());
    thd->clear_error();
    DBUG_RETURN(0);
  }
  if (!tables->view && tables->table->triggers)
  {
    Table_triggers_list *triggers= tables->table->triggers;
    int event, timing;

    if (check_table_access(thd, TRIGGER_ACL, tables, FALSE, 1, TRUE))
      goto ret;

    for (event= 0; event < (int)TRG_EVENT_MAX; event++)
    {
      for (timing= 0; timing < (int)TRG_ACTION_MAX; timing++)
      {
        Trigger *trigger;
        for (trigger= triggers->
               get_trigger((enum trg_event_type) event,
                           (enum trg_action_time_type) timing) ;
             trigger;
             trigger= trigger->next)
        {
          if (store_trigger(thd, trigger, table, db_name, table_name))
            DBUG_RETURN(1);
        }
      }
    }
  }
ret:
  DBUG_RETURN(0);
}


static void
store_key_column_usage(TABLE *table, const LEX_CSTRING *db_name,
                       const LEX_CSTRING *table_name, const char *key_name,
                       size_t key_len, const char *con_type, size_t con_len,
                       longlong idx)
{
  CHARSET_INFO *cs= system_charset_info;
  table->field[0]->store(STRING_WITH_LEN("def"), cs);
  table->field[1]->store(db_name->str, db_name->length, cs);
  table->field[2]->store(key_name, key_len, cs);
  table->field[3]->store(STRING_WITH_LEN("def"), cs);
  table->field[4]->store(db_name->str, db_name->length, cs);
  table->field[5]->store(table_name->str, table_name->length, cs);
  table->field[6]->store(con_type, con_len, cs);
  table->field[7]->store((longlong) idx, TRUE);
}


static int get_schema_key_column_usage_record(THD *thd,
					      TABLE_LIST *tables,
					      TABLE *table, bool res,
					      const LEX_CSTRING *db_name,
					      const LEX_CSTRING *table_name)
{
  DBUG_ENTER("get_schema_key_column_usage_record");
  if (res)
  {
    if (unlikely(thd->is_error()))
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                   thd->get_stmt_da()->sql_errno(),
                   thd->get_stmt_da()->message());
    thd->clear_error();
    DBUG_RETURN(0);
  }
  else if (!tables->view)
  {
    List<FOREIGN_KEY_INFO> f_key_list;
    TABLE *show_table= tables->table;
    KEY *key_info=show_table->s->key_info;
    uint primary_key= show_table->s->primary_key;
    show_table->file->info(HA_STATUS_VARIABLE |
                           HA_STATUS_NO_LOCK |
                           HA_STATUS_TIME);
    for (uint i=0 ; i < show_table->s->keys ; i++, key_info++)
    {
      if (i != primary_key && !(key_info->flags & HA_NOSAME))
        continue;
      uint f_idx= 0;
      KEY_PART_INFO *key_part= key_info->key_part;
      for (uint j=0 ; j < key_info->user_defined_key_parts ; j++,key_part++)
      {
        if (key_part->field)
        {
          f_idx++;
          restore_record(table, s->default_values);
          store_key_column_usage(table, db_name, table_name,
                                 key_info->name.str, key_info->name.length,
                                 key_part->field->field_name.str,
                                 key_part->field->field_name.length,
                                 (longlong) f_idx);
          if (schema_table_store_record(thd, table))
            DBUG_RETURN(1);
        }
      }
    }

    show_table->file->get_foreign_key_list(thd, &f_key_list);
    FOREIGN_KEY_INFO *f_key_info;
    List_iterator_fast<FOREIGN_KEY_INFO> fkey_it(f_key_list);
    while ((f_key_info= fkey_it++))
    {
      LEX_CSTRING *f_info;
      LEX_CSTRING *r_info;
      List_iterator_fast<LEX_CSTRING> it(f_key_info->foreign_fields),
        it1(f_key_info->referenced_fields);
      uint f_idx= 0;
      while ((f_info= it++))
      {
        r_info= it1++;
        f_idx++;
        restore_record(table, s->default_values);
        store_key_column_usage(table, db_name, table_name,
                               f_key_info->foreign_id->str,
                               f_key_info->foreign_id->length,
                               f_info->str, f_info->length,
                               (longlong) f_idx);
        table->field[8]->store((longlong) f_idx, TRUE);
        table->field[8]->set_notnull();
        table->field[9]->store(f_key_info->referenced_db->str,
                               f_key_info->referenced_db->length,
                               system_charset_info);
        table->field[9]->set_notnull();
        table->field[10]->store(f_key_info->referenced_table->str,
                                f_key_info->referenced_table->length,
                                system_charset_info);
        table->field[10]->set_notnull();
        table->field[11]->store(r_info->str, r_info->length,
                                system_charset_info);
        table->field[11]->set_notnull();
        if (schema_table_store_record(thd, table))
          DBUG_RETURN(1);
      }
    }
  }
  DBUG_RETURN(res);
}


#ifdef WITH_PARTITION_STORAGE_ENGINE
static void collect_partition_expr(THD *thd, List<const char> &field_list,
                                   String *str)
{
  List_iterator<const char> part_it(field_list);
  ulong no_fields= field_list.elements;
  const char *field_str;
  str->length(0);
  while ((field_str= part_it++))
  {
    append_identifier(thd, str, field_str, strlen(field_str));
    if (--no_fields != 0)
      str->append(',');
  }
  return;
}

#endif


static void store_schema_partitions_record(THD *thd, TABLE *schema_table,
                                           TABLE *showing_table,
                                           partition_element *part_elem,
                                           handler *file, uint part_id)
{
  TABLE* table= schema_table;
  CHARSET_INFO *cs= system_charset_info;
  PARTITION_STATS stat_info;
  MYSQL_TIME time;
  file->get_dynamic_partition_info(&stat_info, part_id);
  table->field[0]->store(STRING_WITH_LEN("def"), cs);
  table->field[12]->store((longlong) stat_info.records, TRUE);
  table->field[13]->store((longlong) stat_info.mean_rec_length, TRUE);
  table->field[14]->store((longlong) stat_info.data_file_length, TRUE);
  if (stat_info.max_data_file_length)
  {
    table->field[15]->store((longlong) stat_info.max_data_file_length, TRUE);
    table->field[15]->set_notnull();
  }
  table->field[16]->store((longlong) stat_info.index_file_length, TRUE);
  table->field[17]->store((longlong) stat_info.delete_length, TRUE);
  if (stat_info.create_time)
  {
    thd->variables.time_zone->gmt_sec_to_TIME(&time,
                                              (my_time_t)stat_info.create_time);
    table->field[18]->store_time(&time);
    table->field[18]->set_notnull();
  }
  if (stat_info.update_time)
  {
    thd->variables.time_zone->gmt_sec_to_TIME(&time,
                                              (my_time_t)stat_info.update_time);
    table->field[19]->store_time(&time);
    table->field[19]->set_notnull();
  }
  if (stat_info.check_time)
  {
    thd->variables.time_zone->gmt_sec_to_TIME(&time,
                                              (my_time_t)stat_info.check_time);
    table->field[20]->store_time(&time);
    table->field[20]->set_notnull();
  }
  if (file->ha_table_flags() & (HA_HAS_OLD_CHECKSUM | HA_HAS_NEW_CHECKSUM))
  {
    table->field[21]->store((longlong) stat_info.check_sum, TRUE);
    table->field[21]->set_notnull();
  }
  if (part_elem)
  {
    if (part_elem->part_comment)
      table->field[22]->store(part_elem->part_comment,
                              strlen(part_elem->part_comment), cs);
    else
      table->field[22]->store(STRING_WITH_LEN(""), cs);
    if (part_elem->nodegroup_id != UNDEF_NODEGROUP)
      table->field[23]->store((longlong) part_elem->nodegroup_id, TRUE);
    else
      table->field[23]->store(STRING_WITH_LEN("default"), cs);

    table->field[24]->set_notnull();
    table->field[24]->set_null();               // Tablespace
  }
  return;
}

#ifdef WITH_PARTITION_STORAGE_ENGINE
static int get_partition_column_description(THD *thd, partition_info *part_info,
                                 part_elem_value *list_value, String &tmp_str)
{
  uint num_elements= part_info->part_field_list.elements;
  uint i;
  DBUG_ENTER("get_partition_column_description");

  for (i= 0; i < num_elements; i++)
  {
    part_column_list_val *col_val= &list_value->col_val_array[i];
    if (col_val->max_value)
      tmp_str.append(STRING_WITH_LEN("MAXVALUE"));
    else if (col_val->null_value)
      tmp_str.append(NULL_clex_str);
    else
    {
      Item *item= col_val->item_expression;
      StringBuffer<MAX_KEY_LENGTH> val;
      const Field *field= part_info->part_field_array[i];
      const Type_handler *th= field->type_handler();
      th->partition_field_append_value(&val, item,
                                       field->charset(),
                                       PARTITION_VALUE_PRINT_MODE_SHOW);
      tmp_str.append(val);
    }
    if (i != num_elements - 1)
      tmp_str.append(',');
  }
  DBUG_RETURN(0);
}
#endif /* WITH_PARTITION_STORAGE_ENGINE */

static int get_schema_partitions_record(THD *thd, TABLE_LIST *tables,
                                        TABLE *table, bool res,
                                        const LEX_CSTRING *db_name,
                                        const LEX_CSTRING *table_name)
{
  CHARSET_INFO *cs= system_charset_info;
  char buff[61];
  String tmp_res(buff, sizeof(buff), cs);
  String tmp_str;
  TABLE *show_table= tables->table;
  handler *file;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_info *part_info;
#endif
  DBUG_ENTER("get_schema_partitions_record");

  if (res)
  {
    if (unlikely(thd->is_error()))
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                   thd->get_stmt_da()->sql_errno(),
                   thd->get_stmt_da()->message());
    thd->clear_error();
    DBUG_RETURN(0);
  }
  file= show_table->file;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  part_info= show_table->part_info;
  if (part_info)
  {
    partition_element *part_elem;
    List_iterator<partition_element> part_it(part_info->partitions);
    uint part_pos= 0, part_id= 0;

    restore_record(table, s->default_values);
    table->field[0]->store(STRING_WITH_LEN("def"), cs);
    table->field[1]->store(db_name->str, db_name->length, cs);
    table->field[2]->store(table_name->str, table_name->length, cs);


    /* Partition method*/
    switch (part_info->part_type) {
    case RANGE_PARTITION:
    case LIST_PARTITION:
      tmp_res.length(0);
      if (part_info->part_type == RANGE_PARTITION)
        tmp_res.append(STRING_WITH_LEN("RANGE"));
      else
        tmp_res.append(STRING_WITH_LEN("LIST"));
      if (part_info->column_list)
        tmp_res.append(STRING_WITH_LEN(" COLUMNS"));
      table->field[7]->store(tmp_res.ptr(), tmp_res.length(), cs);
      break;
    case HASH_PARTITION:
      tmp_res.length(0);
      if (part_info->linear_hash_ind)
        tmp_res.append(STRING_WITH_LEN("LINEAR "));
      if (part_info->list_of_part_fields)
        tmp_res.append(STRING_WITH_LEN("KEY"));
      else
        tmp_res.append(STRING_WITH_LEN("HASH"));
      table->field[7]->store(tmp_res.ptr(), tmp_res.length(), cs);
      break;
    case VERSIONING_PARTITION:
      table->field[7]->store(STRING_WITH_LEN("SYSTEM_TIME"), cs);
      break;
    default:
      DBUG_ASSERT(0);
      my_error(ER_OUT_OF_RESOURCES, MYF(ME_FATAL));
      DBUG_RETURN(1);
    }
    table->field[7]->set_notnull();

    /* Partition expression */
    if (part_info->part_expr)
    {
      StringBuffer<STRING_BUFFER_USUAL_SIZE> str(cs);
      part_info->part_expr->print_for_table_def(&str);
      table->field[9]->store(str.ptr(), str.length(), str.charset());
    }
    else if (part_info->list_of_part_fields)
    {
      collect_partition_expr(thd, part_info->part_field_list, &tmp_str);
      table->field[9]->store(tmp_str.ptr(), tmp_str.length(), cs);
    }
    table->field[9]->set_notnull();

    if (part_info->is_sub_partitioned())
    {
      /* Subpartition method */
      tmp_res.length(0);
      if (part_info->linear_hash_ind)
        tmp_res.append(STRING_WITH_LEN("LINEAR "));
      if (part_info->list_of_subpart_fields)
        tmp_res.append(STRING_WITH_LEN("KEY"));
      else
        tmp_res.append(STRING_WITH_LEN("HASH"));
      table->field[8]->store(tmp_res.ptr(), tmp_res.length(), cs);
      table->field[8]->set_notnull();

      /* Subpartition expression */
      if (part_info->subpart_expr)
      {
        StringBuffer<STRING_BUFFER_USUAL_SIZE> str(cs);
        part_info->subpart_expr->print_for_table_def(&str);
        table->field[10]->store(str.ptr(), str.length(), str.charset());
      }
      else if (part_info->list_of_subpart_fields)
      {
        collect_partition_expr(thd, part_info->subpart_field_list, &tmp_str);
        table->field[10]->store(tmp_str.ptr(), tmp_str.length(), cs);
      }
      table->field[10]->set_notnull();
    }

    while ((part_elem= part_it++))
    {
      table->field[3]->store(part_elem->partition_name,
                             strlen(part_elem->partition_name), cs);
      table->field[3]->set_notnull();
      /* PARTITION_ORDINAL_POSITION */
      table->field[5]->store((longlong) ++part_pos, TRUE);
      table->field[5]->set_notnull();

      /* Partition description */
      if (part_info->part_type == RANGE_PARTITION)
      {
        if (part_info->column_list)
        {
          List_iterator<part_elem_value> list_val_it(part_elem->list_val_list);
          part_elem_value *list_value= list_val_it++;
          tmp_str.length(0);
          if (get_partition_column_description(thd, part_info, list_value,
                                               tmp_str))
            DBUG_RETURN(1);
          table->field[11]->store(tmp_str.ptr(), tmp_str.length(), cs);
        }
        else
        {
          if (part_elem->range_value != LONGLONG_MAX)
            table->field[11]->store((longlong) part_elem->range_value, FALSE);
          else
            table->field[11]->store(STRING_WITH_LEN("MAXVALUE"), cs);
        }
        table->field[11]->set_notnull();
      }
      else if (part_info->part_type == LIST_PARTITION)
      {
        List_iterator<part_elem_value> list_val_it(part_elem->list_val_list);
        part_elem_value *list_value;
        uint num_items= part_elem->list_val_list.elements;
        tmp_str.length(0);
        tmp_res.length(0);
        if (part_elem->has_null_value)
        {
          tmp_str.append(NULL_clex_str);
          if (num_items > 0)
            tmp_str.append(',');
        }
        while ((list_value= list_val_it++))
        {
          if (part_info->column_list)
          {
            if (part_info->part_field_list.elements > 1U)
              tmp_str.append(STRING_WITH_LEN("("));
            if (get_partition_column_description(thd, part_info, list_value,
                                                 tmp_str))
              DBUG_RETURN(1);
            if (part_info->part_field_list.elements > 1U)
              tmp_str.append(')');
          }
          else
          {
            if (!list_value->unsigned_flag)
              tmp_res.set(list_value->value, cs);
            else
              tmp_res.set((ulonglong)list_value->value, cs);
            tmp_str.append(tmp_res);
          }
          if (--num_items != 0)
            tmp_str.append(',');
        }
        table->field[11]->store(tmp_str.ptr(), tmp_str.length(), cs);
        table->field[11]->set_notnull();
      }
      else if (part_info->part_type == VERSIONING_PARTITION)
      {
        if (part_elem == part_info->vers_info->now_part)
        {
          table->field[11]->store(STRING_WITH_LEN("CURRENT"), cs);
          table->field[11]->set_notnull();
        }
        else if (part_info->vers_info->interval.is_set())
        {
          Timeval tv((my_time_t) part_elem->range_value, 0);
          table->field[11]->store_timestamp_dec(tv, AUTO_SEC_PART_DIGITS);
          table->field[11]->set_notnull();
        }
      }

      if (part_elem->subpartitions.elements)
      {
        List_iterator<partition_element> sub_it(part_elem->subpartitions);
        partition_element *subpart_elem;
        uint subpart_pos= 0;

        while ((subpart_elem= sub_it++))
        {
          table->field[4]->store(subpart_elem->partition_name,
                                 strlen(subpart_elem->partition_name), cs);
          table->field[4]->set_notnull();
          /* SUBPARTITION_ORDINAL_POSITION */
          table->field[6]->store((longlong) ++subpart_pos, TRUE);
          table->field[6]->set_notnull();

          store_schema_partitions_record(thd, table, show_table, subpart_elem,
                                         file, part_id);
          part_id++;
          if(schema_table_store_record(thd, table))
            DBUG_RETURN(1);
        }
      }
      else
      {
        store_schema_partitions_record(thd, table, show_table, part_elem,
                                       file, part_id);
        part_id++;
        if(schema_table_store_record(thd, table))
          DBUG_RETURN(1);
      }
    }
    DBUG_RETURN(0);
  }
  else
#endif
  {
    store_schema_partitions_record(thd, table, show_table, 0, file, 0);
    if(schema_table_store_record(thd, table))
      DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


#ifdef HAVE_EVENT_SCHEDULER
/*
  Loads an event from mysql.event and copies it's data to a row of
  I_S.EVENTS

  Synopsis
    copy_event_to_schema_table()
      thd         Thread
      sch_table   The schema table (information_schema.event)
      event_table The event table to use for loading (mysql.event).

  Returns
    0  OK
    1  Error
*/

int
copy_event_to_schema_table(THD *thd, TABLE *sch_table, TABLE *event_table)
{
  const char *wild= thd->lex->wild ? thd->lex->wild->ptr() : NullS;
  CHARSET_INFO *scs= system_charset_info;
  MYSQL_TIME time;
  Event_timed et;
  DBUG_ENTER("copy_event_to_schema_table");

  restore_record(sch_table, s->default_values);

  if (et.load_from_row(thd, event_table))
  {
    my_error(ER_CANNOT_LOAD_FROM_TABLE_V2, MYF(0), "mysql", "event");
    DBUG_RETURN(1);
  }

  if (!(!wild || !wild[0] || !wild_case_compare(scs, et.name.str, wild)))
    DBUG_RETURN(0);

  /*
    Skip events in schemas one does not have access to. The check is
    optimized. It's guaranteed in case of SHOW EVENTS that the user
    has access.
  */
  if (thd->lex->sql_command != SQLCOM_SHOW_EVENTS &&
      check_access(thd, EVENT_ACL, et.dbname.str, NULL, NULL, 0, 1))
    DBUG_RETURN(0);

  sch_table->field[ISE_EVENT_CATALOG]->store(STRING_WITH_LEN("def"), scs);
  sch_table->field[ISE_EVENT_SCHEMA]->
                                store(et.dbname.str, et.dbname.length,scs);
  sch_table->field[ISE_EVENT_NAME]->
                                store(et.name.str, et.name.length, scs);
  sch_table->field[ISE_DEFINER]->
                                store(et.definer.str, et.definer.length, scs);
  const String *tz_name= et.time_zone->get_name();
  sch_table->field[ISE_TIME_ZONE]->
                                store(tz_name->ptr(), tz_name->length(), scs);
  sch_table->field[ISE_EVENT_BODY]->
                                store(STRING_WITH_LEN("SQL"), scs);
  sch_table->field[ISE_EVENT_DEFINITION]->store(
    et.body_utf8.str, et.body_utf8.length, scs);

  /* SQL_MODE */
  {
    LEX_CSTRING sql_mode;
    sql_mode_string_representation(thd, et.sql_mode, &sql_mode);
    sch_table->field[ISE_SQL_MODE]->
                                store(sql_mode.str, sql_mode.length, scs);
  }

  int not_used=0;

  if (et.expression)
  {
    String show_str;
    /* type */
    sch_table->field[ISE_EVENT_TYPE]->store(STRING_WITH_LEN("RECURRING"), scs);

    if (Events::reconstruct_interval_expression(&show_str, et.interval,
                                                et.expression))
      DBUG_RETURN(1);

    sch_table->field[ISE_INTERVAL_VALUE]->set_notnull();
    sch_table->field[ISE_INTERVAL_VALUE]->
                                store(show_str.ptr(), show_str.length(), scs);

    LEX_CSTRING *ival= &interval_type_to_name[et.interval];
    sch_table->field[ISE_INTERVAL_FIELD]->set_notnull();
    sch_table->field[ISE_INTERVAL_FIELD]->store(ival->str, ival->length, scs);

    /* starts & ends . STARTS is always set - see sql_yacc.yy */
    et.time_zone->gmt_sec_to_TIME(&time, et.starts);
    sch_table->field[ISE_STARTS]->set_notnull();
    sch_table->field[ISE_STARTS]->store_time(&time);

    if (!et.ends_null)
    {
      et.time_zone->gmt_sec_to_TIME(&time, et.ends);
      sch_table->field[ISE_ENDS]->set_notnull();
      sch_table->field[ISE_ENDS]->store_time(&time);
    }
  }
  else
  {
    /* type */
    sch_table->field[ISE_EVENT_TYPE]->store(STRING_WITH_LEN("ONE TIME"), scs);

    et.time_zone->gmt_sec_to_TIME(&time, et.execute_at);
    sch_table->field[ISE_EXECUTE_AT]->set_notnull();
    sch_table->field[ISE_EXECUTE_AT]->store_time(&time);
  }

  /* status */

  switch (et.status)
  {
    case Event_parse_data::ENABLED:
      sch_table->field[ISE_STATUS]->store(STRING_WITH_LEN("ENABLED"), scs);
      break;
    case Event_parse_data::SLAVESIDE_DISABLED:
      sch_table->field[ISE_STATUS]->store(STRING_WITH_LEN("SLAVESIDE_DISABLED"),
                                          scs);
      break;
    case Event_parse_data::DISABLED:
      sch_table->field[ISE_STATUS]->store(STRING_WITH_LEN("DISABLED"), scs);
      break;
    default:
      DBUG_ASSERT(0);
  }
  sch_table->field[ISE_ORIGINATOR]->store(et.originator, TRUE);

  /* on_completion */
  if (et.on_completion == Event_parse_data::ON_COMPLETION_DROP)
    sch_table->field[ISE_ON_COMPLETION]->
                                store(STRING_WITH_LEN("NOT PRESERVE"), scs);
  else
    sch_table->field[ISE_ON_COMPLETION]->
                                store(STRING_WITH_LEN("PRESERVE"), scs);

  number_to_datetime_or_date(et.created, 0, &time, 0, &not_used);
  DBUG_ASSERT(not_used==0);
  sch_table->field[ISE_CREATED]->store_time(&time);

  number_to_datetime_or_date(et.modified, 0, &time, 0, &not_used);
  DBUG_ASSERT(not_used==0);
  sch_table->field[ISE_LAST_ALTERED]->store_time(&time);

  if (et.last_executed)
  {
    et.time_zone->gmt_sec_to_TIME(&time, et.last_executed);
    sch_table->field[ISE_LAST_EXECUTED]->set_notnull();
    sch_table->field[ISE_LAST_EXECUTED]->store_time(&time);
  }

  sch_table->field[ISE_EVENT_COMMENT]->
                      store(et.comment.str, et.comment.length, scs);

  sch_table->field[ISE_CLIENT_CS]->set_notnull();
  sch_table->field[ISE_CLIENT_CS]->store(&et.creation_ctx->get_client_cs()->
                                         cs_name, scs);
  sch_table->field[ISE_CONNECTION_CL]->set_notnull();
  sch_table->field[ISE_CONNECTION_CL]->store(&et.creation_ctx->
                                             get_connection_cl()->coll_name,
                                             scs);
  sch_table->field[ISE_DB_CL]->set_notnull();
  sch_table->field[ISE_DB_CL]->store(&et.creation_ctx->get_db_cl()->coll_name,
                                     scs);

  if (schema_table_store_record(thd, sch_table))
    DBUG_RETURN(1);

  DBUG_RETURN(0);
}
#endif

int fill_open_tables(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_open_tables");
  const char *wild= thd->lex->wild ? thd->lex->wild->ptr() : NullS;
  TABLE *table= tables->table;
  CHARSET_INFO *cs= system_charset_info;
  OPEN_TABLE_LIST *open_list;
  if (!(open_list= list_open_tables(thd, thd->lex->first_select_lex()->db.str,
                                    wild))
            && thd->is_fatal_error)
    DBUG_RETURN(1);

  for (; open_list ; open_list=open_list->next)
  {
    restore_record(table, s->default_values);
    table->field[0]->store(open_list->db, strlen(open_list->db), cs);
    table->field[1]->store(open_list->table, strlen(open_list->table), cs);
    table->field[2]->store((longlong) open_list->in_use, TRUE);
    table->field[3]->store((longlong) open_list->locked, TRUE);
    if (unlikely(schema_table_store_record(thd, table)))
      DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


int fill_variables(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_variables");
  int res= 0;
  LEX *lex= thd->lex;
  const char *wild= lex->wild ? lex->wild->ptr() : NullS;
  enum enum_schema_tables schema_table_idx=
    get_schema_table_idx(tables->schema_table);
  enum enum_var_type scope= OPT_SESSION;
  bool upper_case_names= lex->sql_command != SQLCOM_SHOW_VARIABLES;
  bool sorted_vars= lex->sql_command == SQLCOM_SHOW_VARIABLES;

  if ((sorted_vars && lex->option_type == OPT_GLOBAL) ||
      schema_table_idx == SCH_GLOBAL_VARIABLES)
    scope= OPT_GLOBAL;

  COND *partial_cond= make_cond_for_info_schema(thd, cond, tables);

  mysql_prlock_rdlock(&LOCK_system_variables_hash);

  /*
    Avoid recursive LOCK_system_variables_hash acquisition in
    intern_sys_var_ptr() by pre-syncing dynamic session variables.
  */
  if (scope == OPT_SESSION &&
      (!thd->variables.dynamic_variables_ptr ||
       global_system_variables.dynamic_variables_head >
       thd->variables.dynamic_variables_head))
    sync_dynamic_session_variables(thd, true);

  res= show_status_array(thd, wild, enumerate_sys_vars(thd, sorted_vars, scope),
                         scope, NULL, "", tables->table,
                         upper_case_names, partial_cond);
  mysql_prlock_unlock(&LOCK_system_variables_hash);
  DBUG_RETURN(res);
}

int add_symbol_to_table(const char* name, TABLE* table){
  DBUG_ENTER("add_symbol_to_table");

  size_t length= strlen(name);

  // If you've added a new SQL keyword longer than KEYWORD_SIZE,
  // please increase the defined max length
  DBUG_ASSERT(length < KEYWORD_SIZE);

  restore_record(table, s->default_values);
  table->field[0]->set_notnull();
  table->field[0]->store(name, length,
                         system_charset_info);
  if (schema_table_store_record(table->in_use, table))
    DBUG_RETURN(1);

  DBUG_RETURN(0);
}

int fill_i_s_keywords(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_i_s_keywords");

  TABLE *table= tables->table;

  for (uint i= 0; i < symbols_length; i++){
    const char *name= symbols[i].name;
    if (add_symbol_to_table(name, table))
      DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}

int fill_i_s_sql_functions(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_i_s_sql_functions");

  TABLE *table= tables->table;

  for (uint i= 0; i < sql_functions_length; i++)
    if (add_symbol_to_table(sql_functions[i].name, table))
      DBUG_RETURN(1);

  for (uint i= 0; i < native_func_registry_array.count(); i++)
    if (add_symbol_to_table(native_func_registry_array.element(i).name.str,
                            table))
      DBUG_RETURN(1);

  DBUG_RETURN(0);
}


int fill_status(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_status");
  LEX *lex= thd->lex;
  const char *wild= lex->wild ? lex->wild->ptr() : NullS;
  int res= 0;
  STATUS_VAR *tmp1, tmp;
  enum enum_schema_tables schema_table_idx=
    get_schema_table_idx(tables->schema_table);
  enum enum_var_type scope;
  bool upper_case_names= lex->sql_command != SQLCOM_SHOW_STATUS;

  if (lex->sql_command == SQLCOM_SHOW_STATUS)
  {
    scope= lex->option_type;
    if (scope == OPT_GLOBAL)
      tmp1= &tmp;
    else
      tmp1= thd->initial_status_var;
  }
  else if (schema_table_idx == SCH_GLOBAL_STATUS)
  {
    scope= OPT_GLOBAL;
    tmp1= &tmp;
  }
  else
  {
    scope= OPT_SESSION;
    tmp1= &thd->status_var;
  }

  COND *partial_cond= make_cond_for_info_schema(thd, cond, tables);
  // Evaluate and cache const subqueries now, before the mutex.
  if (partial_cond)
    partial_cond->val_int();

  tmp.local_memory_used= 0; // meaning tmp was not populated yet

  mysql_rwlock_rdlock(&LOCK_all_status_vars);
  res= show_status_array(thd, wild,
                         (SHOW_VAR *)all_status_vars.buffer,
                         scope, tmp1, "", tables->table,
                         upper_case_names, partial_cond);
  mysql_rwlock_unlock(&LOCK_all_status_vars);
  DBUG_RETURN(res);
}


/*
  Fill and store records into I_S.referential_constraints table

  SYNOPSIS
    get_referential_constraints_record()
    thd                 thread handle
    tables              table list struct(processed table)
    table               I_S table
    res                 1 means the error during opening of the processed table
                        0 means processed table is opened without error
    base_name           db name
    file_name           table name

  RETURN
    0	ok
    #   error
*/

static int
get_referential_constraints_record(THD *thd, TABLE_LIST *tables,
                                   TABLE *table, bool res,
                                   const LEX_CSTRING *db_name,
                                   const LEX_CSTRING *table_name)
{
  CHARSET_INFO *cs= system_charset_info;
  LEX_CSTRING *s;
  DBUG_ENTER("get_referential_constraints_record");

  if (res)
  {
    if (unlikely(thd->is_error()))
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                   thd->get_stmt_da()->sql_errno(),
                   thd->get_stmt_da()->message());
    thd->clear_error();
    DBUG_RETURN(0);
  }
  if (!tables->view)
  {
    List<FOREIGN_KEY_INFO> f_key_list;
    TABLE *show_table= tables->table;
    show_table->file->info(HA_STATUS_VARIABLE |
                           HA_STATUS_NO_LOCK |
                           HA_STATUS_TIME);

    show_table->file->get_foreign_key_list(thd, &f_key_list);
    FOREIGN_KEY_INFO *f_key_info;
    List_iterator_fast<FOREIGN_KEY_INFO> it(f_key_list);
    while ((f_key_info= it++))
    {
      restore_record(table, s->default_values);
      table->field[0]->store(STRING_WITH_LEN("def"), cs);
      table->field[1]->store(db_name->str, db_name->length, cs);
      table->field[9]->store(table_name->str, table_name->length, cs);
      table->field[2]->store(f_key_info->foreign_id->str,
                             f_key_info->foreign_id->length, cs);
      table->field[3]->store(STRING_WITH_LEN("def"), cs);
      table->field[4]->store(f_key_info->referenced_db->str, 
                             f_key_info->referenced_db->length, cs);
      table->field[10]->store(f_key_info->referenced_table->str,
                             f_key_info->referenced_table->length, cs);
      if (f_key_info->referenced_key_name)
      {
        table->field[5]->store(f_key_info->referenced_key_name->str,
                               f_key_info->referenced_key_name->length, cs);
        table->field[5]->set_notnull();
      }
      else
        table->field[5]->set_null();
      table->field[6]->store(STRING_WITH_LEN("NONE"), cs);
      s= fk_option_name(f_key_info->update_method);
      table->field[7]->store(s->str, s->length, cs);
      s= fk_option_name(f_key_info->delete_method);
      table->field[8]->store(s->str, s->length, cs);
      if (schema_table_store_record(thd, table))
        DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}

struct schema_table_ref
{
  const char *table_name;
  ST_SCHEMA_TABLE *schema_table;
};

/*
  Find schema_tables elment by name

  SYNOPSIS
    find_schema_table_in_plugin()
    thd                 thread handler
    plugin              plugin
    table_name          table name

  RETURN
    0	table not found
    1   found the schema table
*/
static my_bool find_schema_table_in_plugin(THD *thd, plugin_ref plugin,
                                           void* p_table)
{
  schema_table_ref *p_schema_table= (schema_table_ref *)p_table;
  const char* table_name= p_schema_table->table_name;
  ST_SCHEMA_TABLE *schema_table= plugin_data(plugin, ST_SCHEMA_TABLE *);
  DBUG_ENTER("find_schema_table_in_plugin");

  if (!my_strcasecmp(system_charset_info,
                     schema_table->table_name,
                     table_name))
  {
    my_plugin_lock(thd, plugin);
    p_schema_table->schema_table= schema_table;
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}


/*
  Find schema_tables element by name

  SYNOPSIS
    find_schema_table()
    thd                 thread handler
    table_name          table name

  RETURN
    0	table not found
    #   pointer to 'schema_tables' element
*/

ST_SCHEMA_TABLE *find_schema_table(THD *thd, const LEX_CSTRING *table_name,
                                   bool *in_plugin)
{
  schema_table_ref schema_table_a;
  ST_SCHEMA_TABLE *schema_table= schema_tables;
  DBUG_ENTER("find_schema_table");

  *in_plugin= false;
  for (; schema_table->table_name; schema_table++)
  {
    if (!my_strcasecmp(system_charset_info,
                       schema_table->table_name,
                       table_name->str))
      DBUG_RETURN(schema_table);
  }

  *in_plugin= true;
  schema_table_a.table_name= table_name->str;
  if (plugin_foreach(thd, find_schema_table_in_plugin,
                     MYSQL_INFORMATION_SCHEMA_PLUGIN, &schema_table_a))
    DBUG_RETURN(schema_table_a.schema_table);

  DBUG_RETURN(NULL);
}


ST_SCHEMA_TABLE *get_schema_table(enum enum_schema_tables schema_table_idx)
{
  return &schema_tables[schema_table_idx];
}

/**
  Create information_schema table using schema_table data.

  @note
    For MYSQL_TYPE_DECIMAL fields only, the field_length member has encoded
    into it two numbers, based on modulus of base-10 numbers.  In the ones
    position is the number of decimals.  Tens position is unused.  In the
    hundreds and thousands position is a two-digit decimal number representing
    length.  Encode this value with  (length*100)+decimals  , where
    0<decimals<10 and 0<=length<100 .

  @param
    thd                   thread handler

  @param table_list Used to pass I_S table information(fields info, tables
  parameters etc) and table name.

  @retval  \#             Pointer to created table
  @retval  NULL           Can't create table
*/

TABLE *create_schema_table(THD *thd, TABLE_LIST *table_list)
{
  uint field_count= 0;
  TABLE *table;
  ST_SCHEMA_TABLE *schema_table= table_list->schema_table;
  ST_FIELD_INFO *fields= schema_table->fields_info;
  bool need_all_fieds= table_list->schema_table_reformed || // SHOW command
                       thd->lex->only_view_structure(); // need table structure
  DBUG_ENTER("create_schema_table");

  for (; !fields->end_marker(); fields++)
    field_count++;

  TMP_TABLE_PARAM *tmp_table_param = new (thd->mem_root) TMP_TABLE_PARAM;
  tmp_table_param->init();
  tmp_table_param->table_charset= system_charset_info;
  tmp_table_param->field_count= field_count;
  tmp_table_param->schema_table= 1;
  SELECT_LEX *select_lex= table_list->select_lex;
  bool keep_row_order= is_show_command(thd);
  if (!(table= create_tmp_table_for_schema(thd, tmp_table_param, *schema_table,
                 (select_lex->options | thd->variables.option_bits | TMP_TABLE_ALL_COLUMNS),
                  table_list->alias, !need_all_fieds, keep_row_order)))
    DBUG_RETURN(0);
  my_bitmap_map* bitmaps=
    (my_bitmap_map*) thd->alloc(bitmap_buffer_size(field_count));
  my_bitmap_init(&table->def_read_set, (my_bitmap_map*) bitmaps, field_count);
  table->read_set= &table->def_read_set;
  bitmap_clear_all(table->read_set);
  table_list->schema_table_param= tmp_table_param;
  DBUG_RETURN(table);
}


/*
  For old SHOW compatibility. It is used when
  old SHOW doesn't have generated column names
  Make list of fields for SHOW

  SYNOPSIS
    make_old_format()
    thd			thread handler
    schema_table        pointer to 'schema_tables' element

  RETURN
   1	error
   0	success
*/

static int make_old_format(THD *thd, ST_SCHEMA_TABLE *schema_table)
{
  ST_FIELD_INFO *field_info= schema_table->fields_info;
  Name_resolution_context *context= &thd->lex->first_select_lex()->context;
  for (; !field_info->end_marker(); field_info++)
  {
    if (field_info->old_name().str)
    {
      LEX_CSTRING field_name= field_info->name();
      Item_field *field= new (thd->mem_root)
        Item_field(thd, context, field_name);
      if (field)
      {
        field->set_name(thd, field_info->old_name());
        if (add_item_to_list(thd, field))
          return 1;
      }
    }
  }
  return 0;
}


int make_schemata_old_format(THD *thd, ST_SCHEMA_TABLE *schema_table)
{
  char tmp[128];
  LEX *lex= thd->lex;
  SELECT_LEX *sel= lex->current_select;
  Name_resolution_context *context= &sel->context;

  if (!sel->item_list.elements)
  {
    ST_FIELD_INFO *field_info= &schema_table->fields_info[1];
    String buffer(tmp,sizeof(tmp), system_charset_info);
    Item_field *field= new (thd->mem_root) Item_field(thd, context,
                                                      field_info->name());
    if (!field || add_item_to_list(thd, field))
      return 1;
    buffer.length(0);
    buffer.append(field_info->old_name());
    if (lex->wild && lex->wild->ptr())
    {
      buffer.append(STRING_WITH_LEN(" ("));
      buffer.append(*lex->wild);
      buffer.append(')');
    }
    field->set_name(thd, &buffer);
  }
  return 0;
}


int make_table_names_old_format(THD *thd, ST_SCHEMA_TABLE *schema_table)
{
  char tmp[128];
  String buffer(tmp, sizeof(tmp), system_charset_info);
  LEX *lex= thd->lex;
  Name_resolution_context *context= &lex->first_select_lex()->context;
  ST_FIELD_INFO *field_info= &schema_table->fields_info[2];
  LEX_CSTRING field_name= field_info->name();

  buffer.length(0);
  buffer.append(field_info->old_name());
  buffer.append(&lex->first_select_lex()->db);
  if (lex->wild && lex->wild->ptr())
  {
    buffer.append(STRING_WITH_LEN(" ("));
    buffer.append(*lex->wild);
    buffer.append(')');
  }
  Item_field *field= new (thd->mem_root) Item_field(thd, context, field_name);
  if (add_item_to_list(thd, field))
    return 1;
  field->set_name(thd, &buffer);
  if (thd->lex->verbose)
  {
    field_info= &schema_table->fields_info[3];
    field= new (thd->mem_root) Item_field(thd, context, field_info->name());
    if (add_item_to_list(thd, field))
      return 1;
    field->set_name(thd, field_info->old_name());
  }
  return 0;
}


int make_columns_old_format(THD *thd, ST_SCHEMA_TABLE *schema_table)
{
  int fields_arr[]= {3, 15, 14, 6, 16, 5, 17, 18, 19, -1};
  int *field_num= fields_arr;
  ST_FIELD_INFO *field_info;
  Name_resolution_context *context= &thd->lex->first_select_lex()->context;

  for (; *field_num >= 0; field_num++)
  {
    field_info= &schema_table->fields_info[*field_num];
    if (!thd->lex->verbose && (*field_num == 14 ||
                               *field_num == 18 ||
                               *field_num == 19))
      continue;
    Item_field *field= new (thd->mem_root) Item_field(thd, context,
                                                      field_info->name());
    if (field)
    {
      field->set_name(thd, field_info->old_name());
      if (add_item_to_list(thd, field))
        return 1;
    }
  }
  return 0;
}


int make_character_sets_old_format(THD *thd, ST_SCHEMA_TABLE *schema_table)
{
  int fields_arr[]= {0, 2, 1, 3, -1};
  int *field_num= fields_arr;
  ST_FIELD_INFO *field_info;
  Name_resolution_context *context= &thd->lex->first_select_lex()->context;

  for (; *field_num >= 0; field_num++)
  {
    field_info= &schema_table->fields_info[*field_num];
    Item_field *field= new (thd->mem_root) Item_field(thd, context,
                                                      field_info->name());
    if (field)
    {
      field->set_name(thd, field_info->old_name());
      if (add_item_to_list(thd, field))
        return 1;
    }
  }
  return 0;
}


int make_proc_old_format(THD *thd, ST_SCHEMA_TABLE *schema_table)
{
  int fields_arr[]= {2, 3, 4, 27, 24, 23, 22, 26, 28, 29, 30, -1};
  int *field_num= fields_arr;
  ST_FIELD_INFO *field_info;
  Name_resolution_context *context= &thd->lex->first_select_lex()->context;

  for (; *field_num >= 0; field_num++)
  {
    field_info= &schema_table->fields_info[*field_num];
    Item_field *field= new (thd->mem_root) Item_field(thd, context,
                                                      field_info->name());
    if (field)
    {
      field->set_name(thd, field_info->old_name());
      if (add_item_to_list(thd, field))
        return 1;
    }
  }
  return 0;
}


/*
  Create information_schema table

  SYNOPSIS
  mysql_schema_table()
    thd                thread handler
    lex                pointer to LEX
    table_list         pointer to table_list

  RETURN
    0	success
    1   error
*/

int mysql_schema_table(THD *thd, LEX *lex, TABLE_LIST *table_list)
{
  TABLE *table;
  DBUG_ENTER("mysql_schema_table");
  if (!(table= create_schema_table(thd, table_list)))
    DBUG_RETURN(1);
  table->s->tmp_table= SYSTEM_TMP_TABLE;
  table->grant.privilege= SELECT_ACL;
  /*
    This test is necessary to make
    case insensitive file systems +
    upper case table names(information schema tables) +
    views
    working correctly
  */
  if (table_list->schema_table_name.str)
    table->alias_name_used= my_strcasecmp(table_alias_charset,
                                          table_list->schema_table_name.str,
                                          table_list->alias.str);
  table_list->table= table;
  table->next= thd->derived_tables;
  thd->derived_tables= table;
  table_list->select_lex->options |= OPTION_SCHEMA_TABLE;
  lex->safe_to_cache_query= 0;

  if (table_list->schema_table_reformed) // show command
  {
    SELECT_LEX *sel= lex->current_select;
    Item *item;
    Field_translator *transl, *org_transl;

    if (table_list->field_translation)
    {
      Field_translator *end= table_list->field_translation_end;
      for (transl= table_list->field_translation; transl < end; transl++)
      {
        if (transl->item->fix_fields_if_needed(thd, &transl->item))
          DBUG_RETURN(1);
      }
      DBUG_RETURN(0);
    }
    List_iterator_fast<Item> it(sel->item_list);
    if (!(transl=
          (Field_translator*)(thd->stmt_arena->
                              alloc(sel->item_list.elements *
                                    sizeof(Field_translator)))))
    {
      DBUG_RETURN(1);
    }
    for (org_transl= transl; (item= it++); transl++)
    {
      transl->item= item;
      transl->name= item->name;
      if (item->fix_fields_if_needed(thd, &transl->item))
        DBUG_RETURN(1);
    }
    table_list->field_translation= org_transl;
    table_list->field_translation_end= transl;
  }

  DBUG_RETURN(0);
}


/*
  Generate select from information_schema table

  SYNOPSIS
    make_schema_select()
    thd                  thread handler
    sel                  pointer to SELECT_LEX
    schema_table_idx     index of 'schema_tables' element

  RETURN
    0	success
    1   error
*/

int make_schema_select(THD *thd, SELECT_LEX *sel,
                       ST_SCHEMA_TABLE *schema_table)
{
  LEX_CSTRING db, table;
  DBUG_ENTER("make_schema_select");
  DBUG_PRINT("enter", ("mysql_schema_select: %s", schema_table->table_name));
  /*
     We have to make non const db_name & table_name
     because of lower_case_table_names
  */
  if (!thd->make_lex_string(&db, INFORMATION_SCHEMA_NAME.str,
                            INFORMATION_SCHEMA_NAME.length))
    DBUG_RETURN(1);

  if (!thd->make_lex_string(&table, schema_table->table_name,
                            strlen(schema_table->table_name)))
    DBUG_RETURN(1);

  if (schema_table->old_format(thd, schema_table))
    DBUG_RETURN(1);

  if (!sel->add_table_to_list(thd, new Table_ident(thd, &db, &table, 0),
                              0, 0, TL_READ, MDL_SHARED_READ))
    DBUG_RETURN(1);

  sel->table_list.first->schema_table_reformed= 1;
  DBUG_RETURN(0);
}


/*
  Optimize reading from an I_S table.

  @detail
    This function prepares a plan for populating an I_S table with 
    get_all_tables().

    The plan is in IS_table_read_plan structure, it is saved in
    tables->is_table_read_plan.

  @return
    false - Ok
    true  - Out Of Memory
    
*/

static bool optimize_for_get_all_tables(THD *thd, TABLE_LIST *tables, COND *cond)
{
  SELECT_LEX *lsel= tables->schema_select_lex;
  ST_SCHEMA_TABLE *schema_table= tables->schema_table;
  enum enum_schema_tables schema_table_idx;
  IS_table_read_plan *plan;
  DBUG_ENTER("get_all_tables");

  if (!(plan= new IS_table_read_plan()))
    DBUG_RETURN(1);

  tables->is_table_read_plan= plan;

  schema_table_idx= get_schema_table_idx(schema_table);
  tables->table_open_method= get_table_open_method(tables, schema_table, 
                                                   schema_table_idx);
  DBUG_PRINT("open_method", ("%d", tables->table_open_method));

  /* 
    this branch processes SHOW FIELDS, SHOW INDEXES commands.
    see sql_parse.cc, prepare_schema_table() function where
    this values are initialized
  */
  if (lsel && lsel->table_list.first)
  {
    /* These do not need to have a query plan */
    plan->trivial_show_command= true;
    goto end;
  }

  if (get_lookup_field_values(thd, cond, tables, &plan->lookup_field_vals))
  {
    plan->no_rows= true;
    goto end;
  }

  DBUG_PRINT("info",("db_name='%s', table_name='%s'",
                     plan->lookup_field_vals.db_value.str,
                     plan->lookup_field_vals.table_value.str));

  if (!plan->lookup_field_vals.wild_db_value && 
      !plan->lookup_field_vals.wild_table_value)
  {
    /*
      if lookup value is empty string then
      it's impossible table name or db name
    */
    if ((plan->lookup_field_vals.db_value.str &&
         !plan->lookup_field_vals.db_value.str[0]) ||
        (plan->lookup_field_vals.table_value.str &&
         !plan->lookup_field_vals.table_value.str[0]))
    {
      plan->no_rows= true;
      goto end;
    }
  }

  if (plan->has_db_lookup_value() && plan->has_table_lookup_value())
    plan->partial_cond= 0;
  else
    plan->partial_cond= make_cond_for_info_schema(thd, cond, tables);
  
end:
  DBUG_RETURN(0);
}


bool optimize_schema_tables_memory_usage(List<TABLE_LIST> &tables)
{
  DBUG_ENTER("optimize_schema_tables_memory_usage");

  List_iterator<TABLE_LIST> tli(tables);

  while (TABLE_LIST *table_list= tli++)
  {
    if (!table_list->schema_table)
      continue;

    TABLE *table= table_list->table;
    THD *thd=table->in_use;

    if (!thd->fill_information_schema_tables())
      continue;

    if (!table->is_created())
    {
      TMP_TABLE_PARAM *p= table_list->schema_table_param;
      TMP_ENGINE_COLUMNDEF *from_recinfo, *to_recinfo;
      DBUG_ASSERT(table->s->keys == 0);
      DBUG_ASSERT(table->s->uniques == 0);

      uchar *cur= table->field[0]->ptr;
      /* first recinfo could be a NULL bitmap, not an actual Field */
      from_recinfo= to_recinfo= p->start_recinfo + (cur != table->record[0]);
      for (uint i=0; i < table->s->fields; i++, from_recinfo++)
      {
        Field *field= table->field[i];
        DBUG_ASSERT(field->vcol_info == 0);
        DBUG_ASSERT(from_recinfo->length);
        DBUG_ASSERT(from_recinfo->length == field->pack_length_in_rec());
        if (bitmap_is_set(table->read_set, i))
        {
          field->move_field(cur);
          field->reset();
          *to_recinfo++= *from_recinfo;
          cur+= from_recinfo->length;
        }
        else
        {
          field= new (thd->mem_root) Field_string(cur, 0, field->null_ptr,
                                field->null_bit, Field::NONE,
                                &field->field_name, field->dtcollation());
          field->init(table);
          field->field_index= i;
          DBUG_ASSERT(field->pack_length_in_rec() == 0);
          table->field[i]= field;
        }
      }
      if ((table->s->reclength= (ulong)(cur - table->record[0])) == 0)
      {
        /* all fields were optimized away. Force a non-0-length row */
        table->s->reclength= to_recinfo->length= 1;
        to_recinfo->type= FIELD_NORMAL;
        to_recinfo++;
      }
      store_record(table, s->default_values);
      p->recinfo= to_recinfo;

      // TODO switch from Aria to Memory if all blobs were optimized away?
      if (instantiate_tmp_table(table, p->keyinfo, p->start_recinfo, &p->recinfo,
                   table_list->select_lex->options | thd->variables.option_bits))
        DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}


/*
  This is the optimizer part of get_schema_tables_result().
*/

bool optimize_schema_tables_reads(JOIN *join)
{
  THD *thd= join->thd;
  DBUG_ENTER("optimize_schema_tables_reads");

  JOIN_TAB *tab;
  for (tab= first_linear_tab(join, WITHOUT_BUSH_ROOTS, WITH_CONST_TABLES);
       tab; 
       tab= next_linear_tab(join, tab, WITH_BUSH_ROOTS))
  {
    if (!tab->table || !tab->table->pos_in_table_list)
      continue;

    TABLE_LIST *table_list= tab->table->pos_in_table_list;
    if (table_list->schema_table && thd->fill_information_schema_tables())
    {
      /* A value of 0 indicates a dummy implementation */
      if (table_list->schema_table->fill_table == 0)
        continue;

      /* skip I_S optimizations specific to get_all_tables */
      if (table_list->schema_table->fill_table != get_all_tables)
        continue;

      Item *cond= tab->select_cond;
      if (tab->cache_select && tab->cache_select->cond)
      {
        /*
          If join buffering is used, we should use the condition that is
          attached to the join cache. Cache condition has a part of WHERE that
          can be checked when we're populating this table.
          join_tab->select_cond is of no interest, because it only has
          conditions that depend on both this table and previous tables in the
          join order.
        */
        cond= tab->cache_select->cond;
      }
      if (optimize_for_get_all_tables(thd, table_list, cond))
        DBUG_RETURN(1);   // Handle OOM
    }
  }
  DBUG_RETURN(0);
}


/*
  Fill temporary schema tables before SELECT

  SYNOPSIS
    get_schema_tables_result()
    join  join which use schema tables
    executed_place place where I_S table processed

  SEE ALSO
    The optimization part is done by get_schema_tables_result(). This function
    is run on query execution.

  RETURN
    FALSE success
    TRUE  error
*/

bool get_schema_tables_result(JOIN *join,
                              enum enum_schema_table_state executed_place)
{
  THD *thd= join->thd;
  LEX *lex= thd->lex;
  bool result= 0;
  PSI_stage_info org_stage;
  DBUG_ENTER("get_schema_tables_result");

  Warnings_only_error_handler err_handler;
  thd->push_internal_handler(&err_handler);
  thd->backup_stage(&org_stage);
  THD_STAGE_INFO(thd, stage_filling_schema_table);

  JOIN_TAB *tab;
  for (tab= first_linear_tab(join, WITHOUT_BUSH_ROOTS, WITH_CONST_TABLES);
       tab; 
       tab= next_linear_tab(join, tab, WITH_BUSH_ROOTS))
  {
    if (!tab->table || !tab->table->pos_in_table_list)
      break;

    TABLE_LIST *table_list= tab->table->pos_in_table_list;
    if (table_list->schema_table && thd->fill_information_schema_tables())
    {
      /*
        I_S tables only need to be re-populated if make_cond_for_info_schema()
        preserves outer fields
      */
      bool is_subselect= &lex->unit != lex->current_select->master_unit() &&
                         lex->current_select->master_unit()->item &&
                         tab->select_cond &&
                         tab->select_cond->used_tables() & OUTER_REF_TABLE_BIT;

      /* A value of 0 indicates a dummy implementation */
      if (table_list->schema_table->fill_table == 0)
        continue;

      /*
        Do not fill in tables thare are marked as JT_CONST as these will never
        be read and they also don't have a tab->read_record.table set!
        This can happen with queries like
        SELECT * FROM t1 LEFT JOIN (t1 AS t1b JOIN INFORMATION_SCHEMA.ROUTINES)
        ON (t1b.a IS NULL);
      */
      if (tab->type == JT_CONST)
        continue;

      /* skip I_S optimizations specific to get_all_tables */
      if (lex->describe &&
          (table_list->schema_table->fill_table != get_all_tables))
        continue;

      /*
        If schema table is already processed and the statement is not a
        subselect then we don't need to fill this table again. If schema table
        is already processed and schema_table_state != executed_place then
        table is already processed and we should skip second data processing.
      */
      if (table_list->schema_table_state &&
          (!is_subselect || table_list->schema_table_state != executed_place))
        continue;

      /*
        if table is used in a subselect and
        table has been processed earlier with the same
        'executed_place' value then we should refresh the table.
      */
      if (table_list->schema_table_state && is_subselect)
      {
        table_list->table->file->extra(HA_EXTRA_NO_CACHE);
        table_list->table->file->extra(HA_EXTRA_RESET_STATE);
        table_list->table->file->ha_delete_all_rows();
        table_list->table->null_row= 0;
      }
      else
        table_list->table->file->stats.records= 0;
  
      Item *cond= tab->select_cond;
      if (tab->cache_select && tab->cache_select->cond)
      {
        /*
          If join buffering is used, we should use the condition that is
          attached to the join cache. Cache condition has a part of WHERE that
          can be checked when we're populating this table.
          join_tab->select_cond is of no interest, because it only has
          conditions that depend on both this table and previous tables in the
          join order.
        */
        cond= tab->cache_select->cond;
      }

      Switch_to_definer_security_ctx backup_ctx(thd, table_list);
      Check_level_instant_set check_level_save(thd, CHECK_FIELD_IGNORE);
      if (table_list->schema_table->fill_table(thd, table_list, cond))
      {
        result= 1;
        join->error= 1;
        tab->read_record.table->file= table_list->table->file;
        table_list->schema_table_state= executed_place;
        break;
      }
      tab->read_record.table->file= table_list->table->file;
      table_list->schema_table_state= executed_place;
    }
  }
  thd->pop_internal_handler();
  if (unlikely(thd->is_error()))
  {
    /*
      This hack is here, because I_S code uses thd->clear_error() a lot.
      Which means, a Warnings_only_error_handler cannot handle the error
      corectly as it does not know whether an error is real (e.g. caused
      by tab->select_cond->val_int()) or will be cleared later.
      Thus it ignores all errors, and the real one (that is, the error
      that was not cleared) is pushed now.

      It also means that an audit plugin cannot process the error correctly
      either. See also thd->clear_error()
    */
    thd->get_stmt_da()->push_warning(thd, thd->get_stmt_da()->sql_errno(),
                                     thd->get_stmt_da()->get_sqlstate(),
                                     Sql_condition::WARN_LEVEL_ERROR,
                                     thd->get_stmt_da()->message());
  }
  else if (result)
    my_error(ER_UNKNOWN_ERROR, MYF(0));
  THD_STAGE_INFO(thd, org_stage);
  DBUG_RETURN(result);
}

struct run_hton_fill_schema_table_args
{
  TABLE_LIST *tables;
  COND *cond;
};

static my_bool run_hton_fill_schema_table(THD *thd, plugin_ref plugin,
                                          void *arg)
{
  struct run_hton_fill_schema_table_args *args=
    (run_hton_fill_schema_table_args *) arg;
  handlerton *hton= plugin_hton(plugin);
  if (hton->fill_is_table)
      hton->fill_is_table(hton, thd, args->tables, args->cond,
            get_schema_table_idx(args->tables->schema_table));
  return false;
}

int hton_fill_schema_table(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("hton_fill_schema_table");

  struct run_hton_fill_schema_table_args args;
  args.tables= tables;
  args.cond= cond;

  plugin_foreach(thd, run_hton_fill_schema_table,
                 MYSQL_STORAGE_ENGINE_PLUGIN, &args);

  DBUG_RETURN(0);
}


static
int store_key_cache_table_record(THD *thd, TABLE *table,
                                 const char *name, size_t name_length,
                                 KEY_CACHE *key_cache,
                                 uint partitions, uint partition_no)
{
  KEY_CACHE_STATISTICS keycache_stats;
  uint err;
  DBUG_ENTER("store_key_cache_table_record");

  get_key_cache_statistics(key_cache, partition_no, &keycache_stats);

  if (!key_cache->key_cache_inited || keycache_stats.mem_size == 0)
    DBUG_RETURN(0);

  restore_record(table, s->default_values);
  table->field[0]->store(name, name_length, system_charset_info);
  if (partitions == 0)
    table->field[1]->set_null();
  else
  {
    table->field[1]->set_notnull(); 
    table->field[1]->store((long) partitions, TRUE);
  }

  if (partition_no == 0)
    table->field[2]->set_null();
  else
  {
    table->field[2]->set_notnull();
    table->field[2]->store((long) partition_no, TRUE);
  }
  table->field[3]->store(keycache_stats.mem_size, TRUE);
  table->field[4]->store(keycache_stats.block_size, TRUE);
  table->field[5]->store(keycache_stats.blocks_used, TRUE);
  table->field[6]->store(keycache_stats.blocks_unused, TRUE);
  table->field[7]->store(keycache_stats.blocks_changed, TRUE);
  table->field[8]->store(keycache_stats.read_requests, TRUE);
  table->field[9]->store(keycache_stats.reads, TRUE);
  table->field[10]->store(keycache_stats.write_requests, TRUE);
  table->field[11]->store(keycache_stats.writes, TRUE);

  err= schema_table_store_record(thd, table);
  DBUG_RETURN(err);
}

int run_fill_key_cache_tables(const char *name, KEY_CACHE *key_cache, void *p)
{
  DBUG_ENTER("run_fill_key_cache_tables");

  if (!key_cache->key_cache_inited)
    DBUG_RETURN(0);

  TABLE *table= (TABLE *)p;
  THD *thd= table->in_use;
  uint partitions= key_cache->partitions;    
  size_t namelen= strlen(name);
  DBUG_ASSERT(partitions <= MAX_KEY_CACHE_PARTITIONS);

  if (partitions)
  {
    for (uint i= 0; i < partitions; i++)
    {
      if (store_key_cache_table_record(thd, table, name, namelen,
                                       key_cache, partitions, i+1))
        DBUG_RETURN(1);
    }
  }

  if (store_key_cache_table_record(thd, table, name, namelen,
                                   key_cache, partitions, 0))
    DBUG_RETURN(1);
  DBUG_RETURN(0);
}

int fill_key_cache_tables(THD *thd, TABLE_LIST *tables, COND *cond)
{
  DBUG_ENTER("fill_key_cache_tables");

  int res= process_key_caches(run_fill_key_cache_tables, tables->table);

  DBUG_RETURN(res);
}


namespace Show {

ST_FIELD_INFO schema_fields_info[]=
{
  Column("CATALOG_NAME",               Catalog(),        NOT_NULL),
  Column("SCHEMA_NAME",                Name(),           NOT_NULL, "Database"),
  Column("DEFAULT_CHARACTER_SET_NAME", CSName(),         NOT_NULL),
  Column("DEFAULT_COLLATION_NAME",     CSName(),         NOT_NULL),
  Column("SQL_PATH",                   Varchar(FN_REFLEN),   NULLABLE),
  Column("SCHEMA_COMMENT", Varchar(DATABASE_COMMENT_MAXLEN), NOT_NULL),
  CEnd()
};


ST_FIELD_INFO tables_fields_info[]=
{
  Column("TABLE_CATALOG",   Catalog(),   NOT_NULL),
  Column("TABLE_SCHEMA",    Name(),      NOT_NULL),
  Column("TABLE_NAME",      Name(),      NOT_NULL, "Name"),
  Column("TABLE_TYPE",      Name(),      NOT_NULL,               OPEN_FRM_ONLY),
  Column("ENGINE",          Name(),      NULLABLE, "Engine",     OPEN_FRM_ONLY),
  Column("VERSION",         ULonglong(), NULLABLE, "Version",    OPEN_FRM_ONLY),
  Column("ROW_FORMAT",      Varchar(10), NULLABLE, "Row_format", OPEN_FULL_TABLE),
  Column("TABLE_ROWS",      ULonglong(), NULLABLE, "Rows",       OPEN_FULL_TABLE),
  Column("AVG_ROW_LENGTH",  ULonglong(), NULLABLE, "Avg_row_length",
                                                                 OPEN_FULL_TABLE),
  Column("DATA_LENGTH",     ULonglong(), NULLABLE, "Data_length",OPEN_FULL_TABLE),
  Column("MAX_DATA_LENGTH", ULonglong(), NULLABLE, "Max_data_length",
                                                                 OPEN_FULL_TABLE),
  Column("INDEX_LENGTH",    ULonglong(), NULLABLE, "Index_length",OPEN_FULL_TABLE),
  Column("DATA_FREE",       ULonglong(), NULLABLE, "Data_free",  OPEN_FULL_TABLE),
  Column("AUTO_INCREMENT",  ULonglong(), NULLABLE, "Auto_increment",
                                                                 OPEN_FULL_TABLE),
  Column("CREATE_TIME",     Datetime(0), NULLABLE, "Create_time",OPEN_FULL_TABLE),
  Column("UPDATE_TIME",     Datetime(0), NULLABLE, "Update_time",OPEN_FULL_TABLE),
  Column("CHECK_TIME",      Datetime(0), NULLABLE, "Check_time", OPEN_FULL_TABLE),
  Column("TABLE_COLLATION", CSName(),    NULLABLE, "Collation",  OPEN_FRM_ONLY),
  Column("CHECKSUM",        ULonglong(), NULLABLE, "Checksum",   OPEN_FULL_TABLE),
  Column("CREATE_OPTIONS",  Varchar(2048),NULLABLE, "Create_options",
                                                                 OPEN_FULL_TABLE),
  Column("TABLE_COMMENT",   Varchar(TABLE_COMMENT_MAXLEN),
                                         NOT_NULL, "Comment",    OPEN_FRM_ONLY),
  Column("MAX_INDEX_LENGTH",ULonglong(), NULLABLE, "Max_index_length",
                                                                 OPEN_FULL_TABLE),
  Column("TEMPORARY",       Varchar(1),  NULLABLE, "Temporary",  OPEN_FRM_ONLY),
  CEnd()
};


ST_FIELD_INFO columns_fields_info[]=
{
  Column("TABLE_CATALOG",           Catalog(),   NOT_NULL,          OPEN_FRM_ONLY),
  Column("TABLE_SCHEMA",            Name(),      NOT_NULL,          OPEN_FRM_ONLY),
  Column("TABLE_NAME",              Name(),      NOT_NULL,          OPEN_FRM_ONLY),
  Column("COLUMN_NAME",             Name(),      NOT_NULL, "Field", OPEN_FRM_ONLY),
  Column("ORDINAL_POSITION",        ULonglong(), NOT_NULL,          OPEN_FRM_ONLY),
  Column("COLUMN_DEFAULT", Longtext(MAX_FIELD_VARCHARLENGTH),
                                                 NULLABLE, "Default",OPEN_FRM_ONLY),
  Column("IS_NULLABLE",          Yes_or_empty(), NOT_NULL, "Null",  OPEN_FRM_ONLY),
  Column("DATA_TYPE",               Name(),      NOT_NULL,          OPEN_FRM_ONLY),
  Column("CHARACTER_MAXIMUM_LENGTH",ULonglong(), NULLABLE,          OPEN_FRM_ONLY),
  Column("CHARACTER_OCTET_LENGTH",  ULonglong(), NULLABLE,          OPEN_FRM_ONLY),
  Column("NUMERIC_PRECISION",       ULonglong(), NULLABLE,          OPEN_FRM_ONLY),
  Column("NUMERIC_SCALE",           ULonglong(), NULLABLE,          OPEN_FRM_ONLY),
  Column("DATETIME_PRECISION",      ULonglong(), NULLABLE,          OPEN_FRM_ONLY),
  Column("CHARACTER_SET_NAME",      CSName(),    NULLABLE,          OPEN_FRM_ONLY),
  Column("COLLATION_NAME",          CSName(),    NULLABLE, "Collation", OPEN_FRM_ONLY),
  Column("COLUMN_TYPE",         Longtext(65535), NOT_NULL, "Type",  OPEN_FRM_ONLY),
  Column("COLUMN_KEY",              Varchar(3),  NOT_NULL, "Key",   OPEN_FRM_ONLY),
  Column("EXTRA",                   Varchar(80), NOT_NULL, "Extra", OPEN_FRM_ONLY),
  Column("PRIVILEGES",              Varchar(80), NOT_NULL, "Privileges", OPEN_FRM_ONLY),
  Column("COLUMN_COMMENT", Varchar(COLUMN_COMMENT_MAXLEN), NOT_NULL, "Comment",
                                                                 OPEN_FRM_ONLY),
  Column("IS_GENERATED",            Varchar(6),  NOT_NULL,       OPEN_FRM_ONLY),
  Column("GENERATION_EXPRESSION",   Longtext(MAX_FIELD_VARCHARLENGTH),
                                                 NULLABLE,       OPEN_FRM_ONLY),
  CEnd()
};


ST_FIELD_INFO charsets_fields_info[]=
{
  Column("CHARACTER_SET_NAME",   CSName(),     NOT_NULL, "Charset"),
  Column("DEFAULT_COLLATE_NAME", CSName(),     NOT_NULL, "Default collation"),
  Column("DESCRIPTION",          Varchar(60),  NOT_NULL, "Description"),
  Column("MAXLEN",               SLonglong(3), NOT_NULL, "Maxlen"),
  CEnd()
};


ST_FIELD_INFO collation_fields_info[]=
{
  Column("COLLATION_NAME",               CSName(),     NOT_NULL, "Collation"),
  Column("CHARACTER_SET_NAME",           CSName(),     NOT_NULL, "Charset"),
  Column("ID", SLonglong(MY_INT32_NUM_DECIMAL_DIGITS), NOT_NULL, "Id"),
  Column("IS_DEFAULT",                 Yes_or_empty(), NOT_NULL, "Default"),
  Column("IS_COMPILED",                Yes_or_empty(), NOT_NULL, "Compiled"),
  Column("SORTLEN",                      SLonglong(3), NOT_NULL, "Sortlen"),
  CEnd()
};


ST_FIELD_INFO applicable_roles_fields_info[]=
{
  Column("GRANTEE",                  Userhost(),     NOT_NULL),
  Column("ROLE_NAME", Varchar(USERNAME_CHAR_LENGTH), NOT_NULL),
  Column("IS_GRANTABLE",             Yes_or_empty(), NOT_NULL),
  Column("IS_DEFAULT",               Yes_or_empty(), NULLABLE),
  CEnd()
};


ST_FIELD_INFO enabled_roles_fields_info[]=
{
  Column("ROLE_NAME", Varchar(USERNAME_CHAR_LENGTH), NULLABLE),
  CEnd()
};

ST_FIELD_INFO keywords_field_info[]=
{
  Column("WORD", Varchar(KEYWORD_SIZE), NULLABLE),
  CEnd()
};

ST_FIELD_INFO sql_functions_field_info[]=
{
  Column("FUNCTION", Varchar(KEYWORD_SIZE), NULLABLE),
  CEnd()
};


ST_FIELD_INFO engines_fields_info[]=
{
  Column("ENGINE",       Varchar(64),  NOT_NULL, "Engine"),
  Column("SUPPORT",      Varchar(8),   NOT_NULL, "Support"),
  Column("COMMENT",      Varchar(160), NOT_NULL, "Comment"),
  Column("TRANSACTIONS", Varchar(3),   NULLABLE, "Transactions"),
  Column("XA",           Varchar(3),   NULLABLE, "XA"),
  Column("SAVEPOINTS",   Varchar(3),   NULLABLE, "Savepoints"),
  CEnd()
};


ST_FIELD_INFO events_fields_info[]=
{
  // QQ: shouldn't EVENT_CATALOG be Catalog() like in all other places?
  Column("EVENT_CATALOG",        Name(),      NOT_NULL),
  Column("EVENT_SCHEMA",         Name(),      NOT_NULL, "Db"),
  Column("EVENT_NAME",           Name(),      NOT_NULL, "Name"),
  Column("DEFINER",              Definer(),   NOT_NULL, "Definer"),
  Column("TIME_ZONE",            Varchar(64), NOT_NULL, "Time zone"),
  Column("EVENT_BODY",           Varchar(8),  NOT_NULL),
  Column("EVENT_DEFINITION", Longtext(65535), NOT_NULL),
  Column("EVENT_TYPE",           Varchar(9),  NOT_NULL, "Type"),
  Column("EXECUTE_AT",           Datetime(0), NULLABLE, "Execute at"),
  Column("INTERVAL_VALUE",       Varchar(256),NULLABLE, "Interval value"),
  Column("INTERVAL_FIELD",       Varchar(18), NULLABLE, "Interval field"),
  Column("SQL_MODE",             SQLMode(),   NOT_NULL),
  Column("STARTS",               Datetime(0), NULLABLE, "Starts"),
  Column("ENDS",                 Datetime(0), NULLABLE, "Ends"),
  Column("STATUS",               Varchar(18), NOT_NULL, "Status"),
  Column("ON_COMPLETION",        Varchar(12), NOT_NULL),
  Column("CREATED",              Datetime(0), NOT_NULL),
  Column("LAST_ALTERED",         Datetime(0), NOT_NULL),
  Column("LAST_EXECUTED",        Datetime(0), NULLABLE),
  Column("EVENT_COMMENT",        Name(),      NOT_NULL),
  Column("ORIGINATOR",          SLonglong(10),NOT_NULL,"Originator"),
  Column("CHARACTER_SET_CLIENT", CSName(),    NOT_NULL, "character_set_client"),
  Column("COLLATION_CONNECTION", CSName(),    NOT_NULL, "collation_connection"),
  Column("DATABASE_COLLATION",   CSName(),    NOT_NULL, "Database Collation"),
  CEnd()
};



ST_FIELD_INFO coll_charset_app_fields_info[]=
{
  Column("COLLATION_NAME",     CSName(), NOT_NULL),
  Column("CHARACTER_SET_NAME", CSName(), NOT_NULL),
  CEnd()
};


ST_FIELD_INFO proc_fields_info[]=
{
  Column("SPECIFIC_NAME",           Name(),     NOT_NULL),
  Column("ROUTINE_CATALOG",         Catalog(),  NOT_NULL),
  Column("ROUTINE_SCHEMA",          Name(),     NOT_NULL, "Db"),
  Column("ROUTINE_NAME",            Name(),     NOT_NULL, "Name"),
  Column("ROUTINE_TYPE",            Varchar(13),NOT_NULL, "Type"),
  Column("DATA_TYPE",               Name(),     NOT_NULL),
  Column("CHARACTER_MAXIMUM_LENGTH",SLong(21),  NULLABLE),
  Column("CHARACTER_OCTET_LENGTH",  SLong(21),  NULLABLE),
  Column("NUMERIC_PRECISION",       SLong(21),  NULLABLE),
  Column("NUMERIC_SCALE",           SLong(21),  NULLABLE),
  Column("DATETIME_PRECISION",     ULonglong(), NULLABLE, OPEN_FRM_ONLY),
  Column("CHARACTER_SET_NAME",      Varchar(64),NULLABLE),
  Column("COLLATION_NAME",          Varchar(64),NULLABLE),
  Column("DTD_IDENTIFIER",     Longtext(65535), NULLABLE),
  Column("ROUTINE_BODY",            Varchar(8), NOT_NULL),
  Column("ROUTINE_DEFINITION", Longtext(65535), NULLABLE),
  Column("EXTERNAL_NAME",           Name(),     NULLABLE),
  Column("EXTERNAL_LANGUAGE",       Name(),     NULLABLE),
  Column("PARAMETER_STYLE",         Varchar(8), NOT_NULL),
  Column("IS_DETERMINISTIC",        Varchar(3), NOT_NULL),
  Column("SQL_DATA_ACCESS",         Name(),     NOT_NULL),
  Column("SQL_PATH",                Name(),     NULLABLE),
  Column("SECURITY_TYPE",           Varchar(7), NOT_NULL, "Security_type"),
  Column("CREATED",                Datetime(0), NOT_NULL, "Created"),
  Column("LAST_ALTERED",           Datetime(0), NOT_NULL, "Modified"),
  Column("SQL_MODE",                SQLMode(),  NOT_NULL),
  Column("ROUTINE_COMMENT",    Longtext(65535), NOT_NULL, "Comment"),
  Column("DEFINER",                 Definer(),  NOT_NULL, "Definer"),
  Column("CHARACTER_SET_CLIENT",    CSName(),   NOT_NULL, "character_set_client"),
  Column("COLLATION_CONNECTION",    CSName(),   NOT_NULL, "collation_connection"),
  Column("DATABASE_COLLATION",      CSName(),   NOT_NULL, "Database Collation"),
  CEnd()
};


ST_FIELD_INFO stat_fields_info[]=
{
  Column("TABLE_CATALOG", Catalog(),   NOT_NULL,                OPEN_FRM_ONLY),
  Column("TABLE_SCHEMA",  Name(),      NOT_NULL,                OPEN_FRM_ONLY),
  Column("TABLE_NAME",    Name(),      NOT_NULL, "Table",       OPEN_FRM_ONLY),
  Column("NON_UNIQUE",    SLonglong(1),NOT_NULL, "Non_unique",  OPEN_FRM_ONLY),
  Column("INDEX_SCHEMA",  Name(),      NOT_NULL,                OPEN_FRM_ONLY),
  Column("INDEX_NAME",    Name(),      NOT_NULL, "Key_name",    OPEN_FRM_ONLY),
  Column("SEQ_IN_INDEX",  SLonglong(2),NOT_NULL, "Seq_in_index",OPEN_FRM_ONLY),
  Column("COLUMN_NAME",   Name(),      NOT_NULL, "Column_name", OPEN_FRM_ONLY),
  Column("COLLATION",     Varchar(1),  NULLABLE, "Collation",   OPEN_FULL_TABLE),
  Column("CARDINALITY",   SLonglong(), NULLABLE, "Cardinality", OPEN_FULL_TABLE),
  Column("SUB_PART",      SLonglong(3),NULLABLE, "Sub_part",    OPEN_FRM_ONLY),
  Column("PACKED",        Varchar(10), NULLABLE, "Packed",      OPEN_FRM_ONLY),
  Column("NULLABLE",      Varchar(3),  NOT_NULL, "Null",        OPEN_FRM_ONLY),
  Column("INDEX_TYPE",    Varchar(16), NOT_NULL, "Index_type",  OPEN_FULL_TABLE),
  Column("COMMENT",       Varchar(16), NULLABLE, "Comment",     OPEN_FRM_ONLY),
  Column("INDEX_COMMENT", Varchar(INDEX_COMMENT_MAXLEN),
                                       NOT_NULL, "Index_comment",OPEN_FRM_ONLY),
  Column("IGNORED",      Varchar(3),  NOT_NULL, "Ignored",        OPEN_FRM_ONLY),
  CEnd()
};


ST_FIELD_INFO view_fields_info[]=
{
  Column("TABLE_CATALOG",        Catalog(),  NOT_NULL, OPEN_FRM_ONLY),
  Column("TABLE_SCHEMA",         Name(),     NOT_NULL, OPEN_FRM_ONLY),
  Column("TABLE_NAME",           Name(),     NOT_NULL, OPEN_FRM_ONLY),
  Column("VIEW_DEFINITION", Longtext(65535), NOT_NULL, OPEN_FRM_ONLY),
  Column("CHECK_OPTION",         Varchar(8), NOT_NULL, OPEN_FRM_ONLY),
  Column("IS_UPDATABLE",     Yes_or_empty(), NOT_NULL, OPEN_FULL_TABLE),
  Column("DEFINER",              Definer(),  NOT_NULL, OPEN_FRM_ONLY),
  Column("SECURITY_TYPE",        Varchar(7), NOT_NULL, OPEN_FRM_ONLY),
  Column("CHARACTER_SET_CLIENT", CSName(),   NOT_NULL, OPEN_FRM_ONLY),
  Column("COLLATION_CONNECTION", CSName(),   NOT_NULL, OPEN_FRM_ONLY),
  Column("ALGORITHM",            Varchar(10),NOT_NULL, OPEN_FRM_ONLY),
  CEnd()
};


ST_FIELD_INFO user_privileges_fields_info[]=
{
  Column("GRANTEE",        Userhost(),     NOT_NULL),
  Column("TABLE_CATALOG",  Catalog(),      NOT_NULL),
  Column("PRIVILEGE_TYPE", Name(),         NOT_NULL),
  Column("IS_GRANTABLE",   Yes_or_empty(), NOT_NULL),
  CEnd()
};


ST_FIELD_INFO schema_privileges_fields_info[]=
{
  Column("GRANTEE",        Userhost(),     NOT_NULL),
  Column("TABLE_CATALOG",  Catalog(),      NOT_NULL),
  Column("TABLE_SCHEMA",   Name(),         NOT_NULL),
  Column("PRIVILEGE_TYPE", Name(),         NOT_NULL),
  Column("IS_GRANTABLE",   Yes_or_empty(), NOT_NULL),
  CEnd()
};


ST_FIELD_INFO table_privileges_fields_info[]=
{
  Column("GRANTEE",        Userhost(),     NOT_NULL),
  Column("TABLE_CATALOG",  Catalog(),      NOT_NULL),
  Column("TABLE_SCHEMA",   Name(),         NOT_NULL),
  Column("TABLE_NAME",     Name(),         NOT_NULL),
  Column("PRIVILEGE_TYPE", Name(),         NOT_NULL),
  Column("IS_GRANTABLE",   Yes_or_empty(), NOT_NULL),
  CEnd()
};


ST_FIELD_INFO column_privileges_fields_info[]=
{
  Column("GRANTEE",        Userhost(),     NOT_NULL),
  Column("TABLE_CATALOG",  Catalog(),      NOT_NULL),
  Column("TABLE_SCHEMA",   Name(),         NOT_NULL),
  Column("TABLE_NAME",     Name(),         NOT_NULL),
  Column("COLUMN_NAME",    Name(),         NOT_NULL),
  Column("PRIVILEGE_TYPE", Name(),         NOT_NULL),
  Column("IS_GRANTABLE",   Yes_or_empty(), NOT_NULL),
  CEnd()
};


ST_FIELD_INFO table_constraints_fields_info[]=
{
  Column("CONSTRAINT_CATALOG",  Catalog(), NOT_NULL, OPEN_FULL_TABLE),
  Column("CONSTRAINT_SCHEMA",   Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("CONSTRAINT_NAME",     Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("TABLE_SCHEMA",        Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("TABLE_NAME",          Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("CONSTRAINT_TYPE",     Name(),    NOT_NULL, OPEN_FULL_TABLE),
  CEnd()
};


ST_FIELD_INFO key_column_usage_fields_info[]=
{
  Column("CONSTRAINT_CATALOG",            Catalog(),     NOT_NULL, OPEN_FULL_TABLE),
  Column("CONSTRAINT_SCHEMA",             Name(),        NOT_NULL, OPEN_FULL_TABLE),
  Column("CONSTRAINT_NAME",               Name(),        NOT_NULL, OPEN_FULL_TABLE),
  Column("TABLE_CATALOG",                 Catalog(),     NOT_NULL, OPEN_FULL_TABLE),
  Column("TABLE_SCHEMA",                  Name(),        NOT_NULL, OPEN_FULL_TABLE),
  Column("TABLE_NAME",                    Name(),        NOT_NULL, OPEN_FULL_TABLE),
  Column("COLUMN_NAME",                   Name(),        NOT_NULL, OPEN_FULL_TABLE),
  Column("ORDINAL_POSITION",              SLonglong(10), NOT_NULL, OPEN_FULL_TABLE),
  Column("POSITION_IN_UNIQUE_CONSTRAINT", SLonglong(10), NULLABLE, OPEN_FULL_TABLE),
  Column("REFERENCED_TABLE_SCHEMA",       Name(),        NULLABLE, OPEN_FULL_TABLE),
  Column("REFERENCED_TABLE_NAME",         Name(),        NULLABLE, OPEN_FULL_TABLE),
  Column("REFERENCED_COLUMN_NAME",        Name(),        NULLABLE, OPEN_FULL_TABLE),
  CEnd()
};


ST_FIELD_INFO table_names_fields_info[]=
{
  Column("TABLE_CATALOG", Catalog(), NOT_NULL),
  Column("TABLE_SCHEMA",  Name(),    NOT_NULL),
  Column("TABLE_NAME",    Varchar(NAME_CHAR_LEN + MYSQL50_TABLE_NAME_PREFIX_LENGTH),
                                     NOT_NULL, "Tables_in_"),
  Column("TABLE_TYPE",    Name(),    NOT_NULL, "Table_type", OPEN_FRM_ONLY),
  CEnd()
};


ST_FIELD_INFO open_tables_fields_info[]=
{
  Column("Database",    Name(),       NOT_NULL, "Database"),
  Column("Table",       Name(),       NOT_NULL, "Table"),
  Column("In_use",      SLonglong(1), NOT_NULL, "In_use"),
  Column("Name_locked", SLonglong(4), NOT_NULL, "Name_locked"),
  CEnd()
};


ST_FIELD_INFO triggers_fields_info[]=
{
  Column("TRIGGER_CATALOG",        Catalog(), NOT_NULL,             OPEN_FRM_ONLY),
  Column("TRIGGER_SCHEMA",            Name(), NOT_NULL,             OPEN_FRM_ONLY),
  Column("TRIGGER_NAME",              Name(), NOT_NULL, "Trigger",  OPEN_FRM_ONLY),
  Column("EVENT_MANIPULATION",    Varchar(6), NOT_NULL, "Event",    OPEN_FRM_ONLY),
  Column("EVENT_OBJECT_CATALOG",   Catalog(), NOT_NULL,             OPEN_FRM_ONLY),
  Column("EVENT_OBJECT_SCHEMA",       Name(), NOT_NULL,             OPEN_FRM_ONLY),
  Column("EVENT_OBJECT_TABLE",        Name(), NOT_NULL, "Table",    OPEN_FRM_ONLY),
  Column("ACTION_ORDER",        SLonglong(4), NOT_NULL,             OPEN_FRM_ONLY),
  Column("ACTION_CONDITION", Longtext(65535), NULLABLE,             OPEN_FRM_ONLY),
  Column("ACTION_STATEMENT", Longtext(65535), NOT_NULL, "Statement",OPEN_FRM_ONLY),
  Column("ACTION_ORIENTATION",    Varchar(9), NOT_NULL,             OPEN_FRM_ONLY),
  Column("ACTION_TIMING",         Varchar(6), NOT_NULL, "Timing",   OPEN_FRM_ONLY),
  Column("ACTION_REFERENCE_OLD_TABLE",Name(), NULLABLE,             OPEN_FRM_ONLY),
  Column("ACTION_REFERENCE_NEW_TABLE",Name(), NULLABLE,             OPEN_FRM_ONLY),
  Column("ACTION_REFERENCE_OLD_ROW",Varchar(3),NOT_NULL,            OPEN_FRM_ONLY),
  Column("ACTION_REFERENCE_NEW_ROW",Varchar(3),NOT_NULL,            OPEN_FRM_ONLY),
  /* 2 here indicates 2 decimals */
  Column("CREATED",              Datetime(2), NULLABLE, "Created",  OPEN_FRM_ONLY),
  Column("SQL_MODE",               SQLMode(), NOT_NULL, "sql_mode", OPEN_FRM_ONLY),
  Column("DEFINER",                Definer(), NOT_NULL, "Definer",  OPEN_FRM_ONLY),
  Column("CHARACTER_SET_CLIENT",    CSName(), NOT_NULL, "character_set_client",
                                                                 OPEN_FRM_ONLY),
  Column("COLLATION_CONNECTION",    CSName(), NOT_NULL, "collation_connection",
                                                                 OPEN_FRM_ONLY),
  Column("DATABASE_COLLATION",      CSName(), NOT_NULL, "Database Collation",
                                                                 OPEN_FRM_ONLY),
  CEnd()
};


ST_FIELD_INFO partitions_fields_info[]=
{
  Column("TABLE_CATALOG",               Catalog(),   NOT_NULL, OPEN_FULL_TABLE),
  Column("TABLE_SCHEMA",                Name(),      NOT_NULL, OPEN_FULL_TABLE),
  Column("TABLE_NAME",                  Name(),      NOT_NULL, OPEN_FULL_TABLE),
  Column("PARTITION_NAME",              Name(),      NULLABLE, OPEN_FULL_TABLE),
  Column("SUBPARTITION_NAME",           Name(),      NULLABLE, OPEN_FULL_TABLE),
  Column("PARTITION_ORDINAL_POSITION",  ULonglong(), NULLABLE, OPEN_FULL_TABLE),
  Column("SUBPARTITION_ORDINAL_POSITION",ULonglong(),NULLABLE, OPEN_FULL_TABLE),
  Column("PARTITION_METHOD",            Varchar(18), NULLABLE, OPEN_FULL_TABLE),
  Column("SUBPARTITION_METHOD",         Varchar(12), NULLABLE, OPEN_FULL_TABLE),
  Column("PARTITION_EXPRESSION",    Longtext(65535), NULLABLE, OPEN_FULL_TABLE),
  Column("SUBPARTITION_EXPRESSION", Longtext(65535), NULLABLE, OPEN_FULL_TABLE),
  Column("PARTITION_DESCRIPTION",   Longtext(65535), NULLABLE, OPEN_FULL_TABLE),
  Column("TABLE_ROWS",                  ULonglong(), NOT_NULL, OPEN_FULL_TABLE),
  Column("AVG_ROW_LENGTH",              ULonglong(), NOT_NULL, OPEN_FULL_TABLE),
  Column("DATA_LENGTH",                 ULonglong(), NOT_NULL, OPEN_FULL_TABLE),
  Column("MAX_DATA_LENGTH",             ULonglong(), NULLABLE, OPEN_FULL_TABLE),
  Column("INDEX_LENGTH",                ULonglong(), NOT_NULL, OPEN_FULL_TABLE),
  Column("DATA_FREE",                   ULonglong(), NOT_NULL, OPEN_FULL_TABLE),
  Column("CREATE_TIME",                 Datetime(0), NULLABLE, OPEN_FULL_TABLE),
  Column("UPDATE_TIME",                 Datetime(0), NULLABLE, OPEN_FULL_TABLE),
  Column("CHECK_TIME",                  Datetime(0), NULLABLE, OPEN_FULL_TABLE),
  Column("CHECKSUM",                    ULonglong(), NULLABLE, OPEN_FULL_TABLE),
  Column("PARTITION_COMMENT",           Varchar(80), NOT_NULL, OPEN_FULL_TABLE),
  Column("NODEGROUP",                   Varchar(12), NOT_NULL, OPEN_FULL_TABLE),
  Column("TABLESPACE_NAME",             Name(),      NULLABLE, OPEN_FULL_TABLE),
  CEnd()
};


ST_FIELD_INFO variables_fields_info[]=
{
  Column("VARIABLE_NAME",  Varchar(64),   NOT_NULL, "Variable_name"),
  Column("VARIABLE_VALUE", Varchar(2048), NOT_NULL, "Value"),
  CEnd()
};


ST_FIELD_INFO sysvars_fields_info[]=
{
  Column("VARIABLE_NAME",        Name(),                           NOT_NULL),
  Column("SESSION_VALUE",        Varchar(2048),                    NULLABLE),
  Column("GLOBAL_VALUE",         Varchar(2048),                    NULLABLE),
  Column("GLOBAL_VALUE_ORIGIN",  Name(),                           NOT_NULL),
  Column("DEFAULT_VALUE",        Varchar(2048),                    NULLABLE),
  Column("VARIABLE_SCOPE",       Name(),                           NOT_NULL),
  Column("VARIABLE_TYPE",        Name(),                           NOT_NULL),
  Column("VARIABLE_COMMENT",     Varchar(TABLE_COMMENT_MAXLEN),    NOT_NULL),
  Column("NUMERIC_MIN_VALUE",    Varchar(MY_INT64_NUM_DECIMAL_DIGITS), NULLABLE),
  Column("NUMERIC_MAX_VALUE",    Varchar(MY_INT64_NUM_DECIMAL_DIGITS), NULLABLE),
  Column("NUMERIC_BLOCK_SIZE",   Varchar(MY_INT64_NUM_DECIMAL_DIGITS), NULLABLE),
  Column("ENUM_VALUE_LIST",      Longtext(65535),                  NULLABLE),
  Column("READ_ONLY",            Yes_or_empty(),                   NOT_NULL),
  Column("COMMAND_LINE_ARGUMENT",Name(),                           NULLABLE),
  Column("GLOBAL_VALUE_PATH",    Varchar(2048),                    NULLABLE),
  CEnd()
};


ST_FIELD_INFO processlist_fields_info[]=
{
  Column("ID",             SLonglong(4),              NOT_NULL, "Id"),
  Column("USER",           Varchar(USERNAME_CHAR_LENGTH), NOT_NULL, "User"),
  Column("HOST",           Varchar(LIST_PROCESS_HOST_LEN),NOT_NULL, "Host"),
  Column("DB",             Name(),                    NULLABLE, "Db"),
  Column("COMMAND",        Varchar(16),               NOT_NULL, "Command"),
  Column("TIME",           SLong(7),                  NOT_NULL, "Time"),
  Column("STATE",          Varchar(64),               NULLABLE, "State"),
  Column("INFO",  Longtext(PROCESS_LIST_INFO_WIDTH),
                                                      NULLABLE, "Info"),
  Column("TIME_MS", Decimal(100 * (MY_INT64_NUM_DECIMAL_DIGITS + 1) + 3),
                                                      NOT_NULL, "Time_ms"),
  Column("STAGE",          STiny(2),                  NOT_NULL, "Stage"),
  Column("MAX_STAGE",      STiny(2),                  NOT_NULL, "Max_stage"),
  Column("PROGRESS",       Decimal(703),              NOT_NULL, "Progress"),
  Column("MEMORY_USED",    SLonglong(7),              NOT_NULL, "Memory_used"),
  Column("MAX_MEMORY_USED",SLonglong(7),              NOT_NULL, "Max_memory_used"),
  Column("EXAMINED_ROWS",  SLong(7),                  NOT_NULL, "Examined_rows"),
  Column("QUERY_ID",       SLonglong(4),              NOT_NULL),
  Column("INFO_BINARY",Blob(PROCESS_LIST_INFO_WIDTH),NULLABLE, "Info_binary"),
  Column("TID",            SLonglong(4),              NOT_NULL, "Tid"),
  CEnd()
};


ST_FIELD_INFO plugin_fields_info[]=
{
  Column("PLUGIN_NAME",             Name(),          NOT_NULL, "Name"),
  Column("PLUGIN_VERSION",          Varchar(20),     NOT_NULL),
  Column("PLUGIN_STATUS",           Varchar(16),     NOT_NULL, "Status"),
  Column("PLUGIN_TYPE",             Varchar(80),     NOT_NULL, "Type"),
  Column("PLUGIN_TYPE_VERSION",     Varchar(20),     NOT_NULL),
  Column("PLUGIN_LIBRARY",          Name(),          NULLABLE, "Library"),
  Column("PLUGIN_LIBRARY_VERSION",  Varchar(20),     NULLABLE),
  Column("PLUGIN_AUTHOR",           Name(),          NULLABLE),
  Column("PLUGIN_DESCRIPTION",      Longtext(65535), NULLABLE),
  Column("PLUGIN_LICENSE",          Varchar(80),     NOT_NULL, "License"),
  Column("LOAD_OPTION",             Varchar(64),     NOT_NULL),
  Column("PLUGIN_MATURITY",         Varchar(12),     NOT_NULL),
  Column("PLUGIN_AUTH_VERSION",     Varchar(80),     NULLABLE),
  CEnd()
};

ST_FIELD_INFO files_fields_info[]=
{
  Column("FILE_ID",             SLonglong(4),   NOT_NULL),
  Column("FILE_NAME",        Varchar(FN_REFLEN),NULLABLE),
  Column("FILE_TYPE",           Varchar(20),    NOT_NULL),
  Column("TABLESPACE_NAME",     Name(),         NULLABLE),
  Column("TABLE_CATALOG",       Name(),         NOT_NULL),
  Column("TABLE_SCHEMA",        Name(),         NULLABLE),
  Column("TABLE_NAME",          Name(),         NULLABLE),
  Column("LOGFILE_GROUP_NAME",  Name(),         NULLABLE),
  Column("LOGFILE_GROUP_NUMBER",SLonglong(4),   NULLABLE),
  Column("ENGINE",              Name(),         NOT_NULL),
  Column("FULLTEXT_KEYS",       Name(),         NULLABLE),
  Column("DELETED_ROWS",        SLonglong(4),   NULLABLE),
  Column("UPDATE_COUNT",        SLonglong(4),   NULLABLE),
  Column("FREE_EXTENTS",        SLonglong(4),   NULLABLE),
  Column("TOTAL_EXTENTS",       SLonglong(4),   NULLABLE),
  Column("EXTENT_SIZE",         SLonglong(4),   NOT_NULL),
  Column("INITIAL_SIZE",        ULonglong(),    NULLABLE),
  Column("MAXIMUM_SIZE",        ULonglong(),    NULLABLE),
  Column("AUTOEXTEND_SIZE",     ULonglong(),    NULLABLE),
  Column("CREATION_TIME",       Datetime(0),    NULLABLE),
  Column("LAST_UPDATE_TIME",    Datetime(0),    NULLABLE),
  Column("LAST_ACCESS_TIME",    Datetime(0),    NULLABLE),
  Column("RECOVER_TIME",        SLonglong(4),   NULLABLE),
  Column("TRANSACTION_COUNTER", SLonglong(4),   NULLABLE),
  Column("VERSION",             ULonglong(),    NULLABLE, "Version"),
  Column("ROW_FORMAT",          Varchar(10),    NULLABLE, "Row_format"),
  Column("TABLE_ROWS",          ULonglong(),    NULLABLE, "Rows"),
  Column("AVG_ROW_LENGTH",      ULonglong(),    NULLABLE, "Avg_row_length"),
  Column("DATA_LENGTH",         ULonglong(),    NULLABLE, "Data_length"),
  Column("MAX_DATA_LENGTH",     ULonglong(),    NULLABLE, "Max_data_length"),
  Column("INDEX_LENGTH",        ULonglong(),    NULLABLE, "Index_length"),
  Column("DATA_FREE",           ULonglong(),    NULLABLE, "Data_free"),
  Column("CREATE_TIME",         Datetime(0),    NULLABLE, "Create_time"),
  Column("UPDATE_TIME",         Datetime(0),    NULLABLE, "Update_time"),
  Column("CHECK_TIME",          Datetime(0),    NULLABLE, "Check_time"),
  Column("CHECKSUM",            ULonglong(),    NULLABLE, "Checksum"),
  Column("STATUS",              Varchar(20),    NOT_NULL),
  Column("EXTRA",               Varchar(255),   NULLABLE),
  CEnd()
};

}; // namespace Show


void init_fill_schema_files_row(TABLE* table)
{
  int i;
  for(i=0; !Show::files_fields_info[i].end_marker(); i++)
    table->field[i]->set_null();

  table->field[IS_FILES_STATUS]->set_notnull();
  table->field[IS_FILES_STATUS]->store("NORMAL", 6, system_charset_info);
}


namespace Show {

ST_FIELD_INFO referential_constraints_fields_info[]=
{
  Column("CONSTRAINT_CATALOG",        Catalog(), NOT_NULL, OPEN_FULL_TABLE),
  Column("CONSTRAINT_SCHEMA",         Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("CONSTRAINT_NAME",           Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("UNIQUE_CONSTRAINT_CATALOG", Catalog(), NOT_NULL, OPEN_FULL_TABLE),
  Column("UNIQUE_CONSTRAINT_SCHEMA",  Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("UNIQUE_CONSTRAINT_NAME",    Name(),    NULLABLE, OPEN_FULL_TABLE),
  Column("MATCH_OPTION",              Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("UPDATE_RULE",               Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("DELETE_RULE",               Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("TABLE_NAME",                Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("REFERENCED_TABLE_NAME",     Name(),    NOT_NULL, OPEN_FULL_TABLE),
  CEnd()
};


ST_FIELD_INFO parameters_fields_info[]=
{
  Column("SPECIFIC_CATALOG",        Catalog(),       NOT_NULL, OPEN_FULL_TABLE),
  Column("SPECIFIC_SCHEMA",         Name(),          NOT_NULL, OPEN_FULL_TABLE),
  Column("SPECIFIC_NAME",           Name(),          NOT_NULL, OPEN_FULL_TABLE),
  Column("ORDINAL_POSITION",        SLong(21),       NOT_NULL, OPEN_FULL_TABLE),
  Column("PARAMETER_MODE",          Varchar(5),      NULLABLE, OPEN_FULL_TABLE),
  Column("PARAMETER_NAME",          Name(),          NULLABLE, OPEN_FULL_TABLE),
  Column("DATA_TYPE",               Name(),          NOT_NULL, OPEN_FULL_TABLE),
  Column("CHARACTER_MAXIMUM_LENGTH",SLong(21),       NULLABLE, OPEN_FULL_TABLE),
  Column("CHARACTER_OCTET_LENGTH",  SLong(21),       NULLABLE, OPEN_FULL_TABLE),
  Column("NUMERIC_PRECISION",       SLong(21),       NULLABLE, OPEN_FULL_TABLE),
  Column("NUMERIC_SCALE",           SLong(21),       NULLABLE, OPEN_FULL_TABLE),
  Column("DATETIME_PRECISION",      ULonglong(),     NULLABLE, OPEN_FRM_ONLY),
  Column("CHARACTER_SET_NAME",      Varchar(64),     NULLABLE, OPEN_FULL_TABLE),
  Column("COLLATION_NAME",          Varchar(64),     NULLABLE, OPEN_FULL_TABLE),
  Column("DTD_IDENTIFIER",          Longtext(65535), NOT_NULL, OPEN_FULL_TABLE),
  Column("ROUTINE_TYPE",            Varchar(9),      NOT_NULL, OPEN_FULL_TABLE),
  CEnd()
};


ST_FIELD_INFO tablespaces_fields_info[]=
{
  Column("TABLESPACE_NAME",    Name(),        NOT_NULL),
  Column("ENGINE",             Name(),        NOT_NULL),
  Column("TABLESPACE_TYPE",    Name(),        NULLABLE),
  Column("LOGFILE_GROUP_NAME", Name(),        NULLABLE),
  Column("EXTENT_SIZE",        ULonglong(),   NULLABLE),
  Column("AUTOEXTEND_SIZE",    ULonglong(),   NULLABLE),
  Column("MAXIMUM_SIZE",       ULonglong(),   NULLABLE),
  Column("NODEGROUP_ID",       ULonglong(),   NULLABLE),
  Column("TABLESPACE_COMMENT", Varchar(2048), NULLABLE),
  CEnd()
};


ST_FIELD_INFO keycache_fields_info[]=
{
  Column("KEY_CACHE_NAME",Varchar(NAME_LEN),NOT_NULL),
  Column("SEGMENTS",        ULong(3),       NULLABLE),
  Column("SEGMENT_NUMBER",  ULong(3),       NULLABLE),
  Column("FULL_SIZE",       ULonglong(),    NOT_NULL),
  Column("BLOCK_SIZE",      ULonglong(),    NOT_NULL),
  Column("USED_BLOCKS",     ULonglong(),    NOT_NULL, "Key_blocks_used"),
  Column("UNUSED_BLOCKS",   ULonglong(),    NOT_NULL, "Key_blocks_unused"),
  Column("DIRTY_BLOCKS",    ULonglong(),    NOT_NULL, "Key_blocks_not_flushed"),
  Column("READ_REQUESTS",   ULonglong(),    NOT_NULL, "Key_read_requests"),
  Column("READS",           ULonglong(),    NOT_NULL, "Key_reads"),
  Column("WRITE_REQUESTS",  ULonglong(),    NOT_NULL, "Key_write_requests"),
  Column("WRITES",          ULonglong(),    NOT_NULL, "Key_writes"),
  CEnd()
};


ST_FIELD_INFO show_explain_tabular_fields_info[]=
{
  Column("id",            SLonglong(3),                  NULLABLE, "id"),
  Column("select_type",   Varchar(19),                   NOT_NULL, "select_type"),
  Column("table",         Name(),                        NULLABLE, "table"),
  Column("type",          Varchar(15),                   NULLABLE, "type"),
  Column("possible_keys",Varchar(NAME_CHAR_LEN*MAX_KEY), NULLABLE, "possible_keys"),
  Column("key",          Varchar(NAME_CHAR_LEN*MAX_KEY), NULLABLE, "key"),
  Column("key_len",      Varchar(NAME_CHAR_LEN*MAX_KEY), NULLABLE, "key_len"),
  Column("ref",     Varchar(NAME_CHAR_LEN*MAX_REF_PARTS),NULLABLE, "ref"),
  Column("rows",          SLonglong(10),                 NULLABLE, "rows"),
  Column("Extra",         Varchar(255),                  NOT_NULL, "Extra"),
  CEnd()
};


ST_FIELD_INFO show_explain_json_fields_info[]=
{
  Column("EXPLAIN", Longtext(MAX_FIELD_VARCHARLENGTH), NOT_NULL, "SHOW EXPLAIN"),
  CEnd()
};


ST_FIELD_INFO show_analyze_tabular_fields_info[]=
{
  Column("id",            SLonglong(3),                  NULLABLE, "id"),
  Column("select_type",   Varchar(19),                   NOT_NULL, "select_type"),
  Column("table",         Name(),                        NULLABLE, "table"),
  Column("type",          Varchar(15),                   NULLABLE, "type"),
  Column("possible_keys",Varchar(NAME_CHAR_LEN*MAX_KEY), NULLABLE, "possible_keys"),
  Column("key",          Varchar(NAME_CHAR_LEN*MAX_KEY), NULLABLE, "key"),
  Column("key_len",      Varchar(NAME_CHAR_LEN*MAX_KEY), NULLABLE, "key_len"),
  Column("ref",     Varchar(NAME_CHAR_LEN*MAX_REF_PARTS),NULLABLE, "ref"),
  Column("rows",          SLonglong(10),                 NULLABLE, "rows"),
  Column("r_rows",        Varchar(NAME_CHAR_LEN),        NULLABLE, "r_rows"),

  /* Fields of type DECIMAL(5,2) to represent percentage.
  See Show::Type::decimal_precision() and Show::Type::decimal_scale() to learn
  how 502 converts to precision and scale (5 and 2)*/
  Column("filtered",      Decimal(502),                  NULLABLE, "filtered"),
  Column("r_filtered",    Decimal(502),                  NULLABLE, "r_filtered"),
  Column("Extra",         Varchar(255),                  NOT_NULL, "Extra"),
  CEnd()
};


ST_FIELD_INFO show_analyze_json_fields_info[]= {
    Column("ANALYZE", Longtext(MAX_FIELD_VARCHARLENGTH), NOT_NULL, "SHOW ANALYZE"),
    CEnd()};


ST_FIELD_INFO check_constraints_fields_info[]=
{
  Column("CONSTRAINT_CATALOG", Catalog(), NOT_NULL, OPEN_FULL_TABLE),
  Column("CONSTRAINT_SCHEMA",  Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("TABLE_NAME",         Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("CONSTRAINT_NAME",    Name(),    NOT_NULL, OPEN_FULL_TABLE),
  Column("LEVEL",              Varchar(6),NOT_NULL, OPEN_FULL_TABLE),
  Column("CHECK_CLAUSE",       Longtext(MAX_FIELD_VARCHARLENGTH),
                                          NOT_NULL, OPEN_FULL_TABLE),
  CEnd()
};

}; // namespace Show


namespace Show {

/** For creating fields of information_schema.OPTIMIZER_TRACE */
extern ST_FIELD_INFO optimizer_trace_info[];

} //namespace Show

/*
  Description of ST_FIELD_INFO in sql_i_s.h

  Make sure that the order of schema_tables and enum_schema_tables are the same.

*/

ST_SCHEMA_TABLE schema_tables[]=
{
  {"ALL_PLUGINS", Show::plugin_fields_info, 0,
   fill_all_plugins, make_old_format, 0, 5, -1, 0, 0},
  {"APPLICABLE_ROLES", Show::applicable_roles_fields_info, 0,
   fill_schema_applicable_roles, 0, 0, -1, -1, 0, 0},
  {"CHARACTER_SETS", Show::charsets_fields_info, 0,
   fill_schema_charsets, make_character_sets_old_format, 0, -1, -1, 0, 0},
  {"CHECK_CONSTRAINTS", Show::check_constraints_fields_info, 0,
   get_all_tables, 0,
   get_check_constraints_record, 1, 2, 0, OPTIMIZE_I_S_TABLE|OPEN_TABLE_ONLY},
  {"COLLATIONS", Show::collation_fields_info, 0,
   fill_schema_collation, make_old_format, 0, -1, -1, 0, 0},
  {"COLLATION_CHARACTER_SET_APPLICABILITY", Show::coll_charset_app_fields_info,
   0, fill_schema_coll_charset_app, 0, 0, -1, -1, 0, 0},
  {"COLUMNS", Show::columns_fields_info, 0,
   get_all_tables, make_columns_old_format, get_schema_column_record, 1, 2, 0,
   OPTIMIZE_I_S_TABLE|OPEN_VIEW_FULL},
  {"COLUMN_PRIVILEGES", Show::column_privileges_fields_info, 0,
   fill_schema_column_privileges, 0, 0, -1, -1, 0, 0},
  {"ENABLED_ROLES", Show::enabled_roles_fields_info, 0,
   fill_schema_enabled_roles, 0, 0, -1, -1, 0, 0},
  {"ENGINES", Show::engines_fields_info, 0,
   fill_schema_engines, make_old_format, 0, -1, -1, 0, 0},
#ifdef HAVE_EVENT_SCHEDULER
  {"EVENTS", Show::events_fields_info, 0,
   Events::fill_schema_events, make_old_format, 0, -1, -1, 0, 0},
#else
  {"EVENTS", Show::events_fields_info, 0,
   0, make_old_format, 0, -1, -1, 0, 0},
#endif
  {"EXPLAIN", Show::show_explain_tabular_fields_info, 0,
    fill_show_explain_tabular, make_old_format, 0, -1, -1,
    TRUE /*hidden*/ , 0},
  {"EXPLAIN_JSON", Show::show_explain_json_fields_info, 0,
    fill_show_explain_json, make_old_format, 0, -1, -1,
    TRUE /*hidden*/ , 0},
  {"ANALYZE", Show::show_analyze_tabular_fields_info, 0,
    fill_show_analyze_tabular, make_old_format, 0, -1, -1,
    TRUE /*hidden*/, 0},
  {"ANALYZE_JSON", Show::show_analyze_json_fields_info, 0,
    fill_show_analyze_json, make_old_format, 0, -1, -1,
    TRUE /*hidden*/, 0},
  {"FILES", Show::files_fields_info, 0,
   hton_fill_schema_table, 0, 0, -1, -1, 0, 0},
  {"GLOBAL_STATUS", Show::variables_fields_info, 0,
   fill_status, make_old_format, 0, 0, -1, 0, 0},
  {"GLOBAL_VARIABLES", Show::variables_fields_info, 0,
   fill_variables, make_old_format, 0, 0, -1, 0, 0},
  {"KEYWORDS", Show::keywords_field_info, 0,
   fill_i_s_keywords, 0, 0, -1, -1, 0, 0},
  {"KEY_CACHES", Show::keycache_fields_info, 0,
   fill_key_cache_tables, 0, 0, -1,-1, 0, 0},
  {"KEY_COLUMN_USAGE", Show::key_column_usage_fields_info, 0,
   get_all_tables, 0, get_schema_key_column_usage_record, 4, 5, 0,
   OPTIMIZE_I_S_TABLE|OPEN_TABLE_ONLY},
  {"OPEN_TABLES", Show::open_tables_fields_info, 0,
   fill_open_tables, make_old_format, 0, -1, -1, 1, 0},
  {"OPTIMIZER_TRACE", Show::optimizer_trace_info, 0,
     fill_optimizer_trace_info, NULL, NULL, -1, -1, false, 0},
  {"PARAMETERS", Show::parameters_fields_info, 0,
   fill_schema_proc, 0, 0, -1, -1, 0, 0},
  {"PARTITIONS", Show::partitions_fields_info, 0,
   get_all_tables, 0, get_schema_partitions_record, 1, 2, 0,
   OPTIMIZE_I_S_TABLE|OPEN_TABLE_ONLY},
  {"PLUGINS", Show::plugin_fields_info, 0,
   fill_plugins, make_old_format, 0, -1, -1, 0, 0},
  {"PROCESSLIST", Show::processlist_fields_info, 0,
   fill_schema_processlist, make_old_format, 0, -1, -1, 0, 0},
  {"PROFILING", Show::query_profile_statistics_info, 0,
    fill_query_profile_statistics_info, make_profile_table_for_show,
    NULL, -1, -1, false, 0},
  {"REFERENTIAL_CONSTRAINTS", Show::referential_constraints_fields_info,
   0, get_all_tables, 0, get_referential_constraints_record,
   1, 9, 0, OPTIMIZE_I_S_TABLE|OPEN_TABLE_ONLY},
  {"ROUTINES", Show::proc_fields_info, 0,
   fill_schema_proc, make_proc_old_format, 0, -1, -1, 0, 0},
  {"SCHEMATA", Show::schema_fields_info, 0,
   fill_schema_schemata, make_schemata_old_format, 0, 1, -1, 0, 0},
  {"SCHEMA_PRIVILEGES", Show::schema_privileges_fields_info, 0,
   fill_schema_schema_privileges, 0, 0, -1, -1, 0, 0},
  {"SESSION_STATUS", Show::variables_fields_info, 0,
   fill_status, make_old_format, 0, 0, -1, 0, 0},
  {"SESSION_VARIABLES", Show::variables_fields_info, 0,
   fill_variables, make_old_format, 0, 0, -1, 0, 0},
  {"STATISTICS", Show::stat_fields_info, 0,
   get_all_tables, make_old_format, get_schema_stat_record, 1, 2, 0,
   OPEN_TABLE_ONLY|OPTIMIZE_I_S_TABLE},
  {"SQL_FUNCTIONS", Show::sql_functions_field_info, 0,
   fill_i_s_sql_functions, 0, 0, -1, -1, 0, 0},
  {"SYSTEM_VARIABLES", Show::sysvars_fields_info, 0,
   fill_sysvars, make_old_format, 0, 0, -1, 0, 0},
  {"TABLES", Show::tables_fields_info, 0,
   get_all_tables, make_old_format, get_schema_tables_record, 1, 2, 0,
   OPTIMIZE_I_S_TABLE},
  {"TABLESPACES", Show::tablespaces_fields_info, 0,
   hton_fill_schema_table, 0, 0, -1, -1, 0, 0},
  {"TABLE_CONSTRAINTS", Show::table_constraints_fields_info, 0,
   get_all_tables, 0, get_schema_constraints_record, 3, 4, 0,
   OPTIMIZE_I_S_TABLE|OPEN_TABLE_ONLY},
  {"TABLE_NAMES", Show::table_names_fields_info, 0,
   get_all_tables, make_table_names_old_format, 0, 1, 2, 1, OPTIMIZE_I_S_TABLE},
  {"TABLE_PRIVILEGES", Show::table_privileges_fields_info, 0,
   fill_schema_table_privileges, 0, 0, -1, -1, 0, 0},
  {"TRIGGERS", Show::triggers_fields_info, 0,
   get_all_tables, make_old_format, get_schema_triggers_record, 5, 6, 0,
   OPEN_TRIGGER_ONLY|OPTIMIZE_I_S_TABLE},
  {"USER_PRIVILEGES", Show::user_privileges_fields_info, 0,
   fill_schema_user_privileges, 0, 0, -1, -1, 0, 0},
  {"VIEWS", Show::view_fields_info, 0,
   get_all_tables, 0, get_schema_views_record, 1, 2, 0,
   OPEN_VIEW_ONLY|OPTIMIZE_I_S_TABLE},
  {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
};


int initialize_schema_table(st_plugin_int *plugin)
{
  ST_SCHEMA_TABLE *schema_table;
  DBUG_ENTER("initialize_schema_table");

  if (!(schema_table= (ST_SCHEMA_TABLE *)my_malloc(key_memory_ST_SCHEMA_TABLE,
                                                   sizeof(ST_SCHEMA_TABLE),
                                                   MYF(MY_WME | MY_ZEROFILL))))
      DBUG_RETURN(1);
  /* Historical Requirement */
  plugin->data= schema_table; // shortcut for the future
  if (plugin->plugin->init)
  {
    schema_table->idx_field1= -1,
    schema_table->idx_field2= -1;

    /* Make the name available to the init() function. */
    schema_table->table_name= plugin->name.str;

    if (plugin->plugin->init(schema_table))
    {
      sql_print_error("Plugin '%s' init function returned error.",
                      plugin->name.str);
      plugin->data= NULL;
      my_free(schema_table);
      DBUG_RETURN(1);
    }

    if (!schema_table->old_format)
      for (ST_FIELD_INFO *f= schema_table->fields_info; !f->end_marker(); f++)
        if (f->old_name().str && f->old_name().str[0])
        {
          schema_table->old_format= make_old_format;
          break;
        }

    /* Make sure the plugin name is not set inside the init() function. */
    schema_table->table_name= plugin->name.str;
  }
  DBUG_RETURN(0);
}

int finalize_schema_table(st_plugin_int *plugin)
{
  int deinit_status= 0;
  ST_SCHEMA_TABLE *schema_table= (ST_SCHEMA_TABLE *)plugin->data;
  DBUG_ENTER("finalize_schema_table");

  if (schema_table)
  {
    if (plugin->plugin->deinit)
      deinit_status= plugin->plugin->deinit(NULL);
    my_free(schema_table);
  }
  DBUG_RETURN(deinit_status);
}


/**
  Output trigger information (SHOW CREATE TRIGGER) to the client.

  @param thd          Thread context.
  @param trigger      Trigger to dump

  @return Operation status
    @retval TRUE Error.
    @retval FALSE Success.
*/

static bool show_create_trigger_impl(THD *thd, Trigger *trigger)
{
  int ret_code;
  Protocol *p= thd->protocol;
  List<Item> fields;
  LEX_CSTRING trg_sql_mode_str, trg_body;
  LEX_CSTRING trg_sql_original_stmt;
  LEX_STRING trg_definer;
  CHARSET_INFO *trg_client_cs;
  MEM_ROOT *mem_root= thd->mem_root;
  char definer_holder[USER_HOST_BUFF_SIZE];
  trg_definer.str= definer_holder;

  /*
    TODO: Check privileges here. This functionality will be added by
    implementation of the following WL items:
      - WL#2227: New privileges for new objects
      - WL#3482: Protect SHOW CREATE PROCEDURE | FUNCTION | VIEW | TRIGGER
        properly

    SHOW TRIGGERS and I_S.TRIGGERS will be affected too.
  */

  /* Prepare trigger "object". */

  trigger->get_trigger_info(&trg_sql_original_stmt, &trg_body, &trg_definer);
  sql_mode_string_representation(thd, trigger->sql_mode, &trg_sql_mode_str);

  /* Resolve trigger client character set. */
  myf utf8_flag= thd->get_utf8_flag();
  if (resolve_charset(trigger->client_cs_name.str, NULL, &trg_client_cs,
                      MYF(utf8_flag)))
    return TRUE;

  /* Send header. */

  fields.push_back(new (mem_root) Item_empty_string(thd, "Trigger", NAME_LEN),
                   mem_root);
  fields.push_back(new (mem_root)
                   Item_empty_string(thd, "sql_mode", (uint)trg_sql_mode_str.length),
                   mem_root);

  {
    /*
      NOTE: SQL statement field must be not less than 1024 in order not to
      confuse old clients.
    */

    Item_empty_string *stmt_fld=
      new (mem_root) Item_empty_string(thd, "SQL Original Statement",
                                       (uint)MY_MAX(trg_sql_original_stmt.length,
                                              1024));

    stmt_fld->set_maybe_null();

    fields.push_back(stmt_fld, mem_root);
  }

  fields.push_back(new (mem_root)
                   Item_empty_string(thd, "character_set_client",
                                     MY_CS_NAME_SIZE),
                   mem_root);

  fields.push_back(new (mem_root)
                   Item_empty_string(thd, "collation_connection",
                                     MY_CS_NAME_SIZE),
                   mem_root);

  fields.push_back(new (mem_root)
                   Item_empty_string(thd, "Database Collation",
                                     MY_CS_NAME_SIZE),
                   mem_root);

  static const Datetime zero_datetime(Datetime::zero());
  Item_datetime_literal *tmp= (new (mem_root) 
                               Item_datetime_literal(thd, &zero_datetime, 2));
  tmp->set_name(thd, Lex_cstring(STRING_WITH_LEN("Created")));
  fields.push_back(tmp, mem_root);

  if (p->send_result_set_metadata(&fields,
                                  Protocol::SEND_NUM_ROWS |
                                  Protocol::SEND_EOF))
    return TRUE;

  /* Send data. */

  p->prepare_for_resend();

  p->store(trigger->name.str,
           trigger->name.length,
           system_charset_info);

  p->store(trg_sql_mode_str.str,
           trg_sql_mode_str.length,
           system_charset_info);

  p->store(trg_sql_original_stmt.str,
           trg_sql_original_stmt.length,
           trg_client_cs);

  p->store(&trigger->client_cs_name, system_charset_info);

  p->store(&trigger->connection_cl_name, system_charset_info);

  p->store(&trigger->db_cl_name, system_charset_info);

  if (trigger->create_time)
  {
    MYSQL_TIME timestamp;
    thd->variables.time_zone->gmt_sec_to_TIME(&timestamp,
                                              (my_time_t)(trigger->create_time/100));
    timestamp.second_part= (trigger->create_time % 100) * 10000;
    p->store_datetime(&timestamp, 2);
  }
  else
    p->store_null();


  ret_code= p->write();

  if (!ret_code)
    my_eof(thd);

  return ret_code != 0;
}


/**
  Read TRN and TRG files to obtain base table name for the specified
  trigger name and construct TABE_LIST object for the base table.

  @param thd      Thread context.
  @param trg_name Trigger name.

  @return TABLE_LIST object corresponding to the base table.

  TODO: This function is a copy&paste from add_table_to_list() and
  sp_add_to_query_tables(). The problem is that in order to be compatible
  with Stored Programs (Prepared Statements), we should not touch thd->lex.
  The "source" functions also add created TABLE_LIST object to the
  thd->lex->query_tables.

  The plan to eliminate this copy&paste is to:

    - get rid of sp_add_to_query_tables() and use Lex::add_table_to_list().
      Only add_table_to_list() must be used to add tables from the parser
      into Lex::query_tables list.

    - do not update Lex::query_tables in add_table_to_list().
*/

static
TABLE_LIST *get_trigger_table(THD *thd, const sp_name *trg_name)
{
  char trn_path_buff[FN_REFLEN];
  LEX_CSTRING trn_path= { trn_path_buff, 0 };
  LEX_CSTRING db;
  LEX_CSTRING tbl_name;
  TABLE_LIST *table;

  build_trn_path(thd, trg_name, (LEX_STRING*) &trn_path);

  if (check_trn_exists(&trn_path))
  {
    my_error(ER_TRG_DOES_NOT_EXIST, MYF(0));
    return NULL;
  }

  if (load_table_name_for_trigger(thd, trg_name, &trn_path, &tbl_name))
    return NULL;

  /* We need to reset statement table list to be PS/SP friendly. */
  if (!(table= (TABLE_LIST*) thd->alloc(sizeof(TABLE_LIST))))
    return NULL;

  db= trg_name->m_db;

  db.str= thd->strmake(db.str, db.length);
  if (lower_case_table_names)
    db.length= my_casedn_str(files_charset_info, (char*) db.str);

  tbl_name.str= thd->strmake(tbl_name.str, tbl_name.length);

  if (db.str == NULL || tbl_name.str == NULL)
    return NULL;

  table->init_one_table(&db, &tbl_name, 0, TL_IGNORE);

  return table;
}


/**
  SHOW CREATE TRIGGER high-level implementation.

  @param thd      Thread context.
  @param trg_name Trigger name.

  @return Operation status
    @retval TRUE Error.
    @retval FALSE Success.
*/

bool show_create_trigger(THD *thd, const sp_name *trg_name)
{
  TABLE_LIST *lst= get_trigger_table(thd, trg_name);
  uint num_tables; /* NOTE: unused, only to pass to open_tables(). */
  Table_triggers_list *triggers;
  Trigger *trigger;
  bool error= TRUE;

  if (!lst)
    return TRUE;

  if (check_table_access(thd, TRIGGER_ACL, lst, FALSE, 1, TRUE))
  {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "TRIGGER");
    return TRUE;
  }

  /*
    Metadata locks taken during SHOW CREATE TRIGGER should be released when
    the statement completes as it is an information statement.
  */
  MDL_savepoint mdl_savepoint= thd->mdl_context.mdl_savepoint();

  /*
    Open the table by name in order to load Table_triggers_list object.
  */
  if (open_tables(thd, &lst, &num_tables,
                  MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL))
  {
    my_error(ER_TRG_CANT_OPEN_TABLE, MYF(0),
             (const char *) trg_name->m_db.str,
             (const char *) lst->table_name.str);

    goto exit;

    /* Perform closing actions and return error status. */
  }

  triggers= lst->table->triggers;

  if (!triggers)
  {
    my_error(ER_TRG_DOES_NOT_EXIST, MYF(0));
    goto exit;
  }

  trigger= triggers->find_trigger(&trg_name->m_name, 0);

  if (!trigger)
  {
    my_error(ER_TRG_CORRUPTED_FILE, MYF(0),
             (const char *) trg_name->m_db.str,
             (const char *) lst->table_name.str);
    goto exit;
  }

  error= show_create_trigger_impl(thd, trigger);

  /*
    NOTE: if show_create_trigger_impl() failed, that means we could not
    send data to the client. In this case we simply raise the error
    status and client connection will be closed.
  */

exit:
  close_thread_tables(thd);
  /* Release any metadata locks taken during SHOW CREATE TRIGGER. */
  thd->mdl_context.rollback_to_savepoint(mdl_savepoint);
  return error;
}

class IS_internal_schema_access : public ACL_internal_schema_access
{
public:
  IS_internal_schema_access()
  {}

  ~IS_internal_schema_access()
  {}

  ACL_internal_access_result check(privilege_t want_access,
                                   privilege_t *save_priv) const;

  const ACL_internal_table_access *lookup(const char *name) const;
};

ACL_internal_access_result
IS_internal_schema_access::check(privilege_t want_access,
                                 privilege_t *save_priv) const
{
  want_access &= ~SELECT_ACL;

  /*
    We don't allow any simple privileges but SELECT_ACL on
    the information_schema database.
  */
  if (unlikely((want_access & DB_ACLS) != NO_ACL))
    return ACL_INTERNAL_ACCESS_DENIED;

  /* Always grant SELECT for the information schema. */
  *save_priv|= SELECT_ACL;

  return want_access ? ACL_INTERNAL_ACCESS_CHECK_GRANT :
                       ACL_INTERNAL_ACCESS_GRANTED;
}

const ACL_internal_table_access *
IS_internal_schema_access::lookup(const char *name) const
{
  /* There are no per table rules for the information schema. */
  return NULL;
}

static IS_internal_schema_access is_internal_schema_access;

void initialize_information_schema_acl()
{
  ACL_internal_schema_registry::register_schema(&INFORMATION_SCHEMA_NAME,
                                                &is_internal_schema_access);
}


/**
  Dumps a text description of a thread, its security context
  (user, host) and the current query.

  @param thd thread context
  @param buffer pointer to preferred result buffer
  @param length length of buffer
  @param max_query_len how many chars of query to copy (0 for all)

  @return Pointer to string
*/

extern "C"
char *thd_get_error_context_description(THD *thd, char *buffer,
                                        unsigned int length,
                                        unsigned int max_query_len)
{
  String str(buffer, length, &my_charset_latin1);
  const Security_context *sctx= &thd->main_security_ctx;
  char header[256];
  size_t len;

  len= my_snprintf(header, sizeof(header),
                   "MariaDB thread id %u, OS thread handle %lu, query id %llu",
                   (uint)thd->thread_id, (ulong) thd->real_id, (ulonglong) thd->query_id);
  str.length(0);
  str.append(header, len);

  if (sctx->host)
  {
    str.append(' ');
    str.append(sctx->host, strlen(sctx->host));
  }

  if (sctx->ip)
  {
    str.append(' ');
    str.append(sctx->ip, strlen(sctx->ip));
  }

  if (sctx->user)
  {
    str.append(' ');
    str.append(sctx->user, strlen(sctx->user));
  }

  /* Don't wait if LOCK_thd_data is used as this could cause a deadlock */
  if (!mysql_mutex_trylock(&thd->LOCK_thd_data))
  {
    if (const char *info= thread_state_info(thd))
    {
      str.append(' ');
      str.append(info, strlen(info));
    }

    if (thd->query())
    {
      if (max_query_len < 1)
        len= thd->query_length();
      else
        len= MY_MIN(thd->query_length(), max_query_len);
      str.append('\n');
      str.append(thd->query(), len);
    }
    mysql_mutex_unlock(&thd->LOCK_thd_data);
  }

  if (str.c_ptr_safe() == buffer)
    return buffer;

  /*
    We have to copy the new string to the destination buffer because the string
    was reallocated to a larger buffer to be able to fit.
  */
  DBUG_ASSERT(buffer != NULL);
  length= MY_MIN(str.length(), length-1);
  memcpy(buffer, str.ptr(), length);
  /* Make sure that the new string is null terminated */
  buffer[length]= '\0';
  return buffer;
}
