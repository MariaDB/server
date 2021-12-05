let $point_count=`select field('EOF', $fail_points)`;
let $p= 1;
--echo # Iterating $point_count fail-points
set @save_dbug= @@debug_dbug;
while ($p < $point_count)
{
  let $point= `select concat('fail_', elt($p, $fail_points))`;
  --echo # Fail-point $p: $point
  eval set @@debug_dbug= "+d,$point";
  --source suite/parts/inc/partition_fail.inc
  set @@debug_dbug= @save_dbug;
  inc $p;
}
