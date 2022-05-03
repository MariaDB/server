/*
   Copyright (c) 2010, 2011, Monty Program Ab

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

#ifndef SQL_EXPRESSION_CACHE_INCLUDED
#define SQL_EXPRESSION_CACHE_INCLUDED

#include "sql_select.h"


/**
  Interface for expression cache

  @note
  Parameters of an expression cache interface are set on the creation of the
  cache. They are passed when a cache object of the implementation class is
  constructed. That's why they are not visible in this interface.
*/

extern ulong subquery_cache_miss, subquery_cache_hit;

class Expression_cache :public Sql_alloc
{
public:
  enum result {ERROR, HIT, MISS};

  Expression_cache(){};
  virtual ~Expression_cache() {};
  /**
    Shall check the presence of expression value in the cache for a given
    set of values of the expression parameters.  Return the result of the
    expression if it's found in the cache.
  */
  virtual result check_value(Item **value)= 0;
  /**
    Shall put the value of an expression for given set of its parameters
    into the expression cache
  */
  virtual my_bool put_value(Item *value)= 0;

  /**
    Print cache parameters
  */
  virtual void print(String *str, enum_query_type query_type)= 0;

  /**
    Is this cache initialized
  */
  virtual bool is_inited()= 0;
  /**
    Initialize this cache
  */
  virtual void init()= 0;

  /**
    Save this object's statistics into Expression_cache_tracker object
  */
  virtual void update_tracker()= 0;
};

struct st_table_ref;
struct st_join_table;
class Item_field;


class Expression_cache_tracker :public Sql_alloc
{
public:
  enum expr_cache_state {UNINITED, STOPPED, OK};
  Expression_cache_tracker(Expression_cache *c) :
    cache(c), hit(0), miss(0), state(UNINITED)
  {}

private:
  // This can be NULL if the cache is already deleted
  Expression_cache *cache;

public:
  ulong hit, miss;
  enum expr_cache_state state;

  static const char* state_str[3];
  void set(ulong h, ulong m, enum expr_cache_state s)
  {hit= h; miss= m; state= s;}

  void detach_from_cache() { cache= NULL; }
  void fetch_current_stats()
  {
    if (cache)
      cache->update_tracker();
  }
};


/**
  Implementation of expression cache over a temporary table
*/

class Expression_cache_tmptable :public Expression_cache
{
public:
  Expression_cache_tmptable(THD *thd, List<Item> &dependants, Item *value);
  virtual ~Expression_cache_tmptable();
  virtual result check_value(Item **value);
  virtual my_bool put_value(Item *value);

  void print(String *str, enum_query_type query_type);
  bool is_inited() { return inited; };
  void init();

  void set_tracker(Expression_cache_tracker *st)
  {
    tracker= st;
    update_tracker();
  }
  virtual void update_tracker()
  {
    if (tracker)
    {
      tracker->set(hit, miss, (inited ? (cache_table ?
                                         Expression_cache_tracker::OK :
                                         Expression_cache_tracker::STOPPED) :
                               Expression_cache_tracker::UNINITED));
    }
  }

private:
  void disable_cache();

  /* tmp table parameters */
  TMP_TABLE_PARAM cache_table_param;
  /* temporary table to store this cache */
  TABLE *cache_table;
  /* Thread handle for the temporary table */
  THD *table_thd;
  /* EXPALIN/ANALYZE statistics */
  Expression_cache_tracker *tracker;
  /* TABLE_REF for index lookup */
  struct st_table_ref ref;
  /* Cached result */
  Item_field *cached_result;
  /* List of parameter items */
  List<Item> &items;
  /* Value Item example */
  Item *val;
  /* hit/miss counters */
  ulong hit, miss;
  /* Set on if the object has been succesfully initialized with init() */
  bool inited;
};

#endif /* SQL_EXPRESSION_CACHE_INCLUDED */
