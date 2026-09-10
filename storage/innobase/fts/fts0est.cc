/*****************************************************************************

Copyright (c) 2026, MariaDB PLC.

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
@file fts/fts0est.cc
Estimating the size of a fulltext search result.

fts_estimate_word_docs() is the fulltext analogue of records_in_range(): it
answers "how many documents contain this word?" cheaply enough to be called
while the optimizer is still choosing a plan.  It probes the auxiliary
INDEX_[1..6] B-tree that holds the word and the in-memory FTS cache, takes no
record locks, opens no read view, creates no transaction, and never reads an
ilist.

Created 2026/09/06
*******************************************************/

#include "fts0fts.h"
#include "fts0priv.h"
#include "fts0types.h"
#include "fts0types.inl"
#include "dict0dict.h"
#include "btr0pcur.h"
#include "mtr0mtr.h"
#include "rem0cmp.h"
#include "page0page.h"

/** Maximum number of auxiliary records fts_estimate_word_docs() samples
before it gives up on an exact answer and extrapolates instead. */
static constexpr uint32_t FTS_EST_MAX_RECS = 64;
/** Maximum number of auxiliary leaf pages fts_estimate_word_docs() reads. */
static constexpr uint32_t FTS_EST_MAX_PAGES = 4;

/* Physical field numbers of an FTS auxiliary INDEX_[1..6] record.  The
clustered index is UNIQUE(word, first_doc_id), so the record is
(word, first_doc_id, DB_TRX_ID, DB_ROLL_PTR, last_doc_id, doc_count, ilist);
see fts_create_one_index_table(). */
static constexpr ulint FTS_AUX_FLD_WORD = 0;
static constexpr ulint FTS_AUX_FLD_FIRST_DOC_ID = 1;
static constexpr ulint FTS_AUX_FLD_LAST_DOC_ID = 4;
static constexpr ulint FTS_AUX_FLD_DOC_COUNT = 5;
/** Number of offsets the estimator needs.  This deliberately stops short of
the ilist (field 6), which may be stored off-page: the estimator must never
read a BLOB. */
static constexpr ulint FTS_AUX_EST_N_FIELDS = FTS_AUX_FLD_DOC_COUNT + 1;

/** Build the search tuple (word) for an FTS auxiliary clustered index.
The index key is (word, first_doc_id), and because the tuple compares on its
first field only it compares equal to every record of the word.  So
PAGE_CUR_GE positions on the word's first record and PAGE_CUR_LE on its last.
@param[in]	heap		heap to allocate the tuple from
@param[in]	aux_index	auxiliary clustered index
@param[in]	word		word to search for
@return the search tuple */
static
dtuple_t*
fts_est_word_tuple(
	mem_heap_t*		heap,
	dict_index_t*		aux_index,
	const fts_string_t*	word) noexcept
{
	dtuple_t*	tuple = dtuple_create(heap, 1);

	dict_index_copy_types(tuple, aux_index, 1);
	dfield_set_data(dtuple_get_nth_field(tuple, 0),
			word->f_str, word->f_len);
	dtuple_set_n_fields_cmp(tuple, 1);

	return(tuple);
}

/** Read the fields the estimator needs out of an FTS auxiliary INDEX_[1..6]
leaf record.  Never touches the ilist.
@param[in]	tuple		search tuple built by fts_est_word_tuple()
@param[in]	rec		auxiliary table record
@param[in]	aux_index	auxiliary clustered index
@param[in]	offsets		rec_get_offsets(rec, aux_index, ...)
@param[out]	first_doc_id	first doc id in this record's ilist
@param[out]	last_doc_id	last doc id in this record's ilist
@param[out]	doc_count	number of doc ids in this record's ilist
@return whether the record belongs to the word tuple was built for */
static
bool
fts_est_read_rec(
	const dtuple_t*		tuple,
	const rec_t*		rec,
	const dict_index_t*	aux_index,
	const rec_offs*		offsets,
	doc_id_t*		first_doc_id,
	doc_id_t*		last_doc_id,
	uint32_t*		doc_count) noexcept
{
	/* tuple has n_fields_cmp == 1, so this compares the word only, in the
	collation of the auxiliary table's word column. */
	if (cmp_dtuple_rec(tuple, rec, aux_index, offsets)) {
		return(false);
	}

	ulint		len;
	const byte*	data;

	data = rec_get_nth_field(rec, offsets, FTS_AUX_FLD_FIRST_DOC_ID, &len);
	*first_doc_id = (data && len == sizeof *first_doc_id)
		? fts_read_doc_id(data) : 0;

	data = rec_get_nth_field(rec, offsets, FTS_AUX_FLD_LAST_DOC_ID, &len);
	*last_doc_id = (data && len == sizeof *last_doc_id)
		? fts_read_doc_id(data) : *first_doc_id;

	data = rec_get_nth_field(rec, offsets, FTS_AUX_FLD_DOC_COUNT, &len);
	*doc_count = (data && len == 4) ? mach_read_from_4(data) : 0;

	return(true);
}

