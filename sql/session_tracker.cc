/* Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2016, MariaDB

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
#include "session_tracker.h"

#include "hash.h"
#include "table.h"
#include "rpl_gtid.h"
#include "sql_class.h"
#include "sql_show.h"
#include "sql_plugin.h"
#include "set_var.h"

class Not_implemented_tracker : public State_tracker
{
public:
  bool enable(THD *thd)
  { return false; }
  bool check(THD *, set_var *)
  { return false; }
  bool update(THD *)
  { return false; }
  bool store(THD *, String *)
  { return false; }
  void mark_as_changed(THD *, LEX_CSTRING *tracked_item_name)
  {}

};

static my_bool name_array_filler(void *ptr, void *data_ptr);
/**
  Session_sysvars_tracker

  This is a tracker class that enables & manages the tracking of session
  system variables. It internally maintains a hash of user supplied variable
  references and a boolean field to store if the variable was changed by the
  last statement.
*/

class Session_sysvars_tracker : public State_tracker
{
private:

  struct sysvar_node_st {
    sys_var *m_svar;
    bool *test_load;
    bool m_changed;
  };

  class vars_list
  {
  private:
    /**
      Registered system variables. (@@session_track_system_variables)
      A hash to store the name of all the system variables specified by the
      user.
    */
    HASH m_registered_sysvars;
    /** Size of buffer for string representation */
    size_t buffer_length;
    myf m_mem_flag;
    /**
      If TRUE then we want to check all session variable.
    */
    bool track_all;
    void init()
    {
      my_hash_init(&m_registered_sysvars,
                   &my_charset_bin,
		   4, 0, 0, (my_hash_get_key) sysvars_get_key,
		   my_free, MYF(HASH_UNIQUE |
                                ((m_mem_flag & MY_THREAD_SPECIFIC) ?
                                 HASH_THREAD_SPECIFIC : 0)));
    }
    void free_hash()
    {
      if (my_hash_inited(&m_registered_sysvars))
      {
	my_hash_free(&m_registered_sysvars);
      }
    }

    uchar* search(const sys_var *svar)
    {
      return (my_hash_search(&m_registered_sysvars, (const uchar *)&svar,
			     sizeof(sys_var *)));
    }

  public:
    vars_list() :
      buffer_length(0)
    {
      m_mem_flag= current_thd ? MY_THREAD_SPECIFIC : 0;
      init();
    }

    size_t get_buffer_length()
    {
      DBUG_ASSERT(buffer_length != 0); // asked earlier then should
      return buffer_length;
    }
    ~vars_list()
    {
      /* free the allocated hash. */
      if (my_hash_inited(&m_registered_sysvars))
      {
	my_hash_free(&m_registered_sysvars);
      }
    }

    uchar* search(sysvar_node_st *node, const sys_var *svar)
    {
      uchar *res;
      res= search(svar);
      if (!res)
      {
	if (track_all)
	{
	  insert(node, svar, m_mem_flag);
	  return search(svar);
	}
      }
      return res;
    }

    uchar* operator[](ulong idx)
    {
      return my_hash_element(&m_registered_sysvars, idx);
    }
    bool insert(sysvar_node_st *node, const sys_var *svar, myf mem_flag);
    void reset();
    void copy(vars_list* from, THD *thd);
    bool parse_var_list(THD *thd, LEX_STRING var_list, bool throw_error,
                        const CHARSET_INFO *char_set, bool session_created);
    bool construct_var_list(char *buf, size_t buf_len);
  };
  /**
    Two objects of vars_list type are maintained to manage
    various operations.
  */
  vars_list *orig_list, *tool_list;

public:
  Session_sysvars_tracker()
  {
    orig_list= new (std::nothrow) vars_list();
    tool_list= new (std::nothrow) vars_list();
  }

  ~Session_sysvars_tracker()
  {
    if (orig_list)
      delete orig_list;
    if (tool_list)
      delete tool_list;
  }

  size_t get_buffer_length()
  {
    return orig_list->get_buffer_length();
  }
  bool construct_var_list(char *buf, size_t buf_len)
  {
    return orig_list->construct_var_list(buf, buf_len);
  }

