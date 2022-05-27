--disable_query_log
--file_exists $XTRABACKUP_OUT
CREATE TEMPORARY TABLE tmp_slave_info_out(lineno SERIAL, line TEXT);
--replace_result $targetdir TARGETDIR
--eval LOAD DATA LOCAL INFILE '$XTRABACKUP_OUT' INTO TABLE tmp_slave_info_out (line);
SELECT
  replace(
  regexp_replace(
  regexp_replace(line,
                '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9] [0-9][0-9]:[0-9][0-9]:[0-9][0-9]',
                'YYYY-MM-DD hh:mm:ss'),
                '[0-9]+-[0-9]+-[0-9]+', '<NUM-NUM-NUM>'),
                '\r','' /* Remove CR on Windows */)
                 AS line
FROM tmp_slave_info_out
WHERE line LIKE '%MySQL slave binlog position%'
   OR line LIKE '%Failed to get master binlog coordinates%'
ORDER BY lineno;
DROP TEMPORARY TABLE tmp_slave_info_out;
--enable_query_log
