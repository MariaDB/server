#include "mariadb.h"              /* NO_EMBEDDED_ACCESS_CHECKS */

#include <m_ctype.h>
#include <mysqld_error.h>

#include "table.h"

#include "event_data_objects.h"   // load_creating_context_for_sys_trg
#include "event_db_repository.h"  // enum_events_table_field
#include "event_parse_data.h"     // Event_parse_data

#include "key.h"                  // key_copy
#include "lex_string.h"
#include "lock.h"                 // lock_object_name
#include "mysqld.h"               // next_thread_id
#include "records.h"

#include "sp_head.h"              // sp_head
#include "sql_base.h"             // close_thread_tables
#include "sql_db.h"               // get_default_db_collation
#include "sql_i_s.h"              // schema_table_store_record
#include "sql_parse.h"            // sp_process_definer
#include "sql_show.h"             // append_identifier
#include "sql_sys_or_ddl_trigger.h"
#include "sql_trigger.h"
#include "strfunc.h"              //set_to_string


static LEX_CSTRING event_table_name{STRING_WITH_LEN("event")};

/**
  Raise the error ER_TRG_ALREADY_EXISTS
*/

static void report_trg_already_exist_error(const sp_name *spname)
{
  /*
    Report error in case there is a trigger on DML event with
    the same name as the system trigger we are going to create
  */
  char trigname_buff[FN_REFLEN];

  strxnmov(trigname_buff, sizeof(trigname_buff) - 1,
           spname->m_db.str, ".",
           spname->m_name.str, NullS);
  my_error(ER_TRG_ALREADY_EXISTS, MYF(0), trigname_buff);
}


/**
  Check whether there is a trigger with specified name on DML event

  @return true and set an error in DA in case there is a trigger
          with supplied name on DML event, else return false
*/

static bool check_dml_trigger_exist(sp_name *spname)
{
  char trn_path_buff[FN_REFLEN];
  LEX_CSTRING trn_path= { trn_path_buff, 0 };

  build_trn_path(spname, (LEX_STRING*) &trn_path);

  if (!check_trn_exists(&trn_path))
  {
    /*
      Report error in case there is a trigger on DML event with
      the same name as the system trigger we are going to create
    */
    report_trg_already_exist_error(spname);
    return true;
  }

  return false;
}


/**
  Search a system or ddl trigger by its name in the table mysql.event.

  @return false in case there is no trigger with specified name,
          else return true
*/

static bool find_sys_trigger_by_name(TABLE *event_table, const sp_name *spname)
{
  event_table->field[ET_FIELD_DB]->store(spname->m_db.str,
                                         spname->m_db.length, &my_charset_bin);
  event_table->field[ET_FIELD_NAME]->store(spname->m_name.str,
                                           spname->m_name.length,
                                           &my_charset_bin);

  uchar key[MAX_KEY_LENGTH];
  key_copy(key, event_table->record[0], event_table->key_info,
           event_table->key_info->key_length);

  int ret= event_table->file->ha_index_read_idx_map(event_table->record[0], 0,
                                                    key, HA_WHOLE_KEY,
                                                    HA_READ_KEY_EXACT);
  /*
    ret != 0 in case 'row not found'; ret == 0 if 'row found'
  */
  return !ret;
}


/**
  Store information about the trigger being created into the table mysql.event

  @param thd  Thread handler
  @param lex  Lex used for parsing the original CREATE TRIGGER statement
  @param event_table  Opened table mysql.event where to store the trigger's
                      metadata
  @param sphead  an instance of sp_head created for trigger
  @param trg_chistics  trigger characteristics (event time, event kind, etc)
  @pram sql_mode  sql_mode used for creation of the trigger

  @return false on success, true on error
*/

