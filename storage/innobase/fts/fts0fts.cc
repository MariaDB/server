/*****************************************************************************

Copyright (c) 2011, 2021, Oracle and/or its affiliates.
Copyright (c) 2016, 2023, MariaDB Corporation.

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
@file fts/fts0fts.cc
Full Text Search interface
***********************************************************************/

#include "trx0roll.h"
#include "trx0purge.h"
#include "row0mysql.h"
#include "row0upd.h"
#include "dict0types.h"
#include "dict0stats_bg.h"
#include "row0sel.h"
#include "fts0fts.h"
#include "fts0priv.h"
#include "fts0types.h"
#include "fts0types.inl"
#include "fts0vlc.h"
#include "fts0plugin.h"
#include "dict0stats.h"
#include "btr0pcur.h"
#include "log.h"
#include "row0sel.h"
#include "row0ins.h"
#include "row0vers.h"

static const ulint FTS_MAX_ID_LEN = 32;

/** Column name from the FTS config table */
#define FTS_MAX_CACHE_SIZE_IN_MB	"cache_size_in_mb"

/** Verify if a aux table name is a obsolete table
by looking up the key word in the obsolete table names */
#define FTS_IS_OBSOLETE_AUX_TABLE(table_name)			\
	(strstr((table_name), "DOC_ID") != NULL			\
	 || strstr((table_name), "ADDED") != NULL		\
	 || strstr((table_name), "STOPWORDS") != NULL)

/** This is maximum FTS cache for each table and would be
a configurable variable */
Atomic_relaxed<size_t> fts_max_cache_size;

/** Whether the total memory used for FTS cache is exhausted, and we will
need a sync to free some memory */
bool	fts_need_sync = false;

/** Variable specifying the total memory allocated for FTS cache */
Atomic_relaxed<size_t> fts_max_total_cache_size;

/** This is FTS result cache limit for each query and would be
a configurable variable */
size_t	fts_result_cache_limit;

/** Variable specifying the maximum FTS max token size */
ulong	fts_max_token_size;

/** Variable specifying the minimum FTS max token size */
ulong	fts_min_token_size;


// FIXME: testing
static time_t elapsed_time;
static ulint n_nodes;

#ifdef FTS_CACHE_SIZE_DEBUG
/** The cache size permissible lower limit (1K) */
static const ulint FTS_CACHE_SIZE_LOWER_LIMIT_IN_MB = 1;

/** The cache size permissible upper limit (1G) */
static const ulint FTS_CACHE_SIZE_UPPER_LIMIT_IN_MB = 1024;
#endif

/** Time to sleep after DEADLOCK error before retrying operation. */
static const std::chrono::milliseconds FTS_DEADLOCK_RETRY_WAIT(100);

/** InnoDB default stopword list:
There are different versions of stopwords, the stop words listed
below comes from "Google Stopword" list. Reference:
http://meta.wikimedia.org/wiki/Stop_word_list/google_stop_word_list.
The final version of InnoDB default stopword list is still pending
for decision */
const char *fts_default_stopword[] =
{
	"a",
	"about",
	"an",
	"are",
	"as",
	"at",
	"be",
	"by",
	"com",
	"de",
	"en",
	"for",
	"from",
	"how",
	"i",
	"in",
	"is",
	"it",
	"la",
	"of",
	"on",
	"or",
	"that",
	"the",
	"this",
	"to",
	"was",
	"what",
	"when",
	"where",
	"who",
	"will",
	"with",
	"und",
	"the",
	"www",
	NULL
};

/** FTS auxiliary table suffixes that are common to all FT indexes. */
const char* fts_common_tables[] = {
	"BEING_DELETED",
	"BEING_DELETED_CACHE",
	"CONFIG",
	"DELETED",
	"DELETED_CACHE",
	NULL
};

/** FTS auxiliary INDEX split intervals. */
const  fts_index_selector_t fts_index_selector[] = {
	{ 9, "INDEX_1" },
	{ 65, "INDEX_2" },
	{ 70, "INDEX_3" },
	{ 75, "INDEX_4" },
	{ 80, "INDEX_5" },
	{ 85, "INDEX_6" },
	{  0 , NULL	 }
};

/** FTS tokenize parmameter for plugin parser */
struct fts_tokenize_param_t {
	fts_doc_t*	result_doc;	/*!< Result doc for tokens */
	ulint		add_pos;	/*!< Added position for tokens */
};

/** Run SYNC on the table, i.e., write out data from the cache to the
FTS auxiliary INDEX table and clear the cache at the end.
@param[in,out]	sync		sync state
@param[in]	unlock_cache	whether unlock cache lock when write node
@param[in]	wait		whether wait when a sync is in progress
@return DB_SUCCESS if all OK */
static
dberr_t
fts_sync(
	fts_sync_t*	sync,
	bool		unlock_cache,
	bool		wait);

/****************************************************************//**
Release all resources help by the words rb tree e.g., the node ilist. */
static
void
fts_words_free(
/*===========*/
	ib_rbt_t*	words)		/*!< in: rb tree of words */
	MY_ATTRIBUTE((nonnull));
#ifdef FTS_CACHE_SIZE_DEBUG
/****************************************************************//**
Read the max cache size parameter from the config table. */
static
void
fts_update_max_cache_size(
/*======================*/
	fts_sync_t*	sync);		/*!< in: sync state */
#endif

/*********************************************************************//**
This function fetches the document just inserted right before
we commit the transaction, and tokenize the inserted text data
and insert into FTS auxiliary table and its cache. */
static
void
fts_add_doc_by_id(
/*==============*/
	fts_trx_table_t*ftt,		/*!< in: FTS trx table */
	doc_id_t	doc_id);	/*!< in: doc id */

/** Tokenize a document.
@param[in,out]	doc	document to tokenize
@param[out]	result	tokenization result
@param[in]	parser	pluggable parser */
static
void
fts_tokenize_document(
	fts_doc_t*		doc,
	fts_doc_t*		result,
	st_mysql_ftparser*	parser);

/** Continue to tokenize a document.
@param[in,out]	doc	document to tokenize
@param[in]	add_pos	add this position to all tokens from this tokenization
@param[out]	result	tokenization result
@param[in]	parser	pluggable parser */
static
void
fts_tokenize_document_next(
	fts_doc_t*		doc,
	ulint			add_pos,
	fts_doc_t*		result,
	st_mysql_ftparser*	parser);

/** Create the vector of fts_get_doc_t instances.
@param[in,out]	cache	fts cache
@return	vector of fts_get_doc_t instances */
static
ib_vector_t*
fts_get_docs_create(
	fts_cache_t*	cache);

/** Free the FTS cache.
@param[in,out]	cache to be freed */
static
void
fts_cache_destroy(fts_cache_t* cache)
{
	mysql_mutex_destroy(&cache->lock);
	mysql_mutex_destroy(&cache->init_lock);
	mysql_mutex_destroy(&cache->deleted_lock);
	mysql_mutex_destroy(&cache->doc_id_lock);
	pthread_cond_destroy(&cache->sync->cond);

	if (cache->stopword_info.cached_stopword) {
		rbt_free(cache->stopword_info.cached_stopword);
	}

	if (cache->sync_heap->arg) {
		mem_heap_free(static_cast<mem_heap_t*>(cache->sync_heap->arg));
	}

	mem_heap_free(cache->cache_heap);
}

/** Get the table id.
@param fts_table FTS Auxiliary table
@param table_id table id must be FTS_AUX_MIN_TABLE_ID_LENGTH bytes
                long
@return number of bytes written */
int fts_get_table_id(const fts_table_t *fts_table, char *table_id)
{
  int len;
  ut_a(fts_table->table != NULL);
  switch (fts_table->type) {
  case FTS_COMMON_TABLE:
    len = fts_write_object_id(fts_table->table_id, table_id);
    break;
  case FTS_INDEX_TABLE:
    len = fts_write_object_id(fts_table->table_id, table_id);
    table_id[len] = '_';
    ++len;
    table_id += len;
    len += fts_write_object_id(fts_table->index_id, table_id);
    break;
  default: ut_error;
  }
  ut_a(len >= 16);
  ut_a(len < FTS_AUX_MIN_TABLE_ID_LENGTH);
  return len;
}

/** Construct the name of an internal FTS table for the given table.
@param[in]	fts_table	metadata on fulltext-indexed table
@param[out]	table_name	a name up to MAX_FULL_NAME_LEN
@param[in]	dict_locked	whether dict_sys.latch is being held */
void fts_get_table_name(const fts_table_t* fts_table, char* table_name,
			bool dict_locked)
{
  if (!dict_locked) dict_sys.freeze(SRW_LOCK_CALL);
  ut_ad(dict_sys.frozen());
  /* Include the separator as well. */
  const size_t dbname_len = fts_table->table->name.dblen() + 1;
  ut_ad(dbname_len > 1);
  memcpy(table_name, fts_table->table->name.m_name, dbname_len);
  if (!dict_locked) dict_sys.unfreeze();
  memcpy(table_name += dbname_len, "FTS_", 4);
  table_name += 4;
  table_name += fts_get_table_id(fts_table, table_name);
  *table_name++ = '_';
  strcpy(table_name, fts_table->suffix);
}

/** Get a character set based on precise type.
@param prtype precise type
@return the corresponding character set */
UNIV_INLINE
CHARSET_INFO*
fts_get_charset(ulint prtype)
{
#ifdef UNIV_DEBUG
	switch (prtype & DATA_MYSQL_TYPE_MASK) {
	case MYSQL_TYPE_BIT:
	case MYSQL_TYPE_STRING:
	case MYSQL_TYPE_VAR_STRING:
	case MYSQL_TYPE_TINY_BLOB:
	case MYSQL_TYPE_MEDIUM_BLOB:
	case MYSQL_TYPE_BLOB:
	case MYSQL_TYPE_LONG_BLOB:
	case MYSQL_TYPE_VARCHAR:
		break;
	default:
		ut_error;
	}
#endif /* UNIV_DEBUG */

	uint cs_num = (uint) dtype_get_charset_coll(prtype);

	if (CHARSET_INFO* cs = get_charset(cs_num, MYF(MY_WME))) {
		return(cs);
	}

	ib::fatal() << "Unable to find charset-collation " << cs_num;
	return(NULL);
}

/****************************************************************//**
This function loads the default InnoDB stopword list */
static
void
fts_load_default_stopword(
/*======================*/
	fts_stopword_t*		stopword_info)	/*!< in: stopword info */
{
	fts_string_t		str;
	mem_heap_t*		heap;
	ib_alloc_t*		allocator;
	ib_rbt_t*		stop_words;

	allocator = stopword_info->heap;
	heap = static_cast<mem_heap_t*>(allocator->arg);

	if (!stopword_info->cached_stopword) {
		stopword_info->cached_stopword = rbt_create_arg_cmp(
			sizeof(fts_tokenizer_word_t), innobase_fts_text_cmp,
			&my_charset_latin1);
	}

	stop_words = stopword_info->cached_stopword;

	str.f_n_char = 0;

	for (ulint i = 0; fts_default_stopword[i]; ++i) {
		char*			word;
		fts_tokenizer_word_t	new_word;

		/* We are going to duplicate the value below. */
		word = const_cast<char*>(fts_default_stopword[i]);

		new_word.nodes = ib_vector_create(
			allocator, sizeof(fts_node_t), 4);

		str.f_len = strlen(word);
		str.f_str = reinterpret_cast<byte*>(word);

		fts_string_dup(&new_word.text, &str, heap);

		rbt_insert(stop_words, &new_word, &new_word);
	}

	stopword_info->status = STOPWORD_FROM_DEFAULT;
}

/** Callback function to read a single stopword value. */
static
bool fts_read_stopword(dict_index_t *index, const rec_t *rec,
                       const rec_offs *offsets, void *user_arg)
{
  ut_ad(dict_index_is_clust(index));
  ib_rbt_bound_t parent;
  fts_stopword_t *stopword_info= static_cast<fts_stopword_t*>(user_arg);
  ib_rbt_t *stop_words= stopword_info->cached_stopword;
  ib_alloc_t *allocator=  static_cast<ib_alloc_t*>(stopword_info->heap);
  mem_heap_t *heap= static_cast<mem_heap_t*>(allocator->arg);
  const byte *data= nullptr;
  ulint len;
  fts_string_t str;
  bool versioning= index->table->versioned();

  for (uint32_t i= 0; i < index->n_fields; i++)
  {
    dict_field_t *f= &index->fields[i];
    if (!strcmp(f->name(), "value"))
    {
      data= rec_get_nth_field(rec, offsets, i, &len);
      str.f_n_char= 0;
      str.f_str= const_cast<byte*>(data);
      str.f_len= len;
    }

    if (versioning && !strcmp(f->name(), "row_end"))
    {
      data= rec_get_nth_field(rec, offsets, i, &len);
      if (index->table->versioned_by_id())
      {
        ut_ad(len == sizeof trx_id_max_bytes);
        if (0 != memcmp(data, trx_id_max_bytes, len)) return true;
      }
      else
      {
        ut_ad(len == sizeof timestamp_max_bytes);
        if (!IS_MAX_TIMESTAMP(data)) return true;
      }
    }
  }

  /* Only create new node if it is a value not already existed */
  if (str.f_len != UNIV_SQL_NULL &&
      rbt_search(stop_words, &parent, &str) != 0)
  {
    fts_tokenizer_word_t new_word;
    new_word.nodes = ib_vector_create(allocator, sizeof(fts_node_t), 4);
    new_word.text.f_str = static_cast<byte*>(
      mem_heap_alloc(heap, str.f_len + 1));

    memcpy(new_word.text.f_str, str.f_str, str.f_len);

    new_word.text.f_n_char = 0;
    new_word.text.f_len = str.f_len;
    new_word.text.f_str[str.f_len] = 0;
    rbt_insert(stop_words, &new_word, &new_word);
  }

  return true;
}

/******************************************************************//**
Load user defined stopword from designated user table
@return whether the operation is successful */
static
bool
fts_load_user_stopword(
/*===================*/
	fts_t*		fts,			/*!< in: FTS struct */
	const char*	stopword_table_name,	/*!< in: Stopword table
						name */
	fts_stopword_t*	stopword_info)		/*!< in: Stopword info */
{
	if (!fts->dict_locked) {
		dict_sys.lock(SRW_LOCK_CALL);
	}

	/* Validate the user table existence in the right format */
	bool ret= false;
	const char* row_end;
	stopword_info->charset = fts_valid_stopword_table(stopword_table_name,
							  &row_end);
	if (!stopword_info->charset) {
cleanup:
		if (!fts->dict_locked) {
			dict_sys.unlock();
		}

		return ret;
	}

	trx_t* trx = trx_create();
	trx->op_info = "Load user stopword table into FTS cache";

	if (!stopword_info->cached_stopword) {
		/* Create the stopword RB tree with the stopword column
		charset. All comparison will use this charset */
		stopword_info->cached_stopword = rbt_create_arg_cmp(
			sizeof(fts_tokenizer_word_t), innobase_fts_text_cmp,
			(void*)stopword_info->charset);

	}

	FTSQueryRunner sqlRunner(trx);
	dberr_t err= DB_SUCCESS;
	dict_table_t *table= dict_table_open_on_name(stopword_table_name, true,
						     DICT_ERR_IGNORE_NONE);
	if (table) {
		err = sqlRunner.prepare_for_read(table);

		if (err == DB_SUCCESS) {
			dict_index_t *index = dict_table_get_first_index(table);
			ut_d(sqlRunner.is_stopword_table= true;);
			err= sqlRunner.record_executor(
				index, READ, MATCH_ALL, PAGE_CUR_GE,
				&fts_read_stopword, stopword_info);
			if (err == DB_END_OF_INDEX) err= DB_SUCCESS;
		}

		if (err == DB_SUCCESS) {
			fts_sql_commit(trx);
			ret = true;
		}
	}

	if (err) {
		ib::error() << "Error '" << err << "' while reading user "
			    << "stopword table.";
		trx->rollback();
	}

	if (table) table->release();
	trx->free();
	goto cleanup;
}

/******************************************************************//**
Initialize the index cache. */
static
void
fts_index_cache_init(
/*=================*/
	ib_alloc_t*		allocator,	/*!< in: the allocator to use */
	fts_index_cache_t*	index_cache)	/*!< in: index cache */
{
	ut_a(index_cache->words == NULL);

	index_cache->words = rbt_create_arg_cmp(
		sizeof(fts_tokenizer_word_t), innobase_fts_text_cmp,
		(void*) index_cache->charset);

	ut_a(index_cache->doc_stats == NULL);

	index_cache->doc_stats = ib_vector_create(
		allocator, sizeof(fts_doc_stats_t), 4);
}

/*********************************************************************//**
Initialize FTS cache. */
void
fts_cache_init(
/*===========*/
	fts_cache_t*	cache)		/*!< in: cache to initialize */
{
	ulint		i;

	/* Just to make sure */
	ut_a(cache->sync_heap->arg == NULL);

	cache->sync_heap->arg = mem_heap_create(1024);

	cache->total_size = 0;
	cache->total_size_at_sync = 0;

	mysql_mutex_lock(&cache->deleted_lock);
	cache->deleted_doc_ids = ib_vector_create(
		cache->sync_heap, sizeof(doc_id_t), 4);
	mysql_mutex_unlock(&cache->deleted_lock);

	/* Reset the cache data for all the FTS indexes. */
	for (i = 0; i < ib_vector_size(cache->indexes); ++i) {
		fts_index_cache_t*	index_cache;

		index_cache = static_cast<fts_index_cache_t*>(
			ib_vector_get(cache->indexes, i));

		fts_index_cache_init(cache->sync_heap, index_cache);
	}
}

/****************************************************************//**
Create a FTS cache. */
fts_cache_t*
fts_cache_create(
/*=============*/
	dict_table_t*	table)	/*!< in: table owns the FTS cache */
{
	mem_heap_t*	heap;
	fts_cache_t*	cache;

	heap = static_cast<mem_heap_t*>(mem_heap_create(512));

	cache = static_cast<fts_cache_t*>(
		mem_heap_zalloc(heap, sizeof(*cache)));

	cache->cache_heap = heap;

	mysql_mutex_init(fts_cache_mutex_key, &cache->lock, nullptr);
	mysql_mutex_init(fts_cache_init_mutex_key, &cache->init_lock, nullptr);
	mysql_mutex_init(fts_delete_mutex_key, &cache->deleted_lock, nullptr);
	mysql_mutex_init(fts_doc_id_mutex_key, &cache->doc_id_lock, nullptr);

	/* This is the heap used to create the cache itself. */
	cache->self_heap = ib_heap_allocator_create(heap);

	/* This is a transient heap, used for storing sync data. */
	cache->sync_heap = ib_heap_allocator_create(heap);
	cache->sync_heap->arg = NULL;

	cache->sync = static_cast<fts_sync_t*>(
		mem_heap_zalloc(heap, sizeof(fts_sync_t)));

	cache->sync->table = table;
	pthread_cond_init(&cache->sync->cond, nullptr);

	/* Create the index cache vector that will hold the inverted indexes. */
	cache->indexes = ib_vector_create(
		cache->self_heap, sizeof(fts_index_cache_t), 2);

	fts_cache_init(cache);

	cache->stopword_info.cached_stopword = NULL;
	cache->stopword_info.charset = NULL;

	cache->stopword_info.heap = cache->self_heap;

	cache->stopword_info.status = STOPWORD_NOT_INIT;

	return(cache);
}

/*******************************************************************//**
Add a newly create index into FTS cache */
void
fts_add_index(
/*==========*/
	dict_index_t*	index,		/*!< FTS index to be added */
	dict_table_t*	table)		/*!< table */
{
	fts_t*			fts = table->fts;
	fts_cache_t*		cache;
	fts_index_cache_t*	index_cache;

	ut_ad(fts);
	cache = table->fts->cache;

	mysql_mutex_lock(&cache->init_lock);

	ib_vector_push(fts->indexes, &index);

	index_cache = fts_find_index_cache(cache, index);

	if (!index_cache) {
		/* Add new index cache structure */
		index_cache = fts_cache_index_cache_create(table, index);
	}

	mysql_mutex_unlock(&cache->init_lock);
}

/*******************************************************************//**
recalibrate get_doc structure after index_cache in cache->indexes changed */
static
void
fts_reset_get_doc(
/*==============*/
	fts_cache_t*	cache)	/*!< in: FTS index cache */
{
	fts_get_doc_t*  get_doc;
	ulint		i;

	mysql_mutex_assert_owner(&cache->init_lock);

	ib_vector_reset(cache->get_docs);

	for (i = 0; i < ib_vector_size(cache->indexes); i++) {
		fts_index_cache_t*	ind_cache;

		ind_cache = static_cast<fts_index_cache_t*>(
			ib_vector_get(cache->indexes, i));

		get_doc = static_cast<fts_get_doc_t*>(
			ib_vector_push(cache->get_docs, NULL));

		memset(get_doc, 0x0, sizeof(*get_doc));

		get_doc->index_cache = ind_cache;
		get_doc->cache = cache;
	}

	ut_ad(ib_vector_size(cache->get_docs)
	      == ib_vector_size(cache->indexes));
}

/*******************************************************************//**
Check an index is in the table->indexes list
@return TRUE if it exists */
static
ibool
fts_in_dict_index(
/*==============*/
	dict_table_t*	table,		/*!< in: Table */
	dict_index_t*	index_check)	/*!< in: index to be checked */
{
	dict_index_t*	index;

	for (index = dict_table_get_first_index(table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {

		if (index == index_check) {
			return(TRUE);
		}
	}

	return(FALSE);
}

/*******************************************************************//**
Check an index is in the fts->cache->indexes list
@return TRUE if it exists */
static
ibool
fts_in_index_cache(
/*===============*/
	dict_table_t*	table,	/*!< in: Table */
	dict_index_t*	index)	/*!< in: index to be checked */
{
	ulint	i;

	for (i = 0; i < ib_vector_size(table->fts->cache->indexes); i++) {
		fts_index_cache_t*      index_cache;

		index_cache = static_cast<fts_index_cache_t*>(
			ib_vector_get(table->fts->cache->indexes, i));

		if (index_cache->index == index) {
			return(TRUE);
		}
	}

	return(FALSE);
}

/*******************************************************************//**
Check indexes in the fts->indexes is also present in index cache and
table->indexes list
@return TRUE if all indexes match */
ibool
fts_check_cached_index(
/*===================*/
	dict_table_t*	table)	/*!< in: Table where indexes are dropped */
{
	ulint	i;

	if (!table->fts || !table->fts->cache) {
		return(TRUE);
	}

	ut_a(ib_vector_size(table->fts->indexes)
	      == ib_vector_size(table->fts->cache->indexes));

	for (i = 0; i < ib_vector_size(table->fts->indexes); i++) {
		dict_index_t*	index;

		index = static_cast<dict_index_t*>(
			ib_vector_getp(table->fts->indexes, i));

		if (!fts_in_index_cache(table, index)) {
			return(FALSE);
		}

		if (!fts_in_dict_index(table, index)) {
			return(FALSE);
		}
	}

	return(TRUE);
}

/** Clear all fts resources when there is no internal DOC_ID
and there are no new fts index to add.
@param[in,out]	table	table  where fts is to be freed */
void fts_clear_all(dict_table_t *table)
{
  if (DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID) ||
      !table->fts ||
      !ib_vector_is_empty(table->fts->indexes))
    return;

  for (const dict_index_t *index= dict_table_get_first_index(table);
       index; index= dict_table_get_next_index(index))
    if (index->type & DICT_FTS)
      return;

  fts_optimize_remove_table(table);

  table->fts->~fts_t();
  table->fts= nullptr;
  DICT_TF2_FLAG_UNSET(table, DICT_TF2_FTS);
}

/*******************************************************************//**
Drop auxiliary tables related to an FTS index
@return DB_SUCCESS or error number */
dberr_t
fts_drop_index(
/*===========*/
	dict_table_t*	table,	/*!< in: Table where indexes are dropped */
	dict_index_t*	index,	/*!< in: Index to be dropped */
	trx_t*		trx)	/*!< in: Transaction for the drop */
{
	ib_vector_t*	indexes = table->fts->indexes;
	dberr_t		err = DB_SUCCESS;

	ut_a(indexes);

	if ((ib_vector_size(indexes) == 1
	     && (index == static_cast<dict_index_t*>(
			ib_vector_getp(table->fts->indexes, 0)))
	     && DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID))
	    || ib_vector_is_empty(indexes)) {
		doc_id_t	current_doc_id;
		doc_id_t	first_doc_id;

		DICT_TF2_FLAG_UNSET(table, DICT_TF2_FTS);

		current_doc_id = table->fts->cache->next_doc_id;
		first_doc_id = table->fts->cache->first_doc_id;
		fts_cache_clear(table->fts->cache);
		fts_cache_destroy(table->fts->cache);
		table->fts->cache = fts_cache_create(table);
		table->fts->cache->next_doc_id = current_doc_id;
		table->fts->cache->first_doc_id = first_doc_id;
	} else {
		fts_cache_t*            cache = table->fts->cache;
		fts_index_cache_t*      index_cache;

		mysql_mutex_lock(&cache->init_lock);

		index_cache = fts_find_index_cache(cache, index);

		if (index_cache != NULL) {
			if (index_cache->words) {
				fts_words_free(index_cache->words);
				rbt_free(index_cache->words);
			}

			ib_vector_remove(cache->indexes, *(void**) index_cache);
		}

		if (cache->get_docs) {
			fts_reset_get_doc(cache);
		}

		mysql_mutex_unlock(&cache->init_lock);
	}

	err = fts_drop_index_tables(trx, *index);

	ib_vector_remove(indexes, (const void*) index);

	return(err);
}

