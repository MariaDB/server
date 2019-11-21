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

#include <my_global.h>
#include "maria_def.h"
#include "ma_blockrec.h"
#include <my_crypt.h>

#define CRYPT_SCHEME_1         1
#define CRYPT_SCHEME_1_ID_LEN  4 /* 4 bytes for counter-block */
#define CRYPT_SCHEME_1_IV_LEN           16
#define CRYPT_SCHEME_1_KEY_VERSION_SIZE  4

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key key_CRYPT_DATA_lock;
#endif

struct st_crypt_key
{
  uint key_version;
  uchar key[CRYPT_SCHEME_1_IV_LEN];
};

struct st_maria_crypt_data
{
  struct st_encryption_scheme scheme;
  uint space;
  mysql_mutex_t lock;          /* protecting keys */
};

/**
  determine what key id to use for Aria encryption

  Same logic as for tempfiles: if key id 2 exists - use it,
  otherwise use key id 1.

  Key id 1 is system, it always exists. Key id 2 is optional,
  it allows to specify fast low-grade encryption for temporary data.
*/
static uint get_encryption_key_id(MARIA_SHARE *share)
{
  if (share->options & HA_OPTION_TMP_TABLE &&
      encryption_key_id_exists(ENCRYPTION_KEY_TEMPORARY_DATA))
    return ENCRYPTION_KEY_TEMPORARY_DATA;
  else
    return ENCRYPTION_KEY_SYSTEM_DATA;
}

uint
ma_crypt_get_data_page_header_space()
{
  return CRYPT_SCHEME_1_KEY_VERSION_SIZE;
}

uint
ma_crypt_get_index_page_header_space(MARIA_SHARE *share)
{
  if (share->base.born_transactional)
  {
    return CRYPT_SCHEME_1_KEY_VERSION_SIZE;
  }
  else
  {
    /* if the index is not transactional, we add 7 bytes LSN anyway
       to be used for counter block
    */
    return LSN_STORE_SIZE + CRYPT_SCHEME_1_KEY_VERSION_SIZE;
  }
}

uint
ma_crypt_get_file_length()
{
  return 2 + CRYPT_SCHEME_1_IV_LEN + CRYPT_SCHEME_1_ID_LEN;
}

static void crypt_data_scheme_locker(struct st_encryption_scheme *scheme,
                                     int unlock)
{
  MARIA_CRYPT_DATA *crypt_data = (MARIA_CRYPT_DATA*)scheme;
  if (unlock)
    mysql_mutex_unlock(&crypt_data->lock);
  else
    mysql_mutex_lock(&crypt_data->lock);
}

int
ma_crypt_create(MARIA_SHARE* share)
{
  MARIA_CRYPT_DATA *crypt_data=
    (MARIA_CRYPT_DATA*)my_malloc(sizeof(MARIA_CRYPT_DATA), MYF(MY_ZEROFILL));
  crypt_data->scheme.type= CRYPT_SCHEME_1;
  crypt_data->scheme.locker= crypt_data_scheme_locker;
  mysql_mutex_init(key_CRYPT_DATA_lock, &crypt_data->lock, MY_MUTEX_INIT_FAST);
  crypt_data->scheme.key_id= get_encryption_key_id(share);
  my_random_bytes(crypt_data->scheme.iv, sizeof(crypt_data->scheme.iv));
  my_random_bytes((uchar*)&crypt_data->space, sizeof(crypt_data->space));
  share->crypt_data= crypt_data;
  share->crypt_page_header_space= CRYPT_SCHEME_1_KEY_VERSION_SIZE;
  return 0;
}

void
ma_crypt_free(MARIA_SHARE* share)
{
  if (share->crypt_data != NULL)
  {
    mysql_mutex_destroy(&share->crypt_data->lock);
    my_free(share->crypt_data);
    share->crypt_data= NULL;
  }
}

int
ma_crypt_write(MARIA_SHARE* share, File file)
{
  MARIA_CRYPT_DATA *crypt_data= share->crypt_data;
  uchar buff[2 + 4 + sizeof(crypt_data->scheme.iv)];
  if (crypt_data == 0)
    return 0;

  buff[0]= crypt_data->scheme.type;
  buff[1]= sizeof(buff) - 2;

  int4store(buff + 2, crypt_data->space);
  memcpy(buff + 6, crypt_data->scheme.iv, sizeof(crypt_data->scheme.iv));

  if (mysql_file_write(file, buff, sizeof(buff), MYF(MY_NABP)))
    return 1;

  return 0;
}

