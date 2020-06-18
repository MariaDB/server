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

#pragma once
#include "ut0rnd.h"

struct hash_table_t;
struct hash_cell_t{
	void*	node;	/*!< hash chain node, NULL if none */
};
typedef void*	hash_node_t;

/*******************************************************************//**
Inserts a struct to a hash table. */

#define HASH_INSERT(TYPE, NAME, TABLE, FOLD, DATA)\
do {\
	hash_cell_t*	cell3333;\
	TYPE*		struct3333;\
\
	(DATA)->NAME = NULL;\
\
	cell3333 = &(TABLE)->array[(TABLE)->calc_hash(FOLD)];	\
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
	cell3333 = &(TABLE)->array[(TABLE)->calc_hash(FOLD)];	\
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
	cell3333 = &(TABLE)->array[(TABLE)->calc_hash(FOLD)]; \
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
			= (TABLE)->array[(TABLE)->calc_hash(FOLD)]; \
		TYPE** struct3333 = (TYPE**)&cell3333.node;                   \
		while (*struct3333 != DATA_OLD) {                             \
			struct3333 = &((*struct3333)->NAME);                  \
		}                                                             \
		*struct3333 = DATA_NEW;                                       \
	} while (0)
/*******************************************************************//**
Gets the first struct in a hash chain, NULL if none. */

#define HASH_GET_FIRST(TABLE, HASH_VAL) (TABLE)->array[HASH_VAL].node

/*******************************************************************//**
Gets the next struct in a hash chain, NULL if none. */

#define HASH_GET_NEXT(NAME, DATA)	((DATA)->NAME)

/********************************************************************//**
Looks for a struct in a hash table. */
#define HASH_SEARCH(NAME, TABLE, FOLD, TYPE, DATA, ASSERTION, TEST)\
{\
	(DATA) = (TYPE) HASH_GET_FIRST(TABLE, (TABLE)->calc_hash(FOLD)); \
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

/****************************************************************//**
Move all hash table entries from OLD_TABLE to NEW_TABLE. */

#define HASH_MIGRATE(OLD_TABLE, NEW_TABLE, NODE_TYPE, PTR_NAME, FOLD_FUNC) \
do {\
	ulint		i2222;\
	ulint		cell_count2222;\
\
	cell_count2222 = (OLD_TABLE)->n_cells;	\
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

/** Hash table with singly-linked overflow lists */
struct hash_table_t
{
  /** number of elements in array (a prime number) */
  ulint n_cells;
  /** the hash array */
  hash_cell_t *array;

  /** Create the hash table.
  @param n  the lower bound of n_cells */
  void create(ulint n)
  {
    n_cells= ut_find_prime(n);
    array= static_cast<hash_cell_t*>(ut_zalloc_nokey(n_cells * sizeof *array));
  }

  /** Clear the hash table. */
  void clear() { memset(array, 0, n_cells * sizeof *array); }

  /** Free the hash table. */
  void free() { ut_free(array); array= nullptr; }

  ulint calc_hash(ulint fold) const { return ut_hash_ulint(fold, n_cells); }
};