static bool store_trigger_metadata(THD *thd, LEX *lex, TABLE *event_table,
                                   sp_head *sphead,
                                   const st_trg_chistics &trg_chistics,
                                   sql_mode_t sql_mode)
{
  restore_record(event_table, s->default_values);

  if (sphead->m_body.length > event_table->field[ET_FIELD_BODY]->field_length)
  {
    my_error(ER_TOO_LONG_BODY, MYF(0), sphead->m_name.str);

    return true;
  }

  Field **fields= event_table->field;
  int ret;

  char definer_buf[USER_HOST_BUFF_SIZE];
  LEX_CSTRING definer;
  thd->lex->definer->set_lex_string(&definer, definer_buf);

  if (fields[ET_FIELD_DEFINER]->store(definer.str, definer.length,
                                      system_charset_info))
  {
    my_error(ER_EVENT_DATA_TOO_LONG, MYF(0),
             fields[ET_FIELD_DEFINER]->field_name.str);
    return true;
  }

  if (fields[ET_FIELD_DB]->store(sphead->m_db.str,
                                 sphead->m_db.length,
                                 system_charset_info))
   {
     my_error(ER_EVENT_DATA_TOO_LONG, MYF(0),
              fields[ET_FIELD_DB]->field_name.str);
     return true;
   }

  if (fields[ET_FIELD_NAME]->store(sphead->m_name.str,
                                   sphead->m_name.length,
                                   system_charset_info))
   {
     my_error(ER_EVENT_DATA_TOO_LONG, MYF(0),
              fields[ET_FIELD_DB]->field_name.str);
     return true;
   }

  fields[ET_FIELD_CHARACTER_SET_CLIENT]->set_notnull();
  ret= fields[ET_FIELD_CHARACTER_SET_CLIENT]->store(
    &thd->variables.character_set_client->cs_name,
    system_charset_info);
  if (ret)
  {
    my_error(ER_EVENT_STORE_FAILED, MYF(0),
             fields[ET_FIELD_CHARACTER_SET_CLIENT]->field_name.str, ret);
    return true;
  }

  fields[ET_FIELD_COLLATION_CONNECTION]->set_notnull();
  ret= fields[ET_FIELD_COLLATION_CONNECTION]->
    store(&thd->variables.collation_connection->coll_name,
          system_charset_info);
  if (ret)
  {
    my_error(ER_EVENT_STORE_FAILED, MYF(0),
             fields[ET_FIELD_COLLATION_CONNECTION]->field_name.str, ret);
    return true;

  }

  CHARSET_INFO *db_cl= get_default_db_collation(thd, sphead->m_db.str);

  fields[ET_FIELD_DB_COLLATION]->set_notnull();
  ret= fields[ET_FIELD_DB_COLLATION]->store(&db_cl->coll_name,
                                              system_charset_info);
  if (ret)
  {
    my_error(ER_EVENT_STORE_FAILED, MYF(0),
             fields[ET_FIELD_DB_COLLATION]->field_name.str, ret);
    return true;

  }

  ret= fields[ET_FIELD_ON_COMPLETION]->store(
    (longlong)Event_parse_data::ON_COMPLETION_DEFAULT, true);
  if (ret)
  {
    my_error(ER_EVENT_STORE_FAILED, MYF(0),
             fields[ET_FIELD_ON_COMPLETION]->field_name.str, ret);
    return true;
  }

  ret= fields[ET_FIELD_ORIGINATOR]->store(
    (longlong)global_system_variables.server_id, true);
  if (ret)
  {
    my_error(ER_EVENT_STORE_FAILED, MYF(0),
             fields[ET_FIELD_ORIGINATOR]->field_name.str, ret);
    return true;
  }

  ret= fields[ET_FIELD_CREATED]->set_time();
  if (ret)
  {
    my_error(ER_EVENT_STORE_FAILED, MYF(0),
             fields[ET_FIELD_CREATED]->field_name.str, ret);
    return true;
  }

  ret= fields[ET_FIELD_SQL_MODE]->store((longlong)sql_mode, true);
  if (ret)
  {
    my_error(ER_EVENT_STORE_FAILED, MYF(0),
             fields[ET_FIELD_SQL_MODE]->field_name.str, ret);
    return true;
  }

  ret= fields[ET_FIELD_BODY]->store(sphead->m_body.str,
                                    sphead->m_body.length,
                                    system_charset_info);
  if (ret)
  {
    my_error(ER_EVENT_STORE_FAILED, MYF(0),
             fields[ET_FIELD_BODY]->field_name.str, ret);
    return true;
  }

  /*
    trg_chistics.events has meaningful bits for every trigger events,
    that is for DML, DDL, system events. Matching of event types and values of
    trg_chistics.events is depicted below:
      ON INSERT = 0x01
      ON UPDATE = 0x02
      ON DELETE = 0x04
      ON STARTUP = 0x08
      ON SHUTDOWN = 0x10

    The table mysql.event declares the column `kind` as a set with
    the following values
      `kind` set('SCHEDULE','STARTUP','SHUTDOWN','LOGON','LOGOFF','DDL')

    So, events for system triggers stored in the column mysql.event.kind
    has the following values:
      SCHEDULE = 0x01
      STARTUP = 0x02
      SHUTDOWN = 0x04
      LOGON = 0x08
      LOGOFF = 0x10
      DDL = 0x20

    So, for mapping trg_chistics.events to mysql.event.kind we have to
    shift trg_chistics.events to the right by 3 bits (to get the first not DML
    trigger event type) and then shift to the left by 1 bit take into account
    the enumerator value for SCHENDULE
  */
  longlong trg_events= (trg_chistics.events >> 3);
  ret= fields[ET_FIELD_KIND]->store((trg_events << 1), true);
  if (ret)
  {
    my_error(ER_EVENT_STORE_FAILED, MYF(0),
             fields[ET_FIELD_KIND]->field_name.str, ret);
    return true;
  }

  ret= fields[ET_FIELD_WHEN]->store((longlong)trg_chistics.action_time + 1,
                                    true);
  if (ret)
  {
    my_error(ER_EVENT_STORE_FAILED, MYF(0),
             fields[ET_FIELD_WHEN]->field_name.str, ret);
    return true;
  }
  fields[ET_FIELD_WHEN]->set_notnull();

  ret= event_table->file->ha_write_row(event_table->record[0]);
  if (ret)
  {
    event_table->file->print_error(ret, MYF(0));
    return true;
  }

  return false;
}

/**
  The class Transaction_Resources_Guard is RAII class to recover
  the transaction related state as it was before entering constructor.
  It is typically used for releasing mdl locks to the savepoint and committing
  a transaction on any return path from a function where this guard class
  is instantiated.
*/

class Transaction_Resources_Guard
{
public:
  Transaction_Resources_Guard(THD *thd, sql_mode_t saved_mode)
  : m_thd{thd}, m_mdl_savepoint{thd->mdl_context.mdl_savepoint()},
    m_saved_mode{saved_mode}
  {}
  ~Transaction_Resources_Guard()
  {
    m_thd->commit_whole_transaction_and_close_tables();
    m_thd->mdl_context.rollback_to_savepoint(m_mdl_savepoint);
    m_thd->variables.sql_mode= m_saved_mode;
  }
private:
  THD *m_thd;
  MDL_savepoint m_mdl_savepoint;
  sql_mode_t m_saved_mode;
};

static THD *thd_for_sys_triggers= nullptr;
static THD *original_thd= nullptr;

static Sys_trigger*
sys_triggers[TRG_ACTION_MAX][TRG_SYS_EVENT_MAX - TRG_EVENT_STARTUP]= {{nullptr}};

static Sys_trigger *
get_trigger_by_type(trg_action_time_type action_time,
                    trg_sys_event_type trg_type)
{
  return sys_triggers[action_time][trg_type - TRG_EVENT_STARTUP];
}

static void register_trigger(Sys_trigger *sys_trg,
                             trg_action_time_type trg_when,
                             longlong trg_kind)
{
  trg_kind= trg_kind - TRG_EVENT_STARTUP;
  Sys_trigger *cur_trg= sys_triggers[trg_when][trg_kind];

  if (cur_trg)
  {
    /*
      Add the trigger to the end of the list of trigger sharing
      the same trigger time/type
    */
    while (cur_trg)
    {
      if (cur_trg->next == nullptr)
      {
        // cur_ptr references the last trigger in the list
        cur_trg->next= sys_trg;
        break;
      }
      else
        cur_trg= cur_trg->next;
    }
  }
  else
    sys_triggers[trg_when][trg_kind]= sys_trg;
}


/**
  Associate the instance of the class Sys_trigger with combination of
  trigger time/trigger type in the two dimensional array sys_triggers.

  @param sys_trg  An instance of the class Sys_trigger to associate with
                  trigger's event time and event kind
  @param trg_when  When to fire the trigger - BEFORE or AFTER the event
  @param trg_kind  Kind of trigger event (ON STARTUP, ON SHUTDOWN, etc)
*/

static void register_system_triggers(Sys_trigger *sys_trg,
                                     enum trg_action_time_type trg_when,
                                     Event_parse_data::enum_kind trg_kind)
{
  /*
    trg_kind is a bit set . Every turned on bit of the set specifies
    the event type. trg_kind is stored in the table mysql.event and
    declared as SET('SCHEDULE','STARTUP','SHUTDOWN','LOGON','LOGOFF','DDL')
    So, binary representation of different event kinds are as following:
      0x01 -- SCHEDULE (special value to represent events
                        rather than triggers)
      0x02 -- STARTUP
      0x04 -- SHUTDOWN
      0x08 -- LOGON
      0x10 -- LOGOFF
      0x20 -- DDL
   On the other hand, values of trg_sys_event_type are sequentially
   enumerated values, so need to do translation from bit mask to enumeration
  */
  trg_all_events_set trg_event_for_reg= TRG_EVENT_STARTUP;
  for (trg_all_events_set tk= trg_kind >> 1; tk != 0;
       tk= tk >>1, trg_event_for_reg++)
  {
    if (tk & 0x01)
      register_trigger(sys_trg->inc_ref_count(), trg_when,
                       Event_parse_data::enum_kind(trg_event_for_reg));
  }
}


