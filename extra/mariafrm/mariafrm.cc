/*
   Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2010, 2019, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

/* Utility to parse frm files and print their DDL */

#define MARIAFRM_VERSION "1.0"

#include <my_global.h>
#include <my_sys.h>
#include <my_base.h>
#include <m_ctype.h>
#include <m_string.h>
#include <mysql_com.h>
#include <decimal.h>
#include <my_time.h>
#include <myisampack.h>
#include <compat56.h>

#include <my_getopt.h>
#include <welcome_copyright_notice.h> /* ORACLE_WELCOME_COPYRIGHT_NOTICE */

#include "mariafrm.h"

static uint opt_verbose= 0;

static struct my_option my_long_options[]= {
    {"help", '?', "Display this help and exit.", 0, 0, 0, GET_NO_ARG, NO_ARG,
     0, 0, 0, 0, 0, 0},
    {"verbose", 'v',
     "More verbose output; you can use this multiple times to get even more "
     "verbose output.",
     0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
    {"version", 'V', "Output version information and exit.", 0, 0, 0,
     GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}};

static void print_version(void)
{
  printf("%s  Ver %s Distrib %s, for %s (%s)\n", my_progname, MARIAFRM_VERSION,
         MYSQL_SERVER_VERSION, SYSTEM_TYPE, MACHINE_TYPE);
}

static void usage(void)
{
  print_version();
  puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2000"));
  puts("Generates the table DDL by parsing the FRM file. \n");
  printf("Usage: %s [OPTIONS] [FILE] [DIRECTORY]\n", my_progname);
  puts("");
  my_print_help(my_long_options);
}

static my_bool get_one_option(const struct my_option *opt,
                              const char *argument, const char *filename)
{
  switch (opt->id)
  {
  case 'v':
    opt_verbose++;
    break;
  case 'V':
    print_version();
    exit(0);
    break;
  case '?':
    usage();
    exit(0);
  }
  return 0;
}

static void get_options(int *argc, char ***argv)
{
  int ho_error;
  if ((ho_error= handle_options(argc, argv, my_long_options, get_one_option)))
    exit(ho_error);
  return;
}

static int read_file(const char *path, const uchar **frm, size_t *len)
{
  File file;
  uchar *read_data;
  size_t read_len= 0;
  *frm= NULL;
  *len= 0;
  int error= 1;
  MY_STAT stat;
  if (!my_stat(path, &stat, MYF(MY_WME)))
    goto err_end;
  read_len= stat.st_size;
  error= 2;
  if (!MY_S_ISREG(stat.st_mode))
    goto err_end;
  error= 3;
  if ((file= my_open(path, O_RDONLY, MYF(MY_WME))) < 0)
    goto err_end;
  error= 4;
  if (!(read_data=
            (uchar *) my_malloc(PSI_NOT_INSTRUMENTED, read_len, MYF(MY_FAE))))
    goto err;
  error= 5;
  if (my_read(file, read_data, read_len, MYF(MY_NABP)))
  {
    my_free(read_data);
    goto err;
  }
  error= 0;
  *frm= read_data;
  *len= read_len;
err:
  my_close(file, MYF(MY_WME));
err_end:
  return error;
}

static int get_tablename(const char *filename, char **tablename,
                  size_t *tablename_len)
{
  const char *basename= my_basename(filename);
  int i= 0;
  while (basename[i] != '\0' && basename[i] != '.')
    i++;
  char *ts= (char *) my_safe_alloca(i);
  memcpy(ts, basename, i);
  char *name_buff;
  CHARSET_INFO *system_charset_info= &my_charset_utf8mb3_general_ci;
  uint errors;
  size_t res;
  int error= 1;
  if (!(name_buff=
            (char *) my_malloc(PSI_NOT_INSTRUMENTED, FN_LEN, MYF(MY_FAE))))
    goto err_end;
  error= 2;
  res= my_convert(name_buff, FN_LEN, system_charset_info, ts, i,
                  &my_charset_filename, &errors);
  name_buff[res]= 0;
  if (unlikely(errors))
    goto err_end;
  error= 0;
  *tablename= name_buff;
  *tablename_len= res;
err_end:
  my_safe_afree(ts, i);
  return error;
}

static int get_charset(frm_file_data *ffd, uint cs_number)
{
  CHARSET_INFO *c= get_charset(cs_number, MYF(0));
  ffd->table_cs_name= c->cs_name;
  ffd->table_coll_name= c->coll_name;
  ffd->charset_primary_number= c->primary_number;
  return 0;
}

static char *rtrim(char *s)
{
  char *back= s + strlen(s);
  while (*--back == ' ')
    ;
  *(back + 1)= '\0';
  return s;
}

static void set_integer_default(column *col, char *buffer, char **ts, size_t len)
{
  char *t= c_malloc(len + 1);
  memcpy(t, buffer, len + 1);
  col->default_value= {t, len};
  *ts= t;
}

static char *copy_string(const uchar *src, size_t len)
{
  char *dest= c_malloc(len + 1);
  memcpy(dest, src, len);
  dest[len]= 0;
  return dest;
}

static void change_default_value_charset(column *col)
{
  CHARSET_INFO *c= get_charset(col->charset_id, MYF(0));
  if (strstr(c->cs_name.str, MY_UTF8MB3) ||
      strstr(c->cs_name.str, MY_UTF8MB4) || strstr(c->cs_name.str, "latin1"))
    return;
  uint max_len= (uint)col->default_value.length * 3;
  char *ts=c_malloc(max_len + 1);
  uint errors= 0;
  char *tts= copy_string((const uchar *) col->default_value.str,
                         col->default_value.length + 1);
  size_t len= my_convert(ts, max_len, &my_charset_utf8mb3_general_ci, tts,
                         (uint)col->default_value.length, c, &errors);
  ts[len]= 0;
  ts= (char*) my_realloc(PSI_NOT_INSTRUMENTED, ts, len + 1, MYF(0));
  if (!errors)
  {
    col->default_value.str= ts;
    col->default_value.length= len;
  }
}

static void prepend_zeroes(column *col, char *ts) 
{
  int diff;
  if ((diff= (int) (col->length - col->default_value.length)) > 0)
  {
    ts= (char*)my_realloc(PSI_NOT_INSTRUMENTED, ts, col->length + 1, MYF(0));
    bmove_upp((uchar *) ts + col->length + 1,
              (uchar *) ts + col->default_value.length + 1,
              col->default_value.length + 1);
    bfill((uchar *) ts, diff, '0');
    col->default_value.length= col->length;
    col->default_value.str= ts;
  }
}

static bool is_numeric_type(enum_field_types ftype)
{ 
  switch(ftype)
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
    return true;
  default: return false;
  }
  return false;
}

static size_t Inet4_to_string(const uchar *s, char *dst, size_t dstsize)
{
  return (size_t) my_snprintf(dst, dstsize, "%d.%d.%d.%d", s[0], s[1],
                              (uchar) s[2], s[3]);
}

static size_t Inet6_to_string(const uchar *s, char *dst, size_t dstsize)
{
  struct Region
  {
    int pos;
    int length;
  };
  char *dstend= dst + dstsize;
  const uchar *ipv6_bytes= (const uchar *) s;
  uint16 ipv6_words[IN6_ADDR_NUM_WORDS];
  for (size_t i= 0; i < IN6_ADDR_NUM_WORDS; ++i)
    ipv6_words[i]= (ipv6_bytes[2 * i] << 8) + ipv6_bytes[2 * i + 1];
  Region gap= {-1, -1};
  {
    Region rg= {-1, -1};
    for (size_t i= 0; i < IN6_ADDR_NUM_WORDS; ++i)
    {
      if (ipv6_words[i] != 0)
      {
        if (rg.pos >= 0)
        {
          if (rg.length > gap.length)
            gap= rg;

          rg.pos= -1;
          rg.length= -1;
        }
      }
      else
      {
        if (rg.pos >= 0)
        {
          ++rg.length;
        }
        else
        {
          rg.pos= (int) i;
          rg.length= 1;
        }
      }
    }
    if (rg.pos >= 0)
    {
      if (rg.length > gap.length)
        gap= rg;
    }
  }
  char *p= dst;
  for (int i= 0; i < (int) IN6_ADDR_NUM_WORDS; ++i)
  {
    size_t dstsize_available= dstend - p;
    if (dstsize_available < 5)
      break;
    if (i == gap.pos)
    {
      if (i == 0)
      {
        *p= ':';
        ++p;
      }
      *p= ':';
      ++p;

      i+= gap.length - 1;
    }
    else if (i == 6 && gap.pos == 0 &&
             (gap.length == 6 ||                           // IPv4-compatible
              (gap.length == 5 && ipv6_words[5] == 0xffff) // IPv4-mapped
              ))
    {
      return (size_t) (p - dst) +
             Inet4_to_string(s + 12, p, dstsize_available);
    }
    else
    {
      p+= sprintf(p, "%x", ipv6_words[i]);
      if (i + 1 != IN6_ADDR_NUM_WORDS)
      {
        *p= ':';
        ++p;
      }
    }
  }
  *p= 0;
  return (size_t) (p - dst);
}

