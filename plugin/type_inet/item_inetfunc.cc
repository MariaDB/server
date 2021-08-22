/* Copyright (c) 2011, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2014 MariaDB Foundation
   Copyright (c) 2019 MariaDB Corporation

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
#include "item_inetfunc.h"
#include "sql_type_inet.h"

///////////////////////////////////////////////////////////////////////////

longlong Item_func_inet_aton::val_int()
{
  DBUG_ASSERT(fixed());

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
  DBUG_ASSERT(fixed());

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

/**
  Converts IP-address-string to IP-address-data.

    ipv4-string -> varbinary(4)
    ipv6-string -> varbinary(16)

  @return Completion status.
  @retval NULL  Given string does not represent an IP-address.
  @retval !NULL The string has been converted sucessfully.
*/

String *Item_func_inet6_aton::val_str(String *buffer)
{
  DBUG_ASSERT(fixed());

  Ascii_ptr_and_buffer<STRING_BUFFER_USUAL_SIZE> tmp(args[0]);
  if ((null_value= tmp.is_null()))
    return NULL;

  Inet4_null ipv4(*tmp.string());
  if (!ipv4.is_null())
  {
    ipv4.to_binary(buffer);
    return buffer;
  }

  Inet6Bundle::Fbt_null ipv6(*tmp.string());
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
  DBUG_ASSERT(fixed());

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

  Inet6Bundle::Fbt_null ipv6(static_cast<const Binary_string&>(*tmp.string()));
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
  DBUG_ASSERT(fixed());
  String_ptr_and_buffer<STRING_BUFFER_USUAL_SIZE> tmp(args[0]);
  return !tmp.is_null() && !Inet4_null(*tmp.string()).is_null();
}

class IP6 : public Inet6Bundle::Fbt_null
{
public:
  IP6(Item* arg) : Inet6Bundle::Fbt_null(arg) {}
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


/**
  Checks if the passed string represents an IPv6-address.
*/

longlong Item_func_is_ipv6::val_int()
{
  DBUG_ASSERT(fixed());
  String_ptr_and_buffer<STRING_BUFFER_USUAL_SIZE> tmp(args[0]);
  return !tmp.is_null() && !Inet6Bundle::Fbt_null(*tmp.string()).is_null();
}

/**
  Checks if the passed IPv6-address is an IPv4-compat IPv6-address.
*/

longlong Item_func_is_ipv4_compat::val_int()
{
  IP6 ip6(args[0]);
  return !ip6.is_null() && ip6.is_v4compat();
}


/**
  Checks if the passed IPv6-address is an IPv4-mapped IPv6-address.
*/

longlong Item_func_is_ipv4_mapped::val_int()
{
  IP6 ip6(args[0]);
  return !ip6.is_null() && ip6.is_v4mapped();
}
