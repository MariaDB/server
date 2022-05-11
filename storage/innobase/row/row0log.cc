/*****************************************************************************

Copyright (c) 2011, 2018, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

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
@file row/row0log.cc
Modification log for online index creation and online table rebuild

Created 2011-05-26 Marko Makela
*******************************************************/

#include "row0log.h"
#include "row0row.h"
#include "row0ins.h"
#include "row0upd.h"
#include "row0merge.h"
#include "row0ext.h"
#include "log0crypt.h"
#include "data0data.h"
#include "que0que.h"
#include "srv0mon.h"
#include "handler0alter.h"
#include "ut0stage.h"
#include "trx0rec.h"

#include <sql_class.h>
#include <algorithm>
#include <map>

Atomic_counter<ulint> onlineddl_rowlog_rows;
ulint onlineddl_rowlog_pct_used;
ulint onlineddl_pct_progress;

/** Table row modification operations during online table rebuild.
Delete-marked records are not copied to the rebuilt table. */
enum row_tab_op {
	/** Insert a record */
	ROW_T_INSERT = 0x41,
	/** Update a record in place */
	ROW_T_UPDATE,
	/** Delete (purge) a record */
	ROW_T_DELETE
};

/** Index record modification operations during online index creation */
enum row_op {
	/** Insert a record */
	ROW_OP_INSERT = 0x61,
	/** Delete a record */
	ROW_OP_DELETE
};

/** Size of the modification log entry header, in bytes */
#define ROW_LOG_HEADER_SIZE 2/*op, extra_size*/

/** Log block for modifications during online ALTER TABLE */
struct row_log_buf_t {
	byte*		block;	/*!< file block buffer */
	size_t		size; /*!< length of block in bytes */
	ut_new_pfx_t	block_pfx; /*!< opaque descriptor of "block". Set
				by ut_allocator::allocate_large() and fed to
				ut_allocator::deallocate_large(). */
	mrec_buf_t	buf;	/*!< buffer for accessing a record
				that spans two blocks */
	ulint		blocks; /*!< current position in blocks */
	ulint		bytes;	/*!< current position within block */
	ulonglong	total;	/*!< logical position, in bytes from
				the start of the row_log_table log;
				0 for row_log_online_op() and
				row_log_apply(). */
};

/** @brief Buffer for logging modifications during online index creation

All modifications to an index that is being created will be logged by
row_log_online_op() to this buffer.

All modifications to a table that is being rebuilt will be logged by
row_log_table_delete(), row_log_table_update(), row_log_table_insert()
to this buffer.

When head.blocks == tail.blocks, the reader will access tail.block
directly. When also head.bytes == tail.bytes, both counts will be
reset to 0 and the file will be truncated. */
struct row_log_t {
	pfs_os_file_t	fd;	/*!< file descriptor */
	mysql_mutex_t	mutex;	/*!< mutex protecting error,
				max_trx and tail */
	dict_table_t*	table;	/*!< table that is being rebuilt,
				or NULL when this is a secondary
				index that is being created online */
	bool		same_pk;/*!< whether the definition of the PRIMARY KEY
				has remained the same */
	const dtuple_t*	defaults;
				/*!< default values of added, changed columns,
				or NULL */
	const ulint*	col_map;/*!< mapping of old column numbers to
				new ones, or NULL if !table */
	dberr_t		error;	/*!< error that occurred during online
				table rebuild */
	/** The transaction ID of the ALTER TABLE transaction.  Any
	concurrent DML would necessarily be logged with a larger
	transaction ID, because ha_innobase::prepare_inplace_alter_table()
	acts as a barrier that ensures that any concurrent transaction
	that operates on the table would have been started after
	ha_innobase::prepare_inplace_alter_table() returns and before
	ha_innobase::commit_inplace_alter_table(commit=true) is invoked.

	Due to the nondeterministic nature of purge and due to the
	possibility of upgrading from an earlier version of MariaDB
	or MySQL, it is possible that row_log_table_low() would be
	fed DB_TRX_ID that precedes than min_trx. We must normalize
	such references to reset_trx_id[]. */
	trx_id_t	min_trx;
	trx_id_t	max_trx;/*!< biggest observed trx_id in
				row_log_online_op();
				protected by mutex and index->lock S-latch,
				or by index->lock X-latch only */
	row_log_buf_t	tail;	/*!< writer context;
				protected by mutex and index->lock S-latch,
				or by index->lock X-latch only */
	size_t		crypt_tail_size; /*!< size of crypt_tail_size*/
	byte*		crypt_tail; /*!< writer context;
				temporary buffer used in encryption,
				decryption or NULL*/
	row_log_buf_t	head;	/*!< reader context; protected by MDL only;
				modifiable by row_log_apply_ops() */
	size_t		crypt_head_size; /*!< size of crypt_tail_size*/
	byte*		crypt_head; /*!< reader context;
				temporary buffer used in encryption,
				decryption or NULL */
	const char*	path;	/*!< where to create temporary file during
				log operation */
	/** the number of core fields in the clustered index of the
	source table; before row_log_table_apply() completes, the
	table could be emptied, so that table->is_instant() no longer holds,
	but all log records must be in the "instant" format. */
	unsigned	n_core_fields;
	/** the default values of non-core fields when the operation started */
	dict_col_t::def_t* non_core_fields;
	bool		allow_not_null; /*!< Whether the alter ignore is being
				used or if the sql mode is non-strict mode;
				if not, NULL values will not be converted to
				defaults */
	const TABLE*	old_table; /*< Use old table in case of error. */

	uint64_t	n_rows; /*< Number of rows read from the table */

	/** Alter table transaction. It can be used to apply the DML logs
	into the table */
	const trx_t*	alter_trx;

	/** Determine whether the log should be in the 'instant ADD' format
	@param[in]	index	the clustered index of the source table
	@return	whether to use the 'instant ADD COLUMN' format */
	bool is_instant(const dict_index_t* index) const
	{
		ut_ad(table);
		ut_ad(n_core_fields <= index->n_fields);
		return n_core_fields != index->n_fields;
	}

	const byte* instant_field_value(ulint n, ulint* len) const
	{
		ut_ad(n >= n_core_fields);
		const dict_col_t::def_t& d= non_core_fields[n - n_core_fields];
		*len = d.len;
		return static_cast<const byte*>(d.data);
	}
};

/** Create the file or online log if it does not exist.
@param[in,out] log     online rebuild log
@return true if success, false if not */
static MY_ATTRIBUTE((warn_unused_result))
pfs_os_file_t
row_log_tmpfile(
	row_log_t*	log)
{
	DBUG_ENTER("row_log_tmpfile");
	if (log->fd == OS_FILE_CLOSED) {
		log->fd = row_merge_file_create_low(log->path);
		DBUG_EXECUTE_IF("row_log_tmpfile_fail",
				if (log->fd != OS_FILE_CLOSED)
					row_merge_file_destroy_low(log->fd);
				log->fd = OS_FILE_CLOSED;);
		if (log->fd != OS_FILE_CLOSED) {
			MONITOR_ATOMIC_INC(MONITOR_ALTER_TABLE_LOG_FILES);
		}
	}

	DBUG_RETURN(log->fd);
}

/** Allocate the memory for the log buffer.
@param[in,out]	log_buf	Buffer used for log operation
@return TRUE if success, false if not */
static MY_ATTRIBUTE((warn_unused_result))
bool
row_log_block_allocate(
	row_log_buf_t&	log_buf)
{
	DBUG_ENTER("row_log_block_allocate");
	if (log_buf.block == NULL) {
		DBUG_EXECUTE_IF(
			"simulate_row_log_allocation_failure",
			DBUG_RETURN(false);
		);

		log_buf.block = ut_allocator<byte>(mem_key_row_log_buf)
			.allocate_large(srv_sort_buf_size,
					&log_buf.block_pfx);

		if (log_buf.block == NULL) {
			DBUG_RETURN(false);
		}
		log_buf.size = srv_sort_buf_size;
	}
	DBUG_RETURN(true);
}

/** Free the log buffer.
@param[in,out]	log_buf	Buffer used for log operation */
static
void
row_log_block_free(
	row_log_buf_t&	log_buf)
{
	DBUG_ENTER("row_log_block_free");
	if (log_buf.block != NULL) {
		ut_allocator<byte>(mem_key_row_log_buf).deallocate_large(
			log_buf.block, &log_buf.block_pfx);
		log_buf.block = NULL;
	}
	DBUG_VOID_RETURN;
}

/** Logs an operation to a secondary index that is (or was) being created.
@param  index   index, S or X latched
@param  tuple   index tuple
@param  trx_id  transaction ID for insert, or 0 for delete
@retval false if row_log_apply() failure happens
or true otherwise */
bool row_log_online_op(dict_index_t *index, const dtuple_t *tuple,
                       trx_id_t trx_id)
{
	byte*		b;
	ulint		extra_size;
	ulint		size;
	ulint		mrec_size;
	ulint		avail_size;
	row_log_t*	log;
	bool		success= true;

	ut_ad(dtuple_validate(tuple));
	ut_ad(dtuple_get_n_fields(tuple) == dict_index_get_n_fields(index));
	ut_ad(index->lock.have_x() || index->lock.have_s());

	if (index->is_corrupted()) {
		return success;
	}

	ut_ad(dict_index_is_online_ddl(index)
	      || (index->online_log
		  && index->online_status == ONLINE_INDEX_COMPLETE));

	/* Compute the size of the record. This differs from
	row_merge_buf_encode(), because here we do not encode
	extra_size+1 (and reserve 0 as the end-of-chunk marker). */

	size = rec_get_converted_size_temp<false>(
		index, tuple->fields, tuple->n_fields, &extra_size);
	ut_ad(size >= extra_size);
	ut_ad(size <= sizeof log->tail.buf);

	mrec_size = ROW_LOG_HEADER_SIZE
		+ (extra_size >= 0x80) + size
		+ (trx_id ? DATA_TRX_ID_LEN : 0);

	log = index->online_log;
	mysql_mutex_lock(&log->mutex);

start_log:
	if (trx_id > log->max_trx) {
		log->max_trx = trx_id;
	}

	if (!row_log_block_allocate(log->tail)) {
		log->error = DB_OUT_OF_MEMORY;
		goto err_exit;
	}

	MEM_UNDEFINED(log->tail.buf, sizeof log->tail.buf);

	ut_ad(log->tail.bytes < srv_sort_buf_size);
	avail_size = srv_sort_buf_size - log->tail.bytes;

	if (mrec_size > avail_size) {
		b = log->tail.buf;
	} else {
		b = log->tail.block + log->tail.bytes;
	}

	if (trx_id != 0) {
		*b++ = ROW_OP_INSERT;
		trx_write_trx_id(b, trx_id);
		b += DATA_TRX_ID_LEN;
	} else {
		*b++ = ROW_OP_DELETE;
	}

	if (extra_size < 0x80) {
		*b++ = (byte) extra_size;
	} else {
		ut_ad(extra_size < 0x8000);
		*b++ = (byte) (0x80 | (extra_size >> 8));
		*b++ = (byte) extra_size;
	}

	rec_convert_dtuple_to_temp<false>(
		b + extra_size, index, tuple->fields, tuple->n_fields);

	b += size;

	if (mrec_size >= avail_size) {
		const os_offset_t	byte_offset
			= (os_offset_t) log->tail.blocks
			* srv_sort_buf_size;
		byte*			buf = log->tail.block;

		if (byte_offset + srv_sort_buf_size >= srv_online_max_size) {
			if (index->online_status != ONLINE_INDEX_COMPLETE)
				goto write_failed;
			/* About to run out of log, InnoDB has to
			apply the online log for the completed index */
			index->lock.s_unlock();
			dberr_t error= row_log_apply(
				log->alter_trx, index, nullptr, nullptr);
			index->lock.s_lock(SRW_LOCK_CALL);
			if (error != DB_SUCCESS) {
				/* Mark all newly added indexes
				as corrupted */
				log->error = error;
				success = false;
				goto err_exit;
			}

			/* Recheck whether the index online log */
			if (!index->online_log) {
				goto err_exit;
			}

			goto start_log;
		}

		if (mrec_size == avail_size) {
			ut_ad(b == &buf[srv_sort_buf_size]);
		} else {
			ut_ad(b == log->tail.buf + mrec_size);
			memcpy(buf + log->tail.bytes,
			       log->tail.buf, avail_size);
		}

		MEM_CHECK_DEFINED(buf, srv_sort_buf_size);

		if (row_log_tmpfile(log) == OS_FILE_CLOSED) {
			log->error = DB_OUT_OF_MEMORY;
			goto err_exit;
		}

		/* If encryption is enabled encrypt buffer before writing it
		to file system. */
		if (srv_encrypt_log) {
			if (!log_tmp_block_encrypt(
				    buf, srv_sort_buf_size,
				    log->crypt_tail, byte_offset)) {
				log->error = DB_DECRYPTION_FAILED;
				goto write_failed;
			}

			srv_stats.n_rowlog_blocks_encrypted.inc();
			buf = log->crypt_tail;
		}

		log->tail.blocks++;
		if (os_file_write(
			    IORequestWrite,
			    "(modification log)",
			    log->fd,
			    buf, byte_offset, srv_sort_buf_size)
		    != DB_SUCCESS) {
write_failed:
			/* We set the flag directly instead of invoking
			dict_set_corrupted_index_cache_only(index) here,
			because the index is not "public" yet. */
			index->type |= DICT_CORRUPT;
		}

		MEM_UNDEFINED(log->tail.block, srv_sort_buf_size);
		MEM_UNDEFINED(buf, srv_sort_buf_size);

		memcpy(log->tail.block, log->tail.buf + avail_size,
		       mrec_size - avail_size);
		log->tail.bytes = mrec_size - avail_size;
	} else {
		log->tail.bytes += mrec_size;
		ut_ad(b == log->tail.block + log->tail.bytes);
	}

	MEM_UNDEFINED(log->tail.buf, sizeof log->tail.buf);
err_exit:
	mysql_mutex_unlock(&log->mutex);
	return success;
}

/******************************************************//**
Gets the error status of the online index rebuild log.
@return DB_SUCCESS or error code */
dberr_t
row_log_table_get_error(
/*====================*/
	const dict_index_t*	index)	/*!< in: clustered index of a table
					that is being rebuilt online */
{
	ut_ad(dict_index_is_clust(index));
	ut_ad(dict_index_is_online_ddl(index));
	return(index->online_log->error);
}

/******************************************************//**
Starts logging an operation to a table that is being rebuilt.
@return pointer to log, or NULL if no logging is necessary */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
byte*
row_log_table_open(
/*===============*/
	row_log_t*	log,	/*!< in/out: online rebuild log */
	ulint		size,	/*!< in: size of log record */
	ulint*		avail)	/*!< out: available size for log record */
{
	mysql_mutex_lock(&log->mutex);

	MEM_UNDEFINED(log->tail.buf, sizeof log->tail.buf);

	if (log->error != DB_SUCCESS) {
err_exit:
		mysql_mutex_unlock(&log->mutex);
		return(NULL);
	}

	if (!row_log_block_allocate(log->tail)) {
		log->error = DB_OUT_OF_MEMORY;
		goto err_exit;
	}

	ut_ad(log->tail.bytes < srv_sort_buf_size);
	*avail = srv_sort_buf_size - log->tail.bytes;

	if (size > *avail) {
		/* Make sure log->tail.buf is large enough */
		ut_ad(size <= sizeof log->tail.buf);
		return(log->tail.buf);
	} else {
		return(log->tail.block + log->tail.bytes);
	}
}

/******************************************************//**
Stops logging an operation to a table that is being rebuilt. */
static MY_ATTRIBUTE((nonnull))
void
row_log_table_close_func(
/*=====================*/
	dict_index_t*	index,	/*!< in/out: online rebuilt index */
#ifdef UNIV_DEBUG
	const byte*	b,	/*!< in: end of log record */
#endif /* UNIV_DEBUG */
	ulint		size,	/*!< in: size of log record */
	ulint		avail)	/*!< in: available size for log record */
{
	row_log_t*	log = index->online_log;

	mysql_mutex_assert_owner(&log->mutex);

	if (size >= avail) {
		const os_offset_t	byte_offset
			= (os_offset_t) log->tail.blocks
			* srv_sort_buf_size;
		byte*			buf = log->tail.block;

		if (byte_offset + srv_sort_buf_size >= srv_online_max_size) {
			goto write_failed;
		}

		if (size == avail) {
			ut_ad(b == &buf[srv_sort_buf_size]);
		} else {
			ut_ad(b == log->tail.buf + size);
			memcpy(buf + log->tail.bytes, log->tail.buf, avail);
		}

		MEM_CHECK_DEFINED(buf, srv_sort_buf_size);

		if (row_log_tmpfile(log) == OS_FILE_CLOSED) {
			log->error = DB_OUT_OF_MEMORY;
			goto err_exit;
		}

		/* If encryption is enabled encrypt buffer before writing it
		to file system. */
		if (srv_encrypt_log) {
			if (!log_tmp_block_encrypt(
				    log->tail.block, srv_sort_buf_size,
				    log->crypt_tail, byte_offset,
				    index->table->space_id)) {
				log->error = DB_DECRYPTION_FAILED;
				goto err_exit;
			}

			srv_stats.n_rowlog_blocks_encrypted.inc();
			buf = log->crypt_tail;
		}

		log->tail.blocks++;
		if (os_file_write(
			    IORequestWrite,
			    "(modification log)",
			    log->fd,
			    buf, byte_offset, srv_sort_buf_size)
		    != DB_SUCCESS) {
write_failed:
			log->error = DB_ONLINE_LOG_TOO_BIG;
		}

		MEM_UNDEFINED(log->tail.block, srv_sort_buf_size);
		MEM_UNDEFINED(buf, srv_sort_buf_size);
		memcpy(log->tail.block, log->tail.buf + avail, size - avail);
		log->tail.bytes = size - avail;
	} else {
		log->tail.bytes += size;
		ut_ad(b == log->tail.block + log->tail.bytes);
	}

	log->tail.total += size;
	MEM_UNDEFINED(log->tail.buf, sizeof log->tail.buf);
err_exit:
	mysql_mutex_unlock(&log->mutex);

	onlineddl_rowlog_rows++;
	/* 10000 means 100.00%, 4525 means 45.25% */
	onlineddl_rowlog_pct_used = static_cast<ulint>((log->tail.total * 10000) / srv_online_max_size);
}

