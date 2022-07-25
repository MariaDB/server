/*
   Copyright (c) 2000, 2011, Oracle and/or its affiliates.
   Copyright (c) 2009, 2018, MariaDB Corporation

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

/*
  Functions to create a unireg form-file from a FIELD and a fieldname-fieldinfo
  struct.
  In the following functions FIELD * is an ordinary field-structure with
  the following exeptions:
    sc_length,typepos,row,kol,dtype,regnr and field need not to be set.
    str is a (long) to record position where 0 is the first position.
*/

#include <my_global.h>
#include "sql_priv.h"
#include "unireg.h"
#include "sql_partition.h"                      // struct partition_info
#include "sql_class.h"                  // THD, Internal_error_handler
#include "create_options.h"
#include "discover.h"
#include <m_ctype.h>

#define FCOMP			17		/* Bytes for a packed field */

/* threshold for safe_alloca */
#define ALLOCA_THRESHOLD       2048

static uint pack_keys(uchar *,uint, KEY *, ulong);
static bool pack_header(THD *, uchar *, List<Create_field> &, HA_CREATE_INFO *,
                        ulong, handler *);
static bool pack_vcols(String *, List<Create_field> &, List<Virtual_column_info> *);
static uint get_interval_id(uint *,List<Create_field> &, Create_field *);
static bool pack_fields(uchar **, List<Create_field> &, HA_CREATE_INFO*,
                        ulong);
static size_t packed_fields_length(List<Create_field> &);
static bool make_empty_rec(THD *, uchar *, uint, List<Create_field> &, uint,
                           ulong);

/*
  write the length as
  if (  0 < length <= 255)      one byte
  if (256 < length <= 65535)    zero byte, then two bytes, low-endian
*/
static uchar *extra2_write_len(uchar *pos, size_t len)
{
  if (len <= 255)
    *pos++= (uchar)len;
  else
  {
    /*
      At the moment we support options_len up to 64K.
      We can easily extend it in the future, if the need arises.
    */
    DBUG_ASSERT(len <= 65535);
    int2store(pos + 1, len);
    pos+= 3;
  }
  return pos;
}

static uchar *extra2_write(uchar *pos, enum extra2_frm_value_type type,
                           LEX_STRING *str)
{
  *pos++ = type;
  pos= extra2_write_len(pos, str->length);
  memcpy(pos, str->str, str->length);
  return pos + str->length;
}

static uchar *extra2_write(uchar *pos, enum extra2_frm_value_type type,
                           LEX_CUSTRING *str)
{
  return extra2_write(pos, type, reinterpret_cast<LEX_STRING *>(str));
}

/*
  Create a frm (table definition) file

  @param thd                    Thread handler
  @param table                  Name of table
  @param create_info            create info parameters
  @param create_fields          Fields to create
  @param keys                   number of keys to create
  @param key_info               Keys to create
  @param db_file                Handler to use.

  @return the generated frm image as a LEX_CUSTRING,
  or null LEX_CUSTRING (str==0) in case of an error.
*/

