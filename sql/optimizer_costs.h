#ifndef OPTIMIZER_COSTS_INCLUDED
#define OPTIMIZER_COSTS_INCLUDED
/*
   Copyright (c) 2022, MariaDB AB

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

/*
  This file defines costs structures and cost functions used by the optimizer
*/


/*
  OPTIMIZER_COSTS stores cost variables for each engine. They are stored
  in linked_optimizer_costs (pointed to by handlerton) and TABLE_SHARE.
*/

#define OPTIMIZER_COST_UNDEF -1.0
struct OPTIMIZER_COSTS
{
  double disk_read_cost;
  double index_block_copy_cost;
  double key_cmp_cost;
  double key_copy_cost;
  double key_lookup_cost;
  double key_next_find_cost;
  double disk_read_ratio;
  double row_copy_cost;
  double row_lookup_cost;
  double row_next_find_cost;
  double rowid_cmp_cost;
  double rowid_copy_cost;
  double initialized;   // Set if default or connected with handlerton
};

/* Default optimizer costs */
extern OPTIMIZER_COSTS default_optimizer_costs;
/*
  These are used to avoid taking mutex while creating tmp tables
  These are created once after the server is started so they are
  not dynamic.
*/
extern OPTIMIZER_COSTS heap_optimizer_costs, tmp_table_optimizer_costs;

/*
  Interface to the engine cost variables. See optimizer_defaults.h for
  the default values.
*/

#define DISK_READ_RATIO        costs->disk_read_ratio
#define KEY_LOOKUP_COST        costs->key_lookup_cost
#define ROW_LOOKUP_COST        costs->row_lookup_cost
#define INDEX_BLOCK_COPY_COST  costs->index_block_copy_cost
#define KEY_COPY_COST          costs->key_copy_cost
#define ROW_COPY_COST          costs->row_copy_cost
#define ROW_COPY_COST_THD(THD) default_optimizer_costs.row_copy_cost
#define KEY_NEXT_FIND_COST     costs->key_next_find_cost
#define ROW_NEXT_FIND_COST     costs->row_next_find_cost
#define KEY_COMPARE_COST       costs->key_cmp_cost
#define SORT_INDEX_CMP_COST    default_optimizer_costs.key_cmp_cost
#define DISK_READ_COST         costs->disk_read_cost
#define DISK_READ_COST_THD(thd) default_optimizer_costs.disk_read_cost

/* Cost of comparing two rowids. This is set relative to KEY_COMPARE_COST */
#define ROWID_COMPARE_COST          costs->rowid_cmp_cost
#define ROWID_COMPARE_COST_THD(THD) default_optimizer_costs.rowid_cmp_cost

/* Cost of comparing two rowids. This is set relative to KEY_COPY_COST */
#define ROWID_COPY_COST             costs->rowid_copy_cost

/* Engine unrelated costs. Stored in THD so that the user can change them */
#define WHERE_COST              optimizer_where_cost
#define WHERE_COST_THD(THD)     ((THD)->variables.optimizer_where_cost)
#define TABLE_SCAN_SETUP_COST   optimizer_scan_setup_cost
#define TABLE_SCAN_SETUP_COST_THD(THD) (THD)->variables.optimizer_scan_setup_cost
#define INDEX_SCAN_SETUP_COST   optimizer_scan_setup_cost/2
/* Cost for doing duplicate removal in test_quick_select */
#define DUPLICATE_REMOVAL_COST  default_optimizer_costs.key_copy_cost

/* Default fill factors of an (b-tree) index block is assumed to be 0.75 */
#define INDEX_BLOCK_FILL_FACTOR_DIV 3
#define INDEX_BLOCK_FILL_FACTOR_MUL 4

/*
   These constants impact the cost of QSORT and priority queue sorting,
   scaling the "n * log(n)" operations cost proportionally.
   These factors are < 1.0 to scale down the sorting cost to be comparable
   to 'read a row' = 1.0, (or 0.55 with default caching).
   A factor of 0.1 makes the cost of get_pq_sort_cost(10, 10, false) =0.52
   (Reading 10 rows into a priority queue of 10 elements).

   One consenquence if this factor is too high is that priority_queue will
   not use addon fields (to solve the sort without having to do an extra
   re-read of rows) even if the number of LIMIT is low.
*/
#define QSORT_SORT_SLOWNESS_CORRECTION_FACTOR    (0.1)
#define PQ_SORT_SLOWNESS_CORRECTION_FACTOR       (0.1)

/*
  Creating a record from the join cache is faster than getting a row from
  the engine. JOIN_CACHE_ROW_COPY_COST_FACTOR is the factor used to
  take this into account. This is multiplied with ROW_COPY_COST.
*/
#define JOIN_CACHE_ROW_COPY_COST_FACTOR(thd) 1.0

/*
  cost1 is better that cost2 only if cost1 + COST_EPS < cost2
  The main purpose of this is to ensure we use the first index or plan
  when there are identical plans. Without COST_EPS some plans in the
  test suite would vary depending on floating point calculations done
  in different paths.
*/
#define COST_EPS  0.0000001

#define COST_MAX (DBL_MAX * (1.0 - DBL_EPSILON))

static inline double COST_ADD(double c, double d)
{
  DBUG_ASSERT(c >= 0);
  DBUG_ASSERT(d >= 0);
  return (COST_MAX - (d) > (c) ? (c) + (d) : COST_MAX);
}

static inline double COST_MULT(double c, double f)
{
  DBUG_ASSERT(c >= 0);
  DBUG_ASSERT(f >= 0);
  return (COST_MAX / (f) > (c) ? (c) * (f) : COST_MAX);
}

OPTIMIZER_COSTS *get_optimizer_costs(const LEX_CSTRING *cache_name);
OPTIMIZER_COSTS *create_optimizer_costs(const char *name, size_t length);
OPTIMIZER_COSTS *get_or_create_optimizer_costs(const char *name,
                                               size_t length);
bool create_default_optimizer_costs();
void copy_tmptable_optimizer_costs();
void free_all_optimizer_costs();
struct TABLE;

extern "C"
{
  typedef int (*process_optimizer_costs_t) (const LEX_CSTRING *,
                                            const OPTIMIZER_COSTS *,
                                            TABLE *);
  bool process_optimizer_costs(process_optimizer_costs_t func, TABLE *param);
}


#endif /* OPTIMIZER_COSTS_INCLUDED */
