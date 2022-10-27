#ifndef SQL_MODE_H_INCLUDED
#define SQL_MODE_H_INCLUDED
/*
   Copyright (c) 2019, MariaDB.

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

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "sql_basic_types.h"

/*
  class Sql_mode_dependency

  A combination of hard and soft dependency on sql_mode.
  Used to watch if a GENERATED ALWAYS AS expression guarantees consitent
  data written to its virtual column.

  A virtual column can appear in an index if:
  - the generation expression does not depend on any sql_mode flags, or
  - the generation expression has a soft dependency on an sql_mode flag,
    and the column knows how to handle this dependeny.

  A virtual column cannot appear in an index if:
  - its generation expression has a hard dependency
  - its generation expression has a soft dependency, but the column
    cannot handle it on store.
  An error is reported in such cases.

  How dependencies appear:
  - When a column return value depends on some sql_mode flag,
    its Item_field adds a corresponding bit to m_soft. For example,
    Item_field for a CHAR(N) column adds the PAD_CHAR_TO_FULL_LENGTH flag.
  - When an SQL function/operator return value depends on some sql_mode flag,
    it adds a corresponding bit to m_soft. For example, Item_func_minus
    adds the MODE_NO_UNSIGNED_SUBTRACTION in case of unsigned arguments.

  How dependency are processed (see examples below):
  - All SQL functions/operators bit-OR all hard dependencies from all arguments.
  - Some soft dependencies can be handled by the underlying Field on store,
    e.g. CHAR(N) can handle PAD_CHAR_TO_FULL_LENGTH.
  - Some soft dependencies can be handled by SQL functions and operators,
    e.g. RTRIM(expr) removes expr's soft dependency on PAD_CHAR_TO_FULL_LENGTH.
    If a function or operator handles a soft dependency on a certain sql_mode
    flag, it removes the corresponding bit from m_soft (see below).
    Note, m_hard is not touched in such cases.
  - When an expression with a soft dependency on a certain sql_mode flag
    goes as an argument to an SQL function/operator which cannot handle
    this flag, the dependency escalates from soft to hard
    (by moving the corresponding bit from m_soft to m_hard) and cannot be
    handled any more on the upper level, neither by a Field on store,
    nor by another SQL function/operator.

  There are four kinds of Items:
  1. Items that generate a soft or hard dependency, e.g.
     - Item_field for CHAR(N) - generates soft/PAD_CHAR_TO_FULL_LENGTH
     - Item_func_minus        - generates soft/NO_UNSIGNED_SUBTRACTION
  2. Items that convert a soft dependency to a hard dependency.
     This happens e.g. when an Item_func instance gets a soft dependency
     from its arguments, and it does not know how to handle this dependency.
     Most Item_func descendants do this.
  3. Items that remove soft dependencies, e.g.:
     - Item_func_rtrim - removes soft/PAD_CHAR_TO_FULL_LENGTH
                         that came from args[0] (under certain conditions)
     - Item_func_rpad  - removes soft/PAD_CJAR_TO_FULL_LENGTH
                         that came from args[0] (under certain conditions)
  4. Items that repeat soft dependency from its arguments to the caller.
     They are not implemented yet. But functions like Item_func_coalesce,
     Item_func_case, Item_func_case_abbreviation2 could do this.

  Examples:

  1. CREATE OR REPLACE TABLE t1 (a CHAR(5), v CHAR(20) AS(a), KEY(v));

     Here `v` has a soft dependency on `a`.
     The value of `a` depends on PAD_CHAR_TO_FULL_LENGTH, it can return:
     - 'a'                         - if PAD_CHAR_TO_FULL_LENGTH is disabled
     - 'a' followed by four spaces - if PAD_CHAR_TO_FULL_LENGTH is enabled
     But `v` will pad trailing spaces to the full length on store anyway.
     So Field_string handles this soft dependency on store.
     This combination of the virtial column data type and its generation
     expression is safe and provides consistent data in `v`, which is
     'a' followed by four spaces, no matter what PAD_CHAR_TO_FULL_LENGTH is.

  2. CREATE OR REPLACE TABLE t1 (a CHAR(5), v VARCHAR(20) AS(a), KEY(v));

     Here `v` has a soft dependency on `a`. But Field_varstring does
     not pad spaces on store, so it cannot handle this dependency.
     This combination of the virtual column data type and its generation
     expression is not safe. An error is returned.

  3. CREATE OR REPLACE TABLE t1 (a CHAR(5), v INT AS(LENGTH(a)), KEY(v));

     Here `v` has a hard dependency on `a`, because the value of `a`
     is wrapped to the function LENGTH().
     The value of `LENGTH(a)` depends on PAD_CHAR_TO_FULL_LENGTH, it can return:
     - 1  - if PAD_CHAR_TO_FULL_LENGTH is disabled
     - 4  - if PAD_CHAR_TO_FULL_LENGTH is enabled
     This combination cannot provide consistent data stored to `v`,
     therefore it's disallowed.
*/
class Sql_mode_dependency
{
  sql_mode_t m_hard;
  sql_mode_t m_soft;
public:
  Sql_mode_dependency()
   :m_hard(0), m_soft(0)
  { }
  Sql_mode_dependency(sql_mode_t hard, sql_mode_t soft)
   :m_hard(hard), m_soft(soft)
  { }
  sql_mode_t hard() const { return m_hard; }
  sql_mode_t soft() const { return m_soft; }
  operator bool () const
  {
    return m_hard > 0 || m_soft > 0;
  }
  Sql_mode_dependency operator|(const Sql_mode_dependency &other) const
  {
    return Sql_mode_dependency(m_hard | other.m_hard, m_soft | other.m_soft);
  }
  Sql_mode_dependency operator&(const Sql_mode_dependency &other) const
  {
    return Sql_mode_dependency(m_hard & other.m_hard, m_soft & other.m_soft);
  }
  Sql_mode_dependency &operator|=(const Sql_mode_dependency &other)
  {
    m_hard|= other.m_hard;
    m_soft|= other.m_soft;
    return *this;
  }
  Sql_mode_dependency &operator&=(const Sql_mode_dependency &other)
  {
    m_hard&= other.m_hard;
    m_soft&= other.m_soft;
    return *this;
  }
  Sql_mode_dependency &soft_to_hard()
  {
    m_hard|= m_soft;
    m_soft= 0;
    return *this;
  }
  void push_dependency_warnings(THD *thd) const;
};


#endif // SQL_MODE_H_INCLUDED