LEX_CUSTRING build_frm_image(THD *thd, const char *table,
                              HA_CREATE_INFO *create_info,
                              List<Create_field> &create_fields,
                              uint keys, KEY *key_info, handler *db_file)
{
  LEX_STRING str_db_type;
  uint reclength, key_info_length, i;
  ulong key_buff_length;
  ulong filepos, data_offset;
  uint options_len;
  uint gis_extra2_len= 0;
  uchar fileinfo[FRM_HEADER_SIZE],forminfo[FRM_FORMINFO_SIZE];
  const partition_info *part_info= IF_PARTITIONING(thd->work_part_info, 0);
  int error;
  uchar *frm_ptr, *pos;
  LEX_CUSTRING frm= {0,0};
  StringBuffer<MAX_FIELD_WIDTH> vcols;
  DBUG_ENTER("build_frm_image");

 /* If fixed row records, we need one bit to check for deleted rows */
  if (!(create_info->table_options & HA_OPTION_PACK_RECORD))
    create_info->null_bits++;
  data_offset= (create_info->null_bits + 7) / 8;

  sql_mode_t save_sql_mode= thd->variables.sql_mode;
  thd->variables.sql_mode &= ~MODE_ANSI_QUOTES;
  error= pack_vcols(&vcols, create_fields, create_info->check_constraint_list);
  thd->variables.sql_mode= save_sql_mode;

  if (error)
    DBUG_RETURN(frm);

  if (vcols.length())
    create_info->expression_length= vcols.length() + FRM_VCOL_NEW_BASE_SIZE;

  error= pack_header(thd, forminfo, create_fields, create_info,
                     data_offset, db_file);
  if (error)
    DBUG_RETURN(frm);

  reclength= uint2korr(forminfo+266);

  /* Calculate extra data segment length */
  str_db_type= *hton_name(create_info->db_type);
  /* str_db_type */
  create_info->extra_size= (2 + str_db_type.length +
                            2 + create_info->connect_string.length);
  /*
    Partition:
      Length of partition info = 4 byte
      Potential NULL byte at end of partition info string = 1 byte
      Indicator if auto-partitioned table = 1 byte
      => Total 6 byte
  */
  create_info->extra_size+= 6;
  if (part_info)
    create_info->extra_size+= part_info->part_info_len;

  for (i= 0; i < keys; i++)
  {
    if (key_info[i].parser_name)
      create_info->extra_size+= key_info[i].parser_name->length + 1;
  }

  options_len= engine_table_options_frm_length(create_info->option_list,
                                               create_fields,
                                               keys, key_info);
#ifdef HAVE_SPATIAL
  gis_extra2_len= gis_field_options_image(NULL, create_fields);
#endif /*HAVE_SPATIAL*/
  DBUG_PRINT("info", ("Options length: %u", options_len));

  if (validate_comment_length(thd, &create_info->comment, TABLE_COMMENT_MAXLEN,
                              ER_TOO_LONG_TABLE_COMMENT,
                              table))
     DBUG_RETURN(frm);
  /*
    If table comment is longer than TABLE_COMMENT_INLINE_MAXLEN bytes,
    store the comment in an extra segment (up to TABLE_COMMENT_MAXLEN bytes).
    Pre 5.5, the limit was 60 characters, with no extra segment-handling.
  */
  if (create_info->comment.length > TABLE_COMMENT_INLINE_MAXLEN)
  {
    forminfo[46]=255;
    create_info->extra_size+= 2 + create_info->comment.length;
  }
  else
  {
    strmake((char*) forminfo+47, create_info->comment.str ?
            create_info->comment.str : "", create_info->comment.length);
    forminfo[46]=(uchar) create_info->comment.length;
  }

  if (!create_info->tabledef_version.str)
  {
    uchar *to= (uchar*) thd->alloc(MY_UUID_SIZE);
    if (unlikely(!to))
      DBUG_RETURN(frm);
    my_uuid(to);
    create_info->tabledef_version.str= to;
    create_info->tabledef_version.length= MY_UUID_SIZE;
  }
  DBUG_ASSERT(create_info->tabledef_version.length > 0);
  DBUG_ASSERT(create_info->tabledef_version.length <= 255);

  prepare_frm_header(thd, reclength, fileinfo, create_info, keys, key_info);

  /* one byte for a type, one or three for a length */
  uint extra2_size= 1 + 1 + create_info->tabledef_version.length;
  if (options_len)
    extra2_size+= 1 + (options_len > 255 ? 3 : 1) + options_len;

  if (part_info)
    extra2_size+= 1 + 1 + hton_name(part_info->default_engine_type)->length;

  if (gis_extra2_len)
    extra2_size+= 1 + (gis_extra2_len > 255 ? 3 : 1) + gis_extra2_len;


  key_buff_length= uint4korr(fileinfo+47);

  frm.length= FRM_HEADER_SIZE;                  // fileinfo;
  frm.length+= extra2_size + 4;                 // mariadb extra2 frm segment

  int2store(fileinfo+4, extra2_size);
  int2store(fileinfo+6, frm.length);            // Position to key information
  frm.length+= key_buff_length;
  frm.length+= reclength;                       // row with default values
  frm.length+= create_info->extra_size;

  filepos= frm.length;
  frm.length+= FRM_FORMINFO_SIZE;               // forminfo
  frm.length+= packed_fields_length(create_fields);
  frm.length+= create_info->expression_length;

  if (frm.length > FRM_MAX_SIZE ||
      create_info->expression_length > UINT_MAX32)
  {
    my_error(ER_TABLE_DEFINITION_TOO_BIG, MYF(0), table);
    DBUG_RETURN(frm);
  }

  frm_ptr= (uchar*) my_malloc(frm.length, MYF(MY_WME | MY_ZEROFILL |
                                              MY_THREAD_SPECIFIC));
  if (!frm_ptr)
    DBUG_RETURN(frm);

  /* write the extra2 segment */
  pos = frm_ptr + 64;
  compile_time_assert(EXTRA2_TABLEDEF_VERSION != '/');
  pos= extra2_write(pos, EXTRA2_TABLEDEF_VERSION,
                    &create_info->tabledef_version);

  if (part_info)
    pos= extra2_write(pos, EXTRA2_DEFAULT_PART_ENGINE,
                      hton_name(part_info->default_engine_type));

  if (options_len)
  {
    *pos++= EXTRA2_ENGINE_TABLEOPTS;
    pos= extra2_write_len(pos, options_len);
    pos= engine_table_options_frm_image(pos, create_info->option_list,
                                        create_fields, keys, key_info);
  }

#ifdef HAVE_SPATIAL
  if (gis_extra2_len)
  {
    *pos= EXTRA2_GIS;
    pos= extra2_write_len(pos+1, gis_extra2_len);
    pos+= gis_field_options_image(pos, create_fields);
  }
#endif /*HAVE_SPATIAL*/

  int4store(pos, filepos); // end of the extra2 segment
  pos+= 4;

  DBUG_ASSERT(pos == frm_ptr + uint2korr(fileinfo+6));
  key_info_length= pack_keys(pos, keys, key_info, data_offset);
  if (key_info_length > UINT_MAX16)
  {
    my_printf_error(ER_CANT_CREATE_TABLE,
                    "Cannot create table %`s: index information is too long. "
                    "Decrease number of indexes or use shorter index names or shorter comments.",
                    MYF(0), table);
    goto err;
  }

  int2store(forminfo+2, frm.length - filepos);
  int4store(fileinfo+10, frm.length);
  fileinfo[26]= (uchar) MY_TEST((create_info->max_rows == 1) &&
                                (create_info->min_rows == 1) && (keys == 0));
  int2store(fileinfo+28,key_info_length);

  if (part_info)
  {
    fileinfo[61]= (uchar) ha_legacy_type(part_info->default_engine_type);
    DBUG_PRINT("info", ("part_db_type = %d", fileinfo[61]));
  }

  memcpy(frm_ptr, fileinfo, FRM_HEADER_SIZE);

  pos+= key_buff_length;
  if (make_empty_rec(thd, pos, create_info->table_options, create_fields,
                     reclength, data_offset))
    goto err;

  pos+= reclength;
  int2store(pos, create_info->connect_string.length);
  pos+= 2;
  if (create_info->connect_string.length)
    memcpy(pos, create_info->connect_string.str, create_info->connect_string.length);
  pos+= create_info->connect_string.length;
  int2store(pos, str_db_type.length);
  pos+= 2;
  memcpy(pos, str_db_type.str, str_db_type.length);
  pos+= str_db_type.length;

  if (part_info)
  {
    char auto_partitioned= part_info->is_auto_partitioned ? 1 : 0;
    int4store(pos, part_info->part_info_len);
    pos+= 4;
    memcpy(pos, part_info->part_info_string, part_info->part_info_len + 1);
    pos+= part_info->part_info_len + 1;
    *pos++= auto_partitioned;
  }
  else
  {
    pos+= 6;
  }

  for (i= 0; i < keys; i++)
  {
    if (key_info[i].parser_name)
    {
      memcpy(pos, key_info[i].parser_name->str, key_info[i].parser_name->length + 1);
      pos+= key_info[i].parser_name->length + 1;
    }
  }
  if (forminfo[46] == (uchar)255)       // New style MySQL 5.5 table comment
  {
    int2store(pos, create_info->comment.length);
    pos+=2;
    memcpy(pos, create_info->comment.str, create_info->comment.length);
    pos+= create_info->comment.length;
  }

  memcpy(frm_ptr + filepos, forminfo, 288);
  pos= frm_ptr + filepos + 288;
  if (pack_fields(&pos, create_fields, create_info, data_offset))
    goto err;

  if (vcols.length())
  {
    /* Store header for packed fields (extra space for future) */
    bzero(pos, FRM_VCOL_NEW_BASE_SIZE);
    pos+= FRM_VCOL_NEW_BASE_SIZE;
    memcpy(pos, vcols.ptr(), vcols.length());
    pos+= vcols.length();
  }

  {
    /* 
      Restore all UCS2 intervals.
      HEX representation of them is not needed anymore.
    */
    List_iterator<Create_field> it(create_fields);
    Create_field *field;
    while ((field=it++))
    {
      if (field->save_interval)
      {
        field->interval= field->save_interval;
        field->save_interval= 0;
      }
    }
  }

  frm.str= frm_ptr;
  DBUG_RETURN(frm);

err:
  my_free(frm_ptr);
  DBUG_RETURN(frm);
}


