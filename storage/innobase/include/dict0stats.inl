/*****************************************************************************

Copyright (c) 2012, 2015, Oracle and/or its affiliates. All rights reserved.
Copyright (c) 2017, 2021, MariaDB Corporation.

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
@file include/dict0stats.ic
Code used for calculating and manipulating table statistics.

Created Jan 23, 2012 Vasil Dimov
*******************************************************/

#include "dict0dict.h"

/*********************************************************************//**
Initialize table's stats for the first time when opening a table. */
UNIV_INLINE
void
dict_stats_init(
/*============*/
	dict_table_t*	table)	/*!< in/out: table */
{
	ut_ad(!table->stats_mutex_is_owner());

	uint32_t stat = table->stat;

	if (stat & dict_table_t::STATS_INITIALIZED) {
		return;
	}

	dict_stats_upd_option_t	opt;

	if (table->stats_is_persistent(stat)) {
		opt = DICT_STATS_FETCH_ONLY_IF_NOT_IN_MEMORY;
	} else {
		opt = DICT_STATS_RECALC_TRANSIENT;
	}

	dict_stats_update(table, opt);
}

/*********************************************************************//**
Deinitialize table's stats after the last close of the table. This is
used to detect "FLUSH TABLE" and refresh the stats upon next open. */
UNIV_INLINE
void
dict_stats_deinit(
/*==============*/
	dict_table_t*	table)	/*!< in/out: table */
{
	ut_ad(table->stats_mutex_is_owner());
	ut_ad(table->get_ref_count() == 0);

#ifdef HAVE_valgrind
	if (!table->stat_initialized()) {
		return;
	}

	MEM_UNDEFINED(&table->stat_n_rows, sizeof table->stat_n_rows);
	MEM_UNDEFINED(&table->stat_clustered_index_size,
		      sizeof table->stat_clustered_index_size);
	MEM_UNDEFINED(&table->stat_sum_of_other_index_sizes,
		      sizeof table->stat_sum_of_other_index_sizes);
	MEM_UNDEFINED(&table->stat_modified_counter,
		      sizeof table->stat_modified_counter);

	dict_index_t*   index;

	for (index = dict_table_get_first_index(table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {
		MEM_UNDEFINED(
			index->stat_n_diff_key_vals,
			index->n_uniq
			* sizeof index->stat_n_diff_key_vals[0]);
		MEM_UNDEFINED(
			index->stat_n_sample_sizes,
			index->n_uniq
			* sizeof index->stat_n_sample_sizes[0]);
		MEM_UNDEFINED(
			index->stat_n_non_null_key_vals,
			index->n_uniq
			* sizeof index->stat_n_non_null_key_vals[0]);
		MEM_UNDEFINED(
			&index->stat_index_size,
			sizeof(index->stat_index_size));
		MEM_UNDEFINED(
			&index->stat_n_leaf_pages,
			sizeof(index->stat_n_leaf_pages));
	}
#endif /* HAVE_valgrind */

	table->stat = table->stat & ~dict_table_t::STATS_INITIALIZED;
}
