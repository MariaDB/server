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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/* File that includes common types used globally in MariaDB */

#ifndef SQL_TYPES_INCLUDED
#define SQL_TYPES_INCLUDED

typedef ulonglong sql_mode_t;
typedef int64 query_id_t;

enum enum_nullability { NOT_NULL, NULLABLE };


/*
  "fuzzydate" with strict data type control.
  Represents a mixture of *only* data type conversion flags, without rounding.
  Please keep "explicit" in constructors and conversion methods.
*/
class date_conv_mode_t
{
public:
  enum value_t
  {
    CONV_NONE=          0U,
    /*
      FUZZY_DATES is used for the result will only be used for comparison
      purposes. Conversion is as relaxed as possible.
    */
    FUZZY_DATES=        1U,
    TIME_ONLY=          4U,
    INTERVAL_hhmmssff=  8U,
    INTERVAL_DAY=      16U,
    RANGE0_LAST=       INTERVAL_DAY,
    NO_ZERO_IN_DATE=   (1UL << 23),  // MODE_NO_ZERO_IN_DATE
    NO_ZERO_DATE=      (1UL << 24),  // MODE_NO_ZERO_DATE
    INVALID_DATES=     (1UL << 25)   // MODE_INVALID_DATES
  };

  /*
    BIT-OR for all known values. Let's have a separate enum for it.
    - We don't put this value "value_t", to avoid handling it in switch().
    - We don't put this value as a static const inside the class,
      because "gdb" would display it every time when we do "print"
      for a time_round_mode_t value.
    - We can't put into into a function returning this value, because
      it's not allowed to use functions in static_assert.
  */
  enum known_values_t
  {
    KNOWN_MODES= FUZZY_DATES |
                       TIME_ONLY | INTERVAL_hhmmssff | INTERVAL_DAY |
                       NO_ZERO_IN_DATE | NO_ZERO_DATE | INVALID_DATES
  };
private:
  value_t m_mode;
public:

  // Constructors
  explicit date_conv_mode_t(ulonglong fuzzydate)
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
  ulonglong operator~() const
  {
    return ~m_mode;
  }

  // Dyadic bitwise operators
  date_conv_mode_t operator&(const date_conv_mode_t &other) const
  {
    return date_conv_mode_t(m_mode & other.m_mode);
  }
  date_conv_mode_t operator&(const ulonglong other) const
  {
    return date_conv_mode_t(m_mode & other);
  }

  date_conv_mode_t operator|(const date_conv_mode_t &other) const
  {
    return date_conv_mode_t(m_mode | other.m_mode);
  }

  // Dyadic bitwise assignment operators
  date_conv_mode_t &operator&=(const date_conv_mode_t &other)
  {
    m_mode= value_t(m_mode & other.m_mode);
    return *this;
  }

  date_conv_mode_t &operator|=(const date_conv_mode_t &other)
  {
    m_mode= value_t(m_mode | other.m_mode);
    return *this;
  }
};


/*
  Fractional rounding mode for temporal data types.
*/
class time_round_mode_t
{
public:
  enum value_t
  {
    /*
      Use FRAC_NONE when the value needs no rounding nor truncation,
      because it is already known not to haveany fractional digits outside
      of the requested precision.
    */
    FRAC_NONE= 0,
    FRAC_TRUNCATE= date_conv_mode_t::RANGE0_LAST << 1,  // 32
    FRAC_ROUND=    date_conv_mode_t::RANGE0_LAST << 2   // 64
  };
  // BIT-OR for all known values. See comments in time_conv_mode_t.
  enum known_values_t
  {
    KNOWN_MODES= FRAC_TRUNCATE | FRAC_ROUND
  };
private:
  value_t m_mode;
public:
  // Constructors
  explicit time_round_mode_t(ulonglong mode)
   :m_mode((value_t) mode)
  {
#ifdef MYSQL_SERVER
    DBUG_ASSERT(mode == FRAC_NONE ||
                mode == FRAC_TRUNCATE ||
                mode == FRAC_ROUND);
#endif
  }
  // Conversion operators
  explicit operator ulonglong() const
  {
    return m_mode;
  }
  value_t mode() const
  {
    return m_mode;
  }
  // Comparison operators
  bool operator==(const time_round_mode_t &other)
  {
    return m_mode == other.m_mode;
  }
};


