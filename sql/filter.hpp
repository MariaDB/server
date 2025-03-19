/* Copyright (c) 2025, MariaDB Corporation.
   
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

#ifndef ABSTRACT_FILTER_H
#define ABSTRACT_FILTER_H
#include "sql_string.h"

/** an interface that maps filtered keys to result values
  @tparam K filter key type
  @tparam V result value type
            (typically `bool` indicating whether the key is **excluded**)
*/
template<typename K, typename V= bool> class Filter
{
public:
  virtual ~Filter()= default;
  virtual V operator[](K key)= 0;
};

/** a shorthand to invert an existing Filter<K, bool> class */
template<class Filter>
class InvertedFilter: public Filter
{
public:
  virtual inline bool operator[](const Binary_string *key) override //FIXME: inflexible? what should be its scope?
  { return !Filter::operator[](key); }
};

/**
  @ref HASH based abstract Filter featuring set_rules() that
  parses a string separated by commas (with any trailing spaces)
*/
template<typename K, typename V= bool> class HashFilter: public Filter<K, V>
{
protected:
  HASH hashset;
  /**
    set_rules() callback:
    Parse and add the next `spec_end`-terminated (exclusive end) rule
    @return true if error
  */
  inline virtual bool add_rule(const char *rule, const char *rule_end)= 0;
  /** to_string() callback: append the @ref hashset `element` to `out_string` */
  inline virtual void hash_element_append_string
    (uchar *element, String *out_string) { DBUG_ASSERT(!"not implemented"); }
public:
  virtual ~HashFilter() override;
  /** @return true if error (it's up to the caller if they want to destruct) */
  bool set_rules(const char *spec);
  void to_string(String *out_string);
};

class RewriteDB: public HashFilter<const Binary_string *, const String *>
{
  struct StringPair { String from, to; };
  inline bool add_rule(const char *rule, const char *rule_end) override;
  inline void
  hash_element_append_string(uchar *element, String *out_string) override;
public:
  RewriteDB();
  virtual const String *operator[](const Binary_string *key) override;
};

#endif