#ifdef UNIV_DEBUG
# define row_log_table_close(index, b, size, avail)	\
	row_log_table_close_func(index, b, size, avail)
#else /* UNIV_DEBUG */
# define row_log_table_close(log, b, size, avail)	\
	row_log_table_close_func(index, size, avail)
#endif /* UNIV_DEBUG */

/** Check whether a virtual column is indexed in the new table being
created during alter table
@param[in]	index	cluster index
@param[in]	v_no	virtual column number
@return true if it is indexed, else false */
bool
row_log_col_is_indexed(
	const dict_index_t*	index,
	ulint			v_no)
{
	return(dict_table_get_nth_v_col(
		index->online_log->table, v_no)->m_col.ord_part);
}

/******************************************************//**
Logs a delete operation to a table that is being rebuilt.
This will be merged in row_log_table_apply_delete(). */
void
row_log_table_delete(
/*=================*/
	const rec_t*	rec,	/*!< in: clustered index leaf page record,
				page X-latched */
	dict_index_t*	index,	/*!< in/out: clustered index, S-latched
				or X-latched */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec,index) */
	const byte*	sys)	/*!< in: DB_TRX_ID,DB_ROLL_PTR that should
				be logged, or NULL to use those in rec */
{
	ulint		old_pk_extra_size;
	ulint		old_pk_size;
	ulint		mrec_size;
	ulint		avail_size;
	mem_heap_t*	heap		= NULL;
	const dtuple_t*	old_pk;

	ut_ad(dict_index_is_clust(index));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(rec_offs_n_fields(offsets) == dict_index_get_n_fields(index));
	ut_ad(rec_offs_size(offsets) <= sizeof index->online_log->tail.buf);
	ut_ad(index->lock.have_any());

	if (index->online_status != ONLINE_INDEX_CREATION
	    || (index->type & DICT_CORRUPT) || index->table->corrupted
	    || index->online_log->error != DB_SUCCESS) {
		return;
	}

	dict_table_t* new_table = index->online_log->table;
	dict_index_t* new_index = dict_table_get_first_index(new_table);

	ut_ad(dict_index_is_clust(new_index));
	ut_ad(!dict_index_is_online_ddl(new_index));
	ut_ad(index->online_log->min_trx);

	/* Create the tuple PRIMARY KEY,DB_TRX_ID,DB_ROLL_PTR in new_table. */
	if (index->online_log->same_pk) {
		dtuple_t*	tuple;
		ut_ad(new_index->n_uniq == index->n_uniq);

		/* The PRIMARY KEY and DB_TRX_ID,DB_ROLL_PTR are in the first
		fields of the record. */
		heap = mem_heap_create(
			DATA_TRX_ID_LEN
			+ DTUPLE_EST_ALLOC(new_index->first_user_field()));
		old_pk = tuple = dtuple_create(heap,
					       new_index->first_user_field());
		dict_index_copy_types(tuple, new_index, tuple->n_fields);
		dtuple_set_n_fields_cmp(tuple, new_index->n_uniq);

		for (ulint i = 0; i < dtuple_get_n_fields(tuple); i++) {
			ulint		len;
			const void*	field	= rec_get_nth_field(
				rec, offsets, i, &len);
			dfield_t*	dfield	= dtuple_get_nth_field(
				tuple, i);
			ut_ad(len != UNIV_SQL_NULL);
			ut_ad(!rec_offs_nth_extern(offsets, i));
			dfield_set_data(dfield, field, len);
		}

		dfield_t* db_trx_id = dtuple_get_nth_field(
			tuple, new_index->n_uniq);

		const bool replace_sys_fields
			= sys
			|| trx_read_trx_id(static_cast<byte*>(db_trx_id->data))
			< index->online_log->min_trx;

		if (replace_sys_fields) {
			if (!sys || trx_read_trx_id(sys)
			    < index->online_log->min_trx) {
				sys = reset_trx_id;
			}

			dfield_set_data(db_trx_id, sys, DATA_TRX_ID_LEN);
			dfield_set_data(db_trx_id + 1, sys + DATA_TRX_ID_LEN,
					DATA_ROLL_PTR_LEN);
		}

		ut_d(trx_id_check(db_trx_id->data,
				  index->online_log->min_trx));
	} else {
		/* The PRIMARY KEY has changed. Translate the tuple. */
		old_pk = row_log_table_get_pk(
			rec, index, offsets, NULL, &heap);

		if (!old_pk) {
			ut_ad(index->online_log->error != DB_SUCCESS);
			if (heap) {
				goto func_exit;
			}
			return;
		}
	}

	ut_ad(DATA_TRX_ID_LEN == dtuple_get_nth_field(
		      old_pk, old_pk->n_fields - 2)->len);
	ut_ad(DATA_ROLL_PTR_LEN == dtuple_get_nth_field(
		      old_pk, old_pk->n_fields - 1)->len);
	old_pk_size = rec_get_converted_size_temp<false>(
		new_index, old_pk->fields, old_pk->n_fields,
		&old_pk_extra_size);
	ut_ad(old_pk_extra_size < 0x100);

	/* 2 = 1 (extra_size) + at least 1 byte payload */
	mrec_size = 2 + old_pk_size;

	if (byte* b = row_log_table_open(index->online_log,
					 mrec_size, &avail_size)) {
		*b++ = ROW_T_DELETE;
		*b++ = static_cast<byte>(old_pk_extra_size);

		rec_convert_dtuple_to_temp<false>(
			b + old_pk_extra_size, new_index,
			old_pk->fields, old_pk->n_fields);

		b += old_pk_size;

		row_log_table_close(index, b, mrec_size, avail_size);
	}

func_exit:
	mem_heap_free(heap);
}

/******************************************************//**
Logs an insert or update to a table that is being rebuilt. */
static
void
row_log_table_low_redundant(
/*========================*/
	const rec_t*		rec,	/*!< in: clustered index leaf
					page record in ROW_FORMAT=REDUNDANT,
					page X-latched */
	dict_index_t*		index,	/*!< in/out: clustered index, S-latched
					or X-latched */
	bool			insert,	/*!< in: true if insert,
					false if update */
	const dtuple_t*		old_pk,	/*!< in: old PRIMARY KEY value
					(if !insert and a PRIMARY KEY
					is being created) */
	const dict_index_t*	new_index)
					/*!< in: clustered index of the
					new table, not latched */
{
	ulint		old_pk_size;
	ulint		old_pk_extra_size;
	ulint		size;
	ulint		extra_size;
	ulint		mrec_size;
	ulint		avail_size;
	mem_heap_t*	heap		= NULL;
	dtuple_t*	tuple;
	const ulint	n_fields = rec_get_n_fields_old(rec);

	ut_ad(index->n_fields >= n_fields);
	ut_ad(index->n_fields == n_fields || index->is_instant());
	ut_ad(dict_tf2_is_valid(index->table->flags, index->table->flags2));
	ut_ad(!dict_table_is_comp(index->table));  /* redundant row format */
	ut_ad(dict_index_is_clust(new_index));

	heap = mem_heap_create(DTUPLE_EST_ALLOC(n_fields));
	tuple = dtuple_create(heap, n_fields);
	dict_index_copy_types(tuple, index, n_fields);

	dtuple_set_n_fields_cmp(tuple, dict_index_get_n_unique(index));

	if (rec_get_1byte_offs_flag(rec)) {
		for (ulint i = 0; i < n_fields; i++) {
			dfield_t*	dfield;
			ulint		len;
			const void*	field;

			dfield = dtuple_get_nth_field(tuple, i);
			field = rec_get_nth_field_old(rec, i, &len);

			dfield_set_data(dfield, field, len);
		}
	} else {
		for (ulint i = 0; i < n_fields; i++) {
			dfield_t*	dfield;
			ulint		len;
			const void*	field;

			dfield = dtuple_get_nth_field(tuple, i);
			field = rec_get_nth_field_old(rec, i, &len);

			dfield_set_data(dfield, field, len);

			if (rec_2_is_field_extern(rec, i)) {
				dfield_set_ext(dfield);
			}
		}
	}

	dfield_t* db_trx_id = dtuple_get_nth_field(tuple, index->n_uniq);
	ut_ad(dfield_get_len(db_trx_id) == DATA_TRX_ID_LEN);
	ut_ad(dfield_get_len(db_trx_id + 1) == DATA_ROLL_PTR_LEN);

	if (trx_read_trx_id(static_cast<const byte*>
			    (dfield_get_data(db_trx_id)))
	    < index->online_log->min_trx) {
		dfield_set_data(db_trx_id, reset_trx_id, DATA_TRX_ID_LEN);
		dfield_set_data(db_trx_id + 1, reset_trx_id + DATA_TRX_ID_LEN,
				DATA_ROLL_PTR_LEN);
	}

	const bool is_instant = index->online_log->is_instant(index);
	rec_comp_status_t status = is_instant
		? REC_STATUS_INSTANT : REC_STATUS_ORDINARY;

	size = rec_get_converted_size_temp<true>(
		index, tuple->fields, tuple->n_fields, &extra_size, status);
	if (is_instant) {
		size++;
		extra_size++;
	}

	mrec_size = ROW_LOG_HEADER_SIZE + size + (extra_size >= 0x80);

	if (insert || index->online_log->same_pk) {
		ut_ad(!old_pk);
		old_pk_extra_size = old_pk_size = 0;
	} else {
		ut_ad(old_pk);
		ut_ad(old_pk->n_fields == 2 + old_pk->n_fields_cmp);
		ut_ad(DATA_TRX_ID_LEN == dtuple_get_nth_field(
			      old_pk, old_pk->n_fields - 2)->len);
		ut_ad(DATA_ROLL_PTR_LEN == dtuple_get_nth_field(
			      old_pk, old_pk->n_fields - 1)->len);

		old_pk_size = rec_get_converted_size_temp<false>(
			new_index, old_pk->fields, old_pk->n_fields,
			&old_pk_extra_size);
		ut_ad(old_pk_extra_size < 0x100);
		mrec_size += 1/*old_pk_extra_size*/ + old_pk_size;
	}

	if (byte* b = row_log_table_open(index->online_log,
					 mrec_size, &avail_size)) {
		if (insert) {
			*b++ = ROW_T_INSERT;
		} else {
			*b++ = ROW_T_UPDATE;

			if (old_pk_size) {
				*b++ = static_cast<byte>(old_pk_extra_size);

				rec_convert_dtuple_to_temp<false>(
					b + old_pk_extra_size, new_index,
					old_pk->fields, old_pk->n_fields);
				b += old_pk_size;
			}
		}

		if (extra_size < 0x80) {
			*b++ = static_cast<byte>(extra_size);
		} else {
			ut_ad(extra_size < 0x8000);
			*b++ = static_cast<byte>(0x80 | (extra_size >> 8));
			*b++ = static_cast<byte>(extra_size);
		}

		if (status == REC_STATUS_INSTANT) {
			ut_ad(is_instant);
			if (n_fields <= index->online_log->n_core_fields) {
				status = REC_STATUS_ORDINARY;
			}
			*b = status;
		}

		rec_convert_dtuple_to_temp<true>(
			b + extra_size, index, tuple->fields, tuple->n_fields,
			status);
		b += size;

		row_log_table_close(index, b, mrec_size, avail_size);
	}

	mem_heap_free(heap);
}

/******************************************************//**
Logs an insert or update to a table that is being rebuilt. */
static
void
row_log_table_low(
/*==============*/
	const rec_t*	rec,	/*!< in: clustered index leaf page record,
				page X-latched */
	dict_index_t*	index,	/*!< in/out: clustered index, S-latched
				or X-latched */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec,index) */
	bool		insert,	/*!< in: true if insert, false if update */
	const dtuple_t*	old_pk)	/*!< in: old PRIMARY KEY value (if !insert
				and a PRIMARY KEY is being created) */
{
	ulint			old_pk_size;
	ulint			old_pk_extra_size;
	ulint			extra_size;
	ulint			mrec_size;
	ulint			avail_size;
	const dict_index_t*	new_index;
	row_log_t*		log = index->online_log;

	new_index = dict_table_get_first_index(log->table);

	ut_ad(dict_index_is_clust(index));
	ut_ad(dict_index_is_clust(new_index));
	ut_ad(!dict_index_is_online_ddl(new_index));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(rec_offs_n_fields(offsets) == dict_index_get_n_fields(index));
	ut_ad(rec_offs_size(offsets) <= sizeof log->tail.buf);
	ut_ad(index->lock.have_any());

	/* old_pk=row_log_table_get_pk() [not needed in INSERT] is a prefix
	of the clustered index record (PRIMARY KEY,DB_TRX_ID,DB_ROLL_PTR),
	with no information on virtual columns */
	ut_ad(!old_pk || !insert);
	ut_ad(!old_pk || old_pk->n_v_fields == 0);

	if (index->online_status != ONLINE_INDEX_CREATION
	    || (index->type & DICT_CORRUPT) || index->table->corrupted
	    || log->error != DB_SUCCESS) {
		return;
	}

	if (!rec_offs_comp(offsets)) {
		row_log_table_low_redundant(
			rec, index, insert, old_pk, new_index);
		return;
	}

	ut_ad(rec_get_status(rec) == REC_STATUS_ORDINARY
	      || rec_get_status(rec) == REC_STATUS_INSTANT);

	const ulint omit_size = REC_N_NEW_EXTRA_BYTES;

	const ulint rec_extra_size = rec_offs_extra_size(offsets) - omit_size;
	const bool is_instant = log->is_instant(index);
	extra_size = rec_extra_size + is_instant;

	unsigned fake_extra_size = 0;
	byte fake_extra_buf[3];
	if (is_instant && UNIV_UNLIKELY(!index->is_instant())) {
		/* The source table was emptied after ALTER TABLE
		started, and it was converted to non-instant format.
		Because row_log_table_apply_op() expects to find
		all records to be logged in the same way, we will
		be unable to copy the rec_extra_size bytes from the
		record header, but must convert them here. */
		unsigned n_add = index->n_fields - 1 - log->n_core_fields;
		fake_extra_size = rec_get_n_add_field_len(n_add);
		ut_ad(fake_extra_size == 1 || fake_extra_size == 2);
		extra_size += fake_extra_size;
		byte* fake_extra = fake_extra_buf + fake_extra_size;
		rec_set_n_add_field(fake_extra, n_add);
		ut_ad(fake_extra == fake_extra_buf);
	}

	mrec_size = ROW_LOG_HEADER_SIZE
		+ (extra_size >= 0x80) + rec_offs_size(offsets) - omit_size
		+ is_instant + fake_extra_size;

	if (insert || log->same_pk) {
		ut_ad(!old_pk);
		old_pk_extra_size = old_pk_size = 0;
	} else {
		ut_ad(old_pk);
		ut_ad(old_pk->n_fields == 2 + old_pk->n_fields_cmp);
		ut_ad(DATA_TRX_ID_LEN == dtuple_get_nth_field(
			      old_pk, old_pk->n_fields - 2)->len);
		ut_ad(DATA_ROLL_PTR_LEN == dtuple_get_nth_field(
			      old_pk, old_pk->n_fields - 1)->len);

		old_pk_size = rec_get_converted_size_temp<false>(
			new_index, old_pk->fields, old_pk->n_fields,
			&old_pk_extra_size);
		ut_ad(old_pk_extra_size < 0x100);
		mrec_size += 1/*old_pk_extra_size*/ + old_pk_size;
	}

	if (byte* b = row_log_table_open(log, mrec_size, &avail_size)) {
		if (insert) {
			*b++ = ROW_T_INSERT;
		} else {
			*b++ = ROW_T_UPDATE;

			if (old_pk_size) {
				*b++ = static_cast<byte>(old_pk_extra_size);

				rec_convert_dtuple_to_temp<false>(
					b + old_pk_extra_size, new_index,
					old_pk->fields, old_pk->n_fields);
				b += old_pk_size;
			}
		}

		if (extra_size < 0x80) {
			*b++ = static_cast<byte>(extra_size);
		} else {
			ut_ad(extra_size < 0x8000);
			*b++ = static_cast<byte>(0x80 | (extra_size >> 8));
			*b++ = static_cast<byte>(extra_size);
		}

		if (is_instant) {
			*b++ = fake_extra_size
				? REC_STATUS_INSTANT
				: rec_get_status(rec);
		} else {
			ut_ad(rec_get_status(rec) == REC_STATUS_ORDINARY);
		}

		memcpy(b, rec - rec_extra_size - omit_size, rec_extra_size);
		b += rec_extra_size;
		memcpy(b, fake_extra_buf + 1, fake_extra_size);
		b += fake_extra_size;
		ulint len;
		ulint trx_id_offs = rec_get_nth_field_offs(
			offsets, index->n_uniq, &len);
		ut_ad(len == DATA_TRX_ID_LEN);
		memcpy(b, rec, rec_offs_data_size(offsets));
		if (trx_read_trx_id(b + trx_id_offs) < log->min_trx) {
			memcpy(b + trx_id_offs,
			       reset_trx_id, sizeof reset_trx_id);
		}
		b += rec_offs_data_size(offsets);

		row_log_table_close(index, b, mrec_size, avail_size);
	}
}

