# MDEV-38243 — Binlog row events for cascading foreign key operations

## Design document

Status: implemented on branch `MDEV-38243-new_API`, current at commit
`7e4a9d269c4` (third round of review changes: the SE ↔ server boundary is now
the generic FK-cascade service `include/mysql/service_thd_fk_cascade.h`).

---

## 1. Problem statement

Foreign key cascade actions — `ON DELETE CASCADE`, `ON UPDATE CASCADE`,
`ON DELETE SET NULL`, `ON UPDATE SET NULL` — are executed **inside InnoDB**
(`row_ins_foreign_check_on_constraint()`), below the SQL layer. In row-based
replication the SQL layer only logs the row changes it drives directly, i.e.
the change to the *parent* table named in the statement. The cascaded changes
to *child* tables are performed by InnoDB and never surface to the binlogging
layer.

Currently replication handles changes done by the foreign key constraint cascade
execution so that the replication **SQL slave re-executes the cascade**:
it applies the parent-table row event with foreign key checks enabled, and its
own InnoDB reproduces the child-row changes.

Re-executing the cascade on the replica is a source of problems:

- **Non-determinism / divergence** when the replica's schema, indexes or FK
  definitions differ, or when `SET NULL` ordering is ambiguous.
- **Parallel-apply hazards**, particularly for Galera appliers, where a cascade
  fired during apply interacts unpredictably with other concurrent appliers.
  Conflicts in galera applying is the primary reason for the feature.

## 2. Prior Art

MySQL 9.6 has refactored Foreign key constraint handling to happen in server side,
which directly allows recording binlog events for cascading operation changes.
See: https://blogs.oracle.com/mysql/no-more-hidden-changes-how-mysql-9-6-transforms-foreign-key-management

If similar feature is planned to be implemented in MariaDB, then this MDEV-38243
becomes obsolete.

## 3. Goal

Have the **origin** capture the row changes produced by cascading FK operations
and write them into the binary log as **explicit row events**, so that the
**replica applies just those events and does not re-run the cascade**. This
makes applying deterministic and free of cascade-induced parallel-apply hazards.

## 4. Feature switch

- New session variable **`rpl_use_binlog_events_for_fk_cascade`**, default
  `OFF`. It cannot be changed inside a transaction or sub-statement
  (`error_if_in_trans_or_substatement`); the check is skipped for
  `SET GLOBAL`.
- **The session variable is required in all cases, Galera included.** Under
  `WSREP_EMULATE_BINLOG` (Galera with the binary log off) two of the *other*
  preconditions become moot and are skipped — the row-format test, because a
  writeset is always row-based, and the table-eligibility test of §6.3, because
  a writeset is not subject to that row-image caveat — but emulation alone does
  **not** turn the feature on. Treating `WSREP_EMULATE_BINLOG` as sufficient
  would enable cascade capture for every Galera session and break cascade
  replication; `thd_fk_cascade_wanted()` carries a comment saying so.
- Row-based binary logging is otherwise required; the capture is a no-op
  in statement format.

## 5. Architecture / data flow

```
  Statement executes on origin
        │
        ▼
  InnoDB parent DML ── cascade ──► row_ins_foreign_check_on_constraint()
        │                            │
        │                            ├─ locate the child's open MySQL TABLE
        │                            ├─ thd_fk_cascade_wanted(thd, child)? ──── no ──► plain cascade
        │                            ├─ position cascade cursor on the row
        │                            ├─ thd_fk_cascade_capture(BEFORE) ─┐
        │                            ├─ row_update_cascade_for_mysql()  │  server materialises
        │                            ├─ position cursor on changed row  │  the images, via
        │                            ├─ thd_fk_cascade_capture(AFTER) ──┘  handler::fk_cascade_fetch_row()
        │                            └─ thd_fk_cascade_row(DELETE|UPDATE)
        │                                        │
        │                                        ▼   (server side, sql/sql_class.cc)
        │                               dispatch to consumers; consumer 1 is
        │                               binlogging: THD::binlog_report_cascade_row()
        ▼                               queues the images on the THD
  parent row event logged (flagged)
        │
        ▼
  statement end / commit  ──►  THD::flush_pending_cascade_binlog()
                                     │  emits queued child events in order
                                     ▼
                               binary log:  [parent event][derived child events...]
                                            all flagged, applied verbatim on replica
```

The engine/server boundary sits at the *report* step: the engine says only
"a cascade is about to happen / has happened on this child row"; every decision
about what the row image contains, whether it is binlogged, when and how, is
made on the server side. See §6.

## 6. Origin side — capture

### 6.1 The SE ↔ server interface

`include/mysql/service_thd_fk_cascade.h` is a **generic FK-cascade service**,
not a binlog service. Its premise: an engine that performs FK cascades
internally cannot know what the server wants done about them — the row may
need to be binlogged, may need to fire triggers, may need CHECK constraints
re-evaluated. Rather than teach the engine each of those, the engine reports
the action and the server decides.

