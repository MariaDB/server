/* Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2016, 2020, MariaDB Corporation

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
#include "sql_parse.h"                       // check_access
#include "sql_table.h"                       // mysql_alter_table,
                                             // mysql_exchange_partition
#include "sql_statistics.h"                  // delete_statistics_for_column
#include "sql_alter.h"
#include "rpl_mi.h"
#include "slave.h"
#include "debug_sync.h"
#include "wsrep_mysqld.h"

Alter_info::Alter_info(const Alter_info &rhs, MEM_ROOT *mem_root)
  :drop_list(rhs.drop_list, mem_root),
  alter_list(rhs.alter_list, mem_root),
  key_list(rhs.key_list, mem_root),
  alter_rename_key_list(rhs.alter_rename_key_list, mem_root),
  create_list(rhs.create_list, mem_root),
  alter_index_ignorability_list(rhs.alter_index_ignorability_list, mem_root),
  check_constraint_list(rhs.check_constraint_list, mem_root),
  flags(rhs.flags), partition_flags(rhs.partition_flags),
  keys_onoff(rhs.keys_onoff),
  original_table(0),
  partition_names(rhs.partition_names, mem_root),
  num_parts(rhs.num_parts),
  requested_algorithm(rhs.requested_algorithm),
  requested_lock(rhs.requested_lock)
{
  /*
    Make deep copies of used objects.
    This is not a fully deep copy - clone() implementations
    of Alter_drop, Alter_column, Key, foreign_key, Key_part_spec
    do not copy string constants. At the same length the only
    reason we make a copy currently is that ALTER/CREATE TABLE
    code changes input Alter_info definitions, but string
    constants never change.
  */
  list_copy_and_replace_each_value(drop_list, mem_root);
  list_copy_and_replace_each_value(alter_list, mem_root);
  list_copy_and_replace_each_value(key_list, mem_root);
  list_copy_and_replace_each_value(alter_rename_key_list, mem_root);
  list_copy_and_replace_each_value(create_list, mem_root);
  /* partition_names are not deeply copied currently */
}


bool Alter_info::set_requested_algorithm(const LEX_CSTRING *str)
{
  // To avoid adding new keywords to the grammar, we match strings here.
  if (lex_string_eq(str, STRING_WITH_LEN("INPLACE")))
    requested_algorithm= ALTER_TABLE_ALGORITHM_INPLACE;
  else if (lex_string_eq(str, STRING_WITH_LEN("COPY")))
    requested_algorithm= ALTER_TABLE_ALGORITHM_COPY;
  else if (lex_string_eq(str, STRING_WITH_LEN("DEFAULT")))
    requested_algorithm= ALTER_TABLE_ALGORITHM_DEFAULT;
  else if (lex_string_eq(str, STRING_WITH_LEN("NOCOPY")))
    requested_algorithm= ALTER_TABLE_ALGORITHM_NOCOPY;
  else if (lex_string_eq(str, STRING_WITH_LEN("INSTANT")))
    requested_algorithm= ALTER_TABLE_ALGORITHM_INSTANT;
  else
    return true;
  return false;
}

void Alter_info::set_requested_algorithm(enum_alter_table_algorithm algo_val)
{
  requested_algorithm= algo_val;
}

bool Alter_info::set_requested_lock(const LEX_CSTRING *str)
{
  // To avoid adding new keywords to the grammar, we match strings here.
  if (lex_string_eq(str, STRING_WITH_LEN("NONE")))
    requested_lock= ALTER_TABLE_LOCK_NONE;
  else if (lex_string_eq(str, STRING_WITH_LEN("SHARED")))
    requested_lock= ALTER_TABLE_LOCK_SHARED;
  else if (lex_string_eq(str, STRING_WITH_LEN("EXCLUSIVE")))
    requested_lock= ALTER_TABLE_LOCK_EXCLUSIVE;
  else if (lex_string_eq(str, STRING_WITH_LEN("DEFAULT")))
    requested_lock= ALTER_TABLE_LOCK_DEFAULT;
  else
    return true;
  return false;
}