/******************************************************//**
Logs an update to a table that is being rebuilt.
This will be merged in row_log_table_apply_update(). */
void
row_log_table_update(
/*=================*/
	const rec_t*	rec,	/*!< in: clustered index leaf page record,
				page X-latched */
	dict_index_t*	index,	/*!< in/out: clustered index, S-latched
				or X-latched */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec,index) */
	const dtuple_t*	old_pk)	/*!< in: row_log_table_get_pk()
				before the update */
{
	row_log_table_low(rec, index, offsets, false, old_pk);
}

/** Gets the old table column of a PRIMARY KEY column.
@param table old table (before ALTER TABLE)
@param col_map mapping of old column numbers to new ones
@param col_no column position in the new table
@return old table column, or NULL if this is an added column */
static
const dict_col_t*
row_log_table_get_pk_old_col(
/*=========================*/
	const dict_table_t*	table,
	const ulint*		col_map,
	ulint			col_no)
{
	for (ulint i = 0; i < table->n_cols; i++) {
		if (col_no == col_map[i]) {
			return(dict_table_get_nth_col(table, i));
		}
	}

	return(NULL);
}

/** Maps an old table column of a PRIMARY KEY column.
@param[in]	ifield		clustered index field in the new table (after
ALTER TABLE)
@param[in]	index		the clustered index of ifield
@param[in,out]	dfield		clustered index tuple field in the new table
@param[in,out]	heap		memory heap for allocating dfield contents
@param[in]	rec		clustered index leaf page record in the old
table
@param[in]	offsets		rec_get_offsets(rec)
@param[in]	i		rec field corresponding to col
@param[in]	zip_size	ROW_FORMAT=COMPRESSED size of the old table
@param[in]	max_len		maximum length of dfield
@param[in]	log		row log for the table
@retval DB_INVALID_NULL		if a NULL value is encountered
@retval DB_TOO_BIG_INDEX_COL	if the maximum prefix length is exceeded */
static
dberr_t
row_log_table_get_pk_col(
	const dict_field_t*	ifield,
	const dict_index_t*	index,
	dfield_t*		dfield,
	mem_heap_t*		heap,
	const rec_t*		rec,
	const rec_offs*		offsets,
	ulint			i,
	ulint			zip_size,
	ulint			max_len,
	const row_log_t*	log)
{
	const byte*	field;
	ulint		len;

	field = rec_get_nth_field(rec, offsets, i, &len);

	if (len == UNIV_SQL_DEFAULT) {
		field = log->instant_field_value(i, &len);
	}

	if (len == UNIV_SQL_NULL) {
		if (!log->allow_not_null) {
			return(DB_INVALID_NULL);
		}

		unsigned col_no= ifield->col->ind;
		ut_ad(col_no < log->defaults->n_fields);

		field = static_cast<const byte*>(
			log->defaults->fields[col_no].data);
		if (!field) {
			return(DB_INVALID_NULL);
		}
		len = log->defaults->fields[col_no].len;
	}

	if (rec_offs_nth_extern(offsets, i)) {
		ulint	field_len = ifield->prefix_len;
		byte*	blob_field;

		if (!field_len) {
			field_len = ifield->fixed_len;
			if (!field_len) {
				field_len = max_len + 1;
			}
		}

		blob_field = static_cast<byte*>(
			mem_heap_alloc(heap, field_len));

		len = btr_copy_externally_stored_field_prefix(
			blob_field, field_len, zip_size, field, len);
		if (len >= max_len + 1) {
			return(DB_TOO_BIG_INDEX_COL);
		}

		dfield_set_data(dfield, blob_field, len);
	} else {
		dfield_set_data(dfield, mem_heap_dup(heap, field, len), len);
	}

	return(DB_SUCCESS);
}

/******************************************************//**
Constructs the old PRIMARY KEY and DB_TRX_ID,DB_ROLL_PTR
of a table that is being rebuilt.
@return tuple of PRIMARY KEY,DB_TRX_ID,DB_ROLL_PTR in the rebuilt table,
or NULL if the PRIMARY KEY definition does not change */
const dtuple_t*
row_log_table_get_pk(
/*=================*/
	const rec_t*	rec,	/*!< in: clustered index leaf page record,
				page X-latched */
	dict_index_t*	index,	/*!< in/out: clustered index, S-latched
				or X-latched */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec,index) */
	byte*		sys,	/*!< out: DB_TRX_ID,DB_ROLL_PTR for
				row_log_table_delete(), or NULL */
	mem_heap_t**	heap)	/*!< in/out: memory heap where allocated */
{
	dtuple_t*	tuple	= NULL;
	row_log_t*	log	= index->online_log;

	ut_ad(dict_index_is_clust(index));
	ut_ad(dict_index_is_online_ddl(index));
	ut_ad(!offsets || rec_offs_validate(rec, index, offsets));
	ut_ad(index->lock.have_any());
	ut_ad(log);
	ut_ad(log->table);
	ut_ad(log->min_trx);

	if (log->same_pk) {
		/* The PRIMARY KEY columns are unchanged. */
		if (sys) {
			/* Store the DB_TRX_ID,DB_ROLL_PTR. */
			ulint	trx_id_offs = index->trx_id_offset;

			if (!trx_id_offs) {
				ulint	len;

				if (!offsets) {
					offsets = rec_get_offsets(
						rec, index, nullptr,
						index->n_core_fields,
						index->db_trx_id() + 1, heap);
				}

				trx_id_offs = rec_get_nth_field_offs(
					offsets, index->db_trx_id(), &len);
				ut_ad(len == DATA_TRX_ID_LEN);
			}

			const byte* ptr = trx_read_trx_id(rec + trx_id_offs)
				< log->min_trx
				? reset_trx_id
				: rec + trx_id_offs;

			memcpy(sys, ptr, DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
			ut_d(trx_id_check(sys, log->min_trx));
		}

		return(NULL);
	}

	mysql_mutex_lock(&log->mutex);

	/* log->error is protected by log->mutex. */
	if (log->error == DB_SUCCESS) {
		dict_table_t*	new_table	= log->table;
		dict_index_t*	new_index
			= dict_table_get_first_index(new_table);
		const ulint	new_n_uniq
			= dict_index_get_n_unique(new_index);

		if (!*heap) {
			ulint	size = 0;

			if (!offsets) {
				size += (1 + REC_OFFS_HEADER_SIZE
					 + unsigned(index->n_fields))
					* sizeof *offsets;
			}

			for (ulint i = 0; i < new_n_uniq; i++) {
				size += dict_col_get_min_size(
					dict_index_get_nth_col(new_index, i));
			}

			*heap = mem_heap_create(
				DTUPLE_EST_ALLOC(new_n_uniq + 2) + size);
		}

		if (!offsets) {
			offsets = rec_get_offsets(rec, index, nullptr,
						  index->n_core_fields,
						  ULINT_UNDEFINED, heap);
		}

		tuple = dtuple_create(*heap, new_n_uniq + 2);
		dict_index_copy_types(tuple, new_index, tuple->n_fields);
		dtuple_set_n_fields_cmp(tuple, new_n_uniq);

		const ulint max_len = DICT_MAX_FIELD_LEN_BY_FORMAT(new_table);

		const ulint zip_size = index->table->space->zip_size();

		for (ulint new_i = 0; new_i < new_n_uniq; new_i++) {
			dict_field_t*	ifield;
			dfield_t*	dfield;
			ulint		prtype;
			ulint		mbminlen, mbmaxlen;

			ifield = dict_index_get_nth_field(new_index, new_i);
			dfield = dtuple_get_nth_field(tuple, new_i);

			const ulint	col_no
				= dict_field_get_col(ifield)->ind;

			if (const dict_col_t* col
			    = row_log_table_get_pk_old_col(
				    index->table, log->col_map, col_no)) {
				ulint	i = dict_col_get_clust_pos(col, index);

				if (i == ULINT_UNDEFINED) {
					ut_ad(0);
					log->error = DB_CORRUPTION;
					goto err_exit;
				}

				log->error = row_log_table_get_pk_col(
					ifield, new_index, dfield, *heap,
					rec, offsets, i, zip_size, max_len,
					log);

				if (log->error != DB_SUCCESS) {
err_exit:
					tuple = NULL;
					goto func_exit;
				}

				mbminlen = col->mbminlen;
				mbmaxlen = col->mbmaxlen;
				prtype = col->prtype;
			} else {
				/* No matching column was found in the old
				table, so this must be an added column.
				Copy the default value. */
				ut_ad(log->defaults);

				dfield_copy(dfield, dtuple_get_nth_field(
						    log->defaults, col_no));
				mbminlen = dfield->type.mbminlen;
				mbmaxlen = dfield->type.mbmaxlen;
				prtype = dfield->type.prtype;
			}

			ut_ad(!dfield_is_ext(dfield));
			ut_ad(!dfield_is_null(dfield));

			if (ifield->prefix_len) {
				ulint	len = dtype_get_at_most_n_mbchars(
					prtype, mbminlen, mbmaxlen,
					ifield->prefix_len,
					dfield_get_len(dfield),
					static_cast<const char*>(
						dfield_get_data(dfield)));

				ut_ad(len <= dfield_get_len(dfield));
				dfield_set_len(dfield, len);
			}
		}

		const byte* trx_roll = rec
			+ row_get_trx_id_offset(index, offsets);

		/* Copy the fields, because the fields will be updated
		or the record may be moved somewhere else in the B-tree
		as part of the upcoming operation. */
		if (trx_read_trx_id(trx_roll) < log->min_trx) {
			trx_roll = reset_trx_id;
			if (sys) {
				memcpy(sys, trx_roll,
				       DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
			}
		} else if (sys) {
			memcpy(sys, trx_roll,
			       DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
			trx_roll = sys;
		} else {
			trx_roll = static_cast<const byte*>(
				mem_heap_dup(
					*heap, trx_roll,
					DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN));
		}

		ut_d(trx_id_check(trx_roll, log->min_trx));

		dfield_set_data(dtuple_get_nth_field(tuple, new_n_uniq),
				trx_roll, DATA_TRX_ID_LEN);
		dfield_set_data(dtuple_get_nth_field(tuple, new_n_uniq + 1),
				trx_roll + DATA_TRX_ID_LEN, DATA_ROLL_PTR_LEN);
	}

func_exit:
	mysql_mutex_unlock(&log->mutex);
	return(tuple);
}

/******************************************************//**
Logs an insert to a table that is being rebuilt.
This will be merged in row_log_table_apply_insert(). */
void
row_log_table_insert(
/*=================*/
	const rec_t*	rec,	/*!< in: clustered index leaf page record,
				page X-latched */
	dict_index_t*	index,	/*!< in/out: clustered index, S-latched
				or X-latched */
	const rec_offs*	offsets)/*!< in: rec_get_offsets(rec,index) */
{
	row_log_table_low(rec, index, offsets, true, NULL);
}

/******************************************************//**
Converts a log record to a table row.
@return converted row, or NULL if the conversion fails */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
const dtuple_t*
row_log_table_apply_convert_mrec(
/*=============================*/
	const mrec_t*		mrec,		/*!< in: merge record */
	dict_index_t*		index,		/*!< in: index of mrec */
	const rec_offs*		offsets,	/*!< in: offsets of mrec */
	row_log_t*		log,		/*!< in: rebuild context */
	mem_heap_t*		heap,		/*!< in/out: memory heap */
	dberr_t*		error)		/*!< out: DB_SUCCESS or
						DB_MISSING_HISTORY or
						reason of failure */
{
	dtuple_t*	row;

	log->n_rows++;
	*error = DB_SUCCESS;

	/* This is based on row_build(). */
	if (log->defaults) {
		row = dtuple_copy(log->defaults, heap);
		/* dict_table_copy_types() would set the fields to NULL */
		for (ulint i = 0; i < dict_table_get_n_cols(log->table); i++) {
			dict_col_copy_type(
				dict_table_get_nth_col(log->table, i),
				dfield_get_type(dtuple_get_nth_field(row, i)));
		}
	} else {
		row = dtuple_create(heap, dict_table_get_n_cols(log->table));
		dict_table_copy_types(row, log->table);
	}

	for (ulint i = 0; i < rec_offs_n_fields(offsets); i++) {
		const dict_field_t*	ind_field
			= dict_index_get_nth_field(index, i);

		if (ind_field->prefix_len) {
			/* Column prefixes can only occur in key
			fields, which cannot be stored externally. For
			a column prefix, there should also be the full
			field in the clustered index tuple. The row
			tuple comprises full fields, not prefixes. */
			ut_ad(!rec_offs_nth_extern(offsets, i));
			continue;
		}

		const dict_col_t*	col
			= dict_field_get_col(ind_field);

		if (col->is_dropped()) {
			/* the column was instantly dropped earlier */
			ut_ad(index->table->instant);
			continue;
		}

		ulint			col_no
			= log->col_map[dict_col_get_no(col)];

		if (col_no == ULINT_UNDEFINED) {
			/* the column is being dropped now */
			continue;
		}

		dfield_t*	dfield
			= dtuple_get_nth_field(row, col_no);

		ulint			len;
		const byte*		data;

		if (rec_offs_nth_extern(offsets, i)) {
			ut_ad(rec_offs_any_extern(offsets));
			index->lock.x_lock(SRW_LOCK_CALL);

			data = btr_rec_copy_externally_stored_field(
				mrec, offsets,
				index->table->space->zip_size(),
				i, &len, heap);
			ut_a(data);
			dfield_set_data(dfield, data, len);

			index->lock.x_unlock();
		} else {
			data = rec_get_nth_field(mrec, offsets, i, &len);
			if (len == UNIV_SQL_DEFAULT) {
				data = log->instant_field_value(i, &len);
			}
			dfield_set_data(dfield, data, len);
		}

		if (len != UNIV_SQL_NULL && col->mtype == DATA_MYSQL
		    && col->len != len && !dict_table_is_comp(log->table)) {

			ut_ad(col->len >= len);
			if (dict_table_is_comp(index->table)) {
				byte*	buf = (byte*) mem_heap_alloc(heap,
								     col->len);
				memcpy(buf, dfield->data, len);
				memset(buf + len, 0x20, col->len - len);

				dfield_set_data(dfield, buf, col->len);
			} else {
				/* field length mismatch should not happen
				when rebuilding the redundant row format
				table. */
				ut_ad(0);
				*error = DB_CORRUPTION;
				return(NULL);
			}
		}

		/* See if any columns were changed to NULL or NOT NULL. */
		const dict_col_t*	new_col
			= dict_table_get_nth_col(log->table, col_no);
		ut_ad(new_col->same_format(*col));

		/* Assert that prtype matches except for nullability. */
		ut_ad(!((new_col->prtype ^ dfield_get_type(dfield)->prtype)
			& ~(DATA_NOT_NULL | DATA_VERSIONED
			    | CHAR_COLL_MASK << 16 | DATA_LONG_TRUE_VARCHAR)));

		if (new_col->prtype == col->prtype) {
			continue;
		}

		if ((new_col->prtype & DATA_NOT_NULL)
		    && dfield_is_null(dfield)) {

			if (!log->allow_not_null) {
				/* We got a NULL value for a NOT NULL column. */
				*error = DB_INVALID_NULL;
				return NULL;
			}

			const dfield_t& default_field
				= log->defaults->fields[col_no];

			Field* field = log->old_table->field[col->ind];

			field->set_warning(Sql_condition::WARN_LEVEL_WARN,
					   WARN_DATA_TRUNCATED, 1,
					   ulong(log->n_rows));

			*dfield = default_field;
		}

		/* Adjust the DATA_NOT_NULL flag in the parsed row. */
		dfield_get_type(dfield)->prtype = new_col->prtype;

		ut_ad(dict_col_type_assert_equal(new_col,
						 dfield_get_type(dfield)));
	}

	return(row);
}

/******************************************************//**
Replays an insert operation on a table that was rebuilt.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_log_table_apply_insert_low(
/*===========================*/
	que_thr_t*		thr,		/*!< in: query graph */
	const dtuple_t*		row,		/*!< in: table row
						in the old table definition */
	mem_heap_t*		offsets_heap,	/*!< in/out: memory heap
						that can be emptied */
	mem_heap_t*		heap,		/*!< in/out: memory heap */
	row_merge_dup_t*	dup)		/*!< in/out: for reporting
						duplicate key errors */
{
	dberr_t		error;
	dtuple_t*	entry;
	const row_log_t*log	= dup->index->online_log;
	dict_index_t*	index	= dict_table_get_first_index(log->table);
	ulint		n_index = 0;

	ut_ad(dtuple_validate(row));

	DBUG_LOG("ib_alter_table",
		 "insert table " << index->table->id << " (index "
		 << index->id << "): " << rec_printer(row).str());

	static const ulint	flags
		= (BTR_CREATE_FLAG
		   | BTR_NO_LOCKING_FLAG
		   | BTR_NO_UNDO_LOG_FLAG
		   | BTR_KEEP_SYS_FLAG);

	entry = row_build_index_entry(row, NULL, index, heap);

	error = row_ins_clust_index_entry_low(
		flags, BTR_MODIFY_TREE, index, index->n_uniq,
		entry, 0, thr);

	switch (error) {
	case DB_SUCCESS:
		break;
	case DB_SUCCESS_LOCKED_REC:
		/* The row had already been copied to the table. */
		return(DB_SUCCESS);
	default:
		return(error);
	}

	ut_ad(dict_index_is_clust(index));

	for (n_index += index->type != DICT_CLUSTERED;
	     (index = dict_table_get_next_index(index)); n_index++) {
		if (index->type & DICT_FTS) {
			continue;
		}

		entry = row_build_index_entry(row, NULL, index, heap);
		error = row_ins_sec_index_entry_low(
			flags, BTR_MODIFY_TREE,
			index, offsets_heap, heap, entry,
			thr_get_trx(thr)->id, thr);

		if (error != DB_SUCCESS) {
			if (error == DB_DUPLICATE_KEY) {
				thr_get_trx(thr)->error_key_num = n_index;
			}
			break;
		}
	}

	return(error);
}

/******************************************************//**
Replays an insert operation on a table that was rebuilt.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_log_table_apply_insert(
/*=======================*/
	que_thr_t*		thr,		/*!< in: query graph */
	const mrec_t*		mrec,		/*!< in: record to insert */
	const rec_offs*		offsets,	/*!< in: offsets of mrec */
	mem_heap_t*		offsets_heap,	/*!< in/out: memory heap
						that can be emptied */
	mem_heap_t*		heap,		/*!< in/out: memory heap */
	row_merge_dup_t*	dup)		/*!< in/out: for reporting
						duplicate key errors */
{
	row_log_t*log	= dup->index->online_log;
	dberr_t		error;
	const dtuple_t*	row	= row_log_table_apply_convert_mrec(
		mrec, dup->index, offsets, log, heap, &error);

	switch (error) {
	case DB_SUCCESS:
		ut_ad(row != NULL);
		break;
	default:
		ut_ad(0);
		/* fall through */
	case DB_INVALID_NULL:
		ut_ad(row == NULL);
		return(error);
	}

	error = row_log_table_apply_insert_low(
		thr, row, offsets_heap, heap, dup);
	if (error != DB_SUCCESS) {
		/* Report the erroneous row using the new
		version of the table. */
		innobase_row_to_mysql(dup->table, log->table, row);
	}
	return(error);
}

/******************************************************//**
Deletes a record from a table that is being rebuilt.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_log_table_apply_delete_low(
/*===========================*/
	btr_pcur_t*		pcur,		/*!< in/out: B-tree cursor,
						will be trashed */
	const rec_offs*		offsets,	/*!< in: offsets on pcur */
	mem_heap_t*		heap,		/*!< in/out: memory heap */
	mtr_t*			mtr)		/*!< in/out: mini-transaction,
						will be committed */
{
	dberr_t		error;
	row_ext_t*	ext;
	dtuple_t*	row;
	dict_index_t*	index	= btr_pcur_get_btr_cur(pcur)->index;

	ut_ad(dict_index_is_clust(index));

	DBUG_LOG("ib_alter_table",
		 "delete table " << index->table->id << " (index "
		 << index->id << "): "
		 << rec_printer(btr_pcur_get_rec(pcur), offsets).str());

	if (dict_table_get_next_index(index)) {
		/* Build a row template for purging secondary index entries. */
		row = row_build(
			ROW_COPY_DATA, index, btr_pcur_get_rec(pcur),
			offsets, NULL, NULL, NULL, &ext, heap);
	} else {
		row = NULL;
	}

	btr_cur_pessimistic_delete(&error, FALSE, btr_pcur_get_btr_cur(pcur),
				   BTR_CREATE_FLAG, false, mtr);
	mtr_commit(mtr);

	if (error != DB_SUCCESS) {
		return(error);
	}

	while ((index = dict_table_get_next_index(index)) != NULL) {
		if (index->type & DICT_FTS) {
			continue;
		}

		const dtuple_t*	entry = row_build_index_entry(
			row, ext, index, heap);
		mtr->start();
		index->set_modified(*mtr);
		btr_pcur_open(index, entry, PAGE_CUR_LE,
			      BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE,
			      pcur, mtr);
#ifdef UNIV_DEBUG
		switch (btr_pcur_get_btr_cur(pcur)->flag) {
		case BTR_CUR_DELETE_REF:
		case BTR_CUR_DEL_MARK_IBUF:
		case BTR_CUR_DELETE_IBUF:
		case BTR_CUR_INSERT_TO_IBUF:
			/* We did not request buffering. */
			break;
		case BTR_CUR_HASH:
		case BTR_CUR_HASH_FAIL:
		case BTR_CUR_BINARY:
			goto flag_ok;
		}
		ut_ad(0);
flag_ok:
#endif /* UNIV_DEBUG */

		if (page_rec_is_infimum(btr_pcur_get_rec(pcur))
		    || btr_pcur_get_low_match(pcur) < index->n_uniq) {
			/* All secondary index entries should be
			found, because new_table is being modified by
			this thread only, and all indexes should be
			updated in sync. */
			mtr->commit();
			return(DB_INDEX_CORRUPT);
		}

		btr_cur_pessimistic_delete(&error, FALSE,
					   btr_pcur_get_btr_cur(pcur),
					   BTR_CREATE_FLAG, false, mtr);
		mtr->commit();
	}

	return(error);
}

