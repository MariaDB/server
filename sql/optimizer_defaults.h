#ifndef OPTIMIZER_DEFAULTS_INCLUDED
#define OPTIMIZER_DEFAULTS_INCLUDED
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
  This file contains costs constants used by the optimizer
  All costs should be based on milliseconds (1 cost = 1 ms)
*/

/* Cost for finding the first key in a key scan */
#define DEFAULT_KEY_LOOKUP_COST      ((double) 0.000435777)

/* Cost of finding a row based on row_ID */
#define DEFAULT_ROW_LOOKUP_COST      ((double) 0.000130839)

/*
  Cost of finding and copying key and row blocks from the storage
  engine index cache to an internal cache as part of an index
  scan. This includes all mutexes that needs to be taken to get
  exclusive access to a page.  The number is taken from accessing an
  existing blocks from Aria page cache.
  Used in handler::scan_time() and handler::keyread_time()
*/
#define DEFAULT_INDEX_BLOCK_COPY_COST  ((double) 3.56e-05)

/*
  Cost of copying a row to 'table->record'.
  Used by scan_time() and rnd_pos_time() methods.

  If this is too small, then table scans will be prefered over 'ref'
  as with table scans there are no key read (KEY_LOOKUP_COST), fewer
  disk reads but more record copying and row comparisions.  If it's
  too big then MariaDB will used key lookup even when table scan is
  better.
*/
#define DEFAULT_ROW_COPY_COST     ((double) 0.000060866)

/*
  Cost of copying the key to 'table->record'

  If this is too small, then, for small tables, index scans will be
  prefered over 'ref' as with index scans there are fewer disk reads.
*/
#define DEFAULT_KEY_COPY_COST     ((double) 0.000015685)

/*
  Cost of finding the next index entry and checking its rowid against filter
  This cost is very low as it's done inside the storage engine.
  Should be smaller than KEY_COPY_COST.
 */
#define DEFAULT_KEY_NEXT_FIND_COST     ((double) 0.000082347)

/* Cost of finding the next row when scanning a table */
#define DEFAULT_ROW_NEXT_FIND_COST     ((double) 0.000045916)

/**
  The cost of executing the WHERE clause as part of any row check.
  Increasing this would force the optimizer to use row combinations
  that reads fewer rows.
  The default cost comes from recording times from a simple where clause that
  compares two fields (date and a double) with constants.
*/
#define DEFAULT_WHERE_COST             ((double) 3.2e-05)

/* The cost of comparing a key when using range access or sorting */
#define DEFAULT_KEY_COMPARE_COST       0.000011361

/* Rowid compare is usually just a single memcmp of a short string */
#define DEFAULT_ROWID_COMPARE_COST     0.000002653
/* Rowid copy is usually just a single memcpy of a short string */
#define DEFAULT_ROWID_COPY_COST        0.000002653

/*
  Cost modifiers rowid_filter. These takes into account the overhead of
  using and calling Rowid_filter_sorted_array::check() from the engine
*/
#define ROWID_FILTER_PER_CHECK_MODIFIER 4       /* times key_copy_cost */
#define ROWID_FILTER_PER_ELEMENT_MODIFIER 1     /* times rowid_compare_cost */

/*
  Average disk seek time on a hard disk is 8-10 ms, which is also
  about the time to read a IO_SIZE (8192) block.

  A medium ssd is about 400MB/second, which gives us the time for
  reading an IO_SIZE block to IO_SIZE/400000000 = 0.0000204 sec= 0.02 ms.
*/
#define DEFAULT_DISK_READ_COST ((double) IO_SIZE / 400000000.0 * 1000)

/*
  The follwoing is an old comment for hard-disks, please ignore the
  following, except if you like history:

  For sequential hard disk seeks the cost formula is:
    DISK_SEEK_BASE_COST + DISK_SEEK_PROP_COST * #blocks_to_skip

  The cost of average seek
    DISK_SEEK_BASE_COST + DISK_SEEK_PROP_COST*BLOCKS_IN_AVG_SEEK = 10.
*/


/*
  The table/index cache_miss/total_cache_request ratio.
  1.0 means that a searched for key or row will never be in the cache while
  0.0 means it always in the cache (and we don't have to do any disk reads).

  According to folklore, one should not have to access disk for more
  than 20% of the cache request for MariaDB to run very well.
  However in practice when we read rows or keys in a query, we will often
  read the same row over and over again. Because of this we set
  DEFAULT_DISK_READ_RATIO to 0.20/10 = 0.02.

  Increasing DISK_READ_RATIO will make MariaDB prefer key lookup over
  table scans as the impact of ROW_COPY_COST and INDEX_COPY cost will
  have a larger impact when more rows are examined..

  We are not yet taking into account cache usage statistics as this
  could confuse users as the EXPLAIN and costs for a query would change
  between to query calls, which may confuse users (and also make the
  mtr tests very unpredictable).

  Note that the engine's avg_io_cost() (DEFAULT_DISK_READ_COST by default)
  is multiplied with this constant!
*/

#define DEFAULT_DISK_READ_RATIO 0.02

/*
  The following costs are mainly to ensure we don't do table and index
  scans for small tables, like the one we have in the mtr test suite.

  This is mostly to keep the mtr tests use indexes (as the optimizer would
  if the tables are large).  It will also ensure that EXPLAIN is showing
  more key user for users where they are testing queries with small tables
  at the start of projects.
  This is probably OK for most a the execution time difference between table
  scan and index scan compared to key lookups so small when using small
  tables. It also helps to fill the index cache which will help mitigate
  the speed difference.
*/

/*
  Extra cost for full table and index scan. Used to prefer key and range
  over index and table scans

  INDEX_SCAN_SETUP_COST (defined in optimizer_costs.h) is half of
  table_scan_setup_cost to get the optimizer to prefer index scans to table
  scans as key copy is faster than row copy and index blocks provides
  more information in the cache.

  This will also help MyISAM as with MyISAM the table scans has a cost
  very close to index scans (they are fast but require a read call
  that we want to avoid even if it's small).

  10 usec is about 10 MyISAM row lookups with optimizer_disk_read_ratio= 0.02
*/
#define DEFAULT_TABLE_SCAN_SETUP_COST 0.01          // 10 usec

/* Extra cost for doing a range scan. Used to prefer 'ref' over range */
#define MULTI_RANGE_READ_SETUP_COST KEY_LOOKUP_COST

/*
  Temporary file and temporary table related costs
  Used with subquery materialization, derived tables etc
*/

#define TMPFILE_CREATE_COST         0.5  // Cost of creating and deleting files
#define HEAP_TEMPTABLE_CREATE_COST  0.025 // ms
/* Cost taken from HEAP_LOOKUP_COST in ha_heap.cc */
#define HEAP_TEMPTABLE_LOOKUP_COST (0.00016097)
#define DISK_TEMPTABLE_LOOKUP_COST(thd) (tmp_table_optimizer_costs.key_lookup_cost + tmp_table_optimizer_costs.row_lookup_cost + tmp_table_optimizer_costs.row_copy_cost)
#define DISK_TEMPTABLE_CREATE_COST TMPFILE_CREATE_COST*2 // 2 tmp tables
#define DISK_TEMPTABLE_BLOCK_SIZE  IO_SIZE

#endif /* OPTIMIZER_DEFAULTS_INCLUDED */
