#include"table.h"
#include"sql_lex.h"
#include"protocol.h"
#include"sql_plugin.h"
#include"sql_insert.h"

#define MYSQL_SERVER 1
#define MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL 0x0400

typedef enum
{
  WITHOUT_DB_NAME,
  WITH_DB_NAME
} enum_with_db_name;


class Internal_error_handler
{
protected:
  Internal_error_handler() : m_prev_internal_handler(NULL) {}

  virtual ~Internal_error_handler() {}

public:
  /**
    Handle a sql condition.
    This method can be implemented by a subclass to achieve any of the
    following:
    - mask a warning/error internally, prevent exposing it to the user,
    - mask a warning/error and throw another one instead.
    When this method returns true, the sql condition is considered
    'handled', and will not be propagated to upper layers.
    It is the responsability of the code installing an internal handler
    to then check for trapped conditions, and implement logic to recover
    from the anticipated conditions trapped during runtime.

    This mechanism is similar to C++ try/throw/catch:
    - 'try' correspond to <code>THD::push_internal_handler()</code>,
    - 'throw' correspond to <code>my_error()</code>,
    which invokes <code>my_message_sql()</code>,
    - 'catch' correspond to checking how/if an internal handler was invoked,
    before removing it from the exception stack with
    <code>THD::pop_internal_handler()</code>.

    @param thd the calling thread
    @param cond the condition raised.
    @return true if the condition is handled
  */
  virtual bool handle_condition(uint sql_errno, const char *sqlstate,
                                Sql_condition::enum_warning_level *level,
                                const char *msg, Sql_condition **cond_hdl)= 0;

private:
  Internal_error_handler *m_prev_internal_handler;
  friend class THD;
};

class Show_create_error_handler : public Internal_error_handler
{

  TABLE_LIST *m_top_view;
  bool m_handling;
  Security_context *m_sctx;

  char m_view_access_denied_message[MYSQL_ERRMSG_SIZE];
  char *m_view_access_denied_message_ptr;

public:
  /**
     Creates a new Show_create_error_handler for the particular security
     context and view.

     @thd Thread context, used for security context information if needed.
     @top_view The view. We do not verify at this point that top_view is in
     fact a view since, alas, these things do not stay constant.
  */
  explicit Show_create_error_handler(TABLE_LIST *top_view)
      : m_top_view(top_view), m_handling(FALSE),
        m_view_access_denied_message_ptr(NULL)
  {

    m_sctx= m_top_view->security_ctx;
  }

  /**
     Lazy instantiation of 'view access denied' message. The purpose of the
     Show_create_error_handler is to hide details of underlying tables for
     which we have no privileges behind ER_VIEW_INVALID messages. But this
     obviously does not apply if we lack privileges on the view itself.
     Unfortunately the information about for which table privilege checking
     failed is not available at this point. The only way for us to check is by
     reconstructing the actual error message and see if it's the same.
  */
  char *get_view_access_denied_message()
  {
    if (!m_view_access_denied_message_ptr)
    {
      m_view_access_denied_message_ptr= m_view_access_denied_message;
      //my_snprintf(m_view_access_denied_message, MYSQL_ERRMSG_SIZE,
      //            ER_THD(thd, ER_TABLEACCESS_DENIED_ERROR), "SHOW VIEW",
      //            m_sctx->priv_user, m_sctx->host_or_ip,
      //            m_top_view->get_table_name());
    }
    return m_view_access_denied_message_ptr;
  }

  bool handle_condition(uint sql_errno, const char * /* sqlstate */,
                        Sql_condition::enum_warning_level *level,
                        const char *message, Sql_condition ** /* cond_hdl */)
  {
    /*
       The handler does not handle the errors raised by itself.
       At this point we know if top_view is really a view.
    */
    if (m_handling || !m_top_view->view)
      return FALSE;

    m_handling= TRUE;

    bool is_handled;

    switch (sql_errno)
    {
    case ER_TABLEACCESS_DENIED_ERROR:
      if (!strcmp(get_view_access_denied_message(), message))
      {
        /* Access to top view is not granted, don't interfere. */
        is_handled= FALSE;
        break;
      }
      /* fall through */
    case ER_COLUMNACCESS_DENIED_ERROR:
    case ER_VIEW_NO_EXPLAIN: /* Error was anonymized, ignore all the same. */
    case ER_PROCACCESS_DENIED_ERROR:
      is_handled= TRUE;
      break;

    case ER_BAD_FIELD_ERROR:
    case ER_SP_DOES_NOT_EXIST:
    case ER_NO_SUCH_TABLE:
    case ER_NO_SUCH_TABLE_IN_ENGINE:
      /* Established behavior: warn if underlying tables, columns, or functions
         are missing. */
      //push_warning_printf(thd,Sql_condition::WARN_LEVEL_WARN, ER_VIEW_INVALID,
      //                    ER_THD(thd, ER_VIEW_INVALID),
      //                    m_top_view->get_db_name(),
      //                    m_top_view->get_table_name());
      is_handled= TRUE;
      break;

    default:
      is_handled= FALSE;
    }

    m_handling= FALSE;
    return is_handled;
  }
};

