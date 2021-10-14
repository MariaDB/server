/* Copyright (c) 2011, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2014 MariaDB Foundation
   Copyright (c) 2019,2021 MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#define MYSQL_SERVER
#include "mariadb.h"
#include "my_net.h"
#include "sql_class.h" // THD, SORT_FIELD_ATTR
#include "opt_range.h" // SEL_ARG
#include "sql_type_inet.h"

///////////////////////////////////////////////////////////////////////////

static const char HEX_DIGITS[]= "0123456789abcdef";


///////////////////////////////////////////////////////////////////////////

/**
  Tries to convert given string to binary IPv4-address representation.
  This is a portable alternative to inet_pton(AF_INET).

  @param      str          String to convert.
  @param      str_length   String length.

  @return Completion status.
  @retval true  - error, the given string does not represent an IPv4-address.
  @retval false - ok, the string has been converted sucessfully.

  @note The problem with inet_pton() is that it treats leading zeros in
  IPv4-part differently on different platforms.
*/

bool Inet4::ascii_to_ipv4(const char *str, size_t str_length)
{
  if (str_length < 7)
  {
    DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): "
                         "invalid IPv4 address: too short.",
                         (int) str_length, str));
    return true;
  }

  if (str_length > IN_ADDR_MAX_CHAR_LENGTH)
  {
    DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): "
                         "invalid IPv4 address: too long.",
                         (int) str_length, str));
    return true;
  }

  unsigned char *ipv4_bytes= (unsigned char *) &m_buffer;
  const char *str_end= str + str_length;
  const char *p= str;
  int byte_value= 0;
  int chars_in_group= 0;
  int dot_count= 0;
  char c= 0;

  while (p < str_end && *p)
  {
    c= *p++;

    if (my_isdigit(&my_charset_latin1, c))
    {
      ++chars_in_group;

      if (chars_in_group > 3)
      {
        DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                             "too many characters in a group.",
                             (int) str_length, str));
        return true;
      }

      byte_value= byte_value * 10 + (c - '0');

      if (byte_value > 255)
      {
        DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                             "invalid byte value.",
                             (int) str_length, str));
        return true;
      }
    }
    else if (c == '.')
    {
      if (chars_in_group == 0)
      {
        DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                             "too few characters in a group.",
                             (int) str_length, str));
        return true;
      }

      ipv4_bytes[dot_count]= (unsigned char) byte_value;

      ++dot_count;
      byte_value= 0;
      chars_in_group= 0;

      if (dot_count > 3)
      {
        DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                             "too many dots.", (int) str_length, str));
        return true;
      }
    }
    else
    {
      DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                           "invalid character at pos %d.",
                           (int) str_length, str, (int) (p - str)));
      return true;
    }
  }

  if (c == '.')
  {
    DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                         "ending at '.'.", (int) str_length, str));
    return true;
  }

  if (dot_count != 3)
  {
    DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                         "too few groups.",
                         (int) str_length, str));
    return true;
  }

  ipv4_bytes[3]= (unsigned char) byte_value;

  DBUG_PRINT("info", ("ascii_to_ipv4(%.*s): valid IPv4 address: %d.%d.%d.%d",
                      (int) str_length, str,
                      ipv4_bytes[0], ipv4_bytes[1],
                      ipv4_bytes[2], ipv4_bytes[3]));
  return false;
}


/**
  Tries to convert given string to binary IPv6-address representation.
  This is a portable alternative to inet_pton(AF_INET6).

  @param      str          String to convert.
  @param      str_length   String length.

  @return Completion status.
  @retval true  - error, the given string does not represent an IPv6-address.
  @retval false - ok, the string has been converted sucessfully.

  @note The problem with inet_pton() is that it treats leading zeros in
  IPv4-part differently on different platforms.
*/

