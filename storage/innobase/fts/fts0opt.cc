/*****************************************************************************

Copyright (c) 2007, 2018, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2022, MariaDB Corporation.

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
@file fts/fts0opt.cc
Full Text Search optimize thread

Created 2007/03/27 Sunny Bains
Completed 2011/7/10 Sunny and Jimmy Yang

***********************************************************************/

#include "fts0fts.h"
#include "row0sel.h"
#include "que0types.h"
#include "fts0priv.h"
#include "fts0types.h"
#include "ut0wqueue.h"
#include "srv0start.h"
#include "ut0list.h"
#include "zlib.h"
#include "fts0opt.h"
#include "fts0vlc.h"
#include "wsrep.h"

#ifdef WITH_WSREP
extern Atomic_relaxed<bool> wsrep_sst_disable_writes;
#else
constexpr bool wsrep_sst_disable_writes= false;
#endif

/** The FTS optimize thread's work queue. */
ib_wqueue_t* fts_optimize_wq;
static void fts_optimize_callback(void *);
static void timer_callback(void*);
static tpool::timer* timer;

static tpool::task_group task_group(1);
static tpool::task task(fts_optimize_callback,0, &task_group);

/** FTS optimize thread, for MDL acquisition */
static THD *fts_opt_thd;

/** The FTS vector to store fts_slot_t */
static ib_vector_t*  fts_slots;

/** Default optimize interval in secs. */
static const ulint FTS_OPTIMIZE_INTERVAL_IN_SECS = 300;

/** Server is shutting down, so does we exiting the optimize thread */
static bool fts_opt_start_shutdown = false;

/** Condition variable for shutting down the optimize thread.
Protected by fts_optimize_wq->mutex. */
static pthread_cond_t fts_opt_shutdown_cond;

/** Initial size of nodes in fts_word_t. */
static const ulint FTS_WORD_NODES_INIT_SIZE = 64;

/** Last time we did check whether system need a sync */
static time_t	last_check_sync_time;

/** FTS optimize thread message types. */
enum fts_msg_type_t {
	FTS_MSG_STOP,			/*!< Stop optimizing and exit thread */

	FTS_MSG_ADD_TABLE,		/*!< Add table to the optimize thread's
					work queue */

	FTS_MSG_DEL_TABLE,		/*!< Remove a table from the optimize
					threads work queue */
	FTS_MSG_SYNC_TABLE		/*!< Sync fts cache of a table */
};

/** Compressed list of words that have been read from FTS INDEX
that needs to be optimized. */
struct fts_zip_t {
	lint		status;		/*!< Status of (un)/zip operation */

	ulint		n_words;	/*!< Number of words compressed */

	ulint		block_sz;	/*!< Size of a block in bytes */

	ib_vector_t*	blocks;		/*!< Vector of compressed blocks */

	ib_alloc_t*	heap_alloc;	/*!< Heap to use for allocations */

	ulint		pos;		/*!< Offset into blocks */

	ulint		last_big_block;	/*!< Offset of last block in the
					blocks array that is of size
					block_sz. Blocks beyond this offset
					are of size FTS_MAX_WORD_LEN */

	z_streamp	zp;		/*!< ZLib state */

					/*!< The value of the last word read
					from the FTS INDEX table. This is
					used to discard duplicates */

	fts_string_t	word;		/*!< UTF-8 string */

	ulint		max_words;	/*!< maximum number of words to read
					in one pass */
};

/** Prepared statements used during optimize */
struct fts_optimize_graph_t {
					/*!< Delete a word from FTS INDEX */
	que_t*		delete_nodes_graph;
					/*!< Insert a word into FTS INDEX */
	que_t*		write_nodes_graph;
					/*!< COMMIT a transaction */
	que_t*		commit_graph;
					/*!< Read the nodes from FTS_INDEX */
	que_t*		read_nodes_graph;
};

/** Used by fts_optimize() to store state. */
struct fts_optimize_t {
	trx_t*		trx;		/*!< The transaction used for all SQL */

	ib_alloc_t*	self_heap;	/*!< Heap to use for allocations */

	char*		name_prefix;	/*!< FTS table name prefix */

	fts_table_t	fts_index_table;/*!< Common table definition */

					/*!< Common table definition */
	fts_table_t	fts_common_table;

	dict_table_t*	table;		/*!< Table that has to be queried */

	dict_index_t*	index;		/*!< The FTS index to be optimized */

	fts_doc_ids_t*	to_delete;	/*!< doc ids to delete, we check against
					this vector and purge the matching
					entries during the optimizing
					process. The vector entries are
					sorted on doc id */

	ulint		del_pos;	/*!< Offset within to_delete vector,
					this is used to keep track of where
					we are up to in the vector */

	ibool		done;		/*!< TRUE when optimize finishes */

	ib_vector_t*	words;		/*!< Word + Nodes read from FTS_INDEX,
					it contains instances of fts_word_t */

	fts_zip_t*	zip;		/*!< Words read from the FTS_INDEX */

	fts_optimize_graph_t		/*!< Prepared statements used during */
			graph;		/*optimize */

	ulint		n_completed;	/*!< Number of FTS indexes that have
					been optimized */
	ibool		del_list_regenerated;
					/*!< BEING_DELETED list regenerated */
};

/** Used by the optimize, to keep state during compacting nodes. */
struct fts_encode_t {
	doc_id_t	src_last_doc_id;/*!< Last doc id read from src node */
	byte*		src_ilist_ptr;	/*!< Current ptr within src ilist */
};

/** We use this information to determine when to start the optimize
cycle for a table. */
struct fts_slot_t {
	/** table, or NULL if the slot is unused */
	dict_table_t*	table;

	/** whether this slot is being processed */
	bool		running;

	ulint		added;		/*!< Number of doc ids added since the
					last time this table was optimized */

	ulint		deleted;	/*!< Number of doc ids deleted since the
					last time this table was optimized */

	/** time(NULL) of completing fts_optimize_table_bk() */
	time_t		last_run;

	/** time(NULL) of latest successful fts_optimize_table() */
	time_t		completed;
};

/** A table remove message for the FTS optimize thread. */
struct fts_msg_del_t
{
  /** the table to remove */
  dict_table_t *table;
  /** condition variable to signal message consumption */
  pthread_cond_t *cond;
};

/** The FTS optimize message work queue message type. */
struct fts_msg_t {
	fts_msg_type_t	type;		/*!< Message type */

	void*		ptr;		/*!< The message contents */

	mem_heap_t*	heap;		/*!< The heap used to allocate this
					message, the message consumer will
					free the heap. */
};

/** The number of words to read and optimize in a single pass. */
ulong	fts_num_word_optimize;

/** Whether to enable additional FTS diagnostic printout. */
char	fts_enable_diag_print;

/** ZLib compressed block size.*/
static ulint FTS_ZIP_BLOCK_SIZE	= 1024;

/** The amount of time optimizing in a single pass, in seconds. */
static ulint fts_optimize_time_limit;

/** It's defined in fts0fts.cc  */
extern const char* fts_common_tables[];

/** SQL Statement for changing state of rows to be deleted from FTS Index. */
static	const char* fts_init_delete_sql =
	"BEGIN\n"
	"\n"
	"INSERT INTO $BEING_DELETED\n"
		"SELECT doc_id FROM $DELETED;\n"
	"\n"
	"INSERT INTO $BEING_DELETED_CACHE\n"
		"SELECT doc_id FROM $DELETED_CACHE;\n";

static const char* fts_delete_doc_ids_sql =
	"BEGIN\n"
	"\n"
	"DELETE FROM $DELETED WHERE doc_id = :doc_id1;\n"
	"DELETE FROM $DELETED_CACHE WHERE doc_id = :doc_id2;\n";

static const char* fts_end_delete_sql =
	"BEGIN\n"
	"\n"
	"DELETE FROM $BEING_DELETED;\n"
	"DELETE FROM $BEING_DELETED_CACHE;\n";

/**********************************************************************//**
Initialize fts_zip_t. */
static
void
fts_zip_initialize(
/*===============*/
	fts_zip_t*	zip)		/*!< out: zip instance to initialize */
{
	zip->pos = 0;
	zip->n_words = 0;

	zip->status = Z_OK;

	zip->last_big_block = 0;

	zip->word.f_len = 0;
	*zip->word.f_str = 0;

	ib_vector_reset(zip->blocks);

	memset(zip->zp, 0, sizeof(*zip->zp));
}

/**********************************************************************//**
Create an instance of fts_zip_t.
@return a new instance of fts_zip_t */
static
fts_zip_t*
fts_zip_create(
/*===========*/
	mem_heap_t*	heap,		/*!< in: heap */
	ulint		block_sz,	/*!< in: size of a zip block.*/
	ulint		max_words)	/*!< in: max words to read */
{
	fts_zip_t*	zip;

	zip = static_cast<fts_zip_t*>(mem_heap_zalloc(heap, sizeof(*zip)));

	zip->word.f_str = static_cast<byte*>(
		mem_heap_zalloc(heap, FTS_MAX_WORD_LEN + 1));

	zip->block_sz = block_sz;

	zip->heap_alloc = ib_heap_allocator_create(heap);

	zip->blocks = ib_vector_create(zip->heap_alloc, sizeof(void*), 128);

	zip->max_words = max_words;

	zip->zp = static_cast<z_stream*>(
		mem_heap_zalloc(heap, sizeof(*zip->zp)));

	return(zip);
}

/**********************************************************************//**
Initialize an instance of fts_zip_t. */
static
void
fts_zip_init(
/*=========*/

	fts_zip_t*	zip)		/*!< in: zip instance to init */
{
	memset(zip->zp, 0, sizeof(*zip->zp));

	zip->word.f_len = 0;
	*zip->word.f_str = '\0';
}

/**********************************************************************//**
Create a fts_optimizer_word_t instance.
@return new instance */
static
fts_word_t*
fts_word_init(
/*==========*/
	fts_word_t*	word,		/*!< in: word to initialize */
	byte*		utf8,		/*!< in: UTF-8 string */
	ulint		len)		/*!< in: length of string in bytes */
{
	mem_heap_t*	heap = mem_heap_create(sizeof(fts_node_t));

	memset(word, 0, sizeof(*word));

	word->text.f_len = len;
	word->text.f_str = static_cast<byte*>(mem_heap_alloc(heap, len + 1));

	/* Need to copy the NUL character too. */
	memcpy(word->text.f_str, utf8, word->text.f_len);
	word->text.f_str[word->text.f_len] = 0;

	word->heap_alloc = ib_heap_allocator_create(heap);

	word->nodes = ib_vector_create(
		word->heap_alloc, sizeof(fts_node_t), FTS_WORD_NODES_INIT_SIZE);

	return(word);
}

