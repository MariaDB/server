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

#include "data0type.h"
#include "mem0mem.h"
#include "row0types.h"
#include "rem0types.h"
#include "btr0types.h"
#include "lock0types.h"
#include "que0types.h"
#include "sync0rw.h"
#include "ut0mem.h"
#include "ut0rnd.h"
#include "ut0byte.h"
#include "hash0hash.h"
#include "trx0types.h"
#include "fts0fts.h"
#include "buf0buf.h"
#include "gis0type.h"
#include "fil0fil.h"
#include <my_crypt.h>
#include "fil0crypt.h"
#include <set>
#include <algorithm>
#include <iterator>
#include <ostream>

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
#define DICT_TF_COMPACT			1	/*!< Compact row format. */

/** This bitmask is used in SYS_TABLES.N_COLS to set and test whether
the Compact page format is used, i.e ROW_FORMAT != REDUNDANT */
#define DICT_N_COLS_COMPACT	0x80000000UL

/** Width of the COMPACT flag */
#define DICT_TF_WIDTH_COMPACT		1

/** Width of the ZIP_SSIZE flag */
#define DICT_TF_WIDTH_ZIP_SSIZE		4

/** Width of the ATOMIC_BLOBS flag.  The Antelope file formats broke up
BLOB and TEXT fields, storing the first 768 bytes in the clustered index.
Barracuda row formats store the whole blob or text field off-page atomically.
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
Width of atomic writes flag
DEFAULT=0, ON = 1, OFF = 2
*/
#define DICT_TF_WIDTH_ATOMIC_WRITES 2

/** Width of all the currently known table flags */
#define DICT_TF_BITS	(DICT_TF_WIDTH_COMPACT			\
			+ DICT_TF_WIDTH_ZIP_SSIZE		\
			+ DICT_TF_WIDTH_ATOMIC_BLOBS		\
			+ DICT_TF_WIDTH_DATA_DIR		\
			+ DICT_TF_WIDTH_PAGE_COMPRESSION	\
			+ DICT_TF_WIDTH_PAGE_COMPRESSION_LEVEL)

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
/** Zero relative shift position of the ATOMIC_WRITES field */
#define DICT_TF_POS_ATOMIC_WRITES	(DICT_TF_POS_PAGE_COMPRESSION_LEVEL \
					+ DICT_TF_WIDTH_PAGE_COMPRESSION_LEVEL)
#define DICT_TF_POS_UNUSED		(DICT_TF_POS_ATOMIC_WRITES     \
					+ DICT_TF_WIDTH_ATOMIC_WRITES)

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
/** Bit mask of the ATOMIC_WRITES field */
#define DICT_TF_MASK_ATOMIC_WRITES		\
		((~(~0U << DICT_TF_WIDTH_ATOMIC_WRITES)) \
		<< DICT_TF_POS_ATOMIC_WRITES)

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
/** Return the value of the ATOMIC_WRITES field */
#define DICT_TF_GET_ATOMIC_WRITES(flags)       \
		((flags & DICT_TF_MASK_ATOMIC_WRITES)	\
		>> DICT_TF_POS_ATOMIC_WRITES)

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
	(table->flags2 &= ~(flag))

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

/** Creates a table memory object.
@param[in]      name            table name
@param[in]      space           space where the clustered index
                                of the table is placed
@param[in]      n_cols          total number of columns including
                                virtual and non-virtual columns
@param[in]      n_v_cols        number of virtual columns
@param[in]      flags           table flags
@param[in]      flags2          table flags2
@return own: table object */
dict_table_t*
dict_mem_table_create(
	const char*     name,
	ulint           space,
	ulint           n_cols,
        ulint           n_v_cols,
        ulint           flags,
        ulint           flags2);

/****************************************************************//**
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
	const char*	table_name,	/*!< in: table name */
	const char*	index_name,	/*!< in: index name */
	ulint		space,		/*!< in: space where the index tree is
					placed, ignored if the index is of
					the clustered type */
	ulint		type,		/*!< in: DICT_UNIQUE,
					DICT_CLUSTERED, ... ORed */
	ulint		n_fields);	/*!< in: number of fields */
