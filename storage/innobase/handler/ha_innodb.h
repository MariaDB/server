/*****************************************************************************

Copyright (c) 2000, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2021, MariaDB Corporation.

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

#ifdef WITH_WSREP
# include <mysql/service_wsrep.h>
# include "../../../wsrep/wsrep_api.h"
#endif /* WITH_WSREP */

#include "table.h"

/* The InnoDB handler: the interface between MySQL and InnoDB. */

/** "GEN_CLUST_INDEX" is the name reserved for InnoDB default
system clustered index when there is no primary key. */
extern const char innobase_index_reserve_name[];

/** Prebuilt structures in an InnoDB table handle used within MySQL */
struct row_prebuilt_t;

/** InnoDB transaction */
struct trx_t;

/** Engine specific table options are defined using this struct */
struct ha_table_option_struct
{
	bool		page_compressed;	/*!< Table is using page compression
						if this option is true. */
	ulonglong	page_compression_level;	/*!< Table page compression level
						0-9. */
	uint		atomic_writes;		/*!< Use atomic writes for this
						table if this options is ON or
						in DEFAULT if
						innodb_use_atomic_writes.
						Atomic writes are not used if
						value OFF.*/
	uint		encryption;		/*!<  DEFAULT, ON, OFF */
	ulonglong	encryption_key_id;	/*!< encryption key id  */
};
/* JAN: TODO: MySQL 5.7 handler.h */
struct st_handler_tablename
{
  const char *db;
  const char *tablename;
};
/** The class defining a handle to an Innodb table */
class ha_innobase: public handler
{
public:
	ha_innobase(handlerton* hton, TABLE_SHARE* table_arg);
	~ha_innobase();

	/** Get the row type from the storage engine.  If this method returns
	ROW_TYPE_NOT_USED, the information in HA_CREATE_INFO should be used. */
	enum row_type get_row_type() const;

	const char* table_type() const;

	const char* index_type(uint key_number);

	const char** bas_ext() const;

	Table_flags table_flags() const;

	ulong index_flags(uint idx, uint part, bool all_parts) const;

	uint max_supported_keys() const;

	uint max_supported_key_length() const;

	uint max_supported_key_part_length() const;

	const key_map* keys_to_use_for_scanning();

	void column_bitmaps_signal();

	/** Opens dictionary table object using table name. For partition, we need to
	try alternative lower/upper case names to support moving data files across
	platforms.
	@param[in]	table_name	name of the table/partition
	@param[in]	norm_name	normalized name of the table/partition
	@param[in]	is_partition	if this is a partition of a table
	@param[in]	ignore_err	error to ignore for loading dictionary object
	@return dictionary table object or NULL if not found */
        static dict_table_t* open_dict_table(
		const char*		table_name,
		const char*		norm_name,
		bool			is_partition,
		dict_err_ignore_t	ignore_err);

	int open(const char *name, int mode, uint test_if_locked);

	handler* clone(const char *name, MEM_ROOT *mem_root);

	int close(void);

	double scan_time();

	double read_time(uint index, uint ranges, ha_rows rows);

	longlong get_memory_buffer_size() const;

	int delete_all_rows();

	int write_row(uchar * buf);

	int update_row(const uchar * old_data, uchar * new_data);

	int delete_row(const uchar * buf);

	bool was_semi_consistent_read();

	void try_semi_consistent_read(bool yes);

	void unlock_row();

	int index_init(uint index, bool sorted);

	int index_end();

	int index_read(
		uchar*			buf,
		const uchar*		key,
		uint			key_len,
		ha_rkey_function	find_flag);

	int index_read_last(uchar * buf, const uchar * key, uint key_len);

	int index_next(uchar * buf);

	int index_next_same(uchar * buf, const uchar *key, uint keylen);

	int index_prev(uchar * buf);

	int index_first(uchar * buf);

	int index_last(uchar * buf);

