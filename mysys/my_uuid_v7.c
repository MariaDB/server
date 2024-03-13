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
  implements Universal Unique Identifiers version 7, as described in
  draft-ietf-uuidrev-rfc4122bis-14.

  A UUIDv7 has the following structure:

  Field                       Octet #          Note
 unix_ts_ms                    0-5     Big-endian unsigned number of
                                       Unix epoch timestamp in
                                       milliseconds.
 version_and_sub_ms_precision  6-7     Sub millisecond clock precision
                                       encoded to fill all the possible
                                       values in 12 bits and version.
 variant_and_rand              8-15    CSPRNG 62 bits multiplexed
                                       with the version number.
*/

#include "mysys_priv.h"
#include <my_rnd.h>
#include <m_string.h>
#include <myisampack.h> /* mi_int2store, mi_int4store */
#include <errmsg.h>

static my_bool my_uuid_v7_inited= 0;
static uint64 borrowed_microseconds;
static uint64 uuid_time= 0;

static mysql_mutex_t LOCK_uuid_v7_generator;

#define UUID_VERSION                         0x7000
#define UUID_VERSION_MASK                    0x0FFF
#define UUID_VARIANT                         0x8000000000000000
#define UUID_VARIANT_MASK                    0x3FFFFFFFFFFFFFFF
#define MICROSECONDS_TO_12BIT_MAPPING_FACTOR 4.096
#define MAX_BORROWED_MICROSECONDS            500000
#define SLEEP_MILLISECONDS                   250

/**
  Init structures needed for my_uuid_v7
*/
void my_uuid_v7_init()
{
  if (my_uuid_v7_inited)
    return;
  my_uuid_v7_inited= 1;
  borrowed_microseconds= 0;

  mysql_mutex_init(key_LOCK_uuid_v7_generator, &LOCK_uuid_v7_generator, MY_MUTEX_INIT_FAST);
}

/**
   Create a global unique identifier version 7(uuidv7)

   @func  my_uuid_v7()
   @param to   Store uuidv7 here. Must be of size MY_UUID_SIZE (16)
   @return 1 in case of success and 0 in case of failure
*/
int my_uuid_v7(uchar *to)
{
  uint64 tv, unix_ts_ms;
  uint16 sub_ms_precision, version_and_sub_ms_precision;
  uint64 rand, variant_and_rand;

  DBUG_ASSERT(my_uuid_v7_inited);

  tv= my_hrtime().val;

  // Regulate the access to uuid_time and microseconds
  mysql_mutex_lock(&LOCK_uuid_v7_generator);

  if (likely(tv > uuid_time))
  {
    /*
      Current time is ahead of last timestamp, as it should be.
      If we "borrowed time", give it back, just as long as we
      stay ahead of the previous timestamp.
    */
    if (borrowed_microseconds)
    {
      uint64 delta;
      /*
        -1 so we won't make tv= uuid_time for microseconds >= (tv - uuid_time)
      */
      delta= MY_MIN(borrowed_microseconds, (uint16)(tv - uuid_time -1));
      tv-= delta;
      borrowed_microseconds-= delta;
    }
  }
  else if (unlikely(tv <= uuid_time))
  {
    /*
      If several requests for UUIDs end up on the same tick
      or if the system clock is turned *back*.
      We add milli-seconds to make them different.
      ( current_timestamp + microseconds * calls_in_this_period )
      may end up > next_timestamp; this is OK. Nonetheless, we'll
      try to unwind microseconds when we get a chance to.
    */
    borrowed_microseconds+= uuid_time - tv + 1;
    tv = uuid_time + 1;

    if (borrowed_microseconds > MAX_BORROWED_MICROSECONDS)
    {
      /* We are building up too much borrowed time, >500 milliseconds.
         The output could become non-time-sortable if the server
         process restarts, causing `borrowed_microseconds` to reset to 0.

         Sleep until we have HALF of that maximum debt,
         in order to avoid repeatedly blocking and sleeping
         on successive calls.
      */
      my_sleep(SLEEP_MILLISECONDS);
      borrowed_microseconds-= MAX_BORROWED_MICROSECONDS / 2;
    }
  }

  uuid_time= tv;
  mysql_mutex_unlock(&LOCK_uuid_v7_generator);

  if (my_random_bytes((unsigned char *)&rand, (int) sizeof(rand)) != MY_AES_OK)
  {
    return 0;
  }

  unix_ts_ms=  (uint64) ((tv / 1000) & 0xFFFFFFFFFFFF);
  /**
   Map all the possible microseconds values (from 0 to 999) to all the
   values that can be represented in 12 bits (from 0 to 4095) as
   described in section 6.2, Method 3.
  */
  sub_ms_precision= (uint16)
    (MICROSECONDS_TO_12BIT_MAPPING_FACTOR * (tv % 1000));
  /**
   Clear bits 48-51 and 64-65 to 0 and then set them to the
   value specified by the UUIDv7 specification.
  */
  version_and_sub_ms_precision= (uint16)
    ((sub_ms_precision & UUID_VERSION_MASK) | UUID_VERSION);
  variant_and_rand= (uint64) ((rand & UUID_VARIANT_MASK) | UUID_VARIANT);

  mi_int6store(to, unix_ts_ms);
  mi_int2store(to+6, version_and_sub_ms_precision);
  mi_int8store(to+8, variant_and_rand);
  return 1;
}

void my_uuid_v7_end()
{
  if (my_uuid_v7_inited)
  {
    my_uuid_v7_inited= 0;
    mysql_mutex_destroy(&LOCK_uuid_v7_generator);
  }
}