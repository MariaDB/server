let $point_count=`select field('EOF', $crash_points)`;
let $p= 1;
--echo # Iterating $point_count crash-points
set @save_dbug= @@debug_dbug;
while ($p < $point_count)
{
  let $point= `select concat('crash_', elt($p, $crash_points))`;
  --echo # Crash-point $p: $point
  eval set @@debug_dbug= "+d,$point";
  --source suite/parts/inc/partition_crash.inc
  set @@debug_dbug= @save_dbug;
  inc $p;
}