	/* Copy a cached MySQL row. If requested, also avoids
	overwriting non-read columns. */
	void copy_cached_row(uchar *to_rec, const uchar *from_rec,
				uint rec_length);
	int rnd_init(bool scan);

	int rnd_end();

	int rnd_next(uchar *buf);

	int rnd_pos(uchar * buf, uchar *pos);

	int ft_init();
	void ft_end() { rnd_end(); }
	FT_INFO *ft_init_ext(uint flags, uint inx, String* key);
	int ft_read(uchar* buf);

	void position(const uchar *record);

	int info(uint);

	int analyze(THD* thd,HA_CHECK_OPT* check_opt);

	int optimize(THD* thd,HA_CHECK_OPT* check_opt);

	int discard_or_import_tablespace(my_bool discard);

	int extra(ha_extra_function operation);

	int reset();

	int external_lock(THD *thd, int lock_type);

	int start_stmt(THD *thd, thr_lock_type lock_type);

	void position(uchar *record);

	ha_rows records_in_range(
		uint			inx,
		key_range*		min_key,
		key_range*		max_key);

	ha_rows estimate_rows_upper_bound();

	void update_create_info(HA_CREATE_INFO* create_info);

	inline int create(
		const char*		name,
		TABLE*			form,
		HA_CREATE_INFO*		create_info,
		bool			file_per_table,
		trx_t*			trx = NULL);

	int create(
		const char*		name,
		TABLE*			form,
		HA_CREATE_INFO*		create_info);

	const char* check_table_options(THD *thd, TABLE* table,
		HA_CREATE_INFO*	create_info, const bool use_tablespace, const ulint file_format);

	inline int delete_table(const char* name, enum_sql_command sqlcom);

	int truncate();

	int delete_table(const char *name);

	int rename_table(const char* from, const char* to);
	inline int defragment_table(const char* name);
	int check(THD* thd, HA_CHECK_OPT* check_opt);

	char* get_foreign_key_create_info();

	int get_foreign_key_list(THD *thd, List<FOREIGN_KEY_INFO> *f_key_list);

	int get_parent_foreign_key_list(
		THD*			thd,
		List<FOREIGN_KEY_INFO>*	f_key_list);
	int get_cascade_foreign_key_table_list(
		THD*				thd,
		List<st_handler_tablename>*	fk_table_list);


	bool can_switch_engines();

	uint referenced_by_foreign_key();

	void free_foreign_key_create_info(char* str);

	uint lock_count(void) const;

	THR_LOCK_DATA** store_lock(
		THD*			thd,
		THR_LOCK_DATA**		to,
		thr_lock_type		lock_type);

	void init_table_handle_for_HANDLER();

	virtual void get_auto_increment(
		ulonglong		offset,
		ulonglong		increment,
		ulonglong		nb_desired_values,
		ulonglong*		first_value,
		ulonglong*		nb_reserved_values);
	int reset_auto_increment(ulonglong value);

	virtual bool get_error_message(int error, String *buf);

	virtual bool get_foreign_dup_key(char*, uint, char*, uint);

	uint8 table_cache_type();

	/**
	Ask handler about permission to cache table during query registration
	*/
	my_bool register_query_cache_table(
		THD*			thd,
		char*			table_key,
		uint			key_length,
		qc_engine_callback*	call_back,
		ulonglong*		engine_data);

	bool primary_key_is_clustered();

	int cmp_ref(const uchar* ref1, const uchar* ref2);

	/** On-line ALTER TABLE interface @see handler0alter.cc @{ */

	/** Check if InnoDB supports a particular alter table in-place
	@param altered_table TABLE object for new version of table.
	@param ha_alter_info Structure describing changes to be done
	by ALTER TABLE and holding data used during in-place alter.

	@retval HA_ALTER_INPLACE_NOT_SUPPORTED Not supported
	@retval HA_ALTER_INPLACE_NO_LOCK Supported
	@retval HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE
		Supported, but requires lock during main phase and
		exclusive lock during prepare phase.
	@retval HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE
		Supported, prepare phase requires exclusive lock.  */
	enum_alter_inplace_result check_if_supported_inplace_alter(
		TABLE*			altered_table,
		Alter_inplace_info*	ha_alter_info);

