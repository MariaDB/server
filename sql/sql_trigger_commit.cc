/*
   Copyright (c) 2026, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335  USA */

/*
  Hookup sketch for AFTER ... FOR EACH STATEMENT and
  AFTER ... BEFORE COMMIT [OR SAVEPOINT] triggers.

  This file establishes WHERE the new trigger granularities attach to the
  server and WHAT each attachment point has in hand.  It does not execute
  trigger bodies and it does not store row images: the accumulator and the
  statement sets below keep only row counts, which is enough to exercise
  savepoint and statement-rollback truncation and per-trigger watermarks.
  Every hook reports itself with trg_hook_trace().

  What the real implementation does at a hook that this sketch cannot do
  yet (run a body, refuse a statement, roll back a body's writes) is a
  stub: a named function called where the real code goes, with the
  arguments that code needs, a comment that says what it does, and a
  trace line.  A stub that would refuse something traces what it would
  refuse and reports no error.

  The accumulator:
    - one per transaction, hung off thd->transaction and allocated on the
      transaction mem_root, so it is freed with the savepoint list;
    - an append-only sequence of captured rows; each takes one position.
      A statement that matches no row captures nothing, so a trigger
      fires only over rows;
    - one Trg_commit_table per (transaction, table), shared by every
      BEFORE COMMIT trigger of that table, kept in the order in which the
      transaction first changed the tables, which is the order they fire
      in.  It is created by the table's first captured row, and its
      column set (the union of the columns its triggers capture) is fixed
      then: the table's MDL is held from then on, so its triggers cannot
      change.  A rollback to a point before that position removes the
      entry, and the next row creates it again from the triggers the
      table has then;
    - one record per (transaction, trigger), made with the table's entry:
      the trigger's name, events and OR SAVEPOINT flag, and the position
      up to which it has already been fired.  A boundary decides from
      these, without the table, whether the table has anything to fire;
    - it is a transaction participant, trg_commit_tp, registered with
      trans_register_ha() by every statement that captures a row, as the
      binary log and the engines are: engines do not promise to register
      every statement, and the end of a statement and of the transaction
      must reach the accumulator whatever the engine does.  Its callbacks
      keep the positions: the end of a successful statement inside a
      transaction records the position as the statement mark, a failed
      statement is truncated back to it (the implicit savepoint every
      statement has), SAVEPOINT records the position in the participant's
      part of the savepoint whether or not any trigger fires on
      SAVEPOINT, ROLLBACK TO SAVEPOINT truncates back to it, and the end
      of the transaction frees the accumulator;
    - firing is never done from a callback: the triggers fire before
      anything starts to end the transaction or to set the savepoint, in
      explicit calls from trans_commit(), trans_commit_implicit(),
      trans_begin(), trans_xa_end(), trans_savepoint() and, for an
      autocommit statement, the top of ha_commit_trans();
    - every statement that writes a table prelocks the tables of the
      table's BEFORE COMMIT bodies, as it does for ROW triggers, so the
      metadata locks are taken by the statements, in the same way in
      autocommit mode and in a transaction, and are held at a boundary;
    - the bodies of a boundary run in a statement.  At the end of an
      autocommit statement it is that statement, whose tables are still
      open and which prelocked the tables of the bodies.  At any other
      boundary no table is open, and the boundary runs a statement of its
      own: trg_commit_boundary_open() opens and locks, with prelocking,
      the tables whose triggers fire and everything their bodies use,
      and the statement ends with a statement commit, or a statement
      rollback if a body failed.  A boundary with nothing to fire opens
      nothing.

  The statement sets:
    - one per statement that captured a row, in LEX::trg_stmt_set: a
      statement inside a trigger body or a stored routine has a LEX, and
      so a set, of its own;
    - one entry per table, whatever TABLE instance (alias, view) wrote the
      row, so a self-join UPDATE fires the table's triggers once over the
      rows of both aliases;
    - fired and reset at the end of the statement; a failed statement
      drops its set without firing.
*/

#define MYSQL_LEX 1
#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sql_trigger.h"
#include "sql_base.h"
#include "sp_head.h"
#include "log.h"
#include "mdl.h"
#include "transaction.h"
#include "rpl_rli.h"                            // rpl_group_info


void trg_hook_trace(const char *format, ...)
{
#ifndef DBUG_OFF
  char buff[1024];
  va_list args;
  va_start(args, format);
  my_vsnprintf(buff, sizeof(buff), format, args);
  va_end(args);
  sql_print_information("Trigger hook: %s", buff);
#endif
}


/*
  Every captured row carries these besides the captured columns:
  __op_order the order of the change; the OLD and NEW rows of an UPDATE
             share it
  __op       I, U or D
  __state    O for an OLD image, N for a NEW image
  OF and EXCEPT never name them, and no captured column may be named
  like one.
*/
static const LEX_CSTRING trg_capture_control_columns[]=
{
  { STRING_WITH_LEN("__op_order") },
  { STRING_WITH_LEN("__op") },
  { STRING_WITH_LEN("__state") }
};


/*
  Add the columns one FOR EACH STATEMENT or BEFORE COMMIT trigger
  captures to set: the OF list, all columns but the EXCEPT list, or all
  columns when there is no list.  All columns are those that NEW and OLD
  reach in a row trigger:
  every column but the INVISIBLE_FULL ones.

  Checked at CREATE TRIGGER, and again every time a capture set is built,
  because ALTER TABLE does not look at triggers (as for NEW.x and OLD.x in
  row triggers).  A listed column that does not exist, a captured column
  named like a control column, and an empty set are errors when
  report_errors is set; otherwise they are skipped.

  @return true on error (reported only if report_errors)
*/

bool trg_capture_columns_resolve(TABLE *table, List<LEX_CSTRING> *names,
                                 bool except, MY_BITMAP *set,
                                 bool report_errors)
{
  if (names && report_errors)
  {
    List_iterator_fast<LEX_CSTRING> it(*names);
    while (LEX_CSTRING *name= it++)
    {
      Field *field= table->find_field_by_name(name);
      if (!field || field->invisible == INVISIBLE_FULL)
      {
        my_error(ER_BAD_FIELD_ERROR, MYF(0), name->str,
                 except ? "EXCEPT" : "OF");
        return true;
      }
    }
  }

  uint captured= 0;
  for (Field **field_ptr= table->field; *field_ptr; field_ptr++)
  {
    Field *field= *field_ptr;
    if (field->invisible == INVISIBLE_FULL)
      continue;
    if (names)
    {
      bool listed= false;
      List_iterator_fast<LEX_CSTRING> it(*names);
      while (LEX_CSTRING *name= it++)
        if ((listed= field->field_name.streq(*name)))
          break;
      if (listed == except)
        continue;
    }
    bool control= false;
    for (const LEX_CSTRING &name : trg_capture_control_columns)
      control|= field->field_name.streq(name);
    if (control)
    {
      if (!report_errors)
        continue;
      my_error(ER_DUP_FIELDNAME, MYF(0), field->field_name.str);
      return true;
    }
    bitmap_set_bit(set, field->field_index);
    captured++;
  }

  if (!captured && report_errors)
  {
    my_error(ER_TABLE_MUST_HAVE_COLUMNS, MYF(0));
    return true;
  }
  return false;
}


/*
  The union of the columns captured by this table's triggers of one
  granularity, over all events: every row in a statement set or an
  accumulator entry carries all of them.
*/

bool Table_triggers_list::get_capture_columns(trg_granularity_type granularity,
                                              MY_BITMAP *set,
                                              bool report_errors)
{
  for (uint e= 0; e < (uint) TRG_EVENT_MAX; e++)
    for (Trigger *tr= get_trigger(granularity, (trg_event_type) e,
                                  TRG_ACTION_AFTER);
         tr; tr= tr->next[e])
    {
      if (!is_the_right_most_event_bit(tr->events, e))
        continue;
      if (trg_capture_columns_resolve(trigger_table, tr->capture_columns,
                                      tr->capture_columns_except, set,
                                      report_errors))
        return true;
    }
  return false;
}


void Table_triggers_list::print_capture_columns(const MY_BITMAP *set,
                                                String *out)
{
  for (uint i= 0; i < trigger_table->s->fields; i++)
  {
    if (!bitmap_is_set(set, i))
      continue;
    out->append(trigger_table->field[i]->field_name);
    out->append(STRING_WITH_LEN(", "));
  }
  out->append(STRING_WITH_LEN("+ __op_order, __op, __state"));
}


static bool trg_stmt_capture(THD *thd, TABLE *table, trg_event_type event,
                             const char *images);
static bool trg_commit_capture(THD *thd, TABLE *table, trg_event_type event,
                               const char *images);


#ifndef DBUG_OFF
/*
  Debug injection trg_commit_body_writes: stands for a BEFORE COMMIT body
  that runs, as far as the accumulator sees it.  For every table that a
  statement of the body writes and that has BEFORE COMMIT triggers of
  INSERT, one INSERT row is captured, as that statement would capture
  it.  No row is written to the table: only the positions of nested
  firings are exercised.  A table on the firing chain is skipped; the
  real code refuses the body's statement that writes it (see
  trg_chain_check_write()).
*/

static bool trg_simulate_body_writes(THD *thd, Trigger *trigger)
{
  TABLE_LIST *tables= nullptr, **last= &tables;
  trigger->body->add_used_tables_to_table_list(thd, &last, nullptr);
  for (TABLE_LIST *tl= tables; tl; tl= tl->next_global)
  {
    if (tl->lock_type < TL_FIRST_WRITE)
      continue;
    bool on_chain= false;
    for (Trg_firing_link *l= thd->trg_firing_chain; l; l= l->prev)
      if (tl->db.streq(l->db) && tl->table_name.streq(l->name))
        on_chain= true;
    TABLE *table= on_chain ? nullptr :
      find_locked_table(thd->open_tables, tl->db.str, tl->table_name.str);
    if (!table || !table->triggers ||
        !table->triggers->get_trigger(TRG_GRANULARITY_COMMIT,
                                      TRG_EVENT_INSERT, TRG_ACTION_AFTER))
      continue;
    table->triggers->mark_fields_used(TRG_EVENT_INSERT);
    if (trg_commit_capture(thd, table, TRG_EVENT_INSERT,
                           "simulated write of a body"))
      return true;
  }
  return false;
}
#endif


