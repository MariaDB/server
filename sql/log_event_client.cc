/*
   Copyright (c) 2000, 2018, Oracle and/or its affiliates.
   Copyright (c) 2009, 2021, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/


#include "log_event.h"
#ifndef MYSQL_CLIENT
#error MYSQL_CLIENT must be defined here
#endif

#ifdef MYSQL_SERVER
#error MYSQL_SERVER must not be defined here
#endif


static bool pretty_print_str(IO_CACHE* cache, const char* str,
                              size_t len, bool identifier)
{
  const char* end = str + len;
  if (my_b_write_byte(cache, identifier ? '`' : '\''))
    goto err;

  while (str < end)
  {
    char c;
    int error;

    switch ((c=*str++)) {
    case '\n': error= my_b_write(cache, (uchar*)"\\n", 2); break;
    case '\r': error= my_b_write(cache, (uchar*)"\\r", 2); break;
    case '\\': error= my_b_write(cache, (uchar*)"\\\\", 2); break;
    case '\b': error= my_b_write(cache, (uchar*)"\\b", 2); break;
    case '\t': error= my_b_write(cache, (uchar*)"\\t", 2); break;
    case '\'': error= my_b_write(cache, (uchar*)"\\'", 2); break;
    case 0   : error= my_b_write(cache, (uchar*)"\\0", 2); break;
    default:
      error= my_b_write_byte(cache, c);
      break;
    }
    if (unlikely(error))
      goto err;
  }
  return my_b_write_byte(cache, identifier ? '`' : '\'');

err:
  return 1;
}

/**
  Print src as an string enclosed with "'"

  @param[out] cache  IO_CACHE where the string will be printed.
  @param[in] str  the string will be printed.
  @param[in] len  length of the string.
*/
static inline bool pretty_print_str(IO_CACHE* cache, const char* str,
                                    size_t len)
{
  return pretty_print_str(cache, str, len, false);
}

/**
  Print src as an identifier enclosed with "`"

  @param[out] cache  IO_CACHE where the identifier will be printed.
  @param[in] str  the string will be printed.
  @param[in] len  length of the string.
 */
static inline bool pretty_print_identifier(IO_CACHE* cache, const char* str,
                                           size_t len)
{
  return pretty_print_str(cache, str, len, true);
}



/**
  Prints a "session_var=value" string. Used by mysqlbinlog to print some SET
  commands just before it prints a query.
*/

static bool print_set_option(IO_CACHE* file, uint32 bits_changed,
                             uint32 option, uint32 flags, const char* name,
                             bool* need_comma)
{
  if (bits_changed & option)
  {
    if (*need_comma)
      if (my_b_write(file, (uchar*)", ", 2))
        goto err;
    if (my_b_printf(file, "%s=%d", name, MY_TEST(flags & option)))
      goto err;
    *need_comma= 1;
  }
  return 0;
err:
  return 1;
}


static bool hexdump_minimal_header_to_io_cache(IO_CACHE *file,
                                               my_off_t offset,
                                               uchar *ptr)
{
  DBUG_ASSERT(LOG_EVENT_MINIMAL_HEADER_LEN == 19);

  /*
    Pretty-print the first LOG_EVENT_MINIMAL_HEADER_LEN (19) bytes of the
    common header, which contains the basic information about the log event.
    Every event will have at least this much header, but events could contain
    more headers (which must be printed by other methods, if desired).
  */
  char emit_buf[120];               // Enough for storing one line
  size_t emit_buf_written;

  if (my_b_printf(file,
                  "#           "
                  "|Timestamp   "
                  "|Type "
                  "|Master ID   "
                  "|Size        "
                  "|Master Pos  "
                  "|Flags\n"))
    goto err;
  emit_buf_written=
    my_snprintf(emit_buf, sizeof(emit_buf),
                "# %8llx  "                         /* Position */
                "|%02x %02x %02x %02x "             /* Timestamp */
                "|%02x   "                          /* Type */
                "|%02x %02x %02x %02x "             /* Master ID */
                "|%02x %02x %02x %02x "             /* Size */
                "|%02x %02x %02x %02x "             /* Master Pos */
                "|%02x %02x\n",                     /* Flags */
                (ulonglong) offset,                 /* Position */
                ptr[0], ptr[1], ptr[2], ptr[3],     /* Timestamp */
                ptr[4],                             /* Type */
                ptr[5], ptr[6], ptr[7], ptr[8],     /* Master ID */
                ptr[9], ptr[10], ptr[11], ptr[12],  /* Size */
                ptr[13], ptr[14], ptr[15], ptr[16], /* Master Pos */
                ptr[17], ptr[18]);                  /* Flags */

  DBUG_ASSERT(static_cast<size_t>(emit_buf_written) < sizeof(emit_buf));
  if (my_b_write(file, reinterpret_cast<uchar*>(emit_buf), emit_buf_written) ||
      my_b_write(file, (uchar*)"#\n", 2))
    goto err;

  return 0;
err:
  return 1;
}


/*
  The number of bytes to print per line. Should be an even number,
  and "hexdump -C" uses 16, so we'll duplicate that here.
*/
#define HEXDUMP_BYTES_PER_LINE 16

static void format_hex_line(char *emit_buff)
{
  memset(emit_buff + 1, ' ',
         1 + 8 + 2 + (HEXDUMP_BYTES_PER_LINE * 3 + 1) + 2 +
         HEXDUMP_BYTES_PER_LINE);
  emit_buff[0]= '#';
  emit_buff[2 + 8 + 2 + (HEXDUMP_BYTES_PER_LINE * 3 + 1) + 1]= '|';
  emit_buff[2 + 8 + 2 + (HEXDUMP_BYTES_PER_LINE * 3 + 1) + 2 +
    HEXDUMP_BYTES_PER_LINE]= '|';
  emit_buff[2 + 8 + 2 + (HEXDUMP_BYTES_PER_LINE * 3 + 1) + 2 +
    HEXDUMP_BYTES_PER_LINE + 1]= '\n';
  emit_buff[2 + 8 + 2 + (HEXDUMP_BYTES_PER_LINE * 3 + 1) + 2 +
    HEXDUMP_BYTES_PER_LINE + 2]= '\0';
}

static bool hexdump_data_to_io_cache(IO_CACHE *file,
                                     my_off_t offset,
                                     uchar *ptr,
                                     my_off_t size)
{
  /*
    2 = '# '
    8 = address
    2 = '  '
    (HEXDUMP_BYTES_PER_LINE * 3 + 1) = Each byte prints as two hex digits,
       plus a space
    2 = ' |'
    HEXDUMP_BYTES_PER_LINE = text representation
    2 = '|\n'
    1 = '\0'
  */
  char emit_buffer[2 + 8 + 2 + (HEXDUMP_BYTES_PER_LINE * 3 + 1) + 2 +
    HEXDUMP_BYTES_PER_LINE + 2 + 1 ];
  char *h,*c;
  my_off_t i;

  if (size == 0)
    return 0;                                   // ok, nothing to do

  format_hex_line(emit_buffer);
  /*
    Print the rest of the event (without common header)
  */
  my_off_t starting_offset = offset;
  for (i= 0,
       c= emit_buffer + 2 + 8 + 2 + (HEXDUMP_BYTES_PER_LINE * 3 + 1) + 2,
       h= emit_buffer + 2 + 8 + 2;
       i < size;
       i++, ptr++)
  {
    my_snprintf(h, 4, "%02x ", *ptr);
    h+= 3;

    *c++= my_isprint(&my_charset_bin, *ptr) ? *ptr : '.';

    /* Print in groups of HEXDUMP_BYTES_PER_LINE characters. */
    if ((i % HEXDUMP_BYTES_PER_LINE) == (HEXDUMP_BYTES_PER_LINE - 1))
    {
      /* remove \0 left after printing hex byte representation */
      *h= ' ';
      /* prepare space to print address */
      memset(emit_buffer + 2, ' ', 8);
      /* print address */
      size_t const emit_buf_written= my_snprintf(emit_buffer + 2, 9, "%8llx",
                                                 (ulonglong) starting_offset);
      /* remove \0 left after printing address */
      emit_buffer[2 + emit_buf_written]= ' ';
      if (my_b_write(file, reinterpret_cast<uchar*>(emit_buffer),
                     sizeof(emit_buffer) - 1))
        goto err;
      c= emit_buffer + 2 + 8 + 2 + (HEXDUMP_BYTES_PER_LINE * 3 + 1) + 2;
      h= emit_buffer + 2 + 8 + 2;
      format_hex_line(emit_buffer);
      starting_offset+= HEXDUMP_BYTES_PER_LINE;
    }
    else if ((i % (HEXDUMP_BYTES_PER_LINE / 2))
             == ((HEXDUMP_BYTES_PER_LINE / 2) - 1))
    {
      /*
        In the middle of the group of HEXDUMP_BYTES_PER_LINE, emit an extra
        space in the hex string, to make two groups.
      */
      *h++= ' ';
    }

  }

  /*
    There is still data left in our buffer, which means that the previous
    line was not perfectly HEXDUMP_BYTES_PER_LINE characters, so write an
    incomplete line, with spaces to pad out to the same length as a full
    line would be, to make things more readable.
  */
  if (h != emit_buffer + 2 + 8 + 2)
  {
    *h= ' ';
    *c++= '|'; *c++= '\n';
    memset(emit_buffer + 2, ' ', 8);
    size_t const emit_buf_written= my_snprintf(emit_buffer + 2, 9, "%8llx",
                                               (ulonglong) starting_offset);
    emit_buffer[2 + emit_buf_written]= ' ';
    /* pad unprinted area */
    memset(h, ' ',
           (HEXDUMP_BYTES_PER_LINE * 3 + 1) - (h - (emit_buffer + 2 + 8 + 2)));
    if (my_b_write(file, reinterpret_cast<uchar*>(emit_buffer),
                   c - emit_buffer))
      goto err;
  }
  if (my_b_write(file, (uchar*)"#\n", 2))
    goto err;

  return 0;
err:
  return 1;
}

static inline bool is_numeric_type(uint type)
{
  switch (type)
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_NEWDECIMAL:
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
    return true;
  default:
    return false;
  }
  return false;
}

static inline bool is_character_type(uint type)
{
  switch (type)
  {
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_BLOB:
  // Base class is blob for geom type
  case MYSQL_TYPE_GEOMETRY:
    return true;
  default:
    return false;
  }
}

static inline bool is_enum_or_set_type(uint type) {
  return type == MYSQL_TYPE_ENUM || type == MYSQL_TYPE_SET;
}


/*
  Log_event::print_header()
*/

bool Log_event::print_header(IO_CACHE* file,
                             PRINT_EVENT_INFO* print_event_info,
                             bool is_more __attribute__((unused)))
{
  char llbuff[22];
  my_off_t hexdump_from= print_event_info->hexdump_from;
  DBUG_ENTER("Log_event::print_header");

  if (my_b_write_byte(file, '#') ||
      print_timestamp(file) ||
      my_b_printf(file, " server id %lu  end_log_pos %s ", (ulong) server_id,
                  llstr(log_pos,llbuff)))
    goto err;

  /* print the checksum */

  if (checksum_alg != BINLOG_CHECKSUM_ALG_OFF &&
      checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF)
  {
    char checksum_buf[BINLOG_CHECKSUM_LEN * 2 + 4]; // to fit to "%p "
    size_t const bytes_written=
      my_snprintf(checksum_buf, sizeof(checksum_buf), "0x%08x ", crc);
    if (my_b_printf(file, "%s ", get_type(&binlog_checksum_typelib,
                                          checksum_alg)) ||
        my_b_printf(file, checksum_buf, bytes_written))
      goto err;
  }

  /* mysqlbinlog --hexdump */
  if (print_event_info->hexdump_from)
  {
    my_b_write_byte(file, '\n');
    uchar *ptr= (uchar*)temp_buf;
    my_off_t size= uint4korr(ptr + EVENT_LEN_OFFSET);
    my_off_t hdr_len= get_header_len(print_event_info->common_header_len);

    size-= hdr_len;

    if (my_b_printf(file, "# Position\n"))
      goto err;

    /* Write the header, nicely formatted by field. */
    if (hexdump_minimal_header_to_io_cache(file, hexdump_from, ptr))
      goto err;

    ptr+= hdr_len;
    hexdump_from+= hdr_len;

    /* Print the rest of the data, mimicking "hexdump -C" output. */
    if (hexdump_data_to_io_cache(file, hexdump_from, ptr, size))
      goto err;

    /*
      Prefix the next line so that the output from print_helper()
      will appear as a comment.
    */
    if (my_b_write(file, (uchar*)"# Event: ", 9))
      goto err;
  }

  DBUG_RETURN(0);

err:
  DBUG_RETURN(1);
}


/**
  Prints a quoted string to io cache.
  Control characters are displayed as hex sequence, e.g. \x00
  Single-quote and backslash characters are escaped with a \

  @param[in] file              IO cache
  @param[in] prt               Pointer to string
  @param[in] length            String length
*/

static void
my_b_write_quoted(IO_CACHE *file, const uchar *ptr, uint length)
{
  const uchar *s;
  my_b_write_byte(file, '\'');
  for (s= ptr; length > 0 ; s++, length--)
  {
    if (*s > 0x1F)
      my_b_write_byte(file, *s);
    else if (*s == '\'')
      my_b_write(file, (uchar*)"\\'", 2);
    else if (*s == '\\')
      my_b_write(file, (uchar*)"\\\\", 2);
    else
    {
      uchar hex[10];
      size_t len= my_snprintf((char*) hex, sizeof(hex), "%s%02x", "\\x", *s);
      my_b_write(file, hex, len);
    }
  }
  my_b_write_byte(file, '\'');
}


/**
  Prints a bit string to io cache in format  b'1010'.
  
  @param[in] file              IO cache
  @param[in] ptr               Pointer to string
  @param[in] nbits             Number of bits
*/
static void
my_b_write_bit(IO_CACHE *file, const uchar *ptr, uint nbits)
{
  uint bitnum, nbits8= ((nbits + 7) / 8) * 8, skip_bits= nbits8 - nbits;
  my_b_write(file, (uchar*)"b'", 2);
  for (bitnum= skip_bits ; bitnum < nbits8; bitnum++)
  {
    int is_set= (ptr[(bitnum) / 8] >> (7 - bitnum % 8))  & 0x01;
    my_b_write_byte(file, (is_set ? '1' : '0'));
  }
  my_b_write_byte(file, '\'');
}


/**
  Prints a packed string to io cache.
  The string consists of length packed to 1 or 2 bytes,
  followed by string data itself.
  
  @param[in] file              IO cache
  @param[in] ptr               Pointer to string
  @param[in] length            String size
  
  @retval   - number of bytes scanned.
*/
static size_t
my_b_write_quoted_with_length(IO_CACHE *file, const uchar *ptr, uint length)
{
  if (length < 256)
  {
    length= *ptr;
    my_b_write_quoted(file, ptr + 1, length);
    return length + 1;
  }
  else
  {
    length= uint2korr(ptr);
    my_b_write_quoted(file, ptr + 2, length);
    return length + 2;
  }
}


/**
  Prints a 32-bit number in both signed and unsigned representation
  
  @param[in] file              IO cache
  @param[in] sl                Signed number
  @param[in] ul                Unsigned number
*/
static bool
my_b_write_sint32_and_uint32(IO_CACHE *file, int32 si, uint32 ui)
{
  bool res= my_b_printf(file, "%d", si);
  if (si < 0)
    if (my_b_printf(file, " (%u)", ui))
      res= 1;
  return res;
}


/**
  Print a packed value of the given SQL type into IO cache
  
  @param[in] file              IO cache
  @param[in] ptr               Pointer to string
  @param[in] type              Column type
  @param[in] meta              Column meta information
  @param[out] typestr          SQL type string buffer (for verbose output)
  @param[out] typestr_length   Size of typestr
  
  @retval   - number of bytes scanned from ptr.
              Except in case of NULL, in which case we return 1 to indicate ok
*/

