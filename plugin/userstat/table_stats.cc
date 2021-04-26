static ST_FIELD_INFO table_stats_fields[]=
{
  {"TABLE_SCHEMA", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "Table_schema",SKIP_OPEN_TABLE},
  {"TABLE_NAME", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "Table_name",SKIP_OPEN_TABLE},
  {"ROWS_READ", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Rows_read",SKIP_OPEN_TABLE},
  {"ROWS_CHANGED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Rows_changed",SKIP_OPEN_TABLE},
  {"ROWS_CHANGED_X_INDEXES", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Rows_changed_x_#indexes",SKIP_OPEN_TABLE},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}
};

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
    tmp_table.db=         table_stats->table;
    tmp_table.table_name= end_of_schema+1;
    tmp_table.grant.privilege= 0;
    if (check_access(thd, SELECT_ACL, tmp_table.db,
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
  schema->fields_info= table_stats_fields;
  schema->fill_table= table_stats_fill;
  schema->reset_table= table_stats_reset;
  return 0;
}