static void read_default_value(frm_file_data *ffd, const uchar *frm, column *col)
{
  enum_field_types ftype= col->type;
  uint offset= ffd->defaults_offset + col->defaults_offset;
  //printf("%s  %d\t%u\t%x\n", col->name.str, ftype, offset, offset);
  //printf("column length: %u\n", col->length);
  char buffer[100];
  char *tts=0;
  char *ts=0;
  size_t len= 0;
  switch (ftype)
  {
  case MYSQL_TYPE_TINY:
    len= f_is_dec(col->flags) ? sprintf(buffer, "%d", (int) frm[offset])
                              : sprintf(buffer, "%u", (uint) frm[offset]);
    set_integer_default(col, buffer, (char **) &ts, len);
    break;
  case MYSQL_TYPE_SHORT:
    len= f_is_dec(col->flags) ? sprintf(buffer, "%d", sint2korr(frm + offset))
                              : sprintf(buffer, "%u", uint2korr(frm + offset));
    set_integer_default(col, buffer, (char **) &ts, len);
    break;
  case MYSQL_TYPE_INT24:
    len= f_is_dec(col->flags) ? sprintf(buffer, "%d", sint3korr(frm + offset))
                              : sprintf(buffer, "%u", uint3korr(frm + offset));
    set_integer_default(col, buffer, (char **) &ts, len);
    break;
  case MYSQL_TYPE_LONG:
    len= f_is_dec(col->flags) ? sprintf(buffer, "%d", sint4korr(frm + offset))
                              : sprintf(buffer, "%u", uint4korr(frm + offset));
    set_integer_default(col, buffer, (char **) &ts, len);
    break;
  case MYSQL_TYPE_LONGLONG:
    len= f_is_dec(col->flags)
             ? sprintf(buffer, "%lld", sint8korr(frm + offset))
             : sprintf(buffer, "%llu", uint8korr(frm + offset));
    set_integer_default(col, buffer, (char **) &ts, len);
    break;
  case MYSQL_TYPE_VARCHAR:
    if (col->length < 256)
      len= frm[offset++];
    else
    {
      len= uint2korr(frm + offset);
      offset+= 2;
    }
    ts= copy_string(frm + offset, len);
    col->default_value= {ts, len};
    break;
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_STRING: {
    if (col->extra_data_type_info.length)
    {
      if (strstr((char*)col->extra_data_type_info.str, "inet6"))
      {
        ts= c_malloc(IN6_ADDR_MAX_CHAR_LENGTH);
        len= Inet6_to_string(frm + offset, ts, IN6_ADDR_MAX_CHAR_LENGTH);
      }
      else if (strstr((char*)col->extra_data_type_info.str, "inet4"))
      {
        ts= c_malloc(IN_ADDR_MAX_CHAR_LENGTH);
        len= Inet4_to_string(frm + offset, ts, IN_ADDR_MAX_CHAR_LENGTH);
      }
    }
    else
    {
      ts= copy_string(frm + offset, col->length);
      rtrim(ts);
      len= strlen(ts);
    }
    col->default_value= {ts, len};
    break;
  }
  case MYSQL_TYPE_DECIMAL:
    len= col->length;
    ts= copy_string(frm + offset, len);
    col->default_value= {ts, len};
    break;
  case MYSQL_TYPE_NEWDECIMAL: {
    decimal_digits_t precision= (decimal_digits_t) col->length;
    decimal_digits_t scale= (decimal_digits_t) f_decimals(col->flags);
    if (scale)
      precision--;
    if (precision)
      precision--;
    if (!f_is_dec(col->flags))
      precision++;

    decimal_digit_t dec_buf[DECIMAL_MAX_PRECISION];
    decimal_t *dec= new decimal_t();
    dec->len= precision;
    dec->buf= dec_buf;

    bin2decimal(frm + offset, dec, precision, scale);
    ts= c_malloc(DECIMAL_MAX_STR_LENGTH);
    int *ts_len= new int();
    *ts_len= DECIMAL_MAX_STR_LENGTH;
    decimal2string(dec, ts, ts_len, 0, 0, ' ');
    len= strlen(ts);
    col->default_value= {ts, len};
    break;
  }
  case MYSQL_TYPE_FLOAT: {
    float f;
    float4get(f, frm + offset);
    uint scale= f_decimals(col->flags);
    ts= c_malloc(70);
    if (scale >= FLOATING_POINT_DECIMALS)
      len= my_gcvt(f, MY_GCVT_ARG_FLOAT, 69, ts, NULL);
    else
      len= my_fcvt(f, (int) scale, ts, NULL);
    col->default_value= {ts, len};
    break;
  }
  case MYSQL_TYPE_DOUBLE: {
    double d;
    float8get(d, frm + offset);
    uint scale= f_decimals(col->flags);
    ts= c_malloc(FLOATING_POINT_BUFFER);
    if (scale >= FLOATING_POINT_DECIMALS)
      len= my_gcvt(d, MY_GCVT_ARG_DOUBLE, FLOATING_POINT_BUFFER - 1, ts, NULL);
    else
      len= my_fcvt(d, scale, ts, NULL);
    col->default_value= {ts, len};
    break;
  }
  case MYSQL_TYPE_BIT: {
    uint nbytes= (col->length + 7) / 8;
    ulonglong bits= 0;
    switch (nbytes)
    {
    case 1:
      bits= (ulonglong) frm[offset];
      break;
    case 2:
      bits= mi_uint2korr(frm + offset);
      break;
    case 3:
      bits= mi_uint3korr(frm + offset);
      break;
    case 4:
      bits= mi_uint4korr(frm + offset);
      break;
    case 5:
      bits= mi_uint5korr(frm + offset);
      break;
    case 6:
      bits= mi_uint6korr(frm + offset);
      break;
    case 7:
      bits= mi_uint7korr(frm + offset);
      break;
    default:
      bits= mi_uint8korr(frm + offset);
      break;
    }
    char buff[65];
    ll2str((longlong) bits, buff, 2, 0);
    size_t buff_len= strlen(buff);
    len= buff_len + 3;
    ts= c_malloc(len + 1);
    memcpy(ts, "b'", 2);
    memcpy(ts + 2, buff, len);
    memcpy(ts + len - 1, "'\0", 2);
    col->default_value= {ts, len};
    break;
  }
  case MYSQL_TYPE_ENUM: {
    uint in= frm[offset] - 1;
    if (ffd->labels[col->label_id].names.size() >= 256)
      in= uint2korr(frm + offset) - 1;
    LEX_CSTRING ls= ffd->labels[col->label_id].names.at(in);
    len= ls.length;
    ts= c_malloc(len + 1);
    memcpy(ts, ls.str, ls.length);
    ts[len]= 0;
    col->default_value= {ts, len};
    break;
  }
  case MYSQL_TYPE_SET: {
    size_t size= ffd->labels[col->label_id].names.size();
    uint nbytes= (uint) (size + 7) / 8;
    if (nbytes > 4)
      nbytes= 8;
    ulonglong value= 0;
    switch (nbytes)
    {
    case 1:
      value= frm[offset];
      break;
    case 2:
      value= uint2korr(frm + offset);
      break;
    case 3:
      value= uint3korr(frm + offset);
      break;
    case 4:
      value= uint4korr(frm + offset);
      break;
    case 8:
      value= uint8korr(frm + offset);
      break;
    }
    uint capacity= 100;
    tts= c_malloc(capacity);
    uint current_size= 0;
    bool at_least_once= false;
    for (ulonglong i= 0; i < size; i++)
    {
      if (!(value & (1ULL << i)))
        continue;
      LEX_CSTRING ls= ffd->labels[col->label_id].names.at(i);
      if (current_size + 1 + ls.length >= capacity)
      {
        capacity+= (uint) (100 + ls.length);
        tts= (char *) my_realloc(PSI_NOT_INSTRUMENTED, tts, capacity, MYF(0));
      }
      if (at_least_once)
        tts[current_size++]= ',';
      memcpy(tts + current_size, ls.str, ls.length);
      current_size+= (uint) ls.length;
      at_least_once= true;
    }
    len= current_size;
    ts= c_malloc(len + 1);
    memcpy(ts, tts, current_size);
    ts[len]= 0;
    my_free(tts);
    col->default_value= {ts, len};
    break;
  }
  case MYSQL_TYPE_DATE: {
    MYSQL_TIME ltime;
    int32 tmp= sint4korr(frm + offset);
    ltime.year= (int) ((uint32) tmp / 10000L % 10000);
    ltime.month= (int) ((uint32) tmp / 100 % 100);
    ltime.day= (int) ((uint32) tmp % 100);
    ltime.time_type= MYSQL_TIMESTAMP_DATE;
    ltime.hour= ltime.minute= ltime.second= ltime.second_part= ltime.neg= 0;
    ts= c_malloc(MAX_DATE_STRING_REP_LENGTH);
    len= my_date_to_str(&ltime, ts);
    col->default_value= {ts, len};
    break;
  }
  case MYSQL_TYPE_NEWDATE: {
    ts= c_malloc(col->length + 1);
    len= col->length;
    uint32 tmp= (uint32) uint3korr(frm + offset);
    int part;
    char *pos= (char *) ts + 10;

    *pos--= 0; // End NULL
    part= (int) (tmp & 31);
    *pos--= (char) ('0' + part % 10);
    *pos--= (char) ('0' + part / 10);
    *pos--= '-';
    part= (int) (tmp >> 5 & 15);
    *pos--= (char) ('0' + part % 10);
    *pos--= (char) ('0' + part / 10);
    *pos--= '-';
    part= (int) (tmp >> 9);
    *pos--= (char) ('0' + part % 10);
    part/= 10;
    *pos--= (char) ('0' + part % 10);
    part/= 10;
    *pos--= (char) ('0' + part % 10);
    part/= 10;
    *pos= (char) ('0' + part);
    col->default_value= {ts, len};
    break;
  }
  case MYSQL_TYPE_TIME: {
    int scale= col->length - MIN_TIME_WIDTH - 1;
    scale= scale < 0 ? 0 : scale;
    if (scale <= 0)
    {
      MYSQL_TIME ltime;
      long tmp= (long) sint3korr(frm + offset);
      ltime.neg= 0;
      if (tmp < 0)
      {
        ltime.neg= 1;
        tmp= -tmp;
      }
      ltime.year= ltime.month= ltime.day= 0;
      ltime.hour= (int) (tmp / 10000);
      tmp-= ltime.hour * 10000;
      ltime.minute= (int) tmp / 100;
      ltime.second= (int) tmp % 100;
      ltime.second_part= 0;
      ltime.time_type= MYSQL_TIMESTAMP_TIME;
      ts= c_malloc(MAX_DATE_STRING_REP_LENGTH);
      len= my_time_to_str(&ltime, ts, scale);
      col->default_value= {ts, len};
    }
    else
    {
      uint32 nbyte= time_m_hires_bytes[scale];
      longlong packed= 0;
      switch (nbyte)
      {
      case 3:
        packed= mi_uint3korr(frm + offset);
        break;
      case 4:
        packed= mi_uint4korr(frm + offset);
        break;
      case 5:
        packed= mi_uint5korr(frm + offset);
        break;
      case 6:
        packed= mi_uint6korr(frm + offset);
        break;
      }
      longlong zero_point= sec_part_shift(
          ((TIME_MAX_VALUE_SECONDS + 1LL) * TIME_SECOND_PART_FACTOR), scale);
      packed= sec_part_unshift(packed - zero_point, scale);

      MYSQL_TIME my_time;
      if ((my_time.neg= packed < 0))
        packed= -packed;
      get_one(my_time.second_part, 1000000ULL);
      get_one(my_time.second, 60U);
      get_one(my_time.minute, 60U);
      get_one(my_time.hour, 24U);
      get_one(my_time.day, 32U);
      get_one(my_time.month, 13U);
      my_time.year= (uint) packed;
      my_time.time_type= MYSQL_TIMESTAMP_TIME;
      my_time.hour+= (my_time.month * 32 + my_time.day) * 24;
      my_time.month= my_time.day= 0;
      ts= c_malloc(MAX_DATE_STRING_REP_LENGTH);
      len= my_time_to_str(&my_time, ts, scale);
      col->default_value= {ts, len};
    }
    break;
  }
  case MYSQL_TYPE_TIME2: {
    int scale= col->length - MIN_TIME_WIDTH - 1;
    scale= scale < 0 ? 0 : scale;
    MYSQL_TIME ltime;
    decimal_digits_t dec= (decimal_digits_t) scale;
    longlong tmp= my_time_packed_from_binary(frm + offset, dec);
    TIME_from_longlong_time_packed(&ltime, tmp);
    ts= c_malloc(MAX_DATE_STRING_REP_LENGTH);
    len= my_time_to_str(&ltime, ts, dec);
    col->default_value= {ts, len};
    break;
  }
  case MYSQL_TYPE_DATETIME: {
    int scale= col->length - MAX_DATETIME_WIDTH - 1;
    scale= scale < 0 ? 0 : scale;
    if (scale <= 0)
    {
      ts= c_malloc(col->length + 1);
      len= col->length;
      ulonglong tmp;
      long part1, part2;
      char *pos;
      int part3;
      tmp= sint8korr(frm + offset);
      part1= (long) (tmp / 1000000LL);
      part2= (long) (tmp - (ulonglong) part1 * 1000000LL);
      pos= ts + MAX_DATETIME_WIDTH;
      *pos--= 0;
      *pos--= (char) ('0' + (char) (part2 % 10));
      part2/= 10;
      *pos--= (char) ('0' + (char) (part2 % 10));
      part3= (int) (part2 / 10);
      *pos--= ':';
      *pos--= (char) ('0' + (char) (part3 % 10));
      part3/= 10;
      *pos--= (char) ('0' + (char) (part3 % 10));
      part3/= 10;
      *pos--= ':';
      *pos--= (char) ('0' + (char) (part3 % 10));
      part3/= 10;
      *pos--= (char) ('0' + (char) part3);
      *pos--= ' ';
      *pos--= (char) ('0' + (char) (part1 % 10));
      part1/= 10;
      *pos--= (char) ('0' + (char) (part1 % 10));
      part1/= 10;
      *pos--= '-';
      *pos--= (char) ('0' + (char) (part1 % 10));
      part1/= 10;
      *pos--= (char) ('0' + (char) (part1 % 10));
      part3= (int) (part1 / 10);
      *pos--= '-';
      *pos--= (char) ('0' + (char) (part3 % 10));
      part3/= 10;
      *pos--= (char) ('0' + (char) (part3 % 10));
      part3/= 10;
      *pos--= (char) ('0' + (char) (part3 % 10));
      part3/= 10;
      *pos= (char) ('0' + (char) part3);
    }
    else
    {
      longlong packed= mi_sint8korr(frm + offset);
      packed= sec_part_unshift(packed, scale);
      MYSQL_TIME my_time;
      if ((my_time.neg= packed < 0))
        packed= -packed;
      get_one(my_time.second_part, 1000000ULL);
      get_one(my_time.second, 60U);
      get_one(my_time.minute, 60U);
      get_one(my_time.hour, 24U);
      get_one(my_time.day, 32U);
      get_one(my_time.month, 13U);
      my_time.year= (uint) packed;
      my_time.time_type= MYSQL_TIMESTAMP_DATETIME;
      ts= c_malloc(MAX_DATE_STRING_REP_LENGTH);
      len= my_datetime_to_str(&my_time, ts, scale);
      col->default_value= {ts, len};
    }
    break;
  }
  case MYSQL_TYPE_DATETIME2: {
    int scale= col->length - MAX_DATETIME_WIDTH - 1;
    scale= scale < 0 ? 0 : scale;
    MYSQL_TIME ltime;
    decimal_digits_t dec= (decimal_digits_t) scale;
    longlong tmp= my_datetime_packed_from_binary(frm + offset, dec);
    TIME_from_longlong_datetime_packed(&ltime, tmp);
    ts= c_malloc(MAX_DATE_STRING_REP_LENGTH);
    len= my_datetime_to_str(&ltime, ts, dec);
    col->default_value= {ts, len};
    break;
  }
  case MYSQL_TYPE_TIMESTAMP2: {
    int scale= col->length - MAX_DATETIME_WIDTH - 1;
    scale= scale < 0 ? 0 : scale;
    decimal_digits_t dec= (decimal_digits_t) scale;
    struct timeval tm;
    my_timestamp_from_binary(&tm, frm + offset, dec);
    char *value= c_malloc(100 + col->length + 1);
    struct tm tmp_tm;
    time_t tmp_t= (time_t) tm.tv_sec;
    localtime_r(&tmp_t, &tmp_tm);
    strftime(value, col->length + 1, "%Y-%m-%d %H:%M:%S", &tmp_tm); //UTC to system time
    if (dec)
    {
      ulong sec_part= tm.tv_usec;
      char *buf= value + MAX_DATETIME_WIDTH;
      for (int i= dec; i > 0; i--, sec_part/= 10)
        buf[i]= (char) (sec_part % 10) + '0';
      buf[0]= '.';
      buf[dec + 1]= 0;
      (dec + 1);
    }
    char scale_str[5]= "";
    if(scale > 0)
      sprintf(scale_str, "(%d)", scale);
    switch(col->unireg_check)
    {
    case TIMESTAMP_DN_FIELD:
      len= sprintf(buffer, "CURRENT_TIMESTAMP%s", scale_str);
      break;
    case TIMESTAMP_UN_FIELD:
      len= sprintf(buffer, "'%s' ON UPDATE CURRENT_TIMESTAMP%s", value, scale_str);
      break;
    case TIMESTAMP_DNUN_FIELD:
      len= sprintf(buffer, "CURRENT_TIMESTAMP%s on UPDATE CURRENT_TIMESTAMP%s",
              scale_str, scale_str);
      break;
    default:
      len= sprintf(buffer, "'%s'", value);
      break;
    }
    ts= c_malloc(len + 1);
    memcpy(ts, buffer, len + 1);
    col->default_value= {ts, len};
    break;
  }
  case MYSQL_TYPE_YEAR: {
    int tmp= (int) frm[offset];
    if (col->length != 4)
      tmp%= 100;
    else if (tmp)
      tmp+= 1900;
    ts= c_malloc(5);
    sprintf(ts, col->length == 2 ? "%02d" : "%04d", tmp);
    len=col->length;
    col->default_value= {ts, len};
    break;
  }
  default: break;
  }
  if (is_numeric_type(ftype) && f_is_zerofill(col->flags))
    prepend_zeroes(col, ts);
  if (ftype == MYSQL_TYPE_VARCHAR || ftype == MYSQL_TYPE_VAR_STRING ||
      ftype == MYSQL_TYPE_STRING)
    change_default_value_charset(col);
}

