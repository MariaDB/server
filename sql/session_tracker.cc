/* Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2016, 2020, MariaDB

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


#include "sql_plugin.h"
#include "table.h"
#include "rpl_gtid.h"
#include "sql_class.h"
#include "sql_show.h"
#include "sql_plugin.h"
#include "set_var.h"

void State_tracker::set_changed(THD *thd)
{
  m_changed= true;
  thd->lex->safe_to_cache_query= 0;
  thd->server_status|= SERVER_SESSION_STATE_CHANGED;
}


/* To be used in expanding the buffer. */
static const unsigned int EXTRA_ALLOC= 1024;


void Session_sysvars_tracker::vars_list::reinit()
{
  track_all= 0;
  if (m_registered_sysvars.records)
    my_hash_reset(&m_registered_sysvars);
}

/**
  Copy the given list.

  @param  from    Source vars_list object.
  @param  thd     THD handle to retrive the charset in use.

  @retval true  there is something to track
  @retval false nothing to track
*/

void Session_sysvars_tracker::vars_list::copy(vars_list* from, THD *thd)
{
  track_all= from->track_all;
  free_hash();
  m_registered_sysvars= from->m_registered_sysvars;
  from->init();
}

/**
  Inserts the variable to be tracked into m_registered_sysvars hash.

  @param   svar   address of the system variable

  @retval false success
  @retval true  error
*/

bool Session_sysvars_tracker::vars_list::insert(const sys_var *svar)
{
  sysvar_node_st *node;
  if (!(node= (sysvar_node_st *) my_malloc(PSI_INSTRUMENT_ME,
                                           sizeof(sysvar_node_st),
                                           MYF(MY_WME |
                                               (mysqld_server_initialized ?
                                                MY_THREAD_SPECIFIC : 0)))))
    return true;

  node->m_svar= (sys_var *)svar;
  node->test_load= node->m_svar->test_load;
  node->m_changed= false;
  if (my_hash_insert(&m_registered_sysvars, (uchar *) node))
  {
    my_free(node);
    if (!search((sys_var *)svar))
    {
      //EOF (error is already reported)
      return true;
    }
  }
  return false;
}

/**
  Parse the specified system variables list.

  @Note In case of invalid entry a warning is raised per invalid entry.
  This is done in order to handle 'potentially' valid system
  variables from uninstalled plugins which might get installed in
  future.


  @param thd             [IN]    The thd handle.
  @param var_list        [IN]    System variable list.
  @param throw_error     [IN]    bool when set to true, returns an error
                                 in case of invalid/duplicate values.
  @param char_set	 [IN]	 charecter set information used for string
				 manipulations.

  @return
    true                    Error
    false                   Success
*/
bool Session_sysvars_tracker::vars_list::parse_var_list(THD *thd,
                                                        LEX_STRING var_list,
                                                        bool throw_error,
							CHARSET_INFO *char_set)
{
  const char separator= ',';
  char *token, *lasts= NULL;
  size_t rest= var_list.length;

  if (!var_list.str || var_list.length == 0)
    return false;

  if(!strcmp(var_list.str, "*"))
  {
    track_all= true;
    return false;
  }

  token= var_list.str;

  track_all= false;
  for (;;)
  {
    sys_var *svar;
    LEX_CSTRING var;

    lasts= (char *) memchr(token, separator, rest);

    var.str= token;
    if (lasts)
    {
      var.length= (lasts - token);
      rest-= var.length + 1;
    }
    else
      var.length= rest;

    /* Remove leading/trailing whitespace. */
    trim_whitespace(char_set, &var);

    if(!strcmp(var.str, "*"))
    {
      track_all= true;
    }
    else if ((svar= find_sys_var(thd, var.str, var.length, throw_error)))
    {
      if (insert(svar) == TRUE)
        return true;
    }
    else if (throw_error && thd)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WRONG_VALUE_FOR_VAR,
                          "%.*s is not a valid system variable and will"
                          "be ignored.", (int)var.length, token);
    }
    else
      return true;

    if (lasts)
      token= lasts + 1;
    else
      break;
  }
  return false;
}


bool sysvartrack_validate_value(THD *thd, const char *str, size_t len)
{
  LEX_STRING var_list= { (char *) str, len };
  const char separator= ',';
  char *token, *lasts= NULL;
  size_t rest= var_list.length;

  if (!var_list.str || var_list.length == 0 ||
      !strcmp(var_list.str, "*"))
  {
    return false;
  }

  token= var_list.str;

  for (;;)
  {
    LEX_CSTRING var;

    lasts= (char *) memchr(token, separator, rest);

    var.str= token;
    if (lasts)
    {
      var.length= (lasts - token);
      rest-= var.length + 1;
    }
    else
      var.length= rest;

    /* Remove leading/trailing whitespace. */
    trim_whitespace(system_charset_info, &var);

    if (!strcmp(var.str, "*") && !find_sys_var(thd, var.str, var.length))
      return true;

    if (lasts)
      token= lasts + 1;
    else
      break;
  }
  return false;
}