/*
  Where a STATEMENT or BEFORE COMMIT trigger body runs.  Stub: the sketch
  executes no body.

  The real code runs the body the way process_triggers() runs a ROW
  trigger body:
    - inside thd->reset_sub_statement_state(&state, SUB_STMT_TRIGGER) and
      restore_sub_statement_state(), so LAST_INSERT_ID(), ROW_COUNT() and
      FOUND_ROWS() of the statement that reached the boundary (COMMIT,
      SAVEPOINT, the DML itself) stay untouched, a result set from the
      body is refused, and COMMIT or ROLLBACK in it fails with
      ER_COMMIT_NOT_ALLOWED_IN_SF_OR_TRG;
    - through trigger->body->execute_trigger(thd, &db, &table_name,
      &trigger->subject_table_grants), which switches to the DEFINER's
      security context and checks the DEFINER's TRIGGER privilege on the
      subject table at that moment, so a REVOKE between the DML and the
      COMMIT makes the COMMIT fail;
    - reading a column of MODIFIED needs the DEFINER's SELECT privilege on
      it, the same check_grant_column() that NEW.col and OLD.col get;
    - session variables (sql_mode, time_zone, ...) are read when the body
      runs, as they are for an explicit CALL made at that point;
    - with thd->bulk_param set to nullptr around the body and restored
      after it, so that a DML statement in the body does not consume the
      next values of an array that the calling statement has bound;
    - with thd->lex->current_select set to NULL before each body and
      restored after the last one;
    - counting each body in the status variable Executed_triggers.

  @return true if the body failed; the caller fails the statement that
          reached the boundary
*/

static bool trg_execute_body(THD *thd, TABLE *table, Trigger *trigger,
                             const char *fired)
{
  trg_hook_trace("%s; body would run here: execute_trigger() as %s, "
                 "SUB_STMT_TRIGGER", fired, trigger->definer.str);

  /*
    Fails the last BEFORE COMMIT trigger of the table, so that those
    before it have already moved their watermarks.
  */
  DBUG_EXECUTE_IF("trg_commit_body_fails",
                  if (trigger->granularity == TRG_GRANULARITY_COMMIT &&
                      !trigger->next_commit)
                  {
                    my_printf_error(ER_UNKNOWN_ERROR,
                                    "BEFORE COMMIT trigger %s failed "
                                    "(injected)", MYF(0), trigger->name.str);
                    return true;
                  });
  /*
    The same, but only once: a second firing succeeds.  Marked by adding a
    keyword, since removing the only one would enable all of them.
  */
  DBUG_EXECUTE_IF("trg_commit_body_fails_once",
                  if (trigger->granularity == TRG_GRANULARITY_COMMIT &&
                      !trigger->next_commit &&
                      !DBUG_IF("trg_commit_body_failed_once"))
                  {
                    DBUG_SET("+d,trg_commit_body_failed_once");
                    my_printf_error(ER_UNKNOWN_ERROR,
                                    "BEFORE COMMIT trigger %s failed "
                                    "(injected)", MYF(0), trigger->name.str);
                    return true;
                  });
  /* Bodies that fail as a statement waiting for a lock would */
  DBUG_EXECUTE_IF("trg_commit_body_deadlocks",
                  if (trigger->granularity == TRG_GRANULARITY_COMMIT &&
                      !trigger->next_commit)
                  {
                    my_error(ER_LOCK_DEADLOCK, MYF(0));
                    return true;
                  });
  DBUG_EXECUTE_IF("trg_commit_body_deadlocks_once",
                  if (trigger->granularity == TRG_GRANULARITY_COMMIT &&
                      !trigger->next_commit &&
                      !DBUG_IF("trg_commit_body_deadlocked_once"))
                  {
                    DBUG_SET("+d,trg_commit_body_deadlocked_once");
                    my_error(ER_LOCK_DEADLOCK, MYF(0));
                    return true;
                  });
  DBUG_EXECUTE_IF("trg_commit_body_lock_wait_timeout",
                  if (trigger->granularity == TRG_GRANULARITY_COMMIT &&
                      !trigger->next_commit)
                  {
                    my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
                    return true;
                  });
  DBUG_EXECUTE_IF("trg_commit_body_lock_wait_timeout_once",
                  if (trigger->granularity == TRG_GRANULARITY_COMMIT &&
                      !trigger->next_commit &&
                      !DBUG_IF("trg_commit_body_timed_out_once"))
                  {
                    DBUG_SET("+d,trg_commit_body_timed_out_once");
                    my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
                    return true;
                  });
  /* Fails a body that fires nested, under the body of another table */
  DBUG_EXECUTE_IF("trg_commit_nested_body_fails",
                  if (trigger->granularity == TRG_GRANULARITY_COMMIT &&
                      thd->trg_firing_chain && thd->trg_firing_chain->prev)
                  {
                    my_printf_error(ER_UNKNOWN_ERROR,
                                    "BEFORE COMMIT trigger %s failed "
                                    "(injected)", MYF(0), trigger->name.str);
                    return true;
                  });
  /* A body that writes the tables its statements write */
  DBUG_EXECUTE_IF("trg_commit_body_writes",
                  if (trigger->granularity == TRG_GRANULARITY_COMMIT &&
                      trg_simulate_body_writes(thd, trigger))
                    return true;);
  DBUG_EXECUTE_IF("trg_stmt_body_fails",
                  if (trigger->granularity == TRG_GRANULARITY_STATEMENT)
                  {
                    my_printf_error(ER_UNKNOWN_ERROR,
                                    "FOR EACH STATEMENT trigger %s failed "
                                    "(injected)",
                                    MYF(0), trigger->name.str);
                    return true;
                  });
  /*
    A FOR EACH STATEMENT body that runs until the statement is killed, for
    at most ten seconds, as one that calls SLEEP() would
  */
  DBUG_EXECUTE_IF("trg_stmt_body_runs_until_killed",
                  if (trigger->granularity == TRG_GRANULARITY_STATEMENT)
                  {
                    for (uint i= 0; i < 1000 && !thd->killed; i++)
                      my_sleep(10000);
                    if (thd->killed)
                    {
                      thd->send_kill_message();
                      return true;
                    }
                  });
  return false;
}


/* A run of consecutive captured rows of one (table, event) */
struct Trg_commit_run: public Sql_alloc
{
  Trg_commit_run *next;
  struct Trg_commit_table *table;
  trg_event_type event;
  ulonglong start;                              // position of the first row
  ulonglong count;
};


/*
  Per (transaction, trigger): one for every BEFORE COMMIT trigger the
  table had when its entry was created, in firing order.  The triggers
  cannot change while the entry exists, so a boundary decides from these,
  without the TABLE, whether the table has anything to fire.
*/
struct Trg_commit_trigger: public Sql_alloc
{
  Trg_commit_trigger *next;
  LEX_CSTRING name;
  trg_event_set events;
  bool fires_on_savepoint;
  /* The watermark: the trigger has fired over the rows before it */
  ulonglong pos;
  /* pos when the boundary now firing started, to restore if it fails */
  ulonglong boundary_pos;
};


/*
  Per (transaction, table).  Created lazily by the first captured row of the
  table, and removed again by a rollback to a point before that row.

  The table's triggers are not copied here: the boundary opens the table
  again (trg_commit_boundary_open()) and reaches them through its TABLE,
  as a ROW trigger does.  The transaction's metadata lock on the table,
  held since the first captured row, keeps them from changing in between.
*/
struct Trg_commit_table: public Sql_alloc
{
  Trg_commit_table *next;
  LEX_CSTRING db, name;
  ulonglong created_pos;                        // position of the first row
  /* The captured user columns, fixed with the entry; field_index bits */
  MY_BITMAP columns;
  Trg_commit_trigger *triggers;
  /* Per event, the end of its last run: 0, or one past its last position */
  ulonglong last_end[TRG_EVENT_MAX];

  bool is(const LEX_CSTRING &db_arg, const LEX_CSTRING &name_arg) const
  {
    return Lex_ident_db(db).streq(db_arg) &&
           Lex_ident_table(name).streq(name_arg);
  }

  Trg_commit_trigger *find_trigger(const Lex_ident_trigger &trigger_name) const
  {
    for (Trg_commit_trigger *r= triggers; r; r= r->next)
      if (trigger_name.streq(r->name))
        return r;
    return nullptr;
  }
};


static const trg_event_set trg_all_events= trg2bit(TRG_EVENT_INSERT) |
                                           trg2bit(TRG_EVENT_UPDATE) |
                                           trg2bit(TRG_EVENT_DELETE);


class Trg_commit_accumulator: public Sql_alloc
{
public:
  /* In the order the transaction first changed them */
  Trg_commit_table *tables= nullptr, **tables_last= &tables;
  Trg_commit_run *runs= nullptr, *last_run= nullptr;
  ulonglong pos= 0;                             // rows captured so far
  ulonglong stmt_mark= 0;                       // pos at statement start
  ulonglong boundary_pos= 0;                    // pos when a boundary began
  /* A body ran at a COMMIT or SAVEPOINT boundary of the transaction */
  bool bodies_ran= false;
  bool boundary_bodies_ran= false;
  /*
    A boundary is firing.  The statement it runs the bodies in ends with a
    statement commit, which must not be taken for a boundary of its own.
  */
  bool in_boundary= false;
  /*
    The table whose triggers failed to fire at a boundary, and the
    trigger whose body failed, if a body failed
  */
  LEX_CSTRING failed_db= null_clex_str, failed_table= null_clex_str;
  LEX_CSTRING failed_trigger= null_clex_str;

  Trg_commit_table *find_table(const LEX_CSTRING &db, const LEX_CSTRING &name)
  {
    /* Consecutive rows are nearly always of the table of the last run */
    if (last_run && last_run->table->is(db, name))
      return last_run->table;
    for (Trg_commit_table *t= tables; t; t= t->next)
      if (t->is(db, name))
        return t;
    return nullptr;
  }

  /* Whether t has rows of the given events at or after position from */
  static bool has_entries_after(const Trg_commit_table *t,
                                trg_event_set events, ulonglong from)
  {
    for (uint e= 0; e < (uint) TRG_EVENT_MAX; e++)
      if (is_trg_event_on(events, (trg_event_type) e) && t->last_end[e] > from)
        return true;
    return false;
  }

  /*
    The rows of the given events of t at or after position from, in total
    and per event
  */
  ulonglong entries_after(const Trg_commit_table *t, trg_event_set events,
                          ulonglong from, ulonglong per_event[TRG_EVENT_MAX])
  {
    ulonglong total= 0;
    for (Trg_commit_run *r= runs; r; r= r->next)
    {
      if (r->table != t || !is_trg_event_on(events, r->event))
        continue;
      ulonglong end= r->start + r->count;
      if (end <= from)
        continue;
      ulonglong n= end - MY_MAX(r->start, from);
      per_event[r->event]+= n;
      total+= n;
    }
    return total;
  }

  /* The rows captured at or after position from, of all tables */
  ulonglong rows_from(ulonglong from)
  {
    return pos > from ? pos - from : 0;
  }