/**********************************************************************//**
Creates an index memory object.
@return own: index object */
dict_index_t*
dict_mem_index_create(
/*==================*/
	const char*	table_name,	/*!< in: table name */
	const char*	index_name,	/*!< in: index name */
	ulint		space,		/*!< in: space where the index tree is
					placed, ignored if the index is of
					the clustered type */
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

/** Initialize dict memory variables */
void
dict_mem_init(void);

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
	size_t dblen() const { return dbend() - m_name; }

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
					3072 for Barracuda table */

  /** @return whether this is a virtual column */
  bool is_virtual() const { return prtype & DATA_VIRTUAL; }

  /** Detach a virtual column from an index.
  @param index  being-freed index */
  inline void detach(const dict_index_t &index);
};

/** Index information put in a list of virtual column structure. Index
id and virtual column position in the index will be logged.
There can be multiple entries for a given index, with a different position. */
struct dict_v_idx_t {
	/** active index on the column */
	dict_index_t*	index;

	/** position in this index */
	ulint		nth_field;
};

/** Index list to put in dict_v_col_t */
typedef	std::list<dict_v_idx_t, ut_allocator<dict_v_idx_t> >	dict_v_idx_list;

/** Data structure for a virtual column in a table */
struct dict_v_col_t{
	/** column structure */
	dict_col_t		m_col;

	/** array of base column ptr */
	dict_col_t**		base_col;

	/** number of base column */
	ulint			num_base;

	/** column pos in table */
	ulint			v_pos;

	/** Virtual index list, and column position in the index,
	the allocated memory is not from table->heap */
	dict_v_idx_list*	v_indexes;

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
typedef std::list<dict_s_col_t, ut_allocator<dict_s_col_t> >	dict_s_col_list;

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
Barracuda row formats COMPRESSED and DYNAMIC, the length could
be REC_VERSION_56_MAX_INDEX_COL_LEN (3072) bytes */
#define DICT_MAX_FIELD_LEN_BY_FORMAT(table)				\
		((dict_table_get_format(table) < UNIV_FORMAT_B)		\
			? (REC_ANTELOPE_MAX_INDEX_COL_LEN - 1)		\
			: REC_VERSION_56_MAX_INDEX_COL_LEN)

#define DICT_MAX_FIELD_LEN_BY_FORMAT_FLAG(flags)			\
		((DICT_TF_HAS_ATOMIC_BLOBS(flags) < UNIV_FORMAT_B)	\
			? (REC_ANTELOPE_MAX_INDEX_COL_LEN - 1)		\
			: REC_VERSION_56_MAX_INDEX_COL_LEN)

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
	mysql_mutex_t	mutex;	/*!< mutex protecting the info */
	ulint		pad;	/*!< number of bytes used as pad */
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
struct dict_index_t{
	index_id_t	id;	/*!< id of the index */
	mem_heap_t*	heap;	/*!< memory heap */
	id_name_t	name;	/*!< index name */
	const char*	table_name;/*!< table name */
	dict_table_t*	table;	/*!< back pointer to table */
	unsigned	space:32;
				/*!< space where the index tree is placed */
	/** root page number, or FIL_NULL if the index has been detached
	from storage (DISCARD TABLESPACE or similar),
	or 1 if the index is in table->freed_indexes */
	unsigned	page:32;/*!< index tree root page number */
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
#if (1<<MAX_KEY_LENGTH_BITS) < MAX_KEY_LENGTH
# error (1<<MAX_KEY_LENGTH_BITS) < MAX_KEY_LENGTH
#endif
	unsigned	n_user_defined_cols:10;
				/*!< number of columns the user defined to
				be in the index: in the internal
				representation we add more columns */
	unsigned	nulls_equal:1;
				/*!< if true, SQL NULL == SQL NULL */
#ifdef BTR_CUR_HASH_ADAPT
#ifdef MYSQL_INDEX_DISABLE_AHI
 	unsigned	disable_ahi:1;
				/*!< whether to disable the
				adaptive hash index.
				Maybe this could be disabled for
				temporary tables? */
#endif
#endif /* BTR_CUR_HASH_ADAPT */
	unsigned	n_uniq:10;/*!< number of fields from the beginning
				which are enough to determine an index
				entry uniquely */
	unsigned	n_def:10;/*!< number of fields defined so far */
	unsigned	n_fields:10;/*!< number of fields in the index */
	unsigned	n_nullable:10;/*!< number of nullable fields */
	unsigned	cached:1;/*!< TRUE if the index object is in the
				dictionary cache */
	unsigned	to_be_dropped:1;
				/*!< TRUE if the index is to be dropped;
				protected by dict_operation_lock */
	unsigned	online_status:2;
				/*!< enum online_index_status.
				Transitions from ONLINE_INDEX_COMPLETE (to
				ONLINE_INDEX_CREATION) are protected
				by dict_operation_lock and
				dict_sys->mutex. Other changes are
				protected by index->lock. */
	unsigned	uncommitted:1;
				/*!< a flag that is set for secondary indexes
				that have not been committed to the
				data dictionary yet */

#ifdef UNIV_DEBUG
	/** whether this is a dummy index object */
	bool		is_dummy;
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

