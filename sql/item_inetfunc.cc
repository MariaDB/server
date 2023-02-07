/* Copyright (c) 2011, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2014 MariaDB Foundation

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

#include "mariadb.h"
#include "item_inetfunc.h"

#include "my_net.h"

///////////////////////////////////////////////////////////////////////////

static const size_t IN_ADDR_SIZE= 4;
static const size_t IN_ADDR_MAX_CHAR_LENGTH= 15;

static const size_t IN6_ADDR_SIZE= 16;
static const size_t IN6_ADDR_NUM_WORDS= IN6_ADDR_SIZE / 2;

/**
  Non-abbreviated syntax is 8 groups, up to 4 digits each,
  plus 7 delimiters between the groups.
  Abbreviated syntax is even shorter.
*/
static const uint IN6_ADDR_MAX_CHAR_LENGTH= 8 * 4 + 7;

static const char HEX_DIGITS[]= "0123456789abcdef";

///////////////////////////////////////////////////////////////////////////

longlong Item_func_inet_aton::val_int()
{
  DBUG_ASSERT(fixed);

  uint byte_result= 0;
  ulonglong result= 0;                    // We are ready for 64 bit addresses
  const char *p,* end;
  char c= '.'; // we mark c to indicate invalid IP in case length is 0
  int dot_count= 0;

  StringBuffer<36> tmp;
  String *s= args[0]->val_str_ascii(&tmp);

  if (!s)       // If null value
    goto err;

  null_value= 0;

  end= (p = s->ptr()) + s->length();
  while (p < end)
  {
    c= *p++;
    int digit= (int) (c - '0');
    if (digit >= 0 && digit <= 9)
    {
      if ((byte_result= byte_result * 10 + digit) > 255)
        goto err;                               // Wrong address
    }
    else if (c == '.')
    {
      dot_count++;
      result= (result << 8) + (ulonglong) byte_result;
      byte_result= 0;
    }
    else
      goto err;                                 // Invalid character
  }
  if (c != '.')                                 // IP number can't end on '.'
  {
    /*
      Attempt to support short forms of IP-addresses. It's however pretty
      basic one comparing to the BSD support.
      Examples:
        127     -> 0.0.0.127
        127.255 -> 127.0.0.255
        127.256 -> NULL (should have been 127.0.1.0)
        127.2.1 -> 127.2.0.1
    */
    switch (dot_count) {
    case 1: result<<= 8; /* Fall through */
    case 2: result<<= 8; /* Fall through */
    }
    return (result << 8) + (ulonglong) byte_result;
  }

err:
  null_value=1;
  return 0;
}


String* Item_func_inet_ntoa::val_str(String* str)
{
  DBUG_ASSERT(fixed);

  ulonglong n= (ulonglong) args[0]->val_int();

  /*
    We do not know if args[0] is NULL until we have called
    some val function on it if args[0] is not a constant!

    Also return null if n > 255.255.255.255
  */
  if ((null_value= (args[0]->null_value || n > 0xffffffff)))
    return 0;                                   // Null value

  str->set_charset(collation.collation);
  str->length(0);

  uchar buf[8];
  int4store(buf, n);

  /* Now we can assume little endian. */

  char num[4];
  num[3]= '.';

  for (uchar *p= buf + 4; p-- > buf;)
  {
    uint c= *p;
    uint n1, n2;                                // Try to avoid divisions
    n1= c / 100;                                // 100 digits
    c-= n1 * 100;
    n2= c / 10;                                 // 10 digits
    c-= n2 * 10;                                // last digit
    num[0]= (char) n1 + '0';
    num[1]= (char) n2 + '0';
    num[2]= (char) c + '0';
    uint length= (n1 ? 4 : n2 ? 3 : 2);         // Remove pre-zero
    uint dot_length= (p <= buf) ? 1 : 0;
    (void) str->append(num + 4 - length, length - dot_length,
                       &my_charset_latin1);
  }

  return str;
}

///////////////////////////////////////////////////////////////////////////


class Inet4
{
  char m_buffer[IN_ADDR_SIZE];
protected:
  bool ascii_to_ipv4(const char *str, size_t length);
  bool character_string_to_ipv4(const char *str, size_t str_length,
                                CHARSET_INFO *cs)
  {
    if (cs->state & MY_CS_NONASCII)
    {
      char tmp[IN_ADDR_MAX_CHAR_LENGTH];
      String_copier copier;
      uint length= copier.well_formed_copy(&my_charset_latin1, tmp, sizeof(tmp),
                                           cs, str, str_length);
      return ascii_to_ipv4(tmp, length);
    }
    return ascii_to_ipv4(str, str_length);
  }
  bool binary_to_ipv4(const char *str, size_t length)
  {
    if (length != sizeof(m_buffer))
      return true;
    memcpy(m_buffer, str, length);
    return false;
  }
  // Non-initializing constructor
  Inet4() = default;
public:
  void to_binary(char *dst, size_t dstsize) const
  {
    DBUG_ASSERT(dstsize >= sizeof(m_buffer));
    memcpy(dst, m_buffer, sizeof(m_buffer));
  }
  bool to_binary(String *to) const
  {
    return to->copy(m_buffer, sizeof(m_buffer), &my_charset_bin);
  }
  size_t to_string(char *dst, size_t dstsize) const;
  bool to_string(String *to) const
  {
    to->set_charset(&my_charset_latin1);
    if (to->alloc(INET_ADDRSTRLEN))
      return true;
    to->length((uint32) to_string((char*) to->ptr(), INET_ADDRSTRLEN));
    return false;
  }
};


