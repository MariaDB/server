/* Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "mariadb.h"
#include "datadict.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "sql_table.h"
#include "ha_sequence.h"

static int read_string(File file, uchar**to, size_t length)
{
  DBUG_ENTER("read_string");

  /* This can't use MY_THREAD_SPECIFIC as it's used on server start */
  if (!(*to= (uchar*) my_malloc(length+1,MYF(MY_WME))) ||
      mysql_file_read(file, *to, length, MYF(MY_NABP)))
  {
     my_free(*to);
    *to= 0;
    DBUG_RETURN(1);
  }
  *((char*) *to+length)= '\0'; // C-style safety
  DBUG_RETURN (0);
}


/**
  Check type of .frm if we are not going to parse it.

  @param[in]  thd               The current session.
  @param[in]  path              path to FRM file.
  @param[in/out] engine_name    table engine name (length < NAME_CHAR_LEN)

  engine_name is a LEX_CSTRING, where engine_name->str must point to
  a buffer of at least NAME_CHAR_LEN+1 bytes.
  If both engine_name and is_versioned are 0, then the function will
  only test if the file is a view or not

  @param[out] is_sequence  1 if table is a SEQUENCE, 0 otherwise
  @param[out] is_versioned 1 if table is  versioned, 0 otherwise

  @retval  TABLE_TYPE_UNKNOWN   error  - file can't be opened
  @retval  TABLE_TYPE_NORMAL    table
  @retval  TABLE_TYPE_SEQUENCE  sequence table
  @retval  TABLE_TYPE_VIEW      view
*/

Table_type dd_frm_type(THD *thd, char *path, LEX_CSTRING *engine_name,
                       bool *is_sequence, bool *is_versioned)
{
  File file;
  uchar header[40];     //"TYPE=VIEW\n" it is 10 characters
  size_t error;
  Table_type type= TABLE_TYPE_UNKNOWN;
  uchar dbt;
  bool need_engine= engine_name;
  DBUG_ENTER("dd_frm_type");

  *is_sequence= 0;

  if ((file= mysql_file_open(key_file_frm, path, O_RDONLY | O_SHARE, MYF(0)))
      < 0)
    DBUG_RETURN(TABLE_TYPE_UNKNOWN);

  /*
    We return TABLE_TYPE_NORMAL if we can open the .frm file. This allows us
    to drop a bad .frm file with DROP TABLE
  */
  type= TABLE_TYPE_NORMAL;

  /*
    Initialize engine name in case we are not able to find it out
    The cast is safe, as engine_name->str points to a usable buffer.
   */
  if (engine_name)
  {
    engine_name->length= 0;
    ((char*) (engine_name->str))[0]= 0;
  }

  if (unlikely((error= mysql_file_read(file, (uchar*) header, sizeof(header), MYF(MY_NABP)))))
    goto err;

  if (unlikely((!strncmp((char*) header, "TYPE=VIEW\n", 10))))
  {
    type= TABLE_TYPE_VIEW;
    goto err;
  }

  if (!is_binary_frm_header(header))
    goto err;

  dbt= header[3];

  if (((header[39] >> 4) & 3) == HA_CHOICE_YES && is_sequence)
  {
    DBUG_PRINT("info", ("Sequence found"));
    *is_sequence= 1;
  }

  /* cannot use ha_resolve_by_legacy_type without a THD */
  if (thd && dbt < DB_TYPE_FIRST_DYNAMIC && engine_name)
  {
    handlerton *ht= ha_resolve_by_legacy_type(thd, (enum legacy_db_type)dbt);
    if (ht)
    {
      *engine_name= hton2plugin[ht->slot]->name;
      need_engine= false;
    }
  }

  /* read the true engine name */
  if (need_engine || is_versioned)
  {
    MY_STAT state;  
    uchar *frm_image= 0;
    uint n_length;

    if (mysql_file_fstat(file, &state, MYF(MY_WME)))
      goto err;

    if (mysql_file_seek(file, 0, SEEK_SET, MYF(MY_WME)))
      goto err;

    if (read_string(file, &frm_image, (size_t)state.st_size))
      goto err;

    if (need_engine && (n_length= uint4korr(frm_image+55)))
    {
      uint record_offset= uint2korr(frm_image+6)+
                      ((uint2korr(frm_image+14) == 0xffff ?
                        uint4korr(frm_image+47) : uint2korr(frm_image+14)));
      uint reclength= uint2korr(frm_image+16);

      uchar *next_chunk= frm_image + record_offset + reclength;
      uchar *buff_end= next_chunk + n_length;
      uint connect_string_length= uint2korr(next_chunk);
      next_chunk+= connect_string_length + 2;
      if (next_chunk + 2 < buff_end)
      {
        uint len= uint2korr(next_chunk);
        if (len <= NAME_CHAR_LEN)
        {
          /*
            The following cast is safe as the caller has allocated buffer
            and it's up to this function to generate the name.
          */
          strmake((char*) engine_name->str, (char*)next_chunk + 2,
                  engine_name->length= len);
        }
      }
    }

    if (is_versioned)
    {
      extra2_fields extra2;
      dd_read_extra2(frm_image, (size_t)state.st_size, &extra2);
      *is_versioned = extra2.system_period;

    }

    my_free(frm_image);
  }

  /* Probably a table. */
err:
  mysql_file_close(file, MYF(MY_WME));
  DBUG_RETURN(type);
}