class Prelocking_strategy
{
public:
  virtual ~Prelocking_strategy() {}

  virtual void reset(){};
  virtual bool handle_routine(Query_tables_list *prelocking_ctx,
                              Sroutine_hash_entry *rt, sp_head *sp,
                              bool *need_prelocking)= 0;
  virtual bool handle_table( Query_tables_list *prelocking_ctx,
                            TABLE_LIST *table_list, bool *need_prelocking)= 0;
  virtual bool handle_view(Query_tables_list *prelocking_ctx,
                           TABLE_LIST *table_list, bool *need_prelocking)= 0;
  virtual bool handle_end() { return 0; };
};

enum enum_schema_tables get_schema_table_idx(ST_SCHEMA_TABLE *schema_table)
{
  return (enum enum_schema_tables)(schema_table - &schema_tables[0]); // IT'S JUST FEAR (schema_tables)
}


bool check_global_access(bool no_errors)
{ return 0; }

bool check_grant(privilege_t, TABLE_LIST *, bool, uint, bool)
{
  return 0;
}

bool check_access(const privilege_t want_access,
                                    bool match_any)
{
  DBUG_ENTER("Security_context::check_access");
  DBUG_RETURN((match_any ? (want_access) != NO_ACL
                         : ((want_access) == want_access)));
}

static bool check_show_access(TABLE_LIST *table)
{
  /*
    This is a SHOW command using an INFORMATION_SCHEMA table.
    check_access() has not been called for 'table',
    and SELECT is currently always granted on the I_S, so we automatically
    grant SELECT on table here, to bypass a call to check_access().
    Note that not calling check_access(table) is an optimization,
    which needs to be revisited if the INFORMATION_SCHEMA does
    not always automatically grant SELECT but use the grant tables.
    See Bug#38837 need a way to disable information_schema for security
  */
  table->grant.privilege= SELECT_ACL;

  switch (get_schema_table_idx(table->schema_table))
  {
  case SCH_SCHEMATA:
    return (specialflag & SPECIAL_SKIP_SHOW_DB) &&
           check_global_access(SHOW_DB_ACL);

  case SCH_TABLE_NAMES:
  case SCH_TABLES:
  case SCH_VIEWS:
  case SCH_TRIGGERS:
  case SCH_EVENTS: {
    const char *dst_db_name= table->schema_select_lex->db.str;

    DBUG_ASSERT(dst_db_name);

    if (check_access(thd, SELECT_ACL, dst_db_name, &thd->col_access, NULL,
                     FALSE, FALSE))
      return TRUE;

    if (!thd->col_access && check_grant_db(thd, dst_db_name))
    {
      status_var_increment(thd->status_var.access_denied_errors);
      my_error(ER_DBACCESS_DENIED_ERROR, MYF(0), thd->security_ctx->priv_user,
               thd->security_ctx->priv_host, dst_db_name);
      return TRUE;
    }

    return FALSE;
  }

  case SCH_COLUMNS:
  case SCH_STATISTICS: {
    TABLE_LIST *dst_table;
    dst_table= table->schema_select_lex->table_list.first;

    DBUG_ASSERT(dst_table);

    /*
      Open temporary tables to be able to detect them during privilege check.
    */
    if (thd->open_temporary_tables(dst_table))
      return TRUE;

    if (check_access(SELECT_ACL, dst_table->db.str,
                     &dst_table->grant.privilege, &dst_table->grant.m_internal,
                     FALSE, FALSE))
      return TRUE; /* Access denied */

    /*
      Check_grant will grant access if there is any column privileges on
      all of the tables thanks to the fourth parameter (bool show_table).
    */
    if (check_grant(SELECT_ACL, dst_table, TRUE, 1, FALSE))
      return TRUE; /* Access denied */

    //close_thread_tables(thd);
    dst_table->table= NULL;

    /* Access granted */
    return FALSE;
  }
  default:
    break;
  }

  return FALSE;
}

bool check_table_access(TABLE_LIST *first_not_own_table, privilege_t requirements,
    TABLE_LIST *tables,
                        bool any_combination_of_privileges_will_do,
                        uint number, bool no_errors)
{
  TABLE_LIST *org_tables= tables;
  uint i= 0;
  /*
    The check that first_not_own_table is not reached is for the case when
    the given table list refers to the list for prelocking (contains tables
    of other queries). For simple queries first_not_own_table is 0.
  */
  for (; i < number && tables != first_not_own_table && tables;
       tables= tables->next_global, i++)
  {
    TABLE_LIST *const table_ref=
        tables->correspondent_table ? tables->correspondent_table : tables;
    //Switch_to_definer_security_ctx backup_ctx(thd, table_ref);

    privilege_t want_access(requirements);

    /*
       Register access for view underlying table.
       Remove SHOW_VIEW_ACL, because it will be checked during making view
     */
    table_ref->grant.orig_want_privilege= (want_access & ~SHOW_VIEW_ACL);

    if (table_ref->schema_table_reformed)
    {
      if (check_show_access(table_ref))
        return 1;
      continue;
    }

    DBUG_PRINT("info", ("derived: %d  view: %d", table_ref->derived != 0,
                        table_ref->view != 0));

    if (table_ref->is_anonymous_derived_table())
      continue;

    if (table_ref->sequence)
    {
      /* We want to have either SELECT or INSERT rights to sequences depending
         on how they are accessed
      */
      want_access=
          ((table_ref->lock_type >= TL_FIRST_WRITE) ? INSERT_ACL : SELECT_ACL);
    }

    if (check_access(want_access, table_ref->get_db_name(),
                     &table_ref->grant.privilege, &table_ref->grant.m_internal,
                     0, no_errors))
      return 1;
  }
  return check_grant(requirements, org_tables,
                     any_combination_of_privileges_will_do, number, no_errors);
}

