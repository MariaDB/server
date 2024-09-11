--echo #
--echo #  Tests below show the avg_frequency calculated during EITS collection and
--echo #  we can compare that value by calculating distinct values for the same columns
--echo #

--echo #
--echo # Running ANALYZE for customer table
--echo #

ANALYZE TABLE customer PERSISTENT FOR COLUMNS (c_name, c_phone, c_nationkey) INDEXES ();

SELECT column_name, avg_frequency
FROM mysql.column_stats
WHERE table_name='customer' AND column_name IN ('c_name', 'c_phone', 'c_nationkey');

SELECT COUNT(*), COUNT(DISTINCT c_name), COUNT(DISTINCT c_nationkey), COUNT(DISTINCT c_phone)
FROM customer;

--echo #
--echo # Running ANALYZE for lineitem table
--echo #

ANALYZE TABLE lineitem PERSISTENT FOR COLUMNS (l_shipDATE, l_shipinstruct) INDEXES ();
SELECT column_name, avg_frequency
FROM mysql.column_stats
WHERE table_name='lineitem' AND column_name IN ('l_shipDATE', 'l_shipinstruct');

SELECT COUNT(*), COUNT(DISTINCT l_shipDATE), COUNT(DISTINCT l_shipinstruct) FROM lineitem;

--echo #
--echo # Running ANALYZE for nation table
--echo #

ANALYZE TABLE nation PERSISTENT FOR COLUMNS (n_name, n_regionkey) INDEXES ();
SELECT column_name, avg_frequency
FROM mysql.column_stats
WHERE table_name='nation' AND column_name IN ('n_name', 'n_regionkey');

SELECT COUNT(*), COUNT(DISTINCT n_name), COUNT(DISTINCT n_regionkey) FROM nation;

--echo #
--echo # Running ANALYZE for part table
--echo #

ANALYZE TABLE part PERSISTENT FOR COLUMNS (p_name, p_mfgr) INDEXES ();
SELECT column_name, avg_frequency
FROM mysql.column_stats
WHERE table_name='part' AND column_name IN ('p_name', 'p_mfgr');

SELECT COUNT(*), COUNT(DISTINCT p_name), COUNT(DISTINCT p_mfgr) FROM part;

--echo #
--echo # Running ANALYZE for supplier table
--echo #

ANALYZE TABLE supplier PERSISTENT FOR COLUMNS (s_acctbal, s_name, s_phone) INDEXES ();
SELECT column_name, avg_frequency
FROM mysql.column_stats
WHERE table_name='supplier' AND column_name IN ('s_acctbal', 's_name', 's_phone');

SELECT COUNT(*), COUNT(DISTINCT s_acctbal), COUNT(DISTINCT s_name), COUNT(DISTINCT s_phone)
FROM supplier;
