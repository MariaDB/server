/* Copyright (C) 2009 Codership Oy <info@codersihp.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*! @file Helper functions to deal with history UUID string representations */

#include <errno.h>
#include <ctype.h>
#include <stdio.h>

#include "wsrep_api.h"

/*!
 * Read UUID from string
 * @return length of UUID string representation or -EINVAL in case of error
 */
ssize_t
wsrep_uuid_scan (const char* str, size_t str_len, wsrep_uuid_t* uuid)
{
    size_t uuid_len = 0;
    size_t uuid_offt = 0;

    while (uuid_len + 1 < str_len) {
        if ((4  == uuid_offt || 6 == uuid_offt || 8 == uuid_offt ||
             10 == uuid_offt) && str[uuid_len] == '-') {
            // skip dashes after 4th, 6th, 8th and 10th positions
            uuid_len += 1;
            continue;
        }
        if (isxdigit(str[uuid_len]) && isxdigit(str[uuid_len + 1])) {
            // got hex digit
            sscanf (str + uuid_len, "%2hhx", uuid->uuid + uuid_offt);
            uuid_len  += 2;
            uuid_offt += 1;
            if (sizeof (uuid->uuid) == uuid_offt)
                return uuid_len;
        }
        else {
            break;
        }
    }

    *uuid = WSREP_UUID_UNDEFINED;
    return -EINVAL;
}

/*!
 * Write UUID to string
 * @return length of UUID string representation or -EMSGSIZE if string is too
 *         short
 */
ssize_t
wsrep_uuid_print (const wsrep_uuid_t* uuid, char* str, size_t str_len)
{
    if (str_len > 36) {
        const unsigned char* u = uuid->uuid;
        return snprintf(str, str_len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
                        "%02x%02x-%02x%02x%02x%02x%02x%02x",
                        u[ 0], u[ 1], u[ 2], u[ 3], u[ 4], u[ 5], u[ 6], u[ 7],
                        u[ 8], u[ 9], u[10], u[11], u[12], u[13], u[14], u[15]);
    }
    else {
        return -EMSGSIZE;
    }
}

