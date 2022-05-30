--disable_warnings
--disable_query_log
--disable_result_log
--source ../t/test_init.inc
--enable_result_log
--enable_query_log
--enable_warnings
--let $SQL_MODES= real_as_float,pipes_as_concat,ansi_quotes,ignore_space,ignore_bad_table_options,only_full_group_by,no_unsigned_subtraction,no_dir_in_create,postgresql,oracle,mssql,db2,maxdb,no_key_options,no_table_options,no_field_options,mysql323,mysql40,ansi,no_auto_value_on_zero,no_backslash_escapes,strict_trans_tables,strict_all_tables,no_zero_in_date,no_zero_date,allow_invalid_dates,error_for_division_by_zero,traditional,no_auto_create_user,high_not_precedence,no_engine_substitution,pad_char_to_full_length
if (`SELECT IF(STRCMP('$SERVER_NAME', 'MariaDB') = 0, 1, 0)`)
{
  if (`SELECT IF($SERVER_MAJOR_VERSION = 10, 1, 0)`)
  {
    if (`SELECT IF($SERVER_MINOR_VERSION >= 3, 1, 0)`)
    {
      --let $SQL_MODES= $SQL_MODES,empty_string_is_null,simultaneous_assignment
    }
    if (`SELECT IF($SERVER_MINOR_VERSION >= 4, 1, 0)`)
    {
      --let $SQL_MODES= $SQL_MODES,time_round_fractional
    }
  }
}
--connection master_1
set @old_sql_mode= @@sql_mode;
eval set session sql_mode= '$SQL_MODES';
--let $MASTER_1_COMMENT_2_1_BACKUP= $MASTER_1_COMMENT_2_1
--let $CHILD2_1_DROP_TABLES_BACKUP= $CHILD2_1_DROP_TABLES
let $CHILD2_1_DROP_TABLES=
  DROP TABLE IF EXISTS tbl_a;
--let $CHILD2_1_CREATE_TABLES_BACKUP= $CHILD2_1_CREATE_TABLES
let $CHILD2_1_CREATE_TABLES=
  CREATE TABLE tbl_a (
    pkey int NOT NULL,
    PRIMARY KEY (pkey)
  ) $CHILD2_1_ENGINE $CHILD2_1_CHARSET;
--let $CHILD2_1_SELECT_TABLES_BACKUP= $CHILD2_1_SELECT_TABLES
let $CHILD2_1_SELECT_TABLES=
  SELECT pkey FROM tbl_a ORDER BY pkey;
let $CHILD2_1_SELECT_ARGUMENT1=
  SELECT argument FROM mysql.general_log WHERE command_type != 'Execute' AND argument LIKE '%sql_mode%';
