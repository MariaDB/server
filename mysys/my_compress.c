/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2022, MariaDB Corporation.

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

/* Written by Sinisa Milivojevic <sinisa@mysql.com> */

#include <mysys_priv.h>
#ifdef HAVE_COMPRESS
#include <my_sys.h>
#ifndef SCO
#include <m_string.h>
#endif
#include <zlib.h>

/*
   This replaces the packet with a compressed packet

   SYNOPSIS
     my_compress()
     packet	Data to compress. This is is replaced with the compressed data.
     len	Length of data to compress at 'packet'
     complen	out: 0 if packet was not compressed

   RETURN
     1   error. 'len' is not changed'
     0   ok.  In this case 'len' contains the size of the compressed packet
*/

my_bool my_compress(uchar *packet, size_t *len, size_t *complen)
{
  DBUG_ENTER("my_compress");
  if (*len < MIN_COMPRESS_LENGTH)
  {
    *complen=0;
    DBUG_PRINT("note",("Packet too short: Not compressed"));
  }
  else
  {
    uchar *compbuf=my_compress_alloc(packet,len,complen);
    if (!compbuf)
      DBUG_RETURN(*complen ? 0 : 1);
    memcpy(packet,compbuf,*len);
    my_free(compbuf);
  }
  DBUG_RETURN(0);
}


void *my_az_allocator(void *dummy __attribute__((unused)), unsigned int items,
                      unsigned int size)
{
  return my_malloc(key_memory_my_compress_alloc, (size_t)items*(size_t)size,
                   MYF(0));
}

void my_az_free(void *dummy __attribute__((unused)), void *address)
{
  my_free(address);
}

/*
  This works like zlib compress(), but using custom memory allocators to work
  better with my_malloc leak detection and Valgrind.
*/
int my_compress_buffer(uchar *dest, size_t *destLen,
                       const uchar *source, size_t sourceLen)
{
    z_stream stream;
    int err;

    stream.next_in = (Bytef*)source;
    stream.avail_in = (uInt)sourceLen;
    stream.next_out = (Bytef*)dest;
    stream.avail_out = (uInt)*destLen;
    if ((size_t)stream.avail_out != *destLen)
      return Z_BUF_ERROR;

    stream.zalloc = (alloc_func)my_az_allocator;
    stream.zfree = (free_func)my_az_free;
    stream.opaque = (voidpf)0;

    err = deflateInit(&stream, Z_DEFAULT_COMPRESSION);
    if (err != Z_OK) return err;

    err = deflate(&stream, Z_FINISH);
    if (err != Z_STREAM_END) {
        deflateEnd(&stream);
        return err == Z_OK ? Z_BUF_ERROR : err;
    }
    *destLen = stream.total_out;

    err = deflateEnd(&stream);
    return err;
}

uchar *my_compress_alloc(const uchar *packet, size_t *len, size_t *complen)
{
  uchar *compbuf;
  int res;
  *complen=  *len * 120 / 100 + 12;

  if (!(compbuf= (uchar *) my_malloc(key_memory_my_compress_alloc,
                                     *complen, MYF(MY_WME))))
    return 0;

  res= my_compress_buffer(compbuf, complen, packet, *len);

  if (res != Z_OK)
  {
    my_free(compbuf);
    return 0;
  }

  if (*complen >= *len)
  {
    *complen= 0;
    my_free(compbuf);
    DBUG_PRINT("note",("Packet got longer on compression; Not compressed"));
    return 0;
  }
  /* Store length of compressed packet in *len */
  swap_variables(size_t, *len, *complen);
  return compbuf;
}


/*
  Uncompress packet

   SYNOPSIS
     my_uncompress()
     packet	Compressed data. This is is replaced with the original data.
     len	Length of compressed data
     complen	Length of the packet buffer (must be enough for the original
	        data)

   RETURN
     1   error
     0   ok.  In this case 'complen' contains the updated size of the
              real data.
*/

my_bool my_uncompress(uchar *packet, size_t len, size_t *complen)
{
  uLongf tmp_complen;
  DBUG_ENTER("my_uncompress");

  if (*complen)					/* If compressed */
  {
    uchar *compbuf= (uchar *) my_malloc(key_memory_my_compress_alloc,
                                        *complen,MYF(MY_WME));
    int error;
    if (!compbuf)
      DBUG_RETURN(1);				/* Not enough memory */

    tmp_complen= (uLongf) *complen;
    error= uncompress((Bytef*) compbuf, &tmp_complen, (Bytef*) packet,
                      (uLong) len);
    *complen= tmp_complen;
    if (error != Z_OK)
    {						/* Probably wrong packet */
      DBUG_PRINT("error",("Can't uncompress packet, error: %d",error));
      my_free(compbuf);
      DBUG_RETURN(1);
    }
    memcpy(packet, compbuf, *complen);
    my_free(compbuf);
  }
  else
    *complen= len;
  DBUG_RETURN(0);
}

#endif /* HAVE_COMPRESS */