uchar*
ma_crypt_read(MARIA_SHARE* share, uchar *buff)
{
  uchar type= buff[0];
  uchar iv_length= buff[1];

  /* currently only supported type */
  if (type != CRYPT_SCHEME_1 ||
      iv_length != sizeof(((MARIA_CRYPT_DATA*)1)->scheme.iv) + 4)
  {
    my_printf_error(HA_ERR_UNSUPPORTED,
             "Unsupported crypt scheme! type: %d iv_length: %d\n",
             MYF(ME_FATALERROR|ME_NOREFRESH),
             type, iv_length);
    return 0;
  }

  if (share->crypt_data == NULL)
  {
    /* opening a table */
    MARIA_CRYPT_DATA *crypt_data=
      (MARIA_CRYPT_DATA*)my_malloc(sizeof(MARIA_CRYPT_DATA), MYF(MY_ZEROFILL));

    crypt_data->scheme.type= type;
    mysql_mutex_init(key_CRYPT_DATA_lock, &crypt_data->lock,
                     MY_MUTEX_INIT_FAST);
    crypt_data->scheme.locker= crypt_data_scheme_locker;
    crypt_data->scheme.key_id= get_encryption_key_id(share);
    crypt_data->space= uint4korr(buff + 2);
    memcpy(crypt_data->scheme.iv, buff + 6, sizeof(crypt_data->scheme.iv));
    share->crypt_data= crypt_data;
  }

  share->crypt_page_header_space= CRYPT_SCHEME_1_KEY_VERSION_SIZE;
  return buff + 2 + iv_length;
}

static int ma_encrypt(MARIA_SHARE *, MARIA_CRYPT_DATA *, const uchar *,
                      uchar *, uint, uint, LSN, uint *);
static int ma_decrypt(MARIA_SHARE *, MARIA_CRYPT_DATA *, const uchar *,
                      uchar *, uint, uint, LSN, uint);

static my_bool ma_crypt_pre_read_hook(PAGECACHE_IO_HOOK_ARGS *args)
{
  MARIA_SHARE *share= (MARIA_SHARE*) args->data;
  uchar *crypt_buf= my_malloc(share->block_size, MYF(0));
  if (crypt_buf == NULL)
  {
    args->crypt_buf= NULL; /* for post-hook */
    return 1;
  }

  /* swap pointers to read into crypt_buf */
  args->crypt_buf= args->page;
  args->page= crypt_buf;

  return 0;
}

static my_bool ma_crypt_data_post_read_hook(int res,
                                            PAGECACHE_IO_HOOK_ARGS *args)
{
  MARIA_SHARE *share= (MARIA_SHARE*) args->data;
  const uint size= share->block_size;
  const uchar page_type= args->page[PAGE_TYPE_OFFSET] & PAGE_TYPE_MASK;
  const uint32 key_version_offset= (page_type <= TAIL_PAGE) ?
      KEY_VERSION_OFFSET : FULL_PAGE_KEY_VERSION_OFFSET;

  if (res == 0)
  {
    const uchar *src= args->page;
    uchar* dst= args->crypt_buf;
    uint pageno= (uint)args->pageno;
    LSN lsn= lsn_korr(src);
    const uint head= (page_type <= TAIL_PAGE) ?
        PAGE_HEADER_SIZE(share) : FULL_PAGE_HEADER_SIZE(share);
    const uint tail= CRC_SIZE;
    const uint32 key_version= uint4korr(src + key_version_offset);

    /* 1 - copy head */
    memcpy(dst, src, head);
    /* 2 - decrypt page */
    res= ma_decrypt(share, share->crypt_data,
                    src + head, dst + head, size - (head + tail), pageno, lsn,
                    key_version);
    /* 3 - copy tail */
    memcpy(dst + size - tail, src + size - tail, tail);
    /* 4 clear key version to get correct crc */
    int4store(dst + key_version_offset, 0);
  }

  if (args->crypt_buf != NULL)
  {
    uchar *tmp= args->page;
    args->page= args->crypt_buf;
    args->crypt_buf= NULL;
    my_free(tmp);
  }

  return maria_page_crc_check_data(res, args);
}

static void store_rand_lsn(uchar * page)
{
  LSN lsn= 0;
  lsn+= rand();
  lsn<<= 32;
  lsn+= rand();
  lsn_store(page, lsn);
}

