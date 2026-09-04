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


#include "mariadb.h"
#include "mysqld_error.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_select.h"
#include "opt_trace.h"
#include "sql_parallel_workers.h"
#include "transaction.h"


/*****************************************************************************
  The gate: which queries the workers may run.
  All of this runs on the user's thread at optimize time; a query that fails
  any of it simply runs serially.
*****************************************************************************/


/**
  @brief
    Whether this item can be copied into something a worker can own.

  @description
    Clonable, and the copy sharing no node at all with the original.

    Asking about nodes rather than about Item_field leaves matters twice over.
    Several classes implement deep_copy() as a shallow copy while still holding
    child items -- Item_outer_ref and Item_copy_string among them -- so their
    copy keeps pointing at the original's children. The worker repoints the
    Item_field leaves of its copy at its own tables, so a shared leaf moves the
    manager's own item onto a worker's table, and the next worker moves it
    again, which is what the in_use assertion in Field::val_int() catches. And a
    shared node that is not a leaf carries evaluation state, so several workers
    evaluating one object at once tear it. item_clone_shares_nodes() answers the
    general question, and so covers the shallow-copying classes not yet met
    rather than the ones now known.

    Half of pwt_item_is_worker_safe(), which the gate asks at optimize time,
    where a query that fails either test still falls back to serial execution
    for free.
*/
static bool pwt_item_is_clonable(THD *thd, Item *item)
{
  Item *clone= item->deep_copy_with_checks(thd);
  return clone && !item_clone_shares_nodes(item, clone);
}


/*
  Looks for an Item_field the rebinder will have no table to rebind to.
*/
class Pwt_foreign_field_finder : public Field_enumerator
{
  JOIN *join;
public:
  bool found;
  Pwt_foreign_field_finder(JOIN *join_arg) : join(join_arg), found(false) {}
  void visit_field(Item_field *item) override
  {
    if (found || !item->field)
      return;
    for (uint j= join->const_tables; j < join->table_count; j++)
      if (item->field->table == join->join_tab[j].table)
        return;
    found= true;
  }
};


/**
  @brief
    Whether 'item' reads a field of a table the worker will not have a copy of.

  @description
    A worker owns a copy of every non-const table of the join and the rebinder
    repoints the Item_field leaves of its expressions at those copies. A leaf
    naming any other table is left pointing at the manager's own table, and a
    worker that evaluates it reads a record another thread owns, which is what
    the in_use assertion in Field::val_int() catches.

    Two shapes reach here. A correlated reference to an enclosing query's table:
    "t1.f1 IN (SELECT f1 FROM t4)" run as a subquery leaves the workers scanning
    t4 under "t4.f1 = <outer ref to t1.f1>", and t1 belongs to the query above.
    And a const table, read once during optimization, which is no more the
    worker's to read for being read-only. Neither is caught by the clonability
    test: the copy is a proper deep copy, there is simply nothing to rebind its
    leaf to.
*/
static bool pwt_item_reads_a_foreign_table(JOIN *join, Item *item)
{
  Pwt_foreign_field_finder finder(join);
  item->walk(&Item::enumerate_field_refs_processor, (void*) &finder, 0);
  return finder.found;
}


/* Both of the above: an item the gate may hand to a worker. */
static bool pwt_item_is_worker_safe(JOIN *join, Item *item)
{
  return pwt_item_is_clonable(join->thd, item) &&
         !pwt_item_reads_a_foreign_table(join, item);
}


/**
  @brief
    Can the workers be handed disjoint pieces of the way this table is read?

  @description
    Four things have to hold of the driving table, and each of them can be
    taken away by a plan decision made after the gate has run, so there is one
    place that says what they are and both askers come here:

    - the access is a full scan or a range scan (JT_ALL / JT_RANGE). Those are
      the two the engine partitions: the whole clustered index, or the key
      intervals of the range;

    - the table is read by the ordinary record reader, which is the one a
      worker replaces with the pscan chunk reader. An ordered index read
      (join_read_first -- from a covering key, or from test_if_skip_sort_order()
      giving the table the GROUP BY or ORDER BY order for free) delivers rows in
      key order, and rows handed out in chunks do not arrive in key order;

    - the quick select, if there is one, is one the partitioner can map onto
      chunk boundaries (is_parallel_scan_applicable);

    - the table itself is one the workers can read and ship
      (table_can_be_parallel_scanned).

    NULL is a plan whose tables are all constant, which first_linear_tab()
    reports that way; it has no driving table to scan.

  @return  true if this table's access path can be divided among the workers.
*/

bool table_can_be_parallel_scanned(JOIN_TAB *tab)
{
  return tab &&
         (tab->type == JT_ALL || tab->type == JT_RANGE) &&
         tab->read_first_record == join_init_read_record &&
         is_parallel_scan_applicable(tab) &&
         table_can_be_parallel_scanned(tab->table);
}


/**
  @brief
    Recheck whether we can still run our query in parallel.
    trace it.

  @description
    Run this query with parallel workers if possible.

    1) Parallel worker threads available
    2) the driving table is read a way the workers can be given disjoint
         pieces of, and the query is one they can run themselves over those
         pieces (can_run_query_in_workers, which examines both -- it starts
         with table_can_be_parallel_scanned on the table passed to it)

    Engine-intrinsic constraints (consistent-read only, record format,
    discarded tablespace, ...) are enforced later inside
    parallel_init_coordinator(), which declines with HA_ERR_UNSUPPORTED so
    run_worker_side_join() falls back to serial execution. We leave the table's
    read_first_record as the serial reader: the manager never scans it (it only
    collects the workers' result rows), and the serial reader is what the
    fall-back path needs. do_select() dispatches on worker_side_parallel.
*/

void check_parallel_scan(JOIN *join)
{
  JOIN_TAB *first= first_linear_tab(join, WITH_BUSH_ROOTS,
                                    WITHOUT_CONST_TABLES);
  if (join->thd->variables.parallel_worker_threads > 0 &&       //1
      can_run_query_in_workers(join, first))                    //2
  {
    first->use_parallel_scan= join->worker_side_parallel= true;
    if (unlikely(join->thd->trace_started()))
    {
      Json_writer_object trace_pscan(join->thd);
      /*
        A candidate and not yet a conclusion: the plan can still change after
        this and take the parallel scan away again. JOIN::optimize_stage2()
        re-checks once it has stopped changing and says what became of it.
      */
      trace_pscan.add("parallel_scan_candidate", first->table->alias.c_ptr());
      /*
        What the workers will divide: the whole clustered index, or the key
        intervals of the range scan. The two are partitioned the same way, but
        which one it is decides whether the chunk boundaries are bounded by
        the range, so say so rather than leaving it to be inferred from the
        access method shown elsewhere in the trace.
      */
      trace_pscan.add("range_scan",
                      first->select && first->select->quick != NULL);
    }
  }

}

/**
  @brief
    Recheck whether we can still run our query in parallel.
    trace it.

  @description
    make_join_readinfo() decided whether the driving table would be scanned in
    parallel, and the plan has been able to change since: the
    test_if_skip_sort_order() calls above may have given that table an ordered
    index scan so that it supplies the GROUP BY or the ORDER BY order for free,
    and an ordered scan is not one that can be handed out in chunks.

    So re-check, and clear the decision if it no longer holds. Leaving it set
    is not a small matter either way: JOIN::worker_side_parallel is what
    do_select() dispatches on, so a manager would be built and the engine asked
    to partition the B-tree before something further down declined, and the
    trace would go on naming a table as chosen for a query that then ran
    serially -- which sends a reader looking in the wrong place.

    Nothing reaches the abandoned branch as the gate stands today, since it
    declines GROUP BY and ORDER BY outright and those are the only plans
    test_if_skip_sort_order() runs for. It goes in with the trace keys that
    describe it, so that when those plans are let through the decision is
    already being re-checked rather than being trusted from before the plan
    settled.
*/

void recheck_parallel_scan(JOIN *join)
{
  THD *thd= join->thd;
  JOIN_TAB *par= first_linear_tab(join, WITH_BUSH_ROOTS,
                                    WITHOUT_CONST_TABLES);
  JOIN_TAB *sorted= NULL;
  for (uint t= join->const_tables; t < join->table_count; t++)
    if (join->join_tab[t].filesort)
    {
      sorted= join->join_tab + t;
      break;
    }

  const bool still_divisible= table_can_be_parallel_scanned(par) && !sorted;

  if (!still_divisible)
  {
    if (par)
      par->use_parallel_scan= false;
    join->worker_side_parallel= false;
  }
  if (unlikely(thd->trace_started()))
  {
    Json_writer_object trace_pscan(thd);
    if (still_divisible)
      trace_pscan.add("chosen_for_parallel_scan", par->table->alias.c_ptr());
    else
    {
      trace_pscan.add("parallel_scan_abandoned",
                      par ? par->table->alias.c_ptr() : "");
      /* Named so it can be grepped for: "cause" is a common trace key. */
      trace_pscan.add("parallel_scan_abandoned_because",
                  sorted ?
                  "the rows would have to reach the join sorted" :
                  join->ordered_index_usage == join->ordered_index_group_by ?
                  "an index now supplies the GROUP BY order" :
                  join->ordered_index_usage == join->ordered_index_order_by ?
                  "an index now supplies the ORDER BY order" :
                  "the access path can no longer be divided in chunks");
    }
  }

}


/**
  @brief
    The whole condition this table has to be filtered by, whatever the plan did
    with it.

  @description
    When the optimizer pushes part of a condition into the engine,
    push_index_cond() leaves tab->select_cond holding only the remainder and
    keeps the original in tab->pre_idx_push_select_cond. The pushed half then
    lives in handler::pushed_idx_cond, which belongs to the manager's handler:
    a worker reads through its own, opened by open_table_from_share() with no
    condition pushed into it, so filtering by select_cond alone would apply the
    pushed half nowhere and return rows the serial plan rejects.

    A worker therefore filters by the pre-pushdown condition and does the whole
    job itself. That gives up what the pushdown was for -- the engine no longer
    rejects an index entry before the row is read -- so a worker does more
    clustered-index work per match than the serial plan does. Correctness first,
    and the alternative is to refuse these plans and lose the parallel scan
    altogether. Pushing a clone onto the worker's own handler would recover it,
    and wants the worker to hold a real JOIN_TAB to hang the key number off.

    There is a second place a condition can go. When an inner table is read
    through a BNL/BNLH join buffer, JOIN_TAB::make_scan_filter() copies the
    conjuncts that need only this table into cache_select->cond, for the buffer
    to apply as it fills, and JOIN_TAB::remove_redundant_bnl_scan_conds() then
    *removes* those conjuncts from select_cond -- it can empty it altogether. A
    worker uses no join buffer, so for a cached table select_cond is not the
    whole condition and filtering by it alone lets through everything the buffer
    would have rejected.

    There is a third. A table the plan sorts has its condition taken away
    from it altogether: JOIN::add_sorting_to_table() hands tab->select to the
    Filesort it builds and then nulls both tab->select and tab->select_cond,
    because the sorted read the tab is left with is fed by a scan the filesort
    has already filtered. So for a sorted tab the condition survives only as
    tab->filesort->select->cond, and a worker that asked the tab would filter
    by nothing and return the whole table.

    It is asked for last, not first: the SQL_SELECT the sort was handed is the
    one push_index_cond() had already taken a bite out of, because that runs
    long before the sort is added and leaves select->cond holding only the
    remainder. So a sorted tab that also pushed reaches here with the whole
    condition in pre_idx_push_select_cond and a partial one in the filesort,
    and taking the filesort's would apply the pushed half nowhere -- the same
    failure the pushdown case above exists to avoid, arriving by another route.

    All three are reported together, and the gate and both clone sites go
    through here, so the condition the gate approves is always the condition a
    worker ends up evaluating.
*/

static void pwt_table_conds(JOIN_TAB *tab, Item **cond, Item **cache_cond)
{
  if (tab->pre_idx_push_select_cond)
    *cond= tab->pre_idx_push_select_cond;
  else if (tab->filesort && tab->filesort->select)
    *cond= tab->filesort->select->cond;
  else
    *cond= tab->select_cond;
  *cache_cond= tab->cache_select ? tab->cache_select->cond : nullptr;
}


/*
  Whether every base-table column an expression reads is one of the GROUP BY
  columns, and so has one value per group.
*/

class Pwt_group_checker : public Field_enumerator
{
  ORDER *group;
public:
  bool all_grouped;
  Pwt_group_checker(ORDER *group_arg) : group(group_arg), all_grouped(true) {}
  void visit_field(Item_field *item) override
  {
    if (!item->field)
      return;
    for (ORDER *g= group; g; g= g->next)
    {
      Item *it= (*g->item)->real_item();
      if (it->type() == Item::FIELD_ITEM &&
          ((Item_field *) it)->field == item->field)
        return;
    }
    all_grouped= false;
  }
};


