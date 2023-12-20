# Some character are not possible to represent in some encodings. Ignore them.
--disable_warnings
--disable_query_log
# http://www.gerolf.org/doc/polyglot/polyglot.htm.
INSERT IGNORE INTO t VALUES
  ('Chinese', '他们为什么不说中文？'),
  ('English', 'Why can''t they just speak English?'),
  ('Japanese', 'なぜ、みんな日本語を話してくれないのか？'),
  ('Korean', '세계의 모든 사람들이 한국어를 이해한다면 얼마나 좋을까?');
--enable_query_log
--enable_warnings

SELECT * FROM t ORDER BY lang;

SELECT * FROM t WHERE MATCH(str) AGAINST('之一' IN BOOLEAN MODE) ORDER BY lang;
SELECT * FROM t WHERE MATCH(str) AGAINST('数据' IN BOOLEAN MODE) ORDER BY lang;
SELECT * FROM t WHERE MATCH(str) AGAINST('zzz' IN BOOLEAN MODE) ORDER BY lang;
SELECT * FROM t WHERE MATCH(str) AGAINST('gli' IN BOOLEAN MODE) ORDER BY lang;
SELECT * FROM t WHERE MATCH(str) AGAINST('ない' IN BOOLEAN MODE) ORDER BY lang;
SELECT * FROM t WHERE MATCH(str) AGAINST('람들' IN BOOLEAN MODE) ORDER BY lang;

DROP TABLE t;