bool check_some_access(privilege_t want_access, TABLE_LIST *table)
{
  DBUG_ENTER("check_some_access");

  for (ulonglong bit= 1; bit < (ulonglong) want_access; bit<<= 1)
  {
    if (bit & want_access)
    {
      privilege_t access= ALL_KNOWN_ACL & bit;
      if (!check_access(access, 0) && // not sure check_access
          !check_grant(access, table, FALSE, 1, TRUE))
        DBUG_RETURN(0);
    }
  }
  DBUG_PRINT("exit", ("no matching access rights"));
  DBUG_RETURN(1);
}

static const LEX_CSTRING *view_algorithm(TABLE_LIST *table)
{
  static const LEX_CSTRING undefined= {STRING_WITH_LEN("UNDEFINED")};
  static const LEX_CSTRING merge= {STRING_WITH_LEN("MERGE")};
  static const LEX_CSTRING temptable= {STRING_WITH_LEN("TEMPTABLE")};
  switch (table->algorithm)
  {
  case VIEW_ALGORITHM_TMPTABLE:
    return &temptable;
  case VIEW_ALGORITHM_MERGE:
    return &merge;
  default:
   // DBUG_ASSERT(0); // never should happen
    /* fall through */
  case VIEW_ALGORITHM_UNDEFINED:
    return &undefined;
  }
}



bool append_identifier(String *packet, const char *name,
                       size_t length)
{
  const char *name_end;
  char quote_char;
  int q= get_quote_char_for_identifier(thd, name, length); //// ?????????

  if (q == EOF)
    return packet->append(name, length, packet->charset());

  /*
    The identifier must be quoted as it includes a quote character or
    it's a keyword
  */

  /*
    Special code for swe7. It encodes the letter "E WITH ACUTE" on
    the position 0x60, where backtick normally resides.
    In swe7 we cannot append 0x60 using system_charset_info,
    because it cannot be converted to swe7 and will be replaced to
    question mark '?'. Use &my_charset_bin to avoid this.
    It will prevent conversion and will append the backtick as is.
  */
  CHARSET_INFO *quote_charset=
      q == 0x60 && (packet->charset()->state & MY_CS_NONASCII) &&
              packet->charset()->mbmaxlen == 1
          ? &my_charset_bin  //// ?????????
          : system_charset_info;

  (void) packet->reserve(length * 2 + 2);
  quote_char= (char) q;
  if (packet->append(&quote_char, 1, quote_charset))
    return true;

  for (name_end= name + length; name < name_end;)
  {
    uchar chr= (uchar) *name;
    int char_length= system_charset_info->charlen(name, name_end);
    /*
      charlen can return 0 and negative numbers on a wrong multibyte
      sequence. It is possible when upgrading from 4.0,
      and identifier contains some accented characters.
      The manual says it does not work. So we'll just
      change char_length to 1 not to hang in the endless loop.
    */
    if (char_length <= 0)
      char_length= 1;
    if (char_length == 1 && chr == (uchar) quote_char &&
        packet->append(&quote_char, 1, quote_charset))
      return true;
    if (packet->append(name, char_length, system_charset_info))
      return true;
    name+= char_length;
  }
  return packet->append(&quote_char, 1, quote_charset);
}

static inline bool append_identifier(String *packet,
                                     const LEX_CSTRING *name)
{
  return append_identifier(packet, name->str, name->length);
}

static bool append_at_host(String *buffer, const LEX_CSTRING *host)
{
  if (!host->str || !host->str[0])
    return false;
  return buffer->append('@') || append_identifier(buffer, host);
}

bool append_definer(String *buffer, const LEX_CSTRING *definer_user,
                    const LEX_CSTRING *definer_host)
{
  return buffer->append(STRING_WITH_LEN("DEFINER=")) ||
         append_identifier(buffer, definer_user) ||
         append_at_host(buffer, definer_host) || buffer->append(' ');
}

void view_store_options(TABLE_LIST *table, String *buff)
{
  if (table->algorithm != VIEW_ALGORITHM_INHERIT)
  {
    buff->append(STRING_WITH_LEN("ALGORITHM="));
    buff->append(view_algorithm(table));
  }
  buff->append(' ');
  append_definer(buff, &table->definer.user, &table->definer.host);
  if (table->view_suid)
    buff->append(STRING_WITH_LEN("SQL SECURITY DEFINER "));
  else
    buff->append(STRING_WITH_LEN("SQL SECURITY INVOKER "));
}

