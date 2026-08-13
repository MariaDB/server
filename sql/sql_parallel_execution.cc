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
#include "sql_parallel_workers.h"
#include "transaction.h"


/*****************************************************************************
  The gate: which queries the workers may run.
  All of this runs on the user's thread at optimize time; a query that fails
  any of it simply runs serially.
*****************************************************************************/


/*
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


/*
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


/*
  @brief
    Whether the access method on 'tab' is one the workers can partition.

  @description
    A plain full scan always is: the engine divides the whole clustered index.
    A range scan can be, because the engine partitions each key interval the
    same way it partitions the whole index (parallel_init_coordinator() takes
    the intervals and calls add_scan() once per interval), and the worker's
    chunk boundaries are then clustered-index keys inside that interval.

    Which intervals it will accept is decided here, because what it does with
    them is not visible from the interval alone. It converts each endpoint
    against the *clustered* index -- pscan_convert_key() builds the tuple from
    table->s->primary_key -- so an interval over any other index would be read
    as a primary-key interval and silently scan the wrong part of the table.
    Hence: a real primary key, and the quick select reading through it.

    QS_TYPE_RANGE and nothing else. An index merge or a ROR intersect combines
    several indexes and has no single interval list to hand over; a GROUP BY
    min/max scan visits one row per group rather than a range; and a
    descending read (QS_TYPE_RANGE_DESC) walks the intervals backwards, which
    the partitioner does not produce -- it assumes ascending physical key
    order, as ha_innobase::parallel_init_coordinator() says when it declines a
    descending key column.

  @return  true if the workers can scan what this access method reads.
*/
bool parallel_scan_supports_access(JOIN_TAB *tab)
{
  if (!tab->select || !tab->select->quick)
    return true;                                 // full scan: nothing to map

  QUICK_SELECT_I *quick= tab->select->quick;
  return quick->get_type() == QUICK_SELECT_I::QS_TYPE_RANGE &&
         tab->table->s->primary_key < MAX_KEY &&
         quick->index == tab->table->s->primary_key;
}


/*
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

    Both halves are reported together, and the gate and both clone sites go
    through here, so the condition the gate approves is always the condition a
    worker ends up evaluating.
*/

