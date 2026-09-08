/*****************************************************************************

Copyright (c) 2009, 2019, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2023, MariaDB Corporation.

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
@file dict/dict0stats.cc
Code used for calculating and manipulating table statistics.

Created Jan 06, 2010 Vasil Dimov
*******************************************************/

#include "dict0stats.h"
#include "row0sel.h"
#include "trx0trx.h"
#include "lock0lock.h"
#include "pars0pars.h"
#include <mysql_com.h>
#include "log.h"
#include "btr0btr.h"
#include "que0que.h"
#include "scope.h"
#include "debug_sync.h"
#include "btr0cur.h"
#include "small_vector.h"
#ifdef WITH_WSREP
# include <mysql/service_wsrep.h>
#endif

#include <algorithm>
#include <map>
#include <vector>
#include <thread>

/** Gets the externally stored size of a record, in units of a database page.
@param	rec	record
@param	offsets	array returned by rec_get_offsets()
@param	table	table containing the record
@return externally stored size in units of a database page */
static uint32_t rec_get_n_blob_pages(const rec_t *rec,
                                     const rec_offs *offsets,
                                     const dict_table_t *table) noexcept
{
  ut_ad(!rec_offs_comp(offsets) || !rec_get_node_ptr_flag(rec));
  if (!rec_offs_any_extern(offsets))
    return 0;
  uint32_t n_blobs= 0;
  const uint32_t blob_part_size= (DICT_TF_MASK_ZIP_SSIZE & table->flags)
    ? uint32_t(dict_tf_get_zip_size(table->flags)) - FIL_PAGE_DATA
    : uint32_t(srv_page_size) - FIL_PAGE_DATA -
        BTR_BLOB_HDR_SIZE - FIL_PAGE_DATA_END;
  for (ulint i= rec_offs_n_fields(offsets); i--; )
  {
    if (rec_offs_nth_extern(offsets, i))
    {
      uint32_t len=
        mach_read_from_4(btr_rec_get_field_ref(rec, offsets, i) +
                         BTR_EXTERN_LEN + 4);
      n_blobs+= (len + blob_part_size - 1) / blob_part_size;
    }
  }
  return n_blobs;
}

/* Persistent-statistics sampling algorithm (overview).

Estimates each index's cardinality by sampling A = N_SAMPLE_PAGES leaf pages
per n-column prefix (n = 1..n_uniq), rather than scanning all L leaf pages.

Symbols:
A = sampled pages/prefix;
L = size of BTR_SEG_LEAF (including possible BLOB pages in clustered index)
L_ORD = number of geniue (non-blob) leaf pages
LA =  level chosen to sample from; see dict_stats_index_set_n_diff() [REF01]
for the final estimator and the meaning of TOTAL_LA, N_DIFF_LA, Pi, R.

Call graph:
  dict_stats_analyze_index()             driver; loops over n_prefix
    IndexLevelStats::analyze_level()     full-scan one level, collect n_diff[]
    IndexLevelStats::sample_leaf_pages() pick A records on LA, dive below each
    PageStats::scan_below()              descend to a leaf via non-boring recs
    PageStats::scan<true>()              full leaf scan: distinct/non-null/BLOB
    dict_stats_index_set_n_diff()        [REF01] turn samples into cardinality */

/* names of the tables from the persistent statistics storage */
#define TABLE_STATS_NAME_PRINT	"mysql.innodb_table_stats"
#define INDEX_STATS_NAME_PRINT	"mysql.innodb_index_stats"
#define DEBUG_PRINTF(fmt, ...)	/* noop */

/* Gets the number of leaf pages to sample in persistent stats estimation */
#define N_SAMPLE_PAGES(index)					\
		(index)->table->stats_sample_pages != 0		\
		? (index)->table->stats_sample_pages		\
		: srv_stats_persistent_sample_pages

/* number of distinct records on a given level that are required to stop
descending to lower levels and fetch N_SAMPLE_PAGES(index) records
from that level */
#define N_DIFF_REQUIRED(index)	(N_SAMPLE_PAGES(index) * 10)

/* A dynamic array where we store the boundaries of each distinct group
of keys. For example if a btree level is:
index: 0,1,2,3,4,5,6,7,8,9,10,11,12
data:  b,b,b,b,b,b,g,g,j,j,j, x, y
then we would store 5,7,10,11,12 in the array. */
typedef small_vector<uint64_t, 8>	Boundaries;

/** Allocator type used for index_map_t. */
typedef ut_allocator<std::pair<const char* const, dict_index_t*> >
	index_map_t_allocator;

/** Auxiliary map used for sorting indexes by name in dict_stats_save(). */
typedef std::map<const char*, dict_index_t*, ut_strcmp_functor,
		index_map_t_allocator>	index_map_t;

bool dict_table_t::is_stats_table() const
{
  return !strcmp(name.m_name, TABLE_STATS_NAME) ||
         !strcmp(name.m_name, INDEX_STATS_NAME);
}

bool trx_t::has_stats_table_lock() const
{
  for (const lock_t *l : lock.table_locks)
    if (l && l->un_member.tab_lock.table->is_stats_table())
      return true;
  return false;
}

/*********************************************************************//**
Checks whether an index should be ignored in stats manipulations:
* stats fetch
* stats recalc
* stats save
@return true if exists and all tables are ok */
UNIV_INLINE
bool
dict_stats_should_ignore_index(
/*===========================*/
	const dict_index_t*	index)	/*!< in: index */
{
  return !index->is_btree() || index->to_be_dropped || !index->is_committed();
}


namespace {

/** expected column definition */
struct ColMeta
{
  /** column name */
  const char *name;
  /** main type */
  unsigned mtype;
  /** prtype mask; all these bits have to be set in prtype */
  unsigned prtype_mask;
  /** column length in bytes */
  unsigned len;
};

/** For checking whether a table exists and has a predefined schema */
struct TableSchema
{
  /** table name */
  span<const char> table_name;
  /** table name in SQL */
  const char *table_name_sql;
  /** number of columns */
  unsigned n_cols;
  /** columns */
  const ColMeta columns[8];
};

} // anonymous namespace

static const TableSchema table_stats_schema =
{
  {C_STRING_WITH_LEN(TABLE_STATS_NAME)}, TABLE_STATS_NAME_PRINT, 6,
  {
    {"database_name", DATA_VARMYSQL, DATA_NOT_NULL, 192},
    {"table_name", DATA_VARMYSQL, DATA_NOT_NULL, 597},
    /*
      Don't check the DATA_UNSIGNED flag in last_update.
      It presents if the server is running in a pure MariaDB installation,
      because MariaDB's Field_timestampf::flags has UNSIGNED_FLAG.
      But DATA_UNSIGNED misses when the server starts on a MySQL-5.7 directory
      (during a migration), because MySQL's Field_timestampf::flags does not
      have UNSIGNED_FLAG.
      This is fine not to check DATA_UNSIGNED, because Field_timestampf
      in both MariaDB and MySQL support only non-negative time_t values.
    */
    {"last_update", DATA_INT, DATA_NOT_NULL, 4},
    {"n_rows", DATA_INT, DATA_NOT_NULL | DATA_UNSIGNED, 8},
    {"clustered_index_size", DATA_INT, DATA_NOT_NULL | DATA_UNSIGNED, 8},
    {"sum_of_other_index_sizes", DATA_INT, DATA_NOT_NULL | DATA_UNSIGNED, 8},
  }
};

static const TableSchema index_stats_schema =
{
  {C_STRING_WITH_LEN(INDEX_STATS_NAME)}, INDEX_STATS_NAME_PRINT, 8,
  {
    {"database_name", DATA_VARMYSQL, DATA_NOT_NULL, 192},
    {"table_name", DATA_VARMYSQL, DATA_NOT_NULL, 597},
    {"index_name", DATA_VARMYSQL, DATA_NOT_NULL, 192},
    /*
      Don't check the DATA_UNSIGNED flag in last_update.
      See comments about last_update in table_stats_schema above.
    */
    {"last_update", DATA_INT, DATA_NOT_NULL, 4},
    {"stat_name", DATA_VARMYSQL, DATA_NOT_NULL, 64*3},
    {"stat_value", DATA_INT, DATA_NOT_NULL | DATA_UNSIGNED, 8},
    {"sample_size", DATA_INT, DATA_UNSIGNED, 8},
    {"stat_description", DATA_VARMYSQL, DATA_NOT_NULL, 1024*3}
  }
};

/** Construct the type's SQL name (e.g. BIGINT UNSIGNED)
@param mtype   InnoDB main type
@param prtype  InnoDB precise type
@param len     length of the column
@param name    the SQL name
@param name_sz size of the name buffer
@return number of bytes written (excluding the terminating NUL byte) */
static int dtype_sql_name(unsigned mtype, unsigned prtype, unsigned len,
                          char *name, size_t name_sz)
{
  const char *Unsigned= "";
  const char *Main= "UNKNOWN";

  switch (mtype) {
  case DATA_INT:
    switch (len) {
    case 1:
      Main= "TINYINT";
      break;
    case 2:
      Main= "SMALLINT";
      break;
    case 3:
      Main= "MEDIUMINT";
      break;
    case 4:
      Main= "INT";
      break;
    case 8:
      Main= "BIGINT";
      break;
    }

  append_unsigned:
    if (prtype & DATA_UNSIGNED)
      Unsigned= " UNSIGNED";
    len= 0;
    break;
  case DATA_FLOAT:
    Main= "FLOAT";
    goto append_unsigned;
  case DATA_DOUBLE:
    Main= "DOUBLE";
    goto append_unsigned;
  case DATA_FIXBINARY:
    Main= "BINARY";
    break;
  case DATA_CHAR:
  case DATA_MYSQL:
    Main= "CHAR";
    break;
  case DATA_VARCHAR:
  case DATA_VARMYSQL:
    Main= "VARCHAR";
    break;
  case DATA_BINARY:
    Main= "VARBINARY";
    break;
  case DATA_GEOMETRY:
    Main= "GEOMETRY";
    len= 0;
    break;
  case DATA_BLOB:
    switch (len) {
    case 9:
      Main= "TINYBLOB";
      break;
    case 10:
      Main= "BLOB";
      break;
    case 11:
      Main= "MEDIUMBLOB";
      break;
    case 12:
      Main= "LONGBLOB";
      break;
    }
    len= 0;
  }

  const char* Not_null= (prtype & DATA_NOT_NULL) ? " NOT NULL" : "";
  if (len)
    return snprintf(name, name_sz, "%s(%u)%s%s", Main, len, Unsigned,
                    Not_null);
  else
    return snprintf(name, name_sz, "%s%s%s", Main, Unsigned, Not_null);
}

static bool innodb_index_stats_not_found;
static bool innodb_table_stats_not_found_reported;
static bool innodb_index_stats_not_found_reported;

/*********************************************************************//**
Checks whether a table exists and whether it has the given structure.
The table must have the same number of columns with the same names and
types. The order of the columns does not matter.
dict_table_schema_check() @{
@return DB_SUCCESS if the table exists and contains the necessary columns */
static
dberr_t
dict_table_schema_check(
/*====================*/
	const TableSchema* req_schema,	/*!< in: required table
						schema */
	char*			errstr,		/*!< out: human readable error
						message if != DB_SUCCESS is
						returned */
	size_t			errstr_sz)	/*!< in: errstr size */
{
	const dict_table_t* table= dict_sys.load_table(req_schema->table_name);

	if (!table) {
		if (opt_bootstrap)
			return DB_STATS_DO_NOT_EXIST;
		if (req_schema == &table_stats_schema) {
			if (innodb_table_stats_not_found_reported) {
				return DB_STATS_DO_NOT_EXIST;
			}
			innodb_table_stats_not_found_reported = true;
		} else {
			ut_ad(req_schema == &index_stats_schema);
			if (innodb_index_stats_not_found_reported) {
				return DB_STATS_DO_NOT_EXIST;
			}
			innodb_index_stats_not_found = true;
			innodb_index_stats_not_found_reported = true;
		}

		snprintf(errstr, errstr_sz, "Table %s not found.",
			 req_schema->table_name_sql);
		return DB_STATS_DO_NOT_EXIST;
	}

	if (!table->is_readable()) {
		/* table is not readable */
		snprintf(errstr, errstr_sz,
			 "Table %s is not readable.",
			 req_schema->table_name_sql);
		return DB_ERROR;
	}

	if (!table->space) {
		/* missing tablespace */
		snprintf(errstr, errstr_sz,
			 "Tablespace for table %s is missing.",
			 req_schema->table_name_sql);
		return DB_TABLE_NOT_FOUND;
	}

	if (unsigned(table->n_def - DATA_N_SYS_COLS) != req_schema->n_cols) {
		/* the table has a different number of columns than required */
		snprintf(errstr, errstr_sz,
			 "%s has %d columns but should have %u.",
			 req_schema->table_name_sql,
			 table->n_def - DATA_N_SYS_COLS,
			 req_schema->n_cols);
		return DB_ERROR;
	}

	/* For each column from req_schema->columns[] search
	whether it is present in table->cols[].
	The following algorithm is O(n_cols^2), but is optimized to
	be O(n_cols) if the columns are in the same order in both arrays. */

	for (unsigned i = 0; i < req_schema->n_cols; i++) {
		ulint	j = dict_table_has_column(
			table, req_schema->columns[i].name, i);

		if (j == table->n_def) {
			snprintf(errstr, errstr_sz,
				    "required column %s"
				    " not found in table %s.",
				    req_schema->columns[i].name,
				    req_schema->table_name_sql);

			return(DB_ERROR);
		}

		/* we found a column with the same name on j'th position,
		compare column types and flags */

		/* check length for exact match */
		if (req_schema->columns[i].len != table->cols[j].len) {
			snprintf(errstr, errstr_sz,
				 "Unexpected length of %s.%s. Please run "
				 "mariadb-upgrade or ALTER TABLE",
				 req_schema->table_name_sql,
				 req_schema->columns[i].name);
			return DB_ERROR;
		}

		/*
                  check mtype for exact match.
                  This check is relaxed to allow use to use TIMESTAMP
                  (ie INT) for last_update instead of DATA_BINARY.
                  We have to test for both values as the innodb_table_stats
                  table may come from MySQL and have the old type.
                */
		if (req_schema->columns[i].mtype != table->cols[j].mtype &&
                    !(req_schema->columns[i].mtype == DATA_INT &&
                      table->cols[j].mtype == DATA_FIXBINARY)) {
		} else if ((~table->cols[j].prtype
			    & req_schema->columns[i].prtype_mask)) {
		} else {
			continue;
		}

		int s = snprintf(errstr, errstr_sz,
				 "Column %s in table %s is ",
				 req_schema->columns[i].name,
				 req_schema->table_name_sql);
		if (s < 0 || static_cast<size_t>(s) >= errstr_sz) {
			return DB_ERROR;
		}
		errstr += s;
		errstr_sz -= s;
		s = dtype_sql_name(table->cols[j].mtype, table->cols[j].prtype,
				   table->cols[j].len, errstr, errstr_sz);
		if (s < 0 || static_cast<size_t>(s) + sizeof " but should be "
		    >= errstr_sz) {
			return DB_ERROR;
		}
		errstr += s;
		memcpy(errstr, " but should be ", sizeof " but should be ");
		errstr += (sizeof " but should be ") - 1;
		errstr_sz -= s + (sizeof " but should be ") - 1;
		s = dtype_sql_name(req_schema->columns[i].mtype,
				   req_schema->columns[i].prtype_mask,
				   req_schema->columns[i].len,
				   errstr, errstr_sz);
		return DB_ERROR;
	}

	if (size_t n_foreign = table->foreign_set.size()) {
		snprintf(errstr, errstr_sz,
			 "Table %s has %zu foreign key(s) pointing"
			 " to other tables, but it must have 0.",
			 req_schema->table_name_sql, n_foreign);
		return DB_ERROR;
	}

	if (size_t n_referenced = table->referenced_set.size()) {
		snprintf(errstr, errstr_sz,
			 "There are %zu foreign key(s) pointing to %s, "
			 "but there must be 0.", n_referenced,
			 req_schema->table_name_sql);
		return DB_ERROR;
	}

	return DB_SUCCESS;
}

dict_stats_schema_check
dict_stats_persistent_storage_check(bool dict_already_locked) noexcept
{
	char		errstr[512];
	dberr_t		ret;

	if (!dict_already_locked) {
		dict_sys.lock(SRW_LOCK_CALL);
	}

	ut_ad(dict_sys.locked());

	/* first check table_stats */
	ret = dict_table_schema_check(&table_stats_schema, errstr,
				      sizeof(errstr));
	if (ret == DB_SUCCESS) {
		/* if it is ok, then check index_stats */
		ret = dict_table_schema_check(&index_stats_schema, errstr,
					      sizeof(errstr));
	}

	if (!dict_already_locked) {
		dict_sys.unlock();
	}

	switch (ret) {
	case DB_SUCCESS:
		return SCHEMA_OK;
	case DB_STATS_DO_NOT_EXIST:
		return SCHEMA_NOT_EXIST;
	default:
		if (!opt_bootstrap) {
			sql_print_error("InnoDB: %s", errstr);
		}
		return SCHEMA_INVALID;
	}
}

/** Executes a given SQL statement using the InnoDB internal SQL parser.
This function will free the pinfo object.
@param[in,out]	pinfo	pinfo to pass to que_eval_sql() must already
have any literals bound to it
@param[in]	sql	SQL string to execute
@param[in,out]	trx	transaction
@return DB_SUCCESS or error code */
static
dberr_t dict_stats_exec_sql(pars_info_t *pinfo, const char* sql, trx_t *trx)
{
  ut_ad(dict_sys.locked());

  switch (dict_stats_persistent_storage_check(true)) {
  case SCHEMA_OK:
    return que_eval_sql(pinfo, sql, trx);
  case SCHEMA_INVALID:
  case SCHEMA_NOT_EXIST:
    break;
  }

  pars_info_free(pinfo);
  return DB_STATS_DO_NOT_EXIST;
}


