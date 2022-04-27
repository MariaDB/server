/*****************************************************************************

Copyright (c) 1994, 2019, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2020, 2022, MariaDB Corporation.

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
@file rem/rem0cmp.cc
Comparison services for records

Created 7/1/1994 Heikki Tuuri
************************************************************************/

#include "rem0cmp.h"
#include "rem0rec.h"
#include "page0page.h"
#include "dict0mem.h"
#include "handler0alter.h"

/*		ALPHABETICAL ORDER
		==================

The records are put into alphabetical order in the following
way: let F be the first field where two records disagree.
If there is a character in some position n where the
records disagree, the order is determined by comparison of
the characters at position n, possibly after
collating transformation. If there is no such character,
but the corresponding fields have different lengths, then
if the data type of the fields is paddable,
shorter field is padded with a padding character. If the
data type is not paddable, longer field is considered greater.
Finally, the SQL null is bigger than any other value.

At the present, the comparison functions return 0 in the case,
where two records disagree only in the way that one
has more fields than the other. */

#ifndef DBUG_OFF
/** @return whether a data type is compatible with strnncoll() functions */
static bool is_strnncoll_compatible(ulint type)
{
  switch (type) {
  case MYSQL_TYPE_BIT:
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_VARCHAR:
    return true;
  default:
    return false;
  }
}
#endif /* DBUG_OFF */

/*************************************************************//**
Returns TRUE if two columns are equal for comparison purposes.
@return TRUE if the columns are considered equal in comparisons */
ibool
cmp_cols_are_equal(
/*===============*/
	const dict_col_t*	col1,	/*!< in: column 1 */
	const dict_col_t*	col2,	/*!< in: column 2 */
	ibool			check_charsets)
					/*!< in: whether to check charsets */
{
	if (dtype_is_non_binary_string_type(col1->mtype, col1->prtype)
	    && dtype_is_non_binary_string_type(col2->mtype, col2->prtype)) {

		/* Both are non-binary string types: they can be compared if
		and only if the charset-collation is the same */

		if (check_charsets) {
			return(dtype_get_charset_coll(col1->prtype)
			       == dtype_get_charset_coll(col2->prtype));
		} else {
			return(TRUE);
		}
	}

	if (dtype_is_binary_string_type(col1->mtype, col1->prtype)
	    && dtype_is_binary_string_type(col2->mtype, col2->prtype)) {

		/* Both are binary string types: they can be compared */

		return(TRUE);
	}

	if (col1->mtype != col2->mtype) {

		return(FALSE);
	}

	if (col1->mtype == DATA_INT
	    && (col1->prtype & DATA_UNSIGNED)
	    != (col2->prtype & DATA_UNSIGNED)) {

		/* The storage format of an unsigned integer is different
		from a signed integer: in a signed integer we OR
		0x8000... to the value of positive integers. */

		return(FALSE);
	}

	return(col1->mtype != DATA_INT || col1->len == col2->len);
}

