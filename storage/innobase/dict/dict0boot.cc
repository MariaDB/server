/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2021, MariaDB Corporation.

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
@file dict/dict0boot.cc
Data dictionary creation and booting

Created 4/18/1996 Heikki Tuuri
*******************************************************/

#include "dict0boot.h"
#include "dict0crea.h"
#include "btr0btr.h"
#include "dict0load.h"
#include "trx0trx.h"
#include "srv0srv.h"
#include "ibuf0ibuf.h"
#include "buf0flu.h"
#include "log0recv.h"
#include "os0file.h"

/**********************************************************************//**
Returns a new table, index, or space id. */
void
dict_hdr_get_new_id(
/*================*/
	table_id_t*		table_id,	/*!< out: table id
						(not assigned if NULL) */
	index_id_t*		index_id,	/*!< out: index id
						(not assigned if NULL) */
	uint32_t*		space_id)	/*!< out: space id
						(not assigned if NULL) */
{
	ib_id_t		id;
	mtr_t		mtr;

	mtr.start();
	buf_block_t* dict_hdr = dict_hdr_get(&mtr);

	if (table_id) {
		id = mach_read_from_8(DICT_HDR + DICT_HDR_TABLE_ID
				      + dict_hdr->page.frame);
		id++;
		mtr.write<8>(*dict_hdr, DICT_HDR + DICT_HDR_TABLE_ID
			     + dict_hdr->page.frame, id);
		*table_id = id;
	}

	if (index_id) {
		id = mach_read_from_8(DICT_HDR + DICT_HDR_INDEX_ID
				      + dict_hdr->page.frame);
		id++;
		mtr.write<8>(*dict_hdr, DICT_HDR + DICT_HDR_INDEX_ID
			     + dict_hdr->page.frame, id);
		*index_id = id;
	}

	if (space_id) {
		*space_id = mach_read_from_4(DICT_HDR + DICT_HDR_MAX_SPACE_ID
					     + dict_hdr->page.frame);
		if (fil_assign_new_space_id(space_id)) {
			mtr.write<4>(*dict_hdr,
				     DICT_HDR + DICT_HDR_MAX_SPACE_ID
				     + dict_hdr->page.frame, *space_id);
		}
	}

	mtr.commit();
}

/** Update dict_sys.row_id in the dictionary header file page. */
void dict_hdr_flush_row_id(row_id_t id)
{
  mtr_t mtr;
  mtr.start();
  buf_block_t* d= dict_hdr_get(&mtr);
  byte *row_id= DICT_HDR + DICT_HDR_ROW_ID + d->page.frame;
  if (mach_read_from_8(row_id) < id)
    mtr.write<8>(*d, row_id, id);
  mtr.commit();
}

/** Create the DICT_HDR page on database initialization.
@return whether the operation failed */
static bool dict_hdr_create()
{
	buf_block_t*	block;
	ulint		root_page_no;

	bool fail = false;
	mtr_t mtr;
	mtr.start();
	compile_time_assert(DICT_HDR_SPACE == 0);

	/* Create the dictionary header file block in a new, allocated file
	segment in the system tablespace */
	block = fseg_create(fil_system.sys_space,
			    DICT_HDR + DICT_HDR_FSEG_HEADER, &mtr);

	ut_a(block->page.id() == page_id_t(DICT_HDR_SPACE, DICT_HDR_PAGE_NO));

	buf_block_t* d = dict_hdr_get(&mtr);

	/* Start counting row, table, index, and tree ids from
	DICT_HDR_FIRST_ID */
	mtr.write<8>(*d, DICT_HDR + DICT_HDR_ROW_ID + d->page.frame,
		     DICT_HDR_FIRST_ID);
	mtr.write<8>(*d, DICT_HDR + DICT_HDR_TABLE_ID + d->page.frame,
		     DICT_HDR_FIRST_ID);
	mtr.write<8>(*d, DICT_HDR + DICT_HDR_INDEX_ID + d->page.frame,
		     DICT_HDR_FIRST_ID);

	ut_ad(!mach_read_from_4(DICT_HDR + DICT_HDR_MAX_SPACE_ID
				+ d->page.frame));

	/* Obsolete, but we must initialize it anyway. */
	mtr.write<4>(*d, DICT_HDR + DICT_HDR_MIX_ID_LOW + d->page.frame,
		     DICT_HDR_FIRST_ID);

	/* Create the B-tree roots for the clustered indexes of the basic
	system tables */

	/*--------------------------*/
	root_page_no = btr_create(DICT_CLUSTERED | DICT_UNIQUE,
				  fil_system.sys_space, DICT_TABLES_ID,
				  nullptr, &mtr);
	if (root_page_no == FIL_NULL) {
failed:
		fail = true;
		goto func_exit;
	}

	mtr.write<4>(*d, DICT_HDR + DICT_HDR_TABLES + d->page.frame,
		     root_page_no);
	/*--------------------------*/
	root_page_no = btr_create(DICT_UNIQUE,
				  fil_system.sys_space, DICT_TABLE_IDS_ID,
				  nullptr, &mtr);
	if (root_page_no == FIL_NULL) {
		goto failed;
	}

	mtr.write<4>(*d, DICT_HDR + DICT_HDR_TABLE_IDS + d->page.frame,
		     root_page_no);
	/*--------------------------*/
	root_page_no = btr_create(DICT_CLUSTERED | DICT_UNIQUE,
				  fil_system.sys_space, DICT_COLUMNS_ID,
				  nullptr, &mtr);
	if (root_page_no == FIL_NULL) {
		goto failed;
	}

	mtr.write<4>(*d, DICT_HDR + DICT_HDR_COLUMNS + d->page.frame,
		     root_page_no);
	/*--------------------------*/
	root_page_no = btr_create(DICT_CLUSTERED | DICT_UNIQUE,
				  fil_system.sys_space, DICT_INDEXES_ID,
				  nullptr, &mtr);
	if (root_page_no == FIL_NULL) {
		goto failed;
	}

	mtr.write<4>(*d, DICT_HDR + DICT_HDR_INDEXES + d->page.frame,
		     root_page_no);
	/*--------------------------*/
	root_page_no = btr_create(DICT_CLUSTERED | DICT_UNIQUE,
				  fil_system.sys_space, DICT_FIELDS_ID,
				  nullptr, &mtr);
	if (root_page_no == FIL_NULL) {
		goto failed;
	}

	mtr.write<4>(*d, DICT_HDR + DICT_HDR_FIELDS + d->page.frame,
		     root_page_no);
func_exit:
	mtr.commit();
	return fail;
}