/** What fts_est_sample_word() found. */
struct fts_est_sample_t
{
	/** Sum of doc_count over the records sampled. */
	uint64_t	docs;
	/** Number of records sampled. */
	uint32_t	n_recs;
	/** First doc id of the word's first record. */
	doc_id_t	first_doc_id;
	/** Last doc id of the last record sampled. */
	doc_id_t	last_doc_id;
	/** Whether sampling stopped because a budget ran out rather than
	because the word's key range ended.  When this is false, docs is the
	exact number of documents that contain the word. */
	bool		truncated;
};

/** Dive to a word's first auxiliary record and walk forward from it, over at
most FTS_EST_MAX_RECS records and FTS_EST_MAX_PAGES leaf pages.  The walk is
nearly free because the dive has already latched the leaf page, and if it
reaches a different word before a budget runs out the result is exact.
@param[in]	trx		transaction, used only for the mtr
@param[in]	aux_index	auxiliary clustered index
@param[in]	tuple		search tuple built by fts_est_word_tuple()
@param[out]	out		what the walk found
@return DB_SUCCESS, or DB_RECORD_NOT_FOUND if the auxiliary table is empty
(nothing has been SYNCed yet, so there is no information at all) */
static
dberr_t
fts_est_sample_word(
	trx_t*			trx,
	dict_index_t*		aux_index,
	const dtuple_t*		tuple,
	fts_est_sample_t*	out) noexcept
{
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets = offsets_;
	mem_heap_t*	offs_heap = NULL;
	btr_pcur_t	pcur;
	mtr_t		mtr{trx};

	rec_offs_init(offsets_);
	memset(out, 0, sizeof *out);

	mtr.start();
	pcur.btr_cur.page_cur.index = aux_index;

	dberr_t	err = btr_pcur_open_on_user_rec(tuple, BTR_SEARCH_LEAF,
						&pcur, &mtr);

	if (err != DB_SUCCESS) {
		goto func_exit;
	}

	if (!btr_pcur_is_on_user_rec(&pcur)) {
		/* Nothing at or after the word.  Tell an empty auxiliary
		table, where we have no information at all, apart from a word
		that simply sorts after everything that is indexed. */
		const page_t*	page = btr_pcur_get_page(&pcur);

		if (!page_has_prev(page) && !page_get_n_recs(page)) {
			err = DB_RECORD_NOT_FOUND;
		}

		goto func_exit;
	}

	{
		uint32_t	n_pages = 1;
		page_id_t	last_page
			= btr_pcur_get_block(&pcur)->page.id();

		do {
			const rec_t*		rec = btr_pcur_get_rec(&pcur);
			const buf_block_t*	block
				= btr_pcur_get_block(&pcur);
			const ulint		offs
				= ulint(rec - block->page.frame);

			if (page_rec_is_infimum_low(offs)
			    || page_rec_is_supremum_low(offs)) {
				continue;
			}

			if (block->page.id() != last_page) {
				last_page = block->page.id();
				if (++n_pages > FTS_EST_MAX_PAGES) {
					out->truncated = true;
					break;
				}
			}

			offsets = rec_get_offsets(rec, aux_index, offsets,
						  aux_index->n_core_fields,
						  FTS_AUX_EST_N_FIELDS,
						  &offs_heap);

			doc_id_t	first;
			doc_id_t	last;
			uint32_t	doc_count;

			if (!fts_est_read_rec(tuple, rec, aux_index, offsets,
					      &first, &last, &doc_count)) {
				/* Walked past the word's key range, so we
				have seen all of it. */
				break;
			}

			if (!out->n_recs) {
				out->first_doc_id = first;
			}

			out->last_doc_id = last;
			out->docs += doc_count;

			if (++out->n_recs >= FTS_EST_MAX_RECS) {
				out->truncated = true;
				break;
			}
		} while (btr_pcur_move_to_next(&pcur, &mtr));
	}

func_exit:
	mtr.commit();
	ut_free(pcur.old_rec_buf);

	if (UNIV_LIKELY_NULL(offs_heap)) {
		mem_heap_free(offs_heap);
	}

	return(err);
}

