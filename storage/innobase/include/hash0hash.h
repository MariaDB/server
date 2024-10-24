/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2022, MariaDB Corporation.

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
@file include/hash0hash.h
The simple hash table utility

Created 5/20/1997 Heikki Tuuri
*******************************************************/

#pragma once
#include "ut0rnd.h"
#include "ut0new.h"

struct hash_cell_t
{
  /** singly-linked, nullptr terminated list of hash buckets */
  void *node;

  /** Append an element.
  @tparam T      type of the element
  @param insert  the being-inserted element
  @param next    the next-element pointer in T */
  template<typename T>
  void append(T &insert, T *T::*next) noexcept
  {
    void **after;
    for (after= &node; *after;
         after= reinterpret_cast<void**>(&(static_cast<T*>(*after)->*next)));
    insert.*next= nullptr;
    *after= &insert;
  }

  /** Remove an element.
  @tparam T      type of the element
  @param element the being-removed element
  @param next    the next-element pointer in T */
  template<typename T>
  void remove(T &element, T *T::*next) noexcept
  {
    T **prev= reinterpret_cast<T**>(&node);
    while (*prev != &element)
      prev= &((*prev)->*next);
    *prev= element.*next;
    element.*next= nullptr;
  }
};

/** Hash table with singly-linked overflow lists */
struct hash_table_t
{
  /** number of elements in array (a prime number) */
  ulint n_cells;
  /** the hash array */
  hash_cell_t *array;

  /** Create the hash table.
  @param n  the lower bound of n_cells */
  void create(ulint n) noexcept
  {
    n_cells= ut_find_prime(n);
    array= static_cast<hash_cell_t*>(ut_zalloc_nokey(n_cells * sizeof *array));
  }

  /** Clear the hash table. */
  void clear() noexcept { memset(array, 0, n_cells * sizeof *array); }

  /** Free the hash table. */
  void free() noexcept { ut_free(array); array= nullptr; }

  ulint calc_hash(ulint fold) const noexcept
  { return ut_hash_ulint(fold, n_cells); }

  hash_cell_t *cell_get(ulint fold) const noexcept
  { return &array[calc_hash(fold)]; }
};