/*********************************************************************//**
Write all zeros (or 1 where it makes sense) into an index
statistics members. The resulting stats correspond to an empty index. */
static
void
dict_stats_empty_index(
/*===================*/
	dict_index_t*	index,	/*!< in/out: index */
	bool		empty_defrag_stats)
				/*!< in: whether to empty defrag stats */
{
	ut_ad(!(index->type & DICT_FTS));
	ut_ad(!dict_index_is_ibuf(index));
	ut_ad(index->table->stats_mutex_is_owner());

	ulint	n_uniq = index->n_uniq;

	for (ulint i = 0; i < n_uniq; i++) {
		index->stat_n_diff_key_vals[i] = 0;
		index->stat_n_sample_sizes[i] = 1;
		index->stat_n_non_null_key_vals[i] = 0;
	}

	index->stat_index_size = 1;
	index->stat_n_leaf_pages = 1;

	if (empty_defrag_stats) {
		dict_stats_empty_defrag_stats(index);
		dict_stats_empty_defrag_summary(index);
	}
}

void dict_stats_empty_table(
	dict_table_t*	table,
	bool		empty_defrag_stats)
{
	/* Initialize table/index level stats is now protected by
	table level lock_mutex.*/
	table->stats_mutex_lock();

	/* Zero the stats members */
	table->stat_n_rows = 0;
	table->stat_clustered_index_size = 1;
	/* 1 page for each index, not counting the clustered */
	table->stat_sum_of_other_index_sizes
		= uint32_t(UT_LIST_GET_LEN(table->indexes) - 1);
	table->stat_modified_counter = 0;

	dict_index_t*	index;

	for (index = dict_table_get_first_index(table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {

		if (index->type & DICT_FTS) {
			continue;
		}

		ut_ad(!dict_index_is_ibuf(index));

		dict_stats_empty_index(index, empty_defrag_stats);
	}

	table->stat = table->stat | dict_table_t::STATS_INITIALIZED;
	table->stats_mutex_unlock();
}

/*********************************************************************//**
Check whether index's stats are initialized (assert if they are not). */
static
void
dict_stats_assert_initialized_index(
/*================================*/
	const dict_index_t*	index)	/*!< in: index */
{
	MEM_CHECK_DEFINED(
		index->stat_n_diff_key_vals,
		index->n_uniq * sizeof(index->stat_n_diff_key_vals[0]));

	MEM_CHECK_DEFINED(
		index->stat_n_sample_sizes,
		index->n_uniq * sizeof(index->stat_n_sample_sizes[0]));

	MEM_CHECK_DEFINED(
		index->stat_n_non_null_key_vals,
		index->n_uniq * sizeof(index->stat_n_non_null_key_vals[0]));

	MEM_CHECK_DEFINED(
		&index->stat_index_size,
		sizeof(index->stat_index_size));

	MEM_CHECK_DEFINED(
		&index->stat_n_leaf_pages,
		sizeof(index->stat_n_leaf_pages));
}

/*********************************************************************//**
Check whether table's stats are initialized (assert if they are not). */
static
void
dict_stats_assert_initialized(
/*==========================*/
	const dict_table_t*	table)	/*!< in: table */
{
	MEM_CHECK_DEFINED(&table->stats_last_recalc,
			  sizeof table->stats_last_recalc);

	MEM_CHECK_DEFINED(&table->stat, sizeof table->stat);

	MEM_CHECK_DEFINED(&table->stats_sample_pages,
			  sizeof table->stats_sample_pages);

	MEM_CHECK_DEFINED(&table->stat_n_rows,
			  sizeof table->stat_n_rows);

	MEM_CHECK_DEFINED(&table->stat_clustered_index_size,
			  sizeof table->stat_clustered_index_size);

	MEM_CHECK_DEFINED(&table->stat_sum_of_other_index_sizes,
			  sizeof table->stat_sum_of_other_index_sizes);

	MEM_CHECK_DEFINED(&table->stat_modified_counter,
			  sizeof table->stat_modified_counter);

	for (dict_index_t* index = dict_table_get_first_index(table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {

		if (!dict_stats_should_ignore_index(index)) {
			dict_stats_assert_initialized_index(index);
		}
	}
}

#define INDEX_EQ(i1, i2) \
	((i1) != NULL \
	 && (i2) != NULL \
	 && (i1)->id == (i2)->id \
	 && strcmp((i1)->name, (i2)->name) == 0)

/** Statistics for one field of an index. */
namespace {
struct IndexFieldStats
{
  uint64_t n_diff_key_vals;
  uint64_t n_sample_sizes;
  uint64_t n_non_null_key_vals;

  IndexFieldStats(uint64_t n_diff_key_vals= 0,
                  uint64_t n_sample_sizes= 0,
                  uint64_t n_non_null_key_vals= 0)
      : n_diff_key_vals(n_diff_key_vals), n_sample_sizes(n_sample_sizes),
        n_non_null_key_vals(n_non_null_key_vals)
  {
  }

  bool is_bulk_operation() const
  {
    return n_diff_key_vals == UINT64_MAX &&
      n_sample_sizes == UINT64_MAX && n_non_null_key_vals == UINT64_MAX;
  }
};
} // anonymous namespace

/** Record the number of non_null key values in a given index for
each column of the index where 1 <= n <= dict_index_get_n_unique(index).
The estimates are eventually stored in the array:
index->stat_n_non_null_key_vals[], which is indexed from 0 to n-1.
The counts are per column; innodb_rec_per_key() derives from them the
prefix-wise count that it needs.
@param n_unique   number of unique column for an index
@param offsets    offsets for all fields that of n_unique
@param n_not_null array to record number of non null rows for each individual column */
__attribute__((nonnull))
static void btr_record_not_null_field_in_rec(ulint n_unique,
                                             const rec_offs *offsets,
                                             uint64_t *n_not_null) noexcept
{
  ut_ad(rec_offs_n_fields(offsets) >= n_unique);
  for (ulint i = 0; i < n_unique; i++)
    n_not_null[i] += !rec_offs_nth_sql_null(offsets, i);
}

inline dberr_t
btr_cur_t::open_random_leaf(rec_offs *&offsets, mem_heap_t *&heap, mtr_t &mtr)
{
  ut_ad(!index()->is_spatial());
  ut_ad(!mtr.get_savepoint());

  mtr_s_lock_index(index(), &mtr);

  if (index()->page == FIL_NULL)
    return DB_CORRUPTION;

  dberr_t err;
  auto offset= index()->page;
  bool merge= false;
  ulint height= ULINT_UNDEFINED;

  while (buf_block_t *block=
         btr_block_get(*index(), offset, RW_S_LATCH, merge, &mtr, &err))
  {
    page_cur.block= block;

    if (height == ULINT_UNDEFINED)
    {
      height= btr_page_get_level(block->page.frame);
      if (height > BTR_MAX_LEVELS)
        return DB_CORRUPTION;

      if (height == 0)
        goto got_leaf;
    }

    if (height == 0)
    {
      mtr.rollback_to_savepoint(0, mtr.get_savepoint() - 1);
    got_leaf:
      page_cur.rec= page_get_infimum_rec(block->page.frame);
      return DB_SUCCESS;
    }

    if (!--height)
      merge= !index()->is_clust();

    page_cur_open_on_rnd_user_rec(&page_cur);

    offsets= rec_get_offsets(page_cur.rec, page_cur.index, offsets, 0,
                             ULINT_UNDEFINED, &heap);

    /* Go to the child node */
    offset= btr_node_ptr_get_child_page_no(page_cur.rec, offsets);
  }

  return err;
}

/** Estimated table level stats from sampled value.
@param value sampled stats
@param index index being sampled
@param sample number of sampled rows
@param ext_size external stored data size
@param not_empty table not empty
@return estimated table wide stats from sampled value */
#define BTR_TABLE_STATS_FROM_SAMPLE(value, index, sample, ext_size, not_empty) \
	(((value) * static_cast<uint64_t>(index->stat_n_leaf_pages) \
	  + (sample) - 1 + (ext_size) + (not_empty)) / ((sample) + (ext_size)))

/** Estimates the number of different key values in a given index, for
each n-column prefix of the index where 1 <= n <= dict_index_get_n_unique(index).
The estimates are stored in the array index->stat_n_diff_key_vals[] (indexed
0..n_uniq-1) and the number of pages that were sampled is saved in
result.n_sample_sizes[].
If innodb_stats_method is nulls_ignored, we also record the number of
non-null values for each prefix and stored the estimates in
array result.n_non_null_key_vals.
@param index          B-tree index
@param bulk_trx_id    the value of index->table->bulk_trx_id at the start
@return vector with statistics information
empty vector if the index is unavailable. */
static
std::vector<IndexFieldStats>
btr_estimate_number_of_different_key_vals(dict_index_t* index,
					  trx_id_t bulk_trx_id)
{
	page_t*		page;
	rec_t*		rec;
	ulint		n_cols;
	uint64_t*	n_diff;
	uint64_t*	n_not_null;
	bool		stats_null_not_equal;
	uint32_t	n_sample_pages = 1; /* number of pages to sample */
	ulint		not_empty_flag	= 0;
	uint32_t	n_blob_pages = 0;
	uintmax_t	add_on;
	mtr_t		mtr;
	mem_heap_t*	heap		= NULL;
	rec_offs*	offsets_rec	= NULL;
	rec_offs*	offsets_next_rec = NULL;

	std::vector<IndexFieldStats> result;

	ut_ad(index->is_btree());

	n_cols = dict_index_get_n_unique(index);

	heap = mem_heap_create((sizeof *n_diff + sizeof *n_not_null)
			       * n_cols
			       + dict_index_get_n_fields(index)
			       * (sizeof *offsets_rec
				  + sizeof *offsets_next_rec));

	n_diff = static_cast<uint64_t*>(mem_heap_zalloc(
		heap, n_cols * sizeof *n_diff));

	n_not_null = static_cast<uint64_t*>(mem_heap_zalloc(
		heap, n_cols * sizeof *n_not_null));

	/* Check srv_innodb_stats_method setting, and decide whether we
	need to record non-null value and also decide if NULL is
	considered equal (by setting stats_null_not_equal value) */
	switch (srv_innodb_stats_method) {
	case SRV_STATS_NULLS_IGNORED:
	case SRV_STATS_NULLS_UNEQUAL:
		/* for both SRV_STATS_NULLS_IGNORED and SRV_STATS_NULLS_UNEQUAL
		case, we will treat NULLs as unequal value */
		stats_null_not_equal = true;
		break;

	case SRV_STATS_NULLS_EQUAL:
		stats_null_not_equal = false;
		break;

	default:
		ut_error;
	}

	if (srv_stats_sample_traditional) {
		/* It makes no sense to test more pages than are contained
		in the index, thus we lower the number if it is too high */
		if (srv_stats_transient_sample_pages > index->stat_index_size) {
			if (index->stat_index_size > 0) {
				n_sample_pages = index->stat_index_size;
			}
		} else {
			n_sample_pages = srv_stats_transient_sample_pages;
		}
	} else {
		/* New logaritmic number of pages that are estimated.
		Number of pages estimated should be between 1 and
		index->stat_index_size.

		If we have only 0 or 1 index pages then we can only take 1
		sample. We have already initialized n_sample_pages to 1.

		So taking index size as I and sample as S and log(I)*S as L

		requirement 1) we want the out limit of the expression to not exceed I;
		requirement 2) we want the ideal pages to be at least S;
		so the current expression is min(I, max( min(S,I), L)

		looking for simplifications:

		case 1: assume S < I
		min(I, max( min(S,I), L) -> min(I , max( S, L))

		but since L=LOG2(I)*S and log2(I) >=1   L>S always so max(S,L) = L.

		so we have: min(I , L)

		case 2: assume I < S
		    min(I, max( min(S,I), L) -> min(I, max( I, L))

		case 2a: L > I
		    min(I, max( I, L)) -> min(I, L) -> I

		case 2b: when L < I
		    min(I, max( I, L))  ->  min(I, I ) -> I

		so taking all case2 paths is I, our expression is:
		n_pages = S < I? min(I,L) : I
		*/
		if (uint32_t I = index->stat_index_size) {
			const uint32_t S{srv_stats_transient_sample_pages};
			n_sample_pages = S < I
				? std::min(I,
					   uint32_t(log2(double(I))
						    * double(S)))
				: I;
		}
	}

	/* Sanity check */
	ut_ad(n_sample_pages);
	ut_ad(n_sample_pages <= (index->stat_index_size <= 1
				 ? 1 : index->stat_index_size));

	/* We sample some pages in the index to get an estimate */
	btr_cur_t cursor;
	cursor.page_cur.index = index;

	for (ulint i = 0; i < n_sample_pages; i++) {
		mtr.start();

		if (cursor.open_random_leaf(offsets_rec, heap, mtr) !=
                    DB_SUCCESS
		    || index->table->bulk_trx_id != bulk_trx_id) {
			mtr.commit();
			goto exit_loop;
		}

		/* Count the number of different key values for each prefix of
		the key on this index page. If the prefix does not determine
		the index record uniquely in the B-tree, then we subtract one
		because otherwise our algorithm would give a wrong estimate
		for an index where there is just one key value. */

		page = btr_cur_get_page(&cursor);

		rec = page_rec_get_next(cursor.page_cur.rec);
		const ulint n_core = index->n_core_fields;

		if (rec && rec != page_get_supremum_rec(page)) {
			not_empty_flag = 1;
			offsets_rec = rec_get_offsets(rec, index, offsets_rec,
						      n_core,
						      ULINT_UNDEFINED, &heap);

			btr_record_not_null_field_in_rec(
				n_cols, offsets_rec, n_not_null);
		}

		while (rec != page_get_supremum_rec(page)) {
			ulint	matched_fields;
			rec_t*	next_rec = page_rec_get_next(rec);
			if (!next_rec
			    || next_rec == page_get_supremum_rec(page)) {
				n_blob_pages += rec_get_n_blob_pages(
					rec, offsets_rec, index->table);
				break;
			}

			offsets_next_rec = rec_get_offsets(next_rec, index,
							   offsets_next_rec,
							   n_core,
							   ULINT_UNDEFINED,
							   &heap);

			cmp_rec_rec(rec, next_rec,
				    offsets_rec, offsets_next_rec,
				    index, stats_null_not_equal,
				    &matched_fields);

			for (ulint j = matched_fields; j < n_cols; j++) {
				/* We add one if this index record has
				a different prefix from the previous */

				n_diff[j]++;
			}

			btr_record_not_null_field_in_rec(
				n_cols, offsets_next_rec, n_not_null);

			n_blob_pages += rec_get_n_blob_pages(
				rec, offsets_rec, index->table);

			rec = next_rec;
			/* Initialize offsets_rec for the next round
			and assign the old offsets_rec buffer to
			offsets_next_rec. */
			{
				rec_offs* offsets_tmp = offsets_rec;
				offsets_rec = offsets_next_rec;
				offsets_next_rec = offsets_tmp;
			}
		}

		if (n_cols == dict_index_get_n_unique_in_tree(index)
		    && page_has_siblings(page)) {

			/* If there is more than one leaf page in the tree,
			we add one because we know that the first record
			on the page certainly had a different prefix than the
			last record on the previous index page in the
			alphabetical order. Before this fix, if there was
			just one big record on each clustered index page, the
			algorithm grossly underestimated the number of rows
			in the table. */

			n_diff[n_cols - 1]++;
		}

		mtr.commit();
	}

exit_loop:
	/* If we saw k borders between different key values on
	n_sample_pages leaf pages, we can estimate how many
	there will be in index->stat_n_leaf_pages */

	/* We must take into account that our sample actually represents
	also the pages used for external storage of fields (those pages are
	included in index->stat_n_leaf_pages) */

	result.reserve(n_cols);

	for (ulint j = 0; j < n_cols; j++) {
		IndexFieldStats stat;

		stat.n_diff_key_vals
			= BTR_TABLE_STATS_FROM_SAMPLE(
				n_diff[j], index, n_sample_pages,
				n_blob_pages, not_empty_flag);

		/* If the tree is small, smaller than
		10 * (n_sample_pages + n_blob_pages), then
		the above estimate is ok. For bigger trees it is common that we
		do not see any borders between key values in the few pages
		we pick. But still there may be n_sample_pages
		different key values, or even more. Let us try to approximate
		that: */

		add_on = index->stat_n_leaf_pages
			/ (10 * (n_sample_pages
				 + n_blob_pages));

		if (add_on > n_sample_pages) {
			add_on = n_sample_pages;
		}

		stat.n_diff_key_vals += add_on;

		stat.n_sample_sizes = n_sample_pages;

		stat.n_non_null_key_vals =
			 BTR_TABLE_STATS_FROM_SAMPLE(
				n_not_null[j], index, n_sample_pages,
				n_blob_pages, not_empty_flag);

		result.push_back(stat);
	}

	mem_heap_free(heap);
	return result;
}

/*********************************************************************//**
Calculates new estimates for index statistics. This function is
relatively quick and is used to calculate transient statistics that
are not saved on disk. This was the only way to calculate statistics
before the Persistent Statistics feature was introduced.
This function doesn't update the defragmentation related stats.
Only persistent statistics supports defragmentation stats.
@return error code
@retval DB_SUCCESS_LOCKED_REC if the table under bulk insert operation */
static
dberr_t
dict_stats_update_transient_for_index(
/*==================================*/
	dict_index_t*	index)	/*!< in/out: index */
{
	dberr_t err = DB_SUCCESS;
	if (srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO
	    && (srv_force_recovery >= SRV_FORCE_NO_LOG_REDO
		|| !dict_index_is_clust(index))) {
		/* If we have set a high innodb_force_recovery
		level, do not calculate statistics, as a badly
		corrupted index can cause a crash in it.
		Initialize some bogus index cardinality
		statistics, so that the data can be queried in
		various means, also via secondary indexes. */
dummy_empty:
		index->table->stats_mutex_lock();
		dict_stats_empty_index(index, false);
		index->table->stats_mutex_unlock();
		return err;
#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
	} else if (ibuf_debug && !dict_index_is_clust(index)) {
		goto dummy_empty;
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */
	} else if (dict_index_is_online_ddl(index) || !index->is_committed()
		   || !index->is_btree()
		   || !index->table->space) {
		goto dummy_empty;
	} else {
		mtr_t	mtr;

		mtr.start();
		mtr_sx_lock_index(index, &mtr);

		dberr_t err;
		buf_block_t* root = btr_root_block_get(index, RW_SX_LATCH,
						       &mtr, &err);
		if (!root) {
invalid:
			mtr.commit();
			goto dummy_empty;
		}

		const auto bulk_trx_id = index->table->bulk_trx_id;
		if (trx_sys.is_registered(nullptr, bulk_trx_id)) {
			err= DB_SUCCESS_LOCKED_REC;
			goto invalid;
		}

		mtr.x_lock_space(index->table->space);

		uint32_t dummy, size;
		index->stat_index_size
			= fseg_n_reserved_pages(*root, PAGE_HEADER
						+ PAGE_BTR_SEG_LEAF
						+ root->page.frame, &size,
						&mtr)
			+ fseg_n_reserved_pages(*root, PAGE_HEADER
						+ PAGE_BTR_SEG_TOP
						+ root->page.frame, &dummy,
						&mtr);

		mtr.commit();

		index->stat_n_leaf_pages = size ? size : 1;

		/* Do not continue if table decryption has failed or
		table is already marked as corrupted. */
		if (index->is_readable()) {
			std::vector<IndexFieldStats> stats
				= btr_estimate_number_of_different_key_vals(
					index, bulk_trx_id);

			if (!stats.empty()) {
				index->table->stats_mutex_lock();
				for (size_t i = 0; i < stats.size(); ++i) {
					index->stat_n_diff_key_vals[i]
						= stats[i].n_diff_key_vals;
					index->stat_n_sample_sizes[i]
						= stats[i].n_sample_sizes;
					index->stat_n_non_null_key_vals[i]
						= stats[i].n_non_null_key_vals;
				}
				index->table->stats_mutex_unlock();
			}
		}
	}

	return err;
}

dberr_t dict_stats_update_transient(dict_table_t *table) noexcept
{
	ut_ad(!table->stats_mutex_is_owner());

	dict_index_t*	index;
	uint32_t	sum_of_index_sizes	= 0;
	dberr_t		err = DB_SUCCESS;

	/* Find out the sizes of the indexes and how many different values
	for the key they approximately have */

	index = dict_table_get_first_index(table);

	if (!index || !table->space) {
		dict_stats_empty_table(table, true);
		return DB_SUCCESS;
	}

	if (trx_sys.is_registered(nullptr, table->bulk_trx_id)) {
		dict_stats_empty_table(table, false);
		return DB_SUCCESS_LOCKED_REC;
	}

	for (; index != NULL; index = dict_table_get_next_index(index)) {

		ut_ad(!dict_index_is_ibuf(index));

		if (!index->is_btree()) {
			continue;
		}

		if (dict_stats_should_ignore_index(index)
		    || !index->is_readable()
		    || err == DB_SUCCESS_LOCKED_REC) {
			index->table->stats_mutex_lock();
			dict_stats_empty_index(index, false);
			index->table->stats_mutex_unlock();
			continue;
		}

		err = dict_stats_update_transient_for_index(index);

		sum_of_index_sizes += index->stat_index_size;
	}

	table->stats_mutex_lock();

	index = dict_table_get_first_index(table);

	table->stat_n_rows = index->stat_n_diff_key_vals[
		dict_index_get_n_unique(index) - 1];

	table->stat_clustered_index_size = index->stat_index_size;

	table->stat_sum_of_other_index_sizes = sum_of_index_sizes
		- index->stat_index_size;

	table->stats_last_recalc = time(NULL);

	table->stat_modified_counter = 0;

	table->stat = table->stat | dict_table_t::STATS_INITIALIZED;

	table->stats_mutex_unlock();

	return err;
}

/** Open a cursor at the first page in a tree level.
@param page_cur  cursor
@param level     level to search for (0=leaf)
@param mtr       mini-transaction */
static dberr_t page_cur_open_level(page_cur_t *page_cur, ulint level,
                                   mtr_t *mtr)
{
  mem_heap_t *heap= nullptr;
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs *offsets= offsets_;
  dberr_t err;

  dict_index_t *const index= page_cur->index;

  rec_offs_init(offsets_);
  ut_ad(level != ULINT_UNDEFINED);
  ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_SX_LOCK));
  ut_ad(mtr->get_savepoint() == 1);

  uint32_t page= index->page;

  for (ulint height = ULINT_UNDEFINED;; height--)
  {
    buf_block_t* block=
      btr_block_get(*index, page, RW_S_LATCH,
                    !height && !index->is_clust(), mtr, &err);
    if (!block)
      break;

    const uint32_t l= btr_page_get_level(block->page.frame);

    if (height == ULINT_UNDEFINED)
    {
      ut_ad(!heap);
      /* We are in the root node */
      height= l;
      if (UNIV_UNLIKELY(height < level))
        return DB_CORRUPTION;
    }
    else if (UNIV_UNLIKELY(height != l) || page_has_prev(block->page.frame))
    {
      err= DB_CORRUPTION;
      break;
    }

    page_cur_set_before_first(block, page_cur);

    if (height == level)
      break;

    ut_ad(height);

    if (!page_cur_move_to_next(page_cur))
    {
      err= DB_CORRUPTION;
      break;
    }

    offsets= rec_get_offsets(page_cur->rec, index, offsets, 0, ULINT_UNDEFINED,
                             &heap);
    page= btr_node_ptr_get_child_page_no(page_cur->rec, offsets);
  }

  if (UNIV_LIKELY_NULL(heap))
    mem_heap_free(heap);

  /* Release all page latches except the one on the desired page. */
  const auto end= mtr->get_savepoint();
  if (end > 1)
    mtr->rollback_to_savepoint(1, end - 1);

  return err;
}

/** Open a cursor at the first page in a tree level.
@param page_cur  cursor
@param level     level to search for (0=leaf)
@param mtr       mini-transaction
@param index     index tree */
static dberr_t btr_pcur_open_level(btr_pcur_t *pcur, ulint level, mtr_t *mtr,
                                   dict_index_t *index)
{
  pcur->latch_mode= BTR_SEARCH_LEAF;
  pcur->search_mode= PAGE_CUR_G;
  pcur->pos_state= BTR_PCUR_IS_POSITIONED;
  pcur->btr_cur.page_cur.index= index;
  return page_cur_open_level(&pcur->btr_cur.page_cur, level, mtr);
}


namespace {

/** Input data that is used to calculate dict_index_t::stat_n_diff_key_vals[]
for each n-columns prefix (n from 1 to n_uniq). */
struct NDiffData {
	/** Index of the level on which the descent through the btree
	stopped. level 0 is the leaf level. This is >= 1 because we
	avoid scanning the leaf level because it may contain too many
	pages and doing so is useless when combined with the random dives -
	if we are to scan the leaf level, this means a full scan and we can
	simply do that instead of fiddling with picking random records higher
	in the tree and to dive below them. At the start of the analyzing
	we may decide to do full scan of the leaf level, but then this
	structure is not used in that code path. */
	ulint		level;

	/** Number of records on the level where the descend through the btree
	stopped. When we scan the btree from the root, we stop at some mid
	level, choose some records from it and dive below them towards a leaf
	page to analyze. */
	uint64_t	n_recs_on_level;

	/** Number of different key values that were found on the mid level. */
	uint64_t	n_diff_on_level;

	/** Number of leaf pages that are analyzed. This is also the same as
	the number of records that we pick from the mid level and dive below
	them. */
	uint32_t	n_analyze_leaf_pages;

	/** Cumulative sum of the number of external pages (stored outside of
	the btree but in the same file segment). */
	uint32_t	n_external_pages;

	/** Cumulative sum of the number of different key values that were
	found on all analyzed pages. */
	uint64_t	n_diff_total;

	/** Cumulative sum of the number of non-null key values that
	 were found on all analyzed pages. */
	uint64_t 	n_non_null_total;
};

/** Statistics collected for a single B-tree level
during index analysis. Used to track distinct key values,
record counts, and page information when analyzing index
levels for the query optimizer's statistics. */
struct IndexLevelStats
{
  /** Whether nulls are considered unequal for statistics */
  const bool nulls_unequal;
  /** distance from the leaf level (0). */
  uint16_t level;
  /** Array for number of distinct keys */
  uint64_t *const n_diff;
  /** Array of the number of non-null values of each individual
  column, or nullptr when the level being scanned is not the leaf
  level */
  uint64_t *const n_non_null_per_col;
  /** Boundaries of groups of distinct keys */
  Boundaries *const bounds;
  /** Total number of records */
  uint64_t n_recs;
  /** Index */
  dict_index_t *const index;
  /** Mini-transaction */
  mtr_t *const mtr;

  IndexLevelStats(bool nulls_unequal_, uint16_t level_,
		  uint64_t *n_diff_, uint64_t *n_non_null_per_col_,
		  Boundaries *bounds_, dict_index_t *index_,
		  mtr_t *mtr_) :
    nulls_unequal(nulls_unequal_), level(level_),
    n_diff(n_diff_), n_non_null_per_col(n_non_null_per_col_), bounds(bounds_),
    n_recs(!!level_), index(index_), mtr(mtr_)
  {
  }

  /** Reset statistics for scanning a new level
  @param  level_  new level number to scan */
  void reset_for_level(uint16_t level_)
  {
    level= level_;
    n_recs= 0;
    auto n= index->n_uniq;
    memset(n_diff, 0, n * sizeof *n_diff);
    for (auto i= n; i--; )
      bounds[i].clear();
  }

  /** Full-scan one B-tree level in a single mtr.
  For every n-prefix (1..n_uniq) compute
  n_diff[] (= N_DIFF_LA, distinct keys on the level) and
  n_recs (= TOTAL_LA, records on the level), and
  record in bounds[] the index of the last record of each group of equal keys
  (0-based, counted continuously across pages). These boundaries
  let sample_leaf_pages() split the level into groups and pick one
  representative record per group.
  @return number of pages in this level */
  uint32_t analyze_level() noexcept;

  /** Estimate the number of different key values in an index when looking at
  the first n_prefix columns. Using the group boundaries from analyze_level(),
  split level LA into n_analyze_leaf_pages (<= A) groups of equal keys, pick
  the last record of each group and dive below it (scan_below()). Accumulate over
  the samples:
  n_diff_total (= sum of Pi), n_non_null_total and
  n_external_pages (= E, BLOB pages).
  @param[in]	 n_prefix    look at first 'n_prefix' columns when comparing records
  @param[in,out] n_diff_data n_diff_total and n_external_pages
  in this structure will be set by this function.
  The members level, n_diff_on_level and n_analyze_leaf_pages
  must be set by the caller in advance. They are used by
  some calculations inside this function */
  void sample_leaf_pages(uint16_t n_prefix,
                         NDiffData *n_diff_data)const noexcept;
};

/** Statistics collected for a leaf level during index
analysis. Used to calculate the number of different key values,
number of blob pages, number of non-null values in an index
when looking at the first n_prefix columns. This will be
initialized in dict_stats_scan_page() */
struct PageStats
{
  /** Collecting statistics for this Index */
  const dict_index_t *index;
  /** Number of columns to consider for prefix statistics */
  const uint8_t n_prefix;
  /** Whether NULL values are considered unequal for statistics */
  const bool nulls_unequal;
  /** Number of externally stored blob pages encountered in scan<true> */
  uint32_t n_blob= 0;
  /** Number of distinct key values found for the first n_prefix columns.
  On leaf pages this is the full count; on non-leaf pages it is capped at 2
  (0 = empty, 1 = boring/all-equal, 2 = non-boring). see scan(). */
  uint64_t n_diff= 0;
  /** Number of non-null key values found for the prefix in scan<true>
  for the prefix */
  uint64_t n_non_null= 0;

  rec_offs *offsets1= nullptr;

  rec_offs *offsets2= nullptr;

  mem_heap_t *heap= nullptr;

  PageStats(dict_index_t *ind, uint8_t n_pfx, bool nulls) :
    index(ind), n_prefix(n_pfx), nulls_unequal(nulls)
  {
    ulint size= (1 + REC_OFFS_HEADER_SIZE) + 1 + index->n_fields;
    heap= mem_heap_create(size * (2 * sizeof (rec_offs)));
    offsets1= static_cast<rec_offs*>(
      mem_heap_alloc(heap, size * sizeof (*offsets1)));
    offsets2= static_cast<rec_offs*>(
      mem_heap_alloc(heap, size * sizeof (*offsets2)));
    rec_offs_set_n_alloc(offsets1, size);
    rec_offs_set_n_alloc(offsets2, size);
  }

  void scan_below(const btr_cur_t *cur) noexcept;

  uint32_t scan(const page_t *page, bool leaf= true) noexcept;

  ~PageStats() { mem_heap_free(heap); }
};

} // anonymous namespace

uint32_t IndexLevelStats::analyze_level() noexcept
{
	mem_heap_t*	heap;
	btr_pcur_t	pcur;
	const page_t*	page;
	const rec_t*	rec;
	const rec_t*	prev_rec;
	rec_offs*	rec_offsets;
	rec_offs*	prev_rec_offsets;
	/* mtr savepoint of the page that contains prev_rec, in case
	that page is not the one the cursor is positioned on;
	0 (the savepoint of index->lock) if there is no such page.
	Instead of copying the last record of a page, retain the
	latch on that page until the record has been compared with the
	first record of the next page. */
	ulint		prev_savepoint = 0;
	/* Number of pages on this level. It is counted at the end of
	each page, and is returned to the caller. */
	uint32_t n_pages = 0;

	/* Allocate space for the offsets header (the allocation size at
	offsets[0] and the REC_OFFS_HEADER_SIZE bytes), and n_uniq + 1,
	so that this will never be less than the size calculated in
	rec_get_offsets_func(). */
	ulint i = (REC_OFFS_HEADER_SIZE + 1 + 1) + index->n_uniq;

	heap = mem_heap_create((2 * sizeof *rec_offsets) * i);
	rec_offsets = static_cast<rec_offs*>(
		mem_heap_alloc(heap, i * sizeof *rec_offsets));
	prev_rec_offsets = static_cast<rec_offs*>(
		mem_heap_alloc(heap, i * sizeof *prev_rec_offsets));
	rec_offs_set_n_alloc(rec_offsets, i);
	rec_offs_set_n_alloc(prev_rec_offsets, i);

	/* Position pcur on the leftmost record on the leftmost page
	on the desired level. */

	if (btr_pcur_open_level(&pcur, level, mtr, index)
	    || !btr_pcur_move_to_next_on_page(&pcur)) {
		goto func_exit;
	}

	page = btr_pcur_get_page(&pcur);

	/* The page must not be empty, except when
	it is the root page (and the whole index is empty). */
	ut_ad(btr_pcur_is_on_user_rec(&pcur) || page_is_leaf(page));

	prev_rec = NULL;

	if (REC_INFO_MIN_REC_FLAG & rec_get_info_bits(
		    btr_pcur_get_rec(&pcur), page_is_comp(page))) {
		ut_ad(btr_pcur_is_on_user_rec(&pcur));
		if (level == 0) {
			/* Skip the metadata pseudo-record */
			ut_ad(index->is_instant());
			btr_pcur_move_to_next_user_rec(&pcur, mtr);
		}
	} else if (UNIV_UNLIKELY(level != 0)) {
		/* The first record on the leftmost page must be
		marked as such on each level except the leaf level. */
		goto func_exit;
	}

	/* iterate over all user records on this level
	and compare each two adjacent ones, even the last on page
	X and the fist on page X+1 */
	while (btr_pcur_is_on_user_rec(&pcur)) {
		bool	rec_is_last_on_page;

		page = btr_pcur_get_page(&pcur);
		rec = btr_pcur_get_rec(&pcur);

		/* If rec and prev_rec are on different pages, then we
		must still be holding the latch on the page of prev_rec. */
		ut_ad(!prev_rec || prev_savepoint
		      || page == page_align(prev_rec));
		ut_ad(!prev_savepoint || page != page_align(prev_rec));

		rec_is_last_on_page = page_rec_get_next_const(rec)
			== page_get_supremum_rec(page);

		/* increment the pages counter at the end of each page */
		if (rec_is_last_on_page) {
			n_pages++;
		}

		/* Skip delete-marked records on the leaf level. If we
		do not skip them, then ANALYZE quickly after DELETE
		could count them or not (purge may have already wiped
		them away) which brings non-determinism. We skip only
		leaf-level delete marks because delete marks on
		non-leaf level do not make sense. */

		if (level == 0
		    && !srv_stats_include_delete_marked
		    && rec_get_deleted_flag(rec, page_is_comp(page))) {
			/* The latch on the page of prev_rec must be
			retained until a record that is not skipped is
			found, even if that record is on a later page. */
			goto next_rec;
		}

		rec_offsets = rec_get_offsets(rec, index, rec_offsets,
					      level ? 0 : index->n_core_fields,
					      index->n_uniq, &heap);

		n_recs++;

		if (n_non_null_per_col) {
			ut_ad(level == 0);
			for (ulint i = 0; i < index->n_uniq; i++) {
				n_non_null_per_col[i]
					+= index->fields[i].col->is_nullable()
					&& !rec_offs_nth_sql_null(
						rec_offsets, i);
			}
		}

		if (prev_rec != NULL) {
			ulint	matched_fields;

			prev_rec_offsets = rec_get_offsets(
				prev_rec, index, prev_rec_offsets,
				level ? 0 : index->n_core_fields,
				index->n_uniq, &heap);

			cmp_rec_rec(prev_rec, rec,
				    prev_rec_offsets, rec_offsets, index,
				    nulls_unequal,
				    &matched_fields);

			if (bounds) {
				for (i = matched_fields;
				     i < index->n_uniq; i++) {
					/* push the index of the previous
					record, that is - the last one from
					a group of equal keys */

					/* the index of the current record
					is n_recs - 1, the index of the
					previous record is n_recs - 2;
					we know that idx is not going to
					become negative here because if we
					are in this branch then there is a
					previous record and thus
					n_recs >= 2 */
					bounds[i].emplace_back(n_recs - 2);
				}
			}

			for (i = matched_fields; i < index->n_uniq; i++) {
				/* increment the number of different keys
				for n_prefix=i+1 (e.g. if i=0 then we increment
				for n_prefix=1 which is stored in n_diff[0]) */
				n_diff[i]++;
			}
		} else {
			/* this is the first non-delete marked record */
			for (i = 0; i < index->n_uniq; i++) {
				n_diff[i] = 1;
			}
		}

		/* prev_rec has been compared with rec; if prev_rec was on
		a preceding page, that page can be released now. */
		if (prev_savepoint) {
			mtr->rollback_to_savepoint(prev_savepoint,
						   prev_savepoint + 1);
			prev_savepoint = 0;
		}

		prev_rec = rec;
next_rec:
		if (!rec_is_last_on_page) {
			/* still on the same page; moving to the next
			record will not release any page latch */
			if (UNIV_UNLIKELY(!btr_pcur_move_to_next_on_page(
						  &pcur))) {
				break;
			}
			continue;
		}

		/* The end of a page has been reached. Position the cursor
		on the supremum record, so that we can move to the next
		page. */
		if (UNIV_UNLIKELY(!btr_pcur_move_to_next_on_page(&pcur))) {
			break;
		}

		for (;;) {
			/* Instead of copying prev_rec, retain the latch on
			the page that it resides on, so that it can be
			compared with the first record of the next page.
			The latch will be released once that comparison has
			been made. */
			const bool retain_latch = prev_rec
				&& page_align(prev_rec)
				   == btr_pcur_get_page(&pcur);

			ut_ad(btr_pcur_is_after_last_on_page(&pcur));

			if (btr_pcur_is_after_last_in_tree(&pcur)
			    || btr_pcur_move_to_next_page(&pcur, mtr,
							  retain_latch)
			       != DB_SUCCESS) {
				goto end_of_level;
			}

			if (retain_latch) {
				prev_savepoint = mtr->get_savepoint() - 2;
			}

			if (UNIV_UNLIKELY(!btr_pcur_move_to_next_on_page(
							&pcur))) {
				goto end_of_level;
			}

			if (btr_pcur_is_on_user_rec(&pcur)) {
				break;
			}
		}
	}

end_of_level:
	/* if n_pages is left untouched then
	above loop was not entered at all and there is one page
	in the whole tree which is empty or the loop was entered
	but this is level 0, contains one page and all records
	are delete-marked */
	if (n_pages == 0) {
		ut_ad(level == 0);
		ut_ad(n_recs == 0);
		/* Nothing was counted: there are no boundaries to save,
		and the zero counts are already in place, because
		dict_stats_analyze_index() zero-initialized the arrays
		before starting this analysis. */
		n_pages = 1;
	} else {
		if (n_recs > 0 && bounds != NULL) {
			/* there are records on this level and boundaries
			should be saved: remember the index of the last
			record on the level as the last one from the last
			group of equal keys; this holds for all possible
			prefixes */
			for (ulint i = 0; i < index->n_uniq; i++) {
				bounds[i].emplace_back(n_recs - 1);
			}
		}

		if (level == 0) {
			for (ulint i = 0; i < index->n_uniq; i++) {
				if (!index->fields[i].col->is_nullable()) {
					n_non_null_per_col[i]= n_recs;
				}
			}
		}
	}
func_exit:
	mem_heap_free(heap);
	return n_pages;
}

/************************************************************//**
Gets the pointer to the next non delete-marked record on the page.
If all subsequent records are delete-marked, then this function
will return the supremum record.
@return pointer to next non delete-marked record or pointer to supremum */
template<bool comp>
static
const rec_t*
page_rec_get_next_non_del_marked(const page_t *page, const rec_t *rec)
{
  ut_ad(!!page_is_comp(page) == comp);
  ut_ad(page_align(rec) == page);

  for (rec= page_rec_next_get<comp>(page, rec);
       rec && rec_get_deleted_flag(rec, comp);
       rec= page_rec_next_get<comp>(page, rec));
  return rec ? rec : page + (comp ? PAGE_NEW_SUPREMUM : PAGE_OLD_SUPREMUM);
}

/** Scan a page left to right, counting distinct records for the first n_prefix
columns (and, on leaf pages, non-null values and external/BLOB pages).

An "n-prefix-boring" record is a non-leaf record equal to the next record to
its right on the same level (crossing pages, skipping infimum/supremum) in the
first n_prefix columns; the last user record on a level is never boring. Every
record in the subtree below a boring record is equal to it, so that subtree
contributes exactly 1 distinct key which is why scan_below() never descends
through boring records.
@tparam leaf   true  = leaf scan: count ALL distinct keys (per nulls_unequal),
                       non-null values and BLOB page counts;
               false = non-leaf scan: quick boring-detection that stops at the
                       first non-boring record, so n_diff ends as 0 (empty),
                       1 (all keys equal / boring page) or 2 (non-boring found)
@param  page   index page to scan
@retval 0 if leaf=true OR if page is empty
@retval child_page_no if leaf=false and page contains records */
uint32_t PageStats::scan(const page_t *page, const bool leaf)noexcept
{
	rec_offs*	offsets_rec		= offsets1;
	rec_offs*	offsets_next_rec	= offsets2;
	const rec_t*	next_rec;
	const ulint	n_core = leaf ? index->n_core_fields : 0;
	ut_ad(!!n_core == page_is_leaf(page));
	ulint n_recs = 1;
	const rec_t*	(*get_next)(const page_t*, const rec_t*)
		= !n_core || srv_stats_include_delete_marked
		? (page_is_comp(page)
		   ? page_rec_next_get<true> : page_rec_next_get<false>)
		: page_is_comp(page)
		  ? page_rec_get_next_non_del_marked<true>
		  : page_rec_get_next_non_del_marked<false>;
	const rec_t *rec = get_next(page, page_get_infimum_rec(page));
	ut_ad(!n_blob);
	if (!rec || rec == page_get_supremum_rec(page)) {
		/* the page is empty or contains only delete-marked records */
		n_diff = 0;
		return 0;
	}
	const bool nullable = index->fields[n_prefix - 1].col->is_nullable();
	offsets_rec = rec_get_offsets(rec, index, offsets_rec, n_core,
				      ULINT_UNDEFINED, &heap);
	if (leaf) {
		n_blob += rec_get_n_blob_pages(rec, offsets_rec, index->table);
		n_non_null += static_cast<uint64_t>(
		  nullable && !rec_offs_nth_sql_null(offsets_rec,
						     n_prefix - 1));
	}

	next_rec = get_next(page, rec);
	n_diff = 1;
	while (next_rec && !page_rec_is_supremum_low(next_rec - page)) {
		ulint	matched_fields;
		n_recs++;
		offsets_next_rec = rec_get_offsets(next_rec, index, offsets_next_rec,
						   n_core, ULINT_UNDEFINED, &heap);

		/* check whether rec != next_rec when looking at the first
		n_prefix fields */
		cmp_rec_rec(rec, next_rec, offsets_rec, offsets_next_rec,
			    index, nulls_unequal, &matched_fields);
		if (matched_fields < n_prefix) {
			/* rec != next_rec, => rec is non-boring */
			n_diff++;
			if (!n_core) {
				break;
			}
		}

		rec = next_rec;
		/* Assign offsets_rec = offsets_next_rec so that offsets_rec matches
		with rec which was just assigned rec = next_rec above.  Also need to
		point offsets_next_rec to the place where offsets_rec was pointing
		before because we have just 2 placeholders where data is actually
		stored: offsets1 and offsets2 and we are using them in circular fashion
		(offsets[_next]_rec are just pointers to those placeholders). */
		std::swap(offsets_rec, offsets_next_rec);

		if (leaf) {
			n_blob += rec_get_n_blob_pages(rec, offsets_rec,
						       index->table);
			n_non_null += nullable &&
				      !rec_offs_nth_sql_null(offsets_rec,
							     n_prefix - 1);
		}
		next_rec = get_next(page, next_rec);
	}

	if (leaf) {
		n_non_null = nullable ? n_non_null : n_recs;
		return 0;
	}

	return btr_node_ptr_get_child_page_no(rec, offsets_rec);
}

/** Descend from a record on level LA to a leaf page and scan it.
At each non-leaf level follow a non-boring child (a boring subtree is all-equal
and would only add 1 distinct key); stop early if the page is boring
(n_diff == 1), since no dive is needed. On the reached leaf, scan<true>()
accumulates Pi (distinct records for the first n_prefix columns), non-null
counts, and the external/BLOB pages pointed to by records on that leaf. */
void PageStats::scan_below(const btr_cur_t *cur) noexcept
{
	/* Allocate offsets for the record and the node pointer, for
	node pointer records. In a secondary index, the node pointer
	record will consist of all index fields followed by a child
	page number.
	Allocate space for the offsets header (the allocation size at
	offsets[0] and the REC_OFFS_HEADER_SIZE bytes), and n_fields + 1,
	so that this will never be less than the size calculated in
	rec_get_offsets_func(). */
	rec_t*		rec = btr_cur_get_rec(cur);
	const page_t*	page = btr_cur_get_page(cur);
	ut_ad(!page_is_leaf(page));
	rec_offs*	offsets_rec = rec_get_offsets(rec, index, offsets1, 0,
						  ULINT_UNDEFINED, &heap);
	page_id_t	page_id(index->table->space_id,
				btr_node_ptr_get_child_page_no(rec, offsets_rec));
	const ulint	zip_size = index->table->space->zip_size();
	const bool	may_ibuf_exist = !index->is_clust() && !index->is_unique();
	mtr_t		mtr;
	mtr_start(&mtr);

	/* descend to the leaf level on the B-tree */
	for (;;) {
		dberr_t		err;
		buf_block_t*	block = buf_page_get_gen(
			page_id, zip_size, RW_S_LATCH, nullptr,
			BUF_GET, &mtr, &err,
			may_ibuf_exist && 1 == btr_page_get_level(page));
		if (!block) {
			mtr_commit(&mtr);
			return;
		}

		page = block->page.frame;

		if (page_is_leaf(page)) {
			/* leaf level */
			break;
		}

		/* search for the first non-boring record on the page */
		uint32_t child_page_no = scan(page, false);

		/* level > 0, so child page number shouldn't be 0 */
		ut_a(child_page_no != 0);
		/* if page is not empty (offsets_rec != NULL) then n_diff must
		be > 0, otherwise there is a bug in dict_stats_scan_page() */
		ut_a(n_diff > 0);

		if (n_diff == 1) {
			mtr_commit(&mtr);
			/* page has all keys equal and the end of the page was reached by
			dict_stats_scan_page(), no need to descend to the leaf level */
			/* can't get an estimate for n_external_pages here because we
			do not dive to the leaf level, assume no external pages
			(*n_external_pages was assigned to 0 above). */
			return;
		}

		/* when we instruct dict_stats_scan_page() to quit on the
		first non-boring record it finds, then the returned n_diff
		can either be 0 (empty page), 1 (page has all keys equal) or
		2 (non-boring record was found) */
		ut_a(n_diff == 2);
		/* we have a non-boring record in rec, descend below it */
		page_id.set_page_no(child_page_no);
	}

	/* make sure we got a leaf page as a result from the above loop */
	ut_ad(page_is_leaf(page));
	/* scan the leaf page and find the number of distinct keys, when looking
	only at the first n_prefix columns; also estimate the number of externally
	stored pages pointed by records on this page */
	scan(page);

	mtr_commit(&mtr);
}

void
IndexLevelStats::sample_leaf_pages(
	uint16_t		n_prefix,
	NDiffData*		n_diff_data)const noexcept
{
	btr_pcur_t	pcur;
	const page_t*	page;
	uint64_t	rec_idx;
	uint64_t	i;

	ut_ad(n_diff_data->level);

	if (bounds[n_prefix - 1].empty()) {
		return;
	}

	/* Position pcur on the leftmost record on the leftmost page
	on the desired level. */

	if (btr_pcur_open_level(&pcur, n_diff_data->level, mtr, index)
	    != DB_SUCCESS
	    || !btr_pcur_move_to_next_on_page(&pcur)) {
		return;
	}

	page = btr_pcur_get_page(&pcur);

	const rec_t*	first_rec = btr_pcur_get_rec(&pcur);

	/* The page must not be empty, except when
	it is the root page (and the whole index is empty). */
	if (page_has_prev(page)
	    || !btr_pcur_is_on_user_rec(&pcur)
	    || btr_page_get_level(page) != n_diff_data->level
	    || first_rec != page_rec_get_next_const(page_get_infimum_rec(page))
	    || !(rec_get_info_bits(first_rec, page_is_comp(page))
		 & REC_INFO_MIN_REC_FLAG)) {
		return;
	}

	const uint64_t	last_idx_on_level = bounds[n_prefix - 1][
	  unsigned(n_diff_data->n_diff_on_level - 1)];

	rec_idx = 0;

	for (i = 0; i < n_diff_data->n_analyze_leaf_pages; i++) {
		/* there are n_diff_on_level elements
		in 'boundaries' and we divide those elements
		into n_analyze_leaf_pages segments, for example:

		let n_diff_on_level=100, n_analyze_leaf_pages=4, then:
		segment i=0:  [0, 24]
		segment i=1: [25, 49]
		segment i=2: [50, 74]
		segment i=3: [75, 99] or

		let n_diff_on_level=1, n_analyze_leaf_pages=1, then:
		segment i=0: [0, 0] or

		let n_diff_on_level=2, n_analyze_leaf_pages=2, then:
		segment i=0: [0, 0]
		segment i=1: [1, 1] or

		let n_diff_on_level=13, n_analyze_leaf_pages=7, then:
		segment i=0:  [0,  0]
		segment i=1:  [1,  2]
		segment i=2:  [3,  4]
		segment i=3:  [5,  6]
		segment i=4:  [7,  8]
		segment i=5:  [9, 10]
		segment i=6: [11, 12]

		then we select a random record from each segment and dive
		below it */
		const uint64_t	n_diff = n_diff_data->n_diff_on_level;
		const uint64_t	n_pick = n_diff_data->n_analyze_leaf_pages;

		const uint64_t	left = n_diff * i / n_pick;
		const uint64_t	right = n_diff * (i + 1) / n_pick - 1;

		ut_a(left <= right);
		ut_a(right <= last_idx_on_level);

		const ulint	rnd = ut_rnd_interval(
			static_cast<ulint>(right - left));

		const uint64_t	dive_below_idx
			= bounds[n_prefix - 1][unsigned(left + rnd)];

#if 0
		DEBUG_PRINTF("    %s(): dive below record with index="
			     UINT64PF "\n", __func__, dive_below_idx);
#endif

		/* seek to the record with index dive_below_idx */
		while (rec_idx < dive_below_idx
		       && btr_pcur_is_on_user_rec(&pcur)) {

			btr_pcur_move_to_next_user_rec(&pcur, mtr);
			rec_idx++;
		}

		/* if the level has finished before the record we are
		searching for, this means that the B-tree has changed in
		the meantime, quit our sampling and use whatever stats
		we have collected so far */
		if (rec_idx < dive_below_idx) {

			ut_ad(!btr_pcur_is_on_user_rec(&pcur));
			break;
		}

		/* it could be that the tree has changed in such a way that
		the record under dive_below_idx is the supremum record, in
		this case rec_idx == dive_below_idx and pcur is positioned
		on the supremum, we do not want to dive below it */
		if (!btr_pcur_is_on_user_rec(&pcur)) {
			break;
		}

		ut_a(rec_idx == dive_below_idx);

		PageStats leaf_stats{index, uint8_t(n_prefix), nulls_unequal};

		leaf_stats.scan_below(btr_pcur_get_btr_cur(&pcur));

		/* We adjust n_diff_on_leaf_page here to avoid counting
		one value twice - once as the last on some page and once
		as the first on another page. Consider the following example:
		Leaf level:
		page: (2,2,2,2,3,3)
		... many pages like (3,3,3,3,3,3) ...
		page: (3,3,3,3,5,5)
		... many pages like (5,5,5,5,5,5) ...
		page: (5,5,5,5,8,8)
		page: (8,8,8,8,9,9)
		our algo would (correctly) get an estimate that there are
		2 distinct records per page (average). Having 4 pages below
		non-boring records, it would (wrongly) estimate the number
		of distinct records to 8. */
		if (leaf_stats.n_diff) {
			n_diff_data->n_diff_total += leaf_stats.n_diff - 1;
		}

		/* n_non_null is a per-record count, so no boundary
		double-count adjustment (unlike n_diff above). */
		n_diff_data->n_non_null_total += leaf_stats.n_non_null;

		n_diff_data->n_external_pages += leaf_stats.n_blob;
	}
}

/** statistics for an index */
namespace {
struct IndexStats
{
  std::vector<IndexFieldStats> stats;
  uint32_t index_size;
  uint32_t n_leaf_pages;

  IndexStats(ulint n_uniq) : index_size(1), n_leaf_pages(1)
  {
    stats.reserve(n_uniq);
    for (ulint i= 0; i < n_uniq; ++i)
      stats.push_back(IndexFieldStats{0, 1, 0});
  }

  void set_bulk_operation()
  {
    memset((void*) &stats[0], 0xff, stats.size() * sizeof stats[0]);
  }

  bool is_bulk_operation() const
  {
    for (auto &s : stats)
      if (!s.is_bulk_operation())
        return false;
    return true;
  }
};
} // anonymous namespace

/** Save the persistent statistics of a table or an index.
@param table            table whose stats to save
@param stats_method	innodb_stats_method variable value
@param index_id		Index ID to save statistics for (0=all)
@return DB_SUCCESS or error code */
static
dberr_t dict_stats_save(dict_table_t *table, unsigned stats_method,
			index_id_t index_id= 0) noexcept;

/** [REF01] Derive stat_n_diff_key_vals[] / stat_n_non_null_key_vals[] and
stat_n_sample_sizes[] from the sampled data.

Per n-prefix, on the sampled level LA (see dict_stats_analyze_index()):
  TOTAL_LA = records on LA             (n_recs_on_level)
  N_DIFF_LA= distinct keys on LA       (n_diff_on_level)
  Pi       = distinct keys on the i-th sampled leaf page (i = 1..A)
  D        = ordinary leaves analyzed  (n_analyze_leaf_pages)
  E        = external/BLOB pages linked from those D leaves (n_external_pages)
  L        = total index leaf pages    (index_stats.n_leaf_pages)

  R        = N_DIFF_LA / TOTAL_LA           distinctness ratio on LA, assumed
                                            to also hold at the leaf level
  avg(Pi)  = n_diff_total / D
  L_ord    = L * D/(D+E)                    ordinary (non-BLOB) leaf pages
  n_diff   = L_ord * R * avg(Pi)

BLOB pages live in BTR_SEG_LEAF, so they are included in L; the D/(D+E) factor
removes them, giving an estimate over key-bearing leaves only. n_non_null uses
the same L_ord scaling applied to the per-sample non-null counts, so for a
NOT NULL column it yields the estimated record count.
@param[in]	index		index
@param[in]	n_diff_data	input data to use to derive the results
@param[in,out]	index_stats	index stats to set */
UNIV_INLINE
void
dict_stats_index_set_n_diff(
	const dict_index_t*	index,
	const NDiffData*	n_diff_data,
	IndexStats&		index_stats)
{
	for (ulint n_prefix = index_stats.stats.size();
	     n_prefix >= 1;
	     n_prefix--) {
		/* n_diff_total can be 0 here if
		all the leaf pages sampled contained only
		delete-marked records. In this case we should assign
		0 to index->stat_n_diff_key_vals[n_prefix - 1], which
		the formula below does. */

		const NDiffData*	data = &n_diff_data[n_prefix - 1];

		ut_ad(data->n_analyze_leaf_pages > 0);
		ut_ad(data->n_recs_on_level > 0);

		uint64_t	n_ordinary_leaf_pages;

		if (data->level == 1) {
			/* If we know the number of records on level 1, then
			this number is the same as the number of pages on
			level 0 (leaf). */
			n_ordinary_leaf_pages = data->n_recs_on_level;
		} else {
			/* If we analyzed D ordinary leaf pages and found E
			external pages in total linked from those D ordinary
			leaf pages, then this means that the ratio
			ordinary/external is D/E. Then the ratio ordinary/total
			is D / (D + E). Knowing that the total number of leaf
			pages is L (including ordinary and external) then we
			estimate that the number of ordinary leaf pages is
			L_ord = L * D / (D + E). */
			n_ordinary_leaf_pages
				= index_stats.n_leaf_pages
				* data->n_analyze_leaf_pages
				/ (data->n_analyze_leaf_pages
				   + data->n_external_pages);
		}

		/* n_diff = L_ord * R * avg(Pi); see [REF01] above */
		index_stats.stats[n_prefix - 1].n_diff_key_vals
			= n_ordinary_leaf_pages

			* data->n_diff_on_level
			/ data->n_recs_on_level

			* data->n_diff_total
			/ data->n_analyze_leaf_pages;

		/* Calculate n_non_null_key_vals from sampled leaf pages.
		Non-NULL values are counted only at the leaf level, because
		non-leaf pages hold node pointers rather than data records and
		NULL tracking is only meaningful for actual records. The ratio
		of non-NULL values is assumed consistent across leaf pages, so
		sampling gives an accurate estimate for the whole index. We use
		the same formula as n_diff: multiply the average non-NULL count
		per sampled leaf by the number of ordinary leaf pages. For a
		NOT NULL column every record is counted, so this yields the
		estimated record count (i.e. no NULLs). */
		index_stats.stats[n_prefix - 1].n_non_null_key_vals
			= n_ordinary_leaf_pages
			* data->n_non_null_total
			  / data->n_analyze_leaf_pages;

		index_stats.stats[n_prefix - 1].n_sample_sizes
			= data->n_analyze_leaf_pages;

		DEBUG_PRINTF("    %s(): n_diff=" UINT64PF
			     " for n_prefix=" ULINTPF
			     " (" ULINTPF
			     " * " UINT64PF " / " UINT64PF
			     " * " UINT64PF " / " UINT64PF ")\n",
			     __func__,
			     index_stats.stats[n_prefix - 1].n_diff_key_vals,
			     n_prefix,
			     index_stats.n_leaf_pages,
			     data->n_diff_on_level,
			     data->n_recs_on_level,
			     data->n_diff_total,
			     data->n_analyze_leaf_pages);
	}
}

/** Calculate and persist statistics for one index by sampling. Saves the
results into stat_n_diff_key_vals[], stat_n_sample_sizes[], stat_index_size
and stat_n_leaf_pages. This function can be slow.

For each n-prefix (n = 1..n_uniq):
  1. Descend from the root, full-scanning each level (analyze_level()) until a
     level LA has >= 10*A distinct keys for this prefix. The search stops early
     at level 1 (never scan the leaf level) or if the next level would exceed A
     pages then scanning many more than A non-leaf pages to sample A leaf pages is
     not worthwhile.
  2. Divide LA into groups of equal keys, pick A representative records, and
     dive below each to a leaf page (sample_leaf_pages()).
  3. Derive the estimate from the samples (dict_stats_index_set_n_diff()).

Shortcut: tiny indexes (root_level == 0, or A*n_uniq > L) are scanned in full
instead of sampled, which is both faster and exact.
@param[in]	index		index to analyze
@param[in]	stats_method	innodb_stats_method variable
@return index stats */
static IndexStats dict_stats_analyze_index(dict_index_t *index,
					      unsigned stats_method)
{
	bool		level_is_analyzed;
	mtr_t		mtr;
	IndexStats	result(index->n_uniq);
	uint32_t n_sample_pages = N_SAMPLE_PAGES(index);
	uint64_t n_diff_required = N_DIFF_REQUIRED(index);

	DBUG_ENTER("dict_stats_analyze_index");

	DBUG_PRINT("info", ("index: %s, online status: %d", index->name(),
			    dict_index_get_online_status(index)));

	ut_ad(!index->table->stats_mutex_is_owner());
	ut_ad(index->table->get_ref_count());

	if (!index->is_btree()) {
		DBUG_RETURN(result);
	}

	DEBUG_PRINTF("  %s(index=%s)\n", __func__, index->name());

	mtr.start();
	mtr_sx_lock_index(index, &mtr);
	dberr_t err;
	buf_block_t* root = btr_root_block_get(index, RW_SX_LATCH, &mtr, &err);
	if (!root) {
empty_index:
		mtr.commit();
		dict_stats_assert_initialized_index(index);
		DBUG_RETURN(result);
	}

	uint16_t root_level = btr_page_get_level(root->page.frame);
	mtr.x_lock_space(index->table->space);
	uint32_t dummy, size;
	result.index_size
		= fseg_n_reserved_pages(*root, PAGE_HEADER + PAGE_BTR_SEG_LEAF
					+ root->page.frame, &size, &mtr)
		+ fseg_n_reserved_pages(*root, PAGE_HEADER + PAGE_BTR_SEG_TOP
					+ root->page.frame, &dummy, &mtr);
	result.n_leaf_pages = size ? size : 1;

	const auto bulk_trx_id = index->table->bulk_trx_id;
	if (trx_sys.is_registered(nullptr, bulk_trx_id)) {
		result.set_bulk_operation();
		goto empty_index;
	}

	mtr.commit();

	mtr.start();
	mtr_sx_lock_index(index, &mtr);

	uint16_t n_uniq = dict_index_get_n_unique(index);

	/* If the tree has just one level (and one page) or if the user
	has requested to sample too many pages then do full scan.

	For each n-column prefix (for n=1..n_uniq) N_SAMPLE_PAGES(index)
	will be sampled, so in total N_SAMPLE_PAGES(index) * n_uniq leaf
	pages will be sampled. If that number is bigger than the total
	number of leaf pages then do full scan of the leaf level instead
	since it will be faster and will give better results. */

	if (root_level == 0
	    || n_sample_pages * n_uniq > result.n_leaf_pages) {

		if (root_level == 0) {
			DEBUG_PRINTF("  %s(): just one page,"
				     " doing full scan\n", __func__);
		} else {
			DEBUG_PRINTF("  %s(): too many pages requested for"
				     " sampling, doing full scan\n", __func__);
		}

		memset(index->stat_n_diff_key_vals, 0x0,
		       n_uniq * sizeof *index->stat_n_diff_key_vals);
		if (index->stat_n_non_null_key_vals) {
			memset(index->stat_n_non_null_key_vals, 0x0,
			       n_uniq *
			       sizeof *index->stat_n_non_null_key_vals);
		}

		/* do full scan of level 0; save results directly
		into the index */

		const uint32_t n_leaf_pages{
			IndexLevelStats{stats_method > SRV_STATS_NULLS_EQUAL,
					0, index->stat_n_diff_key_vals,
					index->stat_n_non_null_key_vals,
					nullptr, index, &mtr}
			.analyze_level()};

		mtr.commit();

		index->table->stats_mutex_lock();
		for (ulint i = 0; i < n_uniq; i++) {
			result.stats[i].n_diff_key_vals =
				index->stat_n_diff_key_vals[i];
			result.stats[i].n_sample_sizes = n_leaf_pages;
			result.stats[i].n_non_null_key_vals =
				index->stat_n_non_null_key_vals[i];
		}
		/* For multi-level indexes, use the actual number
		of leaf pages counted during the full scan.
		For single-page indexes (root_level == 0),
		use the pre-calculated stat_n_leaf_pages which
		is always 1. */
		result.n_leaf_pages = (root_level != 0)
				      ? n_leaf_pages
				      : index->stat_n_leaf_pages;
		index->table->stats_mutex_unlock();

		DBUG_RETURN(result);
	}

	/* For each n-column prefix this array contains the
	input data that is used to calculate
	dict_index_t::stat_n_diff_key_vals[]. */
	NDiffData n_diff_data[MAX_REF_PARTS * 2]={{0, 0, 0, 0, 0, 0, 0}};

	/* Here we use the following optimization:
	If we find that level L is the first one (searching from the
	root) that contains at least D distinct keys when looking at
	the first n_prefix columns, then:
	if we look at the first n_prefix-1 columns then the first
	level that contains D distinct keys will be either L or a
	lower one.
	So if we find that the first level containing D distinct
	keys (on n_prefix columns) is L, we continue from L when
	searching for D distinct keys on n_prefix-1 columns. */
	auto level = root_level;
	uint16_t n_prefix;
	level_is_analyzed = false;

	/* Stack-allocated buffers for level statistics.
	Since there can be at most 64 fields in an index,
	stack allocation is safe and efficient. */
	uint64_t n_diff_buf[64]{};
	Boundaries bounds[64];
	ut_ad(n_uniq <= 64);

	IndexLevelStats level_stats(stats_method > SRV_STATS_NULLS_EQUAL,
				    level, n_diff_buf, nullptr,
				    bounds, index, &mtr);

	for (n_prefix = n_uniq; n_prefix >= 1; n_prefix--) {

		/* Commit the mtr to release the tree S lock to allow
		other threads to do some work too. */
		mtr.commit();
		mtr.start();
		mtr_sx_lock_index(index, &mtr);
		ut_ad(mtr.get_savepoint() == 1);
		buf_block_t *root = btr_root_block_get(index, RW_S_LATCH,
						       &mtr, &err);
		if (!root || root_level != btr_page_get_level(root->page.frame)
		    || index->table->bulk_trx_id != bulk_trx_id) {
			/* Just quit if the tree has changed beyond
			recognition here. The old stats from previous
			runs will remain in the values that we have
			not calculated yet. Initially when the index
			object is created the stats members are given
			some sensible values so leaving them untouched
			here even the first time will not cause us to
			read uninitialized memory later. */
			break;
		}

		mtr.rollback_to_savepoint(1);

		/* check whether we should pick the current level;
		we pick level 1 even if it does not have enough
		distinct records because we do not want to scan the
		leaf level because it may contain too many records */
		if (level_is_analyzed &&
		    (level_stats.n_diff[n_prefix - 1] >=
		       n_diff_required || level == 1)) {
			goto found_level;
		}

		/* search for a level that contains enough distinct records */
		if (level_is_analyzed && level > 1) {

			/* if this does not hold we should be on
			"found_level" instead of here */
			ut_ad(level_stats.n_diff[n_prefix - 1]
			      < n_diff_required);

			level--;
			level_is_analyzed = false;
		}

		/* descend into the tree, searching for "good enough" level */
		for (;;) {

			/* make sure we do not scan the leaf level
			accidentally, it may contain too many pages */
			ut_ad(level > 0);

			/* scanning the same level twice is an optimization
			bug */
			ut_ad(!level_is_analyzed);

			/* Do not scan if this would read too many pages.
			Here we use the following fact:
			the number of pages on level L equals the number
			of records on level L+1, thus we deduce that the
			following call would scan level_stats.n_recs pages,
			because level_stats.n_recs is left from the previous
			iteration when we scanned one level upper or we have
			not scanned any levels yet in which case
			level_stats.n_recs is 1. */
			if (level_stats.n_recs > n_sample_pages) {

				/* if the above cond is true then we are
				not at the root level since on the root
				level level_stats.n_recs == 1 (set by
				the IndexLevelStats constructor, before
				we enter the n-prefix loop) and cannot
				be > N_SAMPLE_PAGES(index) */
				ut_a(level != root_level);

				/* step one level back and be satisfied with
				whatever it contains */
				level++;
				level_is_analyzed = true;

				break;
			}

			mtr.rollback_to_savepoint(1);

			level_stats.reset_for_level(level);

			level_stats.analyze_level();

			mtr.rollback_to_savepoint(1);
			level_is_analyzed = true;

			if (level == 1
			    || level_stats.n_diff[n_prefix - 1]
			    >= n_diff_required) {
				/* we have reached the last level we could scan
				or we found a good level with many distinct
				records */
				break;
			}

			level--;
			level_is_analyzed = false;
		}
found_level:
		/* here we are either on level 1 or the level that we are on
		contains >= N_DIFF_REQUIRED distinct keys or we did not scan
		deeper levels because they would contain too many pages */

		ut_ad(level > 0);

		ut_ad(level_is_analyzed);

		/* if any of these is 0 then there is exactly one page in the
		B-tree and it is empty and we should have done full scan and
		should not be here */
		ut_ad(level_stats.n_recs > 0);
		ut_ad(level_stats.n_diff[n_prefix - 1] > 0);

		ut_ad(n_sample_pages > 0);

		NDiffData*	data = &n_diff_data[n_prefix - 1];

		data->level = level;

		data->n_recs_on_level = level_stats.n_recs;

		data->n_diff_on_level = level_stats.n_diff[n_prefix - 1];

		data->n_analyze_leaf_pages = std::min(
			n_sample_pages,
			uint32_t(level_stats.n_diff[n_prefix - 1]));

		ut_ad(level_stats.n_non_null_per_col == nullptr);
		/* pick some records from this level and dive below
		them for the given n_prefix */
		level_stats.sample_leaf_pages(n_prefix, data);
	}

	mtr.commit();

	/* n_prefix == 0 means that the above loop did not end up prematurely
	due to tree being changed and so n_diff_data[] is set up. */
	if (n_prefix == 0) {
		dict_stats_index_set_n_diff(index, n_diff_data, result);
	}

	DBUG_RETURN(result);
}

dberr_t dict_stats_update_persistent(dict_table_t *table) noexcept
{
	dict_index_t*	index;

	DEBUG_SYNC_C("dict_stats_update_persistent");

	const unsigned stats_method = unsigned(srv_innodb_stats_method);
	if (trx_sys.is_registered(nullptr, table->bulk_trx_id)) {
		dict_stats_empty_table(table, false);
		return DB_SUCCESS_LOCKED_REC;
	}

	/* analyze the clustered index first */

	index = dict_table_get_first_index(table);

	if (index == NULL
	    || index->is_corrupted()
	    || (index->type | DICT_UNIQUE) != (DICT_CLUSTERED | DICT_UNIQUE)) {

		/* Table definition is corrupt */
		dict_stats_empty_table(table, true);

		return(DB_CORRUPTION);
	}

	ut_ad(!dict_index_is_ibuf(index));
	table->stats_mutex_lock();
	dict_stats_empty_index(index, false);
	table->stats_mutex_unlock();

	IndexStats stats = dict_stats_analyze_index(index, stats_method);

	if (stats.is_bulk_operation()) {
		dict_stats_empty_table(table, false);
		return DB_SUCCESS_LOCKED_REC;
	}

	table->stats_mutex_lock();
	index->stat_index_size = stats.index_size;
	index->stat_n_leaf_pages = stats.n_leaf_pages;
	for (size_t i = 0; i < stats.stats.size(); ++i) {
		index->stat_n_diff_key_vals[i] = stats.stats[i].n_diff_key_vals;
		index->stat_n_sample_sizes[i] = stats.stats[i].n_sample_sizes;
		index->stat_n_non_null_key_vals[i] =
			stats.stats[i].n_non_null_key_vals;
	}

	ulint	n_unique = dict_index_get_n_unique(index);

	table->stat_n_rows = index->stat_n_diff_key_vals[n_unique - 1];

	table->stat_clustered_index_size = index->stat_index_size;

	/* analyze other indexes from the table, if any */

	table->stat_sum_of_other_index_sizes = 0;

	for (index = dict_table_get_next_index(index);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {

		if (!index->is_btree()) {
			continue;
		}

		dict_stats_empty_index(index, false);

		if (dict_stats_should_ignore_index(index)) {
			continue;
		}

		table->stats_mutex_unlock();
		stats = dict_stats_analyze_index(index, stats_method);
		table->stats_mutex_lock();

		if (stats.is_bulk_operation()) {
			table->stats_mutex_unlock();
			dict_stats_empty_table(table, false);
			return DB_SUCCESS_LOCKED_REC;
		}

		index->stat_index_size = stats.index_size;
		index->stat_n_leaf_pages = stats.n_leaf_pages;

		for (size_t i = 0; i < stats.stats.size(); ++i) {
			index->stat_n_diff_key_vals[i]
				= stats.stats[i].n_diff_key_vals;
			index->stat_n_sample_sizes[i]
				= stats.stats[i].n_sample_sizes;
			index->stat_n_non_null_key_vals[i]
				= stats.stats[i].n_non_null_key_vals;
		}

		table->stat_sum_of_other_index_sizes
			+= index->stat_index_size;
	}

	table->stats_last_recalc = time(NULL);

	table->stat_modified_counter = 0;

	table->stat = table->stat | dict_table_t::STATS_INITIALIZED;

	dict_stats_assert_initialized(table);

	table->stats_mutex_unlock();

	return dict_stats_save(table, stats_method);
}

dberr_t dict_stats_update_persistent_try(dict_table_t *table)
{
  if (table->stats_is_persistent() &&
      dict_stats_persistent_storage_check(false) == SCHEMA_OK)
    return dict_stats_update_persistent(table);
  return DB_SUCCESS;
}

#include "mysql_com.h"
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
	uint64_t	stat_value,
	uint64_t*	sample_size,
	const char*	stat_description,
	trx_t*		trx)
{
	dberr_t		ret;
	pars_info_t*	pinfo;
	char		db_utf8[MAX_DB_UTF8_LEN];
	char		table_utf8[MAX_TABLE_UTF8_LEN];

	ut_ad(dict_sys.locked());

	dict_fs2utf8(index->table->name.m_name, db_utf8, sizeof(db_utf8),
		     table_utf8, sizeof(table_utf8));

	pinfo = pars_info_create();
	pars_info_add_str_literal(pinfo, "database_name", db_utf8);
	pars_info_add_str_literal(pinfo, "table_name", table_utf8);
	pars_info_add_str_literal(pinfo, "index_name", index->name);
	MEM_CHECK_DEFINED(&last_update, 4);
	pars_info_add_int4_literal(pinfo, "last_update", uint32(last_update));
	MEM_CHECK_DEFINED(stat_name, strlen(stat_name));
	pars_info_add_str_literal(pinfo, "stat_name", stat_name);
	MEM_CHECK_DEFINED(&stat_value, 8);
	pars_info_add_ull_literal(pinfo, "stat_value", stat_value);
	if (sample_size != NULL) {
		MEM_CHECK_DEFINED(sample_size, 8);
		pars_info_add_ull_literal(pinfo, "sample_size", *sample_size);
	} else {
		pars_info_add_literal(pinfo, "sample_size", NULL,
				      UNIV_SQL_NULL, DATA_FIXBINARY, 0);
	}
	pars_info_add_str_literal(pinfo, "stat_description",
				  stat_description);

	ret = dict_stats_exec_sql(
		pinfo,
		"PROCEDURE INDEX_STATS_SAVE () IS\n"
		"BEGIN\n"

		"DELETE FROM \"" INDEX_STATS_NAME "\"\n"
		"WHERE\n"
		"database_name = :database_name AND\n"
		"table_name = :table_name AND\n"
		"index_name = :index_name AND\n"
		"stat_name = :stat_name;\n"

		"INSERT INTO \"" INDEX_STATS_NAME "\"\n"
		"VALUES\n"
		"(\n"
		":database_name,\n"
		":table_name,\n"
		":index_name,\n"
		":last_update,\n"
		":stat_name,\n"
		":stat_value,\n"
		":sample_size,\n"
		":stat_description\n"
		");\n"
		"END;", trx);

	if (UNIV_UNLIKELY(ret != DB_SUCCESS)) {
		if (innodb_index_stats_not_found == false
		    && !index->table->stats_error_printed) {
			index->table->stats_error_printed = true;
		ib::error() << "Cannot save index statistics for table "
			<< index->table->name
			<< ", index " << index->name
			<< ", stat name \"" << stat_name << "\": "
			<< ret;
		}
	}

	return(ret);
}

/** Report an error if updating table statistics failed because
.ibd file is missing, table decryption failed or table is corrupted.
@param[in,out]	table	Table
@param[in]	defragment	true if statistics is for defragment
@retval DB_DECRYPTION_FAILED if decryption of the table failed
@retval DB_TABLESPACE_DELETED if .ibd file is missing
@retval DB_CORRUPTION if table is marked as corrupted */
dberr_t
dict_stats_report_error(dict_table_t* table, bool defragment)
{
	dberr_t		err;

	const char*	df = defragment ? " defragment" : "";

	if (!table->space) {
		ib::warn() << "Cannot save" << df << " statistics for table "
			   << table->name
			   << " because the .ibd file is missing. "
			   << TROUBLESHOOTING_MSG;
		err = DB_TABLESPACE_DELETED;
	} else {
		ib::warn() << "Cannot save" << df << " statistics for table "
			   << table->name
			   << " because file "
			   << table->space->chain.start->name
			   << (table->corrupted
			       ? " is corrupted."
			       : " cannot be decrypted.");
		err = table->corrupted ? DB_CORRUPTION : DB_DECRYPTION_FAILED;
	}

	dict_stats_empty_table(table, defragment);
	return err;
}

static const char *stats_method_name[]= {
  nullptr, " NULLS_UNEQUAL", " NULLS_IGNORED"
};

/** Return a display name for the innodb_stats_method
@param  stat_method   innodb_stats_method during index statistics
@return innodb_stats_method name */
static const char *get_innodb_stats_method(unsigned stat_method)
{
  ut_ad(stat_method < 3);
  static_assert(SRV_STATS_NULLS_EQUAL == 0, "");
  static_assert(SRV_STATS_NULLS_UNEQUAL == 1, "");
  static_assert(SRV_STATS_NULLS_IGNORED == 2, "");
  return stat_method < 3 ? stats_method_name[stat_method] : nullptr;
}

static
dberr_t dict_stats_save(dict_table_t* table, unsigned stats_method,
			index_id_t index_id) noexcept
{
	pars_info_t*	pinfo;
	char		db_utf8[MAX_DB_UTF8_LEN];
	char		table_utf8[MAX_TABLE_UTF8_LEN];
	THD* const	thd = current_thd;

#ifdef ENABLED_DEBUG_SYNC
	DBUG_EXECUTE_IF("dict_stats_save_exit_notify",
	   SCOPE_EXIT([thd] {
	       debug_sync_set_action(thd,
	       STRING_WITH_LEN("now SIGNAL dict_stats_save_finished"));
	    });
	);
	DBUG_EXECUTE_IF("dict_stats_save_exit_notify_and_wait",
	   SCOPE_EXIT([] {
	       debug_sync_set_action(current_thd,
	       STRING_WITH_LEN("now SIGNAL dict_stats_save_finished"
			       " WAIT_FOR dict_stats_save_unblock"));
	    });
	);
#endif /* ENABLED_DEBUG_SYNC */

	if (high_level_read_only) {
		return DB_READ_ONLY;
	}

	if (!table->is_readable()) {
		return (dict_stats_report_error(table));
	}

	dict_stats stats;
	if (stats.open(thd)) {
		return DB_STATS_DO_NOT_EXIST;
	}
	dict_fs2utf8(table->name.m_name, db_utf8, sizeof(db_utf8),
		     table_utf8, sizeof(table_utf8));
	const time_t now = time(NULL);
	trx_t*	trx = trx_create();
	trx->mysql_thd = thd;
	trx_start_internal(trx);
	dberr_t ret = trx->read_only
		? DB_READ_ONLY
		: lock_table_for_trx(stats.table(), trx, LOCK_X);
	if (ret == DB_SUCCESS) {
		ret = lock_table_for_trx(stats.index(), trx, LOCK_X);
	}
	if (ret != DB_SUCCESS) {
		if (trx->state != TRX_STATE_NOT_STARTED) {
			trx->commit();
		}
		goto unlocked_free_and_exit;
	}

	pinfo = pars_info_create();

	pars_info_add_str_literal(pinfo, "database_name", db_utf8);
	pars_info_add_str_literal(pinfo, "table_name", table_utf8);
	pars_info_add_int4_literal(pinfo, "last_update", uint32(now));
	pars_info_add_ull_literal(pinfo, "n_rows", table->stat_n_rows);
	pars_info_add_ull_literal(pinfo, "clustered_index_size",
		table->stat_clustered_index_size);
	pars_info_add_ull_literal(pinfo, "sum_of_other_index_sizes",
		table->stat_sum_of_other_index_sizes);

	dict_sys.lock(SRW_LOCK_CALL);
	trx->dict_operation_lock_mode = true;

	ret = dict_stats_exec_sql(
		pinfo,
		"PROCEDURE TABLE_STATS_SAVE () IS\n"
		"BEGIN\n"

		"DELETE FROM \"" TABLE_STATS_NAME "\"\n"
		"WHERE\n"
		"database_name = :database_name AND\n"
		"table_name = :table_name;\n"

		"INSERT INTO \"" TABLE_STATS_NAME "\"\n"
		"VALUES\n"
		"(\n"
		":database_name,\n"
		":table_name,\n"
		":last_update,\n"
		":n_rows,\n"
		":clustered_index_size,\n"
		":sum_of_other_index_sizes\n"
		");\n"
		"END;", trx);

	if (UNIV_UNLIKELY(ret != DB_SUCCESS)) {
		sql_print_error("InnoDB: Cannot save table statistics for"
#ifdef EMBEDDED_LIBRARY
				" table %.*s.%s: %s",
#else
				" table %`.*s.%`s: %s",
#endif
				int(table->name.dblen()), table->name.m_name,
				table->name.basename(), ut_strerr(ret));
rollback_and_exit:
		trx->rollback();
free_and_exit:
		trx->dict_operation_lock_mode = false;
		dict_sys.unlock();
unlocked_free_and_exit:
		trx->clear_and_free();
		stats.close();
		return ret;
	}

	dict_index_t*	index;
	index_map_t	indexes(
		(ut_strcmp_functor()),
		index_map_t_allocator(mem_key_dict_stats_index_map_t));

	/* Below we do all the modifications in innodb_index_stats in a single
	transaction for performance reasons. Modifying more than one row in a
	single transaction may deadlock with other transactions if they
	lock the rows in different order. Other transaction could be for
	example when we DROP a table and do
	DELETE FROM innodb_index_stats WHERE database_name = '...'
	AND table_name = '...'; which will affect more than one row. To
	prevent deadlocks we always lock the rows in the same order - the
	order of the PK, which is (database_name, table_name, index_name,
	stat_name). This is why below we sort the indexes by name and then
	for each index, do the mods ordered by stat_name. */

	for (index = dict_table_get_first_index(table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {

		indexes[index->name] = index;
	}

	index_map_t::const_iterator	it;

	for (it = indexes.begin(); it != indexes.end(); ++it) {

		index = it->second;

		if (index_id != 0 && index->id != index_id) {
			continue;
		}

		if (dict_stats_should_ignore_index(index)) {
			continue;
		}

		ut_ad(!dict_index_is_ibuf(index));

		for (unsigned i = 0; i < index->n_uniq; i++) {

			char	stat_name[16];
			char	stat_description[1024];

			snprintf(stat_name, sizeof(stat_name),
				 "n_diff_pfx%02u", i + 1);

			/* craft a string that contains the column names */
			snprintf(stat_description, sizeof(stat_description),
				 "%s", index->fields[0].name());
			for (unsigned j = 1; j <= i; j++) {
				size_t	len = strlen(stat_description);
				size_t	remaining =
					sizeof(stat_description) - len;
				/* Ensure we have enough space for
				"," + field_name + null terminator */
				size_t	field_name_len =
					strlen(index->fields[j].name());
				if (remaining < field_name_len + 2) {
					break;
				}
				snprintf(stat_description + len,
					 remaining, ",%s",
					 index->fields[j].name());
			}

			if (const char *stats_method_name =
				get_innodb_stats_method(stats_method)) {
				size_t desc_len = strlen(stat_description);
				size_t method_len = strlen(stats_method_name);
				size_t remaining = sizeof(stat_description)
							- desc_len - 1;
				if (method_len <= remaining) {
					strncat(stat_description,
						stats_method_name, remaining);
				}
			}

			ret = dict_stats_save_index_stat(
				index, now, stat_name,
				index->stat_n_diff_key_vals[i],
				&index->stat_n_sample_sizes[i],
				stat_description, trx);

			if (ret != DB_SUCCESS) {
				goto rollback_and_exit;
			}

			/* Update n_nonnull_fld */
			snprintf(stat_name, sizeof(stat_name),
				 "n_nonnull_fld%02u", i + 1);

			snprintf(stat_description,
				 sizeof(stat_description),
				 "%s", index->fields[i].name());

			ret = dict_stats_save_index_stat(
				index, now, stat_name,
				index->stat_n_non_null_key_vals[i],
				&index->stat_n_sample_sizes[i],
				stat_description, trx);

			if (ret != DB_SUCCESS) {
				goto rollback_and_exit;
			}
		}

		ret = dict_stats_save_index_stat(index, now, "n_leaf_pages",
						 index->stat_n_leaf_pages,
						 NULL,
						 "Number of leaf pages "
						 "in the index", trx);
		if (ret != DB_SUCCESS) {
			goto rollback_and_exit;
		}

		ret = dict_stats_save_index_stat(index, now, "size",
						 index->stat_index_size,
						 NULL,
						 "Number of pages "
						 "in the index", trx);
		if (ret != DB_SUCCESS) {
			goto rollback_and_exit;
		}
	}

	ret= trx->bulk_insert_apply();
	if (ret != DB_SUCCESS) {
		goto rollback_and_exit;
	}

	trx->commit();
	goto free_and_exit;
}

static dberr_t dict_stats_save(dict_table_t *table)
{
  return dict_stats_save(table, unsigned(srv_innodb_stats_method));
}

void dict_stats_empty_table_and_save(dict_table_t *table)
{
  dict_stats_empty_table(table, true);
  if (table->stats_is_persistent() &&
      dict_stats_persistent_storage_check(false) == SCHEMA_OK)
    dict_stats_save(table);
}

/*********************************************************************//**
Called for the row that is selected by
SELECT ... FROM mysql.innodb_table_stats WHERE table='...'
The second argument is a pointer to the table and the fetched stats are
written to it.
@return non-NULL dummy */
static
ibool
dict_stats_fetch_table_stats_step(
/*==============================*/
	void*	node_void,	/*!< in: select node */
	void*	table_void)	/*!< out: table */
{
	sel_node_t*	node = (sel_node_t*) node_void;
	dict_table_t*	table = (dict_table_t*) table_void;
	que_common_t*	cnode;
	int		i;

	/* this should loop exactly 3 times - for
	n_rows,clustered_index_size,sum_of_other_index_sizes */
	for (cnode = static_cast<que_common_t*>(node->select_list), i = 0;
	     cnode != NULL;
	     cnode = static_cast<que_common_t*>(que_node_get_next(cnode)),
	     i++) {

		const byte*	data;
		dfield_t*	dfield = que_node_get_val(cnode);
		dtype_t*	type = dfield_get_type(dfield);
		ulint		len = dfield_get_len(dfield);

		data = static_cast<const byte*>(dfield_get_data(dfield));

		switch (i) {
		case 0: /* mysql.innodb_table_stats.n_rows */

			ut_a(dtype_get_mtype(type) == DATA_INT);
			ut_a(len == 8);

			table->stat_n_rows = mach_read_from_8(data);

			break;

		case 1: /* mysql.innodb_table_stats.clustered_index_size */
		{
			ut_a(dtype_get_mtype(type) == DATA_INT);
			ut_a(len == 8);

			table->stat_clustered_index_size
				= std::max(mach_read_from_4(data + 4), 1U);
			break;
		}

		case 2: /* mysql.innodb_table_stats.sum_of_other_index_sizes */
		{
			ut_a(dtype_get_mtype(type) == DATA_INT);
			ut_a(len == 8);

			table->stat_sum_of_other_index_sizes = std::max(
				mach_read_from_4(data + 4),
				uint32_t(UT_LIST_GET_LEN(table->indexes) - 1));
			break;
		}
		default:

			/* someone changed SELECT
			n_rows,clustered_index_size,sum_of_other_index_sizes
			to select more columns from innodb_table_stats without
			adjusting here */
			ut_error;
		}
	}

	/* if i < 3 this means someone changed the
	SELECT n_rows,clustered_index_size,sum_of_other_index_sizes
	to select less columns from innodb_table_stats without adjusting here;
	if i > 3 we would have ut_error'ed earlier */
	ut_a(i == 3 /*n_rows,clustered_index_size,sum_of_other_index_sizes*/);

	/* XXX this is not used but returning non-NULL is necessary */
	return(TRUE);
}

/** Report that a row with a malformed or out-of-range stat_name was found
in mysql.innodb_index_stats and is being ignored.
@param table          table the statistics belong to
@param index          index the statistics belong to
@param stat_name      value of the stat_name column (not NUL-terminated)
@param stat_name_len  length of stat_name */
static void dict_stats_report_bad_index_stat(
	const dict_table_t*	table,
	const dict_index_t*	index,
	const char*		stat_name,
	size_t			stat_name_len)
{
	char	db_utf8[MAX_DB_UTF8_LEN];
	char	table_utf8[MAX_TABLE_UTF8_LEN];

	dict_fs2utf8(table->name.m_name, db_utf8, sizeof(db_utf8),
		     table_utf8, sizeof(table_utf8));

	sql_print_information("InnoDB: Malformed stat_name='%.*s' in "
			      INDEX_STATS_NAME_PRINT " WHERE "
			      "database_name=%`s AND table_name=%`s AND "
			      "index_name=%`s",
			      (int) stat_name_len, stat_name,
			      db_utf8, table_utf8, index->name());
}

/** Aux struct used to pass a table and a boolean to
dict_stats_fetch_index_stats_step(). */
namespace {
struct IndexFetch {
	dict_table_t*	table;	/*!< table whose indexes are to be modified */
	bool		stats_were_modified; /*!< will be set to true if at
				least one index stats were modified */
};
} // anonymous namespace

/*********************************************************************//**
Called for the rows that are selected by
SELECT ... FROM mysql.innodb_index_stats WHERE table='...'
The second argument is a pointer to the table and the fetched stats are
written to its indexes.
Let a table has N indexes and each index has Ui unique columns for i=1..N,
then mysql.innodb_index_stats will have SUM(Ui) i=1..N rows for that table.
So this function will be called SUM(Ui) times where SUM(Ui) is of magnitude
N*AVG(Ui). In each call it searches for the currently fetched index into
table->indexes linearly, assuming this list is not sorted. Thus, overall,
fetching all indexes' stats from mysql.innodb_index_stats is O(N^2) where N
is the number of indexes.
This can be improved if we sort table->indexes in a temporary area just once
and then search in that sorted list. Then the complexity will be O(N*log(N)).
We assume a table will not have more than 100 indexes, so we go with the
simpler N^2 algorithm.
@return non-NULL dummy */
static
ibool
dict_stats_fetch_index_stats_step(
/*==============================*/
	void*	node_void,	/*!< in: select node */
	void*	arg_void)	/*!< out: table + a flag that tells if we
				modified anything */
{
	sel_node_t*	node = (sel_node_t*) node_void;
	IndexFetch*	arg = (IndexFetch*) arg_void;
	dict_table_t*	table = arg->table;
	dict_index_t*	index = NULL;
	que_common_t*	cnode;
	const char*	stat_name = NULL;
	ulint		stat_name_len = ULINT_UNDEFINED;
	uint64_t	stat_value = UINT64_UNDEFINED;
	uint64_t	sample_size = UINT64_UNDEFINED;
	int		i;

	/* this should loop exactly 4 times - for the columns that
	were selected: index_name,stat_name,stat_value,sample_size */
	for (cnode = static_cast<que_common_t*>(node->select_list), i = 0;
	     cnode != NULL;
	     cnode = static_cast<que_common_t*>(que_node_get_next(cnode)),
	     i++) {

		const byte*	data;
		dfield_t*	dfield = que_node_get_val(cnode);
		dtype_t*	type = dfield_get_type(dfield);
		ulint		len = dfield_get_len(dfield);

		data = static_cast<const byte*>(dfield_get_data(dfield));

		switch (i) {
		case 0: /* mysql.innodb_index_stats.index_name */

			ut_a(dtype_get_mtype(type) == DATA_VARMYSQL);

			/* search for index in table's indexes whose name
			matches data; the fetched index name is in data,
			has no terminating '\0' and has length len */
			for (index = dict_table_get_first_index(table);
			     index != NULL;
			     index = dict_table_get_next_index(index)) {

				if (index->is_committed()
				    && strlen(index->name) == len
				    && memcmp(index->name, data, len) == 0) {
					/* the corresponding index was found */
					break;
				}
			}

			/* if index is NULL here this means that
			mysql.innodb_index_stats contains more rows than the
			number of indexes in the table; this is ok, we just
			return ignoring those extra rows; in other words
			dict_stats_fetch_index_stats_step() has been called
			for a row from index_stats with unknown index_name
			column */
			if (index == NULL) {

				return(TRUE);
			}

			break;

		case 1: /* mysql.innodb_index_stats.stat_name */

			ut_a(dtype_get_mtype(type) == DATA_VARMYSQL);

			ut_a(index != NULL);

			stat_name = (const char*) data;
			stat_name_len = len;

			break;

		case 2: /* mysql.innodb_index_stats.stat_value */

			ut_a(dtype_get_mtype(type) == DATA_INT);
			ut_a(len == 8);

			ut_a(index != NULL);
			ut_a(stat_name != NULL);
			ut_a(stat_name_len != ULINT_UNDEFINED);

			stat_value = mach_read_from_8(data);

			break;

		case 3: /* mysql.innodb_index_stats.sample_size */

			ut_a(dtype_get_mtype(type) == DATA_INT);
			ut_a(len == 8 || len == UNIV_SQL_NULL);

			ut_a(index != NULL);
			ut_a(stat_name != NULL);
			ut_a(stat_name_len != ULINT_UNDEFINED);
			ut_a(stat_value != UINT64_UNDEFINED);

			if (len == UNIV_SQL_NULL) {
				break;
			}
			/* else */

			sample_size = mach_read_from_8(data);

			break;

		default:

			/* someone changed
			SELECT index_name,stat_name,stat_value,sample_size
			to select more columns from innodb_index_stats without
			adjusting here */
			ut_error;
		}
	}

	/* if i < 4 this means someone changed the
	SELECT index_name,stat_name,stat_value,sample_size
	to select less columns from innodb_index_stats without adjusting here;
	if i > 4 we would have ut_error'ed earlier */
	ut_a(i == 4 /* index_name,stat_name,stat_value,sample_size */);

	ut_a(index != NULL);
	ut_a(stat_name != NULL);
	ut_a(stat_name_len != ULINT_UNDEFINED);
	ut_a(stat_value != UINT64_UNDEFINED);
	/* sample_size could be UINT64_UNDEFINED here, if it is NULL */

#define PFX	"n_diff_pfx"
#define PFX_LEN	10

	if (stat_name_len == 4 /* strlen("size") */
	    && strncasecmp("size", stat_name, stat_name_len) == 0) {
		index->stat_index_size = std::max(uint32_t(stat_value), 1U);
		arg->stats_were_modified = true;
	} else if (stat_name_len == 12 /* strlen("n_leaf_pages") */
		   && strncasecmp("n_leaf_pages", stat_name, stat_name_len)
		   == 0) {
		index->stat_n_leaf_pages = std::max(uint32_t(stat_value), 1U);
		arg->stats_were_modified = true;
	} else if (stat_name_len == 12 /* strlen("n_page_split") */
		   && strncasecmp("n_page_split", stat_name, stat_name_len)
		      == 0) {
		index->stat_defrag_n_page_split = (ulint) stat_value;
		arg->stats_were_modified = true;
	} else if (stat_name_len == 13 /* strlen("n_pages_freed") */
		   && strncasecmp("n_pages_freed", stat_name, stat_name_len)
		      == 0) {
		index->stat_defrag_n_pages_freed = (ulint) stat_value;
		arg->stats_were_modified = true;
	} else if (stat_name_len > PFX_LEN /* e.g. stat_name=="n_diff_pfx01" */
		   && strncasecmp(PFX, stat_name, PFX_LEN) == 0) {

		const char*	num_ptr;
		unsigned long	n_pfx;

		/* point num_ptr into "1" from "n_diff_pfx12..." */
		num_ptr = stat_name + PFX_LEN;

		/* stat_name should have exactly 2 chars appended to PFX
		and they should be digits */
		if (stat_name_len != PFX_LEN + 2
		    || num_ptr[0] < '0' || num_ptr[0] > '9'
		    || num_ptr[1] < '0' || num_ptr[1] > '9') {

			dict_stats_report_bad_index_stat(
				table, index, stat_name, stat_name_len);
			return(TRUE);
		}
		/* else */

		/* extract 12 from "n_diff_pfx12..." into n_pfx
		note that stat_name does not have a terminating '\0' */
		n_pfx = ulong(num_ptr[0] - '0') * 10 + ulong(num_ptr[1] - '0');

		ulint	n_uniq = index->n_uniq;

		if (n_pfx == 0 || n_pfx > n_uniq) {

			dict_stats_report_bad_index_stat(
				table, index, stat_name, stat_name_len);
			return(TRUE);
		}
		/* else */

		index->stat_n_diff_key_vals[n_pfx - 1] = stat_value;

		if (sample_size != UINT64_UNDEFINED) {
			index->stat_n_sample_sizes[n_pfx - 1] =
				std::max<uint64_t>(sample_size, 1);
		} else {
			/* hmm, strange... the user must have UPDATEd the
			table manually and SET sample_size = NULL */
			index->stat_n_sample_sizes[n_pfx - 1] = 0;
		}

		arg->stats_were_modified = true;
	} else if (stat_name_len >= sizeof "n_nonnull_fld"
		   && strncasecmp("n_nonnull_fld", stat_name,
				  sizeof "n_nonnull_fld" - 1) == 0) {

		const char*	num_ptr;
		unsigned	n_field;

		/* point num_ptr into "01" from "n_nonnull_fld01..." */
		num_ptr = stat_name + (sizeof "n_nonnull_fld" - 1);

		/* stat_name should have exactly 2 chars appended to "n_nonnull_fld"
		and they should be digits */
		if (stat_name_len != sizeof "n_nonnull_fld01" - 1
		    || num_ptr[0] < '0' || num_ptr[0] > '9'
		    || num_ptr[1] < '0' || num_ptr[1] > '9') {

			dict_stats_report_bad_index_stat(
				table, index, stat_name, stat_name_len);
			return(TRUE);
		}
		/* else */

		/* extract 01 from "n_nonnull_fld01..." into n_col
		note that stat_name does not have a terminating '\0' */
		n_field = (num_ptr[0] - '0') * 10 + (num_ptr[1] - '0');

		if (n_field == 0 || n_field > index->n_uniq) {

			dict_stats_report_bad_index_stat(
				table, index, stat_name, stat_name_len);
			return(TRUE);
		}
		/* else */

		index->stat_n_non_null_key_vals[n_field - 1] = stat_value;

		arg->stats_were_modified = true;
	} else {
		/* silently ignore rows with unknown stat_name, the
		user may have developed her own stats */
	}

	/* XXX this is not used but returning non-NULL is necessary */
	return(TRUE);
}

/** Read the stored persistent statistics of a table. */
dberr_t dict_stats_fetch_from_ps(dict_table_t *table)
{
	IndexFetch	index_fetch_arg;
	pars_info_t*	pinfo;
	char		db_utf8[MAX_DB_UTF8_LEN];
	char		table_utf8[MAX_TABLE_UTF8_LEN];

	/* Initialize all stats to dummy values before fetching because if
	the persistent storage contains incomplete stats (e.g. missing stats
	for some index) then we would end up with (partially) uninitialized
	stats. */
	dict_stats_empty_table(table, true);

	THD* const thd = current_thd;
	dict_stats stats;
	if (stats.open(thd)) {
		return DB_STATS_DO_NOT_EXIST;
	}

#ifdef ENABLED_DEBUG_SYNC
	DEBUG_SYNC(thd, "dict_stats_mdl_acquired");
#endif /* ENABLED_DEBUG_SYNC */

	dict_fs2utf8(table->name.m_name, db_utf8, sizeof(db_utf8),
		     table_utf8, sizeof(table_utf8));

	pinfo = pars_info_create();

	pars_info_add_str_literal(pinfo, "database_name", db_utf8);

	pars_info_add_str_literal(pinfo, "table_name", table_utf8);

	pars_info_bind_function(pinfo,
			       "fetch_table_stats_step",
			       dict_stats_fetch_table_stats_step,
			       table);

	index_fetch_arg.table = table;
	index_fetch_arg.stats_were_modified = false;
	pars_info_bind_function(pinfo,
			        "fetch_index_stats_step",
			        dict_stats_fetch_index_stats_step,
			        &index_fetch_arg);
	dict_sys.lock(SRW_LOCK_CALL);
	que_t* graph = pars_sql(
		pinfo,
		"PROCEDURE FETCH_STATS () IS\n"
		"found INT;\n"
		"DECLARE FUNCTION fetch_table_stats_step;\n"
		"DECLARE FUNCTION fetch_index_stats_step;\n"
		"DECLARE CURSOR table_stats_cur IS\n"
		"  SELECT\n"
		/* if you change the selected fields, be
		sure to adjust
		dict_stats_fetch_table_stats_step() */
		"  n_rows,\n"
		"  clustered_index_size,\n"
		"  sum_of_other_index_sizes\n"
		"  FROM \"" TABLE_STATS_NAME "\"\n"
		"  WHERE\n"
		"  database_name = :database_name AND\n"
		"  table_name = :table_name;\n"
		"DECLARE CURSOR index_stats_cur IS\n"
		"  SELECT\n"
		/* if you change the selected fields, be
		sure to adjust
		dict_stats_fetch_index_stats_step() */
		"  index_name,\n"
		"  stat_name,\n"
		"  stat_value,\n"
		"  sample_size\n"
		"  FROM \"" INDEX_STATS_NAME "\"\n"
		"  WHERE\n"
		"  database_name = :database_name AND\n"
		"  table_name = :table_name;\n"

		"BEGIN\n"

		"OPEN table_stats_cur;\n"
		"FETCH table_stats_cur INTO\n"
		"  fetch_table_stats_step();\n"
		"IF (SQL % NOTFOUND) THEN\n"
		"  CLOSE table_stats_cur;\n"
		"  RETURN;\n"
		"END IF;\n"
		"CLOSE table_stats_cur;\n"

		"OPEN index_stats_cur;\n"
		"found := 1;\n"
		"WHILE found = 1 LOOP\n"
		"  FETCH index_stats_cur INTO\n"
		"    fetch_index_stats_step();\n"
		"  IF (SQL % NOTFOUND) THEN\n"
		"    found := 0;\n"
		"  END IF;\n"
		"END LOOP;\n"
		"CLOSE index_stats_cur;\n"

		"END;");
	dict_sys.unlock();

	trx_t* trx = trx_create();
	trx->graph = nullptr;
	graph->trx = trx;

	trx_start_internal_read_only(trx);
	que_run_threads(que_fork_start_command(graph));
	que_graph_free(graph);
	trx_commit_for_mysql(trx);
	dberr_t ret = index_fetch_arg.stats_were_modified
		? trx->error_state : DB_STATS_DO_NOT_EXIST;
	trx->free();
	stats.close();
	return ret;
}

/*********************************************************************//**
Fetches or calculates new estimates for index statistics. */
void
dict_stats_update_for_index(
/*========================*/
	dict_index_t*	index)	/*!< in/out: index */
{
  dict_table_t *const table= index->table;
  unsigned stats_method = unsigned(srv_innodb_stats_method);
  ut_ad(table->stat_initialized());

  if (table->stats_is_persistent())
    switch (dict_stats_persistent_storage_check(false)) {
    case SCHEMA_NOT_EXIST:
      break;
    case SCHEMA_INVALID:
      if (table->stats_error_printed)
        break;
      table->stats_error_printed= true;
      sql_print_information("InnoDB: Recalculation of persistent statistics"
#ifdef EMBEDDED_LIBRARY
                            " requested for table %.*s.%s index %s but"
#else
                            " requested for table %`.*s.%`s index %`s but"
#endif
                            " the required persistent statistics storage"
                            " is corrupted. Using transient stats instead.",
                            int(table->name.dblen()), table->name.m_name,
                            table->name.basename(), index->name());
      break;
    case SCHEMA_OK:
      IndexStats stats{dict_stats_analyze_index(index, stats_method)};
      table->stats_mutex_lock();
      index->stat_index_size = stats.index_size;
      index->stat_n_leaf_pages = stats.n_leaf_pages;
      for (size_t i = 0; i < stats.stats.size(); ++i)
      {
        index->stat_n_diff_key_vals[i]= stats.stats[i].n_diff_key_vals;
        index->stat_n_sample_sizes[i]= stats.stats[i].n_sample_sizes;
        index->stat_n_non_null_key_vals[i]= stats.stats[i].n_non_null_key_vals;
      }
      table->stat_sum_of_other_index_sizes+= index->stat_index_size;
      table->stats_mutex_unlock();
      dict_stats_save(table, stats_method, index->id);
      return;
    }

  dict_stats_update_transient_for_index(index);
}

/** Execute DELETE FROM mysql.innodb_table_stats
@param database_name  database name
@param table_name     table name
@param trx            transaction (nullptr=start and commit a new one)
@return DB_SUCCESS or error code */
dberr_t dict_stats_delete_from_table_stats(const char *database_name,
                                           const char *table_name, trx_t *trx)
{
	pars_info_t*	pinfo;

	ut_ad(dict_sys.locked());

	pinfo = pars_info_create();

	pars_info_add_str_literal(pinfo, "database_name", database_name);
	pars_info_add_str_literal(pinfo, "table_name", table_name);

	return dict_stats_exec_sql(
		pinfo,
		"PROCEDURE DELETE_FROM_TABLE_STATS () IS\n"
		"BEGIN\n"
		"DELETE FROM \"" TABLE_STATS_NAME "\" WHERE\n"
		"database_name = :database_name AND\n"
		"table_name = :table_name;\n"
		"END;\n", trx);
}

/** Execute DELETE FROM mysql.innodb_index_stats
@param database_name  database name
@param table_name     table name
@param trx            transaction
@return DB_SUCCESS or error code */
dberr_t dict_stats_delete_from_index_stats(const char *database_name,
                                           const char *table_name, trx_t *trx)
{
	pars_info_t*	pinfo;

	ut_ad(dict_sys.locked());

	pinfo = pars_info_create();

	pars_info_add_str_literal(pinfo, "database_name", database_name);
	pars_info_add_str_literal(pinfo, "table_name", table_name);

	return dict_stats_exec_sql(
		pinfo,
		"PROCEDURE DELETE_FROM_INDEX_STATS () IS\n"
		"BEGIN\n"
		"DELETE FROM \"" INDEX_STATS_NAME "\" WHERE\n"
		"database_name = :database_name AND\n"
		"table_name = :table_name;\n"
		"END;\n", trx);
}

/** Execute DELETE FROM mysql.innodb_index_stats
@param database_name  database name
@param table_name     table name
@param index_name     name of the index
@param trx            transaction
@return DB_SUCCESS or error code */
dberr_t dict_stats_delete_from_index_stats(const char *database_name,
                                           const char *table_name,
                                           const char *index_name, trx_t *trx)
{
	pars_info_t*	pinfo;

	ut_ad(dict_sys.locked());

	pinfo = pars_info_create();

	pars_info_add_str_literal(pinfo, "database_name", database_name);
	pars_info_add_str_literal(pinfo, "table_name", table_name);
	pars_info_add_str_literal(pinfo, "index_name", index_name);

	return dict_stats_exec_sql(
		pinfo,
		"PROCEDURE DELETE_FROM_INDEX_STATS () IS\n"
		"BEGIN\n"
		"DELETE FROM \"" INDEX_STATS_NAME "\" WHERE\n"
		"database_name = :database_name AND\n"
		"table_name = :table_name AND\n"
		"index_name = :index_name;\n"
		"END;\n", trx);
}

/** Rename a table in InnoDB persistent stats storage.
@param old_name  old table name
@param new_name  new table name
@param trx       transaction
@return DB_SUCCESS or error code */
dberr_t dict_stats_rename_table(const char *old_name, const char *new_name,
                                trx_t *trx)
{
  /* skip the statistics tables themselves */
  if (!strcmp(old_name, TABLE_STATS_NAME) ||
      !strcmp(old_name, INDEX_STATS_NAME) ||
      !strcmp(new_name, TABLE_STATS_NAME) ||
      !strcmp(new_name, INDEX_STATS_NAME))
    return DB_SUCCESS;

  char old_db[MAX_DB_UTF8_LEN];
  char new_db[MAX_DB_UTF8_LEN];
  char old_table[MAX_TABLE_UTF8_LEN];
  char new_table[MAX_TABLE_UTF8_LEN];

  dict_fs2utf8(old_name, old_db, sizeof old_db, old_table, sizeof old_table);
  dict_fs2utf8(new_name, new_db, sizeof new_db, new_table, sizeof new_table);

  /* Delete the stats only if renaming the table from old table to
  intermediate table during COPY algorithm */
  if (dict_table_t::is_temporary_name(new_name))
  {
    if (dberr_t e= dict_stats_delete_from_table_stats(old_db, old_table, trx))
      return e;
    return dict_stats_delete_from_index_stats(old_db, old_table, trx);
  }

  pars_info_t *pinfo= pars_info_create();
  pars_info_add_str_literal(pinfo, "old_db", old_db);
  pars_info_add_str_literal(pinfo, "old_table", old_table);
  pars_info_add_str_literal(pinfo, "new_db", new_db);
  pars_info_add_str_literal(pinfo, "new_table", new_table);

  static const char sql[]=
    "PROCEDURE RENAME_TABLE_IN_STATS() IS\n"
    "BEGIN\n"
    "UPDATE \"" TABLE_STATS_NAME "\" SET\n"
    "database_name=:new_db, table_name=:new_table\n"
    "WHERE database_name=:old_db AND table_name=:old_table;\n"
    "UPDATE \"" INDEX_STATS_NAME "\" SET\n"
    "database_name=:new_db, table_name=:new_table\n"
    "WHERE database_name=:old_db AND table_name=:old_table;\n"
    "END;\n";

  return dict_stats_exec_sql(pinfo, sql, trx);
}

/** Rename an index in InnoDB persistent statistics.
@param db         database name
@param table      table name
@param old_name   old table name
@param new_name   new table name
@param trx        transaction
@return DB_SUCCESS or error code */
dberr_t dict_stats_rename_index(const char *db, const char *table,
                                const char *old_name, const char *new_name,
                                trx_t *trx)
{
  if (dict_stats_persistent_storage_check(true) != SCHEMA_OK)
    return DB_STATS_DO_NOT_EXIST;
  pars_info_t *pinfo= pars_info_create();

  pars_info_add_str_literal(pinfo, "db", db);
  pars_info_add_str_literal(pinfo, "table", table);
  pars_info_add_str_literal(pinfo, "old", old_name);
  pars_info_add_str_literal(pinfo, "new", new_name);

  static const char sql[]=
    "PROCEDURE RENAME_INDEX_IN_STATS() IS\n"
    "BEGIN\n"
    "UPDATE \"" INDEX_STATS_NAME "\" SET index_name=:new\n"
    "WHERE database_name=:db AND table_name=:table AND index_name=:old;\n"
    "END;\n";

  return dict_stats_exec_sql(pinfo, sql, trx);
}

/** Delete all persistent statistics for a database.
@param db    database name
@param trx   transaction
@return DB_SUCCESS or error code */
dberr_t dict_stats_delete(const char *db, trx_t *trx)
{
  static const char sql[] =
    "PROCEDURE DROP_DATABASE_STATS () IS\n"
    "BEGIN\n"
    "DELETE FROM \"" TABLE_STATS_NAME "\" WHERE database_name=:db;\n"
    "DELETE FROM \"" INDEX_STATS_NAME "\" WHERE database_name=:db;\n"
    "END;\n";

  pars_info_t *pinfo= pars_info_create();
  pars_info_add_str_literal(pinfo, "db", db);
  return dict_stats_exec_sql(pinfo, sql, trx);
}

/* tests @{ */
#ifdef UNIV_ENABLE_UNIT_TEST_DICT_STATS
/* save/fetch aux macros @{ */
#define TEST_DATABASE_NAME		"foobardb"
#define TEST_TABLE_NAME			"test_dict_stats"

#define TEST_N_ROWS			111
#define TEST_CLUSTERED_INDEX_SIZE	222
#define TEST_SUM_OF_OTHER_INDEX_SIZES	333

#define TEST_IDX1_NAME			"tidx1"
#define TEST_IDX1_COL1_NAME		"tidx1_col1"
#define TEST_IDX1_INDEX_SIZE		123
#define TEST_IDX1_N_LEAF_PAGES		234
#define TEST_IDX1_N_DIFF1		50
#define TEST_IDX1_N_DIFF1_SAMPLE_SIZE	500

#define TEST_IDX2_NAME			"tidx2"
#define TEST_IDX2_COL1_NAME		"tidx2_col1"
#define TEST_IDX2_COL2_NAME		"tidx2_col2"
#define TEST_IDX2_COL3_NAME		"tidx2_col3"
#define TEST_IDX2_COL4_NAME		"tidx2_col4"
#define TEST_IDX2_INDEX_SIZE		321
#define TEST_IDX2_N_LEAF_PAGES		432
#define TEST_IDX2_N_DIFF1		60
#define TEST_IDX2_N_DIFF1_SAMPLE_SIZE	600
#define TEST_IDX2_N_DIFF2		61
#define TEST_IDX2_N_DIFF2_SAMPLE_SIZE	610
#define TEST_IDX2_N_DIFF3		62
#define TEST_IDX2_N_DIFF3_SAMPLE_SIZE	620
#define TEST_IDX2_N_DIFF4		63
#define TEST_IDX2_N_DIFF4_SAMPLE_SIZE	630
/* @} */

/* test_dict_stats_save() @{ */
void
test_dict_stats_save()
{
	dict_table_t	table;
	dict_index_t	index1;
	dict_field_t	index1_fields[1];
	uint64_t	index1_stat_n_diff_key_vals[1];
	uint64_t	index1_stat_n_sample_sizes[1];
	dict_index_t	index2;
	dict_field_t	index2_fields[4];
	uint64_t	index2_stat_n_diff_key_vals[4];
	uint64_t	index2_stat_n_sample_sizes[4];
	dberr_t		ret;

	/* craft a dummy dict_table_t */
	table.name.m_name = (char*) (TEST_DATABASE_NAME "/" TEST_TABLE_NAME);
	table.stat_n_rows = TEST_N_ROWS;
	table.stat_clustered_index_size = TEST_CLUSTERED_INDEX_SIZE;
	table.stat_sum_of_other_index_sizes = TEST_SUM_OF_OTHER_INDEX_SIZES;
	UT_LIST_INIT(table.indexes, &dict_index_t::indexes);
#ifdef BTR_CUR_HASH_ADAPT
	UT_LIST_INIT(table.freed_indexes, &dict_index_t::indexes);
#endif /* BTR_CUR_HASH_ADAPT */
	UT_LIST_ADD_LAST(table.indexes, &index1);
	UT_LIST_ADD_LAST(table.indexes, &index2);
	ut_d(table.magic_n = DICT_TABLE_MAGIC_N);
	ut_d(index1.magic_n = DICT_INDEX_MAGIC_N);

	index1.name = TEST_IDX1_NAME;
	index1.table = &table;
	index1.cached = 1;
	index1.n_uniq = 1;
	index1.fields = index1_fields;
	index1.stat_n_diff_key_vals = index1_stat_n_diff_key_vals;
	index1.stat_n_sample_sizes = index1_stat_n_sample_sizes;
	index1.stat_index_size = TEST_IDX1_INDEX_SIZE;
	index1.stat_n_leaf_pages = TEST_IDX1_N_LEAF_PAGES;
	index1_fields[0].name = TEST_IDX1_COL1_NAME;
	index1_stat_n_diff_key_vals[0] = TEST_IDX1_N_DIFF1;
	index1_stat_n_sample_sizes[0] = TEST_IDX1_N_DIFF1_SAMPLE_SIZE;

	ut_d(index2.magic_n = DICT_INDEX_MAGIC_N);
	index2.name = TEST_IDX2_NAME;
	index2.table = &table;
	index2.cached = 1;
	index2.n_uniq = 4;
	index2.fields = index2_fields;
	index2.stat_n_diff_key_vals = index2_stat_n_diff_key_vals;
	index2.stat_n_sample_sizes = index2_stat_n_sample_sizes;
	index2.stat_index_size = TEST_IDX2_INDEX_SIZE;
	index2.stat_n_leaf_pages = TEST_IDX2_N_LEAF_PAGES;
	index2_fields[0].name = TEST_IDX2_COL1_NAME;
	index2_fields[1].name = TEST_IDX2_COL2_NAME;
	index2_fields[2].name = TEST_IDX2_COL3_NAME;
	index2_fields[3].name = TEST_IDX2_COL4_NAME;
	index2_stat_n_diff_key_vals[0] = TEST_IDX2_N_DIFF1;
	index2_stat_n_diff_key_vals[1] = TEST_IDX2_N_DIFF2;
	index2_stat_n_diff_key_vals[2] = TEST_IDX2_N_DIFF3;
	index2_stat_n_diff_key_vals[3] = TEST_IDX2_N_DIFF4;
	index2_stat_n_sample_sizes[0] = TEST_IDX2_N_DIFF1_SAMPLE_SIZE;
	index2_stat_n_sample_sizes[1] = TEST_IDX2_N_DIFF2_SAMPLE_SIZE;
	index2_stat_n_sample_sizes[2] = TEST_IDX2_N_DIFF3_SAMPLE_SIZE;
	index2_stat_n_sample_sizes[3] = TEST_IDX2_N_DIFF4_SAMPLE_SIZE;

	ret = dict_stats_save(&table);

	ut_a(ret == DB_SUCCESS);

	printf("\nOK: stats saved successfully, now go ahead and read"
	       " what's inside %s and %s:\n\n",
	       TABLE_STATS_NAME_PRINT,
	       INDEX_STATS_NAME_PRINT);

	printf("SELECT COUNT(*) = 1 AS table_stats_saved_successfully\n"
	       "FROM %s\n"
	       "WHERE\n"
	       "database_name = '%s' AND\n"
	       "table_name = '%s' AND\n"
	       "n_rows = %d AND\n"
	       "clustered_index_size = %d AND\n"
	       "sum_of_other_index_sizes = %d;\n"
	       "\n",
	       TABLE_STATS_NAME_PRINT,
	       TEST_DATABASE_NAME,
	       TEST_TABLE_NAME,
	       TEST_N_ROWS,
	       TEST_CLUSTERED_INDEX_SIZE,
	       TEST_SUM_OF_OTHER_INDEX_SIZES);

	printf("SELECT COUNT(*) = 3 AS tidx1_stats_saved_successfully\n"
	       "FROM %s\n"
	       "WHERE\n"
	       "database_name = '%s' AND\n"
	       "table_name = '%s' AND\n"
	       "index_name = '%s' AND\n"
	       "(\n"
	       " (stat_name = 'size' AND stat_value = %d AND"
	       "  sample_size IS NULL) OR\n"
	       " (stat_name = 'n_leaf_pages' AND stat_value = %d AND"
	       "  sample_size IS NULL) OR\n"
	       " (stat_name = 'n_diff_pfx01' AND stat_value = %d AND"
	       "  sample_size = '%d' AND stat_description = '%s')\n"
	       ");\n"
	       "\n",
	       INDEX_STATS_NAME_PRINT,
	       TEST_DATABASE_NAME,
	       TEST_TABLE_NAME,
	       TEST_IDX1_NAME,
	       TEST_IDX1_INDEX_SIZE,
	       TEST_IDX1_N_LEAF_PAGES,
	       TEST_IDX1_N_DIFF1,
	       TEST_IDX1_N_DIFF1_SAMPLE_SIZE,
	       TEST_IDX1_COL1_NAME);

	printf("SELECT COUNT(*) = 6 AS tidx2_stats_saved_successfully\n"
	       "FROM %s\n"
	       "WHERE\n"
	       "database_name = '%s' AND\n"
	       "table_name = '%s' AND\n"
	       "index_name = '%s' AND\n"
	       "(\n"
	       " (stat_name = 'size' AND stat_value = %d AND"
	       "  sample_size IS NULL) OR\n"
	       " (stat_name = 'n_leaf_pages' AND stat_value = %d AND"
	       "  sample_size IS NULL) OR\n"
	       " (stat_name = 'n_diff_pfx01' AND stat_value = %d AND"
	       "  sample_size = '%d' AND stat_description = '%s') OR\n"
	       " (stat_name = 'n_diff_pfx02' AND stat_value = %d AND"
	       "  sample_size = '%d' AND stat_description = '%s,%s') OR\n"
	       " (stat_name = 'n_diff_pfx03' AND stat_value = %d AND"
	       "  sample_size = '%d' AND stat_description = '%s,%s,%s') OR\n"
	       " (stat_name = 'n_diff_pfx04' AND stat_value = %d AND"
	       "  sample_size = '%d' AND stat_description = '%s,%s,%s,%s')\n"
	       ");\n"
	       "\n",
	       INDEX_STATS_NAME_PRINT,
	       TEST_DATABASE_NAME,
	       TEST_TABLE_NAME,
	       TEST_IDX2_NAME,
	       TEST_IDX2_INDEX_SIZE,
	       TEST_IDX2_N_LEAF_PAGES,
	       TEST_IDX2_N_DIFF1,
	       TEST_IDX2_N_DIFF1_SAMPLE_SIZE, TEST_IDX2_COL1_NAME,
	       TEST_IDX2_N_DIFF2,
	       TEST_IDX2_N_DIFF2_SAMPLE_SIZE,
	       TEST_IDX2_COL1_NAME, TEST_IDX2_COL2_NAME,
	       TEST_IDX2_N_DIFF3,
	       TEST_IDX2_N_DIFF3_SAMPLE_SIZE,
	       TEST_IDX2_COL1_NAME, TEST_IDX2_COL2_NAME, TEST_IDX2_COL3_NAME,
	       TEST_IDX2_N_DIFF4,
	       TEST_IDX2_N_DIFF4_SAMPLE_SIZE,
	       TEST_IDX2_COL1_NAME, TEST_IDX2_COL2_NAME, TEST_IDX2_COL3_NAME,
	       TEST_IDX2_COL4_NAME);
}
/* @} */

/* test_dict_stats_fetch_from_ps() @{ */
void
test_dict_stats_fetch_from_ps()
{
	dict_table_t	table;
	dict_index_t	index1;
	uint64_t	index1_stat_n_diff_key_vals[1];
	uint64_t	index1_stat_n_sample_sizes[1];
	dict_index_t	index2;
	uint64_t	index2_stat_n_diff_key_vals[4];
	uint64_t	index2_stat_n_sample_sizes[4];
	dberr_t		ret;

	/* craft a dummy dict_table_t */
	table.name.m_name = (char*) (TEST_DATABASE_NAME "/" TEST_TABLE_NAME);
	UT_LIST_INIT(table.indexes, &dict_index_t::indexes);
#ifdef BTR_CUR_HASH_ADAPT
	UT_LIST_INIT(table.freed_indexes, &dict_index_t::indexes);
#endif /* BTR_CUR_HASH_ADAPT */
	UT_LIST_ADD_LAST(table.indexes, &index1);
	UT_LIST_ADD_LAST(table.indexes, &index2);
	ut_d(table.magic_n = DICT_TABLE_MAGIC_N);

	index1.name = TEST_IDX1_NAME;
	ut_d(index1.magic_n = DICT_INDEX_MAGIC_N);
	index1.cached = 1;
	index1.n_uniq = 1;
	index1.stat_n_diff_key_vals = index1_stat_n_diff_key_vals;
	index1.stat_n_sample_sizes = index1_stat_n_sample_sizes;

	index2.name = TEST_IDX2_NAME;
	ut_d(index2.magic_n = DICT_INDEX_MAGIC_N);
	index2.cached = 1;
	index2.n_uniq = 4;
	index2.stat_n_diff_key_vals = index2_stat_n_diff_key_vals;
	index2.stat_n_sample_sizes = index2_stat_n_sample_sizes;

	ret = dict_stats_fetch_from_ps(&table);

	ut_a(ret == DB_SUCCESS);

	ut_a(table.stat_n_rows == TEST_N_ROWS);
	ut_a(table.stat_clustered_index_size == TEST_CLUSTERED_INDEX_SIZE);
	ut_a(table.stat_sum_of_other_index_sizes
	     == TEST_SUM_OF_OTHER_INDEX_SIZES);

	ut_a(index1.stat_index_size == TEST_IDX1_INDEX_SIZE);
	ut_a(index1.stat_n_leaf_pages == TEST_IDX1_N_LEAF_PAGES);
	ut_a(index1_stat_n_diff_key_vals[0] == TEST_IDX1_N_DIFF1);
	ut_a(index1_stat_n_sample_sizes[0] == TEST_IDX1_N_DIFF1_SAMPLE_SIZE);

	ut_a(index2.stat_index_size == TEST_IDX2_INDEX_SIZE);
	ut_a(index2.stat_n_leaf_pages == TEST_IDX2_N_LEAF_PAGES);
	ut_a(index2_stat_n_diff_key_vals[0] == TEST_IDX2_N_DIFF1);
	ut_a(index2_stat_n_sample_sizes[0] == TEST_IDX2_N_DIFF1_SAMPLE_SIZE);
	ut_a(index2_stat_n_diff_key_vals[1] == TEST_IDX2_N_DIFF2);
	ut_a(index2_stat_n_sample_sizes[1] == TEST_IDX2_N_DIFF2_SAMPLE_SIZE);
	ut_a(index2_stat_n_diff_key_vals[2] == TEST_IDX2_N_DIFF3);
	ut_a(index2_stat_n_sample_sizes[2] == TEST_IDX2_N_DIFF3_SAMPLE_SIZE);
	ut_a(index2_stat_n_diff_key_vals[3] == TEST_IDX2_N_DIFF4);
	ut_a(index2_stat_n_sample_sizes[3] == TEST_IDX2_N_DIFF4_SAMPLE_SIZE);

	printf("OK: fetch successful\n");
}
/* @} */

/* test_dict_stats_all() @{ */
void
test_dict_stats_all()
{
	test_dict_table_schema_check();

	test_dict_stats_save();

	test_dict_stats_fetch_from_ps();
}
/* @} */

#endif /* UNIV_ENABLE_UNIT_TEST_DICT_STATS */
/* @} */