/* Sorts variable references array */
static int name_array_sorter(const void *a, const void *b)
{
  LEX_CSTRING **an= (LEX_CSTRING **)a, **bn=(LEX_CSTRING **)b;
  size_t min= MY_MIN((*an)->length, (*bn)->length);
  int res= strncmp((*an)->str, (*bn)->str, min);
  if (res == 0)
    res= ((int)(*bn)->length)- ((int)(*an)->length);
  return res;
}

/**
  Construct variable list by internal hash with references
*/

bool Session_sysvars_tracker::vars_list::construct_var_list(char *buf,
                                                            size_t buf_len)
{
  LEX_CSTRING **names;
  uint idx;
  size_t left= buf_len;
  size_t names_size= m_registered_sysvars.records * sizeof(LEX_CSTRING *);
  const char separator= ',';

  if (unlikely(buf_len < 1))
    return true;

  if (unlikely(track_all))
  {
    if (buf_len < 2)
      return true;
    buf[0]= '*';
    buf[1]= '\0';
    return false;
  }

  if (m_registered_sysvars.records == 0)
  {
    buf[0]= '\0';
    return false;
  }

  if (unlikely(!(names= (LEX_CSTRING**) my_safe_alloca(names_size))))
    return true;

  idx= 0;

  mysql_mutex_lock(&LOCK_plugin);
  for (ulong i= 0; i < m_registered_sysvars.records; i++)
  {
    sysvar_node_st *node= at(i);
    if (*node->test_load)
      names[idx++]= &node->m_svar->name;
  }
  DBUG_ASSERT(idx <= m_registered_sysvars.records);

  /*
    We check number of records again here because number of variables
    could be reduced in case of plugin unload.
  */
  if (m_registered_sysvars.records == 0)
  {
    mysql_mutex_unlock(&LOCK_plugin);
    buf[0]= '\0';
    return false;
  }

  my_qsort(names, idx, sizeof(LEX_CSTRING*), &name_array_sorter);

  for(uint i= 0; i < idx; i++)
  {
    LEX_CSTRING *nm= names[i];
    size_t ln= nm->length + 1;
    if (ln > left)
    {
      mysql_mutex_unlock(&LOCK_plugin);
      my_safe_afree(names, names_size);
      return true;
    }
    memcpy(buf, nm->str, nm->length);
    buf[nm->length]= separator;
    buf+= ln;
    left-= ln;
  }
  mysql_mutex_unlock(&LOCK_plugin);

  buf--; buf[0]= '\0';
  my_safe_afree(names, names_size);

  return false;
}


void Session_sysvars_tracker::init(THD *thd)
{
  mysql_mutex_assert_owner(&LOCK_global_system_variables);
  DBUG_ASSERT(thd->variables.session_track_system_variables ==
              global_system_variables.session_track_system_variables);
  DBUG_ASSERT(global_system_variables.session_track_system_variables);
  thd->variables.session_track_system_variables=
    my_strdup(PSI_INSTRUMENT_ME,
              global_system_variables.session_track_system_variables,
              MYF(MY_WME | MY_THREAD_SPECIFIC));
}


void Session_sysvars_tracker::deinit(THD *thd)
{
  my_free(thd->variables.session_track_system_variables);
  thd->variables.session_track_system_variables= 0;
}


/**
  Enable session tracker by parsing global value of tracked variables.

  @param thd    [IN]        The thd handle.

  @retval true  Error
  @retval false Success
*/

bool Session_sysvars_tracker::enable(THD *thd)
{
  orig_list.reinit();
  m_parsed= false;
  m_enabled= thd->variables.session_track_system_variables &&
             *thd->variables.session_track_system_variables;
  reset_changed();
  return false;
}


/**
  Once the value of the @@session_track_system_variables has been
  successfully updated, this function calls
  Session_sysvars_tracker::vars_list::copy updating the hash in orig_list
  which represents the system variables to be tracked.

  We are doing via tool list because there possible errors with memory
  in this case value will be unchanged.

  @note This function is called from the ON_UPDATE() function of the
        session_track_system_variables' sys_var class.

  @param thd    [IN]        The thd handle.

  @retval true  Error
  @retval false Success
*/

