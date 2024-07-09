#ifndef SESSION_TRACKER_INCLUDED
#define SESSION_TRACKER_INCLUDED

/* Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2016, 2017, MariaDB Corporation.

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
#include "sql_hset.h"

#ifndef EMBEDDED_LIBRARY
/* forward declarations */
class THD;
class set_var;
class String;
class user_var_entry;


enum enum_session_tracker
{
  SESSION_SYSVARS_TRACKER,                       /* Session system variables */
  CURRENT_SCHEMA_TRACKER,                        /* Current schema */
  SESSION_STATE_CHANGE_TRACKER,
  TRANSACTION_INFO_TRACKER,                      /* Transaction state */
#ifdef USER_VAR_TRACKING
  USER_VARIABLES_TRACKER,
#endif // USER_VAR_TRACKING
  SESSION_TRACKER_END                            /* must be the last */
};

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
  (ON_UPDATE) of the tracker system variable, namely - update().
*/

class State_tracker
{
protected:
  /**
    Is tracking enabled for a particular session state type ?

    @note: it is a cache of the corresponding thd->variables.session_track_xxx
    variable
  */
  bool m_enabled;

  void set_changed(THD *thd);

private:
  /** Has the session state type changed ? */
  bool m_changed;

public:
  virtual ~State_tracker() = default;

  /** Getters */
  bool is_enabled() const
  { return m_enabled; }

  bool is_changed() const
  { return m_changed; }

  void reset_changed() { m_changed= false; }

  /**
    Called by THD::init() when new connection is being created

    We may inherit m_changed from previous connection served by this THD if
    connection was broken or client didn't have session tracking capability.
    Thus we have to reset it here.
  */
  virtual bool enable(THD *thd)
  {
    reset_changed();
    return update(thd, 0);
  }

  /** To be invoked when the tracker's system variable is updated (ON_UPDATE).*/
  virtual bool update(THD *thd, set_var *var)= 0;

  /** Store changed data into the given buffer. */
  virtual bool store(THD *thd, String *buf)= 0;

  /** Mark the entity as changed. */
  void mark_as_changed(THD *thd) { if (is_enabled()) set_changed(thd); }
};


/**
  Session_sysvars_tracker

  This is a tracker class that enables & manages the tracking of session
  system variables. It internally maintains a hash of user supplied variable
  references and a boolean field to store if the variable was changed by the
  last statement.
*/

class Session_sysvars_tracker: public State_tracker
{
  struct sysvar_node_st {
    sys_var *m_svar;
    bool *test_load;
    bool m_changed;
  };

  class vars_list
  {
    /**
      Registered system variables. (@@session_track_system_variables)
      A hash to store the name of all the system variables specified by the
      user.
    */
    HASH m_registered_sysvars;
    /**
      If TRUE then we want to check all session variable.
    */
    bool track_all;
    void init()
    {
      my_hash_init(PSI_INSTRUMENT_ME, &m_registered_sysvars, &my_charset_bin,
                   0, 0, 0, (my_hash_get_key) sysvars_get_key, my_free,
                   HASH_UNIQUE | (mysqld_server_initialized ?  HASH_THREAD_SPECIFIC : 0));
    }
    void free_hash()
    {
      DBUG_ASSERT(my_hash_inited(&m_registered_sysvars));
      my_hash_free(&m_registered_sysvars);
    }

    sysvar_node_st *search(const sys_var *svar);
    sysvar_node_st *at(ulong i)
    {
      DBUG_ASSERT(i < m_registered_sysvars.records);
      return reinterpret_cast<sysvar_node_st*>(
               my_hash_element(&m_registered_sysvars, i));
    }
  public:
    vars_list(): track_all(false) { init(); }
    ~vars_list() { if (my_hash_inited(&m_registered_sysvars)) free_hash(); }
    void deinit() { free_hash(); }

    sysvar_node_st *insert_or_search(const sys_var *svar)
    {
      sysvar_node_st *res= search(svar);
      if (!res)
      {
        if (track_all)
        {
          insert(svar);
          return search(svar);
        }
      }
      return res;
    }

