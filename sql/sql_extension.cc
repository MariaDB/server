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
#include "sql_type_json.h"                      // Type_handler_json_common
#include "my_decimal.h"
#include <json_lib.h>
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

class Result_sequence;

/**
  The extension statement executing on this thread: its THD (NULL if none),
  plugin and results. Deliberately thread-local, not a THD member: handler
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
  /** Where the service functions put its results */
  Result_sequence *results;
};
thread_local Running_extension running= { nullptr, nullptr, nullptr };

class Extension_running_guard
{
  Running_extension m_saved;
public:
  Extension_running_guard(THD *thd, const char *plugin_name,
                          Result_sequence *results) : m_saved(running)
  { running= { thd, plugin_name, results }; }
  ~Extension_running_guard() { running= m_saved; }
  Extension_running_guard(const Extension_running_guard &)= delete;
  Extension_running_guard &operator=(const Extension_running_guard &)= delete;
};


/** SQL_EXTENSION_CLIENT_* for the statement executing on thd */
uint client_flags(THD *thd)
{
  bool binary= thd->protocol->type() == Protocol::PROTOCOL_BINARY;
  uint flags= 0;
  if (thd->client_capabilities &
      (binary ? CLIENT_PS_MULTI_RESULTS : CLIENT_MULTI_RESULTS))
    flags|= SQL_EXTENSION_CLIENT_MULTI_RESULTS;
  if ((thd->client_capabilities & CLIENT_MULTI_STATEMENTS) &&
      thd->stmt_arena->type() != Query_arena::PREPARED_STATEMENT)
    flags|= SQL_EXTENSION_CLIENT_MULTI_STATEMENTS;
  DBUG_EXECUTE_IF("sql_extension_no_client_multi_results",
                  flags&= ~SQL_EXTENSION_CLIENT_MULTI_RESULTS;);
  DBUG_EXECUTE_IF("sql_extension_no_client_multi_statements",
                  flags&= ~SQL_EXTENSION_CLIENT_MULTI_STATEMENTS;);
  return flags;
}


/**
  The results of one extension statement. The final packet of the latest
  result (its OK or end of rows) is held back: when another result starts
  it is sent with SERVER_MORE_RESULTS_EXISTS, and when the statement ends
  it becomes the statement's own final packet. So a single result is sent
  with no SERVER_MORE_RESULTS_EXISTS, several end like those of a stored
  procedure, and an error after some results still lets the client read
  them.
*/
class Result_sequence
{
  enum { NONE, END_OF_ROWS, OK } m_held= NONE;
  ulonglong m_affected_rows= 0;
  ulonglong m_last_insert_id= 0;
  char m_message[MYSQL_ERRMSG_SIZE];

  const char *message() const { return m_message[0] ? m_message : nullptr; }

public:
  Result_sequence()= default;
  Result_sequence(const Result_sequence &)= delete;
  Result_sequence &operator=(const Result_sequence &)= delete;
  /** Every held result is sent (finish(), send_with_more()) or dropped */
  ~Result_sequence() { DBUG_ASSERT(m_held == NONE); }

  bool holds_result() const { return m_held != NONE; }

  /**
    Take the result the diagnostics area holds (the OK or end of rows of a
    result just sent) and empty it, so that a later error can be raised.
  */
  void hold(THD *thd)
  {
    Diagnostics_area *da= thd->get_stmt_da();
    DBUG_ASSERT(da->is_ok() || da->is_eof());
    DBUG_ASSERT(m_held == NONE);
    if (da->is_ok())
    {
      m_held= OK;
      m_affected_rows= da->affected_rows();
      m_last_insert_id= da->last_insert_id();
      strmake_buf(m_message, da->message());
    }
    else
      m_held= END_OF_ROWS;
    da->reset_diagnostics_area();
  }