/**********************************************************************//**
Read the FTS INDEX row.
@return fts_node_t instance */
static
fts_node_t*
fts_optimize_read_node(
/*===================*/
	fts_word_t*	word,		/*!< in: */
	que_node_t*	exp)		/*!< in: */
{
	int		i;
	fts_node_t*	node = static_cast<fts_node_t*>(
		ib_vector_push(word->nodes, NULL));

	/* Start from 1 since the first node has been read by the caller */
	for (i = 1; exp; exp = que_node_get_next(exp), ++i) {

		dfield_t*	dfield = que_node_get_val(exp);
		byte*		data = static_cast<byte*>(
			dfield_get_data(dfield));
		ulint		len = dfield_get_len(dfield);

		ut_a(len != UNIV_SQL_NULL);

		/* Note: The column numbers below must match the SELECT */
		switch (i) {
		case 1: /* DOC_COUNT */
			node->doc_count = mach_read_from_4(data);
			break;

		case 2: /* FIRST_DOC_ID */
			node->first_doc_id = fts_read_doc_id(data);
			break;

		case 3: /* LAST_DOC_ID */
			node->last_doc_id = fts_read_doc_id(data);
			break;

		case 4: /* ILIST */
			node->ilist_size_alloc = node->ilist_size = len;
			node->ilist = static_cast<byte*>(ut_malloc_nokey(len));
			memcpy(node->ilist, data, len);
			break;

		default:
			ut_error;
		}
	}

	/* Make sure all columns were read. */
	ut_a(i == 5);

	return(node);
}

/**********************************************************************//**
Callback function to fetch the rows in an FTS INDEX record.
@return always returns non-NULL */
ibool
fts_optimize_index_fetch_node(
/*==========================*/
	void*		row,		/*!< in: sel_node_t* */
	void*		user_arg)	/*!< in: pointer to ib_vector_t */
{
	fts_word_t*	word;
	sel_node_t*	sel_node = static_cast<sel_node_t*>(row);
	fts_fetch_t*	fetch = static_cast<fts_fetch_t*>(user_arg);
	ib_vector_t*	words = static_cast<ib_vector_t*>(fetch->read_arg);
	que_node_t*	exp = sel_node->select_list;
	dfield_t*	dfield = que_node_get_val(exp);
	void*		data = dfield_get_data(dfield);
	ulint		dfield_len = dfield_get_len(dfield);
	fts_node_t*	node;
	bool		is_word_init = false;

	ut_a(dfield_len <= FTS_MAX_WORD_LEN);

	if (ib_vector_size(words) == 0) {

		word = static_cast<fts_word_t*>(ib_vector_push(words, NULL));
		fts_word_init(word, (byte*) data, dfield_len);
		is_word_init = true;
	}

	word = static_cast<fts_word_t*>(ib_vector_last(words));

	if (dfield_len != word->text.f_len
	    || memcmp(word->text.f_str, data, dfield_len)) {

		word = static_cast<fts_word_t*>(ib_vector_push(words, NULL));
		fts_word_init(word, (byte*) data, dfield_len);
		is_word_init = true;
	}

	node = fts_optimize_read_node(word, que_node_get_next(exp));

	fetch->total_memory += node->ilist_size;
	if (is_word_init) {
		fetch->total_memory += sizeof(fts_word_t)
			+ sizeof(ib_alloc_t) + sizeof(ib_vector_t) + dfield_len
			+ sizeof(fts_node_t) * FTS_WORD_NODES_INIT_SIZE;
	} else if (ib_vector_size(words) > FTS_WORD_NODES_INIT_SIZE) {
		fetch->total_memory += sizeof(fts_node_t);
	}

	if (fetch->total_memory >= fts_result_cache_limit) {
		return(FALSE);
	}

	return(TRUE);
}

/**********************************************************************//**
Read the rows from the FTS inde.
@return DB_SUCCESS or error code */
dberr_t
fts_index_fetch_nodes(
/*==================*/
	trx_t*		trx,		/*!< in: transaction */
	que_t**		graph,		/*!< in: prepared statement */
	fts_table_t*	fts_table,	/*!< in: table of the FTS INDEX */
	const fts_string_t*
			word,		/*!< in: the word to fetch */
	fts_fetch_t*	fetch)		/*!< in: fetch callback.*/
{
	pars_info_t*	info;
	dberr_t		error;
	char		table_name[MAX_FULL_NAME_LEN];

	trx->op_info = "fetching FTS index nodes";

	if (*graph) {
		info = (*graph)->info;
	} else {
		ulint	selected;

		info = pars_info_create();

		ut_a(fts_table->type == FTS_INDEX_TABLE);

		selected = fts_select_index(fts_table->charset,
					    word->f_str, word->f_len);

		fts_table->suffix = fts_get_suffix(selected);

		fts_get_table_name(fts_table, table_name);

		pars_info_bind_id(info, "table_name", table_name);
	}

	pars_info_bind_function(info, "my_func", fetch->read_record, fetch);
	pars_info_bind_varchar_literal(info, "word", word->f_str, word->f_len);

	if (!*graph) {

		*graph = fts_parse_sql(
			fts_table,
			info,
			"DECLARE FUNCTION my_func;\n"
			"DECLARE CURSOR c IS"
			" SELECT word, doc_count, first_doc_id, last_doc_id,"
			" ilist\n"
			" FROM $table_name\n"
			" WHERE word LIKE :word\n"
			" ORDER BY first_doc_id;\n"
			"BEGIN\n"
			"\n"
			"OPEN c;\n"
			"WHILE 1 = 1 LOOP\n"
			"  FETCH c INTO my_func();\n"
			"  IF c % NOTFOUND THEN\n"
			"    EXIT;\n"
			"  END IF;\n"
			"END LOOP;\n"
			"CLOSE c;");
	}

	for (;;) {
		error = fts_eval_sql(trx, *graph);

		if (UNIV_LIKELY(error == DB_SUCCESS)) {
			fts_sql_commit(trx);

			break;				/* Exit the loop. */
		} else {
			fts_sql_rollback(trx);

			if (error == DB_LOCK_WAIT_TIMEOUT) {
				ib::warn() << "lock wait timeout reading"
					" FTS index. Retrying!";

				trx->error_state = DB_SUCCESS;
			} else {
				ib::error() << "(" << error
					<< ") while reading FTS index.";

				break;			/* Exit the loop. */
			}
		}
	}

	return(error);
}

/**********************************************************************//**
Read a word */
static
byte*
fts_zip_read_word(
/*==============*/
	fts_zip_t*	zip,		/*!< in: Zip state + data */
	fts_string_t*	word)		/*!< out: uncompressed word */
{
	short		len = 0;
	void*		null = NULL;
	byte*		ptr = word->f_str;
	int		flush = Z_NO_FLUSH;

	/* Either there was an error or we are at the Z_STREAM_END. */
	if (zip->status != Z_OK) {
		return(NULL);
	}

	zip->zp->next_out = reinterpret_cast<byte*>(&len);
	zip->zp->avail_out = sizeof(len);

	while (zip->status == Z_OK && zip->zp->avail_out > 0) {

		/* Finished decompressing block. */
		if (zip->zp->avail_in == 0) {

			/* Free the block that's been decompressed. */
			if (zip->pos > 0) {
				ulint	prev = zip->pos - 1;

				ut_a(zip->pos < ib_vector_size(zip->blocks));

				ut_free(ib_vector_getp(zip->blocks, prev));
				ib_vector_set(zip->blocks, prev, &null);
			}

			/* Any more blocks to decompress. */
			if (zip->pos < ib_vector_size(zip->blocks)) {

				zip->zp->next_in = static_cast<byte*>(
					ib_vector_getp(
						zip->blocks, zip->pos));

				if (zip->pos > zip->last_big_block) {
					zip->zp->avail_in =
						FTS_MAX_WORD_LEN;
				} else {
					zip->zp->avail_in =
						static_cast<uInt>(zip->block_sz);
				}

				++zip->pos;
			} else {
				flush = Z_FINISH;
			}
		}

		switch (zip->status = inflate(zip->zp, flush)) {
		case Z_OK:
			if (zip->zp->avail_out == 0 && len > 0) {

				ut_a(len <= FTS_MAX_WORD_LEN);
				ptr[len] = 0;

				zip->zp->next_out = ptr;
				zip->zp->avail_out = uInt(len);

				word->f_len = ulint(len);
				len = 0;
			}
			break;

		case Z_BUF_ERROR:	/* No progress possible. */
		case Z_STREAM_END:
			inflateEnd(zip->zp);
			break;

		case Z_STREAM_ERROR:
		default:
			ut_error;
		}
	}

	/* All blocks must be freed at end of inflate. */
	if (zip->status != Z_OK) {
		for (ulint i = 0; i < ib_vector_size(zip->blocks); ++i) {
			if (ib_vector_getp(zip->blocks, i)) {
				ut_free(ib_vector_getp(zip->blocks, i));
				ib_vector_set(zip->blocks, i, &null);
			}
		}
	}

	if (ptr != NULL) {
		ut_ad(word->f_len == strlen((char*) ptr));
	}

	return(zip->status == Z_OK || zip->status == Z_STREAM_END ? ptr : NULL);
}

/**********************************************************************//**
Callback function to fetch and compress the word in an FTS
INDEX record.
@return FALSE on EOF */
static
ibool
fts_fetch_index_words(
/*==================*/
	void*		row,		/*!< in: sel_node_t* */
	void*		user_arg)	/*!< in: pointer to ib_vector_t */
{
	sel_node_t*	sel_node = static_cast<sel_node_t*>(row);
	fts_zip_t*	zip = static_cast<fts_zip_t*>(user_arg);
	que_node_t*	exp = sel_node->select_list;
	dfield_t*	dfield = que_node_get_val(exp);

	ut_a(dfield_get_len(dfield) <= FTS_MAX_WORD_LEN);

	uint16		len = uint16(dfield_get_len(dfield));
	void*		data = dfield_get_data(dfield);

	/* Skip the duplicate words. */
	if (zip->word.f_len == len && !memcmp(zip->word.f_str, data, len)) {
		return(TRUE);
	}

	memcpy(zip->word.f_str, data, len);
	zip->word.f_len = len;

	ut_a(zip->zp->avail_in == 0);
	ut_a(zip->zp->next_in == NULL);

	/* The string is prefixed by len. */
	/* FIXME: This is not byte order agnostic (InnoDB data files
	with FULLTEXT INDEX are not portable between little-endian and
	big-endian systems!) */
	zip->zp->next_in = reinterpret_cast<byte*>(&len);
	zip->zp->avail_in = sizeof(len);

	/* Compress the word, create output blocks as necessary. */
	while (zip->zp->avail_in > 0) {

		/* No space left in output buffer, create a new one. */
		if (zip->zp->avail_out == 0) {
			byte*		block;

			block = static_cast<byte*>(
				ut_malloc_nokey(zip->block_sz));

			ib_vector_push(zip->blocks, &block);

			zip->zp->next_out = block;
			zip->zp->avail_out = static_cast<uInt>(zip->block_sz);
		}

		switch (zip->status = deflate(zip->zp, Z_NO_FLUSH)) {
		case Z_OK:
			if (zip->zp->avail_in == 0) {
				zip->zp->next_in = static_cast<byte*>(data);
				zip->zp->avail_in = uInt(len);
				ut_a(len <= FTS_MAX_WORD_LEN);
				len = 0;
			}
			continue;

		case Z_STREAM_END:
		case Z_BUF_ERROR:
		case Z_STREAM_ERROR:
		default:
			ut_error;
		}
	}

	/* All data should have been compressed. */
	ut_a(zip->zp->avail_in == 0);
	zip->zp->next_in = NULL;

	++zip->n_words;

	return(zip->n_words >= zip->max_words ? FALSE : TRUE);
}