	/** Allows InnoDB to update internal structures with concurrent
	writes blocked (provided that check_if_supported_inplace_alter()
	did not return HA_ALTER_INPLACE_NO_LOCK).
	This will be invoked before inplace_alter_table().

	@param altered_table TABLE object for new version of table.
	@param ha_alter_info Structure describing changes to be done
	by ALTER TABLE and holding data used during in-place alter.

	@retval true Failure
	@retval false Success
	*/
	bool prepare_inplace_alter_table(
		TABLE*			altered_table,
		Alter_inplace_info*	ha_alter_info);

	/** Alter the table structure in-place with operations
	specified using HA_ALTER_FLAGS and Alter_inplace_information.
	The level of concurrency allowed during this operation depends
	on the return value from check_if_supported_inplace_alter().

	@param altered_table TABLE object for new version of table.
	@param ha_alter_info Structure describing changes to be done
	by ALTER TABLE and holding data used during in-place alter.

	@retval true Failure
	@retval false Success
	*/
	bool inplace_alter_table(
		TABLE*			altered_table,
		Alter_inplace_info*	ha_alter_info);

	/** Commit or rollback the changes made during
	prepare_inplace_alter_table() and inplace_alter_table() inside
	the storage engine. Note that the allowed level of concurrency
	during this operation will be the same as for
	inplace_alter_table() and thus might be higher than during
	prepare_inplace_alter_table(). (E.g concurrent writes were
	blocked during prepare, but might not be during commit).
	@param altered_table TABLE object for new version of table.
	@param ha_alter_info Structure describing changes to be done
	by ALTER TABLE and holding data used during in-place alter.
	@param commit true => Commit, false => Rollback.
	@retval true Failure
	@retval false Success
	*/
	bool commit_inplace_alter_table(
		TABLE*			altered_table,
		Alter_inplace_info*	ha_alter_info,
		bool			commit);
	/** @} */

	bool check_if_incompatible_data(
		HA_CREATE_INFO*		info,
		uint			table_changes);

	/** @name Multi Range Read interface @{ */

	/** Initialize multi range read @see DsMrr_impl::dsmrr_init
	@param seq
	@param seq_init_param
	@param n_ranges
	@param mode
	@param buf */
	int multi_range_read_init(
		RANGE_SEQ_IF*		seq,
		void*			seq_init_param,
		uint			n_ranges,
		uint			mode,
		HANDLER_BUFFER*		buf);

	/** Process next multi range read @see DsMrr_impl::dsmrr_next
	@param range_info */
	int multi_range_read_next(range_id_t *range_info);

	/** Initialize multi range read and get information.
	@see ha_myisam::multi_range_read_info_const
	@see DsMrr_impl::dsmrr_info_const
	@param keyno
	@param seq
	@param seq_init_param
	@param n_ranges
	@param bufsz
	@param flags
	@param cost */
	ha_rows multi_range_read_info_const(
		uint			keyno,
		RANGE_SEQ_IF*		seq,
		void*			seq_init_param,
		uint			n_ranges,
		uint*			bufsz,
		uint*			flags,
		Cost_estimate*		cost);

	/** Initialize multi range read and get information.
	@see DsMrr_impl::dsmrr_info
	@param keyno
	@param seq
	@param seq_init_param
	@param n_ranges
	@param bufsz
	@param flags
	@param cost */
	ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
				      uint key_parts, uint* bufsz, uint* flags,
				      Cost_estimate* cost);

	int multi_range_read_explain_info(uint mrr_mode,
					  char *str, size_t size);

	/** Attempt to push down an index condition.
	@param[in] keyno MySQL key number
	@param[in] idx_cond Index condition to be checked
	@return idx_cond if pushed; NULL if not pushed */
	Item* idx_cond_push(uint keyno, Item* idx_cond);
	/* @} */

	/** Check if InnoDB is not storing virtual column metadata for a table.
	@param	s	table definition (based on .frm file)
	@return	whether InnoDB will omit virtual column metadata */
	static bool omits_virtual_cols(const TABLE_SHARE& s)
	{
		return s.frm_version<FRM_VER_EXPRESSSIONS && s.virtual_fields;
	}

