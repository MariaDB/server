/*****************************************************************************

Copyright (c) 2014, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, 2020, MariaDB Corporation.

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
@file ut/ut0new.cc
Instrumented memory allocator.

Created May 26, 2014 Vasil Dimov
*******************************************************/

#include "univ.i"
#include "ut0new.h"
/** The total amount of memory currently allocated from the operating
system with allocate_large(). */
Atomic_counter<ulint> os_total_large_mem_allocated;

/** Maximum number of retries to allocate memory. */
const size_t	alloc_max_retries = 60;

/** Keys for registering allocations with performance schema.
Keep this list alphabetically sorted. */
#ifdef BTR_CUR_HASH_ADAPT
PSI_memory_key	mem_key_ahi;
#endif /* BTR_CUR_HASH_ADAPT */
PSI_memory_key	mem_key_buf_buf_pool;
PSI_memory_key	mem_key_dict_stats_bg_recalc_pool_t;
PSI_memory_key	mem_key_dict_stats_index_map_t;
PSI_memory_key	mem_key_dict_stats_n_diff_on_level;
PSI_memory_key	mem_key_other;
PSI_memory_key	mem_key_row_log_buf;
PSI_memory_key	mem_key_row_merge_sort;
PSI_memory_key	mem_key_std;

#ifdef UNIV_PFS_MEMORY

/** Auxiliary array of performance schema 'PSI_memory_info'.
Each allocation appears in
performance_schema.memory_summary_global_by_event_name (and alike) in the form
of e.g. 'memory/innodb/NAME' where the last component NAME is picked from
the list below:
1. If key is specified, then the respective name is used
2. Without a specified key, allocations from inside std::* containers use
   mem_key_std
3. Without a specified key, allocations from outside std::* pick up the key
   based on the file name, and if file name is not found in the predefined list
   (in ut_new_boot()) then mem_key_other is used.
Keep this list alphabetically sorted. */
static PSI_memory_info	pfs_info[] = {
#ifdef BTR_CUR_HASH_ADAPT
  {&mem_key_ahi, "adaptive hash index", 0},
#endif /* BTR_CUR_HASH_ADAPT */
  {&mem_key_buf_buf_pool, "buf_buf_pool", 0},
  {&mem_key_dict_stats_bg_recalc_pool_t, "dict_stats_bg_recalc_pool_t", 0},
  {&mem_key_dict_stats_index_map_t, "dict_stats_index_map_t", 0},
  {&mem_key_dict_stats_n_diff_on_level, "dict_stats_n_diff_on_level", 0},
  {&mem_key_other, "other", 0},
  {&mem_key_row_log_buf, "row_log_buf", 0},
  {&mem_key_row_merge_sort, "row_merge_sort", 0},
  {&mem_key_std, "std", 0},
};

static const int NKEYS = static_cast<int>UT_ARR_SIZE(auto_event_names)-1;
static PSI_memory_key auto_event_keys[NKEYS];

/** Setup the internal objects needed for UT_NEW() to operate.
This must be called before the first call to UT_NEW(). */
void ut_new_boot()
{
  PSI_MEMORY_CALL(register_memory)("innodb", pfs_info, static_cast<int>
                                   UT_ARR_SIZE(pfs_info));

  PSI_memory_info pfs_info_auto[NKEYS];
  for (int i= 0; i < NKEYS; i++)
  {
    pfs_info_auto[i]= {&auto_event_keys[i], auto_event_names[i], 0};
  }

  PSI_MEMORY_CALL(register_memory)("innodb", pfs_info_auto,NKEYS);
}

/** Retrieve a memory key (registered with PFS), corresponding to source file .

@param[in] autoevent_idx - offset to the auto_event_names corresponding to the
file name of the caller.

@return registered memory key or PSI_NOT_INSTRUMENTED
*/
PSI_memory_key ut_new_get_key_by_file(uint32_t autoevent_idx)
{
  ut_ad(autoevent_idx < NKEYS);
  return auto_event_keys[autoevent_idx];
}

#else /* UNIV_PFS_MEMORY */
void ut_new_boot(){}
#endif