```c
#define FK_CASCADE_IMAGE_BEFORE 0
#define FK_CASCADE_IMAGE_AFTER  1

#define FK_CASCADE_ACTION_DELETE   0
#define FK_CASCADE_ACTION_UPDATE   1
#define FK_CASCADE_ACTION_SET_NULL 2

int  thd_fk_cascade_wanted (MYSQL_THD thd, struct TABLE *table);
int  thd_fk_cascade_capture(MYSQL_THD thd, struct TABLE *table, int which);
void thd_fk_cascade_row    (MYSQL_THD thd, struct TABLE *table, int action);
void thd_fk_cascade_abort  (MYSQL_THD thd);
```

The engine's side of the contract is those four calls **and no row buffers**:

```c
  if (thd_fk_cascade_wanted(thd, child_table))
  {
    <position the cascade cursor on the row about to change>
    thd_fk_cascade_capture(thd, child_table, FK_CASCADE_IMAGE_BEFORE);

    <perform the cascade>

    <position the cascade cursor on the changed row>
    thd_fk_cascade_capture(thd, child_table, FK_CASCADE_IMAGE_AFTER);

    thd_fk_cascade_row(thd, child_table, FK_CASCADE_ACTION_UPDATE);
  }
```

The server owns the row images end to end: it decides which columns make up
the image (the engine does not touch the `TABLE`'s column bitmaps), it owns the
buffers they are materialised into, and it frees them. Capture is *driven* from
the engine — only the engine knows when the row is about to change and when it
has changed — but *performed* by the server.

The engine's one remaining contribution is a new handler method:

```c
  /* sql/handler.h */
  virtual int handler::fk_cascade_fetch_row(uchar *buf)
  { return HA_ERR_WRONG_COMMAND; }
```

which converts the record the engine's cascade cursor is sitting on into MySQL
row format, honouring the bitmaps the server has already set. Engines that do
not cascade internally never see the call.

### 6.2 Engine side — `storage/innobase/row/row0ins.cc`

In `row_ins_foreign_check_on_constraint()`:

1. **Locate the child table.** `row_ins_find_open_table_for_cascade()` parses
   the child's db/table name and calls `find_fk_open_table()`, which returns the
   child `TABLE` only if it was opened via FK **prelocking**
   (`TABLE_LIST::PRELOCK_FK`). Only the engine can map its own `dict_table_t` to
   a name, which is why this much stays here. See §11 for the consequence on the
   replica.

2. **Ask the server.** `thd_fk_cascade_wanted(trx->mysql_thd, child_mysql_table)`.
   Nothing further happens if it says no.

3. **Capture the before-image.** In a short `mtr` of its own, the cascade cursor
   is restored (`cascade->pcur->restore_position(BTR_SEARCH_LEAF, mtr)`, requiring
   `SAME_ALL`), the record and clustered index are handed to
   `ha_innobase::fk_cascade_set_cursor(rec, index)`, and
   `thd_fk_cascade_capture(..., FK_CASCADE_IMAGE_BEFORE)` is called. The cursor
   is cleared again immediately (`fk_cascade_set_cursor(NULL, NULL)`): it is
   only valid while the caller holds the page latch. Success is tracked in
   `need_cascade_binlog`.

4. **Run the actual cascade** via `row_update_cascade_for_mysql()`.
   `can_cascade_binlog = (err == DB_SUCCESS && need_cascade_binlog)`.

5. **Capture the after-image** (for `UPDATE` / `SET NULL`; not for
   `PLAIN_DELETE`) the same way, from the just-modified record — provided it is
   still a user record and not delete-marked. Result in `have_after_image`.

6. **Report or abort.** If a before-image was captured
   (`need_cascade_binlog`), exactly one of the two happens:
   `thd_fk_cascade_row(..., FK_CASCADE_ACTION_DELETE | FK_CASCADE_ACTION_UPDATE)`
   when the cascade succeeded and the required images are present, otherwise
   `thd_fk_cascade_abort()`. Either call frees the server-side images, so no
   capture can leak because the cascade failed midway.

`ha_innobase::fk_cascade_fetch_row()` (in `ha_innodb.cc`) does the conversion:
it rebuilds the InnoDB row template for the server's bitmaps
(`rebuild_template_for_cascade_binlog_row_image()`), computes `rec_offsets`,
temporarily points `prebuilt->index` at the cascade index, calls
`row_sel_store_mysql_rec()`, and resets the template
(`reset_template_for_cascade_binlog_row_image()`) — the template in force
belongs to the statement being executed and describes a different set of
columns. It returns `HA_ERR_GENERIC` if the record cannot be converted, and the
cursor state (`m_fk_cascade_rec`, `m_fk_cascade_index`) is per-handler and
valid only for the duration of one capture call.

### 6.3 Server side — `sql/sql_class.cc`

`thd_fk_cascade_wanted()` answers for **every consumer at once**, so the engine
can skip the capture cost when nothing is interested. It is cheap enough to call
per cascaded row. Its binlog arm requires, in order: the session variable; row
format *or* `WSREP_EMULATE_BINLOG`; eligibility *or* `WSREP_EMULATE_BINLOG`;
and `table->file->prepare_for_row_logging()`.

`fk_cascade_table_eligible()` — moved here from InnoDB — rejects a table that
has *no primary key* **and** a *virtual column participating in a key*: such a
row cannot be identified unambiguously from a full row image, so we decline to
report the cascade rather than hand a consumer something it would apply to the
wrong row.

Eligibility is then decided **for the statement, not for the table**, by
`fk_cascade_stmt_capturable()`. The reason is that the two halves of the
mechanism have different scopes: eligibility is a property of one table, but
the flag that stops the replica re-running the cascade is a property of the
whole statement — the first captured child row calls
`binlog_mark_fk_cascade_events()`, and from then on *every* row event of the
statement carries `FK_CASCADE_EVENTS_F | NO_FOREIGN_KEY_CHECKS_F` (§8), so the
replica stops cascading for all children, not just the captured one.

A per-child answer therefore lets an eligible child switch the replica's
cascade off on behalf of an ineligible sibling whose rows were never logged:
the replica neither receives those rows nor recreates them, and silently keeps
the orphans. Pinned by `rpl_fk_cascade_binlog_row_mixed_eligibility` (§14),
which diverges without this check.

The decision is made all-or-nothing over the **FK prelocking set**, which is
what makes it decidable before any row is touched:
`prepare_fk_prelocking_list()` (`sql_base.cc:5390`) has already enumerated,
transitively, every child table the statement can write through a cascade, and
opened each one — children needed only for read-only FK checks are `OPEN_STUB`
(`table.h:2603`) and never reach `thd->open_tables`, so a scan of that list for
`PRELOCK_FK` entries with `TL_FIRST_WRITE` is exactly the reachable set. If any
of them is ineligible the feature is declined for the whole statement and the
classic replica-side cascade handles all of it — slower, but correct. The
answer is cached on the `THD` and keyed on `query_id`, so it cannot outlive the
statement it was computed for and needs no reset hook.

`thd_fk_cascade_capture()` allocates the image slot
(`THD::fk_cascade_before_image` / `fk_cascade_after_image`, `reclength` bytes),
switches the table to the full row image via
`fk_cascade_begin_full_row_image()` — `tmp_set` set to all columns minus the
virtual ones, installed as read/write set, and as `rpl_write_set` if that was
NULL — calls `handler::fk_cascade_fetch_row()`, and restores the statement's own
bitmaps with `fk_cascade_end_full_row_image()`. This bitmap policy is *server*
policy, which is why it lives here and not in the engine. On failure the slot is
freed and non-zero returned.

`thd_fk_cascade_row()` consumes the images and dispatches. At most one cascade
action is in flight per THD at a time, which is what lets the images live as two
plain `THD` members: the engine captures before, cascades, captures after, and
reports, all within one call to its cascade routine. An update whose
after-image could not be captured is dropped rather than reported. The images
are freed on the way out (`THD::fk_cascade_free_images()`), and `~THD()` frees
them as a backstop for an in-flight capture the engine never reported.

### 6.4 Consumers

The point of routing cascades through the server is that each known gap in how
FK cascades behave becomes an arm of `thd_fk_cascade_wanted()` plus an arm of
`thd_fk_cascade_row()`, instead of new machinery inside every engine that
cascades. Three consumers are identified; **only the first is implemented**, the
other two are documented in place in `sql_class.cc` as commented-out arms:

| # | Consumer | State | Note |
|---|----------|-------|------|
| 1 | Binary logging of the cascaded rows | **implemented** | deferred: queued now, written at statement end (§7) |
| 2 | Triggers on the child table | not wired up | `AFTER` triggers would have to be queued and run at statement end; `BEFORE` triggers cannot be supported at this call site at all — see §6.4.1 |
| 3 | CHECK constraints on the child table | not wired up | safe to evaluate inline: `TABLE::verify_constraints()` only evaluates expressions over `record[0]` and touches no other table |

#### 6.4.1 Why `BEFORE` triggers cannot be honoured here

Worth recording, because it is not a matter of the service growing another
call. Three blockers, the third of which is the decisive one.

**No pre-cascade callback carries the new row.** The only calls that happen
before the child row changes are `thd_fk_cascade_wanted()`
(`row0ins.cc:1408`) and `thd_fk_cascade_capture(FK_CASCADE_IMAGE_BEFORE)`
(`:1416`). The first is a yes/no gate; the second materialises the *old* row.
Neither is given the proposed new values. The only call that carries the
action, `thd_fk_cascade_row()` (`:1469`), runs after
`row_update_cascade_for_mysql()` (`:1424`) has returned — the row is already
rewritten, or for a cascade delete, gone.

**Nothing can carry a trigger's result back.** `thd_fk_cascade_row()` returns
`void`; `thd_fk_cascade_capture()` returns `int`, but the engine uses it only
to set `need_cascade_binlog` and never to abort or redirect the cascade. All
three effects of a `BEFORE` trigger are therefore unrepresentable: rewriting
`NEW` has no return channel, `SIGNAL`-ing an error has no abort path, and
skipping the row has neither. The last is not hypothetical —
`Table_triggers_list::process_triggers()` asserts that a `TRG_ACTION_BEFORE`
invocation *must* supply a `skip_row_indicator` out-param and that a
`TRG_ACTION_AFTER` one must not (`sql/sql_trigger.cc:2799`), so the skip
channel is structurally part of running a `BEFORE` trigger.

**A wider API would not help: the call site cannot run a stored program at
all.** Both captures execute inside `mtr_start()`/`mtr_commit()` holding a
`BTR_SEARCH_LEAF` page latch on the child's clustered index (`:1411`-`:1421`,
`:1427`-`:1457`). Beyond that, the whole of
`row_ins_foreign_check_on_constraint()` runs under a shared `dict_sys.latch`:
`row_ins_check_foreign_constraint()`, which calls it at `:1867`, documents
that requirement at `:1576`. A trigger body is arbitrary SQL that opens tables
and re-enters the engine, so it cannot run under either latch, anywhere in
this function — including the report point at `:1469`, which is free of page
latches (which is why binlogging is safe there) but still under the dictionary
latch and still nested inside the parent statement's `ha_update_row()`.

