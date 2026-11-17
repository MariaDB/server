/* Copyright (c) 2026, MariaDB plc

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

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sql_cmd.h"
#include "sql_parse.h"
#include "sql_acl.h"                            // check_access
#include "mysqld.h"
#include "sql_extension.h"
#include "sql_extension_matcher.h"
#include <mysql/plugin_sql_extension.h>
#include <my_counter.h>

namespace {

/**
  Registry entry: one per installed extension plugin. The only type added to
  the trie, so the owners the trie returns are cast back to it.
*/
struct Sql_ext_plugin : Sql_ext_owner
{
  st_plugin_int *const plugin;
  /** plugin->plugin->info, checked by check_descriptor() */
  const st_mariadb_sql_extension *const descriptor;

  Sql_ext_plugin(st_plugin_int *plugin_arg,
                 const st_mariadb_sql_extension *descriptor_arg)
    : plugin(plugin_arg), descriptor(descriptor_arg) {}
};


/*
  The registry. Install and uninstall take the write lock; the failed-parse
  path takes the read lock, and only when installed_count is non-zero.
*/
mysql_rwlock_t LOCK_sql_extension;
/** LOCK_sql_extension is initialized: sql_extension_free() has work */
static bool registry_initialized;
Atomic_counter<uint32> installed_count;
Sql_ext_keyword_trie *trie;
/** Recognizer plugins, by descending priority, then by name */
Dynamic_array<Sql_ext_plugin *> *recognizers;
/**
  All installed plugins, to find a plugin's entry when it is uninstalled.
  The entry is not kept in st_plugin_int::data, which is the plugin's own:
  its init() may store its state there.
*/
Dynamic_array<Sql_ext_plugin *> *entries;

PSI_memory_key key_memory_sql_extension;
#ifdef HAVE_PSI_INTERFACE
PSI_rwlock_key key_rwlock_LOCK_sql_extension;
PSI_rwlock_info sql_extension_rwlocks[]=
{
  { &key_rwlock_LOCK_sql_extension, "LOCK_sql_extension", PSI_FLAG_GLOBAL }
};
PSI_memory_info sql_extension_memory[]=
{
  { &key_memory_sql_extension, "sql_extension", PSI_FLAG_GLOBAL }
};
#endif


/** Higher priority first, then by name, so the order is the same whatever
    the order of INSTALL or of loading at startup */
int cmp_recognizers(const void *a_, const void *b_)
{
  const Sql_ext_plugin *a= *static_cast<Sql_ext_plugin *const *>(a_);
  const Sql_ext_plugin *b= *static_cast<Sql_ext_plugin *const *>(b_);
  if (a->descriptor->priority != b->descriptor->priority)
    return a->descriptor->priority > b->descriptor->priority ? -1 : 1;
  return my_strcasecmp_latin1(a->plugin->name.str, b->plugin->name.str);
}


/**
  @return NULL if the descriptor is usable, otherwise why it is not
*/
const char *check_descriptor(const st_mariadb_sql_extension *d)
{
  if (!d)
    return "no descriptor";
  if (!d->execute)
    return "execute() is not set";
  bool has_prefix= d->prefixes && d->prefixes[0];
  if (!has_prefix && !d->recognize)
    return "neither prefixes nor recognize() is set";
  if (has_prefix && d->recognize)
    return "both prefixes and recognize() are set";
  if (has_prefix && d->priority)
    return "priority is set with prefixes";
  if (has_prefix)
    for (const char *const *p= d->prefixes; *p; p++)
      if (const char *why= Sql_ext_keyword_trie::check_prefix_syntax(*p))
        return why;
  return nullptr;
}


