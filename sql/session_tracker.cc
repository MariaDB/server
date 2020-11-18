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


#ifndef EMBEDDED_LIBRARY
#include "sql_plugin.h"
#include "session_tracker.h"

#include "hash.h"
#include "table.h"
#include "rpl_gtid.h"
#include "sql_class.h"
#include "sql_show.h"
#include "sql_plugin.h"
#include "set_var.h"

void State_tracker::mark_as_changed(THD *thd, LEX_CSTRING *tracked_item_name)
{
  m_changed= true;
  thd->lex->safe_to_cache_query= 0;
  thd->server_status|= SERVER_SESSION_STATE_CHANGED;
}


class Not_implemented_tracker : public State_tracker
{
public:
  bool enable(THD *thd)
  { return false; }
  bool update(THD *, set_var *)
  { return false; }
  bool store(THD *, String *)
  { return false; }
  void mark_as_changed(THD *, LEX_CSTRING *tracked_item_name)
  {}

};

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

    uchar* insert_or_search(sysvar_node_st *node, const sys_var *svar)
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

    bool insert(sysvar_node_st *node, const sys_var *svar, myf mem_flag);
    void reinit();
    void reset();
    inline bool is_enabled()
    {
      return track_all || m_registered_sysvars.records;
    }
    void copy(vars_list* from, THD *thd);
    bool parse_var_list(THD *thd, LEX_STRING var_list, bool throw_error,
                        CHARSET_INFO *char_set, bool take_mutex);
    bool construct_var_list(char *buf, size_t buf_len);
    bool store(THD *thd, String *buf);
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
  static bool server_init_check(THD *thd, CHARSET_INFO *char_set,
                                LEX_STRING var_list)
  {
    return check_var_list(thd, var_list, false, char_set, false);
  }

  static bool server_init_process(THD *thd, CHARSET_INFO *char_set,
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
  bool check_str(THD *thd, LEX_STRING *val);
  bool update(THD *thd, set_var *var);
  bool store(THD *thd, String *buf);
  void mark_as_changed(THD *thd, LEX_CSTRING *tracked_item_name);
  /* callback */
  static uchar *sysvars_get_key(const char *entry, size_t *length,
                                my_bool not_used __attribute__((unused)));

  // hash iterators
  static my_bool name_array_filler(void *ptr, void *data_ptr);
  static my_bool store_variable(void *ptr, void *data_ptr);
  static my_bool reset_variable(void *ptr, void *data_ptr);

  static bool check_var_list(THD *thd, LEX_STRING var_list, bool throw_error,
                             CHARSET_INFO *char_set, bool take_mutex);
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
  { return update(thd, NULL); }
  bool update(THD *thd, set_var *var);
  bool store(THD *thd, String *buf);
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
  { return update(thd, NULL); };
  bool update(THD *thd, set_var *var);
  bool store(THD *thd, String *buf);
  bool is_state_changed(THD*);
};


/* To be used in expanding the buffer. */
static const unsigned int EXTRA_ALLOC= 1024;


void Session_sysvars_tracker::vars_list::reinit()
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
  reinit();
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
      reinit();
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
      reinit();
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
  @param take_mutex      [IN]    take LOCK_plugin

  @return
    true                    Error
    false                   Success
