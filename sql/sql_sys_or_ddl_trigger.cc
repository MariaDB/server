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
#include "sql_parse.h"            // sp_process_definer
#include "sql_show.h"             // append_identifier
#include "sql_sys_or_ddl_trigger.h"
#include "sql_trigger.h"
#include "strfunc.h"              //set_to_string

/**
  Raise the error ER_TRG_ALREADY_EXISTS
*/

static void report_error(uint error_num, sp_name *spname)
{
  /*
    Report error in case there is a trigger on DML event with
    the same name as the system trigger we are going to create
  */
  char trigname_buff[FN_REFLEN];

  strxnmov(trigname_buff, sizeof(trigname_buff) - 1,
           spname->m_db.str, ".",
           spname->m_name.str, NullS);
  my_error(error_num, MYF(0), trigname_buff);
}


/**
  Check whether there is a trigger specified name on a DML event

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
    report_error(ER_TRG_ALREADY_EXISTS, spname);
    return true;
  }

  return false;
}


/**
  Search a system or ddl trigger by its name in the table mysql.event.

  @return false in case there is no trigger with specified name,
          else return true
*/

static bool find_sys_trigger_by_name(TABLE *event_table, sp_name *spname)
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

static bool store_trigger_metadata(THD *thd, LEX *lex, TABLE *event_table,
                                   sp_head *sphead,
                                   const st_trg_chistics &trg_chistics)
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

  if (fields[ET_FIELD_BODY]->store(sphead->m_body.str,
                                   sphead->m_body.length,
                                   system_charset_info))
  {
    my_error(ER_EVENT_STORE_FAILED, MYF(0),
             fields[ET_FIELD_BODY]->field_name.str, ret);
    return true;
  }

  /*
    trg_chistics.events has meaningful bits for every trigger events,
    that is for DML, DDL, system events. The table mysql.event declares
    the column `kind` as a set with the following values
      `kind` set('SCHEDULE','STARTUP','SHUTDOWN','LOGON','LOGOFF','DDL')
    Since the first value is special value `SCHEDULE`, move events value
    one bit left.
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

// Transaction_Resources_Guard
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
  sql_mode_t saved_mode= thd->variables.sql_mode;
  thd->variables.sql_mode= 0;

  TABLE *event_table;
  if (Event_db_repository::open_event_table(thd, TL_WRITE, &event_table))
  {
    thd->variables.sql_mode= saved_mode;

    return true;
  }
  /*
    Activate the guard to release mdl lock to the savepoint and commit
    transaction on any return path from this function.
  */
  Transaction_Resources_Guard mdl_savepoint_guard{thd, saved_mode};

  if (find_sys_trigger_by_name(event_table, thd->lex->spname))
  {
    if (thd->lex->create_info.if_not_exists())
      return false;

    report_error(ER_TRG_ALREADY_EXISTS, thd->lex->spname);
    return true;
  }

  if (store_trigger_metadata(thd, thd->lex, event_table, thd->lex->sphead,
                             thd->lex->trg_chistics))
    return true;

  my_ok(thd);
  return false;
}

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
  if (lock_object_name(thd, MDL_key::TRIGGER, thd->lex->spname->m_db,
                       thd->lex->spname->m_name))
    return true;

  /* Reset sql_mode during data dictionary operations. */
  sql_mode_t saved_mode= thd->variables.sql_mode;
  thd->variables.sql_mode= 0;

  TABLE *event_table;
  if (Event_db_repository::open_event_table(thd, TL_WRITE, &event_table))
    return true;

  Transaction_Resources_Guard mdl_savepoint_guard{thd, saved_mode};

  if (!find_sys_trigger_by_name(event_table, thd->lex->spname))
  {
    /*
      The use case 'trigger not found' is handled at the function
      mysql_create_or_drop_trigger() if there is no a DML trigger
      with specified name
    */
    *no_ddl_trigger_found= true;
    return false;
  }

  int ret= event_table->file->ha_delete_row(event_table->record[0]);
  if (ret)
    event_table->file->print_error(ret, MYF(0));
  else
    my_ok(thd);

  return ret;
}

static Sys_trigger*
sys_triggers[TRG_ACTION_MAX][TRG_SYS_EVENT_MAX - TRG_SYS_EVENT_MIN]= {{nullptr}};

static THD *thd_for_before_sys_triggers= nullptr;

Sys_trigger *
get_trigger_by_type(trg_action_time_type action_time,
                    trg_sys_event_type trg_type)
{
  return sys_triggers[action_time][trg_type - TRG_SYS_EVENT_MIN];
}

bool Sys_trigger::execute()
{
  List<Item> empty_item_list;
  bool ret= m_sp->execute_procedure(m_thd, &empty_item_list);

  return ret;
}