static void pwt_table_conds(JOIN_TAB *tab, Item **cond, Item **cache_cond)
{
  *cond= tab->pre_idx_push_select_cond ? tab->pre_idx_push_select_cond
                                       : tab->select_cond;
  *cache_cond= tab->cache_select ? tab->cache_select->cond : nullptr;
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
    - at least one non-const table (the parallel-scanned driving table);
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
bool can_run_query_in_workers(JOIN *join, JOIN_TAB *scan_tab)
{
  DBUG_ENTER("can_run_query_in_workers");
  SELECT_LEX *sl= join->select_lex;

  if (join->table_count - join->const_tables < 1)   // need the driving table
  {
    DBUG_PRINT("info", ("only constant tables"));
    DBUG_RETURN(false);
  }
  if (join->need_tmp)                             // group/distinct/order/...
  {
    DBUG_PRINT("info", ("group/distinct/order by"));
    DBUG_RETURN(false);
  }
  if (sl->limit_params.explicit_limit)            // LIMIT / OFFSET
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
  if (sl->agg_func_used() || sl->with_sum_func )
  {
    DBUG_PRINT("info", ("aggregate funcs"));
    DBUG_RETURN(false);
  }

  if (join->group_list || join->group)
  {
    DBUG_PRINT("info", ("group"));
    DBUG_RETURN(false);
  }
  if (join->select_distinct)
  {
    DBUG_PRINT("info", ("distinct"));
    DBUG_RETURN(false);
  }
  if (join->order)
  {
    DBUG_PRINT("info", ("order by"));
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
      The driving table may be read through a quick select, the tables joined
      after it may not.
      worker_join_inner() looks a row up by ref or scans, and has no quick
      select of its own to read through.
    */
    if (tab->select && tab->select->quick &&
        (j > join->const_tables || !parallel_scan_supports_access(tab)))
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


/**
  @brief
    Create + instantiate one result container in result_defn's column layout.

  Only the record buffer and the fields are ever used (the worker projects into
  result_table->record[0] and ships its image; the manager receives images and
  sends from it) -- no rows are written through the storage engine.

  @return
    true     error
    false    success (*out set).
*/
bool pwt_manager::make_result_table(THD *thd, List<Item> &defn, TABLE **out)
{
  /*
    Start from a freshly counted param every time. create_tmp_table() overwrites
    param->func_count with the number of items it actually has to copy, so a
    second table built from the same param can allocate fewer fields than the
    layout needs, which is what the assertion in Create_tmp_table::finalize()
    catches. The manager and every worker build this same layout, so each of
    them re-counts.
  */
  transport.result_tmp_param->init();
  count_field_types(exec.join->select_lex, transport.result_tmp_param, defn,
                    false);
  transport.result_tmp_param->skip_create_table= true;

  /*
    TMP_TABLE_ALL_COLUMNS makes create_tmp_table() give every item of the list a
    field, including the constant ones. By default it skips constants, which is
    right for a query that materialises its result and can evaluate them once
    outside the table, but wrong here: the layout has to mirror the select list
    one for one, because the worker projects item i into field i and ships the
    record image, and the manager sends one Item_field per field to the client.
    Without this a select list holding a constant, "SELECT 42, a FROM t1", built
    a table with fewer fields than the projection walks over.
  */
  const ulonglong opts= exec.join->select_options | TMP_TABLE_ALL_COLUMNS;
  TABLE *t= create_tmp_table(thd, transport.result_tmp_param, defn,
                             nullptr, false, false,
                             opts, HA_POS_ERROR,
                             &empty_clex_str, true, false);
  if (!t)
    return true;
  if (instantiate_tmp_table(t, transport.result_tmp_param->keyinfo,
                            transport.result_tmp_param->start_recinfo,
                            &transport.result_tmp_param->recinfo,
                            opts, true /*cross_thread*/))
  {
    free_tmp_table(thd, t);
    return true;
  }
  /*
    The whole transport is positional, so a layout that does not match the
    select list item for item would have the worker and the manager reading
    different columns, or walking past the end of the field array. Refuse
    instead, whatever the reason turns out to be.
  */
  DBUG_ASSERT(t->s->fields == defn.elements);
  if (t->s->fields != defn.elements)
  {
    free_tmp_table(thd, t);
    return true;
  }
  *out= t;

  return false;
}


/**
  @brief  Free the manager and per-worker result containers.
*/
void pwt_manager::free_result_tables(THD *thd)
{
  if (workers)
    for (uint i= 0; i < nworkers; i++)
      if (workers[i].exec.result_table)
      {
        free_tmp_table(thd, workers[i].exec.result_table);
        workers[i].exec.result_table= nullptr;
      }
  if (transport.result_table)
  {
    free_tmp_table(thd, transport.result_table);
    transport.result_table= nullptr;
  }
}


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
#ifndef DBUG_OFF
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
    worker never reads through it: setup_worker_tabs() clears the copy's
    'select', and the worker filters by the cloned condition instead.
  */
  DBUG_ASSERT(!tab->rowid_filter);
  DBUG_ASSERT(is_driving || !tab->select || !tab->select->quick);

  /* Materialized derived tables, CTEs and split materialization. */
  DBUG_ASSERT(!tab->split_derived_to_update);

  /* DISTINCT and HAVING are refused, so neither shortcut applies. */
  DBUG_ASSERT(!tab->shortcut_for_distinct);
  DBUG_ASSERT(!tab->having);

  /* The driving table is scanned; only the tables after it are looked up. */
  DBUG_ASSERT(is_driving || tab->type == JT_ALL || tab->type == JT_REF ||
              tab->type == JT_EQ_REF);
#else
  (void) tab; (void) is_driving;
#endif
}


/*
  Every JOIN field the worker's executor could reach that the gate is supposed to
  have made inert. Same purpose as pwt_assert_tab_inert(), one level up: relaxing
  a gate without teaching setup_worker_join() about the field it lets through
  fails here rather than running with a field this worker's JOIN does not carry.
*/
static void pwt_assert_join_inert(JOIN *join)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(!join->outer_join);
  DBUG_ASSERT(!join->need_tmp);
  DBUG_ASSERT(!join->select_distinct);
  DBUG_ASSERT(!join->group && !join->group_list);
  DBUG_ASSERT(!join->order);
  DBUG_ASSERT(!join->having && !join->tmp_having);
  DBUG_ASSERT(!join->procedure);
  DBUG_ASSERT(!join->sort_and_group);
  DBUG_ASSERT(!join->group_optimized_away);
  DBUG_ASSERT(!(join->select_options & OPTION_FOUND_ROWS));
#else
  (void) join;
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
  */
#ifndef DBUG_OFF
  static_assert(sizeof(JOIN) == 1552,
                "JOIN has changed shape: review what setup_worker_join() "
                "carries over to a worker");
#endif

  pwt_assert_join_inert(exec.join);

  if (!(worker->exec.join= new (thd->mem_root)
          JOIN(worker->thd, exec.join->fields_list, exec.join->select_options,
               nullptr)))
    return true;

  worker->exec.join->table_count= exec.join->table_count;
  worker->exec.join->const_tables= exec.join->const_tables;
  worker->exec.join->select_lex= exec.join->select_lex;
  return false;
}


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
bool pwt_manager::setup_worker_tabs(THD *thd, pwt_worker *worker)
{
  if (!(worker->exec.jointabs= thd->alloc<JOIN_TAB>(exec.n_tables)))
    return true;

  for (uint k= 0; k < exec.n_tables; k++)
  {
    JOIN_TAB *mtab= exec.jointabs[k];
    JOIN_TAB *wtab= &worker->exec.jointabs[k];

    pwt_assert_tab_inert(mtab, k == 0);
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

    /* No join buffer in a worker: it joins a row at a time. */
    wtab->cache= nullptr;
    wtab->use_join_cache= FALSE;
    wtab->jbuf_tracker= nullptr;

    /* ANALYZE reads these back off the manager, see quiesce_workers(). */
    wtab->tracker= &worker->exec.tab_stats[k];

    wtab->join= worker->exec.join;

    /*
      The record sources the tabs are driven through are the next step of the
      executor split. Nulled so that reaching them before then is a crash and
      not a read of the manager's.
    */
    wtab->next_select= nullptr;
    wtab->read_first_record= nullptr;
    bzero((char*) &wtab->read_record, sizeof(wtab->read_record));

    if (k && (wtab->type == JT_EQ_REF || wtab->type == JT_REF))
    {
      if (clone_table_ref(thd, &mtab->ref, wtab->table,
                          exec.tables, worker->exec.tables, exec.n_tables,
                          &wtab->ref))
        return true;
    }
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

  // shipped columns -> per-item projection into result_table->field[i]
  worker->exec.proj_count= transport.ship_list.elements;
  worker->exec.proj=
    (Item**) thd->alloc(worker->exec.proj_count * sizeof(Item*));
  if (!worker->exec.proj)
    return true;

  List_iterator_fast<Item> li(transport.ship_list);
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

int pwt_worker::emit_row(uint level)
{
  DBUG_ENTER("pwt_worker::emit_row");
  if (level + 1 < exec.n_tables)
    DBUG_RETURN(-1);
  /*
    Evaluate each cloned select-list item (which reads from the worker's table
    copies, now holding the current matching row of every table) and store it
    into the matching exec.result_table field. Evaluation errors are
    intercepted by PWT_error_handler, which trips fatal_error; the caller
    checks that.
  */
  for (uint i= 0; i < exec.proj_count; i++)
    exec.proj[i]->save_in_field(exec.result_table->field[i], false);

  if (manager->fatal_error)               // projection raised an error
    DBUG_RETURN(1);

  memcpy(batch.rows + (size_t) batch.count * manager->drain.reclength,
         exec.result_table->record[0], manager->drain.reclength);
  if (++batch.count == PWT_CHUNK_ROWS)
  {
    if (manager->handoff_batch(this))             // manager asked us to stop
      DBUG_RETURN(1);
    batch.count= 0;                               // buffer drained; refill
  }
  DBUG_RETURN(0);
}


/**
  @brief
    Join the non-driving tables, levels [level .. exec.n_tables-2], against
    the rows of the earlier tables already in their record[0], and emit each
    full match.

  Inner equi-join only (the gate excludes outer joins and semijoins): for a
  REF/EQ_REF table the key is built from the earlier tables (cp_buffer_from_ref)
  and looked up in the worker's private index; for a JT_ALL table the worker's
  copy is rnd-scanned. Each table's cloned condition is applied as we descend,
  which is where the optimizer left the multi-table predicates, so together
  they reconstitute the whole WHERE.

  @return
    0 continue
    1 stop (manager stop or fatal error).
*/

int pwt_worker::join_inner(uint level)
{
  DBUG_ENTER("pwt_worker::join_inner");
  int ret= emit_row(level);
  if (ret >= 0)
    DBUG_RETURN(ret);

  JOIN_TAB *it= &exec.jointabs[level + 1];
  TABLE *t= it->table;
  int err;
  Table_access_tracker *tr= it->tracker;
  tr->r_scans++;                        // sub_select() counts one per probe too

  if (it->type == JT_ALL)
  {
    if ((err= t->file->ha_rnd_init(true)))
    {
      manager->fatal_error= true;
      t->file->print_error(err, MYF(0));
      DBUG_RETURN(1);
    }
    while (!(err= t->file->ha_rnd_next(t->record[0])))
    {
      tr->r_rows++;
      if (it->select_cond)
      {
        bool pass= it->select_cond->val_bool();
        if (manager->fatal_error)
        {
          t->file->ha_rnd_end();
          DBUG_RETURN(1);
        }
        if (!pass)
          continue;
      }
      tr->r_rows_after_where++;
      if (join_inner(level+1))
      {
        t->file->ha_rnd_end();
        DBUG_RETURN(1);
      }
    }
    t->file->ha_rnd_end();
    if (err != HA_ERR_END_OF_FILE)
    {
      manager->fatal_error= true;
      t->file->print_error(err, MYF(0));
      DBUG_RETURN(1);
    }
    DBUG_RETURN(0);
  }

  // REF / EQ_REF: build the lookup key from the earlier tables and probe.
  // cp_buffer_from_ref returns true if a null-rejecting key part is NULL,
  // i.e. there can be no match -> just backtrack.
  if (cp_buffer_from_ref(thd, t, &it->ref))
    DBUG_RETURN(0);
  err= t->file->ha_index_read_map(t->record[0], it->ref.key_buff,
                                  make_prev_keypart_map(it->ref.key_parts),
                                  HA_READ_KEY_EXACT);
  while (!err)
  {
    tr->r_rows++;
    bool pass= !it->select_cond || it->select_cond->val_bool();
    if (manager->fatal_error)
      DBUG_RETURN(1);
    if (pass)
      tr->r_rows_after_where++;
    if (pass && join_inner(level + 1))
      DBUG_RETURN(1);
    if (it->type == JT_EQ_REF)
      break;                                     // unique key: at most one match
    err= t->file->ha_index_next_same(t->record[0], it->ref.key_buff,
                                     it->ref.key_length);
  }
  if (err && err != HA_ERR_KEY_NOT_FOUND && err != HA_ERR_END_OF_FILE)
  {
    manager->fatal_error= true;
    t->file->print_error(err, MYF(0));
    DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


/**
  @brief
    Run the query over the worker's private chunk of the driving table, joining
    the other tables, and stream the *result* rows to the manager a batch at a
    time through the worker's reused row buffer.

  The worker scans its own copy of the driving table (exec.scan_table, opened
  with in_use == this worker's thd) so the workers scan concurrently with no
  shared-scan lock. For each driving row that passes the pushed WHERE it joins
  the remaining tables (join_inner) and, for every full match, projects
  the select list into exec.result_table and ships that record image. The
  manager (drain_and_send) only concatenates these final rows.

  @return
  0 on success, or a handler error code. A clean stop requested by the manager
  (handoff_batch -> stop) also returns success: the manager is done, not in
  error.
*/

int pwt_worker::execute_and_handoff()
{
  DBUG_ENTER("pwt_worker::execute_and_handoff");
  TABLE *src= exec.scan_table;
  pwt_manager *mgr= manager;
  const uint nt= exec.n_tables;
  int err= 0;
  uint i;

  // the source tables were marked in open_worker_tables; this one is ours
  exec.result_table->use_all_columns();         // we write every result column

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

  if (!err && !(err= src->file->parallel_init_worker(exec.handler_ctx)))
  {
    batch.count= 0;
    bool eof= false, killed= false;
    while (!eof && !killed)
    {
      // honour a direct KILL of this worker's thread
      mysql_mutex_lock(&thd->LOCK_thd_kill);
      killed= thd->killed;
      mysql_mutex_unlock(&thd->LOCK_thd_kill);
      if (killed)
      {
        my_error(ER_QUERY_INTERRUPTED, MYF(0));
        break;
      }

      if ((err= src->file->ha_parallel_get_next_row(exec.handler_ctx)))
      {
        if (err == HA_ERR_END_OF_FILE)
        {
          err= 0;
          eof= true;
        }
        break;
      }

      exec.tab_stats[0].r_rows++;              // what ANALYZE calls r_rows

      // apply per worker select_cond
      if (Item *scan_cond= exec.jointabs[0].select_cond)
      {
        bool pass= scan_cond->val_bool();
        if (mgr->fatal_error)
        {
          eof= true;
          break;
        }
        if (!pass)         // skip below
          continue;
      }
      exec.tab_stats[0].r_rows_after_where++;

      // join the rest of the tables and emit each full match
      if (join_inner(0))
      {
        eof= true;
        break;
      }
    }
    src->file->parallel_end_worker();

    // hand off the final partial batch (ignore a late stop -- we are done)
    if (!err && !killed && !mgr->fatal_error && batch.count)
      mgr->handoff_batch(this);
  }
  else if (err == HA_ERR_END_OF_FILE)
    err= 0;

  // end any open index/rnd scans (no-op for tables left in NONE state), unlock
  for (i= 1; i < nt; i++)
    exec.jointabs[i].table->file->ha_index_or_rnd_end();
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

  pwt_manager *mgr= manager;
  int err= execute_and_handoff();

  /*
    End the worker's read transaction now, while we are still on the worker
    thread. destroy_background_thd() -> THD::cleanup() rolls the transaction
    back and asserts (trans_check) that the statement transaction is already
    empty, so we must close it out here. Any commit failure is captured by the
    installed PWT_error_handler.
  */
  trans_commit_stmt(thd);
  trans_commit(thd);

  /*
    Mark this producer done so the consumer can detect EOF, and wake it in
    case it is blocked waiting for data. A real engine error trips fatal_error
    so the consumer aborts the join instead of returning a truncated result.
    If this worker was killed (e.g. a user KILL aimed at it), record the kill
    so the consumer can propagate it to the manager's THD and abort the join
    with ER_QUERY_INTERRUPTED before any result is sent.
  */
  mysql_mutex_lock(&thd->LOCK_thd_kill);
  killed_state killed= thd->killed;
  mysql_mutex_unlock(&thd->LOCK_thd_kill);

  mysql_mutex_lock(&mgr->LOCK_data);
  if (err)
    mgr->fatal_error= true;
  if (killed && mgr->kill_signal == NOT_KILLED)
    mgr->kill_signal= killed;
  mgr->drain.active_workers--;
  mysql_cond_broadcast(&mgr->COND_data_avail);
  mysql_mutex_unlock(&mgr->LOCK_data);

  if (err)
    exec.scan_table->file->print_error(err, MYF(0));

  DBUG_VOID_RETURN;
}


/*
  @brief
    Drain the workers' result rows and send them to the client.

  @description
  The workers ran the whole select-project query over their disjoint chunks
  and produced the final result rows; the manager just concatenates them (in
  arrival order) and sends each to the client. The select-list metadata was
  already sent (from the query's own field list) before do_select() ran, so we
  only supply the row values here -- the select list is evaluated against the
  manager's own records, which the drain has filled from the shipped columns.
  send_records is advanced so do_select() can set limit_found_rows.

  @return
    0 success (all rows sent)
    1 error.
*/
int pwt_manager::drain_and_send(JOIN *join)
{
  DBUG_ENTER("pwt_manager::drain_and_send");
  uchar *dst= transport.result_table->record[0];
  int ret= 0;

  /*
    The manager's tables were opened but never read, so clear the flags a reader
    would have left. Copy_field captures &table->null_row, and a stale null_row
    would make every copied field read as NULL.

    Writing into these records is not something a SELECT's write_set allows, and
    Field::store() asserts on that, so mark the fields writable for the drain. A
    record is being filled here in place of the reader that would normally have
    filled it, which is what the helper is for.
  */
  MY_BITMAP **saved_write_set= (MY_BITMAP**)
                                thd->alloc(exec.n_tables * sizeof(MY_BITMAP*));
  if (!saved_write_set)
    DBUG_RETURN(1);
  for (uint t= 0; t < exec.n_tables; t++)
  {
    exec.tables[t]->status= 0;
    exec.tables[t]->null_row= false;
    saved_write_set[t]= dbug_tmp_use_all_columns(exec.tables[t],
                                                 &exec.tables[t]->write_set);
  }

  for (;;)
  {
    int rc= drain_next_row(dst);
    if (rc < 0)
      break;                                      // -1: all rows drained
    if (rc > 0)
    {
      ret= 1;                                     // killed / worker error
      break;
    }

    /* Put the shipped columns back where the query expects to find them. */
    for (uint i= 0; i < transport.n_copy_back; i++)
      (*transport.copy_back[i].do_copy)(&transport.copy_back[i]);

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

  for (uint t= 0; t < exec.n_tables; t++)
    dbug_tmp_restore_column_map(&exec.tables[t]->write_set, saved_write_set[t]);

  DBUG_PRINT("info", ("join records:%llu", join->send_records));
  DBUG_RETURN(ret);
}
