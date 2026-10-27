#ifndef SQL_SQL_SYS_OR_DDL_TRIGGER_H
#define SQL_SQL_SYS_OR_DDL_TRIGGER_H

#include "sp_head.h"
#include "sql_trigger.h"  // trg_all_events_set, TRG_EVENT_MAX

/**
  Type representing events for system triggers (on logon, on logoff,
  on startup, on shutdown) and ddl triggers
*/
enum trg_sys_event_type
{
  TRG_SYS_EVENT_MIN= TRG_EVENT_MAX,
  TRG_EVENT_STARTUP= TRG_SYS_EVENT_MIN, // bit #3
  TRG_EVENT_SHUTDOWN, // bit #4
  TRG_EVENT_LOGON, // bit #5
  TRG_EVENT_LOGOFF, // bit #6
  TRG_EVENT_DDL, // bit #7
  TRG_SYS_EVENT_MAX
};

static inline trg_all_events_set sys_trg2bit(enum trg_sys_event_type trg)
{ return static_cast<trg_all_events_set>(1 << static_cast<int>(trg)); }

static inline bool is_sys_trg_events(trg_all_events_set events)
{
  static const trg_all_events_set sys_events= trg_all_events_set(0) |
                                              sys_trg2bit(TRG_EVENT_LOGON) |
                                              sys_trg2bit(TRG_EVENT_LOGOFF) |
                                              sys_trg2bit(TRG_EVENT_STARTUP) |
                                              sys_trg2bit(TRG_EVENT_SHUTDOWN);
  /*
    Return true in case any of system events is set in the mask
  */
  return ((events & sys_events) != 0);
}

static inline bool is_ddl_trg_events(trg_all_events_set events)
{
  static const trg_all_events_set ddl_events= trg_all_events_set(0) |
                                              sys_trg2bit(TRG_EVENT_DDL);
  /*
    Return true in case the only TRG_EVENT_DDL bit is set in the mask,
    that is the trigger is solely for handling DDL events
  */
  return events != 0 && ((events & ddl_events) == events);
}


/**
  Class for representing a system trigger to run on events like
  AFTER STARTUP / BEFORE SHUTDOWN / etc.
*/

class Sys_trigger : public Sql_alloc
{
public:
  Sys_trigger(THD *thd, sp_head *sp)
  : m_thd{thd}, m_sp{sp}
  {
    DBUG_ASSERT(sp);
  }

  ~Sys_trigger()
  {
    sp_head::destroy(m_sp);
  }

  /*
    Execute the system trigger.

    The method designed specially to don't return the value in order
    to ignore errors that could happen on running any of 'system' triggers.
    Do it so in order to at least be able to start the server in case any of
    on start up trigger failing regardless of failure reason
  */
  void execute();

  Sys_trigger(const Sys_trigger&)= delete;
  Sys_trigger &operator=(const Sys_trigger&)= delete;

private:
  bool execute_impl();

  THD *m_thd;
  sp_head *m_sp;
};

void run_after_startup_triggers(bool bootstrap_or_noacl);
void run_before_shutdown_triggers(bool bootstrap_or_noacl);

bool show_create_sys_trigger(THD *thd, const sp_name *trg_name);
bool fill_schema_triggers_from_mysql_events(THD *thd, TABLE_LIST *tables);
void report_trg_already_exist_error(const sp_name *spname);
bool find_sys_trigger_by_name(THD *thd, sp_name *spname, bool *found);

#endif /* SQL_SQL_SYS_OR_DDL_TRIGGER_H */
