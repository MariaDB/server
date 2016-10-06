#
# Date and time column types
# (DATE, DATETIME, TIMESTAMP, TIME, YEAR)
#

--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings

eval CREATE TABLE t1 (
  d DATE $extra_col_opts,
  dt DATETIME $extra_col_opts,
  ts TIMESTAMP $extra_col_opts,
  t TIME $extra_col_opts,
  y YEAR $extra_col_opts,
  y4 YEAR(4) $extra_col_opts,
  y2 YEAR(2) $extra_col_opts,
  pk DATETIME PRIMARY KEY
) ENGINE=rocksdb;

SHOW COLUMNS IN t1;

SET @tm = '2012-04-09 05:27:00';

# Valid values 
# '1970-01-01 00:00:01'
INSERT INTO t1 (d,dt,ts,t,y,y4,y2,pk) VALUES
('1000-01-01', '1000-01-01 00:00:00', FROM_UNIXTIME(1), '-838:59:59', '1901', '1901', '00','2012-12-12 12:12:12'),
('9999-12-31', '9999-12-31 23:59:59', FROM_UNIXTIME(2147483647), '838:59:59', '2155', '2155', '99','2012-12-12 12:12:13'),
('0000-00-00', '0000-00-00 00:00:00', '0000-00-00 00:00:00', '00:00:00', '0', '0', '0','2012-12-12 12:12:14'),
(DATE(@tm),@tm,TIMESTAMP(@tm),TIME(@tm),YEAR(@tm),YEAR(@tm),YEAR(@tm),'2012-12-12 12:12:15'); 

--sorted_result
SELECT d,dt,ts,t,y,y4,y2 FROM t1;

# Invalid values

INSERT INTO t1 (d,dt,ts,t,y,y4,y2,pk) VALUES
('999-13-32', '999-11-31 00:00:00', '0', '-839:00:00', '1900', '1900', '-1','2012-12-12 12:12:16');

SELECT d,dt,ts,t,y,y4,y2 FROM t1;

DROP TABLE t1;