static int show_create_view(TABLE_LIST *table, String *buff, sql_mode_t sql_mode)
{
  my_bool compact_view_name= TRUE;
  my_bool foreign_db_mode=
      (sql_mode & (MODE_POSTGRESQL | MODE_ORACLE | MODE_MSSQL |
                                  MODE_DB2 | MODE_MAXDB | MODE_ANSI)) != 0;
  //???????????????????????
  if (!thd->db.str || cmp(&thd->db, &table->view_db))
    /*
      print compact view name if the view belongs to the current database
    */
    compact_view_name= table->compact_view_format= FALSE;
  else
  {
    /*
      Compact output format for view body can be used
      if this view only references table inside it's own db
    */
    TABLE_LIST *tbl;
    table->compact_view_format= TRUE;
    for (tbl= thd->lex->query_tables; tbl; tbl= tbl->next_global)
    {
      if (cmp(&table->view_db, tbl->view ? &tbl->view_db : &tbl->db))
      {
        table->compact_view_format= FALSE;
        break;
      }
    }
  }

  buff->append(STRING_WITH_LEN("CREATE "));
  if (!foreign_db_mode)
  {
    view_store_options(table, buff);
  }
  buff->append(STRING_WITH_LEN("VIEW "));
  if (!compact_view_name)
  {
    append_identifier(buff, &table->view_db);
    buff->append('.');
  }
  append_identifier(buff, &table->view_name);
  buff->append(STRING_WITH_LEN(" AS "));

  /*
    We can't just use table->query, because our SQL_MODE may trigger
    a different syntax, like when ANSI_QUOTES is defined.
  */
  table->view->unit.print(
      buff, enum_query_type(QT_VIEW_INTERNAL | QT_ITEM_ORIGINAL_FUNC_NULLIF));

  if (table->with_check != VIEW_CHECK_NONE)
  {
    if (table->with_check == VIEW_CHECK_LOCAL)
      buff->append(STRING_WITH_LEN(" WITH LOCAL CHECK OPTION"));
    else
      buff->append(STRING_WITH_LEN(" WITH CASCADED CHECK OPTION"));
  }
  return 0;
}