static size_t
log_event_print_value(IO_CACHE *file, PRINT_EVENT_INFO *print_event_info,
                      const uchar *ptr, uint type, uint meta,
                      char *typestr, size_t typestr_length)
{
  uint32 length= 0;

  if (type == MYSQL_TYPE_STRING)
  {
    if (meta >= 256)
    {
      uint byte0= meta >> 8;
      uint byte1= meta & 0xFF;
      
      if ((byte0 & 0x30) != 0x30)
      {
        /* a long CHAR() field: see #37426 */
        length= byte1 | (((byte0 & 0x30) ^ 0x30) << 4);
        type= byte0 | 0x30;
      }
      else
        length = meta & 0xFF;
    }
    else
      length= meta;
  }

  switch (type) {
  case MYSQL_TYPE_LONG:
    {
      strmake(typestr, "INT", typestr_length);
      if (!ptr)
        goto return_null;

      int32 si= sint4korr(ptr);
      uint32 ui= uint4korr(ptr);
      my_b_write_sint32_and_uint32(file, si, ui);
      return 4;
    }

  case MYSQL_TYPE_TINY:
    {
      strmake(typestr, "TINYINT", typestr_length);
      if (!ptr)
        goto return_null;

      my_b_write_sint32_and_uint32(file, (int) (signed char) *ptr,
                                  (uint) (unsigned char) *ptr);
      return 1;
    }

  case MYSQL_TYPE_SHORT:
    {
      strmake(typestr, "SHORTINT", typestr_length);
      if (!ptr)
        goto return_null;

      int32 si= (int32) sint2korr(ptr);
      uint32 ui= (uint32) uint2korr(ptr);
      my_b_write_sint32_and_uint32(file, si, ui);
      return 2;
    }
  
  case MYSQL_TYPE_INT24:
    {
      strmake(typestr, "MEDIUMINT", typestr_length);
      if (!ptr)
        goto return_null;

      int32 si= sint3korr(ptr);
      uint32 ui= uint3korr(ptr);
      my_b_write_sint32_and_uint32(file, si, ui);
      return 3;
    }

  case MYSQL_TYPE_LONGLONG:
    {
      strmake(typestr, "LONGINT", typestr_length);
      if (!ptr)
        goto return_null;

      char tmp[64];
      size_t length;
      longlong si= sint8korr(ptr);
      length= (longlong10_to_str(si, tmp, -10) - tmp);
      my_b_write(file, (uchar*)tmp, length);
      if (si < 0)
      {
        ulonglong ui= uint8korr(ptr);
        longlong10_to_str((longlong) ui, tmp, 10);
        my_b_printf(file, " (%s)", tmp);        
      }
      return 8;
    }

  case MYSQL_TYPE_NEWDECIMAL:
    {
      uint precision= meta >> 8;
      uint decimals= meta & 0xFF;
      my_snprintf(typestr, typestr_length, "DECIMAL(%d,%d)",
                  precision, decimals);
      if (!ptr)
        goto return_null;

      uint bin_size= my_decimal_get_binary_size(precision, decimals);
      my_decimal dec((const uchar *) ptr, precision, decimals);
      int length= DECIMAL_MAX_STR_LENGTH;
      char buff[DECIMAL_MAX_STR_LENGTH + 1];
      decimal2string(&dec, buff, &length, 0, 0, 0);
      my_b_write(file, (uchar*)buff, length);
      return bin_size;
    }

  case MYSQL_TYPE_FLOAT:
    {
      strmake(typestr, "FLOAT", typestr_length);
      if (!ptr)
        goto return_null;

      float fl;
      float4get(fl, ptr);
      char tmp[320];
      sprintf(tmp, "%-20g", (double) fl);
      my_b_printf(file, "%s", tmp); /* my_snprintf doesn't support %-20g */
      return 4;
    }

  case MYSQL_TYPE_DOUBLE:
    {
      double dbl;
      strmake(typestr, "DOUBLE", typestr_length);
      if (!ptr)
        goto return_null;

      float8get(dbl, ptr);
      char tmp[320];
      sprintf(tmp, "%-.20g", dbl); /* strmake doesn't support %-20g */
      my_b_printf(file, tmp, "%s");
      return 8;
    }
  
  case MYSQL_TYPE_BIT:
    {
      /* Meta-data: bit_len, bytes_in_rec, 2 bytes */
      uint nbits= ((meta >> 8) * 8) + (meta & 0xFF);
      my_snprintf(typestr, typestr_length, "BIT(%d)", nbits);
      if (!ptr)
        goto return_null;

      length= (nbits + 7) / 8;
      my_b_write_bit(file, ptr, nbits);
      return length;
    }

  case MYSQL_TYPE_TIMESTAMP:
    {
      strmake(typestr, "TIMESTAMP", typestr_length);
      if (!ptr)
        goto return_null;

      uint32 i32= uint4korr(ptr);
      my_b_printf(file, "%d", i32);
      return 4;
    }

  case MYSQL_TYPE_TIMESTAMP2:
    {
      my_snprintf(typestr, typestr_length, "TIMESTAMP(%d)", meta);
      if (!ptr)
        goto return_null;

      char buf[MAX_DATE_STRING_REP_LENGTH];
      struct timeval tm;
      my_timestamp_from_binary(&tm, ptr, meta);
      int buflen= my_timeval_to_str(&tm, buf, meta);
      my_b_write(file, (uchar*)buf, buflen);
      return my_timestamp_binary_length(meta);
    }

  case MYSQL_TYPE_DATETIME:
    {
      strmake(typestr, "DATETIME", typestr_length);
      if (!ptr)
        goto return_null;

      ulong d, t;
      uint64 i64= uint8korr(ptr); /* YYYYMMDDhhmmss */
      d= (ulong) (i64 / 1000000);
      t= (ulong) (i64 % 1000000);

      my_b_printf(file, "'%04d-%02d-%02d %02d:%02d:%02d'",
                  (int) (d / 10000), (int) (d % 10000) / 100, (int) (d % 100),
                  (int) (t / 10000), (int) (t % 10000) / 100, (int) t % 100);
      return 8;
    }

  case MYSQL_TYPE_DATETIME2:
    {
      my_snprintf(typestr, typestr_length, "DATETIME(%d)", meta);
      if (!ptr)
        goto return_null;

      char buf[MAX_DATE_STRING_REP_LENGTH];
      MYSQL_TIME ltime;
      longlong packed= my_datetime_packed_from_binary(ptr, meta);
      TIME_from_longlong_datetime_packed(&ltime, packed);
      int buflen= my_datetime_to_str(&ltime, buf, meta);
      my_b_write_quoted(file, (uchar *) buf, buflen);
      return my_datetime_binary_length(meta);
    }

  case MYSQL_TYPE_TIME:
    {
      strmake(typestr, "TIME",  typestr_length);
      if (!ptr)
        goto return_null;

      int32 tmp= sint3korr(ptr);
      int32 i32= tmp >= 0 ? tmp : - tmp;
      const char *sign= tmp < 0 ? "-" : "";
      my_b_printf(file, "'%s%02d:%02d:%02d'",
                  sign, i32 / 10000, (i32 % 10000) / 100, i32 % 100, i32);
      return 3;
    }

  case MYSQL_TYPE_TIME2:
    {
      my_snprintf(typestr, typestr_length, "TIME(%d)", meta);
      if (!ptr)
        goto return_null;

      char buf[MAX_DATE_STRING_REP_LENGTH];
      MYSQL_TIME ltime;
      longlong packed= my_time_packed_from_binary(ptr, meta);
      TIME_from_longlong_time_packed(&ltime, packed);
      int buflen= my_time_to_str(&ltime, buf, meta);
      my_b_write_quoted(file, (uchar *) buf, buflen);
      return my_time_binary_length(meta);
    }

  case MYSQL_TYPE_NEWDATE:
    {
      strmake(typestr, "DATE", typestr_length);
      if (!ptr)
        goto return_null;

      uint32 tmp= uint3korr(ptr);
      int part;
      char buf[11];
      char *pos= &buf[10];  // start from '\0' to the beginning

      /* Copied from field.cc */
      *pos--=0;					// End NULL
      part=(int) (tmp & 31);
      *pos--= (char) ('0'+part%10);
      *pos--= (char) ('0'+part/10);
      *pos--= ':';
      part=(int) (tmp >> 5 & 15);
      *pos--= (char) ('0'+part%10);
      *pos--= (char) ('0'+part/10);
      *pos--= ':';
      part=(int) (tmp >> 9);
      *pos--= (char) ('0'+part%10); part/=10;
      *pos--= (char) ('0'+part%10); part/=10;
      *pos--= (char) ('0'+part%10); part/=10;
      *pos=   (char) ('0'+part);
      my_b_printf(file , "'%s'", buf);
      return 3;
    }
    
  case MYSQL_TYPE_DATE:
    {
      strmake(typestr, "DATE", typestr_length);
      if (!ptr)
        goto return_null;

      uint i32= uint3korr(ptr);
      my_b_printf(file , "'%04d:%02d:%02d'",
                  (int)(i32 / (16L * 32L)), (int)(i32 / 32L % 16L),
                  (int)(i32 % 32L));
      return 3;
    }
  
  case MYSQL_TYPE_YEAR:
    {
      strmake(typestr, "YEAR", typestr_length);
      if (!ptr)
        goto return_null;

      uint32 i32= *ptr;
      my_b_printf(file, "%04d", i32+ 1900);
      return 1;
    }
  
  case MYSQL_TYPE_ENUM:
    switch (meta & 0xFF) {
    case 1:
      strmake(typestr, "ENUM(1 byte)", typestr_length);
      if (!ptr)
        goto return_null;

      my_b_printf(file, "%d", (int) *ptr);
      return 1;
    case 2:
      {
        strmake(typestr, "ENUM(2 bytes)", typestr_length);
        if (!ptr)
          goto return_null;

        int32 i32= uint2korr(ptr);
        my_b_printf(file, "%d", i32);
        return 2;
      }
    default:
      my_b_printf(file, "!! Unknown ENUM packlen=%d", meta & 0xFF); 
      return 0;
    }
    break;
    
  case MYSQL_TYPE_SET:
    my_snprintf(typestr, typestr_length, "SET(%d bytes)", meta & 0xFF);
      if (!ptr)
        goto return_null;

    my_b_write_bit(file, ptr , (meta & 0xFF) * 8);
    return meta & 0xFF;
  
  case MYSQL_TYPE_BLOB:
    switch (meta) {
    case 1:
      strmake(typestr, "TINYBLOB/TINYTEXT", typestr_length);
      if (!ptr)
        goto return_null;

      length= *ptr;
      my_b_write_quoted(file, ptr + 1, length);
      return length + 1;
    case 2:
      strmake(typestr, "BLOB/TEXT", typestr_length);
      if (!ptr)
        goto return_null;

      length= uint2korr(ptr);
      my_b_write_quoted(file, ptr + 2, length);
      return length + 2;
    case 3:
      strmake(typestr, "MEDIUMBLOB/MEDIUMTEXT", typestr_length);
      if (!ptr)
        goto return_null;

      length= uint3korr(ptr);
      my_b_write_quoted(file, ptr + 3, length);
      return length + 3;
    case 4:
      strmake(typestr, "LONGBLOB/LONGTEXT", typestr_length);
      if (!ptr)
        goto return_null;

      length= uint4korr(ptr);
      my_b_write_quoted(file, ptr + 4, length);
      return length + 4;
    default:
      my_b_printf(file, "!! Unknown BLOB packlen=%d", length);
      return 0;
    }

  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VAR_STRING:
    length= meta;
    my_snprintf(typestr, typestr_length, "VARSTRING(%d)", length);
    if (!ptr)
      goto return_null;

    return my_b_write_quoted_with_length(file, ptr, length);

  case MYSQL_TYPE_STRING:
    my_snprintf(typestr, typestr_length, "STRING(%d)", length);
    if (!ptr)
      goto return_null;

    return my_b_write_quoted_with_length(file, ptr, length);

  case MYSQL_TYPE_DECIMAL:
    print_event_info->flush_for_error();
    fprintf(stderr, "\nError: Found Old DECIMAL (mysql-4.1 or earlier). "
            "Not enough metadata to display the value.\n");
    break;

  case MYSQL_TYPE_GEOMETRY:
    strmake(typestr, "GEOMETRY", typestr_length);
    if (!ptr)
      goto return_null;

    length= uint4korr(ptr);
    my_b_write_quoted(file, ptr + meta, length);
    return length + meta;

  default:
    print_event_info->flush_for_error();
    fprintf(stderr,
            "\nError: Don't know how to handle column type: %d meta: %d (%04x)\n",
            type, meta, meta);
    break;
  }
  *typestr= 0;
  return 0;

return_null:
  return my_b_write(file, (uchar*) "NULL", 4) ? 0 : 1;
}


/**
  Print a packed row into IO cache
  
  @param[in] file              IO cache
  @param[in] td                Table definition
  @param[in] print_event_into  Print parameters
  @param[in] cols_bitmap       Column bitmaps.
  @param[in] value             Pointer to packed row
  @param[in] prefix            Row's SQL clause ("SET", "WHERE", etc)
  
  @retval   0 error
            # number of bytes scanned.
*/


size_t
Rows_log_event::print_verbose_one_row(IO_CACHE *file, table_def *td,
                                      PRINT_EVENT_INFO *print_event_info,
                                      MY_BITMAP *cols_bitmap,
                                      const uchar *value, const uchar *prefix,
                                      const my_bool no_fill_output)
{
  const uchar *value0= value;
  const uchar *null_bits= value;
  uint null_bit_index= 0;
  char typestr[64]= "";

#ifdef WHEN_FLASHBACK_REVIEW_READY
  /* Storing the review SQL */
  IO_CACHE *review_sql= &print_event_info->review_sql_cache;
  LEX_STRING review_str;
#endif

  /*
    Skip metadata bytes which gives the information about nullabity of master
    columns. Master writes one bit for each affected column.
   */

  value+= (bitmap_bits_set(cols_bitmap) + 7) / 8;

  if (!no_fill_output)
    if (my_b_printf(file, "%s", prefix))
      goto err;

  for (uint i= 0; i < (uint)td->size(); i ++)
  {
    size_t size;
    int is_null= (null_bits[null_bit_index / 8] 
                  >> (null_bit_index % 8))  & 0x01;

    if (bitmap_is_set(cols_bitmap, i) == 0)
      continue;

    if (!no_fill_output)
      if (my_b_printf(file, "###   @%d=", static_cast<int>(i + 1)))
        goto err;

    if (!is_null)
    {
      size_t fsize= td->calc_field_size((uint)i, (uchar*) value);
      if (value + fsize > m_rows_end)
      {
        if (!no_fill_output)
          if (my_b_printf(file, "***Corrupted replication event was detected."
                          " Not printing the value***\n"))
            goto err;
        value+= fsize;
        return 0;
      }
    }

    if (!no_fill_output)
    {
      size= log_event_print_value(file, print_event_info, is_null? NULL: value,
                                  td->type(i), td->field_metadata(i),
                                  typestr, sizeof(typestr));
#ifdef WHEN_FLASHBACK_REVIEW_READY
      if (need_flashback_review)
      {
        String tmp_str, hex_str;
        IO_CACHE tmp_cache;

        // Using a tmp IO_CACHE to get the value output
        open_cached_file(&tmp_cache, NULL, NULL, 0, MYF(MY_WME | MY_NABP));
        size= log_event_print_value(&tmp_cache, print_event_info,
                                    is_null ? NULL: value,
                                    td->type(i), td->field_metadata(i),
                                    typestr, sizeof(typestr));
        error= copy_event_cache_to_string_and_reinit(&tmp_cache, &review_str);
        close_cached_file(&tmp_cache);
        if (unlikely(error))
          return 0;

        switch (td->type(i)) // Converting a string to HEX format
        {
          case MYSQL_TYPE_VARCHAR:
          case MYSQL_TYPE_VAR_STRING:
          case MYSQL_TYPE_STRING:
          case MYSQL_TYPE_BLOB:
            // Avoid write_pos changed to a new area
            // tmp_str.free();
            tmp_str.append(review_str.str + 1, review_str.length - 2); // Removing quotation marks
            if (hex_str.alloc(tmp_str.length()*2+1)) // If out of memory
            {
              fprintf(stderr, "\nError: Out of memory. "
                      "Could not print correct binlog event.\n");
              exit(1);
            }
            octet2hex((char*) hex_str.ptr(), tmp_str.ptr(), tmp_str.length());
            if (my_b_printf(review_sql, ", UNHEX('%s')", hex_str.ptr()))
              goto err;
            break;
          default:
            tmp_str.free();
            if (tmp_str.append(review_str.str, review_str.length) ||
                my_b_printf(review_sql, ", %s", tmp_str.ptr()))
              goto err;
            break;
        }
        my_free(revieww_str.str);
      }
#endif
    }
    else
    {
      IO_CACHE tmp_cache;
      open_cached_file(&tmp_cache, NULL, NULL, 0, MYF(MY_WME | MY_NABP));
      size= log_event_print_value(&tmp_cache, print_event_info,
                                  is_null ? NULL: value,
                                  td->type(i), td->field_metadata(i),
                                  typestr, sizeof(typestr));
      close_cached_file(&tmp_cache);
    }

    if (!size)
      goto err;

    if (!is_null)
      value+= size;

    if (print_event_info->verbose > 1 && !no_fill_output)
    {
      if (my_b_write(file, (uchar*)" /* ", 4) ||
          my_b_printf(file, "%s ", typestr) ||
          my_b_printf(file, "meta=%d nullable=%d is_null=%d ",
                      td->field_metadata(i),
                      td->maybe_null(i), is_null) ||
          my_b_write(file, (uchar*)"*/", 2))
        goto err;
    }

    if (!no_fill_output)
      if (my_b_write_byte(file, '\n'))
        goto err;

    null_bit_index++;
  }
  return value - value0;

err:
  return 0;
}


