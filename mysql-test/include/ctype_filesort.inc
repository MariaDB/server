#
# Set desired charset_connection and collation_collation
# before including this file.
#

# The next query creates a LONGTEXT column
# using the current character_set_connection
# and collation_connection.

create table t1 select repeat('a',4000) a;
delete from t1;

insert into t1 values ('a'), ('a '), ('a\t');
select collation(a),hex(a) from t1 order by a;
drop table t1;


--echo #
--echo # MDEV-5453 Assertion `src' fails in my_strnxfrm_unicode on GROUP BY MID(..) WITH ROLLUP
--echo #
SELECT @@collation_connection;
CREATE TABLE t1 (i INT) ENGINE=MyISAM;
INSERT INTO t1 VALUES (1),(2);
SELECT * FROM t1 GROUP BY MID(CURRENT_USER,0) WITH ROLLUP;
SELECT * FROM t1 GROUP BY MID('test',0) WITH ROLLUP; 
DROP TABLE t1;

--echo #
--echo # MDEV-6170 Incorrect ordering with utf8_bin and utf8mb4_bin collations
--echo #
SELECT @@collation_connection;
CREATE TABLE t1 ENGINE=MEMORY AS SELECT REPEAT('a',5) AS a LIMIT 0;
INSERT INTO t1 (a) VALUES ("a");
INSERT INTO t1 (a) VALUES ("b");
INSERT INTO t1 (a) VALUES ("c");
INSERT INTO t1 (a) VALUES ("d");
INSERT INTO t1 (a) VALUES ("e");
INSERT INTO t1 (a) VALUES ("f");
INSERT INTO t1 (a) VALUES ("g");
INSERT INTO t1 (a) VALUES ("h");
INSERT INTO t1 (a) VALUES ("i");
INSERT INTO t1 (a) VALUES ("j");
INSERT INTO t1 (a) VALUES ("k");
INSERT INTO t1 (a) VALUES ("l");
INSERT INTO t1 (a) VALUES ("m");
SELECT * FROM t1 ORDER BY LOWER(a);
SELECT * FROM t1 ORDER BY LOWER(a) DESC;
DROP TABLE t1;
