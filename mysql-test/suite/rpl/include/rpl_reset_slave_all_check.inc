# This file ensures that a slave's id filtering variables (i.e. DO_DOMAIN_IDS,
# IGNORE_DOMAIN_IDS, and IGNORE_SERVER_IDS) are cleared after issuing
# `RESET SLAVE ALL`.
#
# param $_do_domain_ids      Integer list of values to use for DO_DOMAIN_IDS
# param $_ignore_domain_ids  Integer list of values to use for IGNORE_DOMAIN_IDS
# param $_ignore_server_ids  Integer list of values to use for IGNORE_SERVER_IDS
#

--echo # Id filtering variable values should be empty initially
let $do_domain_ids_before= query_get_value(SHOW SLAVE STATUS, Replicate_Do_Domain_Ids, 1);
let $ignore_domain_ids_before= query_get_value(SHOW SLAVE STATUS, Replicate_Ignore_Domain_Ids, 1);
let $ignore_server_ids_before= query_get_value(SHOW SLAVE STATUS, Replicate_Ignore_Server_Ids, 1);

if (`SELECT "$do_domain_ids_before" != "" OR
            "$ignore_domain_ids_before" != "" OR
            "$ignore_server_ids_before" != ""`)
{
    die("CHANGE MASTER TO id filter variables are not empty initially");
}


--echo # Set id filtering variables
eval CHANGE MASTER TO DO_DOMAIN_IDS=$_do_domain_ids, IGNORE_DOMAIN_IDS=$_ignore_domain_ids, IGNORE_SERVER_IDS=$_ignore_server_ids, MASTER_USE_GTID=SLAVE_POS;
let $do_domain_ids_set= query_get_value(SHOW SLAVE STATUS, Replicate_Do_Domain_Ids, 1);
let $ignore_domain_ids_set= query_get_value(SHOW SLAVE STATUS, Replicate_Ignore_Domain_Ids, 1);
let $ignore_server_ids_set= query_get_value(SHOW SLAVE STATUS, Replicate_Ignore_Server_Ids, 1);
--echo # do domain id list: $do_domain_ids_set
--echo # ignore domain id list: $ignore_domain_ids_set
--echo # ignore server id list: $ignore_server_ids_set


--echo # RESET SLAVE ALL should clear values for all id filtering variables
RESET SLAVE ALL;
--replace_result $MASTER_MYPORT MASTER_MYPORT
eval change master to master_port=$MASTER_MYPORT, master_host='127.0.0.1', master_user='root';
--source include/start_slave.inc
--source include/stop_slave.inc

let $do_domain_ids_cleared= query_get_value(SHOW SLAVE STATUS, Replicate_Do_Domain_Ids, 1);
let $ignore_domain_ids_cleared= query_get_value(SHOW SLAVE STATUS, Replicate_Ignore_Domain_Ids, 1);
let $ignore_server_ids_cleared= query_get_value(SHOW SLAVE STATUS, Replicate_Ignore_Server_Ids, 1);
if (`SELECT "$do_domain_ids_cleared" != "" OR
            "$ignore_domain_ids_cleared" != "" OR
            "$ignore_server_ids_cleared" != ""`)
{
    die("RESET SLAVE ALL did not clear id filtering variables");
}
