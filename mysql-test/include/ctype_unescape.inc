--echo # Start of ctype_unescape.inc

SET sql_mode = '';

#
# Testing how string literals with backslash and quote-quote are unescaped.
# The tests assume that single quote (') is used as a delimiter.
#

#
# Make sure that the parser really works using the character set we need.
# We use binary strings to compose strings, to be able to test get malformed
# sequences, which are possible as a result of mysql_real_escape_string().
# The important thing for this test is to make the parser unescape using
# the client character set, rather than binary. Currently it works exactly
# that way by default, so the query below should return @@character_set_client 
#
SET @query=_binary'SELECT CHARSET(\'test\'),@@character_set_client,@@character_set_connection';
PREPARE stmt FROM @query;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

let $CHARSET=`SELECT @@character_set_connection`;

CREATE TABLE allbytes (a VARBINARY(10));

#
# Create various byte sequences to test. Testing the full banch of
# possible combinations takes about 2 minutes. So this test provides
# variants to run with:
# - the full set of possible combinations
# - a reduced test of combinations for selected bytes only
# 

# Create selected byte combinations
if ($ctype_unescape_combinations == 'selected')
{
--echo # Using selected bytes combinations
--source include/bytes.inc
#
# Populate "selected_bytes" with bytes that have a special meaning.
# We'll use "selected_bytes" to generate byte seqeunces,
# instead of the full possible byte combinations, to reduce test time.
#
CREATE TABLE selected_bytes (a VARBINARY(10));

# Bytes that have a special meaning in all character sets:
# 0x00 - mysql_real_escape_string() quotes this to '\0'
# 0x0D - mysql_real_escape_string() quotes this to '\r'
# 0x0A - mysql_real_escape_string() quotes this to '\n'
# 0x1A - mysql_real_escape_string() quotes this to '\Z'
# 0x08 - mysql_real_escape_string() does not quote this,
#        but '\b' is unescaped to 0x08.
# 0x09 - mysql_real_escape_string() does not quote this,
#        but '\t' is unescaped to 0x09.
# 0x30 - '0', as in '\0'
# 0x5A - 'Z', as in '\Z'
# 0x62 - 'b', as in '\b'
# 0x6E - 'n', as in '\n'
# 0x72 - 't', as in '\r'
# 0x74 - 't', as in '\t'

INSERT INTO selected_bytes (a) VALUES ('\0'),('\b'),('\t'),('\r'),('\n'),('\Z');
INSERT INTO selected_bytes (a) VALUES ('0'),('b'),('t'),('r'),('n'),('Z');

# 0x22 - double quote
# 0x25 - percent sign, '\%' is preserved as is for LIKE.
# 0x27 - single quote
# 0x5C - backslash
# 0x5F - underscore, '\_' is preserved as is for LIKE.
INSERT INTO selected_bytes (a) VALUES ('\\'),('_'),('%'),(0x22),(0x27);

# Some bytes do not have any special meaning, for example basic Latin letters.
# Let's add, one should be enough for a good enough coverage.
INSERT INTO selected_bytes (a) VALUES ('a');

#
# This maps summarizes bytes that have a special
# meaning in various character sets:
#
#          MBHEAD           MBTAIL            NONASCII-8BIT   BAD
#          ------           ------           --------------   ----------
#  big5:   [A1..F9]         [40..7E,A1..FE]   N/A             [80..A0,FA..FF]
#  cp932:  [81..9F,E0..FC]  [40..7E,80..FC]   [A1..DF]        [FD..FF]
#  gbk:    [81..FE]         [40..7E,80..FE]   N/A             [FF]
#  sjis:   [81..9F,E0..FC]  [40..7E,80..FC]   [A1..DF]        [FD..FF]
#  swe7:   N/A              N/A               [5B..5E,7B..7E] [80..FF]
#
 
INSERT INTO selected_bytes (a) VALUES
(0x3F), # 7bit
(0x40), # 7bit          mbtail
(0x7E), # 7bit          mbtail  nonascii-8bit
(0x7F), # 7bit                  nonascii-8bit
(0x80), #               mbtail                 bad-mb
(0x81), #       mbhead  mbtail
(0x9F), #       mbhead  mbtail                 bad-mb
(0xA0), #       mbhead  mbtail                 bad-mb
(0xA1), #       mbhead  mbtail  nonascii-8bit
(0xE0), #       mbhead  mbtai
(0xEF), #       mbhead  mbtail
(0xF9), #       mbhead  mbtail
(0xFA), #       mbhead  mbtail                 bad-mb
(0xFC), #       mbhead  mbtail                 bad-mb
(0xFD), #       mbhead  mbtail                 bad-mb
(0xFE), #       mbhead  mbtial                 bad-mb
(0xFF); #                                      bad-mb

#
# Now populate the test table
#

# Use all single bytes, this is cheap, there are only 256 values.
INSERT INTO allbytes (a) SELECT a FROM bytes;

# Add selected bytes combinations
INSERT INTO allbytes (a) SELECT CONCAT(t1.a,t2.a) FROM selected_bytes t1,selected_bytes t2;
INSERT INTO allbytes (a) SELECT CONCAT(0x5C,t1.a,t2.a) FROM selected_bytes t1,selected_bytes t2;
INSERT INTO allbytes (a) SELECT CONCAT(0x5C,t1.a,0x5C,t2.a) FROM selected_bytes t1,selected_bytes t2;
DROP TABLE selected_bytes;

# Delete all non-single byte sequences that do not have
# backslashes or quotes at all. There is nothing special with these strings.
DELETE FROM allbytes WHERE
  OCTET_LENGTH(a)>1 AND
  LOCATE(0x5C,a)=0 AND
  a NOT LIKE '%\'%' AND
  a NOT LIKE '%"%';

}