bool open_tables(const DDL_options_st &options, TABLE_LIST **start,
                 uint *counter, uint flags,
                 Prelocking_strategy *prelocking_strategy)
{
  /*
    We use pointers to "next_global" member in the last processed
    TABLE_LIST element and to the "next" member in the last processed
    Sroutine_hash_entry element as iterators over, correspondingly,
    the table list and stored routines list which stay valid and allow
    to continue iteration when new elements are added to the tail of
    the lists.
  */
  TABLE_LIST **table_to_open;
  Sroutine_hash_entry **sroutine_to_open;
  TABLE_LIST *tables;
  Open_table_context ot_ctx(thd, flags);
  bool error= FALSE;
  bool some_routine_modifies_data= FALSE;
  bool has_prelocking_list;
  DBUG_ENTER("open_tables");

  /* Data access in XA transaction is only allowed when it is active. */
  for (TABLE_LIST *table= *start; table; table= table->next_global)
    if (!table->schema_table)
    {
      if (thd->transaction->xid_state.check_has_uncommitted_xa())
      {
        thd->transaction->xid_state.er_xaer_rmfail();
        DBUG_RETURN(true);
      }
      else
        break;
    }

  thd->current_tablenr= 0;
restart:
  /*
    Close HANDLER tables which are marked for flush or against which there
    are pending exclusive metadata locks. This is needed both in order to
    avoid deadlocks and to have a point during statement execution at
    which such HANDLERs are closed even if they don't create problems for
    the current session (i.e. to avoid having a DDL blocked by HANDLERs
    opened for a long time).
  */
  if (thd->handler_tables_hash.records)
    mysql_ha_flush(thd);

  has_prelocking_list= thd->lex->requires_prelocking();
  table_to_open= start;
  sroutine_to_open= &thd->lex->sroutines_list.first;
  *counter= 0;
  THD_STAGE_INFO(thd, stage_opening_tables);
  prelocking_strategy->reset(thd);

  /*
    If we are executing LOCK TABLES statement or a DDL statement
    (in non-LOCK TABLES mode) we might have to acquire upgradable
    semi-exclusive metadata locks (SNW or SNRW) on some of the
    tables to be opened.
    When executing CREATE TABLE .. If NOT EXISTS .. SELECT, the
    table may not yet exist, in which case we acquire an exclusive
    lock.
    We acquire all such locks at once here as doing this in one
    by one fashion may lead to deadlocks or starvation. Later when
    we will be opening corresponding table pre-acquired metadata
    lock will be reused (thanks to the fact that in recursive case
    metadata locks are acquired without waiting).
  */
  if (!(flags & (MYSQL_OPEN_HAS_MDL_LOCK | MYSQL_OPEN_FORCE_SHARED_MDL |
                 MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL)))
  {
    if (thd->locked_tables_mode)
    {
      /*
        Under LOCK TABLES, we can't acquire new locks, so we instead
        need to check if appropriate locks were pre-acquired.
      */
      if (open_tables_check_upgradable_mdl(
              thd, *start, thd->lex->first_not_own_table(), flags))
      {
        error= TRUE;
        goto error;
      }
    }
    else
    {
      TABLE_LIST *table;
      if (lock_table_names(thd, options, *start,
                           thd->lex->first_not_own_table(),
                           ot_ctx.get_timeout(), flags))
      {
        error= TRUE;
        goto error;
      }
      for (table= *start; table && table != thd->lex->first_not_own_table();
           table= table->next_global)
      {
        if (table->mdl_request.type >= MDL_SHARED_UPGRADABLE)
          table->mdl_request.ticket= NULL;
      }
    }
  }

  /*
    Perform steps of prelocking algorithm until there are unprocessed
    elements in prelocking list/set.
  */
  while (*table_to_open ||
         (thd->locked_tables_mode <= LTM_LOCK_TABLES && *sroutine_to_open))
  {
    /*
      For every table in the list of tables to open, try to find or open
      a table.
    */
    for (tables= *table_to_open; tables;
         table_to_open= &tables->next_global, tables= tables->next_global)
    {
      error= open_and_process_table(thd, tables, counter, flags,
                                    prelocking_strategy, has_prelocking_list,
                                    &ot_ctx);

      if (unlikely(error))
      {
        if (ot_ctx.can_recover_from_failed_open())
        {
          /*
            We have met exclusive metadata lock or old version of table.
            Now we have to close all tables and release metadata locks.
            We also have to throw away set of prelocked tables (and thus
            close tables from this set that were open by now) since it
            is possible that one of tables which determined its content
            was changed.

            Instead of implementing complex/non-robust logic mentioned
            above we simply close and then reopen all tables.

            We have to save pointer to table list element for table which we
            have failed to open since closing tables can trigger removal of
            elements from the table list (if MERGE tables are involved),
          */
          close_tables_for_reopen(thd, start, ot_ctx.start_of_statement_svp());

          /*
            Here we rely on the fact that 'tables' still points to the valid
            TABLE_LIST element. Altough currently this assumption is valid
            it may change in future.
          */
          if (ot_ctx.recover_from_failed_open())
            goto error;

          /* Re-open temporary tables after close_tables_for_reopen(). */
          if (thd->open_temporary_tables(*start))
            goto error;

          error= FALSE;
          goto restart;
        }
        goto error;
      }

      DEBUG_SYNC(thd, "open_tables_after_open_and_process_table");
    }

    /*
      If we are not already in prelocked mode and extended table list is
      not yet built for our statement we need to cache routines it uses
      and build the prelocking list for it.
      If we are not in prelocked mode but have built the extended table
      list, we still need to call open_and_process_routine() to take
      MDL locks on the routines.
    */
    if (thd->locked_tables_mode <= LTM_LOCK_TABLES && *sroutine_to_open)
    {
      /*
        Process elements of the prelocking set which are present there
        since parsing stage or were added to it by invocations of
        Prelocking_strategy methods in the above loop over tables.

        For example, if element is a routine, cache it and then,
        if prelocking strategy prescribes so, add tables it uses to the
        table list and routines it might invoke to the prelocking set.
      */
      for (Sroutine_hash_entry *rt= *sroutine_to_open; rt;
           sroutine_to_open= &rt->next, rt= rt->next)
      {
        bool need_prelocking= false;
        bool routine_modifies_data;
        TABLE_LIST **save_query_tables_last= thd->lex->query_tables_last;

        error= open_and_process_routine(
            thd, thd->lex, rt, prelocking_strategy, has_prelocking_list,
            &ot_ctx, &need_prelocking, &routine_modifies_data);

        // Remember if any of SF modifies data.
        some_routine_modifies_data|= routine_modifies_data;

        if (need_prelocking && !thd->lex->requires_prelocking())
          thd->lex->mark_as_requiring_prelocking(save_query_tables_last);

        if (need_prelocking && !*start)
          *start= thd->lex->query_tables;

        if (unlikely(error))
        {
          if (ot_ctx.can_recover_from_failed_open())
          {
            close_tables_for_reopen(thd, start,
                                    ot_ctx.start_of_statement_svp());
            if (ot_ctx.recover_from_failed_open())
              goto error;

            /* Re-open temporary tables after close_tables_for_reopen(). */
            if (thd->open_temporary_tables(*start))
              goto error;

            error= FALSE;
            goto restart;
          }
          /*
            Serious error during reading stored routines from mysql.proc table.
            Something is wrong with the table or its contents, and an error has
            been emitted; we must abort.
          */
          goto error;
        }
      }
    }
    if ((error= prelocking_strategy->handle_end(thd)))
      goto error;
  }

  /*
    After successful open of all tables, including MERGE parents and
    children, attach the children to their parents. At end of statement,
    the children are detached. Attaching and detaching are always done,
    even under LOCK TABLES.

    We also convert all TL_WRITE_DEFAULT and TL_READ_DEFAULT locks to
    appropriate "real" lock types to be used for locking and to be passed
    to storage engine.

    And start wsrep TOI if needed.
  */
  for (tables= *start; tables; tables= tables->next_global)
  {
    TABLE *tbl= tables->table;

    if (!tbl)
      continue;

    /* Schema tables may not have a TABLE object here. */
    if (tbl->file->ha_table_flags() & HA_CAN_MULTISTEP_MERGE)
    {
      /* MERGE tables need to access parent and child TABLE_LISTs. */
      DBUG_ASSERT(tbl->pos_in_table_list == tables);
      if (tbl->file->extra(HA_EXTRA_ATTACH_CHILDREN))
      {
        error= TRUE;
        goto error;
      }
    }

    /* Set appropriate TABLE::lock_type. */
    if (tbl && tables->lock_type != TL_UNLOCK && !thd->locked_tables_mode)
    {
      if (tables->lock_type == TL_WRITE_DEFAULT ||
          unlikely(
              tables->lock_type == TL_WRITE_SKIP_LOCKED &&
              !(tables->table->file->ha_table_flags() & HA_CAN_SKIP_LOCKED)))
        tbl->reginfo.lock_type= thd->update_lock_default;
      else if (likely(tables->lock_type == TL_READ_DEFAULT) ||
               (tables->lock_type == TL_READ_SKIP_LOCKED &&
                !(tables->table->file->ha_table_flags() & HA_CAN_SKIP_LOCKED)))
        tbl->reginfo.lock_type= read_lock_type_for_table(
            thd, thd->lex, tables, some_routine_modifies_data);
      else
        tbl->reginfo.lock_type= tables->lock_type;
      tbl->reginfo.skip_locked= tables->skip_locked;
    }
#ifdef WITH_WSREP
    /*
       At this point we have SE associated with table so we can check
       wsrep_mode rules at this point.
    */
    if (WSREP(thd) && wsrep_thd_is_local(thd) && tbl && tables == *start &&
        !wsrep_check_mode_after_open_table(thd, tbl->file->ht, tables))
    {
      error= TRUE;
      goto error;
    }

    /* If user has issued wsrep_on = OFF and wsrep was on before
    we need to check is local gtid feature disabled */
    if (thd->wsrep_was_on && thd->variables.sql_log_bin == 1 && !WSREP(thd) &&
        wsrep_check_mode(WSREP_MODE_DISALLOW_LOCAL_GTID))
    {
      enum_sql_command sql_command= thd->lex->sql_command;
      bool is_dml_stmt=
          thd->get_command() != COM_STMT_PREPARE &&
          !thd->stmt_arena->is_stmt_prepare() &&
          (sql_command == SQLCOM_INSERT ||
           sql_command == SQLCOM_INSERT_SELECT ||
           sql_command == SQLCOM_REPLACE ||
           sql_command == SQLCOM_REPLACE_SELECT ||
           sql_command == SQLCOM_UPDATE ||
           sql_command == SQLCOM_UPDATE_MULTI || sql_command == SQLCOM_LOAD ||
           sql_command == SQLCOM_DELETE);

      if (is_dml_stmt && !is_temporary_table(tables))
      {
        /* wsrep_mode = WSREP_MODE_DISALLOW_LOCAL_GTID, treat as error */
        my_error(ER_GALERA_REPLICATION_NOT_SUPPORTED, MYF(0));
        push_warning_printf(
            thd, Sql_condition::WARN_LEVEL_WARN, ER_OPTION_PREVENTS_STATEMENT,
            "You can't execute statements that would generate local "
            "GTIDs when wsrep_mode = DISALLOW_LOCAL_GTID is set. "
            "Try disabling binary logging with SET sql_log_bin=0 "
            "to execute this statement.");

        error= TRUE;
        goto error;
      }
    }
#endif /* WITH_WSREP */
  }

error:
  thd_proc_info(thd, 0);

  if (unlikely(error) && *table_to_open)
  {
    (*table_to_open)->table= NULL;
  }
  DBUG_PRINT("open_tables", ("returning: %d", (int) error));
  DBUG_RETURN(error);
}

