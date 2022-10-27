connection master;
--disable_warnings
DROP TABLE IF EXISTS connect.t1;
DROP DATABASE IF EXISTS connect;

connection slave;
DROP TABLE IF EXISTS connect.t1;
DROP DATABASE IF EXISTS connect;
--enable_warnings
