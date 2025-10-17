/*****************************************************************************

Copyright (c) 2011, 2018, Oracle and/or its affiliates. All Rights Reserved.
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

/******************************************************************//**
@file include/fts0priv.h
Full text search internal header file

Created 2011/09/02 Sunny Bains
***********************************************************************/

#ifndef INNOBASE_FTS0PRIV_H
#define INNOBASE_FTS0PRIV_H

#include "dict0dict.h"
#include "pars0pars.h"
#include "que0que.h"
#include "que0types.h"
#include "fts0types.h"

/* The various states of the FTS sub system pertaining to a table with
FTS indexes defined on it. */
enum fts_table_state_enum {
					/* !<This must be 0 since we insert
					a hard coded '0' at create time
					to the config table */

	FTS_TABLE_STATE_RUNNING = 0,	/*!< Auxiliary tables created OK */

	FTS_TABLE_STATE_OPTIMIZING,	/*!< This is a substate of RUNNING */

	FTS_TABLE_STATE_DELETED		/*!< All aux tables to be dropped when
					it's safe to do so */
};

typedef enum fts_table_state_enum fts_table_state_t;

/** The default time to wait for the background thread (in microsecnds). */
#define FTS_MAX_BACKGROUND_THREAD_WAIT		10000

/** Maximum number of iterations to wait before we complain */
#define FTS_BACKGROUND_THREAD_WAIT_COUNT	1000

/** The maximum length of the config table's value column in bytes */
#define FTS_MAX_CONFIG_NAME_LEN			64

/** The maximum length of the config table's value column in bytes */
#define FTS_MAX_CONFIG_VALUE_LEN		1024

/** Approx. upper limit of ilist length in bytes. */
#define FTS_ILIST_MAX_SIZE			(64 * 1024)

/** FTS config table name parameters */

/** The number of seconds after which an OPTIMIZE run will stop */
#define FTS_OPTIMIZE_LIMIT_IN_SECS	"optimize_checkpoint_limit"

/** The next doc id */
#define FTS_SYNCED_DOC_ID		"synced_doc_id"

/** The last word that was OPTIMIZED */
#define FTS_LAST_OPTIMIZED_WORD		"last_optimized_word"

/** Total number of documents that have been deleted. The next_doc_id
minus this count gives us the total number of documents. */
#define FTS_TOTAL_DELETED_COUNT		"deleted_doc_count"

/** Total number of words parsed from all documents */
#define FTS_TOTAL_WORD_COUNT		"total_word_count"

/** Start of optimize of an FTS index */
#define FTS_OPTIMIZE_START_TIME		"optimize_start_time"

/** End of optimize for an FTS index */
#define FTS_OPTIMIZE_END_TIME		"optimize_end_time"

/** User specified stopword table name */
#define	FTS_STOPWORD_TABLE_NAME		"stopword_table_name"

/** Whether to use (turn on/off) stopword */
#define	FTS_USE_STOPWORD		"use_stopword"

/** State of the FTS system for this table. It can be one of
 RUNNING, OPTIMIZING, DELETED. */
#define FTS_TABLE_STATE			"table_state"

/** The minimum length of an FTS auxiliary table names's id component
e.g., For an auxiliary table name

	FTS_<TABLE_ID>_SUFFIX

This constant is for the minimum length required to store the <TABLE_ID>
component.
*/
#define FTS_AUX_MIN_TABLE_ID_LENGTH	48

/** Maximum length of an integer stored in the config table value column. */
#define FTS_MAX_INT_LEN			32

/** Construct the name of an internal FTS table for the given table.
@param[in]	fts_table	metadata on fulltext-indexed table
@param[out]	table_name	a name up to MAX_FULL_NAME_LEN
@param[in]	dict_locked	whether dict_sys.latch is being held */
void fts_get_table_name(const fts_table_t* fts_table, char* table_name,
			bool dict_locked = false)
	MY_ATTRIBUTE((nonnull));

/** define for fts_doc_fetch_by_doc_id() "option" value, defines whether
we want to get Doc whose ID is equal to or greater or smaller than supplied
ID */
#define	FTS_FETCH_DOC_BY_ID_EQUAL	1
#define	FTS_FETCH_DOC_BY_ID_LARGE	2
#define	FTS_FETCH_DOC_BY_ID_SMALL	3

