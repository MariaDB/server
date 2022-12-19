--disable_query_log
--connection node_2
SELECT variable_value INTO @old_dump_status FROM information_schema.global_status
    WHERE LOWER(variable_name) = 'innodb_buffer_pool_dump_status';

if (`SELECT variable_value LIKE '%completed at%' FROM information_schema.global_status
     WHERE LOWER(variable_name) = 'innodb_buffer_pool_dump_status'`)
{
--sleep 2
}
# Do the dump
--eval SET GLOBAL innodb_buffer_pool_dump_now='ON';
--enable_query_log

# Wait for the dump to complete
let $wait_condition =
  SELECT variable_value != @old_dump_status
     AND SUBSTR(variable_value, 1, 33) = 'Buffer pool(s) dump completed at '
  FROM information_schema.global_status
  WHERE LOWER(variable_name) = 'innodb_buffer_pool_dump_status';
--source include/wait_condition.inc

perl;
    use strict;
    if ("$ENV{CHECK_REDO_1}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data") {
        die "Redo log file is in the wrong directory on node #1" unless ! -e "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data/ib_logfile0";
    }
    if ("$ENV{CHECK_REDO_2}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data") {
        die "Redo log file is in the wrong directory on node #2" unless ! -e "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data/ib_logfile0";
    }
    die "Redo log file on node #1 does not exist" unless -e "$ENV{CHECK_REDO_1}/ib_logfile0";
    die "Redo log file on node #2 does not exist" unless -e "$ENV{CHECK_REDO_2}/ib_logfile0";
    die "Redo log file on node #1 is empty" unless -s "$ENV{CHECK_REDO_1}/ib_logfile0";
    die "Redo log file on node #2 is empty" unless -s "$ENV{CHECK_REDO_2}/ib_logfile0";
    if ("$ENV{CHECK_REDO_1}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data") {
        if (glob("$ENV{MYSQLTEST_VARDIR}/mysqld.1/data/ib_logfile*")) {
            die "Extra redo log files in the wrong directory on node #1";
        }
    }
    if ("$ENV{CHECK_REDO_2}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data") {
        if (glob("$ENV{MYSQLTEST_VARDIR}/mysqld.2/data/ib_logfile*")) {
            die "Extra redo log files in the wrong directory on node #2";
        }
    }
    if ("$ENV{CHECK_UNDO_1}") {
        if ("$ENV{CHECK_UNDO_1}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data") {
            die "Undo log file #1 is in the wrong directory on node #1" unless ! -e "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data/undo001";
        }
        die "Undo log file #1 on node #1 does not exist" unless -e "$ENV{CHECK_UNDO_1}/undo001";
        die "Undo log file #1 on node #1 is empty" unless -s "$ENV{CHECK_UNDO_1}/undo001";
        if ("$ENV{CHECK_UNDO_1}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data") {
            die "Undo log file #2 is in the wrong directory on node #1" unless ! -e "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data/undo002";
        }
        die "Undo log file #2 on node #1 does not exist" unless -e "$ENV{CHECK_UNDO_1}/undo002";
        die "Undo log file #2 on node #1 is empty" unless -s "$ENV{CHECK_UNDO_1}/undo002";
        if ("$ENV{CHECK_UNDO_1}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data") {
            if (glob("$ENV{MYSQLTEST_VARDIR}/mysqld.1/data/undo*")) {
                die "Extra undo log files in the wrong directory on node #1";
            }
        }
    }
    if ("$ENV{CHECK_UNDO_2}") {
        if ("$ENV{CHECK_UNDO_2}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data") {
            die "Undo log file #1 is in the wrong directory on node #2" unless ! -e "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data/undo001";
        }
        die "Undo log file #1 on node #2 does not exist" unless -e "$ENV{CHECK_UNDO_2}/undo001";
        die "Undo log file #1 on node #2 is empty" unless -s "$ENV{CHECK_UNDO_2}/undo001";
        if ("$ENV{CHECK_UNDO_2}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data") {
            die "Undo log file #2 is in the wrong directory on node #2" unless ! -e "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data/undo002";
        }
        die "Undo log file #2 on node #2 does not exist" unless -e "$ENV{CHECK_UNDO_2}/undo002";
        die "Undo log file #2 on node #2 is empty" unless -s "$ENV{CHECK_UNDO_2}/undo002";
        if ("$ENV{CHECK_UNDO_2}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data") {
            if (glob("$ENV{MYSQLTEST_VARDIR}/mysqld.2/data/undo*")) {
                die "Extra undo log files in the wrong directory on node #2";
            }
        }
    }
    if ("$ENV{CHECK_HOME_1}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data") {
        die "Innodb data file is in the wrong directory on node #1" unless ! -e "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data/ibdata1";
    }
    if ("$ENV{CHECK_HOME_2}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data") {
        die "Innodb data file is in the wrong directory on node #2" unless ! -e "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data/ibdata1";
    }
    die "Innodb data file on node #1 does not exist" unless -e "$ENV{CHECK_HOME_1}/ibdata1";
    die "Innodb data file on node #2 does not exist" unless -e "$ENV{CHECK_HOME_2}/ibdata1";
    die "Innodb data file on node #1 is empty" unless -s "$ENV{CHECK_HOME_1}/ibdata1";
    die "Innodb data file on node #2 is empty" unless -s "$ENV{CHECK_HOME_2}/ibdata1";
    if ("$ENV{CHECK_HOME_1}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data") {
        if (glob("$ENV{MYSQLTEST_VARDIR}/mysqld.1/data/ibdata*")) {
            die "Extra Innodb data files in the wrong directory on node #1";
        }
        if (glob("$ENV{MYSQLTEST_VARDIR}/mysqld.1/data/ibtmp*")) {
            die "Extra Innodb temporary files in the wrong directory on node #1";
        }
    }
    if ("$ENV{CHECK_HOME_2}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data") {
        if (glob("$ENV{MYSQLTEST_VARDIR}/mysqld.2/data/ibdata*")) {
            die "Extra Innodb data files in the wrong directory on node #2";
        }
        if (glob("$ENV{MYSQLTEST_VARDIR}/mysqld.2/data/ibtmp*")) {
            die "Extra Innodb temporary files in the wrong directory on node #2";
        }
    }
    if ("$ENV{CHECK_BP_1}" != "ib_buffer_pool") {
        die "Buffer pool has the wrong name on node #1" unless ! -e "$ENV{CHECK_HOME_1}/ib_buffer_pool";
    }
    if ("$ENV{CHECK_BP_2}" != "ib_buffer_pool") {
        die "Buffer pool has the wrong name on node #2" unless ! -e "$ENV{CHECK_HOME_2}/ib_buffer_pool";
    }
    if ("$ENV{CHECK_BP_1}" != "ib_buffer_pool") {
        die "Extra buffer pool in the wrong directory on node #1" unless ! -e "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data/ib_buffer_pool";
    }
    if ("$ENV{CHECK_BP_2}" != "ib_buffer_pool") {
        die "Extra buffer pool in the wrong directory on node #2" unless ! -e "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data/ib_buffer_pool";
    }
    if ("$ENV{CHECK_HOME_1}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data") {
        die "Buffer pool is in the wrong directory on node #1" unless ! -e "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data/$ENV{CHECK_BP_1}";
    }
    if ("$ENV{CHECK_HOME_2}" != "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data") {
        die "Buffer pool is in the wrong directory on node #2" unless ! -e "$ENV{MYSQLTEST_VARDIR}/mysqld.2/data/$ENV{CHECK_BP_2}";
    }
    die "Buffer pool on node #1 does not exist" unless -e "$ENV{CHECK_HOME_1}/$ENV{CHECK_BP_1}";
    die "Buffer pool on node #2 does not exist" unless -e "$ENV{CHECK_HOME_2}/$ENV{CHECK_BP_2}";
    die "Buffer pool on node #1 is empty" unless -s "$ENV{CHECK_HOME_1}/$ENV{CHECK_BP_1}";
    die "Buffer pool on node #2 is empty" unless -s "$ENV{CHECK_HOME_2}/$ENV{CHECK_BP_2}";
EOF
