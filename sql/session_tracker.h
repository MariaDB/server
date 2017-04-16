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

#ifndef EMBEDDED_LIBRARY
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
  TRANSACTION_INFO_TRACKER,                      /* Transaction state */
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

  /** To be invoked when the tracker's system variable is updated (ON_UPDATE).*/
  virtual bool update(THD *thd, set_var *var)= 0;

  /** Store changed data into the given buffer. */
  virtual bool store(THD *thd, String *buf)= 0;

  /** Mark the entity as changed. */
  virtual void mark_as_changed(THD *thd, LEX_CSTRING *name);
};

bool sysvartrack_validate_value(THD *thd, const char *str, size_t len);
bool sysvartrack_reprint_value(THD *thd, char *str, size_t len);
bool sysvartrack_update(THD *thd, set_var *var);
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

  Session_tracker();
  ~Session_tracker()
  {
    deinit();
  }

  /* trick to make happy memory accounting system */
  void deinit()
  {
    for (int i= 0; i < SESSION_TRACKER_END; i++)
    {
      if (m_trackers[i])
        delete m_trackers[i];
      m_trackers[i]= NULL;
    }
  }

  void enable(THD *thd);
  static bool server_boot_verify(CHARSET_INFO *char_set);

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
  TX_READ_INHERIT =   0,  ///< not explicitly set, inherit session.tx_read_only
  TX_READ_ONLY    =   1,  ///< START TRANSACTION READ ONLY,  or tx_read_only=1
  TX_READ_WRITE   =   2,  ///< START TRANSACTION READ WRITE, or tx_read_only=0
};


/**
  Transaction isolation level
*/
enum enum_tx_isol_level {
  TX_ISOL_INHERIT     = 0, ///< not explicitly set, inherit session.tx_isolation
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
private:
  /** Helper function: turn table info into table access flag */
  enum_tx_state calc_trx_state(THD *thd, thr_lock_type l, bool has_trx);
public:
  /** Constructor */
  Transaction_state_tracker();
  bool enable(THD *thd)
  { return update(thd, NULL); }
  bool update(THD *thd, set_var *var);
  bool store(THD *thd, String *buf);

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

  void reset();

  inline void update_change_flags(THD *thd)
  {
    tx_changed &= uint(~TX_CHG_STATE);
    tx_changed |= (tx_curr_state != tx_reported_state) ? TX_CHG_STATE : 0;
    if (tx_changed != TX_CHG_NONE)
      mark_as_changed(thd, NULL);
  }
};

#define TRANSACT_TRACKER(X) \
 do { if (thd->variables.session_track_transaction_info > TX_TRACK_NONE) \
   {((Transaction_state_tracker *) \
         thd->session_tracker.get_tracker(TRANSACTION_INFO_TRACKER)) \
          ->X; } } while(0)
#define SESSION_TRACKER_CHANGED(A,B,C) \
  thd->session_tracker.mark_as_changed(A,B,C)
#else

#define TRANSACT_TRACKER(X) do{}while(0)
#define SESSION_TRACKER_CHANGED(A,B,C) do{}while(0)

#endif //EMBEDDED_LIBRARY

#endif /* SESSION_TRACKER_INCLUDED */
