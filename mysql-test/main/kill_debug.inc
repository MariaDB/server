#
# Run $query and kill it, then show the errors it got. The query must fail
# with ER_QUERY_INTERRUPTED. Source it from connection default, it ends
# there.
#
# $dbug        Kill with this debug_dbug keyword in this connection. The
#              caller saves the old value in @save_dbug.
# $sync_point  Otherwise run $query in a new connection con1 and kill it
#              at this debug sync point.
#
if ($dbug)
{
  eval set debug_dbug= '+d,$dbug';
  # A view would be created in another connection, without $dbug
  --disable_view_protocol
  --error ER_QUERY_INTERRUPTED
  eval $query;
  --enable_view_protocol
  show warnings;
  set debug_dbug= @save_dbug;
}
if (!$dbug)
{
  connect con1, localhost, root;
  let $con1_id= `select connection_id()`;
  eval set debug_sync= '$sync_point signal opening wait_for go';
  send_eval $query;

  connection default;
  set debug_sync= 'now wait_for opening';
  --replace_result $con1_id CON1_ID
  eval kill query $con1_id;

  connection con1;
  --error ER_QUERY_INTERRUPTED
  reap;
  show warnings;

  disconnect con1;
  connection default;
  set debug_sync= 'reset';
}
let $dbug=;
