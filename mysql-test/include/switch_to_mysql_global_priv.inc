echo # switching back from mysql.user to mysql.global_priv;
disable_query_log;
drop table mysql.user;
rename table mysql.user_bak to mysql.user;
rename table mysql.global_priv_bak to mysql.global_priv;
flush privileges;
enable_query_log;
