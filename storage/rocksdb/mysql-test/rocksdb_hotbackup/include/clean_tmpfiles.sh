#!/bin/bash

COPY_LOG="${MYSQL_TMP_DIR}/myrocks_hotbackup_copy_log"
SIGNAL_FILE=${MYSQL_TMP_DIR}/myrocks_hotbackup_signal
MOVEBACK_LOG="${MYSQL_TMP_DIR}/myrocks_hotbackup_moveback_log"
rm -f $COPY_LOG
rm -f $SIGNAL_FILE
rm -f $MOVEBACK_LOG
