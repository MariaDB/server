if "%MTR_PARALLEL%"=="" set MTR_PARALLEL=%NUMBER_OF_PROCESSORS%
perl mysql-test-run.pl --force --suite-timeout=120 --max-test-fail=10 --retry=3  --suite=^
vcol,gcol,period,perfschema,parts,^
main,^
innodb,^
binlog_in_engine,^
versioning,^
plugins,^
mariabackup,^
roles,^
auth_gssapi,^
mysql_sha2,^
query_response_time,^
mysql_sha2,^
rocksdb,^
sysschema
