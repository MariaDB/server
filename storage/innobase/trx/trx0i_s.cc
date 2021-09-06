/*****************************************************************************

Copyright (c) 2007, 2015, Oracle and/or its affiliates. All Rights Reserved.
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
@file trx/trx0i_s.cc
INFORMATION SCHEMA innodb_trx, innodb_locks and
innodb_lock_waits tables fetch code.

The code below fetches information needed to fill those
3 dynamic tables and uploads it into a "transactions
table cache" for later retrieval.

Created July 17, 2007 Vasil Dimov
*******************************************************/

#include "trx0i_s.h"
#include "buf0buf.h"
#include "dict0dict.h"
#include "ha0storage.h"
#include "hash0hash.h"
#include "lock0iter.h"
#include "lock0lock.h"
#include "mem0mem.h"
#include "page0page.h"
#include "rem0rec.h"
#include "row0row.h"
#include "srv0srv.h"
#include "trx0sys.h"
#include "que0que.h"
#include "trx0purge.h"
#include "sql_class.h"

/** Initial number of rows in the table cache */
#define TABLE_CACHE_INITIAL_ROWSNUM	1024

/** @brief The maximum number of chunks to allocate for a table cache.

The rows of a table cache are stored in a set of chunks. When a new
row is added a new chunk is allocated if necessary. Assuming that the
first one is 1024 rows (TABLE_CACHE_INITIAL_ROWSNUM) and each
subsequent is N/2 where N is the number of rows we have allocated till
now, then 39th chunk would accommodate 1677416425 rows and all chunks
would accommodate 3354832851 rows. */
#define MEM_CHUNKS_IN_TABLE_CACHE	39

/** The following are some testing auxiliary macros. Do not enable them
in a production environment. */
/* @{ */

#if 0
/** If this is enabled then lock folds will always be different
resulting in equal rows being put in a different cells of the hash
table. Checking for duplicates will be flawed because different
fold will be calculated when a row is searched in the hash table. */
#define TEST_LOCK_FOLD_ALWAYS_DIFFERENT
#endif

#if 0
/** This effectively kills the search-for-duplicate-before-adding-a-row
function, but searching in the hash is still performed. It will always
be assumed that lock is not present and insertion will be performed in
the hash table. */
#define TEST_NO_LOCKS_ROW_IS_EVER_EQUAL_TO_LOCK_T
#endif

#if 0
/** This aggressively repeats adding each row many times. Depending on
the above settings this may be noop or may result in lots of rows being
added. */
#define TEST_ADD_EACH_LOCKS_ROW_MANY_TIMES
#endif

#if 0
/** Very similar to TEST_NO_LOCKS_ROW_IS_EVER_EQUAL_TO_LOCK_T but hash
table search is not performed at all. */
#define TEST_DO_NOT_CHECK_FOR_DUPLICATE_ROWS
#endif

#if 0
/** Do not insert each row into the hash table, duplicates may appear
if this is enabled, also if this is enabled searching into the hash is
noop because it will be empty. */
#define TEST_DO_NOT_INSERT_INTO_THE_HASH_TABLE
#endif
/* @} */

/** Memory limit passed to ha_storage_put_memlim().
@param cache hash storage
@return maximum allowed allocation size */
#define MAX_ALLOWED_FOR_STORAGE(cache)		\
	(TRX_I_S_MEM_LIMIT			\
	 - (cache)->mem_allocd)

/** Memory limit in table_cache_create_empty_row().
@param cache hash storage
@return maximum allowed allocation size */
#define MAX_ALLOWED_FOR_ALLOC(cache)		\
	(TRX_I_S_MEM_LIMIT			\
	 - (cache)->mem_allocd			\
	 - ha_storage_get_size((cache)->storage))

/** Memory for each table in the intermediate buffer is allocated in
separate chunks. These chunks are considered to be concatenated to
represent one flat array of rows. */
struct i_s_mem_chunk_t {
	ulint	offset;		/*!< offset, in number of rows */
	ulint	rows_allocd;	/*!< the size of this chunk, in number
				of rows */
	void*	base;		/*!< start of the chunk */
};

/** This represents one table's cache. */
struct i_s_table_cache_t {
	ulint		rows_used;	/*!< number of used rows */
	ulint		rows_allocd;	/*!< number of allocated rows */
	ulint		row_size;	/*!< size of a single row */
	i_s_mem_chunk_t	chunks[MEM_CHUNKS_IN_TABLE_CACHE]; /*!< array of
					memory chunks that stores the
					rows */
};

/** This structure describes the intermediate buffer */
struct trx_i_s_cache_t {
	srw_lock rw_lock;		/*!< read-write lock protecting this */
	Atomic_relaxed<ulonglong> last_read;
					/*!< last time the cache was read;
					measured in nanoseconds */
	i_s_table_cache_t innodb_trx;	/*!< innodb_trx table */
	i_s_table_cache_t innodb_locks;	/*!< innodb_locks table */
	i_s_table_cache_t innodb_lock_waits;/*!< innodb_lock_waits table */
/** the hash table size is LOCKS_HASH_CELLS_NUM * sizeof(void*) bytes */
#define LOCKS_HASH_CELLS_NUM		10000
	hash_table_t	locks_hash;	/*!< hash table used to eliminate
					duplicate entries in the
					innodb_locks table */
/** Initial size of the cache storage */
#define CACHE_STORAGE_INITIAL_SIZE	1024
/** Number of hash cells in the cache storage */
#define CACHE_STORAGE_HASH_CELLS	2048
	ha_storage_t*	storage;	/*!< storage for external volatile
					data that may become unavailable
					when we release
					lock_sys.latch */
	ulint		mem_allocd;	/*!< the amount of memory
					allocated with mem_alloc*() */
	bool		is_truncated;	/*!< this is true if the memory
					limit was hit and thus the data
					in the cache is truncated */
};

