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
    if (m_value == LONGLONG_MIN) // avoid undefined behavior
      return ((ulonglong) LONGLONG_MAX) + 1;
    return m_value < 0 ? -m_value : m_value;
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


#endif // SQL_TYPE_INT_INCLUDED