protected:
	/**
	MySQL calls this method at the end of each statement. This method
	exists for readability only, called from reset(). The name reset()
	doesn't give any clue that it is called at the end of a statement. */
	int end_stmt();

	dberr_t innobase_get_autoinc(ulonglong* value);
	dberr_t innobase_lock_autoinc();
	ulonglong innobase_peek_autoinc();
	dberr_t innobase_set_max_autoinc(ulonglong auto_inc);
	dberr_t innobase_reset_autoinc(ulonglong auto_inc);

	/** Resets a query execution 'template'.
	@see build_template() */
	void reset_template();

	inline void update_thd(THD* thd);
	void update_thd();

	int general_fetch(uchar* buf, uint direction, uint match_mode);
	int change_active_index(uint keynr);
	dict_index_t* innobase_get_index(uint keynr);

#ifdef WITH_WSREP
	int wsrep_append_keys(THD *thd, wsrep_key_type key_type,
			      const uchar* record0, const uchar* record1);
#endif
	/** Builds a 'template' to the prebuilt struct.

	The template is used in fast retrieval of just those column
	values MySQL needs in its processing.
	@param whole_row true if access is needed to a whole row,
	false if accessing individual fields is enough */
	void build_template(bool whole_row);

	virtual int info_low(uint, bool);

	/** The multi range read session object */
	DsMrr_impl		m_ds_mrr;

	/** Save CPU time with prebuilt/cached data structures */
	row_prebuilt_t*		m_prebuilt;

	/** Thread handle of the user currently using the handler;
	this is set in external_lock function */
	THD*			m_user_thd;

	/** buffer used in updates */
	uchar*			m_upd_buf;

	/** the size of upd_buf in bytes */
	ulint			m_upd_buf_size;

	/** Flags that specificy the handler instance (table) capability. */
	Table_flags		m_int_table_flags;

	/** Index into the server's primkary keye meta-data table->key_info{} */
	uint			m_primary_key;

	/** this is set to 1 when we are starting a table scan but have
	not yet fetched any row, else false */
	bool			m_start_of_scan;

	/*!< match mode of the latest search: ROW_SEL_EXACT,
	ROW_SEL_EXACT_PREFIX, or undefined */
	uint			m_last_match_mode;

        /** If mysql has locked with external_lock() */
        bool                    m_mysql_has_locked;
};


/* Some accessor functions which the InnoDB plugin needs, but which
can not be added to mysql/plugin.h as part of the public interface;
the definitions are bracketed with #ifdef INNODB_COMPATIBILITY_HOOKS */

#ifndef INNODB_COMPATIBILITY_HOOKS
#error InnoDB needs MySQL to be built with #define INNODB_COMPATIBILITY_HOOKS
#endif

LEX_STRING* thd_query_string(MYSQL_THD thd);
size_t thd_query_safe(MYSQL_THD thd, char *buf, size_t buflen);

