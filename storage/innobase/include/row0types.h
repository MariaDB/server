/*****************************************************************************

Copyright (c) 1996, 2012, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, MariaDB Corporation.

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
@file include/row0types.h
Row operation global types

Created 12/27/1996 Heikki Tuuri
*******************************************************/

#ifndef row0types_h
#define row0types_h

struct plan_t;

struct upd_t;
struct upd_field_t;
struct upd_node_t;
struct del_node_t;
struct ins_node_t;
struct sel_node_t;
struct open_node_t;
struct fetch_node_t;

struct row_printf_node_t;
struct sel_buf_t;

struct undo_node_t;

struct purge_node_t;

struct row_ext_t;

/** Buffer for logging modifications during online index creation */
struct row_log_t;

/* MySQL data types */
struct TABLE;

/** Purge virtual column node information. */
struct purge_vcol_info_t
{
private:
	/** Is there a possible need to evaluate virtual columns? */
	bool	requested;
	/** Do we have to evaluate virtual columns (using mariadb_table)? */
	bool	used;

	/** True if it is used for the first time. */
	bool	first_use;

	/** MariaDB table opened for virtual column computation. */
	TABLE*	mariadb_table;

public:
	/** Default constructor */
	purge_vcol_info_t() :
		requested(false), used(false), first_use(false),
		mariadb_table(NULL)
	{}
	/** Reset the state. */
	void reset()
	{
		requested = false;
		used = false;
		first_use = false;
		mariadb_table = NULL;
	}

	/** Validate the virtual column information.
	@return true if the mariadb table opened successfully
	or doesn't try to calculate virtual column. */
	bool validate() const { return !used || mariadb_table; }

	/** @return the table handle for evaluating virtual columns */
	TABLE* table() const { return mariadb_table; }

	/** Set the table handle for evaluating virtual columns.
	@param[in]	table	table handle */
	void set_table(TABLE* table)
	{
		ut_ad(!table || is_first_fetch());
		mariadb_table = table;
	}

	/** Note that virtual column information may be needed. */
	void set_requested()
	{
		ut_ad(!used);
		ut_ad(!first_use);
		ut_ad(!mariadb_table);
		requested = true;
	}

	/** @return whether the virtual column information may be needed */
	bool is_requested() const { return requested; }

	/** Note that the virtual column information is needed. */
	void set_used()
	{
		ut_ad(requested);

		if (first_use) {
			first_use = false;
			ut_ad(used);
			return;
		}

		if (!used) {
			first_use = used = true;
		}
	}

	/** @return whether the virtual column information is needed */
	bool is_used() const
	{
		ut_ad(!first_use || used);
		ut_ad(!used || requested);
		ut_ad(used || !mariadb_table);
		return used;
	}

	/** Check whether it fetches mariadb table for the first time.
	@return true if first time tries to open mariadb table. */
	bool is_first_fetch() const
	{
		ut_ad(!first_use || used);
		ut_ad(!used || requested);
		ut_ad(used || !mariadb_table);
		return first_use;
	}
};

#endif
