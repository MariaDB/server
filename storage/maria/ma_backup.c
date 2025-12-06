/* Copyright (C) 2018, 2020 MariaDB Corporation Ab

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

/* Code for doing backups of Aria tables */

#include "maria_def.h"
#include "ma_blockrec.h"                        /* PAGE_SUFFIX_SIZE */
#include "ma_checkpoint.h"
#include "ma_crypt.h"
#include <aria_backup.h>

/**
  @brief Get capabilities for an Aria table

  @param kfile   key file (.MAI)
  @param cap     Capabilities are stored here

  @return 0      ok
  @return X      errno
*/

int aria_get_capabilities(File kfile, const char *table_name,
                          ARIA_TABLE_CAPABILITIES *cap)
  __attribute__((visibility("default"))) ;
int aria_get_capabilities(File kfile, const char *table_name,
                          ARIA_TABLE_CAPABILITIES *cap)
{
  MARIA_SHARE share;
  int error= 0;
  uint head_length= sizeof(share.state.header), base_pos;
  uint aligned_bit_blocks;
  size_t info_length;
  uchar *disc_cache;
  DBUG_ENTER("aria_get_capabilities");

  bzero(cap, sizeof(*cap));
  bzero(&share, sizeof(share));
  if (my_pread(kfile,share.state.header.file_version, head_length, 0,
               MYF(MY_NABP)))
    DBUG_RETURN(HA_ERR_NOT_A_TABLE);

  if (memcmp(share.state.header.file_version, maria_file_magic, 4))
    DBUG_RETURN(HA_ERR_NOT_A_TABLE);

  share.options= mi_uint2korr(share.state.header.options);

  info_length= mi_uint2korr(share.state.header.header_length);
  base_pos=    mi_uint2korr(share.state.header.base_pos);

  /*
    Allocate space for header information and for data that is too
    big to keep on stack
  */
  if (!(disc_cache= my_malloc(PSI_NOT_INSTRUMENTED, info_length, MYF(MY_WME))))
    DBUG_RETURN(ENOMEM);

  if (my_pread(kfile, disc_cache, info_length, 0L, MYF(MY_NABP)))
  {
    error= my_errno;
    goto err;
  }
  _ma_base_info_read(disc_cache + base_pos, &share.base);
  strmake(cap->filename, table_name, sizeof(cap->filename)-1);
  cap->transactional= share.base.born_transactional;
  cap->checksum= MY_TEST(share.options & HA_OPTION_PAGE_CHECKSUM);
  cap->online_backup_safe= cap->transactional && cap->checksum;
  cap->header_size= share.base.keystart;
  cap->keypage_header= ((share.base.born_transactional ?
                         LSN_STORE_SIZE + TRANSID_SIZE : 0) +
                        KEYPAGE_KEYID_SIZE + KEYPAGE_FLAG_SIZE +
                        KEYPAGE_USED_SIZE);
  cap->block_size= share.base.block_size;
  cap->data_file_type= share.state.header.data_file_type;
  cap->s3_block_size=  share.base.s3_block_size;
  cap->compression=    share.base.compression_algorithm;
  cap->encrypted=      MY_TEST(share.base.extra_options &
                               MA_EXTRA_OPTIONS_ENCRYPTED);
  if (cap->encrypted)
  {
    share.data_file_name.str= (char*) table_name;      /* in case of error */
    share.crypt_data= 0;
    if (maria_read_crypt_data(kfile, &share))
    {
      error= HA_ERR_DECRYPTION_FAILED;
      goto err;
    }
    cap->keypage_header+= ma_crypt_get_index_page_header_space(&share);
    cap->crypt_data= share.crypt_data;
    cap->crypt_page_header_space= ma_crypt_get_data_page_header_space();
  }

  if (share.state.header.data_file_type == BLOCK_RECORD)
  {
    /* Calulate how many pages the row bitmap covers. From _ma_bitmap_init() */
    aligned_bit_blocks= (cap->block_size - PAGE_SUFFIX_SIZE) / 6;
    /*
      In each 6 bytes, we have 6*8/3 = 16 pages covered
      The +1 is to add the bitmap page, as this doesn't have to be covered
    */
    cap->bitmap_pages_covered= aligned_bit_blocks * 16 + 1;
  }

  /* Do a check that we got things right */
  if (share.state.header.data_file_type != BLOCK_RECORD &&
      cap->online_backup_safe)
    error= HA_ERR_NOT_A_TABLE;

err:
  my_free(disc_cache);
  if (error)
    aria_free_capabilities(cap);
  DBUG_RETURN(error);
} /* aria_get_capabilities */


void aria_free_capabilities(ARIA_TABLE_CAPABILITIES *cap)
{
  DBUG_ENTER("aria_free_capabilities");
  if (cap->crypt_data)
  {
    MARIA_SHARE share;
    share.crypt_data= cap->crypt_data;
    ma_crypt_free(&share);
    cap->crypt_data= 0;
  }
  DBUG_VOID_RETURN;
}


/****************************************************************************
**  store MARIA_BASE_INFO
****************************************************************************/