const char* Alter_info::algorithm_clause(THD *thd) const
{
  switch (algorithm(thd)) {
  case ALTER_TABLE_ALGORITHM_INPLACE:
    return "ALGORITHM=INPLACE";
  case ALTER_TABLE_ALGORITHM_COPY:
    return "ALGORITHM=COPY";
  case ALTER_TABLE_ALGORITHM_NONE:
    DBUG_ASSERT(0);
    /* Fall through */
  case ALTER_TABLE_ALGORITHM_DEFAULT:
    return "ALGORITHM=DEFAULT";
  case ALTER_TABLE_ALGORITHM_NOCOPY:
    return "ALGORITHM=NOCOPY";
  case ALTER_TABLE_ALGORITHM_INSTANT:
    return "ALGORITHM=INSTANT";
  }

  return NULL; /* purecov: begin deadcode */
}

const char* Alter_info::lock() const
{
  switch (requested_lock) {
  case ALTER_TABLE_LOCK_SHARED:
    return "LOCK=SHARED";
  case ALTER_TABLE_LOCK_NONE:
    return "LOCK=NONE";
  case ALTER_TABLE_LOCK_DEFAULT:
    return "LOCK=DEFAULT";
  case ALTER_TABLE_LOCK_EXCLUSIVE:
    return "LOCK=EXCLUSIVE";
  }
  return NULL; /* purecov: begin deadcode */
}


bool Alter_info::supports_algorithm(THD *thd,
                                    const Alter_inplace_info *ha_alter_info)
{
  switch (ha_alter_info->inplace_supported) {
  case HA_ALTER_INPLACE_EXCLUSIVE_LOCK:
  case HA_ALTER_INPLACE_SHARED_LOCK:
  case HA_ALTER_INPLACE_NO_LOCK:
  case HA_ALTER_INPLACE_INSTANT:
     return false;
  case HA_ALTER_INPLACE_COPY_NO_LOCK:
  case HA_ALTER_INPLACE_COPY_LOCK:
    if (algorithm(thd) >= Alter_info::ALTER_TABLE_ALGORITHM_NOCOPY)
    {
      ha_alter_info->report_unsupported_error(algorithm_clause(thd),
                                              "ALGORITHM=INPLACE");
      return true;
    }
    return false;
  case HA_ALTER_INPLACE_NOCOPY_NO_LOCK:
  case HA_ALTER_INPLACE_NOCOPY_LOCK:
    if (algorithm(thd) == Alter_info::ALTER_TABLE_ALGORITHM_INSTANT)
    {
      ha_alter_info->report_unsupported_error("ALGORITHM=INSTANT",
                                              "ALGORITHM=NOCOPY");
      return true;
    }
    return false;
  case HA_ALTER_INPLACE_NOT_SUPPORTED:
    if (algorithm(thd) >= Alter_info::ALTER_TABLE_ALGORITHM_INPLACE)
    {
      ha_alter_info->report_unsupported_error(algorithm_clause(thd),
					      "ALGORITHM=COPY");
      return true;
    }
    return false;
  case HA_ALTER_ERROR:
    return true;
  }
  /* purecov: begin deadcode */
  DBUG_ASSERT(0);
  return false;
}