/**
   Remove the trigger being dropped from the sys_triggers array.
   System triggers to be fired for some combination of type/time are searched
   in this array, before a trigger be considering as deleted it should be
   removed from this array.

   @param spname  name of trigger to be removed from the sys_triggers array
 */

void unregister_trigger(sp_name *spname)
{
  for (int i= 0; i < TRG_ACTION_MAX; i++)
  {
    for (int j= 0; j < TRG_SYS_EVENT_MAX - TRG_EVENT_STARTUP; j++)
    {
      Sys_trigger *sys_trg= sys_triggers[i][j];
      Sys_trigger *prev_sys_trg= nullptr;
      while (sys_trg)
      {
        if (sys_trg->compare_name(spname))
        {
          if (prev_sys_trg)
            /* Exclude the trigger being dropped from the list */
            prev_sys_trg->next= sys_trg->next;
          else
            sys_triggers[i][j]= sys_trg->next;

          return;
        }
        prev_sys_trg= sys_trg;
        sys_trg= sys_trg->next;
      }
    }
  }
}


/**
  Handle the statement CREATE TRIGGER for system triggers,
  such as ON STARTUP, ON SHUTDOWN.

  @param thd  Thread handler

  @return false on success, true on error
*/

bool mysql_create_sys_trigger(THD *thd)
{
  if (!thd->lex->spname->m_db.length)
  {
    my_error(ER_NO_DB_ERROR, MYF(0));
    return true;
  }

  /*
    We don't allow creating triggers on tables in the 'mysql' schema
  */
  if (thd->lex->spname->m_db.streq(MYSQL_SCHEMA_NAME))
  {
    my_error(ER_NO_TRIGGERS_ON_SYSTEM_SCHEMA, MYF(0));
    return true;
  }

  if (thd->lex->trg_chistics.action_time == TRG_ACTION_BEFORE &&
      (sys_trg2bit(TRG_EVENT_STARTUP) & thd->lex->trg_chistics.events))
  {
    my_error(ER_SYS_TRG_SEMANTIC_ERROR, MYF(0), thd->lex->spname->m_db.str,
             thd->lex->spname->m_name.str, "BEFORE", "STARTUP");
    return true;
  }

  if (thd->lex->trg_chistics.action_time == TRG_ACTION_AFTER &&
      (sys_trg2bit(TRG_EVENT_SHUTDOWN) & thd->lex->trg_chistics.events))
  {
    my_error(ER_SYS_TRG_SEMANTIC_ERROR, MYF(0), thd->lex->spname->m_db.str,
             thd->lex->spname->m_name.str, "AFTER", "SHUTDOWN");
    return true;
  }

  if (sp_process_definer(thd))
    return true;

  /*
    Since the table mysql.event is used both for storing meta data about
    events and system/ddl triggers, use the MDL_key::EVENT namespace
    for acquiring the mdl lock
  */
  if (lock_object_name(thd, MDL_key::EVENT, thd->lex->spname->m_db,
                       thd->lex->spname->m_name))
    return true;

  if (check_dml_trigger_exist(thd->lex->spname))
    return true;

  /* Reset sql_mode during data dictionary operations. */
  sql_mode_t saved_sql_mode= thd->variables.sql_mode;
  thd->variables.sql_mode= 0;

  TABLE *event_table;
  if (Event_db_repository::open_event_table(thd, TL_WRITE, &event_table))
  {
    thd->variables.sql_mode= saved_sql_mode;

    return true;
  }

  /*
    Activate the guard to release mdl lock to the savepoint and commit
    transaction on any return path from this function.
  */
  Transaction_Resources_Guard transaction_guard{thd, saved_sql_mode};

  if (find_sys_trigger_by_name(event_table, thd->lex->spname))
  {
    if (thd->lex->create_info.if_not_exists())
    {
      my_ok(thd);
      return false;
    }

    report_trg_already_exist_error(thd->lex->spname);
    return true;
  }

  if (store_trigger_metadata(thd, thd->lex, event_table, thd->lex->sphead,
                             thd->lex->trg_chistics, saved_sql_mode))
    return true;

  char definer_buf[USER_HOST_BUFF_SIZE];
  LEX_CSTRING definer;
  thd->lex->definer->set_lex_string(&definer, definer_buf);

  thd->lex->sphead->set_definer(definer.str, definer.length);
  thd->lex->sphead->init_psi_share();

  /*
    First move to 3 bits by right to ignore DML trigger events.
    After that events_mask must be shifted to 1 bit by right to get a value
    compatible with Event_parse_data::enum_kind
  */
  trg_all_events_set events_mask= thd->lex->trg_chistics.events >> 3;

  events_mask = events_mask << 1;

  Sys_trigger *sys_trg=
    new (thd_for_sys_triggers->mem_root) Sys_trigger(thd_for_sys_triggers,
                                                     thd->lex->sphead);
  register_system_triggers(
    sys_trg, thd->lex->trg_chistics.action_time,
    Event_parse_data::enum_kind(events_mask));

  /*
    Stop destroy of sp_head for just handled CREATE TRIGGER statement
    that else would happened on running
      mysql_parse() -> THD::end_statement() -> lex_end() ->
       lex_end_nops() -> sp_head::destroy
  */
  thd->lex->sphead= nullptr;
  my_ok(thd);
  return false;
}


/**
  Check for presence a trigger with specified name. Do it in a separate
  transaction to work under LOCK TABLE

  @param thd  Thread handler
  @param spname  the trigger name to check for existence

  @return false in case there is no trigger with specified name,
          else return true
*/

bool find_sys_trigger_by_name(THD *thd, sp_name *spname)
{
  start_new_trans new_trans(thd);
  TABLE_LIST event_table;

  Open_tables_backup open_tables_state_backup;
  thd->reset_n_backup_open_tables_state(&open_tables_state_backup);

  event_table.init_one_table(&MYSQL_SCHEMA_NAME,
                             &event_table_name, 0, TL_READ);

  if (open_system_tables_for_read(thd, &event_table))
  {
    new_trans.restore_old_transaction();
    return true;
  }

  bool ret= find_sys_trigger_by_name(event_table.table, thd->lex->spname);

  thd->commit_whole_transaction_and_close_tables();

  return ret;
}