  /**
    Method used to check the validity of string provided
    for session_track_system_variables during the server
    startup.
  */
  static bool server_init_check(THD *thd, const CHARSET_INFO *char_set,
                                LEX_STRING var_list)
  {
    vars_list dummy;
    bool result;
    result= dummy.parse_var_list(thd, var_list, false, char_set, false);
    return result;
  }
  static bool server_init_process(THD *thd, const CHARSET_INFO *char_set,
                                  LEX_STRING var_list)
  {
    vars_list dummy;
    bool result;
    result= dummy.parse_var_list(thd, var_list, false, char_set, false);
    if (!result)
      dummy.construct_var_list(var_list.str, var_list.length + 1);
    return result;
  }

  void reset();
  bool enable(THD *thd);
  bool check(THD *thd, set_var *var);
  bool check_str(THD *thd, LEX_STRING val);
  bool update(THD *thd);
  bool store(THD *thd, String *buf);
  void mark_as_changed(THD *thd, LEX_CSTRING *tracked_item_name);
  /* callback */
  static uchar *sysvars_get_key(const char *entry, size_t *length,
                                my_bool not_used __attribute__((unused)));

  friend my_bool name_array_filler(void *ptr, void *data_ptr);
};



/**
  Current_schema_tracker,

  This is a tracker class that enables & manages the tracking of current
  schema for a particular connection.
*/

class Current_schema_tracker : public State_tracker
{
private:
  bool schema_track_inited;
  void reset();

public:

  Current_schema_tracker()
  {
    schema_track_inited= false;
  }

  bool enable(THD *thd)
  { return update(thd); }
  bool check(THD *thd, set_var *var)
  { return false; }
  bool update(THD *thd);
  bool store(THD *thd, String *buf);
  void mark_as_changed(THD *thd, LEX_CSTRING *tracked_item_name);
};

/*
  Session_state_change_tracker

  This is a boolean tracker class that will monitor any change that contributes
  to a session state change.
  Attributes that contribute to session state change include:
     - Successful change to System variables
     - User defined variables assignments
     - temporary tables created, altered or deleted
     - prepared statements added or removed
     - change in current database
     - change of current role
*/

class Session_state_change_tracker : public State_tracker
{
private:

  void reset();

public:
  Session_state_change_tracker();
  bool enable(THD *thd)
  { return update(thd); };
  bool check(THD *thd, set_var *var)
  { return false; }
  bool update(THD *thd);
  bool store(THD *thd, String *buf);
  void mark_as_changed(THD *thd, LEX_CSTRING *tracked_item_name);
  bool is_state_changed(THD*);
  void ensure_enabled(THD *thd)
  {}
};


/* To be used in expanding the buffer. */
static const unsigned int EXTRA_ALLOC= 1024;


void Session_sysvars_tracker::vars_list::reset()
{
  buffer_length= 0;
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
  reset();
  track_all= from->track_all;
  free_hash();
  buffer_length= from->buffer_length;
  m_registered_sysvars= from->m_registered_sysvars;
  from->init();
}

/**
  Inserts the variable to be tracked into m_registered_sysvars hash.

  @param   node   Node to be inserted.
  @param   svar   address of the system variable

  @retval false success
  @retval true  error
*/

