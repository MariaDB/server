/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2013, 2021, MariaDB Corporation.

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
@file include/dict0mem.h
Data dictionary memory object creation

Created 1/8/1996 Heikki Tuuri
*******************************************************/

#ifndef dict0mem_h
#define dict0mem_h

#include "dict0types.h"
#include "data0type.h"
#include "mem0mem.h"
#include "row0types.h"
#include "btr0types.h"
#include "lock0types.h"
#include "que0types.h"
#include "sux_lock.h"
#include "ut0mem.h"
#include "ut0rnd.h"
#include "ut0byte.h"
#include "hash0hash.h"
#include "trx0types.h"
#include "fts0fts.h"
#include "buf0buf.h"
#include "gis0type.h"
#include "fil0fil.h"
#include "fil0crypt.h"
#include "mysql_com.h"
#include <sql_const.h>
#include <set>
#include <algorithm>
#include <iterator>
#include <ostream>
#include <mutex>

/* Forward declaration. */
struct ib_rbt_t;

/** Type flags of an index: OR'ing of the flags is allowed to define a
combination of types */
/* @{ */
#define DICT_CLUSTERED	1	/*!< clustered index; for other than
				auto-generated clustered indexes,
				also DICT_UNIQUE will be set */
#define DICT_UNIQUE	2	/*!< unique index */
#define	DICT_IBUF	8	/*!< insert buffer tree */
#define	DICT_CORRUPT	16	/*!< bit to store the corrupted flag
				in SYS_INDEXES.TYPE */
#define	DICT_FTS	32	/* FTS index; can't be combined with the
				other flags */
#define	DICT_SPATIAL	64	/* SPATIAL index; can't be combined with the
				other flags */
#define	DICT_VIRTUAL	128	/* Index on Virtual column */

#define	DICT_IT_BITS	8	/*!< number of bits used for
				SYS_INDEXES.TYPE */
/* @} */

#if 0 /* not implemented, retained for history */
/** Types for a table object */
#define DICT_TABLE_ORDINARY		1 /*!< ordinary table */
#define	DICT_TABLE_CLUSTER_MEMBER	2
#define	DICT_TABLE_CLUSTER		3 /* this means that the table is
					  really a cluster definition */
#endif

/* Table and tablespace flags are generally not used for the Antelope file
format except for the low order bit, which is used differently depending on
where the flags are stored.

==================== Low order flags bit =========================
                    | REDUNDANT | COMPACT | COMPRESSED and DYNAMIC
SYS_TABLES.TYPE     |     1     |    1    |     1
dict_table_t::flags |     0     |    1    |     1
FSP_SPACE_FLAGS     |     0     |    0    |     1
fil_space_t::flags  |     0     |    0    |     1

Before the 5.1 plugin, SYS_TABLES.TYPE was always DICT_TABLE_ORDINARY (1)
and the tablespace flags field was always 0. In the 5.1 plugin, these fields
were repurposed to identify compressed and dynamic row formats.

The following types and constants describe the flags found in dict_table_t
and SYS_TABLES.TYPE.  Similar flags found in fil_space_t and FSP_SPACE_FLAGS
are described in fsp0fsp.h. */

/* @{ */
/** dict_table_t::flags bit 0 is equal to 0 if the row format = Redundant */
#define DICT_TF_REDUNDANT		0	/*!< Redundant row format. */
/** dict_table_t::flags bit 0 is equal to 1 if the row format = Compact */
#define DICT_TF_COMPACT			1U	/*!< Compact row format. */

/** This bitmask is used in SYS_TABLES.N_COLS to set and test whether
the Compact page format is used, i.e ROW_FORMAT != REDUNDANT */
#define DICT_N_COLS_COMPACT	0x80000000UL

/** Width of the COMPACT flag */
#define DICT_TF_WIDTH_COMPACT		1

/** Width of the ZIP_SSIZE flag */
#define DICT_TF_WIDTH_ZIP_SSIZE		4

/** Width of the ATOMIC_BLOBS flag.  The ROW_FORMAT=REDUNDANT and
ROW_FORMAT=COMPACT broke up BLOB and TEXT fields, storing the first 768 bytes
in the clustered index. ROW_FORMAT=DYNAMIC and ROW_FORMAT=COMPRESSED
store the whole blob or text field off-page atomically.
Secondary indexes are created from this external data using row_ext_t
to cache the BLOB prefixes. */
#define DICT_TF_WIDTH_ATOMIC_BLOBS	1

/** If a table is created with the MYSQL option DATA DIRECTORY and
innodb-file-per-table, an older engine will not be able to find that table.
This flag prevents older engines from attempting to open the table and
allows InnoDB to update_create_info() accordingly. */
#define DICT_TF_WIDTH_DATA_DIR		1

/**
Width of the page compression flag
*/
#define DICT_TF_WIDTH_PAGE_COMPRESSION  1
#define DICT_TF_WIDTH_PAGE_COMPRESSION_LEVEL 4

/**
The NO_ROLLBACK flag (3=yes; the values 1,2 used stand for
ATOMIC_WRITES=ON and ATOMIC_WRITES=OFF between MariaDB 10.1.0 and 10.2.3)
*/
#define DICT_TF_WIDTH_NO_ROLLBACK 2

/** Width of all the currently known table flags */
#define DICT_TF_BITS	(DICT_TF_WIDTH_COMPACT			\
			+ DICT_TF_WIDTH_ZIP_SSIZE		\
			+ DICT_TF_WIDTH_ATOMIC_BLOBS		\
			+ DICT_TF_WIDTH_DATA_DIR		\
			+ DICT_TF_WIDTH_PAGE_COMPRESSION	\
			+ DICT_TF_WIDTH_PAGE_COMPRESSION_LEVEL	\
			+ DICT_TF_WIDTH_NO_ROLLBACK)

/** Zero relative shift position of the COMPACT field */
#define DICT_TF_POS_COMPACT		0
/** Zero relative shift position of the ZIP_SSIZE field */
#define DICT_TF_POS_ZIP_SSIZE		(DICT_TF_POS_COMPACT		\
					+ DICT_TF_WIDTH_COMPACT)
/** Zero relative shift position of the ATOMIC_BLOBS field */
#define DICT_TF_POS_ATOMIC_BLOBS	(DICT_TF_POS_ZIP_SSIZE		\
					+ DICT_TF_WIDTH_ZIP_SSIZE)
/** Zero relative shift position of the DATA_DIR field */
#define DICT_TF_POS_DATA_DIR		(DICT_TF_POS_ATOMIC_BLOBS	\
					+ DICT_TF_WIDTH_ATOMIC_BLOBS)
/** Zero relative shift position of the PAGE_COMPRESSION field */
#define DICT_TF_POS_PAGE_COMPRESSION	(DICT_TF_POS_DATA_DIR		\
					+ DICT_TF_WIDTH_DATA_DIR)
/** Zero relative shift position of the PAGE_COMPRESSION_LEVEL field */
#define DICT_TF_POS_PAGE_COMPRESSION_LEVEL	(DICT_TF_POS_PAGE_COMPRESSION	\
					+ DICT_TF_WIDTH_PAGE_COMPRESSION)
/** Zero relative shift position of the NO_ROLLBACK field */
#define DICT_TF_POS_NO_ROLLBACK		(DICT_TF_POS_PAGE_COMPRESSION_LEVEL \
					+ DICT_TF_WIDTH_PAGE_COMPRESSION_LEVEL)
#define DICT_TF_POS_UNUSED		(DICT_TF_POS_NO_ROLLBACK     \
					+ DICT_TF_WIDTH_NO_ROLLBACK)

/** Bit mask of the COMPACT field */
#define DICT_TF_MASK_COMPACT				\
		((~(~0U << DICT_TF_WIDTH_COMPACT))	\
		<< DICT_TF_POS_COMPACT)
/** Bit mask of the ZIP_SSIZE field */
#define DICT_TF_MASK_ZIP_SSIZE				\
		((~(~0U << DICT_TF_WIDTH_ZIP_SSIZE))	\
		<< DICT_TF_POS_ZIP_SSIZE)
/** Bit mask of the ATOMIC_BLOBS field */
#define DICT_TF_MASK_ATOMIC_BLOBS			\
		((~(~0U << DICT_TF_WIDTH_ATOMIC_BLOBS))	\
		<< DICT_TF_POS_ATOMIC_BLOBS)
/** Bit mask of the DATA_DIR field */
#define DICT_TF_MASK_DATA_DIR				\
		((~(~0U << DICT_TF_WIDTH_DATA_DIR))	\
		<< DICT_TF_POS_DATA_DIR)
/** Bit mask of the PAGE_COMPRESSION field */
#define DICT_TF_MASK_PAGE_COMPRESSION			\
		((~(~0U << DICT_TF_WIDTH_PAGE_COMPRESSION)) \
		<< DICT_TF_POS_PAGE_COMPRESSION)
/** Bit mask of the PAGE_COMPRESSION_LEVEL field */
#define DICT_TF_MASK_PAGE_COMPRESSION_LEVEL		\
		((~(~0U << DICT_TF_WIDTH_PAGE_COMPRESSION_LEVEL)) \
		<< DICT_TF_POS_PAGE_COMPRESSION_LEVEL)
/** Bit mask of the NO_ROLLBACK field */
#define DICT_TF_MASK_NO_ROLLBACK		\
		((~(~0U << DICT_TF_WIDTH_NO_ROLLBACK)) \
		<< DICT_TF_POS_NO_ROLLBACK)

/** Return the value of the COMPACT field */
#define DICT_TF_GET_COMPACT(flags)			\
		((flags & DICT_TF_MASK_COMPACT)		\
		>> DICT_TF_POS_COMPACT)
/** Return the value of the ZIP_SSIZE field */
#define DICT_TF_GET_ZIP_SSIZE(flags)			\
		((flags & DICT_TF_MASK_ZIP_SSIZE)	\
		>> DICT_TF_POS_ZIP_SSIZE)
/** Return the value of the ATOMIC_BLOBS field */
#define DICT_TF_HAS_ATOMIC_BLOBS(flags)			\
		((flags & DICT_TF_MASK_ATOMIC_BLOBS)	\
		>> DICT_TF_POS_ATOMIC_BLOBS)
/** Return the value of the DATA_DIR field */
#define DICT_TF_HAS_DATA_DIR(flags)			\
		((flags & DICT_TF_MASK_DATA_DIR)	\
		>> DICT_TF_POS_DATA_DIR)
/** Return the value of the PAGE_COMPRESSION field */
#define DICT_TF_GET_PAGE_COMPRESSION(flags)	       \
		((flags & DICT_TF_MASK_PAGE_COMPRESSION) \
		>> DICT_TF_POS_PAGE_COMPRESSION)
/** Return the value of the PAGE_COMPRESSION_LEVEL field */
#define DICT_TF_GET_PAGE_COMPRESSION_LEVEL(flags)       \
		((flags & DICT_TF_MASK_PAGE_COMPRESSION_LEVEL)	\
		>> DICT_TF_POS_PAGE_COMPRESSION_LEVEL)

/* @} */

/** @brief Table Flags set number 2.

These flags will be stored in SYS_TABLES.MIX_LEN.  All unused flags
will be written as 0.  The column may contain garbage for tables
created with old versions of InnoDB that only implemented
ROW_FORMAT=REDUNDANT.  InnoDB engines do not check these flags
for unknown bits in order to protect backward incompatibility. */
/* @{ */
/** Total number of bits in table->flags2. */
#define DICT_TF2_BITS			7
#define DICT_TF2_UNUSED_BIT_MASK	(~0U << DICT_TF2_BITS)
#define DICT_TF2_BIT_MASK		~DICT_TF2_UNUSED_BIT_MASK

/** TEMPORARY; TRUE for tables from CREATE TEMPORARY TABLE. */
#define DICT_TF2_TEMPORARY		1U