That third point is also why deferring is not a workaround for `BEFORE`
specifically: running it at statement end, as consumer 2 would have to for
`AFTER`, is exactly what a `BEFORE` trigger cannot tolerate.

Note the near-miss. The proposed new values *do* exist before the cascade
runs — `row_ins_cascade_calc_update_vec()` builds `cascade->update` at
`row0ins.cc:1314`, 110 lines ahead of the cascade itself — so it is the
latching context, not the data flow, that blocks this. Supporting `BEFORE`
triggers on cascaded rows means hoisting cascade execution above the engine,
i.e. the MySQL 9.6 route in §2, not another service call. §6.4.2 records the
main counter-proposal to that conclusion and why it does not change it.

#### 6.4.2 Considered and rejected: unwind-and-retry

The obvious counter-proposal, recorded here because a reviewer will arrive at
it independently. Sketch: let `thd_fk_cascade_wanted()` return not just yes/no
but "a `BEFORE` trigger must run first"; on seeing that, InnoDB unwinds out of
the cascade back to the point of entry, releasing all latches; the server runs
the trigger with no latches held; the cascade then restarts from the
beginning.

Two things genuinely favour it.

*The latch problem does dissolve.* Unwinding to the engine-entry boundary is
the right answer to §6.4.1's third blocker, and the shape is not foreign to
InnoDB: `DB_LOCK_WAIT` is already handled by `row_mysql_handle_errors()` with
a `trx_savept_t` partial rollback and a re-run of the query graph.

