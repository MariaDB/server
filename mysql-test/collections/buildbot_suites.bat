perl mysql-test-run.pl --verbose-restart --force --testcase-timeout=45 --suite-timeout=600 --max-test-fail=500 --retry=3  --parallel=4 --suite=^
main,^
innodb,^
versioning,^
plugins,^
mariabackup,^
rocksdb
