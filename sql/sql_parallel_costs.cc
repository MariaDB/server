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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA */

/**
  @file

  Running a query in the parallel worker threads: deciding whether the workers
  can run it (can_run_query_in_workers), giving each worker a private copy of
  the plan it needs -- its tables, its join tabs, its conditions and its
  result container -- running the join over a worker's chunk, and collecting
  the result rows on the manager.

  The workers themselves, the channel they ship rows through and their
  lifetime are in sql_parallel_workers.cc.
*/


#include "sql_parallel_workers.h"
#include "transaction.h"

/**
  @brief
    Cost the driving table's scan as one divided among parallel workers.

  @description
    Called by best_access_path() for the driving position (idx ==
    const_tables) once it has the serial cost of a full or range scan of
    'table'. The parallel reading of the same scan is priced as one worker's
    share of it plus the cost of setting the workers up:

      serial_cost / N + optimizer_parallel_setup_cost * (tables in the join)

    with N = parallel_worker_threads. The setup term is what a parallel
    execution pays before and after the scan -- creating N worker threads with
    a THD each, opening every non-const table once per worker, building and
    draining the result containers -- and is charged per table because the
    opening is the part of it that grows with the join. It is the term that
    keeps a scan too small to repay it serial.

    *divisor is by how many workers the rows that survive the table's own
    WHERE are spread: min(N, rows_out). The join-order search divides the
    read_time of every later position by it, because each worker joins only
    its share of the driving rows and the plan's cost is one worker's. It is
    capped by rows_out rather than fixed at N because a driving table that
    yields fewer rows than there are workers cannot spread them any further: a
    five-row table of which one row survives puts the whole join under it on
    one worker, whatever N is, and crediting it with 1/N of that join is what
    made the optimizer split exactly such tables.

    Eligibility is the table-level half of the runtime gate
    (table_can_be_parallel_scanned); the query-level half was asked by
    choose_plan() before it allowed any parallel candidate at all.

  @param join            the join being ordered
  @param table           the driving table
  @param serial_cost     best_access_path()'s cost of scanning it serially
  @param rows_out        rows that survive the table's own WHERE
  @param parallel_cost   [out] the cost of the parallel reading
  @param divisor         [out] what later positions' costs are divided by

  @return
    true   the parallel reading is cheaper: *parallel_cost and *divisor are set
    false  parallel scan unavailable or no cheaper; outputs untouched
*/

bool parallel_scan_cost(JOIN *join, TABLE *table, double serial_cost,
                        double rows_out, double *parallel_cost,
                        double *divisor)
{
  THD *thd= join->thd;
  const uint n= (uint) thd->variables.parallel_worker_threads;
  if (n < 2 ||                                   // disabled, or no speed-up
      !table_can_be_parallel_scanned(table))
    return false;

  const double setup= (thd->variables.optimizer_parallel_setup_cost *
                       (join->table_count - join->const_tables));
  const double cost= serial_cost / (double) n + setup;
  if (cost + COST_EPS >= serial_cost)
    return false;

  *parallel_cost= cost;
  *divisor= MY_MIN((double) n, MY_MAX(rows_out, 1.0));
  return true;
}


/**
  @brief
    Whether a table's format and engine permit a parallel worker scan.

  @description
    Table-level eligibility shared by the optimizer cost hook
    (scale_cost_for_parallel_scan) and the runtime gate, which reaches it
    through tab_can_be_parallel_scanned():

      - a real base table (not an internal/temporary table);
      - no blob-backed columns (BLOB/TEXT/GEOMETRY/JSON) -- their payload lives
        off the record buffer and is not reproduced by the by-value row
        transport;
      - not fulltext-searched -- a MATCH ... AGAINST relevance is derived from
        handler state, not a stored column;
      - not partitioned;
      - the engine advertises some form of parallel scan.

    What kind it advertises is not decided here: parallel_scan_support()
    returns a bitmap (PSCAN_TABLE_FULL, PSCAN_TABLE_RANGE, PSCAN_INDEX_FULL,
    PSCAN_INDEX_RANGE) and matching a bit against the access method the plan
    actually chose is is_parallel_scan_applicable()'s job. This only asks
    whether the engine does it at all.

    Caller-specific conditions (parallel_worker_threads, the access method being
    a full scan, the join position) are checked by each caller, not here.
*/

bool table_can_be_parallel_scanned(TABLE *table)
{
  return table->s->tmp_table == NO_TMP_TABLE &&
         table->s->blob_fields == 0 &&
         !table->fulltext_searched &&
#ifdef WITH_PARTITION_STORAGE_ENGINE
         !table->part_info &&
#endif
         table->file->parallel_scan_support() != 0;
}

