select @@collation_connection;

#
# MDEV-5180 Data type for WEIGHT_STRING is too short in some cases
#
CREATE TABLE t1 AS SELECT 'a' AS a;
SHOW CREATE TABLE t1;
CREATE TABLE t2 AS SELECT WEIGHT_STRING(a) AS ws FROM t1;
SHOW CREATE TABLE t2;
SELECT HEX(WEIGHT_STRING(a)) FROM t1;
SELECT HEX(ws) FROM t2;
DROP TABLE t2;
DROP TABLE t1;

CREATE TABLE t1 AS SELECT REPEAT('a',5) AS a;
SHOW CREATE TABLE t1;
CREATE TABLE t2 AS SELECT WEIGHT_STRING(a) AS ws FROM t1;
SHOW CREATE TABLE t2;
SELECT HEX(WEIGHT_STRING(a)) FROM t1;
SELECT HEX(ws) FROM t2;
DROP TABLE t2;
CREATE TABLE t2 AS SELECT WEIGHT_STRING(a AS CHAR(3)) AS ws FROM t1;
SHOW CREATE TABLE t2;
SELECT HEX(WEIGHT_STRING(a AS CHAR(3))) FROM t1;
SELECT HEX(ws) FROM t2;
DROP TABLE t2;
CREATE TABLE t2 AS SELECT WEIGHT_STRING(a AS CHAR(10)) AS ws FROM t1;
SHOW CREATE TABLE t2;
SELECT HEX(WEIGHT_STRING(a AS CHAR(10))) FROM t1;
SELECT HEX(ws) FROM t2;
DROP TABLE t2;
DROP TABLE t1;
#
# End of MDEV-5180
#

select hex(weight_string('a'));
select hex(weight_string('A'));
select hex(weight_string('abc'));
select hex(weight_string('abc' as char(2)));
select hex(weight_string('abc' as char(3)));
select hex(weight_string('abc' as char(5)));

# Read 2 characters from the source string (the last character is not used).
select hex(weight_string('abc', 1, 2, 0xC0));
select hex(weight_string('abc', 2, 2, 0xC0));
select hex(weight_string('abc', 3, 2, 0xC0));
select hex(weight_string('abc', 4, 2, 0xC0));
select hex(weight_string('abc', 5, 2, 0xC0));
select hex(weight_string('abc',25, 2, 0xC0));

# Read 3 characters from the source string (the entire string is used).
select hex(weight_string('abc', 1, 3, 0xC0));
select hex(weight_string('abc', 2, 3, 0xC0));
select hex(weight_string('abc', 3, 3, 0xC0));
select hex(weight_string('abc', 4, 3, 0xC0));
select hex(weight_string('abc', 5, 3, 0xC0));
select hex(weight_string('abc',25, 3, 0xC0));

# Read 4 characters from the source string (extra space is added)
select hex(weight_string('abc', 1, 4, 0xC0));
select hex(weight_string('abc', 2, 4, 0xC0));
select hex(weight_string('abc', 3, 4, 0xC0));
select hex(weight_string('abc', 4, 4, 0xC0));
select hex(weight_string('abc', 5, 4, 0xC0));
select hex(weight_string('abc',25, 4, 0xC0));
