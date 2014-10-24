static ST_FIELD_INFO user_stats_fields[]=
{
  {"USER", USERNAME_CHAR_LENGTH, MYSQL_TYPE_STRING, 0, 0, "User",  0},
  {"TOTAL_CONNECTIONS", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, "Total_connections", 0},
  {"CONCURRENT_CONNECTIONS", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, "Concurrent_connections", 0},
  {"CONNECTED_TIME", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG, 0, 0, "Connected_time", 0},
  {"BUSY_TIME", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_DOUBLE, 0, 0, "Busy_time", 0},
  {"CPU_TIME", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_DOUBLE, 0, 0, "Cpu_time", 0},
  {"BYTES_RECEIVED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Bytes_received", 0},
  {"BYTES_SENT", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Bytes_sent", 0},
  {"BINLOG_BYTES_WRITTEN", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Binlog_bytes_written", 0},
  {"ROWS_READ", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Rows_read", 0},
  {"ROWS_SENT", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Rows_sent", 0},
  {"ROWS_DELETED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Rows_deleted", 0},
  {"ROWS_INSERTED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Rows_inserted", 0},
  {"ROWS_UPDATED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Rows_updated", 0},
  {"SELECT_COMMANDS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Select_commands", 0},
  {"UPDATE_COMMANDS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Update_commands", 0},
  {"OTHER_COMMANDS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Other_commands", 0},
  {"COMMIT_TRANSACTIONS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Commit_transactions", 0},
  {"ROLLBACK_TRANSACTIONS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Rollback_transactions", 0},
  {"DENIED_CONNECTIONS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Denied_connections", 0},
  {"LOST_CONNECTIONS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Lost_connections", 0},
  {"ACCESS_DENIED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Access_denied", 0},
  {"EMPTY_QUERIES", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Empty_queries", 0},
  {"TOTAL_SSL_CONNECTIONS", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "Total_ssl_connections", 0},
  {"MAX_STATEMENT_TIME_EXCEEDED", MY_INT64_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONGLONG, 0, 0, "Max_statement_time_exceeded",SKIP_OPEN_TABLE},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}
};

static int user_stats_fill(THD* thd, TABLE_LIST* tables, COND* cond)
{
  if (check_global_access(thd, SUPER_ACL | PROCESS_ACL, true))
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
  schema->fields_info= user_stats_fields;
  schema->fill_table= user_stats_fill;
  schema->reset_table= user_stats_reset;
  return 0;
}