static LEX_CSTRING events_to_string(const LEX_CSTRING base_event_names[],
                                    char *set_of_events, ulonglong trg_kind)
{
  size_t offset= 0;

  for (int idx= 0; trg_kind != 0; trg_kind= trg_kind >> 1)
  {
    if (trg_kind & 0x1)
      idx++;
    else
      continue;

    offset+= sprintf(set_of_events + offset, "%s,", base_event_names[idx].str);
  }
  set_of_events[offset - 1]= 0;

  return LEX_CSTRING{set_of_events, offset - 1};
}

static bool reconstruct_create_trigger_stmt(THD *thd, String *create_trg_stmt,
                                            const LEX_CSTRING &trg_definer,
                                            const LEX_CSTRING &trg_name,
                                            ulonglong trg_kind,
                                            ulonglong trg_when,
                                            const LEX_CSTRING &body)
{
  static const LEX_CSTRING prefix{STRING_WITH_LEN("CREATE DEFINER=")};

  static const LEX_CSTRING trigger_clause{STRING_WITH_LEN(" TRIGGER ")};

  static constexpr LEX_CSTRING base_event_names[]= {
    LEX_CSTRING{STRING_WITH_LEN("STARTUP")},
    LEX_CSTRING{STRING_WITH_LEN("SHUTDOWN")},
    LEX_CSTRING{STRING_WITH_LEN("LOGON")},
    LEX_CSTRING{STRING_WITH_LEN("LOGOFF")}
  };

  static const LEX_CSTRING base_event_time[]= {
    LEX_CSTRING{STRING_WITH_LEN("BEFORE")},
    LEX_CSTRING{STRING_WITH_LEN("AFTER")}
  };

  static constexpr int max_event_names_length =
    (base_event_names[0].length + 1) +
    (base_event_names[1].length + 1) +
    (base_event_names[2].length + 1) +
    (base_event_names[3].length + 1);

  char *buffer;
  size_t buffer_len= prefix.length + trg_definer.length +
                     trigger_clause.length + trg_name.length + 1 +
                     base_event_time[trg_when - 1].length + 1 +
                     max_event_names_length +
                     body.length + 1;
  buffer= thd->alloc(buffer_len);
  if (buffer == nullptr)
    return true;

  // CREATE OR REPLACE TRIGGER IF NOT EXISTS trg BEFORE SHUTDOWN

  create_trg_stmt->set(buffer, buffer_len, system_charset_info);
  create_trg_stmt->length(0);

  create_trg_stmt->append(STRING_WITH_LEN("CREATE "));
  create_trg_stmt->append_name_value(LEX_CSTRING{STRING_WITH_LEN("DEFINER")},
                                     trg_definer);
  create_trg_stmt->append(trigger_clause);
  create_trg_stmt->append(trg_name);
  create_trg_stmt->append(' ');
  create_trg_stmt->append(base_event_time[trg_when - 1]);
  create_trg_stmt->append(' ');
  char event_names_buf[max_event_names_length];
  create_trg_stmt->append(events_to_string(base_event_names, event_names_buf,
                                           trg_kind));
  create_trg_stmt->append(' ');
  create_trg_stmt->append(body, system_charset_info);

  return false;
}


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

static sp_head *compile_trigger_stmt(THD *thd,
                                     const LEX_CSTRING &db_name,
                                     String *create_trigger_stmt,
                                     Stored_program_creation_ctx *ctx)
{
  LEX *old_lex= thd->lex;
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

  bool parse_error= parse_sql(thd, & parser_state, ctx);

  if (parse_error)
    return nullptr;

  sp_head *sphead= thd->lex->sphead;
  if (sphead != nullptr)
    sphead->init_psi_share();

  thd->lex->sphead= nullptr;

  lex_end(&lex);
  thd->lex= old_lex;

  return sphead;
}

static Sys_trigger *instantiate_sys_trigger(THD *thd,
                                            const LEX_CSTRING &db_name,
                                            const LEX_CSTRING &trg_name,
                                            const LEX_CSTRING &trg_definer,
                                            ulonglong trg_kind,
                                            ulonglong trg_when,
                                            const LEX_CSTRING &trg_body,
                                            sql_mode_t sql_mode,
                                            Stored_program_creation_ctx *ctx)
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

  sp_head *sp= compile_trigger_stmt(thd, db_name, &create_trigger_stmt, ctx);
  if (sp)
  {
    // TODO: Check whether it should be allocated on memory root!!!
    sys_trg= new Sys_trigger(thd_for_before_sys_triggers, sp);
    sp->set_definer(trg_definer.str, trg_definer.length);
  }
  thd->variables.sql_mode= save_sql_mode;

  return sys_trg;
}

static class Stored_program_creation_ctx *creation_ctx= nullptr;