/**
  Exchange the SET part and WHERE part for the Update events.
  Revert the operations order for the Write and Delete events.
  And then revert the events order from the last one to the first one.

  @param[in] print_event_info   PRINT_EVENT_INFO
  @param[in] rows_buff          Packed event buff
*/

void Rows_log_event::change_to_flashback_event(PRINT_EVENT_INFO *print_event_info,
                                               uchar *rows_buff, Log_event_type ev_type)
{
  Table_map_log_event *map;
  table_def *td;
  DYNAMIC_ARRAY rows_arr;
  uchar *swap_buff1;
  uchar *rows_pos= rows_buff + m_rows_before_size;

  if (!(map= print_event_info->m_table_map.get_table(m_table_id)) ||
      !(td= map->create_table_def()))
    return;

  /* If the write rows event contained no values for the AI */
  if (((get_general_type_code() == WRITE_ROWS_EVENT) && (m_rows_buf==m_rows_end)))
    goto end;

  (void) my_init_dynamic_array(PSI_NOT_INSTRUMENTED, &rows_arr, sizeof(LEX_STRING), 8, 8, MYF(0));

  for (uchar *value= m_rows_buf; value < m_rows_end; )
  {
    uchar *start_pos= value;
    size_t length1= 0;
    if (!(length1= print_verbose_one_row(NULL, td, print_event_info,
                                         &m_cols, value,
                                         (const uchar*) "", TRUE)))
    {
      fprintf(stderr, "\nError row length: %zu\n", length1);
      exit(1);
    }
    value+= length1;

    swap_buff1= (uchar *) my_malloc(PSI_NOT_INSTRUMENTED, length1, MYF(0));
    if (!swap_buff1)
    {
      fprintf(stderr, "\nError: Out of memory. "
              "Could not exchange to flashback event.\n");
      exit(1);
    }
    memcpy(swap_buff1, start_pos, length1);

    // For Update_event, we have the second part
    size_t length2= 0;
    if (ev_type == UPDATE_ROWS_EVENT ||
        ev_type == UPDATE_ROWS_EVENT_V1)
    {
      if (!(length2= print_verbose_one_row(NULL, td, print_event_info,
                                           &m_cols, value,
                                           (const uchar*) "", TRUE)))
      {
        fprintf(stderr, "\nError row length: %zu\n", length2);
        exit(1);
      }
      value+= length2;

      void *swap_buff2= my_malloc(PSI_NOT_INSTRUMENTED, length2, MYF(0));
      if (!swap_buff2)
      {
        fprintf(stderr, "\nError: Out of memory. "
                "Could not exchange to flashback event.\n");
        exit(1);
      }
      memcpy(swap_buff2, start_pos + length1, length2); // WHERE part

      /* Swap SET and WHERE part */
      memcpy(start_pos, swap_buff2, length2);
      memcpy(start_pos + length2, swap_buff1, length1);
      my_free(swap_buff2);
    }

    my_free(swap_buff1);

    /* Copying one row into a buff, and pushing into the array */
    LEX_STRING one_row;

    one_row.length= length1 + length2;
    one_row.str=    (char *) my_malloc(PSI_NOT_INSTRUMENTED, one_row.length, MYF(0));
    memcpy(one_row.str, start_pos, one_row.length);
    if (one_row.str == NULL || push_dynamic(&rows_arr, (uchar *) &one_row))
    {
      fprintf(stderr, "\nError: Out of memory. "
              "Could not push flashback event into array.\n");
      exit(1);
    }
  }

  /* Copying rows from the end to the begining into event */
  for (size_t i= rows_arr.elements; i > 0; --i)
  {
    LEX_STRING *one_row= dynamic_element(&rows_arr, i - 1, LEX_STRING*);

    memcpy(rows_pos, (uchar *)one_row->str, one_row->length);
    rows_pos+= one_row->length;
    my_free(one_row->str);
  }
  delete_dynamic(&rows_arr);

end:
  delete td;
}

/**
  Calc length of a packed value of the given SQL type

  @param[in] ptr               Pointer to string
  @param[in] type              Column type
  @param[in] meta              Column meta information

  @retval   - number of bytes scanned from ptr.
              Except in case of NULL, in which case we return 1 to indicate ok
*/

static size_t calc_field_event_length(const uchar *ptr, uint type, uint meta)
{
  uint32 length= 0;

  if (type == MYSQL_TYPE_STRING)
  {
    if (meta >= 256)
    {
      uint byte0= meta >> 8;
      uint byte1= meta & 0xFF;

      if ((byte0 & 0x30) != 0x30)
      {
        /* a long CHAR() field: see #37426 */
        length= byte1 | (((byte0 & 0x30) ^ 0x30) << 4);
        type= byte0 | 0x30;
      }
      else
        length = meta & 0xFF;
    }
    else
      length= meta;
  }

  switch (type) {
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_TIMESTAMP:
    return 4;
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_YEAR:
    return 1;
  case MYSQL_TYPE_SHORT:
    return 2;
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_NEWDATE:
  case MYSQL_TYPE_DATE:
      return 3;
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_DATETIME:
    return 8;
  case MYSQL_TYPE_NEWDECIMAL:
  {
    uint precision= meta >> 8;
    uint decimals= meta & 0xFF;
    uint bin_size= my_decimal_get_binary_size(precision, decimals);
    return bin_size;
  }
  case MYSQL_TYPE_FLOAT:
    return 4;
  case MYSQL_TYPE_DOUBLE:
    return 8;
  case MYSQL_TYPE_BIT:
  {
    /* Meta-data: bit_len, bytes_in_rec, 2 bytes */
    uint nbits= ((meta >> 8) * 8) + (meta & 0xFF);
    length= (nbits + 7) / 8;
    return length;
  }
  case MYSQL_TYPE_TIMESTAMP2:
    return my_timestamp_binary_length(meta);
  case MYSQL_TYPE_DATETIME2:
    return my_datetime_binary_length(meta);
  case MYSQL_TYPE_TIME2:
    return my_time_binary_length(meta);
  case MYSQL_TYPE_ENUM:
    switch (meta & 0xFF) {
    case 1:
    case 2:
      return (meta & 0xFF);
    default:
      /* Unknown ENUM packlen=%d", meta & 0xFF */
      return 0;
    }
    break;
  case MYSQL_TYPE_SET:
    return meta & 0xFF;
  case MYSQL_TYPE_BLOB:
    switch (meta) {
    default:
      return 0;
    case 1:
      return *ptr + 1;
    case 2:
      return uint2korr(ptr) + 2;
    case 3:
      return uint3korr(ptr) + 3;
    case 4:
      return uint4korr(ptr) + 4;
    }
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VAR_STRING:
    length= meta;
    /* fall through */
  case MYSQL_TYPE_STRING:
    if (length < 256)
      return (uint) *ptr + 1;
    return uint2korr(ptr) + 2;
  case MYSQL_TYPE_DECIMAL:
    break;
  default:
    break;
  }
  return 0;
}


size_t
Rows_log_event::calc_row_event_length(table_def *td,
                                      PRINT_EVENT_INFO *print_event_info,
                                      MY_BITMAP *cols_bitmap,
                                      const uchar *value)
{
  const uchar *value0= value;
  const uchar *null_bits= value;
  uint null_bit_index= 0;

  /*
    Skip metadata bytes which gives the information about nullabity of master
    columns. Master writes one bit for each affected column.
   */

  value+= (bitmap_bits_set(cols_bitmap) + 7) / 8;

  for (uint i= 0; i < (uint)td->size(); i ++)
  {
    int is_null;
    is_null= (null_bits[null_bit_index / 8] >> (null_bit_index % 8)) & 0x01;

    if (bitmap_is_set(cols_bitmap, i) == 0)
      continue;

    if (!is_null)
    {
      size_t size;
      size_t fsize= td->calc_field_size((uint)i, (uchar*) value);
      if (value + fsize > m_rows_end)
      {
        /* Corrupted replication event was detected, skipping entry */
        return 0;
      }
      if (!(size= calc_field_event_length(value, td->type(i),
                                          td->field_metadata(i))))
        return 0;
      value+= size;
    }
    null_bit_index++;
  }
  return value - value0;
}


/**
   Calculate how many rows there are in the event

  @param[in] file              IO cache
  @param[in] print_event_into  Print parameters
*/

void Rows_log_event::count_row_events(PRINT_EVENT_INFO *print_event_info)
{
  Table_map_log_event *map;
  table_def *td;
  uint row_events;
  Log_event_type general_type_code= get_general_type_code();

  switch (general_type_code) {
  case WRITE_ROWS_EVENT:
  case DELETE_ROWS_EVENT:
    row_events= 1;
    break;
  case UPDATE_ROWS_EVENT:
    row_events= 2;
    break;
  default:
    DBUG_ASSERT(0); /* Not possible */
    return;
  }

  if (!(map= print_event_info->m_table_map.get_table(m_table_id)) ||
      !(td= map->create_table_def()))
  {
    /* Row event for unknown table */
    return;
  }

  for (const uchar *value= m_rows_buf; value < m_rows_end; )
  {
    size_t length;
    print_event_info->row_events++;

    /* Print the first image */
    if (!(length= calc_row_event_length(td, print_event_info,
                                        &m_cols, value)))
      break;
    value+= length;
    DBUG_ASSERT(value <= m_rows_end);

    /* Print the second image (for UPDATE only) */
    if (row_events == 2)
    {
      if (!(length= calc_row_event_length(td, print_event_info,
                                          &m_cols_ai, value)))
        break;
      value+= length;
      DBUG_ASSERT(value <= m_rows_end);
    }
  }
  delete td;
}


/**
  Print a row event into IO cache in human readable form (in SQL format)

  @param[in] file              IO cache
  @param[in] print_event_into  Print parameters
*/

bool Rows_log_event::print_verbose(IO_CACHE *file,
                                   PRINT_EVENT_INFO *print_event_info)
{
  Table_map_log_event *map;
  table_def *td= 0;
  const char *sql_command, *sql_clause1, *sql_clause2;
  const char *sql_command_short __attribute__((unused));
  Log_event_type general_type_code= get_general_type_code();
#ifdef WHEN_FLASHBACK_REVIEW_READY
  IO_CACHE *review_sql= &print_event_info->review_sql_cache;
#endif

  if (m_extra_row_data)
  {
    uint8 extra_data_len= m_extra_row_data[EXTRA_ROW_INFO_LEN_OFFSET];
    uint8 extra_payload_len= extra_data_len - EXTRA_ROW_INFO_HDR_BYTES;
    assert(extra_data_len >= EXTRA_ROW_INFO_HDR_BYTES);

    if (my_b_printf(file, "### Extra row data format: %u, len: %u :",
                    m_extra_row_data[EXTRA_ROW_INFO_FORMAT_OFFSET],
                    extra_payload_len))
      goto err;
    if (extra_payload_len)
    {
      /*
         Buffer for hex view of string, including '0x' prefix,
         2 hex chars / byte and trailing 0
      */
      const int buff_len= 2 + (256 * 2) + 1;
      char buff[buff_len];
      str_to_hex(buff, (const char*) &m_extra_row_data[EXTRA_ROW_INFO_HDR_BYTES],
                 extra_payload_len);
      if (my_b_printf(file, "%s", buff))
        goto err;
    }
    if (my_b_printf(file, "\n"))
      goto err;
  }

  switch (general_type_code) {
  case WRITE_ROWS_EVENT:
    sql_command= "INSERT INTO";
    sql_clause1= "### SET\n";
    sql_clause2= NULL;
    sql_command_short= "I";
    break;
  case DELETE_ROWS_EVENT:
    sql_command= "DELETE FROM";
    sql_clause1= "### WHERE\n";
    sql_clause2= NULL;
    sql_command_short= "D";
    break;
  case UPDATE_ROWS_EVENT:
    sql_command= "UPDATE";
    sql_clause1= "### WHERE\n";
    sql_clause2= "### SET\n";
    sql_command_short= "U";
    break;
  default:
    sql_command= sql_clause1= sql_clause2= NULL;
    sql_command_short= "";
    DBUG_ASSERT(0); /* Not possible */
  }

  if (!(map= print_event_info->m_table_map.get_table(m_table_id)) ||
      !(td= map->create_table_def()))
  {
    return (my_b_printf(file, "### Row event for unknown table #%lu",
                        (ulong) m_table_id));
  }

  /* If the write rows event contained no values for the AI */
  if (((general_type_code == WRITE_ROWS_EVENT) && (m_rows_buf==m_rows_end)))
  {
    if (my_b_printf(file, "### INSERT INTO %`s.%`s VALUES ()\n",
                    map->get_db_name(), map->get_table_name()))
      goto err;
    goto end;
  }

  for (const uchar *value= m_rows_buf; value < m_rows_end; )
  {
    size_t length;
    print_event_info->row_events++;

    if (my_b_printf(file, "### %s %`s.%`s\n",
                    sql_command,
                    map->get_db_name(), map->get_table_name()))
      goto err;
#ifdef WHEN_FLASHBACK_REVIEW_READY
    if (need_flashback_review)
      if (my_b_printf(review_sql, "\nINSERT INTO `%s`.`%s` VALUES ('%s'",
                      map->get_review_dbname(), map->get_review_tablename(),
                      sql_command_short))
        goto err;
#endif

    /* Print the first image */
    if (!(length= print_verbose_one_row(file, td, print_event_info,
                                  &m_cols, value,
                                  (const uchar*) sql_clause1)))
      goto err;
    value+= length;

    /* Print the second image (for UPDATE only) */
    if (sql_clause2)
    {
      if (!(length= print_verbose_one_row(file, td, print_event_info,
                                      &m_cols_ai, value,
                                      (const uchar*) sql_clause2)))
        goto err;
      value+= length;
    }
#ifdef WHEN_FLASHBACK_REVIEW_READY
    else
    {
      if (need_flashback_review)
        for (size_t i= 0; i < td->size(); i ++)
          if (my_b_printf(review_sql, ", NULL"))
            goto err;
    }

    if (need_flashback_review)
      if (my_b_printf(review_sql, ")%s\n", print_event_info->delimiter))
        goto err;
#endif
  }

end:
  delete td;
  return 0;
err:
  delete td;
  return 1;
}

void free_table_map_log_event(Table_map_log_event *event)
{
  delete event;
}

