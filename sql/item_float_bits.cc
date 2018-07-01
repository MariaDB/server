/* Copyright (c) 2018 MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "mariadb.h"
#include "item_float_bits.h"
#include "my_global.h" // isfinite defined in math.h, from my_global
#include <m_string.h> // memcpy

/*
   Casting between an int pointer and a float pointer is usually a
   violation of C99/C11 "strict aliasing" rules (see also C++
   reinterpret_cast and "type aliasing" rules), thus to be legal we can
   either "type-pun" via a union or we can use memcpy(). Type-punning
   with a union looks pretty arcane and is not modern best practice;
   memcpy() is more clear.  These calls to memcpy() are probably going
   to be removed by the compiler, e.g. this gcc output:
   https://godbolt.org/g/hiX8oL
*/


///////////////////////////////////////////////////////////////////////////
longlong Item_func_float_to_int32_bits::val_int()
{
  float f;
  int32 i;
  DBUG_ASSERT(fixed);
  DBUG_ASSERT(sizeof(float) == sizeof(int32));

  f= args[0]->val_real();
  memcpy(&i, &f, sizeof(int32));
  null_value= args[0]->null_value;

  return i;
}


ulonglong Item_func_float_to_int32_bits::val_uint()
{
  return (ulonglong)val_int();
}


///////////////////////////////////////////////////////////////////////////

double Item_func_int32_bits_to_float::val_real()
{
  float f;
  int32 i;
  DBUG_ASSERT(fixed);
  DBUG_ASSERT(sizeof(float) == sizeof(int32));

  i= args[0]->val_int();
  memcpy(&f, &i, sizeof(int32));
  null_value= isfinite(f) ? args[0]->null_value : 1;

  return f;
}


///////////////////////////////////////////////////////////////////////////
longlong Item_func_double_to_int64_bits::val_int()
{
  double d;
  int64 i;
  DBUG_ASSERT(fixed);
  DBUG_ASSERT(sizeof(double) == sizeof(int64));

  d= args[0]->val_real();
  memcpy(&i, &d, sizeof(int64));
  null_value= args[0]->null_value;

  return i;
}


ulonglong Item_func_double_to_int64_bits::val_uint()
{
  return (ulonglong)val_int();
}

///////////////////////////////////////////////////////////////////////////

double Item_func_int64_bits_to_double::val_real()
{
  double d;
  uint64 i;
  DBUG_ASSERT(fixed);
  DBUG_ASSERT(sizeof(double) == sizeof(int64));

  i= args[0]->val_int();
  memcpy(&d, &i, sizeof(int64));
  null_value= isfinite(d) ? args[0]->null_value : 1;

  return d;
}
