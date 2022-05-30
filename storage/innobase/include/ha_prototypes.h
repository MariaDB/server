/*****************************************************************************

Copyright (c) 2006, 2016, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/ha_prototypes.h
Prototypes for global functions in ha_innodb.cc that are called by
InnoDB C code.

NOTE: This header is intended to insulate InnoDB from SQL names and functions.
Do not include any headers other than univ.i into this unless they are very
simple headers.
************************************************************************/

#ifndef HA_INNODB_PROTOTYPES_H
#define HA_INNODB_PROTOTYPES_H

#include "univ.i"

#ifndef UNIV_INNOCHECKSUM

/* Forward declarations */
class THD;
class Field;

// JAN: TODO missing features:
#undef MYSQL_FT_INIT_EXT
#undef MYSQL_PFS
#undef MYSQL_STORE_FTS_DOC_ID

/*******************************************************************//**
Formats the raw data in "data" (in InnoDB on-disk format) that is of
type DATA_(CHAR|VARCHAR|MYSQL|VARMYSQL) using "charset_coll" and writes
the result to "buf". The result is converted to "system_charset_info".
Not more than "buf_size" bytes are written to "buf".
The result is always NUL-terminated (provided buf_size > 0) and the
number of bytes that were written to "buf" is returned (including the
terminating NUL).
@return number of bytes that were written */
ulint
innobase_raw_format(
/*================*/
	const char*	data,		/*!< in: raw data */
	ulint		data_len,	/*!< in: raw data length
					in bytes */
	ulint		charset_coll,	/*!< in: charset collation */
	char*		buf,		/*!< out: output buffer */
	ulint		buf_size);	/*!< in: output buffer size
					in bytes */

/*****************************************************************//**
Invalidates the MySQL query cache for the table. */
void
innobase_invalidate_query_cache(
/*============================*/
	trx_t*		trx,		/*!< in: transaction which
					modifies the table */
	const char*	full_name);	/*!< in: concatenation of
					database name, path separator,
					table name, null char NUL;
					NOTE that in Windows this is
					always in LOWER CASE! */

/** Quote a standard SQL identifier like tablespace, index or column name.
@param[in]	file	output stream
@param[in]	trx	InnoDB transaction, or NULL
@param[in]	id	identifier to quote */
void
innobase_quote_identifier(
	FILE*		file,
	trx_t*		trx,
	const char*	id);

/** Quote an standard SQL identifier like tablespace, index or column name.
Return the string as an std:string object.
@param[in]	trx	InnoDB transaction, or NULL
@param[in]	id	identifier to quote
@return a std::string with id properly quoted. */
std::string
innobase_quote_identifier(
	trx_t*		trx,
	const char*	id);

/*****************************************************************//**
Convert a table name to the MySQL system_charset_info (UTF-8).
@return pointer to the end of buf */
char*
innobase_convert_name(
/*==================*/
	char*		buf,	/*!< out: buffer for converted identifier */
	ulint		buflen,	/*!< in: length of buf, in bytes */
	const char*	id,	/*!< in: table name to convert */
	ulint		idlen,	/*!< in: length of id, in bytes */
	THD*		thd);	/*!< in: MySQL connection thread, or NULL */

/******************************************************************//**
Returns true if the transaction this thread is processing has edited
non-transactional tables. Used by the deadlock detector when deciding
which transaction to rollback in case of a deadlock - we try to avoid
rolling back transactions that have edited non-transactional tables.
@return true if non-transactional tables have been edited */
ibool
thd_has_edited_nontrans_tables(
/*===========================*/
	THD*	thd);	/*!< in: thread handle */

/*************************************************************//**
Prints info of a THD object (== user session thread) to the given file. */
void
innobase_mysql_print_thd(
/*=====================*/
	FILE*	f,		/*!< in: output stream */
	THD*	thd,		/*!< in: pointer to a MySQL THD object */
	uint	max_query_len);	/*!< in: max query length to print, or 0 to
				   use the default max length */

