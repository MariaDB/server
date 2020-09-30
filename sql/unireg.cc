/*
   Copyright (c) 2000, 2011, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB Corporation.

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

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_partition.h"                      // struct partition_info
#include "sql_class.h"                  // THD, Internal_error_handler
#include "create_options.h"
#include "discover.h"
#include "datadict.h"
#include "table_cache.h"                // Share_acquire
#include <m_ctype.h>

#define FCOMP			17		/* Bytes for a packed field */

/* threshold for safe_alloca */
#define ALLOCA_THRESHOLD       2048

static uint pack_keys(uchar *,uint, KEY *, ulong, uint);
static bool pack_header(THD *, uchar *, List<Create_field> &, HA_CREATE_INFO *,
                        ulong, handler *);
static bool pack_vcols(String *, List<Create_field> &, List<Virtual_column_info> *);
static uint get_interval_id(uint *,List<Create_field> &, Create_field *);
static bool pack_fields(uchar **, List<Create_field> &, HA_CREATE_INFO*,
                        ulong);
static size_t packed_fields_length(List<Create_field> &);
static bool make_empty_rec(THD *, uchar *, uint, List<Create_field> &, uint,
                           ulong);

// TODO: move extra_ functions to datadict.cc
/*
  write the length as
  if (  0 < length <= 255)      one byte
  if (256 < length < 65535)    zero byte, then two bytes, low-endian
*/
uchar *
extra2_write_len(uchar *pos, size_t len)
{
  if (len <= 255)
    *pos++= (uchar)len;
  else
  {
    DBUG_ASSERT(len <= 0xffff - FRM_HEADER_SIZE - 8);
    *pos++= 0;
    int2store(pos, len);
    pos+= 2;
  }
  return pos;
}

uchar *
extra2_write_str(uchar *pos, const LEX_CSTRING &str)
{
  pos= extra2_write_len(pos, str.length);
  memcpy(pos, str.str, str.length);
  return pos + str.length;
}

static uchar* extra2_write_str(uchar *pos, const Binary_string *str)
{
  pos= extra2_write_len(pos, str->length());
  memcpy(pos, str->ptr(), str->length());
  return pos + str->length();
}

uchar *
extra2_write_field_properties(uchar *pos, List<Create_field> &create_fields)
{
  List_iterator<Create_field> it(create_fields);
  *pos++= EXTRA2_FIELD_FLAGS;
  /*
   always first 2  for field visibility
  */
  pos= extra2_write_len(pos, create_fields.elements);
  while (Create_field *cf= it++)
  {
    uchar flags= cf->invisible;
    if (cf->flags & VERS_UPDATE_UNVERSIONED_FLAG)
      flags|= VERS_OPTIMIZED_UPDATE;
    *pos++= flags;
  }
  return pos;
}


static uint16
get_fieldno_by_name(HA_CREATE_INFO *create_info, List<Create_field> &create_fields,
                    const Lex_ident &field_name)
{
  List_iterator<Create_field> it(create_fields);
  Create_field *sql_field = NULL;

  DBUG_ASSERT(field_name);

  for (unsigned field_no = 0; (sql_field = it++); ++field_no)
  {
    if (field_name.streq(sql_field->field_name))
    {
      DBUG_ASSERT(field_no < NO_CACHED_FIELD_INDEX);
      return (field_index_t) field_no;
    }
  }

  DBUG_ASSERT(0); /* Not Reachable */
  return 0;
}

static inline
bool has_extra2_field_flags(List<Create_field> &create_fields)
{
  List_iterator<Create_field> it(create_fields);
  while (Create_field *f= it++)
  {
    if (f->invisible)
      return true;
    if (f->flags & VERS_UPDATE_UNVERSIONED_FLAG)
      return true;
  }
  return false;
}

static uint gis_field_options_image(uchar *buff,
                                    List<Create_field> &create_fields)
{
  uint image_size= 0;
  List_iterator<Create_field> it(create_fields);
  Create_field *field;
  while ((field= it++))
  {
    if (field->real_field_type() != MYSQL_TYPE_GEOMETRY)
      continue;
    uchar *cbuf= buff ? buff + image_size : NULL;
    image_size+= field->type_handler()->
                   Column_definition_gis_options_image(cbuf, *field);
  }
  return image_size;
}