/**********************************************************************//**
Finish Zip deflate. */
static
void
fts_zip_deflate_end(
/*================*/
	fts_zip_t*	zip)		/*!< in: instance that should be closed*/
{
	ut_a(zip->zp->avail_in == 0);
	ut_a(zip->zp->next_in == NULL);

	zip->status = deflate(zip->zp, Z_FINISH);

	ut_a(ib_vector_size(zip->blocks) > 0);
	zip->last_big_block = ib_vector_size(zip->blocks) - 1;

	/* Allocate smaller block(s), since this is trailing data. */
	while (zip->status == Z_OK) {
		byte*		block;

		ut_a(zip->zp->avail_out == 0);

		block = static_cast<byte*>(
			ut_malloc_nokey(FTS_MAX_WORD_LEN + 1));

		ib_vector_push(zip->blocks, &block);

		zip->zp->next_out = block;
		zip->zp->avail_out = FTS_MAX_WORD_LEN;

		zip->status = deflate(zip->zp, Z_FINISH);
	}

	ut_a(zip->status == Z_STREAM_END);

	zip->status = deflateEnd(zip->zp);
	ut_a(zip->status == Z_OK);

	/* Reset the ZLib data structure. */
	memset(zip->zp, 0, sizeof(*zip->zp));
}

/**********************************************************************//**
Read the words from the FTS INDEX.
@return DB_SUCCESS if all OK, DB_TABLE_NOT_FOUND if no more indexes
        to search else error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_index_fetch_words(
/*==================*/
	fts_optimize_t*		optim,	/*!< in: optimize scratch pad */
	const fts_string_t*	word,	/*!< in: get words greater than this
					 word */
	ulint			n_words)/*!< in: max words to read */
{
	pars_info_t*	info;
	que_t*		graph;
	ulint		selected;
	fts_zip_t*	zip = NULL;
	dberr_t		error = DB_SUCCESS;
	mem_heap_t*	heap = static_cast<mem_heap_t*>(optim->self_heap->arg);
	ibool		inited = FALSE;

	optim->trx->op_info = "fetching FTS index words";

	if (optim->zip == NULL) {
		optim->zip = fts_zip_create(heap, FTS_ZIP_BLOCK_SIZE, n_words);
	} else {
		fts_zip_initialize(optim->zip);
	}

	for (selected = fts_select_index(
		     optim->fts_index_table.charset, word->f_str, word->f_len);
	     selected < FTS_NUM_AUX_INDEX;
	     selected++) {

		char	table_name[MAX_FULL_NAME_LEN];

		optim->fts_index_table.suffix = fts_get_suffix(selected);

		info = pars_info_create();

		pars_info_bind_function(
			info, "my_func", fts_fetch_index_words, optim->zip);

		pars_info_bind_varchar_literal(
			info, "word", word->f_str, word->f_len);

		fts_get_table_name(&optim->fts_index_table, table_name);
		pars_info_bind_id(info, "table_name", table_name);

		graph = fts_parse_sql(
			&optim->fts_index_table,
			info,
			"DECLARE FUNCTION my_func;\n"
			"DECLARE CURSOR c IS"
			" SELECT word\n"
			" FROM $table_name\n"
			" WHERE word > :word\n"
			" ORDER BY word;\n"
			"BEGIN\n"
			"\n"
			"OPEN c;\n"
			"WHILE 1 = 1 LOOP\n"
			"  FETCH c INTO my_func();\n"
			"  IF c % NOTFOUND THEN\n"
			"    EXIT;\n"
			"  END IF;\n"
			"END LOOP;\n"
			"CLOSE c;");

		zip = optim->zip;

		for (;;) {
			int	err;

			if (!inited && ((err = deflateInit(zip->zp, 9))
					!= Z_OK)) {
				ib::error() << "ZLib deflateInit() failed: "
					<< err;

				error = DB_ERROR;
				break;
			} else {
				inited = TRUE;
				error = fts_eval_sql(optim->trx, graph);
			}

			if (UNIV_LIKELY(error == DB_SUCCESS)) {
				//FIXME fts_sql_commit(optim->trx);
				break;
			} else {
				//FIXME fts_sql_rollback(optim->trx);

				if (error == DB_LOCK_WAIT_TIMEOUT) {
					ib::warn() << "Lock wait timeout"
						" reading document. Retrying!";

					/* We need to reset the ZLib state. */
					inited = FALSE;
					deflateEnd(zip->zp);
					fts_zip_init(zip);

					optim->trx->error_state = DB_SUCCESS;
				} else {
					ib::error() << "(" << error
						<< ") while reading document.";

					break;	/* Exit the loop. */
				}
			}
		}

		que_graph_free(graph);

		/* Check if max word to fetch is exceeded */
		if (optim->zip->n_words >= n_words) {
			break;
		}
	}

	if (error == DB_SUCCESS && zip->status == Z_OK && zip->n_words > 0) {

		/* All data should have been read. */
		ut_a(zip->zp->avail_in == 0);

		fts_zip_deflate_end(zip);
	} else {
		deflateEnd(zip->zp);
	}

	return(error);
}

/**********************************************************************//**
Callback function to fetch the doc id from the record.
@return always returns TRUE */
static
ibool
fts_fetch_doc_ids(
/*==============*/
	void*	row,		/*!< in: sel_node_t* */
	void*	user_arg)	/*!< in: pointer to ib_vector_t */
{
	que_node_t*	exp;
	int		i = 0;
	sel_node_t*	sel_node = static_cast<sel_node_t*>(row);
	fts_doc_ids_t*	fts_doc_ids = static_cast<fts_doc_ids_t*>(user_arg);
	doc_id_t*	update = static_cast<doc_id_t*>(
		ib_vector_push(fts_doc_ids->doc_ids, NULL));

	for (exp = sel_node->select_list;
	     exp;
	     exp = que_node_get_next(exp), ++i) {

		dfield_t*	dfield = que_node_get_val(exp);
		void*		data = dfield_get_data(dfield);
		ulint		len = dfield_get_len(dfield);

		ut_a(len != UNIV_SQL_NULL);

		/* Note: The column numbers below must match the SELECT. */
		switch (i) {
		case 0: /* DOC_ID */
			*update = fts_read_doc_id(
				static_cast<byte*>(data));
			break;

		default:
			ut_error;
		}
	}

	return(TRUE);
}

/**********************************************************************//**
Read the rows from a FTS common auxiliary table.
@return DB_SUCCESS or error code */
dberr_t
fts_table_fetch_doc_ids(
/*====================*/
	trx_t*		trx,		/*!< in: transaction */
	fts_table_t*	fts_table,	/*!< in: table */
	fts_doc_ids_t*	doc_ids)	/*!< in: For collecting doc ids */
{
	dberr_t		error;
	que_t*		graph;
	pars_info_t*	info = pars_info_create();
	ibool		alloc_bk_trx = FALSE;
	char		table_name[MAX_FULL_NAME_LEN];

	ut_a(fts_table->suffix != NULL);
	ut_a(fts_table->type == FTS_COMMON_TABLE);

	if (!trx) {
		trx = trx_create();
		alloc_bk_trx = TRUE;
	}

	trx->op_info = "fetching FTS doc ids";

	pars_info_bind_function(info, "my_func", fts_fetch_doc_ids, doc_ids);

	fts_get_table_name(fts_table, table_name);
	pars_info_bind_id(info, "table_name", table_name);

	graph = fts_parse_sql(
		fts_table,
		info,
		"DECLARE FUNCTION my_func;\n"
		"DECLARE CURSOR c IS"
		" SELECT doc_id FROM $table_name;\n"
		"BEGIN\n"
		"\n"
		"OPEN c;\n"
		"WHILE 1 = 1 LOOP\n"
		"  FETCH c INTO my_func();\n"
		"  IF c % NOTFOUND THEN\n"
		"    EXIT;\n"
		"  END IF;\n"
		"END LOOP;\n"
		"CLOSE c;");

	error = fts_eval_sql(trx, graph);
	fts_sql_commit(trx);
	que_graph_free(graph);

	if (error == DB_SUCCESS) {
		fts_doc_ids_sort(doc_ids->doc_ids);
	}

	if (alloc_bk_trx) {
		trx->free();
	}

	return(error);
}

/**********************************************************************//**
Do a binary search for a doc id in the array
@return +ve index if found -ve index where it should be inserted
        if not found */
int
fts_bsearch(
/*========*/
	doc_id_t*	array,	/*!< in: array to sort */
	int		lower,	/*!< in: the array lower bound */
	int		upper,	/*!< in: the array upper bound */
	doc_id_t	doc_id)	/*!< in: the doc id to search for */
{
	int	orig_size = upper;

	if (upper == 0) {
		/* Nothing to search */
		return(-1);
	} else {
		while (lower < upper) {
			int	i = (lower + upper) >> 1;

			if (doc_id > array[i]) {
				lower = i + 1;
			} else if (doc_id < array[i]) {
				upper = i - 1;
			} else {
				return(i); /* Found. */
			}
		}
	}

	if (lower == upper && lower < orig_size) {
		if (doc_id == array[lower]) {
			return(lower);
		} else if (lower == 0) {
			return(-1);
		}
	}

	/* Not found. */
	return( (lower == 0) ? -1 : -(lower));
}

/**********************************************************************//**
Search in the to delete array whether any of the doc ids within
the [first, last] range are to be deleted
@return +ve index if found -ve index where it should be inserted
        if not found */
static
int
fts_optimize_lookup(
/*================*/
	ib_vector_t*	doc_ids,	/*!< in: array to search */
	ulint		lower,		/*!< in: lower limit of array */
	doc_id_t	first_doc_id,	/*!< in: doc id to lookup */
	doc_id_t	last_doc_id)	/*!< in: doc id to lookup */
{
	int		pos;
	int		upper = static_cast<int>(ib_vector_size(doc_ids));
	doc_id_t*	array = (doc_id_t*) doc_ids->data;

	pos = fts_bsearch(array, static_cast<int>(lower), upper, first_doc_id);

	ut_a(abs(pos) <= upper + 1);

	if (pos < 0) {

		int	i = abs(pos);

		/* If i is 1, it could be first_doc_id is less than
		either the first or second array item, do a
		double check */
		if (i == 1 && array[0] <= last_doc_id
		    && first_doc_id < array[0]) {
			pos = 0;
		} else if (i < upper && array[i] <= last_doc_id) {

			/* Check if the "next" doc id is within the
			first & last doc id of the node. */
			pos = i;
		}
	}

	return(pos);
}

