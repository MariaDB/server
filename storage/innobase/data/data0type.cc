/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2019, MariaDB Corporation.

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
@file data/data0type.cc
Data types

Created 1/16/1996 Heikki Tuuri
*******************************************************/

#include "dict0mem.h"
#include "my_sys.h"

/** The DB_TRX_ID,DB_ROLL_PTR values for "no history is available" */
const byte reset_trx_id[DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN] = {
	0, 0, 0, 0, 0, 0,
	0x80, 0, 0, 0, 0, 0, 0
};

/* At the database startup we store the default-charset collation number of
this MySQL installation to this global variable. If we have < 4.1.2 format
column definitions, or records in the insert buffer, we use this
charset-collation code for them. */

ulint	data_mysql_default_charset_coll;

/*********************************************************************//**
Determine how many bytes the first n characters of the given string occupy.
If the string is shorter than n characters, returns the number of bytes
the characters in the string occupy.
@return length of the prefix, in bytes */
ulint
dtype_get_at_most_n_mbchars(
/*========================*/
	ulint		prtype,		/*!< in: precise type */
	ulint		mbminlen,	/*!< in: minimum length of
					a multi-byte character, in bytes */
	ulint		mbmaxlen,	/*!< in: maximum length of
					a multi-byte character, in bytes */
	ulint		prefix_len,	/*!< in: length of the requested
					prefix, in characters, multiplied by
					dtype_get_mbmaxlen(dtype) */
	ulint		data_len,	/*!< in: length of str (in bytes) */
	const char*	str)		/*!< in: the string whose prefix
					length is being determined */
{
	ut_a(len_is_stored(data_len));
	ut_ad(!mbmaxlen || !(prefix_len % mbmaxlen) || !(prefix_len % 4));

	if (mbminlen != mbmaxlen) {
		ut_a(!(prefix_len % mbmaxlen) || !(prefix_len % 4));
		return(innobase_get_at_most_n_mbchars(
			dtype_get_charset_coll(prtype),
			prefix_len, data_len, str));
	}

	if (prefix_len < data_len) {

		return(prefix_len);

	}

	return(data_len);
}

/*********************************************************************//**
Validates a data type structure.
@return TRUE if ok */
ibool
dtype_validate(
/*===========*/
	const dtype_t*	type)	/*!< in: type struct to validate */
{
	ut_a(type);
	ut_a(type->mtype >= DATA_VARCHAR);
	ut_a(type->mtype <= DATA_MTYPE_MAX);

	if (type->mtype == DATA_SYS) {
		ut_a((type->prtype & DATA_MYSQL_TYPE_MASK) < DATA_N_SYS_COLS);
	}

	ut_a(dtype_get_mbminlen(type) <= dtype_get_mbmaxlen(type));

	return(TRUE);
}

#ifdef UNIV_DEBUG
/** Print a data type structure.
@param[in]	type	data type */
void
dtype_print(const dtype_t* type)
{
	ulint	mtype;
	ulint	prtype;
	ulint	len;

	ut_a(type);

	mtype = type->mtype;
	prtype = type->prtype;

	switch (mtype) {
	case DATA_VARCHAR:
		fputs("DATA_VARCHAR", stderr);
		break;

	case DATA_CHAR:
		fputs("DATA_CHAR", stderr);
		break;

	case DATA_BINARY:
		fputs("DATA_BINARY", stderr);
		break;

	case DATA_FIXBINARY:
		fputs("DATA_FIXBINARY", stderr);
		break;

	case DATA_BLOB:
		fputs("DATA_BLOB", stderr);
		break;

	case DATA_GEOMETRY:
		fputs("DATA_GEOMETRY", stderr);
		break;

	case DATA_INT:
		fputs("DATA_INT", stderr);
		break;

	case DATA_MYSQL:
		fputs("DATA_MYSQL", stderr);
		break;

	case DATA_SYS:
		fputs("DATA_SYS", stderr);
		break;

	case DATA_FLOAT:
		fputs("DATA_FLOAT", stderr);
		break;

	case DATA_DOUBLE:
		fputs("DATA_DOUBLE", stderr);
		break;

	case DATA_DECIMAL:
		fputs("DATA_DECIMAL", stderr);
		break;

	case DATA_VARMYSQL:
		fputs("DATA_VARMYSQL", stderr);
		break;

	default:
		fprintf(stderr, "type %lu", (ulong) mtype);
		break;
	}

	len = type->len;

	if ((type->mtype == DATA_SYS)
	    || (type->mtype == DATA_VARCHAR)
	    || (type->mtype == DATA_CHAR)) {
		putc(' ', stderr);
		if (prtype == DATA_ROW_ID) {
			fputs("DATA_ROW_ID", stderr);
			len = DATA_ROW_ID_LEN;
		} else if (prtype == DATA_ROLL_PTR) {
			fputs("DATA_ROLL_PTR", stderr);
			len = DATA_ROLL_PTR_LEN;
		} else if (prtype == DATA_TRX_ID) {
			fputs("DATA_TRX_ID", stderr);
			len = DATA_TRX_ID_LEN;
		} else if (prtype == DATA_ENGLISH) {
			fputs("DATA_ENGLISH", stderr);
		} else {
			fprintf(stderr, "prtype %lu", (ulong) prtype);
		}
	} else {
		if (prtype & DATA_UNSIGNED) {
			fputs(" DATA_UNSIGNED", stderr);
		}

		if (prtype & DATA_BINARY_TYPE) {
			fputs(" DATA_BINARY_TYPE", stderr);
		}

		if (prtype & DATA_NOT_NULL) {
			fputs(" DATA_NOT_NULL", stderr);
		}
	}

	fprintf(stderr, " len %lu", (ulong) len);
}
#endif /* UNIV_DEBUG */