/** The table has an internal defined DOC ID column */
#define DICT_TF2_FTS_HAS_DOC_ID		2U

/** The table has an FTS index */
#define DICT_TF2_FTS			4U

/** Need to add Doc ID column for FTS index build.
This is a transient bit for index build */
#define DICT_TF2_FTS_ADD_DOC_ID		8U

/** This bit is used during table creation to indicate that it will
use its own tablespace instead of the system tablespace. */
#define DICT_TF2_USE_FILE_PER_TABLE	16U

/** Set when we discard/detach the tablespace */
#define DICT_TF2_DISCARDED		32U

/** This bit is set if all aux table names (both common tables and
index tables) of a FTS table are in HEX format. */
#define DICT_TF2_FTS_AUX_HEX_NAME	64U

/* @} */

#define DICT_TF2_FLAG_SET(table, flag)		\
	(table->flags2 |= (flag))

#define DICT_TF2_FLAG_IS_SET(table, flag)	\
	(table->flags2 & (flag))

#define DICT_TF2_FLAG_UNSET(table, flag)	\
	(table->flags2 &= ~(flag) & ((1U << DICT_TF2_BITS) - 1))

/** Tables could be chained together with Foreign key constraint. When
first load the parent table, we would load all of its descedents.
This could result in rescursive calls and out of stack error eventually.
DICT_FK_MAX_RECURSIVE_LOAD defines the maximum number of recursive loads,
when exceeded, the child table will not be loaded. It will be loaded when
the foreign constraint check needs to be run. */
#define DICT_FK_MAX_RECURSIVE_LOAD	20

/** Similarly, when tables are chained together with foreign key constraints
with on cascading delete/update clause, delete from parent table could
result in recursive cascading calls. This defines the maximum number of
such cascading deletes/updates allowed. When exceeded, the delete from
parent table will fail, and user has to drop excessive foreign constraint
before proceeds. */
#define FK_MAX_CASCADE_DEL		15

/****************************************************************/ /**
 Free a table memory object. */
void
dict_mem_table_free(
/*================*/
	dict_table_t*	table);		/*!< in: table */
/**********************************************************************//**
Adds a column definition to a table. */
void
dict_mem_table_add_col(
/*===================*/
	dict_table_t*	table,	/*!< in: table */
	mem_heap_t*	heap,	/*!< in: temporary memory heap, or NULL */
	const char*	name,	/*!< in: column name, or NULL */
	ulint		mtype,	/*!< in: main datatype */
	ulint		prtype,	/*!< in: precise type */
	ulint		len)	/*!< in: precision */
	MY_ATTRIBUTE((nonnull(1)));
/** Adds a virtual column definition to a table.
@param[in,out]	table		table
@param[in]	heap		temporary memory heap, or NULL. It is
				used to store name when we have not finished
				adding all columns. When all columns are
				added, the whole name will copy to memory from
				table->heap
@param[in]	name		column name
@param[in]	mtype		main datatype
@param[in]	prtype		precise type
@param[in]	len		length
@param[in]	pos		position in a table
@param[in]	num_base	number of base columns
@return the virtual column definition */
dict_v_col_t*
dict_mem_table_add_v_col(
	dict_table_t*	table,
	mem_heap_t*	heap,
	const char*	name,
	ulint		mtype,
	ulint		prtype,
	ulint		len,
	ulint		pos,
	ulint		num_base);

/** Adds a stored column definition to a table.
@param[in]	table		table
@param[in]	num_base	number of base columns. */
void
dict_mem_table_add_s_col(
	dict_table_t*	table,
	ulint		num_base);

/**********************************************************************//**
Renames a column of a table in the data dictionary cache. */
void
dict_mem_table_col_rename(
/*======================*/
	dict_table_t*	table,	/*!< in/out: table */
	ulint		nth_col,/*!< in: column index */
	const char*	from,	/*!< in: old column name */
	const char*	to,	/*!< in: new column name */
	bool		is_virtual);
				/*!< in: if this is a virtual column */
/**********************************************************************//**
This function populates a dict_col_t memory structure with
supplied information. */
void
dict_mem_fill_column_struct(
/*========================*/
	dict_col_t*	column,		/*!< out: column struct to be
					filled */
	ulint		col_pos,	/*!< in: column position */
	ulint		mtype,		/*!< in: main data type */
	ulint		prtype,		/*!< in: precise type */
	ulint		col_len);	/*!< in: column length */
/**********************************************************************//**
This function poplulates a dict_index_t index memory structure with
supplied information. */
UNIV_INLINE
void
dict_mem_fill_index_struct(
/*=======================*/
	dict_index_t*	index,		/*!< out: index to be filled */
	mem_heap_t*	heap,		/*!< in: memory heap */
	const char*	index_name,	/*!< in: index name */
	ulint		type,		/*!< in: DICT_UNIQUE,
					DICT_CLUSTERED, ... ORed */
	ulint		n_fields);	/*!< in: number of fields */
/**********************************************************************//**
Creates an index memory object.
@return own: index object */
dict_index_t*
dict_mem_index_create(
/*==================*/
	dict_table_t*	table,		/*!< in: table */
	const char*	index_name,	/*!< in: index name */
	ulint		type,		/*!< in: DICT_UNIQUE,
					DICT_CLUSTERED, ... ORed */
	ulint		n_fields);	/*!< in: number of fields */
/**********************************************************************//**
Adds a field definition to an index. NOTE: does not take a copy
of the column name if the field is a column. The memory occupied
by the column name may be released only after publishing the index. */
void
dict_mem_index_add_field(
/*=====================*/
	dict_index_t*	index,		/*!< in: index */
	const char*	name,		/*!< in: column name */
	ulint		prefix_len);	/*!< in: 0 or the column prefix length
					in a MySQL index like
					INDEX (textcol(25)) */
/**********************************************************************//**
Frees an index memory object. */
void
dict_mem_index_free(
/*================*/
	dict_index_t*	index);	/*!< in: index */
/**********************************************************************//**
Creates and initializes a foreign constraint memory object.
@return own: foreign constraint struct */
dict_foreign_t*
dict_mem_foreign_create(void);
/*=========================*/

/**********************************************************************//**
Sets the foreign_table_name_lookup pointer based on the value of
lower_case_table_names.  If that is 0 or 1, foreign_table_name_lookup
will point to foreign_table_name.  If 2, then another string is
allocated from the heap and set to lower case. */
void
dict_mem_foreign_table_name_lookup_set(
/*===================================*/
	dict_foreign_t*	foreign,	/*!< in/out: foreign struct */
	ibool		do_alloc);	/*!< in: is an alloc needed */

/**********************************************************************//**
Sets the referenced_table_name_lookup pointer based on the value of
lower_case_table_names.  If that is 0 or 1, referenced_table_name_lookup
will point to referenced_table_name.  If 2, then another string is
allocated from the heap and set to lower case. */
void
dict_mem_referenced_table_name_lookup_set(
/*======================================*/
	dict_foreign_t*	foreign,	/*!< in/out: foreign struct */
	ibool		do_alloc);	/*!< in: is an alloc needed */

/** Fills the dependent virtual columns in a set.
Reason for being dependent are
1) FK can be present on base column of virtual columns
2) FK can be present on column which is a part of virtual index
@param[in,out] foreign foreign key information. */
void
dict_mem_foreign_fill_vcol_set(
       dict_foreign_t*	foreign);

/** Fill virtual columns set in each fk constraint present in the table.
@param[in,out] table   innodb table object. */
void
dict_mem_table_fill_foreign_vcol_set(
        dict_table_t*	table);

/** Free the vcol_set from all foreign key constraint on the table.
@param[in,out] table   innodb table object. */
void
dict_mem_table_free_foreign_vcol_set(
	dict_table_t*	table);

/** Create a temporary tablename like "#sql-ibNNN".
@param[in]	heap	A memory heap
@param[in]	dbtab	Table name in the form database/table name
@param[in]	id	Table id
@return A unique temporary tablename suitable for InnoDB use */
char*
dict_mem_create_temporary_tablename(
	mem_heap_t*	heap,
	const char*	dbtab,
	table_id_t	id);

/** SQL identifier name wrapper for pretty-printing */
class id_name_t
{
public:
	/** Default constructor */
	id_name_t()
		: m_name()
	{}
	/** Constructor
	@param[in]	name	identifier to assign */
	explicit id_name_t(
		const char*	name)
		: m_name(name)
	{}

	/** Assignment operator
	@param[in]	name	identifier to assign */
	id_name_t& operator=(
		const char*	name)
	{
		m_name = name;
		return(*this);
	}

	/** Implicit type conversion
	@return the name */
	operator const char*() const
	{
		return(m_name);
	}

	/** Explicit type conversion
	@return the name */
	const char* operator()() const
	{
		return(m_name);
	}

private:
	/** The name in internal representation */
	const char*	m_name;
};

/** Data structure for a column in a table */
struct dict_col_t{
	/*----------------------*/
	/** The following are copied from dtype_t,
	so that all bit-fields can be packed tightly. */
	/* @{ */
	unsigned	prtype:32;	/*!< precise type; MySQL data
					type, charset code, flags to
					indicate nullability,
					signedness, whether this is a
					binary string, whether this is
					a true VARCHAR where MySQL
					uses 2 bytes to store the length */
	unsigned	mtype:8;	/*!< main data type */

	/* the remaining fields do not affect alphabetical ordering: */

	unsigned	len:16;		/*!< length; for MySQL data this
					is field->pack_length(),
					except that for a >= 5.0.3
					type true VARCHAR this is the
					maximum byte length of the
					string data (in addition to
					the string, MySQL uses 1 or 2
					bytes to store the string length) */

	unsigned	mbminlen:3;	/*!< minimum length of a
					character, in bytes */
	unsigned	mbmaxlen:3;	/*!< maximum length of a
					character, in bytes */
	/*----------------------*/
	/* End of definitions copied from dtype_t */
	/* @} */

	unsigned	ind:10;		/*!< table column position
					(starting from 0) */
	unsigned	ord_part:1;	/*!< nonzero if this column
					appears in the ordering fields
					of an index */
	unsigned	max_prefix:12;	/*!< maximum index prefix length on
					this column. Our current max limit is
					3072 (REC_VERSION_56_MAX_INDEX_COL_LEN)
					bytes. */
private:
	/** Special value of ind for a dropped column */
	static const unsigned DROPPED = 1023;
public:

  /** Detach a virtual column from an index.
  @param index  being-freed index */
  inline void detach(const dict_index_t &index);

  /** Data for instantly added columns */
  struct def_t
  {
    /** original default value of instantly added column */
    const void *data;
    /** len of data, or UNIV_SQL_DEFAULT if unavailable */
    ulint len;
  } def_val;

  /** Retrieve the column name.
  @param table  the table of this column */
  const char *name(const dict_table_t &table) const;

  /** @return whether this is a virtual column */
  bool is_virtual() const { return prtype & DATA_VIRTUAL; }
  /** @return whether NULL is an allowed value for this column */
  bool is_nullable() const { return !(prtype & DATA_NOT_NULL); }

  /** @return whether table of this system field is TRX_ID-based */
  bool vers_native() const
  {
    ut_ad(vers_sys_start() || vers_sys_end());
    ut_ad(mtype == DATA_INT || mtype == DATA_FIXBINARY);
    return mtype == DATA_INT;
  }
  /** @return whether this user column (not row_start, row_end)
  has System Versioning property */
  bool is_versioned() const { return !(~prtype & DATA_VERSIONED); }
  /** @return whether this is the system version start */
  bool vers_sys_start() const
  {
    return (prtype & DATA_VERSIONED) == DATA_VERS_START;
  }
  /** @return whether this is the system version end */
  bool vers_sys_end() const
  {
    return (prtype & DATA_VERSIONED) == DATA_VERS_END;
  }

