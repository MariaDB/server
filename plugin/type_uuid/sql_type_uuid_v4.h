#ifndef SQL_TYPE_UUID_V4_INCLUDED
#define SQL_TYPE_UUID_V4_INCLUDED

/* Copyright (c) 2024, Stefano Petrilli

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

/*
  Implements Universal Unique Identifiers version 4, as described in
  RFC 9562.

    Field                       Octet #          Note
  random_a                       0-5     Random CSPRNG 48 bits.
  ver                            6       The 4 bit version field, set to
                                         0b0100. Occupies bits 48 through 51 of
                                         octet 6.
  random_b                       6-7     Random CSPRNG 12 bits.
  var                            8       The 2 bit variant field, set to 0b10.
                                         Occupies bits 64 and 65 of octet 8.
  random_c                       8-15    Random CSPRNG 62 bits.

  The structure of an UUIDv4 is: llllllll-mmmm-Vhhh-vsss-nnnnnnnnnnnn
  The replacement of the version and variant field bits results in 122
  bits of random data.
*/

#include "sql_type_uuid.h"

class UUIDv4: public Type_handler_uuid_new::Fbt
{
  static constexpr uchar UUID_VERSION()      { return 0x40; }
  static constexpr uchar UUID_VARIANT()      { return 0x80; }

  static void inject_version_and_variant(uchar *to)
  {
    to[6]= ((to[6] & UUID_VERSION_MASK()) | UUID_VERSION());
    to[8]= ((to[8] & UUID_VARIANT_MASK()) | UUID_VARIANT());
  }

  static void construct(char *to)
  {
    bool error= my_random_bytes((uchar*) to, 16) != MY_AES_OK;
    DBUG_EXECUTE_IF("simulate_uuidv4_my_random_bytes_failure", error= true; );

    if (error) // A very unlikely failure happened.
    {
      push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_UNKNOWN_ERROR,
                          "UUID_v4: RANDOM_BYTES() failed, using fallback");
    }
    /*
      We have random bytes at to[6] and to[8].
      Let's inject proper version and variant to make it good UUIDv4.
    */
    inject_version_and_variant((uchar*) to);
  }

public:

  UUIDv4()
  {
    construct(m_buffer);
  }
  static bool construct_native(Native *to)
  {
    to->alloc(MY_UUID_SIZE);
    to->length(MY_UUID_SIZE);
    construct((char*)to->ptr());
    return 0;
  }

};

#endif // SQL_TYPE_UUID_V4_INCLUDED