*/
bool Session_sysvars_tracker::vars_list::parse_var_list(THD *thd,
                                                        LEX_STRING var_list,
                                                        bool throw_error,
							CHARSET_INFO *char_set,
							bool take_mutex)
{
  const char separator= ',';
  char *token, *lasts= NULL;
  size_t rest= var_list.length;
  reinit();

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
  if (!thd || take_mutex)
    mysql_mutex_lock(&LOCK_plugin);
  for (;;)
  {
    sys_var *svar;
    LEX_STRING var;
    uint not_used;

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
    trim_whitespace(char_set, &var, &not_used);

    if(!strcmp(var.str,(const char *)"*"))
    {
      track_all= true;
    }
    else if ((svar=
              find_sys_var_ex(thd, var.str, var.length, throw_error, true)))
    {
      if (insert(NULL, svar, m_mem_flag) == TRUE)
        goto error;
    }
    else if (throw_error && thd)
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
  if (!thd || take_mutex)
    mysql_mutex_unlock(&LOCK_plugin);

  return false;

error:
  if (!thd || take_mutex)
    mysql_mutex_unlock(&LOCK_plugin);
  return true;
}


bool Session_sysvars_tracker::check_var_list(THD *thd,
                                             LEX_STRING var_list,
                                             bool throw_error,
                                             CHARSET_INFO *char_set,
                                             bool take_mutex)
{
  const char separator= ',';
  char *token, *lasts= NULL;
  size_t rest= var_list.length;

  if (!var_list.str || var_list.length == 0 ||
      !strcmp(var_list.str,(const char *)"*"))
  {
    return false;
  }

  token= var_list.str;

  /*
    If Lock to the plugin mutex is not acquired here itself, it results
    in having to acquire it multiple times in find_sys_var_ex for each
    token value. Hence the mutex is handled here to avoid a performance
    overhead.
  */
  if (!thd || take_mutex)
    mysql_mutex_lock(&LOCK_plugin);
  for (;;)
  {
    LEX_STRING var;
    uint not_used;

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
    trim_whitespace(char_set, &var, &not_used);

    if(!strcmp(var.str,(const char *)"*") &&
       !find_sys_var_ex(thd, var.str, var.length, throw_error, true))
    {
      if (throw_error && take_mutex && thd)
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WRONG_VALUE_FOR_VAR,
                            "%.*s is not a valid system variable and will"
                            "be ignored.", (int)var.length, token);
      }
      else
      {
        if (!thd || take_mutex)
          mysql_mutex_unlock(&LOCK_plugin);
        return true;
      }
    }

    if (lasts)
      token= lasts + 1;
    else
      break;
  }
  if (!thd || take_mutex)
    mysql_mutex_unlock(&LOCK_plugin);

  return false;
}

struct name_array_filler_data
{
  LEX_CSTRING **names;
  uint idx;

};

