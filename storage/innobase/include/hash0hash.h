/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2020, MariaDB Corporation.

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
@file include/hash0hash.h
The simple hash table utility

Created 5/20/1997 Heikki Tuuri
*******************************************************/

#ifndef hash0hash_h
#define hash0hash_h

#include "mem0mem.h"
#include "sync0rw.h"

struct hash_table_t;
struct hash_cell_t{
	void*	node;	/*!< hash chain node, NULL if none */
};
typedef void*	hash_node_t;

/* Fix Bug #13859: symbol collision between imap/mysql */
#define hash_create hash0_create

/**
Create a hash table.
@param n   the minimum number of hash array elements
@return created table (with n_cells being a prime, at least n) */
hash_table_t *hash_create(ulint n);

/*************************************************************//**
Frees a hash table. */
void
hash_table_free(
/*============*/
	hash_table_t*	table);	/*!< in, own: hash table */

#define hash_calc_hash(FOLD, TABLE) (TABLE)->calc_hash(FOLD)

/*******************************************************************//**
Inserts a struct to a hash table. */

#define HASH_INSERT(TYPE, NAME, TABLE, FOLD, DATA)\
do {\
	hash_cell_t*	cell3333;\
	TYPE*		struct3333;\
\
	(DATA)->NAME = NULL;\
\
	cell3333 = hash_get_nth_cell(TABLE, hash_calc_hash(FOLD, TABLE));\
\
	if (cell3333->node == NULL) {\
		cell3333->node = DATA;\
	} else {\
		struct3333 = (TYPE*) cell3333->node;\
\
		while (struct3333->NAME != NULL) {\
\
			struct3333 = (TYPE*) struct3333->NAME;\
		}\
\
		struct3333->NAME = DATA;\
	}\
} while (0)

/*******************************************************************//**
Inserts a struct to the head of hash table. */

#define HASH_PREPEND(TYPE, NAME, TABLE, FOLD, DATA)	\
do {							\
	hash_cell_t*	cell3333;			\
	TYPE*		struct3333;			\
							\
	(DATA)->NAME = NULL;				\
							\
	cell3333 = hash_get_nth_cell(TABLE, hash_calc_hash(FOLD, TABLE));\
							\
	if (cell3333->node == NULL) {			\
		cell3333->node = DATA;			\
		DATA->NAME = NULL;			\
	} else {					\
		struct3333 = (TYPE*) cell3333->node;	\
							\
		DATA->NAME = struct3333;		\
							\
		cell3333->node = DATA;			\
	}						\
} while (0)
#ifdef UNIV_HASH_DEBUG
# define HASH_ASSERT_VALID(DATA) ut_a((void*) (DATA) != (void*) -1)
# define HASH_INVALIDATE(DATA, NAME) *(void**) (&DATA->NAME) = (void*) -1
#else
# define HASH_ASSERT_VALID(DATA) do {} while (0)
# define HASH_INVALIDATE(DATA, NAME) do {} while (0)
#endif

/*******************************************************************//**
Deletes a struct from a hash table. */

#define HASH_DELETE(TYPE, NAME, TABLE, FOLD, DATA)\
do {\
	hash_cell_t*	cell3333;\
	TYPE*		struct3333;\
\
	cell3333 = hash_get_nth_cell(TABLE, hash_calc_hash(FOLD, TABLE));\
\
	if (cell3333->node == DATA) {\
		HASH_ASSERT_VALID(DATA->NAME);\
		cell3333->node = DATA->NAME;\
	} else {\
		struct3333 = (TYPE*) cell3333->node;\
\
		while (struct3333->NAME != DATA) {\
\
			struct3333 = (TYPE*) struct3333->NAME;\
			ut_a(struct3333);\
		}\
\
		struct3333->NAME = DATA->NAME;\
	}\
	HASH_INVALIDATE(DATA, NAME);\
} while (0)

#define HASH_REPLACE(TYPE, NAME, TABLE, FOLD, DATA_OLD, DATA_NEW)             \
	do {                                                                  \
		(DATA_NEW)->NAME = (DATA_OLD)->NAME;                          \
                                                                              \
		hash_cell_t& cell3333                                         \
			= TABLE->array[hash_calc_hash(FOLD, TABLE)];          \
		TYPE** struct3333 = (TYPE**)&cell3333.node;                   \
		while (*struct3333 != DATA_OLD) {                             \
			struct3333 = &((*struct3333)->NAME);                  \
		}                                                             \
		*struct3333 = DATA_NEW;                                       \
	} while (0)
