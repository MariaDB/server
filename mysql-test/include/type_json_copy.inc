--echo # JSON field copies: $json_copy_engine

--eval CREATE TABLE t1 (id INT PRIMARY KEY, a LONGTEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin, b TINYTEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin) ENGINE=$json_copy_engine
INSERT INTO t1 VALUES (1, '{}', '[]'), (2, 'invalid', 'invalid'), (3, NULL, NULL);

# A failure after copying a valid row must leave the original table as it.
--error ER_TRUNCATED_WRONG_VALUE_FOR_FIELD
ALTER TABLE t1 MODIFY a JSON, ALGORITHM=COPY;
--error ER_TRUNCATED_WRONG_VALUE_FOR_FIELD
ALTER TABLE t1 MODIFY b JSON, ALGORITHM=COPY;
SELECT COLUMN_NAME, DATA_TYPE FROM information_schema.COLUMNS
WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1' ORDER BY ORDINAL_POSITION;
SELECT id, a, b FROM t1 ORDER BY id;

UPDATE t1 SET a = '{"a":1}', b = '123' WHERE id = 2;
ALTER TABLE t1 MODIFY a JSON, MODIFY b JSON, ALGORITHM=COPY;
SELECT id, a, b, JSON_VALID(a), JSON_VALID(b) FROM t1 ORDER BY id;
DROP TABLE t1;


--eval CREATE TABLE t1 (a LONGTEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin, j JSON) ENGINE=$json_copy_engine
INSERT INTO t1 VALUES ('invalid', '{}');
--error ER_TRUNCATED_WRONG_VALUE_FOR_FIELD
UPDATE t1 SET j = a;
SELECT a, j FROM t1;
UPDATE t1 SET a = '[1,2]';
UPDATE t1 SET j = a;
SELECT a, j, JSON_VALID(j) FROM t1;
DROP TABLE t1;

# Exercise field-to-field assignment as well as ALTER's Copy_field path.
--eval CREATE TABLE src (id INT PRIMARY KEY, a LONGTEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin) ENGINE=$json_copy_engine
--eval CREATE TABLE dst (id INT PRIMARY KEY, a JSON) ENGINE=$json_copy_engine
INSERT INTO src VALUES (1, 'invalid');
--error ER_TRUNCATED_WRONG_VALUE_FOR_FIELD
INSERT INTO dst SELECT * FROM src;
SELECT COUNT(*) FROM dst;

UPDATE src SET a = '{"a":1}';
INSERT INTO src VALUES (2, '[]'), (3, NULL);
INSERT INTO dst SELECT * FROM src;
SELECT id, a, JSON_VALID(a) FROM dst ORDER BY id;

DROP TABLE src, dst;