bool Inet6::ascii_to_fbt(const char *str, size_t str_length)
{
  if (str_length < 2)
  {
    DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: too short.",
                         (int) str_length, str));
    return true;
  }

  if (str_length > IN6_ADDR_MAX_CHAR_LENGTH)
  {
    DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: too long.",
                         (int) str_length, str));
    return true;
  }

  memset(m_buffer, 0, sizeof(m_buffer));

  const char *p= str;

  if (*p == ':')
  {
    ++p;

    if (*p != ':')
    {
      DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                           "can not start with ':x'.", (int) str_length, str));
      return true;
    }
  }

  const char *str_end= str + str_length;
  char *ipv6_bytes_end= m_buffer + sizeof(m_buffer);
  char *dst= m_buffer;
  char *gap_ptr= NULL;
  const char *group_start_ptr= p;
  int chars_in_group= 0;
  int group_value= 0;

  while (p < str_end && *p)
  {
    char c= *p++;

    if (c == ':')
    {
      group_start_ptr= p;

      if (!chars_in_group)
      {
        if (gap_ptr)
        {
          DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                               "too many gaps(::).", (int) str_length, str));
          return true;
        }

        gap_ptr= dst;
        continue;
      }

      if (!*p || p >= str_end)
      {
        DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                             "ending at ':'.", (int) str_length, str));
        return true;
      }

      if (dst + 2 > ipv6_bytes_end)
      {
        DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                             "too many groups (1).", (int) str_length, str));
        return true;
      }

      dst[0]= (unsigned char) (group_value >> 8) & 0xff;
      dst[1]= (unsigned char) group_value & 0xff;
      dst += 2;

      chars_in_group= 0;
      group_value= 0;
    }
    else if (c == '.')
    {
      if (dst + IN_ADDR_SIZE > ipv6_bytes_end)
      {
        DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                             "unexpected IPv4-part.", (int) str_length, str));
        return true;
      }

      Inet4_null tmp(group_start_ptr, (size_t) (str_end - group_start_ptr),
                     &my_charset_latin1);
      if (tmp.is_null())
      {
        DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                             "invalid IPv4-part.", (int) str_length, str));
        return true;
      }

      tmp.to_binary(dst, IN_ADDR_SIZE);
      dst += IN_ADDR_SIZE;
      chars_in_group= 0;

      break;
    }
    else
    {
      const char *hdp= strchr(HEX_DIGITS, my_tolower(&my_charset_latin1, c));

      if (!hdp)
      {
        DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                             "invalid character at pos %d.",
                             (int) str_length, str, (int) (p - str)));
        return true;
      }

      if (chars_in_group >= 4)
      {
        DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                             "too many digits in group.",
                             (int) str_length, str));
        return true;
      }

      group_value <<= 4;
      group_value |= hdp - HEX_DIGITS;

      DBUG_ASSERT(group_value <= 0xffff);

      ++chars_in_group;
    }
  }

  if (chars_in_group > 0)
  {
    if (dst + 2 > ipv6_bytes_end)
    {
      DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                           "too many groups (2).", (int) str_length, str));
      return true;
    }

    dst[0]= (unsigned char) (group_value >> 8) & 0xff;
    dst[1]= (unsigned char) group_value & 0xff;
    dst += 2;
  }

  if (gap_ptr)
  {
    if (dst == ipv6_bytes_end)
    {
      DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                           "no room for a gap (::).", (int) str_length, str));
      return true;
    }

    int bytes_to_move= (int)(dst - gap_ptr);

    for (int i= 1; i <= bytes_to_move; ++i)
    {
      ipv6_bytes_end[-i]= gap_ptr[bytes_to_move - i];
      gap_ptr[bytes_to_move - i]= 0;
    }

    dst= ipv6_bytes_end;
  }

  if (dst < ipv6_bytes_end)
  {
    DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                         "too few groups.", (int) str_length, str));
    return true;
  }

  return false;
}


/**
  Converts IPv4-binary-address to a string. This function is a portable
  alternative to inet_ntop(AF_INET).

  @param[in] ipv4 IPv4-address data (byte array)
  @param[out] dst A buffer to store string representation of IPv4-address.
  @param[in]  dstsize Number of bytes avaiable in "dst"

  @note The problem with inet_ntop() is that it is available starting from
  Windows Vista, but the minimum supported version is Windows 2000.
*/

