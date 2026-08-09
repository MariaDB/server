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
#include "item.h"
#include "sql_alloc.h"

enum tablesample_method_enum
{
  TABLESAMPLE_NONE= 0,
  TABLESAMPLE_SYSTEM,
  TABLESAMPLE_BERNOULLI
};

class THD;

class Lex_tablesample: public Sql_alloc
{
private:
  enum tablesample_method_enum sampling_method=
    tablesample_method_enum::TABLESAMPLE_NONE;
  Item *sampling_percentage;
  double percentage= 0.0;
  ulong seed1= 0;
  ulong seed2= 0;

public:
  Lex_tablesample(enum tablesample_method_enum method, Item *percentage) :
    sampling_method(method), sampling_percentage(percentage) {}

  int fix_tablesample_fields(THD *thd);

  double get_sampling_percentage_fraction() const
  {
    return percentage;
  }

  tablesample_method_enum get_sampling_method() const
  {
    return sampling_method;
  }

  void seed_sample_rand(my_rnd_struct *out) const
  {
    my_rnd_init(out, seed1, seed2);
  }
};

#endif