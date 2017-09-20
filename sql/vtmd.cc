#include "vtmd.h"
#include "sql_base.h"
#include "sql_class.h"
#include "sql_handler.h" // mysql_ha_rm_tables()
#include "sql_table.h"
#include "sql_select.h"
#include "table_cache.h" // tdc_remove_table()
#include "key.h"

LString VERS_VTMD_TEMPLATE(C_STRING_WITH_LEN("vtmd_template"));

bool
VTMD_table::create(THD *thd)
{
  Table_specification_st create_info;
  TABLE_LIST src_table, table;
  create_info.init(DDL_options_st::OPT_LIKE);
  create_info.options|= HA_VTMD;
  create_info.alias= vtmd_name;
  table.init_one_table(
    DB_WITH_LEN(about),
    XSTRING_WITH_LEN(vtmd_name),
    vtmd_name,
    TL_READ);
  src_table.init_one_table(
    LEX_STRING_WITH_LEN(MYSQL_SCHEMA_NAME),
    XSTRING_WITH_LEN(VERS_VTMD_TEMPLATE),
    VERS_VTMD_TEMPLATE,
    TL_READ);

  Query_tables_backup backup(thd);
  thd->lex->sql_command= backup.get().sql_command;
  thd->lex->add_to_query_tables(&src_table);

  MDL_auto_lock mdl_lock(thd, table);
  if (mdl_lock.acquire_error())
    return true;

  Reprepare_observer *reprepare_observer= thd->m_reprepare_observer;
  partition_info *work_part_info= thd->work_part_info;
  thd->m_reprepare_observer= NULL;
  thd->work_part_info= NULL;
  bool rc= mysql_create_like_table(thd, &table, &src_table, &create_info);
  thd->m_reprepare_observer= reprepare_observer;
  thd->work_part_info= work_part_info;
  return rc;
}

bool
VTMD_table::find_record(ulonglong sys_trx_end, bool &found)
{
  int error;
  key_buf_t key;
  found= false;

  DBUG_ASSERT(vtmd);

  if (key.allocate(vtmd->s->max_unique_length))
    return true;

  DBUG_ASSERT(sys_trx_end);
  vtmd->vers_end_field()->set_notnull();
  vtmd->vers_end_field()->store(sys_trx_end, true);
  key_copy(key, vtmd->record[0], vtmd->key_info + IDX_TRX_END, 0);

  error= vtmd->file->ha_index_read_idx_map(vtmd->record[1], IDX_TRX_END,
                                            key,
                                            HA_WHOLE_KEY,
                                            HA_READ_KEY_EXACT);
  if (error)
  {
    if (error == HA_ERR_RECORD_DELETED || error == HA_ERR_KEY_NOT_FOUND)
      return false;
    vtmd->file->print_error(error, MYF(0));
    return true;
  }

  restore_record(vtmd, record[1]);

  found= true;
  return false;
}