/**
  Handle the statement DROP TRIGGER for system triggers,
  such as ON STARTUP, ON SHUTDOWN. Trigger name is specified by
  thd->lex->spname.

  @param thd  Thread handler
  @param[out] no_ddl_trigger_found  true in case there is no trigger with
                                    the specified name, else false

  @return false on success, true on error
  @note the case `trigger not found` is considered as a success result
         with setting the @param no_ddl_trigger_found to the true value
*/

bool mysql_drop_sys_or_ddl_trigger(THD *thd, bool *no_ddl_trigger_found)
{
  MDL_request mdl_request;

  /*
    Note that once we will have check for TRIGGER privilege in place we won't
    need second part of condition below, since check_access() function also
    checks that db is specified.
  */
  if (!thd->lex->spname->m_db.length)
  {
    my_error(ER_NO_DB_ERROR, MYF(0));
    return true;
  }

  *no_ddl_trigger_found= false;

  /* Protect against concurrent create/drop */
  MDL_REQUEST_INIT(&mdl_request, MDL_key::TRIGGER,
                   thd->lex->spname->m_db.str,
                   thd->lex->spname->m_name.str,
                   MDL_EXCLUSIVE, MDL_EXPLICIT);
  if (thd->mdl_context.acquire_lock(&mdl_request,
                                    thd->variables.lock_wait_timeout))
    return true;

  /*
    Check whether the trigger does exist. It is performed by a separate
    function that opens the table mysql.event on reading within
    a new independent transaction to handle the case when DROP TRIGGER
    be executed in locked_tables_mode and there is no a trigger with
    the supplied name.
  */
  if (!find_sys_trigger_by_name(thd, thd->lex->spname))
  {
    thd->mdl_context.release_lock(mdl_request.ticket);

    *no_ddl_trigger_found= true;
    return false;
  }

  /* Reset sql_mode during data dictionary operations. */
  sql_mode_t saved_mode= thd->variables.sql_mode;
  thd->variables.sql_mode= 0;

  TABLE *event_table;
  if (Event_db_repository::open_event_table(thd, TL_WRITE, &event_table))
  {
    thd->mdl_context.release_lock(mdl_request.ticket);

    return true;
  }

  Transaction_Resources_Guard transaction_guard{thd, saved_mode};

  if (!find_sys_trigger_by_name(event_table, thd->lex->spname))
  {
    /*
      The use case 'trigger not found' is handled at the function
      mysql_create_or_drop_trigger() if there is no a DML trigger
      with specified name
    */
    *no_ddl_trigger_found= true;
    if (mdl_request.ticket)
      thd->mdl_context.release_lock(mdl_request.ticket);

    return false;
  }

  int ret= event_table->file->ha_delete_row(event_table->record[0]);
  if (ret)
    event_table->file->print_error(ret, MYF(0));
  else
  {
    unregister_trigger(thd->lex->spname);
    my_ok(thd);
  }

  thd->mdl_context.release_lock(mdl_request.ticket);

  return ret;
}

bool Sys_trigger::execute()
{
  List<Item> empty_item_list;
  bool ret= m_sp->execute_procedure(m_thd, &empty_item_list);

  m_thd->end_statement();
  m_thd->cleanup_after_query();
  m_thd->reset_query();

  return ret;
}


/**
  Convert the value of trigger kind mask into a comma separated strings value

  @param base_event_names - array of names for every event type retrieved
                            from the column mysq.event.kind. These names are
                            in the following order:
                              base_event_names[0] == "SCHEDULE"
                              base_event_names[1] == "STARTUP"
                              base_event_names[2] == "SHUTDOWN"
                              base_event_names[3] == "LOGON",
                              base_event_names[4] == "LOGOFF"
                              base_event_names[5] == "DDL"
  @param [out] set_of_events - text representation of trigger events bitmap
                               value
  @param trg_kind  - bitmap containing turned on bit for every event the system
                     trigger is created for. Events mask numbering is started
                     from the bit number 1, bit number 0 is reserved for the
                     special value 'SCHEDULE'. The parameter trg_kind never
                     contains a value with the 0 bit set.

  @return LEX_CSTRING containing text representation of trigger events bitmap
                      value
*/

static LEX_CSTRING events_to_string(const LEX_CSTRING base_event_names[],
                                    char *set_of_events,
                                    const Event_parse_data::enum_kind trg_kind)
{
  size_t offset= 0;

  /*
    Shift right by one bit since the bit for "SCHEDULE" is never set in
    the argument trg_kind
  */
  ulonglong kind= ((ulonglong )trg_kind) >> 1;
  for (int idx= 1; kind != 0; kind= kind >> 1, idx++)
  {
    if (kind & 0x1)
      offset+= sprintf(set_of_events + offset, "%s,",
                       base_event_names[idx].str);
  }
  set_of_events[offset - 1]= 0;

  return LEX_CSTRING{set_of_events, offset - 1};
}


static const LEX_CSTRING base_event_time[]= {
  LEX_CSTRING{STRING_WITH_LEN("BEFORE")},
  LEX_CSTRING{STRING_WITH_LEN("AFTER")}
};

static const LEX_CSTRING base_event_names[]= {
  LEX_CSTRING{STRING_WITH_LEN("SCHEDULE")},
  LEX_CSTRING{STRING_WITH_LEN("STARTUP")},
  LEX_CSTRING{STRING_WITH_LEN("SHUTDOWN")},
  LEX_CSTRING{STRING_WITH_LEN("LOGON")},
  LEX_CSTRING{STRING_WITH_LEN("LOGOFF")}
};

static const int max_event_names_length =
  (base_event_names[0].length + 1) +
  (base_event_names[1].length + 1) +
  (base_event_names[2].length + 1) +
  (base_event_names[3].length + 1) +
  (base_event_names[4].length + 1);


/**
  Based on input parameter values, assemble the CREATE TRIGGER statement
  used to create the system trigger.

  @param       thd               Thread handler
  @param[out]  create_trg_stmt   Where to store the resulted
                                 CREATE TRIGGER statement
  @param       trg_definer       Definer used for trigger creation
  @param       trg_name          Trigger name
  @param       trg_kind          Trigger kind (ON STARTUP, ON SHUTDOWN, etc)
  @param       trg_when          Trigger event time (BEFORE, AFTER)
  @param       body              Trigger body

  @return false on success, true on error
*/

