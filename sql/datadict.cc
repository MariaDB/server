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

#include "mariadb.h"
#include "datadict.h"
#include "sql_priv.h"
#include "sql_base.h"
#include "sql_class.h"
#include "sql_table.h"
#include "ha_sequence.h"
#include "discover.h"

static const char * const tmp_fk_prefix= "#sqlf";
static const char * const bak_ext= ".fbk";

static int read_string(File file, uchar**to, size_t length)
{
  DBUG_ENTER("read_string");

  /* This can't use MY_THREAD_SPECIFIC as it's used on server start */
  if (!(*to= (uchar*) my_malloc(PSI_INSTRUMENT_ME, length+1,MYF(MY_WME))) ||
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
  If engine_name is 0, then the function will only test if the file is a
  view or not

  @retval  TABLE_TYPE_UNKNOWN   error  - file can't be opened
  @retval  TABLE_TYPE_NORMAL    table
  @retval  TABLE_TYPE_SEQUENCE  sequence table
  @retval  TABLE_TYPE_VIEW      view
*/

Table_type dd_frm_type(THD *thd, char *path, LEX_CSTRING *engine_name)
{
  File file;
  uchar header[40];     //"TYPE=VIEW\n" it is 10 characters
  size_t error;
  Table_type type= TABLE_TYPE_UNKNOWN;
  uchar dbt;
  DBUG_ENTER("dd_frm_type");

  file= mysql_file_open(key_file_frm, path, O_RDONLY | O_SHARE, MYF(0));
  if (file < 0)
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

  /* engine_name is 0 if we only want to know if table is view or not */
  if (!engine_name)
    goto err;

  if (!is_binary_frm_header(header))
    goto err;

  dbt= header[3];

  if (((header[39] >> 4) & 3) == HA_CHOICE_YES)
  {
    DBUG_PRINT("info", ("Sequence found"));
    type= TABLE_TYPE_SEQUENCE;
  }

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

  @retval  FALSE  Success.
  @retval  TRUE   Error.
*/

bool dd_recreate_table(THD *thd, const char *db, const char *table_name)
{
  HA_CREATE_INFO create_info;
  char path_buf[FN_REFLEN + 1];
  DBUG_ENTER("dd_recreate_table");

  /* There should be a exclusive metadata lock on the table. */
  DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE, db, table_name,
                                             MDL_EXCLUSIVE));
  create_info.init();
  build_table_filename(path_buf, sizeof(path_buf) - 1,
                       db, table_name, "", 0);
  /* Attempt to reconstruct the table. */
  DBUG_RETURN(ha_create_table(thd, path_buf, db, table_name, &create_info, NULL, 0));
}

size_t dd_extra2_len(const uchar **pos, const uchar *end)
{
  size_t length= *(*pos)++;
  if (length)
    return length;

  if ((*pos) + 2 >= end)
    return 0;
  length= uint2korr(*pos);
  (*pos)+= 2;
  if (length < 256 || *pos + length > end)
    return 0;
  return length;
}


bool Extra2_info::read(const uchar *frm_image, size_t frm_size)
{
  read_size= uint2korr(frm_image + 4);

  if (frm_size < FRM_HEADER_SIZE + read_size)
    return true;

  const uchar *pos= frm_image + 64;

  DBUG_ENTER("read_extra2");

  if (*pos == '/')   // old frm had '/' there
    DBUG_RETURN(false);

  const uchar *e2end= pos + read_size;
  while (pos + 3 <= e2end)
  {
    extra2_frm_value_type type= (extra2_frm_value_type)*pos++;
    size_t length= dd_extra2_len(&pos, e2end);
    if (!length)
      DBUG_RETURN(true);
    switch (type) {
      case EXTRA2_TABLEDEF_VERSION:
        if (version.str) // see init_from_sql_statement_string()
        {
          if (length != version.length)
            DBUG_RETURN(true);
        }
        else
        {
          version.str= pos;
          version.length= length;
        }
        break;
      case EXTRA2_ENGINE_TABLEOPTS:
        if (options.str)
          DBUG_RETURN(true);
        options.str= pos;
        options.length= length;
        break;
      case EXTRA2_DEFAULT_PART_ENGINE:
        engine.set((const char*)pos, length);
        break;
      case EXTRA2_GIS:
        if (gis.str)
          DBUG_RETURN(true);
        gis.str= pos;
        gis.length= length;
        break;
      case EXTRA2_PERIOD_FOR_SYSTEM_TIME:
        if (system_period.str || length != 2 * frm_fieldno_size)
          DBUG_RETURN(true);
        system_period.str = pos;
        system_period.length= length;
        break;
      case EXTRA2_FIELD_FLAGS:
        if (field_flags.str)
          DBUG_RETURN(true);
        field_flags.str= pos;
        field_flags.length= length;
        break;
      case EXTRA2_APPLICATION_TIME_PERIOD:
        if (application_period.str)
          DBUG_RETURN(true);
        application_period.str= pos;
        application_period.length= length;
        break;
      case EXTRA2_FIELD_DATA_TYPE_INFO:
        if (field_data_type_info.str)
          DBUG_RETURN(true);
        field_data_type_info.str= pos;
        field_data_type_info.length= length;
        break;
      case EXTRA2_PERIOD_WITHOUT_OVERLAPS:
        if (without_overlaps.str)
          DBUG_RETURN(true);
        without_overlaps.str= pos;
        without_overlaps.length= length;
        break;
      case EXTRA2_FOREIGN_KEY_INFO:
        if (foreign_key_info.str)
          DBUG_RETURN(true);
        foreign_key_info.str= pos;
        foreign_key_info.length= length;
        break;
      default:
        /* abort frm parsing if it's an unknown but important extra2 value */
        if (type >= EXTRA2_ENGINE_IMPORTANT)
          DBUG_RETURN(true);
    }
    pos+= length;
  }
  if (pos != e2end)
    DBUG_RETURN(true);

  DBUG_ASSERT(store_size() == read_size);
  DBUG_RETURN(false);
}


uchar *
Extra2_info::write(uchar *frm_image, size_t frm_size)
{
  // FIXME: what to do with frm_size here (and in read())?
  uchar *pos;
  /* write the extra2 segment */
  pos = frm_image + FRM_HEADER_SIZE;
  compile_time_assert(EXTRA2_TABLEDEF_VERSION != '/');

  if (version.str)
    pos= extra2_write(pos, EXTRA2_TABLEDEF_VERSION, version);

  if (engine.str)
    pos= extra2_write(pos, EXTRA2_DEFAULT_PART_ENGINE, engine);

  if (options.str)
    pos= extra2_write(pos, EXTRA2_ENGINE_TABLEOPTS, options);

  if (gis.str)
    pos= extra2_write(pos, EXTRA2_GIS, gis);

  if (field_data_type_info.str)
    pos= extra2_write(pos, EXTRA2_FIELD_DATA_TYPE_INFO, field_data_type_info);

  if (foreign_key_info.str)
    pos= extra2_write(pos, EXTRA2_FOREIGN_KEY_INFO, foreign_key_info);

  if (system_period.str)
    pos= extra2_write(pos, EXTRA2_PERIOD_FOR_SYSTEM_TIME, system_period);

  if (application_period.str)
    pos= extra2_write(pos, EXTRA2_APPLICATION_TIME_PERIOD, application_period);

  if (without_overlaps.str)
    pos= extra2_write(pos, EXTRA2_PERIOD_WITHOUT_OVERLAPS, without_overlaps);

  if (field_flags.str)
    pos= extra2_write(pos, EXTRA2_FIELD_FLAGS, field_flags);

  write_size= pos - frm_image - FRM_HEADER_SIZE;
  DBUG_ASSERT(write_size == store_size());
  DBUG_ASSERT(write_size <= 0xffff - FRM_HEADER_SIZE - 4);

#if 0
  int4store(pos, filepos); // end of the extra2 segment
#endif
  return pos;
}


int TABLE_SHARE::fk_write_shadow_frm_impl(const char *shadow_path)
{
  const uchar * frm_src;
  uchar * frm_dst;
  uchar * pos;
  size_t frm_size;
  Extra2_info extra2;
  Foreign_key_io foreign_key_io;

  int err= read_frm_image(&frm_src, &frm_size);
  if (err)
  {
    char path[FN_REFLEN + 1];
    strxmov(path, normalized_path.str, reg_ext, NullS);
    switch (err)
    {
    case 1:
      my_error(ER_CANT_OPEN_FILE, MYF(0), path, my_errno);
      break;
    case 2:
      my_error(ER_FILE_NOT_FOUND, MYF(0), path, my_errno);
      break;
    default:
      DBUG_ASSERT(err < 10);
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      break;
    }
    return err;
  }

  Scope_malloc frm_src_freer(frm_src); // read_frm_image() passed ownership to us

  if (frm_size < FRM_HEADER_SIZE + FRM_FORMINFO_SIZE)
  {
frm_err:
    char path[FN_REFLEN + 1];
    strxmov(path, normalized_path.str, reg_ext, NullS);
    my_error(ER_NOT_FORM_FILE, MYF(0), path);
    return 10;
  }

  if (!is_binary_frm_header(frm_src))
    goto frm_err;

  if (extra2.read(frm_src, frm_size))
  {
    my_printf_error(ER_CANT_CREATE_TABLE,
                    "Cannot create table %`s: "
                    "Read of extra2 section failed.",
                    MYF(0), table_name.str);
    return 10;
  }

  const uchar * const rest_src= frm_src + FRM_HEADER_SIZE + extra2.read_size;
  const size_t rest_size= frm_size - FRM_HEADER_SIZE - extra2.read_size;
  size_t forminfo_off= uint4korr(rest_src);

  foreign_key_io.store(foreign_keys, referenced_keys);
  extra2.foreign_key_info= foreign_key_io.lex_custring();
  if (!extra2.foreign_key_info.length)
    extra2.foreign_key_info.str= NULL;
  else if (extra2.foreign_key_info.length > 0xffff - FRM_HEADER_SIZE - 8)
  {
    my_printf_error(ER_CANT_CREATE_TABLE,
                    "Cannot create table %`s: "
                    "Building the foreign key info image failed.",
                    MYF(0), table_name.str);
    return 10;
  }

  const size_t extra2_increase= extra2.store_size() - extra2.read_size; // FIXME: can be negative
  frm_size+= extra2_increase;

  if (frm_size > FRM_MAX_SIZE)
  {
    my_error(ER_TABLE_DEFINITION_TOO_BIG, MYF(0), table_name.str);
    return 10;
  }

  Scope_malloc frm_dst_freer(frm_dst, frm_size, MY_WME);
  if (!frm_dst)
    return 10;

  memcpy((void *)frm_dst, (void *)frm_src, FRM_HEADER_SIZE);

  if (!(pos= extra2.write(frm_dst, frm_size)))
  {
    my_printf_error(ER_CANT_CREATE_TABLE,
                    "Cannot create table %`s: "
                    "Write of extra2 section failed.",
                    MYF(0), table_name.str);
    return 10;
  }

  forminfo_off+= extra2_increase;
  int4store(pos, forminfo_off);
  pos+= 4;
  int2store(frm_dst + 4, extra2.write_size);
  int2store(frm_dst + 6, FRM_HEADER_SIZE + extra2.write_size + 4); // Position to key information
  int4store(frm_dst + 10, frm_size);
  memcpy((void *)pos, rest_src + 4, rest_size - 4);

  char shadow_frm_name[FN_REFLEN + 1];
  strxnmov(shadow_frm_name, sizeof(shadow_frm_name), shadow_path, reg_ext, NullS);
  if (writefile(shadow_frm_name, db.str, table_name.str, false, frm_dst, frm_size))
    return 10;

  return 0;
}


bool ddl_log_info::write_log_replace_delete_file(const char *from_path,
                                                const char *to_path,
                                                bool replace_flag)
{
  DDL_LOG_ENTRY ddl_log_entry;
  bzero(&ddl_log_entry, sizeof(ddl_log_entry));

  if (replace_flag)
    ddl_log_entry.action_type= DDL_LOG_REPLACE_ACTION;
  else
    ddl_log_entry.action_type= DDL_LOG_DELETE_ACTION;
  ddl_log_entry.entry_type= DDL_TRY_LOG_ENTRY_CODE;
  ddl_log_entry.next_entry= list ? list->entry_pos : 0;
  ddl_log_entry.handler_name= file_action;
  ddl_log_entry.name= { to_path, strlen(to_path) };
  if (replace_flag)
    ddl_log_entry.from_name= { from_path, strlen(from_path) };
  if (ERROR_INJECT("fail_log_replace_delete_1", "crash_log_replace_delete_1"))
    return true;
  DDL_LOG_MEMORY_ENTRY *next_active_log_entry= list;
  Mutex_lock lock_gdl(&LOCK_gdl);
  if (ddl_log_write_entry(&ddl_log_entry, &list))
  {
error:
    my_error(ER_DDL_LOG_ERROR, MYF(0));
    return true;
  }
  if (ERROR_INJECT("fail_log_replace_delete_2", "crash_log_replace_delete_2"))
    goto error;
  list->next_active_log_entry= next_active_log_entry;
  if (ddl_log_write_execute_entry(list->entry_pos, &execute_entry))
    goto error;
  if (ERROR_INJECT("fail_log_replace_delete_3", "crash_log_replace_delete_3"))
    goto error;
  return false;
}


int FK_backup::fk_write_shadow_frm(ddl_log_info &log_info)
{
  char shadow_path[FN_REFLEN + 1];
  char frm_name[FN_REFLEN + 1];
  int err;
  TABLE_SHARE *s= get_share();
  DBUG_ASSERT(s);
  build_table_shadow_filename(shadow_path, sizeof(shadow_path) - 1,
                              s->db, s->table_name, tmp_fk_prefix);
  strxnmov(frm_name, sizeof(frm_name), shadow_path, reg_ext, NullS);
  if (log_info.write_log_replace_delete_file(NULL, frm_name, false))
    return true;
  delete_shadow_entry= log_info.list;
#ifndef DBUG_OFF
  if (log_info.dbg_fail &&
      (ERROR_INJECT("fail_fk_write_shadow_frm", "crash_fk_write_shadow_frm")))
  {
    err= 10;
    goto write_shadow_failed;
  }
#endif
  err= s->fk_write_shadow_frm_impl(shadow_path);
  if (err)
  {
#ifndef DBUG_OFF
write_shadow_failed:
#endif
    if (ddl_log_increment_phase(delete_shadow_entry->entry_pos))
    {
      /* This is very bad case because log replay will delete original frm.
         At least try prohibit replaying it and push an alert message. */
      log_info.write_log_finish();
      my_printf_error(ER_DDL_LOG_ERROR, "Deactivating delete shadow entry %u failed",
                      MYF(0), delete_shadow_entry->entry_pos);
    }
    delete_shadow_entry= NULL;
  }
  return err;
}


bool FK_backup::fk_backup_frm(ddl_log_info &log_info)
{
  MY_STAT stat_info;
  DBUG_ASSERT(0 != strcmp(reg_ext, bak_ext));
  char path[FN_REFLEN + 1];
  char bak_name[FN_REFLEN + 1];
  char frm_name[FN_REFLEN + 1];
  TABLE_SHARE *s= get_share();
  build_table_filename(path, sizeof(path), s->db.str,
                       s->table_name.str, "", 0);
  strxnmov(frm_name, sizeof(frm_name), path, reg_ext, NullS);
  strxnmov(bak_name, sizeof(bak_name), path, bak_ext, NullS);
  if (mysql_file_stat(key_file_frm, bak_name, &stat_info, MYF(0)))
  {
    my_error(ER_FILE_EXISTS_ERROR, MYF(0), bak_name);
    return true;
  }
  if (log_info.write_log_replace_delete_file(bak_name, frm_name, true))
    return true;
  restore_backup_entry= log_info.list;
#ifndef DBUG_OFF
  if (log_info.dbg_fail &&
      (ERROR_INJECT("fail_fk_backup_frm", "crash_fk_backup_frm")))
  {
    goto rename_failed;
  }
#endif
  if (mysql_file_rename(key_file_frm, frm_name, bak_name, MYF(MY_WME)))
  {
#ifndef DBUG_OFF
rename_failed:
#endif
    /* Rename failed and we don't want rollback to delete original frm */
    if (ddl_log_increment_phase(restore_backup_entry->entry_pos))
    {
      /* This is very bad case because log replay will delete original frm.
         At least try prohibit replaying it and push an alert message. */
      log_info.write_log_finish();
      my_printf_error(ER_DDL_LOG_ERROR, "Deactivating restore backup entry %u failed",
                      MYF(0), restore_backup_entry->entry_pos);
    }
    restore_backup_entry= NULL;
    return true;
  }
  return false;
}


bool FK_backup::fk_install_shadow_frm(ddl_log_info &log_info)
{
  MY_STAT stat_info;
  char shadow_path[FN_REFLEN + 1];
  char path[FN_REFLEN + 1];
  char shadow_frm_name[FN_REFLEN + 1];
  char frm_name[FN_REFLEN + 1];
  TABLE_SHARE *s= get_share();
  build_table_shadow_filename(shadow_path, sizeof(shadow_path) - 1,
                              s->db, s->table_name, tmp_fk_prefix);
  build_table_filename(path, sizeof(path), s->db.str,
                       s->table_name.str, "", 0);
  strxnmov(shadow_frm_name, sizeof(shadow_frm_name), shadow_path, reg_ext, NullS);
  strxnmov(frm_name, sizeof(frm_name), path, reg_ext, NullS);
  if (!mysql_file_stat(key_file_frm, shadow_frm_name, &stat_info, MYF(MY_WME)))
    return true;
#ifndef DBUG_OFF
  if (log_info.dbg_fail &&
      (ERROR_INJECT("fail_fk_install_shadow_frm", "crash_fk_install_shadow_frm")))
  {
    return true;
  }
#endif
  if (mysql_file_rename(key_file_frm, shadow_frm_name, frm_name, MYF(MY_WME)))
    return true;
  if (ddl_log_increment_phase(delete_shadow_entry->entry_pos))
  {
    my_printf_error(ER_DDL_LOG_ERROR, "Deactivating delete shadow entry %u failed",
                    MYF(0), delete_shadow_entry->entry_pos);
    return true;
  }
  delete_shadow_entry= NULL;
  return false;
}


void FK_backup::fk_drop_shadow_frm(ddl_log_info &log_info)
{
  char shadow_path[FN_REFLEN+1];
  char shadow_frm_name[FN_REFLEN+1];
  TABLE_SHARE *s= get_share();
  build_table_shadow_filename(shadow_path, sizeof(shadow_path) - 1,
                              s->db, s->table_name, tmp_fk_prefix);
  strxnmov(shadow_frm_name, sizeof(shadow_frm_name), shadow_path, reg_ext, NullS);
  mysql_file_delete(key_file_frm, shadow_frm_name, MYF(0));
}



void FK_backup::fk_drop_backup_frm(ddl_log_info &log_info)
{
  char path[FN_REFLEN + 1];
  char bak_name[FN_REFLEN + 1];
  TABLE_SHARE *s= get_share();
  build_table_filename(path, sizeof(path), s->db.str,
                       s->table_name.str, "", 0);
  strxnmov(bak_name, sizeof(bak_name), path, bak_ext, NullS);
  mysql_file_delete(key_file_frm, bak_name, MYF(0));
}


int FK_backup_storage::write_shadow_frms()
{
  int err;
#ifndef DBUG_OFF
  FK_backup *last= NULL;
  for (auto &bak: *this)
  {
    if (!bak.second.update_frm)
      continue;
    last= &bak.second;
  }
  dbg_fail= false;
#endif
  for (auto &bak: *this)
  {
    if (!bak.second.update_frm)
      continue;
#ifndef DBUG_OFF
    if (&bak.second == last)
      dbg_fail= true;
#endif
    if ((err= bak.second.fk_write_shadow_frm(*this)))
      return err;
  }
  return 0;
}


bool FK_backup_storage::install_shadow_frms()
{
  if (!size())
    return false;
#ifndef DBUG_OFF
  FK_backup *last= NULL;
  for (auto &bak: *this)
  {
    if (!bak.second.update_frm)
      continue;
    last= &bak.second;
  }
  dbg_fail= false;
#endif
  for (auto &bak: *this)
  {
    if (!bak.second.update_frm)
      continue;
#ifndef DBUG_OFF
    if (&bak.second == last)
      dbg_fail= true;
#endif
    if (bak.second.fk_backup_frm(*this))
      return true;
  }
#ifndef DBUG_OFF
  dbg_fail= false;
#endif
  for (auto &bak: *this)
  {
    if (!bak.second.update_frm)
      continue;
#ifndef DBUG_OFF
    if (&bak.second == last)
      dbg_fail= true;
#endif
    if (bak.second.fk_install_shadow_frm(*this))
      return true;
  }

  return false;
}


void FK_backup_storage::rollback(THD *thd)
{
  for (auto &bak: *this)
    bak.second.rollback(*this);

  // NB: we might not fk_write_shadow_frm() at all f.ex. when rename table failed
  if (!list)
    return;

  if (ddl_log_execute_entry(thd, list->entry_pos))
  {
    my_printf_error(ER_DDL_LOG_ERROR, "Executing some rollback actions from entry %u failed",
                    MYF(0), list->entry_pos);
  }
  write_log_finish();
}


void FK_backup_storage::drop_backup_frms(THD *thd)
{
#ifndef DBUG_OFF
  dbg_fail= true;
#endif
  for (auto &bak: *this)
  {
    if (!bak.second.update_frm)
      continue;
    if (ddl_log_increment_phase(bak.second.restore_backup_entry->entry_pos))
    {
      // TODO: test getting into here (and other deactivate_ddl_log_entry() failures)
      my_printf_error(ER_DDL_LOG_ERROR, "Deactivating restore backup entry %u failed",
                      MYF(0), bak.second.restore_backup_entry->entry_pos);
      // TODO: must be atomic
    }
#ifndef DBUG_OFF
    dbg_fail= false;
#endif
  }
#ifndef DBUG_OFF
  dbg_fail= true;
#endif
  for (auto &bak: *this)
  {
    if (!bak.second.update_frm)
      continue;
    bak.second.fk_drop_backup_frm(*this);
#ifndef DBUG_OFF
    dbg_fail= false;
#endif
  }
  if (list)
    write_log_finish();
}
