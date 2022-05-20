/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2022, MariaDB Corporation.

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
@file include/dict0types.h
Data dictionary global types

Created 1/8/1996 Heikki Tuuri
*******************************************************/

#ifndef dict0types_h
#define dict0types_h

#include "univ.i"
#include "span.h"
#include <rem0types.h>

using st_::span;

struct dict_col_t;
struct dict_field_t;
struct dict_index_t;
struct dict_table_t;
struct dict_foreign_t;
struct dict_v_col_t;

struct ind_node_t;
struct tab_node_t;
struct dict_add_v_col_t;

/* Space id and page no where the dictionary header resides */
#define	DICT_HDR_SPACE		0	/* the SYSTEM tablespace */
#define	DICT_HDR_PAGE_NO	FSP_DICT_HDR_PAGE_NO

/* The ibuf table and indexes's ID are assigned as the number
DICT_IBUF_ID_MIN plus the space id */
#define DICT_IBUF_ID_MIN	0xFFFFFFFF00000000ULL

typedef ib_id_t		table_id_t;
typedef ib_id_t		index_id_t;

/** Maximum transaction identifier */
#define TRX_ID_MAX	IB_ID_MAX

/** The bit pattern corresponding to TRX_ID_MAX */
extern const byte trx_id_max_bytes[8];
extern const byte timestamp_max_bytes[7];

/** Error to ignore when we load table dictionary into memory. However,
the table and index will be marked as "corrupted", and caller will
be responsible to deal with corrupted table or index.
Note: please define the IGNORE_ERR_* as bits, so their value can
be or-ed together */
enum dict_err_ignore_t {
	DICT_ERR_IGNORE_NONE = 0,	/*!< no error to ignore */
	DICT_ERR_IGNORE_FK_NOKEY = 1,	/*!< ignore error if any foreign
					key is missing */
	DICT_ERR_IGNORE_INDEX = 2,	/*!< ignore corrupted indexes */
	DICT_ERR_IGNORE_RECOVER_LOCK = 4 | DICT_ERR_IGNORE_FK_NOKEY,
					/*!< Used when recovering table locks
					for resurrected transactions.
					Silently load a missing
					tablespace, and do not load
					incomplete index definitions. */
	/** ignore all errors above */
	DICT_ERR_IGNORE_ALL = 7,
	/** prepare some DDL operation;
	do not attempt to load tablespace */
	DICT_ERR_IGNORE_TABLESPACE = 15,
	/** prepare to drop the table; do not attempt to load tablespace
	or the metadata */
	DICT_ERR_IGNORE_DROP = 31
};

/** Quiescing states for flushing tables to disk. */
enum ib_quiesce_t {
	QUIESCE_NONE,
	QUIESCE_START,			/*!< Initialise, prepare to start */
	QUIESCE_COMPLETE		/*!< All done */
};

/** Prefix for InnoDB internal tables, adopted from sql/table.h */
#define TEMP_FILE_PREFIX_INNODB		"#sql-ib"

/** Table name wrapper for pretty-printing */
struct table_name_t
{
	/** The name in internal representation */
	char*	m_name;

	/** Default constructor */
	table_name_t() {}
	/** Constructor */
	table_name_t(char* name) : m_name(name) {}

	/** @return the end of the schema name */
	const char* dbend() const
	{
		const char* sep = strchr(m_name, '/');
		ut_ad(sep);
		return sep;
	}

	/** @return the length of the schema name, in bytes */
	size_t dblen() const { return size_t(dbend() - m_name); }

	/** Determine the filename-safe encoded table name.
	@return	the filename-safe encoded table name */
	const char* basename() const { return dbend() + 1; }

	/** The start of the table basename suffix for partitioned tables */
	static const char part_suffix[4];

	/** Determine the partition or subpartition name suffix.
	@return the partition name
	@retval	NULL	if the table is not partitioned */
	const char* part() const { return strstr(basename(), part_suffix); }

	/** @return whether this is a temporary or intermediate table name */
	inline bool is_temporary() const;
};

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
/** Dump the change buffer at startup */
extern my_bool		ibuf_dump;
/** Flag to control insert buffer debugging. */
extern uint		ibuf_debug;
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

/** Shift for spatial status */
#define SPATIAL_STATUS_SHIFT	12

/** Mask to encode/decode spatial status. */
#define SPATIAL_STATUS_MASK	(3U << SPATIAL_STATUS_SHIFT)

#if SPATIAL_STATUS_MASK < REC_VERSION_56_MAX_INDEX_COL_LEN
# error SPATIAL_STATUS_MASK < REC_VERSION_56_MAX_INDEX_COL_LEN
#endif

/** whether a col is used in spatial index or regular index
Note: the spatial status is part of persistent undo log,
so we should not modify the values in MySQL 5.7 */
enum spatial_status_t {
	/* Unkown status (undo format in 5.7.9) */
	SPATIAL_UNKNOWN = 0,

	/** Not used in gis index. */
	SPATIAL_NONE	= 1,

	/** Used in both spatial index and regular index. */
	SPATIAL_MIXED	= 2,

	/** Only used in spatial index. */
	SPATIAL_ONLY	= 3
};

#define TABLE_STATS_NAME "mysql/innodb_table_stats"
#define INDEX_STATS_NAME "mysql/innodb_index_stats"

#endif