  /*
    Whether trigger r of t has rows of its events that it has not fired
    over; at a SAVEPOINT only the OR SAVEPOINT triggers count
  */
  static bool trigger_has_unfired(const Trg_commit_table *t,
                                  const Trg_commit_trigger *r,
                                  bool at_savepoint)
  {
    return (!at_savepoint || r->fires_on_savepoint) &&
           has_entries_after(t, r->events, r->pos);
  }

  /* Whether a trigger of t has such rows */
  static bool has_unfired(const Trg_commit_table *t, bool at_savepoint)
  {
    for (Trg_commit_trigger *r= t->triggers; r; r= r->next)
      if (trigger_has_unfired(t, r, at_savepoint))
        return true;
    return false;
  }

  /* Whether every trigger has fired over every captured row of its events */
  bool all_fired() const
  {
    for (Trg_commit_table *t= tables; t; t= t->next)
      if (has_unfired(t, false))
        return false;
    return true;
  }

  /* Take the next position for a row of (t, event) */
  bool append(MEM_ROOT *root, Trg_commit_table *t, trg_event_type event)
  {
    if (last_run && last_run->table == t && last_run->event == event)
      last_run->count++;
    else
    {
      Trg_commit_run *r= new (root) Trg_commit_run();
      if (!r)
        return true;
      r->next= nullptr;
      r->table= t;
      r->event= event;
      r->start= pos;
      r->count= 1;
      if (last_run)
        last_run->next= r;
      else
        runs= r;
      last_run= r;
    }
    t->last_end[event]= ++pos;
    return false;
  }

  /*
    Drop every row at or after new_pos and every table entry created at or
    after it, returning to the state before those tables were touched;
    clamp watermarks past new_pos.
  */
  void truncate(ulonglong new_pos)
  {
    if (new_pos >= pos)
      return;
    Trg_commit_run *keep= nullptr;
    for (Trg_commit_run *r= runs; r && r->start < new_pos; r= r->next)
      keep= r;
    if (keep)
    {
      keep->count= MY_MIN(keep->count, new_pos - keep->start);
      keep->next= nullptr;
    }
    else
      runs= nullptr;
    last_run= keep;
    /* Entries are in creation order, so the ones created after new_pos
       are the tail of the list */
    Trg_commit_table **tail= &tables;
    while (*tail && (*tail)->created_pos < new_pos)
      tail= &(*tail)->next;
    for (Trg_commit_table *t= *tail; t; t= t->next)
      trg_hook_trace("accumulator: entry for %s.%s removed, the table was "
                     "first touched after the rollback point",
                     t->db.str, t->name.str);
    *tail= nullptr;
    tables_last= tail;
    for (Trg_commit_table *t= tables; t; t= t->next)
    {
      bzero((char *) t->last_end, sizeof(t->last_end));
      for (Trg_commit_trigger *r= t->triggers; r; r= r->next)
        r->pos= MY_MIN(r->pos, new_pos);
    }
    for (Trg_commit_run *r= runs; r; r= r->next)
      r->table->last_end[r->event]= r->start + r->count;
    pos= new_pos;
    stmt_mark= MY_MIN(stmt_mark, new_pos);
  }
};


/*
  THD::st_transactions::cleanup() found an accumulator: the transaction
  state is cleaned up on a path that called neither the commit nor the
  rollback of the participant below, which free it otherwise.
*/

void trg_commit_detach(Trg_commit_accumulator *acc)
{
  trg_hook_trace("transaction state cleaned up with no commit or rollback "
                 "of the accumulator: %llu rows freed with the transaction "
                 "mem_root", acc->rows_from(0));
}


/*
  The accumulator as a transaction participant.

  It takes part in no two-phase commit and writes nothing: it is
  registered so that the server tells it where statements, savepoints and
  the transaction end.  It is never marked read-write, so it does not
  count when the server chooses between one-phase and two-phase commit.

  The callbacks only keep positions.  No trigger fires from them: bodies
  write to engines, so they run before the first participant is asked to
  prepare, commit or set a savepoint; see the callers of
  trg_commit_fire().
*/

/*
  commit(all=false) at the end of a statement inside a transaction: the
  statement's rows stay with the transaction.  Any other commit ends the
  transaction, as ha_commit_trans() sees it: the triggers have fired, and
  the accumulator is freed with the transaction mem_root.
*/

static int trg_commit_tp_commit(THD *thd, bool all)
{
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  if (!acc)
    return 0;
  if (all || (!thd->transaction->all.ha_list &&
              !(thd->variables.option_bits & OPTION_GTID_BEGIN)))
  {
    /*
      Every trigger has fired over every captured row of its events: a
      transaction commits only after a boundary that succeeded, and
      nothing is captured after it.  A path that commits a transaction
      without one of the firing calls fails here.
    */
    DBUG_ASSERT(acc->all_fired());
    trg_hook_trace("transaction end: accumulator of %llu rows freed with the "
                   "transaction mem_root", acc->rows_from(0));
    thd->transaction->trg_commit= nullptr;
  }
  else if (acc->stmt_mark != acc->pos)
  {
    trg_hook_trace("statement committed inside the transaction: statement "
                   "mark moves from %llu to %llu", acc->stmt_mark, acc->pos);
    acc->stmt_mark= acc->pos;
  }
  return 0;
}


/*
  rollback(all=false) of a statement inside a transaction drops the
  failed statement's rows.  Any other rollback drops all of them:
  ROLLBACK, a deadlock, a failed commit, including one whose BEFORE
  COMMIT trigger failed, a disconnect, and ROLLBACK TO a savepoint that
  was set before the transaction captured its first row, where the
  server rolls back the participants that joined after the savepoint.
*/

static int trg_commit_tp_rollback(THD *thd, bool all)
{
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  if (!acc)
    return 0;
  if (all || !thd->transaction->all.ha_list)
  {
    trg_hook_trace("ROLLBACK of the transaction, or to a savepoint set "
                   "before its first captured row: %llu captured rows "
                   "discarded; BEFORE COMMIT triggers had %s",
                   acc->rows_from(0), acc->bodies_ran ?
                   "ALREADY FIRED, their writes roll back with the "
                   "transaction" : "not fired");
    thd->transaction->trg_commit= nullptr;
  }
  else if (acc->pos != acc->stmt_mark)
  {
    trg_hook_trace("statement rollback inside the transaction: %llu rows "
                   "captured by the failed statement dropped",
                   acc->rows_from(acc->stmt_mark));
    acc->truncate(acc->stmt_mark);
  }
  return 0;
}


/*
  The position of a savepoint is kept in the participant's part of the
  savepoint (savepoint_offset).  The callbacks are not given the name of
  the savepoint.
*/

static int trg_commit_tp_savepoint_set(THD *thd, void *sv)
{
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  ulonglong pos= acc ? acc->pos : 0;
  memcpy(sv, &pos, sizeof(pos));
  if (acc)
    trg_hook_trace("SAVEPOINT: marker at position %llu", pos);
  return 0;
}


static int trg_commit_tp_savepoint_rollback(THD *thd, void *sv)
{
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  if (!acc)
    return 0;
  ulonglong pos;
  memcpy(&pos, sv, sizeof(pos));
  trg_hook_trace("ROLLBACK TO SAVEPOINT: %llu rows captured after the "
                 "savepoint dropped, watermarks clamped",
                 acc->rows_from(pos));
  acc->truncate(pos);
  return 0;
}


static int trg_commit_tp_savepoint_release(THD *thd, void *sv)
{
  if (thd->transaction->trg_commit)
  {
    ulonglong pos;
    memcpy(&pos, sv, sizeof(pos));
    trg_hook_trace("RELEASE SAVEPOINT: marker at position %llu dropped, "
                   "accumulator unchanged", pos);
  }
  return 0;
}


transaction_participant trg_commit_tp=
{
  0, sizeof(ulonglong), 0,              /* slot, savepoint_offset, flags */
  nullptr,                              /* close_connection */
  trg_commit_tp_savepoint_set,
  trg_commit_tp_savepoint_rollback,
  /* The accumulator needs no metadata lock taken after the savepoint */
  /* savepoint_rollback_can_release_mdl */
  [](THD *){ return true; },
  trg_commit_tp_savepoint_release,
  trg_commit_tp_commit,
  trg_commit_tp_rollback,
  /* Without it the server would switch two-phase commit off */
  [](THD *, bool){ return 0; },         /* prepare */
  nullptr,                              /* recover */
  nullptr, nullptr,                     /* commit/rollback_by_xid */
  nullptr, nullptr,                     /* recover_rollback_by_xid, .._done */
  nullptr, nullptr, nullptr,            /* snapshot, commit/prepare_ordered */
  nullptr, nullptr                      /* checkpoint, versioned */
};


static int trg_commit_plugin_init(void *p)
{
  st_plugin_int *plugin= (st_plugin_int *) p;
  plugin->data= &trg_commit_tp;
  return setup_transaction_participant(plugin);
}


static struct st_mysql_daemon trg_commit_plugin=
{ MYSQL_DAEMON_INTERFACE_VERSION };

