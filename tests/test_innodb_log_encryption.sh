#!/bin/bash
##################################################################
# - restart server with default (no encryption, no key ver and key).
# - verify various inputs will not corrupt.
# - verify transition from without crypto to with crypto and vice versa work.
# - verify transition from various key versions, including dynamically change it
#   after startup, work.
##################################################################
TEST_INSTANCE_NAME="test"
TEST_INSTANCE_DIR="/tmp/instance"
TEST_INSTANCE_PATH=${TEST_INSTANCE_DIR}/${TEST_INSTANCE_NAME}
TEST_INSTANCE_SOCK_FILENAME=mysql.sock
TEST_INSTANCE_SOCK=${TEST_INSTANCE_PATH}/${TEST_INSTANCE_SOCK_FILENAME}
TEST_INSTANCE_ERR_FILE=${TEST_INSTANCE_PATH}/mysql.err
TEST_INSTANCE_DATA_DIR=${TEST_INSTANCE_PATH}/datadir

google/instance restart ${TEST_INSTANCE_NAME}

MYSQLD_EXTRA_ARGS="--debug_use_static_keys" google/instance restart ${TEST_INSTANCE_NAME}

MYSQLD_EXTRA_ARGS="--innodb_encrypt_log=1" google/instance restart ${TEST_INSTANCE_NAME}

MYSQLD_EXTRA_ARGS="--debug_use_static_keys --innodb_encrypt_log=1" google/instance restart ${TEST_INSTANCE_NAME}

MYSQLD_EXTRA_ARGS="--debug_use_static_keys --debug_crypto_key_version=11" google/instance restart ${TEST_INSTANCE_NAME}

MYSQLD_EXTRA_ARGS="--debug_use_static_keys --debug_crypto_key_version=12 --innodb_encrypt_log=1" google/instance restart ${TEST_INSTANCE_NAME}

MYSQLD_EXTRA_ARGS="--debug_use_static_keys --debug_crypto_key_version=123 --innodb_encrypt_log=1" google/instance restart ${TEST_INSTANCE_NAME}

# -- manually create a database sbtest
# mysql> create database sbtest;

sysbench --test=oltp --oltp-table-size=1000 --mysql-user=root --mysql-socket=${TEST_INSTANCE_SOCK} prepare &

sysbench --num-threads=10 --test=oltp --oltp-table-size=1000 --mysql-user=root --mysql-socket=${TEST_INSTANCE_SOCK} run &

# -- change key version through mysql client
# mysql -S ${TEST_INSTANCE_SOCK} k -u root
# mysql> set global variable debug_crypto_key_version=7;
# ps aux | grep mysqld
# -- simulate a fast shutdown
# kill <myslqd's pid>

MYSQLD_EXTRA_ARGS="--debug_use_static_keys" google/instance restart ${TEST_INSTANCE_NAME}

google/instance restart ${TEST_INSTANCE_NAME}

grep -n corrupt ${TEST_INSTANCE_ERR_FILE} | tail -100