/**********************************************************************//**
Encode the word pos list into the node
@return DB_SUCCESS or error code*/
static MY_ATTRIBUTE((nonnull))
dberr_t
fts_optimize_encode_node(
/*=====================*/
	fts_node_t*	node,		/*!< in: node to fill*/
	doc_id_t	doc_id,		/*!< in: doc id to encode */
	fts_encode_t*	enc)		/*!< in: encoding state.*/
{
	byte*		dst;
	ulint		enc_len;
	ulint		pos_enc_len;
	doc_id_t	doc_id_delta;
	dberr_t		error = DB_SUCCESS;
	const byte*	src = enc->src_ilist_ptr;

	if (node->first_doc_id == 0) {
		ut_a(node->last_doc_id == 0);

		node->first_doc_id = doc_id;
	}

	/* Calculate the space required to store the ilist. */
	ut_ad(doc_id > node->last_doc_id);
	doc_id_delta = doc_id - node->last_doc_id;
	enc_len = fts_get_encoded_len(static_cast<ulint>(doc_id_delta));

	/* Calculate the size of the encoded pos array. */
	while (*src) {
		fts_decode_vlc(&src);
	}

	/* Skip the 0x00 byte at the end of the word positions list. */
	++src;

	/* Number of encoded pos bytes to copy. */
	pos_enc_len = ulint(src - enc->src_ilist_ptr);

	/* Total number of bytes required for copy. */
	enc_len += pos_enc_len;

	/* Check we have enough space in the destination buffer for
	copying the document word list. */
	if (!node->ilist) {
		ulint	new_size;

		ut_a(node->ilist_size == 0);

		new_size = enc_len > FTS_ILIST_MAX_SIZE
			? enc_len : FTS_ILIST_MAX_SIZE;

		node->ilist = static_cast<byte*>(ut_malloc_nokey(new_size));
		node->ilist_size_alloc = new_size;

	} else if ((node->ilist_size + enc_len) > node->ilist_size_alloc) {
		ulint	new_size = node->ilist_size + enc_len;
		byte*	ilist = static_cast<byte*>(ut_malloc_nokey(new_size));

		memcpy(ilist, node->ilist, node->ilist_size);

		ut_free(node->ilist);

		node->ilist = ilist;
		node->ilist_size_alloc = new_size;
	}

	src = enc->src_ilist_ptr;
	dst = node->ilist + node->ilist_size;

	/* Encode the doc id. Cast to ulint, the delta should be small and
	therefore no loss of precision. */
	dst = fts_encode_int(doc_id_delta, dst);

	/* Copy the encoded pos array. */
	memcpy(dst, src, pos_enc_len);

	node->last_doc_id = doc_id;

	/* Data copied upto here. */
	node->ilist_size += enc_len;
	enc->src_ilist_ptr += pos_enc_len;

	ut_a(node->ilist_size <= node->ilist_size_alloc);

	return(error);
}

/**********************************************************************//**
Optimize the data contained in a node.
@return DB_SUCCESS or error code*/
static MY_ATTRIBUTE((nonnull))
dberr_t
fts_optimize_node(
/*==============*/
	ib_vector_t*	del_vec,	/*!< in: vector of doc ids to delete*/
	int*		del_pos,	/*!< in: offset into above vector */
	fts_node_t*	dst_node,	/*!< in: node to fill*/
	fts_node_t*	src_node,	/*!< in: source node for data*/
	fts_encode_t*	enc)		/*!< in: encoding state */
{
	ulint		copied;
	dberr_t		error = DB_SUCCESS;
	doc_id_t	doc_id = enc->src_last_doc_id;

	if (!enc->src_ilist_ptr) {
		enc->src_ilist_ptr = src_node->ilist;
	}

	copied = ulint(enc->src_ilist_ptr - src_node->ilist);

	/* While there is data in the source node and space to copy
	into in the destination node. */
	while (copied < src_node->ilist_size
	       && dst_node->ilist_size < FTS_ILIST_MAX_SIZE) {

		doc_id_t	delta;
		doc_id_t	del_doc_id = FTS_NULL_DOC_ID;

		delta = fts_decode_vlc(
			(const byte**)&enc->src_ilist_ptr);

test_again:
		/* Check whether the doc id is in the delete list, if
		so then we skip the entries but we need to track the
		delta for decoding the entries following this document's
		entries. */
		if (*del_pos >= 0 && *del_pos < (int) ib_vector_size(del_vec)) {
			doc_id_t*	update;

			update = (doc_id_t*) ib_vector_get(
				del_vec, ulint(*del_pos));

			del_doc_id = *update;
		}

		if (enc->src_ilist_ptr == src_node->ilist && doc_id == 0) {
			ut_a(delta == src_node->first_doc_id);
		}

		doc_id += delta;

		if (del_doc_id > 0 && doc_id == del_doc_id) {

			++*del_pos;

			/* Skip the entries for this document. */
			while (*enc->src_ilist_ptr) {
				fts_decode_vlc((const byte**)&enc->src_ilist_ptr);
			}

			/* Skip the end of word position marker. */
			++enc->src_ilist_ptr;

		} else {

			/* DOC ID already becomes larger than
			del_doc_id, check the next del_doc_id */
			if (del_doc_id > 0 && doc_id > del_doc_id) {
				del_doc_id = 0;
				++*del_pos;
				delta = 0;
				goto test_again;
			}

			/* Decode and copy the word positions into
			the dest node. */
			fts_optimize_encode_node(dst_node, doc_id, enc);

			++dst_node->doc_count;

			ut_a(dst_node->last_doc_id == doc_id);
		}

		/* Bytes copied so for from source. */
		copied = ulint(enc->src_ilist_ptr - src_node->ilist);
	}

	if (copied >= src_node->ilist_size) {
		ut_a(doc_id == src_node->last_doc_id);
	}

	enc->src_last_doc_id = doc_id;

	return(error);
}

/**********************************************************************//**
Determine the starting pos within the deleted doc id vector for a word.
@return delete position */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
int
fts_optimize_deleted_pos(
/*=====================*/
	fts_optimize_t*	optim,		/*!< in: optimize state data */
	fts_word_t*	word)		/*!< in: the word data to check */
{
	int		del_pos;
	ib_vector_t*	del_vec = optim->to_delete->doc_ids;

	/* Get the first and last dict ids for the word, we will use
	these values to determine which doc ids need to be removed
	when we coalesce the nodes. This way we can reduce the numer
	of elements that need to be searched in the deleted doc ids
	vector and secondly we can remove the doc ids during the
	coalescing phase. */
	if (ib_vector_size(del_vec) > 0) {
		fts_node_t*	node;
		doc_id_t	last_id;
		doc_id_t	first_id;
		ulint		size = ib_vector_size(word->nodes);

		node = (fts_node_t*) ib_vector_get(word->nodes, 0);
		first_id = node->first_doc_id;

		node = (fts_node_t*) ib_vector_get(word->nodes, size - 1);
		last_id = node->last_doc_id;

		ut_a(first_id <= last_id);

		del_pos = fts_optimize_lookup(
			del_vec, optim->del_pos, first_id, last_id);
	} else {

		del_pos = -1; /* Note that there is nothing to delete. */
	}

	return(del_pos);
}

#define FTS_DEBUG_PRINT
/**********************************************************************//**
Compact the nodes for a word, we also remove any doc ids during the
compaction pass.
@return DB_SUCCESS or error code.*/
static
ib_vector_t*
fts_optimize_word(
/*==============*/
	fts_optimize_t*	optim,		/*!< in: optimize state data */
	fts_word_t*	word)		/*!< in: the word to optimize */
{
	fts_encode_t	enc;
	ib_vector_t*	nodes;
	ulint		i = 0;
	int		del_pos;
	fts_node_t*	dst_node = NULL;
	ib_vector_t*	del_vec = optim->to_delete->doc_ids;
	ulint		size = ib_vector_size(word->nodes);

	del_pos = fts_optimize_deleted_pos(optim, word);
	nodes = ib_vector_create(word->heap_alloc, sizeof(*dst_node), 128);

	enc.src_last_doc_id = 0;
	enc.src_ilist_ptr = NULL;

	while (i < size) {
		ulint		copied;
		fts_node_t*	src_node;

		src_node = (fts_node_t*) ib_vector_get(word->nodes, i);

		if (dst_node == NULL
		    || dst_node->last_doc_id > src_node->first_doc_id) {

			dst_node = static_cast<fts_node_t*>(
				ib_vector_push(nodes, NULL));
			memset(dst_node, 0, sizeof(*dst_node));
		}

		/* Copy from the src to the dst node. */
		fts_optimize_node(del_vec, &del_pos, dst_node, src_node, &enc);

		ut_a(enc.src_ilist_ptr != NULL);

		/* Determine the numer of bytes copied to dst_node. */
		copied = ulint(enc.src_ilist_ptr - src_node->ilist);

		/* Can't copy more than whats in the vlc array. */
		ut_a(copied <= src_node->ilist_size);

		/* We are done with this node release the resources. */
		if (copied == src_node->ilist_size) {

			enc.src_last_doc_id = 0;
			enc.src_ilist_ptr = NULL;

			ut_free(src_node->ilist);

			src_node->ilist = NULL;
			src_node->ilist_size = src_node->ilist_size_alloc = 0;

			src_node = NULL;

			++i; /* Get next source node to OPTIMIZE. */
		}

		if (dst_node->ilist_size >= FTS_ILIST_MAX_SIZE || i >= size) {

			dst_node = NULL;
		}
	}

	/* All dst nodes created should have been added to the vector. */
	ut_a(dst_node == NULL);

	/* Return the OPTIMIZED nodes. */
	return(nodes);
}

/**********************************************************************//**
Update the FTS index table. This is a delete followed by an insert.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_write_word(
/*====================*/
	trx_t*		trx,		/*!< in: transaction */
	fts_table_t*	fts_table,	/*!< in: table of FTS index */
	fts_string_t*	word,		/*!< in: word data to write */
	ib_vector_t*	nodes)		/*!< in: the nodes to write */
{
	ulint		i;
	pars_info_t*	info;
	que_t*		graph;
	ulint		selected;
	dberr_t		error = DB_SUCCESS;
	char		table_name[MAX_FULL_NAME_LEN];

	info = pars_info_create();

	ut_ad(fts_table->charset);

	pars_info_bind_varchar_literal(
		info, "word", word->f_str, word->f_len);

	selected = fts_select_index(fts_table->charset,
				    word->f_str, word->f_len);

	fts_table->suffix = fts_get_suffix(selected);
	fts_get_table_name(fts_table, table_name);
	pars_info_bind_id(info, "table_name", table_name);

	graph = fts_parse_sql(
		fts_table,
		info,
		"BEGIN DELETE FROM $table_name WHERE word = :word;");

	error = fts_eval_sql(trx, graph);

	if (UNIV_UNLIKELY(error != DB_SUCCESS)) {
		ib::error() << "(" << error << ") during optimize,"
			" when deleting a word from the FTS index.";
	}

	que_graph_free(graph);
	graph = NULL;

	/* Even if the operation needs to be rolled back and redone,
	we iterate over the nodes in order to free the ilist. */
	for (i = 0; i < ib_vector_size(nodes); ++i) {

		fts_node_t* node = (fts_node_t*) ib_vector_get(nodes, i);

		if (error == DB_SUCCESS) {
			/* Skip empty node. */
			if (node->ilist == NULL) {
				ut_ad(node->ilist_size == 0);
				continue;
			}

			error = fts_write_node(
				trx, &graph, fts_table, word, node);

			if (UNIV_UNLIKELY(error != DB_SUCCESS)) {
				ib::error() << "(" << error << ")"
					" during optimize, while adding a"
					" word to the FTS index.";
			}
		}

		ut_free(node->ilist);
		node->ilist = NULL;
		node->ilist_size = node->ilist_size_alloc = 0;
	}

	if (graph != NULL) {
		que_graph_free(graph);
	}

	return(error);
}

/**********************************************************************//**
Free fts_optimizer_word_t instanace.*/
void
fts_word_free(
/*==========*/
	fts_word_t*	word)		/*!< in: instance to free.*/
{
	mem_heap_t*	heap = static_cast<mem_heap_t*>(word->heap_alloc->arg);

#ifdef UNIV_DEBUG
	memset(word, 0, sizeof(*word));
#endif /* UNIV_DEBUG */

	mem_heap_free(heap);
}

