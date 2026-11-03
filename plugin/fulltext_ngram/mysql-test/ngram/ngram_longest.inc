eval CREATE TABLE t (id INT PRIMARY KEY, a TEXT, FULLTEXT(a) WITH PARSER ngram)
  ENGINE=$engine CHARSET=utf8mb4;
INSERT INTO t VALUES (1, 'a一b二c三d四e五f'), (2, 'αa中βb一ωc二γd三δe四εf五ζg六');
# The second n-gram of each row, which starts after a character of another
# length than its own first character, and the last one of row 2, after
# the ring buffer of character lengths has wrapped around
SELECT id FROM t WHERE MATCH(a) AGAINST('一b二c三d四e五f' IN BOOLEAN MODE);
SELECT id FROM t WHERE MATCH(a) AGAINST('a中βb一ωc二γd' IN BOOLEAN MODE);
SELECT id FROM t WHERE MATCH(a) AGAINST('三δe四εf五ζg六' IN BOOLEAN MODE);
# A phrase of all n-grams of row 2
SELECT id FROM t WHERE MATCH(a) AGAINST('"αa中βb一ωc二γd三δe四εf五ζg六"' IN BOOLEAN MODE);
# A word of 9 characters gives no 10-gram
SELECT id FROM t WHERE MATCH(a) AGAINST('a一b二c三d四e' IN BOOLEAN MODE);
DROP TABLE t;