static my_bool ma_crypt_data_pre_write_hook(PAGECACHE_IO_HOOK_ARGS *args)
{
  MARIA_SHARE *share= (MARIA_SHARE*) args->data;
  const uint size= share->block_size;
  uint key_version;
  uchar *crypt_buf= my_malloc(share->block_size, MYF(0));

  if (crypt_buf == NULL)
  {
    args->crypt_buf= NULL; /* for post-hook */
    return 1;
  }

  if (!share->now_transactional)
  {
    /* store a random number instead of LSN (for counter block) */
    store_rand_lsn(args->page);
  }

  maria_page_crc_set_normal(args);

  {
    const uchar *src= args->page;
    uchar* dst= crypt_buf;
    uint pageno= (uint)args->pageno;
    LSN lsn= lsn_korr(src);
    const uchar page_type= src[PAGE_TYPE_OFFSET] & PAGE_TYPE_MASK;
    const uint head= (page_type <= TAIL_PAGE) ?
        PAGE_HEADER_SIZE(share) : FULL_PAGE_HEADER_SIZE(share);
    const uint tail= CRC_SIZE;
    const uint32 key_version_offset= (page_type <= TAIL_PAGE) ?
        KEY_VERSION_OFFSET : FULL_PAGE_KEY_VERSION_OFFSET;

    DBUG_ASSERT(page_type < MAX_PAGE_TYPE);

    /* 1 - copy head */
    memcpy(dst, src, head);
    /* 2 - encrypt page */
    if (ma_encrypt(share, share->crypt_data,
                   src + head, dst + head, size - (head + tail), pageno, lsn,
                   &key_version))
      return 1;
    /* 3 - copy tail */
    memcpy(dst + size - tail, src + size - tail, tail);
    /* 4 - store key version */
    int4store(dst + key_version_offset, key_version);
  }

  /* swap pointers to instead write out the encrypted block */
  args->crypt_buf= args->page;
  args->page= crypt_buf;

  return 0;
}

static void ma_crypt_post_write_hook(int res,
                                     PAGECACHE_IO_HOOK_ARGS *args)
{
  if (args->crypt_buf != NULL)
  {
    uchar *tmp= args->page;
    args->page= args->crypt_buf;
    args->crypt_buf= NULL;
    my_free(tmp);
  }

  maria_page_write_failure(res, args);
}

void ma_crypt_set_data_pagecache_callbacks(PAGECACHE_FILE *file,
                                           MARIA_SHARE *share
                                           __attribute__((unused)))
{
  /* Only use encryption if we have defined it */
  if (encryption_key_id_exists(get_encryption_key_id(share)))
  {
    file->pre_read_hook= ma_crypt_pre_read_hook;
    file->post_read_hook= ma_crypt_data_post_read_hook;
    file->pre_write_hook= ma_crypt_data_pre_write_hook;
    file->post_write_hook= ma_crypt_post_write_hook;
  }
}

static my_bool ma_crypt_index_post_read_hook(int res,
                                            PAGECACHE_IO_HOOK_ARGS *args)
{
  MARIA_SHARE *share= (MARIA_SHARE*) args->data;
  const uint block_size= share->block_size;
  const uint page_used= _ma_get_page_used(share, args->page);

  if (res == 0 && page_used <= block_size - CRC_SIZE)
  {
    const uchar *src= args->page;
    uchar* dst= args->crypt_buf;
    uint pageno= (uint)args->pageno;
    LSN lsn= lsn_korr(src);
    const uint head= share->keypage_header;
    const uint tail= CRC_SIZE;
    const uint32 key_version= _ma_get_key_version(share, src);
    /* page_used includes header (but not trailer) */
    const uint size= page_used - head;

    /* 1 - copy head */
    memcpy(dst, src, head);
    /* 2 - decrypt page */
    res= ma_decrypt(share, share->crypt_data,
                    src + head, dst + head, size, pageno, lsn, key_version);
    /* 3 - copy tail */
    memcpy(dst + block_size - tail, src + block_size - tail, tail);
    /* 4 clear key version to get correct crc */
    _ma_store_key_version(share, dst, 0);
  }

  if (args->crypt_buf != NULL)
  {
    uchar *tmp= args->page;
    args->page= args->crypt_buf;
    args->crypt_buf= NULL;
    my_free(tmp);
  }

  return maria_page_crc_check_index(res, args);
}

