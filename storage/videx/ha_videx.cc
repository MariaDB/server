/* Copyright (c) 2025 Bytedance Ltd. and/or its affiliates

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License, version 2.0,
	as published by the Free Software Foundation.

	This program is also distributed with certain software (including
	but not limited to OpenSSL) that is licensed under separate terms,
	as designated in a particular file or component or in included license
	documentation.  The authors of MySQL hereby grant you an additional
	permission to link the program and your derivative works with the
	separately licensed software that they have included with MySQL.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License, version 2.0, for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <my_global.h>
#include <mysql/plugin.h>
#include "ha_videx.h"
#include "sql_class.h"

/**
 * Sends a request to the Videx HTTP server and validates the response.
 * If the response is successful (code=200), the passed-in &request will be filled.
 *
 * @param request The VidexJsonItem object containing the request data.
 * @param res_json The VidexStringMap object to store the response data.
 * @param thd Pointer to the current thread's THD object.
 * @return 0 if the request is successful, 1 otherwise.
 */
int ask_from_videx_http(VidexJsonItem &request, VidexStringMap &res_json, THD* thd) {
	return 1;
}

static handler *videx_create_handler(handlerton *hton, TABLE_SHARE *table, MEM_ROOT *mem_root);

handlerton *videx_hton;

/**
	@brief
	Function we use in the creation of our hash to get key.
*/

static const char *ha_videx_exts[] = {
	NullS
};

videx_share::videx_share()
{
	thr_lock_init(&lock);
	mysql_mutex_init(ex_key_mutex_videx_share_mutex, &mutex, MY_MUTEX_INIT_FAST);
}

static int videx_init(void *p)
{
	DBUG_ENTER("videx_init");

	videx_hton= static_cast<handlerton*>(p);
	videx_hton_ptr= videx_hton;

	
	videx_hton->create=  videx_create_handler;
	videx_hton->flags= HTON_SUPPORTS_EXTENDED_KEYS | HTON_SUPPORTS_FOREIGN_KEYS |
		HTON_NATIVE_SYS_VERSIONING | HTON_WSREP_REPLICATION |
			HTON_REQUIRES_CLOSE_AFTER_TRUNCATE | HTON_TRUNCATE_REQUIRES_EXCLUSIVE_USE | HTON_REQUIRES_NOTIFY_TABLEDEF_CHANGED_AFTER_COMMIT;

	videx_hton->tablefile_extensions= ha_videx_exts;
	DBUG_RETURN(0);
}

/**
	@brief
	videx of simple lock controls. The "share" it creates is a
	structure we will pass to each videx handler. Do you have to have
	one of these? Well, you have pieces that are used for locking, and
	they are needed to function.
*/

videx_share *ha_videx::get_share() // ✅
{
	videx_share *tmp_share;

	DBUG_ENTER("ha_videx::get_share()");

	lock_shared_ha_data();
	if (!(tmp_share= static_cast<videx_share*>(get_ha_share_ptr())))
	{
		tmp_share= new videx_share;
		if (!tmp_share)
			goto err;

		set_ha_share_ptr(static_cast<Handler_share*>(tmp_share));
	}
err:
	unlock_shared_ha_data();
	DBUG_RETURN(tmp_share);
}

static handler* videx_create_handler(handlerton *hton,
					TABLE_SHARE *table, 
					MEM_ROOT *mem_root) // ✅
{
	return new (mem_root) ha_videx(hton, table);
}

ha_videx::ha_videx(
	handlerton*	hton,
	TABLE_SHARE*	table_arg)
	:handler(hton, table_arg),
	// m_prebuilt(),
	m_user_thd(),
	m_int_table_flags(HA_REC_NOT_IN_SEQ
			  | HA_NULL_IN_KEY
			  | HA_CAN_VIRTUAL_COLUMNS
			  | HA_CAN_INDEX_BLOBS
			  | HA_CAN_SQL_HANDLER
			  | HA_REQUIRES_KEY_COLUMNS_FOR_DELETE
			  | HA_PRIMARY_KEY_REQUIRED_FOR_POSITION
			  | HA_PRIMARY_KEY_IN_READ_INDEX
			  | HA_BINLOG_ROW_CAPABLE
			  | HA_CAN_GEOMETRY
			  | HA_PARTIAL_COLUMN_READ
			  | HA_TABLE_SCAN_ON_INDEX
			  // | HA_CAN_FULLTEXT
			  // | HA_CAN_FULLTEXT_EXT
			  // | HA_CAN_FULLTEXT_HINTS
			  | HA_CAN_EXPORT
        	  | HA_ONLINE_ANALYZE
			  | HA_CAN_RTREEKEYS
			  | HA_CAN_TABLES_WITHOUT_ROLLBACK
			  | HA_CAN_ONLINE_BACKUPS
			  | HA_CONCURRENT_OPTIMIZE
			  | HA_CAN_SKIP_LOCKED
		  ),
	m_start_of_scan(),
    m_mysql_has_locked()
{}

ha_videx::~ha_videx() = default;

ulonglong ha_videx::table_version() const
{
	// TODO:
	return 0;
}

enum row_type ha_videx::get_row_type() const
{
	// TODO:
	return ROW_TYPE_NOT_USED;
}

const char* ha_videx::table_type() const // ✅
{
	return "VIDEX";
}

handler::Table_flags ha_videx::table_flags() const
{
	THD*			thd = ha_thd();
	handler::Table_flags	flags = m_int_table_flags;

	if (thd_sql_command(thd) == SQLCOM_CREATE_TABLE) {
		flags|= HA_REQUIRE_PRIMARY_KEY;
	}

	/* Need to use tx_isolation here since table flags is (also)
	called before prebuilt is inited. */

	if (thd_tx_isolation(thd) <= ISO_READ_COMMITTED) {
		return(flags);
	}

	return(flags | HA_BINLOG_STMT_CAPABLE);
}