/** Dive to a word's last auxiliary record, to learn how far its doc ids
reach.
@param[in]	trx		transaction, used only for the mtr
@param[in]	aux_index	auxiliary clustered index
@param[in]	tuple		search tuple built by fts_est_word_tuple()
@param[in,out]	last_doc_id	last doc id of the word; left alone if the
				record cannot be read
@param[in,out]	doc_count	doc_count of that record; left alone if the
				record cannot be read
@return DB_SUCCESS or error code */
static
dberr_t
fts_est_last_rec(
	trx_t*			trx,
	dict_index_t*		aux_index,
	const dtuple_t*		tuple,
	doc_id_t*		last_doc_id,
	uint32_t*		doc_count) noexcept
{
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets = offsets_;
	mem_heap_t*	offs_heap = NULL;
	btr_pcur_t	pcur;
	mtr_t		mtr{trx};

	rec_offs_init(offsets_);

	mtr.start();
	pcur.btr_cur.page_cur.index = aux_index;

	dberr_t	err = btr_pcur_open(tuple, PAGE_CUR_LE, BTR_SEARCH_LEAF,
				    &pcur, &mtr);

	if (err == DB_SUCCESS) {
		/* PAGE_CUR_LE may leave the cursor on the page infimum, in
		which case the record we want is the last one of the previous
		page. */
		const bool	positioned
			= !btr_pcur_is_before_first_on_page(&pcur)
			|| btr_pcur_move_to_prev(&pcur, &mtr);

		if (positioned && btr_pcur_is_on_user_rec(&pcur)) {
			const rec_t*	rec = btr_pcur_get_rec(&pcur);
			doc_id_t	first;
			doc_id_t	last;
			uint32_t	count;

			offsets = rec_get_offsets(rec, aux_index, offsets,
						  aux_index->n_core_fields,
						  FTS_AUX_EST_N_FIELDS,
						  &offs_heap);

			if (fts_est_read_rec(tuple, rec, aux_index, offsets,
					     &first, &last, &count)) {
				*last_doc_id = last;
				*doc_count = count;
			}
		}
	}

	mtr.commit();
	ut_free(pcur.old_rec_buf);

	if (UNIV_LIKELY_NULL(offs_heap)) {
		mem_heap_free(offs_heap);
	}

	return(err);
}

/** Estimate how many documents contain a word, by probing an already opened
FTS auxiliary INDEX_[1..6] table.  See fts_estimate_word_docs().
@param[in]	trx	transaction, used only for the mtr
@param[in]	aux	auxiliary table that holds the word
@param[in]	word	word to look up
@param[out]	n_docs	estimated number of matching documents
@return DB_SUCCESS, DB_RECORD_NOT_FOUND or DB_CORRUPTION */
static
dberr_t
fts_est_probe_aux(
	trx_t*			trx,
	dict_table_t*		aux,
	const fts_string_t*	word,
	uint64_t*		n_docs) noexcept
{
	dict_index_t*	aux_index = dict_table_get_first_index(aux);

	if (!aux->space || !aux->is_readable() || !aux_index
	    || aux_index->page == FIL_NULL || aux_index->is_corrupted()) {
		return(DB_CORRUPTION);
	}

	mem_heap_t*		heap = mem_heap_create(256);
	const dtuple_t*		tuple = fts_est_word_tuple(heap, aux_index,
							   word);
	fts_est_sample_t	s;
	dberr_t			err = fts_est_sample_word(trx, aux_index,
							  tuple, &s);

	if (err != DB_SUCCESS) {
		mem_heap_free(heap);
		return(err);
	}

	if (!s.n_recs || !s.truncated) {
		/* Either the word is absent, or the walk covered its whole
		key range, in which case the count is exact. */
		*n_docs = s.docs;
		mem_heap_free(heap);
		return(DB_SUCCESS);
	}

	/* The word has more records than we are willing to read.  Dive once
	more, to its last record, and extrapolate the density we measured
	across the word's whole doc id span.  Doc ids only ever increase, so
	the records we sampled are a prefix of that span. */
	doc_id_t	last_doc_id = s.last_doc_id;
	uint32_t	last_count = 0;

	err = fts_est_last_rec(trx, aux_index, tuple, &last_doc_id,
			       &last_count);

	if (err == DB_SUCCESS) {
		if (last_doc_id < s.last_doc_id) {
			last_doc_id = s.last_doc_id;
		}

		const uint64_t	sampled_span
			= s.last_doc_id > s.first_doc_id
			? s.last_doc_id - s.first_doc_id + 1 : 1;
		const uint64_t	total_span
			= last_doc_id - s.first_doc_id + 1;

		/* In double, because docs * total_span overflows 64 bits for
		large inputs.  The caller clamps the result to the number of
		rows in the table. */
		uint64_t	est = uint64_t(double(s.docs)
					       * double(total_span)
					       / double(sampled_span));

		/* Never below what we actually counted. */
		if (est < s.docs + last_count) {
			est = s.docs + last_count;
		}

		*n_docs = est;
	}

	mem_heap_free(heap);

	return(err);
}