bool Session_sysvars_tracker::update(THD *thd, set_var *var)
{
  vars_list tool_list;
  void *copy;
  size_t length= 1;

  if (var->save_result.string_value.str)
    copy= my_memdup(PSI_INSTRUMENT_ME, var->save_result.string_value.str,
                    (length= var->save_result.string_value.length + 1),
                    MYF(MY_WME | MY_THREAD_SPECIFIC));
    else
      copy= my_strdup(PSI_INSTRUMENT_ME, "", MYF(MY_WME | MY_THREAD_SPECIFIC));

  if (!copy)
    return true;

  if (tool_list.parse_var_list(thd, var->save_result.string_value, true,
                               thd->charset()))
  {
    my_free(copy);
    return true;
  }

  my_free(thd->variables.session_track_system_variables);
  thd->variables.session_track_system_variables= static_cast<char*>(copy);

  m_parsed= true;
  orig_list.copy(&tool_list, thd);
  orig_list.construct_var_list(thd->variables.session_track_system_variables,
                               length);
  return false;
}


bool Session_sysvars_tracker::vars_list::store(THD *thd, String *buf)
{
  for (ulong i= 0; i < m_registered_sysvars.records; i++)
  {
    sysvar_node_st *node= at(i);

    if (!node->m_changed)
      continue;

    char val_buf[SHOW_VAR_FUNC_BUFF_SIZE];
    SHOW_VAR show;
    CHARSET_INFO *charset;
    size_t val_length, length;
    mysql_mutex_lock(&LOCK_plugin);
    if (!*node->test_load)
    {
      mysql_mutex_unlock(&LOCK_plugin);
      continue;
    }
    sys_var *svar= node->m_svar;
    bool is_plugin= svar->cast_pluginvar();
    if (!is_plugin)
      mysql_mutex_unlock(&LOCK_plugin);

    /* As its always system variable. */
    show.type= SHOW_SYS;
    show.name= svar->name.str;
    show.value= (char *) svar;

    const char *value= get_one_variable(thd, &show, OPT_SESSION, SHOW_SYS, NULL,
                                        &charset, val_buf, &val_length);
    if (is_plugin)
      mysql_mutex_unlock(&LOCK_plugin);

    length= net_length_size(svar->name.length) +
      svar->name.length +
      net_length_size(val_length) +
      val_length;

    compile_time_assert(SESSION_TRACK_SYSTEM_VARIABLES < 251);
    if (unlikely((1 + net_length_size(length) + length + buf->length() >=
                  MAX_PACKET_LENGTH) ||
                 buf->reserve(1 + net_length_size(length) + length,
                              EXTRA_ALLOC)))
      return true;


    /* Session state type (SESSION_TRACK_SYSTEM_VARIABLES) */
    buf->q_append((char)SESSION_TRACK_SYSTEM_VARIABLES);

    /* Length of the overall entity. */
    buf->q_net_store_length((ulonglong)length);

    /* System variable's name (length-encoded string). */
    buf->q_net_store_data((const uchar*)svar->name.str, svar->name.length);

    /* System variable's value (length-encoded string). */
    buf->q_net_store_data((const uchar*)value, val_length);
  }
  return false;
}


/**
  Store the data for changed system variables in the specified buffer.
  Once the data is stored, we reset the flags related to state-change
  (see reset()).

  @param thd [IN]           The thd handle.
  @paran buf [INOUT]        Buffer to store the information to.

  @retval true  Error
  @retval false Success
*/

bool Session_sysvars_tracker::store(THD *thd, String *buf)
{
  if (!orig_list.is_enabled())
    return false;

  if (orig_list.store(thd, buf))
    return true;

  orig_list.reset();

  return false;
}


/**
  Mark the system variable as changed.

  @param               [IN] pointer on a variable
*/

void Session_sysvars_tracker::mark_as_changed(THD *thd, const sys_var *var)
{
  sysvar_node_st *node;

  if (!is_enabled())
    return;

  if (!m_parsed)
  {
    DBUG_ASSERT(thd->variables.session_track_system_variables);
    LEX_STRING tmp= { thd->variables.session_track_system_variables,
                      strlen(thd->variables.session_track_system_variables) };
    if (orig_list.parse_var_list(thd, tmp, true, thd->charset()))
    {
      orig_list.reinit();
      return;
    }
    m_parsed= true;
  }

  /*
    Check if the specified system variable is being tracked, if so
    mark it as changed and also set the class's m_changed flag.
  */
  if (orig_list.is_enabled() && (node= orig_list.insert_or_search(var)))
  {
    node->m_changed= true;
    set_changed(thd);
  }
}


/**
  Supply key to a hash.

  @param entry  [IN]        A single entry.
  @param length [OUT]       Length of the key.
  @param not_used           Unused.

  @return Pointer to the key buffer.
*/

