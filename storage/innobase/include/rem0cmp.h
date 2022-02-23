/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
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

/*******************************************************************//**
@file include/rem0cmp.h
Comparison services for records

Created 7/1/1994 Heikki Tuuri
************************************************************************/

#pragma once

#include "data0data.h"
#include "data0type.h"
#include "rem0types.h"
#include "page0types.h"

/*************************************************************//**
Returns TRUE if two columns are equal for comparison purposes.
@return TRUE if the columns are considered equal in comparisons */
ibool
cmp_cols_are_equal(
/*===============*/
	const dict_col_t*	col1,	/*!< in: column 1 */
	const dict_col_t*	col2,	/*!< in: column 2 */
	ibool			check_charsets);
					/*!< in: whether to check charsets */
/** Compare two data fields.
@param mtype          main type
@param prtype         precise type
@param descending     whether to use descending order
@param data1          data field
@param len1           length of data1 in bytes, or UNIV_SQL_NULL
@param data2          data field
@param len2           length of data2 in bytes, or UNIV_SQL_NULL
@return the comparison result of data1 and data2
@retval 0 if data1 is equal to data2
@retval negative if data1 is less than data2
@retval positive if data1 is greater than data2 */
int cmp_data(ulint mtype, ulint prtype, bool descending,
             const byte *data1, size_t len1, const byte *data2, size_t len2)
  MY_ATTRIBUTE((warn_unused_result));

/** Compare two data fields.
@param dfield1       data field; must have type field set
@param dfield2       data field
@param descending    whether to use descending order
@return the comparison result of dfield1 and dfield2
@retval 0 if dfield1 is equal to dfield2
@retval negative if dfield1 is less than dfield2
@retval positive if dfield1 is greater than dfield2 */
inline int cmp_dfield_dfield(const dfield_t *dfield1, const dfield_t *dfield2,
                             bool descending= false)
{
  ut_ad(dfield_check_typed(dfield1));
  const dtype_t *type= dfield_get_type(dfield1);
  return cmp_data(type->mtype, type->prtype, descending,
                  static_cast<const byte*>(dfield_get_data(dfield1)),
                  dfield_get_len(dfield1),
                  static_cast<const byte*>(dfield_get_data(dfield2)),
                  dfield_get_len(dfield2));
}

#ifdef UNIV_DEBUG
/** Compare a GIS data tuple to a physical record.
@param[in] dtuple data tuple
@param[in] rec R-tree record
@param[in] mode compare mode
@retval negative if dtuple is less than rec */
int cmp_dtuple_rec_with_gis(const dtuple_t *dtuple, const rec_t *rec,
                            page_cur_mode_t mode)
  MY_ATTRIBUTE((nonnull));
#endif

/** Compare two minimum bounding rectangles.
@return	1, 0, -1, if a is greater, equal, less than b, respectively */
inline int cmp_geometry_field(const void *a, const void *b)
{
  const byte *mbr1= static_cast<const byte*>(a);
  const byte *mbr2= static_cast<const byte*>(b);

  static_assert(SPDIMS == 2, "compatibility");
  static_assert(DATA_MBR_LEN == SPDIMS * 2 * sizeof(double), "compatibility");

  /* Try to compare mbr left lower corner (xmin, ymin) */
  double x1= mach_double_read(mbr1);
  double x2= mach_double_read(mbr2);
  if (x1 > x2)
    return 1;
  if (x1 < x2)
    return -1;

  x1= mach_double_read(mbr1 + sizeof(double) * SPDIMS);
  x2= mach_double_read(mbr2 + sizeof(double) * SPDIMS);

  if (x1 > x2)
    return 1;
  if (x1 < x2)
    return -1;

  /* left lower corner (xmin, ymin) overlaps, now right upper corner */
  x1= mach_double_read(mbr1 + sizeof(double));
  x2= mach_double_read(mbr2 + sizeof(double));

  if (x1 > x2)
    return 1;
  if (x1 < x2)
    return -1;

  x1= mach_double_read(mbr1 + sizeof(double) * 2 + sizeof(double));
  x2= mach_double_read(mbr2 + sizeof(double) * 2 + sizeof(double));

  if (x1 > x2)
    return 1;
  if (x1 < x2)
    return -1;

  return 0;
}

/** Compare a data tuple to a physical record.
@param dtuple          data tuple
@param rec             B-tree index record
@param index           B-tree index
@param offsets         rec_get_offsets(rec,index)
@param n_cmp           number of fields to compare
@param matched_fields  number of completely matched fields
@return the comparison result of dtuple and rec
@retval 0 if dtuple is equal to rec
@retval negative if dtuple is less than rec
@retval positive if dtuple is greater than rec */
int cmp_dtuple_rec_with_match_low(const dtuple_t *dtuple, const rec_t *rec,
                                  const dict_index_t *index,
                                  const rec_offs *offsets,
                                  ulint n_cmp, ulint *matched_fields)
  MY_ATTRIBUTE((nonnull));