  /** Send the held result, followed by more results */
  void send_with_more(THD *thd)
  {
    if (m_held == NONE)
      return;
    /*
      Through a diagnostics area of its own, so that one the statement
      holds (an error) is kept. Writing to a client that is gone (KILL
      CONNECTION, a disconnect) raises an error in it, so it needs warning
      memory; the error goes with it, as the client cannot get it anyway.
    */
    Diagnostics_area da(thd->query_id, false, true);
    Diagnostics_area *save_da= thd->get_stmt_da();
    thd->set_stmt_da(&da);
    if (m_held == OK)
      da.set_ok_status(m_affected_rows, m_last_insert_id, message());
    else
      da.set_eof_status(thd);
    uint more= ~thd->server_status & SERVER_MORE_RESULTS_EXISTS;
    thd->server_status|= SERVER_MORE_RESULTS_EXISTS;
    thd->protocol->end_statement();
    thd->server_status&= ~more;
    thd->set_stmt_da(save_da);
    m_held= NONE;
  }

  /** Drop the held result: an error replaces it */
  void drop() { m_held= NONE; }

  /** Make the held result the final packet of the statement */
  void finish(THD *thd)
  {
    DBUG_ASSERT(!thd->get_stmt_da()->is_set());
    if (m_held == OK)
      my_ok(thd, m_affected_rows, m_last_insert_id, message());
    else if (m_held == END_OF_ROWS)
      my_eof(thd);
    m_held= NONE;
  }
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
  Refuse an OK or end of rows the plugin set itself, not through the
  service (a plugin built with the server headers can call my_ok()): the
  service cannot tell what it belongs to.
  @return true
*/
bool refuse_result_outside_service(Diagnostics_area *da,
                                   const char *plugin_name)
{
  da->reset_diagnostics_area();
  my_error(ER_SQL_EXTENSION_INVALID_RESULT, MYF(0), plugin_name,
           "a result was set outside the service");
  return true;
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
  Result_sequence results;
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
    Extension_running_guard guard(thd, m_plugin_name.str, &results);
    rc= d->execute(thd, m_text.str, m_text.length);
    /* A plugin built with the server headers can call my_ok() itself */
    DBUG_EXECUTE_IF("sql_extension_plugin_sets_ok",
                    if (!thd->get_stmt_da()->is_set()) my_ok(thd););
  }
  plugin_unlock(NULL, plugin);

  Diagnostics_area *da= thd->get_stmt_da();
  if (rc && !thd->is_error())
  {
    /* Failure must come with an error, or the client gets nothing */
    da->reset_diagnostics_area();
    my_error(ER_SQL_EXTENSION_FAILED, MYF(0), m_plugin_name.str);
  }
  else if (da->is_set() && !da->is_error())
    refuse_result_outside_service(da, m_plugin_name.str);
  if (thd->is_error())
  {
    /*
      The results sent so far, then the error; a client that does not
      accept several results gets the error in place of the end of the
      result it was sent, as when a SELECT fails after some rows
    */
    if (client_flags(thd) & SQL_EXTENSION_CLIENT_MULTI_RESULTS)
      results.send_with_more(thd);
    else
      results.drop();
    return true;
  }
  if (results.holds_result())
    results.finish(thd);
  else
    my_ok(thd);                         // success without a result
  return false;
}


} // namespace


/*
  The SQL extension service (include/mysql/service_sql_extension.h)
*/

