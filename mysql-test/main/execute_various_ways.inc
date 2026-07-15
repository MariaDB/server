# Run $qry six different ways, echo each to the result log (as before),
# then capture each method's result set and assert they are all
# row-for-row identical.  See include/evw_capture.inc for how/why the
# capture is done through the mysql client rather than backticks or a
# table sink.
#
# In:  $ver  - label for this group
#      $qry  - the query under test (no interior ';')

# ---- visible executions: unchanged, still populate the .result file ----
echo test $ver.0;
eval $qry;
echo test $ver.1;
eval prepare s from '$qry';
execute s;
execute s;
deallocate prepare s;
echo test $ver.2;
eval select * from ($qry) dt;
echo test $ver.3;
eval create view v as $qry;
select * from v;
select * from v;
drop view v;
echo test $ver.4;
eval with cte as ($qry) select * from cte;
echo test $ver.5;
eval create procedure p() $qry;
call p();
call p();
drop procedure p;

# ---- cross-check: every method must return the same rows as method 0 ----
--let EVW_MARKER= __EVW_ROWS__
--let EVW_QRY= $qry
--let EVW_DB= `select database()`

--let $m= 0
while ($m <= 5)
{
  --let EVW_METHOD= $m
  --let EVW_OUT= $MYSQLTEST_VARDIR/tmp/evw_$m.tsv
  --source include/evw_capture.inc
  --inc $m
}

--diff_files $MYSQLTEST_VARDIR/tmp/evw_0.tsv $MYSQLTEST_VARDIR/tmp/evw_1.tsv
--diff_files $MYSQLTEST_VARDIR/tmp/evw_0.tsv $MYSQLTEST_VARDIR/tmp/evw_2.tsv
--diff_files $MYSQLTEST_VARDIR/tmp/evw_0.tsv $MYSQLTEST_VARDIR/tmp/evw_3.tsv
--diff_files $MYSQLTEST_VARDIR/tmp/evw_0.tsv $MYSQLTEST_VARDIR/tmp/evw_4.tsv
--diff_files $MYSQLTEST_VARDIR/tmp/evw_0.tsv $MYSQLTEST_VARDIR/tmp/evw_5.tsv

echo "END GROUP";
