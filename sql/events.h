#ifndef _EVENT_H_
#define _EVENT_H_
/* Copyright (c) 2004, 2013, Oracle and/or its affiliates. All rights reserved.

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

/**
  @defgroup Event_Scheduler Event Scheduler
  @ingroup Runtime_Environment
  @{

  @file events.h

  A public interface of Events_Scheduler module.
*/

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key key_event_scheduler_LOCK_scheduler_state;
extern PSI_cond_key key_event_scheduler_COND_state;
extern PSI_thread_key key_thread_event_scheduler, key_thread_event_worker;
#endif /* HAVE_PSI_INTERFACE */

extern PSI_memory_key key_memory_event_basic_root;

/* Always defined, for SHOW PROCESSLIST. */
extern PSI_stage_info stage_waiting_on_empty_queue;
extern PSI_stage_info stage_waiting_for_next_activation;
extern PSI_stage_info stage_waiting_for_scheduler_to_stop;

#include "sql_string.h"                         /* LEX_CSTRING */
#include "my_time.h"                            /* interval_type */

class Event_db_repository;
class Event_parse_data;
class Event_queue;
class Event_scheduler;
struct TABLE_LIST;
class THD;
class SQL_CATALOG;
typedef class Item COND;

int
sortcmp_lex_string(const LEX_CSTRING *s, const LEX_CSTRING *t,
                   const CHARSET_INFO *cs);

/**
  @brief A facade to the functionality of the Event Scheduler.

  The life cycle of the Events module is the following:

  At server start up, do for each catalog:
     init_mutexes() -> init()
  When the server is running:
     create_event(), drop_event(), start_or_stop_event_scheduler(), etc
  At shutdown:
     deinit(), destroy_mutexes().

  The peculiar initialization and shutdown cycle is an adaptation to the
  outside server startup/shutdown framework and mimics the rest of MySQL
  subsystems (ACL, time zone tables, etc).
*/

class Events
{
public:
  Events() :
    state(EVENTS_OFF),
    startup_state(EVENTS_OFF)
    {}

  /*
    the following block is to support --event-scheduler command line option
    and the @@global.event_scheduler SQL variable.
    See sys_var.cc
  */
  enum event_states { EVENTS_OFF, EVENTS_ON, EVENTS_DISABLED,
    EVENTS_ORIGINAL };
  /* Protected using LOCK_global_system_variables only. */
  event_states state, startup_state;
  ulong inited;
  bool check_if_system_tables_error();
  bool start(int *err_no);
  bool stop();

public:
  /* A hack needed for Event_queue_element */
  Event_db_repository *
  get_db_repository() { return db_repository; }

  int init(THD *thd, const SQL_CATALOG *catalog, event_states start_state,
           bool opt_noacl);

  void deinit();

  void init_mutexes();

  void destroy_mutexes();

  bool create_event(THD *thd, Event_parse_data *parse_data);

  bool update_event(THD *thd, Event_parse_data *parse_data,
                    LEX_CSTRING *new_dbname, LEX_CSTRING *new_name);

  bool drop_event(THD *thd, const LEX_CSTRING *dbname, const LEX_CSTRING *name,
                  bool if_exists);

  void drop_schema_events(THD *thd, const char *db);

  bool show_create_event(THD *thd, const LEX_CSTRING *dbname,
                         const LEX_CSTRING *name);

  /* Needed for both SHOW CREATE EVENT and INFORMATION_SCHEMA */
  int reconstruct_interval_expression(String *buf, interval_type interval,
                                      longlong expression);

  int fill_schema_events(THD *thd, TABLE_LIST *tables, COND * /* cond */);

  void dump_internal_status();

private:

  bool load_events_from_db(THD *thd);

private:
  Event_queue         *event_queue;
  Event_scheduler     *scheduler;
  Event_db_repository *db_repository;

private:
  /* Prevent use of these */
  Events(const Events &);
  void operator=(Events &);
};

/**
  @} (end of group Event Scheduler)
*/

extern Events global_events;

bool startup_events(const SQL_CATALOG *catalog, bool noacl_or_bootstrap);
int fill_schema_events(THD *thd, TABLE_LIST *tables, COND * /* cond */);
#endif /* _EVENT_H_ */
