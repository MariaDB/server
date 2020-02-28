namespace Show {

static ST_FIELD_INFO client_stats_fields[]=
{
  Column("CLIENT",Varchar(LIST_PROCESS_HOST_LEN), NOT_NULL, "Client"),
  Column("TOTAL_CONNECTIONS",        SLonglong(), NOT_NULL, "Total_connections"),
  Column("CONCURRENT_CONNECTIONS",   SLonglong(), NOT_NULL, "Concurrent_connections"),
  Column("CONNECTED_TIME",           SLonglong(), NOT_NULL, "Connected_time"),
  Column("BUSY_TIME", Double(MY_INT64_NUM_DECIMAL_DIGITS), NOT_NULL, "Busy_time"),
  Column("CPU_TIME", Double(MY_INT64_NUM_DECIMAL_DIGITS), NOT_NULL, "Cpu_time"),
  Column("BYTES_RECEIVED",           SLonglong(), NOT_NULL, "Bytes_received"),
  Column("BYTES_SENT",               SLonglong(), NOT_NULL, "Bytes_sent"),
  Column("BINLOG_BYTES_WRITTEN",     SLonglong(), NOT_NULL, "Binlog_bytes_written"),
  Column("ROWS_READ",                SLonglong(), NOT_NULL, "Rows_read"),
  Column("ROWS_SENT",                SLonglong(), NOT_NULL, "Rows_sent"),
  Column("ROWS_DELETED",             SLonglong(), NOT_NULL, "Rows_deleted"),
  Column("ROWS_INSERTED",            SLonglong(), NOT_NULL, "Rows_inserted"),
  Column("ROWS_UPDATED",             SLonglong(), NOT_NULL, "Rows_updated"),
  Column("SELECT_COMMANDS",          SLonglong(), NOT_NULL, "Select_commands"),
  Column("UPDATE_COMMANDS",          SLonglong(), NOT_NULL, "Update_commands"),
  Column("OTHER_COMMANDS",           SLonglong(), NOT_NULL, "Other_commands"),
  Column("COMMIT_TRANSACTIONS",      SLonglong(), NOT_NULL, "Commit_transactions"),
  Column("ROLLBACK_TRANSACTIONS",    SLonglong(), NOT_NULL, "Rollback_transactions"),
  Column("DENIED_CONNECTIONS",       SLonglong(), NOT_NULL, "Denied_connections"),
  Column("LOST_CONNECTIONS",         SLonglong(), NOT_NULL, "Lost_connections"),
  Column("ACCESS_DENIED",            SLonglong(), NOT_NULL, "Access_denied"),
  Column("EMPTY_QUERIES",            SLonglong(), NOT_NULL, "Empty_queries"),
  Column("TOTAL_SSL_CONNECTIONS",    ULonglong(), NOT_NULL, "Total_ssl_connections"),
  Column("MAX_STATEMENT_TIME_EXCEEDED", SLonglong(), NOT_NULL, "Max_statement_time_exceeded"),
  CEnd()
};

} // namespace Show

static int send_user_stats(THD* thd, HASH *all_user_stats, TABLE *table)
{
  mysql_mutex_lock(&LOCK_global_user_client_stats);
  for (uint i= 0; i < all_user_stats->records; i++)
  {
    uint j= 0;
    USER_STATS *user_stats= (USER_STATS*) my_hash_element(all_user_stats, i);

    table->field[j++]->store(user_stats->user, user_stats->user_name_length,
                             system_charset_info);
    table->field[j++]->store((longlong)user_stats->total_connections,TRUE);
    table->field[j++]->store((longlong)user_stats->concurrent_connections, TRUE);
    table->field[j++]->store((longlong)user_stats->connected_time, TRUE);
    table->field[j++]->store((double)user_stats->busy_time);
    table->field[j++]->store((double)user_stats->cpu_time);
    table->field[j++]->store((longlong)user_stats->bytes_received, TRUE);
    table->field[j++]->store((longlong)user_stats->bytes_sent, TRUE);
    table->field[j++]->store((longlong)user_stats->binlog_bytes_written, TRUE);
    table->field[j++]->store((longlong)user_stats->rows_read, TRUE);
    table->field[j++]->store((longlong)user_stats->rows_sent, TRUE);
    table->field[j++]->store((longlong)user_stats->rows_deleted, TRUE);
    table->field[j++]->store((longlong)user_stats->rows_inserted, TRUE);
    table->field[j++]->store((longlong)user_stats->rows_updated, TRUE);
    table->field[j++]->store((longlong)user_stats->select_commands, TRUE);
    table->field[j++]->store((longlong)user_stats->update_commands, TRUE);
    table->field[j++]->store((longlong)user_stats->other_commands, TRUE);
    table->field[j++]->store((longlong)user_stats->commit_trans, TRUE);
    table->field[j++]->store((longlong)user_stats->rollback_trans, TRUE);
    table->field[j++]->store((longlong)user_stats->denied_connections, TRUE);
    table->field[j++]->store((longlong)user_stats->lost_connections, TRUE);
    table->field[j++]->store((longlong)user_stats->access_denied_errors, TRUE);
    table->field[j++]->store((longlong)user_stats->empty_queries, TRUE);
    table->field[j++]->store((longlong)user_stats->total_ssl_connections, TRUE);
    table->field[j++]->store((longlong)user_stats->max_statement_time_exceeded, TRUE);
    if (schema_table_store_record(thd, table))
    {
      mysql_mutex_unlock(&LOCK_global_user_client_stats);
      return 1;
    }
  }
  mysql_mutex_unlock(&LOCK_global_user_client_stats);
  return 0;
}

static int client_stats_fill(THD* thd, TABLE_LIST* tables, COND* cond)
{
  if (check_global_access(thd, PROCESS_ACL, true))
    return 0;

  return send_user_stats(thd, &global_client_stats, tables->table);
}

static int client_stats_reset()
{
  mysql_mutex_lock(&LOCK_global_user_client_stats);
  free_global_client_stats();
  init_global_client_stats();
  mysql_mutex_unlock(&LOCK_global_user_client_stats);
  return 0;
}

static int client_stats_init(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;
  schema->fields_info= Show::client_stats_fields;
  schema->fill_table= client_stats_fill;
  schema->reset_table= client_stats_reset;
  return 0;
}