/**********************************************************************//**
Optimize the word ilist and rewrite data to the FTS index.
@return status one of RESTART, EXIT, ERROR */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_compact(
/*=================*/
	fts_optimize_t*	optim,		/*!< in: optimize state data */
	dict_index_t*	index,		/*!< in: current FTS being optimized */
	time_t		start_time)	/*!< in: optimize start time */
{
	ulint		i;
	dberr_t		error = DB_SUCCESS;
	ulint		size = ib_vector_size(optim->words);

	for (i = 0; i < size && error == DB_SUCCESS && !optim->done; ++i) {
		fts_word_t*	word;
		ib_vector_t*	nodes;
		trx_t*		trx = optim->trx;

		word = (fts_word_t*) ib_vector_get(optim->words, i);

		/* nodes is allocated from the word heap and will be destroyed
		when the word is freed. We however have to be careful about
		the ilist, that needs to be freed explicitly. */
		nodes = fts_optimize_word(optim, word);

		/* Update the data on disk. */
		error = fts_optimize_write_word(
			trx, &optim->fts_index_table, &word->text, nodes);

		if (error == DB_SUCCESS) {
			/* Write the last word optimized to the config table,
			we use this value for restarting optimize. */
			error = fts_config_set_index_value(
				optim->trx, index,
				FTS_LAST_OPTIMIZED_WORD, &word->text);
		}

		/* Free the word that was optimized. */
		fts_word_free(word);

		ulint interval = ulint(time(NULL) - start_time);

		if (fts_optimize_time_limit > 0
		    && (lint(interval) < 0
			|| interval > fts_optimize_time_limit)) {

			optim->done = TRUE;
		}
	}

	return(error);
}

/**********************************************************************//**
Create an instance of fts_optimize_t. Also create a new
background transaction.*/
static
fts_optimize_t*
fts_optimize_create(
/*================*/
	dict_table_t*	table)		/*!< in: table with FTS indexes */
{
	fts_optimize_t*	optim;
	mem_heap_t*	heap = mem_heap_create(128);

	optim = (fts_optimize_t*) mem_heap_zalloc(heap, sizeof(*optim));

	optim->self_heap = ib_heap_allocator_create(heap);

	optim->to_delete = fts_doc_ids_create();

	optim->words = ib_vector_create(
		optim->self_heap, sizeof(fts_word_t), 256);

	optim->table = table;

	optim->trx = trx_create();
	trx_start_internal(optim->trx);

	optim->fts_common_table.table_id = table->id;
	optim->fts_common_table.type = FTS_COMMON_TABLE;
	optim->fts_common_table.table = table;

	optim->fts_index_table.table_id = table->id;
	optim->fts_index_table.type = FTS_INDEX_TABLE;
	optim->fts_index_table.table = table;

	/* The common prefix for all this parent table's aux tables. */
	char table_id[FTS_AUX_MIN_TABLE_ID_LENGTH];
	const size_t table_id_len = 1
		+ size_t(fts_get_table_id(&optim->fts_common_table, table_id));
	dict_sys.freeze(SRW_LOCK_CALL);
	/* Include the separator as well. */
	const size_t dbname_len = table->name.dblen() + 1;
	ut_ad(dbname_len > 1);
	const size_t prefix_name_len = dbname_len + 4 + table_id_len;
	char* prefix_name = static_cast<char*>(
		ut_malloc_nokey(prefix_name_len));
	memcpy(prefix_name, table->name.m_name, dbname_len);
	dict_sys.unfreeze();
	memcpy(prefix_name + dbname_len, "FTS_", 4);
	memcpy(prefix_name + dbname_len + 4, table_id, table_id_len);
	optim->name_prefix =prefix_name;

	return(optim);
}

#ifdef FTS_OPTIMIZE_DEBUG
/**********************************************************************//**
Get optimize start time of an FTS index.
@return DB_SUCCESS if all OK else error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_get_index_start_time(
/*==============================*/
	trx_t*		trx,			/*!< in: transaction */
	dict_index_t*	index,			/*!< in: FTS index */
	time_t*		start_time)		/*!< out: time in secs */
{
	return(fts_config_get_index_ulint(
		       trx, index, FTS_OPTIMIZE_START_TIME,
		       (ulint*) start_time));
}

/**********************************************************************//**
Set the optimize start time of an FTS index.
@return DB_SUCCESS if all OK else error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_set_index_start_time(
/*==============================*/
	trx_t*		trx,			/*!< in: transaction */
	dict_index_t*	index,			/*!< in: FTS index */
	time_t		start_time)		/*!< in: start time */
{
	return(fts_config_set_index_ulint(
		       trx, index, FTS_OPTIMIZE_START_TIME,
		       (ulint) start_time));
}

/**********************************************************************//**
Get optimize end time of an FTS index.
@return DB_SUCCESS if all OK else error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_get_index_end_time(
/*============================*/
	trx_t*		trx,			/*!< in: transaction */
	dict_index_t*	index,			/*!< in: FTS index */
	time_t*		end_time)		/*!< out: time in secs */
{
	return(fts_config_get_index_ulint(
		       trx, index, FTS_OPTIMIZE_END_TIME, (ulint*) end_time));
}

/**********************************************************************//**
Set the optimize end time of an FTS index.
@return DB_SUCCESS if all OK else error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_set_index_end_time(
/*============================*/
	trx_t*		trx,			/*!< in: transaction */
	dict_index_t*	index,			/*!< in: FTS index */
	time_t		end_time)		/*!< in: end time */
{
	return(fts_config_set_index_ulint(
		       trx, index, FTS_OPTIMIZE_END_TIME, (ulint) end_time));
}
#endif

/**********************************************************************//**
Free the optimize prepared statements.*/
static
void
fts_optimize_graph_free(
/*====================*/
	fts_optimize_graph_t*	graph)	/*!< in/out: The graph instances
					to free */
{
	if (graph->commit_graph) {
		que_graph_free(graph->commit_graph);
		graph->commit_graph = NULL;
	}

	if (graph->write_nodes_graph) {
		que_graph_free(graph->write_nodes_graph);
		graph->write_nodes_graph = NULL;
	}

	if (graph->delete_nodes_graph) {
		que_graph_free(graph->delete_nodes_graph);
		graph->delete_nodes_graph = NULL;
	}

	if (graph->read_nodes_graph) {
		que_graph_free(graph->read_nodes_graph);
		graph->read_nodes_graph = NULL;
	}
}

/**********************************************************************//**
Free all optimize resources. */
static
void
fts_optimize_free(
/*==============*/
	fts_optimize_t*	optim)		/*!< in: table with on FTS index */
{
	mem_heap_t*	heap = static_cast<mem_heap_t*>(optim->self_heap->arg);

	trx_commit_for_mysql(optim->trx);
	optim->trx->free();
	optim->trx = NULL;

	fts_doc_ids_free(optim->to_delete);
	fts_optimize_graph_free(&optim->graph);

	ut_free(optim->name_prefix);

	/* This will free the heap from which optim itself was allocated. */
	mem_heap_free(heap);
}

/**********************************************************************//**
Get the max time optimize should run in millisecs.
@return max optimize time limit in millisecs. */
static
ulint
fts_optimize_get_time_limit(
/*========================*/
	trx_t*		trx,			/*!< in: transaction */
	fts_table_t*	fts_table)		/*!< in: aux table */
{
	ulint	time_limit = 0;

	fts_config_get_ulint(
		trx, fts_table,
		FTS_OPTIMIZE_LIMIT_IN_SECS, &time_limit);

	/* FIXME: This is returning milliseconds, while the variable
	is being stored and interpreted as seconds! */
	return(time_limit * 1000);
}

/**********************************************************************//**
Run OPTIMIZE on the given table. Note: this can take a very long time
(hours). */
static
void
fts_optimize_words(
/*===============*/
	fts_optimize_t*	optim,	/*!< in: optimize instance */
	dict_index_t*	index,	/*!< in: current FTS being optimized */
	fts_string_t*	word)	/*!< in: the starting word to optimize */
{
	fts_fetch_t	fetch;
	que_t*		graph = NULL;
	CHARSET_INFO*	charset = optim->fts_index_table.charset;

	ut_a(!optim->done);

	/* Get the time limit from the config table. */
	fts_optimize_time_limit = fts_optimize_get_time_limit(
		optim->trx, &optim->fts_common_table);

	const time_t start_time = time(NULL);

	/* Setup the callback to use for fetching the word ilist etc. */
	fetch.read_arg = optim->words;
	fetch.read_record = fts_optimize_index_fetch_node;

	while (!optim->done) {
		dberr_t	error;
		trx_t*	trx = optim->trx;
		ulint	selected;

		ut_a(ib_vector_size(optim->words) == 0);

		selected = fts_select_index(charset, word->f_str, word->f_len);

		/* Read the index records to optimize. */
		fetch.total_memory = 0;
		error = fts_index_fetch_nodes(
			trx, &graph, &optim->fts_index_table, word,
			&fetch);
		ut_ad(fetch.total_memory < fts_result_cache_limit);

		if (error == DB_SUCCESS) {
			/* There must be some nodes to read. */
			ut_a(ib_vector_size(optim->words) > 0);

			/* Optimize the nodes that were read and write
			back to DB. */
			error = fts_optimize_compact(optim, index, start_time);

			if (error == DB_SUCCESS) {
				fts_sql_commit(optim->trx);
			} else {
				fts_sql_rollback(optim->trx);
			}
		}

		ib_vector_reset(optim->words);

		if (error == DB_SUCCESS) {
			if (!optim->done) {
				if (!fts_zip_read_word(optim->zip, word)) {
					optim->done = TRUE;
				} else if (selected
					   != fts_select_index(
						charset, word->f_str,
						word->f_len)
					  && graph) {
					que_graph_free(graph);
					graph = NULL;
				}
			}
		} else if (error == DB_LOCK_WAIT_TIMEOUT) {
			ib::warn() << "Lock wait timeout during optimize."
				" Retrying!";

			trx->error_state = DB_SUCCESS;
		} else if (error == DB_DEADLOCK) {
			ib::warn() << "Deadlock during optimize. Retrying!";

			trx->error_state = DB_SUCCESS;
		} else {
			optim->done = TRUE;		/* Exit the loop. */
		}
	}

	if (graph != NULL) {
		que_graph_free(graph);
	}
}

/**********************************************************************//**
Optimize is complete. Set the completion time, and reset the optimize
start string for this FTS index to "".
@return DB_SUCCESS if all OK */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_index_completed(
/*=========================*/
	fts_optimize_t*	optim,	/*!< in: optimize instance */
	dict_index_t*	index)	/*!< in: table with one FTS index */
{
	fts_string_t	word;
	dberr_t		error;
	byte		buf[sizeof(ulint)];
#ifdef FTS_OPTIMIZE_DEBUG
	time_t		end_time = time(NULL);

	error = fts_optimize_set_index_end_time(optim->trx, index, end_time);
#endif

	/* If we've reached the end of the index then set the start
	word to the empty string. */

	word.f_len = 0;
	word.f_str = buf;
	*word.f_str = '\0';

	error = fts_config_set_index_value(
		optim->trx, index, FTS_LAST_OPTIMIZED_WORD, &word);

	if (UNIV_UNLIKELY(error != DB_SUCCESS)) {
		ib::error() << "(" << error << ") while updating"
			" last optimized word!";
	}

	return(error);
}