static int parse(frm_file_data *ffd, const uchar *frm, size_t len)
{
  if (len < FRM_HEADER_SIZE + FRM_FORMINFO_SIZE)
    goto err;
  size_t current_pos, end;
  size_t t, comment_pos; //, extra_info_pos;
  size_t parser_offset;
  size_t column_comment_pos;
  //ffd->connect_string= {NULL, 0};
  //ffd->engine_name= {NULL, 0};
  ffd->magic_number= uint2korr(frm);

  ffd->mysql_version= uint4korr(frm + 51);
  ffd->keyinfo_offset= uint2korr(frm + 6);
  ffd->keyinfo_length= uint2korr(frm + 14);
  if (ffd->keyinfo_length == 65535)
    ffd->keyinfo_length= uint4korr(frm + 47);
  ffd->defaults_offset= ffd->keyinfo_offset + ffd->keyinfo_length;
  ffd->defaults_length= uint2korr(frm + 16);

  ffd->extrainfo_offset= ffd->defaults_offset + ffd->defaults_length;
  ffd->extrainfo_length= uint2korr(frm + 55);

  ffd->names_length= uint2korr(frm + 4);
  ffd->forminfo_offset= uint4korr(frm + FRM_HEADER_SIZE + ffd->names_length);

  ffd->screens_length= uint2korr(frm + ffd->forminfo_offset + 260);

  ffd->null_fields= uint2korr(frm + ffd->forminfo_offset + 282);
  ffd->column_count= uint2korr(frm + ffd->forminfo_offset + 258);
  ffd->names_length= uint2korr(frm + ffd->forminfo_offset + 268);
  ffd->labels_length= uint2korr(frm + ffd->forminfo_offset + 274);
  ffd->comments_length= uint2korr(frm + ffd->forminfo_offset + 284);
  ffd->metadata_offset=
      ffd->forminfo_offset + FRM_FORMINFO_SIZE + ffd->screens_length;
  ffd->metadata_length=
      17 * ffd->column_count; // 17 bytes of metadata per column

  ffd->table_charset= frm[38];
  if (get_charset(ffd, ffd->table_charset))
    goto err;
  ffd->min_rows= uint4korr(frm + 22);
  ffd->max_rows= uint4korr(frm + 18);
  ffd->avg_row_length= uint4korr(frm + 34);
  ffd->row_format= frm[40];
  ffd->key_block_size= uint2korr(frm + 62);
  ffd->handler_option= uint2korr(frm + 30);

  parser_offset= ffd->extrainfo_length;
  if (ffd->extrainfo_length)
  {
    current_pos= ffd->extrainfo_offset;
    end= current_pos + ffd->extrainfo_length;
    ffd->connect_string.length= uint2korr(frm + current_pos);
    current_pos+= 2;
    ffd->connection= c_malloc(ffd->connect_string.length);
    memcpy(ffd->connection, frm + current_pos, ffd->connect_string.length);
    ffd->connect_string.str= ffd->connection;
    current_pos+= ffd->connect_string.length;
    if (current_pos + 2 < end)
    {
      ffd->engine_name.length= uint2korr(frm + current_pos);
      current_pos+= 2;
      char *ts= c_malloc(ffd->engine_name.length + 1);
      memcpy(ts, frm + current_pos, ffd->engine_name.length);
      ts[ffd->engine_name.length]= '\0';
      ffd->engine_name.str= ts;
      current_pos+= ffd->engine_name.length;
    }
    if (current_pos + 5 < end)
    {
      ffd->partition_info.length= uint4korr(frm + current_pos);
      current_pos+= 4;
      char *ts= c_malloc(ffd->partition_info.length + 1);
      memcpy(ts, frm + current_pos, ffd->partition_info.length + 1);
      // ts[ffd->partition_info.length + 1]= '\0';
      ffd->partition_info.str= ts;
      current_pos+= (ffd->partition_info.length + 1);
    }
    if (ffd->mysql_version >= 50110 && current_pos < end)
      current_pos++;
    // extra_info_pos= current_pos;
    parser_offset= current_pos;
  }
  ffd->legacy_db_type_1= frm[3];
  ffd->legacy_db_type_2= frm[61];
  //---READ EXTRA2---
  ffd->extra2_len= uint2korr(frm + 4);
  current_pos= 64;
  end= current_pos + ffd->extra2_len;
  //ffd->field_data_type_info= {(uchar *) "", 0};
  if ((uchar) frm[64] != '/')
  {
    while (current_pos + 3 <= end)
    {
      extra2_frm_value_type type= (extra2_frm_value_type) frm[current_pos++];
      size_t tlen= frm[current_pos++];
      if (!tlen)
      {
        if (current_pos + 2 >= end)
          goto err;
        tlen= uint2korr(frm + current_pos);
        current_pos+= 2;
        if (tlen < 256 || current_pos + tlen > end)
          goto err;
      }
      switch (type)
      {
      case EXTRA2_TABLEDEF_VERSION:
        ffd->version= {frm + current_pos, tlen};
        break;
      case EXTRA2_ENGINE_TABLEOPTS:
        ffd->options= {frm + current_pos, tlen};
        break;
      case EXTRA2_DEFAULT_PART_ENGINE:
        ffd->engine= {frm + current_pos, tlen};
        break;
      case EXTRA2_GIS:
        ffd->gis= {frm + current_pos, tlen};
        break;
      case EXTRA2_PERIOD_FOR_SYSTEM_TIME:
        ffd->system_period= {frm + current_pos, tlen};
        break;
      case EXTRA2_FIELD_FLAGS:
        ffd->field_flags= {frm + current_pos, tlen};
        break;
      case EXTRA2_APPLICATION_TIME_PERIOD:
        ffd->application_period= {frm + current_pos, tlen};
        break;
      case EXTRA2_PERIOD_WITHOUT_OVERLAPS:
        ffd->without_overlaps= {frm + current_pos, tlen};
        break;
      case EXTRA2_FIELD_DATA_TYPE_INFO:
        ffd->field_data_type_info= {frm + current_pos, tlen};
        break;
      case EXTRA2_INDEX_FLAGS:
        ffd->index_flags= {frm + current_pos, tlen};
        break;
      default:
        if (type >= EXTRA2_ENGINE_IMPORTANT)
          goto extra2_end;
      }
      current_pos+= tlen;
    }
  }
  ffd->columns= new column[ffd->column_count];
  if (ffd->field_data_type_info.length)
  {
    current_pos= 0;
    end= current_pos + ffd->field_data_type_info.length;
    while (current_pos < end)
    {
      uint fieldnr= (uint) ffd->field_data_type_info.str[current_pos++];
      size_t tlen= (uint) ffd->field_data_type_info.str[current_pos++];
      uchar *ts=
          (uchar *) my_malloc(PSI_NOT_INSTRUMENTED, (tlen + 1), MYF(MY_WME));
      memcpy(ts, ffd->field_data_type_info.str + current_pos, tlen);
      ts[tlen]= '\0';
      ffd->columns[fieldnr].extra_data_type_info.length= tlen;
      ffd->columns[fieldnr].extra_data_type_info.str= ts;
      current_pos+= tlen;
    }
  }
  //---READ COLUMN NAMES---
  current_pos= ffd->metadata_offset + ffd->metadata_length;
  end= current_pos + ffd->names_length;
  current_pos+= 1;
  for (uint i= 0; i < ffd->column_count; i++)
  {
    size_t start= current_pos;
    while (frm[current_pos++] != 255)
      ;
    size_t len= current_pos - start - 1;
    ffd->columns[i].name.length= len;
    char *ts= c_malloc(len + 1);
    memcpy(ts, frm + start, len);
    ts[len]= '\0';
    ffd->columns[i].name.str= ts;
  }
  //---READ LABEL INFORMATION---
  current_pos= end;
  end= current_pos + ffd->labels_length;
  ffd->labels= new label[ffd->column_count];
  current_pos+= 1;
  for (uint i= 0; current_pos < end;)
  {
    size_t start= current_pos;
    while (frm[current_pos++] != 255)
      ;
    size_t len= current_pos - start - 1;
    char *ts= c_malloc(len + 1);
    memcpy(ts, frm + start, len);
    ts[len]= '\0';
    ffd->labels[i].names.push_back({ts, len});
    if (frm[current_pos] == 0)
    {
      i+= 1;
      current_pos+= 2;
    }
  }
  column_comment_pos= end;
  //---READ MORE COLUMN INFO---
  current_pos= ffd->metadata_offset;
  end= current_pos + ffd->metadata_length;
  for (uint i= 0; i < ffd->column_count; i++)
  {
    ffd->columns[i].length= uint2korr(frm + current_pos + 3);
    ffd->columns[i].flags= uint2korr(frm + current_pos + 8);
    ffd->columns[i].unireg_check= (uint) frm[current_pos + 10];
    ffd->columns[i].type= (enum enum_field_types)(uint) frm[current_pos + 13];
    ffd->columns[i].comment.length= uint2korr(frm + current_pos + 15);
    if (ffd->columns[i].comment.length)
    {
      char *ts= c_malloc(ffd->columns[i].comment.length + 1);
      memcpy(ts, frm + column_comment_pos, ffd->columns[i].comment.length);
      ts[ffd->columns[i].comment.length]= '\0';
      ffd->columns[i].comment.str= ts;
      column_comment_pos+= ffd->columns[i].comment.length;
    }
    ffd->columns[i].charset_id=
        (frm[current_pos + 11] << 8) + frm[current_pos + 14];
    if (ffd->columns[i].type == MYSQL_TYPE_GEOMETRY)
    {
      ffd->columns[i].charset_id= 63;
      ffd->columns[i].subtype=
          (enum geometry_types)(uint) frm[current_pos + 14];
    }
    ffd->columns[i].defaults_offset= uint3korr(frm + current_pos + 5) - 1;
    ffd->columns[i].label_id= (uint) frm[current_pos + 12] - 1;
    current_pos+= 17;
    //ffd->columns[i].vcol_exp= {"", 0};
    ffd->columns[i].isVirtual= false;
    //ffd->columns[i].check_constraint= {"", 0};
  }
  //---READ DEFAULTS---
  ffd->null_bit= 1;
  if (ffd->handler_option & HA_OPTION_PACK_RECORD)
    ffd->null_bit= 0;
  current_pos= ffd->defaults_offset;
  end= current_pos + ffd->defaults_length;
  //printf("Null bit: %u\n", ffd->null_bit);
  //printf("Default offset: %zu\n", current_pos);
  for (uint i= 0; i < ffd->column_count; i++)
  {
    bool auto_increment= ffd->columns[i].unireg_check == NEXT_NUMBER;
    if (f_no_default(ffd->columns[i].flags) || auto_increment)
    {
      ffd->columns[i].default_value= {NULL, 0};
      continue;
    }
    if (f_maybe_null(ffd->columns[i].flags))
    {
      uint ofst= ffd->null_bit / 8;
      uint null_byte= frm[current_pos + ofst];
      uint null_bit= ffd->null_bit % 8;
      ffd->null_bit++;
      if (null_byte & (1 << null_bit) && ffd->columns[i].unireg_check != 20)
      {
        ffd->columns[i].default_value= {"NULL", 4};
        continue;
      } 
    }
    if (ffd->columns[i].unireg_check == 20)
    {
      ffd->columns[i].default_value= {NULL, 0};
      continue;
    }
    read_default_value(ffd, frm, ffd->columns + i);
  }
  //---READ KEY INFORMATION---
  current_pos= ffd->keyinfo_offset;
  end= current_pos + ffd->keyinfo_length;
  ffd->key_count= frm[current_pos++];
  if (ffd->key_count < 128)
  {
    ffd->key_parts_count= frm[current_pos++];
  }
  else
  {
    ffd->key_count= (ffd->key_count & 0x7f) | (frm[current_pos++] << 7);
    ffd->key_parts_count= uint2korr(frm + current_pos);
  }
  current_pos+= 2;
  ffd->key_extra_length= uint2korr(frm + current_pos);
  current_pos+= 2;
  ffd->key_extra_info_offset=
      (uint) (current_pos + ffd->key_count * BYTES_PER_KEY +
              ffd->key_parts_count * BYTES_PER_KEY_PART);
  ffd->keys= new key[ffd->key_count];
  t= current_pos;
  current_pos= ffd->key_extra_info_offset;
  end= current_pos + ffd->keyinfo_length;
  current_pos+= 1;
  for (uint i= 0; i < ffd->key_count; i++)
  {
    size_t start= current_pos;
    while (frm[current_pos++] != 255)
      ;
    size_t len= current_pos - start - 1;
    ffd->keys[i].name.length= len;
    char *ts= c_malloc(len + 1);
    memcpy(ts, frm + start, len);
    ts[len]= '\0';
    ffd->keys[i].name.str= ts;
  }
  ffd->key_comment_offset= (uint) current_pos;
  current_pos= t;
  comment_pos= ffd->key_comment_offset + 1;
  for (uint i= 0; i < ffd->key_count; i++)
  {
    ffd->keys[i].flags= uint2korr(frm + current_pos) ^ HA_NOSAME;
    current_pos+= 2;
    ffd->keys[i].key_info_length=
        uint2korr(frm + current_pos); // length, not used
    current_pos+= 2;
    ffd->keys[i].parts_count= frm[current_pos++];
    ffd->keys[i].algorithm= (enum ha_key_alg)(uint) frm[current_pos++];
    ffd->keys[i].key_block_size= uint2korr(frm + current_pos);
    current_pos+= 2;
    if (ffd->keys[i].flags & HA_USES_COMMENT)
    {
      ffd->keys[i].comment.length= uint2korr(frm + comment_pos);
      comment_pos+= 2;
      char *ts= c_malloc(ffd->keys[i].comment.length + 1);
      memcpy(ts, frm + comment_pos, ffd->keys[i].comment.length);
      ts[ffd->keys[i].comment.length]= '\0';
      ffd->keys[i].comment.str= ts;
      comment_pos+= ffd->keys[i].comment.length;
    }
    if (ffd->keys[i].flags & HA_USES_PARSER)
    {
      // read parser information
      ffd->keys[i].parser= {(char *) (frm + parser_offset),
                            strlen((char *) (frm + parser_offset))};
      parser_offset= ffd->keys[i].parser.length + 1;
    }
    ffd->keys[i].key_parts= new key_part[ffd->keys[i].parts_count];
    for (uint j= 0; j < ffd->keys[i].parts_count; j++)
    {
      ffd->keys[i].key_parts[j].fieldnr=
          uint2korr(frm + current_pos) & FIELD_NR_MASK;
      ffd->keys[i].key_parts[j].offset= uint2korr(frm + current_pos + 2) - 1;
      ffd->keys[i].key_parts[j].key_part_flag= frm[current_pos + 4];
      ffd->keys[i].key_parts[j].key_type= uint2korr(frm + current_pos + 5);
      ffd->keys[i].key_parts[j].length= uint2korr(frm + current_pos + 7);
      current_pos+= 9;
    }
    ffd->keys[i].is_unique= ffd->keys[i].flags & HA_NOSAME;
  }
  if (frm[ffd->forminfo_offset + 46] == (uchar) 255) // table comment
  {
    if (parser_offset + 2 > ffd->extrainfo_offset + ffd->extrainfo_length)
      goto err;
    ffd->table_comment.length= uint2korr(frm + parser_offset);
    parser_offset+= 2;
    char *ts c_malloc(ffd->table_comment.length);
    memcpy(ts, frm + parser_offset, ffd->table_comment.length);
    ffd->table_comment.str= ts;
  }
  else
  {
    ffd->table_comment= {(char *) frm + ffd->forminfo_offset + 47,
                         frm[ffd->forminfo_offset + 46]};
  }
  ffd->disk_buff=
      uint4korr(frm + FRM_HEADER_SIZE + ffd->extra2_len) + FRM_FORMINFO_SIZE;
  ffd->vcol_screen_length= uint2korr(frm + ffd->forminfo_offset + 286);
  ffd->vcol_offset= ffd->disk_buff + ffd->metadata_length +
                    ffd->screens_length + ffd->names_length +
                    ffd->labels_length + ffd->comments_length;
  current_pos= ffd->vcol_offset;
  end= current_pos + ffd->vcol_screen_length;
  current_pos+= FRM_VCOL_NEW_BASE_SIZE;
  while (current_pos < end)
  {
    uint type= frm[current_pos++];
    uint field_nr= uint2korr(frm + current_pos);
    current_pos+= 2;
    uint expr_length= uint2korr(frm + current_pos);
    current_pos+= 2;
    uint name_length= frm[current_pos++];
    char *nts= c_malloc(name_length + 1);
    memcpy(nts, frm + current_pos, name_length);
    nts[name_length]= '\0';
    current_pos+= name_length;
    char *ets= c_malloc(expr_length + 1);
    memcpy(ets, frm + current_pos, expr_length);
    ets[expr_length]= '\0';
    current_pos+= expr_length;
    switch (type)
    {
    case VCOL_GENERATED_VIRTUAL:
      ffd->columns[field_nr].isVirtual= true;
      ffd->columns[field_nr].vcol_exp.length= expr_length;
      ffd->columns[field_nr].vcol_exp.str= ets;
      break;
    case VCOL_GENERATED_STORED:
      ffd->columns[field_nr].vcol_exp.length= expr_length;
      ffd->columns[field_nr].vcol_exp.str= ets;
      break;
    case VCOL_DEFAULT:
      break;
    case VCOL_CHECK_FIELD:
      ffd->columns[field_nr].check_constraint.length= expr_length;
      ffd->columns[field_nr].check_constraint.str= ets;
      break;
    case VCOL_CHECK_TABLE:
      ffd->check_constraint_names.push_back({nts, name_length});
      ffd->check_constraints.push_back({ets, expr_length});
      break;
    }
  }
  return 0;
extra2_end:
  printf("Unknown important extra2 value...\n");
  return 0;
err:
  printf("Corrupt frm file...\n");
  return 1;
}

