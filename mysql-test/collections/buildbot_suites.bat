if "%MTR_PARALLEL%"=="" set MTR_PARALLEL=%NUMBER_OF_PROCESSORS%
perl mysql-test-run.pl --force --suite-timeout=120 --max-test-fail=10 --retry=3  --suite=^
vcol,gcol,period,perfschema,^
main,^
innodb,^
versioning,^
plugins,^
mariabackup,^
roles,^
auth_gssapi,^
query_response_time,^
rocksdb,^
sysschema