*Trigger prelocking is already in place.* This would normally be fatal — a
trigger body needs its own tables and routines locked, and acquiring locks
mid-statement invites deadlock — but it is already handled.
`prepare_fk_prelocking_list()` adds every *modified* FK child with
`TL_FIRST_WRITE`; only read-only children are reduced to `OPEN_STUB`
(`table.h:2603`). So the child gets its own `handle_table()` pass
(`sql_base.cc:4160`), which calls `add_tables_and_routines_for_triggers()`,
and — since that pass runs `prepare_fk_prelocking_list()` again on the child —
the walk is transitive through grandchildren. The child's triggers are loaded
and everything their bodies touch is already in the prelocking set.

This has been verified by instrumenting `handle_table()`: for a parent with a
cascade child, the child appears with `prelocking_placeholder = PRELOCK_FK`, a
write lock, and a loaded `TABLE::triggers`, and a grandchild reachable only
through the child appears the same way. There is no SQL-observable way to
assert it, so no test pins it.

See §6.4.3 for the event-mapping defect this exposed.

Four things sink it.

**Retries are per child row, not per statement.**
`row_ins_foreign_check_on_constraint()` is called from the scan loop in
`row_ins_check_foreign_constraint()` (`row0ins.cc:1867`), once per matching
child record. N cascaded rows therefore means N unwind/replay cycles, each
redoing the parent row operation and every child cascaded before it, and
recursing into grandchildren — O(N²). Lock-wait retry tolerates this because
it is exceptional; a `BEFORE` trigger on a cascade child is the common case as
soon as it is supported at all.

**The replay has an ordering paradox.** The trigger must run *after* the
rollback, or the rollback discards its writes; the replay then re-enters the
cascade and asks again. That requires memoising "already fired for this child
row" against a row identity that survives a rollback and replay. The failure
modes are an infinite loop or a trigger that fires twice.

**The trigger's output still has nowhere to go.** A `BEFORE UPDATE` trigger
exists to rewrite `NEW`, but after the replay InnoDB rebuilds
`cascade->update` from the parent's new values (`row0ins.cc:1314`), discarding
it. The scheme would additionally need a channel making InnoDB derive the
update vector from a server-supplied image, an abort path for `SIGNAL`, and a
skip-this-child-but-continue mode for `skip_row_indicator`. Building the first
of those yields most of what is needed to drive the cascade from the server
anyway, at which point the retry machinery is redundant.