extern "C" {

/** Check if a user thread is a replication slave thread
@param thd user thread
@retval 0 the user thread is not a replication slave thread
@retval 1 the user thread is a replication slave thread */
int thd_slave_thread(const MYSQL_THD thd);

/** Check if a user thread is running a non-transactional update
@param thd user thread
@retval 0 the user thread is not running a non-transactional update
@retval 1 the user thread is running a non-transactional update */
int thd_non_transactional_update(const MYSQL_THD thd);

/** Get high resolution timestamp for the current query start time.
The timestamp is not anchored to any specific point in time,
but can be used for comparison.
@param thd user thread
@retval timestamp in microseconds precision
*/
unsigned long long thd_start_utime(const MYSQL_THD thd);

/** Get the user thread's binary logging format
@param thd user thread
@return Value to be used as index into the binlog_format_names array */
int thd_binlog_format(const MYSQL_THD thd);

/** Check if binary logging is filtered for thread's current db.
@param thd Thread handle
@retval 1 the query is not filtered, 0 otherwise. */
bool thd_binlog_filter_ok(const MYSQL_THD thd);

/** Check if the query may generate row changes which may end up in the binary.
@param thd Thread handle
@retval 1 the query may generate row changes, 0 otherwise.
*/
bool thd_sqlcom_can_generate_row_events(const MYSQL_THD thd);

/** Is strict sql_mode set.
@param thd Thread object
@return True if sql_mode has strict mode (all or trans), false otherwise. */
bool thd_is_strict_mode(const MYSQL_THD thd);

} /* extern "C" */

/** Get the file name and position of the MySQL binlog corresponding to the
 * current commit.
 */
extern void mysql_bin_log_commit_pos(THD *thd, ulonglong *out_pos, const char **out_file);

struct trx_t;
#ifdef WITH_WSREP
//extern "C" int wsrep_trx_order_before(void *thd1, void *thd2);

extern "C" bool wsrep_thd_is_wsrep_on(THD *thd);


extern "C" void wsrep_thd_set_exec_mode(THD *thd, enum wsrep_exec_mode mode);
extern "C" void wsrep_thd_set_query_state(
	THD *thd, enum wsrep_query_state state);

extern "C" void wsrep_thd_set_trx_to_replay(THD *thd, uint64 trx_id);

extern "C" uint32 wsrep_thd_wsrep_rand(THD *thd);
extern "C" time_t wsrep_thd_query_start(THD *thd);
extern "C" query_id_t wsrep_thd_query_id(THD *thd);
extern "C" query_id_t wsrep_thd_wsrep_last_query_id(THD *thd);
extern "C" void wsrep_thd_set_wsrep_last_query_id(THD *thd, query_id_t id);
#endif

extern const struct _ft_vft ft_vft_result;

/** Structure Returned by ha_innobase::ft_init_ext() */
typedef struct new_ft_info
{
	struct _ft_vft		*please;
	struct _ft_vft_ext	*could_you;
	row_prebuilt_t*		ft_prebuilt;
	fts_result_t*		ft_result;
} NEW_FT_INFO;

/**
Allocates an InnoDB transaction for a MySQL handler object.
@return InnoDB transaction handle */
trx_t*
innobase_trx_allocate(
	MYSQL_THD	thd);	/*!< in: user thread handle */

/*********************************************************************//**
This function checks each index name for a table against reserved
system default primary index name 'GEN_CLUST_INDEX'. If a name
matches, this function pushes an warning message to the client,
and returns true.
@return true if the index name matches the reserved name */
bool
innobase_index_name_is_reserved(
	THD*		thd,		/*!< in/out: MySQL connection */
	const KEY*	key_info,	/*!< in: Indexes to be created */
	ulint		num_of_keys)	/*!< in: Number of indexes to
					be created. */
	MY_ATTRIBUTE((nonnull(1), warn_unused_result));

/** Parse hint for table and its indexes, and update the information
in dictionary.
@param[in]	thd		Connection thread
@param[in,out]	table		Target table
@param[in]	table_share	Table definition */
void
innobase_parse_hint_from_comment(
	THD*			thd,
	dict_table_t*		table,
	const TABLE_SHARE*	table_share);

/** Class for handling create table information. */
class create_table_info_t
{
public:
	/** Constructor.
	Used in two ways:
	- all but file_per_table is used, when creating the table.
	- all but name/path is used, when validating options and using flags. */
	create_table_info_t(
		THD*		thd,
		const TABLE*	form,
		HA_CREATE_INFO*	create_info,
		char*		table_name,
		char*		remote_path,
		bool		file_per_table,
		trx_t*		trx = NULL);

