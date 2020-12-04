/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2020, MariaDB Corporation.
Copyright (c) 2008, Google Inc.
Copyright (c) 2020, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

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
@file sync/sync0sync.cc
Mutex, the basic synchronization primitive

Created 9/5/1995 Heikki Tuuri
*******************************************************/

#include "sync0sync.h"
#include "ut0mutex.h"

#ifdef UNIV_PFS_MUTEX
mysql_pfs_key_t	buf_pool_mutex_key;
mysql_pfs_key_t	dict_foreign_err_mutex_key;
mysql_pfs_key_t	dict_sys_mutex_key;
mysql_pfs_key_t	fil_system_mutex_key;
mysql_pfs_key_t	flush_list_mutex_key;
mysql_pfs_key_t	fts_cache_mutex_key;
mysql_pfs_key_t	fts_cache_init_mutex_key;
mysql_pfs_key_t	fts_delete_mutex_key;
mysql_pfs_key_t	fts_doc_id_mutex_key;
mysql_pfs_key_t	fts_pll_tokenize_mutex_key;
mysql_pfs_key_t	ibuf_bitmap_mutex_key;
mysql_pfs_key_t	ibuf_mutex_key;
mysql_pfs_key_t	ibuf_pessimistic_insert_mutex_key;
mysql_pfs_key_t	log_sys_mutex_key;
mysql_pfs_key_t	log_cmdq_mutex_key;
mysql_pfs_key_t	log_flush_order_mutex_key;
mysql_pfs_key_t	recalc_pool_mutex_key;
mysql_pfs_key_t	purge_sys_pq_mutex_key;
mysql_pfs_key_t	recv_sys_mutex_key;
mysql_pfs_key_t	redo_rseg_mutex_key;
mysql_pfs_key_t	noredo_rseg_mutex_key;
mysql_pfs_key_t page_zip_stat_per_index_mutex_key;
mysql_pfs_key_t rtr_active_mutex_key;
mysql_pfs_key_t	rtr_match_mutex_key;
mysql_pfs_key_t	rtr_path_mutex_key;
mysql_pfs_key_t	srv_innodb_monitor_mutex_key;
mysql_pfs_key_t	srv_misc_tmpfile_mutex_key;
mysql_pfs_key_t	srv_monitor_file_mutex_key;
mysql_pfs_key_t	buf_dblwr_mutex_key;
mysql_pfs_key_t	trx_pool_mutex_key;
mysql_pfs_key_t	trx_pool_manager_mutex_key;
mysql_pfs_key_t	lock_mutex_key;
mysql_pfs_key_t	lock_wait_mutex_key;
mysql_pfs_key_t	trx_sys_mutex_key;
mysql_pfs_key_t	srv_threads_mutex_key;
mysql_pfs_key_t	sync_array_mutex_key;
mysql_pfs_key_t	thread_mutex_key;
mysql_pfs_key_t row_drop_list_mutex_key;
mysql_pfs_key_t	rw_trx_hash_element_mutex_key;
mysql_pfs_key_t	read_view_mutex_key;
#endif /* UNIV_PFS_MUTEX */
#ifdef UNIV_PFS_RWLOCK
mysql_pfs_key_t	dict_operation_lock_key;
mysql_pfs_key_t	index_tree_rw_lock_key;
mysql_pfs_key_t	index_online_log_key;
mysql_pfs_key_t	fil_space_latch_key;
mysql_pfs_key_t trx_i_s_cache_lock_key;
mysql_pfs_key_t	trx_purge_latch_key;
#endif /* UNIV_PFS_RWLOCK */

/** For monitoring active mutexes */
MutexMonitor	mutex_monitor;

/** Print the filename "basename" e.g., p = "/a/b/c/d/e.cc" -> p = "e.cc"
@param[in]	filename	Name from where to extract the basename
@return the basename */
const char*
sync_basename(const char* filename)
{
	const char*	ptr = filename + strlen(filename) - 1;

	while (ptr > filename && *ptr != '/' && *ptr != '\\') {
		--ptr;
	}

	++ptr;

	return(ptr);
}

/** String representation of the filename and line number where the
latch was created
@param[in]	id		Latch ID
@param[in]	created		Filename and line number where it was crated
@return the string representation */
std::string
sync_mutex_to_string(
	latch_id_t		id,
	const std::string&	created)
{
	std::ostringstream msg;

	msg << "Mutex " << sync_latch_get_name(id) << " "
	    << "created " << created;

	return(msg.str());
}

/** Enable the mutex monitoring */
void
MutexMonitor::enable()
{
	/** Note: We don't add any latch meta-data after startup. Therefore
	there is no need to use a mutex here. */

	LatchMetaData::iterator	end = latch_meta.end();

	for (LatchMetaData::iterator it = latch_meta.begin(); it != end; ++it) {

		if (*it != NULL) {
			(*it)->get_counter()->enable();
		}
	}
}

/** Disable the mutex monitoring */
void
MutexMonitor::disable()
{
	/** Note: We don't add any latch meta-data after startup. Therefore
	there is no need to use a mutex here. */

	LatchMetaData::iterator	end = latch_meta.end();

	for (LatchMetaData::iterator it = latch_meta.begin(); it != end; ++it) {

		if (*it != NULL) {
			(*it)->get_counter()->disable();
		}
	}
}

/** Reset the mutex monitoring counters */
void
MutexMonitor::reset()
{
	/** Note: We don't add any latch meta-data after startup. Therefore
	there is no need to use a mutex here. */

	for (auto l : latch_meta) if (l) l->get_counter()->reset();
}
