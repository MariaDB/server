/*
   Copyright (c) 2015, 2020, MariaDB

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

/*************************************************************************
  Limitation of encrypted IO_CACHEs
  1. Designed to support temporary files only (open_cached_file, fd=-1)
  2. Created with WRITE_CACHE, later can be reinit_io_cache'ed to
     READ_CACHE and WRITE_CACHE in any order arbitrary number of times.
  3. no seeks for writes, but reinit_io_cache(WRITE_CACHE, seek_offset)
     is allowed (there's a special hack in reinit_io_cache() for that)
*/

#include "../mysys/mysys_priv.h"
#include "log.h"
#include "mysqld.h"
#include "sql_class.h"

static uint keyid, keyver;

#define set_iv(IV, N1, N2)                                              \
  do {                                                                  \
    compile_time_assert(sizeof(IV) >= sizeof(N1) + sizeof(N2));         \
    memcpy(IV, &(N1), sizeof(N1));                                      \
    memcpy(IV + sizeof(N1), &(N2), sizeof(N2));                         \
  } while(0)

static int my_b_encr_read(IO_CACHE *info, uchar *Buffer, size_t Count)
{
  my_off_t pos_in_file= info->pos_in_file + (info->read_end - info->buffer);
  my_off_t old_pos_in_file= pos_in_file, pos_offset= 0;
  IO_CACHE_CRYPT *crypt_data=
    (IO_CACHE_CRYPT *)(info->buffer + info->buffer_length + MY_AES_BLOCK_SIZE);
  uchar *wbuffer= (uchar*)&(crypt_data->inbuf_counter);
  uchar *ebuffer= (uchar*)(crypt_data + 1);
  DBUG_ENTER("my_b_encr_read");

  if (pos_in_file == info->end_of_file)
  {
    /*  reading past EOF should not empty the cache */
    info->read_pos= info->read_end;
    info->error= 0;
    DBUG_RETURN(MY_TEST(Count));
  }

  if (info->seek_not_done)
  {
    my_off_t wpos;

    pos_offset= pos_in_file % info->buffer_length;
    pos_in_file-= pos_offset;

    wpos= pos_in_file / info->buffer_length * crypt_data->block_length;

    if ((mysql_file_seek(info->file, wpos, MY_SEEK_SET, MYF(0))
        == MY_FILEPOS_ERROR))
    {
      info->error= -1;
      DBUG_RETURN(1);
    }
    info->seek_not_done= 0;
    if (info->next_file_user)
    {
      IO_CACHE *c;
      for (c= info->next_file_user;
           c!= info;
           c= c->next_file_user)
      {
        c->seek_not_done= 1;
      }
    }
  }

  do
  {
    uint elength, wlength, length;
    uchar iv[MY_AES_BLOCK_SIZE]= {0};

    DBUG_ASSERT(pos_in_file % info->buffer_length == 0);

    if (info->end_of_file - pos_in_file >= info->buffer_length)
      wlength= crypt_data->block_length;
    else
      wlength= crypt_data->last_block_length;

    if (mysql_file_read(info->file, wbuffer, wlength, info->myflags | MY_NABP))
    {
      info->error= -1;
      DBUG_RETURN(1);
    }

    elength= wlength - (uint)(ebuffer - wbuffer);
    set_iv(iv, pos_in_file, crypt_data->inbuf_counter);

    if (encryption_crypt(ebuffer, elength, info->buffer, &length,
                         crypt_data->key, sizeof(crypt_data->key),
                         iv, sizeof(iv), ENCRYPTION_FLAG_DECRYPT,
                         keyid, keyver))
    {
      my_errno= 1;
      DBUG_RETURN(info->error= -1);
    }

    DBUG_ASSERT(length <= info->buffer_length);

    size_t copied= MY_MIN(Count, (size_t)(length - pos_offset));
    if (copied)
    {
      memcpy(Buffer, info->buffer + pos_offset, copied);
      Count-= copied;
      Buffer+= copied;
    }

    info->read_pos= info->buffer + pos_offset + copied;
    info->read_end= info->buffer + length;
    info->pos_in_file= pos_in_file;
    pos_in_file+= length;
    pos_offset= 0;

    if (wlength < crypt_data->block_length && pos_in_file < info->end_of_file)
    {
      info->error= (int)(pos_in_file - old_pos_in_file);
      DBUG_RETURN(1);
    }
  } while (Count);

  DBUG_RETURN(0);
}

