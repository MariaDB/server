#
# Create a table with all byte values
#
CREATE TABLE halfs (a INT);
INSERT INTO halfs VALUES (0x00),(0x01),(0x02),(0x03),(0x04),(0x05),(0x06),(0x07);
INSERT INTO halfs VALUES (0x08),(0x09),(0x0A),(0x0B),(0x0C),(0x0D),(0x0E),(0x0F);
CREATE TEMPORARY TABLE bytes (a BINARY(1), KEY(a)) ENGINE=MyISAM;
INSERT INTO bytes SELECT CHAR((t1.a << 4) | t2.a USING BINARY) FROM halfs t1, halfs t2;
DROP TABLE halfs;
