/*****************************************************************************

Copyright (c) 2013, 2018, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2018, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file row/row0trunc.cc
TRUNCATE implementation

Created 2013-04-12 Sunny Bains
*******************************************************/

#include "row0trunc.h"
#include "btr0sea.h"
#include "pars0pars.h"
#include "dict0crea.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"
#include "lock0lock.h"
#include "fts0fts.h"
#include "fsp0sysspace.h"
#include "ibuf0ibuf.h"
#include "os0file.h"
#include "que0que.h"
#include "trx0undo.h"

/* FIXME: For temporary tables, use a simple approach of btr_free()
and btr_create() of each index tree. */

/* FIXME: For persistent tables, remove this code in MDEV-11655
and use a combination of the transactional DDL log to make atomic the
low-level operations ha_innobase::delete_table(), ha_innobase::create(). */

bool	truncate_t::s_fix_up_active = false;
truncate_t::tables_t		truncate_t::s_tables;
truncate_t::truncated_tables_t	truncate_t::s_truncated_tables;

/**
Iterator over the the raw records in an index, doesn't support MVCC. */
class IndexIterator {

public:
	/**
	Iterate over an indexes records
	@param index		index to iterate over */
	explicit IndexIterator(dict_index_t* index)
		:
		m_index(index)
	{
		/* Do nothing */
	}

	/**
	Search for key. Position the cursor on a record GE key.
	@return DB_SUCCESS or error code. */
	dberr_t search(dtuple_t& key, bool noredo)
	{
		mtr_start(&m_mtr);

		if (noredo) {
			mtr_set_log_mode(&m_mtr, MTR_LOG_NO_REDO);
		}

		btr_pcur_open_on_user_rec(
			m_index,
			&key,
			PAGE_CUR_GE,
			BTR_MODIFY_LEAF,
			&m_pcur, &m_mtr);

		return(DB_SUCCESS);
	}

	/**
	Iterate over all the records
	@return DB_SUCCESS or error code */
	template <typename Callback>
	dberr_t for_each(Callback& callback)
	{
		dberr_t	err = DB_SUCCESS;

		for (;;) {

			if (!btr_pcur_is_on_user_rec(&m_pcur)
			    || !callback.match(&m_pcur)) {

				/* The end of of the index has been reached. */
				err = DB_END_OF_INDEX;
				break;
			}

			rec_t*	rec = btr_pcur_get_rec(&m_pcur);

			if (!rec_get_deleted_flag(rec, FALSE)) {

				err = callback(&m_mtr, &m_pcur);

				if (err != DB_SUCCESS) {
					break;
				}
			}

			btr_pcur_move_to_next_user_rec(&m_pcur, &m_mtr);
		}

		btr_pcur_close(&m_pcur);
		mtr_commit(&m_mtr);

		return(err == DB_END_OF_INDEX ? DB_SUCCESS : err);
	}

private:
	// Disable copying
	IndexIterator(const IndexIterator&);
	IndexIterator& operator=(const IndexIterator&);

private:
	mtr_t		m_mtr;
	btr_pcur_t	m_pcur;
	dict_index_t*	m_index;
};

/** SysIndex table iterator, iterate over records for a table. */
class SysIndexIterator {

public:
	/**
	Iterate over all the records that match the table id.
	@return DB_SUCCESS or error code */
	template <typename Callback>
	dberr_t for_each(Callback& callback) const
	{
		dict_index_t*	sys_index;
		byte		buf[DTUPLE_EST_ALLOC(1)];
		dtuple_t*	tuple =
			dtuple_create_from_mem(buf, sizeof(buf), 1, 0);
		dfield_t*	dfield = dtuple_get_nth_field(tuple, 0);

		dfield_set_data(
			dfield,
			callback.table_id(),
			sizeof(*callback.table_id()));

		sys_index = dict_table_get_first_index(dict_sys->sys_indexes);

		dict_index_copy_types(tuple, sys_index, 1);

		IndexIterator	iterator(sys_index);

		/* Search on the table id and position the cursor
		on GE table_id. */
		iterator.search(*tuple, callback.get_logging_status());

		return(iterator.for_each(callback));
	}
};

/** Generic callback abstract class. */
class Callback
{

public:
	/**
	Constructor
	@param	table_id		id of the table being operated.
	@param	noredo			if true turn off logging. */
	Callback(table_id_t table_id, bool noredo)
		:
		m_id(),
		m_noredo(noredo)
	{
		/* Convert to storage byte order. */
		mach_write_to_8(&m_id, table_id);
	}

	/**
	Destructor */
	virtual ~Callback()
	{
		/* Do nothing */
	}

	/**
	@param pcur		persistent cursor used for iteration
	@return true if the table id column matches. */
	bool match(btr_pcur_t* pcur) const
	{
		ulint		len;
		const byte*	field;
		rec_t*		rec = btr_pcur_get_rec(pcur);

		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_INDEXES__TABLE_ID, &len);

		ut_ad(len == 8);

		return(memcmp(&m_id, field, len) == 0);
	}

	/**
	@return pointer to table id storage format buffer */
	const table_id_t* table_id() const
	{
		return(&m_id);
	}

	/**
	@return	return if logging needs to be turned off. */
	bool get_logging_status() const
	{
		return(m_noredo);
	}

protected:
	// Disably copying
	Callback(const Callback&);
	Callback& operator=(const Callback&);

protected:
	/** Table id in storage format */
	table_id_t		m_id;

	/** Turn off logging. */
	const bool		m_noredo;
};

/**
Scan to find out truncate log file from the given directory path.

@param dir_path		look for log directory in following path.
@param log_files	cache to hold truncate log file name found.
@return DB_SUCCESS or error code. */
dberr_t
TruncateLogParser::scan(
	const char*		dir_path,
	trunc_log_files_t&	log_files)
{
	os_file_dir_t	dir;
	os_file_stat_t	fileinfo;
	dberr_t		err = DB_SUCCESS;
	const ulint	dir_len = strlen(dir_path);

	/* Scan and look out for the truncate log files. */
	dir = os_file_opendir(dir_path, true);
	if (dir == NULL) {
		return(DB_IO_ERROR);
	}

	while (fil_file_readdir_next_file(
			&err, dir_path, dir, &fileinfo) == 0) {

		ulint nm_len = strlen(fileinfo.name);

		if (fileinfo.type == OS_FILE_TYPE_FILE
		    && nm_len > sizeof "ib_trunc.log"
		    && (0 == strncmp(fileinfo.name + nm_len
				     - ((sizeof "trunc.log") - 1),
				     "trunc.log", (sizeof "trunc.log") - 1))
		    && (0 == strncmp(fileinfo.name, "ib_", 3))) {

			if (fileinfo.size == 0) {
				/* Truncate log not written. Remove the file. */
				os_file_delete(
					innodb_log_file_key, fileinfo.name);
				continue;
			}

			/* Construct file name by appending directory path */
			ulint	sz = dir_len + 22 + 22 + sizeof "ib_trunc.log";
			char*	log_file_name = UT_NEW_ARRAY_NOKEY(char, sz);
			if (log_file_name == NULL) {
				err = DB_OUT_OF_MEMORY;
				break;
			}
			memset(log_file_name, 0, sz);

			strncpy(log_file_name, dir_path, dir_len);
			ulint	log_file_name_len = strlen(log_file_name);
			if (log_file_name[log_file_name_len - 1]
				!= OS_PATH_SEPARATOR) {

				log_file_name[log_file_name_len]
					= OS_PATH_SEPARATOR;
				log_file_name_len = strlen(log_file_name);
			}
			strcat(log_file_name, fileinfo.name);
			log_files.push_back(log_file_name);
		}
	}

	os_file_closedir(dir);

	return(err);
}