/****************************************************************//**
Create an FTS index cache. */
CHARSET_INFO*
fts_index_get_charset(
/*==================*/
	dict_index_t*		index)		/*!< in: FTS index */
{
	CHARSET_INFO*		charset = NULL;
	dict_field_t*		field;
	ulint			prtype;

	field = dict_index_get_nth_field(index, 0);
	prtype = field->col->prtype;

	charset = fts_get_charset(prtype);

#ifdef FTS_DEBUG
	/* Set up charset info for this index. Please note all
	field of the FTS index should have the same charset */
	for (i = 1; i < index->n_fields; i++) {
		CHARSET_INFO*   fld_charset;

		field = dict_index_get_nth_field(index, i);
		prtype = field->col->prtype;

		fld_charset = fts_get_charset(prtype);

		/* All FTS columns should have the same charset */
		if (charset) {
			ut_a(charset == fld_charset);
		} else {
			charset = fld_charset;
		}
	}
#endif

	return(charset);

}
/****************************************************************//**
Create an FTS index cache.
@return Index Cache */
fts_index_cache_t*
fts_cache_index_cache_create(
/*=========================*/
	dict_table_t*		table,		/*!< in: table with FTS index */
	dict_index_t*		index)		/*!< in: FTS index */
{
	fts_index_cache_t*	index_cache;
	fts_cache_t*		cache = table->fts->cache;

	ut_a(cache != NULL);

	mysql_mutex_assert_owner(&cache->init_lock);

	/* Must not already exist in the cache vector. */
	ut_a(fts_find_index_cache(cache, index) == NULL);

	index_cache = static_cast<fts_index_cache_t*>(
		ib_vector_push(cache->indexes, NULL));

	memset(index_cache, 0x0, sizeof(*index_cache));

	index_cache->index = index;

	index_cache->charset = fts_index_get_charset(index);

	fts_index_cache_init(cache->sync_heap, index_cache);

	if (cache->get_docs) {
		fts_reset_get_doc(cache);
	}

	return(index_cache);
}

/****************************************************************//**
Release all resources help by the words rb tree e.g., the node ilist. */
static
void
fts_words_free(
/*===========*/
	ib_rbt_t*	words)			/*!< in: rb tree of words */
{
	const ib_rbt_node_t*	rbt_node;

	/* Free the resources held by a word. */
	for (rbt_node = rbt_first(words);
	     rbt_node != NULL;
	     rbt_node = rbt_first(words)) {

		ulint			i;
		fts_tokenizer_word_t*	word;

		word = rbt_value(fts_tokenizer_word_t, rbt_node);

		/* Free the ilists of this word. */
		for (i = 0; i < ib_vector_size(word->nodes); ++i) {

			fts_node_t* fts_node = static_cast<fts_node_t*>(
				ib_vector_get(word->nodes, i));

			ut_free(fts_node->ilist);
			fts_node->ilist = NULL;
		}

		/* NOTE: We are responsible for free'ing the node */
		ut_free(rbt_remove_node(words, rbt_node));
	}
}

/** Clear cache.
@param[in,out]	cache	fts cache */
void
fts_cache_clear(
	fts_cache_t*	cache)
{
	ulint		i;

	for (i = 0; i < ib_vector_size(cache->indexes); ++i) {
		fts_index_cache_t*	index_cache;

		index_cache = static_cast<fts_index_cache_t*>(
			ib_vector_get(cache->indexes, i));

		fts_words_free(index_cache->words);

		rbt_free(index_cache->words);

		index_cache->words = NULL;

		index_cache->doc_stats = NULL;
	}

	fts_need_sync = false;

	cache->total_size = 0;

	mysql_mutex_lock(&cache->deleted_lock);
	cache->deleted_doc_ids = NULL;
	mysql_mutex_unlock(&cache->deleted_lock);

	mem_heap_free(static_cast<mem_heap_t*>(cache->sync_heap->arg));
	cache->sync_heap->arg = NULL;
}

/*********************************************************************//**
Search the index specific cache for a particular FTS index.
@return the index cache else NULL */
UNIV_INLINE
fts_index_cache_t*
fts_get_index_cache(
/*================*/
	fts_cache_t*		cache,		/*!< in: cache to search */
	const dict_index_t*	index)		/*!< in: index to search for */
{
#ifdef SAFE_MUTEX
	ut_ad(mysql_mutex_is_owner(&cache->lock)
	      || mysql_mutex_is_owner(&cache->init_lock));
#endif /* SAFE_MUTEX */

	for (ulint i = 0; i < ib_vector_size(cache->indexes); ++i) {
		fts_index_cache_t*	index_cache;

		index_cache = static_cast<fts_index_cache_t*>(
			ib_vector_get(cache->indexes, i));

		if (index_cache->index == index) {

			return(index_cache);
		}
	}

	return(NULL);
}

#ifdef FTS_DEBUG
/*********************************************************************//**
Search the index cache for a get_doc structure.
@return the fts_get_doc_t item else NULL */
static
fts_get_doc_t*
fts_get_index_get_doc(
/*==================*/
	fts_cache_t*		cache,		/*!< in: cache to search */
	const dict_index_t*	index)		/*!< in: index to search for */
{
	ulint			i;

	mysql_mutex_assert_owner(&cache->init_lock);

	for (i = 0; i < ib_vector_size(cache->get_docs); ++i) {
		fts_get_doc_t*	get_doc;

		get_doc = static_cast<fts_get_doc_t*>(
			ib_vector_get(cache->get_docs, i));

		if (get_doc->index_cache->index == index) {

			return(get_doc);
		}
	}

	return(NULL);
}
#endif

/**********************************************************************//**
Find an existing word, or if not found, create one and return it.
@return specified word token */
static
fts_tokenizer_word_t*
fts_tokenizer_word_get(
/*===================*/
	fts_cache_t*	cache,			/*!< in: cache */
	fts_index_cache_t*
			index_cache,		/*!< in: index cache */
	fts_string_t*	text)			/*!< in: node text */
{
	fts_tokenizer_word_t*	word;
	ib_rbt_bound_t		parent;

	mysql_mutex_assert_owner(&cache->lock);

	/* If it is a stopword, do not index it */
	if (!fts_check_token(text,
		    cache->stopword_info.cached_stopword,
		    index_cache->charset)) {

		return(NULL);
	}

	/* Check if we found a match, if not then add word to tree. */
	if (rbt_search(index_cache->words, &parent, text) != 0) {
		mem_heap_t*		heap;
		fts_tokenizer_word_t	new_word;

		heap = static_cast<mem_heap_t*>(cache->sync_heap->arg);

		new_word.nodes = ib_vector_create(
			cache->sync_heap, sizeof(fts_node_t), 4);

		fts_string_dup(&new_word.text, text, heap);

		parent.last = rbt_add_node(
			index_cache->words, &parent, &new_word);

		/* Take into account the RB tree memory use and the vector. */
		cache->total_size += sizeof(new_word)
			+ sizeof(ib_rbt_node_t)
			+ text->f_len
			+ (sizeof(fts_node_t) * 4)
			+ sizeof(*new_word.nodes);

		ut_ad(rbt_validate(index_cache->words));
	}

	word = rbt_value(fts_tokenizer_word_t, parent.last);

	return(word);
}

/**********************************************************************//**
Add the given doc_id/word positions to the given node's ilist. */
void
fts_cache_node_add_positions(
/*=========================*/
	fts_cache_t*	cache,		/*!< in: cache */
	fts_node_t*	node,		/*!< in: word node */
	doc_id_t	doc_id,		/*!< in: doc id */
	ib_vector_t*	positions)	/*!< in: fts_token_t::positions */
{
	ulint		i;
	byte*		ptr;
	byte*		ilist;
	ulint		enc_len;
	ulint		last_pos;
	byte*		ptr_start;
	doc_id_t	doc_id_delta;

#ifdef SAFE_MUTEX
	if (cache) {
		mysql_mutex_assert_owner(&cache->lock);
	}
#endif /* SAFE_MUTEX */

	ut_ad(doc_id >= node->last_doc_id);

	/* Calculate the space required to store the ilist. */
	doc_id_delta = doc_id - node->last_doc_id;
	enc_len = fts_get_encoded_len(doc_id_delta);

	last_pos = 0;
	for (i = 0; i < ib_vector_size(positions); i++) {
		ulint	pos = *(static_cast<ulint*>(
			ib_vector_get(positions, i)));

		ut_ad(last_pos == 0 || pos > last_pos);

		enc_len += fts_get_encoded_len(pos - last_pos);
		last_pos = pos;
	}

	/* The 0x00 byte at the end of the token positions list. */
	enc_len++;

	if ((node->ilist_size_alloc - node->ilist_size) >= enc_len) {
		/* No need to allocate more space, we can fit in the new
		data at the end of the old one. */
		ilist = NULL;
		ptr = node->ilist + node->ilist_size;
	} else {
		ulint	new_size = node->ilist_size + enc_len;

		/* Over-reserve space by a fixed size for small lengths and
		by 20% for lengths >= 48 bytes. */
		if (new_size < 16) {
			new_size = 16;
		} else if (new_size < 32) {
			new_size = 32;
		} else if (new_size < 48) {
			new_size = 48;
		} else {
			new_size = new_size * 6 / 5;
		}

		ilist = static_cast<byte*>(ut_malloc_nokey(new_size));
		ptr = ilist + node->ilist_size;

		node->ilist_size_alloc = new_size;
		if (cache) {
			cache->total_size += new_size;
		}
	}

	ptr_start = ptr;

	/* Encode the new fragment. */
	ptr = fts_encode_int(doc_id_delta, ptr);

	last_pos = 0;
	for (i = 0; i < ib_vector_size(positions); i++) {
		ulint	pos = *(static_cast<ulint*>(
			 ib_vector_get(positions, i)));

		ptr = fts_encode_int(pos - last_pos, ptr);
		last_pos = pos;
	}

	*ptr++ = 0;

	ut_a(enc_len == (ulint)(ptr - ptr_start));

	if (ilist) {
		/* Copy old ilist to the start of the new one and switch the
		new one into place in the node. */
		if (node->ilist_size > 0) {
			memcpy(ilist, node->ilist, node->ilist_size);
			ut_free(node->ilist);
			if (cache) {
				cache->total_size -= node->ilist_size;
			}
		}

		node->ilist = ilist;
	}

	node->ilist_size += enc_len;

	if (node->first_doc_id == FTS_NULL_DOC_ID) {
		node->first_doc_id = doc_id;
	}

	node->last_doc_id = doc_id;
	++node->doc_count;
}

/**********************************************************************//**
Add document to the cache. */
static
void
fts_cache_add_doc(
/*==============*/
	fts_cache_t*	cache,			/*!< in: cache */
	fts_index_cache_t*
			index_cache,		/*!< in: index cache */
	doc_id_t	doc_id,			/*!< in: doc id to add */
	ib_rbt_t*	tokens)			/*!< in: document tokens */
{
	const ib_rbt_node_t*	node;
	ulint			n_words;
	fts_doc_stats_t*	doc_stats;

	if (!tokens) {
		return;
	}

	mysql_mutex_assert_owner(&cache->lock);

	n_words = rbt_size(tokens);

	for (node = rbt_first(tokens); node; node = rbt_first(tokens)) {

		fts_tokenizer_word_t*	word;
		fts_node_t*		fts_node = NULL;
		fts_token_t*		token = rbt_value(fts_token_t, node);

		/* Find and/or add token to the cache. */
		word = fts_tokenizer_word_get(
			cache, index_cache, &token->text);

		if (!word) {
			ut_free(rbt_remove_node(tokens, node));
			continue;
		}

		if (ib_vector_size(word->nodes) > 0) {
			fts_node = static_cast<fts_node_t*>(
				ib_vector_last(word->nodes));
		}

		if (fts_node == NULL || fts_node->synced
		    || fts_node->ilist_size > FTS_ILIST_MAX_SIZE
		    || doc_id < fts_node->last_doc_id) {

			fts_node = static_cast<fts_node_t*>(
				ib_vector_push(word->nodes, NULL));

			memset(fts_node, 0x0, sizeof(*fts_node));

			cache->total_size += sizeof(*fts_node);
		}

		fts_cache_node_add_positions(
			cache, fts_node, doc_id, token->positions);

		ut_free(rbt_remove_node(tokens, node));
	}

	ut_a(rbt_empty(tokens));

	/* Add to doc ids processed so far. */
	doc_stats = static_cast<fts_doc_stats_t*>(
		ib_vector_push(index_cache->doc_stats, NULL));

	doc_stats->doc_id = doc_id;
	doc_stats->word_count = n_words;

	/* Add the doc stats memory usage too. */
	cache->total_size += sizeof(*doc_stats);

	if (doc_id > cache->sync->max_doc_id) {
		cache->sync->max_doc_id = doc_id;
	}
}

/** Drop a table.
@param trx          transaction
@param table_name   FTS_ table name
@param rename       whether to rename before dropping
@return error code
@retval DB_SUCCESS  if the table was dropped
@retval DB_FAIL     if the table did not exist */
static dberr_t fts_drop_table(trx_t *trx, const char *table_name, bool rename)
{
  if (dict_table_t *table= dict_table_open_on_name(table_name, true,
                                                   DICT_ERR_IGNORE_TABLESPACE))
  {
    table->release();
    if (rename)
    {
      mem_heap_t *heap= mem_heap_create(FN_REFLEN);
      char *tmp= dict_mem_create_temporary_tablename(heap, table->name.m_name,
                                                     table->id);
      dberr_t err= row_rename_table_for_mysql(table->name.m_name, tmp, trx,
                                              RENAME_IGNORE_FK);
      mem_heap_free(heap);
      if (err != DB_SUCCESS)
      {
        ib::error() << "Unable to rename table " << table_name << ": " << err;
        return err;
      }
    }
    if (dberr_t err= trx->drop_table(*table))
    {
      ib::error() << "Unable to drop table " << table->name << ": " << err;
      return err;
    }

#ifdef UNIV_DEBUG
    for (auto &p : trx->mod_tables)
    {
      if (p.first == table)
	p.second.set_aux_table();
    }
#endif /* UNIV_DEBUG */
    return DB_SUCCESS;
  }

  return DB_FAIL;
}

/****************************************************************//**
Rename a single auxiliary table due to database name change.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_rename_one_aux_table(
/*=====================*/
	const char*	new_name,		/*!< in: new parent tbl name */
	const char*	fts_table_old_name,	/*!< in: old aux tbl name */
	trx_t*		trx)			/*!< in: transaction */
{
	char	fts_table_new_name[MAX_TABLE_NAME_LEN];
	ulint	new_db_name_len = dict_get_db_name_len(new_name);
	ulint	old_db_name_len = dict_get_db_name_len(fts_table_old_name);
	ulint	table_new_name_len = strlen(fts_table_old_name)
				     + new_db_name_len - old_db_name_len;

	/* Check if the new and old database names are the same, if so,
	nothing to do */
	ut_ad((new_db_name_len != old_db_name_len)
	      || strncmp(new_name, fts_table_old_name, old_db_name_len) != 0);

	/* Get the database name from "new_name", and table name
	from the fts_table_old_name */
	strncpy(fts_table_new_name, new_name, new_db_name_len);
	strncpy(fts_table_new_name + new_db_name_len,
	       strchr(fts_table_old_name, '/'),
	       table_new_name_len - new_db_name_len);
	fts_table_new_name[table_new_name_len] = 0;

	return row_rename_table_for_mysql(
		fts_table_old_name, fts_table_new_name, trx, RENAME_IGNORE_FK);
}

/****************************************************************//**
Rename auxiliary tables for all fts index for a table. This(rename)
is due to database name change
@return DB_SUCCESS or error code */
dberr_t
fts_rename_aux_tables(
/*==================*/
	dict_table_t*	table,		/*!< in: user Table */
	const char*     new_name,       /*!< in: new table name */
	trx_t*		trx)		/*!< in: transaction */
{
	ulint		i;
	fts_table_t	fts_table;

	FTS_INIT_FTS_TABLE(&fts_table, NULL, FTS_COMMON_TABLE, table);

	dberr_t err = DB_SUCCESS;
	char old_table_name[MAX_FULL_NAME_LEN];

	/* Rename common auxiliary tables */
	for (i = 0; fts_common_tables[i] != NULL; ++i) {
		fts_table.suffix = fts_common_tables[i];
		fts_get_table_name(&fts_table, old_table_name, true);

		err = fts_rename_one_aux_table(new_name, old_table_name, trx);

		if (err != DB_SUCCESS) {
			return(err);
		}
	}

	fts_t*	fts = table->fts;

	/* Rename index specific auxiliary tables */
	for (i = 0; fts->indexes != 0 && i < ib_vector_size(fts->indexes);
	     ++i) {
		dict_index_t*	index;

		index = static_cast<dict_index_t*>(
			ib_vector_getp(fts->indexes, i));

		FTS_INIT_INDEX_TABLE(&fts_table, NULL, FTS_INDEX_TABLE, index);

		for (ulint j = 0; j < FTS_NUM_AUX_INDEX; ++j) {
			fts_table.suffix = fts_get_suffix(j);
			fts_get_table_name(&fts_table, old_table_name, true);

			err = fts_rename_one_aux_table(
				new_name, old_table_name, trx);

			DBUG_EXECUTE_IF("fts_rename_failure",
					err = DB_DEADLOCK;
					fts_sql_rollback(trx););

			if (err != DB_SUCCESS) {
				return(err);
			}
		}
	}

	return(DB_SUCCESS);
}

