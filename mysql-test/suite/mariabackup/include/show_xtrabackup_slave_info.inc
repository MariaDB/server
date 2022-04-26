--disable_query_log
--file_exists $targetdir/xtrabackup_slave_info
CREATE TEMPORARY TABLE tmp_slave_info(lineno SERIAL, line TEXT);
--replace_result $targetdir TARGETDIR
--eval LOAD DATA LOCAL INFILE '$targetdir/xtrabackup_slave_info' INTO TABLE tmp_slave_info (line);
SELECT
  lineno,
  regexp_replace(
  regexp_replace(line, '(?<=MASTER_LOG_POS=)[0-9]+', '<NUM>'),
                       '[0-9]+-[0-9]+-[0-9]+', '<NUM-NUM-NUM>')
  AS line
FROM tmp_slave_info ORDER BY lineno;
DROP TEMPORARY TABLE tmp_slave_info;
--enable_query_log
