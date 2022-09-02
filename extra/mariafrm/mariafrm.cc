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
#include <strfunc.h>

#include <table.h>
#include <my_getopt.h>
#include <welcome_copyright_notice.h>   /* ORACLE_WELCOME_COPYRIGHT_NOTICE */

#include "mariafrm.h"

static uint opt_verbose= 0;

static struct my_option my_long_option[]= {
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
  my_print_help(my_long_option);
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
  if ((ho_error= handle_options(argc, argv, my_long_option, get_one_option)))
    exit(ho_error);
  return;
}

int read_file(const char *path,const uchar **frm, size_t *len)
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

int get_tablename(const char* filename, char** tablename, size_t *tablename_len)
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
  res= strconvert(&my_charset_filename, ts, i,
                  system_charset_info, name_buff, FN_LEN, &errors);
  if (unlikely(errors))
    goto err_end;
  error= 0;
  *tablename= name_buff;
  *tablename_len= res;
err_end:
  my_safe_afree(ts,i);
  return error;
}

int get_charset(frm_file_data *ffd, uint cs_number)
{
  CHARSET_INFO *c= get_charset(cs_number, MYF(0));
  ffd->table_cs_name= c->cs_name;
  ffd->table_coll_name= c->coll_name;
  ffd->charset_primary_number= c->primary_number;
  return 0;
}

int parse(frm_file_data *ffd, const uchar *frm, size_t len)
{
  size_t current_pos, end;
  size_t t, comment_pos; //, extra_info_pos;
  ffd->connect_string= {NULL, 0};
  ffd->engine_name= {NULL, 0};
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
  ffd->rtype= (enum row_type)(uint) ffd->row_format;
  ffd->key_block_size= uint2korr(frm + 62);
  ffd->handler_option= uint2korr(frm + 30);

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
      ffd->engine_name.str= (char *) (frm + current_pos);
      current_pos+= ffd->engine_name.length;
    }
    if (current_pos + 5 < end)
    {
      ffd->partition_info_str_len= uint4korr(frm + current_pos);
      current_pos+= 4;
      ffd->partition_info_str= c_malloc(ffd->partition_info_str_len + 1);
      memcpy(ffd->partition_info_str, frm + current_pos,
             ffd->partition_info_str_len + 1);
      current_pos+= (ffd->partition_info_str_len + 1);
    }
    //extra_info_pos= current_pos;
  }
  ffd->legacy_db_type_1= (enum legacy_db_type)(uint) frm[3];
  ffd->legacy_db_type_2= (enum legacy_db_type)(uint) frm[61];
  //---READ COLUMN NAMES---
  ffd->columns= new column[ffd->column_count];
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
  for (uint i=0;current_pos<end;)
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
      i+=1;
      current_pos+= 2;
    } 
  }
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
    ffd->columns[i].charset_id=
        (frm[current_pos + 11] << 8) + frm[current_pos + 14];
    if (ffd->columns[i].type == MYSQL_TYPE_GEOMETRY)
    {
      ffd->columns[i].charset_id= 63;
      ffd->columns[i].subtype=
          (enum geometry_types)(uint) frm[current_pos + 14];
    }
    ffd->columns[i].defaults_offset= uint3korr(frm + current_pos + 5);
    ffd->columns[i].label_id= (uint) frm[current_pos + 12] - 1;
    current_pos+= 17;
  }
  //---READ DEFAULTS---
  ffd->null_bit= 1;
  if (ffd->handler_option & HA_PACK_RECORD)
    ffd->null_bit= 0;
  current_pos= ffd->defaults_offset;
  end= current_pos + ffd->defaults_length;
  for (uint i=0;i< ffd->column_count;i++)
  {
    bool auto_increment= ffd->columns[i].unireg_check == 15;
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
        ffd->columns[i].default_value= {"NULL", 4};
    }
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
  ffd->key_extra_info_offset= (uint)(current_pos + ffd->key_count * BYTES_PER_KEY +
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
    char *ts= c_malloc(len+1);
    memcpy(ts, frm + start, len);
    ts[len]= '\0';
    ffd->keys[i].name.str= ts;
  }
  ffd->key_comment_offset= (uint)current_pos;
  current_pos= t;
  comment_pos= ffd->key_comment_offset;
  for (uint i= 0; i < ffd->key_count; i++)
  {
    ffd->keys[i].flags= uint2korr(frm + current_pos) ^ HA_NOSAME;
    current_pos+= 2;
    ffd->keys[i].key_info_length=uint2korr(frm + current_pos); // length, not used
    current_pos+= 2;
    ffd->keys[i].parts_count= frm[current_pos++];
    ffd->keys[i].algorithm= (enum ha_key_alg)(uint) frm[current_pos++];
    ffd->keys[i].key_block_size= uint2korr(frm + current_pos);
    current_pos+= 2;
    if (ffd->keys[i].flags & HA_USES_COMMENT)
    {
      ffd->keys[i].comment.length= uint2korr(frm + comment_pos);
      comment_pos+= 2;
      char *ts= c_malloc(ffd->keys[i].comment.length);
      memcpy(ts, frm + comment_pos, ffd->keys[i].comment.length);
      ffd->keys[i].comment.str= ts;
      comment_pos+= ffd->keys[i].comment.length;
    }
    if (ffd->keys[i].flags & HA_USES_PARSER)
    {
      // read parser information
    }
    ffd->keys[i].key_parts= new key_part[ffd->keys[i].parts_count];
    for (uint j = 0; j < ffd->keys[i].parts_count; j++)
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
  ffd->extra2_len= uint2korr(frm + 4);
  current_pos= 64;
  end= current_pos + ffd->extra2_len;
  if ((uchar)frm[64] !='/')
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
extra2_end:;
  return 0;
err:
  printf("Corrupt frm file...\n");
  printf("Do nothing...\n");
  exit(0);
  return -1;
}

