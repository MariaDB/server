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

enum dict_stats_upd_option_t {
	DICT_STATS_RECALC_PERSISTENT,/* (re) calculate the
				statistics using a precise and slow
				algo and save them to the persistent
				storage, if the persistent storage is
				not present then emit a warning and
				fall back to transient stats */
	DICT_STATS_RECALC_TRANSIENT,/* (re) calculate the statistics
				using an imprecise quick algo
				without saving the results
				persistently */
	DICT_STATS_EMPTY_TABLE,	/* Write all zeros (or 1 where it makes sense)
				into a table and its indexes' statistics
				members. The resulting stats correspond to an
				empty table. If the table is using persistent
				statistics, then they are saved on disk. */
	DICT_STATS_FETCH_ONLY_IF_NOT_IN_MEMORY /* fetch the stats
				from the persistent storage if the in-memory
				structures have not been initialized yet,
				otherwise do nothing */
};

/*********************************************************************//**
Set the persistent statistics flag for a given table. This is set only
in the in-memory table object and is not saved on disk. It will be read
from the .frm file upon first open from MySQL after a server restart. */
UNIV_INLINE
void
dict_stats_set_persistent(
/*======================*/
	dict_table_t*	table,	/*!< in/out: table */
	ibool		ps_on,	/*!< in: persistent stats explicitly enabled */
	ibool		ps_off)	/*!< in: persistent stats explicitly disabled */
	MY_ATTRIBUTE((nonnull));

/** @return whether persistent statistics is enabled for a given table */
UNIV_INLINE
bool
dict_stats_is_persistent_enabled(const dict_table_t* table)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/*********************************************************************//**
Set the auto recalc flag for a given table (only honored for a persistent
stats enabled table). The flag is set only in the in-memory table object
and is not saved in InnoDB files. It will be read from the .frm file upon
first open from MySQL after a server restart. */
UNIV_INLINE
void
dict_stats_auto_recalc_set(
/*=======================*/
	dict_table_t*	table,			/*!< in/out: table */
	ibool		auto_recalc_on,		/*!< in: explicitly enabled */
	ibool		auto_recalc_off);	/*!< in: explicitly disabled */

/** @return whether auto recalc is enabled for a given table*/
UNIV_INLINE
bool
dict_stats_auto_recalc_is_enabled(const dict_table_t* table)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/*********************************************************************//**
Initialize table's stats for the first time when opening a table. */
UNIV_INLINE
void
dict_stats_init(
/*============*/
	dict_table_t*	table);	/*!< in/out: table */

/*********************************************************************//**
Deinitialize table's stats after the last close of the table. This is
used to detect "FLUSH TABLE" and refresh the stats upon next open. */
UNIV_INLINE
void
dict_stats_deinit(
/*==============*/
	dict_table_t*	table)	/*!< in/out: table */
	MY_ATTRIBUTE((nonnull));

#ifdef WITH_WSREP
/** Update the table modification counter and if necessary,
schedule new estimates for table and index statistics to be calculated.
@param[in,out]	table	persistent or temporary table
@param[in]	trx	transaction */
void dict_stats_update_if_needed(dict_table_t *table, const trx_t &trx)
	MY_ATTRIBUTE((nonnull));
#else
/** Update the table modification counter and if necessary,
schedule new estimates for table and index statistics to be calculated.
@param[in,out]	table	persistent or temporary table */
void dict_stats_update_if_needed_func(dict_table_t *table)
	MY_ATTRIBUTE((nonnull));
# define dict_stats_update_if_needed(t,trx) dict_stats_update_if_needed_func(t)
#endif

/*********************************************************************//**
Calculates new estimates for table and index statistics. The statistics
are used in query optimization.
@return DB_* error code or DB_SUCCESS */
dberr_t
dict_stats_update(
/*==============*/
	dict_table_t*		table,	/*!< in/out: table */
	dict_stats_upd_option_t	stats_upd_option);
					/*!< in: whether to (re) calc
					the stats or to fetch them from
					the persistent storage */

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
void
dict_stats_update_for_index(
/*========================*/
	dict_index_t*	index)	/*!< in/out: index */
	MY_ATTRIBUTE((nonnull));

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

/** Report an error if updating table statistics failed because
.ibd file is missing, table decryption failed or table is corrupted.
@param[in,out]	table	Table
@param[in]	defragment	true if statistics is for defragment
@retval DB_DECRYPTION_FAILED if decryption of the table failed
@retval DB_TABLESPACE_DELETED if .ibd file is missing
@retval DB_CORRUPTION if table is marked as corrupted */
dberr_t
dict_stats_report_error(dict_table_t* table, bool defragment = false)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

#include "dict0stats.inl"

#ifdef UNIV_ENABLE_UNIT_TEST_DICT_STATS
void test_dict_stats_all();
#endif /* UNIV_ENABLE_UNIT_TEST_DICT_STATS */

#endif /* dict0stats_h */