/**
Parse the log file and populate table to truncate information.
(Add this table to truncate information to central vector that is then
 used by truncate fix-up routine to fix-up truncate action of the table.)

@param	log_file_name	log file to parse
@return DB_SUCCESS or error code. */
dberr_t
TruncateLogParser::parse(
	const char*	log_file_name)
{
	dberr_t		err = DB_SUCCESS;
	truncate_t*	truncate = NULL;

	/* Open the file and read magic-number to findout if truncate action
	was completed. */
	bool		ret;
	os_file_t	handle = os_file_create_simple(
		innodb_log_file_key, log_file_name,
		OS_FILE_OPEN, OS_FILE_READ_ONLY, srv_read_only_mode, &ret);
	if (!ret) {
		ib::error() << "Error opening truncate log file: "
			<< log_file_name;
		return(DB_IO_ERROR);
	}

	ulint	sz = srv_page_size;
	void*	buf = ut_zalloc_nokey(sz + srv_page_size);
	if (buf == 0) {
		os_file_close(handle);
		return(DB_OUT_OF_MEMORY);
	}

	IORequest	request(IORequest::READ);

	/* Align the memory for file i/o if we might have O_DIRECT set*/
	byte*	log_buf = static_cast<byte*>(ut_align(buf, srv_page_size));

	do {
		err = os_file_read(request, handle, log_buf, 0, sz);

		if (err != DB_SUCCESS) {
			os_file_close(handle);
			break;
		}

		if (mach_read_from_4(log_buf) == 32743712) {

			/* Truncate action completed. Avoid parsing the file. */
			os_file_close(handle);

			os_file_delete(innodb_log_file_key, log_file_name);
			break;
		}

		if (truncate == NULL) {
			truncate = UT_NEW_NOKEY(truncate_t(log_file_name));
			if (truncate == NULL) {
				os_file_close(handle);
				err = DB_OUT_OF_MEMORY;
				break;
			}
		}

		err = truncate->parse(log_buf + 4, log_buf + sz - 4);

		if (err != DB_SUCCESS) {

			ut_ad(err == DB_FAIL);

			ut_free(buf);
			buf = 0;

			sz *= 2;

			buf = ut_zalloc_nokey(sz + srv_page_size);

			if (buf == 0) {
				os_file_close(handle);
				err = DB_OUT_OF_MEMORY;
				UT_DELETE(truncate);
				truncate = NULL;
				break;
			}

			log_buf = static_cast<byte*>(
				ut_align(buf, srv_page_size));
		}
	} while (err != DB_SUCCESS);

	ut_free(buf);

	if (err == DB_SUCCESS && truncate != NULL) {
		truncate_t::add(truncate);
		os_file_close(handle);
	}

	return(err);
}

/**
Scan and Parse truncate log files.

@param dir_path		look for log directory in following path
@return DB_SUCCESS or error code. */
dberr_t
TruncateLogParser::scan_and_parse(
	const char*	dir_path)
{
	dberr_t			err;
	trunc_log_files_t	log_files;

	/* Scan and trace all the truncate log files. */
	err = TruncateLogParser::scan(dir_path, log_files);

	/* Parse truncate lof files if scan was successful. */
	if (err == DB_SUCCESS) {

		for (ulint i = 0;
		     i < log_files.size() && err == DB_SUCCESS;
		     i++) {
			err = TruncateLogParser::parse(log_files[i]);
		}
	}

	trunc_log_files_t::const_iterator end = log_files.end();
	for (trunc_log_files_t::const_iterator it = log_files.begin();
	     it != end;
	     ++it) {
		if (*it != NULL) {
			UT_DELETE_ARRAY(*it);
		}
	}
	log_files.clear();

	return(err);
}

/** Callback to drop indexes during TRUNCATE */
class DropIndex : public Callback {

public:
	/**
	Constructor

	@param[in,out]	table	Table to truncate
	@param[in]	noredo	whether to disable redo logging */
	DropIndex(dict_table_t* table, bool noredo)
		:
		Callback(table->id, noredo),
		m_table(table)
	{
		/* No op */
	}

	/**
	@param mtr	mini-transaction covering the read
	@param pcur	persistent cursor used for reading
	@return DB_SUCCESS or error code */
	dberr_t operator()(mtr_t* mtr, btr_pcur_t* pcur) const;

private:
	/** Table to be truncated */
	dict_table_t*		m_table;
};

/** Callback to create the indexes during TRUNCATE */
class CreateIndex : public Callback {

public:
	/**
	Constructor

	@param[in,out]	table	Table to truncate
	@param[in]	noredo	whether to disable redo logging */
	CreateIndex(dict_table_t* table, bool noredo)
		:
		Callback(table->id, noredo),
		m_table(table)
	{
		/* No op */
	}

	/**
	Create the new index and update the root page number in the
	SysIndex table.

	@param mtr	mini-transaction covering the read
	@param pcur	persistent cursor used for reading
	@return DB_SUCCESS or error code */
	dberr_t operator()(mtr_t* mtr, btr_pcur_t* pcur) const;

private:
	// Disably copying
	CreateIndex(const CreateIndex&);
	CreateIndex& operator=(const CreateIndex&);

private:
	/** Table to be truncated */
	dict_table_t*		m_table;
};

/** Check for presence of table-id in SYS_XXXX tables. */
class TableLocator : public Callback {

public:
	/**
	Constructor
	@param table_id	table_id to look for */
	explicit TableLocator(table_id_t table_id)
		:
		Callback(table_id, false),
		m_table_found()
	{
		/* No op */
	}

	/**
	@return true if table is found */
	bool is_table_found() const
	{
		return(m_table_found);
	}

	/**
	Look for table-id in SYS_XXXX tables without loading the table.

	@param pcur	persistent cursor used for reading
	@return DB_SUCCESS */
	dberr_t operator()(mtr_t*, btr_pcur_t*)
	{
		m_table_found = true;
		return(DB_SUCCESS);
	}

private:
	/** Set to true if table is present */
	bool			m_table_found;
};