static std::unordered_set<uint> default_cs{
    1,  3,  4,  6,  7,  8,  9,  10, 11, 12, 13, 16, 18, 19,
    22, 24, 25, 26, 28, 30, 32, 33, 35, 36, 37, 38, 39, 40,
    41, 45, 51, 54, 56, 57, 59, 60, 63, 92, 95, 97, 98};

void print_column_charset(uint cs_id, uint table_cs_id) 
{
  if (table_cs_id == cs_id)
    return;
  if (cs_id == 63)  //binary
    return;
  CHARSET_INFO *c= get_charset(cs_id, MYF(0));
  printf(" CHARACTER SET %s", c->cs_name.str);
  if (!default_cs.count(cs_id))
    printf(" COLLATE=%s", c->coll_name.str);
}

uint get_max_len(uint cs_id) 
{
  CHARSET_INFO *c= get_charset(cs_id, MYF(0));
  return c->mbmaxlen;
}

void print_column(frm_file_data *ffd, int c_id) 
{
  enum_field_types ftype= ffd->columns[c_id].type;
  uint length= ffd->columns[c_id].length;
  int label_id= ffd->columns[c_id].label_id;
  const Type_handler *handler= Type_handler::get_handler_by_real_type(ftype);
  Name type_name= handler->name();
  if (ftype == MYSQL_TYPE_TINY || ftype == MYSQL_TYPE_SHORT ||
      ftype == MYSQL_TYPE_INT24 || ftype == MYSQL_TYPE_LONG ||
      ftype == MYSQL_TYPE_LONGLONG)
  {
    printf("%s(%d)", type_name.ptr(), length);
    if (!f_is_dec(ffd->columns[c_id].flags))
      printf(" unsigned");
    if (f_is_zerofill(ffd->columns[c_id].flags))
      printf(" zerofill");
  }
  else if (ftype == MYSQL_TYPE_NEWDECIMAL)
  {
    uint precision= ffd->columns[c_id].length;
    uint scale= f_decimals(ffd->columns[c_id].flags);
    if (scale)
      precision--;
    if (precision)
      precision--;
    printf("%s(%d,%d)",type_name.ptr(), precision, scale);
  }
  else if (ftype == MYSQL_TYPE_FLOAT || ftype == MYSQL_TYPE_DOUBLE)
  {
    uint decimals= f_decimals(ffd->columns[c_id].flags);
    if (decimals != NOT_FIXED_DEC)
    {
      uint precision= ffd->columns[c_id].length;
      uint scale= decimals;
      if (scale < NOT_FIXED_DEC)
        printf("%s(%d,%d)", type_name.ptr(), precision, scale);
      if (!f_is_dec(ffd->columns[c_id].flags))
        printf(" unsigned");
      if (f_is_zerofill(ffd->columns[c_id].flags))
        printf(" zerofill");
    }
  }
  else if (ftype == MYSQL_TYPE_STRING)
  {
    if (ffd->columns[c_id].charset_id == 63)
      printf("binary");
    else
      printf("char");
    printf("(%d)",
        ffd->columns[c_id].length / get_max_len(ffd->columns[c_id].charset_id));
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
    uint scale= ffd->columns[c_id].length - MAX_TIME_WIDTH - 1;
    if (scale > 0)
      printf("time(%d)", scale);
    else
      printf("time");
  }
  else if (ftype == MYSQL_TYPE_TIMESTAMP || ftype == MYSQL_TYPE_TIMESTAMP2)
  {
    uint scale= ffd->columns[c_id].length - MAX_DATETIME_WIDTH - 1;
    if (scale > 0)
      printf("timestamp(%d)", scale);
    else printf("timestamp");
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
    uint scale= ffd->columns[c_id].length - MAX_DATETIME_WIDTH - 1;
    if (scale > 0)
      printf("datetime(%d)", scale);
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
    printf("%s(", type_name.ptr());
    for (uint j=0;j<ffd->labels[label_id].names.size() - 1;j++)
    {
      LEX_CSTRING ts= ffd->labels[label_id].names.at(j);
      printf("'%s',", ts.str);
    }
    LEX_CSTRING ts=
        ffd->labels[label_id].names.at(ffd->labels[label_id].names.size() - 1);
    printf("'%s')", ts.str);
  }
  else
    printf("%s(%d)", type_name.ptr(), length); //remove it
  if(!f_maybe_null(ffd->columns[c_id].flags))
    printf(" NOT NULL");
  if (ffd->columns[c_id].unireg_check == 15)
    printf(" AUTO INCREMENT");
  if (ffd->columns[c_id].default_value.length != 0)
    printf(" DEFAULT %s", ffd->columns[c_id].default_value.str);
}