/** Structure to use it in fts_doc_fetch_by_doc_id() funtion.
This structure contains fields to fetch from clustered index record
and points to user_arg */
struct fts_fetch_doc_id
{
  std::vector<uint32_t> field_to_fetch;
  void *user_arg;
};

/** Fetch document with the given document id.
@param get_doc      state
@param doc_id       document id to fetch
@param index_to_use index to be used for fetching fts information
@param option       search option, if it is greater than doc_id or equal
@param callback     callback function after fetching the fulltext indexed
                    column
@param arg          callback argument
@return DB_SUCCESS if OK else error */
dberr_t fts_doc_fetch_by_doc_id(fts_get_doc_t *get_doc, doc_id_t doc_id,
                                dict_index_t *index_to_use, ulint option,
                                fts_sql_callback *callback, void *arg)
                                MY_ATTRIBUTE((nonnull(6)));

/** Callback function for fetch that stores the text of an FTS document,
converting each column to UTF-16.
@param index    clustered index
@param rec      clustered index record
@param offsets  offsets for the record
@param user_arg points to fts_fetch_doc_id since it has field information
                to fetch and user_arg which points to fts_doc_t
@return always true */
bool fts_query_expansion_fetch_doc(dict_index_t *index, const rec_t *rec,
                                   const rec_offs *offsets, void *user_arg);

class FTSQueryRunner;

/** Write out a single word's data as new entry/entries in the INDEX table.
@param fts_table FTS auxiliary table information
@param word word in UTF-8
@param node node columns
@param runner Runs the query in FTS internal SYSTEM TABLES
@return DB_SUCCESS if all OK. */
dberr_t
fts_write_node(dict_table_t *table, fts_string_t *word,
               fts_node_t* node, FTSQueryRunner *sqlRunner)
               MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Check if a fts token is a stopword or less than fts_min_token_size
or greater than fts_max_token_size.
@param[in]	token		token string
@param[in]	stopwords	stopwords rb tree
@param[in]	cs		token charset
@retval true	if it is not stopword and length in range
@retval false	if it is stopword or length not in range */
bool
fts_check_token(
	const fts_string_t*	token,
	const ib_rbt_t*		stopwords,
	const CHARSET_INFO*	cs);

/******************************************************************//**
Initialize a document. */
void
fts_doc_init(
/*=========*/
	fts_doc_t*	doc)		/*!< in: doc to initialize */
	MY_ATTRIBUTE((nonnull));

/******************************************************************//**
Do a binary search for a doc id in the array
@return +ve index if found -ve index where it should be
        inserted if not found */
int
fts_bsearch(
/*========*/
	doc_id_t*	array,		/*!< in: array to sort */
	int		lower,		/*!< in: lower bound of array*/
	int		upper,		/*!< in: upper bound of array*/
	doc_id_t	doc_id)		/*!< in: doc id to lookup */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/******************************************************************//**
Free document. */
void
fts_doc_free(
/*=========*/
	fts_doc_t*	doc)		/*!< in: document */
	MY_ATTRIBUTE((nonnull));
/******************************************************************//**
Free fts_optimizer_word_t instanace.*/
void
fts_word_free(
/*==========*/
	fts_word_t*	word)		/*!< in: instance to free.*/
	MY_ATTRIBUTE((nonnull));

/** Reads the rows from the given aux table.
@param aux_table   auxiliary table
@param word        word to fetch
@param fetch       store the rows value
@param sqlRunner   Query executor
@return DB_SUCCESS or error code */
dberr_t fts_index_fetch_nodes_low(
  dict_table_t *aux_table, const fts_string_t *word, fts_fetch_t *fetch,
  FTSQueryRunner *sqlRunner);

/** Reads the rows from fts auxiliary table
@param trx       transaction
@param fts_table fulltext auxiliary table
@param word      word to fetch
@param fetch     stores the rows value and calculates memory needed
@return error code or DB_SUCCESS */
dberr_t fts_index_fetch_nodes(trx_t *trx, fts_table_t *fts_table,
                              const fts_string_t *word, fts_fetch_t *fetch);

