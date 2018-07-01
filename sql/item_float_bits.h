#ifndef ITEM_FLOAT_BITS_INCLUDED
#define ITEM_FLOAT_BITS_INCLUDED

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

#include "item.h"

/*
Assuming IEEE 754 (radix 2), transforming the exponent is a bit tricky, but
the bit layout is straight-forward:

float32 Sign	Exponent	Significand (mantissa)
float32 1 [31]	8 [30-23]	23 [22-00]
float32 SEEEEEEE EMMMMMMM MMMMMMMM MMMMMMMM
https://en.wikipedia.org/wiki/Single-precision_floating-point_format

  int sign, exponent;
  uint32_t significand;

  sign = ((bits >> 31) == 0) ? 1 : -1;
  exponent = ((bits >> 23) & 0xff);
  significand = (exponent == 0)
              ? (bits & 0x7fffff) << 1
              : (bits & 0x7fffff) | 0x800000;


float64 Sign	Exponent	Significand (mantissa)
float64 1 [63]	11 [62-52]	52 [51-00]
float64 SEEEEEEE EEEEMMMM MMMMMMMM MMMMMMMM MMMMMMMM MMMMMMMM MMMMMMMM MMMMMMMM
https://en.wikipedia.org/wiki/Double-precision_floating-point_format

  int sign, exponent;
  uint64_t significand;

  sign = ((bits >> 63) == 0) ? 1 : -1;
  exponent = ((bits >> 52) & 0x7ffL);
  significand = (exponent == 0)
              ? (bits & 0xfffffffffffffL) << 1
              : (bits & 0xfffffffffffffL) | 0x10000000000000L;

See also:
https://docs.oracle.com/javase/7/docs/api/java/lang/Double.html#longBitsToDouble(long)
https://docs.oracle.com/javase/7/docs/api/java/lang/Float.html#floatToIntBits(float)
https://docs.oracle.com/javase/7/docs/api/java/lang/Float.html#intBitsToFloat(int)
https://docs.oracle.com/javase/7/docs/api/java/lang/Double.html#DoubleToLongBits(double)

https://en.wikipedia.org/wiki/IEEE_754-1985
https://en.wikipedia.org/wiki/IEEE_754-2008
https://en.wikipedia.org/wiki/Floating-point_arithmetic#IEEE_754:_floating_point_in_modern_computers
http://www.quadibloc.com/comp/cp0201.htm
*/


/*************************************************************************
  Item_func_float_to_int32_bits
*************************************************************************/

class Item_func_float_to_int32_bits : public Item_long_func {
  bool check_arguments() const {
    return check_argument_types_can_return_real(0, arg_count);
  }

 public:
  Item_func_float_to_int32_bits(THD *thd, Item *a)
      : Item_long_func(thd, a) {}
  ulonglong val_uint();
  longlong val_int();
  const char *func_name() const { return "float_to_int32_bits"; }
  bool fix_length_and_dec() {
    decimals= 0;
    max_length= MAX_INT_WIDTH;
    maybe_null= args[0]->maybe_null;
    unsigned_flag= 0;
    return FALSE;
  }
  Item *get_copy(THD *thd) {
    return get_item_copy<Item_func_float_to_int32_bits>(thd, this);
  }
};

/*************************************************************************
  Item_func_int32_bits_to_float
*************************************************************************/

class Item_func_int32_bits_to_float : public Item_real_func {
  bool check_arguments() const {
    return check_argument_types_can_return_int(0, arg_count);
  }

 public:
  Item_func_int32_bits_to_float(THD *thd, Item *a)
      : Item_real_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "uint32_bits_to_float"; }
  bool fix_length_and_dec() {
    maybe_null= 1; // bits may represent +/-inf or +/-nan
    decimals= NOT_FIXED_DEC;
    return FALSE;
  }
  Item *get_copy(THD *thd) {
    return get_item_copy<Item_func_int32_bits_to_float>(thd, this);
  }
};


/*************************************************************************
  Item_func_double_to_int64_bits
*************************************************************************/

class Item_func_double_to_int64_bits : public Item_longlong_func {
  bool check_arguments() const {
    return check_argument_types_can_return_real(0, arg_count);
  }

 public:
  Item_func_double_to_int64_bits(THD *thd, Item *a)
      : Item_longlong_func(thd, a) {}
  ulonglong val_uint();
  longlong val_int();
  const char *func_name() const { return "double_to_int64_bits"; }
  bool fix_length_and_dec() {
    decimals= 0;
    max_length= MAX_BIGINT_WIDTH;
    maybe_null= args[0]->maybe_null;
    unsigned_flag= 0;
    return FALSE;
  }
  Item *get_copy(THD *thd) {
    return get_item_copy<Item_func_double_to_int64_bits>(thd, this);
  }
};

/*************************************************************************
  Item_func_int64_bits_to_double
*************************************************************************/

class Item_func_int64_bits_to_double : public Item_real_func {
  bool check_arguments() const {
    return check_argument_types_can_return_int(0, arg_count);
  }

 public:
  Item_func_int64_bits_to_double(THD *thd, Item *a)
      : Item_real_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "uint64_bits_to_double"; }
  bool fix_length_and_dec() {
    maybe_null= 1; // bits may represent +/-inf or +/-nan
    decimals= NOT_FIXED_DEC;
    return FALSE;
  }
  Item *get_copy(THD *thd) {
    return get_item_copy<Item_func_int64_bits_to_double>(thd, this);
  }
};

#endif  // ITEM_FLOAT_BITS_INCLUDED
