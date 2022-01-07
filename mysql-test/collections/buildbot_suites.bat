perl mysql-test-run.pl --verbose-restart --force --suite-timeout=120 --max-test-fail=10 --retry=3  --parallel=auto --suite=^
vcol,gcol,perfschema,^
main,^
innodb,^
plugins,^
mariabackup,^
roles,^
auth_gssapi,^
rocksdb
