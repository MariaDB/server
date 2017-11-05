/* Copyright (c) 2017, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */

#include <sql_class.h>
#include <tztime.h>

#include "btr0pcur.h"
#include "dict0load.h"
#include "ha_innodb.h"
#include "row0ins.h"
#include "row0row.h"
#include "trx0trx.h"
#include "trx0types.h"


/** Field or record selector.
@param[in]	thd	MySQL thread
@param[in]	q	VTQ record to get values from
@param[out]	out	field value or whole record returned
@param[in]	field	field to get in `out` or VTQ_ALL for whole record (vtq_record_t copied) */
static
inline
void
vtq_result(THD* thd, vtq_record_t& q, void *out, vtq_field_t field)
{
	ut_ad(field == VTQ_ALL || out);

	switch (field) {
	case VTQ_ALL:
		if (out) {
			*reinterpret_cast<vtq_record_t *>(out) = q;
		}
		break;
	case VTQ_TRX_ID:
		*reinterpret_cast<trx_id_t *>(out) = q.trx_id;
		break;
	case VTQ_COMMIT_ID:
		*reinterpret_cast<trx_id_t *>(out) = q.commit_id;
		break;
	case VTQ_BEGIN_TS: {
		MYSQL_TIME* out_ts = reinterpret_cast<MYSQL_TIME *>(out);
		thd_get_timezone(thd)->gmt_sec_to_TIME(out_ts, q.begin_ts.tv_sec);
		out_ts->second_part = q.begin_ts.tv_usec;
		break;
	}
	case VTQ_COMMIT_TS: {
		MYSQL_TIME* out_ts = reinterpret_cast<MYSQL_TIME *>(out);
		thd_get_timezone(thd)->gmt_sec_to_TIME(out_ts, q.commit_ts.tv_sec);
		out_ts->second_part = q.commit_ts.tv_usec;
		break;
	}
	case VTQ_ISO_LEVEL:
		*reinterpret_cast<uint *>(out) = q.iso_level;
		break;
	default:
		ut_error;
	}
}

inline
const char *
vtq_query_t::cache_result(mem_heap_t* heap, const rec_t* rec)
{
	prev_query.tv_sec = 0;
	return dict_process_sys_vtq(heap, rec, result);
}


/** Query VTQ by TRX_ID.
@param[in]	thd	MySQL thread
@param[out]	out	field value or whole record returned by query (selected by `field`)
@param[in]	in_trx_id query parameter TRX_ID
@param[in]	field	field to get in `out` or VTQ_ALL for whole record (vtq_record_t)
@return	TRUE if record is found, FALSE otherwise */
UNIV_INTERN
bool
vtq_query_trx_id(THD* thd, void *out, ulonglong _in_trx_id, vtq_field_t field)
{
	trx_t*		trx;
	dict_index_t*	index;
	btr_pcur_t	pcur;
	dtuple_t*	tuple;
	dfield_t*	dfield;
	trx_id_t	trx_id_net;
	mtr_t		mtr;
	mem_heap_t*	heap;
	rec_t*		rec;
	bool		found = false;

	DBUG_ENTER("vtq_query_trx_id");

	if (_in_trx_id == 0) {
		DBUG_RETURN(false);
	}

	ut_ad(sizeof(_in_trx_id) == sizeof(trx_id_t));
	trx_id_t	in_trx_id = static_cast<trx_id_t>(_in_trx_id);

	trx = thd_to_trx(thd);
	ut_a(trx);

	vtq_record_t	&cached = trx->vtq_query.result;

	if (cached.trx_id == in_trx_id) {
		vtq_result(thd, cached, out, field);
		DBUG_RETURN(true);
	}

	index = dict_table_get_first_index(dict_sys->sys_vtq);
	heap = mem_heap_create(0);

	ut_ad(index);
	ut_ad(dict_index_is_clust(index));

	mach_write_to_8(
		reinterpret_cast<byte*>(&trx_id_net),
		in_trx_id);

	tuple = dtuple_create(heap, 1);
	dfield = dtuple_get_nth_field(tuple, DICT_FLD__SYS_VTQ__TRX_ID);
	dfield_set_data(dfield, &trx_id_net, 8);
	dict_index_copy_types(tuple, index, 1);

	mtr_start_trx(&mtr, trx);
	btr_pcur_open_on_user_rec(index, tuple, PAGE_CUR_GE,
			BTR_SEARCH_LEAF, &pcur, &mtr);

	if (!btr_pcur_is_on_user_rec(&pcur))
		goto not_found;

	rec = btr_pcur_get_rec(&pcur);
	{
		const char *err = trx->vtq_query.cache_result(heap, rec);
		if (err) {
			ib::error()
				<< "vtq_query_trx_id: get VTQ field failed: "
				<< err;
			ut_ad(false && "get VTQ field failed");
			goto not_found;
		}
	}

	if (cached.trx_id != in_trx_id)
		goto not_found;

	vtq_result(thd, cached, out, field);
	found = true;

not_found:
	btr_pcur_close(&pcur);
	mtr_commit(&mtr);
	mem_heap_free(heap);

	DBUG_RETURN(found);
}