/**
Drop an index in the table.

@param mtr	mini-transaction covering the read
@param pcur	persistent cursor used for reading
@return DB_SUCCESS or error code */
dberr_t
DropIndex::operator()(mtr_t* mtr, btr_pcur_t* pcur) const
{
	rec_t*	rec = btr_pcur_get_rec(pcur);

	bool	freed = dict_drop_index_tree(rec, pcur, mtr);

#ifdef UNIV_DEBUG
	{
		ulint		len;
		const byte*	field;
		ulint		index_type;

		field = rec_get_nth_field_old(
			btr_pcur_get_rec(pcur), DICT_FLD__SYS_INDEXES__TYPE,
			&len);
		ut_ad(len == 4);

		index_type = mach_read_from_4(field);

		if (index_type & DICT_CLUSTERED) {
			/* Clustered index */
			DBUG_EXECUTE_IF("ib_trunc_crash_on_drop_of_clust_index",
					log_buffer_flush_to_disk();
					os_thread_sleep(2000000);
					DBUG_SUICIDE(););
		} else if (index_type & DICT_UNIQUE) {
			/* Unique index */
			DBUG_EXECUTE_IF("ib_trunc_crash_on_drop_of_uniq_index",
					log_buffer_flush_to_disk();
					os_thread_sleep(2000000);
					DBUG_SUICIDE(););
		} else if (index_type == 0) {
			/* Secondary index */
			DBUG_EXECUTE_IF("ib_trunc_crash_on_drop_of_sec_index",
					log_buffer_flush_to_disk();
					os_thread_sleep(2000000);
					DBUG_SUICIDE(););
		}
	}
#endif /* UNIV_DEBUG */

	DBUG_EXECUTE_IF("ib_err_trunc_drop_index", return DB_ERROR;);

	if (freed) {

		/* We will need to commit and restart the
		mini-transaction in order to avoid deadlocks.
		The dict_drop_index_tree() call has freed
		a page in this mini-transaction, and the rest
		of this loop could latch another index page.*/
		const mtr_log_t log_mode = mtr->get_log_mode();
		mtr_commit(mtr);

		mtr_start(mtr);
		mtr->set_log_mode(log_mode);

		btr_pcur_restore_position(BTR_MODIFY_LEAF, pcur, mtr);
	} else {
		if (!m_table->space) {
			return DB_ERROR;
		}
	}

	return(DB_SUCCESS);
}

/**
Create the new index and update the root page number in the
SysIndex table.

@param mtr	mini-transaction covering the read
@param pcur	persistent cursor used for reading
@return DB_SUCCESS or error code */
dberr_t
CreateIndex::operator()(mtr_t* mtr, btr_pcur_t* pcur) const
{
	ulint	root_page_no;

	root_page_no = dict_recreate_index_tree(m_table, pcur, mtr);

#ifdef UNIV_DEBUG
	{
		ulint		len;
		const byte*	field;
		ulint		index_type;

		field = rec_get_nth_field_old(
			btr_pcur_get_rec(pcur), DICT_FLD__SYS_INDEXES__TYPE,
			&len);
		ut_ad(len == 4);

		index_type = mach_read_from_4(field);

		if (index_type & DICT_CLUSTERED) {
			/* Clustered index */
			DBUG_EXECUTE_IF(
				"ib_trunc_crash_on_create_of_clust_index",
				log_buffer_flush_to_disk();
				os_thread_sleep(2000000);
				DBUG_SUICIDE(););
		} else if (index_type & DICT_UNIQUE) {
			/* Unique index */
			DBUG_EXECUTE_IF(
				"ib_trunc_crash_on_create_of_uniq_index",
				log_buffer_flush_to_disk();
				os_thread_sleep(2000000);
				DBUG_SUICIDE(););
		} else if (index_type == 0) {
			/* Secondary index */
			DBUG_EXECUTE_IF(
				"ib_trunc_crash_on_create_of_sec_index",
				log_buffer_flush_to_disk();
				os_thread_sleep(2000000);
				DBUG_SUICIDE(););
		}
	}
#endif /* UNIV_DEBUG */

	DBUG_EXECUTE_IF("ib_err_trunc_create_index", return DB_ERROR;);

	if (root_page_no != FIL_NULL) {

		rec_t*	rec = btr_pcur_get_rec(pcur);

		page_rec_write_field(
			rec, DICT_FLD__SYS_INDEXES__PAGE_NO,
			root_page_no, mtr);

		/* We will need to commit and restart the
		mini-transaction in order to avoid deadlocks.
		The dict_create_index_tree() call has allocated
		a page in this mini-transaction, and the rest of
		this loop could latch another index page. */
		mtr_commit(mtr);

		mtr_start(mtr);

		btr_pcur_restore_position(BTR_MODIFY_LEAF, pcur, mtr);

	} else {
		if (!m_table->space) {
			return(DB_ERROR);
		}
	}

	return(DB_SUCCESS);
}

/**
Update system table to reflect new table id.
@param old_table_id		old table id
@param new_table_id		new table id
@param reserve_dict_mutex	if TRUE, acquire/release
				dict_sys->mutex around call to pars_sql.
@param trx			transaction
@return error code or DB_SUCCESS */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
row_truncate_update_table_id(
	table_id_t	old_table_id,
	table_id_t	new_table_id,
	ibool		reserve_dict_mutex,
	trx_t*		trx)
{
	pars_info_t*	info	= NULL;
	dberr_t		err	= DB_SUCCESS;

	/* Scan the SYS_XXXX table and update to reflect new table-id. */
	info = pars_info_create();
	pars_info_add_ull_literal(info, "old_id", old_table_id);
	pars_info_add_ull_literal(info, "new_id", new_table_id);

	err = que_eval_sql(
		info,
		"PROCEDURE RENUMBER_TABLE_ID_PROC () IS\n"
		"BEGIN\n"
		"UPDATE SYS_TABLES"
		" SET ID = :new_id\n"
		" WHERE ID = :old_id;\n"
		"UPDATE SYS_COLUMNS SET TABLE_ID = :new_id\n"
		" WHERE TABLE_ID = :old_id;\n"
		"UPDATE SYS_INDEXES"
		" SET TABLE_ID = :new_id\n"
		" WHERE TABLE_ID = :old_id;\n"
		"UPDATE SYS_VIRTUAL"
		" SET TABLE_ID = :new_id\n"
		" WHERE TABLE_ID = :old_id;\n"
		"END;\n", reserve_dict_mutex, trx);

	return(err);
}

/**
Get the table id to truncate.
@param truncate_t		old/new table id of table to truncate
@return table_id_t		table_id to use in SYS_XXXX table update. */
static MY_ATTRIBUTE((warn_unused_result))
table_id_t
row_truncate_get_trunc_table_id(
	const truncate_t&	truncate)
{
	TableLocator tableLocator(truncate.old_table_id());

	SysIndexIterator().for_each(tableLocator);

	return(tableLocator.is_table_found() ?
		truncate.old_table_id(): truncate.new_table_id());
}

