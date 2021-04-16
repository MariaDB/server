namespace Show {

static ST_FIELD_INFO table_stats_fields[]=
{
  Column("TABLE_SCHEMA",      Varchar(NAME_LEN), NOT_NULL, "Table_schema"),
  Column("TABLE_NAME",        Varchar(NAME_LEN), NOT_NULL, "Table_name"),
  Column("ROWS_READ",             SLonglong(),   NOT_NULL, "Rows_read"),
  Column("ROWS_CHANGED",          SLonglong(),   NOT_NULL, "Rows_changed"),
  Column("ROWS_CHANGED_X_INDEXES",SLonglong(),   NOT_NULL, "Rows_changed_x_#indexes"),
  CEnd()
};

} // namespace Show

static int table_stats_fill(THD *thd, TABLE_LIST *tables, COND *cond)
{
  TABLE *table= tables->table;

  mysql_mutex_lock(&LOCK_global_table_stats);
  for (uint i= 0; i < global_table_stats.records; i++)
  {
    char *end_of_schema;
    TABLE_STATS *table_stats=
      (TABLE_STATS*)my_hash_element(&global_table_stats, i);
    TABLE_LIST tmp_table;
    size_t schema_length, table_name_length;

    end_of_schema= strend(table_stats->table);
    schema_length= (size_t) (end_of_schema - table_stats->table);
    table_name_length= strlen(table_stats->table + schema_length + 1);

    bzero((char*) &tmp_table,sizeof(tmp_table));
    tmp_table.db.str= table_stats->table;
    tmp_table.db.length= schema_length;
    tmp_table.table_name.str= end_of_schema+1;
    tmp_table.table_name.length= table_name_length;
    tmp_table.grant.privilege= NO_ACL;
    if (check_access(thd, SELECT_ACL, tmp_table.db.str,
                     &tmp_table.grant.privilege, NULL, 0, 1) ||
        check_grant(thd, SELECT_ACL, &tmp_table, 1, 1, 1))
      continue;

    table->field[0]->store(table_stats->table, schema_length,
                           system_charset_info);
    table->field[1]->store(table_stats->table + schema_length+1,
                           table_name_length, system_charset_info);
    table->field[2]->store((longlong)table_stats->rows_read, TRUE);
    table->field[3]->store((longlong)table_stats->rows_changed, TRUE);
    table->field[4]->store((longlong)table_stats->rows_changed_x_indexes,
                           TRUE);
    if (schema_table_store_record(thd, table))
    {
      mysql_mutex_unlock(&LOCK_global_table_stats);
      return 1;
    }
  }
  mysql_mutex_unlock(&LOCK_global_table_stats);
  return 0;
}

static int table_stats_reset()
{
  mysql_mutex_lock(&LOCK_global_table_stats);
  free_global_table_stats();
  init_global_table_stats();
  mysql_mutex_unlock(&LOCK_global_table_stats);
  return 0;
}

static int table_stats_init(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;
  schema->fields_info= Show::table_stats_fields;
  schema->fill_table= table_stats_fill;
  schema->reset_table= table_stats_reset;
  return 0;
}