class Inet4_null: public Inet4, public Null_flag
{
public:
  // Initialize from a text representation
  Inet4_null(const char *str, size_t length, CHARSET_INFO *cs)
   :Null_flag(character_string_to_ipv4(str, length, cs))
  { }
  Inet4_null(const String &str)
   :Inet4_null(str.ptr(), str.length(), str.charset())
  { }
  // Initialize from a binary representation
  Inet4_null(const char *str, size_t length)
   :Null_flag(binary_to_ipv4(str, length))
  { }
  Inet4_null(const Binary_string &str)
   :Inet4_null(str.ptr(), str.length())
  { }
public:
  const Inet4& to_inet4() const
  {
    DBUG_ASSERT(!is_null());
    return *this;
  }
  void to_binary(char *dst, size_t dstsize) const
  {
    to_inet4().to_binary(dst, dstsize);
  }
  bool to_binary(String *to) const
  {
    return to_inet4().to_binary(to);
  }
  size_t to_string(char *dst, size_t dstsize) const
  {
    return to_inet4().to_string(dst, dstsize);
  }
  bool to_string(String *to) const
  {
    return to_inet4().to_string(to);
  }
};


class Inet6
{
  char m_buffer[IN6_ADDR_SIZE];
protected:
  bool make_from_item(Item *item);
  bool ascii_to_ipv6(const char *str, size_t str_length);
  bool character_string_to_ipv6(const char *str, size_t str_length,
                                CHARSET_INFO *cs)
  {
    if (cs->state & MY_CS_NONASCII)
    {
      char tmp[IN6_ADDR_MAX_CHAR_LENGTH];
      String_copier copier;
      uint length= copier.well_formed_copy(&my_charset_latin1, tmp, sizeof(tmp),
                                           cs, str, str_length);
      return ascii_to_ipv6(tmp, length);
    }
    return ascii_to_ipv6(str, str_length);
  }
  bool binary_to_ipv6(const char *str, size_t length)
  {
    if (length != sizeof(m_buffer))
      return true;
    memcpy(m_buffer, str, length);
    return false;
  }
  // Non-initializing constructor
  Inet6() = default;
public:
  bool to_binary(String *to) const
  {
    return to->copy(m_buffer, sizeof(m_buffer), &my_charset_bin);
  }
  size_t to_string(char *dst, size_t dstsize) const;
  bool to_string(String *to) const
  {
    to->set_charset(&my_charset_latin1);
    if (to->alloc(INET6_ADDRSTRLEN))
      return true;
    to->length((uint32) to_string((char*) to->ptr(), INET6_ADDRSTRLEN));
    return false;
  }
  bool is_v4compat() const
  {
    static_assert(sizeof(in6_addr) == IN6_ADDR_SIZE, "unexpected in6_addr size");
    return IN6_IS_ADDR_V4COMPAT((struct in6_addr *) m_buffer);
  }
  bool is_v4mapped() const
  {
    static_assert(sizeof(in6_addr) == IN6_ADDR_SIZE, "unexpected in6_addr size");
    return IN6_IS_ADDR_V4MAPPED((struct in6_addr *) m_buffer);
  }
};


class Inet6_null: public Inet6, public Null_flag
{
public:
  // Initialize from a text representation
  Inet6_null(const char *str, size_t length, CHARSET_INFO *cs)
   :Null_flag(character_string_to_ipv6(str, length, cs))
  { }
  Inet6_null(const String &str)
   :Inet6_null(str.ptr(), str.length(), str.charset())
  { }
  // Initialize from a binary representation
  Inet6_null(const char *str, size_t length)
   :Null_flag(binary_to_ipv6(str, length))
  { }
  Inet6_null(const Binary_string &str)
   :Inet6_null(str.ptr(), str.length())
  { }
  // Initialize from an Item
  Inet6_null(Item *item)
   :Null_flag(make_from_item(item))
  { }
public:
  const Inet6& to_inet6() const
  {
    DBUG_ASSERT(!is_null());
    return *this;
  }
  bool to_binary(String *to) const
  {
    DBUG_ASSERT(!is_null());
    return to_inet6().to_binary(to);
  }
  size_t to_string(char *dst, size_t dstsize) const
  {
    return to_inet6().to_string(dst, dstsize);
  }
  bool to_string(String *to) const
  {
    return to_inet6().to_string(to);
  }
  bool is_v4compat() const
  {
    return to_inet6().is_v4compat();
  }
  bool is_v4mapped() const
  {
    return to_inet6().is_v4mapped();
  }
};