class Field_data_type_info_image: public BinaryStringBuffer<512>
{
  static uchar *store_length(uchar *pos, ulonglong length)
  {
    return net_store_length(pos, length);
  }
  static uchar *store_string(uchar *pos, const Binary_string *str)
  {
    pos= store_length(pos, str->length());
    memcpy(pos, str->ptr(), str->length());
    return pos + str->length();
  }
public:
  Field_data_type_info_image() { }
  bool append(uint fieldnr, const Column_definition &def)
  {
    BinaryStringBuffer<64> type_info;
    if (def.type_handler()->
              Column_definition_data_type_info_image(&type_info, def) ||
        type_info.length() > 0xFFFF/*Some reasonable limit*/)
      return true; // Error
    if (!type_info.length())
      return false;
    size_t need_length= net_length_size(fieldnr) +
                        net_length_size(type_info.length()) +
                        type_info.length();
    if (reserve(need_length))
      return true; // Error
    uchar *pos= (uchar *) end();
    pos= store_length(pos, fieldnr);
    pos= store_string(pos, &type_info);
    size_t new_length= (const char *) pos - ptr();
    DBUG_ASSERT(new_length < alloced_length());
    length((uint32) new_length);
    return false;
  }
  bool append(List<Create_field> &fields)
  {
    uint fieldnr= 0;
    Create_field *field;
    List_iterator<Create_field> it(fields);
    for (field= it++; field; field= it++, fieldnr++)
    {
      if (append(fieldnr, *field))
        return true; // Error
    }
    return false;
  }
};


