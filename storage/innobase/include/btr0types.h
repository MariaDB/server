/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2019, MariaDB Corporation.

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

/********************************************************************//**
@file include/btr0types.h
The index tree general types

Created 2/17/1996 Heikki Tuuri
*************************************************************************/

#ifndef btr0types_h
#define btr0types_h

#include "page0types.h"
#include "rem0types.h"

/** Persistent cursor */
struct btr_pcur_t;
/** B-tree cursor */
struct btr_cur_t;
/** B-tree search information for the adaptive hash index */
struct btr_search_t;

#ifdef BTR_CUR_HASH_ADAPT
/** Is search system enabled.
Search system is protected by array of latches. */
extern char	btr_search_enabled;

/** Number of adaptive hash index partition. */
extern ulong	btr_ahi_parts;
#endif /* BTR_CUR_HASH_ADAPT */

/** The size of a reference to data stored on a different page.
The reference is stored at the end of the prefix of the field
in the index record. */
#define FIELD_REF_SIZE			20U
#define BTR_EXTERN_FIELD_REF_SIZE	FIELD_REF_SIZE

/** If the data don't exceed the size, the data are stored locally. */
#define BTR_EXTERN_LOCAL_STORED_MAX_SIZE	\
	(BTR_EXTERN_FIELD_REF_SIZE * 2)

#endif