/** This is the intermediate buffer where data needed to fill the
INFORMATION SCHEMA tables is fetched and later retrieved by the C++
code in handler/i_s.cc. */
static trx_i_s_cache_t	trx_i_s_cache_static;
/** This is the intermediate buffer where data needed to fill the
INFORMATION SCHEMA tables is fetched and later retrieved by the C++
code in handler/i_s.cc. */
trx_i_s_cache_t*	trx_i_s_cache = &trx_i_s_cache_static;

/** @return the heap number of a record lock
@retval 0xFFFF for table locks */
static uint16_t wait_lock_get_heap_no(const lock_t *lock)
{
  return !lock->is_table()
    ? static_cast<uint16_t>(lock_rec_find_set_bit(lock))
    : uint16_t{0xFFFF};
}

/*******************************************************************//**
Initializes the members of a table cache. */
static
void
table_cache_init(
/*=============*/
	i_s_table_cache_t*	table_cache,	/*!< out: table cache */
	size_t			row_size)	/*!< in: the size of a
						row */
{
	ulint	i;

	table_cache->rows_used = 0;
	table_cache->rows_allocd = 0;
	table_cache->row_size = row_size;

	for (i = 0; i < MEM_CHUNKS_IN_TABLE_CACHE; i++) {

		/* the memory is actually allocated in
		table_cache_create_empty_row() */
		table_cache->chunks[i].base = NULL;
	}
}

/*******************************************************************//**
Frees a table cache. */
static
void
table_cache_free(
/*=============*/
	i_s_table_cache_t*	table_cache)	/*!< in/out: table cache */
{
	ulint	i;

	for (i = 0; i < MEM_CHUNKS_IN_TABLE_CACHE; i++) {

		/* the memory is actually allocated in
		table_cache_create_empty_row() */
		if (table_cache->chunks[i].base) {
			ut_free(table_cache->chunks[i].base);
			table_cache->chunks[i].base = NULL;
		}
	}
}

/*******************************************************************//**
Returns an empty row from a table cache. The row is allocated if no more
empty rows are available. The number of used rows is incremented.
If the memory limit is hit then NULL is returned and nothing is
allocated.
@return empty row, or NULL if out of memory */
static
void*
table_cache_create_empty_row(
/*=========================*/
	i_s_table_cache_t*	table_cache,	/*!< in/out: table cache */
	trx_i_s_cache_t*	cache)		/*!< in/out: cache to record
						how many bytes are
						allocated */
{
	ulint	i;
	void*	row;

	ut_a(table_cache->rows_used <= table_cache->rows_allocd);

	if (table_cache->rows_used == table_cache->rows_allocd) {

		/* rows_used == rows_allocd means that new chunk needs
		to be allocated: either no more empty rows in the
		last allocated chunk or nothing has been allocated yet
		(rows_num == rows_allocd == 0); */

		i_s_mem_chunk_t*	chunk;
		ulint			req_bytes;
		ulint			got_bytes;
		ulint			req_rows;
		ulint			got_rows;

		/* find the first not allocated chunk */
		for (i = 0; i < MEM_CHUNKS_IN_TABLE_CACHE; i++) {

			if (table_cache->chunks[i].base == NULL) {

				break;
			}
		}

		/* i == MEM_CHUNKS_IN_TABLE_CACHE means that all chunks
		have been allocated :-X */
		ut_a(i < MEM_CHUNKS_IN_TABLE_CACHE);

		/* allocate the chunk we just found */

		if (i == 0) {

			/* first chunk, nothing is allocated yet */
			req_rows = TABLE_CACHE_INITIAL_ROWSNUM;
		} else {

			/* Memory is increased by the formula
			new = old + old / 2; We are trying not to be
			aggressive here (= using the common new = old * 2)
			because the allocated memory will not be freed
			until InnoDB exit (it is reused). So it is better
			to once allocate the memory in more steps, but
			have less unused/wasted memory than to use less
			steps in allocation (which is done once in a
			lifetime) but end up with lots of unused/wasted
			memory. */
			req_rows = table_cache->rows_allocd / 2;
		}
		req_bytes = req_rows * table_cache->row_size;

		if (req_bytes > MAX_ALLOWED_FOR_ALLOC(cache)) {

			return(NULL);
		}

		chunk = &table_cache->chunks[i];

		got_bytes = req_bytes;
		chunk->base = ut_malloc_nokey(req_bytes);

		got_rows = got_bytes / table_cache->row_size;

		cache->mem_allocd += got_bytes;

#if 0
		printf("allocating chunk %d req bytes=%lu, got bytes=%lu,"
		       " row size=%lu,"
		       " req rows=%lu, got rows=%lu\n",
		       i, req_bytes, got_bytes,
		       table_cache->row_size,
		       req_rows, got_rows);
#endif

		chunk->rows_allocd = got_rows;

		table_cache->rows_allocd += got_rows;

		/* adjust the offset of the next chunk */
		if (i < MEM_CHUNKS_IN_TABLE_CACHE - 1) {

			table_cache->chunks[i + 1].offset
				= chunk->offset + chunk->rows_allocd;
		}

		/* return the first empty row in the newly allocated
		chunk */
		row = chunk->base;
	} else {

		char*	chunk_start;
		ulint	offset;

		/* there is an empty row, no need to allocate new
		chunks */

		/* find the first chunk that contains allocated but
		empty/unused rows */
		for (i = 0; i < MEM_CHUNKS_IN_TABLE_CACHE; i++) {

			if (table_cache->chunks[i].offset
			    + table_cache->chunks[i].rows_allocd
			    > table_cache->rows_used) {

				break;
			}
		}

		/* i == MEM_CHUNKS_IN_TABLE_CACHE means that all chunks
		are full, but
		table_cache->rows_used != table_cache->rows_allocd means
		exactly the opposite - there are allocated but
		empty/unused rows :-X */
		ut_a(i < MEM_CHUNKS_IN_TABLE_CACHE);

		chunk_start = (char*) table_cache->chunks[i].base;
		offset = table_cache->rows_used
			- table_cache->chunks[i].offset;

		row = chunk_start + offset * table_cache->row_size;
	}

	table_cache->rows_used++;

	return(row);
}