/**
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

LEX_CUSTRING build_frm_image(THD *thd, const LEX_CSTRING &table,
                             HA_CREATE_INFO *create_info,
                             List<Create_field> &create_fields,
                             uint keys, KEY *key_info,  FK_list &foreign_keys,
                             FK_list &referenced_keys,
                             handler *db_file)
{
  LEX_CSTRING str_db_type;
  uint reclength, key_info_length, i;
  ulong key_buff_length;
  ulong data_offset;
  uint options_len;
  uint gis_extra2_len= 0;
  size_t period_info_len= create_info->period_info.name
                          ? extra2_str_size(create_info->period_info.name.length)
                            + extra2_str_size(create_info->period_info.constr->name.length)
                            + 2 * frm_fieldno_size
                          : 0;
  size_t without_overlaps_len= frm_keyno_size * (create_info->period_info.unique_keys + 1);
  uint e_unique_hash_extra_parts= 0;
  uchar fileinfo[FRM_HEADER_SIZE],forminfo[FRM_FORMINFO_SIZE];
  const partition_info *part_info= IF_PARTITIONING(thd->work_part_info, 0);
  bool error;
  uchar *frm_ptr, *pos;
  LEX_CUSTRING frm= {0,0};
  StringBuffer<MAX_FIELD_WIDTH> vcols;
  Field_data_type_info_image field_data_type_info_image;
  Foreign_key_io foreign_key_io;
  DBUG_ENTER("build_frm_image");

 /* If fixed row records, we need one bit to check for deleted rows */
  if (!(create_info->table_options & HA_OPTION_PACK_RECORD))
    create_info->null_bits++;
  data_offset= (create_info->null_bits + 7) / 8;

  sql_mode_t save_sql_mode= thd->variables.sql_mode;
  thd->variables.sql_mode &= ~MODE_ANSI_QUOTES;
  error= pack_vcols(&vcols, create_fields, create_info->check_constraint_list);
  thd->variables.sql_mode= save_sql_mode;

  if (unlikely(error))
    DBUG_RETURN(frm);

  if (vcols.length())
    create_info->expression_length= vcols.length() + FRM_VCOL_NEW_BASE_SIZE;

  error= pack_header(thd, forminfo, create_fields, create_info,
                     (ulong)data_offset, db_file);
  if (unlikely(error))
    DBUG_RETURN(frm);

  reclength= uint2korr(forminfo+266);

  /* Calculate extra data segment length */
  str_db_type= *hton_name(create_info->db_type);
  /* str_db_type */
  create_info->extra_size= (uint)(2 + str_db_type.length +
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
    create_info->extra_size+= (uint)part_info->part_info_len;

  for (i= 0; i < keys; i++)
  {
    if (key_info[i].parser_name)
      create_info->extra_size+= (uint)key_info[i].parser_name->length + 1;
  }

  options_len= engine_table_options_frm_length(create_info->option_list,
                                               create_fields,
                                               keys, key_info);
  gis_extra2_len= gis_field_options_image(NULL, create_fields);
  DBUG_PRINT("info", ("Options length: %u", options_len));

  if (field_data_type_info_image.append(create_fields))
  {
    my_printf_error(ER_CANT_CREATE_TABLE,
                    "Cannot create table %`s: "
                    "Building the field data type info image failed.",
                    MYF(0), table.str);
    DBUG_RETURN(frm);
  }
  if (field_data_type_info_image.length() > 0xffff - FRM_HEADER_SIZE - 8)
  {
    my_printf_error(ER_CANT_CREATE_TABLE,
                    "Cannot create table %`s: "
                    "field data type info image is too large. "
                    "Decrease the number of columns with "
                    "extended data types.",
                    MYF(0), table.str);
    DBUG_RETURN(frm);
  }
  if (foreign_key_io.store(foreign_keys, referenced_keys))
  {
    my_printf_error(ER_CANT_CREATE_TABLE,
                    "Cannot create table %`s: "
                    "Building the foreign key info image failed.",
                    MYF(0), table.str);
    DBUG_RETURN(frm);
  }
  if (foreign_key_io.length() > 0xffff - FRM_HEADER_SIZE - 8)
  {
    my_printf_error(ER_CANT_CREATE_TABLE,
                    "Cannot create table %`s: "
                    "foreign key info image is too large.",
                    MYF(0), table.str);
    DBUG_RETURN(frm);
  }

  DBUG_PRINT("info", ("Field data type info length: %u",
                      (uint) field_data_type_info_image.length()));
  DBUG_EXECUTE_IF("frm_data_type_info",
                  push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                  ER_UNKNOWN_ERROR,
                  "build_frm_image: Field data type info length: %u",
                  (uint) field_data_type_info_image.length()););

  if (validate_comment_length(thd, &create_info->comment, TABLE_COMMENT_MAXLEN,
                              ER_TOO_LONG_TABLE_COMMENT, table.str))
     DBUG_RETURN(frm);
  /*
    If table comment is longer than TABLE_COMMENT_INLINE_MAXLEN bytes,
    store the comment in an extra segment (up to TABLE_COMMENT_MAXLEN bytes).
    Pre 5.5, the limit was 60 characters, with no extra segment-handling.
  */
  if (create_info->comment.length > TABLE_COMMENT_INLINE_MAXLEN)
  {
    forminfo[46]=255;
    create_info->extra_size+= 2 + (uint)create_info->comment.length;
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
  size_t extra2_size= 1 + extra2_str_size(create_info->tabledef_version.length);
  if (options_len)
    extra2_size+= 1 + extra2_str_size(options_len);

  if (part_info)
    extra2_size+= 1 + extra2_str_size(hton_name(part_info->default_engine_type)->length);

  if (gis_extra2_len)
    extra2_size+= 1 + extra2_str_size(gis_extra2_len);

  if (field_data_type_info_image.length())
    extra2_size+= 1 + extra2_str_size(field_data_type_info_image.length());

  if (foreign_key_io.length())
    extra2_size+= 1 + extra2_str_size(foreign_key_io.length());

  if (create_info->versioned())
  {
    extra2_size+= 1 + extra2_str_size(2 * frm_fieldno_size);
  }

  if (create_info->period_info.name)
  {
    extra2_size+= 2 + extra2_str_size(period_info_len)
                    + extra2_str_size(without_overlaps_len);
  }

  bool has_extra2_field_flags_= has_extra2_field_flags(create_fields);
  if (has_extra2_field_flags_)
  {
    extra2_size+= 1 + extra2_str_size(create_fields.elements);
  }

  for (i= 0; i < keys; i++)
    if (key_info[i].algorithm == HA_KEY_ALG_LONG_HASH)
      e_unique_hash_extra_parts++;
  key_buff_length= uint4korr(fileinfo+47);

  frm.length= FRM_HEADER_SIZE;                  // fileinfo;
  frm.length+= extra2_size + 4;                 // mariadb extra2 frm segment

  int2store(fileinfo+4, extra2_size);
  int2store(fileinfo+6, frm.length);            // Position to key information
  frm.length+= key_buff_length;
  frm.length+= reclength;                       // row with default values
  frm.length+= create_info->extra_size;

  const size_t forminfo_pos= frm.length;
  frm.length+= FRM_FORMINFO_SIZE;               // forminfo
  frm.length+= packed_fields_length(create_fields);
  frm.length+= create_info->expression_length;

  if (frm.length > FRM_MAX_SIZE ||
      create_info->expression_length > UINT_MAX32)
  {
    my_error(ER_TABLE_DEFINITION_TOO_BIG, MYF(0), table.str);
    DBUG_RETURN(frm);
  }

  frm_ptr= (uchar*) my_malloc(PSI_INSTRUMENT_ME, frm.length,
                              MYF(MY_WME | MY_ZEROFILL | MY_THREAD_SPECIFIC));
  if (!frm_ptr)
    DBUG_RETURN(frm);

  /* write the extra2 segment */
  pos = frm_ptr + FRM_HEADER_SIZE;
  compile_time_assert(EXTRA2_TABLEDEF_VERSION != '/');
  pos= extra2_write(pos, EXTRA2_TABLEDEF_VERSION,
                    create_info->tabledef_version);

  if (part_info)
    pos= extra2_write(pos, EXTRA2_DEFAULT_PART_ENGINE,
                      *hton_name(part_info->default_engine_type));

  if (options_len)
  {
    *pos++= EXTRA2_ENGINE_TABLEOPTS;
    pos= extra2_write_len(pos, options_len);
    pos= engine_table_options_frm_image(pos, create_info->option_list,
                                        create_fields, keys, key_info);
  }

  if (gis_extra2_len)
  {
    *pos= EXTRA2_GIS;
    pos= extra2_write_len(pos+1, gis_extra2_len);
    pos+= gis_field_options_image(pos, create_fields);
  }

  if (field_data_type_info_image.length())
  {
    *pos= EXTRA2_FIELD_DATA_TYPE_INFO;
    pos= extra2_write_str(pos + 1, &field_data_type_info_image);
  }

  if (foreign_key_io.length())
  {
    *pos= EXTRA2_FOREIGN_KEY_INFO;
    pos= extra2_write_str(pos + 1, foreign_key_io.lex_cstring());
  }

  if (create_info->versioned())
  {
    *pos++= EXTRA2_PERIOD_FOR_SYSTEM_TIME;
    *pos++= 2 * frm_fieldno_size;
    store_frm_fieldno(pos, get_fieldno_by_name(create_info, create_fields,
                                       create_info->vers_info.as_row.start));
    pos+= frm_fieldno_size;
    store_frm_fieldno(pos, get_fieldno_by_name(create_info, create_fields,
                                       create_info->vers_info.as_row.end));
    pos+= frm_fieldno_size;
  }

  // PERIOD
  if (create_info->period_info.is_set())
  {
    *pos++= EXTRA2_APPLICATION_TIME_PERIOD;
    pos= extra2_write_len(pos, period_info_len);
    pos= extra2_write_str(pos, create_info->period_info.name);
    pos= extra2_write_str(pos, create_info->period_info.constr->name);

    store_frm_fieldno(pos, get_fieldno_by_name(create_info, create_fields,
                                       create_info->period_info.period.start));
    pos+= frm_fieldno_size;
    store_frm_fieldno(pos, get_fieldno_by_name(create_info, create_fields,
                                       create_info->period_info.period.end));
    pos+= frm_fieldno_size;

    *pos++= EXTRA2_PERIOD_WITHOUT_OVERLAPS;
    pos= extra2_write_len(pos, without_overlaps_len);
    store_frm_keyno(pos, create_info->period_info.unique_keys);
    pos+= frm_keyno_size;
    for (uint key= 0; key < keys; key++)
    {
      if (key_info[key].without_overlaps)
      {
        store_frm_keyno(pos, key);
        pos+= frm_keyno_size;
      }
    }
  }

  if (has_extra2_field_flags_)
    pos= extra2_write_field_properties(pos, create_fields);

  int4store(pos, forminfo_pos); // end of the extra2 segment
  pos+= 4;

  DBUG_ASSERT(pos == frm_ptr + uint2korr(fileinfo+6));
  key_info_length= pack_keys(pos, keys, key_info, data_offset, e_unique_hash_extra_parts);
  if (key_info_length > UINT_MAX16)
  {
    my_printf_error(ER_CANT_CREATE_TABLE,
                    "Cannot create table %`s: index information is too long. "
                    "Decrease number of indexes or use shorter index names or shorter comments.",
                    MYF(0), table.str);
    goto err;
  }

  int2store(forminfo+2, frm.length - forminfo_pos);
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

  memcpy(frm_ptr + forminfo_pos, forminfo, FRM_FORMINFO_SIZE);
  pos= frm_ptr + forminfo_pos + FRM_FORMINFO_SIZE;
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


/* Pack keyinfo and keynames to keybuff for save in form-file. */

static uint pack_keys(uchar *keybuff, uint key_count, KEY *keyinfo,
                      ulong data_offset, uint e_unique_hash_extra_parts)
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
    uchar *tmp=(uchar*) strmov((char*) pos,key->name.str);
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

  key_parts+= e_unique_hash_extra_parts;
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
  buf->q_append(&vcol->name);
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
    if (field->vcol_info && field->vcol_info->expr)
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
                                ER_TOO_LONG_FIELD_COMMENT,
                                field->field_name.str))
       DBUG_RETURN(1);

    totlength+= (size_t)field->length;
    com_length+= field->comment.length;
    /*
      We mark first TIMESTAMP field with NOW() in DEFAULT or ON UPDATE
      as auto-update field.
    */
    if (field->real_field_type() == MYSQL_TYPE_TIMESTAMP &&
        field->unireg_check != Field::NONE &&
	!time_stamp_pos)
      time_stamp_pos= (uint) field->offset+ (uint) data_offset + 1;
    length=field->pack_length;
    if ((uint) field->offset+ (uint) data_offset+ length > reclength)
      reclength=(uint) (field->offset+ data_offset + length);
    n_length+= field->field_name.length + 1;
    field->interval_id=0;
    field->save_interval= 0;
    if (field->interval)
    {
      uint old_int_count=int_count;

      if (field->charset->mbminlen > 1)
      {
        TYPELIB *tmpint;
        /* 
          Escape UCS2 intervals using HEX notation to avoid
          problems with delimiters between enum elements.
          As the original representation is still needed in 
          the function make_empty_rec to create a record of
          filled with default values it is saved in save_interval
          The HEX representation is created from this copy.
        */
        uint count= field->interval->count;
        field->save_interval= field->interval;
        field->interval= tmpint= (TYPELIB*) thd->alloc(sizeof(TYPELIB));
        *tmpint= *field->save_interval;
        tmpint->type_names=
          (const char **) thd->alloc(sizeof(char*) *
                                     (count + 1));
        tmpint->type_lengths= (uint *) thd->alloc(sizeof(uint) * (count + 1));
        tmpint->type_names[count]= 0;
        tmpint->type_lengths[count]= 0;

        for (uint pos= 0; pos < field->interval->count; pos++)
        {
          char *dst;
          const char *src= field->save_interval->type_names[pos];
          size_t hex_length;
          length= field->save_interval->type_lengths[pos];
          hex_length= length * 2;
          tmpint->type_lengths[pos]= (uint) hex_length;
          tmpint->type_names[pos]= dst= (char*) thd->alloc(hex_length + 1);
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
  const TYPELIB *interval= last_field->interval;

  while ((field=it++) != last_field)
  {
    if (field->interval_id && field->interval->count == interval->count)
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
    length+= field->field_name.length + 1;
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
  uint int_count;
  size_t comment_length= 0;
  Create_field *field;
  DBUG_ENTER("pack_fields");

  /* Write field info */
  List_iterator<Create_field> it(create_fields);
  int_count=0;
  while ((field=it++))
  {
    uint recpos;
    /* The +1 is here becasue the col offset in .frm file have offset 1 */
    recpos= field->offset+1 + (uint) data_offset;
    int3store(buff+5,recpos);
    buff[12]= (uchar) field->interval_id;
    buff[13]= (uchar) field->type_handler()->real_field_type();
    field->type_handler()->Column_definition_attributes_frm_pack(field, buff);
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
    buff= (uchar*)strmov((char*) buff, field->field_name.str);
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


static bool make_empty_rec_store_default(THD *thd, Field *regfield,
                                         Create_field *field)
{
  Virtual_column_info *default_value= field->default_value;
  if (!field->vers_sys_field() && default_value && !default_value->flags)
  {
    Item *expr= default_value->expr;
    // may be already fixed if ALTER TABLE
    if (expr->fix_fields_if_needed(thd, &expr))
      return true;
    DBUG_ASSERT(expr == default_value->expr); // Should not change
    if (regfield->make_empty_rec_store_default_value(thd, expr))
    {
      my_error(ER_INVALID_DEFAULT, MYF(0), regfield->field_name.str);
      return true;
    }
    return false;
  }
  regfield->make_empty_rec_reset(thd);
  return false;
}


/* save an empty record on start of formfile */

static bool make_empty_rec(THD *thd, uchar *buff, uint table_options,
			   List<Create_field> &create_fields,
			   uint reclength, ulong data_offset)
{
  int error= false;
  uint null_count;
  uchar *null_pos;
  TABLE table;
  TABLE_SHARE share;
  Create_field *field;
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
  Check_level_instant_set check_level_save(thd, CHECK_FIELD_WARN);
  while ((field=it++))
  {
    Record_addr addr(buff + field->offset + data_offset,
                     null_pos + null_count / 8, null_count & 7);
    Column_definition_attributes tmp(*field);
    tmp.interval= field->save_interval ?
                  field->save_interval : field->interval;
    /* regfield don't have to be deleted as it's allocated on THD::mem_root */
    Field *regfield= tmp.make_field(&share, thd->mem_root, &addr,
                                    field->type_handler(),
                                    &field->field_name,
                                    field->flags);
    if (!regfield)
    {
      error= true;
      goto err;                                 // End of memory
    }

    /* save_in_field() will access regfield->table->in_use */
    regfield->init(&table);

    if (!(field->flags & NOT_NULL_FLAG))
    {
      *regfield->null_ptr|= regfield->null_bit;
      null_count++;
    }

    if (field->real_field_type() == MYSQL_TYPE_BIT &&
        !f_bit_as_char(field->pack_flag))
      null_count+= field->length & 7;

    error= make_empty_rec_store_default(thd, regfield, field);
    delete regfield; // Avoid memory leaks
    if (error)
      goto err;
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

ulonglong Foreign_key_io::fk_size(FK_info &fk)
{
  ulonglong store_size= 0;
  store_size+= string_size(fk.foreign_id);
  store_size+= string_size(fk.referenced_db);
  store_size+= string_size(fk.referenced_table);
  store_size+= net_length_size(fk.update_method);
  store_size+= net_length_size(fk.delete_method);
  store_size+= net_length_size(fk.foreign_fields.elements);
  DBUG_ASSERT(fk.foreign_fields.elements == fk.referenced_fields.elements);
  List_iterator_fast<Lex_cstring> ref_it(fk.referenced_fields);
  for (Lex_cstring &fcol: fk.foreign_fields)
  {
    store_size+= string_size(fcol);
    Lex_cstring *ref_col= ref_it++;
    store_size+= string_size(*ref_col);
  }
  return store_size;
}

ulonglong Foreign_key_io::hint_size(FK_info &rk)
{
  ulonglong store_size= 0;
  DBUG_ASSERT(rk.foreign_db.str);
  DBUG_ASSERT(rk.foreign_table.str);
  store_size+= string_size(rk.foreign_db);
  store_size+= string_size(rk.foreign_table);
  return store_size;
}

void Foreign_key_io::store_fk(FK_info &fk, uchar *&pos)
{
#ifndef DBUG_OFF
  uchar *old_pos= pos;
#endif
  pos= store_string(pos, fk.foreign_id);
  pos= store_string(pos, fk.referenced_db, true);
  pos= store_string(pos, fk.referenced_table);
  pos= store_length(pos, fk.update_method);
  pos= store_length(pos, fk.delete_method);
  pos= store_length(pos, fk.foreign_fields.elements);
  DBUG_ASSERT(fk.foreign_fields.elements == fk.referenced_fields.elements);
  List_iterator_fast<Lex_cstring> ref_it(fk.referenced_fields);
  for (Lex_cstring &fcol: fk.foreign_fields)
  {
    pos= store_string(pos, fcol);
    Lex_cstring *ref_col= ref_it++;
    pos= store_string(pos, *ref_col);
  }
  DBUG_ASSERT(pos - old_pos == (long int)fk_size(fk));
}

bool Foreign_key_io::store(FK_list &foreign_keys, FK_list &referenced_keys)
{
  DBUG_EXECUTE_IF("fk_skip_store", return false;);

  ulonglong fk_count= 0;
  mbd::set<Table_name> hints;
  bool inserted;

  if (foreign_keys.is_empty() && referenced_keys.is_empty())
    return false;

  ulonglong store_size= net_length_size(fk_io_version);
  for (FK_info &fk: foreign_keys)
  {
    fk_count++;
    store_size+= fk_size(fk);
  }
  store_size+= net_length_size(fk_count);

  for (FK_info &rk: referenced_keys)
  {
    // NB: we do not store hints on self-refs, they are stored from foreign_keys.
    if (rk.self_ref())
      continue;

    if (!hints.insert(Table_name(rk.foreign_db, rk.foreign_table), &inserted))
      return true;

    if (!inserted)
      continue;

    store_size+= hint_size(rk);
  }
  store_size+= net_length_size(referenced_keys.elements);
  store_size+= net_length_size(0); // Reserved: stored referenced keys count
  store_size+= net_length_size(hints.size());

  if (reserve(store_size))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return true;
  }
  uchar *pos= (uchar *) end();
  pos= store_length(pos, fk_io_version);

  pos= store_length(pos, fk_count);
  for (FK_info &fk: foreign_keys)
    store_fk(fk, pos);

  pos= store_length(pos, referenced_keys.elements);
  pos= store_length(pos, 0); // Reserved: stored referenced keys count
  pos= store_length(pos, hints.size());
  for (const Table_name &hint: hints)
  {
    pos= store_string(pos, hint.db);
    pos= store_string(pos, hint.name);
  }

  size_t new_length= (char *) pos - ptr();
  DBUG_ASSERT(new_length < alloced_length());
  length((uint32) new_length);
  return false;
}

bool Foreign_key_io::parse(THD *thd, TABLE_SHARE *s, LEX_CUSTRING& image)
{
  Pos p(image);
  size_t version, fk_count, rk_count, stored_rk_count, hint_count;
  Lex_cstring hint_db, hint_table;

  if (read_length(version, p))
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_CANNOT_ADD_FOREIGN,
                        "Foreign_key_io failed to read binary data version");
    return true;
  }
  if (read_length(fk_count, p))
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_CANNOT_ADD_FOREIGN,
                        "Foreign_key_io failed to read foreign key count");
    return true;
  }
  if (version > fk_io_version)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_CANNOT_ADD_FOREIGN,
                        "Foreign_key_io does not support %d version of binary data", version);
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_CANNOT_ADD_FOREIGN,
                        "Foreign_key_io max supported version is %d", fk_io_version);
    return true;
  }
  for (uint i= 0; i < fk_count; ++i)
  {
    FK_info *dst= new (&s->mem_root) FK_info();
    if (s->foreign_keys.push_back(dst, &s->mem_root))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return true;
    }
    if (read_string(dst->foreign_id, &s->mem_root, p))
      return true;
    dst->foreign_db= s->db;
    dst->foreign_table= s->table_name;
    if (read_string(dst->referenced_db, &s->mem_root, p))
      return true;
    if (!dst->referenced_db.length)
      dst->referenced_db.strdup(&s->mem_root, s->db);
    if (read_string(dst->referenced_table, &s->mem_root, p))
      return true;
    size_t update_method, delete_method;
    if (read_length(update_method, p))
      return true;
    if (read_length(delete_method, p))
      return true;
    if (update_method > FK_OPTION_SET_DEFAULT || delete_method > FK_OPTION_SET_DEFAULT)
      return true;
    dst->update_method= (enum_fk_option) update_method;
    dst->delete_method= (enum_fk_option) delete_method;
    size_t col_count;
    if (read_length(col_count, p))
      return true;
    for (uint j= 0; j < col_count; ++j)
    {
      Lex_cstring *field_name= new (&s->mem_root) Lex_cstring;
      if (!field_name ||
          dst->foreign_fields.push_back(field_name, &s->mem_root))
      {
        my_error(ER_OUT_OF_RESOURCES, MYF(0));
        return true;
      }
      if (read_string(*field_name, &s->mem_root, p))
        return true;
      field_name= new (&s->mem_root) Lex_cstring;
      if (!field_name ||
          dst->referenced_fields.push_back(field_name, &s->mem_root))
      {
        my_error(ER_OUT_OF_RESOURCES, MYF(0));
        return true;
      }
      if (read_string(*field_name, &s->mem_root, p))
        return true;
    }
    /* If it is self-reference we also push to referenced_keys: */
    if (dst->self_ref() && s->referenced_keys.push_back(dst, &s->mem_root))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return true;
    }
  }
  if (read_length(rk_count, p))
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_CANNOT_ADD_FOREIGN,
                        "Foreign_key_io failed to read referenced keys count");
    return true;
  }
  if (read_length(stored_rk_count, p))
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_CANNOT_ADD_FOREIGN,
                        "Foreign_key_io failed to read referenced keys count");
    return true;
  }
  if (stored_rk_count > 0)
  {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0), "stored referenced keys");
    DBUG_ASSERT(0);
    return true;
  }
  if (read_length(hint_count, p))
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_CANNOT_ADD_FOREIGN,
                        "Foreign_key_io failed to read reference hints count");
    return true;
  }

  const bool shallow_hints= s->tmp_table || s->open_flags & GTS_FK_SHALLOW_HINTS;

  for (uint i= 0; i < hint_count; ++i)
  {
    if (read_string(hint_db, &s->mem_root, p))
      return true;
    if (read_string(hint_table, &s->mem_root, p))
      return true;
    // NB: we do not store self-references into referenced hints
    DBUG_ASSERT(cmp_table(hint_db, s->db) || cmp_table(hint_table, s->table_name));
    if (shallow_hints)
    {
      /* For DROP TABLE we don't need full reference resolution. We just need
         to know if anything from the outside references the dropped table.
         Temporary share may have FK columns renamed so we can't resolve by
         column names.*/
      FK_info *dst= new (&s->mem_root) FK_info;
      dst->foreign_db= hint_db;
      dst->foreign_table= hint_table;
      dst->referenced_db= s->db;
      dst->referenced_table= s->table_name;
      if (s->referenced_keys.push_back(dst, &s->mem_root))
      {
        my_error(ER_OUT_OF_RESOURCES, MYF(0));
        return true;
      }
      continue;
    }
    TABLE_SHARE *fs= NULL;
    for (TABLE_SHARE &c: thd->fk_circular_check)
    {
      if (!cmp_table(c.db, hint_db) && !cmp_table(c.table_name, hint_table))
      {
        fs= &c;
        break;
      }
    }
    TABLE_LIST tl;
    Share_acquire sa;
    tl.init_one_table(&hint_db, &hint_table, &hint_table, TL_IGNORE);
    if (!fs)
    {
      if (thd->fk_circular_check.push_front(s))
      {
        my_error(ER_OUT_OF_RESOURCES, MYF(0));
        return true;
      }
      sa.acquire(thd, tl);
      thd->fk_circular_check.pop();
      if (!sa.share)
      {
        DBUG_ASSERT(thd->is_error());
        if (thd->get_stmt_da()->sql_errno() == ER_NO_SUCH_TABLE)
        {
          thd->clear_error();
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_CANNOT_ADD_FOREIGN,
                              "Reference hint to non-existent table `%s.%s` skipped",
                              hint_db.str, hint_table.str);
          rk_count--;
          continue;
        }
        return true;
      }
      fs= sa.share;
    }
    size_t refs_was= s->referenced_keys.elements;
    if (s->fk_resolve_referenced_keys(thd, fs))
      return true;
    if (s->referenced_keys.elements == refs_was)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_CANNOT_ADD_FOREIGN,
                          "Table `%s.%s` has no foreign keys to `%s.%s`",
                          hint_db.str, hint_table.str, s->db.str, s->table_name.str);
    }
  } // for (hint_count)
  if (!shallow_hints && (s->referenced_keys.elements != rk_count))
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_CANNOT_ADD_FOREIGN,
                        "Expected %u refenced keys but found %u",
                        s->referenced_keys.elements, rk_count);
  }
  return p.pos < p.end; // Error if some data is still left
}


