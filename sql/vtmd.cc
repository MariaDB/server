#include "vtmd.h"
#include "sql_base.h"
#include "sql_class.h"
#include "sql_handler.h" // mysql_ha_rm_tables()
#include "sql_table.h"
#include "sql_select.h"
#include "table_cache.h" // tdc_remove_table()
#include "key.h"
#include "sql_show.h"
#include "sql_parse.h"
#include "sql_lex.h"
#include "sp_head.h"
#include "sp_rcontext.h"

LString VERS_VTMD_TEMPLATE(C_STRING_WITH_LEN("vtmd_template"));

bool
VTMD_table::create(THD *thd)
{
  Table_specification_st create_info;
  TABLE_LIST src_table, table;
  create_info.init(DDL_options_st::OPT_LIKE);
  create_info.options|= HA_VTMD;
  create_info.alias.str= vtmd_name.ptr();
  create_info.alias.length= vtmd_name.length();
  table.init_one_table(&about.db, &create_info.alias, NULL, TL_READ);
  src_table.init_one_table(&MYSQL_SCHEMA_NAME, &VERS_VTMD_TEMPLATE,
                           &VERS_VTMD_TEMPLATE, TL_READ);

  Query_tables_backup backup(thd);
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
VTMD_table::find_record(ulonglong row_end, bool &found)
{
  int error;
  key_buf_t key;
  found= false;

  DBUG_ASSERT(vtmd.table);

  if (key.allocate(vtmd.table->s->max_unique_length))
    return true;

  DBUG_ASSERT(row_end);
  vtmd.table->vers_end_field()->set_notnull();
  vtmd.table->vers_end_field()->store(row_end, true);
  key_copy(key, vtmd.table->record[0], vtmd.table->key_info + IDX_TRX_END, 0);

  error= vtmd.table->file->ha_index_read_idx_map(vtmd.table->record[1], IDX_TRX_END,
                                            key,
                                            HA_WHOLE_KEY,
                                            HA_READ_KEY_EXACT);
  if (error)
  {
    if (error == HA_ERR_RECORD_DELETED || error == HA_ERR_KEY_NOT_FOUND)
      return false;
    vtmd.table->file->print_error(error, MYF(0));
    return true;
  }

  restore_record(vtmd.table, record[1]);

  found= true;
  return false;
}


bool
VTMD_table::open(THD *thd, Local_da &local_da, bool *created)
{
  if (created)
    *created= false;

  if (0 == vtmd_name.length() && about.vers_vtmd_name(vtmd_name))
    return true;

  while (true) // max 2 iterations
  {
    LEX_CSTRING table_name= { vtmd_name.ptr(), vtmd_name.length() };
    vtmd.init_one_table(&about.db, &table_name, NULL,
                        TL_WRITE_CONCURRENT_INSERT);

    TABLE *res= open_log_table(thd, &vtmd, &open_tables_backup);
    if (res)
      return false;

    if (created && !*created && local_da.is_error() &&
        local_da.sql_errno() == ER_NO_SUCH_TABLE)
    {
      local_da.reset_diagnostics_area();
      if (create(thd))
        break;
      *created= true;
    }
    else
      break;
  }
  return true;
}

bool
VTMD_table::update(THD *thd, const char* archive_name)
{
  bool result= true;
  bool found= false;
  bool created;
  int error;
  size_t an_len= 0;
  ulonglong save_thd_options;
  {
    Local_da local_da(thd, ER_VERS_VTMD_ERROR);

    save_thd_options= thd->variables.option_bits;
    thd->variables.option_bits&= ~OPTION_BIN_LOG;

    if (open(thd, local_da, &created))
      goto open_error;

    if (!vtmd.table->versioned())
    {
      my_message(ER_VERS_VTMD_ERROR, "VTMD is not versioned", MYF(0));
      goto quit;
    }

    if (!created && find_record(ULONGLONG_MAX, found))
      goto quit;

    if ((error= vtmd.table->file->extra(HA_EXTRA_MARK_AS_LOG_TABLE)))
    {
      vtmd.table->file->print_error(error, MYF(0));
      goto quit;
    }

    /* Honor next number columns if present */
    vtmd.table->next_number_field= vtmd.table->found_next_number_field;

    if (vtmd.table->s->fields != FIELD_COUNT)
    {
      my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` unexpected fields count: %d", MYF(0),
                      vtmd.table->s->db.str, vtmd.table->s->table_name.str, vtmd.table->s->fields);
      goto quit;
    }

    if (archive_name)
    {
      an_len= strlen(archive_name);
      vtmd.table->field[FLD_ARCHIVE_NAME]->store(archive_name, an_len, table_alias_charset);
      vtmd.table->field[FLD_ARCHIVE_NAME]->set_notnull();
    }
    else
    {
      vtmd.table->field[FLD_ARCHIVE_NAME]->set_null();
    }
    vtmd.table->field[FLD_COL_RENAMES]->set_null();

    if (found)
    {
      if (thd->lex->sql_command == SQLCOM_CREATE_TABLE)
      {
        my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` exists and not empty!", MYF(0),
                        vtmd.table->s->db.str, vtmd.table->s->table_name.str);
        goto quit;
      }
      vtmd.table->mark_columns_needed_for_update(); // not needed?
      if (archive_name)
      {
        vtmd.table->vers_write= false;
        error= vtmd.table->file->ha_update_row(vtmd.table->record[1], vtmd.table->record[0]);
        vtmd.table->vers_write= true;

        if (!error)
        {
          if (thd->lex->sql_command == SQLCOM_DROP_TABLE)
          {
            error= vtmd.table->file->ha_delete_row(vtmd.table->record[0]);
          }
          else
          {
            DBUG_ASSERT(thd->lex->sql_command == SQLCOM_ALTER_TABLE);
            ulonglong row_end= (ulonglong) vtmd.table->vers_start_field()->val_int();
            store_record(vtmd.table, record[1]);
            vtmd.table->field[FLD_NAME]->store(about.table_name.str, about.table_name.length, system_charset_info);
            vtmd.table->field[FLD_NAME]->set_notnull();
            vtmd.table->field[FLD_ARCHIVE_NAME]->set_null();
            error= vtmd.table->file->ha_update_row(vtmd.table->record[1], vtmd.table->record[0]);
            if (error)
              goto err;

            DBUG_ASSERT(an_len);
            while (true)
            { // fill archive_name of last sequential renames
              bool found;
              if (find_record(row_end, found))
                goto quit;
              if (!found || !vtmd.table->field[FLD_ARCHIVE_NAME]->is_null())
                break;

              store_record(vtmd.table, record[1]);
              vtmd.table->field[FLD_ARCHIVE_NAME]->store(archive_name, an_len, table_alias_charset);
              vtmd.table->field[FLD_ARCHIVE_NAME]->set_notnull();
              vtmd.table->vers_write= false;
              error= vtmd.table->file->ha_update_row(vtmd.table->record[1], vtmd.table->record[0]);
              vtmd.table->vers_write= true;
              if (error)
                goto err;
              row_end= (ulonglong) vtmd.table->vers_start_field()->val_int();
            } // while (true)
          } // else (thd->lex->sql_command != SQLCOM_DROP_TABLE)
        } // if (!error)
      } // if (archive_name)
      else
      {
        vtmd.table->field[FLD_NAME]->store(about.table_name.str,
                                           about.table_name.length,
                                           system_charset_info);
        vtmd.table->field[FLD_NAME]->set_notnull();
        error= vtmd.table->file->ha_update_row(vtmd.table->record[1],
                                               vtmd.table->record[0]);
      }
    } // if (found)
    else
    {
      vtmd.table->field[FLD_NAME]->store(about.table_name.str,
                                         about.table_name.length,
                                         system_charset_info);
      vtmd.table->field[FLD_NAME]->set_notnull();
      vtmd.table->mark_columns_needed_for_insert(); // not needed?
      error= vtmd.table->file->ha_write_row(vtmd.table->record[0]);
    }

    if (error)
    {
err:
      vtmd.table->file->print_error(error, MYF(0));
    }
    else
      result= local_da.is_error();
  }

