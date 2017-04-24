#ifndef FIELD_COMP_H_INCLUDED
#define FIELD_COMP_H_INCLUDED
/* Copyright (C) 2017 MariaDB Foundation

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


#define MAX_COMPRESSION_METHODS 16

struct Compression_method
{
  const char *name;
  uint (*compress)(THD *thd, char *to, const char *from, uint length);
  int (*uncompress)(String *to, const uchar *from, uint from_length,
                    uint field_length);
};


extern Compression_method compression_methods[MAX_COMPRESSION_METHODS];
#define zlib_compression_method (&compression_methods[8])

#endif
