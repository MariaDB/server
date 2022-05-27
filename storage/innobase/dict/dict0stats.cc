/*****************************************************************************

Copyright (c) 2009, 2019, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2022, MariaDB Corporation.

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
#include "dyn0buf.h"
#include "row0sel.h"
#include "trx0trx.h"
#include "lock0lock.h"
#include "pars0pars.h"
#include <mysql_com.h>
#include "log.h"
#include "btr0btr.h"

#include <algorithm>
#include <map>
#include <vector>
#include <thread>

/* Sampling algorithm description @{

The algorithm is controlled by one number - N_SAMPLE_PAGES(index),
let it be A, which is the number of leaf pages to analyze for a given index
for each n-prefix (if the index is on 3 columns, then 3*A leaf pages will be
analyzed).

Let the total number of leaf pages in the table be T.
Level 0 - leaf pages, level H - root.

Definition: N-prefix-boring record is a record on a non-leaf page that equals
the next (to the right, cross page boundaries, skipping the supremum and
infimum) record on the same level when looking at the fist n-prefix columns.
The last (user) record on a level is not boring (it does not match the
non-existent user record to the right). We call the records boring because all
the records on the page below a boring record are equal to that boring record.

We avoid diving below boring records when searching for a leaf page to
estimate the number of distinct records because we know that such a leaf
page will have number of distinct records == 1.

For each n-prefix: start from the root level and full scan subsequent lower
levels until a level that contains at least A*10 distinct records is found.
Lets call this level LA.
As an optimization the search is canceled if it has reached level 1 (never
descend to the level 0 (leaf)) and also if the next level to be scanned
would contain more than A pages. The latter is because the user has asked
to analyze A leaf pages and it does not make sense to scan much more than
A non-leaf pages with the sole purpose of finding a good sample of A leaf
pages.

After finding the appropriate level LA with >A*10 distinct records (or less in
the exceptions described above), divide it into groups of equal records and
pick A such groups. Then pick the last record from each group. For example,
let the level be:

index:  0,1,2,3,4,5,6,7,8,9,10
record: 1,1,1,2,2,7,7,7,7,7,9

There are 4 groups of distinct records and if A=2 random ones are selected,
e.g. 1,1,1 and 7,7,7,7,7, then records with indexes 2 and 9 will be selected.

After selecting A records as described above, dive below them to find A leaf
pages and analyze them, finding the total number of distinct records. The
dive to the leaf level is performed by selecting a non-boring record from
each page and diving below it.

This way, a total of A leaf pages are analyzed for the given n-prefix.

Let the number of different key values found in each leaf page i be Pi (i=1..A).
Let N_DIFF_AVG_LEAF be (P1 + P2 + ... + PA) / A.
Let the number of different key values on level LA be N_DIFF_LA.
Let the total number of records on level LA be TOTAL_LA.
Let R be N_DIFF_LA / TOTAL_LA, we assume this ratio is the same on the
leaf level.
Let the number of leaf pages be N.
Then the total number of different key values on the leaf level is:
N * R * N_DIFF_AVG_LEAF.
See REF01 for the implementation.

The above describes how to calculate the cardinality of an index.
This algorithm is executed for each n-prefix of a multi-column index
where n=1..n_uniq.
@} */

/* names of the tables from the persistent statistics storage */
#define TABLE_STATS_NAME_PRINT	"mysql.innodb_table_stats"
#define INDEX_STATS_NAME_PRINT	"mysql.innodb_index_stats"

#ifdef UNIV_STATS_DEBUG
#define DEBUG_PRINTF(fmt, ...)	printf(fmt, ## __VA_ARGS__)
#else /* UNIV_STATS_DEBUG */
#define DEBUG_PRINTF(fmt, ...)	/* noop */
#endif /* UNIV_STATS_DEBUG */

/* Gets the number of leaf pages to sample in persistent stats estimation */
#define N_SAMPLE_PAGES(index)					\
	static_cast<ib_uint64_t>(				\
		(index)->table->stats_sample_pages != 0		\
		? (index)->table->stats_sample_pages		\
		: srv_stats_persistent_sample_pages)

/* number of distinct records on a given level that are required to stop
descending to lower levels and fetch N_SAMPLE_PAGES(index) records
from that level */
#define N_DIFF_REQUIRED(index)	(N_SAMPLE_PAGES(index) * 10)

/* A dynamic array where we store the boundaries of each distinct group
of keys. For example if a btree level is:
index: 0,1,2,3,4,5,6,7,8,9,10,11,12
data:  b,b,b,b,b,b,g,g,j,j,j, x, y
then we would store 5,7,10,11,12 in the array. */
typedef std::vector<ib_uint64_t, ut_allocator<ib_uint64_t> >	boundaries_t;

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
	return((index->type & (DICT_FTS | DICT_SPATIAL))
	       || index->is_corrupted()
	       || index->to_be_dropped
	       || !index->is_committed());
}


/** expected column definition */
struct dict_col_meta_t
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
struct dict_table_schema_t
{
  /** table name */
  span<const char> table_name;
  /** table name in SQL */
  const char *table_name_sql;
  /** number of columns */
  unsigned n_cols;
  /** columns */
  const dict_col_meta_t columns[8];
};

static const dict_table_schema_t table_stats_schema =
{
  {C_STRING_WITH_LEN(TABLE_STATS_NAME)}, TABLE_STATS_NAME_PRINT, 6,
  {
    {"database_name", DATA_VARMYSQL, DATA_NOT_NULL, 192},
    {"table_name", DATA_VARMYSQL, DATA_NOT_NULL, 597},
    {"last_update", DATA_INT, DATA_NOT_NULL | DATA_UNSIGNED, 4},
    {"n_rows", DATA_INT, DATA_NOT_NULL | DATA_UNSIGNED, 8},
    {"clustered_index_size", DATA_INT, DATA_NOT_NULL | DATA_UNSIGNED, 8},
    {"sum_of_other_index_sizes", DATA_INT, DATA_NOT_NULL | DATA_UNSIGNED, 8},
  }
};