uchar *_ma_base_info_read(uchar *ptr, MARIA_BASE_INFO *base)
{
  bmove(base->uuid, ptr, MY_UUID_SIZE);                 ptr+= MY_UUID_SIZE;
  base->keystart= mi_sizekorr(ptr);			ptr+= 8;
  base->max_data_file_length= mi_sizekorr(ptr); 	ptr+= 8;
  base->max_key_file_length= mi_sizekorr(ptr);		ptr+= 8;
  base->records=  (ha_rows) mi_sizekorr(ptr);		ptr+= 8;
  base->reloc= (ha_rows) mi_sizekorr(ptr);		ptr+= 8;
  base->mean_row_length= mi_uint4korr(ptr);		ptr+= 4;
  base->reclength= mi_uint4korr(ptr);			ptr+= 4;
  base->pack_reclength= mi_uint4korr(ptr);		ptr+= 4;
  base->min_pack_length= mi_uint4korr(ptr);		ptr+= 4;
  base->max_pack_length= mi_uint4korr(ptr);		ptr+= 4;
  base->min_block_length= mi_uint4korr(ptr);		ptr+= 4;
  base->fields= mi_uint2korr(ptr);			ptr+= 2;
  base->fixed_not_null_fields= mi_uint2korr(ptr);       ptr+= 2;
  base->fixed_not_null_fields_length= mi_uint2korr(ptr);ptr+= 2;
  base->max_field_lengths= mi_uint2korr(ptr);	        ptr+= 2;
  base->pack_fields= mi_uint2korr(ptr);			ptr+= 2;
  base->extra_options= mi_uint2korr(ptr);		ptr+= 2;
  base->null_bytes= mi_uint2korr(ptr);			ptr+= 2;
  base->original_null_bytes= mi_uint2korr(ptr);		ptr+= 2;
  base->field_offsets= mi_uint2korr(ptr);		ptr+= 2;
  base->language= mi_uint2korr(ptr);		        ptr+= 2;
  base->block_size= mi_uint2korr(ptr);			ptr+= 2;

  base->rec_reflength= *ptr++;
  base->key_reflength= *ptr++;
  base->keys=	       *ptr++;
  base->auto_key=      *ptr++;
  base->born_transactional= *ptr++;
  base->compression_algorithm= *ptr++;
  base->pack_bytes= mi_uint2korr(ptr);			ptr+= 2;
  base->blobs= mi_uint2korr(ptr);			ptr+= 2;
  base->max_key_block_length= mi_uint2korr(ptr);	ptr+= 2;
  base->max_key_length= mi_uint2korr(ptr);		ptr+= 2;
  base->extra_alloc_bytes= mi_uint2korr(ptr);		ptr+= 2;
  base->extra_alloc_procent= *ptr++;
  base->s3_block_size= mi_uint3korr(ptr);               ptr+= 3;
  ptr+= 13;
  return ptr;
}


/**
   @brief Copy an index block with re-read if checksum doesn't match

   @param dfile       data file (.MAD)
   @param cap         aria capabilities from aria_get_capabilities
   @param block       block number to read (0, 1, 2, 3...)
   @param buffer      read data to this buffer
   @param bytes_read  number of bytes actually read (in case of end of file)

   @return 0 ok
   @return HA_ERR_END_OF_FILE  ; End of file
   @return # error number
*/

#define MAX_RETRY 10

int aria_read_index(File kfile, ARIA_TABLE_CAPABILITIES *cap, ulonglong block,
                    uchar *buffer)
{
  MARIA_SHARE share;
  int retry= 0, error= 0;
  DBUG_ENTER("aria_read_index");

  share.keypage_header= cap->keypage_header;
  share.block_size= cap->block_size;

  for (;;)
  {
    size_t length;
    if ((length= my_pread(kfile, buffer, cap->block_size,
                          block * cap->block_size, MYF(0))) != cap->block_size)
    {
      if (length == 0)
        DBUG_RETURN(HA_ERR_END_OF_FILE);
      if (length == (size_t) -1)
        DBUG_RETURN(my_errno ? my_errno : -1);
      /* Assume we got a half read; Do a re-read */
    }
    /* If not transactional or key file header, there are no checksums */
    if (!cap->online_backup_safe ||
        block < cap->header_size/ cap->block_size)
      DBUG_RETURN(length == cap->block_size ? 0 : HA_ERR_CRASHED);

    if (length == cap->block_size)
    {
      length= _ma_get_page_used(&share, buffer);
      if (length > cap->block_size - CRC_SIZE)
        DBUG_RETURN(HA_ERR_CRASHED);

      if (cap->encrypted)
      {
        /* We have to decrypt block to be able to calculate checksum */
        PAGECACHE_IO_HOOK_ARGS io_arg;
        my_bool crypt_error;
        io_arg.data= (void*) &share;
        io_arg.page= buffer;
        io_arg.crypt_buf= (uchar*) my_alloca(cap->block_size);
        io_arg.pageno= block;
        share.crypt_data= cap->crypt_data;
        share.silence_encryption_errors= 1;
        share.crypt_page_header_space= cap->crypt_page_header_space;
        share.open_file_name.str= cap->filename;

        crypt_error= ma_crypt_index_post_read_hook(0, &io_arg);
        my_afree(io_arg.crypt_buf);
        if (!crypt_error)
          DBUG_RETURN(0);                       /* ok */
        error= my_errno;
        if (error == HA_ERR_DECRYPTION_FAILED ||
            error == HA_ERR_WRONG_CRC)
          goto retry;
        break;                                  /* Give error */
      }

      error= maria_page_crc_check(buffer, block, &share,
                                  MARIA_NO_CRC_NORMAL_PAGE,
                                  (int) length);
      if (error == 0)
        DBUG_RETURN(0);
      if ((error= my_errno) != HA_ERR_WRONG_CRC)
        break;
    }
  retry:
    if (retry++ >= MAX_RETRY)
    {
      error= HA_ERR_WRONG_CRC;
      break;
    }
    my_sleep(100000);                              /* Sleep 0.1 seconds */
  }
  my_printf_error(error,
                  "Error %d reading index file %s  block %lld",
                  MYF(ME_FATAL|ME_ERROR_LOG),
                  error, cap->filename, block);
  DBUG_RETURN(error);
}