/**
  Create a frm (table definition) file and the tables

  @param thd           Thread handler
  @param frm           Binary frm image of the table to create
  @param path          Name of file (including database, without .frm)
  @param db            Data base name
  @param table_name    Table name
  @param create_info   create info parameters
  @param file          Handler to use or NULL if only frm needs to be created

  @retval 0   ok
  @retval 1   error
*/

int rea_create_table(THD *thd, LEX_CUSTRING *frm,
                     const char *path, const char *db, const char *table_name,
                     HA_CREATE_INFO *create_info, handler *file,
                     bool no_ha_create_table)
{
  DBUG_ENTER("rea_create_table");

  if (no_ha_create_table)
  {
    if (writefrm(path, db, table_name, true, frm->str, frm->length))
      goto err_frm;
  }

  if (thd->variables.keep_files_on_create)
    create_info->options|= HA_CREATE_KEEP_FILES;

  if (file->ha_create_partitioning_metadata(path, NULL, CHF_CREATE_FLAG))
    goto err_part;

  if (!no_ha_create_table)
  {
    if (ha_create_table(thd, path, db, table_name, create_info, frm))
      goto err_part;
  }

  DBUG_RETURN(0);

err_part:
  file->ha_create_partitioning_metadata(path, NULL, CHF_DELETE_FLAG);
err_frm:
  deletefrm(path);
  DBUG_RETURN(1);
} /* rea_create_table */