bool Alter_info::supports_lock(THD *thd, bool online,
                               Alter_inplace_info *ha_alter_info)
{
  switch (ha_alter_info->inplace_supported) {
  case HA_ALTER_INPLACE_EXCLUSIVE_LOCK:
    // If SHARED lock and no particular algorithm was requested, use COPY.
    if (requested_lock == Alter_info::ALTER_TABLE_LOCK_SHARED &&
        algorithm(thd) == Alter_info::ALTER_TABLE_ALGORITHM_DEFAULT &&
        thd->variables.alter_algorithm ==
                Alter_info::ALTER_TABLE_ALGORITHM_DEFAULT)
         return false;

    if (requested_lock == Alter_info::ALTER_TABLE_LOCK_SHARED ||
        requested_lock == Alter_info::ALTER_TABLE_LOCK_NONE)
    {
      ha_alter_info->report_unsupported_error(lock(), "LOCK=EXCLUSIVE");
      return true;
    }
    return false;
  case HA_ALTER_INPLACE_NO_LOCK:
  case HA_ALTER_INPLACE_INSTANT:
  case HA_ALTER_INPLACE_COPY_NO_LOCK:
  case HA_ALTER_INPLACE_NOCOPY_NO_LOCK:
    return false;
  case HA_ALTER_INPLACE_COPY_LOCK:
  case HA_ALTER_INPLACE_NOCOPY_LOCK:
  case HA_ALTER_INPLACE_NOT_SUPPORTED:
  case HA_ALTER_INPLACE_SHARED_LOCK:
    if (requested_lock == Alter_info::ALTER_TABLE_LOCK_NONE)
    {
      if (online)
        ha_alter_info->inplace_supported= HA_ALTER_INPLACE_NOT_SUPPORTED;
      else
      {
        ha_alter_info->report_unsupported_error("LOCK=NONE", "LOCK=SHARED");
        return true;
      }
    }
    return false;
  case HA_ALTER_ERROR:
    return true;
  }
  /* purecov: begin deadcode */
  DBUG_ASSERT(0);
  return false;
}

bool Alter_info::vers_prohibited(THD *thd) const
{
  if (thd->slave_thread ||
      thd->variables.vers_alter_history != VERS_ALTER_HISTORY_ERROR)
  {
    return false;
  }
  if (flags & (
    ALTER_PARSER_ADD_COLUMN |
    ALTER_PARSER_DROP_COLUMN |
    ALTER_CHANGE_COLUMN |
    ALTER_COLUMN_ORDER))
  {
    return true;
  }
  if (flags & ALTER_ADD_INDEX)
  {
    List_iterator_fast<Key> key_it(const_cast<List<Key> &>(key_list));
    Key *key;
    while ((key= key_it++))
    {
      if (key->type == Key::PRIMARY || key->type == Key::UNIQUE)
        return true;
    }
  }
  return false;
}

Alter_info::enum_alter_table_algorithm
Alter_info::algorithm(const THD *thd) const
{
  if (requested_algorithm == ALTER_TABLE_ALGORITHM_NONE)
   return (Alter_info::enum_alter_table_algorithm) thd->variables.alter_algorithm;
  return requested_algorithm;
}

bool Alter_info::algorithm_is_nocopy(const THD *thd) const
{
  auto alg= algorithm(thd);
  return alg == ALTER_TABLE_ALGORITHM_INPLACE
         || alg == ALTER_TABLE_ALGORITHM_INSTANT
         || alg == ALTER_TABLE_ALGORITHM_NOCOPY;
}


uint Alter_info::check_vcol_field(Item_field *item) const
{
  /*
    vcol->flags are modified in-place, so we'll need to reset them
    if ALTER fails for any reason
  */
  if (item->field && !item->field->table->needs_reopen())
    item->field->table->mark_table_for_reopen();

  if (!item->field &&
      ((item->db_name.length && !db.streq(item->db_name)) ||
       (item->table_name.length && !table_name.streq(item->table_name))))
  {
    char *ptr= (char*)current_thd->alloc(item->db_name.length +
                                         item->table_name.length +
                                         item->field_name.length + 3);
    strxmov(ptr, safe_str(item->db_name.str), item->db_name.length ? "." : "",
            item->table_name.str, ".", item->field_name.str, NullS);
    item->field_name.str= ptr;
    return VCOL_IMPOSSIBLE;
  }
  for (Key &k: key_list)
  {
    if (k.type != Key::FOREIGN_KEY)
      continue;
    Foreign_key *fk= (Foreign_key*) &k;
    if (fk->update_opt < FK_OPTION_CASCADE &&
        fk->delete_opt < FK_OPTION_SET_NULL)
      continue;
    for (Key_part_spec& kp: fk->columns)
    {
      if (item->field_name.streq(kp.field_name))
        return VCOL_NON_DETERMINISTIC;
    }
  }
  for (Create_field &cf: create_list)
  {
    if (item->field_name.streq(cf.field_name))
      return cf.vcol_info ? cf.vcol_info->flags : 0;
  }
  return 0;
}


