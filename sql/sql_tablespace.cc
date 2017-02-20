/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.

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

/* drop and alter of tablespaces */

#include <my_global.h>
#include "sql_priv.h"
#include "unireg.h"
#include "sql_tablespace.h"
#include "sql_table.h"                          // write_bin_log
#include "sql_class.h"                          // THD

/**
  Check if tablespace name is valid

  @param tablespace_name        Name of the tablespace

  @note Tablespace names are not reflected in the file system, so
        character case conversion or consideration is not relevant.

  @note Checking for path characters or ending space is not done.
        The only checks are for identifier length, both in terms of
        number of characters and number of bytes.

  @retval  IDENT_NAME_OK        Identifier name is ok (Success)
  @retval  IDENT_NAME_WRONG     Identifier name is wrong, if length == 0
*                               (ER_WRONG_TABLESPACE_NAME)
  @retval  IDENT_NAME_TOO_LONG  Identifier name is too long if it is greater
                                than 64 characters (ER_TOO_LONG_IDENT)

  @note In case of IDENT_NAME_TOO_LONG or IDENT_NAME_WRONG, the function
        reports an error (using my_error()).
*/

enum_ident_name_check check_tablespace_name(const char *tablespace_name)
{
  size_t name_length= 0;                       //< Length as number of bytes
  size_t name_length_symbols= 0;               //< Length as number of symbols

  // Name must be != NULL and length must be > 0
  if (!tablespace_name || (name_length= strlen(tablespace_name)) == 0)
  {
    my_error(ER_WRONG_TABLESPACE_NAME, MYF(0), tablespace_name);
    return IDENT_NAME_WRONG;
  }

  // If we do not have too many bytes, we must check the number of symbols,
  // provided the system character set may use more than one byte per symbol.
  if (name_length <= NAME_LEN && use_mb(system_charset_info))
  {
    const char *name= tablespace_name;   //< The actual tablespace name
    const char *end= name + name_length; //< Pointer to first byte after name

    // Loop over all symbols as long as we don't have too many already
    while (name != end && name_length_symbols <= NAME_CHAR_LEN)
    {
      int len= my_ismbchar(system_charset_info, name, end);
      if (len)
        name += len;
      else
        name++;

      name_length_symbols++;
    }
  }

  if (name_length_symbols > NAME_CHAR_LEN || name_length > NAME_LEN)
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), tablespace_name);
    return IDENT_NAME_TOO_LONG;
  }

  return IDENT_NAME_OK;
}


int mysql_alter_tablespace(THD *thd, st_alter_tablespace *ts_info)
{
  int error= HA_ADMIN_NOT_IMPLEMENTED;
  handlerton *hton= ts_info->storage_engine;

  DBUG_ENTER("mysql_alter_tablespace");
  /*
    If the user haven't defined an engine, this will fallback to using the
    default storage engine.
  */
  if (hton == NULL || hton->state != SHOW_OPTION_YES)
  {
    hton= ha_default_handlerton(thd);
    if (ts_info->storage_engine != 0)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_USING_OTHER_HANDLER,
                          ER_THD(thd, ER_WARN_USING_OTHER_HANDLER),
                          hton_name(hton)->str,
                          ts_info->tablespace_name ? ts_info->tablespace_name
                                                : ts_info->logfile_group_name);
  }

  if (hton->alter_tablespace)
  {
    if ((error= hton->alter_tablespace(hton, thd, ts_info)))
    {
      if (error == 1)
      {
        DBUG_RETURN(1);
      }

      if (error == HA_ADMIN_NOT_IMPLEMENTED)
      {
        my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), "");
      }
      else
      {
        my_error(error, MYF(0));
      }
      DBUG_RETURN(error);
    }
  }
  else
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_ILLEGAL_HA_CREATE_OPTION,
                        ER_THD(thd, ER_ILLEGAL_HA_CREATE_OPTION),
                        hton_name(hton)->str,
                        "TABLESPACE or LOGFILE GROUP");
  }
  error= write_bin_log(thd, FALSE, thd->query(), thd->query_length());
  DBUG_RETURN(error);
}