/** Lock an internal FTS_ table, before fts_drop_table() */
static dberr_t fts_lock_table(trx_t *trx, const char *table_name)
{
  ut_ad(purge_sys.must_wait_FTS());

  if (dict_table_t *table= dict_table_open_on_name(table_name, false,
                                                   DICT_ERR_IGNORE_TABLESPACE))
  {
    dberr_t err= lock_table_for_trx(table, trx, LOCK_X);
    /* Wait for purge threads to stop using the table. */
    for (uint n= 15; table->get_ref_count() > 1; )
    {
      if (!--n)
      {
        err= DB_LOCK_WAIT_TIMEOUT;
        goto fail;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
fail:
    table->release();
    return err;
  }
  return DB_SUCCESS;
}

/** Lock the internal FTS_ tables for an index, before fts_drop_index_tables().
@param trx   transaction
@param index fulltext index */
dberr_t fts_lock_index_tables(trx_t *trx, const dict_index_t &index)
{
  ut_ad(index.type & DICT_FTS);
  fts_table_t fts_table;
  char table_name[MAX_FULL_NAME_LEN];
  FTS_INIT_INDEX_TABLE(&fts_table, nullptr, FTS_INDEX_TABLE, (&index));
  for (const fts_index_selector_t *s= fts_index_selector; s->suffix; s++)
  {
    fts_table.suffix= s->suffix;
    fts_get_table_name(&fts_table, table_name, false);
    if (dberr_t err= fts_lock_table(trx, table_name))
      return err;
  }
  return DB_SUCCESS;
}

/** Lock the internal common FTS_ tables, before fts_drop_common_tables().
@param trx    transaction
@param table  table containing FULLTEXT INDEX
@return DB_SUCCESS or error code */
dberr_t fts_lock_common_tables(trx_t *trx, const dict_table_t &table)
{
  fts_table_t fts_table;
  char table_name[MAX_FULL_NAME_LEN];

  FTS_INIT_FTS_TABLE(&fts_table, nullptr, FTS_COMMON_TABLE, (&table));

  for (const char **suffix= fts_common_tables; *suffix; suffix++)
  {
    fts_table.suffix= *suffix;
    fts_get_table_name(&fts_table, table_name, false);
    if (dberr_t err= fts_lock_table(trx, table_name))
      return err;
  }
  return DB_SUCCESS;
}

/** This function make sure that table doesn't
have any other reference count.
@param	table_name	table name */
static void fts_table_no_ref_count(const char *table_name)
{
  dict_table_t *table= dict_table_open_on_name(
    table_name, true, DICT_ERR_IGNORE_TABLESPACE);
  if (!table)
    return;

  while (table->get_ref_count() > 1)
  {
    dict_sys.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dict_sys.lock(SRW_LOCK_CALL);
  }

  table->release();
}

/** Stop the purge thread and check n_ref_count of all auxiliary
and common table associated with the fts table.
@param	table		parent FTS table
@param	already_stopped	True indicates purge threads were
			already stopped*/
void purge_sys_t::stop_FTS(const dict_table_t &table, bool already_stopped)
{
  if (!already_stopped)
    purge_sys.stop_FTS();

  dict_sys.lock(SRW_LOCK_CALL);

  fts_table_t fts_table;
  char table_name[MAX_FULL_NAME_LEN];

  FTS_INIT_FTS_TABLE(&fts_table, nullptr, FTS_COMMON_TABLE, (&table));

  for (const char **suffix= fts_common_tables; *suffix; suffix++)
  {
    fts_table.suffix= *suffix;
    fts_get_table_name(&fts_table, table_name, true);
    fts_table_no_ref_count(table_name);
  }

  if (table.fts)
  {
    if (auto indexes= table.fts->indexes)
    {
      for (ulint i= 0;i < ib_vector_size(indexes); ++i)
      {
        const dict_index_t *index= static_cast<const dict_index_t*>(
          ib_vector_getp(indexes, i));
        FTS_INIT_INDEX_TABLE(&fts_table, nullptr, FTS_INDEX_TABLE, index);
        for (const fts_index_selector_t *s= fts_index_selector;
             s->suffix; s++)
        {
          fts_table.suffix= s->suffix;
          fts_get_table_name(&fts_table, table_name, true);
          fts_table_no_ref_count(table_name);
        }
      }
    }
  }

  dict_sys.unlock();
}

/** Lock the internal FTS_ tables for table, before fts_drop_tables().
@param trx    transaction
@param table  table containing FULLTEXT INDEX
@return DB_SUCCESS or error code */
dberr_t fts_lock_tables(trx_t *trx, const dict_table_t &table)
{
  if (dberr_t err= fts_lock_common_tables(trx, table))
    return err;

  if (!table.fts)
    return DB_SUCCESS;

  auto indexes= table.fts->indexes;
  if (!indexes)
    return DB_SUCCESS;

  for (ulint i= 0; i < ib_vector_size(indexes); ++i)
    if (dberr_t err=
        fts_lock_index_tables(trx, *static_cast<const dict_index_t*>
                              (ib_vector_getp(indexes, i))))
      return err;
  return DB_SUCCESS;
}

/** Drops the common ancillary tables needed for supporting an FTS index
on the given table.
@param trx          transaction to drop fts common table
@param fts_table    table with an FTS index
@param rename       whether to rename before dropping
@return DB_SUCCESS or error code */
static dberr_t fts_drop_common_tables(trx_t *trx, fts_table_t *fts_table,
                                      bool rename)
{
  dberr_t error= DB_SUCCESS;

  for (ulint i= 0; fts_common_tables[i]; ++i)
  {
    char table_name[MAX_FULL_NAME_LEN];

    fts_table->suffix= fts_common_tables[i];
    fts_get_table_name(fts_table, table_name, true);

    if (dberr_t err= fts_drop_table(trx, table_name, rename))
    {
      if (trx->state != TRX_STATE_ACTIVE)
        return err;
      /* We only return the status of the last error. */
      if (err != DB_FAIL)
        error= err;
    }
  }

  return error;
}

/****************************************************************//**
Drops FTS auxiliary tables for an FTS index
@return DB_SUCCESS or error code */
dberr_t fts_drop_index_tables(trx_t *trx, const dict_index_t &index)
{
	ulint		i;
	fts_table_t	fts_table;
	dberr_t		error = DB_SUCCESS;

	FTS_INIT_INDEX_TABLE(&fts_table, nullptr, FTS_INDEX_TABLE, (&index));

	for (i = 0; i < FTS_NUM_AUX_INDEX; ++i) {
		dberr_t	err;
		char	table_name[MAX_FULL_NAME_LEN];

		fts_table.suffix = fts_get_suffix(i);
		fts_get_table_name(&fts_table, table_name, true);

		err = fts_drop_table(trx, table_name, false);

		/* We only return the status of the last error. */
		if (err != DB_SUCCESS && err != DB_FAIL) {
			error = err;
		}
	}

	return(error);
}

/****************************************************************//**
Drops FTS ancillary tables needed for supporting an FTS index
on the given table.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_drop_all_index_tables(
/*======================*/
	trx_t*		trx,			/*!< in: transaction */
	const fts_t*	fts)			/*!< in: fts instance */
{
  dberr_t error= DB_SUCCESS;
  auto indexes= fts->indexes;
  if (!indexes)
    return DB_SUCCESS;

  for (ulint i= 0; i < ib_vector_size(indexes); ++i)
    if (dberr_t err= fts_drop_index_tables(trx,
                                           *static_cast<const dict_index_t*>
                                           (ib_vector_getp(indexes, i))))
      error= err;
  return error;
}

/** Drop the internal FTS_ tables for table.
@param trx    transaction
@param table  table containing FULLTEXT INDEX
@return DB_SUCCESS or error code */
dberr_t fts_drop_tables(trx_t *trx, const dict_table_t &table)
{
	dberr_t		error;
	fts_table_t	fts_table;

	FTS_INIT_FTS_TABLE(&fts_table, NULL, FTS_COMMON_TABLE, (&table));

	error = fts_drop_common_tables(trx, &fts_table, false);

	if (error == DB_SUCCESS && table.fts) {
		error = fts_drop_all_index_tables(trx, table.fts);
	}

	return(error);
}

/** Create dict_table_t object for FTS Aux tables.
@param[in]	aux_table_name	FTS Aux table name
@param[in]	table		table object of FTS Index
@param[in]	n_cols		number of columns for FTS Aux table
@return table object for FTS Aux table */
static
dict_table_t*
fts_create_in_mem_aux_table(
	const char*		aux_table_name,
	const dict_table_t*	table,
	ulint			n_cols)
{
	dict_table_t*	new_table = dict_table_t::create(
		{aux_table_name,strlen(aux_table_name)},
		nullptr, n_cols, 0, table->flags,
		table->space_id == TRX_SYS_SPACE
		? 0 : table->space_id == SRV_TMP_SPACE_ID
		? DICT_TF2_TEMPORARY : DICT_TF2_USE_FILE_PER_TABLE);

	if (DICT_TF_HAS_DATA_DIR(table->flags)) {
		ut_ad(table->data_dir_path != NULL);
		new_table->data_dir_path = mem_heap_strdup(
			new_table->heap, table->data_dir_path);
	}

	return(new_table);
}

/** Function to create on FTS common table.
@param[in,out]	trx		InnoDB transaction
@param[in]	table		Table that has FTS Index
@param[in]	fts_table_name	FTS AUX table name
@param[in]	fts_suffix	FTS AUX table suffix
@param[in,out]	heap		temporary memory heap
@return table object if created, else NULL */
static
dict_table_t*
fts_create_one_common_table(
	trx_t*			trx,
	const dict_table_t*	table,
	const char*		fts_table_name,
	const char*		fts_suffix,
	mem_heap_t*		heap)
{
	dict_table_t*		new_table;
	dberr_t			error;
	bool			is_config = strcmp(fts_suffix, "CONFIG") == 0;

	if (!is_config) {

		new_table = fts_create_in_mem_aux_table(
			fts_table_name, table, FTS_DELETED_TABLE_NUM_COLS);

		dict_mem_table_add_col(
			new_table, heap, "doc_id", DATA_INT, DATA_UNSIGNED,
			FTS_DELETED_TABLE_COL_LEN);
	} else {
		/* Config table has different schema. */
		new_table = fts_create_in_mem_aux_table(
			fts_table_name, table, FTS_CONFIG_TABLE_NUM_COLS);

		dict_mem_table_add_col(
			new_table, heap, "key", DATA_VARCHAR, 0,
			FTS_CONFIG_TABLE_KEY_COL_LEN);

		dict_mem_table_add_col(
			new_table, heap, "value", DATA_VARCHAR, DATA_NOT_NULL,
			FTS_CONFIG_TABLE_VALUE_COL_LEN);
	}

	dict_table_add_system_columns(new_table, heap);
	error = row_create_table_for_mysql(new_table, trx);

	if (error == DB_SUCCESS) {

		dict_index_t*	index = dict_mem_index_create(
			new_table, "FTS_COMMON_TABLE_IND",
			DICT_UNIQUE|DICT_CLUSTERED, 1);

		if (!is_config) {
			dict_mem_index_add_field(index, "doc_id", 0);
		} else {
			dict_mem_index_add_field(index, "key", 0);
		}

		error =	row_create_index_for_mysql(index, trx, NULL,
						   FIL_ENCRYPTION_DEFAULT,
						   FIL_DEFAULT_ENCRYPTION_KEY);
		if (error == DB_SUCCESS) {
			return new_table;
		}
	}

	ut_ad(trx->state == TRX_STATE_NOT_STARTED
	      || trx->error_state == error);
	sql_print_warning("InnoDB: Failed to create FTS common table %s: %s",
			  fts_table_name, ut_strerr(error));
	return NULL;
}

/** Creates the common auxiliary tables needed for supporting an FTS index
on the given table.
The following tables are created.
CREATE TABLE $FTS_PREFIX_DELETED
	(doc_id BIGINT UNSIGNED, UNIQUE CLUSTERED INDEX on doc_id)
CREATE TABLE $FTS_PREFIX_DELETED_CACHE
	(doc_id BIGINT UNSIGNED, UNIQUE CLUSTERED INDEX on doc_id)
CREATE TABLE $FTS_PREFIX_BEING_DELETED
	(doc_id BIGINT UNSIGNED, UNIQUE CLUSTERED INDEX on doc_id)
CREATE TABLE $FTS_PREFIX_BEING_DELETED_CACHE
	(doc_id BIGINT UNSIGNED, UNIQUE CLUSTERED INDEX on doc_id)
CREATE TABLE $FTS_PREFIX_CONFIG
	(key CHAR(50), value CHAR(200), UNIQUE CLUSTERED INDEX on key)
@param[in,out]	trx			transaction
@param[in,out]	table			table with FTS index
@param[in]	skip_doc_id_index	Skip index on doc id
@return DB_SUCCESS if succeed */
dberr_t
fts_create_common_tables(
	trx_t*		trx,
	dict_table_t*	table,
	bool		skip_doc_id_index)
{
	dberr_t		err;
	fts_table_t	fts_table;
	char		full_name[sizeof(fts_common_tables) / sizeof(char*)]
				[MAX_FULL_NAME_LEN];

	dict_index_t*	index = NULL;
	dict_table_t*   fts_config= nullptr;

	FTS_INIT_FTS_TABLE(&fts_table, NULL, FTS_COMMON_TABLE, table);

	err = fts_drop_common_tables(trx, &fts_table, true);

	if (err != DB_SUCCESS) {
		return err;
	}

	FTSQueryRunner sqlRunner(trx);
	mem_heap_t* heap = sqlRunner.heap();

	/* Create the FTS tables that are common to an FTS index. */
	for (ulint i = 0; fts_common_tables[i] != NULL; ++i) {

		fts_table.suffix = fts_common_tables[i];
		fts_get_table_name(&fts_table, full_name[i], true);
		dict_table_t*	common_table = fts_create_one_common_table(
			trx, table, full_name[i], fts_table.suffix, heap);

		if (!common_table) {
			trx->error_state = DB_SUCCESS;
			err = DB_ERROR;
			goto func_exit;
		}

		mem_heap_empty(heap);
	}

	fts_table.suffix = "CONFIG";

	fts_config= sqlRunner.open_table(&fts_table, &err, true);
	if (fts_config == nullptr)
          goto func_exit;


	err = sqlRunner.prepare_for_write(fts_config);
	if (err) goto func_exit;

	ut_ad(UT_LIST_GET_LEN(fts_config->indexes) == 1);
	sqlRunner.build_tuple(dict_table_get_first_index(fts_config));

	/* Following commands are executed:
	INSERT INTO $config_table VALUES("FTS_MAX_CACHE_SIZE_IN_MB", "256");
	INSERT INTO $config_table VALUES("FTS_OPTIMIZE_LIMIT_IN_SECS","180");
	INSERT INTO $config_table VALUES ("FTS_SYNCED_DOC_ID", "0");
	INSERT INTO $config_table VALUES ("FTS_TOTAL_DELETED_COUNT","0");
	Note: 0 == FTS_TABLE_STATE_RUNNING
	INSERT INTO $config_table VALUES ("FTS_TABLE_STATE","0"); */
	sqlRunner.assign_config_fields(FTS_MAX_CACHE_SIZE_IN_MB, "256", 3);
	err = sqlRunner.write_record(fts_config);

	if (err) goto func_exit;

	sqlRunner.assign_config_fields(FTS_OPTIMIZE_LIMIT_IN_SECS, "180", 3);
	err = sqlRunner.write_record(fts_config);

	if (err) goto func_exit;

	sqlRunner.assign_config_fields(FTS_SYNCED_DOC_ID, "0", 1);
	err = sqlRunner.write_record(fts_config);

	if (err) goto func_exit;

	sqlRunner.assign_config_fields(FTS_TOTAL_DELETED_COUNT, "0", 1);
	err = sqlRunner.write_record(fts_config);

	if (err) goto func_exit;

	sqlRunner.assign_config_fields(FTS_TABLE_STATE, "0", 1);
	err = sqlRunner.write_record(fts_config);

	if (err || skip_doc_id_index) goto func_exit;

	if (table->versioned()) {
		index = dict_mem_index_create(table,
					      FTS_DOC_ID_INDEX.str,
					      DICT_UNIQUE, 2);
		dict_mem_index_add_field(index, FTS_DOC_ID.str, 0);
		dict_mem_index_add_field(
		  index, table->cols[table->vers_end].name(*table).str, 0);
	} else {
		index = dict_mem_index_create(table,
					      FTS_DOC_ID_INDEX.str,
					      DICT_UNIQUE, 1);
		dict_mem_index_add_field(index, FTS_DOC_ID.str, 0);
	}

	err = row_create_index_for_mysql(index, trx, NULL,
	                                 FIL_ENCRYPTION_DEFAULT,
					 FIL_DEFAULT_ENCRYPTION_KEY);
func_exit:
	if (fts_config) fts_config->release();
	return err;
}

/** Create one FTS auxiliary index table for an FTS index.
@param[in,out]	trx		transaction
@param[in]	index		the index instance
@param[in]	fts_table	fts_table structure
@param[in,out]	heap		temporary memory heap
@see row_merge_create_fts_sort_index()
@return DB_SUCCESS or error code */
static
dict_table_t*
fts_create_one_index_table(
	trx_t*			trx,
	const dict_index_t*	index,
	const fts_table_t*	fts_table,
	mem_heap_t*		heap)
{
	dict_field_t*		field;
	dict_table_t*		new_table;
	char			table_name[MAX_FULL_NAME_LEN];
	dberr_t			error;
	CHARSET_INFO*		charset;

	ut_ad(index->type & DICT_FTS);

	fts_get_table_name(fts_table, table_name, true);

	new_table = fts_create_in_mem_aux_table(
			table_name, fts_table->table,
			FTS_AUX_INDEX_TABLE_NUM_COLS);

	field = dict_index_get_nth_field(index, 0);
	charset = fts_get_charset(field->col->prtype);

	dict_mem_table_add_col(new_table, heap, "word",
			       charset == &my_charset_latin1
			       ? DATA_VARCHAR : DATA_VARMYSQL,
			       field->col->prtype,
			       FTS_MAX_WORD_LEN_IN_CHAR
			       * unsigned(field->col->mbmaxlen));

	dict_mem_table_add_col(new_table, heap, "first_doc_id", DATA_INT,
			       DATA_NOT_NULL | DATA_UNSIGNED,
			       FTS_INDEX_FIRST_DOC_ID_LEN);

	dict_mem_table_add_col(new_table, heap, "last_doc_id", DATA_INT,
			       DATA_NOT_NULL | DATA_UNSIGNED,
			       FTS_INDEX_LAST_DOC_ID_LEN);

	dict_mem_table_add_col(new_table, heap, "doc_count", DATA_INT,
			       DATA_NOT_NULL | DATA_UNSIGNED,
			       FTS_INDEX_DOC_COUNT_LEN);

	/* The precise type calculation is as follows:
	least signficiant byte: MySQL type code (not applicable for sys cols)
	second least : DATA_NOT_NULL | DATA_BINARY_TYPE
	third least  : the MySQL charset-collation code (DATA_MTYPE_MAX) */

	dict_mem_table_add_col(
		new_table, heap, "ilist", DATA_BLOB,
		(DATA_MTYPE_MAX << 16) | DATA_UNSIGNED | DATA_NOT_NULL,
		FTS_INDEX_ILIST_LEN);

	dict_table_add_system_columns(new_table, heap);
	error = row_create_table_for_mysql(new_table, trx);

	if (error == DB_SUCCESS) {
		dict_index_t*	index = dict_mem_index_create(
			new_table, "FTS_INDEX_TABLE_IND",
			DICT_UNIQUE|DICT_CLUSTERED, 2);
		dict_mem_index_add_field(index, "word", 0);
		dict_mem_index_add_field(index, "first_doc_id", 0);

		error =	row_create_index_for_mysql(index, trx, NULL,
						   FIL_ENCRYPTION_DEFAULT,
						   FIL_DEFAULT_ENCRYPTION_KEY);

		if (error == DB_SUCCESS) {
			return new_table;
		}
	}

	ut_ad(trx->state == TRX_STATE_NOT_STARTED
	      || trx->error_state == error);
	sql_print_warning("InnoDB: Failed to create FTS index table %s: %s",
			  table_name, ut_strerr(error));
	return NULL;
}

/** Creates the column specific ancillary tables needed for supporting an
FTS index on the given table.

All FTS AUX Index tables have the following schema.
CREAT TABLE $FTS_PREFIX_INDEX_[1-6](
	word		VARCHAR(FTS_MAX_WORD_LEN),
	first_doc_id	INT NOT NULL,
	last_doc_id	UNSIGNED NOT NULL,
	doc_count	UNSIGNED INT NOT NULL,
	ilist		VARBINARY NOT NULL,
	UNIQUE CLUSTERED INDEX ON (word, first_doc_id))
@param[in,out]	trx	dictionary transaction
@param[in]	index	fulltext index
@param[in]	id	table id
@return DB_SUCCESS or error code */
dberr_t
fts_create_index_tables(trx_t* trx, const dict_index_t* index, table_id_t id)
{
	ulint		i;
	fts_table_t	fts_table;
	dberr_t		error = DB_SUCCESS;
	mem_heap_t*	heap = mem_heap_create(1024);

	fts_table.type = FTS_INDEX_TABLE;
	fts_table.index_id = index->id;
	fts_table.table_id = id;
	fts_table.table = index->table;

	for (i = 0; i < FTS_NUM_AUX_INDEX && error == DB_SUCCESS; ++i) {
		dict_table_t*	new_table;

		/* Create the FTS auxiliary tables that are specific
		to an FTS index. */
		fts_table.suffix = fts_get_suffix(i);

		new_table = fts_create_one_index_table(
			trx, index, &fts_table, heap);

		if (new_table == NULL) {
			error = DB_FAIL;
			break;
		}

		mem_heap_empty(heap);
	}

	mem_heap_free(heap);

	return(error);
}

/******************************************************************//**
Calculate the new state of a row given the existing state and a new event.
@return new state of row */
static
fts_row_state
fts_trx_row_get_new_state(
/*======================*/
	fts_row_state	old_state,		/*!< in: existing state of row */
	fts_row_state	event)			/*!< in: new event */
{
	/* The rules for transforming states:

	I = inserted
	M = modified
	D = deleted
	N = nothing

	M+D -> D:

	If the row existed before the transaction started and it is modified
	during the transaction, followed by a deletion of the row, only the
	deletion will be signaled.

	M+ -> M:

	If the row existed before the transaction started and it is modified
	more than once during the transaction, only the last modification
	will be signaled.

	IM*D -> N:

	If a new row is added during the transaction (and possibly modified
	after its initial insertion) but it is deleted before the end of the
	transaction, nothing will be signaled.

	IM* -> I:

	If a new row is added during the transaction and modified after its
	initial insertion, only the addition will be signaled.

	M*DI -> M:

	If the row existed before the transaction started and it is deleted,
	then re-inserted, only a modification will be signaled. Note that
	this case is only possible if the table is using the row's primary
	key for FTS row ids, since those can be re-inserted by the user,
	which is not true for InnoDB generated row ids.

	It is easily seen that the above rules decompose such that we do not
	need to store the row's entire history of events. Instead, we can
	store just one state for the row and update that when new events
	arrive. Then we can implement the above rules as a two-dimensional
	look-up table, and get checking of invalid combinations "for free"
	in the process. */

	/* The lookup table for transforming states. old_state is the
	Y-axis, event is the X-axis. */
	static const fts_row_state table[4][4] = {
			/*    I            M            D            N */
		/* I */	{ FTS_INVALID, FTS_INSERT,  FTS_NOTHING, FTS_INVALID },
		/* M */	{ FTS_INVALID, FTS_MODIFY,  FTS_DELETE,  FTS_INVALID },
		/* D */	{ FTS_MODIFY,  FTS_INVALID, FTS_INVALID, FTS_INVALID },
		/* N */	{ FTS_INVALID, FTS_INVALID, FTS_INVALID, FTS_INVALID }
	};

	fts_row_state result;

	ut_a(old_state < FTS_INVALID);
	ut_a(event < FTS_INVALID);

	result = table[(int) old_state][(int) event];
	ut_a(result != FTS_INVALID);

	return(result);
}

/** Compare two doubly indirected pointers */
static int fts_ptr2_cmp(const void *p1, const void *p2)
{
  const void *a= **static_cast<const void*const*const*>(p1);
  const void *b= **static_cast<const void*const*const*>(p2);
  return b > a ? -1 : a > b;
}

/** Compare a singly indirected pointer to a doubly indirected one */
static int fts_ptr1_ptr2_cmp(const void *p1, const void *p2)
{
  const void *a= *static_cast<const void*const*>(p1);
  const void *b= **static_cast<const void*const*const*>(p2);
  return b > a ? -1 : a > b;
}

/******************************************************************//**
Create a savepoint instance.
@return savepoint instance */
static
fts_savepoint_t*
fts_savepoint_create(
/*=================*/
	ib_vector_t*	savepoints,		/*!< out: InnoDB transaction */
	const void*	name,			/*!< in: savepoint */
	mem_heap_t*	heap)			/*!< in: heap */
{
	fts_savepoint_t*	savepoint;

	savepoint = static_cast<fts_savepoint_t*>(
		ib_vector_push(savepoints, NULL));

	memset(savepoint, 0x0, sizeof(*savepoint));
	savepoint->name = name;
	static_assert(!offsetof(fts_trx_table_t, table), "ABI");
	savepoint->tables = rbt_create(sizeof(fts_trx_table_t*), fts_ptr2_cmp);

	return(savepoint);
}

/******************************************************************//**
Create an FTS trx.
@return FTS trx */
fts_trx_t*
fts_trx_create(
/*===========*/
	trx_t*	trx)				/*!< in/out: InnoDB
						transaction */
{
	fts_trx_t*		ftt;
	ib_alloc_t*		heap_alloc;
	mem_heap_t*		heap = mem_heap_create(1024);

	ut_a(trx->fts_trx == NULL);

	ftt = static_cast<fts_trx_t*>(mem_heap_alloc(heap, sizeof(fts_trx_t)));
	ftt->trx = trx;
	ftt->heap = heap;

	heap_alloc = ib_heap_allocator_create(heap);

	ftt->savepoints = static_cast<ib_vector_t*>(ib_vector_create(
		heap_alloc, sizeof(fts_savepoint_t), 4));

	ftt->last_stmt = static_cast<ib_vector_t*>(ib_vector_create(
		heap_alloc, sizeof(fts_savepoint_t), 4));

	/* Default instance has no name and no heap. */
	fts_savepoint_create(ftt->savepoints, NULL, NULL);
	fts_savepoint_create(ftt->last_stmt, NULL, NULL);

	return(ftt);
}

/** Compare two doc_id */
static inline int doc_id_cmp(doc_id_t a, doc_id_t b)
{
  return b > a ? -1 : a > b;
}

/** Compare two DOC_ID. */
int fts_doc_id_cmp(const void *p1, const void *p2)
{
  return doc_id_cmp(*static_cast<const doc_id_t*>(p1),
                    *static_cast<const doc_id_t*>(p2));
}

/******************************************************************//**
Create an FTS trx table.
@return FTS trx table */
static
fts_trx_table_t*
fts_trx_table_create(
/*=================*/
	fts_trx_t*	fts_trx,		/*!< in: FTS trx */
	dict_table_t*	table)			/*!< in: table */
{
	fts_trx_table_t*	ftt;

	ftt = static_cast<fts_trx_table_t*>(
		mem_heap_zalloc(fts_trx->heap, sizeof *ftt));

	ftt->table = table;
	ftt->fts_trx = fts_trx;

	static_assert(!offsetof(fts_trx_row_t, doc_id), "ABI");
	ftt->rows = rbt_create(sizeof(fts_trx_row_t), fts_doc_id_cmp);

	return(ftt);
}

/******************************************************************//**
Clone an FTS trx table.
@return FTS trx table */
static
fts_trx_table_t*
fts_trx_table_clone(
/*=================*/
	const fts_trx_table_t*	ftt_src)	/*!< in: FTS trx */
{
	fts_trx_table_t*	ftt;

	ftt = static_cast<fts_trx_table_t*>(
		mem_heap_alloc(ftt_src->fts_trx->heap, sizeof(*ftt)));

	memset(ftt, 0x0, sizeof(*ftt));

	ftt->table = ftt_src->table;
	ftt->fts_trx = ftt_src->fts_trx;

	static_assert(!offsetof(fts_trx_row_t, doc_id), "ABI");
	ftt->rows = rbt_create(sizeof(fts_trx_row_t), fts_doc_id_cmp);

	/* Copy the rb tree values to the new savepoint. */
	rbt_merge_uniq(ftt->rows, ftt_src->rows);

	/* These are only added on commit. At this stage we only have
	the updated row state. */
	ut_a(ftt_src->added_doc_ids == NULL);

	return(ftt);
}

/******************************************************************//**
Initialize the FTS trx instance.
@return FTS trx instance */
static
fts_trx_table_t*
fts_trx_init(
/*=========*/
	trx_t*			trx,		/*!< in: transaction */
	dict_table_t*		table,		/*!< in: FTS table instance */
	ib_vector_t*		savepoints)	/*!< in: Savepoints */
{
	fts_trx_table_t*	ftt;
	ib_rbt_bound_t		parent;
	ib_rbt_t* tables = static_cast<fts_savepoint_t*>(
		ib_vector_last(savepoints))->tables;
	rbt_search_cmp(tables, &parent, &table, fts_ptr1_ptr2_cmp, nullptr);

	if (parent.result == 0) {
		fts_trx_table_t**	fttp;

		fttp = rbt_value(fts_trx_table_t*, parent.last);
		ftt = *fttp;
	} else {
		ftt = fts_trx_table_create(trx->fts_trx, table);
		rbt_add_node(tables, &parent, &ftt);
	}

	ut_a(ftt->table == table);

	return(ftt);
}

/******************************************************************//**
Notify the FTS system about an operation on an FTS-indexed table. */
static
void
fts_trx_table_add_op(
/*=================*/
	fts_trx_table_t*ftt,			/*!< in: FTS trx table */
	doc_id_t	doc_id,			/*!< in: doc id */
	fts_row_state	state,			/*!< in: state of the row */
	ib_vector_t*	fts_indexes)		/*!< in: FTS indexes affected */
{
	ib_rbt_t*	rows;
	ib_rbt_bound_t	parent;

	rows = ftt->rows;
	rbt_search(rows, &parent, &doc_id);

	/* Row id found, update state, and if new state is FTS_NOTHING,
	we delete the row from our tree. */
	if (parent.result == 0) {
		fts_trx_row_t*	row = rbt_value(fts_trx_row_t, parent.last);

		row->state = fts_trx_row_get_new_state(row->state, state);

		if (row->state == FTS_NOTHING) {
			if (row->fts_indexes) {
				ib_vector_free(row->fts_indexes);
			}

			ut_free(rbt_remove_node(rows, parent.last));
			row = NULL;
		} else if (row->fts_indexes != NULL) {
			ib_vector_free(row->fts_indexes);
			row->fts_indexes = fts_indexes;
		}

	} else { /* Row-id not found, create a new one. */
		fts_trx_row_t	row;

		row.doc_id = doc_id;
		row.state = state;
		row.fts_indexes = fts_indexes;

		rbt_add_node(rows, &parent, &row);
	}
}

/******************************************************************//**
Notify the FTS system about an operation on an FTS-indexed table. */
void
fts_trx_add_op(
/*===========*/
	trx_t*		trx,			/*!< in: InnoDB transaction */
	dict_table_t*	table,			/*!< in: table */
	doc_id_t	doc_id,			/*!< in: new doc id */
	fts_row_state	state,			/*!< in: state of the row */
	ib_vector_t*	fts_indexes)		/*!< in: FTS indexes affected
						(NULL=all) */
{
	fts_trx_table_t*	tran_ftt;
	fts_trx_table_t*	stmt_ftt;

	if (!trx->fts_trx) {
		trx->fts_trx = fts_trx_create(trx);
	}

	tran_ftt = fts_trx_init(trx, table, trx->fts_trx->savepoints);
	stmt_ftt = fts_trx_init(trx, table, trx->fts_trx->last_stmt);

	fts_trx_table_add_op(tran_ftt, doc_id, state, fts_indexes);
	fts_trx_table_add_op(stmt_ftt, doc_id, state, fts_indexes);
}

/** Fetch callback that converts a textual document id to a binary value and
stores it in the given place.
@return always returns NULL */
static bool fts_fetch_store_doc_id(dict_index_t *index, const rec_t *rec,
                                   const rec_offs *offsets, void *user_arg)
{
  ut_ad(dict_index_is_clust(index));
  doc_id_t *doc_id= static_cast<doc_id_t*>(user_arg);

  for (uint32_t i= 0; i < index->n_fields; i++)
  {
    if (strcmp(index->fields[i].name, "value") != 0)
      continue;

    ulint len;
    const byte *data= rec_get_nth_field(rec, offsets, i, &len);
    ut_ad(len != 8);
    char buf[32];
    memcpy(buf, data, len);
    buf[len]= '\0';

    return sscanf(buf, FTS_DOC_ID_FORMAT, doc_id) == 1;
  }
  return false;
}

#ifdef FTS_CACHE_SIZE_DEBUG
/******************************************************************//**
Get the max cache size in bytes. If there is an error reading the
value we simply print an error message here and return the default
value to the caller.
@return max cache size in bytes */
static
ulint
fts_get_max_cache_size(
/*===================*/
	trx_t*		trx,			/*!< in: transaction */
	fts_table_t*	fts_table)		/*!< in: table instance */
{
	dberr_t		error;
	fts_string_t	value;
	ulong		cache_size_in_mb;

	/* Set to the default value. */
	cache_size_in_mb = FTS_CACHE_SIZE_LOWER_LIMIT_IN_MB;

	/* We set the length of value to the max bytes it can hold. This
	information is used by the callback that reads the value. */
	value.f_n_char = 0;
	value.f_len = FTS_MAX_CONFIG_VALUE_LEN;
	value.f_str = ut_malloc_nokey(value.f_len + 1);

	error = fts_config_get_value(
		trx, fts_table, FTS_MAX_CACHE_SIZE_IN_MB, &value);

	if (UNIV_LIKELY(error == DB_SUCCESS)) {
		value.f_str[value.f_len] = 0;
		cache_size_in_mb = strtoul((char*) value.f_str, NULL, 10);

		if (cache_size_in_mb > FTS_CACHE_SIZE_UPPER_LIMIT_IN_MB) {

			ib::warn() << "FTS max cache size ("
				<< cache_size_in_mb << ") out of range."
				" Minimum value is "
				<< FTS_CACHE_SIZE_LOWER_LIMIT_IN_MB
				<< "MB and the maximum value is "
				<< FTS_CACHE_SIZE_UPPER_LIMIT_IN_MB
				<< "MB, setting cache size to upper limit";

			cache_size_in_mb = FTS_CACHE_SIZE_UPPER_LIMIT_IN_MB;

		} else if  (cache_size_in_mb
			    < FTS_CACHE_SIZE_LOWER_LIMIT_IN_MB) {

			ib::warn() << "FTS max cache size ("
				<< cache_size_in_mb << ") out of range."
				" Minimum value is "
				<< FTS_CACHE_SIZE_LOWER_LIMIT_IN_MB
				<< "MB and the maximum value is"
				<< FTS_CACHE_SIZE_UPPER_LIMIT_IN_MB
				<< "MB, setting cache size to lower limit";

			cache_size_in_mb = FTS_CACHE_SIZE_LOWER_LIMIT_IN_MB;
		}
	} else {
		ib::error() << "(" << error << ") reading max"
			" cache config value from config table "
			<< fts_table->table->name;
	}

	ut_free(value.f_str);

	return(cache_size_in_mb * 1024 * 1024);
}
#endif

/*********************************************************************//**
Get the next available document id.
@return DB_SUCCESS if OK */
dberr_t
fts_get_next_doc_id(
/*================*/
	const dict_table_t*	table,		/*!< in: table */
	doc_id_t*		doc_id)		/*!< out: new document id */
{
	fts_cache_t*	cache = table->fts->cache;

	/* If the Doc ID system has not yet been initialized, we
	will consult the CONFIG table and user table to re-establish
	the initial value of the Doc ID */
	if (cache->first_doc_id == FTS_NULL_DOC_ID) {
		fts_init_doc_id(table);
	}

	if (!DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID)) {
		*doc_id = FTS_NULL_DOC_ID;
		return(DB_SUCCESS);
	}

	DEBUG_SYNC_C("get_next_FTS_DOC_ID");
	mysql_mutex_lock(&cache->doc_id_lock);
	*doc_id = cache->next_doc_id++;
	mysql_mutex_unlock(&cache->doc_id_lock);

	return(DB_SUCCESS);
}