	/** Initialize the object. */
	int initialize();

	/** Set m_tablespace_type. */
	void set_tablespace_type(bool table_being_altered_is_file_per_table);

	/** Create the internal innodb table.
	@param create_fk	whether to add FOREIGN KEY constraints */
	int create_table(bool create_fk = true);

	/** Update the internal data dictionary. */
	int create_table_update_dict();

	/** Validates the create options. Checks that the options
	KEY_BLOCK_SIZE, ROW_FORMAT, DATA DIRECTORY, TEMPORARY & TABLESPACE
	are compatible with each other and other settings.
	These CREATE OPTIONS are not validated here unless innodb_strict_mode
	is on. With strict mode, this function will report each problem it
	finds using a custom message with error code
	ER_ILLEGAL_HA_CREATE_OPTION, not its built-in message.
	@return NULL if valid, string name of bad option if not. */
	const char* create_options_are_invalid();

	bool gcols_in_fulltext_or_spatial();

	/** Validates engine specific table options not handled by
	SQL-parser.
	@return NULL if valid, string name of bad option if not. */
	const char* check_table_options();

	/** Validate DATA DIRECTORY option. */
	bool create_option_data_directory_is_valid();

	/** Validate TABLESPACE option. */
	bool create_option_tablespace_is_valid();

	/** Prepare to create a table. */
	int prepare_create_table(const char* name, bool strict = true);

	void allocate_trx();

	/** Checks that every index have sane size. Depends on strict mode */
	bool row_size_is_acceptable(const dict_table_t& table,
				    bool strict) const;
	/** Checks that given index have sane size. Depends on strict mode */
	bool row_size_is_acceptable(const dict_index_t& index,
				    bool strict) const;

	/** Determines InnoDB table flags.
	If strict_mode=OFF, this will adjust the flags to what should be assumed.
	@retval true if successful, false if error */
	bool innobase_table_flags();

	/** Set flags and append '/' to remote path if necessary. */
	void set_remote_path_flags();

	/** Get table flags. */
	ulint flags() const
	{ return(m_flags); }

	/** Update table flags. */
	void flags_set(ulint flags) { m_flags |= flags; }

	/** Get table flags2. */
	ulint flags2() const
	{ return(m_flags2); }

	/** Get trx. */
	trx_t* trx() const
	{ return(m_trx); }

	/** Return table name. */
	const char* table_name() const
	{ return(m_table_name); }

	/** @return whether the table needs to be dropped on rollback */
	bool drop_before_rollback() const { return m_drop_before_rollback; }

	THD* thd() const
	{ return(m_thd); }

	/** Normalizes a table name string.
	A normalized name consists of the database name catenated to '/' and
	table name. An example: test/mytable. On Windows normalization puts
	both the database name and the table name always to lower case if
	"set_lower_case" is set to true.
	@param[in,out]	norm_name	Buffer to return the normalized name in.
	@param[in]	name		Table name string.
	@param[in]	set_lower_case	True if we want to set name to lower
					case. */
	static void normalize_table_name_low(
		char*           norm_name,
		const char*     name,
		ibool           set_lower_case);

private:
	/** Parses the table name into normal name and either temp path or
	remote path if needed.*/
	int
	parse_table_name(
		const char*	name);

	/** Create the internal innodb table definition. */
	int create_table_def();

	/** Connection thread handle. */
	THD*		m_thd;

	/** InnoDB transaction handle. */
	trx_t*		m_trx;

	/** Information on table columns and indexes. */
	const TABLE*	m_form;

	/** Value of innodb_default_row_format */
	const ulong	m_default_row_format;

	/** Create options. */
	HA_CREATE_INFO*	m_create_info;

	/** Table name */
	char*		m_table_name;
	/** Whether the table needs to be dropped before rollback */
	bool		m_drop_before_rollback;