/* Pack keyinfo and keynames to keybuff for save in form-file. */

static uint pack_keys(uchar *keybuff, uint key_count, KEY *keyinfo,
                      ulong data_offset)
{
  uint key_parts,length;
  uchar *pos, *keyname_pos;
  KEY *key,*end;
  KEY_PART_INFO *key_part,*key_part_end;
  DBUG_ENTER("pack_keys");

  pos=keybuff+6;
  key_parts=0;
  for (key=keyinfo,end=keyinfo+key_count ; key != end ; key++)
  {
    int2store(pos, (key->flags ^ HA_NOSAME));
    int2store(pos+2,key->key_length);
    pos[4]= (uchar) key->user_defined_key_parts;
    pos[5]= (uchar) key->algorithm;
    int2store(pos+6, key->block_size);
    pos+=8;
    key_parts+=key->user_defined_key_parts;
    DBUG_PRINT("loop", ("flags: %lu  key_parts: %d  key_part: %p",
                        key->flags, key->user_defined_key_parts,
                        key->key_part));
    for (key_part=key->key_part,key_part_end=key_part+key->user_defined_key_parts ;
	 key_part != key_part_end ;
	 key_part++)

    {
      uint offset;
      DBUG_PRINT("loop",("field: %d  startpos: %lu  length: %d",
			 key_part->fieldnr, key_part->offset + data_offset,
                         key_part->length));
      int2store(pos,key_part->fieldnr+1+FIELD_NAME_USED);
      offset= (uint) (key_part->offset+data_offset+1);
      int2store(pos+2, offset);
      pos[4]=0;					// Sort order
      int2store(pos+5,key_part->key_type);
      int2store(pos+7,key_part->length);
      pos+=9;
    }
  }
	/* Save keynames */
  keyname_pos=pos;
  *pos++=(uchar) NAMES_SEP_CHAR;
  for (key=keyinfo ; key != end ; key++)
  {
    uchar *tmp=(uchar*) strmov((char*) pos,key->name);
    *tmp++= (uchar) NAMES_SEP_CHAR;
    *tmp=0;
    pos=tmp;
  }
  *(pos++)=0;
  for (key=keyinfo,end=keyinfo+key_count ; key != end ; key++)
  {
    if (key->flags & HA_USES_COMMENT)
    {
      int2store(pos, key->comment.length);
      uchar *tmp= (uchar*)strnmov((char*) pos+2,key->comment.str,
                                  key->comment.length);
      pos= tmp;
    }
  }

  if (key_count > 127 || key_parts > 127)
  {
    keybuff[0]= (key_count & 0x7f) | 0x80;
    keybuff[1]= key_count >> 7;
    int2store(keybuff+2,key_parts);
  }
  else
  {
    keybuff[0]=(uchar) key_count;
    keybuff[1]=(uchar) key_parts;
    keybuff[2]= keybuff[3]= 0;
  }
  length=(uint) (pos-keyname_pos);
  int2store(keybuff+4,length);
  DBUG_RETURN((uint) (pos-keybuff));
} /* pack_keys */


