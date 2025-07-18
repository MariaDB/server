/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2013, 2023, MariaDB Corporation.

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

/******************************************************************//**
@file dict/dict0dict.cc
Data dictionary system

Created 1/8/1996 Heikki Tuuri
***********************************************************************/

#include <my_config.h>
#include <string>

#include "ha_prototypes.h"
#include <mysqld.h>
#include <strfunc.h>

#include "dict0dict.h"
#include "fts0fts.h"
#include "fil0fil.h"
#include <algorithm>
#include "sql_class.h"
#include "sql_table.h"
#include <mysql/service_thd_mdl.h>

#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0sea.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "data0type.h"
#include "dict0boot.h"
#include "dict0load.h"
#include "dict0crea.h"
#include "dict0mem.h"
#include "dict0stats.h"
#include "fts0fts.h"
#include "fts0types.h"
#include "lock0lock.h"
#include "mach0data.h"
#include "mem0mem.h"
#include "page0page.h"
#include "page0zip.h"
#include "pars0pars.h"
#include "pars0sym.h"
#include "que0que.h"
#include "rem0cmp.h"
#include "row0log.h"
#include "row0merge.h"
#include "row0mysql.h"
#include "row0upd.h"
#include "srv0mon.h"
#include "srv0start.h"
#include "trx0undo.h"
#include "trx0purge.h"

#include <vector>
#include <algorithm>

/** the dictionary system */
dict_sys_t	dict_sys;

/** System table names; @see dict_system_id_t */
const span<const char> dict_sys_t::SYS_TABLE[]=
{
  {C_STRING_WITH_LEN("SYS_TABLES")},{C_STRING_WITH_LEN("SYS_INDEXES")},
  {C_STRING_WITH_LEN("SYS_COLUMNS")},{C_STRING_WITH_LEN("SYS_FIELDS")},
  {C_STRING_WITH_LEN("SYS_FOREIGN")},{C_STRING_WITH_LEN("SYS_FOREIGN_COLS")},
  {C_STRING_WITH_LEN("SYS_VIRTUAL")}
};

/** Diagnostic message for exceeding the mutex_lock_wait() timeout */
const char dict_sys_t::fatal_msg[]=
  "innodb_fatal_semaphore_wait_threshold was exceeded for dict_sys.latch. "
  "Please refer to "
  "https://mariadb.com/kb/en/how-to-produce-a-full-stack-trace-for-mysqld/";

/** Percentage of compression failures that are allowed in a single
round */
ulong	zip_failure_threshold_pct = 5;

/** Maximum percentage of a page that can be allowed as a pad to avoid
compression failures */
ulong	zip_pad_max = 50;

#define	DICT_HEAP_SIZE		100	/*!< initial memory heap size when
					creating a table or index object */
#define DICT_POOL_PER_TABLE_HASH 512	/*!< buffer pool max size per table
					hash table fixed size in bytes */
#define DICT_POOL_PER_VARYING	4	/*!< buffer pool max size per data
					dictionary varying size in bytes */

/*******************************************************************//**
Tries to find column names for the index and sets the col field of the
index.
@param[in]	index	index
@param[in]	add_v	new virtual columns added along with an add index call
@return whether the column names were found */
static
bool
dict_index_find_cols(
	dict_index_t*		index,
	const dict_add_v_col_t*	add_v);
/*******************************************************************//**
Builds the internal dictionary cache representation for a clustered
index, containing also system fields not defined by the user.
@return own: the internal representation of the clustered index */
static
dict_index_t*
dict_index_build_internal_clust(
/*============================*/
	dict_index_t*		index);	/*!< in: user representation of
					a clustered index */
/*******************************************************************//**
Builds the internal dictionary cache representation for a non-clustered
index, containing also system fields not defined by the user.
@return own: the internal representation of the non-clustered index */
static
dict_index_t*
dict_index_build_internal_non_clust(
/*================================*/
	dict_index_t*		index);	/*!< in: user representation of
					a non-clustered index */
/**********************************************************************//**
Builds the internal dictionary cache representation for an FTS index.
@return own: the internal representation of the FTS index */
static
dict_index_t*
dict_index_build_internal_fts(
/*==========================*/
	dict_index_t*	index);	/*!< in: user representation of an FTS index */

/**********************************************************************//**
Removes an index from the dictionary cache. */
static
void
dict_index_remove_from_cache_low(
/*=============================*/
	dict_table_t*	table,		/*!< in/out: table */
	dict_index_t*	index,		/*!< in, own: index */
	ibool		lru_evict);	/*!< in: TRUE if page being evicted
					to make room in the table LRU list */
#ifdef UNIV_DEBUG
/**********************************************************************//**
Validate the dictionary table LRU list.
@return TRUE if validate OK */
static
ibool
dict_lru_validate(void);
/*===================*/
#endif /* UNIV_DEBUG */

/* Stream for storing detailed information about the latest foreign key
and unique key errors. Only created if !srv_read_only_mode */
FILE*	dict_foreign_err_file		= NULL;
/* mutex protecting the foreign and unique error buffers */
mysql_mutex_t dict_foreign_err_mutex;

/********************************************************************//**
Checks if the database name in two table names is the same.
@return TRUE if same db name */
ibool
dict_tables_have_same_db(
/*=====================*/
	const char*	name1,	/*!< in: table name in the form
				dbname '/' tablename */
	const char*	name2)	/*!< in: table name in the form
				dbname '/' tablename */
{
	for (; *name1 == *name2; name1++, name2++) {
		if (*name1 == '/') {
			return(TRUE);
		}
		ut_a(*name1); /* the names must contain '/' */
	}
	return(FALSE);
}

/** Check if the table has a given (non_virtual) column.
@param[in]	table		table object
@param[in]	col_name	column name
@param[in]	col_nr		column number guessed, 0 as default
@return column number if the table has the specified column,
otherwise table->n_def */
ulint
dict_table_has_column(
	const dict_table_t*	table,
	const LEX_CSTRING	&col_name,
	ulint			col_nr)
{
	ulint		col_max = table->n_def;

	ut_ad(table);
	ut_ad(col_name.str);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	if (col_nr < col_max
	    && dict_table_get_col_name(table, col_nr).streq(col_name)) {
		return(col_nr);
	}

	/** The order of column may changed, check it with other columns */
	for (ulint i = 0; i < col_max; i++) {
		if (i != col_nr
		    && dict_table_get_col_name(table, i).streq(col_name)) {
			return(i);
		}
	}

	return(col_max);
}


/** Retrieve a column name from a 0-separated list
@param str     the list in the format "name1\0name2\0...nameN\0"
@param col_nr  the position
*/
Lex_ident_column
dict_table_t::get_name_from_z_list(const char *str, size_t col_nr)
{
  if (!str)
    return Lex_ident_column();
  Lex_ident_column res(str, strlen(str));
  for (size_t i= 0; i < col_nr; i++)
  {
    res.str+= res.length + 1;
    res.length= strlen(res.str);
  }
  return res;
}


/** Retrieve the column name.
@param[in]    table    the table of this column */
Lex_ident_column
dict_col_t::name(const dict_table_t& table) const
{
  ut_ad(table.magic_n == DICT_TABLE_MAGIC_N);

  size_t col_nr;

  if (is_virtual())
  {
    col_nr= size_t(reinterpret_cast<const dict_v_col_t*>(this) - table.v_cols);
    ut_ad(col_nr < table.n_v_def);
    return dict_table_t::get_name_from_z_list(table.v_col_names, col_nr);
  }
  col_nr= size_t(this - table.cols);
  ut_ad(col_nr < table.n_def);
  return dict_table_t::get_name_from_z_list(table.col_names, col_nr);
}

/** Returns a virtual column's name.
@param[in]    table     target table
@param[in]    col_nr    virtual column number (nth virtual column)
@return column name or NULL if column number out of range. */
Lex_ident_column
dict_table_get_v_col_name(const dict_table_t*  table, ulint col_nr)
{
  ut_ad(table);
  ut_ad(col_nr < table->n_v_def);
  ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

  return col_nr >= table->n_v_def ?
         Lex_ident_column() :
         dict_table_t::get_name_from_z_list(table->v_col_names, col_nr);
}

/** Search virtual column's position in InnoDB according to its position
in original table's position
@param[in]	table	target table
@param[in]	col_nr	column number (nth column in the MySQL table)
@return virtual column's position in InnoDB, ULINT_UNDEFINED if not find */
static
ulint
dict_table_get_v_col_pos_for_mysql(
	const dict_table_t*	table,
	ulint			col_nr)
{
	ulint	i;

	ut_ad(table);
	ut_ad(col_nr < static_cast<ulint>(table->n_t_def));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	for (i = 0; i < table->n_v_def; i++) {
		if (col_nr == dict_get_v_col_mysql_pos(
				table->v_cols[i].m_col.ind)) {
			break;
		}
	}

	if (i == table->n_v_def) {
		return(ULINT_UNDEFINED);
	}

	return(i);
}

/** Returns a virtual column's name according to its original
MySQL table position.
@param[in]	table	target table
@param[in]	col_nr	column number (nth column in the table)
@return column name. */
static
Lex_ident_column
dict_table_get_v_col_name_mysql(
	const dict_table_t*	table,
	ulint			col_nr)
{
	ulint	i = dict_table_get_v_col_pos_for_mysql(table, col_nr);

	if (i == ULINT_UNDEFINED) {
		return Lex_ident_column();
	}

	return(dict_table_get_v_col_name(table, i));
}

/** Get nth virtual column according to its original MySQL table position
@param[in]	table	target table
@param[in]	col_nr	column number in MySQL Table definition
@return dict_v_col_t ptr */
dict_v_col_t*
dict_table_get_nth_v_col_mysql(
	const dict_table_t*	table,
	ulint			col_nr)
{
	ulint	i = dict_table_get_v_col_pos_for_mysql(table, col_nr);

	if (i == ULINT_UNDEFINED) {
		return(NULL);
	}

	return(dict_table_get_nth_v_col(table, i));
}


/** Get all the FTS indexes on a table.
@param[in]	table	table
@param[out]	indexes	all FTS indexes on this table
@return number of FTS indexes */
ulint
dict_table_get_all_fts_indexes(
	const dict_table_t*	table,
	ib_vector_t*		indexes)
{
	dict_index_t* index;

	ut_a(ib_vector_size(indexes) == 0);

	for (index = dict_table_get_first_index(table);
	     index;
	     index = dict_table_get_next_index(index)) {

		if (index->type == DICT_FTS) {
			ib_vector_push(indexes, &index);
		}
	}

	return(ib_vector_size(indexes));
}

/** Looks for column n in an index.
@param[in]	index		index
@param[in]	n		column number
@param[in]	inc_prefix	true=consider column prefixes too
@param[in]	is_virtual	true==virtual column
@param[out]	prefix_col_pos	col num if prefix
@return position in internal representation of the index;
ULINT_UNDEFINED if not contained */
ulint
dict_index_get_nth_col_or_prefix_pos(
	const dict_index_t*	index,
	ulint			n,
	bool			inc_prefix,
	bool			is_virtual,
	ulint*			prefix_col_pos)
{
	const dict_field_t*	field;
	const dict_col_t*	col;
	ulint			pos;
	ulint			n_fields;

	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	if (prefix_col_pos) {
		*prefix_col_pos = ULINT_UNDEFINED;
	}

	if (is_virtual) {
		col = &(dict_table_get_nth_v_col(index->table, n)->m_col);
	} else {
		col = dict_table_get_nth_col(index->table, n);
	}

	if (dict_index_is_clust(index)) {

		return(dict_col_get_clust_pos(col, index));
	}

	n_fields = dict_index_get_n_fields(index);

	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		if (col == field->col) {
			if (prefix_col_pos) {
				*prefix_col_pos = pos;
			}
			if (inc_prefix || field->prefix_len == 0) {
				return(pos);
			}
		}
	}

	return(ULINT_UNDEFINED);
}

/** Check if the index contains a column or a prefix of that column.
@param[in]	n		column number
@param[in]	is_virtual	whether it is a virtual col
@return whether the index contains the column or its prefix */
bool dict_index_t::contains_col_or_prefix(ulint n, bool is_virtual) const
{
	ut_ad(magic_n == DICT_INDEX_MAGIC_N);

	if (is_primary()) {
		return(!is_virtual);
	}

	const dict_col_t* col = is_virtual
		? &dict_table_get_nth_v_col(table, n)->m_col
		: dict_table_get_nth_col(table, n);

	for (ulint pos = 0; pos < n_fields; pos++) {
		if (col == fields[pos].col) {
			return true;
		}
	}

	return false;
}

/********************************************************************//**
Looks for a matching field in an index. The column has to be the same. The
column in index must be complete, or must contain a prefix longer than the
column in index2. That is, we must be able to construct the prefix in index2
from the prefix in index.
@return position in internal representation of the index;
ULINT_UNDEFINED if not contained */
ulint
dict_index_get_nth_field_pos(
/*=========================*/
	const dict_index_t*	index,	/*!< in: index from which to search */
	const dict_index_t*	index2,	/*!< in: index */
	ulint			n)	/*!< in: field number in index2 */
{
	const dict_field_t*	field;
	const dict_field_t*	field2;
	ulint			n_fields;
	ulint			pos;

	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	field2 = dict_index_get_nth_field(index2, n);

	n_fields = dict_index_get_n_fields(index);

	/* Are we looking for a MBR (Minimum Bound Box) field of
	a spatial index */
	bool	is_mbr_fld = (n == 0 && dict_index_is_spatial(index2));

	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		/* The first field of a spatial index is a transformed
		MBR (Minimum Bound Box) field made out of original column,
		so its field->col still points to original cluster index
		col, but the actual content is different. So we cannot
		consider them equal if neither of them is MBR field */
		if (pos == 0 && dict_index_is_spatial(index) && !is_mbr_fld) {
			continue;
		}

		if (field->col == field2->col
		    && (field->prefix_len == 0
			|| (field->prefix_len >= field2->prefix_len
			    && field2->prefix_len != 0))) {

			return(pos);
		}
	}

	return(ULINT_UNDEFINED);
}

void mdl_release(THD *thd, MDL_ticket *mdl) noexcept
{
  if (!thd || !mdl);
  else if (MDL_context *mdl_context= static_cast<MDL_context*>
           (thd_mdl_context(thd)))
    mdl_context->release_lock(mdl);
}

/** Parse the table file name into table name and database name.
@tparam        dict_frozen  whether the caller holds dict_sys.latch
@param[in,out] db_name      database name buffer
@param[in,out] tbl_name     table name buffer
@param[out] db_name_len     database name length
@param[out] tbl_name_len    table name length
@return whether the table name is visible to SQL */
template<bool dict_frozen>
bool dict_table_t::parse_name(char (&db_name)[NAME_LEN + 1],
                              char (&tbl_name)[NAME_LEN + 1],
                              size_t *db_name_len, size_t *tbl_name_len) const
{
  char db_buf[MAX_DATABASE_NAME_LEN + 1];
  char tbl_buf[MAX_TABLE_NAME_LEN + 1];

  if (!dict_frozen)
    dict_sys.freeze(SRW_LOCK_CALL); /* protect against renaming */
  ut_ad(dict_sys.frozen());
  const size_t db_len= name.dblen();
  ut_ad(db_len <= MAX_DATABASE_NAME_LEN);

  memcpy(db_buf, mdl_name.m_name, db_len);
  db_buf[db_len]= 0;

  size_t tbl_len= strlen(mdl_name.m_name + db_len + 1);
  const bool is_temp= mdl_name.is_temporary();

  if (is_temp);
  else if (const char *is_part= static_cast<const char*>
           (memchr(mdl_name.m_name + db_len + 1, '#', tbl_len)))
    tbl_len= static_cast<size_t>(is_part - &mdl_name.m_name[db_len + 1]);

  memcpy(tbl_buf, mdl_name.m_name + db_len + 1, tbl_len);
  tbl_buf[tbl_len]= 0;

  if (!dict_frozen)
    dict_sys.unfreeze();

  *db_name_len= filename_to_tablename(db_buf, db_name,
                                      MAX_DATABASE_NAME_LEN + 1, true);

  if (is_temp)
    return false;

  *tbl_name_len= filename_to_tablename(tbl_buf, tbl_name,
                                       MAX_TABLE_NAME_LEN + 1, true);
  return true;
}

template bool
dict_table_t::parse_name<>(char(&)[NAME_LEN + 1], char(&)[NAME_LEN + 1],
                           size_t*, size_t*) const;