	/** Remote path (DATA DIRECTORY) or zero length-string */
	char*		m_remote_path;

	/** Local copy of srv_file_per_table. */
	bool		m_innodb_file_per_table;

	/** Allow file_per_table for this table either because:
	1) the setting innodb_file_per_table=on,
	2) it was explicitly requested by tablespace=innodb_file_per_table.
	3) the table being altered is currently file_per_table */
	bool		m_allow_file_per_table;

	/** After all considerations, this shows whether we will actually
	create a table and tablespace using file-per-table. */
	bool		m_use_file_per_table;

	/** Using DATA DIRECTORY */
	bool		m_use_data_dir;

	/** Table flags */
	ulint		m_flags;

	/** Table flags2 */
	ulint		m_flags2;
};

/**
Initialize the table FTS stopword list
@return TRUE if success */
ibool
innobase_fts_load_stopword(
/*=======================*/
	dict_table_t*	table,		/*!< in: Table has the FTS */
	trx_t*		trx,		/*!< in: transaction */
	THD*		thd)		/*!< in: current thread */
	MY_ATTRIBUTE((warn_unused_result));

/** Some defines for innobase_fts_check_doc_id_index() return value */
enum fts_doc_id_index_enum {
	FTS_INCORRECT_DOC_ID_INDEX,
	FTS_EXIST_DOC_ID_INDEX,
	FTS_NOT_EXIST_DOC_ID_INDEX
};

/**
Check whether the table has a unique index with FTS_DOC_ID_INDEX_NAME
on the Doc ID column.
@return the status of the FTS_DOC_ID index */
fts_doc_id_index_enum
innobase_fts_check_doc_id_index(
	const dict_table_t*	table,		/*!< in: table definition */
	const TABLE*		altered_table,	/*!< in: MySQL table
						that is being altered */
	ulint*			fts_doc_col_no)	/*!< out: The column number for
						Doc ID */
	MY_ATTRIBUTE((warn_unused_result));

/**
Check whether the table has a unique index with FTS_DOC_ID_INDEX_NAME
on the Doc ID column in MySQL create index definition.
@return FTS_EXIST_DOC_ID_INDEX if there exists the FTS_DOC_ID index,
FTS_INCORRECT_DOC_ID_INDEX if the FTS_DOC_ID index is of wrong format */
fts_doc_id_index_enum
innobase_fts_check_doc_id_index_in_def(
	ulint		n_key,		/*!< in: Number of keys */
	const KEY*	key_info)	/*!< in: Key definitions */
	MY_ATTRIBUTE((warn_unused_result));

/**
Copy table flags from MySQL's TABLE_SHARE into an InnoDB table object.
Those flags are stored in .frm file and end up in the MySQL table object,
but are frequently used inside InnoDB so we keep their copies into the
InnoDB table object. */
void
innobase_copy_frm_flags_from_table_share(
	dict_table_t*		innodb_table,	/*!< in/out: InnoDB table */
	const TABLE_SHARE*	table_share);	/*!< in: table share */

/** Set up base columns for virtual column
@param[in]	table	the InnoDB table
@param[in]	field	MySQL field
@param[in,out]	v_col	virtual column to be set up */
void
innodb_base_col_setup(
	dict_table_t*	table,
	const Field*	field,
	dict_v_col_t*	v_col);

/** Set up base columns for stored column
@param[in]	table	InnoDB table
@param[in]	field	MySQL field
@param[in,out]	s_col	stored column */
void
innodb_base_col_setup_for_stored(
	const dict_table_t*	table,
	const Field*		field,
	dict_s_col_t*		s_col);

/** whether this is a stored generated column */
#define innobase_is_s_fld(field) ((field)->vcol_info && (field)->stored_in_db())

/** Always normalize table name to lower case on Windows */
#ifdef _WIN32
#define normalize_table_name(norm_name, name)           \
	create_table_info_t::normalize_table_name_low(norm_name, name, TRUE)