#ifdef UNIV_DEBUG
/*******************************************************************//**
Validates a row in the locks cache.
@return TRUE if valid */
static
ibool
i_s_locks_row_validate(
/*===================*/
	const i_s_locks_row_t*	row)	/*!< in: row to validate */
{
	ut_ad(row->lock_mode);
	ut_ad(row->lock_table != NULL);
	ut_ad(row->lock_table_id != 0);

	if (!row->lock_index) {
		/* table lock */
		ut_ad(!row->lock_data);
		ut_ad(row->lock_page == page_id_t(0, 0));
		ut_ad(!row->lock_rec);
	} else {
		/* record lock */
		/* row->lock_data == NULL if buf_page_try_get() == NULL */
	}

	return(TRUE);
}
#endif /* UNIV_DEBUG */

/*******************************************************************//**
Fills i_s_trx_row_t object.
If memory can not be allocated then FALSE is returned.
@return FALSE if allocation fails */
static
ibool
fill_trx_row(
/*=========*/
	i_s_trx_row_t*		row,		/*!< out: result object
						that's filled */
	const trx_t*		trx,		/*!< in: transaction to
						get data from */
	const i_s_locks_row_t*	requested_lock_row,/*!< in: pointer to the
						corresponding row in
						innodb_locks if trx is
						waiting or NULL if trx
						is not waiting */
	trx_i_s_cache_t*	cache)		/*!< in/out: cache into
						which to copy volatile
						strings */
{
	const char*	s;

	lock_sys.assert_locked();

	const lock_t* wait_lock = trx->lock.wait_lock;

	row->trx_id = trx->id;
	row->trx_started = trx->start_time;
	if (trx->in_rollback) {
		row->trx_state = "ROLLING BACK";
	} else if (trx->state == TRX_STATE_COMMITTED_IN_MEMORY) {
		row->trx_state = "COMMITTING";
	} else if (wait_lock) {
		row->trx_state = "LOCK WAIT";
	} else {
		row->trx_state = "RUNNING";
	}

	row->requested_lock_row = requested_lock_row;
	ut_ad(requested_lock_row == NULL
	      || i_s_locks_row_validate(requested_lock_row));

	ut_ad(!wait_lock == !requested_lock_row);

	const my_hrtime_t suspend_time= trx->lock.suspend_time;
	row->trx_wait_started = wait_lock ? hrtime_to_time(suspend_time) : 0;

	row->trx_weight = static_cast<uintmax_t>(TRX_WEIGHT(trx));

	if (trx->mysql_thd == NULL) {
		/* For internal transactions e.g., purge and transactions
		being recovered at startup there is no associated MySQL
		thread data structure. */
		row->trx_mysql_thread_id = 0;
		row->trx_query = NULL;
		goto thd_done;
	}

	row->trx_mysql_thread_id = thd_get_thread_id(trx->mysql_thd);

	char	query[TRX_I_S_TRX_QUERY_MAX_LEN + 1];
	if (size_t stmt_len = thd_query_safe(trx->mysql_thd, query,
					     sizeof query)) {
		row->trx_query = static_cast<const char*>(
			ha_storage_put_memlim(
				cache->storage, query, stmt_len + 1,
				MAX_ALLOWED_FOR_STORAGE(cache)));

		row->trx_query_cs = thd_charset(trx->mysql_thd);

		if (row->trx_query == NULL) {

			return(FALSE);
		}
	} else {

		row->trx_query = NULL;
	}

thd_done:
	row->trx_operation_state = trx->op_info;

	row->trx_tables_in_use = trx->n_mysql_tables_in_use;

	row->trx_tables_locked = lock_number_of_tables_locked(&trx->lock);

	/* These are protected by lock_sys.latch (which we are holding)
	and sometimes also trx->mutex. */

	row->trx_lock_structs = UT_LIST_GET_LEN(trx->lock.trx_locks);

	row->trx_lock_memory_bytes = mem_heap_get_size(trx->lock.lock_heap);

	row->trx_rows_locked = trx->lock.n_rec_locks;

	row->trx_rows_modified = trx->undo_no;

	row->trx_isolation_level = trx->isolation_level;

	row->trx_unique_checks = (ibool) trx->check_unique_secondary;

	row->trx_foreign_key_checks = (ibool) trx->check_foreigns;

	s = trx->detailed_error;

	if (s != NULL && s[0] != '\0') {

		TRX_I_S_STRING_COPY(s,
				    row->trx_foreign_key_error,
				    TRX_I_S_TRX_FK_ERROR_MAX_LEN, cache);

		if (row->trx_foreign_key_error == NULL) {

			return(FALSE);
		}
	} else {
		row->trx_foreign_key_error = NULL;
	}

	row->trx_is_read_only = trx->read_only;

	row->trx_is_autocommit_non_locking = trx->is_autocommit_non_locking();

	return(TRUE);
}