/******************************************************//**
Replays a delete operation on a table that was rebuilt.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_log_table_apply_delete(
/*=======================*/
	ulint			trx_id_col,	/*!< in: position of
						DB_TRX_ID in the new
						clustered index */
	const mrec_t*		mrec,		/*!< in: merge record */
	const rec_offs*		moffsets,	/*!< in: offsets of mrec */
	mem_heap_t*		offsets_heap,	/*!< in/out: memory heap
						that can be emptied */
	mem_heap_t*		heap,		/*!< in/out: memory heap */
	const row_log_t*	log)		/*!< in: online log */
{
	dict_table_t*	new_table = log->table;
	dict_index_t*	index = dict_table_get_first_index(new_table);
	dtuple_t*	old_pk;
	mtr_t		mtr;
	btr_pcur_t	pcur;
	rec_offs*	offsets;

	ut_ad(rec_offs_n_fields(moffsets) == index->first_user_field());
	ut_ad(!rec_offs_any_extern(moffsets));

	/* Convert the row to a search tuple. */
	old_pk = dtuple_create(heap, index->n_uniq);
	dict_index_copy_types(old_pk, index, index->n_uniq);

	for (ulint i = 0; i < index->n_uniq; i++) {
		ulint		len;
		const void*	field;
		field = rec_get_nth_field(mrec, moffsets, i, &len);
		ut_ad(len != UNIV_SQL_NULL);
		dfield_set_data(dtuple_get_nth_field(old_pk, i),
				field, len);
	}

	mtr_start(&mtr);
	index->set_modified(mtr);
	btr_pcur_open(index, old_pk, PAGE_CUR_LE,
		      BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE,
		      &pcur, &mtr);
#ifdef UNIV_DEBUG
	switch (btr_pcur_get_btr_cur(&pcur)->flag) {
	case BTR_CUR_DELETE_REF:
	case BTR_CUR_DEL_MARK_IBUF:
	case BTR_CUR_DELETE_IBUF:
	case BTR_CUR_INSERT_TO_IBUF:
		/* We did not request buffering. */
		break;
	case BTR_CUR_HASH:
	case BTR_CUR_HASH_FAIL:
	case BTR_CUR_BINARY:
		goto flag_ok;
	}
	ut_ad(0);
flag_ok:
#endif /* UNIV_DEBUG */

	if (page_rec_is_infimum(btr_pcur_get_rec(&pcur))
	    || btr_pcur_get_low_match(&pcur) < index->n_uniq) {
all_done:
		mtr_commit(&mtr);
		/* The record was not found. All done. */
		/* This should only happen when an earlier
		ROW_T_INSERT was skipped or
		ROW_T_UPDATE was interpreted as ROW_T_DELETE
		due to BLOBs having been freed by rollback. */
		return(DB_SUCCESS);
	}

	offsets = rec_get_offsets(btr_pcur_get_rec(&pcur), index, nullptr,
				  index->n_core_fields,
				  ULINT_UNDEFINED, &offsets_heap);
#if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
	ut_a(!rec_offs_any_null_extern(btr_pcur_get_rec(&pcur), offsets));
#endif /* UNIV_DEBUG || UNIV_BLOB_LIGHT_DEBUG */

	/* Only remove the record if DB_TRX_ID,DB_ROLL_PTR match. */

	{
		ulint		len;
		const byte*	mrec_trx_id
			= rec_get_nth_field(mrec, moffsets, trx_id_col, &len);
		ut_ad(len == DATA_TRX_ID_LEN);
		const byte*	rec_trx_id
			= rec_get_nth_field(btr_pcur_get_rec(&pcur), offsets,
					    trx_id_col, &len);
		ut_ad(len == DATA_TRX_ID_LEN);
		ut_d(trx_id_check(rec_trx_id, log->min_trx));
		ut_d(trx_id_check(mrec_trx_id, log->min_trx));

		ut_ad(rec_get_nth_field(mrec, moffsets, trx_id_col + 1, &len)
		      == mrec_trx_id + DATA_TRX_ID_LEN);
		ut_ad(len == DATA_ROLL_PTR_LEN);
		ut_ad(rec_get_nth_field(btr_pcur_get_rec(&pcur), offsets,
					trx_id_col + 1, &len)
		      == rec_trx_id + DATA_TRX_ID_LEN);
		ut_ad(len == DATA_ROLL_PTR_LEN);

		if (memcmp(mrec_trx_id, rec_trx_id,
			   DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN)) {
			/* The ROW_T_DELETE was logged for a different
			PRIMARY KEY,DB_TRX_ID,DB_ROLL_PTR.
			This is possible if a ROW_T_INSERT was skipped
			or a ROW_T_UPDATE was interpreted as ROW_T_DELETE
			because some BLOBs were missing due to
			(1) rolling back the initial insert, or
			(2) purging the BLOB for a later ROW_T_DELETE
			(3) purging 'old values' for a later ROW_T_UPDATE
			or ROW_T_DELETE. */
			ut_ad(!log->same_pk);
			goto all_done;
		}
	}

	return row_log_table_apply_delete_low(&pcur, offsets, heap, &mtr);
}

/******************************************************//**
Replays an update operation on a table that was rebuilt.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_log_table_apply_update(
/*=======================*/
	que_thr_t*		thr,		/*!< in: query graph */
	ulint			new_trx_id_col,	/*!< in: position of
						DB_TRX_ID in the new
						clustered index */
	const mrec_t*		mrec,		/*!< in: new value */
	const rec_offs*		offsets,	/*!< in: offsets of mrec */
	mem_heap_t*		offsets_heap,	/*!< in/out: memory heap
						that can be emptied */
	mem_heap_t*		heap,		/*!< in/out: memory heap */
	row_merge_dup_t*	dup,		/*!< in/out: for reporting
						duplicate key errors */
	const dtuple_t*		old_pk)		/*!< in: PRIMARY KEY and
						DB_TRX_ID,DB_ROLL_PTR
						of the old value,
						or PRIMARY KEY if same_pk */
{
	row_log_t*	log	= dup->index->online_log;
	const dtuple_t*	row;
	dict_index_t*	index	= dict_table_get_first_index(log->table);
	mtr_t		mtr;
	btr_pcur_t	pcur;
	dberr_t		error;
	ulint		n_index = 0;

	ut_ad(dtuple_get_n_fields_cmp(old_pk)
	      == dict_index_get_n_unique(index));
	ut_ad(dtuple_get_n_fields(old_pk) - (log->same_pk ? 0 : 2)
	      == dict_index_get_n_unique(index));

	row = row_log_table_apply_convert_mrec(
		mrec, dup->index, offsets, log, heap, &error);

	switch (error) {
	case DB_SUCCESS:
		ut_ad(row != NULL);
		break;
	default:
		ut_ad(0);
		/* fall through */
	case DB_INVALID_NULL:
		ut_ad(row == NULL);
		return(error);
	}

	mtr_start(&mtr);
	index->set_modified(mtr);
	btr_pcur_open(index, old_pk, PAGE_CUR_LE,
		      BTR_MODIFY_TREE, &pcur, &mtr);
#ifdef UNIV_DEBUG
	switch (btr_pcur_get_btr_cur(&pcur)->flag) {
	case BTR_CUR_DELETE_REF:
	case BTR_CUR_DEL_MARK_IBUF:
	case BTR_CUR_DELETE_IBUF:
	case BTR_CUR_INSERT_TO_IBUF:
		ut_ad(0);/* We did not request buffering. */
	case BTR_CUR_HASH:
	case BTR_CUR_HASH_FAIL:
	case BTR_CUR_BINARY:
		break;
	}
#endif /* UNIV_DEBUG */

	ut_ad(!page_rec_is_infimum(btr_pcur_get_rec(&pcur))
	      && btr_pcur_get_low_match(&pcur) >= index->n_uniq);

	/* Prepare to update (or delete) the record. */
	rec_offs*		cur_offsets	= rec_get_offsets(
		btr_pcur_get_rec(&pcur), index, nullptr, index->n_core_fields,
		ULINT_UNDEFINED, &offsets_heap);

#ifdef UNIV_DEBUG
	if (!log->same_pk) {
		ulint		len;
		const byte*	rec_trx_id
			= rec_get_nth_field(btr_pcur_get_rec(&pcur),
					    cur_offsets, index->n_uniq, &len);
		const dfield_t*	old_pk_trx_id
			= dtuple_get_nth_field(old_pk, index->n_uniq);
		ut_ad(len == DATA_TRX_ID_LEN);
		ut_d(trx_id_check(rec_trx_id, log->min_trx));
		ut_ad(old_pk_trx_id->len == DATA_TRX_ID_LEN);
		ut_ad(old_pk_trx_id[1].len == DATA_ROLL_PTR_LEN);
		ut_ad(DATA_TRX_ID_LEN
		      + static_cast<const char*>(old_pk_trx_id->data)
		      == old_pk_trx_id[1].data);
		ut_d(trx_id_check(old_pk_trx_id->data, log->min_trx));
		ut_ad(!memcmp(rec_trx_id, old_pk_trx_id->data,
			      DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN));
	}
#endif

	dtuple_t*	entry	= row_build_index_entry_low(
		row, NULL, index, heap, ROW_BUILD_NORMAL);
	upd_t*		update	= row_upd_build_difference_binary(
		index, entry, btr_pcur_get_rec(&pcur), cur_offsets,
		false, NULL, heap, dup->table, &error);
	if (error != DB_SUCCESS || !update->n_fields) {
func_exit:
		mtr.commit();
func_exit_committed:
		ut_ad(mtr.has_committed());

		if (error != DB_SUCCESS) {
			/* Report the erroneous row using the new
			version of the table. */
			innobase_row_to_mysql(dup->table, log->table, row);
		}

		return error;
	}

	const bool	pk_updated
		= upd_get_nth_field(update, 0)->field_no < new_trx_id_col;

	if (pk_updated || rec_offs_any_extern(cur_offsets)) {
		/* If the record contains any externally stored
		columns, perform the update by delete and insert,
		because we will not write any undo log that would
		allow purge to free any orphaned externally stored
		columns. */

		if (pk_updated && log->same_pk) {
			/* The ROW_T_UPDATE log record should only be
			written when the PRIMARY KEY fields of the
			record did not change in the old table.  We
			can only get a change of PRIMARY KEY columns
			in the rebuilt table if the PRIMARY KEY was
			redefined (!same_pk). */
			ut_ad(0);
			error = DB_CORRUPTION;
			goto func_exit;
		}

		error = row_log_table_apply_delete_low(
			&pcur, cur_offsets, heap, &mtr);
		ut_ad(mtr.has_committed());

		if (error == DB_SUCCESS) {
			error = row_log_table_apply_insert_low(
				thr, row, offsets_heap, heap, dup);
		}

		goto func_exit_committed;
	}

	dtuple_t*	old_row;
	row_ext_t*	old_ext;

	if (dict_table_get_next_index(index)) {
		/* Construct the row corresponding to the old value of
		the record. */
		old_row = row_build(
			ROW_COPY_DATA, index, btr_pcur_get_rec(&pcur),
			cur_offsets, NULL, NULL, NULL, &old_ext, heap);
		ut_ad(old_row);

		DBUG_LOG("ib_alter_table",
			 "update table " << index->table->id
			 << " (index " << index->id
			 << ": " << rec_printer(old_row).str()
			 << " to " << rec_printer(row).str());
	} else {
		old_row = NULL;
		old_ext = NULL;
	}

	big_rec_t*	big_rec;

	error = btr_cur_pessimistic_update(
		BTR_CREATE_FLAG | BTR_NO_LOCKING_FLAG
		| BTR_NO_UNDO_LOG_FLAG | BTR_KEEP_SYS_FLAG
		| BTR_KEEP_POS_FLAG,
		btr_pcur_get_btr_cur(&pcur),
		&cur_offsets, &offsets_heap, heap, &big_rec,
		update, 0, thr, 0, &mtr);

	if (big_rec) {
		if (error == DB_SUCCESS) {
			error = btr_store_big_rec_extern_fields(
				&pcur, cur_offsets, big_rec, &mtr,
				BTR_STORE_UPDATE);
		}

		dtuple_big_rec_free(big_rec);
	}

	for (n_index += index->type != DICT_CLUSTERED;
	     (index = dict_table_get_next_index(index)); n_index++) {
		if (index->type & DICT_FTS) {
			continue;
		}

		if (error != DB_SUCCESS) {
			break;
		}

		if (!row_upd_changes_ord_field_binary(
			    index, update, thr, old_row, NULL)) {
			continue;
		}

		if (dict_index_has_virtual(index)) {
			dtuple_copy_v_fields(old_row, old_pk);
		}

		mtr_commit(&mtr);

		entry = row_build_index_entry(old_row, old_ext, index, heap);
		if (!entry) {
			ut_ad(0);
			return(DB_CORRUPTION);
		}

		mtr_start(&mtr);
		index->set_modified(mtr);

		if (ROW_FOUND != row_search_index_entry(
			    index, entry, BTR_MODIFY_TREE, &pcur, &mtr)) {
			ut_ad(0);
			error = DB_CORRUPTION;
			break;
		}

		btr_cur_pessimistic_delete(
			&error, FALSE, btr_pcur_get_btr_cur(&pcur),
			BTR_CREATE_FLAG, false, &mtr);

		if (error != DB_SUCCESS) {
			break;
		}

		mtr_commit(&mtr);

		entry = row_build_index_entry(row, NULL, index, heap);
		error = row_ins_sec_index_entry_low(
			BTR_CREATE_FLAG | BTR_NO_LOCKING_FLAG
			| BTR_NO_UNDO_LOG_FLAG | BTR_KEEP_SYS_FLAG,
			BTR_MODIFY_TREE, index, offsets_heap, heap,
			entry, thr_get_trx(thr)->id, thr);

		/* Report correct index name for duplicate key error. */
		if (error == DB_DUPLICATE_KEY) {
			thr_get_trx(thr)->error_key_num = n_index;
		}

		mtr_start(&mtr);
		index->set_modified(mtr);
	}

	goto func_exit;
}

