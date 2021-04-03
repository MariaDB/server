static ST_FIELD_INFO index_stats_fields[]=
{
  {"TABLE_SCHEMA", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "Table_schema",SKIP_OPEN_TABLE},
  {"TABLE_NAME", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "Table_name",SKIP_OPEN_TABLE},
  {"INDEX_NAME", NAME_LEN, MYSQL_TYPE_STRING, 0, 0, "Index_name",SKIP_OPEN_TABLE},
  {"ROWS_READ", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Rows_read",SKIP_OPEN_TABLE},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0,0}
};

static int index_stats_fill(THD *thd, TABLE_LIST *tables, COND *cond)
{
  TABLE *table= tables->table;

  mysql_mutex_lock(&LOCK_global_index_stats);
  for (uint i= 0; i < global_index_stats.records; i++)
  {
    INDEX_STATS *index_stats =
      (INDEX_STATS*) my_hash_element(&global_index_stats, i);
    TABLE_LIST tmp_table;
    char *index_name;
    size_t schema_name_length, table_name_length, index_name_length;

    bzero((char*) &tmp_table,sizeof(tmp_table));
    tmp_table.db=         index_stats->index;
    tmp_table.table_name= strend(index_stats->index)+1;
    tmp_table.grant.privilege= 0;
    if (check_access(thd, SELECT_ACL, tmp_table.db,
                      &tmp_table.grant.privilege, NULL, 0, 1) ||
        check_grant(thd, SELECT_ACL, &tmp_table, 1, 1, 1))
      continue;

    index_name=         strend(tmp_table.table_name)+1;
    schema_name_length= (tmp_table.table_name - index_stats->index) -1;
    table_name_length=  (index_name - tmp_table.table_name)-1;
    index_name_length=  (index_stats->index_name_length - schema_name_length -
                         table_name_length - 3);

    table->field[0]->store(tmp_table.db, (uint)schema_name_length,
                           system_charset_info);
    table->field[1]->store(tmp_table.table_name, (uint) table_name_length,
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
  schema->fields_info= index_stats_fields;
  schema->fill_table= index_stats_fill;
  schema->reset_table= index_stats_reset;
  return 0;
}