/** Compare two DATA_DECIMAL (MYSQL_TYPE_DECIMAL) fields.
TODO: Remove this function. Everything should use MYSQL_TYPE_NEWDECIMAL.
@param[in] a data field
@param[in] a_length length of a, in bytes (not UNIV_SQL_NULL)
@param[in] b data field
@param[in] b_length length of b, in bytes (not UNIV_SQL_NULL)
@return positive, 0, negative, if a is greater, equal, less than b,
respectively */
static ATTRIBUTE_COLD
int
cmp_decimal(const byte*	a, ulint a_length, const byte* b, ulint b_length)
{
	int	swap_flag;

	/* Remove preceding spaces */
	for (; a_length && *a == ' '; a++, a_length--) { }
	for (; b_length && *b == ' '; b++, b_length--) { }

	if (*a == '-') {
		swap_flag = -1;

		if (*b != '-') {
			return(swap_flag);
		}

		a++; b++;
		a_length--;
		b_length--;
	} else {
		swap_flag = 1;

		if (*b == '-') {
			return(swap_flag);
		}
	}

	while (a_length > 0 && (*a == '+' || *a == '0')) {
		a++; a_length--;
	}

	while (b_length > 0 && (*b == '+' || *b == '0')) {
		b++; b_length--;
	}

	if (a_length != b_length) {
		if (a_length < b_length) {
			return(-swap_flag);
		}

		return(swap_flag);
	}

	while (a_length > 0 && *a == *b) {

		a++; b++; a_length--;
	}

	if (a_length == 0) {
		return(0);
	}

	if (*a <= *b) {
		swap_flag = -swap_flag;
	}

	return(swap_flag);
}

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
{
  ut_ad(len1 != UNIV_SQL_DEFAULT);
  ut_ad(len2 != UNIV_SQL_DEFAULT);

  int cmp= 0;

  if (len1 == UNIV_SQL_NULL || len2 == UNIV_SQL_NULL)
  {
    if (len1 == len2)
      return 0;

    /* We define the SQL null to be the smallest possible value of a field. */
    cmp= len1 == UNIV_SQL_NULL ? -1 : 1;
  func_exit:
    return UNIV_UNLIKELY(descending) ? -cmp : cmp;
  }

  switch (mtype) {
  default:
    ib::fatal() << "Unknown data type number " << mtype;
  case DATA_DECIMAL:
    cmp= cmp_decimal(data1, len1, data2, len2);
    goto func_exit;
  case DATA_DOUBLE:
    {
      const double af= mach_double_read(data1), bf= mach_double_read(data2);
      cmp= af > bf ? 1 : bf > af ? -1 : 0;
    }
    goto func_exit;
  case DATA_FLOAT:
    {
      const float af= mach_float_read(data1), bf= mach_float_read(data2);
      cmp= af > bf ? 1 : bf > af ? -1 : 0;
    }
    goto func_exit;
  case DATA_FIXBINARY:
  case DATA_BINARY:
    if (dtype_get_charset_coll(prtype) != DATA_MYSQL_BINARY_CHARSET_COLL)
    {
      if (ulint len= std::min(len1, len2))
      {
        cmp= memcmp(data1, data2, len);
        if (cmp)
          goto func_exit;
        data1+= len;
        data2+= len;
        len1-= len;
        len2-= len;
      }
      if (len1)
      {
        const byte *end= &data1[len1];
        do
          cmp= static_cast<int>(*data1++ - byte{0x20});
        while (cmp == 0 && data1 < end);
      }
      else if (len2)
      {
        const byte *end= &data2[len2];
        do
          cmp= static_cast<int>(byte{0x20} - *data2++);
        while (cmp == 0 && data2 < end);
      }
      goto func_exit;
    }
    /* fall through */
  case DATA_INT:
  case DATA_SYS_CHILD:
  case DATA_SYS:
    break;
  case DATA_GEOMETRY:
    ut_ad(prtype & DATA_BINARY_TYPE);
    if (prtype & DATA_GIS_MBR)
    {
      ut_ad(len1 == DATA_MBR_LEN);
      ut_ad(len2 == DATA_MBR_LEN);
      cmp= cmp_geometry_field(data1, data2);
      goto func_exit;
    }
    break;
  case DATA_BLOB:
    if (prtype & DATA_BINARY_TYPE)
      break;
    /* fall through */
  case DATA_VARMYSQL:
    DBUG_ASSERT(is_strnncoll_compatible(prtype & DATA_MYSQL_TYPE_MASK));
    if (CHARSET_INFO *cs= all_charsets[dtype_get_charset_coll(prtype)])
    {
      cmp= cs->coll->strnncollsp(cs, data1, len1, data2, len2);
      goto func_exit;
    }
  no_collation:
    ib::fatal() << "Unable to find charset-collation for " << prtype;
  case DATA_MYSQL:
    DBUG_ASSERT(is_strnncoll_compatible(prtype & DATA_MYSQL_TYPE_MASK));
    if (CHARSET_INFO *cs= all_charsets[dtype_get_charset_coll(prtype)])
    {
      cmp= cs->coll->strnncollsp_nchars(cs, data1, len1, data2, len2,
                                        std::max(len1, len2));
      goto func_exit;
    }
    goto no_collation;
  case DATA_VARCHAR:
  case DATA_CHAR:
    /* latin1_swedish_ci is treated as a special case in InnoDB.
    Because it is a fixed-length encoding (mbminlen=mbmaxlen=1),
    non-NULL CHAR(n) values will always occupy n bytes and we
    can invoke strnncollsp() instead of strnncollsp_nchars(). */
    cmp= my_charset_latin1.strnncollsp(data1, len1, data2, len2);
    goto func_exit;
  }

  if (ulint len= std::min(len1, len2))
  {
    cmp= memcmp(data1, data2, len);
    if (cmp)
      goto func_exit;
  }

  cmp= int(len1 - len2);
  goto func_exit;
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
{
	ulint		cur_field;	/* current field number */
	int		ret = 0;	/* return value */

	ut_ad(dtuple_check_typed(dtuple));
	ut_ad(rec_offs_validate(rec, index, offsets));

	cur_field = *matched_fields;

	ut_ad(n_cmp > 0);
	ut_ad(n_cmp <= dtuple_get_n_fields(dtuple));
	ut_ad(cur_field <= n_cmp);
	ut_ad(cur_field <= rec_offs_n_fields(offsets));

	if (cur_field == 0) {
		ulint	rec_info = rec_get_info_bits(rec,
						     rec_offs_comp(offsets));
		ulint	tup_info = dtuple_get_info_bits(dtuple);

		/* The "infimum node pointer" is always first. */
		if (UNIV_UNLIKELY(rec_info & REC_INFO_MIN_REC_FLAG)) {
			ret = !(tup_info & REC_INFO_MIN_REC_FLAG);
			goto order_resolved;
		} else if (UNIV_UNLIKELY(tup_info & REC_INFO_MIN_REC_FLAG)) {
			ret = -1;
			goto order_resolved;
		}
	}

	/* Match fields in a loop */

	for (; cur_field < n_cmp; cur_field++) {
		const byte*	rec_b_ptr;
		const dfield_t*	dtuple_field
			= dtuple_get_nth_field(dtuple, cur_field);
		const byte*	dtuple_b_ptr
			= static_cast<const byte*>(
				dfield_get_data(dtuple_field));
		const dtype_t*	type
			= dfield_get_type(dtuple_field);
		ulint		dtuple_f_len
			= dfield_get_len(dtuple_field);
		ulint		rec_f_len;

		/* We should never compare against an externally
		stored field.  Only clustered index records can
		contain externally stored fields, and the first fields
		(primary key fields) should already differ. */
		ut_ad(!rec_offs_nth_extern(offsets, cur_field));
		/* We should never compare against instantly added columns.
		Columns can only be instantly added to clustered index
		leaf page records, and the first fields (primary key fields)
		should already differ. */
		ut_ad(!rec_offs_nth_default(offsets, cur_field));

		rec_b_ptr = rec_get_nth_field(rec, offsets, cur_field,
					      &rec_f_len);

		ut_ad(!dfield_is_ext(dtuple_field));

		ret = cmp_data(type->mtype, type->prtype, !index->is_ibuf()
			       && index->fields[cur_field].descending,
			       dtuple_b_ptr, dtuple_f_len,
			       rec_b_ptr, rec_f_len);
		if (ret) {
			goto order_resolved;
		}
	}

order_resolved:
	*matched_fields = cur_field;
	return(ret);
}

/** Get the pad character code point for a type.
@param[in]	type
@return		pad character code point
@retval		ULINT_UNDEFINED if no padding is specified */
UNIV_INLINE
ulint
cmp_get_pad_char(
	const dtype_t*	type)
{
	switch (type->mtype) {
	case DATA_FIXBINARY:
	case DATA_BINARY:
		if (dtype_get_charset_coll(type->prtype)
		    == DATA_MYSQL_BINARY_CHARSET_COLL) {
			/* Starting from 5.0.18, do not pad
			VARBINARY or BINARY columns. */
			return(ULINT_UNDEFINED);
		}
		/* Fall through */
	case DATA_CHAR:
	case DATA_VARCHAR:
	case DATA_MYSQL:
	case DATA_VARMYSQL:
		/* Space is the padding character for all char and binary
		strings, and starting from 5.0.3, also for TEXT strings. */
		return(0x20);
	case DATA_GEOMETRY:
                /* DATA_GEOMETRY is binary data, not ASCII-based. */
	        return(ULINT_UNDEFINED);
	case DATA_BLOB:
		if (!(type->prtype & DATA_BINARY_TYPE)) {
			return(0x20);
		}
		/* Fall through */
	default:
		/* No padding specified */
		return(ULINT_UNDEFINED);
	}
}

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
{
	ut_ad(dtuple_check_typed(dtuple));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!(REC_INFO_MIN_REC_FLAG
		& dtuple_get_info_bits(dtuple)));
	ut_ad(!index->is_ibuf());

	if (UNIV_UNLIKELY(REC_INFO_MIN_REC_FLAG
			  & rec_get_info_bits(rec, rec_offs_comp(offsets)))) {
		ut_ad(page_rec_is_first(rec, page_align(rec)));
		ut_ad(!page_has_prev(page_align(rec)));
		ut_ad(rec_is_metadata(rec, *index));
		return 1;
	}

	ulint cur_field = *matched_fields;
	ulint cur_bytes = *matched_bytes;
	ulint n_cmp = dtuple_get_n_fields_cmp(dtuple);
	int ret = 0;

	ut_ad(n_cmp <= dtuple_get_n_fields(dtuple));
	ut_ad(cur_field <= n_cmp);
	ut_ad(cur_field + (cur_bytes > 0) <= rec_offs_n_fields(offsets));

	/* Match fields in a loop; stop if we run out of fields in dtuple
	or find an externally stored field */

	while (cur_field < n_cmp) {
		const dfield_t*	dfield		= dtuple_get_nth_field(
			dtuple, cur_field);
		const dtype_t*	type		= dfield_get_type(dfield);
		ulint		dtuple_f_len	= dfield_get_len(dfield);
		const byte*	dtuple_b_ptr;
		const byte*	rec_b_ptr;
		ulint		rec_f_len;

		dtuple_b_ptr = static_cast<const byte*>(
			dfield_get_data(dfield));

		ut_ad(!rec_offs_nth_default(offsets, cur_field));
		rec_b_ptr = rec_get_nth_field(rec, offsets,
					      cur_field, &rec_f_len);
		ut_ad(!rec_offs_nth_extern(offsets, cur_field));

		/* If we have matched yet 0 bytes, it may be that one or
		both the fields are SQL null, or the record or dtuple may be
		the predefined minimum record. */
		if (cur_bytes == 0) {
			if (dtuple_f_len == UNIV_SQL_NULL) {
				if (rec_f_len == UNIV_SQL_NULL) {

					goto next_field;
				}

				ret = -1;
				goto order_resolved;
			} else if (rec_f_len == UNIV_SQL_NULL) {
				/* We define the SQL null to be the
				smallest possible value of a field
				in the alphabetical order */

				ret = 1;
				goto order_resolved;
			}
		}

		switch (type->mtype) {
		case DATA_FIXBINARY:
		case DATA_BINARY:
		case DATA_INT:
		case DATA_SYS_CHILD:
		case DATA_SYS:
			break;
		case DATA_BLOB:
			if (type->prtype & DATA_BINARY_TYPE) {
				break;
			}
			/* fall through */
		default:
			ret = cmp_data(type->mtype, type->prtype, false,
				       dtuple_b_ptr, dtuple_f_len,
				       rec_b_ptr, rec_f_len);

			if (!ret) {
				goto next_field;
			}

			cur_bytes = 0;
			goto order_resolved;
		}

		/* Set the pointers at the current byte */

		rec_b_ptr += cur_bytes;
		dtuple_b_ptr += cur_bytes;
		/* Compare then the fields */

		for (const ulint pad = cmp_get_pad_char(type);;
		     cur_bytes++) {
			ulint	rec_byte = pad;
			ulint	dtuple_byte = pad;

			if (rec_f_len <= cur_bytes) {
				if (dtuple_f_len <= cur_bytes) {

					goto next_field;
				}

				if (rec_byte == ULINT_UNDEFINED) {
					ret = 1;

					goto order_resolved;
				}
			} else {
				rec_byte = *rec_b_ptr++;
			}

			if (dtuple_f_len <= cur_bytes) {
				if (dtuple_byte == ULINT_UNDEFINED) {
					ret = -1;

					goto order_resolved;
				}
			} else {
				dtuple_byte = *dtuple_b_ptr++;
			}

			if (dtuple_byte < rec_byte) {
				ret = -1;
				goto order_resolved;
			} else if (dtuple_byte > rec_byte) {
				ret = 1;
				goto order_resolved;
			}
		}

next_field:
		cur_field++;
		cur_bytes = 0;
	}

	ut_ad(cur_bytes == 0);

order_resolved:
	*matched_fields = cur_field;
	*matched_bytes = cur_bytes;

	return !ret || UNIV_LIKELY(!index->fields[cur_field].descending)
		? ret : -ret;
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
{
  ulint	matched_fields= 0;
  ulint n_fields= dtuple_get_n_fields(dtuple);
  ut_ad(n_fields <= rec_offs_n_fields(offsets));
  cmp_dtuple_rec_with_match(dtuple, rec, index, offsets, &matched_fields);
  return matched_fields == n_fields;
}

/*************************************************************//**
Compare two physical record fields.
@retval positive if rec1 field is greater than rec2
@retval negative if rec1 field is less than rec2
@retval 0 if rec1 field equals to rec2 */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
int
cmp_rec_rec_simple_field(
/*=====================*/
	const rec_t*		rec1,	/*!< in: physical record */
	const rec_t*		rec2,	/*!< in: physical record */
	const rec_offs*		offsets1,/*!< in: rec_get_offsets(rec1, ...) */
	const rec_offs*		offsets2,/*!< in: rec_get_offsets(rec2, ...) */
	const dict_index_t*	index,	/*!< in: data dictionary index */
	ulint			n)	/*!< in: field to compare */
{
	const byte*	rec1_b_ptr;
	const byte*	rec2_b_ptr;
	ulint		rec1_f_len;
	ulint		rec2_f_len;
	const dict_field_t* field = dict_index_get_nth_field(index, n);

	ut_ad(!rec_offs_nth_extern(offsets1, n));
	ut_ad(!rec_offs_nth_extern(offsets2, n));

	rec1_b_ptr = rec_get_nth_field(rec1, offsets1, n, &rec1_f_len);
	rec2_b_ptr = rec_get_nth_field(rec2, offsets2, n, &rec2_f_len);

	return cmp_data(field->col->mtype, field->col->prtype,
			field->descending,
			rec1_b_ptr, rec1_f_len, rec2_b_ptr, rec2_f_len);
}

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
{
	ulint		n;
	ulint		n_uniq	= dict_index_get_n_unique(index);
	bool		null_eq	= false;

	ut_ad(rec_offs_n_fields(offsets1) >= n_uniq);
	ut_ad(rec_offs_n_fields(offsets2) == rec_offs_n_fields(offsets2));

	ut_ad(rec_offs_comp(offsets1) == rec_offs_comp(offsets2));

	for (n = 0; n < n_uniq; n++) {
		int cmp = cmp_rec_rec_simple_field(
			rec1, rec2, offsets1, offsets2, index, n);

		if (cmp) {
			return(cmp);
		}

		/* If the fields are internally equal, they must both
		be NULL or non-NULL. */
		ut_ad(rec_offs_nth_sql_null(offsets1, n)
		      == rec_offs_nth_sql_null(offsets2, n));

		if (rec_offs_nth_sql_null(offsets1, n)) {
			ut_ad(!(dict_index_get_nth_col(index, n)->prtype
				& DATA_NOT_NULL));
			null_eq = true;
		}
	}

	/* If we ran out of fields, the ordering columns of rec1 were
	equal to rec2. Issue a duplicate key error if needed. */

	if (!null_eq && table && dict_index_is_unique(index)) {
		/* Report erroneous row using new version of table. */
		innobase_rec_to_mysql(table, rec1, index, offsets1);
		return(0);
	}

	/* Else, keep comparing so that we have the full internal
	order. */
	for (; n < dict_index_get_n_fields(index); n++) {
		int cmp = cmp_rec_rec_simple_field(
			rec1, rec2, offsets1, offsets2, index, n);

		if (cmp) {
			return(cmp);
		}

		/* If the fields are internally equal, they must both
		be NULL or non-NULL. */
		ut_ad(rec_offs_nth_sql_null(offsets1, n)
		      == rec_offs_nth_sql_null(offsets2, n));
	}

	/* This should never be reached. Internally, an index must
	never contain duplicate entries. */
	ut_ad(0);
	return(0);
}

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
	bool			nulls_unequal,
	ulint*			matched_fields)
{
	ulint		rec1_f_len;	/* length of current field in rec */
	const byte*	rec1_b_ptr;	/* pointer to the current byte
					in rec field */
	ulint		rec2_f_len;	/* length of current field in rec */
	const byte*	rec2_b_ptr;	/* pointer to the current byte
					in rec field */
	ulint		cur_field = 0;	/* current field number */
	int		ret = 0;	/* return value */

	ut_ad(rec1 != NULL);
	ut_ad(rec2 != NULL);
	ut_ad(index != NULL);
	ut_ad(rec_offs_validate(rec1, index, offsets1));
	ut_ad(rec_offs_validate(rec2, index, offsets2));
	ut_ad(rec_offs_comp(offsets1) == rec_offs_comp(offsets2));
	ut_ad(fil_page_index_page_check(page_align(rec2)));
	ut_ad(!!dict_index_is_spatial(index)
	      == (fil_page_get_type(page_align(rec2)) == FIL_PAGE_RTREE));

	ulint comp = rec_offs_comp(offsets1);
	ulint n_fields;

	/* Test if rec is the predefined minimum record */
	if (UNIV_UNLIKELY(rec_get_info_bits(rec1, comp)
			  & REC_INFO_MIN_REC_FLAG)) {
		ret = UNIV_UNLIKELY(rec_get_info_bits(rec2, comp)
				    & REC_INFO_MIN_REC_FLAG)
			? 0 : -1;
		goto order_resolved;
	} else if (UNIV_UNLIKELY
		   (rec_get_info_bits(rec2, comp)
		    & REC_INFO_MIN_REC_FLAG)) {
		ret = 1;
		goto order_resolved;
	}

	/* For non-leaf spatial index records, the
	dict_index_get_n_unique_in_tree() does include the child page
	number, because spatial index node pointers only contain
	the MBR (minimum bounding rectangle) and the child page number.

	For B-tree node pointers, the key alone (secondary index
	columns and PRIMARY KEY columns) must be unique, and there is
	no need to compare the child page number. */
	n_fields = std::min(rec_offs_n_fields(offsets1),
			    rec_offs_n_fields(offsets2));
	n_fields = std::min<ulint>(n_fields,
				   dict_index_get_n_unique_in_tree(index));

	for (; cur_field < n_fields; cur_field++) {
		ulint	mtype;
		ulint	prtype;
		bool	descending;

		if (UNIV_UNLIKELY(dict_index_is_ibuf(index))) {
			/* This is for the insert buffer B-tree. */
			mtype = DATA_BINARY;
			prtype = 0;
			descending = false;
		} else {
			const dict_field_t* field = dict_index_get_nth_field(
				index, cur_field);
			descending = field->descending;
			mtype = field->col->mtype;
			prtype = field->col->prtype;

			if (UNIV_LIKELY(!dict_index_is_spatial(index))) {
			} else if (cur_field == 0) {
				ut_ad(DATA_GEOMETRY_MTYPE(mtype));
				prtype |= DATA_GIS_MBR;
			} else if (!page_rec_is_leaf(rec2)) {
				/* Compare the child page number. */
				ut_ad(cur_field == 1);
				mtype = DATA_SYS_CHILD;
				prtype = 0;
			}
		}

		/* We should never encounter an externally stored field.
		Externally stored fields only exist in clustered index
		leaf page records. These fields should already differ
		in the primary key columns already, before DB_TRX_ID,
		DB_ROLL_PTR, and any externally stored columns. */
		ut_ad(!rec_offs_nth_extern(offsets1, cur_field));
		ut_ad(!rec_offs_nth_extern(offsets2, cur_field));
		ut_ad(!rec_offs_nth_default(offsets1, cur_field));
		ut_ad(!rec_offs_nth_default(offsets2, cur_field));

		rec1_b_ptr = rec_get_nth_field(rec1, offsets1,
					       cur_field, &rec1_f_len);
		rec2_b_ptr = rec_get_nth_field(rec2, offsets2,
					       cur_field, &rec2_f_len);

		if (nulls_unequal
		    && rec1_f_len == UNIV_SQL_NULL
		    && rec2_f_len == UNIV_SQL_NULL) {
			ret = -1;
			goto order_resolved;
		}

		ret = cmp_data(mtype, prtype, descending,
			       rec1_b_ptr, rec1_f_len, rec2_b_ptr, rec2_f_len);
		if (ret) {
			goto order_resolved;
		}
	}

	/* If we ran out of fields, rec1 was equal to rec2 up
	to the common fields */
	ut_ad(ret == 0);
order_resolved:
	if (matched_fields) {
		*matched_fields = cur_field;
	}
	return ret;
}
