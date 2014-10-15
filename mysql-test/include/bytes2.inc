#
# Create a table with all 2 byte sequence values
#

--source include/bytes.inc

CREATE TABLE halfs (a BINARY(1));
# "bytes" is a temporary table, hence is not allowed in joins
# Create a non-temporary copy.
INSERT INTO halfs SELECT * FROM bytes;
CREATE TEMPORARY TABLE bytes2 (
  a BINARY(2),
  hi BINARY(1),
  lo BINARY(1),
  KEY(a),
  KEY(lo)
) ENGINE=MyISAM;
INSERT INTO bytes2
SELECT CONCAT(t1.a, t2.a), t1.a, t2.a FROM halfs t1, halfs t2
ORDER BY t1.a, t2.a;
DROP TABLE halfs;