bool
VTMD_table::update(THD *thd, const char* archive_name)
{
  TABLE_LIST vtmd_tl;
  bool result= true;
  bool close_log= false;
  bool found= false;
  bool created= false;
  int error;
  size_t an_len= 0;
  Open_tables_backup open_tables_backup;
  ulonglong save_thd_options;
  {
    Local_da local_da(thd, ER_VERS_VTMD_ERROR);

    save_thd_options= thd->variables.option_bits;
    thd->variables.option_bits&= ~OPTION_BIN_LOG;

    if (about.vers_vtmd_name(vtmd_name))
      goto quit;

    while (true) // max 2 iterations
    {
      vtmd_tl.init_one_table(
        DB_WITH_LEN(about),
        XSTRING_WITH_LEN(vtmd_name),
        vtmd_name,
        TL_WRITE_CONCURRENT_INSERT);

      vtmd= open_log_table(thd, &vtmd_tl, &open_tables_backup);
      if (vtmd)
        break;

      if (!created && local_da.is_error() && local_da.sql_errno() == ER_NO_SUCH_TABLE)
      {
        local_da.reset_diagnostics_area();
        if (create(thd))
          goto quit;
        created= true;
        continue;
      }
      goto quit;
    }
    close_log= true;

    if (!vtmd->versioned())
    {
      my_message(ER_VERS_VTMD_ERROR, "VTMD is not versioned", MYF(0));
      goto quit;
    }

    if (!created && find_record(ULONGLONG_MAX, found))
      goto quit;

    if ((error= vtmd->file->extra(HA_EXTRA_MARK_AS_LOG_TABLE)))
    {
      vtmd->file->print_error(error, MYF(0));
      goto quit;
    }

    /* Honor next number columns if present */
    vtmd->next_number_field= vtmd->found_next_number_field;

    if (vtmd->s->fields != FIELD_COUNT)
    {
      my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` unexpected fields count: %d", MYF(0),
                      vtmd->s->db.str, vtmd->s->table_name.str, vtmd->s->fields);
      goto quit;
    }

    if (archive_name)
    {
      an_len= strlen(archive_name);
      vtmd->field[FLD_ARCHIVE_NAME]->store(archive_name, an_len, table_alias_charset);
      vtmd->field[FLD_ARCHIVE_NAME]->set_notnull();
    }
    else
    {
      vtmd->field[FLD_ARCHIVE_NAME]->set_null();
    }
    vtmd->field[FLD_COL_RENAMES]->set_null();

    if (found)
    {
      if (thd->lex->sql_command == SQLCOM_CREATE_TABLE)
      {
        my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` exists and not empty!", MYF(0),
                        vtmd->s->db.str, vtmd->s->table_name.str);
        goto quit;
      }
      vtmd->mark_columns_needed_for_update(); // not needed?
      if (archive_name)
      {
        vtmd->s->versioned= false;
        error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
        vtmd->s->versioned= true;

        if (!error)
        {
          if (thd->lex->sql_command == SQLCOM_DROP_TABLE)
          {
            error= vtmd->file->ha_delete_row(vtmd->record[0]);
          }
          else
          {
            DBUG_ASSERT(thd->lex->sql_command == SQLCOM_ALTER_TABLE);
            ulonglong sys_trx_end= (ulonglong) vtmd->vers_start_field()->val_int();
            store_record(vtmd, record[1]);
            vtmd->field[FLD_NAME]->store(TABLE_NAME_WITH_LEN(about), system_charset_info);
            vtmd->field[FLD_NAME]->set_notnull();
            vtmd->field[FLD_ARCHIVE_NAME]->set_null();
            error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
            if (error)
              goto err;

            DBUG_ASSERT(an_len);
            while (true)
            { // fill archive_name of last sequential renames
              bool found;
              if (find_record(sys_trx_end, found))
                goto quit;
              if (!found || !vtmd->field[FLD_ARCHIVE_NAME]->is_null())
                break;

              store_record(vtmd, record[1]);
              vtmd->field[FLD_ARCHIVE_NAME]->store(archive_name, an_len, table_alias_charset);
              vtmd->field[FLD_ARCHIVE_NAME]->set_notnull();
              vtmd->s->versioned= false;
              error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
              vtmd->s->versioned= true;
              if (error)
                goto err;
              sys_trx_end= (ulonglong) vtmd->vers_start_field()->val_int();
            } // while (true)
          } // else (thd->lex->sql_command != SQLCOM_DROP_TABLE)
        } // if (!error)
      } // if (archive_name)
      else
      {
        vtmd->field[FLD_NAME]->store(TABLE_NAME_WITH_LEN(about), system_charset_info);
        vtmd->field[FLD_NAME]->set_notnull();
        error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
      }
    } // if (found)
    else
    {
      vtmd->field[FLD_NAME]->store(TABLE_NAME_WITH_LEN(about), system_charset_info);
      vtmd->field[FLD_NAME]->set_notnull();
      vtmd->mark_columns_needed_for_insert(); // not needed?
      error= vtmd->file->ha_write_row(vtmd->record[0]);
    }

    if (error)
    {
err:
      vtmd->file->print_error(error, MYF(0));
    }
    else
      result= local_da.is_error();
  }

quit:
  if (close_log)
    close_log_table(thd, &open_tables_backup);

  thd->variables.option_bits= save_thd_options;
  return result;
}