static const dict_table_schema_t index_stats_schema =
{
  {C_STRING_WITH_LEN(INDEX_STATS_NAME)}, INDEX_STATS_NAME_PRINT, 8,
  {
    {"database_name", DATA_VARMYSQL, DATA_NOT_NULL, 192},
    {"table_name", DATA_VARMYSQL, DATA_NOT_NULL, 597},
    {"index_name", DATA_VARMYSQL, DATA_NOT_NULL, 192},
    {"last_update", DATA_INT, DATA_NOT_NULL | DATA_UNSIGNED, 4},
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

static bool innodb_table_stats_not_found;
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
	const dict_table_schema_t* req_schema,	/*!< in: required table
						schema */
	char*			errstr,		/*!< out: human readable error
						message if != DB_SUCCESS is
						returned */
	size_t			errstr_sz)	/*!< in: errstr size */
{
	const dict_table_t* table= dict_sys.load_table(req_schema->table_name);

	if (!table) {
		if (req_schema == &table_stats_schema) {
			if (innodb_table_stats_not_found_reported) {
				return DB_STATS_DO_NOT_EXIST;
			}
			innodb_table_stats_not_found = true;
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
		return DB_TABLE_NOT_FOUND;
	}

	if (!table->is_readable() && !table->space) {
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
			sql_print_warning("InnoDB: Table %s has"
					  " length mismatch in the"
					  " column name %s."
					  " Please run mariadb-upgrade",
					  req_schema->table_name_sql,
					  req_schema->columns[i].name);
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

/*********************************************************************//**
Checks whether the persistent statistics storage exists and that all
tables have the proper structure.
@return true if exists and all tables are ok */
static bool dict_stats_persistent_storage_check(bool dict_already_locked)
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

	if (ret != DB_SUCCESS && ret != DB_STATS_DO_NOT_EXIST) {
		ib::error() << errstr;
		return(false);
	} else if (ret == DB_STATS_DO_NOT_EXIST) {
		return false;
	}
	/* else */

	return(true);
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

  if (!dict_stats_persistent_storage_check(true))
  {
    pars_info_free(pinfo);
    return DB_STATS_DO_NOT_EXIST;
  }

  return que_eval_sql(pinfo, sql, trx);
}

/*********************************************************************//**
Duplicate a table object and its indexes.
This function creates a dummy dict_table_t object and initializes the
following table and index members:
dict_table_t::id (copied)
dict_table_t::heap (newly created)
dict_table_t::name (copied)
dict_table_t::corrupted (copied)
dict_table_t::indexes<> (newly created)
dict_table_t::magic_n
for each entry in dict_table_t::indexes, the following are initialized:
(indexes that have DICT_FTS set in index->type are skipped)
dict_index_t::id (copied)
dict_index_t::name (copied)
dict_index_t::table_name (points to the copied table name)
dict_index_t::table (points to the above semi-initialized object)
dict_index_t::type (copied)
dict_index_t::to_be_dropped (copied)
dict_index_t::online_status (copied)
dict_index_t::n_uniq (copied)
dict_index_t::fields[] (newly created, only first n_uniq, only fields[i].name)
dict_index_t::indexes<> (newly created)
dict_index_t::stat_n_diff_key_vals[] (only allocated, left uninitialized)
dict_index_t::stat_n_sample_sizes[] (only allocated, left uninitialized)
dict_index_t::stat_n_non_null_key_vals[] (only allocated, left uninitialized)
dict_index_t::magic_n
The returned object should be freed with dict_stats_table_clone_free()
when no longer needed.
@return incomplete table object */
static
dict_table_t*
dict_stats_table_clone_create(
/*==========================*/
	const dict_table_t*	table)	/*!< in: table whose stats to copy */
{
	size_t		heap_size;
	dict_index_t*	index;

	/* Estimate the size needed for the table and all of its indexes */

	heap_size = 0;
	heap_size += sizeof(dict_table_t);
	heap_size += strlen(table->name.m_name) + 1;

	for (index = dict_table_get_first_index(table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {

		if (dict_stats_should_ignore_index(index)) {
			continue;
		}

		ut_ad(!dict_index_is_ibuf(index));

		ulint	n_uniq = dict_index_get_n_unique(index);

		heap_size += sizeof(dict_index_t);
		heap_size += strlen(index->name) + 1;
		heap_size += n_uniq * sizeof(index->fields[0]);
		for (ulint i = 0; i < n_uniq; i++) {
			heap_size += strlen(index->fields[i].name) + 1;
		}
		heap_size += n_uniq * sizeof(index->stat_n_diff_key_vals[0]);
		heap_size += n_uniq * sizeof(index->stat_n_sample_sizes[0]);
		heap_size += n_uniq * sizeof(index->stat_n_non_null_key_vals[0]);
	}

	/* Allocate the memory and copy the members */

	mem_heap_t*	heap;

	heap = mem_heap_create(heap_size);

	dict_table_t*	t;

	t = (dict_table_t*) mem_heap_zalloc(heap, sizeof(*t));

	t->stats_mutex_init();

	MEM_CHECK_DEFINED(&table->id, sizeof(table->id));
	t->id = table->id;

	t->heap = heap;

	t->name.m_name = mem_heap_strdup(heap, table->name.m_name);
	t->mdl_name.m_name = t->name.m_name;

	t->corrupted = table->corrupted;

	UT_LIST_INIT(t->indexes, &dict_index_t::indexes);
#ifdef BTR_CUR_HASH_ADAPT
	UT_LIST_INIT(t->freed_indexes, &dict_index_t::indexes);
#endif /* BTR_CUR_HASH_ADAPT */

	for (index = dict_table_get_first_index(table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {

		if (dict_stats_should_ignore_index(index)) {
			continue;
		}

		ut_ad(!dict_index_is_ibuf(index));

		dict_index_t*	idx;

		idx = (dict_index_t*) mem_heap_zalloc(heap, sizeof(*idx));

		MEM_CHECK_DEFINED(&index->id, sizeof(index->id));
		idx->id = index->id;

		idx->name = mem_heap_strdup(heap, index->name);

		idx->table = t;

		idx->type = index->type;

		idx->to_be_dropped = 0;

		idx->online_status = ONLINE_INDEX_COMPLETE;
		idx->set_committed(true);

		idx->n_uniq = index->n_uniq;

		idx->fields = (dict_field_t*) mem_heap_zalloc(
			heap, idx->n_uniq * sizeof(idx->fields[0]));

		for (ulint i = 0; i < idx->n_uniq; i++) {
			idx->fields[i].name = mem_heap_strdup(
				heap, index->fields[i].name);
		}

		/* hook idx into t->indexes */
		UT_LIST_ADD_LAST(t->indexes, idx);

		idx->stat_n_diff_key_vals = (ib_uint64_t*) mem_heap_zalloc(
			heap,
			idx->n_uniq * sizeof(idx->stat_n_diff_key_vals[0]));

		idx->stat_n_sample_sizes = (ib_uint64_t*) mem_heap_zalloc(
			heap,
			idx->n_uniq * sizeof(idx->stat_n_sample_sizes[0]));

		idx->stat_n_non_null_key_vals = (ib_uint64_t*) mem_heap_zalloc(
			heap,
			idx->n_uniq * sizeof(idx->stat_n_non_null_key_vals[0]));
		ut_d(idx->magic_n = DICT_INDEX_MAGIC_N);

		idx->stat_defrag_n_page_split = 0;
		idx->stat_defrag_n_pages_freed = 0;
	}

	ut_d(t->magic_n = DICT_TABLE_MAGIC_N);

	return(t);
}

/*********************************************************************//**
Free the resources occupied by an object returned by
dict_stats_table_clone_create(). */
static
void
dict_stats_table_clone_free(
/*========================*/
	dict_table_t*	t)	/*!< in: dummy table object to free */
{
	t->stats_mutex_destroy();
	mem_heap_free(t->heap);
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

/*********************************************************************//**
Write all zeros (or 1 where it makes sense) into a table and its indexes'
statistics members. The resulting stats correspond to an empty table. */
static
void
dict_stats_empty_table(
/*===================*/
	dict_table_t*	table,	/*!< in/out: table */
	bool		empty_defrag_stats)
				/*!< in: whether to empty defrag stats */
{
	/* Initialize table/index level stats is now protected by
	table level lock_mutex.*/
	table->stats_mutex_lock();

	/* Zero the stats members */
	table->stat_n_rows = 0;
	table->stat_clustered_index_size = 1;
	/* 1 page for each index, not counting the clustered */
	table->stat_sum_of_other_index_sizes
		= UT_LIST_GET_LEN(table->indexes) - 1;
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

	table->stat_initialized = TRUE;
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
	ut_a(table->stat_initialized);

	MEM_CHECK_DEFINED(&table->stats_last_recalc,
			  sizeof table->stats_last_recalc);

	MEM_CHECK_DEFINED(&table->stat_persistent,
			  sizeof table->stat_persistent);

	MEM_CHECK_DEFINED(&table->stats_auto_recalc,
			  sizeof table->stats_auto_recalc);

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

/*********************************************************************//**
Copy table and index statistics from one table to another, including index
stats. Extra indexes in src are ignored and extra indexes in dst are
initialized to correspond to an empty index. */
static
void
dict_stats_copy(
/*============*/
	dict_table_t*		dst,	/*!< in/out: destination table */
	const dict_table_t*	src,	/*!< in: source table */
	bool reset_ignored_indexes)	/*!< in: if true, set ignored indexes
                                             to have the same statistics as if
                                             the table was empty */
{
	ut_ad(src->stats_mutex_is_owner());
	ut_ad(dst->stats_mutex_is_owner());

	dst->stats_last_recalc = src->stats_last_recalc;
	dst->stat_n_rows = src->stat_n_rows;
	dst->stat_clustered_index_size = src->stat_clustered_index_size;
	dst->stat_sum_of_other_index_sizes = src->stat_sum_of_other_index_sizes;
	dst->stat_modified_counter = src->stat_modified_counter;

	dict_index_t*	dst_idx;
	dict_index_t*	src_idx;

	for (dst_idx = dict_table_get_first_index(dst),
	     src_idx = dict_table_get_first_index(src);
	     dst_idx != NULL;
	     dst_idx = dict_table_get_next_index(dst_idx),
	     (src_idx != NULL
	      && (src_idx = dict_table_get_next_index(src_idx)))) {

		if (dict_stats_should_ignore_index(dst_idx)) {
			if (reset_ignored_indexes) {
				/* Reset index statistics for all ignored indexes,
				unless they are FT indexes (these have no statistics)*/
				if (dst_idx->type & DICT_FTS) {
					continue;
				}
				dict_stats_empty_index(dst_idx, true);
			} else {
				continue;
			}
		}

		ut_ad(!dict_index_is_ibuf(dst_idx));

		if (!INDEX_EQ(src_idx, dst_idx)) {
			for (src_idx = dict_table_get_first_index(src);
			     src_idx != NULL;
			     src_idx = dict_table_get_next_index(src_idx)) {

				if (INDEX_EQ(src_idx, dst_idx)) {
					break;
				}
			}
		}

		if (!INDEX_EQ(src_idx, dst_idx)) {
			dict_stats_empty_index(dst_idx, true);
			continue;
		}

		ulint	n_copy_el;

		if (dst_idx->n_uniq > src_idx->n_uniq) {
			n_copy_el = src_idx->n_uniq;
			/* Since src is smaller some elements in dst
			will remain untouched by the following memmove(),
			thus we init all of them here. */
			dict_stats_empty_index(dst_idx, true);
		} else {
			n_copy_el = dst_idx->n_uniq;
		}

		memmove(dst_idx->stat_n_diff_key_vals,
			src_idx->stat_n_diff_key_vals,
			n_copy_el * sizeof(dst_idx->stat_n_diff_key_vals[0]));

		memmove(dst_idx->stat_n_sample_sizes,
			src_idx->stat_n_sample_sizes,
			n_copy_el * sizeof(dst_idx->stat_n_sample_sizes[0]));

		memmove(dst_idx->stat_n_non_null_key_vals,
			src_idx->stat_n_non_null_key_vals,
			n_copy_el * sizeof(dst_idx->stat_n_non_null_key_vals[0]));

		dst_idx->stat_index_size = src_idx->stat_index_size;

		dst_idx->stat_n_leaf_pages = src_idx->stat_n_leaf_pages;

		dst_idx->stat_defrag_modified_counter =
			src_idx->stat_defrag_modified_counter;
		dst_idx->stat_defrag_n_pages_freed =
			src_idx->stat_defrag_n_pages_freed;
		dst_idx->stat_defrag_n_page_split =
			src_idx->stat_defrag_n_page_split;
	}

	dst->stat_initialized = TRUE;
}

/** Duplicate the stats of a table and its indexes.
This function creates a dummy dict_table_t object and copies the input
table's stats into it. The returned table object is not in the dictionary
cache and cannot be accessed by any other threads. In addition to the
members copied in dict_stats_table_clone_create() this function initializes
the following:
dict_table_t::stat_initialized
dict_table_t::stat_persistent
dict_table_t::stat_n_rows
dict_table_t::stat_clustered_index_size
dict_table_t::stat_sum_of_other_index_sizes
dict_table_t::stat_modified_counter
dict_index_t::stat_n_diff_key_vals[]
dict_index_t::stat_n_sample_sizes[]
dict_index_t::stat_n_non_null_key_vals[]
dict_index_t::stat_index_size
dict_index_t::stat_n_leaf_pages
dict_index_t::stat_defrag_modified_counter
dict_index_t::stat_defrag_n_pages_freed
dict_index_t::stat_defrag_n_page_split
The returned object should be freed with dict_stats_snapshot_free()
when no longer needed.
@param[in]	table	table whose stats to copy
@return incomplete table object */
static
dict_table_t*
dict_stats_snapshot_create(
	dict_table_t*	table)
{
	dict_sys.lock(SRW_LOCK_CALL);

	dict_stats_assert_initialized(table);

	dict_table_t*	t;

	t = dict_stats_table_clone_create(table);

	table->stats_mutex_lock();
	ut_d(t->stats_mutex_lock());

	dict_stats_copy(t, table, false);

	ut_d(t->stats_mutex_unlock());
	table->stats_mutex_unlock();

	t->stat_persistent = table->stat_persistent;
	t->stats_auto_recalc = table->stats_auto_recalc;
	t->stats_sample_pages = table->stats_sample_pages;

	dict_sys.unlock();

	return(t);
}

/*********************************************************************//**
Free the resources occupied by an object returned by
dict_stats_snapshot_create(). */
static
void
dict_stats_snapshot_free(
/*=====================*/
	dict_table_t*	t)	/*!< in: dummy table object to free */
{
	dict_stats_table_clone_free(t);
}

/** Statistics for one field of an index. */
struct index_field_stats_t
{
  ib_uint64_t n_diff_key_vals;
  ib_uint64_t n_sample_sizes;
  ib_uint64_t n_non_null_key_vals;

  index_field_stats_t(ib_uint64_t n_diff_key_vals= 0,
                      ib_uint64_t n_sample_sizes= 0,
                      ib_uint64_t n_non_null_key_vals= 0)
      : n_diff_key_vals(n_diff_key_vals), n_sample_sizes(n_sample_sizes),
        n_non_null_key_vals(n_non_null_key_vals)
  {
  }
};

/*******************************************************************//**
Record the number of non_null key values in a given index for
each n-column prefix of the index where 1 <= n <= dict_index_get_n_unique(index).
The estimates are eventually stored in the array:
index->stat_n_non_null_key_vals[], which is indexed from 0 to n-1. */
static
void
btr_record_not_null_field_in_rec(
/*=============================*/
	ulint		n_unique,	/*!< in: dict_index_get_n_unique(index),
					number of columns uniquely determine
					an index entry */
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec, index),
					its size could be for all fields or
					that of "n_unique" */
	ib_uint64_t*	n_not_null)	/*!< in/out: array to record number of
					not null rows for n-column prefix */
{
	ulint	i;

	ut_ad(rec_offs_n_fields(offsets) >= n_unique);

	if (n_not_null == NULL) {
		return;
	}

	for (i = 0; i < n_unique; i++) {
		if (rec_offs_nth_sql_null(offsets, i)) {
			break;
		}

		n_not_null[i]++;
	}
}

/** Estimated table level stats from sampled value.
@param value sampled stats
@param index index being sampled
@param sample number of sampled rows
@param ext_size external stored data size
@param not_empty table not empty
@return estimated table wide stats from sampled value */
#define BTR_TABLE_STATS_FROM_SAMPLE(value, index, sample, ext_size, not_empty) \
	(((value) * static_cast<ib_uint64_t>(index->stat_n_leaf_pages) \
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
std::vector<index_field_stats_t>
btr_estimate_number_of_different_key_vals(dict_index_t* index,
					  trx_id_t bulk_trx_id)
{
	btr_cur_t	cursor;
	page_t*		page;
	rec_t*		rec;
	ulint		n_cols;
	ib_uint64_t*	n_diff;
	ib_uint64_t*	n_not_null;
	ibool		stats_null_not_equal;
	uintmax_t	n_sample_pages=1; /* number of pages to sample */
	ulint		not_empty_flag	= 0;
	ulint		total_external_size = 0;
	ulint		i;
	ulint		j;
	uintmax_t	add_on;
	mtr_t		mtr;
	mem_heap_t*	heap		= NULL;
	rec_offs*	offsets_rec	= NULL;
	rec_offs*	offsets_next_rec = NULL;

	std::vector<index_field_stats_t> result;

	ut_ad(index->is_btree());

	n_cols = dict_index_get_n_unique(index);

	heap = mem_heap_create((sizeof *n_diff + sizeof *n_not_null)
			       * n_cols
			       + dict_index_get_n_fields(index)
			       * (sizeof *offsets_rec
				  + sizeof *offsets_next_rec));

	n_diff = (ib_uint64_t*) mem_heap_zalloc(
		heap, n_cols * sizeof(n_diff[0]));

	n_not_null = NULL;

	/* Check srv_innodb_stats_method setting, and decide whether we
	need to record non-null value and also decide if NULL is
	considered equal (by setting stats_null_not_equal value) */
	switch (srv_innodb_stats_method) {
	case SRV_STATS_NULLS_IGNORED:
		n_not_null = (ib_uint64_t*) mem_heap_zalloc(
			heap, n_cols * sizeof *n_not_null);
		/* fall through */

	case SRV_STATS_NULLS_UNEQUAL:
		/* for both SRV_STATS_NULLS_IGNORED and SRV_STATS_NULLS_UNEQUAL
		case, we will treat NULLs as unequal value */
		stats_null_not_equal = TRUE;
		break;

	case SRV_STATS_NULLS_EQUAL:
		stats_null_not_equal = FALSE;
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
		if (index->stat_index_size > 1) {
			n_sample_pages = (srv_stats_transient_sample_pages < index->stat_index_size)
				? ut_min(index->stat_index_size,
					 static_cast<ulint>(
						 log2(double(index->stat_index_size))
						 * double(srv_stats_transient_sample_pages)))
				: index->stat_index_size;
		}
	}

	/* Sanity check */
	ut_ad(n_sample_pages > 0 && n_sample_pages <= (index->stat_index_size <= 1 ? 1 : index->stat_index_size));

	/* We sample some pages in the index to get an estimate */

	for (i = 0; i < n_sample_pages; i++) {
		mtr.start();

		bool	available;

		available = btr_cur_open_at_rnd_pos(index, BTR_SEARCH_LEAF,
						    &cursor, &mtr);

		if (!available || index->table->bulk_trx_id != bulk_trx_id) {
			mtr.commit();
			mem_heap_free(heap);

			return result;
		}

		/* Count the number of different key values for each prefix of
		the key on this index page. If the prefix does not determine
		the index record uniquely in the B-tree, then we subtract one
		because otherwise our algorithm would give a wrong estimate
		for an index where there is just one key value. */

		if (!index->is_readable()) {
			mtr.commit();
			goto exit_loop;
		}

		page = btr_cur_get_page(&cursor);

		rec = page_rec_get_next(page_get_infimum_rec(page));
		const ulint n_core = page_is_leaf(page)
			? index->n_core_fields : 0;

		if (!page_rec_is_supremum(rec)) {
			not_empty_flag = 1;
			offsets_rec = rec_get_offsets(rec, index, offsets_rec,
						      n_core,
						      ULINT_UNDEFINED, &heap);

			if (n_not_null != NULL) {
				btr_record_not_null_field_in_rec(
					n_cols, offsets_rec, n_not_null);
			}
		}

		while (!page_rec_is_supremum(rec)) {
			ulint	matched_fields;
			rec_t*	next_rec = page_rec_get_next(rec);
			if (page_rec_is_supremum(next_rec)) {
				total_external_size +=
					btr_rec_get_externally_stored_len(
						rec, offsets_rec);
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

			for (j = matched_fields; j < n_cols; j++) {
				/* We add one if this index record has
				a different prefix from the previous */

				n_diff[j]++;
			}

			if (n_not_null != NULL) {
				btr_record_not_null_field_in_rec(
					n_cols, offsets_next_rec, n_not_null);
			}

			total_external_size
				+= btr_rec_get_externally_stored_len(
					rec, offsets_rec);

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

	for (j = 0; j < n_cols; j++) {
		index_field_stats_t stat;

		stat.n_diff_key_vals
			= BTR_TABLE_STATS_FROM_SAMPLE(
				n_diff[j], index, n_sample_pages,
				total_external_size, not_empty_flag);

		/* If the tree is small, smaller than
		10 * n_sample_pages + total_external_size, then
		the above estimate is ok. For bigger trees it is common that we
		do not see any borders between key values in the few pages
		we pick. But still there may be n_sample_pages
		different key values, or even more. Let us try to approximate
		that: */

		add_on = index->stat_n_leaf_pages
			/ (10 * (n_sample_pages
				 + total_external_size));

		if (add_on > n_sample_pages) {
			add_on = n_sample_pages;
		}

		stat.n_diff_key_vals += add_on;

		stat.n_sample_sizes = n_sample_pages;

		if (n_not_null != NULL) {
			stat.n_non_null_key_vals =
				 BTR_TABLE_STATS_FROM_SAMPLE(
					n_not_null[j], index, n_sample_pages,
					total_external_size, not_empty_flag);
		}

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
Only persistent statistics supports defragmentation stats. */
static
void
dict_stats_update_transient_for_index(
/*==================================*/
	dict_index_t*	index)	/*!< in/out: index */
{
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
#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
	} else if (ibuf_debug && !dict_index_is_clust(index)) {
		goto dummy_empty;
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */
	} else if (dict_index_is_online_ddl(index) || !index->is_committed()
		   || !index->table->space) {
		goto dummy_empty;
	} else {
		mtr_t	mtr;

		mtr.start();
		mtr_s_lock_index(index, &mtr);
		buf_block_t* root = btr_root_block_get(index, RW_SX_LATCH,
						       &mtr);
		if (!root) {
invalid:
			mtr.commit();
			goto dummy_empty;
		}

		const auto bulk_trx_id = index->table->bulk_trx_id;
		if (bulk_trx_id && trx_sys.find(nullptr, bulk_trx_id, false)) {
			goto invalid;
		}

		mtr.x_lock_space(index->table->space);

		ulint dummy, size;
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
			std::vector<index_field_stats_t> stats
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
}

/*********************************************************************//**
Calculates new estimates for table and index statistics. This function
is relatively quick and is used to calculate transient statistics that
are not saved on disk.
This was the only way to calculate statistics before the
Persistent Statistics feature was introduced. */
static
void
dict_stats_update_transient(
/*========================*/
	dict_table_t*	table)	/*!< in/out: table */
{
	ut_ad(!table->stats_mutex_is_owner());

	dict_index_t*	index;
	ulint		sum_of_index_sizes	= 0;

	/* Find out the sizes of the indexes and how many different values
	for the key they approximately have */

	index = dict_table_get_first_index(table);

	if (!table->space) {
		/* Nothing to do. */
		dict_stats_empty_table(table, true);
		return;
	} else if (index == NULL) {
		/* Table definition is corrupt */

		ib::warn() << "Table " << table->name
			<< " has no indexes. Cannot calculate statistics.";
		dict_stats_empty_table(table, true);
		return;
	}

	for (; index != NULL; index = dict_table_get_next_index(index)) {

		ut_ad(!dict_index_is_ibuf(index));

		if (!index->is_btree()) {
			continue;
		}

		if (dict_stats_should_ignore_index(index)
		    || !index->is_readable()) {
			index->table->stats_mutex_lock();
			dict_stats_empty_index(index, false);
			index->table->stats_mutex_unlock();
			continue;
		}

		dict_stats_update_transient_for_index(index);

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

	table->stat_initialized = TRUE;

	table->stats_mutex_unlock();
}

/* @{ Pseudo code about the relation between the following functions

let N = N_SAMPLE_PAGES(index)

dict_stats_analyze_index()
  for each n_prefix
    search for good enough level:
      dict_stats_analyze_index_level() // only called if level has <= N pages
        // full scan of the level in one mtr
        collect statistics about the given level
      if we are not satisfied with the level, search next lower level
    we have found a good enough level here
    dict_stats_analyze_index_for_n_prefix(that level, stats collected above)
      // full scan of the level in one mtr
      dive below some records and analyze the leaf page there:
      dict_stats_analyze_index_below_cur()
@} */

/*********************************************************************//**
Find the total number and the number of distinct keys on a given level in
an index. Each of the 1..n_uniq prefixes are looked up and the results are
saved in the array n_diff[0] .. n_diff[n_uniq - 1]. The total number of
records on the level is saved in total_recs.
Also, the index of the last record in each group of equal records is saved
in n_diff_boundaries[0..n_uniq - 1], records indexing starts from the leftmost
record on the level and continues cross pages boundaries, counting from 0. */
static
void
dict_stats_analyze_index_level(
/*===========================*/
	dict_index_t*	index,		/*!< in: index */
	ulint		level,		/*!< in: level */
	ib_uint64_t*	n_diff,		/*!< out: array for number of
					distinct keys for all prefixes */
	ib_uint64_t*	total_recs,	/*!< out: total number of records */
	ib_uint64_t*	total_pages,	/*!< out: total number of pages */
	boundaries_t*	n_diff_boundaries,/*!< out: boundaries of the groups
					of distinct keys */
	mtr_t*		mtr)		/*!< in/out: mini-transaction */
{
	ulint		n_uniq;
	mem_heap_t*	heap;
	btr_pcur_t	pcur;
	const page_t*	page;
	const rec_t*	rec;
	const rec_t*	prev_rec;
	bool		prev_rec_is_copied;
	byte*		prev_rec_buf = NULL;
	ulint		prev_rec_buf_size = 0;
	rec_offs*	rec_offsets;
	rec_offs*	prev_rec_offsets;
	ulint		i;

	DEBUG_PRINTF("    %s(table=%s, index=%s, level=" ULINTPF ")\n",
		     __func__, index->table->name, index->name, level);

	ut_ad(mtr->memo_contains(index->lock, MTR_MEMO_SX_LOCK));

	n_uniq = dict_index_get_n_unique(index);

	/* elements in the n_diff array are 0..n_uniq-1 (inclusive) */
	memset(n_diff, 0x0, n_uniq * sizeof(n_diff[0]));

	/* Allocate space for the offsets header (the allocation size at
	offsets[0] and the REC_OFFS_HEADER_SIZE bytes), and n_uniq + 1,
	so that this will never be less than the size calculated in
	rec_get_offsets_func(). */
	i = (REC_OFFS_HEADER_SIZE + 1 + 1) + n_uniq;

	heap = mem_heap_create((2 * sizeof *rec_offsets) * i);
	rec_offsets = static_cast<rec_offs*>(
		mem_heap_alloc(heap, i * sizeof *rec_offsets));
	prev_rec_offsets = static_cast<rec_offs*>(
		mem_heap_alloc(heap, i * sizeof *prev_rec_offsets));
	rec_offs_set_n_alloc(rec_offsets, i);
	rec_offs_set_n_alloc(prev_rec_offsets, i);

	/* reset the dynamic arrays n_diff_boundaries[0..n_uniq-1] */
	if (n_diff_boundaries != NULL) {
		for (i = 0; i < n_uniq; i++) {
			n_diff_boundaries[i].erase(
				n_diff_boundaries[i].begin(),
				n_diff_boundaries[i].end());
		}
	}

	/* Position pcur on the leftmost record on the leftmost page
	on the desired level. */

	btr_pcur_open_at_index_side(
		true, index, BTR_SEARCH_TREE_ALREADY_S_LATCHED,
		&pcur, true, level, mtr);
	btr_pcur_move_to_next_on_page(&pcur);

	page = btr_pcur_get_page(&pcur);

	/* The page must not be empty, except when
	it is the root page (and the whole index is empty). */
	ut_ad(btr_pcur_is_on_user_rec(&pcur) || page_is_leaf(page));
	ut_ad(btr_pcur_get_rec(&pcur)
	      == page_rec_get_next_const(page_get_infimum_rec(page)));

	/* check that we are indeed on the desired level */
	ut_a(btr_page_get_level(page) == level);

	/* there should not be any pages on the left */
	ut_a(!page_has_prev(page));

	if (REC_INFO_MIN_REC_FLAG & rec_get_info_bits(
		    btr_pcur_get_rec(&pcur), page_is_comp(page))) {
		ut_ad(btr_pcur_is_on_user_rec(&pcur));
		if (level == 0) {
			/* Skip the metadata pseudo-record */
			ut_ad(index->is_instant());
			btr_pcur_move_to_next_user_rec(&pcur, mtr);
		}
	} else {
		/* The first record on the leftmost page must be
		marked as such on each level except the leaf level. */
		ut_a(level == 0);
	}

	prev_rec = NULL;
	prev_rec_is_copied = false;

	/* no records by default */
	*total_recs = 0;

	*total_pages = 0;

	/* iterate over all user records on this level
	and compare each two adjacent ones, even the last on page
	X and the fist on page X+1 */
	for (;
	     btr_pcur_is_on_user_rec(&pcur);
	     btr_pcur_move_to_next_user_rec(&pcur, mtr)) {

		bool	rec_is_last_on_page;

		rec = btr_pcur_get_rec(&pcur);

		/* If rec and prev_rec are on different pages, then prev_rec
		must have been copied, because we hold latch only on the page
		where rec resides. */
		if (prev_rec != NULL
		    && page_align(rec) != page_align(prev_rec)) {

			ut_a(prev_rec_is_copied);
		}

		rec_is_last_on_page =
			page_rec_is_supremum(page_rec_get_next_const(rec));

		/* increment the pages counter at the end of each page */
		if (rec_is_last_on_page) {

			(*total_pages)++;
		}

		/* Skip delete-marked records on the leaf level. If we
		do not skip them, then ANALYZE quickly after DELETE
		could count them or not (purge may have already wiped
		them away) which brings non-determinism. We skip only
		leaf-level delete marks because delete marks on
		non-leaf level do not make sense. */

		if (level == 0
		    && !srv_stats_include_delete_marked
		    && rec_get_deleted_flag(
			    rec,
			    page_is_comp(btr_pcur_get_page(&pcur)))) {

			if (rec_is_last_on_page
			    && !prev_rec_is_copied
			    && prev_rec != NULL) {
				/* copy prev_rec */

				prev_rec_offsets = rec_get_offsets(
					prev_rec, index, prev_rec_offsets,
					index->n_core_fields,
					n_uniq, &heap);

				prev_rec = rec_copy_prefix_to_buf(
					prev_rec, index, n_uniq,
					&prev_rec_buf, &prev_rec_buf_size);

				prev_rec_is_copied = true;
			}

			continue;
		}
		rec_offsets = rec_get_offsets(rec, index, rec_offsets,
					      level ? 0 : index->n_core_fields,
					      n_uniq, &heap);

		(*total_recs)++;

		if (prev_rec != NULL) {
			ulint	matched_fields;

			prev_rec_offsets = rec_get_offsets(
				prev_rec, index, prev_rec_offsets,
				level ? 0 : index->n_core_fields,
				n_uniq, &heap);

			cmp_rec_rec(prev_rec, rec,
				    prev_rec_offsets, rec_offsets, index,
				    false, &matched_fields);

			for (i = matched_fields; i < n_uniq; i++) {

				if (n_diff_boundaries != NULL) {
					/* push the index of the previous
					record, that is - the last one from
					a group of equal keys */

					ib_uint64_t	idx;

					/* the index of the current record
					is total_recs - 1, the index of the
					previous record is total_recs - 2;
					we know that idx is not going to
					become negative here because if we
					are in this branch then there is a
					previous record and thus
					total_recs >= 2 */
					idx = *total_recs - 2;

					n_diff_boundaries[i].push_back(idx);
				}

				/* increment the number of different keys
				for n_prefix=i+1 (e.g. if i=0 then we increment
				for n_prefix=1 which is stored in n_diff[0]) */
				n_diff[i]++;
			}
		} else {
			/* this is the first non-delete marked record */
			for (i = 0; i < n_uniq; i++) {
				n_diff[i] = 1;
			}
		}

		if (rec_is_last_on_page) {
			/* end of a page has been reached */

			/* we need to copy the record instead of assigning
			like prev_rec = rec; because when we traverse the
			records on this level at some point we will jump from
			one page to the next and then rec and prev_rec will
			be on different pages and
			btr_pcur_move_to_next_user_rec() will release the
			latch on the page that prev_rec is on */
			prev_rec = rec_copy_prefix_to_buf(
				rec, index, n_uniq,
				&prev_rec_buf, &prev_rec_buf_size);
			prev_rec_is_copied = true;

		} else {
			/* still on the same page, the next call to
			btr_pcur_move_to_next_user_rec() will not jump
			on the next page, we can simply assign pointers
			instead of copying the records like above */

			prev_rec = rec;
			prev_rec_is_copied = false;
		}
	}

	/* if *total_pages is left untouched then the above loop was not
	entered at all and there is one page in the whole tree which is
	empty or the loop was entered but this is level 0, contains one page
	and all records are delete-marked */
	if (*total_pages == 0) {

		ut_ad(level == 0);
		ut_ad(*total_recs == 0);

		*total_pages = 1;
	}

	/* if there are records on this level and boundaries
	should be saved */
	if (*total_recs > 0 && n_diff_boundaries != NULL) {

		/* remember the index of the last record on the level as the
		last one from the last group of equal keys; this holds for
		all possible prefixes */
		for (i = 0; i < n_uniq; i++) {
			ib_uint64_t	idx;

			idx = *total_recs - 1;

			n_diff_boundaries[i].push_back(idx);
		}
	}

	/* now in n_diff_boundaries[i] there are exactly n_diff[i] integers,
	for i=0..n_uniq-1 */

#ifdef UNIV_STATS_DEBUG
	for (i = 0; i < n_uniq; i++) {

		DEBUG_PRINTF("    %s(): total recs: " UINT64PF
			     ", total pages: " UINT64PF
			     ", n_diff[" ULINTPF "]: " UINT64PF "\n",
			     __func__, *total_recs,
			     *total_pages,
			     i, n_diff[i]);

#if 0
		if (n_diff_boundaries != NULL) {
			ib_uint64_t	j;

			DEBUG_PRINTF("    %s(): boundaries[%lu]: ",
				     __func__, i);

			for (j = 0; j < n_diff[i]; j++) {
				ib_uint64_t	idx;

				idx = n_diff_boundaries[i][j];

				DEBUG_PRINTF(UINT64PF "=" UINT64PF ", ",
					     j, idx);
			}
			DEBUG_PRINTF("\n");
		}
#endif
	}
#endif /* UNIV_STATS_DEBUG */

	btr_leaf_page_release(btr_pcur_get_block(&pcur), BTR_SEARCH_LEAF, mtr);

	ut_free(prev_rec_buf);
	mem_heap_free(heap);
}

/** Scan a page, reading records from left to right and counting the number
of distinct records (looking only at the first n_prefix
columns) and the number of external pages pointed by records from this page.
If scan_method is QUIT_ON_FIRST_NON_BORING then the function
will return as soon as it finds a record that does not match its neighbor
to the right, which means that in the case of QUIT_ON_FIRST_NON_BORING the
returned n_diff can either be 0 (empty page), 1 (the whole page has all keys
equal) or 2 (the function found a non-boring record and returned).
@param[out]	out_rec			record, or NULL
@param[out]	offsets1		rec_get_offsets() working space (must
be big enough)
@param[out]	offsets2		rec_get_offsets() working space (must
be big enough)
@param[in]	index			index of the page
@param[in]	page			the page to scan
@param[in]	n_prefix		look at the first n_prefix columns
@param[in]	n_core			0, or index->n_core_fields for leaf
@param[out]	n_diff			number of distinct records encountered
@param[out]	n_external_pages	if this is non-NULL then it will be set
to the number of externally stored pages which were encountered
@return offsets1 or offsets2 (the offsets of *out_rec),
or NULL if the page is empty and does not contain user records. */
UNIV_INLINE
rec_offs*
dict_stats_scan_page(
	const rec_t**		out_rec,
	rec_offs*		offsets1,
	rec_offs*		offsets2,
	const dict_index_t*	index,
	const page_t*		page,
	ulint			n_prefix,
	ulint		 	n_core,
	ib_uint64_t*		n_diff,
	ib_uint64_t*		n_external_pages)
{
	rec_offs*	offsets_rec		= offsets1;
	rec_offs*	offsets_next_rec	= offsets2;
	const rec_t*	rec;
	const rec_t*	next_rec;
	/* A dummy heap, to be passed to rec_get_offsets().
	Because offsets1,offsets2 should be big enough,
	this memory heap should never be used. */
	mem_heap_t*	heap			= NULL;
	ut_ad(!!n_core == page_is_leaf(page));
	const rec_t*	(*get_next)(const rec_t*)
		= !n_core || srv_stats_include_delete_marked
		? page_rec_get_next_const
		: page_rec_get_next_non_del_marked;

	const bool	should_count_external_pages = n_external_pages != NULL;

	if (should_count_external_pages) {
		*n_external_pages = 0;
	}

	rec = get_next(page_get_infimum_rec(page));

	if (page_rec_is_supremum(rec)) {
		/* the page is empty or contains only delete-marked records */
		*n_diff = 0;
		*out_rec = NULL;
		return(NULL);
	}

	offsets_rec = rec_get_offsets(rec, index, offsets_rec, n_core,
				      ULINT_UNDEFINED, &heap);

	if (should_count_external_pages) {
		*n_external_pages += btr_rec_get_externally_stored_len(
			rec, offsets_rec);
	}

	next_rec = get_next(rec);

	*n_diff = 1;

	while (!page_rec_is_supremum(next_rec)) {

		ulint	matched_fields;

		offsets_next_rec = rec_get_offsets(next_rec, index,
						   offsets_next_rec, n_core,
						   ULINT_UNDEFINED,
						   &heap);

		/* check whether rec != next_rec when looking at
		the first n_prefix fields */
		cmp_rec_rec(rec, next_rec, offsets_rec, offsets_next_rec,
			    index, false, &matched_fields);

		if (matched_fields < n_prefix) {
			/* rec != next_rec, => rec is non-boring */

			(*n_diff)++;

			if (!n_core) {
				break;
			}
		}

		rec = next_rec;
		/* Assign offsets_rec = offsets_next_rec so that
		offsets_rec matches with rec which was just assigned
		rec = next_rec above.  Also need to point
		offsets_next_rec to the place where offsets_rec was
		pointing before because we have just 2 placeholders
		where data is actually stored: offsets1 and offsets2
		and we are using them in circular fashion
		(offsets[_next]_rec are just pointers to those
		placeholders). */
		std::swap(offsets_rec, offsets_next_rec);

		if (should_count_external_pages) {
			*n_external_pages += btr_rec_get_externally_stored_len(
				rec, offsets_rec);
		}

		next_rec = get_next(next_rec);
	}

	/* offsets1,offsets2 should have been big enough */
	ut_a(heap == NULL);
	*out_rec = rec;
	return(offsets_rec);
}

/** Dive below the current position of a cursor and calculate the number of
distinct records on the leaf page, when looking at the fist n_prefix
columns. Also calculate the number of external pages pointed by records
on the leaf page.
@param[in]	cur			cursor
@param[in]	n_prefix		look at the first n_prefix columns
when comparing records
@param[out]	n_diff			number of distinct records
@param[out]	n_external_pages	number of external pages
@return number of distinct records on the leaf page */
static
void
dict_stats_analyze_index_below_cur(
	const btr_cur_t*	cur,
	ulint			n_prefix,
	ib_uint64_t*		n_diff,
	ib_uint64_t*		n_external_pages)
{
	dict_index_t*	index;
	buf_block_t*	block;
	const page_t*	page;
	mem_heap_t*	heap;
	const rec_t*	rec;
	rec_offs*	offsets1;
	rec_offs*	offsets2;
	rec_offs*	offsets_rec;
	ulint		size;
	mtr_t		mtr;

	index = btr_cur_get_index(cur);

	/* Allocate offsets for the record and the node pointer, for
	node pointer records. In a secondary index, the node pointer
	record will consist of all index fields followed by a child
	page number.
	Allocate space for the offsets header (the allocation size at
	offsets[0] and the REC_OFFS_HEADER_SIZE bytes), and n_fields + 1,
	so that this will never be less than the size calculated in
	rec_get_offsets_func(). */
	size = (1 + REC_OFFS_HEADER_SIZE) + 1 + dict_index_get_n_fields(index);

	heap = mem_heap_create(size * (sizeof *offsets1 + sizeof *offsets2));

	offsets1 = static_cast<rec_offs*>(mem_heap_alloc(
			heap, size * sizeof *offsets1));

	offsets2 = static_cast<rec_offs*>(mem_heap_alloc(
			heap, size * sizeof *offsets2));

	rec_offs_set_n_alloc(offsets1, size);
	rec_offs_set_n_alloc(offsets2, size);

	rec = btr_cur_get_rec(cur);
	page = page_align(rec);
	ut_ad(!page_rec_is_leaf(rec));

	offsets_rec = rec_get_offsets(rec, index, offsets1, 0,
				      ULINT_UNDEFINED, &heap);

	page_id_t		page_id(index->table->space_id,
					btr_node_ptr_get_child_page_no(
						rec, offsets_rec));
	const ulint zip_size = index->table->space->zip_size();

	/* assume no external pages by default - in case we quit from this
	function without analyzing any leaf pages */
	*n_external_pages = 0;

	mtr_start(&mtr);

	/* descend to the leaf level on the B-tree */
	for (;;) {

		dberr_t err = DB_SUCCESS;

		block = buf_page_get_gen(page_id, zip_size,
					 RW_S_LATCH, NULL, BUF_GET,
					 &mtr, &err,
					 !index->is_clust()
					 && 1 == btr_page_get_level(page));

		page = buf_block_get_frame(block);

		if (page_is_leaf(page)) {
			/* leaf level */
			break;
		}
		/* else */

		/* search for the first non-boring record on the page */
		offsets_rec = dict_stats_scan_page(
			&rec, offsets1, offsets2, index, page, n_prefix,
			0, n_diff, NULL);

		/* pages on level > 0 are not allowed to be empty */
		ut_a(offsets_rec != NULL);
		/* if page is not empty (offsets_rec != NULL) then n_diff must
		be > 0, otherwise there is a bug in dict_stats_scan_page() */
		ut_a(*n_diff > 0);

		if (*n_diff == 1) {
			mtr_commit(&mtr);

			/* page has all keys equal and the end of the page
			was reached by dict_stats_scan_page(), no need to
			descend to the leaf level */
			mem_heap_free(heap);
			/* can't get an estimate for n_external_pages here
			because we do not dive to the leaf level, assume no
			external pages (*n_external_pages was assigned to 0
			above). */
			return;
		}
		/* else */

		/* when we instruct dict_stats_scan_page() to quit on the
		first non-boring record it finds, then the returned n_diff
		can either be 0 (empty page), 1 (page has all keys equal) or
		2 (non-boring record was found) */
		ut_a(*n_diff == 2);

		/* we have a non-boring record in rec, descend below it */

		page_id.set_page_no(
			btr_node_ptr_get_child_page_no(rec, offsets_rec));
	}

	/* make sure we got a leaf page as a result from the above loop */
	ut_ad(page_is_leaf(page));

	/* scan the leaf page and find the number of distinct keys,
	when looking only at the first n_prefix columns; also estimate
	the number of externally stored pages pointed by records on this
	page */

	offsets_rec = dict_stats_scan_page(
		&rec, offsets1, offsets2, index, page, n_prefix,
		index->n_core_fields, n_diff,
		n_external_pages);

#if 0
	DEBUG_PRINTF("      %s(): n_diff below page_no=%lu: " UINT64PF "\n",
		     __func__, page_no, n_diff);
#endif

	mtr_commit(&mtr);
	mem_heap_free(heap);
}

/** Input data that is used to calculate dict_index_t::stat_n_diff_key_vals[]
for each n-columns prefix (n from 1 to n_uniq). */
struct n_diff_data_t {
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
	ib_uint64_t	n_recs_on_level;

	/** Number of different key values that were found on the mid level. */
	ib_uint64_t	n_diff_on_level;

	/** Number of leaf pages that are analyzed. This is also the same as
	the number of records that we pick from the mid level and dive below
	them. */
	ib_uint64_t	n_leaf_pages_to_analyze;

	/** Cumulative sum of the number of different key values that were
	found on all analyzed pages. */
	ib_uint64_t	n_diff_all_analyzed_pages;

	/** Cumulative sum of the number of external pages (stored outside of
	the btree but in the same file segment). */
	ib_uint64_t	n_external_pages_sum;
};

/** Estimate the number of different key values in an index when looking at
the first n_prefix columns. For a given level in an index select
n_diff_data->n_leaf_pages_to_analyze records from that level and dive below
them to the corresponding leaf pages, then scan those leaf pages and save the
sampling results in n_diff_data->n_diff_all_analyzed_pages.
@param[in]	index			index
@param[in]	n_prefix		look at first 'n_prefix' columns when
comparing records
@param[in]	boundaries		a vector that contains
n_diff_data->n_diff_on_level integers each of which represents the index (on
level 'level', counting from left/smallest to right/biggest from 0) of the
last record from each group of distinct keys
@param[in,out]	n_diff_data		n_diff_all_analyzed_pages and
n_external_pages_sum in this structure will be set by this function. The
members level, n_diff_on_level and n_leaf_pages_to_analyze must be set by the
caller in advance - they are used by some calculations inside this function
@param[in,out]	mtr			mini-transaction */
static
void
dict_stats_analyze_index_for_n_prefix(
	dict_index_t*		index,
	ulint			n_prefix,
	const boundaries_t*	boundaries,
	n_diff_data_t*		n_diff_data,
	mtr_t*			mtr)
{
	btr_pcur_t	pcur;
	const page_t*	page;
	ib_uint64_t	rec_idx;
	ib_uint64_t	i;

#if 0
	DEBUG_PRINTF("    %s(table=%s, index=%s, level=%lu, n_prefix=%lu,"
		     " n_diff_on_level=" UINT64PF ")\n",
		     __func__, index->table->name, index->name, level,
		     n_prefix, n_diff_data->n_diff_on_level);
#endif

	ut_ad(mtr->memo_contains(index->lock, MTR_MEMO_SX_LOCK));

	/* Position pcur on the leftmost record on the leftmost page
	on the desired level. */

	btr_pcur_open_at_index_side(
		true, index, BTR_SEARCH_TREE_ALREADY_S_LATCHED,
		&pcur, true, n_diff_data->level, mtr);
	btr_pcur_move_to_next_on_page(&pcur);

	page = btr_pcur_get_page(&pcur);

	const rec_t*	first_rec = btr_pcur_get_rec(&pcur);

	/* We shouldn't be scanning the leaf level. The caller of this function
	should have stopped the descend on level 1 or higher. */
	ut_ad(n_diff_data->level > 0);
	ut_ad(!page_is_leaf(page));

	/* The page must not be empty, except when
	it is the root page (and the whole index is empty). */
	ut_ad(btr_pcur_is_on_user_rec(&pcur));
	ut_ad(first_rec == page_rec_get_next_const(page_get_infimum_rec(page)));

	/* check that we are indeed on the desired level */
	ut_a(btr_page_get_level(page) == n_diff_data->level);

	/* there should not be any pages on the left */
	ut_a(!page_has_prev(page));

	/* check whether the first record on the leftmost page is marked
	as such; we are on a non-leaf level */
	ut_a(rec_get_info_bits(first_rec, page_is_comp(page))
	     & REC_INFO_MIN_REC_FLAG);

	const ib_uint64_t	last_idx_on_level = boundaries->at(
		static_cast<unsigned>(n_diff_data->n_diff_on_level - 1));

	rec_idx = 0;

	n_diff_data->n_diff_all_analyzed_pages = 0;
	n_diff_data->n_external_pages_sum = 0;

	for (i = 0; i < n_diff_data->n_leaf_pages_to_analyze; i++) {
		/* there are n_diff_on_level elements
		in 'boundaries' and we divide those elements
		into n_leaf_pages_to_analyze segments, for example:

		let n_diff_on_level=100, n_leaf_pages_to_analyze=4, then:
		segment i=0:  [0, 24]
		segment i=1: [25, 49]
		segment i=2: [50, 74]
		segment i=3: [75, 99] or

		let n_diff_on_level=1, n_leaf_pages_to_analyze=1, then:
		segment i=0: [0, 0] or

		let n_diff_on_level=2, n_leaf_pages_to_analyze=2, then:
		segment i=0: [0, 0]
		segment i=1: [1, 1] or

		let n_diff_on_level=13, n_leaf_pages_to_analyze=7, then:
		segment i=0:  [0,  0]
		segment i=1:  [1,  2]
		segment i=2:  [3,  4]
		segment i=3:  [5,  6]
		segment i=4:  [7,  8]
		segment i=5:  [9, 10]
		segment i=6: [11, 12]

		then we select a random record from each segment and dive
		below it */
		const ib_uint64_t	n_diff = n_diff_data->n_diff_on_level;
		const ib_uint64_t	n_pick
			= n_diff_data->n_leaf_pages_to_analyze;

		const ib_uint64_t	left = n_diff * i / n_pick;
		const ib_uint64_t	right = n_diff * (i + 1) / n_pick - 1;

		ut_a(left <= right);
		ut_a(right <= last_idx_on_level);

		const ulint	rnd = ut_rnd_interval(
			static_cast<ulint>(right - left));

		const ib_uint64_t	dive_below_idx
			= boundaries->at(static_cast<unsigned>(left + rnd));

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

		ib_uint64_t	n_diff_on_leaf_page;
		ib_uint64_t	n_external_pages;

		dict_stats_analyze_index_below_cur(btr_pcur_get_btr_cur(&pcur),
						   n_prefix,
						   &n_diff_on_leaf_page,
						   &n_external_pages);

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
		if (n_diff_on_leaf_page > 0) {
			n_diff_on_leaf_page--;
		}

		n_diff_data->n_diff_all_analyzed_pages += n_diff_on_leaf_page;

		n_diff_data->n_external_pages_sum += n_external_pages;
	}
}

/** statistics for an index */
struct index_stats_t
{
  std::vector<index_field_stats_t> stats;
  ulint index_size;
  ulint n_leaf_pages;

  index_stats_t(ulint n_uniq) : index_size(1), n_leaf_pages(1)
  {
    stats.reserve(n_uniq);
    for (ulint i= 0; i < n_uniq; ++i)
      stats.push_back(index_field_stats_t{0, 1, 0});
  }
};

/** Set dict_index_t::stat_n_diff_key_vals[] and stat_n_sample_sizes[].
@param[in]	n_diff_data	input data to use to derive the results
@param[in,out]	index_stats	index stats to set */
UNIV_INLINE
void
dict_stats_index_set_n_diff(
	const n_diff_data_t*	n_diff_data,
	index_stats_t&		index_stats)
{
	for (ulint n_prefix = index_stats.stats.size();
	     n_prefix >= 1;
	     n_prefix--) {
		/* n_diff_all_analyzed_pages can be 0 here if
		all the leaf pages sampled contained only
		delete-marked records. In this case we should assign
		0 to index->stat_n_diff_key_vals[n_prefix - 1], which
		the formula below does. */

		const n_diff_data_t*	data = &n_diff_data[n_prefix - 1];

		ut_ad(data->n_leaf_pages_to_analyze > 0);
		ut_ad(data->n_recs_on_level > 0);

		ib_uint64_t	n_ordinary_leaf_pages;

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
			is D / (D + E). Knowing that the total number of pages
			is T (including ordinary and external) then we estimate
			that the total number of ordinary leaf pages is
			T * D / (D + E). */
			n_ordinary_leaf_pages
				= index_stats.n_leaf_pages
				* data->n_leaf_pages_to_analyze
				/ (data->n_leaf_pages_to_analyze
				   + data->n_external_pages_sum);
		}

		/* See REF01 for an explanation of the algorithm */
		index_stats.stats[n_prefix - 1].n_diff_key_vals
			= n_ordinary_leaf_pages

			* data->n_diff_on_level
			/ data->n_recs_on_level

			* data->n_diff_all_analyzed_pages
			/ data->n_leaf_pages_to_analyze;

		index_stats.stats[n_prefix - 1].n_sample_sizes
			= data->n_leaf_pages_to_analyze;

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
			     data->n_diff_all_analyzed_pages,
			     data->n_leaf_pages_to_analyze);
	}
}

/** Calculates new statistics for a given index and saves them to the index
members stat_n_diff_key_vals[], stat_n_sample_sizes[], stat_index_size and
stat_n_leaf_pages. This function can be slow.
@param[in]	index	index to analyze
@return index stats */
static index_stats_t dict_stats_analyze_index(dict_index_t* index)
{
	bool		level_is_analyzed;
	ulint		n_uniq;
	ulint		n_prefix;
	ib_uint64_t	total_recs;
	ib_uint64_t	total_pages;
	mtr_t		mtr;
	index_stats_t	result(index->n_uniq);
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
	mtr_s_lock_index(index, &mtr);
	uint16_t root_level;

	{
		buf_block_t* root;
		root = btr_root_block_get(index, RW_SX_LATCH, &mtr);
		if (!root) {
empty_index:
			mtr.commit();
			dict_stats_assert_initialized_index(index);
			DBUG_RETURN(result);
		}

		root_level = btr_page_get_level(root->page.frame);

		mtr.x_lock_space(index->table->space);
		ulint dummy, size;
		result.index_size
			= fseg_n_reserved_pages(*root, PAGE_HEADER
						+ PAGE_BTR_SEG_LEAF
						+ root->page.frame,
						&size, &mtr)
			+ fseg_n_reserved_pages(*root, PAGE_HEADER
						+ PAGE_BTR_SEG_TOP
						+ root->page.frame,
						&dummy, &mtr);
		result.n_leaf_pages = size ? size : 1;
	}

	const auto bulk_trx_id = index->table->bulk_trx_id;
	if (bulk_trx_id && trx_sys.find(nullptr, bulk_trx_id, false)) {
		result.index_size = 1;
		result.n_leaf_pages = 1;
		goto empty_index;
	}

	mtr.commit();

	mtr.start();
	mtr_sx_lock_index(index, &mtr);

	n_uniq = dict_index_get_n_unique(index);

	/* If the tree has just one level (and one page) or if the user
	has requested to sample too many pages then do full scan.

	For each n-column prefix (for n=1..n_uniq) N_SAMPLE_PAGES(index)
	will be sampled, so in total N_SAMPLE_PAGES(index) * n_uniq leaf
	pages will be sampled. If that number is bigger than the total
	number of leaf pages then do full scan of the leaf level instead
	since it will be faster and will give better results. */

	if (root_level == 0
	    || N_SAMPLE_PAGES(index) * n_uniq > result.n_leaf_pages) {

		if (root_level == 0) {
			DEBUG_PRINTF("  %s(): just one page,"
				     " doing full scan\n", __func__);
		} else {
			DEBUG_PRINTF("  %s(): too many pages requested for"
				     " sampling, doing full scan\n", __func__);
		}

		/* do full scan of level 0; save results directly
		into the index */

		dict_stats_analyze_index_level(index,
					       0 /* leaf level */,
					       index->stat_n_diff_key_vals,
					       &total_recs,
					       &total_pages,
					       NULL /* boundaries not needed */,
					       &mtr);

		mtr.commit();

		index->table->stats_mutex_lock();
		for (ulint i = 0; i < n_uniq; i++) {
			result.stats[i].n_diff_key_vals = index->stat_n_diff_key_vals[i];
			result.stats[i].n_sample_sizes = total_pages;
			result.stats[i].n_non_null_key_vals = index->stat_n_non_null_key_vals[i];
		}
		result.n_leaf_pages = index->stat_n_leaf_pages;
		index->table->stats_mutex_unlock();

		DBUG_RETURN(result);
	}

	/* For each level that is being scanned in the btree, this contains the
	number of different key values for all possible n-column prefixes. */
	ib_uint64_t*	n_diff_on_level = UT_NEW_ARRAY(
		ib_uint64_t, n_uniq, mem_key_dict_stats_n_diff_on_level);

	/* For each level that is being scanned in the btree, this contains the
	index of the last record from each group of equal records (when
	comparing only the first n columns, n=1..n_uniq). */
	boundaries_t*	n_diff_boundaries = UT_NEW_ARRAY_NOKEY(boundaries_t,
							       n_uniq);

	/* For each n-column prefix this array contains the input data that is
	used to calculate dict_index_t::stat_n_diff_key_vals[]. */
	n_diff_data_t*	n_diff_data = UT_NEW_ARRAY_NOKEY(n_diff_data_t, n_uniq);

	/* total_recs is also used to estimate the number of pages on one
	level below, so at the start we have 1 page (the root) */
	total_recs = 1;

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
	level_is_analyzed = false;

	for (n_prefix = n_uniq; n_prefix >= 1; n_prefix--) {

		DEBUG_PRINTF("  %s(): searching level with >=%llu "
			     "distinct records, n_prefix=" ULINTPF "\n",
			     __func__, N_DIFF_REQUIRED(index), n_prefix);

		/* Commit the mtr to release the tree S lock to allow
		other threads to do some work too. */
		mtr.commit();
		mtr.start();
		mtr_sx_lock_index(index, &mtr);
		buf_block_t *root = btr_root_block_get(index, RW_S_LATCH,
						       &mtr);
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

		mtr.memo_release(root, MTR_MEMO_PAGE_S_FIX);

		/* check whether we should pick the current level;
		we pick level 1 even if it does not have enough
		distinct records because we do not want to scan the
		leaf level because it may contain too many records */
		if (level_is_analyzed
		    && (n_diff_on_level[n_prefix - 1] >= N_DIFF_REQUIRED(index)
			|| level == 1)) {

			goto found_level;
		}

		/* search for a level that contains enough distinct records */

		if (level_is_analyzed && level > 1) {

			/* if this does not hold we should be on
			"found_level" instead of here */
			ut_ad(n_diff_on_level[n_prefix - 1]
			      < N_DIFF_REQUIRED(index));

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
			following call would scan total_recs pages, because
			total_recs is left from the previous iteration when
			we scanned one level upper or we have not scanned any
			levels yet in which case total_recs is 1. */
			if (total_recs > N_SAMPLE_PAGES(index)) {

				/* if the above cond is true then we are
				not at the root level since on the root
				level total_recs == 1 (set before we
				enter the n-prefix loop) and cannot
				be > N_SAMPLE_PAGES(index) */
				ut_a(level != root_level);

				/* step one level back and be satisfied with
				whatever it contains */
				level++;
				level_is_analyzed = true;

				break;
			}

			dict_stats_analyze_index_level(index,
						       level,
						       n_diff_on_level,
						       &total_recs,
						       &total_pages,
						       n_diff_boundaries,
						       &mtr);

			level_is_analyzed = true;

			if (level == 1
			    || n_diff_on_level[n_prefix - 1]
			    >= N_DIFF_REQUIRED(index)) {
				/* we have reached the last level we could scan
				or we found a good level with many distinct
				records */
				break;
			}

			level--;
			level_is_analyzed = false;
		}
found_level:

		DEBUG_PRINTF("  %s(): found level " ULINTPF
			     " that has " UINT64PF
			     " distinct records for n_prefix=" ULINTPF "\n",
			     __func__, level, n_diff_on_level[n_prefix - 1],
			     n_prefix);
		/* here we are either on level 1 or the level that we are on
		contains >= N_DIFF_REQUIRED distinct keys or we did not scan
		deeper levels because they would contain too many pages */

		ut_ad(level > 0);

		ut_ad(level_is_analyzed);

		/* if any of these is 0 then there is exactly one page in the
		B-tree and it is empty and we should have done full scan and
		should not be here */
		ut_ad(total_recs > 0);
		ut_ad(n_diff_on_level[n_prefix - 1] > 0);

		ut_ad(N_SAMPLE_PAGES(index) > 0);

		n_diff_data_t*	data = &n_diff_data[n_prefix - 1];

		data->level = level;

		data->n_recs_on_level = total_recs;

		data->n_diff_on_level = n_diff_on_level[n_prefix - 1];

		data->n_leaf_pages_to_analyze = std::min(
			N_SAMPLE_PAGES(index),
			n_diff_on_level[n_prefix - 1]);

		/* pick some records from this level and dive below them for
		the given n_prefix */

		dict_stats_analyze_index_for_n_prefix(
			index, n_prefix, &n_diff_boundaries[n_prefix - 1],
			data, &mtr);
	}

	mtr.commit();

	UT_DELETE_ARRAY(n_diff_boundaries);

	UT_DELETE_ARRAY(n_diff_on_level);

	/* n_prefix == 0 means that the above loop did not end up prematurely
	due to tree being changed and so n_diff_data[] is set up. */
	if (n_prefix == 0) {
		dict_stats_index_set_n_diff(n_diff_data, result);
	}

	UT_DELETE_ARRAY(n_diff_data);

	DBUG_RETURN(result);
}

/*********************************************************************//**
Calculates new estimates for table and index statistics. This function
is relatively slow and is used to calculate persistent statistics that
will be saved on disk.
@return DB_SUCCESS or error code */
static
dberr_t
dict_stats_update_persistent(
/*=========================*/
	dict_table_t*	table)		/*!< in/out: table */
{
	dict_index_t*	index;

	DEBUG_PRINTF("%s(table=%s)\n", __func__, table->name);

	DEBUG_SYNC_C("dict_stats_update_persistent");

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

	index_stats_t stats = dict_stats_analyze_index(index);

	table->stats_mutex_lock();
	index->stat_index_size = stats.index_size;
	index->stat_n_leaf_pages = stats.n_leaf_pages;
	for (size_t i = 0; i < stats.stats.size(); ++i) {
		index->stat_n_diff_key_vals[i] = stats.stats[i].n_diff_key_vals;
		index->stat_n_sample_sizes[i] = stats.stats[i].n_sample_sizes;
		index->stat_n_non_null_key_vals[i] = stats.stats[i].n_non_null_key_vals;
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
		stats = dict_stats_analyze_index(index);
		table->stats_mutex_lock();

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

	table->stat_initialized = TRUE;

	dict_stats_assert_initialized(table);

	table->stats_mutex_unlock();

	return(DB_SUCCESS);
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
	ib_uint64_t	stat_value,
	ib_uint64_t*	sample_size,
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
		if (innodb_index_stats_not_found == false &&
		    index->stats_error_printed == false) {
		ib::error() << "Cannot save index statistics for table "
			<< index->table->name
			<< ", index " << index->name
			<< ", stat name \"" << stat_name << "\": "
			<< ret;
			index->stats_error_printed = true;
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

/** Save the table's statistics into the persistent statistics storage.
@param[in]	table_orig	table whose stats to save
@param[in]	only_for_index	if this is non-NULL, then stats for indexes
that are not equal to it will not be saved, if NULL, then all indexes' stats
are saved
@return DB_SUCCESS or error code */
static
dberr_t
dict_stats_save(
	dict_table_t*		table_orig,
	const index_id_t*	only_for_index)
{
	pars_info_t*	pinfo;
	char		db_utf8[MAX_DB_UTF8_LEN];
	char		table_utf8[MAX_TABLE_UTF8_LEN];

	if (high_level_read_only) {
		return DB_READ_ONLY;
	}

	if (!table_orig->is_readable()) {
		return (dict_stats_report_error(table_orig));
	}

	THD* thd = current_thd;
	MDL_ticket *mdl_table = nullptr, *mdl_index = nullptr;
	dict_table_t* table_stats = dict_table_open_on_name(
		TABLE_STATS_NAME, false, DICT_ERR_IGNORE_NONE);
	if (table_stats) {
		dict_sys.freeze(SRW_LOCK_CALL);
		table_stats = dict_acquire_mdl_shared<false>(table_stats, thd,
							     &mdl_table);
		dict_sys.unfreeze();
	}
	if (!table_stats
	    || strcmp(table_stats->name.m_name, TABLE_STATS_NAME)) {
release_and_exit:
		if (table_stats) {
			dict_table_close(table_stats, false, thd, mdl_table);
		}
		return DB_STATS_DO_NOT_EXIST;
	}

	dict_table_t* index_stats = dict_table_open_on_name(
		INDEX_STATS_NAME, false, DICT_ERR_IGNORE_NONE);
	if (index_stats) {
		dict_sys.freeze(SRW_LOCK_CALL);
		index_stats = dict_acquire_mdl_shared<false>(index_stats, thd,
							     &mdl_index);
		dict_sys.unfreeze();
	}
	if (!index_stats) {
		goto release_and_exit;
	}
	if (strcmp(index_stats->name.m_name, INDEX_STATS_NAME)) {
		dict_table_close(index_stats, false, thd, mdl_index);
		goto release_and_exit;
	}

	dict_table_t* table = dict_stats_snapshot_create(table_orig);

	dict_fs2utf8(table->name.m_name, db_utf8, sizeof(db_utf8),
		     table_utf8, sizeof(table_utf8));
	const time_t now = time(NULL);
	trx_t*	trx = trx_create();
	trx->mysql_thd = thd;
	trx_start_internal(trx);
	dberr_t ret = trx->read_only
		? DB_READ_ONLY
		: lock_table_for_trx(table_stats, trx, LOCK_X);
	if (ret == DB_SUCCESS) {
		ret = lock_table_for_trx(index_stats, trx, LOCK_X);
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
		ib::error() << "Cannot save table statistics for table "
			<< table->name << ": " << ret;
rollback_and_exit:
		trx->rollback();
free_and_exit:
		trx->dict_operation_lock_mode = false;
		dict_sys.unlock();
unlocked_free_and_exit:
		trx->free();
		dict_stats_snapshot_free(table);
		dict_table_close(table_stats, false, thd, mdl_table);
		dict_table_close(index_stats, false, thd, mdl_index);
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

		if (only_for_index != NULL && index->id != *only_for_index) {
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
				size_t	len;

				len = strlen(stat_description);

				snprintf(stat_description + len,
					 sizeof(stat_description) - len,
					 ",%s", index->fields[j].name());
			}

			ret = dict_stats_save_index_stat(
				index, now, stat_name,
				index->stat_n_diff_key_vals[i],
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

			ut_a(dtype_get_mtype(type) == DATA_INT);
			ut_a(len == 8);

			table->stat_clustered_index_size
				= (ulint) mach_read_from_8(data);

			break;

		case 2: /* mysql.innodb_table_stats.sum_of_other_index_sizes */

			ut_a(dtype_get_mtype(type) == DATA_INT);
			ut_a(len == 8);

			table->stat_sum_of_other_index_sizes
				= (ulint) mach_read_from_8(data);

			break;

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

/** Aux struct used to pass a table and a boolean to
dict_stats_fetch_index_stats_step(). */
struct index_fetch_t {
	dict_table_t*	table;	/*!< table whose indexes are to be modified */
	bool		stats_were_modified; /*!< will be set to true if at
				least one index stats were modified */
};

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
	index_fetch_t*	arg = (index_fetch_t*) arg_void;
	dict_table_t*	table = arg->table;
	dict_index_t*	index = NULL;
	que_common_t*	cnode;
	const char*	stat_name = NULL;
	ulint		stat_name_len = ULINT_UNDEFINED;
	ib_uint64_t	stat_value = UINT64_UNDEFINED;
	ib_uint64_t	sample_size = UINT64_UNDEFINED;
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
		index->stat_index_size = (ulint) stat_value;
		arg->stats_were_modified = true;
	} else if (stat_name_len == 12 /* strlen("n_leaf_pages") */
		   && strncasecmp("n_leaf_pages", stat_name, stat_name_len)
		   == 0) {
		index->stat_n_leaf_pages = (ulint) stat_value;
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

			char	db_utf8[MAX_DB_UTF8_LEN];
			char	table_utf8[MAX_TABLE_UTF8_LEN];

			dict_fs2utf8(table->name.m_name,
				     db_utf8, sizeof(db_utf8),
				     table_utf8, sizeof(table_utf8));

			ib::info	out;
			out << "Ignoring strange row from "
				<< INDEX_STATS_NAME_PRINT << " WHERE"
				" database_name = '" << db_utf8
				<< "' AND table_name = '" << table_utf8
				<< "' AND index_name = '" << index->name()
				<< "' AND stat_name = '";
			out.write(stat_name, stat_name_len);
			out << "'; because stat_name is malformed";
			return(TRUE);
		}
		/* else */

		/* extract 12 from "n_diff_pfx12..." into n_pfx
		note that stat_name does not have a terminating '\0' */
		n_pfx = ulong(num_ptr[0] - '0') * 10 + ulong(num_ptr[1] - '0');

		ulint	n_uniq = index->n_uniq;

		if (n_pfx == 0 || n_pfx > n_uniq) {

			char	db_utf8[MAX_DB_UTF8_LEN];
			char	table_utf8[MAX_TABLE_UTF8_LEN];

			dict_fs2utf8(table->name.m_name,
				     db_utf8, sizeof(db_utf8),
				     table_utf8, sizeof(table_utf8));

			ib::info	out;
			out << "Ignoring strange row from "
				<< INDEX_STATS_NAME_PRINT << " WHERE"
				" database_name = '" << db_utf8
				<< "' AND table_name = '" << table_utf8
				<< "' AND index_name = '" << index->name()
				<< "' AND stat_name = '";
			out.write(stat_name, stat_name_len);
			out << "'; because stat_name is out of range, the index"
				" has " << n_uniq << " unique columns";

			return(TRUE);
		}
		/* else */

		index->stat_n_diff_key_vals[n_pfx - 1] = stat_value;

		if (sample_size != UINT64_UNDEFINED) {
			index->stat_n_sample_sizes[n_pfx - 1] = sample_size;
		} else {
			/* hmm, strange... the user must have UPDATEd the
			table manually and SET sample_size = NULL */
			index->stat_n_sample_sizes[n_pfx - 1] = 0;
		}

		index->stat_n_non_null_key_vals[n_pfx - 1] = 0;

		arg->stats_were_modified = true;
	} else {
		/* silently ignore rows with unknown stat_name, the
		user may have developed her own stats */
	}

	/* XXX this is not used but returning non-NULL is necessary */
	return(TRUE);
}

/*********************************************************************//**
Read table's statistics from the persistent statistics storage.
@return DB_SUCCESS or error code */
static
dberr_t
dict_stats_fetch_from_ps(
/*=====================*/
	dict_table_t*	table)	/*!< in/out: table */
{
	index_fetch_t	index_fetch_arg;
	trx_t*		trx;
	pars_info_t*	pinfo;
	dberr_t		ret;
	char		db_utf8[MAX_DB_UTF8_LEN];
	char		table_utf8[MAX_TABLE_UTF8_LEN];

	/* Initialize all stats to dummy values before fetching because if
	the persistent storage contains incomplete stats (e.g. missing stats
	for some index) then we would end up with (partially) uninitialized
	stats. */
	dict_stats_empty_table(table, true);

	trx = trx_create();

	trx_start_internal_read_only(trx);

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
	dict_sys.lock(SRW_LOCK_CALL); /* FIXME: remove this */
	ret = que_eval_sql(pinfo,
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

			   "END;", trx);
	/* pinfo is freed by que_eval_sql() */
	dict_sys.unlock();

	trx_commit_for_mysql(trx);

	trx->free();

	if (!index_fetch_arg.stats_were_modified) {
		return(DB_STATS_DO_NOT_EXIST);
	}

	return(ret);
}

/*********************************************************************//**
Clear defragmentation stats modified counter for all indices in table. */
static
void
dict_stats_empty_defrag_modified_counter(
	dict_table_t*	table)	/*!< in: table */
{
	dict_index_t*	index;
	ut_a(table);
	for (index = dict_table_get_first_index(table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {
		index->stat_defrag_modified_counter = 0;
	}
}

/*********************************************************************//**
Fetches or calculates new estimates for index statistics. */
void
dict_stats_update_for_index(
/*========================*/
	dict_index_t*	index)	/*!< in/out: index */
{
	DBUG_ENTER("dict_stats_update_for_index");

	if (dict_stats_is_persistent_enabled(index->table)) {

		if (dict_stats_persistent_storage_check(false)) {
			index_stats_t stats = dict_stats_analyze_index(index);
			index->table->stats_mutex_lock();
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
			index->table->stat_sum_of_other_index_sizes
				+= index->stat_index_size;
			index->table->stats_mutex_unlock();

			dict_stats_save(index->table, &index->id);
			DBUG_VOID_RETURN;
		}
		/* else */

		if (innodb_index_stats_not_found == false &&
		    index->stats_error_printed == false) {
			/* Fall back to transient stats since the persistent
			storage is not present or is corrupted */

		ib::info() << "Recalculation of persistent statistics"
			" requested for table " << index->table->name
			<< " index " << index->name
			<< " but the required"
			" persistent statistics storage is not present or is"
			" corrupted. Using transient stats instead.";
			index->stats_error_printed = false;
		}
	}

	dict_stats_update_transient_for_index(index);

	DBUG_VOID_RETURN;
}

/*********************************************************************//**
Calculates new estimates for table and index statistics. The statistics
are used in query optimization.
@return DB_SUCCESS or error code */
dberr_t
dict_stats_update(
/*==============*/
	dict_table_t*		table,	/*!< in/out: table */
	dict_stats_upd_option_t	stats_upd_option)
					/*!< in: whether to (re) calc
					the stats or to fetch them from
					the persistent statistics
					storage */
{
	ut_ad(!table->stats_mutex_is_owner());

	if (!table->is_readable()) {
		return (dict_stats_report_error(table));
	} else if (srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN) {
		/* If we have set a high innodb_force_recovery level, do
		not calculate statistics, as a badly corrupted index can
		cause a crash in it. */
		dict_stats_empty_table(table, false);
		return(DB_SUCCESS);
	}

	if (trx_id_t bulk_trx_id = table->bulk_trx_id) {
		if (trx_sys.find(nullptr, bulk_trx_id, false)) {
			dict_stats_empty_table(table, false);
			return DB_SUCCESS;
		}
	}

	switch (stats_upd_option) {
	case DICT_STATS_RECALC_PERSISTENT:

		if (srv_read_only_mode) {
			goto transient;
		}

		/* Persistent recalculation requested, called from
		1) ANALYZE TABLE, or
		2) the auto recalculation background thread, or
		3) open table if stats do not exist on disk and auto recalc
		   is enabled */

		/* InnoDB internal tables (e.g. SYS_TABLES) cannot have
		persistent stats enabled */
		ut_a(strchr(table->name.m_name, '/') != NULL);

		/* check if the persistent statistics storage exists
		before calling the potentially slow function
		dict_stats_update_persistent(); that is a
		prerequisite for dict_stats_save() succeeding */
		if (dict_stats_persistent_storage_check(false)) {

			dberr_t	err;

			err = dict_stats_update_persistent(table);

			if (err != DB_SUCCESS) {
				return(err);
			}

			err = dict_stats_save(table, NULL);

			return(err);
		}

		/* Fall back to transient stats since the persistent
		storage is not present or is corrupted */

		if (innodb_table_stats_not_found == false &&
		    table->stats_error_printed == false) {
		ib::warn() << "Recalculation of persistent statistics"
			" requested for table "
			<< table->name
			<< " but the required persistent"
			" statistics storage is not present or is corrupted."
			" Using transient stats instead.";
			table->stats_error_printed = true;
		}

		goto transient;

	case DICT_STATS_RECALC_TRANSIENT:

		goto transient;

	case DICT_STATS_EMPTY_TABLE:

		dict_stats_empty_table(table, true);

		/* If table is using persistent stats,
		then save the stats on disk */

		if (dict_stats_is_persistent_enabled(table)) {

			if (dict_stats_persistent_storage_check(false)) {

				return(dict_stats_save(table, NULL));
			}

			return(DB_STATS_DO_NOT_EXIST);
		}

		return(DB_SUCCESS);

	case DICT_STATS_FETCH_ONLY_IF_NOT_IN_MEMORY:

		/* fetch requested, either fetch from persistent statistics
		storage or use the old method */

		if (table->stat_initialized) {
			return(DB_SUCCESS);
		}

		/* InnoDB internal tables (e.g. SYS_TABLES) cannot have
		persistent stats enabled */
		ut_a(strchr(table->name.m_name, '/') != NULL);

		if (!dict_stats_persistent_storage_check(false)) {
			/* persistent statistics storage does not exist
			or is corrupted, calculate the transient stats */

			if (innodb_table_stats_not_found == false &&
			    table->stats_error_printed == false) {
				ib::error() << "Fetch of persistent statistics"
					" requested for table "
					<< table->name
					<< " but the required system tables "
					<< TABLE_STATS_NAME_PRINT
					<< " and " << INDEX_STATS_NAME_PRINT
					<< " are not present or have unexpected"
					" structure. Using transient stats instead.";
					table->stats_error_printed = true;
			}

			goto transient;
		}

		dict_table_t*	t;

		/* Create a dummy table object with the same name and
		indexes, suitable for fetching the stats into it. */
		t = dict_stats_table_clone_create(table);

		dberr_t	err = dict_stats_fetch_from_ps(t);

		t->stats_last_recalc = table->stats_last_recalc;
		t->stat_modified_counter = 0;
		dict_stats_empty_defrag_modified_counter(t);

		switch (err) {
		case DB_SUCCESS:

			table->stats_mutex_lock();
			/* t is localized to this thread so no need to
			take stats mutex lock (limiting it to debug only) */
			ut_d(t->stats_mutex_lock());

			/* Pass reset_ignored_indexes=true as parameter
			to dict_stats_copy. This will cause statictics
			for corrupted indexes to be set to empty values */
			dict_stats_copy(table, t, true);

			dict_stats_assert_initialized(table);

			ut_d(t->stats_mutex_unlock());
			table->stats_mutex_unlock();

			dict_stats_table_clone_free(t);

			return(DB_SUCCESS);
		case DB_STATS_DO_NOT_EXIST:

			dict_stats_table_clone_free(t);

			if (srv_read_only_mode) {
				goto transient;
			}

			if (dict_stats_auto_recalc_is_enabled(table)) {
				return(dict_stats_update(
						table,
						DICT_STATS_RECALC_PERSISTENT));
			}

			ib::info() << "Trying to use table " << table->name
				<< " which has persistent statistics enabled,"
				" but auto recalculation turned off and the"
				" statistics do not exist in "
				TABLE_STATS_NAME_PRINT
				" and " INDEX_STATS_NAME_PRINT
				". Please either run \"ANALYZE TABLE "
				<< table->name << ";\" manually or enable the"
				" auto recalculation with \"ALTER TABLE "
				<< table->name << " STATS_AUTO_RECALC=1;\"."
				" InnoDB will now use transient statistics for "
				<< table->name << ".";

			goto transient;
		default:

			dict_stats_table_clone_free(t);

			if (innodb_table_stats_not_found == false &&
			    table->stats_error_printed == false) {
				ib::error() << "Error fetching persistent statistics"
					" for table "
					<< table->name
					<< " from " TABLE_STATS_NAME_PRINT " and "
					INDEX_STATS_NAME_PRINT ": " << err
					<< ". Using transient stats method instead.";
			}

			goto transient;
		}
	/* no "default:" in order to produce a compilation warning
	about unhandled enumeration value */
	}

transient:
	dict_stats_update_transient(table);

	return(DB_SUCCESS);
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

  if (dict_table_t::is_temporary_name(old_name) ||
      dict_table_t::is_temporary_name(new_name))
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
  if (!dict_stats_persistent_storage_check(true))
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
	ib_uint64_t	index1_stat_n_diff_key_vals[1];
	ib_uint64_t	index1_stat_n_sample_sizes[1];
	dict_index_t	index2;
	dict_field_t	index2_fields[4];
	ib_uint64_t	index2_stat_n_diff_key_vals[4];
	ib_uint64_t	index2_stat_n_sample_sizes[4];
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

	ret = dict_stats_save(&table, NULL);

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
	ib_uint64_t	index1_stat_n_diff_key_vals[1];
	ib_uint64_t	index1_stat_n_sample_sizes[1];
	dict_index_t	index2;
	ib_uint64_t	index2_stat_n_diff_key_vals[4];
	ib_uint64_t	index2_stat_n_sample_sizes[4];
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