static void print_column_charset(uint cs_id, uint table_cs_id)
{
  if (table_cs_id == cs_id)
    return;
  if (cs_id == 63) // binary
    return;
  CHARSET_INFO *c= get_charset(cs_id, MYF(0));
  printf(" CHARACTER SET %s", c->cs_name.str);
  if (!default_chrsts.count(cs_id))
    printf(" COLLATE=%s", c->coll_name.str);
}

static uint get_max_len(uint cs_id)
{
  CHARSET_INFO *c= get_charset(cs_id, MYF(0));
  return c->mbmaxlen;
}

static const char *get_type_name(enum_field_types ftype)
{
  switch (ftype)
  {
  case MYSQL_TYPE_DECIMAL:
    return "decimal";
  case MYSQL_TYPE_TINY:
    return "tinyint";
  case MYSQL_TYPE_SHORT:
    return "smallint";
  case MYSQL_TYPE_LONG:
    return "int";
  case MYSQL_TYPE_FLOAT:
    return "float";
  case MYSQL_TYPE_DOUBLE:
    return "double";
  case MYSQL_TYPE_NULL:
    return "null";
  case MYSQL_TYPE_TIMESTAMP:
    return "timestamp";
  case MYSQL_TYPE_LONGLONG:
    return "bigint";
  case MYSQL_TYPE_INT24:
    return "mediumint";
  case MYSQL_TYPE_DATE:
    return "date";
  case MYSQL_TYPE_TIME:
    return "time";
  case MYSQL_TYPE_DATETIME:
    return "datetime";
  case MYSQL_TYPE_YEAR:
    return "year";
  case MYSQL_TYPE_NEWDATE:
    return "newdate";
  case MYSQL_TYPE_VARCHAR:
    return "varchar";
  case MYSQL_TYPE_BIT:
    return "bit";
  case MYSQL_TYPE_TIMESTAMP2:
    return "timestamp";
  case MYSQL_TYPE_DATETIME2:
    return "datetime";
  case MYSQL_TYPE_TIME2:
    return "time";
  case MYSQL_TYPE_NEWDECIMAL:
    return "decimal";
  case MYSQL_TYPE_ENUM:
    return "enum";
  case MYSQL_TYPE_SET:
    return "set";
  case MYSQL_TYPE_TINY_BLOB:
    return "tinytext";
  case MYSQL_TYPE_MEDIUM_BLOB:
    return "mediumtext";
  case MYSQL_TYPE_LONG_BLOB:
    return "longtext";
  case MYSQL_TYPE_BLOB:
    return "text";
  case MYSQL_TYPE_VAR_STRING:
    return "var_string";
  case MYSQL_TYPE_STRING:
    return "char";
  case MYSQL_TYPE_GEOMETRY:
    return "geometry";
  default: break;
  }
  return "invalid type";
}

