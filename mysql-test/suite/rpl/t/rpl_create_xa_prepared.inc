# param $xid    to name xa and take part in the connection name
# param $query  to execute as the xa body
# param $db_ign the default database

--connect (master_$xid, 127.0.0.1,root,,$db,$MASTER_MYPORT,)
--eval xa start '$xid'
--eval $query
--eval xa end '$xid'
--eval xa prepare '$xid';
