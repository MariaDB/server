/*
   Copyright (c) 2011, 2018 MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/*
  This file contains definitions and functions for
  the interface between the auth_pam.so (PAM plugin version 2)
  and the auth_pam_tool executable.
  To be included both in auth_pam.c and auth_pam_tool.c.
*/

#define AP_AUTHENTICATED_AS 'A'
#define AP_CONV 'C'
#define AP_EOF 'E'


static int read_length(int file)
{
  unsigned char hdr[2];

  if (read(file, hdr, 2) < 2)
    return -1;

  return (((int) hdr[0]) << 8) + (int) hdr[1];
}


static void store_length(int len, unsigned char *p_len)
{
  p_len[0]= (unsigned char) ((len >> 8) & 0xFF);
  p_len[1]= (unsigned char) (len  & 0xFF);
}


/*
  Returns the length of the string read,
  or -1 on error.
*/

static int read_string(int file, char *s, int s_size)
{
  int len;

  len= read_length(file);

  if (len < 0 || len > s_size-1 ||
      read(file, s, len) < len)
    return -1;

  s[len]= 0;

  return len;
}


/*
  Returns 0 on success.
*/

static int write_string(int file, const unsigned char *s, int s_len)
{
  unsigned char hdr[2];
  store_length(s_len, hdr);
  return write(file, hdr, 2) < 2 ||
         write(file, s, s_len) < s_len;
}


#define MAX_PAM_SERVICE_NAME 1024
