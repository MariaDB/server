--enable_prepare_warnings
SELECT * FROM t1 WHERE indexed_col=20230101;
SELECT * FROM t1 WHERE indexed_col=20230101102030;
SELECT * FROM t1 WHERE indexed_col=20230101102030.1;
SELECT * FROM t1 WHERE indexed_col=20230101102030.1e0;
SELECT * FROM t1 WHERE indexed_col='10:20:30';
SELECT * FROM t1 WHERE indexed_col='2001-01-01';
SELECT * FROM t1 WHERE indexed_col='2001-01-01 10:20:30';
SELECT * FROM t1 WHERE indexed_col=DATE'2001-01-01';
SELECT * FROM t1 WHERE indexed_col=TIME'10:20:30';
SELECT * FROM t1 WHERE indexed_col=TIMESTAMP'2001-01-01 10:20:30';
SELECT * FROM t1 WHERE indexed_col=0x00;
SELECT * FROM t1 WHERE indexed_col=_utf8mb3'0' COLLATE utf8mb3_bin;
--disable_prepare_warnings
