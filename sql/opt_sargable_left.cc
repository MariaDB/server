/* Copyright (c) 2024, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/**
  @file

  @brief
  This file contains SargableLeft optimization
*/

#include "mariadb.h"
#include "sql_priv.h"
#include <m_ctype.h>
#include "sql_select.h"

/*
  SargableLeft
  ============

  This optimization makes conditions in forms like

    LEFT(key_col, N) = 'string_const'
    SUBSTRING(key_col, 1, N) = 'string_const'

  sargable. The conditions take the first N characters of key_col and
  compare them with a string constant.
  However, producing index lookup intervals for this collation is complex
  due to contractions.

  Contractions
  ------------
  A contraction is a property of collation where a sequence of multiple
  characters is compared as some other character(s).
  For example, in utfmb4_danish_ci, 'AA' is compared as one character 'Å'
  which sorts after 'Z':

  MariaDB [test]> select a from t1 order by col1;
  +------+
  | col1 |
  +------+
  | BA1  | (1)
  | BC   |
  | BZ   |
  | BAA2 | (2)
  +------+

  Now suppose we're producing lookup ranges for condition

  LEFT(col1, 2)='BA'

  In addition to looking near 'BA' (1), we need to look into the area right
  after 'BZ' (2), where we may find 'BAA'.

  Fortunately, this was already implemented for handling LIKE conditions in
  form 'key_col LIKE 'BA%'. Each collation provides like_range() call which
  produces lookup range in a collation-aware way.

  Differences between LIKE and LEFT=
  ----------------------------------
  So can one reduce or even rewrite conditions with LEFT() into LIKE? No, there
  are differences.

  First, LIKE does character-by-character comparison, ignoring the collation's
  contractions:

  MariaDB [test]> select col1, col1='AA', col1 LIKE 'AA' from t1;
  +------+-----------+----------------+
  | col1 | col1='AA' | col1 LIKE 'AA' |
  +------+-----------+----------------+
  | AA   |         1 |              1 |
  | Å    |         1 |              0 |
  +------+-----------+----------------+

  (However, index comparison function uses equality's comparison rules.
  my_like_range() will produce an index range 'AA' <= col1 <= 'AA'. Reading rows
  from it will return 'Å' as well)

  Second, LEFT imposes additional constraints on the length of both parts. For
  example:
  - LEFT(col,2)='string-longer-than-two-chars' - is false for any value of col.
  - LEFT(col,2)='A' is not equivalent to (col LIKE 'A%'), consider col='Ab'.

  Take-aways
  ----------
  - SargableLeft makes use of my_like_range() to produce index intervals.
  - LEFT(col, N)='foo'
  - We ignore the value of N when producing the lookup range (this may make the
    range to include rows for which the predicate is false)
    = For the SUBSTRING form, we only need to check that M=1 in the
      SUBSTRING(col, M, N)='foo'.
*/


/*
  @brief Check if this condition is sargable LEFT(key_col, N)='foo', or
         similar condition with SUBSTRING().

  @detail
    'foo' here can be any constant we can compute during optimization,
    Only equality conditions are supported.
    See SargableLeft above for details.

  @param  field      The first argument of LEFT or SUBSTRING if sargable,
                     otherwise dereferenced to NULL
  @param  value_idx  The index of argument that is the prefix string
                     if sargable, otherwise dereferenced to -1
*/

bool Item_bool_func::with_sargable_substr(Item_field **field, int *value_idx) const
{
  int func_idx, val_idx= -1;
  Item **func_args, *real= NULL;
  bool ret= false;
  enum Functype type;
  if (functype() != EQ_FUNC)
    goto done;
  if (args[0]->type() == FUNC_ITEM)
    func_idx= 0;
  else if (args[1]->type() == FUNC_ITEM)
    func_idx= 1;
  else
    goto done;
  type= ((Item_func *) args[func_idx])->functype();
  if (type != SUBSTR_FUNC && type != LEFT_FUNC)
    goto done;
  func_args= ((Item_func *) args[func_idx])->arguments();
  real= func_args[0]->real_item();
  val_idx= 1 - func_idx;
  if (real->type() == FIELD_ITEM &&
      args[val_idx]->can_eval_in_optimize() &&
      (type == LEFT_FUNC || func_args[1]->val_int() == 1))
  {
    ret= true;
    goto done;
  }
  real= NULL;
  val_idx= -1;
done:
  if (field != NULL)
    *field= (Item_field *) real;
  if (value_idx != NULL)
    *value_idx= val_idx;
  return ret;
}


