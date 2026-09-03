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
    Discount a full-table-scan cost when the table is eligible to be scanned by
    parallel workers.

  @description
    Disabled for now, not only because it changes QEPs in our mtr suite, but
    because it only discounts costs associated with the split table, not tables
    further down the join in the workers.

    When parallel query is enabled the first non-const table can be scanned by
    N worker threads, each reading a disjoint partition concurrently while the
    manager runs the rest of the join. The wall-clock cost of reading and
    copying the rows is therefore roughly 1/N of a serial scan, so the row
    (full-scan) components of 'cost' -- I/O, CPU and row-copy -- are scaled by
    1/N. The index components are left untouched: this only ever discounts a
    full table scan.

    Eligibility mirrors the table-level half of the runtime gate exactly
    (engine support, no blob-backed columns, not fulltext-searched, a real base
    table, not partitioned), so the optimizer never discounts a scan that will
    not actually run in parallel. The caller is responsible for invoking this
    only for the driving table (idx == const_tables), the single position a
    parallel scan applies to, and 'cost' must be the caller's local copy, not
    the cached per-table estimate.

  @return
    true   the cost was scaled (table is parallel-scan eligible)
    false  no change (parallel scan disabled or table not eligible)
*/

bool scale_cost_for_parallel_scan(THD *thd, TABLE *table, ALL_READ_COST *cost)
{
#if 0
  const uint n= thd->variables.parallel_worker_threads;
  if (n < 2 ||                                   // disabled, or no speed-up
      !table_can_be_parallel_scanned(table))
    return false;

  const double factor= 1.0 / (double) n;
  cost->row_cost.io  *= factor;
  cost->row_cost.cpu *= factor;
  cost->copy_cost    *= factor;
#endif
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

