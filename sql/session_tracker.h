#ifndef SESSION_TRACKER_INCLUDED
#define SESSION_TRACKER_INCLUDED

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

#include "m_string.h"
#include "thr_lock.h"

/* forward declarations */
class THD;
class set_var;
class String;


enum enum_session_tracker
{
  SESSION_SYSVARS_TRACKER,                       /* Session system variables */
  CURRENT_SCHEMA_TRACKER,                        /* Current schema */
  SESSION_STATE_CHANGE_TRACKER,
  SESSION_GTIDS_TRACKER,                         /* Tracks GTIDs */
  TRANSACTION_INFO_TRACKER                       /* Transaction state */
};

#define SESSION_TRACKER_END TRANSACTION_INFO_TRACKER


/**
  State_tracker

  An abstract class that defines the interface for any of the server's
  'session state change tracker'. A tracker, however, is a sub- class of
  this class which takes care of tracking the change in value of a part-
  icular session state type and thus defines various methods listed in this
  interface. The change information is later serialized and transmitted to
  the client through protocol's OK packet.

  Tracker system variables :-
  A tracker is normally mapped to a system variable. So in order to enable,
  disable or modify the sub-entities of a tracker, the user needs to modify
  the respective system variable either through SET command or via command
  line option. As required in system variable handling, this interface also
  includes two functions to help in the verification of the supplied value
  (ON_CHECK) and the updation (ON_UPDATE) of the tracker system variable,
  namely - check() and update().
*/

class State_tracker
{
protected:
  /**
    Is tracking enabled for a particular session state type ?

    @note: It is cache to avoid virtual functions and checking thd
    when we want mark tracker as changed.
  */
  bool m_enabled;

  /** Has the session state type changed ? */
  bool m_changed;

public:
  /** Constructor */
  State_tracker() : m_enabled(false), m_changed(false)
  {}

  /** Destructor */
  virtual ~State_tracker()
  {}

  /** Getters */
  bool is_enabled() const
  { return m_enabled; }

  bool is_changed() const
  { return m_changed; }

  /** Called in the constructor of THD*/
  virtual bool enable(THD *thd)= 0;

  /** To be invoked when the tracker's system variable is checked (ON_CHECK). */
  virtual bool check(THD *thd, set_var *var)= 0;

  /** To be invoked when the tracker's system variable is updated (ON_UPDATE).*/
  virtual bool update(THD *thd)= 0;

  /** Store changed data into the given buffer. */
  virtual bool store(THD *thd, String *buf)= 0;

  /** Mark the entity as changed. */
  virtual void mark_as_changed(THD *thd, LEX_CSTRING *name)= 0;
};

bool sysvartrack_validate_value(THD *thd, const char *str, size_t len);
bool sysvartrack_reprint_value(THD *thd, char *str, size_t len);
bool sysvartrack_update(THD *thd);
size_t sysvartrack_value_len(THD *thd);
bool sysvartrack_value_construct(THD *thd, char *val, size_t len);


/**
  Session_tracker

  This class holds an object each for all tracker classes and provides
  methods necessary for systematic detection and generation of session
  state change information.
*/

class Session_tracker
{
private:
  State_tracker *m_trackers[SESSION_TRACKER_END + 1];

  /* The following two functions are private to disable copying. */
  Session_tracker(Session_tracker const &other)
  {
    DBUG_ASSERT(FALSE);
  }
  Session_tracker& operator= (Session_tracker const &rhs)
  {
    DBUG_ASSERT(FALSE);
    return *this;
  }

public:

  Session_tracker();
  ~Session_tracker()
  {
    deinit();
  }

  /* trick to make happy memory accounting system */
  void deinit()
  {
    for (int i= 0; i <= SESSION_TRACKER_END; i ++)
    {
      if (m_trackers[i])
        delete m_trackers[i];
      m_trackers[i]= NULL;
    }
  }

  void enable(THD *thd);
  bool server_boot_verify(const CHARSET_INFO *char_set);

  /** Returns the pointer to the tracker object for the specified tracker. */
  inline State_tracker *get_tracker(enum_session_tracker tracker) const
  {
    return m_trackers[tracker];
  }

  inline void mark_as_changed(THD *thd, enum enum_session_tracker tracker,
                              LEX_CSTRING *data)
  {
    if (m_trackers[tracker]->is_enabled())
      m_trackers[tracker]->mark_as_changed(thd, data);
  }


  void store(THD *thd, String *main_buf);
};

#endif /* SESSION_TRACKER_INCLUDED */