static void print_default_value(column col)
{
  printf(" DEFAULT ");
  const char *str= col.default_value.str;
  enum_field_types ftype= col.type;
  if (is_numeric_type(ftype) || ftype == MYSQL_TYPE_YEAR ||
      ftype == MYSQL_TYPE_TIMESTAMP2 || !strcmp(str, "NULL"))
    printf("%s", str);
  else
    printf("'%s'", str);
}

static void print_column(frm_file_data *ffd, int c_id)
{
  enum_field_types ftype= ffd->columns[c_id].type;
  uint length= ffd->columns[c_id].length;
  int label_id= ffd->columns[c_id].label_id;
  const char *type_name= get_type_name(ftype);
  if (ffd->columns[c_id].extra_data_type_info.length)
  {
    printf("%s", ffd->columns[c_id].extra_data_type_info.str);
  }
  else if (ftype == MYSQL_TYPE_TINY || ftype == MYSQL_TYPE_SHORT ||
           ftype == MYSQL_TYPE_INT24 || ftype == MYSQL_TYPE_LONG ||
           ftype == MYSQL_TYPE_LONGLONG)
  {
    printf("%s(%d)", type_name, length);
    if (!f_is_dec(ffd->columns[c_id].flags))
      printf(" unsigned");
    if (f_is_zerofill(ffd->columns[c_id].flags))
      printf(" zerofill");
  }
  else if (ftype == MYSQL_TYPE_NEWDECIMAL||ftype==MYSQL_TYPE_DECIMAL)
  {
    uint precision= ffd->columns[c_id].length;
    uint scale= f_decimals(ffd->columns[c_id].flags);
    if (scale)
      precision--;
    if (precision)
      precision--;
    if (!f_is_dec(ffd->columns[c_id].flags))
      precision++;
    printf("%s(%d,%d)", type_name, precision, scale);
    if (!f_is_dec(ffd->columns[c_id].flags))
      printf(" unsigned");
    if (f_is_zerofill(ffd->columns[c_id].flags))
      printf(" zerofill");
  }
  else if (ftype == MYSQL_TYPE_FLOAT || ftype == MYSQL_TYPE_DOUBLE)
  {
    uint decimals= f_decimals(ffd->columns[c_id].flags);
    if (decimals != NOT_FIXED_DEC)
    {
      uint precision= ffd->columns[c_id].length;
      uint scale= decimals;
      if (scale < NOT_FIXED_DEC)
        printf("%s(%d,%d)", type_name, precision, scale);
    }
    else
      printf("%s", type_name);
    if (!f_is_dec(ffd->columns[c_id].flags))
      printf(" unsigned");
    if (f_is_zerofill(ffd->columns[c_id].flags))
      printf(" zerofill");
  }
  else if (ftype == MYSQL_TYPE_STRING)
  {
    if (ffd->columns[c_id].charset_id == 63)
      printf("binary");
    else
      printf("char");
    printf("(%d)", ffd->columns[c_id].length /
                       get_max_len(ffd->columns[c_id].charset_id));
    print_column_charset(ffd->columns[c_id].charset_id, ffd->table_charset);
  }
  else if (ftype == MYSQL_TYPE_VARCHAR)
  {
    if (ffd->columns[c_id].charset_id == 63)
      printf("varbinary");
    else
      printf("varchar");
    printf("(%d)", ffd->columns[c_id].length /
                       get_max_len(ffd->columns[c_id].charset_id));
    print_column_charset(ffd->columns[c_id].charset_id, ffd->table_charset);
  }
  else if (ftype == MYSQL_TYPE_TINY_BLOB)
  {
    if (ffd->columns[c_id].charset_id == 63)
      printf("tinyblob");
    else
      printf("tinytext");
    print_column_charset(ffd->columns[c_id].charset_id, ffd->table_charset);
  }
  else if (ftype == MYSQL_TYPE_BLOB)
  {
    if (ffd->columns[c_id].charset_id == 63)
      printf("blob");
    else
      printf("text");
    print_column_charset(ffd->columns[c_id].charset_id, ffd->table_charset);
  }
  else if (ftype == MYSQL_TYPE_MEDIUM_BLOB)
  {
    if (ffd->columns[c_id].charset_id == 63)
      printf("mediumblob");
    else
      printf("mediumtext");
    print_column_charset(ffd->columns[c_id].charset_id, ffd->table_charset);
  }
  else if (ftype == MYSQL_TYPE_LONG_BLOB)
  {
    if (ffd->columns[c_id].charset_id == 63)
      printf("longblob");
    else
      printf("longtext");
    print_column_charset(ffd->columns[c_id].charset_id, ffd->table_charset);
  }
  else if (ftype == MYSQL_TYPE_BIT)
  {
    printf("bit(%d)", ffd->columns[c_id].length);
  }
  else if (ftype == MYSQL_TYPE_TIME || ftype == MYSQL_TYPE_TIME2)
  {
    long scale= (long) ffd->columns[c_id].length - MIN_TIME_WIDTH - 1;
    if (scale > 0)
      printf("time(%ld)", scale);
    else
      printf("time");
  }
  else if (ftype == MYSQL_TYPE_TIMESTAMP || ftype == MYSQL_TYPE_TIMESTAMP2)
  {
    long scale= (long) ffd->columns[c_id].length - MAX_DATETIME_WIDTH - 1;
    if (scale > 0)
      printf("timestamp(%ld)", scale);
    else
      printf("timestamp");
  }
  else if (ftype == MYSQL_TYPE_YEAR)
  {
    printf("year(%d)", ffd->columns[c_id].length);
  }
  else if (ftype == MYSQL_TYPE_DATE || ftype == MYSQL_TYPE_NEWDATE)
  {
    printf("date");
  }
  else if (ftype == MYSQL_TYPE_DATETIME || ftype == MYSQL_TYPE_DATETIME2)
  {
    long scale= (long) ffd->columns[c_id].length - MAX_DATETIME_WIDTH - 1;
    if (scale > 0)
      printf("datetime(%ld)", scale);
    else
      printf("datetime");
  }
  else if (ftype == MYSQL_TYPE_GEOMETRY)
  {
    switch (ffd->columns[c_id].subtype)
    {
    case GEOM_GEOMETRY:
      printf("geometry");
      break;
    case GEOM_POINT:
      printf("point");
      break;
    case GEOM_LINESTRING:
      printf("linestring");
      break;
    case GEOM_POLYGON:
      printf("polygon");
      break;
    case GEOM_MULTIPOINT:
      printf("multipoint");
      break;
    case GEOM_MULTILINESTRING:
      printf("multilinestring");
      break;
    case GEOM_MULTIPOLYGON:
      printf("multipolygon");
      break;
    case GEOM_GEOMETRYCOLLECTION:
      printf("geometrycollection");
      break;
    }
  }
  else if (ftype == MYSQL_TYPE_ENUM || ftype == MYSQL_TYPE_SET)
  {
    printf("%s(", type_name);
    for (uint j= 0; j < ffd->labels[label_id].names.size() - 1; j++)
    {
      LEX_CSTRING ts= ffd->labels[label_id].names.at(j);
      printf("'%s',", ts.str);
    }
    LEX_CSTRING ts=
        ffd->labels[label_id].names.at(ffd->labels[label_id].names.size() - 1);
    printf("'%s')", ts.str);
  }
  else
    printf("%s(%d)", type_name, length); // remove it
  if (!f_maybe_null(ffd->columns[c_id].flags))
    printf(" NOT NULL");
  if (ffd->columns[c_id].unireg_check == 15)
    printf(" AUTO_INCREMENT");
  if (ffd->columns[c_id].default_value.length != 0 &&
      !ffd->columns[c_id].vcol_exp.length)
    print_default_value(ffd->columns[c_id]);
  if (ffd->columns[c_id].vcol_exp.length)
  {
    printf(" GENERATED ALWAYS AS (");
    printf("%s", ffd->columns[c_id].vcol_exp.str);
    printf(")");
    if (ffd->columns[c_id].isVirtual)
      printf(" VIRTUAL");
    else
      printf(" STORED");
  }
  if (ffd->columns[c_id].check_constraint.length)
  {
    printf(" CHECK");
    printf(" (%s)", ffd->columns[c_id].check_constraint.str);
  }
  if (ffd->columns[c_id].comment.length != 0)
    printf(" COMMENT '%s'", ffd->columns[c_id].comment.str);
}

