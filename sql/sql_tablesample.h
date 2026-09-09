/* Copyright (c) 2026 Dearsh Oberoi

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef SQL_TABLESAMPLE_INCLUDED
#define SQL_TABLESAMPLE_INCLUDED


#include "my_global.h"
#include "sql_alloc.h"
#include "my_rnd.h"

enum tablesample_method_enum
{
  TABLESAMPLE_NONE= 0,
  TABLESAMPLE_SYSTEM,
  TABLESAMPLE_BERNOULLI
};

class THD;
struct TABLE;
class Item;

class Lex_tablesample: public Sql_alloc
{
private:
  /*
    SYSTEM sampling draws independent random index descents and rejects
    duplicates (see mi_random_dive() / rr_sampling_system()). As the
    requested fraction approaches 1, collisions between draws become
    frequent, and the expected number of descents to get unique rows,
    increase. So SYSTEM falls back to BERNOULLI if sampling percentage is
    greater than 30.
  */
  static constexpr double SYSTEM_TO_BERNOULLI_MAX_FRACTION= 0.3;

  enum tablesample_method_enum sampling_method=
    tablesample_method_enum::TABLESAMPLE_NONE;
  /*
    The method actually used at read/cost time, which may differ from
    sampling_method (e.g. SYSTEM falls back to BERNOULLI when the table
    has no primary key). Recomputed on every fix_tablesample_fields() call
    (once per statement execution) from the immutable sampling_method, so
    that a prepared statement/stored procedure re-executed against a table
    whose primary key changed always reflects the current table, never a
    stale decision from an earlier execution.
  */
  enum tablesample_method_enum effective_method=
    tablesample_method_enum::TABLESAMPLE_NONE;
  Item *sampling_percentage;
  double percentage= 0.0;
  ulong seed1= 0;
  ulong seed2= 0;

public:
  Lex_tablesample(enum tablesample_method_enum method, Item *percentage) :
    sampling_method(method), effective_method(method),
    sampling_percentage(percentage) {}

  int fix_tablesample_fields(THD *thd, TABLE *table);

  double get_sampling_percentage_fraction() const
  {
    return percentage;
  }

  tablesample_method_enum get_effective_sampling_method() const
  {
    return effective_method;
  }

  void seed_sample_rand(my_rnd_struct *out) const
  {
    my_rnd_init(out, seed1, seed2);
  }
};

#endif