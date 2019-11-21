/*****************************************************************************

Copyright (c) 2006, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, MariaDB Corporation.

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
InnoDB C code

Created 5/11/2006 Osku Salerma
************************************************************************/

#ifndef HA_INNODB_PROTOTYPES_H
#define HA_INNODB_PROTOTYPES_H

#include "my_dbug.h"
#include "my_compare.h"
#include "my_sys.h"
#include "m_string.h"
#include "my_base.h"
#include "dur_prop.h"

#ifndef UNIV_INNOCHECKSUM
#include "mysqld_error.h"
#include "debug_sync.h"
#include "trx0types.h"
#endif

#include "m_ctype.h" /* CHARSET_INFO */

// Forward declarations
class Field;
struct fts_string_t;

/*********************************************************************//**
Wrapper around MySQL's copy_and_convert function.
@return	number of bytes copied to 'to' */
UNIV_INTERN
ulint
innobase_convert_string(
/*====================*/
	void*		to,		/*!< out: converted string */
	ulint		to_length,	/*!< in: number of bytes reserved
					for the converted string */
	CHARSET_INFO*	to_cs,		/*!< in: character set to convert to */
	const void*	from,		/*!< in: string to convert */
	ulint		from_length,	/*!< in: number of bytes to convert */
	CHARSET_INFO*	from_cs,	/*!< in: character set to convert
					from */
	uint*		errors);	/*!< out: number of errors encountered
					during the conversion */

/*******************************************************************//**
Formats the raw data in "data" (in InnoDB on-disk format) that is of
type DATA_(CHAR|VARCHAR|MYSQL|VARMYSQL) using "charset_coll" and writes
the result to "buf". The result is converted to "system_charset_info".
Not more than "buf_size" bytes are written to "buf".
The result is always NUL-terminated (provided buf_size > 0) and the
number of bytes that were written to "buf" is returned (including the
terminating NUL).
@return	number of bytes that were written */
UNIV_INTERN
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

#ifndef UNIV_INNOCHECKSUM

/*****************************************************************//**
Invalidates the MySQL query cache for the table. */
UNIV_INTERN
void
innobase_invalidate_query_cache(
/*============================*/
	trx_t*		trx,		/*!< in: transaction which
					modifies the table */
	const char*	full_name,	/*!< in: concatenation of
					database name, null char NUL,
					table name, null char NUL;
					NOTE that in Windows this is
					always in LOWER CASE! */
	ulint		full_name_len);	/*!< in: full name length where
					also the null chars count */

#endif /* #ifndef UNIV_INNOCHECKSUM */

/*****************************************************************//**
Convert a table or index name to the MySQL system_charset_info (UTF-8)
and quote it if needed.
@return	pointer to the end of buf */
UNIV_INTERN
char*
innobase_convert_name(
/*==================*/
	char*		buf,	/*!< out: buffer for converted identifier */
	ulint		buflen,	/*!< in: length of buf, in bytes */
	const char*	id,	/*!< in: identifier to convert */
	ulint		idlen,	/*!< in: length of id, in bytes */
	THD*		thd,	/*!< in: MySQL connection thread, or NULL */
	ibool		table_id);/*!< in: TRUE=id is a table or database name;
				FALSE=id is an index name */

/******************************************************************//**
Returns true if the thread is the replication thread on the slave
server. Used in srv_conc_enter_innodb() to determine if the thread
should be allowed to enter InnoDB - the replication thread is treated
differently than other threads. Also used in
srv_conc_force_exit_innodb().
@return	true if thd is the replication thread */
UNIV_INTERN
ibool
thd_is_replication_slave_thread(
/*============================*/
	THD*	thd);	/*!< in: thread handle */

/******************************************************************//**
Gets information on the durability property requested by thread.
Used when writing either a prepare or commit record to the log
buffer.
@return the durability property. */
UNIV_INTERN
enum durability_properties
thd_requested_durability(
/*=====================*/
	const THD* thd)	/*!< in: thread handle */
	MY_ATTRIBUTE((warn_unused_result));