bool Inet6::make_from_item(Item *item)
{
  String tmp(m_buffer, sizeof(m_buffer), &my_charset_bin);
  String *str= item->val_str(&tmp);
  /*
    Charset could be tested in item->collation.collation before the val_str()
    call, but traditionally Inet6 functions still call item->val_str()
    for non-binary arguments and therefore execute side effects.
  */
  if (!str || str->length() != sizeof(m_buffer) ||
      str->charset() != &my_charset_bin)
    return true;
  if (str->ptr() != m_buffer)
      memcpy(m_buffer, str->ptr(), sizeof(m_buffer));
  return false;
};


/**
  Tries to convert given string to binary IPv4-address representation.
  This is a portable alternative to inet_pton(AF_INET).

  @param      str          String to convert.
  @param      str_length   String length.

  @return Completion status.
  @retval true  - error, the given string does not represent an IPv4-address.
  @retval false - ok, the string has been converted successfully.

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
  @retval false - ok, the string has been converted successfully.

  @note The problem with inet_pton() is that it treats leading zeros in
  IPv4-part differently on different platforms.
*/

bool Inet6::ascii_to_ipv6(const char *str, size_t str_length)
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

///////////////////////////////////////////////////////////////////////////

/**
  Converts IP-address-string to IP-address-data.

    ipv4-string -> varbinary(4)
    ipv6-string -> varbinary(16)

  @return Completion status.
  @retval NULL  Given string does not represent an IP-address.
  @retval !NULL The string has been converted successfully.
*/

String *Item_func_inet6_aton::val_str(String *buffer)
{
  DBUG_ASSERT(fixed);

  Ascii_ptr_and_buffer<STRING_BUFFER_USUAL_SIZE> tmp(args[0]);
  if ((null_value= tmp.is_null()))
    return NULL;

  Inet4_null ipv4(*tmp.string());
  if (!ipv4.is_null())
  {
    ipv4.to_binary(buffer);
    return buffer;
  }

  Inet6_null ipv6(*tmp.string());
  if (!ipv6.is_null())
  {
    ipv6.to_binary(buffer);
    return buffer;
  }

  null_value= true;
  return NULL;
}


/**
  Converts IP-address-data to IP-address-string.
*/

String *Item_func_inet6_ntoa::val_str_ascii(String *buffer)
{
  DBUG_ASSERT(fixed);

  // Binary string argument expected
  if (unlikely(args[0]->result_type() != STRING_RESULT ||
               args[0]->collation.collation != &my_charset_bin))
  {
    null_value= true;
    return NULL;
  }

  String_ptr_and_buffer<STRING_BUFFER_USUAL_SIZE> tmp(args[0]);
  if ((null_value= tmp.is_null()))
    return NULL;

  Inet4_null ipv4(static_cast<const Binary_string&>(*tmp.string()));
  if (!ipv4.is_null())
  {
    ipv4.to_string(buffer);
    return buffer;
  }

  Inet6_null ipv6(static_cast<const Binary_string&>(*tmp.string()));
  if (!ipv6.is_null())
  {
    ipv6.to_string(buffer);
    return buffer;
  }

  DBUG_PRINT("info", ("INET6_NTOA(): varbinary(4) or varbinary(16) expected."));
  null_value= true;
  return NULL;
}


/**
  Checks if the passed string represents an IPv4-address.
*/

longlong Item_func_is_ipv4::val_int()
{
  DBUG_ASSERT(fixed);
  String_ptr_and_buffer<STRING_BUFFER_USUAL_SIZE> tmp(args[0]);
  return !tmp.is_null() && !Inet4_null(*tmp.string()).is_null();
}


/**
  Checks if the passed string represents an IPv6-address.
*/

longlong Item_func_is_ipv6::val_int()
{
  DBUG_ASSERT(fixed);
  String_ptr_and_buffer<STRING_BUFFER_USUAL_SIZE> tmp(args[0]);
  return !tmp.is_null() && !Inet6_null(*tmp.string()).is_null();
}


/**
  Checks if the passed IPv6-address is an IPv4-compat IPv6-address.
*/

longlong Item_func_is_ipv4_compat::val_int()
{
  Inet6_null ip6(args[0]);
  return !ip6.is_null() && ip6.is_v4compat();
}


/**
  Checks if the passed IPv6-address is an IPv4-mapped IPv6-address.
*/

longlong Item_func_is_ipv4_mapped::val_int()
{
  Inet6_null ip6(args[0]);
  return !ip6.is_null() && ip6.is_v4mapped();
}
