/*
  Copyright (c) 2015 Red Hat, Inc.
  Copyright (c) 2015 Robbie Harwood <rharwood@redhat.com>

  Permission to use, copy, modify, and/or distribute this software for
  any purpose with or without fee is hereby granted, provided that the
  above copyright notice and this permission notice appear in all
  copies.

  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
  WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
  AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
  DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA
  OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
  TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
  PERFORMANCE OF THIS SOFTWARE.
*/

#include "vio_priv.h"
#include "my_context.h"
#include <mysql_async.h>

#ifdef HAVE_GSSAPI

static size_t vio_gss_dump_plaintext(Vio *me, uchar *buf, size_t n)
{
  size_t get;

  DBUG_ENTER("vio_gss_dump_plaintext");

  /* a packet is decrypted and ready to go */
  get = MY_MIN(n, (size_t) (me->read_pos - me->read_buffer));
  memcpy(buf, me->read_buffer, get);
  memmove(me->read_buffer, me->read_buffer + get,
          me->read_end - me->read_buffer - get);
  me->read_pos -= get;
  me->read_end -= get;
  DBUG_RETURN(get);
}

/* Always buffered */
size_t vio_gss_read(Vio *me, uchar *buf, size_t n)
{
  /*
    vio->read_buffer: start of buffer
    vio->read_pos: start of encrypted data
    vio->read_end: current insertion point
  */
  ssize_t len, missing;
  size_t packet_size;
  OM_uint32 major, minor;
  gss_buffer_desc input, output;
  int conf;

  DBUG_ENTER("vio_gss_read");
  
  if (vio_gss_has_data(me))
  {
    len = vio_gss_dump_plaintext(me, buf, n);
    DBUG_RETURN(len);
  }

  missing = me->read_pos + 4 - me->read_end;
  if (missing > 0)
  {
    /* we need to get the length */
    len = vio_read(me, (uchar *) me->read_end, missing);
    if (len < 0)
    {
      /* error already logged from vio_read */
      DBUG_RETURN(len);
    }
    me->read_end += len;
    missing = me->read_pos + 4 - me->read_end;
    if (missing > 0)
      DBUG_RETURN(0);
  }

  /* we now have the length */
  memcpy(&packet_size, me->read_pos, 4);
  packet_size = ntohl(packet_size);
  if (packet_size > VIO_READ_BUFFER_SIZE - 4)
  {
    DBUG_PRINT("vio_gss_read",
               ("declared packet size larger than read buffer!"));
    DBUG_RETURN(-1);
  }

  missing = me->read_pos + packet_size + 4 - me->read_end;
  if (missing > 0)
  {
    /* try to get the rest of the packet */
    len = vio_read(me, (uchar *) me->read_end, missing);
    if (len < 0)
    {
      /* error already logged from vio_read */
      DBUG_RETURN(len);
    }
    me->read_end += len;
    missing = me->read_pos + packet_size + 4 - me->read_end;
    if (missing > 0)
      DBUG_RETURN(0);
  }

  /* we now have a full packet ready to decrypt */
  input.value = me->read_buffer + 4;
  input.length = packet_size;
  major = gss_unwrap(&minor, me->gss_ctxt, &input, &output, &conf, NULL);
  if (GSS_ERROR(major))
  {
    GSS_DBUG_ERROR(major, minor);
    DBUG_RETURN(-1);
  }
  else if (conf == 0)
  {
    DBUG_PRINT("vio_gss_read", ("GSSAPI failed to provide confidentiality!"));
    DBUG_RETURN(-1);
  }

  memcpy(me->read_buffer, output.value, output.length);
  me->read_pos = me->read_end = me->read_buffer + output.length;
  gss_release_buffer(&minor, &output);

  len = vio_gss_dump_plaintext(me, buf, n);
  DBUG_RETURN(len);
}

size_t vio_gss_write(Vio *me, const uchar *buf, size_t len)
{
  OM_uint32 major, minor;
  gss_buffer_desc input, output;
  int conf;
  uchar *send_buf;
  uint32 packetlen;
  ssize_t ret;

  DBUG_ENTER("vio_gss_write");
  
  /* 
     Pre-compute what this looks like encrypted.

     The type of a gss_buffer_t does not allow specification of the input
     buffer as const, but it will not modify the contents of this buffer as
     per rfc2744.
  */
  input.value = (uchar *) buf;
  input.length = len;

  major = gss_wrap(&minor, me->gss_ctxt, 1, GSS_C_QOP_DEFAULT, &input,
                   &conf, &output);
  if (GSS_ERROR(major))
  {
    GSS_DBUG_ERROR(major, minor);
    DBUG_RETURN(-1);
  }
  else if (!conf)
  {
    DBUG_PRINT("vio_gss_write", ("GSSAPI did not receive confidentiality!"));
    DBUG_RETURN(-1);
  }

  /*
    It "should be" faster to hit malloc here than to do two sends.

    Additionally, we need four bytes for length because GSS-encrypted
    packets can be larger, and it's valid to ask for a maxlen packet
    to be encrypted.

    Though the underlying buffer in output.value is allocated using malloc(),
    we cannot count on this behavior as it is not mandated by rfc2744.
  */
  send_buf = malloc(output.length + 4);
  if (!send_buf)
  {
    DBUG_PRINT("vio_error", ("Failed to malloc!"));
    DBUG_RETURN(errno);
  }
  packetlen = htonl(output.length);
  memcpy(send_buf, &packetlen, 4);
  memcpy(send_buf + 4, output.value, output.length);

  /*
    Mandatory blocking here.  What we consider a "packet" does not match what
    our caller considers a packet, since encryption increases the length.
  */
  ret = mysql_socket_send(me->mysql_socket, (SOCKBUF_T *) send_buf,
                          output.length + 4, 0);
  major = gss_release_buffer(&minor, &output);
  if (htonl(ret - 4) == packetlen)
    DBUG_RETURN(len);
  else if (ret < 0)
    DBUG_RETURN(ret);
  else
    DBUG_ASSERT(0);
}

int vio_gss_close(Vio *me)
{
  OM_uint32 minor;

  DBUG_ENTER("vio_gss_close");
  
  if (me->gss_ctxt != GSS_C_NO_CONTEXT)
  {
    gss_delete_sec_context(&minor, &me->gss_ctxt, GSS_C_NO_BUFFER);
    me->gss_ctxt = GSS_C_NO_CONTEXT;
  }

  DBUG_RETURN(vio_close(me));
}

my_bool vio_gss_has_data(Vio *me)
{
  DBUG_ENTER("vio_gss_has_data");

  DBUG_RETURN(me->read_buffer != me->read_pos);
}

#endif /* HAVE_GSSAPI */
