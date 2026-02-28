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
* Policy: local buffer first else my_malloc()
*
* Usage: the order of calling comp_buf_dealloc()
* should be the reverse of the order of calling
* comp_buf_alloc().
*
* Note: for a local buffer memory, first sizeof(unsigned long) bytes
* indicates the length of the memory available.
*/
typedef struct Compress_buffer
{
  void* buf;		    // buffer start
  void* buf_end;	  // buffer end
  void* write_pos;  // next position to write in buffer
} Compress_buffer;

/**
 * @param comp_buf a Compress_buffer object for memory management
 * @param bytes size of memory to be allocated
 * @ret pointer to a buffer else NULL if there is no available memory
**/
void* comp_buf_alloc(Compress_buffer* comp_buf, size_t bytes)
{
  void* buf;
  if ((uchar*)comp_buf->write_pos + bytes + sizeof(unsigned long) >= (uchar*)comp_buf->buf_end)
  {
    buf= my_malloc(key_memory_my_compress_alloc, bytes, MYF(0));
  }
  else
  {
    *(unsigned long*)(comp_buf->write_pos)= (unsigned long)bytes;
    comp_buf->write_pos= (uchar*)comp_buf->write_pos + sizeof(unsigned long);
    buf= comp_buf->write_pos;
    comp_buf->write_pos= (uchar*)comp_buf->write_pos + bytes;
  }
  return buf;
}

/**
 * @param comp_buf a Compress_buffer object for memory management
 * @param ptr pointer to memory to be deallocated
**/
void comp_buf_dealloc(Compress_buffer* comp_buf, void* ptr)
{
  if ((uchar*)ptr >= (uchar*)comp_buf->buf + sizeof(unsigned long) && ptr < comp_buf->buf_end) 
  {
    comp_buf->write_pos= (uchar*)ptr - sizeof(unsigned long);
  }
  else 
  {
    my_free(ptr);
  }
}



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