/*
  @brief
    Whether this query's aggregates can be computed per group by the workers
    and merged by the manager.

  @description
    Merging a partial is not adding a row: COUNT has to add a count rather than
    increment, and MIN and MAX have to reach the Item_cache their add() reads
    rather than args[0]. Item_sum::direct_add() does exactly that, and exists
    on Item_sum_count, Item_sum_sum -- a decimal and a real overload -- and
    Item_sum_min_max. Those four are what this accepts, and they are also the
    four whose reset_field() and update_field() honour a direct value, which is
    what a merge per group needs.

    What is refused, and why:

      - AVG, STD and VARIANCE. A partial average cannot be averaged. Their
        state would have to travel -- (sum, count), and (n, sum, sum-of-squares)
        -- which their temp-table field already holds, so this is a shape
        question rather than an impossibility.
      - BIT_AND, BIT_OR and BIT_XOR, which have no direct_add. They do not need
        one, being self-composing, but folding them in means redirecting
        args[0] and that is a second mechanism for a rare case.
      - The DISTINCT variants, whose set has to be complete before it can be
        counted. The server agrees: every merge path asserts the aggregator is
        not a DISTINCT one.
      - A key part that is not a plain column, or one too wide to key on.
      - A select-list item that is not an aggregate and not functionally
        dependent on the group, because the manager evaluates it once per group
        from base-table columns that hold only one of that group's rows.

  @param group  out: the GROUP BY key to accumulate per.

  @return true if the workers can pre-aggregate.
*/

static bool pwt_grouped_preagg_supported(JOIN *join, ORDER **group)
{
  *group= nullptr;
  if (!join->sum_funcs || !*join->sum_funcs)
    return false;
  if (join->rollup.state != ROLLUP::STATE_NONE)
    return false;

  ORDER *g= pwt_plan_group_key(join);
  if (!g)
    return false;

  for (Item_sum **s= join->sum_funcs; *s; s++)
  {
    switch ((*s)->sum_func()) {
    case Item_sum::COUNT_FUNC:
    case Item_sum::SUM_FUNC:
    case Item_sum::MIN_FUNC:
    case Item_sum::MAX_FUNC:
      break;
    default:
      return false;
    }
    if ((*s)->has_with_distinct())
      return false;
    if ((*s)->argument_count() != 1 ||
        !pwt_item_is_worker_safe(join, (*s)->get_arg(0)))
      return false;
  }

  uint key_length= 0;
  for (ORDER *k= g; k; k= k->next)
  {
    Item *item= (*k->item)->real_item();
    if (item->type() != Item::FIELD_ITEM || item->const_item())
      return false;
    if ((*k->item)->too_big_for_varchar())
      return false;
    Field *f= ((Item_field *) item)->field;
    key_length+= f->type() == MYSQL_TYPE_VARCHAR ||
                 f->type() == MYSQL_TYPE_VAR_STRING
                 ? f->field_length + HA_KEY_BLOB_LENGTH : f->pack_length();
    if ((*k->item)->maybe_null())
      key_length++;
  }
  if (key_length >= MAX_BLOB_WIDTH)
    return false;

  List_iterator_fast<Item> li(join->fields_list);
  Item *it;
  while ((it= li++))
  {
    if (it->type() == Item::SUM_FUNC_ITEM || it->const_item())
      continue;
    Pwt_group_checker check(g);
    it->walk(&Item::enumerate_field_refs_processor, (void *) &check, 0);
    if (!check.all_grouped)
      return false;
  }

  *group= g;
  return true;
}


/*
  The GROUP BY key this plan will pre-aggregate on, or nullptr if it will not.
  Asked by the gate and again when the workers are built, rather than carried
  between them: it is derived from the finished plan, which does not change in
  between.
*/

/*
  @brief
    The ORDER BY this thread has to apply itself, if there is one.

  @description
    make_aggr_tables_info() can sort the driving table's own read --
    add_sorting_to_table() on join_tab[const_tables] -- for a plan that needs
    no temporary table. The workers take that read over, so the sort loses the
    scan it was attached to, and a plan of this shape has no aggregation table
    to move it to: the terminal after the driving tab sends straight to the
    client. The manager does it instead, over the rows it drains, which costs
    nothing in ordering terms because a sort is indifferent to the order its
    input arrives in.

    What it can take is limited by where it does it. It sorts its own result
    container, so every ORDER BY element has to name a column that is in that
    container -- a field of a scanned table that the query actually reads, and
    so ships. An expression, or a field of some other table, has no column
    there to sort by, and the plan is left serial.

    A plan that has an aggregation table (join->aggr_tables) is not this shape:
    its sort belongs to that table and AGGR_OP::end_send() performs it, which
    is the manager's existing terminal path rather than this one.

  @return  the order to sort by, or nullptr if this plan's sort is not ours.
*/

ORDER *pwt_manager_sort_order(JOIN *join)
{
  JOIN_TAB *first= first_linear_tab(join, WITH_BUSH_ROOTS,
                                    WITHOUT_CONST_TABLES);
  if (!first || !first->filesort || join->aggr_tables)
    return nullptr;

  Filesort *fs= first->filesort;
  /*
    A sort that returns row ids, unpacks into other fields, or stops early is
    doing something for the plan beyond ordering, and the manager's is a plain
    sort of a container. LIMIT is refused by the gate already; the limit here
    is the filesort's own, which a plan can set separately.
  */
  if (!fs->order || fs->limit != HA_ROWS_MAX || fs->sort_positions ||
      fs->set_all_read_bits || fs->unpack)
    return nullptr;

  for (ORDER *o= fs->order; o; o= o->next)
  {
    Item *real= (*o->item)->real_item();
    if (real->type() != Item::FIELD_ITEM)
      return nullptr;                      // an expression: nothing to sort by
    Field *f= ((Item_field *) real)->field;

    bool scanned= false;
    for (uint t= join->const_tables; t < join->table_count; t++)
      if (join->join_tab[t].table == f->table)
      {
        scanned= true;
        break;
      }
    if (!scanned)
      return nullptr;
    /*
      Shipped is the same question as read: pwt_row_layout::build() ships every
      column whose read_set bit is set. Asked rather than assumed, because this
      runs before the layout exists.
    */
    if (!bitmap_is_set(f->table->read_set, f->field_index))
      return nullptr;
  }
  return fs->order;
}


ORDER *pwt_preagg_group(JOIN *join)
{
  ORDER *g= nullptr;
  if (join->group_list || join->group ||
      join->select_lex->agg_func_used() || join->select_lex->with_sum_func)
    (void) pwt_grouped_preagg_supported(join, &g);
  return g;
}


/*
  @brief
    Determine if the parallel workers run this whole query themselves and ship
      final result rows.
      (Called from make_join_readinfo, see JOIN::worker_side_parallel.)

  @description
  True for a streaming select-project[-join] query whose driving table
  'scan_tab' is parallel-scannable and whose remaining tables the workers can
  join themselves:
    - 'scan_tab' is read a way the workers can be given disjoint pieces of
      (tab_can_be_parallel_scanned), which also rules out a plan whose tables
      are all constant and so has no driving table at all;
    - all joins are inner (no outer-join null-complementation, no semijoin),
      and every non-driving table is reached by an index ref/eq_ref lookup or a
      plain full scan -- nothing that needs a join buffer, range/quick, rowid
      filter, loose scan, or a subquery trigger guard on the ref;
    - no temporary table (rules out GROUP BY, DISTINCT, ORDER BY, window
      functions, SQL_BUFFER_RESULT, ...): the result is the plain concatenation
      of the per-chunk join results;
    - no LIMIT/OFFSET, SQL_CALC_FOUND_ROWS, aggregate, or PROCEDURE -- these
      need a global post-pass the workers cannot do independently;
    - every condition, ref value and select-list item can be deep-cloned (so
      each worker gets private, thread-safe copies bound to its own tables).

  Anything else simply runs serially.

  @return
        true  if we can run this query in parallel
        false otherwise
*/
/*
  @brief
    PROTOTYPE. Whether to run the workers as a bare parallel scan.

  @description
    Not for the shipping tree: a second execution path, behind
    debug_dbug='+d,pwt_scan_only', in which the workers do nothing but read
    their chunk of the driving table and ship the rows. There are no worker
    JOIN_TABs, no cloned conditions or select list, and no join in a worker.
    This thread runs the plan it would have run serially, and the only
    difference is that the driving table's JOIN_TAB reads its rows from the
    transport instead of from the handler.

    What it is for. The gate admits a few per cent of the queries in the test
    suite, because almost every refusal it makes is about evaluating cloned
    Items in another thread rather than about dividing a scan. Take the
    evaluation away and nearly all of them go: outer joins, subqueries,
    aggregates, DISTINCT, LIMIT, window functions all become eligible, because
    this thread does them. So the chunked scan and the transport -- the two
    layers underneath -- can be run over the whole suite instead of over the
    sliver the real gate allows.

    What it cannot tell you, which is the more important half. Everything a
    worker does with an Item is gone, and that is where the defects have
    actually been: a worker THD that did not carry the session's time zone, a
    condition left only in a Filesort, a reader that wanted a SQL_SELECT the
    worker copy did not have, a materialized subquery re-opened per worker.
    None of those are reachable here -- most cannot even exist in this shape.
    Passing in this mode says nothing about the path that ships.

    Its value is therefore as a lower layer's test and as a bisection tool: a
    query that answers wrongly in both modes is wrong in the scan or the
    transport, and one that answers wrongly only in the full mode is wrong in
    the worker join.

  @return  true if this statement should run scan-only.
*/

bool pwt_scan_only_enabled()
{
  bool on= false;
  DBUG_EXECUTE_IF("pwt_scan_only", on= true;);
  return on;
}


