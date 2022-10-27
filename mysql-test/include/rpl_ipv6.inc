connect (master,$IPv6,root,,test,$MASTER_MYPORT);
connect (slave,$IPv6,root,,test,$SLAVE_MYPORT);
connection master;
reset master;
source include/show_master_status.inc;
save_master_pos;
connection slave;
reset slave;
eval change master to master_host='$IPv6';
let $master_host= query_get_value(SHOW SLAVE STATUS, Master_Host, 1);
--echo Master-Host: $master_host
disconnect slave;
disconnect master;
connection default;

