/*****************************************************************************

Copyright (c) 1996, 2014, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2023, MariaDB Corporation.

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
@file include/data0type.ic
Data types

Created 1/16/1996 Heikki Tuuri
*******************************************************/

#include "mach0data.h"
#include "ha_prototypes.h"

/*********************************************************************//**
Determines if a MySQL string type is a subset of UTF-8.  This function
may return false negatives, in case further character-set collation
codes are introduced in MySQL later.
@return whether a subset of UTF-8 */
UNIV_INLINE
bool
dtype_is_utf8(
/*==========*/
	ulint	prtype)	/*!< in: precise data type */
{
	/* These codes have been copied from strings/ctype-extra.c
	and strings/ctype-utf8.c. */
	switch (dtype_get_charset_coll(prtype)) {
	case 11: /* ascii_general_ci */
	case 65: /* ascii_bin */
	case 33: /* utf8_general_ci */
	case 83: /* utf8_bin */
	case 254: /* utf8_general_cs */
		return true;
	}

	return false;
}

/*********************************************************************//**
Gets the MySQL type code from a dtype.
@return MySQL type code; this is NOT an InnoDB type code! */
UNIV_INLINE
ulint
dtype_get_mysql_type(
/*=================*/
	const dtype_t*	type)	/*!< in: type struct */
{
	return(type->prtype & 0xFFUL);
}

/*********************************************************************//**
Compute the mbminlen and mbmaxlen members of a data type structure. */
UNIV_INLINE
void
dtype_set_mblen(
/*============*/
	dtype_t*	type)	/*!< in/out: type */
{
	unsigned mbminlen, mbmaxlen;

	dtype_get_mblen(type->mtype, type->prtype, &mbminlen, &mbmaxlen);
	type->mbminlen = mbminlen & 7;
	type->mbmaxlen = mbmaxlen & 7;

	ut_ad(dtype_validate(type));
}

/*********************************************************************//**
Sets a data type structure. */
UNIV_INLINE
void
dtype_set(
/*======*/
	dtype_t*	type,	/*!< in: type struct to init */
	ulint		mtype,	/*!< in: main data type */
	ulint		prtype,	/*!< in: precise type */
	ulint		len)	/*!< in: precision of type */
{
	ut_ad(type);
	ut_ad(mtype <= DATA_MTYPE_MAX);

	type->mtype = static_cast<byte>(mtype);
	type->prtype = static_cast<unsigned>(prtype);
	type->len = static_cast<uint16_t>(len);

	dtype_set_mblen(type);
}

/*********************************************************************//**
Copies a data type structure. */
UNIV_INLINE
void
dtype_copy(
/*=======*/
	dtype_t*	type1,	/*!< in: type struct to copy to */
	const dtype_t*	type2)	/*!< in: type struct to copy from */
{
	*type1 = *type2;

	ut_ad(dtype_validate(type1));
}

/*********************************************************************//**
Gets the SQL main data type.
@return SQL main data type */
UNIV_INLINE
ulint
dtype_get_mtype(
/*============*/
	const dtype_t*	type)	/*!< in: data type */
{
	ut_ad(type);

	return(type->mtype);
}

/*********************************************************************//**
Gets the precise data type.
@return precise data type */
UNIV_INLINE
ulint
dtype_get_prtype(
/*=============*/
	const dtype_t*	type)	/*!< in: data type */
{
	ut_ad(type);

	return(type->prtype);
}

/*********************************************************************//**
Gets the type length.
@return fixed length of the type, in bytes, or 0 if variable-length */
UNIV_INLINE
ulint
dtype_get_len(
/*==========*/
	const dtype_t*	type)	/*!< in: data type */
{
	ut_ad(type);

	return(type->len);
}

/*********************************************************************//**
Gets the minimum length of a character, in bytes.
@return minimum length of a char, in bytes, or 0 if this is not a
character type */
UNIV_INLINE
ulint
dtype_get_mbminlen(
/*===============*/
	const dtype_t*	type)	/*!< in: type */
{
	return type->mbminlen;
}
/*********************************************************************//**
Gets the maximum length of a character, in bytes.
@return maximum length of a char, in bytes, or 0 if this is not a
character type */
UNIV_INLINE
ulint
dtype_get_mbmaxlen(
/*===============*/
	const dtype_t*	type)	/*!< in: type */
{
	return type->mbmaxlen;
}