/*******************************************************************//**
Format the nth field of "rec" and put it in "buf". The result is always
NUL-terminated. Returns the number of bytes that were written to "buf"
(including the terminating NUL).
@return end of the result */
static
ulint
put_nth_field(
/*==========*/
	char*			buf,	/*!< out: buffer */
	ulint			buf_size,/*!< in: buffer size in bytes */
	ulint			n,	/*!< in: number of field */
	const dict_index_t*	index,	/*!< in: index */
	const rec_t*		rec,	/*!< in: record */
	const rec_offs*		offsets)/*!< in: record offsets, returned
					by rec_get_offsets() */
{
	const byte*	data;
	ulint		data_len;
	dict_field_t*	dict_field;
	ulint		ret;

	ut_ad(rec_offs_validate(rec, NULL, offsets));

	if (buf_size == 0) {

		return(0);
	}

	ret = 0;

	if (n > 0) {
		/* we must append ", " before the actual data */

		if (buf_size < 3) {

			buf[0] = '\0';
			return(1);
		}

		memcpy(buf, ", ", 3);

		buf += 2;
		buf_size -= 2;
		ret += 2;
	}

	/* now buf_size >= 1 */

	data = rec_get_nth_field(rec, offsets, n, &data_len);

	dict_field = dict_index_get_nth_field(index, n);

	ret += row_raw_format((const char*) data, data_len,
			      dict_field, buf, buf_size);

	return(ret);
}

/*******************************************************************//**
Fills the "lock_data" member of i_s_locks_row_t object.
If memory can not be allocated then FALSE is returned.
@return FALSE if allocation fails */
static
ibool
fill_lock_data(
/*===========*/
	const char**		lock_data,/*!< out: "lock_data" to fill */
	const lock_t*		lock,	/*!< in: lock used to find the data */
	ulint			heap_no,/*!< in: rec num used to find the data */
	trx_i_s_cache_t*	cache)	/*!< in/out: cache where to store
					volatile data */
{
	ut_a(!lock->is_table());

	switch (heap_no) {
	case PAGE_HEAP_NO_INFIMUM:
	case PAGE_HEAP_NO_SUPREMUM:
		*lock_data = ha_storage_put_str_memlim(
			cache->storage,
			heap_no == PAGE_HEAP_NO_INFIMUM
			? "infimum pseudo-record"
			: "supremum pseudo-record",
			MAX_ALLOWED_FOR_STORAGE(cache));
		return(*lock_data != NULL);
	}

	mtr_t			mtr;

	const buf_block_t*	block;
	const page_t*		page;
	const rec_t*		rec;
	ulint			n_fields;
	mem_heap_t*		heap;
	rec_offs		offsets_onstack[REC_OFFS_NORMAL_SIZE];
	rec_offs*		offsets;
	char			buf[TRX_I_S_LOCK_DATA_MAX_LEN];
	ulint			buf_used;
	ulint			i;

	mtr_start(&mtr);

	block = buf_page_try_get(lock->un_member.rec_lock.page_id, &mtr);

	if (block == NULL) {

		*lock_data = NULL;

		mtr_commit(&mtr);

		return(TRUE);
	}

	page = reinterpret_cast<const page_t*>(buf_block_get_frame(block));

	rec_offs_init(offsets_onstack);
	offsets = offsets_onstack;

	rec = page_find_rec_with_heap_no(page, heap_no);

	const dict_index_t* index = lock->index;
	ut_ad(index->is_primary() || !dict_index_is_online_ddl(index));

	n_fields = dict_index_get_n_unique(index);

	ut_a(n_fields > 0);

	heap = NULL;
	offsets = rec_get_offsets(rec, index, offsets, index->n_core_fields,
				  n_fields, &heap);

	/* format and store the data */

	buf_used = 0;
	for (i = 0; i < n_fields; i++) {

		buf_used += put_nth_field(
			buf + buf_used, sizeof(buf) - buf_used,
			i, index, rec, offsets) - 1;
	}

	*lock_data = (const char*) ha_storage_put_memlim(
		cache->storage, buf, buf_used + 1,
		MAX_ALLOWED_FOR_STORAGE(cache));

	if (heap != NULL) {

		/* this means that rec_get_offsets() has created a new
		heap and has stored offsets in it; check that this is
		really the case and free the heap */
		ut_a(offsets != offsets_onstack);
		mem_heap_free(heap);
	}

	mtr_commit(&mtr);

	if (*lock_data == NULL) {

		return(FALSE);
	}

	return(TRUE);
}

/** @return the table of a lock */
static const dict_table_t *lock_get_table(const lock_t &lock)
{
  if (lock.is_table())
    return lock.un_member.tab_lock.table;
  ut_ad(lock.index->is_primary() || !dict_index_is_online_ddl(lock.index));
  return lock.index->table;
}