if ($ctype_unescape_combinations=='')
{
--echo # Using full byte combinations
--source include/bytes2.inc
INSERT INTO allbytes (a) SELECT a FROM bytes;
INSERT INTO allbytes (a) SELECT CONCAT(hi,lo) FROM bytes2;
INSERT INTO allbytes (a) SELECT CONCAT(0x5C,hi,lo) FROM bytes2;
INSERT INTO allbytes (a) SELECT CONCAT(0x5C,hi,0x5C,lo) FROM bytes2;
}


DELIMITER //;

#
# A procedure that make an SQL query using 'val' as a string literal.
# The result of the query execution is written into the table 't1'.
# NULL in t1.b means that query failed due to syntax error,
# typically because of mis-interpreted closing quote delimiter.
#
CREATE PROCEDURE p1(val VARBINARY(10))
BEGIN
  DECLARE EXIT HANDLER FOR SQLSTATE '42000' INSERT INTO t1 (a,b) VALUES(val,NULL);
  SET @query=CONCAT(_binary"INSERT INTO t1 (a,b) VALUES (0x",HEX(val),",'",val,"')");
  PREPARE stmt FROM @query;
  EXECUTE stmt;
  DEALLOCATE PREPARE stmt;
END//

#
# A procedure that iterates through all records in "allbytes".
# And runs p1() for every record.
#
CREATE PROCEDURE p2()
BEGIN
  DECLARE val VARBINARY(10);
  DECLARE done INT DEFAULT FALSE;
  DECLARE stmt CURSOR FOR SELECT a FROM allbytes;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done=TRUE;
  OPEN stmt;
read_loop1: LOOP
  FETCH stmt INTO val;
  IF done THEN
    LEAVE read_loop1;
  END IF;
  CALL p1(val);
END LOOP;  
  CLOSE stmt;
END//


# A function that converts the value from binary to $CHARSET
# and check if it has changed. CONVERT() fixes malformed strings.
# So if the string changes in CONVERT(), it means it was not wellformed.
--eval CREATE FUNCTION iswellformed(a VARBINARY(256)) RETURNS INT RETURN a=BINARY CONVERT(a USING $CHARSET);

#
# A function that approximately reproduces how the SQL parser
# would unescape a binary string.
#
CREATE FUNCTION unescape(a VARBINARY(256)) RETURNS VARBINARY(256)
BEGIN
  # We need to do it in a way to avoid producing new escape sequences
  # First, enclose all known escsape sequences to '{{xx}}'
  # - Backslash not followed by a LIKE pattern characters _ and %
  # - Double escapes
  # This uses PCRE Branch Reset Groups: (?|(alt1)|(alt2)|(alt3)).
  # So '\\1' in the last argument always means the match, no matter
  # which alternative it came from.
  SET a=REGEXP_REPLACE(a,'(?|(\\\\[^_%])|(\\x{27}\\x{27}))','{{\\1}}');
  # Now unescape all enclosed standard escape sequences
  SET a=REPLACE(a,'{{\\0}}', '\0');
  SET a=REPLACE(a,'{{\\b}}', '\b');
  SET a=REPLACE(a,'{{\\t}}', '\t');
  SET a=REPLACE(a,'{{\\r}}', '\r');
  SET a=REPLACE(a,'{{\\n}}', '\n');
  SET a=REPLACE(a,'{{\\Z}}', '\Z');
  SET a=REPLACE(a,'{{\\\'}}', '\'');
  # Unescape double quotes
  SET a=REPLACE(a,'{{\'\'}}', '\'');
  # Unescape the rest: all other \x sequences mean just 'x'
  SET a=REGEXP_REPLACE(a, '{{\\\\(.|\\R)}}', '\\1');
  RETURN a;
