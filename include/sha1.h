#ifndef SHA1_INCLUDED
#define SHA1_INCLUDED

/* Copyright (c) 2013, Monty Program Ab

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include <mysql/service_sha1.h>
#define SHA1_HASH_SIZE MY_SHA1_HASH_SIZE
#define compute_sha1_hash(A,B,C) my_sha1(A,B,C)
#define compute_sha1_hash_multi(A,B,C,D,E) my_sha1_multi(A,B,C,D,E,NULL)

#endif /* SHA__INCLUDED */