/**
Update system table to reflect new table id and root page number.
@param truncate_t		old/new table id of table to truncate
				and updated root_page_no of indexes.
@param new_table_id		new table id
@param reserve_dict_mutex	if TRUE, acquire/release
				dict_sys->mutex around call to pars_sql.
@param mark_index_corrupted	if true, then mark index corrupted.
@return error code or DB_SUCCESS */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
row_truncate_update_sys_tables_during_fix_up(
	const truncate_t&	truncate,
	table_id_t		new_table_id,
	ibool			reserve_dict_mutex,
	bool			mark_index_corrupted)
{
	trx_t*		trx = trx_create();

	trx_set_dict_operation(trx, TRX_DICT_OP_TABLE);

	table_id_t	table_id = row_truncate_get_trunc_table_id(truncate);

	/* Step-1: Update the root-page-no */

	dberr_t	err;

	err = truncate.update_root_page_no(
		trx, table_id, reserve_dict_mutex, mark_index_corrupted);

	if (err != DB_SUCCESS) {
		return(err);
	}

	/* Step-2: Update table-id. */

	err = row_truncate_update_table_id(
		table_id, new_table_id, reserve_dict_mutex, trx);

	if (err == DB_SUCCESS) {
		dict_mutex_enter_for_mysql();

		/* Remove the table with old table_id from cache. */
		dict_table_t*	old_table = dict_table_open_on_id(
			table_id, true, DICT_TABLE_OP_NORMAL);

		if (old_table != NULL) {
			dict_table_close(old_table, true, false);
			dict_table_remove_from_cache(old_table);
		}

		/* Open table with new table_id and set table as
		corrupted if it has FTS index. */

		dict_table_t*	table = dict_table_open_on_id(
			new_table_id, true, DICT_TABLE_OP_NORMAL);
		ut_ad(table->id == new_table_id);

		bool	has_internal_doc_id =
			dict_table_has_fts_index(table)
			|| DICT_TF2_FLAG_IS_SET(
				table, DICT_TF2_FTS_HAS_DOC_ID);

		if (has_internal_doc_id) {
			trx->dict_operation_lock_mode = RW_X_LATCH;
			fts_check_corrupt(table, trx);
			trx->dict_operation_lock_mode = 0;
		}

		dict_table_close(table, true, false);
		dict_mutex_exit_for_mysql();
	}

	trx_commit_for_mysql(trx);
	trx_free(trx);

	return(err);
}

/********************************************************//**
Recreates table indexes by applying
TRUNCATE log record during recovery.
@return DB_SUCCESS or error code */
static
dberr_t
fil_recreate_table(
/*===============*/
	ulint		format_flags,	/*!< in: page format */
	const char*	name,		/*!< in: table name */
	truncate_t&	truncate)	/*!< in: The information of
					TRUNCATE log record */
{
	ut_ad(!truncate_t::s_fix_up_active);
	truncate_t::s_fix_up_active = true;

	/* Step-1: Scan for active indexes from REDO logs and drop
	all the indexes using low level function that take root_page_no
	and space-id. */
	truncate.drop_indexes(fil_system.sys_space);

	/* Step-2: Scan for active indexes and re-create them. */
	dberr_t err = truncate.create_indexes(
		name, fil_system.sys_space, format_flags);
	if (err != DB_SUCCESS) {
		ib::info() << "Recovery failed for TRUNCATE TABLE '"
			<< name << "' within the system tablespace";
	}

	truncate_t::s_fix_up_active = false;

	return(err);
}

/********************************************************//**
Recreates the tablespace and table indexes by applying
TRUNCATE log record during recovery.
@return DB_SUCCESS or error code */
static
dberr_t
fil_recreate_tablespace(
/*====================*/
	ulint		space_id,	/*!< in: space id */
	ulint		format_flags,	/*!< in: page format */
	ulint		flags,		/*!< in: tablespace flags */
	const char*	name,		/*!< in: table name */
	truncate_t&	truncate,	/*!< in: The information of
					TRUNCATE log record */
	lsn_t		recv_lsn)	/*!< in: the end LSN of
						the log record */
{
	dberr_t		err = DB_SUCCESS;
	mtr_t		mtr;

	ut_ad(!truncate_t::s_fix_up_active);
	truncate_t::s_fix_up_active = true;

	/* Step-1: Invalidate buffer pool pages belonging to the tablespace
	to re-create. */
	buf_LRU_flush_or_remove_pages(space_id, NULL);

	/* Remove all insert buffer entries for the tablespace */
	ibuf_delete_for_discarded_space(space_id);

	/* Step-2: truncate tablespace (reset the size back to original or
	default size) of tablespace. */
	err = truncate.truncate(
		space_id, truncate.get_dir_path(), name, flags, true);

	if (err != DB_SUCCESS) {

		ib::info() << "Cannot access .ibd file for table '"
			<< name << "' with tablespace " << space_id
			<< " while truncating";
		return(DB_ERROR);
	}

	fil_space_t* space = fil_space_acquire(space_id);
	if (!space) {
		ib::info() << "Missing .ibd file for table '" << name
			<< "' with tablespace " << space_id;
		return(DB_ERROR);
	}

	const page_size_t page_size(space->flags);

	/* Step-3: Initialize Header. */
	if (page_size.is_compressed()) {
		byte*	buf;
		page_t*	page;

		buf = static_cast<byte*>(
			ut_zalloc_nokey(3U << srv_page_size_shift));

		/* Align the memory for file i/o */
		page = static_cast<byte*>(ut_align(buf, srv_page_size));

		flags |= FSP_FLAGS_PAGE_SSIZE();

		fsp_header_init_fields(page, space_id, flags);

		mach_write_to_4(
			page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);

		page_zip_des_t  page_zip;
		page_zip_set_size(&page_zip, page_size.physical());
		page_zip.data = page + srv_page_size;

#ifdef UNIV_DEBUG
		page_zip.m_start =
#endif /* UNIV_DEBUG */
		page_zip.m_end = page_zip.m_nonempty = page_zip.n_blobs = 0;
		buf_flush_init_for_writing(NULL, page, &page_zip, 0);

		err = fil_io(IORequestWrite, true, page_id_t(space_id, 0),
			     page_size, 0, page_size.physical(), page_zip.data,
			     NULL);

		ut_free(buf);

		if (err != DB_SUCCESS) {
			ib::info() << "Failed to clean header of the"
				" table '" << name << "' with tablespace "
				<< space_id;
			goto func_exit;
		}
	}

	mtr_start(&mtr);
	/* Don't log the operation while fixing up table truncate operation
	as crash at this level can still be sustained with recovery restarting
	from last checkpoint. */
	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

	/* Initialize the first extent descriptor page and
	the second bitmap page for the new tablespace. */
	fsp_header_init(space, FIL_IBD_FILE_INITIAL_SIZE, &mtr);
	mtr_commit(&mtr);

	/* Step-4: Re-Create Indexes to newly re-created tablespace.
	This operation will restore tablespace back to what it was
	when it was created during CREATE TABLE. */
	err = truncate.create_indexes(name, space, format_flags);
	if (err != DB_SUCCESS) {
		goto func_exit;
	}

	/* Step-5: Write new created pages into ibd file handle and
	flush it to disk for the tablespace, in case i/o-handler thread
	deletes the bitmap page from buffer. */
	mtr_start(&mtr);

	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

	for (ulint page_no = 0;
	     page_no < UT_LIST_GET_FIRST(space->chain)->size; ++page_no) {

		const page_id_t	cur_page_id(space_id, page_no);

		buf_block_t*	block = buf_page_get(cur_page_id, page_size,
						     RW_X_LATCH, &mtr);

		byte*	page = buf_block_get_frame(block);

		if (!FSP_FLAGS_GET_ZIP_SSIZE(flags)) {
			ut_ad(!page_size.is_compressed());

			buf_flush_init_for_writing(
				block, page, NULL, recv_lsn);

			err = fil_io(IORequestWrite, true, cur_page_id,
				     page_size, 0, srv_page_size, page, NULL);
		} else {
			ut_ad(page_size.is_compressed());

			/* We don't want to rewrite empty pages. */

			if (fil_page_get_type(page) != 0) {
				page_zip_des_t*  page_zip =
					buf_block_get_page_zip(block);

				buf_flush_init_for_writing(
					block, page, page_zip, recv_lsn);

				err = fil_io(IORequestWrite, true,
					     cur_page_id,
					     page_size, 0,
					     page_size.physical(),
					     page_zip->data, NULL);
			} else {
#ifdef UNIV_DEBUG
				const byte*	data = block->page.zip.data;

				/* Make sure that the page is really empty */
				for (ulint i = 0;
				     i < page_size.physical();
				     ++i) {

					ut_a(data[i] == 0);
				}
#endif /* UNIV_DEBUG */
			}
		}

		if (err != DB_SUCCESS) {
			ib::info() << "Cannot write page " << page_no
				<< " into a .ibd file for table '"
				<< name << "' with tablespace " << space_id;
		}
	}

	mtr_commit(&mtr);

	truncate_t::s_fix_up_active = false;
func_exit:
	space->release();
	return(err);
}