/**
  Add a plugin to the registry, or refuse it if one of its prefixes
  overlaps a prefix of another plugin. Caller holds the write lock.

  @return false on success, true (error raised) on overlap or out of memory
*/
bool registry_add(Sql_ext_plugin *entry)
{
  const st_mariadb_sql_extension *d= entry->descriptor;

  /* First, so that a failure here has nothing to undo */
  if (entries->append(entry))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return true;
  }

  if (d->recognize)
  {
    if (recognizers->append(entry))
    {
      entries->pop();
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return true;
    }
    recognizers->sort(cmp_recognizers);
    return false;
  }

  /* All or none: a refused plugin leaves no trace */
  Sql_ext_keyword_trie::Conflict conflict;
  switch (trie->add_claims(d->prefixes, entry, &conflict)) {
  case Sql_ext_keyword_trie::ADDED:
    return false;
  case Sql_ext_keyword_trie::CONFLICT:
  {
    entries->pop();
    auto *other= static_cast<Sql_ext_plugin *>(conflict.owner);
    my_error(ER_SQL_EXTENSION_PREFIX_OVERLAP, MYF(0), conflict.refused,
             entry->plugin->name.str, conflict.prefix,
             other->plugin->name.str);
    return true;
  }
  case Sql_ext_keyword_trie::OUT_OF_MEMORY:
    break;
  }
  entries->pop();
  my_error(ER_OUT_OF_RESOURCES, MYF(0));
  return true;
}


/** @return the entry of a plugin, or NULL if it is not registered. Caller
    holds the lock. */
Sql_ext_plugin *registry_find(const st_plugin_int *plugin)
{
  for (size_t i= 0; i < entries->elements(); i++)
    if (entries->at(i)->plugin == plugin)
      return entries->at(i);
  return nullptr;
}


void registry_remove(Sql_ext_plugin *entry)
{
  trie->remove(entry);
  size_t i= recognizers->find_first(entry);
  if (i != Dynamic_array<Sql_ext_plugin *>::NOT_FOUND)
    recognizers->del(i);
  i= entries->find_first(entry);
  DBUG_ASSERT(i != Dynamic_array<Sql_ext_plugin *>::NOT_FOUND);
  entries->del(i);
}

} // namespace


bool sql_extension_init()
{
#ifdef HAVE_PSI_INTERFACE
  mysql_rwlock_register("sql", sql_extension_rwlocks,
                        array_elements(sql_extension_rwlocks));
  mysql_memory_register("sql", sql_extension_memory,
                        array_elements(sql_extension_memory));
#endif
  mysql_rwlock_init(key_rwlock_LOCK_sql_extension, &LOCK_sql_extension);
  registry_initialized= true;
  trie= new (std::nothrow) Sql_ext_keyword_trie(key_memory_sql_extension);
  recognizers= new (std::nothrow)
    Dynamic_array<Sql_ext_plugin *>(key_memory_sql_extension, 4, 4);
  entries= new (std::nothrow)
    Dynamic_array<Sql_ext_plugin *>(key_memory_sql_extension, 4, 4);
  return !trie || !trie->is_valid() || !recognizers || !entries;
}


void sql_extension_free()
{
  /* Nothing to free when start-up aborted before sql_extension_init() */
  if (!registry_initialized)
    return;
  DBUG_ASSERT(!installed_count);
  DBUG_ASSERT(!trie || !trie->is_valid() || trie->empty());
  delete trie;
  trie= nullptr;
  delete recognizers;
  recognizers= nullptr;
  delete entries;
  entries= nullptr;
  mysql_rwlock_destroy(&LOCK_sql_extension);
  registry_initialized= false;
}


