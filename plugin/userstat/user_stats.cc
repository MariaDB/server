namespace Show {

static ST_FIELD_INFO user_stats_fields[]=
{
  Column("USER",Varchar(USERNAME_CHAR_LENGTH),NOT_NULL, "User"),
  Column("TOTAL_CONNECTIONS",    SLong(),     NOT_NULL, "Total_connections"),
  Column("CONCURRENT_CONNECTIONS",SLong(),    NOT_NULL, "Concurrent_connections"),
  Column("CONNECTED_TIME",       SLong(),     NOT_NULL, "Connected_time"),
  Column("BUSY_TIME", Double(MY_INT64_NUM_DECIMAL_DIGITS), NOT_NULL, "Busy_time"),
  Column("CPU_TIME", Double(MY_INT64_NUM_DECIMAL_DIGITS), NOT_NULL, "Cpu_time"),
  Column("BYTES_RECEIVED",       SLonglong(), NOT_NULL, "Bytes_received"),
  Column("BYTES_SENT",           SLonglong(), NOT_NULL, "Bytes_sent"),
  Column("BINLOG_BYTES_WRITTEN", SLonglong(), NOT_NULL, "Binlog_bytes_written"),
  Column("ROWS_READ",            SLonglong(), NOT_NULL, "Rows_read"),
  Column("ROWS_SENT",            SLonglong(), NOT_NULL, "Rows_sent"),
  Column("ROWS_DELETED",         SLonglong(), NOT_NULL, "Rows_deleted"),
  Column("ROWS_INSERTED",        SLonglong(), NOT_NULL, "Rows_inserted"),
  Column("ROWS_UPDATED",         SLonglong(), NOT_NULL, "Rows_updated"),
  Column("SELECT_COMMANDS",      SLonglong(), NOT_NULL, "Select_commands"),
  Column("UPDATE_COMMANDS",      SLonglong(), NOT_NULL, "Update_commands"),
  Column("OTHER_COMMANDS",       SLonglong(), NOT_NULL, "Other_commands"),
  Column("COMMIT_TRANSACTIONS",  SLonglong(), NOT_NULL, "Commit_transactions"),
  Column("ROLLBACK_TRANSACTIONS",SLonglong(), NOT_NULL, "Rollback_transactions"),
  Column("DENIED_CONNECTIONS",   SLonglong(), NOT_NULL, "Denied_connections"),
  Column("LOST_CONNECTIONS",     SLonglong(), NOT_NULL, "Lost_connections"),
  Column("ACCESS_DENIED",        SLonglong(), NOT_NULL, "Access_denied"),
  Column("EMPTY_QUERIES",        SLonglong(), NOT_NULL, "Empty_queries"),
  Column("TOTAL_SSL_CONNECTIONS",ULonglong(), NOT_NULL, "Total_ssl_connections"),
  Column("MAX_STATEMENT_TIME_EXCEEDED",SLonglong(),NOT_NULL, "Max_statement_time_exceeded"),
  CEnd()
};

} // namespace Show

static int user_stats_fill(THD* thd, TABLE_LIST* tables, COND* cond)
{
  if (check_global_access(thd, PROCESS_ACL, true))
    return 0;

  return send_user_stats(thd, &global_user_stats,  tables->table);
}

static int user_stats_reset()
{
  mysql_mutex_lock(&LOCK_global_user_client_stats);
  free_global_user_stats();
  init_global_user_stats();
  mysql_mutex_unlock(&LOCK_global_user_client_stats);
  return 0;
}

static int user_stats_init(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *)p;
  schema->fields_info= Show::user_stats_fields;
  schema->fill_table= user_stats_fill;
  schema->reset_table= user_stats_reset;
  return 0;
}