/**********************************************************************//**
Read the list of words from the FTS auxiliary index that will be
optimized in this pass.
@return DB_SUCCESS if all OK */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_index_read_words(
/*==========================*/
	fts_optimize_t*	optim,	/*!< in: optimize instance */
	dict_index_t*	index,	/*!< in: table with one FTS index */
	fts_string_t*	word)	/*!< in: buffer to use */
{
	dberr_t	error = DB_SUCCESS;

	if (optim->del_list_regenerated) {
		word->f_len = 0;
	} else {

		/* Get the last word that was optimized from
		the config table. */
		error = fts_config_get_index_value(
			optim->trx, index, FTS_LAST_OPTIMIZED_WORD, word);
	}

	/* If record not found then we start from the top. */
	if (error == DB_RECORD_NOT_FOUND) {
		word->f_len = 0;
		error = DB_SUCCESS;
	}

	while (error == DB_SUCCESS) {

		error = fts_index_fetch_words(
			optim, word, fts_num_word_optimize);

		if (error == DB_SUCCESS) {
			/* Reset the last optimized word to '' if no
			more words could be read from the FTS index. */
			if (optim->zip->n_words == 0) {
				word->f_len = 0;
				*word->f_str = 0;
			}

			break;
		}
	}

	return(error);
}

/**********************************************************************//**
Run OPTIMIZE on the given FTS index. Note: this can take a very long
time (hours).
@return DB_SUCCESS if all OK */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_index(
/*===============*/
	fts_optimize_t*	optim,	/*!< in: optimize instance */
	dict_index_t*	index)	/*!< in: table with one FTS index */
{
	fts_string_t	word;
	dberr_t		error;
	byte		str[FTS_MAX_WORD_LEN + 1];

	/* Set the current index that we have to optimize. */
	optim->fts_index_table.index_id = index->id;
	optim->fts_index_table.charset = fts_index_get_charset(index);

	optim->done = FALSE; /* Optimize until !done */

	/* We need to read the last word optimized so that we start from
	the next word. */
	word.f_str = str;

	/* We set the length of word to the size of str since we
	need to pass the max len info to the fts_get_config_value() function. */
	word.f_len = sizeof(str) - 1;

	memset(word.f_str, 0x0, word.f_len);

	/* Read the words that will be optimized in this pass. */
	error = fts_optimize_index_read_words(optim, index, &word);

	if (error == DB_SUCCESS) {
		int	zip_error;

		ut_a(optim->zip->pos == 0);
		ut_a(optim->zip->zp->total_in == 0);
		ut_a(optim->zip->zp->total_out == 0);

		zip_error = inflateInit(optim->zip->zp);
		ut_a(zip_error == Z_OK);

		word.f_len = 0;
		word.f_str = str;

		/* Read the first word to optimize from the Zip buffer. */
		if (!fts_zip_read_word(optim->zip, &word)) {

			optim->done = TRUE;
		} else {
			fts_optimize_words(optim, index, &word);
		}

		/* If we couldn't read any records then optimize is
		complete. Increment the number of indexes that have
		been optimized and set FTS index optimize state to
		completed. */
		if (error == DB_SUCCESS && optim->zip->n_words == 0) {

			error = fts_optimize_index_completed(optim, index);

			if (error == DB_SUCCESS) {
				++optim->n_completed;
			}
		}
	}

	return(error);
}

/**********************************************************************//**
Delete the document ids in the delete, and delete cache tables.
@return DB_SUCCESS if all OK */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_purge_deleted_doc_ids(
/*===============================*/
	fts_optimize_t*	optim)	/*!< in: optimize instance */
{
	ulint		i;
	pars_info_t*	info;
	que_t*		graph;
	doc_id_t*	update;
	doc_id_t	write_doc_id;
	dberr_t		error = DB_SUCCESS;
	char		deleted[MAX_FULL_NAME_LEN];
	char		deleted_cache[MAX_FULL_NAME_LEN];

	info = pars_info_create();

	ut_a(ib_vector_size(optim->to_delete->doc_ids) > 0);

	update = static_cast<doc_id_t*>(
		ib_vector_get(optim->to_delete->doc_ids, 0));

	/* Convert to "storage" byte order. */
	fts_write_doc_id((byte*) &write_doc_id, *update);

	/* This is required for the SQL parser to work. It must be able
	to find the following variables. So we do it twice. */
	fts_bind_doc_id(info, "doc_id1", &write_doc_id);
	fts_bind_doc_id(info, "doc_id2", &write_doc_id);

	/* Make sure the following two names are consistent with the name
	used in the fts_delete_doc_ids_sql */
	optim->fts_common_table.suffix = fts_common_tables[3];
	fts_get_table_name(&optim->fts_common_table, deleted);
	pars_info_bind_id(info, fts_common_tables[3], deleted);

	optim->fts_common_table.suffix = fts_common_tables[4];
	fts_get_table_name(&optim->fts_common_table, deleted_cache);
	pars_info_bind_id(info, fts_common_tables[4], deleted_cache);

	graph = fts_parse_sql(NULL, info, fts_delete_doc_ids_sql);

	/* Delete the doc ids that were copied at the start. */
	for (i = 0; i < ib_vector_size(optim->to_delete->doc_ids); ++i) {

		update = static_cast<doc_id_t*>(ib_vector_get(
			optim->to_delete->doc_ids, i));

		/* Convert to "storage" byte order. */
		fts_write_doc_id((byte*) &write_doc_id, *update);

		fts_bind_doc_id(info, "doc_id1", &write_doc_id);

		fts_bind_doc_id(info, "doc_id2", &write_doc_id);

		error = fts_eval_sql(optim->trx, graph);

		// FIXME: Check whether delete actually succeeded!
		if (error != DB_SUCCESS) {

			fts_sql_rollback(optim->trx);
			break;
		}
	}

	que_graph_free(graph);

	return(error);
}

/**********************************************************************//**
Delete the document ids in the pending delete, and delete tables.
@return DB_SUCCESS if all OK */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_purge_deleted_doc_id_snapshot(
/*=======================================*/
	fts_optimize_t*	optim)	/*!< in: optimize instance */
{
	dberr_t		error;
	que_t*		graph;
	pars_info_t*	info;
	char		being_deleted[MAX_FULL_NAME_LEN];
	char		being_deleted_cache[MAX_FULL_NAME_LEN];

	info = pars_info_create();

	/* Make sure the following two names are consistent with the name
	used in the fts_end_delete_sql */
	optim->fts_common_table.suffix = fts_common_tables[0];
	fts_get_table_name(&optim->fts_common_table, being_deleted);
	pars_info_bind_id(info, fts_common_tables[0], being_deleted);

	optim->fts_common_table.suffix = fts_common_tables[1];
	fts_get_table_name(&optim->fts_common_table, being_deleted_cache);
	pars_info_bind_id(info, fts_common_tables[1], being_deleted_cache);

	/* Delete the doc ids that were copied to delete pending state at
	the start of optimize. */
	graph = fts_parse_sql(NULL, info, fts_end_delete_sql);

	error = fts_eval_sql(optim->trx, graph);
	que_graph_free(graph);

	return(error);
}

/**********************************************************************//**
Copy the deleted doc ids that will be purged during this optimize run
to the being deleted FTS auxiliary tables. The transaction is committed
upon successfull copy and rolled back on DB_DUPLICATE_KEY error.
@return DB_SUCCESS if all OK */
static
ulint
fts_optimize_being_deleted_count(
/*=============================*/
	fts_optimize_t*	optim)	/*!< in: optimize instance */
{
	fts_table_t	fts_table;

	FTS_INIT_FTS_TABLE(&fts_table, "BEING_DELETED", FTS_COMMON_TABLE,
			   optim->table);

	return(fts_get_rows_count(&fts_table));
}

/*********************************************************************//**
Copy the deleted doc ids that will be purged during this optimize run
to the being deleted FTS auxiliary tables. The transaction is committed
upon successfull copy and rolled back on DB_DUPLICATE_KEY error.
@return DB_SUCCESS if all OK */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_create_deleted_doc_id_snapshot(
/*========================================*/
	fts_optimize_t*	optim)	/*!< in: optimize instance */
{
	dberr_t		error;
	que_t*		graph;
	pars_info_t*	info;
	char		being_deleted[MAX_FULL_NAME_LEN];
	char		deleted[MAX_FULL_NAME_LEN];
	char		being_deleted_cache[MAX_FULL_NAME_LEN];
	char		deleted_cache[MAX_FULL_NAME_LEN];

	info = pars_info_create();

	/* Make sure the following four names are consistent with the name
	used in the fts_init_delete_sql */
	optim->fts_common_table.suffix = fts_common_tables[0];
	fts_get_table_name(&optim->fts_common_table, being_deleted);
	pars_info_bind_id(info, fts_common_tables[0], being_deleted);

	optim->fts_common_table.suffix = fts_common_tables[3];
	fts_get_table_name(&optim->fts_common_table, deleted);
	pars_info_bind_id(info, fts_common_tables[3], deleted);

	optim->fts_common_table.suffix = fts_common_tables[1];
	fts_get_table_name(&optim->fts_common_table, being_deleted_cache);
	pars_info_bind_id(info, fts_common_tables[1], being_deleted_cache);

	optim->fts_common_table.suffix = fts_common_tables[4];
	fts_get_table_name(&optim->fts_common_table, deleted_cache);
	pars_info_bind_id(info, fts_common_tables[4], deleted_cache);

	/* Move doc_ids that are to be deleted to state being deleted. */
	graph = fts_parse_sql(NULL, info, fts_init_delete_sql);

	error = fts_eval_sql(optim->trx, graph);

	que_graph_free(graph);

	if (error != DB_SUCCESS) {
		fts_sql_rollback(optim->trx);
	} else {
		fts_sql_commit(optim->trx);
	}

	optim->del_list_regenerated = TRUE;

	return(error);
}

/*********************************************************************//**
Read in the document ids that are to be purged during optimize. The
transaction is committed upon successfully read.
@return DB_SUCCESS if all OK */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_read_deleted_doc_id_snapshot(
/*======================================*/
	fts_optimize_t*	optim)	/*!< in: optimize instance */
{
	dberr_t		error;

	optim->fts_common_table.suffix = "BEING_DELETED";

	/* Read the doc_ids to delete. */
	error = fts_table_fetch_doc_ids(
		optim->trx, &optim->fts_common_table, optim->to_delete);

	if (error == DB_SUCCESS) {

		optim->fts_common_table.suffix = "BEING_DELETED_CACHE";

		/* Read additional doc_ids to delete. */
		error = fts_table_fetch_doc_ids(
			optim->trx, &optim->fts_common_table, optim->to_delete);
	}

	if (error != DB_SUCCESS) {

		fts_doc_ids_free(optim->to_delete);
		optim->to_delete = NULL;
	}

	return(error);
}

/*********************************************************************//**
Optimize all the FTS indexes, skipping those that have already been
optimized, since the FTS auxiliary indexes are not guaranteed to be
of the same cardinality.
@return DB_SUCCESS if all OK */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_indexes(
/*=================*/
	fts_optimize_t*	optim)	/*!< in: optimize instance */
{
	ulint		i;
	dberr_t		error = DB_SUCCESS;
	fts_t*		fts = optim->table->fts;

	/* Optimize the FTS indexes. */
	for (i = 0; i < ib_vector_size(fts->indexes); ++i) {
		dict_index_t*	index;

#ifdef	FTS_OPTIMIZE_DEBUG
		time_t	end_time;
		time_t	start_time;

		/* Get the start and end optimize times for this index. */
		error = fts_optimize_get_index_start_time(
			optim->trx, index, &start_time);

		if (error != DB_SUCCESS) {
			break;
		}

		error = fts_optimize_get_index_end_time(
			optim->trx, index, &end_time);

		if (error != DB_SUCCESS) {
			break;
		}

		/* Start time will be 0 only for the first time or after
		completing the optimization of all FTS indexes. */
		if (start_time == 0) {
			start_time = time(NULL);

			error = fts_optimize_set_index_start_time(
				optim->trx, index, start_time);
		}

		/* Check if this index needs to be optimized or not. */
		if (difftime(end_time, start_time) < 0) {
			error = fts_optimize_index(optim, index);

			if (error != DB_SUCCESS) {
				break;
			}
		} else {
			++optim->n_completed;
		}
#endif
		index = static_cast<dict_index_t*>(
			ib_vector_getp(fts->indexes, i));
		error = fts_optimize_index(optim, index);
	}

	if (error == DB_SUCCESS) {
		fts_sql_commit(optim->trx);
	} else {
		fts_sql_rollback(optim->trx);
	}

	return(error);
}