maria_declare_plugin(commit_triggers)
{
  MYSQL_DAEMON_PLUGIN,
  &trg_commit_plugin,
  "commit_triggers",
  "MariaDB plc",
  "The rows captured for BEFORE COMMIT triggers as a transaction participant",
  PLUGIN_LICENSE_GPL,
  trg_commit_plugin_init,
  NULL,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;


/* The rows one statement level captured for one table, all aliases and
   views of the table together.  The triggers of an event fire when it
   has rows. */
struct Trg_stmt_set_table: public Sql_alloc
{
  Trg_stmt_set_table *next;
  TABLE *table;                         // the first TABLE that reached it
  ha_rows rows[TRG_EVENT_MAX];
};


class Trg_stmt_set: public Sql_alloc
{
public:
  /* In the order the statement first changed them */
  Trg_stmt_set_table *tables= nullptr, **tables_last= &tables;

  Trg_stmt_set_table *find_table(const TABLE_SHARE *s)
  {
    for (Trg_stmt_set_table *t= tables; t; t= t->next)
      if (t->table->s == s ||
          (t->table->s->db.streq(s->db) &&
           t->table->s->table_name.streq(s->table_name)))
        return t;
    return nullptr;
  }
};


/*
  Capture: called from Table_triggers_list::process_triggers() for the
  AFTER call of every written row, on the TABLE the row was written to.

  @return true if the capture columns no longer resolve; the row's
          statement fails
*/

bool Table_triggers_list::capture_row_for_multi_row_triggers(
  THD *thd, trg_event_type event, bool old_row_is_record1)
{
  const char *images;
  switch (event) {
  case TRG_EVENT_INSERT:
    images= "NEW=record[0]";
    break;
  case TRG_EVENT_UPDATE:
    images= "OLD=record[1] NEW=record[0]";
    break;
  default:
    images= old_row_is_record1 ? "OLD=record[1]" : "OLD=record[0]";
  }

  if (get_trigger(TRG_GRANULARITY_STATEMENT, event, TRG_ACTION_AFTER) &&
      trg_stmt_capture(thd, trigger_table, event, images))
    return true;
  if (get_trigger(TRG_GRANULARITY_COMMIT, event, TRG_ACTION_AFTER))
    return trg_commit_capture(thd, trigger_table, event, images);
  return false;
}


/*
  The entry for table in the statement set of the statement now
  executing, thd->lex, created with the set itself if need be by the
  statement's first captured row of the table.  An entry belongs to the
  table, not to the TABLE instance that reached it.

  @return nullptr on error
*/

static Trg_stmt_set_table *trg_stmt_set_entry(THD *thd, TABLE *table)
{
  Trg_stmt_set *set= thd->lex->trg_stmt_set;
  if (!set)
  {
    /*
      The set lives on the statement's mem_root: it is fired and reset
      at the end of the statement, before that is freed.
    */
    if (!(set= new (thd->mem_root) Trg_stmt_set()))
      return nullptr;
    thd->lex->trg_stmt_set= set;
  }

  Trg_stmt_set_table *t= set->find_table(table->s);
  if (!t)
  {
    /*
      The entry's creation is where the capture columns are checked
      again.
    */
    MY_BITMAP columns;
    if (my_bitmap_init(&columns, nullptr, table->s->fields))
      return nullptr;
    bool error= table->triggers->get_capture_columns(TRG_GRANULARITY_STATEMENT,
                                                     &columns, true);
    if (!error)
    {
      /*
        The row images hold every captured column: mark_fields_used()
        marked them for read, whatever the statement itself reads, and a
        replica that runs triggers reads them whatever the row image of
        the event holds.
      */
      DBUG_ASSERT(bitmap_is_subset(&columns, table->read_set));
      StringBuffer<256> names;
      table->triggers->print_capture_columns(&columns, &names);
      trg_hook_trace("statement set: entry for %s.%s created, columns: %s",
                     table->s->db.str, table->s->table_name.str,
                     names.c_ptr_safe());
    }
    my_bitmap_free(&columns);
    if (error)
      return nullptr;
    if (!(t= new (thd->mem_root) Trg_stmt_set_table()))
      return nullptr;
    t->next= nullptr;
    t->table= table;
    bzero((char *) t->rows, sizeof(t->rows));
    *set->tables_last= t;
    set->tables_last= &t->next;
  }
  return t;
}


/* Capture a row into the statement set of the statement now executing */

static bool trg_stmt_capture(THD *thd, TABLE *table, trg_event_type event,
                             const char *images)
{
  Trg_stmt_set_table *t= trg_stmt_set_entry(thd, table);
  if (!t)
    return true;
  t->rows[event]++;
  trg_hook_trace("capture %s row of %s.%s through %s (%s) into the "
                 "statement set, now %llu %s rows%s",
                 trg_event_type_names[trg2bit(event)].str,
                 table->s->db.str, table->s->table_name.str,
                 table->alias.c_ptr_safe(), images, (ulonglong) t->rows[event],
                 trg_event_type_names[trg2bit(event)].str,
                 thd->in_sub_stmt ? " (inside a sub-statement)" : "");
  return false;
}


/*
  What INSERTING, UPDATING and DELETING read in the bodies of the FOR EACH
  STATEMENT triggers of a statement: they name the statement that fired
  the triggers, as in Oracle: INSERT for the whole INSERT family, REPLACE
  and INSERT ... ON DUPLICATE KEY UPDATE included, and UPDATE or DELETE
  also when FOR PORTION OF captures INSERT rows, whatever events of the
  table have triggers.  The replication applier (row_events) runs no such
  statement, and its lex->sql_command says nothing of one: there they
  name the first of INSERT, UPDATE and DELETE whose rows the table's
  FOR EACH STATEMENT triggers captured.
*/

static trg_event_type trg_statement_kind(THD *thd, bool row_events,
                                         const ha_rows rows[TRG_EVENT_MAX])
{
  if (!row_events)
  {
    switch (thd->lex->sql_command) {
    case SQLCOM_UPDATE:
    case SQLCOM_UPDATE_MULTI:
      return TRG_EVENT_UPDATE;
    case SQLCOM_DELETE:
    case SQLCOM_DELETE_MULTI:
      return TRG_EVENT_DELETE;
    default:
      /* Only the INSERT family captures rows besides these */
      DBUG_ASSERT(thd->lex->sql_command == SQLCOM_INSERT ||
                  thd->lex->sql_command == SQLCOM_INSERT_SELECT ||
                  thd->lex->sql_command == SQLCOM_LOAD ||
                  thd->lex->sql_command == SQLCOM_REPLACE ||
                  thd->lex->sql_command == SQLCOM_REPLACE_SELECT);
      return TRG_EVENT_INSERT;
    }
  }
  return rows[TRG_EVENT_INSERT] ? TRG_EVENT_INSERT :
         rows[TRG_EVENT_UPDATE] ? TRG_EVENT_UPDATE : TRG_EVENT_DELETE;
}


/*
  End of statement: called from the function that executes a DML
  statement, after it wrote its last row (the row loop, do_updates(),
  do_deletes(), ha_end_bulk_insert()) and before it computes the
  statement's error code and writes the statement to the binary log:
  mysql_insert(), select_insert::prepare_eof(), mysql_load(),
  Sql_cmd_update::update_single_table(), multi_update::send_eof(),
  Sql_cmd_delete::delete_from_single_table() and
  multi_delete::send_eof(); and from Rows_log_event::do_apply_event() at
  the end of a replicated statement (STMT_END_F).  The tables are still
  open and locked (prelocked), the statement transaction is not yet
  committed or rolled back, no OK is set yet, and the statement's
  max_statement_time still applies, as it does to ROW bodies.  It is
  reached by statements run inside stored routines and trigger bodies
  too.

  A failing body fails the statement like a failing AFTER ROW trigger:
  the DML function sees the error before it logs, so a statement that
  changed a table that cannot roll back is logged with the body's error
  code, and the statement transaction is rolled back.

  A statement that fails before its function gets here keeps its set
  until mysql_execute_command() drops it at finish:, also with this
  function.

  Fires the STATEMENT triggers of every table the statement captured a
  row of, in the order it first reached them, each trigger once if the
  statement captured a row of one of its events.  A statement that
  matched no row has no set and fires nothing.  A failed statement fires
  nothing.

  @return true if a body failed; the statement fails.  Never true when
          statement_failed is.
*/

bool trg_fire_statement_triggers(THD *thd, bool statement_failed,
                                 bool row_events)
{
  Trg_stmt_set *set= thd->lex->trg_stmt_set;
  if (!set)
    return false;
  thd->lex->trg_stmt_set= nullptr;

  for (Trg_stmt_set_table *t= set->tables; t; t= t->next)
  {
    TABLE *table= t->table;
    /* The statement's tables are open until its set is gone */
    DBUG_ASSERT(table->in_use == thd);
    if (statement_failed)
    {
      trg_hook_trace("end of statement: statement failed, %llu INSERT, "
                     "%llu UPDATE, %llu DELETE rows dropped from the "
                     "statement set of %s.%s",
                     (ulonglong) t->rows[TRG_EVENT_INSERT],
                     (ulonglong) t->rows[TRG_EVENT_UPDATE],
                     (ulonglong) t->rows[TRG_EVENT_DELETE],
                     table->s->db.str, table->s->table_name.str);
      continue;
    }
    Table_triggers_list *triggers= table->triggers;
    const trg_event_type stmt_kind=
      trg_statement_kind(thd, row_events, t->rows);
    Trigger_event_guard event_guard(thd, stmt_kind);
    /*
      In the order of ROW triggers: the list of each event the statement
      captured a row of, INSERT, UPDATE, DELETE; a trigger on several of
      them fires once, in the first of these lists it is on.
    */
    trg_event_set walked= 0;
    for (uint e= 0; e < (uint) TRG_EVENT_MAX; e++)
    {
      if (!t->rows[e])
        continue;
      for (Trigger *tr= triggers->get_trigger(TRG_GRANULARITY_STATEMENT,
                                              (trg_event_type) e,
                                              TRG_ACTION_AFTER);
           tr; tr= tr->next[e])
      {
        if (tr->events & walked)
          continue;
        ha_rows rows[TRG_EVENT_MAX];
        for (uint i= 0; i < (uint) TRG_EVENT_MAX; i++)
          rows[i]= is_trg_event_on(tr->events, (trg_event_type) i) ?
                   t->rows[i] : 0;
        char fired[512];
        /* "true:" is what INSERTING, UPDATING and DELETING read in the body */
        my_snprintf(fired, sizeof(fired),
                    "end of statement: FIRE %s.%s (AFTER %s FOR EACH "
                    "STATEMENT) over %llu INSERT, %llu UPDATE, %llu DELETE "
                    "rows; true: %s; subject TABLE open and locked: %s%s",
                    table->s->db.str, tr->name.str,
                    trg_event_type_names[tr->events].str,
                    (ulonglong) rows[TRG_EVENT_INSERT],
                    (ulonglong) rows[TRG_EVENT_UPDATE],
                    (ulonglong) rows[TRG_EVENT_DELETE],
                    Item_trigger_type_of_statement::
                      clause_name(thd->current_trg_event()).str,
                    table->file->get_lock_type() == F_UNLCK ? "no" : "yes",
                    thd->in_sub_stmt ? " (inside a sub-statement)" : "");
        if (trg_execute_body(thd, table, tr, fired))
          return true;
      }
      walked|= trg2bit((trg_event_type) e);
    }
  }
  return false;
}


/*
  The transaction's accumulator entry for table, created with the
  accumulator itself if need be by the table's first captured row.

  @return nullptr on error
*/

static Trg_commit_table *trg_commit_entry(THD *thd, TABLE *table)
{
  MEM_ROOT *root= &thd->transaction->mem_root;
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  /*
    Every statement that captures a row registers the accumulator, as an
    engine registers itself for every statement that uses it: with the
    statement, and with the transaction when the statement is not the
    whole transaction.  The first captured row of a statement does it.
  */
  if (!thd->ha_data[trg_commit_tp.slot].ha_info[0].is_started())
  {
    trans_register_ha(thd, false, &trg_commit_tp, 0);
    if (thd->in_multi_stmt_transaction_mode())
      trans_register_ha(thd, true, &trg_commit_tp, 0);
  }
  if (!acc)
  {
    if (!(acc= new (root) Trg_commit_accumulator()))
      return nullptr;
    thd->transaction->trg_commit= acc;
    trg_hook_trace("accumulator attached to thd->transaction "
                   "(transaction mem_root)");
  }

  const LEX_CSTRING &db= table->s->db, &name= table->s->table_name;
  Trg_commit_table *t= acc->find_table(db, name);
  if (!t)
  {
    Table_triggers_list *trg= table->triggers;
    my_bitmap_map *bitmap_buf;
    if (!(t= new (root) Trg_commit_table()) ||
        !(bitmap_buf= (my_bitmap_map *)
            alloc_root(root, bitmap_buffer_size(table->s->fields))))
      return nullptr;
    my_bitmap_init(&t->columns, bitmap_buf, table->s->fields);
    /*
      The column set is the union of the columns these triggers capture,
      fixed for the life of the entry.  It is checked here, at the entry's
      creation, because ALTER TABLE does not check it; no entry lives
      across an ALTER TABLE, which commits the transaction first.
    */
    if (trg->get_capture_columns(TRG_GRANULARITY_COMMIT, &t->columns, true))
      return nullptr;
    t->db= safe_lexcstrdup_root(root, db);
    t->name= safe_lexcstrdup_root(root, name);
    t->created_pos= acc->pos;
    t->triggers= nullptr;
    bzero((char *) t->last_end, sizeof(t->last_end));
    bool has_savepoint_triggers= false;
    StringBuffer<256> column_set_from;
    Trg_commit_trigger **triggers_last= &t->triggers;
    for (Trigger *tr= trg->get_commit_triggers(); tr; tr= tr->next_commit)
    {
      Trg_commit_trigger *r= new (root) Trg_commit_trigger();
      if (!r)
        return nullptr;
      r->next= nullptr;
      r->name= safe_lexcstrdup_root(root, tr->name);
      r->events= tr->events;
      r->fires_on_savepoint= tr->fires_on_savepoint;
      r->pos= r->boundary_pos= 0;
      *triggers_last= r;
      triggers_last= &r->next;
      has_savepoint_triggers|= tr->fires_on_savepoint;
      if (column_set_from.length())
        column_set_from.append(STRING_WITH_LEN(", "));
      column_set_from.append(tr->name.str, tr->name.length);
    }
    /* Appended: the tables fire in the order they were first changed */
    t->next= nullptr;
    *acc->tables_last= t;
    acc->tables_last= &t->next;
    StringBuffer<256> columns;
    trg->print_capture_columns(&t->columns, &columns);
    trg_hook_trace("accumulator: new (transaction, table) entry for %s.%s, "
                   "columns: %s; fixed from triggers %s; has OR SAVEPOINT "
                   "triggers: %s",
                   db.str, name.str, columns.c_ptr_safe(),
                   column_set_from.c_ptr_safe(),
                   has_savepoint_triggers ? "yes" : "no");
  }
  return t;
}


/* Capture a row into the transaction's accumulator */

static bool trg_commit_capture(THD *thd, TABLE *table, trg_event_type event,
                               const char *images)
{
  Trg_commit_table *t= trg_commit_entry(thd, table);
  if (!t)
    return true;
  /* As in trg_stmt_set_entry(); per row, as the entry outlives statements */
  DBUG_ASSERT(bitmap_is_subset(&t->columns, table->read_set));
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  if (acc->append(&thd->transaction->mem_root, t, event))
    return true;
  trg_hook_trace("capture %s row of %s.%s (%s) into the transaction "
                 "accumulator at position %llu%s",
                 trg_event_type_names[trg2bit(event)].str,
                 table->s->db.str, table->s->table_name.str, images,
                 acc->pos - 1,
                 thd->in_sub_stmt ? " (inside a sub-statement)" : "");
  return false;
}


/*
  The prelocking strategy of the statement that a commit boundary runs
  its bodies in (see trg_commit_boundary_open()).

  The statement's own tables are the tables whose BEFORE COMMIT triggers
  fire at the boundary.  The statement itself reads and writes none of
  them, so they are opened for read, and open_tables() asks the strategy
  about a table only when the statement writes it.  The bodies of the
  triggers that fire are therefore added in handle_end(), once the own
  tables are open, and open_tables() goes on to open what was added, as
  it does for a multi-table UPDATE.  For a table that a body uses,
  DML_prelocking_strategy does what it does in any statement: it adds the
  triggers of a table a body writes, foreign keys and routines.  Among
  the BEFORE COMMIT triggers of such a table those are added that fire
  nested at this boundary: at a SAVEPOINT only the ones declared OR
  SAVEPOINT (see trg_commit_boundary in Query_tables_list).  The tables
  of the others stay locked by the statement that captured the first
  row, which prelocked the whole chain.
*/

class Trg_commit_prelocking_strategy: public DML_prelocking_strategy
{
  Trg_commit_accumulator *acc;
  const char *boundary;
  bool at_savepoint;
public:
  Trg_commit_prelocking_strategy(Trg_commit_accumulator *acc_arg,
                                 const char *boundary_arg,
                                 bool at_savepoint_arg)
   :acc(acc_arg), boundary(boundary_arg), at_savepoint(at_savepoint_arg)
  {}
  bool handle_end(THD *thd) override;
};


/*
  open_tables() calls this each time it reaches the end of the table
  list, and it can start over from the first table, after a table sent it
  to close everything and try again.  The bodies are added once, by the
  first call, which marks the statement as prelocked: when open_tables()
  starts over, it either keeps what was added, and the list then holds
  the tables of the bodies too, or removes it along with the mark.
*/

bool Trg_commit_prelocking_strategy::handle_end(THD *thd)
{
  LEX *lex= thd->lex;
  if (lex->requires_prelocking())
    return false;

  /* Nothing was added yet: the list holds the statement's own tables */
  TABLE_LIST **own_last= lex->query_tables_last;
  for (TABLE_LIST *tl= lex->query_tables; tl; tl= tl->next_global)
  {
    Trg_commit_table *t= acc->find_table(tl->db, tl->table_name);
    Table_triggers_list *triggers= tl->table->triggers;
    /* Trigger DDL waits for the transaction's metadata lock on the table */
    DBUG_ASSERT(t && triggers);
    StringBuffer<256> names;
    for (Trigger *tr= t && triggers ? triggers->get_commit_triggers() :
                                      nullptr;
         tr; tr= tr->next_commit)
    {
      Trg_commit_trigger *r= t->find_trigger(tr->name);
      if (!r || !acc->trigger_has_unfired(t, r, at_savepoint))
        continue;
      Table_triggers_list::add_trigger_to_prelocking_set(thd, lex, tl, tr);
      if (names.length())
        names.append(STRING_WITH_LEN(", "));
      names.append(tr->name.str, tr->name.length);
    }
    trg_hook_trace("%s: prelocking for %s.%s: the bodies of the triggers "
                   "that have rows to fire over (%s) are prelocked by the "
                   "boundary's statement",
                   boundary, tl->db.str, tl->table_name.str,
                   names.c_ptr_safe());
    if (&tl->next_global == own_last)
      break;
  }
  lex->mark_as_requiring_prelocking(own_last);
  return false;
}


/*
  The statement that a commit boundary runs its bodies in.

  Every statement closes its tables at its end, so at COMMIT, SAVEPOINT
  and XA END of a multi-statement transaction the tables whose triggers
  fire are closed.  The boundary opens and locks them as a statement
  opens its tables, with prelocking (Trg_commit_prelocking_strategy),
  which adds every table and routine the bodies use: the bodies run as
  ROW trigger bodies do, against a prelocked set.  The tables are the
  statement's own, opened for read: the transaction's MDL_SHARED_WRITE on
  each is still held, so opening them takes no new metadata lock.  A
  session temporary table of the same name is not the subject table, so
  only base tables are opened.  The metadata locks on the tables of the
  bodies are held too: every statement that captured a row prelocked
  them (see add_tables_and_routines_for_triggers()).  What the boundary
  can still wait for is what any statement that writes waits for after
  its metadata locks: the global read lock and a backup, and the table
  locks of an engine that has them.

  A boundary is not parsed, so it has a LEX of its own, and what
  prelocking allocates goes to a memory root of its own through a
  conventional statement arena: a COMMIT that a stored procedure or a
  prepared statement executes must not leave the boundary's table list
  on the memory root of the procedure or of the prepared statement.
  Such a statement may run with a reprepare observer, which checks what
  the statement opens against the versions it was prepared with.  The
  boundary's statement was never prepared, so it runs without one, as
  the body of a stored routine does.

  The statement that reached the boundary may have tables of its own
  open and locked: SET autocommit= 1 commits while the tables that its
  expressions read are open, and with a stored function among them the
  statement is a prelocked one.  They are not the boundary's tables, and
  the statement goes on using them after the commit.  Its open tables,
  its locks and its reprepare observer are set aside while the
  boundary's statement runs (old_open_tables), as they are for a
  statement that opens a system table; the session's temporary tables
  stay, since the bodies see them.

  Locking the tables decides the binary log format of the boundary's
  statement, from the tables the bodies write and from what makes a body
  unsafe to log as a statement (see Trg_commit_binlog_format).

  The statement ends as any other: its statement transaction is
  committed, or rolled back if a body failed, and its tables are closed.
  The statement transaction is the scope that the bodies' writes are
  rolled back to when a body fails at a SAVEPOINT.
*/

struct Trg_commit_boundary_statement
{
  MEM_ROOT mem_root;
  Query_arena arena, backup_arena, *old_stmt_arena= nullptr;
  LEX *old_lex= nullptr;
  Open_tables_state old_open_tables;
  LEX lex;
  Trg_commit_prelocking_strategy strategy;
  bool locked= false;                           // tables are open and locked

  Trg_commit_boundary_statement(Trg_commit_accumulator *acc,
                                const char *boundary, bool at_savepoint)
   :arena(&mem_root, Query_arena::STMT_CONVENTIONAL_EXECUTION),
    strategy(acc, boundary, at_savepoint)
  {}
};


/*
  Open and lock the tables whose triggers fire at the boundary and what
  their bodies use.

  @return true on error: nothing is open, and the caller ends the
          statement with trg_commit_boundary_close()
*/

static bool trg_commit_boundary_open(THD *thd,
                                     Trg_commit_boundary_statement *stmt,
                                     Trg_commit_accumulator *acc,
                                     const char *boundary, bool at_savepoint)
{
  init_sql_alloc(PSI_INSTRUMENT_ME, &stmt->mem_root, 1024, 0,
                 MYF(MY_THREAD_SPECIFIC));
  stmt->old_stmt_arena= thd->stmt_arena;
  thd->stmt_arena= &stmt->arena;
  thd->set_n_backup_active_arena(&stmt->arena, &stmt->backup_arena);
  stmt->old_open_tables.set_open_tables_state(thd);
  thd->reset_open_tables_state();
  thd->temporary_tables= stmt->old_open_tables.temporary_tables;
  stmt->old_lex= thd->lex;
  thd->lex= &stmt->lex;
  lex_start(thd);
  stmt->lex.sql_command= SQLCOM_SELECT;
  stmt->lex.trg_commit_boundary= at_savepoint ? LEX::TRG_BOUNDARY_SAVEPOINT :
                                                LEX::TRG_BOUNDARY_COMMIT;

  for (Trg_commit_table *t= acc->tables; t; t= t->next)
  {
    if (!acc->has_unfired(t, at_savepoint))
      continue;
    TABLE_LIST *tl= thd->alloc<TABLE_LIST>(1);
    if (!tl)
      return true;
    tl->init_one_table(&t->db, &t->name, nullptr, TL_READ);
    tl->open_type= OT_BASE_ONLY;
    stmt->lex.add_to_query_tables(tl);
  }

  trg_hook_trace("%s: the boundary's statement opens and locks the tables "
                 "whose triggers fire and the tables their bodies use",
                 boundary);
  /* The first boundary of the thread times out waiting for a lock */
  DBUG_EXECUTE_IF("trg_commit_boundary_lock_wait_timeout_once",
                  if (!DBUG_IF("trg_commit_boundary_timed_out_once"))
                  {
                    DBUG_SET("+d,trg_commit_boundary_timed_out_once");
                    my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
                    return true;
                  });
  /* The first boundary of the thread cannot reach a remote table */
  DBUG_EXECUTE_IF("trg_commit_boundary_connect_error_once",
                  if (!DBUG_IF("trg_commit_boundary_connect_failed_once"))
                  {
                    DBUG_SET("+d,trg_commit_boundary_connect_failed_once");
                    my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0),
                             "injected");
                    return true;
                  });
  if (open_and_lock_tables(thd, stmt->lex.query_tables, false, 0,
                           &stmt->strategy))
    return true;
  stmt->locked= true;

  /*
    The statement does not use its own tables: a body may write a table
    whose triggers fire at this boundary too, unless the table is on the
    firing chain (see trg_chain_check_write()).  So they do not count as
    used by the calling statement, which a ROW trigger body may not write.
  */
  TABLE_LIST *first_not_own= stmt->lex.first_not_own_table();
  for (TABLE_LIST *tl= stmt->lex.query_tables; tl && tl != first_not_own;
       tl= tl->next_global)
    tl->table->query_id= 0;

  /*
    With a binary log that is not row-based the replica runs the bodies
    again, so here they have to read under the locks of a statement that
    writes (see open_tables() and read_lock_type_for_table()).
  */
  for (TABLE_LIST *tl= first_not_own; tl; tl= tl->next_global)
  {
    if (!tl->table || tl->prelocking_placeholder != TABLE_LIST::PRELOCK_ROUTINE ||
        tl->table->reginfo.lock_type >= TL_FIRST_WRITE)
      continue;
    thr_lock_type lock_type= tl->table->reginfo.lock_type;
    trg_hook_trace("%s: %s.%s, which the bodies only read, is locked with %s",
                   boundary, tl->db.str, tl->table_name.str,
                   lock_type == TL_READ_NO_INSERT ? "TL_READ_NO_INSERT" :
                   lock_type == TL_READ ? "TL_READ" : "another read lock");
  }
  return false;
}