/** Read the synced document id from the fts configuration table
@param table  fts table
@param doc_id document id to be read
@param trx    transaction to read from config table
@return DB_SUCCESS in case of success */
static
dberr_t fts_read_synced_doc_id(const dict_table_t *table,
                               doc_id_t *doc_id,
                               trx_t *trx)
{
  dberr_t err= DB_SUCCESS;
  fts_table_t fts_table;
  fts_table.suffix= "CONFIG";
  fts_table.table_id= table->id;
  fts_table.type= FTS_COMMON_TABLE;
  fts_table.table= table;
  ut_a(table->fts->doc_col != ULINT_UNDEFINED);

  trx->op_info = "update the next FTS document id";
  FTSQueryRunner sqlRunner(trx);

  dict_table_t *fts_config= sqlRunner.open_table(&fts_table, &err);
  if (fts_config)
  {
    err= sqlRunner.prepare_for_write(fts_config);
    if (err == DB_SUCCESS)
    {
      ut_ad(UT_LIST_GET_LEN(fts_config->indexes) == 1);
      dict_index_t *clust_index= dict_table_get_first_index(fts_config);
      sqlRunner.build_tuple(clust_index, 1, 1);
      sqlRunner.assign_config_fields("synced_doc_id");
      err= sqlRunner.record_executor(clust_index, SELECT_UPDATE,
                                     MATCH_UNIQUE, PAGE_CUR_GE,
                                     &fts_fetch_store_doc_id,
                                     doc_id);
      if (err == DB_END_OF_INDEX || err == DB_RECORD_NOT_FOUND)
        err= DB_SUCCESS;
    }
    fts_config->release();
  }
  return err;
}

/** This function fetch the Doc ID from CONFIG table, and compare with
the Doc ID supplied. And store the larger one to the CONFIG table.
@param table fts  table
@param cmp_doc_id Doc ID to compare
@param doc_id     larger document id after comparing "cmp_doc_id" to
	          the one stored in CONFIG table
@param trx	  transaction
@return DB_SUCCESS if OK */
static
dberr_t
fts_cmp_set_sync_doc_id(
	const dict_table_t *table,
	doc_id_t           cmp_doc_id,
	doc_id_t           *doc_id,
	trx_t	           *trx=nullptr)
{
	if (srv_read_only_mode) {
		return DB_READ_ONLY;
	}

	fts_cache_t*	cache= table->fts->cache;
	dberr_t 	error = DB_SUCCESS;
	const trx_t*	const caller_trx = trx;

	if (trx == nullptr) {
		trx = trx_create();
		trx_start_internal_read_only(trx);
	}
retry:
	error = fts_read_synced_doc_id(table, doc_id, trx);

	if (error != DB_SUCCESS) goto func_exit;

	if (cmp_doc_id == 0 && *doc_id) {
		cache->synced_doc_id = *doc_id - 1;
	} else {
		cache->synced_doc_id = ut_max(cmp_doc_id, *doc_id);
	}

	mysql_mutex_lock(&cache->doc_id_lock);
	/* For each sync operation, we will add next_doc_id by 1,
	so to mark a sync operation */
	if (cache->next_doc_id < cache->synced_doc_id + 1) {
		cache->next_doc_id = cache->synced_doc_id + 1;
	}
	mysql_mutex_unlock(&cache->doc_id_lock);

	if (cmp_doc_id && cmp_doc_id >= *doc_id) {
		error = fts_update_sync_doc_id(
			table, cache->synced_doc_id, trx);
	}

	*doc_id = cache->next_doc_id;

func_exit:

	if (caller_trx) {
		return error;
	}

	if (UNIV_LIKELY(error == DB_SUCCESS)) {
		fts_sql_commit(trx);
	} else {
		*doc_id = 0;

		ib::error() << "(" << error << ") while getting next doc id "
			"for table " << table->name;

		fts_sql_rollback(trx);

		if (error == DB_DEADLOCK || error == DB_LOCK_WAIT_TIMEOUT) {
			DEBUG_SYNC_C("fts_cmp_set_sync_doc_id_retry");
			std::this_thread::sleep_for(FTS_DEADLOCK_RETRY_WAIT);
			goto retry;
		}
	}

	trx->free();

	return(error);
}

/** Update the last document id. This function could create a new
transaction to update the last document id.
@param  table   table to be updated
@param  doc_id  last document id
@param  trx     update trx or null
@retval DB_SUCCESS if OK */
dberr_t fts_update_sync_doc_id(const dict_table_t *table, doc_id_t doc_id,
                               trx_t *trx)
{
  fts_table_t fts_table;
  dberr_t err= DB_SUCCESS;
  bool local_trx= false;
  fts_cache_t *cache = table->fts->cache;

  if (srv_read_only_mode) return DB_READ_ONLY;

  fts_table.suffix = "CONFIG";
  fts_table.table_id = table->id;
  fts_table.type = FTS_COMMON_TABLE;
  fts_table.table = table;

  if (!trx)
  {
    trx= trx_create();
    trx_start_internal(trx);
    trx->op_info= "setting last FTS document id";
    local_trx= true;
  }

  FTSQueryRunner sqlRunner(trx);
  dict_table_t *fts_config= sqlRunner.open_table(&fts_table, &err);
  if (fts_config)
  {
    err= sqlRunner.prepare_for_write(fts_config);
    if (err == DB_SUCCESS)
    {
      ut_ad(UT_LIST_GET_LEN(fts_config->indexes) == 1);
      dict_index_t *clust_index= dict_table_get_first_index(fts_config);
      sqlRunner.build_tuple(clust_index, 1, 1);
      sqlRunner.assign_config_fields("synced_doc_id");
      fts_string_t old_value;
      fts_string_t new_value;

      old_value.f_len= FTS_MAX_ID_LEN;
      old_value.f_str=
        static_cast<byte*>(ut_malloc_nokey(old_value.f_len + 1));

      err= sqlRunner.record_executor(clust_index, SELECT_UPDATE, MATCH_UNIQUE,
                                     PAGE_CUR_GE, &read_fts_config, &old_value);

      ut_a(err != DB_END_OF_INDEX || err != DB_RECORD_NOT_FOUND);

      if (err != DB_SUCCESS)
      {
        ut_free(old_value.f_str);
        goto func_exit;
      }

      /* Construct new value for the record to be updated */
      byte id[FTS_MAX_ID_LEN];
      ulint new_len= (ulint) snprintf((char*) id, sizeof(id),
                                      FTS_DOC_ID_FORMAT, doc_id + 1);

      new_value.f_len= new_len;
      new_value.f_str=
        static_cast<byte*>(ut_malloc_nokey(new_value.f_len + 1));
      memcpy(new_value.f_str, id, new_len);
      new_value.f_str[new_len]='\0';

      /* Update only if there is a change in new value */
      if (old_value.f_len != new_value.f_len ||
           memcmp(old_value.f_str, new_value.f_str, new_value.f_len) != 0)
      {
        sqlRunner.build_update_config(fts_config, 3, &new_value);
        err= sqlRunner.update_record(fts_config,
                                     static_cast<uint16_t>(old_value.f_len),
                                     static_cast<uint16_t>(new_len));
      }
      ut_free(new_value.f_str);
      ut_free(old_value.f_str);
    }
  }

func_exit:
  if (local_trx)
  {
    if (UNIV_LIKELY(err == DB_SUCCESS))
    {
      fts_sql_commit(trx);
      cache->synced_doc_id = doc_id;
    }
    else
    {
      ib::error() << "(" << err << ") while updating last doc id for table"
                  << table->name;
      fts_sql_rollback(trx);
    }
    trx->free();
  }

  if (fts_config) fts_config->release();
  return err;
}

/*********************************************************************//**
Create a new fts_doc_ids_t.
@return new fts_doc_ids_t */
fts_doc_ids_t*
fts_doc_ids_create(void)
/*====================*/
{
	fts_doc_ids_t*	fts_doc_ids;
	mem_heap_t*	heap = mem_heap_create(512);

	fts_doc_ids = static_cast<fts_doc_ids_t*>(
		mem_heap_alloc(heap, sizeof(*fts_doc_ids)));

	fts_doc_ids->self_heap = ib_heap_allocator_create(heap);

	fts_doc_ids->doc_ids = static_cast<ib_vector_t*>(ib_vector_create(
		fts_doc_ids->self_heap, sizeof(doc_id_t), 32));

	return(fts_doc_ids);
}

/*********************************************************************//**
Do commit-phase steps necessary for the insertion of a new row. */
void
fts_add(
/*====*/
	fts_trx_table_t*ftt,			/*!< in: FTS trx table */
	fts_trx_row_t*	row)			/*!< in: row */
{
	dict_table_t*	table = ftt->table;
	doc_id_t	doc_id = row->doc_id;

	ut_a(row->state == FTS_INSERT || row->state == FTS_MODIFY);

	fts_add_doc_by_id(ftt, doc_id);

	mysql_mutex_lock(&table->fts->cache->deleted_lock);
	++table->fts->cache->added;
	mysql_mutex_unlock(&table->fts->cache->deleted_lock);

	if (!DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID)
	    && doc_id >= table->fts->cache->next_doc_id) {
		table->fts->cache->next_doc_id = doc_id + 1;
	}
}

/*********************************************************************//**
Do commit-phase steps necessary for the deletion of a row.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_delete(
/*=======*/
	fts_trx_table_t*ftt,			/*!< in: FTS trx table */
	fts_trx_row_t*	row)			/*!< in: row */
{
	fts_table_t	fts_table;
	doc_id_t	write_doc_id;
	dict_table_t*	table = ftt->table;
	doc_id_t	doc_id = row->doc_id;
	trx_t*		trx = ftt->fts_trx->trx;
	fts_cache_t*	cache = table->fts->cache;

	/* we do not index Documents whose Doc ID value is 0 */
	if (doc_id == FTS_NULL_DOC_ID) {
		ut_ad(!DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID));
		return DB_SUCCESS;
	}

	ut_a(row->state == FTS_DELETE || row->state == FTS_MODIFY);

	FTS_INIT_FTS_TABLE(&fts_table, "DELETED", FTS_COMMON_TABLE, table);

	/* Convert to "storage" byte order. */
	fts_write_doc_id((byte*) &write_doc_id, doc_id);

	/* It is possible we update a record that has not yet been sync-ed
	into cache from last crash (delete Doc will not initialize the
	sync). Avoid any added counter accounting until the FTS cache
	is re-established and sync-ed */
	if (table->fts->added_synced
	    && doc_id > cache->synced_doc_id) {
		mysql_mutex_lock(&table->fts->cache->deleted_lock);

		/* The Doc ID could belong to those left in
		ADDED table from last crash. So need to check
		if it is less than first_doc_id when we initialize
		the Doc ID system after reboot */
		if (doc_id >= table->fts->cache->first_doc_id
		    && table->fts->cache->added > 0) {
			--table->fts->cache->added;
		}

		mysql_mutex_unlock(&table->fts->cache->deleted_lock);

		/* Only if the row was really deleted. */
		ut_a(row->state == FTS_DELETE || row->state == FTS_MODIFY);
	}

	trx->op_info = "adding doc id to FTS DELETED";
	fts_table.suffix = "DELETED";

	dberr_t err= DB_SUCCESS;
	FTSQueryRunner sqlRunner(trx);
	dict_table_t *fts_deleted= sqlRunner.open_table(&fts_table, &err);

	if (fts_deleted == nullptr) goto func_exit;

	err = sqlRunner.prepare_for_write(fts_deleted);
	if (err) {
		goto func_exit;
	}

	ut_ad(UT_LIST_GET_LEN(fts_deleted->indexes) == 1);
	sqlRunner.build_tuple(dict_table_get_first_index(fts_deleted));
	sqlRunner.assign_common_table_fields(&write_doc_id);
	/* INSERT INTO FTS_DELETED VALUES(:DOC_ID)  */
	err = sqlRunner.write_record(fts_deleted);
	if (err == DB_SUCCESS) {
		/* Increment the total deleted count, this is used to
		calculate the number of documents indexed. */
		mysql_mutex_lock(&table->fts->cache->deleted_lock);

		++table->fts->cache->deleted;

		mysql_mutex_unlock(&table->fts->cache->deleted_lock);
	}

func_exit:
	if (fts_deleted) {
		fts_deleted->release();
	}
	return err;
}

/*********************************************************************//**
Do commit-phase steps necessary for the modification of a row.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_modify(
/*=======*/
	fts_trx_table_t*	ftt,		/*!< in: FTS trx table */
	fts_trx_row_t*		row)		/*!< in: row */
{
	dberr_t	error;

	ut_a(row->state == FTS_MODIFY);

	error = fts_delete(ftt, row);

	if (error == DB_SUCCESS) {
		fts_add(ftt, row);
	}

	return(error);
}

/*********************************************************************//**
The given transaction is about to be committed; do whatever is necessary
from the FTS system's POV.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_commit_table(
/*=============*/
	fts_trx_table_t*	ftt)		/*!< in: FTS table to commit*/
{
	if (srv_read_only_mode) {
		return DB_READ_ONLY;
	}

	const ib_rbt_node_t*	node;
	ib_rbt_t*		rows;
	dberr_t			error = DB_SUCCESS;
	fts_cache_t*		cache = ftt->table->fts->cache;
	trx_t*			trx = trx_create();

	trx_start_internal(trx);

	rows = ftt->rows;

	ftt->fts_trx->trx = trx;

	if (cache->get_docs == NULL) {
		mysql_mutex_lock(&cache->init_lock);
		if (cache->get_docs == NULL) {
			cache->get_docs = fts_get_docs_create(cache);
		}
		mysql_mutex_unlock(&cache->init_lock);
	}

	for (node = rbt_first(rows);
	     node != NULL && error == DB_SUCCESS;
	     node = rbt_next(rows, node)) {

		fts_trx_row_t*	row = rbt_value(fts_trx_row_t, node);

		switch (row->state) {
		case FTS_INSERT:
			fts_add(ftt, row);
			break;

		case FTS_MODIFY:
			error = fts_modify(ftt, row);
			break;

		case FTS_DELETE:
			error = fts_delete(ftt, row);
			break;

		default:
			ut_error;
		}
	}

	fts_sql_commit(trx);

	trx->free();

	return(error);
}

/*********************************************************************//**
The given transaction is about to be committed; do whatever is necessary
from the FTS system's POV.
@return DB_SUCCESS or error code */
dberr_t
fts_commit(
/*=======*/
	trx_t*	trx)				/*!< in: transaction */
{
	const ib_rbt_node_t*	node;
	dberr_t			error;
	ib_rbt_t*		tables;
	fts_savepoint_t*	savepoint;

	savepoint = static_cast<fts_savepoint_t*>(
		ib_vector_last(trx->fts_trx->savepoints));
	tables = savepoint->tables;

	for (node = rbt_first(tables), error = DB_SUCCESS;
	     node != NULL && error == DB_SUCCESS;
	     node = rbt_next(tables, node)) {

		fts_trx_table_t**	ftt;

		ftt = rbt_value(fts_trx_table_t*, node);

		error = fts_commit_table(*ftt);
	}

	return(error);
}

/*********************************************************************//**
Initialize a document. */
void
fts_doc_init(
/*=========*/
	fts_doc_t*	doc)			/*!< in: doc to initialize */
{
	mem_heap_t*	heap = mem_heap_create(32);

	memset(doc, 0, sizeof(*doc));

	doc->self_heap = ib_heap_allocator_create(heap);
}

/*********************************************************************//**
Free document. */
void
fts_doc_free(
/*=========*/
	fts_doc_t*	doc)			/*!< in: document */
{
	mem_heap_t*	heap = static_cast<mem_heap_t*>(doc->self_heap->arg);

	if (doc->tokens) {
		rbt_free(doc->tokens);
	}

	ut_d(memset(doc, 0, sizeof(*doc)));

	mem_heap_free(heap);
}

/** Callback function for fetch that stores the text of an FTS document,
converting each column to UTF-16.
@param index    clustered index
@param rec      clustered index record
@param offsets  offsets for the record
@param user_arg points to fts_fetch_doc_id since it has field information
                to fetch and user_arg which points to fts_doc_t
@return always true */
bool fts_query_expansion_fetch_doc(dict_index_t *index, const rec_t *rec,
                                   const rec_offs *offsets, void *user_arg)
{
  ut_ad(dict_index_is_clust(index));
  fts_fetch_doc_id *fetch= static_cast<fts_fetch_doc_id*>(user_arg);
  fts_doc_t *result_doc= static_cast<fts_doc_t*>(fetch->user_arg);
  ulint	doc_len= 0;
  fts_doc_t doc;
  CHARSET_INFO *doc_charset = NULL;

  fts_doc_init(&doc);
  doc.found= TRUE;
  doc_charset= result_doc->charset;

  bool start_doc= true;
  for (uint32_t i= 0; i < fetch->field_to_fetch.size(); i++)
  {
    ulint field_no= fetch->field_to_fetch[i];
    if (!doc_charset)
      doc_charset = fts_get_charset(index->fields[field_no].col->prtype);
    doc.charset = doc_charset;

    /* We ignore columns that are stored externally, this
    could result in too many words to search */
    if (rec_offs_nth_extern(offsets, field_no)) continue;
    else
      doc.text.f_str = (byte*) rec_get_nth_field(rec, offsets, field_no,
                                                 &doc.text.f_len);
    if (start_doc)
    {
      fts_tokenize_document(&doc, result_doc, result_doc->parser);
      start_doc= false;
    }
    else fts_tokenize_document_next(&doc, ++doc_len, result_doc,
                                    result_doc->parser);
    doc_len+= doc.text.f_len;
  }

  ut_ad(doc_charset);
  if (!result_doc->charset) result_doc->charset = doc_charset;
  fts_doc_free(&doc);
  return true;
}

/*********************************************************************//**
fetch and tokenize the document. */
static
void
fts_fetch_doc_from_rec(
/*===================*/
	fts_get_doc_t*  get_doc,	/*!< in: FTS index's get_doc struct */
	dict_index_t*	clust_index,	/*!< in: cluster index */
	btr_pcur_t*	pcur,		/*!< in: cursor whose position
					has been stored */
	rec_offs*	offsets,	/*!< in: offsets */
	fts_doc_t*	doc)		/*!< out: fts doc to hold parsed
					documents */
{
	dict_index_t*		index;
	const rec_t*		clust_rec;
	const dict_field_t*	ifield;
	ulint			clust_pos;
	ulint			doc_len = 0;
	st_mysql_ftparser*	parser;

	if (!get_doc) {
		return;
	}

	index = get_doc->index_cache->index;
	parser = get_doc->index_cache->index->parser;

	clust_rec = btr_pcur_get_rec(pcur);
	ut_ad(!page_is_comp(btr_pcur_get_page(pcur))
	      || rec_get_status(clust_rec) == REC_STATUS_ORDINARY);

	for (ulint i = 0; i < index->n_fields; i++) {
		ifield = dict_index_get_nth_field(index, i);
		clust_pos = dict_col_get_clust_pos(ifield->col, clust_index);

		if (!get_doc->index_cache->charset) {
			get_doc->index_cache->charset = fts_get_charset(
				ifield->col->prtype);
		}

		if (rec_offs_nth_extern(offsets, clust_pos)) {
			doc->text.f_str =
				btr_rec_copy_externally_stored_field(
					clust_rec, offsets,
					btr_pcur_get_block(pcur)->zip_size(),
					clust_pos, &doc->text.f_len,
					static_cast<mem_heap_t*>(
						doc->self_heap->arg));
		} else {
			doc->text.f_str = (byte*) rec_get_nth_field(
				clust_rec, offsets, clust_pos,
				&doc->text.f_len);
		}

		doc->found = TRUE;
		doc->charset = get_doc->index_cache->charset;

		/* Null Field */
		if (doc->text.f_len == UNIV_SQL_NULL || doc->text.f_len == 0) {
			continue;
		}

		if (!doc_len) {
			fts_tokenize_document(doc, NULL, parser);
		} else {
			fts_tokenize_document_next(doc, doc_len, NULL, parser);
		}

		doc_len += doc->text.f_len + 1;
	}
}

/** Fetch the data from tuple and tokenize the document.
@param[in]     get_doc FTS index's get_doc struct
@param[in]     tuple   tuple should be arranged in table schema order
@param[out]    doc     fts doc to hold parsed documents. */
static
void
fts_fetch_doc_from_tuple(
       fts_get_doc_t*  get_doc,
       const dtuple_t* tuple,
       fts_doc_t*      doc)
{
       dict_index_t*           index;
       st_mysql_ftparser*      parser;
       ulint                   doc_len = 0;
       ulint                   processed_doc = 0;
       ulint                   num_field;

       if (get_doc == NULL) {
               return;
       }

       index = get_doc->index_cache->index;
       parser = get_doc->index_cache->index->parser;
       num_field = dict_index_get_n_fields(index);

       for (ulint i = 0; i < num_field; i++) {
               const dict_field_t*     ifield;
               const dict_col_t*       col;
               ulint                   pos;

               ifield = dict_index_get_nth_field(index, i);
               col = dict_field_get_col(ifield);
               pos = dict_col_get_no(col);
		const dfield_t* field = dtuple_get_nth_field(tuple, pos);

               if (!get_doc->index_cache->charset) {
                       get_doc->index_cache->charset = fts_get_charset(
                               ifield->col->prtype);
               }

               ut_ad(!dfield_is_ext(field));

               doc->text.f_str = (byte*) dfield_get_data(field);
               doc->text.f_len = dfield_get_len(field);
               doc->found = TRUE;
               doc->charset = get_doc->index_cache->charset;

               /* field data is NULL. */
               if (doc->text.f_len == UNIV_SQL_NULL || doc->text.f_len == 0) {
                       continue;
               }

               if (processed_doc == 0) {
                       fts_tokenize_document(doc, NULL, parser);
               } else {
                       fts_tokenize_document_next(doc, doc_len, NULL, parser);
               }

               processed_doc++;
               doc_len += doc->text.f_len + 1;
       }
}

/** Fetch the document from tuple, tokenize the text data and
insert the text data into fts auxiliary table and
its cache. Moreover this tuple fields doesn't contain any information
about externally stored field. This tuple contains data directly
converted from mysql.
@param[in]     ftt     FTS transaction table
@param[in]     doc_id  doc id
@param[in]     tuple   tuple from where data can be retrieved
                       and tuple should be arranged in table
                       schema order. */