namespace {

bool invalid_result(const char *what)
{
  my_error(ER_SQL_EXTENSION_INVALID_RESULT, MYF(0), running.plugin_name, what);
  return true;
}


/**
  Check that a service function is called from execute(), on its THD.
  With `result`, the function starts a result: the statement must have no
  error, and a result already sent is followed by more results, which the
  client must accept. Returns true with an error raised, or already set,
  otherwise.
*/
bool check_service_call(THD *thd, const char *func, bool result)
{
  if (!thd || thd != running.thd || thd != current_thd)
  {
    /* A plugin bug, reported to the plugin as the header promises */
    my_error(ER_STD_INVALID_ARGUMENT, MYF(0),
             "thd is not the connection of the execute() in progress", func);
    return true;
  }
  if (!result)
    return false;
  DBUG_EXECUTE_IF("sql_extension_plugin_sets_ok",
                  if (!thd->get_stmt_da()->is_set()) my_ok(thd););
  Diagnostics_area *da= thd->get_stmt_da();
  if (da->is_error())
    return true;
  Result_sequence *results= running.results;
  if (da->is_set())
    return refuse_result_outside_service(da, running.plugin_name);
  if (results->holds_result())
  {
    if (!(client_flags(thd) & SQL_EXTENSION_CLIENT_MULTI_RESULTS))
      return invalid_result("the client does not accept several results");
    results->send_with_more(thd);
  }
  return false;
}


/**
  Parse a decimal integer cell. Leading and trailing spaces are not
  accepted: the plugin produced the text.
*/
bool parse_integer_cell(const sql_extension_cell &cell, bool is_unsigned,
                        longlong *value)
{
  const char *p= cell.str, *end= cell.str + cell.length;
  bool negative= false;
  if (p < end && *p == '-' && !is_unsigned)
  {
    negative= true;
    p++;
  }
  if (p == end)
    return true;
  ulonglong limit= is_unsigned ? ULONGLONG_MAX :
                   negative ? ulonglong(LONGLONG_MAX) + 1 : LONGLONG_MAX;
  ulonglong n= 0;
  for (; p < end; p++)
  {
    if (*p < '0' || *p > '9')
      return true;
    uint digit= uint(*p - '0');
    if (n > (limit - digit) / 10)
      return true;
    n= n * 10 + digit;
  }
  *value= negative ? longlong(0 - n) : longlong(n);
  return false;
}


/** State shared by the cells of one result set */
struct Parse_context
{
  THD *thd;
  json_engine_t je;
  bool je_ready= false;               // je.stack set up, on the first use