/*
  End the statement transaction that the bodies of a boundary ran in: it
  is committed, which leaves what they wrote with the transaction, or
  rolled back when the boundary failed.

  @return true if the boundary failed, here or before
*/

static bool trg_commit_end_statement(THD *thd, bool error)
{
  if (!error)
    return trans_commit_stmt(thd);
  trans_rollback_stmt(thd);
  return true;
}


/*
  End the boundary's statement: commit its statement transaction, or roll
  it back when the boundary failed, close its tables, and return to the
  LEX and the arena of the statement that reached the boundary.

  @return true if the boundary failed, here or before
*/

static bool trg_commit_boundary_close(THD *thd,
                                      Trg_commit_boundary_statement *stmt,
                                      bool error)
{
  if (stmt->locked)
  {
    error= trg_commit_end_statement(thd, error);
    /* While the LEX is the boundary's: it says the statement is prelocked */
    close_thread_tables(thd);
  }
  lex_end(&stmt->lex);
  thd->free_items();
  thd->lex= stmt->old_lex;
  /* A body may have created or dropped a temporary table */
  stmt->old_open_tables.temporary_tables= thd->temporary_tables;
  thd->set_open_tables_state(&stmt->old_open_tables);
  thd->stmt_arena= stmt->old_stmt_arena;
  thd->restore_active_arena(&stmt->arena, &stmt->backup_arena);
  free_root(&stmt->mem_root, MYF(0));
  return error;
}