quit:
  if (!result && vtmd.table->file->ht->prepare_commit_versioned)
  {
    DBUG_ASSERT(TR_table::use_transaction_registry); // FIXME: disable survival mode while TRT is disabled
    TR_table trt(thd, true);
    ulonglong trx_start_id= 0;
    ulonglong trx_end_id= vtmd.table->file->ht->prepare_commit_versioned(thd, &trx_start_id);
    result= trx_end_id && trt.update(trx_start_id, trx_end_id);
  }

  close_log_table(thd, &open_tables_backup);

open_error:
  thd->variables.option_bits= save_thd_options;
  return result;
}

bool
VTMD_rename::move_archives(THD *thd, LString &new_db)
{
  int error;
  bool rc= false;
  SString_fs archive;
  bool end_keyread= false;
  bool index_end= false;
  Open_tables_backup open_tables_backup;
  key_buf_t key;

  LEX_CSTRING table_name= { vtmd_name.ptr(), vtmd_name.length() };
  vtmd.init_one_table(&about.db, &table_name, NULL, TL_READ);

  TABLE *res= open_log_table(thd, &vtmd, &open_tables_backup);
  if (!res)
    return true;

  if (key.allocate(vtmd.table->key_info[IDX_ARCHIVE_NAME].key_length))
  {
    close_log_table(thd, &open_tables_backup);
    return true;
  }

  if ((error= vtmd.table->file->ha_start_keyread(IDX_ARCHIVE_NAME)))
    goto err;
  end_keyread= true;

  if ((error= vtmd.table->file->ha_index_init(IDX_ARCHIVE_NAME, true)))
    goto err;
  index_end= true;

  error= vtmd.table->file->ha_index_first(vtmd.table->record[0]);
  while (!error)
  {
    if (!vtmd.table->field[FLD_ARCHIVE_NAME]->is_null())
    {
      vtmd.table->field[FLD_ARCHIVE_NAME]->val_str(&archive);
      key_copy(key,
                vtmd.table->record[0],
                &vtmd.table->key_info[IDX_ARCHIVE_NAME],
                vtmd.table->key_info[IDX_ARCHIVE_NAME].key_length,
                false);
      error= vtmd.table->file->ha_index_read_map(
        vtmd.table->record[0],
        key,
        vtmd.table->key_info[IDX_ARCHIVE_NAME].ext_key_part_map,
        HA_READ_PREFIX_LAST);
      if (!error)
      {
        if ((rc= move_table(thd, archive, new_db)))
          break;

        error= vtmd.table->file->ha_index_next(vtmd.table->record[0]);
      }
    }
    else
    {
      archive.length(0);
      error= vtmd.table->file->ha_index_next(vtmd.table->record[0]);
    }
  }

  if (error && error != HA_ERR_END_OF_FILE)
  {
err:
    vtmd.table->file->print_error(error, MYF(0));
    rc= true;
  }

  if (index_end)
    vtmd.table->file->ha_index_end();
  if (end_keyread)
    vtmd.table->file->ha_end_keyread();

  close_log_table(thd, &open_tables_backup);
  return rc;
}