void
fts_add_doc_from_tuple(
       fts_trx_table_t*ftt,
       doc_id_t        doc_id,
       const dtuple_t* tuple)
{
       mtr_t           mtr;
       fts_cache_t*    cache = ftt->table->fts->cache;

       ut_ad(cache->get_docs);

       if (!ftt->table->fts->added_synced) {
               fts_init_index(ftt->table, FALSE);
       }

       mtr_start(&mtr);

       ulint   num_idx = ib_vector_size(cache->get_docs);

       for (ulint i = 0; i < num_idx; ++i) {
               fts_doc_t       doc;
               dict_table_t*   table;
               fts_get_doc_t*  get_doc;

               get_doc = static_cast<fts_get_doc_t*>(
                       ib_vector_get(cache->get_docs, i));
               table = get_doc->index_cache->index->table;

               fts_doc_init(&doc);
               fts_fetch_doc_from_tuple(
                       get_doc, tuple, &doc);

               if (doc.found) {
                       mtr_commit(&mtr);
                       mysql_mutex_lock(&table->fts->cache->lock);

                       if (table->fts->cache->stopword_info.status
                           & STOPWORD_NOT_INIT) {
                               fts_load_stopword(table, NULL, NULL,
                                                 true, true);
                       }

                       fts_cache_add_doc(
                               table->fts->cache,
                               get_doc->index_cache,
                               doc_id, doc.tokens);

                       mysql_mutex_unlock(&table->fts->cache->lock);

                       if (cache->total_size > fts_max_cache_size / 5
                           || fts_need_sync) {
                               fts_sync(cache->sync, true, false);
                       }

                       mtr_start(&mtr);

               }

               fts_doc_free(&doc);
       }

       mtr_commit(&mtr);
}

/*********************************************************************//**
This function fetches the document inserted during the committing
transaction, and tokenize the inserted text data and insert into
FTS auxiliary table and its cache. */
static
void
fts_add_doc_by_id(
/*==============*/
	fts_trx_table_t*ftt,		/*!< in: FTS trx table */
	doc_id_t	doc_id)		/*!< in: doc id */
{
	mtr_t		mtr;
	mem_heap_t*	heap;
	btr_pcur_t	pcur;
	dict_table_t*	table;
	dtuple_t*	tuple;
	dfield_t*       dfield;
	fts_get_doc_t*	get_doc;
	doc_id_t        temp_doc_id;
	dict_index_t*   clust_index;
	dict_index_t*	fts_id_index;
	ibool		is_id_cluster;
	fts_cache_t*   	cache = ftt->table->fts->cache;

	ut_ad(cache->get_docs);

	/* If Doc ID has been supplied by the user, then the table
	might not yet be sync-ed */

	if (!ftt->table->fts->added_synced) {
		fts_init_index(ftt->table, FALSE);
	}

	/* Get the first FTS index's get_doc */
	get_doc = static_cast<fts_get_doc_t*>(
		ib_vector_get(cache->get_docs, 0));
	ut_ad(get_doc);

	table = get_doc->index_cache->index->table;

	heap = mem_heap_create(512);

	clust_index = dict_table_get_first_index(table);
	fts_id_index = table->fts_doc_id_index;

	/* Check whether the index on FTS_DOC_ID is cluster index */
	is_id_cluster = (clust_index == fts_id_index);

	mtr_start(&mtr);

	/* Search based on Doc ID. Here, we'll need to consider the case
	when there is no primary index on Doc ID */
	const auto n_uniq = table->fts_n_uniq();
	tuple = dtuple_create(heap, n_uniq);
	dfield = dtuple_get_nth_field(tuple, 0);
	dfield->type.mtype = DATA_INT;
	dfield->type.prtype = DATA_NOT_NULL | DATA_UNSIGNED | DATA_BINARY_TYPE;

	mach_write_to_8((byte*) &temp_doc_id, doc_id);
	dfield_set_data(dfield, &temp_doc_id, sizeof(temp_doc_id));
	pcur.btr_cur.page_cur.index = fts_id_index;

	if (n_uniq == 2) {
		ut_ad(table->versioned());
		ut_ad(fts_id_index->fields[1].col->vers_sys_end());
		dfield = dtuple_get_nth_field(tuple, 1);
		dfield->type.mtype = fts_id_index->fields[1].col->mtype;
		dfield->type.prtype = fts_id_index->fields[1].col->prtype;
		if (table->versioned_by_id()) {
			dfield_set_data(dfield, trx_id_max_bytes,
					sizeof(trx_id_max_bytes));
		} else {
			dfield_set_data(dfield, timestamp_max_bytes,
					sizeof(timestamp_max_bytes));
		}
	}

	/* If we have a match, add the data to doc structure */
	if (btr_pcur_open_with_no_init(tuple, PAGE_CUR_LE,
				       BTR_SEARCH_LEAF, &pcur, &mtr)
	    == DB_SUCCESS
	    && btr_pcur_get_low_match(&pcur) == n_uniq) {
		const rec_t*	rec;
		btr_pcur_t*	doc_pcur;
		const rec_t*	clust_rec;
		btr_pcur_t	clust_pcur;
		rec_offs*	offsets = NULL;
		ulint		num_idx = ib_vector_size(cache->get_docs);

		rec = btr_pcur_get_rec(&pcur);

		/* Doc could be deleted */
		if (page_rec_is_infimum(rec)
		    || rec_get_deleted_flag(rec, dict_table_is_comp(table))) {

			goto func_exit;
		}

		if (is_id_cluster) {
			clust_rec = rec;
			doc_pcur = &pcur;
		} else {
			dtuple_t*	clust_ref;
			auto n_fields = dict_index_get_n_unique(clust_index);

			clust_ref = dtuple_create(heap, n_fields);
			dict_index_copy_types(clust_ref, clust_index, n_fields);

			row_build_row_ref_in_tuple(
				clust_ref, rec, fts_id_index, NULL);
			clust_pcur.btr_cur.page_cur.index = clust_index;

			if (btr_pcur_open_with_no_init(clust_ref,
						       PAGE_CUR_LE,
						       BTR_SEARCH_LEAF,
						       &clust_pcur, &mtr)
			    != DB_SUCCESS) {
				goto func_exit;
			}

			doc_pcur = &clust_pcur;
			clust_rec = btr_pcur_get_rec(&clust_pcur);
		}

		offsets = rec_get_offsets(clust_rec, clust_index, NULL,
					  clust_index->n_core_fields,
					  ULINT_UNDEFINED, &heap);

		for (ulint i = 0; i < num_idx; ++i) {
			fts_doc_t       doc;
			dict_table_t*   table;
			fts_get_doc_t*  get_doc;

			get_doc = static_cast<fts_get_doc_t*>(
				ib_vector_get(cache->get_docs, i));

			table = get_doc->index_cache->index->table;

			fts_doc_init(&doc);

			fts_fetch_doc_from_rec(
				get_doc, clust_index, doc_pcur, offsets, &doc);

			if (doc.found) {

				btr_pcur_store_position(doc_pcur, &mtr);
				mtr_commit(&mtr);

				mysql_mutex_lock(&table->fts->cache->lock);

				if (table->fts->cache->stopword_info.status
				    & STOPWORD_NOT_INIT) {
					fts_load_stopword(table, NULL,
							  NULL, true, true);
				}

				fts_cache_add_doc(
					table->fts->cache,
					get_doc->index_cache,
					doc_id, doc.tokens);

				bool	need_sync = !cache->sync->in_progress
					&& (fts_need_sync
					    || (cache->total_size
						- cache->total_size_at_sync)
					    > fts_max_cache_size / 10);
				if (need_sync) {
					cache->total_size_at_sync =
						cache->total_size;
				}

				mysql_mutex_unlock(&table->fts->cache->lock);

				DBUG_EXECUTE_IF(
					"fts_instrument_sync",
					fts_optimize_request_sync_table(table);
					mysql_mutex_lock(&cache->lock);
					if (cache->sync->in_progress)
						my_cond_wait(
							&cache->sync->cond,
							&cache->lock.m_mutex);
					mysql_mutex_unlock(&cache->lock);
				);

				DBUG_EXECUTE_IF(
					"fts_instrument_sync_debug",
					fts_sync(cache->sync, true, true);
				);

				DEBUG_SYNC_C("fts_instrument_sync_request");
				DBUG_EXECUTE_IF(
					"fts_instrument_sync_request",
					fts_optimize_request_sync_table(table);
				);

				if (need_sync) {
					fts_optimize_request_sync_table(table);
				}

				mtr_start(&mtr);

				if (i < num_idx - 1) {
					if (doc_pcur->restore_position(
					      BTR_SEARCH_LEAF, &mtr)
					    != btr_pcur_t::SAME_ALL) {
						ut_ad("invalid state" == 0);
						i = num_idx - 1;
					}
				}
			}

			fts_doc_free(&doc);
		}

		if (!is_id_cluster) {
			ut_free(doc_pcur->old_rec_buf);
		}
	}
func_exit:
	mtr_commit(&mtr);

	ut_free(pcur.old_rec_buf);

	mem_heap_free(heap);
}

/*********************************************************************//**
Get maximum Doc ID in a table if index "FTS_DOC_ID_INDEX" exists
@return max Doc ID or 0 if index "FTS_DOC_ID_INDEX" does not exist */
doc_id_t
fts_get_max_doc_id(
/*===============*/
	dict_table_t*	table)		/*!< in: user table */
{
	dict_index_t*	index;
	dict_field_t*	dfield MY_ATTRIBUTE((unused)) = NULL;
	doc_id_t	doc_id = 0;
	mtr_t		mtr;
	btr_pcur_t	pcur;

	index = table->fts_doc_id_index;

	if (!index) {
		return(0);
	}

	ut_ad(!index->is_instant());

	dfield = dict_index_get_nth_field(index, 0);

#if 0 /* This can fail when renaming a column to FTS_DOC_ID. */
	ut_ad(Lex_ident_column(Lex_cstring_strlen(dfield->name)).
		streq(FTS_DOC_ID));
#endif

	mtr.start();

	/* fetch the largest indexes value */
	if (pcur.open_leaf(false, index, BTR_SEARCH_LEAF, &mtr) == DB_SUCCESS
	    && !page_is_empty(btr_pcur_get_page(&pcur))) {
		const rec_t*    rec = NULL;
		constexpr ulint	doc_id_len= 8;

		do {
			rec = btr_pcur_get_rec(&pcur);

			if (!page_rec_is_user_rec(rec)) {
				continue;
			}

			if (index->n_uniq == 1) {
				break;
			}

			ut_ad(table->versioned());
			ut_ad(index->n_uniq == 2);

			const byte *data = rec + doc_id_len;
			if (table->versioned_by_id()) {
				if (0 == memcmp(data, trx_id_max_bytes,
						sizeof trx_id_max_bytes)) {
					break;
				}
			} else {
                                if (IS_MAX_TIMESTAMP(data)) {
					break;
				}
			}
		} while (btr_pcur_move_to_prev(&pcur, &mtr));

		if (!rec || rec_is_metadata(rec, *index)) {
			goto func_exit;
		}

		doc_id = fts_read_doc_id(rec);
	}

func_exit:
	mtr.commit();
	return(doc_id);
}

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
{
  dberr_t  err;
  doc_id_t write_doc_id;
  dict_index_t *index= nullptr;
  trx_t *trx = trx_create();
  trx->op_info = "fetching indexed FTS document";

  /* The FTS index can be supplied by caller directly with
  "index_to_use", otherwise, get it from "get_doc" */
  index = (index_to_use) ? index_to_use : get_doc->index_cache->index;

  fts_fetch_doc_id fetch;
  fetch.user_arg= arg;

  fts_match_key match_op= MATCH_UNIQUE;
  page_cur_mode_t mode= PAGE_CUR_GE;
  dict_table_t *table= index->table;
  dict_index_t *clust_index= dict_table_get_first_index(table);
  dict_index_t *fts_idx = dict_table_get_index_on_name(table,
                                                       FTS_DOC_ID_INDEX.str);

  if (option == FTS_FETCH_DOC_BY_ID_LARGE)
  {
    /* Get the FTS_DOC_ID field number in clustered index record */
    dict_col_t *col= fts_idx->fields[0].col;
    for (uint32_t i= 0; i < clust_index->n_fields; i++)
    {
      if (clust_index->fields[i].col == col)
      {
        fetch.field_to_fetch.push_back(i);
        break;
      }
    }

    /* SELECT %s, %s FROM $table_name WHERE %s > :doc_id */
    match_op= MATCH_ALL;
    mode= PAGE_CUR_G;
  }
  else if (table->versioned())
    /* In case of versioned table, FTS_DOC_ID_IDX can have
    {FTS_DOC_ID, ROW_END}. So there can be multiple entries
    for the same DOC_ID */
    match_op= MATCH_PREFIX;

  if (index == fts_idx);
  else
  {
    /* Get the field number of INDEX FIELDS in clustered index record */
    for (uint32_t i= 0; i < index->n_user_defined_cols; i++)
    {
      dict_col_t *col= index->fields[i].col;
      for (uint32_t j= 0; j < clust_index->n_fields; j++)
      {
        if (clust_index->fields[j].col == col)
        {
          fetch.field_to_fetch.push_back(j);
          break;
        }
      }
    }
  }

  /* Convert to "storage" byte order. */
  fts_write_doc_id((byte*) &write_doc_id, doc_id);

  FTSQueryRunner sqlRunner(trx);
  sqlRunner.build_tuple(fts_idx, 1, 1);
  err= sqlRunner.prepare_for_read(table);
  if (err == DB_SUCCESS)
  {
    dtuple_t *tuple= sqlRunner.tuple();
    dfield_set_data(&tuple->fields[0], &write_doc_id, sizeof(doc_id_t));
    err= sqlRunner.record_executor(fts_idx, READ, match_op,
                                   mode, callback, &fetch);
  }
  if (err == DB_RECORD_NOT_FOUND || err == DB_END_OF_INDEX)
    err= DB_SUCCESS;
  fts_sql_commit(trx);
  trx->free();
  return err;
}

/** Write out a single word's data as new entry/entries in the INDEX table.
@param word word in UTF-8
@param node node columns
@param sqlRunner Runs the query in FTS SYSTEM TABLES
@return DB_SUCCESS if all OK. */
dberr_t
fts_write_node(dict_table_t *table,
               fts_string_t *word, fts_node_t* node,
               FTSQueryRunner *sqlRunner)
{
  dberr_t err= sqlRunner->prepare_for_write(table);
  if (err == DB_SUCCESS)
  {
    dict_index_t *index= dict_table_get_first_index(table);
    sqlRunner->build_tuple(index);
    sqlRunner->assign_aux_table_fields(word->f_str, word->f_len, node);
    time_t start_time = time(NULL);
    err= sqlRunner->write_record(table);
    elapsed_time += time(NULL) - start_time;
    ++n_nodes;
  }
  return err;
}

/** Sort an array of doc_id */
void fts_doc_ids_sort(ib_vector_t *doc_ids)
{
  doc_id_t *const data= reinterpret_cast<doc_id_t*>(doc_ids->data);
  std::sort(data, data + doc_ids->used);
}

/** Add rows to the DELETED_CACHE table.
@return DB_SUCCESS if all went well else error code*/
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t fts_sync_add_deleted_cache(fts_sync_t *sync, ib_vector_t *doc_ids)
{
  ulint	n_elems = ib_vector_size(doc_ids);
  ut_a(ib_vector_size(doc_ids) > 0);
  fts_doc_ids_sort(doc_ids);

  fts_table_t fts_table;
  FTS_INIT_FTS_TABLE(&fts_table, "DELETED_CACHE", FTS_COMMON_TABLE,
                     sync->table);
  FTSQueryRunner sqlRunner(sync->trx);
  dberr_t err= DB_SUCCESS;
  dict_index_t *clust_index= nullptr;
  dict_table_t *deleted_cache= sqlRunner.open_table(&fts_table, &err);

  if (deleted_cache)
  {
    err= sqlRunner.prepare_for_write(deleted_cache);
    if (err) goto func_exit;
    ut_ad(UT_LIST_GET_LEN(deleted_cache->indexes) == 1);
    clust_index= dict_table_get_first_index(deleted_cache);
    sqlRunner.build_tuple(clust_index);
    for (ulint i = 0; i < n_elems && err == DB_SUCCESS; ++i)
    {
      doc_id_t* update= static_cast<doc_id_t*>(ib_vector_get(doc_ids, i));
      sqlRunner.assign_common_table_fields(update);
      /* INSERT INTO DELETED_CACHE VALUES(doc_id) */
      err= sqlRunner.write_record(deleted_cache);
    }
  }

func_exit:
  if (deleted_cache) deleted_cache->release();
  return err;
}

/** Write the words and ilist to disk.
@param[in,out]	trx		transaction
@param[in]	index_cache	index cache
@param[in]	unlock_cache	whether unlock cache when write node
@return DB_SUCCESS if all went well else error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_sync_write_words(
	trx_t*			trx,
	fts_index_cache_t*	index_cache,
	bool			unlock_cache)
{
	fts_table_t	fts_table;
	ulint		n_nodes = 0;
	ulint		n_words = 0;
	const ib_rbt_node_t* rbt_node;
	dberr_t		error = DB_SUCCESS;
	ibool		print_error = FALSE;
	dict_table_t*	table = index_cache->index->table;
	dict_table_t*   aux_table = nullptr;
	std::map<ulint, dict_table_t*> aux_tables;
	FTSQueryRunner  sqlRunner(trx);

	FTS_INIT_INDEX_TABLE(
		&fts_table, NULL, FTS_INDEX_TABLE, index_cache->index);

	n_words = rbt_size(index_cache->words);

	/* We iterate over the entire tree, even if there is an error,
	since we want to free the memory used during caching. */
	for (rbt_node = rbt_first(index_cache->words);
	     rbt_node;
	     rbt_node = rbt_next(index_cache->words, rbt_node)) {

		ulint			i;
		ulint			selected;
		fts_tokenizer_word_t*	word;

		word = rbt_value(fts_tokenizer_word_t, rbt_node);

		DBUG_EXECUTE_IF(
			"fts_instrument_write_words_before_select_index",
			std::this_thread::sleep_for(
				std::chrono::milliseconds(300)););

		selected = fts_select_index(
			index_cache->charset, word->text.f_str,
			word->text.f_len);

		fts_table.suffix = fts_get_suffix(selected);

		auto find_table = aux_tables.find(selected);
		if (find_table != aux_tables.end()) {
			aux_table = find_table->second;
		} else {
			fts_table.suffix = fts_get_suffix(selected);
			aux_table= sqlRunner.open_table(&fts_table, &error);
			if (error == DB_SUCCESS) {
				aux_tables[selected]= aux_table;
			}
                }

		/* We iterate over all the nodes even if there was an error */
		for (i = 0; i < ib_vector_size(word->nodes); ++i) {

			fts_node_t* fts_node = static_cast<fts_node_t*>(
				ib_vector_get(word->nodes, i));

			if (fts_node->synced) {
				continue;
			} else {
				fts_node->synced = true;
			}

			/*FIXME: we need to handle the error properly. */
			if (error == DB_SUCCESS) {
				if (unlock_cache) {
					mysql_mutex_unlock(
						&table->fts->cache->lock);
				}
				error = fts_write_node(
					aux_table, &word->text, fts_node,
					&sqlRunner);
				DEBUG_SYNC_C("fts_write_node");
				DBUG_EXECUTE_IF("fts_write_node_crash",
					DBUG_SUICIDE(););

				DBUG_EXECUTE_IF(
					"fts_instrument_sync_sleep",
					std::this_thread::sleep_for(
						std::chrono::seconds(1)););

				if (unlock_cache) {
					mysql_mutex_lock(
						&table->fts->cache->lock);
				}
			}
		}

		n_nodes += ib_vector_size(word->nodes);

		if (UNIV_UNLIKELY(error != DB_SUCCESS) && !print_error) {
			ib::error() << "(" << error << ") writing"
				" word node to FTS auxiliary index table "
				<< table->name;
			print_error = TRUE;
		}
	}

	for (auto it : aux_tables) {
		it.second->release();
	}

	if (UNIV_UNLIKELY(fts_enable_diag_print)) {
		printf("Avg number of nodes: %lf\n",
		       (double) n_nodes / (double) (n_words > 1 ? n_words : 1));
	}

	return(error);
}

/*********************************************************************//**
Begin Sync, create transaction, acquire locks, etc. */
static
void
fts_sync_begin(
/*===========*/
	fts_sync_t*	sync)			/*!< in: sync state */
{
	fts_cache_t*	cache = sync->table->fts->cache;

	n_nodes = 0;
	elapsed_time = 0;

	sync->start_time = time(NULL);

	sync->trx = trx_create();
	trx_start_internal(sync->trx);

	if (UNIV_UNLIKELY(fts_enable_diag_print)) {
		ib::info() << "FTS SYNC for table " << sync->table->name
			<< ", deleted count: "
			<< ib_vector_size(cache->deleted_doc_ids)
			<< " size: " << ib::bytes_iec{cache->total_size};
	}
}

/*********************************************************************//**
Run SYNC on the table, i.e., write out data from the index specific
cache to the FTS aux INDEX table and FTS aux doc id stats table.
@return DB_SUCCESS if all OK */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_sync_index(
/*===========*/
	fts_sync_t*		sync,		/*!< in: sync state */
	fts_index_cache_t*	index_cache)	/*!< in: index cache */
{
	trx_t*		trx = sync->trx;

	trx->op_info = "doing SYNC index";

	if (UNIV_UNLIKELY(fts_enable_diag_print)) {
		ib::info() << "SYNC words: " << rbt_size(index_cache->words);
	}

	ut_ad(rbt_validate(index_cache->words));

	return(fts_sync_write_words(trx, index_cache, sync->unlock_cache));
}

/** Check if index cache has been synced completely
@param[in,out]	index_cache	index cache
@return true if index is synced, otherwise false. */
static
bool
fts_sync_index_check(
	fts_index_cache_t*	index_cache)
{
	const ib_rbt_node_t*	rbt_node;

	for (rbt_node = rbt_first(index_cache->words);
	     rbt_node != NULL;
	     rbt_node = rbt_next(index_cache->words, rbt_node)) {

		fts_tokenizer_word_t*	word;
		word = rbt_value(fts_tokenizer_word_t, rbt_node);

		fts_node_t*	fts_node;
		fts_node = static_cast<fts_node_t*>(ib_vector_last(word->nodes));

		if (!fts_node->synced) {
			return(false);
		}
	}

	return(true);
}

/** Reset synced flag in index cache when rollback
@param[in,out]	index_cache	index cache */
static
void
fts_sync_index_reset(
	fts_index_cache_t*	index_cache)
{
	const ib_rbt_node_t*	rbt_node;

	for (rbt_node = rbt_first(index_cache->words);
	     rbt_node != NULL;
	     rbt_node = rbt_next(index_cache->words, rbt_node)) {

		fts_tokenizer_word_t*	word;
		word = rbt_value(fts_tokenizer_word_t, rbt_node);

		fts_node_t*	fts_node;
		fts_node = static_cast<fts_node_t*>(ib_vector_last(word->nodes));

		fts_node->synced = false;
	}
}

/** Commit the SYNC, change state of processed doc ids etc.
@param[in,out]	sync	sync state
@return DB_SUCCESS if all OK */
static  MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fts_sync_commit(
	fts_sync_t*	sync)
{
	dberr_t		error;
	trx_t*		trx = sync->trx;
	fts_cache_t*	cache = sync->table->fts->cache;
	doc_id_t	last_doc_id;

	trx->op_info = "doing SYNC commit";

	/* After each Sync, update the CONFIG table about the max doc id
	we just sync-ed to index table */
	error = fts_cmp_set_sync_doc_id(sync->table, sync->max_doc_id,
					&last_doc_id, trx);

	/* Get the list of deleted documents that are either in the
	cache or were headed there but were deleted before the add
	thread got to them. */

	if (error == DB_SUCCESS && ib_vector_size(cache->deleted_doc_ids) > 0) {

		error = fts_sync_add_deleted_cache(
			sync, cache->deleted_doc_ids);
	}

	/* We need to do this within the deleted lock since fts_delete() can
	attempt to add a deleted doc id to the cache deleted id array. */
	fts_cache_clear(cache);
	DEBUG_SYNC_C("fts_deleted_doc_ids_clear");
	fts_cache_init(cache);
	mysql_mutex_unlock(&cache->lock);

	if (UNIV_LIKELY(error == DB_SUCCESS)) {
		DEBUG_SYNC_C("fts_crash_before_commit_sync");
		fts_sql_commit(trx);
	} else {
		fts_sql_rollback(trx);
		ib::error() << "(" << error << ") during SYNC of "
			"table " << sync->table->name;
	}

	if (UNIV_UNLIKELY(fts_enable_diag_print) && elapsed_time) {
		ib::info() << "SYNC for table " << sync->table->name
			<< ": SYNC time: "
			<< (time(NULL) - sync->start_time)
			<< " secs: elapsed "
			<< static_cast<double>(n_nodes)
			/ static_cast<double>(elapsed_time)
			<< " ins/sec";
	}

	/* Avoid assertion in trx_t::free(). */
	trx->dict_operation_lock_mode = false;
	trx->free();

	return(error);
}

