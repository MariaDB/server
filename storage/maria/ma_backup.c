/* Copyright (C) 2018 MariaDB corporation

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
#include <aria_backup.h>

static uchar *_ma_base_info_read(uchar *ptr, MARIA_BASE_INFO *base);

/**
  @brief Get capabilites for an Aria table

  @param kfile   key file (.MAI)
  @param cap     Capabilities are stored here

  @return 0      ok
  @return X      errno
*/

int aria_get_capabilities(File kfile, ARIA_TABLE_CAPABILITIES *cap)
{
  MARIA_SHARE share;
  int error= 0;
  uint head_length= sizeof(share.state.header), base_pos;
  uint aligned_bit_blocks;
  size_t info_length;
  uchar *disc_cache;
  DBUG_ENTER("aria_get_capabilities");

  bzero(cap, sizeof(*cap));
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
  if (!(disc_cache= my_malloc(info_length, MYF(MY_WME))))
    DBUG_RETURN(ENOMEM);

  if (my_pread(kfile, disc_cache, info_length, 0L, MYF(MY_NABP)))
  {
    error= my_errno;
    goto err;
  }
  _ma_base_info_read(disc_cache + base_pos, &share.base);
  cap->transactional= share.base.born_transactional;
  cap->checksum= MY_TEST(share.options & HA_OPTION_PAGE_CHECKSUM);
  cap->online_backup_safe= cap->transactional && cap->checksum;
  cap->header_size= share.base.keystart;
  cap->keypage_header= ((share.base.born_transactional ?
                         LSN_STORE_SIZE + TRANSID_SIZE :
                         0) + KEYPAGE_KEYID_SIZE + KEYPAGE_FLAG_SIZE +
                        KEYPAGE_USED_SIZE);
  cap->block_size= share.base.block_size;

  if (share.state.header.data_file_type == BLOCK_RECORD)
  {
    /* Calulate how man pages the row bitmap covers. From _ma_bitmap_init() */
    aligned_bit_blocks= (cap->block_size - PAGE_SUFFIX_SIZE) / 6;
    /*
      In each 6 bytes, we have 6*8/3 = 16 pages covered
      The +1 is to add the bitmap page, as this doesn't have to be covered
    */
    cap->bitmap_pages_covered= aligned_bit_blocks * 16 + 1;
  }

  /* Do a check that that we got things right */
  if (share.state.header.data_file_type != BLOCK_RECORD &&
      cap->online_backup_safe)
    error= HA_ERR_NOT_A_TABLE;

err:
  my_free(disc_cache);
  DBUG_RETURN(error);
} /* maria_get_capabilities */


/*
  This is a copy of my_base_info_read from ma_open().
  The base information will never change (something may be added
  last, but not relevant for maria_get_capabilities), so it's safe to
  copy it here.

  The copy is done to avoid linking in the fill Aria library just
  because maria_backup uses maria_get_capabilities()
*/


static uchar *_ma_base_info_read(uchar *ptr, MARIA_BASE_INFO *base)
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
  ptr++;
  base->pack_bytes= mi_uint2korr(ptr);			ptr+= 2;
  base->blobs= mi_uint2korr(ptr);			ptr+= 2;
  base->max_key_block_length= mi_uint2korr(ptr);	ptr+= 2;
  base->max_key_length= mi_uint2korr(ptr);		ptr+= 2;
  base->extra_alloc_bytes= mi_uint2korr(ptr);		ptr+= 2;
  base->extra_alloc_procent= *ptr++;
  ptr+= 16;
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
  int retry= 0;
  DBUG_ENTER("aria_read_index");

  share.keypage_header= cap->keypage_header;
  share.block_size= cap->block_size;
  do
  {
    int error;
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
      error= maria_page_crc_check(buffer, block, &share,
                                  MARIA_NO_CRC_NORMAL_PAGE,
                                  (int) length);
      if (error != HA_ERR_WRONG_CRC)
        DBUG_RETURN(error);
    }
    my_sleep(100000);                              /* Sleep 0.1 seconds */
  } while (retry < MAX_RETRY);
  DBUG_RETURN(HA_ERR_WRONG_CRC);
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
  int retry= 0;
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
  do
  {
    int error;
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
      error= maria_page_crc_check(buffer, block, &share,
                                  ((block % cap->bitmap_pages_covered) == 0 ?
                                   MARIA_NO_CRC_BITMAP_PAGE :
                                   MARIA_NO_CRC_NORMAL_PAGE),
                                  share.block_size - CRC_SIZE);
      if (error != HA_ERR_WRONG_CRC)
        DBUG_RETURN(error);
    }
    my_sleep(100000);                              /* Sleep 0.1 seconds */
  } while (retry < MAX_RETRY);
    DBUG_RETURN(HA_ERR_WRONG_CRC);
}
