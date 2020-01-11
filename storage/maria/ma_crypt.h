/*
  Copyright (c) 2013 Google Inc.
  Copyright (c) 2014, 2015 MariaDB Corporation

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef STORAGE_MARIA_MA_CRYPT_INCLUDED
#define STORAGE_MARIA_MA_CRYPT_INCLUDED

#include <my_global.h>

struct st_maria_share;
struct st_pagecache_file;

uint ma_crypt_get_data_page_header_space();/* bytes in data/index page header */
uint ma_crypt_get_index_page_header_space(struct st_maria_share *);
uint ma_crypt_get_file_length();                    /* bytes needed in file   */
int ma_crypt_create(struct st_maria_share *);       /* create encryption data */
int ma_crypt_write(struct st_maria_share *, File); /* write encryption data */
uchar* ma_crypt_read(struct st_maria_share *, uchar *buff); /* read crypt data*/

void ma_crypt_set_data_pagecache_callbacks(struct st_pagecache_file *file,
                                           struct st_maria_share *share);

void ma_crypt_set_index_pagecache_callbacks(struct st_pagecache_file *file,
                                            struct st_maria_share *share);

void ma_crypt_free(struct st_maria_share *share);

#endif
