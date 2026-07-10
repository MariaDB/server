#ifndef SQL_TYPE_UUID_V7_INCLUDED
#define SQL_TYPE_UUID_V7_INCLUDED

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
  implements Universal Universally Unique Identifier version 7, as described in
  RFC 9562.

  A UUIDv7 has the following structure:

  Field                       Octet #          Note
 unix_ts_ms                    0-5     Big-endian unsigned number of
                                       Unix epoch timestamp in
                                       milliseconds.
 ver                           6       The 4 bit version field, set to
                                       0b0100. Occupies bits 48 through 51 of
                                       octet 6.
 sub_ms_precision              6-7     Sub millisecond clock precision
                                       encoded to fill all the possible
                                       values in 12 bits.
 var                           8       The 2 bit variant field, set to 0b10.
                                       Occupies bits 64 and 65 of octet 8.
 rand                          8-15    CSPRNG 62 bits multiplexed
                                       with the version number.

  The structure of an UUIDv7 is: mmmmmmmm-mmmm-Vsss-vrrr-rrrrrrrrrrrr
*/

#include "sql_type_uuid.h"
#include <m_string.h>
#include <myisampack.h> /* mi_int2store, mi_int6store */
#include <errmsg.h>

extern uint64 last_uuidv7_timestamp;
extern mysql_mutex_t LOCK_uuid_v7_generator;

class UUIDv7: public Type_handler_uuid_new::Fbt
{
  static constexpr uchar UUID_VERSION()      { return 0x70; }
  static constexpr uchar UUID_VARIANT()      { return 0x80; }

  static void inject_version_and_variant(uchar *to)
  {
    to[6]= ((to[6] & UUID_VERSION_MASK()) | UUID_VERSION());
    to[8]= ((to[8] & UUID_VARIANT_MASK()) | UUID_VARIANT());
  }

  static void construct(char *to)
  {
    bool error= my_random_bytes((uchar*) to+8, 8) != MY_AES_OK;
    DBUG_EXECUTE_IF("simulate_uuidv7_my_random_bytes_failure", error= true; );

    if (error) // A very unlikely failure happened.
    {
      push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_UNKNOWN_ERROR,
                          "UUID_v7: RANDOM_BYTES() failed, using fallback");
    }

    /*
      We have 12 bits to ensure monotonicity. Let's store microseconds
      there (from 0 to 999) as described in section 6.2, Method 3 of RFC 9562,
      and use two remaining bits as a counter, thus allowing 4000 UUIDv7
      values to be generated within one millisecond.
    */
    uint64 tv= my_hrtime().val * 4;
    mysql_mutex_lock(&LOCK_uuid_v7_generator);
    last_uuidv7_timestamp= tv= MY_MAX(last_uuidv7_timestamp+1, tv);
    mysql_mutex_unlock(&LOCK_uuid_v7_generator);

    mi_int6store(to, tv / 4000);
    mi_int2store(to+6, tv % 4000);

    /*
      Let's inject proper version and variant to make it good UUIDv7.
    */
    inject_version_and_variant((uchar*) to);
  }

public:

  UUIDv7()
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

#endif // SQL_TYPE_UUID_V7_INCLUDED