  explicit Parse_context(THD *thd_arg) : thd(thd_arg) {}
};


/** A cell parsed according to the type of its column */
struct Cell_value
{
  longlong integer;
  double real;
  my_decimal decimal;
  MYSQL_TIME time;
  uint fraction_digits;               // of TIME and DATETIME cells
};


/**
  The type of a column, once sql_extension_send_result_set() has checked
  it: as an enum, so that -Wswitch checks the switches on it
*/
typedef enum sql_extension_column_type Column_type;

inline Column_type column_type(const sql_extension_column &col)
{
  return static_cast<Column_type>(col.type);
}


/** Cells of STRING, BINARY and JSON columns are sent as given */
bool is_text_column(Column_type type)
{
  return type == SQL_EXTENSION_COLUMN_STRING ||
         type == SQL_EXTENSION_COLUMN_BINARY ||
         type == SQL_EXTENSION_COLUMN_JSON;
}


CHARSET_INFO *column_charset(Column_type type)
{
  switch (type) {
  case SQL_EXTENSION_COLUMN_STRING: return &my_charset_utf8mb4_general_ci;
  case SQL_EXTENSION_COLUMN_BINARY: return &my_charset_bin;
  case SQL_EXTENSION_COLUMN_JSON:   return &my_charset_utf8mb4_bin;
  case SQL_EXTENSION_COLUMN_INTEGER:
  case SQL_EXTENSION_COLUMN_UNSIGNED:
  case SQL_EXTENSION_COLUMN_DOUBLE:
  case SQL_EXTENSION_COLUMN_DECIMAL:
  case SQL_EXTENSION_COLUMN_DATE:
  case SQL_EXTENSION_COLUMN_TIME:
  case SQL_EXTENSION_COLUMN_DATETIME:
    break;
  }
  return &my_charset_numeric;
}

/**
  Parse a cell in the format of its column's type (service_sql_extension.h).

  @return true if the text is not a valid value of that type
*/
bool parse_cell(Parse_context *ctx, Column_type type,
                const sql_extension_cell &cell, Cell_value *value)
{
  const char *end= cell.str + cell.length;
  MYSQL_TIME_STATUS status;
  my_time_status_init(&status);
  value->fraction_digits= 0;

  switch (type) {
  case SQL_EXTENSION_COLUMN_STRING:
    /* Sent as utf8mb4: ill-formed text would reach the client as '?' */
    return Well_formed_prefix(&my_charset_utf8mb4_general_ci, cell.str,
                              cell.length).length() != cell.length;
  case SQL_EXTENSION_COLUMN_BINARY:
    return false;
  case SQL_EXTENSION_COLUMN_INTEGER:
  case SQL_EXTENSION_COLUMN_UNSIGNED:
    return parse_integer_cell(cell, type == SQL_EXTENSION_COLUMN_UNSIGNED,
                              &value->integer);
  case SQL_EXTENSION_COLUMN_DOUBLE:
  {
    char *stop= const_cast<char *>(end);
    int error;
    if (!cell.length)
      return true;
    value->real= my_strtod(cell.str, &stop, &error);
    return error || stop != end;
  }
  case SQL_EXTENSION_COLUMN_DECIMAL:
  {
    const char *stop;
    /* Mask 0: report the outcome, raise nothing */
    return !cell.length ||
           str2my_decimal(0, cell.str, cell.length, &my_charset_latin1,
                          &value->decimal, &stop) != E_DEC_OK ||
           stop != end;
  }
  case SQL_EXTENSION_COLUMN_DATE:
  case SQL_EXTENSION_COLUMN_DATETIME:
    if (str_to_datetime_or_date(cell.str, cell.length, &value->time, 0,
                                &status) || status.warnings)
      return true;
    value->fraction_digits= status.precision;
    return value->time.time_type !=
           (type == SQL_EXTENSION_COLUMN_DATE ? MYSQL_TIMESTAMP_DATE
                                              : MYSQL_TIMESTAMP_DATETIME);
  case SQL_EXTENSION_COLUMN_TIME:
    if (str_to_datetime_or_date_or_time(cell.str, cell.length, &value->time,
                                        0, &status, TIME_MAX_HOUR,
                                        TIME_MAX_HOUR) || status.warnings)
      return true;
    value->fraction_digits= status.precision;
    return value->time.time_type != MYSQL_TIMESTAMP_TIME;
  case SQL_EXTENSION_COLUMN_JSON:
    if (!ctx->je_ready)
    {
      if (mem_root_dynamic_array_init(ctx->thd->mem_root, PSI_INSTRUMENT_MEM,
                                      &ctx->je.stack, sizeof(int), NULL,
                                      JSON_DEPTH_DEFAULT, JSON_DEPTH_INC,
                                      MYF(0)))
        return true;
      ctx->je_ready= true;
    }
    /* As item_jsonfunc.cc sets it: the engine polls the volatile state */
    ctx->je.killed_ptr= const_cast<uint32_t *>(
      reinterpret_cast<volatile uint32_t *>(&ctx->thd->killed));
    DBUG_EXECUTE_IF("sql_extension_json_killed",
                    ctx->thd->set_killed(KILL_QUERY););
    return !json_valid(cell.str, cell.length, &my_charset_utf8mb4_bin,
                       &ctx->je);
  }
  return true;
}


/** Indexed by enum sql_extension_column_type, for error messages */
const char *const column_type_names[]=
{
  "utf8mb4 string", "binary string", "integer", "unsigned integer", "double",
  "decimal", "date", "time", "datetime", "JSON document"
};
static_assert(array_elements(column_type_names) ==
              SQL_EXTENSION_COLUMN_JSON + 1, "a name for every column type");


/** What the cells of a column require of its metadata */
struct Column_extent : public Sql_alloc
{
  size_t max_chars= 0;                // longest cell in characters
  uint int_digits= 1;                 // DECIMAL: digits before the point
  uint decimals= 0;                   // DECIMAL scale, fractional seconds
  bool nullable= false;
};


/**
  The metadata of one result set column, with the type handler
  make_column_item() chose for it.
*/
class Item_extension_column : public Item_empty_string
{
  const Type_handler *m_handler;
public:
  Item_extension_column(THD *thd, const char *name, uint length,
                        CHARSET_INFO *cs, const Type_handler *handler)
    : Item_empty_string(thd, name, length, cs), m_handler(handler)
  { DBUG_ASSERT(handler); }
  const Type_handler *type_handler() const override { return m_handler; }
  void make_send_field(THD *thd, Send_field *field) override
  { init_make_send_field(field, type_handler()); }
protected:
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_extension_column>(thd, this); }
};