/**
  Encode the event, optionally per 'do_print_encoded' arg store the
  result into the argument cache; optionally per event_info's
  'verbose' print into the cache a verbose representation of the event.
  Note, no extra wrapping is done to the being io-cached data, like
  to producing a BINLOG query. It's left for a routine that extracts from
  the cache.

  @param file               pointer to IO_CACHE
  @param print_event_info   pointer to print_event_info specializing
                            what out of and how to print the event
  @param do_print_encoded   whether to store base64-encoded event
                            into @file.
*/
bool Log_event::print_base64(IO_CACHE* file,
                             PRINT_EVENT_INFO* print_event_info,
                             bool do_print_encoded)
{
  uchar *ptr= temp_buf;
  uint32 size= uint4korr(ptr + EVENT_LEN_OFFSET);
  DBUG_ENTER("Log_event::print_base64");

  if (is_flashback)
  {
    uint tmp_size= size;
    Rows_log_event *ev= NULL;
    Log_event_type ev_type = (enum Log_event_type) ptr[EVENT_TYPE_OFFSET];
    if (checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF &&
        checksum_alg != BINLOG_CHECKSUM_ALG_OFF)
      tmp_size-= BINLOG_CHECKSUM_LEN; // checksum is displayed through the header
    switch (ev_type) {
      case WRITE_ROWS_EVENT:
        ptr[EVENT_TYPE_OFFSET]= DELETE_ROWS_EVENT;
        ev= new Delete_rows_log_event(ptr, tmp_size,
                                       glob_description_event);
        ev->change_to_flashback_event(print_event_info, ptr, ev_type);
        break;
      case WRITE_ROWS_EVENT_V1:
        ptr[EVENT_TYPE_OFFSET]= DELETE_ROWS_EVENT_V1;
        ev= new Delete_rows_log_event(ptr, tmp_size,
                                       glob_description_event);
        ev->change_to_flashback_event(print_event_info, ptr, ev_type);
        break;
      case DELETE_ROWS_EVENT:
        ptr[EVENT_TYPE_OFFSET]= WRITE_ROWS_EVENT;
        ev= new Write_rows_log_event(ptr, tmp_size,
                                       glob_description_event);
        ev->change_to_flashback_event(print_event_info, ptr, ev_type);
        break;
      case DELETE_ROWS_EVENT_V1:
        ptr[EVENT_TYPE_OFFSET]= WRITE_ROWS_EVENT_V1;
        ev= new Write_rows_log_event(ptr, tmp_size,
                                       glob_description_event);
        ev->change_to_flashback_event(print_event_info, ptr, ev_type);
        break;
      case UPDATE_ROWS_EVENT:
      case UPDATE_ROWS_EVENT_V1:
        ev= new Update_rows_log_event(ptr, tmp_size,
                                       glob_description_event);
        ev->change_to_flashback_event(print_event_info, ptr, ev_type);
        break;
      default:
        break;
    }
    delete ev;
  }

  if (do_print_encoded)
  {
    size_t const tmp_str_sz= my_base64_needed_encoded_length((int) size);
    char *tmp_str;
    if (!(tmp_str= (char *) my_malloc(PSI_NOT_INSTRUMENTED, tmp_str_sz, MYF(MY_WME))))
      goto err;

    if (my_base64_encode(ptr, (size_t) size, tmp_str))
    {
      DBUG_ASSERT(0);
    }

    my_b_printf(file, "%s\n", tmp_str);
    my_free(tmp_str);
  }

#ifdef WHEN_FLASHBACK_REVIEW_READY
  if (print_event_info->verbose || print_event_info->print_row_count ||
      need_flashback_review)
#else
  // Flashback need the table_map to parse the event
  if (print_event_info->verbose || print_event_info->print_row_count ||
      is_flashback)
#endif
  {
    Rows_log_event *ev= NULL;
    Log_event_type et= (Log_event_type) ptr[EVENT_TYPE_OFFSET];

    if (checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF &&
        checksum_alg != BINLOG_CHECKSUM_ALG_OFF)
      size-= BINLOG_CHECKSUM_LEN; // checksum is displayed through the header

    switch (et)
    {
    case TABLE_MAP_EVENT:
    {
      Table_map_log_event *map; 
      map= new Table_map_log_event(ptr, size, 
                                   glob_description_event);
#ifdef WHEN_FLASHBACK_REVIEW_READY
      if (need_flashback_review)
      {
        map->set_review_dbname(m_review_dbname.ptr());
        map->set_review_tablename(m_review_tablename.ptr());
      }
#endif
      print_event_info->m_table_map.set_table(map->get_table_id(), map);
      break;
    }
    case WRITE_ROWS_EVENT:
    case WRITE_ROWS_EVENT_V1:
    {
      ev= new Write_rows_log_event(ptr, size,
                                   glob_description_event);
      break;
    }
    case DELETE_ROWS_EVENT:
    case DELETE_ROWS_EVENT_V1:
    {
      ev= new Delete_rows_log_event(ptr, size,
                                    glob_description_event);
      break;
    }
    case UPDATE_ROWS_EVENT:
    case UPDATE_ROWS_EVENT_V1:
    {
      ev= new Update_rows_log_event(ptr, size,
                                    glob_description_event);
      break;
    }
    case WRITE_ROWS_COMPRESSED_EVENT:
    case WRITE_ROWS_COMPRESSED_EVENT_V1:
    {
      ev= new Write_rows_compressed_log_event(ptr, size,
                                              glob_description_event);
      break;
    }
    case UPDATE_ROWS_COMPRESSED_EVENT:
    case UPDATE_ROWS_COMPRESSED_EVENT_V1:
    {
      ev= new Update_rows_compressed_log_event(ptr, size,
                                               glob_description_event);
      break;
      }
    case DELETE_ROWS_COMPRESSED_EVENT:
    case DELETE_ROWS_COMPRESSED_EVENT_V1:
    {
      ev= new Delete_rows_compressed_log_event(ptr, size,
                                               glob_description_event);
      break;
    }
    default:
      break;
    }

    if (ev)
    {
      bool error= 0;

#ifdef WHEN_FLASHBACK_REVIEW_READY
      ev->need_flashback_review= need_flashback_review;
      if (print_event_info->verbose)
      {
        if (ev->print_verbose(&print_event_info->tail_cache, print_event_info))
          goto err;
      }
      else
      {
        IO_CACHE tmp_cache;

        if (open_cached_file(&tmp_cache, NULL, NULL, 0,
                              MYF(MY_WME | MY_NABP)))
        {
          delete ev;
          goto err;
        }

        error= ev->print_verbose(&tmp_cache, print_event_info);
        close_cached_file(&tmp_cache);
        if (unlikely(error))
        {
          delete ev;
          goto err;
        }
      }
#else
      if (print_event_info->verbose)
        error= ev->print_verbose(&print_event_info->tail_cache, print_event_info);
      else
        ev->count_row_events(print_event_info);
#endif
      delete ev;
      if (unlikely(error))
        goto err;
    }
  }
  DBUG_RETURN(0);

err:
  DBUG_RETURN(1);
}


/*
  Log_event::print_timestamp()
*/

bool Log_event::print_timestamp(IO_CACHE* file, time_t* ts)
{
  struct tm *res;
  time_t my_when= when;
  DBUG_ENTER("Log_event::print_timestamp");
  if (!ts)
    ts = &my_when;
  res=localtime(ts);

  DBUG_RETURN(my_b_printf(file,"%02d%02d%02d %2d:%02d:%02d",
                          res->tm_year % 100,
                          res->tm_mon+1,
                          res->tm_mday,
                          res->tm_hour,
                          res->tm_min,
                          res->tm_sec));
}


/**
  Query_log_event::print().

  @todo
    print the catalog ??
*/
bool Query_log_event::print_query_header(IO_CACHE* file,
					 PRINT_EVENT_INFO* print_event_info)
{
  // TODO: print the catalog ??
  char buff[64], *end;				// Enough for SET TIMESTAMP
  bool different_db= 1;
  uint32 tmp;

  if (!print_event_info->short_form)
  {
    if (print_header(file, print_event_info, FALSE) ||
        my_b_printf(file,
                    "\t%s\tthread_id=%lu\texec_time=%lu\terror_code=%d"
                    "\txid=%lu\n",
                    get_type_str(), (ulong) thread_id, (ulong) exec_time,
                    error_code, (ulong) xid))
      goto err;
  }

  if ((flags & LOG_EVENT_SUPPRESS_USE_F))
  {
    if (!is_trans_keyword())
      print_event_info->db[0]= '\0';
  }
  else if (db)
  {
    different_db= memcmp(print_event_info->db, db, db_len + 1);
    if (different_db)
      memcpy(print_event_info->db, db, db_len + 1);
    if (db[0] && different_db) 
      if (my_b_printf(file, "use %`s%s\n", db, print_event_info->delimiter))
        goto err;
  }

  end=int10_to_str((long) when, strmov(buff,"SET TIMESTAMP="),10);
  if (when_sec_part && when_sec_part <= TIME_MAX_SECOND_PART)
  {
    *end++= '.';
    end=int10_to_str(when_sec_part, end, 10);
  }
  end= strmov(end, print_event_info->delimiter);
  *end++='\n';
  if (my_b_write(file, (uchar*) buff, (uint) (end-buff)))
    goto err;
  if ((!print_event_info->thread_id_printed ||
       ((flags & LOG_EVENT_THREAD_SPECIFIC_F) &&
        thread_id != print_event_info->thread_id)))
  {
    // If --short-form, print deterministic value instead of pseudo_thread_id.
    if (my_b_printf(file,"SET @@session.pseudo_thread_id=%lu%s\n",
                    short_form ? 999999999 : (ulong)thread_id,
                    print_event_info->delimiter))
      goto err;
    print_event_info->thread_id= thread_id;
    print_event_info->thread_id_printed= 1;
  }

  /*
    If flags2_inited==0, this is an event from 3.23 or 4.0 or a dummy
    event from the mtr test suite; nothing to print (remember we don't
    produce mixed relay logs so there cannot be 5.0 events before that
    one so there is nothing to reset).
  */
  if (likely(flags2_inited)) /* likely as this will mainly read 5.0 logs */
  {
    /* tmp is a bitmask of bits which have changed. */
    if (likely(print_event_info->flags2_inited)) 
      /* All bits which have changed */
      tmp= (print_event_info->flags2) ^ flags2;
    else /* that's the first Query event we read */
    {
      print_event_info->flags2_inited= 1;
      tmp= ~((uint32)0); /* all bits have changed */
    }

    if (unlikely(tmp)) /* some bits have changed */
    {
      bool need_comma= 0;
      if (my_b_write_string(file, "SET ") ||
          print_set_option(file, tmp, OPTION_NO_FOREIGN_KEY_CHECKS, ~flags2,
                           "@@session.foreign_key_checks", &need_comma)||
          print_set_option(file, tmp, OPTION_AUTO_IS_NULL, flags2,
                           "@@session.sql_auto_is_null", &need_comma) ||
          print_set_option(file, tmp, OPTION_RELAXED_UNIQUE_CHECKS, ~flags2,
                           "@@session.unique_checks", &need_comma) ||
          print_set_option(file, tmp, OPTION_NOT_AUTOCOMMIT, ~flags2,
                           "@@session.autocommit", &need_comma) ||
          print_set_option(file, tmp, OPTION_NO_CHECK_CONSTRAINT_CHECKS,
                           ~flags2,
                           "@@session.check_constraint_checks", &need_comma) ||
          print_set_option(file, tmp, OPTION_IF_EXISTS, flags2,
                           "@@session.sql_if_exists", &need_comma)||
          my_b_printf(file,"%s\n", print_event_info->delimiter))
        goto err;
      print_event_info->flags2= flags2;
    }
  }

  /*
    Now the session variables;
    it's more efficient to pass SQL_MODE as a number instead of a
    comma-separated list.
    FOREIGN_KEY_CHECKS, SQL_AUTO_IS_NULL, UNIQUE_CHECKS are session-only
    variables (they have no global version; they're not listed in
    sql_class.h), The tests below work for pure binlogs or pure relay
    logs. Won't work for mixed relay logs but we don't create mixed
    relay logs (that is, there is no relay log with a format change
    except within the 3 first events, which mysqlbinlog handles
    gracefully). So this code should always be good.
  */

  if (likely(sql_mode_inited) &&
      (unlikely(print_event_info->sql_mode != sql_mode ||
                !print_event_info->sql_mode_inited)))
  {
    char llbuff[22];
    if (my_b_printf(file,"SET @@session.sql_mode=%s%s\n",
                    ullstr(sql_mode, llbuff), print_event_info->delimiter))
      goto err;
    print_event_info->sql_mode= sql_mode;
    print_event_info->sql_mode_inited= 1;
  }
  if (print_event_info->auto_increment_increment != auto_increment_increment ||
      print_event_info->auto_increment_offset != auto_increment_offset)
  {
    if (my_b_printf(file,"SET @@session.auto_increment_increment=%lu, @@session.auto_increment_offset=%lu%s\n",
                    auto_increment_increment,auto_increment_offset,
                    print_event_info->delimiter))
      goto err;
    print_event_info->auto_increment_increment= auto_increment_increment;
    print_event_info->auto_increment_offset=    auto_increment_offset;
  }

  /* TODO: print the catalog when we feature SET CATALOG */

  if (likely(charset_inited) &&
      (unlikely(!print_event_info->charset_inited ||
                memcmp(print_event_info->charset, charset, 6))))
  {
    CHARSET_INFO *cs_info= get_charset(uint2korr(charset), MYF(MY_WME));
    if (cs_info)
    {
      /* for mysql client */
      if (my_b_printf(file, "/*!\\C %s */%s\n",
                      cs_info->cs_name.str, print_event_info->delimiter))
        goto err;
    }
    if (my_b_printf(file,"SET "
                    "@@session.character_set_client=%d,"
                    "@@session.collation_connection=%d,"
                    "@@session.collation_server=%d"
                    "%s\n",
                    uint2korr(charset),
                    uint2korr(charset+2),
                    uint2korr(charset+4),
                    print_event_info->delimiter))
      goto err;
    memcpy(print_event_info->charset, charset, 6);
    print_event_info->charset_inited= 1;
  }
  if (time_zone_len)
  {
    if (memcmp(print_event_info->time_zone_str,
               time_zone_str, time_zone_len+1))
    {
      if (my_b_printf(file,"SET @@session.time_zone='%s'%s\n",
                      time_zone_str, print_event_info->delimiter))
        goto err;
      memcpy(print_event_info->time_zone_str, time_zone_str, time_zone_len+1);
    }
  }
  if (lc_time_names_number != print_event_info->lc_time_names_number)
  {
    if (my_b_printf(file, "SET @@session.lc_time_names=%d%s\n",
                    lc_time_names_number, print_event_info->delimiter))
      goto err;
    print_event_info->lc_time_names_number= lc_time_names_number;
  }
  if (charset_database_number != print_event_info->charset_database_number)
  {
    if (charset_database_number)
    {
      if (my_b_printf(file, "SET @@session.collation_database=%d%s\n",
                      charset_database_number, print_event_info->delimiter))
        goto err;
    }
    else if (my_b_printf(file, "SET @@session.collation_database=DEFAULT%s\n",
                         print_event_info->delimiter))
      goto err;
    print_event_info->charset_database_number= charset_database_number;
  }
  return 0;

err:
  return 1;
}

bool Query_log_event::print_verbose(IO_CACHE* cache, PRINT_EVENT_INFO* print_event_info)
{
  if (my_b_printf(cache, "### ") ||
      my_b_write(cache, (uchar *) query, q_len) ||
      my_b_printf(cache, "\n"))
  {
    goto err;
  }
  return 0;

err:
  return 1;
}