/** Converts a MySQL type to an InnoDB type. Note that this function returns
the 'mtype' of InnoDB. InnoDB differentiates between MySQL's old <= 4.1
VARCHAR and the new true VARCHAR in >= 5.0.3 by the 'prtype'.
@param[out]	unsigned_flag		DATA_UNSIGNED if an 'unsigned type';
at least ENUM and SET, and unsigned integer types are 'unsigned types'
@param[in]	f			MySQL Field
@return DATA_BINARY, DATA_VARCHAR, ... */
uint8_t
get_innobase_type_from_mysql_type(unsigned *unsigned_flag, const Field *field);

/******************************************************************//**
Compares NUL-terminated UTF-8 strings case insensitively.
@return 0 if a=b, <0 if a<b, >1 if a>b */
int
innobase_strcasecmp(
/*================*/
	const char*	a,	/*!< in: first string to compare */
	const char*	b);	/*!< in: second string to compare */

/** Strip dir name from a full path name and return only the file name
@param[in]	path_name	full path name
@return file name or "null" if no file name */
const char*
innobase_basename(
	const char*	path_name);

/******************************************************************//**
Converts an identifier to a table name. */
void
innobase_convert_from_table_id(
/*===========================*/
	CHARSET_INFO*	cs,	/*!< in: the 'from' character set */
	char*		to,	/*!< out: converted identifier */
	const char*	from,	/*!< in: identifier to convert */
	ulint		len);	/*!< in: length of 'to', in bytes; should
				be at least 5 * strlen(to) + 1 */
/******************************************************************//**
Converts an identifier to UTF-8. */
void
innobase_convert_from_id(
/*=====================*/
	CHARSET_INFO*	cs,	/*!< in: the 'from' character set */
	char*		to,	/*!< out: converted identifier */
	const char*	from,	/*!< in: identifier to convert */
	ulint		len);	/*!< in: length of 'to', in bytes;
				should be at least 3 * strlen(to) + 1 */
/******************************************************************//**
Makes all characters in a NUL-terminated UTF-8 string lower case. */
void
innobase_casedn_str(
/*================*/
	char*	a);	/*!< in/out: string to put in lower case */

#ifdef WITH_WSREP
ulint wsrep_innobase_mysql_sort(int mysql_type, uint charset_number,
                             unsigned char* str, ulint str_length,
                             ulint buf_length);
#endif /* WITH_WSREP */

extern "C" struct charset_info_st *thd_charset(THD *thd);

/** Get high resolution timestamp for the current query start time.
The timestamp is not anchored to any specific point in time,
but can be used for comparison.
@param thd user thread
@retval timestamp in microseconds precision
*/
extern "C" unsigned long long thd_start_utime(const MYSQL_THD thd);


/**
  Indicate the start of an async operation in a foreground thread.
@param thd          current_thd
@return thd
@retval nullptr   if this is not a foreground thread */
THD *innodb_thd_increment_pending_ops(THD *thd);

/** Determines the current SQL statement.
Thread unsafe, can only be called from the thread owning the THD.
@param[in]	thd	MySQL thread handle
@param[out]	length	Length of the SQL statement
@return			SQL statement string */
const char*
innobase_get_stmt_unsafe(
	THD*	thd,
	size_t*	length);

/******************************************************************//**
This function is used to find the storage length in bytes of the first n
characters for prefix indexes using a multibyte character set. The function
finds charset information and returns length of prefix_len characters in the
index field in bytes.
@return number of bytes occupied by the first n characters */
ulint
innobase_get_at_most_n_mbchars(
/*===========================*/
	ulint charset_id,	/*!< in: character set id */
	ulint prefix_len,	/*!< in: prefix length in bytes of the index
				(this has to be divided by mbmaxlen to get the
				number of CHARACTERS n in the prefix) */
	ulint data_len,		/*!< in: length of the string in bytes */
	const char* str);	/*!< in: character string */

/** Get status of innodb_tmpdir.
@param[in]	thd	thread handle, or NULL to query
			the global innodb_tmpdir.
@retval NULL if innodb_tmpdir="" */
const char *thd_innodb_tmpdir(THD *thd);

