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

#include "mariadb.h"
#include "keycaches.h"

/****************************************************************************
  Named list handling
****************************************************************************/

NAMED_ILIST key_caches;
NAMED_ILIST rpl_filters;

extern "C" PSI_memory_key key_memory_KEY_CACHE;
extern PSI_memory_key key_memory_NAMED_ILINK_name;

/**
  ilink (intrusive list element) with a name
*/
class NAMED_ILINK :public ilink
{
public:
  const char *name;
  size_t name_length;
  uchar* data;

  NAMED_ILINK(I_List<NAMED_ILINK> *links, const char *name_arg,
             size_t name_length_arg, uchar* data_arg)
    :name_length(name_length_arg), data(data_arg)
  {
    name= my_strndup(key_memory_NAMED_ILINK_name, name_arg, name_length,
                     MYF(MY_WME));
    links->push_back(this);
  }
  inline bool cmp(const char *name_cmp, size_t length)
  {
    return length == name_length && !memcmp(name, name_cmp, length);
  }
  ~NAMED_ILINK()
  {
    my_free((void *) name);
  }
};

uchar* find_named(I_List<NAMED_ILINK> *list, const char *name, size_t length,
                NAMED_ILINK **found)
{
  I_List_iterator<NAMED_ILINK> it(*list);
  NAMED_ILINK *element;
  while ((element= it++))
  {
    if (element->cmp(name, length))
    {
      if (found)
        *found= element;
      return element->data;
    }
  }
  return 0;
}


bool NAMED_ILIST::delete_element(const char *name, size_t length, void (*free_element)(const char *name, void*))
{
  I_List_iterator<NAMED_ILINK> it(*this);
  NAMED_ILINK *element;
  DBUG_ENTER("NAMED_ILIST::delete_element");
  while ((element= it++))
  {
    if (element->cmp(name, length))
    {
      (*free_element)(element->name, element->data);
      delete element;
      DBUG_RETURN(0);
    }
  }
  DBUG_RETURN(1);
}

void NAMED_ILIST::delete_elements(void (*free_element)(const char *name, void*))
{
  NAMED_ILINK *element;
  DBUG_ENTER("NAMED_ILIST::delete_elements");
  while ((element= get()))
  {
    (*free_element)(element->name, element->data);
    delete element;
  }
  DBUG_VOID_RETURN;
}


/* Key cache functions */

LEX_CSTRING default_key_cache_base= {STRING_WITH_LEN("default")};

KEY_CACHE zero_key_cache; ///< @@nonexistent_cache.param->value_ptr() points here

KEY_CACHE *get_key_cache(const LEX_CSTRING *cache_name)
{
  if (!cache_name || ! cache_name->length)
    cache_name= &default_key_cache_base;
  return ((KEY_CACHE*) find_named(&key_caches,
                                  cache_name->str, cache_name->length, 0));
}

KEY_CACHE *create_key_cache(const char *name, size_t length)
{
  KEY_CACHE *key_cache;
  DBUG_ENTER("create_key_cache");
  DBUG_PRINT("enter",("name: %.*s", (int)length, name));
  
  if ((key_cache= (KEY_CACHE*) my_malloc(key_memory_KEY_CACHE,
                                sizeof(KEY_CACHE), MYF(MY_ZEROFILL | MY_WME))))
  {
    if (!new NAMED_ILINK(&key_caches, name, length, (uchar*) key_cache))
    {
      my_free(key_cache);
      key_cache= 0;
    }
    else
    {
      /*
        Set default values for a key cache
        The values in dflt_key_cache_var is set by my_getopt() at startup

        We don't set 'buff_size' as this is used to enable the key cache
      */
      key_cache->param_block_size=     dflt_key_cache_var.param_block_size;
      key_cache->param_division_limit= dflt_key_cache_var.param_division_limit;
      key_cache->param_age_threshold=  dflt_key_cache_var.param_age_threshold;
      key_cache->param_partitions=     dflt_key_cache_var.param_partitions;
    }
  }
  DBUG_RETURN(key_cache);
}


KEY_CACHE *get_or_create_key_cache(const char *name, size_t length)
{
  LEX_CSTRING key_cache_name;
  KEY_CACHE *key_cache;

  key_cache_name.str= name;
  key_cache_name.length= length;
  if (!(key_cache= get_key_cache(&key_cache_name)))
    key_cache= create_key_cache(name, length);
  return key_cache;
}


void free_key_cache(const char *name, void *key_cache)
{
  end_key_cache(static_cast<KEY_CACHE *>(key_cache), 1); // Can never fail
  my_free(key_cache);
}


bool process_key_caches(process_key_cache_t func, void *param)
{
  I_List_iterator<NAMED_ILINK> it(key_caches);
  NAMED_ILINK *element;
  int res= 0;

  while ((element= it++))
  {
    KEY_CACHE *key_cache= (KEY_CACHE *) element->data;
    res |= func(element->name, key_cache, param);
  }
  return res != 0;
}

/* Rpl_filter functions */

LEX_CSTRING default_rpl_filter_base= {STRING_WITH_LEN("")};

Rpl_filter *get_rpl_filter(LEX_CSTRING *filter_name)
{
  if (!filter_name->length)
    filter_name= &default_rpl_filter_base;
  return ((Rpl_filter*) find_named(&rpl_filters,
                                   filter_name->str, filter_name->length, 0));
}

Rpl_filter *create_rpl_filter(const char *name, size_t length)
{
  Rpl_filter *filter;
  DBUG_ENTER("create_rpl_filter");
  DBUG_PRINT("enter",("name: %.*s", (int)length, name));
  
  filter= new Rpl_filter;
  if (filter) 
  {
    if (!new NAMED_ILINK(&rpl_filters, name, length, (uchar*) filter))
    {
      delete filter;
      filter= 0;
    }
  }
  DBUG_RETURN(filter);
}


Rpl_filter *get_or_create_rpl_filter(const char *name, size_t length)
{
  LEX_CSTRING rpl_filter_name;
  Rpl_filter *filter;

  rpl_filter_name.str= (char *) name;
  rpl_filter_name.length= length;
  if (!(filter= get_rpl_filter(&rpl_filter_name)))
    filter= create_rpl_filter(name, length);
  return filter;
}

void free_rpl_filter(const char *name, void *filter)
{
  delete static_cast<Rpl_filter*>(filter);
}

void free_all_rpl_filters()
{
  rpl_filters.delete_elements(free_rpl_filter);
}