/*
  Remember what a SAVEPOINT boundary returns to if a body fails there.

  A body that fails at a commit boundary (COMMIT, implicit commit, end of
  an autocommit statement, XA END) rolls back the whole transaction, so
  the bodies need no rollback scope there.  At a SAVEPOINT, where the
  transaction stays active, the scope that the bodies' writes are rolled
  back to without touching the rest of the transaction is the statement
  transaction of the statement the boundary runs them in (see
  trg_commit_fire()).  The positions kept here are the accumulator's side
  of that scope.
*/

static void trg_commit_boundary_begin(Trg_commit_accumulator *acc)
{
  acc->boundary_pos= acc->pos;
  acc->boundary_bodies_ran= acc->bodies_ran;
  acc->failed_db= acc->failed_table= acc->failed_trigger= null_clex_str;
  for (Trg_commit_table *t= acc->tables; t; t= t->next)
    for (Trg_commit_trigger *r= t->triggers; r; r= r->next)
      r->boundary_pos= r->pos;
}


/*
  A SAVEPOINT failed at or after its boundary: a body failed, or the
  savepoint could not be set after the bodies ran.  The SAVEPOINT fails
  like a DML statement whose ROW trigger fails: it sets no savepoint,
  and the transaction stays active.  Rows the bodies captured are
  dropped and every watermark returns to where it was, so retrying the
  SAVEPOINT fires every trigger again over the same rows.

  What every body of this boundary wrote, nested firings included, is
  undone on the engine side by the caller: when a body failed, by the
  rollback of the boundary's statement.  When the savepoint could not be
  set, that statement is already committed (see
  trg_commit_savepoint_failed()).

  @param undone  what happened to the bodies' writes, for the trace
*/

static void trg_commit_boundary_rollback(Trg_commit_accumulator *acc,
                                         const char *boundary,
                                         const char *cause,
                                         const char *undone)
{
  trg_hook_trace("%s: %s; %s, rows captured after position %llu "
                 "dropped, watermarks restored; the statement fails, the "
                 "transaction stays active",
                 boundary, cause, undone, acc->boundary_pos);
  acc->truncate(acc->boundary_pos);
  acc->bodies_ran= acc->boundary_bodies_ran;
  for (Trg_commit_table *t= acc->tables; t; t= t->next)
    for (Trg_commit_trigger *r= t->triggers; r; r= r->next)
      r->pos= r->boundary_pos;
}


/*
  A commit boundary failed: a body failed, or the triggers of a table
  could not fire, such as when the boundary's statement could not lock a
  table that a body uses.  The whole transaction is rolled back, as
  SQL:1999 16.6 GR 6b-c has it for a COMMIT whose triggered statements
  fail or that any other error prevents, by the caller of
  trg_commit_fire() once the error is set, an XA branch included, which
  is left ROLLBACK ONLY.  The statement that caused the
  commit fails: COMMIT does not commit, an implicit commit does not run
  its DDL, LOCK TABLES and autocommit stay as they were, and an
  autocommit statement is rolled back with the rest.
*/

static void trg_commit_boundary_abort(THD *thd, const char *boundary,
                                      const char *cause)
{
  trg_hook_trace("%s: %s; the transaction is rolled back, and the "
                 "statement that caused the commit fails", boundary, cause);
  thd->trg_commit_failed_query_id= thd->query_id;
}


/*
  The statement's error after a failed commit boundary says that the
  transaction was rolled back, as SQL:1999 16.6 GR 6 requires: SQLSTATE
  class 40.  An error that says so already, a deadlock, stays the
  statement's error, whether a body raised it or the boundary's statement
  did when it locked its tables, so that clients and replicas recognize it
  and retry the transaction.  Any other cause stays before the new error
  as a further condition of the statement:
    - a body's error before ER_TRG_BEFORE_COMMIT_ROLLBACK (40004,
      triggered action exception);
    - a lock wait timeout of the boundary's statement before
      ER_LOCK_WAIT_TIMEOUT_ROLLBACK (40001, serialization failure): the
      transaction lost to a concurrent one, as after a deadlock, and
      ER_LOCK_WAIT_TIMEOUT itself does not say that it was rolled back;
    - any other failure, such as KILL QUERY, before
      ER_TRG_BEFORE_COMMIT_NOT_FIRED (40000).

  An XA branch gets the XA_RB* error of the cause instead, as after a
  deadlock or a lock wait timeout in the branch itself.

  @return the error number of the cause
*/

static uint trg_commit_boundary_error(THD *thd, bool xa)
{
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  Diagnostics_area *da= thd->get_stmt_da();
  uint cause= da->is_error() ? da->sql_errno() : 0;
  if (xa)
  {
    thd->clear_error();
    my_error(cause == ER_LOCK_DEADLOCK ? ER_XA_RBDEADLOCK :
             cause == ER_LOCK_WAIT_TIMEOUT ? ER_XA_RBTIMEOUT :
             ER_XA_RBROLLBACK, MYF(0));
    return cause;
  }
  if (cause && !strncmp(da->get_sqlstate(), "40", 2))
    return cause;
  thd->clear_error();
  if (acc->failed_trigger.str)
    my_error(ER_TRG_BEFORE_COMMIT_ROLLBACK, MYF(0), acc->failed_db.str,
             acc->failed_trigger.str);
  else if (cause == ER_LOCK_WAIT_TIMEOUT)
    my_error(ER_LOCK_WAIT_TIMEOUT_ROLLBACK, MYF(0));
  else
    my_error(ER_TRG_BEFORE_COMMIT_NOT_FIRED, MYF(0),
             safe_str(acc->failed_db.str), safe_str(acc->failed_table.str));
  return cause;
}


/*
  A boundary of the current statement failed.  What the statement would
  do after a commit that succeeded must not happen: ending LOCK TABLES.
*/

bool trg_commit_boundary_failed(THD *thd)
{
  return thd->trg_commit_failed_query_id == thd->query_id;
}


static bool trg_commit_fire_nested(THD *thd, Trg_commit_accumulator *acc,
                                   ulonglong from, const char *boundary,
                                   bool at_savepoint);


