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
#include "sql_statistics.h"
#include "optimizer_defaults.h"
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
    keeps a scan too small to repay it serial -- but whether it is repaid is
    not decided here. The join-order search offers the driving position both
    readings of every candidate (add_parallel_driver_candidates) and keeps
    the cheaper complete plan: a parallel reading dearer than the serial one
    on the driving table alone, when the setup costs more than the scan it
    divides, is still the cheaper plan by far once the join under it is
    divided among the workers as well.

    *divisor is by how many workers the rows that survive the table's own
    WHERE are spread. The join-order search divides the read_time of every
    later position by it, because each worker joins only its share of the
    driving rows and the plan's cost is one worker's. It is not simply N,
    for two reasons. The engine hands the workers pieces of the table, and
    a table it cannot cut into N pieces -- a single-page B-tree, or a range
    reading a page or two of a larger one -- reaches fewer workers than
    that, however many there are: W = min(N, parallel_scan_pieces() scaled
    by the share of the table the scan reads). Nor do the rows that
    survive the WHERE reach more workers than the pieces they lie in, and
    where they lie is not known: the rows of date_dim for one year are a
    few pages next to each other in the primary key, and so are the rows
    of any table whose filtered column follows its key, which a dimension
    table's dates and a fact table's dates and keys commonly do. Taken as
    lying together, R surviving rows of a T-row table of P pieces fill
    ceil(R * P / T) of them, and W is capped by that as well. For rows
    spread evenly through the table that is pessimistic, and the cap only
    bites when the survivors are fewer than N pieces' worth; for rows that
    do lie together it is what the workers will find, and a plan chosen
    on a division the workers cannot make costs more than a division not
    made. And a driving table that yields fewer rows than W cannot spread
    them any further, while the join under it runs at the pace of the
    worker with the most of them: with R rows over W workers the busiest
    holds ceil(R/W), so the divisor is R / ceil(R/W). A five-row table of
    which one row survives puts the whole join under it on one worker,
    whatever N is, and five rows over four workers go 2-1-1-1, a division
    by 2.5 and not by 4. Crediting such a table with 1/N of the join under
    it is what made the optimizer split exactly such tables. The scan's
    own cost is divided by N regardless: a scan the engine cannot cut into
    pieces is at most a couple of pages, whose cost is nothing next to the
    setup, and what has to be honest is the join under it, which is where
    the cost is.

    Eligibility is the table-level half of the runtime gate
    (table_can_be_parallel_scanned); the query-level half was asked by
    choose_plan() before it allowed any parallel candidate at all.

  @param join            the join being ordered
  @param table           the driving table
  @param serial_cost     best_access_path()'s cost of scanning it serially
  @param rows_scanned    rows the scan reads: the table, or the range's share
  @param rows_out        rows that survive the table's own WHERE
  @param parallel_cost   [out] the cost of the parallel reading
  @param divisor         [out] what later positions' costs are divided by

  @return
    true   the parallel reading is priced: *parallel_cost and *divisor are set
    false  parallel scan unavailable; outputs untouched
*/

bool parallel_scan_cost(JOIN *join, TABLE *table, double serial_cost,
                        double rows_scanned, double rows_out,
                        double *parallel_cost, double *divisor)
{
  THD *thd= join->thd;
  const uint n= (uint) thd->variables.parallel_worker_threads;
  if (n < 2 ||                                   // disabled, or no speed-up
      !table_can_be_parallel_scanned(table))
    return false;

  const double setup= (thd->variables.optimizer_parallel_setup_cost *
                       (join->table_count - join->const_tables));
  *parallel_cost= serial_cost / (double) n + setup;

  /*
    The workers the scan's rows can reach: the pieces the engine can cut
    the scan into, or the range's share of them, and no more pieces than
    the surviving rows fill if they lie together; but no more than there
    are workers and no fewer than one.
  */
  const double rows= MY_MAX(rows_out, 1.0);
  const double table_pieces= (double) table->file->parallel_scan_pieces();
  const double table_rows= rows2double(table->file->stats.records);
  double scan_pieces= table_pieces, out_pieces= table_pieces;
  if (table_rows > 0.0)
  {
    if (rows_scanned < table_rows)
      scan_pieces*= rows_scanned / table_rows;
    if (rows < table_rows)
      out_pieces= ceil(table_pieces * rows / table_rows);
  }
  const double workers= MY_MAX(MY_MIN((double) n,
                                      MY_MIN(floor(scan_pieces), out_pieces)),
                               1.0);
  *divisor= rows / ceil(rows / workers);
  return true;
}