bool
VTMD_rename::move_archives(THD *thd, LString &new_db)
{
  TABLE_LIST vtmd_tl;
  vtmd_tl.init_one_table(
    DB_WITH_LEN(about),
    XSTRING_WITH_LEN(vtmd_name),
    vtmd_name,
    TL_READ);
  int error;
  bool rc= false;
  SString_fs archive;
  bool end_keyread= false;
  bool index_end= false;
  Open_tables_backup open_tables_backup;
  key_buf_t key;

  vtmd= open_log_table(thd, &vtmd_tl, &open_tables_backup);
  if (!vtmd)
    return true;

  if (key.allocate(vtmd->key_info[IDX_ARCHIVE_NAME].key_length))
  {
    close_log_table(thd, &open_tables_backup);
    return true;
  }

  if ((error= vtmd->file->ha_start_keyread(IDX_ARCHIVE_NAME)))
    goto err;
  end_keyread= true;

  if ((error= vtmd->file->ha_index_init(IDX_ARCHIVE_NAME, true)))
    goto err;
  index_end= true;

  error= vtmd->file->ha_index_first(vtmd->record[0]);
  while (!error)
  {
    if (!vtmd->field[FLD_ARCHIVE_NAME]->is_null())
    {
      vtmd->field[FLD_ARCHIVE_NAME]->val_str(&archive);
      key_copy(key,
                vtmd->record[0],
                &vtmd->key_info[IDX_ARCHIVE_NAME],
                vtmd->key_info[IDX_ARCHIVE_NAME].key_length,
                false);
      error= vtmd->file->ha_index_read_map(
        vtmd->record[0],
        key,
        vtmd->key_info[IDX_ARCHIVE_NAME].ext_key_part_map,
        HA_READ_PREFIX_LAST);
      if (!error)
      {
        if ((rc= move_table(thd, archive, new_db)))
          break;

        error= vtmd->file->ha_index_next(vtmd->record[0]);
      }
    }
    else
    {
      archive.length(0);
      error= vtmd->file->ha_index_next(vtmd->record[0]);
    }
  }

  if (error && error != HA_ERR_END_OF_FILE)
  {
err:
    vtmd->file->print_error(error, MYF(0));
    rc= true;
  }

  if (index_end)
    vtmd->file->ha_index_end();
  if (end_keyread)
    vtmd->file->ha_end_keyread();

  close_log_table(thd, &open_tables_backup);
  return rc;
}

bool
VTMD_rename::move_table(THD *thd, SString_fs &table_name, LString &new_db)
{
  handlerton *table_hton= NULL;
  if (!ha_table_exists(thd, about.db, table_name, &table_hton) || !table_hton)
  {
    push_warning_printf(
        thd, Sql_condition::WARN_LEVEL_WARN,
        ER_VERS_VTMD_ERROR,
        "`%s.%s` archive doesn't exist",
        about.db, table_name.ptr());
    return false;
  }

  if (ha_table_exists(thd, new_db, table_name))
  {
    my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` archive already exists!", MYF(0),
                        new_db.ptr(), table_name.ptr());
    return true;
  }

  TABLE_LIST tl;
  tl.init_one_table(
    DB_WITH_LEN(about),
    XSTRING_WITH_LEN(table_name),
    table_name,
    TL_WRITE_ONLY);
  tl.mdl_request.set_type(MDL_EXCLUSIVE);

  mysql_ha_rm_tables(thd, &tl);
  if (lock_table_names(thd, &tl, 0, thd->variables.lock_wait_timeout, 0))
    return true;
  tdc_remove_table(thd, TDC_RT_REMOVE_ALL, about.db, table_name, false);

  bool rc= mysql_rename_table(
    table_hton,
    about.db, table_name,
    new_db, table_name,
    NO_FK_CHECKS);
  if (!rc)
    query_cache_invalidate3(thd, &tl, 0);

  return rc;
}

bool
VTMD_rename::try_rename(THD *thd, LString new_db, LString new_alias, const char *archive_name)
{
  Local_da local_da(thd, ER_VERS_VTMD_ERROR);
  TABLE_LIST new_table;

  if (check_exists(thd))
    return true;

  new_table.init_one_table(
    XSTRING_WITH_LEN(new_db),
    XSTRING_WITH_LEN(new_alias),
    new_alias, TL_READ);

  if (new_table.vers_vtmd_name(vtmd_new_name))
    return true;

  if (ha_table_exists(thd, new_db, vtmd_new_name))
  {
    if (exists)
    {
      my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` table already exists!", MYF(0),
                          new_db.ptr(), vtmd_new_name.ptr());
      return true;
    }
    push_warning_printf(
        thd, Sql_condition::WARN_LEVEL_WARN,
        ER_VERS_VTMD_ERROR,
        "`%s.%s` table already exists!",
        new_db.ptr(), vtmd_new_name.ptr());
    return false;
  }

  if (!exists)
    return false;

  bool same_db= true;
  if (LString_fs(DB_WITH_LEN(about)) != LString_fs(new_db))
  {
    // Move archives before VTMD so if the operation is interrupted, it could be continued.
    if (move_archives(thd, new_db))
      return true;
    same_db= false;
  }

  TABLE_LIST vtmd_tl;
  vtmd_tl.init_one_table(
    DB_WITH_LEN(about),
    XSTRING_WITH_LEN(vtmd_name),
    vtmd_name,
    TL_WRITE_ONLY);
  vtmd_tl.mdl_request.set_type(MDL_EXCLUSIVE);

  mysql_ha_rm_tables(thd, &vtmd_tl);
  if (lock_table_names(thd, &vtmd_tl, 0, thd->variables.lock_wait_timeout, 0))
    return true;
  tdc_remove_table(thd, TDC_RT_REMOVE_ALL, about.db, vtmd_name, false);
  if (local_da.is_error()) // just safety check
    return true;
  bool rc= mysql_rename_table(hton,
    about.db, vtmd_name,
    new_db, vtmd_new_name,
    NO_FK_CHECKS);
  if (!rc)
  {
    query_cache_invalidate3(thd, &vtmd_tl, 0);
    if (same_db || archive_name || new_alias != LString(TABLE_NAME_WITH_LEN(about)))
    {
      local_da.finish();
      VTMD_table new_vtmd(new_table);
      rc= new_vtmd.update(thd, archive_name);
    }
  }
  return rc;
}