**The trigger would observe the wrong state.** It runs after the unwind, so a
body reading the parent table sees the pre-statement state rather than what a
`BEFORE` trigger sees in a server-driven cascade.

#### 6.4.3 Child trigger events: what the cascade does, not what the statement does

`prepare_fk_prelocking_list()` used to propagate the *parent's*
`trg_event_map` to the child verbatim. That is right for `ON DELETE CASCADE`,
where a parent delete becomes a child delete, and for every `ON UPDATE`
action, which updates the child. It is wrong for **`ON DELETE SET NULL`** (and
`SET DEFAULT`): the child row is *updated*, not deleted, so the child was
prelocked carrying `TRG_EVENT_DELETE`. Its `UPDATE` triggers' tables and
routines were therefore never added to the prelocking set, and firing one
would have tripped the `DBUG_ASSERT` in
`Table_triggers_list::process_triggers()` (`sql_trigger.cc:2829`), which
requires the event bit to be present in `pos_in_table_list->trg_event_map`.

The child's map is now derived from the cascade action:

| parent event | FK action | child event |
|---|---|---|
| `DELETE` | `ON DELETE CASCADE` | `TRG_EVENT_DELETE` |
| `DELETE` | `ON DELETE SET NULL` / `SET DEFAULT` | `TRG_EVENT_UPDATE` |
| `UPDATE` | `ON UPDATE CASCADE` / `SET NULL` / `SET DEFAULT` | `TRG_EVENT_UPDATE` |
| either | `RESTRICT` / `NO ACTION` | none (read-only child, `TL_READ`) |

A zero map is exactly the read-only case, so it also decides the lock type,
replacing the previous separate `fk_modifies_child()` test.

One related gap closed with it: a child reachable through *several*
constraints undergoes the union of their actions — `ON DELETE CASCADE` on one
column and `ON DELETE SET NULL` on another both apply, deleting some rows and
updating others. `table_already_fk_prelocked()` skipped the second constraint,
leaving the first one's events to stand for both; it now returns the existing
entry so the events can be OR-ed into it.

**No user-visible effect today**, because triggers still do not fire on
cascaded rows — this is groundwork for consumer 2, and a latent assertion
failure removed. What it does change is that a `SET NULL` child's `UPDATE`
triggers now contribute their tables and routines to the prelocking set, so
such statements take a slightly wider set of table locks than before.

##### Salvageable: decide before entering the engine

The detection half of the proposal stands on its own, without any unwinding.
Because modified FK children are prelocked *with their triggers loaded*, the
server can answer "does any reachable cascade child carry a `BEFORE` trigger?"
at statement start, before InnoDB is entered — by walking the prelocking list
for `PRELOCK_FK` entries with `TL_FIRST_WRITE` and a non-NULL
`TABLE::triggers`. No sentinel return, no replay, no latch exposure. That is
enough to refuse the statement or divert it to a server-driven path, instead
of today's behaviour of silently not firing the trigger.

The same observation is already load-bearing elsewhere: `fk_cascade_stmt_capturable()`
(§6.3) uses exactly this walk of the FK prelocking set to decide binlog
eligibility for the whole statement up front. A trigger consumer would reuse
the scan and only change the predicate.

### 6.5 Deferred, ordered logging

Binlog events are **queued, not emitted inline**. The queue preserves *execution
order*. Emitting deletes inline would place every cascade delete ahead of every
deferred update within a statement, reordering events that touch the same row
and potentially making the replica apply an update to an already-deleted row.

## 7. Lifecycle — flush and discard

The queue is `THD::pending_cascade_binlog_row_events`, a
`Dynamic_array<THD::Cascade_binlog_row_event>` holding the child `TABLE`, the
server's own copies of the before/after images, and an `is_delete` flag. The
`THD` owns it outright; no engine structure participates in its lifetime.
`THD::binlog_report_cascade_row()` copies the images into it and calls
`binlog_mark_fk_cascade_events()`, so that the originating statement's own
already-pending row event gets flagged too (§8).

**Flush** (`THD::flush_pending_cascade_binlog()`): emits each queued event via
`handler::binlog_log_row()` with the table's bitmaps temporarily set to
`table->s->all_set`, choosing `Delete_rows_log_event` vs.
`Update_rows_log_event` from `is_delete`, and frees the record copies as it
goes. Temporary tables and entries whose `TABLE`/handler is gone are skipped
(their buffers freed). It is driven from two choke points:

- `binlog_flush_pending_rows_event()` in `sql/log.cc` at **statement end**
  (`stmt_end`), guarded by `thd->binlog_fk_cascade_events` and the feature
  test; the pending event is re-read afterwards, since flushing the cascade
  rows may have created or replaced it, before `STMT_END_F` is stamped;
- `ha_commit_trans()` in `sql/handler.cc` at **commit**, when `all ||
  thd->in_active_multi_stmt_transaction()`.

**Discard** (`THD::discard_pending_cascade_binlog()`): frees the queued record
copies and empties the array without writing anything. It runs on:

