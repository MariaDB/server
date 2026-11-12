if "%MTR_PARALLEL%"=="" set MTR_PARALLEL=%NUMBER_OF_PROCESSORS%
perl mysql-test-run.pl --force --suite-timeout=120 --max-test-fail=10 --retry=2 --skip-test=rpl_read_old_relay_log_info --suite=^
vcol,gcol,perfschema,^
main,^
innodb,^
versioning,^
plugins,^
mariabackup,^
roles,^
auth_gssapi,^
rocksdb,^
rpl,^
sysschema
