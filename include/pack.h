/* Copyright (c) 2016, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifdef __cplusplus
extern "C" {
#endif

ulong net_field_length(uchar **packet);
my_ulonglong net_field_length_ll(uchar **packet);
my_ulonglong safe_net_field_length_ll(uchar **packet, size_t packet_len);
uchar *net_store_length(uchar *pkg, ulonglong length);
uchar *safe_net_store_length(uchar *pkg, size_t pkg_len, ulonglong length);
unsigned int net_length_size(ulonglong num);

#ifdef __cplusplus
}
#endif