int initialize_sql_extension_plugin(void *plugin_)
{
  st_plugin_int *plugin= static_cast<st_plugin_int *>(plugin_);
  auto *d= static_cast<const st_mariadb_sql_extension *>(plugin->plugin->info);

  if (const char *why= check_descriptor(d))
  {
    my_error(ER_SQL_EXTENSION_BAD_DESCRIPTOR, MYF(0), plugin->name.str, why);
    return 1;
  }

  /*
    Initialize the plugin before registering it, so that no statement
    reaches it before it is ready. Do not hold the registry lock while
    plugin code runs: its init may run SQL, whose parse errors take the
    read lock.
  */
  if (plugin->plugin->init && plugin->plugin->init(plugin))
  {
    sql_print_error("Plugin '%s' init function returned error.",
                    plugin->name.str);
    return 1;
  }

  auto *entry= new (std::nothrow) Sql_ext_plugin(plugin, d);
  if (!entry)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), int(sizeof(Sql_ext_plugin)));
    goto err;
  }

  mysql_rwlock_wrlock(&LOCK_sql_extension);
  if (registry_add(entry))
  {
    mysql_rwlock_unlock(&LOCK_sql_extension);
    delete entry;
    goto err;
  }
  installed_count++;
  mysql_rwlock_unlock(&LOCK_sql_extension);
  return 0;

err:
  if (plugin->plugin->deinit && plugin->plugin->deinit(plugin))
    sql_print_warning("Plugin '%s' deinit function returned error.",
                      plugin->name.str);
  return 1;
}


int finalize_sql_extension_plugin(void *plugin_)
{
  st_plugin_int *plugin= static_cast<st_plugin_int *>(plugin_);

  DEBUG_SYNC(current_thd, "sql_extension_finalize");
  /* The write lock waits for recognize() calls in progress to return */
  mysql_rwlock_wrlock(&LOCK_sql_extension);
  Sql_ext_plugin *entry= registry_find(plugin);
  if (entry)
  {
    registry_remove(entry);
    installed_count--;
  }
  mysql_rwlock_unlock(&LOCK_sql_extension);

  /*
    No entry if initialize_sql_extension_plugin() failed: it called
    deinit() itself if init() had succeeded, as ha_initialize_handlerton()
    does
  */
  if (!entry)
    return 0;
  delete entry;

  if (plugin->plugin->deinit && plugin->plugin->deinit(plugin))
    return 1;
  return 0;
}


namespace {

/**
  Find the plugin that claims a statement: the claimed prefix if any,
  otherwise the first recognizer (by priority, then name) whose
  recognize() returns non-zero.

  @param thd      the client's session: its sql_mode decides how quotes
                  are read when looking for the end of a prefix plugin's
                  statement
  @param cs       character set of the statement text
  @param query    the statement text, to the end of the packet
  @param plugin   the plugin, pinned by plugin_lock_ready() until
                  plugin_unlock()
  @param claimed  the number of bytes of the statement it claims; more
                  than query.length if a recognizer claims past the end
  @param lock_failed  set if the plugin that claims the statement is
                  READY but could not be pinned: out of memory, in debug
                  builds only

  @return true if a plugin claimed the statement and was pinned; a plugin
          that is not READY (being installed or uninstalled) claims nothing
*/
bool sql_extension_find(THD *thd, CHARSET_INFO *cs, const LEX_CSTRING &query,
                        plugin_ref *plugin, size_t *claimed,
                        bool *lock_failed)
{
  if (!installed_count)
    return false;

  Sql_ext_plugin *winner= nullptr;
  bool prefix_claim= false;

  mysql_rwlock_rdlock(&LOCK_sql_extension);
  if (Sql_ext_owner *claim= trie->match(cs, query))
  {
    winner= static_cast<Sql_ext_plugin *>(claim);
    prefix_claim= true;
  }
  else
  {
    for (size_t i= 0; i < recognizers->elements(); i++)
    {
      Sql_ext_plugin *r= recognizers->at(i);
      if (size_t n= r->descriptor->recognize(query.str, query.length))
      {
        /*
          Even a claim past the end, a plugin bug: the caller refuses the
          statement with an error that names the plugin
        */
        winner= r;
        *claimed= n;
        break;
      }
    }
  }

  bool found= false;
  if (winner)
  {
    /*
      Only a READY plugin claims a statement. plugin_lock() would also lock
      one that UNINSTALL has marked deleted while it is still in use, which
      would keep claiming new statements and might never be unloaded. Not
      claimed, the statement keeps its parse error.
    */
    st_plugin_int *pi= winner->plugin;
    if (DBUG_IF("sql_extension_lock_oom"))
      *lock_failed= true;
    else if ((*plugin= plugin_lock_ready(NULL, plugin_int_to_ref(pi),
                                         lock_failed)))
      found= true;
  }
  mysql_rwlock_unlock(&LOCK_sql_extension);

  /*
    A prefix plugin's statement ends at the first ';' outside quotes and
    comments, like an ordinary one. The plugin is not asked: every plugin
    would need the same scan, and only the server knows the sql_mode. Done
    outside the lock, as the text can be long.
  */
  if (found && prefix_claim)
  {
    sql_mode_t mode= thd->variables.sql_mode;
    *claimed= sql_ext_statement_length(cs, query,
                                       mode & MODE_ANSI_QUOTES,
                                       mode & MODE_NO_BACKSLASH_ESCAPES);
  }
  return found;
}

} // namespace