/***********************************************************************//**
Returns the size of a fixed size data type, 0 if not a fixed size type.
@return fixed size, or 0 */
UNIV_INLINE
unsigned
dtype_get_fixed_size_low(
/*=====================*/
	ulint	mtype,		/*!< in: main type */
	ulint	prtype,		/*!< in: precise type */
	ulint	len,		/*!< in: length */
	ulint	mbminlen,	/*!< in: minimum length of a
				multibyte character, in bytes */
	ulint	mbmaxlen,	/*!< in: maximum length of a
				multibyte character, in bytes */
	ulint	comp)		/*!< in: nonzero=ROW_FORMAT=COMPACT  */
{
	switch (mtype) {
	case DATA_SYS:
#ifdef UNIV_DEBUG
		switch (prtype & DATA_MYSQL_TYPE_MASK) {
		case DATA_ROW_ID:
			ut_ad(len == DATA_ROW_ID_LEN);
			break;
		case DATA_TRX_ID:
			ut_ad(len == DATA_TRX_ID_LEN);
			break;
		case DATA_ROLL_PTR:
			ut_ad(len == DATA_ROLL_PTR_LEN);
			break;
		default:
			ut_ad(0);
			return(0);
		}
#endif /* UNIV_DEBUG */
		/* fall through */
	case DATA_CHAR:
	case DATA_FIXBINARY:
	case DATA_INT:
	case DATA_FLOAT:
	case DATA_DOUBLE:
		return static_cast<unsigned>(len);
	case DATA_MYSQL:
		if (prtype & DATA_BINARY_TYPE) {
			return static_cast<unsigned>(len);
		} else if (!comp) {
			return static_cast<unsigned>(len);
		} else {
			if (mbminlen == mbmaxlen) {
				return static_cast<unsigned>(len);
			}
		}
		/* Treat as variable-length. */
		/* fall through */
	case DATA_VARCHAR:
	case DATA_BINARY:
	case DATA_DECIMAL:
	case DATA_VARMYSQL:
	case DATA_GEOMETRY:
	case DATA_BLOB:
		return(0);
	default:
		ut_error;
	}

	return(0);
}

/***********************************************************************//**
Returns the minimum size of a data type.
@return minimum size */
UNIV_INLINE
unsigned
dtype_get_min_size_low(
/*===================*/
	ulint	mtype,		/*!< in: main type */
	ulint	prtype,		/*!< in: precise type */
	ulint	len,		/*!< in: length */
	ulint	mbminlen,	/*!< in: minimum length of a character */
	ulint	mbmaxlen)	/*!< in: maximum length of a character */
{
	switch (mtype) {
	case DATA_SYS:
#ifdef UNIV_DEBUG
		switch (prtype & DATA_MYSQL_TYPE_MASK) {
		case DATA_ROW_ID:
			ut_ad(len == DATA_ROW_ID_LEN);
			break;
		case DATA_TRX_ID:
			ut_ad(len == DATA_TRX_ID_LEN);
			break;
		case DATA_ROLL_PTR:
			ut_ad(len == DATA_ROLL_PTR_LEN);
			break;
		default:
			ut_ad(0);
			return(0);
		}
#endif /* UNIV_DEBUG */
		/* fall through */
	case DATA_CHAR:
	case DATA_FIXBINARY:
	case DATA_INT:
	case DATA_FLOAT:
	case DATA_DOUBLE:
		return static_cast<unsigned>(len);
	case DATA_MYSQL:
		if (prtype & DATA_BINARY_TYPE) {
			return static_cast<unsigned>(len);
		} else {
			if (mbminlen == mbmaxlen) {
				return static_cast<unsigned>(len);
			}

			/* this is a variable-length character set */
			ut_a(mbminlen > 0);
			ut_a(mbmaxlen > mbminlen);
			ut_a(len % mbmaxlen == 0);
			return static_cast<unsigned>(
				len * mbminlen / mbmaxlen);
		}
	case DATA_VARCHAR:
	case DATA_BINARY:
	case DATA_DECIMAL:
	case DATA_VARMYSQL:
	case DATA_GEOMETRY:
	case DATA_BLOB:
		return(0);
	default:
		ut_error;
	}

	return(0);
}

/***********************************************************************//**
Returns the maximum size of a data type. Note: types in system tables may be
incomplete and return incorrect information.
@return maximum size */
UNIV_INLINE
ulint
dtype_get_max_size_low(
/*===================*/
	ulint	mtype,		/*!< in: main type */
	ulint	len)		/*!< in: length */
{
	switch (mtype) {
	case DATA_SYS:
	case DATA_CHAR:
	case DATA_FIXBINARY:
	case DATA_INT:
	case DATA_FLOAT:
	case DATA_DOUBLE:
	case DATA_MYSQL:
	case DATA_VARCHAR:
	case DATA_BINARY:
	case DATA_DECIMAL:
	case DATA_VARMYSQL:
		return(len);
	case DATA_GEOMETRY:
	case DATA_BLOB:
		break;
	default:
		ut_error;
	}

	return(ULINT_MAX);
}

/***********************************************************************//**
Returns the ROW_FORMAT=REDUNDANT stored SQL NULL size of a type.
For fixed length types it is the fixed length of the type, otherwise 0.
@return SQL null storage size in ROW_FORMAT=REDUNDANT */
UNIV_INLINE
ulint
dtype_get_sql_null_size(
/*====================*/
	const dtype_t*	type,	/*!< in: type */
	ulint		comp)	/*!< in: nonzero=ROW_FORMAT=COMPACT  */
{
	return(dtype_get_fixed_size_low(type->mtype, type->prtype, type->len,
					type->mbminlen, type->mbmaxlen, comp));
}