bool Query_log_event::print(FILE* file, PRINT_EVENT_INFO* print_event_info)
{
  Write_on_release_cache cache(&print_event_info->head_cache, file, 0, this);

  /**
    reduce the size of io cache so that the write function is called
    for every call to my_b_write().
   */
  DBUG_EXECUTE_IF ("simulate_file_write_error",
                   {(&cache)->write_pos= (&cache)->write_end- 500;});
  if (print_query_header(&cache, print_event_info))
    goto err;
  if (!is_flashback)
  {
    if (gtid_flags_extra & (Gtid_log_event::FL_START_ALTER_E1 |
                            Gtid_log_event::FL_COMMIT_ALTER_E1 |
                            Gtid_log_event::FL_ROLLBACK_ALTER_E1))
    {
      bool do_print_encoded=
        print_event_info->base64_output_mode != BASE64_OUTPUT_NEVER &&
        print_event_info->base64_output_mode != BASE64_OUTPUT_DECODE_ROWS &&
        !print_event_info->short_form;
      bool comment_mode= do_print_encoded &&
        gtid_flags_extra & (Gtid_log_event::FL_START_ALTER_E1 |
                            Gtid_log_event::FL_ROLLBACK_ALTER_E1);

      if(comment_mode)
        my_b_printf(&cache, "/*!100600 ");
      if (do_print_encoded)
        my_b_printf(&cache, "BINLOG '\n");
      if (print_base64(&cache, print_event_info, do_print_encoded))
        goto err;
      if (do_print_encoded)
      {
        if(comment_mode)
           my_b_printf(&cache, "' */%s\n", print_event_info->delimiter);
        else
           my_b_printf(&cache, "'%s\n", print_event_info->delimiter);
      }
      if (print_event_info->verbose && print_verbose(&cache, print_event_info))
      {
        goto err;
      }
    }
    else
    {
      if (my_b_write(&cache, (uchar*) query, q_len) ||
          my_b_printf(&cache, "\n%s\n", print_event_info->delimiter))
        goto err;
    }
  }
  else // is_flashback == 1
  {
    if (strcmp("BEGIN", query) == 0)
    {
      if (my_b_write(&cache, (uchar*) "COMMIT", 6) ||
          my_b_printf(&cache, "\n%s\n", print_event_info->delimiter))
        goto err;
    }
    else if (strcmp("COMMIT", query) == 0)
    {
      if (my_b_printf(&cache, "START TRANSACTION\n%s\n", print_event_info->delimiter))
        goto err;
    }
  }
  return cache.flush_data();
err:
  return 1;
}


bool Start_log_event_v3::print(FILE* file, PRINT_EVENT_INFO* print_event_info)
{
  DBUG_ENTER("Start_log_event_v3::print");

  Write_on_release_cache cache(&print_event_info->head_cache, file,
                               Write_on_release_cache::FLUSH_F);

  if (!print_event_info->short_form)
  {
    if (print_header(&cache, print_event_info, FALSE) ||
        my_b_printf(&cache, "\tStart: binlog v %d, server v %s created ",
                    binlog_version, server_version) ||
        print_timestamp(&cache))
      goto err;
    if (created)
      if (my_b_printf(&cache," at startup"))
        goto err;
    if (my_b_printf(&cache, "\n"))
      goto err;
    if (flags & LOG_EVENT_BINLOG_IN_USE_F)
      if (my_b_printf(&cache,
                      "# Warning: this binlog is either in use or was not "
                      "closed properly.\n"))
        goto err;
  }
  if (!is_artificial_event() && created)
  {
#ifdef WHEN_WE_HAVE_THE_RESET_CONNECTION_SQL_COMMAND
    /*
      This is for mysqlbinlog: like in replication, we want to delete the stale
      tmp files left by an unclean shutdown of mysqld (temporary tables)
      and rollback unfinished transaction.
      Probably this can be done with RESET CONNECTION (syntax to be defined).
    */
    if (my_b_printf(&cache,"RESET CONNECTION%s\n",
                    print_event_info->delimiter))
      goto err;
#else
    if (my_b_printf(&cache,"ROLLBACK%s\n", print_event_info->delimiter))
      goto err;
#endif
  }
  if (temp_buf &&
      print_event_info->base64_output_mode != BASE64_OUTPUT_NEVER &&
      !print_event_info->short_form)
  {
    /* BINLOG is matched with the delimiter below on the same level */
    bool do_print_encoded=
      print_event_info->base64_output_mode != BASE64_OUTPUT_DECODE_ROWS;
    if (do_print_encoded)
      my_b_printf(&cache, "BINLOG '\n");

    if (print_base64(&cache, print_event_info, do_print_encoded))
      goto err;

    if (do_print_encoded)
      my_b_printf(&cache, "'%s\n", print_event_info->delimiter);

    print_event_info->printed_fd_event= TRUE;
  }
  DBUG_RETURN(cache.flush_data());
err:
  DBUG_RETURN(1);
}


bool Start_encryption_log_event::print(FILE* file,
                                       PRINT_EVENT_INFO* print_event_info)
{
    Write_on_release_cache cache(&print_event_info->head_cache, file);
    StringBuffer<1024> buf;
    buf.append(STRING_WITH_LEN("# Encryption scheme: "));
    buf.append_ulonglong(crypto_scheme);
    buf.append(STRING_WITH_LEN(", key_version: "));
    buf.append_ulonglong(key_version);
    buf.append(STRING_WITH_LEN(", nonce: "));
    buf.append_hex(nonce, BINLOG_NONCE_LENGTH);
    buf.append(STRING_WITH_LEN("\n# The rest of the binlog is encrypted!\n"));
    if (my_b_write(&cache, (uchar*)buf.ptr(), buf.length()))
      return 1;
    return (cache.flush_data());
}


bool Load_log_event::print(FILE* file, PRINT_EVENT_INFO* print_event_info)
{
  return print(file, print_event_info, 0);
}


bool Load_log_event::print(FILE* file_arg, PRINT_EVENT_INFO* print_event_info,
			   bool commented)
{
  Write_on_release_cache cache(&print_event_info->head_cache, file_arg);
  bool different_db= 1;
  DBUG_ENTER("Load_log_event::print");

  if (!print_event_info->short_form)
  {
    if (print_header(&cache, print_event_info, FALSE) ||
        my_b_printf(&cache, "\tQuery\tthread_id=%ld\texec_time=%ld\n",
                    thread_id, exec_time))
      goto err;
  }

  if (db)
  {
    /*
      If the database is different from the one of the previous statement, we
      need to print the "use" command, and we update the last_db.
      But if commented, the "use" is going to be commented so we should not
      update the last_db.
    */
    if ((different_db= memcmp(print_event_info->db, db, db_len + 1)) &&
        !commented)
      memcpy(print_event_info->db, db, db_len + 1);
  }

  if (db && db[0] && different_db)
    if (my_b_printf(&cache, "%suse %`s%s\n",
                    commented ? "# " : "",
                    db, print_event_info->delimiter))
      goto err;

  if (flags & LOG_EVENT_THREAD_SPECIFIC_F)
    if (my_b_printf(&cache,"%sSET @@session.pseudo_thread_id=%lu%s\n",
                    commented ? "# " : "", (ulong)thread_id,
                    print_event_info->delimiter))
      goto err;
  if (my_b_printf(&cache, "%sLOAD DATA ",
                  commented ? "# " : ""))
    goto err;
  if (check_fname_outside_temp_buf())
    if (my_b_write_string(&cache, "LOCAL "))
      goto err;
  if (my_b_printf(&cache, "INFILE '%-*s' ", fname_len, fname))
    goto err;

  if (sql_ex.opt_flags & REPLACE_FLAG)
  {
    if (my_b_write_string(&cache, "REPLACE "))
      goto err;
  }
  else if (sql_ex.opt_flags & IGNORE_FLAG)
    if (my_b_write_string(&cache, "IGNORE "))
      goto err;

  if (my_b_printf(&cache, "INTO TABLE `%s`", table_name) ||
      my_b_write_string(&cache, " FIELDS TERMINATED BY ") ||
      pretty_print_str(&cache, sql_ex.field_term, sql_ex.field_term_len))
    goto err;

  if (sql_ex.opt_flags & OPT_ENCLOSED_FLAG)
    if (my_b_write_string(&cache, " OPTIONALLY "))
      goto err;
  if (my_b_write_string(&cache, " ENCLOSED BY ") ||
      pretty_print_str(&cache, sql_ex.enclosed, sql_ex.enclosed_len) ||
      my_b_write_string(&cache, " ESCAPED BY ") ||
      pretty_print_str(&cache, sql_ex.escaped, sql_ex.escaped_len) ||
      my_b_write_string(&cache, " LINES TERMINATED BY ") ||
      pretty_print_str(&cache, sql_ex.line_term, sql_ex.line_term_len))
    goto err;

  if (sql_ex.line_start)
  {
    if (my_b_write_string(&cache," STARTING BY ") ||
        pretty_print_str(&cache, sql_ex.line_start, sql_ex.line_start_len))
      goto err;
  }
  if ((long) skip_lines > 0)
    if (my_b_printf(&cache, " IGNORE %ld LINES", (long) skip_lines))
      goto err;

  if (num_fields)
  {
    uint i;
    const char* field = fields;
    if (my_b_write_string(&cache, " ("))
      goto err;
    for (i = 0; i < num_fields; i++)
    {
      if (i)
        if (my_b_write_byte(&cache, ','))
          goto err;
      if (my_b_printf(&cache, "%`s", field))
        goto err;
      field += field_lens[i]  + 1;
    }
    if (my_b_write_byte(&cache, ')'))
      goto err;
  }

  if (my_b_printf(&cache, "%s\n", print_event_info->delimiter))
    goto err;
  DBUG_RETURN(cache.flush_data());
err:
  DBUG_RETURN(1);
}


bool Rotate_log_event::print(FILE* file, PRINT_EVENT_INFO* print_event_info)
{
  if (print_event_info->short_form)
    return 0;

  char buf[22];
  Write_on_release_cache cache(&print_event_info->head_cache, file,
                               Write_on_release_cache::FLUSH_F);
  if (print_header(&cache, print_event_info, FALSE) ||
      my_b_write_string(&cache, "\tRotate to "))
    goto err;
  if (new_log_ident)
    if (my_b_write(&cache, (uchar*) new_log_ident, (uint)ident_len))
      goto err;
  if (my_b_printf(&cache, "  pos: %s\n", llstr(pos, buf)))
    goto err;
  return cache.flush_data();
err:
  return 1;
}


bool Binlog_checkpoint_log_event::print(FILE *file,
                                        PRINT_EVENT_INFO *print_event_info)
{
  if (print_event_info->short_form)
    return 0;

  Write_on_release_cache cache(&print_event_info->head_cache, file,
                               Write_on_release_cache::FLUSH_F);

  if (print_header(&cache, print_event_info, FALSE) ||
      my_b_write_string(&cache, "\tBinlog checkpoint ") ||
      my_b_write(&cache, (uchar*)binlog_file_name, binlog_file_len) ||
      my_b_write_byte(&cache, '\n'))
    return 1;
  return cache.flush_data();
}


bool
Gtid_list_log_event::print(FILE *file, PRINT_EVENT_INFO *print_event_info)
{
  if (print_event_info->short_form)
    return 0;

  Write_on_release_cache cache(&print_event_info->head_cache, file,
                               Write_on_release_cache::FLUSH_F);
  char buf[21];
  uint32 i;

  qsort(list, count, sizeof(rpl_gtid), compare_glle_gtids);

  if (print_header(&cache, print_event_info, FALSE) ||
      my_b_printf(&cache, "\tGtid list ["))
    goto err;

  for (i= 0; i < count; ++i)
  {
    longlong10_to_str(list[i].seq_no, buf, 10);
    if (my_b_printf(&cache, "%u-%u-%s", list[i].domain_id,
                    list[i].server_id, buf))
      goto err;
    if (i < count-1)
      if (my_b_printf(&cache, ",\n# "))
        goto err;
  }
  if (my_b_printf(&cache, "]\n"))
    goto err;

  return cache.flush_data();
err:
  return 1;
}


bool Intvar_log_event::print(FILE* file, PRINT_EVENT_INFO* print_event_info)
{
  char llbuff[22];
  const char *UNINIT_VAR(msg);
  Write_on_release_cache cache(&print_event_info->head_cache, file,
                               Write_on_release_cache::FLUSH_F);

  if (!print_event_info->short_form)
  {
    if (print_header(&cache, print_event_info, FALSE) ||
        my_b_write_string(&cache, "\tIntvar\n"))
      goto err;
  }

  if (my_b_printf(&cache, "SET "))
    goto err;
  switch (type) {
  case LAST_INSERT_ID_EVENT:
    msg="LAST_INSERT_ID";
    break;
  case INSERT_ID_EVENT:
    msg="INSERT_ID";
    break;
  case INVALID_INT_EVENT:
  default: // cannot happen
    msg="INVALID_INT";
    break;
  }
  if (my_b_printf(&cache, "%s=%s%s\n",
                  msg, llstr(val,llbuff), print_event_info->delimiter))
    goto err;

  return cache.flush_data();
err:
  return 1;
}


bool Rand_log_event::print(FILE* file, PRINT_EVENT_INFO* print_event_info)
{
  Write_on_release_cache cache(&print_event_info->head_cache, file,
                               Write_on_release_cache::FLUSH_F);

  char llbuff[22],llbuff2[22];
  if (!print_event_info->short_form)
  {
    if (print_header(&cache, print_event_info, FALSE) ||
        my_b_write_string(&cache, "\tRand\n"))
      goto err;
  }
  if (my_b_printf(&cache, "SET @@RAND_SEED1=%s, @@RAND_SEED2=%s%s\n",
                  llstr(seed1, llbuff),llstr(seed2, llbuff2),
                  print_event_info->delimiter))
    goto err;

  return cache.flush_data();
err:
  return 1;
}


bool Xid_log_event::print(FILE* file, PRINT_EVENT_INFO* print_event_info)
{
  Write_on_release_cache cache(&print_event_info->head_cache, file,
                               Write_on_release_cache::FLUSH_F, this);

  if (!print_event_info->short_form)
  {
    char buf[64];
    longlong10_to_str(xid, buf, 10);

    if (print_header(&cache, print_event_info, FALSE) ||
        my_b_printf(&cache, "\tXid = %s\n", buf))
      goto err;
  }
  if (my_b_printf(&cache, is_flashback ? "START TRANSACTION%s\n" : "COMMIT%s\n",
                  print_event_info->delimiter))
    goto err;

  return cache.flush_data();
err:
  return 1;
}


bool User_var_log_event::print(FILE* file, PRINT_EVENT_INFO* print_event_info)
{
  Write_on_release_cache cache(&print_event_info->head_cache, file,
                               Write_on_release_cache::FLUSH_F);

  if (!print_event_info->short_form)
  {
    if (print_header(&cache, print_event_info, FALSE) ||
        my_b_write_string(&cache, "\tUser_var\n"))
      goto err;
  }

  if (my_b_write_string(&cache, "SET @") ||
      my_b_write_backtick_quote(&cache, name, name_len))
    goto err;

  if (is_null)
  {
    if (my_b_printf(&cache, ":=NULL%s\n", print_event_info->delimiter))
      goto err;
  }
  else
  {
    switch (type) {
    case REAL_RESULT:
      double real_val;
      char real_buf[FMT_G_BUFSIZE(14)];
      float8get(real_val, val);
      sprintf(real_buf, "%.14g", real_val);
      if (my_b_printf(&cache, ":=%s%s\n", real_buf,
                      print_event_info->delimiter))
        goto err;
      break;
    case INT_RESULT:
      char int_buf[22];
      longlong10_to_str(uint8korr(val), int_buf, 
                        ((flags & User_var_log_event::UNSIGNED_F) ? 10 : -10));
      if (my_b_printf(&cache, ":=%s%s\n", int_buf,
                      print_event_info->delimiter))
        goto err;
      break;
    case DECIMAL_RESULT:
    {
      char str_buf[200];
      int str_len= sizeof(str_buf) - 1;
      int precision= (int)val[0];
      int scale= (int)val[1];
      decimal_digit_t dec_buf[10];
      decimal_t dec;
      dec.len= 10;
      dec.buf= dec_buf;

      bin2decimal((uchar*) val+2, &dec, precision, scale);
      decimal2string(&dec, str_buf, &str_len, 0, 0, 0);
      str_buf[str_len]= 0;
      if (my_b_printf(&cache, ":=%s%s\n", str_buf,
                      print_event_info->delimiter))
        goto err;
      break;
    }
    case STRING_RESULT:
    {
      /*
        Let's express the string in hex. That's the most robust way. If we
        print it in character form instead, we need to escape it with
        character_set_client which we don't know (we will know it in 5.0, but
        in 4.1 we don't know it easily when we are printing
        User_var_log_event). Explanation why we would need to bother with
        character_set_client (quoting Bar):
        > Note, the parser doesn't switch to another unescaping mode after
        > it has met a character set introducer.
        > For example, if an SJIS client says something like:
        > SET @a= _ucs2 \0a\0b'
        > the string constant is still unescaped according to SJIS, not
        > according to UCS2.
      */
      char *hex_str;
      CHARSET_INFO *cs;
      bool error;

      // 2 hex digits / byte
      hex_str= (char *) my_malloc(PSI_NOT_INSTRUMENTED, 2 * val_len + 1 + 3, MYF(MY_WME));
      if (!hex_str)
        goto err;
      str_to_hex(hex_str, val, val_len);
      /*
        For proper behaviour when mysqlbinlog|mysql, we need to explicitly
        specify the variable's collation. It will however cause problems when
        people want to mysqlbinlog|mysql into another server not supporting the
        character set. But there's not much to do about this and it's unlikely.
      */
      if (!(cs= get_charset(charset_number, MYF(0))))
      {        /*
          Generate an unusable command (=> syntax error) is probably the best
          thing we can do here.
        */
        error= my_b_printf(&cache, ":=???%s\n", print_event_info->delimiter);
      }
      else
        error= my_b_printf(&cache, ":=_%s %s COLLATE `%s`%s\n",
                           cs->cs_name.str, hex_str, cs->coll_name.str,
                           print_event_info->delimiter);
      my_free(hex_str);
      if (unlikely(error))
        goto err;
      break;
    }
    case ROW_RESULT:
    default:
      DBUG_ASSERT(0);
      break;
    }
  }

  return cache.flush_data();
err:
  return 1;
}