/**
  @brief
    How many rows the workers would ship for this query, in the terms
    parallel_ship_cost() charges them: set JOIN::parallel_groups.

  @description
    A worker ships what reaches the end of its nested loop. For a query that
    does not aggregate that is every joined row, and the search knows the
    count at the end of each plan it costs. For one that aggregates over
    everything it reads (no GROUP BY) it is one partial per worker, which
    costs nothing worth charging. For a grouped query it is one partial per
    group per worker, and the number of groups is estimated here, once, as
    the product over the GROUP BY columns of each column's distinct values
    -- the table's rows over the column's average frequency, from the
    engine-independent statistics -- and capped by the joined rows at each
    plan. Without statistics get_column_avg_frequency() reports one value
    for the whole column, and the estimate is one group: the optimistic
    side, which leaves such a query costed as it was before this term, rather
    than charging it every joined row and keeping a five-group query serial.

    Asked pre-plan, so the GROUP BY is the query's own
    (pwt_query_shape_allows_workers() takes it the same way), whose elements
    that gate has checked are plain columns.
*/

void parallel_estimate_shipped_groups(JOIN *join)
{
  SELECT_LEX *sl= join->select_lex;
  join->parallel_groups= -1.0;
  if (!join->group_list && !join->group &&
      !sl->agg_func_used() && !sl->with_sum_func)
    return;                                     // ships every joined row
  ORDER *group= join->group_list ? join->group_list : sl->group_list.first;
  double groups= 0.0;
  for (ORDER *k= group; k; k= k->next)
  {
    Item *item= (*k->item)->real_item();
    if (item->type() != Item::FIELD_ITEM)
      continue;
    Field *field= ((Item_field *) item)->field;
    double rows= (double) field->table->stat_records();
    double freq= get_column_avg_frequency(field);
    double distinct= freq > 0.0 ? MY_MAX(rows / freq, 1.0) : 1.0;
    groups= groups == 0.0 ? distinct : COST_MULT(groups, distinct);
  }
  join->parallel_groups= groups;
}


/**
  @brief
    The cost of shipping a plan's result from the workers to the manager.

  @description
    The manager receives every row the workers ship, by itself, and that
    is not divided by anything: a plan whose output is a large share of
    what it reads -- a cross join, a selective join of two wide tables
    shipped whole -- has the workers hand the manager as many rows as the
    join produced, and the join-order search adds the cost of that to a
    parallel plan when the plan is complete and its row count known. What
    is shipped is JOIN::parallel_groups' answer
    (parallel_estimate_shipped_groups()): the joined rows, or a partial per
    group per worker, the workers that got rows being what the driving
    position's parallel_divisor says, and never more than the joined rows.

    A row costs optimizer_parallel_row_cost while the worker's container
    is in memory. The container is a temporary table that create_tmp_table()
    starts as a HEAP table of at most min(tmp_memory_table_size,
    max_heap_table_size) bytes and converts to Aria on disk when that is
    exceeded, after which every row written and read back goes through the
    page cache, so the rows past what the workers' containers hold in
    memory cost DISK_TEMPTABLE_LOOKUP_COST more each. The width of a shipped
    row is the plan's tables' used field lengths added together -- the
    read_set columns, which are what the layout ships
    (pwt_row_layout::build()) -- at their full declared length, which is
    how a HEAP table stores a VARCHAR, and not capped at the engine's mean
    row length as the join buffer's estimate is. The busiest worker holds
    rows / parallel_divisor of the rows, so the containers hold
    parallel_divisor times one container's worth in memory between them.

    optimizer_parallel_row_cost set to 0 switches the charge off, the disk
    tier included: a test that wants a plan divided whatever it ships sets
    it to 0 beside optimizer_parallel_setup_cost.

  @param join          the join being ordered; positions[const_tables] is
                       the driving position and every table is placed
  @param joined_rows   rows the complete plan produces
  @param rows_shipped  [out] what the workers ship, for the trace
  @param rows_spilled  [out] how many of them from containers gone to disk

  @return  the cost to add to the plan
*/

double parallel_ship_cost(JOIN *join, double joined_rows,
                          double *rows_shipped, double *rows_spilled)
{
  THD *thd= join->thd;
  double workers=
    MY_MAX(join->positions[join->const_tables].parallel_divisor, 1.0);
  double rows= joined_rows;
  if (join->parallel_groups >= 0.0)
    rows= MY_MIN(joined_rows, COST_MULT(join->parallel_groups, ceil(workers)));
  *rows_shipped= rows;
  *rows_spilled= 0.0;
  if (thd->variables.optimizer_parallel_row_cost == 0.0)
    return 0.0;                                 // the charge is switched off

  double width= 0.0;
  for (uint i= join->const_tables; i < join->table_count; i++)
    if (JOIN_TAB *tab= join->positions[i].table)
      width+= (double) tab->get_max_used_fieldlength();
  const double heap_bytes= (double) MY_MIN(thd->variables.tmp_memory_table_size,
                                           thd->variables.max_heap_table_size);
  const double in_memory= width > 0.0 ? workers * heap_bytes / width : rows;
  const double spilled= MY_MAX(rows - in_memory, 0.0);

  *rows_spilled= spilled;
  return COST_ADD(COST_MULT(rows, thd->variables.optimizer_parallel_row_cost),
                  COST_MULT(spilled, DISK_TEMPTABLE_LOOKUP_COST(thd)));
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

