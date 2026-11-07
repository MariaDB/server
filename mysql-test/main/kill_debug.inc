#
# Run $query in a new connection con1, kill it at debug sync point
# $sync_point and show the errors it got. The query must fail with
# ER_QUERY_INTERRUPTED. Source it from connection default, it ends there.
#
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