/******************************************************************//**
Returns the lock wait timeout for the current connection.
@return the lock wait timeout, in seconds */
uint&
thd_lock_wait_timeout(
/*==================*/
	THD*	thd);	/*!< in: thread handle, or NULL to query
			the global innodb_lock_wait_timeout */

/******************************************************************//**
compare two character string case insensitively according to their charset. */
int
innobase_fts_text_case_cmp(
/*=======================*/
	const void*	cs,		/*!< in: Character set */
	const void*	p1,		/*!< in: key */
	const void*	p2);		/*!< in: node */

/******************************************************************//**
Returns true if transaction should be flagged as read-only.
@return true if the thd is marked as read-only */
bool
thd_trx_is_read_only(
/*=================*/
	THD*	thd);	/*!< in/out: thread handle */

/******************************************************************//**
Check if the transaction is an auto-commit transaction. TRUE also
implies that it is a SELECT (read-only) transaction.
@return true if the transaction is an auto commit read-only transaction. */
ibool
thd_trx_is_auto_commit(
/*===================*/
	THD*	thd);	/*!< in: thread handle, or NULL */

/*****************************************************************//**
A wrapper function of innobase_convert_name(), convert a table name
to the MySQL system_charset_info (UTF-8) and quote it if needed.
@return pointer to the end of buf */
void
innobase_format_name(
/*==================*/
	char*		buf,	/*!< out: buffer for converted identifier */
	ulint		buflen,	/*!< in: length of buf, in bytes */
	const char*	name);	/*!< in: table name to format */

/** Corresponds to Sql_condition:enum_warning_level. */
enum ib_log_level_t {
	IB_LOG_LEVEL_INFO,
	IB_LOG_LEVEL_WARN,
	IB_LOG_LEVEL_ERROR,
	IB_LOG_LEVEL_FATAL
};

/******************************************************************//**
Use this when the args are first converted to a formatted string and then
passed to the format string from errmsg-utf8.txt. The error message format
must be: "Some string ... %s".

Push a warning message to the client, it is a wrapper around:

void push_warning_printf(
	THD *thd, Sql_condition::enum_warning_level level,
	uint code, const char *format, ...);
*/
void
ib_errf(
/*====*/
	THD*		thd,		/*!< in/out: session */
	ib_log_level_t	level,		/*!< in: warning level */
	ib_uint32_t	code,		/*!< MySQL error code */
	const char*	format,		/*!< printf format */
	...)				/*!< Args */
	MY_ATTRIBUTE((format(printf, 4, 5)));

/******************************************************************//**
Use this when the args are passed to the format string from
errmsg-utf8.txt directly as is.

Push a warning message to the client, it is a wrapper around:

void push_warning_printf(
	THD *thd, Sql_condition::enum_warning_level level,
	uint code, const char *format, ...);
*/
void
ib_senderrf(
/*========*/
	THD*		thd,		/*!< in/out: session */
	ib_log_level_t	level,		/*!< in: warning level */
	ib_uint32_t	code,		/*!< MySQL error code */
	...);				/*!< Args */

extern const char* 	TROUBLESHOOTING_MSG;
extern const char* 	TROUBLESHOOT_DATADICT_MSG;
extern const char* 	BUG_REPORT_MSG;
extern const char* 	FORCE_RECOVERY_MSG;
extern const char*      OPERATING_SYSTEM_ERROR_MSG;
extern const char*      FOREIGN_KEY_CONSTRAINTS_MSG;
extern const char*      SET_TRANSACTION_MSG;
extern const char*      INNODB_PARAMETERS_MSG;

/******************************************************************//**
Returns the NUL terminated value of glob_hostname.
@return pointer to glob_hostname. */
const char*
server_get_hostname();
/*=================*/