/**
Fix the table truncate by applying information parsed from TRUNCATE log.
Fix-up includes re-creating table (drop and re-create indexes)
@return	error code or DB_SUCCESS */
dberr_t
truncate_t::fixup_tables_in_system_tablespace()
{
	dberr_t	err = DB_SUCCESS;

	/* Using the info cached during REDO log scan phase fix the
	table truncate. */

	for (tables_t::iterator it = s_tables.begin();
	     it != s_tables.end();) {

		if ((*it)->m_space_id == TRX_SYS_SPACE) {
			/* Step-1: Drop and re-create indexes. */
			ib::info() << "Completing truncate for table with "
				"id (" << (*it)->m_old_table_id << ") "
				"residing in the system tablespace.";

			err = fil_recreate_table(
				(*it)->m_format_flags,
				(*it)->m_tablename,
				**it);

			/* Step-2: Update the SYS_XXXX tables to reflect
			this new table_id and root_page_no. */
			table_id_t	new_id;

			dict_hdr_get_new_id(&new_id, NULL, NULL, NULL, true);

			err = row_truncate_update_sys_tables_during_fix_up(
				**it, new_id, TRUE,
				(err == DB_SUCCESS) ? false : true);

			if (err != DB_SUCCESS) {
				break;
			}

			os_file_delete(
				innodb_log_file_key, (*it)->m_log_file_name);
			UT_DELETE(*it);
			it = s_tables.erase(it);
		} else {
			++it;
		}
	}

	/* Also clear the map used to track tablespace truncated. */
	s_truncated_tables.clear();

	return(err);
}

/**
Fix the table truncate by applying information parsed from TRUNCATE log.
Fix-up includes re-creating tablespace.
@return	error code or DB_SUCCESS */
dberr_t
truncate_t::fixup_tables_in_non_system_tablespace()
{
	dberr_t	err = DB_SUCCESS;

	/* Using the info cached during REDO log scan phase fix the
	table truncate. */
	tables_t::iterator end = s_tables.end();

	for (tables_t::iterator it = s_tables.begin(); it != end; ++it) {

		/* All tables in the system tablespace have already been
		done and erased from this list. */
		ut_a((*it)->m_space_id != TRX_SYS_SPACE);

		/* Drop tablespace, drop indexes and re-create indexes. */

		ib::info() << "Completing truncate for table with "
			"id (" << (*it)->m_old_table_id << ") "
			"residing in file-per-table tablespace with "
			"id (" << (*it)->m_space_id << ")";

		fil_space_t* space = fil_space_get((*it)->m_space_id);

		if (!space) {
			/* Create the database directory for name,
			if it does not exist yet */
			fil_create_directory_for_tablename(
				(*it)->m_tablename);

			space = fil_ibd_create((*it)->m_space_id,
					       (*it)->m_tablename,
					       (*it)->m_dir_path,
					       (*it)->m_tablespace_flags,
					       FIL_IBD_FILE_INITIAL_SIZE,
					       (*it)->m_encryption,
					       (*it)->m_key_id, &err);
			if (!space) {
				/* If checkpoint is not yet done
				and table is dropped and then we might
				still have REDO entries for this table
				which are INVALID. Ignore them. */
				ib::warn() << "Failed to create"
					" tablespace for "
					   << (*it)->m_space_id
					   << " space-id";
				err = DB_ERROR;
				break;
			}
		}

		err = fil_recreate_tablespace(
			(*it)->m_space_id,
			(*it)->m_format_flags,
			(*it)->m_tablespace_flags,
			(*it)->m_tablename,
			**it, log_get_lsn());

		/* Step-2: Update the SYS_XXXX tables to reflect new
		table-id and root_page_no. */
		table_id_t	new_id;

		dict_hdr_get_new_id(&new_id, NULL, NULL, NULL, true);

		err = row_truncate_update_sys_tables_during_fix_up(
			**it, new_id, TRUE, (err == DB_SUCCESS) ? false : true);

		if (err != DB_SUCCESS) {
			break;
		}
	}

	if (err == DB_SUCCESS && s_tables.size() > 0) {

		log_make_checkpoint_at(LSN_MAX, TRUE);
	}

	for (ulint i = 0; i < s_tables.size(); ++i) {
		os_file_delete(
			innodb_log_file_key, s_tables[i]->m_log_file_name);
		UT_DELETE(s_tables[i]);
	}

	s_tables.clear();

	return(err);
}

/**
Constructor

@param old_table_id	old table id assigned to table before truncate
@param new_table_id	new table id that will be assigned to table
			after truncate
@param dir_path		directory path */

truncate_t::truncate_t(
	table_id_t	old_table_id,
	table_id_t	new_table_id,
	const char*	dir_path)
	:
	m_space_id(),
	m_old_table_id(old_table_id),
	m_new_table_id(new_table_id),
	m_dir_path(),
	m_tablename(),
	m_tablespace_flags(),
	m_format_flags(),
	m_indexes(),
	m_log_lsn(),
	m_log_file_name(),
	/* JAN: TODO: Encryption */
	m_encryption(FIL_ENCRYPTION_DEFAULT),
	m_key_id(FIL_DEFAULT_ENCRYPTION_KEY)
{
	if (dir_path != NULL) {
		m_dir_path = mem_strdup(dir_path);
	}
}

