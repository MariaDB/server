/* Copyright (c) 2026, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1335  USA */

#ifndef PARTIAL_RESULT_CACHE_INCLUDED
#define PARTIAL_RESULT_CACHE_INCLUDED

struct st_join_table;
typedef struct st_join_table JOIN_TAB;
struct READ_RECORD;
struct TABLE;
struct Partial_result_cache_stats;

class Partial_result_cache;

extern ulong partial_result_cache_hit;
extern ulong partial_result_cache_miss;
extern ulong partial_result_cache_rows_cached;
extern ulong partial_result_cache_rows_replayed;
extern ulong partial_result_cache_bypass;

// Configures PTRC for one executor join tab.
//
// The caller must already have selected the normal access method. This
// function only replaces the ref read callbacks when the table is safe to
// cache and the local cost check predicts repeated probes.
//
// Returns true when allocation fails and execution setup should abort.
bool setup_partial_result_cache(JOIN_TAB *tab, JOIN_TAB *first_tab,
                                uint join_cache_level);

// Releases a PTRC instance and publishes its per-execution counters.
void free_partial_result_cache(Partial_result_cache *cache);

// Connects a cache instance to ANALYZE output counters owned by EXPLAIN data.
void set_partial_result_cache_stats(Partial_result_cache *cache,
                                    Partial_result_cache_stats *stats);

#endif /* PARTIAL_RESULT_CACHE_INCLUDED */