bool Alter_info::collect_renamed_fields(THD *thd)
{
  List_iterator_fast<Create_field> new_field_it;
  Create_field *new_field;
  DBUG_ENTER("Alter_info::collect_renamed_fields");

  new_field_it.init(create_list);
  while ((new_field= new_field_it++))
  {
    Field *field= new_field->field;

    if (new_field->field &&
        cmp(&field->field_name, &new_field->field_name))
    {
      field->flags|= FIELD_IS_RENAMED;
      if (add_stat_rename_field(field,
                                &new_field->field_name,
                                thd->mem_root))
        DBUG_RETURN(true);

    }
  }
  DBUG_RETURN(false);
}


/*
  Delete duplicate index found during mysql_prepare_create_table()

  Notes:
    - In case of temporary generated foreign keys, the key_name may not
      be set!  These keys are ignored.
*/

bool Alter_info::add_stat_drop_index(THD *thd, const LEX_CSTRING *key_name)
{
  if (original_table && key_name->length)       // If from alter table
  {
    KEY *key_info= original_table->key_info;
    for (uint i= 0; i < original_table->s->keys; i++, key_info++)
    {
      if (key_info->name.length &&
          !lex_string_cmp(system_charset_info, &key_info->name,
                          key_name))
        return add_stat_drop_index(key_info, false, thd->mem_root);
    }
  }
  return false;
}


void Alter_info::apply_statistics_deletes_renames(THD *thd, TABLE *table)
{
  List_iterator<Field>                     it_drop_field(drop_stat_fields);
  List_iterator<RENAME_COLUMN_STAT_PARAMS> it_rename_field(rename_stat_fields);
  List_iterator<DROP_INDEX_STAT_PARAMS>    it_drop_index(drop_stat_indexes);
  List_iterator<RENAME_INDEX_STAT_PARAMS>  it_rename_index(rename_stat_indexes);

  while (Field *field= it_drop_field++)
    delete_statistics_for_column(thd, table, field);

  if (!rename_stat_fields.is_empty())
    (void) rename_columns_in_stat_table(thd, table, &rename_stat_fields);

  while (DROP_INDEX_STAT_PARAMS *key= it_drop_index++)
    (void) delete_statistics_for_index(thd, table, key->key,
                                       key->ext_prefixes_only);

  if (!rename_stat_indexes.is_empty())
    (void) rename_indexes_in_stat_table(thd, table, &rename_stat_indexes);
}


Alter_table_ctx::Alter_table_ctx()
  : db(null_clex_str), table_name(null_clex_str), alias(null_clex_str),
    new_db(null_clex_str), new_name(null_clex_str), new_alias(null_clex_str)
{
}

/*
  TODO: new_name_arg changes if lower case table names.
  Should be copied or converted before call
*/