/*********************************************************************//**
Cleanup the snapshot tables and the master deleted table.
@return DB_SUCCESS if all OK */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_purge_snapshot(
/*========================*/
	fts_optimize_t*	optim)	/*!< in: optimize instance */
{
	dberr_t		error;

	/* Delete the doc ids from the master deleted tables, that were
	in the snapshot that was taken at the start of optimize. */
	error = fts_optimize_purge_deleted_doc_ids(optim);

	if (error == DB_SUCCESS) {
		/* Destroy the deleted doc id snapshot. */
		error = fts_optimize_purge_deleted_doc_id_snapshot(optim);
	}

	if (error == DB_SUCCESS) {
		fts_sql_commit(optim->trx);
	} else {
		fts_sql_rollback(optim->trx);
	}

	return(error);
}

/*********************************************************************//**
Reset the start time to 0 so that a new optimize can be started.
@return DB_SUCCESS if all OK */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_optimize_reset_start_time(
/*==========================*/
	fts_optimize_t*	optim)	/*!< in: optimize instance */
{
	dberr_t		error = DB_SUCCESS;
#ifdef FTS_OPTIMIZE_DEBUG
	fts_t*		fts = optim->table->fts;

	/* Optimization should have been completed for all indexes. */
	ut_a(optim->n_completed == ib_vector_size(fts->indexes));

	for (uint i = 0; i < ib_vector_size(fts->indexes); ++i) {
		dict_index_t*	index;

		time_t	start_time = 0;

		/* Reset the start time to 0 for this index. */
		error = fts_optimize_set_index_start_time(
			optim->trx, index, start_time);

		index = static_cast<dict_index_t*>(
			ib_vector_getp(fts->indexes, i));
	}
#endif

	if (error == DB_SUCCESS) {
		fts_sql_commit(optim->trx);
	} else {
		fts_sql_rollback(optim->trx);
	}

	return(error);
}

/*********************************************************************//**
Run OPTIMIZE on the given table by a background thread.
@return DB_SUCCESS if all OK */
static MY_ATTRIBUTE((nonnull))
dberr_t
fts_optimize_table_bk(
/*==================*/
	fts_slot_t*	slot)	/*!< in: table to optimiza */
{
	const time_t now = time(NULL);
	const ulint interval = ulint(now - slot->last_run);

	/* Avoid optimizing tables that were optimized recently. */
	if (slot->last_run > 0
	    && lint(interval) >= 0
	    && interval < FTS_OPTIMIZE_INTERVAL_IN_SECS) {

		return(DB_SUCCESS);
	}

	dict_table_t*	table = slot->table;
	dberr_t		error;

	if (table->is_accessible()
	    && table->fts && table->fts->cache
	    && table->fts->cache->deleted >= FTS_OPTIMIZE_THRESHOLD) {
		error = fts_optimize_table(table);

		slot->last_run = time(NULL);

		if (error == DB_SUCCESS) {
			slot->running = false;
			slot->completed = slot->last_run;
		}
	} else {
		/* Note time this run completed. */
		slot->last_run = now;
		error = DB_SUCCESS;
	}

	return(error);
}
/*********************************************************************//**
Run OPTIMIZE on the given table.
@return DB_SUCCESS if all OK */
dberr_t
fts_optimize_table(
/*===============*/
	dict_table_t*	table)	/*!< in: table to optimiza */
{
	if (srv_read_only_mode) {
		return DB_READ_ONLY;
	}

	dberr_t		error = DB_SUCCESS;
	fts_optimize_t*	optim = NULL;
	fts_t*		fts = table->fts;

	if (UNIV_UNLIKELY(fts_enable_diag_print)) {
		ib::info() << "FTS start optimize " << table->name;
	}

	optim = fts_optimize_create(table);

	// FIXME: Call this only at the start of optimize, currently we
	// rely on DB_DUPLICATE_KEY to handle corrupting the snapshot.

	/* Check whether there are still records in BEING_DELETED table */
	if (fts_optimize_being_deleted_count(optim) == 0) {
		/* Take a snapshot of the deleted document ids, they are copied
		to the BEING_ tables. */
		error = fts_optimize_create_deleted_doc_id_snapshot(optim);
	}

	/* A duplicate error is OK, since we don't erase the
	doc ids from the being deleted state until all FTS
	indexes have been optimized. */
	if (error == DB_DUPLICATE_KEY) {
		error = DB_SUCCESS;
	}

	if (error == DB_SUCCESS) {

		/* These document ids will be filtered out during the
		index optimization phase. They are in the snapshot that we
		took above, at the start of the optimize. */
		error = fts_optimize_read_deleted_doc_id_snapshot(optim);

		if (error == DB_SUCCESS) {

			/* Commit the read of being deleted
			doc ids transaction. */
			fts_sql_commit(optim->trx);

			/* We would do optimization only if there
			are deleted records to be cleaned up */
			if (ib_vector_size(optim->to_delete->doc_ids) > 0) {
				error = fts_optimize_indexes(optim);
			}

		} else {
			ut_a(optim->to_delete == NULL);
		}

		/* Only after all indexes have been optimized can we
		delete the (snapshot) doc ids in the pending delete,
		and master deleted tables. */
		if (error == DB_SUCCESS
		    && optim->n_completed == ib_vector_size(fts->indexes)) {

			if (UNIV_UNLIKELY(fts_enable_diag_print)) {
				ib::info() << "FTS_OPTIMIZE: Completed"
					" Optimize, cleanup DELETED table";
			}

			if (ib_vector_size(optim->to_delete->doc_ids) > 0) {

				/* Purge the doc ids that were in the
				snapshot from the snapshot tables and
				the master deleted table. */
				error = fts_optimize_purge_snapshot(optim);
			}

			if (error == DB_SUCCESS) {
				/* Reset the start time of all the FTS indexes
				so that optimize can be restarted. */
				error = fts_optimize_reset_start_time(optim);
			}
		}
	}

	fts_optimize_free(optim);

	if (UNIV_UNLIKELY(fts_enable_diag_print)) {
		ib::info() << "FTS end optimize " << table->name;
	}

	return(error);
}

/********************************************************************//**
Add the table to add to the OPTIMIZER's list.
@return new message instance */
static
fts_msg_t*
fts_optimize_create_msg(
/*====================*/
	fts_msg_type_t	type,		/*!< in: type of message */
	void*		ptr)		/*!< in: message payload */
{
	mem_heap_t*	heap;
	fts_msg_t*	msg;

	heap = mem_heap_create(sizeof(*msg) + sizeof(ib_list_node_t) + 16);
	msg = static_cast<fts_msg_t*>(mem_heap_alloc(heap, sizeof(*msg)));

	msg->ptr = ptr;
	msg->type = type;
	msg->heap = heap;

	return(msg);
}

/** Add message to wqueue, signal thread pool*/
static void add_msg(fts_msg_t *msg)
{
  ib_wqueue_add(fts_optimize_wq, msg, msg->heap, true);
  srv_thread_pool->submit_task(&task);
}

/**
Called by "idle" timer. Submits optimize task, which
will only recalculate is_sync_needed, in case the queue is empty.
*/
static void timer_callback(void*)
{
  srv_thread_pool->submit_task(&task);
}

/** Add the table to add to the OPTIMIZER's list.
@param[in]	table	table to add */
void fts_optimize_add_table(dict_table_t* table)
{
	fts_msg_t*	msg;

	if (!fts_optimize_wq) {
		return;
	}

	/* Make sure table with FTS index cannot be evicted */
	dict_sys.prevent_eviction(table);

	msg = fts_optimize_create_msg(FTS_MSG_ADD_TABLE, table);

	mysql_mutex_lock(&fts_optimize_wq->mutex);

	add_msg(msg);

	table->fts->in_queue = true;

	mysql_mutex_unlock(&fts_optimize_wq->mutex);
}

/**********************************************************************//**
Remove the table from the OPTIMIZER's list. We do wait for
acknowledgement from the consumer of the message. */
void
fts_optimize_remove_table(
/*======================*/
	dict_table_t*	table)			/*!< in: table to remove */
{
  if (!fts_optimize_wq)
    return;

  if (fts_opt_start_shutdown)
  {
    ib::info() << "Try to remove table " << table->name
               << " after FTS optimize thread exiting.";
    while (fts_optimize_wq)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return;
  }

  mysql_mutex_lock(&fts_optimize_wq->mutex);

  if (table->fts->in_queue)
  {
    fts_msg_t *msg= fts_optimize_create_msg(FTS_MSG_DEL_TABLE, nullptr);
    pthread_cond_t cond;
    pthread_cond_init(&cond, nullptr);
    msg->ptr= new(mem_heap_alloc(msg->heap, sizeof(fts_msg_del_t)))
      fts_msg_del_t{table, &cond};
    add_msg(msg);
    my_cond_wait(&cond, &fts_optimize_wq->mutex.m_mutex);
    pthread_cond_destroy(&cond);
    ut_ad(!table->fts->in_queue);
  }

  mysql_mutex_unlock(&fts_optimize_wq->mutex);
}

/** Send sync fts cache for the table.
@param[in]	table	table to sync */
void
fts_optimize_request_sync_table(
	dict_table_t*	table)
{
	/* if the optimize system not yet initialized, return */
	if (!fts_optimize_wq) {
		return;
	}

	mysql_mutex_lock(&fts_optimize_wq->mutex);

	/* FTS optimizer thread is already exited */
	if (fts_opt_start_shutdown) {
		ib::info() << "Try to sync table " << table->name
			<< " after FTS optimize thread exiting.";
	} else if (table->fts->sync_message) {
		/* If the table already has SYNC message in
		fts_optimize_wq queue then ignore it */
	} else {
		add_msg(fts_optimize_create_msg(FTS_MSG_SYNC_TABLE, table));
		table->fts->sync_message = true;
		DBUG_EXECUTE_IF("fts_optimize_wq_count_check",
				DBUG_ASSERT(fts_optimize_wq->length <= 1000););
	}

	mysql_mutex_unlock(&fts_optimize_wq->mutex);
}

/** Add a table to fts_slots if it doesn't already exist. */
static bool fts_optimize_new_table(dict_table_t* table)
{
	ut_ad(table);

	ulint		i;
	fts_slot_t*	slot;
	fts_slot_t*	empty = NULL;

	/* Search for duplicates, also find a free slot if one exists. */
	for (i = 0; i < ib_vector_size(fts_slots); ++i) {

		slot = static_cast<fts_slot_t*>(ib_vector_get(fts_slots, i));

		if (!slot->table) {
			empty = slot;
		} else if (slot->table == table) {
			/* Already exists in our optimize queue. */
			return false;
		}
	}

	slot = empty ? empty : static_cast<fts_slot_t*>(
		ib_vector_push(fts_slots, NULL));

	memset(slot, 0x0, sizeof(*slot));

	slot->table = table;
	return true;
}