static my_bool ma_crypt_index_pre_write_hook(PAGECACHE_IO_HOOK_ARGS *args)
{
  MARIA_SHARE *share= (MARIA_SHARE*) args->data;
  const uint block_size= share->block_size;
  const uint page_used= _ma_get_page_used(share, args->page);
  uint key_version;
  uchar *crypt_buf= my_malloc(block_size, MYF(0));
  if (crypt_buf == NULL)
  {
    args->crypt_buf= NULL; /* for post-hook */
    return 1;
  }

  if (!share->now_transactional)
  {
    /* store a random number instead of LSN (for counter block) */
    store_rand_lsn(args->page);
  }

  maria_page_crc_set_index(args);

  {
    const uchar *src= args->page;
    uchar* dst= crypt_buf;
    uint pageno= (uint)args->pageno;
    LSN lsn= lsn_korr(src);
    const uint head= share->keypage_header;
    const uint tail= CRC_SIZE;
    /* page_used includes header (but not trailer) */
    const uint size= page_used - head;

    /* 1 - copy head */
    memcpy(dst, src, head);
    /* 2 - encrypt page */
    if (ma_encrypt(share, share->crypt_data,
                   src + head, dst + head, size, pageno, lsn, &key_version))
    {
      my_free(crypt_buf);
      return 1;
    }
    /* 3 - copy tail */
    memcpy(dst + block_size - tail, src + block_size - tail, tail);
    /* 4 - store key version */
    _ma_store_key_version(share, dst, key_version);
#ifdef HAVE_valgrind
    /* 5 - keep valgrind happy by zeroing not used bytes */
    bzero(dst+head+size, block_size - size - tail - head);
#endif
  }

  /* swap pointers to instead write out the encrypted block */
  args->crypt_buf= args->page;
  args->page= crypt_buf;

  return 0;
}

void ma_crypt_set_index_pagecache_callbacks(PAGECACHE_FILE *file,
                                            MARIA_SHARE *share
                                            __attribute__((unused)))
{
  file->pre_read_hook= ma_crypt_pre_read_hook;
  file->post_read_hook= ma_crypt_index_post_read_hook;
  file->pre_write_hook= ma_crypt_index_pre_write_hook;
  file->post_write_hook= ma_crypt_post_write_hook;
}

static int ma_encrypt(MARIA_SHARE *share, MARIA_CRYPT_DATA *crypt_data,
                      const uchar *src, uchar *dst, uint size,
                      uint pageno, LSN lsn,
                      uint *key_version)
{
  int rc;
  uint32 dstlen= 0;              /* Must be set because of error message */

  *key_version = encryption_key_get_latest_version(crypt_data->scheme.key_id);
  if (*key_version == ENCRYPTION_KEY_VERSION_INVALID)
  {
    /*
      We use this error for both encryption and decryption, as in normal
      cases it should be impossible to get an error here.
    */
    my_errno= HA_ERR_DECRYPTION_FAILED;
    my_printf_error(HA_ERR_DECRYPTION_FAILED,
                    "Unknown key id %u. Can't continue!",
                    MYF(ME_FATALERROR|ME_NOREFRESH),
                    crypt_data->scheme.key_id);
    return 1;
  }

  rc= encryption_scheme_encrypt(src, size, dst, &dstlen,
                                &crypt_data->scheme, *key_version,
                                crypt_data->space, pageno, lsn);

  /* The following can only fail if the encryption key is wrong */
  DBUG_ASSERT(!my_assert_on_error || rc == MY_AES_OK);
  DBUG_ASSERT(!my_assert_on_error || dstlen == size);
  if (! (rc == MY_AES_OK && dstlen == size))
  {
    my_errno= HA_ERR_DECRYPTION_FAILED;
    my_printf_error(HA_ERR_DECRYPTION_FAILED,
                    "failed to encrypt '%s'  rc: %d  dstlen: %u  size: %u\n",
                    MYF(ME_FATALERROR|ME_NOREFRESH),
                    share->open_file_name.str, rc, dstlen, size);
    return 1;
  }

  return 0;
}

static int ma_decrypt(MARIA_SHARE *share, MARIA_CRYPT_DATA *crypt_data,
                      const uchar *src, uchar *dst, uint size,
                      uint pageno, LSN lsn,
                      uint key_version)
{
  int rc;
  uint32 dstlen= 0;              /* Must be set because of error message */

  rc= encryption_scheme_decrypt(src, size, dst, &dstlen,
                                &crypt_data->scheme, key_version,
                                crypt_data->space, pageno, lsn);

  DBUG_ASSERT(!my_assert_on_error || rc == MY_AES_OK);
  DBUG_ASSERT(!my_assert_on_error || dstlen == size);
  if (! (rc == MY_AES_OK && dstlen == size))
  {
    my_errno= HA_ERR_DECRYPTION_FAILED;
    my_printf_error(HA_ERR_DECRYPTION_FAILED,
                    "failed to decrypt '%s'  rc: %d  dstlen: %u  size: %u\n",
                    MYF(ME_FATALERROR|ME_NOREFRESH),
                    share->open_file_name.str, rc, dstlen, size);
    return 1;
  }
  return 0;
}