/*
  Statement execution
*/

namespace {

/**
  The extension statement executing on this thread: its THD (NULL if none)
  and plugin. Deliberately thread-local, not a THD member: handler
  SQL runs on the same OS thread, on the client's THD or on a THD of its
  own (the plain SQL service under LOCK TABLES), and a nested extension
  statement is refused on both.
*/
struct Running_extension
{
  /** The client's THD; NULL while no extension statement executes */
  THD *thd;
  /** The plugin executing it, for error messages */
  const char *plugin_name;
};
thread_local Running_extension running= { nullptr, nullptr };

class Extension_running_guard
{
  Running_extension m_saved;
public:
  Extension_running_guard(THD *thd, const char *plugin_name)
    : m_saved(running)
  { running= { thd, plugin_name }; }
  ~Extension_running_guard() { running= m_saved; }
  Extension_running_guard(const Extension_running_guard &)= delete;
  Extension_running_guard &operator=(const Extension_running_guard &)= delete;
};


/**
  The command of a statement claimed by an extension plugin. It holds the
  plugin name and the claimed text, both on the statement's memory root, so
  it can be executed again as a prepared statement. The plugin is locked by
  name for each execution, as storage engines are; if it was uninstalled
  since PREPARE, EXECUTE fails with ER_PLUGIN_IS_NOT_LOADED.
*/
class Sql_cmd_sql_extension : public Sql_cmd
{
  LEX_CSTRING m_plugin_name;
  LEX_CSTRING m_text;
public:
  Sql_cmd_sql_extension(LEX_CSTRING plugin_name, LEX_CSTRING text)
    : m_plugin_name(plugin_name), m_text(text) {}
  enum_sql_command sql_command_code() const override
  { return SQLCOM_SQL_EXTENSION; }
  bool execute(THD *thd) override;
};


/**
  A claimed statement's text in utf8mb4, the character set of everything a
  plugin sees, on the statement's memory root: converted when the client's
  character set (cs) is another one. A character utf8mb4 lacks cannot
  occur (every client character set maps to Unicode); a byte sequence
  that is not a character of cs becomes '?', as in a string literal. NULL
  str on out of memory, without an error: the caller raises it.
*/
static LEX_CSTRING utf8mb4_text(THD *thd, CHARSET_INFO *cs, const char *str,
                                size_t length)
{
  uint32 offset;
  if (!String::needs_conversion(length, cs, &my_charset_utf8mb4_bin, &offset))
    return thd->strmake_lex_cstring(str, length);
  size_t max_length= length / cs->mbminlen * my_charset_utf8mb4_bin.mbmaxlen;
  char *to= (char *) thd->alloc(max_length + 1);
  if (!to)
    return { NULL, 0 };
  uint errors;
  size_t to_length= copy_and_convert(to, max_length, &my_charset_utf8mb4_bin,
                                     str, length, cs, &errors);
  to[to_length]= 0;
  return { to, to_length };
}


bool Sql_cmd_sql_extension::execute(THD *thd)
{
  /*
    EXECUTE of a prepared extension statement does not parse it, so
    sql_extension_dispatch() cannot refuse it there
  */
  if (running.thd)
  {
    my_error(ER_SQL_EXTENSION_NESTED, MYF(0), m_plugin_name.str,
             running.plugin_name);
    return true;
  }
  /*
    Only a READY plugin: plugin_lock_by_name() would also lock one that
    UNINSTALL has marked deleted while another statement still uses it
  */
  plugin_ref plugin= plugin_lock_by_name_ready(NULL, &m_plugin_name,
                                               MariaDB_SQL_EXTENSION_PLUGIN);
  if (!plugin)
  {
    if (!thd->is_error())               // else the lookup raised one
      my_error(ER_PLUGIN_IS_NOT_LOADED, MYF(0), m_plugin_name.str);
    return true;
  }
  auto *d= static_cast<const st_mariadb_sql_extension *>
             (plugin_decl(plugin)->info);
  int rc;
  {
    /*
      Nothing the handler runs may be stored by the query cache (why: see
      sql_extension_dispatch(), which clears this for a statement sent as
      text). EXECUTE of a prepared statement clears it in its own lookup,
      which safe_to_cache_query= 0 makes fail, except when it opens a
      cursor, which skips the lookup: cleared here for every path.
    */
    thd->query_cache_is_applicable= 0;
    Extension_running_guard guard(thd, m_plugin_name.str);
    rc= d->execute(thd, m_text.str, m_text.length);
  }
  plugin_unlock(NULL, plugin);

  Diagnostics_area *da= thd->get_stmt_da();
  if (rc && !thd->is_error())
  {
    /* Failure must come with an error, or the client gets nothing */
    da->reset_diagnostics_area();
    my_error(ER_SQL_EXTENSION_FAILED, MYF(0), m_plugin_name.str);
  }
  if (thd->is_error())
    return true;
  if (!da->is_set())
    my_ok(thd);                         // success without a result
  return false;
}


} // namespace


