--source include/have_sequence.inc

--echo #
--echo # Test create_internal_tmp_table_from_heap() with blob/text columns.
--echo #
--echo # Field_blob_key is used for blob/text columns that appear as key columns
--echo # (GROUP BY, SELECT DISTINCT, UNION DISTINCT) in internal tmp tables
--echo # created by class Create_tmp_table.
--echo #

# SHOW STATUS output differs across protocol variants; disable them globally.
--disable_cursor_protocol
--disable_ps2_protocol

--echo #
--echo # Setup: 50 distinct text/blob values of ~1500 bytes each.
--echo # Row count: 100 (50 original + 50 duplicate rows for aggregation).
--echo # Blob data in the tmp table: ~75 KB (50 * 1500 bytes),
--echo # which is between 64 K and 1 M.
--echo #

CREATE TABLE t1 (
  pk INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  a  TEXT NOT NULL,
  b  BLOB,
  v  INT  NOT NULL
);

# Each value is a 1500-char string uniquely identifying the row.
# LPAD(seq,4,'0') gives '0001'..'0050' so all 50 values are distinct.
INSERT INTO t1 (a, b, v)
  SELECT REPEAT(LPAD(seq, 4, '0'), 375),
         REPEAT(LPAD(seq, 4, '0'), 375),
         seq
  FROM seq_1_to_50;

# Duplicate rows: same a/b values, v offset by 100 (v=101..150).
INSERT INTO t1 (a, b, v)
  SELECT a, b, v + 100 FROM t1 ORDER BY pk;

# Sanity: 100 rows, 50 distinct a values, total v = 1+...+50 + 101+...+150 = 7550
SELECT COUNT(*), COUNT(DISTINCT a), SUM(v) FROM t1;

set @@max_sort_length=65536;
set @@sort_buffer_size=1024*1024*128;

--echo #
--echo # ================================================================
--echo # Run 1: tmp_memory_table_size=1M — tmp table stays in HEAP
--echo # ================================================================
--echo #

--echo # --- GROUP BY on TEXT column (end_update path) ---
--echo # Tmp table: 50 groups x ~1500-byte TEXT key = ~75 KB. Fits in 1 M HEAP.
FLUSH STATUS;
SELECT COUNT(*), SUM(gsum) FROM (
  SELECT a, SUM(v) AS gsum FROM t1 GROUP BY a
) dt;
SHOW STATUS LIKE 'Created_tmp%';

--echo # --- SELECT DISTINCT on TEXT column (end_write path) ---
FLUSH STATUS;
SELECT COUNT(*) AS cnt_distinct FROM (SELECT DISTINCT a FROM t1) dt;
SHOW STATUS LIKE 'Created_tmp%';

--echo # --- SELECT DISTINCT on BLOB column ---
FLUSH STATUS;
SELECT COUNT(*) AS cnt_distinct FROM (SELECT DISTINCT b FROM t1) dt;
SHOW STATUS LIKE 'Created_tmp%';

--echo # --- SELECT DISTINCT on TEXT + INT columns ---
FLUSH STATUS;
SELECT COUNT(*) AS cnt_distinct FROM (SELECT DISTINCT a, v FROM t1) dt;
SHOW STATUS LIKE 'Created_tmp%';

--echo # --- GROUP BY TEXT column + HAVING ---
FLUSH STATUS;
SELECT COUNT(*) AS groups_with_2rows FROM (
  SELECT a FROM t1 GROUP BY a HAVING COUNT(*) = 2
) dt;
SHOW STATUS LIKE 'Created_tmp%';

--echo # --- GROUP BY on TEXT + BLOB columns (two blob key columns) ---
FLUSH STATUS;
SELECT COUNT(*) AS groups FROM (
  SELECT a, b, COUNT(*) AS cnt FROM t1 GROUP BY a, b
) dt;
SHOW STATUS LIKE 'Created_tmp%';

--echo # --- GROUP BY TEXT WITH ROLLUP ---
--echo # 50 groups + 1 NULL summary row = 51
FLUSH STATUS;
SELECT COUNT(*) AS rows_incl_rollup FROM (
  SELECT a, SUM(v) AS sv FROM t1 GROUP BY a WITH ROLLUP
) dt;
SHOW STATUS LIKE 'Created_tmp%';

--echo # --- UNION DISTINCT on TEXT column (end_write path) ---
--echo # Both halves share the same 50 distinct 'a' values -> 50 rows after UNION.
FLUSH STATUS;
SELECT COUNT(*) AS union_rows FROM (
  SELECT a FROM t1 WHERE v <= 50
  UNION
  SELECT a FROM t1 WHERE v > 100
) dt;
SHOW STATUS LIKE 'Created_tmp%';

--echo # --- UNION DISTINCT: same table both sides (all 50 distinct values) ---
FLUSH STATUS;
SELECT COUNT(*) AS distinct_rows FROM (
  SELECT a FROM t1 UNION SELECT a FROM t1
) dt;
SHOW STATUS LIKE 'Created_tmp%';

--echo # --- GROUP_CONCAT(DISTINCT text_col) ---
--echo # GROUP_CONCAT uses blob_storage for blob values; the HEAP key part is
--echo # tiny (12 bytes/row). Expect Created_tmp_disk_tables=0 in both runs.
FLUSH STATUS;
SELECT LENGTH(GROUP_CONCAT(DISTINCT LEFT(a, 4) ORDER BY a)) AS gc_len FROM t1;
SHOW STATUS LIKE 'Created_tmp%';

--echo # --- GROUP_CONCAT(text_col ORDER BY ...) — blob_storage path, no DISTINCT ---
--echo # No HEAP key table is needed; blob data goes to blob_storage (MEM_ROOT).
FLUSH STATUS;
SELECT LENGTH(GROUP_CONCAT(LEFT(a, 4) ORDER BY v)) AS gc_len FROM t1;
SHOW STATUS LIKE 'Created_tmp%';

--echo # --- ORDER BY on TEXT column (DISTINCT subquery + ORDER BY) ---
FLUSH STATUS;
SELECT COUNT(*) AS cnt FROM (
  SELECT DISTINCT a FROM t1 ORDER BY a
) dt;
SHOW STATUS LIKE 'Created_tmp%';

--echo # --- GROUP BY with aggregate ORDER BY (two-pass: group then sort) ---
FLUSH STATUS;
SELECT COUNT(*), SUM(gv) FROM (
  SELECT a, SUM(v) AS gv FROM t1 GROUP BY a ORDER BY SUM(v)
) dt;
SHOW STATUS LIKE 'Created_tmp%';

DROP TABLE t1;

--enable_ps2_protocol
--enable_cursor_protocol