static
inline
void rec_get_timeval(const rec_t* rec, ulint nfield, timeval& out)
{
	ulint		len;
	const byte*	field;
	field = rec_get_nth_field_old(
		rec, nfield, &len);

	ut_ad(len == sizeof(uint64_t));

	out.tv_sec = mach_read_from_4(field);
	out.tv_usec = mach_read_from_4(field + 4);
}

inline
const char *
vtq_query_t::cache_result(
	mem_heap_t* heap,
	const rec_t* rec,
	const timeval& _ts_query,
	bool _backwards)
{
	prev_query = _ts_query;
	backwards = _backwards;
	return dict_process_sys_vtq(heap, rec, result);
}

static
inline
bool
operator== (const timeval &a, const timeval &b)
{
	return a.tv_sec == b.tv_sec && a.tv_usec == b.tv_usec;
}

static
inline
bool
operator!= (const timeval &a, const timeval &b)
{
	return !(a == b);
}

static
inline
bool
operator> (const timeval &a, const timeval &b)
{
	return a.tv_sec > b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_usec > b.tv_usec);
}

static
inline
bool
operator< (const timeval &a, const timeval &b)
{
	return b > a;
}

static
trx_id_t
read_trx_id(const rec_t *rec) {
	ulint len = 0;
	const rec_t *field = rec_get_nth_field_old(rec, 1, &len);
	DBUG_ASSERT(len == 8);
	return mach_read_from_8(field);
}

/** Find a row with given commit_ts but MAX()/MIN() trx_id
@param[in]	mtr		mini-transaction handler
@param[in, out]	pcur		btree cursor which may be changed by this function
@param[in]	backwards	search direction
@param[in]	commit_ts	target timestamp for records
@param[in]	rec		record buffer for pcur
*/
static
void
find_best_match(
	mtr_t &mtr,
	btr_pcur_t &pcur,
	bool backwards,
	timeval commit_ts,
	const rec_t *rec)
{
	btr_pcur_t best;
	btr_pcur_init(&best);
	btr_pcur_copy_stored_position(&best, &pcur);
	trx_id_t best_trx_id = read_trx_id(rec);

	while (true) {
		if (backwards ? !btr_pcur_move_to_prev_user_rec(&pcur, &mtr)
			      : !btr_pcur_move_to_next_user_rec(&pcur, &mtr))
			break;

		timeval tv;
		rec = btr_pcur_get_rec(&pcur);
		rec_get_timeval(rec, 0, tv);
		if (tv != commit_ts)
			break;

		trx_id_t trx_id = read_trx_id(rec);
		if (backwards ? trx_id < best_trx_id : trx_id > best_trx_id) {
			best_trx_id = trx_id;
			btr_pcur_copy_stored_position(&best, &pcur);
		}
	}

	btr_pcur_copy_stored_position(&pcur, &best);
	btr_pcur_free(&best);
}