/*
  The SQL extension services (include/mysql/service_sql_extension*.h)
*/

namespace {

/**
  Check that a service function is called from execute(), on its THD.
  Returns true with an error raised otherwise.
*/
bool check_service_call(THD *thd, const char *func)
{
  if (!thd || thd != running.thd || thd != current_thd)
  {
    /* A plugin bug, reported to the plugin as the header promises */
    my_error(ER_STD_INVALID_ARGUMENT, MYF(0),
             "thd is not the connection of the execute() in progress", func);
    return true;
  }
  return false;
}

} // namespace


namespace {

/** A SQL_EXTENSION_PRIV_* bit and the privilege it stands for */
struct Priv_map
{
  unsigned long long bit;
  privilege_t acl;
};

constexpr Priv_map priv_map[]=
{
  { SQL_EXTENSION_PRIV_SELECT,         SELECT_ACL },
  { SQL_EXTENSION_PRIV_INSERT,         INSERT_ACL },
  { SQL_EXTENSION_PRIV_UPDATE,         UPDATE_ACL },
  { SQL_EXTENSION_PRIV_DELETE,         DELETE_ACL },
  { SQL_EXTENSION_PRIV_CREATE,         CREATE_ACL },
  { SQL_EXTENSION_PRIV_DROP,           DROP_ACL },
  { SQL_EXTENSION_PRIV_ALTER,          ALTER_ACL },
  { SQL_EXTENSION_PRIV_INDEX,          INDEX_ACL },
  { SQL_EXTENSION_PRIV_CREATE_VIEW,    CREATE_VIEW_ACL },
  { SQL_EXTENSION_PRIV_SHOW_VIEW,      SHOW_VIEW_ACL },
  { SQL_EXTENSION_PRIV_TRIGGER,        TRIGGER_ACL },
  { SQL_EXTENSION_PRIV_EXECUTE,        EXECUTE_ACL },
  { SQL_EXTENSION_PRIV_CREATE_ROUTINE, CREATE_PROC_ACL },
  { SQL_EXTENSION_PRIV_ALTER_ROUTINE,  ALTER_PROC_ACL },
  { SQL_EXTENSION_PRIV_REFERENCES,     REFERENCES_ACL },
  { SQL_EXTENSION_PRIV_GRANT_OPTION,   GRANT_ACL },
  { SQL_EXTENSION_PRIV_DELETE_HISTORY, DELETE_HISTORY_ACL },
  { SQL_EXTENSION_PRIV_CREATE_TEMPORARY_TABLES, CREATE_TMP_ACL },
  { SQL_EXTENSION_PRIV_LOCK_TABLES,    LOCK_TABLES_ACL },
  { SQL_EXTENSION_PRIV_EVENT,          EVENT_ACL },
  { SQL_EXTENSION_PRIV_SHOW_CREATE_ROUTINE, SHOW_CREATE_ROUTINE_ACL },
};

constexpr privilege_t priv_map_acls(size_t i= 0)
{
  return i == array_elements(priv_map) ? NO_ACL :
         priv_map[i].acl | priv_map_acls(i + 1);
}

/* The service offers every privilege GRANT accepts on a database */
static_assert(priv_map_acls() == DB_ACLS,
              "SQL_EXTENSION_PRIV_* must map exactly to DB_ACLS");

} // namespace


