namespace Show {

static ST_FIELD_INFO index_stats_fields[]=
{
  Column("TABLE_SCHEMA", Varchar(NAME_LEN),   NOT_NULL, "Table_schema"),
  Column("TABLE_NAME",   Varchar(NAME_LEN),   NOT_NULL, "Table_name"),
  Column("INDEX_NAME",   Varchar(NAME_LEN),   NOT_NULL, "Index_name"),
  Column("ROWS_READ",    SLonglong(),         NOT_NULL, "Rows_read"),
  CEnd()
};

} // namespace Show

static int index_stats_fill(THD *thd, TABLE_LIST *tables, COND *cond)
{
  TABLE *table= tables->table;

  mysql_mutex_lock(&LOCK_global_index_stats);
  for (uint i= 0; i < global_index_stats.records; i++)
  {
    INDEX_STATS *index_stats =
      (INDEX_STATS*) my_hash_element(&global_index_stats, i);
    TABLE_LIST tmp_table;
    const char *index_name;
    size_t index_name_length;

    bzero((char*) &tmp_table,sizeof(tmp_table));
    tmp_table.db.str=    index_stats->index;
    tmp_table.db.length= strlen(index_stats->index);
    tmp_table.table_name.str= index_stats->index + tmp_table.db.length + 1;
    tmp_table.table_name.length= strlen(tmp_table.table_name.str);
    tmp_table.grant.privilege= NO_ACL;
    if (check_access(thd, SELECT_ACL, tmp_table.db.str,
                      &tmp_table.grant.privilege, NULL, 0, 1) ||
        check_grant(thd, SELECT_ACL, &tmp_table, 1, 1, 1))
      continue;

    index_name=         tmp_table.table_name.str + tmp_table.table_name.length + 1;
    index_name_length=  (index_stats->index_name_length - tmp_table.db.length -
                         tmp_table.table_name.length - 3);

    table->field[0]->store(tmp_table.db.str, tmp_table.db.length, system_charset_info);
    table->field[1]->store(tmp_table.table_name.str, tmp_table.table_name.length,
                           system_charset_info);
    table->field[2]->store(index_name, (uint) index_name_length, system_charset_info);
    table->field[3]->store((longlong)index_stats->rows_read, TRUE);

    if (schema_table_store_record(thd, table))
    {
      mysql_mutex_unlock(&LOCK_global_index_stats);
      return 1;
    }
  }
  mysql_mutex_unlock(&LOCK_global_index_stats);
  return 0;
}

static int index_stats_reset()
{
  mysql_mutex_lock(&LOCK_global_index_stats);
  free_global_index_stats();
  init_global_index_stats();
  mysql_mutex_unlock(&LOCK_global_index_stats);
  return 0;
}

static int index_stats_init(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;
  schema->fields_info= Show::index_stats_fields;
  schema->fill_table= index_stats_fill;
  schema->reset_table= index_stats_reset;
  return 0;
}