size_t Inet4::to_string(char *dst, size_t dstsize) const
{
  return (size_t) my_snprintf(dst, dstsize, "%d.%d.%d.%d",
                              (uchar) m_buffer[0], (uchar) m_buffer[1],
                              (uchar) m_buffer[2], (uchar) m_buffer[3]);
}


/**
  Converts IPv6-binary-address to a string. This function is a portable
  alternative to inet_ntop(AF_INET6).

  @param[in] ipv6 IPv6-address data (byte array)
  @param[out] dst A buffer to store string representation of IPv6-address.
                  It must be at least of INET6_ADDRSTRLEN.
  @param[in] dstsize Number of bytes available dst.

  @note The problem with inet_ntop() is that it is available starting from
  Windows Vista, but out the minimum supported version is Windows 2000.
*/

size_t Inet6::to_string(char *dst, size_t dstsize) const
{
  struct Region
  {
    int pos;
    int length;
  };

  const char *ipv6= m_buffer;
  char *dstend= dst + dstsize;
  const unsigned char *ipv6_bytes= (const unsigned char *) ipv6;

  // 1. Translate IPv6-address bytes to words.
  // We can't just cast to short, because it's not guaranteed
  // that sizeof (short) == 2. So, we have to make a copy.

  uint16 ipv6_words[IN6_ADDR_NUM_WORDS];

  DBUG_ASSERT(dstsize > 0); // Need a space at least for the trailing '\0'
  for (size_t i= 0; i < IN6_ADDR_NUM_WORDS; ++i)
    ipv6_words[i]= (ipv6_bytes[2 * i] << 8) + ipv6_bytes[2 * i + 1];

  // 2. Find "the gap" -- longest sequence of zeros in IPv6-address.

  Region gap= { -1, -1 };

  {
    Region rg= { -1, -1 };

    for (size_t i= 0; i < IN6_ADDR_NUM_WORDS; ++i)
    {
      if (ipv6_words[i] != 0)
      {
        if (rg.pos >= 0)
        {
          if (rg.length > gap.length)
            gap= rg;

          rg.pos= -1;
          rg.length= -1;
        }
      }
      else
      {
        if (rg.pos >= 0)
        {
          ++rg.length;
        }
        else
        {
          rg.pos= (int) i;
          rg.length= 1;
        }
      }
    }

    if (rg.pos >= 0)
    {
      if (rg.length > gap.length)
        gap= rg;
    }
  }

  // 3. Convert binary data to string.

  char *p= dst;

  for (int i= 0; i < (int) IN6_ADDR_NUM_WORDS; ++i)
  {
    DBUG_ASSERT(dstend >= p);
    size_t dstsize_available= dstend - p;
    if (dstsize_available < 5)
      break;
    if (i == gap.pos)
    {
      // We're at the gap position. We should put trailing ':' and jump to
      // the end of the gap.

      if (i == 0)
      {
        // The gap starts from the beginning of the data -- leading ':'
        // should be put additionally.

        *p= ':';
        ++p;
      }

      *p= ':';
      ++p;

      i += gap.length - 1;
    }
    else if (i == 6 && gap.pos == 0 &&
             (gap.length == 6 ||                           // IPv4-compatible
              (gap.length == 5 && ipv6_words[5] == 0xffff) // IPv4-mapped
             ))
    {
      // The data represents either IPv4-compatible or IPv4-mapped address.
      // The IPv6-part (zeros or zeros + ffff) has been already put into
      // the string (dst). Now it's time to dump IPv4-part.

      return (size_t) (p - dst) +
             Inet4_null((const char *) (ipv6_bytes + 12), 4).
               to_string(p, dstsize_available);
    }
    else
    {
      // Usual IPv6-address-field. Print it out using lower-case
      // hex-letters without leading zeros (recommended IPv6-format).
      //
      // If it is not the last field, append closing ':'.

      p += sprintf(p, "%x", ipv6_words[i]);

      if (i + 1 != IN6_ADDR_NUM_WORDS)
      {
        *p= ':';
        ++p;
      }
    }
  }

  *p= 0;
  return (size_t) (p - dst);
}

const Name &Inet6::default_value()
{
  static Name def(STRING_WITH_LEN("::"));
  return def;
}