/** Remove a table from fts_slots if it exists.
@param remove	table to be removed from fts_slots */
static bool fts_optimize_del_table(fts_msg_del_t *remove)
{
	const dict_table_t* table = remove->table;
	ut_ad(table);
	for (ulint i = 0; i < ib_vector_size(fts_slots); ++i) {
		fts_slot_t*	slot;

		slot = static_cast<fts_slot_t*>(ib_vector_get(fts_slots, i));

		if (slot->table == table) {
			if (UNIV_UNLIKELY(fts_enable_diag_print)) {
				ib::info() << "FTS Optimize Removing table "
					<< table->name;
			}

			mysql_mutex_lock(&fts_optimize_wq->mutex);
			table->fts->in_queue = false;
			pthread_cond_signal(remove->cond);
			mysql_mutex_unlock(&fts_optimize_wq->mutex);
			slot->table = NULL;
			return true;
		}
	}

	mysql_mutex_lock(&fts_optimize_wq->mutex);
	pthread_cond_signal(remove->cond);
	mysql_mutex_unlock(&fts_optimize_wq->mutex);
	return false;
}

/**********************************************************************//**
Calculate how many tables in fts_slots need to be optimized.
@return no. of tables to optimize */
static ulint fts_optimize_how_many()
{
	ulint n_tables = 0;
	const time_t current_time = time(NULL);

	for (ulint i = 0; i < ib_vector_size(fts_slots); ++i) {
		const fts_slot_t* slot = static_cast<const fts_slot_t*>(
			ib_vector_get_const(fts_slots, i));
		if (!slot->table) {
			continue;
		}

		const time_t end = slot->running
			? slot->last_run : slot->completed;
		ulint interval = ulint(current_time - end);

		if (lint(interval) < 0
		    || interval >= FTS_OPTIMIZE_INTERVAL_IN_SECS) {
			++n_tables;
		}
	}

	return(n_tables);
}

/**********************************************************************//**
Check if the total memory used by all FTS table exceeds the maximum limit.
@return true if a sync is needed, false otherwise */
static bool fts_is_sync_needed()
{
	ulint		total_memory = 0;
	const time_t	now = time(NULL);
	double		time_diff = difftime(now, last_check_sync_time);

	if (fts_need_sync || (time_diff >= 0 && time_diff < 5)) {
		return(false);
	}

	last_check_sync_time = now;

	for (ulint i = 0; i < ib_vector_size(fts_slots); ++i) {
		const fts_slot_t* slot = static_cast<const fts_slot_t*>(
			ib_vector_get_const(fts_slots, i));

		if (!slot->table) {
			continue;
		}

		if (slot->table->fts && slot->table->fts->cache) {
			total_memory += slot->table->fts->cache->total_size;
		}

		if (total_memory > fts_max_total_cache_size) {
			return(true);
		}
	}

	return(false);
}

/** Sync fts cache of a table
@param[in,out]  table           table to be synced
@param[in]      process_message processing messages from fts_optimize_wq */
static void fts_optimize_sync_table(dict_table_t *table,
                                    bool process_message= false)
{
  MDL_ticket* mdl_ticket= nullptr;
  dict_table_t *sync_table= dict_acquire_mdl_shared<true>(table, fts_opt_thd,
                                                          &mdl_ticket);

  if (!sync_table)
    return;

  if (sync_table->fts && sync_table->fts->cache && sync_table->is_accessible())
  {
    fts_sync_table(sync_table, false);
    if (process_message)
    {
      mysql_mutex_lock(&fts_optimize_wq->mutex);
      sync_table->fts->sync_message = false;
      mysql_mutex_unlock(&fts_optimize_wq->mutex);
    }
  }

  DBUG_EXECUTE_IF("ib_optimize_wq_hang",
		  std::this_thread::sleep_for(std::chrono::seconds(6)););

  if (mdl_ticket)
    dict_table_close(sync_table, fts_opt_thd, mdl_ticket);
}

/**********************************************************************//**
Optimize all FTS tables.
@return Dummy return */
static void fts_optimize_callback(void *)
{
	ut_ad(!srv_read_only_mode);

	static ulint	current;
	static bool	done;
	static ulint	n_optimize;

	if (!fts_optimize_wq || done) {
		/* Possibly timer initiated callback, can come after FTS_MSG_STOP.*/
		return;
	}

	static ulint		n_tables = ib_vector_size(fts_slots);

	while (!done && srv_shutdown_state <= SRV_SHUTDOWN_INITIATED) {
		/* If there is no message in the queue and we have tables
		to optimize then optimize the tables. */

		if (!done
		    && ib_wqueue_is_empty(fts_optimize_wq)
		    && n_tables > 0
		    && n_optimize > 0) {

			/* The queue is empty but we have tables
			to optimize. */
			if (UNIV_UNLIKELY(wsrep_sst_disable_writes)) {
retry_later:
				if (fts_is_sync_needed()) {
					fts_need_sync = true;
				}
				if (n_tables) {
					timer->set_time(5000, 0);
				}
				return;
			}

			fts_slot_t* slot = static_cast<fts_slot_t*>(
				ib_vector_get(fts_slots, current));

			/* Handle the case of empty slots. */
			if (slot->table) {
				slot->running = true;
				fts_optimize_table_bk(slot);
			}

			/* Wrap around the counter. */
			if (++current >= ib_vector_size(fts_slots)) {
				n_optimize = fts_optimize_how_many();
				current = 0;
			}
		} else if (n_optimize == 0
			   || !ib_wqueue_is_empty(fts_optimize_wq)) {
			fts_msg_t* msg = static_cast<fts_msg_t*>
				(ib_wqueue_nowait(fts_optimize_wq));
			/* Timeout ? */
			if (!msg) {
				goto retry_later;
			}

			switch (msg->type) {
			case FTS_MSG_STOP:
				done = true;
				break;

			case FTS_MSG_ADD_TABLE:
				ut_a(!done);
				if (fts_optimize_new_table(
					    static_cast<dict_table_t*>(
						    msg->ptr))) {
					++n_tables;
				}
				break;

			case FTS_MSG_DEL_TABLE:
				if (fts_optimize_del_table(
					    static_cast<fts_msg_del_t*>(
						    msg->ptr))) {
					--n_tables;
				}
				break;

			case FTS_MSG_SYNC_TABLE:
				if (UNIV_UNLIKELY(wsrep_sst_disable_writes)) {
					add_msg(msg);
					goto retry_later;
				}

				DBUG_EXECUTE_IF(
					"fts_instrument_msg_sync_sleep",
					std::this_thread::sleep_for(
						std::chrono::milliseconds(
							300)););

				fts_optimize_sync_table(
					static_cast<dict_table_t*>(msg->ptr),
					true);
				break;

			default:
				ut_error;
			}

			mem_heap_free(msg->heap);
			n_optimize = done ? 0 : fts_optimize_how_many();
		}
	}

	/* Server is being shutdown, sync the data from FTS cache to disk
	if needed */
	if (n_tables > 0) {
		for (ulint i = 0; i < ib_vector_size(fts_slots); i++) {
			fts_slot_t* slot = static_cast<fts_slot_t*>(
				ib_vector_get(fts_slots, i));

			if (slot->table) {
				fts_optimize_sync_table(slot->table);
			}
		}
	}

	ib_vector_free(fts_slots);
	mysql_mutex_lock(&fts_optimize_wq->mutex);
	fts_slots = NULL;
	pthread_cond_broadcast(&fts_opt_shutdown_cond);
	mysql_mutex_unlock(&fts_optimize_wq->mutex);

	ib::info() << "FTS optimize thread exiting.";
}

/**********************************************************************//**
Startup the optimize thread and create the work queue. */
void
fts_optimize_init(void)
/*===================*/
{
	mem_heap_t*	heap;
	ib_alloc_t*     heap_alloc;

	ut_ad(!srv_read_only_mode);

	/* For now we only support one optimize thread. */
	ut_a(!fts_optimize_wq);

	/* Create FTS optimize work queue */
	fts_optimize_wq = ib_wqueue_create();
	timer = srv_thread_pool->create_timer(timer_callback);

	/* Create FTS vector to store fts_slot_t */
	heap = mem_heap_create(sizeof(dict_table_t*) * 64);
	heap_alloc = ib_heap_allocator_create(heap);
	fts_slots = ib_vector_create(heap_alloc, sizeof(fts_slot_t), 4);

	fts_opt_thd = innobase_create_background_thd("InnoDB FTS optimizer");
	/* Add fts tables to fts_slots which could be skipped
	during dict_load_table_one() because fts_optimize_thread
	wasn't even started. */
	dict_sys.freeze(SRW_LOCK_CALL);
	for (dict_table_t* table = UT_LIST_GET_FIRST(dict_sys.table_LRU);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {
		if (!table->fts || !dict_table_has_fts_index(table)) {
			continue;
		}

		/* fts_optimize_thread is not started yet. So there is no
		need to acquire fts_optimize_wq->mutex for adding the fts
		table to the fts slots. */
		ut_ad(!table->can_be_evicted);
		fts_optimize_new_table(table);
		table->fts->in_queue = true;
	}
	dict_sys.unfreeze();

	pthread_cond_init(&fts_opt_shutdown_cond, nullptr);
	last_check_sync_time = time(NULL);
}

/** Shutdown fts optimize thread. */
void
fts_optimize_shutdown()
{
	ut_ad(!srv_read_only_mode);

	/* If there is an ongoing activity on dictionary, such as
	srv_master_evict_from_table_cache(), wait for it */
	dict_sys.freeze(SRW_LOCK_CALL);
	mysql_mutex_lock(&fts_optimize_wq->mutex);
	/* Tells FTS optimizer system that we are exiting from
	optimizer thread, message send their after will not be
	processed */
	fts_opt_start_shutdown = true;
	dict_sys.unfreeze();

	/* We tell the OPTIMIZE thread to switch to state done, we
	can't delete the work queue here because the add thread needs
	deregister the FTS tables. */
	timer->disarm();
	task_group.cancel_pending(&task);

	add_msg(fts_optimize_create_msg(FTS_MSG_STOP, nullptr));

	while (fts_slots) {
		my_cond_wait(&fts_opt_shutdown_cond,
			     &fts_optimize_wq->mutex.m_mutex);
	}

	destroy_background_thd(fts_opt_thd);
	fts_opt_thd = NULL;
	pthread_cond_destroy(&fts_opt_shutdown_cond);
	mysql_mutex_unlock(&fts_optimize_wq->mutex);

	ib_wqueue_free(fts_optimize_wq);
	fts_optimize_wq = NULL;

	delete timer;
	timer = NULL;
}

/** Sync the table during commit phase
@param[in]	table	table to be synced */
void fts_sync_during_ddl(dict_table_t* table)
{
  if (!fts_optimize_wq)
    return;
  mysql_mutex_lock(&fts_optimize_wq->mutex);
  const auto sync_message= table->fts->sync_message;
  mysql_mutex_unlock(&fts_optimize_wq->mutex);
  if (!sync_message)
    return;

  fts_sync_table(table, false);

  mysql_mutex_lock(&fts_optimize_wq->mutex);
  table->fts->sync_message = false;
  mysql_mutex_unlock(&fts_optimize_wq->mutex);
}