int sql_extension_check_access(MYSQL_THD thd, unsigned long long privileges,
                               const char *db, const char *table)
{
  if (check_service_call(thd, "sql_extension_check_access"))
    return 1;

  privilege_t want= NO_ACL;
  unsigned long long known= 0;
  for (const Priv_map &m : priv_map)
  {
    known|= m.bit;
    if (privileges & m.bit)
      want|= m.acl;
  }
  const char *bad= !db || !*db ? "db is NULL or empty" :
                   table && !*table ? "table is empty" :
                   !privileges ? "privileges is 0" :
                   privileges & ~known ? "privileges has unknown bits" :
                   nullptr;
  if (bad)
  {
    my_error(ER_STD_INVALID_ARGUMENT, MYF(0), bad,
             "sql_extension_check_access");
    return 1;
  }

  /*
    Names as a statement would carry them: checked, and lower-cased as
    lower_case_table_names requires (the grants and the metadata locks
    are keyed on the normalized form)
  */
  LEX_CSTRING db_name= { db, strlen(db) };
  Lex_ident_db_normalized dbn= thd->to_ident_db_normalized_with_error(db_name);
  if (!dbn.str)
    return 1;                                   // ER_WRONG_DB_NAME or OOM
  db= dbn.str;

  if (!table)
  {
    /*
      The privileges on the database itself, from global and database
      grants, with the database as a name. check_access() does not do
      that: with dont_check_global_grants it reads the name as a GRANT
      pattern when GRANT OPTION is asked for (an escaped grant such as
      `my\_db` then fails to match my_db), and without it any table
      privilege in the database passes the check, leaving the rest to a
      table-level check that never comes. So: the built-in rules of the
      internal schemas as check_access() applies them, then the database
      privileges as fill_effective_table_privileges() computes them.
    */
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    Security_context *sctx= thd->security_ctx;
    bool denied= false;
    if (const ACL_internal_schema_access *access=
          get_cached_schema_access(NULL, db))
    {
      /* information_schema, performance_schema */
      privilege_t granted(NO_ACL);
      switch (access->check(want, &granted)) {
      case ACL_INTERNAL_ACCESS_GRANTED:
        return 0;
      case ACL_INTERNAL_ACCESS_DENIED:
        denied= true;
        break;
      case ACL_INTERNAL_ACCESS_CHECK_GRANT:
        want&= ~granted;
        break;
      }
    }
    if (!denied)
    {
      privilege_t db_access= (thd->db.str && !strcmp(db, thd->db.str) ?
                              sctx->db_access :
                              acl_get_all3(sctx, db, FALSE)) |
                             sctx->master_access;
      if ((db_access & want) == want)
        return 0;
    }
    status_var_increment(thd->status_var.access_denied_errors);
    my_error(ER_DBACCESS_DENIED_ERROR, MYF(0), sctx->priv_user,
             sctx->priv_host, db);
    return 1;
#else
    return 0;
#endif
  }

  LEX_CSTRING table_name= { table, strlen(table) };
  if (Lex_ident_table::check_name(table_name, false))
  {
    my_error(ER_WRONG_TABLE_NAME, MYF(0), table);
    return 1;
  }
  if (lower_case_table_names)
  {
    size_t size= table_name.length * files_charset_info->mbmaxlen + 1;
    char *buf= static_cast<char *>(thd->alloc(size));
    if (!buf)
      return 1;
    table_name.length= files_charset_info->casedn_z(table, table_name.length,
                                                    buf, size);
    table_name.str= buf;
  }
  TABLE_LIST tl;
  tl.init_one_table(&dbn, &table_name, NULL, TL_IGNORE);
  return check_table_access(thd, want, &tl, false, 1, false);
}