/** Rollback a sync operation
@param[in,out]	sync	sync state */
static
void
fts_sync_rollback(
	fts_sync_t*	sync)
{
	trx_t*		trx = sync->trx;
	fts_cache_t*	cache = sync->table->fts->cache;

	for (ulint i = 0; i < ib_vector_size(cache->indexes); ++i) {
		fts_index_cache_t*	index_cache;

		index_cache = static_cast<fts_index_cache_t*>(
			ib_vector_get(cache->indexes, i));

		/* Reset synced flag so nodes will not be skipped
		in the next sync, see fts_sync_write_words(). */
		fts_sync_index_reset(index_cache);
	}

	mysql_mutex_unlock(&cache->lock);

	fts_sql_rollback(trx);

	/* Avoid assertion in trx_t::free(). */
	trx->dict_operation_lock_mode = false;
	trx->free();
}

/** Run SYNC on the table, i.e., write out data from the cache to the
FTS auxiliary INDEX table and clear the cache at the end.
@param[in,out]	sync		sync state
@param[in]	unlock_cache	whether unlock cache lock when write node
@param[in]	wait		whether wait when a sync is in progress
@return DB_SUCCESS if all OK */
static
dberr_t
fts_sync(
	fts_sync_t*	sync,
	bool		unlock_cache,
	bool		wait)
{
	if (srv_read_only_mode) {
		return DB_READ_ONLY;
	}

	ulint		i;
	dberr_t		error = DB_SUCCESS;
	fts_cache_t*	cache = sync->table->fts->cache;

	mysql_mutex_lock(&cache->lock);

	if (cache->total_size == 0) {
                mysql_mutex_unlock(&cache->lock);
		return DB_SUCCESS;
	}

	/* Check if cache is being synced.
	Note: we release cache lock in fts_sync_write_words() to
	avoid long wait for the lock by other threads. */
	if (sync->in_progress) {
		if (!wait) {
			mysql_mutex_unlock(&cache->lock);
			return(DB_SUCCESS);
		}
		do {
			my_cond_wait(&sync->cond, &cache->lock.m_mutex);
		} while (sync->in_progress);
	}

	sync->unlock_cache = unlock_cache;
	sync->in_progress = true;

	DEBUG_SYNC_C("fts_sync_begin");
	fts_sync_begin(sync);

begin_sync:
	const size_t fts_cache_size= fts_max_cache_size;
	if (cache->total_size > fts_cache_size) {
		/* Avoid the case: sync never finish when
		insert/update keeps comming. */
		ut_ad(sync->unlock_cache);
		sync->unlock_cache = false;
		ib::warn() << "Total InnoDB FTS size "
			<< cache->total_size << " for the table "
			<< cache->sync->table->name
			<< " exceeds the innodb_ft_cache_size "
			<< fts_cache_size;
	}

	for (i = 0; i < ib_vector_size(cache->indexes); ++i) {
		fts_index_cache_t*	index_cache;

		index_cache = static_cast<fts_index_cache_t*>(
			ib_vector_get(cache->indexes, i));

		if (index_cache->index->to_be_dropped) {
			continue;
		}

		DBUG_EXECUTE_IF("fts_instrument_sync_before_syncing",
				std::this_thread::sleep_for(
					std::chrono::milliseconds(300)););
		error = fts_sync_index(sync, index_cache);

		if (error != DB_SUCCESS) {
			goto end_sync;
		}

		if (!sync->unlock_cache
		    && cache->total_size < fts_max_cache_size) {
			/* Reset the unlock cache if the value
			is less than innodb_ft_cache_size */
			sync->unlock_cache = true;
		}
	}

	DBUG_EXECUTE_IF("fts_instrument_sync_interrupted",
			sync->interrupted = true;
			error = DB_INTERRUPTED;
			goto end_sync;
	);

	/* Make sure all the caches are synced. */
	for (i = 0; i < ib_vector_size(cache->indexes); ++i) {
		fts_index_cache_t*	index_cache;

		index_cache = static_cast<fts_index_cache_t*>(
			ib_vector_get(cache->indexes, i));

		if (index_cache->index->to_be_dropped
		    || fts_sync_index_check(index_cache)) {
			continue;
		}

		goto begin_sync;
	}

end_sync:
	if (error == DB_SUCCESS && !sync->interrupted) {
		error = fts_sync_commit(sync);
	} else {
		fts_sync_rollback(sync);
	}

	mysql_mutex_lock(&cache->lock);
	ut_ad(sync->in_progress);
	sync->interrupted = false;
	sync->in_progress = false;
	pthread_cond_broadcast(&sync->cond);
	mysql_mutex_unlock(&cache->lock);

	/* We need to check whether an optimize is required, for that
	we make copies of the two variables that control the trigger. These
	variables can change behind our back and we don't want to hold the
	lock for longer than is needed. */
	mysql_mutex_lock(&cache->deleted_lock);

	cache->added = 0;
	cache->deleted = 0;

	mysql_mutex_unlock(&cache->deleted_lock);

	return(error);
}

/** Run SYNC on the table, i.e., write out data from the cache to the
FTS auxiliary INDEX table and clear the cache at the end.
@param[in,out]	table		fts table
@param[in]	wait		whether wait for existing sync to finish
@return DB_SUCCESS on success, error code on failure. */
dberr_t fts_sync_table(dict_table_t* table, bool wait)
{
  ut_ad(table->fts);

  return table->space && !table->corrupted && table->fts->cache
    ? fts_sync(table->fts->cache->sync, !wait, wait)
    : DB_SUCCESS;
}

/** Check if a fts token is a stopword or less than fts_min_token_size
or greater than fts_max_token_size.
@param[in]	token		token string
@param[in]	stopwords	stopwords rb tree
@param[in]	cs		token charset
@retval	true	if it is not stopword and length in range
@retval	false	if it is stopword or lenght not in range */
bool
fts_check_token(
	const fts_string_t*		token,
	const ib_rbt_t*			stopwords,
	const CHARSET_INFO*		cs)
{
	ut_ad(cs != NULL || stopwords == NULL);

	ib_rbt_bound_t  parent;

	return(token->f_n_char >= fts_min_token_size
	       && token->f_n_char <= fts_max_token_size
	       && (stopwords == NULL
		   || rbt_search(stopwords, &parent, token) != 0));
}

/** Add the token and its start position to the token's list of positions.
@param[in,out]	result_doc	result doc rb tree
@param[in]	str		token string
@param[in]	position	token position */
static
void
fts_add_token(
	fts_doc_t*	result_doc,
	fts_string_t	str,
	ulint		position)
{
	/* Ignore string whose character number is less than
	"fts_min_token_size" or more than "fts_max_token_size" */

	if (fts_check_token(&str, NULL, result_doc->charset)) {

		mem_heap_t*	heap;
		fts_string_t	t_str;
		fts_token_t*	token;
		ib_rbt_bound_t	parent;

		heap = static_cast<mem_heap_t*>(result_doc->self_heap->arg);

		t_str.f_n_char = str.f_n_char;

		t_str.f_len = str.f_len * result_doc->charset->casedn_multiply() + 1;

		t_str.f_str = static_cast<byte*>(
			mem_heap_alloc(heap, t_str.f_len));

		/* For binary collations, a case sensitive search is
		performed. Hence don't convert to lower case. */
		if (my_binary_compare(result_doc->charset)) {
			memcpy(t_str.f_str, str.f_str, str.f_len);
			t_str.f_str[str.f_len]= 0;
			t_str.f_len= str.f_len;
		} else {
			t_str.f_len= result_doc->charset->casedn_z(
					(const char*) str.f_str, str.f_len,
					(char *) t_str.f_str, t_str.f_len);
		}

		/* Add the word to the document statistics. If the word
		hasn't been seen before we create a new entry for it. */
		if (rbt_search(result_doc->tokens, &parent, &t_str) != 0) {
			fts_token_t	new_token;

			new_token.text = t_str;

			new_token.positions = ib_vector_create(
				result_doc->self_heap, sizeof(ulint), 32);

			parent.last = rbt_add_node(
				result_doc->tokens, &parent, &new_token);

			ut_ad(rbt_validate(result_doc->tokens));
		}

		token = rbt_value(fts_token_t, parent.last);
		ib_vector_push(token->positions, &position);
	}
}

/********************************************************************
Process next token from document starting at the given position, i.e., add
the token's start position to the token's list of positions.
@return number of characters handled in this call */
static
ulint
fts_process_token(
/*==============*/
	fts_doc_t*	doc,		/* in/out: document to
					tokenize */
	fts_doc_t*	result,		/* out: if provided, save
					result here */
	ulint		start_pos,	/*!< in: start position in text */
	ulint		add_pos)	/*!< in: add this position to all
					tokens from this tokenization */
{
	ulint		ret;
	fts_string_t	str;
	ulint		position;
	fts_doc_t*	result_doc;
	byte		buf[FTS_MAX_WORD_LEN + 1];

	str.f_str = buf;

	/* Determine where to save the result. */
	result_doc = (result != NULL) ? result : doc;

	/* The length of a string in characters is set here only. */

	ret = innobase_mysql_fts_get_token(
		doc->charset, doc->text.f_str + start_pos,
		doc->text.f_str + doc->text.f_len, &str);

	position = start_pos + ret - str.f_len + add_pos;

	fts_add_token(result_doc, str, position);

	return(ret);
}

/*************************************************************//**
Get token char size by charset
@return token size */
ulint
fts_get_token_size(
/*===============*/
	const CHARSET_INFO*	cs,	/*!< in: Character set */
	const char*		token,	/*!< in: token */
	ulint			len)	/*!< in: token length */
{
	char*	start;
	char*	end;
	ulint	size = 0;

	/* const_cast is for reinterpret_cast below, or it will fail. */
	start = const_cast<char*>(token);
	end = start + len;
	while (start < end) {
		int	ctype;
		int	mbl;

		mbl = cs->ctype(
			&ctype,
			reinterpret_cast<uchar*>(start),
			reinterpret_cast<uchar*>(end));

		size++;

		start += mbl > 0 ? mbl : (mbl < 0 ? -mbl : 1);
	}

	return(size);
}

/*************************************************************//**
FTS plugin parser 'myql_parser' callback function for document tokenize.
Refer to 'st_mysql_ftparser_param' for more detail.
@return always returns 0 */
int
fts_tokenize_document_internal(
/*===========================*/
	MYSQL_FTPARSER_PARAM*	param,	/*!< in: parser parameter */
	const char*		doc,/*!< in/out: document */
	int			len)	/*!< in: document length */
{
	fts_string_t	str;
	byte		buf[FTS_MAX_WORD_LEN + 1];
	/* JAN: TODO: MySQL 5.7
	MYSQL_FTPARSER_BOOLEAN_INFO bool_info =
		{ FT_TOKEN_WORD, 0, 0, 0, 0, 0, ' ', 0 };
	*/
	MYSQL_FTPARSER_BOOLEAN_INFO bool_info =
		{ FT_TOKEN_WORD, 0, 0, 0, 0, ' ', 0};

	ut_ad(len >= 0);

	str.f_str = buf;

	for (ulint i = 0, inc = 0; i < static_cast<ulint>(len); i += inc) {
		inc = innobase_mysql_fts_get_token(
			const_cast<CHARSET_INFO*>(param->cs),
			(uchar*)(doc) + i,
			(uchar*)(doc) + len,
			&str);

		if (str.f_len > 0) {
			/* JAN: TODO: MySQL 5.7
			bool_info.position =
				static_cast<int>(i + inc - str.f_len);
			ut_ad(bool_info.position >= 0);
			*/

			/* Stop when add word fails */
			if (param->mysql_add_word(
				param,
				reinterpret_cast<char*>(str.f_str),
				static_cast<int>(str.f_len),
				&bool_info)) {
				break;
			}
		}
	}

	return(0);
}

/******************************************************************//**
FTS plugin parser 'myql_add_word' callback function for document tokenize.
Refer to 'st_mysql_ftparser_param' for more detail.
@return always returns 0 */
static
int
fts_tokenize_add_word_for_parser(
/*=============================*/
	MYSQL_FTPARSER_PARAM*	param,		/* in: parser paramter */
	const char*			word,		/* in: token word */
	int			word_len,	/* in: word len */
	MYSQL_FTPARSER_BOOLEAN_INFO*)
{
	fts_string_t	str;
	fts_tokenize_param_t*	fts_param;
	fts_doc_t*	result_doc;
	ulint		position;

	fts_param = static_cast<fts_tokenize_param_t*>(param->mysql_ftparam);
	result_doc = fts_param->result_doc;
	ut_ad(result_doc != NULL);

	str.f_str = (byte*)(word);
	str.f_len = ulint(word_len);
	str.f_n_char = fts_get_token_size(
		const_cast<CHARSET_INFO*>(param->cs), word, str.f_len);

	/* JAN: TODO: MySQL 5.7 FTS
	ut_ad(boolean_info->position >= 0);
	position = boolean_info->position + fts_param->add_pos;
	*/
	position = fts_param->add_pos++;

	fts_add_token(result_doc, str, position);

	return(0);
}

/******************************************************************//**
Parse a document using an external / user supplied parser */
static
void
fts_tokenize_by_parser(
/*===================*/
	fts_doc_t*		doc,	/* in/out: document to tokenize */
	st_mysql_ftparser*	parser, /* in: plugin fts parser */
	fts_tokenize_param_t*	fts_param) /* in: fts tokenize param */
{
	MYSQL_FTPARSER_PARAM	param;

	ut_a(parser);

	/* Set paramters for param */
	param.mysql_parse = fts_tokenize_document_internal;
	param.mysql_add_word = fts_tokenize_add_word_for_parser;
	param.mysql_ftparam = fts_param;
	param.cs = doc->charset;
	param.doc = reinterpret_cast<char*>(doc->text.f_str);
	param.length = static_cast<int>(doc->text.f_len);
	param.mode= MYSQL_FTPARSER_SIMPLE_MODE;

	PARSER_INIT(parser, &param);
	parser->parse(&param);
	PARSER_DEINIT(parser, &param);
}

/** Tokenize a document.
@param[in,out]	doc	document to tokenize
@param[out]	result	tokenization result
@param[in]	parser	pluggable parser */
static
void
fts_tokenize_document(
	fts_doc_t*		doc,
	fts_doc_t*		result,
	st_mysql_ftparser*	parser)
{
	ut_a(!doc->tokens);
	ut_a(doc->charset);

	doc->tokens = rbt_create_arg_cmp(sizeof(fts_token_t),
					 innobase_fts_text_cmp,
					 (void*) doc->charset);

	if (parser != NULL) {
		fts_tokenize_param_t	fts_param;
		fts_param.result_doc = (result != NULL) ? result : doc;
		fts_param.add_pos = 0;

		fts_tokenize_by_parser(doc, parser, &fts_param);
	} else {
		ulint		inc;

		for (ulint i = 0; i < doc->text.f_len; i += inc) {
			inc = fts_process_token(doc, result, i, 0);
			ut_a(inc > 0);
		}
	}
}

/** Continue to tokenize a document.
@param[in,out]	doc	document to tokenize
@param[in]	add_pos	add this position to all tokens from this tokenization
@param[out]	result	tokenization result
@param[in]	parser	pluggable parser */
static
void
fts_tokenize_document_next(
	fts_doc_t*		doc,
	ulint			add_pos,
	fts_doc_t*		result,
	st_mysql_ftparser*	parser)
{
	ut_a(doc->tokens);

	if (parser) {
		fts_tokenize_param_t	fts_param;

		fts_param.result_doc = (result != NULL) ? result : doc;
		fts_param.add_pos = add_pos;

		fts_tokenize_by_parser(doc, parser, &fts_param);
	} else {
		ulint		inc;

		for (ulint i = 0; i < doc->text.f_len; i += inc) {
			inc = fts_process_token(doc, result, i, add_pos);
			ut_a(inc > 0);
		}
	}
}

/** Create the vector of fts_get_doc_t instances.
@param[in,out]	cache	fts cache
@return	vector of fts_get_doc_t instances */
static
ib_vector_t*
fts_get_docs_create(
	fts_cache_t*	cache)
{
	ib_vector_t*	get_docs;

	mysql_mutex_assert_owner(&cache->init_lock);

	/* We need one instance of fts_get_doc_t per index. */
	get_docs = ib_vector_create(cache->self_heap, sizeof(fts_get_doc_t), 4);

	/* Create the get_doc instance, we need one of these
	per FTS index. */
	for (ulint i = 0; i < ib_vector_size(cache->indexes); ++i) {

		dict_index_t**	index;
		fts_get_doc_t*	get_doc;

		index = static_cast<dict_index_t**>(
			ib_vector_get(cache->indexes, i));

		get_doc = static_cast<fts_get_doc_t*>(
			ib_vector_push(get_docs, NULL));

		memset(get_doc, 0x0, sizeof(*get_doc));

		get_doc->index_cache = fts_get_index_cache(cache, *index);
		get_doc->cache = cache;

		/* Must find the index cache. */
		ut_a(get_doc->index_cache != NULL);
	}

	return(get_docs);
}

/*********************************************************************//**
Get the initial Doc ID by consulting the CONFIG table
@return initial Doc ID */
doc_id_t
fts_init_doc_id(
/*============*/
	const dict_table_t*	table)		/*!< in: table */
{
	doc_id_t	max_doc_id = 0;

	mysql_mutex_lock(&table->fts->cache->lock);

	/* Return if the table is already initialized for DOC ID */
	if (table->fts->cache->first_doc_id != FTS_NULL_DOC_ID) {
		mysql_mutex_unlock(&table->fts->cache->lock);
		return(0);
	}

	DEBUG_SYNC_C("fts_initialize_doc_id");

	/* Then compare this value with the ID value stored in the CONFIG
	table. The larger one will be our new initial Doc ID */
	fts_cmp_set_sync_doc_id(table, 0, &max_doc_id);

	/* If DICT_TF2_FTS_ADD_DOC_ID is set, we are in the process of
	creating index (and add doc id column. No need to recovery
	documents */
	if (!DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_ADD_DOC_ID)) {
		fts_init_index((dict_table_t*) table, TRUE);
	}

	table->fts->added_synced = true;

	table->fts->cache->first_doc_id = max_doc_id;

	mysql_mutex_unlock(&table->fts->cache->lock);

	ut_ad(max_doc_id > 0);

	return(max_doc_id);
}

#ifdef FTS_MULT_INDEX
/*********************************************************************//**
Check if the index is in the affected set.
@return TRUE if index is updated */
static
ibool
fts_is_index_updated(
/*=================*/
	const ib_vector_t*	fts_indexes,	/*!< in: affected FTS indexes */
	const fts_get_doc_t*	get_doc)	/*!< in: info for reading
						document */
{
	ulint		i;
	dict_index_t*	index = get_doc->index_cache->index;

	for (i = 0; i < ib_vector_size(fts_indexes); ++i) {
		const dict_index_t*	updated_fts_index;

		updated_fts_index = static_cast<const dict_index_t*>(
			ib_vector_getp_const(fts_indexes, i));

		ut_a(updated_fts_index != NULL);

		if (updated_fts_index == index) {
			return(TRUE);
		}
	}

	return(FALSE);
}
#endif

bool fts_read_count(dict_index_t *index, const rec_t *rec,
                    const rec_offs *offset, void *user_arg)
{
  ut_ad(dict_index_is_clust(index));
  ulint* n_count= static_cast<ulint*>(user_arg);
  (*n_count)++;
  return true;
}

/** Fetch COUNT(*) from specified table.
@param fts_table Fulltext table to be fetched
@return the number of rows in the table */
ulint fts_get_rows_count(fts_table_t *fts_table)
{
  trx_t *trx= trx_create();
  trx->op_info= "fetching FT table rows count";
  FTSQueryRunner sqlRunner(trx);
  ulint n_count= 0;
  dberr_t err= DB_SUCCESS;
  dict_table_t *table= sqlRunner.open_table(fts_table, &err);
  if (table)
  {
    err= sqlRunner.prepare_for_read(table);
    if (err == DB_SUCCESS)
    {
      ut_ad(UT_LIST_GET_LEN(table->indexes) == 1);
      dict_index_t *clust_index= dict_table_get_first_index(table); 
      err= sqlRunner.record_executor(clust_index, READ, MATCH_UNIQUE,
                                     PAGE_CUR_GE, &fts_read_count, &n_count);
      if (err == DB_END_OF_INDEX) err= DB_SUCCESS;
    }
  }

  if (err == DB_SUCCESS)
    fts_sql_commit(trx);
  else
  {
    fts_sql_rollback(trx);
    ib::error() << "(" << err << ") while reading FTS table "
                << table->name.m_name;
  }

  if (table) table->release();
  trx->free();
  return n_count;
}

#ifdef FTS_CACHE_SIZE_DEBUG
/*********************************************************************//**
Read the max cache size parameter from the config table. */
static
void
fts_update_max_cache_size(
/*======================*/
	fts_sync_t*	sync)			/*!< in: sync state */
{
	trx_t*		trx;
	fts_table_t	fts_table;

	trx = trx_create();

	FTS_INIT_FTS_TABLE(&fts_table, "CONFIG", FTS_COMMON_TABLE, sync->table);

	/* The size returned is in bytes. */
	sync->max_cache_size = fts_get_max_cache_size(trx, &fts_table);

	fts_sql_commit(trx);

	trx->free();
}
#endif /* FTS_CACHE_SIZE_DEBUG */

/*********************************************************************//**
Free the modified rows of a table. */
UNIV_INLINE
void
fts_trx_table_rows_free(
/*====================*/
	ib_rbt_t*	rows)			/*!< in: rbt of rows to free */
{
	const ib_rbt_node_t*	node;

	for (node = rbt_first(rows); node; node = rbt_first(rows)) {
		fts_trx_row_t*	row;

		row = rbt_value(fts_trx_row_t, node);

		if (row->fts_indexes != NULL) {
			/* This vector shouldn't be using the
			heap allocator.  */
			ut_a(row->fts_indexes->allocator->arg == NULL);

			ib_vector_free(row->fts_indexes);
			row->fts_indexes = NULL;
		}

		ut_free(rbt_remove_node(rows, node));
	}

	ut_a(rbt_empty(rows));
	rbt_free(rows);
}

/*********************************************************************//**
Free an FTS savepoint instance. */
UNIV_INLINE
void
fts_savepoint_free(
/*===============*/
	fts_savepoint_t*	savepoint)	/*!< in: savepoint instance */
{
	const ib_rbt_node_t*	node;
	ib_rbt_t*		tables = savepoint->tables;

	/* Nothing to free! */
	if (tables == NULL) {
		return;
	}

	for (node = rbt_first(tables); node; node = rbt_first(tables)) {
		fts_trx_table_t*	ftt;
		fts_trx_table_t**	fttp;

		fttp = rbt_value(fts_trx_table_t*, node);
		ftt = *fttp;

		/* This can be NULL if a savepoint was released. */
		if (ftt->rows != NULL) {
			fts_trx_table_rows_free(ftt->rows);
			ftt->rows = NULL;
		}

		/* This can be NULL if a savepoint was released. */
		if (ftt->added_doc_ids != NULL) {
			fts_doc_ids_free(ftt->added_doc_ids);
			ftt->added_doc_ids = NULL;
		}

		/* The default savepoint name must be NULL. */
		if (ftt->docs_added_graph) {
			que_graph_free(ftt->docs_added_graph);
		}

		/* NOTE: We are responsible for free'ing the node */
		ut_free(rbt_remove_node(tables, node));
	}

	ut_a(rbt_empty(tables));
	rbt_free(tables);
	savepoint->tables = NULL;
}

/*********************************************************************//**
Free an FTS trx. */
void
fts_trx_free(
/*=========*/
	fts_trx_t*	fts_trx)		/* in, own: FTS trx */
{
	ulint		i;

	for (i = 0; i < ib_vector_size(fts_trx->savepoints); ++i) {
		fts_savepoint_t*	savepoint;

		savepoint = static_cast<fts_savepoint_t*>(
			ib_vector_get(fts_trx->savepoints, i));

		/* The default savepoint name must be NULL. */
		if (i == 0) {
			ut_a(savepoint->name == NULL);
		}

		fts_savepoint_free(savepoint);
	}

	for (i = 0; i < ib_vector_size(fts_trx->last_stmt); ++i) {
		fts_savepoint_t*	savepoint;

		savepoint = static_cast<fts_savepoint_t*>(
			ib_vector_get(fts_trx->last_stmt, i));

		/* The default savepoint name must be NULL. */
		if (i == 0) {
			ut_a(savepoint->name == NULL);
		}

		fts_savepoint_free(savepoint);
	}

	if (fts_trx->heap) {
		mem_heap_free(fts_trx->heap);
	}
}

/*********************************************************************//**
Extract the doc id from the FTS hidden column.
@return doc id that was extracted from rec */
doc_id_t
fts_get_doc_id_from_row(
/*====================*/
	dict_table_t*	table,			/*!< in: table */
	dtuple_t*	row)			/*!< in: row whose FTS doc id we
						want to extract.*/
{
	dfield_t*	field;
	doc_id_t	doc_id = 0;

	ut_a(table->fts->doc_col != ULINT_UNDEFINED);

	field = dtuple_get_nth_field(row, table->fts->doc_col);

	ut_a(dfield_get_len(field) == sizeof(doc_id));
	ut_a(dfield_get_type(field)->mtype == DATA_INT);

	doc_id = fts_read_doc_id(
		static_cast<const byte*>(dfield_get_data(field)));

	return(doc_id);
}

/** Extract the doc id from the record that belongs to index.
@param[in]	rec	record containing FTS_DOC_ID
@param[in]	index	index of rec
@param[in]	offsets	rec_get_offsets(rec,index)
@return doc id that was extracted from rec */
doc_id_t
fts_get_doc_id_from_rec(
	const rec_t*		rec,
	const dict_index_t*	index,
	const rec_offs*		offsets)
{
	ulint f = dict_col_get_index_pos(
		&index->table->cols[index->table->fts->doc_col], index);
	ulint len;
	doc_id_t doc_id = mach_read_from_8(
		rec_get_nth_field(rec, offsets, f, &len));
	ut_ad(len == 8);
	return doc_id;
}

