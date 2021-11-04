/*
   Copyright (c) 2021 MariaDB

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


#include "rowid_filter.h"


/**
  @class Bloom_filter_container

  The bloom filter container implementation to store info on the set
  of rowids / primary keys that defines a pk-filter.
*/
class Bloom_filter_container : public Rowid_filter_container
{
public:

  Rowid_filter_container_type get_type() override { return BLOOM_FILTER_CONTAINER; }

  /* Allocate memory for the container */
  bool alloc() override;

  /*
    @brief Add info on a rowid / primary to the container
    @param ctxt   The context info (opaque)
    @param elem   The rowid / primary key to be added to the container
    @retval       true if elem is successfully added
  */
  bool add(void *ctxt, char *elem) override;

  /*
    @brief Check whether a rowid / primary key is in container
    @param ctxt   The context info (opaque)
    @param elem   The rowid / primary key to be checked against the container
    @retval       False if elem is definitely not in the container
  */
  bool check(void *ctxt, char *elem) override;

};
