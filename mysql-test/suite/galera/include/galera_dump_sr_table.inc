#
# Dump the contents of the SR table using mysqldump
#

--let $sr_min = `SELECT MIN(seqno) FROM mysql.wsrep_streaming_log`
--let $sr_max = `SELECT MAX(seqno) FROM mysql.wsrep_streaming_log`

--let $seqno = $sr_min
while ($seqno <= $sr_max)
{
  --let $sr_fragment_file = $MYSQLTEST_VARDIR/tmp/sr_fragment.log
  --exec rm -rf $sr_fragment_file
  --disable_query_log
  --eval SELECT frag INTO DUMPFILE '$sr_fragment_file' FROM mysql.wsrep_streaming_log WHERE seqno = $seqno
  --enable_query_log

  --let $sr_binlog_file = $MYSQLTEST_VARDIR/tmp/sr_binlog.log
  --exec rm -rf $sr_binlog_file

  --exec cp std_data/binlog-header.binlog $sr_binlog_file
  --exec cat $sr_fragment_file >> $sr_binlog_file

  --replace_regex /SET TIMESTAMP=[0-9]+/SET TIMESTAMP=<TIMESTAMP>/ /#[0-9]+ +[0-9]+:[0-9]+:[0-9]+/<ISO TIMESTAMP>/ /pseudo_thread_id=[0-9]+/pseudo_thread_id=<PSEUDO_THREAD_ID>/ /thread_id=[0-9]+/thread_id=<QUERY_THREAD_ID>/ /table id [0-9]+/table id <TABLE_ID>/ /mapped to number [0-9]+/mapped to number <TABLE_ID>/ /auto_increment_increment=[0-9]+/auto_increment_increment=<AUTO_INCREMENT_INCREMENT>/ /auto_increment_offset=[0-9]+/auto_increment_offset=<AUTO_INCREMENT_OFFSET>/ /exec_time=[0-9]+/exec_time=<EXEC_TIME>/
  --exec $MYSQL_BINLOG $sr_binlog_file --base64-output=decode-rows --start-position=256 --skip-annotate-row-events | grep -v 'SET @' 2>&1

  --inc $seqno
}

