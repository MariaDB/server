/* Copyright (C) 2019 MariaDB Corporation AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1335 USA
*/

/*
  Types that are different in Aria from those used by MyISAM check tables
  in myisamchk.h
*/

typedef struct st_sort_key_blocks		/* Used when sorting */
{
  uchar *buff, *end_pos;
  uchar lastkey[MARIA_MAX_POSSIBLE_KEY_BUFF];
  uint last_length;
  int inited;
} MA_SORT_KEY_BLOCKS;

typedef struct st_sort_ftbuf
{
  uchar *buf, *end;
  int count;
  uchar lastkey[MARIA_MAX_KEY_BUFF];
} MA_SORT_FT_BUF;
