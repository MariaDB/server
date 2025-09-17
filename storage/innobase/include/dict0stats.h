/*****************************************************************************

Copyright (c) 2009, 2018, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/dict0stats.h
Code used for calculating and manipulating table statistics.

Created Jan 06, 2010 Vasil Dimov
*******************************************************/

#ifndef dict0stats_h
#define dict0stats_h

#include "dict0types.h"
#include "trx0types.h"

/** Update the table modification counter and if necessary,
schedule new estimates for table and index statistics to be calculated.
@param[in,out]	table	persistent or temporary table
@param[in,out]	trx	transaction */
void dict_stats_update_if_needed(dict_table_t *table, trx_t &trx) noexcept
	MY_ATTRIBUTE((nonnull));

/** Execute DELETE FROM mysql.innodb_table_stats
@param database_name  database name
@param table_name     table name
@param trx            transaction
@return DB_SUCCESS or error code */
dberr_t dict_stats_delete_from_table_stats(const char *database_name,
                                           const char *table_name,
                                           trx_t *trx)
  MY_ATTRIBUTE((nonnull));
/** Execute DELETE FROM mysql.innodb_index_stats
@param database_name  database name
@param table_name     table name
@param trx            transaction
@return DB_SUCCESS or error code */
dberr_t dict_stats_delete_from_index_stats(const char *database_name,
                                           const char *table_name,
                                           trx_t *trx)
  MY_ATTRIBUTE((nonnull));
/** Execute DELETE FROM mysql.innodb_index_stats
@param database_name  database name
@param table_name     table name
@param index_name     name of the index
@param trx            transaction (nullptr=start and commit a new one)
@return DB_SUCCESS or error code */
dberr_t dict_stats_delete_from_index_stats(const char *database_name,
                                           const char *table_name,
                                           const char *index_name, trx_t *trx);

/*********************************************************************//**
Fetches or calculates new estimates for index statistics. */
void dict_stats_update_for_index(trx_t *trx, dict_index_t *index) noexcept
  MY_ATTRIBUTE((nonnull));

enum dict_stats_schema_check {
  /** The InnoDB persistent statistics tables do not exist. */
  SCHEMA_NOT_EXIST= -1,
  /** The schema of the InnoDB persistent statistics tables is valid. */
  SCHEMA_OK= 0,
  /** The schema is invalid. */
  SCHEMA_INVALID
};

/** @return whether the persistent statistics storage is usable */
dict_stats_schema_check
dict_stats_persistent_storage_check(bool dict_already_locked= false) noexcept;

/** Save the persistent statistics of a table or an index.
@param table            table whose stats to save
@param only_for_index   the index ID to save statistics for (0=all)
@return DB_SUCCESS or error code */
dberr_t dict_stats_save(dict_table_t* table, index_id_t index_id= 0);

/** Read the stored persistent statistics of a table. */
dberr_t dict_stats_fetch_from_ps(dict_table_t *table);

/**
Calculate new estimates for table and index statistics. This function
is relatively quick and is used to calculate non-persistent statistics.
@param trx      transaction
@param table    table for which the non-persistent statistics are being updated
@return error code
@retval DB_SUCCESS_LOCKED REC if the table under bulk insert operation */
dberr_t dict_stats_update_transient(trx_t *trx, dict_table_t *table) noexcept;

/**
Calculate new estimates for table and index statistics. This function
is slower than dict_stats_update_transient().
@param trx      transaction
@param table    table for which the persistent statistics are being updated
@return DB_SUCCESS or error code
@retval DB_SUCCESS_LOCKED_REC if the table under bulk insert operation */
dberr_t dict_stats_update_persistent(trx_t *trx, dict_table_t *table) noexcept;

/**
Try to calculate and save new estimates for persistent statistics.
If persistent statistics are not enabled for the table or not available,
this does nothing. */
dberr_t dict_stats_update_persistent_try(trx_t *trx, dict_table_t *table)
  noexcept;

/** Rename a table in InnoDB persistent stats storage.
@param old_name  old table name
@param new_name  new table name
@param trx       transaction
@return DB_SUCCESS or error code */
dberr_t dict_stats_rename_table(const char *old_name, const char *new_name,
                                trx_t *trx);
/** Rename an index in InnoDB persistent statistics.
@param db         database name
@param table      table name
@param old_name   old table name
@param new_name   new table name
@param trx        transaction
@return DB_SUCCESS or error code */
dberr_t dict_stats_rename_index(const char *db, const char *table,
                                const char *old_name, const char *new_name,
                                trx_t *trx);

/** Delete all persistent statistics for a database.
@param db    database name
@param trx   transaction
@return DB_SUCCESS or error code */
dberr_t dict_stats_delete(const char *db, trx_t *trx);

/** Save an individual index's statistic into the persistent statistics
storage.
@param[in]	index			index to be updated
@param[in]	last_update		timestamp of the stat
@param[in]	stat_name		name of the stat
@param[in]	stat_value		value of the stat
@param[in]	sample_size		n pages sampled or NULL
@param[in]	stat_description	description of the stat
@param[in,out]	trx			transaction
@return DB_SUCCESS or error code */
dberr_t
dict_stats_save_index_stat(
	dict_index_t*	index,
	time_t		last_update,
	const char*	stat_name,
	ib_uint64_t	stat_value,
	ib_uint64_t*	sample_size,
	const char*	stat_description,
	trx_t*		trx)
	MY_ATTRIBUTE((nonnull(1, 3, 6, 7)));

#ifdef UNIV_ENABLE_UNIT_TEST_DICT_STATS
void test_dict_stats_all();
#endif /* UNIV_ENABLE_UNIT_TEST_DICT_STATS */

/** Write all zeros (or 1 where it makes sense) into a table and its indexes'
statistics members. The resulting stats correspond to an empty table.
@param table  table statistics to be emptied */
void dict_stats_empty_table(dict_table_t *table);

/** Clear the statistics for a table and save them if
persistent statistics are enabled. */
void dict_stats_empty_table_and_save(dict_table_t *table);
#endif /* dict0stats_h */