ulong ha_videx::index_flags( // ✅
	uint	key,
	uint,
	bool) const
{
	if (table_share->key_info[key].algorithm == HA_KEY_ALG_FULLTEXT) {
		return(0);
	}

	/* For spatial index, we don't support descending scan
	and ICP so far. */
	if (table_share->key_info[key].algorithm == HA_KEY_ALG_RTREE) {
		return HA_READ_NEXT | HA_READ_ORDER| HA_READ_RANGE
			| HA_KEYREAD_ONLY | HA_KEY_SCAN_NOT_ROR;
	}

	ulong flags= key == table_share->primary_key
		? HA_CLUSTERED_INDEX : HA_KEYREAD_ONLY | HA_DO_RANGE_FILTER_PUSHDOWN;

	flags |= HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER
		| HA_READ_RANGE
		| HA_DO_INDEX_COND_PUSHDOWN;
	return(flags);
}

uint ha_videx::max_supported_keys() const // ✅
{
	return(MAX_KEY);
}

uint ha_videx::max_supported_key_length() const // ✅
{
	return(3500);
}

uint ha_videx::max_supported_key_part_length() const // ✅
{
	return(3072);
}

const key_map* ha_videx::keys_to_use_for_scanning() // ✅
{
	return(&key_map_full);
}

void ha_videx::column_bitmaps_signal()
{
	DBUG_ENTER("ha_videx::column_bitmaps_signal");
	// TODO:
	// indexed virtual columns
	DBUG_VOID_RETURN;
}

/**
	@brief
	Used for opening tables. The name will be the name of the file.

	@details
	A table is opened when it needs to be opened; e.g. when a request comes in
	for a SELECT on the table (tables are not open and closed for each request,
	they are cached).

	Called from handler.cc by handler::ha_open(). The server opens all tables by
	calling ha_open() which then calls the handler specific open().

	@see
	handler::ha_open() in handler.cc
*/

int ha_videx::open(const char *name, int mode, uint test_if_locked)
{
	DBUG_ENTER("ha_videx::open");

	if (!(share = get_share()))
		DBUG_RETURN(1);
	thr_lock_data_init(&share->lock,&lock,NULL);

	m_primary_key = table->s->primary_key;

	if (m_primary_key  >= MAX_KEY) {
		ref_length = 6; // DATA_ROW_ID_LEN;
	}
	else {
		ref_length = table->key_info[m_primary_key].key_length;
	}

	stats.block_size = 16384;

	info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST
		| HA_STATUS_OPEN);

	DBUG_RETURN(0);
}

/**
	@brief
	Closes a table.

	@details
	Called from sql_base.cc, sql_select.cc, and table.cc. In sql_select.cc it is
	only used to close up temporary tables or during the process where a
	temporary table is converted over to being a myisam table.

	For sql_base.cc look at close_data_tables().

	@see
	sql_base.cc, sql_select.cc and table.cc
*/

int ha_videx::close(void) // ✅
{
	DBUG_ENTER("ha_videx::close");
	DBUG_RETURN(0);
}

handler* ha_videx::clone(
	const char*	name,		/*!< in: table name */
	MEM_ROOT*	mem_root)	/*!< in: memory context */
{
	// TODO:
	DBUG_ENTER("ha_videx::clone");
	DBUG_RETURN(NULL);
}

IO_AND_CPU_COST ha_videx::scan_time()
{
	DBUG_ENTER("ha_videx::scan_time");

	DBUG_RETURN(handler::scan_time());
}

IO_AND_CPU_COST ha_videx::rnd_pos_time(ha_rows rows)
{
	DBUG_ENTER("ha_videx::rnd_pos_time");
	DBUG_RETURN(handler::rnd_pos_time(rows));
}

/**
	@brief
	write_row() inserts a row. No extra() hint is given currently if a bulk load
	is happening. buf() is a byte array of data. You can use the field
	information to extract the data from the native byte array type.

	@details
	videx of this would be:
	@code
	for (Field **field=table->field ; *field ; field++)
	{
		...
	}
	@endcode

	See ha_tina.cc for an example of extracting all of the data as strings.

	See the note for update_row() on auto_increments and timestamps. This
	case also applies to write_row().

	Called from item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
	sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc, and sql_update.cc.

	@see
	item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
	sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc and sql_update.cc
*/

int ha_videx::write_row(const uchar *buf) // ✅
{
	DBUG_ENTER("ha_videx::write_row");

	DBUG_RETURN(0);
}

