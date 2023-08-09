/* Copyright (c) 2018, 2021, MariaDB

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

#ifndef SQL_TYPE_INT_INCLUDED
#define SQL_TYPE_INT_INCLUDED

#include "my_bit.h" // my_count_bits()


class Null_flag
{
protected:
  bool m_is_null;
public:
  bool is_null() const { return m_is_null; }
  Null_flag(bool is_null) :m_is_null(is_null) { }
};


class Longlong
{
protected:
  longlong m_value;
public:
  longlong value() const { return m_value; }
  Longlong(longlong nr) :m_value(nr) { }
  ulonglong abs()
  {
    if (m_value == LONGLONG_MIN) // avoid undefined behavior
      return ((ulonglong) LONGLONG_MAX) + 1;
    return m_value < 0 ? -m_value : m_value;
  }
};


class Longlong_null: public Longlong, public Null_flag
{
public:
  Longlong_null(longlong nr, bool is_null)
   :Longlong(nr), Null_flag(is_null)
  { }
  explicit Longlong_null()
   :Longlong(0), Null_flag(true)
  { }
  explicit Longlong_null(longlong nr)
   :Longlong(nr), Null_flag(false)
  { }
  Longlong_null operator|(const Longlong_null &other) const
  {
    if (is_null() || other.is_null())
      return Longlong_null();
    return Longlong_null(value() | other.value());
  }
  Longlong_null operator&(const Longlong_null &other) const
  {
    if (is_null() || other.is_null())
      return Longlong_null();
    return Longlong_null(value() & other.value());
  }
  Longlong_null operator^(const Longlong_null &other) const
  {
    if (is_null() || other.is_null())
      return Longlong_null();
    return Longlong_null((longlong) (value() ^ other.value()));
  }
  Longlong_null operator~() const
  {
    if (is_null())
      return *this;
    return Longlong_null((longlong) ~ (ulonglong) value());
  }
  Longlong_null operator<<(const Longlong_null &llshift) const
  {
    ulonglong res;
    uint shift;
    if (is_null() || llshift.is_null())
      return Longlong_null();
    shift= (uint) llshift.value();
    res= 0;
    if (shift < sizeof(longlong) * 8)
      res= ((ulonglong) value()) << shift;
    return Longlong_null((longlong) res);
  }
  Longlong_null operator>>(const Longlong_null &llshift) const
  {
    ulonglong res;
    uint shift;
    if (is_null() || llshift.is_null())
      return Longlong_null();
    shift= (uint) llshift.value();
    res= 0;
    if (shift < sizeof(longlong) * 8)
      res= ((ulonglong) value()) >> shift;
    return Longlong_null(res);
  }
  Longlong_null bit_count() const
  {
    if (is_null())
      return *this;
    return Longlong_null((longlong) my_count_bits((ulonglong) value()));
  }
};


class ULonglong
{
protected:
  ulonglong m_value;
public:
  ulonglong value() const { return m_value; }
  explicit ULonglong(ulonglong nr) :m_value(nr) { }

  static bool test_if_sum_overflows_ull(ulonglong arg1, ulonglong arg2)
  {
    return ULONGLONG_MAX - arg1 < arg2;
  }

  Longlong_null operator-() const
  {
    if (m_value > (ulonglong) LONGLONG_MAX) // Avoid undefined behaviour
    {
      return m_value == (ulonglong) LONGLONG_MAX + 1 ?
             Longlong_null(LONGLONG_MIN, false) :
             Longlong_null(0, true);
    }
    return Longlong_null(-(longlong) m_value, false);
  }

  // Convert to Longlong_null with the range check
  Longlong_null to_longlong_null() const
  {
    if (m_value > (ulonglong) LONGLONG_MAX)
      return Longlong_null(0, true);
    return Longlong_null((longlong) m_value, false);
  }

};


class ULonglong_null: public ULonglong, public Null_flag
{
public:
  ULonglong_null(ulonglong nr, bool is_null)
   :ULonglong(nr), Null_flag(is_null)
  { }

  /*
    Multiply two ulonglong values.

    Let a = a1 * 2^32 + a0 and b = b1 * 2^32 + b0. Then
    a * b = (a1 * 2^32 + a0) * (b1 * 2^32 + b0) = a1 * b1 * 2^64 +
            + (a1 * b0 + a0 * b1) * 2^32 + a0 * b0;
    We can determine if the above sum overflows the ulonglong range by
    sequentially checking the following conditions:
    1. If both a1 and b1 are non-zero.
    2. Otherwise, if (a1 * b0 + a0 * b1) is greater than ULONG_MAX.
    3. Otherwise, if (a1 * b0 + a0 * b1) * 2^32 + a0 * b0 is greater than
    ULONGLONG_MAX.
  */
  static ULonglong_null ullmul(ulonglong a, ulonglong b)
  {
    ulong a1= (ulong)(a >> 32);
    ulong b1= (ulong)(b >> 32);

    if (a1 && b1)
      return ULonglong_null(0, true);

    ulong a0= (ulong)(0xFFFFFFFFUL & a);
    ulong b0= (ulong)(0xFFFFFFFFUL & b);

    ulonglong res1= (ulonglong) a1 * b0 + (ulonglong) a0 * b1;
    if (res1 > 0xFFFFFFFFUL)
      return ULonglong_null(0, true);

    res1= res1 << 32;
    ulonglong res0= (ulonglong) a0 * b0;

    if (test_if_sum_overflows_ull(res1, res0))
      return ULonglong_null(0, true);
    return ULonglong_null(res1 + res0, false);
  }
};