static bool reconstruct_create_trigger_stmt(
  THD *thd, String *create_trg_stmt,
  const LEX_STRING &trg_definer,
  const LEX_STRING &trg_name,
  Event_parse_data::enum_kind trg_kind,
  trg_action_time_type trg_when,
  const LEX_STRING &body)
{
  static const LEX_CSTRING prefix{STRING_WITH_LEN("CREATE DEFINER=")};

  static const LEX_CSTRING trigger_clause{STRING_WITH_LEN(" TRIGGER ")};

  char *buffer;
  size_t buffer_len= prefix.length + trg_definer.length +
                     trigger_clause.length + trg_name.length + 1 +
                     base_event_time[trg_when].length + 1 +
                     max_event_names_length + 1 +
                     body.length + 1;
  buffer= thd->alloc(buffer_len);
  if (buffer == nullptr)
    return true;

  create_trg_stmt->set(buffer, buffer_len, system_charset_info);
  create_trg_stmt->length(0);

  create_trg_stmt->append(STRING_WITH_LEN("CREATE "));
  create_trg_stmt->append_name_value(LEX_CSTRING{STRING_WITH_LEN("DEFINER")},
                                     trg_definer);
  create_trg_stmt->append(trigger_clause);
  create_trg_stmt->append(trg_name);
  create_trg_stmt->append(' ');
  create_trg_stmt->append(base_event_time[trg_when]);
  create_trg_stmt->append(' ');
  char event_names_buf[max_event_names_length + 1];
  create_trg_stmt->append(events_to_string(base_event_names, event_names_buf,
                                           trg_kind));
  create_trg_stmt->append(' ');
  create_trg_stmt->append(body, system_charset_info);

  return false;
}


/**
  RAII class to restore original lex object on return from the function
  compile_trigger_stmt().
*/

class Trigger_Compilation_Resources_Guard
{
public:
  explicit Trigger_Compilation_Resources_Guard(THD *thd)
  : m_thd{thd}, m_lex{thd->lex}
  {}
  ~Trigger_Compilation_Resources_Guard()
  {
    m_thd->lex= m_lex;
  }
private:
  THD *m_thd;
  LEX *m_lex;
};


/**
  Parse the CREATE TRIGGER statement and return sp_head for compiled trigger.

  @param thd  Thread context
  @param db_name  database name
  @param create_trigger_stmt  CREATE TRIGGER statement to compile
  @param ctx  Trigger creation context
  @param[out] parse_error  output parameter for storing result of
                           parsing the statement: false on success,
                           true on error

  @return sp_head object on success, nullptr on error
*/

static sp_head *compile_trigger_stmt(THD *thd,
                                     const LEX_CSTRING &db_name,
                                     const String *create_trigger_stmt,
                                     Stored_program_creation_ctx *ctx,
                                     bool *parse_error)
{
  LEX lex;
  Parser_state parser_state;

  Trigger_Compilation_Resources_Guard guard{thd};
  thd->set_db(&db_name);
  thd->lex= &lex;

  if (parser_state.init(thd, (char*) create_trigger_stmt->ptr(),
                        create_trigger_stmt->length()))
    return nullptr;

  lex_start(thd);
  thd->spcont= NULL;
  lex.trg_chistics.events= TRG_EVENT_UNKNOWN;
  lex.trg_chistics.action_time= TRG_ACTION_MAX;

  *parse_error= parse_sql(thd, &parser_state, ctx);

  if (*parse_error)
    return nullptr;

  sp_head *sphead= thd->lex->sphead;
  if (sphead != nullptr)
    sphead->init_psi_share();

  thd->lex->sphead= nullptr;

  lex_end(&lex);

  return sphead;
}


/**
  Based on trigger's meta-data retrieved from the table mysql.event,
  reconstruct the original CREATE TRIGGER statement, parse it and
  create an instance of the class Sys_trigger that encapsulate all
  trigger-specific stuff including sp_head.

  @param thd          Thread handler
  @param db_name      database name where the trigger is defined
  @param trg_name     trigger name
  @param trg_definer  trigger definer
  @param trg_kind     trigger event type (ON STARTUP, ON SHUTDOWN, etc)
  @param trg_when     time (BEFORE, AFTER) when the trigger fired
  @param trg_body     trigger body
  @param sql_mode     sql_mode used on trigger creation
  @param ctx          creation context
  @param[out]         output variable to store parsing result:
                      false on success, true on error

  @return a pointer to the instance of the class Sys_trigger on success,
          null_ptr on error
*/

static Sys_trigger *
instantiate_sys_trigger(THD *thd,
                        const LEX_STRING &db_name,
                        const LEX_STRING &trg_name,
                        const LEX_STRING &trg_definer,
                        Event_parse_data::enum_kind trg_kind,
                        trg_action_time_type trg_when,
                        const LEX_STRING &trg_body,
                        sql_mode_t sql_mode,
                        Stored_program_creation_ctx *ctx,
                        bool *parse_error)
{
  String create_trigger_stmt;

  /*
    The method instantiate_sys_trigger() is called before run_main_loop(), so
    at the time of calling there is no active connections and as a consequence
    no one can drop a system trigger being loaded from the table mysql.event.
    Therefore no need to take mdl lock on system trigger names.
  */

  sql_mode_t save_sql_mode= thd->variables.sql_mode;

  thd->variables.sql_mode= sql_mode;

  /*
    Reconstruct an original CREATE TRIGGER statement based on metadata
    retrieved for the trigger from the table mysql.event.
  */
  if (reconstruct_create_trigger_stmt(thd, &create_trigger_stmt,
                                      trg_definer, trg_name,
                                      trg_kind, trg_when, trg_body))
    return nullptr;

  Sys_trigger *sys_trg= nullptr;

  sp_head *sp= compile_trigger_stmt(thd, db_name, &create_trigger_stmt, ctx,
                                    parse_error);
  if (sp)
  {
    sys_trg=
      new (thd_for_sys_triggers->mem_root) Sys_trigger(thd_for_sys_triggers,
                                                       sp);
    sp->set_definer(trg_definer.str, trg_definer.length);
  }
  thd->variables.sql_mode= save_sql_mode;

  return sys_trg;
}

static class Stored_program_creation_ctx *creation_ctx= nullptr;


/**
  Do loading of a system trigger from the next record fetched from the table
  mysql.event for event types different from SCHEDULE_EVENT, that is from
  the record storing system trigger's metadata

  @param thd  Thread handler
  @param event_table  Opened table mysql.event
  @param[out] db_name  database name
  @param[out] trg_name  trigger name
  @param[out] trg_body  trigger body
  @param[out] trg_definer  trigger definer
  @param[out] sql_mode  sql_mode used on trigger creation
  @param[out] trg_when  when to fire the trigger (BEFORE, AFTER)

  @return false on success, true on error
*/

