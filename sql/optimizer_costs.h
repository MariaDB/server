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

/* This file includes costs variables used by the optimizer */

/*
  The table/index cache hit ratio in %. 0 means that a searched for key or row
  will never be in the cache while 100 means it always in the cache.

  According to folklore, one need at least 80 % hit rate in the cache for
  MariaDB to run very well. We set CACHE_HIT_RATIO to a bit smaller
  as there is still a cost involved in finding the row in the B tree, hash
  or other seek structure.

  Increasing CACHE_HIT_RATIO will make MariaDB prefer key lookups over
  table scans as the impact of ROW_COPY_COST and INDEX_COPY cost will
  have a larger impact when more rows are exmined..

  Note that avg_io_cost() is multipled with this constant!
*/
#define DEFAULT_CACHE_HIT_RATIO 50

/* Convert ratio to cost */

static inline double cache_hit_ratio(uint ratio)
{
  return (((double) (100 - ratio)) / 100.0);
}

/*
  Base cost for finding keys and rows from the engine is 1.0
  All other costs should be proportional to these
*/

/* Cost for finding the first key in a key scan */
#define KEY_LOOKUP_COST      ((double) 1.0)
/* Cost of finding a key from a row_ID (not used for clustered keys) */
#define ROW_LOOKUP_COST      ((double) 1.0)

/*
  Cost of finding and copying keys from the storage engine index cache to
  an internal cache as part of an index scan.
  Used in handler::keyread_time()
*/
#define DEFAULT_INDEX_BLOCK_COPY_COST  ((double) 1 / 5.0)
#define INDEX_BLOCK_COPY_COST(THD) ((THD)->variables.optimizer_index_block_copy_cost)

/*
  Cost of finding the next row during table scan and copying it to
  'table->record'.
  If this is too small, then table scans will be prefered over 'ref'
  as with table scans there are no key read (KEY_LOOKUP_COST), fewer
  disk reads but more record copying and row comparisions.  If it's
  too big then MariaDB will used key lookup even when table scan is
  better.
*/
#define DEFAULT_ROW_COPY_COST     ((double) 1.0 / 20.0)
#define ROW_COPY_COST             optimizer_row_copy_cost
#define ROW_COPY_COST_THD(THD)    ((THD)->variables.optimizer_row_copy_cost)

/*
  Creating a record from the join cache is faster than getting a row from
  the engine. JOIN_CACHE_ROW_COPY_COST_FACTOR is the factor used to
  take this into account. This is multiplied with ROW_COPY_COST.
*/
#define JOIN_CACHE_ROW_COPY_COST_FACTOR 0.75

/*
  Cost of finding the next key during index scan and copying it to
  'table->record'

  If this is too small, then index scans will be prefered over 'ref'
  as with table scans there are no key read (KEY_LOOKUP_COST) and
  fewer disk reads.
*/
#define DEFAULT_KEY_COPY_COST     ((double) 1.0 / 40.0)
#define KEY_COPY_COST             optimizer_key_copy_cost
#define KEY_COPY_COST_THD(THD)    ((THD)->variables.optimizer_key_copy_cost)

/*
  Cost of finding the next index entry and checking it against filter
  This cost is very low as it's done inside the storage engine.
  Should be smaller than KEY_COPY_COST.
 */
#define DEFAULT_KEY_NEXT_FIND_COST ((double) 1.0 / 80.0)
#define KEY_NEXT_FIND_COST         optimizer_next_find_cost

/**
  The following is used to decide if MariaDB should use table scanning
  instead of reading with keys.  The number says how many evaluation of the
  WHERE clause is comparable to reading one extra row from a table.
*/
#define DEFAULT_WHERE_COST      (1 / 5.0)
#define WHERE_COST              optimizer_where_cost
#define WHERE_COST_THD(THD)     ((THD)->variables.optimizer_where_cost)

#define DEFAULT_KEY_COMPARE_COST        (1 / 20.0)
#define KEY_COMPARE_COST                optimizer_key_cmp_cost

/*
  Cost of comparing two rowids. This is set relative to KEY_COMPARE_COST
  This is usally just a memcmp!
*/
#define ROWID_COMPARE_COST          KEY_COMPARE_COST/10.0
#define ROWID_COMPARE_COST_THD(THD) ((THD)->variables.KEY_COMPARE_COST / 10.0)

/*
  Setup cost for different operations
*/

/* Extra cost for doing a range scan. Used to prefer 'ref' over range */
#define MULTI_RANGE_READ_SETUP_COST (double) (1.0 / 50.0)

/*
  These costs are mainly to handle small tables, like the one we have in the
  mtr test suite
*/
/* Extra cost for full table scan. Used to prefer range over table scans */
#define TABLE_SCAN_SETUP_COST 1.0
/* Extra cost for full index scan. Used to prefer range over index scans */
#define INDEX_SCAN_SETUP_COST 1.0

/*
  The lower bound of accepted rows when using filter.
  This is used to ensure that filters are not too agressive.
*/
#define MIN_ROWS_AFTER_FILTERING 1.0

/*
  cost1 is better that cost2 only if cost1 + COST_EPS < cost2
  The main purpose of this is to ensure we use the first index or plan
  when there are identical plans. Without COST_EPS some plans in the
  test suite would vary depending on floating point calculations done
  in different paths.
 */
#define COST_EPS  0.0001

/*
  For sequential disk seeks the cost formula is:
    DISK_SEEK_BASE_COST + DISK_SEEK_PROP_COST * #blocks_to_skip

  The cost of average seek
    DISK_SEEK_BASE_COST + DISK_SEEK_PROP_COST*BLOCKS_IN_AVG_SEEK =1.0.
*/
#define DISK_SEEK_BASE_COST ((double)0.9)

#define BLOCKS_IN_AVG_SEEK  128

#define DISK_SEEK_PROP_COST ((double)0.1/BLOCKS_IN_AVG_SEEK)

/*
  Subquery materialization-related constants
*/
/* This should match ha_heap::read_time() */
#define HEAP_TEMPTABLE_LOOKUP_COST 0.05
#define HEAP_TEMPTABLE_CREATE_COST 1.0
#define DISK_TEMPTABLE_LOOKUP_COST 1.0
#define DISK_TEMPTABLE_CREATE_COST TMPFILE_CREATE_COST*2 /* 2 tmp tables */
#define DISK_TEMPTABLE_BLOCK_SIZE  8192

#define SORT_INDEX_CMP_COST 0.02

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

#endif /* OPTIMIZER_COSTS_INCLUDED */
