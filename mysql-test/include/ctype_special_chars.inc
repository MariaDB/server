#
# Various tests with blank and control characters
#

# Tests covering a change in mysql-5.5.37:
#
# commit 63e1d22f8f46966c13d88a4f2e9acd7fa3e9c9b6
# Date:   Fri Mar 26 18:14:39 2004 +0400
#
#    UTF8 now process space as PAD character correctly.

--disable_service_connection

SELECT COLLATION('a');

SELECT 'a'   = 'a ' AS c;
SELECT 'a\0' = 'a'  AS c;
SELECT 'a\0' = 'a ' AS c;
SELECT 'a\t' = 'a'  AS c;
SELECT 'a\t' = 'a ' AS c;

SELECT 'a'   < 'a ' AS c;
SELECT 'a\0' < 'a'  AS c;
SELECT 'a\0' < 'a ' AS c;
SELECT 'a\t' < 'a'  AS c;
SELECT 'a\t' < 'a ' AS c;

SELECT 'a'   > 'a ' AS c;
SELECT 'a\0' > 'a'  AS c;
SELECT 'a\0' > 'a ' AS c;
SELECT 'a\t' > 'a'  AS c;
SELECT 'a\t' > 'a ' AS c;

--enable_service_connection

CREATE TABLE t1 AS SELECT REPEAT('a', 10) AS a LIMIT 0;

#
# Add a column `a_readable` returning a readable form of 'a'
# Some special characters get replaces to dots:
# - 0x00 - to make "diff" handle the output as text rather than binary
# - 0x09 - to the the output have a more predictable visible width
#
# It's important for `a_readable` to have exactly
# the same character set with `a`, to avoid data loss on conversion.
# Let's also create it with the same collation with `a`, just in case.

EXECUTE IMMEDIATE
  REPLACE(REPLACE(
  "ALTER TABLE t1 ADD a_readable "
  "TEXT CHARACTER SET latin1 COLLATE DEFAULT "
  "GENERATED ALWAYS AS "
  "(RPAD(QUOTE(REGEXP_REPLACE(t1.a, '(\\\\x{00}|\\\\x{09})', '.')), 10))",
  'DEFAULT', collation('a')),
  'latin1', charset('a'));

SHOW CREATE TABLE t1;


INSERT INTO t1 (a) VALUES ('a'),('a\0'),('a\t'),('a ');
SELECT hex(a),STRCMP(a,'a'), STRCMP(a,'a ') FROM t1;
SELECT HEX(a), a_readable FROM t1 ORDER BY a, BINARY a;
SELECT
  HEX(t1.a) AS t1a,
  CASE STRCMP(t1.a, t2.a) WHEN -1 THEN '<' WHEN 1 THEN '>' ELSE '=' END AS cmp,
  HEX(t2.a) AS t2a,
  t1.a_readable,
  t2.a_readable
FROM t1 t1, t1 t2
ORDER BY BINARY t1.a, BINARY t2.a;
DROP TABLE t1;