namespace {

/**
  Where the statement after a claim starts.

  @param next     receives the byte after the ';' that ends the claimed
                  statement, or NULL if nothing but whitespace and ';'
                  follows
  @param refused  receives, on failure, where the text that cannot follow
                  the claim starts

  @return false if the rest cannot be executed separately: something other
          than ';' follows the claim, or further statements follow in a
          packet that is not a multi-statement one (whitespace and ';'
          alone are accepted either way)
*/
bool split_after_claim(CHARSET_INFO *cs, const Lex_input_stream &lip,
                       const char *claim_end, const char **next,
                       const char **refused)
{
  const char *end= lip.get_end_of_query();
  const char *p= claim_end;
  *next= NULL;

  while (p < end && my_isspace(cs, *p))
    p++;
  if (p == end)
    return true;
  if (*p != ';')
  {
    *refused= p;
    return false;
  }
  const char *after_semicolon= ++p;
  while (p < end && (my_isspace(cs, *p) || *p == ';'))
    p++;
  if (p == end)
    return true;
  if (!lip.multi_statements)
  {
    *refused= p;
    return false;
  }
  *next= after_semicolon;
  return true;
}


/**
  A syntax error at pos, worded and placed as the parser does for an
  ordinary statement: "near '<text from pos>' at line <its line>".

  @param buf  the start of the statement, line 1
  @param end  the end of the query text
*/
void parse_error_at(THD *thd, const char *buf, const char *pos,
                    const char *end)
{
  uint line= 1;
  for (const char *p= buf; p < pos; p++)
    if (*p == '\n')
      line++;
  ErrConvString near_text(pos, size_t(end - pos),
                          thd->variables.character_set_client);
  my_printf_error(ER_PARSE_ERROR, ER_THD(thd, ER_PARSE_ERROR), MYF(0),
                  ER_THD(thd, ER_SYNTAX_ERROR), near_text.ptr(), line);
}


/*
  Drop the parse error and the conditions the rejected parse pushed.
  clear_error() only resets the status; the error condition stays in the
  warning list, so the list is emptied. It holds only this statement's
  conditions: those of earlier statements went at its first condition,
  usually the parse error. That also drops warnings raised before the
  parse, while evaluating the text of PREPARE ... FROM <expr> or EXECUTE
  IMMEDIATE <expr>.
*/
void clear_parse_error(THD *thd)
{
  thd->clear_error();
  thd->get_stmt_da()->clear_warning_info(thd->query_id);
}

} // namespace