static void print_keys(frm_file_data *ffd, uint k_id)
{
  key key= ffd->keys[k_id];
  if (key.flags & HA_INVISIBLE_KEY)
    return;
  bool is_primary= false;
  if (!strcmp("PRIMARY", key.name.str))
  {
    is_primary= true;
    printf("PRIMARY KEY");
  }
  else if (key.is_unique)
    printf("UNIQUE KEY");
  else if (key.flags & HA_FULLTEXT)
    printf("FULLTEXT KEY");
  else if (key.flags & HA_SPATIAL)
    printf("SPATIAL KEY");
  else
    printf("KEY");
  if (key.name.length != 0 && !is_primary)
    printf(" `%s`", key.name.str);
  printf(" (");
  for (uint i= 0; i < key.parts_count; i++)
  {
    if (i)
      printf(",");
    column colmn= ffd->columns[ffd->keys[k_id].key_parts[i].fieldnr - 1];
    enum_field_types ftype= colmn.type;
    printf("`%s`", colmn.name.str);
    if (key.flags & (HA_FULLTEXT | HA_SPATIAL))
      continue;
    if (!colmn.extra_data_type_info.length &&
        ((key.key_parts[i].length != colmn.length &&
          (ftype == MYSQL_TYPE_VARCHAR || ftype == MYSQL_TYPE_VAR_STRING ||
           ftype == MYSQL_TYPE_STRING)) ||
         ftype == MYSQL_TYPE_TINY_BLOB || ftype == MYSQL_TYPE_MEDIUM_BLOB ||
         ftype == MYSQL_TYPE_LONG_BLOB || ftype == MYSQL_TYPE_BLOB ||
         ftype == MYSQL_TYPE_GEOMETRY))
    {
      CHARSET_INFO *c= get_charset(colmn.charset_id, MYF(0));
      long len= (long) (key.key_parts[i].length / c->mbmaxlen);
      printf("(%ld)", len);
    }
  }
  printf(")");
  if (key.algorithm == HA_KEY_ALG_BTREE)
    printf(" USING BTREE");
  if (key.algorithm == HA_KEY_ALG_HASH ||
      key.algorithm == HA_KEY_ALG_LONG_HASH)
    printf(" USING HASH");
  if (key.algorithm == HA_KEY_ALG_RTREE && key.flags & HA_SPATIAL)
    printf(" USING RTREE");
  if ((key.flags & HA_USES_BLOCK_SIZE) &&
      ffd->key_block_size != key.key_block_size)
    printf(" KEY_BLOCK_SIZE=%u", key.key_block_size);
  if (key.flags & HA_USES_COMMENT)
    printf(" COMMENT '%s'", key.comment.str);
}

