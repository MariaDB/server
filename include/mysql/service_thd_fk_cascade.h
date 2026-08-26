/* Copyright (c) 2026, MariaDB Corporation.

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

#pragma once

/*
  Generic server-side handling of foreign-key cascade actions performed
  inside a storage engine.

  A storage engine that performs FK cascades internally (InnoDB does, in
  row_ins_foreign_check_on_constraint()) cannot know what the server wants
  done about them: the row may need to be written to the binary log, may need
  to fire triggers, may need CHECK constraints re-evaluated, and so on. Rather
  than teach the engine each of those, the engine reports the action and the
  server decides.

  The engine's side of the contract is three calls and no row buffers:

    if (thd_fk_cascade_wanted(thd, child_table))
    {
      <position the cascade cursor on the row about to change>
      thd_fk_cascade_capture(thd, child_table, FK_CASCADE_IMAGE_BEFORE);

      <perform the cascade>

      <position the cascade cursor on the changed row>
      thd_fk_cascade_capture(thd, child_table, FK_CASCADE_IMAGE_AFTER);

      thd_fk_cascade_row(thd, child_table, FK_CASCADE_ACTION_UPDATE);
    }

  The server owns the row images end to end: it decides which columns make up
  the image (the engine does not touch the TABLE's column bitmaps), it owns the
  buffers they are materialised into, and it frees them. The engine's only
  contribution is handler::fk_cascade_fetch_row(), which converts the record
  the cascade cursor is sitting on into MySQL row format.

  Capture is therefore driven from the engine (only the engine knows when the
  row is about to change and when it has changed) but performed by the server.
*/

#ifdef __cplusplus
extern "C" {
#endif

struct TABLE;

/* Which image thd_fk_cascade_capture() should materialise. */
#define FK_CASCADE_IMAGE_BEFORE 0
#define FK_CASCADE_IMAGE_AFTER  1

/* The action the engine performed on the child row. */
#define FK_CASCADE_ACTION_DELETE   0
#define FK_CASCADE_ACTION_UPDATE   1
#define FK_CASCADE_ACTION_SET_NULL 2

/*
  Does the server want to be told about cascade actions on this child table?

  Answers for every consumer at once (binary logging, triggers, CHECK
  constraints, ...), so the engine can skip the cost of capturing row images
  when nothing is interested. Also screens out tables whose row image would be
  unsafe, and sessions in which the feature does not apply. Cheap enough to
  call per cascaded row.
*/
int thd_fk_cascade_wanted(MYSQL_THD thd, struct TABLE *table);

/*
  Materialise one row image of the child row the cascade cursor is currently
  positioned on. `which` is FK_CASCADE_IMAGE_BEFORE or FK_CASCADE_IMAGE_AFTER.

  The caller must have positioned the engine's cascade cursor on the row (for
  InnoDB, via ha_innobase::fk_cascade_set_cursor()) and must hold whatever
  latch the engine needs to read it. Returns 0 on success; non-zero means the
  image could not be produced, in which case the engine should abandon
  reporting this row via thd_fk_cascade_abort().
*/
int thd_fk_cascade_capture(MYSQL_THD thd, struct TABLE *table, int which);

/*
  Report the completed cascade action, consuming the images captured above,
  and dispatch it to whichever server-side consumers apply. Frees the images.
*/
void thd_fk_cascade_row(MYSQL_THD thd, struct TABLE *table, int action);

/* Abandon a partially captured cascade action, freeing any images. */
void thd_fk_cascade_abort(MYSQL_THD thd);

#ifdef __cplusplus
}
#endif