/*
  "fuzzydate" with strict data type control.
  Used as a parameter to get_date() and represents a mixture of:
  - data type conversion flags
  - fractional second rounding flags
  Please keep "explicit" in constructors and conversion methods.
*/
class date_mode_t
{
public:
  enum value_t
  {
    CONV_NONE=         date_conv_mode_t::CONV_NONE,         // 0
    FUZZY_DATES=       date_conv_mode_t::FUZZY_DATES,       // 1
    TIME_ONLY=         date_conv_mode_t::TIME_ONLY,         // 4
    INTERVAL_hhmmssff= date_conv_mode_t::INTERVAL_hhmmssff, // 8
    INTERVAL_DAY=      date_conv_mode_t::INTERVAL_DAY,      // 16
    FRAC_TRUNCATE=     time_round_mode_t::FRAC_TRUNCATE,    // 32
    FRAC_ROUND=        time_round_mode_t::FRAC_ROUND,       // 64
    NO_ZERO_IN_DATE=   date_conv_mode_t::NO_ZERO_IN_DATE,   // (1UL << 23)
    NO_ZERO_DATE=      date_conv_mode_t::NO_ZERO_DATE,      // (1UL << 24)
    INVALID_DATES=     date_conv_mode_t::INVALID_DATES,     // (1UL << 25)
  };
protected:
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
  explicit operator date_conv_mode_t() const
  {
    return date_conv_mode_t(ulonglong(m_mode) & date_conv_mode_t::KNOWN_MODES);
  }
  explicit operator time_round_mode_t() const
  {
    return time_round_mode_t(ulonglong(m_mode) & time_round_mode_t::KNOWN_MODES);
  }
  // Unary operators
  ulonglong operator~() const
  {
    return ~m_mode;
  }
  bool operator!() const
  {
    return !m_mode;
  }

  // Dyadic bitwise operators
  date_mode_t operator&(const date_mode_t &other) const
  {
    return date_mode_t(m_mode & other.m_mode);
  }
  date_mode_t operator&(ulonglong other) const
  {
    return date_mode_t(m_mode & other);
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

  date_mode_t &operator|=(const date_conv_mode_t &other)
  {
    m_mode= value_t(m_mode | ulonglong(other));
    return *this;
  }
};


// Bitwise OR out-of-class operators for data type mixtures
static inline date_mode_t operator|(const date_mode_t &a,
                                    const date_conv_mode_t &b)
{
  return date_mode_t(ulonglong(a) | ulonglong(b));
}

static inline date_mode_t operator|(const date_conv_mode_t &a,
                                    const time_round_mode_t &b)
{
  return date_mode_t(ulonglong(a) | ulonglong(b));
}


static inline date_mode_t operator|(const date_conv_mode_t &a,
                                    const date_mode_t &b)
{
  return date_mode_t(ulonglong(a) | ulonglong(b));
}


// Bitwise AND out-of-class operators for data type mixtures
static inline date_conv_mode_t operator&(const date_mode_t &a,
                                         const date_conv_mode_t &b)
{
  return date_conv_mode_t(ulonglong(a) & ulonglong(b));
}

static inline date_conv_mode_t operator&(const date_conv_mode_t &a,
                                         const date_mode_t &b)
{
  return date_conv_mode_t(ulonglong(a) & ulonglong(b));
}

static inline date_conv_mode_t operator&(sql_mode_t &a,
                                         const date_conv_mode_t &b)
{
  return date_conv_mode_t(a & ulonglong(b));
}


static const date_conv_mode_t
  TIME_CONV_NONE              (date_conv_mode_t::CONV_NONE),
  TIME_FUZZY_DATES            (date_conv_mode_t::FUZZY_DATES),
  TIME_TIME_ONLY              (date_conv_mode_t::TIME_ONLY),
  TIME_INTERVAL_hhmmssff      (date_conv_mode_t::INTERVAL_hhmmssff),
  TIME_INTERVAL_DAY           (date_conv_mode_t::INTERVAL_DAY),
  TIME_NO_ZERO_IN_DATE        (date_conv_mode_t::NO_ZERO_IN_DATE),
  TIME_NO_ZERO_DATE           (date_conv_mode_t::NO_ZERO_DATE),
  TIME_INVALID_DATES          (date_conv_mode_t::INVALID_DATES);

// An often used combination
static const date_conv_mode_t
  TIME_NO_ZEROS               (date_conv_mode_t::NO_ZERO_DATE|
                               date_conv_mode_t::NO_ZERO_IN_DATE);

// Flags understood by str_to_xxx, number_to_xxx, check_date
static const date_conv_mode_t
  TIME_MODE_FOR_XXX_TO_DATE   (date_mode_t::NO_ZERO_IN_DATE |
                               date_mode_t::NO_ZERO_DATE    |
                               date_mode_t::INVALID_DATES);

static const time_round_mode_t
  TIME_FRAC_NONE              (time_round_mode_t::FRAC_NONE),
  TIME_FRAC_TRUNCATE          (time_round_mode_t::FRAC_TRUNCATE),
  TIME_FRAC_ROUND             (time_round_mode_t::FRAC_ROUND);


#endif