bool
VTMD_rename::move_table(THD *thd, SString_fs &table_name, LString &new_db)
{
  handlerton *table_hton= NULL;
  LEX_CSTRING tbl_name= { table_name.ptr(), table_name.length() };
  LEX_CSTRING db_name=  { new_db.ptr(), new_db.length() };
  if (!ha_table_exists(thd, &about.db, &tbl_name, &table_hton) || !table_hton)
  {
    push_warning_printf( thd, Sql_condition::WARN_LEVEL_WARN,
                         ER_VERS_VTMD_ERROR,
                         "`%s.%s` archive doesn't exist",
                         about.db.str, tbl_name.str);
    return false;
  }

  if (ha_table_exists(thd, &db_name, &tbl_name))
  {
    my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` archive already exists!",
                    MYF(0),
                    db_name.str, tbl_name.str);
    return true;
  }

  TABLE_LIST tl;
  tl.init_one_table(&about.db, &tbl_name, NULL, TL_WRITE_ONLY);
  tl.mdl_request.set_type(MDL_EXCLUSIVE);

  mysql_ha_rm_tables(thd, &tl);
  if (lock_table_names(thd, &tl, 0, thd->variables.lock_wait_timeout, 0))
    return true;
  tdc_remove_table(thd, TDC_RT_REMOVE_ALL, about.db.str, table_name, false);

  bool rc= mysql_rename_table(table_hton,
                              &about.db, &tbl_name, &db_name, &tbl_name,
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

  LEX_CSTRING new_db_name=  { XSTRING_WITH_LEN(new_db) };
  LEX_CSTRING new_tbl_name= { XSTRING_WITH_LEN(new_alias) };

  new_table.init_one_table(&new_db_name, &new_tbl_name, NULL, TL_READ);

  if (new_table.vers_vtmd_name(vtmd_new_name))
    return true;

  LEX_CSTRING new_name= { vtmd_new_name.ptr(), vtmd_new_name.length() };

  if (ha_table_exists(thd, &new_db_name, &new_name))
  {
    if (exists)
    {
      my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` table already exists!",
                      MYF(0),
                      new_db_name.str, new_name.str);
      return true;
    }
    push_warning_printf( thd, Sql_condition::WARN_LEVEL_WARN,
                         ER_VERS_VTMD_ERROR,
                         "`%s.%s` table already exists!",
                         new_db_name.str, new_name.str);
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
  LEX_CSTRING table_name= { vtmd_name.ptr(), vtmd_name.length() };
  vtmd_tl.init_one_table(&about.db, &table_name, NULL, TL_WRITE_ONLY);
  vtmd_tl.mdl_request.set_type(MDL_EXCLUSIVE);

  mysql_ha_rm_tables(thd, &vtmd_tl);
  if (lock_table_names(thd, &vtmd_tl, 0, thd->variables.lock_wait_timeout, 0))
    return true;
  tdc_remove_table(thd, TDC_RT_REMOVE_ALL, about.db.str, vtmd_name, false);
  if (local_da.is_error()) // just safety check
    return true;
  bool rc= mysql_rename_table(hton, &about.db, &table_name,
                              &new_db_name, &new_name,
                              NO_FK_CHECKS);
  if (!rc)
  {
    query_cache_invalidate3(thd, &vtmd_tl, 0);
    if (same_db || archive_name ||
        new_alias != LString(TABLE_NAME_WITH_LEN(about)))
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
  LEX_CSTRING new_name=      { XSTRING_WITH_LEN(vtmd_new_name) };
  LEX_CSTRING v_name=        { XSTRING_WITH_LEN(vtmd_name) };
  LEX_CSTRING new_db_name=   { XSTRING_WITH_LEN(new_db) };

  vtmd_tl.init_one_table(&about.db, &new_name, NULL, TL_WRITE_ONLY);
  vtmd_tl.mdl_request.set_type(MDL_EXCLUSIVE);
  mysql_ha_rm_tables(thd, &vtmd_tl);
  if (lock_table_names(thd, &vtmd_tl, 0, thd->variables.lock_wait_timeout, 0))
    return true;
  tdc_remove_table(thd, TDC_RT_REMOVE_ALL, new_db, vtmd_new_name, false);

  bool rc= mysql_rename_table(hton, &new_db_name, &new_name,
                              &new_db_name, &v_name,
                              NO_FK_CHECKS);

  if (!rc)
    query_cache_invalidate3(thd, &vtmd_tl, 0);

  return rc;
}

