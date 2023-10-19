if "%MTR_PARALLEL%"=="" set MTR_PARALLEL=%NUMBER_OF_PROCESSORS%
perl mysql-test-run.pl --verbose-restart --force --suite-timeout=120 --max-test-fail=10 --retry=3  --suite=^
funcs_1,perfschema,^
main,^
period,^
versioning,^
plugins,^
type_inet,^
roles,^
type_uuid,^
rocksdb,^
sysschema