static void register_trigger(Sys_trigger *sys_trg,
                             trg_action_time_type trg_when,
                             trg_all_events_set trg_kind)
{
  /*
    TRG_EVENT_STARTUP= TRG_SYS_EVENT_MIN, // 3 (1 << (3 - 1) ==
    TRG_EVENT_SHUTDOWN, // 4
    TRG_EVENT_LOGON, // 5
    TRG_EVENT_LOGOFF, // 6
    TRG_EVENT_DDL, // 7
    TRG_SYS_EVENT_MAX // 8
  */
  Sys_trigger *cur_trg= sys_triggers[trg_when - 1][trg_kind];

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
    sys_triggers[trg_when - 1][trg_kind]= sys_trg;
}

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
  trg_action_time_type trg_when;
  Event_parse_data::enum_status trg_status;
  sql_mode_t sql_mode;

  while (!(read_record_info.read_record()))
  {
    trg_all_events_set trg_kind=
      (trg_all_events_set)event_table->field[ET_FIELD_KIND]->val_int();

    if ((Event_parse_data::enum_kind)trg_kind ==
        Event_parse_data::SCHEDULE_EVENT)
      continue;

    trg_status= (Event_parse_data::enum_status)
      event_table->field[ET_FIELD_STATUS]->val_int();
    if (trg_status != Event_parse_data::ENABLED)
      continue;

    // Do loading of a system trigger from the record of mysq.event

    const Lex_cstring db_name{
      event_table->field[ET_FIELD_DB]->val_lex_string_strmake(thd->mem_root)};
    if (db_name.str == nullptr)
    {
      ret= true;
      break;
    }

    const Lex_cstring trg_name{
      event_table->field[ET_FIELD_NAME]->val_lex_string_strmake(
        thd->mem_root)};
    if (trg_name.str == nullptr)
    {
      ret= true;
      break;
    }

    const Lex_cstring trg_body{
      event_table->field[ET_FIELD_BODY]->val_lex_string_strmake(
        thd->mem_root)};
    if (trg_body.str == nullptr)
    {
      ret= true;
      break;
    }

    const Lex_cstring trg_definer(
      event_table->field[ET_FIELD_DEFINER]->val_lex_string_strmake(
        thd->mem_root));

    if (trg_definer.str == nullptr)
    {
      ret= true;
      break;
    }

    sql_mode= (sql_mode_t) event_table->field[ET_FIELD_SQL_MODE]->val_int();

    load_creation_context_for_sys_trg(thd, thd->mem_root,
                                      db_name.str, trg_name.str,
                                      event_table,
                                      &creation_ctx);
    trg_when=
      (trg_action_time_type)event_table->field[ET_FIELD_WHEN]->val_int();

    Sys_trigger *sys_trg=
      instantiate_sys_trigger(thd, db_name, trg_name,
                              trg_definer, trg_kind, trg_when,
                              trg_body, sql_mode, creation_ctx);

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
    trg_all_events_set trg_event_for_registering = TRG_EVENT_STARTUP;
    for (trg_all_events_set tk= trg_kind >> 1; tk != 0;
         tk= tk >>1, trg_event_for_registering++)
    {
      if (tk & 0x01)
        register_trigger(sys_trg->inc_ref_count(), trg_when,
                         trg_event_for_registering - TRG_EVENT_STARTUP);
    }
  }

  end_read_record(&read_record_info);
  close_mysql_tables(thd);

  return ret;
}

bool run_after_startup_triggers()
{
  if (opt_bootstrap)
    return false;

  bool stack_top;

  thd_for_before_sys_triggers= new THD{0};
  thd_for_before_sys_triggers->thread_stack= (char*) &stack_top;
  thd_for_before_sys_triggers->store_globals();
  thd_for_before_sys_triggers->set_query_inner(
    (char*) STRING_WITH_LEN("load_system_triggers"),
    default_charset_info);
  thd_for_before_sys_triggers->set_time();

  /*
    First, load all available system triggers from the table mysql.event and
    store them in the two dimensional array based on trigger's action time and
    event type.
  */
  if (load_system_triggers(thd_for_before_sys_triggers))
    return true;

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

  thd_for_before_sys_triggers->thread_stack= nullptr;

  return false;
}


static void destroy_sys_triggers()
{
  for (int i=0; i< TRG_ACTION_MAX; i++)
    for (int j= 0; j< TRG_SYS_EVENT_MAX - TRG_SYS_EVENT_MIN; j++)
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


void run_before_shutdown_triggers()
{
  if (opt_bootstrap)
    return;

  bool stack_top;

  thd_for_before_sys_triggers->thread_stack= (char*) &stack_top;

  Sys_trigger *trg=
        get_trigger_by_type(TRG_ACTION_BEFORE, TRG_EVENT_SHUTDOWN);
  while (trg)
  {
    (void)trg->execute();
    trg= trg->next;
  }

  destroy_sys_triggers();
  thd_for_before_sys_triggers->thread_stack= nullptr;
  delete thd_for_before_sys_triggers;
}
