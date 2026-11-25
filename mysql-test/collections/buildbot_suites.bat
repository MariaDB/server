if "%MTR_PARALLEL%"=="" set MTR_PARALLEL=%NUMBER_OF_PROCESSORS%
perl mysql-test-run.pl --force rpl_read_old_relay_log_info