/******************************************************************//**
Returns true if the transaction this thread is processing has edited
non-transactional tables. Used by the deadlock detector when deciding
which transaction to rollback in case of a deadlock - we try to avoid
rolling back transactions that have edited non-transactional tables.
@return	true if non-transactional tables have been edited */
UNIV_INTERN
ibool
thd_has_edited_nontrans_tables(
/*===========================*/
	THD*	thd);	/*!< in: thread handle */

/**
Get high resolution timestamp for the current query start time.

@retval timestamp in microseconds precision
*/
unsigned long long thd_query_start_micro(const MYSQL_THD thd);

/*************************************************************//**
Prints info of a THD object (== user session thread) to the given file. */
UNIV_INTERN
void
innobase_mysql_print_thd(
/*=====================*/
	FILE*	f,		/*!< in: output stream */
	THD*	thd,		/*!< in: pointer to a MySQL THD object */
	uint	max_query_len);	/*!< in: max query length to print, or 0 to
				   use the default max length */

/*****************************************************************//**
Log code calls this whenever log has been written and/or flushed up
to a new position. We use this to notify upper layer of a new commit
checkpoint when necessary.*/
UNIV_INTERN
void
innobase_mysql_log_notify(
/*===============*/
	ib_uint64_t	write_lsn,	/*!< in: LSN written to log file */
	ib_uint64_t	flush_lsn);	/*!< in: LSN flushed to disk */

/*************************************************************//**
InnoDB uses this function to compare two data fields for which the data type
is such that we must use MySQL code to compare them.
@return	1, 0, -1, if a is greater, equal, less than b, respectively */
UNIV_INTERN
int
innobase_mysql_cmp(
/*===============*/
	int		mysql_type,	/*!< in: MySQL type */
	uint		charset_number,	/*!< in: number of the charset */
	const unsigned char* a,		/*!< in: data field */
	unsigned int	a_length,	/*!< in: data field length,
					not UNIV_SQL_NULL */
	const unsigned char* b,		/*!< in: data field */
	unsigned int	b_length)	/*!< in: data field length,
					not UNIV_SQL_NULL */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/**************************************************************//**
Converts a MySQL type to an InnoDB type. Note that this function returns
the 'mtype' of InnoDB. InnoDB differentiates between MySQL's old <= 4.1
VARCHAR and the new true VARCHAR in >= 5.0.3 by the 'prtype'.
@return	DATA_BINARY, DATA_VARCHAR, ... */
UNIV_INTERN
ulint
get_innobase_type_from_mysql_type(
/*==============================*/
	ulint*		unsigned_flag,	/*!< out: DATA_UNSIGNED if an
					'unsigned type';
					at least ENUM and SET,
					and unsigned integer
					types are 'unsigned types' */
	const void*	field)		/*!< in: MySQL Field */
	MY_ATTRIBUTE((nonnull));

/******************************************************************//**
Get the variable length bounds of the given character set. */
UNIV_INTERN
void
innobase_get_cset_width(
/*====================*/
	ulint	cset,		/*!< in: MySQL charset-collation code */
	ulint*	mbminlen,	/*!< out: minimum length of a char (in bytes) */
	ulint*	mbmaxlen);	/*!< out: maximum length of a char (in bytes) */

/******************************************************************//**
Compares NUL-terminated UTF-8 strings case insensitively.
@return	0 if a=b, <0 if a<b, >1 if a>b */
UNIV_INTERN
int
innobase_strcasecmp(
/*================*/
	const char*	a,	/*!< in: first string to compare */
	const char*	b);	/*!< in: second string to compare */

/******************************************************************//**
Compares NUL-terminated UTF-8 strings case insensitively. The
second string contains wildcards.
@return 0 if a match is found, 1 if not */
UNIV_INTERN
int
innobase_wildcasecmp(
/*=================*/
	const char*	a,	/*!< in: string to compare */
	const char*	b);	/*!< in: wildcard string to compare */

/******************************************************************//**
Strip dir name from a full path name and return only its file name.
@return file name or "null" if no file name */
UNIV_INTERN
const char*
innobase_basename(
/*==============*/
	const char*	path_name);	/*!< in: full path name */

