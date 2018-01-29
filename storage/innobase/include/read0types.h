/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/read0types.h
Cursor read

Created 2/16/1997 Heikki Tuuri
*******************************************************/

#ifndef read0types_h
#define read0types_h

#include <algorithm>
#include "dict0mem.h"

#include "trx0types.h"

// Friend declaration
class MVCC;

/** Read view lists the trx ids of those transactions for which a consistent
read should not see the modifications to the database. */

class ReadView {
public:
	ReadView() : m_creator_trx_id(TRX_ID_MAX), m_ids(),
		     m_registered(false) {}
	/** Check whether transaction id is valid.
	@param[in]	id		transaction id to check
	@param[in]	name		table name */
	static void check_trx_id_sanity(
		trx_id_t		id,
		const table_name_t&	name);

	/** Check whether the changes by id are visible.
	@param[in]	id	transaction id to check against the view
	@param[in]	name	table name
	@return whether the view sees the modifications of id. */
	bool changes_visible(
		trx_id_t		id,
		const table_name_t&	name) const
		MY_ATTRIBUTE((warn_unused_result))
	{
		if (id < m_up_limit_id || id == m_creator_trx_id) {

			return(true);
		}

		check_trx_id_sanity(id, name);

		if (id >= m_low_limit_id) {

			return(false);

		} else if (m_ids.empty()) {

			return(true);
		}

		return(!std::binary_search(m_ids.begin(), m_ids.end(), id));
	}

	/**
	@param id		transaction to check
	@return true if view sees transaction id */
	bool sees(trx_id_t id) const
	{
		return(id < m_up_limit_id);
	}

	/**
	Mark the view as closed */
	void close()
	{
		set_creator_trx_id(TRX_ID_MAX);
	}

	bool is_open() const
	{
		return static_cast<trx_id_t>(my_atomic_load64_explicit(
				const_cast<int64*>(
				reinterpret_cast<const int64*>(
				&m_creator_trx_id)),
				MY_MEMORY_ORDER_RELAXED)) != TRX_ID_MAX;
	}

	bool is_registered() const { return(m_registered); }
	void set_registered(bool registered) { m_registered= registered; }

	/**
	Write the limits to the file.
	@param file		file to write to */
	void print_limits(FILE* file) const
	{
		fprintf(file,
			"Trx read view will not see trx with"
			" id >= " TRX_ID_FMT ", sees < " TRX_ID_FMT "\n",
			m_low_limit_id, m_up_limit_id);
	}

	/**
	@return the low limit no */
	trx_id_t low_limit_no() const
	{
		return(m_low_limit_no);
	}

	/**
	@return the low limit id */
	trx_id_t low_limit_id() const
	{
		return(m_low_limit_id);
	}

	/**
	@return true if there are no transaction ids in the snapshot */
	bool empty() const
	{
		return(m_ids.empty());
	}

	/**
	Set the creator transaction id, existing id must be 0.

	Note: This shouldbe set only for views created by RW
	transactions. */
	void set_creator_trx_id(trx_id_t id)
	{
		my_atomic_store64_explicit(
				reinterpret_cast<int64*>(&m_creator_trx_id),
				id, MY_MEMORY_ORDER_RELAXED);
	}

#ifdef UNIV_DEBUG
	/**
	@param rhs		view to compare with
	@return truen if this view is less than or equal rhs */
	bool le(const ReadView* rhs) const
	{
		return(m_low_limit_no <= rhs->m_low_limit_no);
	}

	trx_id_t up_limit_id() const
	{
		return(m_up_limit_id);
	}
#endif /* UNIV_DEBUG */
private:
	/**
	Opens a read view where exactly the transactions serialized before this
	point in time are seen in the view. */
	inline void clone();

	/**
	Copy state from another view.
	@param other		view to copy from */
	inline void copy(const ReadView& other);

	friend class MVCC;

	/** The read should not see any transaction with trx id >= this
	value. In other words, this is the "high water mark". */
	trx_id_t	m_low_limit_id;

	/** The read should see all trx ids which are strictly
	smaller (<) than this value.  In other words, this is the
	low water mark". */
	trx_id_t	m_up_limit_id;

	/** trx id of creating transaction, set to TRX_ID_MAX for free
	views. */
	trx_id_t	m_creator_trx_id;

	/** Set of RW transactions that was active when this snapshot
	was taken */
	trx_ids_t	m_ids;

	/** The view does not need to see the undo logs for transactions
	whose transaction number is strictly smaller (<) than this value:
	they can be removed in purge if not needed by other views */
	trx_id_t	m_low_limit_no;

	/** true if transaction is in MVCC::m_views. Only thread that owns
	this view may access it. */
	bool		m_registered;

	byte		pad1[CACHE_LINE_SIZE];
	UT_LIST_NODE_T(ReadView)	m_view_list;
};

#endif
