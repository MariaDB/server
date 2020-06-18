/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2020, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file ha/hash0hash.cc
The simple hash table utility

Created 5/20/1997 Heikki Tuuri
*******************************************************/

#include "hash0hash.h"
#include "mem0mem.h"
#include "sync0sync.h"

/**
Create a hash table.
@param n   the minimum number of hash array elements
@return created table (with n_cells being a prime, at least n) */
hash_table_t *hash_create(ulint n)
{
  ulint prime= ut_find_prime(n);

  hash_table_t *table= static_cast<hash_table_t*>
    (ut_zalloc_nokey(sizeof *table));
  table->array= static_cast<hash_cell_t*>(ut_zalloc_nokey(sizeof(hash_cell_t) *
                                                          prime));
  table->n_cells= prime;
  ut_d(table->magic_n= HASH_TABLE_MAGIC_N);
  return table;
}

/*************************************************************//**
Frees a hash table. */
void
hash_table_free(
/*============*/
	hash_table_t*	table)	/*!< in, own: hash table */
{
	ut_ad(table->magic_n == HASH_TABLE_MAGIC_N);

	ut_free(table->array);
	ut_free(table);
}