  /** @return whether this is an instantly-added column */
  bool is_added() const
  {
    DBUG_ASSERT(def_val.len != UNIV_SQL_DEFAULT || !def_val.data);
    return def_val.len != UNIV_SQL_DEFAULT;
  }
  /** Flag the column instantly dropped */
  void set_dropped() { ind = DROPPED; }
  /** Flag the column instantly dropped.
  @param not_null  whether the column was NOT NULL
  @param len2      whether the length exceeds 255 bytes
  @param fixed_len the fixed length in bytes, or 0 */
  void set_dropped(bool not_null, bool len2, unsigned fixed)
  {
    DBUG_ASSERT(!len2 || !fixed);
    prtype= not_null ? DATA_NOT_NULL | DATA_BINARY_TYPE : DATA_BINARY_TYPE;
    if (fixed)
    {
      mtype= DATA_FIXBINARY;
      len= static_cast<uint16_t>(fixed);
    }
    else
    {
      mtype= DATA_BINARY;
      len= len2 ? 65535 : 255;
    }
    mbminlen= mbmaxlen= 0;
    ind= DROPPED;
    ord_part= 0;
    max_prefix= 0;
  }
  /** @return whether the column was instantly dropped */
  bool is_dropped() const { return ind == DROPPED; }
  /** @return whether the column was instantly dropped
  @param index  the clustered index */
  inline bool is_dropped(const dict_index_t &index) const;

  /** Get the default value of an instantly-added column.
  @param[out] len   value length (in bytes), or UNIV_SQL_NULL
  @return default value
  @retval NULL if the default value is SQL NULL (len=UNIV_SQL_NULL) */
  const byte *instant_value(ulint *len) const
  {
    DBUG_ASSERT(is_added());
    *len= def_val.len;
    return static_cast<const byte*>(def_val.data);
  }

  /** Remove the 'instant ADD' status of the column */
  void clear_instant()
  {
    def_val.len= UNIV_SQL_DEFAULT;
    def_val.data= NULL;
  }

  /** @return whether two columns have compatible data type encoding */
  bool same_type(const dict_col_t &other) const
  {
    if (mtype != other.mtype)
    {
      /* For latin1_swedish_ci, DATA_CHAR and DATA_VARCHAR
      will be used instead of DATA_MYSQL and DATA_VARMYSQL.
      As long as mtype,prtype are being written to InnoDB
      data dictionary tables, we cannot simplify this. */
      switch (mtype) {
      default:
        return false;
      case DATA_VARCHAR:
        if (other.mtype != DATA_VARMYSQL)
          return false;
        goto check_encoding;
      case DATA_VARMYSQL:
        if (other.mtype != DATA_VARCHAR)
          return false;
        goto check_encoding;
      case DATA_CHAR:
        if (other.mtype != DATA_MYSQL)
          return false;
        goto check_encoding;
      case DATA_MYSQL:
        if (other.mtype != DATA_CHAR)
          return false;
        goto check_encoding;
      }
    }
    else if (dtype_is_string_type(mtype))
    {
    check_encoding:
      const uint16_t cset= dtype_get_charset_coll(prtype);
      const uint16_t ocset= dtype_get_charset_coll(other.prtype);
      return cset == ocset || dict_col_t::same_encoding(cset, ocset);
    }

    return true;
  }

  /** @return whether two collations codes have the same character encoding */
  static bool same_encoding(uint16_t a, uint16_t b);

  /** Determine if the columns have the same format
  except for is_nullable() and is_versioned().
  @param other   column to compare to
  @return whether the columns have the same format */
  bool same_format(const dict_col_t &other) const
  {
    return same_type(other) && len >= other.len &&
      mbminlen == other.mbminlen && mbmaxlen >= other.mbmaxlen &&
      !((prtype ^ other.prtype) & ~(DATA_NOT_NULL | DATA_VERSIONED |
                                    CHAR_COLL_MASK << 16 |
                                    DATA_LONG_TRUE_VARCHAR));
  }

  /** @return whether the column values are comparable by memcmp() */
  bool is_binary() const { return prtype & DATA_BINARY_TYPE; }
};

/** Index information put in a list of virtual column structure. Index
id and virtual column position in the index will be logged.
There can be multiple entries for a given index, with a different position. */
struct dict_v_idx_t {
	/** active index on the column */
	dict_index_t*	index;

	/** position in this index */
	ulint		nth_field;

	dict_v_idx_t(dict_index_t* index, ulint nth_field)
		: index(index), nth_field(nth_field) {}
};

/** Data structure for a virtual column in a table */
struct dict_v_col_t{
	/** column structure */
	dict_col_t		m_col;

	/** array of base column ptr */
	dict_col_t**		base_col;

	/** number of base column */
	unsigned		num_base:10;

	/** column pos in table */
	unsigned		v_pos:10;

	/** Virtual index list, and column position in the index */
	std::forward_list<dict_v_idx_t, ut_allocator<dict_v_idx_t> >
	v_indexes;

  /** Detach the column from an index.
  @param index  index to be detached from */
  void detach(const dict_index_t &index)
  {
    if (v_indexes.empty()) return;
    auto i= v_indexes.before_begin();
    do {
      auto prev = i++;
      if (i == v_indexes.end())
      {
        return;
      }
      if (i->index == &index)
      {
        v_indexes.erase_after(prev);
        return;
      }
    }
    while (i != v_indexes.end());
  }
};

/** Data structure for newly added virtual column in a index.
It is used only during rollback_inplace_alter_table() of
addition of index depending on newly added virtual columns
and uses index heap. Should be freed when index is being
removed from cache. */
struct dict_add_v_col_info
{
  ulint n_v_col;
  dict_v_col_t *v_col;

  /** Add the newly added virtual column while rollbacking
  the index which contains new virtual columns
  @param col    virtual column to be duplicated
  @param offset offset where to duplicate virtual column */
  dict_v_col_t* add_drop_v_col(mem_heap_t *heap, dict_v_col_t *col,
                               ulint offset)
  {
    ut_ad(n_v_col);
    ut_ad(offset < n_v_col);
    if (!v_col)
      v_col= static_cast<dict_v_col_t*>
        (mem_heap_alloc(heap, n_v_col * sizeof *v_col));
    new (&v_col[offset]) dict_v_col_t();
    v_col[offset].m_col= col->m_col;
    v_col[offset].v_pos= col->v_pos;
    return &v_col[offset];
  }
};

/** Data structure for newly added virtual column in a table */
struct dict_add_v_col_t{
	/** number of new virtual column */
	ulint			n_v_col;

	/** column structures */
	const dict_v_col_t*	v_col;

	/** new col names */
	const char**		v_col_name;
};

/** Data structure for a stored column in a table. */
struct dict_s_col_t {
	/** Stored column ptr */
	dict_col_t*	m_col;
	/** array of base col ptr */
	dict_col_t**	base_col;
	/** number of base columns */
	ulint		num_base;
	/** column pos in table */
	ulint		s_pos;
};

/** list to put stored column for create_table_info_t */
typedef std::forward_list<dict_s_col_t, ut_allocator<dict_s_col_t> >
dict_s_col_list;

/** @brief DICT_ANTELOPE_MAX_INDEX_COL_LEN is measured in bytes and
is the maximum indexed column length (or indexed prefix length) in
ROW_FORMAT=REDUNDANT and ROW_FORMAT=COMPACT. Also, in any format,
any fixed-length field that is longer than this will be encoded as
a variable-length field.

It is set to 3*256, so that one can create a column prefix index on
256 characters of a TEXT or VARCHAR column also in the UTF-8
charset. In that charset, a character may take at most 3 bytes.  This
constant MUST NOT BE CHANGED, or the compatibility of InnoDB data
files would be at risk! */
#define DICT_ANTELOPE_MAX_INDEX_COL_LEN	REC_ANTELOPE_MAX_INDEX_COL_LEN

/** Find out maximum indexed column length by its table format.
For ROW_FORMAT=REDUNDANT and ROW_FORMAT=COMPACT, the maximum
field length is REC_ANTELOPE_MAX_INDEX_COL_LEN - 1 (767). For
ROW_FORMAT=COMPRESSED and ROW_FORMAT=DYNAMIC, the length could
be REC_VERSION_56_MAX_INDEX_COL_LEN (3072) bytes */
#define DICT_MAX_FIELD_LEN_BY_FORMAT(table)	\
	(dict_table_has_atomic_blobs(table)	\
	 ? REC_VERSION_56_MAX_INDEX_COL_LEN	\
	 : REC_ANTELOPE_MAX_INDEX_COL_LEN - 1)

#define DICT_MAX_FIELD_LEN_BY_FORMAT_FLAG(flags)	\
	(DICT_TF_HAS_ATOMIC_BLOBS(flags)		\
	 ? REC_VERSION_56_MAX_INDEX_COL_LEN		\
	 : REC_ANTELOPE_MAX_INDEX_COL_LEN - 1)

/** Defines the maximum fixed length column size */
#define DICT_MAX_FIXED_COL_LEN		DICT_ANTELOPE_MAX_INDEX_COL_LEN

#ifdef WITH_WSREP
#define WSREP_MAX_SUPPORTED_KEY_LENGTH 3500
#endif /* WITH_WSREP */

/** Data structure for a field in an index */
struct dict_field_t{
	dict_col_t*	col;		/*!< pointer to the table column */
	id_name_t	name;		/*!< name of the column */
	unsigned	prefix_len:12;	/*!< 0 or the length of the column
					prefix in bytes in a MySQL index of
					type, e.g., INDEX (textcol(25));
					must be smaller than
					DICT_MAX_FIELD_LEN_BY_FORMAT;
					NOTE that in the UTF-8 charset, MySQL
					sets this to (mbmaxlen * the prefix len)
					in UTF-8 chars */
	unsigned	fixed_len:10;	/*!< 0 or the fixed length of the
					column if smaller than
					DICT_ANTELOPE_MAX_INDEX_COL_LEN */

	/** Zero-initialize all fields */
	dict_field_t() : col(NULL), name(NULL), prefix_len(0), fixed_len(0) {}

	/** Check whether two index fields are equivalent.
	@param[in]	old	the other index field
	@return	whether the index fields are equivalent */
	bool same(const dict_field_t& other) const
	{
		return(prefix_len == other.prefix_len
		       && fixed_len == other.fixed_len);
	}
};

/**********************************************************************//**
PADDING HEURISTIC BASED ON LINEAR INCREASE OF PADDING TO AVOID
COMPRESSION FAILURES
(Note: this is relevant only for compressed indexes)
GOAL: Avoid compression failures by maintaining information about the
compressibility of data. If data is not very compressible then leave
some extra space 'padding' in the uncompressed page making it more
likely that compression of less than fully packed uncompressed page will
succeed.

This padding heuristic works by increasing the pad linearly until the
desired failure rate is reached. A "round" is a fixed number of
compression operations.
After each round, the compression failure rate for that round is
computed. If the failure rate is too high, then padding is incremented
by a fixed value, otherwise it's left intact.
If the compression failure is lower than the desired rate for a fixed
number of consecutive rounds, then the padding is decreased by a fixed
value. This is done to prevent overshooting the padding value,
and to accommodate the possible change in data compressibility. */

/** Number of zip ops in one round. */
#define ZIP_PAD_ROUND_LEN			(128)

/** Number of successful rounds after which the padding is decreased */
#define ZIP_PAD_SUCCESSFUL_ROUND_LIMIT		(5)

/** Amount by which padding is increased. */
#define ZIP_PAD_INCR				(128)

/** Percentage of compression failures that are allowed in a single
round */
extern ulong	zip_failure_threshold_pct;