- `ha_rollback_trans()` — full/statement rollback (`sql/handler.cc`);
- `ha_rollback_to_savepoint()` — rollback to savepoint (events already flushed
  into the binlog cache by earlier statements are handled separately, by the
  binlog savepoint machinery truncating the cache);
- `~THD()` — backstop only, so that an undrained queue cannot leak.

Discard is **unconditional** with respect to thread type — it must run for
applier threads too, since it only frees memory. Both helpers
(`flush_pending_cascade_binlog_for_thd()`,
`discard_pending_cascade_binlog_for_thd()`) are called under the feature test
`rpl_use_binlog_events_for_fk_cascade || WSREP_EMULATE_BINLOG(thd)`.

### 7.1 Queue lifetime vs. `TABLE` lifetime

A queued entry stores a raw `TABLE *`. Because the queue outlives the engine
transaction (it is reclaimed by the server, not by `trx_t::free()`), the
invariant that matters is that the queue is **drained before the statement
closes its tables**. It holds: in `mysql_execute_command()`'s `finish:` block,
`trans_commit_stmt()` / `trans_rollback_stmt()` run *before*
`close_thread_tables_for_query()`; and for a multi-statement transaction the
`in_active_multi_stmt_transaction()` arm of the `ha_commit_trans()` gate drains
per statement, likewise with tables still open.

This is asserted rather than assumed — `mysql_execute_command()` carries a
`DBUG_ASSERT` immediately after `close_thread_tables_for_query()` that the
queue is empty at the top level (a substatement may legitimately leave rows
queued for the enclosing statement, since the draining commit/rollback is
itself under `! thd->in_sub_stmt`). The assert exists because the
`!table || !table->file` guard inside the flush loop can only catch a *closed*
table, not one that was freed and had its memory reused; a leftover entry would
otherwise be flushed by a later statement against a dangling pointer, silently.

## 8. Event marking

Three flags on `Rows_log_event` participate (`sql/log_event.h`):

| Flag | Bit | Set on | Meaning |
|------|-----|--------|---------|
| `NO_FOREIGN_KEY_CHECKS_F` | 1 | every cascade-logged event | pre-existing flag; disables FK checks on apply |
| `FK_CASCADE_EVENTS_F` | 4 | every cascade-logged event (root + derived) | this statement's cascade rows were logged; suppress re-cascade |
| `FK_CASCADE_DERIVED_F` | 5 | derived (child) events only | distinguishes cascade-derived rows from the originating rows |

Origin-side mechanism:

- `THD::binlog_fk_cascade_events` (bool) is set by
  `THD::binlog_mark_fk_cascade_events()` on the first cascade of a statement; it
  also stamps the currently-pending row events. It is reset in
  `reset_binlog_for_next_statement()`.
- `Event_log::prepare_pending_rows_event()` stamps every newly created event
  with `FK_CASCADE_EVENTS_F | NO_FOREIGN_KEY_CHECKS_F` while that THD flag is on.
- `THD::binlog_fk_cascade_derived` (bool) is turned on only around the flush
  loop (`binlog_begin_fk_cascade_derived()` / `binlog_end_fk_cascade_derived()`
  in `THD::flush_pending_cascade_binlog()`), so events created during
  the flush additionally get `FK_CASCADE_DERIVED_F`. The originating (parent)
  event is created *outside* the flush loop and therefore stays underived.

Result: the **root** event carries `{EVENTS_F, NO_FK_CHECKS_F}`; each **derived**
event carries `{EVENTS_F, DERIVED_F, NO_FK_CHECKS_F}`.

## 9. Replica side — apply

`Rows_log_event::do_apply_event()` (`sql/log_event_server.cc`): if the event
carries `NO_FOREIGN_KEY_CHECKS_F` **or** `FK_CASCADE_EVENTS_F`, it sets
`OPTION_NO_FOREIGN_KEY_CHECKS`, which InnoDB maps to `trx->check_foreigns =
false` (`ha_innodb.cc`). With FK checks off, `row_ins_check_foreign_constraint()`
returns early and `row_ins_foreign_check_on_constraint()` is never reached — the
replica does **not** re-run the cascade. It applies the explicit parent and
child row events directly, in log order.

No slave-thread guards (`!thd->rgi_slave` / `!thd_is_slave`) are used on the
capture or flush paths. They were removed as redundant: the flag-driven
suppression above already prevents an applier from re-cascading, so capture is
unreachable there for cascade-logged transactions.

## 10. Backward compatibility (older replica, strict mode)

An older MariaDB replica does not understand `FK_CASCADE_EVENTS_F`/
`FK_CASCADE_DERIVED_F` and would ignore them. It **does** understand the ancient
`NO_FOREIGN_KEY_CHECKS_F`. Because the origin sets that flag on every
cascade-logged event, an old replica:

1. applies the parent event with FK checks disabled → does **not** re-cascade;
2. applies the derived child events as the sole source of child changes.

No collision occurs and the data is correct **even under strict
`slave_exec_mode`**. Had `NO_FOREIGN_KEY_CHECKS_F` not been set, an old replica
would re-cascade *and* apply the derived events, colliding on the same keys →
`HA_ERR_KEY_NOT_FOUND` and a stopped replica in strict mode (tolerated only in
idempotent mode).