/*****************************************************************//**
Initializes the data dictionary memory structures when the database is
started. This function is also called when the data dictionary is created.
@return DB_SUCCESS or error code. */
dberr_t
dict_boot(void)
/*===========*/
{
	dict_table_t*	table;
	dict_index_t*	index;
	mem_heap_t*	heap;
	mtr_t		mtr;

	static_assert(DICT_NUM_COLS__SYS_TABLES == 8, "compatibility");
	static_assert(DICT_NUM_FIELDS__SYS_TABLES == 10, "compatibility");
	static_assert(DICT_NUM_FIELDS__SYS_TABLE_IDS == 2, "compatibility");
	static_assert(DICT_NUM_COLS__SYS_COLUMNS == 7, "compatibility");
	static_assert(DICT_NUM_FIELDS__SYS_COLUMNS == 9, "compatibility");
	static_assert(DICT_NUM_COLS__SYS_INDEXES == 8, "compatibility");
	static_assert(DICT_NUM_FIELDS__SYS_INDEXES == 10, "compatibility");
	static_assert(DICT_NUM_COLS__SYS_FIELDS == 3, "compatibility");
	static_assert(DICT_NUM_FIELDS__SYS_FIELDS == 5, "compatibility");
	static_assert(DICT_NUM_COLS__SYS_FOREIGN == 4, "compatibility");
	static_assert(DICT_NUM_FIELDS__SYS_FOREIGN == 6, "compatibility");
	static_assert(DICT_NUM_FIELDS__SYS_FOREIGN_FOR_NAME == 2,
		      "compatibility");
	static_assert(DICT_NUM_COLS__SYS_FOREIGN_COLS == 4, "compatibility");
	static_assert(DICT_NUM_FIELDS__SYS_FOREIGN_COLS == 6, "compatibility");

	mtr_start(&mtr);

	/* Create the hash tables etc. */
	dict_sys.create();

	heap = mem_heap_create(450);

	dict_sys.lock(SRW_LOCK_CALL);

	/* Get the dictionary header */
	const byte* dict_hdr = &dict_hdr_get(&mtr)->page.frame[DICT_HDR];

	/* Because we only write new row ids to disk-based data structure
	(dictionary header) when it is divisible by
	DICT_HDR_ROW_ID_WRITE_MARGIN, in recovery we will not recover
	the latest value of the row id counter. Therefore we advance
	the counter at the database startup to avoid overlapping values.
	Note that when a user after database startup first time asks for
	a new row id, then because the counter is now divisible by
	..._MARGIN, it will immediately be updated to the disk-based
	header. */

	dict_sys.recover_row_id(mach_read_from_8(dict_hdr + DICT_HDR_ROW_ID));
	if (uint32_t max_space_id
	    = mach_read_from_4(dict_hdr + DICT_HDR_MAX_SPACE_ID)) {
		max_space_id--;
		fil_assign_new_space_id(&max_space_id);
	}

	/* Insert into the dictionary cache the descriptions of the basic
	system tables */
	/*-------------------------*/
	table = dict_table_t::create(dict_sys.SYS_TABLE[dict_sys.SYS_TABLES],
				     fil_system.sys_space,
				     DICT_NUM_COLS__SYS_TABLES, 0, 0, 0);
	dict_mem_table_add_col(table, heap, "NAME", DATA_BINARY, 0,
			       MAX_FULL_NAME_LEN);
	dict_mem_table_add_col(table, heap, "ID", DATA_BINARY, 0, 8);
	/* ROW_FORMAT = (N_COLS >> 31) ? COMPACT : REDUNDANT */
	dict_mem_table_add_col(table, heap, "N_COLS", DATA_INT, 0, 4);
	/* The low order bit of TYPE is always set to 1.  If ROW_FORMAT
	is not REDUNDANT or COMPACT, this field matches table->flags. */
	dict_mem_table_add_col(table, heap, "TYPE", DATA_INT, 0, 4);
	dict_mem_table_add_col(table, heap, "MIX_ID", DATA_BINARY, 0, 0);
	/* MIX_LEN may contain additional table flags when
	ROW_FORMAT!=REDUNDANT. */
	dict_mem_table_add_col(table, heap, "MIX_LEN", DATA_INT, 0, 4);
	dict_mem_table_add_col(table, heap, "CLUSTER_NAME", DATA_BINARY, 0, 0);
	dict_mem_table_add_col(table, heap, "SPACE", DATA_INT, 0, 4);

	table->id = DICT_TABLES_ID;

	dict_table_add_system_columns(table, heap);
	table->add_to_cache();
	dict_sys.sys_tables = table;
	mem_heap_empty(heap);

	index = dict_mem_index_create(table, "CLUST_IND",
				      DICT_UNIQUE | DICT_CLUSTERED, 1);

	dict_mem_index_add_field(index, "NAME", 0);

	index->id = DICT_TABLES_ID;
	dberr_t error = dict_index_add_to_cache(
		index, mach_read_from_4(dict_hdr + DICT_HDR_TABLES));
	ut_a(error == DB_SUCCESS);
	ut_ad(!table->is_instant());
	table->indexes.start->n_core_null_bytes = static_cast<uint8_t>(
		UT_BITS_IN_BYTES(unsigned(table->indexes.start->n_nullable)));

	/*-------------------------*/
	index = dict_mem_index_create(table, "ID_IND", DICT_UNIQUE, 1);
	dict_mem_index_add_field(index, "ID", 0);

	index->id = DICT_TABLE_IDS_ID;
	error = dict_index_add_to_cache(
		index, mach_read_from_4(dict_hdr + DICT_HDR_TABLE_IDS));
	ut_a(error == DB_SUCCESS);

	/*-------------------------*/
	table = dict_table_t::create(dict_sys.SYS_TABLE[dict_sys.SYS_COLUMNS],
				     fil_system.sys_space,
				     DICT_NUM_COLS__SYS_COLUMNS, 0, 0, 0);
	dict_mem_table_add_col(table, heap, "TABLE_ID", DATA_BINARY, 0, 8);
	dict_mem_table_add_col(table, heap, "POS", DATA_INT, 0, 4);
	dict_mem_table_add_col(table, heap, "NAME", DATA_BINARY, 0, 0);
	dict_mem_table_add_col(table, heap, "MTYPE", DATA_INT, 0, 4);
	dict_mem_table_add_col(table, heap, "PRTYPE", DATA_INT, 0, 4);
	dict_mem_table_add_col(table, heap, "LEN", DATA_INT, 0, 4);
	dict_mem_table_add_col(table, heap, "PREC", DATA_INT, 0, 4);

	table->id = DICT_COLUMNS_ID;

	dict_table_add_system_columns(table, heap);
	table->add_to_cache();
	dict_sys.sys_columns = table;
	mem_heap_empty(heap);

	index = dict_mem_index_create(table, "CLUST_IND",
				      DICT_UNIQUE | DICT_CLUSTERED, 2);

	dict_mem_index_add_field(index, "TABLE_ID", 0);
	dict_mem_index_add_field(index, "POS", 0);

	index->id = DICT_COLUMNS_ID;
	error = dict_index_add_to_cache(
		index, mach_read_from_4(dict_hdr + DICT_HDR_COLUMNS));
	ut_a(error == DB_SUCCESS);
	ut_ad(!table->is_instant());
	table->indexes.start->n_core_null_bytes = static_cast<uint8_t>(
		UT_BITS_IN_BYTES(unsigned(table->indexes.start->n_nullable)));

	/*-------------------------*/
	table = dict_table_t::create(dict_sys.SYS_TABLE[dict_sys.SYS_INDEXES],
				     fil_system.sys_space,
				     DICT_NUM_COLS__SYS_INDEXES, 0, 0, 0);

	dict_mem_table_add_col(table, heap, "TABLE_ID", DATA_BINARY, 0, 8);
	dict_mem_table_add_col(table, heap, "ID", DATA_BINARY, 0, 8);
	dict_mem_table_add_col(table, heap, "NAME", DATA_BINARY, 0, 0);
	dict_mem_table_add_col(table, heap, "N_FIELDS", DATA_INT, 0, 4);
	dict_mem_table_add_col(table, heap, "TYPE", DATA_INT, 0, 4);
	/* SYS_INDEXES.SPACE is only read by in dict_drop_index_tree() */
	dict_mem_table_add_col(table, heap, "SPACE", DATA_INT, 0, 4);
	dict_mem_table_add_col(table, heap, "PAGE_NO", DATA_INT, 0, 4);
	dict_mem_table_add_col(table, heap, "MERGE_THRESHOLD", DATA_INT, 0, 4);

	table->id = DICT_INDEXES_ID;

	dict_table_add_system_columns(table, heap);
	/* The column SYS_INDEXES.MERGE_THRESHOLD was "instantly"
	added in MySQL 5.7 and MariaDB 10.2.2. Assign it DEFAULT NULL.
	Because of file format compatibility, we must treat SYS_INDEXES
	as a special case, relaxing some debug assertions
	for DICT_INDEXES_ID. */
	dict_table_get_nth_col(table, DICT_COL__SYS_INDEXES__MERGE_THRESHOLD)
		->def_val.len = UNIV_SQL_NULL;
	table->add_to_cache();
	dict_sys.sys_indexes = table;
	mem_heap_empty(heap);

	index = dict_mem_index_create(table, "CLUST_IND",
				      DICT_UNIQUE | DICT_CLUSTERED, 2);

	dict_mem_index_add_field(index, "TABLE_ID", 0);
	dict_mem_index_add_field(index, "ID", 0);

	index->id = DICT_INDEXES_ID;
	error = dict_index_add_to_cache(
		index, mach_read_from_4(dict_hdr + DICT_HDR_INDEXES));
	ut_a(error == DB_SUCCESS);
	ut_ad(!table->is_instant());
	table->indexes.start->n_core_null_bytes = static_cast<uint8_t>(
		UT_BITS_IN_BYTES(unsigned(table->indexes.start->n_nullable)));

	/*-------------------------*/
	table = dict_table_t::create(dict_sys.SYS_TABLE[dict_sys.SYS_FIELDS],
				     fil_system.sys_space,
				     DICT_NUM_COLS__SYS_FIELDS, 0, 0, 0);
	dict_mem_table_add_col(table, heap, "INDEX_ID", DATA_BINARY, 0, 8);
	dict_mem_table_add_col(table, heap, "POS", DATA_INT, 0, 4);
	dict_mem_table_add_col(table, heap, "COL_NAME", DATA_BINARY, 0, 0);

	table->id = DICT_FIELDS_ID;

	dict_table_add_system_columns(table, heap);
	table->add_to_cache();
	dict_sys.sys_fields = table;
	mem_heap_free(heap);

	index = dict_mem_index_create(table, "CLUST_IND",
				      DICT_UNIQUE | DICT_CLUSTERED, 2);

	dict_mem_index_add_field(index, "INDEX_ID", 0);
	dict_mem_index_add_field(index, "POS", 0);

	index->id = DICT_FIELDS_ID;
	error = dict_index_add_to_cache(
		index, mach_read_from_4(dict_hdr + DICT_HDR_FIELDS));
	ut_a(error == DB_SUCCESS);
	ut_ad(!table->is_instant());
	table->indexes.start->n_core_null_bytes = static_cast<uint8_t>(
		UT_BITS_IN_BYTES(unsigned(table->indexes.start->n_nullable)));

	mtr_commit(&mtr);

	dberr_t	err = ibuf_init_at_db_start();

	if (err == DB_SUCCESS) {
		/* Load definitions of other indexes on system tables */

		dict_load_sys_table(dict_sys.sys_tables);
		dict_load_sys_table(dict_sys.sys_columns);
		dict_load_sys_table(dict_sys.sys_indexes);
		dict_load_sys_table(dict_sys.sys_fields);
		dict_sys.unlock();
		dict_sys.load_sys_tables();
	} else {
		dict_sys.unlock();
	}

	return(err);
}

/*****************************************************************//**
Creates and initializes the data dictionary at the server bootstrap.
@return DB_SUCCESS or error code. */
dberr_t dict_create()
{
  return dict_hdr_create() ? DB_ERROR : dict_boot();
}