/**
   Pack the expression (for GENERATED ALWAYS AS, DEFAULT, CHECK)

   The data is stored as:
   1 byte      type (enum_vcol_info_type)
   2 bytes     field_number
   2 bytes     length of expression
   1 byte      length of name
   name
   next bytes  column expression (text data)

   @return 0 ok
   @return 1 error (out of memory or wrong characters in expression)
*/

static bool pack_expression(String *buf, Virtual_column_info *vcol,
                            uint field_nr, enum_vcol_info_type type)
{
  if (buf->reserve(FRM_VCOL_NEW_HEADER_SIZE + vcol->name.length))
    return 1;

  buf->q_append((char) type);
  buf->q_append2b(field_nr);
  size_t len_off= buf->length();
  buf->q_append2b(0); // to be added later
  buf->q_append((char)vcol->name.length);
  buf->q_append(vcol->name.str, vcol->name.length);
  size_t expr_start= buf->length();
  vcol->print(buf);
  size_t expr_len= buf->length() - expr_start;
  if (expr_len >= 65536)
  {
    my_error(ER_EXPRESSION_IS_TOO_BIG, MYF(0), vcol_type_name(type));
    return 1;
  }
  int2store(buf->ptr() + len_off, expr_len);
  return 0;
}


static bool pack_vcols(String *buf, List<Create_field> &create_fields,
                             List<Virtual_column_info> *check_constraint_list)
{
  List_iterator<Create_field> it(create_fields);
  Create_field *field;

  for (uint field_nr=0; (field= it++); field_nr++)
  {
    if (field->vcol_info)
      if (pack_expression(buf, field->vcol_info, field_nr,
                          field->vcol_info->stored_in_db
                          ? VCOL_GENERATED_STORED : VCOL_GENERATED_VIRTUAL))
        return 1;
    if (field->has_default_expression() && !field->has_default_now_unireg_check())
      if (pack_expression(buf, field->default_value, field_nr, VCOL_DEFAULT))
        return 1;
    if (field->check_constraint)
      if (pack_expression(buf, field->check_constraint, field_nr,
                          VCOL_CHECK_FIELD))
        return 1;
  }

  List_iterator<Virtual_column_info> cit(*check_constraint_list);
  Virtual_column_info *check;
  while ((check= cit++))
    if (pack_expression(buf, check, UINT_MAX32, VCOL_CHECK_TABLE))
      return 1;
  return 0;
}


static uint typelib_values_packed_length(const TYPELIB *t)
{
  uint length= 0;
  for (uint i= 0; t->type_names[i]; i++)
  {
    length+= t->type_lengths[i];
    length++; /* Separator */
  }
  return length;
}


/* Make formheader */