/*******************************************************************//**
Fills i_s_locks_row_t object. Returns its first argument.
If memory can not be allocated then FALSE is returned.
@return false if allocation fails */
static bool fill_locks_row(
	i_s_locks_row_t* row,	/*!< out: result object that's filled */
	const lock_t*	lock,	/*!< in: lock to get data from */
	uint16_t	heap_no,/*!< in: lock's record number
				or 0 if the lock
				is a table lock */
	trx_i_s_cache_t* cache)	/*!< in/out: cache into which to copy
				volatile strings */
{
	row->lock_trx_id = lock->trx->id;
	const bool is_gap_lock = lock->is_gap();
	ut_ad(!is_gap_lock || !lock->is_table());
	switch (lock->mode()) {
	case LOCK_S:
		row->lock_mode = uint8_t(1 + is_gap_lock);
		break;
	case LOCK_X:
		row->lock_mode = uint8_t(3 + is_gap_lock);
		break;
	case LOCK_IS:
		row->lock_mode = uint8_t(5 + is_gap_lock);
		break;
	case LOCK_IX:
		row->lock_mode = uint8_t(7 + is_gap_lock);
		break;
	case LOCK_AUTO_INC:
		row->lock_mode = 9;
		break;
	default:
		ut_ad("unknown lock mode" == 0);
		row->lock_mode = 0;
	}

	const dict_table_t* table= lock_get_table(*lock);

	row->lock_table = ha_storage_put_str_memlim(
		cache->storage, table->name.m_name,
		MAX_ALLOWED_FOR_STORAGE(cache));

	/* memory could not be allocated */
	if (row->lock_table == NULL) {

		return false;
	}

	if (!lock->is_table()) {
		row->lock_index = ha_storage_put_str_memlim(
			cache->storage, lock->index->name,
			MAX_ALLOWED_FOR_STORAGE(cache));

		/* memory could not be allocated */
		if (row->lock_index == NULL) {

			return false;
		}

		row->lock_page = lock->un_member.rec_lock.page_id;
		row->lock_rec = heap_no;

		if (!fill_lock_data(&row->lock_data, lock, heap_no, cache)) {

			/* memory could not be allocated */
			return false;
		}
	} else {
		row->lock_index = NULL;

		row->lock_page = page_id_t(0, 0);
		row->lock_rec = 0;

		row->lock_data = NULL;
	}

	row->lock_table_id = table->id;

	row->hash_chain.value = row;
	ut_ad(i_s_locks_row_validate(row));

	return true;
}

/*******************************************************************//**
Fills i_s_lock_waits_row_t object. Returns its first argument.
@return result object that's filled */
static
i_s_lock_waits_row_t*
fill_lock_waits_row(
/*================*/
	i_s_lock_waits_row_t*	row,		/*!< out: result object
						that's filled */
	const i_s_locks_row_t*	requested_lock_row,/*!< in: pointer to the
						relevant requested lock
						row in innodb_locks */
	const i_s_locks_row_t*	blocking_lock_row)/*!< in: pointer to the
						relevant blocking lock
						row in innodb_locks */
{
	ut_ad(i_s_locks_row_validate(requested_lock_row));
	ut_ad(i_s_locks_row_validate(blocking_lock_row));

	row->requested_lock_row = requested_lock_row;
	row->blocking_lock_row = blocking_lock_row;

	return(row);
}

/*******************************************************************//**
Calculates a hash fold for a lock. For a record lock the fold is
calculated from 4 elements, which uniquely identify a lock at a given
point in time: transaction id, space id, page number, record number.
For a table lock the fold is table's id.
@return fold */
static
ulint
fold_lock(
/*======*/
	const lock_t*	lock,	/*!< in: lock object to fold */
	ulint		heap_no)/*!< in: lock's record number
				or 0xFFFF if the lock
				is a table lock */
{
#ifdef TEST_LOCK_FOLD_ALWAYS_DIFFERENT
	static ulint	fold = 0;

	return(fold++);
#else
	ulint	ret;

	if (!lock->is_table()) {
		ut_a(heap_no != 0xFFFF);
		ret = ut_fold_ulint_pair((ulint) lock->trx->id,
					 lock->un_member.rec_lock.page_id.
					 fold());
		ret = ut_fold_ulint_pair(ret, heap_no);
	} else {
		/* this check is actually not necessary for continuing
		correct operation, but something must have gone wrong if
		it fails. */
		ut_a(heap_no == 0xFFFF);

		ret = (ulint) lock_get_table(*lock)->id;
	}

	return(ret);
#endif
}

/*******************************************************************//**
Checks whether i_s_locks_row_t object represents a lock_t object.
@return TRUE if they match */
static
ibool
locks_row_eq_lock(
/*==============*/
	const i_s_locks_row_t*	row,	/*!< in: innodb_locks row */
	const lock_t*		lock,	/*!< in: lock object */
	ulint			heap_no)/*!< in: lock's record number
					or 0xFFFF if the lock
					is a table lock */
{
	ut_ad(i_s_locks_row_validate(row));
#ifdef TEST_NO_LOCKS_ROW_IS_EVER_EQUAL_TO_LOCK_T
	return(0);
#else
	if (!lock->is_table()) {
		ut_a(heap_no != 0xFFFF);

		return(row->lock_trx_id == lock->trx->id
		       && row->lock_page == lock->un_member.rec_lock.page_id
		       && row->lock_rec == heap_no);
	} else {
		/* this check is actually not necessary for continuing
		correct operation, but something must have gone wrong if
		it fails. */
		ut_a(heap_no == 0xFFFF);

		return(row->lock_trx_id == lock->trx->id
		       && row->lock_table_id == lock_get_table(*lock)->id);
	}
#endif
}

/*******************************************************************//**
Searches for a row in the innodb_locks cache that has a specified id.
This happens in O(1) time since a hash table is used. Returns pointer to
the row or NULL if none is found.
@return row or NULL */
static
i_s_locks_row_t*
search_innodb_locks(
/*================*/
	trx_i_s_cache_t*	cache,	/*!< in: cache */
	const lock_t*		lock,	/*!< in: lock to search for */
	uint16_t		heap_no)/*!< in: lock's record number
					or 0xFFFF if the lock
					is a table lock */
{
	i_s_hash_chain_t*	hash_chain;

	HASH_SEARCH(
		/* hash_chain->"next" */
		next,
		/* the hash table */
		&cache->locks_hash,
		/* fold */
		fold_lock(lock, heap_no),
		/* the type of the next variable */
		i_s_hash_chain_t*,
		/* auxiliary variable */
		hash_chain,
		/* assertion on every traversed item */
		ut_ad(i_s_locks_row_validate(hash_chain->value)),
		/* this determines if we have found the lock */
		locks_row_eq_lock(hash_chain->value, lock, heap_no));

	if (hash_chain == NULL) {

		return(NULL);
	}
	/* else */

	return(hash_chain->value);
}