A newer replica that wants to re-execute the cascade instead of applying the
derived events can still recognise these events via the new flags and override
`NO_FOREIGN_KEY_CHECKS_F` (see §12).

### 10.1 Old-replica emulation for testing

Standard mtr runs every server from one build, so a genuine "old binary as
replica" test cannot run in the default suite. To still exercise the mechanism
in-tree, a debug-only injection point emulates a pre-MDEV-38243 replica:

- `Rows_log_event::do_apply_event()` computes a local `fk_cascade_events`
  (initialised from `get_flags(FK_CASCADE_EVENTS_F)`), and
  `DBUG_EXECUTE_IF("rpl_emulate_old_slave_fk_cascade", fk_cascade_events= false)`
  forces it off. The FK-check decision then depends **only** on
  `NO_FOREIGN_KEY_CHECKS_F` — exactly how an old server behaves.

The test `rpl_fk_cascade_binlog_row_old_slave` enables this keyword on a
strict-mode replica and confirms that a feature-ON origin's `CASCADE` and
`SET NULL` transactions apply with `Last_SQL_Errno=0` and correct data. Because
the emulated replica ignores `FK_CASCADE_EVENTS_F`, a passing run proves the
`NO_FOREIGN_KEY_CHECKS_F` stamp alone suffices; equally, if that stamp were ever
dropped, this test would fail (the emulated replica would re-cascade and
collide).

## 11. Known limitations and edge cases

- **Applier capture is a no-op (by design/limitation).** The capture requires
  the child table to be open via FK prelocking (`PRELOCK_FK`). A row-based
  applier opens only the tables named in the events, not prelocked FK children,
  so `find_fk_open_table()` returns NULL on an applier. Therefore, with the
  feature enabled **only on a replica** (origin OFF), the replica re-cascades
  the classic way and its own binary log contains only parent events — the
  option is effectively inert on the applier path. This is pinned by
  `rpl_fk_cascade_binlog_row_slave_option`.
- **Row format only.** Statement-based logging is unaffected.
- **Eligibility is all-or-nothing per statement.** A single ineligible child
  — no PK plus a virtual column in a key — disables capture for every child of
  that statement, not just itself (§6.3); the statement then falls back to the
  classic replica-side cascade. Conservative in two ways: the FK prelocking set
  covers children the statement *could* cascade into, not only those it
  actually does, and the check does not apply at all under
  `WSREP_EMULATE_BINLOG`.
- **Only one consumer.** Triggers and CHECK constraints on cascaded child rows
  still do not fire; the service is shaped for them (§6.4) but nothing is
  wired up. `AFTER` triggers and CHECK constraints are reachable from here;
  `BEFORE` triggers are not reachable from this call site at any API width
  (§6.4.1).
- **Only InnoDB implements the engine side.** `handler::fk_cascade_fetch_row()`
  defaults to `HA_ERR_WRONG_COMMAND`, so any other engine that starts calling
  the service must override it.
- **Observability.** `mysqlbinlog` prints only `STMT_END_F` in its verbose
  header; the FK cascade flags are not surfaced there. They are observable only
  through apply behaviour (or a future print extension).

## 12. Future work — "applier decides"

The `FK_CASCADE_DERIVED_F` marking is groundwork for an optional mode letting an
applier choose between applying the derived events (default) and re-executing
the cascade. Sketch:

- Add a replica variable, e.g. `slave_fk_cascade_mode = {APPLY_EVENTS |
  EXECUTE_CASCADE}`.
- `APPLY_EVENTS` (default): current behaviour.
- `EXECUTE_CASCADE`: for events carrying `FK_CASCADE_EVENTS_F`, do **not** honour
  `NO_FOREIGN_KEY_CHECKS_F` on the *root* events (those without
  `FK_CASCADE_DERIVED_F`), so InnoDB re-cascades; and **skip** the events
  carrying `FK_CASCADE_DERIVED_F`.

This reintroduces cascade non-determinism deliberately, so it is a
compatibility/fallback knob, not a routine mode.

## 13. Code map