dict_table_t *dict_sys_t::acquire_temporary_table(table_id_t id) const noexcept
{
  ut_ad(frozen());
  ut_ad(id >= DICT_HDR_FIRST_ID);
  return temp_id_hash.cell_get(ut_fold_ull(id))->
    find(&dict_table_t::id_hash, [id](dict_table_t *t)
    {
      ut_ad(t->is_temporary());
      ut_ad(t->cached);
      if (t->id != id)
        return false;
      t->acquire();
      return true;
    });
}

dict_table_t *dict_sys_t::find_table(table_id_t id) const noexcept
{
  ut_ad(frozen());
  return table_id_hash.cell_get(ut_fold_ull(id))->
    find(&dict_table_t::id_hash, [id](const dict_table_t *t)
    {
      ut_ad(!t->is_temporary());
      ut_ad(t->cached);
      return t->id == id;
    });
}

dict_table_t *dict_sys_t::find_table(const span<const char> &name)
  const noexcept
{
  ut_ad(frozen());
  return table_hash.cell_get(my_crc32c(0, name.data(), name.size()))->
    find(&dict_table_t::name_hash, [name](const dict_table_t *t)
    {
      return strlen(t->name.m_name) == name.size() &&
        !memcmp(t->name.m_name, name.data(), name.size());
    });
}

/** Acquire MDL shared for the table name.
@tparam trylock whether to use non-blocking operation
@param[in,out]  table           table object
@param[in,out]  mdl_context     MDL context
@param[out]     mdl             MDL ticket
@param[in]      table_op        operation to perform when opening
@return table object after locking MDL shared
@retval nullptr if the table is not readable, or if trylock && MDL blocked */
template<bool trylock>
__attribute__((nonnull, warn_unused_result))
dict_table_t*
dict_acquire_mdl_shared(dict_table_t *table,
                        MDL_context *mdl_context, MDL_ticket **mdl,
                        dict_table_op_t table_op)
{
  char db_buf[NAME_LEN + 1], db_buf1[NAME_LEN + 1];
  char tbl_buf[NAME_LEN + 1], tbl_buf1[NAME_LEN + 1];
  size_t db_len, tbl_len;

  if (!table->parse_name<!trylock>(db_buf, tbl_buf, &db_len, &tbl_len))
    /* The name of an intermediate table starts with #sql */
    return table;

retry:
  ut_ad(!trylock == dict_sys.frozen());

  if (!table->is_readable() || table->corrupted)
  {
    if (*mdl)
    {
      mdl_context->release_lock(*mdl);
      *mdl= nullptr;
    }
    return nullptr;
  }

  const table_id_t table_id{table->id};

  if (!trylock)
    dict_sys.unfreeze();

  {
    MDL_request request;
    MDL_REQUEST_INIT(&request,MDL_key::TABLE, db_buf, tbl_buf, MDL_SHARED,
                     MDL_EXPLICIT);
    if (trylock
        ? mdl_context->try_acquire_lock(&request)
        : mdl_context->acquire_lock(&request,
                                    /* FIXME: use compatible type, and maybe
                                    remove this parameter altogether! */
                                    static_cast<double>(global_system_variables
                                                        .lock_wait_timeout)))
    {
      *mdl= nullptr;
      if (trylock)
        return nullptr;
    }
    else
    {
      *mdl= request.ticket;
      if (trylock && !*mdl)
        return nullptr;
    }
  }

  size_t db1_len, tbl1_len;
lookup:
  dict_sys.freeze(SRW_LOCK_CALL);
  table= dict_sys.find_table(table_id);
  if (table)
  {
    if (!table->is_accessible())
    {
      table= nullptr;
    unlock_and_return_without_mdl:
      if (trylock)
        dict_sys.unfreeze();
    return_without_mdl:
      if (*mdl)
      {
        mdl_context->release_lock(*mdl);
        *mdl= nullptr;
      }
      return table;
    }

    if (trylock)
      table->acquire();

    if (!table->parse_name<true>(db_buf1, tbl_buf1, &db1_len, &tbl1_len))
    {
      /* The table was renamed to #sql prefix.
      Release MDL (if any) for the old name and return. */
      goto unlock_and_return_without_mdl;
    }
  }
  else if (table_op != DICT_TABLE_OP_OPEN_ONLY_IF_CACHED)
  {
    dict_sys.unfreeze();
    dict_sys.lock(SRW_LOCK_CALL);
    table= dict_load_table_on_id(table_id,
                                 table_op == DICT_TABLE_OP_LOAD_TABLESPACE
                                 ? DICT_ERR_IGNORE_RECOVER_LOCK
                                 : DICT_ERR_IGNORE_FK_NOKEY);
    dict_sys.unlock();
    /* At this point, the freshly loaded table may already have been evicted.
    We must look it up again while holding a shared dict_sys.latch.  We keep
    trying this until the table is found in the cache or it cannot be found
    in the dictionary (because the table has been dropped or rebuilt). */
    if (table)
      goto lookup;
    if (!trylock)
      dict_sys.freeze(SRW_LOCK_CALL);
    goto return_without_mdl;
  }
  else
    goto return_without_mdl;

  if (*mdl)
  {
    if (db_len == db1_len && tbl_len == tbl1_len &&
        !memcmp(db_buf, db_buf1, db_len) &&
        !memcmp(tbl_buf, tbl_buf1, tbl_len))
    {
      if (trylock)
        dict_sys.unfreeze();
      return table;
    }

    /* The table was renamed. Release MDL for the old name and
    try to acquire MDL for the new name. */
    mdl_context->release_lock(*mdl);
    *mdl= nullptr;
  }

  db_len= db1_len;
  tbl_len= tbl1_len;

  memcpy(tbl_buf, tbl_buf1, tbl_len + 1);
  memcpy(db_buf, db_buf1, db_len + 1);
  goto retry;
}

template dict_table_t* dict_acquire_mdl_shared<false>
(dict_table_t*,MDL_context*,MDL_ticket**,dict_table_op_t);

/** Acquire MDL shared for the table name.
@tparam trylock whether to use non-blocking operation
@param[in,out]  table           table object
@param[in,out]  thd             background thread
@param[out]     mdl             mdl ticket
@param[in]      table_op        operation to perform when opening
@return table object after locking MDL shared
@retval nullptr if the table is not readable, or if trylock && MDL blocked */
template<bool trylock>
dict_table_t*
dict_acquire_mdl_shared(dict_table_t *table,
                        THD *thd,
                        MDL_ticket **mdl,
                        dict_table_op_t table_op)
{
  if (!table || !mdl)
    return table;

  MDL_context *mdl_context= static_cast<MDL_context*>(thd_mdl_context(thd));
  size_t db_len;

  if (trylock)
  {
    dict_sys.freeze(SRW_LOCK_CALL);
    db_len= dict_get_db_name_len(table->name.m_name);
    dict_sys.unfreeze();
  }
  else
  {
    ut_ad(dict_sys.frozen_not_locked());
    db_len= dict_get_db_name_len(table->name.m_name);
  }

  if (db_len == 0)
    return table; /* InnoDB system tables are not covered by MDL */

  return mdl_context
    ? dict_acquire_mdl_shared<trylock>(table, mdl_context, mdl, table_op)
    : nullptr;
}

template dict_table_t* dict_acquire_mdl_shared<false>
(dict_table_t*,THD*,MDL_ticket**,dict_table_op_t);
template dict_table_t* dict_acquire_mdl_shared<true>
(dict_table_t*,THD*,MDL_ticket**,dict_table_op_t);

/** Look up a table by numeric identifier.
@param[in]      table_id        table identifier
@param[in]      dict_locked     data dictionary locked
@param[in]      table_op        operation to perform when opening
@param[in,out]  thd             background thread, or NULL to not acquire MDL
@param[out]     mdl             mdl ticket, or NULL
@return table, NULL if does not exist */
dict_table_t *dict_table_open_on_id(table_id_t table_id, bool dict_locked,
                                    dict_table_op_t table_op, THD *thd,
                                    MDL_ticket **mdl)
{
retry:
  if (!dict_locked)
    dict_sys.freeze(SRW_LOCK_CALL);

  dict_table_t *table= dict_sys.find_table(table_id);

  if (table)
  {
    if (!dict_locked)
    {
      if (thd)
      {
        table= dict_acquire_mdl_shared<false>(table, thd, mdl, table_op);
        if (table)
          goto acquire;
      }
      else
      acquire:
        table->acquire();
      dict_sys.unfreeze();
    }
    else
      table->acquire();
  }
  else if (table_op != DICT_TABLE_OP_OPEN_ONLY_IF_CACHED)
  {
    if (!dict_locked)
    {
      dict_sys.unfreeze();
      dict_sys.lock(SRW_LOCK_CALL);
    }
    table= dict_load_table_on_id(table_id,
                                 table_op == DICT_TABLE_OP_LOAD_TABLESPACE
                                 ? DICT_ERR_IGNORE_RECOVER_LOCK
                                 : DICT_ERR_IGNORE_FK_NOKEY);
    if (!dict_locked)
    {
      dict_sys.unlock();
      if (table)
        goto retry;
    }
    else if (table)
      table->acquire();
  }

  return table;
}

/********************************************************************//**
Looks for column n position in the clustered index.
@return position in internal representation of the clustered index */
unsigned
dict_table_get_nth_col_pos(
/*=======================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			n,	/*!< in: column number */
	ulint*			prefix_col_pos)
{
  ulint pos= dict_index_get_nth_col_pos(dict_table_get_first_index(table),
					n, prefix_col_pos);
  DBUG_ASSERT(pos <= dict_index_t::MAX_N_FIELDS);
  return static_cast<unsigned>(pos);
}

