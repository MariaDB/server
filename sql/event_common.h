#ifndef _EVENT_COMMON_H_
#define _EVENT_COMMON_H_

#include "sp_head.h"       // Stored_program_creation_ctx
#include "sql_alloc.h"     // Sql_alloc

/**
  Event_creation_ctx -- creation context of events.
*/

class Event_creation_ctx :public Stored_program_creation_ctx,
                          public Sql_alloc
{
public:
  static bool load_from_db(THD *thd,
                           MEM_ROOT *event_mem_root,
                           const char *db_name,
                           const char *event_name,
                           TABLE *event_tbl,
                           Stored_program_creation_ctx **ctx);

public:
  Stored_program_creation_ctx *clone(MEM_ROOT *mem_root) override
  {
    return new (mem_root)
               Event_creation_ctx(m_client_cs, m_connection_cl, m_db_cl);
  }

protected:
  Object_creation_ctx *create_backup_ctx(THD *thd) const override
  {
    /*
      We can avoid usual backup/restore employed in stored programs since we
      know that this is a top level statement and the worker thread is
      allocated exclusively to execute this event.
    */

    return NULL;
  }

private:
  Event_creation_ctx(CHARSET_INFO *client_cs,
                     CHARSET_INFO *connection_cl,
                     CHARSET_INFO *db_cl)
    : Stored_program_creation_ctx(client_cs, connection_cl, db_cl)
  { }
};

class Event_db_repository_common
{
public:
  static bool open_event_table(THD *thd, enum thr_lock_type lock_type,
                               TABLE **table);

  static bool check_system_tables(THD *thd);

  /** In case of an error, a message is printed to the error log. */
  static Table_check_intact_log_error table_intact;

  static LEX_CSTRING MYSQL_EVENT_NAME;

  static const TABLE_FIELD_DEF event_table_def;
};

class Events_common
{
public:
  static ulong startup_state;

  /*
    the following block is to support --event-scheduler command line option
    and the @@global.event_scheduler SQL variable.
    See sys_var.cc
  */
  enum enum_opt_event_scheduler { EVENTS_OFF, EVENTS_ON, EVENTS_DISABLED,
                                  EVENTS_ORIGINAL };
};

#endif