#define fts_sql_commit(trx) trx_commit_for_mysql(trx)
#define fts_sql_rollback(trx) (trx)->rollback()
/******************************************************************//**
Get value from config table. The caller must ensure that enough
space is allocated for value to hold the column contents
@return DB_SUCCESS or error code */
dberr_t
fts_config_get_value(
/*=================*/
	trx_t*		trx,		/* transaction */
	fts_table_t*	fts_table,	/*!< in: the indexed FTS table */
	const char*	name,		/*!< in: get config value for
					this parameter name */
	fts_string_t*	value)		/*!< out: value read from
					config table */
	MY_ATTRIBUTE((nonnull));
/******************************************************************//**
Get value specific to an FTS index from the config table. The caller
must ensure that enough space is allocated for value to hold the
column contents.
@return DB_SUCCESS or error code */
dberr_t
fts_config_get_index_value(
/*=======================*/
	trx_t*		trx,		/*!< transaction */
	dict_index_t*	index,		/*!< in: index */
	const char*	param,		/*!< in: get config value for
					this parameter name */
	fts_string_t*	value)		/*!< out: value read from
					config table */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/******************************************************************//**
Set the value in the config table for name.
@return DB_SUCCESS or error code */
dberr_t
fts_config_set_value(
/*=================*/
	trx_t*		trx,		/*!< transaction */
	fts_table_t*	fts_table,	/*!< in: the indexed FTS table */
	const char*	name,		/*!< in: get config value for
					this parameter name */
	const fts_string_t*
			value)		/*!< in: value to update */
	MY_ATTRIBUTE((nonnull));
/****************************************************************//**
Set an ulint value in the config table.
@return DB_SUCCESS if all OK else error code */
dberr_t
fts_config_set_ulint(
/*=================*/
	trx_t*		trx,		/*!< in: transaction */
	fts_table_t*	fts_table,	/*!< in: the indexed FTS table */
	const char*	name,		/*!< in: param name */
	ulint		int_value)	/*!< in: value */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/******************************************************************//**
Set the value specific to an FTS index in the config table.
@return DB_SUCCESS or error code */
dberr_t
fts_config_set_index_value(
/*=======================*/
	trx_t*		trx,		/*!< transaction */
	dict_index_t*	index,		/*!< in: index */
	const char*	param,		/*!< in: get config value for
					this parameter name */
	fts_string_t*	value)		/*!< out: value read from
					config table */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

#ifdef FTS_OPTIMIZE_DEBUG
/******************************************************************//**
Get an ulint value from the config table.
@return DB_SUCCESS or error code */
dberr_t
fts_config_get_index_ulint(
/*=======================*/
	trx_t*		trx,		/*!< in: transaction */
	dict_index_t*	index,		/*!< in: FTS index */
	const char*	name,		/*!< in: param name */
	ulint*		int_value)	/*!< out: value */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
#endif /* FTS_OPTIMIZE_DEBUG */

/******************************************************************//**
Set an ulint value int the config table.
@return DB_SUCCESS or error code */
dberr_t
fts_config_set_index_ulint(
/*=======================*/
	trx_t*		trx,		/*!< in: transaction */
	dict_index_t*	index,		/*!< in: FTS index */
	const char*	name,		/*!< in: param name */
	ulint		int_value)	/*!< in: value */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/******************************************************************//**
Get an ulint value from the config table.
@return DB_SUCCESS or error code */
dberr_t
fts_config_get_ulint(
/*=================*/
	trx_t*		trx,		/*!< in: transaction */
	fts_table_t*	fts_table,	/*!< in: the indexed FTS table */
	const char*	name,		/*!< in: param name */
	ulint*		int_value)	/*!< out: value */
	MY_ATTRIBUTE((nonnull));
/******************************************************************//**
Search cache for word.
@return the word node vector if found else NULL */
const ib_vector_t*
fts_cache_find_word(
/*================*/
	const fts_index_cache_t*
			index_cache,	/*!< in: cache to search */
	const fts_string_t*
			text)		/*!< in: word to search for */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/******************************************************************//**
Append deleted doc ids to vector and sort the vector. */
void
fts_cache_append_deleted_doc_ids(
/*=============================*/
	fts_cache_t*	cache,		/*!< in: cache to use */
	ib_vector_t*	vector);	/*!< in: append to this vector */