uchar *Session_sysvars_tracker::sysvars_get_key(const char *entry,
                                                size_t *length,
                                                my_bool not_used __attribute__((unused)))
{
  *length= sizeof(sys_var *);
  return (uchar *) &(((sysvar_node_st *) entry)->m_svar);
}


void Session_sysvars_tracker::vars_list::reset()
{
  for (ulong i= 0; i < m_registered_sysvars.records; i++)
    at(i)->m_changed= false;
}


bool sysvartrack_global_update(THD *thd, char *str, size_t len)
{
  LEX_STRING tmp= { str, len };
  Session_sysvars_tracker::vars_list dummy;
  if (!dummy.parse_var_list(thd, tmp, false, system_charset_info))
  {
    dummy.construct_var_list(str, len + 1);
    return false;
  }
  return true;
}


int session_tracker_init()
{
  DBUG_ASSERT(global_system_variables.session_track_system_variables);
  if (sysvartrack_validate_value(0,
        global_system_variables.session_track_system_variables,
        strlen(global_system_variables.session_track_system_variables)))
  {
    sql_print_error("The variable session_track_system_variables has "
                    "invalid values.");
    return 1;
  }
  return 0;
}


///////////////////////////////////////////////////////////////////////////////

/**
  Enable/disable the tracker based on @@session_track_schema's value.

  @param thd [IN]           The thd handle.

  @return
    false (always)
*/

bool Current_schema_tracker::update(THD *thd, set_var *)
{
  m_enabled= thd->variables.session_track_schema;
  return false;
}


/**
  Store the schema name as length-encoded string in the specified buffer.

  @param thd [IN]           The thd handle.
  @paran buf [INOUT]        Buffer to store the information to.

  @reval  false Success
  @retval true  Error
*/

bool Current_schema_tracker::store(THD *thd, String *buf)
{
  size_t db_length, length;

  /*
    Protocol made (by unknown reasons) redundant:
    It saves length of database name and name of database name +
    length of saved length of database length.
  */
  length= db_length= thd->db.length;
  length += net_length_size(length);

  compile_time_assert(SESSION_TRACK_SCHEMA < 251);
  compile_time_assert(NAME_LEN < 251);
  DBUG_ASSERT(length < 251);
  if (unlikely((1 + 1 + length + buf->length() >= MAX_PACKET_LENGTH) ||
               buf->reserve(1 + 1 + length, EXTRA_ALLOC)))
    return true;

  /* Session state type (SESSION_TRACK_SCHEMA) */
  buf->q_append((char)SESSION_TRACK_SCHEMA);

  /* Length of the overall entity. */
  buf->q_net_store_length(length);

  /* Length and current schema name */
  buf->q_net_store_data((const uchar *)thd->db.str, thd->db.length);

  return false;
}


///////////////////////////////////////////////////////////////////////////////

/**
  Enable/disable the tracker based on @@session_track_transaction_info.

  @param thd [IN]           The thd handle.

  @retval true if updating the tracking level failed
  @retval false otherwise
*/

bool Transaction_state_tracker::update(THD *thd, set_var *)
{
  if (thd->variables.session_track_transaction_info != TX_TRACK_NONE)
  {
    /*
      If we only just turned reporting on (rather than changing between
      state and characteristics reporting), start from a defined state.
    */
    if (!m_enabled)
    {
      tx_curr_state     =
      tx_reported_state = TX_EMPTY;
      tx_changed       |= TX_CHG_STATE;
      m_enabled= true;
    }
    if (thd->variables.session_track_transaction_info == TX_TRACK_CHISTICS)
      tx_changed       |= TX_CHG_CHISTICS;
    set_changed(thd);
  }
  else
    m_enabled= false;

  return false;
}


/**
  Store the transaction state (and, optionally, characteristics)
  as length-encoded string in the specified buffer.  Once the data
  is stored, we reset the flags related to state-change (see reset()).


  @param thd [IN]           The thd handle.
  @paran buf [INOUT]        Buffer to store the information to.

  @retval false Success
  @retval true  Error
*/

static LEX_CSTRING isol[]= {
  { STRING_WITH_LEN("READ UNCOMMITTED") },
  { STRING_WITH_LEN("READ COMMITTED") },
  { STRING_WITH_LEN("REPEATABLE READ") },
  { STRING_WITH_LEN("SERIALIZABLE") }
};