/**
	@brief
	Yes, update_row() does what you expect, it updates a row. old_data will have
	the previous row record in it, while new_data will have the newest data in it.
	Keep in mind that the server can do updates based on ordering if an ORDER BY
	clause was used. Consecutive ordering is not guaranteed.

	@details
	Currently new_data will not have an updated auto_increament record, or
	and updated timestamp field. You can do these for videx by doing:
	@code
	if (table->next_number_field && record == table->record[0])
		update_auto_increment();
	@endcode

	Called from sql_select.cc, sql_acl.cc, sql_update.cc, and sql_insert.cc.

	@see
	sql_select.cc, sql_acl.cc, sql_update.cc and sql_insert.cc
*/
int ha_videx::update_row(const uchar *old_data, const uchar *new_data) // ✅
{
	DBUG_ENTER("ha_videx::update_row");

	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

/**
	@brief
	This will delete a row. buf will contain a copy of the row to be deleted.
	The server will call this right after the current row has been called (from
	either a previous rnd_nexT() or index call).

	@details
	If you keep a pointer to the last row or can access a primary key it will
	make doing the deletion quite a bit easier. Keep in mind that the server does
	not guarantee consecutive deletions. ORDER BY clauses can be used.

	Called in sql_acl.cc and sql_udf.cc to manage internal table
	information.  Called in sql_delete.cc, sql_insert.cc, and
	sql_select.cc. In sql_select it is used for removing duplicates
	while in insert it is used for REPLACE calls.

	@see
	sql_acl.cc, sql_udf.cc, sql_delete.cc, sql_insert.cc and sql_select.cc
*/

int ha_videx::delete_row(const uchar *buf) // ✅
{
	DBUG_ENTER("ha_videx::delete_row");

	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

bool ha_videx::was_semi_consistent_read()
{
	DBUG_ENTER("ha_videx::was_semi_consistent_read");
	// TODO:
	DBUG_RETURN(false);
}

void ha_videx::try_semi_consistent_read(bool yes)
{
	DBUG_ENTER("ha_videx::try_semi_consistent_read");
	// TODO:
	DBUG_VOID_RETURN;
}

void ha_videx::unlock_row() // ✅
{
	DBUG_ENTER("ha_videx::unlock_row");

	DBUG_VOID_RETURN;
}

/**
	@brief
	Used to read forward through the index.
*/

int ha_videx::index_next(uchar *buf) // ✅
{
	int rc;
	DBUG_ENTER("ha_videx::index_next");
	rc= HA_ERR_WRONG_COMMAND;
	DBUG_RETURN(rc);
}

/**
	@brief
	Used to read backwards through the index.
*/

int ha_videx::index_prev(uchar *buf) // ✅
{
	int rc;
	DBUG_ENTER("ha_videx::index_prev");
	rc= HA_ERR_WRONG_COMMAND;
	DBUG_RETURN(rc);
}

/**
	@brief
	index_first() asks for the first key in the index.

	@details
	Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

	@see
	opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_videx::index_first(uchar *buf) // ✅
{
	int rc;
	DBUG_ENTER("ha_videx::index_first");
	rc= HA_ERR_WRONG_COMMAND;
	DBUG_RETURN(rc);
}

/**
	@brief
	index_last() asks for the last key in the index.

	@details
	Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

	@see
	opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_videx::index_last(uchar *buf) // ✅
{
	int rc;
	DBUG_ENTER("ha_videx::index_last");
	rc= HA_ERR_WRONG_COMMAND;
	DBUG_RETURN(rc);
}

/**
	@brief
	rnd_init() is called when the system wants the storage engine to do a table
	scan. See the example in the introduction at the top of this file to see when
	rnd_init() is called.

	@details
	Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
	and sql_update.cc.

	@see
	filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int ha_videx::rnd_init(bool scan) // ✅
{
	DBUG_ENTER("ha_videx::rnd_init");
	DBUG_RETURN(0);
}

int ha_videx::rnd_end() // ✅
{
	DBUG_ENTER("ha_videx::rnd_end");
	DBUG_RETURN(0);
}

/**
	@brief
	This is called for each row of the table scan. When you run out of records
	you should return HA_ERR_END_OF_FILE. Fill buff up with the row information.
	The Field structure for the table is the key to getting data into buf
	in a manner that will allow the server to understand it.

	@details
	Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
	and sql_update.cc.

	@see
	filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int ha_videx::rnd_next(uchar *buf) // ✅
{
	int rc;
	DBUG_ENTER("ha_videx::rnd_next");
	rc= HA_ERR_END_OF_FILE;
	DBUG_RETURN(rc);
}

/**
	@brief
	This is like rnd_next, but you are given a position to use
	to determine the row. The position will be of the type that you stored in
	ref. You can use ha_get_ptr(pos,ref_length) to retrieve whatever key
	or position you saved when position() was called.

	@details
	Called from filesort.cc, records.cc, sql_insert.cc, sql_select.cc, and sql_update.cc.

	@see
	filesort.cc, records.cc, sql_insert.cc, sql_select.cc and sql_update.cc
*/
int ha_videx::rnd_pos(uchar *buf, uchar *pos) // ✅
{
	int rc;
	DBUG_ENTER("ha_videx::rnd_pos");
	rc= HA_ERR_WRONG_COMMAND;
	DBUG_RETURN(rc);
}

/**
	@brief
	position() is called after each call to rnd_next() if the data needs
	to be ordered. You can do something like the following to store
	the position:
	@code
	my_store_ptr(ref, ref_length, current_position);
	@endcode

	@details
	The server uses ref to store data. ref_length in the above case is
	the size needed to store current_position. ref is just a byte array
	that the server will maintain. If you are using offsets to mark rows, then
	current_position should be the offset. If it is a primary key like in
	BDB, then it needs to be a primary key.

	Called from filesort.cc, sql_select.cc, sql_delete.cc, and sql_update.cc.

	@see
	filesort.cc, sql_select.cc, sql_delete.cc and sql_update.cc
*/
void ha_videx::position(const uchar *record) // ✅
{
	DBUG_ENTER("ha_videx::position");
	DBUG_VOID_RETURN;
}

/**
	@brief
	::info() is used to return information to the optimizer. See my_base.h for
	the complete description.

	@details
	Currently this table handler doesn't implement most of the fields really needed.
	SHOW also makes use of this data.

	You will probably want to have the following in your code:
	@code
	if (records < 2)
		records = 2;
	@endcode
	The reason is that the server will optimize for cases of only a single
	record. If, in a table scan, you don't know the number of records, it
	will probably be better to set records to two so you can return as many
	records as you need. Along with records, a few more variables you may wish
	to set are:
		records
		deleted
		data_file_length
		index_file_length
		delete_length
		check_time
	Take a look at the public variables in handler.h for more information.

	Called in filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc,
	sql_delete.cc, sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc,
	sql_select.cc, sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc,
	sql_table.cc, sql_union.cc, and sql_update.cc.

	@see
	filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc, sql_delete.cc,
	sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc, sql_select.cc,
	sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_table.cc,
	sql_union.cc and sql_update.cc
*/
int ha_videx::info(uint flag) // ✅
{
	return (info_low(flag, false));
}

/**
	@brief
	extra() is called whenever the server wishes to send a hint to
	the storage engine. The myisam engine implements the most hints.
	ha_innodb.cc has the most exhaustive list of these hints.

	@see
	ha_innodb.cc
*/
int ha_videx::extra(enum ha_extra_function operation)
{
	DBUG_ENTER("ha_videx::extra");
	// TODO:
	DBUG_RETURN(0);
}

int ha_videx::reset()
{
	DBUG_ENTER("ha_videx::reset");
	// TODO:
	DBUG_RETURN(0);
}

// /**
//   @brief
//   Used to delete all rows in a table, including cases of truncate and cases where
//   the optimizer realizes that all rows will be removed as a result of an SQL statement.

//   @details
//   Called from item_sum.cc by Item_func_group_concat::clear(),
//   Item_sum_count_distinct::clear(), and Item_func_group_concat::clear().
//   Called from sql_delete.cc by mysql_delete().
//   Called from sql_select.cc by JOIN::reinit().
//   Called from sql_union.cc by st_select_lex_unit::exec().

//   @see
//   Item_func_group_concat::clear(), Item_sum_count_distinct::clear() and
//   Item_func_group_concat::clear() in item_sum.cc;
//   mysql_delete() in sql_delete.cc;
//   JOIN::reinit() in sql_select.cc and
//   st_select_lex_unit::exec() in sql_union.cc.
// */
// int ha_videx::delete_all_rows() // ✅
// {
//   DBUG_ENTER("ha_videx::delete_all_rows");
//   DBUG_RETURN(HA_ERR_WRONG_COMMAND);
// }

/**
	@brief
	This create a lock on the table. If you are implementing a storage engine
	that can handle transacations look at ha_berkely.cc to see how you will
	want to go about doing this. Otherwise you should consider calling flock()
	here. Hint: Read the section "locking functions for mysql" in lock.cc to understand
	this.

	@details
	Called from lock.cc by lock_external() and unlock_external(). Also called
	from sql_table.cc by copy_data_between_tables().

	@see
	lock.cc by lock_external() and unlock_external() in lock.cc;
	the section "locking functions for mysql" in lock.cc;
	copy_data_between_tables() in sql_table.cc.
*/
int ha_videx::external_lock(THD *thd, int lock_type) // ✅
{
	DBUG_ENTER("ha_videx::external_lock");
	DBUG_RETURN(0);
}

/**
	@brief
	The idea with handler::store_lock() is: The statement decides which locks
	should be needed for the table. For updates/deletes/inserts we get WRITE
	locks, for SELECT... we get read locks.

	@details
	Before adding the lock into the table lock handler (see thr_lock.c),
	mysqld calls store lock with the requested locks. Store lock can now
	modify a write lock to a read lock (or some other lock), ignore the
	lock (if we don't want to use MariaDB table locks at all), or add locks
	for many tables (like we do when we are using a MERGE handler).

	Berkeley DB, for example, changes all WRITE locks to TL_WRITE_ALLOW_WRITE
	(which signals that we are doing WRITES, but are still allowing other
	readers and writers).

	When releasing locks, store_lock() is also called. In this case one
	usually doesn't have to do anything.

	In some exceptional cases MariaDB may send a request for a TL_IGNORE;
	This means that we are requesting the same lock as last time and this
	should also be ignored. (This may happen when someone does a flush
	table when we have opened a part of the tables, in which case mysqld
	closes and reopens the tables and tries to get the same locks at last
	time). In the future we will probably try to remove this.

	Called from lock.cc by get_lock_data().

	@note
	In this method one should NEVER rely on table->in_use, it may, in fact,
	refer to a different thread! (this happens if get_lock_data() is called
	from mysql_lock_abort_for_thread() function)

	@see
	get_lock_data() in lock.cc
*/
THR_LOCK_DATA **ha_videx::store_lock(THD *thd,
                                       THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type) // ✅
{
	if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
		lock.type=lock_type;
	*to++= &lock;
	return to;
}

/**
	@brief
	Given a starting key and an ending key, estimate the number of rows that
	will exist between the two keys.
	The handler can also optionally update the 'pages' parameter with the page
	number that contains the min and max keys. This will help the optimizer
	to know if two ranges are partly on the same pages and if the min and
	max key are on the same page.

	@details
	end_key may be empty, in which case determine if start_key matches any rows.

	Called from opt_range.cc by check_quick_keys().

	@see
	check_quick_keys() in opt_range.cc
*/
ha_rows ha_videx::records_in_range(uint inx,
                                     const key_range *min_key,
                                     const key_range *max_key,
                                     page_range *pages)
{
	DBUG_ENTER("ha_videx::records_in_range");
	// TODO:
	DBUG_RETURN(10);                         // low number to force index usage
}

ha_rows ha_videx::estimate_rows_upper_bound()
{
	DBUG_ENTER("ha_videx::estimate_rows_upper_bound");
	// TODO:
	DBUG_RETURN(10);
}

/**
	@brief
	create() is called to create a database. The variable name will have the name
	of the table.

	@details
	When create() is called you do not need to worry about
	opening the table. Also, the .frm file will have already been
	created so adjusting create_info is not necessary. You can overwrite
	the .frm file at this point if you wish to change the table
	definition, but there are no methods currently provided for doing
	so.

	Called from handle.cc by ha_create_table().

	@see
	ha_create_table() in handle.cc
*/

int ha_videx::create(const char *name, TABLE *table_arg,
                       HA_CREATE_INFO *create_info)
{
	DBUG_ENTER("ha_videx::create");
	DBUG_PRINT("info", ("name: %s, table_arg: %p, create_info: %p", name, table_arg, create_info));
	DBUG_RETURN(0);
}

int ha_videx::delete_table(const char *name) // ✅
{
	DBUG_ENTER("ha_videx::delete_table");
	/* This is not implemented but we want someone to be able that it works. */
	DBUG_RETURN(0);
}

int ha_videx::rename_table(const char* from, const char* to) // ✅
{
	DBUG_ENTER("ha_videx::rename_table");
	DBUG_RETURN(0);
}

/**
	**************************************************************************
	* DS-MRR implementation
	**************************************************************************
*/

/**
	Multi Range Read interface, DS-MRR calls */
int ha_videx::multi_range_read_init(
	RANGE_SEQ_IF*	seq,
	void*		seq_init_param,
	uint		n_ranges,
	uint		mode,
	HANDLER_BUFFER*	buf) // ✅
{
	return(m_ds_mrr.dsmrr_init(this, seq, seq_init_param,
				 n_ranges, mode, buf));
}

int ha_videx::multi_range_read_next(
	range_id_t*		range_info) // ✅
{
	return(m_ds_mrr.dsmrr_next(range_info));
}

ha_rows ha_videx::multi_range_read_info_const(
	uint		keyno,
	RANGE_SEQ_IF*	seq,
	void*		seq_init_param,
	uint		n_ranges,
	uint*		bufsz,
	uint*		flags,
	ha_rows         limit,
	Cost_estimate*	cost) // ✅
{
	/* See comments in ha_myisam::multi_range_read_info_const */
	m_ds_mrr.init(this, table);

	// if (m_prebuilt->select_lock_type != LOCK_NONE) {
	// 	*flags |= HA_MRR_USE_DEFAULT_IMPL;
	// }

	return (m_ds_mrr.dsmrr_info_const(keyno, seq, seq_init_param,
									   n_ranges,
									   bufsz, flags, limit, cost));
}

ha_rows ha_videx::multi_range_read_info(
	uint		keyno,
	uint		n_ranges,
	uint		keys,
	uint		key_parts,
	uint*		bufsz,
	uint*		flags,
	Cost_estimate*	cost) // ✅
{
	m_ds_mrr.init(this, table);
	return (m_ds_mrr.dsmrr_info(keyno, n_ranges, keys, key_parts, bufsz,
					flags, cost));
}

int ha_videx::multi_range_read_explain_info(uint mrr_mode,
	char *str, size_t size) // ✅
{
	return(m_ds_mrr.dsmrr_explain_info(mrr_mode, str, size));
}

/** Attempt to push down an index condition.
@param[in] keyno MySQL key number
@param[in] idx_cond Index condition to be checked
@return Part of idx_cond which the handler will not evaluate */

class Item* ha_videx::idx_cond_push(
	uint		keyno,
	class Item*	idx_cond) // ✅
{
	DBUG_ENTER("ha_videx::idx_cond_push");
	DBUG_ASSERT(keyno != MAX_KEY);
	DBUG_ASSERT(idx_cond != NULL);

	// /* We can only evaluate the condition if all columns are stored.*/
	// dict_index_t* idx  = innobase_get_index(keyno);
	// if (idx && dict_index_has_virtual(idx)) {
	// 	DBUG_RETURN(idx_cond);
	// }

	pushed_idx_cond = idx_cond;
	pushed_idx_cond_keyno = keyno;
	in_range_check_pushed_down = TRUE;
	/* We will evaluate the condition entirely */
	DBUG_RETURN(NULL);
}

int ha_videx::info_low(uint flag, bool is_analyze)
{
	// dict_table_t*	ib_table;
	uint64_t	n_rows;
	char		path[FN_REFLEN];
	// os_file_stat_t	stat_info;

	DBUG_ENTER("ha_videx::info");
	DEBUG_SYNC_C("ha_innobase_info_low");

	// construct request
	VidexStringMap res_json;
	VidexJsonItem request_item = construct_request(table->s->db.str, table->s->table_name.str, __PRETTY_FUNCTION__);
	for (uint i = 0; i < table->s->keys; i++) {
		KEY *key = &table->key_info[i];
		VidexJsonItem * keyItem = request_item.create("key");
		keyItem->add_property("name", key->name.str);
		keyItem->add_property_nonan("key_length", key->key_length);
		for (ulong j = 0; j < key->usable_key_parts; j++) {
			if ((key->flags & HA_KEY_ALG_FULLTEXT) || (key->flags & HA_SPATIAL_legacy)) {
				continue;
			}
			VidexJsonItem* field = keyItem->create("field");
			field->add_property("name", key->key_part[j].field->field_name.str);
			field->add_property_nonan("store_length", key->key_part[j].store_length);
		}
	}
  
	DBUG_PRINT("info", ("Request JSON: %s", request_item.to_json().c_str()));
	std::cout << "Request JSON:" << std::endl;
	std::cout << request_item.to_json() << std::endl;
	std::cout << "=== END DEBUG OUTPUT ===" << std::endl;

	THD* thd = ha_thd();
	int error = ask_from_videx_http(request_item, res_json, thd);
  	if (error) {
  		std::cout << "ask_from_videx_http failed, using default values" << std::endl;
  		// Set default values when HTTP request fails

		// nation table
  		res_json["nation #@# stat_n_rows"] = "25";
		res_json["nation #@# data_file_length"] = "16384";
		res_json["nation #@# index_file_length"] = "16384";
  		res_json["nation #@# data_free_length"] = "0";
		res_json["nation #@# rec_per_key #@# PRIMARY #@# N_NATIONKEY"] = "1";
		res_json["nation #@# rec_per_key #@# NATION_FK1 #@# N_REGIONKEY"] = "5";
		res_json["nation #@# rec_per_key #@# NATION_FK1 #@# N_NATIONKEY"] = "1";

		// orders table
		res_json["orders #@# stat_n_rows"] = "14907";
		res_json["orders #@# data_file_length"] = "2637824";
		res_json["orders #@# index_file_length"] = "245760";
  		res_json["orders #@# data_free_length"] = "0";
		res_json["orders #@# rec_per_key #@# PRIMARY #@# O_ORDERKEY"] = "1";
		res_json["orders #@# rec_per_key #@# ORDERS_FK1 #@# O_CUSTKEY"] = "1";
		res_json["orders #@# rec_per_key #@# ORDERS_FK1 #@# O_ORDERKEY"] = "1";

		// customer table
		res_json["customer #@# stat_n_rows"] = "1545";
		res_json["customer #@# data_file_length"] = "327680";
		res_json["customer #@# index_file_length"] = "49152";
  		res_json["customer #@# data_free_length"] = "0";
		res_json["customer #@# rec_per_key #@# PRIMARY #@# C_CUSTKEY"] = "1";
		res_json["customer #@# rec_per_key #@# CUSTOMER_FK1 #@# C_NATIONKEY"] = "61";
		res_json["customer #@# rec_per_key #@# CUSTOMER_FK1 #@# C_CUSTKEY"] = "1";
  	}
	else {
		// validate the returned json
    	// stat_n_rows。stat_clustered_index_size。stat_sum_of_other_index_sizes。data_file_length。index_file_length。data_free_length
		if (!(
			videx_contains_key(res_json, "stat_n_rows") &&
			videx_contains_key(res_json, "stat_clustered_index_size") &&
			videx_contains_key(res_json, "stat_sum_of_other_index_sizes") &&
			videx_contains_key(res_json, "data_file_length") &&
			videx_contains_key(res_json, "index_file_length") &&
			videx_contains_key(res_json, "data_free_length")
		)) {
			std::cout << "res_json data error=0 but miss some key." << std::endl;
			DBUG_RETURN(0);
		}
	}

	// update_thd(ha_thd());
	//
	// m_prebuilt->trx->op_info = "returning various info to MariaDB";

	// ib_table = m_prebuilt->table;
	// DBUG_ASSERT(ib_table->get_ref_count() > 0);

	// if (!ib_table->is_readable()
	//     || srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN) {
	// 	dict_stats_empty_table(ib_table);
 //        } else if (flag & HA_STATUS_TIME) {
	// 	stats.update_time = ib_table->update_time;
	// 	if (!is_analyze && !innobase_stats_on_metadata) {
	// 		goto stats_fetch;
	// 	}

		// dberr_t ret;
		// m_prebuilt->trx->op_info = "updating table statistics";

// 		if (ib_table->stats_is_persistent()
// 		    && !srv_read_only_mode
// 		    && dict_stats_persistent_storage_check(false)
// 		    == SCHEMA_OK) {
// 			if (is_analyze) {
// 				dict_stats_recalc_pool_del(ib_table->id,
// 							   false);
// recalc:
// 				ret = statistics_init(ib_table, is_analyze);
// 			} else {
// 				/* This is e.g. 'SHOW INDEXES' */
// 				ret = statistics_init(ib_table, is_analyze);
// 				switch (ret) {
// 				case DB_SUCCESS:
// 				case DB_READ_ONLY:
// 					break;
// 				default:
// 					goto error;
// 				case DB_STATS_DO_NOT_EXIST:
// 					if (!ib_table
// 					    ->stats_is_auto_recalc()) {
// 						break;
// 					}
//
// 					if (opt_bootstrap) {
// 						break;
// 					}
// #ifdef WITH_WSREP
// 					if (wsrep_thd_skip_locking(
// 						    m_user_thd)) {
// 						break;
// 					}
// #endif
// 					is_analyze = true;
// 					goto recalc;
// 				}
// 			}
// 		} else {
// 			ret = dict_stats_update_transient(ib_table);
// 			if (ret != DB_SUCCESS) {
// error:
// 				m_prebuilt->trx->op_info = "";
// 				DBUG_RETURN(HA_ERR_GENERIC);
// 			}
// 		}

// 		m_prebuilt->trx->op_info = "returning various info to MariaDB";
// 	} else {
// stats_fetch:
// 		statistics_init(ib_table, false);
// 	}

	if (flag & HA_STATUS_VARIABLE) {

		// ulint	stat_clustered_index_size;
		// ulint	stat_sum_of_other_index_sizes;

// #if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
// 		if (xbegin()) {
// 			if (ib_table->stats_mutex_is_locked())
// 				xabort();
//
// 			ut_ad(ib_table->stat_initialized());
//
// 			n_rows = ib_table->stat_n_rows;
//
// 			stat_clustered_index_size
// 				= ib_table->stat_clustered_index_size;
//
// 			stat_sum_of_other_index_sizes
// 				= ib_table->stat_sum_of_other_index_sizes;
//
// 			xend();
// 		} else
// #endif
// 		{
// 			ib_table->stats_shared_lock();
//
// 			ut_ad(ib_table->stat_initialized());
//
// 			n_rows = ib_table->stat_n_rows;
//
// 			stat_clustered_index_size
// 				= ib_table->stat_clustered_index_size;
//
// 			stat_sum_of_other_index_sizes
// 				= ib_table->stat_sum_of_other_index_sizes;
//
// 			ib_table->stats_shared_unlock();
// 		}

		std::string concat_stat_n_rows = std::string(table->s->table_name.str) + " #@# stat_n_rows";
		n_rows = std::stoull(res_json[concat_stat_n_rows.c_str()]);
		DBUG_PRINT("videx", ("n_rows: %lu", n_rows));

		/*
		The MySQL optimizer seems to assume in a left join that n_rows
		is an accurate estimate if it is zero. Of course, it is not,
		since we do not have any locks on the rows yet at this phase.
		Since SHOW TABLE STATUS seems to call this function with the
		HA_STATUS_TIME flag set, while the left join optimizer does not
		set that flag, we add one to a zero value if the flag is not
		set. That way SHOW TABLE STATUS will show the best estimate,
		while the optimizer never sees the table empty. */

		if (n_rows == 0 && !(flag & (HA_STATUS_TIME | HA_STATUS_OPEN))) {
			n_rows++;
		}

		/* Fix bug#40386: Not flushing query cache after truncate.
		n_rows can not be 0 unless the table is empty, set to 1
		instead. The original problem of bug#29507 is actually
		fixed in the server code. */
		// if (thd_sql_command(m_user_thd) == SQLCOM_TRUNCATE) {

		// 	n_rows = 1;

		// 	/* We need to reset the m_prebuilt value too, otherwise
		// 	checks for values greater than the last value written
		// 	to the table will fail and the autoinc counter will
		// 	not be updated. This will force write_row() into
		// 	attempting an update of the table's AUTOINC counter. */

		// 	m_prebuilt->autoinc_last_value = 0;
		// }

		stats.records = (ha_rows) n_rows;
		stats.deleted = 0;

		std::string concat_data_file_length = std::string(table->s->table_name.str) + " #@# data_file_length";
		std::string concat_index_file_length = std::string(table->s->table_name.str) + " #@# index_file_length";

	    stats.data_file_length = std::stoull(res_json[concat_data_file_length.c_str()]);
	    stats.index_file_length = std::stoull(res_json[concat_index_file_length.c_str()]);
		DBUG_PRINT("videx", ("stats.data_file_length: %llu", stats.data_file_length));
		DBUG_PRINT("videx", ("stats.index_file_length: %llu", stats.index_file_length));
	    if (flag & HA_STATUS_VARIABLE_EXTRA)
	    {
			std::string concat_data_free_length = std::string(table->s->table_name.str) + " #@# data_free_length";
			stats.delete_length = std::stoull(res_json[concat_data_free_length.c_str()]);
			DBUG_PRINT("videx", ("stats.delete_length: %llu", stats.delete_length));
        }
		// if (fil_space_t* space = ib_table->space) {
		// 	const ulint size = space->physical_size();
		// 	stats.data_file_length
		// 		= ulonglong(stat_clustered_index_size)
		// 		* size;
		// 	stats.index_file_length
		// 		= ulonglong(stat_sum_of_other_index_sizes)
		// 		* size;
		// 	if (flag & HA_STATUS_VARIABLE_EXTRA) {
		// 		space->s_lock();
		// 		stats.delete_length = 1024
		// 			* fsp_get_available_space_in_free_extents(
		// 			*space);
		// 		space->s_unlock();
		// 	}
		// }
		stats.check_time = 0;
		stats.mrr_length_per_rec= (uint)ref_length +  8; // 8 = max(sizeof(void *));

		if (stats.records == 0) {
			stats.mean_rec_length = 0;
		} else {
			stats.mean_rec_length = (ulong)
				(stats.data_file_length / stats.records);
		}
	}

	if (flag & HA_STATUS_CONST) {
		/* Verify the number of index in InnoDB and MySQL
		matches up. If m_prebuilt->clust_index_was_generated
		holds, InnoDB defines GEN_CLUST_INDEX internally */
		// ulint	num_innodb_index = UT_LIST_GET_LEN(ib_table->indexes)
		// 	- m_prebuilt->clust_index_was_generated;
		// if (table->s->keys < num_innodb_index) {
		// 	/* If there are too many indexes defined
		// 	inside InnoDB, ignore those that are being
		// 	created, because MySQL will only consider
		// 	the fully built indexes here. */
		//
		// 	for (const dict_index_t* index
		// 		     = UT_LIST_GET_FIRST(ib_table->indexes);
		// 	     index != NULL;
		// 	     index = UT_LIST_GET_NEXT(indexes, index)) {
		//
		// 		/* First, online index creation is
		// 		completed inside InnoDB, and then
		// 		MySQL attempts to upgrade the
		// 		meta-data lock so that it can rebuild
		// 		the .frm file. If we get here in that
		// 		time frame, dict_index_is_online_ddl()
		// 		would not hold and the index would
		// 		still not be included in TABLE_SHARE. */
		// 		if (!index->is_committed()) {
		// 			num_innodb_index--;
		// 		}
		// 	}
		//
		// 	if (table->s->keys < num_innodb_index
		// 	    && innobase_fts_check_doc_id_index(
		// 		    ib_table, NULL, NULL)
		// 	    == FTS_EXIST_DOC_ID_INDEX) {
		// 		num_innodb_index--;
		// 	}
		// }

		// if (table->s->keys != num_innodb_index) {
		// 	ib_table->dict_frm_mismatch = DICT_FRM_INCONSISTENT_KEYS;
		// 	ib_push_frm_error(m_user_thd, ib_table, table, num_innodb_index, true);
		// }

		snprintf(path, sizeof(path), "%s/%s%s",
			 mysql_data_home, table->s->normalized_path.str,
			 reg_ext);

		unpack_filename(path,path);

		/* Note that we do not know the access time of the table,
		nor the CHECK TABLE time, nor the UPDATE or INSERT time. */

		// if (os_file_get_status(
		// 	    path, &stat_info, false,
		// 	    srv_read_only_mode) == DB_SUCCESS) {
		// 	stats.create_time = (ulong) stat_info.ctime;
		// }

		// ib_table->stats_shared_lock();
		// auto _ = make_scope_exit([ib_table]() {
		// 	ib_table->stats_shared_unlock(); });
		//
		// ut_ad(ib_table->stat_initialized());

		for (uint i = 0; i < table->s->keys; i++) {
			ulong	j;

			// dict_index_t* index = innobase_get_index(i);
			//
			// if (index == NULL) {
			// 	ib_table->dict_frm_mismatch = DICT_FRM_INCONSISTENT_KEYS;
			// 	ib_push_frm_error(m_user_thd, ib_table, table, num_innodb_index, true);
			// 	break;
			// }

			KEY*	key = &table->key_info[i];

			for (j = 0; j < key->ext_key_parts; j++) {

				if ((key->algorithm == HA_KEY_ALG_FULLTEXT)
				    || (key->algorithm == HA_KEY_ALG_RTREE)) {

					/* The record per key does not apply to
					FTS or Spatial indexes. */
				/*
					key->rec_per_key[j] = 1;
					key->set_records_per_key(j, 1.0);
				*/
					continue;
				}

				// if (j + 1 > index->n_uniq) {
				// 	sql_print_error(
				// 		"Index %s of %s has %u columns"
				// 	        " unique inside InnoDB, but "
				// 		"server is asking statistics for"
				// 	        " %lu columns. Have you mixed "
				// 		"up .frm files from different "
				// 		" installations? %s",
				// 		index->name(),
				// 		ib_table->name.m_name,
				// 		index->n_uniq, j + 1,
				// 		TROUBLESHOOTING_MSG);
				// 	break;
				// }

				/* innodb_rec_per_key() will use
				index->stat_n_diff_key_vals[] and the value we
				pass index->table->stat_n_rows. Both are
				calculated by ANALYZE and by the background
				stats gathering thread (which kicks in when too
				much of the table has been changed). In
				addition table->stat_n_rows is adjusted with
				each DML (e.g. ++ on row insert). Those
				adjustments are not MVCC'ed and not even
				reversed on rollback. So,
				index->stat_n_diff_key_vals[] and
				index->table->stat_n_rows could have been
				calculated at different time. This is
				acceptable. */

				// ulong	rec_per_key_int = static_cast<ulong>(
				// 	innodb_rec_per_key(index, j,
				// 			   stats.records));
				//
				// if (rec_per_key_int == 0) {
				// 	rec_per_key_int = 1;
				// }

				std::string concat_key = std::string(table->s->table_name.str) + " #@# rec_per_key #@# " + key->name.str + " #@# " + key->key_part[j].field->field_name.str;
				ulong rec_per_key_int = 0;
			  	if (videx_contains_key(res_json, concat_key.c_str())){
		        	rec_per_key_int = std::stoul(res_json[concat_key.c_str()]);
		        }
				else
				{
			    	rec_per_key_int = stats.records;
			  	}
			    if (rec_per_key_int == 0) {
		        	rec_per_key_int = 1;
		        }
			    key->rec_per_key[j] = rec_per_key_int;
				DBUG_PRINT("videx", ("key->rec_per_key[%lu]: %lu", j, key->rec_per_key[j]));
			}
		}
	}

	// if (srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN) {
	//
	// 	goto func_exit;

	// } else if (flag & HA_STATUS_ERRKEY) {
	// 	const dict_index_t*	err_index;
	//
	// 	ut_a(m_prebuilt->trx);
	// 	ut_a(m_prebuilt->trx->magic_n == TRX_MAGIC_N);
	//
	// 	err_index = trx_get_error_info(m_prebuilt->trx);
	//
	// 	if (err_index) {
	// 		errkey = innobase_get_mysql_key_number_for_index(
	// 				table, ib_table, err_index);
	// 	} else {
	// 		errkey = (unsigned int) (
	// 			(m_prebuilt->trx->error_key_num
	// 			 == ULINT_UNDEFINED)
	// 				? ~0U
	// 				: m_prebuilt->trx->error_key_num);
	// 	}
	// }

	// if ((flag & HA_STATUS_AUTO) && table->found_next_number_field) {
	// 	stats.auto_increment_value = innobase_peek_autoinc();
	// }

// func_exit:
// 	m_prebuilt->trx->op_info = (char*)"";

	DBUG_RETURN(0);
}

struct st_mysql_storage_engine videx_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

static ulong srv_enum_var= 0;
static ulong srv_ulong_var= 0;
static double srv_double_var= 0;

const char *enum_var_names[]=
{
	"e1", "e2", NullS
};

TYPELIB enum_var_typelib= CREATE_TYPELIB_FOR(enum_var_names);

static MYSQL_SYSVAR_ENUM(
	enum_var,                       // name
	srv_enum_var,                   // varname
	PLUGIN_VAR_RQCMDARG,            // opt
	"Sample ENUM system variable",  // comment
	NULL,                           // check
	NULL,                           // update
	0,                              // def
	&enum_var_typelib);             // typelib

static MYSQL_THDVAR_INT(int_var, PLUGIN_VAR_RQCMDARG, "-1..1",
	NULL, NULL, 0, -1, 1, 0);

static MYSQL_SYSVAR_ULONG(
	ulong_var,
	srv_ulong_var,
	PLUGIN_VAR_RQCMDARG,
	"0..1000",
	NULL,
	NULL,
	8,
	0,
	1000,
	0);

static MYSQL_SYSVAR_DOUBLE(
	double_var,
	srv_double_var,
	PLUGIN_VAR_RQCMDARG,
	"0.500000..1000.500000",
	NULL,
	NULL,
	8.5,
	0.5,
	1000.5,
	0);                             // reserved always 0

static MYSQL_THDVAR_DOUBLE(
	double_thdvar,
	PLUGIN_VAR_RQCMDARG,
	"0.500000..1000.500000",
	NULL,
	NULL,
	8.5,
	0.5,
	1000.5,
	0);

static MYSQL_THDVAR_INT(
	deprecated_var, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_DEPRECATED, "-1..1",
	NULL, NULL, 0, -1, 1, 0);

static struct st_mysql_sys_var* videx_system_variables[]= {
	MYSQL_SYSVAR(enum_var), // enum variable 
	MYSQL_SYSVAR(ulong_var), // ulong variable
	MYSQL_SYSVAR(int_var), // int variable
	MYSQL_SYSVAR(double_var), // double variable
	MYSQL_SYSVAR(double_thdvar), // double variable for thread
	MYSQL_SYSVAR(deprecated_var), // deprecated variable
	NULL
};

// this is an example of SHOW_SIMPLE_FUNC and of my_snprintf() service
// If this function would return an array, one should use SHOW_FUNC
static int show_func_videx(MYSQL_THD thd, struct st_mysql_show_var *var,
							 char *buf)
{
	var->type= SHOW_CHAR;
	var->value= buf; // it's of SHOW_VAR_FUNC_BUFF_SIZE bytes
	my_snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE,
				"enum_var is %lu, ulong_var is %lu, int_var is %d, "
				"double_var is %f, %.6sB", // %sB is a MariaDB extension
				srv_enum_var, srv_ulong_var, THDVAR(thd, int_var),
				srv_double_var, "really");
	return 0;
}

static struct st_mysql_show_var func_status[]=
{
	{"func_videx",  (char *)show_func_videx, SHOW_SIMPLE_FUNC},
	{0,0,SHOW_UNDEF}
};

struct st_mysql_daemon unusable_videx=
{ MYSQL_DAEMON_INTERFACE_VERSION };

#ifdef STATIC_VIDEX
maria_declare_plugin(videx_static)
#else
maria_declare_plugin(videx)
#endif
{
	MYSQL_STORAGE_ENGINE_PLUGIN,
	&videx_storage_engine,
	"VIDEX",
	"Haibo Yang",
	"Disaggregated, Extensible Virtual Index Engine for What-If Analysis",
	PLUGIN_LICENSE_GPL,
	videx_init,                            /* Plugin Init */
	NULL,                                         /* Plugin Deinit */
	0x0001,                                       /* version number (0.1) */
	func_status,                                  /* status variables */
	videx_system_variables,                     /* system variables */
	"0.1",                                        /* string version */
	MariaDB_PLUGIN_MATURITY_EXPERIMENTAL          /* maturity */
}
maria_declare_plugin_end;