void print_keys(frm_file_data *ffd, uint k_id) 
{
  key key= ffd->keys[k_id];
  if (key.flags & HA_INVISIBLE_KEY)
    return;
  bool is_primary=false;
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
  if (key.name.length!=0 && !is_primary)
    printf(" `%s`", key.name.str);
  printf(" (");
  for (uint i= 0; i < key.parts_count;i++)
  {
    if (i)
      printf(",");
    column colmn= ffd->columns[ffd->keys[k_id].key_parts[i].fieldnr - 1];
    enum_field_types ftype= colmn.type;
    printf("`%s`", colmn.name.str);
    if (key.flags & (HA_FULLTEXT | HA_SPATIAL))
      continue;
    if ((key.key_parts[i].length != colmn.length &&
        (ftype == MYSQL_TYPE_VARCHAR ||
         ftype == MYSQL_TYPE_VAR_STRING ||
         ftype == MYSQL_TYPE_STRING)) ||
        ftype == MYSQL_TYPE_TINY_BLOB ||
        ftype == MYSQL_TYPE_MEDIUM_BLOB ||
        ftype == MYSQL_TYPE_LONG_BLOB ||
        ftype == MYSQL_TYPE_BLOB ||
        ftype == MYSQL_TYPE_GEOMETRY)
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
  if (key.algorithm == HA_KEY_ALG_RTREE &&
      key.flags & HA_SPATIAL)
    printf(" USING RTREE");
  if ((key.flags & HA_USES_BLOCK_SIZE) &&
      ffd->key_block_size != key.key_block_size)
    printf(" KEY_BLOCK_SIZE=%u", key.key_block_size);
  if (key.flags & HA_USES_COMMENT)
    printf(" COMMENT '%s'", key.comment.str);

}

void print_table_options(frm_file_data *ffd)
{
  if (ffd->engine_name.length != 0)
    printf(" ENGINE=%s", ffd->engine_name.str);
  if (ffd->table_cs_name.length != 0)
  {
    printf(" DEFAULT CHARSET=%s", ffd->table_cs_name.str);
    if (!default_cs.count(ffd->table_charset))
      printf(" COLLATE=%s", ffd->table_coll_name.str);
  }
}

int show_create_table(LEX_CSTRING table_name, frm_file_data *ffd)
{
  printf("CREATE TABLE `%s` (\n", table_name.str);
  for(uint i=0;i<ffd->column_count;i++)
  {
    printf("  `%s` ", ffd->columns[i].name.str);
    print_column(ffd, i);
    if (!(i == ffd->column_count - 1 && !ffd->key_count))
      printf(",");
    printf("\n");
  }
  for (uint i=0;i<ffd->key_count;i++)
  {
    printf("  ");
    print_keys(ffd, i);
    if (i != ffd->key_count - 1)
      printf(",");
    printf("\n");
  }
  printf(")");
  print_table_options(ffd);
  printf("\n");
  return 0;
}

void display_frm_mariadb_version(frm_file_data *ffd) 
{
  printf("%u\n", ffd->mysql_version);
  uint major = ffd->mysql_version/10000;
  uint minor= (ffd->mysql_version % 1000) / 100;
  uint release= ffd->mysql_version % 100;
  if (major == 0 && minor == 0 && release == 0)
    printf("< 5.0");
  else
    printf("-- FRM created with MariaDB version: %u.%u.%u \n",
        major, minor, release);
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
    read_file(path, (const uchar **) &frm, &len);
    if (!is_binary_frm_header(frm))
    {
      printf("The .frm file is not a table...\n");
      continue;
    }
    parse(ffd, frm, len);
    LEX_CSTRING table_name= {NULL, 0};
    get_tablename(path, (char**)&table_name.str, &table_name.length);
    if (opt_verbose > 0)
      display_frm_mariadb_version(ffd);
    show_create_table(table_name, ffd);
  }
  my_end(0);
  return 0;
}