/*********************************************************************//**
Search the index specific cache for a particular FTS index.
@return the index specific cache else NULL */
fts_index_cache_t*
fts_find_index_cache(
/*=================*/
	const fts_cache_t*	cache,		/*!< in: cache to search */
	const dict_index_t*	index)		/*!< in: index to search for */
{
	/* We cast away the const because our internal function, takes
	non-const cache arg and returns a non-const pointer. */
	return(static_cast<fts_index_cache_t*>(
		fts_get_index_cache((fts_cache_t*) cache, index)));
}

/*********************************************************************//**
Search cache for word.
@return the word node vector if found else NULL */
const ib_vector_t*
fts_cache_find_word(
/*================*/
	const fts_index_cache_t*index_cache,	/*!< in: cache to search */
	const fts_string_t*	text)		/*!< in: word to search for */
{
	ib_rbt_bound_t		parent;
	const ib_vector_t*	nodes = NULL;

	mysql_mutex_assert_owner(&index_cache->index->table->fts->cache->lock);

	/* Lookup the word in the rb tree */
	if (rbt_search(index_cache->words, &parent, text) == 0) {
		const fts_tokenizer_word_t*	word;

		word = rbt_value(fts_tokenizer_word_t, parent.last);

		nodes = word->nodes;
	}

	return(nodes);
}

/*********************************************************************//**
Append deleted doc ids to vector. */
void
fts_cache_append_deleted_doc_ids(
/*=============================*/
	fts_cache_t*		cache,		/*!< in: cache to use */
	ib_vector_t*		vector)		/*!< in: append to this vector */
{
  mysql_mutex_lock(&cache->deleted_lock);

  if (cache->deleted_doc_ids)
    for (ulint i= 0; i < ib_vector_size(cache->deleted_doc_ids); ++i)
    {
      doc_id_t *update= static_cast<doc_id_t*>(
        ib_vector_get(cache->deleted_doc_ids, i));
      ib_vector_push(vector, &update);
    }

  mysql_mutex_unlock(&cache->deleted_lock);
}

/*********************************************************************//**
Add the FTS document id hidden column. */
void
fts_add_doc_id_column(
/*==================*/
	dict_table_t*	table,	/*!< in/out: Table with FTS index */
	mem_heap_t*	heap)	/*!< in: temporary memory heap, or NULL */
{
	dict_mem_table_add_col(
		table, heap,
		FTS_DOC_ID.str,
		DATA_INT,
		dtype_form_prtype(
			DATA_NOT_NULL | DATA_UNSIGNED
			| DATA_BINARY_TYPE | DATA_FTS_DOC_ID, 0),
		sizeof(doc_id_t));
	DICT_TF2_FLAG_SET(table, DICT_TF2_FTS_HAS_DOC_ID);
}

/** Add new fts doc id to the update vector.
@param[in]	table		the table that contains the FTS index.
@param[in,out]	ufield		the fts doc id field in the update vector.
				No new memory is allocated for this in this
				function.
@param[in,out]	next_doc_id	the fts doc id that has been added to the
				update vector.  If 0, a new fts doc id is
				automatically generated.  The memory provided
				for this argument will be used by the update
				vector. Ensure that the life time of this
				memory matches that of the update vector.
@return the fts doc id used in the update vector */
doc_id_t
fts_update_doc_id(
	dict_table_t*	table,
	upd_field_t*	ufield,
	doc_id_t*	next_doc_id)
{
	doc_id_t	doc_id;
	dberr_t		error = DB_SUCCESS;

	if (*next_doc_id) {
		doc_id = *next_doc_id;
	} else {
		/* Get the new document id that will be added. */
		error = fts_get_next_doc_id(table, &doc_id);
	}

	if (error == DB_SUCCESS) {
		dict_index_t*	clust_index;
		dict_col_t*	col = dict_table_get_nth_col(
			table, table->fts->doc_col);

		ufield->exp = NULL;

		ufield->new_val.len = sizeof(doc_id);

		clust_index = dict_table_get_first_index(table);

		ufield->field_no = static_cast<unsigned>(
			dict_col_get_clust_pos(col, clust_index))
			& dict_index_t::MAX_N_FIELDS;
		dict_col_copy_type(col, dfield_get_type(&ufield->new_val));

		/* It is possible we update record that has
		not yet be sync-ed from last crash. */

		/* Convert to storage byte order. */
		ut_a(doc_id != FTS_NULL_DOC_ID);
		fts_write_doc_id((byte*) next_doc_id, doc_id);

		ufield->new_val.data = next_doc_id;
                ufield->new_val.ext = 0;
	}

	return(doc_id);
}

/** fts_t constructor.
@param[in]	table	table with FTS indexes
@param[in,out]	heap	memory heap where 'this' is stored */
fts_t::fts_t(
	const dict_table_t*	table,
	mem_heap_t*		heap)
	:
	added_synced(0), dict_locked(0),
	add_wq(NULL),
	cache(NULL),
	doc_col(ULINT_UNDEFINED), in_queue(false), sync_message(false),
	fts_heap(heap)
{
	ut_a(table->fts == NULL);

	ib_alloc_t*	heap_alloc = ib_heap_allocator_create(fts_heap);

	indexes = ib_vector_create(heap_alloc, sizeof(dict_index_t*), 4);

	dict_table_get_all_fts_indexes(table, indexes);
}

/** fts_t destructor. */
fts_t::~fts_t()
{
	ut_ad(add_wq == NULL);

	if (cache) {
		fts_cache_clear(cache);
		fts_cache_destroy(cache);
	}

	/* There is no need to call ib_vector_free() on this->indexes
	because it is stored in this->fts_heap. */
	mem_heap_free(fts_heap);
}

/*********************************************************************//**
Create an instance of fts_t.
@return instance of fts_t */
fts_t*
fts_create(
/*=======*/
	dict_table_t*	table)		/*!< in/out: table with FTS indexes */
{
	fts_t*		fts;
	mem_heap_t*	heap;

	heap = mem_heap_create(512);

	fts = static_cast<fts_t*>(mem_heap_alloc(heap, sizeof(*fts)));

	new(fts) fts_t(table, heap);

	return(fts);
}

/*********************************************************************//**
Take a FTS savepoint. */
UNIV_INLINE
void
fts_savepoint_copy(
/*===============*/
	const fts_savepoint_t*	src,	/*!< in: source savepoint */
	fts_savepoint_t*	dst)	/*!< out: destination savepoint */
{
	const ib_rbt_node_t*	node;
	const ib_rbt_t*		tables;

	tables = src->tables;

	for (node = rbt_first(tables); node; node = rbt_next(tables, node)) {

		fts_trx_table_t*	ftt_dst;
		const fts_trx_table_t**	ftt_src;

		ftt_src = rbt_value(const fts_trx_table_t*, node);

		ftt_dst = fts_trx_table_clone(*ftt_src);

		rbt_insert(dst->tables, &ftt_dst, &ftt_dst);
	}
}

/*********************************************************************//**
Take a FTS savepoint. */
void
fts_savepoint_take(
/*===============*/
	fts_trx_t*	fts_trx,	/*!< in: fts transaction */
	const void*	name)		/*!< in: savepoint */
{
	mem_heap_t*		heap;
	fts_savepoint_t*	savepoint;
	fts_savepoint_t*	last_savepoint;

	ut_a(name != NULL);

	heap = fts_trx->heap;

	/* The implied savepoint must exist. */
	ut_a(ib_vector_size(fts_trx->savepoints) > 0);

	last_savepoint = static_cast<fts_savepoint_t*>(
		ib_vector_last(fts_trx->savepoints));
	savepoint = fts_savepoint_create(fts_trx->savepoints, name, heap);

	if (last_savepoint->tables != NULL) {
		fts_savepoint_copy(last_savepoint, savepoint);
	}
}

/*********************************************************************//**
Lookup a savepoint instance.
@return 0 if not found */
static
ulint
fts_savepoint_lookup(
/*==================*/
	ib_vector_t*	savepoints,	/*!< in: savepoints */
	const void*	name)		/*!< in: savepoint */
{
  ut_a(ib_vector_size(savepoints) > 0);
  for (ulint i= 1; i < ib_vector_size(savepoints); ++i)
    if (name == static_cast<const fts_savepoint_t*>
        (ib_vector_get(savepoints, i))->name)
      return i;
  return 0;
}

/*********************************************************************//**
Release the savepoint data identified by  name. All savepoints created
after the named savepoint are kept.
@return DB_SUCCESS or error code */
void
fts_savepoint_release(
/*==================*/
	trx_t*		trx,		/*!< in: transaction */
	const void*	name)		/*!< in: savepoint name */
{
	ut_a(name != NULL);

	ib_vector_t*	savepoints = trx->fts_trx->savepoints;

	ut_a(ib_vector_size(savepoints) > 0);

	if (ulint i = fts_savepoint_lookup(savepoints, name)) {
		fts_savepoint_t*        savepoint;
		savepoint = static_cast<fts_savepoint_t*>(
			ib_vector_get(savepoints, i));

		if (i == ib_vector_size(savepoints) - 1) {
			/* If the savepoint is the last, we save its
			tables to the  previous savepoint. */
			fts_savepoint_t*	prev_savepoint;
			prev_savepoint = static_cast<fts_savepoint_t*>(
				ib_vector_get(savepoints, i - 1));

			ib_rbt_t*	tables = savepoint->tables;
			savepoint->tables = prev_savepoint->tables;
			prev_savepoint->tables = tables;
		}

		fts_savepoint_free(savepoint);
		ib_vector_remove(savepoints, *(void**)savepoint);

		/* Make sure we don't delete the implied savepoint. */
		ut_a(ib_vector_size(savepoints) > 0);
	}
}

/**********************************************************************//**
Refresh last statement savepoint. */
void
fts_savepoint_laststmt_refresh(
/*===========================*/
	trx_t*			trx)	/*!< in: transaction */
{

	fts_trx_t*              fts_trx;
	fts_savepoint_t*        savepoint;

	fts_trx = trx->fts_trx;

	savepoint = static_cast<fts_savepoint_t*>(
		ib_vector_pop(fts_trx->last_stmt));
	fts_savepoint_free(savepoint);

	ut_ad(ib_vector_is_empty(fts_trx->last_stmt));
	savepoint = fts_savepoint_create(fts_trx->last_stmt, NULL, NULL);
}

/********************************************************************
Undo the Doc ID add/delete operations in last stmt */
static
void
fts_undo_last_stmt(
/*===============*/
	fts_trx_table_t*	s_ftt,	/*!< in: Transaction FTS table */
	fts_trx_table_t*	l_ftt)	/*!< in: last stmt FTS table */
{
	ib_rbt_t*		s_rows;
	ib_rbt_t*		l_rows;
	const ib_rbt_node_t*	node;

	l_rows = l_ftt->rows;
	s_rows = s_ftt->rows;

	for (node = rbt_first(l_rows);
	     node;
	     node = rbt_next(l_rows, node)) {
		fts_trx_row_t*	l_row = rbt_value(fts_trx_row_t, node);
		ib_rbt_bound_t	parent;

		rbt_search(s_rows, &parent, &(l_row->doc_id));

		if (parent.result == 0) {
			fts_trx_row_t*	s_row = rbt_value(
				fts_trx_row_t, parent.last);

			switch (l_row->state) {
			case FTS_INSERT:
				ut_free(rbt_remove_node(s_rows, parent.last));
				break;

			case FTS_DELETE:
				if (s_row->state == FTS_NOTHING) {
					s_row->state = FTS_INSERT;
				} else if (s_row->state == FTS_DELETE) {
					ut_free(rbt_remove_node(
						s_rows, parent.last));
				}
				break;

			/* FIXME: Check if FTS_MODIFY need to be addressed */
			case FTS_MODIFY:
			case FTS_NOTHING:
				break;
			default:
				ut_error;
			}
		}
	}
}

/**********************************************************************//**
Rollback to savepoint indentified by name.
@return DB_SUCCESS or error code */
void
fts_savepoint_rollback_last_stmt(
/*=============================*/
	trx_t*		trx)		/*!< in: transaction */
{
	ib_vector_t*		savepoints;
	fts_savepoint_t*	savepoint;
	fts_savepoint_t*	last_stmt;
	fts_trx_t*		fts_trx;
	ib_rbt_bound_t		parent;
	const ib_rbt_node_t*    node;
	ib_rbt_t*		l_tables;
	ib_rbt_t*		s_tables;

	fts_trx = trx->fts_trx;
	savepoints = fts_trx->savepoints;

	savepoint = static_cast<fts_savepoint_t*>(ib_vector_last(savepoints));
	last_stmt = static_cast<fts_savepoint_t*>(
		ib_vector_last(fts_trx->last_stmt));

	l_tables = last_stmt->tables;
	s_tables = savepoint->tables;

	for (node = rbt_first(l_tables);
	     node;
	     node = rbt_next(l_tables, node)) {

		fts_trx_table_t**	l_ftt;

		l_ftt = rbt_value(fts_trx_table_t*, node);

		rbt_search_cmp(
			s_tables, &parent, &(*l_ftt)->table,
			fts_ptr1_ptr2_cmp, nullptr);

		if (parent.result == 0) {
			fts_trx_table_t**	s_ftt;

			s_ftt = rbt_value(fts_trx_table_t*, parent.last);

			fts_undo_last_stmt(*s_ftt, *l_ftt);
		}
	}
}

/**********************************************************************//**
Rollback to savepoint indentified by name.
@return DB_SUCCESS or error code */
void
fts_savepoint_rollback(
/*===================*/
	trx_t*		trx,		/*!< in: transaction */
	const void*	name)		/*!< in: savepoint */
{
	ib_vector_t*	savepoints;

	ut_a(name != NULL);

	savepoints = trx->fts_trx->savepoints;

	/* We pop all savepoints from the the top of the stack up to
	and including the instance that was found. */
	ulint i = fts_savepoint_lookup(savepoints, name);

	if (i == 0) {
		/* fts_trx_create() must have been invoked after
		this savepoint had been created, and we must roll back
		everything. */
		i = 1;
	}

	{
		fts_savepoint_t*	savepoint;

		while (ib_vector_size(savepoints) > i) {
			savepoint = static_cast<fts_savepoint_t*>(
				ib_vector_pop(savepoints));

			if (savepoint->name != NULL) {
				/* Since name was allocated on the heap, the
				memory will be released when the transaction
				completes. */
				savepoint->name = NULL;

				fts_savepoint_free(savepoint);
			}
		}

		/* Pop all a elements from the top of the stack that may
		have been released. We have to be careful that we don't
		delete the implied savepoint. */

		for (savepoint = static_cast<fts_savepoint_t*>(
				ib_vector_last(savepoints));
		     ib_vector_size(savepoints) > 1
		     && savepoint->name == NULL;
		     savepoint = static_cast<fts_savepoint_t*>(
				ib_vector_last(savepoints))) {

			ib_vector_pop(savepoints);
		}

		/* Make sure we don't delete the implied savepoint. */
		ut_a(ib_vector_size(savepoints) > 0);

		/* Restore the savepoint. */
		fts_savepoint_take(trx->fts_trx, name);
	}
}

bool fts_check_aux_table(const char *name,
                         table_id_t *table_id,
                         index_id_t *index_id)
{
  ulint len= strlen(name);
  const char* ptr;
  const char* end= name + len;

  ut_ad(len <= MAX_FULL_NAME_LEN);
  ptr= static_cast<const char*>(memchr(name, '/', len));
  IF_WIN(if (!ptr) ptr= static_cast<const char*>(memchr(name, '\\', len)), );

  if (!ptr)
    return false;

  /* We will start the match after the '/' */
  ++ptr;
  len= end - ptr;

  /* All auxiliary tables are prefixed with "FTS_" and the name
  length will be at the very least greater than 20 bytes. */
  if (len > 24 && !memcmp(ptr, "FTS_", 4))
  {
    /* Skip the prefix. */
    ptr+= 4;
    len-= 4;

    const char *table_id_ptr= ptr;
    /* Skip the table id. */
    ptr= static_cast<const char*>(memchr(ptr, '_', len));

    if (!ptr)
      return false;

    /* Skip the underscore. */
    ++ptr;
    ut_ad(end > ptr);
    len= end - ptr;

    sscanf(table_id_ptr, UINT64PFx, table_id);
    /* First search the common table suffix array. */
    for (ulint i = 0; fts_common_tables[i]; ++i)
    {
      if (!strncmp(ptr, fts_common_tables[i], len))
        return true;
    }

    /* Could be obsolete common tables. */
    if ((len == 5 && !memcmp(ptr, "ADDED", len)) ||
        (len == 9 && !memcmp(ptr, "STOPWORDS", len)))
      return true;

    const char* index_id_ptr= ptr;
    /* Skip the index id. */
    ptr= static_cast<const char*>(memchr(ptr, '_', len));
    if (!ptr)
      return false;

    sscanf(index_id_ptr, UINT64PFx, index_id);

    /* Skip the underscore. */
    ++ptr;
    ut_a(end > ptr);
    len= end - ptr;

    if (len <= 4)
      return false;

    len-= 4; /* .ibd suffix */

    if (len > 7)
      return false;

    /* Search the FT index specific array. */
    for (ulint i = 0; i < FTS_NUM_AUX_INDEX; ++i)
    {
      if (!memcmp(ptr, "INDEX_", len - 1))
        return true;
    }

    /* Other FT index specific table(s). */
    if (len == 6 && !memcmp(ptr, "DOC_ID", len))
      return true;
  }

  return false;
}

/**********************************************************************//**
Check whether user supplied stopword table is of the right format.
Caller is responsible to hold dictionary locks.
@param stopword_table_name   table name
@param row_end   name of the system-versioning end column, or "value"
@return the stopword column charset
@retval NULL if the table does not exist or qualify */
CHARSET_INFO*
fts_valid_stopword_table(
/*=====================*/
	const char*	stopword_table_name,	/*!< in: Stopword table
						name */
	const char**	row_end) /* row_end value of system-versioned table */
{
	dict_table_t*	table;
	dict_col_t*     col = NULL;

	if (!stopword_table_name) {
		return(NULL);
	}

	table = dict_sys.load_table(
		{stopword_table_name, strlen(stopword_table_name)});

	if (!table) {
		ib::error() << "User stopword table " << stopword_table_name
			<< " does not exist.";

		return(NULL);
	} else {
		if (strcmp(dict_table_get_col_name(table, 0).str, "value")) {
			ib::error() << "Invalid column name for stopword"
				" table " << stopword_table_name << ". Its"
				" first column must be named as 'value'.";

			return(NULL);
		}

		col = dict_table_get_nth_col(table, 0);

		if (col->mtype != DATA_VARCHAR
		    && col->mtype != DATA_VARMYSQL) {
			ib::error() << "Invalid column type for stopword"
				" table " << stopword_table_name << ". Its"
				" first column must be of varchar type";

			return(NULL);
		}
	}

	ut_ad(col);
	ut_ad(!table->versioned() || col->ind != table->vers_end);

	if (row_end) {
		*row_end = table->versioned()
			? dict_table_get_col_name(table, table->vers_end).str
			: "value"; /* for fts_load_user_stopword() */
	}

	return(fts_get_charset(col->prtype));
}

/**********************************************************************//**
This function loads the stopword into the FTS cache. It also
records/fetches stopword configuration to/from FTS configure
table, depending on whether we are creating or reloading the
FTS.
@return true if load operation is successful */
bool
fts_load_stopword(
/*==============*/
	const dict_table_t*
			table,			/*!< in: Table with FTS */
	trx_t*		trx,			/*!< in: Transactions */
	const char*	session_stopword_table,	/*!< in: Session stopword table
						name */
	bool		stopword_is_on,		/*!< in: Whether stopword
						option is turned on/off */
	bool		reload)			/*!< in: Whether it is
						for reloading FTS table */
{
	fts_table_t	fts_table;
	fts_string_t	str;
	dberr_t		error = DB_SUCCESS;
	ulint		use_stopword;
	fts_cache_t*	cache;
	const char*	stopword_to_use = NULL;
	ibool		new_trx = FALSE;
	byte		str_buffer[MAX_FULL_NAME_LEN + 1];

	FTS_INIT_FTS_TABLE(&fts_table, "CONFIG", FTS_COMMON_TABLE, table);

	cache = table->fts->cache;

	if (!reload && !(cache->stopword_info.status & STOPWORD_NOT_INIT)) {
		return true;
	}

	if (!trx) {
		trx = trx_create();
#ifdef UNIV_DEBUG
		trx->start_line = __LINE__;
		trx->start_file = __FILE__;
#endif
		trx_start_internal_low(trx, !high_level_read_only);
		trx->op_info = "upload FTS stopword";
		new_trx = TRUE;
	}

	/* First check whether stopword filtering is turned off */
	if (reload) {
		error = fts_config_get_ulint(
			trx, &fts_table, FTS_USE_STOPWORD, &use_stopword);
	} else {
		use_stopword = (ulint) stopword_is_on;

		error = fts_config_set_ulint(
			trx, &fts_table, FTS_USE_STOPWORD, use_stopword);
	}

	if (error != DB_SUCCESS) {
		goto cleanup;
	}

	/* If stopword is turned off, no need to continue to load the
	stopword into cache, but still need to do initialization */
	if (!use_stopword) {
		cache->stopword_info.status = STOPWORD_OFF;
		goto cleanup;
	}

	if (reload) {
		/* Fetch the stopword table name from FTS config
		table */
		str.f_n_char = 0;
		str.f_str = str_buffer;
		str.f_len = sizeof(str_buffer) - 1;

		error = fts_config_get_value(
			trx, &fts_table, FTS_STOPWORD_TABLE_NAME, &str);

		if (error != DB_SUCCESS) {
			goto cleanup;
		}

		if (*str.f_str) {
			stopword_to_use = (const char*) str.f_str;
		}
	} else {
		stopword_to_use = session_stopword_table;
	}

	if (stopword_to_use
	    && fts_load_user_stopword(table->fts, stopword_to_use,
				      &cache->stopword_info)) {
		/* Save the stopword table name to the configure
		table */
		if (!reload) {
			str.f_n_char = 0;
			str.f_str = (byte*) stopword_to_use;
			str.f_len = strlen(stopword_to_use);

			error = fts_config_set_value(
				trx, &fts_table, FTS_STOPWORD_TABLE_NAME, &str);
		}
	} else {
		/* Load system default stopword list */
		fts_load_default_stopword(&cache->stopword_info);
	}

cleanup:
	if (new_trx) {
		if (error == DB_SUCCESS) {
			fts_sql_commit(trx);
		} else {
			fts_sql_rollback(trx);
		}

		trx->free();
	}

	if (!cache->stopword_info.cached_stopword) {
		cache->stopword_info.cached_stopword = rbt_create_arg_cmp(
			sizeof(fts_tokenizer_word_t), innobase_fts_text_cmp,
			&my_charset_latin1);
	}

	return error == DB_SUCCESS;
}

/** Callback function when we initialize the FTS at the start up
time. It recovers the maximum Doc IDs presented in the current table.
Tested by innodb_fts.crash_recovery
@param index    clustered index
@param rec      clustered index record
@param offsets  offsets to the clustered index record
@param user_arg points to fts_fetch_doc_id. fetch->user_arg points
                to dict_table_t
@return: always returns TRUE */
static
bool fts_init_get_doc_id(dict_index_t *index, const rec_t *rec,
                         const rec_offs *offsets, void *user_arg)
{
  fts_fetch_doc_id *fetch= static_cast<fts_fetch_doc_id*>(user_arg);
  dict_table_t *table= static_cast<dict_table_t *>(fetch->user_arg);
  fts_cache_t *cache= table->fts->cache;
  ut_ad(fetch->field_to_fetch.size() == 1);
  ut_ad(ib_vector_is_empty(cache->get_docs));
  ulint len;
  const byte *data= rec_get_nth_field(rec, offsets,
                                fetch->field_to_fetch.front(), &len);
  doc_id_t doc_id = static_cast<doc_id_t>(mach_read_from_8(data));

  if (doc_id >= cache->next_doc_id)
    cache->next_doc_id = doc_id + 1;
  return true;
}

