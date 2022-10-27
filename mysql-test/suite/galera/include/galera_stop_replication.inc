--echo Unloading wsrep provider ...

--let $wsrep_cluster_address_orig = `SELECT @@wsrep_cluster_address`
--let $wsrep_provider_options_orig = `SELECT @@wsrep_provider_options`
--let $wsrep_error_log_orig = `SELECT @@log_error`
if(!$wsrep_log_error_orig)
{
    # MySQL Server on windows is started with --console and thus
    # does not know the location of its .err log, use default location
    let $wsrep_log_error_orig = $MYSQLTEST_VARDIR/log/mysqld.2.err;
}
--let LOG_FILE= $wsrep_log_error_orig

SET GLOBAL wsrep_cluster_address = '';
