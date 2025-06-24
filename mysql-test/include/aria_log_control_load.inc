#
# This file loads aria_log_control file into a user variable @aria_log_control.
# Set $ARIA_DATADIR before including this file
#

--disable_query_log
--copy_file $ARIA_DATADIR/aria_log_control $MYSQLTEST_VARDIR/aria_log_control_tmp
--chmod 0777 $MYSQLTEST_VARDIR/aria_log_control_tmp
--eval SET @aria_log_control=(SELECT LOAD_FILE('$MYSQLTEST_VARDIR/aria_log_control_tmp'))
--remove_file $MYSQLTEST_VARDIR/aria_log_control_tmp
--enable_query_log