static int show_create_sequence(TABLE_LIST *table_list, String *packet,
                                sql_mode_t sql_mode)
{
  TABLE *table= table_list->table;
  SEQUENCE *seq= table->s->sequence;
  LEX_CSTRING alias;
  bool foreign_db_mode=
      sql_mode & (MODE_POSTGRESQL | MODE_ORACLE | MODE_MSSQL | MODE_DB2 |
                  MODE_MAXDB | MODE_ANSI);
  bool show_table_options=
      !(sql_mode & MODE_NO_TABLE_OPTIONS) && !foreign_db_mode;

  if (lower_case_table_names == 2)
  {
    alias.str= table->alias.c_ptr();
    alias.length= table->alias.length();
  }
  else
    alias= table->s->table_name;

  packet->append(STRING_WITH_LEN("CREATE SEQUENCE "));
  append_identifier(packet, &alias);
  packet->append(STRING_WITH_LEN(" start with "));
  packet->append_longlong(seq->start);
  packet->append(STRING_WITH_LEN(" minvalue "));
  packet->append_longlong(seq->min_value);
  packet->append(STRING_WITH_LEN(" maxvalue "));
  packet->append_longlong(seq->max_value);
  packet->append(STRING_WITH_LEN(" increment by "));
  packet->append_longlong(seq->increment);
  if (seq->cache)
  {
    packet->append(STRING_WITH_LEN(" cache "));
    packet->append_longlong(seq->cache);
  }
  else
    packet->append(STRING_WITH_LEN(" nocache"));
  if (seq->cycle)
    packet->append(STRING_WITH_LEN(" cycle"));
  else
    packet->append(STRING_WITH_LEN(" nocycle"));

  if (show_table_options)
    ////TODO
    add_table_options(table, 0, 0, 1, packet);
  return 0;
}