bool Transaction_state_tracker::store(THD *thd, String *buf)
{
  /* STATE */
  if (tx_changed & TX_CHG_STATE)
  {
    if (unlikely((11 + buf->length() >= MAX_PACKET_LENGTH) ||
                 buf->reserve(11, EXTRA_ALLOC)))
      return true;

    buf->q_append((char)SESSION_TRACK_TRANSACTION_STATE);

    buf->q_append((char)9); // whole packet length
    buf->q_append((char)8); // results length

    buf->q_append((char)((tx_curr_state & TX_EXPLICIT)        ? 'T' :
                         ((tx_curr_state & TX_IMPLICIT)       ? 'I' : '_')));
    buf->q_append((char)((tx_curr_state & TX_READ_UNSAFE)     ? 'r' : '_'));
    buf->q_append((char)(((tx_curr_state & TX_READ_TRX) ||
                          (tx_curr_state & TX_WITH_SNAPSHOT)) ? 'R' : '_'));
    buf->q_append((char)((tx_curr_state & TX_WRITE_UNSAFE)   ? 'w' : '_'));
    buf->q_append((char)((tx_curr_state & TX_WRITE_TRX)      ? 'W' : '_'));
    buf->q_append((char)((tx_curr_state & TX_STMT_UNSAFE)    ? 's' : '_'));
    buf->q_append((char)((tx_curr_state & TX_RESULT_SET)     ? 'S' : '_'));
    buf->q_append((char)((tx_curr_state & TX_LOCKED_TABLES)  ? 'L' : '_'));
  }

  /* CHARACTERISTICS -- How to restart the transaction */

  if ((thd->variables.session_track_transaction_info == TX_TRACK_CHISTICS) &&
      (tx_changed & TX_CHG_CHISTICS))
  {
    bool is_xa= thd->transaction->xid_state.is_explicit_XA();
    size_t start;

    /* 2 length by 1 byte and code */
    if (unlikely((1 + 1 + 1 + 110 + buf->length() >= MAX_PACKET_LENGTH) ||
                 buf->reserve(1 + 1 + 1, EXTRA_ALLOC)))
      return true;

    compile_time_assert(SESSION_TRACK_TRANSACTION_CHARACTERISTICS < 251);
    /* Session state type (SESSION_TRACK_TRANSACTION_CHARACTERISTICS) */
    buf->q_append((char)SESSION_TRACK_TRANSACTION_CHARACTERISTICS);

    /* placeholders for lengths. will be filled in at the end */
    buf->q_append('\0');
    buf->q_append('\0');

    start= buf->length();

    {
      /*
        We have four basic replay scenarios:

        a) SET TRANSACTION was used, but before an actual transaction
           was started, the load balancer moves the connection elsewhere.
           In that case, the same one-shots should be set up in the
           target session.  (read-only/read-write; isolation-level)

        b) The initial transaction has begun; the relevant characteristics
           are the session defaults, possibly overridden by previous
           SET TRANSACTION statements, possibly overridden or extended
           by options passed to the START TRANSACTION statement.
           If the load balancer wishes to move this transaction,
           it needs to be replayed with the correct characteristics.
           (read-only/read-write from SET or START;
           isolation-level from SET only, snapshot from START only)

        c) A subsequent transaction started with START TRANSACTION
           (which is legal syntax in lieu of COMMIT AND CHAIN in MySQL)
           may add/modify the current one-shots:

           - It may set up a read-only/read-write one-shot.
             This one-shot will override the value used in the previous
             transaction (whether that came from the default or a one-shot),
             and, like all one-shots currently do, it will carry over into
             any subsequent transactions that don't explicitly override them
             in turn. This behavior is not guaranteed in the docs and may
             change in the future, but the tracker item should correctly
             reflect whatever behavior a given version of mysqld implements.

           - It may also set up a WITH CONSISTENT SNAPSHOT one-shot.
             This one-shot does not currently carry over into subsequent
             transactions (meaning that with "traditional syntax", WITH
             CONSISTENT SNAPSHOT can only be requested for the first part
             of a transaction chain). Again, the tracker item should reflect
             mysqld behavior.

        d) A subsequent transaction started using COMMIT AND CHAIN
           (or, for that matter, BEGIN WORK, which is currently
           legal and equivalent syntax in MySQL, or START TRANSACTION
           sans options) will re-use any one-shots set up so far
           (with SET before the first transaction started, and with
           all subsequent STARTs), except for WITH CONSISTANT SNAPSHOT,
           which will never be chained and only applies when explicitly
           given.

        It bears noting that if we switch sessions in a follow-up
        transaction, SET TRANSACTION would be illegal in the old
        session (as a transaction is active), whereas in the target
        session which is being prepared, it should be legal, as no
        transaction (chain) should have started yet.

        Therefore, we are free to generate SET TRANSACTION as a replay
        statement even for a transaction that isn't the first in an
        ongoing chain. Consider

          SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED;
          START TRANSACTION READ ONLY, WITH CONSISTENT SNAPSHOT;
          # work
          COMMIT AND CHAIN;

        If we switch away at this point, the replay in the new session
        needs to be

          SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED;
          START TRANSACTION READ ONLY;

        When a transaction ends (COMMIT/ROLLBACK sans CHAIN), all
        per-transaction characteristics are reset to the session's
        defaults.

        This also holds for a transaction ended implicitly!  (transaction.cc)
        Once again, the aim is to have the tracker item reflect on a
        given mysqld's actual behavior.
      */

      /*
        "ISOLATION LEVEL"
        Only legal in SET TRANSACTION, so will always be replayed as such.
      */
      if (tx_isol_level != TX_ISOL_INHERIT)
      {
        /*
          Unfortunately, we can't re-use tx_isolation_names /
          tx_isolation_typelib as it hyphenates its items.
        */
        buf->append(STRING_WITH_LEN("SET TRANSACTION ISOLATION LEVEL "));
        buf->append(&isol[tx_isol_level - 1]);
        buf->append(STRING_WITH_LEN("; "));
      }

      /*
        Start transaction will usually result in TX_EXPLICIT (transaction
        started, but no data attached yet), except when WITH CONSISTENT
        SNAPSHOT, in which case we may have data pending.
        If it's an XA transaction, we don't go through here so we can
        first print the trx access mode ("SET TRANSACTION READ ...")
        separately before adding XA START (whereas with START TRANSACTION,
        we can merge the access mode into the same statement).
      */
      if ((tx_curr_state & TX_EXPLICIT) && !is_xa)
      {
        buf->append(STRING_WITH_LEN("START TRANSACTION"));

        /*
          "WITH CONSISTENT SNAPSHOT"
          Defaults to no, can only be enabled.
          Only appears in START TRANSACTION.
        */
        if (tx_curr_state & TX_WITH_SNAPSHOT)
        {
          buf->append(STRING_WITH_LEN(" WITH CONSISTENT SNAPSHOT"));
          if (tx_read_flags != TX_READ_INHERIT)
            buf->append(STRING_WITH_LEN(","));
        }

        /*
          "READ WRITE / READ ONLY" can be set globally, per-session,
          or just for one transaction.

          The latter case can take the form of
          START TRANSACTION READ (WRITE|ONLY), or of
          SET TRANSACTION READ (ONLY|WRITE).
          (Both set thd->read_only for the upcoming transaction;
          it will ultimately be re-set to the session default.)

          As the regular session-variable tracker does not monitor the one-shot,
          we'll have to do it here.

          If READ is flagged as set explicitly (rather than just inherited
          from the session's default), we'll get the actual bool from the THD.
        */
        if (tx_read_flags != TX_READ_INHERIT)
        {
          if (tx_read_flags == TX_READ_ONLY)
            buf->append(STRING_WITH_LEN(" READ ONLY"));
          else
            buf->append(STRING_WITH_LEN(" READ WRITE"));
        }
        buf->append(STRING_WITH_LEN("; "));
      }
      else if (tx_read_flags != TX_READ_INHERIT)
      {
        /*
          "READ ONLY" / "READ WRITE"
          We could transform this to SET TRANSACTION even when it occurs
          in START TRANSACTION, but for now, we'll resysynthesize the original
          command as closely as possible.
        */
        buf->append(STRING_WITH_LEN("SET TRANSACTION "));
        if (tx_read_flags == TX_READ_ONLY)
          buf->append(STRING_WITH_LEN("READ ONLY; "));
        else
          buf->append(STRING_WITH_LEN("READ WRITE; "));
      }

      if ((tx_curr_state & TX_EXPLICIT) && is_xa)
      {
        XID *xid= thd->transaction->xid_state.get_xid();
        long glen, blen;

        buf->append(STRING_WITH_LEN("XA START"));

        if ((glen= xid->gtrid_length) > 0)
        {
          buf->append(STRING_WITH_LEN(" '"));
          buf->append(xid->data, glen);

          if ((blen= xid->bqual_length) > 0)
          {
            buf->append(STRING_WITH_LEN("','"));
            buf->append(xid->data + glen, blen);
          }
          buf->append(STRING_WITH_LEN("'"));

          if (xid->formatID != 1)
          {
            buf->append(STRING_WITH_LEN(","));
            buf->append_ulonglong(xid->formatID);
          }
        }

        buf->append(STRING_WITH_LEN("; "));
      }

      // discard trailing space
      if (buf->length() > start)
        buf->length(buf->length() - 1);
    }

    {
      size_t length= buf->length() - start;
      uchar *place= (uchar *)(buf->ptr() + (start - 2));
      DBUG_ASSERT(length < 249); // in fact < 110
      DBUG_ASSERT(start >= 3);

      DBUG_ASSERT((place - 1)[0] == SESSION_TRACK_TRANSACTION_CHARACTERISTICS);
      /* Length of the overall entity. */
      place[0]= (uchar)length + 1;
      /* Transaction characteristics (length-encoded string). */
      place[1]= (uchar)length;
    }
  }

  tx_reported_state= tx_curr_state;
  tx_changed= TX_CHG_NONE;

  return false;
}