/*
  Fire the BEFORE COMMIT triggers of one table that have rows they have
  not seen yet, then, nested under it, every table the bodies wrote.
*/

static bool trg_commit_fire_table(THD *thd, Trg_commit_accumulator *acc,
                                  Trg_commit_table *t, const char *boundary,
                                  bool at_savepoint)
{
  /*
    A table none of whose triggers has a row it has not fired over is
    not reached at all: its rows were truncated away, its triggers have
    fired over them already, or the ones that have not do not fire on
    SAVEPOINT.
  */
  if (!acc->has_unfired(t, at_savepoint))
    return false;

  /*
    The firing chain: the table whose triggers run, and the tables whose
    triggers led to it.  A body may not write a table on it; that is
    checked in lock_tables() by trg_chain_check_write().
  */
  Trg_firing_link link= { thd->trg_firing_chain, t->db, t->name };
  uint depth= 1;
  for (Trg_firing_link *l= link.prev; l; l= l->prev)
    depth++;
  thd->trg_firing_chain= &link;

  bool error= false;
  bool mdl_held= thd->mdl_context.is_lock_owner(MDL_key::TABLE, t->db.str,
                                                 t->name.str,
                                                 MDL_SHARED_WRITE);
  TABLE *table= find_locked_table(thd->open_tables, t->db.str, t->name.str);
  ulonglong bodies_from= acc->pos;
  if (table && thd->lex->trg_commit_boundary != LEX::TRG_BOUNDARY_NONE)
  {
    /*
      The boundary's own statement opened and locked the table, as one of
      its own tables or, for a nested firing, as a table that a body
      writes, together with the tables of the bodies.
    */
    trg_hook_trace("%s: %s.%s enters the firing chain at depth %u; it is "
                   "open and locked by the boundary's statement, "
                   "transactional MDL_SHARED_WRITE held: %s; bodies would "
                   "run on the tables the boundary prelocked",
                   boundary, t->db.str, t->name.str, depth,
                   mdl_held ? "yes" : "NO");
  }
  else if (table)
  {
    /*
      Under LOCK TABLES the bodies run against the locked tables, which
      include the tables of BEFORE COMMIT bodies (see
      add_tables_and_routines_for_triggers()).  A statement on a table
      with triggers is prelocked, so under LOCK TABLES it runs as
      LTM_PRELOCKED_UNDER_LOCK_TABLES.  Otherwise this is the end of an
      autocommit statement, which prelocked the tables of the bodies with
      its own, and the bodies run against those.  A statement that is not
      prelocked has only its own tables open and locked.
    */
    const char *body_tables=
      thd->locked_tables_mode == LTM_LOCK_TABLES ||
      thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES ?
      "on the tables LOCK TABLES locked" :
      thd->locked_tables_mode == LTM_PRELOCKED ?
      "on the tables the statement prelocked" :
      "with only the statement's own tables locked";
    trg_hook_trace("%s: %s.%s enters the firing chain at depth %u; it is "
                   "still open in thd->open_tables, transactional "
                   "MDL_SHARED_WRITE held: %s; bodies would run %s",
                   boundary, t->db.str, t->name.str, depth,
                   mdl_held ? "yes" : "NO", body_tables);
  }
  else
  {
    /*
      Every table that a boundary fires for is locked before the first
      body runs: by the boundary's statement, by LOCK TABLES, or by the
      autocommit statement that is the transaction.  Nothing can be
      locked once bodies have run.
    */
    trg_hook_trace("%s: %s.%s enters the firing chain at depth %u; it is "
                   "not among the locked tables, transactional "
                   "MDL_SHARED_WRITE held: %s; its triggers cannot fire",
                   boundary, t->db.str, t->name.str, depth,
                   mdl_held ? "yes" : "NO");
    my_error(ER_TABLE_NOT_LOCKED, MYF(0), t->name.str);
    error= true;
  }

  Table_triggers_list *trg_list= table ? table->triggers : nullptr;
  /*
    One list in firing order, whatever the triggers' events: they all
    fire at this one boundary.
  */
  for (Trigger *tr= trg_list ? trg_list->get_commit_triggers() : nullptr;
       tr; tr= tr->next_commit)
  {
    if (at_savepoint && !tr->fires_on_savepoint)
      continue;

    /* The table's triggers are those the entry was created with */
    Trg_commit_trigger *w= t->find_trigger(tr->name);
    DBUG_ASSERT(w);
    if (!w)
      continue;
    ulonglong from= w->pos;
    ulonglong per_event[TRG_EVENT_MAX]= {0, 0, 0};
    if (!acc->entries_after(t, tr->events, from, per_event))
      continue;

    char fired[512];
    my_snprintf(fired, sizeof(fired),
                "%s: FIRE %s.%s (AFTER %s BEFORE COMMIT%s, order %u) "
                "over positions [%llu, %llu): %llu INSERT, %llu UPDATE, "
                "%llu DELETE rows",
                boundary, t->db.str, tr->name.str,
                trg_event_type_names[tr->events].str,
                tr->fires_on_savepoint ? " OR SAVEPOINT" : "",
                tr->commit_order, from, acc->pos,
                per_event[TRG_EVENT_INSERT],
                per_event[TRG_EVENT_UPDATE],
                per_event[TRG_EVENT_DELETE]);
    ulonglong upto= acc->pos;
    /* Before the body runs: a ROLLBACK must know that bodies wrote */
    acc->bodies_ran= true;
    if (trg_execute_body(thd, table, tr, fired))
    {
      acc->failed_trigger= safe_lexcstrdup_root(&thd->transaction->mem_root,
                                                tr->name);
      error= true;
      break;
    }

    /* Rows the body itself captured are for the nested firings */
    w->pos= upto;
  }

  /*
    Every table the bodies wrote fires now, nested under this one and
    before the next table of the boundary, over the rows written since
    its previous firing.  The table may have fired already at this
    boundary; it fires again over the new rows.
  */
  if (!error)
    error= trg_commit_fire_nested(thd, acc, bodies_from, boundary,
                                  at_savepoint);

  /* The innermost table of a failure names it, a body's or not */
  if (error && !acc->failed_table.str)
  {
    acc->failed_db= t->db;
    acc->failed_table= t->name;
  }
  thd->trg_firing_chain= link.prev;
  return error;
}


static bool trg_commit_fire_nested(THD *thd, Trg_commit_accumulator *acc,
                                   ulonglong from, const char *boundary,
                                   bool at_savepoint)
{
  if (acc->pos == from)
    return false;
  for (Trg_commit_table *t= acc->tables; t; t= t->next)
    if (acc->has_entries_after(t, trg_all_events, from) &&
        trg_commit_fire_table(thd, acc, t, boundary, at_savepoint))
      return true;
  return false;
}


/* Fire the tables of a boundary on the tables that are open and locked */

static bool trg_commit_fire_tables(THD *thd, Trg_commit_accumulator *acc,
                                   const char *boundary, bool at_savepoint)
{
  for (Trg_commit_table *t= acc->tables; t; t= t->next)
    if (trg_commit_fire_table(thd, acc, t, boundary, at_savepoint))
      return true;
  return false;
}


/*
  The binary log format of the statement that the bodies of a boundary run
  in, when it is not the statement that is committing.

  The statement that reached the boundary may be one for which
  mysql_execute_command() set the session's binlog_format to STATEMENT,
  as it does for every command that writes no rows: COMMIT, SAVEPOINT,
  XA END, a DDL statement.  The bodies write rows, so their statement
  starts from the format of the session, as a statement of the client
  does.

  At the end the statement that reached the boundary gets its own format
  back: a DDL statement that committed implicitly is logged as it is
  without the triggers.  It also gets back its own pending warnings about
  a statement that is unsafe to log: what the boundary's statement adds
  to them for an unsafe body would otherwise be reported when the
  statement that reached the boundary is logged.  The same holds for the
  decision that a statement is not logged because it writes no replicated
  table: the bodies may write only such tables, and the statement that
  reached the boundary, a DDL statement for one, is logged all the same.
*/

class Trg_commit_binlog_format
{
  THD *thd;
  enum_binlog_format old_session_format, old_stmt_format;
  uint32 old_unsafe_warning_flags;
  THD::binlog_filter_state old_filter_state;
public:
  Trg_commit_binlog_format(THD *thd_arg)
   :thd(thd_arg),
    old_unsafe_warning_flags(thd_arg->binlog_unsafe_warning_flags),
    old_filter_state(thd_arg->get_binlog_local_stmt_filter())
  {
    thd->get_binlog_format(&old_session_format, &old_stmt_format);
    if (thd->binlog_format_before_force != BINLOG_FORMAT_UNSPEC)
      thd->variables.binlog_format= thd->binlog_format_before_force;
    thd->reset_current_stmt_binlog_format_row();
  }
  ~Trg_commit_binlog_format()
  {
    thd->set_binlog_format(old_session_format, old_stmt_format);
    thd->binlog_unsafe_warning_flags= old_unsafe_warning_flags;
    thd->reset_binlog_local_stmt_filter();
    if (old_filter_state == THD::BINLOG_FILTER_SET)
      thd->set_binlog_local_stmt_filter();
    else if (old_filter_state == THD::BINLOG_FILTER_CLEAR)
      thd->clear_binlog_local_stmt_filter();
  }
};


/* Fire the tables of a boundary in a statement of the boundary's own */

static bool trg_commit_fire_in_own_statement(THD *thd,
                                             Trg_commit_accumulator *acc,
                                             const char *boundary,
                                             bool at_savepoint)
{
  Trg_commit_boundary_statement stmt(acc, boundary, at_savepoint);
  bool error=
    trg_commit_boundary_open(thd, &stmt, acc, boundary, at_savepoint) ||
    trg_commit_fire_tables(thd, acc, boundary, at_savepoint);
  return trg_commit_boundary_close(thd, &stmt, error);
}


/*
  Fire the BEFORE COMMIT triggers that have rows they have not seen yet,
  table by table in the order the transaction first changed the tables.

  The bodies run in a statement:
    - at the end of an autocommit statement, in that statement, which is
      committing: its tables are still open and locked, with the tables
      of the bodies, which it prelocked;
    - otherwise in a statement of the boundary's own, which ends with a
      statement commit, or a statement rollback if the boundary failed,
      before the caller commits the transaction or sets the savepoint.
      Its tables are those LOCK TABLES locked, or they are opened and
      locked by trg_commit_boundary_open(), also when the statement that
      reached the boundary has tables of its own open.
  A boundary with nothing to fire opens nothing.

  @param boundary       what is happening, for the trace
  @param at_savepoint   only triggers declared OR SAVEPOINT fire
  @param statement_end  the end of an autocommit statement
*/

