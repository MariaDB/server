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

Compared with the previous revision, the engine no longer sees `Event_log`,
`binlog_cache_data`, `enum_binlog_row_image`, the transactional-cache flag, the
row-logging function, the row-image column policy, or the buffers themselves.

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
format *or* `WSREP_EMULATE_BINLOG`; table eligibility *or*
`WSREP_EMULATE_BINLOG`; and `table->file->prepare_for_row_logging()`.

`fk_cascade_table_eligible()` — moved here from InnoDB — rejects a table that
has *no primary key* **and** a *virtual column participating in a key*: such a
row cannot be identified unambiguously from a full row image, so we decline to
report the cascade rather than hand a consumer something it would apply to the
wrong row.

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
| 2 | Triggers on the child table | not wired up | cannot run at the report point: firing a trigger runs a stored program that may re-enter the engine. Would have to be queued and run at statement end — and `BEFORE` triggers, which may modify or skip the row, cannot be honoured at all once the engine has already performed the cascade |
| 3 | CHECK constraints on the child table | not wired up | safe to evaluate inline: `TABLE::verify_constraints()` only evaluates expressions over `record[0]` and touches no other table |

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
- **Table eligibility.** Tables with no PK and a virtual column in a key are
  skipped (§6.3) — except under `WSREP_EMULATE_BINLOG`, where the check does
  not apply.
- **Only one consumer.** Triggers and CHECK constraints on cascaded child rows
  still do not fire; the service is shaped for them (§6.4) but nothing is
  wired up.
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
| Service implementation, consumer dispatch | `sql/sql_class.cc` | `fk_cascade_table_eligible()`, `fk_cascade_begin_full_row_image()`, `fk_cascade_end_full_row_image()`, the four `thd_fk_cascade_*()` |
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