/**
Consturctor

@param log_file_name	parse the log file during recovery to populate
			information related to table to truncate */
truncate_t::truncate_t(
	const char*	log_file_name)
	:
	m_space_id(),
	m_old_table_id(),
	m_new_table_id(),
	m_dir_path(),
	m_tablename(),
	m_tablespace_flags(),
	m_format_flags(),
	m_indexes(),
	m_log_lsn(),
	m_log_file_name(),
	/* JAN: TODO: Encryption */
	m_encryption(FIL_ENCRYPTION_DEFAULT),
	m_key_id(FIL_DEFAULT_ENCRYPTION_KEY)

{
	m_log_file_name = mem_strdup(log_file_name);
	if (m_log_file_name == NULL) {
		ib::fatal() << "Failed creating truncate_t; out of memory";
	}
}

/** Constructor */

truncate_t::index_t::index_t()
	:
	m_id(),
	m_type(),
	m_root_page_no(FIL_NULL),
	m_new_root_page_no(FIL_NULL),
	m_n_fields(),
	m_trx_id_pos(ULINT_UNDEFINED),
	m_fields()
{
	/* Do nothing */
}

/** Destructor */

truncate_t::~truncate_t()
{
	if (m_dir_path != NULL) {
		ut_free(m_dir_path);
		m_dir_path = NULL;
	}

	if (m_tablename != NULL) {
		ut_free(m_tablename);
		m_tablename = NULL;
	}

	if (m_log_file_name != NULL) {
		ut_free(m_log_file_name);
		m_log_file_name = NULL;
	}

	m_indexes.clear();
}

/**
@return number of indexes parsed from the log record */

size_t
truncate_t::indexes() const
{
	return(m_indexes.size());
}

/**
Update root page number in SYS_XXXX tables.

@param trx			transaction object
@param table_id			table id for which information needs to
				be updated.
@param reserve_dict_mutex	if TRUE, acquire/release
				dict_sys->mutex around call to pars_sql.
@param mark_index_corrupted	if true, then mark index corrupted.
@return DB_SUCCESS or error code */

dberr_t
truncate_t::update_root_page_no(
	trx_t*		trx,
	table_id_t	table_id,
	ibool		reserve_dict_mutex,
	bool		mark_index_corrupted) const
{
	indexes_t::const_iterator end = m_indexes.end();

	dberr_t	err = DB_SUCCESS;

	for (indexes_t::const_iterator it = m_indexes.begin();
	     it != end;
	     ++it) {

		pars_info_t*	info = pars_info_create();

		pars_info_add_int4_literal(
			info, "page_no", it->m_new_root_page_no);

		pars_info_add_ull_literal(info, "table_id", table_id);

		pars_info_add_ull_literal(
			info, "index_id",
			(mark_index_corrupted ? IB_ID_MAX : it->m_id));

		err = que_eval_sql(
			info,
			"PROCEDURE RENUMBER_IDX_PAGE_NO_PROC () IS\n"
			"BEGIN\n"
			"UPDATE SYS_INDEXES"
			" SET PAGE_NO = :page_no\n"
			" WHERE TABLE_ID = :table_id"
			" AND ID = :index_id;\n"
			"END;\n", reserve_dict_mutex, trx);

		if (err != DB_SUCCESS) {
			break;
		}
	}

	return(err);
}

/**
Check whether a tablespace was truncated during recovery
@param space_id	tablespace id to check
@return true if the tablespace was truncated */

bool
truncate_t::is_tablespace_truncated(ulint space_id)
{
	tables_t::iterator end = s_tables.end();

	for (tables_t::iterator it = s_tables.begin(); it != end; ++it) {

		if ((*it)->m_space_id == space_id) {

			return(true);
		}
	}

	return(false);
}

/** Was tablespace truncated (on crash before checkpoint).
If the MLOG_TRUNCATE redo-record is still available then tablespace
was truncated and checkpoint is yet to happen.
@param[in]	space_id	tablespace id to check.
@return true if tablespace is was truncated. */
bool
truncate_t::was_tablespace_truncated(ulint space_id)
{
	return(s_truncated_tables.find(space_id) != s_truncated_tables.end());
}

/** Get the lsn associated with space.
@param[in]	space_id	tablespace id to check.
@return associated lsn. */
lsn_t
truncate_t::get_truncated_tablespace_init_lsn(ulint space_id)
{
	ut_ad(was_tablespace_truncated(space_id));

	return(s_truncated_tables.find(space_id)->second);
}

/**
Parses log record during recovery
@param start_ptr	buffer containing log body to parse
@param end_ptr		buffer end

@return DB_SUCCESS or error code */

dberr_t
truncate_t::parse(
	byte*		start_ptr,
	const byte*	end_ptr)
{
	/* Parse lsn, space-id, format-flags and tablespace-flags. */
	if (end_ptr < start_ptr + (8 + 4 + 4 + 4)) {
		return(DB_FAIL);
	}

	m_log_lsn = mach_read_from_8(start_ptr);
	start_ptr += 8;

	m_space_id = mach_read_from_4(start_ptr);
	start_ptr += 4;

	m_format_flags = mach_read_from_4(start_ptr);
	start_ptr += 4;

	m_tablespace_flags = mach_read_from_4(start_ptr);
	start_ptr += 4;

	/* Parse table-name. */
	if (end_ptr < start_ptr + (2)) {
		return(DB_FAIL);
	}

	ulint n_tablename_len = mach_read_from_2(start_ptr);
	start_ptr += 2;

	if (n_tablename_len > 0) {
		if (end_ptr < start_ptr + n_tablename_len) {
			return(DB_FAIL);
		}
		m_tablename = mem_strdup(reinterpret_cast<char*>(start_ptr));
		ut_ad(m_tablename[n_tablename_len - 1] == 0);
		start_ptr += n_tablename_len;
	}


	/* Parse and read old/new table-id, number of indexes */
	if (end_ptr < start_ptr + (8 + 8 + 2 + 2)) {
		return(DB_FAIL);
	}

	ut_ad(m_indexes.empty());

	m_old_table_id = mach_read_from_8(start_ptr);
	start_ptr += 8;

	m_new_table_id = mach_read_from_8(start_ptr);
	start_ptr += 8;

	ulint n_indexes = mach_read_from_2(start_ptr);
	start_ptr += 2;

	/* Parse the remote directory from TRUNCATE log record */
	{
		ulint	n_tabledirpath_len = mach_read_from_2(start_ptr);
		start_ptr += 2;

		if (end_ptr < start_ptr + n_tabledirpath_len) {
			return(DB_FAIL);
		}

		if (n_tabledirpath_len > 0) {

			m_dir_path = mem_strdup(reinterpret_cast<char*>(start_ptr));
			ut_ad(m_dir_path[n_tabledirpath_len - 1] == 0);
			start_ptr += n_tabledirpath_len;
		}
	}

	/* Parse index ids and types from TRUNCATE log record */
	for (ulint i = 0; i < n_indexes; ++i) {
		index_t	index;

		if (end_ptr < start_ptr + (8 + 4 + 4 + 4)) {
			return(DB_FAIL);
		}

		index.m_id = mach_read_from_8(start_ptr);
		start_ptr += 8;

		index.m_type = mach_read_from_4(start_ptr);
		start_ptr += 4;

		index.m_root_page_no = mach_read_from_4(start_ptr);
		start_ptr += 4;

		index.m_trx_id_pos = mach_read_from_4(start_ptr);
		start_ptr += 4;

		if (!(index.m_type & DICT_FTS)) {
			m_indexes.push_back(index);
		}
	}

	ut_ad(!m_indexes.empty());

	if (FSP_FLAGS_GET_ZIP_SSIZE(m_tablespace_flags)) {

		/* Parse the number of index fields from TRUNCATE log record */
		for (ulint i = 0; i < m_indexes.size(); ++i) {

			if (end_ptr < start_ptr + (2 + 2)) {
				return(DB_FAIL);
			}

			m_indexes[i].m_n_fields = mach_read_from_2(start_ptr);
			start_ptr += 2;

			ulint	len = mach_read_from_2(start_ptr);
			start_ptr += 2;

			if (end_ptr < start_ptr + len) {
				return(DB_FAIL);
			}

			index_t&	index = m_indexes[i];

			/* Should be NUL terminated. */
			ut_ad((start_ptr)[len - 1] == 0);

			index_t::fields_t::iterator	end;

			end = index.m_fields.end();

			index.m_fields.insert(
				end, start_ptr, &(start_ptr)[len]);

			start_ptr += len;
		}
	}

	return(DB_SUCCESS);
}

