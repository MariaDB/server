/* Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.

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

#ifndef PASSWORD_INCLUDED
#define PASSWORD_INCLUDED

/*
  SCRAMBLE_LENGTH_323 and SCRAMBLED_PASSWORD_CHAR_LENGTH_323 may
  already be defined via mysql_com.h. Define them here as well so
  that translation units which do not include mysql_com.h (e.g.
  mariadb-install-db via libmariadb headers) can still use them.
*/
#ifndef SCRAMBLE_LENGTH_323
#define SCRAMBLE_LENGTH_323 8
#endif
#ifndef SCRAMBLED_PASSWORD_CHAR_LENGTH_323
#define SCRAMBLED_PASSWORD_CHAR_LENGTH_323 (SCRAMBLE_LENGTH_323 * 2)
#endif

C_MODE_START

void my_make_scrambled_password_323(char *to, const char *password,
                                    size_t pass_len);
void my_make_scrambled_password(char *to, const char *password,
                                size_t pass_len);

void hash_password(ulong *result, const char *password, uint password_len);

C_MODE_END

#endif /* PASSWORD_INCLUDED */
