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
    - an append-only sequence of captured rows and activations (a
      statement that activated an event on the table and changed no row
      of it); each takes one position;
    - one Trg_commit_table per (transaction, table), shared by every
      BEFORE COMMIT trigger of that table, kept in the order in which the
      transaction first changed the tables, which is the order they fire
      in.  It is created by the table's first captured row or activation,
      and its column set (the union of the columns its triggers capture)
      is fixed then: the table's MDL is held from then on, so its
      triggers cannot change.  A rollback to a point before that position
      removes the entry, and the next row or activation creates it again
      from the triggers the table has then;
    - one watermark per (transaction, trigger): the position up to which
      that trigger has already been fired;
    - SAVEPOINT records the current position in SAVEPOINT::trg_commit_pos
      whether or not any trigger fires on SAVEPOINT;
    - the end of every successful statement inside a transaction records
      the position as the statement mark, so a failed statement can be
      truncated away (the implicit savepoint every statement has).

  The statement sets:
    - one per statement that captured a row or activated an event, in
      LEX::trg_stmt_set: a statement inside a trigger body or a stored
      routine has a LEX, and so a set, of its own;
    - one entry per table, whatever TABLE instance (alias, view) wrote the
      row, so a self-join UPDATE fires the table's triggers once over the
      rows of both aliases; it records the events the statement activated
      on the table, with or without a row;
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
      runs, as they are for an explicit CALL made at that point.

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


/*
  A run of consecutive captured rows of one (table, event), or an
  activation: a statement that activated the event on the table and
  changed no row of it.  An activation takes one position and holds no
  row, so it is truncated and fired over like a row, and a trigger it
  fires sees no row of it.
*/
struct Trg_commit_run: public Sql_alloc
{
  Trg_commit_run *next;
  struct Trg_commit_table *table;
  trg_event_type event;
  ulonglong start;                              // position of the first row
  ulonglong count;
  bool activation;
};


/* Per (transaction, trigger): rows before pos were already fired over */
struct Trg_commit_watermark: public Sql_alloc
{
  Trg_commit_watermark *next;
  LEX_CSTRING trigger_name;
  ulonglong pos;
  /* pos when the boundary now firing started, to restore if it fails */
  ulonglong boundary_pos;
};


/*
  Per (transaction, table).  Created lazily by the first captured row of the
  table, and removed again by a rollback to a point before that row.

  The table's triggers are not copied here: the boundary re-opens the
  table (Subject_table_reopen) and reaches them through its TABLE, as a
  ROW trigger does.  The transaction's metadata lock on the table, held
  since the first captured row, keeps them from changing in between.
*/
struct Trg_commit_table: public Sql_alloc
{
  Trg_commit_table *next;
  LEX_CSTRING db, name;
  ulonglong created_pos;                        // position of the first row
  bool has_savepoint_triggers;
  /* The captured user columns, fixed with the entry; field_index bits */
  MY_BITMAP columns;
  Trg_commit_watermark *watermarks;
  /* Per event, the end of its last run: 0, or one past its last position */
  ulonglong last_end[TRG_EVENT_MAX];