/******************************************************************//**
Returns true if the thread is executing a SELECT statement.
@return	true if thd is executing SELECT */
UNIV_INTERN
ibool
thd_is_select(
/*==========*/
	const THD*	thd);	/*!< in: thread handle */

/******************************************************************//**
Converts an identifier to a table name. */
UNIV_INTERN
void
innobase_convert_from_table_id(
/*===========================*/
	struct charset_info_st*	cs,	/*!< in: the 'from' character set */
	char*			to,	/*!< out: converted identifier */
	const char*		from,	/*!< in: identifier to convert */
	ulint			len);	/*!< in: length of 'to', in bytes; should
					be at least 5 * strlen(to) + 1 */
/******************************************************************//**
Converts an identifier to UTF-8. */
UNIV_INTERN
void
innobase_convert_from_id(
/*=====================*/
	struct charset_info_st*	cs,	/*!< in: the 'from' character set */
	char*			to,	/*!< out: converted identifier */
	const char*		from,	/*!< in: identifier to convert */
	ulint			len);	/*!< in: length of 'to', in bytes;
					should be at least 3 * strlen(to) + 1 */
/******************************************************************//**
Makes all characters in a NUL-terminated UTF-8 string lower case. */
UNIV_INTERN
void
innobase_casedn_str(
/*================*/
	char*	a);	/*!< in/out: string to put in lower case */

#ifdef WITH_WSREP
UNIV_INTERN
int
wsrep_innobase_kill_one_trx(void * const thd_ptr,
                            const trx_t * const bf_trx,
                            trx_t *victim_trx,
                            ibool signal);
int wsrep_innobase_mysql_sort(int mysql_type, uint charset_number,
			      unsigned char* str, unsigned int str_length,
			      unsigned int buf_length);
#endif /* WITH_WSREP */
/**********************************************************************//**
Determines the connection character set.
@return	connection character set */
UNIV_INTERN
struct charset_info_st*
innobase_get_charset(
/*=================*/
	THD*	thd);	/*!< in: MySQL thread handle */
/**********************************************************************//**
Determines the current SQL statement.
@return	SQL statement string */
UNIV_INTERN
const char*
innobase_get_stmt(
/*==============*/
	THD*	thd,		/*!< in: MySQL thread handle */
	size_t*	length)		/*!< out: length of the SQL statement */
	MY_ATTRIBUTE((nonnull));
/******************************************************************//**
This function is used to find the storage length in bytes of the first n
characters for prefix indexes using a multibyte character set. The function
finds charset information and returns length of prefix_len characters in the
index field in bytes.
@return	number of bytes occupied by the first n characters */
UNIV_INTERN
ulint
innobase_get_at_most_n_mbchars(
/*===========================*/
	ulint charset_id,	/*!< in: character set id */
	ulint prefix_len,	/*!< in: prefix length in bytes of the index
				(this has to be divided by mbmaxlen to get the
				number of CHARACTERS n in the prefix) */
	ulint data_len,		/*!< in: length of the string in bytes */
	const char* str);	/*!< in: character string */

/*************************************************************//**
InnoDB index push-down condition check
@return ICP_NO_MATCH, ICP_MATCH, or ICP_OUT_OF_RANGE */
UNIV_INTERN
enum icp_result
innobase_index_cond(
/*================*/
	void*	file)	/*!< in/out: pointer to ha_innobase */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/******************************************************************//**
Returns true if the thread supports XA,
global value of innodb_supports_xa if thd is NULL.
@return	true if thd supports XA */
UNIV_INTERN
ibool
thd_supports_xa(
/*============*/
	THD*	thd);	/*!< in: thread handle, or NULL to query
			the global innodb_supports_xa */

/** Get status of innodb_tmpdir.
@param[in]	thd	thread handle, or NULL to query
			the global innodb_tmpdir.
@retval NULL if innodb_tmpdir="" */
UNIV_INTERN
const char*
thd_innodb_tmpdir(
	THD*	thd);