/** Maximum percentage of a page that can be allowed as a pad to avoid
compression failures */
extern ulong	zip_pad_max;

/** Data structure to hold information about about how much space in
an uncompressed page should be left as padding to avoid compression
failures. This estimate is based on a self-adapting heuristic. */
struct zip_pad_info_t {
  /** Dummy assignment operator for dict_index_t::clone() */
  zip_pad_info_t &operator=(const zip_pad_info_t&) { return *this; }
	std::mutex	mutex;	/*!< mutex protecting the info */
	Atomic_relaxed<ulint>
			pad;	/*!< number of bytes used as pad */
	ulint		success;/*!< successful compression ops during
				current round */
	ulint		failure;/*!< failed compression ops during
				current round */
	ulint		n_rounds;/*!< number of currently successful
				rounds */
};

/** Number of samples of data size kept when page compression fails for
a certain index.*/
#define STAT_DEFRAG_DATA_SIZE_N_SAMPLE	10

/** "GEN_CLUST_INDEX" is the name reserved for InnoDB default
system clustered index when there is no primary key. */
const char innobase_index_reserve_name[] = "GEN_CLUST_INDEX";

/** Data structure for an index.  Most fields will be
initialized to 0, NULL or FALSE in dict_mem_index_create(). */
struct dict_index_t {
  /** Maximum number of fields */
  static constexpr unsigned MAX_N_FIELDS= (1U << 10) - 1;

	index_id_t	id;	/*!< id of the index */
	mem_heap_t*	heap;	/*!< memory heap */
	id_name_t	name;	/*!< index name */
	dict_table_t*	table;	/*!< back pointer to table */
	/** root page number, or FIL_NULL if the index has been detached
	from storage (DISCARD TABLESPACE or similar),
	or 1 if the index is in table->freed_indexes */
	unsigned	page:32;
	unsigned	merge_threshold:6;
				/*!< In the pessimistic delete, if the page
				data size drops below this limit in percent,
				merging it to a neighbor is tried */
# define DICT_INDEX_MERGE_THRESHOLD_DEFAULT 50
	unsigned	type:DICT_IT_BITS;
				/*!< index type (DICT_CLUSTERED, DICT_UNIQUE,
				DICT_IBUF, DICT_CORRUPT) */
#define MAX_KEY_LENGTH_BITS 12
	unsigned	trx_id_offset:MAX_KEY_LENGTH_BITS;
				/*!< position of the trx id column
				in a clustered index record, if the fields
				before it are known to be of a fixed size,
				0 otherwise */
#if (1<<MAX_KEY_LENGTH_BITS) < HA_MAX_KEY_LENGTH
# error (1<<MAX_KEY_LENGTH_BITS) < HA_MAX_KEY_LENGTH
#endif
	unsigned	n_user_defined_cols:10;
				/*!< number of columns the user defined to
				be in the index: in the internal
				representation we add more columns */
	unsigned	nulls_equal:1;
				/*!< if true, SQL NULL == SQL NULL */
	unsigned	n_uniq:10;/*!< number of fields from the beginning
				which are enough to determine an index
				entry uniquely */
	unsigned	n_def:10;/*!< number of fields defined so far */
	unsigned	n_fields:10;/*!< number of fields in the index */
	unsigned	n_nullable:10;/*!< number of nullable fields */
	unsigned	n_core_fields:10;/*!< number of fields in the index
				(before the first time of instant add columns) */
	/** number of bytes of null bits in ROW_FORMAT!=REDUNDANT node pointer
	records; usually equal to UT_BITS_IN_BYTES(n_nullable), but
	can be less in clustered indexes with instant ADD COLUMN */
	unsigned	n_core_null_bytes:8;
	/** magic value signalling that n_core_null_bytes was not
	initialized yet */
	static const unsigned NO_CORE_NULL_BYTES = 0xff;
	/** The clustered index ID of the hard-coded SYS_INDEXES table. */
	static const unsigned DICT_INDEXES_ID = 3;
	unsigned	cached:1;/*!< TRUE if the index object is in the
				dictionary cache */
	unsigned	to_be_dropped:1;
				/*!< TRUE if the index is to be dropped;
				protected by dict_sys.latch */
	unsigned	online_status:2;
				/*!< enum online_index_status.
				Transitions from ONLINE_INDEX_COMPLETE (to
				ONLINE_INDEX_CREATION) are protected
				by dict_sys.latch. Other changes are
				protected by index->lock. */
	unsigned	uncommitted:1;
				/*!< a flag that is set for secondary indexes
				that have not been committed to the
				data dictionary yet */

#ifdef UNIV_DEBUG
	/** whether this is a dummy index object */
	bool		is_dummy;
	/** whether btr_cur_instant_init() is in progress */
	bool		in_instant_init;
	uint32_t	magic_n;/*!< magic number */
/** Value of dict_index_t::magic_n */
# define DICT_INDEX_MAGIC_N	76789786
#endif
	dict_field_t*	fields;	/*!< array of field descriptions */
	st_mysql_ftparser*
			parser;	/*!< fulltext parser plugin */

	/** It just indicates whether newly added virtual column
	during alter. It stores column in case of alter failure.
	It should use heap from dict_index_t. It should be freed
	while removing the index from table. */
	dict_add_v_col_info* new_vcol_info;
	UT_LIST_NODE_T(dict_index_t)
			indexes;/*!< list of indexes of the table */
#ifdef BTR_CUR_ADAPT
	btr_search_t*	search_info;
				/*!< info used in optimistic searches */
#endif /* BTR_CUR_ADAPT */
	row_log_t*	online_log;
				/*!< the log of modifications
				during online index creation;
				valid when online_status is
				ONLINE_INDEX_CREATION */
	/*----------------------*/
	/** Statistics for query optimization */
	/* @{ */
	ib_uint64_t*	stat_n_diff_key_vals;
				/*!< approximate number of different
				key values for this index, for each
				n-column prefix where 1 <= n <=
				dict_get_n_unique(index) (the array is
				indexed from 0 to n_uniq-1); we
				periodically calculate new
				estimates */
	ib_uint64_t*	stat_n_sample_sizes;
				/*!< number of pages that were sampled
				to calculate each of stat_n_diff_key_vals[],
				e.g. stat_n_sample_sizes[3] pages were sampled
				to get the number stat_n_diff_key_vals[3]. */
	ib_uint64_t*	stat_n_non_null_key_vals;
				/* approximate number of non-null key values
				for this index, for each column where
				1 <= n <= dict_get_n_unique(index) (the array
				is indexed from 0 to n_uniq-1); This
				is used when innodb_stats_method is
				"nulls_ignored". */
	ulint		stat_index_size;
				/*!< approximate index size in
				database pages */
	ulint		stat_n_leaf_pages;
				/*!< approximate number of leaf pages in the
				index tree */
	bool		stats_error_printed;
				/*!< has persistent statistics error printed
				for this index ? */
	/* @} */
	/** Statistics for defragmentation, these numbers are estimations and
	could be very inaccurate at certain times, e.g. right after restart,
	during defragmentation, etc. */
	/* @{ */
	ulint		stat_defrag_modified_counter;
	ulint		stat_defrag_n_pages_freed;
				/* number of pages freed by defragmentation. */
	ulint		stat_defrag_n_page_split;
				/* number of page splits since last full index
				defragmentation. */
	ulint		stat_defrag_data_size_sample[STAT_DEFRAG_DATA_SIZE_N_SAMPLE];
				/* data size when compression failure happened
				the most recent 10 times. */
	ulint		stat_defrag_sample_next_slot;
				/* in which slot the next sample should be
				saved. */
	/* @} */
private:
  /** R-tree split sequence number */
  Atomic_relaxed<node_seq_t> rtr_ssn;
public:
  void set_ssn(node_seq_t ssn) { rtr_ssn= ssn; }
  node_seq_t assign_ssn() { return rtr_ssn.fetch_add(1) + 1; }
  node_seq_t ssn() const { return rtr_ssn; }

	rtr_info_track_t*
			rtr_track;/*!< tracking all R-Tree search cursors */
	trx_id_t	trx_id; /*!< id of the transaction that created this
				index, or 0 if the index existed
				when InnoDB was started up */
	zip_pad_info_t	zip_pad;/*!< Information about state of
				compression failures and successes */
  /** lock protecting the non-leaf index pages */
  mutable index_lock lock;

	/** Determine if the index has been committed to the
	data dictionary.
	@return whether the index definition has been committed */
	bool is_committed() const
	{
		ut_ad(!uncommitted || !(type & DICT_CLUSTERED));
		return(UNIV_LIKELY(!uncommitted));
	}

	/** Flag an index committed or uncommitted.
	@param[in]	committed	whether the index is committed */
	void set_committed(bool committed)
	{
		ut_ad(!to_be_dropped);
		ut_ad(committed || !(type & DICT_CLUSTERED));
		uncommitted = !committed;
	}

	/** Notify that the index pages are going to be modified.
	@param[in,out]	mtr	mini-transaction */
	inline void set_modified(mtr_t& mtr) const;

	/** @return whether this index is readable
	@retval	true	normally
	@retval	false	if this is a single-table tablespace
			and the .ibd file is missing, or a
			page cannot be read or decrypted */
	inline bool is_readable() const;

	/** @return whether instant ALTER TABLE is in effect */
	inline bool is_instant() const;

	/** @return whether the index is the primary key index
	(not the clustered index of the change buffer) */
	bool is_primary() const
	{
		return DICT_CLUSTERED == (type & (DICT_CLUSTERED | DICT_IBUF));
	}

	/** @return whether this is a generated clustered index */
	bool is_gen_clust() const { return type == DICT_CLUSTERED; }

	/** @return whether this is a clustered index */
	bool is_clust() const { return type & DICT_CLUSTERED; }

	/** @return whether this is a unique index */
	bool is_unique() const { return type & DICT_UNIQUE; }

	/** @return whether this is a spatial index */
	bool is_spatial() const { return UNIV_UNLIKELY(type & DICT_SPATIAL); }

	/** @return whether this is the change buffer */
	bool is_ibuf() const { return UNIV_UNLIKELY(type & DICT_IBUF); }

	/** @return whether the index includes virtual columns */
	bool has_virtual() const { return type & DICT_VIRTUAL; }

	/** @return the position of DB_TRX_ID */
	uint16_t db_trx_id() const {
		DBUG_ASSERT(is_primary());
		DBUG_ASSERT(n_uniq);
		DBUG_ASSERT(n_uniq <= MAX_REF_PARTS);
		return n_uniq;
	}
	/** @return the position of DB_ROLL_PTR */
	uint16_t db_roll_ptr() const
	{
		return static_cast<uint16_t>(db_trx_id() + 1);
	}

	/** @return the offset of the metadata BLOB field,
	or the first user field after the PRIMARY KEY,DB_TRX_ID,DB_ROLL_PTR */
	uint16_t first_user_field() const
	{
		return static_cast<uint16_t>(db_trx_id() + 2);
	}

	/** @return whether the index is corrupted */
	inline bool is_corrupted() const;

  /** Detach the virtual columns from the index that is to be removed. */
  void detach_columns()
  {
    if (!has_virtual() || !cached)
      return;
    for (unsigned i= 0; i < n_fields; i++)
    {
      dict_col_t* col= fields[i].col;
      if (!col || !col->is_virtual())
        continue;
      col->detach(*this);
    }
  }

	/** Determine how many fields of a given prefix can be set NULL.
	@param[in]	n_prefix	number of fields in the prefix
	@return	number of fields 0..n_prefix-1 that can be set NULL */
	unsigned get_n_nullable(ulint n_prefix) const
	{
		DBUG_ASSERT(n_prefix > 0);
		DBUG_ASSERT(n_prefix <= n_fields);
		unsigned n = n_nullable;
		for (; n_prefix < n_fields; n_prefix++) {
			const dict_col_t* col = fields[n_prefix].col;
			DBUG_ASSERT(!col->is_virtual());
			n -= col->is_nullable();
		}
		DBUG_ASSERT(n < n_def);
		return n;
	}

