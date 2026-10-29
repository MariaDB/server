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
#include "mysqld_error.h"

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
    /* Buffer used by aria_read_index() and aria_read_data() */
    if (!(cap->crypt_buffer= my_malloc(PSI_NOT_INSTRUMENTED, cap->block_size,
                                       MYF(MY_WME))))
    {
      error= ENOMEM;
      goto err;
    }
  }

  if (share.state.header.data_file_type == BLOCK_RECORD)
  {
    /* Calculate how many pages the row bitmap covers. From _ma_bitmap_init() */
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
  my_free(cap->crypt_buffer);
  cap->crypt_buffer= 0;
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

   @param kfile       index file (.MAI)
   @param cap         aria capabilities from aria_get_capabilities
   @param block       block number to read (0, 1, 2, 3...)
   @param buffer      read data to this buffer
   @param buffer_size Size of buffer
   @param bytes_read  How many bytes was read. Undefined in case of erro

   @return 0 ok
   @return HA_ERR_END_OF_FILE  ; End of file
   @return # error number

   Notes: A file which size is not a multiple of block_size is threated as
   an error.
*/

#define MAX_RETRY 10

#ifndef DBUG_OFF
/**
  Simulate a torn read in a test: change the second half of a page,
  as if a concurrent write had replaced it during the read.

  @param page        page in the read buffer
  @param block_size  page size
  @param torn        whether a page was already torn in this read call
*/
static void aria_backup_debug_tear(uchar *page, uint block_size,
                                   my_bool *torn)
{
  my_bool tear= FALSE;
  DBUG_EXECUTE_IF("aria_backup_torn_read", tear= !*torn;);
  DBUG_EXECUTE_IF("aria_backup_torn_read_always", tear= TRUE;);
  if (tear)
  {
    uint i;
    for (i= block_size / 2; i < block_size; i++)
      page[i]^= 0xa5;
    *torn= TRUE;
  }
}
#else
# define aria_backup_debug_tear(page, block_size, torn) ((void) 0)
#endif

int aria_read_index(File kfile, ARIA_TABLE_CAPABILITIES *cap, ulonglong block,
                    uchar *buffer, size_t buffer_size, size_t *bytes_read)
{
  MARIA_SHARE share;
  int retry= 0, error= 0;
  my_off_t filepos;
  size_t length;
  size_t blocks;
#ifndef DBUG_OFF
  my_bool torn= FALSE;
#endif
  uchar *buffer_end;
  DBUG_ENTER("aria_read_index");

  share.keypage_header= cap->keypage_header;
  share.block_size= cap->block_size;
  DBUG_ASSERT(buffer_size % cap->block_size == 0);

  filepos= block * cap->block_size;

  my_errno= 0;
  while ((longlong) (length= my_pread(kfile, buffer, buffer_size,
                                      filepos, MYF(0))) <
         cap->block_size)
  {
    if (length == 0)
    {
      error= HA_ERR_END_OF_FILE;
      *bytes_read= 0;
      goto end;
    }
    if (length == (size_t) -1 ||  retry++ >= MAX_RETRY)
    {
      error= my_errno ? my_errno :  HA_ERR_FILE_TOO_SHORT;
      goto end;
    }
    my_sleep(100000);                              /* Sleep 0.1 seconds */
  }
  /* Convert to full blocks. Half blocks will be read by next call */
  length-= (length % cap->block_size);
  *bytes_read= length;

  /* If not transactional there are no checksums */
  if (!cap->online_backup_safe)
    goto end;

  /* Check all blocks for checksum errors */
  blocks= length / cap->block_size;
  buffer_end= buffer + length;

  /* First page does not have a checksum and is safe to read */
  if (block < cap->header_size/ cap->block_size)
  {
    size_t tmp= (size_t) MY_MIN(cap->header_size/ cap->block_size - block,
                                blocks);
    block+= tmp;
    filepos+= cap->block_size * tmp;
    buffer+=  cap->block_size * tmp;
  }

  retry= 0;
  for ( ; buffer < buffer_end ;
        block++, buffer+= cap->block_size, filepos+= cap->block_size,
          retry= 0)
  {
retry:
    error= 0;
    /*
      skip an all-zero page. it can happen if the ma_checkpoint_background
      thread flushes the later page before ever flushing this one.
    */
    if (!_ma_check_if_zero(buffer, cap->block_size))
      continue;
    aria_backup_debug_tear(buffer, cap->block_size, &torn);

    length= _ma_get_page_used(&share, buffer);
    if (length > cap->block_size - CRC_SIZE)
    {
      error= HA_ERR_CRASHED;
      goto end;
    }
    if (cap->encrypted)
    {
      /* We have to decrypt block to be able to calculate checksum */
      PAGECACHE_IO_HOOK_ARGS io_arg;
      my_bool crypt_error;
      io_arg.data= (void*) &share;
      io_arg.page= buffer;
      io_arg.crypt_buf= cap->crypt_buffer;
      io_arg.pageno= block;
      share.crypt_data= cap->crypt_data;
      share.silence_encryption_errors= 1;
      share.crypt_page_header_space= cap->crypt_page_header_space;
      share.open_file_name.str= cap->filename;

      crypt_error= ma_crypt_index_post_read_hook(0, &io_arg);
      if (!crypt_error)
        continue;

      error= my_errno;
      if (error != HA_ERR_DECRYPTION_FAILED &&
          error != HA_ERR_WRONG_CRC)
        break;                                  /* Give error */
    }
    else
    {
      error= maria_page_crc_check(buffer, block, &share,
                                  MARIA_NO_CRC_NORMAL_PAGE,
                                  (int) length);
      if (error == 0)
        continue;
      if ((error= my_errno) != HA_ERR_WRONG_CRC)
        break;
    }
    /* Retry read of the wrong block*/
    if (retry++ >= MAX_RETRY)
    {
      error= HA_ERR_WRONG_CRC;
      break;
    }
    my_sleep(100000);                              /* Sleep 0.1 seconds */

    if ((longlong) my_pread(kfile, buffer, cap->block_size, filepos, MYF(0)) <
        cap->block_size)
    {
      error= my_errno ? my_errno : -1;
      break;
    }
    goto retry;
  }
  if (error)
    my_printf_error(error,
                    "Error %d reading index file %s  block %lld",
                    MYF(ME_FATAL|ME_ERROR_LOG),
                    error, cap->filename, block);
end:
  DBUG_RETURN(error);
}


/**
   @brief Copy data blocks with re-read if checksum doesn't match

   @param dfile       data file (.MAD)
   @param cap         aria capabilities from aria_get_capabilities
   @param block       block number to read (0, 1, 2, 3...)
   @param buffer      read data to this buffer
   @param buffer_size Size of buffer. Must be a multiple of block_size
                      for tables with checksums
   @param bytes_read  How many bytes was read. Undefined in case of error

   @return 0 ok
   @return HA_ERR_END_OF_FILE  ; End of file
   @return # error number

   Notes: For a table with checksums, a file which size is not a
   multiple of block_size is threated as an error. Tables without
   checksums are not page based, which is why the last read can be
   shorter than block_size.
*/

int aria_read_data(File dfile, ARIA_TABLE_CAPABILITIES *cap, ulonglong block,
                   uchar *buffer, size_t buffer_size, size_t *bytes_read)
{
  MARIA_SHARE share;
  int retry= 0, error= 0;
  my_off_t filepos;
  size_t length;
  uchar *buffer_end;
#ifndef DBUG_OFF
  my_bool torn= FALSE;
#endif
  DBUG_ENTER("aria_read_data");

  share.keypage_header= cap->keypage_header;
  share.block_size= cap->block_size;

  filepos= block * cap->block_size;
  my_errno= 0;

  if (!cap->online_backup_safe)
  {
    /* No checksums and not page based; copy the file as it is */
    length= my_pread(dfile, buffer, buffer_size, filepos, MYF(MY_WME));
    if (length == (size_t) -1)
      DBUG_RETURN(my_errno ? my_errno : -1);
    *bytes_read= length;
    DBUG_RETURN(length ? 0 : HA_ERR_END_OF_FILE);
  }

  /* All transactional tables with checksums are page based */
  DBUG_ASSERT(cap->bitmap_pages_covered);
  DBUG_ASSERT(buffer_size % cap->block_size == 0);

  while ((longlong) (length= my_pread(dfile, buffer, buffer_size,
                                      filepos, MYF(0))) <
         cap->block_size)
  {
    if (length == 0)
    {
      error= HA_ERR_END_OF_FILE;
      *bytes_read= 0;
      goto end;
    }
    if (length == (size_t) -1 || retry++ >= MAX_RETRY)
    {
      error= my_errno ? my_errno : HA_ERR_FILE_TOO_SHORT;
      goto end;
    }
    my_sleep(100000);                              /* Sleep 0.1 seconds */
  }
  /* Convert to full blocks. Half blocks will be read by next call */
  length-= (length % cap->block_size);
  *bytes_read= length;

  /* Check all blocks for checksum errors */
  buffer_end= buffer + length;

  retry= 0;
  for ( ; buffer < buffer_end ;
        block++, buffer+= cap->block_size, filepos+= cap->block_size,
          retry= 0)
  {
retry:
    error= 0;
    /*
      skip an all-zero page. it can happen if the ma_checkpoint_background
      thread flushes the later page before ever flushing this one.
    */
    if (!_ma_check_if_zero(buffer, cap->block_size - CRC_SIZE))
      continue;
    aria_backup_debug_tear(buffer, cap->block_size, &torn);

    /* Test if encrypted page. Note that bitmap pages are not encrypted */
    if (cap->encrypted && (block % cap->bitmap_pages_covered))
    {
      /* We have to decrypt block to be able to calculate checksum */
      PAGECACHE_IO_HOOK_ARGS io_arg;
      my_bool crypt_error;
      io_arg.data= (void*) &share;
      io_arg.page= buffer;
      io_arg.crypt_buf= cap->crypt_buffer;
      io_arg.pageno= block;
      share.crypt_data= cap->crypt_data;
      share.silence_encryption_errors= 1;
      share.crypt_page_header_space= cap->crypt_page_header_space;
      share.open_file_name.str= cap->filename;

      crypt_error= ma_crypt_data_post_read_hook(0, &io_arg);
      if (!crypt_error)
        continue;

      error= my_errno;
      if (error != HA_ERR_DECRYPTION_FAILED &&
          error != HA_ERR_WRONG_CRC)
        break;                                  /* Give error */
    }
    else
    {
      error= maria_page_crc_check(buffer, block, &share,
                                  ((block % cap->bitmap_pages_covered) == 0 ?
                                   MARIA_NO_CRC_BITMAP_PAGE :
                                   MARIA_NO_CRC_NORMAL_PAGE),
                                  (int) (cap->block_size - CRC_SIZE));
      if (error == 0)
        continue;
      if ((error= my_errno) != HA_ERR_WRONG_CRC)
        break;
    }
    /* Retry read of the wrong block*/
    if (retry++ >= MAX_RETRY)
    {
      error= HA_ERR_WRONG_CRC;
      break;
    }
    my_sleep(100000);                              /* Sleep 0.1 seconds */

    if ((longlong) my_pread(dfile, buffer, cap->block_size, filepos, MYF(0)) <
        cap->block_size)
    {
      error= my_errno ? my_errno : HA_ERR_FILE_TOO_SHORT;
      break;
    }
    goto retry;
  }
  if (error)
    my_printf_error(error,
                    "Error %d reading data file %s  block %lld",
                    MYF(ME_FATAL|ME_ERROR_LOG),
                    error, cap->filename, block);
end:
  DBUG_RETURN(error);
}


/******************************************************************************
  Interfaces for backup server
******************************************************************************/

/**
   @brief Open one file of an Aria table for backup

   Like maria_open(), refuse a symbolic link that cannot be resolved or
   that mysys_test_invalid_symlink() rejects, and open the resolved
   name without following symbolic links.

   @param data_file  TRUE for the data file, FALSE for the index file
   @param name       file name
   @param file       the opened file is stored here
   @param real_name  the resolved name is stored here (FN_REFLEN bytes)

   @return 0  ok
   @return X  error number. The error has been reported.
*/

static int aria_open_file_for_backup(my_bool data_file, const char *name,
                                     File *file, char *real_name)
{
  int realpath_err= my_realpath(real_name, name, MYF(0));

  if (my_is_symlink(name) &&
      (realpath_err || mysys_test_invalid_symlink(real_name)))
  {
    my_error(ER_CANT_OPEN_FILE, MYF(0), name, HA_WRONG_CREATE_OPTION);
    return HA_WRONG_CREATE_OPTION;
  }
  if ((*file= mysql_file_open(data_file ? key_file_dfile : key_file_kfile,
                              real_name,
                              O_RDONLY | O_SHARE | O_NOFOLLOW | O_CLOEXEC,
                              MYF(MY_NOSYMLINKS))) < 0)
  {
    int error= my_errno;
    my_error(ER_CANT_OPEN_FILE, MYF(0), name, error);
    return error;
  }
  return 0;
}


/**
   @brief Open the files of an Aria table for backup

   @param path     table file name without extension
   @param context  backup context to initialize

   @return 0  ok
   @return X  error number. The error has been reported.
*/

int aria_open_files_for_backup(const char *path,
                               ARIA_BACKUP_CONTEXT *context)
{
  char name[FN_REFLEN], data_name[FN_REFLEN];
  int error;
  DBUG_ENTER("aria_open_files_for_backup");

  bzero(context, sizeof(*context));
  fn_format(name, path, "", MARIA_NAME_IEXT, MY_REPLACE_EXT);
  if ((error= aria_open_file_for_backup(FALSE, name,
                                        &context->kfile,
                                        context->index_name)))
    DBUG_RETURN(error);
  if ((error= aria_get_capabilities(context->kfile, name,
                                    &context->capabilities)))
  {
    my_printf_error(error, "Error %d reading the header of %s",
                    MYF(ME_ERROR_LOG), error, name);
    goto close_kfile;
  }
  fn_format(name, path, "", MARIA_NAME_DEXT, MY_REPLACE_EXT);
  if (!(error= aria_open_file_for_backup(TRUE, name,
                                         &context->dfile, data_name)))
    DBUG_RETURN(0);

  aria_free_capabilities(&context->capabilities);
close_kfile:
  my_close(context->kfile, MYF(0));
  DBUG_RETURN(error);
}


/**
   @brief Close the files after a successful aria_open_files_for_backup()
*/

void aria_close_files_for_backup(ARIA_BACKUP_CONTEXT *context)
{
  aria_free_capabilities(&context->capabilities);
  my_close(context->dfile, MYF(0));
  my_close(context->kfile, MYF(0));
}


/**
   @brief Read the header blocks of the index file

   The header blocks have no checksum. The table state at the start of
   the header is written with my_pwrite(), outside the page cache, so
   a concurrent read could see part of a write. Read the header while
   no such write can run: maria_open() and maria_close() hold
   THR_LOCK_maria, and the other writers of an open table hold
   MARIA_SHARE::intern_lock.

   @param context     Backup context, positioned in the header
   @param buffer      read data to this buffer
   @param buff_length Size of buffer. Must be a multiple of block_size

   @return > 0  number of bytes read into buffer
   @return < 0  error number
*/

static longlong aria_read_header(ARIA_BACKUP_CONTEXT *context,
                                 uchar *buffer, size_t buff_length)
{
  const ARIA_TABLE_CAPABILITIES *cap= &context->capabilities;
  size_t length= (size_t) (cap->header_size / cap->block_size -
                           context->kblock) * cap->block_size;
  MARIA_HA *info;
  size_t bytes_read;
  DBUG_ENTER("aria_read_header");

  set_if_smaller(length, buff_length);
  DEBUG_SYNC_C("aria_backup_header");
  mysql_mutex_lock(&THR_LOCK_maria);
  if ((info= _ma_test_if_reopen(context->index_name)))
  {
    mysql_mutex_lock(&info->s->intern_lock);
    DEBUG_SYNC_C("aria_backup_header_locked");
  }
  bytes_read= my_pread(context->kfile, buffer, length,
                       context->kblock * cap->block_size, MYF(0));
  if (info)
    mysql_mutex_unlock(&info->s->intern_lock);
  mysql_mutex_unlock(&THR_LOCK_maria);

  if (bytes_read != length)
    DBUG_RETURN(bytes_read == (size_t) -1 && my_errno
                ? -(longlong) my_errno : -(longlong) HA_ERR_FILE_TOO_SHORT);
  context->kblock+= length / cap->block_size;
  DBUG_RETURN((longlong) length);
}


/**
   @brief Read the next part of the index file

   @param context     Backup context with the open files and the
                      position in them
   @param buffer      read data to this buffer
   @param buff_length Size of buffer. Must be a multiple of block_size

   @return            number of bytes read
   @retval 0          End of file
   @retval < 0        Error code
*/

longlong aria_read_index_file(ARIA_BACKUP_CONTEXT *context,
                              uchar *buffer, size_t buff_length)
{
  size_t bytes_read;
  int error;
  if (context->capabilities.online_backup_safe &&
      context->kblock < (context->capabilities.header_size /
                         context->capabilities.block_size))
    return aria_read_header(context, buffer, buff_length);
  error= aria_read_index(context->kfile, &context->capabilities,
                         context->kblock, buffer, buff_length,
                         &bytes_read);
  if (!error)
  {
    context->kblock+= bytes_read / context->capabilities.block_size;
    return (longlong) bytes_read;
  }
  return error == HA_ERR_END_OF_FILE ? 0 : -error;
}


/*
  Read a set of pages from data file

  @brief Read the next part of the index file

  @param context     Backup context with the open files and the
                     position in them
  @param buffer      read data to this buffer
  @param buff_length Size of buffer. Must be a multiple of block_size

  @return            number of bytes read
  @retval 0          End of file
  @retval < 0        Error code

  Note that this function should only be used with transactional
  tables with checksums.  Non transactional tables can be in a not
  block format and/or not have checksums and aria_read_data_file() will
  give errors for these. Use my_copy() on non transactional files while
  ensuring data is flushed and there is no writes to them.

  @return > 0   number of bytes read into buffer
  @return < -1  error number
*/

longlong aria_read_data_file(ARIA_BACKUP_CONTEXT *context,
                             uchar *buffer, size_t buff_length)
{
  size_t bytes_read;
  int error= aria_read_data(context->dfile, &context->capabilities,
                            context->dblock, buffer, buff_length,
                            &bytes_read);
  if (!error)
  {
    context->dblock+= bytes_read / context->capabilities.block_size;
    return (longlong) bytes_read;
  }
  return error == HA_ERR_END_OF_FILE ? 0 : -error;
}