Alter_table_ctx::Alter_table_ctx(THD *thd, TABLE_LIST *table_list,
                                 uint tables_opened_arg,
                                 const LEX_CSTRING *new_db_arg,
                                 const LEX_CSTRING *new_name_arg)
  : tables_opened(tables_opened_arg),
    new_db(*new_db_arg), new_name(*new_name_arg)
{
  /*
    Assign members db, table_name, new_db and new_name
    to simplify further comparisions: we want to see if it's a RENAME
    later just by comparing the pointers, avoiding the need for strcmp.
  */
  db= table_list->db;
  table_name= table_list->table_name;
  alias= (lower_case_table_names == 2) ? table_list->alias : table_name;

  if (!new_db.str || !my_strcasecmp(table_alias_charset, new_db.str, db.str))
    new_db= db;

  if (new_name.str)
  {
    DBUG_PRINT("info", ("new_db.new_name: '%s'.'%s'", new_db.str, new_name.str));

    if (lower_case_table_names == 1) // Convert new_name/new_alias to lower
    {
      new_name.length= my_casedn_str(files_charset_info, (char*) new_name.str);
      new_alias= new_name;
    }
    else if (lower_case_table_names == 2) // Convert new_name to lower case
    {
      new_alias.str=    new_alias_buff;
      new_alias.length= new_name.length;
      strmov(new_alias_buff, new_name.str);
      new_name.length= my_casedn_str(files_charset_info, (char*) new_name.str);

    }
    else
      new_alias= new_name; // LCTN=0 => case sensitive + case preserving

    if (!is_database_changed() &&
        !my_strcasecmp(table_alias_charset, new_name.str, table_name.str))
    {
      /*
        Source and destination table names are equal:
        make is_table_renamed() more efficient.
      */
      new_alias= table_name;
      new_name= table_name;
    }
  }
  else
  {
    new_alias= alias;
    new_name= table_name;
  }

  tmp_name.str= tmp_name_buff;
  tmp_name.length= my_snprintf(tmp_name_buff, sizeof(tmp_name_buff),
                               "%s-alter-%lx-%llx",
                               tmp_file_prefix, current_pid, thd->thread_id);
  /* Safety fix for InnoDB */
  if (lower_case_table_names)
    tmp_name.length= my_casedn_str(files_charset_info, tmp_name_buff);

  if (table_list->table->s->tmp_table == NO_TMP_TABLE)
  {
    build_table_filename(path, sizeof(path) - 1, db.str, table_name.str, "", 0);

    build_table_filename(new_path, sizeof(new_path) - 1, new_db.str, new_name.str, "", 0);

    build_table_filename(new_filename, sizeof(new_filename) - 1,
                         new_db.str, new_name.str, reg_ext, 0);

    build_table_filename(tmp_path, sizeof(tmp_path) - 1, new_db.str, tmp_name.str, "",
                         FN_IS_TMP);
  }
  else
  {
    /*
      We are not filling path, new_path and new_filename members if
      we are altering temporary table as these members are not used in
      this case. This fact is enforced with assert.
    */
    build_tmptable_filename(thd, tmp_path, sizeof(tmp_path));
    tmp_table= true;
  }
  if ((id.length= table_list->table->s->tabledef_version.length))
    memcpy(id_buff, table_list->table->s->tabledef_version.str, MY_UUID_SIZE);
  id.str= id_buff;
  storage_engine_partitioned= table_list->table->file->partition_engine();
  storage_engine_name.str= storage_engine_buff;
  storage_engine_name.length= ((strmake(storage_engine_buff,
                                        table_list->table->file->
                                        real_table_type(),
                                        sizeof(storage_engine_buff)-1)) -
                               storage_engine_buff);
  tmp_storage_engine_name.str= tmp_storage_engine_buff;
  tmp_storage_engine_name.length= 0;
  tmp_id.str= 0;
  tmp_id.length= 0;
}


void Alter_table_ctx::report_implicit_default_value_error(THD *thd,
                                                          const TABLE_SHARE *s)
                                                          const
{
  Create_field *error_field= implicit_default_value_error_field;
  const Type_handler *h= error_field->type_handler();
  thd->push_warning_truncated_value_for_field(Sql_condition::WARN_LEVEL_WARN,
                                              h->name().ptr(),
                                              h->default_value().ptr(),
                                              s ? s->db.str : nullptr,
                                              s ? s->table_name.str : nullptr,
                                              error_field->field_name.str);
}