Item *make_column_item(THD *thd, const sql_extension_column &col,
                       const Column_extent &extent)
{
  /*
    Display width of strings in characters, as for a VARCHAR(n); at least
    1, as for ''. Item_empty_string multiplies it by the charset's bytes
    per character, 4 for utf8mb4, hence the bound.
  */
  uint length= uint(MY_MIN(MY_MAX(extent.max_chars, 1), UINT_MAX32 / 4));
  uint frac= extent.decimals;
  const Type_handler *handler= NULL;
  Column_type type= column_type(col);
  CHARSET_INFO *cs= column_charset(type);
  uint max_length= 0;
  bool is_unsigned= false;

  switch (type) {
  case SQL_EXTENSION_COLUMN_STRING:
  case SQL_EXTENSION_COLUMN_BINARY:
    /* VARCHAR, or a BLOB type for long strings, as for any string */
    handler= Type_handler::string_type_handler(length * cs->mbmaxlen);
    break;
  case SQL_EXTENSION_COLUMN_JSON:
    handler= Type_handler_json_common::json_type_handler(length *
                                                         cs->mbmaxlen);
    break;
  case SQL_EXTENSION_COLUMN_UNSIGNED:
    is_unsigned= true;
    /* fall through */
  case SQL_EXTENSION_COLUMN_INTEGER:
    handler= is_unsigned
             ? static_cast<const Type_handler *>(&type_handler_ulonglong)
             : &type_handler_slonglong;
    max_length= MY_INT64_NUM_DECIMAL_DIGITS;
    frac= 0;
    break;
  case SQL_EXTENSION_COLUMN_DOUBLE:
    handler= &type_handler_double;
    max_length= MAX_DOUBLE_STR_LENGTH;
    frac= NOT_FIXED_DEC;
    break;
  case SQL_EXTENSION_COLUMN_DECIMAL:
    handler= &type_handler_newdecimal;
    max_length= my_decimal_precision_to_length_no_truncation(
                  decimal_digits_t(extent.int_digits + frac),
                  decimal_digits_t(frac), false);
    break;
  case SQL_EXTENSION_COLUMN_DATE:
    handler= &type_handler_newdate;
    break;
  case SQL_EXTENSION_COLUMN_TIME:
    handler= &type_handler_time2;
    break;
  case SQL_EXTENSION_COLUMN_DATETIME:
    handler= &type_handler_datetime2;
    break;
  }

  /* The Item copies the name onto the memory root */
  Item_extension_column *item= new (thd->mem_root)
    Item_extension_column(thd, col.name, length, cs, handler);
  if (!item)
    return NULL;
  switch (type) {
  case SQL_EXTENSION_COLUMN_STRING:
  case SQL_EXTENSION_COLUMN_BINARY:
  case SQL_EXTENSION_COLUMN_JSON:
    break;
  case SQL_EXTENSION_COLUMN_DATE:
    item->fix_attributes_date();
    break;
  case SQL_EXTENSION_COLUMN_TIME:
    item->fix_attributes_time(decimal_digits_t(frac));
    break;
  case SQL_EXTENSION_COLUMN_DATETIME:
    item->fix_attributes_datetime(decimal_digits_t(frac));
    break;
  case SQL_EXTENSION_COLUMN_INTEGER:
  case SQL_EXTENSION_COLUMN_UNSIGNED:
  case SQL_EXTENSION_COLUMN_DOUBLE:
  case SQL_EXTENSION_COLUMN_DECIMAL:
    item->max_length= max_length;
    item->decimals= decimal_digits_t(frac);
    item->unsigned_flag= is_unsigned;
    break;
  }
  item->set_maybe_null(extent.nullable);
  return item;
}