/********************************************************************//**
Checks if a column is in the ordering columns of the clustered index of a
table. Column prefixes are treated like whole columns.
@return TRUE if the column, or its prefix, is in the clustered key */
ibool
dict_table_col_in_clustered_key(
/*============================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			n)	/*!< in: column number */
{
	const dict_index_t*	index;
	const dict_field_t*	field;
	const dict_col_t*	col;
	ulint			pos;
	ulint			n_fields;

	col = dict_table_get_nth_col(table, n);

	index = dict_table_get_first_index(table);

	n_fields = dict_index_get_n_unique(index);

	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		if (col == field->col) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/** Initialise the data dictionary cache. */
void dict_sys_t::create() noexcept
{
  ut_ad(this == &dict_sys);
  ut_ad(!is_initialised());
  m_initialised= true;
  UT_LIST_INIT(table_LRU, &dict_table_t::table_LRU);
  UT_LIST_INIT(table_non_LRU, &dict_table_t::table_LRU);

  const ulint hash_size = buf_pool.curr_pool_size()
    / (DICT_POOL_PER_TABLE_HASH * UNIV_WORD_SIZE);

  table_hash.create(hash_size);
  table_id_hash.create(hash_size);
  temp_id_hash.create(hash_size);

  latch.SRW_LOCK_INIT(dict_operation_lock_key);

  if (!srv_read_only_mode)
  {
    dict_foreign_err_file= os_file_create_tmpfile();
    ut_a(dict_foreign_err_file);
  }

  mysql_mutex_init(dict_foreign_err_mutex_key, &dict_foreign_err_mutex,
                   nullptr);
}


void dict_sys_t::lock_wait(SRW_LOCK_ARGS(const char *file, unsigned line)) noexcept
{
  ulonglong now= my_hrtime_coarse().val, old= 0;
  if (latch_ex_wait_start.compare_exchange_strong
      (old, now, std::memory_order_relaxed, std::memory_order_relaxed))
  {
    latch.wr_lock(SRW_LOCK_ARGS(file, line));
    latch_ex_wait_start.store(0, std::memory_order_relaxed);
    return;
  }

  ut_ad(old);
  /* We could have old > now due to our use of my_hrtime_coarse(). */
  ulong waited= old <= now ? static_cast<ulong>((now - old) / 1000000) : 0;
  const ulong threshold= srv_fatal_semaphore_wait_threshold;

  if (waited >= threshold)
  {
    buf_pool.print_flush_info();
    ib::fatal() << fatal_msg;
  }

  if (waited > threshold / 4)
    ib::warn() << "A long wait (" << waited
               << " seconds) was observed for dict_sys.latch";
  latch.wr_lock(SRW_LOCK_ARGS(file, line));
}

#ifdef UNIV_PFS_RWLOCK
ATTRIBUTE_NOINLINE void dict_sys_t::unlock() noexcept
{
  latch.wr_unlock();
}

ATTRIBUTE_NOINLINE void dict_sys_t::freeze(const char *file, unsigned line) noexcept
{
  latch.rd_lock(file, line);
}

ATTRIBUTE_NOINLINE void dict_sys_t::unfreeze() noexcept
{
  latch.rd_unlock();
}
#endif /* UNIV_PFS_RWLOCK */

/** Report an error about failing to open a table.
@param name   table name */
static void dict_table_open_failed(const table_name_t &name)
{
  my_printf_error(ER_TABLE_CORRUPT,
                  "Table %.*sQ.%sQ is corrupted."
                  " Please drop the table and recreate.",
                  MYF(ME_ERROR_LOG),
                  int(name.dblen()), name.m_name, name.basename());
}

/**********************************************************************//**
Returns a table object and increments its open handle count.
NOTE! This is a high-level function to be used mainly from outside the
'dict' directory. Inside this directory dict_table_get_low
is usually the appropriate function.
@param[in] table_name Table name
@param[in] dict_locked whether dict_sys.latch is being held exclusively
@param[in] ignore_err error to be ignored when loading the table
@return table
@retval nullptr if does not exist */
dict_table_t*
dict_table_open_on_name(
	const char*		table_name,
	bool			dict_locked,
	dict_err_ignore_t	ignore_err)
{
  dict_table_t *table;
  DBUG_ENTER("dict_table_open_on_name");
  DBUG_PRINT("dict_table_open_on_name", ("table: '%s'", table_name));

  const span<const char> name{table_name, strlen(table_name)};

  if (!dict_locked)
  {
    dict_sys.freeze(SRW_LOCK_CALL);
    table= dict_sys.find_table(name);
    if (table)
    {
      ut_ad(table->cached);
      if (!(ignore_err & ~DICT_ERR_IGNORE_FK_NOKEY) &&
          !table->is_readable() && table->corrupted)
      {
        ulint algo= table->space->get_compression_algo();
        if (algo <= PAGE_ALGORITHM_LAST && !fil_comp_algo_loaded(algo))
          my_printf_error(ER_PROVIDER_NOT_LOADED,
                          "Table %.*sQ.%sQ is compressed with %s,"
                          " which is not currently loaded. "
                          "Please load the %s provider plugin"
                          " to open the table",
                          MYF(ME_ERROR_LOG),
                          int(table->name.dblen()), table->name.m_name,
                          table->name.basename(),
                          page_compression_algorithms[algo],
                          page_compression_algorithms[algo]);
        else
          dict_table_open_failed(table->name);
        dict_sys.unfreeze();
        DBUG_RETURN(nullptr);
      }
      table->acquire();
      dict_sys.unfreeze();
      DBUG_RETURN(table);
    }
    dict_sys.unfreeze();
    dict_sys.lock(SRW_LOCK_CALL);
  }

  table= dict_sys.load_table(name, ignore_err);

  if (table)
  {
    ut_ad(table->cached);
    if (!(ignore_err & ~DICT_ERR_IGNORE_FK_NOKEY) &&
        !table->is_readable() && table->corrupted)
    {
      dict_table_open_failed(table->name);
      if (!dict_locked)
        dict_sys.unlock();
      DBUG_RETURN(nullptr);
    }

    table->acquire();
  }

  ut_ad(dict_lru_validate());
  if (!dict_locked)
    dict_sys.unlock();

  DBUG_RETURN(table);
}

bool dict_stats::open(THD *thd) noexcept
{
  ut_ad(!mdl_table);
  ut_ad(!mdl_index);
  ut_ad(!table_stats);
  ut_ad(!index_stats);
  ut_ad(!mdl_context);

  mdl_context= static_cast<MDL_context*>(thd_mdl_context(thd));
  if (!mdl_context)
    return true;
  /* FIXME: use compatible type, and maybe remove this parameter altogether! */
  const double timeout= double(global_system_variables.lock_wait_timeout);
  MDL_request request;
  MDL_REQUEST_INIT(&request, MDL_key::TABLE, "mysql", "innodb_table_stats",
                   MDL_SHARED, MDL_EXPLICIT);
  if (UNIV_UNLIKELY(mdl_context->acquire_lock(&request, timeout)))
    return true;
  mdl_table= request.ticket;
  MDL_REQUEST_INIT(&request, MDL_key::TABLE, "mysql", "innodb_index_stats",
                   MDL_SHARED, MDL_EXPLICIT);
  if (UNIV_UNLIKELY(mdl_context->acquire_lock(&request, timeout)))
    goto release_mdl;
  mdl_index= request.ticket;
  table_stats= dict_table_open_on_name("mysql/innodb_table_stats", false,
                                       DICT_ERR_IGNORE_NONE);
  if (!table_stats)
    goto release_mdl;
  index_stats= dict_table_open_on_name("mysql/innodb_index_stats", false,
                                       DICT_ERR_IGNORE_NONE);
  if (index_stats)
    return false;

  table_stats->release();
release_mdl:
  if (mdl_index)
    mdl_context->release_lock(mdl_index);
  mdl_context->release_lock(mdl_table);
  return true;
}

void dict_stats::close() noexcept
{
  table_stats->release();
  index_stats->release();
  mdl_context->release_lock(mdl_table);
  mdl_context->release_lock(mdl_index);
}

/**********************************************************************//**
Adds system columns to a table object. */
void
dict_table_add_system_columns(
/*==========================*/
	dict_table_t*	table,	/*!< in/out: table */
	mem_heap_t*	heap)	/*!< in: temporary heap */
{
	ut_ad(table->n_def == table->n_cols - DATA_N_SYS_COLS);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(!table->cached);

	/* NOTE: the system columns MUST be added in the following order
	(so that they can be indexed by the numerical value of DATA_ROW_ID,
	etc.) and as the last columns of the table memory object.
	The clustered index will not always physically contain all system
	columns. */

	dict_mem_table_add_col(table, heap, "DB_ROW_ID", DATA_SYS,
			       DATA_ROW_ID | DATA_NOT_NULL,
			       DATA_ROW_ID_LEN);

	compile_time_assert(DATA_ROW_ID == 0);
	dict_mem_table_add_col(table, heap, "DB_TRX_ID", DATA_SYS,
			       DATA_TRX_ID | DATA_NOT_NULL,
			       DATA_TRX_ID_LEN);
	compile_time_assert(DATA_TRX_ID == 1);
	dict_mem_table_add_col(table, heap, "DB_ROLL_PTR", DATA_SYS,
			       DATA_ROLL_PTR | DATA_NOT_NULL,
			       DATA_ROLL_PTR_LEN);
	compile_time_assert(DATA_ROLL_PTR == 2);

	/* This check reminds that if a new system column is added to
	the program, it should be dealt with here */
	compile_time_assert(DATA_N_SYS_COLS == 3);
}

/** Add the table definition to the data dictionary cache */
void dict_table_t::add_to_cache()
{
	cached = TRUE;

	dict_sys.add(this);
}

/** Add a table definition to the data dictionary cache */
inline void dict_sys_t::add(dict_table_t *table) noexcept
{
  ut_ad(!table->name_hash);
  ut_ad(!table->id_hash);
  table->row_id= 0;
  table->autoinc_mutex.init();
  table->lock_mutex_init();
  const char *name= table->name.m_name;
  dict_table_t **prev= table_hash.cell_get(my_crc32c(0, name, strlen(name)))->
    search(&dict_table_t::name_hash, [name](const dict_table_t *t)
    {
      if (!t) return true;
      ut_ad(t->cached);
      ut_a(strcmp(t->name.m_name, name));
      return false;
    });
  *prev= table;
  prev= (table->is_temporary() ? temp_id_hash : table_id_hash).
    cell_get(ut_fold_ull(table->id))->
    search(&dict_table_t::id_hash, [table](const dict_table_t *t)
    {
      if (!t) return true;
      ut_ad(t->cached);
      ut_a(t->id != table->id);
      return false;
    });
  *prev= table;
  UT_LIST_ADD_FIRST(table->can_be_evicted ? table_LRU : table_non_LRU, table);
  ut_ad(dict_lru_validate());
}

/** Test whether a table can be evicted from dict_sys.table_LRU.
@param table   table to be considered for eviction
@return whether the table can be evicted */
TRANSACTIONAL_TARGET
static bool dict_table_can_be_evicted(dict_table_t *table)
{
	ut_ad(dict_sys.locked());
	ut_a(table->can_be_evicted);
	ut_a(table->foreign_set.empty());
	ut_a(table->referenced_set.empty());

	if (table->get_ref_count() == 0) {
		/* The transaction commit and rollback are called from
		outside the handler interface. This means that there is
		a window where the table->n_ref_count can be zero but
		the table instance is in "use". */

		if (lock_table_has_locks(table)) {
			return false;
		}

#ifdef BTR_CUR_HASH_ADAPT
		/* We cannot really evict the table if adaptive hash
		index entries are pointing to any of its indexes. */
		for (const dict_index_t* index
			     = dict_table_get_first_index(table);
		     index; index = dict_table_get_next_index(index)) {
			if (index->any_ahi_pages()) {
				return false;
			}
		}
#endif /* BTR_CUR_HASH_ADAPT */

		ut_ad(!table->fts);
		return true;
	}

	return false;
}

#ifdef BTR_CUR_HASH_ADAPT
/** @return a clone of this */
dict_index_t *dict_index_t::clone() const
{
  ut_ad(n_fields);
  ut_ad(is_btree());
  ut_ad(online_status == ONLINE_INDEX_COMPLETE);
  ut_ad(is_committed());
  ut_ad(!is_dummy);
  ut_ad(!parser);
  ut_ad(!online_log);
  ut_ad(!rtr_track);

  const size_t size= sizeof *this + n_fields * sizeof(*fields) +
    1 + strlen(name) +
    n_uniq * (sizeof *stat_n_diff_key_vals +
              sizeof *stat_n_sample_sizes +
              sizeof *stat_n_non_null_key_vals);

  mem_heap_t* heap= mem_heap_create(size);
  dict_index_t *index= static_cast<dict_index_t*>
    (mem_heap_alloc(heap, sizeof *this));
  *index= *this;
  index->lock.SRW_LOCK_INIT(index_tree_rw_lock_key);
  index->heap= heap;
  index->name= mem_heap_strdup(heap, name);
  index->fields= static_cast<dict_field_t*>
    (mem_heap_dup(heap, fields, n_fields * sizeof *fields));
  index->stat_n_diff_key_vals= static_cast<ib_uint64_t*>
    (mem_heap_zalloc(heap, n_uniq * sizeof *stat_n_diff_key_vals));
  index->stat_n_sample_sizes= static_cast<ib_uint64_t*>
    (mem_heap_zalloc(heap, n_uniq * sizeof *stat_n_sample_sizes));
  index->stat_n_non_null_key_vals= static_cast<ib_uint64_t*>
    (mem_heap_zalloc(heap, n_uniq * sizeof *stat_n_non_null_key_vals));
  new (&index->zip_pad.mutex) std::mutex();
  return index;
}

/** Clone this index for lazy dropping of the adaptive hash.
@return this or a clone */
dict_index_t *dict_index_t::clone_if_needed()
{
  if (!search_info.ref_count)
    return this;
  dict_index_t *prev= UT_LIST_GET_PREV(indexes, this);

  table->autoinc_mutex.wr_lock();
  UT_LIST_REMOVE(table->indexes, this);
  UT_LIST_ADD_LAST(table->freed_indexes, this);
  dict_index_t *index= clone();
  set_freed();
  if (prev)
    UT_LIST_INSERT_AFTER(table->indexes, prev, index);
  else
    UT_LIST_ADD_FIRST(table->indexes, index);
  table->autoinc_mutex.wr_unlock();
  return index;
}
#endif /* BTR_CUR_HASH_ADAPT */

/** Evict unused, unlocked tables from table_LRU.
@param half whether to consider half the tables only (instead of all)
@return number of tables evicted */
ulint dict_sys_t::evict_table_LRU(bool half) noexcept
{
#ifdef MYSQL_DYNAMIC_PLUGIN
	constexpr ulint max_tables = 400;
#else
	extern ulong tdc_size;
	const ulint max_tables = tdc_size;
#endif
	ulint n_evicted = 0;

	lock(SRW_LOCK_CALL);
	ut_ad(dict_lru_validate());

	const ulint len = UT_LIST_GET_LEN(table_LRU);

	if (len < max_tables) {
func_exit:
		unlock();
		return(n_evicted);
	}

	const ulint check_up_to = half ? len / 2 : 0;
	ulint i = len;

	/* Find a suitable candidate to evict from the cache. Don't scan the
	entire LRU list. Only scan pct_check list entries. */

	for (dict_table_t *table = UT_LIST_GET_LAST(table_LRU);
	     table && i > check_up_to && (len - n_evicted) > max_tables; --i) {
		dict_table_t* prev_table = UT_LIST_GET_PREV(table_LRU, table);

		if (dict_table_can_be_evicted(table)) {
			remove(table, true);
			++n_evicted;
		}

		table = prev_table;
	}

	goto func_exit;
}

/** Looks for an index with the given id given a table instance.
@param[in]	table	table instance
@param[in]	id	index id
@return index or NULL */
dict_index_t*
dict_table_find_index_on_id(
	const dict_table_t*	table,
	index_id_t		id)
{
	dict_index_t*	index;

	for (index = dict_table_get_first_index(table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {

		if (id == index->id) {
			/* Found */

			return(index);
		}
	}

	return(NULL);
}

/** Function object to remove a foreign key constraint from the
referenced_set of the referenced table.  The foreign key object is
also removed from the dictionary cache.  The foreign key constraint
is not removed from the foreign_set of the table containing the
constraint. */
struct dict_foreign_remove_partial
{
	void operator()(dict_foreign_t* foreign) {
		dict_table_t*	table = foreign->referenced_table;
		if (table != NULL) {
			table->referenced_set.erase(foreign);
		}
		dict_foreign_free(foreign);
	}
};

/** This function returns a new path name after replacing the basename
in an old path with a new basename.  The old_path is a full path
name including the extension.  The tablename is in the normal
form "databasename/tablename".  The new base name is found after
the forward slash.  Both input strings are null terminated.

This function allocates memory to be returned.  It is the callers
responsibility to free the return value after it is no longer needed.

@param[in]	old_path		Pathname
@param[in]	tablename		Contains new base name
@return own: new full pathname */
static char *dir_pathname(const char *old_path, span<const char> tablename)
{
  /* Split the tablename into its database and table name components.
  They are separated by a '/'. */
  const char *base_name= tablename.data();
  for (const char *last= tablename.end(); last > tablename.data(); last--)
  {
    if (last[-1] == '/')
    {
      base_name= last;
      break;
    }
  }
  const size_t base_name_len= tablename.end() - base_name;

  /* Find the offset of the last slash. We will strip off the
  old basename.ibd which starts after that slash. */
  const char *last_slash= strrchr(old_path, '/');
#ifdef _WIN32
  if (const char *last= strrchr(old_path, '\\'))
    if (last > last_slash)
      last_slash= last;
#endif

  size_t dir_len= last_slash
    ? size_t(last_slash - old_path)
    : strlen(old_path);

  /* allocate a new path and move the old directory path to it. */
  size_t new_path_len= dir_len + base_name_len + sizeof "/.ibd";
  char *new_path= static_cast<char*>(ut_malloc_nokey(new_path_len));
  memcpy(new_path, old_path, dir_len);
  snprintf(new_path + dir_len, new_path_len - dir_len, "/%.*s.ibd",
           int(base_name_len), base_name);
  return new_path;
}

/** Rename the data file.
@param new_name     name of the table
@param replace      whether to replace the file with the new name
                    (as part of rolling back TRUNCATE) */
dberr_t
dict_table_t::rename_tablespace(span<const char> new_name, bool replace) const
{
  ut_ad(dict_table_is_file_per_table(this));
  ut_ad(!is_temporary());

  if (!space)
    return DB_SUCCESS;

  const char *old_path= UT_LIST_GET_FIRST(space->chain)->name;
  const bool data_dir= DICT_TF_HAS_DATA_DIR(flags);
  char *path= data_dir
    ? dir_pathname(old_path, new_name)
    : fil_make_filepath(nullptr, new_name, IBD, false);
  dberr_t err;
  if (!path)
    err= DB_OUT_OF_MEMORY;
  else if (!strcmp(path, old_path))
    err= DB_SUCCESS;
  else if (data_dir &&
           DB_SUCCESS != RemoteDatafile::create_link_file(new_name, path))
    err= DB_TABLESPACE_EXISTS;
  else
  {
    space->x_lock();
    err= space->rename(path, true, replace);
    if (data_dir)
    {
      if (err == DB_SUCCESS)
        new_name= {name.m_name, strlen(name.m_name)};
      RemoteDatafile::delete_link_file(new_name);
    }
    space->x_unlock();
  }

  ut_free(path);
  return err;
}

/**********************************************************************//**
Renames a table object.
@return TRUE if success */
dberr_t
dict_table_rename_in_cache(
/*=======================*/
	dict_table_t*	table,		/*!< in/out: table */
	span<const char> new_name,	/*!< in: new name */
	bool		replace_new_file)
					/*!< in: whether to replace the
					file with the new name
					(as part of rolling back TRUNCATE) */
{
	dict_foreign_t*	foreign;
	char		old_name[MAX_FULL_NAME_LEN + 1];

	ut_ad(dict_sys.locked());

	/* store the old/current name to an automatic variable */
	const size_t old_name_len = strlen(table->name.m_name);
	ut_a(old_name_len < sizeof old_name);
	strcpy(old_name, table->name.m_name);

	if (!dict_table_is_file_per_table(table)) {
	} else if (dberr_t err = table->rename_tablespace(new_name,
							  replace_new_file)) {
		return err;
	}

	/* Remove table from the hash tables of tables */
	dict_sys.table_hash.cell_get(my_crc32c(0, table->name.m_name,
					       old_name_len))
		->remove(*table, &dict_table_t::name_hash);

	bool keep_mdl_name = !table->name.is_temporary();

	if (!keep_mdl_name) {
	} else if (const char* s = static_cast<const char*>
		   (memchr(new_name.data(), '/', new_name.size()))) {
		keep_mdl_name = new_name.end() - s >= 5
			&& !memcmp(s, "/#sql", 5);
	}

	if (keep_mdl_name) {
		/* Preserve the original table name for
		dict_table_t::parse_name() and dict_acquire_mdl_shared(). */
		table->mdl_name.m_name = mem_heap_strdup(table->heap,
							 table->name.m_name);
	}

	if (new_name.size() > strlen(table->name.m_name)) {
		/* We allocate MAX_FULL_NAME_LEN + 1 bytes here to avoid
		memory fragmentation, we assume a repeated calls of
		ut_realloc() with the same size do not cause fragmentation */
		ut_a(new_name.size() <= MAX_FULL_NAME_LEN);

		table->name.m_name = static_cast<char*>(
			ut_realloc(table->name.m_name, MAX_FULL_NAME_LEN + 1));
	}
	memcpy(table->name.m_name, new_name.data(), new_name.size());
	table->name.m_name[new_name.size()] = '\0';

	if (!keep_mdl_name) {
		table->mdl_name.m_name = table->name.m_name;
	}

	/* Add table to hash table of tables */
	ut_ad(!table->name_hash);
	dict_table_t** after = reinterpret_cast<dict_table_t**>(
		&dict_sys.table_hash.cell_get(my_crc32c(0, new_name.data(),
							new_name.size()))
		->node);
	for (; *after; after = &(*after)->name_hash) {
		ut_ad((*after)->cached);
		ut_a(strcmp((*after)->name.m_name, new_name.data()));
	}
	*after = table;

	if (table->name.is_temporary()) {
		/* In ALTER TABLE we think of the rename table operation
		in the direction table -> temporary table (#sql...)
		as dropping the table with the old name and creating
		a new with the new name. Thus we kind of drop the
		constraints from the dictionary cache here. The foreign key
		constraints will be inherited to the new table from the
		system tables through a call of dict_load_foreigns. */

		/* Remove the foreign constraints from the cache */
		std::for_each(table->foreign_set.begin(),
			      table->foreign_set.end(),
			      dict_foreign_remove_partial());
		table->foreign_set.clear();

		/* Reset table field in referencing constraints */
		for (dict_foreign_set::iterator it
			= table->referenced_set.begin();
		     it != table->referenced_set.end();
		     ++it) {

			foreign = *it;
			foreign->referenced_table = NULL;
			foreign->referenced_index = NULL;

		}

		/* Make the set of referencing constraints empty */
		table->referenced_set.clear();

		return(DB_SUCCESS);
	}

	/* Update the table name fields in foreign constraints, and update also
	the constraint id of new format >= 4.0.18 constraints. Note that at
	this point we have already changed table->name to the new name. */

	dict_foreign_set	fk_set;

	for (;;) {

		dict_foreign_set::iterator	it
			= table->foreign_set.begin();

		if (it == table->foreign_set.end()) {
			break;
		}

		foreign = *it;

		if (foreign->referenced_table) {
			foreign->referenced_table->referenced_set.erase(foreign);
		}

		/* Allocate a name buffer;
		TODO: store buf len to save memory */

		foreign->foreign_table_name = mem_heap_strdup(
			foreign->heap, table->name.m_name);
		foreign->foreign_table_name_lookup_set();

		const char* sql_id = foreign->sql_id();
		size_t fklen = snprintf(nullptr, 0, "%s\377%s",
					table->name.m_name, sql_id);
		char* id = foreign->id;
		if (fklen++ > strlen(id)) {
			id = static_cast<char*>(
				mem_heap_alloc(foreign->heap, fklen));
		}
		table->foreign_set.erase(it);
		foreign->id = id;
		snprintf(id, fklen, "%s\377%s", table->name.m_name, sql_id);
		fk_set.insert(foreign);

		if (foreign->referenced_table) {
			foreign->referenced_table->referenced_set.insert(foreign);
		}
	}

	ut_a(table->foreign_set.empty());
	table->foreign_set.swap(fk_set);

	for (dict_foreign_set::iterator it = table->referenced_set.begin();
	     it != table->referenced_set.end();
	     ++it) {

		foreign = *it;

		/* Allocate a name buffer;
		TODO: store buf len to save memory */

		foreign->referenced_table_name = mem_heap_strdup(
			foreign->heap, table->name.m_name);

		foreign->referenced_table_name_lookup_set();
	}

	return(DB_SUCCESS);
}

/** Evict a table definition from the InnoDB data dictionary cache.
@param[in,out]	table	cached table definition to be evicted
@param[in]	lru	whether this is part of least-recently-used eviction
@param[in]	keep	whether to keep (not free) the object */
void dict_sys_t::remove(dict_table_t* table, bool lru, bool keep) noexcept
{
	dict_foreign_t*	foreign;
	dict_index_t*	index;

	ut_ad(dict_lru_validate());
	ut_a(table->get_ref_count() == 0);
	ut_a(table->n_rec_locks == 0);
	ut_ad(find(table));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	/* Remove the foreign constraints from the cache */
	std::for_each(table->foreign_set.begin(), table->foreign_set.end(),
		      dict_foreign_remove_partial());
	table->foreign_set.clear();

	/* Reset table field in referencing constraints */
	for (dict_foreign_set::iterator it = table->referenced_set.begin();
	     it != table->referenced_set.end();
	     ++it) {

		foreign = *it;
		foreign->referenced_table = NULL;
		foreign->referenced_index = NULL;
	}

	/* Remove the indexes from the cache */

	for (index = UT_LIST_GET_LAST(table->indexes);
	     index != NULL;
	     index = UT_LIST_GET_LAST(table->indexes)) {

		dict_index_remove_from_cache_low(table, index, lru);
	}

	/* Remove table from the hash tables of tables */
	table_hash.cell_get(my_crc32c(0, table->name.m_name,
				      strlen(table->name.m_name)))
		->remove(*table, &dict_table_t::name_hash);
	(table->is_temporary() ? temp_id_hash : table_id_hash)
		.cell_get(ut_fold_ull(table->id))
		->remove(*table, &dict_table_t::id_hash);

	/* Remove table from LRU or non-LRU list. */
	if (table->can_be_evicted) {
		UT_LIST_REMOVE(table_LRU, table);
	} else {
		UT_LIST_REMOVE(table_non_LRU, table);
	}

	/* Free virtual column template if any */
	if (table->vc_templ != NULL) {
		dict_free_vc_templ(table->vc_templ);
		UT_DELETE(table->vc_templ);
	}

	table->lock_mutex_destroy();

	if (keep) {
		table->autoinc_mutex.destroy();
		return;
	}

#ifdef BTR_CUR_HASH_ADAPT
	if (table->fts) {
		fts_optimize_remove_table(table);
		table->fts->~fts_t();
		table->fts = nullptr;
	}

	table->autoinc_mutex.wr_lock();

	ulint freed = UT_LIST_GET_LEN(table->freed_indexes);

	table->vc_templ = NULL;
	table->id = 0;
	table->autoinc_mutex.wr_unlock();

	if (UNIV_UNLIKELY(freed != 0)) {
		return;
	}
#endif /* BTR_CUR_HASH_ADAPT */

	table->autoinc_mutex.destroy();
	dict_mem_table_free(table);
}

/****************************************************************//**
If the given column name is reserved for InnoDB system columns, return
TRUE.
@return TRUE if name is reserved */
ibool
dict_col_name_is_reserved(
/*======================*/
	const LEX_CSTRING &name)	/*!< in: column name */
{
	static Lex_ident_column reserved_names[] = {
		"DB_ROW_ID"_Lex_ident_column,
		"DB_TRX_ID"_Lex_ident_column,
		"DB_ROLL_PTR"_Lex_ident_column
	};

	compile_time_assert(UT_ARR_SIZE(reserved_names) == DATA_N_SYS_COLS);

	for (ulint i = 0; i < UT_ARR_SIZE(reserved_names); i++) {
		if (reserved_names[i].streq(name)) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/** Adds an index to the dictionary cache, with possible indexing newly
added column.
@param[in,out]	index	index; NOTE! The index memory
			object is freed in this function!
@param[in]	page_no	root page number of the index
@param[in]	add_v	virtual columns being added along with ADD INDEX
@return DB_SUCCESS, or DB_CORRUPTION */
dberr_t
dict_index_add_to_cache(
	dict_index_t*&		index,
	ulint			page_no,
	const dict_add_v_col_t* add_v)
{
	dict_index_t*	new_index;
	ulint		n_ord;
	ulint		i;

	ut_ad(dict_sys.locked());
	ut_ad(index->n_def == index->n_fields);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
	ut_ad(!dict_index_is_online_ddl(index));

	ut_d(mem_heap_validate(index->heap));
	ut_a(!dict_index_is_clust(index)
	     || UT_LIST_GET_LEN(index->table->indexes) == 0);
	ut_ad(dict_index_is_clust(index) || !index->table->no_rollback());

	if (!dict_index_find_cols(index, add_v)) {

		dict_mem_index_free(index);
		index = NULL;
		return DB_CORRUPTION;
	}

	/* Build the cache internal representation of the index,
	containing also the added system fields */

	if (dict_index_is_clust(index)) {
		new_index = dict_index_build_internal_clust(index);
	} else {
		new_index = (index->type & DICT_FTS)
			? dict_index_build_internal_fts(index)
			: dict_index_build_internal_non_clust(index);
		new_index->n_core_null_bytes = static_cast<uint8_t>(
			UT_BITS_IN_BYTES(unsigned(new_index->n_nullable)));
	}

	/* Set the n_fields value in new_index to the actual defined
	number of fields in the cache internal representation */

	new_index->n_fields = new_index->n_def;
	new_index->trx_id = index->trx_id;
	new_index->set_committed(index->is_committed());

	n_ord = new_index->n_uniq;
	/* Flag the ordering columns and also set column max_prefix */

	for (i = 0; i < n_ord; i++) {
		const dict_field_t*	field
			= dict_index_get_nth_field(new_index, i);

		/* Check the column being added in the index for
		the first time and flag the ordering column. */
		if (field->col->ord_part == 0 ) {
			field->col->max_prefix = field->prefix_len;
			field->col->ord_part = 1;
		} else if (field->prefix_len == 0) {
			/* Set the max_prefix for a column to 0 if
			its prefix length is 0 (for this index)
			even if it was a part of any other index
			with some prefix length. */
			field->col->max_prefix = 0;
		} else if (field->col->max_prefix != 0
			   && field->prefix_len
			   > field->col->max_prefix) {
			/* Set the max_prefix value based on the
			prefix_len. */
			ut_ad(field->col->is_binary()
			      || field->prefix_len % field->col->mbmaxlen == 0
			      || field->prefix_len % 4 == 0);
			field->col->max_prefix = field->prefix_len;
		}
		ut_ad(field->col->ord_part == 1);
	}

	new_index->stat_n_diff_key_vals =
		static_cast<ib_uint64_t*>(mem_heap_zalloc(
			new_index->heap,
			dict_index_get_n_unique(new_index)
			* sizeof(*new_index->stat_n_diff_key_vals)));

	new_index->stat_n_sample_sizes =
		static_cast<ib_uint64_t*>(mem_heap_zalloc(
			new_index->heap,
			dict_index_get_n_unique(new_index)
			* sizeof(*new_index->stat_n_sample_sizes)));

	new_index->stat_n_non_null_key_vals =
		static_cast<ib_uint64_t*>(mem_heap_zalloc(
			new_index->heap,
			dict_index_get_n_unique(new_index)
			* sizeof(*new_index->stat_n_non_null_key_vals)));

	new_index->stat_index_size = 1;
	new_index->stat_n_leaf_pages = 1;

	/* Add the new index as the last index for the table */

	UT_LIST_ADD_LAST(new_index->table->indexes, new_index);

	new_index->page = unsigned(page_no);
	new_index->lock.SRW_LOCK_INIT(index_tree_rw_lock_key);

	new_index->n_core_fields = new_index->n_fields;

	dict_mem_index_free(index);
	index = new_index;
	return DB_SUCCESS;
}

/**********************************************************************//**
Removes an index from the dictionary cache. */
TRANSACTIONAL_TARGET
static
void
dict_index_remove_from_cache_low(
/*=============================*/
	dict_table_t*	table,		/*!< in/out: table */
	dict_index_t*	index,		/*!< in, own: index */
	ibool		lru_evict)	/*!< in: TRUE if index being evicted
					to make room in the table LRU list */
{
	ut_ad(table && index);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
	ut_ad(dict_sys.locked());
	ut_ad(table->id);
#ifdef BTR_CUR_HASH_ADAPT
	ut_ad(!index->freed());
#endif /* BTR_CUR_HASH_ADAPT */

	/* No need to acquire the dict_index_t::lock here because
	there can't be any active operations on this index (or table). */

	if (index->online_log) {
		row_log_free(index->online_log);
		index->online_log = NULL;
	}

	/* Remove the index from the list of indexes of the table */
	UT_LIST_REMOVE(table->indexes, index);

	/* The index is being dropped, remove any compression stats for it. */
	if (!lru_evict && DICT_TF_GET_ZIP_SSIZE(index->table->flags)) {
		mysql_mutex_lock(&page_zip_stat_per_index_mutex);
		page_zip_stat_per_index.erase(index->id);
		mysql_mutex_unlock(&page_zip_stat_per_index_mutex);
	}

	/* Remove the index from affected virtual column index list */
	index->detach_columns();

#ifdef BTR_CUR_HASH_ADAPT
	/* We always create search info whether or not adaptive
	hash index is enabled or not. */
	/* We are not allowed to free the in-memory index struct
	dict_index_t until all entries in the adaptive hash index
	that point to any of the page belonging to his b-tree index
	are dropped. This is so because dropping of these entries
	require access to dict_index_t struct. To avoid such scenario
	We keep a count of number of such pages in the search_info and
	only free the dict_index_t struct when this count drops to
	zero. See also: dict_table_can_be_evicted() */

	if (index->any_ahi_pages()) {
		table->autoinc_mutex.wr_lock();
		index->set_freed();
		UT_LIST_ADD_LAST(table->freed_indexes, index);
		table->autoinc_mutex.wr_unlock();
		return;
	}
#endif /* BTR_CUR_HASH_ADAPT */

	index->lock.free();

	dict_mem_index_free(index);
}

/**********************************************************************//**
Removes an index from the dictionary cache. */
void
dict_index_remove_from_cache(
/*=========================*/
	dict_table_t*	table,	/*!< in/out: table */
	dict_index_t*	index)	/*!< in, own: index */
{
	dict_index_remove_from_cache_low(table, index, FALSE);
}

/** Tries to find column names for the index and sets the col field of the
index.
@param[in]	table	table
@param[in,out]	index	index
@param[in]	add_v	new virtual columns added along with an add index call
@return whether the column names were found */
static
bool
dict_index_find_cols(
	dict_index_t*		index,
	const dict_add_v_col_t*	add_v)
{
	std::vector<ulint, ut_allocator<ulint> >	col_added;
	std::vector<ulint, ut_allocator<ulint> >	v_col_added;

	const dict_table_t* table = index->table;
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(dict_sys.locked());

	for (ulint i = 0; i < index->n_fields; i++) {
		ulint		j;
		dict_field_t*	field = dict_index_get_nth_field(index, i);
		const Lex_ident_column field_name = Lex_cstring_strlen(field->name);

		for (j = 0; j < table->n_cols; j++) {
			if (field_name.
				streq(dict_table_get_col_name(table, j))) {

				/* Check if same column is being assigned again
				which suggest that column has duplicate name. */
				bool exists =
					std::find(col_added.begin(),
						  col_added.end(), j)
					!= col_added.end();

				if (exists) {
					/* Duplicate column found. */
					goto dup_err;
				}

				field->col = dict_table_get_nth_col(table, j);

				col_added.push_back(j);

				goto found;
			}
		}

		/* Let's check if it is a virtual column */
		for (j = 0; j < table->n_v_cols; j++) {
			if (!strcmp(dict_table_get_v_col_name(table, j).str,
				    field->name)) {

				/* Check if same column is being assigned again
				which suggest that column has duplicate name. */
				bool exists =
					std::find(v_col_added.begin(),
						  v_col_added.end(), j)
					!= v_col_added.end();

				if (exists) {
					/* Duplicate column found. */
					break;
				}

				field->col = reinterpret_cast<dict_col_t*>(
					dict_table_get_nth_v_col(table, j));

				v_col_added.push_back(j);

				goto found;
			}
		}

		if (add_v) {
			for (j = 0; j < add_v->n_v_col; j++) {
				if (!strcmp(add_v->v_col_name[j],
					    field->name)) {
					field->col = const_cast<dict_col_t*>(
						&add_v->v_col[j].m_col);
					goto found;
				}
			}
		}

dup_err:
#ifdef UNIV_DEBUG
		/* It is an error not to find a matching column. */
		ib::error() << "No matching column for " << field->name
			<< " in index " << index->name
			<< " of table " << table->name;
#endif /* UNIV_DEBUG */
		return(FALSE);

found:
		;
	}

	return(TRUE);
}

/** Add a column to an index.
@param index          index
@param table          table
@param col            column
@param prefix_len     column prefix length
@param descending     whether to use descending order */
void dict_index_add_col(dict_index_t *index, const dict_table_t *table,
                        dict_col_t *col, ulint prefix_len, bool descending)
{
	dict_field_t*	field;
	const char*	col_name;

	if (col->is_virtual()) {
		dict_v_col_t*	v_col = reinterpret_cast<dict_v_col_t*>(col);
		/* Register the index with the virtual column index list */
		v_col->v_indexes.push_front(dict_v_idx_t(index, index->n_def));
		col_name = dict_table_get_v_col_name_mysql(
			table, dict_col_get_no(col)).str;
	} else {
		col_name = dict_table_get_col_name(table, dict_col_get_no(col)).str;
	}

	dict_mem_index_add_field(index, col_name, prefix_len);

	field = dict_index_get_nth_field(index, unsigned(index->n_def) - 1);

	field->col = col;
	field->fixed_len = static_cast<uint16_t>(
		dict_col_get_fixed_size(
			col, dict_table_is_comp(table)))
		& ((1U << 10) - 1);

	if (prefix_len && field->fixed_len > prefix_len) {
		field->fixed_len = static_cast<uint16_t>(prefix_len)
			& ((1U << 10) - 1);
	}

	/* Long fixed-length fields that need external storage are treated as
	variable-length fields, so that the extern flag can be embedded in
	the length word. */

	if (field->fixed_len > DICT_MAX_FIXED_COL_LEN) {
		field->fixed_len = 0;
	}

	field->descending = descending;

	/* The comparison limit above must be constant.  If it were
	changed, the disk format of some fixed-length columns would
	change, which would be a disaster. */
	compile_time_assert(DICT_MAX_FIXED_COL_LEN == 768);

	if (!(col->prtype & DATA_NOT_NULL)) {
		index->n_nullable++;
	}
}

/*******************************************************************//**
Copies fields contained in index2 to index1. */
static
void
dict_index_copy(
/*============*/
	dict_index_t*		index1,	/*!< in: index to copy to */
	const dict_index_t*	index2,	/*!< in: index to copy from */
	ulint			start,	/*!< in: first position to copy */
	ulint			end)	/*!< in: last position to copy */
{
	dict_field_t*	field;
	ulint		i;

	/* Copy fields contained in index2 */

	for (i = start; i < end; i++) {

		field = dict_index_get_nth_field(index2, i);

		dict_index_add_col(index1, index2->table, field->col,
				   field->prefix_len, field->descending);
	}
}

/*******************************************************************//**
Copies types of fields contained in index to tuple. */
void
dict_index_copy_types(
/*==================*/
	dtuple_t*		tuple,		/*!< in/out: data tuple */
	const dict_index_t*	index,		/*!< in: index */
	ulint			n_fields)	/*!< in: number of
						field types to copy */
{
	for (ulint i = 0; i < n_fields; i++) {
		const dict_field_t*	ifield;
		dtype_t*		dfield_type;

		ifield = dict_index_get_nth_field(index, i);
		dfield_type = dfield_get_type(dtuple_get_nth_field(tuple, i));
		dict_col_copy_type(dict_field_get_col(ifield), dfield_type);
		if (dict_index_is_spatial(index)
		    && DATA_GEOMETRY_MTYPE(dfield_type->mtype)) {
			dfield_type->prtype |= DATA_GIS_MBR;
		}
	}
}

/** Copies types of virtual columns contained in table to tuple and sets all
fields of the tuple to the SQL NULL value.  This function should
be called right after dtuple_create().
@param[in,out]	tuple	data tuple
@param[in]	table	table
*/
void
dict_table_copy_v_types(
	dtuple_t*		tuple,
	const dict_table_t*	table)
{
	/* tuple could have more virtual columns than existing table,
	if we are calling this for creating index along with adding
	virtual columns */
	ulint	n_fields = std::min<ulint>(dtuple_get_n_v_fields(tuple),
					   table->n_v_def);

	for (ulint i = 0; i < n_fields; i++) {

		dfield_t*	dfield	= dtuple_get_nth_v_field(tuple, i);
		dtype_t*	dtype	= dfield_get_type(dfield);

		dfield_set_null(dfield);
		dict_col_copy_type(
			&(dict_table_get_nth_v_col(table, i)->m_col),
			dtype);
	}
}
/*******************************************************************//**
Copies types of columns contained in table to tuple and sets all
fields of the tuple to the SQL NULL value.  This function should
be called right after dtuple_create(). */
void
dict_table_copy_types(
/*==================*/
	dtuple_t*		tuple,	/*!< in/out: data tuple */
	const dict_table_t*	table)	/*!< in: table */
{
	ulint		i;

	for (i = 0; i < dtuple_get_n_fields(tuple); i++) {

		dfield_t*	dfield	= dtuple_get_nth_field(tuple, i);
		dtype_t*	dtype	= dfield_get_type(dfield);

		dfield_set_null(dfield);
		dict_col_copy_type(dict_table_get_nth_col(table, i), dtype);
	}

	dict_table_copy_v_types(tuple, table);
}

/*******************************************************************//**
Builds the internal dictionary cache representation for a clustered
index, containing also system fields not defined by the user.
@return own: the internal representation of the clustered index */
static
dict_index_t*
dict_index_build_internal_clust(
/*============================*/
	dict_index_t*		index)	/*!< in: user representation of
					a clustered index */
{
	dict_table_t*	table = index->table;
	dict_index_t*	new_index;
	dict_field_t*	field;
	ulint		trx_id_pos;
	ulint		i;
	ibool*		indexed;

	ut_ad(index->is_primary());
	ut_ad(!index->has_virtual());

	ut_ad(dict_sys.locked());

	/* Create a new index object with certainly enough fields */
	new_index = dict_mem_index_create(index->table, index->name,
					  index->type,
					  unsigned(index->n_fields
						   + table->n_cols));

	/* Copy other relevant data from the old index struct to the new
	struct: it inherits the values */

	new_index->n_user_defined_cols = index->n_fields;

	new_index->id = index->id;

	/* Copy the fields of index */
	dict_index_copy(new_index, index, 0, index->n_fields);

	if (dict_index_is_unique(index)) {
		/* Only the fields defined so far are needed to identify
		the index entry uniquely */

		new_index->n_uniq = new_index->n_def;
	} else {
		/* Also the row id is needed to identify the entry */
		new_index->n_uniq = unsigned(new_index->n_def + 1)
			& dict_index_t::MAX_N_FIELDS;
	}

	new_index->trx_id_offset = 0;

	/* Add system columns, trx id first */

	trx_id_pos = new_index->n_def;

	compile_time_assert(DATA_ROW_ID == 0);
	compile_time_assert(DATA_TRX_ID == 1);
	compile_time_assert(DATA_ROLL_PTR == 2);

	if (!dict_index_is_unique(index)) {
		dict_index_add_col(new_index, table,
				   dict_table_get_sys_col(
					   table, DATA_ROW_ID),
				   0);
		trx_id_pos++;
	}

	dict_index_add_col(
		new_index, table,
		dict_table_get_sys_col(table, DATA_TRX_ID), 0);

	for (i = 0; i < trx_id_pos; i++) {

		ulint	fixed_size = dict_col_get_fixed_size(
			dict_index_get_nth_col(new_index, i),
			dict_table_is_comp(table));

		if (fixed_size == 0) {
			new_index->trx_id_offset = 0;

			break;
		}

		dict_field_t* field = dict_index_get_nth_field(
			new_index, i);
		if (field->prefix_len > 0) {
			new_index->trx_id_offset = 0;

			break;
		}

		/* Add fixed_size to new_index->trx_id_offset.
		Because the latter is a bit-field, an overflow
		can theoretically occur. Check for it. */
		fixed_size += new_index->trx_id_offset;

		new_index->trx_id_offset = static_cast<unsigned>(fixed_size)
			& ((1U << 12) - 1);

		if (new_index->trx_id_offset != fixed_size) {
			/* Overflow. Pretend that this is a
			variable-length PRIMARY KEY. */
			ut_ad(0);
			new_index->trx_id_offset = 0;
			break;
		}
	}

	dict_index_add_col(
		new_index, table,
		dict_table_get_sys_col(table, DATA_ROLL_PTR), 0);

	/* Remember the table columns already contained in new_index */
	indexed = static_cast<ibool*>(
		ut_zalloc_nokey(table->n_cols * sizeof *indexed));

	/* Mark the table columns already contained in new_index */
	for (i = 0; i < new_index->n_def; i++) {

		field = dict_index_get_nth_field(new_index, i);

		/* If there is only a prefix of the column in the index
		field, do not mark the column as contained in the index */

		if (field->prefix_len == 0) {

			indexed[field->col->ind] = TRUE;
		}
	}

	/* Add to new_index non-system columns of table not yet included
	there */
	for (i = 0; i + DATA_N_SYS_COLS < ulint(table->n_cols); i++) {
		dict_col_t*	col = dict_table_get_nth_col(table, i);
		ut_ad(col->mtype != DATA_SYS);

		if (!indexed[col->ind]) {
			dict_index_add_col(new_index, table, col, 0);
		}
	}

	ut_free(indexed);

	ut_ad(UT_LIST_GET_LEN(table->indexes) == 0);

	new_index->n_core_null_bytes = table->supports_instant()
		? dict_index_t::NO_CORE_NULL_BYTES
		: static_cast<uint8_t>(
			UT_BITS_IN_BYTES(unsigned(new_index->n_nullable)));
	new_index->cached = TRUE;

	return(new_index);
}

/*******************************************************************//**
Builds the internal dictionary cache representation for a non-clustered
index, containing also system fields not defined by the user.
@return own: the internal representation of the non-clustered index */
static
dict_index_t*
dict_index_build_internal_non_clust(
/*================================*/
	dict_index_t*		index)	/*!< in: user representation of
					a non-clustered index */
{
	dict_field_t*	field;
	dict_index_t*	new_index;
	dict_index_t*	clust_index;
	dict_table_t*	table = index->table;
	ulint		i;
	ibool*		indexed;

	ut_ad(!index->is_primary());
	ut_ad(dict_sys.locked());

	/* The clustered index should be the first in the list of indexes */
	clust_index = UT_LIST_GET_FIRST(table->indexes);

	ut_ad(clust_index);
	ut_ad(clust_index->is_clust());

	/* Create a new index */
	new_index = dict_mem_index_create(
		index->table, index->name, index->type,
		ulint(index->n_fields + 1 + clust_index->n_uniq));

	/* Copy other relevant data from the old index
	struct to the new struct: it inherits the values */

	new_index->n_user_defined_cols = index->n_fields;

	new_index->id = index->id;

	/* Copy fields from index to new_index */
	dict_index_copy(new_index, index, 0, index->n_fields);

	/* Remember the table columns already contained in new_index */
	indexed = static_cast<ibool*>(
		ut_zalloc_nokey(table->n_cols * sizeof *indexed));

	/* Mark the table columns already contained in new_index */
	for (i = 0; i < new_index->n_def; i++) {

		field = dict_index_get_nth_field(new_index, i);

		if (field->col->is_virtual()) {
			continue;
		}

		/* If there is only a prefix of the column in the index
		field, do not mark the column as contained in the index */

		if (field->prefix_len == 0) {

			indexed[field->col->ind] = TRUE;
		}
	}

	/* Add to new_index the columns necessary to determine the clustered
	index entry uniquely */

	for (i = 0; i < clust_index->n_uniq; i++) {
		field = dict_index_get_nth_field(clust_index, i);

		if (!indexed[field->col->ind] || index->is_spatial()) {
			dict_index_add_col(new_index, table, field->col,
					   field->prefix_len,
					   field->descending);
		}
	}

	ut_free(indexed);

	if (dict_index_is_unique(index)) {
		new_index->n_uniq = index->n_fields;
	} else {
		new_index->n_uniq = new_index->n_def;
	}

	/* Set the n_fields value in new_index to the actual defined
	number of fields */

	new_index->n_fields = new_index->n_def;

	new_index->cached = TRUE;

	return(new_index);
}

/***********************************************************************
Builds the internal dictionary cache representation for an FTS index.
@return own: the internal representation of the FTS index */
static
dict_index_t*
dict_index_build_internal_fts(
/*==========================*/
	dict_index_t*	index)	/*!< in: user representation of an FTS index */
{
	dict_index_t*	new_index;

	ut_ad(index->type & DICT_FTS);
	ut_ad(dict_sys.locked());

	/* Create a new index */
	new_index = dict_mem_index_create(index->table, index->name,
					  index->type, index->n_fields);

	/* Copy other relevant data from the old index struct to the new
	struct: it inherits the values */

	new_index->n_user_defined_cols = index->n_fields;

	new_index->id = index->id;

	/* Copy fields from index to new_index */
	dict_index_copy(new_index, index, 0, index->n_fields);

	new_index->n_uniq = 0;
	new_index->cached = TRUE;

	dict_table_t* table = index->table;

	if (table->fts->cache == NULL) {
		table->fts->cache = fts_cache_create(table);
	}

	mysql_mutex_lock(&table->fts->cache->init_lock);
	/* Notify the FTS cache about this index. */
	fts_cache_index_cache_create(table, new_index);
	mysql_mutex_unlock(&table->fts->cache->init_lock);

	return(new_index);
}
/*====================== FOREIGN KEY PROCESSING ========================*/

/**********************************************************************//**
Removes a foreign constraint struct from the dictionary cache. */
void
dict_foreign_remove_from_cache(
/*===========================*/
	dict_foreign_t*	foreign)	/*!< in, own: foreign constraint */
{
	ut_ad(dict_sys.locked());
	ut_a(foreign);

	if (foreign->referenced_table != NULL) {
		foreign->referenced_table->referenced_set.erase(foreign);
	}

	if (foreign->foreign_table != NULL) {
		foreign->foreign_table->foreign_set.erase(foreign);
	}

	dict_foreign_free(foreign);
}

/**********************************************************************//**
Looks for the foreign constraint from the foreign and referenced lists
of a table.
@return foreign constraint */
static
dict_foreign_t*
dict_foreign_find(
/*==============*/
	dict_table_t*	table,		/*!< in: table object */
	dict_foreign_t*	foreign)	/*!< in: foreign constraint */
{
	ut_ad(dict_sys.frozen());

	ut_ad(dict_foreign_set_validate(table->foreign_set));
	ut_ad(dict_foreign_set_validate(table->referenced_set));

	dict_foreign_set::iterator it = table->foreign_set.find(foreign);

	if (it != table->foreign_set.end()) {
		return(*it);
	}

	it = table->referenced_set.find(foreign);

	if (it != table->referenced_set.end()) {
		return(*it);
	}

	return(NULL);
}

/*********************************************************************//**
Tries to find an index whose first fields are the columns in the array,
in the same order and is not marked for deletion and is not the same
as types_idx.
@return matching index, NULL if not found */
dict_index_t*
dict_foreign_find_index(
/*====================*/
	const dict_table_t*	table,	/*!< in: table */
	const char**		col_names,
					/*!< in: column names, or NULL
					to use table->col_names */
	const char**		columns,/*!< in: array of column names */
	ulint			n_cols,	/*!< in: number of columns */
	const dict_index_t*	types_idx,
					/*!< in: NULL or an index
					whose types the column types
					must match */
	bool			check_charsets,
					/*!< in: whether to check
					charsets.  only has an effect
					if types_idx != NULL */
	ulint			check_null,
					/*!< in: nonzero if none of
					the columns must be declared
					NOT NULL */
	fkerr_t*		error,	/*!< out: error code */
	ulint*			err_col_no,
					/*!< out: column number where
					error happened */
	dict_index_t**		err_index)
					/*!< out: index where error
					happened */
{
	ut_ad(dict_sys.frozen());

	if (error) {
		*error = FK_INDEX_NOT_FOUND;
	}

	for (dict_index_t* index = dict_table_get_first_index(table);
	     index;
	     index = dict_table_get_next_index(index)) {
		if (!index->to_be_dropped
		    && !dict_index_is_online_ddl(index)
		    && dict_foreign_qualify_index(
			    table, col_names, columns, n_cols,
			    index, types_idx,
			    check_charsets, check_null,
			    error, err_col_no, err_index)) {
			if (error) {
				*error = FK_SUCCESS;
			}

			return(index);
		}
	}

	return(NULL);
}
/**********************************************************************//**
Report an error in a foreign key definition. */
static
void
dict_foreign_error_report_low(
/*==========================*/
	FILE*		file,	/*!< in: output stream */
	const char*	name)	/*!< in: table name */
{
	rewind(file);
	ut_print_timestamp(file);
	fprintf(file, " Error in foreign key constraint of table %s:\n",
		name);
}

/**********************************************************************//**
Report an error in a foreign key definition. */
static
void
dict_foreign_error_report(
/*======================*/
	FILE*		file,	/*!< in: output stream */
	dict_foreign_t*	fk,	/*!< in: foreign key constraint */
	const char*	msg)	/*!< in: the error message */
{
	std::string fk_str;
	mysql_mutex_lock(&dict_foreign_err_mutex);
	dict_foreign_error_report_low(file, fk->foreign_table_name);
	fputs(msg, file);
	fputs(" Constraint:\n", file);
	fk_str = dict_print_info_on_foreign_key_in_create_format(NULL, fk, TRUE);
	fputs(fk_str.c_str(), file);
	putc('\n', file);
	if (fk->foreign_index) {
		fprintf(file, "The index in the foreign key in table is"
			" %s\n%s\n", fk->foreign_index->name(),
			FOREIGN_KEY_CONSTRAINTS_MSG);
	}
	mysql_mutex_unlock(&dict_foreign_err_mutex);
}

/**********************************************************************//**
Adds a foreign key constraint object to the dictionary cache. May free
the object if there already is an object with the same identifier in.
At least one of the foreign table and the referenced table must already
be in the dictionary cache!
@return DB_SUCCESS or error code */
dberr_t
dict_foreign_add_to_cache(
/*======================*/
	dict_foreign_t*		foreign,
				/*!< in, own: foreign key constraint */
	const char**		col_names,
				/*!< in: column names, or NULL to use
				foreign->foreign_table->col_names */
	bool			check_charsets,
				/*!< in: whether to check charset
				compatibility */
	dict_err_ignore_t	ignore_err)
				/*!< in: error to be ignored */
{
	dict_table_t*	for_table;
	dict_table_t*	ref_table;
	dict_foreign_t*	for_in_cache		= NULL;
	dict_index_t*	index;
	ibool		added_to_referenced_list= FALSE;
	FILE*		ef			= dict_foreign_err_file;

	DBUG_ENTER("dict_foreign_add_to_cache");
	DBUG_PRINT("dict_foreign_add_to_cache", ("id: %s", foreign->id));

	ut_ad(dict_sys.locked());

	for_table = dict_sys.find_table(
		{foreign->foreign_table_name_lookup,
		 strlen(foreign->foreign_table_name_lookup)});

	ref_table = dict_sys.find_table(
		{foreign->referenced_table_name_lookup,
		 strlen(foreign->referenced_table_name_lookup)});
	ut_a(for_table || ref_table);

	if (for_table) {
		for_in_cache = dict_foreign_find(for_table, foreign);
	}

	if (!for_in_cache && ref_table) {
		for_in_cache = dict_foreign_find(ref_table, foreign);
	}

	if (for_in_cache) {
		dict_foreign_free(foreign);
	} else {
		for_in_cache = foreign;

	}

	if (ref_table && !for_in_cache->referenced_table) {
		index = dict_foreign_find_index(
			ref_table, NULL,
			for_in_cache->referenced_col_names,
			for_in_cache->n_fields, for_in_cache->foreign_index,
			check_charsets, false);

		if (index == NULL
		    && !(ignore_err & DICT_ERR_IGNORE_FK_NOKEY)) {
			dict_foreign_error_report(
				ef, for_in_cache,
				"there is no index in referenced table"
				" which would contain\n"
				"the columns as the first columns,"
				" or the data types in the\n"
				"referenced table do not match"
				" the ones in table.");

			if (for_in_cache == foreign) {
				dict_foreign_free(foreign);
			}

			DBUG_RETURN(DB_CANNOT_ADD_CONSTRAINT);
		}

		for_in_cache->referenced_table = ref_table;
		for_in_cache->referenced_index = index;

		std::pair<dict_foreign_set::iterator, bool>	ret
			= ref_table->referenced_set.insert(for_in_cache);

		ut_a(ret.second);	/* second is true if the insertion
					took place */
		added_to_referenced_list = TRUE;
	}

	if (for_table && !for_in_cache->foreign_table) {
		index = dict_foreign_find_index(
			for_table, col_names,
			for_in_cache->foreign_col_names,
			for_in_cache->n_fields,
			for_in_cache->referenced_index, check_charsets,
			for_in_cache->type
			& (foreign->DELETE_SET_NULL
			   | foreign->UPDATE_SET_NULL));

		if (index == NULL
		    && !(ignore_err & DICT_ERR_IGNORE_FK_NOKEY)) {
			dict_foreign_error_report(
				ef, for_in_cache,
				"there is no index in the table"
				" which would contain\n"
				"the columns as the first columns,"
				" or the data types in the\n"
				"table do not match"
				" the ones in the referenced table\n"
				"or one of the ON ... SET NULL columns"
				" is declared NOT NULL.");

			if (for_in_cache == foreign) {
				if (added_to_referenced_list) {
					const dict_foreign_set::size_type
						n = ref_table->referenced_set
						  .erase(for_in_cache);

					ut_a(n == 1);	/* the number of
							elements removed must
							be one */
				}

				dict_foreign_free(foreign);
			}

			DBUG_RETURN(DB_CANNOT_ADD_CONSTRAINT);
		}

		for_in_cache->foreign_table = for_table;
		for_in_cache->foreign_index = index;

		std::pair<dict_foreign_set::iterator, bool>	ret
			= for_table->foreign_set.insert(for_in_cache);

		ut_a(ret.second);	/* second is true if the insertion
					took place */
	}

	/* We need to move the table to the non-LRU end of the table LRU
	list. Otherwise it will be evicted from the cache. */

	if (ref_table != NULL) {
		dict_sys.prevent_eviction(ref_table);
	}

	if (for_table != NULL) {
		dict_sys.prevent_eviction(for_table);
	}

	ut_ad(dict_lru_validate());
	DBUG_RETURN(DB_SUCCESS);
}

/*********************************************************************//**
Scans from pointer onwards. Stops if is at the start of a copy of
'string' where characters are compared without case sensitivity, and
only outside `` or "" quotes. Stops also at NUL.
@return scanned up to this */
static
const char*
dict_scan_to(
/*=========*/
	const char*	ptr,	/*!< in: scan from */
	const char*	string)	/*!< in: look for this */
{
	char	quote	= '\0';
	bool	escape	= false;

	for (; *ptr; ptr++) {
		if (*ptr == quote) {
			/* Closing quote character: do not look for
			starting quote or the keyword. */

			/* If the quote character is escaped by a
			backslash, ignore it. */
			if (escape) {
				escape = false;
			} else {
				quote = '\0';
			}
		} else if (quote) {
			/* Within quotes: do nothing. */
			if (escape) {
				escape = false;
			} else if (*ptr == '\\') {
				escape = true;
			}
		} else if (*ptr == '`' || *ptr == '"' || *ptr == '\'') {
			/* Starting quote: remember the quote character. */
			quote = *ptr;
		} else {
			/* Outside quotes: look for the keyword. */
			ulint	i;
			for (i = 0; string[i]; i++) {
				if (toupper((int)(unsigned char)(ptr[i]))
				    != toupper((int)(unsigned char)
					       (string[i]))) {
					goto nomatch;
				}
			}
			break;
nomatch:
			;
		}
	}

	return(ptr);
}

/*********************************************************************//**
Accepts a specified string. Comparisons are case-insensitive.
@return if string was accepted, the pointer is moved after that, else
ptr is returned */
static
const char*
dict_accept(
/*========*/
	CHARSET_INFO*	cs,	/*!< in: the character set of ptr */
	const char*	ptr,	/*!< in: scan from this */
	const char*	string,	/*!< in: accept only this string as the next
				non-whitespace string */
	ibool*		success)/*!< out: TRUE if accepted */
{
	const char*	old_ptr = ptr;
	const char*	old_ptr2;

	*success = FALSE;

	while (my_isspace(cs, *ptr)) {
		ptr++;
	}

	old_ptr2 = ptr;

	ptr = dict_scan_to(ptr, string);

	if (*ptr == '\0' || old_ptr2 != ptr) {
		return(old_ptr);
	}

	*success = TRUE;

	return ptr + strlen(string);
}

/*********************************************************************//**
Scans an id. For the lexical definition of an 'id', see the code below.
Strips backquotes or double quotes from around the id.
@return scanned to */
static
const char*
dict_scan_id(
/*=========*/
	CHARSET_INFO*	cs,	/*!< in: the character set of ptr */
	const char*	ptr,	/*!< in: scanned to */
	mem_heap_t*	heap,	/*!< in: heap where to allocate the id
				(NULL=id will not be allocated, but it
				will point to string near ptr) */
	const char**	id)	/*!< out,own: the id; NULL if no id was
				scannable */
{
	char		quote	= '\0';
	ulint		len	= 0;
	const char*	s;
	char*		str;

	*id = NULL;

	while (my_isspace(cs, *ptr)) {
		ptr++;
	}

	if (*ptr == '\0') {

		return(ptr);
	}

	if (*ptr == '`' || *ptr == '"') {
		quote = *ptr++;
	}

	s = ptr;

	if (quote) {
		for (;;) {
			if (!*ptr) {
				/* Syntax error */
				return(ptr);
			}
			if (*ptr == quote) {
				ptr++;
				if (*ptr != quote) {
					break;
				}
			}
			ptr++;
			len++;
		}
	} else {
		while (!my_isspace(cs, *ptr) && *ptr != '(' && *ptr != ')'
		       && *ptr != ',' && *ptr != '\0') {

			ptr++;
		}

		len = ulint(ptr - s);
	}

	if (heap == NULL) {
		/* no heap given: id will point to source string */
		*id = s;
		return(ptr);
	}

	if (quote) {
		char*	d;

		str = d = static_cast<char*>(
			mem_heap_alloc(heap, len + 1));

		while (len--) {
			if ((*d++ = *s++) == quote) {
				s++;
			}
		}
		*d++ = 0;
		len = ulint(d - str);
		ut_ad(*s == quote);
		ut_ad(s + 1 == ptr);
	} else {
		str = mem_heap_strdupl(heap, s, len);
	}

	ulint dstlen = 3 * len + 1;
        char *dst = static_cast<char*>(mem_heap_alloc(heap, dstlen));
	*id = dst;
	uint errors;
	strconvert(cs, str, uint(len), system_charset_info, dst,
                   uint(dstlen), &errors);
	return(ptr);
}

/*********************************************************************//**
Removes MySQL comments from an SQL string. A comment is either
(a) '#' to the end of the line,
(b) '--[space]' to the end of the line, or
(c) '[slash][asterisk]' till the next '[asterisk][slash]' (like the familiar
C comment syntax).
@return own: SQL string stripped from comments; the caller must free
this with ut_free()! */
static
char*
dict_strip_comments(
/*================*/
	const char*	sql_string,	/*!< in: SQL string */
	size_t		sql_length)	/*!< in: length of sql_string */
{
	char*		str;
	const char*	sptr;
	const char*	eptr	= sql_string + sql_length;
	char*		ptr;
	/* unclosed quote character (0 if none) */
	char		quote	= 0;
	bool		escape = false;

	DBUG_ENTER("dict_strip_comments");

	DBUG_PRINT("dict_strip_comments", ("%s", sql_string));

	str = static_cast<char*>(ut_malloc_nokey(sql_length + 1));

	sptr = sql_string;
	ptr = str;

	for (;;) {
scan_more:
		if (sptr >= eptr || *sptr == '\0') {
end_of_string:
			*ptr = '\0';

			ut_a(ptr <= str + sql_length);

			DBUG_PRINT("dict_strip_comments", ("%s", str));
			DBUG_RETURN(str);
		}

		if (*sptr == quote) {
			/* Closing quote character: do not look for
			starting quote or comments. */

			/* If the quote character is escaped by a
			backslash, ignore it. */
			if (escape) {
				escape = false;
			} else {
				quote = 0;
			}
		} else if (quote) {
			/* Within quotes: do not look for
			starting quotes or comments. */
			if (escape) {
				escape = false;
			} else if (*sptr == '\\') {
				escape = true;
			}
		} else if (*sptr == '"' || *sptr == '`' || *sptr == '\'') {
			/* Starting quote: remember the quote character. */
			quote = *sptr;
		} else if (*sptr == '#'
			   || (sptr[0] == '-' && sptr[1] == '-'
			       && sptr[2] == ' ')) {
			for (;;) {
				if (++sptr >= eptr) {
					goto end_of_string;
				}

				/* In Unix a newline is 0x0A while in Windows
				it is 0x0D followed by 0x0A */

				switch (*sptr) {
				case (char) 0X0A:
				case (char) 0x0D:
				case '\0':
					goto scan_more;
				}
			}
		} else if (!quote && *sptr == '/' && *(sptr + 1) == '*') {
			sptr += 2;
			for (;;) {
				if (sptr >= eptr) {
					goto end_of_string;
				}

				switch (*sptr) {
				case '\0':
					goto scan_more;
				case '*':
					if (sptr[1] == '/') {
						sptr += 2;
						goto scan_more;
					}
				}

				sptr++;
			}
		}

		*ptr = *sptr;

		ptr++;
		sptr++;
	}
}

/**********************************************************************//**
Parses the CONSTRAINT id's to be dropped in an ALTER TABLE statement.
@return DB_SUCCESS or DB_CANNOT_DROP_CONSTRAINT if syntax error or the
constraint id does not match */
dberr_t
dict_foreign_parse_drop_constraints(
/*================================*/
	mem_heap_t*	heap,			/*!< in: heap from which we can
						allocate memory */
	trx_t*		trx,			/*!< in: transaction */
	dict_table_t*	table,			/*!< in: table */
	ulint*		n,			/*!< out: number of constraints
						to drop */
	const char***	constraints_to_drop)	/*!< out: id's of the
						constraints to drop */
{
	ibool			success;
	char*			str;
	size_t			len;
	const char*		ptr;
	const char*		ptr1;
	const char*		id;
	CHARSET_INFO*		cs;
	bool			if_exists = false;

	ut_a(trx->mysql_thd);

	cs = thd_charset(trx->mysql_thd);

	*n = 0;

	*constraints_to_drop = static_cast<const char**>(
		mem_heap_alloc(heap, 1000 * sizeof(char*)));

	ptr = innobase_get_stmt_unsafe(trx->mysql_thd, &len);

	str = dict_strip_comments(ptr, len);

	ptr = str;

	ut_ad(dict_sys.locked());
loop:
	ptr = dict_scan_to(ptr, "DROP");

	if (*ptr == '\0') {
		ut_free(str);

		return(DB_SUCCESS);
	}

	ptr = dict_accept(cs, ptr, "DROP", &success);

	if (!my_isspace(cs, *ptr)) {

		goto loop;
	}

	ptr = dict_accept(cs, ptr, "FOREIGN", &success);

	if (!success || !my_isspace(cs, *ptr)) {

		goto loop;
	}

	ptr = dict_accept(cs, ptr, "KEY", &success);

	if (!success) {
syntax_error:
		if (!srv_read_only_mode) {
			FILE*	ef = dict_foreign_err_file;

			mysql_mutex_lock(&dict_foreign_err_mutex);
			rewind(ef);
			ut_print_timestamp(ef);
                        fputs(" Syntax error in dropping of a"
			      " foreign key constraint of table ", ef);
			ut_print_name(ef, NULL, table->name.m_name);
			fprintf(ef, ",\n"
				"close to:\n%s\n in SQL command\n%s\n",
				ptr, str);
			mysql_mutex_unlock(&dict_foreign_err_mutex);
		}

		ut_free(str);

		return DB_CANNOT_DROP_CONSTRAINT;
	}

	ptr1 = dict_accept(cs, ptr, "IF", &success);

	if (success && my_isspace(cs, *ptr1)) {
		ptr1 = dict_accept(cs, ptr1, "EXISTS", &success);
		if (success) {
			ptr = ptr1;
			if_exists = true;
		}
	}

	ptr = dict_scan_id(cs, ptr, heap, &id);

	if (!id) {
		goto syntax_error;
	}

	const Lex_ident_column i{Lex_cstring_strlen(id)};

	if (std::find_if(table->foreign_set.begin(), table->foreign_set.end(),
			 [&i](const dict_foreign_t *fk)
			 {return i.streq(Lex_cstring_strlen(fk->sql_id()));})
	    == table->foreign_set.end()) {
		if (if_exists) {
			goto loop;
		}

		if (!srv_read_only_mode) {
			FILE*	ef = dict_foreign_err_file;

			mysql_mutex_lock(&dict_foreign_err_mutex);
			rewind(ef);
			ut_print_timestamp(ef);
			fputs(" Error in dropping of a foreign key"
			      " constraint of table ", ef);
			ut_print_name(ef, NULL, table->name.m_name);
			fprintf(ef, ",\nin SQL command\n%s"
				"\nCannot find a constraint with the"
				" given id %s.\n", str, id);
			mysql_mutex_unlock(&dict_foreign_err_mutex);
		}

		ut_free(str);

		return(DB_CANNOT_DROP_CONSTRAINT);
	}

	ut_a(*n < 1000);
	(*constraints_to_drop)[*n] = id;
	(*n)++;
	goto loop;
}

/*==================== END OF FOREIGN KEY PROCESSING ====================*/

/**********************************************************************//**
Returns an index object if it is found in the dictionary cache.
Assumes that dict_sys.latch is already being held.
@return index, NULL if not found */
dict_index_t*
dict_index_get_if_in_cache_low(
/*===========================*/
	index_id_t	index_id)	/*!< in: index id */
{
  ut_ad(dict_sys.frozen());

  for (dict_table_t *table= UT_LIST_GET_FIRST(dict_sys.table_LRU);
       table; table= UT_LIST_GET_NEXT(table_LRU, table))
    if (dict_index_t *index= dict_table_find_index_on_id(table, index_id))
      return index;

  for (dict_table_t *table = UT_LIST_GET_FIRST(dict_sys.table_non_LRU);
       table; table= UT_LIST_GET_NEXT(table_LRU, table))
    if (dict_index_t *index= dict_table_find_index_on_id(table, index_id))
      return index;

  return nullptr;
}

#ifdef UNIV_DEBUG
/**********************************************************************//**
Returns an index object if it is found in the dictionary cache.
@return index, NULL if not found */
dict_index_t*
dict_index_get_if_in_cache(
/*=======================*/
	index_id_t	index_id)	/*!< in: index id */
{
	dict_index_t*	index;

	if (!dict_sys.is_initialised()) {
		return(NULL);
	}

	dict_sys.freeze(SRW_LOCK_CALL);

	index = dict_index_get_if_in_cache_low(index_id);

	dict_sys.unfreeze();

	return(index);
}

/**********************************************************************//**
Checks that a tuple has n_fields_cmp value in a sensible range, so that
no comparison can occur with the page number field in a node pointer.
@return TRUE if ok */
ibool
dict_index_check_search_tuple(
/*==========================*/
	const dict_index_t*	index,	/*!< in: index tree */
	const dtuple_t*		tuple)	/*!< in: tuple used in a search */
{
	ut_ad(dtuple_get_n_fields_cmp(tuple)
	      <= dict_index_get_n_unique_in_tree(index));
	return(TRUE);
}
#endif /* UNIV_DEBUG */

/**********************************************************************//**
Builds a node pointer out of a physical record and a page number.
@return own: node pointer */
dtuple_t*
dict_index_build_node_ptr(
/*======================*/
	const dict_index_t*	index,	/*!< in: index */
	const rec_t*		rec,	/*!< in: record for which to build node
					pointer */
	ulint			page_no,/*!< in: page number to put in node
					pointer */
	mem_heap_t*		heap,	/*!< in: memory heap where pointer
					created */
	ulint			level)	/*!< in: level of rec in tree:
					0 means leaf level */
{
	dtuple_t*	tuple;
	dfield_t*	field;
	byte*		buf;
	uint16_t n_unique = dict_index_get_n_unique_in_tree_nonleaf(index);

	tuple = dtuple_create(heap, n_unique + 1);

	/* When searching in the tree for the node pointer, we must not do
	comparison on the last field, the page number field, as on upper
	levels in the tree there may be identical node pointers with a
	different page number; therefore, we set the n_fields_cmp to one
	less: */

	dtuple_set_n_fields_cmp(tuple, n_unique);

	dict_index_copy_types(tuple, index, n_unique);

	buf = static_cast<byte*>(mem_heap_alloc(heap, 4));

	mach_write_to_4(buf, page_no);

	field = dtuple_get_nth_field(tuple, n_unique);
	dfield_set_data(field, buf, 4);

	dtype_set(dfield_get_type(field), DATA_SYS_CHILD, DATA_NOT_NULL, 4);

	rec_copy_prefix_to_dtuple(tuple, rec, index,
				  level ? 0 : index->n_core_fields,
				  n_unique, heap);
	dtuple_set_info_bits(tuple, dtuple_get_info_bits(tuple)
			     | REC_STATUS_NODE_PTR);

	ut_ad(dtuple_check_typed(tuple));

	return(tuple);
}

/** Convert a physical record into a search tuple.
@param[in]	rec		index record (not necessarily in an index page)
@param[in]	index		index
@param[in]	leaf		whether rec is in a leaf page
@param[in]	n_fields	number of data fields
@param[in,out]	heap		memory heap for allocation
@return own: data tuple */
dtuple_t*
dict_index_build_data_tuple(
	const rec_t*		rec,
	const dict_index_t*	index,
	bool			leaf,
	ulint			n_fields,
	mem_heap_t*		heap)
{
	ut_ad(!index->is_clust());

	dtuple_t* tuple = dtuple_create(heap, uint16_t(n_fields));

	dict_index_copy_types(tuple, index, n_fields);

	rec_copy_prefix_to_dtuple(tuple, rec, index,
				  leaf ? n_fields : 0, n_fields, heap);

	ut_ad(dtuple_check_typed(tuple));

	return(tuple);
}

/*********************************************************************//**
Calculates the minimum record length in an index. */
ulint
dict_index_calc_min_rec_len(
/*========================*/
	const dict_index_t*	index)	/*!< in: index */
{
	ulint	sum	= 0;
	ulint	i;
	ulint	comp	= dict_table_is_comp(index->table);

	if (comp) {
		ulint nullable = 0;
		sum = REC_N_NEW_EXTRA_BYTES;
		for (i = 0; i < dict_index_get_n_fields(index); i++) {
			const dict_col_t*	col
				= dict_index_get_nth_col(index, i);
			ulint	size = dict_col_get_fixed_size(col, comp);
			sum += size;
			if (!size) {
				size = col->len;
				sum += size < 128 ? 1 : 2;
			}
			if (!(col->prtype & DATA_NOT_NULL)) {
				nullable++;
			}
		}

		/* round the NULL flags up to full bytes */
		sum += UT_BITS_IN_BYTES(nullable);

		return(sum);
	}

	for (i = 0; i < dict_index_get_n_fields(index); i++) {
		sum += dict_col_get_fixed_size(
			dict_index_get_nth_col(index, i), comp);
	}

	if (sum > 127) {
		sum += 2 * dict_index_get_n_fields(index);
	} else {
		sum += dict_index_get_n_fields(index);
	}

	sum += REC_N_OLD_EXTRA_BYTES;

	return(sum);
}

std::string
dict_print_info_on_foreign_key_in_create_format(const trx_t *trx,
                                                const dict_foreign_t *foreign,
                                                bool add_newline)
{
	const char*	id = foreign->sql_id();
	ulint	i;
	std::string	str;

	str.append(",");

	if (add_newline) {
		/* SHOW CREATE TABLE wants constraints each printed nicely
		on its own line, while error messages want no newlines
		inserted. */
		str.append("\n ");
	}

	str.append(" CONSTRAINT ");

	str.append(innobase_quote_identifier(trx, id));
	str.append(" FOREIGN KEY (");

	for (i = 0;;) {
		str.append(innobase_quote_identifier(trx, foreign->foreign_col_names[i]));

		if (++i < foreign->n_fields) {
			str.append(", ");
		} else {
			break;
		}
	}

	str.append(") REFERENCES ");

	if (dict_tables_have_same_db(foreign->foreign_table_name_lookup,
				     foreign->referenced_table_name_lookup)) {
		/* Do not print the database name of the referenced table */
		str.append(ut_get_name(trx,
			      dict_remove_db_name(
				      foreign->referenced_table_name)));
	} else {
		str.append(ut_get_name(trx,
				foreign->referenced_table_name));
	}

	str.append(" (");

	for (i = 0;;) {
		str.append(innobase_quote_identifier(trx,
				foreign->referenced_col_names[i]));

		if (++i < foreign->n_fields) {
			str.append(", ");
		} else {
			break;
		}
	}

	str.append(")");

	if (foreign->type & foreign->DELETE_CASCADE) {
		str.append(" ON DELETE CASCADE");
	}

	if (foreign->type & foreign->DELETE_SET_NULL) {
		str.append(" ON DELETE SET NULL");
	}

	if (foreign->type & foreign->DELETE_NO_ACTION) {
		str.append(" ON DELETE NO ACTION");
	}

	if (foreign->type & foreign->UPDATE_CASCADE) {
		str.append(" ON UPDATE CASCADE");
	}

	if (foreign->type & foreign->UPDATE_SET_NULL) {
		str.append(" ON UPDATE SET NULL");
	}

	if (foreign->type & foreign->UPDATE_NO_ACTION) {
		str.append(" ON UPDATE NO ACTION");
	}

	return str;
}

/**********************************************************************//**
Outputs info on foreign keys of a table. */
std::string
dict_print_info_on_foreign_keys(
/*============================*/
	ibool		create_table_format, /*!< in: if TRUE then print in
				a format suitable to be inserted into
				a CREATE TABLE, otherwise in the format
				of SHOW TABLE STATUS */
	trx_t*		trx,	/*!< in: transaction */
	dict_table_t*	table)	/*!< in: table */
{
	dict_foreign_t*	foreign;
	std::string 	str;

	dict_sys.freeze(SRW_LOCK_CALL);

	for (dict_foreign_set::iterator it = table->foreign_set.begin();
	     it != table->foreign_set.end();
	     ++it) {

		foreign = *it;

		if (create_table_format) {
			str.append(
				dict_print_info_on_foreign_key_in_create_format(
					trx, foreign, TRUE));
		} else {
			ulint	i;
			str.append("; (");

			for (i = 0; i < foreign->n_fields; i++) {
				if (i) {
					str.append(" ");
				}

				str.append(innobase_quote_identifier(trx,
						foreign->foreign_col_names[i]));
			}

			str.append(") REFER ");
			str.append(ut_get_name(trx,
					foreign->referenced_table_name));
			str.append(")");

			for (i = 0; i < foreign->n_fields; i++) {
				if (i) {
					str.append(" ");
				}
				str.append(innobase_quote_identifier(
						trx,
						foreign->referenced_col_names[i]));
			}

			str.append(")");

			if (foreign->type == foreign->DELETE_CASCADE) {
				str.append(" ON DELETE CASCADE");
			}

			if (foreign->type == foreign->DELETE_SET_NULL) {
				str.append(" ON DELETE SET NULL");
			}

			if (foreign->type & foreign->DELETE_NO_ACTION) {
				str.append(" ON DELETE NO ACTION");
			}

			if (foreign->type & foreign->UPDATE_CASCADE) {
				str.append(" ON UPDATE CASCADE");
			}

			if (foreign->type & foreign->UPDATE_SET_NULL) {
				str.append(" ON UPDATE SET NULL");
			}

			if (foreign->type & foreign->UPDATE_NO_ACTION) {
				str.append(" ON UPDATE NO ACTION");
			}
		}
	}

	dict_sys.unfreeze();
	return str;
}

/**********************************************************************//**
Flags an index corrupted both in the data dictionary cache
and in the SYS_INDEXES */
void dict_set_corrupted(dict_index_t *index, const char *ctx)
{
	mem_heap_t*	heap;
	mtr_t		mtr;
	dict_index_t*	sys_index;
	dtuple_t*	tuple;
	dfield_t*	dfield;
	byte*		buf;
	const char*	status;
	btr_cur_t	cursor;

	dict_sys.lock(SRW_LOCK_CALL);

	ut_ad(!dict_table_is_comp(dict_sys.sys_tables));
	ut_ad(!dict_table_is_comp(dict_sys.sys_indexes));

	/* Mark the table as corrupted only if the clustered index
	is corrupted */
	if (dict_index_is_clust(index)) {
		index->table->corrupted = TRUE;
		goto func_exit;
	}

	if (index->type & DICT_CORRUPT) {
		/* The index was already flagged corrupted. */
		ut_ad(!dict_index_is_clust(index) || index->table->corrupted);
		goto func_exit;
	}

	/* If this is read only mode, do not update SYS_INDEXES, just
	mark it as corrupted in memory */
	if (high_level_read_only) {
		index->type |= DICT_CORRUPT;
		goto func_exit;
	}

	heap = mem_heap_create(sizeof(dtuple_t) + 2 * (sizeof(dfield_t)
			       + sizeof(que_fork_t) + sizeof(upd_node_t)
			       + sizeof(upd_t) + 12));
	mtr_start(&mtr);
	index->type |= DICT_CORRUPT;

	sys_index = UT_LIST_GET_FIRST(dict_sys.sys_indexes->indexes);

	/* Find the index row in SYS_INDEXES */
	tuple = dtuple_create(heap, 2);

	dfield = dtuple_get_nth_field(tuple, 0);
	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, index->table->id);
	dfield_set_data(dfield, buf, 8);

	dfield = dtuple_get_nth_field(tuple, 1);
	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, index->id);
	dfield_set_data(dfield, buf, 8);

	dict_index_copy_types(tuple, sys_index, 2);
	cursor.page_cur.index = sys_index;

	if (cursor.search_leaf(tuple, PAGE_CUR_LE, BTR_MODIFY_LEAF, &mtr)
	    != DB_SUCCESS) {
		goto fail;
	}

	if (cursor.low_match == dtuple_get_n_fields(tuple)) {
		/* UPDATE SYS_INDEXES SET TYPE=index->type
		WHERE TABLE_ID=index->table->id AND INDEX_ID=index->id */
		ulint	len;
		byte*	field	= rec_get_nth_field_old(
			btr_cur_get_rec(&cursor),
			DICT_FLD__SYS_INDEXES__TYPE, &len);
		if (len != 4) {
			goto fail;
		}
		mtr.write<4>(*btr_cur_get_block(&cursor), field, index->type);
		status = "Flagged";
	} else {
fail:
		status = "Unable to flag";
	}

	mtr_commit(&mtr);
	mem_heap_free(heap);
	ib::error() << status << " corruption of " << index->name
		<< " in table " << index->table->name << " in " << ctx;

func_exit:
	dict_sys.unlock();
}

/** Sets merge_threshold in the SYS_INDEXES
@param[in,out]	index		index
@param[in]	merge_threshold	value to set */
void
dict_index_set_merge_threshold(
	dict_index_t*	index,
	ulint		merge_threshold)
{
	mem_heap_t*	heap;
	mtr_t		mtr;
	dict_index_t*	sys_index;
	dtuple_t*	tuple;
	dfield_t*	dfield;
	byte*		buf;
	btr_cur_t	cursor;

	ut_ad(index != NULL);
	ut_ad(!dict_table_is_comp(dict_sys.sys_tables));
	ut_ad(!dict_table_is_comp(dict_sys.sys_indexes));

	heap = mem_heap_create(sizeof(dtuple_t) + 2 * (sizeof(dfield_t)
			       + sizeof(que_fork_t) + sizeof(upd_node_t)
			       + sizeof(upd_t) + 12));

	mtr.start();

	sys_index = UT_LIST_GET_FIRST(dict_sys.sys_indexes->indexes);

	/* Find the index row in SYS_INDEXES */
	tuple = dtuple_create(heap, 2);

	dfield = dtuple_get_nth_field(tuple, 0);
	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, index->table->id);
	dfield_set_data(dfield, buf, 8);

	dfield = dtuple_get_nth_field(tuple, 1);
	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, index->id);
	dfield_set_data(dfield, buf, 8);

	dict_index_copy_types(tuple, sys_index, 2);
	cursor.page_cur.index = sys_index;

	if (cursor.search_leaf(tuple, PAGE_CUR_GE, BTR_MODIFY_LEAF, &mtr)
	    != DB_SUCCESS) {
		goto func_exit;
	}

	if (cursor.up_match == dtuple_get_n_fields(tuple)
	    && rec_get_n_fields_old(btr_cur_get_rec(&cursor))
	       == DICT_NUM_FIELDS__SYS_INDEXES) {
		ulint	len;
		byte*	field	= rec_get_nth_field_old(
			btr_cur_get_rec(&cursor),
			DICT_FLD__SYS_INDEXES__MERGE_THRESHOLD, &len);

		ut_ad(len == 4);
		mtr.write<4,mtr_t::MAYBE_NOP>(*btr_cur_get_block(&cursor),
					      field, merge_threshold);
	}

func_exit:
	mtr_commit(&mtr);
	mem_heap_free(heap);
}

#ifdef UNIV_DEBUG
/** Sets merge_threshold for all indexes in the list of tables
@param[in]	list	pointer to the list of tables */
inline
void
dict_set_merge_threshold_list_debug(
	UT_LIST_BASE_NODE_T(dict_table_t)*	list,
	uint					merge_threshold_all)
{
	for (dict_table_t* table = UT_LIST_GET_FIRST(*list);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {
		for (dict_index_t* index = UT_LIST_GET_FIRST(table->indexes);
		     index != NULL;
		     index = UT_LIST_GET_NEXT(indexes, index)) {
			index->lock.x_lock(SRW_LOCK_CALL);
			index->merge_threshold = merge_threshold_all
				& ((1U << 6) - 1);
			index->lock.x_unlock();
		}
	}
}

/** Sets merge_threshold for all indexes in dictionary cache for debug.
@param[in]	merge_threshold_all	value to set for all indexes */
void
dict_set_merge_threshold_all_debug(
	uint	merge_threshold_all)
{
	dict_sys.freeze(SRW_LOCK_CALL);

	dict_set_merge_threshold_list_debug(
		&dict_sys.table_LRU, merge_threshold_all);
	dict_set_merge_threshold_list_debug(
		&dict_sys.table_non_LRU, merge_threshold_all);

	dict_sys.unfreeze();
}

#endif /* UNIV_DEBUG */

/** Get an index by name.
@param[in]	table		the table where to look for the index
@param[in]	name		the index name to look for
@return index, NULL if does not exist */
dict_index_t*
dict_table_get_index_on_name(dict_table_t* table, const char* name)
{
	dict_index_t*	index;

	index = dict_table_get_first_index(table);

	while (index != NULL) {
		if (index->is_committed() && !strcmp(index->name, name)) {
			return(index);
		}

		index = dict_table_get_next_index(index);
	}

	return(NULL);
}

/**********************************************************************//**
Replace the index passed in with another equivalent index in the
foreign key lists of the table.
@return whether all replacements were found */
bool
dict_foreign_replace_index(
/*=======================*/
	dict_table_t*		table,  /*!< in/out: table */
	const char**		col_names,
					/*!< in: column names, or NULL
					to use table->col_names */
	const dict_index_t*	index)	/*!< in: index to be replaced */
{
	bool		found	= true;
	dict_foreign_t*	foreign;

	ut_ad(index->to_be_dropped);
	ut_ad(index->table == table);

	for (dict_foreign_set::iterator it = table->foreign_set.begin();
	     it != table->foreign_set.end();
	     ++it) {

		foreign = *it;
		if (foreign->foreign_index == index) {
			ut_ad(foreign->foreign_table == index->table);

			dict_index_t* new_index = dict_foreign_find_index(
				foreign->foreign_table, col_names,
				foreign->foreign_col_names,
				foreign->n_fields, index,
				/*check_charsets=*/TRUE, /*check_null=*/FALSE,
				NULL, NULL, NULL);
			if (new_index) {
				ut_ad(new_index->table == index->table);
				ut_ad(!new_index->to_be_dropped);
			} else {
				found = false;
			}

			foreign->foreign_index = new_index;
		}
	}

	for (dict_foreign_set::iterator it = table->referenced_set.begin();
	     it != table->referenced_set.end();
	     ++it) {

		foreign = *it;
		if (foreign->referenced_index == index) {
			ut_ad(foreign->referenced_table == index->table);

			dict_index_t* new_index = dict_foreign_find_index(
				foreign->referenced_table, NULL,
				foreign->referenced_col_names,
				foreign->n_fields, index,
				/*check_charsets=*/TRUE, /*check_null=*/FALSE,
				NULL, NULL, NULL);
			/* There must exist an alternative index,
			since this must have been checked earlier. */
			if (new_index) {
				ut_ad(new_index->table == index->table);
				ut_ad(!new_index->to_be_dropped);
			} else {
				found = false;
			}

			foreign->referenced_index = new_index;
		}
	}

	return(found);
}

#ifdef UNIV_DEBUG
/**********************************************************************//**
Check for duplicate index entries in a table [using the index name] */
void
dict_table_check_for_dup_indexes(
/*=============================*/
	const dict_table_t*	table,	/*!< in: Check for dup indexes
					in this table */
	enum check_name		check)	/*!< in: whether and when to allow
					temporary index names */
{
	/* Check for duplicates, ignoring indexes that are marked
	as to be dropped */

	const dict_index_t*	index1;
	const dict_index_t*	index2;

	ut_ad(dict_sys.frozen());

	/* The primary index _must_ exist */
	ut_a(UT_LIST_GET_LEN(table->indexes) > 0);

	index1 = UT_LIST_GET_FIRST(table->indexes);

	do {
		if (!index1->is_committed()) {
			ut_a(!dict_index_is_clust(index1));

			switch (check) {
			case CHECK_ALL_COMPLETE:
				ut_error;
			case CHECK_ABORTED_OK:
				switch (dict_index_get_online_status(index1)) {
				case ONLINE_INDEX_COMPLETE:
				case ONLINE_INDEX_CREATION:
					ut_error;
					break;
				case ONLINE_INDEX_ABORTED:
				case ONLINE_INDEX_ABORTED_DROPPED:
					break;
				}
				/* fall through */
			case CHECK_PARTIAL_OK:
				break;
			}
		}

		for (index2 = UT_LIST_GET_NEXT(indexes, index1);
		     index2 != NULL;
		     index2 = UT_LIST_GET_NEXT(indexes, index2)) {
			ut_ad(index1->is_committed()
			      != index2->is_committed()
			      || strcmp(index1->name, index2->name) != 0);
		}

		index1 = UT_LIST_GET_NEXT(indexes, index1);
	} while (index1);
}
#endif /* UNIV_DEBUG */

/*********************************************************************//**
Converts a database and table name from filesystem encoding
(e.g. d@i1b/a@q1b@1Kc, same format as used in dict_table_t::name) in two
strings in UTF8 encoding (e.g. dцb and aюbØc). The output buffers must be
at least MAX_DB_UTF8_LEN and MAX_TABLE_UTF8_LEN bytes. */
void
dict_fs2utf8(
/*=========*/
	const char*	db_and_table,	/*!< in: database and table names,
					e.g. d@i1b/a@q1b@1Kc */
	char*		db_utf8,	/*!< out: database name, e.g. dцb */
	size_t		db_utf8_size,	/*!< in: dbname_utf8 size */
	char*		table_utf8,	/*!< out: table name, e.g. aюbØc */
	size_t		table_utf8_size)/*!< in: table_utf8 size */
{
	char	db[MAX_DATABASE_NAME_LEN + 1];
	ulint	db_len;
	uint	errors;

	db_len = dict_get_db_name_len(db_and_table);

	ut_a(db_len <= sizeof(db));

	memcpy(db, db_and_table, db_len);
	db[db_len] = '\0';

	strconvert(
		&my_charset_filename, db, uint(db_len), system_charset_info,
		db_utf8, uint(db_utf8_size), &errors);

	/* convert each # to @0023 in table name and store the result in buf */
	const char*	table = dict_remove_db_name(db_and_table);
	const char*	table_p;
	char		buf[MAX_TABLE_NAME_LEN * 5 + 1];
	char*		buf_p;
	for (table_p = table, buf_p = buf; table_p[0] != '\0'; table_p++) {
		if (table_p[0] != '#') {
			buf_p[0] = table_p[0];
			buf_p++;
		} else {
			buf_p[0] = '@';
			buf_p[1] = '0';
			buf_p[2] = '0';
			buf_p[3] = '2';
			buf_p[4] = '3';
			buf_p += 5;
		}
		ut_a((size_t) (buf_p - buf) < sizeof(buf));
	}
	buf_p[0] = '\0';

	errors = 0;
	strconvert(
		&my_charset_filename, buf, (uint) (buf_p - buf),
		system_charset_info,
		table_utf8, uint(table_utf8_size),
		&errors);

	if (errors != 0) {
		snprintf(table_utf8, table_utf8_size, "%s%s",
			    srv_mysql50_table_name_prefix, table);
	}
}

/** Insert a table into the hash tables
@param table   the table
@param id_hash dict_sys.table_id_hash or dict_sys.temp_id_hash */
static void hash_insert(dict_table_t *table, hash_table_t& id_hash) noexcept
{
  ut_ad(table->cached);
  dict_sys.table_hash.cell_get(my_crc32c(0, table->name.m_name,
                                         strlen(table->name.m_name)))->
    append(*table, &dict_table_t::name_hash);
  id_hash.cell_get(ut_fold_ull(table->id))->append(*table,
                                                   &dict_table_t::id_hash);
}

/** Resize the hash tables based on the current buffer pool size. */
void dict_sys_t::resize() noexcept
{
  ut_ad(this == &dict_sys);
  ut_ad(is_initialised());
  lock(SRW_LOCK_CALL);

  /* all table entries are in table_LRU and table_non_LRU lists */
  table_hash.free();
  table_id_hash.free();
  temp_id_hash.free();

  const ulint hash_size = buf_pool.curr_pool_size()
    / (DICT_POOL_PER_TABLE_HASH * UNIV_WORD_SIZE);
  table_hash.create(hash_size);
  table_id_hash.create(hash_size);
  temp_id_hash.create(hash_size);

  for (dict_table_t *table= UT_LIST_GET_FIRST(table_LRU); table;
       table= UT_LIST_GET_NEXT(table_LRU, table))
  {
    ut_ad(!table->is_temporary());
    hash_insert(table, table_id_hash);
  }

  for (dict_table_t *table = UT_LIST_GET_FIRST(table_non_LRU); table;
       table= UT_LIST_GET_NEXT(table_LRU, table))
    hash_insert(table, table->is_temporary() ? temp_id_hash : table_id_hash);

  unlock();
}

/** Close the data dictionary cache on shutdown. */
void dict_sys_t::close() noexcept
{
  ut_ad(this == &dict_sys);
  if (!is_initialised()) return;

  lock(SRW_LOCK_CALL);

  /* Free the hash elements. We don't remove them from table_hash
  because we are invoking table_hash.free() below. */
  for (ulint i= table_hash.n_cells; i--; )
    while (auto table= static_cast<dict_table_t*>(table_hash.array[i].node))
      dict_sys.remove(table);

  table_hash.free();

  /* table_id_hash contains the same elements as in table_hash,
  therefore we don't delete the individual elements. */
  table_id_hash.free();

  /* No temporary tables should exist at this point. */
  temp_id_hash.free();

  unlock();
  latch.destroy();

  mysql_mutex_destroy(&dict_foreign_err_mutex);

  if (dict_foreign_err_file)
  {
    my_fclose(dict_foreign_err_file, MYF(MY_WME));
    dict_foreign_err_file = NULL;
  }

  m_initialised= false;
}

#ifdef UNIV_DEBUG
/**********************************************************************//**
Validate the dictionary table LRU list.
@return TRUE if valid */
static
ibool
dict_lru_validate(void)
/*===================*/
{
	dict_table_t*	table;

	ut_ad(dict_sys.frozen());

	for (table = UT_LIST_GET_FIRST(dict_sys.table_LRU);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		ut_a(table->can_be_evicted);
	}

	for (table = UT_LIST_GET_FIRST(dict_sys.table_non_LRU);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		ut_a(!table->can_be_evicted);
	}

	return(TRUE);
}
#endif /* UNIV_DEBUG */
/*********************************************************************//**
Check an index to see whether its first fields are the columns in the array,
in the same order and is not marked for deletion and is not the same
as types_idx.
@return true if the index qualifies, otherwise false */
bool
dict_foreign_qualify_index(
/*=======================*/
	const dict_table_t*	table,	/*!< in: table */
	const char**		col_names,
					/*!< in: column names, or NULL
					to use table->col_names */
	const char**		columns,/*!< in: array of column names */
	ulint			n_cols,	/*!< in: number of columns */
	const dict_index_t*	index,	/*!< in: index to check */
	const dict_index_t*	types_idx,
					/*!< in: NULL or an index
					whose types the column types
					must match */
	bool			check_charsets,
					/*!< in: whether to check
					charsets.  only has an effect
					if types_idx != NULL */
	ulint			check_null,
					/*!< in: nonzero if none of
					the columns must be declared
					NOT NULL */
	fkerr_t*		error,	/*!< out: error code */
	ulint*			err_col_no,
					/*!< out: column number where
					error happened */
	dict_index_t**		err_index)
					/*!< out: index where error
					happened */
{
	if (dict_index_get_n_fields(index) < n_cols) {
		return(false);
	}

	if (!index->is_btree()) {
		return false;
	}

	if (index->online_status >= ONLINE_INDEX_ABORTED) {
		return false;
	}

	for (ulint i = 0; i < n_cols; i++) {
		const dict_field_t * const field(dict_index_get_nth_field(index, i));
		const Lex_ident_column field_name= Lex_cstring_strlen(field->name);
		const ulint col_no = dict_col_get_no(field->col);
		Lex_ident_column col_name;

		if (field->prefix_len != 0) {
			/* We do not accept column prefix
			indexes here */
			if (error && err_col_no && err_index) {
				*error = FK_IS_PREFIX_INDEX;
				*err_col_no = i;
				*err_index = (dict_index_t*)index;
			}
			return(false);
		}

		if (check_null
		    && (field->col->prtype & DATA_NOT_NULL)) {
			if (error && err_col_no && err_index) {
				*error = FK_COL_NOT_NULL;
				*err_col_no = i;
				*err_index = (dict_index_t*)index;
			}
			return(false);
		}

		if (field->col->is_virtual()) {
			col_name = ""_Lex_ident_column;
			for (ulint j = 0; j < table->n_v_def; j++) {
				col_name = dict_table_get_v_col_name(table, j);
				if (field_name.streq(col_name)) {
					break;
				}
			}
		} else {
			col_name = col_names
				? Lex_ident_column(Lex_cstring_strlen(col_names[col_no]))
				: dict_table_get_col_name(table, col_no);
		}

		if (!col_name.streq(Lex_cstring_strlen(columns[i]))) {
			return(false);
		}

		if (types_idx && !cmp_cols_are_equal(
			    dict_index_get_nth_col(index, i),
			    dict_index_get_nth_col(types_idx, i),
			    check_charsets)) {
			if (error && err_col_no && err_index) {
				*error = FK_COLS_NOT_EQUAL;
				*err_col_no = i;
				*err_index = (dict_index_t*)index;
			}

			return(false);
		}
	}

	return(true);
}

/*********************************************************************//**
Update the state of compression failure padding heuristics. This is
called whenever a compression operation succeeds or fails.
The caller must be holding info->mutex */
static
void
dict_index_zip_pad_update(
/*======================*/
	zip_pad_info_t*	info,	/*<! in/out: info to be updated */
	ulint	zip_threshold)	/*<! in: zip threshold value */
{
	ulint	total;
	ulint	fail_pct;

	ut_ad(info);
	ut_ad(info->pad % ZIP_PAD_INCR == 0);

	total = info->success + info->failure;

	ut_ad(total > 0);

	if (zip_threshold == 0) {
		/* User has just disabled the padding. */
		return;
	}

	if (total < ZIP_PAD_ROUND_LEN) {
		/* We are in middle of a round. Do nothing. */
		return;
	}

	/* We are at a 'round' boundary. Reset the values but first
	calculate fail rate for our heuristic. */
	fail_pct = (info->failure * 100) / total;
	info->failure = 0;
	info->success = 0;

	if (fail_pct > zip_threshold) {
		/* Compression failures are more then user defined
		threshold. Increase the pad size to reduce chances of
		compression failures.

		Only do increment if it won't increase padding
		beyond max pad size. */
		if (info->pad + ZIP_PAD_INCR
		    < (srv_page_size * zip_pad_max) / 100) {
			info->pad.fetch_add(ZIP_PAD_INCR);

			MONITOR_INC(MONITOR_PAD_INCREMENTS);
		}

		info->n_rounds = 0;

	} else {
		/* Failure rate was OK. Another successful round
		completed. */
		++info->n_rounds;

		/* If enough successful rounds are completed with
		compression failure rate in control, decrease the
		padding. */
		if (info->n_rounds >= ZIP_PAD_SUCCESSFUL_ROUND_LIMIT
		    && info->pad > 0) {
			info->pad.fetch_sub(ZIP_PAD_INCR);

			info->n_rounds = 0;

			MONITOR_INC(MONITOR_PAD_DECREMENTS);
		}
	}
}

/*********************************************************************//**
This function should be called whenever a page is successfully
compressed. Updates the compression padding information. */
void
dict_index_zip_success(
/*===================*/
	dict_index_t*	index)	/*!< in/out: index to be updated. */
{
	ulint zip_threshold = zip_failure_threshold_pct;
	if (!zip_threshold) {
		/* Disabled by user. */
		return;
	}

	index->zip_pad.mutex.lock();
	++index->zip_pad.success;
	dict_index_zip_pad_update(&index->zip_pad, zip_threshold);
	index->zip_pad.mutex.unlock();
}

/*********************************************************************//**
This function should be called whenever a page compression attempt
fails. Updates the compression padding information. */
void
dict_index_zip_failure(
/*===================*/
	dict_index_t*	index)	/*!< in/out: index to be updated. */
{
	ulint zip_threshold = zip_failure_threshold_pct;
	if (!zip_threshold) {
		/* Disabled by user. */
		return;
	}

	index->zip_pad.mutex.lock();
	++index->zip_pad.failure;
	dict_index_zip_pad_update(&index->zip_pad, zip_threshold);
	index->zip_pad.mutex.unlock();
}

/*********************************************************************//**
Return the optimal page size, for which page will likely compress.
@return page size beyond which page might not compress */
ulint
dict_index_zip_pad_optimal_page_size(
/*=================================*/
	dict_index_t*	index)	/*!< in: index for which page size
				is requested */
{
	ulint	pad;
	ulint	min_sz;
	ulint	sz;

	if (!zip_failure_threshold_pct) {
		/* Disabled by user. */
		return(srv_page_size);
	}

	pad = index->zip_pad.pad;

	ut_ad(pad < srv_page_size);
	sz = srv_page_size - pad;

	/* Min size allowed by user. */
	ut_ad(zip_pad_max < 100);
	min_sz = (srv_page_size * (100 - zip_pad_max)) / 100;

	return(ut_max(sz, min_sz));
}

/*************************************************************//**
Convert table flag to row format string.
@return row format name. */
const char*
dict_tf_to_row_format_string(
/*=========================*/
	ulint	table_flag)		/*!< in: row format setting */
{
	switch (dict_tf_get_rec_format(table_flag)) {
	case REC_FORMAT_REDUNDANT:
		return("ROW_TYPE_REDUNDANT");
	case REC_FORMAT_COMPACT:
		return("ROW_TYPE_COMPACT");
	case REC_FORMAT_COMPRESSED:
		return("ROW_TYPE_COMPRESSED");
	case REC_FORMAT_DYNAMIC:
		return("ROW_TYPE_DYNAMIC");
	}

	ut_error;
	return(0);
}