/*******************************************************************//**
Adds new element to the locks cache, enlarging it if necessary.
Returns a pointer to the added row. If the row is already present then
no row is added and a pointer to the existing row is returned.
If row can not be allocated then NULL is returned.
@return row */
static
i_s_locks_row_t*
add_lock_to_cache(
/*==============*/
	trx_i_s_cache_t*	cache,	/*!< in/out: cache */
	const lock_t*		lock,	/*!< in: the element to add */
	uint16_t		heap_no)/*!< in: lock's record number
					or 0 if the lock
					is a table lock */
{
	i_s_locks_row_t*	dst_row;

#ifdef TEST_ADD_EACH_LOCKS_ROW_MANY_TIMES
	ulint	i;
	for (i = 0; i < 10000; i++) {
#endif
#ifndef TEST_DO_NOT_CHECK_FOR_DUPLICATE_ROWS
	/* quit if this lock is already present */
	dst_row = search_innodb_locks(cache, lock, heap_no);
	if (dst_row != NULL) {

		ut_ad(i_s_locks_row_validate(dst_row));
		return(dst_row);
	}
#endif

	dst_row = (i_s_locks_row_t*)
		table_cache_create_empty_row(&cache->innodb_locks, cache);

	/* memory could not be allocated */
	if (dst_row == NULL) {

		return(NULL);
	}

	if (!fill_locks_row(dst_row, lock, heap_no, cache)) {

		/* memory could not be allocated */
		cache->innodb_locks.rows_used--;
		return(NULL);
	}

#ifndef TEST_DO_NOT_INSERT_INTO_THE_HASH_TABLE
	HASH_INSERT(
		/* the type used in the hash chain */
		i_s_hash_chain_t,
		/* hash_chain->"next" */
		next,
		/* the hash table */
		&cache->locks_hash,
		/* fold */
		fold_lock(lock, heap_no),
		/* add this data to the hash */
		&dst_row->hash_chain);
#endif
#ifdef TEST_ADD_EACH_LOCKS_ROW_MANY_TIMES
	} /* for()-loop */
#endif

	ut_ad(i_s_locks_row_validate(dst_row));
	return(dst_row);
}

/*******************************************************************//**
Adds new pair of locks to the lock waits cache.
If memory can not be allocated then FALSE is returned.
@return FALSE if allocation fails */
static
ibool
add_lock_wait_to_cache(
/*===================*/
	trx_i_s_cache_t*	cache,		/*!< in/out: cache */
	const i_s_locks_row_t*	requested_lock_row,/*!< in: pointer to the
						relevant requested lock
						row in innodb_locks */
	const i_s_locks_row_t*	blocking_lock_row)/*!< in: pointer to the
						relevant blocking lock
						row in innodb_locks */
{
	i_s_lock_waits_row_t*	dst_row;

	dst_row = (i_s_lock_waits_row_t*)
		table_cache_create_empty_row(&cache->innodb_lock_waits,
					     cache);

	/* memory could not be allocated */
	if (dst_row == NULL) {

		return(FALSE);
	}

	fill_lock_waits_row(dst_row, requested_lock_row, blocking_lock_row);

	return(TRUE);
}

/*******************************************************************//**
Adds transaction's relevant (important) locks to cache.
If the transaction is waiting, then the wait lock is added to
innodb_locks and a pointer to the added row is returned in
requested_lock_row, otherwise requested_lock_row is set to NULL.
If rows can not be allocated then FALSE is returned and the value of
requested_lock_row is undefined.
@return FALSE if allocation fails */
static
ibool
add_trx_relevant_locks_to_cache(
/*============================*/
	trx_i_s_cache_t*	cache,	/*!< in/out: cache */
	const trx_t*		trx,	/*!< in: transaction */
	i_s_locks_row_t**	requested_lock_row)/*!< out: pointer to the
					requested lock row, or NULL or
					undefined */
{
	lock_sys.assert_locked();

	/* If transaction is waiting we add the wait lock and all locks
	from another transactions that are blocking the wait lock. */
	if (const lock_t *wait_lock = trx->lock.wait_lock) {

		const lock_t*		curr_lock;
		i_s_locks_row_t*	blocking_lock_row;
		lock_queue_iterator_t	iter;

		uint16_t wait_lock_heap_no
			= wait_lock_get_heap_no(wait_lock);

		/* add the requested lock */
		*requested_lock_row = add_lock_to_cache(cache, wait_lock,
							wait_lock_heap_no);

		/* memory could not be allocated */
		if (*requested_lock_row == NULL) {

			return(FALSE);
		}

		/* then iterate over the locks before the wait lock and
		add the ones that are blocking it */

		lock_queue_iterator_reset(&iter, wait_lock, ULINT_UNDEFINED);

		for (curr_lock = lock_queue_iterator_get_prev(&iter);
		     curr_lock != NULL;
		     curr_lock = lock_queue_iterator_get_prev(&iter)) {

			if (lock_has_to_wait(wait_lock, curr_lock)) {

				/* add the lock that is
				blocking wait_lock */
				blocking_lock_row
					= add_lock_to_cache(
						cache, curr_lock,
						/* heap_no is the same
						for the wait and waited
						locks */
						wait_lock_heap_no);

				/* memory could not be allocated */
				if (blocking_lock_row == NULL) {

					return(FALSE);
				}

				/* add the relation between both locks
				to innodb_lock_waits */
				if (!add_lock_wait_to_cache(
						cache, *requested_lock_row,
						blocking_lock_row)) {

					/* memory could not be allocated */
					return(FALSE);
				}
			}
		}
	} else {

		*requested_lock_row = NULL;
	}

	return(TRUE);
}