// A longlong/ulonglong hybrid. Good to store results of val_int().
class Longlong_hybrid: public Longlong
{
protected:
  bool m_unsigned;
  int cmp_signed(const Longlong_hybrid& other) const
  {
    return m_value < other.m_value ? -1 : m_value == other.m_value ? 0 : 1;
  }
  int cmp_unsigned(const Longlong_hybrid& other) const
  {
    return (ulonglong) m_value < (ulonglong) other.m_value ? -1 :
            m_value == other.m_value ? 0 : 1;
  }
public:
  Longlong_hybrid(longlong nr, bool unsigned_flag)
   :Longlong(nr), m_unsigned(unsigned_flag)
  { }
  bool is_unsigned() const { return m_unsigned; }
  bool is_unsigned_outside_of_signed_range() const
  {
    return m_unsigned && ((ulonglong) m_value) > (ulonglong) LONGLONG_MAX;
  }
  bool neg() const { return m_value < 0 && !m_unsigned; }
  ulonglong abs() const
  {
    if (m_unsigned)
      return (ulonglong) m_value;
    return Longlong(m_value).abs();
  }
  /*
    Convert to an unsigned number:
    - Negative numbers are converted to 0.
    - Positive numbers bigger than upper_bound are converted to upper_bound.
    - Other numbers are returned as is.
  */
  ulonglong to_ulonglong(ulonglong upper_bound) const
  {
    return neg() ? 0 :
           (ulonglong) m_value > upper_bound ? upper_bound :
           (ulonglong) m_value;
  }
  uint to_uint(uint upper_bound) const
  {
    return (uint) to_ulonglong(upper_bound);
  }


  Longlong_null val_int_signed() const
  {
    if (m_unsigned)
      return ULonglong((ulonglong) m_value).to_longlong_null();
    return Longlong_null(m_value, false);
  }

  Longlong_null val_int_unsigned() const
  {
    if (!m_unsigned && m_value < 0)
      return Longlong_null(0, true);
    return Longlong_null(m_value, false);
  }

  /*
    Return in Item compatible val_int() format:
    - signed numbers as a straight longlong value
    - unsigned numbers as a ulonglong value reinterpreted to longlong
  */
  Longlong_null val_int(bool want_unsigned_value) const
  {
    return want_unsigned_value ? val_int_unsigned() :
                                 val_int_signed();
  }

  int cmp(const Longlong_hybrid& other) const
  {
    if (m_unsigned == other.m_unsigned)
      return m_unsigned ? cmp_unsigned(other) : cmp_signed(other);
    if (is_unsigned_outside_of_signed_range())
      return 1;
    if (other.is_unsigned_outside_of_signed_range())
      return -1;
    /*
      The unsigned argument is in the range 0..LONGLONG_MAX.
      The signed argument is in the range LONGLONG_MIN..LONGLONG_MAX.
      Safe to compare as signed.
    */
    return cmp_signed(other);
  }
  bool operator==(const Longlong_hybrid &nr) const
  {
    return cmp(nr) == 0;
  }
  bool operator==(ulonglong nr) const
  {
    return cmp(Longlong_hybrid((longlong) nr, true)) == 0;
  }
  bool operator==(uint nr) const
  {
    return cmp(Longlong_hybrid((longlong) nr, true)) == 0;
  }
  bool operator==(longlong nr) const
  {
    return cmp(Longlong_hybrid(nr, false)) == 0;
  }
  bool operator==(int nr) const
  {
    return cmp(Longlong_hybrid(nr, false)) == 0;
  }
};


class Longlong_hybrid_null: public Longlong_hybrid,
                            public Null_flag
{
public:
  Longlong_hybrid_null(const Longlong_null &nr, bool unsigned_flag)
   :Longlong_hybrid(nr.value(), unsigned_flag),
    Null_flag(nr.is_null())
  { }
};


/*
  Stores the absolute value of a number, and the sign.
  Value range: -ULONGLONG_MAX .. +ULONGLONG_MAX.

  Provides a wider range for negative numbers than Longlong_hybrid does.
  Usefull to store intermediate results of an expression whose value
  is further needed to be negated. For example, these methods:
    - Item_func_mul::int_op()
    - Item_func_int_div::val_int()
    - Item_func_mod::int_op()
  calculate the result of absolute values of the arguments,
  then optionally negate the result.
*/
class ULonglong_hybrid: public ULonglong
{
  bool m_neg;
public:
  ULonglong_hybrid(ulonglong value, bool neg)
   :ULonglong(value), m_neg(neg)
  {
    if (m_neg && !m_value)
      m_neg= false;        // convert -0 to +0
  }
  Longlong_null val_int_unsigned() const
  {
    return m_neg ? Longlong_null(0, true) :
                   Longlong_null((longlong) m_value, false);
  }
  Longlong_null val_int_signed() const
  {
    return m_neg ? -ULonglong(m_value) : ULonglong::to_longlong_null();
  }

  /*
    Return in Item compatible val_int() format:
    - signed numbers as a straight longlong value
    - unsigned numbers as a ulonglong value reinterpreted to longlong
  */
  Longlong_null val_int(bool want_unsigned_value) const
  {
    return want_unsigned_value ? val_int_unsigned() :
                                 val_int_signed();
  }
};


#endif // SQL_TYPE_INT_INCLUDED