/******************************************************//**
Applies an operation to a table that was rebuilt.
@return NULL on failure (mrec corruption) or when out of data;
pointer to next record on success */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
const mrec_t*
row_log_table_apply_op(
/*===================*/
	que_thr_t*		thr,		/*!< in: query graph */
	ulint			new_trx_id_col,	/*!< in: position of
						DB_TRX_ID in new index */
	row_merge_dup_t*	dup,		/*!< in/out: for reporting
						duplicate key errors */
	dberr_t*		error,		/*!< out: DB_SUCCESS
						or error code */
	mem_heap_t*		offsets_heap,	/*!< in/out: memory heap
						that can be emptied */
	mem_heap_t*		heap,		/*!< in/out: memory heap */
	const mrec_t*		mrec,		/*!< in: merge record */
	const mrec_t*		mrec_end,	/*!< in: end of buffer */
	rec_offs*		offsets)	/*!< in/out: work area
						for parsing mrec */
{
	row_log_t*	log	= dup->index->online_log;
	dict_index_t*	new_index = dict_table_get_first_index(log->table);
	ulint		extra_size;
	const mrec_t*	next_mrec;
	dtuple_t*	old_pk;

	ut_ad(dict_index_is_clust(dup->index));
	ut_ad(dup->index->table != log->table);
	ut_ad(log->head.total <= log->tail.total);

	*error = DB_SUCCESS;

	const bool is_instant = log->is_instant(dup->index);
	const mrec_t* const mrec_start = mrec;

	switch (*mrec++) {
	default:
		ut_ad(0);
		*error = DB_CORRUPTION;
		return(NULL);
	case ROW_T_INSERT:
		extra_size = *mrec++;

		if (extra_size >= 0x80) {
			/* Read another byte of extra_size. */

			extra_size = (extra_size & 0x7f) << 8;
			extra_size |= *mrec++;
		}

		mrec += extra_size;

		ut_ad(extra_size || !is_instant);

		if (mrec > mrec_end) {
			return(NULL);
		}

		rec_offs_set_n_fields(offsets, dup->index->n_fields);
		rec_init_offsets_temp(mrec, dup->index, offsets,
				      log->n_core_fields, log->non_core_fields,
				      is_instant
				      ? static_cast<rec_comp_status_t>(
					      *(mrec - extra_size))
				      : REC_STATUS_ORDINARY);

		next_mrec = mrec + rec_offs_data_size(offsets);

		if (next_mrec > mrec_end) {
			return(NULL);
		} else {
			log->head.total += ulint(next_mrec - mrec_start);
			*error = row_log_table_apply_insert(
				thr, mrec, offsets, offsets_heap,
				heap, dup);
		}
		break;

	case ROW_T_DELETE:
		/* 1 (extra_size) + at least 1 (payload) */
		if (mrec + 2 >= mrec_end) {
			return(NULL);
		}

		extra_size = *mrec++;
		ut_ad(mrec < mrec_end);

		/* We assume extra_size < 0x100 for the PRIMARY KEY prefix.
		For fixed-length PRIMARY key columns, it is 0. */
		mrec += extra_size;

		/* The ROW_T_DELETE record was converted by
		rec_convert_dtuple_to_temp() using new_index. */
		ut_ad(!new_index->is_instant());
		rec_offs_set_n_fields(offsets, new_index->first_user_field());
		rec_init_offsets_temp(mrec, new_index, offsets);
		next_mrec = mrec + rec_offs_data_size(offsets);
		if (next_mrec > mrec_end) {
			return(NULL);
		}

		log->head.total += ulint(next_mrec - mrec_start);

		*error = row_log_table_apply_delete(
			new_trx_id_col,
			mrec, offsets, offsets_heap, heap, log);
		break;

	case ROW_T_UPDATE:
		/* Logically, the log entry consists of the
		(PRIMARY KEY,DB_TRX_ID) of the old value (converted
		to the new primary key definition) followed by
		the new value in the old table definition. If the
		definition of the columns belonging to PRIMARY KEY
		is not changed, the log will only contain
		DB_TRX_ID,new_row. */

		if (log->same_pk) {
			ut_ad(new_index->n_uniq == dup->index->n_uniq);

			extra_size = *mrec++;

			if (extra_size >= 0x80) {
				/* Read another byte of extra_size. */

				extra_size = (extra_size & 0x7f) << 8;
				extra_size |= *mrec++;
			}

			mrec += extra_size;

			ut_ad(extra_size || !is_instant);

			if (mrec > mrec_end) {
				return(NULL);
			}

			rec_offs_set_n_fields(offsets, dup->index->n_fields);
			rec_init_offsets_temp(mrec, dup->index, offsets,
					      log->n_core_fields,
					      log->non_core_fields,
					      is_instant
					      ? static_cast<rec_comp_status_t>(
						      *(mrec - extra_size))
					      : REC_STATUS_ORDINARY);

			next_mrec = mrec + rec_offs_data_size(offsets);

			if (next_mrec > mrec_end) {
				return(NULL);
			}

			old_pk = dtuple_create(heap, new_index->n_uniq);
			dict_index_copy_types(
				old_pk, new_index, old_pk->n_fields);

			/* Copy the PRIMARY KEY fields from mrec to old_pk. */
			for (ulint i = 0; i < new_index->n_uniq; i++) {
				const void*	field;
				ulint		len;
				dfield_t*	dfield;

				ut_ad(!rec_offs_nth_extern(offsets, i));

				field = rec_get_nth_field(
					mrec, offsets, i, &len);
				ut_ad(len != UNIV_SQL_NULL);

				dfield = dtuple_get_nth_field(old_pk, i);
				dfield_set_data(dfield, field, len);
			}
		} else {
			/* We assume extra_size < 0x100
			for the PRIMARY KEY prefix. */
			mrec += *mrec + 1;

			if (mrec > mrec_end) {
				return(NULL);
			}

			/* Get offsets for PRIMARY KEY,
			DB_TRX_ID, DB_ROLL_PTR. */
			/* The old_pk prefix was converted by
			rec_convert_dtuple_to_temp() using new_index. */
			ut_ad(!new_index->is_instant());
			rec_offs_set_n_fields(offsets,
					      new_index->first_user_field());
			rec_init_offsets_temp(mrec, new_index, offsets);

			next_mrec = mrec + rec_offs_data_size(offsets);
			if (next_mrec + 2 > mrec_end) {
				return(NULL);
			}

			/* Copy the PRIMARY KEY fields and
			DB_TRX_ID, DB_ROLL_PTR from mrec to old_pk. */
			old_pk = dtuple_create(heap,
					       new_index->first_user_field());
			dict_index_copy_types(old_pk, new_index,
					      old_pk->n_fields);

			for (ulint i = 0; i < new_index->first_user_field();
			     i++) {
				const void*	field;
				ulint		len;
				dfield_t*	dfield;

				ut_ad(!rec_offs_nth_extern(offsets, i));

				field = rec_get_nth_field(
					mrec, offsets, i, &len);
				ut_ad(len != UNIV_SQL_NULL);

				dfield = dtuple_get_nth_field(old_pk, i);
				dfield_set_data(dfield, field, len);
			}

			mrec = next_mrec;

			/* Fetch the new value of the row as it was
			in the old table definition. */
			extra_size = *mrec++;

			if (extra_size >= 0x80) {
				/* Read another byte of extra_size. */

				extra_size = (extra_size & 0x7f) << 8;
				extra_size |= *mrec++;
			}

			mrec += extra_size;

			ut_ad(extra_size || !is_instant);

			if (mrec > mrec_end) {
				return(NULL);
			}

			rec_offs_set_n_fields(offsets, dup->index->n_fields);
			rec_init_offsets_temp(mrec, dup->index, offsets,
					      log->n_core_fields,
					      log->non_core_fields,
					      is_instant
					      ? static_cast<rec_comp_status_t>(
						      *(mrec - extra_size))
					      : REC_STATUS_ORDINARY);

			next_mrec = mrec + rec_offs_data_size(offsets);

			if (next_mrec > mrec_end) {
				return(NULL);
			}
		}

		ut_ad(next_mrec <= mrec_end);
		log->head.total += ulint(next_mrec - mrec_start);
		dtuple_set_n_fields_cmp(old_pk, new_index->n_uniq);

		*error = row_log_table_apply_update(
			thr, new_trx_id_col,
			mrec, offsets, offsets_heap, heap, dup, old_pk);
		break;
	}

	ut_ad(log->head.total <= log->tail.total);
	mem_heap_empty(offsets_heap);
	mem_heap_empty(heap);
	return(next_mrec);
}

#ifdef HAVE_PSI_STAGE_INTERFACE
/** Estimate how much an ALTER TABLE progress should be incremented per
one block of log applied.
For the other phases of ALTER TABLE we increment the progress with 1 per
page processed.
@return amount of abstract units to add to work_completed when one block
of log is applied.
*/
inline
ulint
row_log_progress_inc_per_block()
{
	/* We must increment the progress once per page (as in
	srv_page_size, default = innodb_page_size=16KiB).
	One block here is srv_sort_buf_size (usually 1MiB). */
	const ulint	pages_per_block = std::max<ulint>(
		ulint(srv_sort_buf_size >> srv_page_size_shift), 1);

	/* Multiply by an artificial factor of 6 to even the pace with
	the rest of the ALTER TABLE phases, they process page_size amount
	of data faster. */
	return(pages_per_block * 6);
}

/** Estimate how much work is to be done by the log apply phase
of an ALTER TABLE for this index.
@param[in]	index	index whose log to assess
@return work to be done by log-apply in abstract units
*/
ulint
row_log_estimate_work(
	const dict_index_t*	index)
{
	if (index == NULL || index->online_log == NULL
	    || index->online_log_is_dummy()) {
		return(0);
	}

	const row_log_t*	l = index->online_log;
	const ulint		bytes_left =
		static_cast<ulint>(l->tail.total - l->head.total);
	const ulint		blocks_left = bytes_left / srv_sort_buf_size;

	return(blocks_left * row_log_progress_inc_per_block());
}
#else /* HAVE_PSI_STAGE_INTERFACE */
inline
ulint
row_log_progress_inc_per_block()
{
	return(0);
}
#endif /* HAVE_PSI_STAGE_INTERFACE */