/** Count the documents that contain a word and are still only in the
in-memory FTS cache, that is, have not been SYNCed to the auxiliary table yet.

fts_node_t::doc_count already holds the number we want, so this decodes no
ilist and reads nothing from disk; it is one rb tree lookup.

Nodes flagged as synced are skipped.  An in-flight SYNC has already written
them to the auxiliary table, and fts_est_probe_aux() reads the B-tree without
a read view, so it sees those records; counting the node as well would count
its documents twice.

@param[in]	index	fulltext index
@param[in]	word	word to look up, folded the same way the caller folds it
			for the auxiliary table
@return number of matching documents found in the cache, or 0 if there are
none, if the cache is not initialized, or if another thread holds the cache
mutex (an estimate is not worth waiting for a SYNC to finish) */
static
uint64_t
fts_est_cache_docs(
	const dict_index_t*	index,
	const fts_string_t*	word) noexcept
{
	const fts_t*	fts = index->table->fts;

	if (!fts || !fts->cache) {
		return(0);
	}

	fts_cache_t*	cache = fts->cache;

	if (mysql_mutex_trylock(&cache->lock)) {
		return(0);
	}

	uint64_t	n_docs = 0;

	if (const fts_index_cache_t* index_cache
	    = fts_find_index_cache(cache, index)) {
		/* fts_cache_clear() leaves words NULL until the following
		fts_cache_init().  The query path never observes that, because
		it runs after fts_init_index(); we may. */
		if (index_cache->words) {
			const ib_vector_t*	nodes
				= fts_cache_find_word(index_cache, word);

			for (ulint i = 0; nodes && i < ib_vector_size(nodes);
			     ++i) {
				const fts_node_t*	node
					= static_cast<const fts_node_t*>(
						ib_vector_get_const(nodes, i));

				if (!node->synced) {
					n_docs += node->doc_count;
				}
			}
		}
	}

	mysql_mutex_unlock(&cache->lock);

	return(n_docs);
}

dberr_t
fts_estimate_word_docs(
	trx_t*			trx,
	dict_index_t*		index,
	const fts_string_t*	word,
	uint64_t*		n_docs) noexcept
{
	ut_ad(index->type & DICT_FTS);
	ut_ad(!dict_sys.locked());
	ut_ad(word->f_len);

	*n_docs = 0;

	/* Documents that have been inserted but not SYNCed yet are only in the
	memory cache.  Look there first: it costs no I/O, and on a table that
	has never been SYNCed it is the only information there is. */
	const uint64_t	cached = fts_est_cache_docs(index, word);

	/* A word lives in exactly one of INDEX_1..INDEX_6, so only that one
	auxiliary table is ever opened -- unlike the query path, which opens
	all six. */
	CHARSET_INFO*	cs = fts_index_get_charset(index);
	const uint8_t	selected = fts_select_index(cs, word->f_str,
						    word->f_len);
	fts_table_t	fts_table;

	FTS_INIT_INDEX_TABLE(&fts_table, fts_get_suffix(selected),
			     FTS_INDEX_TABLE, index);

	char	aux_name[MAX_FULL_NAME_LEN];

	fts_get_table_name(&fts_table, aux_name, false);

	dict_table_t*	aux = dict_table_open_on_name(
		aux_name, false, DICT_ERR_IGNORE_TABLESPACE);

	if (!aux) {
		if (cached) {
			*n_docs = cached;
			return(DB_SUCCESS);
		}

		return(DB_TABLE_NOT_FOUND);
	}

	dberr_t	err = fts_est_probe_aux(trx, aux, word, n_docs);

	aux->release();

	if (err == DB_SUCCESS) {
		/* The two populations are disjoint: fts_est_cache_docs()
		skipped every node that the auxiliary table already holds. */
		*n_docs += cached;
	} else if (cached && err == DB_RECORD_NOT_FOUND) {
		/* The auxiliary table is empty, but the cache is not, so we
		are no longer without information.  The other way round --
		nothing in either -- stays DB_RECORD_NOT_FOUND: the cache is
		only complete once fts_init_index() has run, and an estimate
		must not run it. */
		*n_docs = cached;
		err = DB_SUCCESS;
	}

	return(err);
}