static bool load_trigger_metadata(THD *thd, TABLE *event_table,
                                  LEX_STRING *db_name,
                                  LEX_STRING *trg_name,
                                  LEX_STRING *trg_body,
                                  LEX_STRING *trg_definer,
                                  sql_mode_t *sql_mode,
                                  trg_action_time_type *trg_when)
{
  *db_name=
    event_table->field[ET_FIELD_DB]->val_lex_string_strmake(thd->mem_root);
  if (db_name->str == nullptr)
    return true;

  *trg_name=
    event_table->field[ET_FIELD_NAME]->val_lex_string_strmake(thd->mem_root);
  if (trg_name->str == nullptr)
    return true;

  *trg_body=
    event_table->field[ET_FIELD_BODY]->val_lex_string_strmake(thd->mem_root);
  if (trg_body->str == nullptr)
    return true;

  *trg_definer=
    event_table->field[ET_FIELD_DEFINER]->val_lex_string_strmake(thd->mem_root);

  if (trg_definer->str == nullptr)
    return true;

  *sql_mode= (sql_mode_t) event_table->field[ET_FIELD_SQL_MODE]->val_int();

  if (load_creation_context_for_sys_trg(thd, thd->mem_root,
                                        db_name->str, trg_name->str,
                                        event_table,
                                        &creation_ctx))
    return true;

  /*
    trigger event time is stored in the mysql.event table in the column
    `when` declared as enum('BEFORE','AFTER'). So, enumerators has
    the following values: 1 for `BEFORE`, 2 for `AFTER`.
    On the other hand, the enum trg_action_time_type has values starting
    from 0, so adjust values restored from the table mysql.event before using
    them for calculations.
  */
  *trg_when=
    (trg_action_time_type)(event_table->field[ET_FIELD_WHEN]->val_int() - 1);

  return false;
}


/**
  Load system triggers from the table mysql.event

  @param thd  Thread handler

  @return false on success, true on error
*/

static bool load_system_triggers(THD *thd)
{
  TABLE *event_table;

  if (Event_db_repository::open_event_table(thd, TL_WRITE, &event_table))
    return true;

  READ_RECORD read_record_info;
  if (init_read_record(&read_record_info, thd, event_table,
                       nullptr, nullptr, 0, 1, false))
  {
    close_thread_tables(thd);
    return true;
  }

  bool ret= false;

  while (!(read_record_info.read_record()))
  {
    Event_parse_data::enum_status trg_status;
    sql_mode_t sql_mode;

    Event_parse_data::enum_kind trg_kind=
      (Event_parse_data::enum_kind)event_table->field[ET_FIELD_KIND]->val_int();

    if (trg_kind == Event_parse_data::SCHEDULE_EVENT)
      continue;

    trg_status= (Event_parse_data::enum_status)
      event_table->field[ET_FIELD_STATUS]->val_int();
    if (trg_status != Event_parse_data::ENABLED)
      continue;

    LEX_STRING db_name, trg_name, trg_body, trg_definer;
    trg_action_time_type trg_when;
    if (load_trigger_metadata(thd, event_table, &db_name, &trg_name,
                              &trg_body, &trg_definer, &sql_mode, &trg_when))
    {
      ret= true;
      break;
    }

    bool parse_error= false;
    Sys_trigger *sys_trg=
      instantiate_sys_trigger(thd, db_name, trg_name,
                              trg_definer, trg_kind, trg_when,
                              trg_body, sql_mode, creation_ctx,
                              &parse_error);

    if (parse_error)
      /*
        Skip triggers containing non-parsable body, probably updated
        intentionally for some records in the table mysql.event. Doing this way
        we allow to start server even on presence of unparseable triggers
      */
      continue;

    if (sys_trg == nullptr)
    {
      /* OOM error happened */
      ret= true;
      break;
    }

    register_system_triggers(sys_trg, trg_when, trg_kind);
  }

  end_read_record(&read_record_info);
  close_mysql_tables(thd);

  return ret;
}


/**
  First, load system triggers from the table mysql.event and then run
  ON STARTUP triggers if ones present.
*/

bool run_after_startup_triggers()
{
  if (opt_bootstrap || opt_readonly)
    return false;

  bool stack_top;

  original_thd= current_thd;

  thd_for_sys_triggers= new THD{0};
  thd_for_sys_triggers->thread_stack= (char*) &stack_top;
  thd_for_sys_triggers->store_globals();
  thd_for_sys_triggers->set_query_inner(
    (char*) STRING_WITH_LEN("load_system_triggers"),
    default_charset_info);
  thd_for_sys_triggers->set_time();

  /*
    First, load all available system triggers from the table mysql.event and
    store them in the two dimensional array based on trigger's action time and
    event type. Any memory allocation is performed on the memory root of
    thd_for_sys_triggers.
  */
  if (load_system_triggers(thd_for_sys_triggers))
  {
    delete thd_for_sys_triggers;

    return true;
  }

  /*
    Then get a list of AFTER STARTUP triggers and execute them one by one
  */
  Sys_trigger *trg=
      get_trigger_by_type(TRG_ACTION_AFTER, TRG_EVENT_STARTUP);
  while (trg)
  {
    /*
      Ignore errors that could happen on running any of 'on startup' triggers
      to start the server regardless of possible trigger errors
    */
    (void)trg->execute();

    trg= trg->next;
  }

  thd_for_sys_triggers->thread_stack= nullptr;
  set_current_thd(original_thd);
  return false;
}


/**
  System triggers are whole instance-wide, therefore they should be destroyed
  just before database server shutdown
*/
static void destroy_sys_triggers()
{
  for (int i=0; i< TRG_ACTION_MAX; i++)
    for (int j= 0; j< TRG_SYS_EVENT_MAX - TRG_EVENT_STARTUP; j++)
    {
      Sys_trigger *sys_trg= sys_triggers[i][j];

      while (sys_trg)
      {
        Sys_trigger *next_trg= sys_trg->next;
        sys_trg->destroy();
        sys_trg= next_trg;
      }

      sys_triggers[i][j]= nullptr;
    }
}


/**
  Run ON SHUTDOWN triggers
*/

void run_before_shutdown_triggers()
{
  if (opt_bootstrap || opt_readonly)
    return;

  bool stack_top;

  original_thd= current_thd;
  set_current_thd(thd_for_sys_triggers);

  thd_for_sys_triggers->thread_stack= (char*) &stack_top;

  Sys_trigger *trg=
    get_trigger_by_type(TRG_ACTION_BEFORE, TRG_EVENT_SHUTDOWN);
  while (trg)
  {
    (void)trg->execute();
    trg= trg->next;
  }

  destroy_sys_triggers();
  delete thd_for_sys_triggers;
}