/******************************************************************//**
Search the index specific cache for a particular FTS index.
@return the index specific cache else NULL */
fts_index_cache_t*
fts_find_index_cache(
/*================*/
	const fts_cache_t*
			cache,		/*!< in: cache to search */
	const dict_index_t*
			index)		/*!< in: index to search for */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/******************************************************************//**
Write the table id to the given buffer (including final NUL). Buffer must be
at least FTS_AUX_MIN_TABLE_ID_LENGTH bytes long.
@return number of bytes written */
UNIV_INLINE
int
fts_write_object_id(
/*================*/
	ib_id_t		id,		/*!< in: a table/index id */
	char*		str);		/*!< in: buffer to write the id to */
/******************************************************************//**
Read the table id from the string generated by fts_write_object_id().
@return TRUE if parse successful */
UNIV_INLINE
ibool
fts_read_object_id(
/*===============*/
	ib_id_t*	id,		/*!< out: a table id */
	const char*	str)		/*!< in: buffer to read from */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/******************************************************************//**
Get the table id.
@return number of bytes written */
int
fts_get_table_id(
/*=============*/
	const fts_table_t*
			fts_table,	/*!< in: FTS Auxiliary table */
	char*		table_id)	/*!< out: table id, must be at least
					FTS_AUX_MIN_TABLE_ID_LENGTH bytes
					long */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/******************************************************************//**
Add node positions. */
void
fts_cache_node_add_positions(
/*=========================*/
	fts_cache_t*	cache,		/*!< in: cache */
	fts_node_t*	node,		/*!< in: word node */
	doc_id_t	doc_id,		/*!< in: doc id */
	ib_vector_t*	positions)	/*!< in: fts_token_t::positions */
	MY_ATTRIBUTE((nonnull(2,4)));

/******************************************************************//**
Create the config table name for retrieving index specific value.
@return index config parameter name */
char*
fts_config_create_index_param_name(
/*===============================*/
	const char*		param,	/*!< in: base name of param */
	const dict_index_t*	index)	/*!< in: index for config */
	MY_ATTRIBUTE((nonnull, malloc, warn_unused_result));

/** Callback function for fetching the config value. */
bool read_fts_config(dict_index_t *index, const rec_t *rec,
                     const rec_offs *offsets, void *user_arg);

/** Operation on FTS internal tables */
enum fts_operation
{
  INSERT,
  READ,
  SELECT_UPDATE,
  REMOVE
};

/** Match mode for fulltext tables */
enum fts_match_key
{
  /** Searching single unique records */
  MATCH_UNIQUE,
  /** Searching single key record in multiple key index */
  MATCH_PREFIX,
  /** Searching for pattern like records */
  MATCH_PATTERN,
  /** Traverse all records on the table */
  MATCH_ALL
};

/** This class does insert, delete, search in all FTS internal tables.*/
class FTSQueryRunner
{
private:
  /** Query thread */
  que_thr_t *m_thr= nullptr;
  /** Transaction to do the operation on FTS interal table */
  trx_t *m_trx= nullptr;
  /** Tuple to do DML operation on FTS internal table */
  dtuple_t *m_tuple= nullptr;
  /** Update build vector */
  upd_t *m_update= nullptr;
  /** system buffer to create DB_ROLL_PTR and DB_TRX_ID */
  byte *m_sys_buf= nullptr;
  /** Persistent cursor for search operation */
  btr_pcur_t *m_pcur= nullptr;
  /** Persistent cursor for clustered index */
  btr_pcur_t *m_clust_pcur= nullptr;
  /** Clustered index reference in case of lookup */
  dtuple_t *m_clust_ref= nullptr;
  /** lock type for the operation */
  lock_mode m_lock_type;
  /** Heap for old version record */
  mem_heap_t *m_old_vers_heap= nullptr;
  /** Heap to allocate query thread */
  mem_heap_t *m_heap= nullptr;

private:
  /** Create a query thread for executing the query */
  void create_query_thread() noexcept;

  /** Handle the lock wait while doing any operation on FTS internal table
  @param err error to be handled
  @param table_lock Encountered the error during table lock
  @return DB_SUCCESS in case of lock wait or error code */
  dberr_t handle_wait(dberr_t err, bool table_lock= false) noexcept;