static void print_engine(frm_file_data *ffd)
{
  printf(" ENGINE=");
  uint engine= 0;
  if (ffd->engine_name.length == 0)
    engine= ffd->legacy_db_type_1;
  else if (ffd->engine_name.length != 0)
  {
    if (strcmp("partition", ffd->engine_name.str))
    {
      printf("%s", ffd->engine_name.str);
      return;
    }
    engine= ffd->legacy_db_type_2;
  }
  if (engine <= 28)
    printf("%s", legacy_db_types[engine]);
  else if (engine == 42)
    printf("FIRST_DYNAMIC");
  else if (engine == 127)
    printf("DEFAULT");
}

static void print_table_options(frm_file_data *ffd)
{
  if (ffd->connect_string.length)
    printf(" CONNECTION='%s'", ffd->connect_string.str);
  print_engine(ffd);
  if (ffd->table_cs_name.length != 0)
  {
    printf(" DEFAULT CHARSET=%s", ffd->table_cs_name.str);
    if (!default_chrsts.count(ffd->table_charset))
      printf(" COLLATE=%s", ffd->table_coll_name.str);
  }
  if (ffd->min_rows)
    printf(" MIN_ROWS=%u", ffd->min_rows);
  if (ffd->max_rows)
    printf(" MAX_ROWS=%u", ffd->max_rows);
  if (ffd->avg_row_length)
    printf(" AVG_ROW_LENGTH=%u", ffd->avg_row_length);
  if (ffd->key_block_size)
    printf(" KEY_BLOCK_SIZE=%u", ffd->key_block_size);
  if (ffd->table_comment.length)
    printf(" COMMENT='%s'", ffd->table_comment.str);
  if (ffd->partition_info.length)
    printf("\n%s", ffd->partition_info.str);
}