  bool is(const LEX_CSTRING &db_arg, const LEX_CSTRING &name_arg) const
  {
    return Lex_ident_db(db).streq(db_arg) &&
           Lex_ident_table(name).streq(name_arg);
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

  /* Whether t has rows or activations of the given events at or after from */
  static bool has_entries_after(const Trg_commit_table *t,
                                trg_event_set events, ulonglong from)
  {
    for (uint e= 0; e < (uint) TRG_EVENT_MAX; e++)
      if (is_trg_event_on(events, (trg_event_type) e) && t->last_end[e] > from)
        return true;
    return false;
  }

  /*
    The positions at or after from that hold rows or activations of the
    given events of t; per_event counts the rows only.
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
      if (!r->activation)
        per_event[r->event]+= n;
      total+= n;
    }
    return total;
  }

  /* The rows captured at or after position from, of all tables */
  ulonglong rows_from(ulonglong from)
  {
    ulonglong total= 0;
    for (Trg_commit_run *r= runs; r; r= r->next)
      if (!r->activation && r->start + r->count > from)
        total+= r->start + r->count - MY_MAX(r->start, from);
    return total;
  }

  /* The position up to which every trigger of t that fired has fired */
  ulonglong fired_upto(const Trg_commit_table *t)
  {
    ulonglong upto= 0;
    for (Trg_commit_watermark *w= t->watermarks; w; w= w->next)
      upto= MY_MAX(upto, w->pos);
    return upto;
  }

  /* Take the next position for a row or an activation of (t, event) */
  bool append(MEM_ROOT *root, Trg_commit_table *t, trg_event_type event,
              bool activation)
  {
    if (!activation && last_run && !last_run->activation &&
        last_run->table == t && last_run->event == event)
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
      r->activation= activation;
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
      for (Trg_commit_watermark *w= t->watermarks; w; w= w->next)
        w->pos= MY_MIN(w->pos, new_pos);
    }
    for (Trg_commit_run *r= runs; r; r= r->next)
      r->table->last_end[r->event]= r->start + r->count;
    pos= new_pos;
    stmt_mark= MY_MIN(stmt_mark, new_pos);
  }
};


void trg_commit_detach(Trg_commit_accumulator *acc)
{
  trg_hook_trace("transaction end: accumulator of %llu rows freed with the "
                 "transaction mem_root", acc->rows_from(0));
}


/* The rows one statement level captured for one table, all aliases and
   views of the table together, and the events it activated on it */
struct Trg_stmt_set_table: public Sql_alloc
{
  Trg_stmt_set_table *next;
  TABLE *table;                         // the first TABLE that reached it
  ha_rows rows[TRG_EVENT_MAX];
  /* The events whose triggers fire: those that captured a row, and those
     the statement activated without changing a row */
  trg_event_set activated;
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


static bool trg_stmt_capture(THD *thd, TABLE *table, trg_event_type event,
                             const char *images);
static bool trg_commit_capture(THD *thd, TABLE *table, trg_event_type event,
                               const char *images);


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
  statement's first captured row of the table or its activation.  An
  entry belongs to the table, not to the TABLE instance that reached it.

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
    t->activated= 0;
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
  t->activated|= trg2bit(event);
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
  End of statement: called from mysql_execute_command() at finish:, for
  every statement including those run inside stored routines and trigger
  bodies, while the statement's tables are still open and locked and
  before the statement transaction is committed or rolled back; and from
  Rows_log_event::do_apply_event() at the end of a replicated statement
  (STMT_END_F).

  Fires the STATEMENT triggers of every table the statement changed or
  activated an event on, in the order it first reached them, each trigger
  once if the statement activated one of its events, whether or not a row
  of it changed (see trg_activate_statement_events()).  A failed
  statement fires nothing.

  INSERTING, UPDATING and DELETING in the bodies name the statement that
  fired them, as in Oracle: INSERT for the whole INSERT family, REPLACE
  and INSERT ... ON DUPLICATE KEY UPDATE included, and UPDATE or DELETE
  also when FOR PORTION OF activates INSERT, whatever events of the table
  have triggers.  The replication applier (row_events) runs no such
  statement, and its lex->sql_command says nothing of one: there they
  name the first of INSERT, UPDATE and DELETE whose rows the table's
  FOR EACH STATEMENT triggers captured.