	/** Get the default value of an instantly-added clustered index field.
	@param[in]	n	instantly added field position
	@param[out]	len	value length (in bytes), or UNIV_SQL_NULL
	@return	default value
	@retval	NULL	if the default value is SQL NULL (len=UNIV_SQL_NULL) */
	const byte* instant_field_value(ulint n, ulint* len) const
	{
		DBUG_ASSERT(is_instant() || id == DICT_INDEXES_ID);
		DBUG_ASSERT(n + (id == DICT_INDEXES_ID) >= n_core_fields);
		DBUG_ASSERT(n < n_fields);
		return fields[n].col->instant_value(len);
	}

	/** Adjust index metadata for instant ADD/DROP/reorder COLUMN.
	@param[in]	clustered index definition after instant ALTER TABLE */
	inline void instant_add_field(const dict_index_t& instant);
	/** Remove instant ADD COLUMN metadata. */
	inline void clear_instant_add();
	/** Remove instant ALTER TABLE metadata. */
	inline void clear_instant_alter();

	/** Construct the metadata record for instant ALTER TABLE.
	@param[in]	row	dummy or default values for existing columns
	@param[in,out]	heap	memory heap for allocations
	@return	metadata record */
	inline dtuple_t*
	instant_metadata(const dtuple_t& row, mem_heap_t* heap) const;

	/** Check if record in clustered index is historical row.
	@param[in]	rec	clustered row
	@param[in]	offsets	offsets
	@return true if row is historical */
	bool
	vers_history_row(const rec_t* rec, const rec_offs* offsets);

	/** Check if record in secondary index is historical row.
	@param[in]	rec	record in a secondary index
	@param[out]	history_row true if row is historical
	@return true on error */
	bool
	vers_history_row(const rec_t* rec, bool &history_row);

  /** Assign the number of new column to be added as a part
  of the index
  @param        n_vcol  number of virtual columns to be added */
  void assign_new_v_col(ulint n_vcol)
  {
    new_vcol_info= static_cast<dict_add_v_col_info*>
      (mem_heap_zalloc(heap, sizeof *new_vcol_info));
    new_vcol_info->n_v_col= n_vcol;
  }

  /* @return whether index has new virtual column */
  bool has_new_v_col() const { return new_vcol_info; }

  /* @return number of newly added virtual column */
  ulint get_new_n_vcol() const
  { return new_vcol_info ? new_vcol_info->n_v_col : 0; }

  /** Reconstruct the clustered index fields. */
  inline void reconstruct_fields();

  /** Check if the index contains a column or a prefix of that column.
  @param[in]	n		column number
  @param[in]	is_virtual	whether it is a virtual col
  @return whether the index contains the column or its prefix */
  bool contains_col_or_prefix(ulint n, bool is_virtual) const
  MY_ATTRIBUTE((warn_unused_result));

#ifdef BTR_CUR_HASH_ADAPT
  /** @return a clone of this */
  dict_index_t* clone() const;
  /** Clone this index for lazy dropping of the adaptive hash index.
  @return this or a clone */
  dict_index_t* clone_if_needed();
  /** @return number of leaf pages pointed to by the adaptive hash index */
  inline ulint n_ahi_pages() const;
  /** @return whether mark_freed() had been invoked */
  bool freed() const { return UNIV_UNLIKELY(page == 1); }
  /** Note that the index is waiting for btr_search_lazy_free() */
  void set_freed() { ut_ad(!freed()); page= 1; }
#endif /* BTR_CUR_HASH_ADAPT */

  /** @return whether it is forbidden to invoke clear_instant_add() */
  bool must_avoid_clear_instant_add() const
  {
    if (is_instant())
      for (auto i= this; (i= UT_LIST_GET_NEXT(indexes, i)) != nullptr; )
        if (i->to_be_dropped /* || i->online_log*/)
          return true;
    return false;
  }

	/** This ad-hoc class is used by record_size_info only.	*/
	class record_size_info_t {
	public:
		record_size_info_t()
		    : max_leaf_size(0), shortest_size(0), too_big(false),
		      first_overrun_field_index(SIZE_T_MAX), overrun_size(0)
		{
		}

		/** Mark row potentially too big for page and set up first
		overflow field index. */
		void set_too_big(size_t field_index)
		{
			ut_ad(field_index != SIZE_T_MAX);

			too_big = true;
			if (first_overrun_field_index > field_index) {
				first_overrun_field_index = field_index;
				overrun_size = shortest_size;
			}
		}

		/** @return overrun field index or SIZE_T_MAX if nothing
		overflowed*/
		size_t get_first_overrun_field_index() const
		{
			ut_ad(row_is_too_big());
			ut_ad(first_overrun_field_index != SIZE_T_MAX);
			return first_overrun_field_index;
		}

		size_t get_overrun_size() const
		{
			ut_ad(row_is_too_big());
			return overrun_size;
		}

		bool row_is_too_big() const { return too_big; }

		size_t max_leaf_size; /** Bigger row size this index can
				      produce */
		size_t shortest_size; /** shortest because it counts everything
				      as in overflow pages */

	private:
		bool too_big; /** This one is true when maximum row size this
			      index can produce is bigger than maximum row
			      size given page can hold. */
		size_t first_overrun_field_index; /** After adding this field
						  index row overflowed maximum
						  allowed size. Useful for
						  reporting back to user. */
		size_t overrun_size;		  /** Just overrun row size */
	};

	/** Returns max possibly record size for that index, size of a shortest
	everything in overflow) size of the longest possible row and index
	of a field which made index records too big to fit on a page.*/
	inline record_size_info_t record_size_info() const;

  /** Clear the index tree and reinitialize the root page, in the
  rollback of TRX_UNDO_EMPTY. The BTR_SEG_LEAF is freed and reinitialized.
  @param thr query thread */
  void clear(que_thr_t *thr);
};

/** Detach a virtual column from an index.
@param index  being-freed index */
inline void dict_col_t::detach(const dict_index_t &index)
{
  if (is_virtual())
    reinterpret_cast<dict_v_col_t*>(this)->detach(index);
}

/** The status of online index creation */
enum online_index_status {
	/** the index is complete and ready for access */
	ONLINE_INDEX_COMPLETE = 0,
	/** the index is being created, online
	(allowing concurrent modifications) */
	ONLINE_INDEX_CREATION,
	/** secondary index creation was aborted and the index
	should be dropped as soon as index->table->n_ref_count reaches 0,
	or online table rebuild was aborted and the clustered index
	of the original table should soon be restored to
	ONLINE_INDEX_COMPLETE */
	ONLINE_INDEX_ABORTED,
	/** the online index creation was aborted, the index was
	dropped from the data dictionary and the tablespace, and it
	should be dropped from the data dictionary cache as soon as
	index->table->n_ref_count reaches 0. */
	ONLINE_INDEX_ABORTED_DROPPED
};

/** Set to store the virtual columns which are affected by Foreign
key constraint. */
typedef std::set<dict_v_col_t*, std::less<dict_v_col_t*>,
		ut_allocator<dict_v_col_t*> >		dict_vcol_set;

/** Data structure for a foreign key constraint; an example:
FOREIGN KEY (A, B) REFERENCES TABLE2 (C, D).  Most fields will be
initialized to 0, NULL or FALSE in dict_mem_foreign_create(). */
struct dict_foreign_t{
	mem_heap_t*	heap;		/*!< this object is allocated from
					this memory heap */
	char*		id;		/*!< id of the constraint as a
					null-terminated string */
	unsigned	n_fields:10;	/*!< number of indexes' first fields
					for which the foreign key
					constraint is defined: we allow the
					indexes to contain more fields than
					mentioned in the constraint, as long
					as the first fields are as mentioned */
	unsigned	type:6;		/*!< 0 or DICT_FOREIGN_ON_DELETE_CASCADE
					or DICT_FOREIGN_ON_DELETE_SET_NULL */
	char*		foreign_table_name;/*!< foreign table name */
	char*		foreign_table_name_lookup;
				/*!< foreign table name used for dict lookup */
	dict_table_t*	foreign_table;	/*!< table where the foreign key is */
	const char**	foreign_col_names;/*!< names of the columns in the
					foreign key */
	char*		referenced_table_name;/*!< referenced table name */
	char*		referenced_table_name_lookup;
				/*!< referenced table name for dict lookup*/
	dict_table_t*	referenced_table;/*!< table where the referenced key
					is */
	const char**	referenced_col_names;/*!< names of the referenced
					columns in the referenced table */
	dict_index_t*	foreign_index;	/*!< foreign index; we require that
					both tables contain explicitly defined
					indexes for the constraint: InnoDB
					does not generate new indexes
					implicitly */
	dict_index_t*	referenced_index;/*!< referenced index */

	dict_vcol_set*	v_cols;		/*!< set of virtual columns affected
					by foreign key constraint. */

	/** Check whether the fulltext index gets affected by
	foreign key constraint */
	bool affects_fulltext() const;
};

std::ostream&
operator<< (std::ostream& out, const dict_foreign_t& foreign);

struct dict_foreign_print {

	dict_foreign_print(std::ostream& out)
		: m_out(out)
	{}

	void operator()(const dict_foreign_t* foreign) {
		m_out << *foreign;
	}
private:
	std::ostream&	m_out;
};

/** Compare two dict_foreign_t objects using their ids. Used in the ordering
of dict_table_t::foreign_set and dict_table_t::referenced_set.  It returns
true if the first argument is considered to go before the second in the
strict weak ordering it defines, and false otherwise. */
struct dict_foreign_compare {

	bool operator()(
		const dict_foreign_t*	lhs,
		const dict_foreign_t*	rhs) const
	{
		return strcmp(lhs->id, rhs->id) < 0;
	}
};

/** A function object to find a foreign key with the given index as the
referenced index. Return the foreign key with matching criteria or NULL */
struct dict_foreign_with_index {

	dict_foreign_with_index(const dict_index_t*	index)
	: m_index(index)
	{}

	bool operator()(const dict_foreign_t*	foreign) const
	{
		return(foreign->referenced_index == m_index);
	}

	const dict_index_t*	m_index;
};

/* A function object to check if the foreign constraint is between different
tables.  Returns true if foreign key constraint is between different tables,
false otherwise. */
struct dict_foreign_different_tables {

	bool operator()(const dict_foreign_t*	foreign) const
	{
		return(foreign->foreign_table != foreign->referenced_table);
	}
};

/** A function object to check if the foreign key constraint has the same
name as given.  If the full name of the foreign key constraint doesn't match,
then, check if removing the database name from the foreign key constraint
matches. Return true if it matches, false otherwise. */
struct dict_foreign_matches_id {

	dict_foreign_matches_id(const char* id)
		: m_id(id)
	{}

	bool operator()(const dict_foreign_t*	foreign) const
	{
		if (0 == innobase_strcasecmp(foreign->id, m_id)) {
			return(true);
		}
		if (const char* pos = strchr(foreign->id, '/')) {
			if (0 == innobase_strcasecmp(m_id, pos + 1)) {
				return(true);
			}
		}
		return(false);
	}

	const char*	m_id;
};

typedef std::set<
	dict_foreign_t*,
	dict_foreign_compare,
	ut_allocator<dict_foreign_t*> >	dict_foreign_set;

std::ostream&
operator<< (std::ostream& out, const dict_foreign_set& fk_set);

/** Function object to check if a foreign key object is there
in the given foreign key set or not.  It returns true if the
foreign key is not found, false otherwise */
struct dict_foreign_not_exists {
	dict_foreign_not_exists(const dict_foreign_set& obj_)
		: m_foreigns(obj_)
	{}