    bool insert(const sys_var *svar);
    void reinit();
    void reset();
    inline bool is_enabled()
    {
      return track_all || m_registered_sysvars.records;
    }
    void copy(vars_list* from, THD *thd);
    bool parse_var_list(THD *thd, LEX_STRING var_list, bool throw_error,
                        CHARSET_INFO *char_set);
    bool construct_var_list(char *buf, size_t buf_len);
    bool store(THD *thd, String *buf);
  };
  /**
    Two objects of vars_list type are maintained to manage
    various operations.
  */
  vars_list orig_list;
  bool m_parsed;

public:
  void init(THD *thd);
  void deinit(THD *thd);
  bool enable(THD *thd) override;
  bool update(THD *thd, set_var *var) override;
  bool store(THD *thd, String *buf) override;
  void mark_as_changed(THD *thd, const sys_var *var);
  void deinit() { orig_list.deinit(); }
  /* callback */
  static uchar *sysvars_get_key(const char *entry, size_t *length,
                                my_bool not_used __attribute__((unused)));

  friend bool sysvartrack_global_update(THD *thd, char *str, size_t len);
};


bool sysvartrack_validate_value(THD *thd, const char *str, size_t len);
bool sysvartrack_global_update(THD *thd, char *str, size_t len);


/**
  Current_schema_tracker,

  This is a tracker class that enables & manages the tracking of current
  schema for a particular connection.
*/

class Current_schema_tracker: public State_tracker
{
public:
  bool update(THD *thd, set_var *var) override;
  bool store(THD *thd, String *buf) override;
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

class Session_state_change_tracker: public State_tracker
{
public:
  bool update(THD *thd, set_var *var) override;
  bool store(THD *thd, String *buf) override;
};


/*
  Transaction_state_tracker
*/

/**
  Transaction state (no transaction, transaction active, work attached, etc.)
*/
enum enum_tx_state {
  TX_EMPTY        =   0,  ///< "none of the below"
  TX_EXPLICIT     =   1,  ///< an explicit transaction is active
  TX_IMPLICIT     =   2,  ///< an implicit transaction is active
  TX_READ_TRX     =   4,  ///<     transactional reads  were done
  TX_READ_UNSAFE  =   8,  ///< non-transaction   reads  were done
  TX_WRITE_TRX    =  16,  ///<     transactional writes were done
  TX_WRITE_UNSAFE =  32,  ///< non-transactional writes were done
  TX_STMT_UNSAFE  =  64,  ///< "unsafe" (non-deterministic like UUID()) stmts
  TX_RESULT_SET   = 128,  ///< result set was sent
  TX_WITH_SNAPSHOT= 256,  ///< WITH CONSISTENT SNAPSHOT was used
  TX_LOCKED_TABLES= 512   ///< LOCK TABLES is active
};


/**
  Transaction access mode
*/
enum enum_tx_read_flags {
  TX_READ_INHERIT =   0,  ///< not explicitly set, inherit session.transaction_read_only
  TX_READ_ONLY    =   1,  ///< START TRANSACTION READ ONLY,  or transaction_read_only=1
  TX_READ_WRITE   =   2,  ///< START TRANSACTION READ WRITE, or transaction_read_only=0
};


/**
  Transaction isolation level
*/
enum enum_tx_isol_level {
  TX_ISOL_INHERIT     = 0, ///< not explicitly set, inherit session.transaction_isolation
  TX_ISOL_UNCOMMITTED = 1,
  TX_ISOL_COMMITTED   = 2,
  TX_ISOL_REPEATABLE  = 3,
  TX_ISOL_SERIALIZABLE= 4
};


/**
  Transaction tracking level
*/
enum enum_session_track_transaction_info {
  TX_TRACK_NONE      = 0,  ///< do not send tracker items on transaction info
  TX_TRACK_STATE     = 1,  ///< track transaction status
  TX_TRACK_CHISTICS  = 2   ///< track status and characteristics
};


/**
  This is a tracker class that enables & manages the tracking of
  current transaction info for a particular connection.
*/

class Transaction_state_tracker : public State_tracker
{
  /** Helper function: turn table info into table access flag */
  enum_tx_state calc_trx_state(THD *thd, thr_lock_type l, bool has_trx);
public:

  bool enable(THD *thd) override
  {
    m_enabled= false;
    tx_changed= TX_CHG_NONE;
    tx_curr_state= TX_EMPTY;
    tx_reported_state= TX_EMPTY;
    tx_read_flags= TX_READ_INHERIT;
    tx_isol_level= TX_ISOL_INHERIT;
    return State_tracker::enable(thd);
  }

  bool update(THD *thd, set_var *var) override;
  bool store(THD *thd, String *buf) override;