void
VTMD_table::archive_name(
  THD* thd,
  const char* table_name,
  char* new_name,
  size_t new_name_size)
{
  const MYSQL_TIME now= thd->query_start_TIME();
  my_snprintf(new_name, new_name_size, "%s_%04d%02d%02d_%02d%02d%02d_%06lu",
              table_name, now.year, now.month, now.day, now.hour, now.minute,
              now.second, (ulong) now.second_part);
}

bool
VTMD_table::find_archive_name(THD *thd, String &out)
{
  READ_RECORD info;
  int error;
  SQL_SELECT *select= NULL;
  COND *conds= NULL;
  List<TABLE_LIST> dummy;
  SELECT_LEX &select_lex= thd->lex->select_lex;

  Local_da local_da(thd, ER_VERS_VTMD_ERROR);
  if (open(thd, local_da))
    return true;

  Name_resolution_context &ctx= thd->lex->select_lex.context;
  TABLE_LIST *table_list= ctx.table_list;
  TABLE_LIST *first_name_resolution_table= ctx.first_name_resolution_table;
  table_map map = vtmd.table->map;
  ctx.table_list= &vtmd;
  ctx.first_name_resolution_table= &vtmd;
  vtmd.table->map= 1;

  vtmd.vers_conditions= about.vers_conditions;
  if ((error= select_lex.vers_setup_conds(thd, &vtmd, &conds)) ||
      (error= setup_conds(thd, &vtmd, dummy, &conds)))
    goto err;

  select= make_select(vtmd.table, 0, 0, conds, NULL, 0, &error);
  if (error)
    goto loc_err;

  error= init_read_record(&info, thd, vtmd.table, select, NULL,
                          1 /* use_record_cache */, true /* print_error */,
                          false /* disable_rr_cache */);
  if (error)
    goto loc_err;

  while (!(error= info.read_record()) && !thd->killed && !thd->is_error())
  {
    if (!select || select->skip_record(thd) > 0)
    {
      vtmd.table->field[FLD_ARCHIVE_NAME]->val_str(&out);
      break;
    }
  }

  if (error < 0)
    my_error(ER_NO_SUCH_TABLE, MYF(0), about.db.str, about.alias.str);

loc_err:
  end_read_record(&info);
err:
  delete select;
  ctx.table_list= table_list;
  ctx.first_name_resolution_table= first_name_resolution_table;
  vtmd.table->map= map;
  close_log_table(thd, &open_tables_backup);
  DBUG_ASSERT(!error || local_da.is_error());
  return error;
}

