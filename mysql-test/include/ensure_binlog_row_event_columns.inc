#
# Helper file to ensure that a binary log file properly writes the expected
# fields based on the binlog_row_image value.
#
# ==== Usage ====
#
# --let $expected_columns= (COLUMN_IDS)
# --let $binlog_filename= FILENAME
# --source include/count_binlog_row_event_columns.inc
#
# Parameters:
#   expected_columns (list<uint>, in) : A list of positive integers which
#       correspond to the column numbers that should be output in a binary
#       log's write_rows event
#   binlog_filename (string, in) : Name of the binary log file to analyze
#

if (!$expected_columns)
{
  --die expected_columns parameter is required but was not set
}

if (!$binlog_filename)
{
  --die binlog_filename parameter is required but was not set
}

--let $include_filename= ensure_binlog_row_event_columns.inc [$expected_columns]
--source include/begin_include_file.inc

--let $assert_file=$MYSQLTEST_VARDIR/tmp/binlog_decoded.out
--let mysqld_datadir=`select @@datadir`

--echo # MYSQL_BINLOG mysqld_datadir/binlog_filename -vv > assert_file
--exec $MYSQL_BINLOG $mysqld_datadir/$binlog_filename -vv > $assert_file

--echo # Verifying all expected column ids appear in binlog event output..
--let num_cols_found=0
--let last_expected_col= `SELECT GREATEST $expected_columns`
--let i= 1
while($i <= $last_expected_col)
{
    # By default, assume the column is not expected to be in the binary log.
    # If the column id is set in expected_columns, then override assertion
    # parameters.
    --let assert_count= 0
    --let assert_text=  Column @$i should not be in binary log

    if (`SELECT $i IN $expected_columns`)
    {
        --let assert_count= 1
        --let assert_text= Column @$i should be in binary log

        --inc $num_cols_found
    }

    --let assert_select= @$i
    --source include/assert_grep.inc

    --inc $i
}
--echo # ..success

--echo # Verifying only expected column ids appear in binlog event output..
--let assert_count= $num_cols_found
--let assert_text= The binlog event should only have $num_cols_found columns
--let assert_select= @[\d]+
--source include/assert_grep.inc
--echo # ..success

--let $include_filename= ensure_binlog_row_event_columns.inc [$expected_columns]
--source include/end_include_file.inc