/** The minimum time that a cache must not be updated after it has been
read for the last time; measured in nanoseconds. We use this technique
to ensure that SELECTs which join several INFORMATION SCHEMA tables read
the same version of the cache. */
#define CACHE_MIN_IDLE_TIME_NS	100000000 /* 0.1 sec */

/*******************************************************************//**
Checks if the cache can safely be updated.
@return whether the cache can be updated */
static bool can_cache_be_updated(trx_i_s_cache_t* cache)
{
	/* cache->last_read is only updated when a shared rw lock on the
	whole cache is being held (see trx_i_s_cache_end_read()) and
	we are currently holding an exclusive rw lock on the cache.
	So it is not possible for last_read to be updated while we are
	reading it. */
	return my_interval_timer() - cache->last_read > CACHE_MIN_IDLE_TIME_NS;
}

/*******************************************************************//**
Declare a cache empty, preparing it to be filled up. Not all resources
are freed because they can be reused. */
static
void
trx_i_s_cache_clear(
/*================*/
	trx_i_s_cache_t*	cache)	/*!< out: cache to clear */
{
	cache->innodb_trx.rows_used = 0;
	cache->innodb_locks.rows_used = 0;
	cache->innodb_lock_waits.rows_used = 0;

	cache->locks_hash.clear();

	ha_storage_empty(&cache->storage);
}


/**
  Add transactions to innodb_trx's cache.

  We also add all locks that are relevant to each transaction into
  innodb_locks' and innodb_lock_waits' caches.
*/

static void fetch_data_into_cache_low(trx_i_s_cache_t *cache, const trx_t *trx)
{
  i_s_locks_row_t *requested_lock_row;

#ifdef UNIV_DEBUG
  {
    const auto state= trx->state;

    if (trx->is_autocommit_non_locking())
    {
      ut_ad(trx->read_only);
      ut_ad(!trx->is_recovered);
      ut_ad(trx->mysql_thd);
      ut_ad(state == TRX_STATE_NOT_STARTED || state == TRX_STATE_ACTIVE);
    }
    else
      ut_ad(state == TRX_STATE_ACTIVE ||
            state == TRX_STATE_PREPARED ||
            state == TRX_STATE_PREPARED_RECOVERED ||
            state == TRX_STATE_COMMITTED_IN_MEMORY);
  }
#endif /* UNIV_DEBUG */

  if (add_trx_relevant_locks_to_cache(cache, trx, &requested_lock_row))
  {
    if (i_s_trx_row_t *trx_row= reinterpret_cast<i_s_trx_row_t*>(
        table_cache_create_empty_row(&cache->innodb_trx, cache)))
    {
      if (fill_trx_row(trx_row, trx, requested_lock_row, cache))
        return;
      --cache->innodb_trx.rows_used;
    }
  }

  /* memory could not be allocated */
  cache->is_truncated= true;
}


/**
  Fetches the data needed to fill the 3 INFORMATION SCHEMA tables into the
  table cache buffer. Cache must be locked for write.
*/

static void fetch_data_into_cache(trx_i_s_cache_t *cache)
{
  LockMutexGuard g{SRW_LOCK_CALL};
  trx_i_s_cache_clear(cache);

  /* Capture the state of transactions */
  trx_sys.trx_list.for_each([cache](trx_t &trx) {
    if (!cache->is_truncated && trx.state != TRX_STATE_NOT_STARTED &&
        &trx != purge_sys.query->trx)
    {
      trx.mutex_lock();
      if (trx.state != TRX_STATE_NOT_STARTED)
        fetch_data_into_cache_low(cache, &trx);
      trx.mutex_unlock();
    }
  });
  cache->is_truncated= false;
}


/*******************************************************************//**
Update the transactions cache if it has not been read for some time.
Called from handler/i_s.cc.
@return 0 - fetched, 1 - not */
int
trx_i_s_possibly_fetch_data_into_cache(
/*===================================*/
	trx_i_s_cache_t*	cache)	/*!< in/out: cache */
{
	if (!can_cache_be_updated(cache)) {

		return(1);
	}

	/* We need to read trx_sys and record/table lock queues */
	fetch_data_into_cache(cache);

	/* update cache last read time */
	cache->last_read = my_interval_timer();

	return(0);
}

/*******************************************************************//**
Returns TRUE if the data in the cache is truncated due to the memory
limit posed by TRX_I_S_MEM_LIMIT.
@return TRUE if truncated */
bool
trx_i_s_cache_is_truncated(
/*=======================*/
	trx_i_s_cache_t*	cache)	/*!< in: cache */
{
	return(cache->is_truncated);
}

/*******************************************************************//**
Initialize INFORMATION SCHEMA trx related cache. */
void
trx_i_s_cache_init(
/*===============*/
	trx_i_s_cache_t*	cache)	/*!< out: cache to init */
{
	/* The latching is done in the following order:
	acquire trx_i_s_cache_t::rw_lock, rwlock
	acquire exclusive lock_sys.latch
	release exclusive lock_sys.latch
	release trx_i_s_cache_t::rw_lock
	acquire trx_i_s_cache_t::rw_lock, rdlock
	release trx_i_s_cache_t::rw_lock */

	cache->rw_lock.SRW_LOCK_INIT(trx_i_s_cache_lock_key);

	cache->last_read = 0;

	table_cache_init(&cache->innodb_trx, sizeof(i_s_trx_row_t));
	table_cache_init(&cache->innodb_locks, sizeof(i_s_locks_row_t));
	table_cache_init(&cache->innodb_lock_waits,
			 sizeof(i_s_lock_waits_row_t));

	cache->locks_hash.create(LOCKS_HASH_CELLS_NUM);

	cache->storage = ha_storage_create(CACHE_STORAGE_INITIAL_SIZE,
					   CACHE_STORAGE_HASH_CELLS);

	cache->mem_allocd = 0;

	cache->is_truncated = false;
}