static bool pack_header(THD *thd, uchar *forminfo,
                        List<Create_field> &create_fields,
                        HA_CREATE_INFO *create_info, ulong data_offset,
                        handler *file)
{
  uint int_count,int_length, int_parts;
  uint time_stamp_pos,null_fields;
  uint table_options= create_info->table_options;
  size_t length, reclength, totlength, n_length, com_length;
  DBUG_ENTER("pack_header");

  if (create_fields.elements > MAX_FIELDS)
  {
    my_message(ER_TOO_MANY_FIELDS, ER_THD(thd, ER_TOO_MANY_FIELDS), MYF(0));
    DBUG_RETURN(1);
  }

  totlength= 0L;
  reclength= data_offset;
  int_count=int_parts=int_length=time_stamp_pos=null_fields=0;
  com_length= 0;
  n_length=2L;
  create_info->field_check_constraints= 0;

  /* Check fields */
  List_iterator<Create_field> it(create_fields);
  Create_field *field;
  while ((field=it++))
  {
    if (validate_comment_length(thd, &field->comment, COLUMN_COMMENT_MAXLEN,
                                ER_TOO_LONG_FIELD_COMMENT, field->field_name))
       DBUG_RETURN(1);

    totlength+= (size_t)field->length;
    com_length+= field->comment.length;
    /*
      We mark first TIMESTAMP field with NOW() in DEFAULT or ON UPDATE
      as auto-update field.
    */
    if (field->sql_type == MYSQL_TYPE_TIMESTAMP &&
        MTYP_TYPENR(field->unireg_check) != Field::NONE &&
	!time_stamp_pos)
      time_stamp_pos= (uint) field->offset+ (uint) data_offset + 1;
    length=field->pack_length;
    if ((uint) field->offset+ (uint) data_offset+ length > reclength)
      reclength=(uint) (field->offset+ data_offset + length);
    n_length+= (ulong) strlen(field->field_name)+1;
    field->interval_id=0;
    field->save_interval= 0;
    if (field->interval)
    {
      uint old_int_count=int_count;

      if (field->charset->mbminlen > 1)
      {
        /* 
          Escape UCS2 intervals using HEX notation to avoid
          problems with delimiters between enum elements.
          As the original representation is still needed in 
          the function make_empty_rec to create a record of
          filled with default values it is saved in save_interval
          The HEX representation is created from this copy.
        */
        field->save_interval= field->interval;
        field->interval= (TYPELIB*) thd->alloc(sizeof(TYPELIB));
        *field->interval= *field->save_interval; 
        field->interval->type_names= 
          (const char **) thd->alloc(sizeof(char*) * 
                                     (field->interval->count+1));
        field->interval->type_names[field->interval->count]= 0;
        field->interval->type_lengths=
          (uint *) thd->alloc(sizeof(uint) * field->interval->count);
 
        for (uint pos= 0; pos < field->interval->count; pos++)
        {
          char *dst;
          const char *src= field->save_interval->type_names[pos];
          uint hex_length;
          length= field->save_interval->type_lengths[pos];
          hex_length= length * 2;
          field->interval->type_lengths[pos]= hex_length;
          field->interval->type_names[pos]= dst=
            (char*) thd->alloc(hex_length + 1);
          octet2hex(dst, src, length);
        }
      }

      field->interval_id=get_interval_id(&int_count,create_fields,field);
      if (old_int_count != int_count)
      {
        int_length+= typelib_values_packed_length(field->interval);
        int_parts+= field->interval->count + 1;
      }
    }
    if (f_maybe_null(field->pack_flag))
      null_fields++;
    if (field->check_constraint)
      create_info->field_check_constraints++;
  }
  int_length+=int_count*2;			// 255 prefix + 0 suffix

  /* Save values in forminfo */
  if (reclength > (ulong) file->max_record_length())
  {
    my_error(ER_TOO_BIG_ROWSIZE, MYF(0), static_cast<long>(file->max_record_length()));
    DBUG_RETURN(1);
  }

  /* Hack to avoid bugs with small static rows in MySQL */
  reclength= MY_MAX(file->min_record_length(table_options), reclength);
  length= n_length + create_fields.elements*FCOMP + FRM_FORMINFO_SIZE +
          int_length + com_length + create_info->expression_length;
  if (length > 65535L || int_count > 255)
  {
    my_message(ER_TOO_MANY_FIELDS, "Table definition is too large", MYF(0));
    DBUG_RETURN(1);
  }

  bzero((char*)forminfo,FRM_FORMINFO_SIZE);
  int2store(forminfo,length);
  int2store(forminfo+258,create_fields.elements);
  // bytes 260-261 are unused
  int2store(forminfo+262,totlength);
  // bytes 264-265 are unused
  int2store(forminfo+266,reclength);
  int2store(forminfo+268,n_length);
  int2store(forminfo+270,int_count);
  int2store(forminfo+272,int_parts);
  int2store(forminfo+274,int_length);
  int2store(forminfo+276,time_stamp_pos);
  int2store(forminfo+278,80);			/* Columns needed */
  int2store(forminfo+280,22);			/* Rows needed */
  int2store(forminfo+282,null_fields);
  int2store(forminfo+284,com_length);
  int2store(forminfo+286,create_info->expression_length);
  DBUG_RETURN(0);
} /* pack_header */