bool TABLE_SHARE::fk_resolve_referenced_keys(THD *thd, TABLE_SHARE *from)
{
  Lex_ident_set ids;
  bool inserted;

  for (FK_info &rk: referenced_keys)
  {
    DBUG_ASSERT(rk.foreign_id.length);
    if (!ids.insert(rk.foreign_id, &inserted))
      return true;

    DBUG_ASSERT(inserted);
  }

  for (FK_info &fk: from->foreign_keys)
  {
    if (0 != cmp_db_table(fk.referenced_db, fk.referenced_table))
      continue;

    DBUG_ASSERT(fk.foreign_id.length);
    if (!ids.insert(fk.foreign_id, &inserted))
      return true;

    if (!inserted)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_DUP_CONSTRAINT_NAME,
                          "Foreign ID already exists `%s`", fk.foreign_id.str);
      continue;
    }

    for (Lex_cstring &fld: fk.referenced_fields)
    {
      uint i;
      for (i= 0; i < fields ; i++)
      {
        if (0 == cmp_ident(field[i]->field_name, fld))
          break;
      }
      if (i == fields)
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_CANNOT_ADD_FOREIGN,
                            "Missing field `%s` hint table `%s.%s` refers to",
                            fld.str, from->db.str, from->table_name.str);
        return true;
      }
    }
    FK_info *dst= fk.clone(&mem_root);
    if (referenced_keys.push_back(dst, &mem_root))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return true;
    }
  }
  return false;
}