/*********************************************************************//**
Compute the next autoinc value.

For MySQL replication the autoincrement values can be partitioned among
the nodes. The offset is the start or origin of the autoincrement value
for a particular node. For n nodes the increment will be n and the offset
will be in the interval [1, n]. The formula tries to allocate the next
value for a particular node.

Note: This function is also called with increment set to the number of
values we want to reserve for multi-value inserts e.g.,

	INSERT INTO T VALUES(), (), ();

innobase_next_autoinc() will be called with increment set to 3 where
autoinc_lock_mode != TRADITIONAL because we want to reserve 3 values for
the multi-value INSERT above.
@return the next value */
ulonglong
innobase_next_autoinc(
/*==================*/
	ulonglong	current,	/*!< in: Current value */
	ulonglong	need,		/*!< in: count of values needed */
	ulonglong	step,		/*!< in: AUTOINC increment step */
	ulonglong	offset,		/*!< in: AUTOINC offset */
	ulonglong	max_value)	/*!< in: max value for type */
	MY_ATTRIBUTE((pure, warn_unused_result));

/**********************************************************************
Converts an identifier from my_charset_filename to UTF-8 charset. */
uint
innobase_convert_to_system_charset(
/*===============================*/
	char*           to,		/* out: converted identifier */
	const char*     from,		/* in: identifier to convert */
	ulint           len,		/* in: length of 'to', in bytes */
	uint*		errors);	/* out: error return */
/**********************************************************************
Check if the length of the identifier exceeds the maximum allowed.
The input to this function is an identifier in charset my_charset_filename.
return true when length of identifier is too long. */
my_bool
innobase_check_identifier_length(
/*=============================*/
	const char*	id);	/* in: identifier to check.  it must belong
				to charset my_charset_filename */

/**********************************************************************
Converts an identifier from my_charset_filename to UTF-8 charset. */
uint
innobase_convert_to_system_charset(
/*===============================*/
	char*		to,		/* out: converted identifier */
	const char*	from,		/* in: identifier to convert */
	ulint		len,		/* in: length of 'to', in bytes */
	uint*		errors);	/* out: error return */

/**********************************************************************
Converts an identifier from my_charset_filename to UTF-8 charset. */
uint
innobase_convert_to_filename_charset(
/*=================================*/
	char*		to,	/* out: converted identifier */
	const char*	from,	/* in: identifier to convert */
	ulint		len);	/* in: length of 'to', in bytes */

/********************************************************************//**
Helper function to push warnings from InnoDB internals to SQL-layer. */
void
ib_push_warning(
	trx_t*		trx,	/*!< in: trx */
	dberr_t		error,	/*!< in: error code to push as warning */
	const char	*format,/*!< in: warning message */
	...);

/********************************************************************//**
Helper function to push warnings from InnoDB internals to SQL-layer. */
void
ib_push_warning(
	void*		ithd,	/*!< in: thd */
	dberr_t		error,	/*!< in: error code to push as warning */
	const char	*format,/*!< in: warning message */
	...);

/********************************************************************//**
Helper function to push warnings from InnoDB internals to SQL-layer. */
void
ib_foreign_warn(
	trx_t*		trx,	/*!< in: trx */
	dberr_t		error,	/*!< in: error code to push as warning */
	const char	*table_name,
	const char	*format,/*!< in: warning message */
	...);

/*****************************************************************//**
Normalizes a table name string. A normalized name consists of the
database name catenated to '/' and table name. An example:
test/mytable. On Windows normalization puts both the database name and the
table name always to lower case if "set_lower_case" is set to TRUE. */
void
normalize_table_name_c_low(
/*=======================*/
	char*		norm_name,	/*!< out: normalized name as a
					null-terminated string */
	const char*	name,		/*!< in: table name string */
	bool		set_lower_case); /*!< in: true if we want to set
					name to lower case */

/** Create a MYSQL_THD for a background thread and mark it as such.
@param name thread info for SHOW PROCESSLIST
@return new MYSQL_THD */
MYSQL_THD innobase_create_background_thd(const char* name);

/** Destroy a background purge thread THD.
@param[in]	thd	MYSQL_THD to destroy */
void
innobase_destroy_background_thd(MYSQL_THD);

/** Close opened tables, free memory, delete items for a MYSQL_THD.
@param[in]	thd	MYSQL_THD to reset */
void
innobase_reset_background_thd(MYSQL_THD);

#endif /* !UNIV_INNOCHECKSUM */
#endif /* HA_INNODB_PROTOTYPES_H */