/* get each unique interval each own id */
static uint get_interval_id(uint *int_count,List<Create_field> &create_fields,
			    Create_field *last_field)
{
  List_iterator<Create_field> it(create_fields);
  Create_field *field;
  TYPELIB *interval=last_field->interval;

  while ((field=it++) != last_field)
  {
    /*
      ENUM/SET columns with equal value lists share a single
      copy of the underlying TYPELIB.
      Fields with different mbminlen can't reuse TYPELIBs, because:
      - mbminlen==1 are written to FRM as is
      - mbminlen>1 are written to FRM in hex-encoded format
    */
    if (field->interval_id &&
        field->interval->count == interval->count &&
        field->charset->mbminlen == last_field->charset->mbminlen)
    {
      const char **a,**b;
      for (a=field->interval->type_names, b=interval->type_names ;
	   *a && !strcmp(*a,*b);
	   a++,b++) ;

      if (! *a)
      {
	return field->interval_id;		// Re-use last interval
      }
    }
  }
  return ++*int_count;				// New unique interval
}


static size_t packed_fields_length(List<Create_field> &create_fields)
{
  Create_field *field;
  size_t length= 0;
  DBUG_ENTER("packed_fields_length");

  List_iterator<Create_field> it(create_fields);
  uint int_count=0;
  while ((field=it++))
  {
    if (field->interval_id > int_count)
    {
      int_count= field->interval_id;
      length++;
      length+= typelib_values_packed_length(field->interval);
      length++;
    }

    length+= FCOMP;
    length+= strlen(field->field_name)+1;
    length+= field->comment.length;
  }
  length+= 2;
  DBUG_RETURN(length);
}

/* Save fields, fieldnames and intervals */

static bool pack_fields(uchar **buff_arg, List<Create_field> &create_fields,
                        HA_CREATE_INFO *create_info,
                        ulong data_offset)
{
  uchar *buff= *buff_arg;
  uint int_count, comment_length= 0;
  Create_field *field;
  DBUG_ENTER("pack_fields");

  /* Write field info */
  List_iterator<Create_field> it(create_fields);
  int_count=0;
  while ((field=it++))
  {
    uint recpos;
    int2store(buff+3, field->length);
    /* The +1 is here becasue the col offset in .frm file have offset 1 */
    recpos= field->offset+1 + (uint) data_offset;
    int3store(buff+5,recpos);
    int2store(buff+8,field->pack_flag);
    buff[10]= (uchar) field->unireg_check;
    buff[12]= (uchar) field->interval_id;
    buff[13]= (uchar) field->sql_type;
    if (field->sql_type == MYSQL_TYPE_GEOMETRY)
    {
      buff[11]= 0;
      buff[14]= (uchar) field->geom_type;
#ifndef HAVE_SPATIAL
      DBUG_ASSERT(0);                           // Should newer happen
#endif
    }
    else if (field->charset) 
    {
      buff[11]= (uchar) (field->charset->number >> 8);
      buff[14]= (uchar) field->charset->number;
    }
    else
    {
      buff[11]= buff[14]= 0;			// Numerical
    }

    int2store(buff+15, field->comment.length);
    comment_length+= field->comment.length;
    set_if_bigger(int_count,field->interval_id);
    buff+= FCOMP;
  }

  /* Write fieldnames */
  *buff++= NAMES_SEP_CHAR;
  it.rewind();
  while ((field=it++))
  {
    buff= (uchar*)strmov((char*) buff, field->field_name);
    *buff++=NAMES_SEP_CHAR;
  }
  *buff++= 0;

  /* Write intervals */
  if (int_count)
  {
    it.rewind();
    int_count=0;
    while ((field=it++))
    {
      if (field->interval_id > int_count)
      {
        unsigned char  sep= 0;
        unsigned char  occ[256];
        uint           i;
        unsigned char *val= NULL;

        bzero(occ, sizeof(occ));

        for (i=0; (val= (unsigned char*) field->interval->type_names[i]); i++)
          for (uint j = 0; j < field->interval->type_lengths[i]; j++)
            occ[(unsigned int) (val[j])]= 1;

        if (!occ[(unsigned char)NAMES_SEP_CHAR])
          sep= (unsigned char) NAMES_SEP_CHAR;
        else if (!occ[(unsigned int)','])
          sep= ',';
        else
        {
          for (uint i=1; i<256; i++)
          {
            if(!occ[i])
            {
              sep= i;
              break;
            }
          }

          if (!sep)
          {
            /* disaster, enum uses all characters, none left as separator */
            my_message(ER_WRONG_FIELD_TERMINATORS,
                       ER(ER_WRONG_FIELD_TERMINATORS),
                       MYF(0));
            DBUG_RETURN(1);
          }
        }

        int_count= field->interval_id;
        *buff++= sep;
        for (int i=0; field->interval->type_names[i]; i++)
        {
          memcpy(buff, field->interval->type_names[i], field->interval->type_lengths[i]);
          buff+= field->interval->type_lengths[i];
          *buff++= sep;
        }
        *buff++= 0;
      }
    }
  }
  if (comment_length)
  {
    it.rewind();
    while ((field=it++))
    {
      if (size_t l= field->comment.length)
      {
        memcpy(buff, field->comment.str, l);
        buff+= l;
      }
    }
  }
  *buff_arg= buff;
  DBUG_RETURN(0);
}