/*******************************************************************//**
Free the INFORMATION SCHEMA trx related cache. */
void
trx_i_s_cache_free(
/*===============*/
	trx_i_s_cache_t*	cache)	/*!< in, own: cache to free */
{
	cache->rw_lock.destroy();

	cache->locks_hash.free();
	ha_storage_free(cache->storage);
	table_cache_free(&cache->innodb_trx);
	table_cache_free(&cache->innodb_locks);
	table_cache_free(&cache->innodb_lock_waits);
}

/*******************************************************************//**
Issue a shared/read lock on the tables cache. */
void
trx_i_s_cache_start_read(
/*=====================*/
	trx_i_s_cache_t*	cache)	/*!< in: cache */
{
	cache->rw_lock.rd_lock(SRW_LOCK_CALL);
}

/*******************************************************************//**
Release a shared/read lock on the tables cache. */
void
trx_i_s_cache_end_read(
/*===================*/
	trx_i_s_cache_t*	cache)	/*!< in: cache */
{
	cache->last_read = my_interval_timer();
	cache->rw_lock.rd_unlock();
}

/*******************************************************************//**
Issue an exclusive/write lock on the tables cache. */
void
trx_i_s_cache_start_write(
/*======================*/
	trx_i_s_cache_t*	cache)	/*!< in: cache */
{
	cache->rw_lock.wr_lock(SRW_LOCK_CALL);
}

/*******************************************************************//**
Release an exclusive/write lock on the tables cache. */
void
trx_i_s_cache_end_write(
/*====================*/
	trx_i_s_cache_t*	cache)	/*!< in: cache */
{
	cache->rw_lock.wr_unlock();
}

/*******************************************************************//**
Selects a INFORMATION SCHEMA table cache from the whole cache.
@return table cache */
static
i_s_table_cache_t*
cache_select_table(
/*===============*/
	trx_i_s_cache_t*	cache,	/*!< in: whole cache */
	enum i_s_table		table)	/*!< in: which table */
{
	switch (table) {
	case I_S_INNODB_TRX:
		return &cache->innodb_trx;
	case I_S_INNODB_LOCKS:
		return &cache->innodb_locks;
	case I_S_INNODB_LOCK_WAITS:
		return &cache->innodb_lock_waits;
	}

	ut_error;
	return NULL;
}

/*******************************************************************//**
Retrieves the number of used rows in the cache for a given
INFORMATION SCHEMA table.
@return number of rows */
ulint
trx_i_s_cache_get_rows_used(
/*========================*/
	trx_i_s_cache_t*	cache,	/*!< in: cache */
	enum i_s_table		table)	/*!< in: which table */
{
	i_s_table_cache_t*	table_cache;

	table_cache = cache_select_table(cache, table);

	return(table_cache->rows_used);
}

/*******************************************************************//**
Retrieves the nth row (zero-based) in the cache for a given
INFORMATION SCHEMA table.
@return row */
void*
trx_i_s_cache_get_nth_row(
/*======================*/
	trx_i_s_cache_t*	cache,	/*!< in: cache */
	enum i_s_table		table,	/*!< in: which table */
	ulint			n)	/*!< in: row number */
{
	i_s_table_cache_t*	table_cache;
	ulint			i;
	void*			row;

	table_cache = cache_select_table(cache, table);

	ut_a(n < table_cache->rows_used);

	row = NULL;

	for (i = 0; i < MEM_CHUNKS_IN_TABLE_CACHE; i++) {

		if (table_cache->chunks[i].offset
		    + table_cache->chunks[i].rows_allocd > n) {

			row = (char*) table_cache->chunks[i].base
				+ (n - table_cache->chunks[i].offset)
				* table_cache->row_size;
			break;
		}
	}

	ut_a(row != NULL);

	return(row);
}

/*******************************************************************//**
Crafts a lock id string from a i_s_locks_row_t object. Returns its
second argument. This function aborts if there is not enough space in
lock_id. Be sure to provide at least TRX_I_S_LOCK_ID_MAX_LEN + 1 if you
want to be 100% sure that it will not abort.
@return resulting lock id */
char*
trx_i_s_create_lock_id(
/*===================*/
	const i_s_locks_row_t*	row,	/*!< in: innodb_locks row */
	char*			lock_id,/*!< out: resulting lock_id */
	ulint			lock_id_size)/*!< in: size of the lock id
					buffer */
{
	int	res_len;

	/* please adjust TRX_I_S_LOCK_ID_MAX_LEN if you change this */

	if (row->lock_index) {
		/* record lock */
		res_len = snprintf(lock_id, lock_id_size,
				   TRX_ID_FMT
				   ":%u:%u:%u",
				   row->lock_trx_id, row->lock_page.space(),
				   row->lock_page.page_no(), row->lock_rec);
	} else {
		/* table lock */
		res_len = snprintf(lock_id, lock_id_size,
				   TRX_ID_FMT":" UINT64PF,
				   row->lock_trx_id,
				   row->lock_table_id);
	}

	/* the typecast is safe because snprintf(3) never returns
	negative result */
	ut_a(res_len >= 0);
	ut_a((ulint) res_len < lock_id_size);

	return(lock_id);
}
