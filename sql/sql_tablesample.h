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

#include "sql_alloc.h"
#include "my_global.h"
#include "item.h"

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

public:
  Lex_tablesample(enum tablesample_method_enum method, Item *percentage) :
    sampling_method(method), sampling_percentage(percentage) {}

  int fix_tablesample_fields(THD *thd)
  {
    DBUG_ENTER("Lex_tablesample::fix_tablesample_fields");
    DBUG_ASSERT(thd);
    bool err= sampling_percentage->fix_fields_if_needed(thd, NULL);
    if(err)
      DBUG_RETURN(1);
    if (sampling_percentage->const_item())
    {
      double d= sampling_percentage->val_real();
      if (d < 0.0 || d > 100.0)
        DBUG_RETURN(1);
      percentage= d / 100.0;
    }

    DBUG_RETURN(0);
  }

  double get_sampling_percentage_fraction() const
  {
    return percentage;
  }

  tablesample_method_enum get_sampling_method() const
  {
    return sampling_method;
  }
};

#endif