/**
  Send one non-NULL cell, already checked by parse_cell(). Strings and JSON
  documents are sent as given; other cells are parsed again for their value.
*/
bool store_cell(Parse_context *ctx, Protocol *protocol, Column_type type,
                const sql_extension_cell &cell, const Column_extent &extent)
{
  Cell_value value;
  if (!is_text_column(type))
  {
    /* Checked already by the caller: the same text parses the same way */
    bool bad= parse_cell(ctx, type, cell, &value);
    DBUG_ASSERT(!bad);
    if (bad)
    {
      my_error(ER_INTERNAL_ERROR, MYF(0),
               "an SQL extension result cell no longer parses");
      return true;
    }
  }
  switch (type) {
  case SQL_EXTENSION_COLUMN_INTEGER:
  case SQL_EXTENSION_COLUMN_UNSIGNED:
    return protocol->store_longlong(value.integer,
                                    type == SQL_EXTENSION_COLUMN_UNSIGNED);
  case SQL_EXTENSION_COLUMN_DOUBLE:
    return protocol->store_double(value.real, NOT_FIXED_DEC);
  case SQL_EXTENSION_COLUMN_DECIMAL:
  {
    /* Every value with the column's scale, as for a DECIMAL column */
    StringBuffer<DECIMAL_MAX_STR_LENGTH> str;
    my_decimal rounded;
    value.decimal.to_string_round(&str, decimal_digits_t(extent.decimals),
                                  &rounded);
    return protocol->store(str.ptr(), str.length(), &my_charset_numeric);
  }
  case SQL_EXTENSION_COLUMN_DATE:
    return protocol->store_date(&value.time);
  case SQL_EXTENSION_COLUMN_TIME:
    return protocol->store_time(&value.time, int(extent.decimals));
  case SQL_EXTENSION_COLUMN_DATETIME:
    return protocol->store_datetime(&value.time, int(extent.decimals));
  case SQL_EXTENSION_COLUMN_STRING:
  case SQL_EXTENSION_COLUMN_BINARY:
  case SQL_EXTENSION_COLUMN_JSON:
    break;
  }
  return protocol->store(cell.str, cell.length, column_charset(type));
}

} // namespace


