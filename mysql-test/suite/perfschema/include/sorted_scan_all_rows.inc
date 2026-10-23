# Check that a sorted scan of performance_schema.$pfs_table returns as many
# rows as an unsorted one. A sort re-initializes the table context after it
# completes, and rnd_pos() then rebuilds every row from the materialized map
# of that context.
#
# $pfs_table  table to scan, aliased to o
# $pfs_cols   select list
# $pfs_order  ORDER BY list
# $pfs_where  optional WHERE clause, must use the o alias to stay correlated

--disable_query_log
eval SET @unsorted= (SELECT COUNT(*) FROM performance_schema.$pfs_table);
eval CREATE TEMPORARY TABLE t1 AS
     SELECT $pfs_cols FROM performance_schema.$pfs_table o $pfs_where
      ORDER BY $pfs_order;
--enable_query_log
eval SELECT COUNT(*) = @unsorted AS all_rows_returned_by_$pfs_table FROM t1;
DROP TEMPORARY TABLE t1;
--let $pfs_where=
