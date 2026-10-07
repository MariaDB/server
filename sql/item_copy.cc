/*
   Copyright (c) 2026, MariaDB

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "item.h"


Item *Pushdown_copy_context::copy_cache(THD *thd,
                                        const Item_cache *cache) const
{
  return const_cast<Item_cache *>(cache);
}


Item *Parallel_copy_context::copy_cache(THD *thd,
                                        const Item_cache *cache) const
{
  Item *example= cache->get_example();
  /*
    A cache without an example has no item to copy.  A copy of an aggregate
    function, including one under a window function, holds the Aggregator of
    the original, and the cleanup of each of them deletes it.
  */
  if (!example || example->with_sum_func() || example->with_window_func())
    return nullptr;
  return example->copy_for(thd, *this);
}


#ifndef DBUG_OFF
bool Pushdown_copy_context::may_share(const Item *item) const
{
  return item->type() == Item::CACHE_ITEM;
}


bool Parallel_copy_context::substitutes(const Item *item) const
{
  return item->type() == Item::CACHE_ITEM;
}
#endif
