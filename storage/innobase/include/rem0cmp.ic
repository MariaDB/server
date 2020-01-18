/*****************************************************************************

Copyright (c) 1994, 2014, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2020, MariaDB Corporation.

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

/*******************************************************************//**
@file include/rem0cmp.ic
Comparison services for records

Created 7/1/1994 Heikki Tuuri
************************************************************************/

#include <mysql_com.h>
#include <my_sys.h>

/** Compare two data fields.
@param[in] dfield1 data field; must have type field set
@param[in] dfield2 data field
@return the comparison result of dfield1 and dfield2
@retval 0 if dfield1 is equal to dfield2
@retval negative if dfield1 is less than dfield2
@retval positive if dfield1 is greater than dfield2 */
UNIV_INLINE
int
cmp_dfield_dfield(
	const dfield_t*	dfield1,
	const dfield_t*	dfield2)
{
	const dtype_t*	type;

	ut_ad(dfield_check_typed(dfield1));

	type = dfield_get_type(dfield1);

	return(cmp_data_data(type->mtype, type->prtype,
			     (const byte*) dfield_get_data(dfield1),
			     dfield_get_len(dfield1),
			     (const byte*) dfield_get_data(dfield2),
			     dfield_get_len(dfield2)));
}

/** Compare two data fields.
@param[in] dfield1 data field
@param[in] dfield2 data field
@return the comparison result of dfield1 and dfield2
@retval 0 if dfield1 is equal to dfield2, or a prefix of dfield1
@retval negative if dfield1 is less than dfield2
@retval positive if dfield1 is greater than dfield2 */
UNIV_INLINE
int
cmp_dfield_dfield_like_prefix(
	const dfield_t*	dfield1,
	const dfield_t*	dfield2)
{
	const dtype_t*  type;

	ut_ad(dfield_check_typed(dfield1));
	ut_ad(dfield_check_typed(dfield2));

	type = dfield_get_type(dfield1);

#ifdef UNIV_DEBUG
	switch (type->prtype & DATA_MYSQL_TYPE_MASK) {
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

	uint cs_num = (uint) dtype_get_charset_coll(type->prtype);

	if (CHARSET_INFO* cs = get_charset(cs_num, MYF(MY_WME))) {
		return(cs->coll->strnncoll(
			       cs,
			       static_cast<const uchar*>(
				       dfield_get_data(dfield1)),
			       dfield_get_len(dfield1),
			       static_cast<const uchar*>(
				       dfield_get_data(dfield2)),
			       dfield_get_len(dfield2),
			       1));
	}

	ib::fatal() << "Unable to find charset-collation " << cs_num;
	return(0);
}