/*******************************************************************//**
Gets the first struct in a hash chain, NULL if none. */

#define HASH_GET_FIRST(TABLE, HASH_VAL)\
	(hash_get_nth_cell(TABLE, HASH_VAL)->node)

/*******************************************************************//**
Gets the next struct in a hash chain, NULL if none. */

#define HASH_GET_NEXT(NAME, DATA)	((DATA)->NAME)

/********************************************************************//**
Looks for a struct in a hash table. */
#define HASH_SEARCH(NAME, TABLE, FOLD, TYPE, DATA, ASSERTION, TEST)\
{\
	(DATA) = (TYPE) HASH_GET_FIRST(TABLE, hash_calc_hash(FOLD, TABLE));\
	HASH_ASSERT_VALID(DATA);\
\
	while ((DATA) != NULL) {\
		ASSERTION;\
		if (TEST) {\
			break;\
		} else {\
			HASH_ASSERT_VALID(HASH_GET_NEXT(NAME, DATA));\
			(DATA) = (TYPE) HASH_GET_NEXT(NAME, DATA);\
		}\
	}\
}

/********************************************************************//**
Looks for an item in all hash buckets. */
#define HASH_SEARCH_ALL(NAME, TABLE, TYPE, DATA, ASSERTION, TEST)	\
do {									\
	ulint	i3333;							\
									\
	for (i3333 = (TABLE)->n_cells; i3333--; ) {			\
		(DATA) = (TYPE) HASH_GET_FIRST(TABLE, i3333);		\
									\
		while ((DATA) != NULL) {				\
			HASH_ASSERT_VALID(DATA);			\
			ASSERTION;					\
									\
			if (TEST) {					\
				break;					\
			}						\
									\
			(DATA) = (TYPE) HASH_GET_NEXT(NAME, DATA);	\
		}							\
									\
		if ((DATA) != NULL) {					\
			break;						\
		}							\
	}								\
} while (0)

/************************************************************//**
Gets the nth cell in a hash table.
@return pointer to cell */
UNIV_INLINE
hash_cell_t*
hash_get_nth_cell(
/*==============*/
	hash_table_t*	table,	/*!< in: hash table */
	ulint		n);	/*!< in: cell index */

/*************************************************************//**
Clears a hash table so that all the cells become empty. */
UNIV_INLINE
void
hash_table_clear(
/*=============*/
	hash_table_t*	table);	/*!< in/out: hash table */

/*************************************************************//**
Returns the number of cells in a hash table.
@return number of cells */
UNIV_INLINE
ulint
hash_get_n_cells(
/*=============*/
	hash_table_t*	table);	/*!< in: table */

/****************************************************************//**
Move all hash table entries from OLD_TABLE to NEW_TABLE. */

#define HASH_MIGRATE(OLD_TABLE, NEW_TABLE, NODE_TYPE, PTR_NAME, FOLD_FUNC) \
do {\
	ulint		i2222;\
	ulint		cell_count2222;\
\
	cell_count2222 = hash_get_n_cells(OLD_TABLE);\
\
	for (i2222 = 0; i2222 < cell_count2222; i2222++) {\
		NODE_TYPE*	node2222 = static_cast<NODE_TYPE*>(\
			HASH_GET_FIRST((OLD_TABLE), i2222));\
\
		while (node2222) {\
			NODE_TYPE*	next2222 = static_cast<NODE_TYPE*>(\
				node2222->PTR_NAME);\
			ulint		fold2222 = FOLD_FUNC(node2222);\
\
			HASH_INSERT(NODE_TYPE, PTR_NAME, (NEW_TABLE),\
				fold2222, node2222);\
\
			node2222 = next2222;\
		}\
	}\
} while (0)

/* The hash table structure */
struct hash_table_t {
#ifdef BTR_CUR_HASH_ADAPT
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	ibool			adaptive;/* TRUE if this is the hash
					table of the adaptive hash
					index */
# endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
#endif /* BTR_CUR_HASH_ADAPT */
	ulint			n_cells;/* number of cells in the hash table */
	hash_cell_t*		array;	/*!< pointer to cell array */
	mem_heap_t*		heap;
#ifdef UNIV_DEBUG
	ulint			magic_n;
# define HASH_TABLE_MAGIC_N	76561114
#endif /* UNIV_DEBUG */

  ulint calc_hash(ulint fold) const { return ut_hash_ulint(fold, n_cells); }
};

#include "hash0hash.ic"

#endif
