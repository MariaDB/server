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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include <my_global.h>
#include "datadict.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "sql_table.h"

static int read_string(File file, uchar**to, size_t length)
{
  DBUG_ENTER("read_string");

  my_free(*to);
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

  engine_name is a LEX_STRING, where engine_name->str must point to
  a buffer of at least NAME_CHAR_LEN+1 bytes.
  If engine_name is 0, then the function will only test if the file is a
  view or not

  @retval  FRMTYPE_ERROR        error
  @retval  FRMTYPE_TABLE        table
  @retval  FRMTYPE_VIEW         view
*/

frm_type_enum dd_frm_type(THD *thd, char *path, LEX_STRING *engine_name)
{
  File file;
  uchar header[10];     //"TYPE=VIEW\n" it is 10 characters
  size_t error;
  frm_type_enum type= FRMTYPE_ERROR;
  uchar dbt;
  DBUG_ENTER("dd_frm_type");

  if ((file= mysql_file_open(key_file_frm, path, O_RDONLY | O_SHARE, MYF(0))) < 0)
    DBUG_RETURN(FRMTYPE_ERROR);
  error= mysql_file_read(file, (uchar*) header, sizeof(header), MYF(MY_NABP));

  if (error)
    goto err;
  if (!strncmp((char*) header, "TYPE=VIEW\n", sizeof(header)))
  {
    type= FRMTYPE_VIEW;
    goto err;
  }

  /*
    We return FRMTYPE_TABLE if we can read the .frm file. This allows us
    to drop a bad .frm file with DROP TABLE
  */
  type= FRMTYPE_TABLE;

  /* engine_name is 0 if we only want to know if table is view or not */
  if (!engine_name)
    goto err;

  /* Initialize engine name in case we are not able to find it out */
  engine_name->length= 0;
  engine_name->str[0]= 0;

  if (!is_binary_frm_header(header))
    goto err;

  dbt= header[3];

  /* cannot use ha_resolve_by_legacy_type without a THD */
  if (thd && dbt < DB_TYPE_FIRST_DYNAMIC)
  {
    handlerton *ht= ha_resolve_by_legacy_type(thd, (enum legacy_db_type)dbt);
    if (ht)
    {
      *engine_name= hton2plugin[ht->slot]->name;
      goto err;
    }
  }

  /* read the true engine name */
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

    if ((n_length= uint4korr(frm_image+55)))
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
          strmake(engine_name->str, (char*)next_chunk + 2,
                  engine_name->length= len);
      }
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