/** Parse log record from REDO log file during recovery.
@param[in,out]	start_ptr	buffer containing log body to parse
@param[in]	end_ptr		buffer end
@param[in]	space_id	tablespace identifier
@return parsed upto or NULL. */
byte*
truncate_t::parse_redo_entry(
	byte*		start_ptr,
	const byte*	end_ptr,
	ulint		space_id)
{
	lsn_t	lsn;

	/* Parse space-id, lsn */
	if (end_ptr < (start_ptr + 8)) {
		return(NULL);
	}

	lsn = mach_read_from_8(start_ptr);
	start_ptr += 8;

	/* Tablespace can't exist in both state.
	(scheduled-for-truncate, was-truncated). */
	if (!is_tablespace_truncated(space_id)) {

		truncated_tables_t::iterator	it =
				s_truncated_tables.find(space_id);

		if (it == s_truncated_tables.end()) {
			s_truncated_tables.insert(
				std::pair<ulint, lsn_t>(space_id, lsn));
		} else {
			it->second = lsn;
		}
	}

	return(start_ptr);
}

/**
Set the truncate log values for a compressed table.
@param index	index from which recreate infoormation needs to be extracted
@return DB_SUCCESS or error code */

dberr_t
truncate_t::index_t::set(
	const dict_index_t* index)
{
	/* Get trx-id column position (set only for clustered index) */
	if (dict_index_is_clust(index)) {
		m_trx_id_pos = dict_index_get_sys_col_pos(index, DATA_TRX_ID);
		ut_ad(m_trx_id_pos > 0);
		ut_ad(m_trx_id_pos != ULINT_UNDEFINED);
	} else {
		m_trx_id_pos = 0;
	}

	/* Original logic set this field differently if page is not leaf.
	For truncate case this being first page to get created it is
	always a leaf page and so we don't need that condition here. */
	m_n_fields = dict_index_get_n_fields(index);

	/* See requirements of page_zip_fields_encode for size. */
	ulint	encoded_buf_size = (m_n_fields + 1) * 2;
	byte*	encoded_buf = UT_NEW_ARRAY_NOKEY(byte, encoded_buf_size);

	if (encoded_buf == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	ulint len = page_zip_fields_encode(
		m_n_fields, index, m_trx_id_pos, encoded_buf);
	ut_a(len <= encoded_buf_size);

	/* Append the encoded fields data. */
	m_fields.insert(m_fields.end(), &encoded_buf[0], &encoded_buf[len]);

	/* NUL terminate the encoded data */
	m_fields.push_back(0);

	UT_DELETE_ARRAY(encoded_buf);

	return(DB_SUCCESS);
}

/** Create an index for a table.
@param[in]	table_name		table name, for which to create
the index
@param[in]	space			tablespace
@param[in]	page_size		page size of the .ibd file
@param[in]	index_type		type of index to truncate
@param[in]	index_id		id of index to truncate
@param[in]	btr_redo_create_info	control info for ::btr_create()
@param[in,out]	mtr			mini-transaction covering the
create index
@return root page no or FIL_NULL on failure */
inline ulint
truncate_t::create_index(
	const char*		table_name,
	fil_space_t*		space,
	ulint			index_type,
	index_id_t		index_id,
	const btr_create_t&	btr_redo_create_info,
	mtr_t*			mtr) const
{
	ulint	root_page_no = btr_create(
		index_type, space, index_id,
		NULL, &btr_redo_create_info, mtr);

	if (root_page_no == FIL_NULL) {

		ib::info() << "innodb_force_recovery was set to "
			<< srv_force_recovery << ". Continuing crash recovery"
			" even though we failed to create index " << index_id
			<< " for compressed table '" << table_name << "' with"
			" file " << space->chain.start->name;
	}

	return(root_page_no);
}

/** Check if index has been modified since TRUNCATE log snapshot
was recorded.
@param[in]	space		tablespace
@param[in]	root_page_no	index root page number
@return true if modified else false */
inline
bool
truncate_t::is_index_modified_since_logged(
	const fil_space_t*	space,
	ulint			root_page_no) const
{
	dberr_t	err;
	mtr_t	mtr;

	mtr_start(&mtr);

	/* Root page could be in free state if truncate crashed after drop_index
	and page was not allocated for any other object. */
	buf_block_t* block= buf_page_get_gen(
		page_id_t(space->id, root_page_no), page_size_t(space->flags),
		RW_X_LATCH, NULL,
		BUF_GET_POSSIBLY_FREED, __FILE__, __LINE__, &mtr, &err);
	if (!block) return true;

	page_t* root = buf_block_get_frame(block);

#ifdef UNIV_DEBUG
	/* If the root page has been freed as part of truncate drop_index action
	and not yet allocated for any object still the pagelsn > snapshot lsn */
	if (block->page.file_page_was_freed) {
		ut_ad(mach_read_from_8(root + FIL_PAGE_LSN) > m_log_lsn);
	}
#endif /* UNIV_DEBUG */

	lsn_t page_lsn = mach_read_from_8(root + FIL_PAGE_LSN);

	mtr_commit(&mtr);

	if (page_lsn > m_log_lsn) {
		return(true);
	}

	return(false);
}

/** Drop indexes for a table.
@param[in,out] space		tablespace */
void truncate_t::drop_indexes(fil_space_t* space) const
{
	mtr_t           mtr;

	indexes_t::const_iterator       end = m_indexes.end();
	const page_size_t page_size(space->flags);

	for (indexes_t::const_iterator it = m_indexes.begin();
	     it != end;
	     ++it) {

		ulint root_page_no = it->m_root_page_no;

		if (is_index_modified_since_logged(space, root_page_no)) {
			/* Page has been modified since TRUNCATE log snapshot
			was recorded so not safe to drop the index. */
			continue;
		}

		mtr_start(&mtr);

		if (space->id != TRX_SYS_SPACE) {
			/* Do not log changes for single-table
			tablespaces, we are in recovery mode. */
			mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);
		}

		if (root_page_no != FIL_NULL) {
			const page_id_t	root_page_id(space->id, root_page_no);

			btr_free_if_exists(
				root_page_id, page_size, it->m_id, &mtr);
		}

		/* If tree is already freed then we might return immediately
		in which case we need to release the lock we have acquired
		on root_page. */
		mtr_commit(&mtr);
	}
}