	/* Return true if the given foreign key is not found */
	bool operator()(dict_foreign_t* const & foreign) const {
		return(m_foreigns.find(foreign) == m_foreigns.end());
	}
private:
	const dict_foreign_set&	m_foreigns;
};

/** Validate the search order in the foreign key set.
@param[in]	fk_set	the foreign key set to be validated
@return true if search order is fine in the set, false otherwise. */
bool
dict_foreign_set_validate(
	const dict_foreign_set&	fk_set);

/** Validate the search order in the foreign key sets of the table
(foreign_set and referenced_set).
@param[in]	table	table whose foreign key sets are to be validated
@return true if foreign key sets are fine, false otherwise. */
bool
dict_foreign_set_validate(
	const dict_table_t&	table);

/*********************************************************************//**
Frees a foreign key struct. */
inline
void
dict_foreign_free(
/*==============*/
	dict_foreign_t*	foreign)	/*!< in, own: foreign key struct */
{
	if (foreign->v_cols != NULL) {
		UT_DELETE(foreign->v_cols);
	}

	mem_heap_free(foreign->heap);
}

/** The destructor will free all the foreign key constraints in the set
by calling dict_foreign_free() on each of the foreign key constraints.
This is used to free the allocated memory when a local set goes out
of scope. */
struct dict_foreign_set_free {

	dict_foreign_set_free(const dict_foreign_set&	foreign_set)
		: m_foreign_set(foreign_set)
	{}

	~dict_foreign_set_free()
	{
		std::for_each(m_foreign_set.begin(),
			      m_foreign_set.end(),
			      dict_foreign_free);
	}

	const dict_foreign_set&	m_foreign_set;
};

/** The flags for ON_UPDATE and ON_DELETE can be ORed; the default is that
a foreign key constraint is enforced, therefore RESTRICT just means no flag */
/* @{ */
#define DICT_FOREIGN_ON_DELETE_CASCADE	1U	/*!< ON DELETE CASCADE */
#define DICT_FOREIGN_ON_DELETE_SET_NULL	2U	/*!< ON UPDATE SET NULL */
#define DICT_FOREIGN_ON_UPDATE_CASCADE	4U	/*!< ON DELETE CASCADE */
#define DICT_FOREIGN_ON_UPDATE_SET_NULL	8U	/*!< ON UPDATE SET NULL */
#define DICT_FOREIGN_ON_DELETE_NO_ACTION 16U	/*!< ON DELETE NO ACTION */
#define DICT_FOREIGN_ON_UPDATE_NO_ACTION 32U	/*!< ON UPDATE NO ACTION */
/* @} */

/** Display an identifier.
@param[in,out]	s	output stream
@param[in]	id_name	SQL identifier (other than table name)
@return the output stream */
std::ostream&
operator<<(
	std::ostream&		s,
	const id_name_t&	id_name);

/** Display a table name.
@param[in,out]	s		output stream
@param[in]	table_name	table name
@return the output stream */
std::ostream&
operator<<(
	std::ostream&		s,
	const table_name_t&	table_name);

/** List of locks that different transactions have acquired on a table. This
list has a list node that is embedded in a nested union/structure. We have to
generate a specific template for it. */

typedef ut_list_base<lock_t, ut_list_node<lock_t> lock_table_t::*>
	table_lock_list_t;

/** mysql template structure defined in row0mysql.cc */
struct mysql_row_templ_t;

/** Structure defines template related to virtual columns and
their base columns */
struct dict_vcol_templ_t {
	/** number of regular columns */
	ulint			n_col;

	/** number of virtual columns */
	ulint			n_v_col;

	/** array of templates for virtual col and their base columns */
	mysql_row_templ_t**	vtempl;

	/** table's database name */
	std::string		db_name;

	/** table name */
	std::string		tb_name;

	/** MySQL record length */
	ulint			rec_len;

	/** default column value if any */
	byte*			default_rec;

	/** cached MySQL TABLE object */
	TABLE*			mysql_table;

	/** when mysql_table was cached */
	uint64_t		mysql_table_query_id;

	dict_vcol_templ_t() : vtempl(0), mysql_table_query_id(~0ULL) {}
};

/** Metadata on clustered index fields starting from first_user_field() */
class field_map_element_t
{
	/** Number of bits for representing a column number */
	static constexpr uint16_t IND_BITS = 10;

	/** Set if the column of the field has been instantly dropped */
	static constexpr uint16_t DROPPED = 1U << (IND_BITS + 5);

	/** Set if the column was dropped and originally declared NOT NULL */
	static constexpr uint16_t NOT_NULL = 1U << (IND_BITS + 4);

	/** Column index (if !(data & DROPPED)): table->cols[data & IND],
	or field length (if (data & DROPPED)):
	(data & IND) = 0 if variable-length with max_len < 256 bytes;
	(data & IND) = 1 if variable-length with max_len > 255 bytes;
	(data & IND) = 1 + L otherwise, with L=fixed length of the column */
	static constexpr uint16_t IND = (1U << IND_BITS) - 1;

	/** Field metadata */
	uint16_t data;

	void clear_not_null() { data &= uint16_t(~NOT_NULL); }
public:
	bool is_dropped() const { return data & DROPPED; }
	void set_dropped() { data |= DROPPED; }
	bool is_not_null() const { return data & NOT_NULL; }
	void set_not_null() { ut_ad(is_dropped()); data |= NOT_NULL; }
	uint16_t ind() const { return data & IND; }
	void set_ind(uint16_t i)
	{
		DBUG_ASSERT(i <= IND);
		DBUG_ASSERT(!ind());
		data |= i;
	}
	field_map_element_t& operator= (uint16_t value)
	{
		data = value;
		return *this;
	}
	operator uint16_t() { return data; }
};

static_assert(sizeof(field_map_element_t) == 2,
	      "Size mismatch for a persistent data item!");

/** Instantly dropped or reordered columns */
struct dict_instant_t
{
	/** Number of dropped columns */
	unsigned n_dropped;
	/** Dropped columns */
	dict_col_t* dropped;
	/** Map of clustered index non-PK fields[i - first_user_field()]
	to table columns */
	field_map_element_t* field_map;
};

/** These are used when MySQL FRM and InnoDB data dictionary are
in inconsistent state. */
typedef enum {
	DICT_FRM_CONSISTENT = 0,	/*!< Consistent state */
	DICT_FRM_NO_PK = 1,		/*!< MySQL has no primary key
					but InnoDB dictionary has
					non-generated one. */
	DICT_NO_PK_FRM_HAS = 2,		/*!< MySQL has primary key but
					InnoDB dictionary has not. */
	DICT_FRM_INCONSISTENT_KEYS = 3	/*!< Key count mismatch */
} dict_frm_t;

/** Data structure for a database table.  Most fields will be
zero-initialized in dict_table_t::create(). */
struct dict_table_t {

	/** Get reference count.
	@return current value of n_ref_count */
	inline uint32_t get_ref_count() const { return n_ref_count; }

	/** Acquire the table handle. */
	inline void acquire();

	/** Release the table handle.
	@return	whether the last handle was released */
	inline bool release();

	/** @return whether the table supports transactions */
	bool no_rollback() const
	{
		return !(~unsigned(flags) & DICT_TF_MASK_NO_ROLLBACK);
        }
	/** @return whether this is a temporary table */
	bool is_temporary() const
	{
		return flags2 & DICT_TF2_TEMPORARY;
	}

	/** @return whether the table is not in ROW_FORMAT=REDUNDANT */
	bool not_redundant() const { return flags & DICT_TF_COMPACT; }

	/** @return whether this table is readable
	@retval	true	normally
	@retval	false	if this is a single-table tablespace
			and the .ibd file is missing, or a
			page cannot be read or decrypted */
	bool is_readable() const
	{
		ut_ad(file_unreadable || space);
		return(UNIV_LIKELY(!file_unreadable));
	}

	/** @return whether the table is accessible */
	bool is_accessible() const
	{
		return UNIV_LIKELY(is_readable() && !corrupted && space)
			&& !space->is_stopping();
	}

	/** Check if a table name contains the string "/#sql"
	which denotes temporary or intermediate tables in MariaDB. */
	static bool is_temporary_name(const char* name)
	{
		return strstr(name, "/#sql");
	}

	/** @return whether instant ALTER TABLE is in effect */
	bool is_instant() const
	{
		return(UT_LIST_GET_FIRST(indexes)->is_instant());
	}

	/** @return whether the table supports instant ALTER TABLE */
	bool supports_instant() const
	{
		return(!(flags & DICT_TF_MASK_ZIP_SSIZE));
	}

	/** @return the number of instantly dropped columns */
	unsigned n_dropped() const { return instant ? instant->n_dropped : 0; }

	/** Look up an old column.
	@param[in]	cols	the old columns of the table
	@param[in]	col_map	map from old table columns to altered ones
	@param[in]	n_cols	number of old columns
	@param[in]	i	the number of the new column
	@return	old column
	@retval	NULL	if column i was added to the table */
	static const dict_col_t* find(const dict_col_t* cols,
				      const ulint* col_map, ulint n_cols,
				      ulint i)
	{
		for (ulint o = n_cols; o--; ) {
			if (col_map[o] == i) {
				return &cols[o];
			}
		}
		return NULL;
	}

	/** Serialise metadata of dropped or reordered columns.
	@param[in,out]	heap	memory heap for allocation
	@param[out]	field	data field with the metadata */
	inline void serialise_columns(mem_heap_t* heap, dfield_t* field) const;

	/** Reconstruct dropped or reordered columns.
	@param[in]	metadata	data from serialise_columns()
	@param[in]	len		length of the metadata, in bytes
	@return whether parsing the metadata failed */
	bool deserialise_columns(const byte* metadata, ulint len);

	/** Set is_instant() before instant_column().
	@param[in]	old		previous table definition
	@param[in]	col_map		map from old.cols[]
					and old.v_cols[] to this
	@param[out]	first_alter_pos	0, or
					1 + first changed column position */
	inline void prepare_instant(const dict_table_t& old,
				    const ulint* col_map,
				    unsigned& first_alter_pos);

	/** Adjust table metadata for instant ADD/DROP/reorder COLUMN.
	@param[in]	table	table on which prepare_instant() was invoked
	@param[in]	col_map	mapping from cols[] and v_cols[] to table
	@return		whether the metadata record must be updated */
	inline bool instant_column(const dict_table_t& table,
				   const ulint* col_map);

	/** Roll back instant_column().
	@param[in]	old_n_cols		original n_cols
	@param[in]	old_cols		original cols
	@param[in]	old_col_names		original col_names
	@param[in]	old_instant		original instant structure
	@param[in]	old_fields		original fields
	@param[in]	old_n_fields		original number of fields
	@param[in]	old_n_core_fields	original number of core fields
	@param[in]	old_n_v_cols		original n_v_cols
	@param[in]	old_v_cols		original v_cols
	@param[in]	old_v_col_names		original v_col_names
	@param[in]	col_map			column map */
	inline void rollback_instant(
		unsigned	old_n_cols,
		dict_col_t*	old_cols,
		const char*	old_col_names,
		dict_instant_t*	old_instant,
		dict_field_t*	old_fields,
		unsigned	old_n_fields,
		unsigned	old_n_core_fields,
		unsigned	old_n_v_cols,
		dict_v_col_t*	old_v_cols,
		const char*	old_v_col_names,
		const ulint*	col_map);

	/** Add the table definition to the data dictionary cache */
	void add_to_cache();

	/** @return whether the table is versioned.
	It is assumed that both vers_start and vers_end set to 0
	iff table is not versioned. In any other case,
	these fields correspond to actual positions in cols[]. */
	bool versioned() const { return vers_start || vers_end; }
	bool versioned_by_id() const
	{
		return versioned() && cols[vers_start].mtype == DATA_INT;
	}