/**
   @brief Copy a data block with re-read if checksum doesn't match

   @param dfile       data file (.MAD)
   @param cap         aria capabilities from aria_get_capabilities
   @param block       block number to read (0, 1, 2, 3...)
   @param buffer      read data to this buffer
   @param bytes_read  number of bytes actually read (in case of end of file)

   @return 0 ok
   @return HA_ERR_END_OF_FILE  ; End of file
   @return # error number
*/

int aria_read_data(File dfile, ARIA_TABLE_CAPABILITIES *cap, ulonglong block,
                   uchar *buffer, size_t *bytes_read)
{
  MARIA_SHARE share;
  int retry= 0, error= 0;
  DBUG_ENTER("aria_read_data");

  share.keypage_header= cap->keypage_header;
  share.block_size= cap->block_size;

  if (!cap->online_backup_safe)
  {
    *bytes_read= my_pread(dfile, buffer, cap->block_size,
                          block * cap->block_size, MY_WME);
    if (*bytes_read == 0)
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    DBUG_RETURN(*bytes_read > 0 ? 0 : (my_errno ? my_errno : -1));
  }
  *bytes_read= cap->block_size;
  for (;;)
  {
    size_t length;
    if ((length= my_pread(dfile, buffer, cap->block_size,
                          block * cap->block_size, MYF(0))) != cap->block_size)
    {
      if (length == 0)
        DBUG_RETURN(HA_ERR_END_OF_FILE);
      if (length == (size_t) -1)
        DBUG_RETURN(my_errno ? my_errno : -1);
    }

    /* If not transactional or key file header, there are no checksums */
    if (!cap->online_backup_safe)
      DBUG_RETURN(length == cap->block_size ? 0 : HA_ERR_CRASHED);

    if (length == cap->block_size)
    {
      if (!_ma_check_if_zero(buffer, share.block_size - CRC_SIZE))
        DBUG_RETURN(0);
      /* Test if encrypted pages. Note that bitmap pages are not encrypted */
      if (cap->encrypted && (block % cap->bitmap_pages_covered))
      {
        /* We have to decrypt block to be able to calculate checksum */
        PAGECACHE_IO_HOOK_ARGS io_arg;
        my_bool crypt_error;
        io_arg.data= (void*) &share;
        io_arg.page= buffer;
        io_arg.crypt_buf= (uchar*) my_alloca(cap->block_size);
        io_arg.pageno= block;
        share.crypt_data= cap->crypt_data;
        share.silence_encryption_errors= 1;
        share.crypt_page_header_space= cap->crypt_page_header_space;
        share.open_file_name.str= cap->filename;

        crypt_error= ma_crypt_data_post_read_hook(0, &io_arg);
        my_afree(io_arg.crypt_buf);
        if (!crypt_error)
          DBUG_RETURN(0);                       /* ok */
        if (my_errno == HA_ERR_DECRYPTION_FAILED || my_errno== HA_ERR_WRONG_CRC)
          goto retry;
        error= my_errno;
        break;
      }
      else
      {
        error= maria_page_crc_check(buffer, block, &share,
                                    ((block % cap->bitmap_pages_covered) == 0 ?
                                     MARIA_NO_CRC_BITMAP_PAGE :
                                     MARIA_NO_CRC_NORMAL_PAGE),
                                    share.block_size - CRC_SIZE);
        if (error == 0)
          DBUG_RETURN(0);
        if ((error= my_errno) != HA_ERR_WRONG_CRC)
          break;
      }
    }
retry:
    if (retry++ >= MAX_RETRY)
    {
      error= HA_ERR_WRONG_CRC;
      break;
    }
    my_sleep(100000);                              /* Sleep 0.1 seconds */
  }
  my_printf_error(error,
                  "Error %d reading data file %s  block %lld",
                  MYF(ME_FATAL|ME_ERROR_LOG),
                  error, cap->filename, block);
  DBUG_RETURN(error);
}
