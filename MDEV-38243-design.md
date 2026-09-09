# MDEV-38243 — Binlog row events for cascading foreign key operations

## Design document

Status: implemented on branch `MDEV-38243`

---

## 1. Problem statement

Foreign key cascade actions — `ON DELETE CASCADE`, `ON UPDATE CASCADE`,
`ON DELETE SET NULL`, `ON UPDATE SET NULL` — are executed **inside InnoDB**
(`row_ins_foreign_check_on_constraint()`), below the SQL layer. In row-based
replication the SQL layer only logs the row changes it drives directly, i.e.
the change to the *parent* table named in the statement. The cascaded changes
to *child* tables are performed by InnoDB and never surface to the binlogging
layer.

Currently replication handles changes done by the foreing key constraint cascade
execution so that the replication **SQL slave re-executes the cascade**:
it applies the parent-table row event with foreign key checks enabled, and its
own InnoDB reproduces the child-row changes.

Re-executing the cascade on the replica is a source of problems:

- **Non-determinism / divergence** when the replica's schema, indexes or FK
  definitions differ, or when `SET NULL` ordering is ambiguous.
- **Parallel-apply hazards**, particularly for Galera appliers, where a cascade
  fired during apply interacts unpredictably with other concurrent appliers.
  Conflicts in galera applying is the primary reason the feature.

## 2. Prior Art
MySQL 9.6 has refactored Foreign key constraint handling to happen in server side,
which directly allows recording binlog events for cascading operation changes.
See: https://blogs.oracle.com/mysql/no-more-hidden-changes-how-mysql-9-6-transforms-foreign-key-management

If similar feature is planned to be implemented in Mariadb, then this MDEV-28243
becomes obsolete.

## 3. Goal

Have the **origin** capture the row changes produced by cascading FK operations
and write them into the binary log as **explicit row events**, so that the
**replica applies just those events and does not re-run the cascade**. This
makes applying deterministic and free of cascade-induced parallel-apply hazards.

## 4. Feature switch

- New session variable **`rpl_use_binlog_events_for_fk_cascade`**, default
  `OFF`. It cannot be changed inside a transaction or sub-statement
  (`error_if_in_trans_or_substatement`).
- The same capture path is taken automatically under Galera when binloggiing is
  off, code paths with: **`WSREP_EMULATE_BINLOG`** .
- Row-based binary logging is required; the capture is a no-op otherwise.

## 5. Architecture / data flow

```
  Statement executes on origin
        │
        ▼
  InnoDB parent DML  ── triggers ──►  row_ins_foreign_check_on_constraint()
        │                                   │  (feature ON, ROW format)
        │                                   ▼
        │                          capture BEFORE image of child row
        │                          run the real cascade (row_update_cascade_for_mysql)
        │                          capture AFTER image (UPDATE / SET NULL only)
        │                          report it: thd_binlog_cascade_{delete,update}_row()
        │                                   │
        │                                   ▼   (server side, sql/sql_class.cc)
        │                          THD::binlog_report_cascade_row() copies the
        │                          record images and queues them on the THD
        ▼
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
"this child row changed, here are its before/after images"; every decision
about whether, when and how to binlog it is made on the server side. See §6.1.

## 6. Origin side — capture

Location: `storage/innobase/row/row0ins.cc`,
`row_ins_foreign_check_on_constraint()`.

When the feature is engaged (`thd_rpl_use_binlog_events_for_fk_cascade()`), and
the cascade child table is available as an open MySQL `TABLE`:

1. **Locate the child table.** `row_ins_find_open_table_for_cascade_binlog()`
   parses the child's db/table name and calls `find_fk_open_table()`, which
   returns the child `TABLE` only if it was opened via FK **prelocking**
   (`TABLE_LIST::PRELOCK_FK`). See §10 for the consequence on the replica.

2. **Eligibility guard.** `row_ins_allow_fk_cascade_binlog_for_table()` rejects
   a table that has *no primary key* **and** a *virtual column participating in
   a key* (an unsafe row image); otherwise it is allowed.

3. **Capture the before-image.** The handler's column read/write bitmaps and
   `rpl_write_set` are temporarily switched to an all-columns set (virtual
   columns cleared), the InnoDB row template is rebuilt for a full row image
   (`ha_innobase::rebuild_template_for_cascade_binlog_row_image()` →
   `reset_template()` + `build_template(true)`), and the current child record is
   materialised into a MySQL-format buffer with `row_sel_store_mysql_rec()`.
   Bitmaps/template are then restored.

4. **Run the actual cascade** via `row_update_cascade_for_mysql()`.

5. **Capture the after-image** (for `UPDATE` / `SET NULL`; not for
   `PLAIN_DELETE`) by re-positioning the cursor to the just-modified record and
   materialising it the same way.

6. **Report the change to the server.** The engine calls
   `thd_binlog_cascade_delete_row(thd, table, before_rec)` or
   `thd_binlog_cascade_update_row(thd, table, before_rec, after_rec)` and is
   then done with the row — the server copies the images, so the temp-heap
   buffers may be reused or freed immediately afterwards.

### 6.1 The SE ↔ server interface

`include/mysql/service_thd_binlog.h` exposes exactly two cascade entry points:

```c
void thd_binlog_cascade_delete_row(MYSQL_THD thd, struct TABLE *table,
                                   const unsigned char *before_record);
void thd_binlog_cascade_update_row(MYSQL_THD thd, struct TABLE *table,
                                   const unsigned char *before_record,
                                   const unsigned char *after_record);