#define cmp_dtuple_rec_with_match(tuple,rec,index,offsets,fields)	\
	cmp_dtuple_rec_with_match_low(					\
		tuple,rec,index,offsets,dtuple_get_n_fields_cmp(tuple),fields)
/** Compare a data tuple to a physical record.
@param[in]	dtuple		data tuple
@param[in]	rec		B-tree or R-tree index record
@param[in]	index		index tree
@param[in]	offsets		rec_get_offsets(rec)
@param[in,out]	matched_fields	number of completely matched fields
@param[in,out]	matched_bytes	number of matched bytes in the first
field that is not matched
@return the comparison result of dtuple and rec
@retval 0 if dtuple is equal to rec
@retval negative if dtuple is less than rec
@retval positive if dtuple is greater than rec */
int
cmp_dtuple_rec_with_match_bytes(
	const dtuple_t*		dtuple,
	const rec_t*		rec,
	const dict_index_t*	index,
	const rec_offs*		offsets,
	ulint*			matched_fields,
	ulint*			matched_bytes)
	MY_ATTRIBUTE((warn_unused_result));
/** Compare a data tuple to a physical record.
@see cmp_dtuple_rec_with_match
@param dtuple  data tuple
@param rec     index record
@param index   index
@param offsets rec_get_offsets(rec, index)
@return the comparison result of dtuple and rec
@retval 0 if dtuple is equal to rec
@retval negative if dtuple is less than rec
@retval positive if dtuple is greater than rec */
inline int cmp_dtuple_rec(const dtuple_t *dtuple, const rec_t *rec,
                          const dict_index_t *index, const rec_offs *offsets)
{
  ulint matched= 0;
  return cmp_dtuple_rec_with_match(dtuple, rec, index, offsets, &matched);
}

/** Check if a dtuple is a prefix of a record.
@param dtuple  data tuple
@param rec     index record
@param index   index
@param offsets rec_get_offsets(rec)
@return whether dtuple is a prefix of rec */
bool cmp_dtuple_is_prefix_of_rec(const dtuple_t *dtuple, const rec_t *rec,
                                 const dict_index_t *index,
                                 const rec_offs *offsets)
  MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Compare two physical records that contain the same number of columns,
none of which are stored externally.
@retval positive if rec1 (including non-ordering columns) is greater than rec2
@retval negative if rec1 (including non-ordering columns) is less than rec2
@retval 0 if rec1 is a duplicate of rec2 */
int
cmp_rec_rec_simple(
/*===============*/
	const rec_t*		rec1,	/*!< in: physical record */
	const rec_t*		rec2,	/*!< in: physical record */
	const rec_offs*		offsets1,/*!< in: rec_get_offsets(rec1, ...) */
	const rec_offs*		offsets2,/*!< in: rec_get_offsets(rec2, ...) */
	const dict_index_t*	index,	/*!< in: data dictionary index */
	struct TABLE*		table)	/*!< in: MySQL table, for reporting
					duplicate key value if applicable,
					or NULL */
	MY_ATTRIBUTE((nonnull(1,2,3,4), warn_unused_result));

/** Compare two B-tree or R-tree records.
Only the common first fields are compared, and externally stored field
are treated as equal.
@param[in]	rec1		record (possibly not on an index page)
@param[in]	rec2		B-tree or R-tree record in an index page
@param[in]	offsets1	rec_get_offsets(rec1, index)
@param[in]	offsets2	rec_get_offsets(rec2, index)
@param[in]	nulls_unequal	true if this is for index cardinality
				statistics estimation with
				innodb_stats_method=nulls_unequal
				or innodb_stats_method=nulls_ignored
@param[out]	matched_fields	number of completely matched fields
				within the first field not completely matched
@retval 0 if rec1 is equal to rec2
@retval negative if rec1 is less than rec2
@retval positive if rec1 is greater than rec2 */
int
cmp_rec_rec(
	const rec_t*		rec1,
	const rec_t*		rec2,
	const rec_offs*		offsets1,
	const rec_offs*		offsets2,
	const dict_index_t*	index,
	bool			nulls_unequal = false,
	ulint*			matched_fields = NULL)
	MY_ATTRIBUTE((nonnull(1,2,3,4,5)));

/** Compare two data fields.
@param dfield1        data field
@param dfield2        data field
@return the comparison result of dfield1 and dfield2
@retval true if dfield1 is equal to dfield2, or a prefix of dfield1
@retval false otherwise */
inline bool cmp_dfield_dfield_eq_prefix(const dfield_t *dfield1,
                                        const dfield_t *dfield2)
{
  ut_ad(dfield_check_typed(dfield1));
  ut_ad(dfield_check_typed(dfield2));
  const dtype_t *type= dfield_get_type(dfield1);

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

  uint cs_num= dtype_get_charset_coll(type->prtype);
  CHARSET_INFO *cs= get_charset(cs_num, MYF(MY_WME));
  ut_a(cs);
  return !cs->strnncoll(static_cast<const uchar*>(dfield_get_data(dfield1)),
                        dfield_get_len(dfield1),
                        static_cast<const uchar*>(dfield_get_data(dfield2)),
                        dfield_get_len(dfield2), 1);
}