/*
  Regenerate a metadata locked table.

  @param  thd   Thread context.
  @param  db    Name of the database to which the table belongs to.
  @param  name  Table name.
  @param  path  For temporary tables only - path to table files.
                Otherwise NULL (the path is calculated from db and table names).

  @retval  FALSE  Success.
  @retval  TRUE   Error.
*/

bool dd_recreate_table(THD *thd, const char *db, const char *table_name,
                       const char *path)
{
  bool error= TRUE;
  HA_CREATE_INFO create_info;
  char path_buf[FN_REFLEN + 1];
  DBUG_ENTER("dd_recreate_table");

  memset(&create_info, 0, sizeof(create_info));

  if (path)
    create_info.options|= HA_LEX_CREATE_TMP_TABLE;
  else
  {
    build_table_filename(path_buf, sizeof(path_buf) - 1,
                         db, table_name, "", 0);
    path= path_buf;

    /* There should be a exclusive metadata lock on the table. */
    DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE, db, table_name,
                                               MDL_EXCLUSIVE));
  }

  /* Attempt to reconstruct the table. */
  error= ha_create_table(thd, path, db, table_name, &create_info, NULL);

  DBUG_RETURN(error);
}


bool dd_read_extra2(const uchar *frm_image, size_t len, extra2_fields *fields)
{
  const uchar *extra2= frm_image + 64;

  DBUG_ENTER("dd_read_extra2");

  memset(fields, 0, sizeof(extra2_fields));

  if (*extra2 != '/')   // old frm had '/' there
  {
    const uchar *e2end= extra2 + len;
    while (extra2 + 3 <= e2end)
    {
      uchar type= *extra2++;
      size_t length= *extra2++;
      if (!length)
      {
        if (extra2 + 2 >= e2end)
          DBUG_RETURN(true);
        length= uint2korr(extra2);
        extra2+= 2;
        if (length < 256)
          DBUG_RETURN(true);
      }
      if (extra2 + length > e2end)
        DBUG_RETURN(true);
      switch (type) {
        case EXTRA2_TABLEDEF_VERSION:
          if (fields->version.str) // see init_from_sql_statement_string()
          {
            if (length != fields->version.length)
              DBUG_RETURN(true);
          }
          else
          {
            fields->version.str= extra2;
            fields->version.length= length;
          }
          break;
        case EXTRA2_ENGINE_TABLEOPTS:
          if (fields->options.str)
            DBUG_RETURN(true);
          fields->options.str= extra2;
          fields->options.length= length;
          break;
        case EXTRA2_DEFAULT_PART_ENGINE:
          fields->engine.set((char*)extra2, length);
          break;
        case EXTRA2_GIS:
#ifdef HAVE_SPATIAL
          if (fields->gis.str)
            DBUG_RETURN(true);
          fields->gis.str= extra2;
          fields->gis.length= length;
#endif /*HAVE_SPATIAL*/
          break;
        case EXTRA2_PERIOD_FOR_SYSTEM_TIME:
          if (fields->system_period || length != 2 * sizeof(uint16))
            DBUG_RETURN(true);
          fields->system_period = extra2;
          break;
        case EXTRA2_FIELD_FLAGS:
          if (fields->field_flags.str)
            DBUG_RETURN(true);
          fields->field_flags.str= extra2;
          fields->field_flags.length= length;
          break;
        case EXTRA2_APPLICATION_TIME_PERIOD:
          if (fields->application_period.str)
            DBUG_RETURN(true);
          fields->application_period.str= extra2;
          fields->application_period.length= length;
          break;
        default:
          /* abort frm parsing if it's an unknown but important extra2 value */
          if (type >= EXTRA2_ENGINE_IMPORTANT)
            DBUG_RETURN(true);
      }
      extra2+= length;
    }
    if (extra2 != e2end)
      DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}