/** Applies operations to a table was rebuilt.
@param[in]	thr	query graph
@param[in,out]	dup	for reporting duplicate key errors
@param[in,out]	stage	performance schema accounting object, used by
ALTER TABLE. If not NULL, then stage->inc() will be called for each block
of log that is applied.
@return DB_SUCCESS, or error code on failure */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
row_log_table_apply_ops(
	que_thr_t*		thr,
	row_merge_dup_t*	dup,
	ut_stage_alter_t*	stage)
{
	dberr_t		error;
	const mrec_t*	mrec		= NULL;
	const mrec_t*	next_mrec;
	const mrec_t*	mrec_end	= NULL; /* silence bogus warning */
	const mrec_t*	next_mrec_end;
	mem_heap_t*	heap;
	mem_heap_t*	offsets_heap;
	rec_offs*	offsets;
	bool		has_index_lock;
	dict_index_t*	index		= const_cast<dict_index_t*>(
		dup->index);
	dict_table_t*	new_table	= index->online_log->table;
	dict_index_t*	new_index	= dict_table_get_first_index(
		new_table);
	const ulint	i		= 1 + REC_OFFS_HEADER_SIZE
		+ std::max<ulint>(index->n_fields,
				  new_index->first_user_field());
	const ulint	new_trx_id_col	= dict_col_get_clust_pos(
		dict_table_get_sys_col(new_table, DATA_TRX_ID), new_index);
	trx_t*		trx		= thr_get_trx(thr);

	ut_ad(dict_index_is_clust(index));
	ut_ad(dict_index_is_online_ddl(index));
	ut_ad(trx->mysql_thd);
	ut_ad(index->lock.have_x());
	ut_ad(!dict_index_is_online_ddl(new_index));
	ut_ad(dict_col_get_clust_pos(
		      dict_table_get_sys_col(index->table, DATA_TRX_ID), index)
	      != ULINT_UNDEFINED);
	ut_ad(new_trx_id_col > 0);
	ut_ad(new_trx_id_col != ULINT_UNDEFINED);

	MEM_UNDEFINED(&mrec_end, sizeof mrec_end);

	offsets = static_cast<rec_offs*>(ut_malloc_nokey(i * sizeof *offsets));
	rec_offs_set_n_alloc(offsets, i);
	rec_offs_set_n_fields(offsets, dict_index_get_n_fields(index));

	heap = mem_heap_create(srv_page_size);
	offsets_heap = mem_heap_create(srv_page_size);
	has_index_lock = true;

next_block:
	ut_ad(has_index_lock);
	ut_ad(index->lock.have_u_or_x());
	ut_ad(index->online_log->head.bytes == 0);

	stage->inc(row_log_progress_inc_per_block());

	if (trx_is_interrupted(trx)) {
		goto interrupted;
	}

	if (index->is_corrupted()) {
		error = DB_INDEX_CORRUPT;
		goto func_exit;
	}

	ut_ad(dict_index_is_online_ddl(index));

	error = index->online_log->error;

	if (error != DB_SUCCESS) {
		goto func_exit;
	}

	if (UNIV_UNLIKELY(index->online_log->head.blocks
			  > index->online_log->tail.blocks)) {
unexpected_eof:
		ib::error() << "Unexpected end of temporary file for table "
			<< index->table->name;
corruption:
		error = DB_CORRUPTION;
		goto func_exit;
	}

	if (index->online_log->head.blocks
	    == index->online_log->tail.blocks) {
		if (index->online_log->head.blocks) {
#ifdef HAVE_FTRUNCATE
			/* Truncate the file in order to save space. */
			if (index->online_log->fd > 0
			    && ftruncate(index->online_log->fd, 0) == -1) {
				ib::error()
					<< "\'" << index->name + 1
					<< "\' failed with error "
					<< errno << ":" << strerror(errno);

				goto corruption;
			}
#endif /* HAVE_FTRUNCATE */
			index->online_log->head.blocks
				= index->online_log->tail.blocks = 0;
		}

		next_mrec = index->online_log->tail.block;
		next_mrec_end = next_mrec + index->online_log->tail.bytes;

		if (next_mrec_end == next_mrec) {
			/* End of log reached. */
all_done:
			ut_ad(has_index_lock);
			ut_ad(index->online_log->head.blocks == 0);
			ut_ad(index->online_log->tail.blocks == 0);
			index->online_log->head.bytes = 0;
			index->online_log->tail.bytes = 0;
			error = DB_SUCCESS;
			goto func_exit;
		}
	} else {
		os_offset_t	ofs;

		ofs = (os_offset_t) index->online_log->head.blocks
			* srv_sort_buf_size;

		ut_ad(has_index_lock);
		has_index_lock = false;
		index->lock.x_unlock();

		log_free_check();

		ut_ad(dict_index_is_online_ddl(index));

		if (!row_log_block_allocate(index->online_log->head)) {
			error = DB_OUT_OF_MEMORY;
			goto func_exit;
		}

		byte*			buf = index->online_log->head.block;

		if (os_file_read_no_error_handling(
			    IORequestRead, index->online_log->fd,
			    buf, ofs, srv_sort_buf_size, 0) != DB_SUCCESS) {
			ib::error()
				<< "Unable to read temporary file"
				" for table " << index->table->name;
			goto corruption;
		}

		if (srv_encrypt_log) {
			if (!log_tmp_block_decrypt(
				    buf, srv_sort_buf_size,
				    index->online_log->crypt_head, ofs)) {
				error = DB_DECRYPTION_FAILED;
				goto func_exit;
			}

			srv_stats.n_rowlog_blocks_decrypted.inc();
			memcpy(buf, index->online_log->crypt_head,
			       srv_sort_buf_size);
		}

#ifdef POSIX_FADV_DONTNEED
		/* Each block is read exactly once.  Free up the file cache. */
		posix_fadvise(index->online_log->fd,
			      ofs, srv_sort_buf_size, POSIX_FADV_DONTNEED);
#endif /* POSIX_FADV_DONTNEED */

		next_mrec = index->online_log->head.block;
		next_mrec_end = next_mrec + srv_sort_buf_size;
	}

	/* This read is not protected by index->online_log->mutex for
	performance reasons. We will eventually notice any error that
	was flagged by a DML thread. */
	error = index->online_log->error;

	if (error != DB_SUCCESS) {
		goto func_exit;
	}

	if (mrec) {
		/* A partial record was read from the previous block.
		Copy the temporary buffer full, as we do not know the
		length of the record. Parse subsequent records from
		the bigger buffer index->online_log->head.block
		or index->online_log->tail.block. */

		ut_ad(mrec == index->online_log->head.buf);
		ut_ad(mrec_end > mrec);
		ut_ad(mrec_end < (&index->online_log->head.buf)[1]);

		memcpy((mrec_t*) mrec_end, next_mrec,
		       ulint((&index->online_log->head.buf)[1] - mrec_end));
		mrec = row_log_table_apply_op(
			thr, new_trx_id_col,
			dup, &error, offsets_heap, heap,
			index->online_log->head.buf,
			(&index->online_log->head.buf)[1], offsets);
		if (error != DB_SUCCESS) {
			goto func_exit;
		} else if (UNIV_UNLIKELY(mrec == NULL)) {
			/* The record was not reassembled properly. */
			goto corruption;
		}
		/* The record was previously found out to be
		truncated. Now that the parse buffer was extended,
		it should proceed beyond the old end of the buffer. */
		ut_a(mrec > mrec_end);

		index->online_log->head.bytes = ulint(mrec - mrec_end);
		next_mrec += index->online_log->head.bytes;
	}

	ut_ad(next_mrec <= next_mrec_end);
	/* The following loop must not be parsing the temporary
	buffer, but head.block or tail.block. */

	/* mrec!=NULL means that the next record starts from the
	middle of the block */
	ut_ad((mrec == NULL) == (index->online_log->head.bytes == 0));

#ifdef UNIV_DEBUG
	if (next_mrec_end == index->online_log->head.block
	    + srv_sort_buf_size) {
		/* If tail.bytes == 0, next_mrec_end can also be at
		the end of tail.block. */
		if (index->online_log->tail.bytes == 0) {
			ut_ad(next_mrec == next_mrec_end);
			ut_ad(index->online_log->tail.blocks == 0);
			ut_ad(index->online_log->head.blocks == 0);
			ut_ad(index->online_log->head.bytes == 0);
		} else {
			ut_ad(next_mrec == index->online_log->head.block
			      + index->online_log->head.bytes);
			ut_ad(index->online_log->tail.blocks
			      > index->online_log->head.blocks);
		}
	} else if (next_mrec_end == index->online_log->tail.block
		   + index->online_log->tail.bytes) {
		ut_ad(next_mrec == index->online_log->tail.block
		      + index->online_log->head.bytes);
		ut_ad(index->online_log->tail.blocks == 0);
		ut_ad(index->online_log->head.blocks == 0);
		ut_ad(index->online_log->head.bytes
		      <= index->online_log->tail.bytes);
	} else {
		ut_error;
	}
#endif /* UNIV_DEBUG */

	mrec_end = next_mrec_end;

	while (!trx_is_interrupted(trx)) {
		mrec = next_mrec;
		ut_ad(mrec <= mrec_end);

		if (mrec == mrec_end) {
			/* We are at the end of the log.
			   Mark the replay all_done. */
			if (has_index_lock) {
				goto all_done;
			}
		}

		if (!has_index_lock) {
			/* We are applying operations from a different
			block than the one that is being written to.
			We do not hold index->lock in order to
			allow other threads to concurrently buffer
			modifications. */
			ut_ad(mrec >= index->online_log->head.block);
			ut_ad(mrec_end == index->online_log->head.block
			      + srv_sort_buf_size);
			ut_ad(index->online_log->head.bytes
			      < srv_sort_buf_size);

			/* Take the opportunity to do a redo log
			checkpoint if needed. */
			log_free_check();
		} else {
			/* We are applying operations from the last block.
			Do not allow other threads to buffer anything,
			so that we can finally catch up and synchronize. */
			ut_ad(index->online_log->head.blocks == 0);
			ut_ad(index->online_log->tail.blocks == 0);
			ut_ad(mrec_end == index->online_log->tail.block
			      + index->online_log->tail.bytes);
			ut_ad(mrec >= index->online_log->tail.block);
		}

		/* This read is not protected by index->online_log->mutex
		for performance reasons. We will eventually notice any
		error that was flagged by a DML thread. */
		error = index->online_log->error;

		if (error != DB_SUCCESS) {
			goto func_exit;
		}

		next_mrec = row_log_table_apply_op(
			thr, new_trx_id_col,
			dup, &error, offsets_heap, heap,
			mrec, mrec_end, offsets);

		if (error != DB_SUCCESS) {
			goto func_exit;
		} else if (next_mrec == next_mrec_end) {
			/* The record happened to end on a block boundary.
			Do we have more blocks left? */
			if (has_index_lock) {
				/* The index will be locked while
				applying the last block. */
				goto all_done;
			}

			mrec = NULL;
process_next_block:
			index->lock.x_lock(SRW_LOCK_CALL);
			has_index_lock = true;

			index->online_log->head.bytes = 0;
			index->online_log->head.blocks++;
			goto next_block;
		} else if (next_mrec != NULL) {
			ut_ad(next_mrec < next_mrec_end);
			index->online_log->head.bytes
				+= ulint(next_mrec - mrec);
		} else if (has_index_lock) {
			/* When mrec is within tail.block, it should
			be a complete record, because we are holding
			index->lock and thus excluding the writer. */
			ut_ad(index->online_log->tail.blocks == 0);
			ut_ad(mrec_end == index->online_log->tail.block
			      + index->online_log->tail.bytes);
			ut_ad(0);
			goto unexpected_eof;
		} else {
			memcpy(index->online_log->head.buf, mrec,
			       ulint(mrec_end - mrec));
			mrec_end += ulint(index->online_log->head.buf - mrec);
			mrec = index->online_log->head.buf;
			goto process_next_block;
		}
	}

interrupted:
	error = DB_INTERRUPTED;
func_exit:
	if (!has_index_lock) {
		index->lock.x_lock(SRW_LOCK_CALL);
	}

	mem_heap_free(offsets_heap);
	mem_heap_free(heap);
	row_log_block_free(index->online_log->head);
	ut_free(offsets);
	return(error);
}

/** Apply the row_log_table log to a table upon completing rebuild.
@param[in]	thr		query graph
@param[in]	old_table	old table
@param[in,out]	table		MySQL table (for reporting duplicates)
@param[in,out]	stage		performance schema accounting object, used by
ALTER TABLE. stage->begin_phase_log_table() will be called initially and then
stage->inc() will be called for each block of log that is applied.
@param[in]	new_table	Altered table
@return DB_SUCCESS, or error code on failure */
dberr_t
row_log_table_apply(
	que_thr_t*		thr,
	dict_table_t*		old_table,
	struct TABLE*		table,
	ut_stage_alter_t*	stage,
	dict_table_t*		new_table)
{
	dberr_t		error;
	dict_index_t*	clust_index;

	thr_get_trx(thr)->error_key_num = 0;
	DBUG_EXECUTE_IF("innodb_trx_duplicates",
			thr_get_trx(thr)->duplicates = TRX_DUP_REPLACE;);

	stage->begin_phase_log_table();

	clust_index = dict_table_get_first_index(old_table);

	if (clust_index->online_log->n_rows == 0) {
		clust_index->online_log->n_rows = new_table->stat_n_rows;
	}

	clust_index->lock.x_lock(SRW_LOCK_CALL);

	if (!clust_index->online_log) {
		ut_ad(dict_index_get_online_status(clust_index)
		      == ONLINE_INDEX_COMPLETE);
		/* This function should not be called unless
		rebuilding a table online. Build in some fault
		tolerance. */
		ut_ad(0);
		error = DB_ERROR;
	} else {
		row_merge_dup_t	dup = {
			clust_index, table,
			clust_index->online_log->col_map, 0
		};

		error = row_log_table_apply_ops(thr, &dup, stage);

		ut_ad(error != DB_SUCCESS
		      || clust_index->online_log->head.total
		      == clust_index->online_log->tail.total);
	}

	clust_index->lock.x_unlock();
	DBUG_EXECUTE_IF("innodb_trx_duplicates",
			thr_get_trx(thr)->duplicates = 0;);

	return(error);
}

/******************************************************//**
Allocate the row log for an index and flag the index
for online creation.
@retval true if success, false if not */
bool
row_log_allocate(
/*=============*/
	const trx_t*	trx,	/*!< in: the ALTER TABLE transaction */
	dict_index_t*	index,	/*!< in/out: index */
	dict_table_t*	table,	/*!< in/out: new table being rebuilt,
				or NULL when creating a secondary index */
	bool		same_pk,/*!< in: whether the definition of the
				PRIMARY KEY has remained the same */
	const dtuple_t*	defaults,
				/*!< in: default values of
				added, changed columns, or NULL */
	const ulint*	col_map,/*!< in: mapping of old column
				numbers to new ones, or NULL if !table */
	const char*	path,	/*!< in: where to create temporary file */
	const TABLE*	old_table,	/*!< in: table definition before alter */
	const bool	allow_not_null) /*!< in: allow null to not-null
					conversion */
{
	row_log_t*	log;
	DBUG_ENTER("row_log_allocate");

	ut_ad(!dict_index_is_online_ddl(index));
	ut_ad(dict_index_is_clust(index) == !!table);
	ut_ad(!table || index->table != table);
	ut_ad(same_pk || table);
	ut_ad(!table || col_map);
	ut_ad(!defaults || col_map);
	ut_ad(index->lock.have_u_or_x());
	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE));
	ut_ad(trx->id);

	log = static_cast<row_log_t*>(ut_malloc_nokey(sizeof *log));

	if (log == NULL) {
		DBUG_RETURN(false);
	}

	log->fd = OS_FILE_CLOSED;
	mysql_mutex_init(index_online_log_key, &log->mutex, nullptr);

	log->table = table;
	log->same_pk = same_pk;
	log->defaults = defaults;
	log->col_map = col_map;
	log->error = DB_SUCCESS;
	log->min_trx = trx->id;
	log->max_trx = 0;
	log->tail.blocks = log->tail.bytes = 0;
	log->tail.total = 0;
	log->tail.block = log->head.block = NULL;
	log->crypt_tail = log->crypt_head = NULL;
	log->head.blocks = log->head.bytes = 0;
	log->head.total = 0;
	log->path = path;
	log->n_core_fields = index->n_core_fields;
	ut_ad(!table || log->is_instant(index)
	      == (index->n_core_fields < index->n_fields));
	log->allow_not_null = allow_not_null;
	log->old_table = old_table;
	log->n_rows = 0;

	if (table && index->is_instant()) {
		const unsigned n = log->n_core_fields;
		log->non_core_fields = UT_NEW_ARRAY_NOKEY(
			dict_col_t::def_t, index->n_fields - n);
		for (unsigned i = n; i < index->n_fields; i++) {
			log->non_core_fields[i - n]
				= index->fields[i].col->def_val;
		}
	} else {
		log->non_core_fields = NULL;
	}

	dict_index_set_online_status(index, ONLINE_INDEX_CREATION);

	if (srv_encrypt_log) {
		log->crypt_head_size = log->crypt_tail_size = srv_sort_buf_size;
		log->crypt_head = static_cast<byte *>(
			my_large_malloc(&log->crypt_head_size, MYF(MY_WME)));
		log->crypt_tail = static_cast<byte *>(
			my_large_malloc(&log->crypt_tail_size, MYF(MY_WME)));

		if (!log->crypt_head || !log->crypt_tail) {
			row_log_free(log);
			DBUG_RETURN(false);
		}
	}

	index->online_log = log;

	if (!table) {
		/* Assign the clustered index online log to table.
		It can be used by concurrent DML to identify whether
		the table has any online DDL */
		index->table->indexes.start->online_log_make_dummy();
		log->alter_trx = trx;
	}

	/* While we might be holding an exclusive data dictionary lock
	here, in row_log_abort_sec() we will not always be holding it. Use
	atomic operations in both cases. */
	MONITOR_ATOMIC_INC(MONITOR_ONLINE_CREATE_INDEX);

	DBUG_RETURN(true);
}

/******************************************************//**
Free the row log for an index that was being created online. */
void
row_log_free(
/*=========*/
	row_log_t*	log)	/*!< in,own: row log */
{
	MONITOR_ATOMIC_DEC(MONITOR_ONLINE_CREATE_INDEX);

	UT_DELETE_ARRAY(log->non_core_fields);
	row_log_block_free(log->tail);
	row_log_block_free(log->head);
	row_merge_file_destroy_low(log->fd);

	if (log->crypt_head) {
		my_large_free(log->crypt_head, log->crypt_head_size);
	}

	if (log->crypt_tail) {
		my_large_free(log->crypt_tail, log->crypt_tail_size);
	}

	mysql_mutex_destroy(&log->mutex);
	ut_free(log);
}

/******************************************************//**
Get the latest transaction ID that has invoked row_log_online_op()
during online creation.
@return latest transaction ID, or 0 if nothing was logged */
trx_id_t
row_log_get_max_trx(
/*================*/
	dict_index_t*	index)	/*!< in: index, must be locked */
{
	ut_ad(dict_index_get_online_status(index) == ONLINE_INDEX_CREATION);
#ifdef SAFE_MUTEX
	ut_ad(index->lock.have_x()
	      || (index->lock.have_s()
		  && mysql_mutex_is_owner(&index->online_log->mutex)));
#endif
	return(index->online_log->max_trx);
}