/******************************************************************//**
Check the status of fake changes mode (innodb_fake_changes)
@return	true	if fake change mode is enabled. */
UNIV_INTERN
ibool
thd_fake_changes(
/*=============*/
	THD*	thd);	/*!< in: thread handle, or NULL to query
			the global innodb_supports_xa */

/******************************************************************//**
Returns the lock wait timeout for the current connection.
@return	the lock wait timeout, in seconds */
UNIV_INTERN
ulong
thd_lock_wait_timeout(
/*==================*/
	THD*	thd);	/*!< in: thread handle, or NULL to query
			the global innodb_lock_wait_timeout */
/******************************************************************//**
Add up the time waited for the lock for the current query. */
UNIV_INTERN
void
thd_set_lock_wait_time(
/*===================*/
	THD*	thd,	/*!< in/out: thread handle */
	ulint	value);	/*!< in: time waited for the lock */

/**********************************************************************//**
Get the current setting of the table_cache_size global parameter. We do
a dirty read because for one there is no synchronization object and
secondly there is little harm in doing so even if we get a torn read.
@return	SQL statement string */
UNIV_INTERN
ulint
innobase_get_table_cache_size(void);
/*===============================*/

/******************************************************************//**
								     */
ulong
thd_flush_log_at_trx_commit(
/*================================*/
	THD*	thd);

/**********************************************************************//**
Get the current setting of the lower_case_table_names global parameter from
mysqld.cc. We do a dirty read because for one there is no synchronization
object and secondly there is little harm in doing so even if we get a torn
read.
@return	value of lower_case_table_names */
UNIV_INTERN
ulint
innobase_get_lower_case_table_names(void);
/*=====================================*/

/*****************************************************************//**
Frees a possible InnoDB trx object associated with the current THD.
@return 0 or error number */
UNIV_INTERN
int
innobase_close_thd(
/*===============*/
	THD*	thd);		/*!< in: MySQL thread handle for
				which to close the connection */
/*************************************************************//**
Get the next token from the given string and store it in *token. */
UNIV_INTERN
ulint
innobase_mysql_fts_get_token(
/*=========================*/
	CHARSET_INFO*	charset,	/*!< in: Character set */
	const byte*	start,		/*!< in: start of text */
	const byte*	end,		/*!< in: one character past end of
					text */
	fts_string_t*	token,		/*!< out: token's text */
	ulint*		offset);	/*!< out: offset to token,
					measured as characters from
					'start' */

/******************************************************************//**
compare two character string case insensitively according to their charset. */
UNIV_INTERN
int
innobase_fts_text_case_cmp(
/*=======================*/
	const void*	cs,		/*!< in: Character set */
	const void*	p1,		/*!< in: key */
	const void*	p2);		/*!< in: node */

/****************************************************************//**
Get FTS field charset info from the field's prtype
@return charset info */
UNIV_INTERN
CHARSET_INFO*
innobase_get_fts_charset(
/*=====================*/
	int		mysql_type,	/*!< in: MySQL type */
	uint		charset_number);/*!< in: number of the charset */
/******************************************************************//**
Returns true if transaction should be flagged as read-only.
@return	true if the thd is marked as read-only */
UNIV_INTERN
ibool
thd_trx_is_read_only(
/*=================*/
	THD*	thd);	/*!< in/out: thread handle */

/******************************************************************//**
Check if the transaction is an auto-commit transaction. TRUE also
implies that it is a SELECT (read-only) transaction.
@return	true if the transaction is an auto commit read-only transaction. */
UNIV_INTERN
ibool
thd_trx_is_auto_commit(
/*===================*/
	THD*	thd);	/*!< in: thread handle, or NULL */