/**
  Helper function: turn table info into table access flag.
  Accepts table lock type and engine type flag (transactional/
  non-transactional), and returns the corresponding access flag
  out of TX_READ_TRX, TX_READ_UNSAFE, TX_WRITE_TRX, TX_WRITE_UNSAFE.

  @param thd [IN]           The thd handle
  @param set [IN]           The table's access/lock type
  @param set [IN]           Whether the table's engine is transactional

  @return                   The table access flag
*/

enum_tx_state Transaction_state_tracker::calc_trx_state(THD *thd,
                                                        thr_lock_type l,
                                                        bool has_trx)
{
  enum_tx_state      s;
  bool               read= (l <= TL_READ_NO_INSERT);

  if (read)
    s= has_trx ? TX_READ_TRX  : TX_READ_UNSAFE;
  else
    s= has_trx ? TX_WRITE_TRX : TX_WRITE_UNSAFE;

  return s;
}


/**
  Register the end of an (implicit or explicit) transaction.

  @param thd [IN]           The thd handle
*/
void Transaction_state_tracker::end_trx(THD *thd)
{
  DBUG_ASSERT(thd->variables.session_track_transaction_info > TX_TRACK_NONE);

  if ((!m_enabled) || (thd->state_flags & Open_tables_state::BACKUPS_AVAIL))
    return;

  if (tx_curr_state != TX_EMPTY)
  {
    if (tx_curr_state & TX_EXPLICIT)
      tx_changed  |= TX_CHG_CHISTICS;
    tx_curr_state &= TX_LOCKED_TABLES;
  }
  update_change_flags(thd);
}