END//


#
# A function that checks what happened during unescaping.
# 
# @param a - the value before unescaping
# @param b - the value after unescaping
#
# The following return values are possible:
# - SyntErr - b IS NULL, which means syntax error happened in p1().
# - Preserv - the value was not modified during unescaping.
#             This is possible if 0x5C was treated as mbtail.
#             Or only LIKE escape sequences were found: '\_' and '\%'.
# - Trivial - only 0x5C were removed.
# - Regular - the value was unescaped like a binary string.
#             Some standard escape sequences were found.
#             No special multi-byte handling happened.
# - Special - Something else happened. Should not happen.
#
CREATE FUNCTION unescape_type(a VARBINARY(256),b VARBINARY(256)) RETURNS VARBINARY(256)
BEGIN
  RETURN CASE
  WHEN b IS NULL THEN '[SyntErr]'
  WHEN a=b THEN CASE
    WHEN OCTET_LENGTH(a)=1 THEN '[Preserve]'
    WHEN a RLIKE '\\\\[_%]' THEN '[Preserve][LIKE]'
    WHEN a RLIKE '^[[:ascii:]]+$' THEN '[Preserve][ASCII]'
    ELSE '[Preserv][MB]' END
  WHEN REPLACE(a,0x5C,'')=b THEN '[Trivial]'
  WHEN UNESCAPE(a)=b THEN '[Regular]'
  ELSE '[Special]' END;
END// 


#
# Check what happened with wellformedness during unescaping
# @param a  - the value before unescaping
# @param b  - the value after unescaping
#
# Returned values:
# [FIXED] - the value was malformed and become wellformed after unescaping
# [BROKE] - the value was wellformed and become malformed after unescaping
# [ILSEQ] - both values (before unescaping and after unescaping) are malformed
# ''      - both values are wellformed
#
CREATE FUNCTION wellformedness(a VARBINARY(256), b VARBINARY(256))
                               RETURNS VARBINARY(256)
BEGIN
  RETURN CASE
  WHEN b IS NULL THEN ''
  WHEN NOT iswellformed(a) AND iswellformed(b) THEN '[FIXED]'
  WHEN iswellformed(a) AND NOT iswellformed(b) THEN '[BROKE]'
  WHEN NOT iswellformed(a) AND NOT iswellformed(b) THEN '[ILSEQ]'
  ELSE ''
  END;
END//


#
# Check if the value could be generated by mysql_real_escape_string(),
# or can only come from a direct user input.
#
# @param a - the value before unescaping
#
# Returns:
# [USER] - if the value could not be generated by mysql_real_escape_string()
# ''     - if the value was possibly generated by mysql_real_escape_string()
#
#
CREATE FUNCTION mysql_real_escape_string_generated(a VARBINARY(256))
                                                   RETURNS VARBINARY(256)
BEGIN
  DECLARE a1 BINARY(1) DEFAULT SUBSTR(a,1,1);
  DECLARE a2 BINARY(1) DEFAULT SUBSTR(a,2,1);
  DECLARE a3 BINARY(1) DEFAULT SUBSTR(a,3,1);
  DECLARE a4 BINARY(1) DEFAULT SUBSTR(a,4,1);
  DECLARE a2a4 BINARY(2) DEFAULT CONCAT(a2,a4);
  RETURN CASE
  WHEN (a1=0x5C) AND 
       (a3=0x5C) AND
       (a2>0x7F) AND
       (a4 NOT IN ('_','%','0','t','r','n','Z')) AND
       iswellformed(a2a4) THEN '[USER]'
  ELSE ''
  END;
END//

DELIMITER ;//


CREATE TABLE t1 (a VARBINARY(10),b VARBINARY(10));
CALL p2();
# Avoid "Invalid XXX character string" warnings
# We mark malformed strings in the output anyway
--disable_warnings
# All records marked with '[BAD]' mean that the string was unescaped
# in a unexpected way, that means there is a bug in UNESCAPE() above.
SELECT HEX(a),HEX(b),
  CONCAT(unescape_type(a,b),
         wellformedness(a,b),
         mysql_real_escape_string_generated(a),
         IF(UNESCAPE(a)<>b,CONCAT('[BAD',HEX(UNESCAPE(a)),']'),'')) AS comment
FROM t1 ORDER BY LENGTH(a),a;
--enable_warnings
DROP TABLE t1;
DROP PROCEDURE p1;
DROP PROCEDURE p2;
DROP FUNCTION unescape;
DROP FUNCTION unescape_type;
DROP FUNCTION wellformedness;
DROP FUNCTION mysql_real_escape_string_generated;
DROP FUNCTION iswellformed;
DROP TABLE allbytes;
SET sql_mode = DEFAULT;

--echo # End of ctype_backslash.inc
