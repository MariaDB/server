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


#include "session_tracker.h"

#include "hash.h"
#include "table.h"
#include "rpl_gtid.h"
#include "sql_class.h"
#include "sql_show.h"
#include "sql_plugin.h"

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
  m_trackers[SESSION_SYSVARS_TRACKER]=
    new (std::nothrow) Not_implemented_tracker;
  m_trackers[CURRENT_SCHEMA_TRACKER]=
    new (std::nothrow) Current_schema_tracker;
  m_trackers[SESSION_STATE_CHANGE_TRACKER]=
    new (std::nothrow) Session_state_change_tracker;
  m_trackers[SESSION_GTIDS_TRACKER]=
    new (std::nothrow) Not_implemented_tracker;
  m_trackers[TRANSACTION_INFO_TRACKER]=
    new (std::nothrow) Not_implemented_tracker;
}

/**
  @brief Enables the tracker objects.

  @param thd [IN]    The thread handle.

  @return            void
*/
void Session_tracker::enable(THD *thd)
{
  for (int i= 0; i <= SESSION_TRACKER_END; i ++)
    m_trackers[i]->enable(thd);
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
