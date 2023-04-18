/*****************************************************************************

Copyright (C) 2013, 2014 Facebook, Inc. All Rights Reserved.
Copyright (C) 2014, 2021, MariaDB Corporation.

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

#ifndef btr0defragment_h
#define btr0defragment_h

#include "btr0pcur.h"

/* Max number of pages to consider at once during defragmentation. */
#define BTR_DEFRAGMENT_MAX_N_PAGES	32

/** stats in btr_defragment */
extern Atomic_counter<ulint> btr_defragment_compression_failures;
extern Atomic_counter<ulint> btr_defragment_failures;
extern Atomic_counter<ulint> btr_defragment_count;

/******************************************************************//**
Initialize defragmentation. */
void
btr_defragment_init(void);
/******************************************************************//**
Shutdown defragmentation. */
void
btr_defragment_shutdown();
/******************************************************************//**
Check whether the given index is in btr_defragment_wq. */
bool
btr_defragment_find_index(
	dict_index_t*	index);	/*!< Index to find. */
/** Defragment an index.
@param pcur      persistent cursor
@param thd       current session, for checking thd_killed()
@return whether the operation was interrupted */
bool btr_defragment_add_index(btr_pcur_t *pcur, THD *thd);
/******************************************************************//**
When table is dropped, this function is called to mark a table as removed in
btr_efragment_wq. The difference between this function and the remove_index
function is this will not NULL the event. */
void
btr_defragment_remove_table(
	dict_table_t*	table);	/*!< Index to be removed. */
/*********************************************************************//**
Check whether we should save defragmentation statistics to persistent storage.*/
void btr_defragment_save_defrag_stats_if_needed(dict_index_t *index);

/* Stop defragmentation.*/
void btr_defragment_end();
extern bool btr_defragment_active;
#endif