/**
  Clear flags pertaining to the current statement or transaction.
  May be called repeatedly within the same execution cycle.

  @param thd [IN]           The thd handle.
  @param set [IN]           The flags to clear
*/

void Transaction_state_tracker::clear_trx_state(THD *thd, uint clear)
{
  if ((!m_enabled) || (thd->state_flags & Open_tables_state::BACKUPS_AVAIL))
    return;

  tx_curr_state &= ~clear;
  update_change_flags(thd);
}


/**
  Add flags pertaining to the current statement or transaction.
  May be called repeatedly within the same execution cycle,
  e.g. to add access info for more tables.

  @param thd [IN]           The thd handle.
  @param set [IN]           The flags to add
*/

void Transaction_state_tracker::add_trx_state(THD *thd, uint add)
{
  if ((!m_enabled) || (thd->state_flags & Open_tables_state::BACKUPS_AVAIL))
    return;

  if (add == TX_EXPLICIT)
  {
    /* Always send characteristic item (if tracked), always replace state. */
    tx_changed |= TX_CHG_CHISTICS;
    tx_curr_state = TX_EXPLICIT;
  }

  /*
    If we're not in an implicit or explicit transaction, but
    autocommit==0 and tables are accessed, we flag "implicit transaction."
  */
  else if (!(tx_curr_state & (TX_EXPLICIT|TX_IMPLICIT)) &&
           (thd->variables.option_bits & OPTION_NOT_AUTOCOMMIT) &&
           (add &
            (TX_READ_TRX | TX_READ_UNSAFE | TX_WRITE_TRX | TX_WRITE_UNSAFE)))
    tx_curr_state |= TX_IMPLICIT;

  /*
    Only flag state when in transaction or LOCK TABLES is added.
  */
  if ((tx_curr_state & (TX_EXPLICIT | TX_IMPLICIT)) ||
      (add & TX_LOCKED_TABLES))
    tx_curr_state |= add;

  update_change_flags(thd);
}


/**
  Add "unsafe statement" flag if applicable.

  @param thd [IN]           The thd handle.
  @param set [IN]           The flags to add
*/

void Transaction_state_tracker::add_trx_state_from_thd(THD *thd)
{
  if (m_enabled)
  {
    if (thd->lex->is_stmt_unsafe())
      add_trx_state(thd, TX_STMT_UNSAFE);
  }
}


/**
  Set read flags (read only/read write) pertaining to the next
  transaction.

  @param thd [IN]           The thd handle.
  @param set [IN]           The flags to set
*/

void Transaction_state_tracker::set_read_flags(THD *thd,
                                               enum enum_tx_read_flags flags)
{
  if (m_enabled && (tx_read_flags != flags))
  {
    tx_read_flags = flags;
    tx_changed   |= TX_CHG_CHISTICS;
    set_changed(thd);
  }
}


