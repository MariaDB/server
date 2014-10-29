--echo # Start of ctype_E05C.inc

#
# A shared test for character sets big5, cp932, gbk, sjis
# They all can have 0x5C as the second byte in a multi-byte character.
# 0xE05C is one of such characters.
#

# Checking that the character 0xE05C correctly understands 5C as the second byte
# rather than a stand-alone backslash, including the strings that also
# have real backslash escapes and/or separator escapes.

SELECT HEX('à\'),HEX('à\t');
SELECT HEX('\\à\'),HEX('\\à\t'),HEX('\\à\t\t');
SELECT HEX('''à\'),HEX('à\''');
SELECT HEX('\\''à\'),HEX('à\''\\');

SELECT HEX(BINARY('à\')),HEX(BINARY('à\t'));
SELECT HEX(BINARY('\\à\')),HEX(BINARY('\\à\t')),HEX(BINARY('\\à\t\t'));
SELECT HEX(BINARY('''à\')),HEX(BINARY('à\'''));
SELECT HEX(BINARY('\\''à\')),HEX(BINARY('à\''\\'));

SELECT HEX(_BINARY'à\'),HEX(_BINARY'à\t');
SELECT HEX(_BINARY'\\à\'),HEX(_BINARY'\\à\t'),HEX(_BINARY'\\à\t\t');
SELECT HEX(_BINARY'''à\'),HEX(_BINARY'à\''');
SELECT HEX(_BINARY'\\''à\'),HEX(_BINARY'à\''\\');

CREATE TABLE t1 AS SELECT REPEAT(' ',10) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('à\'),('à\t');
INSERT INTO t1 VALUES ('\\à\'),('\\à\t'),('\\à\t\t');
INSERT INTO t1 VALUES ('''à\'),('à\''');
INSERT INTO t1 VALUES ('\\''à\'),('à\''\\');
SELECT a, HEX(a) FROM t1;
DROP TABLE t1;

CREATE TABLE t1 (a BLOB);
INSERT INTO t1 VALUES ('à\'),('à\t');
INSERT INTO t1 VALUES ('\\à\'),('\\à\t'),('\\à\t\t');
INSERT INTO t1 VALUES ('''à\'),('à\''');
INSERT INTO t1 VALUES ('\\''à\'),('à\''\\');
SELECT a, HEX(a) FROM t1;
DROP TABLE t1;

CREATE TABLE t1 AS SELECT REPEAT(' ', 10) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES (BINARY('à\')),(BINARY('à\t'));
INSERT INTO t1 VALUES (BINARY('\\à\')),(BINARY('\\à\t')),(BINARY('\\à\t\t'));
INSERT INTO t1 VALUES (BINARY('''à\')),(BINARY('à\'''));
INSERT INTO t1 VALUES (BINARY('\\''à\')),(BINARY('à\''\\'));
SELECT a, HEX(a) FROM t1;
DROP TABLE t1;

CREATE TABLE t1 (a BLOB);
INSERT INTO t1 VALUES (BINARY('à\')),(BINARY('à\t'));
INSERT INTO t1 VALUES (BINARY('\\à\')),(BINARY('\\à\t')),(BINARY('\\à\t\t'));
INSERT INTO t1 VALUES (BINARY('''à\')),(BINARY('à\'''));
INSERT INTO t1 VALUES (BINARY('\\''à\')),(BINARY('à\''\\'));
SELECT a, HEX(a) FROM t1;
DROP TABLE t1;

CREATE TABLE t1 AS SELECT REPEAT(' ', 10) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES (_BINARY'à\'),(_BINARY'à\t');
INSERT INTO t1 VALUES (_BINARY'\\à\'),(_BINARY'\\à\t'),(_BINARY'\\à\t\t');
INSERT INTO t1 VALUES (_BINARY'''à\'),(_BINARY'à\''');
INSERT INTO t1 VALUES (_BINARY'\\''à\'),(_BINARY'à\''\\');
SELECT a, HEX(a) FROM t1;
DROP TABLE t1;

CREATE TABLE t1 (a BLOB);
INSERT INTO t1 VALUES (_BINARY'à\'),(_BINARY'à\t');
INSERT INTO t1 VALUES (_BINARY'\\à\'),(_BINARY'\\à\t'),(_BINARY'\\à\t\t');
INSERT INTO t1 VALUES (_BINARY'''à\'),(_BINARY'à\''');
INSERT INTO t1 VALUES (_BINARY'\\''à\'),(_BINARY'à\''\\');
SELECT a, HEX(a) FROM t1;
DROP TABLE t1;

# Checking that with character_set_client=binary 0x5C in 0xE05C
# is treated as escape rather than the second byte of a multi-byte character,
# even if character_set_connection is big5/cp932/gbk/sjis.
# Note, the other 0x5C which is before 0xE05C is also treated as escape.
#
SET character_set_client=binary, character_set_results=binary;
SELECT @@character_set_client, @@character_set_connection, @@character_set_results;
SELECT HEX('à\['), HEX('\à\[');
CREATE TABLE t1 AS SELECT REPEAT(' ', 10) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('à\['),('\à\[');
SELECT HEX(a) FROM t1;
DROP TABLE t1;

#
# Checking the other way around:
# 0x5C in 0xE05C is treated as the second byte
# when character_set_client=big5,cp932,gbk,sjis
# and character_set_connection=binary
#
SET character_set_client=@@character_set_connection, character_set_results=@@character_set_connection;
SET character_set_connection=binary;
SELECT @@character_set_client, @@character_set_connection, @@character_set_results;
SELECT HEX('à\['), HEX('\à\[');
CREATE TABLE t1 AS SELECT REPEAT(' ', 10) AS a LIMIT 0;
SHOW CREATE TABLE t1;
INSERT INTO t1 VALUES ('à\['),('\à\[');
SELECT HEX(a) FROM t1;
DROP TABLE t1;


--echo # Start of ctype_E05C.inc

