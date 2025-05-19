/* Copyright (C) MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/*
  Handling of multiple key caches in Aria

  Each data and index pages for a table is put in the same cache, based on the
  filenumber of that index file.
*/

#include "maria_def.h"
#include "ma_pagecache.h"
#include <hash.h>
#include <m_string.h>
#include "../../mysys/my_safehash.h"

/*****************************************************************************
  Functions to handle the pagecache objects
*****************************************************************************/

/**
   Init 'segments' number of independent pagecaches

   @return 0 ok
   @return 1 error
*/

my_bool multi_init_pagecache(PAGECACHES *pagecaches, ulong segments,
                             size_t use_mem, uint division_limit,
                             uint age_threshold,
                             uint block_size, uint changed_blocks_hash_size,
                             myf my_readwrite_flags)
{
  PAGECACHE *pagecache;
  DBUG_ENTER("init_pagecaches");

  pagecaches->initialized= 0;
  if (!(pagecaches->caches= ((PAGECACHE*)
                             my_malloc(PSI_INSTRUMENT_ME,
                                       sizeof(PAGECACHE) * segments,
                                       MYF(MY_FAE | MY_ZEROFILL)))))
    DBUG_RETURN(1);

  pagecache= pagecaches->caches;
  pagecaches->segments= segments;
  for (ulong i= 0; i < segments ; i++, pagecache++)
  {
    if (init_pagecache(pagecache,
                       MY_MAX(MIN_KEY_CACHE_SIZE, use_mem / segments),
                       division_limit, age_threshold,
                       block_size, changed_blocks_hash_size,
                       my_readwrite_flags) == 0)
      goto err;
    pagecache->multi= 1;                        /* Part of segemented cache */
  }
  pagecaches->initialized= 1;
  DBUG_RETURN(0);

err:
  while (pagecache-- != pagecaches->caches)
    end_pagecache(pagecache, TRUE);
  my_free(pagecaches->caches);
  pagecaches->caches= 0;                        /* For easier debugging */
  DBUG_RETURN(1);
}


void multi_end_pagecache(PAGECACHES *pagecaches)
{
  DBUG_ENTER("end_pagecaches");

  if (unlikely(!pagecaches->initialized))
    DBUG_VOID_RETURN;

  for (ulong i= 0; i < pagecaches->segments ; i++)
    end_pagecache(pagecaches->caches+i, TRUE);

  my_free(pagecaches->caches);
  pagecaches->caches= 0;
  pagecaches->initialized= 0;
  pagecaches->segments= 0;
  DBUG_VOID_RETURN;
}


/*
  Call pagecache_collect_changed_blocks_with_len() over all
  pagecaches

  @param pagecaches        The partitioned page cache
  @param str               Buffers for changed blocks
  @param min_rec_lsn[out]  Min rec lsn in pagecache
  @param dirty_pages[out]  Total dirty pages found
*/

my_bool multi_pagecache_collect_changed_blocks_with_lsn(PAGECACHES *pagecaches,
                                                        LEX_STRING *str,
                                                        LSN *min_rec_lsn,
                                                        uint *dirty_pages)
{
  uint pages= 0;
  DBUG_ENTER("multi_pagecache_collect_changed_blocks_with_lsn");
  *min_rec_lsn= LSN_MAX;
  for (ulong i= 0; i < pagecaches->segments ; i++, str++)
  {
    uint tmp_dirty;
    if (unlikely(pagecache_collect_changed_blocks_with_lsn(pagecaches->caches+i,
                                                           str,
                                                           min_rec_lsn,
                                                           &tmp_dirty)))
    {
      /* Free the collected checkpoint information */
      do
      {
        my_free(str->str);
        str->str= 0;
        str--;
      } while (i-- > 0);
      *dirty_pages= 0;
      DBUG_RETURN(1);
    }
    pages+= tmp_dirty;
  }
  *dirty_pages= pages;
  DBUG_RETURN(0);
}


struct st_pagecache_stats pagecache_stats;

/*
  Update the global pagecache status
  This function is called when accessing status variables
*/

void multi_update_pagecache_stats()
{
  struct st_pagecache_stats new;
  bzero(&new, sizeof(new));
  for (uint i= 0 ; i < maria_pagecaches.segments ; i++)
  {
    PAGECACHE *pagecache= maria_pagecaches.caches + i;
    new.blocks_used+=             pagecache->blocks_used;
    new.blocks_unused+=           pagecache->blocks_unused;
    new.blocks_changed+=          pagecache->blocks_changed;
    new.global_blocks_changed+=   pagecache->global_blocks_changed;
    new.global_cache_w_requests+= pagecache->global_cache_w_requests;
    new.global_cache_write+=      pagecache->global_cache_write;
    new.global_cache_r_requests+= pagecache->global_cache_r_requests;
    new.global_cache_read+=       pagecache->global_cache_read;
  }
  pagecache_stats.blocks_used=             new.blocks_used;
  pagecache_stats.blocks_unused=           new.blocks_unused;
  pagecache_stats.blocks_changed=          new.blocks_changed;
  pagecache_stats.global_blocks_changed=   new.global_blocks_changed;
  pagecache_stats.global_cache_w_requests= new.global_cache_w_requests;
  pagecache_stats.global_cache_write=      new.global_cache_write;
  pagecache_stats.global_cache_r_requests= new.global_cache_r_requests;
  pagecache_stats.global_cache_read=       new.global_cache_read;
};


/*
  Get the total writes to the pagecaches
*/

ulonglong multi_global_cache_writes(PAGECACHES *pagecaches)
{
  ulonglong count= 0;
  for (ulong i= 0; i < pagecaches->segments ; i++)
    count+= pagecaches->caches[i].global_cache_write;
  return count;
}


/*
  Reset pagecache statistics
*/

void multi_reset_pagecache_counters(PAGECACHES *pagecaches)
{
  for (ulong i= 0; i < pagecaches->segments ; i++)
    reset_pagecache_counters(NullS, pagecaches->caches+i);
  multi_update_pagecache_stats();
}