static bool send_show_create_trigger_result(
  THD *thd, MEM_ROOT *mem_root,
  Protocol *protocol,
  const LEX_CSTRING &trg_name,
  const LEX_CSTRING &trg_sql_mode,
  const LEX_CSTRING &trg_create_sql_stmt,
  MYSQL_TIME created,
  CHARSET_INFO *client_cs,
  CHARSET_INFO *connection_cl,
  CHARSET_INFO *db_cl)
{
  if (send_show_create_trigger_metadata(thd, mem_root, protocol,
                                        trg_sql_mode, trg_create_sql_stmt))
    return true;

  protocol->prepare_for_resend();

  protocol->store(trg_name.str,
                  trg_name.length,
                  system_charset_info);

  protocol->store(trg_sql_mode.str,
                  trg_sql_mode.length,
                  system_charset_info);

  protocol->store(trg_create_sql_stmt.str,
                  trg_create_sql_stmt.length,
                  client_cs);

  protocol->store(&client_cs->cs_name, system_charset_info);

  protocol->store(&connection_cl->coll_name, system_charset_info);

  protocol->store(&db_cl->coll_name, system_charset_info);

  protocol->store_datetime(&created, 2);

  bool ret= protocol->write();

  if (!ret)
    my_eof(thd);

  return ret;
}


/**
  Implementation of SHOW CREATE TRIGGER statement for system triggers

  @param thd       Thread context
  @param trg_name  Trigger name specified for SHOW CREATE TRIGGER statement

  @return false on success, true on error
*/

bool show_create_sys_trigger(THD *thd, const sp_name *trg_name)
{
  TABLE *event_table;
  sql_mode_t saved_mode= thd->variables.sql_mode;
  thd->variables.sql_mode= 0;

  if (Event_db_repository::open_event_table(thd, TL_READ, &event_table))
  {
    thd->variables.sql_mode= saved_mode;

    return true;
  }

  Transaction_Resources_Guard transaction_guard{thd, saved_mode};

  /*
    Set up the search key and look up the record in mysql.event for
    the trigger.
  */
  if (!find_sys_trigger_by_name(event_table, trg_name))
  {
    my_error(ER_TRG_DOES_NOT_EXIST, MYF(0));
    return true;
  }

  Event_parse_data::enum_kind trg_kind=
    (Event_parse_data::enum_kind)event_table->field[ET_FIELD_KIND]->val_int();

  /*
    Trigger doesn't exist in case the record is for the real event
  */
  if (trg_kind == Event_parse_data::SCHEDULE_EVENT)
  {
    my_error(ER_TRG_DOES_NOT_EXIST, MYF(0));
    return true;
  }

  LEX_STRING db_name, trg_body, trg_definer;
  LEX_STRING trigger_name{(char*)trg_name->m_name.str,
                          trg_name->m_name.length};
  sql_mode_t sql_mode;
  trg_action_time_type trg_when;
  if (load_trigger_metadata(thd, event_table, &db_name, &trigger_name,
                            &trg_body, &trg_definer, &sql_mode, &trg_when))
    return true;


  String create_trg_stmt;

  if (reconstruct_create_trigger_stmt(thd, &create_trg_stmt, trg_definer,
                                      trigger_name, trg_kind, trg_when,
                                      trg_body))
    return true;

  LEX_CSTRING trg_sql_mode_str;
  sql_mode_string_representation(thd, sql_mode, &trg_sql_mode_str);

  CHARSET_INFO *client_cs;
  CHARSET_INFO *connection_cl;
  CHARSET_INFO *db_cl;

  if (load_charset(thd, thd->mem_root,
                   event_table->field[ET_FIELD_CHARACTER_SET_CLIENT],
                   thd->variables.character_set_client,
                   &client_cs))
    return true;

  if (load_collation(thd, thd->mem_root,
                     event_table->field[ET_FIELD_COLLATION_CONNECTION],
                     thd->variables.collation_connection,
                     &connection_cl))
    return true;

  if (load_collation(thd, thd->mem_root,
                     event_table->field[ET_FIELD_DB_COLLATION],
                     NULL,
                     &db_cl))
    return true;

  if (!db_cl)
    db_cl= get_default_db_collation(thd, trg_name->m_db.str);

  ulonglong created= event_table->field[ET_FIELD_CREATED]->val_int();
  MYSQL_TIME created_timestamp;
  int not_used=0;
  number_to_datetime_or_date(created, 0, &created_timestamp, 0, &not_used);

  if (send_show_create_trigger_result(thd, thd->mem_root, thd->protocol,
                                      trg_name->m_name,
                                      trg_sql_mode_str,
                                      create_trg_stmt.to_lex_cstring(),
                                      created_timestamp,
                                      client_cs, connection_cl, db_cl))
    return true;

  return false;
}


/**
  Fill a record of information_schema.triggers for a system trigger

  @param trg_name  the trigger name
  @param table     TABLE object for the table information_schema.triggers
  @param db_name   nullptr for system ON STARTUP/ON SHUTDOWN triggers,
                   else the name of database where the trigger is defined
  @param sql_mode  sql_mode used on trigger creation
  @param definer   definer used on trigger creation
  @param trg_body  body of the trigger
  @param trg_time  trigger event time (BEFORE or AFTER)
  @param created_timestamp  date/time when the trigger created
  @param client_cs_name  client character set name
  @param connection_cs_name  connection character set name
  @param db_cs_name  database character set name

  @return false on success, true on error
*/

static bool store_sys_trigger(THD *thd, const LEX_CSTRING &trg_name,
                              TABLE *table, const LEX_CSTRING *db_name,
                              sql_mode_t sql_mode,
                              const LEX_CSTRING &definer,
                              const LEX_CSTRING &trg_body,
                              const LEX_CSTRING &trg_time,
                              const LEX_CSTRING &trg_event,
                              const MYSQL_TIME &created_timestamp,
                              const LEX_CSTRING &client_cs_name,
                              const LEX_CSTRING &connection_cs_name,
                              const LEX_CSTRING &db_cs_name)
{
  CHARSET_INFO *cs= system_charset_info;

  restore_record(table, s->default_values);
  /* TRIGGER_CATALOG */
  table->field[0]->store(STRING_WITH_LEN("def"), cs);
  /* TRIGGER_SCHEMA */
  if (db_name)
    table->field[1]->store(*db_name, cs);
  else
    table->field[1]->set_null();
  /* TRIGGER_NAME */
  table->field[2]->store(trg_name, cs);
  /* EVENT_MANIPULATION for a system trigger (STARTUP, SHUTDOWN, DDL, etc) */
  table->field[3]->store(trg_event, cs);
  /* EVENT_OBJECT_CATALOG */
  table->field[4]->store(STRING_WITH_LEN("def"), cs);
  /* EVENT_OBJECT_SCHEMA */
  table->field[5]->set_null();
  /* EVENT_OBJECT_TABLE */
  table->field[6]->set_null();
  /* ACTION_ORDER */
  table->field[7]->set_null(); // TODO: adjust!
  /* ACTION_CONDITION */
  table->field[8]->set_null();
  /* ACTION_STATEMENT */
  table->field[9]->store(trg_body, cs);

  /* ACTION_ORIENTATION */
  table->field[10]->store(STRING_WITH_LEN("STATEMENT"), cs); // TODO: adjust
  /* ACTION_TIMING (BEFORE, AFTER) */
  table->field[11]->store(trg_time, cs);
  /* ACTION_REFERENCE_OLD_TABLE */
  table->field[12]->set_null();
  /* ACTION_REFERENCE_NEW_TABLE */
  table->field[13]->set_null();
  /* ACTION_REFERENCE_OLD_ROW */
  table->field[14]->set_null();
  /* ACTION_REFERENCE_NEW_ROW */
  table->field[15]->set_null();

  /* CREATED */
  table->field[16]->set_notnull();
  table->field[16]->store_time_dec(&created_timestamp, 2);

  LEX_CSTRING sql_mode_rep;
  sql_mode_string_representation(thd, sql_mode, &sql_mode_rep);
  /* SQL_MODE */
  table->field[17]->store(sql_mode_rep.str, sql_mode_rep.length, cs);
  /* DEFINER */
  table->field[18]->store(definer, cs);
  /* CHARACTER_SET_CLIENT */
  table->field[19]->store(&client_cs_name, cs);
  /* COLLATION_CONNECTION */
  table->field[20]->store(&connection_cs_name, cs);
  /* DATABASE_COLLATION */
  table->field[21]->store(&db_cs_name, cs);

  return schema_table_store_record(thd, table);
}


