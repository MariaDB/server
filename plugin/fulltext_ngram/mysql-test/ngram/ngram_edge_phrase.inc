eval CREATE TABLE t (id INT PRIMARY KEY, a TEXT, FULLTEXT(a) WITH PARSER ngram)
  ENGINE=$engine;
# Row 3 has the n-grams of 'gli', but not next to each other
INSERT INTO t VALUES (1, 'English'), (2, 'lion'), (3, 'gl zz lion'),
  (4, 'gli lion');
SELECT id FROM t WHERE MATCH(a) AGAINST('gli' IN BOOLEAN MODE) ORDER BY id;
SELECT id FROM t WHERE MATCH(a) AGAINST('+lion -gli' IN BOOLEAN MODE) ORDER BY id;
SELECT id FROM t WHERE MATCH(a) AGAINST('+(gli lio) -ngl' IN BOOLEAN MODE)
ORDER BY id;
DROP TABLE t;
