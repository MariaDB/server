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

/**
  The following is used to decide if MySQL should use table scanning
  instead of reading with keys.  The number says how many evaluation of the
  WHERE clause is comparable to reading one extra row from a table.
*/
#define TIME_FOR_COMPARE         5.0	//  5 WHERE compares == one read
#define TIME_FOR_COMPARE_IDX    20.0

#define IDX_BLOCK_COPY_COST  ((double) 1 / TIME_FOR_COMPARE)
#define IDX_LOOKUP_COST      ((double) 1 / 8)
#define MULTI_RANGE_READ_SETUP_COST (IDX_BLOCK_COPY_COST/10)

/**
  Number of comparisons of table rowids equivalent to reading one row from a
  table.
*/
#define TIME_FOR_COMPARE_ROWID  (TIME_FOR_COMPARE*100)

/* cost1 is better that cost2 only if cost1 + COST_EPS < cost2 */
#define COST_EPS  0.001

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
#define HEAP_TEMPTABLE_LOOKUP_COST 0.05
#define DISK_TEMPTABLE_LOOKUP_COST 1.0
#define SORT_INDEX_CMP_COST 0.02

#define COST_MAX (DBL_MAX * (1.0 - DBL_EPSILON))

#define COST_ADD(c,d) (COST_MAX - (d) > (c) ? (c) + (d) : COST_MAX)

#define COST_MULT(c,f) (COST_MAX / (f) > (c) ? (c) * (f) : COST_MAX)

#endif /* OPTIMIZER_COSTS_INCLUDED */