  @return true if a body failed; the statement fails
*/

static trg_event_type trg_statement_kind(THD *thd, bool row_events,
                                         trg_event_set activated)
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
      /* Only the INSERT family activates events */
      DBUG_ASSERT(thd->lex->sql_command == SQLCOM_INSERT ||
                  thd->lex->sql_command == SQLCOM_INSERT_SELECT ||
                  thd->lex->sql_command == SQLCOM_LOAD ||
                  thd->lex->sql_command == SQLCOM_REPLACE ||
                  thd->lex->sql_command == SQLCOM_REPLACE_SELECT);
      return TRG_EVENT_INSERT;
    }
  }
  return activated & trg2bit(TRG_EVENT_INSERT) ? TRG_EVENT_INSERT :
         activated & trg2bit(TRG_EVENT_UPDATE) ? TRG_EVENT_UPDATE :
                                                  TRG_EVENT_DELETE;
}


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
      trg_statement_kind(thd, row_events, t->activated);
    Trigger_event_guard event_guard(thd, stmt_kind);
    /*
      In the order of ROW triggers: the list of each event the statement
      activated, INSERT, UPDATE, DELETE; a trigger on several of them
      fires once, in the first of these lists it is on.
    */
    trg_event_set walked= 0;
    for (uint e= 0; e < (uint) TRG_EVENT_MAX; e++)
    {
      if (!(t->activated & trg2bit((trg_event_type) e)))
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
  accumulator itself if need be by the table's first captured row or
  activation.

  @return nullptr on error
*/

static Trg_commit_table *trg_commit_entry(THD *thd, TABLE *table)
{
  MEM_ROOT *root= &thd->transaction->mem_root;
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
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
    t->watermarks= nullptr;
    bzero((char *) t->last_end, sizeof(t->last_end));
    t->has_savepoint_triggers= false;
    StringBuffer<256> column_set_from;
    for (Trigger *tr= trg->get_commit_triggers(); tr; tr= tr->next_commit)
    {
      t->has_savepoint_triggers|= tr->fires_on_savepoint;
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
                   t->has_savepoint_triggers ? "yes" : "no");
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
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  if (acc->append(&thd->transaction->mem_root, t, event, false))
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
  End of a successful statement, before its FOR EACH STATEMENT triggers
  fire: called from mysql_execute_command() at finish:, for every
  statement including those run inside stored routines and trigger
  bodies.  Not from the replication applier, which knows a statement
  only by the rows it brings.

  A data change statement activates the events of its trg_event_map on
  every table it writes, whether or not a row changes, as the SQL
  standard has it for statement-level triggers: REPLACE and LOAD DATA
  ... REPLACE activate INSERT and DELETE, INSERT ... ON DUPLICATE KEY
  UPDATE activates INSERT and UPDATE, and FOR PORTION OF adds INSERT.  An
  INSERT, REPLACE or LOAD DATA writes the table it names, or the one
  table of a view it writes through; the tables it reads activate
  nothing, even when FOR UPDATE write-locks them.  A table that an UPDATE
  or DELETE only reads has a read lock by then and activates nothing; nor
  does one only a trigger or routine of the statement uses (those
  statements activate their own).  DELETE HISTORY activates nothing, as it fires no
  ROW trigger, and neither does EXPLAIN, which executes nothing.

  FOR EACH STATEMENT: the event's bit is set in the table's statement
  set entry.  BEFORE COMMIT: unless an entry of the event that the
  table's triggers have not fired over is already in the accumulator,
  the statement takes one position for it, an activation that holds no
  row.  It is truncated like a row by a statement rollback or ROLLBACK
  TO SAVEPOINT, and it makes the triggers of the event fire at the next
  boundary, over no row of it.

  @return true on error; the statement fails
*/

bool trg_activate_statement_events(THD *thd)
{
  LEX *lex= thd->lex;
  if (lex->describe)
    return false;
  /*
    The table list is read only for these commands: a BINLOG statement
    has freed the tables its row events used by now
  */
  bool first_table_only= false;
  switch (lex->sql_command) {
  case SQLCOM_INSERT:
  case SQLCOM_INSERT_SELECT:
  case SQLCOM_LOAD:
  case SQLCOM_REPLACE:
  case SQLCOM_REPLACE_SELECT:
    /*
      The first table: through a view, its TABLE is the base table the
      statement writes (see check_view_single_update())
    */
    first_table_only= true;
    break;
  case SQLCOM_UPDATE:
  case SQLCOM_UPDATE_MULTI:
  case SQLCOM_DELETE:
  case SQLCOM_DELETE_MULTI:
    /*
      A table they only read has a read lock: a multi-table UPDATE or
      DELETE, and a single-table one run as such (through a view of
      several tables, for one), downgrades the tables it does not write
    */
    break;
  default:
    return false;
  }

  TABLE_LIST *end= lex->first_not_own_table();
  if (first_table_only && lex->query_tables && lex->query_tables != end)
    end= lex->query_tables->next_global;
  for (TABLE_LIST *tl= lex->query_tables; tl && tl != end;
       tl= tl->next_global)
  {
    TABLE *table= tl->table;
    if (tl->prelocking_placeholder || !table || !table->triggers ||
        tl->lock_type < TL_FIRST_WRITE || !tl->trg_event_map)
      continue;
    Table_triggers_list *triggers= table->triggers;
    for (uint e= 0; e < (uint) TRG_EVENT_MAX; e++)
    {
      trg_event_type event= (trg_event_type) e;
      if (!is_trg_event_on(tl->trg_event_map, event) ||
          (event == TRG_EVENT_DELETE && tl->vers_conditions.delete_history))
        continue;

      if (triggers->get_trigger(TRG_GRANULARITY_STATEMENT, event,
                                TRG_ACTION_AFTER))
      {
        Trg_stmt_set_table *t= trg_stmt_set_entry(thd, table);
        if (!t)
          return true;
        if (!(t->activated & trg2bit(event)))
        {
          t->activated|= trg2bit(event);
          trg_hook_trace("statement set: %s activated on %s.%s by a "
                         "statement that changed no %s row of it%s",
                         trg_event_type_names[trg2bit(event)].str,
                         table->s->db.str, table->s->table_name.str,
                         trg_event_type_names[trg2bit(event)].str,
                         thd->in_sub_stmt ? " (inside a sub-statement)" : "");
        }
      }

      if (triggers->get_trigger(TRG_GRANULARITY_COMMIT, event,
                                TRG_ACTION_AFTER))
      {
        Trg_commit_accumulator *acc= thd->transaction->trg_commit;
        Trg_commit_table *t= acc ? acc->find_table(table->s->db,
                                                   table->s->table_name) :
                                   nullptr;
        if (t && acc->has_entries_after(t, trg2bit(event), acc->fired_upto(t)))
          continue;
        if (!(t= trg_commit_entry(thd, table)))
          return true;
        acc= thd->transaction->trg_commit;
        if (acc->append(&thd->transaction->mem_root, t, event, true))
          return true;
        trg_hook_trace("accumulator: %s activated on %s.%s by a statement "
                       "that changed no %s row of it, at position %llu%s",
                       trg_event_type_names[trg2bit(event)].str,
                       table->s->db.str, table->s->table_name.str,
                       trg_event_type_names[trg2bit(event)].str,
                       acc->pos - 1,
                       thd->in_sub_stmt ? " (inside a sub-statement)" : "");
      }
    }
  }
  return false;
}


/*
  Open the subject table outside the current statement's table list, the
  way INFORMATION_SCHEMA does: back up the open tables state, which hides
  the session's temporary tables, use a temporary LEX, and open (not
  lock) one table.  The transactional
  MDL_SHARED_WRITE taken by the DML is still held, so the shared read
  request is satisfied by the existing ticket and acquires nothing new.
  The TABLE comes from the table cache with its triggers parsed, so the
  boundary needs no copy of the trigger bodies.
*/

class Subject_table_reopen
{
  THD *thd;
  LEX *old_lex= nullptr;
  LEX temp_lex;
  Open_tables_backup backup;
  TABLE_LIST tl;
  bool active= false;
public:
  explicit Subject_table_reopen(THD *thd_arg): thd(thd_arg) {}
  TABLE *open(const LEX_CSTRING &db, const LEX_CSTRING &name)
  {
    old_lex= thd->lex;
    thd->lex= &temp_lex;
    lex_start(thd);
    temp_lex.sql_command= SQLCOM_SELECT;
    thd->reset_n_backup_open_tables_state(&backup);
    active= true;
    tl.init_one_table(&db, &name, nullptr, TL_READ);
    TABLE_LIST *start= &tl;
    uint counter;
    if (open_tables(thd, &start, &counter, MYSQL_OPEN_IGNORE_FLUSH))
      return nullptr;
    return tl.table;
  }
  ~Subject_table_reopen()
  {
    if (!active)
      return;
    close_thread_tables(thd);
    thd->restore_backup_open_tables_state(&backup);
    lex_end(&temp_lex);
    thd->lex= old_lex;
  }
};


/*
  Remember what a SAVEPOINT boundary returns to if a body fails there.

  A body that fails at a commit boundary (COMMIT, implicit commit, end of
  an autocommit statement, XA END) rolls back the whole transaction, so
  the real code needs no rollback scope for the bodies there.  It does
  need the statement transaction of the bodies' last statement committed
  before trans_commit() reaches ha_commit_trans(thd, TRUE), which asserts
  that no statement transaction is pending.  At a SAVEPOINT, where the
  transaction stays active, this is where the bodies' writes get a scope
  they can be rolled back to without touching the rest of the
  transaction: a statement transaction around the firing, or an implicit
  savepoint.
*/

static void trg_commit_boundary_begin(Trg_commit_accumulator *acc)
{
  acc->boundary_pos= acc->pos;
  acc->boundary_bodies_ran= acc->bodies_ran;
  acc->failed_db= acc->failed_table= acc->failed_trigger= null_clex_str;
  for (Trg_commit_table *t= acc->tables; t; t= t->next)
    for (Trg_commit_watermark *w= t->watermarks; w; w= w->next)
      w->boundary_pos= w->pos;
}


/*
  A SAVEPOINT failed at or after its boundary: a body failed, or the
  savepoint could not be set after the bodies ran.  The SAVEPOINT fails
  like a DML statement whose ROW trigger fails: it sets no savepoint,
  and the transaction stays active.  Stub for the engine side: the real
  code rolls back to the scope opened at trg_commit_boundary_begin(),
  which undoes what every body of this boundary wrote, nested firings
  included.  The accumulator side is real: rows the bodies captured are
  dropped and every watermark returns to where it was, so retrying the
  SAVEPOINT fires every trigger again over the same rows.
*/

static void trg_commit_boundary_rollback(THD *thd,
                                         Trg_commit_accumulator *acc,
                                         const char *boundary,
                                         const char *cause)
{
  trg_hook_trace("%s: %s; the bodies' writes would be rolled back to the "
                 "boundary's scope, rows captured after position %llu "
                 "dropped, watermarks restored; the statement fails, the "
                 "transaction stays active",
                 boundary, cause, acc->boundary_pos);
  acc->truncate(acc->boundary_pos);
  acc->bodies_ran= acc->boundary_bodies_ran;
  for (Trg_commit_table *t= acc->tables; t; t= t->next)
    for (Trg_commit_watermark *w= t->watermarks; w; w= w->next)
      w->pos= w->boundary_pos;
}


/*
  A commit boundary failed: a body failed, or the triggers of a table
  could not fire, such as when re-opening it was killed.  The whole
  transaction is rolled back, as SQL:1999 16.6 GR 6b-c has it for a COMMIT
  whose triggered statements fail or that any other error prevents, by
  the caller of trg_commit_fire() once the error is set, an XA branch
  included, which is left ROLLBACK ONLY.  The statement that caused the
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
  class 40.  A body's error that says so already, a deadlock, stays the
  statement's error, so that clients and replicas recognize it and retry
  the transaction.  Any other cause stays before the new error as a
  further condition of the statement: a body's error before
  ER_TRG_BEFORE_COMMIT_ROLLBACK (40004, triggered action exception), any
  other failure, such as re-opening the subject table being killed,
  before ER_TRG_BEFORE_COMMIT_NOT_FIRED (40000).

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
  if (acc->failed_trigger.str && cause &&
      !strncmp(da->get_sqlstate(), "40", 2))
    return cause;
  thd->clear_error();
  if (acc->failed_trigger.str)
    my_error(ER_TRG_BEFORE_COMMIT_ROLLBACK, MYF(0), acc->failed_db.str,
             acc->failed_trigger.str);
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
  if (at_savepoint && !t->has_savepoint_triggers)
    return false;

  /*
    Skip a table whose rows were all truncated away.  Otherwise the
    table must be reached: a trigger that has no watermark yet has seen
    nothing, and which triggers exist is only known from the TABLE.
  */
  if (!acc->has_entries_after(t, trg_all_events, 0))
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
  Subject_table_reopen reopen(thd);
  TABLE *table= find_locked_table(thd->open_tables, t->db.str, t->name.str);
  ulonglong bodies_from= acc->pos;
  if (table)
  {
    /*
      Under LOCK TABLES the bodies run against the locked tables, which
      include the tables of BEFORE COMMIT bodies (see
      add_tables_and_routines_for_triggers()).  A statement on a table
      with triggers is prelocked, so under LOCK TABLES it runs as
      LTM_PRELOCKED_UNDER_LOCK_TABLES.  Otherwise this is the end of an
      autocommit statement, which prelocked the tables of the bodies with
      its own, and the bodies run against those.  Were the statement not
      prelocked, its own tables would still be open: the bodies would run
      with them parked, the way Subject_table_reopen parks them, and open
      their own.
    */
    const char *body_tables=
      thd->locked_tables_mode == LTM_LOCK_TABLES ||
      thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES ?
      "on the tables LOCK TABLES locked" :
      thd->locked_tables_mode == LTM_PRELOCKED ?
      "on the tables the statement prelocked" :
      "with the statement's open tables parked";
    trg_hook_trace("%s: %s.%s enters the firing chain at depth %u; it is "
                   "still open in thd->open_tables, transactional "
                   "MDL_SHARED_WRITE held: %s; bodies would run %s",
                   boundary, t->db.str, t->name.str, depth,
                   mdl_held ? "yes" : "NO", body_tables);
  }
  else
  {
    trg_hook_trace("%s: %s.%s enters the firing chain at depth %u; it is "
                   "closed, transactional MDL_SHARED_WRITE held: %s; "
                   "re-opening it to reach its triggers",
                   boundary, t->db.str, t->name.str, depth,
                   mdl_held ? "yes" : "NO");
    if (!(table= reopen.open(t->db, t->name)))
      error= true;
  }

  Table_triggers_list *trg_list= table ? table->triggers : nullptr;
  /*
    One list in firing order, whatever the triggers' events: they all
    fire at this one boundary.
  */
  for (Trigger *tr= trg_list ? trg_list->get_commit_triggers() : nullptr;
       tr && !error; tr= tr->next_commit)
  {
    if (at_savepoint && !tr->fires_on_savepoint)
      continue;

    Trg_commit_watermark *w;
    for (w= t->watermarks; w; w= w->next)
      if (tr->name.streq(w->trigger_name))
        break;
    ulonglong from= w ? w->pos : 0;
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
      acc->failed_db= t->db;
      acc->failed_table= t->name;
      acc->failed_trigger= safe_lexcstrdup_root(&thd->transaction->mem_root,
                                                tr->name);
      error= true;
      break;
    }

    if (!w)
    {
      if (!(w= new (&thd->transaction->mem_root) Trg_commit_watermark()))
      {
        error= true;
        break;
      }
      w->trigger_name= safe_lexcstrdup_root(&thd->transaction->mem_root,
                                            tr->name);
      w->boundary_pos= 0;
      w->next= t->watermarks;
      t->watermarks= w;
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


/*
  Fire the BEFORE COMMIT triggers that have rows or activations they have
  not seen yet, table by table in the order the transaction first changed
  the tables.

  @param boundary      what is happening, for the trace
  @param at_savepoint  only triggers declared OR SAVEPOINT fire
*/

static bool trg_commit_fire(THD *thd, const char *boundary, bool at_savepoint)
{
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  if (!acc)
    return false;

  trg_hook_trace("%s: boundary reached, accumulator holds %llu rows, "
                 "in_sub_stmt=%u, open tables in THD: %s",
                 boundary, acc->rows_from(0), thd->in_sub_stmt,
                 thd->open_tables ? "yes" : "none");

  trg_commit_boundary_begin(acc);

  for (Trg_commit_table *t= acc->tables; t; t= t->next)
    if (trg_commit_fire_table(thd, acc, t, boundary, at_savepoint))
    {
      const char *cause= acc->failed_trigger.str ? "a body failed" :
                         "the triggers could not fire";
      if (at_savepoint)
        trg_commit_boundary_rollback(thd, acc, boundary, cause);
      else
        trg_commit_boundary_abort(thd, boundary, cause);
      return true;
    }
  return false;
}


/*
  The end of the transaction, before anything that ends it.  The
  transaction is still active: trigger writes would join it.  On failure
  the error is set while the accumulator still names the failed trigger,
  and the caller rolls the transaction back.
*/

static bool trg_commit_fire_at_end(THD *thd, const char *kind)
{
  if (!thd->transaction->trg_commit)
    return false;
  char boundary[160];
  my_snprintf(boundary, sizeof(boundary), "%s (statement: %.60s)", kind,
              safe_str(thd->query()));
  if (!trg_commit_fire(thd, boundary, false))
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
  if (!trg_commit_fire_at_end(thd, kind))
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
  return trg_commit_fire_at_end(thd, "COMMIT of an autocommit statement");
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
  if (!trg_commit_fire(thd, "XA END", false))
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
  return trg_commit_fire(thd, "SAVEPOINT", true);
}


/*
  trans_savepoint() could not set the savepoint after
  trg_commit_fire_at_savepoint() succeeded: the boundary is undone as if
  a body had failed there.
*/

void trg_commit_savepoint_failed(THD *thd)
{
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  /* The conditions under which trg_commit_fire_at_savepoint() fired */
  if (acc && !thd->in_sub_stmt)
    trg_commit_boundary_rollback(thd, acc, "SAVEPOINT",
                                 "the savepoint could not be set");
}


void trg_commit_savepoint_set(THD *thd, SAVEPOINT *sv)
{
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  sv->trg_commit_pos= acc ? acc->pos : 0;
  if (acc)
    trg_hook_trace("SAVEPOINT %s: marker at position %llu",
                   sv->name, sv->trg_commit_pos);
}


void trg_commit_savepoint_rollback(THD *thd, SAVEPOINT *sv)
{
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  if (!acc)
    return;
  trg_hook_trace("ROLLBACK TO SAVEPOINT %s: %llu rows captured after the "
                 "savepoint dropped, watermarks clamped",
                 sv->name, acc->rows_from(sv->trg_commit_pos));
  acc->truncate(sv->trg_commit_pos);
}


void trg_commit_savepoint_release(THD *thd, SAVEPOINT *sv)
{
  if (thd->transaction->trg_commit)
    trg_hook_trace("RELEASE SAVEPOINT %s: marker at position %llu dropped, "
                   "accumulator unchanged", sv->name, sv->trg_commit_pos);
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


/*
  ha_commit_trans() of a statement inside a multi-statement transaction
  that has an accumulator
*/

void trg_commit_statement_commit(THD *thd)
{
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  if (acc->stmt_mark != acc->pos)
  {
    trg_hook_trace("statement committed inside the transaction: statement "
                   "mark moves from %llu to %llu", acc->stmt_mark, acc->pos);
    acc->stmt_mark= acc->pos;
  }
}


/* ha_rollback_trans() of a transaction that has an accumulator, or of a
   statement inside it */

void trg_commit_rollback(THD *thd, bool is_real_trans)
{
  Trg_commit_accumulator *acc= thd->transaction->trg_commit;
  if (is_real_trans)
    trg_hook_trace("ROLLBACK of the transaction: %llu captured rows "
                   "discarded; BEFORE COMMIT triggers had %s",
                   acc->rows_from(0), acc->bodies_ran ?
                   "ALREADY FIRED, their writes roll back with the "
                   "transaction" : "not fired");
  else if (acc->pos != acc->stmt_mark)
  {
    trg_hook_trace("statement rollback inside the transaction: %llu rows "
                   "captured by the failed statement dropped",
                   acc->rows_from(acc->stmt_mark));
    acc->truncate(acc->stmt_mark);
  }
}
