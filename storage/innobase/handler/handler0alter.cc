/*****************************************************************************

Copyright (c) 2005, 2019, Oracle and/or its affiliates. All Rights Reserved.
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
@file handler/handler0alter.cc
Smart ALTER TABLE
*******************************************************/

/* Include necessary SQL headers */
#include "univ.i"
#include <debug_sync.h>
#include <log.h>
#include <sql_lex.h>
#include <sql_class.h>
#include <sql_table.h>
#include <mysql/plugin.h>

/* Include necessary InnoDB headers */
#include "btr0sea.h"
#include "dict0crea.h"
#include "dict0dict.h"
#include "dict0load.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"
#include "log0log.h"
#include "rem0types.h"
#include "row0log.h"
#include "row0merge.h"
#include "row0ins.h"
#include "row0row.h"
#include "row0upd.h"
#include "trx0trx.h"
#include "trx0purge.h"
#include "handler0alter.h"
#include "srv0mon.h"
#include "srv0srv.h"
#include "fts0priv.h"
#include "fts0plugin.h"
#include "pars0pars.h"
#include "row0sel.h"
#include "ha_innodb.h"
#include "ut0stage.h"
#include <thread>
#include <sstream>

/** File format constraint for ALTER TABLE */
extern ulong innodb_instant_alter_column_allowed;

static const char *MSG_UNSUPPORTED_ALTER_ONLINE_ON_VIRTUAL_COLUMN=
			"INPLACE ADD or DROP of virtual columns cannot be "
			"combined with other ALTER TABLE actions";

/** Operations for creating secondary indexes (no rebuild needed) */
static const alter_table_operations INNOBASE_ONLINE_CREATE
	= ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX
	| ALTER_ADD_UNIQUE_INDEX;

/** Operations that require filling in default values for columns */
static const alter_table_operations INNOBASE_DEFAULTS
	= ALTER_COLUMN_NOT_NULLABLE
	| ALTER_ADD_STORED_BASE_COLUMN;


/** Operations that require knowledge about row_start, row_end values */
static const alter_table_operations INNOBASE_ALTER_VERSIONED_REBUILD
	= ALTER_ADD_SYSTEM_VERSIONING
	| ALTER_DROP_SYSTEM_VERSIONING;

/** Operations for rebuilding a table in place */
static const alter_table_operations INNOBASE_ALTER_REBUILD
	= ALTER_ADD_PK_INDEX
	| ALTER_DROP_PK_INDEX
	| ALTER_OPTIONS
	/* ALTER_OPTIONS needs to check alter_options_need_rebuild() */
	| ALTER_COLUMN_NULLABLE
	| INNOBASE_DEFAULTS
	| ALTER_STORED_COLUMN_ORDER
	| ALTER_DROP_STORED_COLUMN
	| ALTER_RECREATE_TABLE
	/*
	| ALTER_STORED_COLUMN_TYPE
	*/
	| INNOBASE_ALTER_VERSIONED_REBUILD
	;

/** Operations that require changes to data */
static const alter_table_operations INNOBASE_ALTER_DATA
	= INNOBASE_ONLINE_CREATE | INNOBASE_ALTER_REBUILD;

/** Operations for altering a table that InnoDB does not care about */
static const alter_table_operations INNOBASE_INPLACE_IGNORE
	= ALTER_COLUMN_DEFAULT
	| ALTER_PARTITIONED
	| ALTER_COLUMN_COLUMN_FORMAT
	| ALTER_COLUMN_STORAGE_TYPE
	| ALTER_CONVERT_TO
	| ALTER_VIRTUAL_GCOL_EXPR
	| ALTER_DROP_CHECK_CONSTRAINT
	| ALTER_RENAME
	| ALTER_INDEX_ORDER
	| ALTER_COLUMN_INDEX_LENGTH
	| ALTER_CHANGE_INDEX_COMMENT
	| ALTER_INDEX_IGNORABILITY;

/** Operations on foreign key definitions (changing the schema only) */
static const alter_table_operations INNOBASE_FOREIGN_OPERATIONS
	= ALTER_DROP_FOREIGN_KEY
	| ALTER_ADD_FOREIGN_KEY;

/** Operations that InnoDB cares about and can perform without creating data */
static const alter_table_operations INNOBASE_ALTER_NOCREATE
	= ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX
	| ALTER_DROP_UNIQUE_INDEX;

/** Operations that InnoDB cares about and can perform without validation */
static const alter_table_operations INNOBASE_ALTER_NOVALIDATE
	= INNOBASE_ALTER_NOCREATE
	| ALTER_VIRTUAL_COLUMN_ORDER
	| ALTER_COLUMN_NAME
	| INNOBASE_FOREIGN_OPERATIONS
	| ALTER_COLUMN_UNVERSIONED
	| ALTER_DROP_VIRTUAL_COLUMN;

/** Operations that InnoDB cares about and can perform without rebuild */
static const alter_table_operations INNOBASE_ALTER_NOREBUILD
	= INNOBASE_ONLINE_CREATE
	| INNOBASE_ALTER_NOCREATE;

/** Operations that can be performed instantly, without inplace_alter_table() */
static const alter_table_operations INNOBASE_ALTER_INSTANT
	= ALTER_VIRTUAL_COLUMN_ORDER
	| ALTER_COLUMN_NAME
	| ALTER_ADD_VIRTUAL_COLUMN
	| INNOBASE_FOREIGN_OPERATIONS
	| ALTER_COLUMN_TYPE_CHANGE_BY_ENGINE
	| ALTER_COLUMN_UNVERSIONED
	| ALTER_RENAME_INDEX
	| ALTER_DROP_VIRTUAL_COLUMN;

/** Initialize instant->field_map.
@param[in]	table	table definition to copy from */
inline void dict_table_t::init_instant(const dict_table_t& table)
{
	const dict_index_t& oindex __attribute__((unused))= *table.indexes.start;
	dict_index_t& index = *indexes.start;
	const unsigned u = index.first_user_field();
	DBUG_ASSERT(u == oindex.first_user_field());
	DBUG_ASSERT(index.n_fields >= oindex.n_fields);

	field_map_element_t* field_map_it = static_cast<field_map_element_t*>(
		mem_heap_zalloc(heap, (index.n_fields - u)
				* sizeof *field_map_it));
	instant->field_map = field_map_it;

	ut_d(unsigned n_drop = 0);
	ut_d(unsigned n_nullable = 0);
	for (unsigned i = u; i < index.n_fields; i++) {
		auto& f = index.fields[i];
		ut_d(n_nullable += f.col->is_nullable());

		if (!f.col->is_dropped()) {
			(*field_map_it++).set_ind(f.col->ind);
			continue;
		}

		auto fixed_len = dict_col_get_fixed_size(
			f.col, not_redundant());
		field_map_it->set_dropped();
		if (!f.col->is_nullable()) {
			field_map_it->set_not_null();
		}
		field_map_it->set_ind(fixed_len
				      ? uint16_t(fixed_len + 1)
				      : DATA_BIG_COL(f.col));
		field_map_it++;
		ut_ad(f.col >= table.instant->dropped);
		ut_ad(f.col < table.instant->dropped
		      + table.instant->n_dropped);
		ut_d(n_drop++);
		size_t d = f.col - table.instant->dropped;
		ut_ad(f.col == &table.instant->dropped[d]);
		ut_ad(d <= instant->n_dropped);
		f.col = &instant->dropped[d];
	}
	ut_ad(n_drop == n_dropped());
	ut_ad(field_map_it == &instant->field_map[index.n_fields - u]);
	ut_ad(index.n_nullable == n_nullable);
}

/** Set is_instant() before instant_column().
@param[in]	old		previous table definition
@param[in]	col_map		map from old.cols[] and old.v_cols[] to this
@param[out]	first_alter_pos	0, or 1 + first changed column position */
inline void dict_table_t::prepare_instant(const dict_table_t& old,
					  const ulint* col_map,
					  unsigned& first_alter_pos)
{
	DBUG_ASSERT(!is_instant());
	DBUG_ASSERT(n_dropped() == 0);
	DBUG_ASSERT(old.n_cols == old.n_def);
	DBUG_ASSERT(n_cols == n_def);
	DBUG_ASSERT(old.supports_instant());
	DBUG_ASSERT(not_redundant() == old.not_redundant());
	DBUG_ASSERT(DICT_TF_HAS_ATOMIC_BLOBS(flags)
		    == DICT_TF_HAS_ATOMIC_BLOBS(old.flags));
	DBUG_ASSERT(!persistent_autoinc
		    || persistent_autoinc == old.persistent_autoinc);
	/* supports_instant() does not necessarily hold here,
	in case ROW_FORMAT=COMPRESSED according to the
	MariaDB data dictionary, and ALTER_OPTIONS was not set.
	If that is the case, the instant ALTER TABLE would keep
	the InnoDB table in its current format. */

	const dict_index_t& oindex = *old.indexes.start;
	dict_index_t& index = *indexes.start;
	first_alter_pos = 0;

	for (unsigned i = 0; i + DATA_N_SYS_COLS < old.n_cols; i++) {
		if (col_map[i] != i) {
			first_alter_pos = 1 + i;
			goto add_metadata;
		}
	}

	if (!old.instant) {
		/* Columns were not dropped or reordered.
		Therefore columns must have been added at the end,
		or modified instantly in place. */
		DBUG_ASSERT(index.n_fields >= oindex.n_fields);
		DBUG_ASSERT(index.n_fields > oindex.n_fields
			    || !not_redundant());
#ifdef UNIV_DEBUG
		if (index.n_fields == oindex.n_fields) {
			ut_ad(!not_redundant());
			for (unsigned i = index.n_fields; i--; ) {
				ut_ad(index.fields[i].col->same_format(
					      *oindex.fields[i].col));
			}
		}
#endif
set_core_fields:
		index.n_core_fields = oindex.n_core_fields;
		index.n_core_null_bytes = oindex.n_core_null_bytes;
	} else {
add_metadata:
		const unsigned n_old_drop = old.n_dropped();
		unsigned n_drop = n_old_drop;
		for (unsigned i = old.n_cols; i--; ) {
			if (col_map[i] == ULINT_UNDEFINED) {
				DBUG_ASSERT(i + DATA_N_SYS_COLS
					    < uint(old.n_cols));
				n_drop++;
			}
		}

		instant = new (mem_heap_alloc(heap, sizeof(dict_instant_t)))
			dict_instant_t();
		instant->n_dropped = n_drop;
		if (n_drop) {
			instant->dropped
				= static_cast<dict_col_t*>(
					mem_heap_alloc(heap, n_drop
						       * sizeof(dict_col_t)));
			if (n_old_drop) {
				memcpy(instant->dropped, old.instant->dropped,
				       n_old_drop * sizeof(dict_col_t));
			}
		} else {
			instant->dropped = NULL;
		}

		for (unsigned i = 0, d = n_old_drop; i < old.n_cols; i++) {
			if (col_map[i] == ULINT_UNDEFINED) {
				(new (&instant->dropped[d++])
				 dict_col_t(old.cols[i]))->set_dropped();
			}
		}
#ifndef DBUG_OFF
		for (unsigned i = 0; i < n_drop; i++) {
			DBUG_ASSERT(instant->dropped[i].is_dropped());
		}
#endif
		const unsigned n_fields = index.n_fields + n_dropped();

		DBUG_ASSERT(n_fields >= oindex.n_fields);
		dict_field_t* fields = static_cast<dict_field_t*>(
			mem_heap_zalloc(heap, n_fields * sizeof *fields));
		unsigned i = 0, j = 0, n_nullable = 0;
		ut_d(uint core_null = 0);
		for (; i < oindex.n_fields; i++) {
			DBUG_ASSERT(j <= i);
			dict_field_t&f = fields[i] = oindex.fields[i];
			if (f.col->is_dropped()) {
				/* The column has been instantly
				dropped earlier. */
				DBUG_ASSERT(f.col >= old.instant->dropped);
				{
					size_t d = f.col
						- old.instant->dropped;
					DBUG_ASSERT(d < n_old_drop);
					DBUG_ASSERT(&old.instant->dropped[d]
						    == f.col);
					DBUG_ASSERT(!f.name);
					f.col = instant->dropped + d;
				}
				if (f.col->is_nullable()) {
found_nullable:
					n_nullable++;
					ut_d(core_null
					     += i < oindex.n_core_fields);
				}
				continue;
			}

			const ulint col_ind = col_map[f.col->ind];
			if (col_ind != ULINT_UNDEFINED) {
				if (index.fields[j].col->ind != col_ind) {
					/* The fields for instantly
					added columns must be placed
					last in the clustered index.
					Keep pre-existing fields in
					the same position. */
					uint k;
					for (k = j + 1; k < index.n_fields;
					     k++) {
						if (index.fields[k].col->ind
						    == col_ind) {
							goto found_j;
						}
					}
					DBUG_ASSERT("no such col" == 0);
found_j:
					std::swap(index.fields[j],
						  index.fields[k]);
				}
				DBUG_ASSERT(index.fields[j].col->ind
					    == col_ind);
				fields[i] = index.fields[j++];
				DBUG_ASSERT(!fields[i].col->is_dropped());
				DBUG_ASSERT(fields[i].name
					    == fields[i].col->name(*this));
				if (fields[i].col->is_nullable()) {
					goto found_nullable;
				}
				continue;
			}

			/* This column is being dropped. */
			unsigned d = n_old_drop;
			for (unsigned c = 0; c < f.col->ind; c++) {
				d += col_map[c] == ULINT_UNDEFINED;
			}
			DBUG_ASSERT(d < n_drop);
			f.col = &instant->dropped[d];
			f.name = NULL;
			if (f.col->is_nullable()) {
				goto found_nullable;
			}
		}
		/* The n_core_null_bytes only matters for
		ROW_FORMAT=COMPACT and ROW_FORMAT=DYNAMIC tables. */
		ut_ad(UT_BITS_IN_BYTES(core_null) == oindex.n_core_null_bytes
		      || !not_redundant());
		DBUG_ASSERT(i >= oindex.n_core_fields);
		DBUG_ASSERT(j <= i);
		DBUG_ASSERT(n_fields - (i - j) == index.n_fields);
		std::sort(index.fields + j, index.fields + index.n_fields,
			  [](const dict_field_t& a, const dict_field_t& b)
			  { return a.col->ind < b.col->ind; });
		for (; i < n_fields; i++) {
			fields[i] = index.fields[j++];
			n_nullable += fields[i].col->is_nullable();
			DBUG_ASSERT(!fields[i].col->is_dropped());
			DBUG_ASSERT(fields[i].name
				    == fields[i].col->name(*this));
		}
		DBUG_ASSERT(j == index.n_fields);
		index.n_fields = index.n_def = n_fields
			& dict_index_t::MAX_N_FIELDS;
		index.fields = fields;
		DBUG_ASSERT(n_nullable >= index.n_nullable);
		DBUG_ASSERT(n_nullable >= oindex.n_nullable);
		index.n_nullable = n_nullable & dict_index_t::MAX_N_FIELDS;
		goto set_core_fields;
	}

	DBUG_ASSERT(n_cols + n_dropped() >= old.n_cols + old.n_dropped());
	DBUG_ASSERT(n_dropped() >= old.n_dropped());
	DBUG_ASSERT(index.n_core_fields == oindex.n_core_fields);
	DBUG_ASSERT(index.n_core_null_bytes == oindex.n_core_null_bytes);
}

/** Adjust index metadata for instant ADD/DROP/reorder COLUMN.
@param[in]	clustered index definition after instant ALTER TABLE */
inline void dict_index_t::instant_add_field(const dict_index_t& instant)
{
	DBUG_ASSERT(is_primary());
	DBUG_ASSERT(instant.is_primary());
	DBUG_ASSERT(!has_virtual());
	DBUG_ASSERT(!instant.has_virtual());
	DBUG_ASSERT(instant.n_core_fields <= instant.n_fields);
	DBUG_ASSERT(n_def == n_fields);
	DBUG_ASSERT(instant.n_def == instant.n_fields);
	DBUG_ASSERT(type == instant.type);
	DBUG_ASSERT(trx_id_offset == instant.trx_id_offset);
	DBUG_ASSERT(n_user_defined_cols == instant.n_user_defined_cols);
	DBUG_ASSERT(n_uniq == instant.n_uniq);
	DBUG_ASSERT(instant.n_fields >= n_fields);
	DBUG_ASSERT(instant.n_nullable >= n_nullable);
	DBUG_ASSERT(instant.n_core_fields == n_core_fields);
	DBUG_ASSERT(instant.n_core_null_bytes == n_core_null_bytes);

	/* instant will have all fields (including ones for columns
	that have been or are being instantly dropped) in the same position
	as this index. Fields for any added columns are appended at the end. */
#ifndef DBUG_OFF
	for (unsigned i = 0; i < n_fields; i++) {
		DBUG_ASSERT(fields[i].same(instant.fields[i]));
		DBUG_ASSERT(instant.fields[i].col->same_format(*fields[i]
							       .col));
		/* Instant conversion from NULL to NOT NULL is not allowed. */
		DBUG_ASSERT(!fields[i].col->is_nullable()
			    || instant.fields[i].col->is_nullable());
		DBUG_ASSERT(fields[i].col->is_nullable()
			    == instant.fields[i].col->is_nullable()
			    || !table->not_redundant());
	}
#endif
	n_fields = instant.n_fields;
	n_def = instant.n_def;
	n_nullable = instant.n_nullable;
	fields = static_cast<dict_field_t*>(
		mem_heap_dup(heap, instant.fields, n_fields * sizeof *fields));

	ut_d(unsigned n_null = 0);
	ut_d(unsigned n_dropped = 0);

	for (unsigned i = 0; i < n_fields; i++) {
		const dict_col_t* icol = instant.fields[i].col;
		dict_field_t& f = fields[i];
		ut_d(n_null += icol->is_nullable());
		DBUG_ASSERT(!icol->is_virtual());
		if (icol->is_dropped()) {
			ut_d(n_dropped++);
			f.col->set_dropped();
			f.name = NULL;
		} else {
			f.col = &table->cols[icol - instant.table->cols];
			f.name = f.col->name(*table);
		}
	}

	ut_ad(n_null == n_nullable);
	ut_ad(n_dropped == instant.table->n_dropped());
}

/** Adjust table metadata for instant ADD/DROP/reorder COLUMN.
@param[in]	table	altered table (with dropped columns)
@param[in]	col_map	mapping from cols[] and v_cols[] to table
@return		whether the metadata record must be updated */
inline bool dict_table_t::instant_column(const dict_table_t& table,
					 const ulint* col_map)
{
	DBUG_ASSERT(!table.cached);
	DBUG_ASSERT(table.n_def == table.n_cols);
	DBUG_ASSERT(table.n_t_def == table.n_t_cols);
	DBUG_ASSERT(n_def == n_cols);
	DBUG_ASSERT(n_t_def == n_t_cols);
	DBUG_ASSERT(n_v_def == n_v_cols);
	DBUG_ASSERT(table.n_v_def == table.n_v_cols);
	DBUG_ASSERT(table.n_cols + table.n_dropped() >= n_cols + n_dropped());
	DBUG_ASSERT(!table.persistent_autoinc
		    || persistent_autoinc == table.persistent_autoinc);
	ut_ad(dict_sys.locked());

	{
		const char* end = table.col_names;
		for (unsigned i = table.n_cols; i--; ) end += strlen(end) + 1;

		col_names = static_cast<char*>(
			mem_heap_dup(heap, table.col_names,
				     ulint(end - table.col_names)));
	}
	const dict_col_t* const old_cols = cols;
	cols = static_cast<dict_col_t*>(mem_heap_dup(heap, table.cols,
						     table.n_cols
						     * sizeof *cols));

	/* Preserve the default values of previously instantly added
	columns, or copy the new default values to this->heap. */
	for (uint16_t i = 0; i < table.n_cols; i++) {
		dict_col_t& c = cols[i];

		if (const dict_col_t* o = find(old_cols, col_map, n_cols, i)) {
			c.def_val = o->def_val;
			DBUG_ASSERT(!((c.prtype ^ o->prtype)
				      & ~(DATA_NOT_NULL | DATA_VERSIONED
					  | CHAR_COLL_MASK << 16
					  | DATA_LONG_TRUE_VARCHAR)));
			DBUG_ASSERT(c.same_type(*o));
			DBUG_ASSERT(c.len >= o->len);

			if (o->vers_sys_start()) {
				ut_ad(o->ind == vers_start);
				vers_start = i & dict_index_t::MAX_N_FIELDS;
			} else if (o->vers_sys_end()) {
				ut_ad(o->ind == vers_end);
				vers_end = i & dict_index_t::MAX_N_FIELDS;
			}
			continue;
		}

		DBUG_ASSERT(c.is_added());
		if (c.def_val.len <= UNIV_PAGE_SIZE_MAX
		    && (!c.def_val.len
			|| !memcmp(c.def_val.data, field_ref_zero,
				   c.def_val.len))) {
			c.def_val.data = field_ref_zero;
		} else if (const void*& d = c.def_val.data) {
			d = mem_heap_dup(heap, d, c.def_val.len);
		} else {
			DBUG_ASSERT(c.def_val.len == UNIV_SQL_NULL);
		}
	}

	n_t_def = (n_t_def + (table.n_cols - n_cols))
		& dict_index_t::MAX_N_FIELDS;
	n_t_cols = (n_t_cols + (table.n_cols - n_cols))
		& dict_index_t::MAX_N_FIELDS;
	n_def = table.n_cols;

	const dict_v_col_t* const old_v_cols = v_cols;

	if (const char* end = table.v_col_names) {
		for (unsigned i = table.n_v_cols; i--; ) {
			end += strlen(end) + 1;
		}

		v_col_names = static_cast<char*>(
			mem_heap_dup(heap, table.v_col_names,
				     ulint(end - table.v_col_names)));
		v_cols = static_cast<dict_v_col_t*>(
			mem_heap_alloc(heap, table.n_v_cols * sizeof(*v_cols)));
		for (ulint i = table.n_v_cols; i--; ) {
			new (&v_cols[i]) dict_v_col_t(table.v_cols[i]);
			v_cols[i].v_indexes.clear();
		}
	} else {
		ut_ad(table.n_v_cols == 0);
		v_col_names = NULL;
		v_cols = NULL;
	}

	n_t_def = (n_t_def + (table.n_v_cols - n_v_cols))
		& dict_index_t::MAX_N_FIELDS;
	n_t_cols = (n_t_cols + (table.n_v_cols - n_v_cols))
		& dict_index_t::MAX_N_FIELDS;
	n_v_def = table.n_v_cols;

	for (unsigned i = 0; i < n_v_def; i++) {
		dict_v_col_t& v = v_cols[i];
		DBUG_ASSERT(v.v_indexes.empty());
		v.base_col = static_cast<dict_col_t**>(
			mem_heap_dup(heap, v.base_col,
				     v.num_base * sizeof *v.base_col));

		for (ulint n = v.num_base; n--; ) {
			dict_col_t*& base = v.base_col[n];
			if (base->is_virtual()) {
			} else if (base >= table.cols
				   && base < table.cols + table.n_cols) {
				/* The base column was instantly added. */
				size_t c = base - table.cols;
				DBUG_ASSERT(base == &table.cols[c]);
				base = &cols[c];
			} else {
				DBUG_ASSERT(base >= old_cols);
				size_t c = base - old_cols;
				DBUG_ASSERT(c + DATA_N_SYS_COLS < n_cols);
				DBUG_ASSERT(base == &old_cols[c]);
				DBUG_ASSERT(col_map[c] + DATA_N_SYS_COLS
					    < n_cols);
				base = &cols[col_map[c]];
			}
		}
	}

	dict_index_t* index = dict_table_get_first_index(this);
	bool metadata_changed;
	{
		const dict_index_t& i = *dict_table_get_first_index(&table);
		metadata_changed = i.n_fields > index->n_fields;
		ut_ad(i.n_fields >= index->n_fields);
		index->instant_add_field(i);
	}

	if (instant || table.instant) {
		const auto old_instant = instant;
		/* FIXME: add instant->heap, and transfer ownership here */
		if (!instant) {
			instant = new (mem_heap_zalloc(heap, sizeof *instant))
				dict_instant_t();
			goto dup_dropped;
		} else if (n_dropped() < table.n_dropped()) {
dup_dropped:
			instant->dropped = static_cast<dict_col_t*>(
				mem_heap_dup(heap, table.instant->dropped,
					     table.instant->n_dropped
					     * sizeof *instant->dropped));
			instant->n_dropped = table.instant->n_dropped;
		} else if (table.instant->n_dropped) {
			memcpy(instant->dropped, table.instant->dropped,
			       table.instant->n_dropped
			       * sizeof *instant->dropped);
		}

		const field_map_element_t* field_map = old_instant
			? old_instant->field_map : NULL;

		init_instant(table);

		if (!metadata_changed) {
			metadata_changed = !field_map
				|| memcmp(field_map,
					  instant->field_map,
					  (index->n_fields
					   - index->first_user_field())
					  * sizeof *field_map);
		}
	}

	while ((index = dict_table_get_next_index(index)) != NULL) {
		if (index->to_be_dropped) {
			continue;
		}
		for (unsigned i = 0; i < index->n_fields; i++) {
			dict_field_t& f = index->fields[i];
			if (f.col >= table.cols
			    && f.col < table.cols + table.n_cols) {
				/* This is an instantly added column
				in a newly added index. */
				DBUG_ASSERT(!f.col->is_virtual());
				size_t c = f.col - table.cols;
				DBUG_ASSERT(f.col == &table.cols[c]);
				f.col = &cols[c];
			} else if (f.col >= &table.v_cols->m_col
				   && f.col < &table.v_cols[n_v_cols].m_col) {
				/* This is an instantly added virtual column
				in a newly added index. */
				DBUG_ASSERT(f.col->is_virtual());
				size_t c = reinterpret_cast<dict_v_col_t*>(
					f.col) - table.v_cols;
				DBUG_ASSERT(f.col == &table.v_cols[c].m_col);
				f.col = &v_cols[c].m_col;
			} else if (f.col < old_cols
				   || f.col >= old_cols + n_cols) {
				DBUG_ASSERT(f.col->is_virtual());
				f.col = &v_cols[col_map[
						reinterpret_cast<dict_v_col_t*>(
							f.col)
						- old_v_cols + n_cols]].m_col;
			} else {
				f.col = &cols[col_map[f.col - old_cols]];
				DBUG_ASSERT(!f.col->is_virtual());
			}
			f.name = f.col->name(*this);
			if (f.col->is_virtual()) {
				dict_v_col_t* v_col = reinterpret_cast
					<dict_v_col_t*>(f.col);
				v_col->v_indexes.push_front(
					dict_v_idx_t(index, i));
			}
		}
	}

	n_cols = table.n_cols;
	n_v_cols = table.n_v_cols;
	return metadata_changed;
}

/** Find the old column number for the given new column position.
@param[in]	col_map	column map from old column to new column
@param[in]	pos	new column position
@param[in]	n	number of columns present in the column map
@return old column position for the given new column position. */
static ulint find_old_col_no(const ulint* col_map, ulint pos, ulint n)
{
	do {
		ut_ad(n);
	} while (col_map[--n] != pos);
	return n;
}

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
inline void dict_table_t::rollback_instant(
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
	const ulint*	col_map)
{
	ut_ad(dict_sys.locked());

	if (cols == old_cols) {
		/* Alter fails before instant operation happens.
		So there is no need to do rollback instant operation */
		return;
	}

	dict_index_t* index = indexes.start;
	/* index->is_instant() does not necessarily hold here, because
	the table may have been emptied */
	DBUG_ASSERT(old_n_cols >= DATA_N_SYS_COLS);
	DBUG_ASSERT(n_cols == n_def);
	DBUG_ASSERT(index->n_def == index->n_fields);
	DBUG_ASSERT(index->n_core_fields <= index->n_fields);
	DBUG_ASSERT(old_n_core_fields <= old_n_fields);
	DBUG_ASSERT(instant || !old_instant);

	instant = old_instant;

	index->n_nullable = 0;

	for (unsigned i = old_n_fields; i--; ) {
		if (old_fields[i].col->is_nullable()) {
			index->n_nullable++;
		}
	}

	for (unsigned i = n_v_cols; i--; ) {
		v_cols[i].~dict_v_col_t();
	}

	index->n_core_fields = ((index->n_fields == index->n_core_fields)
				? old_n_fields
				: old_n_core_fields)
		& dict_index_t::MAX_N_FIELDS;
	index->n_def = index->n_fields = old_n_fields
		& dict_index_t::MAX_N_FIELDS;
	index->n_core_null_bytes = static_cast<uint8_t>(
		UT_BITS_IN_BYTES(index->get_n_nullable(index->n_core_fields)));

	const dict_col_t* const new_cols = cols;
	const dict_col_t* const new_cols_end __attribute__((unused)) = cols + n_cols;
	const dict_v_col_t* const new_v_cols = v_cols;
	const dict_v_col_t* const new_v_cols_end __attribute__((unused))= v_cols + n_v_cols;

	cols = old_cols;
	col_names = old_col_names;
	v_cols = old_v_cols;
	v_col_names = old_v_col_names;
	n_def = n_cols = old_n_cols & dict_index_t::MAX_N_FIELDS;
	n_v_def = n_v_cols = old_n_v_cols & dict_index_t::MAX_N_FIELDS;
	n_t_def = n_t_cols = (n_cols + n_v_cols) & dict_index_t::MAX_N_FIELDS;

	if (versioned()) {
		for (unsigned i = 0; i < n_cols; ++i) {
			if (cols[i].vers_sys_start()) {
				vers_start = i & dict_index_t::MAX_N_FIELDS;
			} else if (cols[i].vers_sys_end()) {
				vers_end = i & dict_index_t::MAX_N_FIELDS;
			}
		}
	}

	index->fields = old_fields;

	while ((index = dict_table_get_next_index(index)) != NULL) {
		if (index->to_be_dropped) {
			/* instant_column() did not adjust these indexes. */
			continue;
		}

		for (unsigned i = 0; i < index->n_fields; i++) {
			dict_field_t& f = index->fields[i];
			if (f.col->is_virtual()) {
				DBUG_ASSERT(f.col >= &new_v_cols->m_col);
				DBUG_ASSERT(f.col < &new_v_cols_end->m_col);
				size_t n = size_t(
					reinterpret_cast<dict_v_col_t*>(f.col)
					- new_v_cols);
				DBUG_ASSERT(n <= n_v_cols);

				ulint old_col_no = find_old_col_no(
					col_map + n_cols, n, n_v_cols);
				DBUG_ASSERT(old_col_no <= n_v_cols);
				f.col = &v_cols[old_col_no].m_col;
				DBUG_ASSERT(f.col->is_virtual());
			} else {
				DBUG_ASSERT(f.col >= new_cols);
				DBUG_ASSERT(f.col < new_cols_end);
				size_t n = size_t(f.col - new_cols);
				DBUG_ASSERT(n <= n_cols);

				ulint old_col_no = find_old_col_no(col_map,
								   n, n_cols);
				DBUG_ASSERT(old_col_no < n_cols);
				f.col = &cols[old_col_no];
				DBUG_ASSERT(!f.col->is_virtual());
			}
			f.name = f.col->name(*this);
		}
	}
}

/* Report an InnoDB error to the client by invoking my_error(). */
static ATTRIBUTE_COLD __attribute__((nonnull))
void
my_error_innodb(
/*============*/
	dberr_t		error,	/*!< in: InnoDB error code */
	const char*	table,	/*!< in: table name */
	ulint		flags)	/*!< in: table flags */
{
	switch (error) {
	case DB_MISSING_HISTORY:
		my_error(ER_TABLE_DEF_CHANGED, MYF(0));
		break;
	case DB_RECORD_NOT_FOUND:
		my_error(ER_KEY_NOT_FOUND, MYF(0), table);
		break;
	case DB_DEADLOCK:
		my_error(ER_LOCK_DEADLOCK, MYF(0));
		break;
	case DB_LOCK_WAIT_TIMEOUT:
		my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
		break;
	case DB_INTERRUPTED:
		my_error(ER_QUERY_INTERRUPTED, MYF(0));
		break;
	case DB_OUT_OF_MEMORY:
		my_error(ER_OUT_OF_RESOURCES, MYF(0));
		break;
	case DB_OUT_OF_FILE_SPACE:
		my_error(ER_RECORD_FILE_FULL, MYF(0), table);
		break;
	case DB_TEMP_FILE_WRITE_FAIL:
		my_error(ER_TEMP_FILE_WRITE_FAILURE, MYF(0));
		break;
	case DB_TOO_BIG_INDEX_COL:
		my_error(ER_INDEX_COLUMN_TOO_LONG, MYF(0),
			 (ulong) DICT_MAX_FIELD_LEN_BY_FORMAT_FLAG(flags));
		break;
	case DB_TOO_MANY_CONCURRENT_TRXS:
		my_error(ER_TOO_MANY_CONCURRENT_TRXS, MYF(0));
		break;
	case DB_LOCK_TABLE_FULL:
		my_error(ER_LOCK_TABLE_FULL, MYF(0));
		break;
	case DB_UNDO_RECORD_TOO_BIG:
		my_error(ER_UNDO_RECORD_TOO_BIG, MYF(0));
		break;
	case DB_CORRUPTION:
		my_error(ER_NOT_KEYFILE, MYF(0), table);
		break;
	case DB_TOO_BIG_RECORD: {
		/* Note that in page0zip.ic page_zip_rec_needs_ext() rec_size
		is limited to COMPRESSED_REC_MAX_DATA_SIZE (16K) or
		REDUNDANT_REC_MAX_DATA_SIZE (16K-1). */
		bool comp = !!(flags & DICT_TF_COMPACT);
		ulint free_space = page_get_free_space_of_empty(comp) / 2;

		if (free_space >= ulint(comp ? COMPRESSED_REC_MAX_DATA_SIZE :
					  REDUNDANT_REC_MAX_DATA_SIZE)) {
			free_space = (comp ? COMPRESSED_REC_MAX_DATA_SIZE :
				REDUNDANT_REC_MAX_DATA_SIZE) - 1;
		}

		my_error(ER_TOO_BIG_ROWSIZE, MYF(0), free_space);
		break;
	}
	case DB_INVALID_NULL:
		/* TODO: report the row, as we do for DB_DUPLICATE_KEY */
		my_error(ER_INVALID_USE_OF_NULL, MYF(0));
		break;
	case DB_CANT_CREATE_GEOMETRY_OBJECT:
		my_error(ER_CANT_CREATE_GEOMETRY_OBJECT, MYF(0));
		break;
	case DB_TABLESPACE_EXISTS:
		my_error(ER_TABLESPACE_EXISTS, MYF(0), table);
		break;

#ifdef UNIV_DEBUG
	case DB_SUCCESS:
	case DB_DUPLICATE_KEY:
	case DB_ONLINE_LOG_TOO_BIG:
		/* These codes should not be passed here. */
		ut_error;
#endif /* UNIV_DEBUG */
	default:
		my_error(ER_GET_ERRNO, MYF(0), error, "InnoDB");
		break;
	}
}

/** Get the name of an erroneous key.
@param[in]	error_key_num	InnoDB number of the erroneus key
@param[in]	ha_alter_info	changes that were being performed
@param[in]	table		InnoDB table
@return	the name of the erroneous key */
static
const char*
get_error_key_name(
	ulint				error_key_num,
	const Alter_inplace_info*	ha_alter_info,
	const dict_table_t*		table)
{
	if (error_key_num == ULINT_UNDEFINED) {
		return(FTS_DOC_ID_INDEX_NAME);
	} else if (ha_alter_info->key_count == 0) {
		return(dict_table_get_first_index(table)->name);
	} else {
		return(ha_alter_info->key_info_buffer[error_key_num].name.str);
	}
}

struct ha_innobase_inplace_ctx : public inplace_alter_handler_ctx
{
	/** Dummy query graph */
	que_thr_t*const	thr;
	/** The prebuilt struct of the creating instance */
	row_prebuilt_t*&	prebuilt;
	/** InnoDB indexes being created */
	dict_index_t**	add_index;
	/** MySQL key numbers for the InnoDB indexes that are being created */
	const ulint*	add_key_numbers;
	/** number of InnoDB indexes being created */
	ulint		num_to_add_index;
	/** InnoDB indexes being dropped */
	dict_index_t**	drop_index;
	/** number of InnoDB indexes being dropped */
	const ulint	num_to_drop_index;
	/** InnoDB foreign key constraints being dropped */
	dict_foreign_t** drop_fk;
	/** number of InnoDB foreign key constraints being dropped */
	const ulint	num_to_drop_fk;
	/** InnoDB foreign key constraints being added */
	dict_foreign_t** add_fk;
	/** number of InnoDB foreign key constraints being dropped */
	const ulint	num_to_add_fk;
	/** whether to create the indexes online */
	const bool	online;
	/** memory heap */
	mem_heap_t* const heap;
	/** dictionary transaction */
	trx_t*		trx;
	/** original table (if rebuilt, differs from indexed_table) */
	dict_table_t*	old_table;
	/** table where the indexes are being created or dropped */
	dict_table_t*	new_table;
	/** table definition for instant ADD/DROP/reorder COLUMN */
	dict_table_t*	instant_table;
	/** mapping of old column numbers to new ones, or NULL */
	const ulint*	col_map;
	/** new column names, or NULL if nothing was renamed */
	const char**	col_names;
	/** added AUTO_INCREMENT column position, or ULINT_UNDEFINED */
	const ulint	add_autoinc;
	/** default values of ADD and CHANGE COLUMN, or NULL */
	const dtuple_t*	defaults;
	/** autoinc sequence to use */
	ib_sequence_t	sequence;
	/** temporary table name to use for old table when renaming tables */
	const char*	tmp_name;
	/** whether the order of the clustered index is unchanged */
	bool		skip_pk_sort;
	/** number of virtual columns to be added */
	unsigned	num_to_add_vcol;
	/** virtual columns to be added */
	dict_v_col_t*	add_vcol;
	const char**	add_vcol_name;
	/** number of virtual columns to be dropped */
	unsigned	num_to_drop_vcol;
	/** virtual columns to be dropped */
	dict_v_col_t*	drop_vcol;
	const char**	drop_vcol_name;
	/** ALTER TABLE stage progress recorder */
	ut_stage_alter_t* m_stage;
	/** original number of user columns in the table */
	const unsigned	old_n_cols;
	/** original columns of the table */
	dict_col_t* const old_cols;
	/** original column names of the table */
	const char* const old_col_names;
	/** original instantly dropped or reordered columns */
	dict_instant_t*	const	old_instant;
	/** original index fields */
	dict_field_t* const	old_fields;
	/** size of old_fields */
	const unsigned		old_n_fields;
	/** original old_table->n_core_fields */
	const unsigned		old_n_core_fields;
	/** original number of virtual columns in the table */
	const unsigned		old_n_v_cols;
	/** original virtual columns of the table */
	dict_v_col_t* const old_v_cols;
	/** original virtual column names of the table */
	const char* const old_v_col_names;
	/** 0, or 1 + first column whose position changes in instant ALTER */
	unsigned	first_alter_pos;
	/** Allow non-null conversion.
	(1) Alter ignore should allow the conversion
	irrespective of sql mode.
	(2) Don't allow the conversion in strict mode
	(3) Allow the conversion only in non-strict mode. */
	const bool	allow_not_null;

	/** The page_compression_level attribute, or 0 */
	const uint	page_compression_level;

	ha_innobase_inplace_ctx(row_prebuilt_t*& prebuilt_arg,
				dict_index_t** drop_arg,
				ulint num_to_drop_arg,
				dict_foreign_t** drop_fk_arg,
				ulint num_to_drop_fk_arg,
				dict_foreign_t** add_fk_arg,
				ulint num_to_add_fk_arg,
				bool online_arg,
				mem_heap_t* heap_arg,
				dict_table_t* new_table_arg,
				const char** col_names_arg,
				ulint add_autoinc_arg,
				ulonglong autoinc_col_min_value_arg,
				ulonglong autoinc_col_max_value_arg,
				bool allow_not_null_flag,
				bool page_compressed,
				ulonglong page_compression_level_arg) :
		inplace_alter_handler_ctx(),
		thr (pars_complete_graph_for_exec(nullptr, prebuilt_arg->trx,
						  heap_arg, prebuilt_arg)),
		prebuilt (prebuilt_arg),
		add_index (0), add_key_numbers (0), num_to_add_index (0),
		drop_index (drop_arg), num_to_drop_index (num_to_drop_arg),
		drop_fk (drop_fk_arg), num_to_drop_fk (num_to_drop_fk_arg),
		add_fk (add_fk_arg), num_to_add_fk (num_to_add_fk_arg),
		online (online_arg), heap (heap_arg),
		trx (innobase_trx_allocate(prebuilt_arg->trx->mysql_thd)),
		old_table (prebuilt_arg->table),
		new_table (new_table_arg), instant_table (0),
		col_map (0), col_names (col_names_arg),
		add_autoinc (add_autoinc_arg),
		defaults (0),
		sequence(prebuilt->trx->mysql_thd,
			 autoinc_col_min_value_arg, autoinc_col_max_value_arg),
		tmp_name (0),
		skip_pk_sort(false),
		num_to_add_vcol(0),
		add_vcol(0),
		add_vcol_name(0),
		num_to_drop_vcol(0),
		drop_vcol(0),
		drop_vcol_name(0),
		m_stage(NULL),
		old_n_cols(prebuilt_arg->table->n_cols),
		old_cols(prebuilt_arg->table->cols),
		old_col_names(prebuilt_arg->table->col_names),
		old_instant(prebuilt_arg->table->instant),
		old_fields(prebuilt_arg->table->indexes.start->fields),
		old_n_fields(prebuilt_arg->table->indexes.start->n_fields),
		old_n_core_fields(prebuilt_arg->table->indexes.start
				  ->n_core_fields),
		old_n_v_cols(prebuilt_arg->table->n_v_cols),
		old_v_cols(prebuilt_arg->table->v_cols),
		old_v_col_names(prebuilt_arg->table->v_col_names),
		first_alter_pos(0),
		allow_not_null(allow_not_null_flag),
		page_compression_level(page_compressed
				       ? (page_compression_level_arg
					  ? uint(page_compression_level_arg)
					  : page_zip_level)
				       : 0)
	{
		ut_ad(old_n_cols >= DATA_N_SYS_COLS);
		ut_ad(page_compression_level <= 9);
#ifdef UNIV_DEBUG
		for (ulint i = 0; i < num_to_add_index; i++) {
			ut_ad(!add_index[i]->to_be_dropped);
		}
		for (ulint i = 0; i < num_to_drop_index; i++) {
			ut_ad(drop_index[i]->to_be_dropped);
		}
#endif /* UNIV_DEBUG */

		trx_start_for_ddl(trx);
	}

	~ha_innobase_inplace_ctx()
	{
		UT_DELETE(m_stage);
		if (instant_table) {
			ut_ad(!instant_table->id);
			while (dict_index_t* index
			       = UT_LIST_GET_LAST(instant_table->indexes)) {
				UT_LIST_REMOVE(instant_table->indexes, index);
				index->lock.free();
				dict_mem_index_free(index);
			}
			for (unsigned i = old_n_v_cols; i--; ) {
				old_v_cols[i].~dict_v_col_t();
			}
			if (instant_table->fts) {
				fts_free(instant_table);
			}
			dict_mem_table_free(instant_table);
		}
		mem_heap_free(heap);
	}

	/** Determine if the table will be rebuilt.
	@return whether the table will be rebuilt */
	bool need_rebuild () const { return(old_table != new_table); }

	/** Convert table-rebuilding ALTER to instant ALTER. */
	void prepare_instant()
	{
		DBUG_ASSERT(need_rebuild());
		DBUG_ASSERT(!is_instant());
		DBUG_ASSERT(old_table->n_cols == old_n_cols);

		instant_table = new_table;
		new_table = old_table;
		export_vars.innodb_instant_alter_column++;

		instant_table->prepare_instant(*old_table, col_map,
					       first_alter_pos);
	}

	/** Adjust table metadata for instant ADD/DROP/reorder COLUMN.
	@return whether the metadata record must be updated */
	bool instant_column()
	{
		DBUG_ASSERT(is_instant());
		DBUG_ASSERT(old_n_fields
			    == old_table->indexes.start->n_fields);
		return old_table->instant_column(*instant_table, col_map);
	}

	/** Revert prepare_instant() if the transaction is rolled back. */
	void rollback_instant()
	{
		if (!is_instant()) return;
		old_table->rollback_instant(old_n_cols,
					    old_cols, old_col_names,
					    old_instant,
					    old_fields, old_n_fields,
					    old_n_core_fields,
					    old_n_v_cols, old_v_cols,
					    old_v_col_names,
					    col_map);
	}

	/** @return whether this is instant ALTER TABLE */
	bool is_instant() const
	{
		DBUG_ASSERT(!instant_table || !instant_table->can_be_evicted);
		return instant_table;
	}

	/** Create an index table where indexes are ordered as follows:

	IF a new primary key is defined for the table THEN

		1) New primary key
		2) The remaining keys in key_info

	ELSE

		1) All new indexes in the order they arrive from MySQL

	ENDIF

	@return key definitions */
	MY_ATTRIBUTE((nonnull, warn_unused_result, malloc))
	inline index_def_t*
	create_key_defs(
		const Alter_inplace_info*	ha_alter_info,
				/*!< in: alter operation */
		const TABLE*			altered_table,
				/*!< in: MySQL table that is being altered */
		ulint&				n_fts_add,
				/*!< out: number of FTS indexes to be created */
		ulint&				fts_doc_id_col,
				/*!< in: The column number for Doc ID */
		bool&				add_fts_doc_id,
				/*!< in: whether we need to add new DOC ID
				column for FTS index */
		bool&				add_fts_doc_idx,
				/*!< in: whether we need to add new DOC ID
				index for FTS index */
		const TABLE*			table);
				/*!< in: MySQL table that is being altered */

	/** Share context between partitions.
	@param[in] ctx	context from another partition of the table */
	void set_shared_data(const inplace_alter_handler_ctx& ctx)
	{
		if (add_autoinc != ULINT_UNDEFINED) {
			const ha_innobase_inplace_ctx& ha_ctx =
				static_cast<const ha_innobase_inplace_ctx&>
				(ctx);
			/* When adding an AUTO_INCREMENT column to a
			partitioned InnoDB table, we must share the
			sequence for all partitions. */
			ut_ad(ha_ctx.add_autoinc == add_autoinc);
			ut_ad(ha_ctx.sequence.last());
			sequence = ha_ctx.sequence;
		}
	}

   /** @return whether the given column is being added */
   bool is_new_vcol(const dict_v_col_t &v_col) const
   {
     for (ulint i= 0; i < num_to_add_vcol; i++)
       if (&add_vcol[i] == &v_col)
         return true;
     return false;
   }

  /** During rollback, make newly added indexes point to
  newly added virtual columns. */
  void clean_new_vcol_index()
  {
    ut_ad(old_table == new_table);
    const dict_index_t *index= dict_table_get_first_index(old_table);
    while ((index= dict_table_get_next_index(index)) != NULL)
    {
      if (!index->has_virtual() || index->is_committed())
        continue;
      ulint n_drop_new_vcol= index->get_new_n_vcol();
      for (ulint i= 0; n_drop_new_vcol && i < index->n_fields; i++)
      {
        dict_col_t *col= index->fields[i].col;
        /* Skip the non-virtual and old virtual columns */
        if (!col->is_virtual())
          continue;
        dict_v_col_t *vcol= reinterpret_cast<dict_v_col_t*>(col);
        if (!is_new_vcol(*vcol))
          continue;

        index->fields[i].col= &index->new_vcol_info->
          add_drop_v_col(index->heap, vcol, --n_drop_new_vcol)->m_col;
      }
    }
  }

  /** @return whether a FULLTEXT INDEX is being added */
  bool adding_fulltext_index() const
  {
    for (ulint a= 0; a < num_to_add_index; a++)
      if (add_index[a]->type & DICT_FTS)
        return true;
    return false;
  }

  /** Handle the apply log failure for online DDL operation.
  @param ha_alter_info    handler alter inplace info
  @param altered_table    MySQL table that is being altered
  @param error            error code
  @retval false if error value is DB_SUCCESS or
  TRUE in case of error */
  bool log_failure(Alter_inplace_info *ha_alter_info,
                   TABLE *altered_table, dberr_t error)
  {
    ulint err_key= thr_get_trx(thr)->error_key_num;
    switch (error) {
      KEY *dup_key;
    case DB_SUCCESS:
      return false;
    case DB_DUPLICATE_KEY:
      if (err_key == ULINT_UNDEFINED)
        /* This should be the hidden index on FTS_DOC_ID */
        dup_key= nullptr;
      else
      {
        DBUG_ASSERT(err_key < ha_alter_info->key_count);
        dup_key= &ha_alter_info->key_info_buffer[err_key];
      }
      print_keydup_error(altered_table, dup_key, MYF(0));
      break;
    case DB_ONLINE_LOG_TOO_BIG:
      my_error(ER_INNODB_ONLINE_LOG_TOO_BIG, MYF(0),
               get_error_key_name(err_key, ha_alter_info, new_table));
      break;
    case DB_INDEX_CORRUPT:
      my_error(ER_INDEX_CORRUPT, MYF(0),
               get_error_key_name(err_key, ha_alter_info, new_table));
      break;
    default:
      my_error_innodb(error, old_table->name.m_name, old_table->flags);
    }
    return true;
  }
};

/********************************************************************//**
Get the upper limit of the MySQL integral and floating-point type.
@return maximum allowed value for the field */
ulonglong innobase_get_int_col_max_value(const Field *field);

/** Determine if fulltext indexes exist in a given table.
@param table MySQL table
@return number of fulltext indexes */
static uint innobase_fulltext_exist(const TABLE* table)
{
	uint count = 0;

	for (uint i = 0; i < table->s->keys; i++) {
		if (table->key_info[i].flags & HA_FULLTEXT) {
			count++;
		}
	}

	return count;
}

/** Determine whether indexed virtual columns exist in a table.
@param[in]	table	table definition
@return	whether indexes exist on virtual columns */
static bool innobase_indexed_virtual_exist(const TABLE* table)
{
	const KEY* const end = &table->key_info[table->s->keys];

	for (const KEY* key = table->key_info; key < end; key++) {
		const KEY_PART_INFO* const key_part_end = key->key_part
			+ key->user_defined_key_parts;
		for (const KEY_PART_INFO* key_part = key->key_part;
		     key_part < key_part_end; key_part++) {
			if (!key_part->field->stored_in_db())
				return true;
		}
	}

	return false;
}

/** Determine if spatial indexes exist in a given table.
@param table MySQL table
@return whether spatial indexes exist on the table */
static
bool
innobase_spatial_exist(
/*===================*/
	const   TABLE*  table)
{
	for (uint i = 0; i < table->s->keys; i++) {
	       if (table->key_info[i].flags & HA_SPATIAL) {
		       return(true);
	       }
	}

	return(false);
}

/** Determine if ALTER_OPTIONS requires rebuilding the table.
@param[in] ha_alter_info	the ALTER TABLE operation
@param[in] table		metadata before ALTER TABLE
@return whether it is mandatory to rebuild the table */
static bool alter_options_need_rebuild(
	const Alter_inplace_info*	ha_alter_info,
	const TABLE*			table)
{
	DBUG_ASSERT(ha_alter_info->handler_flags & ALTER_OPTIONS);

	if (ha_alter_info->create_info->used_fields
	    & (HA_CREATE_USED_ROW_FORMAT
	       | HA_CREATE_USED_KEY_BLOCK_SIZE)) {
		/* Specifying ROW_FORMAT or KEY_BLOCK_SIZE requires
		rebuilding the table. (These attributes in the .frm
		file may disagree with the InnoDB data dictionary, and
		the interpretation of thse attributes depends on
		InnoDB parameters. That is why we for now always
		require a rebuild when these attributes are specified.) */
		return true;
	}

	const ha_table_option_struct& alt_opt=
			*ha_alter_info->create_info->option_struct;
	const ha_table_option_struct& opt= *table->s->option_struct;

	/* Allow an instant change to enable page_compressed,
	and any change of page_compression_level. */
	if ((!alt_opt.page_compressed && opt.page_compressed)
	    || alt_opt.encryption != opt.encryption
	    || alt_opt.encryption_key_id != opt.encryption_key_id) {
		return(true);
	}

	return false;
}

/** Determine if ALTER TABLE needs to rebuild the table
(or perform instant operation).
@param[in] ha_alter_info	the ALTER TABLE operation
@param[in] table		metadata before ALTER TABLE
@return whether it is necessary to rebuild the table or to alter columns */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
innobase_need_rebuild(
	const Alter_inplace_info*	ha_alter_info,
	const TABLE*			table)
{
	if ((ha_alter_info->handler_flags & ~(INNOBASE_INPLACE_IGNORE
					      | INNOBASE_ALTER_NOREBUILD
					      | INNOBASE_ALTER_INSTANT))
	    == ALTER_OPTIONS) {
		return alter_options_need_rebuild(ha_alter_info, table);
	}

	return !!(ha_alter_info->handler_flags & INNOBASE_ALTER_REBUILD);
}

/** Check if virtual column in old and new table are in order, excluding
those dropped column. This is needed because when we drop a virtual column,
ALTER_VIRTUAL_COLUMN_ORDER is also turned on, so we can't decide if this
is a real ORDER change or just DROP COLUMN
@param[in]	table		old TABLE
@param[in]	altered_table	new TABLE
@param[in]	ha_alter_info	Structure describing changes to be done
by ALTER TABLE and holding data used during in-place alter.
@return	true is all columns in order, false otherwise. */
static
bool
check_v_col_in_order(
	const TABLE*		table,
	const TABLE*		altered_table,
	Alter_inplace_info*	ha_alter_info)
{
	ulint	j = 0;

	/* We don't support any adding new virtual column before
	existed virtual column. */
	if (ha_alter_info->handler_flags
              & ALTER_ADD_VIRTUAL_COLUMN) {
		bool			has_new = false;

		for (const Create_field& new_field :
		     ha_alter_info->alter_info->create_list) {
			if (new_field.stored_in_db()) {
				continue;
			}

			/* Found a new added virtual column. */
			if (!new_field.field) {
				has_new = true;
				continue;
			}

			/* If there's any old virtual column
			after the new added virtual column,
			order must be changed. */
			if (has_new) {
				return(false);
			}
		}
	}

	/* directly return true if ALTER_VIRTUAL_COLUMN_ORDER is not on */
	if (!(ha_alter_info->handler_flags
              & ALTER_VIRTUAL_COLUMN_ORDER)) {
		return(true);
	}

	for (ulint i = 0; i < table->s->fields; i++) {
		Field*		field = table->field[i];

		if (field->stored_in_db()) {
			continue;
		}

		if (field->flags & FIELD_IS_DROPPED) {
			continue;
		}

		/* Now check if the next virtual column in altered table
		matches this column */
		while (j < altered_table->s->fields) {
			 Field*  new_field = altered_table->s->field[j];

			if (new_field->stored_in_db()) {
				j++;
				continue;
			}

			if (my_strcasecmp(system_charset_info,
					  field->field_name.str,
					  new_field->field_name.str) != 0) {
				/* different column */
				return(false);
			} else {
				j++;
				break;
			}
		}

		if (j > altered_table->s->fields) {
			/* there should not be less column in new table
			without them being in drop list */
			ut_ad(0);
			return(false);
		}
	}

	return(true);
}

/** Determine if an instant operation is possible for altering columns.
@param[in]	ib_table	InnoDB table definition
@param[in]	ha_alter_info	the ALTER TABLE operation
@param[in]	table		table definition before ALTER TABLE
@param[in]	altered_table	table definition after ALTER TABLE
@param[in]	strict		whether to ensure that user records fit */
static
bool
instant_alter_column_possible(
	const dict_table_t&		ib_table,
	const Alter_inplace_info*	ha_alter_info,
	const TABLE*			table,
	const TABLE*			altered_table,
	bool				strict)
{
	const dict_index_t* const pk = ib_table.indexes.start;
	ut_ad(pk->is_primary());
	ut_ad(!pk->has_virtual());

	if (ha_alter_info->handler_flags
	    & (ALTER_STORED_COLUMN_ORDER | ALTER_DROP_STORED_COLUMN
	       | ALTER_ADD_STORED_BASE_COLUMN)) {
#if 1 // MDEV-17459: adjust fts_fetch_doc_from_rec() and friends; remove this
		if (ib_table.fts || innobase_fulltext_exist(altered_table))
			return false;
#endif
#if 1 // MDEV-17468: fix bugs with indexed virtual columns & remove this
		for (const dict_index_t* index = ib_table.indexes.start;
		     index; index = index->indexes.next) {
			if (index->has_virtual()) {
				ut_ad(ib_table.n_v_cols
				      || index->is_corrupted());
				return false;
			}
		}
#endif
		uint n_add = 0, n_nullable = 0, lenlen = 0;
		const uint blob_prefix = dict_table_has_atomic_blobs(&ib_table)
			? 0
			: REC_ANTELOPE_MAX_INDEX_COL_LEN;
		const uint min_local_len = blob_prefix
			? blob_prefix + FIELD_REF_SIZE
			: 2 * FIELD_REF_SIZE;
		size_t min_size = 0, max_size = 0;
		Field** af = altered_table->field;
		Field** const end = altered_table->field
			+ altered_table->s->fields;
		List_iterator_fast<Create_field> cf_it(
			ha_alter_info->alter_info->create_list);

		for (; af < end; af++) {
			const Create_field* cf = cf_it++;
			if (!(*af)->stored_in_db() || cf->field) {
				/* Virtual or pre-existing column */
				continue;
			}
			const bool nullable = (*af)->real_maybe_null();
			const bool is_null = (*af)->is_real_null();
			ut_ad(!is_null || nullable);
			n_nullable += nullable;
			n_add++;
			uint l;
			switch ((*af)->type()) {
			case MYSQL_TYPE_VARCHAR:
				l = reinterpret_cast<const Field_varstring*>
					(*af)->get_length();
			variable_length:
				if (l >= min_local_len) {
					max_size += blob_prefix
						+ FIELD_REF_SIZE;
					if (!is_null) {
						min_size += blob_prefix
							+ FIELD_REF_SIZE;
					}
					lenlen += 2;
				} else {
					if (!is_null) {
						min_size += l;
					}
					l = (*af)->pack_length();
					max_size += l;
					lenlen += l > 255 ? 2 : 1;
				}
				break;
			case MYSQL_TYPE_GEOMETRY:
			case MYSQL_TYPE_TINY_BLOB:
			case MYSQL_TYPE_MEDIUM_BLOB:
			case MYSQL_TYPE_BLOB:
			case MYSQL_TYPE_LONG_BLOB:
				l = reinterpret_cast<const Field_blob*>
					((*af))->get_length();
				goto variable_length;
			default:
				l = (*af)->pack_length();
				if (l > 255 && ib_table.not_redundant()) {
					goto variable_length;
				}
				max_size += l;
				if (!is_null) {
					min_size += l;
				}
			}
		}

		ulint n_fields = pk->n_fields + n_add;

		if (n_fields >= REC_MAX_N_USER_FIELDS + DATA_N_SYS_COLS) {
			return false;
		}

		if (pk->is_gen_clust()) {
			min_size += DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN
				+ DATA_ROW_ID_LEN;
			max_size += DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN
				+ DATA_ROW_ID_LEN;
		} else {
			min_size += DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN;
			max_size += DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN;
		}

		uint i = pk->n_fields;
		while (i-- > pk->n_core_fields) {
			const dict_field_t& f = pk->fields[i];
			if (f.col->is_nullable()) {
				n_nullable++;
				if (!f.col->is_dropped()
				    && f.col->def_val.data) {
					goto instantly_added_column;
				}
			} else if (f.fixed_len
				   && (f.fixed_len <= 255
				       || !ib_table.not_redundant())) {
				if (ib_table.not_redundant()
				    || !f.col->is_dropped()) {
					min_size += f.fixed_len;
					max_size += f.fixed_len;
				}
			} else if (f.col->is_dropped() || !f.col->is_added()) {
				lenlen++;
				goto set_max_size;
			} else {
instantly_added_column:
				ut_ad(f.col->is_added());
				if (f.col->def_val.len >= min_local_len) {
					min_size += blob_prefix
						+ FIELD_REF_SIZE;
					lenlen += 2;
				} else {
					min_size += f.col->def_val.len;
					lenlen += f.col->def_val.len
						> 255 ? 2 : 1;
				}
set_max_size:
				if (f.fixed_len
				    && (f.fixed_len <= 255
					|| !ib_table.not_redundant())) {
					max_size += f.fixed_len;
				} else if (f.col->len >= min_local_len) {
					max_size += blob_prefix
						+ FIELD_REF_SIZE;
				} else {
					max_size += f.col->len;
				}
			}
		}

		do {
			const dict_field_t& f = pk->fields[i];
			if (f.col->is_nullable()) {
				n_nullable++;
			} else if (f.fixed_len) {
				min_size += f.fixed_len;
			} else {
				lenlen++;
			}
		} while (i--);

		if (ib_table.instant
		    || (ha_alter_info->handler_flags
			& (ALTER_STORED_COLUMN_ORDER
			   | ALTER_DROP_STORED_COLUMN))) {
			n_fields++;
			lenlen += 2;
			min_size += FIELD_REF_SIZE;
		}

		if (ib_table.not_redundant()) {
			min_size += REC_N_NEW_EXTRA_BYTES
				+ UT_BITS_IN_BYTES(n_nullable)
				+ lenlen;
		} else {
			min_size += (n_fields > 255 || min_size > 255)
				? n_fields * 2 : n_fields;
			min_size += REC_N_OLD_EXTRA_BYTES;
		}

		if (page_zip_rec_needs_ext(min_size, ib_table.not_redundant(),
					   0, 0)) {
			return false;
		}

		if (strict && page_zip_rec_needs_ext(max_size,
						     ib_table.not_redundant(),
						     0, 0)) {
			return false;
		}
	}
	// Making table system-versioned instantly is not implemented yet.
	if (ha_alter_info->handler_flags & ALTER_ADD_SYSTEM_VERSIONING) {
		return false;
	}

	static constexpr alter_table_operations avoid_rebuild
		= ALTER_ADD_STORED_BASE_COLUMN
		| ALTER_DROP_STORED_COLUMN
		| ALTER_STORED_COLUMN_ORDER
		| ALTER_COLUMN_NULLABLE;

	if (!(ha_alter_info->handler_flags & avoid_rebuild)) {
		alter_table_operations flags = ha_alter_info->handler_flags
			& ~avoid_rebuild;
		/* None of the flags are set that we can handle
		specially to avoid rebuild. In this case, we can
		allow ALGORITHM=INSTANT, except if some requested
		operation requires that the table be rebuilt. */
		if (flags & INNOBASE_ALTER_REBUILD) {
			return false;
		}
		if ((flags & ALTER_OPTIONS)
		    && alter_options_need_rebuild(ha_alter_info, table)) {
			return false;
		}
	} else if (!ib_table.supports_instant()) {
		return false;
	}

	/* At the moment, we disallow ADD [UNIQUE] INDEX together with
	instant ADD COLUMN.

	The main reason is that the work of instant ADD must be done
	in commit_inplace_alter_table().  For the rollback_instant()
	to work, we must add the columns to dict_table_t beforehand,
	and roll back those changes in case the transaction is rolled
	back.

	If we added the columns to the dictionary cache already in the
	prepare_inplace_alter_table(), we would have to deal with
	column number mismatch in ha_innobase::open(), write_row() and
	other functions. */

	/* FIXME: allow instant ADD COLUMN together with
	INNOBASE_ONLINE_CREATE (ADD [UNIQUE] INDEX) on pre-existing
	columns. */
	if (ha_alter_info->handler_flags
	    & ((INNOBASE_ALTER_REBUILD | INNOBASE_ONLINE_CREATE)
	       & ~ALTER_DROP_STORED_COLUMN
	       & ~ALTER_STORED_COLUMN_ORDER
	       & ~ALTER_ADD_STORED_BASE_COLUMN
	       & ~ALTER_COLUMN_NULLABLE
	       & ~ALTER_OPTIONS)) {
		return false;
	}

	if ((ha_alter_info->handler_flags & ALTER_OPTIONS)
	    && alter_options_need_rebuild(ha_alter_info, table)) {
		return false;
	}

	if (ha_alter_info->handler_flags & ALTER_COLUMN_NULLABLE) {
		if (ib_table.not_redundant()) {
			/* Instantaneous removal of NOT NULL is
			only supported for ROW_FORMAT=REDUNDANT. */
			return false;
		}
		if (ib_table.fts_doc_id_index
		    && !innobase_fulltext_exist(altered_table)) {
			/* Removing hidden FTS_DOC_ID_INDEX(FTS_DOC_ID)
			requires that the table be rebuilt. */
			return false;
		}

		Field** af = altered_table->field;
		Field** const end = altered_table->field
			+ altered_table->s->fields;
		List_iterator_fast<Create_field> cf_it(
			ha_alter_info->alter_info->create_list);
		for (unsigned c = 0; af < end; af++) {
			const Create_field* cf = cf_it++;
			if (!cf->field || !(*af)->stored_in_db()) {
				/* Ignore virtual or newly created
				column */
				continue;
			}

			const dict_col_t* col = dict_table_get_nth_col(
				&ib_table, c++);

			if (!col->ord_part || col->is_nullable()
			    || !(*af)->real_maybe_null()) {
				continue;
			}

			/* The column would be changed from NOT NULL.
			Ensure that it is not a clustered index key. */
			for (auto i = pk->n_uniq; i--; ) {
				if (pk->fields[i].col == col) {
					return false;
				}
			}
		}
	}

	return true;
}

/** Check whether the non-const default value for the field
@param[in]	field	field which could be added or changed
@return true if the non-const default is present. */
static bool is_non_const_value(Field* field)
{
	return field->default_value
		&& field->default_value->flags
		& uint(~(VCOL_SESSION_FUNC | VCOL_TIME_FUNC));
}

/** Set default value for the field.
@param[in]	field	field which could be added or changed
@return true if the default value is set. */
static bool set_default_value(Field* field)
{
	/* The added/changed NOT NULL column lacks a DEFAULT value,
	   or the DEFAULT is the same for all rows.
	   (Time functions, such as CURRENT_TIMESTAMP(),
	   are evaluated from a timestamp that is assigned
	   at the start of the statement. Session
	   functions, such as USER(), always evaluate the
	   same within a statement.) */

	ut_ad(!is_non_const_value(field));

	/* Compute the DEFAULT values of non-constant columns
	   (VCOL_SESSION_FUNC | VCOL_TIME_FUNC). */
	switch (field->set_default()) {
	case 0: /* OK */
	case 3: /* DATETIME to TIME or DATE conversion */
		return true;
	case -1: /* OOM, or GEOMETRY type mismatch */
	case 1:  /* A number adjusted to the min/max value */
	case 2:  /* String truncation, or conversion problem */
		break;
	}

	return false;
}

/** Check whether the table has the FTS_DOC_ID column
@param[in]	table		InnoDB table with fulltext index
@param[in]	altered_table	MySQL table with fulltext index
@param[out]	fts_doc_col_no	The column number for Doc ID,
				or ULINT_UNDEFINED if it is of wrong type
@param[out]	num_v		Number of virtual column
@param[in]	check_only	check only whether fts doc id exist.
@return whether there exists an FTS_DOC_ID column */
static
bool
innobase_fts_check_doc_id_col(
	const dict_table_t*	table,
	const TABLE*		altered_table,
	ulint*			fts_doc_col_no,
	ulint*			num_v,
	bool			check_only=false)
{
	*fts_doc_col_no = ULINT_UNDEFINED;

	const uint n_cols = altered_table->s->fields;
	ulint	i;
	int	err = 0;
	*num_v = 0;

	for (i = 0; i < n_cols; i++) {
		const Field*	field = altered_table->field[i];

		if (!field->stored_in_db()) {
			(*num_v)++;
		}

		if (my_strcasecmp(system_charset_info,
				  field->field_name.str, FTS_DOC_ID_COL_NAME)) {
			continue;
		}

		if (strcmp(field->field_name.str, FTS_DOC_ID_COL_NAME)) {
			err = ER_WRONG_COLUMN_NAME;
		} else if (field->type() != MYSQL_TYPE_LONGLONG
			   || field->pack_length() != 8
			   || field->real_maybe_null()
			   || !(field->flags & UNSIGNED_FLAG)
			   || !field->stored_in_db()) {
			err = ER_INNODB_FT_WRONG_DOCID_COLUMN;
		} else {
			*fts_doc_col_no = i - *num_v;
		}

		if (err && !check_only) {
			my_error(err, MYF(0), field->field_name.str);
		}

		return(true);
	}

	if (!table) {
		return(false);
	}

	/* Not to count the virtual columns */
	i -= *num_v;

	for (; i + DATA_N_SYS_COLS < (uint) table->n_cols; i++) {
		const char*     name = dict_table_get_col_name(table, i);

		if (strcmp(name, FTS_DOC_ID_COL_NAME) == 0) {
#ifdef UNIV_DEBUG
			const dict_col_t*       col;

			col = dict_table_get_nth_col(table, i);

			/* Because the FTS_DOC_ID does not exist in
			the .frm file or TABLE_SHARE, this must be the
			internally created FTS_DOC_ID column. */
			ut_ad(col->mtype == DATA_INT);
			ut_ad(col->len == 8);
			ut_ad(col->prtype & DATA_NOT_NULL);
			ut_ad(col->prtype & DATA_UNSIGNED);
#endif /* UNIV_DEBUG */
			*fts_doc_col_no = i;
			return(true);
		}
	}

	return(false);
}

/** Check whether the table is empty.
@param[in]	table			table to be checked
@param[in]	ignore_delete_marked	Ignore the delete marked
					flag record
@return true if table is empty */
static bool innobase_table_is_empty(const dict_table_t *table,
				    bool ignore_delete_marked=true)
{
  if (!table->space)
    return false;
  dict_index_t *clust_index= dict_table_get_first_index(table);
  mtr_t mtr;
  btr_pcur_t pcur;
  buf_block_t *block;
  page_cur_t *cur;
  const rec_t *rec;
  bool next_page= false;

  mtr.start();
  btr_pcur_open_at_index_side(true, clust_index, BTR_SEARCH_LEAF,
                              &pcur, true, 0, &mtr);
  btr_pcur_move_to_next_user_rec(&pcur, &mtr);
  if (!rec_is_metadata(btr_pcur_get_rec(&pcur), *clust_index))
    btr_pcur_move_to_prev_on_page(&pcur);
scan_leaf:
  cur= btr_pcur_get_page_cur(&pcur);
  page_cur_move_to_next(cur);
next_page:
  if (next_page)
  {
    uint32_t next_page_no= btr_page_get_next(page_cur_get_page(cur));
    if (next_page_no == FIL_NULL)
    {
      mtr.commit();
      return true;
    }

    next_page= false;
    block= page_cur_get_block(cur);
    block= btr_block_get(*clust_index, next_page_no, BTR_SEARCH_LEAF, false,
                         &mtr);
    btr_leaf_page_release(page_cur_get_block(cur), BTR_SEARCH_LEAF, &mtr);
    page_cur_set_before_first(block, cur);
    page_cur_move_to_next(cur);
  }

  rec= page_cur_get_rec(cur);
  if (rec_get_deleted_flag(rec, dict_table_is_comp(table)))
  {
    if (ignore_delete_marked)
      goto scan_leaf;
non_empty:
    mtr.commit();
    return false;
  }
  else if (!page_rec_is_supremum(rec))
    goto non_empty;
  else
  {
    next_page= true;
    goto next_page;
  }
  goto scan_leaf;
}

/** Check if InnoDB supports a particular alter table in-place
@param altered_table TABLE object for new version of table.
@param ha_alter_info Structure describing changes to be done
by ALTER TABLE and holding data used during in-place alter.

@retval HA_ALTER_INPLACE_NOT_SUPPORTED Not supported
@retval HA_ALTER_INPLACE_INSTANT
MDL_EXCLUSIVE is needed for executing prepare_inplace_alter_table()
and commit_inplace_alter_table(). inplace_alter_table() will not be called.
@retval HA_ALTER_INPLACE_COPY_NO_LOCK
MDL_EXCLUSIVE in prepare_inplace_alter_table(), which can be downgraded to
LOCK=NONE for rebuilding the table in inplace_alter_table()
@retval HA_ALTER_INPLACE_COPY_LOCK
MDL_EXCLUSIVE in prepare_inplace_alter_table(), which can be downgraded to
LOCK=SHARED for rebuilding the table in inplace_alter_table()
@retval HA_ALTER_INPLACE_NOCOPY_NO_LOCK
MDL_EXCLUSIVE in prepare_inplace_alter_table(), which can be downgraded to
LOCK=NONE for inplace_alter_table() which will not rebuild the table
@retval HA_ALTER_INPLACE_NOCOPY_LOCK
MDL_EXCLUSIVE in prepare_inplace_alter_table(), which can be downgraded to
LOCK=SHARED for inplace_alter_table() which will not rebuild the table
*/

enum_alter_inplace_result
ha_innobase::check_if_supported_inplace_alter(
	TABLE*			altered_table,
	Alter_inplace_info*	ha_alter_info)
{
	DBUG_ENTER("check_if_supported_inplace_alter");

	if ((ha_alter_info->handler_flags
	     & INNOBASE_ALTER_VERSIONED_REBUILD)
	    && altered_table->versioned(VERS_TIMESTAMP)) {
		ha_alter_info->unsupported_reason =
			"Not implemented for system-versioned timestamp tables";
		DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
	}

	/* Before 10.2.2 information about virtual columns was not stored in
	system tables. We need to do a full alter to rebuild proper 10.2.2+
	metadata with the information about virtual columns */
	if (omits_virtual_cols(*table_share)) {
		DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
	}

	if (altered_table->s->fields > REC_MAX_N_USER_FIELDS) {
		/* Deny the inplace ALTER TABLE. MySQL will try to
		re-create the table and ha_innobase::create() will
		return an error too. This is how we effectively
		deny adding too many columns to a table. */
		ha_alter_info->unsupported_reason =
			my_get_err_msg(ER_TOO_MANY_FIELDS);
		DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
	}

	update_thd();

	if (is_read_only(!high_level_read_only
			 && (ha_alter_info->handler_flags & ALTER_OPTIONS)
			 && ha_alter_info->create_info->key_block_size == 0
			 && ha_alter_info->create_info->row_type
			 != ROW_TYPE_COMPRESSED)) {
		ha_alter_info->unsupported_reason =
			my_get_err_msg(ER_READ_ONLY_MODE);

		DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
	}

	if (ha_alter_info->handler_flags
	    & ~(INNOBASE_INPLACE_IGNORE
		| INNOBASE_ALTER_INSTANT
		| INNOBASE_ALTER_NOREBUILD
		| INNOBASE_ALTER_REBUILD
		| ALTER_INDEX_IGNORABILITY)) {

		if (ha_alter_info->handler_flags
		    & ALTER_STORED_COLUMN_TYPE) {
			ha_alter_info->unsupported_reason = my_get_err_msg(
				ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_COLUMN_TYPE);
		}

		DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
	}

	ut_ad(dict_sys.sys_tables_exist());

	/* Only support online add foreign key constraint when
	check_foreigns is turned off */
	if ((ha_alter_info->handler_flags & ALTER_ADD_FOREIGN_KEY)
	    && m_prebuilt->trx->check_foreigns) {
		ha_alter_info->unsupported_reason = my_get_err_msg(
			ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_FK_CHECK);
		DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
	}

	const char* reason_rebuild = NULL;

	switch (innodb_instant_alter_column_allowed) {
	case 0: /* never */
		if ((ha_alter_info->handler_flags
		     & (ALTER_ADD_STORED_BASE_COLUMN
			| ALTER_STORED_COLUMN_ORDER
			| ALTER_DROP_STORED_COLUMN))
		    || m_prebuilt->table->is_instant()) {
			reason_rebuild =
				"innodb_instant_alter_column_allowed=never";
innodb_instant_alter_column_allowed_reason:
			if (ha_alter_info->handler_flags
			    & ALTER_RECREATE_TABLE) {
				reason_rebuild = NULL;
			} else {
				ha_alter_info->handler_flags
					|= ALTER_RECREATE_TABLE;
				ha_alter_info->unsupported_reason
					= reason_rebuild;
			}
		}
		break;
	case 1: /* add_last */
		if ((ha_alter_info->handler_flags
		     & (ALTER_STORED_COLUMN_ORDER | ALTER_DROP_STORED_COLUMN))
		    || m_prebuilt->table->instant) {
			reason_rebuild = "innodb_instant_atler_column_allowed="
				"add_last";
			goto innodb_instant_alter_column_allowed_reason;
		}
	}

	switch (ha_alter_info->handler_flags & ~INNOBASE_INPLACE_IGNORE) {
	case ALTER_OPTIONS:
		if (alter_options_need_rebuild(ha_alter_info, table)) {
			reason_rebuild = my_get_err_msg(
				ER_ALTER_OPERATION_TABLE_OPTIONS_NEED_REBUILD);
			ha_alter_info->unsupported_reason = reason_rebuild;
			break;
		}
		/* fall through */
	case 0:
		DBUG_RETURN(HA_ALTER_INPLACE_INSTANT);
	}

	/* InnoDB cannot IGNORE when creating unique indexes. IGNORE
	should silently delete some duplicate rows. Our inplace_alter
	code will not delete anything from existing indexes. */
	if (ha_alter_info->ignore
	    && (ha_alter_info->handler_flags
		& (ALTER_ADD_PK_INDEX | ALTER_ADD_UNIQUE_INDEX))) {
		ha_alter_info->unsupported_reason = my_get_err_msg(
			ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_IGNORE);
		DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
	}

	/* DROP PRIMARY KEY is only allowed in combination with ADD
	PRIMARY KEY. */
	if ((ha_alter_info->handler_flags
	     & (ALTER_ADD_PK_INDEX | ALTER_DROP_PK_INDEX))
	    == ALTER_DROP_PK_INDEX) {
		ha_alter_info->unsupported_reason = my_get_err_msg(
			ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_NOPK);
		DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
	}

	if (ha_alter_info->handler_flags & ALTER_COLUMN_NULLABLE) {
		/* If a NOT NULL attribute is going to be removed and
		a UNIQUE INDEX on the column had been promoted to an
		implicit PRIMARY KEY, the table should be rebuilt by
		ALGORITHM=COPY. (Theoretically, we could support
		rebuilding by ALGORITHM=INPLACE if a PRIMARY KEY is
		going to be added, either explicitly or by promoting
		another UNIQUE KEY.) */
		const uint my_primary_key = altered_table->s->primary_key;

		if (UNIV_UNLIKELY(my_primary_key >= MAX_KEY)
		    && !dict_index_is_auto_gen_clust(
			    dict_table_get_first_index(m_prebuilt->table))) {
			ha_alter_info->unsupported_reason = my_get_err_msg(
				ER_PRIMARY_CANT_HAVE_NULL);
			DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
		}
	}

	/*
	  InnoDB in different MariaDB versions was generating different mtype
	  codes for certain types. In some cases the signed/unsigned bit was
	  generated differently too.

	  Inplace ALTER would change the mtype/unsigned_flag (to what the
	  current code generates) without changing the underlying data
	  represenation, and it might result in data corruption.

	  Don't do inplace ALTER if mtype/unsigned_flag are wrong.
	*/
	for (ulint i = 0, icol= 0; i < table->s->fields; i++) {
		const Field*		field = table->field[i];
		const dict_col_t*	col = dict_table_get_nth_col(
			m_prebuilt->table, icol);
		unsigned unsigned_flag;

		if (!field->stored_in_db()) {
			continue;
		}

		icol++;

		if (col->mtype != get_innobase_type_from_mysql_type(
			    &unsigned_flag, field)) {

			DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
		}

		if ((col->prtype & DATA_UNSIGNED) != unsigned_flag) {

			DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
		}
	}

	ulint n_indexes = UT_LIST_GET_LEN((m_prebuilt->table)->indexes);

	/* If InnoDB dictionary and MySQL frm file are not consistent
	use "Copy" method. */
	if (m_prebuilt->table->dict_frm_mismatch) {

		ha_alter_info->unsupported_reason = my_get_err_msg(
			ER_NO_SUCH_INDEX);
		ib_push_frm_error(m_user_thd, m_prebuilt->table, altered_table,
			n_indexes, true);

		DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
	}

	/* '0000-00-00' value isn't allowed for datetime datatype
	for newly added column when table is not empty */
	if (ha_alter_info->error_if_not_empty
	    && m_prebuilt->table->space
	    && !innobase_table_is_empty(m_prebuilt->table)) {
		DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
	}

	const bool add_drop_v_cols = !!(ha_alter_info->handler_flags
					& (ALTER_ADD_VIRTUAL_COLUMN
					   | ALTER_DROP_VIRTUAL_COLUMN
					   | ALTER_VIRTUAL_COLUMN_ORDER));

	/* We should be able to do the operation in-place.
	See if we can do it online (LOCK=NONE) or without rebuild. */
	bool online = true, need_rebuild = false;
	const uint fulltext_indexes = innobase_fulltext_exist(altered_table);

	/* Fix the key parts. */
	for (KEY* new_key = ha_alter_info->key_info_buffer;
	     new_key < ha_alter_info->key_info_buffer
		     + ha_alter_info->key_count;
	     new_key++) {

		/* Do not support adding/droping a virtual column, while
		there is a table rebuild caused by adding a new FTS_DOC_ID */
		if ((new_key->flags & HA_FULLTEXT) && add_drop_v_cols
		    && !DICT_TF2_FLAG_IS_SET(m_prebuilt->table,
					     DICT_TF2_FTS_HAS_DOC_ID)) {
			ha_alter_info->unsupported_reason =
				MSG_UNSUPPORTED_ALTER_ONLINE_ON_VIRTUAL_COLUMN;
			DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
		}

		for (KEY_PART_INFO* key_part = new_key->key_part;
		     key_part < (new_key->key_part
				 + new_key->user_defined_key_parts);
		     key_part++) {
			DBUG_ASSERT(key_part->fieldnr
				    < altered_table->s->fields);

			const Create_field* new_field
				= ha_alter_info->alter_info->create_list.elem(
					key_part->fieldnr);

			DBUG_ASSERT(new_field);

			key_part->field = altered_table->field[
				key_part->fieldnr];

			/* In some special cases InnoDB emits "false"
			duplicate key errors with NULL key values. Let
			us play safe and ensure that we can correctly
			print key values even in such cases. */
			key_part->null_offset = key_part->field->null_offset();
			key_part->null_bit = key_part->field->null_bit;

			if (new_field->field) {
				/* This is an existing column. */
				continue;
			}

			/* This is an added column. */
			DBUG_ASSERT(ha_alter_info->handler_flags
				    & ALTER_ADD_COLUMN);

			/* We cannot replace a hidden FTS_DOC_ID
			with a user-visible FTS_DOC_ID. */
			if (fulltext_indexes && m_prebuilt->table->fts
			    && !my_strcasecmp(
				    system_charset_info,
				    key_part->field->field_name.str,
				    FTS_DOC_ID_COL_NAME)) {
				ha_alter_info->unsupported_reason = my_get_err_msg(
					ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_HIDDEN_FTS);
				DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
			}

			DBUG_ASSERT((key_part->field->unireg_check
				     == Field::NEXT_NUMBER)
				    == !!(key_part->field->flags
					  & AUTO_INCREMENT_FLAG));

			if (key_part->field->flags & AUTO_INCREMENT_FLAG) {
				/* We cannot assign AUTO_INCREMENT values
				during online or instant ALTER. */
				DBUG_ASSERT(key_part->field == altered_table
					    -> found_next_number_field);

				if (ha_alter_info->online) {
					ha_alter_info->unsupported_reason = my_get_err_msg(
						ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_AUTOINC);
				}

				online = false;
				need_rebuild = true;
			}

			if (!key_part->field->stored_in_db()) {
				/* Do not support adding index on newly added
				virtual column, while there is also a drop
				virtual column in the same clause */
				if (ha_alter_info->handler_flags
				    & ALTER_DROP_VIRTUAL_COLUMN) {
					ha_alter_info->unsupported_reason =
						MSG_UNSUPPORTED_ALTER_ONLINE_ON_VIRTUAL_COLUMN;

					DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
				}

				if (ha_alter_info->online
				    && !ha_alter_info->unsupported_reason) {
					ha_alter_info->unsupported_reason =
						MSG_UNSUPPORTED_ALTER_ONLINE_ON_VIRTUAL_COLUMN;
				}

				online = false;
			}
		}
	}

	DBUG_ASSERT(!m_prebuilt->table->fts
		    || (m_prebuilt->table->fts->doc_col <= table->s->fields));

	DBUG_ASSERT(!m_prebuilt->table->fts
		    || (m_prebuilt->table->fts->doc_col
		        < dict_table_get_n_user_cols(m_prebuilt->table)));

	if (fulltext_indexes && m_prebuilt->table->fts) {
		/* FULLTEXT indexes are supposed to remain. */
		/* Disallow DROP INDEX FTS_DOC_ID_INDEX */

		for (uint i = 0; i < ha_alter_info->index_drop_count; i++) {
			if (!my_strcasecmp(
				    system_charset_info,
				    ha_alter_info->index_drop_buffer[i]->name.str,
				    FTS_DOC_ID_INDEX_NAME)) {
				ha_alter_info->unsupported_reason = my_get_err_msg(
					ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_CHANGE_FTS);
				DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
			}
		}

		/* InnoDB can have a hidden FTS_DOC_ID_INDEX on a
		visible FTS_DOC_ID column as well. Prevent dropping or
		renaming the FTS_DOC_ID. */

		for (Field** fp = table->field; *fp; fp++) {
			if (!((*fp)->flags
			      & (FIELD_IS_RENAMED | FIELD_IS_DROPPED))) {
				continue;
			}

			if (!my_strcasecmp(
				    system_charset_info,
				    (*fp)->field_name.str,
				    FTS_DOC_ID_COL_NAME)) {
				ha_alter_info->unsupported_reason = my_get_err_msg(
					ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_CHANGE_FTS);
				DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
			}
		}
	}

	m_prebuilt->trx->will_lock = true;

	/* When changing a NULL column to NOT NULL and specifying a
	DEFAULT value, ensure that the DEFAULT expression is a constant.
	Also, in ADD COLUMN, for now we only support a
	constant DEFAULT expression. */
	Field **af = altered_table->field;
	bool fts_need_rebuild = false;
	need_rebuild = need_rebuild
		|| innobase_need_rebuild(ha_alter_info, table);

	for (Create_field& cf : ha_alter_info->alter_info->create_list) {
		DBUG_ASSERT(cf.field
			    || (ha_alter_info->handler_flags
				& ALTER_ADD_COLUMN));

		if (const Field* f = cf.field) {
			/* An AUTO_INCREMENT attribute can only
			be added to an existing column by ALGORITHM=COPY,
			but we can remove the attribute. */
			ut_ad((*af)->unireg_check != Field::NEXT_NUMBER
			      || f->unireg_check == Field::NEXT_NUMBER);
			if (!f->real_maybe_null() || (*af)->real_maybe_null())
				goto next_column;
			/* We are changing an existing column
			from NULL to NOT NULL. */
			DBUG_ASSERT(ha_alter_info->handler_flags
				    & ALTER_COLUMN_NOT_NULLABLE);
			/* Virtual columns are never NOT NULL. */
			DBUG_ASSERT(f->stored_in_db());
			switch ((*af)->type()) {
			case MYSQL_TYPE_TIMESTAMP:
			case MYSQL_TYPE_TIMESTAMP2:
				/* Inserting NULL into a TIMESTAMP column
				would cause the DEFAULT value to be
				replaced. Ensure that the DEFAULT
				expression is not changing during
				ALTER TABLE. */
				if (!(*af)->default_value
				    && (*af)->is_real_null()) {
					/* No DEFAULT value is
					specified. We can report
					errors for any NULL values for
					the TIMESTAMP. */
					goto next_column;
				}
				break;
			default:
				/* For any other data type, NULL
				values are not converted. */
				goto next_column;
			}

			ha_alter_info->unsupported_reason = my_get_err_msg(
				ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_NOT_NULL);
		} else if (!is_non_const_value(*af)
			   && set_default_value(*af)) {
			if (fulltext_indexes > 1
			    && !my_strcasecmp(system_charset_info,
					      (*af)->field_name.str,
					      FTS_DOC_ID_COL_NAME)) {
				/* If a hidden FTS_DOC_ID column exists
				(because of FULLTEXT INDEX), it cannot
				be replaced with a user-created one
				except when using ALGORITHM=COPY. */
				ha_alter_info->unsupported_reason =
					my_get_err_msg(ER_INNODB_FT_LIMIT);
				DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
			}
			goto next_column;
		}

		DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);

next_column:
		af++;
	}

	const bool supports_instant = instant_alter_column_possible(
		*m_prebuilt->table, ha_alter_info, table, altered_table,
		is_innodb_strict_mode());
	if (add_drop_v_cols) {
		ulonglong flags = ha_alter_info->handler_flags;

		/* TODO: uncomment the flags below, once we start to
		support them */

		flags &= ~(ALTER_ADD_VIRTUAL_COLUMN
			   | ALTER_DROP_VIRTUAL_COLUMN
			   | ALTER_VIRTUAL_COLUMN_ORDER
		           | ALTER_VIRTUAL_GCOL_EXPR
		           | ALTER_COLUMN_VCOL
		/*
			   | ALTER_ADD_STORED_BASE_COLUMN
			   | ALTER_DROP_STORED_COLUMN
			   | ALTER_STORED_COLUMN_ORDER
			   | ALTER_ADD_UNIQUE_INDEX
		*/
			   | ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX
			   | ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX
			   | ALTER_INDEX_ORDER);
		if (supports_instant) {
			flags &= ~(ALTER_DROP_STORED_COLUMN
#if 0 /* MDEV-17468: remove check_v_col_in_order() and fix the code */
				   | ALTER_ADD_STORED_BASE_COLUMN
#endif
				   | ALTER_STORED_COLUMN_ORDER);
		}
		if (flags != 0
		    || IF_PARTITIONING((altered_table->s->partition_info_str
			&& altered_table->s->partition_info_str_len), 0)
		    || (!check_v_col_in_order(
			this->table, altered_table, ha_alter_info))) {
			ha_alter_info->unsupported_reason =
				MSG_UNSUPPORTED_ALTER_ONLINE_ON_VIRTUAL_COLUMN;
			DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
		}
	}

	if (supports_instant && !(ha_alter_info->handler_flags
				  & INNOBASE_ALTER_NOREBUILD)) {
		DBUG_RETURN(HA_ALTER_INPLACE_INSTANT);
	}

	if (need_rebuild
	    && (fulltext_indexes
		|| innobase_spatial_exist(altered_table)
		|| innobase_indexed_virtual_exist(altered_table))) {
		/* If the table already contains fulltext indexes,
		refuse to rebuild the table natively altogether. */
		if (fulltext_indexes > 1) {
cannot_create_many_fulltext_index:
			ha_alter_info->unsupported_reason =
				my_get_err_msg(ER_INNODB_FT_LIMIT);
			DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
		}

		if (!online || !ha_alter_info->online
		    || ha_alter_info->unsupported_reason != reason_rebuild) {
			/* Either LOCK=NONE was not requested, or we already
			gave specific reason to refuse it. */
		} else if (fulltext_indexes) {
			ha_alter_info->unsupported_reason = my_get_err_msg(
				ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_FTS);
		} else if (innobase_spatial_exist(altered_table)) {
			ha_alter_info->unsupported_reason = my_get_err_msg(
				ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_GIS);
		} else {
			/* MDEV-14341 FIXME: Remove this limitation. */
			ha_alter_info->unsupported_reason =
				"online rebuild with indexed virtual columns";
		}

		online = false;
	}

	if (ha_alter_info->handler_flags
		& ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX) {
		/* ADD FULLTEXT|SPATIAL INDEX requires a lock.

		We could do ADD FULLTEXT INDEX without a lock if the
		table already contains an FTS_DOC_ID column, but in
		that case we would have to apply the modification log
		to the full-text indexes.

		We could also do ADD SPATIAL INDEX by implementing
		row_log_apply() for it. */
		bool add_fulltext = false;

		for (uint i = 0; i < ha_alter_info->index_add_count; i++) {
			const KEY* key =
				&ha_alter_info->key_info_buffer[
					ha_alter_info->index_add_buffer[i]];
			if (key->flags & HA_FULLTEXT) {
				DBUG_ASSERT(!(key->flags & HA_KEYFLAG_MASK
					      & ~(HA_FULLTEXT
						  | HA_PACK_KEY
						  | HA_GENERATED_KEY
						  | HA_BINARY_PACK_KEY)));
				if (add_fulltext) {
					goto cannot_create_many_fulltext_index;
				}

				add_fulltext = true;
				if (ha_alter_info->online
				    && !ha_alter_info->unsupported_reason) {
					ha_alter_info->unsupported_reason = my_get_err_msg(
						ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_FTS);
				}

				online = false;

				/* Full text search index exists, check
				whether the table already has DOC ID column.
				If not, InnoDB have to rebuild the table to
				add a Doc ID hidden column and change
				primary index. */
				ulint	fts_doc_col_no;
				ulint	num_v = 0;

				fts_need_rebuild =
					!innobase_fts_check_doc_id_col(
						m_prebuilt->table,
						altered_table,
						&fts_doc_col_no, &num_v, true);
			}

			if (online && (key->flags & HA_SPATIAL)) {

				if (ha_alter_info->online) {
					ha_alter_info->unsupported_reason = my_get_err_msg(
						ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_GIS);
				}

				online = false;
			}
		}
	}

	// FIXME: implement Online DDL for system-versioned operations
	if (ha_alter_info->handler_flags & INNOBASE_ALTER_VERSIONED_REBUILD) {

		if (ha_alter_info->online) {
			ha_alter_info->unsupported_reason =
				"Not implemented for system-versioned operations";
		}

		online = false;
	}

	if ((need_rebuild && !supports_instant) || fts_need_rebuild) {
		ha_alter_info->handler_flags |= ALTER_RECREATE_TABLE;
		DBUG_RETURN(online
			    ? HA_ALTER_INPLACE_COPY_NO_LOCK
			    : HA_ALTER_INPLACE_COPY_LOCK);
	}

	if (ha_alter_info->unsupported_reason) {
	} else if (ha_alter_info->handler_flags & INNOBASE_ONLINE_CREATE) {
		ha_alter_info->unsupported_reason = "ADD INDEX";
	} else {
		ha_alter_info->unsupported_reason = "DROP INDEX";
	}

	DBUG_RETURN(online
		    ? HA_ALTER_INPLACE_NOCOPY_NO_LOCK
		    : HA_ALTER_INPLACE_NOCOPY_LOCK);
}

/*************************************************************//**
Initialize the dict_foreign_t structure with supplied info
@return true if added, false if duplicate foreign->id */
static MY_ATTRIBUTE((nonnull(1,3,5,7)))
bool
innobase_init_foreign(
/*==================*/
	dict_foreign_t*	foreign,		/*!< in/out: structure to
						initialize */
	const char*	constraint_name,	/*!< in/out: constraint name if
						exists */
	dict_table_t*	table,			/*!< in: foreign table */
	dict_index_t*	index,			/*!< in: foreign key index */
	const char**	column_names,		/*!< in: foreign key column
						names */
	ulint		num_field,		/*!< in: number of columns */
	const char*	referenced_table_name,	/*!< in: referenced table
						name */
	dict_table_t*	referenced_table,	/*!< in: referenced table */
	dict_index_t*	referenced_index,	/*!< in: referenced index */
	const char**	referenced_column_names,/*!< in: referenced column
						names */
	ulint		referenced_num_field)	/*!< in: number of referenced
						columns */
{
	ut_ad(dict_sys.locked());

        if (constraint_name) {
                ulint   db_len;

                /* Catenate 'databasename/' to the constraint name specified
                by the user: we conceive the constraint as belonging to the
                same MySQL 'database' as the table itself. We store the name
                to foreign->id. */

                db_len = dict_get_db_name_len(table->name.m_name);

                foreign->id = static_cast<char*>(mem_heap_alloc(
                        foreign->heap, db_len + strlen(constraint_name) + 2));

                memcpy(foreign->id, table->name.m_name, db_len);
                foreign->id[db_len] = '/';
                strcpy(foreign->id + db_len + 1, constraint_name);

		/* Check if any existing foreign key has the same id,
		this is needed only if user supplies the constraint name */

		if (table->foreign_set.find(foreign)
		    != table->foreign_set.end()) {
			return(false);
		}
        }

        foreign->foreign_table = table;
        foreign->foreign_table_name = mem_heap_strdup(
                foreign->heap, table->name.m_name);
        dict_mem_foreign_table_name_lookup_set(foreign, TRUE);

        foreign->foreign_index = index;
        foreign->n_fields = static_cast<unsigned>(num_field)
		& dict_index_t::MAX_N_FIELDS;

        foreign->foreign_col_names = static_cast<const char**>(
                mem_heap_alloc(foreign->heap, num_field * sizeof(void*)));

        for (ulint i = 0; i < foreign->n_fields; i++) {
                foreign->foreign_col_names[i] = mem_heap_strdup(
                        foreign->heap, column_names[i]);
        }

	foreign->referenced_index = referenced_index;
	foreign->referenced_table = referenced_table;

	foreign->referenced_table_name = mem_heap_strdup(
		foreign->heap, referenced_table_name);
        dict_mem_referenced_table_name_lookup_set(foreign, TRUE);

        foreign->referenced_col_names = static_cast<const char**>(
                mem_heap_alloc(foreign->heap,
			       referenced_num_field * sizeof(void*)));

        for (ulint i = 0; i < foreign->n_fields; i++) {
                foreign->referenced_col_names[i]
                        = mem_heap_strdup(foreign->heap,
					  referenced_column_names[i]);
        }

	return(true);
}

/*************************************************************//**
Check whether the foreign key options is legit
@return true if it is */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
innobase_check_fk_option(
/*=====================*/
	const dict_foreign_t*	foreign)	/*!< in: foreign key */
{
	if (!foreign->foreign_index) {
		return(true);
	}

	if (foreign->type & (DICT_FOREIGN_ON_UPDATE_SET_NULL
			     | DICT_FOREIGN_ON_DELETE_SET_NULL)) {

		for (ulint j = 0; j < foreign->n_fields; j++) {
			if ((dict_index_get_nth_col(
				     foreign->foreign_index, j)->prtype)
			    & DATA_NOT_NULL) {

				/* It is not sensible to define
				SET NULL if the column is not
				allowed to be NULL! */
				return(false);
			}
		}
	}

	return(true);
}

/*************************************************************//**
Set foreign key options
@return true if successfully set */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
innobase_set_foreign_key_option(
/*============================*/
	dict_foreign_t*	foreign,	/*!< in:InnoDB Foreign key */
	Foreign_key*	fk_key)		/*!< in: Foreign key info from
					MySQL */
{
	ut_ad(!foreign->type);

	switch (fk_key->delete_opt) {
	case FK_OPTION_NO_ACTION:
	case FK_OPTION_RESTRICT:
	case FK_OPTION_SET_DEFAULT:
		foreign->type = DICT_FOREIGN_ON_DELETE_NO_ACTION;
		break;
	case FK_OPTION_CASCADE:
		foreign->type = DICT_FOREIGN_ON_DELETE_CASCADE;
		break;
	case FK_OPTION_SET_NULL:
		foreign->type = DICT_FOREIGN_ON_DELETE_SET_NULL;
		break;
	case FK_OPTION_UNDEF:
		break;
	}

	switch (fk_key->update_opt) {
	case FK_OPTION_NO_ACTION:
	case FK_OPTION_RESTRICT:
	case FK_OPTION_SET_DEFAULT:
		foreign->type |= DICT_FOREIGN_ON_UPDATE_NO_ACTION;
		break;
	case FK_OPTION_CASCADE:
		foreign->type |= DICT_FOREIGN_ON_UPDATE_CASCADE;
		break;
	case FK_OPTION_SET_NULL:
		foreign->type |= DICT_FOREIGN_ON_UPDATE_SET_NULL;
		break;
	case FK_OPTION_UNDEF:
		break;
	}

	return(innobase_check_fk_option(foreign));
}

/*******************************************************************//**
Check if a foreign key constraint can make use of an index
that is being created.
@param[in]	col_names	column names
@param[in]	n_cols		number of columns
@param[in]	keys		index information
@param[in]	add		indexes being created
@return useable index, or NULL if none found */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
const KEY*
innobase_find_equiv_index(
	const char*const*	col_names,
	uint			n_cols,
	const KEY*		keys,
	span<uint>		add)
{
	for (span<uint>::iterator it = add.begin(), end = add.end(); it != end;
	     ++it) {
		const KEY*	key = &keys[*it];

		if (key->user_defined_key_parts < n_cols
		    || key->flags & HA_SPATIAL) {
no_match:
			continue;
		}

		for (uint j = 0; j < n_cols; j++) {
			const KEY_PART_INFO&	key_part = key->key_part[j];
			uint32			col_len
				= key_part.field->pack_length();

			/* Any index on virtual columns cannot be used
			for reference constraint */
			if (!key_part.field->stored_in_db()) {
				goto no_match;
			}

			/* The MySQL pack length contains 1 or 2 bytes
			length field for a true VARCHAR. */

			if (key_part.field->type() == MYSQL_TYPE_VARCHAR) {
				col_len -= static_cast<const Field_varstring*>(
					key_part.field)->length_bytes;
			}

			if (key_part.length < col_len) {

				/* Column prefix indexes cannot be
				used for FOREIGN KEY constraints. */
				goto no_match;
			}

			if (innobase_strcasecmp(col_names[j],
						key_part.field->field_name.str)) {
				/* Name mismatch */
				goto no_match;
			}
		}

		return(key);
	}

	return(NULL);
}

/*************************************************************//**
Find an index whose first fields are the columns in the array
in the same order and is not marked for deletion
@return matching index, NULL if not found */
static MY_ATTRIBUTE((nonnull(1,4), warn_unused_result))
dict_index_t*
innobase_find_fk_index(
/*===================*/
	dict_table_t*		table,	/*!< in: table */
	const char**		col_names,
					/*!< in: column names, or NULL
					to use table->col_names */
	span<dict_index_t*>	drop_index,
					/*!< in: indexes to be dropped */
	const char**		columns,/*!< in: array of column names */
	ulint			n_cols) /*!< in: number of columns */
{
	dict_index_t*	index;

	index = dict_table_get_first_index(table);

	while (index != NULL) {
		if (dict_foreign_qualify_index(table, col_names, columns,
					       n_cols, index, NULL, true, 0,
					       NULL, NULL, NULL)
		    && std::find(drop_index.begin(), drop_index.end(), index)
			   == drop_index.end()) {
			return index;
		}

		index = dict_table_get_next_index(index);
	}

	return(NULL);
}

/** Check whether given column is a base of stored column.
@param[in]	col_name	column name
@param[in]	table		table
@param[in]	s_cols		list of stored columns
@return true if the given column is a base of stored column,else false. */
static
bool
innobase_col_check_fk(
	const char*		col_name,
	const dict_table_t*	table,
	dict_s_col_list*	s_cols)
{
	dict_s_col_list::const_iterator	it;

	for (it = s_cols->begin(); it != s_cols->end(); ++it) {
		for (ulint j = it->num_base; j--; ) {
			if (!strcmp(col_name, dict_table_get_col_name(
					    table, it->base_col[j]->ind))) {
				return(true);
			}
		}
	}

	return(false);
}

/** Check whether the foreign key constraint is on base of any stored columns.
@param[in]	foreign	Foriegn key constraing information
@param[in]	table	table to which the foreign key objects
to be added
@param[in]	s_cols	list of stored column information in the table.
@return true if yes, otherwise false. */
static
bool
innobase_check_fk_stored(
	const dict_foreign_t*	foreign,
	const dict_table_t*	table,
	dict_s_col_list*	s_cols)
{
	ulint	type = foreign->type;

	type &= ~(DICT_FOREIGN_ON_DELETE_NO_ACTION
		  | DICT_FOREIGN_ON_UPDATE_NO_ACTION);

	if (type == 0 || s_cols == NULL) {
		return(false);
	}

	for (ulint i = 0; i < foreign->n_fields; i++) {
		if (innobase_col_check_fk(
			foreign->foreign_col_names[i], table, s_cols)) {
			return(true);
		}
	}

	return(false);
}

/** Create InnoDB foreign key structure from MySQL alter_info
@param[in]	ha_alter_info	alter table info
@param[in]	table_share	TABLE_SHARE
@param[in]	table		table object
@param[in]	col_names	column names, or NULL to use
table->col_names
@param[in]	drop_index	indexes to be dropped
@param[in]	n_drop_index	size of drop_index
@param[out]	add_fk		foreign constraint added
@param[out]	n_add_fk	number of foreign constraints
added
@param[in]	trx		user transaction
@param[in]	s_cols		list of stored column information
@retval true if successful
@retval false on error (will call my_error()) */
static MY_ATTRIBUTE((nonnull(1,2,3,7,8), warn_unused_result))
bool
innobase_get_foreign_key_info(
	Alter_inplace_info*
			ha_alter_info,
	const TABLE_SHARE*
			table_share,
	dict_table_t*	table,
	const char**	col_names,
	dict_index_t**	drop_index,
	ulint		n_drop_index,
	dict_foreign_t**add_fk,
	ulint*		n_add_fk,
	const trx_t*	trx,
	dict_s_col_list*s_cols)
{
	dict_table_t*	referenced_table = NULL;
	char*		referenced_table_name = NULL;
	ulint		num_fk = 0;
	Alter_info*	alter_info = ha_alter_info->alter_info;
	const CHARSET_INFO*	cs = thd_charset(trx->mysql_thd);

	DBUG_ENTER("innobase_get_foreign_key_info");

	*n_add_fk = 0;

	for (Key& key : alter_info->key_list) {
		if (key.type != Key::FOREIGN_KEY) {
			continue;
		}

		const char*	column_names[MAX_NUM_FK_COLUMNS];
		dict_index_t*	index = NULL;
		const char*	referenced_column_names[MAX_NUM_FK_COLUMNS];
		dict_index_t*	referenced_index = NULL;
		ulint		num_col = 0;
		ulint		referenced_num_col = 0;
		bool		correct_option;

		Foreign_key* fk_key = static_cast<Foreign_key*>(&key);

		if (fk_key->columns.elements > 0) {
			ulint	i = 0;

			/* Get all the foreign key column info for the
			current table */
			for (const Key_part_spec& column : fk_key->columns) {
				column_names[i] = column.field_name.str;
				ut_ad(i < MAX_NUM_FK_COLUMNS);
				i++;
			}

			index = innobase_find_fk_index(
				table, col_names,
				span<dict_index_t*>(drop_index, n_drop_index),
				column_names, i);

			/* MySQL would add a index in the creation
			list if no such index for foreign table,
			so we have to use DBUG_EXECUTE_IF to simulate
			the scenario */
			DBUG_EXECUTE_IF("innodb_test_no_foreign_idx",
					index = NULL;);

			/* Check whether there exist such
			index in the the index create clause */
			if (!index && !innobase_find_equiv_index(
				    column_names, static_cast<uint>(i),
				    ha_alter_info->key_info_buffer,
				    span<uint>(ha_alter_info->index_add_buffer,
					       ha_alter_info->index_add_count))) {
				my_error(
					ER_FK_NO_INDEX_CHILD,
					MYF(0),
					fk_key->name.str
					? fk_key->name.str : "",
					table_share->table_name.str);
				goto err_exit;
			}

			num_col = i;
		}

		add_fk[num_fk] = dict_mem_foreign_create();

		dict_sys.lock(SRW_LOCK_CALL);

		referenced_table_name = dict_get_referenced_table(
			table->name.m_name,
			LEX_STRING_WITH_LEN(fk_key->ref_db),
			LEX_STRING_WITH_LEN(fk_key->ref_table),
			&referenced_table,
			add_fk[num_fk]->heap, cs);

		/* Test the case when referenced_table failed to
		open, if trx->check_foreigns is not set, we should
		still be able to add the foreign key */
		DBUG_EXECUTE_IF("innodb_test_open_ref_fail",
				referenced_table = NULL;);

		if (!referenced_table && trx->check_foreigns) {
			my_error(ER_FK_CANNOT_OPEN_PARENT,
				 MYF(0), fk_key->ref_table.str);
			goto err_exit_unlock;
		}

		if (fk_key->ref_columns.elements > 0) {
			ulint	i = 0;

			for (Key_part_spec &column : fk_key->ref_columns) {
				referenced_column_names[i] =
					column.field_name.str;
				ut_ad(i < MAX_NUM_FK_COLUMNS);
				i++;
			}

			if (referenced_table) {
				referenced_index =
					dict_foreign_find_index(
						referenced_table, 0,
						referenced_column_names,
						i, index,
						TRUE, FALSE,
						NULL, NULL, NULL);

				DBUG_EXECUTE_IF(
					"innodb_test_no_reference_idx",
					referenced_index = NULL;);

				/* Check whether there exist such
				index in the the index create clause */
				if (!referenced_index) {
					my_error(ER_FK_NO_INDEX_PARENT, MYF(0),
						 fk_key->name.str
						 ? fk_key->name.str : "",
						 fk_key->ref_table.str);
					goto err_exit_unlock;
				}
			} else {
				ut_a(!trx->check_foreigns);
			}

			referenced_num_col = i;
		} else {
			/* Not possible to add a foreign key without a
			referenced column */
			my_error(ER_CANNOT_ADD_FOREIGN, MYF(0),
				 fk_key->ref_table.str);
			goto err_exit_unlock;
		}

		if (!innobase_init_foreign(
			    add_fk[num_fk], fk_key->name.str,
			    table, index, column_names,
			    num_col, referenced_table_name,
			    referenced_table, referenced_index,
			    referenced_column_names, referenced_num_col)) {
			my_error(
				ER_DUP_CONSTRAINT_NAME,
				MYF(0),
                                "FOREIGN KEY", add_fk[num_fk]->id);
			goto err_exit_unlock;
		}

		dict_sys.unlock();

		correct_option = innobase_set_foreign_key_option(
			add_fk[num_fk], fk_key);

		DBUG_EXECUTE_IF("innodb_test_wrong_fk_option",
				correct_option = false;);

		if (!correct_option) {
			my_error(ER_FK_INCORRECT_OPTION,
				 MYF(0),
				 table_share->table_name.str,
				 add_fk[num_fk]->id);
			goto err_exit;
		}

		if (innobase_check_fk_stored(
			add_fk[num_fk], table, s_cols)) {
			my_printf_error(
				HA_ERR_UNSUPPORTED,
				"Cannot add foreign key on the base column "
				"of stored column", MYF(0));
			goto err_exit;
		}

		num_fk++;
	}

	*n_add_fk = num_fk;

	DBUG_RETURN(true);
err_exit_unlock:
	dict_sys.unlock();
err_exit:
	for (ulint i = 0; i <= num_fk; i++) {
		if (add_fk[i]) {
			dict_foreign_free(add_fk[i]);
		}
	}

	DBUG_RETURN(false);
}

/*************************************************************//**
Copies an InnoDB column to a MySQL field.  This function is
adapted from row_sel_field_store_in_mysql_format(). */
static
void
innobase_col_to_mysql(
/*==================*/
	const dict_col_t*	col,	/*!< in: InnoDB column */
	const uchar*		data,	/*!< in: InnoDB column data */
	ulint			len,	/*!< in: length of data, in bytes */
	Field*			field)	/*!< in/out: MySQL field */
{
	uchar*	ptr;
	uchar*	dest	= field->ptr;
	ulint	flen	= field->pack_length();

	switch (col->mtype) {
	case DATA_INT:
		ut_ad(len == flen);

		/* Convert integer data from Innobase to little-endian
		format, sign bit restored to normal */

		for (ptr = dest + len; ptr != dest; ) {
			*--ptr = *data++;
		}

		if (!(col->prtype & DATA_UNSIGNED)) {
			((byte*) dest)[len - 1] ^= 0x80;
		}

		break;

	case DATA_VARCHAR:
	case DATA_VARMYSQL:
	case DATA_BINARY:
		field->reset();

		if (field->type() == MYSQL_TYPE_VARCHAR) {
			/* This is a >= 5.0.3 type true VARCHAR. Store the
			length of the data to the first byte or the first
			two bytes of dest. */

			dest = row_mysql_store_true_var_len(
				dest, len, flen - field->key_length());
		}

		/* Copy the actual data */
		memcpy(dest, data, len);
		break;

	case DATA_GEOMETRY:
	case DATA_BLOB:
		/* Skip MySQL BLOBs when reporting an erroneous row
		during index creation or table rebuild. */
		field->set_null();
		break;

#ifdef UNIV_DEBUG
	case DATA_MYSQL:
		ut_ad(flen >= len);
		ut_ad(col->mbmaxlen >= col->mbminlen);
		memcpy(dest, data, len);
		break;

	default:
	case DATA_SYS_CHILD:
	case DATA_SYS:
		/* These column types should never be shipped to MySQL. */
		ut_ad(0);
		/* fall through */
	case DATA_FLOAT:
	case DATA_DOUBLE:
	case DATA_DECIMAL:
		/* Above are the valid column types for MySQL data. */
		ut_ad(flen == len);
		/* fall through */
	case DATA_FIXBINARY:
	case DATA_CHAR:
		/* We may have flen > len when there is a shorter
		prefix on the CHAR and BINARY column. */
		ut_ad(flen >= len);
#else /* UNIV_DEBUG */
	default:
#endif /* UNIV_DEBUG */
		memcpy(dest, data, len);
	}
}

/*************************************************************//**
Copies an InnoDB record to table->record[0]. */
void
innobase_rec_to_mysql(
/*==================*/
	struct TABLE*		table,	/*!< in/out: MySQL table */
	const rec_t*		rec,	/*!< in: record */
	const dict_index_t*	index,	/*!< in: index */
	const rec_offs*		offsets)/*!< in: rec_get_offsets(
					rec, index, ...) */
{
	uint	n_fields	= table->s->fields;

	ut_ad(n_fields == dict_table_get_n_user_cols(index->table)
	      - !!(DICT_TF2_FLAG_IS_SET(index->table,
					DICT_TF2_FTS_HAS_DOC_ID)));

	for (uint i = 0; i < n_fields; i++) {
		Field*		field	= table->field[i];
		ulint		ipos;
		ulint		ilen;
		const uchar*	ifield;
		ulint prefix_col;

		field->reset();

		ipos = dict_index_get_nth_col_or_prefix_pos(
			index, i, true, false, &prefix_col);

		if (ipos == ULINT_UNDEFINED
		    || rec_offs_nth_extern(offsets, ipos)) {
null_field:
			field->set_null();
			continue;
		}

		ifield = rec_get_nth_cfield(rec, index, offsets, ipos, &ilen);

		/* Assign the NULL flag */
		if (ilen == UNIV_SQL_NULL) {
			ut_ad(field->real_maybe_null());
			goto null_field;
		}

		field->set_notnull();

		innobase_col_to_mysql(
			dict_field_get_col(
				dict_index_get_nth_field(index, ipos)),
			ifield, ilen, field);
	}
}

/*************************************************************//**
Copies an InnoDB index entry to table->record[0].
This is used in preparation for print_keydup_error() from
inline add index */
void
innobase_fields_to_mysql(
/*=====================*/
	struct TABLE*		table,	/*!< in/out: MySQL table */
	const dict_index_t*	index,	/*!< in: InnoDB index */
	const dfield_t*		fields)	/*!< in: InnoDB index fields */
{
	uint	n_fields	= table->s->fields;
	ulint	num_v 		= 0;

	ut_ad(n_fields == dict_table_get_n_user_cols(index->table)
	      + dict_table_get_n_v_cols(index->table)
	      - !!(DICT_TF2_FLAG_IS_SET(index->table,
					DICT_TF2_FTS_HAS_DOC_ID)));

	for (uint i = 0; i < n_fields; i++) {
		Field*		field	= table->field[i];
		ulint		ipos;
		ulint		prefix_col;

		field->reset();

		const bool is_v = !field->stored_in_db();
		const ulint col_n = is_v ? num_v++ : i - num_v;

		ipos = dict_index_get_nth_col_or_prefix_pos(
			index, col_n, true, is_v, &prefix_col);

		if (ipos == ULINT_UNDEFINED
		    || dfield_is_ext(&fields[ipos])
		    || dfield_is_null(&fields[ipos])) {

			field->set_null();
		} else {
			field->set_notnull();

			const dfield_t*	df	= &fields[ipos];

			innobase_col_to_mysql(
				dict_field_get_col(
					dict_index_get_nth_field(index, ipos)),
				static_cast<const uchar*>(dfield_get_data(df)),
				dfield_get_len(df), field);
		}
	}
}

/*************************************************************//**
Copies an InnoDB row to table->record[0].
This is used in preparation for print_keydup_error() from
row_log_table_apply() */
void
innobase_row_to_mysql(
/*==================*/
	struct TABLE*		table,	/*!< in/out: MySQL table */
	const dict_table_t*	itab,	/*!< in: InnoDB table */
	const dtuple_t*		row)	/*!< in: InnoDB row */
{
	uint	n_fields = table->s->fields;
	ulint	num_v = 0;

	/* The InnoDB row may contain an extra FTS_DOC_ID column at the end. */
	ut_ad(row->n_fields == dict_table_get_n_cols(itab));
	ut_ad(n_fields == row->n_fields - DATA_N_SYS_COLS
	      + dict_table_get_n_v_cols(itab)
	      - !!(DICT_TF2_FLAG_IS_SET(itab, DICT_TF2_FTS_HAS_DOC_ID)));

	for (uint i = 0; i < n_fields; i++) {
		Field*		field	= table->field[i];

		field->reset();

		if (!field->stored_in_db()) {
			/* Virtual column are not stored in InnoDB table, so
			skip it */
			num_v++;
			continue;
		}

		const dfield_t*	df	= dtuple_get_nth_field(row, i - num_v);

		if (dfield_is_ext(df) || dfield_is_null(df)) {
			field->set_null();
		} else {
			field->set_notnull();

			innobase_col_to_mysql(
				dict_table_get_nth_col(itab, i - num_v),
				static_cast<const uchar*>(dfield_get_data(df)),
				dfield_get_len(df), field);
		}
	}
	if (table->vfield) {
		MY_BITMAP*	old_read_set = tmp_use_all_columns(table, &table->read_set);
		table->update_virtual_fields(table->file, VCOL_UPDATE_FOR_READ);
		tmp_restore_column_map(&table->read_set, old_read_set);
	}
}

/*******************************************************************//**
This function checks that index keys are sensible.
@return 0 or error number */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
int
innobase_check_index_keys(
/*======================*/
	const Alter_inplace_info*	info,
				/*!< in: indexes to be created or dropped */
	const dict_table_t*		innodb_table)
				/*!< in: Existing indexes */
{
	for (uint key_num = 0; key_num < info->index_add_count;
	     key_num++) {
		const KEY&	key = info->key_info_buffer[
			info->index_add_buffer[key_num]];

		/* Check that the same index name does not appear
		twice in indexes to be created. */

		for (ulint i = 0; i < key_num; i++) {
			const KEY&	key2 = info->key_info_buffer[
				info->index_add_buffer[i]];

			if (0 == strcmp(key.name.str, key2.name.str)) {
				my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0),
					 key.name.str);

				return(ER_WRONG_NAME_FOR_INDEX);
			}
		}

		/* Check that the same index name does not already exist. */

		const dict_index_t* index;

		for (index = dict_table_get_first_index(innodb_table);
		     index; index = dict_table_get_next_index(index)) {

			if (index->is_committed()
			    && !strcmp(key.name.str, index->name)) {
				break;
			}
		}

		/* Now we are in a situation where we have "ADD INDEX x"
		and an index by the same name already exists. We have 4
		possible cases:
		1. No further clauses for an index x are given. Should reject
		the operation.
		2. "DROP INDEX x" is given. Should allow the operation.
		3. "RENAME INDEX x TO y" is given. Should allow the operation.
		4. "DROP INDEX x, RENAME INDEX x TO y" is given. Should allow
		the operation, since no name clash occurs. In this particular
		case MySQL cancels the operation without calling InnoDB
		methods. */

		if (index) {
			/* If a key by the same name is being created and
			dropped, the name clash is OK. */
			for (uint i = 0; i < info->index_drop_count;
			     i++) {
				const KEY*	drop_key
					= info->index_drop_buffer[i];

				if (0 == strcmp(key.name.str,
                                                drop_key->name.str)) {
					goto name_ok;
				}
			}

			for (const Alter_inplace_info::Rename_key_pair& pair :
			     info->rename_keys) {
				if (0 == strcmp(key.name.str,
                                                pair.old_key->name.str)) {
					goto name_ok;
				}
			}

			my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0),
                                 key.name.str);
			return(ER_WRONG_NAME_FOR_INDEX);
		}

name_ok:
		for (ulint i = 0; i < key.user_defined_key_parts; i++) {
			const KEY_PART_INFO&	key_part1
				= key.key_part[i];
			const Field*		field
				= key_part1.field;
			unsigned		is_unsigned;

			switch (get_innobase_type_from_mysql_type(
					&is_unsigned, field)) {
			default:
				break;
			case DATA_INT:
			case DATA_FLOAT:
			case DATA_DOUBLE:
			case DATA_DECIMAL:
				/* Check that MySQL does not try to
				create a column prefix index field on
				an inappropriate data type. */

				if (field->type() == MYSQL_TYPE_VARCHAR) {
					if (key_part1.length
					    >= field->pack_length()
					    - ((Field_varstring*) field)
					    ->length_bytes) {
						break;
					}
				} else {
					if (key_part1.length
					    >= field->pack_length()) {
						break;
					}
				}

				my_error(ER_WRONG_KEY_COLUMN, MYF(0), "InnoDB",
					 field->field_name.str);
				return(ER_WRONG_KEY_COLUMN);
			}

			/* Check that the same column does not appear
			twice in the index. */

			for (ulint j = 0; j < i; j++) {
				const KEY_PART_INFO&	key_part2
					= key.key_part[j];

				if (key_part1.fieldnr != key_part2.fieldnr) {
					continue;
				}

				my_error(ER_WRONG_KEY_COLUMN, MYF(0), "InnoDB",
					 field->field_name.str);
				return(ER_WRONG_KEY_COLUMN);
			}
		}
	}

	return(0);
}

/** Create index field definition for key part
@param[in]	new_clustered	true if alter is generating a new clustered
index
@param[in]	altered_table	MySQL table that is being altered
@param[in]	key_part	MySQL key definition
@param[out]	index_field	index field definition for key_part */
static MY_ATTRIBUTE((nonnull))
void
innobase_create_index_field_def(
	bool			new_clustered,
	const TABLE*		altered_table,
	const KEY_PART_INFO*	key_part,
	index_field_t*		index_field)
{
	const Field*	field;
	unsigned	is_unsigned;
	unsigned	num_v = 0;

	DBUG_ENTER("innobase_create_index_field_def");

	field = new_clustered
		? altered_table->field[key_part->fieldnr]
		: key_part->field;

	for (ulint i = 0; i < key_part->fieldnr; i++) {
		if (!altered_table->field[i]->stored_in_db()) {
			num_v++;
		}
	}

	auto col_type = get_innobase_type_from_mysql_type(
		&is_unsigned, field);

	if ((index_field->is_v_col = !field->stored_in_db())) {
		index_field->col_no = num_v;
	} else {
		index_field->col_no = key_part->fieldnr - num_v;
	}

	index_field->descending= !!(key_part->key_part_flag & HA_REVERSE_SORT);

	if (DATA_LARGE_MTYPE(col_type)
	    || (key_part->length < field->pack_length()
		&& field->type() != MYSQL_TYPE_VARCHAR)
	    || (field->type() == MYSQL_TYPE_VARCHAR
		&& key_part->length < field->pack_length()
			- ((Field_varstring*) field)->length_bytes)) {

		index_field->prefix_len = key_part->length;
	} else {
		index_field->prefix_len = 0;
	}

	DBUG_VOID_RETURN;
}

/** Create index definition for key
@param[in]	altered_table		MySQL table that is being altered
@param[in]	keys			key definitions
@param[in]	key_number		MySQL key number
@param[in]	new_clustered		true if generating a new clustered
index on the table
@param[in]	key_clustered		true if this is the new clustered index
@param[out]	index			index definition
@param[in]	heap			heap where memory is allocated */
static MY_ATTRIBUTE((nonnull))
void
innobase_create_index_def(
	const TABLE*		altered_table,
	const KEY*		keys,
	ulint			key_number,
	bool			new_clustered,
	bool			key_clustered,
	index_def_t*		index,
	mem_heap_t*		heap)
{
	const KEY*	key = &keys[key_number];
	ulint		i;
	ulint		n_fields = key->user_defined_key_parts;

	DBUG_ENTER("innobase_create_index_def");
	DBUG_ASSERT(!key_clustered || new_clustered);

	index->fields = static_cast<index_field_t*>(
		mem_heap_alloc(heap, n_fields * sizeof *index->fields));

	index->parser = NULL;
	index->key_number = key_number;
	index->n_fields = n_fields;
	index->name = mem_heap_strdup(heap, key->name.str);
	index->rebuild = new_clustered;

	if (key_clustered) {
		DBUG_ASSERT(!(key->flags & (HA_FULLTEXT | HA_SPATIAL)));
		DBUG_ASSERT(key->flags & HA_NOSAME);
		index->ind_type = DICT_CLUSTERED | DICT_UNIQUE;
	} else if (key->flags & HA_FULLTEXT) {
		DBUG_ASSERT(!(key->flags & (HA_SPATIAL | HA_NOSAME)));
		DBUG_ASSERT(!(key->flags & HA_KEYFLAG_MASK
			      & ~(HA_FULLTEXT
				  | HA_PACK_KEY
				  | HA_BINARY_PACK_KEY)));
		index->ind_type = DICT_FTS;

		/* Note: key->parser is only parser name,
			 we need to get parser from altered_table instead */

		if (key->flags & HA_USES_PARSER) {
			for (ulint j = 0; j < altered_table->s->keys; j++) {
				if (!strcmp(altered_table->key_info[j].name.str,
					    key->name.str)) {
					ut_ad(altered_table->key_info[j].flags
					      & HA_USES_PARSER);

					plugin_ref	parser =
						altered_table->key_info[j].parser;
					index->parser =
						static_cast<st_mysql_ftparser*>(
						plugin_decl(parser)->info);

					break;
				}
			}

			DBUG_EXECUTE_IF("fts_instrument_use_default_parser",
				index->parser = &fts_default_parser;);
			ut_ad(index->parser);
		}
	} else if (key->flags & HA_SPATIAL) {
		DBUG_ASSERT(!(key->flags & HA_NOSAME));
		index->ind_type = DICT_SPATIAL;
		ut_ad(n_fields == 1);
		ulint	num_v = 0;

		/* Need to count the virtual fields before this spatial
		indexed field */
		for (ulint i = 0; i < key->key_part->fieldnr; i++) {
			num_v += !altered_table->field[i]->stored_in_db();
		}
		index->fields[0].col_no = key->key_part[0].fieldnr - num_v;
		index->fields[0].prefix_len = 0;
		index->fields[0].is_v_col = false;
		index->fields[0].descending = false;

		/* Currently, the spatial index cannot be created
		on virtual columns. It is blocked in the SQL layer. */
		DBUG_ASSERT(key->key_part[0].field->stored_in_db());
	} else {
		index->ind_type = (key->flags & HA_NOSAME) ? DICT_UNIQUE : 0;
	}

	if (!(key->flags & HA_SPATIAL)) {
		for (i = 0; i < n_fields; i++) {
			innobase_create_index_field_def(
				new_clustered, altered_table,
				&key->key_part[i], &index->fields[i]);

			if (index->fields[i].is_v_col) {
				index->ind_type |= DICT_VIRTUAL;
			}
		}
	}

	DBUG_VOID_RETURN;
}

/*******************************************************************//**
Check whether the table has a unique index with FTS_DOC_ID_INDEX_NAME
on the Doc ID column.
@return the status of the FTS_DOC_ID index */
enum fts_doc_id_index_enum
innobase_fts_check_doc_id_index(
/*============================*/
	const dict_table_t*	table,		/*!< in: table definition */
	const TABLE*		altered_table,	/*!< in: MySQL table
						that is being altered */
	ulint*			fts_doc_col_no)	/*!< out: The column number for
						Doc ID, or ULINT_UNDEFINED
						if it is being created in
						ha_alter_info */
{
	const dict_index_t*	index;
	const dict_field_t*	field;

	if (altered_table) {
		/* Check if a unique index with the name of
		FTS_DOC_ID_INDEX_NAME is being created. */

		for (uint i = 0; i < altered_table->s->keys; i++) {
			const KEY& key = altered_table->key_info[i];

			if (innobase_strcasecmp(
				    key.name.str, FTS_DOC_ID_INDEX_NAME)) {
				continue;
			}

			if ((key.flags & HA_NOSAME)
			    && key.user_defined_key_parts == 1
			    && !(key.key_part[0].key_part_flag
				 & HA_REVERSE_SORT)
			    && !strcmp(key.name.str, FTS_DOC_ID_INDEX_NAME)
			    && !strcmp(key.key_part[0].field->field_name.str,
				       FTS_DOC_ID_COL_NAME)) {
				if (fts_doc_col_no) {
					*fts_doc_col_no = ULINT_UNDEFINED;
				}
				return(FTS_EXIST_DOC_ID_INDEX);
			} else {
				return(FTS_INCORRECT_DOC_ID_INDEX);
			}
		}
	}

	if (!table) {
		return(FTS_NOT_EXIST_DOC_ID_INDEX);
	}

	for (index = dict_table_get_first_index(table);
	     index; index = dict_table_get_next_index(index)) {


		/* Check if there exists a unique index with the name of
		FTS_DOC_ID_INDEX_NAME and ignore the corrupted index */
		if (index->type & DICT_CORRUPT
		    || innobase_strcasecmp(index->name, FTS_DOC_ID_INDEX_NAME)) {
			continue;
		}

		if (!dict_index_is_unique(index)
		    || dict_index_get_n_unique(index) != 1
		    || strcmp(index->name, FTS_DOC_ID_INDEX_NAME)) {
			return(FTS_INCORRECT_DOC_ID_INDEX);
		}

		/* Check whether the index has FTS_DOC_ID as its
		first column */
		field = dict_index_get_nth_field(index, 0);

		/* The column would be of a BIGINT data type */
		if (strcmp(field->name, FTS_DOC_ID_COL_NAME) == 0
		    && !field->descending
		    && field->col->mtype == DATA_INT
		    && field->col->len == 8
		    && field->col->prtype & DATA_NOT_NULL
		    && !field->col->is_virtual()) {
			if (fts_doc_col_no) {
				*fts_doc_col_no = dict_col_get_no(field->col);
			}
			return(FTS_EXIST_DOC_ID_INDEX);
		} else {
			return(FTS_INCORRECT_DOC_ID_INDEX);
		}
	}


	/* Not found */
	return(FTS_NOT_EXIST_DOC_ID_INDEX);
}
/*******************************************************************//**
Check whether the table has a unique index with FTS_DOC_ID_INDEX_NAME
on the Doc ID column in MySQL create index definition.
@return FTS_EXIST_DOC_ID_INDEX if there exists the FTS_DOC_ID index,
FTS_INCORRECT_DOC_ID_INDEX if the FTS_DOC_ID index is of wrong format */
enum fts_doc_id_index_enum
innobase_fts_check_doc_id_index_in_def(
/*===================================*/
	ulint		n_key,		/*!< in: Number of keys */
	const KEY*	key_info)	/*!< in: Key definition */
{
	/* Check whether there is a "FTS_DOC_ID_INDEX" in the to be built index
	list */
	for (ulint j = 0; j < n_key; j++) {
		const KEY*	key = &key_info[j];

		if (innobase_strcasecmp(key->name.str, FTS_DOC_ID_INDEX_NAME)) {
			continue;
		}

		/* Do a check on FTS DOC ID_INDEX, it must be unique,
		named as "FTS_DOC_ID_INDEX" and on column "FTS_DOC_ID" */
		if (!(key->flags & HA_NOSAME)
		    || key->user_defined_key_parts != 1
		    || (key->key_part[0].key_part_flag & HA_REVERSE_SORT)
		    || strcmp(key->name.str, FTS_DOC_ID_INDEX_NAME)
		    || strcmp(key->key_part[0].field->field_name.str,
			      FTS_DOC_ID_COL_NAME)) {
			return(FTS_INCORRECT_DOC_ID_INDEX);
		}

		return(FTS_EXIST_DOC_ID_INDEX);
	}

	return(FTS_NOT_EXIST_DOC_ID_INDEX);
}

/** Create an index table where indexes are ordered as follows:

IF a new primary key is defined for the table THEN

	1) New primary key
	2) The remaining keys in key_info

ELSE

	1) All new indexes in the order they arrive from MySQL

ENDIF

@return key definitions */
MY_ATTRIBUTE((nonnull, warn_unused_result, malloc))
inline index_def_t*
ha_innobase_inplace_ctx::create_key_defs(
	const Alter_inplace_info*	ha_alter_info,
			/*!< in: alter operation */
	const TABLE*			altered_table,
			/*!< in: MySQL table that is being altered */
	ulint&				n_fts_add,
			/*!< out: number of FTS indexes to be created */
	ulint&				fts_doc_id_col,
			/*!< in: The column number for Doc ID */
	bool&				add_fts_doc_id,
			/*!< in: whether we need to add new DOC ID
			column for FTS index */
	bool&				add_fts_doc_idx,
			/*!< in: whether we need to add new DOC ID
			index for FTS index */
	const TABLE*			table)
			/*!< in: MySQL table that is being altered */
{
	ulint&			n_add = num_to_add_index;
	const bool got_default_clust = new_table->indexes.start->is_gen_clust();

	index_def_t*		indexdef;
	index_def_t*		indexdefs;
	bool			new_primary;
	const uint*const	add
		= ha_alter_info->index_add_buffer;
	const KEY*const		key_info
		= ha_alter_info->key_info_buffer;

	DBUG_ENTER("ha_innobase_inplace_ctx::create_key_defs");
	DBUG_ASSERT(!add_fts_doc_id || add_fts_doc_idx);
	DBUG_ASSERT(ha_alter_info->index_add_count == n_add);

	/* If there is a primary key, it is always the first index
	defined for the innodb_table. */

	new_primary = n_add > 0
		&& !my_strcasecmp(system_charset_info,
				  key_info[*add].name.str, "PRIMARY");
	n_fts_add = 0;

	/* If there is a UNIQUE INDEX consisting entirely of NOT NULL
	columns and if the index does not contain column prefix(es)
	(only prefix/part of the column is indexed), MySQL will treat the
	index as a PRIMARY KEY unless the table already has one. */

	ut_ad(altered_table->s->primary_key == 0
	      || altered_table->s->primary_key == MAX_KEY);

	if (got_default_clust && !new_primary) {
		new_primary = (altered_table->s->primary_key != MAX_KEY);
	}

	const bool rebuild = new_primary || add_fts_doc_id
		|| innobase_need_rebuild(ha_alter_info, table);

	/* Reserve one more space if new_primary is true, and we might
	need to add the FTS_DOC_ID_INDEX */
	indexdef = indexdefs = static_cast<index_def_t*>(
		mem_heap_alloc(
			heap, sizeof *indexdef
			* (ha_alter_info->key_count
			   + rebuild
			   + got_default_clust)));

	if (rebuild) {
		ulint	primary_key_number;

		if (new_primary) {
			DBUG_ASSERT(n_add || got_default_clust);
			DBUG_ASSERT(n_add || !altered_table->s->primary_key);
			primary_key_number = altered_table->s->primary_key;
		} else if (got_default_clust) {
			/* Create the GEN_CLUST_INDEX */
			index_def_t*	index = indexdef++;

			index->fields = NULL;
			index->n_fields = 0;
			index->ind_type = DICT_CLUSTERED;
			index->name = innobase_index_reserve_name;
			index->rebuild = true;
			index->key_number = ~0U;
			primary_key_number = ULINT_UNDEFINED;
			goto created_clustered;
		} else {
			primary_key_number = 0;
		}

		/* Create the PRIMARY key index definition */
		innobase_create_index_def(
			altered_table, key_info, primary_key_number,
			true, true, indexdef++, heap);

created_clustered:
		n_add = 1;

		for (ulint i = 0; i < ha_alter_info->key_count; i++) {
			if (i == primary_key_number) {
				continue;
			}
			/* Copy the index definitions. */
			innobase_create_index_def(
				altered_table, key_info, i, true,
				false, indexdef, heap);

			if (indexdef->ind_type & DICT_FTS) {
				n_fts_add++;
			}

			indexdef++;
			n_add++;
		}

		if (n_fts_add > 0) {
			ulint	num_v = 0;

			if (!add_fts_doc_id
			    && !innobase_fts_check_doc_id_col(
				    NULL, altered_table,
				    &fts_doc_id_col, &num_v)) {
				fts_doc_id_col = altered_table->s->fields - num_v;
				add_fts_doc_id = true;
			}

			if (!add_fts_doc_idx) {
				fts_doc_id_index_enum	ret;
				ulint			doc_col_no;

				ret = innobase_fts_check_doc_id_index(
					NULL, altered_table, &doc_col_no);

				/* This should have been checked before */
				ut_ad(ret != FTS_INCORRECT_DOC_ID_INDEX);

				if (ret == FTS_NOT_EXIST_DOC_ID_INDEX) {
					add_fts_doc_idx = true;
				} else {
					ut_ad(ret == FTS_EXIST_DOC_ID_INDEX);
					ut_ad(doc_col_no == ULINT_UNDEFINED
					      || doc_col_no == fts_doc_id_col);
				}
			}
		}
	} else {
		/* Create definitions for added secondary indexes. */

		for (ulint i = 0; i < n_add; i++) {
			innobase_create_index_def(
				altered_table, key_info, add[i],
				false, false, indexdef, heap);

			if (indexdef->ind_type & DICT_FTS) {
				n_fts_add++;
			}

			indexdef++;
		}
	}

	DBUG_ASSERT(indexdefs + n_add == indexdef);

	if (add_fts_doc_idx) {
		index_def_t*	index = indexdef++;

		index->fields = static_cast<index_field_t*>(
			mem_heap_alloc(heap, sizeof *index->fields));
		index->n_fields = 1;
		index->fields->col_no = fts_doc_id_col;
		index->fields->prefix_len = 0;
		index->fields->descending = false;
		index->fields->is_v_col = false;
		index->ind_type = DICT_UNIQUE;
		ut_ad(!rebuild
		      || !add_fts_doc_id
		      || fts_doc_id_col <= altered_table->s->fields);

		index->name = FTS_DOC_ID_INDEX_NAME;
		index->rebuild = rebuild;

		/* TODO: assign a real MySQL key number for this */
		index->key_number = ULINT_UNDEFINED;
		n_add++;
	}

	DBUG_ASSERT(indexdef > indexdefs);
	DBUG_ASSERT((ulint) (indexdef - indexdefs)
		    <= ha_alter_info->key_count
		    + add_fts_doc_idx + got_default_clust);
	DBUG_ASSERT(ha_alter_info->index_add_count <= n_add);
	DBUG_RETURN(indexdefs);
}

MY_ATTRIBUTE((warn_unused_result))
bool too_big_key_part_length(size_t max_field_len, const KEY& key)
{
	for (ulint i = 0; i < key.user_defined_key_parts; i++) {
		if (key.key_part[i].length > max_field_len) {
			return true;
		}
	}
	return false;
}

/********************************************************************//**
Drop any indexes that we were not able to free previously due to
open table handles. */
static
void
online_retry_drop_indexes_low(
/*==========================*/
	dict_table_t*	table,	/*!< in/out: table */
	trx_t*		trx)	/*!< in/out: transaction */
{
	ut_ad(dict_sys.locked());
	ut_ad(trx->dict_operation_lock_mode);
	ut_ad(trx->dict_operation);

	/* We can have table->n_ref_count > 1, because other threads
	may have prebuilt->table pointing to the table. However, these
	other threads should be between statements, waiting for the
	next statement to execute, or for a meta-data lock. */
	ut_ad(table->get_ref_count() >= 1);

	if (table->drop_aborted) {
		row_merge_drop_indexes(trx, table, true);
	}
}

/** After commit, unlock the data dictionary and close any deleted files.
@param deleted  handles of deleted files
@param trx      committed transaction */
static void unlock_and_close_files(const std::vector<pfs_os_file_t> &deleted,
                                   trx_t *trx)
{
  row_mysql_unlock_data_dictionary(trx);
  for (pfs_os_file_t d : deleted)
    os_file_close(d);
  log_write_up_to(trx->commit_lsn, true);
}

/** Commit a DDL transaction and unlink any deleted files. */
static void commit_unlock_and_unlink(trx_t *trx)
{
  std::vector<pfs_os_file_t> deleted;
  trx->commit(deleted);
  unlock_and_close_files(deleted, trx);
}

/**
Drop any indexes that we were not able to free previously due to
open table handles.
@param table     InnoDB table
@param thd       connection handle
*/
static void online_retry_drop_indexes(dict_table_t *table, THD *thd)
{
  if (table->drop_aborted)
  {
    trx_t *trx= innobase_trx_allocate(thd);

    trx_start_for_ddl(trx);
    if (lock_sys_tables(trx) == DB_SUCCESS)
    {
      row_mysql_lock_data_dictionary(trx);
      online_retry_drop_indexes_low(table, trx);
      commit_unlock_and_unlink(trx);
    }
    else
      trx->commit();
    trx->free();
  }

  ut_d(dict_sys.freeze(SRW_LOCK_CALL));
  ut_d(dict_table_check_for_dup_indexes(table, CHECK_ALL_COMPLETE));
  ut_d(dict_sys.unfreeze());
  ut_ad(!table->drop_aborted);
}

/** Determines if InnoDB is dropping a foreign key constraint.
@param foreign the constraint
@param drop_fk constraints being dropped
@param n_drop_fk number of constraints that are being dropped
@return whether the constraint is being dropped */
MY_ATTRIBUTE((pure, nonnull(1), warn_unused_result))
inline
bool
innobase_dropping_foreign(
	const dict_foreign_t*	foreign,
	dict_foreign_t**	drop_fk,
	ulint			n_drop_fk)
{
	while (n_drop_fk--) {
		if (*drop_fk++ == foreign) {
			return(true);
		}
	}

	return(false);
}

/** Determines if an InnoDB FOREIGN KEY constraint depends on a
column that is being dropped or modified to NOT NULL.
@param user_table InnoDB table as it is before the ALTER operation
@param col_name Name of the column being altered
@param drop_fk constraints being dropped
@param n_drop_fk number of constraints that are being dropped
@param drop true=drop column, false=set NOT NULL
@retval true Not allowed (will call my_error())
@retval false Allowed
*/
MY_ATTRIBUTE((pure, nonnull(1,4), warn_unused_result))
static
bool
innobase_check_foreigns_low(
	const dict_table_t*	user_table,
	dict_foreign_t**	drop_fk,
	ulint			n_drop_fk,
	const char*		col_name,
	bool			drop)
{
	dict_foreign_t*	foreign;
	ut_ad(dict_sys.locked());

	/* Check if any FOREIGN KEY constraints are defined on this
	column. */

	for (dict_foreign_set::const_iterator it = user_table->foreign_set.begin();
	     it != user_table->foreign_set.end();
	     ++it) {

		foreign = *it;

		if (!drop && !(foreign->type
			       & (DICT_FOREIGN_ON_DELETE_SET_NULL
				  | DICT_FOREIGN_ON_UPDATE_SET_NULL))) {
			continue;
		}

		if (innobase_dropping_foreign(foreign, drop_fk, n_drop_fk)) {
			continue;
		}

		for (unsigned f = 0; f < foreign->n_fields; f++) {
			if (!strcmp(foreign->foreign_col_names[f],
				    col_name)) {
				my_error(drop
					 ? ER_FK_COLUMN_CANNOT_DROP
					 : ER_FK_COLUMN_NOT_NULL, MYF(0),
					 col_name, foreign->id);
				return(true);
			}
		}
	}

	if (!drop) {
		/* SET NULL clauses on foreign key constraints of
		child tables affect the child tables, not the parent table.
		The column can be NOT NULL in the parent table. */
		return(false);
	}

	/* Check if any FOREIGN KEY constraints in other tables are
	referring to the column that is being dropped. */
	for (dict_foreign_set::const_iterator it
		= user_table->referenced_set.begin();
	     it != user_table->referenced_set.end();
	     ++it) {

		foreign = *it;

		if (innobase_dropping_foreign(foreign, drop_fk, n_drop_fk)) {
			continue;
		}

		for (unsigned f = 0; f < foreign->n_fields; f++) {
			char display_name[FN_REFLEN];

			if (strcmp(foreign->referenced_col_names[f],
				   col_name)) {
				continue;
			}

			char* buf_end = innobase_convert_name(
				display_name, (sizeof display_name) - 1,
				foreign->foreign_table_name,
				strlen(foreign->foreign_table_name),
				NULL);
			*buf_end = '\0';
			my_error(ER_FK_COLUMN_CANNOT_DROP_CHILD,
				 MYF(0), col_name, foreign->id,
				 display_name);

			return(true);
		}
	}

	return(false);
}

/** Determines if an InnoDB FOREIGN KEY constraint depends on a
column that is being dropped or modified to NOT NULL.
@param ha_alter_info Data used during in-place alter
@param altered_table MySQL table that is being altered
@param old_table MySQL table as it is before the ALTER operation
@param user_table InnoDB table as it is before the ALTER operation
@param drop_fk constraints being dropped
@param n_drop_fk number of constraints that are being dropped
@retval true Not allowed (will call my_error())
@retval false Allowed
*/
MY_ATTRIBUTE((pure, nonnull(1,2,3), warn_unused_result))
static
bool
innobase_check_foreigns(
	Alter_inplace_info*	ha_alter_info,
	const TABLE*		old_table,
	const dict_table_t*	user_table,
	dict_foreign_t**	drop_fk,
	ulint			n_drop_fk)
{
	for (Field** fp = old_table->field; *fp; fp++) {
		ut_ad(!(*fp)->real_maybe_null()
		      == !!((*fp)->flags & NOT_NULL_FLAG));

		auto end = ha_alter_info->alter_info->create_list.end();
		auto it = std::find_if(
			ha_alter_info->alter_info->create_list.begin(), end,
			[fp](const Create_field& field) {
				return field.field == *fp;
			});

		if (it == end || (it->flags & NOT_NULL_FLAG)) {
			if (innobase_check_foreigns_low(
				    user_table, drop_fk, n_drop_fk,
				    (*fp)->field_name.str, it == end)) {
				return(true);
			}
		}
	}

	return(false);
}

/** Convert a default value for ADD COLUMN.
@param[in,out]	heap		Memory heap where allocated
@param[out]	dfield		InnoDB data field to copy to
@param[in]	field		MySQL value for the column
@param[in]	old_field	Old column if altering; NULL for ADD COLUMN
@param[in]	comp		nonzero if in compact format. */
static void innobase_build_col_map_add(
	mem_heap_t*	heap,
	dfield_t*	dfield,
	const Field*	field,
	const Field*	old_field,
	ulint		comp)
{
	if (old_field && old_field->real_maybe_null()
	    && field->real_maybe_null()) {
		return;
	}

	if (field->is_real_null()) {
		dfield_set_null(dfield);
		return;
	}

	const Field& from = old_field ? *old_field : *field;
	ulint	size	= from.pack_length();

	byte*	buf	= static_cast<byte*>(mem_heap_alloc(heap, size));

	row_mysql_store_col_in_innobase_format(
		dfield, buf, true, from.ptr, size, comp);
}

/** Construct the translation table for reordering, dropping or
adding columns.

@param ha_alter_info Data used during in-place alter
@param altered_table MySQL table that is being altered
@param table MySQL table as it is before the ALTER operation
@param new_table InnoDB table corresponding to MySQL altered_table
@param old_table InnoDB table corresponding to MYSQL table
@param defaults Default values for ADD COLUMN, or NULL if no ADD COLUMN
@param heap Memory heap where allocated
@return array of integers, mapping column numbers in the table
to column numbers in altered_table */
static MY_ATTRIBUTE((nonnull(1,2,3,4,5,7), warn_unused_result))
const ulint*
innobase_build_col_map(
/*===================*/
	Alter_inplace_info*	ha_alter_info,
	const TABLE*		altered_table,
	const TABLE*		table,
	dict_table_t*		new_table,
	const dict_table_t*	old_table,
	dtuple_t*		defaults,
	mem_heap_t*		heap)
{
	DBUG_ENTER("innobase_build_col_map");
	DBUG_ASSERT(altered_table != table);
	DBUG_ASSERT(new_table != old_table);
	DBUG_ASSERT(dict_table_get_n_cols(new_table)
		    + dict_table_get_n_v_cols(new_table)
		    >= altered_table->s->fields + DATA_N_SYS_COLS);
	DBUG_ASSERT(dict_table_get_n_cols(old_table)
		    + dict_table_get_n_v_cols(old_table)
		    >= table->s->fields + DATA_N_SYS_COLS
		    || ha_innobase::omits_virtual_cols(*table->s));
	DBUG_ASSERT(!!defaults == !!(ha_alter_info->handler_flags
				     & INNOBASE_DEFAULTS));
	DBUG_ASSERT(!defaults || dtuple_get_n_fields(defaults)
		    == dict_table_get_n_cols(new_table));

	const uint old_n_v_cols = uint(table->s->fields
				       - table->s->stored_fields);
	DBUG_ASSERT(old_n_v_cols == old_table->n_v_cols
		    || table->s->frm_version < FRM_VER_EXPRESSSIONS);
	DBUG_ASSERT(!old_n_v_cols || table->s->virtual_fields);

	ulint*	col_map = static_cast<ulint*>(
		mem_heap_alloc(
			heap, (size_t(old_table->n_cols) + old_n_v_cols)
			* sizeof *col_map));

	uint	i = 0;
	uint	num_v = 0;

	/* Any dropped columns will map to ULINT_UNDEFINED. */
	for (uint old_i = 0; old_i + DATA_N_SYS_COLS < old_table->n_cols;
	     old_i++) {
		col_map[old_i] = ULINT_UNDEFINED;
	}

	for (uint old_i = 0; old_i < old_n_v_cols; old_i++) {
		col_map[old_i + old_table->n_cols] = ULINT_UNDEFINED;
	}

	const bool omits_virtual = ha_innobase::omits_virtual_cols(*table->s);

	for (const Create_field& new_field :
	     ha_alter_info->alter_info->create_list) {
		bool	is_v = !new_field.stored_in_db();
		ulint	num_old_v = 0;

		for (uint old_i = 0; table->field[old_i]; old_i++) {
			const Field* field = table->field[old_i];
			if (!field->stored_in_db()) {
				if (is_v && new_field.field == field) {
					if (!omits_virtual) {
						col_map[old_table->n_cols
							+ num_v]
							= num_old_v;
					}
					num_old_v++;
					goto found_col;
				}
				num_old_v++;
				continue;
			}

			if (new_field.field == field) {

				const Field* altered_field =
					altered_table->field[i + num_v];

				if (defaults) {
					innobase_build_col_map_add(
						heap,
						dtuple_get_nth_field(
							defaults, i),
						altered_field,
						field,
						dict_table_is_comp(
							new_table));
				}

				col_map[old_i - num_old_v] = i;
				if (!old_table->versioned()
				    || !altered_table->versioned()) {
				} else if (old_i == old_table->vers_start) {
					new_table->vers_start = (i + num_v)
						& dict_index_t::MAX_N_FIELDS;
				} else if (old_i == old_table->vers_end) {
					new_table->vers_end = (i + num_v)
						& dict_index_t::MAX_N_FIELDS;
				}
				goto found_col;
			}
		}

		if (!is_v) {
			innobase_build_col_map_add(
				heap, dtuple_get_nth_field(defaults, i),
				altered_table->field[i + num_v],
				NULL,
				dict_table_is_comp(new_table));
		}
found_col:
		if (is_v) {
			num_v++;
		} else {
			i++;
		}
	}

	DBUG_ASSERT(i == altered_table->s->fields - num_v);

	i = table->s->fields - old_n_v_cols;

	/* Add the InnoDB hidden FTS_DOC_ID column, if any. */
	if (i + DATA_N_SYS_COLS < old_table->n_cols) {
		/* There should be exactly one extra field,
		the FTS_DOC_ID. */
		DBUG_ASSERT(DICT_TF2_FLAG_IS_SET(old_table,
						 DICT_TF2_FTS_HAS_DOC_ID));
		DBUG_ASSERT(i + DATA_N_SYS_COLS + 1 == old_table->n_cols);
		DBUG_ASSERT(!strcmp(dict_table_get_col_name(
					    old_table, i),
				    FTS_DOC_ID_COL_NAME));
		if (altered_table->s->fields + DATA_N_SYS_COLS
		    - new_table->n_v_cols
		    < new_table->n_cols) {
			DBUG_ASSERT(DICT_TF2_FLAG_IS_SET(
					    new_table,
					    DICT_TF2_FTS_HAS_DOC_ID));
			DBUG_ASSERT(altered_table->s->fields
				    + DATA_N_SYS_COLS + 1
				    == static_cast<ulint>(
					new_table->n_cols
					+ new_table->n_v_cols));
			col_map[i] = altered_table->s->fields
				     - new_table->n_v_cols;
		} else {
			DBUG_ASSERT(!DICT_TF2_FLAG_IS_SET(
					    new_table,
					    DICT_TF2_FTS_HAS_DOC_ID));
			col_map[i] = ULINT_UNDEFINED;
		}

		i++;
	} else {
		DBUG_ASSERT(!DICT_TF2_FLAG_IS_SET(
				    old_table,
				    DICT_TF2_FTS_HAS_DOC_ID));
	}

	for (; i < old_table->n_cols; i++) {
		col_map[i] = i + new_table->n_cols - old_table->n_cols;
	}

	DBUG_RETURN(col_map);
}

/** Get the new non-virtual column names if any columns were renamed
@param ha_alter_info	Data used during in-place alter
@param altered_table	MySQL table that is being altered
@param table		MySQL table as it is before the ALTER operation
@param user_table	InnoDB table as it is before the ALTER operation
@param heap		Memory heap for the allocation
@return array of new column names in rebuilt_table, or NULL if not renamed */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
const char**
innobase_get_col_names(
	Alter_inplace_info*	ha_alter_info,
	const TABLE*		altered_table,
	const TABLE*		table,
	const dict_table_t*	user_table,
	mem_heap_t*		heap)
{
	const char**		cols;
	uint			i;

	DBUG_ENTER("innobase_get_col_names");
	DBUG_ASSERT(user_table->n_t_def > table->s->fields);
	DBUG_ASSERT(ha_alter_info->handler_flags
		    & ALTER_COLUMN_NAME);

	cols = static_cast<const char**>(
		mem_heap_zalloc(heap, user_table->n_def * sizeof *cols));

	i = 0;
	for (const Create_field& new_field :
	     ha_alter_info->alter_info->create_list) {
		ulint	num_v = 0;
		DBUG_ASSERT(i < altered_table->s->fields);

		if (!new_field.stored_in_db()) {
			continue;
		}

		for (uint old_i = 0; table->field[old_i]; old_i++) {
			num_v += !table->field[old_i]->stored_in_db();

			if (new_field.field == table->field[old_i]) {
				cols[old_i - num_v] = new_field.field_name.str;
				break;
			}
		}

		i++;
	}

	/* Copy the internal column names. */
	i = table->s->fields - user_table->n_v_def;
	cols[i] = dict_table_get_col_name(user_table, i);

	while (++i < user_table->n_def) {
		cols[i] = cols[i - 1] + strlen(cols[i - 1]) + 1;
	}

	DBUG_RETURN(cols);
}

/** Check whether the column prefix is increased, decreased, or unchanged.
@param[in]	new_prefix_len	new prefix length
@param[in]	old_prefix_len	new prefix length
@retval	1	prefix is increased
@retval	0	prefix is unchanged
@retval	-1	prefix is decreased */
static inline
lint
innobase_pk_col_prefix_compare(
	ulint	new_prefix_len,
	ulint	old_prefix_len)
{
	ut_ad(new_prefix_len < COMPRESSED_REC_MAX_DATA_SIZE);
	ut_ad(old_prefix_len < COMPRESSED_REC_MAX_DATA_SIZE);

	if (new_prefix_len == old_prefix_len) {
		return(0);
	}

	if (new_prefix_len == 0) {
		new_prefix_len = ULINT_MAX;
	}

	if (old_prefix_len == 0) {
		old_prefix_len = ULINT_MAX;
	}

	if (new_prefix_len > old_prefix_len) {
		return(1);
	} else {
		return(-1);
	}
}

/** Check whether the column is existing in old table.
@param[in]	new_col_no	new column no
@param[in]	col_map		mapping of old column numbers to new ones
@param[in]	col_map_size	the column map size
@return true if the column is existing, otherwise false. */
static inline
bool
innobase_pk_col_is_existing(
	const ulint	new_col_no,
	const ulint*	col_map,
	const ulint	col_map_size)
{
	for (ulint i = 0; i < col_map_size; i++) {
		if (col_map[i] == new_col_no) {
			return(true);
		}
	}

	return(false);
}

/** Determine whether both the indexes have same set of primary key
fields arranged in the same order.

Rules when we cannot skip sorting:
(1) Removing existing PK columns somewhere else than at the end of the PK;
(2) Adding existing columns to the PK, except at the end of the PK when no
columns are removed from the PK;
(3) Changing the order of existing PK columns;
(4) Decreasing the prefix length just like removing existing PK columns
follows rule(1), Increasing the prefix length just like adding existing
PK columns follows rule(2);
(5) Changing the ASC/DESC attribute of the existing PK columns.
@param[in]	col_map		mapping of old column numbers to new ones
@param[in]	ha_alter_info	Data used during in-place alter
@param[in]	old_clust_index	index to be compared
@param[in]	new_clust_index index to be compared
@retval true if both indexes have same order.
@retval false. */
static MY_ATTRIBUTE((warn_unused_result))
bool
innobase_pk_order_preserved(
	const ulint*		col_map,
	const dict_index_t*	old_clust_index,
	const dict_index_t*	new_clust_index)
{
	ulint	old_n_uniq
		= dict_index_get_n_ordering_defined_by_user(
			old_clust_index);
	ulint	new_n_uniq
		= dict_index_get_n_ordering_defined_by_user(
			new_clust_index);

	ut_ad(dict_index_is_clust(old_clust_index));
	ut_ad(dict_index_is_clust(new_clust_index));
	ut_ad(old_clust_index->table != new_clust_index->table);
	ut_ad(col_map != NULL);

	if (old_n_uniq == 0) {
		/* There was no PRIMARY KEY in the table.
		If there is no PRIMARY KEY after the ALTER either,
		no sorting is needed. */
		return(new_n_uniq == old_n_uniq);
	}

	/* DROP PRIMARY KEY is only allowed in combination with
	ADD PRIMARY KEY. */
	ut_ad(new_n_uniq > 0);

	/* The order of the last processed new_clust_index key field,
	not counting ADD COLUMN, which are constant. */
	lint	last_field_order = -1;
	ulint	existing_field_count = 0;
	ulint	old_n_cols = dict_table_get_n_cols(old_clust_index->table);
	for (ulint new_field = 0; new_field < new_n_uniq; new_field++) {
		ulint	new_col_no =
			new_clust_index->fields[new_field].col->ind;

		/* Check if there is a match in old primary key. */
		ulint	old_field = 0;
		while (old_field < old_n_uniq) {
			ulint	old_col_no =
				old_clust_index->fields[old_field].col->ind;

			if (col_map[old_col_no] == new_col_no) {
				break;
			}

			old_field++;
		}

		/* The order of key field in the new primary key.
		1. old PK column:      idx in old primary key
		2. existing column:    old_n_uniq + sequence no
		3. newly added column: no order */
		lint		new_field_order;
		const bool	old_pk_column = old_field < old_n_uniq;

		if (old_pk_column) {
			new_field_order = lint(old_field);
		} else if (innobase_pk_col_is_existing(new_col_no, col_map,
						       old_n_cols)
			   || new_clust_index->table->persistent_autoinc
			   == new_field + 1) {
			/* Adding an existing column or an AUTO_INCREMENT
			column may change the existing ordering. */
			new_field_order = lint(old_n_uniq
					       + existing_field_count++);
		} else {
			/* Skip newly added column. */
			continue;
		}

		if (last_field_order + 1 != new_field_order) {
			/* Old PK order is not kept, or existing column
			is not added at the end of old PK. */
			return(false);
		}

		last_field_order = new_field_order;

		if (!old_pk_column) {
			continue;
		}

		const dict_field_t &of = old_clust_index->fields[old_field];
		const dict_field_t &nf = new_clust_index->fields[new_field];

		if (of.descending != nf.descending) {
			return false;
		}

		/* Check prefix length change. */
		const lint	prefix_change = innobase_pk_col_prefix_compare(
			nf.prefix_len, of.prefix_len);

		if (prefix_change < 0) {
			/* If a column's prefix length is decreased, it should
			be the last old PK column in new PK.
			Note: we set last_field_order to -2, so that if	there
			are any old PK colmns or existing columns after it in
			new PK, the comparison to new_field_order will fail in
			the next round.*/
			last_field_order = -2;
		} else if (prefix_change > 0) {
			/* If a column's prefix length is increased, it	should
			be the last PK column in old PK. */
			if (old_field != old_n_uniq - 1) {
				return(false);
			}
		}
	}

	return(true);
}

/** Update the mtype from DATA_BLOB to DATA_GEOMETRY for a specified
GIS column of a table. This is used when we want to create spatial index
on legacy GIS columns coming from 5.6, where we store GIS data as DATA_BLOB
in innodb layer.
@param[in]	table_id	table id
@param[in]	col_name	column name
@param[in]	trx		data dictionary transaction
@retval true Failure
@retval false Success */
static
bool
innobase_update_gis_column_type(
	table_id_t	table_id,
	const char*	col_name,
	trx_t*		trx)
{
	pars_info_t*	info;
	dberr_t		error;

	DBUG_ENTER("innobase_update_gis_column_type");

	DBUG_ASSERT(trx->dict_operation);
	ut_ad(trx->dict_operation_lock_mode);
	ut_ad(dict_sys.locked());

	info = pars_info_create();

	pars_info_add_ull_literal(info, "tableid", table_id);
	pars_info_add_str_literal(info, "name", col_name);
	pars_info_add_int4_literal(info, "mtype", DATA_GEOMETRY);

	trx->op_info = "update column type to DATA_GEOMETRY";

	error = que_eval_sql(
		info,
		"PROCEDURE UPDATE_SYS_COLUMNS_PROC () IS\n"
		"BEGIN\n"
		"UPDATE SYS_COLUMNS SET MTYPE=:mtype\n"
		"WHERE TABLE_ID=:tableid AND NAME=:name;\n"
		"END;\n", trx);

	trx->error_state = DB_SUCCESS;
	trx->op_info = "";

	DBUG_RETURN(error != DB_SUCCESS);
}

/** Check if we are creating spatial indexes on GIS columns, which are
legacy columns from earlier MySQL, such as 5.6. If so, we have to update
the mtypes of the old GIS columns to DATA_GEOMETRY.
In 5.6, we store GIS columns as DATA_BLOB in InnoDB layer, it will introduce
confusion when we run latest server on older data. That's why we need to
do the upgrade.
@param[in] ha_alter_info	Data used during in-place alter
@param[in] table		Table on which we want to add indexes
@param[in] trx			Transaction
@return DB_SUCCESS if update successfully or no columns need to be updated,
otherwise DB_ERROR, which means we can't update the mtype for some
column, and creating spatial index on it should be dangerous */
static
dberr_t
innobase_check_gis_columns(
	Alter_inplace_info*	ha_alter_info,
	dict_table_t*		table,
	trx_t*			trx)
{
	DBUG_ENTER("innobase_check_gis_columns");

	for (uint key_num = 0;
	     key_num < ha_alter_info->index_add_count;
	     key_num++) {

		const KEY&	key = ha_alter_info->key_info_buffer[
			ha_alter_info->index_add_buffer[key_num]];

		if (!(key.flags & HA_SPATIAL)) {
			continue;
		}

		ut_ad(key.user_defined_key_parts == 1);
		const KEY_PART_INFO&    key_part = key.key_part[0];

		/* Does not support spatial index on virtual columns */
		if (!key_part.field->stored_in_db()) {
			DBUG_RETURN(DB_UNSUPPORTED);
		}

		ulint col_nr = dict_table_has_column(
			table,
			key_part.field->field_name.str,
			key_part.fieldnr);
		ut_ad(col_nr != table->n_def);
		dict_col_t*	col = &table->cols[col_nr];

		if (col->mtype != DATA_BLOB) {
			ut_ad(DATA_GEOMETRY_MTYPE(col->mtype));
			continue;
		}

		const char* col_name = dict_table_get_col_name(
			table, col_nr);

		if (innobase_update_gis_column_type(
			table->id, col_name, trx)) {

			DBUG_RETURN(DB_ERROR);
		} else {
			col->mtype = DATA_GEOMETRY;

			ib::info() << "Updated mtype of column" << col_name
				<< " in table " << table->name
				<< ", whose id is " << table->id
				<< " to DATA_GEOMETRY";
		}
	}

	DBUG_RETURN(DB_SUCCESS);
}

/** Collect virtual column info for its addition
@param[in] ha_alter_info	Data used during in-place alter
@param[in] altered_table	MySQL table that is being altered to
@param[in] table		MySQL table as it is before the ALTER operation
@retval true Failure
@retval false Success */
static
bool
prepare_inplace_add_virtual(
	Alter_inplace_info*	ha_alter_info,
	const TABLE*		altered_table,
	const TABLE*		table)
{
	ha_innobase_inplace_ctx*	ctx;
	uint16_t i = 0, j = 0;

	ctx = static_cast<ha_innobase_inplace_ctx*>
		(ha_alter_info->handler_ctx);

	ctx->num_to_add_vcol = altered_table->s->virtual_fields
		+ ctx->num_to_drop_vcol - table->s->virtual_fields;

	ctx->add_vcol = static_cast<dict_v_col_t*>(
		 mem_heap_zalloc(ctx->heap, ctx->num_to_add_vcol
				 * sizeof *ctx->add_vcol));
	ctx->add_vcol_name = static_cast<const char**>(
		 mem_heap_alloc(ctx->heap, ctx->num_to_add_vcol
				* sizeof *ctx->add_vcol_name));

	for (const Create_field& new_field :
	     ha_alter_info->alter_info->create_list) {
		const Field* field = altered_table->field[i++];

		if (new_field.field || field->stored_in_db()) {
			continue;
		}

		unsigned is_unsigned;
		auto col_type = get_innobase_type_from_mysql_type(
			&is_unsigned, field);

		auto col_len = field->pack_length();
		unsigned field_type = field->type() | is_unsigned;

		if (!field->real_maybe_null()) {
			field_type |= DATA_NOT_NULL;
		}

		if (field->binary()) {
			field_type |= DATA_BINARY_TYPE;
		}

		unsigned charset_no;

		if (dtype_is_string_type(col_type)) {
			charset_no = field->charset()->number;

			DBUG_EXECUTE_IF(
				"ib_alter_add_virtual_fail",
				charset_no += MAX_CHAR_COLL_NUM;);

			if (charset_no > MAX_CHAR_COLL_NUM) {
				my_error(ER_WRONG_KEY_COLUMN, MYF(0), "InnoDB",
					 field->field_name.str);
				return(true);
			}
		} else {
			charset_no = 0;
		}

		if (field->type() == MYSQL_TYPE_VARCHAR) {
			uint32  length_bytes
				= static_cast<const Field_varstring*>(
					field)->length_bytes;

			col_len -= length_bytes;

			if (length_bytes == 2) {
				field_type |= DATA_LONG_TRUE_VARCHAR;
			}
		}

		new (&ctx->add_vcol[j]) dict_v_col_t();
		ctx->add_vcol[j].m_col.prtype = dtype_form_prtype(
						field_type, charset_no);

		ctx->add_vcol[j].m_col.prtype |= DATA_VIRTUAL;

		ctx->add_vcol[j].m_col.mtype = col_type;

		ctx->add_vcol[j].m_col.len = static_cast<uint16_t>(col_len);

		ctx->add_vcol[j].m_col.ind = (i - 1)
			& dict_index_t::MAX_N_FIELDS;
		ctx->add_vcol[j].num_base = 0;
		ctx->add_vcol_name[j] = field->field_name.str;
		ctx->add_vcol[j].base_col = NULL;
		ctx->add_vcol[j].v_pos = (ctx->old_table->n_v_cols
					  - ctx->num_to_drop_vcol + j)
			& dict_index_t::MAX_N_FIELDS;

		/* MDEV-17468: Do this on ctx->instant_table later */
		innodb_base_col_setup(ctx->old_table, field, &ctx->add_vcol[j]);
		j++;
	}

	return(false);
}

/** Collect virtual column info for its addition
@param[in] ha_alter_info	Data used during in-place alter
@param[in] table		MySQL table as it is before the ALTER operation
@retval true Failure
@retval false Success */
static
bool
prepare_inplace_drop_virtual(
	Alter_inplace_info*	ha_alter_info,
	const TABLE*		table)
{
	ha_innobase_inplace_ctx*	ctx;
	unsigned i = 0, j = 0;

	ctx = static_cast<ha_innobase_inplace_ctx*>
		(ha_alter_info->handler_ctx);

	ctx->num_to_drop_vcol = 0;
	for (i = 0; table->field[i]; i++) {
		const Field* field = table->field[i];
		if (field->flags & FIELD_IS_DROPPED && !field->stored_in_db()) {
			ctx->num_to_drop_vcol++;
		}
	}

	ctx->drop_vcol = static_cast<dict_v_col_t*>(
		 mem_heap_alloc(ctx->heap, ctx->num_to_drop_vcol
				* sizeof *ctx->drop_vcol));
	ctx->drop_vcol_name = static_cast<const char**>(
		 mem_heap_alloc(ctx->heap, ctx->num_to_drop_vcol
				* sizeof *ctx->drop_vcol_name));

	for (i = 0; table->field[i]; i++) {
		Field *field =  table->field[i];
		if (!(field->flags & FIELD_IS_DROPPED) || field->stored_in_db()) {
			continue;
		}

		unsigned is_unsigned;

		auto col_type = get_innobase_type_from_mysql_type(
			&is_unsigned, field);

		auto col_len = field->pack_length();
		unsigned field_type = field->type() | is_unsigned;

		if (!field->real_maybe_null()) {
			field_type |= DATA_NOT_NULL;
		}

		if (field->binary()) {
			field_type |= DATA_BINARY_TYPE;
		}

		unsigned charset_no = 0;

		if (dtype_is_string_type(col_type)) {
			charset_no = field->charset()->number;

			DBUG_EXECUTE_IF(
				"ib_alter_add_virtual_fail",
				charset_no += MAX_CHAR_COLL_NUM;);

			if (charset_no > MAX_CHAR_COLL_NUM) {
				my_error(ER_WRONG_KEY_COLUMN, MYF(0), "InnoDB",
					 field->field_name.str);
				return(true);
			}
		} else {
			charset_no = 0;
		}

		if (field->type() == MYSQL_TYPE_VARCHAR) {
			uint32  length_bytes
				= static_cast<const Field_varstring*>(
					field)->length_bytes;

			col_len -= length_bytes;

			if (length_bytes == 2) {
				field_type |= DATA_LONG_TRUE_VARCHAR;
			}
		}


		ctx->drop_vcol[j].m_col.prtype = dtype_form_prtype(
						field_type, charset_no);

		ctx->drop_vcol[j].m_col.prtype |= DATA_VIRTUAL;

		ctx->drop_vcol[j].m_col.mtype = col_type;

		ctx->drop_vcol[j].m_col.len = static_cast<uint16_t>(col_len);

		ctx->drop_vcol[j].m_col.ind = i & dict_index_t::MAX_N_FIELDS;

		ctx->drop_vcol_name[j] = field->field_name.str;

		dict_v_col_t*	v_col = dict_table_get_nth_v_col_mysql(
					ctx->old_table, i);
		ctx->drop_vcol[j].v_pos = v_col->v_pos;
		j++;
	}

	return(false);
}

/** Insert a new record to INNODB SYS_VIRTUAL
@param[in] table	InnoDB table
@param[in] pos		virtual column column no
@param[in] base_pos	base column pos
@param[in] trx		transaction
@retval	false	on success
@retval	true	on failure (my_error() will have been called) */
static bool innobase_insert_sys_virtual(
	const dict_table_t*	table,
	ulint			pos,
	ulint			base_pos,
	trx_t*			trx)
{
	pars_info_t*    info = pars_info_create();
	pars_info_add_ull_literal(info, "id", table->id);
	pars_info_add_int4_literal(info, "pos", pos);
	pars_info_add_int4_literal(info, "base_pos", base_pos);

	if (DB_SUCCESS != que_eval_sql(
		    info,
		    "PROCEDURE P () IS\n"
		    "BEGIN\n"
		    "INSERT INTO SYS_VIRTUAL VALUES (:id, :pos, :base_pos);\n"
		    "END;\n", trx)) {
		my_error(ER_INTERNAL_ERROR, MYF(0),
			 "InnoDB: ADD COLUMN...VIRTUAL");
		return true;
	}

	return false;
}

/** Insert a record to the SYS_COLUMNS dictionary table.
@param[in]	table_id	table id
@param[in]	pos		position of the column
@param[in]	field_name	field name
@param[in]	mtype		main type
@param[in]	prtype		precise type
@param[in]	len		fixed length in bytes, or 0
@param[in]	n_base		number of base columns of virtual columns, or 0
@param[in]	update		whether to update instead of inserting
@retval	false	on success
@retval	true	on failure (my_error() will have been called) */
static bool innodb_insert_sys_columns(
	table_id_t	table_id,
	ulint		pos,
	const char*	field_name,
	ulint		mtype,
	ulint		prtype,
	ulint		len,
	ulint		n_base,
	trx_t*		trx,
	bool		update = false)
{
	pars_info_t*	info = pars_info_create();
	pars_info_add_ull_literal(info, "id", table_id);
	pars_info_add_int4_literal(info, "pos", pos);
	pars_info_add_str_literal(info, "name", field_name);
	pars_info_add_int4_literal(info, "mtype", mtype);
	pars_info_add_int4_literal(info, "prtype", prtype);
	pars_info_add_int4_literal(info, "len", len);
	pars_info_add_int4_literal(info, "base", n_base);

	if (update) {
		if (DB_SUCCESS != que_eval_sql(
			    info,
			    "PROCEDURE UPD_COL () IS\n"
			    "BEGIN\n"
			    "UPDATE SYS_COLUMNS SET\n"
			    "NAME=:name, MTYPE=:mtype, PRTYPE=:prtype, "
			    "LEN=:len, PREC=:base\n"
			    "WHERE TABLE_ID=:id AND POS=:pos;\n"
			    "END;\n", trx)) {
			my_error(ER_INTERNAL_ERROR, MYF(0),
				 "InnoDB: Updating SYS_COLUMNS failed");
			return true;
		}

		return false;
	}

	if (DB_SUCCESS != que_eval_sql(
		    info,
		    "PROCEDURE ADD_COL () IS\n"
		    "BEGIN\n"
		    "INSERT INTO SYS_COLUMNS VALUES"
		    "(:id,:pos,:name,:mtype,:prtype,:len,:base);\n"
		    "END;\n", trx)) {
		my_error(ER_INTERNAL_ERROR, MYF(0),
			 "InnoDB: Insert into SYS_COLUMNS failed");
		return true;
	}

	return false;
}

/** Update INNODB SYS_COLUMNS on new virtual columns
@param[in] table	InnoDB table
@param[in] col_name	column name
@param[in] vcol		virtual column
@param[in] trx		transaction
@retval	false	on success
@retval	true	on failure (my_error() will have been called) */
static bool innobase_add_one_virtual(
	const dict_table_t*	table,
	const char*		col_name,
	dict_v_col_t*		vcol,
	trx_t*			trx)
{
	ulint		pos = dict_create_v_col_pos(vcol->v_pos,
						    vcol->m_col.ind);

	if (innodb_insert_sys_columns(table->id, pos, col_name,
				      vcol->m_col.mtype, vcol->m_col.prtype,
				      vcol->m_col.len, vcol->num_base, trx)) {
		return true;
	}

	for (unsigned i = 0; i < vcol->num_base; i++) {
		if (innobase_insert_sys_virtual(
			    table, pos, vcol->base_col[i]->ind, trx)) {
			return true;
		}
	}

	return false;
}

/** Update SYS_TABLES.N_COLS in the data dictionary.
@param[in] user_table	InnoDB table
@param[in] n		the new value of SYS_TABLES.N_COLS
@param[in] trx		transaction
@return whether the operation failed */
static bool innodb_update_cols(const dict_table_t* table, ulint n, trx_t* trx)
{
	pars_info_t*    info = pars_info_create();

	pars_info_add_int4_literal(info, "n", n);
	pars_info_add_ull_literal(info, "id", table->id);

	if (DB_SUCCESS != que_eval_sql(info,
				       "PROCEDURE UPDATE_N_COLS () IS\n"
				       "BEGIN\n"
				       "UPDATE SYS_TABLES SET N_COLS = :n"
				       " WHERE ID = :id;\n"
				       "END;\n", trx)) {
		my_error(ER_INTERNAL_ERROR, MYF(0),
			 "InnoDB: Updating SYS_TABLES.N_COLS failed");
		return true;
	}

	return false;
}

/** Update system table for adding virtual column(s)
@param[in]	ha_alter_info	Data used during in-place alter
@param[in]	user_table	InnoDB table
@param[in]	trx		transaction
@retval true Failure
@retval false Success */
static
bool
innobase_add_virtual_try(
	const Alter_inplace_info*	ha_alter_info,
	const dict_table_t*		user_table,
	trx_t*				trx)
{
	ha_innobase_inplace_ctx* ctx = static_cast<ha_innobase_inplace_ctx*>(
		ha_alter_info->handler_ctx);

	for (ulint i = 0; i < ctx->num_to_add_vcol; i++) {
		if (innobase_add_one_virtual(
			    user_table, ctx->add_vcol_name[i],
			    &ctx->add_vcol[i], trx)) {
			return true;
		}
	}

	return false;
}

/** Delete metadata from SYS_COLUMNS and SYS_VIRTUAL.
@param[in]	id	table id
@param[in]	pos	first SYS_COLUMNS.POS
@param[in,out]	trx	data dictionary transaction
@retval true Failure
@retval false Success. */
static bool innobase_instant_drop_cols(table_id_t id, ulint pos, trx_t* trx)
{
	pars_info_t*	info = pars_info_create();
	pars_info_add_ull_literal(info, "id", id);
	pars_info_add_int4_literal(info, "pos", pos);

	dberr_t err = que_eval_sql(
			info,
			"PROCEDURE DELETE_COL () IS\n"
			"BEGIN\n"
			"DELETE FROM SYS_COLUMNS WHERE\n"
			"TABLE_ID = :id AND POS >= :pos;\n"
			"DELETE FROM SYS_VIRTUAL WHERE TABLE_ID = :id;\n"
			"END;\n", trx);
	if (err != DB_SUCCESS) {
		my_error(ER_INTERNAL_ERROR, MYF(0),
			 "InnoDB: DELETE from SYS_COLUMNS/SYS_VIRTUAL failed");
		return true;
	}

	return false;
}

/** Update INNODB SYS_COLUMNS on new virtual column's position
@param[in]	table	InnoDB table
@param[in]	old_pos	old position
@param[in]	new_pos	new position
@param[in]	trx	transaction
@return DB_SUCCESS if successful, otherwise error code */
static
dberr_t
innobase_update_v_pos_sys_columns(
	const dict_table_t*	table,
	ulint			old_pos,
	ulint			new_pos,
	trx_t*			trx)
{
	pars_info_t*    info = pars_info_create();

	pars_info_add_int4_literal(info, "pos", old_pos);
	pars_info_add_int4_literal(info, "val", new_pos);
	pars_info_add_ull_literal(info, "id", table->id);

	dberr_t error = que_eval_sql(
			info,
			"PROCEDURE P () IS\n"
			"BEGIN\n"
			"UPDATE SYS_COLUMNS\n"
			"SET POS = :val\n"
			"WHERE POS = :pos\n"
			"AND TABLE_ID = :id;\n"
			"END;\n", trx);

	return(error);
}

/** Update INNODB SYS_VIRTUAL table with new virtual column position
@param[in]	table		InnoDB table
@param[in]	old_pos		old position
@param[in]	new_pos		new position
@param[in]	trx		transaction
@return DB_SUCCESS if successful, otherwise error code */
static
dberr_t
innobase_update_v_pos_sys_virtual(
	const dict_table_t*	table,
	ulint			old_pos,
	ulint			new_pos,
	trx_t*			trx)
{
	pars_info_t*    info = pars_info_create();

	pars_info_add_int4_literal(info, "pos", old_pos);
	pars_info_add_int4_literal(info, "val", new_pos);
	pars_info_add_ull_literal(info, "id", table->id);

	dberr_t error = que_eval_sql(
			info,
			"PROCEDURE P () IS\n"
			"BEGIN\n"
			"UPDATE SYS_VIRTUAL\n"
			"SET POS = :val\n"
			"WHERE POS = :pos\n"
			"AND TABLE_ID = :id;\n"
			"END;\n", trx);

	return(error);
}

/** Update InnoDB system tables on dropping a virtual column
@param[in]	table		InnoDB table
@param[in]	col_name	column name of the dropping column
@param[in]	drop_col	col information for the dropping column
@param[in]	n_prev_dropped	number of previously dropped columns in the
				same alter clause
@param[in]	trx		transaction
@return DB_SUCCESS if successful, otherwise error code */
static
dberr_t
innobase_drop_one_virtual_sys_columns(
	const dict_table_t*	table,
	const char*		col_name,
	dict_col_t*		drop_col,
	ulint			n_prev_dropped,
	trx_t*			trx)
{
	pars_info_t*    info = pars_info_create();
	pars_info_add_ull_literal(info, "id", table->id);

	pars_info_add_str_literal(info, "name", col_name);

	dberr_t error = que_eval_sql(
			info,
			"PROCEDURE P () IS\n"
			"BEGIN\n"
			"DELETE FROM SYS_COLUMNS\n"
			"WHERE TABLE_ID = :id\n"
			"AND NAME = :name;\n"
			"END;\n", trx);

	if (error != DB_SUCCESS) {
		return(error);
	}

	dict_v_col_t*	v_col = dict_table_get_nth_v_col_mysql(
				table, drop_col->ind);

	/* Adjust column positions for all subsequent columns */
	for (ulint i = v_col->v_pos + 1; i < table->n_v_cols; i++) {
		dict_v_col_t*   t_col = dict_table_get_nth_v_col(table, i);
		ulint		old_p = dict_create_v_col_pos(
			t_col->v_pos - n_prev_dropped,
			t_col->m_col.ind - n_prev_dropped);
		ulint		new_p = dict_create_v_col_pos(
			t_col->v_pos - 1 - n_prev_dropped,
			ulint(t_col->m_col.ind) - 1 - n_prev_dropped);

		error = innobase_update_v_pos_sys_columns(
			table, old_p, new_p, trx);
		if (error != DB_SUCCESS) {
			return(error);
		}
		error = innobase_update_v_pos_sys_virtual(
			table, old_p, new_p, trx);
		if (error != DB_SUCCESS) {
			return(error);
		}
	}

	return(error);
}

/** Delete virtual column's info from INNODB SYS_VIRTUAL
@param[in]	table	InnoDB table
@param[in]	pos	position of the virtual column to be deleted
@param[in]	trx	transaction
@return DB_SUCCESS if successful, otherwise error code */
static
dberr_t
innobase_drop_one_virtual_sys_virtual(
	const dict_table_t*	table,
	ulint			pos,
	trx_t*			trx)
{
	pars_info_t*    info = pars_info_create();
	pars_info_add_ull_literal(info, "id", table->id);

	pars_info_add_int4_literal(info, "pos", pos);

	dberr_t error = que_eval_sql(
			info,
			"PROCEDURE P () IS\n"
			"BEGIN\n"
			"DELETE FROM SYS_VIRTUAL\n"
			"WHERE TABLE_ID = :id\n"
			"AND POS = :pos;\n"
			"END;\n", trx);

	return(error);
}

/** Update system table for dropping virtual column(s)
@param[in]	ha_alter_info	Data used during in-place alter
@param[in]	user_table	InnoDB table
@param[in]	trx		transaction
@retval true Failure
@retval false Success */
static
bool
innobase_drop_virtual_try(
	const Alter_inplace_info*	ha_alter_info,
	const dict_table_t*	     user_table,
	trx_t*				trx)
{
	ha_innobase_inplace_ctx*	ctx;
	dberr_t				err = DB_SUCCESS;

	ctx = static_cast<ha_innobase_inplace_ctx*>
		(ha_alter_info->handler_ctx);

	for (unsigned i = 0; i < ctx->num_to_drop_vcol; i++) {

		ulint	pos = dict_create_v_col_pos(
			ctx->drop_vcol[i].v_pos - i,
			ctx->drop_vcol[i].m_col.ind - i);
		err = innobase_drop_one_virtual_sys_virtual(
			user_table, pos, trx);

		if (err != DB_SUCCESS) {
			my_error(ER_INTERNAL_ERROR, MYF(0),
				 "InnoDB: DROP COLUMN...VIRTUAL");
			return(true);
		}

		err = innobase_drop_one_virtual_sys_columns(
			user_table, ctx->drop_vcol_name[i],
			&(ctx->drop_vcol[i].m_col), i, trx);

		if (err != DB_SUCCESS) {
			my_error(ER_INTERNAL_ERROR, MYF(0),
				 "InnoDB: DROP COLUMN...VIRTUAL");
			return(true);
		}
	}

	return false;
}

/** Serialise metadata of dropped or reordered columns.
@param[in,out]	heap	memory heap for allocation
@param[out]	field	data field with the metadata */
inline
void dict_table_t::serialise_columns(mem_heap_t* heap, dfield_t* field) const
{
	DBUG_ASSERT(instant);
	const dict_index_t& index = *UT_LIST_GET_FIRST(indexes);
	unsigned n_fixed = index.first_user_field();
	unsigned num_non_pk_fields = index.n_fields - n_fixed;

	ulint len = 4 + num_non_pk_fields * 2;

	byte* data = static_cast<byte*>(mem_heap_alloc(heap, len));

	dfield_set_data(field, data, len);

	mach_write_to_4(data, num_non_pk_fields);

	data += 4;

	for (ulint i = n_fixed; i < index.n_fields; i++) {
		mach_write_to_2(data, instant->field_map[i - n_fixed]);
		data += 2;
	}
}

/** Construct the metadata record for instant ALTER TABLE.
@param[in]	row	dummy or default values for existing columns
@param[in,out]	heap	memory heap for allocations
@return	metadata record */
inline
dtuple_t*
dict_index_t::instant_metadata(const dtuple_t& row, mem_heap_t* heap) const
{
	ut_ad(is_primary());
	dtuple_t* entry;

	if (!table->instant) {
		entry = row_build_index_entry(&row, NULL, this, heap);
		entry->info_bits = REC_INFO_METADATA_ADD;
		return entry;
	}

	entry = dtuple_create(heap, n_fields + 1);
	entry->n_fields_cmp = n_uniq;
	entry->info_bits = REC_INFO_METADATA_ALTER;

	const dict_field_t* field = fields;

	for (uint i = 0; i <= n_fields; i++, field++) {
		dfield_t* dfield = dtuple_get_nth_field(entry, i);

		if (i == first_user_field()) {
			table->serialise_columns(heap, dfield);
			dfield->type.metadata_blob_init();
			field--;
			continue;
		}

		ut_ad(!field->col->is_virtual());

		if (field->col->is_dropped()) {
			dict_col_copy_type(field->col, &dfield->type);
			if (field->col->is_nullable()) {
				dfield_set_null(dfield);
			} else {
				dfield_set_data(dfield, field_ref_zero,
						field->fixed_len);
			}
			continue;
		}

		const dfield_t* s = dtuple_get_nth_field(&row, field->col->ind);
		ut_ad(dict_col_type_assert_equal(field->col, &s->type));
		*dfield = *s;

		if (dfield_is_null(dfield)) {
			continue;
		}

		if (dfield_is_ext(dfield)) {
			ut_ad(i > first_user_field());
			ut_ad(!field->prefix_len);
			ut_ad(dfield->len >= FIELD_REF_SIZE);
			dfield_set_len(dfield, dfield->len - FIELD_REF_SIZE);
		}

		if (!field->prefix_len) {
			continue;
		}

		ut_ad(field->col->ord_part);
		ut_ad(i < n_uniq);

		ulint len = dtype_get_at_most_n_mbchars(
			field->col->prtype,
			field->col->mbminlen, field->col->mbmaxlen,
			field->prefix_len, dfield->len,
			static_cast<char*>(dfield_get_data(dfield)));
		dfield_set_len(dfield, len);
	}

	return entry;
}

/** Insert or update SYS_COLUMNS and the hidden metadata record
for instant ALTER TABLE.
@param[in]	ha_alter_info	ALTER TABLE context
@param[in,out]	ctx		ALTER TABLE context for the current partition
@param[in]	altered_table	MySQL table that is being altered
@param[in]	table		MySQL table as it is before the ALTER operation
@param[in,out]	trx		dictionary transaction
@retval	true	failure
@retval	false	success */
static bool innobase_instant_try(
	const Alter_inplace_info*	ha_alter_info,
	ha_innobase_inplace_ctx*	ctx,
	const TABLE*			altered_table,
	const TABLE*			table,
	trx_t*				trx)
{
	DBUG_ASSERT(!ctx->need_rebuild());
	DBUG_ASSERT(ctx->is_instant());

	dict_table_t* user_table = ctx->old_table;

	dict_index_t* index = dict_table_get_first_index(user_table);
	const unsigned n_old_fields = index->n_fields;
	const dict_col_t* old_cols = user_table->cols;
	DBUG_ASSERT(user_table->n_cols == ctx->old_n_cols);

	const bool metadata_changed = ctx->instant_column();

	DBUG_ASSERT(index->n_fields >= n_old_fields);
	/* The table may have been emptied and may have lost its
	'instantness' during this ALTER TABLE. */

	/* Construct a table row of default values for the stored columns. */
	dtuple_t* row = dtuple_create(ctx->heap, user_table->n_cols);
	dict_table_copy_types(row, user_table);
	Field** af = altered_table->field;
	Field** const end = altered_table->field + altered_table->s->fields;
	ut_d(List_iterator_fast<Create_field> cf_it(
		     ha_alter_info->alter_info->create_list));
	if (ctx->first_alter_pos
	    && innobase_instant_drop_cols(user_table->id,
					  ctx->first_alter_pos - 1, trx)) {
		return true;
	}
	for (uint i = 0; af < end; af++) {
		if (!(*af)->stored_in_db()) {
			ut_d(cf_it++);
			continue;
		}

		const dict_col_t* old = dict_table_t::find(old_cols,
							   ctx->col_map,
							   ctx->old_n_cols, i);
		DBUG_ASSERT(!old || i >= ctx->old_n_cols - DATA_N_SYS_COLS
			    || old->ind == i
			    || (ctx->first_alter_pos
				&& old->ind >= ctx->first_alter_pos - 1));

		dfield_t* d = dtuple_get_nth_field(row, i);
		const dict_col_t* col = dict_table_get_nth_col(user_table, i);
		DBUG_ASSERT(!col->is_virtual());
		DBUG_ASSERT(!col->is_dropped());
		DBUG_ASSERT(col->mtype != DATA_SYS);
		DBUG_ASSERT(!strcmp((*af)->field_name.str,
				    dict_table_get_col_name(user_table, i)));
		DBUG_ASSERT(old || col->is_added());

		ut_d(const Create_field* new_field = cf_it++);
		/* new_field->field would point to an existing column.
		If it is NULL, the column was added by this ALTER TABLE. */
		ut_ad(!new_field->field == !old);

		if (col->is_added()) {
			dfield_set_data(d, col->def_val.data,
					col->def_val.len);
		} else if ((*af)->real_maybe_null()) {
			/* Store NULL for nullable 'core' columns. */
			dfield_set_null(d);
		} else {
			switch ((*af)->type()) {
			case MYSQL_TYPE_VARCHAR:
			case MYSQL_TYPE_GEOMETRY:
			case MYSQL_TYPE_TINY_BLOB:
			case MYSQL_TYPE_MEDIUM_BLOB:
			case MYSQL_TYPE_BLOB:
			case MYSQL_TYPE_LONG_BLOB:
			variable_length:
				/* Store the empty string for 'core'
				variable-length NOT NULL columns. */
				dfield_set_data(d, field_ref_zero, 0);
				break;
			case MYSQL_TYPE_STRING:
				if (col->mbminlen != col->mbmaxlen
				    && user_table->not_redundant()) {
					goto variable_length;
				}
				/* fall through */
			default:
				/* For fixed-length NOT NULL 'core' columns,
				get a dummy default value from SQL. Note that
				we will preserve the old values of these
				columns when updating the metadata
				record, to avoid unnecessary updates. */
				ulint len = (*af)->pack_length();
				DBUG_ASSERT(d->type.mtype != DATA_INT
					    || len <= 8);
				row_mysql_store_col_in_innobase_format(
					d, d->type.mtype == DATA_INT
					? static_cast<byte*>(
						mem_heap_alloc(ctx->heap, len))
					: NULL, true, (*af)->ptr, len,
					dict_table_is_comp(user_table));
				ut_ad(new_field->field->pack_length() == len);
			}
		}

		bool update = old && (!ctx->first_alter_pos
				      || i < ctx->first_alter_pos - 1);
		DBUG_ASSERT(!old || col->same_format(*old));
		if (update
		    && old->prtype == d->type.prtype) {
			/* The record is already present in SYS_COLUMNS. */
		} else if (innodb_insert_sys_columns(user_table->id, i,
						     (*af)->field_name.str,
						     d->type.mtype,
						     d->type.prtype,
						     d->type.len, 0, trx,
						     update)) {
			return true;
		}

		i++;
	}

	if (innodb_update_cols(user_table, dict_table_encode_n_col(
				       unsigned(user_table->n_cols)
				       - DATA_N_SYS_COLS,
				       user_table->n_v_cols)
			       | (user_table->flags & DICT_TF_COMPACT) << 31,
			       trx)) {
		return true;
	}

	if (ctx->first_alter_pos) {
add_all_virtual:
		for (uint i = 0; i < user_table->n_v_cols; i++) {
			if (innobase_add_one_virtual(
				    user_table,
				    dict_table_get_v_col_name(user_table, i),
				    &user_table->v_cols[i], trx)) {
				return true;
			}
		}
	} else if (ha_alter_info->handler_flags & ALTER_DROP_VIRTUAL_COLUMN) {
		if (innobase_instant_drop_cols(user_table->id, 65536, trx)) {
			return true;
		}
		goto add_all_virtual;
	} else if ((ha_alter_info->handler_flags & ALTER_ADD_VIRTUAL_COLUMN)
		   && innobase_add_virtual_try(ha_alter_info, user_table,
					       trx)) {
		return true;
        }

	if (!user_table->space) {
		/* In case of ALTER TABLE...DISCARD TABLESPACE,
		update only the metadata and transform the dictionary
		cache entry to the canonical format. */
		index->clear_instant_alter();
		return false;
	}

	unsigned i = unsigned(user_table->n_cols) - DATA_N_SYS_COLS;
	DBUG_ASSERT(i >= altered_table->s->stored_fields);
	DBUG_ASSERT(i <= altered_table->s->stored_fields + 1);
	if (i > altered_table->s->fields) {
		const dict_col_t& fts_doc_id = user_table->cols[i - 1];
		DBUG_ASSERT(!strcmp(fts_doc_id.name(*user_table),
				    FTS_DOC_ID_COL_NAME));
		DBUG_ASSERT(!fts_doc_id.is_nullable());
		DBUG_ASSERT(fts_doc_id.len == 8);
		dfield_set_data(dtuple_get_nth_field(row, i - 1),
				field_ref_zero, fts_doc_id.len);
	}
	byte trx_id[DATA_TRX_ID_LEN], roll_ptr[DATA_ROLL_PTR_LEN];
	dfield_set_data(dtuple_get_nth_field(row, i++), field_ref_zero,
			DATA_ROW_ID_LEN);
	dfield_set_data(dtuple_get_nth_field(row, i++), trx_id, sizeof trx_id);
	dfield_set_data(dtuple_get_nth_field(row, i),roll_ptr,sizeof roll_ptr);
	DBUG_ASSERT(i + 1 == user_table->n_cols);

	trx_write_trx_id(trx_id, trx->id);
	/* The DB_ROLL_PTR will be assigned later, when allocating undo log.
	Silence a Valgrind warning in dtuple_validate() when
	row_ins_clust_index_entry_low() searches for the insert position. */
	memset(roll_ptr, 0, sizeof roll_ptr);

	dtuple_t* entry = index->instant_metadata(*row, ctx->heap);
	mtr_t	mtr;
	mtr.start();
	index->set_modified(mtr);
	btr_pcur_t pcur;
	btr_pcur_open_at_index_side(true, index, BTR_MODIFY_TREE, &pcur, true,
				    0, &mtr);
	ut_ad(btr_pcur_is_before_first_on_page(&pcur));
	btr_pcur_move_to_next_on_page(&pcur);

	buf_block_t* block = btr_pcur_get_block(&pcur);
	ut_ad(page_is_leaf(block->page.frame));
	ut_ad(!page_has_prev(block->page.frame));
	ut_ad(!buf_block_get_page_zip(block));
	const rec_t* rec = btr_pcur_get_rec(&pcur);
	que_thr_t* thr = pars_complete_graph_for_exec(
		NULL, trx, ctx->heap, NULL);
	const bool is_root = block->page.id().page_no() == index->page;

	dberr_t err = DB_SUCCESS;
	if (rec_is_metadata(rec, *index)) {
		ut_ad(page_rec_is_user_rec(rec));
		if (is_root
		    && !rec_is_alter_metadata(rec, *index)
		    && !index->table->instant
		    && !page_has_next(block->page.frame)
		    && page_rec_is_last(rec, block->page.frame)) {
			goto empty_table;
		}

		if (!metadata_changed) {
			goto func_exit;
		}

		/* Ensure that the root page is in the correct format. */
		buf_block_t* root = btr_root_block_get(index, RW_X_LATCH,
						       &mtr);
		DBUG_ASSERT(root);
		if (fil_page_get_type(root->page.frame)
		    != FIL_PAGE_TYPE_INSTANT) {
			DBUG_ASSERT("wrong page type" == 0);
			err = DB_CORRUPTION;
			goto func_exit;
		}

		btr_set_instant(root, *index, &mtr);

		/* Extend the record with any added columns. */
		uint n = uint(index->n_fields) - n_old_fields;
		/* Reserve room for DB_TRX_ID,DB_ROLL_PTR and any
		non-updated off-page columns in case they are moved off
		page as a result of the update. */
		const uint16_t f = user_table->instant != NULL;
		upd_t* update = upd_create(index->n_fields + f, ctx->heap);
		update->n_fields = n + f;
		update->info_bits = f
			? REC_INFO_METADATA_ALTER
			: REC_INFO_METADATA_ADD;
		if (f) {
			upd_field_t* uf = upd_get_nth_field(update, 0);
			uf->field_no = index->first_user_field();
			uf->new_val = entry->fields[uf->field_no];
			DBUG_ASSERT(!dfield_is_ext(&uf->new_val));
			DBUG_ASSERT(!dfield_is_null(&uf->new_val));
		}

		/* Add the default values for instantly added columns */
		unsigned j = f;

		for (unsigned k = n_old_fields; k < index->n_fields; k++) {
			upd_field_t* uf = upd_get_nth_field(update, j++);
			uf->field_no = static_cast<uint16_t>(k + f);
			uf->new_val = entry->fields[k + f];

			ut_ad(j <= n + f);
		}

		ut_ad(j == n + f);

		rec_offs* offsets = NULL;
		mem_heap_t* offsets_heap = NULL;
		big_rec_t* big_rec;
		err = btr_cur_pessimistic_update(
			BTR_NO_LOCKING_FLAG | BTR_KEEP_POS_FLAG,
			btr_pcur_get_btr_cur(&pcur),
			&offsets, &offsets_heap, ctx->heap,
			&big_rec, update, UPD_NODE_NO_ORD_CHANGE,
			thr, trx->id, &mtr);

		offsets = rec_get_offsets(
			btr_pcur_get_rec(&pcur), index, offsets,
			index->n_core_fields, ULINT_UNDEFINED, &offsets_heap);
		if (big_rec) {
			if (err == DB_SUCCESS) {
				err = btr_store_big_rec_extern_fields(
					&pcur, offsets, big_rec, &mtr,
					BTR_STORE_UPDATE);
			}

			dtuple_big_rec_free(big_rec);
		}
		if (offsets_heap) {
			mem_heap_free(offsets_heap);
		}
		ut_free(pcur.old_rec_buf);
		goto func_exit;
	} else if (is_root && page_rec_is_supremum(rec)
		   && !index->table->instant) {
empty_table:
		/* The table is empty. */
		ut_ad(fil_page_index_page_check(block->page.frame));
		ut_ad(!page_has_siblings(block->page.frame));
		ut_ad(block->page.id().page_no() == index->page);
		/* MDEV-17383: free metadata BLOBs! */
		btr_page_empty(block, NULL, index, 0, &mtr);
		if (index->is_instant()) {
			index->clear_instant_add();
		}
		goto func_exit;
	} else if (!user_table->is_instant()) {
		ut_ad(!user_table->not_redundant());
		goto func_exit;
	}

	/* Convert the table to the instant ALTER TABLE format. */
	mtr.commit();
	mtr.start();
	index->set_modified(mtr);
	if (buf_block_t* root = btr_root_block_get(index, RW_SX_LATCH, &mtr)) {
		if (fil_page_get_type(root->page.frame) != FIL_PAGE_INDEX) {
			DBUG_ASSERT("wrong page type" == 0);
			goto err_exit;
		}

		btr_set_instant(root, *index, &mtr);
		mtr.commit();
		mtr.start();
		index->set_modified(mtr);
		err = row_ins_clust_index_entry_low(
			BTR_NO_LOCKING_FLAG, BTR_MODIFY_TREE, index,
			index->n_uniq, entry, 0, thr);
	} else {
err_exit:
		err = DB_CORRUPTION;
	}

func_exit:
	mtr.commit();

	if (err != DB_SUCCESS) {
		my_error_innodb(err, table->s->table_name.str,
				user_table->flags);
		return true;
	}

	return false;
}

/** Adjust the create index column number from "New table" to
"old InnoDB table" while we are doing dropping virtual column. Since we do
not create separate new table for the dropping/adding virtual columns.
To correctly find the indexed column, we will need to find its col_no
in the "Old Table", not the "New table".
@param[in]	ha_alter_info	Data used during in-place alter
@param[in]	old_table	MySQL table as it is before the ALTER operation
@param[in]	num_v_dropped	number of virtual column dropped
@param[in,out]	index_def	index definition */
static
void
innodb_v_adjust_idx_col(
	const Alter_inplace_info*	ha_alter_info,
	const TABLE*			old_table,
	ulint				num_v_dropped,
	index_def_t*			index_def)
{
	for (ulint i = 0; i < index_def->n_fields; i++) {
#ifdef UNIV_DEBUG
		bool	col_found = false;
#endif /* UNIV_DEBUG */
		ulint	num_v = 0;

		index_field_t*	index_field = &index_def->fields[i];

		/* Only adjust virtual column col_no, since non-virtual
		column position (in non-vcol list) won't change unless
		table rebuild */
		if (!index_field->is_v_col) {
			continue;
		}

		const Field*	field = NULL;

		/* Found the field in the new table */
		for (const Create_field& new_field :
		     ha_alter_info->alter_info->create_list) {
			if (new_field.stored_in_db()) {
				continue;
			}

			field = new_field.field;

			if (num_v == index_field->col_no) {
				break;
			}
			num_v++;
		}

		if (!field) {
			/* this means the field is a newly added field, this
			should have been blocked when we drop virtual column
			at the same time */
			ut_ad(num_v_dropped > 0);
			ut_a(0);
		}

		ut_ad(!field->stored_in_db());

		num_v = 0;

		/* Look for its position in old table */
		for (uint old_i = 0; old_table->field[old_i]; old_i++) {
			if (old_table->field[old_i] == field) {
				/* Found it, adjust its col_no to its position
				in old table */
				index_def->fields[i].col_no = num_v;
				ut_d(col_found = true);
				break;
			}

			num_v += !old_table->field[old_i]->stored_in_db();
		}

		ut_ad(col_found);
	}
}

/** Create index metadata in the data dictionary.
@param[in,out]	trx	dictionary transaction
@param[in,out]	index	index being created
@param[in]	mode	encryption mode (for creating a table)
@param[in]	key_id	encryption key identifier (for creating a table)
@param[in]	add_v	virtual columns that are being added, or NULL
@return the created index */
MY_ATTRIBUTE((nonnull(1,2), warn_unused_result))
static
dict_index_t*
create_index_dict(
	trx_t*			trx,
	dict_index_t*		index,
	fil_encryption_t	mode,
	uint32_t		key_id,
	const dict_add_v_col_t* add_v)
{
	DBUG_ENTER("create_index_dict");

	mem_heap_t* heap = mem_heap_create(512);
	ind_node_t* node = ind_create_graph_create(
		index, index->table->name.m_name, heap, mode, key_id, add_v);
	que_thr_t* thr = pars_complete_graph_for_exec(node, trx, heap, NULL);

	que_fork_start_command(
		static_cast<que_fork_t*>(que_node_get_parent(thr)));

	que_run_threads(thr);

	DBUG_ASSERT(trx->error_state != DB_SUCCESS || index != node->index);
	DBUG_ASSERT(trx->error_state != DB_SUCCESS || node->index);
	index = node->index;

	que_graph_free((que_t*) que_node_get_parent(thr));

	DBUG_RETURN(index);
}

/** Update internal structures with concurrent writes blocked,
while preparing ALTER TABLE.

@param ha_alter_info Data used during in-place alter
@param altered_table MySQL table that is being altered
@param old_table MySQL table as it is before the ALTER operation
@param table_name Table name in MySQL
@param flags Table and tablespace flags
@param flags2 Additional table flags
@param fts_doc_id_col The column number of FTS_DOC_ID
@param add_fts_doc_id Flag: add column FTS_DOC_ID?
@param add_fts_doc_id_idx Flag: add index FTS_DOC_ID_INDEX (FTS_DOC_ID)?

@retval true Failure
@retval false Success
*/
static MY_ATTRIBUTE((warn_unused_result, nonnull(1,2,3,4)))
bool
prepare_inplace_alter_table_dict(
/*=============================*/
	Alter_inplace_info*	ha_alter_info,
	const TABLE*		altered_table,
	const TABLE*		old_table,
	const char*		table_name,
	ulint			flags,
	ulint			flags2,
	ulint			fts_doc_id_col,
	bool			add_fts_doc_id,
	bool			add_fts_doc_id_idx)
{
	bool			dict_locked	= false;
	ulint*			add_key_nums;	/* MySQL key numbers */
	index_def_t*		index_defs;	/* index definitions */
	dict_table_t*		user_table;
	dict_index_t*		fts_index	= NULL;
	bool			new_clustered	= false;
	dberr_t			error		= DB_SUCCESS;
	ulint			num_fts_index;
	dict_add_v_col_t*	add_v = NULL;
	ha_innobase_inplace_ctx*ctx;

	DBUG_ENTER("prepare_inplace_alter_table_dict");

	ctx = static_cast<ha_innobase_inplace_ctx*>
		(ha_alter_info->handler_ctx);

	DBUG_ASSERT((ctx->add_autoinc != ULINT_UNDEFINED)
		    == (ctx->sequence.max_value() > 0));
	DBUG_ASSERT(!ctx->num_to_drop_index == !ctx->drop_index);
	DBUG_ASSERT(!ctx->num_to_drop_fk == !ctx->drop_fk);
	DBUG_ASSERT(!add_fts_doc_id || add_fts_doc_id_idx);
	DBUG_ASSERT(!add_fts_doc_id_idx
		    || innobase_fulltext_exist(altered_table));
	DBUG_ASSERT(!ctx->defaults);
	DBUG_ASSERT(!ctx->add_index);
	DBUG_ASSERT(!ctx->add_key_numbers);
	DBUG_ASSERT(!ctx->num_to_add_index);

	user_table = ctx->new_table;

	switch (ha_alter_info->inplace_supported) {
	default: break;
	case HA_ALTER_INPLACE_INSTANT:
	case HA_ALTER_INPLACE_NOCOPY_LOCK:
	case HA_ALTER_INPLACE_NOCOPY_NO_LOCK:
		/* If we promised ALGORITHM=NOCOPY or ALGORITHM=INSTANT,
		we must retain the original ROW_FORMAT of the table. */
		flags = (user_table->flags & (DICT_TF_MASK_COMPACT
					      | DICT_TF_MASK_ATOMIC_BLOBS))
			| (flags & ~(DICT_TF_MASK_COMPACT
				     | DICT_TF_MASK_ATOMIC_BLOBS));
	}

	trx_start_if_not_started_xa(ctx->prebuilt->trx, true);

	if (ha_alter_info->handler_flags
	    & ALTER_DROP_VIRTUAL_COLUMN) {
		if (prepare_inplace_drop_virtual(ha_alter_info, old_table)) {
			DBUG_RETURN(true);
		}
	}

	if (ha_alter_info->handler_flags
	    & ALTER_ADD_VIRTUAL_COLUMN) {
		if (prepare_inplace_add_virtual(
			    ha_alter_info, altered_table, old_table)) {
			DBUG_RETURN(true);
		}

		/* Need information for newly added virtual columns
		for create index */

		if (ha_alter_info->handler_flags
		    & ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX) {
			for (ulint i = 0; i < ctx->num_to_add_vcol; i++) {
				/* Set mbminmax for newly added column */
				dict_col_t& col = ctx->add_vcol[i].m_col;
				unsigned mbminlen, mbmaxlen;
				dtype_get_mblen(col.mtype, col.prtype,
						&mbminlen, &mbmaxlen);
				col.mbminlen = mbminlen & 7;
				col.mbmaxlen = mbmaxlen & 7;
			}
			add_v = static_cast<dict_add_v_col_t*>(
				mem_heap_alloc(ctx->heap, sizeof *add_v));
			add_v->n_v_col = ctx->num_to_add_vcol;
			add_v->v_col = ctx->add_vcol;
			add_v->v_col_name = ctx->add_vcol_name;
		}
	}

	/* There should be no order change for virtual columns coming in
	here */
	ut_ad(check_v_col_in_order(old_table, altered_table, ha_alter_info));

	/* Create table containing all indexes to be built in this
	ALTER TABLE ADD INDEX so that they are in the correct order
	in the table. */

	ctx->num_to_add_index = ha_alter_info->index_add_count;

	ut_ad(ctx->prebuilt->trx->mysql_thd != NULL);
	const char*	path = thd_innodb_tmpdir(
		ctx->prebuilt->trx->mysql_thd);

	index_defs = ctx->create_key_defs(
		ha_alter_info, altered_table,
		num_fts_index,
		fts_doc_id_col, add_fts_doc_id, add_fts_doc_id_idx,
		old_table);

	new_clustered = (DICT_CLUSTERED & index_defs[0].ind_type) != 0;

	create_table_info_t info(ctx->prebuilt->trx->mysql_thd, altered_table,
				 ha_alter_info->create_info, NULL, NULL,
				 srv_file_per_table);

	/* The primary index would be rebuilt if a FTS Doc ID
	column is to be added, and the primary index definition
	is just copied from old table and stored in indexdefs[0] */
	DBUG_ASSERT(!add_fts_doc_id || new_clustered);
	DBUG_ASSERT(!!new_clustered ==
		    (innobase_need_rebuild(ha_alter_info, old_table)
		     || add_fts_doc_id));

	/* Allocate memory for dictionary index definitions */

	ctx->add_index = static_cast<dict_index_t**>(
		mem_heap_zalloc(ctx->heap, ctx->num_to_add_index
			       * sizeof *ctx->add_index));
	ctx->add_key_numbers = add_key_nums = static_cast<ulint*>(
		mem_heap_alloc(ctx->heap, ctx->num_to_add_index
			       * sizeof *ctx->add_key_numbers));

	const bool fts_exist = ctx->new_table->flags2
		& (DICT_TF2_FTS_HAS_DOC_ID | DICT_TF2_FTS);
	/* Acquire a lock on the table before creating any indexes. */
	bool table_lock_failed = false;

	if (!ctx->online) {
acquire_lock:
		ctx->prebuilt->trx->op_info = "acquiring table lock";
		error = lock_table_for_trx(user_table, ctx->trx, LOCK_S);
	} else if (add_key_nums) {
		/* FIXME: trx_resurrect_table_locks() will not resurrect
		MDL for any recovered transactions that may hold locks on
		the table. We will prevent race conditions by "unnecessarily"
		acquiring an InnoDB table lock even for online operation,
		to ensure that the rollback of recovered transactions will
		not run concurrently with online ADD INDEX. */
		user_table->lock_mutex_lock();
		for (lock_t *lock = UT_LIST_GET_FIRST(user_table->locks);
		     lock;
		     lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock)) {
			if (lock->trx->is_recovered) {
				user_table->lock_mutex_unlock();
				goto acquire_lock;
			}
		}
		user_table->lock_mutex_unlock();
	}

	if (fts_exist) {
		purge_sys.stop_FTS(*ctx->new_table);
		if (error == DB_SUCCESS) {
			error = fts_lock_tables(ctx->trx, *ctx->new_table);
		}
	}

	if (error == DB_SUCCESS) {
		error = lock_sys_tables(ctx->trx);
	}

	if (error != DB_SUCCESS) {
		table_lock_failed = true;
		goto error_handling;
	}

	/* Latch the InnoDB data dictionary exclusively so that no deadlocks
	or lock waits can happen in it during an index create operation. */

	row_mysql_lock_data_dictionary(ctx->trx);
	dict_locked = true;
	online_retry_drop_indexes_low(ctx->new_table, ctx->trx);

	ut_d(dict_table_check_for_dup_indexes(
		     ctx->new_table, CHECK_ABORTED_OK));

	DBUG_EXECUTE_IF("innodb_OOM_prepare_inplace_alter",
			error = DB_OUT_OF_MEMORY;
			goto error_handling;);

	/* If a new clustered index is defined for the table we need
	to rebuild the table with a temporary name. */

	if (new_clustered) {
		if (innobase_check_foreigns(
			    ha_alter_info, old_table,
			    user_table, ctx->drop_fk, ctx->num_to_drop_fk)) {
new_clustered_failed:
			DBUG_ASSERT(ctx->trx != ctx->prebuilt->trx);
			ctx->trx->rollback();

			ut_ad(user_table->get_ref_count() == 1);

			if (user_table->drop_aborted) {
				row_mysql_unlock_data_dictionary(ctx->trx);
				trx_start_for_ddl(ctx->trx);
				if (lock_sys_tables(ctx->trx) == DB_SUCCESS) {
					row_mysql_lock_data_dictionary(
						ctx->trx);
					online_retry_drop_indexes_low(
						user_table, ctx->trx);
					commit_unlock_and_unlink(ctx->trx);
				} else {
					ctx->trx->commit();
				}
				row_mysql_lock_data_dictionary(ctx->trx);
			}

			if (ctx->need_rebuild()) {
				if (ctx->new_table) {
					ut_ad(!ctx->new_table->cached);
					dict_mem_table_free(ctx->new_table);
				}
				ctx->new_table = ctx->old_table;
			}

			while (ctx->num_to_add_index--) {
				if (dict_index_t*& i = ctx->add_index[
					    ctx->num_to_add_index]) {
					dict_mem_index_free(i);
					i = NULL;
				}
			}

			goto err_exit;
		}

		size_t	prefixlen= strlen(mysql_data_home);
                if (mysql_data_home[prefixlen-1] != FN_LIBCHAR)
                  prefixlen++;
		size_t	tablen = altered_table->s->path.length - prefixlen;
		const char* part = ctx->old_table->name.part();
		size_t	partlen = part ? strlen(part) : 0;
		char*	new_table_name = static_cast<char*>(
			mem_heap_alloc(ctx->heap, tablen + partlen + 1));
		memcpy(new_table_name,
		       altered_table->s->path.str + prefixlen, tablen);
#ifdef _WIN32
                {
                  char *sep= strchr(new_table_name, FN_LIBCHAR);
                  sep[0]= '/';
                }
#endif
		memcpy(new_table_name + tablen, part ? part : "", partlen + 1);
		ulint		n_cols = 0;
		ulint		n_v_cols = 0;
		dtuple_t*	defaults;
		ulint		z = 0;

		for (uint i = 0; i < altered_table->s->fields; i++) {
			const Field*	field = altered_table->field[i];

			if (!field->stored_in_db()) {
				n_v_cols++;
			} else {
				n_cols++;
			}
		}

		ut_ad(n_cols + n_v_cols == altered_table->s->fields);

		if (add_fts_doc_id) {
			n_cols++;
			DBUG_ASSERT(flags2 & DICT_TF2_FTS);
			DBUG_ASSERT(add_fts_doc_id_idx);
			flags2 |= DICT_TF2_FTS_ADD_DOC_ID
				| DICT_TF2_FTS_HAS_DOC_ID
				| DICT_TF2_FTS;
		}

		DBUG_ASSERT(!add_fts_doc_id_idx || (flags2 & DICT_TF2_FTS));

		ctx->new_table = dict_table_t::create(
			{new_table_name, tablen + partlen}, nullptr,
			n_cols + n_v_cols, n_v_cols, flags, flags2);

		/* The rebuilt indexed_table will use the renamed
		column names. */
		ctx->col_names = NULL;

		if (DICT_TF_HAS_DATA_DIR(flags)) {
			ctx->new_table->data_dir_path =
				mem_heap_strdup(ctx->new_table->heap,
				user_table->data_dir_path);
		}

		for (uint i = 0; i < altered_table->s->fields; i++) {
			const Field*	field = altered_table->field[i];
			unsigned is_unsigned;
			auto col_type = get_innobase_type_from_mysql_type(
				&is_unsigned, field);
			unsigned field_type = field->type() | is_unsigned;
			const bool is_virtual = !field->stored_in_db();

			/* we assume in dtype_form_prtype() that this
			fits in two bytes */
			ut_a(field_type <= MAX_CHAR_COLL_NUM);

			if (!field->real_maybe_null()) {
				field_type |= DATA_NOT_NULL;
			}

			if (field->binary()) {
				field_type |= DATA_BINARY_TYPE;
			}

			if (altered_table->versioned()) {
				if (i == altered_table->s->vers.start_fieldno) {
					field_type |= DATA_VERS_START;
				} else if (i ==
					   altered_table->s->vers.end_fieldno) {
					field_type |= DATA_VERS_END;
				} else if (!(field->flags
					     & VERS_UPDATE_UNVERSIONED_FLAG)) {
					field_type |= DATA_VERSIONED;
				}
			}

			unsigned charset_no;

			if (dtype_is_string_type(col_type)) {
				charset_no = field->charset()->number;

				if (charset_no > MAX_CHAR_COLL_NUM) {
					my_error(ER_WRONG_KEY_COLUMN, MYF(0), "InnoDB",
						 field->field_name.str);
					goto new_clustered_failed;
				}
			} else {
				charset_no = 0;
			}

			auto col_len = field->pack_length();

			/* The MySQL pack length contains 1 or 2 bytes
			length field for a true VARCHAR. Let us
			subtract that, so that the InnoDB column
			length in the InnoDB data dictionary is the
			real maximum byte length of the actual data. */

			if (field->type() == MYSQL_TYPE_VARCHAR) {
				uint32	length_bytes
					= static_cast<const Field_varstring*>(
						field)->length_bytes;

				col_len -= length_bytes;

				if (length_bytes == 2) {
					field_type |= DATA_LONG_TRUE_VARCHAR;
				}

			}

			if (dict_col_name_is_reserved(field->field_name.str)) {
wrong_column_name:
				dict_mem_table_free(ctx->new_table);
				ctx->new_table = ctx->old_table;
				my_error(ER_WRONG_COLUMN_NAME, MYF(0),
					 field->field_name.str);
				goto new_clustered_failed;
			}

			/** Note the FTS_DOC_ID name is case sensitive due
			 to internal query parser.
			 FTS_DOC_ID column must be of BIGINT NOT NULL type
			 and it should be in all capitalized characters */
			if (!innobase_strcasecmp(field->field_name.str,
						 FTS_DOC_ID_COL_NAME)) {
				if (col_type != DATA_INT
				    || field->real_maybe_null()
				    || col_len != sizeof(doc_id_t)
				    || strcmp(field->field_name.str,
					      FTS_DOC_ID_COL_NAME)) {
					goto wrong_column_name;
				}
			}

			if (is_virtual) {
				dict_mem_table_add_v_col(
					ctx->new_table, ctx->heap,
					field->field_name.str,
					col_type,
					dtype_form_prtype(
						field_type, charset_no)
					| DATA_VIRTUAL,
					col_len, i, 0);
			} else {
				dict_mem_table_add_col(
					ctx->new_table, ctx->heap,
					field->field_name.str,
					col_type,
					dtype_form_prtype(
						field_type, charset_no),
					col_len);
			}
		}

		if (n_v_cols) {
			for (uint i = 0; i < altered_table->s->fields; i++) {
				dict_v_col_t*	v_col;
				const Field*	field = altered_table->field[i];

				if (!!field->stored_in_db()) {
					continue;
				}
				v_col = dict_table_get_nth_v_col(
					ctx->new_table, z);
				z++;
				innodb_base_col_setup(
					ctx->new_table, field, v_col);
			}
		}

		if (add_fts_doc_id) {
			fts_add_doc_id_column(ctx->new_table, ctx->heap);
			ctx->new_table->fts->doc_col = fts_doc_id_col;
			ut_ad(fts_doc_id_col
			      == altered_table->s->fields - n_v_cols);
		} else if (ctx->new_table->fts) {
			ctx->new_table->fts->doc_col = fts_doc_id_col;
		}

		dict_table_add_system_columns(ctx->new_table, ctx->heap);

		if (ha_alter_info->handler_flags & INNOBASE_DEFAULTS) {
			defaults = dtuple_create_with_vcol(
				ctx->heap,
				dict_table_get_n_cols(ctx->new_table),
				dict_table_get_n_v_cols(ctx->new_table));

			dict_table_copy_types(defaults, ctx->new_table);
		} else {
			defaults = NULL;
		}

		ctx->col_map = innobase_build_col_map(
			ha_alter_info, altered_table, old_table,
			ctx->new_table, user_table, defaults, ctx->heap);
		ctx->defaults = defaults;
	} else {
		DBUG_ASSERT(!innobase_need_rebuild(ha_alter_info, old_table));
		DBUG_ASSERT(old_table->s->primary_key
			    == altered_table->s->primary_key);

		for (dict_index_t* index
			     = dict_table_get_first_index(user_table);
		     index != NULL;
		     index = dict_table_get_next_index(index)) {
			if (!index->to_be_dropped && index->is_corrupted()) {
				my_error(ER_CHECK_NO_SUCH_TABLE, MYF(0));
				goto error_handled;
			}
		}

		for (dict_index_t* index
			     = dict_table_get_first_index(user_table);
		     index != NULL;
		     index = dict_table_get_next_index(index)) {
			if (!index->to_be_dropped && index->is_corrupted()) {
				my_error(ER_CHECK_NO_SUCH_TABLE, MYF(0));
				goto error_handled;
			}
		}

		if (!ctx->new_table->fts
		    && innobase_fulltext_exist(altered_table)) {
			ctx->new_table->fts = fts_create(
				ctx->new_table);
			ctx->new_table->fts->doc_col = fts_doc_id_col;
		}

		/* Check if we need to update mtypes of legacy GIS columns.
		This check is only needed when we don't have to rebuild
		the table, since rebuild would update all mtypes for GIS
		columns */
		error = innobase_check_gis_columns(
			ha_alter_info, ctx->new_table, ctx->trx);
		if (error != DB_SUCCESS) {
			ut_ad(error == DB_ERROR);
			my_error(ER_TABLE_CANT_HANDLE_SPKEYS, MYF(0), "SYS_COLUMNS");
			goto error_handled;
		}
	}

	ut_ad(new_clustered == ctx->need_rebuild());

	/* Create the index metadata. */
	for (ulint a = 0; a < ctx->num_to_add_index; a++) {
		if (index_defs[a].ind_type & DICT_VIRTUAL
		    && ctx->num_to_drop_vcol > 0 && !new_clustered) {
			innodb_v_adjust_idx_col(ha_alter_info, old_table,
						ctx->num_to_drop_vcol,
						&index_defs[a]);
		}

		ctx->add_index[a] = row_merge_create_index(
			ctx->new_table, &index_defs[a], add_v);

		add_key_nums[a] = index_defs[a].key_number;

		DBUG_ASSERT(ctx->add_index[a]->is_committed()
			    == !!new_clustered);
	}

	DBUG_ASSERT(!ctx->need_rebuild()
		    || !ctx->new_table->persistent_autoinc);

	if (ctx->need_rebuild() && instant_alter_column_possible(
		    *user_table, ha_alter_info, old_table, altered_table,
		    ha_innobase::is_innodb_strict_mode(ctx->trx->mysql_thd))) {
		for (uint a = 0; a < ctx->num_to_add_index; a++) {
			ctx->add_index[a]->table = ctx->new_table;
			error = dict_index_add_to_cache(
				ctx->add_index[a], FIL_NULL, add_v);
			ut_a(error == DB_SUCCESS);
		}

		DBUG_ASSERT(ha_alter_info->key_count
			    /* hidden GEN_CLUST_INDEX in InnoDB */
			    + dict_index_is_auto_gen_clust(
				    dict_table_get_first_index(ctx->new_table))
			    /* hidden FTS_DOC_ID_INDEX in InnoDB */
			    + (ctx->old_table->fts_doc_id_index
			       && innobase_fts_check_doc_id_index_in_def(
				       altered_table->s->keys,
				       altered_table->key_info)
			       != FTS_EXIST_DOC_ID_INDEX)
			    == ctx->num_to_add_index);

		ctx->num_to_add_index = 0;
		ctx->add_index = NULL;

		uint i = 0; // index of stored columns ctx->new_table->cols[]
		Field **af = altered_table->field;

		for (const Create_field& new_field :
		     ha_alter_info->alter_info->create_list) {
			DBUG_ASSERT(!new_field.field
				    || std::find(old_table->field,
						 old_table->field
						 + old_table->s->fields,
						 new_field.field) !=
				    old_table->field + old_table->s->fields);
			DBUG_ASSERT(new_field.field
				    || !strcmp(new_field.field_name.str,
					       (*af)->field_name.str));

			if (!(*af)->stored_in_db()) {
				af++;
				continue;
			}

			dict_col_t* col = dict_table_get_nth_col(
				ctx->new_table, i);
			DBUG_ASSERT(!strcmp((*af)->field_name.str,
				    dict_table_get_col_name(ctx->new_table,
							    i)));
			DBUG_ASSERT(!col->is_added());

			if (new_field.field) {
				/* This is a pre-existing column,
				possibly at a different position. */
			} else if ((*af)->is_real_null()) {
				/* DEFAULT NULL */
				col->def_val.len = UNIV_SQL_NULL;
			} else {
				switch ((*af)->type()) {
				case MYSQL_TYPE_VARCHAR:
					col->def_val.len = reinterpret_cast
						<const Field_varstring*>
						((*af))->get_length();
					col->def_val.data = reinterpret_cast
						<const Field_varstring*>
						((*af))->get_data();
					break;
				case MYSQL_TYPE_GEOMETRY:
				case MYSQL_TYPE_TINY_BLOB:
				case MYSQL_TYPE_MEDIUM_BLOB:
				case MYSQL_TYPE_BLOB:
				case MYSQL_TYPE_LONG_BLOB:
					col->def_val.len = reinterpret_cast
						<const Field_blob*>
						((*af))->get_length();
					col->def_val.data = reinterpret_cast
						<const Field_blob*>
						((*af))->get_ptr();
					break;
				default:
					dfield_t d;
					dict_col_copy_type(col, &d.type);
					ulint len = (*af)->pack_length();
					DBUG_ASSERT(len <= 8
						    || d.type.mtype
						    != DATA_INT);
					row_mysql_store_col_in_innobase_format(
						&d,
						d.type.mtype == DATA_INT
						? static_cast<byte*>(
							mem_heap_alloc(
								ctx->heap,
								len))
						: NULL,
						true, (*af)->ptr, len,
						dict_table_is_comp(
							user_table));
					col->def_val.len = d.len;
					col->def_val.data = d.data;
				}
			}

			i++;
			af++;
		}

		DBUG_ASSERT(af == altered_table->field
			    + altered_table->s->fields);
		/* There might exist a hidden FTS_DOC_ID column for
		FULLTEXT INDEX. If it exists, the columns should have
		been implicitly added by ADD FULLTEXT INDEX together
		with instant ADD COLUMN. (If a hidden FTS_DOC_ID pre-existed,
		then the ctx->col_map[] check should have prevented
		adding visible user columns after that.) */
		DBUG_ASSERT(DATA_N_SYS_COLS + i == ctx->new_table->n_cols
			    || (1 + DATA_N_SYS_COLS + i
				== ctx->new_table->n_cols
				&& !strcmp(dict_table_get_col_name(
						   ctx->new_table, i),
				   FTS_DOC_ID_COL_NAME)));

		if (altered_table->found_next_number_field) {
			ctx->new_table->persistent_autoinc
				= ctx->old_table->persistent_autoinc;
		}

		ctx->prepare_instant();
	}

	if (ctx->need_rebuild()) {
		DBUG_ASSERT(ctx->need_rebuild());
		DBUG_ASSERT(!ctx->is_instant());
		DBUG_ASSERT(num_fts_index <= 1);
		DBUG_ASSERT(!ctx->online || num_fts_index == 0);
		DBUG_ASSERT(!ctx->online
			    || !ha_alter_info->mdl_exclusive_after_prepare
			    || ctx->add_autoinc == ULINT_UNDEFINED);
		DBUG_ASSERT(!ctx->online
			    || !innobase_need_rebuild(ha_alter_info, old_table)
			    || !innobase_fulltext_exist(altered_table));

		uint32_t		key_id	= FIL_DEFAULT_ENCRYPTION_KEY;
		fil_encryption_t	mode	= FIL_ENCRYPTION_DEFAULT;

		if (fil_space_t* s = user_table->space) {
			if (const fil_space_crypt_t* c = s->crypt_data) {
				key_id = c->key_id;
				mode = c->encryption;
			}
		}

		if (ha_alter_info->handler_flags & ALTER_OPTIONS) {
			const ha_table_option_struct& alt_opt=
				*ha_alter_info->create_info->option_struct;
			const ha_table_option_struct& opt=
				*old_table->s->option_struct;
			if (alt_opt.encryption != opt.encryption
			    || alt_opt.encryption_key_id
			    != opt.encryption_key_id) {
				key_id = uint32_t(alt_opt.encryption_key_id);
				mode = fil_encryption_t(alt_opt.encryption);
			}
		}

		if (dict_sys.find_table(
			    {ctx->new_table->name.m_name,
			     strlen(ctx->new_table->name.m_name)})) {
			my_error(ER_TABLE_EXISTS_ERROR, MYF(0),
				 ctx->new_table->name.m_name);
			goto new_clustered_failed;
		}

		/* Create the table. */
		ctx->trx->dict_operation = true;

		error = row_create_table_for_mysql(ctx->new_table, ctx->trx);

		switch (error) {
		case DB_SUCCESS:
			DBUG_ASSERT(ctx->new_table->get_ref_count() == 0);
			DBUG_ASSERT(ctx->new_table->id != 0);
			break;
		case DB_DUPLICATE_KEY:
			my_error(HA_ERR_TABLE_EXIST, MYF(0),
				 altered_table->s->table_name.str);
			goto new_table_failed;
		case DB_UNSUPPORTED:
			my_error(ER_UNSUPPORTED_EXTENSION, MYF(0),
				 altered_table->s->table_name.str);
			goto new_table_failed;
		default:
			my_error_innodb(error, table_name, flags);
new_table_failed:
			DBUG_ASSERT(ctx->trx != ctx->prebuilt->trx);
			ctx->new_table = NULL;
			goto new_clustered_failed;
		}

		for (ulint a = 0; a < ctx->num_to_add_index; a++) {
			dict_index_t* index = ctx->add_index[a];
			const ulint n_v_col = index->get_new_n_vcol();
			index = create_index_dict(ctx->trx, index,
						  mode, key_id, add_v);
			error = ctx->trx->error_state;
			if (error != DB_SUCCESS) {
				if (index) {
					dict_mem_index_free(index);
				}
error_handling_drop_uncached_1:
				while (++a < ctx->num_to_add_index) {
					dict_mem_index_free(ctx->add_index[a]);
				}
				goto error_handling;
			} else {
				DBUG_ASSERT(index != ctx->add_index[a]);
			}

			ctx->add_index[a] = index;
			/* For ALTER TABLE...FORCE or OPTIMIZE TABLE,
			we may only issue warnings, because there will
			be no schema change from the user perspective. */
			if (!info.row_size_is_acceptable(
				    *index,
				    !!(ha_alter_info->handler_flags
				       & ~(INNOBASE_INPLACE_IGNORE
					   | INNOBASE_ALTER_NOVALIDATE
					   | ALTER_RECREATE_TABLE)))) {
				error = DB_TOO_BIG_RECORD;
				goto error_handling_drop_uncached_1;
			}
			index->parser = index_defs[a].parser;
			if (n_v_col) {
				index->assign_new_v_col(n_v_col);
			}
			/* Note the id of the transaction that created this
			index, we use it to restrict readers from accessing
			this index, to ensure read consistency. */
			ut_ad(index->trx_id == ctx->trx->id);

			if (index->type & DICT_FTS) {
				DBUG_ASSERT(num_fts_index == 1);
				DBUG_ASSERT(!fts_index);
				DBUG_ASSERT(index->type == DICT_FTS);
				fts_index = ctx->add_index[a];
			}
		}

		dict_index_t*	clust_index = dict_table_get_first_index(
			user_table);
		dict_index_t*	new_clust_index = dict_table_get_first_index(
			ctx->new_table);
		ut_ad(!new_clust_index->is_instant());
		/* row_merge_build_index() depends on the correct value */
		ut_ad(new_clust_index->n_core_null_bytes
		      == UT_BITS_IN_BYTES(new_clust_index->n_nullable));

		if (const Field* ai = altered_table->found_next_number_field) {
			const unsigned	col_no = innodb_col_no(ai);

			ctx->new_table->persistent_autoinc =
				(dict_table_get_nth_col_pos(
					ctx->new_table, col_no, NULL) + 1)
				& dict_index_t::MAX_N_FIELDS;

			/* Initialize the AUTO_INCREMENT sequence
			to the rebuilt table from the old one. */
			if (!old_table->found_next_number_field
			    || !user_table->space) {
			} else if (ib_uint64_t autoinc
				   = btr_read_autoinc(clust_index)) {
				btr_write_autoinc(new_clust_index, autoinc);
			}
		}

		ctx->skip_pk_sort = innobase_pk_order_preserved(
			ctx->col_map, clust_index, new_clust_index);

		DBUG_EXECUTE_IF("innodb_alter_table_pk_assert_no_sort",
			DBUG_ASSERT(ctx->skip_pk_sort););

		if (ctx->online) {
			/* Allocate a log for online table rebuild. */
			clust_index->lock.x_lock(SRW_LOCK_CALL);
			bool ok = row_log_allocate(
				ctx->prebuilt->trx,
				clust_index, ctx->new_table,
				!(ha_alter_info->handler_flags
				  & ALTER_ADD_PK_INDEX),
				ctx->defaults, ctx->col_map, path,
				old_table,
				ctx->allow_not_null);
			clust_index->lock.x_unlock();

			if (!ok) {
				error = DB_OUT_OF_MEMORY;
				goto error_handling;
			}
		}
	} else if (ctx->num_to_add_index) {
		ut_ad(!ctx->is_instant());

		for (ulint a = 0; a < ctx->num_to_add_index; a++) {
			dict_index_t* index = ctx->add_index[a];
			const ulint n_v_col = index->get_new_n_vcol();
			DBUG_EXECUTE_IF(
				"create_index_metadata_fail",
				if (a + 1 == ctx->num_to_add_index) {
					ctx->trx->error_state =
						DB_OUT_OF_FILE_SPACE;
					goto index_created;
				});
			index = create_index_dict(ctx->trx, index,
						  FIL_ENCRYPTION_DEFAULT,
						  FIL_DEFAULT_ENCRYPTION_KEY,
						  add_v);
#ifndef DBUG_OFF
index_created:
#endif
			error = ctx->trx->error_state;
			if (error != DB_SUCCESS) {
				if (index) {
					dict_mem_index_free(index);
				}
error_handling_drop_uncached:
				while (++a < ctx->num_to_add_index) {
					dict_mem_index_free(ctx->add_index[a]);
				}
				goto error_handling;
			} else {
				DBUG_ASSERT(index != ctx->add_index[a]);
			}
			ctx->add_index[a]= index;
			if (!info.row_size_is_acceptable(*index, true)) {
				error = DB_TOO_BIG_RECORD;
				goto error_handling_drop_uncached;
			}

			index->parser = index_defs[a].parser;
			if (n_v_col) {
				index->assign_new_v_col(n_v_col);
			}
			/* Note the id of the transaction that created this
			index, we use it to restrict readers from accessing
			this index, to ensure read consistency. */
			ut_ad(index->trx_id == ctx->trx->id);

			/* If ADD INDEX with LOCK=NONE has been
			requested, allocate a modification log. */
			if (index->type & DICT_FTS) {
				DBUG_ASSERT(num_fts_index == 1);
				DBUG_ASSERT(!fts_index);
				DBUG_ASSERT(index->type == DICT_FTS);
				fts_index = ctx->add_index[a];
				/* Fulltext indexes are not covered
				by a modification log. */
			} else if (!ctx->online
				   || !user_table->is_readable()
				   || !user_table->space) {
				/* No need to allocate a modification log. */
				DBUG_ASSERT(!index->online_log);
			} else {
				index->lock.x_lock(SRW_LOCK_CALL);

				bool ok = row_log_allocate(
					ctx->prebuilt->trx,
					index,
					NULL, true, NULL, NULL,
					path, old_table,
					ctx->allow_not_null);

				index->lock.x_unlock();

				DBUG_EXECUTE_IF(
					"innodb_OOM_prepare_add_index",
					if (ok && a == 1) {
						row_log_free(
							index->online_log);
						index->online_log = NULL;
						ctx->old_table->indexes.start
							->online_log = nullptr;
						ok = false;
					});

				if (!ok) {
					error = DB_OUT_OF_MEMORY;
					goto error_handling_drop_uncached;
				}
			}
		}
	} else if (ctx->is_instant()
		   && !info.row_size_is_acceptable(*user_table, true)) {
		error = DB_TOO_BIG_RECORD;
		goto error_handling;
	}

	if (ctx->online && ctx->num_to_add_index) {
		/* Assign a consistent read view for
		row_merge_read_clustered_index(). */
		ctx->prebuilt->trx->read_view.open(ctx->prebuilt->trx);
	}

	if (fts_index) {
		ut_ad(ctx->trx->dict_operation);
		ut_ad(ctx->trx->dict_operation_lock_mode);
		ut_ad(dict_sys.locked());

		DICT_TF2_FLAG_SET(ctx->new_table, DICT_TF2_FTS);
		if (ctx->need_rebuild()) {
			/* For !ctx->need_rebuild(), this will be set at
			commit_cache_norebuild(). */
			ctx->new_table->fts_doc_id_index
				= dict_table_get_index_on_name(
					ctx->new_table, FTS_DOC_ID_INDEX_NAME);
			DBUG_ASSERT(ctx->new_table->fts_doc_id_index != NULL);
		}

		error = fts_create_index_tables(ctx->trx, fts_index,
						ctx->new_table->id);

		DBUG_EXECUTE_IF("innodb_test_fail_after_fts_index_table",
				error = DB_LOCK_WAIT_TIMEOUT;
				goto error_handling;);

		if (error != DB_SUCCESS) {
			goto error_handling;
		}

		if (!ctx->new_table->fts
		    || ib_vector_size(ctx->new_table->fts->indexes) == 0) {
			error = fts_create_common_tables(
				ctx->trx, ctx->new_table, true);

			DBUG_EXECUTE_IF(
				"innodb_test_fail_after_fts_common_table",
				error = DB_LOCK_WAIT_TIMEOUT;);

			if (error != DB_SUCCESS) {
				goto error_handling;
			}

			ctx->new_table->fts->dict_locked = true;

			error = innobase_fts_load_stopword(
				ctx->new_table, ctx->trx,
				ctx->prebuilt->trx->mysql_thd)
				? DB_SUCCESS : DB_ERROR;
			ctx->new_table->fts->dict_locked = false;

			if (error != DB_SUCCESS) {
				goto error_handling;
			}
		}
	}

	DBUG_ASSERT(error == DB_SUCCESS);

	{
		/* Commit the data dictionary transaction in order to release
		the table locks on the system tables.  This means that if
		MariaDB is killed while rebuilding the table inside
		row_merge_build_indexes(), ctx->new_table will not be dropped
		by trx_rollback_active(). */
		ut_d(dict_table_check_for_dup_indexes(user_table,
						      CHECK_PARTIAL_OK));
		if (ctx->need_rebuild()) {
			ctx->new_table->acquire();
		}

		/* fts_create_common_tables() may drop old common tables,
		whose files would be deleted here. */
		commit_unlock_and_unlink(ctx->trx);
		if (fts_exist) {
			purge_sys.resume_FTS();
		}

		trx_start_for_ddl(ctx->trx);
		ctx->prebuilt->trx_id = ctx->trx->id;
	}

	if (ctx->old_table->fts) {
		fts_sync_during_ddl(ctx->old_table);
	}

	DBUG_RETURN(false);

error_handling:
	/* After an error, remove all those index definitions from the
	dictionary which were defined. */

	switch (error) {
	case DB_TABLESPACE_EXISTS:
		my_error(ER_TABLESPACE_EXISTS, MYF(0), "(unknown)");
		break;
	case DB_DUPLICATE_KEY:
		my_error(ER_DUP_KEY, MYF(0), "SYS_INDEXES");
		break;
	default:
		my_error_innodb(error, table_name, user_table->flags);
	}

	ctx->trx->rollback();

	ut_ad(!ctx->need_rebuild()
	      || !user_table->indexes.start->online_log);

	ctx->prebuilt->trx->error_info = NULL;
	ctx->trx->error_state = DB_SUCCESS;

	if (false) {
error_handled:
		ut_ad(!table_lock_failed);
		ut_ad(ctx->trx->state == TRX_STATE_ACTIVE);
		ut_ad(!ctx->trx->undo_no);
		ut_ad(dict_locked);
	} else if (table_lock_failed) {
		if (!dict_locked) {
			row_mysql_lock_data_dictionary(ctx->trx);
		}
		goto err_exit;
	} else {
		ut_ad(ctx->trx->state == TRX_STATE_NOT_STARTED);
		if (new_clustered && !user_table->drop_aborted) {
			goto err_exit;
		}
		if (dict_locked) {
			row_mysql_unlock_data_dictionary(ctx->trx);
		}
		trx_start_for_ddl(ctx->trx);
		dberr_t err= lock_sys_tables(ctx->trx);
		row_mysql_lock_data_dictionary(ctx->trx);
		if (err != DB_SUCCESS) {
			goto err_exit;
		}
	}

	/* n_ref_count must be 1, because background threads cannot
	be executing on this very table as we are
	holding MDL_EXCLUSIVE. */
	ut_ad(ctx->online || user_table->get_ref_count() == 1);

	if (new_clustered) {
		online_retry_drop_indexes_low(user_table, ctx->trx);
		commit_unlock_and_unlink(ctx->trx);
		row_mysql_lock_data_dictionary(ctx->trx);
	} else {
		row_merge_drop_indexes(ctx->trx, user_table, true);
		ctx->trx->commit();
	}

	ut_d(dict_table_check_for_dup_indexes(user_table, CHECK_ALL_COMPLETE));
	ut_ad(!user_table->drop_aborted);

err_exit:
	/* Clear the to_be_dropped flag in the data dictionary cache. */
	for (ulint i = 0; i < ctx->num_to_drop_index; i++) {
		DBUG_ASSERT(ctx->drop_index[i]->is_committed());
		DBUG_ASSERT(ctx->drop_index[i]->to_be_dropped);
		ctx->drop_index[i]->to_be_dropped = 0;
	}

	if (ctx->trx) {
		row_mysql_unlock_data_dictionary(ctx->trx);
		ctx->trx->rollback();
		ctx->trx->free();
	}
	trx_commit_for_mysql(ctx->prebuilt->trx);
	if (fts_exist) {
		purge_sys.resume_FTS();
	}

	for (uint i = 0; i < ctx->num_to_add_fk; i++) {
		if (ctx->add_fk[i]) {
			dict_foreign_free(ctx->add_fk[i]);
		}
	}

	delete ctx;
	ha_alter_info->handler_ctx = NULL;

	DBUG_RETURN(true);
}

/* Check whether an index is needed for the foreign key constraint.
If so, if it is dropped, is there an equivalent index can play its role.
@return true if the index is needed and can't be dropped */
static MY_ATTRIBUTE((nonnull(1,2,3,5), warn_unused_result))
bool
innobase_check_foreign_key_index(
/*=============================*/
	Alter_inplace_info*	ha_alter_info,	/*!< in: Structure describing
						changes to be done by ALTER
						TABLE */
	dict_index_t*		index,		/*!< in: index to check */
	dict_table_t*		indexed_table,	/*!< in: table that owns the
						foreign keys */
	const char**		col_names,	/*!< in: column names, or NULL
						for indexed_table->col_names */
	trx_t*			trx,		/*!< in/out: transaction */
	dict_foreign_t**	drop_fk,	/*!< in: Foreign key constraints
						to drop */
	ulint			n_drop_fk)	/*!< in: Number of foreign keys
						to drop */
{
	const dict_foreign_set*	fks = &indexed_table->referenced_set;

	/* Check for all FK references from other tables to the index. */
	for (dict_foreign_set::const_iterator it = fks->begin();
	     it != fks->end(); ++it) {

		dict_foreign_t*	foreign = *it;
		if (foreign->referenced_index != index) {
			continue;
		}
		ut_ad(indexed_table == foreign->referenced_table);

		if (NULL == dict_foreign_find_index(
			    indexed_table, col_names,
			    foreign->referenced_col_names,
			    foreign->n_fields, index,
			    /*check_charsets=*/TRUE,
			    /*check_null=*/FALSE,
			    NULL, NULL, NULL)
		    && NULL == innobase_find_equiv_index(
			    foreign->referenced_col_names,
			    foreign->n_fields,
			    ha_alter_info->key_info_buffer,
			    span<uint>(ha_alter_info->index_add_buffer,
				       ha_alter_info->index_add_count))) {

			/* Index cannot be dropped. */
			trx->error_info = index;
			return(true);
		}
	}

	fks = &indexed_table->foreign_set;

	/* Check for all FK references in current table using the index. */
	for (dict_foreign_set::const_iterator it = fks->begin();
	     it != fks->end(); ++it) {

		dict_foreign_t*	foreign = *it;
		if (foreign->foreign_index != index) {
			continue;
		}

		ut_ad(indexed_table == foreign->foreign_table);

		if (!innobase_dropping_foreign(
			    foreign, drop_fk, n_drop_fk)
		    && NULL == dict_foreign_find_index(
			    indexed_table, col_names,
			    foreign->foreign_col_names,
			    foreign->n_fields, index,
			    /*check_charsets=*/TRUE,
			    /*check_null=*/FALSE,
			    NULL, NULL, NULL)
		    && NULL == innobase_find_equiv_index(
			    foreign->foreign_col_names,
			    foreign->n_fields,
			    ha_alter_info->key_info_buffer,
			    span<uint>(ha_alter_info->index_add_buffer,
				       ha_alter_info->index_add_count))) {

			/* Index cannot be dropped. */
			trx->error_info = index;
			return(true);
		}
	}

	return(false);
}

/**
Rename a given index in the InnoDB data dictionary.

@param index index to rename
@param new_name new name of the index
@param[in,out] trx dict transaction to use, not going to be committed here

@retval true Failure
@retval false Success */
static MY_ATTRIBUTE((warn_unused_result))
bool
rename_index_try(
	const dict_index_t*	index,
	const char*		new_name,
	trx_t*			trx)
{
	DBUG_ENTER("rename_index_try");
	ut_ad(dict_sys.locked());
	ut_ad(trx->dict_operation_lock_mode);

	pars_info_t*	pinfo;
	dberr_t		err;

	pinfo = pars_info_create();

	pars_info_add_ull_literal(pinfo, "table_id", index->table->id);
	pars_info_add_ull_literal(pinfo, "index_id", index->id);
	pars_info_add_str_literal(pinfo, "new_name", new_name);

	trx->op_info = "Renaming an index in SYS_INDEXES";

	DBUG_EXECUTE_IF(
		"ib_rename_index_fail1",
		DBUG_SET("+d,innodb_report_deadlock");
	);

	err = que_eval_sql(
		pinfo,
		"PROCEDURE RENAME_INDEX_IN_SYS_INDEXES () IS\n"
		"BEGIN\n"
		"UPDATE SYS_INDEXES SET\n"
		"NAME = :new_name\n"
		"WHERE\n"
		"ID = :index_id AND\n"
		"TABLE_ID = :table_id;\n"
		"END;\n", trx); /* pinfo is freed by que_eval_sql() */

	DBUG_EXECUTE_IF(
		"ib_rename_index_fail1",
		DBUG_SET("-d,innodb_report_deadlock");
	);

	trx->op_info = "";

	if (err != DB_SUCCESS) {
		my_error_innodb(err, index->table->name.m_name, 0);
		DBUG_RETURN(true);
	}

	DBUG_RETURN(false);
}


/**
Rename a given index in the InnoDB data dictionary cache.

@param[in,out] index index to rename
@param new_name new index name
*/
static
void
innobase_rename_index_cache(dict_index_t* index, const char* new_name)
{
	DBUG_ENTER("innobase_rename_index_cache");
	ut_ad(dict_sys.locked());

	size_t	old_name_len = strlen(index->name);
	size_t	new_name_len = strlen(new_name);

	if (old_name_len < new_name_len) {
		index->name = static_cast<char*>(
		    mem_heap_alloc(index->heap, new_name_len + 1));
	}

	memcpy(const_cast<char*>(index->name()), new_name, new_name_len + 1);

	DBUG_VOID_RETURN;
}


/** Rename the index name in cache.
@param[in]	ctx		alter context
@param[in]	ha_alter_info	Data used during inplace alter. */
static void
innobase_rename_indexes_cache(const ha_innobase_inplace_ctx *ctx,
                              const Alter_inplace_info *ha_alter_info)
{
  DBUG_ASSERT(ha_alter_info->handler_flags & ALTER_RENAME_INDEX);

  std::vector<std::pair<dict_index_t *, const char *>> rename_info;
  rename_info.reserve(ha_alter_info->rename_keys.size());

  for (const Alter_inplace_info::Rename_key_pair &pair :
       ha_alter_info->rename_keys)
  {
    dict_index_t *index=
        dict_table_get_index_on_name(ctx->old_table, pair.old_key->name.str);
    ut_ad(index);

    rename_info.emplace_back(index, pair.new_key->name.str);
  }

  for (const auto &pair : rename_info)
    innobase_rename_index_cache(pair.first, pair.second);
}

/** Fill the stored column information in s_cols list.
@param[in]	altered_table	mysql table object
@param[in]	table		innodb table object
@param[out]	s_cols		list of stored column
@param[out]	s_heap		heap for storing stored
column information. */
static
void
alter_fill_stored_column(
	const TABLE*		altered_table,
	dict_table_t*		table,
	dict_s_col_list**	s_cols,
	mem_heap_t**		s_heap)
{
	ulint	n_cols = altered_table->s->fields;
	ulint	stored_col_no = 0;

	for (ulint i = 0; i < n_cols; i++) {
		Field* field = altered_table->field[i];
		dict_s_col_t	s_col;

		if (field->stored_in_db()) {
			stored_col_no++;
		}

		if (!innobase_is_s_fld(field)) {
			continue;
		}

		ulint	num_base = 0;
		dict_col_t*	col = dict_table_get_nth_col(table,
							     stored_col_no);

		s_col.m_col = col;
		s_col.s_pos = i;

		if (*s_cols == NULL) {
			*s_cols = UT_NEW_NOKEY(dict_s_col_list());
			*s_heap = mem_heap_create(1000);
		}

		if (num_base != 0) {
			s_col.base_col = static_cast<dict_col_t**>(mem_heap_zalloc(
						*s_heap, num_base * sizeof(dict_col_t*)));
		} else {
			s_col.base_col = NULL;
		}

		s_col.num_base = num_base;
		innodb_base_col_setup_for_stored(table, field, &s_col);
		(*s_cols)->push_front(s_col);
	}
}

static bool alter_templ_needs_rebuild(const TABLE* altered_table,
                                      const Alter_inplace_info* ha_alter_info,
                                      const dict_table_t* table);


/** Allows InnoDB to update internal structures with concurrent
writes blocked (provided that check_if_supported_inplace_alter()
did not return HA_ALTER_INPLACE_NO_LOCK).
This will be invoked before inplace_alter_table().

@param altered_table TABLE object for new version of table.
@param ha_alter_info Structure describing changes to be done
by ALTER TABLE and holding data used during in-place alter.

@retval true Failure
@retval false Success
*/

bool
ha_innobase::prepare_inplace_alter_table(
/*=====================================*/
	TABLE*			altered_table,
	Alter_inplace_info*	ha_alter_info)
{
	dict_index_t**	drop_index;	/*!< Index to be dropped */
	ulint		n_drop_index;	/*!< Number of indexes to drop */
	dict_foreign_t**drop_fk;	/*!< Foreign key constraints to drop */
	ulint		n_drop_fk;	/*!< Number of foreign keys to drop */
	dict_foreign_t**add_fk = NULL;	/*!< Foreign key constraints to drop */
	ulint		n_add_fk;	/*!< Number of foreign keys to drop */
	dict_table_t*	indexed_table;	/*!< Table where indexes are created */
	mem_heap_t*	heap;
	const char**	col_names;
	int		error;
	ulint		add_autoinc_col_no	= ULINT_UNDEFINED;
	ulonglong	autoinc_col_max_value	= 0;
	ulint		fts_doc_col_no		= ULINT_UNDEFINED;
	bool		add_fts_doc_id		= false;
	bool		add_fts_doc_id_idx	= false;
	bool		add_fts_idx		= false;
	dict_s_col_list*s_cols			= NULL;
	mem_heap_t*	s_heap			= NULL;

	DBUG_ENTER("prepare_inplace_alter_table");
	DBUG_ASSERT(!ha_alter_info->handler_ctx);
	DBUG_ASSERT(ha_alter_info->create_info);
	DBUG_ASSERT(!srv_read_only_mode);

	/* Init online ddl status variables */
	onlineddl_rowlog_rows = 0;
	onlineddl_rowlog_pct_used = 0;
	onlineddl_pct_progress = 0;

	MONITOR_ATOMIC_INC(MONITOR_PENDING_ALTER_TABLE);

#ifdef UNIV_DEBUG
	for (dict_index_t* index = dict_table_get_first_index(m_prebuilt->table);
	     index;
	     index = dict_table_get_next_index(index)) {
		ut_ad(!index->to_be_dropped);
	}
#endif /* UNIV_DEBUG */

	ut_d(dict_sys.freeze(SRW_LOCK_CALL));
	ut_d(dict_table_check_for_dup_indexes(
		     m_prebuilt->table, CHECK_ABORTED_OK));
	ut_d(dict_sys.unfreeze());

	if (!(ha_alter_info->handler_flags & ~INNOBASE_INPLACE_IGNORE)) {
		/* Nothing to do */
		DBUG_ASSERT(!m_prebuilt->trx->dict_operation_lock_mode);
		DBUG_RETURN(false);
	}

#ifdef WITH_PARTITION_STORAGE_ENGINE
	if (table->part_info == NULL) {
#endif
	/* Ignore the MDL downgrade when table is empty.
	This optimization is disabled for partition table. */
	ha_alter_info->mdl_exclusive_after_prepare =
		innobase_table_is_empty(m_prebuilt->table, false);
	if (ha_alter_info->online
	    && ha_alter_info->mdl_exclusive_after_prepare) {
		ha_alter_info->online = false;
	}
#ifdef WITH_PARTITION_STORAGE_ENGINE
	}
#endif
	indexed_table = m_prebuilt->table;

	/* ALTER TABLE will not implicitly move a table from a single-table
	tablespace to the system tablespace when innodb_file_per_table=OFF.
	But it will implicitly move a table from the system tablespace to a
	single-table tablespace if innodb_file_per_table = ON. */

	create_table_info_t	info(m_user_thd,
				     altered_table,
				     ha_alter_info->create_info,
				     NULL,
				     NULL,
				     srv_file_per_table);

	info.set_tablespace_type(indexed_table->space != fil_system.sys_space);

	if (ha_alter_info->handler_flags & ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX) {
		if (info.gcols_in_fulltext_or_spatial()) {
			goto err_exit_no_heap;
		}
	}

	if (indexed_table->is_readable()) {
	} else {
		if (indexed_table->corrupted) {
			/* Handled below */
		} else {
			if (const fil_space_t* space = indexed_table->space) {
				String str;
				const char* engine= table_type();

				push_warning_printf(
					m_user_thd,
					Sql_condition::WARN_LEVEL_WARN,
					HA_ERR_DECRYPTION_FAILED,
					"Table %s in file %s is encrypted but encryption service or"
					" used key_id is not available. "
					" Can't continue reading table.",
					table_share->table_name.str,
					space->chain.start->name);

				my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_DECRYPTION_FAILED, str.c_ptr(), engine);
				DBUG_RETURN(true);
			}
		}
	}

	if (indexed_table->corrupted
	    || dict_table_get_first_index(indexed_table) == NULL
	    || dict_table_get_first_index(indexed_table)->is_corrupted()) {
		/* The clustered index is corrupted. */
		my_error(ER_CHECK_NO_SUCH_TABLE, MYF(0));
		DBUG_RETURN(true);
	} else {
		const char* invalid_opt = info.create_options_are_invalid();

		/* Check engine specific table options */
		if (const char* invalid_tbopt = info.check_table_options()) {
			my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0),
				 table_type(), invalid_tbopt);
			goto err_exit_no_heap;
		}

		if (invalid_opt) {
			my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0),
				 table_type(), invalid_opt);
			goto err_exit_no_heap;
		}
	}

	/* Check if any index name is reserved. */
	if (innobase_index_name_is_reserved(
		    m_user_thd,
		    ha_alter_info->key_info_buffer,
		    ha_alter_info->key_count)) {
err_exit_no_heap:
		DBUG_ASSERT(!m_prebuilt->trx->dict_operation_lock_mode);
		online_retry_drop_indexes(m_prebuilt->table, m_user_thd);
		DBUG_RETURN(true);
	}

	indexed_table = m_prebuilt->table;

	/* Check that index keys are sensible */
	error = innobase_check_index_keys(ha_alter_info, indexed_table);

	if (error) {
		goto err_exit_no_heap;
	}

	/* Prohibit renaming a column to something that the table
	already contains. */
	if (ha_alter_info->handler_flags
	    & ALTER_COLUMN_NAME) {
		for (Field** fp = table->field; *fp; fp++) {
			if (!((*fp)->flags & FIELD_IS_RENAMED)) {
				continue;
			}

			const char* name = 0;

			for (const Create_field& cf :
			     ha_alter_info->alter_info->create_list) {
				if (cf.field == *fp) {
					name = cf.field_name.str;
					goto check_if_ok_to_rename;
				}
			}

			ut_error;
check_if_ok_to_rename:
			/* Prohibit renaming a column from FTS_DOC_ID
			if full-text indexes exist. */
			if (!my_strcasecmp(system_charset_info,
					   (*fp)->field_name.str,
					   FTS_DOC_ID_COL_NAME)
			    && innobase_fulltext_exist(altered_table)) {
				my_error(ER_INNODB_FT_WRONG_DOCID_COLUMN,
					 MYF(0), name);
				goto err_exit_no_heap;
			}

			/* Prohibit renaming a column to an internal column. */
			const char*	s = m_prebuilt->table->col_names;
			unsigned j;
			/* Skip user columns.
			MySQL should have checked these already.
			We want to allow renaming of c1 to c2, c2 to c1. */
			for (j = 0; j < table->s->fields; j++) {
				if (table->field[j]->stored_in_db()) {
					s += strlen(s) + 1;
				}
			}

			for (; j < m_prebuilt->table->n_def; j++) {
				if (!my_strcasecmp(
					    system_charset_info, name, s)) {
					my_error(ER_WRONG_COLUMN_NAME, MYF(0),
						 s);
					goto err_exit_no_heap;
				}

				s += strlen(s) + 1;
			}
		}
	}

	if (!info.innobase_table_flags()) {
		my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0),
			 table_type(), "PAGE_COMPRESSED");
		goto err_exit_no_heap;
	}

	if (info.flags2() & DICT_TF2_USE_FILE_PER_TABLE) {
		/* Preserve the DATA DIRECTORY attribute, because it
		currently cannot be changed during ALTER TABLE. */
		info.flags_set(m_prebuilt->table->flags
			       & 1U << DICT_TF_POS_DATA_DIR);
	}


	/* ALGORITHM=INPLACE without rebuild (10.3+ ALGORITHM=NOCOPY)
	must use the current ROW_FORMAT of the table. */
	const ulint max_col_len = DICT_MAX_FIELD_LEN_BY_FORMAT_FLAG(
		innobase_need_rebuild(ha_alter_info, this->table)
		? info.flags()
		: m_prebuilt->table->flags);

	/* Check each index's column length to make sure they do not
	exceed limit */
	for (ulint i = 0; i < ha_alter_info->key_count; i++) {
		const KEY* key = &ha_alter_info->key_info_buffer[i];

		if (key->flags & HA_FULLTEXT) {
			/* The column length does not matter for
			fulltext search indexes. But, UNIQUE
			fulltext indexes are not supported. */
			DBUG_ASSERT(!(key->flags & HA_NOSAME));
			DBUG_ASSERT(!(key->flags & HA_KEYFLAG_MASK
				      & ~(HA_FULLTEXT
					  | HA_PACK_KEY
					  | HA_BINARY_PACK_KEY)));
			add_fts_idx = true;
			continue;
		}

		if (too_big_key_part_length(max_col_len, *key)) {
			my_error(ER_INDEX_COLUMN_TOO_LONG, MYF(0),
				 max_col_len);
			goto err_exit_no_heap;
		}
	}

	/* We won't be allowed to add fts index to a table with
	fts indexes already but without AUX_HEX_NAME set.
	This means the aux tables of the table failed to
	rename to hex format but new created aux tables
	shall be in hex format, which is contradictory. */
	if (!DICT_TF2_FLAG_IS_SET(indexed_table, DICT_TF2_FTS_AUX_HEX_NAME)
	    && indexed_table->fts != NULL && add_fts_idx) {
		my_error(ER_INNODB_FT_AUX_NOT_HEX_ID, MYF(0));
		goto err_exit_no_heap;
	}

	/* Check existing index definitions for too-long column
	prefixes as well, in case max_col_len shrunk. */
	for (const dict_index_t* index
		     = dict_table_get_first_index(indexed_table);
	     index;
	     index = dict_table_get_next_index(index)) {
		if (index->type & DICT_FTS) {
			DBUG_ASSERT(index->type == DICT_FTS
				    || (index->type & DICT_CORRUPT));

			/* We need to drop any corrupted fts indexes
			before we add a new fts index. */
			if (add_fts_idx && index->type & DICT_CORRUPT) {
				ib_errf(m_user_thd, IB_LOG_LEVEL_ERROR,
					ER_INNODB_INDEX_CORRUPT,
					"Fulltext index '%s' is corrupt. "
					"you should drop this index first.",
					index->name());

				goto err_exit_no_heap;
			}

			continue;
		}

		for (ulint i = 0; i < dict_index_get_n_fields(index); i++) {
			const dict_field_t* field
				= dict_index_get_nth_field(index, i);
			if (field->prefix_len > max_col_len) {
				my_error(ER_INDEX_COLUMN_TOO_LONG, MYF(0),
					 max_col_len);
				goto err_exit_no_heap;
			}
		}
	}

	n_drop_index = 0;
	n_drop_fk = 0;

	if (ha_alter_info->handler_flags
	    & (INNOBASE_ALTER_NOREBUILD | INNOBASE_ALTER_REBUILD
	       | INNOBASE_ALTER_INSTANT)) {
		heap = mem_heap_create(1024);

		if (ha_alter_info->handler_flags
		    & ALTER_COLUMN_NAME) {
			col_names = innobase_get_col_names(
				ha_alter_info, altered_table, table,
				indexed_table, heap);
		} else {
			col_names = NULL;
		}
	} else {
		heap = NULL;
		col_names = NULL;
	}

	if (ha_alter_info->handler_flags
	    & ALTER_DROP_FOREIGN_KEY) {
		DBUG_ASSERT(ha_alter_info->alter_info->drop_list.elements > 0);

		drop_fk = static_cast<dict_foreign_t**>(
			mem_heap_alloc(
				heap,
				ha_alter_info->alter_info->drop_list.elements
				* sizeof(dict_foreign_t*)));

		for (Alter_drop& drop : ha_alter_info->alter_info->drop_list) {
			if (drop.type != Alter_drop::FOREIGN_KEY) {
				continue;
			}

			dict_foreign_t* foreign;

			for (dict_foreign_set::iterator it
				= m_prebuilt->table->foreign_set.begin();
			     it != m_prebuilt->table->foreign_set.end();
			     ++it) {

				foreign = *it;
				const char* fid = strchr(foreign->id, '/');

				DBUG_ASSERT(fid);
				/* If no database/ prefix was present in
				the FOREIGN KEY constraint name, compare
				to the full constraint name. */
				fid = fid ? fid + 1 : foreign->id;

				if (!my_strcasecmp(system_charset_info,
						   fid, drop.name)) {
					goto found_fk;
				}
			}

			my_error(ER_CANT_DROP_FIELD_OR_KEY, MYF(0),
				drop.type_name(), drop.name);
			goto err_exit;
found_fk:
			for (ulint i = n_drop_fk; i--; ) {
				if (drop_fk[i] == foreign) {
					goto dup_fk;
				}
			}
			drop_fk[n_drop_fk++] = foreign;
dup_fk:
			continue;
		}

		DBUG_ASSERT(n_drop_fk > 0);

		DBUG_ASSERT(n_drop_fk
			    <= ha_alter_info->alter_info->drop_list.elements);
	} else {
		drop_fk = NULL;
	}

	if (ha_alter_info->index_drop_count) {
		dict_index_t*	drop_primary = NULL;

		DBUG_ASSERT(ha_alter_info->handler_flags
			    & (ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX
			       | ALTER_DROP_UNIQUE_INDEX
			       | ALTER_DROP_PK_INDEX));
		/* Check which indexes to drop. */
		drop_index = static_cast<dict_index_t**>(
			mem_heap_alloc(
				heap, (ha_alter_info->index_drop_count + 1)
				* sizeof *drop_index));

		for (uint i = 0; i < ha_alter_info->index_drop_count; i++) {
			const KEY*	key
				= ha_alter_info->index_drop_buffer[i];
			dict_index_t*	index
				= dict_table_get_index_on_name(
					indexed_table, key->name.str);

			if (!index) {
				push_warning_printf(
					m_user_thd,
					Sql_condition::WARN_LEVEL_WARN,
					HA_ERR_WRONG_INDEX,
					"InnoDB could not find key"
					" with name %s", key->name.str);
			} else {
				ut_ad(!index->to_be_dropped);
				if (!index->is_primary()) {
					drop_index[n_drop_index++] = index;
				} else {
					drop_primary = index;
				}
			}
		}

		/* If all FULLTEXT indexes were removed, drop an
		internal FTS_DOC_ID_INDEX as well, unless it exists in
		the table. */

		if (innobase_fulltext_exist(table)
		    && !innobase_fulltext_exist(altered_table)
		    && !DICT_TF2_FLAG_IS_SET(
			indexed_table, DICT_TF2_FTS_HAS_DOC_ID)) {
			dict_index_t*	fts_doc_index
				= indexed_table->fts_doc_id_index;
			ut_ad(fts_doc_index);

			// Add some fault tolerance for non-debug builds.
			if (fts_doc_index == NULL) {
				goto check_if_can_drop_indexes;
			}

			DBUG_ASSERT(!fts_doc_index->to_be_dropped);

			for (uint i = 0; i < table->s->keys; i++) {
				if (!my_strcasecmp(
					    system_charset_info,
					    FTS_DOC_ID_INDEX_NAME,
					    table->key_info[i].name.str)) {
					/* The index exists in the MySQL
					data dictionary. Do not drop it,
					even though it is no longer needed
					by InnoDB fulltext search. */
					goto check_if_can_drop_indexes;
				}
			}

			drop_index[n_drop_index++] = fts_doc_index;
		}

check_if_can_drop_indexes:
		/* Check if the indexes can be dropped. */

		/* Prevent a race condition between DROP INDEX and
		CREATE TABLE adding FOREIGN KEY constraints. */
		row_mysql_lock_data_dictionary(m_prebuilt->trx);

		if (!n_drop_index) {
			drop_index = NULL;
		} else {
			/* Flag all indexes that are to be dropped. */
			for (ulint i = 0; i < n_drop_index; i++) {
				ut_ad(!drop_index[i]->to_be_dropped);
				drop_index[i]->to_be_dropped = 1;
			}
		}

		if (m_prebuilt->trx->check_foreigns) {
			for (uint i = 0; i < n_drop_index; i++) {
				dict_index_t*	index = drop_index[i];

				if (innobase_check_foreign_key_index(
						ha_alter_info, index,
						indexed_table, col_names,
						m_prebuilt->trx, drop_fk, n_drop_fk)) {
					row_mysql_unlock_data_dictionary(
						m_prebuilt->trx);
					m_prebuilt->trx->error_info = index;
					print_error(HA_ERR_DROP_INDEX_FK,
						MYF(0));
					goto err_exit;
				}
			}

			/* If a primary index is dropped, need to check
			any depending foreign constraints get affected */
			if (drop_primary
				&& innobase_check_foreign_key_index(
					ha_alter_info, drop_primary,
					indexed_table, col_names,
					m_prebuilt->trx, drop_fk, n_drop_fk)) {
				row_mysql_unlock_data_dictionary(m_prebuilt->trx);
				print_error(HA_ERR_DROP_INDEX_FK, MYF(0));
				goto err_exit;
			}
		}

		row_mysql_unlock_data_dictionary(m_prebuilt->trx);
	} else {
		drop_index = NULL;
	}

	/* Check if any of the existing indexes are marked as corruption
	and if they are, refuse adding more indexes. */
	if (ha_alter_info->handler_flags & ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX) {
		for (dict_index_t* index = dict_table_get_first_index(indexed_table);
		     index != NULL; index = dict_table_get_next_index(index)) {

			if (!index->to_be_dropped && index->is_committed()
			    && index->is_corrupted()) {
				my_error(ER_INDEX_CORRUPT, MYF(0), index->name());
				goto err_exit;
			}
		}
	}

	n_add_fk = 0;

	if (ha_alter_info->handler_flags
	    & ALTER_ADD_FOREIGN_KEY) {
		ut_ad(!m_prebuilt->trx->check_foreigns);

		alter_fill_stored_column(altered_table, m_prebuilt->table,
					 &s_cols, &s_heap);

		add_fk = static_cast<dict_foreign_t**>(
			mem_heap_zalloc(
				heap,
				ha_alter_info->alter_info->key_list.elements
				* sizeof(dict_foreign_t*)));

		if (!innobase_get_foreign_key_info(
			    ha_alter_info, table_share,
			    m_prebuilt->table, col_names,
			    drop_index, n_drop_index,
			    add_fk, &n_add_fk, m_prebuilt->trx, s_cols)) {
err_exit:
			if (n_drop_index) {
				row_mysql_lock_data_dictionary(m_prebuilt->trx);

				/* Clear the to_be_dropped flags, which might
				have been set at this point. */
				for (ulint i = 0; i < n_drop_index; i++) {
					ut_ad(drop_index[i]->is_committed());
					drop_index[i]->to_be_dropped = 0;
				}

				row_mysql_unlock_data_dictionary(
					m_prebuilt->trx);
			}

			if (heap) {
				mem_heap_free(heap);
			}

			if (s_cols != NULL) {
				UT_DELETE(s_cols);
				mem_heap_free(s_heap);
			}

			goto err_exit_no_heap;
		}

		if (s_cols != NULL) {
			UT_DELETE(s_cols);
			mem_heap_free(s_heap);
		}
	}

	if (ha_alter_info->handler_flags & ALTER_RENAME_INDEX) {
		for (const Alter_inplace_info::Rename_key_pair& pair :
		     ha_alter_info->rename_keys) {
			dict_index_t* index = dict_table_get_index_on_name(
			    indexed_table, pair.old_key->name.str);

			if (!index || index->is_corrupted()) {
				my_error(ER_INDEX_CORRUPT, MYF(0),
					 index->name());
				goto err_exit;
			}
		}
	}

	const ha_table_option_struct& alt_opt=
		*ha_alter_info->create_info->option_struct;

        ha_innobase_inplace_ctx *ctx = NULL;

	if (!(ha_alter_info->handler_flags & INNOBASE_ALTER_DATA)
	    || ((ha_alter_info->handler_flags & ~(INNOBASE_INPLACE_IGNORE
						  | INNOBASE_ALTER_NOCREATE
						  | INNOBASE_ALTER_INSTANT))
		== ALTER_OPTIONS
		&& !alter_options_need_rebuild(ha_alter_info, table))) {

		DBUG_ASSERT(!m_prebuilt->trx->dict_operation_lock_mode);
		online_retry_drop_indexes(m_prebuilt->table, m_user_thd);

		if (heap) {
			ctx = new ha_innobase_inplace_ctx(
					m_prebuilt,
					drop_index, n_drop_index,
					drop_fk, n_drop_fk,
					add_fk, n_add_fk,
					ha_alter_info->online,
					heap, indexed_table,
					col_names, ULINT_UNDEFINED, 0, 0,
					(ha_alter_info->ignore
					 || !thd_is_strict_mode(m_user_thd)),
					alt_opt.page_compressed,
					alt_opt.page_compression_level);
			ha_alter_info->handler_ctx = ctx;
		}

		if ((ha_alter_info->handler_flags
		     & ALTER_DROP_VIRTUAL_COLUMN)
		    && prepare_inplace_drop_virtual(ha_alter_info, table)) {
			DBUG_RETURN(true);
		}

		if ((ha_alter_info->handler_flags
		     & ALTER_ADD_VIRTUAL_COLUMN)
		    && prepare_inplace_add_virtual(
			    ha_alter_info, altered_table, table)) {
			DBUG_RETURN(true);
		}

		if (!(ha_alter_info->handler_flags & INNOBASE_ALTER_DATA)
		    && alter_templ_needs_rebuild(altered_table, ha_alter_info,
						 ctx->new_table)
		    && ctx->new_table->n_v_cols > 0) {
			/* Changing maria record structure may end up here only
			if virtual columns were altered. In this case, however,
			vc_templ should be rebuilt. Since we don't actually
			change any stored data, we can just dispose vc_templ;
			it will be recreated on next ha_innobase::open(). */

			DBUG_ASSERT(ctx->new_table == ctx->old_table);

			dict_free_vc_templ(ctx->new_table->vc_templ);
			UT_DELETE(ctx->new_table->vc_templ);

			ctx->new_table->vc_templ = NULL;
		}


success:
		/* Memorize the future transaction ID for committing
		the data dictionary change, to be reported by
		ha_innobase::table_version(). */
		m_prebuilt->trx_id = (ha_alter_info->handler_flags
				      & ~INNOBASE_INPLACE_IGNORE)
			? static_cast<ha_innobase_inplace_ctx*>
			(ha_alter_info->handler_ctx)->trx->id
			: 0;
		DBUG_RETURN(false);
	}

	/* If we are to build a full-text search index, check whether
	the table already has a DOC ID column.  If not, we will need to
	add a Doc ID hidden column and rebuild the primary index */
	if (innobase_fulltext_exist(altered_table)) {
		ulint	doc_col_no;
		ulint	num_v = 0;

		if (!innobase_fts_check_doc_id_col(
			    m_prebuilt->table,
			    altered_table, &fts_doc_col_no, &num_v)) {

			fts_doc_col_no = altered_table->s->fields - num_v;
			add_fts_doc_id = true;
			add_fts_doc_id_idx = true;

		} else if (fts_doc_col_no == ULINT_UNDEFINED) {
			goto err_exit;
		}

		switch (innobase_fts_check_doc_id_index(
				m_prebuilt->table, altered_table,
				&doc_col_no)) {
		case FTS_NOT_EXIST_DOC_ID_INDEX:
			add_fts_doc_id_idx = true;
			break;
		case FTS_INCORRECT_DOC_ID_INDEX:
			my_error(ER_INNODB_FT_WRONG_DOCID_INDEX, MYF(0),
				 FTS_DOC_ID_INDEX_NAME);
			goto err_exit;
		case FTS_EXIST_DOC_ID_INDEX:
			DBUG_ASSERT(
				doc_col_no == fts_doc_col_no
				|| doc_col_no == ULINT_UNDEFINED
				|| (ha_alter_info->handler_flags
				    & (ALTER_STORED_COLUMN_ORDER
				       | ALTER_DROP_STORED_COLUMN
				       | ALTER_ADD_STORED_BASE_COLUMN)));
		}
	}

	/* See if an AUTO_INCREMENT column was added. */
	uint	i = 0;
	ulint	num_v = 0;
	for (const Create_field& new_field :
	     ha_alter_info->alter_info->create_list) {
		const Field*	field;

		DBUG_ASSERT(i < altered_table->s->fields);

		for (uint old_i = 0; table->field[old_i]; old_i++) {
			if (new_field.field == table->field[old_i]) {
				goto found_col;
			}
		}

		/* This is an added column. */
		DBUG_ASSERT(!new_field.field);
		DBUG_ASSERT(ha_alter_info->handler_flags
			    & ALTER_ADD_COLUMN);

		field = altered_table->field[i];

		DBUG_ASSERT((field->unireg_check
			     == Field::NEXT_NUMBER)
			    == !!(field->flags & AUTO_INCREMENT_FLAG));

		if (field->flags & AUTO_INCREMENT_FLAG) {
			if (add_autoinc_col_no != ULINT_UNDEFINED) {
				/* This should have been blocked earlier. */
				ut_ad(0);
				my_error(ER_WRONG_AUTO_KEY, MYF(0));
				goto err_exit;
			}

			/* Get the col no of the old table non-virtual column array */
			add_autoinc_col_no = i - num_v;

			autoinc_col_max_value = innobase_get_int_col_max_value(field);
		}
found_col:
		num_v += !new_field.stored_in_db();
		i++;
	}

	DBUG_ASSERT(heap);
	DBUG_ASSERT(m_user_thd == m_prebuilt->trx->mysql_thd);
	DBUG_ASSERT(!ha_alter_info->handler_ctx);

	ha_alter_info->handler_ctx = new ha_innobase_inplace_ctx(
		m_prebuilt,
		drop_index, n_drop_index,
		drop_fk, n_drop_fk, add_fk, n_add_fk,
		ha_alter_info->online,
		heap, m_prebuilt->table, col_names,
		add_autoinc_col_no,
		ha_alter_info->create_info->auto_increment_value,
		autoinc_col_max_value,
		ha_alter_info->ignore || !thd_is_strict_mode(m_user_thd),
		alt_opt.page_compressed, alt_opt.page_compression_level);

	if (!prepare_inplace_alter_table_dict(
		    ha_alter_info, altered_table, table,
		    table_share->table_name.str,
		    info.flags(), info.flags2(),
		    fts_doc_col_no, add_fts_doc_id,
		    add_fts_doc_id_idx)) {
		goto success;
	}

	DBUG_RETURN(true);
}

/* Check whether a columnn length change alter operation requires
to rebuild the template.
@param[in]	altered_table	TABLE object for new version of table.
@param[in]	ha_alter_info	Structure describing changes to be done
				by ALTER TABLE and holding data used
				during in-place alter.
@param[in]	table		table being altered
@return TRUE if needs rebuild. */
static
bool
alter_templ_needs_rebuild(
	const TABLE*            altered_table,
	const Alter_inplace_info*     ha_alter_info,
	const dict_table_t*		table)
{
        ulint	i = 0;

	for (Field** fp = altered_table->field; *fp; fp++, i++) {
		for (const Create_field& cf :
		     ha_alter_info->alter_info->create_list) {
			for (ulint j=0; j < table->n_cols; j++) {
				dict_col_t* cols
                                   = dict_table_get_nth_col(table, j);
				if (cf.length > cols->len) {
					return(true);
				}
			}
		}
	}

	return(false);
}

/** Alter the table structure in-place with operations
specified using Alter_inplace_info.
The level of concurrency allowed during this operation depends
on the return value from check_if_supported_inplace_alter().

@param altered_table TABLE object for new version of table.
@param ha_alter_info Structure describing changes to be done
by ALTER TABLE and holding data used during in-place alter.

@retval true Failure
@retval false Success
*/

bool
ha_innobase::inplace_alter_table(
/*=============================*/
	TABLE*			altered_table,
	Alter_inplace_info*	ha_alter_info)
{
	dberr_t			error;
	dict_add_v_col_t*	add_v = NULL;
	dict_vcol_templ_t*	s_templ = NULL;
	dict_vcol_templ_t*	old_templ = NULL;
	struct TABLE*		eval_table = altered_table;
	bool			rebuild_templ = false;
	DBUG_ENTER("inplace_alter_table");
	DBUG_ASSERT(!srv_read_only_mode);

	DEBUG_SYNC(m_user_thd, "innodb_inplace_alter_table_enter");

	/* Ignore the inplace alter phase when table is empty */
	if (!(ha_alter_info->handler_flags & INNOBASE_ALTER_DATA)
	    || ha_alter_info->mdl_exclusive_after_prepare) {
ok_exit:
		DEBUG_SYNC(m_user_thd, "innodb_after_inplace_alter_table");
		DBUG_RETURN(false);
	}

	if ((ha_alter_info->handler_flags & ~(INNOBASE_INPLACE_IGNORE
					      | INNOBASE_ALTER_NOCREATE
					      | INNOBASE_ALTER_INSTANT))
	    == ALTER_OPTIONS
	    && !alter_options_need_rebuild(ha_alter_info, table)) {
		goto ok_exit;
	}

	ha_innobase_inplace_ctx*	ctx
		= static_cast<ha_innobase_inplace_ctx*>
		(ha_alter_info->handler_ctx);

	DBUG_ASSERT(ctx);
	DBUG_ASSERT(ctx->trx);
	DBUG_ASSERT(ctx->prebuilt == m_prebuilt);

	if (ctx->is_instant()) goto ok_exit;

	dict_index_t*	pk = dict_table_get_first_index(m_prebuilt->table);
	ut_ad(pk != NULL);

	/* For partitioned tables this could be already allocated from a
	previous partition invocation. For normal tables this is NULL. */
	UT_DELETE(ctx->m_stage);

	ctx->m_stage = UT_NEW_NOKEY(ut_stage_alter_t(pk));

	if (!m_prebuilt->table->is_readable()) {
		goto all_done;
	}

	/* If we are doing a table rebuilding or having added virtual
	columns in the same clause, we will need to build a table template
	that carries translation information between MySQL TABLE and InnoDB
	table, which indicates the virtual columns and their base columns
	info. This is used to do the computation callback, so that the
	data in base columns can be extracted send to server.
	If the Column length changes and it is a part of virtual
	index then we need to rebuild the template. */
	rebuild_templ
	     = ctx->need_rebuild()
	       || ((ha_alter_info->handler_flags
		& ALTER_COLUMN_TYPE_CHANGE_BY_ENGINE)
		&& alter_templ_needs_rebuild(
		   altered_table, ha_alter_info, ctx->new_table));

	if ((ctx->new_table->n_v_cols > 0) && rebuild_templ) {
		/* Save the templ if isn't NULL so as to restore the
		original state in case of alter operation failures. */
		if (ctx->new_table->vc_templ != NULL && !ctx->need_rebuild()) {
			old_templ = ctx->new_table->vc_templ;
		}
		s_templ = UT_NEW_NOKEY(dict_vcol_templ_t());

		innobase_build_v_templ(
			altered_table, ctx->new_table, s_templ, NULL, false);

		ctx->new_table->vc_templ = s_templ;
	} else if (ctx->num_to_add_vcol > 0 && ctx->num_to_drop_vcol == 0) {
		/* if there is ongoing drop virtual column, then we disallow
		inplace add index on newly added virtual column, so it does
		not need to come in here to rebuild template with add_v.
		Please also see the assertion in innodb_v_adjust_idx_col() */

		s_templ = UT_NEW_NOKEY(dict_vcol_templ_t());

		add_v = static_cast<dict_add_v_col_t*>(
			mem_heap_alloc(ctx->heap, sizeof *add_v));
		add_v->n_v_col = ctx->num_to_add_vcol;
		add_v->v_col = ctx->add_vcol;
		add_v->v_col_name = ctx->add_vcol_name;

		innobase_build_v_templ(
			altered_table, ctx->new_table, s_templ, add_v, false);
		old_templ = ctx->new_table->vc_templ;
		ctx->new_table->vc_templ = s_templ;
	}

	/* Drop virtual column without rebuild will keep dict table
	unchanged, we use old table to evaluate virtual column value
	in innobase_get_computed_value(). */
	if (!ctx->need_rebuild() && ctx->num_to_drop_vcol > 0) {
		eval_table = table;
	}

	/* Read the clustered index of the table and build
	indexes based on this information using temporary
	files and merge sort. */
	DBUG_EXECUTE_IF("innodb_OOM_inplace_alter",
			error = DB_OUT_OF_MEMORY; goto oom;);

	error = row_merge_build_indexes(
		m_prebuilt->trx,
		m_prebuilt->table, ctx->new_table,
		ctx->online,
		ctx->add_index, ctx->add_key_numbers, ctx->num_to_add_index,
		altered_table, ctx->defaults, ctx->col_map,
		ctx->add_autoinc, ctx->sequence, ctx->skip_pk_sort,
		ctx->m_stage, add_v, eval_table, ctx->allow_not_null);

#ifndef DBUG_OFF
oom:
#endif /* !DBUG_OFF */
	if (error == DB_SUCCESS && ctx->online && ctx->need_rebuild()) {
		DEBUG_SYNC_C("row_log_table_apply1_before");
		error = row_log_table_apply(
			ctx->thr, m_prebuilt->table, altered_table,
			ctx->m_stage, ctx->new_table);
	}

	/* Init online ddl status variables */
	onlineddl_rowlog_rows = 0;
	onlineddl_rowlog_pct_used = 0;
	onlineddl_pct_progress = 0;

	if (s_templ) {
		ut_ad(ctx->need_rebuild() || ctx->num_to_add_vcol > 0
		      || rebuild_templ);
		dict_free_vc_templ(s_templ);
		UT_DELETE(s_templ);

		ctx->new_table->vc_templ = old_templ;
	}

	DEBUG_SYNC_C("inplace_after_index_build");

	DBUG_EXECUTE_IF("create_index_fail",
			error = DB_DUPLICATE_KEY;
			m_prebuilt->trx->error_key_num = ULINT_UNDEFINED;);

	/* After an error, remove all those index definitions
	from the dictionary which were defined. */

	switch (error) {
		KEY*	dup_key;
	all_done:
	case DB_SUCCESS:
		ut_d(dict_sys.freeze(SRW_LOCK_CALL));
		ut_d(dict_table_check_for_dup_indexes(
			     m_prebuilt->table, CHECK_PARTIAL_OK));
		ut_d(dict_sys.unfreeze());
		/* prebuilt->table->n_ref_count can be anything here,
		given that we hold at most a shared lock on the table. */
		goto ok_exit;
	case DB_DUPLICATE_KEY:
		if (m_prebuilt->trx->error_key_num == ULINT_UNDEFINED
		    || ha_alter_info->key_count == 0) {
			/* This should be the hidden index on
			FTS_DOC_ID, or there is no PRIMARY KEY in the
			table. Either way, we should be seeing and
			reporting a bogus duplicate key error. */
			dup_key = NULL;
		} else {
			DBUG_ASSERT(m_prebuilt->trx->error_key_num
				    < ha_alter_info->key_count);
			dup_key = &ha_alter_info->key_info_buffer[
				m_prebuilt->trx->error_key_num];
		}
		print_keydup_error(altered_table, dup_key, MYF(0));
		break;
	case DB_ONLINE_LOG_TOO_BIG:
		DBUG_ASSERT(ctx->online);
		my_error(ER_INNODB_ONLINE_LOG_TOO_BIG, MYF(0),
			 get_error_key_name(m_prebuilt->trx->error_key_num,
					    ha_alter_info, m_prebuilt->table));
		break;
	case DB_INDEX_CORRUPT:
		my_error(ER_INDEX_CORRUPT, MYF(0),
			 get_error_key_name(m_prebuilt->trx->error_key_num,
					    ha_alter_info, m_prebuilt->table));
		break;
	case DB_DECRYPTION_FAILED: {
		String str;
		const char* engine= table_type();
		get_error_message(HA_ERR_DECRYPTION_FAILED, &str);
		my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_DECRYPTION_FAILED, str.c_ptr(), engine);
		break;
	}
	default:
		my_error_innodb(error,
				table_share->table_name.str,
				m_prebuilt->table->flags);
	}

	/* prebuilt->table->n_ref_count can be anything here, given
	that we hold at most a shared lock on the table. */
	m_prebuilt->trx->error_info = NULL;
	ctx->trx->error_state = DB_SUCCESS;

	DBUG_RETURN(true);
}

/** Free the modification log for online table rebuild.
@param table table that was being rebuilt online */
static
void
innobase_online_rebuild_log_free(
/*=============================*/
	dict_table_t*	table)
{
	dict_index_t* clust_index = dict_table_get_first_index(table);
	ut_ad(dict_sys.locked());
	clust_index->lock.x_lock(SRW_LOCK_CALL);

	if (clust_index->online_log) {
		ut_ad(dict_index_get_online_status(clust_index)
		      == ONLINE_INDEX_CREATION);
		clust_index->online_status = ONLINE_INDEX_COMPLETE;
		row_log_free(clust_index->online_log);
		clust_index->online_log = NULL;
		DEBUG_SYNC_C("innodb_online_rebuild_log_free_aborted");
	}

	DBUG_ASSERT(dict_index_get_online_status(clust_index)
		    == ONLINE_INDEX_COMPLETE);
	clust_index->lock.x_unlock();
}

/** For each user column, which is part of an index which is not going to be
dropped, it checks if the column number of the column is same as col_no
argument passed.
@param[in]	table		table
@param[in]	col_no		column number
@param[in]	is_v		if this is a virtual column
@param[in]	only_committed	whether to consider only committed indexes
@retval true column exists
@retval false column does not exist, true if column is system column or
it is in the index. */
static
bool
check_col_exists_in_indexes(
	const dict_table_t*	table,
	ulint			col_no,
	bool			is_v,
	bool			only_committed = false)
{
	/* This function does not check system columns */
	if (!is_v && dict_table_get_nth_col(table, col_no)->mtype == DATA_SYS) {
		return(true);
	}

	for (const dict_index_t* index = dict_table_get_first_index(table);
	     index;
	     index = dict_table_get_next_index(index)) {

		if (only_committed
		    ? !index->is_committed()
		    : index->to_be_dropped) {
			continue;
		}

		for (ulint i = 0; i < index->n_user_defined_cols; i++) {
			const dict_col_t* idx_col
				= dict_index_get_nth_col(index, i);

			if (is_v && idx_col->is_virtual()) {
				const dict_v_col_t*   v_col = reinterpret_cast<
					const dict_v_col_t*>(idx_col);
				if (v_col->v_pos == col_no) {
					return(true);
				}
			}

			if (!is_v && !idx_col->is_virtual()
			    && dict_col_get_no(idx_col) == col_no) {
				return(true);
			}
		}
	}

	return(false);
}

/** Rollback a secondary index creation, drop the indexes with
temparary index prefix
@param user_table InnoDB table
@param table the TABLE
@param locked TRUE=table locked, FALSE=may need to do a lazy drop
@param trx the transaction
@param alter_trx transaction which takes S-lock on the table
                 while creating the index */
static
void
innobase_rollback_sec_index(
        dict_table_t*   user_table,
        const TABLE*    table,
        bool            locked,
        trx_t*          trx,
        const trx_t*    alter_trx=NULL)
{
	row_merge_drop_indexes(trx, user_table, locked, alter_trx);

	/* Free the table->fts only if there is no FTS_DOC_ID
	in the table */
	if (user_table->fts
	    && !DICT_TF2_FLAG_IS_SET(user_table,
				     DICT_TF2_FTS_HAS_DOC_ID)
	    && !innobase_fulltext_exist(table)) {
		fts_free(user_table);
	}
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Roll back the changes made during prepare_inplace_alter_table()
and inplace_alter_table() inside the storage engine. Note that the
allowed level of concurrency during this operation will be the same as
for inplace_alter_table() and thus might be higher than during
prepare_inplace_alter_table(). (E.g concurrent writes were blocked
during prepare, but might not be during commit).

@param ha_alter_info Data used during in-place alter.
@param table the TABLE
@param prebuilt the prebuilt struct
@retval true Failure
@retval false Success
*/
inline bool rollback_inplace_alter_table(Alter_inplace_info *ha_alter_info,
                                         const TABLE *table,
                                         row_prebuilt_t *prebuilt)
{
  bool fail= false;
  ha_innobase_inplace_ctx *ctx= static_cast<ha_innobase_inplace_ctx*>
    (ha_alter_info->handler_ctx);

  DBUG_ENTER("rollback_inplace_alter_table");

  DEBUG_SYNC_C("innodb_rollback_inplace_alter_table");
  if (!ctx)
    /* If we have not started a transaction yet,
    (almost) nothing has been or needs to be done. */
    dict_sys.lock(SRW_LOCK_CALL);
  else if (ctx->trx->state == TRX_STATE_NOT_STARTED)
    goto free_and_exit;
  else if (ctx->new_table)
  {
    ut_ad(ctx->trx->state == TRX_STATE_ACTIVE);
    const bool fts_exist= (ctx->new_table->flags2 &
                           (DICT_TF2_FTS_HAS_DOC_ID | DICT_TF2_FTS)) ||
      ctx->adding_fulltext_index();
    if (ctx->need_rebuild())
    {
      if (fts_exist)
      {
        fts_optimize_remove_table(ctx->new_table);
        purge_sys.stop_FTS(*ctx->new_table);
      }

      dberr_t err= lock_table_for_trx(ctx->new_table, ctx->trx, LOCK_X);
      if (fts_exist)
      {
        if (err == DB_SUCCESS)
          err= fts_lock_common_tables(ctx->trx, *ctx->new_table);
        for (const dict_index_t* index= ctx->new_table->indexes.start;
             err == DB_SUCCESS && index; index= index->indexes.next)
          if (index->type & DICT_FTS)
            err= fts_lock_index_tables(ctx->trx, *index);
      }
      if (err == DB_SUCCESS)
        err= lock_sys_tables(ctx->trx);

      row_mysql_lock_data_dictionary(ctx->trx);
      /* Detach ctx->new_table from dict_index_t::online_log. */
      innobase_online_rebuild_log_free(ctx->old_table);

      ut_d(const bool last_handle=) ctx->new_table->release();
      ut_ad(last_handle);
      if (err == DB_SUCCESS)
        err= ctx->trx->drop_table(*ctx->new_table);

      if (err == DB_SUCCESS)
        for (const dict_index_t* index= ctx->new_table->indexes.start; index;
             index= index->indexes.next)
          if (index->type & DICT_FTS)
            if (dberr_t err2= fts_drop_index_tables(ctx->trx, *index))
              err= err2;

      if (err != DB_SUCCESS)
      {
        my_error_innodb(err, table->s->table_name.str, ctx->new_table->flags);
        fail= true;
      }
    }
    else
    {
      DBUG_ASSERT(!(ha_alter_info->handler_flags & ALTER_ADD_PK_INDEX));
      DBUG_ASSERT(ctx->old_table == prebuilt->table);
      uint &innodb_lock_wait_timeout=
        thd_lock_wait_timeout(ctx->trx->mysql_thd);
      const uint save_timeout= innodb_lock_wait_timeout;
      innodb_lock_wait_timeout= ~0U; /* infinite  */
      dict_index_t *old_clust_index= ctx->old_table->indexes.start;
      old_clust_index->lock.x_lock(SRW_LOCK_CALL);
      old_clust_index->online_log= nullptr;
      old_clust_index->lock.x_unlock();
      if (fts_exist)
      {
        const dict_index_t *fts_index= nullptr;
        for (ulint a= 0; a < ctx->num_to_add_index; a++)
        {
          const dict_index_t *index = ctx->add_index[a];
          if (index->type & DICT_FTS)
            fts_index= index;
        }

        /* Remove the fts table from fts_optimize_wq if there are
        no FTS secondary index exist other than newly added one */
        if (fts_index &&
            (ib_vector_is_empty(prebuilt->table->fts->indexes) ||
             (ib_vector_size(prebuilt->table->fts->indexes) == 1 &&
              fts_index == static_cast<dict_index_t*>(
                ib_vector_getp(prebuilt->table->fts->indexes, 0)))))
          fts_optimize_remove_table(prebuilt->table);

        purge_sys.stop_FTS(*prebuilt->table);
        ut_a(!fts_index || !fts_lock_index_tables(ctx->trx, *fts_index));
        ut_a(!fts_lock_common_tables(ctx->trx, *ctx->new_table));
        ut_a(!lock_sys_tables(ctx->trx));
      }
      else
      {
        ut_a(!lock_table_for_trx(dict_sys.sys_indexes, ctx->trx, LOCK_X));
        ut_a(!lock_table_for_trx(dict_sys.sys_fields, ctx->trx, LOCK_X));
      }
      innodb_lock_wait_timeout= save_timeout;
      row_mysql_lock_data_dictionary(ctx->trx);
      ctx->rollback_instant();
      innobase_rollback_sec_index(ctx->old_table, table,
                                  ha_alter_info->alter_info->requested_lock ==
                                  Alter_info::ALTER_TABLE_LOCK_EXCLUSIVE,
                                  ctx->trx, prebuilt->trx);
      ctx->clean_new_vcol_index();
      ut_d(dict_table_check_for_dup_indexes(ctx->old_table, CHECK_ABORTED_OK));
    }

    DEBUG_SYNC(ctx->trx->mysql_thd, "before_commit_rollback_inplace");
    commit_unlock_and_unlink(ctx->trx);
    if (fts_exist)
      purge_sys.resume_FTS();
    if (ctx->old_table->fts)
    {
      dict_sys.lock(SRW_LOCK_CALL);
      ut_ad(fts_check_cached_index(ctx->old_table));
      fts_optimize_add_table(ctx->old_table);
      dict_sys.unlock();
    }
    goto free_and_exit;
  }
  else
  {
free_and_exit:
    DBUG_ASSERT(ctx->prebuilt == prebuilt);
    ctx->trx->free();
    ctx->trx= nullptr;

    dict_sys.lock(SRW_LOCK_CALL);

    if (ctx->add_vcol)
    {
      for (ulint i = 0; i < ctx->num_to_add_vcol; i++)
        ctx->add_vcol[i].~dict_v_col_t();
      ctx->num_to_add_vcol= 0;
      ctx->add_vcol= nullptr;
    }

    for (ulint i= 0; i < ctx->num_to_add_fk; i++)
      dict_foreign_free(ctx->add_fk[i]);
    /* Clear the to_be_dropped flags in the data dictionary cache.
    The flags may already have been cleared, in case an error was
    detected in commit_inplace_alter_table(). */
    for (ulint i= 0; i < ctx->num_to_drop_index; i++)
    {
      dict_index_t *index= ctx->drop_index[i];
      DBUG_ASSERT(index->is_committed());
      index->to_be_dropped= 0;
    }
  }

  DBUG_ASSERT(!prebuilt->table->indexes.start->online_log);
  DBUG_ASSERT(prebuilt->table->indexes.start->online_status ==
              ONLINE_INDEX_COMPLETE);

  /* Reset dict_col_t::ord_part for unindexed columns */
  for (ulint i= 0; i < dict_table_get_n_cols(prebuilt->table); i++)
  {
    dict_col_t &col= prebuilt->table->cols[i];
    if (col.ord_part && !check_col_exists_in_indexes(prebuilt->table, i, false,
                                                     true))
      col.ord_part= 0;
  }

  for (ulint i = 0; i < dict_table_get_n_v_cols(prebuilt->table); i++)
  {
    dict_col_t &col = prebuilt->table->v_cols[i].m_col;
    if (col.ord_part && !check_col_exists_in_indexes(prebuilt->table, i, true,
                                                     true))
      col.ord_part= 0;
  }
  dict_sys.unlock();
  trx_commit_for_mysql(prebuilt->trx);
  prebuilt->trx_id = 0;
  MONITOR_ATOMIC_DEC(MONITOR_PENDING_ALTER_TABLE);
  DBUG_RETURN(fail);
}

/** Drop a FOREIGN KEY constraint from the data dictionary tables.
@param trx data dictionary transaction
@param table_name Table name in MySQL
@param foreign_id Foreign key constraint identifier
@retval true Failure
@retval false Success */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
innobase_drop_foreign_try(
/*======================*/
	trx_t*			trx,
	const char*		table_name,
	const char*		foreign_id)
{
	DBUG_ENTER("innobase_drop_foreign_try");

	DBUG_ASSERT(trx->dict_operation);
	ut_ad(trx->dict_operation_lock_mode);
	ut_ad(dict_sys.locked());

	/* Drop the constraint from the data dictionary. */
	static const char sql[] =
		"PROCEDURE DROP_FOREIGN_PROC () IS\n"
		"BEGIN\n"
		"DELETE FROM SYS_FOREIGN WHERE ID=:id;\n"
		"DELETE FROM SYS_FOREIGN_COLS WHERE ID=:id;\n"
		"END;\n";

	dberr_t		error;
	pars_info_t*	info;

	info = pars_info_create();
	pars_info_add_str_literal(info, "id", foreign_id);

	trx->op_info = "dropping foreign key constraint from dictionary";
	error = que_eval_sql(info, sql, trx);
	trx->op_info = "";

	DBUG_EXECUTE_IF("ib_drop_foreign_error",
			error = DB_OUT_OF_FILE_SPACE;);

	if (error != DB_SUCCESS) {
		my_error_innodb(error, table_name, 0);
		trx->error_state = DB_SUCCESS;
		DBUG_RETURN(true);
	}

	DBUG_RETURN(false);
}

/** Rename a column in the data dictionary tables.
@param[in] ctx			ALTER TABLE context
@param[in,out] trx		Data dictionary transaction
@param[in] table_name		Table name in MySQL
@param[in] from			old column name
@param[in] to			new column name
@retval true Failure
@retval false Success */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
innobase_rename_column_try(
	const ha_innobase_inplace_ctx&	ctx,
	trx_t*				trx,
	const char*			table_name,
	const char*			from,
	const char*			to)
{
	dberr_t		error;
	bool clust_has_wide_format = false;

	DBUG_ENTER("innobase_rename_column_try");

	DBUG_ASSERT(trx->dict_operation);
	ut_ad(trx->dict_operation_lock_mode);
	ut_ad(dict_sys.locked());

	if (ctx.need_rebuild()) {
		goto rename_foreign;
	}

	error = DB_SUCCESS;

	trx->op_info = "renaming column in SYS_FIELDS";

	for (const dict_index_t* index = dict_table_get_first_index(
		     ctx.old_table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {

		bool wide_format = false;
		for (size_t i = 0; i < dict_index_get_n_fields(index); i++) {
			dict_field_t* field= dict_index_get_nth_field(index, i);
			if (field->prefix_len || field->descending) {
				wide_format = true;
				break;
			}
		}

		for (ulint i = 0; i < dict_index_get_n_fields(index); i++) {
			const dict_field_t& f = index->fields[i];
			DBUG_ASSERT(!f.name == f.col->is_dropped());

			if (!f.name || my_strcasecmp(system_charset_info,
						     f.name, from)) {
				continue;
			}

			pars_info_t* info = pars_info_create();
			ulint pos = wide_format
				    ? i << 16 | f.prefix_len
				      | !!f.descending << 15
				    : i;
			pars_info_add_ull_literal(info, "indexid", index->id);
			pars_info_add_int4_literal(info, "nth", pos);
			pars_info_add_str_literal(info, "new", to);

			error = que_eval_sql(
				info,
				"PROCEDURE RENAME_SYS_FIELDS_PROC () IS\n"
				"BEGIN\n"
				"UPDATE SYS_FIELDS SET COL_NAME=:new\n"
				"WHERE INDEX_ID=:indexid\n"
				"AND POS=:nth;\n"
				"END;\n", trx);
			DBUG_EXECUTE_IF("ib_rename_column_error",
					error = DB_OUT_OF_FILE_SPACE;);

			if (error != DB_SUCCESS) {
				goto err_exit;
			}

			if (!wide_format || !clust_has_wide_format
			    || f.prefix_len || f.descending) {
				continue;
			}

			/* For secondary indexes, the
			wide_format check can be 'polluted'
			by PRIMARY KEY column prefix or descending
			field. Try also the simpler encoding
			of SYS_FIELDS.POS. */
			info = pars_info_create();

			pars_info_add_ull_literal(info, "indexid", index->id);
			pars_info_add_int4_literal(info, "nth", i);
			pars_info_add_str_literal(info, "new", to);

			error = que_eval_sql(
				info,
				"PROCEDURE RENAME_SYS_FIELDS_PROC () IS\n"
				"BEGIN\n"
				"UPDATE SYS_FIELDS SET COL_NAME=:new\n"
				"WHERE INDEX_ID=:indexid\n"
				"AND POS=:nth;\n"
				"END;\n", trx);

			if (error != DB_SUCCESS) {
				goto err_exit;
			}
		}

		if (index == dict_table_get_first_index(ctx.old_table)) {
			clust_has_wide_format = wide_format;
		}
	}

	if (error != DB_SUCCESS) {
err_exit:
		my_error_innodb(error, table_name, 0);
		trx->error_state = DB_SUCCESS;
		trx->op_info = "";
		DBUG_RETURN(true);
	}

rename_foreign:
	trx->op_info = "renaming column in SYS_FOREIGN_COLS";

	std::set<dict_foreign_t*> fk_evict;
	bool		foreign_modified;

	for (dict_foreign_set::const_iterator it = ctx.old_table->foreign_set.begin();
	     it != ctx.old_table->foreign_set.end();
	     ++it) {

		dict_foreign_t*	foreign = *it;
		foreign_modified = false;

		for (unsigned i = 0; i < foreign->n_fields; i++) {
			if (my_strcasecmp(system_charset_info,
					  foreign->foreign_col_names[i],
					  from)) {
				continue;
			}

			/* Ignore the foreign key rename if fk info
			is being dropped. */
			if (innobase_dropping_foreign(
				    foreign, ctx.drop_fk,
				    ctx.num_to_drop_fk)) {
				continue;
			}

			pars_info_t* info = pars_info_create();

			pars_info_add_str_literal(info, "id", foreign->id);
			pars_info_add_int4_literal(info, "nth", i);
			pars_info_add_str_literal(info, "new", to);

			error = que_eval_sql(
				info,
				"PROCEDURE RENAME_SYS_FOREIGN_F_PROC () IS\n"
				"BEGIN\n"
				"UPDATE SYS_FOREIGN_COLS\n"
				"SET FOR_COL_NAME=:new\n"
				"WHERE ID=:id AND POS=:nth;\n"
				"END;\n", trx);

			if (error != DB_SUCCESS) {
				goto err_exit;
			}
			foreign_modified = true;
		}

		if (foreign_modified) {
			fk_evict.insert(foreign);
		}
	}

	for (dict_foreign_set::const_iterator it
		= ctx.old_table->referenced_set.begin();
	     it != ctx.old_table->referenced_set.end();
	     ++it) {

		foreign_modified = false;
		dict_foreign_t*	foreign = *it;

		for (unsigned i = 0; i < foreign->n_fields; i++) {
			if (my_strcasecmp(system_charset_info,
					  foreign->referenced_col_names[i],
					  from)) {
				continue;
			}

			pars_info_t* info = pars_info_create();

			pars_info_add_str_literal(info, "id", foreign->id);
			pars_info_add_int4_literal(info, "nth", i);
			pars_info_add_str_literal(info, "new", to);

			error = que_eval_sql(
				info,
				"PROCEDURE RENAME_SYS_FOREIGN_R_PROC () IS\n"
				"BEGIN\n"
				"UPDATE SYS_FOREIGN_COLS\n"
				"SET REF_COL_NAME=:new\n"
				"WHERE ID=:id AND POS=:nth;\n"
				"END;\n", trx);

			if (error != DB_SUCCESS) {
				goto err_exit;
			}
			foreign_modified = true;
		}

		if (foreign_modified) {
			fk_evict.insert(foreign);
		}
	}

	/* Reload the foreign key info for instant table too. */
	if (ctx.need_rebuild() || ctx.is_instant()) {
		std::for_each(fk_evict.begin(), fk_evict.end(),
			      dict_foreign_remove_from_cache);
	}

	trx->op_info = "";
	DBUG_RETURN(false);
}

/** Rename columns in the data dictionary tables.
@param ha_alter_info Data used during in-place alter.
@param ctx In-place ALTER TABLE context
@param table the TABLE
@param trx data dictionary transaction
@param table_name Table name in MySQL
@retval true Failure
@retval false Success */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
innobase_rename_columns_try(
/*========================*/
	Alter_inplace_info*	ha_alter_info,
	ha_innobase_inplace_ctx*ctx,
	const TABLE*		table,
	trx_t*			trx,
	const char*		table_name)
{
	uint	i = 0;
	ulint	num_v = 0;

	DBUG_ASSERT(ctx->need_rebuild());
	DBUG_ASSERT(ha_alter_info->handler_flags
		    & ALTER_COLUMN_NAME);

	for (Field** fp = table->field; *fp; fp++, i++) {
		const bool is_virtual = !(*fp)->stored_in_db();
		if (!((*fp)->flags & FIELD_IS_RENAMED)) {
			goto processed_field;
		}

		for (const Create_field& cf :
		     ha_alter_info->alter_info->create_list) {
			if (cf.field == *fp) {
				if (innobase_rename_column_try(
					    *ctx, trx, table_name,
					    cf.field->field_name.str,
					    cf.field_name.str)) {
					return(true);
				}
				goto processed_field;
			}
		}

		ut_error;
processed_field:
		if (is_virtual) {
			num_v++;
		}

		continue;
	}

	return(false);
}

/** Convert field type and length to InnoDB format */
static void get_type(const Field& f, uint& prtype, uint8_t& mtype,
                     uint16_t& len)
{
	mtype = get_innobase_type_from_mysql_type(&prtype, &f);
	len = static_cast<uint16_t>(f.pack_length());
	prtype |= f.type();
	if (f.type() == MYSQL_TYPE_VARCHAR) {
		auto l = static_cast<const Field_varstring&>(f).length_bytes;
		len = static_cast<uint16_t>(len - l);
		if (l == 2) prtype |= DATA_LONG_TRUE_VARCHAR;
	}
	if (!f.real_maybe_null()) prtype |= DATA_NOT_NULL;
	if (f.binary()) prtype |= DATA_BINARY_TYPE;
	if (f.table->versioned()) {
		if (&f == f.table->field[f.table->s->vers.start_fieldno]) {
			prtype |= DATA_VERS_START;
		} else if (&f == f.table->field[f.table->s->vers.end_fieldno]) {
			prtype |= DATA_VERS_END;
		} else if (!(f.flags & VERS_UPDATE_UNVERSIONED_FLAG)) {
			prtype |= DATA_VERSIONED;
		}
	}
	if (!f.stored_in_db()) prtype |= DATA_VIRTUAL;

	if (dtype_is_string_type(mtype)) {
		prtype |= f.charset()->number << 16;
	}
}

/** Enlarge a column in the data dictionary tables.
@param ctx In-place ALTER TABLE context
@param trx data dictionary transaction
@param table_name Table name in MySQL
@param pos 0-based index to user_table->cols[] or user_table->v_cols[]
@param f new column
@param is_v if it's a virtual column
@retval true Failure
@retval false Success */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
innobase_rename_or_enlarge_column_try(
	ha_innobase_inplace_ctx*ctx,
	trx_t*			trx,
	const char*		table_name,
	ulint			pos,
	const Field&		f,
	bool			is_v)
{
	dict_col_t*	col;
	dict_table_t* user_table = ctx->old_table;

	DBUG_ENTER("innobase_rename_or_enlarge_column_try");
	DBUG_ASSERT(!ctx->need_rebuild());

	DBUG_ASSERT(trx->dict_operation);
	ut_ad(trx->dict_operation_lock_mode);
	ut_ad(dict_sys.locked());

	ulint n_base;

	if (is_v) {
		dict_v_col_t* v_col= dict_table_get_nth_v_col(user_table, pos);
		pos = dict_create_v_col_pos(v_col->v_pos, v_col->m_col.ind);
		col = &v_col->m_col;
		n_base = v_col->num_base;
	} else {
		col = dict_table_get_nth_col(user_table, pos);
		n_base = 0;
	}

	unsigned prtype;
	uint8_t mtype;
	uint16_t len;
	get_type(f, prtype, mtype, len);
	DBUG_ASSERT(!dtype_is_string_type(col->mtype)
		    || col->mbminlen == f.charset()->mbminlen);
	DBUG_ASSERT(col->len <= len);

#ifdef UNIV_DEBUG
	ut_ad(col->mbminlen <= col->mbmaxlen);
	switch (mtype) {
	case DATA_MYSQL:
		if (!(prtype & DATA_BINARY_TYPE) || user_table->not_redundant()
		    || col->mbminlen != col->mbmaxlen) {
			/* NOTE: we could allow this when !(prtype &
			DATA_BINARY_TYPE) and ROW_FORMAT is not REDUNDANT and
			mbminlen<mbmaxlen. That is, we treat a UTF-8 CHAR(n)
			column somewhat like a VARCHAR. */
			break;
		}
		/* fall through */
	case DATA_FIXBINARY:
	case DATA_CHAR:
		ut_ad(col->len == len);
		break;
	case DATA_BINARY:
	case DATA_VARCHAR:
	case DATA_VARMYSQL:
	case DATA_DECIMAL:
	case DATA_BLOB:
		break;
	default:
		ut_ad(!((col->prtype ^ prtype) & ~DATA_VERSIONED));
		ut_ad(col->mtype == mtype);
		ut_ad(col->len == len);
	}
#endif /* UNIV_DEBUG */

	const char* col_name = col->name(*user_table);
	const bool same_name = !strcmp(col_name, f.field_name.str);

	if (!same_name
	    && innobase_rename_column_try(*ctx, trx, table_name,
					  col_name, f.field_name.str)) {
		DBUG_RETURN(true);
	}

	if (same_name
	    && col->prtype == prtype && col->mtype == mtype
	    && col->len == len) {
		DBUG_RETURN(false);
	}

	DBUG_RETURN(innodb_insert_sys_columns(user_table->id, pos,
					      f.field_name.str,
					      mtype, prtype, len,
					      n_base, trx, true));
}

/** Rename or enlarge columns in the data dictionary cache
as part of commit_try_norebuild().
@param ha_alter_info Data used during in-place alter.
@param ctx In-place ALTER TABLE context
@param altered_table metadata after ALTER TABLE
@param table metadata before ALTER TABLE
@param trx data dictionary transaction
@param table_name Table name in MySQL
@retval true Failure
@retval false Success */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
innobase_rename_or_enlarge_columns_try(
	Alter_inplace_info*	ha_alter_info,
	ha_innobase_inplace_ctx*ctx,
	const TABLE*		altered_table,
	const TABLE*		table,
	trx_t*			trx,
	const char*		table_name)
{
	DBUG_ENTER("innobase_rename_or_enlarge_columns_try");

	if (!(ha_alter_info->handler_flags
	      & (ALTER_COLUMN_TYPE_CHANGE_BY_ENGINE
		 | ALTER_COLUMN_NAME))) {
		DBUG_RETURN(false);
	}

	ulint	i = 0;
	ulint	num_v = 0;

	for (Field** fp = table->field; *fp; fp++, i++) {
		const bool is_v = !(*fp)->stored_in_db();
		ulint idx = is_v ? num_v++ : i - num_v;

		Field** af = altered_table->field;
		for (const Create_field& cf :
		     ha_alter_info->alter_info->create_list) {
			if (cf.field == *fp) {
				if (innobase_rename_or_enlarge_column_try(
					    ctx, trx, table_name,
					    idx, **af, is_v)) {
					DBUG_RETURN(true);
				}
				break;
			}
			af++;
		}
	}

	DBUG_RETURN(false);
}

/** Rename or enlarge columns in the data dictionary cache
as part of commit_cache_norebuild().
@param ha_alter_info Data used during in-place alter.
@param altered_table metadata after ALTER TABLE
@param table metadata before ALTER TABLE
@param user_table InnoDB table that was being altered */
static MY_ATTRIBUTE((nonnull))
void
innobase_rename_or_enlarge_columns_cache(
/*=====================================*/
	Alter_inplace_info*	ha_alter_info,
	const TABLE*		altered_table,
	const TABLE*		table,
	dict_table_t*		user_table)
{
	if (!(ha_alter_info->handler_flags
	      & (ALTER_COLUMN_TYPE_CHANGE_BY_ENGINE
		 | ALTER_COLUMN_NAME))) {
		return;
	}

	uint	i = 0;
	ulint	num_v = 0;

	for (Field** fp = table->field; *fp; fp++, i++) {
		const bool is_virtual = !(*fp)->stored_in_db();

		Field** af = altered_table->field;
		for (Create_field& cf :
		     ha_alter_info->alter_info->create_list) {
			if (cf.field != *fp) {
				af++;
				continue;
			}

			ulint	col_n = is_virtual ? num_v : i - num_v;
			dict_col_t *col = is_virtual
				? &dict_table_get_nth_v_col(user_table, col_n)
				->m_col
				: dict_table_get_nth_col(user_table, col_n);
			const bool is_string= dtype_is_string_type(col->mtype);
			DBUG_ASSERT(col->mbminlen
				    == (is_string
					? (*af)->charset()->mbminlen : 0));
			unsigned prtype;
			uint8_t mtype;
			uint16_t len;
			get_type(**af, prtype, mtype, len);
			DBUG_ASSERT(is_string == dtype_is_string_type(mtype));

			col->prtype = prtype;
			col->mtype = mtype;
			col->len = len;
			col->mbmaxlen = is_string
				? (*af)->charset()->mbmaxlen & 7: 0;

			if ((*fp)->flags & FIELD_IS_RENAMED) {
				dict_mem_table_col_rename(
					user_table, col_n,
					cf.field->field_name.str,
					(*af)->field_name.str, is_virtual);
			}

			break;
		}

		if (is_virtual) {
			num_v++;
		}
	}
}

/** Set the auto-increment value of the table on commit.
@param ha_alter_info Data used during in-place alter
@param ctx In-place ALTER TABLE context
@param altered_table MySQL table that is being altered
@param old_table MySQL table as it is before the ALTER operation
@return whether the operation failed (and my_error() was called) */
static MY_ATTRIBUTE((nonnull))
bool
commit_set_autoinc(
	Alter_inplace_info*	ha_alter_info,
	ha_innobase_inplace_ctx*ctx,
	const TABLE*		altered_table,
	const TABLE*		old_table)
{
	DBUG_ENTER("commit_set_autoinc");

	if (!altered_table->found_next_number_field) {
		/* There is no AUTO_INCREMENT column in the table
		after the ALTER operation. */
	} else if (ctx->add_autoinc != ULINT_UNDEFINED) {
		ut_ad(ctx->need_rebuild());
		/* An AUTO_INCREMENT column was added. Get the last
		value from the sequence, which may be based on a
		supplied AUTO_INCREMENT value. */
		ib_uint64_t autoinc = ctx->sequence.last();
		ctx->new_table->autoinc = autoinc;
		/* Bulk index creation does not update
		PAGE_ROOT_AUTO_INC, so we must persist the "last used"
		value here. */
		btr_write_autoinc(dict_table_get_first_index(ctx->new_table),
				  autoinc - 1, true);
	} else if ((ha_alter_info->handler_flags
		    & ALTER_CHANGE_CREATE_OPTION)
		   && (ha_alter_info->create_info->used_fields
		       & HA_CREATE_USED_AUTO)) {

		if (!ctx->old_table->space) {
			my_error(ER_TABLESPACE_DISCARDED, MYF(0),
				 old_table->s->table_name.str);
			DBUG_RETURN(true);
		}

		/* An AUTO_INCREMENT value was supplied by the user.
		It must be persisted to the data file. */
		const Field*	ai	= old_table->found_next_number_field;
		ut_ad(!strcmp(dict_table_get_col_name(ctx->old_table,
						      innodb_col_no(ai)),
			      ai->field_name.str));

		ib_uint64_t	autoinc
			= ha_alter_info->create_info->auto_increment_value;
		if (autoinc == 0) {
			autoinc = 1;
		}

		if (autoinc >= ctx->old_table->autoinc) {
			/* Persist the predecessor of the
			AUTO_INCREMENT value as the last used one. */
			ctx->new_table->autoinc = autoinc--;
		} else {
			/* Mimic ALGORITHM=COPY in the following scenario:

			CREATE TABLE t (a SERIAL);
			INSERT INTO t SET a=100;
			ALTER TABLE t AUTO_INCREMENT = 1;
			INSERT INTO t SET a=NULL;
			SELECT * FROM t;

			By default, ALGORITHM=INPLACE would reset the
			sequence to 1, while after ALGORITHM=COPY, the
			last INSERT would use a value larger than 100.

			We could only search the tree to know current
			max counter in the table and compare. */
			const dict_col_t*	autoinc_col
				= dict_table_get_nth_col(ctx->old_table,
							 innodb_col_no(ai));
			dict_index_t*		index
				= dict_table_get_first_index(ctx->old_table);
			while (index != NULL
			       && index->fields[0].col != autoinc_col) {
				index = dict_table_get_next_index(index);
			}

			ut_ad(index);

			ib_uint64_t	max_in_table = index
				? row_search_max_autoinc(index)
				: 0;

			if (autoinc <= max_in_table) {
				ctx->new_table->autoinc = innobase_next_autoinc(
					max_in_table, 1,
					ctx->prebuilt->autoinc_increment,
					ctx->prebuilt->autoinc_offset,
					innobase_get_int_col_max_value(ai));
				/* Persist the maximum value as the
				last used one. */
				autoinc = max_in_table;
			} else {
				/* Persist the predecessor of the
				AUTO_INCREMENT value as the last used one. */
				ctx->new_table->autoinc = autoinc--;
			}
		}

		btr_write_autoinc(dict_table_get_first_index(ctx->new_table),
				  autoinc, true);
	} else if (ctx->need_rebuild()) {
		/* No AUTO_INCREMENT value was specified.
		Copy it from the old table. */
		ctx->new_table->autoinc = ctx->old_table->autoinc;
		/* The persistent value was already copied in
		prepare_inplace_alter_table_dict() when ctx->new_table
		was created. If this was a LOCK=NONE operation, the
		AUTO_INCREMENT values would be updated during
		row_log_table_apply(). If this was LOCK!=NONE,
		the table contents could not possibly have changed
		between prepare_inplace and commit_inplace. */
	}

	DBUG_RETURN(false);
}

/** Add or drop foreign key constraints to the data dictionary tables,
but do not touch the data dictionary cache.
@param ha_alter_info Data used during in-place alter
@param ctx In-place ALTER TABLE context
@param trx Data dictionary transaction
@param table_name Table name in MySQL
@retval true Failure
@retval false Success
*/
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
innobase_update_foreign_try(
/*========================*/
	ha_innobase_inplace_ctx*ctx,
	trx_t*			trx,
	const char*		table_name)
{
	ulint	foreign_id;
	ulint	i;

	DBUG_ENTER("innobase_update_foreign_try");

	foreign_id = dict_table_get_highest_foreign_id(ctx->new_table);

	foreign_id++;

	for (i = 0; i < ctx->num_to_add_fk; i++) {
		dict_foreign_t*		fk = ctx->add_fk[i];

		ut_ad(fk->foreign_table == ctx->new_table
		      || fk->foreign_table == ctx->old_table);

		dberr_t error = dict_create_add_foreign_id(
			&foreign_id, ctx->old_table->name.m_name, fk);

		if (error != DB_SUCCESS) {
			my_error(ER_TOO_LONG_IDENT, MYF(0),
				 fk->id);
			DBUG_RETURN(true);
		}

		if (!fk->foreign_index) {
			fk->foreign_index = dict_foreign_find_index(
				ctx->new_table, ctx->col_names,
				fk->foreign_col_names,
				fk->n_fields, fk->referenced_index, TRUE,
				fk->type
				& (DICT_FOREIGN_ON_DELETE_SET_NULL
					| DICT_FOREIGN_ON_UPDATE_SET_NULL),
				NULL, NULL, NULL);
			if (!fk->foreign_index) {
				my_error(ER_FK_INCORRECT_OPTION,
					 MYF(0), table_name, fk->id);
				DBUG_RETURN(true);
			}
		}

		/* The fk->foreign_col_names[] uses renamed column
		names, while the columns in ctx->old_table have not
		been renamed yet. */
		error = dict_create_add_foreign_to_dictionary(
			ctx->old_table->name.m_name, fk, trx);

		DBUG_EXECUTE_IF(
			"innodb_test_cannot_add_fk_system",
			error = DB_ERROR;);

		if (error != DB_SUCCESS) {
			my_error(ER_FK_FAIL_ADD_SYSTEM, MYF(0),
				 fk->id);
			DBUG_RETURN(true);
		}
	}

	for (i = 0; i < ctx->num_to_drop_fk; i++) {
		dict_foreign_t* fk = ctx->drop_fk[i];

		DBUG_ASSERT(fk->foreign_table == ctx->old_table);

		if (innobase_drop_foreign_try(trx, table_name, fk->id)) {
			DBUG_RETURN(true);
		}
	}

	DBUG_RETURN(false);
}

/** Update the foreign key constraint definitions in the data dictionary cache
after the changes to data dictionary tables were committed.
@param ctx	In-place ALTER TABLE context
@param user_thd	MySQL connection
@return		InnoDB error code (should always be DB_SUCCESS) */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
innobase_update_foreign_cache(
/*==========================*/
	ha_innobase_inplace_ctx*	ctx,
	THD*				user_thd)
{
	dict_table_t*	user_table;
	dberr_t		err = DB_SUCCESS;

	DBUG_ENTER("innobase_update_foreign_cache");

	ut_ad(dict_sys.locked());

	user_table = ctx->old_table;

	/* Discard the added foreign keys, because we will
	load them from the data dictionary. */
	for (ulint i = 0; i < ctx->num_to_add_fk; i++) {
		dict_foreign_t*	fk = ctx->add_fk[i];
		dict_foreign_free(fk);
	}

	if (ctx->need_rebuild()) {
		/* The rebuilt table is already using the renamed
		column names. No need to pass col_names or to drop
		constraints from the data dictionary cache. */
		DBUG_ASSERT(!ctx->col_names);
		user_table = ctx->new_table;
	} else {
		/* Drop the foreign key constraints if the
		table was not rebuilt. If the table is rebuilt,
		there would not be any foreign key contraints for
		it yet in the data dictionary cache. */
		for (ulint i = 0; i < ctx->num_to_drop_fk; i++) {
			dict_foreign_t* fk = ctx->drop_fk[i];
			dict_foreign_remove_from_cache(fk);
		}
	}

	/* Load the old or added foreign keys from the data dictionary
	and prevent the table from being evicted from the data
	dictionary cache (work around the lack of WL#6049). */
	dict_names_t	fk_tables;

	err = dict_load_foreigns(user_table->name.m_name,
				 ctx->col_names, 1, true,
				 DICT_ERR_IGNORE_NONE,
				 fk_tables);

	if (err == DB_CANNOT_ADD_CONSTRAINT) {
		fk_tables.clear();

		/* It is possible there are existing foreign key are
		loaded with "foreign_key checks" off,
		so let's retry the loading with charset_check is off */
		err = dict_load_foreigns(user_table->name.m_name,
					 ctx->col_names, 1, false,
					 DICT_ERR_IGNORE_NONE,
					 fk_tables);

		/* The load with "charset_check" off is successful, warn
		the user that the foreign key has loaded with mis-matched
		charset */
		if (err == DB_SUCCESS) {
			push_warning_printf(
				user_thd,
				Sql_condition::WARN_LEVEL_WARN,
				ER_ALTER_INFO,
				"Foreign key constraints for table '%s'"
				" are loaded with charset check off",
				user_table->name.m_name);
		}
	}

	/* For complete loading of foreign keys, all associated tables must
	also be loaded. */
	while (err == DB_SUCCESS && !fk_tables.empty()) {
		const char *f = fk_tables.front();
		if (!dict_sys.load_table({f, strlen(f)})) {
			err = DB_TABLE_NOT_FOUND;
			ib::error()
				<< "Failed to load table "
				<< table_name_t(const_cast<char*>(f))
				<< " which has a foreign key constraint with"
				<< user_table->name;
			break;
		}

		fk_tables.pop_front();
	}

	DBUG_RETURN(err);
}

/** Changes SYS_COLUMNS.PRTYPE for one column.
@param[in,out]	trx	transaction
@param[in]	table_name	table name
@param[in]	tableid	table ID as in SYS_TABLES
@param[in]	pos	column position
@param[in]	prtype	new precise type
@return		boolean flag
@retval	true	on failure
@retval false	on success */
static
bool
vers_change_field_try(
	trx_t* trx,
	const char* table_name,
	const table_id_t tableid,
	const ulint pos,
	const ulint prtype)
{
	DBUG_ENTER("vers_change_field_try");

	pars_info_t* info = pars_info_create();

	pars_info_add_int4_literal(info, "prtype", prtype);
	pars_info_add_ull_literal(info,"tableid", tableid);
	pars_info_add_int4_literal(info, "pos", pos);

	dberr_t error = que_eval_sql(info,
				     "PROCEDURE CHANGE_COLUMN_MTYPE () IS\n"
				     "BEGIN\n"
				     "UPDATE SYS_COLUMNS SET PRTYPE=:prtype\n"
				     "WHERE TABLE_ID=:tableid AND POS=:pos;\n"
				     "END;\n", trx);

	if (error != DB_SUCCESS) {
		my_error_innodb(error, table_name, 0);
		trx->error_state = DB_SUCCESS;
		trx->op_info = "";
		DBUG_RETURN(true);
	}

	DBUG_RETURN(false);
}

/** Changes fields WITH/WITHOUT SYSTEM VERSIONING property in SYS_COLUMNS.
@param[in]	ha_alter_info	alter info
@param[in]	ctx	alter inplace context
@param[in]	trx	transaction
@param[in]	table	old table
@return		boolean flag
@retval	true	on failure
@retval false	on success */
static
bool
vers_change_fields_try(
	const Alter_inplace_info* ha_alter_info,
	const ha_innobase_inplace_ctx* ctx,
	trx_t* trx,
	const TABLE* table)
{
	DBUG_ENTER("vers_change_fields_try");

	DBUG_ASSERT(ha_alter_info);
	DBUG_ASSERT(ctx);

	for (const Create_field& create_field : ha_alter_info->alter_info->create_list) {
		if (!create_field.field) {
			continue;
		}
		if (create_field.versioning
		    == Column_definition::VERSIONING_NOT_SET) {
			continue;
		}

		const dict_table_t* new_table = ctx->new_table;
		const uint pos = innodb_col_no(create_field.field);
		const dict_col_t* col = dict_table_get_nth_col(new_table, pos);

		DBUG_ASSERT(!col->vers_sys_start());
		DBUG_ASSERT(!col->vers_sys_end());

		ulint new_prtype
		    = create_field.versioning
			      == Column_definition::WITHOUT_VERSIONING
			  ? col->prtype & ~DATA_VERSIONED
			  : col->prtype | DATA_VERSIONED;

		if (vers_change_field_try(trx, table->s->table_name.str,
					  new_table->id, pos,
					  new_prtype)) {
			DBUG_RETURN(true);
		}
	}

	DBUG_RETURN(false);
}

/** Changes WITH/WITHOUT SYSTEM VERSIONING for fields
in the data dictionary cache.
@param ha_alter_info Data used during in-place alter
@param ctx In-place ALTER TABLE context
@param table MySQL table as it is before the ALTER operation */
static
void
vers_change_fields_cache(
	Alter_inplace_info*		ha_alter_info,
	const ha_innobase_inplace_ctx*	ctx,
	const TABLE*			table)
{
	DBUG_ENTER("vers_change_fields_cache");

	DBUG_ASSERT(ha_alter_info);
	DBUG_ASSERT(ctx);
	DBUG_ASSERT(ha_alter_info->handler_flags & ALTER_COLUMN_UNVERSIONED);

	for (const Create_field& create_field :
	     ha_alter_info->alter_info->create_list) {
		if (!create_field.field || create_field.field->vcol_info) {
			continue;
		}
		dict_col_t* col = dict_table_get_nth_col(
		    ctx->new_table, innodb_col_no(create_field.field));

		if (create_field.versioning
		    == Column_definition::WITHOUT_VERSIONING) {

			DBUG_ASSERT(!col->vers_sys_start());
			DBUG_ASSERT(!col->vers_sys_end());
			col->prtype &= ~DATA_VERSIONED;
		} else if (create_field.versioning
			   == Column_definition::WITH_VERSIONING) {

			DBUG_ASSERT(!col->vers_sys_start());
			DBUG_ASSERT(!col->vers_sys_end());
			col->prtype |= DATA_VERSIONED;
		}
	}

	DBUG_VOID_RETURN;
}

/** Commit the changes made during prepare_inplace_alter_table()
and inplace_alter_table() inside the data dictionary tables,
when rebuilding the table.
@param ha_alter_info Data used during in-place alter
@param ctx In-place ALTER TABLE context
@param altered_table MySQL table that is being altered
@param old_table MySQL table as it is before the ALTER operation
@param trx Data dictionary transaction
@param table_name Table name in MySQL
@retval true Failure
@retval false Success
*/
inline MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
commit_try_rebuild(
/*===============*/
	Alter_inplace_info*	ha_alter_info,
	ha_innobase_inplace_ctx*ctx,
	TABLE*			altered_table,
	const TABLE*		old_table,
	trx_t*			trx,
	const char*		table_name)
{
	dict_table_t*	rebuilt_table	= ctx->new_table;
	dict_table_t*	user_table	= ctx->old_table;

	DBUG_ENTER("commit_try_rebuild");
	DBUG_ASSERT(ctx->need_rebuild());
	DBUG_ASSERT(trx->dict_operation_lock_mode);
	DBUG_ASSERT(!(ha_alter_info->handler_flags
		      & ALTER_DROP_FOREIGN_KEY)
		    || ctx->num_to_drop_fk > 0);
	DBUG_ASSERT(ctx->num_to_drop_fk
		    <= ha_alter_info->alter_info->drop_list.elements);

	innobase_online_rebuild_log_free(user_table);

	for (dict_index_t* index = dict_table_get_first_index(rebuilt_table);
	     index;
	     index = dict_table_get_next_index(index)) {
		DBUG_ASSERT(dict_index_get_online_status(index)
			    == ONLINE_INDEX_COMPLETE);
		DBUG_ASSERT(index->is_committed());
		if (index->is_corrupted()) {
			my_error(ER_INDEX_CORRUPT, MYF(0), index->name());
			DBUG_RETURN(true);
		}
	}

	if (innobase_update_foreign_try(ctx, trx, table_name)) {
		DBUG_RETURN(true);
	}

	/* Clear the to_be_dropped flag in the data dictionary cache
	of user_table. */
	for (ulint i = 0; i < ctx->num_to_drop_index; i++) {
		dict_index_t*	index = ctx->drop_index[i];
		DBUG_ASSERT(index->table == user_table);
		DBUG_ASSERT(index->is_committed());
		DBUG_ASSERT(index->to_be_dropped);
		index->to_be_dropped = 0;
	}

	if ((ha_alter_info->handler_flags
	     & ALTER_COLUMN_NAME)
	    && innobase_rename_columns_try(ha_alter_info, ctx, old_table,
					   trx, table_name)) {
		DBUG_RETURN(true);
	}

	/* The new table must inherit the flag from the
	"parent" table. */
	if (!user_table->space) {
		rebuilt_table->file_unreadable = true;
		rebuilt_table->flags2 |= DICT_TF2_DISCARDED;
	}

	/* We can now rename the old table as a temporary table,
	rename the new temporary table as the old table and drop the
	old table. */
	char* old_name= mem_heap_strdup(ctx->heap, user_table->name.m_name);

	dberr_t error = row_rename_table_for_mysql(user_table->name.m_name,
						   ctx->tmp_name, trx, false);
	if (error == DB_SUCCESS) {
		error = row_rename_table_for_mysql(
			rebuilt_table->name.m_name, old_name, trx, false);
		if (error == DB_SUCCESS) {
			/* The statistics for the surviving indexes will be
			re-inserted in alter_stats_rebuild(). */
			error = trx->drop_table_statistics(old_name);
			if (error == DB_SUCCESS) {
				error = trx->drop_table(*user_table);
			}
		}
	}

	/* We must be still holding a table handle. */
	DBUG_ASSERT(user_table->get_ref_count() == 1);
	DBUG_EXECUTE_IF("ib_rebuild_cannot_rename", error = DB_ERROR;);

	switch (error) {
	case DB_SUCCESS:
		DBUG_RETURN(false);
	case DB_TABLESPACE_EXISTS:
		ut_a(rebuilt_table->get_ref_count() == 1);
		my_error(ER_TABLESPACE_EXISTS, MYF(0), ctx->tmp_name);
		DBUG_RETURN(true);
	case DB_DUPLICATE_KEY:
		ut_a(rebuilt_table->get_ref_count() == 1);
		my_error(ER_TABLE_EXISTS_ERROR, MYF(0), ctx->tmp_name);
		DBUG_RETURN(true);
	default:
		my_error_innodb(error, table_name, user_table->flags);
		DBUG_RETURN(true);
	}
}

/** Rename indexes in dictionary.
@param[in]	ctx		alter info context
@param[in]	ha_alter_info	Operation used during inplace alter
@param[out]	trx		transaction to change the index name
				in dictionary
@return true if it failed to rename
@return false if it is success. */
static
bool
rename_indexes_try(
	const ha_innobase_inplace_ctx*	ctx,
	const Alter_inplace_info*	ha_alter_info,
	trx_t*				trx)
{
	DBUG_ASSERT(ha_alter_info->handler_flags & ALTER_RENAME_INDEX);

	for (const Alter_inplace_info::Rename_key_pair& pair :
	     ha_alter_info->rename_keys) {
		dict_index_t* index = dict_table_get_index_on_name(
		    ctx->old_table, pair.old_key->name.str);
		// This was checked previously in
		// ha_innobase::prepare_inplace_alter_table()
		ut_ad(index);

		if (rename_index_try(index, pair.new_key->name.str, trx)) {
			return true;
		}
	}

	return false;
}

/** Set of column numbers */
typedef std::set<ulint, std::less<ulint>, ut_allocator<ulint> >	col_set;

/** Collect (not instantly dropped) columns from dropped indexes
@param[in]	ctx		In-place ALTER TABLE context
@param[in, out]	drop_col_list	list which will be set, containing columns
				which is part of index being dropped
@param[in, out]	drop_v_col_list	list which will be set, containing
				virtual columns which is part of index
				being dropped */
static
void
collect_columns_from_dropped_indexes(
	const ha_innobase_inplace_ctx*	ctx,
	col_set&			drop_col_list,
	col_set&			drop_v_col_list)
{
	for (ulint index_count = 0; index_count < ctx->num_to_drop_index;
	     index_count++) {
		const dict_index_t*	index = ctx->drop_index[index_count];

		for (ulint col = 0; col < index->n_user_defined_cols; col++) {
			const dict_col_t*	idx_col
				= dict_index_get_nth_col(index, col);

			if (idx_col->is_virtual()) {
				const dict_v_col_t*	v_col
					= reinterpret_cast<
						const dict_v_col_t*>(idx_col);
				drop_v_col_list.insert(v_col->v_pos);

			} else {
				ulint	col_no = dict_col_get_no(idx_col);
				if (ctx->col_map
				    && ctx->col_map[col_no]
					   == ULINT_UNDEFINED) {
					// this column was instantly dropped
					continue;
				}
				drop_col_list.insert(col_no);
			}
		}
	}
}

/** Change PAGE_COMPRESSED to ON or change the PAGE_COMPRESSION_LEVEL.
@param[in]	level		PAGE_COMPRESSION_LEVEL
@param[in]	table		table before the change
@param[in,out]	trx		data dictionary transaction
@param[in]	table_name	table name in MariaDB
@return	whether the operation succeeded */
MY_ATTRIBUTE((nonnull, warn_unused_result))
static
bool
innobase_page_compression_try(
	uint			level,
	const dict_table_t*	table,
	trx_t*			trx,
	const char*		table_name)
{
	DBUG_ENTER("innobase_page_compression_try");
	DBUG_ASSERT(level >= 1);
	DBUG_ASSERT(level <= 9);

	unsigned flags = table->flags
		& ~(0xFU << DICT_TF_POS_PAGE_COMPRESSION_LEVEL);
	flags |= 1U << DICT_TF_POS_PAGE_COMPRESSION
		| level << DICT_TF_POS_PAGE_COMPRESSION_LEVEL;

	if (table->flags == flags) {
		DBUG_RETURN(false);
	}

	pars_info_t* info = pars_info_create();

	pars_info_add_ull_literal(info, "id", table->id);
	pars_info_add_int4_literal(info, "type",
				   dict_tf_to_sys_tables_type(flags));

	dberr_t error = que_eval_sql(info,
				     "PROCEDURE CHANGE_COMPRESSION () IS\n"
				     "BEGIN\n"
				     "UPDATE SYS_TABLES SET TYPE=:type\n"
				     "WHERE ID=:id;\n"
				     "END;\n", trx);

	if (error != DB_SUCCESS) {
		my_error_innodb(error, table_name, 0);
		trx->error_state = DB_SUCCESS;
		trx->op_info = "";
		DBUG_RETURN(true);
	}

	DBUG_RETURN(false);
}

/** Evict the table from cache and reopen it. Drop outdated statistics.
@param thd           mariadb THD entity
@param table         innodb table
@param table_name    user-friendly table name for errors
@param ctx           ALTER TABLE context
@return newly opened table */
static dict_table_t *innobase_reload_table(THD *thd, dict_table_t *table,
                                           const LEX_CSTRING &table_name,
                                           ha_innobase_inplace_ctx &ctx)
{
  if (ctx.is_instant())
  {
    for (auto i= ctx.old_n_v_cols; i--; )
    {
      ctx.old_v_cols[i].~dict_v_col_t();
      const_cast<unsigned&>(ctx.old_n_v_cols)= 0;
    }
  }

  const table_id_t id= table->id;
  table->release();
  dict_sys.remove(table);
  return dict_table_open_on_id(id, true, DICT_TABLE_OP_NORMAL);
}

/** Commit the changes made during prepare_inplace_alter_table()
and inplace_alter_table() inside the data dictionary tables,
when not rebuilding the table.
@param ha_alter_info Data used during in-place alter
@param ctx In-place ALTER TABLE context
@param old_table MySQL table as it is before the ALTER operation
@param trx Data dictionary transaction
@param table_name Table name in MySQL
@retval true Failure
@retval false Success
*/
inline MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
commit_try_norebuild(
/*=================*/
	Alter_inplace_info*	ha_alter_info,
	ha_innobase_inplace_ctx*ctx,
	TABLE*			altered_table,
	const TABLE*		old_table,
	trx_t*			trx,
	const char*		table_name)
{
	DBUG_ENTER("commit_try_norebuild");
	DBUG_ASSERT(!ctx->need_rebuild());
	DBUG_ASSERT(trx->dict_operation_lock_mode);
	DBUG_ASSERT(!(ha_alter_info->handler_flags
		      & ALTER_DROP_FOREIGN_KEY)
		    || ctx->num_to_drop_fk > 0);
	DBUG_ASSERT(ctx->num_to_drop_fk
		    <= ha_alter_info->alter_info->drop_list.elements
		    || ctx->num_to_drop_vcol
		       == ha_alter_info->alter_info->drop_list.elements);

	if (ctx->page_compression_level
	    && innobase_page_compression_try(ctx->page_compression_level,
					     ctx->new_table, trx,
					     table_name)) {
		DBUG_RETURN(true);
	}

	for (ulint i = 0; i < ctx->num_to_add_index; i++) {
		dict_index_t*	index = ctx->add_index[i];
		DBUG_ASSERT(dict_index_get_online_status(index)
			    == ONLINE_INDEX_COMPLETE);
		DBUG_ASSERT(!index->is_committed());
		if (index->is_corrupted()) {
			/* Report a duplicate key
			error for the index that was
			flagged corrupted, most likely
			because a duplicate value was
			inserted (directly or by
			rollback) after
			ha_innobase::inplace_alter_table()
			completed.
			TODO: report this as a corruption
			with a detailed reason once
			WL#6379 has been implemented. */
			my_error(ER_DUP_UNKNOWN_IN_INDEX,
				 MYF(0), index->name());
			DBUG_RETURN(true);
		}
	}

	if (innobase_update_foreign_try(ctx, trx, table_name)) {
		DBUG_RETURN(true);
	}

	if ((ha_alter_info->handler_flags & ALTER_COLUMN_UNVERSIONED)
	    && vers_change_fields_try(ha_alter_info, ctx, trx, old_table)) {
		DBUG_RETURN(true);
	}

	dberr_t	error = DB_SUCCESS;
	dict_index_t* index;
	const char *op = "rename index to add";
	ulint num_fts_index = 0;

	/* We altered the table in place. Mark the indexes as committed. */
	for (ulint i = 0; i < ctx->num_to_add_index; i++) {
		index = ctx->add_index[i];
		DBUG_ASSERT(dict_index_get_online_status(index)
			    == ONLINE_INDEX_COMPLETE);
		DBUG_ASSERT(!index->is_committed());
		error = row_merge_rename_index_to_add(
			trx, ctx->new_table->id, index->id);
		if (error) {
			goto handle_error;
		}
	}

	for (dict_index_t *index = UT_LIST_GET_FIRST(ctx->old_table->indexes);
	     index; index = UT_LIST_GET_NEXT(indexes, index)) {
		if (index->type & DICT_FTS) {
			num_fts_index++;
		}
	}

	char db[MAX_DB_UTF8_LEN], table[MAX_TABLE_UTF8_LEN];
	if (ctx->num_to_drop_index) {
		dict_fs2utf8(ctx->old_table->name.m_name,
			     db, sizeof db, table, sizeof table);
	}

	for (ulint i = 0; i < ctx->num_to_drop_index; i++) {
		index = ctx->drop_index[i];
		DBUG_ASSERT(index->is_committed());
		DBUG_ASSERT(index->table == ctx->new_table);
		DBUG_ASSERT(index->to_be_dropped);
		op = "DROP INDEX";

		static const char drop_index[] =
			"PROCEDURE DROP_INDEX_PROC () IS\n"
			"BEGIN\n"
			"DELETE FROM SYS_FIELDS WHERE INDEX_ID=:indexid;\n"
			"DELETE FROM SYS_INDEXES WHERE ID=:indexid;\n"
			"END;\n";

		pars_info_t* info = pars_info_create();
		pars_info_add_ull_literal(info, "indexid", index->id);
		error = que_eval_sql(info, drop_index, trx);

		if (error == DB_SUCCESS && index->type & DICT_FTS) {
			DBUG_ASSERT(index->table->fts);
			DEBUG_SYNC_C("norebuild_fts_drop");
			error = fts_drop_index(index->table, index, trx);
			ut_ad(num_fts_index);
			num_fts_index--;
		}

		if (error != DB_SUCCESS) {
			goto handle_error;
		}

		error = dict_stats_delete_from_index_stats(db, table,
							   index->name, trx);
		switch (error) {
		case DB_SUCCESS:
		case DB_STATS_DO_NOT_EXIST:
			continue;
		default:
			goto handle_error;
		}
	}

	if (const size_t size = ha_alter_info->rename_keys.size()) {
		char tmp_name[5];
		char db[MAX_DB_UTF8_LEN], table[MAX_TABLE_UTF8_LEN];

		dict_fs2utf8(ctx->new_table->name.m_name, db, sizeof db,
			     table, sizeof table);
		tmp_name[0]= (char)0xff;
		for (size_t i = 0; error == DB_SUCCESS && i < size; i++) {
			snprintf(tmp_name+1, sizeof(tmp_name)-1, "%zu", i);
			error = dict_stats_rename_index(db, table,
							ha_alter_info->
							rename_keys[i].
							old_key->name.str,
							tmp_name, trx);
		}
		for (size_t i = 0; error == DB_SUCCESS && i < size; i++) {
			snprintf(tmp_name+1, sizeof(tmp_name)-1, "%zu", i);
			error = dict_stats_rename_index(db, table, tmp_name,
							ha_alter_info
							->rename_keys[i].
							new_key->name.str,
							trx);
		}

		switch (error) {
		case DB_SUCCESS:
		case DB_STATS_DO_NOT_EXIST:
			break;
		case DB_DUPLICATE_KEY:
			my_error(ER_DUP_KEY, MYF(0),
				 "mysql.innodb_index_stats");
			DBUG_RETURN(true);
		default:
			goto handle_error;
		}
	}

	if ((ctx->old_table->flags2 & DICT_TF2_FTS) && !num_fts_index) {
		error = fts_drop_tables(trx, *ctx->old_table);
		if (error != DB_SUCCESS) {
handle_error:
			switch (error) {
			case DB_TOO_MANY_CONCURRENT_TRXS:
				my_error(ER_TOO_MANY_CONCURRENT_TRXS, MYF(0));
				break;
			case DB_LOCK_WAIT_TIMEOUT:
				my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
				break;
			default:
				sql_print_error("InnoDB: %s: %s\n", op,
						ut_strerr(error));
				DBUG_ASSERT(0);
				my_error(ER_INTERNAL_ERROR, MYF(0), op);
			}

			DBUG_RETURN(true);
		}
	}

	if (innobase_rename_or_enlarge_columns_try(ha_alter_info, ctx,
						   altered_table, old_table,
						   trx, table_name)) {
		DBUG_RETURN(true);
	}

	if ((ha_alter_info->handler_flags & ALTER_RENAME_INDEX)
	    && rename_indexes_try(ctx, ha_alter_info, trx)) {
		DBUG_RETURN(true);
	}

	if (ctx->is_instant()) {
		DBUG_RETURN(innobase_instant_try(ha_alter_info, ctx,
						 altered_table, old_table,
						 trx));
	}

	if (ha_alter_info->handler_flags
	    & (ALTER_DROP_VIRTUAL_COLUMN | ALTER_ADD_VIRTUAL_COLUMN)) {
		if ((ha_alter_info->handler_flags & ALTER_DROP_VIRTUAL_COLUMN)
		    && innobase_drop_virtual_try(ha_alter_info, ctx->old_table,
						 trx)) {
			DBUG_RETURN(true);
		}

		if ((ha_alter_info->handler_flags & ALTER_ADD_VIRTUAL_COLUMN)
		    && innobase_add_virtual_try(ha_alter_info, ctx->old_table,
						trx)) {
			DBUG_RETURN(true);
		}

		unsigned n_col = ctx->old_table->n_cols
			- DATA_N_SYS_COLS;
		unsigned n_v_col = ctx->old_table->n_v_cols
			+ ctx->num_to_add_vcol - ctx->num_to_drop_vcol;

		if (innodb_update_cols(
			    ctx->old_table,
			    dict_table_encode_n_col(n_col, n_v_col)
			    | unsigned(ctx->old_table->flags & DICT_TF_COMPACT)
			    << 31, trx)) {
			DBUG_RETURN(true);
		}
	}

	DBUG_RETURN(false);
}

/** Commit the changes to the data dictionary cache
after a successful commit_try_norebuild() call.
@param ha_alter_info algorithm=inplace context
@param ctx In-place ALTER TABLE context for the current partition
@param altered_table the TABLE after the ALTER
@param table the TABLE before the ALTER
@param trx Data dictionary transaction
(will be started and committed, for DROP INDEX)
@return whether all replacements were found for dropped indexes */
inline MY_ATTRIBUTE((nonnull))
bool
commit_cache_norebuild(
/*===================*/
	Alter_inplace_info*	ha_alter_info,
	ha_innobase_inplace_ctx*ctx,
	const TABLE*		altered_table,
	const TABLE*		table,
	trx_t*			trx)
{
	DBUG_ENTER("commit_cache_norebuild");
	DBUG_ASSERT(!ctx->need_rebuild());
	DBUG_ASSERT(ctx->new_table->space != fil_system.temp_space);
	DBUG_ASSERT(!ctx->new_table->is_temporary());

	bool found = true;

	if (ctx->page_compression_level) {
		DBUG_ASSERT(ctx->new_table->space != fil_system.sys_space);
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wconversion" /* GCC 4 and 5 need this here */
#endif
		ctx->new_table->flags
			= static_cast<uint16_t>(
				(ctx->new_table->flags
				 & ~(0xFU
				     << DICT_TF_POS_PAGE_COMPRESSION_LEVEL))
				| 1 << DICT_TF_POS_PAGE_COMPRESSION
				| (ctx->page_compression_level & 0xF)
				<< DICT_TF_POS_PAGE_COMPRESSION_LEVEL)
			& ((1U << DICT_TF_BITS) - 1);
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic pop
#endif

		if (fil_space_t* space = ctx->new_table->space) {
			bool update = !(space->flags
					& FSP_FLAGS_MASK_PAGE_COMPRESSION);
			mysql_mutex_lock(&fil_system.mutex);
			space->flags &= ~FSP_FLAGS_MASK_MEM_COMPRESSION_LEVEL;
			space->flags |= ctx->page_compression_level
				<< FSP_FLAGS_MEM_COMPRESSION_LEVEL;
			if (!space->full_crc32()) {
				space->flags
					|= FSP_FLAGS_MASK_PAGE_COMPRESSION;
			} else if (!space->is_compressed()) {
				space->flags |= static_cast<uint32_t>(
					innodb_compression_algorithm)
					<< FSP_FLAGS_FCRC32_POS_COMPRESSED_ALGO;
			}
			mysql_mutex_unlock(&fil_system.mutex);

			if (update) {
				/* Maybe we should introduce an undo
				log record for updating tablespace
				flags, and perform the update already
				in innobase_page_compression_try().

				If the server is killed before the
				following mini-transaction commit
				becomes durable, fsp_flags_try_adjust()
				will perform the equivalent adjustment
				and warn "adjusting FSP_SPACE_FLAGS". */
				mtr_t	mtr;
				mtr.start();
				if (buf_block_t* b = buf_page_get(
					    page_id_t(space->id, 0),
					    space->zip_size(),
					    RW_X_LATCH, &mtr)) {
					byte* f = FSP_HEADER_OFFSET
						+ FSP_SPACE_FLAGS
						+ b->page.frame;
					const auto sf = space->flags
						& ~FSP_FLAGS_MEM_MASK;
					if (mach_read_from_4(f) != sf) {
						mtr.set_named_space(space);
						mtr.write<4,mtr_t::FORCED>(
							*b, f, sf);
					}
				}
				mtr.commit();
			}
		}
	}

	col_set			drop_list;
	col_set			v_drop_list;

	/* Check if the column, part of an index to be dropped is part of any
	other index which is not being dropped. If it so, then set the ord_part
	of the column to 0. */
	collect_columns_from_dropped_indexes(ctx, drop_list, v_drop_list);

	for (ulint col : drop_list) {
		if (!check_col_exists_in_indexes(ctx->new_table, col, false)) {
			ctx->new_table->cols[col].ord_part = 0;
		}
	}

	for (ulint col : v_drop_list) {
		if (!check_col_exists_in_indexes(ctx->new_table, col, true)) {
			ctx->new_table->v_cols[col].m_col.ord_part = 0;
		}
	}

	for (ulint i = 0; i < ctx->num_to_add_index; i++) {
		dict_index_t*	index = ctx->add_index[i];
		DBUG_ASSERT(dict_index_get_online_status(index)
			    == ONLINE_INDEX_COMPLETE);
		DBUG_ASSERT(!index->is_committed());
		index->set_committed(true);
	}

	for (ulint i = 0; i < ctx->num_to_drop_index; i++) {
		dict_index_t*	index = ctx->drop_index[i];
		DBUG_ASSERT(index->is_committed());
		DBUG_ASSERT(index->table == ctx->new_table);
		DBUG_ASSERT(index->to_be_dropped);

		if (!dict_foreign_replace_index(index->table, ctx->col_names,
						index)) {
			found = false;
		}

		dict_index_remove_from_cache(index->table, index);
	}

	fts_clear_all(ctx->old_table);

	if (!ctx->is_instant()) {
		innobase_rename_or_enlarge_columns_cache(
			ha_alter_info, altered_table, table, ctx->new_table);
	} else {
		ut_ad(ctx->col_map);

		if (fts_t* fts = ctx->new_table->fts) {
			ut_ad(fts->doc_col != ULINT_UNDEFINED);
			ut_ad(ctx->new_table->n_cols > DATA_N_SYS_COLS);
			const ulint c = ctx->col_map[fts->doc_col];
			ut_ad(c < ulint(ctx->new_table->n_cols)
			      - DATA_N_SYS_COLS);
			ut_d(const dict_col_t& col = ctx->new_table->cols[c]);
			ut_ad(!col.is_nullable());
			ut_ad(!col.is_virtual());
			ut_ad(!col.is_added());
			ut_ad(col.prtype & DATA_UNSIGNED);
			ut_ad(col.mtype == DATA_INT);
			ut_ad(col.len == 8);
			ut_ad(col.ord_part);
			fts->doc_col = c;
		}

		if (ha_alter_info->handler_flags & ALTER_DROP_STORED_COLUMN) {
			const dict_index_t* index = ctx->new_table->indexes.start;

			for (const dict_field_t* f = index->fields,
				     * const end = f + index->n_fields;
			     f != end; f++) {
				dict_col_t& c = *f->col;
				if (c.is_dropped()) {
					c.set_dropped(!c.is_nullable(),
						      DATA_LARGE_MTYPE(c.mtype)
						      || (!f->fixed_len
							  && c.len > 255),
						      f->fixed_len);
				}
			}
		}

		if (!ctx->instant_table->persistent_autoinc) {
			ctx->new_table->persistent_autoinc = 0;
		}
	}

	if (ha_alter_info->handler_flags & ALTER_COLUMN_UNVERSIONED) {
		vers_change_fields_cache(ha_alter_info, ctx, table);
	}

	if (ha_alter_info->handler_flags & ALTER_RENAME_INDEX) {
		innobase_rename_indexes_cache(ctx, ha_alter_info);
	}

	ctx->new_table->fts_doc_id_index
		= ctx->new_table->fts
		? dict_table_get_index_on_name(
			ctx->new_table, FTS_DOC_ID_INDEX_NAME)
		: NULL;
	DBUG_ASSERT((ctx->new_table->fts == NULL)
		    == (ctx->new_table->fts_doc_id_index == NULL));
	if (table->found_next_number_field
		&& !altered_table->found_next_number_field) {
		ctx->prebuilt->table->persistent_autoinc = 0;
	}
	DBUG_RETURN(found);
}

/** Adjust the persistent statistics after non-rebuilding ALTER TABLE.
Remove statistics for dropped indexes, add statistics for created indexes
and rename statistics for renamed indexes.
@param ha_alter_info Data used during in-place alter
@param ctx In-place ALTER TABLE context
@param thd MySQL connection
*/
static
void
alter_stats_norebuild(
/*==================*/
	Alter_inplace_info*		ha_alter_info,
	ha_innobase_inplace_ctx*	ctx,
	THD*				thd)
{
	DBUG_ENTER("alter_stats_norebuild");
	DBUG_ASSERT(!ctx->need_rebuild());

	if (!dict_stats_is_persistent_enabled(ctx->new_table)) {
		DBUG_VOID_RETURN;
	}

	for (ulint i = 0; i < ctx->num_to_add_index; i++) {
		dict_index_t*	index = ctx->add_index[i];
		DBUG_ASSERT(index->table == ctx->new_table);

		if (!(index->type & DICT_FTS)) {
			dict_stats_init(ctx->new_table);
			dict_stats_update_for_index(index);
		}
	}

	DBUG_VOID_RETURN;
}

/** Adjust the persistent statistics after rebuilding ALTER TABLE.
Remove statistics for dropped indexes, add statistics for created indexes
and rename statistics for renamed indexes.
@param table InnoDB table that was rebuilt by ALTER TABLE
@param table_name Table name in MySQL
@param thd MySQL connection
*/
static
void
alter_stats_rebuild(
/*================*/
	dict_table_t*	table,
	const char*	table_name,
	THD*		thd)
{
	DBUG_ENTER("alter_stats_rebuild");

	if (!table->space
	    || !dict_stats_is_persistent_enabled(table)) {
		DBUG_VOID_RETURN;
	}

	dberr_t	ret = dict_stats_update(table, DICT_STATS_RECALC_PERSISTENT);

	if (ret != DB_SUCCESS) {
		push_warning_printf(
			thd,
			Sql_condition::WARN_LEVEL_WARN,
			ER_ALTER_INFO,
			"Error updating stats for table '%s'"
			" after table rebuild: %s",
			table_name, ut_strerr(ret));
	}

	DBUG_VOID_RETURN;
}

/** Apply the log for the table rebuild operation.
@param[in]	ctx		Inplace Alter table context
@param[in]	altered_table	MySQL table that is being altered
@return true Failure, else false. */
static bool alter_rebuild_apply_log(
	ha_innobase_inplace_ctx*	ctx,
	Alter_inplace_info*		ha_alter_info,
	TABLE*				altered_table)
{
	DBUG_ENTER("alter_rebuild_apply_log");

	if (!ctx->online) {
		DBUG_RETURN(false);
	}

	/* We copied the table. Any indexes that were requested to be
	dropped were not created in the copy of the table. Apply any
	last bit of the rebuild log and then rename the tables. */
	dict_table_t*	user_table = ctx->old_table;

	DEBUG_SYNC_C("row_log_table_apply2_before");

	dict_vcol_templ_t* s_templ  = NULL;

	if (ctx->new_table->n_v_cols > 0) {
		s_templ = UT_NEW_NOKEY(
				dict_vcol_templ_t());
		s_templ->vtempl = NULL;

		innobase_build_v_templ(altered_table, ctx->new_table, s_templ,
				       NULL, true);
		ctx->new_table->vc_templ = s_templ;
	}

	dberr_t error = row_log_table_apply(
		ctx->thr, user_table, altered_table,
		static_cast<ha_innobase_inplace_ctx*>(
			ha_alter_info->handler_ctx)->m_stage,
		ctx->new_table);

	if (s_templ) {
		ut_ad(ctx->need_rebuild());
		dict_free_vc_templ(s_templ);
		UT_DELETE(s_templ);
		ctx->new_table->vc_templ = NULL;
	}

	DBUG_RETURN(ctx->log_failure(
			ha_alter_info, altered_table, error));
}

/** Commit or rollback the changes made during
prepare_inplace_alter_table() and inplace_alter_table() inside
the storage engine. Note that the allowed level of concurrency
during this operation will be the same as for
inplace_alter_table() and thus might be higher than during
prepare_inplace_alter_table(). (E.g concurrent writes were
blocked during prepare, but might not be during commit).
@param altered_table TABLE object for new version of table.
@param ha_alter_info Structure describing changes to be done
by ALTER TABLE and holding data used during in-place alter.
@param commit true => Commit, false => Rollback.
@retval true Failure
@retval false Success
*/

bool
ha_innobase::commit_inplace_alter_table(
/*====================================*/
	TABLE*			altered_table,
	Alter_inplace_info*	ha_alter_info,
	bool			commit)
{
	ha_innobase_inplace_ctx*ctx0;

	ctx0 = static_cast<ha_innobase_inplace_ctx*>
		(ha_alter_info->handler_ctx);

#ifndef DBUG_OFF
	uint	failure_inject_count	= 1;
#endif /* DBUG_OFF */

	DBUG_ENTER("commit_inplace_alter_table");
	DBUG_ASSERT(!srv_read_only_mode);
	DBUG_ASSERT(!ctx0 || ctx0->prebuilt == m_prebuilt);
	DBUG_ASSERT(!ctx0 || ctx0->old_table == m_prebuilt->table);

	DEBUG_SYNC_C("innodb_commit_inplace_alter_table_enter");

	DEBUG_SYNC_C("innodb_commit_inplace_alter_table_wait");

	if (ctx0 != NULL && ctx0->m_stage != NULL) {
		ctx0->m_stage->begin_phase_end();
	}

	if (!commit) {
		/* A rollback is being requested. So far we may at
		most have created stubs for ADD INDEX or a copy of the
		table for rebuild. */
		DBUG_RETURN(rollback_inplace_alter_table(
				    ha_alter_info, table, m_prebuilt));
	}

	if (!(ha_alter_info->handler_flags & ~INNOBASE_INPLACE_IGNORE)) {
		DBUG_ASSERT(!ctx0);
		MONITOR_ATOMIC_DEC(MONITOR_PENDING_ALTER_TABLE);
		if (table->found_next_number_field
			&& !altered_table->found_next_number_field) {
			m_prebuilt->table->persistent_autoinc = 0;
			/* Don't reset ha_alter_info->group_commit_ctx to make
			partitions engine to call this function for all
			partitions. */
		}
		else
			ha_alter_info->group_commit_ctx = NULL;
		DBUG_RETURN(false);
	}

	DBUG_ASSERT(ctx0);

	inplace_alter_handler_ctx**	ctx_array;
	inplace_alter_handler_ctx*	ctx_single[2];

	if (ha_alter_info->group_commit_ctx) {
		ctx_array = ha_alter_info->group_commit_ctx;
	} else {
		ctx_single[0] = ctx0;
		ctx_single[1] = NULL;
		ctx_array = ctx_single;
	}

	DBUG_ASSERT(ctx0 == ctx_array[0]);
	ut_ad(m_prebuilt->table == ctx0->old_table);
	ha_alter_info->group_commit_ctx = NULL;

	const bool new_clustered = ctx0->need_rebuild();
	trx_t* const trx = ctx0->trx;
	trx->op_info = "acquiring table lock";
	bool fts_exist = false;
	for (inplace_alter_handler_ctx** pctx = ctx_array; *pctx; pctx++) {
		auto ctx = static_cast<ha_innobase_inplace_ctx*>(*pctx);
		DBUG_ASSERT(ctx->prebuilt->trx == m_prebuilt->trx);
		ut_ad(m_prebuilt != ctx->prebuilt || ctx == ctx0);
		DBUG_ASSERT(new_clustered == ctx->need_rebuild());
		/* If decryption failed for old table or new table
		fail here. */
		if ((!ctx->old_table->is_readable()
		     && ctx->old_table->space)
		    || (!ctx->new_table->is_readable()
			&& ctx->new_table->space)) {
			String str;
			const char* engine= table_type();
			get_error_message(HA_ERR_DECRYPTION_FAILED, &str);
			my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_DECRYPTION_FAILED, str.c_ptr(), engine);
			DBUG_RETURN(true);
		}
		if ((ctx->old_table->flags2 | ctx->new_table->flags2)
		    & (DICT_TF2_FTS_HAS_DOC_ID | DICT_TF2_FTS)) {
			fts_exist = true;
		}
	}

	bool already_stopped= false;
	for (inplace_alter_handler_ctx** pctx = ctx_array; *pctx; pctx++) {
		auto ctx = static_cast<ha_innobase_inplace_ctx*>(*pctx);
		dberr_t error = DB_SUCCESS;

		if (fts_exist) {
			purge_sys.stop_FTS(*ctx->old_table, already_stopped);
			already_stopped = true;
		}

		if (new_clustered && ctx->old_table->fts) {
			ut_ad(!ctx->old_table->fts->add_wq);
			fts_optimize_remove_table(ctx->old_table);
		}

		dict_sys.freeze(SRW_LOCK_CALL);
		for (auto f : ctx->old_table->referenced_set) {
			if (dict_table_t* child = f->foreign_table) {
				error = lock_table_for_trx(child, trx, LOCK_X);
				if (error != DB_SUCCESS) {
					break;
				}
			}
		}
		dict_sys.unfreeze();

		if (ctx->new_table->fts) {
			ut_ad(!ctx->new_table->fts->add_wq);
			fts_optimize_remove_table(ctx->new_table);
			fts_sync_during_ddl(ctx->new_table);
		}

		/* Exclusively lock the table, to ensure that no other
		transaction is holding locks on the table while we
		change the table definition. Any recovered incomplete
		transactions would be holding InnoDB locks only, not MDL. */
		if (error == DB_SUCCESS) {
			error = lock_table_for_trx(ctx->new_table, trx,
						   LOCK_X);
		}

		DBUG_EXECUTE_IF("deadlock_table_fail",
				{
				  error= DB_DEADLOCK;
				  trx_rollback_for_mysql(trx);
				});

		if (error != DB_SUCCESS) {
lock_fail:
			my_error_innodb(
				error, table_share->table_name.str, 0);
			if (fts_exist) {
				purge_sys.resume_FTS();
			}

			/* Deadlock encountered and rollbacked the
			transaction. So restart the transaction
			to remove the newly created table or
			index from data dictionary and table cache
			in rollback_inplace_alter_table() */
			if (trx->state == TRX_STATE_NOT_STARTED) {
				trx_start_for_ddl(trx);
			}

			DBUG_RETURN(true);
		} else if ((ctx->new_table->flags2
			    & (DICT_TF2_FTS_HAS_DOC_ID | DICT_TF2_FTS))
			   && (error = fts_lock_tables(trx, *ctx->new_table))
			   != DB_SUCCESS) {
			goto lock_fail;
		} else if (!new_clustered) {
		} else if ((error = lock_table_for_trx(ctx->old_table, trx,
						       LOCK_X))
			   != DB_SUCCESS) {
			goto lock_fail;
		} else if ((ctx->old_table->flags2
			    & (DICT_TF2_FTS_HAS_DOC_ID | DICT_TF2_FTS))
			   && (error = fts_lock_tables(trx, *ctx->old_table))
			   != DB_SUCCESS) {
			goto lock_fail;
		}
	}

	DEBUG_SYNC(m_user_thd, "innodb_alter_commit_after_lock_table");

	if (new_clustered) {
		/* We are holding MDL_EXCLUSIVE as well as exclusive
		InnoDB table locks. Let us apply any table rebuild log
		before locking dict_sys. */
		for (inplace_alter_handler_ctx** pctx= ctx_array; *pctx;
		     pctx++) {
			auto ctx= static_cast<ha_innobase_inplace_ctx*>(*pctx);
			DBUG_ASSERT(ctx->need_rebuild());
			if (alter_rebuild_apply_log(ctx, ha_alter_info,
						    altered_table)) {
				if (fts_exist) {
					purge_sys.resume_FTS();
				}
				DBUG_RETURN(true);
			}
		}
	} else {
		dberr_t error= DB_SUCCESS;
		for (inplace_alter_handler_ctx** pctx= ctx_array; *pctx;
		     pctx++) {
			auto ctx= static_cast<ha_innobase_inplace_ctx*>(*pctx);

			if (!ctx->online || !ctx->old_table->space
			    || !ctx->old_table->is_readable()) {
				continue;
			}

			for (ulint i = 0; i < ctx->num_to_add_index; i++) {
				dict_index_t *index= ctx->add_index[i];

				ut_ad(!(index->type &
					(DICT_FTS | DICT_SPATIAL)));

				index->lock.x_lock(SRW_LOCK_CALL);
				if (!index->online_log) {
					/* online log would've cleared
					when we detect the error in
					other index */
					index->lock.x_unlock();
					continue;
				}

				if (index->is_corrupted()) {
					/* Online index log has been
					preserved to show the error
					when it happened via
					row_log_apply() by DML thread */
					error= row_log_get_error(index);
err_index:
					ut_ad(error != DB_SUCCESS);
					ctx->log_failure(
						ha_alter_info,
						altered_table, error);
					row_log_free(index->online_log);
					index->online_log= nullptr;
					index->lock.x_unlock();

					ctx->old_table->indexes.start
						->online_log= nullptr;
					if (fts_exist) {
						purge_sys.resume_FTS();
					}
					MONITOR_ATOMIC_INC(
						MONITOR_BACKGROUND_DROP_INDEX);
					DBUG_RETURN(true);
				}

				index->lock.x_unlock();

				error = row_log_apply(
					m_prebuilt->trx, index, altered_table,
					ctx->m_stage);

				index->lock.x_lock(SRW_LOCK_CALL);

				if (error != DB_SUCCESS) {
					goto err_index;
				}

				row_log_free(index->online_log);
				index->online_log= nullptr;
				index->lock.x_unlock();
			}

			ctx->old_table->indexes.start->online_log= nullptr;
		}
	}

	dict_table_t *table_stats = nullptr, *index_stats = nullptr;
	MDL_ticket *mdl_table = nullptr, *mdl_index = nullptr;
	dberr_t error = DB_SUCCESS;
	if (!ctx0->old_table->is_stats_table() &&
	    !ctx0->new_table->is_stats_table()) {
		table_stats = dict_table_open_on_name(
			TABLE_STATS_NAME, false, DICT_ERR_IGNORE_NONE);
		if (table_stats) {
			dict_sys.freeze(SRW_LOCK_CALL);
			table_stats = dict_acquire_mdl_shared<false>(
				table_stats, m_user_thd, &mdl_table);
			dict_sys.unfreeze();
		}
		index_stats = dict_table_open_on_name(
			INDEX_STATS_NAME, false, DICT_ERR_IGNORE_NONE);
		if (index_stats) {
			dict_sys.freeze(SRW_LOCK_CALL);
			index_stats = dict_acquire_mdl_shared<false>(
				index_stats, m_user_thd, &mdl_index);
			dict_sys.unfreeze();
		}

		if (table_stats && index_stats
		    && !strcmp(table_stats->name.m_name, TABLE_STATS_NAME)
		    && !strcmp(index_stats->name.m_name, INDEX_STATS_NAME)
		    && !(error = lock_table_for_trx(table_stats,
						    trx, LOCK_X))) {
			error = lock_table_for_trx(index_stats, trx, LOCK_X);
		}
	}

	DBUG_EXECUTE_IF("stats_lock_fail",
			error = DB_LOCK_WAIT;);

	if (error == DB_SUCCESS) {
		error = lock_sys_tables(trx);
	}
	if (error != DB_SUCCESS) {
		if (table_stats) {
			dict_table_close(table_stats, false, m_user_thd,
					 mdl_table);
		}
		if (index_stats) {
			dict_table_close(index_stats, false, m_user_thd,
					 mdl_index);
		}
		my_error_innodb(error, table_share->table_name.str, 0);
		if (fts_exist) {
			purge_sys.resume_FTS();
		}
		DBUG_RETURN(true);
	}

	row_mysql_lock_data_dictionary(trx);

	/* Apply the changes to the data dictionary tables, for all
	partitions. */
	for (inplace_alter_handler_ctx** pctx = ctx_array; *pctx; pctx++) {
		auto ctx = static_cast<ha_innobase_inplace_ctx*>(*pctx);

		DBUG_ASSERT(new_clustered == ctx->need_rebuild());
		if (ctx->need_rebuild() && !ctx->old_table->space) {
			my_error(ER_TABLESPACE_DISCARDED, MYF(0),
				 table->s->table_name.str);
fail:
			trx->rollback();
			ut_ad(!trx->fts_trx);
			if (table_stats) {
				dict_table_close(table_stats, true, m_user_thd,
						 mdl_table);
			}
			if (index_stats) {
				dict_table_close(index_stats, true, m_user_thd,
						 mdl_index);
			}
			row_mysql_unlock_data_dictionary(trx);
			if (fts_exist) {
				purge_sys.resume_FTS();
			}
			trx_start_for_ddl(trx);
			DBUG_RETURN(true);
		}

		if (commit_set_autoinc(ha_alter_info, ctx,
				       altered_table, table)) {
			goto fail;
		}

		if (ctx->need_rebuild()) {
			ctx->tmp_name = dict_mem_create_temporary_tablename(
				ctx->heap, ctx->new_table->name.m_name,
				ctx->new_table->id);

			if (commit_try_rebuild(ha_alter_info, ctx,
					       altered_table, table,
					       trx,
					       table_share->table_name.str)) {
				goto fail;
			}
		} else if (commit_try_norebuild(ha_alter_info, ctx,
						altered_table, table, trx,
						table_share->table_name.str)) {
			goto fail;
		}
#ifndef DBUG_OFF
		{
			/* Generate a dynamic dbug text. */
			char buf[32];

			snprintf(buf, sizeof buf,
				    "ib_commit_inplace_fail_%u",
				    failure_inject_count++);

			DBUG_EXECUTE_IF(buf,
					my_error(ER_INTERNAL_ERROR, MYF(0),
						 "Injected error!");
					goto fail;
			);
		}
#endif
	}

	if (table_stats) {
		dict_table_close(table_stats, true, m_user_thd, mdl_table);
	}
	if (index_stats) {
		dict_table_close(index_stats, true, m_user_thd, mdl_index);
	}

	/* Commit or roll back the changes to the data dictionary. */
	DEBUG_SYNC(m_user_thd, "innodb_alter_inplace_before_commit");

	if (new_clustered) {
		ut_ad(trx->has_logged());
		for (inplace_alter_handler_ctx** pctx = ctx_array; *pctx;
		     pctx++) {
			auto ctx= static_cast<ha_innobase_inplace_ctx*>(*pctx);
			ut_ad(!strcmp(ctx->old_table->name.m_name,
				      ctx->tmp_name));
			ut_ad(ctx->new_table->get_ref_count() == 1);
			const bool own = m_prebuilt == ctx->prebuilt;
			trx_t* const user_trx = m_prebuilt->trx;
			ctx->prebuilt->table->release();
			ctx->prebuilt->table = nullptr;
			row_prebuilt_free(ctx->prebuilt);
			/* Rebuild the prebuilt object. */
			ctx->prebuilt = row_create_prebuilt(
				ctx->new_table, altered_table->s->reclength);
			if (own) {
				m_prebuilt = ctx->prebuilt;
			}
			trx_start_if_not_started(user_trx, true);
			m_prebuilt->trx = user_trx;
		}
	}

	ut_ad(!trx->fts_trx);

	std::vector<pfs_os_file_t> deleted;
	DBUG_EXECUTE_IF("innodb_alter_commit_crash_before_commit",
			log_buffer_flush_to_disk(); DBUG_SUICIDE(););
	/* The SQL layer recovery of ALTER TABLE will invoke
	innodb_check_version() to know whether our trx->id, which we
	reported via ha_innobase::table_version() after
	ha_innobase::prepare_inplace_alter_table(), was committed.

	If this trx was committed (the log write below completed),
	we will be able to recover our trx->id to
	dict_table_t::def_trx_id from the data dictionary tables.

	For this logic to work, purge_sys.stop_SYS() and
	purge_sys.resume_SYS() will ensure that the DB_TRX_ID that we
	wrote to the SYS_ tables will be preserved until the SQL layer
	has durably marked the ALTER TABLE operation as completed.

	During recovery, the purge of InnoDB transaction history will
	not start until innodb_ddl_recovery_done(). */
	ha_alter_info->inplace_alter_table_committed = purge_sys.resume_SYS;
	purge_sys.stop_SYS();
	trx->commit(deleted);

	/* At this point, the changes to the persistent storage have
	been committed or rolled back. What remains to be done is to
	update the in-memory structures, close some handles, release
	temporary files, and (unless we rolled back) update persistent
	statistics. */
	for (inplace_alter_handler_ctx** pctx = ctx_array;
	     *pctx; pctx++) {
		ha_innobase_inplace_ctx*	ctx
			= static_cast<ha_innobase_inplace_ctx*>(*pctx);

		DBUG_ASSERT(ctx->need_rebuild() == new_clustered);

		innobase_copy_frm_flags_from_table_share(
			ctx->new_table, altered_table->s);

		if (new_clustered) {
			DBUG_PRINT("to_be_dropped",
				   ("table: %s", ctx->old_table->name.m_name));

			if (innobase_update_foreign_cache(ctx, m_user_thd)
			    != DB_SUCCESS
			    && m_prebuilt->trx->check_foreigns) {
foreign_fail:
				push_warning_printf(
					m_user_thd,
					Sql_condition::WARN_LEVEL_WARN,
					ER_ALTER_INFO,
					"failed to load FOREIGN KEY"
					" constraints");
			}
		} else {
			bool fk_fail = innobase_update_foreign_cache(
				ctx, m_user_thd) != DB_SUCCESS;

			if (!commit_cache_norebuild(ha_alter_info, ctx,
						    altered_table, table,
						    trx)) {
				fk_fail = true;
			}

			if (fk_fail && m_prebuilt->trx->check_foreigns) {
				goto foreign_fail;
			}
		}

		dict_mem_table_free_foreign_vcol_set(ctx->new_table);
		dict_mem_table_fill_foreign_vcol_set(ctx->new_table);
	}

	ut_ad(trx == ctx0->trx);
	ctx0->trx = nullptr;

	/* Free the ctx->trx of other partitions, if any. We will only
	use the ctx0->trx here. Others may have been allocated in
	the prepare stage. */

	for (inplace_alter_handler_ctx** pctx = &ctx_array[1]; *pctx;
	     pctx++) {
		ha_innobase_inplace_ctx*	ctx
			= static_cast<ha_innobase_inplace_ctx*>(*pctx);

		if (ctx->trx) {
			ctx->trx->rollback();
			ctx->trx->free();
			ctx->trx = NULL;
		}
	}

	/* MDEV-17468: Avoid this at least when ctx->is_instant().
	Currently dict_load_column_low() is the only place where
	num_base for virtual columns is assigned to nonzero. */
	if (ctx0->num_to_drop_vcol || ctx0->num_to_add_vcol
	    || (ctx0->new_table->n_v_cols && !new_clustered
		&& (ha_alter_info->alter_info->drop_list.elements
		    || ha_alter_info->alter_info->create_list.elements))
	    || (ctx0->is_instant()
		&& m_prebuilt->table->n_v_cols
		&& ha_alter_info->handler_flags & ALTER_STORED_COLUMN_ORDER)) {
		DBUG_ASSERT(ctx0->old_table->get_ref_count() == 1);
		ut_ad(ctx0->prebuilt == m_prebuilt);

		for (inplace_alter_handler_ctx** pctx = ctx_array; *pctx;
		     pctx++) {
			auto ctx= static_cast<ha_innobase_inplace_ctx*>(*pctx);
			ctx->prebuilt->table = innobase_reload_table(
				m_user_thd, ctx->prebuilt->table,
				table->s->table_name, *ctx);
			innobase_copy_frm_flags_from_table_share(
				ctx->prebuilt->table, altered_table->s);
		}

		unlock_and_close_files(deleted, trx);
		log_write_up_to(trx->commit_lsn, true);
		DBUG_EXECUTE_IF("innodb_alter_commit_crash_after_commit",
				DBUG_SUICIDE(););
		trx->free();
		if (fts_exist) {
			purge_sys.resume_FTS();
		}
		MONITOR_ATOMIC_DEC(MONITOR_PENDING_ALTER_TABLE);
		/* There is no need to reset dict_table_t::persistent_autoinc
		as the table is reloaded */
		DBUG_RETURN(false);
	}

	for (inplace_alter_handler_ctx** pctx = ctx_array;
	     *pctx; pctx++) {
		ha_innobase_inplace_ctx*	ctx
			= static_cast<ha_innobase_inplace_ctx*>
			(*pctx);
		DBUG_ASSERT(ctx->need_rebuild() == new_clustered);

		/* Publish the created fulltext index, if any.
		Note that a fulltext index can be created without
		creating the clustered index, if there already exists
		a suitable FTS_DOC_ID column. If not, one will be
		created, implying new_clustered */
		for (ulint i = 0; i < ctx->num_to_add_index; i++) {
			dict_index_t*	index = ctx->add_index[i];

			if (index->type & DICT_FTS) {
				DBUG_ASSERT(index->type == DICT_FTS);
				/* We reset DICT_TF2_FTS here because the bit
				is left unset when a drop proceeds the add. */
				DICT_TF2_FLAG_SET(ctx->new_table, DICT_TF2_FTS);
				fts_add_index(index, ctx->new_table);
			}
		}

		ut_d(dict_table_check_for_dup_indexes(
			     ctx->new_table, CHECK_ALL_COMPLETE));

		/* Start/Restart the FTS background operations. */
		if (ctx->new_table->fts) {
			fts_optimize_add_table(ctx->new_table);
		}

		ut_d(dict_table_check_for_dup_indexes(
			     ctx->new_table, CHECK_ABORTED_OK));

#ifdef UNIV_DEBUG
		if (!(ctx->new_table->fts != NULL
			&& ctx->new_table->fts->cache->sync->in_progress)) {
			ut_a(fts_check_cached_index(ctx->new_table));
		}
#endif
	}

	unlock_and_close_files(deleted, trx);
	log_write_up_to(trx->commit_lsn, true);
	DBUG_EXECUTE_IF("innodb_alter_commit_crash_after_commit",
			DBUG_SUICIDE(););
	trx->free();
	if (fts_exist) {
		purge_sys.resume_FTS();
	}

	/* TODO: The following code could be executed
	while allowing concurrent access to the table
	(MDL downgrade). */

	if (new_clustered) {
		for (inplace_alter_handler_ctx** pctx = ctx_array;
		     *pctx; pctx++) {
			ha_innobase_inplace_ctx*	ctx
				= static_cast<ha_innobase_inplace_ctx*>
				(*pctx);
			DBUG_ASSERT(ctx->need_rebuild());

			alter_stats_rebuild(
				ctx->new_table, table->s->table_name.str,
				m_user_thd);
		}
	} else {
		for (inplace_alter_handler_ctx** pctx = ctx_array;
		     *pctx; pctx++) {
			ha_innobase_inplace_ctx*	ctx
				= static_cast<ha_innobase_inplace_ctx*>
				(*pctx);
			DBUG_ASSERT(!ctx->need_rebuild());

			alter_stats_norebuild(ha_alter_info, ctx, m_user_thd);
		}
	}

	innobase_parse_hint_from_comment(
		m_user_thd, m_prebuilt->table, altered_table->s);

	/* TODO: Also perform DROP TABLE and DROP INDEX after
	the MDL downgrade. */

#ifndef DBUG_OFF
	dict_index_t* clust_index = dict_table_get_first_index(
		ctx0->prebuilt->table);
	DBUG_ASSERT(!clust_index->online_log);
	DBUG_ASSERT(dict_index_get_online_status(clust_index)
		    == ONLINE_INDEX_COMPLETE);

	for (dict_index_t* index = clust_index;
	     index;
	     index = dict_table_get_next_index(index)) {
		DBUG_ASSERT(!index->to_be_dropped);
	}
#endif /* DBUG_OFF */
	MONITOR_ATOMIC_DEC(MONITOR_PENDING_ALTER_TABLE);
	DBUG_RETURN(false);
}

/**
@param thd the session
@param start_value the lower bound
@param max_value the upper bound (inclusive) */

ib_sequence_t::ib_sequence_t(
	THD*		thd,
	ulonglong	start_value,
	ulonglong	max_value)
	:
	m_max_value(max_value),
	m_increment(0),
	m_offset(0),
	m_next_value(start_value),
	m_eof(false)
{
	if (thd != 0 && m_max_value > 0) {

		thd_get_autoinc(thd, &m_offset, &m_increment);

		if (m_increment > 1 || m_offset > 1) {

			/* If there is an offset or increment specified
			then we need to work out the exact next value. */

			m_next_value = innobase_next_autoinc(
				start_value, 1,
				m_increment, m_offset, m_max_value);

		} else if (start_value == 0) {
			/* The next value can never be 0. */
			m_next_value = 1;
		}
	} else {
		m_eof = true;
	}
}

/**
Postfix increment
@return the next value to insert */

ulonglong
ib_sequence_t::operator++(int) UNIV_NOTHROW
{
	ulonglong	current = m_next_value;

	ut_ad(!m_eof);
	ut_ad(m_max_value > 0);

	m_next_value = innobase_next_autoinc(
		current, 1, m_increment, m_offset, m_max_value);

	if (m_next_value == m_max_value && current == m_next_value) {
		m_eof = true;
	}

	return(current);
}
