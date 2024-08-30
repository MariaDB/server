#ifndef S3_FUNC_INCLUDED
#define S3_FUNC_INCLUDED
/* Copyright (C) 2019, 2022, MariaDB Corporation Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/*
  Interface function used by S3 storage engine and aria_copy_for_s3
*/

#ifdef WITH_S3_STORAGE_ENGINE
#include <libmarias3/marias3.h>

C_MODE_START
#define DEFAULT_AWS_HOST_NAME "s3.amazonaws.com"

extern struct s3_func {
  uint8_t (*set_option)(ms3_st *, ms3_set_option_t, void *);
  void (*free)(S3_BLOCK *);
  void (*deinit)(ms3_st *);
  int32 (*unique_file_number)(void);
  my_bool (*read_index_header)(ms3_st *, S3_INFO *, S3_BLOCK *);
  int (*check_frm_version)(ms3_st *, S3_INFO *);
  S3_INFO *(*info_copy)(S3_INFO *);
  my_bool (*set_database_and_table_from_path)(S3_INFO *, const char *);
  ms3_st *(*open_connection)(S3_INFO *);
} s3f;

extern TYPELIB s3_protocol_typelib;

/* Store information about a s3 connection */

struct s3_info
{
  /* Connection strings */
  LEX_CSTRING access_key, secret_key, region, bucket, host_name;
  int port; // 0 means 'Use default'
  my_bool use_http;

  /* Will be set by caller or by ma_open() */
  LEX_CSTRING database, table;

  /*
    Name of the partition table if the table is partitioned. If not, it's set
    to be same as table. This is used to know which frm file to read to
    check table version.
  */
  LEX_CSTRING base_table;

  /* Sent to open to verify version */
  LEX_CUSTRING tabledef_version;

  /* Protocol for the list bucket API call. 1 for Amazon, 2 for some others */
  uint8_t protocol_version;
};


/* flag + length is stored in this header */
#define COMPRESS_HEADER 4

/* Max length of an AWS PATH */
#define AWS_PATH_LENGTH ((NAME_LEN)*3+3+10+6+11)

void s3_init_library(void);
void s3_deinit_library(void);
int aria_copy_to_s3(ms3_st *s3_client, const char *aws_bucket,
                    const char *path,
                    const char *database, const char *table_name,
                    ulong block_size, my_bool compression,
                    my_bool force, my_bool display, my_bool copy_frm);
int aria_copy_from_s3(ms3_st *s3_client, const char *aws_bucket,
                      const char *path,const char *database,
                      my_bool compression, my_bool force, my_bool display);
int aria_delete_from_s3(ms3_st *s3_client, const char *aws_bucket,
                        const char *database, const char *table,
                        my_bool display);
int aria_rename_s3(ms3_st *s3_client, const char *aws_bucket,
                   const char *from_database, const char *from_table,
                   const char *to_database, const char *to_table,
                   my_bool rename_frm);
ms3_st *s3_open_connection(S3_INFO *s3);
void s3_deinit(ms3_st *s3_client);
int s3_put_object(ms3_st *s3_client, const char *aws_bucket,
                  const char *name, uchar *data, size_t length,
                  my_bool compression);
int s3_get_object(ms3_st *s3_client, const char *aws_bucket,
                  const char *name, S3_BLOCK *block, my_bool compression,
                  int print_error);
int s3_delete_object(ms3_st *s3_client, const char *aws_bucket,
                     const char *name, myf error_flags);
my_bool s3_rename_object(ms3_st *s3_client, const char *aws_bucket,
                         const char *from_name, const char *to_name,
                         myf error_flags);
void s3_free(S3_BLOCK *data);
my_bool s3_copy_from_file(ms3_st *s3_client, const char *aws_bucket,
                          char *aws_path, File file, my_off_t start,
                          my_off_t file_end, uchar *block, size_t block_size,
                          my_bool compression, my_bool display);
my_bool s3_copy_to_file(ms3_st *s3_client, const char *aws_bucket,
                        char *aws_path, File file, my_off_t start,
                        my_off_t file_end, my_bool compression,
                        my_bool display);
int s3_delete_directory(ms3_st *s3_client, const char *aws_bucket,
                        const char *path);
int s3_rename_directory(ms3_st *s3_client, const char *aws_bucket,
                        const char *from_name, const char *to_name,
                        myf error_flags);
int partition_delete_from_s3(ms3_st *s3_client, const char *aws_bucket,
                             const char *database, const char *table,
                             myf error_flags);
int partition_copy_to_s3(ms3_st *s3_client, const char *aws_bucket,
                         const char *path, const char *old_path,
                         const char *database, const char *table_name);

S3_INFO *s3_info_copy(S3_INFO *old);
my_bool set_database_and_table_from_path(S3_INFO *s3, const char *path);
my_bool s3_get_def(ms3_st *s3_client, S3_INFO *S3_info, S3_BLOCK *block,
                   const char *ext);
my_bool s3_frm_exists(ms3_st *s3_client, S3_INFO *s3_info);
int s3_check_frm_version(ms3_st *s3_client, S3_INFO *s3_info);
my_bool read_index_header(ms3_st *client, S3_INFO *s3, S3_BLOCK *block);
int32 s3_unique_file_number(void);
my_bool s3_block_read(struct st_pagecache *pagecache,
                      PAGECACHE_IO_HOOK_ARGS *args,
                      struct st_pagecache_file *file,
                      S3_BLOCK *block);
C_MODE_END
#else

C_MODE_START
/* Dummy structures and interfaces to be used when compiling without S3 */
struct s3_info;
struct ms3_st;
C_MODE_END
#endif /* WITH_S3_STORAGE_ENGINE */
#endif /* HA_S3_FUNC_INCLUDED */