static int my_b_encr_write(IO_CACHE *info, const uchar *Buffer, size_t Count)
{
  IO_CACHE_CRYPT *crypt_data=
    (IO_CACHE_CRYPT *)(info->buffer + info->buffer_length + MY_AES_BLOCK_SIZE);
  uchar *wbuffer= (uchar*)&(crypt_data->inbuf_counter);
  uchar *ebuffer= (uchar*)(crypt_data + 1);
  DBUG_ENTER("my_b_encr_write");

  if (Buffer != info->write_buffer)
  {
    Count-= Count % info->buffer_length;
    if (!Count)
      DBUG_RETURN(0);
  }

  if (info->seek_not_done)
  {
    DBUG_ASSERT(info->pos_in_file % info->buffer_length == 0);
    my_off_t wpos= info->pos_in_file / info->buffer_length * crypt_data->block_length;

    if ((mysql_file_seek(info->file, wpos, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR))
    {
      info->error= -1;
      DBUG_RETURN(1);
    }
    info->seek_not_done= 0;
  }

  if (info->pos_in_file == 0)
  {
    if (my_random_bytes(crypt_data->key, sizeof(crypt_data->key)))
    {
      my_errno= 1;
      DBUG_RETURN(info->error= -1);
    }
    crypt_data->counter= 0;

    IF_DBUG(crypt_data->block_length= 0,);
  }

  do
  {
    size_t length= MY_MIN(info->buffer_length, Count);
    uint elength, wlength;
    uchar iv[MY_AES_BLOCK_SIZE]= {0};

    crypt_data->inbuf_counter= crypt_data->counter;
    set_iv(iv, info->pos_in_file, crypt_data->inbuf_counter);

    if (encryption_crypt(Buffer, (uint)length, ebuffer, &elength,
                         crypt_data->key, (uint) sizeof(crypt_data->key),
                         iv, (uint) sizeof(iv), ENCRYPTION_FLAG_ENCRYPT,
                         keyid, keyver))
    {
      my_errno= 1;
      DBUG_RETURN(info->error= -1);
    }
    wlength= elength + (uint)(ebuffer - wbuffer);

    if (length == info->buffer_length)
    {
      /*
        block_length should be always the same. that is, encrypting
        buffer_length bytes should *always* produce block_length bytes
      */
      DBUG_ASSERT(crypt_data->block_length == 0 || crypt_data->block_length == wlength);
      DBUG_ASSERT(elength <= encryption_encrypted_length((uint)length, keyid, keyver));
      crypt_data->block_length= wlength;
    }
    else
    {
      /* if we write a partial block, it *must* be the last write */
      IF_DBUG(info->write_function= 0,);
      crypt_data->last_block_length= wlength;
    }

    if (mysql_file_write(info->file, wbuffer, wlength, info->myflags | MY_NABP))
      DBUG_RETURN(info->error= -1);

    Buffer+= length;
    Count-= length;
    info->pos_in_file+= length;
    crypt_data->counter++;
  } while (Count);
  DBUG_RETURN(0);
}

/**
  determine what key id and key version to use for IO_CACHE temp files

  First, try key id 2, if it doesn't exist, use key id 1.

  (key id 1 is the default system key id, used pretty much everywhere, it must
  exist. key id 2 is for tempfiles, it can be used, for example, to set a
  faster encryption algorithm for temporary files)

  This looks like it might have a bug: if an encryption plugin is unloaded when
  there's an open IO_CACHE, that IO_CACHE will become unreadable after reinit.
  But in fact it is safe, as an encryption plugin can only be unloaded on
  server shutdown.

  Note that encrypt_tmp_files variable is read-only.
*/
int init_io_cache_encryption()
{
  if (encrypt_tmp_files)
  {
    keyid= ENCRYPTION_KEY_TEMPORARY_DATA;
    keyver= encryption_key_get_latest_version(keyid);
    if (keyver == ENCRYPTION_KEY_VERSION_INVALID)
    {
      keyid= ENCRYPTION_KEY_SYSTEM_DATA;
      keyver= encryption_key_get_latest_version(keyid);
    }
    if (keyver == ENCRYPTION_KEY_VERSION_INVALID)
    {
      sql_print_error("Failed to enable encryption of temporary files");
      return 1;
    }

    if (keyver != ENCRYPTION_KEY_NOT_ENCRYPTED)
    {
      sql_print_information("Using encryption key id %d for temporary files", keyid);
      _my_b_encr_read= my_b_encr_read;
      _my_b_encr_write= my_b_encr_write;
      return 0;
    }
  }

  _my_b_encr_read= 0;
  _my_b_encr_write= 0;
  return 0;
}