/*****************************************************************//**
A wrapper function of innobase_convert_name(), convert a table or
index name to the MySQL system_charset_info (UTF-8) and quote it if needed.
@return	pointer to the end of buf */
UNIV_INTERN
void
innobase_format_name(
/*==================*/
	char*		buf,		/*!< out: buffer for converted
					identifier */
	ulint		buflen,		/*!< in: length of buf, in bytes */
	const char*	name,		/*!< in: index or table name
					to format */
	ibool		is_index_name)	/*!< in: index name */
	MY_ATTRIBUTE((nonnull));

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
UNIV_INTERN
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
UNIV_INTERN
void
ib_senderrf(
/*========*/
	THD*		thd,		/*!< in/out: session */
	ib_log_level_t	level,		/*!< in: warning level */
	ib_uint32_t	code,		/*!< MySQL error code */
	...);				/*!< Args */

/******************************************************************//**
Write a message to the MySQL log, prefixed with "InnoDB: ".
Wrapper around sql_print_information() */
UNIV_INTERN
void
ib_logf(
/*====*/
	ib_log_level_t	level,		/*!< in: warning level */
	const char*	format,		/*!< printf format */
	...)				/*!< Args */
	MY_ATTRIBUTE((format(printf, 2, 3)));

/******************************************************************//**
Returns the NUL terminated value of glob_hostname.
@return	pointer to glob_hostname. */
UNIV_INTERN
const char*
server_get_hostname();
/*=================*/

/******************************************************************//**
Get the error message format string.
@return the format string or 0 if not found. */
UNIV_INTERN
const char*
innobase_get_err_msg(
/*=================*/
	int	error_code);	/*!< in: MySQL error code */

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
@return	the next value */
UNIV_INTERN
ulonglong
innobase_next_autoinc(
/*==================*/
	ulonglong	current,	/*!< in: Current value */
	ulonglong	need,		/*!< in: count of values needed */
	ulonglong	step,		/*!< in: AUTOINC increment step */
	ulonglong	offset,		/*!< in: AUTOINC offset */
	ulonglong	max_value)	/*!< in: max value for type */
	MY_ATTRIBUTE((pure, warn_unused_result));

/********************************************************************//**
Get the upper limit of the MySQL integral and floating-point type.
@return maximum allowed value for the field */
UNIV_INTERN
ulonglong
innobase_get_int_col_max_value(
/*===========================*/
	const Field*	field)	/*!< in: MySQL field */
	MY_ATTRIBUTE((nonnull, pure, warn_unused_result));

/**********************************************************************
Check if the length of the identifier exceeds the maximum allowed.
The input to this function is an identifier in charset my_charset_filename.
return true when length of identifier is too long. */
UNIV_INTERN
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
	char*           to,		/* out: converted identifier */
	const char*     from,		/* in: identifier to convert */
	ulint           len,		/* in: length of 'to', in bytes */
	uint*		errors);	/* out: error return */

/**********************************************************************
Converts an identifier from my_charset_filename to UTF-8 charset. */
uint
innobase_convert_to_filename_charset(
/*=================================*/
	char*           to,     /* out: converted identifier */
	const char*     from,   /* in: identifier to convert */
	ulint           len);   /* in: length of 'to', in bytes */

/********************************************************************//**
Helper function to push warnings from InnoDB internals to SQL-layer. */
UNIV_INTERN
void
ib_push_warning(
	trx_t*		trx,	/*!< in: trx */
	ulint		error,	/*!< in: error code to push as warning */
	const char	*format,/*!< in: warning message */
	...);
/********************************************************************//**
Helper function to push warnings from InnoDB internals to SQL-layer. */
UNIV_INTERN
void
ib_push_warning(
	void*		ithd,	/*!< in: thd */
	ulint		error,	/*!< in: error code to push as warning */
	const char	*format,/*!< in: warning message */
	...);

/*****************************************************************//**
Normalizes a table name string. A normalized name consists of the
database name catenated to '/' and table name. An example:
test/mytable. On Windows normalization puts both the database name and the
table name always to lower case if "set_lower_case" is set to TRUE. */
void
normalize_table_name_low(
/*=====================*/
	char*		norm_name,	/*!< out: normalized name as a
					null-terminated string */
	const char*	name,		/*!< in: table name string */
	ibool		set_lower_case); /*!< in: TRUE if we want to set
					name to lower case */
#endif /* HA_INNODB_PROTOTYPES_H */