my_bool my_compress(uchar *packet, size_t *len, size_t *complen, 
                    void* buf, void* buf_end)
{
  DBUG_ENTER("my_compress");
  if (*len < MIN_COMPRESS_LENGTH)
  {
    *complen=0;
    DBUG_PRINT("note",("Packet too short: Not compressed"));
  }
  else
  {
    // uchar *compbuf=my_compress_alloc(packet,len,complen);
    uchar *compbuf;
    Compress_buffer compress_buf;
    compress_buf.buf= buf;
    compress_buf.buf_end= buf_end;
    compress_buf.write_pos= buf;
    compbuf= my_compress_alloc(packet, len, complen, &compress_buf);
    if (!compbuf)
      DBUG_RETURN(*complen ? 0 : 1);
    memcpy(packet,compbuf,*len);
    // my_free(compbuf);
    comp_buf_dealloc(&compress_buf, compbuf);
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
                       const uchar *source, size_t sourceLen, void* buf)
{
    z_stream stream;
    int err;

    stream.next_in = (Bytef*)source;
    stream.avail_in = (uInt)sourceLen;
    stream.next_out = (Bytef*)dest;
    stream.avail_out = (uInt)*destLen;
    if ((size_t)stream.avail_out != *destLen)
      return Z_BUF_ERROR;

    // stream.zalloc = (alloc_func)my_az_allocator;
    // stream.zfree = (free_func)my_az_free;
    if (!buf)
    {
      stream.zalloc = (alloc_func)my_az_allocator;
      stream.zfree = (free_func)my_az_free;
      stream.opaque = (voidpf)0;
    }
    else
    {
      stream.zalloc = (alloc_func)comp_buf_az_allocator;
      stream.zfree = (free_func)comp_buf_az_free;
      stream.opaque = (voidpf)buf;
    }

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

uchar*
my_compress_alloc(const uchar *packet, size_t *len, size_t *complen, void* buf)
{
  uchar *compbuf;
  int res;
  *complen= *len * 120 / 100 + 12;

  if (!buf)
  {
    if (!(compbuf= (uchar *) my_malloc(key_memory_my_compress_alloc, 
                                      *complen, MYF(MY_WME))))
      return 0;
  }
  else
  {
    if (!(compbuf= (uchar *) comp_buf_alloc((Compress_buffer*)buf, *complen)))
      return 0;
  }

  res= my_compress_buffer(compbuf, complen, packet, *len, buf);

  if (res != Z_OK)
  {
    if (!buf)
      my_free(compbuf);
    else
      comp_buf_dealloc((Compress_buffer*)buf, compbuf);
    return 0;
  }

  if (*complen >= *len)
  {
    *complen= 0;
    if (!buf)
      my_free(compbuf);
    else
      comp_buf_dealloc((Compress_buffer*)buf, compbuf);
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

my_bool my_uncompress(uchar *packet, size_t len, size_t *complen, void* buf, void* buf_end)
{
  size_t tmp_complen;
  uchar *compbuf;
  int error;
  DBUG_ENTER("my_uncompress");

  if (*complen)					/* If compressed */
  {
    Compress_buffer compress_buf;
    compress_buf.buf= buf;
    compress_buf.buf_end= buf_end;
    compress_buf.write_pos= buf;
    compbuf= comp_buf_alloc(&compress_buf, *complen);
    // uchar *compbuf= (uchar *) my_malloc(key_memory_my_compress_alloc,
    //                                  *complen,MYF(MY_WME));
    if (!compbuf)
      DBUG_RETURN(1);				/* Not enough memory */

    tmp_complen= *complen;
    error= my_uncompress_buffer(compbuf, &tmp_complen, packet, &len, (void*)&compress_buf);
    // error= uncompress((Bytef*) compbuf, &tmp_complen, (Bytef*) packet,
    //                  (uLong) len);
    *complen= tmp_complen;
    if (error != Z_OK)
    {						/* Probably wrong packet */
      DBUG_PRINT("error",("Can't uncompress packet, error: %d",error));
      comp_buf_dealloc(&compress_buf, compbuf);
      // my_free(compbuf);
      DBUG_RETURN(1);
    }
    memcpy(packet, compbuf, *complen);
    comp_buf_dealloc(&compress_buf, compbuf);
    // my_free(compbuf);
  }
  else
    *complen= len;
  DBUG_RETURN(0);
}

/**
* allocate ITEMS * SIZE bytes from buffer BUF
*/
void *comp_buf_az_allocator(void *buf, unsigned int items, unsigned int size)
{
  return comp_buf_alloc((Compress_buffer*)buf, (size_t)items * (size_t)size);
}
/**
* deallocate memory pointed by ADDRESS back to buffer BUF
*/
void comp_buf_az_free(void *buf, void *address)
{
  comp_buf_dealloc((Compress_buffer*)buf, address);
}

/* mysys/my_compress.c */
/** works like zlib uncompress(), but using class Compress_buffer
* allocator to try to get better performance
* @param dest buffer where uncompressed data will be put
* @param destLen length of buffer DEST
* @param source buffer where compressed data is stored
* @param sourceLen length fo compressed data
* @param compress_buf a local buffer if COMPRESS_BUF is not NULL
*
* @ret Z_OK if success, Z_MEM_ERROR if there was not enough memory,
*      Z_BUF_ERROR if there was not enough room in the output buffer,
*      Z_DATA_ERROR if the input data was corrupted, including if the
*      input data is an incomplete zlib stream.
*/
int my_uncompress_buffer(uchar* dest, size_t* destLen, 
                         const uchar* source, size_t* sourceLen, void* compress_buf)
{
  z_stream stream;
  int err;
  const uInt max= (uInt)-1;
  uLong len, left;
  Byte buf[1];    /* for detection of incomplete stream when *destLen == 0 */

  len= (uLong)*sourceLen;
  if (*destLen) {
      left= (uLong)*destLen;
      *destLen = 0;
  }
  else {
      left= 1;
      dest= buf;
  }

  stream.next_in= (z_const Bytef *)source;
  stream.avail_in= 0;
  if (!compress_buf)
  {
    stream.zalloc= (alloc_func)my_az_allocator;
    stream.zfree= (free_func)my_az_free;
    stream.opaque= (voidpf)0;
  }
  else
  {
    stream.zalloc= (alloc_func)comp_buf_az_allocator;
    stream.zfree= (free_func)comp_buf_az_free;
    stream.opaque= (voidpf)compress_buf;
  }

  err= inflateInit(&stream);
  if (err != Z_OK) return err;

  stream.next_out= (Bytef*)dest;
  stream.avail_out= 0;

  do {
      if (stream.avail_out == 0) {
          stream.avail_out= left > (uLong)max ? max : (uInt)left;
          left-= stream.avail_out;
      }
      if (stream.avail_in== 0) {
          stream.avail_in= len > (uLong)max ? max : (uInt)len;
          len-= stream.avail_in;
      }
      err= inflate(&stream, Z_NO_FLUSH);
  } while (err == Z_OK);

  *sourceLen-= len + stream.avail_in;
  if (dest != buf)
      *destLen= stream.total_out;
  else if (stream.total_out && err == Z_BUF_ERROR)
      left= 1;

  inflateEnd(&stream);
  return err == Z_STREAM_END ? Z_OK :
         err == Z_NEED_DICT ? Z_DATA_ERROR  :
         err == Z_BUF_ERROR && left + stream.avail_out ? Z_DATA_ERROR :
         err;
}

#endif /* HAVE_COMPRESS */