int sql_extension_send_result_set(MYSQL_THD thd, size_t ncols,
                                  const struct sql_extension_column *cols,
                                  size_t nrows,
                                  const struct sql_extension_cell *cells)
{
  if (check_service_call(thd, "sql_extension_send_result_set", true))
    return 1;
  if (!ncols || !cols)
    return invalid_result("a result set needs columns");
  if (nrows && !cells)
    return invalid_result("rows were given without cells");

  /* Check everything before sending anything */
  char what[MYSQL_ERRMSG_SIZE];
  for (size_t c= 0; c < ncols; c++)
  {
    if (!cols[c].name)
    {
      my_snprintf(what, sizeof(what), "column %llu has no name",
                  ulonglong(c + 1));
      return invalid_result(what);
    }
    if (cols[c].type < SQL_EXTENSION_COLUMN_STRING ||
        cols[c].type > SQL_EXTENSION_COLUMN_JSON)
    {
      my_snprintf(what, sizeof(what), "column %-.64sQ has the unknown type %d",
                  cols[c].name, cols[c].type);
      return invalid_result(what);
    }
  }
  /* On the statement's memory root */
  Column_extent *extent= new (thd->mem_root) Column_extent[ncols];
  if (!extent)
    return 1;
  Parse_context ctx(thd);
  for (size_t r= 0; r < nrows; r++)
    for (size_t c= 0; c < ncols; c++)
    {
      const sql_extension_cell &cell= cells[r * ncols + c];
      const Column_type type= column_type(cols[c]);
      Column_extent &e= extent[c];
      if (!cell.str)
      {
        e.nullable= true;
        continue;
      }
      Cell_value value;
      if (parse_cell(&ctx, type, cell, &value))
      {
        /*
          Not the plugin's fault if the server ran out of memory (the
          error is set) or the statement was killed meanwhile
        */
        if (thd->is_error() || thd->send_kill_message())
          return 1;
        my_snprintf(what, sizeof(what),
                    "cell %llu of column %-.64sQ is not a valid %s",
                    ulonglong(r + 1), cols[c].name,
                    column_type_names[type]);
        return invalid_result(what);
      }
      set_if_bigger(e.max_chars, is_text_column(type) ?
                    column_charset(type)->
                      numchars(cell.str, cell.str + cell.length) :
                    cell.length);
      if (type == SQL_EXTENSION_COLUMN_DECIMAL)
      {
        set_if_bigger(e.int_digits, uint(value.decimal.intg));
        set_if_bigger(e.decimals, uint(value.decimal.frac));
      }
      else
        set_if_bigger(e.decimals, value.fraction_digits);
    }
  for (size_t c= 0; c < ncols; c++)
    if (column_type(cols[c]) == SQL_EXTENSION_COLUMN_DECIMAL &&
        (extent[c].decimals > DECIMAL_MAX_SCALE ||
         extent[c].int_digits + extent[c].decimals > DECIMAL_MAX_PRECISION))
    {
      my_snprintf(what, sizeof(what),
                  "decimal column %-.64sQ needs more than %u digits or more "
                  "than %u after the point", cols[c].name,
                  uint(DECIMAL_MAX_PRECISION), uint(DECIMAL_MAX_SCALE));
      return invalid_result(what);
    }

  List<Item> fields;
  for (size_t c= 0; c < ncols; c++)
  {
    Item *item= make_column_item(thd, cols[c], extent[c]);
    if (!item || fields.push_back(item, thd->mem_root))
      return 1;
  }

  Protocol *protocol= thd->protocol;
  if (protocol->send_result_set_metadata(&fields, Protocol::SEND_NUM_ROWS |
                                                  Protocol::SEND_EOF))
    return 1;
  for (size_t r= 0; r < nrows; r++)
  {
    protocol->prepare_for_resend();
    for (size_t c= 0; c < ncols; c++)
    {
      const sql_extension_cell &cell= cells[r * ncols + c];
      if (cell.str ? store_cell(&ctx, protocol, column_type(cols[c]), cell,
                                extent[c])
                   : protocol->store_null())
        return 1;
    }
    if (protocol->write())
      return 1;
    /* Rows_sent of the slow log, ROWS_SENT of the performance schema */
    thd->inc_sent_row_count(1);
  }
  my_eof(thd);
  running.results->hold(thd);
  return 0;
}


int sql_extension_send_ok(MYSQL_THD thd, unsigned long long affected_rows,
                          const char *message)
{
  if (check_service_call(thd, "sql_extension_send_ok", true))
    return 1;
  if (message)
  {
    /* Refused rather than cut, or sent as '?' */
    static_assert(MYSQL_ERRMSG_SIZE == 512, "the error message says 511");
    size_t length= strlen(message);
    if (length >= MYSQL_ERRMSG_SIZE)
      return invalid_result("an OK message longer than 511 bytes");
    if (Well_formed_prefix(&my_charset_utf8mb4_general_ci, message,
                           length).length() != length)
      return invalid_result("an OK message that is not well-formed utf8mb4");
  }
  my_ok(thd, ulonglong(affected_rows), 0, message);
  running.results->hold(thd);
  return 0;
}


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
  if (check_service_call(thd, "sql_extension_check_access", false))
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


unsigned int sql_extension_client_flags(MYSQL_THD thd)
{
  if (check_service_call(thd, "sql_extension_client_flags", false))
    return 0;
  return client_flags(thd);
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
    LEX_CSTRING text= thd->strmake_lex_cstring(buf, claimed);
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