static bool trg_commit_fire(THD *thd, const char *boundary, bool at_savepoint,
                            bool statement_end)
{
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  /* The statement commit of a boundary's own statement is no boundary */
  if (!acc || acc->in_boundary)
    return false;

  trg_hook_trace("%s: boundary reached, accumulator holds %llu rows, "
                 "in_sub_stmt=%u, open tables in THD: %s",
                 boundary, acc->rows_from(0), thd->in_sub_stmt,
                 thd->open_tables ? "yes" : "none");

  trg_commit_boundary_begin(acc);

  Trg_commit_table *first= acc->tables;
  while (first && !acc->has_unfired(first, at_savepoint))
    first= first->next;
  if (!first)
    return false;

  /*
    A transaction that uses an engine without savepoint support cannot
    set a savepoint: ha_savepoint() refuses it.  The SAVEPOINT is refused
    here, before its triggers fire, and so fails as it does without
    them.  The engine of every table that a body uses is in the
    transaction by now: the statements that captured the rows locked
    those tables (see add_tables_and_routines_for_triggers()).
  */
  if (at_savepoint && ha_savepoint_check(thd))
  {
    trg_hook_trace("%s: an engine of the transaction cannot set a "
                   "savepoint; the statement fails, no trigger fires and "
                   "the rows stay for the next boundary", boundary);
    return true;
  }

  acc->in_boundary= true;
  bool error;
  if (statement_end)
    error= trg_commit_fire_tables(thd, acc, boundary, at_savepoint);
  else
  {
    Trg_commit_binlog_format binlog_format(thd);
    if (thd->locked_tables_mode != LTM_LOCK_TABLES &&
        thd->locked_tables_mode != LTM_PRELOCKED_UNDER_LOCK_TABLES)
      error= trg_commit_fire_in_own_statement(thd, acc, boundary,
                                              at_savepoint);
    else
    {
      /* Under LOCK TABLES: the tables stay, the statement ends */
      error= trg_commit_fire_tables(thd, acc, boundary, at_savepoint);
      error= trg_commit_end_statement(thd, error);
    }
  }
  /* The transaction is rolled back by the caller, not by the above */
  DBUG_ASSERT(thd->transaction->trg_commit == acc);
  acc->in_boundary= false;

  if (error)
  {
    /* The first table that was to fire names a failure before any fired */
    if (!acc->failed_table.str)
    {
      acc->failed_db= first->db;
      acc->failed_table= first->name;
    }
    const char *cause= acc->failed_trigger.str ? "a body failed" :
                       "the triggers could not fire";
    if (at_savepoint)
      trg_commit_boundary_rollback(acc, boundary, cause,
                                   "the boundary's statement is rolled "
                                   "back, and with it what the bodies "
                                   "wrote");
    else
      trg_commit_boundary_abort(thd, boundary, cause);
  }
  return error;
}


/*
  The end of the transaction, before anything that ends it.  The
  transaction is still active: trigger writes would join it.  On failure
  the error is set while the accumulator still names the failed trigger,
  and the caller rolls the transaction back.
*/

static bool trg_commit_fire_at_end(THD *thd, const char *kind,
                                   bool statement_end)
{
  if (!thd->transaction->trg_commit)
    return false;
  char boundary[160];
  my_snprintf(boundary, sizeof(boundary), "%s (statement: %.60s)", kind,
              safe_str(thd->query()));
  if (!trg_commit_fire(thd, boundary, false, statement_end))
    return false;
  trg_commit_boundary_error(thd, false);
  return true;
}


/*
  trans_commit(), trans_commit_implicit() and trans_begin(), before they
  clear the transaction state.  On failure the transaction is rolled back
  here, so that it is over whoever called them: a statement, or
  COM_REFRESH, which commits outside any statement.  A parallel replica
  rolls back later, as it does when ha_commit_trans() fails (MDEV-7458).
*/

bool trg_commit_fire_at_commit(THD *thd, const char *kind)
{
  if (!trg_commit_fire_at_end(thd, kind, false))
    return false;
  if (!(thd->rgi_slave && thd->rgi_slave->is_parallel_exec))
    trans_rollback_implicit(thd);
  return true;
}


/*
  ha_commit_trans() at the end of an autocommit statement, where the
  statement transaction is the transaction.  On failure ha_commit_trans()
  rolls it back, as it does when the commit itself fails.
*/

bool trg_commit_fire_at_autocommit_end(THD *thd)
{
  return trg_commit_fire_at_end(thd, "COMMIT of an autocommit statement",
                                true);
}


/*
  XA END is where an XA branch stops doing work: after it the branch is
  IDLE and accepts no DML, and XA PREPARE / XA COMMIT ONE PHASE run from
  IDLE.  XA START ... RESUME makes it ACTIVE again, and the next XA END
  fires over what it did since.  So BEFORE COMMIT triggers of an XA
  transaction fire here, not in ha_commit_trans() or ha_prepare().  A
  failure rolls the branch back and leaves it ROLLBACK ONLY, as a
  deadlock does, with the XA_RB* error of its cause: XA_RBDEADLOCK for a
  deadlock in a body, XA_RBTIMEOUT for a lock wait timeout, XA_RBROLLBACK
  otherwise.
*/

bool trg_commit_fire_at_xa_end(THD *thd)
{
  if (!trg_commit_fire(thd, "XA END", false, false))
    return false;
  uint cause= trg_commit_boundary_error(thd, true);
  /* What the end of a statement does after a deadlock in the branch */
  XID_STATE &xid_state= thd->transaction->xid_state;
  xid_state.set_error(cause);
  xid_state.set_rollback_only();
  trans_rollback_implicit(thd);
  return true;
}


/*
  trans_savepoint(), before the savepoint is established.  A SAVEPOINT
  fires when no statement is in progress: when the client sends it, or
  when a stored procedure that the client called, directly or through
  other procedures, executes it.  One executed while a statement is
  running, inside a trigger body, a stored function, or a procedure
  called from either, only records its boundary, and the rows stay for
  the next SAVEPOINT that fires them, or COMMIT.
*/

bool trg_commit_fire_at_savepoint(THD *thd)
{
  if (!thd->transaction->trg_commit)
    return false;
  if (thd->in_sub_stmt)
  {
    trg_hook_trace("SAVEPOINT inside a sub-statement: marker only, OR "
                   "SAVEPOINT triggers fire at the next SAVEPOINT that "
                   "fires them, or COMMIT");
    return false;
  }
  return trg_commit_fire(thd, "SAVEPOINT", true, false);
}


/*
  trans_savepoint() could not set the savepoint after
  trg_commit_fire_at_savepoint() succeeded: the boundary is undone as if
  a body had failed there.  An engine without savepoint support is
  refused before the triggers fire (see trg_commit_fire()), so this is
  left for a savepoint that cannot be allocated, an older savepoint of
  the same name that cannot be released, and an engine that joins the
  transaction only when a body first writes to it.  Stub for the engine
  side: the statement the bodies ran in is committed by now, so the real
  code needs a scope around the boundary and the setting of the
  savepoint together, an implicit savepoint taken before the boundary,
  and rolls back to it here.
*/

void trg_commit_savepoint_failed(THD *thd)
{
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  /* The conditions under which trg_commit_fire_at_savepoint() fired */
  if (acc && !thd->in_sub_stmt)
    trg_commit_boundary_rollback(acc, "SAVEPOINT",
                                 "the savepoint could not be set",
                                 "the bodies' writes would be rolled back "
                                 "to an implicit savepoint taken before "
                                 "the boundary");
}


/*
  trans_savepoint(), trans_rollback_to_savepoint() and
  trans_release_savepoint() while a BEFORE COMMIT body runs.  Stub: the
  sketch runs no body, so this is never reached; the real code fails the
  statement.

  A BEFORE COMMIT body may not set, roll back to or release a savepoint,
  whether it is written in the body itself (refused at CREATE TRIGGER,
  see trg_commit_check_body_at_create()) or in a routine the body calls
  (refused here): the boundary owns the savepoints under it.

  @return true if the statement is refused
*/

bool trg_commit_body_refuses(THD *thd, const char *statement)
{
  trg_hook_trace("%s inside a BEFORE COMMIT body (firing %s.%s): would "
                 "fail with ER_STMT_NOT_ALLOWED_IN_SF_OR_TRG", statement,
                 thd->trg_firing_chain->db.str,
                 thd->trg_firing_chain->name.str);
  return false;
}


/*
  CREATE TRIGGER ... BEFORE COMMIT whose body sets, rolls back to or
  releases a savepoint.  Stub: the real code refuses the trigger the way
  sp_head::is_not_allowed_in_function() refuses COMMIT in a trigger,
  with ER_STMT_NOT_ALLOWED_IN_SF_OR_TRG naming the statement.
  FOR EACH STATEMENT bodies follow ROW bodies and may use savepoints.
*/

void trg_commit_check_body_at_create(THD *thd, LEX *lex)
{
  if (lex->trg_chistics.granularity == TRG_GRANULARITY_COMMIT &&
      (lex->sphead->m_flags & sp_head::HAS_SAVEPOINT_STMT))
    trg_hook_trace("CREATE TRIGGER %s: the BEFORE COMMIT body has SAVEPOINT, "
                   "ROLLBACK TO SAVEPOINT or RELEASE SAVEPOINT: would fail "
                   "with ER_STMT_NOT_ALLOWED_IN_SF_OR_TRG",
                   lex->spname->m_name.str);
}


/*
  lock_tables() while a BEFORE COMMIT body runs.  Stub: the sketch runs no
  body, so this is never reached; the real code fails the statement.

  A body may not write a table on the firing chain: its own table, or a
  table whose triggers led to its firing.  This is the BEFORE COMMIT form
  of the check lock_tables() makes for a ROW trigger body, which may not
  write a table its calling statement uses (ER_CANT_UPDATE_USED_TABLE_IN_
  SF_OR_TRG).  That check relies on the calling statement's prelocked
  tables, and at a commit boundary there is no calling statement.  It
  covers every statement under the body, at any depth; reading a table on
  the chain is allowed.

  @return true if the statement is refused
*/

bool trg_chain_check_write(THD *thd, TABLE_LIST *tables)
{
  for (TABLE_LIST *tl= tables; tl; tl= tl->next_global)
  {
    if (tl->placeholder() || tl->lock_type < TL_FIRST_WRITE)
      continue;
    for (Trg_firing_link *l= thd->trg_firing_chain; l; l= l->prev)
      if (tl->table->s->db.streq(l->db) &&
          tl->table->s->table_name.streq(l->name))
        trg_hook_trace("write of %s.%s inside a BEFORE COMMIT body: the "
                       "table is on the firing chain, would fail with "
                       "ER_CANT_UPDATE_USED_TABLE_IN_SF_OR_TRG",
                       l->db.str, l->name.str);
  }
  return false;
}