bool Sql_cmd_alter_table::execute(THD *thd)
{
  LEX *lex= thd->lex;
  /* first SELECT_LEX (have special meaning for many of non-SELECTcommands) */
  SELECT_LEX *select_lex= lex->first_select_lex();
  /* first table of first SELECT_LEX */
  TABLE_LIST *first_table= (TABLE_LIST*) select_lex->table_list.first;

  const bool used_engine= lex->create_info.used_fields & HA_CREATE_USED_ENGINE;
  DBUG_ASSERT((m_storage_engine_name.str != NULL) == used_engine);
  if (used_engine)
  {
    if (resolve_storage_engine_with_error(thd, &lex->create_info.db_type,
                                          lex->create_info.tmp_table()))
      return true; // Engine not found, substitution is not allowed
    if (!lex->create_info.db_type) // Not found, but substitution is allowed
      lex->create_info.used_fields&= ~HA_CREATE_USED_ENGINE;
  }

  /*
    Code in mysql_alter_table() may modify its HA_CREATE_INFO argument,
    so we have to use a copy of this structure to make execution
    prepared statement- safe. A shallow copy is enough as no memory
    referenced from this structure will be modified.
    @todo move these into constructor...
  */
  Table_specification_st create_info(lex->create_info);
  Alter_info alter_info(lex->alter_info, thd->mem_root);
  create_info.alter_info= &alter_info;
  privilege_t priv(NO_ACL);
  privilege_t priv_needed(ALTER_ACL);
  bool result;

  DBUG_ENTER("Sql_cmd_alter_table::execute");

  if (unlikely(thd->is_fatal_error))
  {
    /* out of memory creating a copy of alter_info */
    DBUG_RETURN(TRUE);
  }
  /*
    We also require DROP priv for ALTER TABLE ... DROP PARTITION, as well
    as for RENAME TO, as being done by SQLCOM_RENAME_TABLE
  */
  if ((alter_info.partition_flags & ALTER_PARTITION_DROP) ||
      (alter_info.partition_flags & ALTER_PARTITION_CONVERT_IN) ||
      (alter_info.partition_flags & ALTER_PARTITION_CONVERT_OUT) ||
      (alter_info.flags & ALTER_RENAME))
    priv_needed|= DROP_ACL;

  /* Must be set in the parser */
  DBUG_ASSERT(select_lex->db.str);
  DBUG_ASSERT(!(alter_info.partition_flags & ALTER_PARTITION_EXCHANGE));
  DBUG_ASSERT(!(alter_info.partition_flags & ALTER_PARTITION_ADMIN));
  if (check_access(thd, priv_needed, first_table->db.str,
                   &first_table->grant.privilege,
                   &first_table->grant.m_internal,
                   0, 0) ||
      check_access(thd, INSERT_ACL | CREATE_ACL, select_lex->db.str,
                   &priv,
                   NULL, /* Don't use first_tab->grant with sel_lex->db */
                   0, 0))
    DBUG_RETURN(TRUE);                  /* purecov: inspected */

  if ((alter_info.partition_flags & ALTER_PARTITION_CONVERT_IN))
  {
    TABLE_LIST *tl= first_table->next_local;
    tl->grant.privilege= first_table->grant.privilege;
    tl->grant.m_internal= first_table->grant.m_internal;
  }


  /* If it is a merge table, check privileges for merge children. */
  if (create_info.merge_list)
  {
    /*
      The user must have (SELECT_ACL | UPDATE_ACL | DELETE_ACL) on the
      underlying base tables, even if there are temporary tables with the same
      names.

      From user's point of view, it might look as if the user must have these
      privileges on temporary tables to create a merge table over them. This is
      one of two cases when a set of privileges is required for operations on
      temporary tables (see also CREATE TABLE).

      The reason for this behavior stems from the following facts:

        - For merge tables, the underlying table privileges are checked only
          at CREATE TABLE / ALTER TABLE time.

          In other words, once a merge table is created, the privileges of
          the underlying tables can be revoked, but the user will still have
          access to the merge table (provided that the user has privileges on
          the merge table itself). 

        - Temporary tables shadow base tables.

          I.e. there might be temporary and base tables with the same name, and
          the temporary table takes the precedence in all operations.

        - For temporary MERGE tables we do not track if their child tables are
          base or temporary. As result we can't guarantee that privilege check
          which was done in presence of temporary child will stay relevant
          later as this temporary table might be removed.

      If SELECT_ACL | UPDATE_ACL | DELETE_ACL privileges were not checked for
      the underlying *base* tables, it would create a security breach as in
      Bug#12771903.
    */

    if (check_table_access(thd, SELECT_ACL | UPDATE_ACL | DELETE_ACL,
                           create_info.merge_list, FALSE, UINT_MAX, FALSE))
      DBUG_RETURN(TRUE);
  }

  if (check_grant(thd, priv_needed, first_table, FALSE, UINT_MAX, FALSE))
    DBUG_RETURN(TRUE);                  /* purecov: inspected */

#ifdef WITH_WSREP
  if (WSREP(thd) && wsrep_thd_is_local(thd) &&
      (!thd->is_current_stmt_binlog_format_row() ||
       !thd->find_temporary_table(first_table)))
  {
    /*
      It makes sense to set auto_increment_* to defaults in TOI operations.
      Must be done before wsrep_TOI_begin() since Query_log_event encapsulating
      TOI statement and auto inc variables for wsrep replication is constructed
      there. Variables are reset back in THD::reset_for_next_command() before
      processing of next command.
    */
    if (wsrep_auto_increment_control)
    {
      thd->variables.auto_increment_offset = 1;
      thd->variables.auto_increment_increment = 1;
    }

    wsrep::key_array keys;
    if (!wsrep_append_fk_parent_table(thd, first_table, &keys))
    {
      WSREP_TO_ISOLATION_BEGIN_ALTER(lex->name.str ? select_lex->db.str
                                     : first_table->db.str,
                                     lex->name.str ? lex->name.str
                                     : first_table->table_name.str,
                                     first_table, &alter_info, &keys,
                                     used_engine ? &create_info : nullptr)
      {
        WSREP_WARN("ALTER TABLE isolation failure");
        DBUG_RETURN(TRUE);
      }
    }
    DEBUG_SYNC(thd, "wsrep_alter_table_after_toi");
  }
#endif

  if (lex->name.str && !test_all_bits(priv, INSERT_ACL | CREATE_ACL))
  {
    // Rename of table
    TABLE_LIST tmp_table;
    tmp_table.init_one_table(&select_lex->db, &lex->name, 0, TL_IGNORE);
    tmp_table.grant.privilege= priv;
    if (check_grant(thd, INSERT_ACL | CREATE_ACL, &tmp_table, FALSE,
                    UINT_MAX, FALSE))
      DBUG_RETURN(TRUE);                  /* purecov: inspected */
  }

  /* Don't yet allow changing of symlinks with ALTER TABLE */
  if (create_info.data_file_name)
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        WARN_OPTION_IGNORED, ER_THD(thd, WARN_OPTION_IGNORED),
                        "DATA DIRECTORY");
  if (create_info.index_file_name)
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        WARN_OPTION_IGNORED, ER_THD(thd, WARN_OPTION_IGNORED),
                        "INDEX DIRECTORY");
  create_info.data_file_name= create_info.index_file_name= NULL;

