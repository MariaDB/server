#ifndef KEYCACHES_INCLUDED
#define KEYCACHES_INCLUDED

/* Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "sql_list.h"
#include <keycache.h>
#include <rpl_filter.h>

extern "C"
{
  typedef int (*process_key_cache_t) (const char *, KEY_CACHE *, void *);
}

class NAMED_ILINK;

class NAMED_ILIST: public I_List<NAMED_ILINK>
{
  public:
  void delete_elements(void (*free_element)(const char*, void*));
  bool delete_element(const char *name, size_t length, void (*free_element)(const char*, void*));
};

/* For key cache */
extern LEX_CSTRING default_base;
extern KEY_CACHE zero_key_cache;
extern NAMED_ILIST key_caches;

KEY_CACHE *create_key_cache(const char *name, size_t length);
KEY_CACHE *get_key_cache(const LEX_CSTRING *cache_name);
KEY_CACHE *get_or_create_key_cache(const char *name, size_t length);
void free_key_cache(const char *name, void *key_cache);
bool process_key_caches(process_key_cache_t func, void *param);

/* For Rpl_filter */
extern LEX_CSTRING default_rpl_filter_base;
extern NAMED_ILIST rpl_filters;

Rpl_filter *create_rpl_filter(const char *name, size_t length);
Rpl_filter *get_rpl_filter(LEX_CSTRING *filter_name);
Rpl_filter *get_or_create_rpl_filter(const char *name, size_t length);
void free_all_rpl_filters(void);

#endif /* KEYCACHES_INCLUDED */