int show_create_table(TABLE_LIST *table_list, String *packet,
                      Table_specification_st *create_info_arg,
                      enum_with_db_name with_db_name)
{
    //TODO
  //return show_create_table_ex(thd, table_list, NULL, NULL, packet,
  //                            create_info_arg, with_db_name);
}

bool mysql_handle_derived(LEX *lex, uint phases)
{
  bool res= FALSE;
  DBUG_ENTER("mysql_handle_derived");
  DBUG_PRINT("enter", ("phases: 0x%x", phases));
  if (!lex->derived_tables)
    DBUG_RETURN(FALSE);

  lex->thd->derived_tables_processing= TRUE;

  for (uint phase= 0; phase < DT_PHASES && !res; phase++)
  {
    uint phase_flag= DT_INIT << phase;
    if (phase_flag > phases)
      break;
    if (!(phases & phase_flag))
      continue;

    for (SELECT_LEX *sl= lex->all_selects_list; sl && !res;
         sl= sl->next_select_in_list())
    {
      TABLE_LIST *cursor= sl->get_table_list();
      sl->changed_elements|= TOUCHED_SEL_DERIVED;
      /*
        DT_MERGE_FOR_INSERT is not needed for views/derived tables inside
        subqueries. Views and derived tables of subqueries should be
        processed normally.
      */
      if (phases == DT_MERGE_FOR_INSERT && cursor &&
          (cursor->top_table()->select_lex != lex->first_select_lex()))
        continue;
      for (; cursor && !res; cursor= cursor->next_local)
      {
        if (!cursor->is_view_or_derived() && phases == DT_MERGE_FOR_INSERT)
          continue;
        uint8 allowed_phases=
            (cursor->is_merged_derived()
                 ? DT_PHASES_MERGE
                 : DT_PHASES_MATERIALIZE | DT_MERGE_FOR_INSERT);
        /*
          Skip derived tables to which the phase isn't applicable.
          TODO: mark derived at the parse time, later set it's type
          (merged or materialized)
        */
        if ((phase_flag != DT_PREPARE && !(allowed_phases & phase_flag)) ||
            (cursor->merged_for_insert && phase_flag != DT_REINIT &&
             phase_flag != DT_PREPARE))
          continue;
        res= (*processors[phase])(lex->thd, lex, cursor);
      }
      if (lex->describe)
      {
        /*
          Force join->join_tmp creation, because we will use this JOIN
          twice for EXPLAIN and we have to have unchanged join for EXPLAINing
        */
        sl->uncacheable|= UNCACHEABLE_EXPLAIN;
        sl->master_unit()->uncacheable|= UNCACHEABLE_EXPLAIN;
      }
    }
  }
  lex->thd->derived_tables_processing= FALSE;
  DBUG_RETURN(res);
}