#ifdef WITH_PARTITION_STORAGE_ENGINE
  thd->work_part_info= 0;
#endif

  Recreate_info recreate_info;
  result= mysql_alter_table(thd, &select_lex->db, &lex->name,
                            &create_info,
                            first_table,
                            &recreate_info,
                            &alter_info,
                            select_lex->order_list.elements,
                            select_lex->order_list.first,
                            lex->ignore, lex->if_exists());

  DBUG_RETURN(result);
}

bool Sql_cmd_discard_import_tablespace::execute(THD *thd)
{
  /* first SELECT_LEX (have special meaning for many of non-SELECTcommands) */
  SELECT_LEX *select_lex= thd->lex->first_select_lex();
  /* first table of first SELECT_LEX */
  TABLE_LIST *table_list= (TABLE_LIST*) select_lex->table_list.first;

  if (check_access(thd, ALTER_ACL, table_list->db.str,
                   &table_list->grant.privilege,
                   &table_list->grant.m_internal,
                   0, 0))
    return true;

  if (check_grant(thd, ALTER_ACL, table_list, false, UINT_MAX, false))
    return true;

  /*
    Check if we attempt to alter mysql.slow_log or
    mysql.general_log table and return an error if
    it is the case.
    TODO: this design is obsolete and will be removed.
  */
  if (check_if_log_table(table_list, TRUE, "ALTER"))
    return true;

  return
    mysql_discard_or_import_tablespace(thd, table_list,
                                       m_tablespace_op == DISCARD_TABLESPACE);
}