	bool            index_fts_syncing;/*!< Whether the fts index is
					still syncing in the background;
					FIXME: remove this and use MDL */
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
	/** R-tree split sequence number */
	volatile int32	rtr_ssn;
	rtr_info_track_t*
			rtr_track;/*!< tracking all R-Tree search cursors */
	trx_id_t	trx_id; /*!< id of the transaction that created this
				index, or 0 if the index existed
				when InnoDB was started up */
	zip_pad_info_t	zip_pad;/*!< Information about state of
				compression failures and successes */
	mutable rw_lock_t	lock;	/*!< read-write lock protecting the
				upper levels of the index tree */

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

	/** @return whether this index is readable
	@retval	true	normally
	@retval	false	if this is a single-table tablespace
			and the .ibd file is missing, or a
			page cannot be read or decrypted */
	inline bool is_readable() const;

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
    if (!has_virtual())
      return;
    for (unsigned i= 0; i < n_fields; i++)
    {
      dict_col_t* col= fields[i].col;
      if (!col || !col->is_virtual())
        continue;
      col->detach(*this);
    }
  }

  /** Assign the number of new column to be added as a part
  of the index
  @param        n_vcol  number of virtual columns to be added */
  void assign_new_v_col(ulint n_vcol)
  {
    new_vcol_info= static_cast<dict_add_v_col_info*>(
      mem_heap_zalloc(heap, sizeof *new_vcol_info));
    new_vcol_info->n_v_col= n_vcol;
  }

  /* @return whether index has new virtual column */
  bool has_new_v_col() const
  {
    return new_vcol_info != NULL;
  }

  /* @return number of newly added virtual column */
  ulint get_new_n_vcol() const
  {
    if (new_vcol_info)
      return new_vcol_info->n_v_col;
    return 0;
  }

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
};