/**
  Set isolation level pertaining to the next transaction.

  @param thd [IN]           The thd handle.
  @param set [IN]           The isolation level to set
*/

void Transaction_state_tracker::set_isol_level(THD *thd,
                                               enum enum_tx_isol_level level)
{
  if (m_enabled && (tx_isol_level != level))
  {
    tx_isol_level = level;
    tx_changed   |= TX_CHG_CHISTICS;
    set_changed(thd);
  }
}


///////////////////////////////////////////////////////////////////////////////

/**
  @Enable/disable the tracker based on @@session_track_state_change value.

  @param thd [IN]           The thd handle.
  @return                   false (always)

**/

bool Session_state_change_tracker::update(THD *thd, set_var *)
{
  m_enabled= thd->variables.session_track_state_change;
  return false;
}

/**
  Store the '1' in the specified buffer when state is changed.

  @param thd [IN]           The thd handle.
  @paran buf [INOUT]        Buffer to store the information to.

  @reval  false Success
  @retval true  Error
**/

bool Session_state_change_tracker::store(THD *thd, String *buf)
{
  if (unlikely((1 + 1 + 1 + buf->length() >= MAX_PACKET_LENGTH) ||
               buf->reserve(1 + 1 + 1, EXTRA_ALLOC)))
    return true;

  compile_time_assert(SESSION_TRACK_STATE_CHANGE < 251);
  /* Session state type (SESSION_TRACK_STATE_CHANGE) */
  buf->q_append((char)SESSION_TRACK_STATE_CHANGE);

  /* Length of the overall entity (1 byte) */
  buf->q_append('\1');

  DBUG_ASSERT(is_changed());
  buf->q_append('1');

  return false;
}


bool User_variables_tracker::update(THD *thd, set_var *)
{
  m_enabled= thd->variables.session_track_user_variables;
  return false;
}


bool User_variables_tracker::store(THD *thd, String *buf)
{
  for (ulong i= 0; i < m_changed_user_variables.size(); i++)
  {
    const user_var_entry *var= m_changed_user_variables.at(i);
    String value_str;
    bool null_value;
    size_t length;

    var->val_str(&null_value, &value_str, DECIMAL_MAX_SCALE);
    length= net_length_size(var->name.length) + var->name.length;
    if (!null_value)
      length+= net_length_size(value_str.length()) + value_str.length();

    if (buf->reserve(sizeof(char) + length + net_length_size(length)))
      return true;

    buf->q_append(static_cast<char>(SESSION_TRACK_USER_VARIABLES));
    buf->q_net_store_length(length);
    buf->q_net_store_data(reinterpret_cast<const uchar*>(var->name.str),
                          var->name.length);
    if (!null_value)
      buf->q_net_store_data(reinterpret_cast<const uchar*>(value_str.ptr()),
                            value_str.length());
  }
  m_changed_user_variables.clear();
  return false;
}

///////////////////////////////////////////////////////////////////////////////

/**
  @brief Store all change information in the specified buffer.

  @param thd [IN]           The thd handle.
  @param buf [OUT]          Reference to the string buffer to which the state
                            change data needs to be written.
*/

void Session_tracker::store(THD *thd, String *buf)
{
  size_t start;

  /* track data ID fit into one byte in net coding */
  compile_time_assert(SESSION_TRACK_always_at_the_end < 251);
  /* one tracker could serv several tracking data */
  compile_time_assert((uint) SESSION_TRACK_always_at_the_end >=
                      (uint) SESSION_TRACKER_END);

  /*
    Probably most track result will fit in 251 byte so lets made it at
    least efficient. We allocate 1 byte for length and then will move
    string if there is more.
  */
  buf->append('\0');
  start= buf->length();

  /* Get total length. */
  for (int i= 0; i < SESSION_TRACKER_END; i++)
  {
    if (m_trackers[i]->is_changed())
    {
      if (m_trackers[i]->store(thd, buf))
      {
        // it is safer to have 0-length block in case of error
        buf->length(start);
        return;
      }
      m_trackers[i]->reset_changed();
    }
  }

  size_t length= buf->length() - start;
  uchar *data;
  uint size;

  if ((size= net_length_size(length)) != 1)
  {
    if (buf->reserve(size - 1, 0))
    {
      buf->length(start); // it is safer to have 0-length block in case of error
      return;
    }

    /*
      The 'buf->reserve()' can change the buf->ptr() so we cannot
      calculate the 'data' earlier.
    */
    buf->length(buf->length() + (size - 1));
    data= (uchar *)(buf->ptr() + start);
    memmove(data + (size - 1), data, length);
  }
  else
    data= (uchar *)(buf->ptr() + start);

  net_store_length(data - 1, length);
}
