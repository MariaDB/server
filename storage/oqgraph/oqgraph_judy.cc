/* Copyright (C) 2007-2013 Arjen G Lentz & Antony T Curtis for Open Query

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/* ======================================================================
   Open Query Graph Computation Engine, based on a concept by Arjen Lentz
   v3 implementation by Antony Curtis, Arjen Lentz, Andrew McDonnell
   For more information, documentation, support, enhancement engineering,
   see http://openquery.com/graph or contact graph@openquery.com
   ======================================================================
*/

#include "oqgraph_judy.h"

/*
  Currently the only active code that can return error is:
    judy_bitset::reset()/J1U()
    judy_bitset::setbit()/J1S()

  In most cases errors are either about wrong parameters passed to Judy
  functions or internal structures corruption. These definitely deserve
  abnormal process termination instead of exit() as it is done by original
  JUDYERROR.

  TODO: there's one exception that should be handled properly though: OOM.
*/
#include <stdio.h>
#define JUDYERROR(CallerFile, CallerLine, JudyFunc, JudyErrno, JudyErrID) \
    {                                                                     \
        (void) fprintf(stderr, "File '%s', line %d: %s(), "               \
           "JU_ERRNO_* == %d, ID == %d\n",                                \
           CallerFile, CallerLine,                                        \
           JudyFunc, JudyErrno, JudyErrID);                               \
        abort();                                                          \
    }
#include <Judy.h>

void open_query::judy_bitset::clear()
{
  int rc;
  J1FA(rc, array);
}

bool open_query::judy_bitset::test(size_type n) const
{
  int rc;
  J1T(rc, array, n);
  return rc == 1;
}

open_query::judy_bitset& open_query::judy_bitset::setbit(size_type n)
{
  int rc;
  J1S(rc, array, n);
  return *this;
}

open_query::judy_bitset& open_query::judy_bitset::reset(size_type n)
{
  int rc;
  J1U(rc, array, n);
  return *this;
}

open_query::judy_bitset& open_query::judy_bitset::flip(size_type n)
{
  int rc;
  J1U(rc, array, n);
  if (!rc)
  {
    J1S(rc, array, n);
  }
  return *this;
}

open_query::judy_bitset::size_type open_query::judy_bitset::num_blocks() const
{
  Word_t rc;
  J1MU(rc, array);
  return rc;
}

open_query::judy_bitset::size_type open_query::judy_bitset::size() const
{
  int rc;
  Word_t index = (Word_t) -1;
  J1L(rc, array, index);
  if (!rc)
    return index;
  else
    return npos;
}

open_query::judy_bitset::size_type open_query::judy_bitset::count() const
{
  Word_t rc;
  J1C(rc, array, 0, -1);
  return rc;
}

open_query::judy_bitset& open_query::judy_bitset::set(const judy_bitset& src)
{
  if (!src.empty())
  {
    for (size_type pos= src.find_first(); pos != npos; pos= src.find_next(pos))
    {
      set(pos);
    }
  }
  return *this;
}


open_query::judy_bitset::size_type open_query::judy_bitset::find_first() const
{
  int rc;
  Word_t index = 0;
  J1F(rc, array, index);
  if (!rc)
    return index;
  else
    return npos;
}

open_query::judy_bitset::size_type open_query::judy_bitset::find_next(size_type n) const
{
  int rc;
  Word_t index = (Word_t) n;
  J1N(rc, array, index);
  if (!rc)
    return index;
  else
    return npos;
}