##################################################################
# - clean shutdown.
# - remove InnoDB redo log files.
# - start the server with encryption on.
# - verify no corruption.
##################################################################
MYSQLD_EXTRA_ARGS="--innodb_fast_shutdown=0" google/instance restart ${TEST_INSTANCE_NAME}
google/instance stop ${TEST_INSTANCE_NAME}
mv ${TEST_INSTANCE_DATA_DIR}/ib_logfile0 ${TEST_INSTANCE_DATA_DIR}/ib_logfile0.1
mv ${TEST_INSTANCE_DATA_DIR}/ib_logfile1 ${TEST_INSTANCE_DATA_DIR}/ib_logfile1.1
MYSQLD_EXTRA_ARGS="--debug_use_static_keys --debug_crypto_key_version=777 --innodb_encrypt_log=1 --innodb_fast_shutdown=0" google/instance start ${TEST_INSTANCE_NAME}
grep -n corrupt ${TEST_INSTANCE_ERR_FILE} | tail -100
##################################################################
# - clean shutdown.
# - remove InnoDB redo log files.
# - start the server with encryption off.
# - verify no corruption.
##################################################################
google/instance stop ${TEST_INSTANCE_NAME} 
mv ${TEST_INSTANCE_DATA_DIR}/ib_logfile0 ${TEST_INSTANCE_DATA_DIR}/ib_logfile0.2
mv ${TEST_INSTANCE_DATA_DIR}/ib_logfile1 ${TEST_INSTANCE_DATA_DIR}/ib_logfile1.2
google/instance start ${TEST_INSTANCE_NAME}
grep -n corrupt ${TEST_INSTANCE_ERR_FILE} | tail -100
##################################################################
# - verify fresh start of mysqld instance with encryption off.
##################################################################
google/instance stop  ${TEST_INSTANCE_NAME}
mv ${TEST_INSTANCE_DIR} ${TEST_INSTANCE_DIR}.200
google/instance start ${TEST_INSTANCE_NAME}
grep -n corrupt ${TEST_INSTANCE_ERR_FILE} | tail -100
##################################################################
# - verify fresh start of mysqld instance with encryption on.
##################################################################
google/instance stop ${TEST_INSTANCE_NAME}
mv ${TEST_INSTANCE_DIR} ${TEST_INSTANCE_DIR}.300
MYSQLD_EXTRA_ARGS="--debug_use_static_keys --debug_crypto_key_version=888 --innodb_encrypt_log=1" google/instance start ${TEST_INSTANCE_NAME} 
grep -n corrupt ${TEST_INSTANCE_ERR_FILE} | tail -100
##################################################################
# - fast shutdown.
# - remove InnoDB redo log files.
# - start the server with encryption on.
# - verify no corruption.
##################################################################
google/instance stop ${TEST_INSTANCE_NAME}
mv ${TEST_INSTANCE_DATA_DIR}/ib_logfile0 ${TEST_INSTANCE_DATA_DIR}/ib_logfile0.3
mv ${TEST_INSTANCE_DATA_DIR}/ib_logfile1 ${TEST_INSTANCE_DATA_DIR}/ib_logfile1.3
MYSQLD_EXTRA_ARGS="--debug_use_static_keys --debug_crypto_key_version=999 --innodb_encrypt_log=1" google/instance start ${TEST_INSTANCE_NAME}
grep -n corrupt ${TEST_INSTANCE_ERR_FILE} | tail -100
##################################################################
# - fast shutdown while running workload.
# - remove InnoDB redo log files.
# - start the server with encryption on.
# - verify no corruption.
##################################################################
# -- manually create a database sbtest
# mysql> create database sbtest;
sysbench --test=oltp --oltp-table-size=1000 --mysql-user=root --mysql-socket=${TEST_INSTANCE_SOCK} prepare &
sysbench --num-threads=10 --test=oltp --oltp-table-size=1000 --mysql-user=root --mysql-socket=${TEST_INSTANCE_SOCK} run &
google/instance stop ${TEST_INSTANCE_NAME}
mv ${TEST_INSTANCE_DATA_DIR}/ib_logfile0 ${TEST_INSTANCE_DATA_DIR}/ib_logfile0.4
mv ${TEST_INSTANCE_DATA_DIR}/ib_logfile1 ${TEST_INSTANCE_DATA_DIR}/ib_logfile1.4
MYSQLD_EXTRA_ARGS="--debug_use_static_keys --debug_crypto_key_version=333 --innodb_encrypt_log=1" google/instance start ${TEST_INSTANCE_NAME}
grep -n corrupt ${TEST_INSTANCE_ERR_FILE} | tail -100
##################################################################
# - clean up
##################################################################
google/instance stop ${TEST_INSTANCE_NAME}
MYSQLD_EXTRA_ARGS="--debug_use_static_keys" google/instance start ${TEST_INSTANCE_NAME}
google/instance stop ${TEST_INSTANCE_NAME}