/******************************************************//**
Applies an operation to a secondary index that was being created. */
static MY_ATTRIBUTE((nonnull))
void
row_log_apply_op_low(
/*=================*/
	dict_index_t*	index,		/*!< in/out: index */
	row_merge_dup_t*dup,		/*!< in/out: for reporting
					duplicate key errors */
	dberr_t*	error,		/*!< out: DB_SUCCESS or error code */
	mem_heap_t*	offsets_heap,	/*!< in/out: memory heap for
					allocating offsets; can be emptied */
	bool		has_index_lock, /*!< in: true if holding index->lock
					in exclusive mode */
	enum row_op	op,		/*!< in: operation being applied */
	trx_id_t	trx_id,		/*!< in: transaction identifier */
	const dtuple_t*	entry)		/*!< in: row */
{
	mtr_t		mtr;
	btr_cur_t	cursor;
	rec_offs*	offsets = NULL;

	ut_ad(!dict_index_is_clust(index));

	ut_ad(index->lock.have_x() == has_index_lock);

	ut_ad(!index->is_corrupted());
	ut_ad(trx_id != 0 || op == ROW_OP_DELETE);

	DBUG_LOG("ib_create_index",
		 (op == ROW_OP_INSERT ? "insert " : "delete ")
		 << (has_index_lock ? "locked index " : "unlocked index ")
		 << index->id << ',' << ib::hex(trx_id) << ": "
		 << rec_printer(entry).str());

	mtr_start(&mtr);
	index->set_modified(mtr);

	/* We perform the pessimistic variant of the operations if we
	already hold index->lock exclusively. First, search the
	record. The operation may already have been performed,
	depending on when the row in the clustered index was
	scanned. */
	btr_cur_search_to_nth_level(index, 0, entry, PAGE_CUR_LE,
				    has_index_lock
				    ? BTR_MODIFY_TREE
				    : BTR_MODIFY_LEAF,
				    &cursor, 0, &mtr);

	ut_ad(dict_index_get_n_unique(index) > 0);
	/* This test is somewhat similar to row_ins_must_modify_rec(),
	but not identical for unique secondary indexes. */
	if (cursor.low_match >= dict_index_get_n_unique(index)
	    && !page_rec_is_infimum(btr_cur_get_rec(&cursor))) {
		/* We have a matching record. */
		bool	exists	= (cursor.low_match
				   == dict_index_get_n_fields(index));
#ifdef UNIV_DEBUG
		rec_t*	rec	= btr_cur_get_rec(&cursor);
		ut_ad(page_rec_is_user_rec(rec));
		ut_ad(!rec_get_deleted_flag(rec, page_rec_is_comp(rec)));
#endif /* UNIV_DEBUG */

		ut_ad(exists || dict_index_is_unique(index));

		switch (op) {
		case ROW_OP_DELETE:
			if (!exists) {
				/* The existing record matches the
				unique secondary index key, but the
				PRIMARY KEY columns differ. So, this
				exact record does not exist. For
				example, we could detect a duplicate
				key error in some old index before
				logging an ROW_OP_INSERT for our
				index. This ROW_OP_DELETE could have
				been logged for rolling back
				TRX_UNDO_INSERT_REC. */
				goto func_exit;
			}

			if (btr_cur_optimistic_delete(
				    &cursor, BTR_CREATE_FLAG, &mtr)) {
				*error = DB_SUCCESS;
				break;
			}

			if (!has_index_lock) {
				/* This needs a pessimistic operation.
				Lock the index tree exclusively. */
				mtr_commit(&mtr);
				mtr_start(&mtr);
				index->set_modified(mtr);
				btr_cur_search_to_nth_level(
					index, 0, entry, PAGE_CUR_LE,
					BTR_MODIFY_TREE, &cursor, 0, &mtr);

				/* No other thread than the current one
				is allowed to modify the index tree.
				Thus, the record should still exist. */
				ut_ad(cursor.low_match
				      >= dict_index_get_n_fields(index));
				ut_ad(page_rec_is_user_rec(
					      btr_cur_get_rec(&cursor)));
			}

			/* As there are no externally stored fields in
			a secondary index record, the parameter
			rollback=false will be ignored. */

			btr_cur_pessimistic_delete(
				error, FALSE, &cursor,
				BTR_CREATE_FLAG, false, &mtr);
			break;
		case ROW_OP_INSERT:
			if (exists) {
				/* The record already exists. There
				is nothing to be inserted.
				This could happen when processing
				TRX_UNDO_DEL_MARK_REC in statement
				rollback:

				UPDATE of PRIMARY KEY can lead to
				statement rollback if the updated
				value of the PRIMARY KEY already
				exists. In this case, the UPDATE would
				be mapped to DELETE;INSERT, and we
				only wrote undo log for the DELETE
				part. The duplicate key error would be
				triggered before logging the INSERT
				part.

				Theoretically, we could also get a
				similar situation when a DELETE operation
				is blocked by a FOREIGN KEY constraint. */
				goto func_exit;
			}

			if (dtuple_contains_null(entry)) {
				/* The UNIQUE KEY columns match, but
				there is a NULL value in the key, and
				NULL!=NULL. */
				goto insert_the_rec;
			}

			goto duplicate;
		}
	} else {
		switch (op) {
			rec_t*		rec;
			big_rec_t*	big_rec;
		case ROW_OP_DELETE:
			/* The record does not exist. For example, we
			could detect a duplicate key error in some old
			index before logging an ROW_OP_INSERT for our
			index. This ROW_OP_DELETE could be logged for
			rolling back TRX_UNDO_INSERT_REC. */
			goto func_exit;
		case ROW_OP_INSERT:
			if (dict_index_is_unique(index)
			    && (cursor.up_match
				>= dict_index_get_n_unique(index)
				|| cursor.low_match
				>= dict_index_get_n_unique(index))
			    && (!index->n_nullable
				|| !dtuple_contains_null(entry))) {
duplicate:
				/* Duplicate key */
				ut_ad(dict_index_is_unique(index));
				row_merge_dup_report(dup, entry->fields);
				*error = DB_DUPLICATE_KEY;
				goto func_exit;
			}
insert_the_rec:
			/* Insert the record. As we are inserting into
			a secondary index, there cannot be externally
			stored columns (!big_rec). */
			*error = btr_cur_optimistic_insert(
				BTR_NO_UNDO_LOG_FLAG
				| BTR_NO_LOCKING_FLAG
				| BTR_CREATE_FLAG,
				&cursor, &offsets, &offsets_heap,
				const_cast<dtuple_t*>(entry),
				&rec, &big_rec, 0, NULL, &mtr);
			ut_ad(!big_rec);
			if (*error != DB_FAIL) {
				break;
			}

			if (!has_index_lock) {
				/* This needs a pessimistic operation.
				Lock the index tree exclusively. */
				mtr_commit(&mtr);
				mtr_start(&mtr);
				index->set_modified(mtr);
				btr_cur_search_to_nth_level(
					index, 0, entry, PAGE_CUR_LE,
					BTR_MODIFY_TREE, &cursor, 0, &mtr);
			}

			/* We already determined that the
			record did not exist. No other thread
			than the current one is allowed to
			modify the index tree. Thus, the
			record should still not exist. */

			*error = btr_cur_pessimistic_insert(
				BTR_NO_UNDO_LOG_FLAG
				| BTR_NO_LOCKING_FLAG
				| BTR_CREATE_FLAG,
				&cursor, &offsets, &offsets_heap,
				const_cast<dtuple_t*>(entry),
				&rec, &big_rec,
				0, NULL, &mtr);
			ut_ad(!big_rec);
			break;
		}
		mem_heap_empty(offsets_heap);
	}

	if (*error == DB_SUCCESS && trx_id) {
		page_update_max_trx_id(btr_cur_get_block(&cursor),
				       btr_cur_get_page_zip(&cursor),
				       trx_id, &mtr);
	}

func_exit:
	mtr_commit(&mtr);
}

/******************************************************//**
Applies an operation to a secondary index that was being created.
@return NULL on failure (mrec corruption) or when out of data;
pointer to next record on success */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
const mrec_t*
row_log_apply_op(
/*=============*/
	dict_index_t*	index,		/*!< in/out: index */
	row_merge_dup_t*dup,		/*!< in/out: for reporting
					duplicate key errors */
	dberr_t*	error,		/*!< out: DB_SUCCESS or error code */
	mem_heap_t*	offsets_heap,	/*!< in/out: memory heap for
					allocating offsets; can be emptied */
	mem_heap_t*	heap,		/*!< in/out: memory heap for
					allocating data tuples */
	bool		has_index_lock, /*!< in: true if holding index->lock
					in exclusive mode */
	const mrec_t*	mrec,		/*!< in: merge record */
	const mrec_t*	mrec_end,	/*!< in: end of buffer */
	rec_offs*	offsets)	/*!< in/out: work area for
					rec_init_offsets_temp() */

{
	enum row_op	op;
	ulint		extra_size;
	ulint		data_size;
	dtuple_t*	entry;
	trx_id_t	trx_id;

	/* Online index creation is only used for secondary indexes. */
	ut_ad(!dict_index_is_clust(index));

	ut_ad(index->lock.have_x() == has_index_lock);

	if (index->is_corrupted()) {
		*error = DB_INDEX_CORRUPT;
		return(NULL);
	}

	*error = DB_SUCCESS;

	if (mrec + ROW_LOG_HEADER_SIZE >= mrec_end) {
		return(NULL);
	}

	switch (*mrec) {
	case ROW_OP_INSERT:
		if (ROW_LOG_HEADER_SIZE + DATA_TRX_ID_LEN + mrec >= mrec_end) {
			return(NULL);
		}

		op = static_cast<enum row_op>(*mrec++);
		trx_id = trx_read_trx_id(mrec);
		mrec += DATA_TRX_ID_LEN;
		break;
	case ROW_OP_DELETE:
		op = static_cast<enum row_op>(*mrec++);
		trx_id = 0;
		break;
	default:
corrupted:
		ut_ad(0);
		*error = DB_CORRUPTION;
		return(NULL);
	}

	extra_size = *mrec++;

	ut_ad(mrec < mrec_end);

	if (extra_size >= 0x80) {
		/* Read another byte of extra_size. */

		extra_size = (extra_size & 0x7f) << 8;
		extra_size |= *mrec++;
	}

	mrec += extra_size;

	if (mrec > mrec_end) {
		return(NULL);
	}

	rec_init_offsets_temp(mrec, index, offsets);

	if (rec_offs_any_extern(offsets)) {
		/* There should never be any externally stored fields
		in a secondary index, which is what online index
		creation is used for. Therefore, the log file must be
		corrupted. */
		goto corrupted;
	}

	data_size = rec_offs_data_size(offsets);

	mrec += data_size;

	if (mrec > mrec_end) {
		return(NULL);
	}

	entry = row_rec_to_index_entry_low(
		mrec - data_size, index, offsets, heap);
	/* Online index creation is only implemented for secondary
	indexes, which never contain off-page columns. */
	ut_ad(dtuple_get_n_ext(entry) == 0);

	row_log_apply_op_low(index, dup, error, offsets_heap,
			     has_index_lock, op, trx_id, entry);
	return(mrec);
}

/** Applies operations to a secondary index that was being created.
@param[in]	trx	transaction (for checking if the operation was
interrupted)
@param[in,out]	index	index
@param[in,out]	dup	for reporting duplicate key errors
@param[in,out]	stage	performance schema accounting object, used by
ALTER TABLE. If not NULL, then stage->inc() will be called for each block
of log that is applied or nullptr when row log applied done by DML
thread.
@return DB_SUCCESS, or error code on failure */
static
dberr_t
row_log_apply_ops(
	const trx_t*		trx,
	dict_index_t*		index,
	row_merge_dup_t*	dup,
	ut_stage_alter_t*	stage)
{
	dberr_t		error;
	const mrec_t*	mrec	= NULL;
	const mrec_t*	next_mrec;
	const mrec_t*	mrec_end= NULL; /* silence bogus warning */
	const mrec_t*	next_mrec_end;
	mem_heap_t*	offsets_heap;
	mem_heap_t*	heap;
	rec_offs*	offsets;
	bool		has_index_lock;
	const ulint	i	= 1 + REC_OFFS_HEADER_SIZE
		+ dict_index_get_n_fields(index);

	ut_ad(dict_index_is_online_ddl(index)
	      || (index->online_log
		  && index->online_status == ONLINE_INDEX_COMPLETE));
	ut_ad(!index->is_committed());
	ut_ad(index->lock.have_x());
	ut_ad(index->online_log);

	MEM_UNDEFINED(&mrec_end, sizeof mrec_end);

	offsets = static_cast<rec_offs*>(ut_malloc_nokey(i * sizeof *offsets));
	rec_offs_set_n_alloc(offsets, i);
	rec_offs_set_n_fields(offsets, dict_index_get_n_fields(index));

	offsets_heap = mem_heap_create(srv_page_size);
	heap = mem_heap_create(srv_page_size);
	has_index_lock = true;

next_block:
	ut_ad(has_index_lock);
	ut_ad(index->lock.have_x());
	ut_ad(index->online_log->head.bytes == 0);

	if (stage) {
		stage->inc(row_log_progress_inc_per_block());
	}

	if (trx_is_interrupted(trx)) {
		goto interrupted;
	}

	error = index->online_log->error;
	if (error != DB_SUCCESS) {
		goto func_exit;
	}

	if (index->is_corrupted()) {
		error = DB_INDEX_CORRUPT;
		goto func_exit;
	}

	if (UNIV_UNLIKELY(index->online_log->head.blocks
			  > index->online_log->tail.blocks)) {
unexpected_eof:
		ib::error() << "Unexpected end of temporary file for index "
			<< index->name;
corruption:
		error = DB_CORRUPTION;
		goto func_exit;
	}

	if (index->online_log->head.blocks
	    == index->online_log->tail.blocks) {
		if (index->online_log->head.blocks) {
#ifdef HAVE_FTRUNCATE
			/* Truncate the file in order to save space. */
			if (index->online_log->fd > 0
			    && ftruncate(index->online_log->fd, 0) == -1) {
				ib::error()
					<< "\'" << index->name + 1
					<< "\' failed with error "
					<< errno << ":" << strerror(errno);

				goto corruption;
			}
#endif /* HAVE_FTRUNCATE */
			index->online_log->head.blocks
				= index->online_log->tail.blocks = 0;
		}

		next_mrec = index->online_log->tail.block;
		next_mrec_end = next_mrec + index->online_log->tail.bytes;

		if (next_mrec_end == next_mrec) {
			/* End of log reached. */
all_done:
			ut_ad(has_index_lock);
			ut_ad(index->online_log->head.blocks == 0);
			ut_ad(index->online_log->tail.blocks == 0);
			index->online_log->tail.bytes = 0;
			index->online_log->head.bytes = 0;
			error = DB_SUCCESS;
			goto func_exit;
		}
	} else {
		os_offset_t	ofs = static_cast<os_offset_t>(
			index->online_log->head.blocks)
			* srv_sort_buf_size;
		ut_ad(has_index_lock);
		has_index_lock = false;
		index->lock.x_unlock();

		log_free_check();

		if (!row_log_block_allocate(index->online_log->head)) {
			error = DB_OUT_OF_MEMORY;
			goto func_exit;
		}

		byte*	buf = index->online_log->head.block;

		if (os_file_read_no_error_handling(
			    IORequestRead, index->online_log->fd,
			    buf, ofs, srv_sort_buf_size, 0) != DB_SUCCESS) {
			ib::error()
				<< "Unable to read temporary file"
				" for index " << index->name;
			goto corruption;
		}

		if (srv_encrypt_log) {
			if (!log_tmp_block_decrypt(
				    buf, srv_sort_buf_size,
				    index->online_log->crypt_head, ofs)) {
				error = DB_DECRYPTION_FAILED;
				goto func_exit;
			}

			srv_stats.n_rowlog_blocks_decrypted.inc();
			memcpy(buf, index->online_log->crypt_head, srv_sort_buf_size);
		}

#ifdef POSIX_FADV_DONTNEED
		/* Each block is read exactly once.  Free up the file cache. */
		posix_fadvise(index->online_log->fd,
			      ofs, srv_sort_buf_size, POSIX_FADV_DONTNEED);
#endif /* POSIX_FADV_DONTNEED */

		next_mrec = index->online_log->head.block;
		next_mrec_end = next_mrec + srv_sort_buf_size;
	}

	if (mrec) {
		/* A partial record was read from the previous block.
		Copy the temporary buffer full, as we do not know the
		length of the record. Parse subsequent records from
		the bigger buffer index->online_log->head.block
		or index->online_log->tail.block. */

		ut_ad(mrec == index->online_log->head.buf);
		ut_ad(mrec_end > mrec);
		ut_ad(mrec_end < (&index->online_log->head.buf)[1]);

		memcpy((mrec_t*) mrec_end, next_mrec,
		       ulint((&index->online_log->head.buf)[1] - mrec_end));
		mrec = row_log_apply_op(
			index, dup, &error, offsets_heap, heap,
			has_index_lock, index->online_log->head.buf,
			(&index->online_log->head.buf)[1], offsets);
		if (error != DB_SUCCESS) {
			goto func_exit;
		} else if (UNIV_UNLIKELY(mrec == NULL)) {
			/* The record was not reassembled properly. */
			goto corruption;
		}
		/* The record was previously found out to be
		truncated. Now that the parse buffer was extended,
		it should proceed beyond the old end of the buffer. */
		ut_a(mrec > mrec_end);

		index->online_log->head.bytes = ulint(mrec - mrec_end);
		next_mrec += index->online_log->head.bytes;
	}

	ut_ad(next_mrec <= next_mrec_end);
	/* The following loop must not be parsing the temporary
	buffer, but head.block or tail.block. */

	/* mrec!=NULL means that the next record starts from the
	middle of the block */
	ut_ad((mrec == NULL) == (index->online_log->head.bytes == 0));

#ifdef UNIV_DEBUG
	if (next_mrec_end == index->online_log->head.block
	    + srv_sort_buf_size) {
		/* If tail.bytes == 0, next_mrec_end can also be at
		the end of tail.block. */
		if (index->online_log->tail.bytes == 0) {
			ut_ad(next_mrec == next_mrec_end);
			ut_ad(index->online_log->tail.blocks == 0);
			ut_ad(index->online_log->head.blocks == 0);
			ut_ad(index->online_log->head.bytes == 0);
		} else {
			ut_ad(next_mrec == index->online_log->head.block
			      + index->online_log->head.bytes);
			ut_ad(index->online_log->tail.blocks
			      > index->online_log->head.blocks);
		}
	} else if (next_mrec_end == index->online_log->tail.block
		   + index->online_log->tail.bytes) {
		ut_ad(next_mrec == index->online_log->tail.block
		      + index->online_log->head.bytes);
		ut_ad(index->online_log->tail.blocks == 0);
		ut_ad(index->online_log->head.blocks == 0);
		ut_ad(index->online_log->head.bytes
		      <= index->online_log->tail.bytes);
	} else {
		ut_error;
	}
#endif /* UNIV_DEBUG */

	mrec_end = next_mrec_end;

	while (!trx_is_interrupted(trx)) {
		mrec = next_mrec;
		ut_ad(mrec < mrec_end);

		if (!has_index_lock) {
			/* We are applying operations from a different
			block than the one that is being written to.
			We do not hold index->lock in order to
			allow other threads to concurrently buffer
			modifications. */
			ut_ad(mrec >= index->online_log->head.block);
			ut_ad(mrec_end == index->online_log->head.block
			      + srv_sort_buf_size);
			ut_ad(index->online_log->head.bytes
			      < srv_sort_buf_size);

			/* Take the opportunity to do a redo log
			checkpoint if needed. */
			log_free_check();
		} else {
			/* We are applying operations from the last block.
			Do not allow other threads to buffer anything,
			so that we can finally catch up and synchronize. */
			ut_ad(index->online_log->head.blocks == 0);
			ut_ad(index->online_log->tail.blocks == 0);
			ut_ad(mrec_end == index->online_log->tail.block
			      + index->online_log->tail.bytes);
			ut_ad(mrec >= index->online_log->tail.block);
		}

		next_mrec = row_log_apply_op(
			index, dup, &error, offsets_heap, heap,
			has_index_lock, mrec, mrec_end, offsets);

		if (error != DB_SUCCESS) {
			goto func_exit;
		} else if (next_mrec == next_mrec_end) {
			/* The record happened to end on a block boundary.
			Do we have more blocks left? */
			if (has_index_lock) {
				/* The index will be locked while
				applying the last block. */
				goto all_done;
			}

			mrec = NULL;
process_next_block:
			index->lock.x_lock(SRW_LOCK_CALL);
			has_index_lock = true;

			index->online_log->head.bytes = 0;
			index->online_log->head.blocks++;
			goto next_block;
		} else if (next_mrec != NULL) {
			ut_ad(next_mrec < next_mrec_end);
			index->online_log->head.bytes
				+= ulint(next_mrec - mrec);
		} else if (has_index_lock) {
			/* When mrec is within tail.block, it should
			be a complete record, because we are holding
			index->lock and thus excluding the writer. */
			ut_ad(index->online_log->tail.blocks == 0);
			ut_ad(mrec_end == index->online_log->tail.block
			      + index->online_log->tail.bytes);
			ut_ad(0);
			goto unexpected_eof;
		} else {
			memcpy(index->online_log->head.buf, mrec,
			       ulint(mrec_end - mrec));
			mrec_end += ulint(index->online_log->head.buf - mrec);
			mrec = index->online_log->head.buf;
			goto process_next_block;
		}
	}

interrupted:
	error = DB_INTERRUPTED;
func_exit:
	if (!has_index_lock) {
		index->lock.x_lock(SRW_LOCK_CALL);
	}

	switch (error) {
	case DB_SUCCESS:
		break;
	case DB_INDEX_CORRUPT:
		if (((os_offset_t) index->online_log->tail.blocks + 1)
		    * srv_sort_buf_size >= srv_online_max_size) {
			/* The log file grew too big. */
			error = DB_ONLINE_LOG_TOO_BIG;
		}
		/* fall through */
	default:
		/* We set the flag directly instead of invoking
		dict_set_corrupted_index_cache_only(index) here,
		because the index is not "public" yet. */
		index->type |= DICT_CORRUPT;
	}

	mem_heap_free(heap);
	mem_heap_free(offsets_heap);
	row_log_block_free(index->online_log->head);
	ut_free(offsets);
	return(error);
}

