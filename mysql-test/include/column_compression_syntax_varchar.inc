--echo #
--echo # The following statements run without warnings.
--echo # The `compressed opt_binary` grammar sequence is covered.
--echo #

--eval CREATE TABLE t1 (a $type COMPRESSED)
SHOW CREATE TABLE t1;
DROP TABLE t1;
--eval CREATE TABLE t1 (a $type COMPRESSED BINARY)
SHOW CREATE TABLE t1;
DROP TABLE t1;
--eval CREATE TABLE t1 (a $type COMPRESSED BINARY ASCII)
SHOW CREATE TABLE t1;
DROP TABLE t1;
--eval CREATE TABLE t1 (a $type COMPRESSED BYTE)
SHOW CREATE TABLE t1;
DROP TABLE t1;
--eval CREATE TABLE t1 (a $type COMPRESSED ASCII)
SHOW CREATE TABLE t1;
DROP TABLE t1;
--eval CREATE TABLE t1 (a $type COMPRESSED UNICODE)
SHOW CREATE TABLE t1;
DROP TABLE t1;
--eval CREATE TABLE t1 (a $type COMPRESSED CHARACTER SET utf8)
SHOW CREATE TABLE t1;
DROP TABLE t1;


--echo #
--echo # The following statements run without warnings.
--echo # They have extra column attributes (or GENERATED) after COMPRESSED.
--echo #

--eval CREATE TABLE t1 (a $type COMPRESSED BYTE DEFAULT '')
SHOW CREATE TABLE t1;
DROP TABLE t1;
--eval CREATE TABLE t1 (a $type COMPRESSED BINARY DEFAULT '')
SHOW CREATE TABLE t1;
DROP TABLE t1;
--eval CREATE TABLE t1 (a $type COMPRESSED ASCII DEFAULT '')
SHOW CREATE TABLE t1;
DROP TABLE t1;
--eval CREATE TABLE t1 (a $type COMPRESSED CHARACTER SET utf8 DEFAULT '')
SHOW CREATE TABLE t1;
DROP TABLE t1;

--eval CREATE TABLE t1 (a $type COMPRESSED CHARACTER SET utf8 GENERATED ALWAYS AS (REPEAT('a',100)))
SHOW CREATE TABLE t1;
DROP TABLE t1;


--echo #
--echo # The following statements return deprecated syntax warnings
--echo #

--enable_prepare_warnings

--eval CREATE TABLE t1 (a $type BINARY COMPRESSED)
SHOW CREATE TABLE t1;
DROP TABLE t1;
--eval CREATE TABLE t1 (a $type ASCII COMPRESSED)
SHOW CREATE TABLE t1;
DROP TABLE t1;
--eval CREATE TABLE t1 (a $type BYTE COMPRESSED)
SHOW CREATE TABLE t1;
DROP TABLE t1;

--disable_prepare_warnings

--echo #
--echo # The following statements fail by the grammar,
--echo # because COMPRESSED immediately follows  'field_type'.
--echo #

--error ER_PARSE_ERROR
--eval CREATE TABLE t1 (a $type COMPRESSED BYTE COMPRESSED)
--error ER_PARSE_ERROR
--eval CREATE TABLE t1 (a $type COMPRESSED BINARY COMPRESSED)
--error ER_PARSE_ERROR
--eval CREATE TABLE t1 (a $type COMPRESSED ASCII COMPRESSED)
--error ER_PARSE_ERROR
--eval CREATE TABLE t1 (a $type COMPRESSED CHARACTER SET utf8 COMPRESSED)


--echo #
--echo # The following statements are not prohibited by the *.yy grammar,
--echo # because the sequence `field_type attribute COMPRESSED` is allowed
--echo # (notice there is at least one attribute after `field_type`).
--echo # The first COMPRESSED is parsed inside `field_type`.
--echo # The second COMPRESSED passes through the parser but then is caught
--echo # inside Column_definition::set_compressed_deprecated_with_type_check()
--echo # and a syntax error is raised.
--echo #

--error ER_PARSE_ERROR
--eval CREATE TABLE t1 (a $type COMPRESSED BYTE DEFAULT '' COMPRESSED)
--error ER_PARSE_ERROR
--eval CREATE TABLE t1 (a $type COMPRESSED BINARY DEFAULT '' COMPRESSED)
--error ER_PARSE_ERROR
--eval CREATE TABLE t1 (a $type COMPRESSED ASCII DEFAULT '' COMPRESSED)
--error ER_PARSE_ERROR
--eval CREATE TABLE t1 (a $type COMPRESSED CHARACTER SET utf8 DEFAULT '' COMPRESSED)
