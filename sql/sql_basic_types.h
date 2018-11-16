/*
   Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2009, 2016, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */

/* File that includes common types used globally in MariaDB */

#ifndef SQL_TYPES_INCLUDED
#define SQL_TYPES_INCLUDED

typedef ulonglong sql_mode_t;
typedef int64 query_id_t;


/*
  "fuzzydate" with strict data type control.
  Please keep "explicit" in constructors and conversion methods.
*/
class date_mode_t
{
public:
  enum value_t
  {
    /*
      FUZZY_DATES is used for the result will only be used for comparison
      purposes. Conversion is as relaxed as possible.
    */
    FUZZY_DATES=        1U,
    TIME_ONLY=          4U,
    INTERVAL_hhmmssff=  8U,
    INTERVAL_DAY=      16U,
    NO_ZERO_IN_DATE=   (1UL << 23),  // MODE_NO_ZERO_IN_DATE
    NO_ZERO_DATE=      (1UL << 24),  // MODE_NO_ZERO_DATE
    INVALID_DATES=     (1UL << 25)   // MODE_INVALID_DATES
  };

private:
  value_t m_mode;
public:

  // Constructors
  explicit date_mode_t(ulonglong fuzzydate)
   :m_mode((value_t) fuzzydate)
  { }

  // Conversion operators
  explicit operator ulonglong() const
  {
    return m_mode;
  }
  explicit operator bool() const
  {
    return m_mode != 0;
  }

  // Unary operators
  date_mode_t operator~() const
  {
    return date_mode_t(~m_mode);
  }

  // Dyadic bitwise operators
  date_mode_t operator&(const date_mode_t &other) const
  {
    return date_mode_t(m_mode & other.m_mode);
  }

  date_mode_t operator|(const date_mode_t &other) const
  {
    return date_mode_t(m_mode | other.m_mode);
  }

  // Dyadic bitwise assignment operators
  date_mode_t &operator&=(const date_mode_t &other)
  {
    m_mode= value_t(m_mode & other.m_mode);
    return *this;
  }

  date_mode_t &operator|=(const date_mode_t &other)
  {
    m_mode= value_t(m_mode | other.m_mode);
    return *this;
  }
};


const date_mode_t
  TIME_FUZZY_DATES            (date_mode_t::value_t::FUZZY_DATES),
  TIME_TIME_ONLY              (date_mode_t::value_t::TIME_ONLY),
  TIME_INTERVAL_hhmmssff      (date_mode_t::value_t::INTERVAL_hhmmssff),
  TIME_INTERVAL_DAY           (date_mode_t::value_t::INTERVAL_DAY),
  TIME_NO_ZERO_IN_DATE        (date_mode_t::value_t::NO_ZERO_IN_DATE),
  TIME_NO_ZERO_DATE           (date_mode_t::value_t::NO_ZERO_DATE),
  TIME_INVALID_DATES          (date_mode_t::value_t::INVALID_DATES);

// Flags understood by str_to_xxx, number_to_xxx, check_date
static const date_mode_t
  TIME_MODE_FOR_XXX_TO_DATE   (date_mode_t::NO_ZERO_IN_DATE |
                               date_mode_t::NO_ZERO_DATE    |
                               date_mode_t::INVALID_DATES);

#endif
