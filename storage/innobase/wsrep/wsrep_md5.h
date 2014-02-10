#ifndef WSREP_MD5_INCLUDED
#define WSREP_MD5_INCLUDED

/* Copyright (c) 2014 SkySQL AB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/

#ifdef WITH_WSREP
void *wsrep_md5_init();
void wsrep_md5_update(void *ctx, char* buf, int len);
void wsrep_compute_md5_hash(char *digest, void *ctx);
#endif /* WITH_WSREP */

#endif /* WSREP_MD5_INCLUDED */