bool can_run_query_in_workers(JOIN *join, JOIN_TAB *scan_tab)
{
  DBUG_ENTER("can_run_query_in_workers");
  SELECT_LEX *sl= join->select_lex;

  if (!table_can_be_parallel_scanned(scan_tab))
  {
    DBUG_PRINT("info", ("no table this plan reads in a divisible way"));
    DBUG_RETURN(false);
  }
  /* The loop below reads the driving table as join_tab[const_tables]. */
  DBUG_ASSERT(scan_tab == join->join_tab + join->const_tables);

  /*
    Scan-only: everything below this point asks whether a worker can evaluate
    part of this query, and in that mode no worker evaluates anything. The one
    question left is the one already answered above -- can the driving table's
    access path be divided -- with the sort excluded, because a filesort on the
    driving tab is performed by create_sort_index() reading the handler
    directly, which would walk straight past the transport.
  */
  if (pwt_scan_only_enabled())
  {
    if (scan_tab->filesort || scan_tab->filesort_result)
    {
      DBUG_PRINT("info", ("scan-only: the driving table is sorted"));
      DBUG_RETURN(false);
    }
    DBUG_RETURN(true);
  }
  /*
    A GROUP BY the workers can pre-aggregate is the one query shape that is
    allowed to want a temporary table, hold aggregates, and group. Everything
    below that would refuse those three is asked to let this one through.
  */
  ORDER *preagg_group= pwt_preagg_group(join);

  if (join->need_tmp && !preagg_group)            // group/distinct/order/...
  {
    DBUG_PRINT("info", ("group/distinct/order by"));
    DBUG_RETURN(false);
  }
  /*
    Any cap on the rows this select may send: an explicit LIMIT/OFFSET, or the
    session's sql_select_limit, which arrives with explicit_limit still unset
    -- mysql_execute_command() assigns it as the default limit of a top-level
    SELECT. The serial executor enforces the cap in end_send() against
    unit->lim; the drain does not, so a capped query run in the workers would
    send every row. unit->lim is what end_send() would have read, so that is
    the fact tested rather than the syntax -- which also keeps a derived
    table's select eligible, its unit carrying no session cap.
  */
  if (!join->unit->lim.is_unlimited())            // LIMIT / sql_select_limit
  {
    DBUG_PRINT("info", ("limit/offset"));
    DBUG_RETURN(false);
  }
  if (join->select_options & OPTION_FOUND_ROWS)  // SQL_CALC_FOUND_ROWS
  {
    DBUG_PRINT("info", ("SQL_CALC_FOUND_ROWS"));
    DBUG_RETURN(false);
  }
  if (join->procedure)
  {
    DBUG_PRINT("info", ("procedure"));
    DBUG_RETURN(false);
  }
  if (sl->have_window_funcs())
  {
    DBUG_PRINT("info", ("window funcs"));
    DBUG_RETURN(false);
  }
  if ((sl->agg_func_used() || sl->with_sum_func) && !preagg_group)
  {
    DBUG_PRINT("info", ("aggregate funcs"));
    DBUG_RETURN(false);
  }

  if ((join->group_list || join->group) && !preagg_group)
  {
    DBUG_PRINT("info", ("group"));
    DBUG_RETURN(false);
  }
  if (join->select_distinct)
  {
    DBUG_PRINT("info", ("distinct"));
    DBUG_RETURN(false);
  }
  if (join->having || join->tmp_having)
  {
    DBUG_PRINT("info", ("having"));
    DBUG_RETURN(false);
  }
  if (join->outer_join)                             // no outer joins
  {
    DBUG_PRINT("info", ("outer_join"));
    DBUG_RETURN(false);
  }

  // every non-const join table must be one the worker can scan / look up itself
  for (uint j= join->const_tables; j < join->table_count; j++)
  {
    JOIN_TAB *tab= &join->join_tab[j];
    if (tab->bush_children)                       // semijoin materialization
    {
      DBUG_PRINT("info", ("SJM on %s",tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
    /*
      Every table the worker joins is re-opened from its share --
      open_worker_tables() gives each worker its own TABLE so the handlers do
      not contend -- and only a base table can be opened that way. A temporary
      table's share describes something built in memory for this statement:
      open_table_from_share() walks share->field cloning each column and
      crashes on it.

      The plan reaches here holding one whenever a subquery was materialized:
      SJ-Materialization-Lookup leaves the materialized result in the top-level
      table list as <subqueryN>, read by eq_ref on its distinct key, and that
      tab has no bush_children to catch it -- only the Scan variant does.

      The driving table is checked by table_can_be_parallel_scanned(), which
      asks the same question among others; this is the rest of the plan.
    */
    if (j > join->const_tables && tab->table->s->tmp_table != NO_TMP_TABLE)
    {
      DBUG_PRINT("info", ("temporary table %s",tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
    /*
      The worker runs a plain nested loop, so it implements none of the semijoin
      duplicate-elimination strategies. Nothing in the flat table list shows that
      FirstMatch or DuplicateWeedout is in force, and a worker that ignores them
      emits exactly the duplicates they exist to remove, so refuse a plan that
      uses any of them. bush_children above covers the materialized ones.
    */
    if (tab->sj_strategy != SJ_OPT_NONE || tab->loosescan_match_tab ||
        tab->do_firstmatch || tab->check_weed_out_table ||
        tab->flush_weedout_table || tab->first_weedout_table)
    {
      DBUG_PRINT("info", ("semijoin strategy on %s",tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
    if (tab->rowid_filter)                        // rowid filter
    {
      DBUG_PRINT("info", ("rowid_filter on %s",tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
    /*
      The driving table may be read through a quick select -- checked above,
      where the whole access path is -- but the tables joined after it may not:
      a worker's tab keeps the record source make_join_readinfo() chose and
      reads through no quick select of its own. setup_worker_jointabs() sets
      the copy's 'select' to NULL, because the SQL_SELECT and the quick hanging
      off it name the manager's table and its Items.

      use_quick == 2 is "Range checked for each record", where there is no
      quick select yet to find: make_join_readinfo() gave the tab
      join_init_quick_read_record(), which builds one per outer row by calling
      test_if_quick_select(), and that begins "delete tab->select->quick".
      Testing for a quick therefore misses exactly the plan that needs the
      SQL_SELECT most, and the worker dereferences the NULL.

      So the test is on the reader the plan chose, not on what it has built
      yet.
    */
    if (j > join->const_tables &&
        tab->select && (tab->select->quick || tab->use_quick == 2))
    {
      DBUG_PRINT("info", ("quick select on %s",tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
    if (j > join->const_tables)                     // non-driving table access
    {
      if (tab->type != JT_EQ_REF && tab->type != JT_REF && tab->type != JT_ALL)
      {
        DBUG_PRINT("info", ("%s on %s",
                            join_type_str[tab->type],
                            tab->table->alias.ptr()));
        DBUG_RETURN(false);
      }
      if (tab->type == JT_EQ_REF || tab->type == JT_REF)
        for (uint p= 0; p < tab->ref.key_parts; p++)
        {
          if (tab->ref.cond_guards && tab->ref.cond_guards[p])  // subquery trigger
          {
            DBUG_PRINT("info", ("cond_guards, %s",tab->table->alias.ptr()));
            DBUG_RETURN(false);
          }
          if (!pwt_item_is_worker_safe(join, tab->ref.items[p]))
          {
            DBUG_PRINT("info", ("ref not worker-safe, %s",
                                tab->table->alias.ptr()));
            DBUG_RETURN(false);
          }
        }
    }
    Item *tab_cond, *tab_cache_cond;
    pwt_table_conds(tab, &tab_cond, &tab_cache_cond);
    if ((tab_cond && !pwt_item_is_worker_safe(join, tab_cond)) ||
        (tab_cache_cond && !pwt_item_is_worker_safe(join, tab_cache_cond)))
    {
      DBUG_PRINT("info", ("cond not worker-safe, %s",tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
  }

  // select list safe for a worker to evaluate
  List_iterator_fast<Item> li(join->fields_list);
  Item *item;
  while ((item= li++))
    if (!pwt_item_is_worker_safe(join, item))
    {
      DBUG_PRINT("info", ("select list item not worker-safe"));
      DBUG_RETURN(false);
    }
  DBUG_RETURN(true);
}


/*
  @brief
    Drive worker-side execution from do_select()
    spin up the workers, collect and send their result rows.

  @return
    0 = handled (result sent)
    1 = error
   -1 = the engine declined the parallel scan
          (caller should run the query serially instead).
*/

/*
  Scan-only PROTOTYPE. The manager's half: the driving table's JOIN_TAB is
  pointed at these, so the plan reads that table from the workers.
  ------------------------------------------------------------------------- */

bool pwt_manager::begin_scan_receive(THD *thd)
{
  return layout.begin_receive(thd, exec.tables, exec.n_tables);
}

void pwt_manager::end_scan_receive()
{
  layout.end_receive(exec.tables, exec.n_tables);
}


int pwt_manager::next_scanned_row()
{
  int rc= source->next_row(layout.recv_record());
  if (rc < 0)
    return -1;                                   // every worker is done
  if (rc > 0)
    return 1;                                    // killed, or a worker failed
  /*
    Put the shipped columns where the plan expects to read them: the manager's
    own record buffer for this table. From here the executor cannot tell the
    row came from a worker rather than from the handler.
  */
  layout.copy_back_row();
  return 0;
}


/* The manager for the join this table belongs to. */
static pwt_manager *pwt_mgr_of(TABLE *table)
{
  JOIN_TAB *tab= table->reginfo.join_tab;
  DBUG_ASSERT(tab && tab->join && tab->join->parallel_work_manager);
  return (pwt_manager*) tab->join->parallel_work_manager;
}


static int pwt_mgr_scan_read_record(READ_RECORD *info)
{
  int rc= pwt_mgr_of(info->table)->next_scanned_row();
  if (rc > 0 && info->print_error && !info->table->in_use->is_error())
    my_error(ER_INTERNAL_ERROR, MYF(0), "parallel scan: worker failed");
  return rc;
}


static int pwt_mgr_scan_init_read_record(JOIN_TAB *tab)
{
  pwt_manager *mgr= pwt_mgr_of(tab->table);
  if (mgr->begin_scan_receive(tab->join->thd))
    return 1;
  tab->read_record.table= tab->table;
  tab->read_record.read_record_func= pwt_mgr_scan_read_record;
  tab->read_record.read_record_func_and_unpack_calls= pwt_mgr_scan_read_record;
  /* read_first_record means "and read the first row" -- see
     join_init_read_record(). */
  return tab->read_record.read_record();
}


int pwt_manager::start_scan_only(THD *thd, JOIN *join, JOIN_TAB *scan_tab)
{
  DBUG_ENTER("pwt_manager::start_scan_only");
  scan_only= true;

  int err= init_parallel_workers(thd, join, scan_tab);
  if (err == HA_ERR_UNSUPPORTED)
    DBUG_RETURN(-1);                             // engine declined
  if (err)
    DBUG_RETURN(1);

  status_var_increment(thd->status_var.parallel_queries_executed);

  /*
    And divert the plan's read of this table. Everything after it -- the join,
    the conditions this thread still owns, the terminals -- is the executor's
    own, untouched.
  */
  scan_only_tab= scan_tab;
  saved_read_first_record= scan_tab->read_first_record;
  scan_tab->read_first_record= pwt_mgr_scan_init_read_record;

  /*
    push_index_cond() left this tab filtering by the remainder, with the rest
    in the manager handler's pushed_idx_cond. That handler no longer produces
    the rows, so the pushed half would be applied nowhere and this thread would
    see rows the serial plan rejects. Give the tab the whole condition back for
    the length of the scan -- the same reason pwt_table_conds() prefers
    pre_idx_push_select_cond in the full path, reached from the other side.
  */
  if (scan_tab->pre_idx_push_select_cond)
  {
    saved_select_cond= scan_tab->select_cond;
    scan_tab->set_select_cond(scan_tab->pre_idx_push_select_cond, __LINE__);
  }
  DBUG_RETURN(0);
}


/*
  @brief
    Scan-only PROTOTYPE: set the workers scanning, and leave do_select() to run
    the plan normally on top of them.

  @return  0 if the workers are running, -1 to run serially, 1 on error.
*/

int run_scan_only_workers(JOIN *join, JOIN_TAB *scan_tab)
{
  DBUG_ENTER("run_scan_only_workers");
  THD *thd= join->thd;
  pwt_manager *mgr= new (thd->mem_root) pwt_manager;
  if (!mgr)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(pwt_manager));
    DBUG_RETURN(1);
  }
  join->parallel_work_manager= mgr;

  int rc= mgr->start_scan_only(thd, join, scan_tab);
  if (rc < 0)
    join->parallel_work_manager= nullptr;
  DBUG_RETURN(rc);
}


int run_worker_side_join(JOIN *join, JOIN_TAB *scan_tab)
{
  DBUG_ENTER("run_worker_side_join");
  DBUG_PRINT("info",  ("parallel scan on %s", scan_tab->table->alias.ptr()));
  THD *thd= join->thd;
  pwt_manager *mgr= new (thd->mem_root) pwt_manager;
  if (!mgr)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(pwt_manager));
    DBUG_RETURN(1);
  }
  join->parallel_work_manager= mgr;

  int err= mgr->init_parallel_workers(thd, join, scan_tab);
  if (err == HA_ERR_UNSUPPORTED)
  {
    // TODO: This should not be done (at this stage)
    //  We don't do silent fallback when a part of query plan fails.
    //
    /*
      The engine declined the parallel scan -- e.g. this is a locking read, so
      not a consistent read (see parallel_init_coordinator). Nothing was set up
      and no error was raised: return the "declined" code so do_select() runs
      the query serially. Returning an error here would fail the statement with
      an empty diagnostics area.
    */
    DBUG_PRINT("info", ("engine declined the parallel scan, running serially"));
    join->parallel_work_manager= nullptr;
    DBUG_RETURN(-1);
  }
  if (err)
    DBUG_RETURN(1);           // init_parallel_workers has raised the error

  status_var_increment(thd->status_var.parallel_queries_executed);

  DBUG_RETURN(mgr->drain_and_send(join));
}


/*
  @brief
    Visitor that rebinds an Item_field from a manager join table to the
    worker's private copy of that table.

  @description
    A deep_copy of a WHERE/ref/select-list Item tree carries Item_field nodes
    whose Field* still points into a manager join table. We walk the clone with
    enumerate_field_refs_processor and, for every field that belongs to one of
    the parallel-scanned tables (from_tables[i]), repoint it at the same column
    of the worker's copy (to_tables[i]).

    A field of any other table is left untouched, and there must not be one:
    the worker would read a record the manager owns, which is what the in_use
    assertion in Field::val_int() catches. The gate has already refused any
    expression that reaches one, so the case cannot arise here -- see
    pwt_item_reads_a_foreign_table().
*/
class Pwt_field_rebinder : public Field_enumerator
{
  TABLE **from_tables, **to_tables;
  uint  n;
public:
  Pwt_field_rebinder(TABLE **from, TABLE **to, uint n_arg)
    : from_tables(from), to_tables(to), n(n_arg) {}
  void visit_field(Item_field *item) override
  {
    if (!item->field)
      return;
    for (uint i= 0; i < n; i++)
    {
      if (item->field->table == from_tables[i])
      {
        item->field= to_tables[i]->field[item->field->field_index];
        /*
          Item_field::save_in_field() (used when this field is a top-level
          projection item) copies from result_field, not field. The worker's
          value for this column lives in the rebound source field, so point
          result_field there too. Harmless for fields used only in val_*()
          (conditions, ref values and sub-expressions read field directly).
        */
        item->result_field= item->field;
        return;
      }
    }
  }
};


/*
  @brief
    Deep-clone an Item tree and rebind its join-table field references to the
    worker's private table copies.

  @return
        our cloned Item
        nullptr if the item is not clonable (deep_copy declined)
*/

static Item *pwt_clone_rebind(THD *thd, Item *src,
                              TABLE **from, TABLE **to, uint n)
{
  if (!src)
    return nullptr;

  Item *clone= src->deep_copy_with_checks(thd);
  if (!clone)
    return nullptr;

  /*
    1. Rebind every field reference to the worker's table copy. The fields stay
       fixed and keep their column position, so no name re-resolution happens.
       The gate rejected anything whose copy is not independent, so the walk
       below can only reach fields this clone owns.
  */
  DBUG_ASSERT(!item_clone_shares_nodes(src, clone));
  Pwt_field_rebinder rebinder(from, to, n);
  clone->walk(&Item::enumerate_field_refs_processor, (void*) &rebinder, 0);

  /*
    2. deep_copy duplicates the tree structurally but does not rebuild the
       fix-time caches some predicates hold (e.g. Item_func_in's in_vector,
       a comparator's collation/cmp_item) -- those would still compare against
       the original args and read the wrong table. Reset those caches on the
       non-field items (cleanup_excluding_fields_processor leaves the rebound,
       still-fixed fields alone) and re-fix the tree so they rebuild against the
       rebound args. A bare field clone is already fixed and needs no re-fix.
       (Same protocol the optimizer uses for re-evaluable clones, e.g.
       opt_rewrite_remove_casefold.cc.)
  */
  clone->walk(&Item::cleanup_excluding_fields_processor, (void*) nullptr, 0);
  if (!clone->fixed() && clone->fix_fields(thd, &clone))
    return nullptr;

  return clone;
}


/*
  @brief
    Clone both halves of one table's condition and rebind them to the worker's
    tables, ANDed together when there are two.
    *out is the clone, or NULL if the table has no condition at all.

  @return
    false     success
    true      error
*/

static bool pwt_clone_table_conds(THD *thd, JOIN_TAB *tab,
                                  TABLE **from, TABLE **to, uint n,
                                  Item **out)
{
  Item *cond, *cache_cond, *c= nullptr, *cc= nullptr;
  pwt_table_conds(tab, &cond, &cache_cond);
  *out= nullptr;

  if (cond && !(c= pwt_clone_rebind(thd, cond, from, to, n)))
    return true;
  if (cache_cond && !(cc= pwt_clone_rebind(thd, cache_cond, from, to, n)))
    return true;
  if (!c || !cc)
  {
    *out= c ? c : cc;
    return false;
  }

  /*
    Both clones are already fixed, so the conjunction needs no more than
    quick_fix_field() -- which is what remove_redundant_bnl_scan_conds() itself
    does when it rebuilds a condition out of fixed conjuncts. The worker only
    ever evaluates this item, so the fix-time caches Item_cond::fix_fields()
    would rebuild (used_tables, not_null_tables) are not read.
  */
  Item_cond_and *both= new (thd->mem_root) Item_cond_and(thd, c, cc);
  if (!both)
    return true;
  both->quick_fix_field();
  *out= both;

  return false;
}


/*
  @brief
    Build a worker-bound clone of a ref-access descriptor (mirrors the store_key
    setup in create_ref_for_key). The cloned ref has its own key buffer and a
    store_key per key part that evaluates the worker-bound key-value items into
    that buffer; cp_buffer_from_ref() then builds the lookup key from it.

  All key parts (even constant ones) get a runtime store_key -- we set
  const_ref_part_map to 0 so cp_buffer_from_ref recomputes every part each
  lookup. Re-evaluating a constant per lookup is cheap and avoids the
  "filled once at setup" bookkeeping.

  @return  true on error.
*/

static bool clone_table_ref(THD *thd, TABLE_REF *src, TABLE *wtable,
                            TABLE **from, TABLE **to, uint n, TABLE_REF *dst)
{
  const uint kp= src->key_parts;
  const uint len= src->key_length;
  KEY *keyinfo= wtable->key_info + src->key;

  dst->key_parts= kp;
  dst->key_length= len;
  dst->key= src->key;
  dst->key_err= 1;
  dst->has_record= FALSE;
  dst->null_rejecting= src->null_rejecting;
  dst->disable_cache= FALSE;
  dst->null_ref_part= NO_REF_PART;
  dst->null_ref_key= nullptr;
  dst->const_ref_part_map= 0;            // recompute every part each lookup
  dst->uses_splitting= FALSE;
  dst->use_count= 0;
  dst->cond_guards= nullptr;             // gated: no subquery trigger guards
  dst->depend_map= src->depend_map;

  if (!(dst->key_buff= thd->calloc<uchar>(ALIGN_SIZE(len) * 2)) ||
      !(dst->key_copy= thd->alloc<store_key*>(kp + 1)) ||
      !(dst->items= thd->alloc<Item*>(kp)))
    return true;
  dst->key_buff2= dst->key_buff + ALIGN_SIZE(len);

  store_key **ref_key= dst->key_copy;
  uchar *key_buff= dst->key_buff;
  for (uint i= 0; i < kp; i++)
  {
    Item *it= pwt_clone_rebind(thd, src->items[i], from, to, n);
    if (!it)
      return true;
    dst->items[i]= it;

    KEY_PART_INFO *kpi= keyinfo->key_part + i;
    const uint maybe_null= MY_TEST(kpi->null_bit);
    Item *real= it->real_item();
    store_key *sk;
    if (real->type() == Item::FIELD_ITEM)
      sk= new store_key_field(thd, kpi->field, key_buff + maybe_null,
                              maybe_null ? key_buff : 0, kpi->length,
                              ((Item_field*) real)->field, real->full_name());
    else
      sk= new store_key_item(thd, kpi->field, key_buff + maybe_null,
                             maybe_null ? key_buff : 0, kpi->length, it, FALSE);
    if (!sk)
      return true;
    *ref_key++= sk;
    key_buff+= kpi->store_length;
  }
  *ref_key= nullptr;                     // end marker
  return false;
}


/*
  A worker's private copy of the plan: its tables, its result container, its
  JOIN and its join tabs. Built on the manager's thread, before the worker
  threads start.
*/

static void close_one_worker_table(TABLE **t)
{
  if (*t)
  {
    (*t)->file->update_global_table_stats();
    closefrm(*t);
    my_free(*t);
    *t= nullptr;
  }
}


/**
  @brief
    Open this worker's private copy of every non-const join table from the
    shared TABLE_SHARE.

  @description
    open_table_from_share() runs here on the manager thread, so the open must
    happen with in_use == current_thd (handler::ha_thd() asserts that, and
    ha_innobase::open() calls it); we repoint in_use at the worker afterwards.
    InnoDB caches the THD lazily (update_thd() on first use), not at open, so
    each worker gets a private handler and they scan concurrently without a
    shared-scan lock. exec.tables[0] is the parallel-scanned driving table
    (also kept as exec.scan_table). Self-cleans on failure.

  @return  true on error.
*/
bool pwt_manager::open_worker_tables(THD *thd, pwt_worker *worker)
{
  worker->exec.n_tables= exec.n_tables;
  /* the table array, plus the ANALYZE counters this worker will fill in */
  if (!(worker->exec.tables= thd->alloc<TABLE*>(exec.n_tables)) ||
      !(worker->exec.tab_stats= thd->calloc<Table_access_tracker>(exec.n_tables)) ||
      !(worker->exec.tab_hstats= thd->calloc<ha_handler_stats>(exec.n_tables)))
    return true;
  for (uint t= 0; t < exec.n_tables; t++)
    worker->exec.tables[t]= nullptr;

  for (uint t= 0; t < exec.n_tables; t++)
  {
    TABLE *src= exec.tables[t];
    TABLE *st= (TABLE*) my_malloc(key_memory_TABLE, sizeof(TABLE),
                                  MYF(MY_WME | MY_ZEROFILL));
    if (!st)
      goto err;
    if (open_table_from_share(thd, src->s, &src->s->table_name,
                              HA_OPEN_KEYFILE | HA_TRY_READ_ONLY,
                              EXTRA_RECORD, thd->open_options, st,
                              false, nullptr))
    {
      my_free(st);
      goto err;
    }
    st->in_use= worker->thd;
    st->file->ha_handler_stats_reset();
    /*
      Give the copy the manager table's place in the join: its bit in the table
      map and its join position. Items cloned onto this table take used_tables()
      from TABLE::map, so with a zero map a rebound predicate looks like a
      constant -- Item_cond::fix_fields() then evaluates it while we are still
      cloning it (can_eval_in_optimize()), reading a record buffer that no row
      has been read into yet.
    */
    st->map= src->map;
    st->tablenr= src->tablenr;
    /*
      Mirror the manager table's column bitmaps into the copy's own, rather than
      marking every column: the optimizer has already marked exactly the columns
      this query reads, and the engine builds its fetch template from read_set,
      so a blanket all-columns read_set would convert every column of every row
      to MySQL format instead of just the ones the worker evaluates. Copying
      also keeps write_set empty for a table we only read, so a store into a
      source field still trips marked_for_write().

      The copy owns these bitmaps, so nothing is shared with the manager. Done
      here, before the conditions and the select list are cloned onto these
      tables, so the read_set is already in place if anything evaluates a field
      during cloning. open_worker_tables() runs after the optimizer has
      finished, so src's bitmaps are final.
    */
    bitmap_copy(&st->def_read_set, src->read_set);
    bitmap_copy(&st->def_write_set, src->write_set);
    st->column_bitmaps_set(&st->def_read_set, &st->def_write_set);
    worker->exec.tables[t]= st;
  }
  worker->exec.scan_table= worker->exec.tables[0];   // the driving table
  return false;

err:
  my_error(ER_INTERNAL_ERROR, MYF(0),
           "init_parallel_workers: failed to open worker table from share");
  worker->close_tables();
  return true;
}


/**
  @brief
    Take the engine's per-table counters out of the tables while they are still
    open, so the manager can add them to what ANALYZE reads.
*/

void pwt_worker::snapshot_table_stats()
{
  for (uint i= 0; i < exec.n_tables; i++)
    if (ha_handler_stats *hs= exec.tables[i]->file->handler_stats)
      exec.tab_hstats[i].add(hs);
}


void pwt_worker::close_tables()
{
  if (exec.tables)
    for (uint i= 0; i < exec.n_tables; i++)
      close_one_worker_table(&exec.tables[i]);
  exec.scan_table= nullptr;       // == exec.tables[0], closed above
}


void pwt_manager::free_containers(THD *thd)
{
  for (uint i= 0; i < nworkers(); i++)
  {
    layout.free_container(thd, &workers[i]->exec.result);
    layout.free_container(thd, &workers[i]->exec.group_container);
  }
  /*
    The sort result reads through the container, so it goes first. Both are
    ours alone: nothing outside this thread ever saw either.
  */
  delete sort_result;
  sort_result= nullptr;
  layout.free_container(thd, &sort_container);
  layout.cleanup(thd);
}


#ifndef DBUG_OFF
/*
  Every JOIN_TAB field the worker's executor could reach that the gate is
  supposed to have made inert. Checked on the manager's tab before it is copied,
  so relaxing a gate without teaching the copy about the field it lets through
  fails here rather than silently executing with the manager's state -- a copy
  keeps the manager's pointer in anything the copy does not overwrite, which is
  the same trap TABLE::map and TABLE::in_use were.

  This is not only a guard. Writing it is what found the BNL scan filter: the
  question "is JOIN_TAB::cache_select inert?" turned out to have the answer no,
  and a wrong result behind it. Anything added here should be *checked* against
  the suite, not assumed.
*/
static void pwt_assert_tab_inert(JOIN_TAB *tab, bool is_driving)
{
  /* Outer joins: the gate refuses join->outer_join outright. */
  DBUG_ASSERT(!tab->first_inner);
  DBUG_ASSERT(!tab->last_inner);
  DBUG_ASSERT(!tab->first_upper);
  DBUG_ASSERT(!tab->on_expr_ref || !*tab->on_expr_ref);
  DBUG_ASSERT(!tab->on_precond);

  /* Semijoin strategies and semijoin materialization. */
  DBUG_ASSERT(!tab->bush_children);
  DBUG_ASSERT(tab->sj_strategy == SJ_OPT_NONE);
  DBUG_ASSERT(!tab->loosescan_match_tab);
  DBUG_ASSERT(!tab->do_firstmatch);
  DBUG_ASSERT(!tab->flush_weedout_table);
  DBUG_ASSERT(!tab->check_weed_out_table);
  DBUG_ASSERT(!tab->first_weedout_table);
  DBUG_ASSERT(!tab->emb_sj_nest);

  /*
    Rowid filters and range/index_merge access. The driving table may carry a
    quick select -- its intervals are what the engine partitions -- but the
    worker never reads through it: setup_worker_jointabs() clears the copy's
    'select', and the worker filters by the cloned condition instead.
  */
  DBUG_ASSERT(!tab->rowid_filter);
  DBUG_ASSERT(is_driving || !tab->select || !tab->select->quick);
  /*
    "Range checked for each record": the reader builds a quick select per outer
    row out of tab->select, which a worker does not have. Asserted next to the
    quick above because it is the same rule -- an inner tab reads through no
    quick select of its own -- and the one the quick test cannot see, there
    being nothing built yet at this point.
  */
  DBUG_ASSERT(is_driving || tab->use_quick != 2);

  /*
    A sort is only ever the driving table's -- pwt_manager_sort_order() takes
    that plan shape, and the manager runs the sort -- and no sort has run yet.
  */
  DBUG_ASSERT(is_driving || !tab->filesort);
  DBUG_ASSERT(!tab->filesort_result);

  /* Materialized derived tables, CTEs and split materialization. */
  DBUG_ASSERT(!tab->split_derived_to_update);

  /* DISTINCT and HAVING are refused, so neither shortcut applies. */
  DBUG_ASSERT(!tab->shortcut_for_distinct);
  DBUG_ASSERT(!tab->having);

  /* The driving table is scanned; only the tables after it are looked up. */
  DBUG_ASSERT(is_driving || tab->type == JT_ALL || tab->type == JT_REF ||
              tab->type == JT_EQ_REF);
}
#endif


/*
  Every JOIN field the worker's executor could reach that the gate is supposed to
  have made inert. Same purpose as pwt_assert_tab_inert(), one level up: relaxing
  a gate without teaching setup_worker_join() about the field it lets through
  fails here rather than running with a field this worker's JOIN does not carry.
*/
static void pwt_assert_join_inert(JOIN *join, bool grouped)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(!join->outer_join);
  DBUG_ASSERT(!join->select_distinct);
//  DBUG_ASSERT(!join->order);
  /*
    A pre-aggregating plan does have a temp table, a group and a sort-and-group
    terminal: they belong to the aggregation stage, which the manager runs and
    a worker does not. What matters is that the worker's own JOIN does not
    carry them, and it cannot -- setup_worker_join() builds a fresh one and
    names every field it copies.
  */
  if (!grouped)
  {
    DBUG_ASSERT(!join->need_tmp);
    DBUG_ASSERT(!join->group && !join->group_list);
    DBUG_ASSERT(!join->sort_and_group);
  }
  DBUG_ASSERT(!join->having && !join->tmp_having);
  DBUG_ASSERT(!join->procedure);
  DBUG_ASSERT(!join->group_optimized_away);
  DBUG_ASSERT(!(join->select_options & OPTION_FOUND_ROWS));
#else
  (void) join;
  (void) grouped;
#endif
}


/*
  @brief
    Build the JOIN this worker's JOIN_TABs belong to.

  @description
    Only the fields the executor reads are carried over, by name. The rest are
    whatever JOIN's own constructor makes them, which is why the constructor is
    used rather than a copy of the manager's object: JOIN declares its copy
    constructor and assignment private and unimplemented, so copying it means
    copying bytes past a deliberate prohibition, and every field this code did
    not think about would then hold a value that looks chosen. Built by the
    constructor, an unconsidered field holds JOIN::init()'s default, and the
    static_assert below makes a field added upstream a compile-time prompt to
    decide which of the two it should be.

    What the executor reads, established by walking sub_select() and
    evaluate_join_record() for the shapes the gate allows: thd, join_tab,
    return_tab and found_records. map2table is read only for
    split_derived_to_update, join_tab_execution_startup() only inside its two
    semijoin-materialization branches, and JOIN_TAB::preread_init() returns
    before touching join->thd unless the table is a materialized derived -- all
    of which pwt_assert_tab_inert() asserts away.

    result is deliberately left null. It is the manager's connection to the
    client, and drain_and_send() is the only thing that may send a
    row; a worker that reached for it should crash rather than write to a socket
    two threads share.

  @return  true on error.
*/
bool pwt_manager::setup_worker_join(THD *thd, pwt_worker *worker)
{
  /*
    Adding a field to JOIN does not break this, it just stops the build until
    someone has decided whether a worker needs to carry it. Update the size once
    that decision is made.

    Debug builds only: JOIN carries dbug_join_tab_array_size under
    #ifndef DBUG_OFF, so its size is not the same in the two build types, and
    pinning both would mean two magic numbers of which only one is ever
    checked by whoever changes the class. mtr builds debug, so this is where
    the prompt lands.

    TODO: turn this off, doesn't work on windoze, not portable
  */
#ifndef DBUG_OFF
  static_assert(sizeof(JOIN) == 1552,
                "JOIN has changed shape: review what setup_worker_join() "
                "carries over to a worker");
#endif

  pwt_assert_join_inert(exec.join, layout.plan_aggregates);

  if (!(worker->exec.join= new (thd->mem_root)
          JOIN(worker->thd, exec.join->fields_list, exec.join->select_options,
               nullptr)))
    return true;

  /*
    The worker's JOIN describes the worker's array, which holds the join's
    non-const tables and nothing else. Not the manager's counts: the manager
    counts the const tables it resolved before the join started, and
    JOIN_TAB::pfs_batch_update() finds the innermost table by
    join_tab + table_count - 1, which with the manager's count lands past the
    end of a worker's array. join_tab itself is set by setup_worker_jointabs(),
    once there is an array to point at.
  */
  worker->exec.join->table_count= exec.n_tables;
  worker->exec.join->top_join_tab_count= exec.n_tables;
  worker->exec.join->const_tables= 0;
  worker->exec.join->select_lex= exec.join->select_lex;
  return false;
}


/*
  The executor plumbing this function installs on each tab. Defined below, with
  the rest of what runs on a worker thread.
*/
static int pwt_pscan_init_read_record(JOIN_TAB *tab);
static enum_nested_loop_state pwt_end_send(JOIN *join, JOIN_TAB *join_tab,
                                           bool end_of_records);

/**
  @brief
    Give this worker its own copy of each of the join's non-const JOIN_TABs,
    rebound to the worker: its private TABLE copies, its cloned conditions and
    refs, its own trackers. Tables are taken in join order, which is the order a
    worker joins them in, because a ref value reads the tables before it.

    The tabs are copied rather than built from nothing so that a field this code
    does not know about holds the value the optimizer chose instead of a zero
    that would look deliberate. What must not survive the copy is anything that
    points into the manager's execution state, so every such field is either
    overwritten below or asserted inert by pwt_assert_tab_inert().

  @return  true on error.
*/
bool pwt_manager::setup_worker_jointabs(THD *thd, pwt_worker *worker)
{
  if (!(worker->exec.jointabs= thd->alloc<JOIN_TAB>(exec.n_tables)))
    return true;
  worker->exec.join->join_tab= worker->exec.jointabs;

  for (uint k= 0; k < exec.n_tables; k++)
  {
    JOIN_TAB *mtab= exec.jointabs[k];
    JOIN_TAB *wtab= &worker->exec.jointabs[k];

#ifndef DBUG_OFF
    pwt_assert_tab_inert(mtab, k == 0);
#endif
    *wtab= *mtab;

    wtab->table= worker->exec.tables[k];

    /*
      Both halves of the condition, ANDed, so select_cond alone is the whole of
      what this table is filtered by. The places the optimizer had moved parts
      of it to are then cleared: they name items belonging to the manager, and a
      worker that read them would be evaluating another thread's Items.
    */
    if (pwt_clone_table_conds(thd, mtab, exec.tables, worker->exec.tables,
                              exec.n_tables, &wtab->select_cond))
      return true;
    wtab->pre_idx_push_select_cond= nullptr;
    wtab->cache_select= nullptr;
    wtab->select= nullptr;
    /*
      The plan's sort is the manager's to run (sort_and_send()) and the
      Filesort is the manager's to free: JOIN_TAB::cleanup() deletes both the
      filesort and, through it, the SQL_SELECT holding the plan's condition, so
      a worker's copy must not carry pointers a second cleanup would delete
      again. Nothing in a worker reads them either -- the driving tab reads
      through the chunk reader, not join_init_read_record().
    */
    wtab->filesort= nullptr;
    wtab->filesort_result= nullptr;

    /* No join buffer in a worker: it joins a row at a time. */
    wtab->cache= nullptr;
    wtab->use_join_cache= FALSE;
    wtab->jbuf_tracker= nullptr;

    /* ANALYZE reads these back off the manager, see quiesce_workers(). */
    wtab->tracker= &worker->exec.tab_stats[k];

    wtab->join= worker->exec.join;

    /*
      And the way back. join_read_next_same() and its relatives find the
      JOIN_TAB they are reading for through TABLE::reginfo.join_tab, at
      execution time, so a worker's table has to point at the worker's tab: the
      optimizer set this on the manager's tables and open_table_from_share()
      leaves a copy without it. Third field of TABLE to need this after map and
      in_use -- assume anything the optimizer sets on a TABLE is missing on the
      copy until checked.
    */
    wtab->table->reginfo.join_tab= wtab;

    /*
      Where a row goes once every table has matched: on to the next table, and
      off the end of the last one into pwt_end_send() rather than end_send().
      That is the whole of the difference between what a worker runs and what a
      serial plan runs.
    */
    wtab->next_select= (k + 1 < exec.n_tables) ? sub_select : pwt_end_send;

    /*
      The driving table comes from the engine's chunk reader. The others keep
      the record source make_join_readinfo() chose, which the struct copy
      brought with it: those functions take the JOIN_TAB they are called with,
      so they read this worker's table through this worker's ref and its own
      read_record.
    */
    if (!k)
      wtab->read_first_record= pwt_pscan_init_read_record;

    /*
      READ_RECORD mixes plan and per-scan state. make_join_readinfo() sets the
      row-fetching function and the unlock function at optimize time -- for a
      ref table join_read_next_same, for eq_ref join_no_more_records -- while
      the buffers and cursors are filled by read_first_record() when the scan
      starts. So clear it, which drops any state a previous execution of this
      statement left, and put the plan half back by hand. Zeroing all of it left
      null function pointers for sub_select() to call.
    */
    bzero((char*) &wtab->read_record, sizeof(wtab->read_record));
    wtab->read_record.read_record_func= mtab->read_record.read_record_func;
    wtab->read_record.read_record_func_and_unpack_calls=
      mtab->read_record.read_record_func_and_unpack_calls;
    wtab->read_record.unlock_row= mtab->read_record.unlock_row;
    wtab->read_record.table= wtab->table;
    wtab->read_record.thd= worker->thd;
    wtab->read_record.print_error= TRUE;

    if (k && (wtab->type == JT_EQ_REF || wtab->type == JT_REF))
    {
      if (clone_table_ref(thd, &mtab->ref, wtab->table,
                          exec.tables, worker->exec.tables, exec.n_tables,
                          &wtab->ref))
        return true;
    }

    /*
      Recomputed, not copied: it answers "is this the innermost table", which
      sub_select() asserts against the live answer, and the worker's array is
      not the manager's. Last, because it reads the condition and the type set
      above.
    */
    wtab->cached_pfs_batch_update= wtab->pfs_batch_update();
  }
  return false;
}


/**
  @brief
    Build this worker's private clones of the driving table's WHERE condition
    and the select list, rebinding their field references to the worker's table
    copies.

  @return  true  error
           false clone built
*/
bool pwt_manager::clone_worker_exprs(THD *thd, pwt_worker *worker)
{
  TABLE **from= exec.tables;
  TABLE **to= worker->exec.tables;

  // shipped columns -> per-item projection into the container's field[i]
  worker->exec.proj_count= layout.ship_list.elements;
  worker->exec.proj=
    (Item**) thd->alloc(worker->exec.proj_count * sizeof(Item*));
  if (!worker->exec.proj)
    return true;

  List_iterator_fast<Item> li(layout.ship_list);
  Item *src;
  uint i= 0;
  while ((src= li++))
  {
    if (!(worker->exec.proj[i++]= pwt_clone_rebind(thd, src, from, to,
                                                     exec.n_tables)))
      return true;
  }
  return false;
}


/**
  @brief
    Project the current full row and add its image to the worker's batch,
    handing the batch to the manager when it fills.

  @return  
    -1 = continue with inner join
     0 = row emitted
     1 = stop (manager asked us to stop, or a fatal error).
*/

/*
  The worker whose thread we are on. sub_select() drives the join through
  function pointers whose signatures are fixed -- READ_RECORD::Read_func takes a
  READ_RECORD and Next_select_func takes a JOIN and a JOIN_TAB -- and neither
  reaches the pwt_worker, so the callbacks below find it here. A worker is
  exactly one thread, which is what makes this sound; the pattern is mysqld.cc's
  THR_THD. Set for the length of execute_and_handoff() and cleared after, so a
  callback reached from anywhere else finds nothing rather than a stale worker.
*/
static thread_local pwt_worker *pwt_self;


/* Next row of this worker's chunk of the driving table. */
int pwt_worker::pscan_next_row()
{
  return exec.scan_table->file->ha_parallel_get_next_row(exec.handler_ctx);
}


/*
  @brief
    Give this worker its own grouping table, aggregates and group key.

  @description
    Runs on the manager's thread, with the rest of this worker's setup, because
    every container is built there -- see pwt_row_container.

    The aggregate is cloned and its argument is cloned and rebound separately,
    by the same route the row transport rebinds any expression: the aggregate
    must not go through fix_fields() again, because Item_sum::fix_fields()
    registers it with the select_lex the manager is also using. So the shell is
    copied, the rebound argument is grafted in, and setup_caches() rebuilds
    whatever the shell derived from the old argument -- for MIN and MAX the
    Item_cache pair and the comparator bound to them, for the others nothing.

    The clone then accumulates into a column of this worker's grouping table
    rather than into itself, which is the binding create_tmp_table() makes for
    the query's own aggregates and what reset_field()/update_field() use.

    The key entries had to name the layout's definition items while
    create_tmp_table() ran, because that is how it finds which column each key
    part covers. What they have to name from here on is something that reads
    the row the key is being built for, and a definition item does not: it is a
    clone of an Item_field over the *manager's* copy of the base table, which
    no worker ever fills. Left that way, every row keys on NULL and the whole
    chunk becomes one group.

  @return true on error.
*/

bool pwt_manager::setup_worker_preagg(THD *thd, pwt_worker *worker)
{
  if (!layout.grouped)
    return false;

  /*
    The columns of this worker's grouping table, defined by items that read
    this worker's tables. That is what makes end_update() usable unmodified:
    it fills the table with copy_fields(), through the Copy_field pairs
    create_tmp_table() derives from these very items, and it builds the group
    key with save_org_in_field() from the same items. Define the columns from
    the manager's items instead and both would read records no worker fills.
  */
  List<Item> defn;
  {
    List_iterator_fast<Item> li(layout.ship_list);
    Item *src;
    while ((src= li++))
    {
      Item *c= pwt_clone_rebind(thd, src, exec.tables, worker->exec.tables,
                                exec.n_tables);
      if (!c || defn.push_back(c, thd->mem_root))
        return true;
    }
  }

  /*
    Then this worker's own aggregates. Cloned, and their argument cloned and
    rebound separately, by the same route the row transport rebinds any
    expression: the aggregate must not go through fix_fields() again, because
    Item_sum::fix_fields() registers it with the select_lex the manager is also
    using. So the shell is copied, the rebound argument is grafted in, and
    setup_caches() rebuilds whatever the shell derived from the old argument --
    for MIN and MAX the Item_cache pair and the comparator bound to them.
  */
  if (!(worker->exec.sums= thd->alloc<Item_sum*>(layout.n_sums + 1)))
    return true;
  worker->exec.sums[layout.n_sums]= nullptr;     // sum_funcs is NULL-terminated

  for (uint i= 0; i < layout.n_sums; i++)
  {
    Item *item_clone= layout.mgr_sums[i]->deep_copy_with_checks(thd);
    if (!item_clone)
      return true;
    Item_sum *item_sum= (Item_sum *) item_clone;

    Item *arg= pwt_clone_rebind(thd, layout.mgr_sums[i]->get_arg(0),
                                exec.tables, worker->exec.tables,
                                exec.n_tables);
    if (!arg)
      return true;
    item_sum->arguments()[0]= arg;
    item_sum->get_orig_args()[0]= arg;
    item_sum->setup_caches(thd);

    /*
      Its own aggregator, so that reset_field() has the kind it asserts on and
      it is not the one the query's own aggregate is using.
    */
    if (item_sum->set_aggregator(thd, Aggregator::SIMPLE_AGGREGATOR))
      return true;
    if (defn.push_back(item_sum, thd->mem_root))
      return true;
    worker->exec.sums[i]= item_sum;
  }

  /*
    The group key, over this worker's definition items. create_tmp_table()
    reads them to find which column each key part covers, and end_update()
    reads them again per row to build the key -- from the worker's tables,
    which is where the row it is keying actually is.
  */
  ORDER *group= thd->calloc<ORDER>(layout.n_group);
  if (!group)
    return true;
  {
    for (uint k= 0; k < layout.n_group; k++)
    {
      List_iterator_fast<Item> di(defn);
      Item *it= nullptr;
      for (uint j= 0; j <= layout.group_pos[k]; j++)
        it= di++;
      group[k].item_ptr=  it;
      group[k].item=      &group[k].item_ptr;
      group[k].direction= ORDER::ORDER_ASC;
      group[k].next=      k + 1 < layout.n_group ? &group[k + 1] : nullptr;
    }
  }

  /*
    In memory, and it has to be, which is also why a grouping table that fills
    is a hazard this does not yet handle.

    end_update() answers a full heap table by calling
    create_internal_tmp_table_from_heap(). A worker must not: that re-opens the
    table on this thread, and Aria binds an open handle to the opening thread's
    my_thread_var, which is freed the moment the worker's THD is destroyed.

    Building the container on disk from the start would make that branch
    unreachable -- for a table that is already on disk it reports an error and
    returns -- but it moves the problem rather than solving it, because
    tmp_space_used is charged to whichever thread writes an on-disk temp table
    and released by whichever frees it, and those are the worker and the
    manager. Doing so trips THD::free_connection()'s assertion, which is what
    the 13.0 tree's comment on this predicted.

    TODO: a worker's groups must currently fit its heap container. Neither
    answer to a full one is available to a worker as things stand, so the fix
    is to give AGGR_OP a hook for what to do when the table fills -- convert,
    as now, or ship what is there and start again empty, which is available to
    a worker because a partial aggregate is mergeable.
  */
  if (layout.make_container_from(thd, defn, &worker->exec.group_container,
                                 group))
    return true;

  /*
    flush_groups() copies a row of this table straight into the shipping
    container, so the two layouts have to agree. They are built from different
    item lists -- this one from the worker's, that one from the manager's --
    so they agree by both deriving from the same columns, not by construction.
  */
  DBUG_ASSERT(worker->exec.group_container.table->s->reclength ==
              layout.reclength);
  DBUG_ASSERT(worker->exec.group_container.table->s->fields ==
              layout.recv.table->s->fields);

  /*
    And the aggregation tab itself: the JOIN_TAB end_update() is called with.
    It reads the table, the param (for the key buffer, the Copy_field pairs and
    items_to_copy), the aggregate list off the JOIN, and its own AGGR_OP for
    the one branch that switches write function. Everything else it touches is
    a counter.
  */
  JOIN_TAB *aggr_tab= thd->calloc<JOIN_TAB>(1);
  if (!aggr_tab)
    return true;
  aggr_tab->join=             worker->exec.join;
  aggr_tab->table=            worker->exec.group_container.table;
  aggr_tab->tmp_table_param=  worker->exec.group_container.param;
  if (!(aggr_tab->aggr= new (thd->mem_root) AGGR_OP(aggr_tab)))
    return true;
  aggr_tab->aggr->set_write_func(end_update);
  worker->exec.aggr_tab= aggr_tab;

  /* The aggregates end_update() folds into are the JOIN's, so they are ours. */
  worker->exec.join->sum_funcs= worker->exec.sums;
  return false;
}


/*
  @brief
    Ship one row per group, once this worker's chunk is done with.

  @description
    Each row of the grouping table is a group's base columns and this worker's
    partial for it, in the layout every container shares, so shipping one is
    the same copy the ungrouped transport makes -- through the transport's own
    container, which is what the manager reads.

  @return  pwt_emit_result.
*/

int pwt_worker::flush_groups()
{
  DBUG_ENTER("pwt_worker::flush_groups");
  TABLE *table= exec.group_container.table;
  const uint reclength= manager->row_layout().reclength;
  int rc= PWT_EMIT_OK, error;

  table->file->ha_index_or_rnd_end();
  if ((error= table->file->ha_rnd_init(true)))
  {
    table->file->print_error(error, MYF(0));
    DBUG_RETURN(PWT_EMIT_ERROR);
  }
  while (!(error= table->file->ha_rnd_next(table->record[0])) ||
         error == HA_ERR_RECORD_DELETED)
  {
    if (error)
      continue;
    if (manager->is_fatal_error())
    {
      rc= PWT_EMIT_ERROR;
      break;
    }
    memcpy(exec.result.record(), table->record[0], reclength);
    if ((rc= sink->emit_row(exec.result.record())) != PWT_EMIT_OK)
      break;
  }
  table->file->ha_rnd_end();
  if (rc == PWT_EMIT_OK && error != HA_ERR_END_OF_FILE)
  {
    table->file->print_error(error, MYF(0));
    rc= PWT_EMIT_ERROR;
  }
  DBUG_RETURN(rc);
}


int pwt_worker::emit_joined_row()
{
  DBUG_ENTER("pwt_worker::emit_joined_row");
  /*
    Evaluate each cloned select-list item (which reads from the worker's table
    copies, now holding the current matching row of every table) and store it
    into the matching exec.result field. Evaluation errors are
    intercepted by PWT_error_handler, which trips fatal_error; the caller
    checks that.
  */
  for (uint i= 0; i < exec.proj_count; i++)
    exec.proj[i]->save_in_field(exec.result.table->field[i], false);

  if (manager->is_fatal_error())               // projection raised an error
    DBUG_RETURN(PWT_EMIT_ERROR);

  DBUG_RETURN(sink->emit_row(exec.result.record()));
}


/*
  @brief
    Read the next row of this worker's chunk of the driving table.

  @description
    The driving table's record source. Every other table in the join keeps the
    one make_join_readinfo() chose, because those read through the worker's own
    handler and its own ref; only this one has to come from the engine's chunk
    reader instead of a plain scan.

  @return  0 a row, -1 end of this worker's chunks, > 0 error. The contract
           READ_RECORD::Read_func has, see rr_handle_error().
*/
static int pwt_pscan_read_record(READ_RECORD *info)
{
  int err= pwt_self->pscan_next_row();
  if (!err)
    return 0;
  if (err == HA_ERR_END_OF_FILE)
    return -1;
  if (info->print_error)
    info->table->file->print_error(err, MYF(0));
  return 1;
}


/*
  @brief
    Point the driving table's JOIN_TAB at the chunk reader.

  @return  0 always: parallel_init_worker() is called once by
           execute_and_handoff() before the join starts, not here, because a
           failure to get a chunk has to be told apart from a failure to read
           one.
*/
static int pwt_pscan_init_read_record(JOIN_TAB *tab)
{
  /*
    Only the row-fetching half. setup_worker_jointabs() has already put the rest
    in place, unlock_row included, and evaluate_join_record() calls that.
  */
  tab->read_record.read_record_func= pwt_pscan_read_record;
  tab->read_record.read_record_func_and_unpack_calls= pwt_pscan_read_record;
  /*
    And read the first row, which is what read_first_record() means -- see the
    tail of join_init_read_record(). Returning without reading leaves
    evaluate_join_record() to run once on a record buffer nothing has filled,
    which shows up as one extra row per worker.
  */
  return tab->read_record.read_record();
}


/*
  @brief
    What the worker does with a fully joined row: the end of its nested loop.

  @description
    Stands where end_send() stands in a serial plan. The manager, not this, is
    what talks to the client.

  @return  NESTED_LOOP_OK to keep going, NESTED_LOOP_QUERY_LIMIT when the
           manager has asked this worker to stop, NESTED_LOOP_ERROR on a failed
           projection.
*/
static enum_nested_loop_state pwt_end_send(JOIN *join, JOIN_TAB *join_tab,
                                           bool end_of_records)
{
  DBUG_ENTER("pwt_end_send");
  if (end_of_records)
    DBUG_RETURN(NESTED_LOOP_OK);

  /*
    A pre-aggregating worker hands the row to the server's own grouped
    aggregation, over its own tab: AGGR_OP::put_record() prepares the table on
    first use and then calls end_update(), unmodified. Its result is already
    the enum this function returns.
  */
  if (pwt_self->manager->row_layout().grouped)
    DBUG_RETURN(pwt_self->exec.aggr_tab->aggr->put_record());

  switch (pwt_self->emit_joined_row()) {
  case PWT_EMIT_OK:   DBUG_RETURN(NESTED_LOOP_OK);
  case PWT_EMIT_STOP: DBUG_RETURN(NESTED_LOOP_QUERY_LIMIT);
  default:            DBUG_RETURN(NESTED_LOOP_ERROR);
  }
}


/**
  @brief
    Run the query over the worker's private chunk of the driving table, joining
    the other tables, and hand the *result* rows to the manager through this
    worker's end of the transport.

  The worker scans its own copy of the driving table (exec.scan_table, opened
  with in_use == this worker's thd) so the workers scan concurrently with no
  shared-scan lock. The join itself is the server's own nested loop:
  sub_select() is driven over this worker's JOIN_TABs, reading the driving
  table through the engine's chunk reader and every other table through this
  worker's handler, and each fully joined row leaves the last table through
  pwt_end_send(), which projects the shipped columns into exec.result and
  hands that record to exec's sink. The manager (drain_and_send) only
  concatenates these final rows.

  @return
  0 on success, or a handler error code. A clean stop requested by the manager
  (PWT_EMIT_STOP) also returns success: the manager is done, not in error.
*/

/*
  @brief
    Scan-only PROTOTYPE: project the row now in this worker's copy of the
    driving table into the result container and ship it.

  @description
    The full path evaluates a clone of the query's select list here. Scanning
    needs no such thing: the shipped columns are the columns themselves, so the
    projection is one Item_field per column, built over this worker's own table
    in setup_scan_only_worker().

  @return  pwt_emit_result.
*/

int pwt_worker::emit_scanned_row()
{
  for (uint i= 0; i < exec.scan_proj_count; i++)
    exec.scan_proj[i]->save_in_field(exec.result.table->field[i], false);

  if (manager->is_fatal_error())
    return PWT_EMIT_ERROR;
  return sink->emit_row(exec.result.record());
}


/*
  @brief
    Scan-only PROTOTYPE: read this worker's chunks and ship every row.

  @description
    execute_and_handoff() without the join. No inner tables to lock or index,
    no grouping table, no nested loop -- the chunk reader, and the transport.
    The manager applies the query's conditions, because in this mode it runs
    the whole plan.

  @return  0, or a handler error code.
*/

int pwt_worker::execute_scan_only()
{
  DBUG_ENTER("pwt_worker::execute_scan_only");
  TABLE *src= exec.scan_table;
  int err= 0;
  bool killed= false;

  exec.result.table->use_all_columns();

  if (sink->begin())
    DBUG_RETURN(HA_ERR_GENERIC);

  /* The manager's snapshot, for the same reason as the full path. */
  if (ha_clone_consistent_snapshot(thd, manager->thd))
  {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "parallel worker: cannot read the manager's snapshot");
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  }

  if ((err= src->file->ha_external_lock(thd, F_RDLCK)))
    DBUG_RETURN(err);

  if ((err= src->file->parallel_init_worker(exec.handler_ctx,
                                    manager->exec.scan_tab->table->file)))
    goto scan_exit;

  for (;;)
  {
    if (thd->killed || manager->is_fatal_error())
    {
      killed= true;
      break;
    }
    if ((err= pscan_next_row()))
    {
      /*
        Normalised here rather than at the exit label: running out of chunks is
        how this loop ends, and everything after it -- the flush that publishes
        the container to the manager above all -- tests for success.
      */
      if (err == HA_ERR_END_OF_FILE)
        err= 0;
      break;
    }

    int rc= emit_scanned_row();
    if (rc == PWT_EMIT_STOP)                     // the manager has had enough
      break;
    if (rc == PWT_EMIT_ERROR)
    {
      err= thd->is_error() ? thd->get_stmt_da()->sql_errno() : HA_ERR_GENERIC;
      break;
    }
  }
  src->file->parallel_end_worker();

  if (!err && !killed)
    sink->flush();

scan_exit:
  /*
    The other end of file: parallel_init_worker() reports it when there is no
    chunk left for this worker at all, which is success -- it has nothing to
    scan and nothing to publish. The full path normalises it in the same place
    and for the same reason.
  */
  if (err == HA_ERR_END_OF_FILE)
    err= 0;
  src->file->ha_index_or_rnd_end();
  src->file->ha_external_lock(thd, F_UNLCK);
  DBUG_RETURN(err);
}


int pwt_worker::execute_and_handoff()
{
  DBUG_ENTER("pwt_worker::execute_and_handoff");
  if (manager->is_scan_only())
    DBUG_RETURN(execute_scan_only());
  TABLE *src= exec.scan_table;
  //pwt_manager *mgr= manager;
  const uint nt= exec.n_tables;
  int err= 0;
  uint i;
  bool killed= false;

  // the source tables were marked in open_worker_tables; this one is ours
  exec.result.table->use_all_columns();         // we write every result column

  /*
    First thing on this thread: let the transport claim whatever it holds per
    producer. Before this, everything it owns was built by the manager.
  */
  if (sink->begin())
    DBUG_RETURN(HA_ERR_GENERIC);

  /*
    Adopt the manager's snapshot before touching any table. We run in our own
    THD, hence in our own transaction, so without this every worker would open
    its own read view at its first read: the workers, and the manager they work
    for, could each see a different version of the tables, and the chunk
    boundaries the manager's engine computed would not even belong to the
    snapshot we scan. The manager pinned its snapshot in
    parallel_init_coordinator() before any worker was created, and holds it
    until the workers have been reaped (quiesce_workers), which is also what
    keeps purge from removing the versions we still need.

    This covers every table we read, not just the parallel-scanned one: the
    snapshot belongs to the transaction, so the inner tables of the join are
    read at the same point in time as the driving table.
  */
  if (ha_clone_consistent_snapshot(thd, manager->thd))
  {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "parallel worker: cannot read the manager's snapshot");
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  }

  /*
    Our handles bypassed lock_tables(), so take the engine-level read lock on
    every table ourselves; InnoDB needs this to register a table with its trx
    before reading it. All-or-nothing: unlock the locked prefix on failure.
  */
  uint locked= 0;
  while (locked < nt)
  {
    if ((err= exec.tables[locked]->file->ha_external_lock(thd, F_RDLCK)))
    {
      while (locked--)
        exec.tables[locked]->file->ha_external_lock(thd, F_UNLCK);
      DBUG_RETURN(err);
    }
    locked++;
  }

  // Initialise the index on each ref/eq_ref inner table; reused across lookups.
  for (i= 1; i < nt; i++)
  {
    JOIN_TAB *it= &exec.jointabs[i];
    if ((it->type == JT_EQ_REF || it->type == JT_REF) &&
        (err= it->table->file->ha_index_init(it->ref.key, it->sorted)))
      break;
  }

  /*
    The grouping table is ours to write for the length of this chunk. Its index
    is not opened here: AGGR_OP::put_record() does that on its first call, the
    same lazy preparation the serial plan gets.
  */
  if (manager->row_layout().grouped)
  {
    exec.group_container.table->in_use= thd;
    exec.group_container.table->file->rebind_to_thread();
    exec.group_container.table->use_all_columns();
  }

  if (err)
    goto exec_exit;

  /*
    src is this worker's own copy of the driving table, opened from the share by
    open_worker_tables(), so its handler is not the one the coordinator was
    initialised on. The master's is, and it is where the scan parameters live;
    hand it over so the worker's handler can take what it needs of them.
  */
  if ((err= src->file->parallel_init_worker(exec.handler_ctx,
                                           manager->exec.scan_tab->table->file)))
    goto exec_exit;

  {
    /*
      Run the join the way do_select() runs it: once to produce the rows, then
      once more to signal end of records, which is what lets an operator that
      buffers flush. The chunk reader, the conditions, the refs and the trackers
      all hang off this worker's own JOIN_TABs, so the executor never touches
      the manager's. pwt_end_send() takes each finished row.
    */
    pwt_self= this;
    enum_nested_loop_state rc= sub_select(exec.join, exec.jointabs, FALSE);
    if (rc >= NESTED_LOOP_OK && !thd->killed)
      rc= sub_select(exec.join, exec.jointabs, TRUE);
    pwt_self= nullptr;

    if (rc == NESTED_LOOP_ERROR)
      err= thd->is_error() ? thd->get_stmt_da()->sql_errno() : HA_ERR_GENERIC;
    else if (thd->killed && !thd->is_error())
      my_error(ER_QUERY_INTERRUPTED, MYF(0));
    killed= thd->killed;
  }
  src->file->parallel_end_worker();

  // flush our grouping table
  if (!err && !killed && !manager->is_fatal_error() &&
      manager->row_layout().grouped && flush_groups() == PWT_EMIT_ERROR)
    err= thd->is_error() ? thd->get_stmt_da()->sql_errno() : HA_ERR_GENERIC;

  // hand over whatever the transport is still holding for us
  if (!err && !killed)
    sink->flush();

exec_exit:
  if (err == HA_ERR_END_OF_FILE)
    err= 0;

  // end any open index/rnd scans (no-op for tables left in NONE state), unlock
  for (i= 1; i < nt; i++)
    exec.jointabs[i].table->file->ha_index_or_rnd_end();
  if (manager->row_layout().grouped && exec.group_container.table)
    exec.group_container.table->file->ha_index_or_rnd_end();
  for (i= 0; i < nt; i++)
    exec.tables[i]->file->ha_external_lock(thd, F_UNLCK);

  DBUG_RETURN(err);
}


/**
   @brief  Run the query for this worker, ship the result rows to the manager,
           then tidy up.
*/

void pwt_worker::execute_and_signal_manager()
{
  DBUG_ENTER("pwt_worker::execute_and_signal_manager");

  err= execute_and_handoff();

  /*
    End the worker's read transaction now, while we are still on the worker
    thread. destroy_background_thd() -> THD::cleanup() rolls the transaction
    back and asserts (trans_check) that the statement transaction is already
    empty, so we must close it out here. Any commit failure is captured by the
    installed PWT_error_handler.
  */
  trans_commit_stmt(thd);
  trans_commit(thd);


  if (err)
    exec.scan_table->file->print_error(err, MYF(0));

  DBUG_VOID_RETURN;
}


/*
  @brief
    Drain the workers' result rows and send them to the client.

  @description
  The workers ran the whole select-project query over their disjoint chunks
  and produced the final result rows; the manager just takes them from the
  transport, in whatever order it delivers them, and sends each to the client. The select-list metadata was
  already sent (from the query's own field list) before do_select() ran, so we
  only supply the row values here -- the select list is evaluated against the
  manager's own records, which the drain has filled from the shipped columns.
  send_records is advanced so do_select() can set limit_found_rows.

  @return
    0 success (all rows sent)
    1 error.
*/
/*
  @brief
    The container the drain collects into when the plan's sort is ours.

  @description
    One more container of the transport's own layout, so a drained record can
    be written into it as it stands. It is this thread's alone -- written,
    sorted and read here -- unlike the workers', which cross a thread boundary.

  @return  true on error (my_error() called).
*/

/*
  @brief
    Scan-only PROTOTYPE: the worker's projection.

  @description
    pwt_row_layout::build() ships every column of the driving table whose
    read_set bit is set, in table order, and open_worker_tables() copies that
    read_set onto the worker's table. So walking the worker's own fields the
    same way visits the same columns in the same order, and column i of the
    container is the i-th of them.

  @return  true on error (my_error() called).
*/

bool pwt_manager::setup_scan_only_proj(THD *thd, pwt_worker *worker)
{
  TABLE *wt= worker->exec.scan_table;
  const uint n= layout.ship_list.elements;

  if (!(worker->exec.scan_proj= thd->alloc<Item*>(n)))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) (n * sizeof(Item*)));
    return true;
  }

  uint k= 0;
  for (Field **f= wt->field; *f && k < n; f++)
  {
    if (!bitmap_is_set(wt->read_set, (*f)->field_index))
      continue;
    if (!(worker->exec.scan_proj[k]= new (thd->mem_root) Item_field(thd, *f)))
    {
      my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(Item_field));
      return true;
    }
    k++;
  }
  /*
    A query that reads no column at all still has a row shape -- build() ships
    a constant so that the transport has something to measure -- and there is
    no field to project into it. Nothing downstream reads that column's value,
    only the arrival of the record, so leave it as the container's default.
  */
  worker->exec.scan_proj_count= k;
  DBUG_ASSERT(k == n || k == 0);
  return false;
}


bool pwt_manager::setup_sort_stage(THD *thd)
{
  if (layout.make_container(thd, &sort_container, nullptr))
  {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "parallel query: could not build the sort container");
    return true;
  }
  return false;
}


/*
  @brief
    Collect the record just drained.

  @description
    The container has the layout's own shape, so the record is written as it
    arrived, with no projection: the columns are already the ones the sort and
    the copy-back name. A heap table that fills is rebuilt on disk, the same
    way a worker's container is, except that this one never leaves this thread
    and so needs none of the cross-thread accounting.

  @return  0 on success, 1 on error.
*/

int pwt_manager::sort_collect(THD *thd)
{
  TABLE *t= sort_container.table;
  memcpy(t->record[0], layout.recv_record(), layout.reclength);

  int err= t->file->ha_write_tmp_row(t->record[0]);
  if (likely(!err))
    return 0;
  if (err != HA_ERR_RECORD_FILE_FULL)
  {
    t->file->print_error(err, MYF(0));
    return 1;
  }
  /* Rebuilds on disk and writes the row that did not fit. */
  if (create_internal_tmp_table_from_heap(thd, t,
                                          sort_container.param->start_recinfo,
                                          &sort_container.param->recinfo,
                                          err, 0, NULL))
    return 1;                                    // already reported
  return 0;
}


/*
  @brief
    Sort what was collected and send it to the client.

  @description
    The plan's own Filesort cannot be reused: its order names the manager's
    base-table fields, and the rows are in the container. build_sort_order()
    gives the equivalent order over the container's own fields, and the sort
    runs there. Nothing filters -- the workers applied the condition -- so the
    Filesort is built without a select.

    Reading back is the ordinary way a sorted result is read, and each row then
    takes the path a drained row would have taken: back into the manager's
    base-table records, where this query's items read it, and out.

  @return  0 on success, 1 on error.
*/

int pwt_manager::sort_and_send(JOIN *join)
{
  DBUG_ENTER("pwt_manager::sort_and_send");
  THD *thd= join->thd;
  TABLE *t= sort_container.table;
  int ret= 0;

  ORDER *so= layout.build_sort_order(thd, t);
  if (!so)
    DBUG_RETURN(1);

  Filesort *fs= new (thd->mem_root) Filesort(so, HA_ROWS_MAX, false, nullptr);
  if (!fs)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(Filesort));
    DBUG_RETURN(1);
  }
  fs->accepted_rows= &join->accepted_rows;       // for ROWNUM

  /*
    The plan's tracker, so ANALYZE reports this sort as the plan's sort, which
    is what it is. A plan that was never asked to explain itself has none.
  */
  Filesort_tracker *tracker= exec.scan_tab->filesort
                             ? exec.scan_tab->filesort->tracker : nullptr;
  if (!tracker &&
      !(tracker= new (thd->mem_root) Filesort_tracker(thd->lex->analyze_stmt)))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(Filesort_tracker));
    DBUG_RETURN(1);
  }

  t->file->info(HA_STATUS_VARIABLE);             // filesort wants the count
  if (!(sort_result= filesort(thd, t, fs, tracker, join, 0)))
    DBUG_RETURN(1);
  t->file->ha_index_or_rnd_end();

  READ_RECORD info;
  if (init_read_record(&info, thd, t, nullptr, sort_result, 0, true, false))
    DBUG_RETURN(1);

  for (;;)
  {
    int err= info.read_record();
    if (err)
    {
      ret= err > 0;                              // < 0 is end of records
      break;
    }
    if (unlikely(thd->killed))
    {
      thd->send_kill_message();
      ret= 1;
      break;
    }

    memcpy(layout.recv_record(), t->record[0], layout.reclength);
    layout.copy_back_row();

    int serr= join->result->send_data_with_check(join->fields_list, join->unit,
                                                 join->send_records);
    if (unlikely(serr))
    {
      if (serr > 0)
      {
        ret= 1;
        break;
      }
      join->duplicate_rows++;                    // serr < 0: duplicate row
    }
    join->send_records++;
    join->accepted_rows++;
  }
  end_read_record(&info);
  DBUG_RETURN(ret);
}


int pwt_manager::drain_and_send(JOIN *join)
{
  DBUG_ENTER("pwt_manager::drain_and_send");
  uchar *dst= layout.recv_record();
  int ret= 0;

  if (layout.begin_receive(thd, exec.tables, exec.n_tables))
    DBUG_RETURN(1);

  if (layout.plan_aggregates)
  {
    // the plan aggregates, so last_tab is the worker row destination
    JOIN_TAB *last_tab= join->join_tab + join->top_join_tab_count - 1;

    for (;;)
    {
      int rc= source->next_row(dst);
      if (rc < 0)
        break;                                      // -1: all rows drained
      if (rc > 0)
      {
        ret= 1;                                     // killed / worker error
        break;
      }

      layout.copy_back_row();
      /*
        Hand the row to the plan's own terminal, which is end_update(): it
        knows which group the row belongs to.

        When the workers pre-aggregated, the row is one worker's partials for
        that group, so prime the query's aggregates with them first -- folding
        a partial in is exactly what end_update() then does, because the
        update_field() (or, for a group it has not seen, reset_field()) it
        calls per aggregate takes the direct value in place of the row.

        When they did not -- the fall-back out of pre-aggregation -- the row is
        a plain joined row, and the terminal counts it. That is the same
        grouping, done a row at a time instead of a group at a time.
      */
      if (layout.grouped)
        layout.direct_add_partials();
      enum_nested_loop_state nls=
                           (*last_tab->next_select) (join, last_tab + 1, FALSE);
      if (nls < NESTED_LOOP_OK)                   // ERROR or KILLED
      {
        ret= 1;
        break;
      }
    }
    /*
      End of records. For a grouped query this is the call that makes the
      aggregation stage read back what it accumulated and send it; there is
      nothing to do for a query whose rows went straight to the client.
    */
    if (!ret)
    {
      enum_nested_loop_state nls= (*last_tab->next_select)(join, last_tab + 1,
                                                          TRUE);
      if (nls < NESTED_LOOP_OK)
        ret= 1;
    }
    layout.end_receive(exec.tables, exec.n_tables);

    DBUG_PRINT("info", ("join records:%llu", join->send_records));
    DBUG_RETURN(ret);
  }

  for (;;)
  {
    int rc= source->next_row(dst);
    if (rc < 0)
      break;                                      // -1: all rows drained
    if (rc > 0)
    {
      ret= 1;                                     // killed / worker error
      break;
    }

    /*
      A plan whose sort is ours collects the row instead of sending it: the
      order it goes out in is not the order it arrived in, so nothing can be
      sent until every worker has finished. sort_and_send() does the rest.
    */
    if (layout.plan_sorts)
    {
      if (sort_collect(thd))
      {
        ret= 1;
        break;
      }
      continue;
    }

    /* Put the shipped columns back where the query expects to find them. */
    layout.copy_back_row();

    int err= join->result->send_data_with_check(join->fields_list, join->unit,
                                                 join->send_records);
    if (unlikely(err))
    {
      if (err > 0)
      {
        ret= 1;
        break;
      }
      join->duplicate_rows++;                     // err < 0: duplicate row
    }
    join->send_records++;
    join->accepted_rows++;
  }

  if (!ret && layout.plan_sorts)
    ret= sort_and_send(join);

  layout.end_receive(exec.tables, exec.n_tables);

  DBUG_PRINT("info", ("join records:%llu", join->send_records));
  DBUG_RETURN(ret);
}


/**
  @brief
    Whether the access method the plan settled on is one the engine will hand
    out in pieces.

  @description
    Asked of the driving table only, and asked about the *access method*: the
    table-level question -- a real base table, no blob-backed columns, not
    fulltext-searched, not partitioned, an engine that does this at all -- is
    table_can_be_parallel_scanned()'s, and the caller asks it first. Keeping
    the two apart matters because the optimizer's cost hook
    (scale_cost_for_parallel_scan) asks only the table-level one, and a copy
    here would be free to drift from it.
*/

bool is_parallel_scan_applicable(JOIN_TAB *join_tab)
{
  /*
    Two access methods are eligible: a table scan (EXPLAIN type=ALL) and a
    range scan over one index. A range scan reaches here as either JT_ALL or
    JT_RANGE, depending on how the plan arrived at it -- make_join_select()
    promotes a ref to JT_RANGE when the range uses a longer key -- and both
    read through join_init_read_record. read_first_record is checked next to
    ->type because this runs after make_join_readinfo(), which is what actually
    picked the reader.

    A complete scan of one index (type=index, JT_NEXT) is NOT eligible, even
    though InnoDB advertises PSCAN_INDEX_FULL. init_parallel_workers() already
    knows how to name the index for one, but nothing in the SQL layer has
    driven it yet: the plans that reach here as JT_NEXT are mostly covering
    reads, and a worker's handler is not given the plan's keyread, so taking
    one would throw away the reason the index was chosen. Until that is
    settled, this is the check that keeps the two apart.
  */
  if (!((join_tab->type == JT_ALL || join_tab->type == JT_RANGE) &&
        join_tab->read_first_record == join_init_read_record))
    return false;

  /*
    The plan may be relying on this table's rows arriving in index order to
    satisfy ORDER BY or GROUP BY without a filesort, but a parallel scan
    does not preserve that order. Reject parallelization in that case.
  */
  if (join_tab->join->ordered_index_usage != JOIN::ordered_index_void)
    return false;

  if (join_tab->filesort_result ||
      join_tab->need_to_build_rowid_filter || join_tab->rowid_filter ||
      join_tab->distinct)
    return false;

  /*
    A sort of this table's own read, added by make_aggr_tables_info() after the
    plan was otherwise settled. This one is not about the order rows arrive in
    -- a sort does not care -- it is about where the sort can happen. The
    filesort is bound to the read the workers take over, and there is no
    post-join stage to move it to: for these plans make_aggr_tables_info()
    builds no aggregation table, so the terminal after the driving tab is
    end_send() straight to the client. Until the manager can sort what it
    drains, the sort has nowhere to go and the plan has to run serially.

    Kept apart from the checks above because it is the one of them that a
    manager-side sort stage would lift, and because the trace should say which
    of the two reasons applied. pwt_table_conds() already knows where a sorted
    tab keeps its condition, so the worker half is in place.
  */
  if (join_tab->filesort && !pwt_manager_sort_order(join_tab->join))
    return false;

  const uint32 pscan_support= join_tab->table->file->parallel_scan_support();
  SQL_SELECT *sql_select= join_tab->select;

  if (sql_select && sql_select->quick)
  {
    /*
      The case of a range scan. Any single index will do, primary or
      secondary: the coordinator converts each endpoint against the index it
      was asked to divide, and the worker opens that same index. What is asked
      of the plan is that it be one plain range with few enough intervals to be
      worth splitting.

      QS_TYPE_RANGE and nothing else. An index merge or a ROR intersect
      combines several indexes and has no single interval list to hand over; a
      GROUP BY min/max scan visits one row per group rather than a range; and a
      descending read (QS_TYPE_RANGE_DESC) walks the intervals backwards, which
      the partitioner does not produce.

      Which of the two bits is asked for is decided by whether the index is the
      clustered one, because that is what the engine is really being asked to
      divide. The engine then decides whether it can partition this particular
      index -- see ha_innobase::pscan_resolve_index(), which declines spatial,
      FTS, virtual-column and descending indexes -- and we fall back to the
      serial reader on HA_ERR_UNSUPPORTED.
    */
    const uint MAX_PARALLEL_SCAN_RANGES= 128;
    if (sql_select->quick->get_type() == QUICK_SELECT_I::QS_TYPE_RANGE &&
        sql_select->quick->index < join_tab->table->s->keys &&
        join_tab->use_quick != 2 /*exclude dynamic range*/ &&
        ((QUICK_RANGE_SELECT*) sql_select->quick)->num_ranges() <=
          MAX_PARALLEL_SCAN_RANGES)
    {
      /*
        A rowid-ordered scan collects the row ids, sorts them and then sweeps
        the clustered index in that order. Taking such a plan would quietly
        throw the sorted sweep away. Leave it serial.
      */
      if (((QUICK_RANGE_SELECT*) sql_select->quick)->mrr_flags &
          DSMRR_IMPL_SORT_ROWIDS)
        return false;

      if (sql_select->quick->index == join_tab->table->s->primary_key)
        return (pscan_support & handler::PSCAN_TABLE_RANGE) != 0;
      else
        return (pscan_support & handler::PSCAN_INDEX_RANGE) != 0;
    }
    else
    {
      return false;
    }
  }
  else
  {
    return (pscan_support & handler::PSCAN_TABLE_FULL) != 0;
  }
}


/*
  @brief
    The GROUP BY key the plan's own aggregation table is built on, if the
    terminal that reads it is the one that looks a group up by that key.

  @description
    end_update() reads a row, builds the group key from table->group and folds
    the row into whichever group it finds -- the one terminal whose answer does
    not depend on the order rows arrive in, which is what lets the driving scan
    be handed out in chunks at all. It is also the one a partial can be merged
    through, because the update_field() it calls per aggregate consumes a
    direct value in place of the row.

    The key is taken from the aggregation table rather than from
    join->group_list, and not only because that is the one end_update() will
    actually key on: make_aggr_tables_info() sets join->group_list to NULL once
    the temp table has taken the grouping over, so by this point it is gone.

    Lives here because end_update() is static to this file.

  @return the key, or nullptr if this plan's terminal is not that one.

    TODO: shift this out by whatever means is reqd.
      rewrite the above into something less convoluted
*/

ORDER *pwt_plan_group_key(JOIN *join)
{
  if (!join->aggr_tables)
    return nullptr;
  JOIN_TAB *last= join->join_tab + join->top_join_tab_count - 1;
  if (last->next_select != sub_select_postjoin_aggr)
    return nullptr;
  JOIN_TAB *aggr_tab= last + 1;
  if (!aggr_tab->aggr || aggr_tab->aggr->get_write_func() != end_update ||
      !aggr_tab->table)
    return nullptr;
  return aggr_tab->table->group;
}


/**
  @brief
  Run this query's driving-table scan in parallel workers if possible.

  @description
  Test if
  1) worker threads are available;
  2) the table itself is one a worker can read and ship
     (table_can_be_parallel_scanned);
  3) the access method the plan settled on is one the engine will divide
     (is_parallel_scan_applicable), which is also where the engine's
     parallel_scan_support() bitmap is matched against it;
  4) the query itself is one the workers can run end to end
     (can_run_query_in_workers) -- a streaming select-project[-join] whose
     every expression can be cloned onto a worker's own table copies.

  Called late in optimize_stage2(), everything earlier can change the
  first table's access method. A table given an ordered
  index scan after the fact is not one that can be handed out in chunks.

  Engine-intrinsic constraints (consistent-read only, record format,
  discarded tablespace, ...) are not known here. They are enforced later
  inside parallel_init_coordinator(), which declines with HA_ERR_UNSUPPORTED
  so run_worker_side_join() falls back to serial execution. The table keeps
  the serial reader either way: the manager never scans it, it only collects
  the workers' result rows, and the serial reader is what the fall-back path
  needs. do_select() dispatches on worker_side_parallel.

  (Un)Sets join->worker_side_parallel
*/

void parallel_join_check(JOIN *join)
{
  JOIN_TAB *first= first_linear_tab(join, WITH_BUSH_ROOTS,
                                    WITHOUT_CONST_TABLES);
  if (join->thd->variables.parallel_worker_threads > 0 &&             //1
      first && table_can_be_parallel_scanned(first->table) &&   //2
      is_parallel_scan_applicable(first) &&                     //3
      can_run_query_in_workers(join, first))                    //4
  {
    first->use_parallel_scan= join->worker_side_parallel= true;
    if (unlikely(join->thd->trace_started()))
    {
      Json_writer_object trace_pscan(join->thd);
      trace_pscan.add("chosen_for_parallel_scan",
                      first->table->alias.c_ptr());
      /*
        What the workers will divide: the whole clustered index, or the key
        intervals of the range scan. The two are partitioned the same way,
        but which one it is decides whether the chunk boundaries are bounded
        by the range, so say so rather than leaving it to be inferred from
        the access method shown elsewhere in the trace.
      */
      trace_pscan.add("range_scan",
                      first->select && first->select->quick != NULL);
    }
  }
  else
  {
    join->worker_side_parallel= false;
    /*
      And take back what check_parallel_scan() marked in make_join_readinfo(),
      which is what EXPLAIN reads to print ALL_parallel / range_parallel. It
      runs before the plan is finished and this is the decision that stands, so
      leaving the mark would have EXPLAIN name a parallel scan for a query that
      then runs serially. recheck_parallel_scan() clears both for the same
      reason.
    */
    if (first)
      first->use_parallel_scan= false;
    /*
      Only when the workers were available to begin with. With
      parallel_worker_threads at 0 every query in the server declines, and
      saying so in every trace would tell the reader nothing they did not set
      themselves.
    */
    if (unlikely(join->thd->trace_started()) &&
        join->thd->variables.parallel_worker_threads > 0)
    {
      Json_writer_object trace_pscan(join->thd);
      trace_pscan.add("parallel_scan_declined", first ? first->table->alias.c_ptr()
                                                      : "");
      /*
        Named in the order they are worth telling apart rather than the order
        they are tested. The sort comes first because it is the one a later
        commit is meant to lift, and because it arrives last:
        make_aggr_tables_info() adds it after everything else about the plan
        has been decided, so a plan can pass every other test here and still
        be turned away by it. The two orderings after it are the opposite
        case -- the plan wants the rows in an order chunked delivery cannot
        produce -- and no manager-side stage will change that.

        Anything else is one of many, and the trace would have to name a check
        rather than a reason; the plan itself is in the trace for that.
      */
      trace_pscan.add("parallel_scan_declined_because",
                      !first
                      ? "there is no table to divide" :
                      first->filesort && !pwt_manager_sort_order(join)
                      ? "the manager cannot sort what the workers produce" :
                      join->ordered_index_usage == JOIN::ordered_index_order_by
                      ? "an index supplies the ORDER BY order" :
                      join->ordered_index_usage == JOIN::ordered_index_group_by
                      ? "an index supplies the GROUP BY order"
                      : "the query is not one the workers can run");
    }
  }
}