/** Collects variable references into array */
my_bool Session_sysvars_tracker::name_array_filler(void *ptr,
                                                   void *data_ptr)
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
  mysql_mutex_lock(&LOCK_plugin);
  LEX_STRING tmp;
  tmp.str= global_system_variables.session_track_system_variables;
  tmp.length= safe_strlen(tmp.str);
  if (tool_list->parse_var_list(thd, tmp,
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

inline bool Session_sysvars_tracker::check_str(THD *thd, LEX_STRING *val)
{
  return Session_sysvars_tracker::check_var_list(thd, *val, true,
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

bool Session_sysvars_tracker::update(THD *thd, set_var *var)
{
  /*
    We are doing via tool list because there possible errors with memory
    in this case value will be unchanged.
  */
  tool_list->reinit();
  if (tool_list->parse_var_list(thd, var->save_result.string_value, true,
                                thd->charset(), true))
    return true;
  orig_list->copy(tool_list, thd);
  return false;
}


/*
  Function and structure to support storing variables from hash to the buffer.
*/

struct st_store_variable_param
{
  THD *thd;
  String *buf;
};

my_bool Session_sysvars_tracker::store_variable(void *ptr, void *data_ptr)
{
  Session_sysvars_tracker::sysvar_node_st *node=
    (Session_sysvars_tracker::sysvar_node_st *)ptr;
  if (node->m_changed)
  {
    THD *thd= ((st_store_variable_param *)data_ptr)->thd;
    String *buf= ((st_store_variable_param *)data_ptr)->buf;
    char val_buf[SHOW_VAR_FUNC_BUFF_SIZE];
    SHOW_VAR show;
    CHARSET_INFO *charset;
    size_t val_length, length;
    mysql_mutex_lock(&LOCK_plugin);
    if (!*node->test_load)
    {
      mysql_mutex_unlock(&LOCK_plugin);
      return false;
    }
    sys_var *svar= node->m_svar;
    bool is_plugin= svar->cast_pluginvar();
    if (!is_plugin)
      mysql_mutex_unlock(&LOCK_plugin);

    /* As its always system variable. */
    show.type= SHOW_SYS;
    show.name= svar->name.str;
    show.value= (char *) svar;

    mysql_mutex_lock(&LOCK_global_system_variables);
    const char *value= get_one_variable(thd, &show, OPT_SESSION, SHOW_SYS, NULL,
                                        &charset, val_buf, &val_length);
    mysql_mutex_unlock(&LOCK_global_system_variables);

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

bool Session_sysvars_tracker::vars_list::store(THD *thd, String *buf)
{
  st_store_variable_param data= {thd, buf};
  return my_hash_iterate(&m_registered_sysvars, &store_variable, &data);
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
  if (!orig_list->is_enabled())
    return false;

  if (orig_list->store(thd, buf))
    return true;

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
  if (orig_list->is_enabled() &&
      (node= (sysvar_node_st *) (orig_list->insert_or_search(node, svar))))
  {
    node->m_changed= true;
    State_tracker::mark_as_changed(thd, var);
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


/* Function to support resetting hash nodes for the variables */

my_bool Session_sysvars_tracker::reset_variable(void *ptr,
                                                   void *data_ptr)
{
  ((Session_sysvars_tracker::sysvar_node_st *)ptr)->m_changed= false;
  return false;
}

void Session_sysvars_tracker::vars_list::reset()
{
  my_hash_iterate(&m_registered_sysvars, &reset_variable, NULL);
}

/**
  Prepare/reset the m_registered_sysvars hash for next statement.
*/

void Session_sysvars_tracker::reset()
{

  orig_list->reset();
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
bool sysvartrack_update(THD *thd, set_var *var)
{
  return sysvar_tracker(thd)->update(thd, var);
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
  length= db_length= thd->db_length;
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
  buf->q_net_store_data((const uchar *)thd->db, thd->db_length);

  reset();

  return false;
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


Transaction_state_tracker::Transaction_state_tracker()
{
  m_enabled        = false;
  tx_changed       = TX_CHG_NONE;
  tx_curr_state    =
  tx_reported_state= TX_EMPTY;
  tx_read_flags    = TX_READ_INHERIT;
  tx_isol_level    = TX_ISOL_INHERIT;
}

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
    mark_as_changed(thd, NULL);
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
    bool is_xa= (thd->transaction.xid_state.xa_state != XA_NOTR);
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

          SET TRANSACTION ISOLATION LEVEL READ UNCOMMITED;
          START TRANSACTION READ ONLY, WITH CONSISTENT SNAPSHOT;
          # work
          COMMIT AND CHAIN;

        If we switch away at this point, the replay in the new session
        needs to be

          SET TRANSACTION ISOLATION LEVEL READ UNCOMMITED;
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
        buf->append(isol[tx_isol_level - 1].str, isol[tx_isol_level - 1].length);
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
        XID *xid= &thd->transaction.xid_state.xid;
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

  reset();

  return false;
}


/**
  Reset the m_changed flag for next statement.
*/

void Transaction_state_tracker::reset()
{
  m_changed=  false;
  tx_reported_state=  tx_curr_state;
  tx_changed=  TX_CHG_NONE;
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
    mark_as_changed(thd, NULL);
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
    mark_as_changed(thd, NULL);
  }
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

  DBUG_ASSERT(is_state_changed(thd));
  buf->q_append('1');

  reset();

  return false;
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
  /* track data ID fit into one byte in net coding */
  compile_time_assert(SESSION_TRACK_always_at_the_end < 251);
  /* one tracker could serv several tracking data */
  compile_time_assert((uint)SESSION_TRACK_always_at_the_end >=
                      (uint)SESSION_TRACKER_END);

  for (int i= 0; i < SESSION_TRACKER_END; i++)
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
    new (std::nothrow) Transaction_state_tracker;

  for (int i= 0; i < SESSION_TRACKER_END; i++)
    m_trackers[i]->enable(thd);
}


/**
  Method called during the server startup to verify the contents
  of @@session_track_system_variables.

  @retval false Success
  @retval true  Failure
*/

bool Session_tracker::server_boot_verify(CHARSET_INFO *char_set)
{
  bool result;
  LEX_STRING tmp;
  tmp.str= global_system_variables.session_track_system_variables;
  tmp.length= safe_strlen(tmp.str);
  result=
    Session_sysvars_tracker::server_init_check(NULL, char_set, tmp);
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
  size_t start;

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
    if (m_trackers[i]->is_changed() &&
        m_trackers[i]->store(thd, buf))
    {
      buf->length(start); // it is safer to have 0-length block in case of error
      return;
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

#endif //EMBEDDED_LIBRARY