  /** Lock the given record based on operation, reconstruct the
  previous of the record based on transaction read view.
  @param index    record where index belongs to
  @param rec      record to be locked
  @param out_rec  record which is viewable by transaction
  @param offsets  offsets of the record
  @param mtr      mini-transaction
  @param op       Operation to be performed
  @return error code or DB_SUCCESS */
  dberr_t lock_or_sees_rec(dict_index_t *index, const rec_t *rec,
                           const rec_t **out_rec, rec_offs **offsets,
                           mtr_t *mtr, fts_operation op) noexcept;

  /** Build clustered reference in case of clustered index lookup */
  void build_clust_ref(dict_index_t *index) noexcept;

public:
  /** Build a tuple based on the given index of the table */
  void build_tuple(dict_index_t *index, uint32_t n_fields= 0,
                   uint32_t n_uniq= 0) noexcept;

  /** Assign the FTS config fields for the tuple
  @param name key for the field
  @param value value for the field
  @param value_len length of value string */
  void assign_config_fields(const char* name, const void *value= nullptr,
                            ulint value_len= 0) noexcept;

  /** Assign the FTS common table fields
  @param doc_id doc_id to be assigned */
  void assign_common_table_fields(doc_id_t *doc_id) noexcept;

  /** Assign the auxiliary table fields
  @param word       word to be initialized
  @param word_len   length of the word
  @param node information about word where it stored */
  void assign_aux_table_fields(const byte *word, ulint word_len,
                               const fts_node_t *node= nullptr) noexcept;

  /** Build update vector for the value field of fts internal config table
  @param table     fulltext config table
  @param field_no  value field number
  @param new_value value to be updated */
  void build_update_config(dict_table_t *table, uint16_t field_no,
                           const fts_string_t *new_value) noexcept;

  /** Open the table based on fts_auxiliary table name
  @param fts_table    fts table internal name
  @param err          set error code in case of table is not open
  @param dict_locked  dictionary locked
  @retval DB_TABLE_NOT_FOUND in case if table has been dropped
  @retval DB_SUCCESS in case of success */
  dict_table_t *open_table(fts_table_t *fts_table, dberr_t *err,
                           bool dict_locked= false) noexcept;

  /** Prepare the table for write operation
  @param table table to be prepared
  @retval DB_SUCCESS In case of success
  @retrun error code for failure */
  dberr_t prepare_for_write(dict_table_t *table) noexcept;

  /** Prepare the table for read operation
  @param table table to be prepared
  @retval DB_SUCCESS In case of success
  @retrun error code for failure */
  dberr_t prepare_for_read(dict_table_t *table) noexcept;

  /** Insert the record in the given table.
  @param table table where insert is going to happen
  @return error code or DB_SUCCESS */
  dberr_t write_record(dict_table_t *table) noexcept;

  /** Iterate the record and execute the functionality depends
  on the operation. Lock the record when operation is
  a delete (or) replace statement
  @param index     Doing the operation on the given index
  @param op        Read (or) Write (or) Delete (or) select..for update
  @param match_op  Match mode for the given tuple
  @param mode      Page cursor mode
  @param func      function pointer for read operation
  @param user_arg  user argument for function pointer
  @return DB_SUCCESS or error code */
  dberr_t record_executor(dict_index_t *index, fts_operation op,
                          fts_match_key match_op= MATCH_UNIQUE,
                          page_cur_mode_t mode= PAGE_CUR_GE,
                          fts_sql_callback *func= nullptr,
                          void *user_arg= nullptr) noexcept;

  /** Update the clustered index of the table based on m_update and m_pcur
  @param table    table to be updated
  @param old_len  old length of the old data
  @param new_len  new length of the new data
  @return error code or DB_SUCCESS */
  dberr_t update_record(dict_table_t *table, uint16_t old_len,
                        uint16_t new_len) noexcept;

  /** Get the heap */
  mem_heap_t *heap() noexcept { return m_heap; }

  dtuple_t *tuple() noexcept { return m_tuple; }

  FTSQueryRunner(trx_t *trx): m_trx(trx) { m_heap= mem_heap_create(100); }

  ~FTSQueryRunner();

#ifdef UNIV_DEBUG
  bool is_stopword_table= false;
#endif
};
#include "fts0priv.inl"

#endif /* INNOBASE_FTS0PRIV_H */
