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

/** Query VTQ by TRX_ID.
@param[in]	thd	MySQL thread
@param[out]	out	field value or whole record returned by query (selected by `field`)
@param[in]	in_trx_id query parameter TRX_ID
@param[in]	field	field to get in `out` or VTQ_ALL for whole record (vtq_record_t)
@return	TRUE if record is found, FALSE otherwise */
bool
vtq_query_trx_id(THD* thd, void *out, ulonglong in_trx_id, vtq_field_t field);

/** Query VTQ by COMMIT_TS.
@param[in]	thd	MySQL thread
@param[out]	out	field value or whole record returned by query (selected by `field`)
@param[in]	commit_ts query parameter COMMIT_TS
@param[in]	field	field to get in `out` or VTQ_ALL for whole record (vtq_record_t)
@param[in]	backwards direction of VTQ search
@return	TRUE if record is found, FALSE otherwise */
bool
vtq_query_commit_ts(THD* thd, void *out, const MYSQL_TIME &commit_ts, vtq_field_t field, bool backwards);

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
	ulonglong commit_id0);