static void print_table_check_constraints(frm_file_data *ffd)
{
  uint size= (uint) ffd->check_constraints.size();
  for (uint i= 0; i < size; i++)
  {
    printf("  CONSTRAINT");
    printf(" `%s`", ffd->check_constraint_names.at(i).str);
    printf(" CHECK (");
    printf("%s)\n", ffd->check_constraints.at(i).str);
    if (i != size - 1)
      printf(",");
  }
}

static int show_create_table(LEX_CSTRING table_name, frm_file_data *ffd)
{
  printf("CREATE TABLE `%s` (\n", table_name.str);
  for (uint i= 0; i < ffd->column_count; i++)
  {
    printf("  `%s` ", ffd->columns[i].name.str);
    print_column(ffd, i);
    if (!(i == ffd->column_count - 1 && !ffd->key_count))
      printf(",");
    printf("\n");
  }
  for (uint i= 0; i < ffd->key_count; i++)
  {
    printf("  ");
    print_keys(ffd, i);
    if (i != ffd->key_count - 1)
      printf(",");
    if (i == ffd->key_count - 1 && ffd->check_constraints.size())
      printf(",");
    printf("\n");
  }
  print_table_check_constraints(ffd);
  printf(")");
  print_table_options(ffd);
  printf("\n");
  return 0;
}

static void display_frm_mariadb_version(frm_file_data *ffd)
{
  printf("%u\n", ffd->mysql_version);
  uint major= ffd->mysql_version / 10000;
  uint minor= (ffd->mysql_version % 1000) / 100;
  uint release= ffd->mysql_version % 100;
  if (major == 0 && minor == 0 && release == 0)
    printf("< 5.0");
  else
    printf("-- FRM created with MariaDB version: %u.%u.%u \n", major, minor,
           release);
}

int main(int argc, char **argv)
{
  MY_INIT(argv[0]);
  get_options(&argc, &argv);
  for (int i= 0; i < argc; i++)
  {
    const char *path= argv[i];
    uchar *frm;
    size_t len;
    frm_file_data *ffd= new frm_file_data();
    int error= read_file(path, (const uchar **) &frm, &len);
    if (error)
      continue;
    if (!is_binary_frm_header(frm))
    {
      printf("The .frm file is not a table...\n");
      continue;
    }
    parse(ffd, frm, len);
    LEX_CSTRING table_name= {NULL, 0};
    get_tablename(path, (char **) &table_name.str, &table_name.length);
    if (opt_verbose > 0)
      display_frm_mariadb_version(ffd);
    show_create_table(table_name, ffd);
  }
  my_end(0);
  return 0;
}