	/** For overflow fields returns potential max length stored inline */
	inline size_t get_overflow_field_local_len() const;

	/** Parse the table file name into table name and database name.
	@tparam		dict_locked	whether dict_sys.lock() was called
	@param[in,out]	db_name		database name buffer
	@param[in,out]	tbl_name	table name buffer
	@param[out]	db_name_len	database name length
	@param[out]	tbl_name_len	table name length
	@return whether the table name is visible to SQL */
	template<bool dict_locked= false>
	bool parse_name(char (&db_name)[NAME_LEN + 1],
			char (&tbl_name)[NAME_LEN + 1],
			size_t *db_name_len, size_t *tbl_name_len) const;

  /** Clear the table when rolling back TRX_UNDO_EMPTY */
  void clear(que_thr_t *thr);

#ifdef UNIV_DEBUG
  /** @return whether the current thread holds the lock_mutex */
  bool lock_mutex_is_owner() const
  { return lock_mutex_owner == os_thread_get_curr_id(); }
  /** @return whether the current thread holds the stats_mutex (lock_mutex) */
  bool stats_mutex_is_owner() const
  { return lock_mutex_owner == os_thread_get_curr_id(); }
#endif /* UNIV_DEBUG */
  void lock_mutex_init() { lock_mutex.init(); }
  void lock_mutex_destroy() { lock_mutex.destroy(); }
  /** Acquire lock_mutex */
  void lock_mutex_lock()
  {
    ut_ad(!lock_mutex_is_owner());
    lock_mutex.wr_lock();
    ut_ad(!lock_mutex_owner.exchange(os_thread_get_curr_id()));
  }
  /** Try to acquire lock_mutex */
  bool lock_mutex_trylock()
  {
    ut_ad(!lock_mutex_is_owner());
    bool acquired= lock_mutex.wr_lock_try();
    ut_ad(!acquired || !lock_mutex_owner.exchange(os_thread_get_curr_id()));
    return acquired;
  }
  /** Release lock_mutex */
  void lock_mutex_unlock()
  {
    ut_ad(lock_mutex_owner.exchange(0) == os_thread_get_curr_id());
    lock_mutex.wr_unlock();
  }
#ifndef SUX_LOCK_GENERIC
  /** @return whether the lock mutex is held by some thread */
  bool lock_mutex_is_locked() const noexcept { return lock_mutex.is_locked(); }
#endif

  /* stats mutex lock currently defaults to lock_mutex but in the future,
  there could be a use-case to have separate mutex for stats.
  extra indirection (through inline so no performance hit) should
  help simplify code and increase long-term maintainability */
  void stats_mutex_init() { lock_mutex_init(); }
  void stats_mutex_destroy() { lock_mutex_destroy(); }
  void stats_mutex_lock() { lock_mutex_lock(); }
  void stats_mutex_unlock() { lock_mutex_unlock(); }

  /** Rename the data file.
  @param new_name     name of the table
  @param replace      whether to replace the file with the new name
                      (as part of rolling back TRUNCATE) */
  dberr_t rename_tablespace(const char *new_name, bool replace) const;

private:
	/** Initialize instant->field_map.
	@param[in]	table	table definition to copy from */
	inline void init_instant(const dict_table_t& table);
public:
	/** Id of the table. */
	table_id_t				id;
	/** dict_sys.id_hash chain node */
	dict_table_t*				id_hash;
	/** Table name in name_hash */
	table_name_t				name;
	/** dict_sys.name_hash chain node */
	dict_table_t*				name_hash;

	/** Memory heap */
	mem_heap_t*				heap;

	/** NULL or the directory path specified by DATA DIRECTORY. */
	char*					data_dir_path;

	/** The tablespace of the table */
	fil_space_t*				space;
	/** Tablespace ID */
	ulint					space_id;

	/** Stores information about:
	1 row format (redundant or compact),
	2 compressed page size (zip shift size),
	3 whether using atomic blobs,
	4 whether the table has been created with the option DATA DIRECTORY.
	Use DICT_TF_GET_COMPACT(), DICT_TF_GET_ZIP_SSIZE(),
	DICT_TF_HAS_ATOMIC_BLOBS() and DICT_TF_HAS_DATA_DIR() to parse this
	flag. */
	unsigned				flags:DICT_TF_BITS;

	/** Stores information about:
	1 whether the table has been created using CREATE TEMPORARY TABLE,
	2 whether the table has an internally defined DOC ID column,
	3 whether the table has a FTS index,
	4 whether DOC ID column need to be added to the FTS index,
	5 whether the table is being created its own tablespace,
	6 whether the table has been DISCARDed,
	7 whether the aux FTS tables names are in hex.
	Use DICT_TF2_FLAG_IS_SET() to parse this flag. */
	unsigned				flags2:DICT_TF2_BITS;

	/** TRUE if the table is an intermediate table during copy alter
	operation or a partition/subpartition which is required for copying
	data and skip the undo log for insertion of row in the table.
	This variable will be set and unset during extra(), or during the
	process of altering partitions */
	unsigned                                skip_alter_undo:1;

	/*!< whether this is in a single-table tablespace and the .ibd
	file is missing or page decryption failed and page is corrupted */
	unsigned				file_unreadable:1;

	/** TRUE if the table object has been added to the dictionary cache. */
	unsigned				cached:1;

	/** Number of non-virtual columns defined so far. */
	unsigned				n_def:10;

	/** Number of non-virtual columns. */
	unsigned				n_cols:10;

	/** Number of total columns (inlcude virtual and non-virtual) */
	unsigned				n_t_cols:10;

	/** Number of total columns defined so far. */
	unsigned                                n_t_def:10;

	/** Number of virtual columns defined so far. */
	unsigned                                n_v_def:10;

	/** Number of virtual columns. */
	unsigned                                n_v_cols:10;

	/** 1 + the position of autoinc counter field in clustered
	index, or 0 if there is no persistent AUTO_INCREMENT column in
	the table. */
	unsigned				persistent_autoinc:10;

	/** TRUE if it's not an InnoDB system table or a table that has no FK
	relationships. */
	unsigned				can_be_evicted:1;

	/** TRUE if table is corrupted. */
	unsigned				corrupted:1;

	/** TRUE if some indexes should be dropped after ONLINE_INDEX_ABORTED
	or ONLINE_INDEX_ABORTED_DROPPED. */
	unsigned				drop_aborted:1;

	/** Array of column descriptions. */
	dict_col_t*				cols;

	/** Array of virtual column descriptions. */
	dict_v_col_t*				v_cols;

	/** List of stored column descriptions. It is used only for foreign key
	check during create table and copy alter operations.
	During copy alter, s_cols list is filled during create table operation
	and need to preserve till rename table operation. That is the
	reason s_cols is a part of dict_table_t */
	dict_s_col_list*			s_cols;

	/** Instantly dropped or reordered columns, or NULL if none */
	dict_instant_t*				instant;

	/** Column names packed in a character string
	"name1\0name2\0...nameN\0". Until the string contains n_cols, it will
	be allocated from a temporary heap. The final string will be allocated
	from table->heap. */
	const char*				col_names;

	/** Virtual column names */
	const char*				v_col_names;
	unsigned	vers_start:10;
				/*!< System Versioning: row start col index */
	unsigned	vers_end:10;
				/*!< System Versioning: row end col index */
	bool		is_system_db;
				/*!< True if the table belongs to a system
				database (mysql, information_schema or
				performance_schema) */
	dict_frm_t	dict_frm_mismatch;
				/*!< !DICT_FRM_CONSISTENT==0 if data
				dictionary information and
				MySQL FRM information mismatch. */
	/** The FTS_DOC_ID_INDEX, or NULL if no fulltext indexes exist */
	dict_index_t*				fts_doc_id_index;

	/** List of indexes of the table. */
	UT_LIST_BASE_NODE_T(dict_index_t)	indexes;
#ifdef BTR_CUR_HASH_ADAPT
	/** List of detached indexes that are waiting to be freed along with
	the last adaptive hash index entry.
	Protected by autoinc_mutex (sic!) */
	UT_LIST_BASE_NODE_T(dict_index_t)	freed_indexes;
#endif /* BTR_CUR_HASH_ADAPT */

	/** List of foreign key constraints in the table. These refer to
	columns in other tables. */
	UT_LIST_BASE_NODE_T(dict_foreign_t)	foreign_list;

	/** List of foreign key constraints which refer to this table. */
	UT_LIST_BASE_NODE_T(dict_foreign_t)	referenced_list;

	/** Node of the LRU list of tables. */
	UT_LIST_NODE_T(dict_table_t)		table_LRU;

	/** Maximum recursive level we support when loading tables chained
	together with FK constraints. If exceeds this level, we will stop
	loading child table into memory along with its parent table. */
	byte					fk_max_recusive_level;

  /** DDL transaction that last touched the table definition, or 0 if
  no history is available. This includes possible changes in
  ha_innobase::prepare_inplace_alter_table() and
  ha_innobase::commit_inplace_alter_table(). */
  trx_id_t def_trx_id;

  /** Last transaction that inserted into an empty table.
  Updated while holding exclusive table lock and an exclusive
  latch on the clustered index root page (which must also be
  an empty leaf page), and an ahi_latch (if btr_search_enabled). */
  Atomic_relaxed<trx_id_t> bulk_trx_id;

  /** Original table name, for MDL acquisition in purge. Normally,
  this points to the same as name. When is_temporary_name(name.m_name) holds,
  this should be a copy of the original table name, allocated from heap. */
  table_name_t mdl_name;

	/*!< set of foreign key constraints in the table; these refer to
	columns in other tables */
	dict_foreign_set			foreign_set;

	/*!< set of foreign key constraints which refer to this table */
	dict_foreign_set			referenced_set;

	/** Statistics for query optimization. Mostly protected by
	dict_sys.latch and stats_mutex_lock(). @{ */

	/** TRUE if statistics have been calculated the first time after
	database startup or table creation. */
	unsigned				stat_initialized:1;

	/** Timestamp of last recalc of the stats. */
	time_t					stats_last_recalc;

	/** The two bits below are set in the 'stat_persistent' member. They
	have the following meaning:
	1. _ON=0, _OFF=0, no explicit persistent stats setting for this table,
	the value of the global srv_stats_persistent is used to determine
	whether the table has persistent stats enabled or not
	2. _ON=0, _OFF=1, persistent stats are explicitly disabled for this
	table, regardless of the value of the global srv_stats_persistent
	3. _ON=1, _OFF=0, persistent stats are explicitly enabled for this
	table, regardless of the value of the global srv_stats_persistent
	4. _ON=1, _OFF=1, not allowed, we assert if this ever happens. */
	#define DICT_STATS_PERSISTENT_ON	(1 << 1)
	#define DICT_STATS_PERSISTENT_OFF	(1 << 2)

	/** Indicates whether the table uses persistent stats or not. See
	DICT_STATS_PERSISTENT_ON and DICT_STATS_PERSISTENT_OFF. */
	ib_uint32_t				stat_persistent;

	/** The two bits below are set in the 'stats_auto_recalc' member. They
	have the following meaning:
	1. _ON=0, _OFF=0, no explicit auto recalc setting for this table, the
	value of the global srv_stats_persistent_auto_recalc is used to
	determine whether the table has auto recalc enabled or not
	2. _ON=0, _OFF=1, auto recalc is explicitly disabled for this table,
	regardless of the value of the global srv_stats_persistent_auto_recalc
	3. _ON=1, _OFF=0, auto recalc is explicitly enabled for this table,
	regardless of the value of the global srv_stats_persistent_auto_recalc
	4. _ON=1, _OFF=1, not allowed, we assert if this ever happens. */
	#define DICT_STATS_AUTO_RECALC_ON	(1 << 1)
	#define DICT_STATS_AUTO_RECALC_OFF	(1 << 2)