#else
#define normalize_table_name(norm_name, name)           \
	create_table_info_t::normalize_table_name_low(norm_name, name, FALSE)
#endif /* _WIN32 */

/** Converts an InnoDB error code to a MySQL error code.
Also tells to MySQL about a possible transaction rollback inside InnoDB caused
by a lock wait timeout or a deadlock.
@param[in]	error	InnoDB error code.
@param[in]	flags	InnoDB table flags or 0.
@param[in]	thd	MySQL thread or NULL.
@return MySQL error code */
int
convert_error_code_to_mysql(
	dberr_t	error,
	ulint	flags,
	THD*	thd);

/** Converts a search mode flag understood by MySQL to a flag understood
by InnoDB.
@param[in]	find_flag	MySQL search mode flag.
@return	InnoDB search mode flag. */
page_cur_mode_t
convert_search_mode_to_innobase(
	enum ha_rkey_function	find_flag);

/** Commits a transaction in an InnoDB database.
@param[in]	trx	Transaction handle. */
void
innobase_commit_low(
	trx_t*	trx);

extern my_bool	innobase_stats_on_metadata;

/** Calculate Record Per Key value.
Need to exclude the NULL value if innodb_stats_method is set to "nulls_ignored"
@param[in]	index	InnoDB index.
@param[in]	i	The column we are calculating rec per key.
@param[in]	records	Estimated total records.
@return estimated record per key value */
/* JAN: TODO: MySQL 5.7  */
typedef float rec_per_key_t;
rec_per_key_t
innodb_rec_per_key(
	dict_index_t*	index,
	ulint		i,
	ha_rows		records);

/** Build template for the virtual columns and their base columns
@param[in]	table		MySQL TABLE
@param[in]	ib_table	InnoDB dict_table_t
@param[in,out]	s_templ		InnoDB template structure
@param[in]	add_v		new virtual columns added along with
				add index call
@param[in]	locked		true if innobase_share_mutex is held */
void
innobase_build_v_templ(
	const TABLE*		table,
	const dict_table_t*	ib_table,
	dict_vcol_templ_t*	s_templ,
	const dict_add_v_col_t*	add_v,
	bool			locked);

/** callback used by MySQL server layer to initialized
the table virtual columns' template
@param[in]	table		MySQL TABLE
@param[in,out]	ib_table	InnoDB dict_table_t */
void
innobase_build_v_templ_callback(
        const TABLE*	table,
        void*		ib_table);

/** Callback function definition, used by MySQL server layer to initialized
the table virtual columns' template */
typedef void (*my_gcolumn_templatecallback_t)(const TABLE*, void*);

/** Convert MySQL column number to dict_table_t::cols[] offset.
@param[in]	field	non-virtual column
@return	column number relative to dict_table_t::cols[] */
unsigned
innodb_col_no(const Field* field)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/********************************************************************//**
Helper function to push frm mismatch error to error log and
if needed to sql-layer. */
UNIV_INTERN
void
ib_push_frm_error(
/*==============*/
	THD*		thd,		/*!< in: MySQL thd */
	dict_table_t*	ib_table,	/*!< in: InnoDB table */
	TABLE*		table,		/*!< in: MySQL table */
	ulint		n_keys,		/*!< in: InnoDB #keys */
	bool		push_warning);	/*!< in: print warning ? */

/** Check each index part length whether they not exceed the max limit
@param[in]	max_field_len	maximum allowed key part length
@param[in]	key		MariaDB key definition
@return true if index column length exceeds limit */
MY_ATTRIBUTE((warn_unused_result))
bool too_big_key_part_length(size_t max_field_len, const KEY& key);

/** This function is used to rollback one X/Open XA distributed transaction
which is in the prepared state

@param[in] hton InnoDB handlerton
@param[in] xid X/Open XA transaction identification

@return 0 or error number */
int innobase_rollback_by_xid(handlerton* hton, XID* xid);