/** Apply the row log to the index upon completing index creation.
@param[in]	trx	transaction (for checking if the operation was
interrupted)
@param[in,out]	index	secondary index
@param[in,out]	table	MySQL table (for reporting duplicates)
@param[in,out]	stage	performance schema accounting object, used by
ALTER TABLE. stage->begin_phase_log_index() will be called initially and then
stage->inc() will be called for each block of log that is applied or nullptr
when row log has been applied by DML thread.
@return DB_SUCCESS, or error code on failure */
dberr_t
row_log_apply(
	const trx_t*		trx,
	dict_index_t*		index,
	struct TABLE*		table,
	ut_stage_alter_t*	stage)
{
	dberr_t		error;
	row_merge_dup_t	dup = { index, table, NULL, 0 };
	DBUG_ENTER("row_log_apply");

	ut_ad(dict_index_is_online_ddl(index)
	      || (index->online_log
		  && index->online_status == ONLINE_INDEX_COMPLETE));
	ut_ad(!dict_index_is_clust(index));

	if (stage) {
		stage->begin_phase_log_index();
	}

	log_free_check();

	index->lock.x_lock(SRW_LOCK_CALL);

	if (!dict_table_is_corrupted(index->table)
	    && index->online_log) {
		error = row_log_apply_ops(trx, index, &dup, stage);
	} else {
		error = DB_SUCCESS;
	}

	if (error != DB_SUCCESS) {
		ut_ad(index->table->space);
		/* We set the flag directly instead of invoking
		dict_set_corrupted_index_cache_only(index) here,
		because the index is not "public" yet. */
		index->type |= DICT_CORRUPT;
		index->table->drop_aborted = TRUE;

		dict_index_set_online_status(index, ONLINE_INDEX_ABORTED);
	} else if (stage) {
		/* Mark the index as completed only when it is
		being called by DDL thread */
		ut_ad(dup.n_dup == 0);
		dict_index_set_online_status(index, ONLINE_INDEX_COMPLETE);
	}

	index->lock.x_unlock();

	DBUG_RETURN(error);
}

unsigned row_log_get_n_core_fields(const dict_index_t *index)
{
  ut_ad(index->online_log);
  return index->online_log->n_core_fields;
}

dberr_t row_log_get_error(const dict_index_t *index)
{
  ut_ad(index->online_log);
  return index->online_log->error;
}

void dict_table_t::clear(que_thr_t *thr)
{
  for (dict_index_t *index= UT_LIST_GET_FIRST(indexes); index;
       index= UT_LIST_GET_NEXT(indexes, index))
  {
    if (index->type & DICT_FTS)
      continue;

    switch (dict_index_get_online_status(index)) {
    case ONLINE_INDEX_ABORTED:
    case ONLINE_INDEX_ABORTED_DROPPED:
      continue;
    case ONLINE_INDEX_COMPLETE:
      break;
    case ONLINE_INDEX_CREATION:
      ut_ad("invalid type" == 0);
      MY_ASSERT_UNREACHABLE();
      break;
    }
    index->clear(thr);
  }
}

const rec_t *
UndorecApplier::get_old_rec(const dtuple_t &tuple, dict_index_t *index,
                            const rec_t **clust_rec, rec_offs **offsets)
{
  ut_ad(index->is_primary());
  btr_pcur_t pcur;

  bool found= row_search_on_row_ref(&pcur, BTR_MODIFY_LEAF,
                                    index->table, &tuple, &mtr);
  ut_a(found);
  *clust_rec= btr_pcur_get_rec(&pcur);

  ulint len= 0;
  rec_t *prev_version;
  const rec_t *version= *clust_rec;
  do
  {
    *offsets= rec_get_offsets(version, index, *offsets,
                              index->n_core_fields, ULINT_UNDEFINED,
                              &heap);
    roll_ptr_t roll_ptr= trx_read_roll_ptr(
      rec_get_nth_field(version, *offsets, index->db_roll_ptr(), &len));
    ut_ad(len == DATA_ROLL_PTR_LEN);
    if (is_same(roll_ptr))
      return version;
    trx_undo_prev_version_build(*clust_rec, &mtr, version, index,
                                *offsets, heap, &prev_version, nullptr,
                                nullptr, 0);
    version= prev_version;
  }
  while (version);

  return nullptr;
}

/** Clear out all online log of other online indexes after
encountering the error during row_log_apply() in DML thread
@param	table	table which does online DDL */
static void row_log_mark_other_online_index_abort(dict_table_t *table)
{
  dict_index_t *clust_index= dict_table_get_first_index(table);
  for (dict_index_t *index= dict_table_get_next_index(clust_index);
       index; index= dict_table_get_next_index(index))
  {
    if (index->online_log &&
        index->online_status <= ONLINE_INDEX_CREATION &&
        !index->is_corrupted())
    {
      index->lock.x_lock(SRW_LOCK_CALL);
      row_log_abort_sec(index);
      index->type|= DICT_CORRUPT;
      index->lock.x_unlock();
      MONITOR_ATOMIC_INC(MONITOR_BACKGROUND_DROP_INDEX);
    }
  }

  clust_index->lock.x_lock(SRW_LOCK_CALL);
  clust_index->online_log= nullptr;
  clust_index->lock.x_unlock();
  table->drop_aborted= TRUE;
}

void UndorecApplier::log_insert(const dtuple_t &tuple,
                                dict_index_t *clust_index)
{
  DEBUG_SYNC_C("row_log_insert_handle");
  ut_ad(clust_index->is_primary());
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs *offsets= offsets_;

  rec_offs_init(offsets_);
  mtr.start();
  const rec_t *rec;
  const rec_t *match_rec= get_old_rec(tuple, clust_index, &rec, &offsets);
  if (!match_rec)
  {
    mtr.commit();
    return;
  }
  const rec_t *copy_rec= match_rec;
  if (match_rec == rec)
  {
    copy_rec= rec_copy(mem_heap_alloc(
      heap, rec_offs_size(offsets)), match_rec, offsets);
    rec_offs_make_valid(copy_rec, clust_index, true, offsets);
  }
  mtr.commit();

  dict_table_t *table= clust_index->table;
  clust_index->lock.s_lock(SRW_LOCK_CALL);
  if (clust_index->online_log &&
      !clust_index->online_log_is_dummy() &&
      clust_index->online_status <= ONLINE_INDEX_CREATION)
  {
    row_log_table_insert(copy_rec, clust_index, offsets);
    clust_index->lock.s_unlock();
  }
  else
  {
    clust_index->lock.s_unlock();
    row_ext_t *ext;
    dtuple_t *row= row_build(ROW_COPY_POINTERS, clust_index,
      copy_rec, offsets, table, nullptr, nullptr, &ext, heap);

    if (table->n_v_cols)
    {
      /* Update the row with virtual column values present
      in the undo log or update vector */
      if (type == TRX_UNDO_UPD_DEL_REC)
        row_upd_replace_vcol(row, table, update, false,
                             nullptr,
                             (cmpl_info & UPD_NODE_NO_ORD_CHANGE)
                             ? nullptr : undo_rec);
      else
        trx_undo_read_v_cols(table, undo_rec, row, false);
    }

    bool success= true;
    dict_index_t *index= dict_table_get_next_index(clust_index);
    while (index)
    {
      index->lock.s_lock(SRW_LOCK_CALL);
      if (index->online_log &&
          index->online_status <= ONLINE_INDEX_CREATION &&
          !index->is_corrupted())
      {
        dtuple_t *entry= row_build_index_entry_low(row, ext, index,
                                                   heap, ROW_BUILD_NORMAL);
        success= row_log_online_op(index, entry, trx_id);
      }

      index->lock.s_unlock();
      if (!success)
      {
        row_log_mark_other_online_index_abort(index->table);
        return;
      }
      index= dict_table_get_next_index(index);
    }
  }
}

void UndorecApplier::log_update(const dtuple_t &tuple,
                                dict_index_t *clust_index)
{
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs offsets2_[REC_OFFS_NORMAL_SIZE];
  rec_offs *offsets= offsets_;
  rec_offs *prev_offsets= offsets2_;

  rec_offs_init(offsets_);
  rec_offs_init(offsets2_);

  dict_table_t *table= clust_index->table;

  clust_index->lock.s_lock(SRW_LOCK_CALL);
  bool table_rebuild=
    (clust_index->online_log
     && !clust_index->online_log_is_dummy()
     && clust_index->online_status <= ONLINE_INDEX_CREATION);
  clust_index->lock.s_unlock();

  mtr.start();
  const rec_t *rec;
  rec_t *prev_version;
  bool is_update= (type == TRX_UNDO_UPD_EXIST_REC);
  const rec_t *match_rec= get_old_rec(tuple, clust_index, &rec, &offsets);
  if (!match_rec)
  {
    mtr.commit();
    return;
  }

  if (table_rebuild)
  {
    const rec_t *copy_rec= match_rec;
    if (match_rec == rec)
      copy_rec= rec_copy(mem_heap_alloc(
        heap, rec_offs_size(offsets)), match_rec, offsets);
    trx_undo_prev_version_build(rec, &mtr, match_rec, clust_index,
                                offsets, heap, &prev_version, nullptr,
                                nullptr, 0);

    prev_offsets= rec_get_offsets(prev_version, clust_index, prev_offsets,
                                  clust_index->n_core_fields,
                                  ULINT_UNDEFINED, &heap);
    rec_offs_make_valid(copy_rec, clust_index, true, offsets);
    mtr.commit();

    clust_index->lock.s_lock(SRW_LOCK_CALL);
    /* Recheck whether clustered index online log has been cleared */
    if (clust_index->online_log)
    {
      if (is_update)
      {
        const dtuple_t *rebuilt_old_pk= row_log_table_get_pk(
          prev_version, clust_index, prev_offsets, nullptr, &heap);
        row_log_table_update(copy_rec, clust_index, offsets, rebuilt_old_pk);
      }
      else
        row_log_table_delete(prev_version, clust_index, prev_offsets, nullptr);
    }
    clust_index->lock.s_unlock();
    return;
  }

  dtuple_t *row= nullptr;
  row_ext_t *new_ext;
  if (match_rec != rec)
    row= row_build(ROW_COPY_POINTERS, clust_index, match_rec, offsets,
                   clust_index->table, NULL, NULL, &new_ext, heap);
  else
    row= row_build(ROW_COPY_DATA, clust_index, rec, offsets,
                   clust_index->table, NULL, NULL, &new_ext, heap);
  mtr.commit();
  row_ext_t *old_ext;
  dtuple_t *old_row= nullptr;
  if (!(this->cmpl_info & UPD_NODE_NO_ORD_CHANGE))
  {
    for (ulint i = 0; i < dict_table_get_n_v_cols(table); i++)
       dfield_get_type(
         dtuple_get_nth_v_field(row, i))->mtype = DATA_MISSING;
  }

  if (is_update)
  {
    old_row= dtuple_copy(row, heap);
    row_upd_replace(old_row, &old_ext, clust_index, update, heap);
  }

  if (table->n_v_cols)
    row_upd_replace_vcol(row, table, update, false, nullptr,
                         (cmpl_info & UPD_NODE_NO_ORD_CHANGE)
                         ? nullptr : this->undo_rec);

  bool success= true;
  dict_index_t *index= dict_table_get_next_index(clust_index);
  while (index)
  {
    index->lock.s_lock(SRW_LOCK_CALL);
    if (index->online_log &&
        index->online_status <= ONLINE_INDEX_CREATION &&
        !index->is_corrupted())
    {
      if (is_update)
      {
        dtuple_t *old_entry= row_build_index_entry_low(
          old_row, old_ext, index, heap, ROW_BUILD_NORMAL);

        success= row_log_online_op(index, old_entry, 0);

	dtuple_t *new_entry= row_build_index_entry_low(
          row, new_ext, index, heap, ROW_BUILD_NORMAL);

	if (success)
	  success= row_log_online_op(index, new_entry, trx_id);
      }
      else
      {
        dtuple_t *old_entry= row_build_index_entry_low(
          row, new_ext, index, heap, ROW_BUILD_NORMAL);

        success= row_log_online_op(index, old_entry, 0);
      }
    }
    index->lock.s_unlock();
    if (!success)
    {
      row_log_mark_other_online_index_abort(index->table);
      return;
    }
    index= dict_table_get_next_index(index);
  }
}