/* save an empty record on start of formfile */

static bool make_empty_rec(THD *thd, uchar *buff, uint table_options,
			   List<Create_field> &create_fields,
			   uint reclength, ulong data_offset)
{
  int error= 0;
  uint null_count;
  uchar *null_pos;
  TABLE table;
  TABLE_SHARE share;
  Create_field *field;
  Check_level_instant_set old_count_cuted_fields(thd, CHECK_FIELD_WARN);
  Abort_on_warning_instant_set old_abort_on_warning(thd, 0);
  DBUG_ENTER("make_empty_rec");

  /* We need a table to generate columns for default values */
  bzero((char*) &table, sizeof(table));
  bzero((char*) &share, sizeof(share));
  table.s= &share;

  table.in_use= thd;

  null_count=0;
  if (!(table_options & HA_OPTION_PACK_RECORD))
  {
    null_count++;			// Need one bit for delete mark
    *buff|= 1;
  }
  null_pos= buff;

  List_iterator<Create_field> it(create_fields);
  while ((field=it++))
  {
    /* regfield don't have to be deleted as it's allocated on THD::mem_root */
    Field *regfield= make_field(&share, thd->mem_root,
                                buff+field->offset + data_offset,
                                (uint32)field->length,
                                null_pos + null_count / 8,
                                null_count & 7,
                                field->pack_flag,
                                field->sql_type,
                                field->charset,
                                field->geom_type, field->srid,
                                field->unireg_check,
                                field->save_interval ? field->save_interval
                                                     : field->interval,
                                field->field_name);
    if (!regfield)
    {
      error= 1;
      goto err;                                 // End of memory
    }

    /* save_in_field() will access regfield->table->in_use */
    regfield->init(&table);

    if (!(field->flags & NOT_NULL_FLAG))
    {
      *regfield->null_ptr|= regfield->null_bit;
      null_count++;
    }

    if (field->sql_type == MYSQL_TYPE_BIT && !f_bit_as_char(field->pack_flag))
      null_count+= field->length & 7;

    if (field->default_value && !field->default_value->flags &&
        (!(field->flags & BLOB_FLAG) || field->sql_type == MYSQL_TYPE_GEOMETRY))
    {
      Item *expr= field->default_value->expr;

      int res= !expr->fixed && // may be already fixed if ALTER TABLE
                expr->fix_fields(thd, &expr);
      if (!res)
        res= expr->save_in_field(regfield, 1);
      if (!res && (field->flags & BLOB_FLAG))
        regfield->reset();

      /* If not ok or warning of level 'note' */
      if (res != 0 && res != 3)
      {
        my_error(ER_INVALID_DEFAULT, MYF(0), regfield->field_name);
        error= 1;
        delete regfield; //To avoid memory leak
        goto err;
      }
      delete regfield; //To avoid memory leak
    }
    else if (regfield->real_type() == MYSQL_TYPE_ENUM &&
	     (field->flags & NOT_NULL_FLAG))
    {
      regfield->set_notnull();
      regfield->store((longlong) 1, TRUE);
    }
    else
      regfield->reset();
  }
  DBUG_ASSERT(data_offset == ((null_count + 7) / 8));

  /*
    We need to set the unused bits to 1. If the number of bits is a multiple
    of 8 there are no unused bits.
  */
  if (null_count & 7)
    *(null_pos + null_count / 8)|= ~(((uchar) 1 << (null_count & 7)) - 1);

err:
  DBUG_RETURN(error);
} /* make_empty_rec */
