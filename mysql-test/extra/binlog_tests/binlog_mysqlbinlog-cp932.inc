#
# This include file is used by more than one test suite
# (currently binlog and binlog_encryption).
# Please check all dependent tests after modifying it
#
# Usage:
# --let $use_remote_mysqlbinlog= 1 # optional
# --source extra/binlog_tests/binlog_mysqlbinlog-cp932.inc
#
# By default, the script calls mysqlbinlog to read binary logs directly. 
# If it is undesirable, this behavior can be overridden by setting 
# $use_remote_binlog as shown above.
# The value will be unset after every execution of the script,
# so if it is needed, it should be set explicitly before each call.


# disabled in embedded until tools running is fixed with embedded
--source include/not_embedded.inc

-- source include/have_binlog_format_mixed_or_statement.inc
-- source include/have_cp932.inc
-- source include/have_log_bin.inc

RESET MASTER;

# Bug#16217 (mysql client did not know how not switch its internal charset)
create table t3 (f text character set utf8);
create table t4 (f text character set cp932); 
--exec $MYSQL --default-character-set=utf8 test -e "insert into t3 values(_utf8'ã‚½')"
--exec $MYSQL --default-character-set=cp932 test -e "insert into t4 values(_cp932'ƒ\');"
flush logs;
rename table t3 to t03, t4 to t04;
let $MYSQLD_DATADIR= `select @@datadir`;

--let $mysqlbinlog_args= $MYSQLD_DATADIR/master-bin.000001
if ($use_remote_mysqlbinlog)
{
    --let $mysqlbinlog_args= --read-from-remote-server --protocol=tcp --host=127.0.0.1 --port=$MASTER_MYPORT -uroot master-bin.000001
    --let $use_remote_mysqlbinlog= 0
}

--exec $MYSQL_BINLOG --short-form $mysqlbinlog_args | $MYSQL --default-character-set=utf8
# original and recovered data must be equal
select HEX(f) from t03;
select HEX(f) from t3;
select HEX(f) from t04;
select HEX(f) from t4;

drop table t3, t4, t03, t04;
--echo End of 5.0 tests