#ifdef HAVE_REPLICATION

bool Unknown_log_event::print(FILE* file_arg, PRINT_EVENT_INFO* print_event_info)
{
  if (print_event_info->short_form)
    return 0;

  Write_on_release_cache cache(&print_event_info->head_cache, file_arg);

  if (what != ENCRYPTED)
  {
    if (print_header(&cache, print_event_info, FALSE) ||
        my_b_printf(&cache, "\n# Unknown event\n"))
      goto err;
  }
  else if (my_b_printf(&cache, "# Encrypted event\n"))
    goto err;

  return cache.flush_data();
err:
  return 1;
}


bool Stop_log_event::print(FILE* file, PRINT_EVENT_INFO* print_event_info)
{
  if (print_event_info->short_form)
    return 0;

  Write_on_release_cache cache(&print_event_info->head_cache, file,
                               Write_on_release_cache::FLUSH_F, this);

  if (print_header(&cache, print_event_info, FALSE) ||
      my_b_write_string(&cache, "\tStop\n"))
    return 1;
  return cache.flush_data();
}

#endif


bool Create_file_log_event::print(FILE* file,
                                  PRINT_EVENT_INFO* print_event_info,
				  bool enable_local)
{
  if (print_event_info->short_form)
  {
    if (enable_local && check_fname_outside_temp_buf())
      return Load_log_event::print(file, print_event_info);
    return 0;
  }

  Write_on_release_cache cache(&print_event_info->head_cache, file);

  if (enable_local)
  {
    if (Load_log_event::print(file, print_event_info,
                              !check_fname_outside_temp_buf()))
      goto err;

    /**
      reduce the size of io cache so that the write function is called
      for every call to my_b_printf().
     */
    DBUG_EXECUTE_IF ("simulate_create_event_write_error",
                     {(&cache)->write_pos= (&cache)->write_end;
                     DBUG_SET("+d,simulate_file_write_error");});
    /*
      That one is for "file_id: etc" below: in mysqlbinlog we want the #, in
      SHOW BINLOG EVENTS we don't.
     */
    if (my_b_write_byte(&cache, '#'))
      goto err;
  }

  if (my_b_printf(&cache, " file_id: %d  block_len: %d\n", file_id, block_len))
    goto err;

  return cache.flush_data();
err:
  return 1;

}


bool Create_file_log_event::print(FILE* file,
                                  PRINT_EVENT_INFO* print_event_info)
{
  return print(file, print_event_info, 0);
}


/*
  Append_block_log_event::print()
*/

bool Append_block_log_event::print(FILE* file,
				   PRINT_EVENT_INFO* print_event_info)
{
  if (print_event_info->short_form)
    return 0;

  Write_on_release_cache cache(&print_event_info->head_cache, file);

  if (print_header(&cache, print_event_info, FALSE) ||
      my_b_printf(&cache, "\n#%s: file_id: %d  block_len: %d\n",
                  get_type_str(), file_id, block_len))
    goto err;

  return cache.flush_data();
err:
  return 1;
}


/*
  Delete_file_log_event::print()
*/

bool Delete_file_log_event::print(FILE* file,
				  PRINT_EVENT_INFO* print_event_info)
{
  if (print_event_info->short_form)
    return 0;

  Write_on_release_cache cache(&print_event_info->head_cache, file);

  if (print_header(&cache, print_event_info, FALSE) ||
      my_b_printf(&cache, "\n#Delete_file: file_id=%u\n", file_id))
    return 1;

  return cache.flush_data();
}

/*
  Execute_load_log_event::print()
*/

bool Execute_load_log_event::print(FILE* file,
				   PRINT_EVENT_INFO* print_event_info)
{
  if (print_event_info->short_form)
    return 0;

  Write_on_release_cache cache(&print_event_info->head_cache, file);

  if (print_header(&cache, print_event_info, FALSE) ||
      my_b_printf(&cache, "\n#Exec_load: file_id=%d\n",
                  file_id))
    return 1;

  return cache.flush_data();
}

bool Execute_load_query_log_event::print(FILE* file,
                                         PRINT_EVENT_INFO* print_event_info)
{
  return print(file, print_event_info, 0);
}

/**
  Prints the query as LOAD DATA LOCAL and with rewritten filename.
*/
bool Execute_load_query_log_event::print(FILE* file,
                                         PRINT_EVENT_INFO* print_event_info,
                                         const char *local_fname)
{
  Write_on_release_cache cache(&print_event_info->head_cache, file);

  if (print_query_header(&cache, print_event_info))
    goto err;

  /**
    reduce the size of io cache so that the write function is called
    for every call to my_b_printf().
   */
  DBUG_EXECUTE_IF ("simulate_execute_event_write_error",
                   {(&cache)->write_pos= (&cache)->write_end;
                   DBUG_SET("+d,simulate_file_write_error");});

  if (local_fname)
  {
    if (my_b_write(&cache, (uchar*) query, fn_pos_start) ||
        my_b_write_string(&cache, " LOCAL INFILE ") ||
        pretty_print_str(&cache, local_fname, (int)strlen(local_fname)))
      goto err;

    if (dup_handling == LOAD_DUP_REPLACE)
      if (my_b_write_string(&cache, " REPLACE"))
        goto err;

    if (my_b_write_string(&cache, " INTO") ||
        my_b_write(&cache, (uchar*) query + fn_pos_end, q_len-fn_pos_end) ||
        my_b_printf(&cache, "\n%s\n", print_event_info->delimiter))
      goto err;
  }
  else
  {
    if (my_b_write(&cache, (uchar*) query, q_len) ||
        my_b_printf(&cache, "\n%s\n", print_event_info->delimiter))
      goto err;
  }

  if (!print_event_info->short_form)
    my_b_printf(&cache, "# file_id: %d \n", file_id);

  return cache.flush_data();
err:
  return 1;
}



const char str_binlog[]= "\nBINLOG '\n";
const char fmt_delim[]=   "'%s\n";
const char fmt_n_delim[]= "\n'%s";
const char fmt_frag[]= "\nSET @binlog_fragment_%d ='\n";
const char fmt_binlog2[]= "BINLOG @binlog_fragment_0, @binlog_fragment_1%s\n";

/**
  Print an event "body" cache to @c file possibly in two fragments.
  Each fragement is optionally per @c do_wrap to produce an SQL statement.

  @param file      a file to print to
  @param body      the "body" IO_CACHE of event
  @param do_wrap   whether to wrap base64-encoded strings with
                   SQL cover.
  @param delimiter delimiter string

  @param is_verbose  MDEV-10362 workraround parameter to pass
                   info on presence of verbose printout in cache encoded data

  The function signals on any error through setting @c body->error to -1.
*/
bool copy_cache_to_file_wrapped(IO_CACHE *body,
                                FILE *file,
                                bool do_wrap,
                                const char *delimiter,
                                bool is_verbose /*TODO: remove */)
{
  const my_off_t cache_size= my_b_tell(body);

  if (reinit_io_cache(body, READ_CACHE, 0L, FALSE, FALSE))
    goto err;

  if (!do_wrap)
  {
    my_b_copy_to_file(body, file, SIZE_T_MAX);
  }
  else if (4 + sizeof(str_binlog) + cache_size + sizeof(fmt_delim) >
           opt_binlog_rows_event_max_encoded_size)
  {
    /*
      2 fragments can always represent near 1GB row-based
      base64-encoded event as two strings each of size less than
      max(max_allowed_packet). Greater number of fragments does not
      save from potential need to tweak (increase) @@max_allowed_packet
      before to process the fragments. So 2 is safe and enough.

      Split the big query when its packet size's estimation exceeds a
      limit. The estimate includes the maximum packet header
      contribution of non-compressed packet.
    */
    my_fprintf(file, fmt_frag, 0);
    if (my_b_copy_to_file(body, file, (size_t) cache_size/2 + 1))
      goto err;
    my_fprintf(file, fmt_n_delim, delimiter);

    my_fprintf(file, fmt_frag, 1);
    if (my_b_copy_to_file(body, file, SIZE_T_MAX))
      goto err;
    my_fprintf(file, fmt_delim, delimiter);

    my_fprintf(file, fmt_binlog2, delimiter);
  }
  else
  {
    my_fprintf(file, str_binlog);
    if (my_b_copy_to_file(body, file, SIZE_T_MAX))
      goto err;
    my_fprintf(file, fmt_delim, delimiter);
  }
  reinit_io_cache(body, WRITE_CACHE, 0, FALSE, TRUE);

  return false;

err:
  body->error = -1;
  return true;
}


/**
  Print an event "body" cache to @c file possibly in two fragments.
  Each fragement is optionally per @c do_wrap to produce an SQL statement.

  @param file      a file to print to
  @param body      the "body" IO_CACHE of event
  @param do_wrap   whether to wrap base64-encoded strings with
                   SQL cover.
  @param delimiter delimiter string

  The function signals on any error through setting @c body->error to -1.
*/
bool copy_cache_to_string_wrapped(IO_CACHE *cache,
                                  LEX_STRING *to,
                                  bool do_wrap,
                                  const char *delimiter,
                                  bool is_verbose)
{
  const my_off_t cache_size= my_b_tell(cache);
  // contribution to total size estimate of formating
  const size_t fmt_size=
    sizeof(str_binlog) + 2*(sizeof(fmt_frag) + 2 /* %d */) +
    sizeof(fmt_delim)  + sizeof(fmt_n_delim)               +
    sizeof(fmt_binlog2) +
    3*PRINT_EVENT_INFO::max_delimiter_size;

  if (reinit_io_cache(cache, READ_CACHE, 0L, FALSE, FALSE))
    goto err;

  if (!(to->str= (char*) my_malloc(PSI_NOT_INSTRUMENTED, (size_t)cache->end_of_file + fmt_size,
                                   MYF(0))))
  {
    perror("Out of memory: can't allocate memory in "
           "copy_cache_to_string_wrapped().");
    goto err;
  }

  if (!do_wrap)
  {
    if (my_b_read(cache, (uchar*) to->str,
                  (to->length= (size_t)cache->end_of_file)))
      goto err;
  }
  else if (4 + sizeof(str_binlog) + cache_size + sizeof(fmt_delim) >
           opt_binlog_rows_event_max_encoded_size)
  {
    /*
      2 fragments can always represent near 1GB row-based
      base64-encoded event as two strings each of size less than
      max(max_allowed_packet). Greater number of fragments does not
      save from potential need to tweak (increase) @@max_allowed_packet
      before to process the fragments. So 2 is safe and enough.

      Split the big query when its packet size's estimation exceeds a
      limit. The estimate includes the maximum packet header
      contribution of non-compressed packet.
    */
    char *str= to->str;
    size_t add_to_len;

    str += (to->length= sprintf(str, fmt_frag, 0));
    if (my_b_read(cache, (uchar*) str, (uint32) (cache_size/2 + 1)))
      goto err;
    str += (add_to_len = (uint32) (cache_size/2 + 1));
    to->length += add_to_len;
    str += (add_to_len= sprintf(str, fmt_n_delim, delimiter));
    to->length += add_to_len;

    str += (add_to_len= sprintf(str, fmt_frag, 1));
    to->length += add_to_len;
    if (my_b_read(cache, (uchar*) str, uint32(cache->end_of_file - (cache_size/2 + 1))))
      goto err;
    str += (add_to_len= uint32(cache->end_of_file - (cache_size/2 + 1)));
    to->length += add_to_len;
    {
      str += (add_to_len= sprintf(str , fmt_delim, delimiter));
      to->length += add_to_len;
    }
    to->length += sprintf(str, fmt_binlog2, delimiter);
  }
  else
  {
    char *str= to->str;

    str += (to->length= sprintf(str, str_binlog));
    if (my_b_read(cache, (uchar*) str, (size_t)cache->end_of_file))
      goto err;
    str += cache->end_of_file;
    to->length += (size_t)cache->end_of_file;
      to->length += sprintf(str , fmt_delim, delimiter);
  }

  reinit_io_cache(cache, WRITE_CACHE, 0, FALSE, TRUE);

  return false;

err:
  cache->error= -1;
  return true;
}

/**
  The function invokes base64 encoder to run on the current
  event string and store the result into two caches.
  When the event ends the current statement the caches are is copied into
  the argument file.
  Copying is also concerned how to wrap the event, specifically to produce
  a valid SQL syntax.
  When the encoded data size is within max(MAX_ALLOWED_PACKET)
  a regular BINLOG query is composed. Otherwise it is build as fragmented

    SET @binlog_fragment_0='...';
    SET @binlog_fragment_1='...';
    BINLOG @binlog_fragment_0, @binlog_fragment_1;

  where fragments are represented by a pair of indexed user
  "one shot" variables.

  @note
  If any changes made don't forget to duplicate them to
  Old_rows_log_event as long as it's supported.

  @param file               pointer to IO_CACHE
  @param print_event_info   pointer to print_event_info specializing
                            what out of and how to print the event
  @param name               the name of a table that the event operates on

  The function signals on any error of cache access through setting
  that cache's @c error to -1.
*/
bool Rows_log_event::print_helper(FILE *file,
                                  PRINT_EVENT_INFO *print_event_info,
                                  char const *const name)
{
  IO_CACHE *const head= &print_event_info->head_cache;
  IO_CACHE *const body= &print_event_info->body_cache;
  IO_CACHE *const tail= &print_event_info->tail_cache;
#ifdef WHEN_FLASHBACK_REVIEW_READY
  IO_CACHE *const sql= &print_event_info->review_sql_cache;
#endif
  bool do_print_encoded=
    print_event_info->base64_output_mode != BASE64_OUTPUT_NEVER &&
    print_event_info->base64_output_mode != BASE64_OUTPUT_DECODE_ROWS &&
    !print_event_info->short_form;
  bool const last_stmt_event= get_flags(STMT_END_F);

  if (!print_event_info->short_form)
  {
    char llbuff[22];

    print_header(head, print_event_info, !last_stmt_event);
    if (my_b_printf(head, "\t%s: table id %s%s\n",
                    name, ullstr(m_table_id, llbuff),
                    last_stmt_event ? " flags: STMT_END_F" : ""))
      goto err;
  }
  if (!print_event_info->short_form || print_event_info->print_row_count)
    if (print_base64(body, print_event_info, do_print_encoded))
      goto err;

  if (last_stmt_event)
  {
    if (!is_flashback)
    {
      if (copy_event_cache_to_file_and_reinit(head, file) ||
          copy_cache_to_file_wrapped(body, file, do_print_encoded,
                                     print_event_info->delimiter,
                                     print_event_info->verbose) ||
          copy_event_cache_to_file_and_reinit(tail, file))
        goto err;
    }
    else
    {
    LEX_STRING tmp_str;

    if (copy_event_cache_to_string_and_reinit(head, &tmp_str))
      return 1;
    output_buf.append(tmp_str.str, tmp_str.length);  // Not \0 terminated);
    my_free(tmp_str.str);

    if (copy_cache_to_string_wrapped(body, &tmp_str, do_print_encoded,
                                     print_event_info->delimiter,
                                     print_event_info->verbose))
      return 1;
    output_buf.append(tmp_str.str, tmp_str.length);
    my_free(tmp_str.str);
    if (copy_event_cache_to_string_and_reinit(tail, &tmp_str))
      return 1;
    output_buf.append(tmp_str.str, tmp_str.length);
    my_free(tmp_str.str);

#ifdef WHEN_FLASHBACK_REVIEW_READY
    if (copy_event_cache_to_string_and_reinit(sql, &tmp_str))
      return 1;
    output_buf.append(tmp_str.str, tmp_str.length);
    my_free(tmp_str.str);
#endif
    }
  }

  return 0;
err:
  return 1;
}