```

The engine passes only the child `TABLE` and the affected row image(s) in
MySQL record format. It does **not** see `Event_log`, `binlog_cache_data`,
`enum_binlog_row_image`, the transactional-cache flag, or the row-logging
function — all of which the earlier revision of this design required it to
supply. Consequently the *policy* — whether the change is binlogged at all,
which log function applies, event ordering, event flagging, and the
commit/rollback lifecycle — lives entirely in `THD`
(`THD::binlog_report_cascade_row()` and the flush/discard pair below).

This keeps the contract engine-agnostic: any engine that performs FK cascades
internally can adopt it by calling these two functions, without replicating
binlog internals or tracking a queue of its own.

### Deferred, ordered logging

Events are **queued, not emitted inline**. The queue preserves *execution
order*. Emitting deletes inline would place every cascade delete ahead of every
deferred update within a statement, reordering events that touch the same row
and potentially making the replica apply an update to an already-deleted row.

## 7. Lifecycle — flush and discard

The queue itself is `THD::pending_cascade_binlog_row_events`, a
`Dynamic_array<THD::Cascade_binlog_row_event>` holding the child `TABLE`, the
server's own copies of the before/after images, and an `is_delete` flag. The
`THD` owns it outright; no engine structure participates in its lifetime.

**Flush** (`THD::flush_pending_cascade_binlog()`): emits each queued event via
`handler::binlog_log_row()` using an all-columns bitmap, choosing
`Delete_rows_log_event` vs. `Update_rows_log_event` from `is_delete`, and frees
the record copies as it goes. It is driven from two choke points:

- `binlog_flush_pending_rows_event()` in `sql/log.cc` at **statement end**;
- `ha_commit_trans()` in `sql/handler.cc` at **commit**.

Temporary tables and tables without a usable `TABLE`/handler are skipped (their
buffers freed).

**Discard** (`THD::discard_pending_cascade_binlog()`): frees the queued record
copies and empties the array without writing anything. It runs on:

- `ha_rollback_trans()` — full/statement rollback (`sql/handler.cc`);
- `ha_rollback_to_savepoint()` — rollback to savepoint;
- `~THD()` — backstop only, so that an undrained queue cannot leak.

Discard is **unconditional** with respect to thread type — it must run for
applier threads too, since it only frees memory.

### 7.1 Queue lifetime vs. `TABLE` lifetime

A queued entry stores a raw `TABLE *`. Because the queue now outlives the
engine transaction (it is reclaimed by the server, not by `trx_t::free()`),
the invariant that matters is that the queue is **drained before the statement
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
`NO_FOREIGN_KEY_CHECKS_F` (see §11).

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
  skipped (§5.2).
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
| Feature var | `sql/sys_vars.cc`, `sql/sql_class.h` | `rpl_use_binlog_events_for_fk_cascade` |
| SE ↔ server API | `include/mysql/service_thd_binlog.h` | `thd_binlog_cascade_delete_row()`, `thd_binlog_cascade_update_row()`, `thd_rpl_use_binlog_events_for_fk_cascade()` |
| THD state, queue, flush / discard | `sql/sql_class.{h,cc}` | `Cascade_binlog_row_event`, `pending_cascade_binlog_row_events`, `binlog_report_cascade_row()`, `flush_pending_cascade_binlog()`, `discard_pending_cascade_binlog()`, `binlog_fk_cascade_events`, `binlog_fk_cascade_derived`, `binlog_mark_fk_cascade_events()` |
| Capture / report | `storage/innobase/row/row0ins.cc` | `row_ins_foreign_check_on_constraint()`, `row_ins_find_open_table_for_cascade_binlog()`, `row_ins_allow_fk_cascade_binlog_for_table()` |
| Row-image template | `storage/innobase/handler/ha_innodb.{h,cc}` | `rebuild_template_for_cascade_binlog_row_image()` |
| Drain points | `sql/handler.{h,cc}`, `sql/log.cc`, `sql/sql_parse.cc` | calls in `ha_commit_trans`/`ha_rollback_trans`/`ha_rollback_to_savepoint`, `binlog_flush_pending_rows_event()`, drained-queue `DBUG_ASSERT` in `mysql_execute_command()` |
| Event flags / apply | `sql/log_event.h`, `sql/log_event_server.cc`, `sql/log.cc` | `FK_CASCADE_EVENTS_F`, `FK_CASCADE_DERIVED_F`, `do_apply_event()`, `prepare_pending_rows_event()`, `binlog_flush_pending_rows_event()` |
| wsrep | `sql/service_wsrep.cc`, `include/mysql/service_wsrep.h` | `wsrep_emulate_binlog()` |
| Test aid | `sql/log_event_server.cc` (`do_apply_event()`) | `DBUG_EXECUTE_IF("rpl_emulate_old_slave_fk_cascade", …)` — emulate a pre-MDEV-38243 replica (see §9.1) |

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
  events (pins the applier no-op of §10).
- **`rpl_fk_cascade_binlog_row_old_slave`** — cross-version compatibility
  (§9): a strict-mode replica emulating an older MariaDB (via the
  `rpl_emulate_old_slave_fk_cascade` debug keyword) applies a feature-ON
  origin's `CASCADE` / `SET NULL` transactions with no error and correct data.
  Debug build only.

**Not yet covered:** cross-version replication against a *real* older `mariadbd`
binary. The apply mechanism itself is covered in-tree by `rpl_fk_cascade_binlog_row_old_slave` (§10.1).
