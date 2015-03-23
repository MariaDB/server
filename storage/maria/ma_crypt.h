/* Copyright 2013 Google Inc. All Rights Reserved. */

#ifndef _ma_crypt_h
#define _ma_crypt_h

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