| Area | File(s) | Key symbols |
|------|---------|-------------|
| Feature var | `sql/sys_vars.cc`, `sql/sql_class.h` | `rpl_use_binlog_events_for_fk_cascade`, `rpl_use_binlog_events_for_fk_cascade_check()` |
| SE ↔ server service | `include/mysql/service_thd_fk_cascade.h` | `thd_fk_cascade_wanted()`, `thd_fk_cascade_capture()`, `thd_fk_cascade_row()`, `thd_fk_cascade_abort()`, `FK_CASCADE_IMAGE_*`, `FK_CASCADE_ACTION_*` |
| Service implementation, consumer dispatch | `sql/sql_class.cc` | `fk_cascade_table_eligible()`, `fk_cascade_stmt_capturable()`, `fk_cascade_begin_full_row_image()`, `fk_cascade_end_full_row_image()`, the four `thd_fk_cascade_*()` |
| Statement-level eligibility cache | `sql/sql_class.h` | `THD::fk_cascade_stmt_query_id`, `THD::fk_cascade_stmt_ok` |
| FK prelocking set | `sql/sql_base.{h,cc}` | `prepare_fk_prelocking_list()` (child `trg_event_map` derived from the cascade action, §6.4.3), `table_already_fk_prelocked()` (returns the existing entry so events can be merged) |
| In-flight images | `sql/sql_class.h`, `sql/sql_class.cc` | `THD::fk_cascade_before_image`, `fk_cascade_after_image`, `fk_cascade_free_images()` |
| Handler hook | `sql/handler.h` | `handler::fk_cascade_fetch_row()` (default `HA_ERR_WRONG_COMMAND`) |
| THD queue, flush / discard, marking | `sql/sql_class.{h,cc}` | `Cascade_binlog_row_event`, `pending_cascade_binlog_row_events`, `binlog_report_cascade_row()`, `flush_pending_cascade_binlog()`, `discard_pending_cascade_binlog()`, `binlog_fk_cascade_events`, `binlog_fk_cascade_derived`, `binlog_mark_fk_cascade_events()`, `binlog_begin/end_fk_cascade_derived()` |
| Engine capture driver | `storage/innobase/row/row0ins.cc` | `row_ins_foreign_check_on_constraint()`, `row_ins_find_open_table_for_cascade()` |
| Engine record conversion | `storage/innobase/handler/ha_innodb.{h,cc}` | `ha_innobase::fk_cascade_set_cursor()`, `ha_innobase::fk_cascade_fetch_row()`, `m_fk_cascade_rec`, `m_fk_cascade_index`, `rebuild_/reset_template_for_cascade_binlog_row_image()` |
| Drain points | `sql/handler.{h,cc}`, `sql/log.cc`, `sql/sql_parse.cc` | `flush_pending_cascade_binlog_for_thd()` / `discard_pending_cascade_binlog_for_thd()` called from `ha_commit_trans`/`ha_rollback_trans`/`ha_rollback_to_savepoint`; `binlog_flush_pending_rows_event()`; drained-queue `DBUG_ASSERT` in `mysql_execute_command()` |
| Event flags / apply | `sql/log_event.h`, `sql/log_event_server.cc`, `sql/log.cc` | `FK_CASCADE_EVENTS_F`, `FK_CASCADE_DERIVED_F`, `do_apply_event()`, `prepare_pending_rows_event()`, `binlog_flush_pending_rows_event()` |
| wsrep | `sql/service_wsrep.cc`, `include/mysql/service_wsrep.h` | `wsrep_emulate_binlog()`, `WSREP_EMULATE_BINLOG()` |
| Test aid | `sql/log_event_server.cc` (`do_apply_event()`) | `DBUG_EXECUTE_IF("rpl_emulate_old_slave_fk_cascade", …)` — emulate a pre-MDEV-38243 replica (see §10.1) |

## 14. Tests (`mysql-test/suite/rpl/`)

- **`rpl_fk_cascade_binlog_row`** — feature OFF vs ON; asserts derived child
  row events are absent (OFF) / present (ON) in the origin binlog and replica
  data is correct.
- **`rpl_fk_cascade_binlog_row_ordering`** — event ordering of interleaved
  cascade delete/update.
- **`rpl_fk_cascade_binlog_row_rollback`** — queued events discarded on
  rollback / rollback-to-savepoint; nothing spurious is logged.
- **`rpl_fk_set_null_binlog_row`** — `SET NULL` cascade capture.
- **`rpl_fk_cascade_binlog_row_slave_option`** — origin OFF / replica ON:
  replication stays correct and the replica's binlog contains only parent
  events (pins the applier no-op of §11).
- **`rpl_fk_cascade_binlog_row_mixed_eligibility`** — a parent with one
  eligible and one ineligible child: asserts that no derived events are
  written and the replica keeps no orphan (pins the statement-level decision
  of §6.3).
- **`rpl_fk_cascade_binlog_row_old_slave`** — cross-version compatibility
  (§10): a strict-mode replica emulating an older MariaDB (via the
  `rpl_emulate_old_slave_fk_cascade` debug keyword) applies a feature-ON
  origin's `CASCADE` / `SET NULL` transactions with no error and correct data.
  Debug build only.

**Not yet covered:**

- Cross-version replication against a *real* older `mariadbd` binary. The apply
  mechanism itself is covered in-tree by `rpl_fk_cascade_binlog_row_old_slave`
  (§10.1).
- The Galera path (`WSREP_EMULATE_BINLOG`), which §4 singles out as the primary
  motivation. The `suite/galera` FK tests (`galera_fk_cascade_delete`,
  `galera_fk_cascade_delete_debug`, `galera_fk_cascade_update`) never set
  `rpl_use_binlog_events_for_fk_cascade`, so they exercise the classic
  re-cascade path only; no test in any suite covers cascade capture under
  writeset emulation.

All six tests were written against the pre-refactoring interfaces (§6.1 changed
the SE ↔ server boundary twice since) but pass unchanged on the current tree.
