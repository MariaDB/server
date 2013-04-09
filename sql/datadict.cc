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

#include "datadict.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "sql_table.h"


/**
  Check type of .frm if we are not going to parse it.

  @param[in]  thd   The current session.
  @param[in]  path  path to FRM file.
  @param[out] dbt   db_type of the table if FRMTYPE_TABLE, otherwise undefined.

  @retval  FRMTYPE_ERROR        error
  @retval  FRMTYPE_TABLE        table
  @retval  FRMTYPE_VIEW         view
*/

frm_type_enum dd_frm_type(THD *thd, char *path, enum legacy_db_type *dbt)
{
  File file;
  uchar header[10];     //"TYPE=VIEW\n" it is 10 characters
  size_t error;
  DBUG_ENTER("dd_frm_type");

  *dbt= DB_TYPE_UNKNOWN;

  if ((file= mysql_file_open(key_file_frm, path, O_RDONLY | O_SHARE, MYF(0))) < 0)
    DBUG_RETURN(FRMTYPE_ERROR);
  error= mysql_file_read(file, (uchar*) header, sizeof(header), MYF(MY_NABP));
  mysql_file_close(file, MYF(MY_WME));

  if (error)
    DBUG_RETURN(FRMTYPE_ERROR);
  if (!strncmp((char*) header, "TYPE=VIEW\n", sizeof(header)))
    DBUG_RETURN(FRMTYPE_VIEW);

  /*
    This is just a check for DB_TYPE. We'll return default unknown type
    if the following test is true (arg #3). This should not have effect
    on return value from this function (default FRMTYPE_TABLE)
  */
  if (!is_binary_frm_header(header))
    DBUG_RETURN(FRMTYPE_TABLE);

  /*
    XXX this is a bug.
    if header[3] is > DB_TYPE_FIRST_DYNAMIC, then the complete
    storage engine name must be read from the frm
  */
  *dbt= (enum legacy_db_type) (uint) *(header + 3);

  /* Probably a table. */
  DBUG_RETURN(FRMTYPE_TABLE);
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
  error= ha_create_table(thd, path, db, table_name, &create_info);

  DBUG_RETURN(error);
}

