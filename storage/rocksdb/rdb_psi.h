/* Copyright (c) 2017, Percona and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */
#pragma once

#ifndef _rdb_psi_h_
#define _rdb_psi_h_

/* MySQL header files */
#include <my_global.h>
#include <my_pthread.h>

#include <mysql/psi/mysql_stage.h>

/* MyRocks header files */
#include "./rdb_utils.h"

namespace myrocks {

/*
  The following is needed as an argument for mysql_stage_register,
  irrespectively of whether we're compiling with P_S or not.
*/
extern my_core::PSI_stage_info stage_waiting_on_row_lock;

#ifdef HAVE_PSI_INTERFACE
extern my_core::PSI_thread_key rdb_background_psi_thread_key,
    rdb_drop_idx_psi_thread_key, rdb_mc_psi_thread_key;

extern my_core::PSI_mutex_key rdb_psi_open_tbls_mutex_key,
    rdb_signal_bg_psi_mutex_key, rdb_signal_drop_idx_psi_mutex_key,
    rdb_signal_mc_psi_mutex_key, rdb_collation_data_mutex_key,
    rdb_mem_cmp_space_mutex_key, key_mutex_tx_list, rdb_sysvars_psi_mutex_key,
    rdb_cfm_mutex_key, rdb_sst_commit_key, rdb_block_cache_resize_mutex_key;

extern my_core::PSI_rwlock_key key_rwlock_collation_exception_list,
    key_rwlock_read_free_rpl_tables, key_rwlock_skip_unique_check_tables;

extern my_core::PSI_cond_key rdb_signal_bg_psi_cond_key,
    rdb_signal_drop_idx_psi_cond_key, rdb_signal_mc_psi_cond_key;
#endif  // HAVE_PSI_INTERFACE

void init_rocksdb_psi_keys();

}  // namespace myrocks

#endif  // _rdb_psi_h_