  /** Change transaction characteristics */
  void set_read_flags(THD *thd, enum enum_tx_read_flags flags);
  void set_isol_level(THD *thd, enum enum_tx_isol_level level);

  /** Change transaction state */
  void clear_trx_state(THD *thd, uint clear);
  void add_trx_state(THD *thd, uint add);
  void inline add_trx_state(THD *thd, thr_lock_type l, bool has_trx)
  {
    add_trx_state(thd, calc_trx_state(thd, l, has_trx));
  }
  void add_trx_state_from_thd(THD *thd);
  void end_trx(THD *thd);


private:
  enum enum_tx_changed {
    TX_CHG_NONE     = 0,  ///< no changes from previous stmt
    TX_CHG_STATE    = 1,  ///< state has changed from previous stmt
    TX_CHG_CHISTICS = 2   ///< characteristics have changed from previous stmt
  };

  /** any trackable changes caused by this statement? */
  uint                     tx_changed;

  /** transaction state */
  uint                     tx_curr_state,  tx_reported_state;

  /** r/w or r/o set? session default? */
  enum enum_tx_read_flags  tx_read_flags;

  /**  isolation level */
  enum enum_tx_isol_level  tx_isol_level;

  inline void update_change_flags(THD *thd)
  {
    tx_changed &= uint(~TX_CHG_STATE);
    tx_changed |= (tx_curr_state != tx_reported_state) ? TX_CHG_STATE : 0;
    if (tx_changed != TX_CHG_NONE)
      set_changed(thd);
  }
};

#define TRANSACT_TRACKER(X) \
 do { if (thd->variables.session_track_transaction_info > TX_TRACK_NONE) \
        thd->session_tracker.transaction_info.X; } while(0)


/**
  User_variables_tracker

  This is a tracker class that enables & manages the tracking of user variables.
*/

#ifdef USER_VAR_TRACKING
class User_variables_tracker: public State_tracker
{
  Hash_set<const user_var_entry> m_changed_user_variables;
public:
  User_variables_tracker():
    m_changed_user_variables(PSI_INSTRUMENT_ME, &my_charset_bin, 0, 0,
                             sizeof(const user_var_entry*), 0, 0, HASH_UNIQUE |
                             mysqld_server_initialized ? HASH_THREAD_SPECIFIC : 0) {}
  bool update(THD *thd, set_var *var);
  bool store(THD *thd, String *buf);
  void mark_as_changed(THD *thd, const user_var_entry *var)
  {
    if (is_enabled())
    {
      m_changed_user_variables.insert(var);
      set_changed(thd);
    }
  }
  void deinit() { m_changed_user_variables.~Hash_set(); }
};
#endif // USER_VAR_TRACKING


/**
  Session_tracker

  This class holds an object each for all tracker classes and provides
  methods necessary for systematic detection and generation of session
  state change information.
*/

class Session_tracker
{
  State_tracker *m_trackers[SESSION_TRACKER_END];

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
  Current_schema_tracker current_schema;
  Session_state_change_tracker state_change;
  Transaction_state_tracker transaction_info;
  Session_sysvars_tracker sysvars;
#ifdef USER_VAR_TRACKING
  User_variables_tracker user_variables;
#endif // USER_VAR_TRACKING

  Session_tracker()
  {
    m_trackers[SESSION_SYSVARS_TRACKER]= &sysvars;
    m_trackers[CURRENT_SCHEMA_TRACKER]= &current_schema;
    m_trackers[SESSION_STATE_CHANGE_TRACKER]= &state_change;
    m_trackers[TRANSACTION_INFO_TRACKER]= &transaction_info;
#ifdef USER_VAR_TRACKING
    m_trackers[USER_VARIABLES_TRACKER]= &user_variables;
#endif // USER_VAR_TRACKING
  }

  void enable(THD *thd)
  {
    for (int i= 0; i < SESSION_TRACKER_END; i++)
      m_trackers[i]->enable(thd);
  }

  void store(THD *thd, String *main_buf);
};


int session_tracker_init();
#else

#define TRANSACT_TRACKER(X) do{}while(0)

class Session_tracker
{
  class Dummy_tracker
  {
  public:
    void mark_as_changed(THD *thd) {}
    void mark_as_changed(THD *thd, const sys_var *var) {}
  };
public:
  Dummy_tracker current_schema;
  Dummy_tracker state_change;
  Dummy_tracker sysvars;
};

#endif //EMBEDDED_LIBRARY

#endif /* SESSION_TRACKER_INCLUDED */
