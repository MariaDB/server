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

static handler *videx_create_handler(handlerton *hton,
									 TABLE_SHARE *table, 
									 MEM_ROOT *mem_root);

handlerton *videx_hton;

/**
	@brief
	Function we use in the creation of our hash to get key.
*/

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key ex_key_mutex_videx_share_mutex;

static PSI_mutex_info all_videx_mutexes[]=
{
	{ &ex_key_mutex_videx_share_mutex, "videx_share::mutex", 0}
};

static void init_videx_psi_keys()
{
	const char* category= "videx";
	int count;

	count= array_elements(all_videx_mutexes);
	mysql_mutex_register(category, all_videx_mutexes, count);
}
#else
static void init_videx_psi_keys() { }
#endif

/**
	@brief
	If frm_error() is called then we will use this to determine
	the file extensions that exist for the storage engine. This is also
	used by the default rename_table and delete_table method in
	handler.cc and by the default discover_many method.

	For engines that have two file name extensions (separate meta/index file
	and data file), the order of elements is relevant. First element of engine
	file name extensions array should be meta/index file extention. Second
	element - data file extention. This order is assumed by
	prepare_for_repair() when REPAIR TABLE ... USE_FRM is issued.

	@see
	rename_table method in handler.cc and
	delete_table method in handler.cc
*/

static const char *ha_videx_exts[] = {
	NullS
};

videx_share::videx_share()
{
	thr_lock_init(&lock);
	mysql_mutex_init(ex_key_mutex_videx_share_mutex,
					 &mutex, MY_MUTEX_INIT_FAST);
}

static int videx_init_func(void *p)
{
	DBUG_ENTER("videx_init_func");

	init_videx_psi_keys();

  videx_hton= (handlerton *)p;
  videx_hton->create=  videx_create_handler;
  videx_hton->flags= 
    HTON_SUPPORTS_EXTENDED_KEYS | HTON_SUPPORTS_FOREIGN_KEYS |
    HTON_NATIVE_SYS_VERSIONING |
    HTON_WSREP_REPLICATION |
    HTON_REQUIRES_CLOSE_AFTER_TRUNCATE |
    HTON_TRUNCATE_REQUIRES_EXCLUSIVE_USE |
    HTON_REQUIRES_NOTIFY_TABLEDEF_CHANGED_AFTER_COMMIT;
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

	/* enforce primary key when a table is created, but not when
	an existing (hlindex?) table is auto-discovered */
	if (srv_force_primary_key && // TODO:
	    thd_sql_command(thd) == SQLCOM_CREATE_TABLE) {
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
	return(REC_VERSION_56_MAX_INDEX_COL_LEN);
}

const key_map* ha_videx::keys_to_use_for_scanning() // ✅
{
	return(&key_map_full);
}

void ha_videx::column_bitmaps_signal()
{
  DBUG_ENTER("ha_videx::column_bitmaps_signal");
  // TODO:
  DBUG_VOID_RETURN;
}

static dict_table_t* open_dict_table(
		const char*		table_name,
		const char*		norm_name,
		bool			is_partition,
		dict_err_ignore_t	ignore_err)
{
  // TODO:
  return(NULL);
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

	ref_length = table->key_info[table_share->primary_key].key_length;

	stats.blocks_size = 16384;

	info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLES | HA_STATUS_CONST);

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
	// TODO:
	DBUG_RETURN(IO_AND_CPU_COST(0, 0));
}

double ha_videx::rnd_pos_time(ha_rows rows)
{
	DBUG_ENTER("ha_videx::rnd_pos_time");
	// TODO:
	DBUG_RETURN(0);
}

double ha_videx::read_time(uint index, uint ranges, ha_rows rows)
{
	DBUG_ENTER("ha_videx::read_time");
	// TODO:
	DBUG_RETURN(0);
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
	DBUG_ENTER("ha_videx::info");
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
	// TODO:
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

	/* We can only evaluate the condition if all columns are stored.*/
	dict_index_t* idx  = innobase_get_index(keyno);
	if (idx && dict_index_has_virtual(idx)) {
		DBUG_RETURN(idx_cond);
	}

	pushed_idx_cond = idx_cond;
	pushed_idx_cond_keyno = keyno;
	in_range_check_pushed_down = TRUE;
	/* We will evaluate the condition entirely */
	DBUG_RETURN(NULL);
}

int ha_videx::info_low(uint flag, bool is_analyze)
{
	DBUG_ENTER("ha_videx::info_low");
	// TODO:
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
	MYSQL_SYSVAR(enum_var),
	MYSQL_SYSVAR(ulong_var),
	MYSQL_SYSVAR(int_var),
	MYSQL_SYSVAR(double_var),
	MYSQL_SYSVAR(double_thdvar),
	MYSQL_SYSVAR(deprecated_var),
	MYSQL_SYSVAR(varopt_default),
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

maria_declare_plugin(videx)
{
	MYSQL_STORAGE_ENGINE_PLUGIN,
	&videx_storage_engine,
	"VIDEX",
	"Haibo Yang",
	"Disaggregated, Extensible Virtual Index Engine for What-If Analysis",
	PLUGIN_LICENSE_GPL,
	videx_init_func,                            /* Plugin Init */
	NULL,                                         /* Plugin Deinit */
	0x0001,                                       /* version number (0.1) */
	func_status,                                  /* status variables */
	videx_system_variables,                     /* system variables */
	"0.1",                                        /* string version */
	MariaDB_PLUGIN_MATURITY_EXPERIMENTAL          /* maturity */
}
maria_declare_plugin_end;