/** Detach a virtual column from an index.
@param index  being-freed index */
inline void dict_col_t::detach(const dict_index_t &index)
{
  ut_ad(is_virtual());

  if (dict_v_idx_list *v_indexes= reinterpret_cast<const dict_v_col_t*>(this)
      ->v_indexes)
  {
    for (dict_v_idx_list::iterator i= v_indexes->begin();
         i != v_indexes->end(); i++)
    {
      if (i->index == &index) {
        v_indexes->erase(i);
        return;
      }
    }
  }
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
		return(ut_strcmp(lhs->id, rhs->id) < 0);
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

#ifdef WITH_WSREP
/** A function object to find a foreign key with the given index as the
foreign index. Return the foreign key with matching criteria or NULL */
struct dict_foreign_with_foreign_index {

	dict_foreign_with_foreign_index(const dict_index_t*	index)
	: m_index(index)
	{}

	bool operator()(const dict_foreign_t*	foreign) const
	{
		return(foreign->foreign_index == m_index);
	}

	const dict_index_t*	m_index;
};
#endif

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
initialized to 0, NULL or FALSE in dict_mem_table_create(). */
struct dict_table_t {

	/** Get reference count.
	@return current value of n_ref_count */
	inline int32 get_ref_count()
	{
		return my_atomic_load32_explicit(&n_ref_count,
						 MY_MEMORY_ORDER_RELAXED);
	}

	/** Acquire the table handle. */
	inline void acquire();

	/** Release the table handle.
	@return	whether the last handle was released */
	inline bool release();

	/** @return whether this is a temporary table */
	bool is_temporary() const
	{
		return flags2 & DICT_TF2_TEMPORARY;
	}

	/** @return whether this table is readable
	@retval	true	normally
	@retval	false	if this is a single-table tablespace
			and the .ibd file is missing, or a
			page cannot be read or decrypted */
	bool is_readable() const
	{
		return(UNIV_LIKELY(!file_unreadable));
	}

	/** Check if a table name contains the string "/#sql"
	which denotes temporary or intermediate tables in MariaDB. */
	static bool is_temporary_name(const char* name)
	{
		return strstr(name, "/" TEMP_FILE_PREFIX) != NULL;
	}

	/** For overflow fields returns potential max length stored inline */
	size_t get_overflow_field_local_len() const;

	/** Id of the table. */
	table_id_t				id;
	/** Hash chain node. */
	hash_node_t				id_hash;
	/** Table name. */
	table_name_t				name;
	/** Hash chain node. */
	hash_node_t				name_hash;

	/** Memory heap */
	mem_heap_t*				heap;

	/** NULL or the directory path specified by DATA DIRECTORY. */
	char*					data_dir_path;

	/** Space where the clustered index of the table is placed. */
	uint32_t				space;

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

	/** TRUE if the table is to be dropped, but not yet actually dropped
	(could in the background drop list). It is turned on at the beginning
	of row_drop_table_for_mysql() and turned off just before we start to
	update system tables for the drop. It is protected by
	dict_operation_lock. */
	unsigned				to_be_dropped:1;

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

	/** Column names packed in a character string
	"name1\0name2\0...nameN\0". Until the string contains n_cols, it will
	be allocated from a temporary heap. The final string will be allocated
	from table->heap. */
	const char*				col_names;

	/** Virtual column names */
	const char*				v_col_names;

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
	unsigned				fk_max_recusive_level:8;

	/** Count of how many foreign key check operations are currently being
	performed on the table. We cannot drop the table while there are
	foreign key checks running on it. */
	ulint					n_foreign_key_checks_running;

	/** Transactions whose view low limit is greater than this number are
	not allowed to store to the MySQL query cache or retrieve from it.
	When a trx with undo logs commits, it sets this to the value of the
	transaction id. */
	trx_id_t				query_cache_inv_trx_id;

	/** Transaction id that last touched the table definition. Either when
	loading the definition or CREATE TABLE, or ALTER TABLE (prepare,
	commit, and rollback phases). */
	trx_id_t				def_trx_id;

	/*!< set of foreign key constraints in the table; these refer to
	columns in other tables */
	dict_foreign_set			foreign_set;

	/*!< set of foreign key constraints which refer to this table */
	dict_foreign_set			referenced_set;

	/** Statistics for query optimization. Mostly protected by
	dict_sys->mutex. @{ */

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

	/** Background stats thread is not working on this table. */
	#define BG_STAT_NONE			0

	/** Set in 'stats_bg_flag' when the background stats code is working
	on this table. The DROP TABLE code waits for this to be cleared before
	proceeding. */
	#define BG_STAT_IN_PROGRESS		(1 << 0)

	/** Set in 'stats_bg_flag' when DROP TABLE starts waiting on
	BG_STAT_IN_PROGRESS to be cleared. The background stats thread will
	detect this and will eventually quit sooner. */
	#define BG_STAT_SHOULD_QUIT		(1 << 1)

	/** The state of the background stats thread wrt this table.
	See BG_STAT_NONE, BG_STAT_IN_PROGRESS and BG_STAT_SHOULD_QUIT.
	Writes are covered by dict_sys->mutex. Dirty reads are possible. */

	#define BG_SCRUB_IN_PROGRESS	((byte)(1 << 2))
				/*!< BG_SCRUB_IN_PROGRESS is set in
				stats_bg_flag when the background
				scrub code is working on this table. The DROP
				TABLE code waits for this to be cleared
				before proceeding. */

	#define BG_STAT_SHOULD_QUIT		(1 << 1)

	#define BG_IN_PROGRESS (BG_STAT_IN_PROGRESS | BG_SCRUB_IN_PROGRESS)


	/** The state of the background stats thread wrt this table.
	See BG_STAT_NONE, BG_STAT_IN_PROGRESS and BG_STAT_SHOULD_QUIT.
	Writes are covered by dict_sys->mutex. Dirty reads are possible. */
	byte					stats_bg_flag;

	bool		stats_error_printed;
				/*!< Has persistent stats error beein
				already printed for this table ? */
	/* @} */

	/** AUTOINC related members. @{ */

	/* The actual collection of tables locked during AUTOINC read/write is
	kept in trx_t. In order to quickly determine whether a transaction has
	locked the AUTOINC lock we keep a pointer to the transaction here in
	the 'autoinc_trx' member. This is to avoid acquiring the
	lock_sys_t::mutex and scanning the vector in trx_t.
	When an AUTOINC lock has to wait, the corresponding lock instance is
	created on the trx lock heap rather than use the pre-allocated instance
	in autoinc_lock below. */

	/** A buffer for an AUTOINC lock for this table. We allocate the
	memory here so that individual transactions can get it and release it
	without a need to allocate space from the lock heap of the trx:
	otherwise the lock heap would grow rapidly if we do a large insert
	from a select. */
	lock_t*					autoinc_lock;

	/** Mutex protecting the autoinc counter and freed_indexes. */
	mysql_mutex_t				autoinc_mutex;

	/** Autoinc counter value to give to the next inserted row. */
	ib_uint64_t				autoinc;

	/** This counter is used to track the number of granted and pending
	autoinc locks on this table. This value is set after acquiring the
	lock_sys_t::mutex but we peek the contents to determine whether other
	transactions have acquired the AUTOINC lock or not. Of course only one
	transaction can be granted the lock but there can be multiple
	waiters. */
	ulong					n_waiting_or_granted_auto_inc_locks;

	/** The transaction that currently holds the the AUTOINC lock on this
	table. Protected by lock_sys->mutex. */
	const trx_t*				autoinc_trx;

	/* @} */

	/** FTS specific state variables. */
	fts_t*					fts;

	/** Quiescing states, protected by the dict_index_t::lock. ie. we can
	only change the state if we acquire all the latches (dict_index_t::lock)
	in X mode of this table's indexes. */
	ib_quiesce_t				quiesce;

	/** Count of the number of record locks on this table. We use this to
	determine whether we can evict the table from the dictionary cache.
	It is protected by lock_sys->mutex. */
	ulint					n_rec_locks;
private:
	/** Count of how many handles are opened to this table. Dropping of the
	table is NOT allowed until this count gets to zero. MySQL does NOT
	itself check the number of open handles at DROP. */
	int32					n_ref_count;

public:
	/** List of locks on the table. Protected by lock_sys->mutex. */
	table_lock_list_t			locks;

	/** Timestamp of the last modification of this table. */
	time_t					update_time;

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

  /** Check whether the table name is same as mysql/innodb_stats_table
  or mysql/innodb_index_stats.
  @return true if the table name is same as stats table */
  bool is_stats_table() const;
};

inline bool table_name_t::is_temporary() const
{
	return dict_table_t::is_temporary_name(m_name);
}

inline bool dict_index_t::is_readable() const
{
	return(UNIV_LIKELY(!table->file_unreadable));
}

inline bool dict_index_t::is_corrupted() const
{
	return UNIV_UNLIKELY(online_status >= ONLINE_INDEX_ABORTED
			     || (type & DICT_CORRUPT)
			     || (table && table->corrupted));
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

/** Release the zip_pad_mutex of a given index.
@param[in,out]	index	index whose zip_pad_mutex is to be released */
inline
void
dict_index_zip_pad_unlock(
	dict_index_t*	index)
{
	mysql_mutex_unlock(&index->zip_pad.mutex);
}

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

#include "dict0mem.inl"

#endif /* dict0mem_h */