bool mysqld_show_create_get_fields(TABLE_LIST *table_list,
                                   List<Item> *field_list, String *buffer, LEX *lex)
{
  bool error= TRUE;
  DBUG_ENTER("mysqld_show_create_get_fields");
  DBUG_PRINT("enter", ("db: %s  table: %s", table_list->db.str,
                       table_list->table_name.str));

  if (lex->table_type == TABLE_TYPE_VIEW)
  {
    if (check_table_access(table_list, SELECT_ACL, table_list, FALSE, 1, // not that table_list
                           FALSE)) 
    {
      DBUG_PRINT("debug", ("check_table_access failed"));
      my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0), "SHOW",table_list->alias.str);
      goto exit;
    } 
    DBUG_PRINT("debug", ("check_table_access succeeded")); 

    /* Ignore temporary tables if this is "SHOW CREATE VIEW" */
    table_list->open_type= OT_BASE_ONLY;
  }
  else
  {
    /*
      Temporary tables should be opened for SHOW CREATE TABLE, but not
      for SHOW CREATE VIEW.
    */
   /* if (thd->open_temporary_tables(table_list))
      goto exit;*/

    /*
      The fact that check_some_access() returned FALSE does not mean that
      access is granted. We need to check if table_list->grant.privilege
      contains any table-specific privilege.
    */
    DBUG_PRINT("debug", ("table_list->grant.privilege: %llx",
                         (longlong)(table_list->grant.privilege)));
    if (check_some_access(SHOW_CREATE_TABLE_ACLS, table_list) ||
        (table_list->grant.privilege & SHOW_CREATE_TABLE_ACLS) == NO_ACL)
    {
      my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0), "SHOW",
               table_list->alias.str);
      goto exit;
    }
  }
  /* Access is granted. Execute the command.  */

  /* We want to preserve the tree for views. */
  lex->context_analysis_only|= CONTEXT_ANALYSIS_ONLY_VIEW;

  {
    /*
      Use open_tables() directly rather than
      open_normal_and_derived_tables().  This ensures that
      close_thread_tables() is not called if open tables fails and the
      error is ignored. This allows us to handle broken views nicely.
    */
    uint counter;
    Show_create_error_handler view_error_suppressor(table_list);
    //
    //thd->push_internal_handler(&view_error_suppressor);
    bool open_error= open_tables(&table_list, &counter,
                                 MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL) ||
                     mysql_handle_derived(lex, DT_INIT | DT_PREPARE);
    //
    //thd->pop_internal_handler();
    if (unlikely(open_error && (thd->killed || thd->is_error())))
      goto exit;
  }

  /* TODO: add environment variables show when it become possible */
  if (lex->table_type == TABLE_TYPE_VIEW && !table_list->view)
  {
    my_error(ER_WRONG_OBJECT, MYF(0), table_list->db.str,
             table_list->table_name.str, "VIEW");
    goto exit;
  }
  else if (lex->table_type == TABLE_TYPE_SEQUENCE &&
           (!table_list->table ||
            table_list->table->s->table_type != TABLE_TYPE_SEQUENCE))
  {
    my_error(ER_NOT_SEQUENCE, MYF(0), table_list->db.str,
             table_list->table_name.str);
    goto exit;
  }

  buffer->length(0);

  if (table_list->view)
    buffer->set_charset(table_list->view_creation_ctx->get_client_cs());

  if ((table_list->view ? show_create_view(table_list, buffer)
       : lex->table_type == TABLE_TYPE_SEQUENCE
           ? show_create_sequence(table_list, buffer)
           : show_create_table(table_list, buffer, NULL,
                               WITHOUT_DB_NAME)))
    goto exit;

  if (table_list->view)
  {
    field_list->push_back(new (mem_root)
                              Item_empty_string(thd, "View", NAME_CHAR_LEN),
                          mem_root);
    field_list->push_back(
        new (mem_root) Item_empty_string(thd, "Create View",
                                         MY_MAX(buffer->length(), 1024)),
        mem_root);
    field_list->push_back(new (mem_root) Item_empty_string(
                              thd, "character_set_client", MY_CS_NAME_SIZE),
                          mem_root);
    field_list->push_back(new (mem_root) Item_empty_string(
                              thd, "collation_connection", MY_CS_NAME_SIZE),
                          mem_root);
  }
  else
  {
    field_list->push_back(new (mem_root)
                              Item_empty_string(thd, "Table", NAME_CHAR_LEN),
                          mem_root);
    // 1024 is for not to confuse old clients
    field_list->push_back(
        new (mem_root) Item_empty_string(thd, "Create Table",
                                         MY_MAX(buffer->length(), 1024)),
        mem_root);
  }
  error= FALSE;

exit:
  DBUG_RETURN(error);
}


bool mysqld_show_create(TABLE_LIST *table_list, MDL_savepoint mdl_savepoint, Protocol protocol)
{
  DBUG_ENTER("mysqld_show_create");
  DBUG_PRINT("enter", ("db: %s  table: %s", table_list->db.str,
                       table_list->table_name.str));

  /*
    Metadata locks taken during SHOW CREATE should be released when
    the statmement completes as it is an information statement.
  */

  TABLE_LIST archive;

  if (mysqld_show_create_get_fields(table_list, &field_list, &buffer))
    return 0;
    //goto exit;

  if (protocol->send_result_set_metadata(&field_list, Protocol::SEND_NUM_ROWS |
                                                          Protocol::SEND_EOF))
    goto exit;

  protocol->prepare_for_resend();
  if (table_list->view)
    protocol->store(&table_list->view_name, system_charset_info);
  else
  {
    if (table_list->schema_table)
      protocol->store(table_list->schema_table->table_name,
                      strlen(table_list->schema_table->table_name),
                      system_charset_info);
    else
      protocol->store(table_list->table->alias.ptr(),
                      table_list->table->alias.length(), system_charset_info);
  }

  if (table_list->view)
  {
    buffer.set_charset(table_list->view_creation_ctx->get_client_cs());
    protocol->store(&buffer);

    protocol->store(&table_list->view_creation_ctx->get_client_cs()->cs_name,
                    system_charset_info);

    protocol->store(
        &table_list->view_creation_ctx->get_connection_cl()->coll_name,
        system_charset_info);
  }
  else
    protocol->store(&buffer);

  if (protocol->write())
    goto exit;

  /*error= FALSE;
  my_eof(thd);*/

//exit:
  ////close_thread_tables(thd);
  /* Release any metadata locks taken during SHOW CREATE. */
  //thd->mdl_context.rollback_to_savepoint(mdl_savepoint);
 // DBUG_RETURN(error);
  bool error= FALSE;

exit:
  DBUG_RETURN(error);
}