bool sql_extension_dispatch(THD *thd, Parser_state *parser_state)
{
  if (!installed_count)
    return false;

  Diagnostics_area *da= thd->get_stmt_da();
  if (!da->is_error() || thd->is_fatal_error ||
      da->sql_errno() != ER_PARSE_ERROR)
    return false;
  if (thd->slave_thread || thd->in_sub_stmt || opt_bootstrap ||
      thd->lex->sphead)
    return false;
#ifdef WITH_WSREP
  if (thd->wsrep_applier)                       // Galera sets no slave_thread
    return false;
#endif

  Lex_input_stream &lip= parser_state->m_lip;
  const char *buf= lip.get_buf();
  size_t length= size_t(lip.get_end_of_query() - buf);
  CHARSET_INFO *cs= thd->charset();

  plugin_ref plugin;
  size_t claimed;
  bool lock_failed= false;
  if (!sql_extension_find(thd, cs, { buf, length }, &plugin, &claimed,
                          &lock_failed))
  {
    if (lock_failed)
    {
      /* The statement is a plugin's: say why it failed, not a syntax error */
      clear_parse_error(thd);
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
    }
    return false;
  }

  bool ok= false;
  const char *next= NULL, *refused= NULL;
  if (running.thd)
  {
    /* Replace the parse error with one that says what is wrong */
    clear_parse_error(thd);
    my_error(ER_SQL_EXTENSION_NESTED, MYF(0), plugin_name(plugin)->str,
             running.plugin_name);
  }
  else if (claimed > length)
  {
    /* A plugin bug, not the user's syntax: say which plugin */
    char what[80];
    my_snprintf(what, sizeof(what),
                "recognize() claimed %zu bytes of a %zu-byte statement",
                claimed, length);
    clear_parse_error(thd);
    my_error(ER_SQL_EXTENSION_INVALID_RESULT, MYF(0),
             plugin_name(plugin)->str, what);
  }
  else if (claimed == length ||
           split_after_claim(cs, lip, buf + claimed, &next, &refused))
  {
    /*
      Everything the command needs, on the statement's memory root (a
      prepared statement keeps it), before the parse error goes: the
      memory root raises no error while another is set, so a failure
      here must be reported by hand, and not as a syntax error.
    */
    LEX_CSTRING plugin_name_copy=
      thd->strmake_lex_cstring(*plugin_name(plugin));
    LEX_CSTRING text= utf8mb4_text(thd, cs, buf, claimed);
    DBUG_EXECUTE_IF("sql_extension_dispatch_oom", text.str= NULL;);
    Sql_cmd *cmd= plugin_name_copy.str && text.str ?
      new (thd->mem_root) Sql_cmd_sql_extension(plugin_name_copy, text) :
      nullptr;
    clear_parse_error(thd);
    if (!cmd)
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
    else
    {
      LEX *lex= thd->lex;
      /*
        lex_start() resets context_analysis_only, and PREPARE set
        CONTEXT_ANALYSIS_ONLY_PREPARE in it, which must survive the restart
      */
      uint8 context_analysis_only= lex->context_analysis_only;
      lex_start(thd);
      lex->context_analysis_only= context_analysis_only;
      lex->sql_command= SQLCOM_SQL_EXTENSION;
      lex->safe_to_cache_query= 0;
      /*
        The query cache looked the text up before the parse and, if it
        starts like a SELECT, still treats this statement as one it may
        store under that text: the first SELECT the handler runs on the
        client's protocol would be registered under the carrier's text
        and served to the next client sending it, without the plugin
        running.
      */
      thd->query_cache_is_applicable= 0;
      lex->m_sql_cmd= cmd;
      lip.found_semicolon= next;
      ok= true;
    }
  }
  else
  {
    /*
      The claimed statement is the plugin's, what follows it cannot run:
      point the syntax error there, not at the whole statement
    */
    clear_parse_error(thd);
    parse_error_at(thd, buf, refused, lip.get_end_of_query());
  }
  plugin_unlock(NULL, plugin);
  return ok;
}