bool
VTMD_rename::revert_rename(THD *thd, LString new_db)
{
  DBUG_ASSERT(hton);
  Local_da local_da(thd, ER_VERS_VTMD_ERROR);

  TABLE_LIST vtmd_tl;
  vtmd_tl.init_one_table(
    DB_WITH_LEN(about),
    XSTRING_WITH_LEN(vtmd_new_name),
    vtmd_new_name,
    TL_WRITE_ONLY);
  vtmd_tl.mdl_request.set_type(MDL_EXCLUSIVE);
  mysql_ha_rm_tables(thd, &vtmd_tl);
  if (lock_table_names(thd, &vtmd_tl, 0, thd->variables.lock_wait_timeout, 0))
    return true;
  tdc_remove_table(thd, TDC_RT_REMOVE_ALL, new_db, vtmd_new_name, false);

  bool rc= mysql_rename_table(
    hton,
    new_db, vtmd_new_name,
    new_db, vtmd_name,
    NO_FK_CHECKS);

  if (!rc)
    query_cache_invalidate3(thd, &vtmd_tl, 0);

  return rc;
}

void VTMD_table::archive_name(
  THD* thd,
  const char* table_name,
  char* new_name,
  size_t new_name_size)
{
  const MYSQL_TIME now= thd->query_start_TIME();
  my_snprintf(new_name, new_name_size, "%s_%04d%02d%02d_%02d%02d%02d_%06d",
              table_name, now.year, now.month, now.day, now.hour, now.minute,
              now.second, now.second_part);
}

bool VTMD_table::find_archive_name(THD *thd, String &out)
{
  String vtmd_name;
  if (about.vers_vtmd_name(vtmd_name))
    return true;

  READ_RECORD info;
  int error= 0;
  SQL_SELECT *select= NULL;
  COND *conds= NULL;
  List<TABLE_LIST> dummy;
  SELECT_LEX &select_lex= thd->lex->select_lex;

  TABLE_LIST tl;
  tl.init_one_table(about.db, about.db_length, vtmd_name.ptr(),
                    vtmd_name.length(), vtmd_name.ptr(), TL_READ);

  Open_tables_backup open_tables_backup;
  if (!(vtmd= open_log_table(thd, &tl, &open_tables_backup)))
  {
    my_error(ER_VERS_VTMD_ERROR, MYF(0), "failed to open VTMD table");
    return true;
  }

  Name_resolution_context &ctx= thd->lex->select_lex.context;
  TABLE_LIST *table_list= ctx.table_list;
  TABLE_LIST *first_name_resolution_table= ctx.first_name_resolution_table;
  table_map map = tl.table->map;
  ctx.table_list= &tl;
  ctx.first_name_resolution_table= &tl;
  tl.table->map= 1;

  tl.vers_conditions= about.vers_conditions;
  if ((error= vers_setup_select(thd, &tl, &conds, &select_lex)) ||
      (error= setup_conds(thd, &tl, dummy, &conds)))
  {
    my_error(ER_VERS_VTMD_ERROR, MYF(0),
             "failed to setup conditions for querying VTMD table");
    goto err;
  }

  select= make_select(tl.table, 0, 0, conds, NULL, 0, &error);
  if (error)
    goto loc_err;
  if ((error=
           init_read_record(&info, thd, tl.table, select, NULL, 1, 1, false)))
    goto loc_err;

  while (!(error= info.read_record(&info)) && !thd->killed && !thd->is_error())
  {
    if (select->skip_record(thd) > 0)
    {
      tl.table->field[FLD_ARCHIVE_NAME]->val_str(&out);

      if (out.length() == 0)
      {
          // Handle AS OF NOW or just RENAMEd case
          out.set(about.table_name, about.table_name_length,
                  system_charset_info);
      }
      break;
    }
  }

loc_err:
  if (error)
    my_error(ER_VERS_VTMD_ERROR, MYF(0), "failed to query VTMD table");

  end_read_record(&info);
err:
  delete select;
  ctx.table_list= table_list;
  ctx.first_name_resolution_table= first_name_resolution_table;
  tl.table->map= map;
  close_log_table(thd, &open_tables_backup);
  return error ? true : false;
}