bool Session_sysvars_tracker::vars_list::insert(sysvar_node_st *node,
                                                const sys_var *svar,
                                                myf mem_flag)
{
  if (!node)
  {
    if (!(node= (sysvar_node_st *) my_malloc(sizeof(sysvar_node_st),
                                             MYF(MY_WME | mem_flag))))
    {
      reset();
      return true;
    }
  }

  node->m_svar= (sys_var *)svar;
  node->test_load= node->m_svar->test_load;
  node->m_changed= false;
  if (my_hash_insert(&m_registered_sysvars, (uchar *) node))
  {
    my_free(node);
    if (!search((sys_var *)svar))
    {
      //EOF (error is already reported)
      reset();
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
  @param session_created [IN]    bool variable which says if the parse is
                                 already executed once. The mutex on variables
				 is not acquired if this variable is false.

  @return
    true                    Error
    false                   Success
*/
bool Session_sysvars_tracker::vars_list::parse_var_list(THD *thd,
                                                        LEX_STRING var_list,
                                                        bool throw_error,
							const CHARSET_INFO *char_set,
							bool session_created)
{
  const char separator= ',';
  char *token, *lasts= NULL;
  size_t rest= var_list.length;

  if (!var_list.str || var_list.length == 0)
  {
    buffer_length= 1;
    return false;
  }

  if(!strcmp(var_list.str,(const char *)"*"))
  {
    track_all= true;
    buffer_length= 2;
    return false;
  }

  buffer_length= var_list.length + 1;
  token= var_list.str;

  track_all= false;
  /*
    If Lock to the plugin mutex is not acquired here itself, it results
    in having to acquire it multiple times in find_sys_var_ex for each
    token value. Hence the mutex is handled here to avoid a performance
    overhead.
  */
  if (!thd || session_created)
    mysql_mutex_lock(&LOCK_plugin);
  for (;;)
  {
    sys_var *svar;
    LEX_STRING var;

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

    if ((svar= find_sys_var_ex(thd, var.str, var.length, throw_error, true)))
    {
      if (insert(NULL, svar, m_mem_flag) == TRUE)
        goto error;
    }
    else if (throw_error && session_created && thd)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WRONG_VALUE_FOR_VAR,
                          "%.*s is not a valid system variable and will"
                          "be ignored.", (int)var.length, token);
    }
    else
      goto error;

    if (lasts)
      token= lasts + 1;
    else
      break;
  }
  if (!thd || session_created)
    mysql_mutex_unlock(&LOCK_plugin);

  return false;

error:
  if (!thd || session_created)
    mysql_mutex_unlock(&LOCK_plugin);
  return true;
}

struct name_array_filler_data
{
  LEX_CSTRING **names;
  uint idx;

};

/** Collects variable references into array */
static my_bool name_array_filler(void *ptr, void *data_ptr)
{
  Session_sysvars_tracker::sysvar_node_st *node=
    (Session_sysvars_tracker::sysvar_node_st *)ptr;
  name_array_filler_data *data= (struct name_array_filler_data *)data_ptr;
  if (*node->test_load)
    data->names[data->idx++]= &node->m_svar->name;
  return FALSE;
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
  struct name_array_filler_data data;
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

  data.names= (LEX_CSTRING**)my_safe_alloca(names_size);

  if (unlikely(!data.names))
    return true;

  data.idx= 0;

  mysql_mutex_lock(&LOCK_plugin);
  my_hash_iterate(&m_registered_sysvars, &name_array_filler, &data);
  DBUG_ASSERT(data.idx <= m_registered_sysvars.records);


  if (m_registered_sysvars.records == 0)
  {
    mysql_mutex_unlock(&LOCK_plugin);
    buf[0]= '\0';
    return false;
  }

  my_qsort(data.names, data.idx, sizeof(LEX_CSTRING *),
           &name_array_sorter);

  for(uint i= 0; i < data.idx; i++)
  {
    LEX_CSTRING *nm= data.names[i];
    size_t ln= nm->length + 1;
    if (ln > left)
    {
      mysql_mutex_unlock(&LOCK_plugin);
      my_safe_afree(data.names, names_size);
      return true;
    }
    memcpy(buf, nm->str, nm->length);
    buf[nm->length]= separator;
    buf+= ln;
    left-= ln;
  }
  mysql_mutex_unlock(&LOCK_plugin);

  buf--; buf[0]= '\0';
  my_safe_afree(data.names, names_size);

  return false;
}

/**
  Enable session tracker by parsing global value of tracked variables.

  @param thd    [IN]        The thd handle.

  @retval true  Error
  @retval false Success
*/

bool Session_sysvars_tracker::enable(THD *thd)
{
  sys_var *svar;

  mysql_mutex_lock(&LOCK_plugin);
  svar= find_sys_var_ex(thd, SESSION_TRACK_SYSTEM_VARIABLES_NAME.str,
                        SESSION_TRACK_SYSTEM_VARIABLES_NAME.length,
                        false, true);
  DBUG_ASSERT(svar);

  set_var tmp(thd, SHOW_OPT_GLOBAL, svar, &null_lex_str, NULL);
  svar->session_save_default(thd, &tmp);

  if (tool_list->parse_var_list(thd, tmp.save_result.string_value,
                                true, thd->charset(), false) == true)
  {
    mysql_mutex_unlock(&LOCK_plugin);
    return true;
  }
  mysql_mutex_unlock(&LOCK_plugin);
  orig_list->copy(tool_list, thd);
  m_enabled= true;

  return false;
}


/**
  Check system variable name(s).

  @note This function is called from the ON_CHECK() function of the
        session_track_system_variables' sys_var class.

  @param thd    [IN]        The thd handle.
  @param var    [IN]        A pointer to set_var holding the specified list of
                            system variable names.

  @retval true  Error
  @retval false Success
*/

inline bool Session_sysvars_tracker::check(THD *thd, set_var *var)
{
  return check_str(thd, var->save_result.string_value);
}

inline bool Session_sysvars_tracker::check_str(THD *thd, LEX_STRING val)
{
  tool_list->reset();
  return tool_list->parse_var_list(thd, val, true,
                                   thd->charset(), true);
}


/**
  Once the value of the @@session_track_system_variables has been
  successfully updated, this function calls
  Session_sysvars_tracker::vars_list::copy updating the hash in orig_list
  which represents the system variables to be tracked.

  @note This function is called from the ON_UPDATE() function of the
        session_track_system_variables' sys_var class.

  @param thd    [IN]        The thd handle.

  @retval true  Error
  @retval false Success
*/

bool Session_sysvars_tracker::update(THD *thd)
{
  orig_list->copy(tool_list, thd);
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
  char val_buf[SHOW_VAR_FUNC_BUFF_SIZE];
  SHOW_VAR show;
  const char *value;
  sysvar_node_st *node;
  const CHARSET_INFO *charset;
  size_t val_length, length;
  int idx= 0;

  /* As its always system variable. */
  show.type= SHOW_SYS;

  while ((node= (sysvar_node_st *) (*orig_list)[idx]))
  {
    if (node->m_changed)
    {
      mysql_mutex_lock(&LOCK_plugin);
      if (!*node->test_load)
      {
        mysql_mutex_unlock(&LOCK_plugin);
        continue;
      }
      sys_var *svar= node->m_svar;
      show.name= svar->name.str;
      show.value= (char *) svar;

      value= get_one_variable(thd, &show, OPT_SESSION, SHOW_SYS, NULL,
                              &charset, val_buf, &val_length);
      mysql_mutex_unlock(&LOCK_plugin);

      length= net_length_size(svar->name.length) +
              svar->name.length +
              net_length_size(val_length) +
              val_length;

      compile_time_assert(SESSION_TRACK_SYSTEM_VARIABLES < 251);
      buf->prep_alloc(1 + net_length_size(length) + length, EXTRA_ALLOC);

      /* Session state type (SESSION_TRACK_SYSTEM_VARIABLES) */
      buf->q_net_store_length((ulonglong)SESSION_TRACK_SYSTEM_VARIABLES);

      /* Length of the overall entity. */
      buf->q_net_store_length((ulonglong)length);

      /* System variable's name (length-encoded string). */
      buf->q_net_store_data((const uchar*)svar->name.str, svar->name.length);

      /* System variable's value (length-encoded string). */
      buf->q_net_store_data((const uchar*)value, val_length);
    }
    ++ idx;
  }

  reset();

  return false;
}


/**
  Mark the system variable as changed.

  @param               [IN] pointer on a variable
*/

void Session_sysvars_tracker::mark_as_changed(THD *thd,
                                              LEX_CSTRING *var)
{
  sysvar_node_st *node= NULL;
  sys_var *svar= (sys_var *)var;
  /*
    Check if the specified system variable is being tracked, if so
    mark it as changed and also set the class's m_changed flag.
  */
  if ((node= (sysvar_node_st *) (orig_list->search(node, svar))))
  {
    node->m_changed= true;
    m_changed= true;
    /* do not cache the statement when there is change in session state */
    thd->lex->safe_to_cache_query= 0;
    thd->server_status|= SERVER_SESSION_STATE_CHANGED;
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


/**
  Prepare/reset the m_registered_sysvars hash for next statement.
*/

void Session_sysvars_tracker::reset()
{
  sysvar_node_st *node;
  int idx= 0;

  while ((node= (sysvar_node_st *) (*orig_list)[idx]))
  {
    node->m_changed= false;
    ++ idx;
  }
  m_changed= false;
}

static Session_sysvars_tracker* sysvar_tracker(THD *thd)
{
  return (Session_sysvars_tracker*)
    thd->session_tracker.get_tracker(SESSION_SYSVARS_TRACKER);
}

bool sysvartrack_validate_value(THD *thd, const char *str, size_t len)
{
  LEX_STRING tmp= {(char *)str, len};
  if (thd && sysvar_tracker(thd)->is_enabled())
    return sysvar_tracker(thd)->check_str(thd, tmp);
  return Session_sysvars_tracker::server_init_check(thd, system_charset_info,
                                                    tmp);
}
bool sysvartrack_reprint_value(THD *thd, char *str, size_t len)
{
  LEX_STRING tmp= {str, len};
  return Session_sysvars_tracker::server_init_process(thd,
                                                       system_charset_info,
                                                       tmp);
}
bool sysvartrack_update(THD *thd)
{
  return sysvar_tracker(thd)->update(thd);
}
size_t sysvartrack_value_len(THD *thd)
{
  return sysvar_tracker(thd)->get_buffer_length();
}
bool sysvartrack_value_construct(THD *thd, char *val, size_t len)
{
  return sysvar_tracker(thd)->construct_var_list(val, len);
}

///////////////////////////////////////////////////////////////////////////////

/**
  Enable/disable the tracker based on @@session_track_schema's value.

  @param thd [IN]           The thd handle.

  @return
    false (always)
*/

bool Current_schema_tracker::update(THD *thd)
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
  ulonglong db_length, length;

  /*
    Protocol made (by unknown reasons) redundant:
    It saves length of database name and name of database name +
    length of saved length of database length.
  */
  length= db_length= thd->db_length;
  length += net_length_size(length);

  compile_time_assert(SESSION_TRACK_SCHEMA < 251);
  compile_time_assert(NAME_LEN < 251);
  DBUG_ASSERT(net_length_size(length) < 251);
  if (buf->prep_alloc(1 + 1 + length, EXTRA_ALLOC))
    return true;

  /* Session state type (SESSION_TRACK_SCHEMA) */
  buf->q_net_store_length((ulonglong)SESSION_TRACK_SCHEMA);

  /* Length of the overall entity. */
  buf->q_net_store_length(length);

  /* Length and current schema name */
  buf->q_net_store_data((const uchar *)thd->db, thd->db_length);

  reset();

  return false;
}


/**
  Mark the tracker as changed.
*/

void Current_schema_tracker::mark_as_changed(THD *thd, LEX_CSTRING *)
{
  m_changed= true;
  thd->lex->safe_to_cache_query= 0;
  thd->server_status|= SERVER_SESSION_STATE_CHANGED;
}


/**
  Reset the m_changed flag for next statement.

  @return                   void
*/

void Current_schema_tracker::reset()
{
  m_changed= false;
}


///////////////////////////////////////////////////////////////////////////////

Session_state_change_tracker::Session_state_change_tracker()
{
  m_changed= false;
}

/**
  @Enable/disable the tracker based on @@session_track_state_change value.

  @param thd [IN]           The thd handle.
  @return                   false (always)

**/

bool Session_state_change_tracker::update(THD *thd)
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
  if (buf->prep_alloc(1 + 1 + 1, EXTRA_ALLOC))
    return true;

  compile_time_assert(SESSION_TRACK_STATE_CHANGE < 251);
  /* Session state type (SESSION_TRACK_STATE_CHANGE) */
  buf->q_net_store_length((ulonglong)SESSION_TRACK_STATE_CHANGE);

  /* Length of the overall entity (1 byte) */
  buf->q_append('\1');

  DBUG_ASSERT(is_state_changed(thd));
  buf->q_append('1');

  reset();

  return false;
}

/**
  Mark the tracker as changed and associated session
  attributes accordingly.
*/

void Session_state_change_tracker::mark_as_changed(THD *thd, LEX_CSTRING *)
{
  m_changed= true;
  thd->lex->safe_to_cache_query= 0;
  thd->server_status|= SERVER_SESSION_STATE_CHANGED;
}

/**
  Reset the m_changed flag for next statement.
*/

void Session_state_change_tracker::reset()
{
  m_changed= false;
}

/**
  Find if there is a session state change.
*/

bool Session_state_change_tracker::is_state_changed(THD *)
{
  return m_changed;
}

///////////////////////////////////////////////////////////////////////////////

/**
  @brief Initialize session tracker objects.
*/

Session_tracker::Session_tracker()
{
  for (int i= 0; i <= SESSION_TRACKER_END; i ++)
    m_trackers[i]= NULL;
}


/**
  @brief Enables the tracker objects.

  @param thd [IN]    The thread handle.

  @return            void
*/

void Session_tracker::enable(THD *thd)
{
  /*
    Originally and correctly this allocation was in the constructor and
    deallocation in the destructor, but in this case memory counting
    system works incorrectly (for example in INSERT DELAYED thread)
  */
  deinit();
  m_trackers[SESSION_SYSVARS_TRACKER]=
    new (std::nothrow) Session_sysvars_tracker();
  m_trackers[CURRENT_SCHEMA_TRACKER]=
    new (std::nothrow) Current_schema_tracker;
  m_trackers[SESSION_STATE_CHANGE_TRACKER]=
    new (std::nothrow) Session_state_change_tracker;
  m_trackers[SESSION_GTIDS_TRACKER]=
    new (std::nothrow) Not_implemented_tracker;
  m_trackers[TRANSACTION_INFO_TRACKER]=
    new (std::nothrow) Not_implemented_tracker;

  for (int i= 0; i <= SESSION_TRACKER_END; i ++)
    m_trackers[i]->enable(thd);
}


/**
  Method called during the server startup to verify the contents
  of @@session_track_system_variables.

  @retval false Success
  @retval true  Failure
*/

bool Session_tracker::server_boot_verify(const CHARSET_INFO *char_set)
{
  Session_sysvars_tracker *server_tracker;
  bool result;
  sys_var *svar= find_sys_var_ex(NULL, SESSION_TRACK_SYSTEM_VARIABLES_NAME.str,
                                 SESSION_TRACK_SYSTEM_VARIABLES_NAME.length,
                                 false, true);
  DBUG_ASSERT(svar);
  set_var tmp(NULL, SHOW_OPT_GLOBAL, svar, &null_lex_str, NULL);
  svar->session_save_default(NULL, &tmp);
  server_tracker= new (std::nothrow) Session_sysvars_tracker();
  result= server_tracker->server_init_check(NULL, char_set,
                                            tmp.save_result.string_value);
  delete server_tracker;
  return result;
}


/**
  @brief Store all change information in the specified buffer.

  @param thd [IN]           The thd handle.
  @param buf [OUT]          Reference to the string buffer to which the state
                            change data needs to be written.
*/

void Session_tracker::store(THD *thd, String *buf)
{
  /* Temporary buffer to store all the changes. */
  size_t start;

  /*
    Probably most track result will fit in 251 byte so lets made it at
    least efficient. We allocate 1 byte for length and then will move
    string if there is more.
  */
  buf->append('\0');
  start= buf->length();

  /* Get total length. */
  for (int i= 0; i <= SESSION_TRACKER_END; i ++)
  {
    if (m_trackers[i]->is_changed() &&
        m_trackers[i]->store(thd, buf))
    {
      buf->length(start); // it is safer to have 0-length block in case of error
      return;
    }
  }

  size_t length= buf->length() - start;
  uchar *data= (uchar *)(buf->ptr() + start);
  uint size;

  if ((size= net_length_size(length)) != 1)
  {
    if (buf->prep_alloc(size - 1, EXTRA_ALLOC))
    {
      buf->length(start); // it is safer to have 0-length block in case of error
      return;
    }
    memmove(data + (size - 1), data, length);
  }

  net_store_length(data - 1, length);
}