/** Create the indexes for a table
@param[in]	table_name	table name, for which to create the indexes
@param[in,out]	space		tablespace
@param[in]	format_flags	page format flags
@return DB_SUCCESS or error code. */
inline dberr_t
truncate_t::create_indexes(
	const char*		table_name,
	fil_space_t*		space,
	ulint			format_flags)
{
	mtr_t           mtr;

	mtr_start(&mtr);

	if (space->id != TRX_SYS_SPACE) {
		/* Do not log changes for single-table tablespaces, we
		are in recovery mode. */
		mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);
	}

	/* Create all new index trees with table format, index ids, index
	types, number of index fields and index field information taken
	out from the TRUNCATE log record. */

	ulint   root_page_no = FIL_NULL;
	indexes_t::iterator       end = m_indexes.end();
	for (indexes_t::iterator it = m_indexes.begin();
	     it != end;
	     ++it) {

		btr_create_t    btr_redo_create_info(
			FSP_FLAGS_GET_ZIP_SSIZE(space->flags)
			? &it->m_fields[0] : NULL);

		btr_redo_create_info.format_flags = format_flags;

		if (FSP_FLAGS_GET_ZIP_SSIZE(space->flags)) {

			btr_redo_create_info.n_fields = it->m_n_fields;
			/* Skip the NUL appended field */
			btr_redo_create_info.field_len =
				it->m_fields.size() - 1;
			btr_redo_create_info.trx_id_pos = it->m_trx_id_pos;
		}

		root_page_no = create_index(
			table_name, space, it->m_type, it->m_id,
			btr_redo_create_info, &mtr);

		if (root_page_no == FIL_NULL) {
			break;
		}

		it->m_new_root_page_no = root_page_no;
	}

	mtr_commit(&mtr);

	return(root_page_no == FIL_NULL ? DB_ERROR : DB_SUCCESS);
}

/**
Write a TRUNCATE log record for fixing up table if truncate crashes.
@param start_ptr	buffer to write log record
@param end_ptr		buffer end
@param space_id		space id
@param tablename	the table name in the usual databasename/tablename
			format of InnoDB
@param flags		tablespace flags
@param format_flags	page format
@param lsn		lsn while logging
@return DB_SUCCESS or error code */

dberr_t
truncate_t::write(
	byte*		start_ptr,
	byte*		end_ptr,
	ulint		space_id,
	const char*	tablename,
	ulint		flags,
	ulint		format_flags,
	lsn_t		lsn) const
{
	if (end_ptr < start_ptr) {
		return(DB_FAIL);
	}

	/* LSN, Type, Space-ID, format-flag (also know as log_flag.
	Stored in page_no field), tablespace flags */
	if (end_ptr < (start_ptr + (8 + 4 + 4 + 4)))  {
		return(DB_FAIL);
	}

	mach_write_to_8(start_ptr, lsn);
	start_ptr += 8;

	mach_write_to_4(start_ptr, space_id);
	start_ptr += 4;

	mach_write_to_4(start_ptr, format_flags);
	start_ptr += 4;

	mach_write_to_4(start_ptr, flags);
	start_ptr += 4;

	/* Name of the table. */
	/* Include the NUL in the log record. */
	ulint len = strlen(tablename) + 1;
	if (end_ptr < (start_ptr + (len + 2))) {
		return(DB_FAIL);
	}

	mach_write_to_2(start_ptr, len);
	start_ptr += 2;

	memcpy(start_ptr, tablename, len - 1);
	start_ptr += len;

	DBUG_EXECUTE_IF("ib_trunc_crash_while_writing_redo_log",
			DBUG_SUICIDE(););

	/* Old/New Table-ID, Number of Indexes and Tablespace dir-path-name. */
	/* Write the remote directory of the table into mtr log */
	len = m_dir_path != NULL ? strlen(m_dir_path) + 1 : 0;
	if (end_ptr < (start_ptr + (len + 8 + 8 + 2 + 2))) {
		return(DB_FAIL);
	}

	/* Write out old-table-id. */
	mach_write_to_8(start_ptr, m_old_table_id);
	start_ptr += 8;

	/* Write out new-table-id. */
	mach_write_to_8(start_ptr, m_new_table_id);
	start_ptr += 8;

	/* Write out the number of indexes. */
	mach_write_to_2(start_ptr, m_indexes.size());
	start_ptr += 2;

	/* Write the length (NUL included) of the .ibd path. */
	mach_write_to_2(start_ptr, len);
	start_ptr += 2;

	if (m_dir_path != NULL) {
		memcpy(start_ptr, m_dir_path, len - 1);
		start_ptr += len;
	}

	/* Indexes information (id, type) */
	/* Write index ids, type, root-page-no into mtr log */
	for (ulint i = 0; i < m_indexes.size(); ++i) {

		if (end_ptr < (start_ptr + (8 + 4 + 4 + 4))) {
			return(DB_FAIL);
		}

		mach_write_to_8(start_ptr, m_indexes[i].m_id);
		start_ptr += 8;

		mach_write_to_4(start_ptr, m_indexes[i].m_type);
		start_ptr += 4;

		mach_write_to_4(start_ptr, m_indexes[i].m_root_page_no);
		start_ptr += 4;

		mach_write_to_4(start_ptr, m_indexes[i].m_trx_id_pos);
		start_ptr += 4;
	}

	/* If tablespace compressed then field info of each index. */
	if (FSP_FLAGS_GET_ZIP_SSIZE(flags)) {

		for (ulint i = 0; i < m_indexes.size(); ++i) {

			ulint len = m_indexes[i].m_fields.size();
			if (end_ptr < (start_ptr + (len + 2 + 2))) {
				return(DB_FAIL);
			}

			mach_write_to_2(
				start_ptr, m_indexes[i].m_n_fields);
			start_ptr += 2;

			mach_write_to_2(start_ptr, len);
			start_ptr += 2;

			const byte*	ptr = &m_indexes[i].m_fields[0];
			memcpy(start_ptr, ptr, len - 1);
			start_ptr += len;
		}
	}

	return(DB_SUCCESS);
}