	/** Indicates whether the table uses automatic recalc for persistent
	stats or not. See DICT_STATS_AUTO_RECALC_ON and
	DICT_STATS_AUTO_RECALC_OFF. */
	ib_uint32_t				stats_auto_recalc;

	/** The number of pages to sample for this table during persistent
	stats estimation. If this is 0, then the value of the global
	srv_stats_persistent_sample_pages will be used instead. */
	ulint					stats_sample_pages;

	/** Approximate number of rows in the table. We periodically calculate
	new estimates. */
	ib_uint64_t				stat_n_rows;

	/** Approximate clustered index size in database pages. */
	ulint					stat_clustered_index_size;

	/** Approximate size of other indexes in database pages. */
	ulint					stat_sum_of_other_index_sizes;

	/** How many rows are modified since last stats recalc. When a row is
	inserted, updated, or deleted, we add 1 to this number; we calculate
	new estimates for the table and the indexes if the table has changed
	too much, see dict_stats_update_if_needed(). The counter is reset
	to zero at statistics calculation. This counter is not protected by
	any latch, because this is only used for heuristics. */
	ib_uint64_t				stat_modified_counter;

	bool		stats_error_printed;
				/*!< Has persistent stats error beein
				already printed for this table ? */
	/* @} */

	/** AUTOINC related members. @{ */

	/* The actual collection of tables locked during AUTOINC read/write is
	kept in trx_t. In order to quickly determine whether a transaction has
	locked the AUTOINC lock we keep a pointer to the transaction here in
	the 'autoinc_trx' member. This is to avoid acquiring the
	lock_sys.latch and scanning the vector in trx_t.
	When an AUTOINC lock has to wait, the corresponding lock instance is
	created on the trx lock heap rather than use the pre-allocated instance
	in autoinc_lock below. */

	/** A buffer for an AUTOINC lock for this table. We allocate the
	memory here so that individual transactions can get it and release it
	without a need to allocate space from the lock heap of the trx:
	otherwise the lock heap would grow rapidly if we do a large insert
	from a select. */
	lock_t*					autoinc_lock;

  /** Mutex protecting autoinc and freed_indexes. */
  srw_spin_mutex autoinc_mutex;
private:
  /** Mutex protecting locks on this table. */
  srw_spin_mutex lock_mutex;
#ifdef UNIV_DEBUG
  /** The owner of lock_mutex (0 if none) */
  Atomic_relaxed<os_thread_id_t> lock_mutex_owner{0};
#endif
public:
  /** Autoinc counter value to give to the next inserted row. */
  uint64_t autoinc;

  /** The transaction that currently holds the the AUTOINC lock on this table.
  Protected by lock_mutex.
  The thread that is executing autoinc_trx may read this field without
  holding a latch, in row_lock_table_autoinc_for_mysql().
  Only the autoinc_trx thread may clear this field; it cannot be
  modified on the behalf of a transaction that is being handled by a
  different thread. */
  Atomic_relaxed<const trx_t*> autoinc_trx;

  /** Number of granted or pending autoinc_lock on this table. This
  value is set after acquiring lock_sys.latch but
  in innodb_autoinc_lock_mode=1 (the default),
  ha_innobase::innobase_lock_autoinc() will perform a dirty read
  to determine whether other transactions have acquired the autoinc_lock. */
  uint32_t n_waiting_or_granted_auto_inc_locks;

	/* @} */

  /** Number of granted or pending LOCK_S or LOCK_X on the table.
  Protected by lock_sys.assert_locked(*this). */
  uint32_t n_lock_x_or_s;

	/** FTS specific state variables. */
	fts_t*					fts;

	/** Quiescing states, protected by the dict_index_t::lock. ie. we can
	only change the state if we acquire all the latches (dict_index_t::lock)
	in X mode of this table's indexes. */
	ib_quiesce_t				quiesce;

  /** Count of the number of record locks on this table. We use this to
  determine whether we can evict the table from the dictionary cache.
  Modified when lock_sys.is_writer(), or
  lock_sys.assert_locked(page_id) and trx->mutex_is_owner() hold.
  @see trx_lock_t::trx_locks */
  Atomic_counter<uint32_t> n_rec_locks;
private:
  /** Count of how many handles are opened to this table. Dropping of the
  table is NOT allowed until this count gets to zero. MySQL does NOT
  itself check the number of open handles at DROP. */
  Atomic_counter<uint32_t> n_ref_count;
public:
  /** List of locks on the table. Protected by lock_sys.assert_locked(lock). */
  table_lock_list_t locks;

  /** Timestamp of the last modification of this table. */
  Atomic_relaxed<time_t> update_time;
  /** Transactions whose view low limit is greater than this number are
  not allowed to access the MariaDB query cache.
  @see innobase_query_caching_table_check_low()
  @see trx_t::commit_tables() */
  Atomic_relaxed<trx_id_t> query_cache_inv_trx_id;

#ifdef UNIV_DEBUG
	/** Value of 'magic_n'. */
	#define DICT_TABLE_MAGIC_N		76333786

	/** Magic number. */
	ulint					magic_n;
#endif /* UNIV_DEBUG */
	/** mysql_row_templ_t for base columns used for compute the virtual
	columns */
	dict_vcol_templ_t*			vc_templ;

  /* @return whether the table has any other transcation lock
  other than the given transaction */
  bool has_lock_other_than(const trx_t *trx) const
  {
    for (lock_t *lock= UT_LIST_GET_FIRST(locks); lock;
         lock= UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock))
      if (lock->trx != trx)
        return true;
    return false;
  }

  /** @return whether the name is
  mysql.innodb_index_stats or mysql.innodb_table_stats */
  bool is_stats_table() const;

  /** Create metadata.
  @param name     table name
  @param space    tablespace
  @param n_cols   total number of columns (both virtual and non-virtual)
  @param n_v_cols number of virtual columns
  @param flags    table flags
  @param flags2   table flags2
  @return newly allocated table object */
  static dict_table_t *create(const span<const char> &name, fil_space_t *space,
                              ulint n_cols, ulint n_v_cols, ulint flags,
                              ulint flags2);
};

inline void dict_index_t::set_modified(mtr_t& mtr) const
{
	mtr.set_named_space(table->space);
}

inline bool table_name_t::is_temporary() const
{
	return dict_table_t::is_temporary_name(m_name);
}

inline bool dict_index_t::is_readable() const { return table->is_readable(); }

inline bool dict_index_t::is_instant() const
{
	ut_ad(n_core_fields > 0);
	ut_ad(n_core_fields <= n_fields || table->n_dropped());
	ut_ad(n_core_fields == n_fields
	      || (type & ~(DICT_UNIQUE | DICT_CORRUPT)) == DICT_CLUSTERED);
	ut_ad(n_core_fields == n_fields || table->supports_instant());
	ut_ad(n_core_fields == n_fields || !table->is_temporary());
	ut_ad(!table->instant || !table->is_temporary());

	return n_core_fields != n_fields
		|| (is_primary() && table->instant);
}

inline bool dict_index_t::is_corrupted() const
{
	return UNIV_UNLIKELY(online_status >= ONLINE_INDEX_ABORTED
			     || (type & DICT_CORRUPT)
			     || (table && table->corrupted));
}

inline void dict_index_t::clear_instant_add()
{
  DBUG_ASSERT(is_primary());
  DBUG_ASSERT(is_instant());
  DBUG_ASSERT(!table->instant);
  for (unsigned i= n_core_fields; i < n_fields; i++)
    fields[i].col->clear_instant();
  n_core_fields= n_fields;
  n_core_null_bytes= static_cast<byte>
    (UT_BITS_IN_BYTES(static_cast<unsigned>(n_nullable)));
}

inline void dict_index_t::clear_instant_alter()
{
	DBUG_ASSERT(is_primary());
	DBUG_ASSERT(n_fields == n_def);

	if (!table->instant) {
		if (is_instant()) {
			clear_instant_add();
		}
		return;
	}

#ifndef DBUG_OFF
	for (unsigned i = first_user_field(); i--; ) {
		DBUG_ASSERT(!fields[i].col->is_dropped());
		DBUG_ASSERT(!fields[i].col->is_nullable());
	}
#endif
	const dict_col_t* ai_col = table->persistent_autoinc
		? fields[table->persistent_autoinc - 1].col
		: NULL;
	dict_field_t* const begin = &fields[first_user_field()];
	dict_field_t* end = &fields[n_fields];

	for (dict_field_t* d = begin; d < end; ) {
		/* Move fields for dropped columns to the end. */
		if (!d->col->is_dropped()) {
			d++;
		} else {
			if (d->col->is_nullable()) {
				n_nullable--;
			}

			std::swap(*d, *--end);
		}
	}

	DBUG_ASSERT(&fields[n_fields - table->n_dropped()] == end);
	n_core_fields = n_fields = n_def
		= static_cast<unsigned>(end - fields) & MAX_N_FIELDS;
	n_core_null_bytes = static_cast<byte>(UT_BITS_IN_BYTES(n_nullable));
	std::sort(begin, end, [](const dict_field_t& a, const dict_field_t& b)
			      { return a.col->ind < b.col->ind; });
	table->instant = NULL;
	if (ai_col) {
		auto a = std::find_if(fields, end,
				      [ai_col](const dict_field_t& f)
				      { return f.col == ai_col; });
		table->persistent_autoinc = (a == end)
			? 0
			: (1 + static_cast<unsigned>(a - fields))
			& MAX_N_FIELDS;
	}
}

/** @return whether the column was instantly dropped
@param[in] index	the clustered index */
inline bool dict_col_t::is_dropped(const dict_index_t& index) const
{
	DBUG_ASSERT(index.is_primary());
	DBUG_ASSERT(!is_dropped() == !index.table->instant);
	DBUG_ASSERT(!is_dropped() || (this >= index.table->instant->dropped
				      && this < index.table->instant->dropped
				      + index.table->instant->n_dropped));
	return is_dropped();
}

/*******************************************************************//**
Initialise the table lock list. */
void
lock_table_lock_list_init(
/*======================*/
	table_lock_list_t*	locks);		/*!< List to initialise */

/** A function object to add the foreign key constraint to the referenced set
of the referenced table, if it exists in the dictionary cache. */
struct dict_foreign_add_to_referenced_table {
	void operator()(dict_foreign_t*	foreign) const
	{
		if (dict_table_t* table = foreign->referenced_table) {
			std::pair<dict_foreign_set::iterator, bool>	ret
				= table->referenced_set.insert(foreign);
			ut_a(ret.second);
		}
	}
};

/** Check whether the col is used in spatial index or regular index.
@param[in]	col	column to check
@return spatial status */
inline
spatial_status_t
dict_col_get_spatial_status(
	const dict_col_t*	col)
{
	spatial_status_t	spatial_status = SPATIAL_NONE;

	/* Column is not a part of any index. */
	if (!col->ord_part) {
		return(spatial_status);
	}

	if (DATA_GEOMETRY_MTYPE(col->mtype)) {
		if (col->max_prefix == 0) {
			spatial_status = SPATIAL_ONLY;
		} else {
			/* Any regular index on a geometry column
			should have a prefix. */
			spatial_status = SPATIAL_MIXED;
		}
	}

	return(spatial_status);
}

/** Clear defragmentation summary. */
inline void dict_stats_empty_defrag_summary(dict_index_t* index)
{
	index->stat_defrag_n_pages_freed = 0;
}

/** Clear defragmentation related index stats. */
inline void dict_stats_empty_defrag_stats(dict_index_t* index)
{
	index->stat_defrag_modified_counter = 0;
	index->stat_defrag_n_page_split = 0;
}

#include "dict0mem.ic"

#endif /* dict0mem_h */