/**
  Fill in the table information_schema.triggers with data about existing
  system triggers based on the data stored in the table mysql.event.

  @param thd     thread handler
  @param tables  an instance of the struct TABLE_LIST for the table
                 information_schema.triggers

  @return false on success, true on error
*/

bool fill_schema_triggers_from_mysql_events(THD *thd, TABLE_LIST *tables)
{
  Open_tables_backup open_tables_state_backup;
  TABLE_LIST event_table;

  start_new_trans new_trans(thd);

  thd->reset_n_backup_open_tables_state(&open_tables_state_backup);

  event_table.init_one_table(&MYSQL_SCHEMA_NAME, &event_table_name,
                             0, TL_READ);

  if (open_system_tables_for_read(thd, &event_table))
  {
    new_trans.restore_old_transaction();

    return true;
  }

  READ_RECORD read_record_info;
  if (init_read_record(&read_record_info, thd, event_table.table,
                       nullptr, nullptr, 0, 1, false))
  {
    thd->commit_whole_transaction_and_close_tables();
    new_trans.restore_old_transaction();

    return true;
  }

  bool ret= false;

  Event_parse_data::enum_status trg_status;
  sql_mode_t sql_mode;

  while (!(read_record_info.read_record()))
  {
    TABLE *event= event_table.table;

    Event_parse_data::enum_kind trg_kind=
      (Event_parse_data::enum_kind)event->field[ET_FIELD_KIND]->val_int();

    if (trg_kind == Event_parse_data::SCHEDULE_EVENT)
      continue;

    trg_status= (Event_parse_data::enum_status)
      event->field[ET_FIELD_STATUS]->val_int();
    if (trg_status != Event_parse_data::ENABLED)
      continue;

    const Lex_cstring db_name{
      event->field[ET_FIELD_DB]->val_lex_string_strmake(thd->mem_root)};
    if (db_name.str == nullptr)
    {
      ret= true;
      break;
    }

    const Lex_cstring trg_name{
      event->field[ET_FIELD_NAME]->val_lex_string_strmake(
        thd->mem_root)};
    if (trg_name.str == nullptr)
    {
      ret= true;
      break;
    }

    const Lex_cstring trg_body{
      event->field[ET_FIELD_BODY]->val_lex_string_strmake(
        thd->mem_root)};
    if (trg_body.str == nullptr)
    {
      ret= true;
      break;
    }

    const Lex_cstring trg_definer(
      event->field[ET_FIELD_DEFINER]->val_lex_string_strmake(
        thd->mem_root));

    if (trg_definer.str == nullptr)
    {
      ret= true;
      break;
    }

    sql_mode= (sql_mode_t) event->field[ET_FIELD_SQL_MODE]->val_int();
    /*
      trigger event time is stored in the mysql.event table in the column
      `when` declared as enum('BEFORE','AFTER'). So, enumerators has
      the following values: 1 for `BEFORE`, 2 for `AFTER`.
      On the other hand, the enum trg_action_time_type has values starting
      from 0, so adjust values restored from the table mysql.event before using
      them for calculations.
    */
    trg_action_time_type trg_when=
      (trg_action_time_type)(event->field[ET_FIELD_WHEN]->val_int() - 1);

    const Lex_cstring client_cs_name(
      event->field[ET_FIELD_CHARACTER_SET_CLIENT]->val_lex_string_strmake(
        thd->mem_root));

    if (client_cs_name.str == nullptr)
    {
      ret= true;
      break;
    }

    const Lex_cstring connection_cs_name(
      event->field[ET_FIELD_COLLATION_CONNECTION]->val_lex_string_strmake(
        thd->mem_root));

    if (connection_cs_name.str == nullptr)
    {
      ret= true;
      break;
    }

    const Lex_cstring db_cs_name(
      event->field[ET_FIELD_DB_COLLATION]->val_lex_string_strmake(
        thd->mem_root));

    if (connection_cs_name.str == nullptr)
    {
      ret= true;
      break;
    }

    ulonglong created= event->field[ET_FIELD_CREATED]->val_int();
    MYSQL_TIME created_timestamp;
    int not_used=0;
    number_to_datetime_or_date(created, 0, &created_timestamp, 0, &not_used);

    char event_names_buf[max_event_names_length + 1];

    ret= store_sys_trigger(thd, trg_name,
                           tables->table,
                           /*
                             Although mysql.event has NOT NULL constraint for
                             the column mysql.db, use NULL for a database of
                             system triggers since they don't associate with
                             any database by its nature.
                           */
                           ((trg_kind == Event_parse_data::SYS_TRG_ON_STARTUP ||
                             trg_kind == Event_parse_data::SYS_TRG_ON_SHUTDOWN)
                           ? nullptr : &db_name),
                           sql_mode,
                           trg_definer,
                           trg_body,
                           base_event_time[trg_when],
                           events_to_string(base_event_names, event_names_buf,
                                            trg_kind),
                           created_timestamp,
                           client_cs_name,
                           connection_cs_name,
                           db_cs_name);
    if (ret)
      break;
  }

  end_read_record(&read_record_info);
  thd->commit_whole_transaction_and_close_tables();
  new_trans.restore_old_transaction();

  return ret;
}