/** Query VTQ by COMMIT_TS.
@param[in]	thd	MySQL thread
@param[out]	out	field value or whole record returned by query (selected by `field`)
@param[in]	commit_ts query parameter COMMIT_TS
@param[in]	field	field to get in `out` or VTQ_ALL for whole record (vtq_record_t)
@param[in]	backwards direction of VTQ search
@return	TRUE if record is found, FALSE otherwise */
UNIV_INTERN
bool
vtq_query_commit_ts(
	THD* thd,
	void *out,
	const MYSQL_TIME &_commit_ts,
	vtq_field_t field,
	bool backwards)
{
	trx_t*		trx;
	btr_pcur_t	pcur;
	dtuple_t*	tuple;
	page_cur_mode_t mode;
	mtr_t		mtr;
	mem_heap_t*	heap;
	uint		err;
	timeval		commit_ts;
	timeval		rec_ts = { 0, 0 };
	const rec_t	*rec, *clust_rec;
	dict_index_t*	index = dict_sys->vtq_commit_ts_ind;
	dict_index_t*	clust_index;
	bool		found = false;

	DBUG_ENTER("vtq_query_commit_ts");

	mode = backwards ? PAGE_CUR_LE : PAGE_CUR_GE;

	trx = thd_to_trx(thd);
	ut_a(trx);

	vtq_record_t &cached = trx->vtq_query.result;
	timeval &prev_query = trx->vtq_query.prev_query;
	bool prev_bwds = trx->vtq_query.backwards;

	commit_ts.tv_usec = _commit_ts.second_part;
	commit_ts.tv_sec = thd_get_timezone(thd)->TIME_to_gmt_sec(&_commit_ts, &err);
	if (err) {
		if (err == ER_WARN_DATA_OUT_OF_RANGE) {
			if (_commit_ts.year <= TIMESTAMP_MIN_YEAR) {
				commit_ts.tv_usec = 0;
				commit_ts.tv_sec = 1;
			} else {
				ut_ad(_commit_ts.year >= TIMESTAMP_MAX_YEAR);
				commit_ts.tv_usec = TIME_MAX_SECOND_PART;
				commit_ts.tv_sec = MY_TIME_T_MAX;
			}
		} else {
			DBUG_RETURN(false);
		}
	} else if (cached.commit_ts == commit_ts ||
		(prev_query.tv_sec && prev_bwds == backwards && (
			(!backwards && (commit_ts < prev_query) && commit_ts > cached.commit_ts) ||
			(backwards && (commit_ts > prev_query) && commit_ts < cached.commit_ts))))
	{
		vtq_result(thd, cached, out, field);
		DBUG_RETURN(true);
	}

	heap = mem_heap_create(0);

	tuple = dtuple_create(heap, 1);
	dict_index_copy_types(tuple, index, 1);
	dtuple_get_nth_field(tuple, 0)->len = UNIV_SQL_NULL;
	row_ins_set_tuple_col_8(tuple, 0, commit_ts, heap);

	mtr_start_trx(&mtr, trx);
	btr_pcur_open_on_user_rec(index, tuple, mode,
			BTR_SEARCH_LEAF, &pcur, &mtr);

	if (btr_pcur_is_on_user_rec(&pcur)) {
		rec = btr_pcur_get_rec(&pcur);
		rec_get_timeval(rec, 0, rec_ts);

		if (rec_ts == commit_ts) {
			find_best_match(mtr, pcur, backwards, commit_ts, rec);
			goto found;
                }
	} else {
		rec_ts = commit_ts;
	}

	if (mode == PAGE_CUR_GE) {
		btr_pcur_move_to_prev_user_rec(&pcur, &mtr);
	} else {
		btr_pcur_move_to_next_user_rec(&pcur, &mtr);
	}

	if (!btr_pcur_is_on_user_rec(&pcur))
		goto not_found;

	rec = btr_pcur_get_rec(&pcur);
found:
	clust_rec = row_get_clust_rec(BTR_SEARCH_LEAF, rec, index, &clust_index, &mtr);
	if (!clust_rec) {
		ib::error() << "vtq_query_commit_ts: secondary index is out of "
			       "sync";
		ut_ad(false && "secondary index is out of sync");
		goto not_found;
	}

	{
		const char *err =
			trx->vtq_query.cache_result(
				heap,
				clust_rec,
				rec_ts,
				backwards);
		if (err) {
			ib::error()
				<< "vtq_query_commit_ts: get VTQ field failed: "
				<< err;
			ut_ad(false && "get VTQ field failed");
			goto not_found;
		}
	}
	vtq_result(thd, cached, out, field);
	found = true;

not_found:
	btr_pcur_close(&pcur);
	mtr_commit(&mtr);
	mem_heap_free(heap);

	DBUG_RETURN(found);
}

/** Check if transaction TX1 sees transaction TX0.
@param[in]	thd	MySQL thread
@param[out]	result	true if TX1 sees TX0
@param[in]	trx_id1	TX1 TRX_ID
@param[in]	trx_id0	TX0 TRX_ID
@param[in]	commit_id1 TX1 COMMIT_ID
@param[in]	iso_level1 TX1 isolation level
@param[in]	commit_id0 TX0 COMMIT_ID
@return	FALSE if there is no trx_id1 in VTQ, otherwise TRUE */
bool
vtq_trx_sees(
	THD *thd,
	bool &result,
	ulonglong trx_id1,
	ulonglong trx_id0,
	ulonglong commit_id1,
	uchar iso_level1,
	ulonglong commit_id0)
{
	DBUG_ENTER("vtq_trx_sees");

	if (trx_id1 == trx_id0) {
		result = false;
		DBUG_RETURN(true);
	}

	if (trx_id1 == ULONGLONG_MAX || trx_id0 == 0) {
		result = true;
		DBUG_RETURN(true);
	}

	if (!commit_id1) {
		if (!vtq_query_trx_id(thd, NULL, trx_id1, VTQ_ALL)) {
			ib::info() << "vtq_trx_sees: can't find COMMIT_ID0 by "
				      "TRX_ID: "
				   << trx_id1;
			DBUG_RETURN(false);
		}
		trx_t* trx = thd_to_trx(thd);
		ut_ad(trx);
		commit_id1 = trx->vtq_query.result.commit_id;
		iso_level1 = trx->vtq_query.result.iso_level;
	}

	if (!commit_id0) {
		if (!vtq_query_trx_id(thd, &commit_id0, trx_id0, VTQ_COMMIT_ID)) {
			ib::info() << "vtq_trx_sees: can't find COMMIT_ID1 by "
				      "TRX_ID: "
				   << trx_id0;
			DBUG_RETURN(false);
		}
	}

	// Trivial case: TX1 started after TX0 committed
	if (trx_id1 > commit_id0
		// Concurrent transactions: TX1 committed after TX0 and TX1 is read (un)committed
		|| (commit_id1 > commit_id0 && iso_level1 < TRX_ISO_REPEATABLE_READ))
	{
		result = true;
	} else {
		// All other cases: TX1 does not see TX0
		result = false;
	}

	DBUG_RETURN(true);
}