bool Annotate_rows_log_event::print(FILE *file, PRINT_EVENT_INFO *pinfo)
{
  char *pbeg;   // beginning of the next line
  char *pend;   // end of the next line
  uint cnt= 0;  // characters counter

  if (!pinfo->short_form)
  {
    if (print_header(&pinfo->head_cache, pinfo, TRUE) ||
        my_b_printf(&pinfo->head_cache, "\tAnnotate_rows:\n"))
      goto err;
  }
  else if (my_b_printf(&pinfo->head_cache, "# Annotate_rows:\n"))
    goto err;

  for (pbeg= m_query_txt; ; pbeg= pend)
  {
    // skip all \r's and \n's at the beginning of the next line
    for (;; pbeg++)
    {
      if (++cnt > m_query_len)
        return 0;

      if (*pbeg != '\r' && *pbeg != '\n')
        break;
    }

    // find end of the next line
    for (pend= pbeg + 1;
         ++cnt <= m_query_len && *pend != '\r' && *pend != '\n';
         pend++)
      ;

    // print next line
    if (my_b_write(&pinfo->head_cache, (const uchar*) "#Q> ", 4) ||
        my_b_write(&pinfo->head_cache, (const uchar*) pbeg, pend - pbeg) ||
        my_b_write(&pinfo->head_cache, (const uchar*) "\n", 1))
      goto err;
  }

  return 0;
err:
  return 1;
}


/*
  Rewrite database name for the event to name specified by new_db
  SYNOPSIS
    new_db   Database name to change to
    new_len  Length
    desc     Event describing binlog that we're writing to.

  DESCRIPTION
    Reset db name. This function assumes that temp_buf member contains event
    representation taken from a binary log. It resets m_dbnam and m_dblen and
    rewrites temp_buf with new db name.

  RETURN 
    0     - Success
    other - Error
*/

int Table_map_log_event::rewrite_db(const char* new_db, size_t new_len,
                                    const Format_description_log_event* desc)
{
  DBUG_ENTER("Table_map_log_event::rewrite_db");
  DBUG_ASSERT(temp_buf);

  uint header_len= MY_MIN(desc->common_header_len,
                       LOG_EVENT_MINIMAL_HEADER_LEN) + TABLE_MAP_HEADER_LEN;
  int len_diff;

  if (!(len_diff= (int)(new_len - m_dblen)))
  {
    memcpy((void*) (temp_buf + header_len + 1), new_db, m_dblen + 1);
    memcpy((void*) m_dbnam, new_db, m_dblen + 1);
    DBUG_RETURN(0);
  }

  // Create new temp_buf
  ulong event_cur_len= uint4korr(temp_buf + EVENT_LEN_OFFSET);
  ulong event_new_len= event_cur_len + len_diff;
  uchar* new_temp_buf= (uchar*) my_malloc(PSI_NOT_INSTRUMENTED, event_new_len,
                                          MYF(MY_WME));

  if (!new_temp_buf)
  {
    sql_print_error("Table_map_log_event::rewrite_db: "
                    "failed to allocate new temp_buf (%d bytes required)",
                    event_new_len);
    DBUG_RETURN(-1);
  }

  // Rewrite temp_buf
  uchar *ptr= new_temp_buf;
  size_t cnt= 0;

  // Copy header and change event length
  memcpy(ptr, temp_buf, header_len);
  int4store(ptr + EVENT_LEN_OFFSET, event_new_len);
  ptr += header_len;
  cnt += header_len;

  // Write new db name length and new name
  DBUG_ASSERT(new_len < 0xff);
  *ptr++ = (char)new_len;
  memcpy(ptr, new_db, new_len + 1);
  ptr += new_len + 1;
  cnt += m_dblen + 2;

  // Copy rest part
  memcpy(ptr, temp_buf + cnt, event_cur_len - cnt);

  // Reregister temp buf
  free_temp_buf();
  register_temp_buf(new_temp_buf, TRUE);

  // Reset m_dbnam and m_dblen members
  m_dblen= new_len;

  // m_dbnam resides in m_memory together with m_tblnam and m_coltype
  uchar* memory= m_memory;
  char const* tblnam= m_tblnam;
  uchar* coltype= m_coltype;

  m_memory= (uchar*) my_multi_malloc(PSI_NOT_INSTRUMENTED, MYF(MY_WME),
                                     &m_dbnam, (uint) m_dblen + 1,
                                     &m_tblnam, (uint) m_tbllen + 1,
                                     &m_coltype, (uint) m_colcnt,
                                     NullS);

  if (!m_memory)
  {
    sql_print_error("Table_map_log_event::rewrite_db: "
                    "failed to allocate new m_memory (%d + %d + %d bytes required)",
                    m_dblen + 1, m_tbllen + 1, m_colcnt);
    DBUG_RETURN(-1);
  }

  memcpy((void*)m_dbnam, new_db, m_dblen + 1);
  memcpy((void*)m_tblnam, tblnam, m_tbllen + 1);
  memcpy(m_coltype, coltype, m_colcnt);

  my_free(memory);
  DBUG_RETURN(0);
}


bool Table_map_log_event::print(FILE *file, PRINT_EVENT_INFO *print_event_info)
{
  if (!print_event_info->short_form)
  {
    char llbuff[22];

    print_header(&print_event_info->head_cache, print_event_info, TRUE);
    if (my_b_printf(&print_event_info->head_cache,
                    "\tTable_map: %`s.%`s mapped to number %s%s\n",
                    m_dbnam, m_tblnam, ullstr(m_table_id, llbuff),
                    ((m_flags & TM_BIT_HAS_TRIGGERS_F) ?
                     " (has triggers)" : "")))
      goto err;
  }
  if (!print_event_info->short_form || print_event_info->print_row_count)
  {

    if (print_event_info->print_table_metadata)
    {
      Optional_metadata_fields fields(m_optional_metadata,
                                      m_optional_metadata_len);

      print_columns(&print_event_info->head_cache, fields);
      print_primary_key(&print_event_info->head_cache, fields);
    }
    bool do_print_encoded=
      print_event_info->base64_output_mode != BASE64_OUTPUT_NEVER &&
      print_event_info->base64_output_mode != BASE64_OUTPUT_DECODE_ROWS &&
      !print_event_info->short_form;

    if (print_base64(&print_event_info->body_cache, print_event_info,
                     do_print_encoded) ||
        copy_event_cache_to_file_and_reinit(&print_event_info->head_cache,
                                            file))
      goto err;
  }

  return 0;
err:
  return 1;
}

/**
  Interface for iterator over charset columns.
*/
class Table_map_log_event::Charset_iterator
{
 public:
  typedef Table_map_log_event::Optional_metadata_fields::Default_charset
      Default_charset;
  virtual const CHARSET_INFO *next()= 0;
  virtual ~Charset_iterator(){};
  /**
    Factory method to create an instance of the appropriate subclass.
  */
  static std::unique_ptr<Charset_iterator> create_charset_iterator(
      const Default_charset &default_charset,
      const std::vector<uint> &column_charset);
};

/**
  Implementation of charset iterator for the DEFAULT_CHARSET type.
*/
class Table_map_log_event::Default_charset_iterator : public Charset_iterator
{
 public:
  Default_charset_iterator(const Default_charset &default_charset)
      : m_iterator(default_charset.charset_pairs.begin()),
        m_end(default_charset.charset_pairs.end()),
        m_column_index(0),
        m_default_charset_info(
            get_charset(default_charset.default_charset, 0)) {}

  const CHARSET_INFO *next() override {
    const CHARSET_INFO *ret;
    if (m_iterator != m_end && m_iterator->first == m_column_index) {
      ret = get_charset(m_iterator->second, 0);
      m_iterator++;
    } else
      ret = m_default_charset_info;
    m_column_index++;
    return ret;
  }
  ~Default_charset_iterator(){};

 private:
  std::vector<Optional_metadata_fields::uint_pair>::const_iterator m_iterator,
      m_end;
  uint m_column_index;
  const CHARSET_INFO *m_default_charset_info;
};
//Table_map_log_event::Default_charset_iterator::~Default_charset_iterator(){int a=8;a++; a--;};
/**
  Implementation of charset iterator for the COLUMNT_CHARSET type.
*/
class Table_map_log_event::Column_charset_iterator : public Charset_iterator
{
 public:
  Column_charset_iterator(const std::vector<uint> &column_charset)
      : m_iterator(column_charset.begin()), m_end(column_charset.end()) {}

  const CHARSET_INFO *next() override {
    const CHARSET_INFO *ret = nullptr;
    if (m_iterator != m_end) {
      ret = get_charset(*m_iterator, 0);
      m_iterator++;
    }
    return ret;
  }

 ~Column_charset_iterator(){};
 private:
  std::vector<uint>::const_iterator m_iterator;
  std::vector<uint>::const_iterator m_end;
};
//Table_map_log_event::Column_charset_iterator::~Column_charset_iterator(){int a=8;a++; a--;};

std::unique_ptr<Table_map_log_event::Charset_iterator>
Table_map_log_event::Charset_iterator::create_charset_iterator(
    const Default_charset &default_charset,
    const std::vector<uint> &column_charset)
{
  if (!default_charset.empty())
    return std::unique_ptr<Charset_iterator>(
        new Default_charset_iterator(default_charset));
  else
    return std::unique_ptr<Charset_iterator>(
        new Column_charset_iterator(column_charset));
}
/**
   return the string name of a type.

   @param[in] type  type of a column
   @param[in|out] meta_ptr  the meta_ptr of the column. If the type doesn't have
                            metadata, it will not change  meta_ptr, otherwise
                            meta_ptr will be moved to the end of the column's
                            metadat.
   @param[in] cs charset of the column if it is a character column.
   @param[out] typestr  buffer to storing the string name of the type
   @param[in] typestr_length  length of typestr
   @param[in] geometry_type  internal geometry_type
 */
static void get_type_name(uint type, unsigned char** meta_ptr,
                          const CHARSET_INFO *cs, char *typestr,
                          uint typestr_length, unsigned int geometry_type)
{
  switch (type) {
  case MYSQL_TYPE_LONG:
    my_snprintf(typestr, typestr_length, "%s", "INT");
    break;
  case MYSQL_TYPE_TINY:
    my_snprintf(typestr, typestr_length, "TINYINT");
    break;
  case MYSQL_TYPE_SHORT:
    my_snprintf(typestr, typestr_length, "SMALLINT");
    break;
  case MYSQL_TYPE_INT24:
    my_snprintf(typestr, typestr_length, "MEDIUMINT");
    break;
  case MYSQL_TYPE_LONGLONG:
    my_snprintf(typestr, typestr_length, "BIGINT");
    break;
  case MYSQL_TYPE_NEWDECIMAL:
      my_snprintf(typestr, typestr_length, "DECIMAL(%d,%d)",
                  (*meta_ptr)[0], (*meta_ptr)[1]);
      (*meta_ptr)+= 2;
      break;
  case MYSQL_TYPE_FLOAT:
    my_snprintf(typestr, typestr_length, "FLOAT");
    (*meta_ptr)++;
    break;
  case MYSQL_TYPE_DOUBLE:
    my_snprintf(typestr, typestr_length, "DOUBLE");
    (*meta_ptr)++;
    break;
  case MYSQL_TYPE_BIT:
    my_snprintf(typestr, typestr_length, "BIT(%d)",
                (((*meta_ptr)[0])) + (*meta_ptr)[1]*8);
    (*meta_ptr)+= 2;
    break;
  case MYSQL_TYPE_TIMESTAMP2:
    if (**meta_ptr != 0)
      my_snprintf(typestr, typestr_length, "TIMESTAMP(%d)", **meta_ptr);
    else
      my_snprintf(typestr, typestr_length, "TIMESTAMP");
    (*meta_ptr)++;
    break;
  case MYSQL_TYPE_DATETIME2:
    if (**meta_ptr != 0)
      my_snprintf(typestr, typestr_length, "DATETIME(%d)", **meta_ptr);
    else
      my_snprintf(typestr, typestr_length, "DATETIME");
    (*meta_ptr)++;
    break;
  case MYSQL_TYPE_TIME2:
    if (**meta_ptr != 0)
      my_snprintf(typestr, typestr_length, "TIME(%d)", **meta_ptr);
    else
      my_snprintf(typestr, typestr_length, "TIME");
    (*meta_ptr)++;
    break;
  case MYSQL_TYPE_NEWDATE:
  case MYSQL_TYPE_DATE:
    my_snprintf(typestr, typestr_length, "DATE");
    break;
  case MYSQL_TYPE_YEAR:
    my_snprintf(typestr, typestr_length, "YEAR");
    break;
  case MYSQL_TYPE_ENUM:
    my_snprintf(typestr, typestr_length, "ENUM");
    (*meta_ptr)+= 2;
    break;
  case MYSQL_TYPE_SET:
    my_snprintf(typestr, typestr_length, "SET");
    (*meta_ptr)+= 2;
    break;
  case MYSQL_TYPE_BLOB:
    {
      bool is_text= (cs && cs->number != my_charset_bin.number);
      const char *names[5][2] = {
        {"INVALID_BLOB(%d)", "INVALID_TEXT(%d)"},
        {"TINYBLOB", "TINYTEXT"},
        {"BLOB", "TEXT"},
        {"MEDIUMBLOB", "MEDIUMTEXT"},
        {"LONGBLOB", "LONGTEXT"}
      };
      unsigned char size= **meta_ptr;

      if (size == 0 || size > 4)
        my_snprintf(typestr, typestr_length, names[0][is_text], size);
      else
        my_snprintf(typestr, typestr_length, names[**meta_ptr][is_text]);

      (*meta_ptr)++;
    }
    break;
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VAR_STRING:
    if (cs && cs->number != my_charset_bin.number)
      my_snprintf(typestr, typestr_length, "VARCHAR(%d)",
                  uint2korr(*meta_ptr)/cs->mbmaxlen);
    else
      my_snprintf(typestr, typestr_length, "VARBINARY(%d)",
                  uint2korr(*meta_ptr));

    (*meta_ptr)+= 2;
    break;
  case MYSQL_TYPE_STRING:
    {
      uint byte0= (*meta_ptr)[0];
      uint byte1= (*meta_ptr)[1];
      uint len= (((byte0 & 0x30) ^ 0x30) << 4) | byte1;

      if (cs && cs->number != my_charset_bin.number)
        my_snprintf(typestr, typestr_length, "CHAR(%d)", len/cs->mbmaxlen);
      else
        my_snprintf(typestr, typestr_length, "BINARY(%d)", len);

      (*meta_ptr)+= 2;
    }
    break;
  case MYSQL_TYPE_GEOMETRY:
    {
      const char* names[8] = {
        "GEOMETRY", "POINT", "LINESTRING", "POLYGON", "MULTIPOINT",
        "MULTILINESTRING", "MULTIPOLYGON", "GEOMETRYCOLLECTION"
      };
      if (geometry_type < 8)
        my_snprintf(typestr, typestr_length, names[geometry_type]);
      else
        my_snprintf(typestr, typestr_length, "INVALID_GEOMETRY_TYPE(%u)",
                    geometry_type);
      (*meta_ptr)++;
    }
    break;
  default:
    *typestr= 0;
    break;
  }
}