/** Callback function when we initialize the FTS at the start up
time. It recovers Doc IDs that have not sync-ed to the auxiliary
table, and require to bring them back into FTS index.
@param   index    clustered index
@param   rec      clustered index record
@param   offsets  offset to clustered index record
@param   user_arg points to fts_fetch_doc_id &
                  fts_fetch_doc_id->user_arg points to fts_get_doc_t
@return: always returns TRUE */
static
bool fts_init_recover_doc(dict_index_t *index, const rec_t *rec,
                           const rec_offs *offsets, void *user_arg)
{
  ut_ad(dict_index_is_clust(index));
  fts_fetch_doc_id *fetch= static_cast<fts_fetch_doc_id*>(user_arg);
  fts_get_doc_t *get_doc= static_cast<fts_get_doc_t*>(fetch->user_arg);
  ut_ad(get_doc->cache);
  fts_cache_t *cache= get_doc->cache;
  st_mysql_ftparser *parser= get_doc->index_cache->index->parser;
  doc_id_t doc_id= FTS_NULL_DOC_ID;
  ulint doc_len= 0;

  fts_doc_t doc;
  fts_doc_init(&doc);
  doc.found = TRUE;

  bool first_doc= true;
  for (uint32_t i= 0; i < fetch->field_to_fetch.size(); i++)
  {
    ulint field_no= fetch->field_to_fetch[i];
    if (i == 0)
    {
      ulint len;
      const byte *data= rec_get_nth_field(rec, offsets, field_no, &len);
      ut_ad(len == 8);
      doc_id= static_cast<doc_id_t>(mach_read_from_8(
                static_cast<const byte*>(data)));
      continue;
    }

    if (rec_offs_nth_sql_null(offsets, field_no))
      continue;

    if (!get_doc->index_cache->charset)
      get_doc->index_cache->charset=
        fts_get_charset(index->fields[field_no].col->prtype);

    doc.charset = get_doc->index_cache->charset;
    if (rec_offs_nth_extern(offsets, field_no))
      doc.text.f_str=
        static_cast<byte*>(
          btr_rec_copy_externally_stored_field(
            rec, offsets, 0, field_no, &doc.text.f_len,
            static_cast<mem_heap_t*>(doc.self_heap->arg)));
    else
    {
      ulint len;
      const byte *data= rec_get_nth_field(rec, offsets, field_no, &len);
      doc.text.f_str= const_cast<byte*>(data);
      doc.text.f_len= len;
    }
    if (first_doc)
    {
      fts_tokenize_document(&doc, NULL, parser);
      first_doc= false;
    }
    else fts_tokenize_document_next(&doc, ++doc_len, NULL, parser);
    doc_len+= doc.text.f_len;
  }

  fts_cache_add_doc(cache, get_doc->index_cache, doc_id, doc.tokens);
  fts_doc_free(&doc);
  cache->added++;
  if (doc_id >= cache->next_doc_id)
    cache->next_doc_id = doc_id + 1;
  return true;
}

/**********************************************************************//**
This function brings FTS index in sync when FTS index is first
used. There are documents that have not yet sync-ed to auxiliary
tables from last server abnormally shutdown, we will need to bring
such document into FTS cache before any further operations */
void
fts_init_index(
/*===========*/
	dict_table_t*	table,		/*!< in: Table with FTS */
	bool		has_cache_lock)	/*!< in: Whether we already have
					cache lock */
{
	dict_index_t*   index;
	doc_id_t        start_doc;
	fts_get_doc_t*  get_doc = NULL;
	fts_cache_t*    cache = table->fts->cache;
	bool		need_init = false;

	/* First check cache->get_docs is initialized */
	if (!has_cache_lock) {
		mysql_mutex_lock(&cache->lock);
	}

	mysql_mutex_lock(&cache->init_lock);
	if (cache->get_docs == NULL) {
		cache->get_docs = fts_get_docs_create(cache);
	}
	mysql_mutex_unlock(&cache->init_lock);

	if (table->fts->added_synced) {
		goto func_exit;
	}

	need_init = true;

	start_doc = cache->synced_doc_id;

	if (!start_doc) {
		trx_t *trx = trx_create();
		trx_start_internal_read_only(trx);
		dberr_t err= fts_read_synced_doc_id(table, &start_doc, trx);
		fts_sql_commit(trx);
		trx->free();
		if (err != DB_SUCCESS) {
			goto func_exit;
		}
		if (start_doc) {
			start_doc--;
		}
		cache->synced_doc_id = start_doc;
	}

	/* No FTS index, this is the case when previous FTS index
	dropped, and we re-initialize the Doc ID system for subsequent
	insertion */
	if (ib_vector_is_empty(cache->get_docs)) {
		index = table->fts_doc_id_index;

		ut_a(index);

		fts_doc_fetch_by_doc_id(NULL, start_doc, index,
					FTS_FETCH_DOC_BY_ID_LARGE,
					fts_init_get_doc_id, table);
	} else {
		if (table->fts->cache->stopword_info.status
		    & STOPWORD_NOT_INIT) {
			fts_load_stopword(table, NULL, NULL, true, true);
		}

		for (ulint i = 0; i < ib_vector_size(cache->get_docs); ++i) {
			get_doc = static_cast<fts_get_doc_t*>(
				ib_vector_get(cache->get_docs, i));

			index = get_doc->index_cache->index;

			fts_doc_fetch_by_doc_id(NULL, start_doc, index,
						FTS_FETCH_DOC_BY_ID_LARGE,
						fts_init_recover_doc, get_doc);
		}
	}

	table->fts->added_synced = true;

func_exit:
	if (!has_cache_lock) {
		mysql_mutex_unlock(&cache->lock);
	}

	if (need_init) {
		dict_sys.lock(SRW_LOCK_CALL);
		/* Register the table with the optimize thread. */
		fts_optimize_add_table(table);
		dict_sys.unlock();
	}
}

/** Callback function for fetching the config value. */
bool read_fts_config(dict_index_t *index, const rec_t *rec,
                     const rec_offs *offsets, void *user_arg)
{
  ut_ad(dict_index_is_clust(index));
  ulint len;
  fts_string_t *value= static_cast<fts_string_t*>(user_arg);
  
  for (uint32_t i= 0; i < index->n_fields; i++)
  {
    if (strcmp(index->fields[i].name, "value") == 0)
    {
      const byte *data= rec_get_nth_field(rec, offsets, i, &len);
      if (len != UNIV_SQL_NULL)
      {
        ulint max_len= ut_min(value->f_len - 1, len);
        memcpy(value->f_str, data, max_len);
        value->f_len= max_len;
        value->f_str[value->f_len]= '\0';
        return true;
      }
    }
  }

  return false;
}

void FTSQueryRunner::create_query_thread() noexcept
{
  ut_ad(m_heap);
  m_thr= que_thr_create(que_fork_create(m_heap), m_heap, nullptr);
}

void FTSQueryRunner::build_tuple(dict_index_t *index, uint32_t n_fields,
                                 uint32_t n_uniq) noexcept
{
  if (m_tuple && m_tuple->n_fields == index->n_fields) return;
  if (!n_fields) n_fields= index->n_fields;
  if (!n_uniq) n_uniq= index->n_uniq;
  m_tuple= dtuple_create(m_heap, n_fields);
  dtuple_set_n_fields_cmp(m_tuple, n_uniq);
  dict_index_copy_types(m_tuple, index, n_fields);
}

void FTSQueryRunner::build_clust_ref(dict_index_t *index) noexcept
{
  if (m_clust_ref && m_clust_ref->n_fields == index->n_fields) return;
  m_clust_ref= dtuple_create(m_heap, index->n_uniq);
  dtuple_set_n_fields_cmp(m_clust_ref, index->n_uniq);
  dict_index_copy_types(m_clust_ref, index, m_clust_ref->n_fields);
}

void FTSQueryRunner::assign_config_fields(const char *name, const void* value,
                                          ulint value_len) noexcept
{
  dfield_set_data(dtuple_get_nth_field(m_tuple, 0), name, strlen(name));

  if (m_tuple->n_fields == 1) return;

  if (m_sys_buf == nullptr)
  {
    m_sys_buf= static_cast<byte*>(
      mem_heap_zalloc(m_heap, DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN));
    dfield_set_data(dtuple_get_nth_field(m_tuple, 1),
                    m_sys_buf, DATA_TRX_ID_LEN);
    dfield_set_data(dtuple_get_nth_field(m_tuple, 2),
                    &m_sys_buf[DATA_TRX_ID_LEN], DATA_ROLL_PTR_LEN);
  }
  else
  {
    memset(dtuple_get_nth_field(m_tuple, 1)->data, 0x00, DATA_TRX_ID_LEN);
    memset(dtuple_get_nth_field(m_tuple, 2)->data, 0x00, DATA_ROLL_PTR_LEN);
  }

  if (value)
    dfield_set_data(dtuple_get_nth_field(m_tuple, 3), value, value_len);
}

void FTSQueryRunner::assign_common_table_fields(doc_id_t *doc_id) noexcept
{
  dfield_set_data(dtuple_get_nth_field(m_tuple, 0), doc_id,
                  sizeof(doc_id_t));
  if (m_tuple->n_fields == 1) return;
  if (!m_sys_buf)
  {
    m_sys_buf= static_cast<byte*>(
      mem_heap_zalloc(m_heap, DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN));
    dfield_set_data(dtuple_get_nth_field(m_tuple, 1), m_sys_buf,
                    DATA_TRX_ID_LEN);
    dfield_set_data(dtuple_get_nth_field(m_tuple, 2),
                    &m_sys_buf[DATA_TRX_ID_LEN], DATA_ROLL_PTR_LEN);
  }
  else
  {
    memset(dtuple_get_nth_field(m_tuple, 1)->data, 0x00, DATA_TRX_ID_LEN);
    memset(dtuple_get_nth_field(m_tuple, 2)->data, 0x00, DATA_ROLL_PTR_LEN);
  }
}

void FTSQueryRunner::assign_aux_table_fields(const byte *word,
                                             ulint word_len,
                                             const fts_node_t *node) noexcept
{
  dfield_set_data(dtuple_get_nth_field(m_tuple, 0), word, word_len);
  if (m_tuple->n_fields == 1) return;
  if (!m_sys_buf)
  {
    byte *first_doc_id= static_cast<byte*>(mem_heap_zalloc(m_heap, 8));
    mach_write_to_8(first_doc_id, node->first_doc_id);
    dfield_set_data(dtuple_get_nth_field(m_tuple, 1), first_doc_id, 8);

    m_sys_buf= static_cast<byte*>(
      mem_heap_zalloc(m_heap, DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN));
    dfield_set_data(dtuple_get_nth_field(m_tuple, 2),
                    m_sys_buf, DATA_TRX_ID_LEN);
    dfield_set_data(dtuple_get_nth_field(m_tuple, 3),
                    &m_sys_buf[DATA_TRX_ID_LEN], DATA_ROLL_PTR_LEN);
    byte *last_doc_id= static_cast<byte*>(mem_heap_zalloc(m_heap, 8));
    mach_write_to_8(last_doc_id, node->last_doc_id);
    dfield_set_data(dtuple_get_nth_field(m_tuple, 4), last_doc_id, 8);

    byte *doc_count= static_cast<byte*>(mem_heap_zalloc(m_heap, 4));
    mach_write_to_4(doc_count, node->doc_count);
    dfield_set_data(dtuple_get_nth_field(m_tuple, 5), doc_count, 4);
  }
  else
  {
    mach_write_to_8(static_cast<byte*>(dtuple_get_nth_field(m_tuple, 1)->data),
                    node->first_doc_id);
    memset(dtuple_get_nth_field(m_tuple, 2)->data, 0x00, DATA_TRX_ID_LEN);
    memset(dtuple_get_nth_field(m_tuple, 3)->data, 0x00, DATA_ROLL_PTR_LEN);
    mach_write_to_8(static_cast<byte*>(dtuple_get_nth_field(m_tuple, 4)->data),
                    node->last_doc_id);
    mach_write_to_4(static_cast<byte*>(dtuple_get_nth_field(m_tuple, 5)->data),
                    node->doc_count);
  }
  dfield_set_data(dtuple_get_nth_field(m_tuple, 6), node->ilist,
                  node->ilist_size);
}

dberr_t FTSQueryRunner::handle_wait(dberr_t err, bool table_lock) noexcept
{
  m_trx->error_state= err;
  m_thr->lock_state= QUE_THR_LOCK_ROW;
  if (table_lock)
    m_thr->lock_state= QUE_THR_LOCK_TABLE;
  if (m_trx->lock.wait_thr)
  {
    if (lock_wait(m_thr) == DB_SUCCESS)
    {
      m_thr->lock_state= QUE_THR_LOCK_NOLOCK;
      return DB_SUCCESS;
    }
  }
  return m_trx->error_state;
}

dict_table_t *FTSQueryRunner::open_table(fts_table_t *fts_table, dberr_t *err,
                                         bool dict_locked) noexcept
{
  char table_name[MAX_FULL_NAME_LEN];
  fts_get_table_name(fts_table, table_name, dict_locked);
  dict_table_t *table= dict_table_open_on_name(table_name, dict_locked,
                                               DICT_ERR_IGNORE_NONE);
  if (table == nullptr) *err= DB_TABLE_NOT_FOUND;
  return table;
}

dberr_t FTSQueryRunner::prepare_for_write(dict_table_t *table) noexcept
{
  if (!m_thr) create_query_thread();
  trx_start_if_not_started(m_trx, true);
  m_thr->graph->trx = m_trx;
  if (!m_trx->read_view.is_open())
    m_trx->read_view.open(m_trx);
  dberr_t err= DB_SUCCESS;
lock_again:
  err= lock_table(table, nullptr, LOCK_IX, m_thr);
  if (err)
  {
    err= handle_wait(err, true);
    if (err == DB_SUCCESS)
      goto lock_again;
  }
  return err;
}

dberr_t FTSQueryRunner::prepare_for_read(dict_table_t *table) noexcept
{
  if (!m_thr) create_query_thread();
  trx_start_if_not_started(m_trx, false);
  m_thr->graph->trx = m_trx;
  if (!m_trx->read_view.is_open())
    m_trx->read_view.open(m_trx);
  dberr_t err= DB_SUCCESS;
lock_again:
  err= lock_table(table, nullptr, LOCK_IS, m_thr);
  if (err)
  {
    err= handle_wait(err, true);
    if (err == DB_SUCCESS)
      goto lock_again;
  }
  return err;
}

dberr_t FTSQueryRunner::write_record(dict_table_t *table) noexcept
{
  dberr_t err= DB_SUCCESS;
run_again:
  err= row_ins_clust_index_entry(dict_table_get_first_index(table),
                                 m_tuple, m_thr, 0);
  if (err)
  {
    err= handle_wait(err);
    if (err == DB_SUCCESS)
      goto run_again;
  }
  return err;
}

dberr_t FTSQueryRunner::lock_or_sees_rec(dict_index_t *index, const rec_t *rec,
                                         const rec_t **out_rec,
                                         rec_offs **offsets, mtr_t *mtr,
                                         fts_operation op) noexcept
{
  uint32_t lock_mode= LOCK_REC_NOT_GAP;
  buf_block_t *block= btr_pcur_get_block(m_pcur);
  dberr_t err= DB_SUCCESS;
  bool supremum= page_rec_is_supremum(rec);
  *out_rec= rec;
  switch (op) {
  case SELECT_UPDATE:
  case REMOVE:
    ut_ad(index->is_clust());
    if (supremum) lock_mode= LOCK_ORDINARY;
    err= lock_clust_rec_read_check_and_lock(0, block, rec, index, *offsets,
                                            m_lock_type, lock_mode, m_thr);
    if (err == DB_SUCCESS_LOCKED_REC) err= DB_SUCCESS;
    break;
  case READ:
    if (supremum) return err;

    if (index->is_clust())
    {
      /* Construct the record based on transaction read view */
      const trx_id_t id= row_get_rec_trx_id(rec, index, *offsets);
      ReadView *view= &m_trx->read_view;
      if (view->changes_visible(id));
      else if (id < view->low_limit_id() || id < trx_sys.get_max_trx_id())
      {
        rec_t *old_vers;
        err= row_vers_build_for_consistent_read(rec, mtr, index, offsets,
                                                view, &m_heap, m_heap, 
                                                &old_vers, nullptr);
        if (err) return err;
        *out_rec= old_vers;
      }
    }
    else
    {
      /* Do lookup and construct the clustered index record */
      dict_index_t *clust_index= dict_table_get_first_index(index->table);
      if (!m_clust_pcur)
        m_clust_pcur= static_cast<btr_pcur_t*>(
          mem_heap_zalloc(m_heap, sizeof(btr_pcur_t)));
      build_clust_ref(clust_index);

      Row_sel_get_clust_rec_for_mysql row_sel_get_clust_rec_for_mysql;

      err= row_sel_get_clust_rec_for_mysql(m_clust_pcur, m_pcur,
                                           m_clust_ref, m_lock_type,
                                           m_trx, index, rec, m_thr,
                                           out_rec, offsets, &m_heap,
                                           &m_old_vers_heap, nullptr, mtr);
    }
    break;
  default: err= DB_CORRUPTION;
  }
  return err;
}

static
int fts_cmp_rec_dtuple_pattern(const dtuple_t *dtuple, const rec_t *rec,
                               const dict_index_t *index,
                               const rec_offs *offsets) noexcept
{
  for (uint32_t i= 0; i < dtuple->n_fields; i++)
  {
    const byte*	rec_b_ptr;
    const dfield_t* dtuple_field= dtuple_get_nth_field(dtuple, i);
    const byte*	dtuple_b_ptr=
      static_cast<const byte*>(dfield_get_data(dtuple_field));
    const dtype_t* type= dfield_get_type(dtuple_field);
    ulint dtuple_f_len= dfield_get_len(dtuple_field);
    ulint rec_f_len;

    ut_ad(!rec_offs_nth_extern(offsets, i));
    ut_ad(!rec_offs_nth_default(offsets, i));

    rec_b_ptr= rec_get_nth_field(rec, offsets, i, &rec_f_len);
    ut_ad(!dfield_is_ext(dtuple_field));
    rec_f_len= dtuple_f_len;
    int ret= cmp_data(type->mtype, type->prtype, false, dtuple_b_ptr,
                      dtuple_f_len, rec_b_ptr, rec_f_len);
    if (ret) return ret;
  }
  return 0;
}

dberr_t FTSQueryRunner::record_executor(dict_index_t *index, fts_operation op,
                                        fts_match_key match_op,
                                        page_cur_mode_t mode,
                                        fts_sql_callback *func,
                                        void *user_arg) noexcept
{
  if (m_pcur == nullptr)
    m_pcur= static_cast<btr_pcur_t*>(mem_heap_zalloc(m_heap,
                                                     sizeof(btr_pcur_t)));

  btr_latch_mode latch_mode;
  /* Determine the latch mode and lock type based on statement */
  switch (op) {
  case REMOVE:
  case SELECT_UPDATE:
    latch_mode= BTR_MODIFY_LEAF;
    m_lock_type= LOCK_X;
    break;
  case READ:
    latch_mode= BTR_SEARCH_LEAF;
    m_lock_type= LOCK_NONE;
    break;
  default: return DB_CORRUPTION;
  }

  ut_ad(is_stopword_table ||
        strstr(index->name(), FTS_DOC_ID_INDEX.str) ||
        strstr(index->table->name.m_name, "/FTS_"));
  mtr_t mtr;
  rec_t *rec= nullptr;
  dberr_t err= DB_SUCCESS;
  bool restore_cursor= false;
  bool move_next= false;
  /* If the function executor can't handle any more records then
  abort the operation and retry again */
  bool interrupt= false;
  const rec_t *clust_rec= nullptr;
  dict_table_t *table= index->table;
  dict_index_t *clust_index= dict_table_get_first_index(table);
  bool need_to_access_clust= index != clust_index;

  mtr.start();
  mtr.set_named_space(index->table->space);
  m_pcur->btr_cur.page_cur.index= index;
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs *offsets= offsets_;
  rec_offs_init(offsets_);

reopen_index:
  if (restore_cursor)
  {
    /* Restore the cursor */
    auto status= m_pcur->restore_position(latch_mode, &mtr);
    if (status == btr_pcur_t::SAME_ALL)
    {
      restore_cursor= false;
      if (move_next)
      {
        move_next= false;
        goto next_rec;
      }
    }
    else err= DB_CORRUPTION;
  }
  else if (m_tuple)
    err= btr_pcur_open_with_no_init(m_tuple, mode, latch_mode,
                                    m_pcur, &mtr);
  else err= m_pcur->open_leaf(true, index, latch_mode, &mtr);

  if (err)
  {
func_exit:
    mtr.commit();
    return err;
  }

rec_loop:
  rec= btr_pcur_get_rec(m_pcur);
  if (page_rec_is_infimum(rec))
    goto next_rec;

  offsets= rec_get_offsets(rec, index, offsets, index->n_core_fields,
                           ULINT_UNDEFINED, &m_heap);

  if (page_rec_is_supremum(rec))
  {
    /* lock the supremum record based on the operation */
    err= lock_or_sees_rec(index, rec, &clust_rec, &offsets, &mtr, op);
    if (err) goto lock_wait_or_error;
    goto next_rec;
  }

  if (m_tuple)
  {
    /* In case of unique match, InnoDB should compare the tuple with record */
    if (match_op == MATCH_UNIQUE &&
        cmp_dtuple_rec(m_tuple, rec, index, offsets) != 0)
    {
      err= DB_RECORD_NOT_FOUND;
      goto func_exit;
    }

    if (match_op == MATCH_PREFIX &&
       cmp_dtuple_is_prefix_of_rec(m_tuple, rec, index, offsets) == 0)
    {
      err= DB_RECORD_NOT_FOUND;
      goto func_exit;
    }

    /* Searching tuple field can be pattern of the record key. So compare
    the tuple field data with record only till tuple field length */
    if (match_op == MATCH_PATTERN &&
        fts_cmp_rec_dtuple_pattern(m_tuple, rec, index, offsets))
    {
      err= DB_RECORD_NOT_FOUND;
      goto func_exit;
    }
  }

  err= lock_or_sees_rec(index, rec, &clust_rec, &offsets, &mtr, op);

  if (err) goto lock_wait_or_error;

  if (!need_to_access_clust &&
      (clust_rec == nullptr ||
       rec_get_deleted_flag(clust_rec, dict_table_is_comp(table))))
    goto next_rec;

  /* Found the suitable record and do the operation based on the statement */
  switch (op) {
  case REMOVE:
    if (dict_index_is_clust(index) && clust_rec)
    {
      ut_ad(clust_rec == rec);
      err= btr_cur_del_mark_set_clust_rec(btr_pcur_get_block(m_pcur),
                                          rec, index, offsets, m_thr,
                                          m_tuple, &mtr);
    }
    else err= DB_CORRUPTION;
    break;
  case READ:
  case SELECT_UPDATE:
    if (func && clust_rec)
      interrupt= !((*func)(clust_index, clust_rec, offsets, user_arg));
    break;
  default: err= DB_CORRUPTION;
  }

  if (err == DB_SUCCESS)
  {
    /* Avoid storing and restoring of cursor if we are doing READ_ALL
    operation directly on clustered index */
    if (!interrupt && !need_to_access_clust && op == READ
        && match_op != MATCH_UNIQUE)
      goto next_rec;
  }

lock_wait_or_error:
  if (m_clust_pcur)
    btr_pcur_store_position(m_clust_pcur, &mtr);
  btr_pcur_store_position(m_pcur, &mtr);
  mtr.commit();

  if (err == DB_SUCCESS)
  {
    /* In case, InnoDB got interruption from function handler or
    we already found the unique key value */
    if ((op != REMOVE && match_op == MATCH_UNIQUE) || interrupt)
      return err;
    move_next= true;
  }
  else
  {
    err= handle_wait(err);
    if (err != DB_SUCCESS) return err;
  }
  mtr.start();
  restore_cursor= true;
  goto reopen_index;

next_rec:
  if (btr_pcur_is_after_last_on_page(m_pcur))
  {
    if (btr_pcur_is_after_last_in_tree(m_pcur))
    {
      err= DB_END_OF_INDEX;
      goto func_exit;
    }
    err= btr_pcur_move_to_next_page(m_pcur, &mtr);
    if (err) goto lock_wait_or_error;
  }
  else if (!btr_pcur_move_to_next_on_page(m_pcur))
  {
    err= DB_CORRUPTION;
    goto func_exit;
  }
  goto rec_loop;
}

void FTSQueryRunner::build_update_config(dict_table_t *table, uint16_t field_no,
                                         const fts_string_t *new_value) noexcept
{
  m_update= upd_create(1, m_heap);
  upd_field_t *uf= upd_get_nth_field(m_update, 0);

  dict_index_t *index= dict_table_get_first_index(table);
  dict_field_t *ifield= dict_index_get_nth_field(index, field_no);
  dict_col_copy_type(dict_field_get_col(ifield),
                     dfield_get_type(&uf->new_val));
  dfield_set_data(&uf->new_val, new_value->f_str, new_value->f_len);
  uf->field_no= field_no;
  uf->orig_len= 0;
}

dberr_t FTSQueryRunner::update_record(dict_table_t *table, uint16_t old_len,
                                      uint16_t new_len) noexcept
{
  dberr_t err= DB_SUCCESS;
  dict_index_t *index= dict_table_get_first_index(table);
  ulint cmpl_info= (old_len == new_len)
                   ? UPD_NODE_NO_SIZE_CHANGE : 0;

  mtr_t mtr;
  mtr.start();
  mtr.set_named_space(table->space);
  if (m_pcur->restore_position(BTR_MODIFY_LEAF, &mtr)
      != btr_pcur_t::SAME_ALL)
  {
    err= DB_RECORD_NOT_FOUND;
    mtr.commit();
    return err;
  }

  rec_t *rec= btr_pcur_get_rec(m_pcur);
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs *offsets;
  rec_offs_init(offsets_);

  offsets= rec_get_offsets(rec, index, offsets_, index->n_core_fields,
                           ULINT_UNDEFINED, &m_heap);

  if (old_len == new_len)
    cmpl_info= UPD_NODE_NO_SIZE_CHANGE;

  err= row_upd_clust_rec_low(0, m_update, m_pcur, cmpl_info,
                             index, offsets, &m_heap, m_thr,
                             m_trx, &mtr);
  mtr.commit();
  return err;
}

FTSQueryRunner::~FTSQueryRunner()
{
  if (m_pcur) btr_pcur_close(m_pcur);
  if (m_clust_pcur) btr_pcur_close(m_clust_pcur);
  if (m_old_vers_heap) mem_heap_free(m_old_vers_heap);
  mem_heap_free(m_heap);
}