static
bool
get_vtmd_tables(THD *thd, const char *db,
                size_t db_length, Dynamic_array<LEX_CSTRING *> &table_names)
{
  LOOKUP_FIELD_VALUES lookup_field_values= {
    {db, db_length}, {C_STRING_WITH_LEN("%_vtmd")}, false, true};

  int res= make_table_name_list(thd, &table_names, thd->lex, &lookup_field_values,
                           &lookup_field_values.db_value);

  return res;
}

bool
VTMD_table::get_archive_tables(THD *thd, const char *db, size_t db_length,
                               Dynamic_array<String> &result)
{
  Dynamic_array<LEX_CSTRING *> vtmd_tables;
  if (get_vtmd_tables(thd, db, db_length, vtmd_tables))
    return true;

  Local_da local_da(thd, ER_VERS_VTMD_ERROR);
  for (uint i= 0; i < vtmd_tables.elements(); i++)
  {
    LEX_CSTRING table_name= *vtmd_tables.at(i);
    Open_tables_backup open_tables_backup;
    TABLE_LIST table_list;
    LEX_CSTRING db_name= {db, db_length};
    table_list.init_one_table(&db_name, &table_name, NULL, TL_READ);

    TABLE *table= open_log_table(thd, &table_list, &open_tables_backup);
    if (!table || !table->vers_vtmd())
    {
      if (table)
        close_log_table(thd, &open_tables_backup);
      else
      {
        if (local_da.is_error() && local_da.sql_errno() == ER_NOT_LOG_TABLE)
          local_da.reset_diagnostics_area();
        else
          return true;
      }
      push_warning_printf(
        thd, Sql_condition::WARN_LEVEL_WARN,
        ER_VERS_VTMD_ERROR,
        "Table `%s.%s` is not a VTMD table",
        db, table_name.str);
      continue;
    }

    READ_RECORD read_record;
    int error= 0;
    SQL_SELECT *sql_select= make_select(table, 0, 0, NULL, NULL, 0, &error);
    if (error)
    {
      close_log_table(thd, &open_tables_backup);
      return true;
    }
    error= init_read_record(&read_record, thd, table, sql_select, NULL, 1, 1, false);
    if (error)
    {
      delete sql_select;
      close_log_table(thd, &open_tables_backup);
      return true;
    }

    while (!(error= read_record.read_record()))
    {
      Field *field= table->field[FLD_ARCHIVE_NAME];
      if (field->is_null())
        continue;

      String archive_name;
      field->val_str(&archive_name);
      archive_name.set_ascii(strmake_root(thd->mem_root, archive_name.c_ptr(),
                                          archive_name.length()),
                             archive_name.length());
      result.push(archive_name);
    }
    // check for EOF
    if (!thd->is_error())
      error= 0;

    end_read_record(&read_record);
    delete sql_select;
    close_log_table(thd, &open_tables_backup);
  }

  return false;
}

bool VTMD_table::setup_select(THD* thd)
{
  SString archive_name;
  if (find_archive_name(thd, archive_name))
    return true;

  if (archive_name.length() == 0)
    return false;

  thd->make_lex_string(&about.table_name, archive_name.ptr(),
                       archive_name.length());
  DBUG_ASSERT(!about.mdl_request.ticket);
  about.mdl_request.init(MDL_key::TABLE, about.db.str, about.table_name.str,
                         about.mdl_request.type, about.mdl_request.duration);
  about.vers_force_alias= true;
  // Since we modified SELECT_LEX::table_list, we need to invalidate current SP
  if (thd->spcont)
  {
    DBUG_ASSERT(thd->spcont->m_sp);
    thd->spcont->m_sp->set_sp_cache_version(0);
  }
  return false;
}