void Table_map_log_event::print_columns(IO_CACHE *file,
                                        const Optional_metadata_fields &fields)
{
  unsigned char* field_metadata_ptr= m_field_metadata;
  std::vector<bool>::const_iterator signedness_it= fields.m_signedness.begin();

  std::unique_ptr<Charset_iterator> charset_it =
      Charset_iterator::create_charset_iterator(fields.m_default_charset,
                                                fields.m_column_charset);
  std::unique_ptr<Charset_iterator> enum_and_set_charset_it =
      Charset_iterator::create_charset_iterator(
          fields.m_enum_and_set_default_charset,
          fields.m_enum_and_set_column_charset);
  std::vector<std::string>::const_iterator col_names_it=
    fields.m_column_name.begin();
  std::vector<Optional_metadata_fields::str_vector>::const_iterator
    set_str_values_it= fields.m_set_str_value.begin();
  std::vector<Optional_metadata_fields::str_vector>::const_iterator
    enum_str_values_it= fields.m_enum_str_value.begin();
  std::vector<unsigned int>::const_iterator geometry_type_it=
    fields.m_geometry_type.begin();

  uint geometry_type= 0;

  my_b_printf(file, "# Columns(");

  for (unsigned long i= 0; i < m_colcnt; i++)
  {
    uint real_type = m_coltype[i];
    if (real_type == MYSQL_TYPE_STRING &&
        (*field_metadata_ptr == MYSQL_TYPE_ENUM ||
         *field_metadata_ptr == MYSQL_TYPE_SET))
      real_type= *field_metadata_ptr;

    // Get current column's collation id if it is a character, enum,
    // or set column
    const CHARSET_INFO *cs = NULL;
    if (is_character_type(real_type))
      cs = charset_it->next();
    else if (is_enum_or_set_type(real_type))
      cs = enum_and_set_charset_it->next();

    // Print column name
    if (col_names_it != fields.m_column_name.end())
    {
      pretty_print_identifier(file, col_names_it->c_str(), col_names_it->size());
      my_b_printf(file, " ");
      col_names_it++;
    }


    // update geometry_type for geometry columns
    if (real_type == MYSQL_TYPE_GEOMETRY)
    {
      geometry_type= (geometry_type_it != fields.m_geometry_type.end()) ?
        *geometry_type_it++ : 0;
    }

    // print column type
    const uint TYPE_NAME_LEN = 100;
    char type_name[TYPE_NAME_LEN];
    get_type_name(real_type, &field_metadata_ptr, cs, type_name,
                  TYPE_NAME_LEN, geometry_type);

    if (type_name[0] == '\0')
    {
      my_b_printf(file, "INVALID_TYPE(%d)", real_type);
      continue;
    }
    my_b_printf(file, "%s", type_name);

    // Print UNSIGNED for numeric column
    if (is_numeric_type(real_type) &&
        signedness_it != fields.m_signedness.end())
    {
      if (*signedness_it == true)
        my_b_printf(file, " UNSIGNED");
      signedness_it++;
    }

    // if the column is not marked as 'null', print 'not null'
    if (!(m_null_bits[(i / 8)] & (1 << (i % 8))))
      my_b_printf(file, " NOT NULL");

    // Print string values of SET and ENUM column
    const Optional_metadata_fields::str_vector *str_values= NULL;
    if (real_type == MYSQL_TYPE_ENUM &&
        enum_str_values_it != fields.m_enum_str_value.end())
    {
      str_values= &(*enum_str_values_it);
      enum_str_values_it++;
    }
    else if (real_type == MYSQL_TYPE_SET &&
             set_str_values_it != fields.m_set_str_value.end())
    {
      str_values= &(*set_str_values_it);
      set_str_values_it++;
    }

    if (str_values != NULL)
    {
      const char *separator= "(";
      for (Optional_metadata_fields::str_vector::const_iterator it=
             str_values->begin(); it != str_values->end(); it++)
      {
        my_b_printf(file, "%s", separator);
        pretty_print_str(file, it->c_str(), it->size());
        separator= ",";
      }
      my_b_printf(file, ")");
    }
    // Print column character set, except in text columns with binary collation
    if (cs != NULL &&
        (is_enum_or_set_type(real_type) || cs->number != my_charset_bin.number))
      my_b_printf(file, " CHARSET %s COLLATE %s", cs->cs_name.str,
                  cs->coll_name.str);
    if (i != m_colcnt - 1) my_b_printf(file, ",\n#         ");
  }
  my_b_printf(file, ")");
  my_b_printf(file, "\n");
}

void Table_map_log_event::print_primary_key
  (IO_CACHE *file,const Optional_metadata_fields &fields)
{
  if (!fields.m_primary_key.empty())
  {
    my_b_printf(file, "# Primary Key(");

    std::vector<Optional_metadata_fields::uint_pair>::const_iterator it=
      fields.m_primary_key.begin();

    for (; it != fields.m_primary_key.end(); it++)
    {
      if (it != fields.m_primary_key.begin())
        my_b_printf(file, ", ");

      // Print column name or column index
      if (it->first >= fields.m_column_name.size())
        my_b_printf(file, "%u", it->first);
      else
        my_b_printf(file, "%s", fields.m_column_name[it->first].c_str());

      // Print prefix length
      if (it->second != 0)
        my_b_printf(file, "(%u)", it->second);
    }

    my_b_printf(file, ")\n");
  }
}


bool Write_rows_log_event::print(FILE *file, PRINT_EVENT_INFO* print_event_info)
{
  DBUG_EXECUTE_IF("simulate_cache_read_error",
                  {DBUG_SET("+d,simulate_my_b_fill_error");});
  return Rows_log_event::print_helper(file, print_event_info, is_flashback ? "Delete_rows" : "Write_rows");
}

bool Write_rows_compressed_log_event::print(FILE *file,
                                            PRINT_EVENT_INFO* print_event_info)
{
  uchar *new_buf;
  ulong len;
  bool is_malloc = false;
  if(!row_log_event_uncompress(glob_description_event,
                               checksum_alg == BINLOG_CHECKSUM_ALG_CRC32,
                               temp_buf, UINT_MAX32, NULL, 0, &is_malloc,
                               &new_buf, &len))
  {
    free_temp_buf();
    register_temp_buf(new_buf, true);
    if (Rows_log_event::print_helper(file, print_event_info,
                                     "Write_compressed_rows"))
      goto err;
  }
  else
  {
    if (my_b_printf(&print_event_info->head_cache,
                    "ERROR: uncompress write_compressed_rows failed\n"))
      goto err;
  }

  return 0;
err:
  return 1;
}


bool Delete_rows_log_event::print(FILE *file,
                                  PRINT_EVENT_INFO* print_event_info)
{
  return Rows_log_event::print_helper(file, print_event_info, is_flashback ? "Write_rows" : "Delete_rows");
}


bool Delete_rows_compressed_log_event::print(FILE *file,
                                             PRINT_EVENT_INFO* print_event_info)
{
  uchar *new_buf;
  ulong len;
  bool is_malloc = false;
  if(!row_log_event_uncompress(glob_description_event,
                               checksum_alg == BINLOG_CHECKSUM_ALG_CRC32,
                               temp_buf, UINT_MAX32, NULL, 0, &is_malloc,
                               &new_buf, &len))
  {
    free_temp_buf();
    register_temp_buf(new_buf, true);
    if (Rows_log_event::print_helper(file, print_event_info,
                                     "Delete_compressed_rows"))
      goto err;
  }
  else
  {
    if (my_b_printf(&print_event_info->head_cache,
                    "ERROR: uncompress delete_compressed_rows failed\n"))
      goto err;
  }

  return 0;
err:
  return 1;
}


bool Update_rows_log_event::print(FILE *file,
				  PRINT_EVENT_INFO* print_event_info)
{
  return Rows_log_event::print_helper(file, print_event_info, "Update_rows");
}

bool
Update_rows_compressed_log_event::print(FILE *file,
                                        PRINT_EVENT_INFO *print_event_info)
{
  uchar *new_buf;
  ulong len;
  bool is_malloc= false;
  if(!row_log_event_uncompress(glob_description_event,
                               checksum_alg == BINLOG_CHECKSUM_ALG_CRC32,
                               temp_buf, UINT_MAX32, NULL, 0, &is_malloc,
                               &new_buf, &len))
  {
    free_temp_buf();
    register_temp_buf(new_buf, true);
    if (Rows_log_event::print_helper(file, print_event_info,
                                     "Update_compressed_rows"))
      goto err;
  }
  else
  {
    if (my_b_printf(&print_event_info->head_cache,
                    "ERROR: uncompress update_compressed_rows failed\n"))
      goto err;
  }

  return 0;
err:
  return 1;
}


bool Incident_log_event::print(FILE *file,
                               PRINT_EVENT_INFO *print_event_info)
{
  if (print_event_info->short_form)
    return 0;

  Write_on_release_cache cache(&print_event_info->head_cache, file);

  if (print_header(&cache, print_event_info, FALSE) ||
      my_b_printf(&cache, "\n# Incident: %s\nRELOAD DATABASE; # Shall generate syntax error\n", description()))
    return 1;
  return cache.flush_data();
}


/* Print for its unrecognized ignorable event */
bool Ignorable_log_event::print(FILE *file,
                                PRINT_EVENT_INFO *print_event_info)
{
  if (print_event_info->short_form)
    return 0;

  if (print_header(&print_event_info->head_cache, print_event_info, FALSE) ||
      my_b_printf(&print_event_info->head_cache, "\tIgnorable\n") ||
      my_b_printf(&print_event_info->head_cache,
                  "# Ignorable event type %d (%s)\n", number, description) ||
      copy_event_cache_to_file_and_reinit(&print_event_info->head_cache,
                                          file))
    return 1;
  return 0;
}


/**
  The default values for these variables should be values that are
  *incorrect*, i.e., values that cannot occur in an event.  This way,
  they will always be printed for the first event.
*/
st_print_event_info::st_print_event_info()
{
  myf const flags = MYF(MY_WME | MY_NABP);
  /*
    Currently we only use static PRINT_EVENT_INFO objects, so zeroed at
    program's startup, but these explicit bzero() is for the day someone
    creates dynamic instances.
  */
  bzero(db, sizeof(db));
  bzero(charset, sizeof(charset));
  bzero(time_zone_str, sizeof(time_zone_str));
  delimiter[0]= ';';
  delimiter[1]= 0;
  flags2_inited= 0;
  flags2= 0;
  sql_mode_inited= 0;
  row_events= 0;
  sql_mode= 0;
  auto_increment_increment= 0;
  auto_increment_offset= 0;
  charset_inited= 0;
  lc_time_names_number= ~0;
  charset_database_number= ILLEGAL_CHARSET_INFO_NUMBER;
  thread_id= 0;
  server_id= 0;
  domain_id= 0;
  thread_id_printed= false;
  server_id_printed= false;
  domain_id_printed= false;
  allow_parallel= true;
  allow_parallel_printed= false;
  found_row_event= false;
  print_row_count= false;
  short_form= false;
  skip_replication= 0;
  printed_fd_event=FALSE;
  file= 0;
  base64_output_mode=BASE64_OUTPUT_UNSPEC;
  m_is_event_group_active= TRUE;
  m_is_event_group_filtering_enabled= FALSE;
  open_cached_file(&head_cache, NULL, NULL, 0, flags);
  open_cached_file(&body_cache, NULL, NULL, 0, flags);
  open_cached_file(&tail_cache, NULL, NULL, 0, flags);
#ifdef WHEN_FLASHBACK_REVIEW_READY
  open_cached_file(&review_sql_cache, NULL, NULL, 0, flags);
#endif
}


bool copy_event_cache_to_string_and_reinit(IO_CACHE *cache, LEX_STRING *to)
{
  reinit_io_cache(cache, READ_CACHE, 0L, FALSE, FALSE);
  if (cache->end_of_file > SIZE_T_MAX ||
      !(to->str= (char*) my_malloc(PSI_NOT_INSTRUMENTED, (to->length= (size_t)cache->end_of_file), MYF(0))))
  {
    perror("Out of memory: can't allocate memory in copy_event_cache_to_string_and_reinit().");
    goto err;
  }
  if (my_b_read(cache, (uchar*) to->str, to->length))
  {
    my_free(to->str);
    perror("Can't read data from IO_CACHE");
    return true;
  }
  reinit_io_cache(cache, WRITE_CACHE, 0, FALSE, TRUE);
  return false;

err:
  to->str= 0;
  to->length= 0;
  return true;
}


bool
Gtid_log_event::print(FILE *file, PRINT_EVENT_INFO *print_event_info)
{
  Write_on_release_cache cache(&print_event_info->head_cache, file,
                               Write_on_release_cache::FLUSH_F, this);
  char buf[21];
  char buf2[21];

  if (!print_event_info->short_form && !is_flashback)
  {
    print_header(&cache, print_event_info, FALSE);
    longlong10_to_str(seq_no, buf, 10);
    if (my_b_printf(&cache, "\tGTID %u-%u-%s", domain_id, server_id, buf))
      goto err;
    if (flags2 & FL_GROUP_COMMIT_ID)
    {
      longlong10_to_str(commit_id, buf2, 10);
      if (my_b_printf(&cache, " cid=%s", buf2))
        goto err;
    }
    if (flags2 & FL_DDL)
      if (my_b_write_string(&cache, " ddl"))
        goto err;
    if (flags2 & FL_TRANSACTIONAL)
      if (my_b_write_string(&cache, " trans"))
        goto err;
    if (flags2 & FL_WAITED)
      if (my_b_write_string(&cache, " waited"))
        goto err;
    if (flags_extra & FL_START_ALTER_E1)
      if (my_b_write_string(&cache, " START ALTER"))
        goto err;
    if (flags_extra & FL_COMMIT_ALTER_E1)
      if (my_b_printf(&cache, " COMMIT ALTER id= %lu", sa_seq_no))
        goto err;
    if (flags_extra & FL_ROLLBACK_ALTER_E1)
      if (my_b_printf(&cache, " ROLLBACK ALTER id= %lu", sa_seq_no))
        goto err;
    if (my_b_printf(&cache, "\n"))
      goto err;

    if (!print_event_info->allow_parallel_printed ||
        print_event_info->allow_parallel != !!(flags2 & FL_ALLOW_PARALLEL))
    {
      if (my_b_printf(&cache,
                  "/*!100101 SET @@session.skip_parallel_replication=%u*/%s\n",
                      !(flags2 & FL_ALLOW_PARALLEL),
                      print_event_info->delimiter))
        goto err;
      print_event_info->allow_parallel= !!(flags2 & FL_ALLOW_PARALLEL);
      print_event_info->allow_parallel_printed= true;
    }

    if (!print_event_info->domain_id_printed ||
        print_event_info->domain_id != domain_id)
    {
      if (my_b_printf(&cache,
                      "/*!100001 SET @@session.gtid_domain_id=%u*/%s\n",
                      domain_id, print_event_info->delimiter))
        goto err;
      print_event_info->domain_id= domain_id;
      print_event_info->domain_id_printed= true;
    }

    if (!print_event_info->server_id_printed ||
        print_event_info->server_id != server_id)
    {
      if (my_b_printf(&cache, "/*!100001 SET @@session.server_id=%u*/%s\n",
                      server_id, print_event_info->delimiter))
        goto err;
      print_event_info->server_id= server_id;
      print_event_info->server_id_printed= true;
    }

    if (!is_flashback)
      if (my_b_printf(&cache, "/*!100001 SET @@session.gtid_seq_no=%s*/%s\n",
                      buf, print_event_info->delimiter))
        goto err;
  }
  if ((flags2 & FL_PREPARED_XA) && !is_flashback)
  {
    my_b_write_string(&cache, "XA START ");
    xid.serialize();
    my_b_write(&cache, (uchar*) xid.buf, strlen(xid.buf));
    if (my_b_printf(&cache, "%s\n", print_event_info->delimiter))
      goto err;
  }
  else if (!(flags2 & FL_STANDALONE))
  {
    if (my_b_printf(&cache, is_flashback ? "COMMIT\n%s\n" :
                    "START TRANSACTION\n%s\n", print_event_info->delimiter))
      goto err;
  }

  return cache.flush_data();
err:
  return 1;
}

bool XA_prepare_log_event::print(FILE* file, PRINT_EVENT_INFO* print_event_info)
{
  Write_on_release_cache cache(&print_event_info->head_cache, file,
                               Write_on_release_cache::FLUSH_F, this);
  m_xid.serialize();

  if (!print_event_info->short_form)
  {
    print_header(&cache, print_event_info, FALSE);
    if (my_b_printf(&cache, "\tXID = %s\n", m_xid.buf))
      goto error;
  }

  if (my_b_printf(&cache, "XA PREPARE %s\n%s\n",
                   m_xid.buf, print_event_info->delimiter))
    goto error;

  return cache.flush_data();
error:
  return TRUE;
}
