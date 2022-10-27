--echo # Start of func_str_ascii_checksum.inc

--echo #
--echo # MDEV-10850 Wrong result for WHERE .. (f2=TO_BASE64('test') OR f2=TO_BASE64('TEST'))
--echo #

--eval CREATE TABLE t1 (f1 VARCHAR(4), f2 VARCHAR(255), UNIQUE KEY k1 (f1,f2))
--eval INSERT INTO t1 VALUES ('test',$func('test')), ('TEST', $func('TEST'))
--eval SELECT * FROM t1 IGNORE INDEX(k1) WHERE f1='test' AND (f2= $func("test") OR f2= $func("TEST"))
--eval SELECT * FROM t1                  WHERE f1='test' AND (f2= $func("test") OR f2= $func("TEST"))
--eval SELECT * FROM t1                  WHERE f1='test' AND (f2= $func("TEST") OR f2= $func("test"))
DROP TABLE t1;


--echo #
--echo # MDEV-10425 Assertion `collation.derivation == DERIVATION_IMPLICIT' failed in Item_func_conv_charset::fix_length_and_dec()
--echo #

--eval PREPARE stmt FROM "SELECT $func(CONVERT('foo' USING latin1))"
EXECUTE stmt;
DEALLOCATE PREPARE stmt;


--echo # End of func_str_ascii_checksum.inc
